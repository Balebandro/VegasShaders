"""Small helpers for tools/gen_textures.py: legacy (DX9-header) DDS reading/writing and mip chains.

Only what the generator needs: uncompressed A8R8G8B8 (32-bit) and R8G8B8 (24-bit) files with an optional
full mip chain. Block-compressed files (DXT1/DXT5) are made from an A8R8G8B8 file by Microsoft's texconv.
"""
import struct

import numpy as np

DDSD_CAPS, DDSD_HEIGHT, DDSD_WIDTH, DDSD_PITCH = 0x1, 0x2, 0x4, 0x8
DDSD_PIXELFORMAT, DDSD_MIPMAPCOUNT, DDSD_LINEARSIZE = 0x1000, 0x20000, 0x80000
DDPF_ALPHAPIXELS, DDPF_FOURCC, DDPF_RGB = 0x1, 0x4, 0x40
DDSCAPS_COMPLEX, DDSCAPS_TEXTURE, DDSCAPS_MIPMAP = 0x8, 0x1000, 0x400000


def read_header(path):
    """The fields of a legacy DDS header that matter here (size, mips, pixel format, flags, caps)."""
    with open(path, "rb") as f:
        d = f.read(148)
    if d[:4] != b"DDS ":
        raise ValueError(f"{path}: not a DDS file")
    _, flags, h, w, pitch, depth, mips = struct.unpack("<7I", d[4:32])
    _, pfflags, fourcc, bits, rm, gm, bm, am = struct.unpack("<2I4s5I", d[76:108])
    caps, caps2 = struct.unpack("<2I", d[108:116])
    return dict(width=w, height=h, depth=depth, mips=max(mips, 1), flags=flags, pitch=pitch,
                pfflags=pfflags, fourcc=fourcc.rstrip(b"\0").decode("ascii", "replace"), bits=bits,
                masks=(rm, gm, bm, am), caps=caps, caps2=caps2)


def describe(hdr):
    if hdr["fourcc"]:
        fmt = hdr["fourcc"]
    elif hdr["bits"] == 32:
        fmt = "A8R8G8B8"
    elif hdr["bits"] == 24:
        fmt = "R8G8B8"
    else:
        fmt = f"{hdr['bits']}-bit"
    return f"{hdr['width']}x{hdr['height']} {fmt} mips {hdr['mips']}"


def mip_chain(img, levels, normal=False):
    """Box-filtered mip chain of a float HxWxC image in 0..1. normal=True treats RGB as an encoded unit
    normal and renormalises each level (alpha, if any, is averaged as a plain channel)."""
    chain = [img]
    cur = img
    for _ in range(1, levels):
        h, w = cur.shape[:2]
        nh, nw = max(h // 2, 1), max(w // 2, 1)
        c = cur
        if h > 1:
            c = 0.5 * (c[0:2 * nh:2] + c[1:2 * nh:2])
        if w > 1:
            c = 0.5 * (c[:, 0:2 * nw:2] + c[:, 1:2 * nw:2])
        if normal:
            n = c[..., :3] * 2.0 - 1.0
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-6)
            c = c.copy()
            c[..., :3] = n * 0.5 + 0.5
        chain.append(c)
        cur = c
    return chain


def to_u8(a):
    return np.clip(np.rint(np.asarray(a, dtype=np.float64) * 255.0), 0, 255).astype(np.uint8)


def write_uncompressed(path, chain, bits, flags=None, caps=None):
    """Writes a legacy-header DDS. bits=32: A8R8G8B8 (bytes B,G,R,A); bits=24: R8G8B8 (bytes B,G,R).
    chain: list of float HxWx4 (or x3) arrays in 0..1, level 0 first. flags/caps default to what D3DX-era
    tools write; pass the original file's values to keep its header the same."""
    h, w = chain[0].shape[:2]
    mips = len(chain)
    bpp = bits // 8
    if flags is None:
        flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_PITCH
        if mips > 1:
            flags |= DDSD_MIPMAPCOUNT
    if caps is None:
        caps = DDSCAPS_TEXTURE | ((DDSCAPS_COMPLEX | DDSCAPS_MIPMAP) if mips > 1 else 0)
    pitch = w * h * bpp if (flags & DDSD_LINEARSIZE) else w * bpp
    if bits == 32:
        pf = struct.pack("<2I4s5I", 32, DDPF_RGB | DDPF_ALPHAPIXELS, b"\0\0\0\0", 32,
                         0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    elif bits == 24:
        pf = struct.pack("<2I4s5I", 32, DDPF_RGB, b"\0\0\0\0", 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0)
    else:
        raise ValueError("bits must be 24 or 32")
    hdr = b"DDS " + struct.pack("<7I", 124, flags, h, w, pitch, 0, mips if mips > 1 else 0)
    hdr += b"\0" * 44 + pf + struct.pack("<5I", caps, 0, 0, 0, 0)
    assert len(hdr) == 128
    body = []
    for lvl in chain:
        u = to_u8(lvl)
        if u.shape[2] == 3:
            u = np.concatenate([u, np.full(u.shape[:2] + (1,), 255, np.uint8)], axis=2)
        bgra = u[..., [2, 1, 0, 3]]
        body.append(np.ascontiguousarray(bgra[..., :bpp]).tobytes())
    with open(path, "wb") as f:
        f.write(hdr)
        for b in body:
            f.write(b)


def clear_depth(path):
    """texconv writes depth 1 for a plain 2D texture; the original files have 0 (both are valid for a 2D DDS
    without DDSD_DEPTH). Sets it back to 0 so the header matches."""
    with open(path, "r+b") as f:
        f.seek(4)
        flags = struct.unpack("<2I", f.read(8))[1]
        if not flags & 0x800000:
            f.seek(24)
            f.write(struct.pack("<I", 0))
