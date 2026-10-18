#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Correlate crashpad minidumps with the exact binaries that produced them.

Every build carries an NT_GNU_BUILD_ID note and every minidump module record
carries the same build id, so a dump can name its binary without any symbol
server. Subcommands:

  build-id FILE      print the hex build id of an ELF object
  modules DUMP       list each minidump module with its image and build id
  match DUMP FILE    exit 0 when the ELF object's build id appears in the dump

Keep an unstripped copy of each deployed binary (named by build id) and match
dumps against it before handing them to minidump_stackwalk.
"""
import argparse
import struct
import sys
from pathlib import Path

MINIDUMP_MAGIC = b"MDMP"
MODULE_LIST_STREAM = 4
BUILD_ID_NOTE = 3
CV_ELF_SIGNATURE = 0x4270454C  # 'BpEL': little-endian "LEpB"
MODULE_SIZE = 108


def elf_build_id(path: Path) -> str:
    data = path.read_bytes()
    if data[:4] not in (b"\x7fELF",):
        raise ValueError(f"{path}: not an ELF object")
    if data[4] != 2 or data[5] != 1:
        raise ValueError(f"{path}: only little-endian ELF64 is supported")
    (phoff,) = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _p_flags, p_offset = struct.unpack_from("<IIQ", data, phoff + phentsize * i)
        if p_type != 4:  # PT_NOTE
            continue
        pos = p_offset
        end = pos + 0x40000  # bounded scan; notes are tiny
        while pos + 12 <= len(data) and pos < end:
            namesz, descsz, n_type = struct.unpack_from("<III", data, pos)
            pos += 12
            name = data[pos:pos + namesz]
            pos += (namesz + 3) & ~3
            desc = data[pos:pos + descsz]
            pos += (descsz + 3) & ~3
            if n_type == BUILD_ID_NOTE and name.rstrip(b"\0") == b"GNU":
                return desc.hex()
    raise ValueError(f"{path}: no GNU build id note")


def dump_modules(path: Path):
    data = path.read_bytes()
    magic, _version, nstreams, dir_rva = struct.unpack_from("<4sIII", data, 0)
    if magic != MINIDUMP_MAGIC:
        raise ValueError(f"{path}: not a minidump")
    for i in range(nstreams):
        stream_type, _size, rva = struct.unpack_from("<III", data, dir_rva + 12 * i)
        if stream_type != MODULE_LIST_STREAM:
            continue
        (count,) = struct.unpack_from("<I", data, rva)
        for m in range(count):
            base, size, _checksum, _stamp, name_rva = struct.unpack_from("<QIIII", data, rva + 4 + MODULE_SIZE * m)
            cv_size, cv_rva = struct.unpack_from("<II", data, rva + 4 + MODULE_SIZE * m + 76)
            (name_len,) = struct.unpack_from("<I", data, name_rva)
            name = data[name_rva + 4:name_rva + 4 + name_len].decode("utf-16-le", "replace")
            build_id = ""
            cv = data[cv_rva:cv_rva + cv_size]
            if len(cv) >= 5 and struct.unpack_from("<I", cv, 0)[0] == CV_ELF_SIGNATURE:
                build_id = cv[4:].hex()
            yield name, base, size, build_id


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("build-id", help="print the ELF build id of a file").add_argument("file", type=Path)
    sub.add_parser("modules", help="list build ids recorded in a minidump").add_argument("dump", type=Path)
    match = sub.add_parser("match", help="exit 0 when the file's build id appears in the dump")
    match.add_argument("dump", type=Path)
    match.add_argument("file", type=Path)
    args = parser.parse_args()
    if args.command == "build-id":
        print(elf_build_id(args.file))
    elif args.command == "modules":
        for name, base, size, build_id in dump_modules(args.dump):
            print(f"{build_id or '-':40s} {base:#012x} {size:#010x}  {name}")
    elif args.command == "match":
        wanted = elf_build_id(args.file)
        for name, _base, _size, build_id in dump_modules(args.dump):
            if build_id == wanted:
                print(f"match: {name}")
                return 0
        print(f"no module with build id {wanted} in {args.dump}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
