/* sdl_host.c — the in-process SDL3 window for the moddable engine binary (roth) (task #102 / M1;
 * docs/SDL3_FOLD_DESIGN.md).
 *
 * Absorbs the *logic* of the external SDL2 viewer (src/platform/viewer/viewer.c) as NATIVE
 * SDL3 running on the MAIN thread: init video+events, window, renderer, one streaming ARGB8888
 * texture; a present loop that reads whatever the latest published g_shm frame is and pushes
 * input (set-1 make/break scancodes + relative mouselook deltas + buttons) back into the same
 * shm rings the game thread consumes today. The viewer's SDL2 calls are the semantics reference,
 * not a template — this is written to SDL3 idioms (design §2.7). Video only for M1; audio is M2
 * (with no viewer attached and SDL on, the run is silent — accepted for M1).
 *
 * Threading (design §3, "Model A"): SDL owns the main thread; the game + its SIGALRM ISR-surrogate
 * tick (shm_tick, the frame producer) run on a child thread with SIGALRM masked to them. This TU
 * therefore only ever READS the finished frame (g_shm->pixels/palette, guarded by ->frame) and
 * WRITES the input ring/mouse fields — the exact cross-process discipline the two-process host
 * used, now in one address space. No shared engine/mods state is touched.
 *
 * Guarded #ifdef ROTH_STANDALONE + linked only into the moddable engine binary (roth) — the trap host never sees
 * SDL. SDL symbols are SDL_* so the Makefile nm-clean assertion is unaffected.
 */
#ifdef ROTH_STANDALONE

#include <SDL3/SDL.h>
#include "shared_fb.h"
#include "sdl/sdl_audio.h"   /* M2: the in-process SDL3 audio consumer (PCM ring + MIDI->TSF) */
#include "present_channel.h" /* the SDK RGBA present channel + present pacing + window control (host side) */
#include "roth_sdk.h"        /* public timing/state/enumeration records used by the host plumbing */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/* The present-paced plugin dispatch lives in the loader (moddable flavor only); the vanilla flavor
 * links no loader, so this call site — and every present-channel / window-control call below — is
 * compiled out under -DROTH_VANILLA, leaving the present loop byte-for-byte the engine-only path. */
#ifndef ROTH_VANILLA
void plugins_dispatch_present_pace(const struct roth_present_timing_v1 *timing); /* plugin_loader.c */
int  plugins_any_loaded(void);                     /* plugin_loader.c */
#endif

/* the published framebuffer + input rings (defined in main.c, created by shm_setup()). The game
 * thread's shm_tick is the sole writer of pixels/palette/frame; we are the sole reader. We are the
 * sole writer of the key ring head + mouse fields; the game thread reads them (via dpmi.c mouse_int33
 * and traps.c shm_tick's ring drain). Single-producer/single-consumer each way — lock-free, exactly
 * as viewer.c <-> host over shm today. */
extern struct roth_shm *g_shm;

/* SDL_Scancode -> PC set-1 make code. 0 = unmapped. Extended keys carry a 0xE0 prefix flag in the
 * high byte (0x100). Carried over VERBATIM from viewer.c:29-85 — the SDL_SCANCODE_* enum is stable
 * across SDL2->SDL3 (design §2.7). */
#define EXT 0x100
static int sdl_to_set1(SDL_Scancode s)
{
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03;
    case SDL_SCANCODE_3: return 0x04; case SDL_SCANCODE_4: return 0x05;
    case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09;
    case SDL_SCANCODE_9: return 0x0a; case SDL_SCANCODE_0: return 0x0b;
    case SDL_SCANCODE_MINUS: return 0x0c; case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e; case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11;
    case SDL_SCANCODE_E: return 0x12; case SDL_SCANCODE_R: return 0x13;
    case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17;
    case SDL_SCANCODE_O: return 0x18; case SDL_SCANCODE_P: return 0x19;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: return 0x1c;
    case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_A: return 0x1e; case SDL_SCANCODE_S: return 0x1f;
    case SDL_SCANCODE_D: return 0x20; case SDL_SCANCODE_F: return 0x21;
    case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25;
    case SDL_SCANCODE_L: return 0x26; case SDL_SCANCODE_SEMICOLON: return 0x27;
    case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a; case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_Z: return 0x2c; case SDL_SCANCODE_X: return 0x2d;
    case SDL_SCANCODE_C: return 0x2e; case SDL_SCANCODE_V: return 0x2f;
    case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34; case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_LALT: return 0x38;
    case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_F1: return 0x3b; case SDL_SCANCODE_F2: return 0x3c;
    case SDL_SCANCODE_F3: return 0x3d; case SDL_SCANCODE_F4: return 0x3e;
    case SDL_SCANCODE_F5: return 0x3f; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42;
    case SDL_SCANCODE_F9: return 0x43; case SDL_SCANCODE_F10: return 0x44;
    case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    /* extended (0xE0-prefixed) keys */
    case SDL_SCANCODE_UP: return EXT | 0x48;
    case SDL_SCANCODE_DOWN: return EXT | 0x50;
    case SDL_SCANCODE_LEFT: return EXT | 0x4b;
    case SDL_SCANCODE_RIGHT: return EXT | 0x4d;
    case SDL_SCANCODE_RCTRL: return EXT | 0x1d;
    case SDL_SCANCODE_RALT: return EXT | 0x38;
    case SDL_SCANCODE_HOME: return EXT | 0x47;
    case SDL_SCANCODE_END: return EXT | 0x4f;
    case SDL_SCANCODE_PAGEUP: return EXT | 0x49;
    case SDL_SCANCODE_PAGEDOWN: return EXT | 0x51;
    case SDL_SCANCODE_INSERT: return EXT | 0x52;
    case SDL_SCANCODE_DELETE: return EXT | 0x53;
    case SDL_SCANCODE_KP_ENTER: return EXT | 0x1c;
    default: return 0;
    }
}

/* push one scancode byte into the shm key ring (we write head; the game thread reads tail) */
static void push_key(uint8_t code)
{
    g_shm->key_ring[g_shm->key_head & ROTH_KEY_MASK] = code;
    g_shm->key_head++;
}

static void send_key(SDL_Scancode s, int down)
{
    int code = sdl_to_set1(s);
    if (!code)
        return;
    if (code & EXT)
        push_key(0xe0);
    uint8_t make = code & 0xff;
    push_key(down ? make : (make | 0x80));
}

/* seqlock-lite frame read: the producer (shm_tick) writes the whole frame into ->pixels in one
 * memcpy and bumps ->frame LAST (traps.c single-pass staged publish + frame++). So reading ->frame
 * before and after the palette->RGBA convert, and retrying if it changed, means we never upload a
 * frame the producer was memcpy'ing through. ->frame isn't marked volatile in the (pointer-free,
 * cross-ABI) struct, so read it through a volatile lvalue to force the re-read. Bounded retry, then
 * present regardless — matches the shipping viewer's tolerance (it doesn't check ->frame at all),
 * and touches the producer NOT AT ALL (design §3 "optionally add a seqlock now that producer +
 * consumer share an address space"; the shm publish path is unchanged). */
static inline uint32_t rd_frame(void)
{
    return *(volatile uint32_t *)&g_shm->frame;
}

#ifndef ROTH_VANILLA
/* Refresh the informational display-mode snapshot whenever the window moves between displays. */
static void publish_display_modes(SDL_Window *win)
{
    struct roth_display_mode modes[128];
    int count = 0, available = 0;
    SDL_DisplayMode **list = SDL_GetFullscreenDisplayModes(SDL_GetDisplayForWindow(win), &available);
    if (list) {
        for (int i = 0; i < available && count < 128; i++) {
            modes[count].w = (uint32_t)list[i]->w;
            modes[count].h = (uint32_t)list[i]->h;
            modes[count].refresh_mhz = (uint32_t)(list[i]->refresh_rate * 1000.0f + 0.5f);
            count++;
        }
        SDL_free(list);
    }
    roth_window_publish_modes(modes, count);
}
#endif

/* M3 auto-sanity: can SDL video init here? Init + immediately quit the video subsystem, so main()
 * can fail cleanly (suggesting --headless) before spawning the game thread. See sdl_host.h. */
int sdl_video_preflight(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   /* keep our SIGTERM/SIGINT handlers intact */
    if (!SDL_Init(SDL_INIT_VIDEO)) {                 /* SDL3: bool return, true = ok */
        fprintf(stderr, "[sdl] SDL_Init(VIDEO) preflight: %s\n", SDL_GetError());
        return 0;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return 1;
}

int sdl_present_run(int scale)
{
    if (!g_shm) {
        fprintf(stderr, "[sdl] no shared framebuffer — cannot present\n");
        return -1;
    }

    /* The host owns SIGTERM (dump_screen) and SIGINT; the game-thread signal mask is set up in
     * main.c. Tell SDL to keep its hands off signal handlers so our teardown/screenshot path is
     * intact. Must be set before SDL_Init. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* crisp nearest-neighbour upscaling is set per-texture in SDL3 (SDL_SetTextureScaleMode below);
     * the SDL2 SDL_HINT_RENDER_SCALE_QUALITY global hint is gone (poisoned name in SDL3). */

    if (!SDL_Init(SDL_INIT_VIDEO)) {   /* SDL3: bool return, true = ok */
        fprintf(stderr, "[sdl] SDL_Init(VIDEO): %s\n", SDL_GetError());
        return -1;
    }

    /* Auto-pick a window scale that fills ~85% of the display height (viewer.c:229-238), so it's
     * comfortably large on hi-DPI screens; --scale / ROTH_SCALE override it. */
    if (scale < 1) {
        scale = 4;
        const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
        if (dm && dm->h > 0) {
            int s = (int)(dm->h * 0.85 / ROTH_FB_H);
            scale = s < 2 ? 2 : s;
        }
    }

    SDL_Window *win = SDL_CreateWindow("Realms of the Haunting (native host)",
                                       ROTH_FB_W * scale, ROTH_FB_H * scale,
                                       SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "[sdl] SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);   /* SDL3: no index; NULL = pick a backend */
    if (!ren) {
        fprintf(stderr, "[sdl] SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderVSync(ren, 1);   /* replaces SDL2's PRESENTVSYNC flag; paces the loop to the display */
    fprintf(stderr, "[sdl] renderer backend: %s\n", SDL_GetRendererName(ren));

    int cur_tex_w = ROTH_FB_W, cur_tex_h = ROTH_FB_H;
    int cur_log_w = ROTH_FB_W, cur_log_h = ROTH_FB_H;
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, cur_tex_w, cur_tex_h);
    if (!tex) {
        fprintf(stderr, "[sdl] SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return -1;
    }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    /* SDL_SetRenderLogicalPresentation subsumes the viewer's RenderSetLogicalSize + IntegerScale +
     * the aspect_w/aspect_h juggling (design §2.7, §12): the texture stays cur_w x cur_h (full
     * detail) and is fit to the logical aspect rectangle, letterboxed to the window. 320x400
     * double-scan publishes aspect 320x200, so this shows a 4:3-ish image, not a tall portrait. */
    SDL_SetRenderLogicalPresentation(ren, cur_log_w, cur_log_h, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    g_shm->viewer_alive = 1;

    /* Capture the pointer for mouselook: the in-game cursor is driven by relative motion
     * (int33 AX=000B -> g_shm->mouse_dx/dy), so we grab + hide the OS cursor and feed raw deltas.
     * Released on focus loss, re-grabbed on click. Sensitivity scales physical motion to in-game
     * cursor speed (viewer.c:255-301). */
    const char *se = getenv("ROTH_MOUSE_SENS");
    double sens = se ? atof(se) : 2.0;
    if (sens <= 0) sens = 2.0;
    SDL_SetWindowRelativeMouseMode(win, true);
    int grabbed = 1;
    double ax = 0, ay = 0; /* fractional delta accumulators */

    /* dev/verification hook (env-gated, one-shot): ROTH_SDL_SHOT=<path.bmp> reads back the rendered
     * frame with SDL_RenderReadPixels and SDL_SaveBMPs it — proof the WINDOW pipeline actually drew
     * the game frame (the readback is exactly what was presented, independent of compositor
     * visibility). ROTH_SDL_SHOT_FRAMES sets which present iteration to grab (default 180 ~ 3 s, so
     * the boot dialog has appeared). Off by default → zero cost. */
    const char *shot_path = getenv("ROTH_SDL_SHOT");
    int shot_at = 180, shot_done = 0, present_ct = 0;
    if (getenv("ROTH_SDL_SHOT_FRAMES"))
        shot_at = atoi(getenv("ROTH_SDL_SHOT_FRAMES"));

    static uint32_t rgb[ROTH_FB_MAX]; /* fits hi-res (640x480) */

#ifndef ROTH_VANILLA
    /* The SDK present channel / present pacing / window control run ONLY when a plugin is loaded. With
     * no plugin the present loop stays byte-for-byte the engine-only path (no per-iteration polling, no
     * dispatch, no channel acquire) — the zero-plugins-no-behavior-change invariant, timing included. */
    const int mods_on = plugins_any_loaded();
    /* second streaming texture for a mod's RGBA world image — created on the first publish and resized
     * when the published dimensions change. NULL until a plugin publishes a world frame, so until then
     * the draw path below is exactly the engine-only path (bit-identical to a build with no channel). */
    SDL_Texture *wtex = NULL;
    int wtex_w = 0, wtex_h = 0;
    /* Applied window state is owned here with the SDL objects and mirrored through a seqlock. */
    uint32_t cur_window_mode = ROTH_WINMODE_WINDOWED;
    uint32_t cur_present_mode = ROTH_PRESENT_FIFO;
    uint32_t cur_min_interval_us = 0;
    uint64_t interval_last_pc = 0;
    if (mods_on) {
        roth_window_publish_state(cur_window_mode, cur_present_mode, cur_min_interval_us,
                                  ROTH_OK, 0);
        publish_display_modes(win);
    }
#endif

    int running = 1;
    while (running && g_shm->host_alive) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = 0;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat)
                    send_key(e.key.scancode, 1);
                break;
            case SDL_EVENT_KEY_UP:
                send_key(e.key.scancode, 0);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (grabbed) {
                    /* scaled relative deltas drive the in-game cursor (AX=000B). xrel/yrel are
                     * float in SDL3; accumulate the fraction so slow motion still registers. */
                    ax += (double)e.motion.xrel * sens;
                    ay += (double)e.motion.yrel * sens;
                    int idx = (int)ax, idy = (int)ay;
                    ax -= idx; ay -= idy;
                    g_shm->mouse_dx += idx;
                    g_shm->mouse_dy += idy;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP: {
                if (!grabbed && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    SDL_SetWindowRelativeMouseMode(win, true); /* click to re-grab */
                    grabbed = 1;
                }
                float fx, fy;
                SDL_MouseButtonFlags st = SDL_GetMouseState(&fx, &fy);
                uint32_t b = 0;
                if (st & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) b |= 1;
                if (st & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT)) b |= 2;
                if (st & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) b |= 4;
                g_shm->mouse_buttons = b;
                break;
            }
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                SDL_SetWindowRelativeMouseMode(win, false); /* free cursor on alt-tab */
                grabbed = 0;
                break;
#ifndef ROTH_VANILLA
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
                if (mods_on)
                    publish_display_modes(win);
                break;
#endif
            default:
                break;
            }
        }

        /* M2: bring up in-process audio once the game thread's audio_init() has published the
         * /roth_audio ring (retriable no-op after it succeeds), and (env-gated) log the consumer
         * clock advancing. Runs on this main thread so the SDL audio thread inherits its blocked
         * signal mask. */
        sdl_audio_poll_open();
        sdl_audio_log_tick();

#ifndef ROTH_VANILLA
        if (mods_on) {
            /* All SDL mutation stays on this thread. Each consumed request publishes the resulting
             * applied state and advances the observable state generation, including backend failure. */
            uint32_t requested;
            if (roth_window_take_window_mode_req(&requested)) {
                int32_t result = SDL_SetWindowFullscreen(
                    win, requested == ROTH_WINMODE_BORDERLESS_FULLSCREEN)
                    ? ROTH_OK : ROTH_ERR_UNSUPPORTED;
                if (result == ROTH_OK)
                    cur_window_mode = requested;
                roth_window_publish_state(cur_window_mode, cur_present_mode, cur_min_interval_us,
                                          result, 1);
            }
            if (roth_window_take_present_mode_req(&requested)) {
                int vsync = requested == ROTH_PRESENT_FIFO ? 1 :
                            requested == ROTH_PRESENT_ADAPTIVE ? SDL_RENDERER_VSYNC_ADAPTIVE :
                            SDL_RENDERER_VSYNC_DISABLED;
                int32_t result = SDL_SetRenderVSync(ren, vsync)
                    ? ROTH_OK : ROTH_ERR_UNSUPPORTED;
                if (result == ROTH_OK)
                    cur_present_mode = requested;
                roth_window_publish_state(cur_window_mode, cur_present_mode, cur_min_interval_us,
                                          result, 1);
            }
            if (roth_window_take_min_interval_req(&requested)) {
                cur_min_interval_us = requested;
                roth_window_publish_state(cur_window_mode, cur_present_mode, cur_min_interval_us,
                                          ROTH_OK, 1);
            }
        }
#endif

        /* Follow the host's current resolution + intended display aspect (viewer.c:304-325). The
         * pixel buffer is cur_w x cur_h; the display aspect (aspect_w:aspect_h) may differ (320x400
         * publishes 320:200). Texture stays full-detail; logical presentation scales to the aspect. */
        int w = (int)g_shm->cur_w, h = (int)g_shm->cur_h;
        if (w <= 0 || w > ROTH_FB_MAXW) w = ROTH_FB_W;
        if (h <= 0 || h > ROTH_FB_MAXH) h = ROTH_FB_H;
        int aw = (int)g_shm->aspect_w, ah = (int)g_shm->aspect_h;
        if (aw <= 0 || aw > ROTH_FB_MAXW || ah <= 0 || ah > ROTH_FB_MAXH) {
            aw = w; ah = h; /* fallback: square pixels */
        }
        if (w != cur_tex_w || h != cur_tex_h) {
            SDL_DestroyTexture(tex);
            tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
            cur_tex_w = w; cur_tex_h = h;
        }
        if (aw != cur_log_w || ah != cur_log_h) {
            SDL_SetRenderLogicalPresentation(ren, aw, ah, SDL_LOGICAL_PRESENTATION_LETTERBOX);
            cur_log_w = aw; cur_log_h = ah;
        }

#ifndef ROTH_VANILLA
        if (mods_on)
            roth_present_set_engine_dimensions((uint32_t)w, (uint32_t)h);
#endif

#ifndef ROTH_VANILLA
        /* acquire the freshest published world frame; 0 -> present the engine's 8bpp frame unchanged. */
        struct roth_present_frame wf;
        int world_ready = mods_on ? roth_present_acquire(&wf) : 0;
        if (world_ready && (!wtex || wtex_w != (int)wf.w || wtex_h != (int)wf.h)) {
            if (wtex) SDL_DestroyTexture(wtex);
            wtex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, (int)wf.w, (int)wf.h);
            /* Nearest matches the engine texture: linear blurred the world
             * layer against the crisp 8bpp chrome at any window scale. */
            if (wtex) SDL_SetTextureScaleMode(wtex, SDL_SCALEMODE_NEAREST);
            wtex_w = (int)wf.w;
            wtex_h = (int)wf.h;
        }
        if (world_ready && !wtex)
            world_ready = 0;   /* keep the engine frame opaque if the auxiliary texture failed */
#endif

        /* Palette (6-bit VGA DAC) -> ARGB8888 under the frame guard. Inside an active viewport,
         * colorkey mode clears alpha only for the submitted palette index and world-only mode clears
         * it for every engine pixel. Outside the viewport the engine frame stays opaque. */
        int n = w * h;
        int tries = 0;
        uint32_t f0, f1;
        do {
            f0 = rd_frame();
#ifndef ROTH_VANILLA
            if (world_ready) {
                int vx = wf.vw ? wf.vx : 0, vy = wf.vh ? wf.vy : 0;
                int vr = wf.vw ? (wf.vx + (int)wf.vw) : w;
                int vb = wf.vh ? (wf.vy + (int)wf.vh) : h;
                for (int i = 0; i < n; i++) {
                    uint8_t idx = g_shm->pixels[i];
                    uint8_t r = g_shm->palette[idx * 3 + 0] << 2;
                    uint8_t g = g_shm->palette[idx * 3 + 1] << 2;
                    uint8_t b = g_shm->palette[idx * 3 + 2] << 2;
                    uint32_t a = 0xff000000u;
                    int px = i % w, py = i / w;
                    if (px >= vx && px < vr && py >= vy && py < vb) {
                        if (wf.composite_mode == ROTH_COMPOSITE_WORLD_ONLY ||
                            idx == (uint8_t)wf.transparent_index)
                            a = 0u;
                    }
                    rgb[i] = a | (r << 16) | (g << 8) | b;
                }
            } else
#endif
            {
                for (int i = 0; i < n; i++) {
                    uint8_t idx = g_shm->pixels[i];
                    uint8_t r = g_shm->palette[idx * 3 + 0] << 2;
                    uint8_t g = g_shm->palette[idx * 3 + 1] << 2;
                    uint8_t b = g_shm->palette[idx * 3 + 2] << 2;
                    rgb[i] = 0xff000000u | (r << 16) | (g << 8) | b;
                }
            }
            f1 = rd_frame();
        } while (f1 != f0 && ++tries < 2);

        SDL_UpdateTexture(tex, NULL, rgb, w * 4);
        SDL_RenderClear(ren);
#ifndef ROTH_VANILLA
        if (world_ready) {
            SDL_UpdateTexture(wtex, NULL, wf.rgba, (int)wf.w * 4);
            /* Map the engine-space viewport into the logical presentation rectangle. */
            float sx = (float)aw / (float)w, sy = (float)ah / (float)h;
            SDL_FRect wdst = { (float)wf.vx * sx, (float)wf.vy * sy,
                               (float)wf.vw * sx, (float)wf.vh * sy };
            SDL_RenderTexture(ren, wtex, NULL, &wdst);
            SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
            SDL_RenderTexture(ren, tex, NULL, NULL);
        } else
#endif
        {
            SDL_RenderTexture(ren, tex, NULL, NULL);
        }
        if (shot_path && !shot_done && ++present_ct >= shot_at) {
            SDL_Surface *sh = SDL_RenderReadPixels(ren, NULL);
            if (sh && SDL_SaveBMP(sh, shot_path))
                fprintf(stderr, "[sdl] screenshot -> %s (%dx%d)\n", shot_path, sh->w, sh->h);
            else
                fprintf(stderr, "[sdl] screenshot failed: %s\n", SDL_GetError());
            if (sh) SDL_DestroySurface(sh);
            shot_done = 1;
        }
#ifndef ROTH_VANILLA
        if (mods_on) {
            /* PRESENT context is delivered exactly once per loop, at the last coordination point
             * before the host presents. The callback cannot submit a frame in this ABI version. */
            struct roth_present_timing_v1 timing;
            roth_present_make_timing(&timing);
            roth_present_mark_present_callback(1);
            plugins_dispatch_present_pace(&timing);
            roth_present_mark_present_callback(0);
        }
#endif
        SDL_RenderPresent(ren); /* synchronization mode may pace the loop */
#ifndef ROTH_VANILLA
        /* Post-present minimum interval; synchronization and this interval compose, slower wins. */
        if (mods_on) {
            if (cur_min_interval_us > 0) {
                uint64_t freq = SDL_GetPerformanceFrequency();
                uint64_t target = ((uint64_t)cur_min_interval_us * freq) / 1000000ull;
                uint64_t now_pc = SDL_GetPerformanceCounter();
                if (interval_last_pc && (now_pc - interval_last_pc) < target)
                    SDL_DelayNS((target - (now_pc - interval_last_pc)) * 1000000000ull / freq);
                interval_last_pc = SDL_GetPerformanceCounter();
            } else {
                interval_last_pc = 0;
            }
        }
#endif
    }

    /* window closed (or the game exited): ask the game thread to quit, mirror viewer teardown. */
    g_shm->viewer_alive = 0;
    if (!running)
        g_shm->quit = 1; /* user closed the window: the game thread's shm_tick honors quit -> _exit */
    sdl_audio_shutdown(); /* M2: stop + join the audio callback, close the synth, quit AUDIO subsystem */
#ifndef ROTH_VANILLA
    if (wtex) SDL_DestroyTexture(wtex);
#endif
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

#endif /* ROTH_STANDALONE */
