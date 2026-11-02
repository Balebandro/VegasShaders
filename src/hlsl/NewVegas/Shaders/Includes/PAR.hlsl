// Classic parallax occlusion: march down the height field (BaseMap alpha) along the view ray, then refine the hit between the
// last two samples by intersecting the two height segments. Needs TESR_ParallaxData (x scale, y max steps, z min steps).
float2 ParallaxMapping(float2 uv0, float3 eyeTS) {

	if (TESR_ParallaxData.y == -1.0f) return uv0.xy;	// disabled

	float2 shift = eyeTS.xy / length(eyeTS.xyz) * (TESR_ParallaxData.x * 0.1f);	// full-depth uv shift
	float facing = saturate(dot(eyeTS, float3(0.0f, 0.0f, 1.0f)));
	int steps = (int)lerp(TESR_ParallaxData.z, TESR_ParallaxData.y, facing);	// more steps at grazing angles

	float sampled = 0.0f;
	float layer = 1.0f / (float)steps;
	float lastSampled = 1.0f;
	float unused = 0.0f;
	int n = 0;
	float2 uvStep = layer * shift;
	float2 uv = uv0.xy;
	float rayHeight = 1.0f;
	float depth = 0.0f;
	float2 below = 0.0f;	// (ray height, surface height) at the first sample under the surface
	float2 above = 0.0f;	// the same one step earlier

	while (n < steps) {
		uv -= uvStep;
		sampled = tex2Dgrad(BaseMap, uv, 0, 0).a;
		rayHeight -= layer;
		if (sampled > rayHeight) {
			below = float2(rayHeight, sampled);
			above = float2(rayHeight + layer, lastSampled);
			n = steps + 1;	// stop
		}
		else {
			lastSampled = sampled;
			n++;
		}
	}

	// where the line through the two samples crosses the ray
	float gapAbove = above.x - above.y;
	float gapBelow = below.x - below.y;
	float span = gapAbove - gapBelow;

	if (span == 0.0f)
		depth = 0.0f;
	else
		depth = (below.x * gapAbove - above.x * gapBelow) / span;

	return uv0.xy - (shift * (1 - depth));

}
