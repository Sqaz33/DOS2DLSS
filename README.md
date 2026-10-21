# DOS2DLSS

> **Preview release:** download compiled files from [Releases](https://github.com/Sqaz33/DOS2DLSS/releases). Follow [the Russian installation guide](docs/release-install-ru.md).
>
> **Development build 0.6.2-dev:** fixes displaced interaction masks and missing text when starting directly in Super Resolution. A loaded 3440x1440 save was checked in Quality, Balanced, Performance and DLAA. See [the validation record](docs/text-and-outline-fix.md) for the exact scope. Existing release downloads do not include these changes yet.
>
> **Current NR behavior:** the custom Bridge bypasses mirroring when NR is off and suppresses redundant native DLSS when NR is on. The latest matched gameplay check found no measurable Bridge penalty with NR off and confirmed active NR output. Reliable reverse handover from NR on to off is still unverified.
>
> **Performance check (2026-10-22):** on RTX 3070 at 3440x1440, Quality provided a small gain, Performance about 14%, and DLAA cost about 27% versus processing Off in the tested scene. NR off showed no measurable Bridge penalty in a separate matched comparison; active NR ran around 11 FPS. A black-background defect remains in the story submenu with Quality. See [results and limits](docs/performance-0.6.2.md).

Experimental native DLSS integration for **Divinity: Original Sin 2 – Definitive Edition**.

The intended rendering path is:

1. `DOS2DLSSNative.dll` verifies the supported game executable and shares mod state.
2. `dos2-dlss.addon64` exposes controls and diagnostics in the ReShade overlay.
3. The mod creates a native NVIDIA NGX D3D11 DLSS contract.
4. Optionally, DLSS 5 Bridge can mirror that contract to D3D12.
5. RenoDX DLSS 5 can process the mirrored call when that experimental path is enabled.

DLSS5 Feeder is not part of this design.

The installer preserves NR availability by default. `-PauseNeuralRendering` is
an explicit diagnostic option for testing native DLSS separately. Previous files
and RHI selections are backed up for `scripts/uninstall.ps1`.

## Status

Milestone 0.6 runs native DLAA and Super Resolution in the game. The add-on identifies DOS2's final
scene image before `CombineUI`, reads the real D24S8 scene depth, reconstructs
camera motion in an RG16F GPU texture from the game's `PerView` matrices and
calls the official NVIDIA NGX D3D11 evaluation function. The processed scene is
fed back into `CombineUI`, so menus and HUD remain at native resolution.

Quality, Balanced and Performance reduce the viewport used by the 3D render
chain while leaving DOS2's full-size render-target pool intact. Full-screen
passes receive an adjusted `VPtoRTScaleBias`, so every pass samples the active
top-left subrectangle instead of repeatedly shrinking it. UI surfaces and their
glyph/filter caches stay at output resolution. They are identified from allocation
sites and Scaleform producers before drawing, rather than only being learned at
the final `CombineUI` binding. Scaling starts at the scene depth prepass and ends
at UI composition; per-object rectangles retain their original origin and extent.

DLSS5 DX11 Bridge recognizes feature creation and evaluation as game-supplied
DLSS, with synthesis disabled. Native D3D11 DLSS does not require the Bridge.
The NR path remains experimental and was excluded from the latest UI regression
checks. DLSS5 Feeder is not used.

The latest UI regression check used 3440x1440. Quality
renders at 2293x960 and upscales to 3440x1440. A Halton temporal-jitter sequence
is applied to DOS2's 3D projection and the same offset is passed to NGX. UI and
post-processing viewports stay anchored at (0,0), which removes the whole-screen
drift and keeps CombineUI at its original aspect ratio. The active top-left render
rectangle is copied to an exactly sized NGX input; the production D3D11 runtime
does not reliably sample a smaller subrectangle from DOS2's full-size pooled target.

Balanced and Performance use the dimensions returned by NGX and share the same
dynamic-resolution path. All modes still need broader scene testing and long
stability runs; this is not a stable release. The current build
supplies camera motion reconstructed from depth. DOS2's four-target G-buffer was
checked at shader-bytecode level: its R16G16 target stores octahedrally encoded
surface normals, not object velocity. Skinned/object motion therefore requires a
new velocity pass or a conservative current-color mask; that is the next quality
milestone.

Target game build: `3.6.117.3735`.
