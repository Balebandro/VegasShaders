# Debug

Rewritten 2026-11-02 (wave 3, branch claude/rw3-screen, commit a47cd42b). New shader: src/hlsl/NewVegas/Effects/Debug.fx.hlsl (D3D9 only, two
techniques: the overlay (unnamed) and ClipView). The C++ is unchanged. There is still no Vulkan pass for this effect.
FIXED: a light with radius zero gives no glow instead of dividing by zero (section 6, NaN and inf).
REMOVED: TESR_ShadowRadius (declared, never read) is no longer declared (section 9, item 2).
NEW OPEN (from the rewrite): the average-luminance swatch reads the red channel of the average-luma buffer; the spec does not name
the channel (DepthOfField reads blue for focus), so this is a guess to confirm. The light marker keeps a constant angular size
(cos above 1 - 4e-6); the spec phrase "closer for lights far away" is not implemented as a size rule.


Two separate things live in this effect and must be specified separately:
- **The Debug overlay** (the main technique, switch `[_Shaders.Debug.Status] Enabled`): diagnostic panels drawn over the picture.
- **The clipping view** (the `ClipView` technique, switch `[_Shaders.Debug.Main] ClipView`): marks clipped colour channels. It works even when the overlay is off.

## 1. Purpose

Debug is a developer and fault-finding tool. With the overlay on, the player (or tester) sees a row of small panels across the top and a column of colour swatches down the right side: the normals buffer, the depth buffer, the shadow buffer, the bloom buffer, the SMAA edge and blend buffers, the sun, ambient, fog, horizon, sky and water colours, the average brightness, and the sun and light directions. Light sources are marked on the picture as coloured spots and glows. With the clipping view on, pixels that have lost colour to a clipped (full-brightness) channel are marked with stripes, so the tester can see where detail or colour is lost and whether the tonemapper or a colour effect caused it. Neither is meant for normal play; both are off by default.

## 2. When it runs

- **Overlay switch:** `[_Shaders.Debug.Status] Enabled`, default **false**. When off, the overlay technique is not drawn.
- **Clipping view:** `[_Shaders.Debug.Main] ClipView`, default 0. Values 1, 2 and 3 turn on a mode (section 6). The view is drawn whether or not the overlay is on. Changes apply immediately.
- **Average brightness dependency:** when the overlay is on, `ShaderManager` also forces the screen's average-luminance measurement on (it is needed for the average-luma swatch). That is a side effect the new code must keep.
- **Frame position:**
  1. At the start of the post chain (straight out of the tonemapper, before the first post effect), the C++ `CaptureClipStart` copies the game target into a helper texture, but only when ClipView is 2 or 3 (section 6).
  2. Late in the chain, after Coloring, the LUT, Cinema, ImageAdjust, Bodycam and SkyDither (and the sun-depth debug view, off by default), the overlay (technique 0) is drawn into the chain target.
  3. Immediately after the overlay, the clipping view (technique `ClipView`) is drawn into the same target. When the clipping view is active, Debug is named as the chain's last effect so its output writes the game target directly (the clipping view then covers everything, as the last layer).
- **Menus and interiors:** no special rule. The panels draw wherever the post chain runs.

## 3. Inputs

### Textures (slot order; Vegas Shaders binds samplers by declaration order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | The picture at the Debug step | Linear, clamp |
| s1 | `TESR_SourceBuffer` | The chain's source buffer (declared; the overlay does not read it) | Linear, clamp |
| s2 | `TESR_DepthBuffer` | CombineDepth's combined depth (linear depth over far distance, first channel) | Linear, clamp |
| s3 | `TESR_NormalsBuffer` | The view-space normals buffer | Point, clamp |
| s4 | `TESR_AvgLumaBuffer` | The average-luminance buffer (read at its centre for the average swatch) | Point, clamp |
| s5 | `TESR_PointShadowBuffer` | The point-light shadow buffer (red and green channels read) | Point, clamp |
| s6 | `TESR_BloomBuffer` | The bloom buffer | Point, clamp |
| s7 | `TESR_SMAA_Edges` | SMAA's edge buffer | Linear, clamp |
| s8 | `TESR_SMAA_Blend` | SMAA's blend-weight buffer | Linear, clamp |
| s9 | `TESR_ClipStartBuffer` | Clipping view only: a copy of the picture as it came out of the tonemapper (see section 6). Declared last, so it stays last. | Point, clamp |

The overlay's samplers s0 to s8 must stay in this declaration order; the C++ binds by it. The clipping-view sampler at s9 is registered by name and created on first use (section 6).

### Constants

| Name | Components and meaning |
|---|---|
| `TESR_SunAmbient` | Sun ambient colour (rgb) |
| `TESR_SunColor` | Sun colour (rgb) |
| `TESR_FogColor` | Fog colour (rgb) |
| `TESR_WaterShallowColor` | Water shallow colour used by the game |
| `TESR_WaterDeepColor` | Water deep colour used by the game |
| `TESR_HorizonColor` | Horizon colour (rgb) |
| `TESR_SkyColor` | Sky colour (rgb) |
| `TESR_ViewSpaceLightDir` | Light direction in view space (xyz) |
| `TESR_SunDirection` | Sun direction (xyz) |
| `TESR_DebugVar` | The four developer values from `[Main.Develop.Main]` (`DebugVar1` to `DebugVar4`, x y z w). **Declared, but the overlay does not read it.** Kept for shader experiments. |
| `TESR_ShadowRadius` | Declared; the code that used it is commented out. Not read. |
| `TESR_LightPosition[12]` | Twelve light positions: xyz = world position, w = light radius |
| `TESR_ShadowLightPosition[12]` | Twelve shadow-casting light positions, same layout |
| `TESR_ClipView` | x = clipping view mode (1, 2 or 3; 0 is never drawn) |
| `TESR_ReciprocalResolution` | xy = 1 divided by picture size (used by the clipping view) |
| `TESR_CameraPosition`, `TESR_CameraData` | Used through the helpers for world-position reconstruction and far-distance scaling |

The C++ fills the colour and light constants from the game through the shared registrations; this effect's own C++ registers only `TESR_DebugVar` and `TESR_ClipView`.

## 4. Settings

| Section | Key | Type | Default | What it changes |
|---|---|---|---|---|
| `[_Shaders.Debug.Status]` | `Enabled` | bool | false | Turns the overlay on or off. |
| `[_Shaders.Debug.Main]` | `ClipView` | int | 0 | Clipping view mode: 0 off, 1 finished picture, 2 tonemapper output, 3 who clipped it. Any value outside 0 to 3 is treated as 0. |
| `[_Main.Develop.Main]` | `DebugVar1`, `DebugVar2`, `DebugVar3`, `DebugVar4` | float | 0.0 | Developer values sent as `TESR_DebugVar`. Not read by the overlay (section 3). |

The `Main.Develop` section also holds unrelated developer keys (debug mode, shader trace, GPU labels, RenderDoc key). They are not used by this effect.

## 5. Passes and outputs

- **Overlay (technique 0, unnamed):** one full-screen pass drawing the picture with the panels and markers on top. Writes the chain target, 8-bit LDR, output alpha 1.
- **Clipping view (technique `ClipView`):** one full-screen pass. Writes the chain target. It is drawn by `RenderClipView` after the overlay. That call switches the effect on for that one call, so the clipping view does not need the overlay switch.
- The overlay and clipping view both use the D3D9 path only (see section 7).

## 6. Behaviour details

**Overlay layout (UV space, origin top-left).** Each panel shows its source buffer scaled into a box; pixels outside the box are left as the picture.

| Panel | Box (x, y) | Size | Shows |
|---|---|---|---|
| Normals | 0.10, 0.05 | 0.15 x 0.15 | Normals buffer, as stored |
| Depth | 0.30, 0.05 | 0.15 x 0.15 | Depth as a grey value: linear depth divided by far distance, raised to the power 0.3 (near = dark, far = light) |
| Shadows | 0.50, 0.05 | 0.15 x 0.15 | Point shadow buffer: red x 0.5 plus green |
| Bloom | 0.70, 0.05 | 0.15 x 0.15 | Bloom buffer, as stored |
| SMAA edges | 0.10, 0.25 | 0.15 x 0.15 | SMAA edge buffer |
| SMAA blend | 0.30, 0.25 | 0.15 x 0.15 | SMAA blend buffer |

**Colour swatches (right edge, x 0.90 to 0.95).** Each band is a solid colour, the raw value of one constant. These are not gamma-corrected; the value is written as it is.

| y band | Shows |
|---|---|
| 0.15 to 0.20 | Sun colour |
| 0.20 to 0.25 | Sun ambient |
| 0.25 to 0.30 | Fog colour |
| 0.30 to 0.35 | Horizon colour |
| 0.35 to 0.40 | Sky colour |
| 0.40 to 0.45 | Water shallow colour |
| 0.45 to 0.50 | Water deep colour |
| 0.55 to 0.60 | Average luminance (sampled at the centre of the average-luma buffer), as grey |
| 0.65 to 0.70 | View-space light direction, as a colour (values outside 0 to 1 clip on write) |
| 0.70 to 0.75 | Sun direction, same rule |

**Light markers.** Each light is reconstructed in the world from the depth of each pixel. For each light the shader does two things:
- If the view direction from the camera points at the light (within a very small angle, closer for lights far away), the pixel is painted solid in the marker colour. This marks the light's position.
- Otherwise the pixel gets a soft glow round the light's world position, scaled by the light's radius (its w value) and faded to nothing by a rational falloff, added at one fifth of the marker colour.

The 12 shadow-casting lights (`TESR_ShadowLightPosition`) use green for the first seven and blue for the rest. The 12 ordinary lights (`TESR_LightPosition`) use red. Lights are drawn in that order, so red sits above green and blue.

**Depth from the overlay.** The depth panel and the light markers read depth from CombineDepth; sky pixels are at the far end and show as light grey in the depth panel.

**Clipping view: what counts as clipped.** The post chain's buffers are 8 bits per channel, so a channel is clipped when it reaches 254.5 / 255 or above. A pixel with one or two clipped channels has its colour shifted (for example orange turns yellow); a pixel with all three clipped is burnt to white.

**Clipping view: output.** Unmarked pixels are shown dimmed: 25 percent colour and 75 percent luma, times 0.4 (dark and nearly grey). Marked pixels are drawn in a flat colour with a diagonal stripe: pixels whose position sum (x + y, in whole pixels) falls in the first half of each 8-pixel period take the full colour, the rest take 60 percent of it.

| Mode | Marked as |
|---|---|
| 1 (finished picture) | Magenta where one or two channels are clipped in the final picture; cyan where all three are clipped. |
| 2 (tonemapper output) | The same marks, using the copy taken at the start of the chain (the picture as the tonemapper gave it). |
| 3 (who clipped it) | Red where the tonemapper output is already clipped (too much light: the flashlight, sun, exposure or tonemapper settings). Yellow where it is clipped only by the colour effects after it (any channel clipped in the final picture but not in the tonemapper output). Red takes priority over yellow. |

Colour values: magenta (1, 0, 1), cyan (0, 1, 1), red (1, 0, 0), yellow (1, 1, 0).

**Clipping view: the start copy (modes 2 and 3).** At the start of the post chain, the picture in the game target is copied into a helper texture (`TESR_ClipStartBuffer`) of the same size and format. The helper is created on first use and re-created if the target size or format changes. If creating it fails, the modes 2 and 3 are unavailable and the failure is logged once. The copy is taken only when mode 2 or 3 is selected. When the mode is changed to 0 or 1 the helper is released.

**Clipping view: when it draws.** It draws only when the mode is not 0, a clipping-view technique exists in the loaded file, and (for modes 2 and 3) the start copy exists. When active, Debug is named as the chain's last effect, so the clipping view is the last layer over the whole frame (after the sky dither and after any debug overlay), and it covers every other effect's result.

**First-person weapon.** No special handling.

**Sky.** The depth panel shows the sky as the far end. No other special handling.

**RenderScale / RenderRect frames.** The overlay and the clipping view use the full target. No special handling.

**Jitter (TAA).** None.

**Reversed depth.** Not used; depth is the Vegas Shaders combined depth.

**HDR vs LDR.** LDR only (8 bits). The clipping view's whole purpose is the 8-bit limit; it is meaningless for an HDR chain.

**NaN and inf.** The depth panel's power and the light falloff are written to avoid divide by zero within their ranges, but the light falloff divides by the light's radius (its w value). A light with radius 0 gives a divide by zero. The swatches write raw values, so negative or large values are clipped on write.

**Things that would look wrong if missed:** the overlay's samplers out of order (the panels show the wrong buffer); the average-luma measurement not forced on when the overlay is on (the average swatch is wrong); the clipping view marking everything because the clip level is taken in the wrong range; the start copy missing so modes 2 and 3 show the final picture.

## 7. Vulkan path

- **None.** `DebugEffect` does not override the Vulkan entry point, so it always runs as a D3D9 technique, even when `[Main.Main.Performance] NvrDxvkPasses` is on.
- The clipping view's start copy and draw use the D3D9 device calls directly (a copy of the target, then the technique). They do not use the Vulkan extension.
- Any new Vulkan version would need its own pass and the same push-constant layout for the overlay constants and for `TESR_ClipView`. Nothing of that exists today.

## 8. Look and cost

- **Overlay correct:** the panels show clean, undistorted copies of their buffers in their boxes, with the colour swatches on the right and the light spots on the picture. Standing still should keep the panels steady. Lights mark where the game thinks they are.
- **Overlay typical wrong result:** every panel showing the same buffer (samplers bound in the wrong order); a black depth panel (the far scale wrong); light spots in the wrong places (the light positions or the depth reconstruction wrong); the average swatch stuck (the average-luma measurement off).
- **Clipping view correct:** a dim, nearly grey picture with coloured stripes only where a channel is at its maximum. Mode 3 separates the tonemapper's clipping (red) from the colour effects' clipping (yellow). Standing still should not change the marks.
- **Clipping view typical wrong result:** the whole picture stripped (the clip level too low); no marks in bright scenes (the level too high, or the start copy missing in mode 2 or 3); mode 3 showing everything yellow (the start copy not taken at the chain start).
- **Cost class:** the overlay is one pass with about nine buffer reads, a short loop over 24 lights (each with a few vector operations), and a depth read. Moderately heavy but only while on. The clipping view is one pass with one or two reads, and the start copy is one full-target copy, only in modes 2 and 3.

## 9. Open questions

1. **`TESR_DebugVar` is never read.** The four `DebugVar` settings in `[_Main.Develop.Main]` reach the shader but the overlay ignores them. Decide whether they are for future experiments only; if so, say so in the spec.
2. **`TESR_ShadowRadius` (REMOVED 2026-11-02).** It was declared and never read; the rewrite no longer declares it.
3. **Light radius zero (FIXED 2026-11-02).** A zero radius gives no glow (the falloff is guarded).
4. **Clip level.** The clip threshold is 254.5 / 255. Confirm that matches what testers mean by "clipped" (a channel at full brightness, not one just below).
5. **The swatches are raw, not gamma-corrected.** Sun, fog and sky colours are shown exactly as stored; the new version should say whether the swatches should match what the player sees.
6. **The overlay and the clipping view both use the D3D9 path only.** Confirm that is intended for a developer tool.
7. **Panel layout is fixed in the shader.** The panel positions are hard-coded fractions of the screen; a different screen aspect changes the panel shape. Confirm that is acceptable.
8. **Ranges not stated** for `ClipView` (it accepts only 0 to 3; any other value is set to 0) and for `DebugVar1` to `DebugVar4` (no range given).
