# LocalExposure (each part of the picture exposed for its own brightness)

## 1. Purpose
In harsh sun the shade next to sunlit sand goes black, and from a dark room a doorway burns white; the eye sees into both.
Local exposure does what the eye (and Unreal Engine 5's Local Exposure) does: it measures how bright each neighbourhood of the
picture is and pulls each one toward the view's own mean brightness, so dark areas are lifted and bright ones calmed, without
flattening the whole picture. Off by default.

## 2. When it runs
- [Shaders.LocalExposure.Status] Enabled (default false), and Interiors / Exteriors for where.
- Each frame, in the chain step LumaExposureBloom (core/EffectChain.cpp), right after the exposure pass: it measures the image the
  tone mapper is about to get. It does not change the image itself; the tone mapper reads its result.
- Skipped (the tone mapper skips too) when Shadows and Highlights are both 0 and DebugView is off.

## 3. Passes (Effects\LocalExposure.fx.hlsl)
| Technique | Into | What |
|---|---|---|
| 0 | TESR_LocalExposureBuffer (1/16 res, R16F) | log2 brightness: 16 bilinear taps per texel, decoded with the tone mapper's Linearization |
| 1 | TESR_LocalExposureTemp | horizontal Gaussian (sigma = Blur texels, 13 taps) |
| 2 | TESR_LocalExposureBuffer | vertical Gaussian |
| 3 | TESR_LocalExposureMean (1x1) | the mean of the buffer (8 x 8 reads) |

## 4. In the tone mapper (Shaders\Includes\Tonemapping.hlsl localExposure, both ISHDRBLENDINSHADERCIN shaders)
After Exposure and before the night colour shift and the curve: `stops = mean - local`; a darker neighbourhood gains
`stops * Shadows`, a brighter one loses `stops * Highlights`; that shift fades where the pixel itself is more than a stop off its
neighbourhood (EdgeGuard; a plain blur would leave a halo round the sky or a lamp); at most 3 stops. Constants c34 / c35,
samplers s9 / s10. TESR_LocalExposureData.w = 1 only on frames the buffer was drawn (else the tone mapper skips).

## 5. Settings ([Shaders.LocalExposure.Main])
Shadows 0.4, Highlights 0.3, EdgeGuard 1.0, Blur 4 (buffer texels; about a tenth of the screen), Interiors, Exteriors, DebugView
(1: the measured brightness as grey).

## 6. RenderScale (FSR / DLSS below 1)
The buffer is screen-shaped, so in a rect frame it is drawn in its top-left part like every effect buffer; reads stay inside it,
and RenderRect::RescaleAfterUpscale stretches it over the whole buffer before the tone mapper reads it (as Bloom's top level).

## 7. Tested
2026-11-04 bench: og-desert noon (the dark foreground lifted, the sky calmed, near-black pixels 0.9% -> 0.4%, no halo at the
ridge; strong settings flatten, as expected) and saloon16 (dark corners lifted). Not yet: RenderScale below 1, the DXVK fork.
