#!/usr/bin/env python3
"""
Build a PS Vita TROPHY.TRP for the MCSM port.

PIPELINE
    1. unpack the game's PS4 trophy pack  -> recovers 51 trophy icons + ICON0/GR00n
    2. generate TROPCONF.SFM and TROP.SFM from trophies.def (plain XML, Vita schema)
    3. pack everything as a Vita TRP v2 via trp_tool.py

The schema below was taken from a shipped, working Vita trophy pack, including the
detail that the Sce-Np-Trophy-Signature comment is a run of 'x' characters: homebrew
cannot produce a real signature, and NoTrpDrm is what makes the system accept it.

    python make_trophy_pack.py <icons_dir> <trophies.def> <out.trp>

trophies.def format ('#' comments allowed), one trophy per line:

    npcommid = MCSM00001_00
    title    = Minecraft: Story Mode
    detail   = A Telltale Games Series
    # id | grade | name | detail
    000 | P | Platinum Name | Earn every other trophy.
    001 | B | Some Trophy   | How you earn it.

grade is P(latinum) / G(old) / S(ilver) / B(ronze). Exactly one P is allowed and it
must be id 000; every other trophy hangs off it via pid="000", which is what makes the
platinum unlock automatically when the rest are done.
"""
import os
import subprocess
import sys

# ☠ EXACTLY 320 CHARACTERS. This placeholder stands in for the 160-byte NP
# communication signature, hex-encoded -- 160 * 2 = 320 -- so it is a FIXED-WIDTH
# field, not free padding. A shipped Vita pack has exactly 320; this file had 328,
# and the system rejected the pack with NP-6185-0 even after the container itself
# (header digest, 16-byte entry alignment, entry order) was byte-for-byte correct.
SIG = "x" * 320


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
             .replace('"', "&quot;"))


def parse_def(path):
    meta = {"npcommid": "MCSM00001_00", "title": "Minecraft: Story Mode", "detail": ""}
    rows = []
    for raw in open(path, encoding="utf-8"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "|" not in line and "=" in line:
            k, v = line.split("=", 1)
            meta[k.strip().lower()] = v.strip()
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) < 3:
            continue
        tid, grade, name = parts[0], parts[1].upper(), parts[2]
        detail = parts[3] if len(parts) > 3 else ""
        rows.append((tid.zfill(3), grade, name, detail))
    return meta, rows


def build_sfm(meta, rows, with_text):
    out = ["<!--Sce-Np-Trophy-Signature: %s-->" % SIG,
           '<trophyconf version="1.1" platform="psp2" policy="large">',
           " <npcommid>%s</npcommid>" % esc(meta["npcommid"]),
           " <trophyset-version>01.00</trophyset-version>",
           ' <parental-level license-area="default">0</parental-level>']
    if with_text:
        out.append(" <title-name>%s</title-name>" % esc(meta["title"]))
        out.append(" <title-detail>%s</title-detail>" % esc(meta.get("detail", "")))
    for tid, grade, name, detail in rows:
        pid = "-1" if grade == "P" else "000"
        if with_text:
            out.append(' <trophy id="%s" hidden="no" ttype="%s" pid="%s">' % (tid, grade, pid))
            out.append("  <name>%s</name>" % esc(name))
            out.append("  <detail>%s</detail>" % esc(detail))
            out.append(" </trophy>")
        else:
            out.append(' <trophy id="%s" hidden="no" ttype="%s" pid="%s"/>' % (tid, grade, pid))
    out.append("</trophyconf>")
    return ("\n".join(out) + "\n").encode("utf-8")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    icons_dir, def_path, out_trp = sys.argv[1], sys.argv[2], sys.argv[3]
    meta, rows = parse_def(def_path)
    if not rows:
        print("ERROR: no trophies parsed from %s" % def_path)
        return 1
    plats = [r for r in rows if r[1] == "P"]
    if len(plats) > 1:
        print("ERROR: more than one platinum defined")
        return 1

    stage = out_trp + ".stage"
    os.makedirs(stage, exist_ok=True)
    for f in os.listdir(stage):
        os.remove(os.path.join(stage, f))

    # Configs. TROPCONF is the structural manifest; TROP.SFM carries the visible text.
    open(os.path.join(stage, "TROPCONF.SFM"), "wb").write(build_sfm(meta, rows, False))
    open(os.path.join(stage, "TROP.SFM"), "wb").write(build_sfm(meta, rows, True))

    # Icons, straight from the PS4 pack -- PNG is format-agnostic.
    missing = []
    wanted = ["ICON0.PNG"] + ["TROP%03d.PNG" % i for i in range(len(rows))]
    for name in wanted:
        src = os.path.join(icons_dir, name)
        if os.path.exists(src):
            open(os.path.join(stage, name), "wb").write(open(src, "rb").read())
        else:
            missing.append(name)
    # ☠ GROUP ICONS ARE DELIBERATELY NOT COPIED. The PS4 pack carries GR001..GR003.PNG
    # for its trophy GROUPS, but the TROPCONF we generate declares no <group> at all --
    # every trophy hangs directly off the platinum. Shipping group art that nothing
    # references is not merely redundant: those files are 320x176, the ICON0 size,
    # whereas every trophy icon in a working Vita pack is 240x240. That left our pack
    # with four 320x176 entries where a shipped pack has exactly one, and the trophy
    # list rendered without artwork. If groups are ever declared, add the icons back
    # AND resize them to 240x240 to match what the system expects.
    if missing:
        print("WARNING: %d icon(s) missing, pack will still build: %s"
              % (len(missing), ", ".join(missing[:5])))

    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "trp_tool.py")
    rc = subprocess.call([sys.executable, tool, "pack", stage, out_trp])
    if rc == 0:
        print("trophies: %d (%d platinum)  npcommid: %s"
              % (len(rows), len(plats), meta["npcommid"]))
    return rc


if __name__ == "__main__":
    sys.exit(main())
