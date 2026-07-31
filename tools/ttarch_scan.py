#!/usr/bin/env python3
"""
Scan Telltale .ttarch2 archives for achievement name strings.

WHY: the engine's Lua calls PlatformUnlockAchievement("<name>") and those names are
the ONLY thing standing between this port and working trophies. They are not in the
APK (Android's achievement support is an unimplemented stub) and not in any loose file
-- they live inside the packed archives.

CONTAINER FORMAT (worked out by hexdump, not documented anywhere):
    magic 'ZCTT'  -> compressed:   RAW DEFLATE (zlib wbits=-15) starting at offset 0x24
    magic 'NCTT'  -> uncompressed: payload starts at offset 0x24
  The decompressed payload begins 'TTA4' = TTARCH version 4, which carries a plain
  filename table. Compressed archives are deflate BLOCKS, so the stream is consumed
  repeatedly until exhausted.

Lua inside is compiled bytecode, but string CONSTANTS survive compilation, so a plain
byte scan of the decompressed payload finds the achievement names without needing a
Lua decompiler.

    python ttarch_scan.py <assets_dir> [--dump-strings]
"""
import os
import re
import struct
import sys
import zlib

MAGIC_COMPRESSED = b"ZCTT"
MAGIC_PLAIN = b"NCTT"
PAYLOAD_OFFSET = 0x24


def decompress(path, cap_bytes=64 * 1024 * 1024):
    """Return the decompressed payload, or None if this is not a container we know.

    ☠ THE CONTAINER IS BLOCK-BASED. An earlier version of this function assumed the
    payload was one deflate stream starting at a fixed 0x24 and just fed the rest of
    the file to zlib. That silently returned only the FIRST 64KB block of every
    archive -- and an EMPTY result for any archive whose first block did not decode --
    which made a "sweep of all 90 archives" a sweep of one block each. Layout,
    established by hexdump:

        off 0   magic 'ZCTT' (compressed) or 'NCTT' (plain)
        off 4   version
        off 8   u32 LE block count N
        off 12  (N+1) x u64 LE absolute offsets; entry i..i+1 delimits block i,
                so offsets[0] is where the data starts and offsets[N] is EOF

    Each block is an INDEPENDENT raw-deflate stream (zlib wbits=-15). cap_bytes still
    bounds the expansion of the huge voice/mesh packs.
    """
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 16:
        return None
    magic = data[:4]
    if magic == MAGIC_PLAIN:
        return data[PAYLOAD_OFFSET:PAYLOAD_OFFSET + cap_bytes]
    if magic != MAGIC_COMPRESSED:
        return None

    nblocks = struct.unpack_from("<I", data, 8)[0]
    if nblocks == 0 or nblocks > 1_000_000:
        return b""
    table_end = 12 + (nblocks + 1) * 8
    if table_end > len(data):
        return b""
    offsets = [struct.unpack_from("<Q", data, 12 + i * 8)[0] for i in range(nblocks + 1)]

    out = bytearray()
    for i in range(nblocks):
        a, b = offsets[i], offsets[i + 1]
        if not (0 < a <= b <= len(data)):
            continue
        blob = data[a:b]
        try:
            out += zlib.decompressobj(-15).decompress(blob)
        except zlib.error:
            # A block that will not decode is skipped rather than aborting the whole
            # archive: one bad block must not hide the other eighty-six.
            continue
        if len(out) >= cap_bytes:
            break
    return bytes(out)


# The Lua call site is PlatformUnlockAchievement("NAME"). In compiled bytecode the
# function name and the literal both land in the constant table, so both are findable.
NEEDLES = [b"PlatformUnlockAchievement", b"UnlockAchievement", b"Achievement"]
STR_RE = re.compile(rb"[ -~]{4,64}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    dump = "--dump-strings" in sys.argv

    files = sorted(f for f in os.listdir(root) if f.lower().endswith(".ttarch2"))
    print("scanning %d archives in %s" % (len(files), root))
    hits = 0
    for name in files:
        path = os.path.join(root, name)
        try:
            data = decompress(path)
        except Exception as e:
            print("  %-56s ERROR %s" % (name, str(e)[:40]))
            continue
        if not data:
            continue
        found = [n for n in NEEDLES if n in data]
        if not found:
            continue
        hits += 1
        print("\n=== %s  (%d bytes decompressed) ===" % (name, len(data)))
        print("    contains: %s" % ", ".join(n.decode() for n in found))
        if dump:
            # Print printable strings near each occurrence -- the achievement literal
            # sits adjacent to the call name in the constant table.
            for needle in found:
                start = 0
                shown = 0
                while shown < 60:
                    i = data.find(needle, start)
                    if i < 0:
                        break
                    window = data[max(0, i - 400): i + 400]
                    for s in STR_RE.findall(window):
                        t = s.decode("ascii", "replace")
                        if len(t) >= 4 and not t.startswith("Platform"):
                            print("      %s" % t)
                            shown += 1
                            if shown >= 60:
                                break
                    start = i + 1
    if not hits:
        print("\nno archive contained an achievement string")
    return 0


if __name__ == "__main__":
    sys.exit(main())
