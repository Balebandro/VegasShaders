// UNOFFICIAL 2026-10-26 (debugging tool, needs NVR DXVK 1.5-debug): shader counters and "printf" for any NVR Vulkan pass.
// C++ side: core/ShaderCounters.h. Docs: docs/next/FORK-DEBUG-PIXEL.md.
//
// A pass includes this file and is compiled a second time with -DNVR_COUNTERS=1 (a separate .spv, e.g. <Name>Count.spv), so the
// normal .spv has none of it and costs nothing. In the counting build:
//   NVR_COUNT(i)          adds 1 to counter i (0..31) for this invocation (atomicAdd)
//   NVR_COUNT_N(i, n)     adds n (a whole number >= 0)
//   NVR_PROBE(slot, v)    fragment shaders: at the one chosen pixel (the pixel inspector's, mapped to this pass's target), stores
//                         the vec4 v in probe slot 0..7 ("printf" for one pixel)
//   NVR_PROBE_AT(p, slot, v)  the same for any shader stage, with the invocation's own pixel p (ivec2 or uvec2)
// Without NVR_COUNTERS every macro is empty.
// The C++ side resets the block before the pass each frame (header = the probe pixel, counts and probes zero), copies it out
// after the pass and reads it back a few frames later without waiting; it logs per-frame averages once a second.
//
// Binding: set 3, binding 0 by default (define NVR_COUNTERS_SET / NVR_COUNTERS_BINDING before including to move it); the C++
// side binds it as a written storage buffer (ShaderCounters::Binding gives the NVR_EXT_BINDING_DESC to add to the pipeline).
// Layout (std430, 272 bytes): uvec4 header (x, y: probe pixel, 0xFFFFFFFF = none; z: frame), uint count[32], vec4 probe[8].
#ifndef NVR_COUNTERS_GLSL
#define NVR_COUNTERS_GLSL

#ifdef NVR_COUNTERS
#ifndef NVR_COUNTERS_SET
#define NVR_COUNTERS_SET 3
#endif
#ifndef NVR_COUNTERS_BINDING
#define NVR_COUNTERS_BINDING 0
#endif
layout(set = NVR_COUNTERS_SET, binding = NVR_COUNTERS_BINDING, std430) buffer NvrCounterBlock {
    uvec4 header;
    uint  count[32];
    vec4  probe[8];
} nvrDbg;
#define NVR_COUNT(i) atomicAdd(nvrDbg.count[(i)], 1u)
#define NVR_COUNT_N(i, n) atomicAdd(nvrDbg.count[(i)], uint(n))
#define NVR_PROBE_AT(p, slot, v) if (all(equal(uvec2(p), nvrDbg.header.xy))) nvrDbg.probe[(slot)] = vec4(v)
#define NVR_PROBE(slot, v) NVR_PROBE_AT(uvec2(gl_FragCoord.xy), slot, v)
#else
#define NVR_COUNT(i)
#define NVR_COUNT_N(i, n)
#define NVR_PROBE_AT(p, slot, v)
#define NVR_PROBE(slot, v)
#endif

#endif
