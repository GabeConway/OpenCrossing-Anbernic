#!/usr/bin/env python3
"""Inspect an Animal Crossing (GAFE01) .gci save for inventory state.

Used for save forensics (e.g. the 2026-07 save-while-holding shovel dupe —
kb/issues.md). Prints the equipment (held item) and pocket contents for the
main Save_t and its duplicate copy, for every player slot that looks
populated.

Layout (verified by parsing, kb/issues.md):
  GCI header: 64 bytes
  Save_t: file_data+0x26000, duplicate copy at +0x4C000
  Private_c: save+0x20, 0x2440 bytes each, 4 slots
  pockets: priv+0x68, 15 x u16 big-endian item ids
  equipment: priv+0x4A4, u16 BE
Item ids of note: 0x2202 shovel; 0x0000/0xFFFF-ish = empty.

Usage: inspect-gci.py <file.gci> [file2.gci ...]
"""
import sys

GCI_HEADER = 64
SAVE_OFFSETS = {"main": 0x26000, "backup": 0x4C000}
PRIVATE_OFF = 0x20
PRIVATE_SIZE = 0x2440
N_PLAYERS = 4
POCKETS_OFF = 0x68
N_POCKETS = 15
EQUIP_OFF = 0x4A4


def u16be(buf, off):
    return int.from_bytes(buf[off:off + 2], "big")


def dump(path):
    data = open(path, "rb").read()
    print(f"== {path} ({len(data)} bytes)")
    for name, base in SAVE_OFFSETS.items():
        for p in range(N_PLAYERS):
            priv = GCI_HEADER + base + PRIVATE_OFF + p * PRIVATE_SIZE
            pockets = [u16be(data, priv + POCKETS_OFF + 2 * i)
                       for i in range(N_POCKETS)]
            equip = u16be(data, priv + EQUIP_OFF)
            if equip == 0 and not any(pockets):
                continue  # empty player slot
            pk = " ".join(f"{v:04X}" for v in pockets)
            print(f"  {name} player{p}: equipment={equip:04X} pockets=[{pk}]")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for f in sys.argv[1:]:
        dump(f)
