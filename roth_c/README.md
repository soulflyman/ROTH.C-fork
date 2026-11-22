# roth_c — the engine and its native host

This tree builds `roth` (Linux) and `rothc.exe` (Windows), the native executable that runs
_Realms of the Haunting_ from its original game files. It has three parts:

| path            | contents                                                                                                                                          |
| --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `src/engine/`   | the reconstructed game engine (see the fidelity note in the top-level README; this code does not change except as evidenced fidelity fixes)       |
| `src/data/`     | the engine's reconstructed static data as named and typed C                                                                                       |
| `src/platform/` | the native host: SDL3 video/input/audio, the DOS-era services the engine expects (files, memory, timers, interrupts), and the mod plugin platform |

The `engine` and `data` are the two parts that are considered byte-verified and faithful to the original. The `platform` part naturally is the area that had to be
modernized and it is the part allowing native support on modern systems.

## Building

The executable by default will be built as "mod-ready", just meaning it is set up with the
modding platform. If desired, you can still build the executable without the modding infrastructure.

One-time prerequisite, the 32-bit SDL3 (the engine is a 32-bit program; see the top-level
README for why):

```
../tools/build_sdl3_i386.sh     # builds third_party/sdl3-i386 (needs cmake, git, network,
                                # and 32-bit X11/ALSA/PipeWire/Mesa client libraries)
```

Then one of these:

```
make                    # the default mod-ready engine (roth)
make FLAVOR=vanilla     # the vanilla engine (roth-vanilla): no mod infrastructure at all
make dist               # the release bundle in dist/: stripped `roth`, bundled libSDL3.so.0,
                        # gm.sf2, README_ROTHC.txt, and an empty mods/ folder
make viewer             # optional: the headless-mode dev viewer (needs system SDL2)
```

The link self-checks: the moddable binary asserts it contains the mod platform's function
padding, the vanilla binary asserts it contains none, and both assert the absence of
development-harness symbols.

### The Windows build (`CROSS=mingw`)

The Windows executable is cross-compiled from Linux with the MinGW i686 toolchain
(`i686-w64-mingw32-gcc`; package `gcc-mingw-w64-i686` on Debian/Ubuntu). One-time prerequisite,
the win32 SDL3:

```
../tools/build_sdl3_win32.sh    # builds third_party/sdl3-win32 (needs cmake, git, network)
```

Then the same targets with `CROSS=mingw`:

```
make CROSS=mingw                    # the mod-ready Windows engine (rothc.exe)
make CROSS=mingw FLAVOR=vanilla     # the vanilla Windows engine (rothc-vanilla.exe)
make CROSS=mingw dist-win           # the Windows bundle in dist-win/: stripped rothc.exe,
                                    # SDL3.dll, gm.sf2, README_ROTHC.txt, and an empty mods/ folder
```

Building **on** a Windows PC: run the same cross-build inside
[WSL](https://learn.microsoft.com/windows/wsl/) — it is a full Linux environment, the
instructions above apply unchanged, and the resulting `rothc.exe` runs directly on the same
machine. A native MSYS2 build has not been validated.

Windows-specific notes:

- The engine stores absolute pointers in its reconstructed data, so it requires real memory at a
  set of fixed low addresses. On Windows the executable itself is laid out to make the loader
  reserve them: it is based low with a zero-fill `.arena` section covering the fixed window (a
  generated linker script — `tools/gen_win32_arena_ld.sh` — places it) and ASLR disabled. A
  post-link gate (`tools/pe_arena_gate.sh`) re-checks that geometry on every build, and the
  engine validates it again at startup. If you change link flags or the toolchain and the gate
  fails, that is the gate doing its job.
- `rothc.exe` is a windowed program (no console). To capture its log output, run it from a
  command prompt as `rothc.exe > log.txt 2>&1`. The `--trace` switch additionally logs every
  DOS-era service call the engine makes — the first tool to reach for on file or path problems.
- The application icon and version resource embed automatically when `res/rothc.ico` exists;
  without it the build is simply icon-less.

## Running

The engine finds the game by looking for `CONFIG.INI`:

1. **Beside the executable** - the product case: the binary sits in the game's `ROTH` directory.
2. **`--game-dir DIR`** - point it anywhere.
3. **`ROTH_DEV_GAME_DIR`** - an environment variable naming a fallback install; convenient when
   running the freshly built binary from the repository.

Common switches:

| switch              | effect                                                                                                  |
| ------------------- | ------------------------------------------------------------------------------------------------------- |
| _(none)_            | windowed, mods loaded from `<game dir>/mods/`                                                           |
| `--scale N`         | initial window scale (default: auto-fit)                                                                |
| `--sf2 FILE`        | use a specific SoundFont (otherwise: `ROTH_SF2` env, `gm.sf2` beside the binary, or a system soundfont) |
| `--filter NAME`     | upscaler: `nearest` (default, crisp), `linear`, `sharp` (sharp-bilinear), `scale2x` (EPX) (`ROTH_FILTER=NAME`) |
| `--integer-scale`   | scale only by whole multiples, letterboxed (`ROTH_INTEGER_SCALE=1`)                                    |
| `--scanlines N`     | darken every second output line, strength 0-8, needs >= 2x scale (`ROTH_SCANLINES=N`)                  |
| `--crt N`           | CRT look 0-2: vignette (level 2 adds a fine RGB mask at >= 4x scale); the horizontal lines come from `--scanlines` (`ROTH_CRT=N`)   |
| `--window-mode M`    | `windowed` (default), `borderless` (fullscreen window at desktop resolution) or `fullscreen` (`ROTH_WINDOW_MODE=M`; `--fullscreen` = borderless; also a WINDOW MODE row in the F11 menu) |
| `--aa`              | edge anti-aliasing of the 3D view (`ROTH_AA=1`; also a row in the F11 settings menu) - see the note below |
| `--devmode`         | enable the original's developer keys (e.g. wireframe map, open door); the DEBUG key bindings in the F11 menu need it |
| `--no-mods`         | start with the mod platform inert (`ROTH_MODS=0` does the same)                                         |
| `--list-mods`       | resolve and print the installed mods, then exit                                                         |
| `--strict-mods`     | make mod-validation warnings fatal                                                                      |
| `--dump-mod-chains` | print the resolved function-override chains at boot                                                     |
| `--headless`        | no window; publishes the framebuffer to shared memory for the dev viewer                                |

### Edge anti-aliasing (`--aa`) - known limitation

The renderer is an 8-bit palette rasterizer, so true MSAA/SSAA is not possible. `--aa` is an FXAA-style
post-process on the presented image, limited to the 3D view during gameplay (HUD, menus, cutscenes and mod
overlays stay untouched). **Known issue: it also softens textures** - any high-contrast texel pattern looks
like an edge to a luma-based filter, so walls and objects look blurrier than without it. It is off by default and
deliberately kept in for people who prefer smooth edges over sharp textures. The bottom tenth of the view is left unsmoothed (no AA, and drawn with nearest-neighbour scaling even when a smoothing UPSCALER / `--filter linear` is on) so subtitles and inspect text stay crisp and readable. In wide video modes the F11 menu shows a PREVIEW picture (a built-in test scene, drawn with the current anti-aliasing, upscaler, scanline and CRT settings) because the game view behind the menu is frozen. The F11 settings menu has ANTI-ALIASING and an **AA THRESHOLD** row (4-128, default 24; higher = fewer pixels
touched = less texture softening, but also less edge smoothing); these and UPSCALER / INTEGER SCALING / SCANLINES / CRT LOOK / WINDOW MODE are saved in
`mods/_settings.cfg` and win over the command line. `ROTH_AA_EDGE=N` sets the starting threshold. A smarter variant (e.g. only smoothing silhouette edges, not texture detail) is a
possible future improvement.

## The dev viewer (optional)

`src/platform/viewer/` contains a small SDL2 viewer (`roth-view`) that attaches to a
`--headless` engine's shared-memory framebuffer, which can be useful when working on the platform layer.

This can be built with `make viewer`. It also needs the system's 64-bit SDL2 development package.

## Modding

The mod platform's public face lives in [`../sdk/`](../sdk/) — start with
[`../sdk/MODDING_GUIDE.md`](../sdk/MODDING_GUIDE.md). In short: mods are native plugins in
`<game dir>/mods/<name>/plugin.so`, loaded and validated at boot; they can patch game data,
filter input, draw overlays, call engine functions through a versioned API, and wrap or replace
individual engine functions.
