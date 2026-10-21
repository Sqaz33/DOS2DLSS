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
