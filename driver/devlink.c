// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * devlink: firmware and ABI versions of the running image, rx buffer
 * and tx token occupancy as resources, and the WLAN knobs as params.
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

enum {
	LIBRANPU_PARAM_FORCE_HOST = DEVLINK_PARAM_GENERIC_ID_MAX + 1,
	LIBRANPU_PARAM_AQM_ENABLE,
	LIBRANPU_PARAM_AQM_LIMIT,
	LIBRANPU_PARAM_AQM_TARGET,
	LIBRANPU_PARAM_AQM_DELAY_US,
	LIBRANPU_PARAM_AQM_INTERVAL_US,
	LIBRANPU_PARAM_AQM_MIN_FRAMES,
	LIBRANPU_PARAM_AQM_SMALL_BYTES,
};

static __le32 *libranpu_aqm_field(struct libranpu_wlan_aqm *q, u32 id)
{
	switch (id) {
	case LIBRANPU_PARAM_AQM_LIMIT:
		return &q->limit;
	case LIBRANPU_PARAM_AQM_TARGET:
		return &q->target;
	case LIBRANPU_PARAM_AQM_DELAY_US:
		return &q->delay_us;
	case LIBRANPU_PARAM_AQM_INTERVAL_US:
		return &q->interval_us;
	case LIBRANPU_PARAM_AQM_MIN_FRAMES:
		return &q->min_q;
	default:
		return &q->small;
	}
}

static int libranpu_param_get(struct devlink *dl, u32 id,
			      struct devlink_param_gset_ctx *ctx)
{
	struct libranpu *npu = devlink_priv(dl);

	mutex_lock(&npu->aqm_lock);
	if (id == LIBRANPU_PARAM_FORCE_HOST)
		ctx->val.vbool = READ_ONCE(npu->force_host);
	else if (id == LIBRANPU_PARAM_AQM_ENABLE)
		ctx->val.vbool = npu->aqm.on;
	else
		ctx->val.vu32 = le32_to_cpu(*libranpu_aqm_field(&npu->aqm, id));
	mutex_unlock(&npu->aqm_lock);
	return 0;
}

static int libranpu_param_set(struct devlink *dl, u32 id,
			      struct devlink_param_gset_ctx *ctx,
			      struct netlink_ext_ack *extack)
{
	struct libranpu *npu = devlink_priv(dl);
	struct libranpu_wlan_aqm q;
	int err;

	if (id == LIBRANPU_PARAM_FORCE_HOST)
		return libranpu_wlan_force_host(npu, 0, ctx->val.vbool);

	mutex_lock(&npu->aqm_lock);
	q = npu->aqm;
	if (id == LIBRANPU_PARAM_AQM_ENABLE)
		q.on = ctx->val.vbool;
	else
		*libranpu_aqm_field(&q, id) = cpu_to_le32(ctx->val.vu32);
	if (le32_to_cpu(q.delay_us) > le32_to_cpu(q.interval_us)) {
		NL_SET_ERR_MSG_MOD(extack, "aqm_delay_us above aqm_interval_us");
		err = -EINVAL;
	} else {
		err = libranpu_wlan_aqm_set(npu, &q);
	}
	if (!err)
		npu->aqm = q;
	mutex_unlock(&npu->aqm_lock);
	return err;
}

static int libranpu_interval_validate(struct devlink *dl, u32 id,
				      union devlink_param_value val,
				      struct netlink_ext_ack *extack)
{
	if (val.vu32 && val.vu32 <= LIBRANPU_AQM_INTERVAL_MAX_US)
		return 0;
	NL_SET_ERR_MSG_MOD(extack, "aqm_interval_us out of 1..150000");
	return -EINVAL;
}

#define LIBRANPU_PARAM(_id, _name, _type, _validate)			\
	DEVLINK_PARAM_DRIVER(LIBRANPU_PARAM_##_id, _name,		\
			     DEVLINK_PARAM_TYPE_##_type,		\
			     BIT(DEVLINK_PARAM_CMODE_RUNTIME),		\
			     libranpu_param_get, libranpu_param_set,		\
			     _validate)

static const struct devlink_param libranpu_params[] = {
	LIBRANPU_PARAM(FORCE_HOST, "wlan_force_host", BOOL, NULL),
	LIBRANPU_PARAM(AQM_ENABLE, "aqm_enable", BOOL, NULL),
	LIBRANPU_PARAM(AQM_LIMIT, "aqm_limit", U32, NULL),
	LIBRANPU_PARAM(AQM_TARGET, "aqm_target", U32, NULL),
	LIBRANPU_PARAM(AQM_DELAY_US, "aqm_delay_us", U32, NULL),
	LIBRANPU_PARAM(AQM_INTERVAL_US, "aqm_interval_us", U32,
		       libranpu_interval_validate),
	LIBRANPU_PARAM(AQM_MIN_FRAMES, "aqm_min_frames", U32, NULL),
	LIBRANPU_PARAM(AQM_SMALL_BYTES, "aqm_small_bytes", U32, NULL),
};

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
	npu->dl_params = !devl_params_register(npu->devlink, libranpu_params,
					       ARRAY_SIZE(libranpu_params));
	devl_register(npu->devlink);
	devl_unlock(npu->devlink);
}

void libranpu_devlink_unregister(struct libranpu *npu)
{
	devl_lock(npu->devlink);
	devl_unregister(npu->devlink);
	if (npu->dl_params)
		devl_params_unregister(npu->devlink, libranpu_params,
				       ARRAY_SIZE(libranpu_params));
	devl_resources_unregister(npu->devlink);
	devl_unlock(npu->devlink);
}
