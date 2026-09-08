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

Milestone 0.3 runs native DLAA in the game. The add-on identifies DOS2's final
scene image before `CombineUI`, reads the real D24S8 scene depth, reconstructs
camera motion in an RG16F GPU texture from the game's `PerView` matrices and
calls the official NVIDIA NGX D3D11 evaluation function. The processed scene is
fed back into `CombineUI`, so menus and HUD remain at native resolution.

DLSS5 DX11 Bridge recognizes both feature creation and every evaluation as
game-supplied DLSS, with synthesis disabled. RenoDX DLSS 5 can therefore be
enabled from its existing ReShade tab. DLSS5 Feeder is not used.

DLAA is the validated mode. Quality, Balanced and Performance already create
the correct NGX feature sizes, but evaluation remains gated until DOS2's scene
render targets are safely decoupled from the swap-chain resolution.

Target game build: `3.6.117.3735`.
