"""mixcheck_diff.py <VegasShaders_*.log> [bad mode substring] [good mode substring]

UNOFFICIAL 2026-10-30, for [Main.Main.Performance] NvrDxvkMixCheck (src/core/MixCheck.h). A normal frame already logs many MIXCHECK lines
(cached shadow cubes and atlas cascades, ping-pong history buffers, the first frames after a load): the broken edge is what one mode logs and
the others do not. Each MIXCHECK line ends with its mode, e.g. [passes Vulkan, keep "CombineDepth,Normals"]. This prints, per mode, the
(image, reader, kind) lines no other mode has (the reader's D3D9 / Vulkan path ignored, so a kept effect matches itself), and the setup line.
With a bad and a good mode substring given, only bad-minus-good. Run the game once with the failing NvrDxvkKeepD3D9 list, switch the list to
empty in the menu (or launch again with it empty), and give this the log.
"""
import re
import sys
from collections import defaultdict

LINE = re.compile(r"MIXCHECK (EMPTY|STALE|EDGE |intermittent) (.+?) read by (.+?) \((D3D9|Vulkan)\)(.*?)(?:\[(.+)\])?\s*$")


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    bad = sys.argv[2] if len(sys.argv) > 2 else None
    good = sys.argv[3] if len(sys.argv) > 3 else None
    modes = defaultdict(dict)   # mode -> {(kind, image, reader): text}
    last_mode = "?"
    for raw in open(path, encoding="utf-8", errors="replace"):
        if "NVR DXVK setup" in raw:
            print(raw.rstrip())
        m = LINE.search(raw)
        if not m:
            continue
        kind, image, reader, rpath, rest, mode = m.groups()
        kind = kind.strip()
        mode = mode or last_mode   # intermittent lines carry no mode: the mode before them
        last_mode = mode
        image = re.sub(r"image [0-9A-Fa-f]{8}", "image ?", image)   # unnamed images: addresses differ between runs
        modes[mode][(kind, image, reader)] = "%s %s read by %s (%s)%s" % (kind, image, reader, rpath, rest)
    names = list(modes)
    print("modes: " + "; ".join("%s (%d lines)" % (n, len(modes[n])) for n in names))
    if bad:
        bm = [n for n in names if bad in n]
        gm = [n for n in names if good and good in n] if good else [n for n in names if n not in bm]
        others = set().union(*[set(modes[n]) for n in gm]) if gm else set()
        for n in bm:
            only = [modes[n][k] for k in modes[n] if k not in others]
            print("\n== only in [%s] (%d of %d), against %s" % (n, len(only), len(modes[n]), gm))
            for line in sorted(only):
                print("  " + line)
        return
    for n in names:
        others = set().union(*[set(modes[o]) for o in names if o != n]) if len(names) > 1 else set()
        only = [modes[n][k] for k in modes[n] if k not in others]
        print("\n== only in [%s] (%d of %d)" % (n, len(only), len(modes[n])))
        for line in sorted(only):
            print("  " + line)


if __name__ == "__main__":
    main()
