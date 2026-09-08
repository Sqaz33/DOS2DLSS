# DOS2DLSS

Experimental native DLSS integration for **Divinity: Original Sin 2 – Definitive Edition**.

The intended rendering path is:

1. `DOS2DLSSNative.dll` reads DOS2 camera, object, skeleton and render state.
2. `dos2-dlss.addon64` exposes controls and diagnostics in the ReShade overlay.
3. The mod creates a native NVIDIA NGX D3D11 DLSS contract.
4. DLSS 5 Bridge mirrors that contract to D3D12.
5. RenoDX DLSS 5 processes the mirrored call.

DLSS5 Feeder is not part of this design.

## Status

Milestone 0.2 initializes the official NVIDIA NGX D3D11 runtime, checks DLSS
availability and creates a real DLSS feature contract for the selected quality
mode. DLSS5 DX11 Bridge recognizes that contract as game supplied. Evaluation is
kept disabled until the mod has identified DOS2's HUD-less scene color, depth
and motion-vector inputs, so this milestone does not alter the picture.

Target game build: `3.6.117.3735`.
