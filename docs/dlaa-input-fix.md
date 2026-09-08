# DLAA input and lifetime fix — 2026-09-08

Changes:
- Acquire the actual bound scene SRV through PSGetShaderResources, rather than AddRef on a cached raw descriptor which may be stale after resource destruction.
- Save and restore scene SRV slot 0 around CombineUI. The temporary DLSS output binding must not leak into the next game draw or frame.
- Copy DLAA color to an independent input texture, as with the upscaling modes.
- Exclude fullscreen utility shaders from DLAA camera jitter.

Validation:
- Build and installation succeeded; git diff --check passed.
- Seven menu mode transitions (DLAA, Quality, DLAA, Balanced, DLAA, Off, DLAA) completed with successful NGX evaluations and a responsive process.
- Three later ReShade screenshots explicitly verified quality_mode=1 and native_dlss_active=1; evaluation count advanced 12618 -> 12759 -> 12894. All three show the rendered scene, without a black scene. Artifacts: artifacts/dlaa-active-{1,2,3}.png.
- Sparse screenshots do not exclude intermittent flicker. No loaded-save gameplay or level-transition crash validation was completed in this short test. The changes remove concrete lifetime/state hazards; they do not establish the cause of every reported crash.
- Game closed after testing.
