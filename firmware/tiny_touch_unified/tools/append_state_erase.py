#!/usr/bin/env python3
"""Append erased blocks for the device-state sectors to the factory UF2.

Flashing the factory image then also clears the update marker, the PIV
identity, and the device configuration, so a board always comes back to
first setup. OTA updates never touch these sectors.
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

MAGIC_START0 = 0x0A324655
MAGIC_START1 = 0x9E5D5157
MAGIC_END = 0x0AB16F30
FLASH_BASE = 0x10000000
STATE_OFFSET = 0x1F0000  # FLASH_UPDATE_MARKER_OFFSET in main/flash_layout.h
FLASH_SIZE = 0x200000


def main() -> None:
    path = Path(sys.argv[1])
    data = path.read_bytes()
    if len(data) % 512:
        raise SystemExit(f"{path} is not a UF2 file")
    blocks = [data[i:i + 512] for i in range(0, len(data), 512)]
    family = struct.unpack_from("<I", blocks[0], 28)[0]
    kept = [b for b in blocks if struct.unpack_from("<I", b, 12)[0] < FLASH_BASE + STATE_OFFSET]
    for offset in range(STATE_OFFSET, FLASH_SIZE, 256):
        header = struct.pack("<8I", MAGIC_START0, MAGIC_START1, 0x2000, FLASH_BASE + offset,
                             256, 0, 0, family)
        kept.append(header + b"\xff" * 256 + b"\0" * (476 - 256) + struct.pack("<I", MAGIC_END))
    total = len(kept)
    out = bytearray()
    for number, block in enumerate(kept):
        block = bytearray(block)
        struct.pack_into("<II", block, 20, number, total)
        out += block
    path.write_bytes(out)


if __name__ == "__main__":
    main()
