// NvrRtCapture.h -- the file one captured RT helper frame is written to (UNOFFICIAL 2026-11-03, claude/rt-replay). MIT licence.
//
// Why: trying a trace shader change (Compute/Rt/*.comp) needed a whole game launch on the iGPU (6-12 minutes through the bench
// queue). The running helper can write one frame's complete inputs to a .nvrcap file (NvrRtHelper.capture / .capture-at next to the
// exe, see docs/next/RT-REPLAY.md), and `NvrRtHelper.exe --replay <file>` runs that frame again without the game: the bottom-level
// structures rebuilt from the geometry data, the scene from the frame's instances, the images uploaded as they were before the
// passes, then the passes (optionally with other .spv files), timed, the written images saved as PNG + raw floats.
//
// Layout: NvrRtCaptureHeader, then chunks until the end of the file. Each chunk: NvrRtCaptureChunk, then `size` bytes, then
// padding to a multiple of 8. Readers skip tags they do not know (tools/nvr-rt-helper/rt_replay_compare.py reads the .f32 outputs,
// not this file).

#pragma once

#include <stdint.h>

#define NVR_RT_CAPTURE_MAGIC   0x50414352u    // 'RCAP'
#define NVR_RT_CAPTURE_VERSION 1u

#define NVR_RT_CAPTURE_TAG(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define NVR_RT_CHUNK_IMAGE_DESC   NVR_RT_CAPTURE_TAG('I', 'M', 'G', 'D')  // index = shared image; NvrRtCaptureImage
#define NVR_RT_CHUNK_IMAGE_BEFORE NVR_RT_CAPTURE_TAG('I', 'M', 'G', 'B')  // index = shared image; rows of width * texelBytes, before the passes
#define NVR_RT_CHUNK_IMAGE_AFTER  NVR_RT_CAPTURE_TAG('I', 'M', 'G', 'A')  // index = shared image; the same after the passes (what the game got)
#define NVR_RT_CHUNK_GEOMETRIES   NVR_RT_CAPTURE_TAG('G', 'E', 'O', 'M')  // NvrRtCaptureGeometry[] (every geometry alive in the helper)
#define NVR_RT_CHUNK_GEOMETRY_DATA NVR_RT_CAPTURE_TAG('G', 'D', 'A', 'T') // uint words: each geometry's dataWords at its vertexOffset, in GEOM order
#define NVR_RT_CHUNK_INSTANCES    NVR_RT_CAPTURE_TAG('I', 'N', 'S', 'T')  // NvrRtInstance[] as the scene was built (skipped ones left out)
#define NVR_RT_CHUNK_RECORDS      NVR_RT_CAPTURE_TAG('R', 'E', 'C', 'S')  // NvrRtInstanceRecord[] as bound at binding 10 (offsets filled in)
#define NVR_RT_CHUNK_PASSES       NVR_RT_CAPTURE_TAG('P', 'A', 'S', 'S')  // NvrRtPass[] of the frame (as NVR sent them)
#define NVR_RT_CHUNK_PASS_TIMES   NVR_RT_CAPTURE_TAG('T', 'I', 'M', 'E')  // float[]: GPU ms of each pass that ran in the captured frame (pass order)
#define NVR_RT_CHUNK_SHADER_PATH  NVR_RT_CAPTURE_TAG('S', 'H', 'D', 'N')  // index = shader; the .spv path NVR registered (NUL-terminated)
#define NVR_RT_CHUNK_SHADER_CODE  NVR_RT_CAPTURE_TAG('S', 'H', 'D', 'S')  // index = shader; its SPIR-V as the file was at capture time

#define NVR_RT_CAPTURE_IMAGE_BEFORE  0x1u     // NvrRtCaptureImage::flags: an IMGB chunk follows
#define NVR_RT_CAPTURE_IMAGE_AFTER   0x2u     // an IMGA chunk follows (the image is written by a pass)
#define NVR_RT_CAPTURE_IMAGE_BOUND   0x4u     // a pass of the frame binds it
#define NVR_RT_CAPTURE_IMAGE_NO_COPY 0x8u     // contents not captured (unknown format, or the arena when no pass binds it)

struct NvrRtCaptureHeader {          // 312 bytes
	uint32_t	magic;
	uint32_t	version;
	uint32_t	protocol;            // NVR_RT_PROTOCOL_VERSION of the helper that wrote it
	uint32_t	headerSize;          // sizeof(NvrRtCaptureHeader)
	uint64_t	frame;               // the helper's frame (exchange) number
	uint32_t	imageCount;
	uint32_t	geometryCount;
	uint32_t	instanceCount;
	uint32_t	passCount;
	uint32_t	shaderCount;
	uint32_t	dataWords;           // size of the geometry data buffer (binding 11) in the helper, words
	uint32_t	noDataGeometries;    // geometries alive without data words (no room): replayed as empty structures
	uint32_t	pad;
	char		device[256];         // the GPU that ran the frame
};

struct NvrRtCaptureChunk {           // 16 bytes
	uint32_t	tag;
	uint32_t	index;
	uint64_t	size;
};

struct NvrRtCaptureImage {           // 24 bytes
	uint32_t	vkFormat;
	uint32_t	width, height;
	uint32_t	texelBytes;          // 0: a format the capture does not know (no contents)
	uint32_t	flags;               // NVR_RT_CAPTURE_IMAGE_*
	uint32_t	usage;               // the VkImageUsageFlags it was imported with
};

struct NvrRtCaptureGeometry {        // 40 bytes
	uint32_t	id;
	uint32_t	vertices, triangles;
	uint32_t	vertexOffset;        // words in the geometry data (NVR_RT_NO_DATA: none)
	uint32_t	indexOffset;
	uint32_t	uvOffset;
	uint32_t	dataWords;
	uint32_t	opFlags;             // NVR_RT_OP_FLAG_* of its build (ALPHA: non-opaque; DYNAMIC: fast build + update)
	uint32_t	compacted;
	uint32_t	pad;
};

#ifdef __cplusplus
static_assert(sizeof(NvrRtCaptureHeader) == 312, "NvrRtCaptureHeader layout");
static_assert(sizeof(NvrRtCaptureChunk) == 16, "NvrRtCaptureChunk layout");
static_assert(sizeof(NvrRtCaptureImage) == 24, "NvrRtCaptureImage layout");
static_assert(sizeof(NvrRtCaptureGeometry) == 40, "NvrRtCaptureGeometry layout");
#endif
