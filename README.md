# DOS2DLSS

Experimental native DLSS integration for **Divinity: Original Sin 2 – Definitive Edition**.

The intended rendering path is:

1. `DOS2DLSSNative.dll` reads DOS2 camera, object, skeleton and render state.
2. `dos2-dlss.addon64` exposes controls and diagnostics in the ReShade overlay.
3. The mod creates a native NVIDIA NGX D3D11 DLSS contract.
4. Optionally, DLSS 5 Bridge can mirror that contract to D3D12.
5. RenoDX DLSS 5 can process the mirrored call when that experimental path is enabled.

DLSS5 Feeder is not part of this design.

The installer pauses the DLSS5 Bridge and RenoDX DLSS5 entries for this game so
RHI does not silently restore the expensive mirror on the next launch. The files
and previous RHI selection are backed up and restored by `scripts/uninstall.ps1`.

## Status

Milestone 0.6 runs native DLAA and Super Resolution in the game. The add-on identifies DOS2's final
scene image before `CombineUI`, reads the real D24S8 scene depth, reconstructs
camera motion in an RG16F GPU texture from the game's `PerView` matrices and
calls the official NVIDIA NGX D3D11 evaluation function. The processed scene is
fed back into `CombineUI`, so menus and HUD remain at native resolution.

Quality, Balanced and Performance reduce the viewport used by the 3D render
chain while leaving DOS2's full-size render-target pool intact. Full-screen
passes receive an adjusted `VPtoRTScaleBias`, so every pass samples the active
top-left subrectangle instead of repeatedly shrinking it. The UI resource is
learned from the game's real `CombineUI` binding and stays at output resolution.

DLSS5 DX11 Bridge recognizes both feature creation and every evaluation as
game-supplied DLSS, with synthesis disabled. It is currently kept paused in the
tested installation because mirroring every frame to D3D12 dominated frame time
even with Neural Rendering disabled. The native D3D11 path does not require the
Bridge. DLSS5 Feeder is not used.

DLAA, Quality, Balanced and Performance are validated at 3440x1440. Quality
renders at 2293x960 and upscales to 3440x1440. A Halton temporal-jitter sequence
is applied to DOS2's 3D projection and the same offset is passed to NGX. UI and
post-processing viewports stay anchored at (0,0), which removes the whole-screen
drift and keeps CombineUI at its original aspect ratio. The active top-left render
rectangle is copied to an exactly sized NGX input; the production D3D11 runtime
does not reliably sample a smaller subrectangle from DOS2's full-size pooled target.

Balanced and Performance use the dimensions returned by NGX and share the same
dynamic-resolution path, but still need broader scene testing. The current build
supplies camera motion reconstructed from depth. DOS2's four-target G-buffer was
checked at shader-bytecode level: its R16G16 target stores octahedrally encoded
surface normals, not object velocity. Skinned/object motion therefore requires a
new velocity pass or a conservative current-color mask; that is the next quality
milestone.

Target game build: `3.6.117.3735`.
