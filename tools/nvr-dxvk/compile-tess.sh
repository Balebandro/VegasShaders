#!/usr/bin/env bash
# compile-tess.sh (UNOFFICIAL 2026-10-27, fork patch 0021 / extension 1.7-displace): the tessellation displacement shaders in
# src/hlsl/NewVegas/Compute/Tess as SPIR-V, for [Main.Main.Performance] NvrDxvkDisplacement (docs/next/FORK2-DISPLACE.md).
#   TerrainDisplace.tesc                    -> TerrainDisplace.tcs.spv
#   TerrainDisplace.tese x TEX_COUNT 1-7    -> TerrainDisplace-t<N>.tes.spv (cw: the game's winding) and -t<N>-ccw.tes.spv (test)
#   TerrainDisplace.tese x TEX_COUNT 1-6, OBJECT_DEPTH -> TerrainDisplace-t<N>-d.tes.spv (+ -d-ccw): ObjectBlend's variants, which
#                                           also read the scene's depth at the pixel shader's s6 (2026-10-27; 7 layers leave no slot)
#   TerrainDisplace.tese x TEX_COUNT 1-7, SHADOW_CASTER -> TerrainDisplace-t<N>-s.tes.spv (+ -s-ccw), and 1-6 with OBJECT_DEPTH too
#                                           -> -t<N>-sd.tes.spv (+ -sd-ccw): the land drawn into the sun's shadow cascades
#                                           ([Shaders.Terrain.Displacement] CastShadows, core/DisplaceShadows.h; 2026-10-27)
#   ObjectDisplace.tesc [SKIP_COLOR0]       -> ObjectDisplace.tcs.spv / ObjectDisplace-c.tcs.spv: the parallax objects (rocks, cliffs;
#                                           [Shaders.POM.Displacement], 2026-10-28), for vertex shaders without TEXCOORD2 / without COLOR0
#   ObjectDisplace.tese x HEIGHT_SLOT 3 / 2 -> ObjectDisplace-h3.tes.spv (main pass), -h2 (lamp, highlight, texture-only passes) and
#                                           -h2-c (the DIFFUSE lamp passes: SKIP_COLOR0), each + -ccw
# Every result is checked with spirv-val. Windows (Git Bash): PATH="/c/VulkanSDK/1.3.296.0/Bin:$PATH" bash tools/nvr-dxvk/compile-tess.sh
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../src/hlsl/NewVegas/Compute/Tess"
build() {   # out, source, defines...
  local out="$1" src="$2"; shift 2
  if ! log=$(glslangValidator -V --target-env vulkan1.1 "$@" -o "$out" "$src" 2>&1); then echo "$log" >&2; echo "FAILED: $out" >&2; exit 1; fi
  spirv-val --target-env vulkan1.1 "$out"
}
build TerrainDisplace.tcs.spv TerrainDisplace.tesc
n=1
for t in 1 2 3 4 5 6 7; do
  build "TerrainDisplace-t$t.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t"
  build "TerrainDisplace-t$t-ccw.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DWINDING_CCW
  build "TerrainDisplace-t$t-s.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DSHADOW_CASTER
  build "TerrainDisplace-t$t-s-ccw.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DSHADOW_CASTER -DWINDING_CCW
  n=$((n+4))
  if [ "$t" -lt 7 ]; then
    build "TerrainDisplace-t$t-d.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DOBJECT_DEPTH
    build "TerrainDisplace-t$t-d-ccw.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DOBJECT_DEPTH -DWINDING_CCW
    build "TerrainDisplace-t$t-sd.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DOBJECT_DEPTH -DSHADOW_CASTER
    build "TerrainDisplace-t$t-sd-ccw.tes.spv" TerrainDisplace.tese "-DTEX_COUNT=$t" -DOBJECT_DEPTH -DSHADOW_CASTER -DWINDING_CCW
    n=$((n+4))
  fi
done
# 2026-10-28: the parallax objects (docs/next/FORK2-DISPLACE.md "Objects")
build ObjectDisplace.tcs.spv ObjectDisplace.tesc
build ObjectDisplace-c.tcs.spv ObjectDisplace.tesc -DSKIP_COLOR0
n=$((n+2))
for v in "h3:-DHEIGHT_SLOT=3" "h2:-DHEIGHT_SLOT=2" "h2-c:-DHEIGHT_SLOT=2 -DSKIP_COLOR0"; do
  name="${v%%:*}"; defs="${v#*:}"
  # shellcheck disable=SC2086
  build "ObjectDisplace-$name.tes.spv" ObjectDisplace.tese $defs
  # shellcheck disable=SC2086
  build "ObjectDisplace-$name-ccw.tes.spv" ObjectDisplace.tese $defs -DWINDING_CCW
  n=$((n+2))
done
echo "compiled $n tessellation shaders"
