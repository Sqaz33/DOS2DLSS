# UI viewport correction

The previous 0.6.1 visual-validation claims are withdrawn: they did not establish
that gameplay UI was correct. Comparing DLAA with another broken mode, and
resizing a dual-monitor capture without preserving aspect ratio, were invalid
checks. The earlier DLAA capture also showed a black scene, not a verified fix.

The UI branch incorrectly forced every widget viewport to the full texture
size, stretching modal windows and minimaps. It now preserves the game's UI
viewport and scissor. Scene viewport overrides are scoped to the actual draw
and restored immediately afterwards, protecting the game's cached raster state.

Build and installation passed. The game loaded the rebuilt add-on and performed
NGX evaluations. A main-monitor-only 3440x1440 screenshot showed the menu widgets
with corrected geometry compared with the earlier capture. Gameplay/minimap
verification is incomplete: the process exited during the save-load/mode-switch
attempt; the cause of that exit is undetermined. No DLAA-flicker fix is claimed.
