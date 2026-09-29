# SPDX-License-Identifier: GPL-3.0-only
#
# make [SOC=an7583|an7581|an7552] [PLAT=npu|qemu]
# Output: build/<soc>-<plat>/<soc>-libranpu.bin

SOC ?= an7583
PLAT ?= npu
CROSS ?= riscv64-unknown-elf-
O ?= build/$(SOC)-$(PLAT)

include configs/$(SOC).mk

CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump

BUILD_ID ?= $(shell git rev-parse --verify -q HEAD || echo 00)

ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32
CPPFLAGS := -Iinclude -I../include -I. -Isoc/$(SOC) -Iplat/$(PLAT) \
	-I$(O)/gen
CPPFLAGS-$(CONFIG_DBG) += -DCONFIG_DBG
CPPFLAGS-$(CONFIG_WLAN) += -DCONFIG_WLAN
CFLAGS := $(ARCH) -O2 -g -std=gnu11 -ffreestanding -fno-common \
	-ffunction-sections -fdata-sections -fno-strict-aliasing \
	-Wall -Wextra -Wno-unused-parameter -Werror
LDFLAGS := $(ARCH) -nostdlib -nostartfiles -static -Wl,--gc-sections \
	-Wl,-T,$(O)/link.ld -Wl,-Map,$(O)/firmware.map -Wl,--no-warn-rwx-segments

SRCS := arch/rv32/crt0.S arch/rv32/trap.S \
	$(wildcard core/*.c ctl/*.c lib/*.c) \
	dbg/dbgblk.c plat/$(PLAT)/plat.c
SRCS-$(CONFIG_DBG) += dbg/probe.c
SRCS-$(CONFIG_WLAN) += $(wildcard wlan/*.c)
SRCS += $(SRCS-y)
OBJS := $(patsubst %,$(O)/%.o,$(basename $(SRCS)))

SOC_ID := $(shell sed -n 's/^\#define SOC_ID[[:space:]]*//p' soc/$(SOC)/soc.h)
SOC_HARTS := $(shell sed -n 's/^\#define SOC_HARTS[[:space:]]*//p' soc/$(SOC)/soc.h)
SOC_DBG := $(shell sed -n 's/^\#define SOC_DBG_OFFSET[[:space:]]*//p' soc/$(SOC)/soc.h)

IMG := $(O)/$(SOC)-libranpu.bin

all: $(IMG)

$(IMG): $(O)/firmware.elf scripts/mkimage.py
	@python3 scripts/mkimage.py $< $@ --soc $(SOC_ID) --harts $(SOC_HARTS) \
		--dbg-offset $(SOC_DBG)
	@$(OBJDUMP) -d $< > $(O)/firmware.dis

$(O)/firmware.elf: $(OBJS) $(O)/link.ld
	@echo "  LD $@"
	@$(CC) $(LDFLAGS) -o $@ $(OBJS) -lgcc

$(O)/link.ld: arch/rv32/link.ld.S soc/$(SOC)/soc.h plat/$(PLAT)/map.h
	@mkdir -p $(dir $@)
	$(CC) -E -P -x c $(CPPFLAGS) $< -o $@

$(O)/gen/build_id.h: FORCE
	@mkdir -p $(dir $@)
	@echo "#define FW_BUILD_ID { $$(echo $(BUILD_ID) | \
		sed 's/../0x&, /g; s/, $$//') }" > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@

$(O)/%.o: %.c $(O)/gen/build_id.h
	@mkdir -p $(dir $@)
	@echo "  CC $<"
	@$(CC) $(CPPFLAGS) $(CPPFLAGS-y) -include build_id.h $(CFLAGS) -MMD -c $< -o $@

$(O)/%.o: %.S
	@mkdir -p $(dir $@)
	@echo "  AS $<"
	@$(CC) $(CPPFLAGS) $(CPPFLAGS-y) $(ARCH) -MMD -c $< -o $@

clean:
	rm -rf build

.PHONY: all clean FORCE
-include $(OBJS:.o=.d)
