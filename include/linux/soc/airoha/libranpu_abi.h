/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LibrANPU host interface, ABI v2.
 *
 * Layouts shared by the NPU firmware and the host driver. All fields are
 * little endian. The host owns every buffer described here.
 */
#ifndef __LIBRANPU_ABI_H
#define __LIBRANPU_ABI_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/build_bug.h>
#else
#include "fw/abi_types.h"
#endif

#define LIBRANPU_ABI_MAJOR		2
#define LIBRANPU_ABI_MINOR		0

/* SoC ids, as in the image header and GET_CAPS */
#define LIBRANPU_SOC_AN7552		0x7552
#define LIBRANPU_SOC_AN7581		0x7581
#define LIBRANPU_SOC_AN7583		0x7583

/* ------------------------------------------------------------------ */
/* Image header: the first 128 bytes of the firmware file              */

#define LIBRANPU_IMG_MAGIC		0x55504e41	/* "ANPU" */
#define LIBRANPU_IMG_VERSION		1
#define LIBRANPU_IMG_HDR_SIZE		128

/* one loadable section: file offset, size and NPU load address */
struct libranpu_img_sect {
	__le32 offset;
	__le32 size;
	__le32 load;
};

struct libranpu_img_hdr {
	__le32 magic;
	__u8 hdr_version;
	__u8 harts;			/* harts the image starts */
	__le16 hdr_size;
	__le16 soc;
	__le16 rsv0;
	__le32 fw_version;		/* major << 16 | minor << 8 | patch */
	__le16 abi_major;
	__le16 abi_minor;
	__u8 build_id[20];		/* source revision */
	struct libranpu_img_sect code;		/* text + rodata, to DRAM */
	struct libranpu_img_sect data;		/* to cluster SRAM */
	__le32 entry;			/* boot address of every hart */
	__le32 dram_size;		/* code + stacks from code.load */
	__le32 cluster_size;		/* data + bss from data.load */
	__le32 dbg_offset;		/* debug block, from data.load */
	__le32 services;		/* LIBRANPU_SVC_* */
	__le32 wlan_backends;		/* LIBRANPU_WLAN_BE_* */
	__le32 wlan_features;		/* LIBRANPU_WLAN_F_* */
	__le32 rsv1[8];
	__le32 crc32;			/* bytes 0..123 and all sections */
};

/* ------------------------------------------------------------------ */
/* Boot block: host DRAM, its NPU address in MIB10 before the trigger  */

#define LIBRANPU_BOOT_MAGIC		0x42555043	/* "CPUB" */
#define LIBRANPU_BOOT_READY		0x52454459	/* "REDY" */

#define LIBRANPU_BOOT_F_UART		BIT(0)	/* boot and fatal lines */

struct libranpu_boot {
	/* host fills before the trigger */
	__le32 magic;
	__le16 abi_major;
	__le16 abi_minor;
	__le32 flags;
	__le32 rsv0;
	__le64 cmd_ring;
	__le64 evt_ring;
	__le32 cmd_entries;		/* power of two */
	__le32 evt_entries;		/* power of two */
	__le32 rsv1[6];

	/* NPU fills, ready last */
	__le32 ready;
	__le32 status;			/* 0 or a negative errno */
	__le32 fw_version;
	__le32 harts_up;		/* mask of harts in their loop */
	__le32 boot_cycles;		/* hart 0, reset vector to ready */
	__le32 cpu_mhz;
	__le32 rsv2[10];

	/* ring indices, free running */
	__le32 cmd_prod;		/* host */
	__le32 cmd_cons;		/* NPU */
	__le32 evt_prod;		/* NPU */
	__le32 evt_cons;		/* host */
	__le32 rsv3[4];

	/* per hart trap records, written once by the faulting hart */
	__le32 fault[8];		/* mcause | LIBRANPU_FAULT_VALID */
};

#define LIBRANPU_FAULT_VALID		BIT(31)

/* ------------------------------------------------------------------ */
/* Command ring: host to NPU                                           */

#define LIBRANPU_CMD_SIZE		256
#define LIBRANPU_CMD_PAYLOAD		240
#define LIBRANPU_CMD_ENTRIES		32

#define LIBRANPU_CMD_F_NO_EVENT		BIT(0)

struct libranpu_cmd {
	__le16 opcode;
	__u8 service;
	__u8 flags;
	__le32 seq;			/* echoed in CMD_DONE */
	__le16 len;			/* request payload bytes */
	__le16 rsp_len;			/* NPU: response payload bytes */
	__le32 status;			/* NPU: 0 or a negative errno */
	__u8 payload[LIBRANPU_CMD_PAYLOAD];
};

enum libranpu_service {
	LIBRANPU_SVC_CTL,
	LIBRANPU_SVC_WLAN,
	LIBRANPU_SVC_FE,
	LIBRANPU_SVC_DBA,
	LIBRANPU_SVC_DBG,
};

enum libranpu_ctl_op {
	LIBRANPU_CTL_NOP,
	LIBRANPU_CTL_GET_CAPS,
	LIBRANPU_CTL_STATS_CONFIG,
	LIBRANPU_CTL_IDLE_CONFIG,
	LIBRANPU_CTL_AUDIT,
	LIBRANPU_CTL_TRACE_CONFIG,
	LIBRANPU_CTL_RESET,
	LIBRANPU_CTL_GET_TASKS,
};

/* services bitmap of GET_CAPS and the image header */
#define LIBRANPU_SVC_F_WLAN		BIT(0)
#define LIBRANPU_SVC_F_TUNNEL		BIT(1)
#define LIBRANPU_SVC_F_FRAG		BIT(2)
#define LIBRANPU_SVC_F_L4S		BIT(3)
#define LIBRANPU_SVC_F_FLOW_STATS	BIT(4)
#define LIBRANPU_SVC_F_DBA		BIT(5)
#define LIBRANPU_SVC_F_TR471		BIT(6)
#define LIBRANPU_SVC_F_PLUGIN		BIT(7)
#define LIBRANPU_SVC_F_DBG		BIT(31)

#define LIBRANPU_WLAN_BE_RRO31		BIT(0)
#define LIBRANPU_WLAN_BE_RRO3		BIT(1)
#define LIBRANPU_WLAN_BE_BA		BIT(2)

#define LIBRANPU_WLAN_F_NPU_TX		BIT(0)
#define LIBRANPU_WLAN_F_RX_RETURN	BIT(1)
#define LIBRANPU_WLAN_F_TX_HOST_PAYLOAD BIT(2)
#define LIBRANPU_WLAN_F_TXFREE_STATUS	BIT(3)
#define LIBRANPU_WLAN_F_TXFREE_RATE	BIT(4)
#define LIBRANPU_WLAN_F_TXFREE_AIRTIME BIT(5)
#define LIBRANPU_WLAN_F_SCHED_DELAY	BIT(6)
#define LIBRANPU_WLAN_F_SCHED_FQ	BIT(7)
#define LIBRANPU_WLAN_F_ECN_MARK	BIT(8)
#define LIBRANPU_WLAN_F_STA_STATS	BIT(9)

#define LIBRANPU_MAX_HARTS		8

struct libranpu_caps {
	__le16 abi_major;
	__le16 abi_minor;
	__le32 fw_version;
	__u8 build_id[20];
	__le16 soc;
	__u8 harts;
	__u8 rsv0;
	__le32 harts_up;
	__le32 sram_total;
	__le32 sram_free;
	__le32 cluster_total;
	__le32 cluster_free;
	__le32 services;
	__le32 wlan_backends;
	__le32 wlan_features;
	__le16 max_radios;
	__le16 max_stations;
	__le32 max_rx_ids;
	__le32 max_npu_tokens;
	__le16 max_rings_per_radio;
	__le16 cpu_mhz;
	__le32 task_map[LIBRANPU_MAX_HARTS];	/* LIBRANPU_TASK_* bits */
};

/* task ids, bits of task_map */
enum libranpu_task_id {
	LIBRANPU_TASK_CTL,
	LIBRANPU_TASK_HEALTH,
	LIBRANPU_TASK_DBG,
	LIBRANPU_TASK_RX,
	LIBRANPU_TASK_TX,
	LIBRANPU_TASK_BUF,
	LIBRANPU_TASK_HOST,
	LIBRANPU_TASK_SVC,
	LIBRANPU_TASK_DBA,
	LIBRANPU_TASK_MAX,
};

struct libranpu_reset_rsp {
	__le32 parked;			/* mask of harts parked */
};

/* GET_TASKS: one record per task in the runner, hart order */
struct libranpu_task_info {
	__u8 hart;
	__u8 id;			/* enum libranpu_task_id */
	__le16 rsv;
	__le32 passes;
	__le32 work;
};

#define LIBRANPU_TASK_INFO_MAX \
	(LIBRANPU_CMD_PAYLOAD / sizeof(struct libranpu_task_info))

/* ------------------------------------------------------------------ */
/* Debug service: bench probes of the hardware, debug images only      */

enum libranpu_dbg_op {
	LIBRANPU_DBG_PEEK,		/* NPU address, words */
	LIBRANPU_DBG_POKE,		/* NPU address, value */
	LIBRANPU_DBG_PROBE,		/* hart, probe id, args */
};

struct libranpu_dbg_peek {
	__le32 addr;
	__le32 words;			/* at most 56 */
};

struct libranpu_dbg_poke {
	__le32 addr;
	__le32 val;
};

enum libranpu_probe_id {
	LIBRANPU_PROBE_CYCLES,		/* mcycle over an empty loop */
	LIBRANPU_PROBE_LOAD,		/* cycles per load: addr, stride, n */
	LIBRANPU_PROBE_WFI,		/* wfi with a timer wake */
	LIBRANPU_PROBE_XHART,		/* cached write, other hart reads */
	LIBRANPU_PROBE_CLUSTER,	/* cluster SRAM size */
	LIBRANPU_PROBE_MAX,
};

struct libranpu_dbg_probe {
	__u8 hart;
	__u8 probe;			/* enum libranpu_probe_id */
	__le16 rsv;
	__le32 arg[4];
};

struct libranpu_dbg_probe_rsp {
	__le32 res[8];
};

/* ------------------------------------------------------------------ */
/* Event ring: NPU to host                                             */

#define LIBRANPU_EVT_SIZE		32
#define LIBRANPU_EVT_ENTRIES		256
#define LIBRANPU_EVT_PAYLOAD		24

enum libranpu_evt_type {
	LIBRANPU_EVT_NONE,
	LIBRANPU_EVT_CMD_DONE,
	LIBRANPU_EVT_FATAL,
	LIBRANPU_EVT_TASK_STALL,
	LIBRANPU_EVT_AUDIT,
	LIBRANPU_EVT_POOL_LOW,
	LIBRANPU_EVT_STA,
	LIBRANPU_EVT_DBA,
	LIBRANPU_EVT_LOG,
	LIBRANPU_EVT_MAX,
};

struct libranpu_evt {
	__le16 type;
	__le16 len;			/* payload bytes */
	__le32 seq;			/* command seq for CMD_DONE */
	__u8 payload[LIBRANPU_EVT_PAYLOAD];
};

struct libranpu_evt_cmd_done {
	__le32 status;
	__le16 opcode;
	__u8 service;
	__u8 rsv;
};

struct libranpu_evt_fatal {
	__u8 hart;
	__u8 task;
	__le16 rsv;
	__le32 mcause;
	__le32 mepc;
	__le32 mtval;
	__le32 ra;
	__le32 sp;
};

struct libranpu_evt_stall {
	__u8 hart;
	__u8 task;
	__le16 rsv;
	__le32 pc;
	__le32 passes;
};

/* ------------------------------------------------------------------ */
/* Debug block: fixed place in cluster SRAM, dbg_offset in the header  */

#define LIBRANPU_DBG_MAGIC		0x32474244	/* "DBG2" */
#define LIBRANPU_DBG_VERSION		1
#define LIBRANPU_DBG_SIZE		4096
#define LIBRANPU_DBG_TASKS		16
#define LIBRANPU_DBG_TRACE		64

struct libranpu_dbg_hart {
	__le32 state;			/* LIBRANPU_HART_* */
	__le32 heartbeat;		/* runner passes */
	__le32 ntasks;
	__le32 rsv;
	/* trap record */
	__le32 mcause;
	__le32 mepc;
	__le32 mtval;
	__le32 ra;
	__le32 sp;
	__le32 task;
	__le32 rsv1[2];
};

enum libranpu_hart_state {
	LIBRANPU_HART_OFF,
	LIBRANPU_HART_INIT,
	LIBRANPU_HART_RUN,
	LIBRANPU_HART_PARKED,
	LIBRANPU_HART_FAULT,
};

struct libranpu_dbg_task {
	__u8 hart;
	__u8 id;
	__le16 rsv;
	__le32 passes;
	__le32 work;			/* units the task reported */
	__le32 busy_lo;			/* cycles in passes with work */
	__le32 busy_hi;
	__le32 idle_lo;			/* cycles in empty passes */
	__le32 idle_hi;
	__le32 errors;
};

struct libranpu_dbg_trace {
	__le16 type;
	__u8 hart;
	__u8 rsv;
	__le32 cycles;
	__le32 arg0;
	__le32 arg1;
};

enum libranpu_trace_type {
	LIBRANPU_TRACE_BOOT = 1,	/* arg0 status, arg1 harts up */
	LIBRANPU_TRACE_CMD,		/* arg0 service << 16 | opcode, arg1 status */
	LIBRANPU_TRACE_EVT_DROP,	/* arg0 type */
	LIBRANPU_TRACE_FAULT,		/* arg0 hart, arg1 mcause */
	LIBRANPU_TRACE_STALL,		/* arg0 hart, arg1 pc */
	LIBRANPU_TRACE_PARK,		/* arg0 mask parked */
};

struct libranpu_dbg_block {
	__le32 magic;
	__le16 version;
	__le16 size;
	__le32 harts;
	__le32 ntasks;
	__le32 trace_head;		/* next slot, free running */
	__le32 rsv[11];
	struct libranpu_dbg_hart hart[LIBRANPU_MAX_HARTS];	/* 0x040 */
	struct libranpu_dbg_task task[LIBRANPU_DBG_TASKS];	/* 0x1c0 */
	struct libranpu_dbg_trace trace[LIBRANPU_DBG_TRACE]; /* 0x3c0 */
};

/* ------------------------------------------------------------------ */

#define LIBRANPU_ABI_ASSERT(_c)		_Static_assert(_c, #_c)

LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_img_hdr) ==
		    LIBRANPU_IMG_HDR_SIZE);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_boot) == 192);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_cmd) == LIBRANPU_CMD_SIZE);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_evt) == LIBRANPU_EVT_SIZE);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_caps) <=
		    LIBRANPU_CMD_PAYLOAD);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_evt_fatal) <=
		    LIBRANPU_EVT_PAYLOAD);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_dbg_hart) == 48);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_dbg_task) == 32);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_dbg_block) <=
		    LIBRANPU_DBG_SIZE);

#endif /* __LIBRANPU_ABI_H */
