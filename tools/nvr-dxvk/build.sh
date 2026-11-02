#!/usr/bin/env bash
# NVR DXVK: DXVK v3.0.1 + the NVR extension (patches/), built as the 32-bit d3d9.dll Fallout: New Vegas loads.
#
# Runs on Linux or WSL (Ubuntu 24.04 was used). Needs: git, python3 + pip (meson), ninja, the 32-bit MinGW-w64
# cross compiler and glslang:
#   sudo apt install git ninja-build python3-pip g++-mingw-w64-i686 gcc-mingw-w64-i686 binutils-mingw-w64-i686 mingw-w64-i686-dev glslang-tools
#   pip3 install --user meson      (meson >= 1.4; Ubuntu's own package is too old for DXVK 3.0)
#
# Usage: tools/nvr-dxvk/build.sh [work dir]      (default: ./nvr-dxvk-build next to the NVR repository)
# Result: <work dir>/out/x32/d3d9.dll  (+ the stock build's dxvk.conf next to it, for reference)
#
# The same steps run in .github/workflows/nvr-dxvk.yml (Actions tab -> "NVR DXVK d3d9.dll" -> artifact).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${1:-$(cd "$HERE/../.." && pwd)/../nvr-dxvk-build}"
mkdir -p "$WORK"
WORK="$(cd "$WORK" && pwd)"
SRC="$WORK/dxvk"
BUILD="$WORK/build.32"
OUT="$WORK/out/x32"
export PATH="$HOME/.local/bin:$PATH"

echo "== NVR DXVK: work dir $WORK"

if [ ! -d "$SRC/.git" ]; then
  echo "== cloning DXVK v3.0.1 (with submodules)"
  git clone --branch v3.0.1 --recurse-submodules https://github.com/doitsujin/dxvk.git "$SRC"
fi

cd "$SRC"
git checkout -q v3.0.1
git submodule update --init --recursive -q
git branch -D nvr-ext >/dev/null 2>&1 || true
git checkout -q -b nvr-ext
echo "== applying the NVR patches"
git -c user.name=nvr -c user.email=nvr@localhost am --3way "$HERE"/patches/*.patch
git tag -f v3.0.1-nvr1 >/dev/null   # DXVK's version string (log, HUD) becomes v3.0.1-nvr1
git describe --tags

echo "== building the 32-bit d3d9.dll (d3d9 only; dxgi/d3d8/d3d10/d3d11 are not needed by the game)"
rm -rf "$BUILD"
# NVR_DXVK_DEBUG=1: the same -O2 code with its debug info kept (crash addresses -> source lines with addr2line), not stripped.
if [ "${NVR_DXVK_DEBUG:-0}" = 1 ]; then BUILDOPTS="--buildtype debugoptimized -Db_ndebug=true"; else BUILDOPTS="--buildtype release --strip -Db_ndebug=if-release"; fi
meson setup --cross-file build-win32.txt $BUILDOPTS \
  -Denable_dxgi=false -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false "$BUILD"
ninja -C "$BUILD" src/d3d9/d3d9.dll

mkdir -p "$OUT"
cp "$BUILD/src/d3d9/d3d9.dll" "$OUT/d3d9.dll"
cp "$SRC/dxvk.conf" "$OUT/dxvk.conf.reference"
echo "== done: $OUT/d3d9.dll"
ls -la "$OUT"
