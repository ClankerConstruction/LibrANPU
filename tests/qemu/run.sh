#!/bin/sh
# run.sh [qemu args]: boot the QEMU image with the host model, exit 0 on PASS
set -e
cd "$(dirname "$0")"
make -s
IMG=../../firmware/build/an7583-qemu/an7583-libranpu.bin
LOADERS=""
for h in 0 1 2 3 4 5; do
	LOADERS="$LOADERS -device loader,addr=0x8F000000,cpu-num=$h"
done
exec timeout ${TIMEOUT:-60} qemu-system-riscv32 -M virt -cpu rv32 -smp 7 -m 256M \
	-bios none -display none -serial stdio -monitor none \
	-device loader,file=build/bootrom.bin,addr=0x8F000000,force-raw=on \
	-device loader,file=$IMG,addr=0x8A000000,force-raw=on \
	-device loader,file=build/host.elf,cpu-num=6 \
	$LOADERS "$@"
