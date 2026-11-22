/* settings.c — in-game settings overlay: adjust mouselook sensitivity and key bindings live,
 * without restarting. Compiles against ONLY <roth_sdk.h> — no engine.h, no repo headers.
 *
 *   make            # -> plugin.so  (gcc -m32 -shared -fPIC -std=c11 -Wall -Wextra -Werror -D_GNU_SOURCE)
 *   cp -r .  <game-dir>/mods/_settings/      # plugin.so lives here (no cfg of its own)
 *
 * ARCHITECTURE: per README.md ("Enhancements do not go into the engine. They belong in
 * either the platform layer or as mods.") this is a pure mod — no engine source touched,
 * no function overrides, no calls into api->engine at all.
 *
 * DRAWING: on_compose_tick (TICK_ISR) is the only place a mod's drawing actually survives to
 * be shown — it writes the finished frame's indexed pixel buffer just before display
 * (sdk/MODDING_GUIDE.md:108-109). on_frame_game runs BEFORE the native per-tick render (it
 * rides the int33 mouse-poll chokepoint, see plugin_loader.c's frame_relay()), so anything
 * drawn there would be overwritten before the frame is ever published. Empirically verified
 * (headless roth-inject/dumpfb screenshot) before building out the rest of this file.
 *
 * LOAD ORDER: installed as mods/_settings/ — the leading underscore (0x5F) sorts before
 * 'k'eybinds/'m'ouselook in the lexicographic plugin load order. This matters because
 * on_mouse_poll is a SEQUENTIAL CHAIN (plugin_loader.c:300-311): dispatch_mouse_poll() passes
 * the same roth_mouse_poll_v1 through every plugin in load order, each seeing prior
 * mutations. Loading first lets this mod zero the mouse deltas BEFORE mouselook's
 * on_mouse_poll accumulates them, so the camera genuinely stops responding to mouse motion
 * while the overlay has focus, not just visually.
 *
 * STATE MACHINE (ST_CLOSED / ST_MAIN / ST_REBIND), driven from on_scancode (TICK_ISR — fast,
 * game_ram reads only, no file I/O, no engine calls):
 *   - Toggle key: F11 (scancode 0x57; ROTH_SETTINGS_TOGGLE_KEY overrides). Opens only while
 *     g_player_movement_enabled == 1 (real gameplay).
 *   - ST_MAIN: scrollable row list (yaw/pitch sensitivity, invert pitch, then every keybind
 *     action from a duplicated copy of keybinds.c's kb_actions[] table). Up/Down moves
 *     selection, Left/Right adjusts a slider/toggles a bool, Enter on an action row enters
 *     ST_REBIND.
 *   - ST_REBIND: next scancode (other than Esc) becomes the new binding; Esc cancels.
 *   - Every scancode is swallowed (return 0) while any state but ST_CLOSED is active.
 * All actual file I/O (cfg writes, reading live binding state for display) happens in
 * on_frame_game, triggered by flags set from on_scancode — never from TICK_ISR directly.
 *
 * SCOPE TRIM: keybinds.c's 4 "menu-nav virtual binds" (menu_up/down/left/right) are NOT
 * rebindable here. Unlike the 44 table-based actions, their live state isn't readable from
 * game_ram at all (they're a plugin-local on_scancode translation table inside keybinds.c,
 * not a game_ram-backed record) — showing/editing them would mean this mod re-parsing
 * keybinds.cfg's text as a second source of truth, which isn't worth the complexity for a
 * feature keybinds.cfg itself ships OFF by default. The 44 kb_actions[] entries (all real
 * movement/combat/UI keys) cover everything that matters day to day.
 *
 * CFG WRITE STRATEGY: targeted line patch, not a full rewrite (both mouselook.cfg and
 * keybinds.cfg carry hand-written comments the user already relies on) — see cfg_patch_file().
 * Writes to "<path>.tmp" then rename()s over the original: atomic, so the hot-reload watchers
 * added to mouselook.c/keybinds.c never see a torn read.
 *
 * KNOWN DUPLICATION: this file keeps its own READ-ONLY copy of keybinds.c's kb_actions[]/
 * kb_keys[] tables (no inter-mod call API exists). If keybinds.c's action table changes,
 * this copy must be updated to match.
 *
 * LIVE-APPLY LAG: mouselook.c/keybinds.c pick up cfg changes via an mtime+size poll (~200ms),
 * not synchronously — this overlay's own on-screen display updates instantly (it's the sole
 * writer while open), but the underlying mod's actual in-game behavior lags behind by up to
 * that poll interval. Expected, not a bug.
 *
 * Env overrides:
 *   ROTH_SETTINGS_TOGGLE_KEY=0xNN      override the F11 toggle scancode
 *   ROTH_SETTINGS_FORCE_OPEN=1         force the overlay open at boot (dev/screenshot only)
 *   ROTH_SETTINGS_MOUSELOOK_CFG=/path  override the resolved sibling mouselook.cfg path
 *   ROTH_SETTINGS_KEYBINDS_CFG=/path   override the resolved sibling keybinds.cfg path
 */
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roth_sdk.h"

/* ============================== 5x7 bitmap font ============================== */
/* Each glyph is 7 rows x 5 cols, '#'=pixel on, stored as one 35-char string (row-major, no
 * newlines) so the ASCII-art IS the source of truth — no hand-transcribed bit patterns.
 * Empirically verified legible via the headless dumpfb screenshot workflow. */
#define GLYPH_W 5
#define GLYPH_H 7

static const char *const FONT_GLYPHS[41] = {
    /* A-Z (0-25) */
    "..#.." ".#.#." "#...#" "#####" "#...#" "#...#" "#...#",  /* A */
    "####." "#...#" "#...#" "####." "#...#" "#...#" "####.",  /* B */
    ".####" "#...." "#...." "#...." "#...." "#...." ".####",  /* C */
    "####." "#...#" "#...#" "#...#" "#...#" "#...#" "####.",  /* D */
    "#####" "#...." "#...." "####." "#...." "#...." "#####",  /* E */
    "#####" "#...." "#...." "####." "#...." "#...." "#....",  /* F */
    ".####" "#...." "#...." "#.###" "#...#" "#...#" ".####",  /* G */
    "#...#" "#...#" "#...#" "#####" "#...#" "#...#" "#...#",  /* H */
    "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "#####",  /* I */
    "..###" "...#." "...#." "...#." "...#." "#..#." ".##..",  /* J */
    "#...#" "#..#." "#.#.." "##..." "#.#.." "#..#." "#...#",  /* K */
    "#...." "#...." "#...." "#...." "#...." "#...." "#####",  /* L */
    "#...#" "##.##" "#.#.#" "#...#" "#...#" "#...#" "#...#",  /* M */
    "#...#" "##..#" "#.#.#" "#..##" "#...#" "#...#" "#...#",  /* N */
    ".###." "#...#" "#...#" "#...#" "#...#" "#...#" ".###.",  /* O */
    "####." "#...#" "#...#" "####." "#...." "#...." "#....",  /* P */
    ".###." "#...#" "#...#" "#...#" "#.#.#" "#..#." ".##.#",  /* Q */
    "####." "#...#" "#...#" "####." "#.#.." "#..#." "#...#",  /* R */
    ".####" "#...." "#...." ".###." "....#" "....#" "####.",  /* S */
    "#####" "..#.." "..#.." "..#.." "..#.." "..#.." "..#..",  /* T */
    "#...#" "#...#" "#...#" "#...#" "#...#" "#...#" ".###.",  /* U */
    "#...#" "#...#" "#...#" "#...#" "#...#" ".#.#." "..#..",  /* V */
    "#...#" "#...#" "#...#" "#.#.#" "#.#.#" "##.##" "#...#",  /* W */
    "#...#" ".#.#." "..#.." "..#.." "..#.." ".#.#." "#...#",  /* X */
    "#...#" ".#.#." "..#.." "..#.." "..#.." "..#.." "..#..",  /* Y */
    "#####" "....#" "...#." "..#.." ".#..." "#...." "#####",  /* Z */
    /* 0-9 (26-35) */
    ".###." "#...#" "#..##" "#.#.#" "##..#" "#...#" ".###.",  /* 0 */
    "..#.." ".##.." "..#.." "..#.." "..#.." "..#.." "#####",  /* 1 */
    ".###." "#...#" "....#" "...#." "..#.." ".#..." "#####",  /* 2 */
    "####." "....#" "....#" ".###." "....#" "....#" "####.",  /* 3 */
    "...#." "..##." ".#.#." "#..#." "#####" "...#." "...#.",  /* 4 */
    "#####" "#...." "####." "....#" "....#" "#...#" ".###.",  /* 5 */
    "..##." ".#..." "#...." "####." "#...#" "#...#" ".###.",  /* 6 */
    "#####" "....#" "...#." "..#.." ".#..." ".#..." ".#...",  /* 7 */
    ".###." "#...#" "#...#" ".###." "#...#" "#...#" ".###.",  /* 8 */
    ".###." "#...#" "#...#" ".####" "....#" "...#." ".##..",  /* 9 */
    /* symbols (36-40) */
    "....." "....." "....." "....." "....." "....." ".....",  /* space (36) */
    "....." "....." "....." "....." "....." "..#.." ".....",  /* .     (37) */
    "....." "....." "....." "#####" "....." "....." ".....",  /* -     (38) */
    "#...#" "....#" "...#." "..#.." ".#..." "#...." "#...#",  /* %     (39) */
    "#...." "##..." "###.." "####." "###.." "##..." "#....",  /* >     (40, cursor arrow) */
};

static int glyph_index(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
    case '.': return 37;
    case '-': return 38;
    case '%': return 39;
    case '>': return 40;
    default:  return 36; /* unknown / space */
    }
}

static void plot(uint8_t *pixels, uint32_t width, uint32_t height, int x, int y, uint8_t color)
{
    if (x < 0 || y < 0 || (uint32_t)x >= width || (uint32_t)y >= height) return;
    pixels[(uint32_t)y * width + (uint32_t)x] = color;
}

static void fill_rect(uint8_t *pixels, uint32_t width, uint32_t height,
                       int x, int y, int w, int h, uint8_t color)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            plot(pixels, width, height, xx, yy, color);
}

static void draw_border(uint8_t *pixels, uint32_t width, uint32_t height,
                         int x, int y, int w, int h, int thick, uint8_t color)
{
    fill_rect(pixels, width, height, x, y, w, thick, color);
    fill_rect(pixels, width, height, x, y + h - thick, w, thick, color);
    fill_rect(pixels, width, height, x, y, thick, h, color);
    fill_rect(pixels, width, height, x + w - thick, y, thick, h, color);
}

static void draw_glyph(uint8_t *pixels, uint32_t width, uint32_t height,
                        int x, int y, char c, uint8_t color, int scale)
{
    const char *g = FONT_GLYPHS[glyph_index(c)];
    for (int row = 0; row < GLYPH_H; row++)
        for (int col = 0; col < GLYPH_W; col++)
            if (g[row * GLYPH_W + col] == '#')
                fill_rect(pixels, width, height, x + col * scale, y + row * scale, scale, scale, color);
}

static int draw_string(uint8_t *pixels, uint32_t width, uint32_t height,
                        int x, int y, const char *s, uint8_t color, int scale)
{
    int cx = x;
    for (; *s; s++) {
        draw_glyph(pixels, width, height, cx, y, *s, color, scale);
        cx += (GLYPH_W + 1) * scale;
    }
    return cx - x; /* rendered width, for right-aligned values */
}

/* ============================== palette (provisional — tune empirically) ============================== */
/* Live-editable through the registry (registered at on_load, rows appear in the menu itself). */
static int32_t g_text_color = 0xe8;   /* palette index; 0xe8 = proven-visible default (hello.c) */
static int32_t g_bg_style   = 0;      /* 0 solid, 1 see-through (checkerboard), 2 none */
static int32_t g_gray_bg = 1;                                /* registered BOOL: desaturate the 3D view while open */
static const char *const BG_STYLE_NAMES[3] = { "SOLID", "SEE-THROUGH", "NONE" };
#define COL_BG     0x00u
#define COL_BORDER ((uint8_t)g_text_color)
#define COL_TEXT   ((uint8_t)g_text_color)
#define COL_SEL    ((uint8_t)g_text_color)

/* ================= duplicated from keybinds.c (read-only; keep in sync by hand) ================= */
typedef struct { const char *name; uint32_t va; uint8_t tbl; } kb_action_t;
static const kb_action_t kb_actions[] = {
    { "move_forward",      0x12686, 1 }, { "move_backward",     0x12668, 1 },
    { "turn_left",         0x126b6, 1 }, { "turn_right",        0x12703, 1 },
    { "strafe_left",       0x126ad, 1 }, { "strafe_right",      0x126a4, 1 },
    { "look_up",           0x12927, 1 }, { "look_down",         0x12939, 1 },
    { "look_recenter_up",  0x1296f, 1 }, { "look_recenter_down",0x1294b, 1 },
    { "jump",              0x1c5f9, 1 }, { "crouch",            0x1c5d0, 1 },
    { "sprint_toggle",     0x17fed, 0 }, { "weapon_overlay",    0x175d8, 0 },
    { "subtitles",         0x1f89b, 0 }, { "easy_select",       0x1801c, 0 },
    { "display_cycle_alt", 0x147a8, 0 }, { "mouse_swap",        0x17fd2, 0 },
    { "display_cycle",     0x14794, 0 }, { "noop_f7",           0x17fd1, 0 },
    { "show_version",      0x14c9c, 0 }, { "quicksave",         0x14ce5, 0 },
    { "quickload",         0x14cc9, 0 }, { "gamma_up",          0x14601, 0 },
    { "gamma_down",        0x145ec, 0 }, { "snapshot",          0x11124, 0 },
    { "view_scale_0",      0x14573, 0 }, { "view_smaller",      0x14678, 0 },
    { "view_larger",       0x14613, 0 }, { "noop_n",            0x145a4, 0 },
    { "escape_menu",       0x145ae, 0 }, { "flush_textures",    0x144fb, 0 },
    { "wireframe_map",     0x1444c, 0 }, { "choice_down",       0x146ba, 0 },
    { "choice_up",         0x146c9, 0 }, { "dev_map_menu",      0x17fa4, 0 },
    { "help_overlay",      0x17fc0, 0 }, { "use_enter",         0x173f4, 0 },
    { "noop_t",            0x14ca7, 0 }, { "render_mode",       0x144da, 0 },
    { "weapon_1",          0x1be8e, 0 }, { "weapon_2",          0x1bea9, 0 },
    { "weapon_3",          0x1beb0, 0 }, { "weapon_4",          0x1beb7, 0 },
    { "weapon_5",          0x1bebe, 0 }, { "weapon_6",          0x1bec5, 0 },
    { "dev_open_door",     0x3df96, 0 }, { "inventory",         0x1a132, 0 },
    { "fire",              0x14cb6, 0 },
};
#define NUM_ACTIONS (int)(sizeof kb_actions / sizeof *kb_actions)

typedef struct { const char *name; uint8_t sc; } kb_key_t;
static const kb_key_t kb_keys[] = {
    {"esc",0x01},
    {"1",0x02},{"2",0x03},{"3",0x04},{"4",0x05},{"5",0x06},{"6",0x07},{"7",0x08},{"8",0x09},{"9",0x0a},{"0",0x0b},
    {"minus",0x0c},{"equals",0x0d},{"backspace",0x0e},{"tab",0x0f},
    {"q",0x10},{"w",0x11},{"e",0x12},{"r",0x13},{"t",0x14},{"y",0x15},{"u",0x16},{"i",0x17},{"o",0x18},{"p",0x19},
    {"lbracket",0x1a},{"rbracket",0x1b},{"enter",0x1c},{"ctrl",0x1d},
    {"a",0x1e},{"s",0x1f},{"d",0x20},{"f",0x21},{"g",0x22},{"h",0x23},{"j",0x24},{"k",0x25},{"l",0x26},
    {"semicolon",0x27},{"quote",0x28},{"grave",0x29},{"lshift",0x2a},{"backslash",0x2b},
    {"z",0x2c},{"x",0x2d},{"c",0x2e},{"v",0x2f},{"b",0x30},{"n",0x31},{"m",0x32},
    {"comma",0x33},{"period",0x34},{"slash",0x35},{"rshift",0x36},
    {"alt",0x38},{"space",0x39},{"capslock",0x3a},
    {"f1",0x3b},{"f2",0x3c},{"f3",0x3d},{"f4",0x3e},{"f5",0x3f},
    {"f6",0x40},{"f7",0x41},{"f8",0x42},{"f9",0x43},{"f10",0x44},
    {"kp7",0x47},{"kp8",0x48},{"kp9",0x49},{"kp4",0x4b},{"kp5",0x4c},{"kp6",0x4d},
    {"kp1",0x4f},{"kp2",0x50},{"kp3",0x51},{"kp0",0x52},
};
/* names for keys kb_keys[] (keybinds.c's parser vocabulary) doesn't cover — display only */
static const kb_key_t disp_keys[] = {
    {"f11",0x57},{"f12",0x58},{"up",0x48},{"down",0x50},{"left",0x4b},{"right",0x4d},
    {"home",0x47},{"end",0x4f},{"pgup",0x49},{"pgdn",0x51},{"ins",0x52},{"del",0x53},
};
static const char *disp_keyname(uint8_t sc);
static const char *keyname_of(uint8_t sc)
{
    for (unsigned i = 0; i < sizeof kb_keys / sizeof *kb_keys; i++)
        if (kb_keys[i].sc == sc) return kb_keys[i].name;
    return NULL;
}

static const char *disp_keyname(uint8_t sc)
{
    const char *n = keyname_of(sc);
    if (n) return n;
    for (unsigned i = 0; i < sizeof disp_keys / sizeof *disp_keys; i++)
        if (disp_keys[i].sc == sc) return disp_keys[i].name;
    return NULL;
}

/* read-only mirrors of keybinds.c's table-record accessors (game_ram, bounded) */
#define KEYMAP_MAX 64
static const uint32_t kb_table_canon[2] = { 0x7093du, 0x707f1u }; /* press, held */

static uint8_t find_action_key(const struct roth_game_ram_api_v1 *gr, const kb_action_t *act)
{
    uint32_t rec = kb_table_canon[act->tbl];
    uint32_t want = (uint32_t)(uintptr_t)gr->to_ptr(act->va);
    for (int i = 0; i < KEYMAP_MAX && gr->u8(rec) != 0; i++, rec += 5)
        if (gr->u32(rec + 1) == want) return gr->u8(rec);
    return 0;
}

/* ============================== VAs (game_ram) ============================== */
#define VA_PLAYER_MOVEMENT_ENABLED 0x7674au  /* g_player_movement_enabled: 1 = normal gameplay */

/* ============================== cfg patch helper ============================== */
#define CFG_PATCH_MAX 8

static int ci_ncmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}
static int ci_cmp(const char *a, const char *b) { return ci_ncmp(a, b, strlen(a) + 1); }

static int line_matches_key(const char *p, const char *key)
{
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') return 0;
    size_t klen = strlen(key);
    if (ci_ncmp(p, key, klen) != 0) return 0;
    const char *q = p + klen;
    while (*q == ' ' || *q == '\t') q++;
    return *q == '=';
}

/* Patch (or append) "key = value" lines in `path`, preserving every other line verbatim
 * (comments, blank lines, unrelated keys) and any trailing inline "# ..." comment on a
 * patched line. Writes to "<path>.tmp" then rename()s over the original — atomic, so a
 * concurrent hot-reload poll in mouselook.c/keybinds.c never observes a torn read. */
static int cfg_patch_file(const char *path, const char *const *keys, const char *const *vals, int n)
{
    if (n <= 0 || n > CFG_PATCH_MAX) return -1;
    int found[CFG_PATCH_MAX] = {0};

    long len = 0;
    char *buf;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        buf = (char *)malloc((size_t)len + 1);
        if (!buf) { fclose(f); return -1; }
        size_t rd = fread(buf, 1, (size_t)len, f);
        buf[rd] = '\0';
        fclose(f);
        len = (long)rd;
    } else {
        buf = (char *)malloc(1);
        if (!buf) return -1;
        buf[0] = '\0';
    }

    size_t out_cap = (size_t)len + 256;
    for (int i = 0; i < n; i++) out_cap += strlen(keys[i]) + strlen(vals[i]) + 96;
    char *out = (char *)malloc(out_cap);
    if (!out) { free(buf); return -1; }
    size_t out_len = 0;

    const char *p = buf;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p + 1) : strlen(p);
        int matched = -1;
        for (int i = 0; i < n; i++)
            if (!found[i] && line_matches_key(p, keys[i])) { matched = i; break; }

        char linebuf[320];
        size_t emit_len; const char *emit_ptr;
        if (matched >= 0) {
            found[matched] = 1;
            const char *hash = (const char *)memchr(p, '#', linelen);
            if (hash) {
                size_t taillen = linelen - (size_t)(hash - p) - (eol ? 1 : 0);
                snprintf(linebuf, sizeof linebuf, "%s = %s  %.*s\n",
                         keys[matched], vals[matched], (int)taillen, hash);
            } else {
                snprintf(linebuf, sizeof linebuf, "%s = %s\n", keys[matched], vals[matched]);
            }
            emit_ptr = linebuf; emit_len = strlen(linebuf);
        } else {
            emit_ptr = p; emit_len = linelen;
        }
        if (out_len + emit_len + 1 > out_cap) {
            out_cap = (out_len + emit_len + 1) * 2;
            char *grown = (char *)realloc(out, out_cap);
            if (!grown) { free(buf); free(out); return -1; }
            out = grown;
        }
        memcpy(out + out_len, emit_ptr, emit_len);
        out_len += emit_len;
        out[out_len] = '\0';

        p += linelen;
        if (!eol) break;
    }
    for (int i = 0; i < n; i++) {
        if (found[i]) continue;
        char linebuf[320];
        snprintf(linebuf, sizeof linebuf, "%s = %s\n", keys[i], vals[i]);
        size_t l = strlen(linebuf);
        if (out_len + l + 1 > out_cap) {
            out_cap = (out_len + l + 1) * 2;
            char *grown = (char *)realloc(out, out_cap);
            if (!grown) { free(buf); free(out); return -1; }
            out = grown;
        }
        memcpy(out + out_len, linebuf, l);
        out_len += l;
        out[out_len] = '\0';
    }

    char tmp[2600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *wf = fopen(tmp, "wb");
    if (!wf) { free(buf); free(out); return -1; }
    fwrite(out, 1, out_len, wf);
    fclose(wf);
    int rc = rename(tmp, path);
    free(buf); free(out);
    return rc;
}

/* ============================== plugin state ============================== */
typedef enum { ST_CLOSED, ST_MAIN, ST_REBIND, ST_CAPTURE } ui_state_t;

/* ---- screen resolution (a built-in row, not a registry setting: the source of truth is the engine's
 * resolution index + ROTH.INI's VideoMode). Applied the way the engine's own Screen menu does it:
 * g_selected_video_mode + bit 4 of g_pending_game_action, which the game loop turns into a mode set.
 * The big framebuffer is allocated at boot from VideoMode, so a mode that doesn't fit the CURRENT
 * surface (w*h > g_framebuffer_bytes) can't be switched live: then only VideoMode is saved. */
#define VA_RES_INDEX        0x7f358u   /* g_screen_resolution_index (current table index) */
#define VA_SELECTED_MODE    0x7f578u   /* g_selected_video_mode: index saved to ROTH.INI on exit */
#define VA_PENDING_ACTION   0x7fea8u   /* g_pending_game_action (byte; bit 4 = apply resolution) */
#define VA_FRAMEBUFFER_BYTES 0x85478u
typedef struct { int idx; const char *name; uint32_t w, h; } res_opt_t;
static const res_opt_t RES_LIST[] = {
    {1, "320X200", 320, 200}, {2, "320X400", 320, 400}, {7, "640X400", 640, 400}, {8, "640X480", 640, 480},
    {9, "854X480", 854, 480}, {10, "768X480", 768, 480}, {11, "800X480", 800, 480}, {12, "896X480", 896, 480},
};
#define NUM_RES ((int)(sizeof RES_LIST / sizeof *RES_LIST))
static volatile int g_res_sel;                  /* index into RES_LIST shown in the row */
static volatile int g_res_apply;                /* Enter pressed -> handled in on_frame_game */
static int g_res_now = -1;                      /* RES_LIST position of the running mode, -1 = not in the list */
static char g_res_status[24];
static char g_game_dir[2600];


/* ---- row model. One flat list, built once at the first on_frame_game (when every plugin's on_load has run):
 * category headers (level 0), sub-headers (level 1) and rows. Indent = 2 characters per level. ---- */
typedef enum { RK_HEADER, RK_YAW, RK_PITCH, RK_INVERT, RK_RES, RK_REG, RK_ACTION, RK_NOTE } rowkind_t;   /* RK_NOTE: plain text line, not selectable */
typedef struct {
    uint8_t kind, level;
    const char *title;                        /* RK_HEADER / RK_NOTE text */
    const struct roth_setting_v1 *s;          /* RK_REG */
    uint32_t idx;                             /* RK_REG: registry index; RK_ACTION: kb_actions[] index */
} row_t;
#define ROWS_MAX 320
#define INDENT_PX 12                          /* two characters per level */
static row_t g_rows[ROWS_MAX];
static volatile int g_nrows;
static int g_rows_built;
#define NUM_ROWS g_nrows
static volatile int g_capture_row = -1;         /* registry row waiting for a key (ST_CAPTURE) */
static volatile int g_commit_pending = -1;      /* registry index to setting_commit from on_frame_game */
#define VISIBLE_ROWS 40

static volatile ui_state_t g_state = ST_CLOSED;
static volatile int g_overlay_open;
static int g_paused;                                  /* we parked the game in movement mode 2 (see on_frame_game) */
static volatile uint8_t g_swallowed[128];             /* makes we swallowed: only their break is swallowed too */
static int g_sel = 0;
static int g_scroll_top = 0;
static volatile int g_vis_rows = VISIBLE_ROWS;   /* rows that actually fit the box; published by on_compose_tick */

static int32_t g_nav_move_keys = 1;  /* registered BOOL: movement keys navigate the menu */
static int32_t g_toggle_key = 0x57; /* F11 — a registered KEY setting, so the user can move it */

static double g_disp_yaw = 1.5, g_disp_pitch = 0.2;
static int g_disp_invert = 0;
static int g_mouselook_dirty = 0;

/* Upscale filter (SDK 0.6 roth_api_v1->set/get_display_filter): unlike mouselook/keybinds this
 * isn't backed by a cfg file the overlay can read — it's live host/platform state (g_shm), so
 * on open we ask the host for ground truth (get_display_filter) instead of assuming nearest. */
/* Display options (upscale filter / edge anti-aliasing / AA threshold) are REGISTRY settings registered by this
 * overlay itself, so they persist in mods/_settings.cfg like any mod's; on_change pushes them to the host via
 * set_display_filter (flag word: bit0 linear, bit1 edge-AA, bits 8-15 AA threshold). */
static int32_t g_set_linear = 0 /* scaler id: 0 nearest, 1 linear, 2 sharp-bilinear, 3 scale2x (key stays "linear_filter" so old saves still load) */,
               g_set_aa = 0, g_set_aa_thr = 24, g_set_integer = 0, g_set_scan = 0, g_set_crt = 0, g_set_wmode = 0;
static const char *const g_wmode_names[] = { "WINDOWED", "FULLSCREEN WINDOW", "FULLSCREEN" };
static const char *const g_crt_names[] = { "OFF", "LIGHT", "STRONG" };
static const char *const g_scaler_names[] = { "NEAREST", "LINEAR", "SHARP", "SCALE2X" };
static const struct roth_api_v1 *g_api;
static void apply_display_flags(void)
{
    if (!g_api) return;
    g_api->set_display_filter((uint32_t)((g_set_linear == 1 ? 1 : 0) | (g_set_aa ? 2 : 0) | ((g_set_aa_thr & 0xff) << 8) |
                                         ((g_set_linear & 0xf) << 16) | ((g_set_integer ? 1 : 0) << 20) | ((g_set_scan & 0xf) << 24) | ((g_set_crt & 0x3) << 28) | ((uint32_t)(g_set_wmode & 0x3) << 30)));
}

/* set_display_preview arrived with SDK 0.12: on an older host the table is shorter and the preview is simply absent. */
static void preview_set(const struct roth_api_v1 *api, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (api->struct_size >= offsetof(struct roth_api_v1, set_display_preview) + sizeof api->set_display_preview &&
        api->set_display_preview)
        api->set_display_preview(x, y, w, h);
}

/* set_gameplay_option arrived with SDK 0.13: on an older host the options simply do nothing. */
static int32_t g_set_bob = 100, g_set_pause_focus = 0;
static const struct roth_api_v1 *g_gp_api;
static void apply_gameplay_opts(void)
{
    const struct roth_api_v1 *api = g_gp_api;
    if (api && api->struct_size >= offsetof(struct roth_api_v1, set_gameplay_option) + sizeof api->set_gameplay_option &&
        api->set_gameplay_option) {
        api->set_gameplay_option(1, g_set_bob);
        api->set_gameplay_option(2, g_set_pause_focus);
    }
}

static uint8_t g_action_sc[64]; /* current scancode per action, refreshed from game_ram on open */

static volatile int g_pending_rebind = 0;
static int g_rebind_action_idx = -1;
static volatile uint8_t g_pending_rebind_sc = 0;

static char g_mouselook_cfg[2600];
static char g_keybinds_cfg[2600];

static char *cfg_trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void load_mouselook_display_values(void)
{
    FILE *f = fopen(g_mouselook_cfg, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#'); if (h) *h = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = cfg_trim(line), *v = cfg_trim(eq + 1);
        if      (!ci_cmp(k, "sensitivity"))  g_disp_yaw    = atof(v);
        else if (!ci_cmp(k, "pitch_scale"))  g_disp_pitch  = atof(v);
        else if (!ci_cmp(k, "invert_pitch")) g_disp_invert = atoi(v);
    }
    fclose(f);
}

static void resolve_sibling_cfg(char *out, size_t outsz, const char *plugin_dir,
                                 const char *env_name, const char *mod_name, const char *cfg_name)
{
    const char *env = getenv(env_name);
    if (env) { snprintf(out, outsz, "%s", env); return; }
    char mods_root[2600];
    snprintf(mods_root, sizeof mods_root, "%s", plugin_dir);
    char *slash = strrchr(mods_root, '/');
    if (slash) *slash = '\0';
    snprintf(out, outsz, "%s/%s/%s", mods_root, mod_name, cfg_name);
}

static void on_load(const struct roth_api_v1 *api)
{
    if (getenv("ROTH_SETTINGS_TOGGLE_KEY"))        /* seeds the default; a saved registry value still wins */
        g_toggle_key = (int32_t)(strtoul(getenv("ROTH_SETTINGS_TOGGLE_KEY"), NULL, 0) & 0x7f);

    g_api = api;
    {   /* seed the display options from the host's current flags (CLI --filter/--aa, ROTH_AA_EDGE), then register
         * them: a saved value from mods/_settings.cfg is applied by the host during registration and wins. */
        uint32_t ff = api->get_display_filter();
        g_set_linear = (int32_t)((ff >> 16) & 0xfu);
        if (g_set_linear == 0 && (ff & 1u)) g_set_linear = 1;
        if (g_set_linear > 3) g_set_linear = 0;
        g_set_integer = (int32_t)((ff >> 20) & 1u);
        g_set_scan = (int32_t)((ff >> 24) & 0xfu);
        if (g_set_scan > 8) g_set_scan = 8;
        g_set_crt = (int32_t)((ff >> 28) & 0x3u);
        g_set_wmode = (int32_t)((ff >> 30) & 0x3u);
        if (g_set_wmode > 2) g_set_wmode = 0;
        if (g_set_crt > 2) g_set_crt = 0;
        g_set_aa = (int32_t)((ff >> 1) & 1u);
        if ((ff >> 8) & 0xffu) g_set_aa_thr = (int32_t)((ff >> 8) & 0xffu);
        else if (getenv("ROTH_AA_EDGE")) g_set_aa_thr = atoi(getenv("ROTH_AA_EDGE"));
        if (g_set_aa_thr < 4) g_set_aa_thr = 4;
        if (g_set_aa_thr > 128) g_set_aa_thr = 128;
        struct roth_setting_v1 d;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "linear_filter"; d.label = "UPSCALER"; d.type = ROTH_SETTING_ENUM; d.value = &g_set_linear;
        d.imax = 3; d.enum_names = g_scaler_names; d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "integer_scale"; d.label = "INTEGER SCALING"; d.type = ROTH_SETTING_BOOL; d.value = &g_set_integer;
        d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "scanlines"; d.label = "SCANLINES"; d.type = ROTH_SETTING_INT; d.value = &g_set_scan;
        d.imin = 0; d.imax = 8; d.istep = 1; d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "window_mode"; d.label = "WINDOW MODE"; d.type = ROTH_SETTING_ENUM; d.value = &g_set_wmode;
        d.imax = 2; d.enum_names = g_wmode_names; d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "crt_look"; d.label = "CRT LOOK"; d.type = ROTH_SETTING_ENUM; d.value = &g_set_crt;
        d.imax = 2; d.enum_names = g_crt_names; d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "edge_aa"; d.label = "ANTI-ALIASING"; d.type = ROTH_SETTING_BOOL; d.value = &g_set_aa;
        d.on_change = apply_display_flags;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "DISPLAY";
        d.key = "aa_threshold"; d.label = "AA THRESHOLD"; d.type = ROTH_SETTING_INT; d.value = &g_set_aa_thr;
        d.imin = 4; d.imax = 128; d.istep = 4; d.on_change = apply_display_flags;
        api->register_setting(&d);
        apply_display_flags();                         /* push the (possibly persisted) values to the presenter now */
    }
    {   /* gameplay options (host-side, SDK 0.13) */
        struct roth_setting_v1 d;
        g_gp_api = api;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "GAMEPLAY";
        d.key = "head_bob"; d.label = "HEAD BOB %"; d.type = ROTH_SETTING_INT; d.value = &g_set_bob;
        d.imin = 0; d.imax = 100; d.istep = 25; d.on_change = apply_gameplay_opts;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d; d.group = "GAMEPLAY";
        d.key = "pause_on_focus_loss"; d.label = "PAUSE ON FOCUS LOSS"; d.type = ROTH_SETTING_BOOL; d.value = &g_set_pause_focus;
        d.on_change = apply_gameplay_opts;
        api->register_setting(&d);
        apply_gameplay_opts();                         /* the menu's (persisted) values win over --head-bob / ROTH_HEADBOB */
    }
    {   /* this overlay's own options, through the same registry any other mod uses */
        struct roth_setting_v1 d;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "toggle_key"; d.label = "MENU KEY"; d.type = ROTH_SETTING_KEY; d.value = &g_toggle_key;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "nav_move_keys"; d.label = "MOVE KEYS NAVIGATE"; d.type = ROTH_SETTING_BOOL; d.value = &g_nav_move_keys;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "text_color"; d.label = "TEXT COLOR"; d.type = ROTH_SETTING_INT; d.value = &g_text_color;
        d.imin = 0; d.imax = 255; d.istep = 1;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "background"; d.label = "BACKGROUND"; d.type = ROTH_SETTING_ENUM; d.value = &g_bg_style;
        d.imin = 0; d.imax = 2; d.istep = 1; d.enum_names = BG_STYLE_NAMES;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "gray_background"; d.label = "GRAY GAME VIEW"; d.type = ROTH_SETTING_BOOL; d.value = &g_gray_bg;
        api->register_setting(&d);
    }

    resolve_sibling_cfg(g_mouselook_cfg, sizeof g_mouselook_cfg, api->plugin_dir(),
                        "ROTH_SETTINGS_MOUSELOOK_CFG", "mouselook", "mouselook.cfg");
    resolve_sibling_cfg(g_keybinds_cfg, sizeof g_keybinds_cfg, api->plugin_dir(),
                        "ROTH_SETTINGS_KEYBINDS_CFG", "keybinds", "keybinds.cfg");
    load_mouselook_display_values();

    snprintf(g_game_dir, sizeof g_game_dir, "%s", api->plugin_dir());      /* <game>/mods/_settings -> <game> */
    for (int up = 0; up < 2; up++) { char *sl = strrchr(g_game_dir, '/'); if (sl) *sl = '\0'; }
    if (getenv("ROTH_SETTINGS_GAME_DIR")) snprintf(g_game_dir, sizeof g_game_dir, "%s", getenv("ROTH_SETTINGS_GAME_DIR"));

    if (getenv("ROTH_SETTINGS_FORCE_OPEN")) {
        g_state = ST_MAIN;
        g_overlay_open = 1;
    }
    fprintf(stderr, "[mods:_settings] on_load: toggle_sc=0x%02x mouselook_cfg=%s keybinds_cfg=%s\n",
            (unsigned)g_toggle_key, g_mouselook_cfg, g_keybinds_cfg);
}

/* ---- row navigation: headers are never selectable ---- */
static int row_selectable(int row)
{
    return row >= 0 && row < g_nrows && g_rows[row].kind != RK_HEADER && g_rows[row].kind != RK_NOTE;
}

static void move_selection(int dir)
{
    int next = g_sel;
    do {
        next += dir;
        if (next < 0) next = NUM_ROWS - 1;
        if (next >= NUM_ROWS) next = 0;
    } while (!row_selectable(next) && next != g_sel);
    g_sel = next;
    if (g_sel < g_scroll_top) g_scroll_top = g_sel;
    if (g_sel >= g_scroll_top + g_vis_rows) g_scroll_top = g_sel - g_vis_rows + 1;
}

/* Adjust the registry setting shown on `row` by one step (dir = -1 / +1). TICK_ISR-safe: it only writes
 * the mod-owned value word; setting_commit (persist + on_change) is deferred to on_frame_game. */
static void reg_adjust(int row, int dir)
{
    const row_t *rr = &g_rows[row];
    const struct roth_setting_v1 *s = rr->s;
    if (!s) return;
    if (s->type == ROTH_SETTING_FLOAT) {
        float v = *(float *)s->value + (float)dir * (s->fstep > 0 ? s->fstep : 0.1f);
        if (s->fmax > s->fmin) { if (v < s->fmin) v = s->fmin; if (v > s->fmax) v = s->fmax; }
        *(float *)s->value = v;
    } else if (s->type == ROTH_SETTING_BOOL) {
        *(int32_t *)s->value = !*(int32_t *)s->value;
    } else if (s->type == ROTH_SETTING_ENUM) {
        int32_t n = s->imax + 1, v = *(int32_t *)s->value + dir;
        if (n > 0) { if (v < 0) v = n - 1; if (v >= n) v = 0; *(int32_t *)s->value = v; }
    } else if (s->type == ROTH_SETTING_INT) {
        int32_t st = s->istep > 0 ? s->istep : 1, v = *(int32_t *)s->value + dir * st;
        if (s->imax >= s->imin) { if (v < s->imin) v = s->imin; if (v > s->imax) v = s->imax; }
        *(int32_t *)s->value = v;
    } else return;                                  /* KEY: handled by the capture state */
    g_commit_pending = (int)rr->idx;
}

/* Hold-to-repeat for the navigation keys (the host forwards only the first make code, not typematic repeats).
 * Up/Down always repeat; Left/Right only on rows that step a number (an ON/OFF flip or a list cycle would
 * just flicker). The repeat is driven from on_compose_tick off the 70 Hz engine tick. */
#define VA_TICK 0x90bccu
#define REPEAT_DELAY_TICKS 25            /* ~360 ms before the first repeat */
#define REPEAT_RATE_TICKS  4             /* ~57 ms between repeats */
static volatile uint8_t g_held;          /* make code being held, 0 = none */
static uint16_t g_held_next;

/* Movement keys double as menu navigation (same scheme as the inventory overlay): the live scancodes of the engine's held-key
 * table (5-byte records {scancode, handler ptr} at 0x707f1; handlers = move_forward/backward, strafe_left/right), so a rebind
 * follows automatically. */
static volatile uint8_t g_nav_sc[4];                      /* up, down, left, right; 0 = unknown */
static const uint32_t NAV_HANDLER_VA[4] = { 0x12686u, 0x12668u, 0x126adu, 0x126a4u };

static void refresh_nav_keys(const struct roth_game_ram_api_v1 *gr)
{
    for (int a = 0; a < 4; a++) {
        uint32_t want = (uint32_t)(uintptr_t)gr->to_ptr(NAV_HANDLER_VA[a]), rec = 0x707f1u;
        uint8_t sc = 0;
        for (int i = 0; i < 64 && gr->u8(rec) != 0; i++, rec += 5)
            if (gr->u32(rec + 1) == want) { sc = gr->u8(rec); break; }
        g_nav_sc[a] = sc;
    }
}

static uint8_t nav_translate(uint8_t make)
{
    static const uint8_t ARROW[4] = { 0x48, 0x50, 0x4b, 0x4d };
    if (!g_nav_move_keys || make == (uint8_t)g_toggle_key || make == 0x01 || make == 0x1c) return make;
    for (int a = 0; a < 4; a++)
        if (g_nav_sc[a] && g_nav_sc[a] == make) return ARROW[a];
    return make;
}

static int row_steps_number(int row)
{
    if (row < 0 || row >= g_nrows) return 0;
    const row_t *rr = &g_rows[row];
    if (rr->kind == RK_YAW || rr->kind == RK_PITCH) return 1;
    return rr->kind == RK_REG && (rr->s->type == ROTH_SETTING_INT || rr->s->type == ROTH_SETTING_FLOAT);
}
static int row_repeatable(uint8_t make)
{
    return make == 0x48 || make == 0x50 || row_steps_number(g_sel);
}

/* One navigation/adjust step for Up/Down/Left/Right on the selected row (also what a repeat does). */
static void nav_key(uint8_t make)
{
    if (make == 0x48) { move_selection(-1); return; }
    if (make == 0x50) { move_selection(1);  return; }
    int dir = (make == 0x4d) ? 1 : -1;
    if (g_sel < 0 || g_sel >= g_nrows) return;
    const row_t *rr = &g_rows[g_sel];
    switch (rr->kind) {
    case RK_YAW:
        g_disp_yaw = floor((g_disp_yaw + dir * 0.01) * 100.0 + 0.5) / 100.0;
        if (g_disp_yaw < 0.01) g_disp_yaw = 0.01;
        if (g_disp_yaw > 20.0) g_disp_yaw = 20.0;
        g_mouselook_dirty = 1; break;
    case RK_PITCH:
        g_disp_pitch = floor((g_disp_pitch + dir * 0.01) * 100.0 + 0.5) / 100.0;
        if (g_disp_pitch < 0.01) g_disp_pitch = 0.01;
        if (g_disp_pitch > 5.0) g_disp_pitch = 5.0;
        g_mouselook_dirty = 1; break;
    case RK_INVERT: g_disp_invert = !g_disp_invert; g_mouselook_dirty = 1; break;
    case RK_RES: {
        int v = g_res_sel + dir;
        if (v < 0) v = NUM_RES - 1;
        if (v >= NUM_RES) v = 0;
        g_res_sel = v;
        g_res_status[0] = '\0';
        break; }
    case RK_REG: reg_adjust(g_sel, dir); break;
    default: break;
    }
}

/* on_scancode (TICK_ISR): fast, game_ram reads only, no engine calls, no file I/O. Every key
 * is swallowed while the overlay owns focus; only plain/volatile state is touched here —
 * actual file I/O happens later from on_frame_game. */
static uint8_t on_scancode(const struct roth_api_v1 *api, uint8_t sc)
{
    uint8_t make = (uint8_t)(sc & 0x7f);
    int is_break = (sc & 0x80) != 0;

    if (is_break) {   /* a release must reach the engine unless we swallowed that key's press (else e.g. Space=jump stays "held") */
        if (g_held && nav_translate(make) == g_held) g_held = 0;
        if (g_swallowed[make]) { g_swallowed[make] = 0; return 0; }
        return sc;
    }

    if (g_state == ST_CLOSED) {
        if (make == (uint8_t)g_toggle_key &&
            api->game_ram->u8(VA_PLAYER_MOVEMENT_ENABLED) == 1) {
            g_state = ST_MAIN;
            g_overlay_open = 1;
            g_swallowed[make] = 1;
            return 0;
        }
        return sc; /* normal gameplay: untouched passthrough */
    }

    g_swallowed[make] = 1;                                         /* overlay owns focus: this press (and its release) is ours */

    if (g_state == ST_CAPTURE) {                      /* registry KEY setting waiting for its key */
        if (make == 0x01) {                           /* Esc cancels (so Esc itself cannot be bound) */
            g_state = ST_MAIN;
        } else if (make != 0x60 && make != 0x61) {    /* ignore the 0xE0/0xE1 prefix bytes */
            int r = g_capture_row;
            if (r >= 0 && r < g_nrows && g_rows[r].kind == RK_REG && g_rows[r].s->type == ROTH_SETTING_KEY) {
                *(int32_t *)g_rows[r].s->value = make;
                g_commit_pending = (int)g_rows[r].idx;
            }
            g_state = ST_MAIN;
        }
        return 0;
    }

    if (g_state == ST_REBIND) {
        if (make == 0x01) { /* Esc cancels */
            g_state = ST_MAIN;
        } else {
            g_pending_rebind_sc = make;
            g_pending_rebind = 1;
            g_state = ST_MAIN;
        }
        return 0;
    }

    /* ST_MAIN */
    if (make == (uint8_t)g_toggle_key || make == 0x01) { /* toggle key again, or Esc: close */
        g_state = ST_CLOSED;
        g_overlay_open = 0;
        return 0;
    }
    make = nav_translate(make);
    if (make == 0x48 || make == 0x50 || make == 0x4b || make == 0x4d) {   /* Up / Down / Left / Right (auto-repeat while held) */
        nav_key(make);
        g_held = (row_repeatable(make)) ? make : 0;
        g_held_next = (uint16_t)(api->game_ram->u16(VA_TICK) + REPEAT_DELAY_TICKS);
        return 0;
    }
    if (make == 0x1c) { /* Enter */
        if (g_sel >= 0 && g_sel < g_nrows) {
            const row_t *rr = &g_rows[g_sel];
            if (rr->kind == RK_INVERT) { g_disp_invert = !g_disp_invert; g_mouselook_dirty = 1; }
            else if (rr->kind == RK_RES) g_res_apply = 1;
            else if (rr->kind == RK_REG) {
                if (rr->s->type == ROTH_SETTING_KEY) { g_capture_row = g_sel; g_state = ST_CAPTURE; }
                else reg_adjust(g_sel, 1);
            } else if (rr->kind == RK_ACTION) {
                g_rebind_action_idx = (int)rr->idx;
                g_state = ST_REBIND;
            }
        }
        return 0;
    }
    return 0; /* overlay owns focus: swallow everything else */
}

/* on_mouse_poll (GAME): hide mouse motion/clicks from the game while the overlay has focus —
 * runs before mouselook's own on_mouse_poll thanks to the _settings load-order trick. */
static void on_mouse_poll(const struct roth_api_v1 *api, struct roth_mouse_poll_v1 *mp)
{
    (void)api;
    if (!g_overlay_open) return;
    if (mp->ax == 0x000b) { mp->ecx &= ~0xffffu; mp->edx &= ~0xffffu; }
    else if (mp->ax == 0x0003) { mp->ebx &= ~0xffffu; }
}

/* Set "VideoMode=<idx>" in <game>/ROTH.INI, keeping the file's own line endings and every other line.
 * (The engine also rewrites this file from g_selected_video_mode when the game exits; patching it now
 * just makes the choice survive a crash/kill.) Returns 0 on success. */
static int patch_ini_video_mode(int idx)
{
    static const char *const names[] = { "ROTH.INI", "roth.ini", "Roth.ini", NULL };
    char path[2700];
    FILE *f = NULL;
    for (int i = 0; names[i] && !f; i++) {
        snprintf(path, sizeof path, "%s/%s", g_game_dir, names[i]);
        f = fopen(path, "rb");
    }
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)n, f); fclose(f); buf[rd] = '\0';
    const char *eol = strchr(buf, '\r') ? "\r\n" : "\n";
    char *out = (char *)malloc(rd + 64);
    if (!out) { free(buf); return -1; }
    size_t ol = 0; int done = 0;
    for (char *p = buf; *p; ) {
        char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p + 1) : strlen(p);
        const char *q = p; while (*q == ' ' || *q == '\t') q++;
        if (!done && ci_ncmp(q, "VideoMode", 9) == 0 && (q[9] == '=' || q[9] == ' ')) {
            ol += (size_t)sprintf(out + ol, "VideoMode=%d%s", idx, eol);
            done = 1;
        } else { memcpy(out + ol, p, ll); ol += ll; }
        p += ll;
    }
    if (!done) ol += (size_t)sprintf(out + ol, "VideoMode=%d%s", idx, eol);
    char tmp[2720];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *w = fopen(tmp, "wb");
    int rc = -1;
    if (w) { fwrite(out, 1, ol, w); fclose(w); remove(path); rc = rename(tmp, path); }
    free(buf); free(out);
    return rc;
}

/* Apply the row's resolution (Enter): live if it fits the running framebuffer, else save-for-restart. */
static void apply_resolution(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *gr = api->game_ram;
    int idx = RES_LIST[g_res_sel].idx;
    if ((int)gr->u32(VA_RES_INDEX) == idx) { snprintf(g_res_status, sizeof g_res_status, "ALREADY ACTIVE"); return; }
    uint32_t need = RES_LIST[g_res_sel].w * RES_LIST[g_res_sel].h;   /* modes >= 10 aren't in the game_ram table blob */
    int fits = need > 0 && need <= gr->u32(VA_FRAMEBUFFER_BYTES);
    gr->set_u32(VA_SELECTED_MODE, (uint32_t)idx);              /* what write_roth_ini saves on exit */
    int ini = patch_ini_video_mode(idx);
    if (fits) {
        gr->set_u8(VA_PENDING_ACTION, (uint8_t)(gr->u8(VA_PENDING_ACTION) | 4u));   /* the game loop does the mode set */
        snprintf(g_res_status, sizeof g_res_status, "APPLYING");
    } else {
        snprintf(g_res_status, sizeof g_res_status, ini == 0 ? "RESTART NEEDED" : "RESTART NEEDED NO INI");
    }
}

/* Key-binding topics: which kb_actions[] names go under which sub-heading. Anything unlisted lands in DEBUG. */
static const struct { const char *title; const char *note; const char *names[12]; } KB_TOPICS[] = {
    { "MOVEMENT", NULL,  { "move_forward", "move_backward", "turn_left", "turn_right", "strafe_left", "strafe_right",
                     "jump", "crouch", "sprint_toggle", NULL } },
    { "LOOK", NULL,      { "look_up", "look_down", "look_recenter_up", "look_recenter_down", NULL } },
    { "COMBAT", NULL,    { "fire", "weapon_1", "weapon_2", "weapon_3", "weapon_4", "weapon_5", "weapon_6", "weapon_overlay", NULL } },
    { "INVENTORY", NULL, { "inventory", "use_enter", "easy_select", "choice_up", "choice_down", NULL } },
    { "INTERFACE", NULL, { "escape_menu", "subtitles", "help_overlay", "mouse_swap", NULL } },
    { "SAVE AND LOAD", NULL, { "quicksave", "quickload", NULL } },
    { "DISPLAY", NULL,   { "display_cycle", "display_cycle_alt", "gamma_up", "gamma_down", "view_scale_0", "view_smaller",
                     "view_larger", "snapshot", NULL } },
    { "DEBUG",     "ONLY WORKS WITH THE --DEVMODE OPTION", { "show_version", "render_mode", "flush_textures", "wireframe_map", "dev_map_menu", "dev_open_door",
                     "noop_f7", "noop_n", "noop_t", NULL } },
};
#define NUM_TOPICS ((int)(sizeof KB_TOPICS / sizeof *KB_TOPICS))

static int topic_of_action(const char *name)
{
    for (int t = 0; t < NUM_TOPICS; t++)
        for (int k = 0; KB_TOPICS[t].names[k]; k++)
            if (!strcmp(KB_TOPICS[t].names[k], name)) return t;
    return NUM_TOPICS - 1;                          /* unlisted -> DEBUG */
}

static void add_row(int *n, int kind, int level, const char *title, const struct roth_setting_v1 *s, uint32_t idx)
{
    if (*n >= ROWS_MAX) return;
    row_t *r = &g_rows[(*n)++];
    r->kind = (uint8_t)kind; r->level = (uint8_t)level; r->title = title; r->s = s; r->idx = idx;
}

/* Build the whole menu once: MOUSE (built-ins + group "MOUSE" settings) | DISPLAY (resolution + every registry setting whose group is
 * "DISPLAY") | GAMEPLAY (every registry setting whose group is "GAMEPLAY") | one block per registering mod (its own groups as sub-headings) | KEY BINDINGS by topic. */
static void build_rows(const struct roth_api_v1 *api)
{
    int n = 0;
    uint32_t count = api->setting_count();
    add_row(&n, RK_HEADER, 0, "MOUSE", NULL, 0);
    add_row(&n, RK_YAW, 1, NULL, NULL, 0);
    add_row(&n, RK_PITCH, 1, NULL, NULL, 0);
    add_row(&n, RK_INVERT, 1, NULL, NULL, 0);
    for (uint32_t i = 0; i < count; i++) {                          /* registry settings with group "MOUSE" */
        const struct roth_setting_v1 *s = api->setting_at(i);
        if (s && s->group && !strcmp(s->group, "MOUSE")) add_row(&n, RK_REG, 1, NULL, s, i);
    }

    add_row(&n, RK_HEADER, 0, "DISPLAY", NULL, 0);
    add_row(&n, RK_RES, 1, NULL, NULL, 0);
    for (uint32_t i = 0; i < count; i++) {
        const struct roth_setting_v1 *s = api->setting_at(i);
        if (s && s->group && !strcmp(s->group, "DISPLAY")) add_row(&n, RK_REG, 1, NULL, s, i);
    }

    add_row(&n, RK_HEADER, 0, "GAMEPLAY", NULL, 0);
    for (uint32_t i = 0; i < count; i++) {
        const struct roth_setting_v1 *s = api->setting_at(i);
        if (s && s->group && !strcmp(s->group, "GAMEPLAY")) add_row(&n, RK_REG, 1, NULL, s, i);
    }

    const char *last_owner = NULL, *last_group = NULL;
    for (uint32_t i = 0; i < count; i++) {
        const struct roth_setting_v1 *s = api->setting_at(i);
        if (!s || (s->group && (!strcmp(s->group, "DISPLAY") || !strcmp(s->group, "MOUSE") || !strcmp(s->group, "GAMEPLAY")))) continue;   /* shown in the built-in sections */
        if (!last_owner || strcmp(last_owner, s->owner_id) != 0) {
            add_row(&n, RK_HEADER, 0, s->owner_name, NULL, 0);
            last_owner = s->owner_id; last_group = NULL;
        }
        if (s->group && (!last_group || strcmp(last_group, s->group) != 0)) {
            add_row(&n, RK_HEADER, 1, s->group, NULL, 0);
            last_group = s->group;
        } else if (!s->group) {
            last_group = NULL;                          /* an ungrouped setting sits directly under the owner */
        }
        add_row(&n, RK_REG, last_group ? 2 : 1, NULL, s, i);
    }

    add_row(&n, RK_HEADER, 0, "KEY BINDINGS", NULL, 0);
    for (int t = 0; t < NUM_TOPICS; t++) {
        int shown = 0;
        for (int a = 0; a < NUM_ACTIONS; a++) {
            if (topic_of_action(kb_actions[a].name) != t) continue;
            if (!shown) {
                add_row(&n, RK_HEADER, 1, KB_TOPICS[t].title, NULL, 0);
                if (KB_TOPICS[t].note) add_row(&n, RK_NOTE, 2, KB_TOPICS[t].note, NULL, 0);
                shown = 1;
            }
            add_row(&n, RK_ACTION, 2, NULL, NULL, (uint32_t)a);
        }
    }
    g_nrows = n;                                    /* published last */
    g_sel = 0;
    while (g_sel < n && !row_selectable(g_sel)) g_sel++;   /* start on the first real row, not a header */
}

/* on_frame_game (GAME): all file I/O and game_ram reads for display live here, never in
 * on_scancode. Also refreshes the action-row key display on open, and force-closes if a
 * menu/dialog/cutscene takes over movement while the overlay happened to be open. */
static void on_frame_game(const struct roth_api_v1 *api)
{
    static ui_state_t prev_state = ST_CLOSED;
    const struct roth_game_ram_api_v1 *gr = api->game_ram;

    if (!g_rows_built) { build_rows(api); g_rows_built = 1; }
    refresh_nav_keys(gr);
    if (g_commit_pending >= 0) {                    /* a registry value changed in the ISR: persist + notify */
        int c = g_commit_pending; g_commit_pending = -1;
        api->setting_commit((uint32_t)c);
    }

    /* PAUSE (like the native inventory): the engine's main loop only simulates in mode 1 of g_player_movement_enabled;
     * mode 2 is inert, so park the game there while the overlay is open and restore mode 1 on close — unless something
     * else changed the mode meanwhile (menu, death, load), in which case the overlay just closes. */
    {
        uint8_t mv = gr->u8(VA_PLAYER_MOVEMENT_ENABLED);
        if (g_overlay_open && !getenv("ROTH_SETTINGS_FORCE_OPEN")) {
            if (mv == 1 && !g_paused) { gr->set_u8(VA_PLAYER_MOVEMENT_ENABLED, 2); g_paused = 1; }
            else if (mv != (g_paused ? 2 : 1)) { g_state = ST_CLOSED; g_overlay_open = 0; g_paused = 0; }
        }
        if (!g_overlay_open && g_paused) {
            if (mv == 2) gr->set_u8(VA_PLAYER_MOVEMENT_ENABLED, 1);
            g_paused = 0;
        }
    }

    if (g_state != ST_CLOSED && prev_state == ST_CLOSED) {
        /* just opened: refresh everything from ground truth */
        api->engine->build_view_grayscale_lut();
        load_mouselook_display_values();
        for (int i = 0; i < NUM_ACTIONS; i++)
            g_action_sc[i] = find_action_key(gr, &kb_actions[i]);
        g_res_now = -1;                                 /* position of the running mode in the list */
        for (int i = 0; i < NUM_RES; i++)
            if ((int)gr->u32(VA_RES_INDEX) == RES_LIST[i].idx) g_res_now = i;
        if (g_res_now >= 0) g_res_sel = g_res_now;
        g_res_status[0] = '\0';
    }
    prev_state = g_state;

    if (g_res_apply) { g_res_apply = 0; apply_resolution(api); }
    if (g_state != ST_CLOSED) {                        /* keep "NOW" fresh after a live switch */
        g_res_now = -1;
        for (int i = 0; i < NUM_RES; i++)
            if ((int)gr->u32(VA_RES_INDEX) == RES_LIST[i].idx) g_res_now = i;
    }


    if (g_mouselook_dirty) {
        char yaw_s[32], pitch_s[32], inv_s[8];
        snprintf(yaw_s, sizeof yaw_s, "%.2f", g_disp_yaw);
        snprintf(pitch_s, sizeof pitch_s, "%.2f", g_disp_pitch);
        snprintf(inv_s, sizeof inv_s, "%d", g_disp_invert);
        const char *keys[3] = { "sensitivity", "pitch_scale", "invert_pitch" };
        const char *vals[3] = { yaw_s, pitch_s, inv_s };
        cfg_patch_file(g_mouselook_cfg, keys, vals, 3);
        g_mouselook_dirty = 0;
    }

    if (g_pending_rebind) {
        g_pending_rebind = 0;
        if (g_rebind_action_idx >= 0 && g_rebind_action_idx < NUM_ACTIONS) {
            uint8_t sc = g_pending_rebind_sc;
            const char *kname = keyname_of(sc);
            char hexbuf[8];
            if (!kname) { snprintf(hexbuf, sizeof hexbuf, "0x%02x", sc); kname = hexbuf; }
            const char *keys[1] = { kb_actions[g_rebind_action_idx].name };
            const char *vals[1] = { kname };
            cfg_patch_file(g_keybinds_cfg, keys, vals, 1);
            g_action_sc[g_rebind_action_idx] = sc; /* instant local preview */
        }
    }
}

/* ============================== drawing ============================== */
static void draw_value_bool(uint8_t *pixels, uint32_t w, uint32_t h, int x, int y, int on)
{
    draw_string(pixels, w, h, x, y, on ? "ON" : "OFF", COL_TEXT, 1);
}

/* 7x4 triangle: the scroll-indicator arrows (the 5x7 font has no arrow glyphs). */
static void draw_arrow(uint8_t *pixels, uint32_t w, uint32_t h, int cx, int y, int up, uint8_t color)
{
    for (int r = 0; r < 4; r++) {
        int wd = up ? 2 * r + 1 : 7 - 2 * r;
        fill_rect(pixels, w, h, cx - wd / 2, y + r, wd, 1, color);
    }
}

/* ---- grayscale background (like the native inventory): desaturate the 3D view at compose time ---- */
#define VA_GRAY_LUT       0x7f254u
#define VA_GRAY_LUT_VALID 0x7f354u
#define VA_VIEW_W 0x85cd8u
#define VA_VIEW_H 0x85cdcu
#define VA_VIEW_X 0x85ce0u
#define VA_VIEW_Y 0x85ce4u
#define VA_HIRES_DOUBLE 0x90cbdu

static void gray_view(const struct roth_api_v1 *api, uint8_t *px, uint32_t w, uint32_t h)
{
    const struct roth_game_ram_api_v1 *gr = api->game_ram;
    const uint8_t *lut = (const uint8_t *)gr->to_ptr(VA_GRAY_LUT);
    if (!lut || !gr->u8(VA_GRAY_LUT_VALID)) return;
    uint32_t vx = gr->u32(VA_VIEW_X), vy = gr->u32(VA_VIEW_Y), vw = gr->u32(VA_VIEW_W), vh = gr->u32(VA_VIEW_H);
    if (gr->u8(VA_HIRES_DOUBLE)) { vy *= 2; vh *= 2; }
    if (vx > w || vy > h || vw > w - vx || vh > h - vy) return;
    for (uint32_t y = vy; y < vy + vh; y++) {
        uint8_t *row = px + (size_t)y * w + vx;
        for (uint32_t x = 0; x < vw; x++) row[x] = lut[row[x]];
    }
}

static void on_compose_tick(const struct roth_api_v1 *api, uint8_t *pixels,
                            uint32_t width, uint32_t height)
{
    if (g_held && g_state == ST_MAIN) {                /* hold-to-repeat for the nav keys (see on_scancode) */
        uint16_t now = api->game_ram->u16(VA_TICK);
        if ((int16_t)(uint16_t)(now - g_held_next) >= 0) { nav_key(g_held); g_held_next = (uint16_t)(now + REPEAT_RATE_TICKS); }
    } else if (g_state != ST_MAIN) {
        g_held = 0;
    }
    static int pv_on;
    if (!g_overlay_open || !pixels) {
        if (pv_on) { preview_set(api, 0, 0, 0, 0); pv_on = 0; }   /* menu closed: drop the preview rectangle */
        return;
    }
    if (g_gray_bg) gray_view(api, pixels, width, height);

    int bw = (int)(width * 7 / 10), bh = (int)(height * 3 / 4);
    int bx = ((int)width - bw) / 2, by = ((int)height - bh) / 2;

    if (g_bg_style == 0) {
        fill_rect(pixels, width, height, bx, by, bw, bh, COL_BG);
    } else if (g_bg_style == 1) {                   /* see-through: every second pixel only (checkerboard) */
        for (int yy = by; yy < by + bh; yy++)
            for (int xx = bx + ((yy ^ bx) & 1); xx < bx + bw; xx += 2)
                plot(pixels, width, height, xx, yy, COL_BG);
    }
    draw_border(pixels, width, height, bx, by, bw, bh, 2, COL_BORDER);
    draw_string(pixels, width, height, bx + 12, by + 10, "SETTINGS", COL_TEXT, 2);

    int row_h = 12;
    int list_y = by + 36;
    int list_x = bx + 12;

    {   /* live display-options preview: the host draws a test scene into this rectangle (AA, upscaler, scanlines, CRT) */
        int pw = bw - 310;
        if (pw > 240) pw = 240;
        int ph = pw * 5 / 8, px = bx + bw - 30 - pw, py = list_y + 12;
        if (pw >= 120 && py + ph + 4 < by + bh - 16) {
            draw_string(pixels, width, height, px, list_y, "PREVIEW", COL_TEXT, 1);
            draw_border(pixels, width, height, px - 2, py - 2, pw + 4, ph + 4, 1, COL_BORDER);
            preview_set(api, (uint32_t)px, (uint32_t)py, (uint32_t)pw, (uint32_t)ph);
            pv_on = 1;
        } else if (pv_on) {
            preview_set(api, 0, 0, 0, 0);
            pv_on = 0;
        }
    }

    int vis = (by + bh - 16 - list_y) / row_h;       /* rows that fit above the footer (small modes have short boxes) */
    if (vis > VISIBLE_ROWS) vis = VISIBLE_ROWS;
    if (vis < 3) vis = 3;
    g_vis_rows = vis;
    if (g_scroll_top > 0)                              /* more rows above */
        draw_arrow(pixels, width, height, bx + bw - 18, list_y, 1, COL_TEXT);
    if (g_scroll_top + vis < NUM_ROWS)                 /* more rows below */
        draw_arrow(pixels, width, height, bx + bw - 18, list_y + (vis - 1) * row_h + 2, 0, COL_TEXT);

    for (int i = 0; i < vis; i++) {
        int row = g_scroll_top + i;
        if (row >= NUM_ROWS) break;
        int y = list_y + i * row_h;
        int selected = ((g_state == ST_MAIN || g_state == ST_CAPTURE) && row == g_sel);
        if (selected)
            draw_string(pixels, width, height, bx + 2, y, ">", COL_SEL, 1);

        char buf[48];
        const row_t *rr = &g_rows[row];
        int lx = list_x + rr->level * INDENT_PX, vx = list_x + 150;
        switch (rr->kind) {
        case RK_NOTE:
            draw_string(pixels, width, height, lx, y, rr->title, COL_TEXT, 1);
            break;
        case RK_HEADER: {
            char tt[40];                                  /* "Name (long description)" -> "Name" */
            snprintf(tt, sizeof tt, "%.36s", rr->title);
            char *par = strchr(tt, '('); if (par) { *par = '\0'; while (par > tt && par[-1] == ' ') *--par = '\0'; }
            snprintf(buf, sizeof buf, "- %.30s -", tt);
            draw_string(pixels, width, height, lx, y, buf, COL_TEXT, 1);
            break; }
        case RK_YAW:
            draw_string(pixels, width, height, lx, y, "YAW SENS", COL_TEXT, 1);
            snprintf(buf, sizeof buf, "%.2f", g_disp_yaw);
            draw_string(pixels, width, height, vx, y, buf, COL_TEXT, 1);
            break;
        case RK_PITCH:
            draw_string(pixels, width, height, lx, y, "PITCH SENS", COL_TEXT, 1);
            snprintf(buf, sizeof buf, "%.2f", g_disp_pitch);
            draw_string(pixels, width, height, vx, y, buf, COL_TEXT, 1);
            break;
        case RK_INVERT:
            draw_string(pixels, width, height, lx, y, "INVERT PITCH", COL_TEXT, 1);
            draw_value_bool(pixels, width, height, vx, y, g_disp_invert);
            break;
        case RK_RES: {
            draw_string(pixels, width, height, lx, y, "RESOLUTION", COL_TEXT, 1);
            int sp = g_res_sel;
            snprintf(buf, sizeof buf, "%s%s", RES_LIST[sp].name, sp == g_res_now ? " NOW" : "");
            draw_string(pixels, width, height, vx, y, buf, COL_TEXT, 1);
            if (g_res_status[0])
                draw_string(pixels, width, height, vx + 12 * 6, y, g_res_status, COL_TEXT, 1);
            else if (row == g_sel && sp != g_res_now)
                draw_string(pixels, width, height, vx + 12 * 6, y, "ENTER APPLIES", COL_TEXT, 1);
            break; }
        case RK_REG: {
            const struct roth_setting_v1 *rs = rr->s;
            draw_string(pixels, width, height, lx, y, rs->label, COL_TEXT, 1);
            if (rs->type == ROTH_SETTING_BOOL) {
                snprintf(buf, sizeof buf, "%s", *(const int32_t *)rs->value ? "ON" : "OFF");
            } else if (rs->type == ROTH_SETTING_INT) {
                snprintf(buf, sizeof buf, "%d", (int)*(const int32_t *)rs->value);
            } else if (rs->type == ROTH_SETTING_FLOAT) {
                snprintf(buf, sizeof buf, "%.2f", (double)*(const float *)rs->value);
            } else if (rs->type == ROTH_SETTING_ENUM) {
                int32_t v = *(const int32_t *)rs->value;
                snprintf(buf, sizeof buf, "%s", (v >= 0 && v <= rs->imax && rs->enum_names && rs->enum_names[v])
                                                ? rs->enum_names[v] : "?");
            } else {   /* KEY */
                int32_t v = *(const int32_t *)rs->value;
                const char *kn = disp_keyname((uint8_t)v);
                if (g_state == ST_CAPTURE && g_capture_row == row) snprintf(buf, sizeof buf, "PRESS A KEY");
                else if (kn) snprintf(buf, sizeof buf, "%s", kn);
                else snprintf(buf, sizeof buf, "0x%02X", (unsigned)v);
            }
            draw_string(pixels, width, height, vx, y, buf, COL_TEXT, 1);
            break; }
        case RK_ACTION: {
            int ai = (int)rr->idx;
            char namebuf[24];
            snprintf(namebuf, sizeof namebuf, "%.20s", kb_actions[ai].name);
            for (char *pc = namebuf; *pc; pc++) if (*pc == '_') *pc = ' ';
            draw_string(pixels, width, height, lx, y, namebuf, COL_TEXT, 1);
            const char *kn = disp_keyname(g_action_sc[ai]);
            char keybuf[16];
            if (kn) snprintf(keybuf, sizeof keybuf, "%s", kn);
            else    snprintf(keybuf, sizeof keybuf, "0x%02X", g_action_sc[ai]);
            draw_string(pixels, width, height, vx, y, keybuf, COL_TEXT, 1);
            break; }
        }
    }

    if (g_state == ST_REBIND || g_state == ST_CAPTURE) {
        int my = by + bh - 20;
        draw_string(pixels, width, height, list_x, my, "PRESS A KEY - ESC CANCELS", COL_TEXT, 1);
    }
}

static const struct roth_plugin_info_v1 SETTINGS = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = 11,   /* lowest host this menu runs on; set_display_preview (0.12) and set_gameplay_option (0.13) are optional, see preview_set / apply_gameplay_opts */
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "ire.roth.settings",
    .name          = "Menu (settings overlay: live mouselook, keybinds, display)",
    .version       = "0.1.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_GAME_RAM | ROTH_API_USE_COMPOSE | ROTH_API_USE_INPUT,
    .on_load               = on_load,
    .on_game_ram_ready     = NULL,
    .on_frame_game         = on_frame_game,
    .on_compose_tick       = on_compose_tick,
    .on_audio              = NULL,
    .on_unload             = NULL,
    .on_register_overrides = NULL,
    .on_scancode           = on_scancode,
    .on_mouse_poll         = on_mouse_poll,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &SETTINGS;
}
