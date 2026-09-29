# SPDX-License-Identifier: GPL-3.0-only
# Firmware modules on the build machine, 32-bit like the NPU

FW := ../../firmware
CFLAGS := -m32 -O1 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
	-fsanitize=undefined -fno-sanitize-recover \
	-I$(FW)/include -I../../include -I$(FW) -I$(FW)/soc/an7583 -I$(FW)/plat/qemu
O := build

TESTS := arena spsc pool printf

all: $(addprefix $(O)/test_,$(TESTS))
	@for t in $^; do echo "== $$t"; $$t || exit 1; done

$(O)/test_arena: test_arena.c harness.c $(FW)/core/arena.c
$(O)/test_spsc: test_spsc.c harness.c
$(O)/test_pool: test_pool.c harness.c
$(O)/test_printf: test_printf.c harness.c $(O)/printf.o

# the formatter under another name, beside the C library's
$(O)/printf.o: $(FW)/lib/printf.c
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -Dvsnprintf=fw_vsnprintf -Dsnprintf=fw_snprintf -c $< -o $@

$(O)/test_%:
	@mkdir -p $(O)
	$(CC) $(CFLAGS) -o $@ $(filter %.c %.o,$^) -lpthread

clean:
	rm -rf $(O)

.PHONY: all clean
