// UNOFFICIAL 2026-10-29 ObjectMotion: the reprojection every temporal reader shares (core/ObjectMotion.h; docs/next/ROOTPLAN-RESULTS.md).
// Readers so far: TAA.glsl, FSRPrep.comp, FSRUpPrep.comp, AmbientLightAccumulate.frag, ReflectionsTemporal.frag (each inlines the one
// line below); new ones (an RT denoiser's history: rt-sun-core, rt-lamp-shadows, rt-ao-sky) include this file.
//
// The contract:
//  - TESR_ObjectMotionBuffer (fp16 RGBA, the size and layout of TESR_DepthBuffer, RenderScale v2's top-left rect included): rgb = how far
//    the surface at that pixel moved IN THE WORLD since last frame (game units, world axes), a = 1 where something moved itself; 0
//    everywhere else ("moved with the world"). Made after the effects' depth combine (ShaderManager::RenderEffectsPreTonemapping), so a
//    pass that runs after it reads this frame's; ObjectMotion::Ready() on the C++ side says it is made this frame.
//  - To find a pixel's surface last frame: take its camera-relative position as usual (toWorld(uv) * depth), SUBTRACT the move, then
//    apply last frame's camera exactly as before (camera delta, last frame's view / projection). A world-space move needs no
//    projection, jitter or RenderScale handling of its own.
//  - When the buffer is not made (setting off, first frame, camera cut, not bound), skip the read: the reprojection is then exactly the
//    camera-only one. Bind a stand-in texture in its place (as TAA.cpp / TAAFSR.cpp do) and keep the read behind a flag.
//  - The camera-jump flag for every history is GameState.isCameraCut (ShaderManager.cpp): start the history over on it.
// For the 64-bit RT helper: share ObjectMotion::Texture like the depth it already receives (RtCore image), only on frames Ready() is true.
#ifndef NVR_OBJECT_MOTION_GLSL
#define NVR_OBJECT_MOTION_GLSL

// The world move of the surface at uv (texture coordinates of the depth buffer), sampled point / clamp.
vec3 ObjectMotionMove(texture2D motion, sampler pointClamp, vec2 uv) {
    return textureLod(sampler2D(motion, pointClamp), uv, 0.0).xyz;
}

// cameraVector: the pixel's camera-relative world position this frame. Returns it as the point to reproject with last frame's camera.
vec3 ObjectMotionBack(vec3 cameraVector, texture2D motion, sampler pointClamp, vec2 uv, bool motionMade) {
    return motionMade ? cameraVector - ObjectMotionMove(motion, pointClamp, uv) : cameraVector;
}

#endif
