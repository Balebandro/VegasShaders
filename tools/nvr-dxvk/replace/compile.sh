#!/usr/bin/env bash
# compile.sh <file.frag>...: GLSL replacement -> <file>.spv (glslangValidator, Vulkan 1.1) + spirv-val. Runs in Linux/WSL.
# Tools: glslang-tools and spirv-tools on PATH, or unpacked into ~/vktools/root (apt-get download + dpkg-deb -x, no root needed).
set -euo pipefail
export PATH="$HOME/vktools/root/usr/bin:$PATH"
for f in "$@"; do
  out="${f%.frag}.spv"
  glslangValidator -V --target-env vulkan1.1 -o "$out" "$f" > /tmp/glslang.$$.log || { cat /tmp/glslang.$$.log; exit 1; }
  spirv-val --target-env vulkan1.1 --scalar-block-layout "$out"
  echo "ok: $out ($(stat -c %s "$out") bytes)"
done
