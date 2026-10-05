# SPDX-License-Identifier: GPL-2.0-or-later
# make KERNEL_DIR=... ARCH=arm64 CROSS_COMPILE=...

obj-m += libranpu.o
libranpu-y := core.o cmd.o devlink.o debugfs.o health.o wlan.o rxbuf.o
ccflags-y += -I$(src)/../include

ifneq ($(KERNELRELEASE),)
else
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean
endif
