# LibrANPU

Libre Airoha NPU: firmware and host driver for the NPU of the Airoha AN7552,
AN7581 and AN7583, on the host interface ABI v2.

| path | content |
|---|---|
| `include/linux/soc/airoha/libranpu_abi.h` | the ABI: image header, boot block, command/event rings, debug block; shared by firmware and driver |
| `firmware/` | RV32 firmware, one image per SoC |
| `tests/unit/` | firmware modules on the build machine |
| `tests/qemu/` | the image on QEMU `virt` with a host model on a spare hart |
| `driver/` | `libranpu` host driver |
| `docs/` | firmware, driver, testing |

## Build

```sh
make -C firmware SOC=an7583          # build/an7583-npu/an7583-libranpu.bin
make -C firmware SOC=an7583 PLAT=qemu
```

Needs `riscv64-unknown-elf-gcc` and python3 with `pyelftools`.

## Test

```sh
make -C tests/unit                   # needs gcc -m32
tests/qemu/run.sh                    # needs qemu-system-riscv32; PASS/FAIL
```
