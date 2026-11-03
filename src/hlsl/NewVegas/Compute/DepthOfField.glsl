// DepthOfField's autofocus passes (technique 0; needs NVR DXVK, effects/DepthOfField.cpp ExtRender). Every pass declares this push
// block (DofPush there), so one layout serves all of them. The helpers are Effects/DepthOfField.fx.hlsl's, line for line.
// Matrices: view[i][j] is the D3DXMATRIX element HLSL TESR_ViewTransform[i][j] (see DepthOfField.frag).
layout(push_constant) uniform Push {
    vec4 viewTransform[4];        // 0: TESR_ViewTransform
    vec4 projectionTransform[4];  // 64: TESR_ProjectionTransform
    vec4 reciprocalResolution;    // 128: TESR_ReciprocalResolution
    vec4 blur;                    // 144: TESR_DepthOfFieldBlur (x distant blur, y start, z end, w base blur radius)
    vec4 data;                    // 160: TESR_DepthOfFieldData (x focus falloff, y radius, z diameter range, w near blur cut off)
    vec4 cameraPosition;          // 176: TESR_CameraPosition
    vec4 cameraData;              // 192: TESR_CameraData (y far Z)
    uint samplerIndex;            // 208: LINEAR, CLAMP (heap index)
    float useBokeh;               // 212: BokehBlur's useBokeh (1 bokeh, 0 plain blur)
    uint pad[2];                  // 216
} pc;

// Helpers.hlsl: invlerps
float invlerps(float a, float b, float t) { return saturate((t - a) / (b - a)); }

// HLSL smoothstep written out (GLSL leaves it undefined when edge0 > edge1, which the near blur uses)
float smoothstepHLSL(float edge0, float edge1, float x) {
    float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

// returns the four quadrants (1 or 0 each): top left near field, top right far field, bottom two the blur amounts
vec4 getQuadrants(vec2 uv) {
    float topLeftQuadrant = (uv.x < 0.5 && uv.y < 0.5) ? 1.0 : 0.0;
    float topRightQuadrant = (uv.x > 0.5 && uv.y < 0.5) ? 1.0 : 0.0;
    float bottomLeftQuadrant = (uv.x < 0.5 && uv.y > 0.5) ? 1.0 : 0.0;
    float bottomRightQuadrant = (uv.x > 0.5 && uv.y > 0.5) ? 1.0 : 0.0;
    return vec4(topLeftQuadrant, topRightQuadrant, bottomLeftQuadrant, bottomRightQuadrant);
}

// remaps the coordinate to [0, 1] relative to the quadrant it belongs to
vec2 remapFromQuadrant(vec2 uv, vec4 quadrants) {
    vec2 offset = vec2(0.5);
    vec2 newX = vec2(uv.x, uv.x - offset.x);
    vec2 newY = vec2(uv.y, uv.y - offset.y);
    uv.x = dot(newX.xyxy, quadrants);
    uv.y = dot(newY.xxyy, quadrants);
    return uv * 2.0;
}
