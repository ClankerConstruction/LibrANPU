// SPDX-License-Identifier: GPL-3.0-only
/* The one place the build config turns into capability bits */

#include "fw/info.h"
#include <linux/soc/airoha/libranpu_abi.h>

#ifndef FW_BUILD_ID
#define FW_BUILD_ID	{ 0 }
#endif

const struct fw_info fw_info __attribute__((used)) = {
	.fw_version = FW_VERSION,
	.services = 0
#ifdef CONFIG_DBG
		| LIBRANPU_SVC_F_DBG
#endif
		,
	.wlan_backends = 0,
	.wlan_features = 0,
	.build_id = FW_BUILD_ID,
};
