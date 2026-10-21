# DOS2DLSS stabilization — 2026-10-21

- Repository restored from Sqaz33/DOS2DLSS, baseline f4f2049, local work/DOS2DLSS.
- Reinstalled game still has old addons/configs; original Bink was restored, disabling NativeModLoader. EoCApp SHA256 still matches supported 3.6.117.3735. Engine is native DX11; um scan incorrectly identifies managed launcher utilities as the engine.
- Saves backup: C:/Users/mset6/.universal-modder/backups/dos2-before-stabilization/20261021-232329.zip.
- Replaced-file backup: artifacts/game-before-20261021. No save edits or new quicksaves.
- Native loader/addon reinstalled; Bridge and RenoDX temporarily paused for baseline native tests. Old d3dcompiler backup survived reinstall alongside a restored compiler; installer now preserves both and proceeds.
- Loaded existing sdf save through Continue; baseline reproduced missing HP text and displaced white corpse silhouette at 3440x1440, Quality. Evidence: artifacts/baseline-loaded.png, baseline-hover.trace.
- Actual outline chain: mask pass -> EoCOutlineBlurH VS A24881A89C311963 -> BlurV VS C168C0D066FEB718 (PS 55D5D37A212C2405) -> Combine PS 37E45D1D09238727. Blur VS hashes were absent from fullscreen classification.
- ClearRT compute B821CC2B8D8E719D uses float4 Color then uint4 Viewport (width,height,x,y) and 32x32 groups. Original partial clears and scaled drawing currently disagree.
- Baseline PID 26748 closed. Next: scope constant overrides, preserve viewport/scissor origins, synchronize outline UV and partial clears; build and verify in the same save.

Progress:
- eaad075 committed reinstall recovery for the game-local compiler.
- Updated SR transforms to preserve viewport/scissor origins; added missing outline blur VS hashes and PS clamp correction; scaled ClearRT partial clears consistently. User confirmed silhouettes now align with objects.
- Scoped per-draw VS/PS globals and camera overrides; isolated NGX feature creation/evaluation with D3D11.1 context-state swapping and ignored addon-internal binding events.
- Text is still absent in some Scaleform windows/HP labels. Do not mark fixed. RenderDoc capture in progress to locate the actual text draw.

Font investigation:
- RenderDoc under NGX returns 0xBAD00002, so its unscaled captures are controls, not proof of native DLSS correctness.
- Added status --probe-scale W H (only allowed when NGX is unavailable) to reproduce the scale transform separately. Capture scaled-ui_frame5169.rdc; high-severity validation messages absent in the control capture. Large captures stay ignored.
- No broad shader-name search yielded a font symbol. Narrowed by startup A/B: starting in DLAA creates readable modal text; switching to Quality preserves it. Starting immediately in Quality corrupts initial text-cache production.
- Early UI allocation paths and 2D matrix/shader classification alone did not fix first-Quality startup. Kept explicit UI passthrough and limited UV changes to scene-sized inputs.
- New scene phase starts at scene depth prepass/MRT G-buffer and ends at CombineUI/present. Excludes pre-scene UI/cache work. Initial MRT-only boundary made text readable but missed depth prepass, so expanded to 3D full-size depth draws. Verification in progress.
- Game emits a pre-existing story/addon-settings warning after reinstall; it became readable in DLAA. Do not modify story mods or saves as part of a rendering fix.

Verified on 2026-10-22:
- Actual scene depth prepass is depth-only (capture pass 111); G-buffer follows at pass 112. Broad full-size DSV detection accidentally reopened scaling inside Scaleform, corrupting initial text. Restricted phase opening to depth-only geometry, preserving matching jitter/rectangle for depth and G-buffer.
- Font/filter producer BB1B5991680FE117 writes full-size temporary targets (allocation 1EF2087); included alongside the two existing Scaleform shaders and learned its targets before skipping UI draws.
- Fresh-process Quality -> existing sdf/Reaper's Coast save: warning and Accept caption readable, world geometry restored. Hover farmer: name, 0/1182, level14 and status text visible; correct outline. Mode switches Balanced/Performance/DLAA/Off/Quality preserved text/minimap, NGX success. Newly opened inventory text and item counts readable. Evidence listed in docs/text-and-outline-fix.md.
- Profile Debug_Client_Profile_1 displayed stale menu labels after switching; restarting from game working directory restored sdf and enabled Continue. No profile binary edits were needed.
- Installer read-handle replacement test passed with original backup intact. Diagnostic probe-scale is forbidden when NGX is initialized.
- Test PID 29500 closed. Native UI baseline was tested with NR paused; final installation restores the previous NR addon files. No FPS or NR improvement is claimed.

Performance/artifact check, 2026-10-22:
- User explicitly left the game foreground for measurement after the first attempts were interrupted by Chrome. Added scripts/benchmark.ps1: Present counter / monotonic elapsed time, foreground guard, reverse rounds, CSV/status output, no frame-percentile claim. PresentMon ETW denied without Windows admin/Performance Log Users access; no UAC or membership changes made.
- RTX3070/617.14, 3440x1440, NR off, VSync temporarily off in game and ReLimiter, Diagnostics0. nr-off-full two-round averages: Off134.9, DLAA98.0, Quality138.3, Balanced143.7, Performance153.5 FPS. Small Quality gain; no large performance improvement claim.
- Matched original-save location: Bridge absent Off122.9/Quality133.0 versus restored Bridge+RenoDX, NR off, Off123.1/Quality133.2. Open-warning run bridge-gated excluded; bridge-gated-clear used. Normal bypass log confirms no mirror with NR off.
- NR on Quality11.22 FPS; overlay ACTIVE/NR INJECTED, successful NR frames, GPU99%, ~6.6GB VRAM. Reverse-toggle attempt never confirmed NR off, so its 10.1FPS result is NOT an NR-off measurement. Live reverse handover still unverified.
- ~7s/60fps game-window recordings each Off/DLAA/Quality: no full-black frames. UI/second-object outline retained. No broad claim about foliage or animated-object motion vectors.
- Found story submenu Quality black-background/upper-left-subrectangle defect with no NR addons, auto Debug profile. Off correct. Evidence story-mode-0/2.png, tracked as unresolved rather than silently declared fixed.
- Test PID1536 terminated and waited for exit; original four config files restored, NR addon files restored, game closed. Details in docs/performance-0.6.2.md. No render algorithm edits in this verification session.
