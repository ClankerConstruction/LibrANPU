#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the NPU firmware file: 128-byte header, code, data.

The header fields come from the ELF: sections, symbols and fw_info,
so the capability bits have a single source.
"""
import argparse
import struct
import sys
import zlib

from elftools.elf.elffile import ELFFile

MAGIC = 0x55504E41
HDR_SIZE = 128
ABI_MAJOR, ABI_MINOR = 2, 0


def sym(elf, name):
    s = elf.get_section_by_name(".symtab").get_symbol_by_name(name)
    if not s:
        sys.exit("missing symbol " + name)
    return s[0]["st_value"]


def span(elf, names):
    """Bytes of the named sections, laid out at their addresses."""
    secs = [elf.get_section_by_name(n) for n in names]
    secs = [s for s in secs if s and s["sh_size"]]
    base = min(s["sh_addr"] for s in secs)
    end = max(s["sh_addr"] + s["sh_size"] for s in secs)
    buf = bytearray(end - base)
    for s in secs:
        if s["sh_type"] != "SHT_NOBITS":
            off = s["sh_addr"] - base
            buf[off:off + s["sh_size"]] = s.data()
    return base, bytes(buf)


def read(elf, addr, size):
    for s in elf.iter_sections():
        a = s["sh_addr"]
        if s["sh_type"] != "SHT_NOBITS" and a <= addr < a + s["sh_size"]:
            off = addr - a
            return s.data()[off:off + size]
    sys.exit("address %#x not in the image" % addr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    ap.add_argument("--soc", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--harts", type=int, required=True)
    ap.add_argument("--dbg-offset", type=lambda x: int(x, 0), required=True)
    a = ap.parse_args()

    elf = ELFFile(open(a.elf, "rb"))
    code_load, code = span(elf, [".text", ".rodata"])
    data_start = sym(elf, "__data_start")
    data_end = sym(elf, "__data_end")
    data = read(elf, data_start, data_end - data_start) if data_end > data_start else b""
    if len(data) & 3:
        data += bytes(4 - (len(data) & 3))
    if len(code) & 3:
        code += bytes(4 - (len(code) & 3))

    info = read(elf, sym(elf, "fw_info"), 36)
    fw_version, services, backends, features = struct.unpack_from("<4I", info)
    build_id = info[16:36]

    code_off = HDR_SIZE
    data_off = code_off + len(code)
    hdr = struct.pack(
        "<IBBHHHIHH20s3I3I7I8I",
        MAGIC, 1, a.harts, HDR_SIZE, a.soc, 0, fw_version,
        ABI_MAJOR, ABI_MINOR, build_id,
        code_off, len(code), code_load,
        data_off, len(data), data_start,
        elf.header["e_entry"],
        sym(elf, "__dram_end") - code_load,
        sym(elf, "__bss_end") - data_start,
        a.dbg_offset,
        services, backends, features,
        *([0] * 8))
    assert len(hdr) == HDR_SIZE - 4
    crc = zlib.crc32(hdr + code + data) & 0xFFFFFFFF
    with open(a.out, "wb") as f:
        f.write(hdr + struct.pack("<I", crc) + code + data)
    print("%s: code %d B at %#x, data %d B, cluster %d B, crc %08x" % (
        a.out, len(code), code_load, len(data),
        sym(elf, "__bss_end") - data_start, crc))


if __name__ == "__main__":
    main()
