# Text and interaction masks — 0.6.2-dev

Checked on 2026-10-22 with DOS2 Definitive Edition 3.6.117.3735, DX11,
ReShade 6.8, native NGX, 3440x1440. Bridge and RenoDX NR were paused for these
checks, then restored. This record supersedes older claims about these UI defects.

## Reproduction and cause

Starting directly in Quality produced empty warning dialogs and object HP labels.
Starting in DLAA and then switching to Quality preserved readable text. This
identified corruption of initial glyph/filter cache production, before final UI
composition, rather than a missing localization file or an NGX output failure.

Scaleform creates screen-sized temporary color/depth surfaces. The engine also
shares the scene depth/stencil surface while rendering UI. Scaling every target
with screen dimensions, or starting scaling at any full-size depth attachment,
therefore changes glyph-cache rasterization and filtering. Starting only at the
G-buffer protects text but leaves the preceding depth prepass inconsistent,
which makes opaque world geometry disappear.

## Implementation

- The 3D phase opens on depth-only scene geometry, or the validated four-target
  G-buffer. It ends at CombineUI/present. UI/filter work before it is untouched.
- Scaleform vertex shaders BD26BCCCC2592798, 29309D28C1B99C79 and
  BB1B5991680FE117 identify producers before drawing. Their temporary targets
  remain native-sized. UI allocation sites and identity/zero-depth UI views
  provide additional identification. Destroyed resources lose cached metadata.
- Scene raster overrides preserve partial viewport/scissor origins and restore
  the original state after each draw. Outline blur UVs and pixel-shader clamps
  use the same scale. Partial ClearRT compute rectangles follow that scale.
- NGX creation/evaluation uses a private D3D11.1 context state, restored before
  the engine resumes. Native UI draws bypass manual re-submission.

## Actual checks

| Check | Result |
|---|---|
| Fresh process starting in Quality, then load existing Reaper's Coast save | Warning text and Accept caption readable; terrain and characters visible |
| Hover dead farmer in Quality | Name, 0/1182 HP, level 14 and status text readable; white outline aligned |
| Switch to Balanced (1995x835) and Performance (1720x720) | Same text, outline and native minimap preserved; NGX evaluation succeeds |
| Switch to DLAA (3440x1440), Off and back to Quality (2293x960) | Scene/UI remain visible; no crash during this short sequence |
| Open inventory after mode switching | Character stats, headings, item counts and currency readable |
| Move off the highlighted corpse | White outline disappears without a displaced residual silhouette |
| Installer replacement with an existing read handle sharing delete | Replacement succeeds; original bytes preserved in backup |

Evidence is retained locally under ignored `artifacts/`, including
`depth-only-ready.png`, `quality-hover-verified.png`, `mode-*-verified.png`,
`quality-inventory-verified.png` and matching status snapshots. Screenshots come
from the game's ReShade runtime at 3440x1440, not a combined desktop capture.

## Limits

Only one saved location and a short mode-switch sequence were checked. This does
not establish long-session stability, an FPS improvement, complete object motion
vectors, or NR compatibility in every scene. Character motion vectors remain
unimplemented. RenderDoc initialization makes NGX unavailable here; captures
were used to isolate scaling and inspect passes, not to validate DLSS itself.

The now-readable story/addon configuration warning is separate from rendering.
No story-mod repair or manual save-file changes were performed.
