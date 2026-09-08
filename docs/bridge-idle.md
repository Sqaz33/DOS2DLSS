# Bridge cost with NR disabled

The installed bridge configuration was stage=3, source=auto, skip_game=0.
The 2026-09-08 log records continued D3D12 frame delivery/evaluation while native DLSS is used. The NR checkbox does not disable bridge mirroring.

Set stage=0 in the installed dlss5-bridge.cfg to disable bridge processing while retaining both ReShade panels. Restore stage=3 when NR processing is wanted. The install script preserves stage. This is a manual bridge switch, not automatic synchronization with the NR checkbox.
No game launch or FPS comparison was performed for this configuration change.

Superseded by nr-aware-bridge.md: the custom Bridge automatically bypasses the mirror when NR is disabled; stage=3 can remain set.
