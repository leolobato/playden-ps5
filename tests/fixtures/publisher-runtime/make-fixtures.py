#!/usr/bin/env python3
"""Regenerates the 7z fixtures of tests/runtime_publisher_runtime_test.cpp.

Needs `7z` (p7zip 17.05 or 7-Zip) on PATH. Every file is made up: small PE images with a
VERSIONINFO resource, pseudo x86 code (so BCJ2 has calls and jumps to convert) and text.

  bcj2.7z      1.2.3.4  BCJ2 over three LZMA streams, one file per folder, compressed header
  solid.7z     1.2.3.5  LZMA, solid (several files per folder), compressed header
  plain.7z     1.2.3.6  LZMA, one file per folder, uncompressed header (the test patches names)
  lzma2.7z     1.2.3.7  LZMA2 (unsupported)
  link.7z      1.2.3.8  LZMA, holds a symbolic link
  bcj.7z       1.2.3.9  the x86 BCJ filter over LZMA: one x86/runtime.dll past Playden's 1 MiB decode block, with
                        a converted CALL whose operand straddles the block's end (the filter holds those bytes
                        back for the next block) and pseudo code after it (which needs the right position)
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def pe_image(version, pe32_plus=False, code_size=12 * 1024, seed=1):
    """A minimal PE whose only section is .rsrc with one RT_VERSION resource."""
    a, b, c, d = version
    fixed = struct.pack("<13I", 0xFEEF04BD, 0x10000, a << 16 | b, c << 16 | d, a << 16 | b, c << 16 | d,
                        0x3F, 0, 4, 2, 0, 0, 0)
    key = "VS_VERSION_INFO\0".encode("utf-16-le")
    info = struct.pack("<HHH", 6 + len(key) + 2 + len(fixed), len(fixed), 0) + key + b"\0\0" + fixed
    rva = 0x1000
    rsrc = bytearray()
    rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 16, 0x80000000 | 0x18)
    rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 1, 0x80000000 | 0x30)
    rsrc += struct.pack("<IIHHHH", 0, 0, 0, 0, 0, 1) + struct.pack("<II", 0x409, 0x48)
    rsrc += struct.pack("<IIII", rva + 0x58, len(info), 0, 0)
    rsrc += bytes(0x58 - len(rsrc)) + info
    raw_size = (len(rsrc) + 0x1FF) & ~0x1FF

    optional_size = 0xF0 if pe32_plus else 0xE0
    optional = bytearray(optional_size)
    struct.pack_into("<H", optional, 0, 0x20B if pe32_plus else 0x10B)
    directories = 112 if pe32_plus else 96
    struct.pack_into("<I", optional, directories - 4, 16)
    struct.pack_into("<II", optional, directories + 16, rva, len(rsrc))
    coff = struct.pack("<HHIIIHH", 0x8664 if pe32_plus else 0x14C, 1, 0, 0, 0, optional_size, 0x2102)
    section = b".rsrc\0\0\0" + struct.pack("<IIIIIIHHI", len(rsrc), rva, raw_size, 0x200, 0, 0, 0, 0, 0x40000040)
    dos = bytearray(0x40)
    dos[0:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    header = bytes(dos) + b"PE\0\0" + coff + bytes(optional) + section
    image = header + bytes(0x200 - len(header)) + bytes(rsrc) + bytes(raw_size - len(rsrc))
    return image + pseudo_code(code_size, seed)


def pseudo_code(size, seed):
    """Bytes that look like x86 code: prologues, calls, jumps and conditional jumps."""
    out = bytearray()
    state = seed
    while len(out) < size:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        target = (state >> 4) % 4096 - 2048
        kind = state % 4
        if kind == 0:
            out += b"\x55\x8B\xEC\xE8" + struct.pack("<i", target)
        elif kind == 1:
            out += b"\xE9" + struct.pack("<i", target) + b"\x90"
        elif kind == 2:
            out += b"\x0F\x84" + struct.pack("<i", target)
        else:
            out += b"\x8B\x45\x08\x5D\xC3"
    return bytes(out[:size])


def write(root, path, data):
    full = os.path.join(root, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)


def tree(root, version, seed):
    write(root, "x86/runtime.dll", pe_image(version, seed=seed))
    write(root, "x86/readme.txt", b"hello from x86\n")
    write(root, "x86/empty.txt", b"")
    write(root, "x64/lib/helper.dll", pe_image((9, 8, 7, 6), pe32_plus=True, seed=seed + 1))
    write(root, "x64/locales/en.pak", b"en " * 300)
    os.makedirs(os.path.join(root, "x64/emptydir"), exist_ok=True)
    write(root, "other/skip.bin", pseudo_code(2048, seed + 2))


def seven_zip(root, archive, *switches, items=("x86", "x64", "other")):
    target = os.path.join(HERE, archive)
    if os.path.exists(target):
        os.remove(target)
    subprocess.run(["7z", "a", "-t7z", "-bd", "-mtm=off", "-mtc=off", "-mta=off", *switches, target, *items],
                   cwd=root, check=True, stdout=subprocess.DEVNULL)
    size = os.path.getsize(target)
    if size >= 64 * 1024:
        sys.exit(f"{archive} is {size} bytes, keep fixtures below 64 KiB")
    print(f"{archive}: {size} bytes")


def main():
    if not shutil.which("7z"):
        sys.exit("7z is needed")
    lzma = ["-m0=LZMA:d=64k", "-mf=off"]
    bcj2 = ["-m0=BCJ2", "-m1=LZMA:d=256k", "-m2=LZMA:d=64k:lc0:lp2", "-m3=LZMA:d=64k:lc0:lp2",
            "-mb0s0:1", "-mb0s1:2", "-mb0s2:3"]
    with tempfile.TemporaryDirectory() as scratch:
        for name, version, seed, switches in [
            ("bcj2.7z", (1, 2, 3, 4), 10, [*bcj2, "-ms=off"]),
            ("solid.7z", (1, 2, 3, 5), 20, [*lzma, "-ms=on"]),
            ("plain.7z", (1, 2, 3, 6), 30, [*lzma, "-ms=off", "-mhc=off"]),
        ]:
            root = os.path.join(scratch, name)
            tree(root, version, seed)
            if name == "plain.7z":
                # Names the test patches into unsafe ones (same length).
                write(root, "x86/ab/c.txt", b"c\n")
                write(root, "x86/a.txt", b"a\n")
                write(root, "x86/b.txt", b"b\n")
                write(root, "x64/zz.bin", b"zz\n")
            seven_zip(root, name, *switches)

        root = os.path.join(scratch, "bcj")
        head = pe_image((1, 2, 3, 9), code_size=0)
        block = 1 << 20
        # The folder holds this file alone, so file offsets are folder offsets: E8 two bytes before the block's end,
        # its operand (most significant byte 0, so the filter converts it) running across it.
        code = bytes(block - 2 - len(head)) + b"\xE8" + struct.pack("<i", 0x2010) + pseudo_code(16 * 1024, 60)
        write(root, "x86/runtime.dll", head + code)
        seven_zip(root, "bcj.7z", "-m0=BCJ", "-m1=LZMA:d=64k", items=("x86",))

        root = os.path.join(scratch, "lzma2")
        write(root, "x86/runtime.dll", pe_image((1, 2, 3, 7), code_size=1024, seed=40))
        seven_zip(root, "lzma2.7z", "-m0=LZMA2:d=64k", "-mf=off", items=("x86",))

        root = os.path.join(scratch, "link")
        write(root, "x86/runtime.dll", pe_image((1, 2, 3, 8), code_size=1024, seed=50))
        write(root, "x86/readme.txt", b"hello\n")
        os.symlink("readme.txt", os.path.join(root, "x86/link.txt"))
        seven_zip(root, "link.7z", *lzma, "-snl", items=("x86",))


if __name__ == "__main__":
    main()
