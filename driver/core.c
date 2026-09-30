// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LibrANPU host driver: loads the firmware image, boots the harts
 * and runs the command and event rings of the ABI v2 interface.
 */

#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>

#include "libranpu.h"

#define SHM_BOOT		0
#define SHM_CMD			SZ_4K
#define SHM_EVT			(SHM_CMD + LIBRANPU_CMD_ENTRIES * LIBRANPU_CMD_SIZE)
#define SHM_SIZE		(SHM_EVT + LIBRANPU_EVT_ENTRIES * LIBRANPU_EVT_SIZE)

static bool uart;
module_param(uart, bool, 0444);
MODULE_PARM_DESC(uart, "let the NPU print boot and fault lines on the SoC console");

/* BOOT_CONFIG takes effect on a 0 to 1 write of BOOT_TRIGGER */
static void libranpu_harts(struct libranpu *npu, u32 mask)
{
	npu_wr(npu, REG_BOOT_TRIGGER, 0);
	npu_wr(npu, REG_BOOT_CONFIG, mask);
	npu_wr(npu, REG_BOOT_TRIGGER, 1);
}

static bool span_ok(u32 start, u32 len, u32 win, u32 win_len)
{
	return start >= win && len <= win_len && start - win <= win_len - len;
}

static int libranpu_check(struct libranpu *npu, const struct firmware *fw,
			  struct resource *code)
{
	const struct libranpu_img_hdr *h = (const void *)fw->data;
	u32 crc, code_off, code_len, data_off, data_len;

	if (fw->size < sizeof(*h) || le32_to_cpu(h->magic) != LIBRANPU_IMG_MAGIC ||
	    h->hdr_version != LIBRANPU_IMG_VERSION ||
	    le16_to_cpu(h->hdr_size) != sizeof(*h))
		return dev_err_probe(npu->dev, -EINVAL, "not an NPU image\n");

	if (le16_to_cpu(h->soc) != npu->soc->id)
		return dev_err_probe(npu->dev, -EINVAL, "image for SoC %x\n",
				     le16_to_cpu(h->soc));
	if (le16_to_cpu(h->abi_major) != LIBRANPU_ABI_MAJOR)
		return dev_err_probe(npu->dev, -EINVAL, "image ABI %u, want %u\n",
				     le16_to_cpu(h->abi_major),
				     LIBRANPU_ABI_MAJOR);
	if (!h->harts || h->harts > npu->soc->harts)
		return dev_err_probe(npu->dev, -EINVAL, "image for %u harts\n",
				     h->harts);

	code_off = le32_to_cpu(h->code.offset);
	code_len = le32_to_cpu(h->code.size);
	data_off = le32_to_cpu(h->data.offset);
	data_len = le32_to_cpu(h->data.size);
	if (!span_ok(code_off, code_len, 0, fw->size) ||
	    !span_ok(data_off, data_len, 0, fw->size))
		return dev_err_probe(npu->dev, -EINVAL, "sections past the file\n");

	crc = ~crc32_le(~0, fw->data, offsetof(struct libranpu_img_hdr, crc32));
	crc = crc32_le(~crc, fw->data + sizeof(*h), fw->size - sizeof(*h));
	if (~crc != le32_to_cpu(h->crc32))
		return dev_err_probe(npu->dev, -EINVAL, "bad image checksum\n");

	/* code and stacks in the reserved region, the entry in the code */
	if (le32_to_cpu(h->code.load) != code->start ||
	    le32_to_cpu(h->dram_size) > resource_size(code) ||
	    code_len > le32_to_cpu(h->dram_size) ||
	    !span_ok(le32_to_cpu(h->entry), 4, code->start, code_len))
		return dev_err_probe(npu->dev, -EINVAL,
				     "image does not fit the binary region\n");

	/* data, bss and the debug block in cluster SRAM */
	if (le32_to_cpu(h->data.load) != NPU_WIN_NPU_ADDR ||
	    data_len > le32_to_cpu(h->cluster_size) ||
	    le32_to_cpu(h->cluster_size) > le32_to_cpu(h->dbg_offset) ||
	    !span_ok(le32_to_cpu(h->dbg_offset), LIBRANPU_DBG_SIZE, 0,
		      npu->soc->cluster_size))
		return dev_err_probe(npu->dev, -EINVAL,
				     "image does not fit cluster SRAM\n");

	memcpy(&npu->hdr, h, sizeof(*h));
	return 0;
}

static int libranpu_load(struct libranpu *npu)
{
	const char *name = npu->soc->fw_name;
	const struct firmware *fw;
	struct resource code;
	void __iomem *dst;
	int err;

	err = of_reserved_mem_region_to_resource_byname(npu->dev->of_node,
							"binary", &code);
	if (err)
		return dev_err_probe(npu->dev, err, "no binary region\n");

	of_property_read_string(npu->dev->of_node, "firmware-name", &name);
	err = request_firmware(&fw, name, npu->dev);
	if (err)
		return dev_err_probe(npu->dev, err, "no firmware %s\n", name);

	err = libranpu_check(npu, fw, &code);
	if (err)
		goto out;

	/* uncached, so the harts fetch what was written */
	dst = ioremap(code.start, le32_to_cpu(npu->hdr.code.size));
	if (!dst) {
		err = -ENOMEM;
		goto out;
	}
	memcpy_toio(dst, fw->data + le32_to_cpu(npu->hdr.code.offset),
		    le32_to_cpu(npu->hdr.code.size));
	iounmap(dst);

	memcpy_toio(npu->base, fw->data + le32_to_cpu(npu->hdr.data.offset),
		    le32_to_cpu(npu->hdr.data.size));
out:
	release_firmware(fw);
	return err;
}

static int libranpu_shm_init(struct libranpu *npu)
{
	struct libranpu_boot *b;
	dma_addr_t dma;

	npu->shm = dmam_alloc_coherent(npu->dev, SHM_SIZE, &dma, GFP_KERNEL);
	if (!npu->shm)
		return -ENOMEM;
	if (dma < NPU_DRAM_WIN_START || dma + SHM_SIZE > NPU_DRAM_WIN_END)
		return dev_err_probe(npu->dev, -ERANGE,
				     "rings at %pad, outside the NPU window\n",
				     &dma);

	npu->shm_dma = dma;
	npu->boot = npu->shm + SHM_BOOT;
	npu->cmd = npu->shm + SHM_CMD;
	npu->evt = npu->shm + SHM_EVT;

	b = npu->boot;
	b->magic = cpu_to_le32(LIBRANPU_BOOT_MAGIC);
	b->abi_major = cpu_to_le16(LIBRANPU_ABI_MAJOR);
	b->abi_minor = cpu_to_le16(LIBRANPU_ABI_MINOR);
	b->flags = cpu_to_le32(uart ? LIBRANPU_BOOT_F_UART : 0);
	b->cmd_ring = cpu_to_le64(dma + SHM_CMD);
	b->evt_ring = cpu_to_le64(dma + SHM_EVT);
	b->cmd_entries = cpu_to_le32(LIBRANPU_CMD_ENTRIES);
	b->evt_entries = cpu_to_le32(LIBRANPU_EVT_ENTRIES);
	return 0;
}

/*
 * The firmware resets the NPU bus first; a host register access then
 * hangs the SoC. So after the trigger only host memory is polled.
 */
static u32 boot_word(__le32 *p)
{
	return le32_to_cpu(READ_ONCE(*p));
}

static int libranpu_boot(struct libranpu *npu)
{
	struct libranpu_boot *b = npu->boot;
	u32 entry = le32_to_cpu(npu->hdr.entry), ready, status;
	ktime_t t0;
	int h, err;

	npu_wr(npu, REG_MIB(MIB_BOOT_BLOCK), npu->shm_dma + SHM_BOOT);
	for (h = 0; h < npu->hdr.harts; h++)
		npu_wr(npu, REG_BOOT_BASE(h), entry);
	t0 = ktime_get();
	libranpu_harts(npu, GENMASK(npu->hdr.harts - 1, 0));

	err = read_poll_timeout(boot_word, ready,
				ready == LIBRANPU_BOOT_READY, 500,
				NPU_BOOT_TIMEOUT_MS * USEC_PER_MSEC, false,
				&b->ready);
	npu->boot_time = ktime_sub(ktime_get(), t0);
	if (err)
		return dev_err_probe(npu->dev, err, "no ready, hart0 pc %08x\n",
				     npu_rr(npu, REG_HART_PC(0)));
	dma_rmb();

	status = le32_to_cpu(b->status);
	if (status)
		return dev_err_probe(npu->dev, (s32)status,
				     "firmware refused the boot block\n");
	if (le32_to_cpu(b->harts_up) != GENMASK(npu->hdr.harts - 1, 0))
		dev_warn(npu->dev, "harts up %x\n", le32_to_cpu(b->harts_up));
	return 0;
}

static int libranpu_get_caps(struct libranpu *npu)
{
	u16 len = sizeof(npu->caps);
	int err;

	err = libranpu_cmd(npu, LIBRANPU_SVC_CTL, LIBRANPU_CTL_GET_CAPS,
			   NULL, 0, &npu->caps, &len);
	if (err)
		return dev_err_probe(npu->dev, err, "GET_CAPS failed\n");
	if (len < sizeof(npu->caps) ||
	    le16_to_cpu(npu->caps.abi_major) != LIBRANPU_ABI_MAJOR)
		return dev_err_probe(npu->dev, -EINVAL, "bad capabilities\n");
	return 0;
}

/* Park every hart through the firmware, then stop the cluster */
static void libranpu_halt(struct libranpu *npu)
{
	struct libranpu_reset_rsp rsp;
	u16 len = sizeof(rsp);
	int err;

	err = libranpu_cmd(npu, LIBRANPU_SVC_CTL, LIBRANPU_CTL_RESET,
			   NULL, 0, &rsp, &len);
	if (err)
		dev_warn(npu->dev, "RESET: %d\n", err);
	libranpu_harts(npu, 0);
}

static int libranpu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct libranpu *npu;
	int irq, err;

	npu = libranpu_devlink_alloc(dev);
	if (!npu)
		return -ENOMEM;
	platform_set_drvdata(pdev, npu);

	npu->dev = dev;
	npu->soc = of_device_get_match_data(dev);
	libranpu_cmd_init(npu);

	npu->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(npu->base)) {
		err = PTR_ERR(npu->base);
		goto err_free;
	}

	err = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (err)
		goto err_free;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		err = irq;
		goto err_free;
	}
	npu->irq = irq;

	/* halt whatever runs, before its memory is rewritten */
	libranpu_harts(npu, 0);

	err = libranpu_shm_init(npu);
	if (!err)
		err = libranpu_load(npu);
	if (!err)
		err = libranpu_boot(npu);
	if (err)
		goto err_halt;

	/* only now: the handler touches NPU registers */
	err = devm_request_threaded_irq(dev, irq, libranpu_mbox_irq,
					libranpu_mbox_thread, IRQF_ONESHOT,
					dev_name(dev), npu);
	if (err)
		goto err_halt;

	err = libranpu_get_caps(npu);
	if (err)
		goto err_reset;

	err = libranpu_wlan_init(npu);
	if (err) {
		/* pool pages keep no reference bias past a failed init */
		libranpu_wlan_deinit(npu);
		goto err_reset;
	}

	libranpu_debugfs_init(npu);
	libranpu_devlink_register(npu);

	dev_info(dev, "fw %u.%u.%u on %u harts (up %x), %u MHz, ready in %lld us\n",
		 le32_to_cpu(npu->caps.fw_version) >> 16,
		 (le32_to_cpu(npu->caps.fw_version) >> 8) & 0xff,
		 le32_to_cpu(npu->caps.fw_version) & 0xff, npu->caps.harts,
		 le32_to_cpu(npu->caps.harts_up), le16_to_cpu(npu->caps.cpu_mhz),
		 ktime_to_us(npu->boot_time));
	return 0;

err_reset:
	libranpu_halt(npu);
	devm_free_irq(dev, irq, npu);
	goto err_free;
err_halt:
	libranpu_harts(npu, 0);
err_free:
	libranpu_devlink_free(npu);
	return err;
}

static void libranpu_remove(struct platform_device *pdev)
{
	struct libranpu *npu = platform_get_drvdata(pdev);

	libranpu_devlink_unregister(npu);
	debugfs_remove_recursive(npu->debugfs);
	libranpu_halt(npu);
	libranpu_wlan_deinit(npu);
	/* the rings go with devres, after the harts stopped */
	devm_free_irq(npu->dev, npu->irq, npu);
	libranpu_devlink_free(npu);
}

static struct platform_driver libranpu_driver;

struct libranpu *libranpu_get(struct device *dev)
{
	struct platform_device *pdev;
	struct device_node *np;
	struct libranpu *npu;

	np = of_parse_phandle(dev->of_node, "airoha,npu", 0);
	if (!np)
		return ERR_PTR(-ENODEV);

	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-ENODEV);

	/* another NPU driver may own the node: its drvdata is not ours */
	if (pdev->dev.driver && pdev->dev.driver != &libranpu_driver.driver) {
		put_device(&pdev->dev);
		return ERR_PTR(-ENODEV);
	}

	npu = platform_get_drvdata(pdev);
	if (!npu || !try_module_get(THIS_MODULE)) {
		put_device(&pdev->dev);
		return ERR_PTR(npu ? -ENODEV : -EPROBE_DEFER);
	}
	return npu;
}
EXPORT_SYMBOL_GPL(libranpu_get);

void libranpu_put(struct libranpu *npu)
{
	module_put(THIS_MODULE);
	put_device(npu->dev);
}
EXPORT_SYMBOL_GPL(libranpu_put);

struct device *libranpu_dev(struct libranpu *npu)
{
	return npu->dev;
}
EXPORT_SYMBOL_GPL(libranpu_dev);

const struct libranpu_caps *libranpu_caps(struct libranpu *npu)
{
	return &npu->caps;
}
EXPORT_SYMBOL_GPL(libranpu_caps);

int libranpu_register_notifier(struct libranpu *npu,
			       struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&npu->notifier, nb);
}
EXPORT_SYMBOL_GPL(libranpu_register_notifier);

void libranpu_unregister_notifier(struct libranpu *npu,
				  struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&npu->notifier, nb);
}
EXPORT_SYMBOL_GPL(libranpu_unregister_notifier);

static const struct libranpu_soc an7552_soc = {
	.id = LIBRANPU_SOC_AN7552,
	.harts = 2,
	.cluster_size = SZ_16K,
	.fw_name = "airoha/an7552-libranpu.bin",
	.name = "AN7552",
};

static const struct libranpu_soc an7581_soc = {
	.id = LIBRANPU_SOC_AN7581,
	.harts = 8,
	.cluster_size = SZ_32K,
	.fw_name = "airoha/an7581-libranpu.bin",
	.name = "AN7581",
};

static const struct libranpu_soc an7583_soc = {
	.id = LIBRANPU_SOC_AN7583,
	.harts = 6,
	.cluster_size = SZ_32K,
	.fw_name = "airoha/an7583-libranpu.bin",
	.name = "AN7583",
};

static const struct of_device_id libranpu_of_match[] = {
	{ .compatible = "airoha,an7552-libranpu", .data = &an7552_soc },
	{ .compatible = "airoha,an7581-libranpu", .data = &an7581_soc },
	{ .compatible = "airoha,an7583-libranpu", .data = &an7583_soc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, libranpu_of_match);

static struct platform_driver libranpu_driver = {
	.probe = libranpu_probe,
	.remove = libranpu_remove,
	.driver = {
		.name = "libranpu",
		.of_match_table = libranpu_of_match,
	},
};
module_platform_driver(libranpu_driver);

MODULE_FIRMWARE("airoha/an7552-libranpu.bin");
MODULE_FIRMWARE("airoha/an7581-libranpu.bin");
MODULE_FIRMWARE("airoha/an7583-libranpu.bin");
MODULE_DESCRIPTION("LibrANPU (Libre Airoha NPU) host driver, ABI v2");
MODULE_LICENSE("GPL");
