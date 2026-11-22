/* Shared-memory contract between the 32-bit game host and the SDL2 viewer.
 *
 * The host (32-bit, runs the original code) publishes the mode-13h framebuffer
 * + VGA DAC palette here every timer tick; the viewer (any arch, system SDL2)
 * mmaps the same region, blits it to a window, and pushes input events back
 * through the rings. Decoupling display/IO from the CPU host keeps the core
 * portable and sidesteps needing a 32-bit SDL build.
 *
 * Layout is fixed-size and pointer-free so both ABIs agree. */
#ifndef ROTH_SHARED_FB_H
#define ROTH_SHARED_FB_H

#include <stdint.h>

#define ROTH_SHM_NAME  "/roth_fb"
/* v2 (display-pipeline-overhaul): ROTH_FB_MAXW grew 640->896, which shifts every struct field
 * after pixels[] (key_head oonward) — a real ABI break for any stale prebuilt consumer. Bumping
 * the magic makes a stale roth-inject/dumpfb/viewer refuse to attach instead of silently reading
 * garbage at the old offsets; rebuild all of them together (same Makefile) whenever this changes. */
#define ROTH_SHM_MAGIC 0x324f5452u /* 'RTO2' — v2 layout (ROTH_FB_MAXW=896, + filter_mode) */
#define ROTH_FB_W 320              /* default (mode 13h) */
#define ROTH_FB_H 200
#define ROTH_FB_MAXW 896           /* widest supported mode (854x480, display-pipeline-overhaul);
                                    * rounded up from 854 to a multiple of 64 for headroom */
#define ROTH_FB_MAXH 480
#define ROTH_FB_MAX (ROTH_FB_MAXW * ROTH_FB_MAXH)
#define ROTH_KEY_RING 256
#define ROTH_KEY_MASK (ROTH_KEY_RING - 1)

/* Keyboard events are raw PC set-1 scancodes; bit 7 set = key release (break),
 * exactly as port 0x60 delivers them. Extended (0xE0-prefixed) keys send the
 * 0xE0 byte as its own ring entry, then the code. */
struct roth_shm {
    uint32_t magic;
    uint32_t frame;            /* bumped after each full publish */
    uint32_t width, height;    /* max dimensions (buffer capacity) */
    uint32_t cur_w, cur_h;     /* current mode's active resolution */
    uint8_t palette[768];      /* 6-bit VGA DAC values */
    uint8_t pixels[ROTH_FB_MAX];

    /* keyboard ring: viewer writes head, host reads tail */
    volatile uint32_t key_head;
    volatile uint32_t key_tail;
    uint8_t key_ring[ROTH_KEY_RING];

    /* mouse state, viewer-owned */
    volatile int32_t mouse_x, mouse_y;   /* 0..319, 0..199 (absolute) */
    volatile int32_t mouse_dx, mouse_dy; /* accumulated relative motion (mickeys);
                                            host reads & subtracts what it consumed */
    volatile uint32_t mouse_buttons;     /* bit0 left, bit1 right, bit2 mid */

    volatile uint32_t viewer_alive;
    volatile uint32_t host_alive;
    volatile uint32_t quit;              /* viewer sets to ask host to exit */
    /* debug probe reach-counts (--probe-blend): [0]=mid-texture 0x2bc3c,
       [1]=special translucent 0x2d70c, [2]=world span blend 0x2dc27 */
    volatile uint32_t probe[4];

    /* Intended display aspect ratio (w:h) for the current frame. The pixel
     * buffer is cur_w x cur_h, but line-doubled modes (320x400) should display
     * at their logical aspect (320x200) so they fill a 4:3-ish screen as the
     * original CRT did, not as a tall/narrow portrait. 0 => use cur_w:cur_h. */
    volatile uint32_t aspect_w, aspect_h;

    /* Final upscale filter for the in-process SDL presenter: 0 = nearest (crisp,
     * default), nonzero = linear (softer). Seeded from --filter/ROTH_FILTER at
     * boot; sdl_host.c re-reads it every frame so a plugin can change it live via
     * roth_api_v1->set_display_filter() (sdk/include/roth_sdk.h). */
    volatile uint32_t filter_mode;

    /* filter_mode is a flag word: bit 0 = linear upscale filter, bit 1 = edge anti-aliasing of the 3D view
     * (sdl_host.c), bits 8-15 = AA edge contrast threshold (0 = default 24 / ROTH_AA_EDGE), bits 16-19 = upscaler id (0 nearest, 1 linear, 2 sharp-bilinear, 3 scale2x), bit 20 = integer scaling, bits 24-27 = scanline strength 0-8, bits 28-29 = CRT look 0-2, bits 30-31 = window mode (0 windowed, 1 fullscreen window, 2 fullscreen). The AA pass needs to know WHERE the 3D scene is and which pixels a plugin drew over it:
     *   view_*: the 3D view rectangle in published-frame pixels, published by traps.c only during normal
     *           gameplay (all 0 otherwise: menus, cutscenes, title screens are never smoothed);
     *   ovl_*:  bounding box of the pixels the plugins' compose seam changed this tick (empty when x1 <= x0),
     *           maintained by plugin_loader.c only while AA is on — overlays/text stay crisp. */
    volatile uint32_t view_x, view_y, view_w, view_h;
    volatile uint32_t ovl_x0, ovl_y0, ovl_x1, ovl_y1;

    /* Live display-options preview (roth_api_v1->set_display_preview): the presenter draws a built-in test scene into this
     * rectangle of the published frame, rendered with the current display options. pre_w == 0 = off. */
    volatile uint32_t pre_x, pre_y, pre_w, pre_h;
};

#endif
