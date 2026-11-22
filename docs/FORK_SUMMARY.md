# Fork summary – changes relative to upstream ROTH.C

This fork is based on `beyond-the-ire/ROTH.C` (upstream `master`). 49 files, about 4600 lines changed.

## Display
- Selectable upscalers: nearest, linear, sharp-bilinear, scale2x; integer scaling.
- Scanlines (0–8) and CRT look (aperture-grille mask + vignette), decoupled from each other.
- Window modes: windowed, fullscreen window, fullscreen.
- Edge anti-aliasing (AA) of the 3D view with an adjustable threshold; the bottom tenth (subtitles) stays sharp.
- New widescreen resolutions: 854x480, 768x480, 800x480, 896x480.
- Hor-plus FOV for widescreen.
- Live preview in the F11 menu.

## Input / focus / gameplay
- Input is ignored while the window is unfocused; the click that regains focus no longer fires a shot; stuck keys after Alt+Tab fixed.
- "Pause on focus loss" option (greyscale view; one click lifts the pause and recaptures the mouse).
- Adjustable head-bob (0–100 %).
- Mouselook mod: F7 toggles freelook, settings stored in the registry.
- Yaw/pitch in the menu use a 0.01 step.

## Mods / SDK (up to 0.13)
- Settings registry (`register_setting`), persisted in `mods/_settings.cfg`.
- `set_display_filter`, `get_display_filter`, `set_display_preview`, `set_gameplay_option`.
- Further functions for weapon equip, `load_dbase300_resource_at_offset`, inventory item labels, greyscale LUT.
- `settings_overlay` plugin (F11): sorted menu, hold-to-repeat navigation, key bindings by topic.
- `inventory_page` plugin: single-page inventory with tile view, examine popup, movement keys navigate, game pauses, greyscale backdrop.

## Fixes
- Flat textures in widescreen modes (projection LUT overran the DAS bitset).
- DAS cache: transient load failures are no longer pinned permanently.
- Sound effects stopping after a while (sample descriptor lock leak).
- Examine popup hang (plugin call inside the SIGALRM handler); stall watchdog added.
- F4 abort on video-mode switch; cursor recentred after a mode change.
- Overlay pause blocked the overlays (`on_frame_game` is now driven from the main loop).
