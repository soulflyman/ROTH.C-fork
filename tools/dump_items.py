#!/usr/bin/env python3
"""List every DBASE100 inventory item with its category, flags and display name.

Offline helper (no game run needed): mirrors the engine's own lookups in
roth_c/src/engine/inventory.c (build_inventory_entry_list, format_inventory_item_label):
  table = hdr[0x14] - 4 ; entry for 1-based id = u32 at table + id*4 (0 = absent) ; rec = base + entry
  rec+4  : flags dword, category = (flags >> 8) & 0xf, 0x10 = weapon, 0x80 = not pickable
  rec+0x10: voice offset into DBASE400.DAT -> 8-byte header {u32 clip, u16 len, u8 color} + text
The engine may override the name via resolve_record_conditional_op2(rec); this tool ignores that.

usage: dump_items.py <game-dir> [--by-category]
"""
import struct
import sys
from pathlib import Path


def find(d, name):
    for p in d.iterdir():
        if p.name.upper() == name:
            return p
    raise SystemExit(f"{name} not found in {d}")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    game = Path(args[0])
    by_cat = "--by-category" in sys.argv
    db = find(game, "DBASE100.DAT").read_bytes()
    txt = find(game, "DBASE400.DAT").read_bytes()

    count = struct.unpack_from("<I", db, 0x10)[0]
    table = struct.unpack_from("<I", db, 0x14)[0] - 4

    def name_of(voice_off):
        if voice_off == 0 or voice_off + 8 > len(txt):
            return ""
        _clip, ln, _col = struct.unpack_from("<IHB", txt, voice_off)
        return txt[voice_off + 8: voice_off + 8 + min(ln, 0x61)].split(b"\0")[0].decode("latin-1")

    rows = []
    for item_id in range(1, count + 1):
        off = struct.unpack_from("<I", db, table + item_id * 4)[0]
        if off == 0:
            continue
        flags = struct.unpack_from("<I", db, off + 4)[0]
        voice = struct.unpack_from("<I", db, off + 0x10)[0]
        rows.append({
            "id": item_id,
            "cat": (flags >> 8) & 0xF,
            "weapon": bool(flags & 0x10),
            "nopick": bool(flags & 0x80),
            "stack": bool((flags >> 8) & 0x20 and False) or bool(db[off + 5] & 0x20),
            "name": name_of(voice),
        })

    if by_cat:
        for cat in sorted({r["cat"] for r in rows}):
            print(f"== category {cat} ==")
            for r in (x for x in rows if x["cat"] == cat):
                tags = ("W" if r["weapon"] else "-") + ("S" if r["stack"] else "-") + ("N" if r["nopick"] else "-")
                print(f"  {r['id']:4d} {tags} {r['name']}")
    else:
        for r in rows:
            tags = ("W" if r["weapon"] else "-") + ("S" if r["stack"] else "-") + ("N" if r["nopick"] else "-")
            print(f"{r['id']:4d} cat={r['cat']} {tags} {r['name']}")


if __name__ == "__main__":
    main()
