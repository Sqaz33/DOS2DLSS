# Validation for milestone 0.6.1

Test system: GeForce RTX 3070, DOS2 Definitive Edition 3.6.117.3735,
3440x1440. The DLSS5 Bridge and RenoDX add-ons remained paused.

## Faults reproduced from 0.6.0

- The native UI target was identified with an obsolete allocation address and
  with only one position in the captured call stack. A current render trace
  identifies the Scaleform target through executable address `0x1DC57A3`.
- The add-on replaced pixel input slot 0 for the final CombineUI draw. DOS2 can
  cache an unchanged D3D11 binding, so the add-on's own DLSS output could remain
  in that slot and be selected as the next frame's DLSS input.
- More than one CombineUI draw in one present interval could start more than one
  NGX evaluation.

## Changes

- Recognize the full-resolution UI target before its first draw from any
  position in the verified allocation call stack and retain every learned UI
  resource until destruction.
- Do not apply camera jitter to draws targeting the native UI resource.
- Keep the game's tracked scene input separate from the temporary DLSS output
  binding and use that scene input for the next NGX evaluation.
- Allow at most one successful NGX evaluation per present interval.

## Runtime checks

- A four-second focused DLAA desktop capture sampled at 10 Hz contained 40
  stable frames. Mean luminance stayed between 27.166 and 27.188 and the largest
  adjacent-frame pixel delta was 0.253; no frame-wide black transition occurred.
- A four-second focused Quality capture contained 40 frames with no black
  transition. During that interval the present and NGX-evaluation counters both
  advanced by 1,011, confirming one evaluation per displayed frame.
- The fixed main-menu UI region was compared between DLAA and Quality. Its
  measured horizontal displacement was -0.003 pixels in the quarter-resolution
  desktop capture, with no horizontal scale change.

The menu and Scaleform composition path are covered by these checks. A gameplay
save was not loaded during this automated run, so the minimap should receive a
separate visual check in the user's current save.
