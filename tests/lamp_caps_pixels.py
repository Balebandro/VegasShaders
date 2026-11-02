#!/usr/bin/env python3
"""UNOFFICIAL 2026-10-31 (docs/next/DEFERRED-RENDERING-PLAN.md stage 0b, LampCapsCensus view 4): the share of a shot's pixels that a lamp with no
shadow source lights (painted red by Includes/PointShadowForward.hlsl's NVR_CapsUnshadowed through [Shaders.ShadowsInteriors.Forward] DebugView
4). The view writes the base pass black or red and the game's added passes add red on top, then the post chain runs, so the test is a red pixel
well above its green and blue: R > 60/255 and R > 2 x max(G, B).

Usage: lamp_caps_pixels.py <shot.png> [more...]   prints  <file>: red N of M pixels (P %)
"""
import sys
from PIL import Image
import numpy as np

for path in sys.argv[1:]:
    im = np.asarray(Image.open(path).convert("RGB")).astype(np.int32)
    r, g, b = im[..., 0], im[..., 1], im[..., 2]
    red = (r > 60) & (r > 2 * np.maximum(g, b))
    n = int(red.sum()); total = red.size
    print("%s: red %d of %d pixels (%.2f %%)" % (path, n, total, 100.0 * n / total))
