// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * devlink: firmware and ABI versions of the running image, and rx
 * buffer and tx token occupancy as resources.
 */

#include <net/devlink.h>

#include "libranpu.h"

static int libranpu_info_get(struct devlink *dl, struct devlink_info_req *req,
			     struct netlink_ext_ack *extack)
{
	struct libranpu *npu = devlink_priv(dl);
	u32 v = le32_to_cpu(npu->caps.fw_version);
	char buf[48];
	int err;

	err = devlink_info_version_fixed_put(req, DEVLINK_INFO_VERSION_GENERIC_ASIC_ID,
					     npu->soc->name);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "%u.%u.%u", v >> 16, (v >> 8) & 0xff,
		 v & 0xff);
	err = devlink_info_version_running_put(req, DEVLINK_INFO_VERSION_GENERIC_FW,
					       buf);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "%u.%u", le16_to_cpu(npu->caps.abi_major),
		 le16_to_cpu(npu->caps.abi_minor));
	err = devlink_info_version_running_put(req, "fw.abi", buf);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "%20phN", npu->caps.build_id);
	return devlink_info_version_running_put(req,
						DEVLINK_INFO_VERSION_GENERIC_FW_BUNDLE_ID,
						buf);
}

enum {
	LIBRANPU_RES_RX_BUFFERS = 1,
	LIBRANPU_RES_TX_TOKENS,
};

/* rx buffers the network stack holds */
static u64 libranpu_rx_buffers_occ(void *priv)
{
	struct libranpu *npu = priv;

	return READ_ONCE(npu->rxb.lent);
}

/* NPU tokens under the frame engine's rings and in the chip */
static u64 libranpu_tx_tokens_occ(void *priv)
{
	struct libranpu_wlan_tx_stats st;

	if (libranpu_wlan_tx_stats(priv, 0, &st))
		return 0;
	return le32_to_cpu(st.lan_tokens_used);
}

static void libranpu_resource(struct libranpu *npu, const char *name,
			      u64 size, u64 id,
			      devlink_resource_occ_get_t *occ_get)
{
	struct devlink_resource_size_params p;

	if (!size)
		return;
	devlink_resource_size_params_init(&p, size, size, 1,
					  DEVLINK_RESOURCE_UNIT_ENTRY);
	if (devl_resource_register(npu->devlink, name, size, id,
				   DEVLINK_RESOURCE_ID_PARENT_TOP, &p))
		return;
	devl_resource_occ_get_register(npu->devlink, id, occ_get, npu);
}

static const struct devlink_ops libranpu_devlink_ops = {
	.info_get = libranpu_info_get,
};

struct libranpu *libranpu_devlink_alloc(struct device *dev)
{
	struct devlink *dl;
	struct libranpu *npu;

	dl = devlink_alloc(&libranpu_devlink_ops, sizeof(*npu), dev);
	if (!dl)
		return NULL;
	npu = devlink_priv(dl);
	npu->devlink = dl;
	return npu;
}

void libranpu_devlink_free(struct libranpu *npu)
{
	devlink_free(npu->devlink);
}

void libranpu_devlink_register(struct libranpu *npu)
{
	devl_lock(npu->devlink);
	libranpu_resource(npu, "rx_buffers", npu->pool.cpu ? npu->pool.ids : 0,
			  LIBRANPU_RES_RX_BUFFERS, libranpu_rx_buffers_occ);
	libranpu_resource(npu, "tx_tokens", npu->tx_tokens,
			  LIBRANPU_RES_TX_TOKENS, libranpu_tx_tokens_occ);
	devl_register(npu->devlink);
	devl_unlock(npu->devlink);
}

void libranpu_devlink_unregister(struct libranpu *npu)
{
	devl_lock(npu->devlink);
	devl_unregister(npu->devlink);
	devl_resources_unregister(npu->devlink);
	devl_unlock(npu->devlink);
}
