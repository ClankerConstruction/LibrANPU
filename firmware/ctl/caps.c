// SPDX-License-Identifier: GPL-3.0-only

#include "fw/info.h"
#include "fw/lib.h"
#include "core/arena.h"
#include "core/task.h"
#include "ctl/ctl.h"
#include "dbg/dbg.h"
#include "plat/plat.h"
#include "wlan/wlan.h"

u32 harts_up(void)
{
	u32 h, up = 0;

	for (h = 0; h < SOC_HARTS; h++)
		if (dbg_hart_state_get(h) == LIBRANPU_HART_RUN)
			up |= BIT(h);
	return up;
}

void caps_fill(struct libranpu_caps *caps)
{
	u32 h;

	memset(caps, 0, sizeof(*caps));
	caps->abi_major = LIBRANPU_ABI_MAJOR;
	caps->abi_minor = LIBRANPU_ABI_MINOR;
	caps->fw_version = fw_info.fw_version;
	memcpy(caps->build_id, fw_info.build_id, sizeof(caps->build_id));
	caps->soc = SOC_ID;
	caps->harts = SOC_HARTS;
	caps->harts_up = harts_up();
	caps->sram_total = npu_sram.size;
	caps->sram_free = arena_free_bytes(&npu_sram);
	caps->cluster_total = cluster_sram.size;
	caps->cluster_free = arena_free_bytes(&cluster_sram);
	caps->services = fw_info.services;
	caps->wlan_backends = fw_info.wlan_backends;
	caps->wlan_features = fw_info.wlan_features;
	caps->cpu_mhz = plat_cpu_mhz();
#ifdef CONFIG_WLAN
	caps->max_radios = 1;
	caps->max_rx_ids = WLAN_POOL_MAX;
	caps->max_rings_per_radio = LIBRANPU_WLAN_RINGS;
#endif
	for (h = 0; h < SOC_HARTS; h++)
		caps->task_map[h] = task_map(h);
}
