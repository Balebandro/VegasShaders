// UNOFFICIAL, testbed (2026-10-13): police body-camera look (src/effects/Bodycam.cpp, [Shaders.Bodycam], off by default).
// One full-screen pass at the end of the chain, on the finished LDR picture:
//   lens      a wide-angle bend: each pixel looks further out the further it is from the centre (barrel distortion), zoomed in
//             so no black corners show; red and blue a little apart towards the edges (colour fringes)
//   camera    oversharpened (halos along edges), highlights that bloom and clip, high contrast, washed-out colour with a faint
//             green cast, a vignette
//   video     sensor noise that changes 30 times a second (stronger in the dark) and the blocks of compressed video (8 x 8
//             pixels, smoothing the dark and flat parts)
//   frame     a soft black oval round the picture, blur towards the edges, overexposure, a smear while the view turns,
//             lens dirt lit up by bright light
//   overlay   the timestamp in the top right (white with a dark edge; not bent: it is burnt into the recording)

float4 TESR_ReciprocalResolution;	// xy: one pixel in uv, z: width / height
float4 TESR_BodycamLens;			// x: fisheye strength, y: zoom that keeps the corners inside, z: colour fringe (pixels at 1080p), w: vignette
float4 TESR_BodycamImage;			// x: sharpening, y: contrast, z: saturation, w: highlight glow
float4 TESR_BodycamNoise;			// x: grain, y: compression blocks, z: frame number (30 a second), w: green tint
float4 TESR_BodycamText;			// x: text opacity, y: text texel size (pixels at 1080p), z: text width (texels), w: text height (texels)
float4 TESR_BodycamFrame;			// x: black oval border, y: edge blur, z: exposure, w: lens dirt
float4 TESR_BodycamMotion;			// xy: motion blur (uv the picture moved this frame, scaled), z: motion blur setting, w: shadow lift
float4 TESR_BodycamGrade;			// x: cool (neutralises the game's yellow grade), y: lens blur over the whole picture
// UNOFFICIAL 2026-10-13 (user picked: 30 fps video, slow auto exposure, the video breaking down when moving):
float4 TESR_BodycamVideo;			// x: compression breakdown now, y/z: rolling-shutter lean across/down, w: 1 show the held video frame
float4 TESR_BodycamAdapt;			// x: auto exposure strength, y: frame seconds, z: seconds the exposure takes to follow, w: 1 measured
// UNOFFICIAL 2026-10-13 (user: the rest of the list too):
float4 TESR_BodycamBody;			// xy: the picture's shift (uv: chest bob, gun jolt), z: its roll (radians), w: zoom hiding the edges
float4 TESR_BodycamFlash;			// x: muzzle-flash overload now, y: exposure dip after it, z: low-light mode, w: light smear
float4 TESR_BodycamGlitch;			// x: broken-block burst now, y: its seed, z: autofocus strength, w: hiccups

// (Declared in register order: the samplers bind by the order they are declared in.)
sampler2D TESR_RenderedBuffer : register(s0) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = LINEAR; MINFILTER = LINEAR; MIPFILTER = LINEAR; };
sampler2D TESR_BodycamTextBuffer : register(s1) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-13: the last video frame (shown again until the next is due) and the measured brightness (1 x 1: r now, g what
// the camera's exposure has followed to).
sampler2D TESR_BodycamHoldBuffer : register(s2) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
sampler2D TESR_BodycamExposureBuffer : register(s3) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };
// UNOFFICIAL 2026-10-13: the scene's depth (linear, as a share of the far distance), for the autofocus. Last: it takes the next stage.
sampler2D TESR_DepthBuffer : register(s4) = sampler_state { ADDRESSU = CLAMP; ADDRESSV = CLAMP; MAGFILTER = POINT; MINFILTER = POINT; MIPFILTER = NONE; };

static const float2 TextTextureSize = float2(256.0, 12.0);	// Bodycam.h TextWidth, TextHeight

struct VSOUT {
	float4 vertPos : POSITION;
	float2 UVCoord : TEXCOORD0;
};

struct VSIN {
	float4 vertPos : POSITION0;
	float2 UVCoord : TEXCOORD0;
};

VSOUT FrameVS(VSIN IN) {
	VSOUT OUT = (VSOUT)0.0f;
	OUT.vertPos = IN.vertPos;
	OUT.UVCoord = IN.UVCoord;
	return OUT;
}

float Hash(float2 p) {
	float3 p3 = frac(float3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// UNOFFICIAL 2026-10-13 (user: muzzle flashes blown out and wrongly coloured): past 1 a camera's highlight goes to white, keeping
// its hue on the way; clipping each channel on its own turned an orange flash yellow or cyan (red clipped first).
float3 ClipToWhite(float3 c) {
	c = max(c, 0.0);
	float m = max(c.r, max(c.g, c.b));
	return m <= 1.0 ? c : lerp(c / m, 1.0, saturate((m - 1.0) * 0.8));
}

float ValueNoise(float2 p) {
	float2 i = floor(p), f = frac(p);
	f = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), f.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), f.x), f.y);
}

// Smudges and specks on the glass (fixed to the screen), 0-1: a few soft blotches and some small round specks.
float LensDirt(float2 uv) {
	float2 p = uv * float2(TESR_ReciprocalResolution.z, 1.0);
	float blotch = smoothstep(0.55, 0.85, ValueNoise(p * 3.0 + 11.0) * 0.65 + ValueNoise(p * 7.0 + 3.0) * 0.35);
	float2 cell = floor(p * 24.0);
	float2 inCell = frac(p * 24.0) - 0.5;
	float speck = Hash(cell + 41.0) > 0.93 ? smoothstep(0.35, 0.1, length(inCell)) : 0.0;
	return saturate(blotch * 0.7 + speck * 0.6);
}

// Where the lens shows screen point uv from: centred, x in aspect units, pushed out by (1 + k r^2), zoomed back in.
// UNOFFICIAL 2026-10-13: first the chest mount: the picture rolled and shifted (the bob, the gun's jolt), zoomed in so no edge shows.
float2 Bend(float2 uv, float scale) {
	float aspect = TESR_ReciprocalResolution.z;
	float2 p = (uv * 2.0 - 1.0) * float2(aspect, 1.0);
	float roll = TESR_BodycamBody.z, rs = sin(roll), rc = cos(roll);
	p = float2(p.x * rc - p.y * rs, p.x * rs + p.y * rc) / max(TESR_BodycamBody.w, 1.0) + TESR_BodycamBody.xy * 2.0 * float2(aspect, 1.0);
	float r2 = dot(p, p);
	p *= (1.0 + TESR_BodycamLens.x * r2) * scale / TESR_BodycamLens.y;
	return p / float2(aspect, 1.0) * 0.5 + 0.5;
}

float3 Read(float2 uv) {
	return tex2Dlod(TESR_RenderedBuffer, float4(uv, 0.0, 0.0)).rgb;
}

// While the view turns: seven reads along the way the picture moved this frame (the camera's shutter smearing it).
float3 Smeared(float2 uv, bool moving) {
	[branch] if (!moving) return Read(uv);
	float3 c = 0.0;
	[unroll] for (int i = 0; i < 7; i++) c += Read(uv + TESR_BodycamMotion.xy * (i / 6.0 - 0.5));
	return c / 7.0;
}

// UNOFFICIAL 2026-10-13: the brightness the camera sees now (8 x 8 points of the picture, averaged in log, linear light), and its
// exposure following it slowly: r follows within a tenth of a second, g follows r over ExposureSpeed seconds (both in log, so
// up and down take as long).
float4 MeasurePS(VSOUT IN) : COLOR0 {
	float sum = 0.0;
	[unroll] for (int y = 0; y < 8; y++) {
		[unroll] for (int x = 0; x < 8; x++) {
			float3 c = tex2Dlod(TESR_RenderedBuffer, float4((float2(x, y) + 0.5) / 8.0, 0.0, 0.0)).rgb;
			sum += log(max(Luma(pow(saturate(c), 2.2)), 1e-3));
		}
	}
	float now = sum / 64.0;
	float4 stored = tex2Dlod(TESR_BodycamExposureBuffer, float4(0.5, 0.5, 0.0, 0.0));
	float2 last = log(max(stored.rg, 1e-4));
	if (TESR_BodycamAdapt.w < 0.5) last = now.xx;
	float fast = lerp(last.x, now, 1.0 - exp(-TESR_BodycamAdapt.y / 0.08));
	float slow = lerp(last.y, fast, 1.0 - exp(-TESR_BodycamAdapt.y / max(TESR_BodycamAdapt.z, 0.05)));
	// UNOFFICIAL 2026-10-13: the autofocus: b, how far the middle of the picture is (log of its depth, five points); a, where the
	// lens is focused, following it over about a third of a second (the picture is soft while they differ).
	float depth = 0.0;
	[unroll] for (int k = 0; k < 5; k++) {
		float2 at = 0.5 + float2(k == 1 ? -0.06 : k == 2 ? 0.06 : 0.0, k == 3 ? -0.06 : k == 4 ? 0.06 : 0.0);
		depth += log(max(tex2Dlod(TESR_DepthBuffer, float4(at, 0.0, 0.0)).x, 1e-4));
	}
	depth /= 5.0;
	float focus = TESR_BodycamAdapt.w < 0.5 ? depth : lerp(stored.a, depth, 1.0 - exp(-TESR_BodycamAdapt.y / 0.3));
	return float4(exp(fast), exp(slow), depth, focus);
}

float4 BodycamPS(VSOUT IN) : COLOR0 {
	float2 px = TESR_ReciprocalResolution.xy;
	float2 uv = IN.UVCoord;
	// UNOFFICIAL 2026-10-13: between video frames (VideoFps) the last one shows again, as recorded video does.
	[branch] if (TESR_BodycamVideo.w > 0.5) return tex2Dlod(TESR_BodycamHoldBuffer, float4(uv, 0.0, 0.0));
	float scale1080 = 1.0 / (px.y * 1080.0);		// pixels at this resolution per pixel at 1080p

	// The compressed video: the picture sampled at its 8 x 8 block's centre, mixed in where it is dark and flat.
	float2 blockUV = (floor(uv / (px * 8.0)) + 0.5) * px * 8.0;
	// UNOFFICIAL 2026-10-13: the rolling shutter: lower rows were read later in the turn, so they are moved along with it.
	float2 shutter = TESR_BodycamVideo.yz * (uv.y - 0.5);
	float2 at = Bend(uv, 1.0) + shutter;
	float edge = length((uv * 2.0 - 1.0) * float2(TESR_ReciprocalResolution.z, 1.0));

	// Colour fringes: red a little further out, blue a little further in, growing towards the edges.
	float fringe = TESR_BodycamLens.z * scale1080 * px.y * edge * edge * 0.5;
	float2 outward = normalize(at - 0.5 + 1e-5);
	bool moving = length(TESR_BodycamMotion.xy / px) > 0.75;
	float3 colour;
	colour.r = Smeared(at + outward * fringe, moving).r;
	colour.g = Smeared(at, moving).g;
	colour.b = Smeared(at - outward * fringe, moving).b;

	// The cheap lens blurs towards its edges: eight reads on two rings round the point (a soft disc, not copies), wider the
	// further out (about 6 pixels at 1080p in the corners at 0.7). Lens blur softens the whole picture on top (about 2.5
	// pixels at 1080p at 1: the cheap plastic lens is never quite in focus).
	float blurRadius = (TESR_BodycamFrame.y * edge * edge * 2.0 + TESR_BodycamGrade.y * 2.5) * px.y * scale1080;
	// UNOFFICIAL 2026-10-13: the autofocus hunting: soft while the lens catches up with how far the middle of the picture is.
	[branch] if (TESR_BodycamGlitch.z > 0.0 && TESR_BodycamAdapt.w > 0.5) {
		float2 focus = tex2Dlod(TESR_BodycamExposureBuffer, float4(0.5, 0.5, 0.0, 0.0)).ba;
		blurRadius += TESR_BodycamGlitch.z * saturate(abs(focus.x - focus.y) * 0.7) * 6.0 * px.y * scale1080;
	}
	[branch] if (blurRadius > px.y * 0.5) {
		float2 r = blurRadius * float2(px.x / px.y, 1.0);
		float3 ring = 0.0;
		// The rings turned by a different angle at each pixel (interleaved gradient noise), so that a wide blur comes out as
		// a soft grainy disc rather than eight faint copies of the picture.
		float turn = frac(52.9829189 * frac(dot(uv / px, float2(0.06711056, 0.00583715)))) * 1.570796;
		[unroll] for (int k = 0; k < 4; k++) {
			float a = k * 1.570796 + turn;
			ring += Read(at + r * float2(cos(a), sin(a)) * 0.5);				// inner ring
			ring += Read(at + r * float2(cos(a + 0.785398), sin(a + 0.785398)));	// outer ring, turned 45 degrees
		}
		colour = colour * 0.2 + ring * 0.1;
	}

	// Oversharpening: the difference from the four neighbours, added back (the halos small cameras leave along edges).
	float2 step = px * 1.25;
	float3 around = (Read(at + float2(step.x, 0.0)) + Read(at - float2(step.x, 0.0)) + Read(at + float2(0.0, step.y)) + Read(at - float2(0.0, step.y))) * 0.25;
	colour += (colour - around) * TESR_BodycamImage.x;

	// Highlight glow: bright areas bleed over what is near them (the halation of a small sensor: windows, sky and lamps
	// blown out to white with a wide soft halo). Two rings of eight reads, about 1% and 3.5% of the screen out (turned
	// against each other), the near ring stronger; an even spread, not copies.
	float3 glow = 0.0;
	[unroll] for (int i = 0; i < 8; i++) {
		float a = i * 0.785398;
		float2 way = float2(cos(a), sin(a)) * float2(px.x / px.y, 1.0);
		float2 way2 = float2(cos(a + 0.3927), sin(a + 0.3927)) * float2(px.x / px.y, 1.0);
		glow += max(Read(at + way * 0.012) - 0.65, 0.0) * 0.65;
		glow += max(Read(at + way2 * 0.035) - 0.65, 0.0) * 0.35;
	}
	colour += glow * 0.125 * TESR_BodycamImage.w * 2.0;
	// UNOFFICIAL 2026-10-13: light smear: a cheap sensor bleeds very bright light up and down its column, a vertical streak
	// through lamps and the sun (most of all in low light). Twelve reads up and down the column, jittered per pixel.
	[branch] if (TESR_BodycamFlash.w > 0.0) {
		float jitter = frac(52.9829189 * frac(dot(uv / px, float2(0.06711056, 0.00583715))));
		float3 streak = 0.0;
		[unroll] for (int i = 1; i <= 6; i++) {
			float o = (i - 0.5 + jitter) * 0.045;
			float weight = 1.0 - i / 7.0;
			streak += (max(Read(at + float2(0.0, o)) - 0.85, 0.0) + max(Read(at - float2(0.0, o)) - 0.85, 0.0)) * weight;
		}
		colour += streak * 0.35 * TESR_BodycamFlash.w * (1.0 + TESR_BodycamFlash.z);
	}
	// The muzzle flash overloads the sensor: the frame washes out round bright parts for a moment.
	colour += glow * 0.5 * TESR_BodycamFlash.x;

	// Compression blocks in the dark and flat parts.
	float3 block = Read(Bend(blockUV, 1.0) + shutter);
	float flat = 1.0 - saturate(length(colour - around) * 8.0);
	float dark = 1.0 - saturate(Luma(block) * 2.5);
	colour = lerp(colour, block, TESR_BodycamNoise.y * flat * dark);
	// UNOFFICIAL 2026-10-13: while the view turns or runs the encoder runs out of bits: bigger blocks (16 x 16) everywhere, flat
	// parts first, and the small blocks spread from the dark into everything.
	[branch] if (TESR_BodycamVideo.x > 0.01) {
		// UNOFFICIAL 2026-10-13 (user: 0.1 and 2 looked the same): no longer clipped at 1 (Breakup x how fast the view moves, up
		// to 3): it builds up in steps, small blocks first, then 16 x 16, then 32 x 32 and colour banding at the strongest.
		float breakup = TESR_BodycamVideo.x;
		float2 bigUV = (floor(uv / (px * 16.0)) + 0.5) * px * 16.0;
		float3 big = Read(Bend(bigUV, 1.0) + shutter);
		float2 hugeUV = (floor(uv / (px * 32.0)) + 0.5) * px * 32.0;
		float3 huge = Read(Bend(hugeUV, 1.0) + shutter);
		colour = lerp(colour, block, saturate(breakup * 0.35 * (0.4 + 0.6 * flat)));
		colour = lerp(colour, big, saturate((breakup - 0.3) * 0.45 * (0.3 + 0.7 * flat)));
		colour = lerp(colour, huge, saturate((breakup - 1.2) * 0.4 * (0.2 + 0.8 * flat)));
		float levels = max(48.0 - breakup * 12.0, 8.0);
		colour = lerp(colour, floor(colour * levels + 0.5) / levels, saturate(breakup * 0.3));
	}

	// Lens dirt: smudges on the glass that light up where bright light falls on them.
	float dirt = LensDirt(uv);
	float bright = Luma(glow) * 0.125 + 0.2 * Luma(colour) * Luma(colour);
	colour += dirt * bright * TESR_BodycamFrame.w;

	// The camera's colour: overexposed, contrast round mid-grey, washed out, a faint green cast; highlights clipped softly.
	// (Exposure, contrast, saturation and lift: by day, at night, or indoors; Bodycam.cpp blends them.) Lift raises the dark
	// tones the way a camera's auto exposure does in the dark, leaving black and white where they are.
	// UNOFFICIAL 2026-10-13: the auto exposure lags: when the scene got brighter than the camera has caught up with, the picture is
	// overexposed, darker when it got darker; in steady light the ratio is 1 and only the set exposure applies.
	float gain = 1.0;
	[branch] if (TESR_BodycamAdapt.x > 0.0 && TESR_BodycamAdapt.w > 0.5) {
		float2 measured = tex2Dlod(TESR_BodycamExposureBuffer, float4(0.5, 0.5, 0.0, 0.0)).rg;
		gain = clamp(pow(max(measured.x, 1e-4) / max(measured.y, 1e-4), TESR_BodycamAdapt.x / 2.2), 0.2, 5.0);
	}
	// UNOFFICIAL 2026-10-13: a shot's flash blows the frame out, then the exposure pulls it down before settling.
	gain *= (1.0 + TESR_BodycamFlash.x * 1.5) * (1.0 - TESR_BodycamFlash.y * 0.4);
	colour *= TESR_BodycamFrame.z * gain;
	colour = pow(ClipToWhite(colour), 1.0 / (1.0 + TESR_BodycamMotion.w));
	// (Clipped here: contrast above 1 pushes the channels past 0 and 1, and Cool below then turned those into blown-out blue.)
	colour = ClipToWhite((colour - 0.5) * TESR_BodycamImage.y + 0.5);
	// Cool: takes the yellow out of New Vegas's own grade towards the neutral whites of real footage (blue up, red and
	// green a little down): a fixed white balance, its gains scaled so grey keeps its brightness.
	colour *= lerp(float3(1.0, 1.0, 1.0), float3(0.93, 0.98, 1.12) / 0.9795, TESR_BodycamGrade.x);
	float luma = Luma(colour);
	colour = lerp(luma.xxx, colour, TESR_BodycamImage.z);
	colour *= lerp(float3(1.0, 1.0, 1.0), float3(0.94, 1.0, 0.95), TESR_BodycamNoise.w);
	// UNOFFICIAL 2026-10-13: low-light mode: in the dark the camera gives up colour for brightness: nearly black and white, the
	// shadows lifted, and (below) much more grain. Mostly a night setting.
	[branch] if (TESR_BodycamFlash.z > 0.0) {
		float l = Luma(colour);
		colour = lerp(colour, float3(0.96, 1.0, 0.97) * pow(max(l, 0.0), 0.85), TESR_BodycamFlash.z * 0.9);
	}
	colour = ClipToWhite(colour);
	colour = (1.0 - exp(-max(colour, 0.0) * 1.15)) / (1.0 - exp(-1.15));	// compresses the top, keeps 0 and 1
	colour = saturate(colour);

	// Sensor noise, per video frame: grey grain plus a little colour noise, stronger in the dark.
	float frame = TESR_BodycamNoise.z;
	float2 cell = floor(uv / px);
	float n = Hash(cell + frame * 17.31) - 0.5;
	float3 chroma = float3(Hash(cell * 1.3 + frame * 3.7), Hash(cell * 0.7 + frame * 5.1), Hash(cell * 1.9 + frame * 7.3)) - 0.5;
	float amount = TESR_BodycamNoise.x * 0.06 * lerp(1.0, 0.35, saturate(Luma(colour) * 1.5)) * (1.0 + TESR_BodycamFlash.z * 1.5);
	colour += (n + chroma * 0.4) * amount;

	// Vignette; outside the bent picture (FitEdges below 1): black.
	colour *= 1.0 - TESR_BodycamLens.w * smoothstep(0.6, 1.9, edge);
	// The camera's frame: a soft black oval (in screen proportions) closing in from the corners.
	float oval = length(uv * 2.0 - 1.0);
	float ovalStart = lerp(1.6, 0.75, TESR_BodycamFrame.x);
	colour *= 1.0 - smoothstep(ovalStart, ovalStart + 0.35, oval);
	colour *= all(at >= 0.0) && all(at <= 1.0) ? 1.0 : 0.0;

	// UNOFFICIAL 2026-10-13: an encoder hiccup: for a moment some 16 x 16 blocks show stale picture (the last kept frame,
	// shifted sideways) or smear, some with their colour channels slipped.
	[branch] if (TESR_BodycamGlitch.x > 0.0) {
		float2 blockId = floor(uv / (px * 16.0));
		float h = Hash(blockId + TESR_BodycamGlitch.y);
		float row = Hash(float2(blockId.y, TESR_BodycamGlitch.y * 0.37));
		if (h < TESR_BodycamGlitch.x * 0.35 || row < TESR_BodycamGlitch.x * 0.08) {
			float2 shift = float2((Hash(blockId * 1.7 + TESR_BodycamGlitch.y) - 0.5) * 0.08, 0.0);
			float3 stale = tex2Dlod(TESR_BodycamHoldBuffer, float4(uv + shift, 0.0, 0.0)).rgb;
			float3 smear = Read(Bend((blockId + 0.5) * px * 16.0, 1.0));
			colour = Hash(blockId + 7.0) > 0.5 ? stale : smear;
			if (Hash(blockId + 3.0) > 0.6) colour.rb = colour.br;
		}
	}

	// The timestamp, top right, not bent: white with a dark edge. (Only its two reads are inside the test; the blends are
	// done after it, unconditionally: with the blends inside, the compiler dropped the overlay, 2026-10-13.)
	float lit = 0.0, shadow = 0.0;
	float texel = max(floor(TESR_BodycamText.y * scale1080 + 0.5), 1.0);	// whole pixels per texel: crisp
	float2 corner = float2(1.0 / px.x - TESR_BodycamText.z * texel - 40.0 * scale1080, 34.0 * scale1080);
	float2 pixel = uv / px;
	float2 t = (pixel - corner) / texel;
	[branch] if (TESR_BodycamText.x > 0.0 && all(t > -1.0) && all(t < TESR_BodycamText.zw + 1.0)) {
		lit = tex2Dlod(TESR_BodycamTextBuffer, float4((floor(t) + 0.5) / TextTextureSize, 0.0, 0.0)).a;
		float2 back = (pixel - corner - max(floor(texel * 0.35), 1.0)) / texel;
		shadow = tex2Dlod(TESR_BodycamTextBuffer, float4((floor(back) + 0.5) / TextTextureSize, 0.0, 0.0)).a;
	}
	colour = lerp(colour, colour * 0.25, shadow * TESR_BodycamText.x);
	colour = lerp(colour, float3(0.93, 0.93, 0.9), lit * TESR_BodycamText.x);
	return float4(saturate(colour), 1.0);
}

technique {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 BodycamPS();
	}
}

technique Measure {
	pass {
		VertexShader = compile vs_3_0 FrameVS();
		PixelShader = compile ps_3_0 MeasurePS();
	}
}
