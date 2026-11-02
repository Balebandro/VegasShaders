#pragma once

// UNOFFICIAL LampScissor ([Main.Main.Performance] LampScissor, 2026-10-11).
//
// Indoors the game adds the light of extra lamps to an object in passes of their own (ObjectTemplate DIFFUSE, PS
// SLS2045/2046, and POINT specular, SLS2051-2056): the whole object is drawn again, shaded and blended for every pixel,
// although a lamp only reaches pixels within its radius (vanillaAttSq is exactly 0 beyond it). In the Rivet City hangar
// these passes cost ~2.5-3 ms of the 7 ms world scene (P47), about half of their draws passing 0 pixels.
// Here such a draw is limited to the screen rectangle that can hold its lamps' spheres (scissor test), so pixels no lamp
// can reach are not shaded at all. Exact: the rectangle contains everything of each sphere (widened by a margin) the camera
// can see; anything unusual (not a point light, no radius, another camera's viewport, an orthographic camera, an object
// with its own colour in these passes) leaves the draw unscissored. A draw none of whose lamps is on screen at all is
// skipped (it adds nothing visible).
// Rectangle (2026-10-21 LampScissorExact, live; bench\deep-roadmap\evidence\lampscissor\LOG.md): the exact screen bound of the
// part of each sphere in front of the near plane (AxisRange), so lamps whose reach crosses the near plane are scissored too
// and a lamp wholly behind it is dropped; only a lamp whose reach holds the camera still gives the whole screen. An empty slot
// in the pass's lamp list (about half of these draws indoors) is ignored: the game makes its colour black and every term of
// these shaders is multiplied by it. Off: the projection of each sphere's bounding box, and a lamp whose box reaches behind
// z = near/2, or an empty slot, leaves the draw unscissored (in lamp-rich interiors ~80% of the draws: Ultra-Luxe 434 of 537).
// Reach: ShadowLightShader::SetupGeometryConstants_Lights 0x00B78A90 puts the lamp into the object's model space and
// divides the radius by the object's scale (or scales the position instead for passes 0x231-0x233), so in world space
// a lamp reaches exactly NiPointLight::Spec.r (+0xE0, BSShaderLightingProperty::SetLight1x2x 0x00B70820) from its world
// position (+0x8C). ShadowSceneLight +0xF4 = point light, +0xF8 = the NiPointLight. The pixel shaders interpolate the
// per-vertex squared distance, which is convex: inside a triangle it is never below the true one, so no pixel outside the
// sphere is lit.
// Only the world scene, only the Standard (not skinned) draw, only NVR's own versions of those pixel shaders.
namespace LampScissor {

	inline bool Enabled = false;		// the setting, read each frame
	inline bool Exact = true;			// LampScissorExact, read each frame
	inline bool LampOnlyPass = false;	// the bound pixel shader is one of NVR's lamp-only passes (SetShadersHook)
	inline bool Scissored = false;		// Before() turned the scissor test on; After() restores
	inline DWORD SavedEnable = FALSE;
	inline RECT SavedRect = {};
	inline unsigned StatFrames = 0;
	inline int StatMode = -1;			// 0 off, 1 box, 2 exact: the statistics restart when it changes (one line per A/B shot)
	inline double StatDraws = 0, StatScissored = 0, StatSkipped = 0, StatAreaShare = 0, StatShaded = 0;
	inline double StatBehind = 0, StatUnusual = 0, StatInside = 0, StatCovers = 0;	// the full-screen draws, by reason
	enum Unusual { OwnColour, CameraOff, NoLight, NotPoint, NoSource, BadRadius, StateFailed, UnusualKinds };
	inline double StatWhy[UnusualKinds] = {};
	inline double StatEmpty = 0;		// exact mode: draws with an empty lamp slot (ignored)
	inline const char* BoundName = "";	// the lamp-only pixel shader bound (diagnostic)
	inline unsigned UnusualLogged = 0;	// 2026-10-21 diagnostic: the first examples of each kind and pass type, per launch

	// SetShadersHook, after the game bound the pass's shaders.
	inline void OnBind(const NiD3DPixelShaderEx* shader, bool world) {
		LampOnlyPass = false;
		if (!Enabled || !world || !shader || !shader->Name || !shader->ShaderHandleBackup ||
			shader->ShaderHandle == (IDirect3DPixelShader9*)shader->ShaderHandleBackup) return;
		const char* name = shader->Name;
		if (strncmp(name, "SLS20", 5) || strlen(name) != 11 || strcmp(name + 7, ".pso")) return;
		const int number = (name[5] - '0') * 10 + (name[6] - '0');
		LampOnlyPass = number == 45 || number == 46 || (number >= 51 && number <= 56);
		if (LampOnlyPass) BoundName = name;
	}

	// Diagnostic (profile on): an example of each kind of 'unusual' lamp, once per pass type, at most 40 lines a launch.
	inline void LogUnusual(Unusual why, const PointShadowForward::RenderPassView* pass, int k) {
		static const char* names[UnusualKinds] = { "own colour (barrel glow)", "camera or viewport", "no light", "not a point light", "no source light", "radius", "scissor state" };
		if (UnusualLogged >= 40) return;
		static std::vector<unsigned> done;
		const unsigned key = ((unsigned)why << 16) | pass->passEnum | ((k > 0 ? 1u : 0u) << 24);
		for (unsigned d : done) if (d == key) return;
		done.push_back(key);
		UnusualLogged++;
		const UInt8* light = k >= 0 ? (const UInt8*)pass->lights[k] : nullptr;
		const NiPointLight* source = light ? *(NiPointLight* const*)(light + 0xF8) : nullptr;
		const NiGeometry* geometry = pass->geometry;
		Logger::Log("LAMP SCISSOR unusual: %s, pass 0x%03X (%s, %u lamps of max %u), lamp %d: point %d ambient %d dynamic %d, source %p radius %.2f dimmer %.2f at (%.0f %.0f %.0f), object '%s'",
			names[why], pass->passEnum, BoundName, pass->numLights, pass->maxLights, k, light ? *(const bool*)(light + 0xF4) : -1, light ? *(const bool*)(light + 0xF5) : -1,
			light ? *(const bool*)(light + 0xFC) : -1, (const void*)source, source ? source->Spec.r : 0.0f, source ? source->Dimmer : 0.0f,
			source ? source->m_worldTransform.pos.x : 0.0f, source ? source->m_worldTransform.pos.y : 0.0f, source ? source->m_worldTransform.pos.z : 0.0f,
			geometry && geometry->m_pcName ? geometry->m_pcName : "(unnamed)");
	}

	inline void BeginFrame() {
		Enabled = TheSettingManager->SettingsMain.Main.LampScissor;
		Exact = TheSettingManager->SettingsMain.Main.LampScissorExact;
		LampOnlyPass = false;
		Scissored = false;
		if (!GpuTimer::Enabled) return;
		const int mode = Enabled ? (Exact ? 2 : 1) : 0;
		if (mode != StatMode || StatFrames >= 240) {
			const double f = StatFrames;
			if (f >= 30 && StatDraws > 0) {
				const double full = StatBehind + StatUnusual + StatInside + StatCovers;
				Logger::Log("LAMP SCISSOR %s: lamp-only draws %.0f/frame, scissored %.0f (average %.0f%% of the screen), skipped (no lamp on screen) %.0f, full screen %.0f (camera inside a lamp's reach %.0f, rectangle covers the screen %.0f, lamp behind the camera %.0f, unusual %.0f); screen open to these draws %.1f%% (full screen = 100) (%.0f frames)",
					StatMode == 2 ? "on (exact)" : "on (box)", StatDraws / f, StatScissored / f, StatScissored > 0 ? 100.0 * StatAreaShare / StatScissored : 0.0,
					StatSkipped / f, full / f, StatInside / f, StatCovers / f, StatBehind / f, StatUnusual / f, 100.0 * StatShaded / StatDraws, f);
				if (StatEmpty > 0) Logger::Log("LAMP SCISSOR empty lamp slots (ignored, black): in %.0f draws a frame", StatEmpty / f);
				if (StatUnusual > 0)
					Logger::Log("LAMP SCISSOR unusual by kind: own colour %.0f, camera or viewport %.0f, no light %.0f, not a point light %.0f, no source light %.0f, radius %.0f, scissor state %.0f",
						StatWhy[OwnColour] / f, StatWhy[CameraOff] / f, StatWhy[NoLight] / f, StatWhy[NotPoint] / f, StatWhy[NoSource] / f, StatWhy[BadRadius] / f, StatWhy[StateFailed] / f);
			}
			for (double& w : StatWhy) w = 0;
			StatEmpty = 0;
			StatFrames = 0;
			StatDraws = StatScissored = StatSkipped = StatAreaShare = StatShaded = 0;
			StatBehind = StatUnusual = StatInside = StatCovers = 0;
			StatMode = mode;
		}
		StatFrames++;
	}

	// The range of a/z over the disk of radius r around (a, z) cut at z >= n (n > 0): one screen axis of the exact bound of a
	// sphere seen from the eye (the origin) after the near-plane clip. Any (a, z) of the disk has a point of the sphere above it,
	// so a/z over the clipped sphere spans exactly this range. Its ends are among the 2 tangent points from the eye (when the eye
	// is outside the disk; kept where z >= n) and the 2 ends of the chord z = n (when the near plane cuts the disk).
	// false: no part of the disk is at z >= n.
	inline bool AxisRange(float a, float z, float r, float n, float& lo, float& hi) {
		if (z + r < n) return false;
		lo = 1e30f; hi = -1e30f;
		const bool cut = z - r < n;
		const float d2 = a * a + z * z, r2 = r * r;
		if (d2 > r2) {	// tangent points (l2 (a, z) +- r l (z, -a)) / d2, l = their distance from the eye
			const float l2 = d2 - r2, l = sqrtf(l2);
			for (float side = -1.0f; side <= 1.0f; side += 2.0f) {
				const float ta = (l2 * a + side * r * l * z) / d2, tz = (l2 * z - side * r * l * a) / d2;
				if (tz > 0.0f && (!cut || tz >= n)) { lo = (std::min)(lo, ta / tz); hi = (std::max)(hi, ta / tz); }
			}
		}
		if (cut) {
			const float h = n - z, w = sqrtf((std::max)(0.0f, r2 - h * h));
			lo = (std::min)(lo, (a - w) / n); hi = (std::max)(hi, (a + w) / n);
		}
		return lo <= hi;
	}

	enum Result { Draw, Skip };

	// Before an object's draw (RenderPassStandardHook). Skip: nothing of it can be lit; otherwise drawn, maybe scissored.
	// ownColour: the object adds a colour of its own in these passes (the GunFX barrel glow), never scissored.
	inline Result Before(const PointShadowForward::RenderPassView* pass, bool world, bool ownColour) {
		Scissored = false;
		if (!Enabled || !LampOnlyPass || !world || !pass || !pass->numLights || !pass->lights) return Draw;
		const bool stats = GpuTimer::Enabled;
		if (stats) StatDraws += 1;
		auto unusual = [stats, pass](Unusual why, int k) { if (stats) { StatUnusual += 1; StatShaded += 1; StatWhy[why] += 1; LogUnusual(why, pass, k); } return Draw; };
		if (ownColour) return unusual(OwnColour, -1);
		IDirect3DDevice9* device = TheRenderManager->device;
		D3DVIEWPORT9 port;
		NiCamera* camera = WorldSceneGraph ? WorldSceneGraph->camera : nullptr;
		// UNOFFICIAL 2026-10-27: RenderScale's world viewport (the top-left render size) is the world's too; the rectangle below is
		// computed in the viewport's own pixels.
		if (!camera || camera->Frustum.Ortho || FAILED(device->GetViewport(&port)) || port.X || port.Y ||
			!TAAEffect::IsWorldViewport(port)) return unusual(CameraOff, -1);
		const NiMatrix33& rot = camera->m_worldTransform.rot;
		const NiPoint3& eye = camera->m_worldTransform.pos;
		const NiFrustum& frustum = camera->Frustum;
		const float frustumWidth = frustum.Right - frustum.Left, frustumHeight = frustum.Top - frustum.Bottom;
		if (frustumWidth <= 0.0f || frustumHeight <= 0.0f || frustum.Near <= 0.0f) return unusual(CameraOff, -1);
		// View basis as RenderManager::UpdateSceneCameraData builds it: forward, up, right = columns 0, 1, 2.
		const float forward[3] = { rot.data[0][0], rot.data[1][0], rot.data[2][0] };
		const float up[3] = { rot.data[0][1], rot.data[1][1], rot.data[2][1] };
		const float right[3] = { rot.data[0][2], rot.data[1][2], rot.data[2][2] };
		// x/z and y/z of the view (Left..Right, Bottom..Top at distance 1) to pixels; y grows downwards on screen (exact rectangle).
		auto screenX = [&](float t) { return ((2.0f * t - (frustum.Right + frustum.Left)) / frustumWidth + 1.0f) * 0.5f * port.Width; };
		auto screenY = [&](float t) { return (1.0f - (2.0f * t - (frustum.Top + frustum.Bottom)) / frustumHeight) * 0.5f * port.Height; };
		const float nearClip = frustum.Near * 0.5f;	// the GPU clips at z = Near; half of it keeps a margin (a smaller cut only adds)
		float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
		bool anyOnScreen = false, inside = false, emptySlots = false;
		for (UInt8 k = 0; k < pass->numLights; k++) {
			const UInt8* light = (const UInt8*)pass->lights[k];
			if (!light) {	// an empty slot: the game makes its colour black (SetLight1x2x 0x00B70820), so it adds exactly 0
				if (Exact) { emptySlots = true; continue; }
				return unusual(NoLight, k);
			}
			if (!*(const bool*)(light + 0xF4)) return unusual(NotPoint, k);
			const NiPointLight* source = *(NiPointLight* const*)(light + 0xF8);
			if (!source) return unusual(NoSource, k);
			const float radius = source->Spec.r;
			if (!(radius > 0.0f) || radius > 1e6f) return unusual(BadRadius, k);
			const float reach = radius * 1.02f + 2.0f;
			const NiPoint3& centre = source->m_worldTransform.pos;
			float lampMinX = 1e30f, lampMinY = 1e30f, lampMaxX = -1e30f, lampMaxY = -1e30f;
			if (Exact) {
				const float p[3] = { centre.x - eye.x, centre.y - eye.y, centre.z - eye.z };
				const float z = p[0] * forward[0] + p[1] * forward[1] + p[2] * forward[2];
				const float x = p[0] * right[0] + p[1] * right[1] + p[2] * right[2];
				const float y = p[0] * up[0] + p[1] * up[1] + p[2] * up[2];
				float xLo, xHi, yLo, yHi;
				if (!AxisRange(x, z, reach, nearClip, xLo, xHi) || !AxisRange(y, z, reach, nearClip, yLo, yHi)) continue;	// wholly behind the near plane
				lampMinX = screenX(xLo); lampMaxX = screenX(xHi);
				lampMinY = screenY(yHi); lampMaxY = screenY(yLo);
				if (x * x + y * y + z * z <= reach * reach) inside = true;
			}
			else {
				for (int corner = 0; corner < 8; corner++) {
					const float p[3] = { centre.x + ((corner & 1) ? reach : -reach) - eye.x, centre.y + ((corner & 2) ? reach : -reach) - eye.y,
						centre.z + ((corner & 4) ? reach : -reach) - eye.z };
					const float z = p[0] * forward[0] + p[1] * forward[1] + p[2] * forward[2];
					if (z < frustum.Near * 0.5f) { if (stats) { StatBehind += 1; StatShaded += 1; } return Draw; }	// box reaches behind the camera
					const float x = p[0] * right[0] + p[1] * right[1] + p[2] * right[2];
					const float y = p[0] * up[0] + p[1] * up[1] + p[2] * up[2];
					const float ndcX = (2.0f * x / z - (frustum.Right + frustum.Left)) / frustumWidth;
					const float ndcY = (2.0f * y / z - (frustum.Top + frustum.Bottom)) / frustumHeight;
					const float sx = (ndcX + 1.0f) * 0.5f * port.Width, sy = (1.0f - ndcY) * 0.5f * port.Height;
					lampMinX = (std::min)(lampMinX, sx); lampMaxX = (std::max)(lampMaxX, sx);
					lampMinY = (std::min)(lampMinY, sy); lampMaxY = (std::max)(lampMaxY, sy);
				}
			}
			if (lampMaxX < 0.0f || lampMaxY < 0.0f || lampMinX > (float)port.Width || lampMinY > (float)port.Height) continue;	// off screen
			anyOnScreen = true;
			minX = (std::min)(minX, lampMinX); maxX = (std::max)(maxX, lampMaxX);
			minY = (std::min)(minY, lampMinY); maxY = (std::max)(maxY, lampMaxY);
		}
		if (stats && emptySlots) StatEmpty += 1;
		if (!anyOnScreen) { if (stats) StatSkipped += 1; return Skip; }
		RECT rect;
		rect.left = (LONG)(std::max)(0.0f, floorf(minX) - 1.0f);
		rect.top = (LONG)(std::max)(0.0f, floorf(minY) - 1.0f);
		rect.right = (LONG)(std::min)((float)port.Width, ceilf(maxX) + 1.0f);
		rect.bottom = (LONG)(std::min)((float)port.Height, ceilf(maxY) + 1.0f);
		if (rect.right <= rect.left || rect.bottom <= rect.top) { if (stats) StatSkipped += 1; return Skip; }
		if (rect.left == 0 && rect.top == 0 && rect.right == (LONG)port.Width && rect.bottom == (LONG)port.Height) {	// whole screen anyway
			if (stats) { (inside ? StatInside : StatCovers) += 1; StatShaded += 1; }
			return Draw;
		}
		if (FAILED(device->GetRenderState(D3DRS_SCISSORTESTENABLE, &SavedEnable)) || FAILED(device->GetScissorRect(&SavedRect))) return unusual(StateFailed, -1);
		device->SetScissorRect(&rect);
		device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		Scissored = true;
		if (stats) {
			const double share = (double)(rect.right - rect.left) * (rect.bottom - rect.top) / ((double)port.Width * port.Height);
			StatScissored += 1;
			StatAreaShare += share;
			StatShaded += share;
		}
		return Draw;
	}

	// After the draw: the scissor state as it was (the game's render-state cache never saw the change).
	inline void After() {
		if (!Scissored) return;
		Scissored = false;
		IDirect3DDevice9* device = TheRenderManager->device;
		device->SetScissorRect(&SavedRect);
		device->SetRenderState(D3DRS_SCISSORTESTENABLE, SavedEnable);
	}
}
