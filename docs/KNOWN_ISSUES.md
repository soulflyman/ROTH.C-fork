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
