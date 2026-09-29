/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * LibrANPU: API for the drivers that use its services.
 */
#ifndef __LIBRANPU_H
#define __LIBRANPU_H

#include <linux/notifier.h>
#include <linux/soc/airoha/libranpu_abi.h>

struct libranpu;
struct device;

/* from the "airoha,npu" phandle of @dev */
struct libranpu *libranpu_get(struct device *dev);
void libranpu_put(struct libranpu *npu);

struct device *libranpu_dev(struct libranpu *npu);
const struct libranpu_caps *libranpu_caps(struct libranpu *npu);

/*
 * Run one command and sleep until it completes. @rsp_len is the room
 * in @rsp on entry and the response length on return.
 */
int libranpu_cmd(struct libranpu *npu, u8 service, u16 opcode,
		 const void *req, u16 req_len, void *rsp, u16 *rsp_len);

enum libranpu_event {
	LIBRANPU_FATAL,			/* data: the hart number */
};

int libranpu_register_notifier(struct libranpu *npu,
			       struct notifier_block *nb);
void libranpu_unregister_notifier(struct libranpu *npu,
				  struct notifier_block *nb);

#endif
