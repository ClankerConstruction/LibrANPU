# SPDX-License-Identifier: GPL-3.0-only
# QEMU virt run of the an7583 image: 6 firmware harts + a host model hart

CROSS ?= riscv64-unknown-elf-
CC := $(CROSS)gcc
FW := ../../firmware
O := build
ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32
CPPFLAGS := -I$(FW)/include -I../../include -I$(FW) -I$(FW)/soc/an7583 \
	-I$(FW)/plat/qemu
CFLAGS := $(ARCH) -O2 -g -ffreestanding -fno-common -Wall -Wextra \
	-Wno-unused-parameter -Werror

all: $(O)/host.elf $(O)/bootrom.bin
	$(MAKE) -C $(FW) PLAT=qemu

$(O)/host.elf: host_start.S host.c $(FW)/lib/printf.c $(FW)/lib/string.c host.ld
	@mkdir -p $(O)
	$(CC) $(CPPFLAGS) $(CFLAGS) -nostdlib -nostartfiles -static -T host.ld \
		-Wl,--no-warn-rwx-segments -o $@ host_start.S host.c \
		$(FW)/lib/printf.c $(FW)/lib/string.c -lgcc

$(O)/bootrom.bin: bootrom.S
	@mkdir -p $(O)
	$(CC) $(CPPFLAGS) $(ARCH) -nostdlib -Wl,-Ttext=0x8F000000 -o $(O)/bootrom.elf $<
	$(CROSS)objcopy -O binary $(O)/bootrom.elf $@

clean:
	rm -rf $(O)

.PHONY: all clean
