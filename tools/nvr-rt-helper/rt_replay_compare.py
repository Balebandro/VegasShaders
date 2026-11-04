#!/usr/bin/env python3
# rt_replay_compare.py (UNOFFICIAL 2026-11-03, claude/rt-replay): the difference between two RT helper replay outputs. MIT licence.
#
# NvrRtHelper.exe --replay writes every image a pass wrote as <name>.f32 (exact floats: "NVRF", width, height, channels as uint32,
# then the floats, rows top to bottom) next to a PNG, and the game's own output of the captured frame as game_<name>.f32.
#
#   py rt_replay_compare.py <A> <B>        A, B: two .f32 files, or two replay folders (every img*.f32 in A against B's same name)
#   py rt_replay_compare.py <folder> --game     each imgNN.f32 against game_imgNN.f32 in that folder (did the replay match the game?)
#   options: --diff <file.png> (one pair: where its diff image goes; folders: diff_<name>.png in B / the folder)
#            --scale <s>  (diff image: white = this difference; default the pair's max)   --threshold <t> (default 1e-3)
# Prints per pair: mean |diff|, max |diff|, RMS, the share of pixels whose largest channel difference is over the threshold, NaN
# mismatches. Exit code 1 when any pair differs over the threshold (so a script can use it as a test). Needs numpy.
import os
import struct
import sys
import zlib

import numpy as np


def read_f32(path):
    with open(path, "rb") as f:
        head = f.read(16)
        magic, w, h, ch = struct.unpack("<4I", head)
        if magic != 0x4652564E:
            raise ValueError(f"{path}: not an NVRF image")
        data = np.frombuffer(f.read(), dtype="<f4")
    return data[: w * h * ch].reshape(h, w, ch)


def write_png_grey(path, img8):
    h, w = img8.shape
    raw = b"".join(b"\x00" + img8[y].tobytes() for y in range(h))

    def chunk(t, body):
        c = t + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def compare(a_path, b_path, diff_path, scale, threshold):
    a, b = read_f32(a_path), read_f32(b_path)
    name = os.path.basename(b_path)
    if a.shape != b.shape:
        print(f"{name}: sizes differ {a.shape} vs {b.shape}")
        return True
    na, nb = np.isnan(a), np.isnan(b)
    nan_mismatch = int(np.count_nonzero(na != nb))
    both = ~(na | nb)
    d = np.where(both, np.abs(a.astype(np.float64) - b.astype(np.float64)), 0.0)
    d = np.where(np.isfinite(d), d, 0.0)
    values = int(np.count_nonzero(both))
    mean = float(d.sum() / max(values, 1))
    mx = float(d.max()) if d.size else 0.0
    rms = float(np.sqrt((d * d).sum() / max(values, 1)))
    per_pixel = d.max(axis=2)
    over = int(np.count_nonzero(per_pixel > threshold))
    pixels = per_pixel.size
    print(f"{name}: {a.shape[1]}x{a.shape[0]}x{a.shape[2]}  mean |diff| {mean:.4g}  max {mx:.4g}  RMS {rms:.4g}  "
          f"pixels > {threshold:g}: {over} of {pixels} ({100.0 * over / max(pixels, 1):.3f} %)  NaN mismatches {nan_mismatch}")
    if diff_path:
        s = scale if scale else (mx if mx > 0 else 1.0)
        write_png_grey(diff_path, (np.clip(per_pixel / s, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8))
        print(f"  diff image {diff_path} (white = {s:.4g})")
    return over > 0 or nan_mismatch > 0


def main(argv):
    args, diff, scale, threshold, game = [], None, None, 1e-3, False
    i = 0
    while i < len(argv):
        if argv[i] == "--diff":
            diff = argv[i + 1]; i += 2
        elif argv[i] == "--scale":
            scale = float(argv[i + 1]); i += 2
        elif argv[i] == "--threshold":
            threshold = float(argv[i + 1]); i += 2
        elif argv[i] == "--game":
            game = True; i += 1
        else:
            args.append(argv[i]); i += 1
    pairs = []
    if game and len(args) == 1 and os.path.isdir(args[0]):
        folder = args[0]
        for f in sorted(os.listdir(folder)):
            if f.startswith("img") and f.endswith(".f32") and os.path.exists(os.path.join(folder, "game_" + f)):
                pairs.append((os.path.join(folder, "game_" + f), os.path.join(folder, f), os.path.join(folder, "diff_" + f[:-4] + ".png")))
    elif len(args) == 2 and os.path.isdir(args[0]) and os.path.isdir(args[1]):
        for f in sorted(os.listdir(args[0])):
            if f.startswith("img") and f.endswith(".f32") and os.path.exists(os.path.join(args[1], f)):
                pairs.append((os.path.join(args[0], f), os.path.join(args[1], f), os.path.join(args[1], "diff_" + f[:-4] + ".png")))
    elif len(args) == 2:
        pairs.append((args[0], args[1], diff))
    else:
        print(__doc__ or "usage: rt_replay_compare.py <A> <B> | <folder> --game  [--diff x.png] [--scale s] [--threshold t]")
        return 2
    if not pairs:
        print("nothing to compare (no matching img*.f32)")
        return 2
    differs = False
    for a, b, d in pairs:
        differs = compare(a, b, d, scale, threshold) or differs
    return 1 if differs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
