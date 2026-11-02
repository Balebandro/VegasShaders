#!/usr/bin/env python3
"""
survey.py: try every pass of every NVR effect through hlsl2spv.py (into a scratch folder, nothing shipped) and count which compile
to SPIR-V and which the generic EffectRecord::Render hook could draw (FrameVS, no render states). UNOFFICIAL 2026-10-26.
  python3 tools/nvr-sm6/survey.py [scratch dir]      (default /tmp/nvr-sm6-survey)
"""
import os, re, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
EFFECTS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "src", "hlsl", "NewVegas", "Effects")
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nvr-sm6-survey"
ok, failed, plain, reasons = 0, 0, 0, collections.Counter()
for f in sorted(os.listdir(EFFECTS)):
    if not f.endswith(".fx.hlsl"):
        continue
    text = open(os.path.join(EFFECTS, f), "rb").read().decode("latin-1")
    text = re.sub(r"//[^\n]*", "", text)
    for tm in re.finditer(r"\btechnique\w*\s*(\w*)\s*\{", text):
        name = tm.group(1) or "default"
        start = tm.end()
        depth, i = 1, start
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        npass = len(re.findall(r"\bpass\b", text[start:i]))
        for p in range(npass):
            r = subprocess.run([sys.executable, os.path.join(HERE, "hlsl2spv.py"), os.path.join(EFFECTS, f), name, str(p), "--out", OUT,
                                "-D", "PHYSICAL_SKY_ON=1", "-D", "FORWARD_SHADOWS=0", "-D", "REVERSED_DEPTH="], capture_output=True, text=True)
            tag = "%s %s %d" % (f.split(".")[0], name, p)
            if r.returncode == 0:
                ok += 1
                lay = os.path.join(OUT, "%s.%s.%d.txt" % (f.split(".")[0], name, p))
                body = open(lay).read() if os.path.exists(lay) else ""
                if "vs FrameVS" in body and "states 0" in body:
                    plain += 1
                print("OK   " + tag)
            else:
                failed += 1
                err = [l for l in r.stderr.splitlines() if "ERROR" in l or "error" in l.lower()]
                why = err[1] if len(err) > 1 else (err[0] if err else "?")
                reasons[re.sub(r"\d+", "N", why)[:110]] += 1
                print("FAIL " + tag + ": " + why[:160])
print("\n%d passes compile, %d fail; %d of the compiled are plain (FrameVS, no render states)" % (ok, failed, plain))
for k, v in reasons.most_common(12):
    print("%4d  %s" % (v, k))
