# Validation for milestone 0.6

Test system: GeForce RTX 3070, DOS2 Definitive Edition 3.6.117.3735,
3440x1440. Measurements were recorded for eight seconds per mode in the same
main-menu scene with Intel PresentMon 2.5.1. Neural Rendering was disabled.

| Path | Mode | Average FPS | Average frame | Average GPU time |
| --- | --- | ---: | ---: | ---: |
| Native D3D11 | Off | 158.7 | 6.30 ms | 6.18 ms |
| Native D3D11 | DLAA | 109.8 | 9.10 ms | 8.98 ms |
| Native D3D11 | Quality | 144.4 | 6.93 ms | 6.79 ms |
| Native D3D11 | Balanced | 164.4 | 6.08 ms | 6.01 ms |
| Native D3D11 | Performance | 164.8 | 6.07 ms | 6.04 ms |

With DLSS5 DX11 Bridge loaded, the same five measurements were 155.5 FPS Off
and 10.6-11.1 FPS for every DLSS mode. The Bridge log reported an approximately
90 ms frame interval and an active D3D12 mirror even while RenoDX Neural
Rendering was disabled. The Bridge and RenoDX add-ons were therefore paused for
native-DLSS validation.

Six DLAA screenshots taken at different Halton phases were compared by phase
correlation across the menu UI, tower, statue and foreground stone regions. All
regions measured zero whole-pixel translation on both axes. Quality completed
more than 1,700 consecutive NGX evaluations. A direct 2293x960 subrectangle from
DOS2's 3440x1440 pooled target passed the NGX return-code check but produced a
repeated right edge in the actual image, so milestone 0.6 retains the exactly
sized compact color input.

Milestone 0.6 changes:

- jitter is applied to the game's 3D projection and view-projection matrices;
- viewports no longer move away from the top-left origin;
- camera motion reconstruction removes the current projection jitter;
- CombineUI receives its untouched constant buffer, preserving UI aspect ratio;
- the compact-color copy is retained because visual validation showed that a
  larger native D3D11 input resource is not equivalent to an exact-size input;
- expensive draw tracing is disabled by default and remains available in the panel.

The DLAA cost is expected: it keeps native-resolution scene rendering and adds
the DLSS resolve. Super Resolution modes reduce scene shading enough to recover
that cost in the measured scene. Gameplay cost will vary with CPU load and with
passes that DOS2 renders at fixed resolution.
