#!/usr/bin/env python3
"""
TRP (trophy package) unpacker/packer for the MCSM Vita port.

WHY THIS EXISTS
---------------
PS Vita trophies need an *unencrypted, Vita-format* TROPHY.TRP. The PS4 pack that
ships with the game is a different container and cannot be used:

    field        PS4 pack            Vita pack (what we must produce)
    magic        DCA24D00            DCA24D00        (same)
    version      3                   2
    configs      *.ESFM (encrypted)  *.SFM (plain XML)
    entry table  offset 96           offset 64

The PNG icons are format-agnostic, so the PS4 pack is still useful as an *asset
source*: unpack it, reuse the artwork, and repack as v2 with plain-XML configs.

Both facts above were established by hexdumping a real PS4 pack and a known-good
shipped Vita pack, not from documentation.

USAGE
  python trp_tool.py list   <in.trp>
  python trp_tool.py unpack <in.trp> <outdir>
  python trp_tool.py pack   <indir> <out.trp>     # always writes Vita v2
  python trp_tool.py setcommid <trp> <OLD_00> <NEW_00>   # rewrite the NP comm id

The header carries a 20-byte SHA-1 INTEGRITY digest at offset 28, and it is MANDATORY.
Leaving it zero — on the theory that NoTrpDrm makes it irrelevant — gets the pack
rejected on device with "NP-6185-0 / data corrupted". NoTrpDrm disables the NP
*signature* checks; this is a plain integrity hash and the system computes it itself.
Algorithm (verified by reproducing a shipped pack's digest exactly): SHA-1 over the
whole file with the digest field zeroed. cmd_pack() fills it in.
"""
import hashlib
import os
import struct
import sys

MAGIC = 0xDCA24D00
ENTRY_SIZE = 64
HDR_SIZE = 64


def _table_offset(version):
    # v1/v2 (PS3/Vita) start the entry table immediately after the 64-byte header.
    # v3 (PS4) inserts a further 32 bytes before it -- measured, not documented.
    return 96 if version >= 3 else 64


def read_header(data):
    if len(data) < HDR_SIZE:
        raise ValueError("file too small to be a TRP")
    magic, version = struct.unpack_from(">II", data, 0)
    if magic != MAGIC:
        raise ValueError("not a TRP (magic %08X, expected %08X)" % (magic, MAGIC))
    file_size, = struct.unpack_from(">Q", data, 8)
    count, entry_size = struct.unpack_from(">II", data, 16)
    if entry_size != ENTRY_SIZE:
        raise ValueError("unexpected entry size %d" % entry_size)
    return version, file_size, count, entry_size


def iter_entries(data):
    version, file_size, count, entry_size = read_header(data)
    base = _table_offset(version)
    for i in range(count):
        off = base + i * entry_size
        raw = data[off:off + 32]
        name = raw.split(b"\0", 1)[0].decode("ascii", "replace")
        pos, size = struct.unpack_from(">QQ", data, off + 32)
        yield name, pos, size


def cmd_list(path):
    data = open(path, "rb").read()
    version, file_size, count, _ = read_header(data)
    kind = {1: "PS3", 2: "PS Vita", 3: "PS4"}.get(version, "unknown")
    print("version %d (%s)  declared size %d  actual %d  entries %d"
          % (version, kind, file_size, len(data), count))
    if version != 2:
        print("!! NOT Vita format -- unpack it and repack as v2 before use")
    for name, pos, size in iter_entries(data):
        print("  %-20s off=%-10d size=%d" % (name, pos, size))


def cmd_unpack(path, outdir):
    data = open(path, "rb").read()
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for name, pos, size in iter_entries(data):
        if not name or pos + size > len(data):
            print("  SKIP %r (offset/size outside file)" % name)
            continue
        with open(os.path.join(outdir, name), "wb") as f:
            f.write(data[pos:pos + size])
        n += 1
    print("unpacked %d entries to %s" % (n, outdir))


def _pack_order(name):
    """Order entries the way a shipped Vita pack does: TROPCONF.SFM, then TROP.SFM,
    then the remaining configs, then the images. Packing alphabetically instead put
    GR001.PNG second and TROP.SFM after the icons, which is not a layout the system has
    ever been shown to accept."""
    u = name.upper()
    if u == "TROPCONF.SFM":
        return (0, u)
    if u == "TROP.SFM":
        return (1, u)
    if u.endswith(".SFM"):
        return (2, u)
    return (3, u)


def cmd_pack(indir, outpath):
    names = sorted(os.listdir(indir), key=_pack_order)
    if not names or names[0].upper() != "TROPCONF.SFM":
        print("ERROR: %s must contain TROPCONF.SFM" % indir)
        return 1
    if any(n.upper().endswith(".ESFM") for n in names):
        print("ERROR: .ESFM present -- those are encrypted PS4 configs and the Vita")
        print("       system cannot read them. Provide plain-XML .SFM instead.")
        return 1

    blobs = [open(os.path.join(indir, n), "rb").read() for n in names]
    count = len(names)
    data_off = HDR_SIZE + count * ENTRY_SIZE

    # ☠ EVERY ENTRY MUST START ON A 16-BYTE BOUNDARY. Packing them back-to-back is what
    # kept producing "NP-6185-0 / data corrupted" even after the header digest was
    # correct: in a shipped Vita pack all 68 entries are 16-byte aligned, while 48 of
    # 57 of ours were not. The offset recorded in the table is the ALIGNED one; the
    # size stays the true byte count, and the gap is zero padding.
    ALIGN = 16
    table = bytearray()
    pos = data_off
    if pos % ALIGN:
        pos += ALIGN - (pos % ALIGN)
    layout = []
    for name, b in zip(names, blobs):
        raw = name.encode("ascii")
        if len(raw) > 31:
            print("ERROR: entry name too long (max 31): %s" % name)
            return 1
        table += raw.ljust(32, b"\0")
        table += struct.pack(">QQ", pos, len(b))
        table += b"\0" * 16
        layout.append((pos, b))
        pos += len(b)
        if pos % ALIGN:
            pos += ALIGN - (pos % ALIGN)

    total = pos
    hdr = bytearray(HDR_SIZE)
    struct.pack_into(">II", hdr, 0, MAGIC, 2)      # version 2 = PS Vita
    struct.pack_into(">Q", hdr, 8, total)
    struct.pack_into(">II", hdr, 16, count, ENTRY_SIZE)
    # bytes 24..27 dev flag, 48..63 padding stay zero. Bytes 28..47 are filled below.

    # Build by absolute offset so the padding is real zero bytes, not an accident of
    # concatenation order.
    blob = bytearray(total)
    blob[0:HDR_SIZE] = hdr
    blob[HDR_SIZE:HDR_SIZE + len(table)] = table
    for off, b in layout:
        blob[off:off + len(b)] = b

    # ☠ THE INTEGRITY DIGEST IS MANDATORY. Leaving bytes 28..47 zero produced
    # "Данные повреждены / NP-6185-0" (data corrupted) on device: the system computes
    # this hash itself and compares. NoTrpDrm disables the NP *signature* checks, NOT
    # this — it is a plain integrity digest, not DRM.
    #
    # Algorithm, verified against a shipped Vita pack: SHA-1 over the ENTIRE FILE with
    # the digest field itself zeroed. Reproducing that pack's stored digest exactly
    # (e178b60b…c5f4) is what confirms it, rather than guessing among the plausible
    # variants (payload-only and after-header both mismatched).
    digest = hashlib.sha1(bytes(blob)).digest()
    blob[28:48] = digest

    with open(outpath, "wb") as f:
        f.write(blob)
    print("wrote %s: Vita TRP v2, %d entries, %d bytes, sha1=%s"
          % (outpath, count, total, digest.hex()[:16] + "..."))
    return 0


def cmd_setcommid(path, old, new):
    """Rewrite the NP communication id inside an existing pack, in place.

    The id appears in the <npcommid> element of both SFM configs. It has to agree
    with the folder the pack is installed into (sce_sys/trophy/<COMMID>_00/) and with
    the id the loader passes to sceNpTrophyCreateContext -- three places that are easy
    to let drift apart, and any disagreement simply means no trophies.

    Deliberately a byte-for-byte substitution rather than a repack: the entry layout
    (16-byte alignment, entry order, offsets) of this pack has been validated on
    device, so the only thing that should change is the id. The two ids must therefore
    be the same length, which they are by construction (`XXXXNNNNN_00`). Only the
    header digest has to be recomputed afterwards.
    """
    old_b, new_b = old.encode(), new.encode()
    if len(old_b) != len(new_b):
        print("comm ids must be the same length (%r vs %r)" % (old, new))
        return 2
    with open(path, "rb") as f:
        blob = bytearray(f.read())

    hits = blob.count(old_b)
    if hits == 0:
        print("%s does not contain %s — nothing to do" % (path, old))
        return 2
    blob[:] = blob.replace(old_b, new_b)

    blob[28:48] = b"\0" * 20
    digest = hashlib.sha1(bytes(blob)).digest()
    blob[28:48] = digest
    with open(path, "wb") as f:
        f.write(blob)
    print("%s: %s -> %s (%d occurrences), sha1=%s"
          % (path, old, new, hits, digest.hex()))
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "list":
        cmd_list(sys.argv[2])
    elif cmd == "unpack" and len(sys.argv) >= 4:
        cmd_unpack(sys.argv[2], sys.argv[3])
    elif cmd == "pack" and len(sys.argv) >= 4:
        return cmd_pack(sys.argv[2], sys.argv[3])
    elif cmd == "setcommid" and len(sys.argv) >= 5:
        return cmd_setcommid(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
