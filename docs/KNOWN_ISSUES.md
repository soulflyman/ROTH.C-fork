# Known issues

## Textures occasionally turn into flat colour (open, cause unconfirmed)

**Reported:** 2026-11-22 (two screenshots: half a tabletop; carpet and parts of a wall rendered as a single flat colour).
**Reproduction:** none; appears rarely during normal play on branch `display-pipeline-overhaul`
(wide mode, hor-plus FOV). Not yet checked whether it also happens on `master`.

**Mechanism.** `load_das_block_for_fat_index` (`roth_c/src/engine/das_assets.c`) falls back to a flat
placeholder (status word `0xfd | flat_color_table[idx] << 8`) when a DAS texture block cannot be loaded. In
the original code the `giveup:` tail also sets the index in the permanent bitset `g_das_low_index_bitset`
(0x8c740), so the texture stays flat until the next level load.

**Ruled out (statically):** pool exhaustion. The DPMI stub reports a 64 MB block and the DAS cache takes
nearly all of it.

**Remaining candidates:**
1. All 0xF0 cache slots are in use and `evict_das_cache_slot` finds no victim (more distinct textures
   visible per frame with hor-plus FOV / wide modes).
2. Pool allocation fails and `evict_das_cache_slot(1)` (forbids current-tick slots) frees nothing.
3. `select_das_fat_entry` yields size 0 (entry without data).
4. lseek/read failure on the DAS file handle.

**Mitigation (commit `b65f98e`):** cache-full failures (1, 2) are no longer pinned permanently. The entry is
re-armed 32 frames later and the texture reloads. Cases 3 and 4 keep the original permanent behaviour.

**Diagnostics:** each cache-full fallback logs to stderr
`[das] texture N unavailable (<reason>, size=S, tick=T): flat placeholder, retry in 32 frames`
(first 200 always; all with `ROTH_DAS_TRACE=1`).

**Next step:** if flat textures still appear after the mitigation, grep the log for `[das]`.
No `[das]` line means cases 3/4 (or a different cause); a line points to the cache-pressure path, and
slot eviction in `renderer.c` (`reserve_das_cache_slot` / `evict_das_cache_slot`) is the place to look.

### Update 2026-11-22 (evening): second occurrence, no `[das]` line

The textures vanished again (chandelier, door texture) in a run that also froze when examining a book. No `[das]` line
was logged, so the cache-full path (the only one instrumented at the time) is not the cause. Now every fallback exit logs
once per texture index (`[das] texture N unavailable (<reason>)`) and a census line
`[das] placeholder textures: A -> B (permanent-mark bits: M; first: ...)` is printed whenever the number of flat
placeholders changes. Suspect to check first: the user's `keybinds.cfg` swaps `inventory` and `flush_textures`, so
physical **I** triggers the engine's *flush textures* action (resets the whole DAS status table); pressing I out of habit
to close the inventory/popup would flush the texture cache mid-play.

## Examine popup hang (inventory_page)

`examine id=17 ... loop=0` never returned in the user's run and closing the window did not end the process.
Findings: headless, the popup for a non-looping record waits for Esc (engine callback `gdv_inspect_frame_callback`), and an
injected Esc closes it and returns normally; ticks keep running. Note `roth-inject` cannot inject keys into the
in-process (SDL) build any more (private shm) — use gdb (`set var g_shm->key_ring[...]`) to test input. Changes made:
Esc now arms the engine's exit latch from the plugin's `on_scancode`; the main thread force-exits 3 s after the window
closes if the game thread is wedged; `examine` logs its progress (`label` / `calling the popup`) to pinpoint a hang.
