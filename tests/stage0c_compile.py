#!/usr/bin/env python3
"""UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0c): the Interior SLS2034 / 2035 / 2036 records (the shiny many-lamp
family that carries the slot problem) compiled offline exactly as the game would compile them with the lamp TABLE (MANY_LAMPS_ON 2, i.e.
ManyLampsOnePass + LightCommonLamps), the atlas, HandBlood, POINT_SHADOW_RT on and off, against native's ~3,600-slot cap: D3DX slot counts,
compile seconds, define counts, and the DEVICE's own answer (CreatePixelShader on the GTX 1070: the game's 'device refused it' case). The
plain records of the same settings (MANY_LAMPS_ON 0: the release default on native) and the constant-block one-pass records (MANY_LAMPS_ON 1,
the user's fork today) are compiled beside them.

Usage: stage0c_compile.py --exe build/template-compile/template_compile.exe --shaders src/hlsl/NewVegas/Shaders --out <dir> [--toml FILE]...
"""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import template_defines as td

VARIANTS = [
    # id suffix, forces, drops, (runtime, rt)
    ("ml0-release",       ["MANY_LAMPS_ON=0"],                                                        ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
    ("ml0-atlas",         ["MANY_LAMPS_ON=0", "POINT_SHADOW_ATLAS=1"],                               ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
    ("ml1-fork",          ["MANY_LAMPS_ON=1", "NO_SLOT_LIMIT=1"],                                    [], 0),
    ("ml2-table",         ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0"],                                    ["POINT_SHADOW_RT"], 0),
    ("ml2-table-atlas",   ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_ATLAS=1"],            ["POINT_SHADOW_RT"], 0),
    ("ml2-table-atlas-hb",["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_ATLAS=1", "HAND_BLOOD=1"], ["POINT_SHADOW_RT"], 0),
    ("ml2-table-atlas-hb-rt", ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_ATLAS=1", "HAND_BLOOD=1", "POINT_SHADOW_RT=1"], [], 1),
    ("ml2-table-hb-rt",   ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "HAND_BLOOD=1", "POINT_SHADOW_RT=1"], [], 1),
    ("ml2-table-atlas-hb-rt-fit1", ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_ATLAS=1", "HAND_BLOOD=1", "POINT_SHADOW_RT=1", "POINT_SHADOW_FIT=1"], [], 1),
    ("ml2-table-atlas-hb-rt-fit3", ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_ATLAS=1", "HAND_BLOOD=1", "POINT_SHADOW_RT=1", "POINT_SHADOW_FIT=3"], [], 1),
    ("ml2-table-fit3",     ["MANY_LAMPS_ON=2", "NO_SLOT_LIMIT=0", "POINT_SHADOW_FIT=3"],                 ["POINT_SHADOW_RT"], 0),
    # the atlas alone on native (release default MANY_LAMPS_ON 0): which FIT level the six-lamp records fall to
    ("ml0-atlas-fit1",     ["MANY_LAMPS_ON=0", "POINT_SHADOW_ATLAS=1", "POINT_SHADOW_FIT=1"],            ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
    ("ml0-atlas-fit2",     ["MANY_LAMPS_ON=0", "POINT_SHADOW_ATLAS=1", "POINT_SHADOW_FIT=2"],            ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
    ("ml0-atlas-fit3",     ["MANY_LAMPS_ON=0", "POINT_SHADOW_ATLAS=1", "POINT_SHADOW_FIT=3"],            ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
    ("ml0-release-fit1",   ["MANY_LAMPS_ON=0", "POINT_SHADOW_FIT=1"],                                    ["POINT_SHADOW_RT", "NO_SLOT_LIMIT"], 0),
]
NAMES = ["SLS2034.pso", "SLS2035.pso", "SLS2036.pso"]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True); ap.add_argument("--shaders", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--toml", action="append", default=[]); ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--no-device", action="store_true")
    a = ap.parse_args(); a.exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)
    s = td.Settings(a.toml, a.set)
    by_name = {r[0]: r for r in td.records()}
    class Opt: pass
    lines = []
    for suffix, forces, drops, rt in VARIANTS:
        opt = Opt(); opt.reversed_depth = 1; opt.rt = rt; opt.runtime = "native"; opt.native_manylamps = True; opt.dev = False; opt.displace_objects = False
        for name in NAMES:
            _, template, pairs, _ = by_name[name]
            d = td.apply_forces(td.build(s, pairs, td.overrides_for("interior", s), opt), forces, drops)
            lines.append("\t".join(["%s/%s" % (suffix, name), "ps_3_0", os.path.join(a.shaders, template + ".hlsl"), "-", td.define_words(d)]))
    batch = os.path.join(a.out, "stage0c.batch.txt")
    open(batch, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    cmd = [a.exe, "--flow", "--batch", batch] + ([] if a.no_device else ["--device"])
    log = os.path.join(a.out, "stage0c.log")
    with open(log, "w", encoding="utf-8") as f: subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, text=True)
    print(open(log, encoding="utf-8", errors="replace").read())
    print("settings:", a.toml or "release defaults", "| log:", log)

if __name__ == "__main__": main()
