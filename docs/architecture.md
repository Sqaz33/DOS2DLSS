# Architecture

## User-facing control

The mod is controlled from a **DOS2 DLSS** panel in the ReShade overlay. It exposes Off, DLAA, Quality, Balanced and Performance modes, reset controls and diagnostic views.

RHI remains the installer and launcher. The runtime panel belongs to ReShade because RHI is not injected into the game's frame loop.

## DLSS 5 path

The mod creates a real NGX D3D11 feature and evaluates it with scene color, depth, motion vectors, jitter, exposure and masks. DLSS 5 Bridge mirrors the same contract onto D3D12. RenoDX DLSS 5 then sees a normal game-supplied DLSS invocation.

Milestone 0.2 completes NGX initialization, capability checking and feature
creation. At 3440x1440, the observed Performance contract is 1720x720 to
3440x1440. DLSS5 DX11 Bridge logs it as a game-created feature. Evaluation is
gated until all frame inputs below are valid.

Feeder is deliberately excluded. Its same-resolution synthetic contract is the reason it cannot provide real Super Resolution and can produce high cost and painterly temporal artifacts in DOS2.

## Character motion

DOS2 has no native object velocity buffer. The native module must preserve previous `WorldMatrix` and `BoneMatrices` values. A separate velocity pass re-renders moving rigid and skinned geometry into `R16G16_FLOAT`; camera motion is reconstructed from depth. Cloth, particles and complex transparency are handled with reactive and transparency masks.
