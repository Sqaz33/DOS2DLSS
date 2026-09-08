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

Research is complete enough to begin implementation. The first executable milestone is a safe diagnostic build that identifies the game and records D3D11 render passes without changing the picture.

Target game build: `3.6.117.3735`.

