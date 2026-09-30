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
#define LIBRANPU_ABI_MINOR		9

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
/* WLAN service                                                        */

enum libranpu_wlan_op {
	LIBRANPU_WLAN_ATTACH,
	LIBRANPU_WLAN_START,
	LIBRANPU_WLAN_STOP,
	LIBRANPU_WLAN_DETACH,
	LIBRANPU_WLAN_FORCE_HOST,
	LIBRANPU_WLAN_GET_STATS,	/* ctl in, stats out */
	LIBRANPU_WLAN_AQM,		/* aqm in (set: 1), aqm out */
	LIBRANPU_WLAN_STA_Q,		/* sta_q in (wcid), sta_q out */
};

/* one station's LAN to WiFi frames in the chip, by wcid */
struct libranpu_wlan_sta_q {
	__u8 radio;
	__u8 dropping;			/* out: CoDel drops under way */
	__le16 wcid;
	__le16 in_chip;			/* frames sent, not yet freed */
	__le16 count;			/* drops in the current dropping state */
	__le32 delay_us;		/* the chip's last reported time in it */
};

/*
 * Per-station limit on LAN to WiFi frames in the chip: a hard limit and
 * CoDel drops against a standing in-chip delay. Defaults: on, 8192,
 * target 0, 10 ms, 100 ms, 64, 256.
 */
#define LIBRANPU_AQM_INTERVAL_MAX_US	150000

struct libranpu_wlan_aqm {
	__u8 radio;
	__u8 set;			/* 0: only read */
	__u8 on;
	__u8 rsv;
	__le32 limit;			/* frames in the chip per station */
	__le32 target;			/* frames above which it stands; 0 off */
	__le32 delay_us;		/* in-chip time above which it stands */
	__le32 interval_us;
	__le32 min_q;			/* frames; a shorter queue never stands */
	__le32 small;			/* bytes; never dropped early */
};

enum libranpu_wlan_backend {
	LIBRANPU_WLAN_RRO31,
	LIBRANPU_WLAN_RRO3,
	LIBRANPU_WLAN_BA,
};

/* rings of the WiFi chip the NPU drives, and host rings it talks to */
enum libranpu_ring_kind {
	LIBRANPU_RING_NONE,
	LIBRANPU_RING_RX_DATA,		/* chip rx ring the NPU refills */
	LIBRANPU_RING_RXDMAD_C,		/* chip rx completion ring */
	LIBRANPU_RING_TX_DATA,		/* chip tx ring the NPU fills */
	LIBRANPU_RING_TXFREE,		/* chip rx ring of tx free reports */
	LIBRANPU_RING_HOST_RX,		/* host adaptor rx ring: frames to host */
	LIBRANPU_RING_HOST_RET,		/* host adaptor tx ring: rx ids back */
	LIBRANPU_RING_HOST_TX,		/* host adaptor tx ring: host frames */
	LIBRANPU_RING_HOST_TXFREE,	/* host adaptor rx ring: tx status */
};

#define LIBRANPU_WLAN_RINGS		32

/*
 * Chip rings: regs is the bus address of the ring's register block,
 * base is 0 (the NPU places them in its SRAM). Host rings: regs is the
 * host adaptor ring number, base the host memory. A host tx ring holds
 * chip tx descriptors (16 bytes) the NPU copies into its band's ring.
 * A tx free ring stays in host memory, armed by the host: regs and base
 * are both given, and the NPU re-arms slots with buf64 * 64 bytes.
 */
struct libranpu_wlan_ring {
	__u8 kind;			/* enum libranpu_ring_kind */
	__u8 band;
	__u8 link;			/* PCIe link of a chip ring */
	__u8 buf64;			/* tx free: buffer length / 64 */
	__le16 entries;
	__le16 entry_size;
	__le32 regs;
	__le32 base;
};

#define LIBRANPU_WLAN_F_FORCE_HOST	BIT(0)

struct libranpu_wlan_attach {
	__u8 radio;
	__u8 backend;			/* enum libranpu_wlan_backend */
	__u8 bands;
	__u8 nrings;
	__le32 flags;			/* LIBRANPU_WLAN_F_* */
	__u8 link_win[2];		/* inbound window of each PCIe link */
	__le16 rx_headroom;		/* the chip writes at buffer + this */
	__le32 pool_base;		/* rx buffers: base + id * 2048 */
	__le32 pool_ids;
	__le16 rx_mod_frames;		/* host rx line after this many */
	__le16 rx_mod_us;		/* or this long after the first */
	__le16 rx_buf_len;		/* bytes the chip may write */
	__le16 rsv;
	/* bitmap of ids the host still holds from a previous attach, or 0 */
	__le32 rx_held;
	/*
	 * LAN to WiFi: tokens [0, npu_tokens) are the NPU's, each with the
	 * buffer tx_pool_base + token * 2048; 0 tokens: no LAN to WiFi.
	 */
	__le32 tx_pool_base;
	__le16 npu_tokens;
	__le16 rsv2;
	/*
	 * Host memory with nrings ring entries; into each chip ring's base
	 * the NPU writes the bus address it placed the ring at.
	 */
	__le32 ring_table;
};

#define LIBRANPU_WLAN_RX		BIT(0)
#define LIBRANPU_WLAN_TX		BIT(1)
/* STOP: the chip's DMA is stopped, its frames are not waited for */
#define LIBRANPU_WLAN_NO_DRAIN		BIT(2)

struct libranpu_wlan_ctl {
	__u8 radio;
	__u8 dir;			/* LIBRANPU_WLAN_RX | _TX | _NO_DRAIN */
	__u8 on;			/* FORCE_HOST */
	__u8 page;			/* GET_STATS: 0 rx, 1 tx */
};

/* STOP and DETACH answer where every rx buffer id is */
struct libranpu_wlan_audit {
	__le32 free;			/* in the pool */
	__le32 chip;			/* under chip rx descriptors */
	__le32 host;			/* delivered, not returned */
	__le32 transit;			/* between NPU tasks */
	__le32 lost;			/* none of the above */
	__le32 expired;			/* bounded waits that ran out */
	__le32 fe;			/* sent to the frame engine, not back */
	__le32 tx_chip;			/* tx descriptors the chip had not taken */
	__le32 tx_host;			/* host tx entries the NPU had not taken */
	__le32 lan_tokens;		/* NPU tokens the chip had not freed */
};

/* since attach; each field is counted by one NPU task */
struct libranpu_wlan_stats {
	__le32 rx_frames;		/* chip completions */
	__le32 rx_ind[16];		/* per chip indication reason */
	__le32 rx_stale;		/* repeated or old: dropped */
	__le32 rx_pn_fail;		/* to the host as errors */
	__le32 rx_bad_id;
	__le32 host_segs;		/* host rx entries written */
	__le32 host_full;		/* times the host rx ring filled */
	__le32 host_dropped;		/* segments cut short */
	__le32 buf_refill[2];		/* per band: chip slots refilled */
	__le32 buf_returned;		/* ids back from the host */
	__le32 buf_bad_ret;
	__le32 buf_empty;		/* refills the pool could not cover */
	__le32 ppe_tx;			/* frames to the frame engine */
	__le32 ppe_full;		/* its ring was full: rx waited */
	__le32 ppe_bound;		/* forwarded, id back */
	__le32 ppe_unbound;		/* back to the host with FOE, CRSN */
	__le32 ppe_bad_id;
	__le16 ppe_crsn[32];		/* unbound returns per CPU reason, wrap */
};

/* GET_STATS page 1, the tx task's */
struct libranpu_wlan_tx_stats {
	__le32 descs[2];		/* per band: host descriptors to the chip */
	__le32 full[2];			/* per band: passes the chip ring was full */
	__le32 rewrite;			/* descriptor writes the chip overwrote */
	__le32 txfree_events[2];	/* per tx free ring: chip reports */
	__le32 txfree_host;		/* host token records */
	__le32 txfree_npu;		/* NPU tokens freed */
	__le32 txfree_bad;		/* old version, cut short or too long */
	__le32 txfree_other;		/* other chip reports passed on */
	__le32 txfree_full;		/* host tx free ring full: waited */
	__le32 lan_frames[2];		/* per frame engine ring: to the chip */
	__le32 lan_no_token;		/* passes out of tokens: waited */
	__le32 lan_ring_full;		/* passes the chip ring was full: waited */
	__le32 lan_bad;			/* bad band or length: dropped */
	__le32 lan_aqm_drops;		/* dropped for a standing in-chip delay */
	__le32 lan_limit_drops;		/* dropped at a station's hard limit */
	__le32 lan_tokens_used;		/* NPU tokens not free: rings and chip */
	__le32 txfree_stale;		/* NPU tokens freed the chip did not hold */
};

#define LIBRANPU_RX_BUF_SIZE		2048

/*
 * Host tx free ring entry, 8 bytes, NPU to host, in the order of the
 * chip's reports. A status entry counts for the station, not a token.
 * An event entry carries another chip report on the ring as it came:
 * token is its length, wcid its ring, the next entries its bytes.
 */
struct libranpu_host_txfree {
	__le16 token;
	__le16 wcid;			/* 0xffff: no station */
	__u8 kind;			/* enum libranpu_txfree_kind */
	__u8 count;			/* STATUS: transmit count */
	__u8 failed;			/* STATUS: the last try failed */
	__u8 rsv;
};

enum libranpu_txfree_kind {
	LIBRANPU_TXFREE_TOKEN = 1,
	LIBRANPU_TXFREE_STATUS,
	LIBRANPU_TXFREE_EVENT,
};

#define LIBRANPU_TXFREE_NO_WCID		0xffff

/*
 * Host rx ring entry, 24 bytes. The NPU writes words 1-3, then word 0.
 * The buffer is pool_base + id * 2048 + offset.
 */
struct libranpu_host_rx {
	__le32 ctrl;
	__le32 info;
	__le32 data;
	__le32 buf;
	__le32 rsv[2];
};

#define LIBRANPU_HRX_DONE		BIT(0)
#define LIBRANPU_HRX_SEG_LEN		GENMASK(14, 1)
#define LIBRANPU_HRX_LEN		GENMASK(28, 15)
#define LIBRANPU_HRX_LAST		BIT(29)
#define LIBRANPU_HRX_FOE		GENMASK(15, 0)
#define LIBRANPU_HRX_CRSN		GENMASK(20, 16)
#define LIBRANPU_HRX_REASON		GENMASK(24, 21)	/* enum libranpu_hrx_reason */
#define LIBRANPU_HRX_SEGS		GENMASK(31, 29)
#define LIBRANPU_HRX_WCID		GENMASK(11, 0)
#define LIBRANPU_HRX_BAND		GENMASK(13, 12)
#define LIBRANPU_HRX_ID			GENMASK(15, 0)
#define LIBRANPU_HRX_OFFSET		GENMASK(31, 16)

enum libranpu_hrx_reason {
	LIBRANPU_HRX_FORCED,		/* force host */
	LIBRANPU_HRX_CHIP,		/* the chip asked for the host */
	LIBRANPU_HRX_ERROR,		/* descriptor error */
	LIBRANPU_HRX_RAW,		/* no ethernet header */
	LIBRANPU_HRX_PPE,		/* the PPE did not forward it */
	LIBRANPU_HRX_CHAIN,		/* more than one segment */
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
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_wlan_ring) == 16);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_host_txfree) == 8);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_wlan_tx_stats) <= 240);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_wlan_attach) <=
		    LIBRANPU_CMD_PAYLOAD);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_host_rx) == 24);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_wlan_stats) <= 240);
LIBRANPU_ABI_ASSERT(sizeof(struct libranpu_dbg_block) <=
		    LIBRANPU_DBG_SIZE);

#endif /* __LIBRANPU_ABI_H */
