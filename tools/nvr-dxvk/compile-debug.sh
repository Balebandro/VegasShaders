#!/usr/bin/env bash
# compile-debug.sh (UNOFFICIAL 2026-10-26): the debugging tools' SPIR-V (docs/next/FORK-DEBUG-PIXEL.md).
#   PixelInspect.spv              <- PixelInspect.comp (the pixel inspector's copy step)
#   ReflectionsTraceHiZCount.spv  <- ReflectionsTraceHiZ.frag with -DNVR_COUNTERS=1 (the Hi-Z trace with its shader counters)
# ReflectionsTraceHiZ.spv itself is NOT rebuilt here: the counters exist only in the -DNVR_COUNTERS build, so the normal .spv is
# unchanged. Needs glslangValidator and spirv-val (apt install glslang-tools spirv-tools); also run by .github/workflows/nvr-dxvk.yml.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../../src/hlsl/NewVegas/Compute"
glslangValidator -V --target-env vulkan1.1 -o PixelInspect.spv PixelInspect.comp
spirv-val --target-env vulkan1.1 PixelInspect.spv
glslangValidator -V --target-env vulkan1.1 -DNVR_COUNTERS=1 -o ReflectionsTraceHiZCount.spv ReflectionsTraceHiZ.frag
spirv-val --target-env vulkan1.1 ReflectionsTraceHiZCount.spv
# The normal build of the edited trace must still compile (its committed .spv is not replaced; this is only a check).
glslangValidator -V --target-env vulkan1.1 -o /tmp/ReflectionsTraceHiZ.check.spv ReflectionsTraceHiZ.frag
spirv-val --target-env vulkan1.1 /tmp/ReflectionsTraceHiZ.check.spv
ls -la PixelInspect.spv ReflectionsTraceHiZCount.spv
