# ImageAdjust

Rewritten 2026-11-02 (wave 3, branch claude/rw3-screen, commit 4a235236). The new shaders are
src/hlsl/NewVegas/Effects/ImageAdjust.fx.hlsl (D3D9) and src/hlsl/NewVegas/Compute/ImageAdjust.frag (Vulkan). The C++ is unchanged.
The behaviour in section 6 is the same as before (as the spec describes it); the stale header is FIXED (item 3 below).


## 1. Purpose

ImageAdjust is a set of global picture controls: brightness, contrast, saturation, a strength blend, and separate tints for the darker and lighter tones. Each control has three values, for daylight outdoors, night outdoors, and indoors, and the picture moves smoothly between them as the time of day changes. At the default values the effect does nothing and is not run. Set a value and the player sees a lighter or darker, flatter or punchier, more or less saturated picture, with shadows or highlights tinted a colour the player picks.

## 2. When it runs

- **Switch:** `[_Shaders.ImageAdjust.Status] Enabled`, default **true**. Even when on, the pass is skipped when the values leave the picture unchanged (see below).
- **Frame position:** in the post chain after Cinema and before Bodycam, SkyDither and Debug (`ShaderManager.cpp`, the ImageAdjust block). It sees the output of the earlier colour effects (Coloring, LUT, Sharpening, Cinema).
- **Skip rule (`ShouldRender`):** the effect runs if any of these hold, checked in this order:
  1. Saturation is not exactly 1 (the saturation step runs regardless of Strength).
  2. Otherwise, if Strength is exactly 0, the effect does not run.
  3. Otherwise, the effect runs if brightness, contrast, or any of the six tint values is not exactly 1.
  4. Otherwise it does not run.
- **Day / night / interior blend:** the values are chosen per frame by the game state. Outdoors, each value is a linear blend between the Night value and the Main (day) value, weighted by the sky's transition curve (0 = night, 1 = day). Indoors, only the Interiors value is used. The C++ `GetTransitionValue` rule is: exterior returns `lerp(Night, Day, transitionCurve)`, interior returns `Interior`.
- **Menus and special cases:** no special rule; it runs whenever the post chain runs and the switch is on, subject to the skip rule.

## 3. Inputs

### Textures (slot order)

| Slot | Name | Holds | Filtering / addressing |
|---|---|---|---|
| s0 | `TESR_RenderedBuffer` | The picture arriving at ImageAdjust | Linear, clamp |

It is the only texture. No depth is read.

### Constants

| Name | Register | Components and meaning |
|---|---|---|
| `TESR_ImageAdjustData` | not fixed (registered by name) | x = brightness (multiplier of the tone-weighted light, at least 0); y = contrast (scale around mid-grey; 1 = unchanged); z = saturation (1 = unchanged, 0 = grey, above 1 = more colour); w = strength (0 = no change, 1 = full effect; the blend between the original and the adjusted picture) |
| `TESR_DarkAdjustColor` | not fixed (registered by name) | rgb = tint multiplier for the darker tones (1 = none). Gamma-encoded values (the shader raises them to 2.2 to work in linear light). w unused. |
| `TESR_LightAdjustColor` | not fixed (registered by name) | rgb = tint multiplier for the lighter tones (1 = none). Same encoding as the dark tint. w unused. |

Units: all dimensionless. Values are colours in 0..1 display space.

The C++ computes each of the eleven values with the day / night / interior rule in section 2. Its `UpdateConstants` writes the three constants.

## 4. Settings

All read in `ImageAdjust.cpp` (`UpdateSettings`). The same set of keys exists for each of the three sets (`Main` = day, `Night`, `Interiors`). The toml gives no range for any of them, so ranges are "not stated" (section 9). Each default is 1.0.

| Key | Default | What it changes |
|---|---|---|
| `Brightness` | 1.0 | Overall brightness. Multiplies the light-weighted part of the picture (section 6). 1 = unchanged. |
| `Saturation` | 1.0 | Colour strength. Below 1 washes towards grey, above 1 adds colour. |
| `Contrast` | 1.0 | Contrast around mid-grey. Above 1 harder, below 1 flatter. |
| `Strength` | 1.0 | Blend of the whole adjustment. 0 = off (except saturation, see section 2). |
| `DarkColorR`, `DarkColorG`, `DarkColorB` | 1.0 | Tint of the darker tones, per channel. |
| `LightColorR`, `LightColorG`, `LightColorB` | 1.0 | Tint of the lighter tones, per channel. |

Section and key names:
- `[_Shaders.ImageAdjust.Main]`: daylight outdoors.
- `[_Shaders.ImageAdjust.Night]`: night outdoors.
- `[_Shaders.ImageAdjust.Interiors]`: indoors.
- `[_Shaders.ImageAdjust.Status]` `Enabled`: bool, default true.

## 5. Passes and outputs

- **D3D9 path:** one pass, technique index 0 (the file's only technique, unnamed). Full-screen quad into the chain's output for this step. Output is 8-bit LDR at the picture's resolution. Output alpha is 1.
- **Vulkan path:** the Vulkan pass named `ImageAdjust` (SPIR-V `ImageAdjust.spv`), one full-screen pass into the same target. Same result as the D3D9 path.
- Only technique 0 is handled; other indices are not drawn.

## 6. Behaviour details

The shader works in this order on each pixel. (Written in words; the formulas are the defined behaviour.)

1. **Saturation.** The pixel is blended between its own Rec. 709 luma (grey, 0.2126 R + 0.7152 G + 0.0722 B) and its colour, with the saturation value as the blend amount. Value 1 leaves it unchanged; 0 gives grey.
2. **Linearise.** Each channel is raised to the power 2.2 (sign-preserving).
3. **Tone-weighted light.** Two weights are computed from the linear colour:
   - the darker weight is "1 minus the smooth step of the colour", i.e. high for dark colours (the smooth step is the standard cubic from 0 to 1);
   - the lighter weight is the smooth step of the colour, high for bright colours.
   Each weight multiplies the colour and its own tint (also raised to 2.2). The two parts are added. At the default tints of 1 the two parts add back to the original colour exactly.
4. **Brightness.** The sum is multiplied by brightness, clamped at 0 from below.
5. **Strength blend.** The linear colour is blended between its original value and the result of step 4, by strength.
6. **Delinearise.** Each channel is raised to 1 / 2.2.
7. **Contrast.** Each channel's distance from mid-grey (0.5) is multiplied by contrast, the mid-grey is added back, and the result is blended with the value from step 6 by strength again.

**Identity.** With every setting at its default (brightness 1, saturation 1, contrast 1, strength 1, all tints 1) the output equals the input (up to rounding of the 2.2 power). This is why the skip rule above is needed to avoid running the pass for nothing.

**Strength 0.** Only saturation can still act (section 2).

**Sky and depth.** No depth is read. The sky is adjusted like everything else, so a brightness change also brightens the sky.

**First-person weapon.** No special handling.

**RenderScale / RenderRect frames.** No special handling; the pass covers the chain's picture at its own size. The Vulkan path is off in render-rectangle frames (as for Sharpening), so the D3D9 path runs there.

**Jitter (TAA).** None; it runs on the displayed picture.

**Reversed depth.** Not used.

**HDR vs LDR.** LDR only, 8-bit chain picture. The operations are defined on display values in 0..1.

**NaN and inf.** Brightness is clamped at 0. The power functions use the sign-preserving form so negative channels (from the tint or the contrast step) do not produce NaN. Values above 1 are not clamped at the end; the 8-bit target clips them on write. A value that goes below 0 after the contrast step is also clipped on write. No other guard is needed.

**Easily missed:** the dark and light tints must be applied in linear light (raised to 2.2) or the tint looks wrong; the delinearise step must come after the brightness and strength blend and before the contrast step; the day / night blend must use the transition curve, not a hard switch at dusk; interiors must use only the Interiors values.

## 7. Vulkan path

- **Files:** `src/hlsl/NewVegas/Compute/ImageAdjust.frag` (SPIR-V `ImageAdjust.spv`).
- **Selection:** `ImageAdjustEffect::ExtRender` is used only when Vegas Shaders DXVK is loaded, `[Main.Main.Performance] NvrDxvkPasses` is on, and the frame is not a render-rectangle frame. Technique 0 only. If the pass fails, the D3D9 technique runs for the rest of the session.
- **Bindings (descriptor set 1):** binding 0 = picture (`t_rendered`), one sampled 2D texture.
- **Sampler:** the linear clamp sampler, taken from the push block at offset 48.
- **Push constants (64 bytes):**

| Offset | Type | Field |
|---|---|---|
| 0 | vec4 | `TESR_ImageAdjustData` (x brightness, y contrast, z saturation, w strength) |
| 16 | vec4 | `TESR_DarkAdjustColor` |
| 32 | vec4 | `TESR_LightAdjustColor` |
| 48 | uint | linear clamp sampler index |
| 52 to 63 | uint x 3 | padding |

- The Vulkan shader's steps match section 6. The one visible difference from the D3D9 path is expected to be rounding only.

## 8. Look and cost

- **Correct:** at default settings nothing changes. A brightness change lifts or lowers the whole picture without clipping the highlights hard; a contrast change makes the picture punchier or flatter around mid-grey; a saturation change makes the colour richer or more grey; a tint change colours only the shadows or only the highlights. Night and interior values take effect at the right times with a smooth change at dusk and dawn.
- **Typical wrong result:** a flat grey look when saturation is below 1 (expected, but check the value); crushed black or clipped white from large brightness or contrast values; a colour shift in the shadows that looks like a bug (the dark tint has been set, not a fault); an obvious pop at dusk or dawn (the day / night blend broken); tints that look too weak (the tint applied in display light instead of linear).
- **Cost class:** cheap single pass. One texture read and some power and smooth-step arithmetic per pixel. The power functions are the main cost.

## 9. Open questions

1. **Ranges not stated.** The toml has no range comment for any of the eleven values. The ImGui menu may impose ranges; not checked. Decide the valid ranges for brightness (at least 0 is enforced only for brightness), saturation (negative values are not guarded) and contrast.
2. **Saturation below 0 or above 1.** Not guarded. Negative saturation inverts the colour; large saturation overshoots and clips. Decide whether to clamp.
3. **Stale header comment (FIXED 2026-11-02).** The new shader's header describes New Vegas and its order of operations.
4. **Output not clamped.** Values above 1 or below 0 are only clipped on write. Confirm that is acceptable.
5. **Interior tint and brightness interaction.** The interiors and night values are separate, so setting brightness in one place does not affect the others. Confirm this is wanted and document it for users.
6. **Transition curve.** The exact meaning of `transitionCurve` (0 at night, 1 at full day, and the shape of the change over the day) is held in the game state, not in this effect. Not traced in this spec; the new author should read `ShaderManager` for its definition.
