// SPDX-License-Identifier: GPL-2.0-or-later
/* devlink: firmware and ABI versions of the running image */

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
	devlink_register(npu->devlink);
}

void libranpu_devlink_unregister(struct libranpu *npu)
{
	devlink_unregister(npu->devlink);
}
