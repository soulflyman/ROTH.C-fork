/* mouselook.c — adds modern "always-on" mouse-look (yaw from mouse X, pitch from mouse Y)
 * on top of the original engine's own hold-button click-and-drag scheme. Compiles against
 * ONLY <roth_sdk.h> — no engine.h, no repo headers.
 *
 *   make            # -> plugin.so  (gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror)
 *   cp -r .  <game-dir>/mods/mouselook/      # plugin.so + mouselook.cfg live together
 *
 * ARCHITECTURE: per README.md ("Enhancements do not go into the engine. They belong in
 * either the platform layer or as mods.") this is a pure mod — no engine source touched.
 * It observes raw mouse deltas through the int33 mouse-poll seam (on_mouse_poll) and, once
 * per engine tick (on_frame_game), feeds them into the SAME two globals the original
 * engine's own turn/pitch code already uses:
 *   g_player_angle       0x90a8a (u16, wraps — full circle = 0x10000)
 *   g_view_pitch/_applied 0x90a74 / 0x8c108 (i32; the engine's own per-frame scene-record
 *                          build, player.c, clamps this to [-0x7e, 0x7e] every tick — we don't
 *                          need our own clamp, it already self-corrects)
 * so native pitch clamping and angle wraparound apply exactly as they always did.
 *
 * It never touches g_mouse_relative_mode / g_mouse_dx / g_mouse_dy (the original hold-button
 * virtual-joystick's own state), so that control scheme and WASD/keyboard turning+movement
 * (apply_player_movement_input's KEYBOARD path) are completely unaffected.
 *
 * Software 2D cursor: during gameplay the engine's own poll_mouse_motion (input.c) ALSO
 * drives a free 2D cursor (g_mouse_x/y, 0x707b3/0x707b7) from the same raw deltas — used
 * as the world-interaction aim point (render_world.c:40-41) and, visually, as a wandering
 * reticle. For a modern mouselook feel we want a fixed center crosshair instead, so while
 * gameplay is active on_mouse_poll zeroes the AX=0xb motion the engine sees (the SDK's
 * documented "overlay input capture" use of on_mouse_poll) — poll_mouse_motion's software-
 * cursor accumulators then simply stop drifting — and on_frame_game additionally repins
 * g_mouse_x/y to the engine's own precomputed viewport center (g_view_center_x/y,
 * 0x90a70/0x90a72, set in setup_render_projection_scale) every tick. In menus/dialogs
 * (g_player_movement_enabled != 1) none of this runs, so the free cursor works normally.
 *
 * Gate: only applies while g_player_movement_enabled == 1 (normal 3D gameplay — the same
 * flag the keybinds mod's menu-nav seam reads). Outside that window the accumulated delta
 * is discarded, not just paused, so there's no pent-up jump when a menu/dialog/cutscene
 * closes.
 *
 * Known interaction (verified against game_core.c:700-731): holding the LEFT mouse button
 * alone stacks our yaw/pitch writes with the native hold-to-drag accumulation into
 * g_mouse_dx/dy — harmless, just a double-turn for as long as it's held. Holding LEFT+RIGHT
 * together for >= 0xc frame-time units is a different, narrower case: the engine snapshots
 * a player_angle "mirror" (g_saved_int9_offset+0xc) and from then on recomputes
 * g_player_angle itself each tick as mirror-minus-accumulated-mouse_dx (compute_view_offsets_90a74,
 * canon 0x12179), overwriting whatever we wrote that tick for as long as both buttons stay
 * down. Cosmetic and self-correcting on release; not worth chasing for a WASD player who has
 * no reason to ever hold either button.
 *
 * Freelook toggle: F7 (a dead key in the engine; rebindable in the F11 menu's MOUSE section, which also has a
 * FREELOOK on/off row) turns the always-on look off and on during gameplay. Off = this mod stays out of the way
 * and the mouse drives the original 2D cursor again. Both settings are registry settings (SDK 0.8), so they
 * persist in mods/_settings.cfg. ROTH_MOUSELOOK=0 / ROTH_MOUSELOOK_KEY=0xNN only seed the defaults.
 *
 * Config: <plugin_dir>/mouselook.cfg (optional), or env overrides:
 *   ROTH_MOUSELOOK=0             disable entirely
 *   ROTH_MOUSELOOK_SENS=<float>  yaw sensitivity    (cfg: sensitivity,  default 1.5)
 *   ROTH_MOUSELOOK_PITCH=<float> pitch sensitivity  (cfg: pitch_scale, default 0.2)
 *   ROTH_MOUSELOOK_INVERT=1     invert pitch        (cfg: invert_pitch, default 0)
 *   ROTH_MOUSELOOK_DUMP=1       log the effective settings at boot
 *   ROTH_MOUSELOOK_DEBUG=1      log every applied dx/dy/angle/pitch (noisy — tuning only)
 *
 * HOT RELOAD: the settings-overlay mod (mods/_settings) writes mouselook.cfg live, while the
 * game keeps running — there's no synchronous cross-mod call API, so this mod polls its own
 * cfg file's (mtime, size) every ~14 engine ticks (~200ms at the native 70Hz tick rate, read
 * from the documented game_ram tick counter per the SDK's "Timing law" — never off callback
 * counts, since poll rates vary several-fold between scenes) and reapplies on change. Pure
 * scalar reassignment (sensitivity/pitch_scale/invert_pitch) is trivially safe to redo any
 * number of times, unlike keybinds.c's table-patching (see that file's hot-reload comment for
 * why ITS reload needed a correctness fix, not just a poll).
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "roth_sdk.h"

#define VA_PLAYER_MOVEMENT_ENABLED 0x7674au  /* g_player_movement_enabled: 1 = normal gameplay */
#define VA_PLAYER_ANGLE            0x90a8au  /* g_player_angle (u16, wraps 0..0xffff = full circle) */
#define VA_VIEW_PITCH              0x90a74u  /* g_view_pitch (i32; native per-frame clamp +/-0x7e) */
#define VA_VIEW_PITCH_APPLIED      0x8c108u  /* g_view_pitch_applied (i32; mirrors view_pitch) */
#define VA_MOUSE_X                 0x707b3u  /* g_mouse_x (i32; software-cursor / world aim point) */
#define VA_MOUSE_Y                 0x707b7u  /* g_mouse_y (i32) */
#define VA_VIEW_CENTER_X           0x90a70u  /* g_view_center_x (u16; render_width/2) */
#define VA_VIEW_CENTER_Y           0x90a72u  /* g_view_center_y (u16; render_height/2) */
#define VA_PIXEL_EXTENT_SCALE      0x707bbu  /* g_pixel_extent_scale (u16) */
#define VA_CURSOR_ACC_X            0x7e8c0u  /* software-cursor accumulators (poll_mouse_motion, input.c) */
#define VA_CURSOR_ACC_Y            0x7e8c4u
#define VA_CURSOR_HOT_X            0x7e8b8u  /* cursor hotspot * scale */
#define VA_CURSOR_HOT_Y            0x7e8bcu
#define VA_FRAME_TICK              0x90bccu  /* g_frame_tick_counter (u16, 70Hz, wrap-safe) */
#define RELOAD_CHECK_EVERY_TICKS   14u        /* ~200ms at 70Hz */

static double g_yaw_sens   = 1.5;
static double g_pitch_sens = 0.2;
static int    g_invert     = 0;
static int32_t g_enabled   = 1;           /* freelook on/off — a registered BOOL setting, toggled by g_toggle_key */
static int32_t g_toggle_key = 0x41;       /* F7 (a dead key in the engine) — a registered KEY setting */
static int32_t g_idx_enabled = -1;        /* registry index of 'freelook', for setting_commit */
static volatile int g_commit_req;         /* set from on_scancode (TICK_ISR), committed from on_frame_game (GAME) */
static int    g_debug      = 0;

static int32_t g_accum_dx, g_accum_dy;    /* raw mickeys accumulated since the last on_frame_game */
static double  g_yaw_frac, g_pitch_frac;  /* sub-integer carry, so a low sensitivity never rounds to 0 */

static char g_cfg_path[2600];              /* resolved once at on_load; api->plugin_dir() is only
                                             * valid during MAIN-phase callbacks, so later reload
                                             * checks from on_frame_game (GAME) reuse this cache */
static time_t g_cfg_mtime;
static long   g_cfg_size = -1;
static uint16_t g_last_reload_check_tick;

static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Reads g_cfg_path (if it exists) into the scalar settings. Env vars always win, so repeated
 * calls stay consistent whether triggered by on_load or a later hot-reload. */
static void load_config_from_path(void)
{
    FILE *f = fopen(g_cfg_path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *h = strchr(line, '#'); if (h) *h = 0;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(line), *v = trim(eq + 1);
            if      (!ci_cmp(k, "sensitivity"))  g_yaw_sens   = atof(v);
            else if (!ci_cmp(k, "pitch_scale"))  g_pitch_sens = atof(v);
            else if (!ci_cmp(k, "invert_pitch")) g_invert     = atoi(v);
        }
        fclose(f);
    }

    const char *env;
    if ((env = getenv("ROTH_MOUSELOOK_SENS")))   g_yaw_sens   = atof(env);
    if ((env = getenv("ROTH_MOUSELOOK_PITCH")))  g_pitch_sens = atof(env);
    if ((env = getenv("ROTH_MOUSELOOK_INVERT"))) g_invert     = atoi(env);
    if (getenv("ROTH_MOUSELOOK_DEBUG")) g_debug = 1;

    if (getenv("ROTH_MOUSELOOK_DUMP"))
        fprintf(stderr, "[mods:mouselook] enabled=%d sensitivity=%.2f pitch_scale=%.2f invert_pitch=%d (cfg: %s)\n",
                g_enabled, g_yaw_sens, g_pitch_sens, g_invert, g_cfg_path);
}

/* Checks g_cfg_path's (mtime, size) — NOT bare mtime, whose one-second granularity could miss
 * two quick successive writes from the settings overlay landing in the same second — and
 * reloads only if either changed since the last check. */
static void maybe_reload(void)
{
    struct stat st;
    if (stat(g_cfg_path, &st) != 0) return;
    if (st.st_mtime == g_cfg_mtime && (long)st.st_size == g_cfg_size) return;
    g_cfg_mtime = st.st_mtime;
    g_cfg_size  = (long)st.st_size;
    load_config_from_path();
}

static void on_load(const struct roth_api_v1 *api)
{
    const char *env = getenv("ROTH_MOUSELOOK_CFG");
    if (env) snprintf(g_cfg_path, sizeof g_cfg_path, "%s", env);
    else     snprintf(g_cfg_path, sizeof g_cfg_path, "%s/mouselook.cfg", api->plugin_dir());

    struct stat st;
    if (stat(g_cfg_path, &st) == 0) { g_cfg_mtime = st.st_mtime; g_cfg_size = (long)st.st_size; }
    load_config_from_path();

    /* ROTH_MOUSELOOK=0 seeds the default; a value saved by the settings overlay (registry) still wins. */
    if (getenv("ROTH_MOUSELOOK") && atoi(getenv("ROTH_MOUSELOOK")) == 0) g_enabled = 0;
    if (getenv("ROTH_MOUSELOOK_KEY"))
        g_toggle_key = (int32_t)(strtoul(getenv("ROTH_MOUSELOOK_KEY"), NULL, 0) & 0x7f);

    /* Settings registry (SDK 0.8): shown in the F11 menu's MOUSE section; values persist in mods/_settings.cfg. */
    struct roth_setting_v1 d;
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d; d.group = "MOUSE";
    d.key = "freelook"; d.label = "FREELOOK"; d.type = ROTH_SETTING_BOOL; d.value = &g_enabled;
    g_idx_enabled = api->register_setting(&d);
    memset(&d, 0, sizeof d);
    d.struct_size = (uint32_t)sizeof d; d.group = "MOUSE";
    d.key = "freelook_key"; d.label = "FREELOOK KEY"; d.type = ROTH_SETTING_KEY; d.value = &g_toggle_key;
    api->register_setting(&d);
}

/* on_scancode (TICK_ISR): the freelook toggle key. Only during normal 3D gameplay (so it can't fire while
 * typing in a menu); the make code is swallowed (F7 is a dead key in the engine anyway). The registry commit
 * (persist) is deferred to on_frame_game — no host calls from the tick. */
static uint8_t on_scancode(const struct roth_api_v1 *api, uint8_t sc)
{
    if ((sc & 0x7f) == (uint8_t)g_toggle_key && g_toggle_key != 0 &&
        api->game_ram->u8(VA_PLAYER_MOVEMENT_ENABLED) == 1) {
        if (!(sc & 0x80)) { g_enabled = !g_enabled; g_commit_req = 1; }
        return 0;
    }
    return sc;
}

/* on_mouse_poll (GAME): capture the int33 AX=0xb motion counters into our own accumulator.
 * While gameplay is active, ALSO zero what the engine sees (documented "overlay input
 * capture" use of on_mouse_poll) so poll_mouse_motion's software-cursor accumulators stop
 * drifting — see the file header. In menus/dialogs we pass the real deltas through
 * untouched, so the free 2D cursor works normally there. */
static void on_mouse_poll(const struct roth_api_v1 *api, struct roth_mouse_poll_v1 *mp)
{
    if (!g_enabled || mp->ax != 0x000b) return;
    g_accum_dx += (int16_t)(mp->ecx & 0xffffu);
    g_accum_dy += (int16_t)(mp->edx & 0xffffu);

    if (api->game_ram->u8(VA_PLAYER_MOVEMENT_ENABLED) == 1) {
        mp->ecx &= ~0xffffu;
        mp->edx &= ~0xffffu;
    }
}

/* on_frame_game (GAME): once per engine tick, drain the accumulator into the same globals
 * the native turn/pitch code uses — but only while actually playing; always drained (never
 * left to pile up across a menu/dialog/cutscene). Fractional carry (g_yaw_frac/g_pitch_frac)
 * means a low sensitivity slows the turn rate instead of rounding small deltas away to 0. */
static void on_frame_game(const struct roth_api_v1 *api)
{
    if (g_commit_req) {                                    /* the toggle key flipped g_enabled: persist it */
        g_commit_req = 0;
        if (g_idx_enabled >= 0) api->setting_commit((uint32_t)g_idx_enabled);
    }
    uint16_t tick = api->game_ram->u16(VA_FRAME_TICK);
    if ((uint16_t)(tick - g_last_reload_check_tick) >= RELOAD_CHECK_EVERY_TICKS) {
        g_last_reload_check_tick = tick;
        maybe_reload();
    }

    int32_t dx = g_accum_dx, dy = g_accum_dy;
    g_accum_dx = 0; g_accum_dy = 0;

    if (g_debug) {
        static unsigned n;
        if ((n++ % 140) == 0)
            fprintf(stderr, "[mods:mouselook] [diag] player_movement_enabled=%u\n",
                    api->game_ram->u8(VA_PLAYER_MOVEMENT_ENABLED));
    }

    if (!g_enabled) return;

    const struct roth_game_ram_api_v1 *gr = api->game_ram;
    if (gr->u8(VA_PLAYER_MOVEMENT_ENABLED) != 1) {
        g_yaw_frac = g_pitch_frac = 0;                      /* menu/dialog/cutscene: discard */
        return;
    }

    /* pin the world-interaction aim point / cursor sprite to dead center every tick */
    gr->set_u32(VA_MOUSE_X, (uint32_t)gr->u16(VA_VIEW_CENTER_X));
    gr->set_u32(VA_MOUSE_Y, (uint32_t)gr->u16(VA_VIEW_CENTER_Y));
    {   /* ...and re-base the engine's own cursor accumulators (poll_mouse_motion) on that centre, the relation
         * blit_scaled_sprite_at_mouse uses: acc = pos*scale - hotspot*scale. Then switching freelook OFF resumes
         * the free cursor from the screen centre instead of jumping to wherever it was when freelook began. */
        uint32_t scale = (uint32_t)(int32_t)(int16_t)gr->u16(VA_PIXEL_EXTENT_SCALE);
        if ((int32_t)scale > 0) {
            gr->set_u32(VA_CURSOR_ACC_X, (uint32_t)gr->u16(VA_VIEW_CENTER_X) * scale - gr->u32(VA_CURSOR_HOT_X));
            gr->set_u32(VA_CURSOR_ACC_Y, (uint32_t)gr->u16(VA_VIEW_CENTER_Y) * scale - gr->u32(VA_CURSOR_HOT_Y));
        }
    }

    if (dx != 0) {
        g_yaw_frac += dx * g_yaw_sens;
        int32_t step = (int32_t)g_yaw_frac;                 /* truncate toward 0 */
        g_yaw_frac -= step;
        if (step != 0) {
            uint16_t angle = (uint16_t)(gr->u16(VA_PLAYER_ANGLE) - (uint16_t)step);
            gr->set_u16(VA_PLAYER_ANGLE, angle);
            if (g_debug) fprintf(stderr, "[mods:mouselook] dx=%d -> angle=%u\n", dx, angle);
        }
    }
    if (dy != 0) {
        g_pitch_frac += g_invert ? (dy * g_pitch_sens) : -(dy * g_pitch_sens);
        int32_t step = (int32_t)g_pitch_frac;               /* truncate toward 0 */
        g_pitch_frac -= step;
        if (step != 0) {
            int32_t pitch = (int32_t)gr->u32(VA_VIEW_PITCH) + step;
            gr->set_u32(VA_VIEW_PITCH, (uint32_t)pitch);
            gr->set_u32(VA_VIEW_PITCH_APPLIED, (uint32_t)pitch);   /* mirror, as look_pitch_up/down do */
            if (g_debug) fprintf(stderr, "[mods:mouselook] dy=%d -> pitch=%d\n", dy, pitch);
        }
    }
}

static const struct roth_plugin_info_v1 MOUSELOOK = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "ire.roth.mouselook",
    .name          = "Mouselook (always-on yaw/pitch from mouse motion)",
    .version       = "1.0.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_GAME_RAM | ROTH_API_USE_INPUT,
    .on_load               = on_load,
    .on_game_ram_ready     = NULL,
    .on_frame_game         = on_frame_game,
    .on_compose_tick       = NULL,
    .on_audio              = NULL,
    .on_unload             = NULL,
    .on_register_overrides = NULL,
    .on_scancode           = on_scancode,
    .on_mouse_poll         = on_mouse_poll,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &MOUSELOOK;
}
