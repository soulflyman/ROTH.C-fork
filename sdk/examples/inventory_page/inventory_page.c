/* inventory_page.c — single-page inventory overlay: every carried item, grouped by category, on
 * ONE screen (the native inventory shows one category per tab). Compiles against ONLY roth_sdk.h.
 *
 *   make            # -> plugin.so
 *   cp plugin.so <game>/mods/_inventory/plugin.so
 *
 * Toggle: F12 (ROTH_INVENTORY_TOGGLE_KEY=0xNN overrides), only during normal gameplay.
 * Keys while open: Up/Down move, Left/Right jump a column, PgUp/PgDn page, Enter equips the
 * selected weapon, Space (EXAMINE KEY setting) opens the engine's own item detail view, Esc or the toggle key closes. Every other key is swallowed.
 *
 * DATA. The carried array (game_ram 0x80c30: 256 x {s16 id, u16 qty}, count at 0x80c2c) is read from
 * game_ram every frame. Item names / categories are static game data, so they are loaded ONCE at
 * on_load straight from DBASE100.DAT + DBASE400.DAT in the game directory (mods/<name>/ -> ../..)
 * — no engine pointers, no pool-lifetime caveats. Format (mirrors roth_c/src/engine/inventory.c):
 *   DBASE100: u32 count @0x10 ; inventory table = hdr[0x14]-4 ; entry(id) = u32 @ table+id*4 (0=absent)
 *   record:   +4 flags (category = (flags>>8)&0xf, 0x10 weapon, 0x80 not pickable), +5 bit 0x20 stackable,
 *             +0x10 voice offset into DBASE400: {u32 clip, u16 len, u8 color} + text.
 * Categories (tools/dump_items.py): 0 items, 1 weapons+magazines, 2 dialogue topics, 3 artifacts,
 * 4 documents. Shown in the native tab order 0,1,3,4,2.
 *
 * KNOWN LIMITS (v1): text only (no icons); the engine hides a topic (category 2) whose dialogue node
 * isn't currently available, this overlay can't evaluate that and lists every carried topic;
 * item names the engine overrides at runtime (conditional text) show their base name.
 *
 * ACTIONS. Equip = the exact sequence the engine's number-key handler runs (SDK 0.7 exposed calls):
 * reset_weapon_fire_timing, activate_weapon_item(slot ptr, id), render_weapon_hud(1, attrs ptr).
 * EXAMINE = load_dbase300_resource_at_offset (SDK 0.9 slot 27), the modal popup the native inventory opens on Space.
 * It runs inside on_frame_game; while it does, the overlay is hidden and input passes through to the engine's
 * own popup loop. Items with no detail data (DBASE100 record +8 == 0) report NOTHING TO EXAMINE. No close-up icon yet.
 * Runs from on_frame_game (GAME context); on_scancode (TICK_ISR) only sets flags.
 *
 * Env: ROTH_INVENTORY_TOGGLE_KEY, ROTH_INVENTORY_SCALE (font scale, default 1),
 *      ROTH_INVENTORY_FORCE_OPEN=1 (dev/screenshot: open at boot, ignoring the gameplay gate),
 *      ROTH_INVENTORY_DEMO=1 (dev/screenshot: synthetic list from the item table, not the carried array).
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roth_sdk.h"

/* ---- 5x7 font (same ASCII-art-as-source scheme as the settings overlay) ---- */
#define GLYPH_W 5
#define GLYPH_H 7
static const char *const FONT_GLYPHS[49] = {
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
    "....." "....." "....." "....." "....." "....." ".....",  /* space (36) */
    "....." "....." "....." "....." "....." "..#.." ".....",  /* .  (37) */
    "....." "....." "....." "#####" "....." "....." ".....",  /* -  (38) */
    "#...#" "....#" "...#." "..#.." ".#..." "#...." "#...#",  /* %  (39) */
    "#...." "##..." "###.." "####." "###.." "##..." "#....",  /* >  (40) */
    "....." "..#.." ".###." "#####" ".###." "..#.." ".....",  /* *  (41) equipped marker */
    ".#..." "..#.." "...#." "...#." "...#." "..#.." ".#...",  /* )  (42) */
    "...#." "..#.." ".#..." ".#..." ".#..." "..#.." "...#.",  /* (  (43) */
    "....." "....." "....." "....." "....." ".#..." "#....",  /* ,  (44) */
    "..#.." "..#.." "..#.." "..#.." "..#.." "....." "..#..",  /* !  (45) */
    "....." "..#.." "....." "....." "....." "..#.." ".....",  /* :  (46) */
    ".###." "#...#" "....#" "...#." "..#.." "....." "..#..",  /* ?  (47) */
    "....#" "....#" "...#." "..#.." ".#..." "#...." "#....",  /* /  (48) */
};

static int glyph_index(char c)
{
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
    case '.': return 37;  case '-': return 38;  case '%': return 39;
    case '>': return 40;  case '*': return 41;  case ')': return 42;
    case '(': return 43;  case ',': return 44;  case '!': return 45;
    case ':': return 46;  case '?': return 47;  case '/': return 48;
    case '\'': return 44; case '&': return 37;  /* closest available glyphs */
    default:  return 36;
    }
}

static void fill_rect(uint8_t *px, uint32_t w, uint32_t h, int x, int y, int rw, int rh, uint8_t c)
{
    for (int yy = y; yy < y + rh; yy++) {
        if (yy < 0 || (uint32_t)yy >= h) continue;
        for (int xx = x; xx < x + rw; xx++)
            if (xx >= 0 && (uint32_t)xx < w) px[(uint32_t)yy * w + (uint32_t)xx] = c;
    }
}

static void draw_glyph(uint8_t *px, uint32_t w, uint32_t h, int x, int y, char ch, uint8_t c, int s)
{
    const char *g = FONT_GLYPHS[glyph_index(ch)];
    for (int r = 0; r < GLYPH_H; r++)
        for (int k = 0; k < GLYPH_W; k++)
            if (g[r * GLYPH_W + k] == '#') fill_rect(px, w, h, x + k * s, y + r * s, s, s, c);
}

static void draw_text(uint8_t *px, uint32_t w, uint32_t h, int x, int y, const char *t, int maxch, uint8_t c, int s)
{
    for (int i = 0; t[i] && i < maxch; i++)
        draw_glyph(px, w, h, x + i * (GLYPH_W + 1) * s, y, t[i], c, s);
}

/* Live-editable through the settings registry (SDK 0.8): rows appear in the F11 settings overlay. */
static int32_t g_text_color = 0xe8;   /* palette index */
static int32_t g_bg_style   = 0;      /* 0 solid, 1 see-through (checkerboard), 2 none */
static const char *const BG_STYLE_NAMES[3] = { "SOLID", "SEE-THROUGH", "NONE" };
#define COL_BG     0x00u
#define COL_BORDER ((uint8_t)g_text_color)
#define COL_TEXT   ((uint8_t)g_text_color)

/* ---- game_ram layout ---- */
#define VA_INV_COUNT        0x80c2cu
#define VA_INV_ARRAY        0x80c30u
#define INV_SLOTS           256
#define VA_EQUIPPED_SLOT    0x81038u  /* g_selected_item_secondary: equipped weapon's carried-slot ptr */
#define VA_SELECTED_SLOT    0x81044u  /* g_selected_item_primary: the "held" item slot ptr */
#define VA_WEAPON_ATTRS     0x811b4u
#define VA_PLAYER_MOVEMENT_ENABLED 0x7674au
#define VA_DBASE100_BASE    0x81e1cu  /* runtime ptr to the loaded DBASE100 (record = base + file offset) */
#define VA_PANEL_ANCHOR_X   0x80b24u
#define VA_PANEL_ANCHOR_Y   0x80b28u
#define VA_SCREEN_PITCH     0x85498u
#define VA_SCREEN_HEIGHT    0x8549cu

/* ---- static item data (loaded once from DBASE100/400) ---- */
#define MAX_ITEM_ID 1024
typedef struct { uint8_t cat, weapon, stack, valid, loop; uint32_t recoff, doc; char name[34]; } item_info_t;
static item_info_t g_items[MAX_ITEM_ID + 1];
static int g_items_loaded;
static int g_demo;   /* ROTH_INVENTORY_DEMO=1: synthetic list for screenshots */

static const uint8_t CAT_ORDER[5] = { 0, 1, 3, 4, 2 };
static const char *const CAT_TITLE[5] = { "ITEMS", "WEAPONS AND AMMO", "ARTIFACTS", "DOCUMENTS", "DIALOGUE TOPICS" };   /* indexed like CAT_ORDER */

static uint32_t rd32(const uint8_t *b, size_t n, size_t off) {
    return (off + 4 <= n) ? (uint32_t)b[off] | (uint32_t)b[off+1] << 8 | (uint32_t)b[off+2] << 16 | (uint32_t)b[off+3] << 24 : 0;
}
static uint32_t rd16(const uint8_t *b, size_t n, size_t off) {
    return (off + 2 <= n) ? (uint32_t)b[off] | (uint32_t)b[off+1] << 8 : 0;
}

static uint8_t *slurp(const char *dir, const char *const *names, size_t *len)
{
    for (int i = 0; names[i]; i++) {
        char path[2700];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *buf = (n > 0) ? (uint8_t *)malloc((size_t)n) : NULL;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) { fclose(f); *len = (size_t)n; return buf; }
        free(buf); fclose(f);
    }
    return NULL;
}

static void load_item_data(const char *game_dir)
{
    static const char *const n100[] = { "DBASE100.DAT", "dbase100.dat", "Dbase100.dat", "DBASE100.dat", NULL };
    static const char *const n400[] = { "DBASE400.DAT", "dbase400.dat", "Dbase400.dat", "DBASE400.dat", NULL };
    size_t l100 = 0, l400 = 0;
    uint8_t *db = slurp(game_dir, n100, &l100), *tx = slurp(game_dir, n400, &l400);
    if (!db || !tx) {
        fprintf(stderr, "[mods:_inventory] cannot read DBASE100/DBASE400 from %s — names unavailable\n", game_dir);
        free(db); free(tx); return;
    }
    uint32_t count = rd32(db, l100, 0x10);
    uint32_t table = rd32(db, l100, 0x14) - 4u;
    if (count > MAX_ITEM_ID) count = MAX_ITEM_ID;
    for (uint32_t id = 1; id <= count; id++) {
        uint32_t off = rd32(db, l100, table + id * 4u);
        if (off == 0 || off + 0x14 > l100) continue;
        item_info_t *it = &g_items[id];
        uint32_t flags = rd32(db, l100, off + 4);
        it->cat = (uint8_t)((flags >> 8) & 0xf);
        it->weapon = (flags & 0x10) != 0;
        it->stack = (db[off + 5] & 0x20) != 0;
        uint32_t voice = rd32(db, l100, off + 0x10);
        if (voice && voice + 8 <= l400) {
            uint32_t len = rd16(tx, l400, voice + 4);
            if (voice + 8 + len > l400) len = (uint32_t)(l400 - voice - 8);
            if (len > sizeof it->name - 1) len = sizeof it->name - 1;
            memcpy(it->name, tx + voice + 8, len);
            it->name[len] = '\0';
            char *z = (char *)memchr(it->name, '\0', sizeof it->name); (void)z;
        }
        it->recoff = off;
        it->doc = rd32(db, l100, off + 8);          /* detail-view (DBASE300 GDV/document) offset >> 3; 0 = none */
        it->loop = (uint8_t)(flags & 1);
        it->valid = 1;
    }
    free(db); free(tx);
    g_items_loaded = 1;
}

/* ---- list model (built in on_frame_game, drawn in on_compose_tick; double-buffered) ---- */
#define MAX_ENTRIES 320
typedef struct { uint8_t is_header, cat, weapon, equipped, held; int slot; int id; int qty; } entry_t;
typedef struct { int n; entry_t e[MAX_ENTRIES]; } list_t;
static list_t g_list[2];
static volatile int g_list_front;           /* index compose reads */

typedef enum { ST_CLOSED, ST_OPEN } ui_state_t;
static volatile ui_state_t g_state = ST_CLOSED;
static volatile int g_sel;                  /* index into the front list (always an item entry) */
static volatile int g_page;
static volatile int g_equip_request;        /* slot index + 1, 0 = none */
static volatile int g_inspect_request;      /* slot index + 1, 0 = none */
static volatile int g_inspecting;           /* engine detail popup running: overlay hidden, input passes through */
static int32_t g_inspect_key = 0x39;        /* Space, like the native inventory — a registered KEY setting */
static volatile int g_rows_per_col = 20, g_cols = 1, g_page_cap = 20;   /* published by compose */
static volatile int g_nav;                  /* pending navigation from the ISR: set, consumed in frame */
static int32_t g_toggle_key = 0x58;         /* F12 — a registered KEY setting */
static int g_scale = 1;
static char g_status[40];

static void rebuild_list(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *gr = api->game_ram;
    list_t *dst = &g_list[1 - g_list_front];
    dst->n = 0;
    uint32_t eq = gr->u32(VA_EQUIPPED_SLOT), held = gr->u32(VA_SELECTED_SLOT);
    uintptr_t base = (uintptr_t)gr->to_ptr(VA_INV_ARRAY);
    for (int ci = 0; ci < 5; ci++) {
        int hdr_at = -1;
        uint32_t seen = 0, total = gr->u32(VA_INV_COUNT);
        int demo_n = 0;
        for (int s = 0; s < INV_SLOTS && (g_demo || seen < total); s++) {
            int id = (int16_t)gr->u16(VA_INV_ARRAY + (uint32_t)s * 4u);
            if (g_demo) {   /* dev/screenshot: walk the item table instead of the (empty-at-boot) carried array */
                id = s + 1;
                if (id > MAX_ITEM_ID || !g_items[id].valid || g_items[id].cat != CAT_ORDER[ci] ||
                    !g_items[id].name[0] || demo_n >= 14) continue;
                demo_n++;
            } else {
                if (id == 0) continue;
                seen++;
            }
            if (id < 0 || id > MAX_ITEM_ID || !g_items[id].valid || g_items[id].cat != CAT_ORDER[ci]) continue;
            if (dst->n + 2 >= MAX_ENTRIES) break;
            if (hdr_at < 0) {
                hdr_at = dst->n;
                entry_t *h = &dst->e[dst->n++];
                memset(h, 0, sizeof *h); h->is_header = 1; h->cat = (uint8_t)ci;
            }
            entry_t *e = &dst->e[dst->n++];
            memset(e, 0, sizeof *e);
            e->cat = (uint8_t)ci; e->slot = s; e->id = id;
            e->qty = (int)gr->u16(VA_INV_ARRAY + (uint32_t)s * 4u + 2u);
            e->weapon = g_items[id].weapon && g_items[id].cat == 1;
            uint32_t sp = (uint32_t)(base + (uintptr_t)s * 4u);
            e->equipped = (sp == eq); e->held = (sp == held);
        }
    }
    g_list_front = 1 - g_list_front;
    list_t *cur = &g_list[g_list_front];
    int sel = g_sel;
    if (sel < 0 || sel >= cur->n || cur->e[sel].is_header) {   /* re-seat on the first item */
        sel = -1;
        for (int i = 0; i < cur->n; i++) if (!cur->e[i].is_header) { sel = i; break; }
        g_sel = sel < 0 ? 0 : sel;
    }
}

static int step_item(const list_t *l, int from, int dir)
{
    int i = from + dir;
    while (i >= 0 && i < l->n && l->e[i].is_header) i += dir;
    return (i >= 0 && i < l->n) ? i : from;
}

/* on_scancode (TICK_ISR): flags only. */
static uint8_t on_scancode(const struct roth_api_v1 *api, uint8_t sc)
{
    uint8_t make = (uint8_t)(sc & 0x7f);
    int is_break = (sc & 0x80) != 0;
    if (g_inspecting) return sc;
    if (g_state == ST_CLOSED) {
        if (!is_break && make == (uint8_t)g_toggle_key && api->game_ram->u8(VA_PLAYER_MOVEMENT_ENABLED) == 1) {
            g_state = ST_OPEN; return 0;
        }
        return sc;
    }
    if (is_break) return 0;
    const list_t *l = &g_list[g_list_front];
    int cap = g_page_cap > 0 ? g_page_cap : 1, rows = g_rows_per_col > 0 ? g_rows_per_col : 1;
    if (make == (uint8_t)g_toggle_key || make == 0x01) { g_state = ST_CLOSED; return 0; }
    if (make == 0x48) g_sel = step_item(l, g_sel, -1);
    else if (make == 0x50) g_sel = step_item(l, g_sel, 1);
    else if (make == 0x4b) { int t = g_sel - rows; if (t >= 0) g_sel = l->e[t].is_header ? step_item(l, t, 1) : t; }
    else if (make == 0x4d) { int t = g_sel + rows; if (t < l->n) g_sel = l->e[t].is_header ? step_item(l, t, 1) : t; }
    else if (make == 0x49) { int t = g_sel - cap; if (t < 0) t = 0; g_sel = l->e[t].is_header ? step_item(l, t, 1) : t; }
    else if (make == 0x51) { int t = g_sel + cap; if (t >= l->n) t = l->n - 1; g_sel = (t >= 0 && l->e[t].is_header) ? step_item(l, t, -1) : t; }
    else if (make == (uint8_t)g_inspect_key) {
        if (g_sel >= 0 && g_sel < l->n && !l->e[g_sel].is_header)
            g_inspect_request = l->e[g_sel].slot + 1;
    }
    else if (make == 0x1c) {
        if (g_sel >= 0 && g_sel < l->n && !l->e[g_sel].is_header && l->e[g_sel].weapon)
            g_equip_request = l->e[g_sel].slot + 1;
    }
    return 0;
}

static void on_mouse_poll(const struct roth_api_v1 *api, struct roth_mouse_poll_v1 *mp)
{
    (void)api;
    if (g_state == ST_CLOSED || g_inspecting) return;
    if (mp->ax == 0x000b) { mp->ecx &= ~0xffffu; mp->edx &= ~0xffffu; }
    else if (mp->ax == 0x0003) { mp->ebx &= ~0xffffu; }
}

static void on_frame_game(const struct roth_api_v1 *api)
{
    const struct roth_game_ram_api_v1 *gr = api->game_ram;
    static int was_open;
    if (g_state != ST_CLOSED && !getenv("ROTH_INVENTORY_FORCE_OPEN") &&
        gr->u8(VA_PLAYER_MOVEMENT_ENABLED) != 1) g_state = ST_CLOSED;
    if (g_state == ST_CLOSED) { was_open = 0; return; }
    if (!was_open) { g_sel = 0; g_status[0] = '\0'; was_open = 1; }
    rebuild_list(api);

    int req = g_equip_request;
    if (req) {
        g_equip_request = 0;
        int slot = req - 1;
        uint32_t sp = (uint32_t)((uintptr_t)gr->to_ptr(VA_INV_ARRAY) + (uintptr_t)slot * 4u);
        int id = (int16_t)gr->u16(VA_INV_ARRAY + (uint32_t)slot * 4u);
        if (id <= 0 || id > MAX_ITEM_ID || !g_items[id].weapon || g_items[id].cat != 1) return;
        if (sp == gr->u32(VA_EQUIPPED_SLOT)) { snprintf(g_status, sizeof g_status, "ALREADY EQUIPPED"); return; }
        if (sp == gr->u32(VA_SELECTED_SLOT)) { snprintf(g_status, sizeof g_status, "HELD - PUT IT AWAY FIRST"); return; }
        api->engine->reset_weapon_fire_timing();
        api->engine->activate_weapon_item(sp, (uint32_t)id);
        api->engine->render_weapon_hud(1, (uint32_t)(uintptr_t)gr->to_ptr(VA_WEAPON_ATTRS));
        snprintf(g_status, sizeof g_status, "EQUIPPED");
        return;
    }

    req = g_inspect_request;
    if (req) {
        g_inspect_request = 0;
        int id = (int16_t)gr->u16(VA_INV_ARRAY + (uint32_t)(req - 1) * 4u);
        uint32_t dbase = gr->u32(VA_DBASE100_BASE);
        if (id <= 0 || id > MAX_ITEM_ID || !g_items[id].valid || !dbase) return;
        const item_info_t *it = &g_items[id];
        if (it->doc == 0) { snprintf(g_status, sizeof g_status, "NOTHING TO EXAMINE"); return; }
        /* the popup centres itself on these two panel-origin globals (compute_viewport_half_extents) */
        gr->set_u32(VA_PANEL_ANCHOR_Y, gr->u32(VA_SCREEN_HEIGHT) >> 1);
        gr->set_u32(VA_PANEL_ANCHOR_X, (gr->u32(VA_SCREEN_PITCH) - 0x130u) >> 1);
        static uint32_t hov[3];   /* {close-up icon handle (none), carried slot ptr, item id} — game-side 12-byte entry */
        hov[0] = 0;
        hov[1] = (uint32_t)((uintptr_t)gr->to_ptr(VA_INV_ARRAY) + (uintptr_t)(req - 1) * 4u);
        hov[2] = (uint32_t)id;
        g_inspecting = 1;
        api->engine->load_dbase300_resource_at_offset((it->doc << 3) + 4u, it->loop, dbase + it->recoff, (uint32_t)(uintptr_t)hov);
        g_inspecting = 0;
        g_status[0] = '\0';
    }
}

static void on_compose_tick(const struct roth_api_v1 *api, uint8_t *px, uint32_t w, uint32_t h)
{
    (void)api;
    if (g_state == ST_CLOSED || !px || g_inspecting) return;
    const list_t *l = &g_list[g_list_front];
    int s = g_scale, adv = (GLYPH_W + 1) * s, rowh = (GLYPH_H + 4) * s;

    int bw = (int)(w * 9 / 10), bh = (int)(h * 9 / 10);
    int bx = ((int)w - bw) / 2, by = ((int)h - bh) / 2;
    if (g_bg_style == 0) {
        fill_rect(px, w, h, bx, by, bw, bh, COL_BG);
    } else if (g_bg_style == 1) {                   /* see-through: every second pixel only */
        for (int yy = by; yy < by + bh; yy++)
            for (int xx = bx + ((yy ^ bx) & 1); xx < bx + bw; xx += 2)
                if (yy >= 0 && (uint32_t)yy < h && xx >= 0 && (uint32_t)xx < w) px[(uint32_t)yy * w + (uint32_t)xx] = COL_BG;
    }
    fill_rect(px, w, h, bx, by, bw, 2, COL_BORDER);
    fill_rect(px, w, h, bx, by + bh - 2, bw, 2, COL_BORDER);
    fill_rect(px, w, h, bx, by, 2, bh, COL_BORDER);
    fill_rect(px, w, h, bx + bw - 2, by, 2, bh, COL_BORDER);
    draw_text(px, w, h, bx + 10, by + 8, "INVENTORY", 40, COL_TEXT, s * 2 > 3 ? 3 : s * 2 > 1 ? 2 : 1);

    int top = by + 8 + (GLYPH_H * 2 + 8) * 1 + 6, foot = 14 * s;
    int colw = 30 * adv + 8 * s;
    int cols = (bw - 20) / colw; if (cols < 1) cols = 1;
    int rows = (by + bh - foot - top) / rowh; if (rows < 1) rows = 1;
    int cap = rows * cols;
    g_rows_per_col = rows; g_cols = cols; g_page_cap = cap;

    int sel = g_sel; if (sel < 0) sel = 0;
    int page = (l->n > 0) ? sel / cap : 0;
    int first = page * cap;
    int maxch = colw / adv - 2;
    for (int i = 0; i < cap && first + i < l->n; i++) {
        const entry_t *e = &l->e[first + i];
        int col = i / rows, row = i % rows;
        int x = bx + 10 + col * colw, y = top + row * rowh;
        if (e->is_header) {
            draw_text(px, w, h, x, y, CAT_TITLE[e->cat], maxch, COL_TEXT, s);
            fill_rect(px, w, h, x, y + GLYPH_H * s + 1, colw - 8 * s, 1, COL_BORDER);
            continue;
        }
        if (first + i == sel) draw_text(px, w, h, x, y, ">", 1, COL_TEXT, s);
        if (e->equipped) draw_text(px, w, h, x + adv, y, "*", 1, COL_TEXT, s);
        char buf[48];
        const item_info_t *it = &g_items[e->id];
        const char *nm = it->name[0] ? it->name : "?";
        if (it->stack && e->qty > 1) snprintf(buf, sizeof buf, "%s (%d)", nm, e->qty);
        else snprintf(buf, sizeof buf, "%s", nm);
        draw_text(px, w, h, x + 2 * adv + 2, y, buf, maxch - 2, COL_TEXT, s);
    }
    if (l->n == 0)
        draw_text(px, w, h, bx + 12, top, "NOTHING CARRIED", 40, COL_TEXT, s);

    char foot_buf[128];
    int pages = (l->n + cap - 1) / cap; if (pages < 1) pages = 1;
    snprintf(foot_buf, sizeof foot_buf, "ARROWS MOVE  ENTER EQUIP  SPACE EXAMINE  ESC CLOSE   PAGE %d/%d   %s", page + 1, pages, g_status);
    draw_text(px, w, h, bx + 10, by + bh - foot + 2, foot_buf, (bw - 20) / adv, COL_TEXT, s);
}

static void on_load(const struct roth_api_v1 *api)
{
    if (getenv("ROTH_INVENTORY_TOGGLE_KEY"))        /* seeds the default; a saved registry value still wins */
        g_toggle_key = (int32_t)(strtoul(getenv("ROTH_INVENTORY_TOGGLE_KEY"), NULL, 0) & 0x7f);
    {
        struct roth_setting_v1 d;
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "toggle_key"; d.label = "INVENTORY KEY"; d.type = ROTH_SETTING_KEY; d.value = &g_toggle_key;
        api->register_setting(&d);
        memset(&d, 0, sizeof d);
        d.struct_size = (uint32_t)sizeof d;
        d.key = "inspect_key"; d.label = "EXAMINE KEY"; d.type = ROTH_SETTING_KEY; d.value = &g_inspect_key;
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
    }
    if (getenv("ROTH_INVENTORY_SCALE")) {
        g_scale = atoi(getenv("ROTH_INVENTORY_SCALE"));
        if (g_scale < 1) g_scale = 1;
        if (g_scale > 3) g_scale = 3;
    }
    char dir[2600];
    snprintf(dir, sizeof dir, "%s", api->plugin_dir());          /* <game>/mods/_inventory */
    for (int up = 0; up < 2; up++) {                              /* -> <game> */
        char *sl = strrchr(dir, '/');
#ifdef _WIN32
        char *bs = strrchr(dir, '\\'); if (bs && (!sl || bs > sl)) sl = bs;
#endif
        if (sl) *sl = '\0';
    }
    const char *env = getenv("ROTH_INVENTORY_GAME_DIR");
    load_item_data(env ? env : dir);
    g_demo = getenv("ROTH_INVENTORY_DEMO") != NULL;
    if (getenv("ROTH_INVENTORY_FORCE_OPEN")) g_state = ST_OPEN;
    fprintf(stderr, "[mods:_inventory] on_load: toggle_sc=0x%02x scale=%d items_loaded=%d game_dir=%s\n",
            (unsigned)g_toggle_key, g_scale, g_items_loaded, env ? env : dir);
}

static const struct roth_plugin_info_v1 INVPAGE = {
    .abi_major     = ROTH_ABI_MAJOR,
    .abi_minor     = ROTH_ABI_MINOR,
    .struct_size   = sizeof(struct roth_plugin_info_v1),
    .id            = "ire.roth.inventory_page",
    .name          = "Inventory (single-page overlay)",
    .version       = "0.1.0",
    .sdk_req_major = ROTH_SDK_MAJOR,
    .sdk_req_minor = ROTH_SDK_MINOR,
    .api_use       = ROTH_API_USE_GAME_RAM | ROTH_API_USE_ENGINE | ROTH_API_USE_COMPOSE | ROTH_API_USE_INPUT,
    .on_load       = on_load,
    .on_frame_game = on_frame_game,
    .on_compose_tick = on_compose_tick,
    .on_scancode   = on_scancode,
    .on_mouse_poll = on_mouse_poll,
};

ROTH_PLUGIN_EXPORT const struct roth_plugin_info_v1 *roth_plugin_query_v1(void)
{
    return &INVPAGE;
}
