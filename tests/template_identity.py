#!/usr/bin/env python3
"""UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md: "off = the 222 templates byte-identical"): compiles every template record
twice with the game's D3DX (tests/template_compile.exe --batch), once from a base tree's Shaders folder and once from this tree's, with the
define list the game would use (tests/template_defines.py, the release defaults or a user toml, every new switch off), and compares the
bytecode byte for byte. Records: the default ones and the interior lamp-shadow records (the FIT step-downs are not compiled: same sources).

Usage: template_identity.py --exe build/template-compile/template_compile.exe --base <base>/src/hlsl/NewVegas/Shaders --new src/hlsl/NewVegas/Shaders
                            --out <scratch dir> [--toml FILE]... [--set K=V]... [--runtime native|dxvk] [--rt 0|1] [--records default,interior]
                            [--names SLS2035.pso ...]
Prints: identical / different / failed counts, the names that differ, and the slot count of every record from the new tree.
"""
import argparse, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import template_defines as td

def run_batch(exe, batch, log):
    with open(log, "w", encoding="utf-8") as f:
        p = subprocess.run([exe, "--flow", "--batch", batch], stdout=f, stderr=subprocess.STDOUT, text=True)
    results = {}
    for line in open(log, encoding="utf-8", errors="replace"):
        parts = line.split()
        if len(parts) >= 2 and "/" in parts[0]: results[parts[0]] = line.strip()
    return results

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True); ap.add_argument("--base", required=True); ap.add_argument("--new", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--toml", action="append", default=[]); ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--runtime", default="native"); ap.add_argument("--rt", type=int, default=0); ap.add_argument("--records", default="default,interior")
    ap.add_argument("--names", nargs="*", default=[])
    a = ap.parse_args(); a.exe = os.path.abspath(a.exe)
    os.makedirs(a.out, exist_ok=True)
    common = ["--runtime", a.runtime, "--rt", str(a.rt), "--records", a.records] + sum((["--toml", t] for t in a.toml), []) + sum((["--set", s] for s in a.set), [])
    here = os.path.dirname(os.path.abspath(__file__))
    for side, shaders in (("base", a.base), ("new", a.new)):
        batch = os.path.join(a.out, side + ".batch.txt")
        subprocess.check_call([sys.executable, "-I", os.path.join(here, "template_defines.py"), "--batch", batch, "--shaders", shaders, "--out-dir", os.path.join(a.out, side)] + common + a.names)
    base = run_batch(a.exe, os.path.join(a.out, "base.batch.txt"), os.path.join(a.out, "base.log"))
    new = run_batch(a.exe, os.path.join(a.out, "new.batch.txt"), os.path.join(a.out, "new.log"))
    same = diff = failed = 0; differing = []; slots = []
    for rid in sorted(new):
        kind, name = rid.split("/")
        nb = os.path.join(a.out, "new", kind, name + ".bin"); bb = os.path.join(a.out, "base", kind, name + ".bin")
        if "FAILED" in new[rid] or "FAILED" in base.get(rid, "FAILED"): failed += 1; differing.append(rid + " (compile failed: new: %s | base: %s)" % (new[rid], base.get(rid))); continue
        if open(nb, "rb").read() == open(bb, "rb").read(): same += 1
        else: diff += 1; differing.append(rid)
        slots.append((rid, new[rid]))
    print("IDENTITY: %d identical, %d different, %d failed (of %d records; settings: %s, runtime %s, rt %d)" % (same, diff, failed, len(new), a.toml or "release defaults", a.runtime, a.rt))
    for d in differing: print("  DIFFERENT " + d)
    with open(os.path.join(a.out, "slots.txt"), "w", encoding="utf-8") as f:
        for rid, line in slots: f.write(line + "\n")
    print("slot counts of the new tree in", os.path.join(a.out, "slots.txt"))

if __name__ == "__main__": main()
