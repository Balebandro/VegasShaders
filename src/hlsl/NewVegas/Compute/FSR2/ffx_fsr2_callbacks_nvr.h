// This file is part of the FidelityFX SDK.
//
// Copyright (c) 2022-2023 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ---------------------------------------------------------------------------------------------------------------------------
// New Vegas Reloaded (UNOFFICIAL 2026-10-26, NVR DXVK): AMD's ffx_fsr2_callbacks_glsl.h (FSR 2.2.1, kept beside it unchanged)
// adapted to the NVR DXVK extension's compute contract (src/core/NvrDxvkExt.h). Every other FSR 2 header in this folder is
// AMD's file as released. What differs from AMD's GLSL callbacks:
//  - Samplers come from DXVK's sampler heap (set 0, binding 0), their indices from the push block (the extension writes them),
//    instead of two separate samplers at set 0 bindings 0 and 1.
//  - D3D9 has no 32-bit unsigned integer render-target format, so the reconstructed previous nearest depth (imageAtomicMin on an
//    r32ui image in AMD's code) is a storage buffer of uints, one per render pixel, with atomicMin on it.
//  - Formats D3D9 has no render-target equivalent for are replaced by float formats of at least the same precision:
//    r8 -> r16f (new locks), rg8 -> rg16f (dilated reactive masks), rgba8 -> rgba16f (luma history), rgba16 unorm -> rgba16f
//    (prepared input color), dilated depth r16f -> r32f (AMD's own resource is R32_FLOAT), and the output is declared rgba16f
//    (NVR's output texture) instead of formatless.
//  - No exposure texture: NVR feeds FSR the tonemapped (LDR) frame, so Exposure() is 1 (AMD's default exposure resource is 1 too).
//  - The reactive mask lives in the green channel of NVR's input depth texture (red = depth); the transparency and composition
//    mask is 0 (none).
//  - The luminance mip FSR uses for its shading-change test (SPD mip 4 of the log luma, 1/32 of the render size) is produced by
//    NVR's own small pass (FSRLumaMip.comp) as mip 0 of its own texture; the luminance pyramid / SPD pass is not used.
// ---------------------------------------------------------------------------------------------------------------------------
#include "ffx_fsr2_resources.h"

#if defined(FFX_GPU)
#include "ffx_core.h"
#endif // #if defined(FFX_GPU)

#if defined(FFX_GPU)
#ifndef FFX_FSR2_PREFER_WAVE64
#define FFX_FSR2_PREFER_WAVE64
#endif // #if defined(FFX_GPU)

#extension GL_EXT_nonuniform_qualifier : enable

#if defined(FSR2_BIND_CB_FSR2)
	layout (set = 1, binding = FSR2_BIND_CB_FSR2, std140) uniform cbFSR2_t
	{
		FfxInt32x2    iRenderSize;
		FfxInt32x2    iMaxRenderSize;
		FfxInt32x2    iDisplaySize;
		FfxInt32x2    iInputColorResourceDimensions;
		FfxInt32x2    iLumaMipDimensions;
		FfxInt32      iLumaMipLevelToUse;
		FfxInt32      iFrameIndex;

		FfxFloat32x4  fDeviceToViewDepth;
		FfxFloat32x2  fJitter;
		FfxFloat32x2  fMotionVectorScale;
		FfxFloat32x2  fDownscaleFactor;
		FfxFloat32x2  fMotionVectorJitterCancellation;
		FfxFloat32    fPreExposure;
		FfxFloat32    fPreviousFramePreExposure;
		FfxFloat32    fTanHalfFOV;
		FfxFloat32    fJitterSequenceLength;
		FfxFloat32    fDeltaTime;
		FfxFloat32    fDynamicResChangeFactor;
		FfxFloat32    fViewSpaceToMetersFactor;
	} cbFSR2;
#endif

// NVR: the push block every FSR pass declares (the extension writes the two sampler heap indices; RCAS reads its constants here).
layout(push_constant) uniform NvrFsrPush_t
{
	FfxUInt32   uPointIndex;    // 0: POINT, CLAMP
	FfxUInt32   uLinearIndex;   // 4: LINEAR, CLAMP
	FfxUInt32   uPad0;
	FfxUInt32   uPad1;
	FfxUInt32x4 uRcasConfig;    // 16: FsrRcasCon's four dwords
} nvrPush;

layout (set = 0, binding = 0) uniform sampler s_samplers[];
#define s_PointClamp  s_samplers[nonuniformEXT(nvrPush.uPointIndex & 0xFFFFu)]
#define s_LinearClamp s_samplers[nonuniformEXT(nvrPush.uLinearIndex & 0xFFFFu)]

FfxInt32x2 RenderSize()
{
	return cbFSR2.iRenderSize;
}

FfxInt32x2 MaxRenderSize()
{
	return cbFSR2.iMaxRenderSize;
}

FfxInt32x2 DisplaySize()
{
	return cbFSR2.iDisplaySize;
}

FfxInt32x2 InputColorResourceDimensions()
{
	return cbFSR2.iInputColorResourceDimensions;
}

FfxInt32x2 LumaMipDimensions()
{
	return cbFSR2.iLumaMipDimensions;
}

FfxInt32  LumaMipLevelToUse()
{
	return cbFSR2.iLumaMipLevelToUse;
}

FfxInt32 FrameIndex()
{
	return cbFSR2.iFrameIndex;
}

FfxFloat32x4 DeviceToViewSpaceTransformFactors()
{
	return cbFSR2.fDeviceToViewDepth;
}

FfxFloat32x2 Jitter()
{
	return cbFSR2.fJitter;
}

FfxFloat32x2 MotionVectorScale()
{
	return cbFSR2.fMotionVectorScale;
}

FfxFloat32x2 DownscaleFactor()
{
	return cbFSR2.fDownscaleFactor;
}

FfxFloat32x2 MotionVectorJitterCancellation()
{
	return cbFSR2.fMotionVectorJitterCancellation;
}

FfxFloat32 PreExposure()
{
	return cbFSR2.fPreExposure;
}

FfxFloat32 PreviousFramePreExposure()
{
	return cbFSR2.fPreviousFramePreExposure;
}

FfxFloat32 TanHalfFoV()
{
	return cbFSR2.fTanHalfFOV;
}

FfxFloat32 JitterSequenceLength()
{
	return cbFSR2.fJitterSequenceLength;
}

FfxFloat32 DeltaTime()
{
	return cbFSR2.fDeltaTime;
}

FfxFloat32 DynamicResChangeFactor()
{
	return cbFSR2.fDynamicResChangeFactor;
}

FfxFloat32 ViewSpaceToMetersFactor()
{
	return cbFSR2.fViewSpaceToMetersFactor;
}

// SRVs
#if defined(FSR2_BIND_SRV_INPUT_COLOR)
	layout (set = 1, binding = FSR2_BIND_SRV_INPUT_COLOR)                             uniform texture2D  r_input_color_jittered;
#endif
#if defined(FSR2_BIND_SRV_INPUT_MOTION_VECTORS)
	layout (set = 1, binding = FSR2_BIND_SRV_INPUT_MOTION_VECTORS)                    uniform texture2D  r_input_motion_vectors;
#endif
#if defined(FSR2_BIND_SRV_INPUT_DEPTH)
	layout (set = 1, binding = FSR2_BIND_SRV_INPUT_DEPTH)                             uniform texture2D  r_input_depth;
#endif
#if defined(FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
	layout (set = 1, binding = FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH, std430) readonly buffer NvrReconstructedDepthIn_t { FfxUInt32 data[]; } r_reconstructed_previous_nearest_depth;
#endif
#if defined(FSR2_BIND_SRV_DILATED_MOTION_VECTORS)
	layout (set = 1, binding = FSR2_BIND_SRV_DILATED_MOTION_VECTORS)                  uniform texture2D  r_dilated_motion_vectors;
#endif
#if defined (FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS)
	layout(set = 1, binding = FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS)          uniform texture2D  r_previous_dilated_motion_vectors;
#endif
#if defined(FSR2_BIND_SRV_DILATED_DEPTH)
	layout (set = 1, binding = FSR2_BIND_SRV_DILATED_DEPTH)                           uniform texture2D  r_dilatedDepth;
#endif
#if defined(FSR2_BIND_SRV_INTERNAL_UPSCALED)
	layout (set = 1, binding = FSR2_BIND_SRV_INTERNAL_UPSCALED)                       uniform texture2D  r_internal_upscaled_color;
#endif
#if defined(FSR2_BIND_SRV_LOCK_STATUS)
	layout (set = 1, binding = FSR2_BIND_SRV_LOCK_STATUS)                             uniform texture2D  r_lock_status;
#endif
#if defined(FSR2_BIND_SRV_LOCK_INPUT_LUMA)
	layout (set = 1, binding = FSR2_BIND_SRV_LOCK_INPUT_LUMA)                         uniform texture2D  r_lock_input_luma;
#endif
#if defined(FSR2_BIND_SRV_PREPARED_INPUT_COLOR)
	layout (set = 1, binding = FSR2_BIND_SRV_PREPARED_INPUT_COLOR)                    uniform texture2D  r_prepared_input_color;
#endif
#if defined(FSR2_BIND_SRV_LUMA_HISTORY)
	layout (set = 1, binding = FSR2_BIND_SRV_LUMA_HISTORY)                            uniform texture2D  r_luma_history;
#endif
#if defined(FSR2_BIND_SRV_RCAS_INPUT)
	layout (set = 1, binding = FSR2_BIND_SRV_RCAS_INPUT)                              uniform texture2D  r_rcas_input;
#endif
#if defined(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)
	layout (set = 1, binding = FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)                    uniform texture2D  r_imgMips;
#endif
#if defined(FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT)
	layout (set = 1, binding = FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT)                uniform texture2D  r_upsample_maximum_bias_lut;
#endif
#if defined(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)
	layout (set = 1, binding = FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)                  uniform texture2D  r_dilated_reactive_masks;
#endif

// UAV
#if defined FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH
	layout (set = 1, binding = FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH, std430) buffer NvrReconstructedDepth_t { FfxUInt32 data[]; } rw_reconstructed_previous_nearest_depth;
#endif
#if defined FSR2_BIND_UAV_DILATED_MOTION_VECTORS
	layout (set = 1, binding = FSR2_BIND_UAV_DILATED_MOTION_VECTORS, rg16f)           writeonly uniform image2D  rw_dilated_motion_vectors;
#endif
#if defined FSR2_BIND_UAV_DILATED_DEPTH
	layout (set = 1, binding = FSR2_BIND_UAV_DILATED_DEPTH, r32f)                     writeonly uniform image2D  rw_dilatedDepth;
#endif
#if defined FSR2_BIND_UAV_INTERNAL_UPSCALED
	layout (set = 1, binding = FSR2_BIND_UAV_INTERNAL_UPSCALED, rgba16f)              writeonly uniform image2D  rw_internal_upscaled_color;
#endif
#if defined FSR2_BIND_UAV_LOCK_STATUS
	layout (set = 1, binding = FSR2_BIND_UAV_LOCK_STATUS, rg16f)                      uniform image2D    rw_lock_status;
#endif
#if defined(FSR2_BIND_UAV_LOCK_INPUT_LUMA)
	layout(set = 1, binding = FSR2_BIND_UAV_LOCK_INPUT_LUMA, r16f)                    writeonly uniform image2D    rw_lock_input_luma;
#endif
#if defined FSR2_BIND_UAV_NEW_LOCKS
	layout(set = 1, binding = FSR2_BIND_UAV_NEW_LOCKS, r16f)                          uniform image2D    rw_new_locks;
#endif
#if defined FSR2_BIND_UAV_PREPARED_INPUT_COLOR
	layout (set = 1, binding = FSR2_BIND_UAV_PREPARED_INPUT_COLOR, rgba16f)           writeonly uniform image2D  rw_prepared_input_color;
#endif
#if defined FSR2_BIND_UAV_LUMA_HISTORY
	layout (set = 1, binding = FSR2_BIND_UAV_LUMA_HISTORY, rgba16f)                   uniform image2D  rw_luma_history;
#endif
#if defined FSR2_BIND_UAV_UPSCALED_OUTPUT
	layout (set = 1, binding = FSR2_BIND_UAV_UPSCALED_OUTPUT, rgba16f)                writeonly uniform image2D  rw_upscaled_output;
#endif
#if defined FSR2_BIND_UAV_DILATED_REACTIVE_MASKS
	layout (set = 1, binding = FSR2_BIND_UAV_DILATED_REACTIVE_MASKS, rg16f)           writeonly uniform image2D	 rw_dilated_reactive_masks;
#endif

#if defined(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)
// NVR: the shading-change mip is mip 0 of NVR's own texture (FSRLumaMip.comp), whatever level FSR asks for.
FfxFloat32 LoadMipLuma(FfxInt32x2 iPxPos, FfxInt32 mipLevel)
{
	return texelFetch(r_imgMips, iPxPos, 0).r;
}
#endif

#if defined(FSR2_BIND_SRV_SCENE_LUMINANCE_MIPS)
FfxFloat32 SampleMipLuma(FfxFloat32x2 fUV, FfxInt32 mipLevel)
{
	return textureLod(sampler2D(r_imgMips, s_LinearClamp), fUV, 0.0f).r;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_DEPTH)
FfxFloat32 LoadInputDepth(FfxInt32x2 iPxPos)
{
	return texelFetch(r_input_depth, iPxPos, 0).r;
}
#endif

#if defined(FSR2_BIND_SRV_REACTIVE_MASK)
// NVR: the reactive mask is the green channel of the input depth texture (FSRPrep.comp).
FfxFloat32 LoadReactiveMask(FfxInt32x2 iPxPos)
{
	return texelFetch(r_input_depth, FfxInt32x2(iPxPos), 0).g;
}
#endif

#if defined(FSR2_BIND_SRV_TRANSPARENCY_AND_COMPOSITION_MASK)
FfxFloat32 LoadTransparencyAndCompositionMask(FfxUInt32x2 iPxPos)
{
	return 0.0f;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_COLOR)
FfxFloat32x3 LoadInputColor(FfxInt32x2 iPxPos)
{
	return texelFetch(r_input_color_jittered, iPxPos, 0).rgb;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_COLOR)
FfxFloat32x3 SampleInputColor(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_input_color_jittered, s_LinearClamp), fUV, 0.0f).rgb;
}
#endif

#if defined(FSR2_BIND_SRV_PREPARED_INPUT_COLOR)
FfxFloat32x3 LoadPreparedInputColor(FfxInt32x2 iPxPos)
{
	return texelFetch(r_prepared_input_color, iPxPos, 0).xyz;
}
#endif

#if defined(FSR2_BIND_SRV_INPUT_MOTION_VECTORS)
FfxFloat32x2 LoadInputMotionVector(FfxInt32x2 iPxDilatedMotionVectorPos)
{
	FfxFloat32x2 fSrcMotionVector = texelFetch(r_input_motion_vectors, iPxDilatedMotionVectorPos, 0).xy;

	FfxFloat32x2 fUvMotionVector = fSrcMotionVector * MotionVectorScale();

#if FFX_FSR2_OPTION_JITTERED_MOTION_VECTORS
	fUvMotionVector -= MotionVectorJitterCancellation();
#endif

	return fUvMotionVector;
}
#endif

#if defined(FSR2_BIND_SRV_INTERNAL_UPSCALED)
FfxFloat32x4 LoadHistory(FfxInt32x2 iPxHistory)
{
	return texelFetch(r_internal_upscaled_color, iPxHistory, 0);
}
#endif

#if defined(FSR2_BIND_UAV_LUMA_HISTORY)
void StoreLumaHistory(FfxInt32x2 iPxPos, FfxFloat32x4 fLumaHistory)
{
	imageStore(rw_luma_history, FfxInt32x2(iPxPos), fLumaHistory);
}
#endif

#if defined(FSR2_BIND_SRV_LUMA_HISTORY)
FfxFloat32x4 SampleLumaHistory(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_luma_history, s_LinearClamp), fUV, 0.0f);
}
#endif

#if defined(FSR2_BIND_UAV_INTERNAL_UPSCALED)
void StoreReprojectedHistory(FfxInt32x2 iPxHistory, FfxFloat32x4 fHistory)
{
	imageStore(rw_internal_upscaled_color, iPxHistory, fHistory);
}
#endif

#if defined(FSR2_BIND_UAV_INTERNAL_UPSCALED)
void StoreInternalColorAndWeight(FfxInt32x2 iPxPos, FfxFloat32x4 fColorAndWeight)
{
	imageStore(rw_internal_upscaled_color, FfxInt32x2(iPxPos), fColorAndWeight);
}
#endif

#if defined(FSR2_BIND_UAV_UPSCALED_OUTPUT)
void StoreUpscaledOutput(FfxInt32x2 iPxPos, FfxFloat32x3 fColor)
{
    // NVR: RCAS works in 16x16 tiles; 1080 rows are not a multiple of 16, so skip the stores past the edge.
    if (all(FFX_LESS_THAN(iPxPos, DisplaySize())))
        imageStore(rw_upscaled_output, FfxInt32x2(iPxPos), FfxFloat32x4(fColor, 1.f));
}
#endif

#if defined(FSR2_BIND_SRV_LOCK_STATUS)
FfxFloat32x2 LoadLockStatus(FfxInt32x2 iPxPos)
{
	FfxFloat32x2 fLockStatus = texelFetch(r_lock_status, iPxPos, 0).rg;

    return fLockStatus;
}
#endif

#if defined(FSR2_BIND_UAV_LOCK_STATUS)
void StoreLockStatus(FfxInt32x2 iPxPos, FfxFloat32x2 fLockstatus)
{
	imageStore(rw_lock_status, iPxPos, vec4(fLockstatus, 0.0f, 0.0f));
}
#endif

#if defined(FSR2_BIND_SRV_LOCK_INPUT_LUMA)
FfxFloat32 LoadLockInputLuma(FfxInt32x2 iPxPos)
{
	return texelFetch(r_lock_input_luma, iPxPos, 0).r;
}
#endif

#if defined(FSR2_BIND_UAV_LOCK_INPUT_LUMA)
void StoreLockInputLuma(FfxInt32x2 iPxPos, FfxFloat32 fLuma)
{
	imageStore(rw_lock_input_luma, iPxPos, vec4(fLuma, 0, 0, 0));
}
#endif

#if defined(FSR2_BIND_UAV_NEW_LOCKS)
FfxFloat32 LoadRwNewLocks(FfxInt32x2 iPxPos)
{
	return imageLoad(rw_new_locks, iPxPos).r;
}
#endif

#if defined(FSR2_BIND_UAV_NEW_LOCKS)
void StoreNewLocks(FfxInt32x2 iPxPos, FfxFloat32 newLock)
{
	imageStore(rw_new_locks, iPxPos, vec4(newLock, 0, 0, 0));
}
#endif

#if defined(FSR2_BIND_UAV_PREPARED_INPUT_COLOR)
void StorePreparedInputColor(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x4 fTonemapped)
{
	imageStore(rw_prepared_input_color, iPxPos, fTonemapped);
}
#endif

#if defined(FSR2_BIND_SRV_PREPARED_INPUT_COLOR)
FfxFloat32 SampleDepthClip(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_prepared_input_color, s_LinearClamp), fUV, 0.0f).w;
}
#endif

#if defined(FSR2_BIND_SRV_LOCK_STATUS)
FfxFloat32x2 SampleLockStatus(FfxFloat32x2 fUV)
{
	FfxFloat32x2 fLockStatus = textureLod(sampler2D(r_lock_status, s_LinearClamp), fUV, 0.0f).rg;
	return fLockStatus;
}
#endif

#if defined(FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
FfxFloat32 LoadReconstructedPrevDepth(FfxInt32x2 iPxPos)
{
	// NVR: a buffer, so an out-of-screen read (EvaluateSurface reads a row above and below) is clamped to the screen here.
	iPxPos = clamp(iPxPos, FfxInt32x2(0, 0), RenderSize() - 1);
	return uintBitsToFloat(r_reconstructed_previous_nearest_depth.data[iPxPos.y * RenderSize().x + iPxPos.x]);
}
#endif

#if defined(FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
void StoreReconstructedDepth(FfxInt32x2 iPxSample, FfxFloat32 fDepth)
{
	FfxUInt32 uDepth = floatBitsToUint(fDepth);
	FfxUInt32 index = iPxSample.y * RenderSize().x + iPxSample.x;

	#if FFX_FSR2_OPTION_INVERTED_DEPTH
		atomicMax(rw_reconstructed_previous_nearest_depth.data[index], uDepth);
	#else
		atomicMin(rw_reconstructed_previous_nearest_depth.data[index], uDepth); // min for standard, max for inverted depth
	#endif
}
#endif

#if defined(FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
void SetReconstructedDepth(FfxInt32x2 iPxSample, FfxUInt32 uValue)
{
	rw_reconstructed_previous_nearest_depth.data[iPxSample.y * RenderSize().x + iPxSample.x] = uValue;
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_DEPTH)
void StoreDilatedDepth(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32 fDepth)
{
	imageStore(rw_dilatedDepth, iPxPos, vec4(fDepth, 0.0f, 0.0f, 0.0f));
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_MOTION_VECTORS)
void StoreDilatedMotionVector(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 fMotionVector)
{
	imageStore(rw_dilated_motion_vectors, iPxPos, vec4(fMotionVector, 0.0f, 0.0f));
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_MOTION_VECTORS)
FfxFloat32x2 LoadDilatedMotionVector(FfxInt32x2 iPxInput)
{
	return texelFetch(r_dilated_motion_vectors, iPxInput, 0).rg;
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_MOTION_VECTORS)
FfxFloat32x2 SampleDilatedMotionVector(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_dilated_motion_vectors, s_LinearClamp), fUV, 0.0f).rg;
}
#endif

#if defined(FSR2_BIND_SRV_PREVIOUS_DILATED_MOTION_VECTORS)
FfxFloat32x2 LoadPreviousDilatedMotionVector(FfxInt32x2 iPxInput)
{
	return texelFetch(r_previous_dilated_motion_vectors, iPxInput, 0).rg;
}

FfxFloat32x2 SamplePreviousDilatedMotionVector(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_previous_dilated_motion_vectors, s_LinearClamp), fUV, 0.0f).xy;
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_DEPTH)
FfxFloat32 LoadDilatedDepth(FfxInt32x2 iPxInput)
{
	return texelFetch(r_dilatedDepth, iPxInput, 0).r;
}
#endif

// NVR: no exposure texture; the input is NVR's tonemapped frame.
FfxFloat32 Exposure()
{
	return 1.0f;
}

FfxFloat32 AutoExposure()
{
	return 1.0f;
}

FfxFloat32 SampleLanczos2Weight(FfxFloat32 x)
{
    return 0.f;  // NVR: no Lanczos LUT (reproject uses the reference Lanczos, upsample the approximation)
}

#if defined(FSR2_BIND_SRV_UPSCALE_MAXIMUM_BIAS_LUT)
FfxFloat32 SampleUpsampleMaximumBias(FfxFloat32x2 uv)
{
    // Stored as a SNORM, so make sure to multiply by 2 to retrieve the actual expected range.
    // (NVR stores AMD's SNORM values, already rounded to 16 bits, in a float texture.)
    return FfxFloat32(2.0f) * FfxFloat32(textureLod(sampler2D(r_upsample_maximum_bias_lut, s_LinearClamp), abs(uv) * 2.0f, 0.0f).r);
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)
FfxFloat32x2 SampleDilatedReactiveMasks(FfxFloat32x2 fUV)
{
	return textureLod(sampler2D(r_dilated_reactive_masks, s_LinearClamp), fUV, 0.0f).rg;
}
#endif

#if defined(FSR2_BIND_SRV_DILATED_REACTIVE_MASKS)
FfxFloat32x2 LoadDilatedReactiveMasks(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
{
    return texelFetch(r_dilated_reactive_masks, iPxPos, 0).rg;
}
#endif

#if defined(FSR2_BIND_UAV_DILATED_REACTIVE_MASKS)
void StoreDilatedReactiveMasks(FFX_PARAMETER_IN FfxInt32x2 iPxPos, FFX_PARAMETER_IN FfxFloat32x2 fDilatedReactiveMasks)
{
    imageStore(rw_dilated_reactive_masks, iPxPos, vec4(fDilatedReactiveMasks, 0.0f, 0.0f));
}
#endif

#endif // #if defined(FFX_GPU)
