#!/usr/bin/env python3
"""Verify a built .cia's ARM11 kernel capabilities actually map DSP RAM.

Source-level checks (the .rsf) can only prove intent; this proves the bytes
makerom produced. Exheader layout: the NCCH header is the first 0x200 bytes of
the CIA's ExeFS-adjacent content; the exheader is the 0x400-byte block right
after it. Mapping descriptors in the ARM11 kernel capabilities (ACI at
exheader+0x200..0x400, AccessDesc copy at +0x600..0x800 relative to the
exheader's own base) are 32-bit LE words prefixed 0xff8; the low 20ish bits
carry the page number pair for a mapped range. See DOCTRINE.md's CIA section --
this is the same 0xff8-prefix check that was done by hand over FTP originally.

A .cia is a CIA container (certs, ticket, TMD, then the NCCH), not a bare
NCCH, so we scan for the NCCH magic ("NCCH" at offset+0x100 of the partition)
rather than assuming a fixed CIA header size across makerom versions.
"""
import struct
import sys


def find_ncch(data: bytes) -> int:
    idx = 0
    while True:
        idx = data.find(b"NCCH", idx)
        if idx < 0:
            raise SystemExit("no NCCH partition found in CIA")
        start = idx - 0x100
        if start >= 0:
            return start
        idx += 4


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} <t2k.cia>")
    with open(sys.argv[1], "rb") as f:
        data = f.read()

    ncch_off = find_ncch(data)
    exhdr_off = ncch_off + 0x200  # NCCH header is 0x200 bytes
    exhdr = data[exhdr_off:exhdr_off + 0x800]
    if len(exhdr) < 0x800:
        raise SystemExit("truncated exheader")

    # ARM11 kernel capabilities: ACI at +0x200, AccessDesc copy at +0x600,
    # each a fixed-layout block; mapping descriptors are a run of 32-bit words
    # prefixed 0xff8 (see 3dbrew "Exheader" ARM11KernelCapabilities).
    found_pairs = []
    for base in (0x200, 0x600):
        block = exhdr[base:base + 0x200]
        words = struct.unpack_from(f"<{len(block)//4}I", block, 0)
        pages = [w & 0xFFFFF for w in words if (w >> 20) == 0xFF8]
        found_pairs.append(pages)

    # 0x1ff50000 / 0x1ff70000 as page numbers (addr >> 12). Both the ACI and
    # the AccessDesc copy must carry them -- they have to agree, or the desc
    # check fails at install time even if the ACI alone looks right.
    want = {0x1ff50000 >> 12, 0x1ff70000 >> 12}
    ok = len(found_pairs) == 2 and all(w in pages for pages in found_pairs for w in want)

    if not ok:
        raise SystemExit(
            "DSP RAM mapping missing from the built exheader "
            f"(ACI pages={sorted(found_pairs[0]) if found_pairs else []}, "
            f"AccessDesc pages={sorted(found_pairs[1]) if len(found_pairs) > 1 else []})"
        )
    print("exheader verified: DSP RAM mapping present in ACI and AccessDesc")


if __name__ == "__main__":
    main()
