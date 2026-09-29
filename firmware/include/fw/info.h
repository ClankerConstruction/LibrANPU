/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef __FW_INFO_H
#define __FW_INFO_H

#include "fw/types.h"

#define FW_VERSION_MAJOR	0
#define FW_VERSION_MINOR	1
#define FW_VERSION_PATCH	0
#define FW_VERSION		(FW_VERSION_MAJOR << 16 | FW_VERSION_MINOR << 8 | \
				 FW_VERSION_PATCH)

/* what the image carries; the image header is built from it */
struct fw_info {
	u32 fw_version;
	u32 services;
	u32 wlan_backends;
	u32 wlan_features;
	u8 build_id[20];
};

extern const struct fw_info fw_info;

#endif
