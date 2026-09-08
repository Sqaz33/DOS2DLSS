# NR-aware Bridge for DOS2 — 2026-09-08

Supersedes the temporary stage=0 workaround in bridge-idle.md.

The DOS2-specific build polls ReShade's in-memory RenoDX.DLSS5/NeuralUplift setting at most every 100 ms. With NR off or the NR addon absent, it forwards the native D3D11 DLSS call and returns before opening or running the D3D12 mirror. Both panels remain registered. With NR on, the mirror runs. skip_game=1 suppresses the redundant native evaluation only after the upstream bridge reports a healthy delivered frame. Native history and bridge readiness are reset across handover.

Configuration: stage=3, skip_game=1, synth=0. NR is currently off and may be enabled from RenoDX's panel. This eliminates redundant evaluation, not NR's inference or the required DX11/DX12 transport cost. No zero-cost NR claim is made.

Rebuild: scripts/build-nr-bridge.ps1 applies the tracked include and small source edits to the existing third_party/dlss5_bridge_src checkout, then builds build/dos2-nr-bridge.addon64. It requires the reviewed 1.4.13-pre3 source already present locally and Visual Studio C++ tools. scripts/install.ps1 installs this build when available. RHI replacing the Bridge binary with an upstream version will remove the DOS2-specific gate.

Validation:
- C++ optimized build passed; PowerShell syntax and git diff checks passed.
- NR off + DLAA: native_dlss_active=1, advancing successful evaluations; log explicitly records mirror bypass, with no delivered bridge frame.
- NR on + DLAA (separate launch): bridge delivered 3440x1440; evaluates #2 through #5 report the native evaluation suppressed. ReShade reported successful inline NR feature 18 evaluation (count=60). Screenshot artifacts/nr-gate-on.png shows the scene.
- The final module-presence and native-history reset refinements were rebuilt and checked with NR off on another launch.
- These are short main-menu checks. Live checkbox handover, saved-game performance, and an FPS A/B benchmark remain unverified. Settings are read through the official ReShadeGetConfigValue API; actual UI update timing depends on RenoDX publishing the setting.
- Game closed after tests. Original binary backed up in game bin/DOS2DLSS-Backup/bridge-before-nr-gate.addon64.

API reference: https://github.com/crosire/reshade/blob/main/include/reshade.hpp
Upstream: https://github.com/NIGos/dlss5-bridge
