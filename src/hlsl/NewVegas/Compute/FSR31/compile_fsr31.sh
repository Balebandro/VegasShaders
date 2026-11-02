#!/usr/bin/env bash
# Builds NVR's FSR 3.1 upscaler passes (AMD FidelityFX SDK 1.1.4 FSR 3.1.x upscaler, MIT) into ../<Name>.spv for NVR DXVK.
# UNOFFICIAL 2026-10-28 ([Shaders.TAA.FSR] Version 1). Built on Windows with the Vulkan SDK (glslang 15), in Git Bash:
#   PATH="/c/VulkanSDK/1.3.296.0/Bin:$PATH" bash compile_fsr31.sh
# Options are the ones AMD's own Vulkan build uses (sdk/src/backends/vk/CMakeShadersFSR3Upscaler.txt and
# gpu/fsr3upscaler/CMakeCompileFSR3UpscalerShaders.txt), with NVR's fixed permutation: low-resolution (render-size) motion
# vectors, unjittered motion vectors, standard (not inverted) depth, reference Lanczos for reprojection (no LUT), approximate
# Lanczos for upsampling, no FP16; LDR and HDR builds of Accumulate (the only pass FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT changes).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/.."
COMMON=(-V --target-env vulkan1.1 -S comp -Os -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
  -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=0
  -DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
  -DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0 -DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
LOG="$(mktemp)"

build() {   # build <source> <name> [extra defines...]
  local src="$1" name="$2"; shift 2
  glslangValidator "${COMMON[@]}" "$@" -I"$HERE" -I"$HERE/fsr3upscaler" -o "$OUT/$name.spv" "$HERE/$src" > "$LOG" || { cat "$LOG"; exit 1; }
  spirv-val --target-env vulkan1.1 "$OUT/$name.spv"
  echo "ok: $name.spv ($(stat -c %s "$OUT/$name.spv") bytes)"
}

LDR=(-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0 -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0)
build FSR31PrepareInputs.comp        FSR31PrepareInputs        "${LDR[@]}"
build FSR31LumaPyramid.comp          FSR31LumaPyramid          "${LDR[@]}"
build FSR31ShadingChangePyramid.comp FSR31ShadingChangePyramid "${LDR[@]}"
build FSR31ShadingChange.comp        FSR31ShadingChange        "${LDR[@]}"
build FSR31PrepareReactivity.comp    FSR31PrepareReactivity    "${LDR[@]}"
build FSR31LumaInstability.comp      FSR31LumaInstability      "${LDR[@]}"
build FSR31RCAS.comp                 FSR31RCAS                 "${LDR[@]}"
build FSR31Accumulate.comp FSR31Accumulate          -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0 -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0
build FSR31Accumulate.comp FSR31AccumulateSharpen   -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0 -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1
build FSR31Accumulate.comp FSR31AccumulateHdr       -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1 -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=0
build FSR31Accumulate.comp FSR31AccumulateSharpenHdr -DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1 -DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=1
rm -f "$LOG"
