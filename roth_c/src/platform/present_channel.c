/* present_channel.c — portable host implementation of the SDK world-frame channel, present
 * timing, and window-control mailboxes. SDL calls remain in sdl_host.c on the present thread.
 */
#include "present_channel.h"
#include "roth_sdk.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ============================================================ world present channel =========== */
#define WSLOT_IDX_MASK 3u
#define WSLOT_FRESH    4u
#define WSTALE_LIMIT   16

struct wslot {
    uint32_t *px;
    uint32_t cap_px;
    uint32_t w, h;
    int32_t vx, vy;
    uint32_t vw, vh;
    uint32_t composite_mode;
    uint32_t transparent_index;
    uint32_t serial;
};

static struct wslot g_slot[3];
static int g_producer = 0;                 /* GAME thread only */
static int g_consumer = 1;                 /* present thread only */
static unsigned g_ready = 2;               /* slot index plus WSLOT_FRESH */
static uint32_t g_frame_serial;            /* GAME thread only */
static int g_seen;                         /* present thread only */
static int g_stale;                        /* present thread only */
static uintptr_t g_owner;                  /* per-plugin roth_api_v1 address, zero when unowned */
static uint32_t g_active;                  /* zero after withdraw/unload */
static uint32_t g_engine_w = 320;
static uint32_t g_engine_h = 200;
static __thread int g_in_present_callback;

void roth_present_set_engine_dimensions(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0)
        return;
    __atomic_store_n(&g_engine_w, w, __ATOMIC_RELEASE);
    __atomic_store_n(&g_engine_h, h, __ATOMIC_RELEASE);
}

static int clip_viewport(const struct roth_world_frame_v1 *f,
                         int32_t *vx, int32_t *vy, uint32_t *vw, uint32_t *vh)
{
    if (f->vw == 0 || f->vh == 0)
        return 0;

    int64_t left = f->vx;
    int64_t top = f->vy;
    int64_t right = left + (int64_t)f->vw;
    int64_t bottom = top + (int64_t)f->vh;
    if (right > INT32_MAX || right < INT32_MIN ||
        bottom > INT32_MAX || bottom < INT32_MIN)
        return 0;

    uint32_t ew = __atomic_load_n(&g_engine_w, __ATOMIC_ACQUIRE);
    uint32_t eh = __atomic_load_n(&g_engine_h, __ATOMIC_ACQUIRE);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int64_t)ew) right = ew;
    if (bottom > (int64_t)eh) bottom = eh;
    if (right <= left || bottom <= top)
        return 0;

    *vx = (int32_t)left;
    *vy = (int32_t)top;
    *vw = (uint32_t)(right - left);
    *vh = (uint32_t)(bottom - top);
    return 1;
}

int32_t roth_present_submit_world_frame(const struct roth_api_v1 *api,
                                        const struct roth_world_frame_v1 *frame)
{
    if (g_in_present_callback)
        return ROTH_ERR_UNSUPPORTED;
    if (!api || !frame ||
        frame->struct_size < (uint32_t)sizeof(struct roth_world_frame_v1) ||
        !frame->pixels || frame->w == 0 || frame->h == 0 || frame->flags != 0)
        return ROTH_ERR_INVALID;
    if (frame->w > ROTH_WORLD_MAX_W || frame->h > ROTH_WORLD_MAX_H)
        return ROTH_ERR_RANGE;
    if (frame->format != ROTH_PIXFMT_ARGB8888)
        return ROTH_ERR_UNSUPPORTED;
    if (frame->composite_mode != ROTH_COMPOSITE_ENGINE_COLORKEY_OVER_WORLD &&
        frame->composite_mode != ROTH_COMPOSITE_WORLD_ONLY)
        return ROTH_ERR_INVALID;
    if (frame->composite_mode == ROTH_COMPOSITE_ENGINE_COLORKEY_OVER_WORLD &&
        frame->transparent_index > 255u)
        return ROTH_ERR_RANGE;

    uint32_t row_bytes = frame->w * 4u;
    uint32_t pitch = frame->pitch_bytes ? frame->pitch_bytes : row_bytes;
    if (pitch < row_bytes)
        return ROTH_ERR_INVALID;
    if ((uint64_t)(frame->h - 1u) * pitch + row_bytes > UINT32_MAX)
        return ROTH_ERR_INVALID;

    int32_t vx, vy;
    uint32_t vw, vh;
    if (!clip_viewport(frame, &vx, &vy, &vw, &vh))
        return ROTH_ERR_INVALID;

    uintptr_t owner = __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE);
    if (owner != 0 && owner != (uintptr_t)api)
        return ROTH_ERR_NOT_OWNER;

    struct wslot *s = &g_slot[g_producer];
    uint32_t need = frame->w * frame->h;
    if (s->cap_px < need) {
        uint32_t *np = (uint32_t *)realloc(s->px, (size_t)need * sizeof(uint32_t));
        if (!np)
            return ROTH_ERR_NOMEM;
        s->px = np;
        s->cap_px = need;
    }
    const uint8_t *src = (const uint8_t *)frame->pixels;
    uint8_t *dst = (uint8_t *)s->px;
    for (uint32_t y = 0; y < frame->h; y++)
        memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * pitch, row_bytes);

    if (owner == 0) {
        uintptr_t expected = 0;
        if (!__atomic_compare_exchange_n(&g_owner, &expected, (uintptr_t)api, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return ROTH_ERR_NOT_OWNER;
    }

    s->w = frame->w;
    s->h = frame->h;
    s->vx = vx;
    s->vy = vy;
    s->vw = vw;
    s->vh = vh;
    s->composite_mode = frame->composite_mode;
    s->transparent_index = frame->transparent_index;
    s->serial = ++g_frame_serial;

    __atomic_store_n(&g_active, 1u, __ATOMIC_RELEASE);
    unsigned prev = __atomic_exchange_n(&g_ready, (unsigned)g_producer | WSLOT_FRESH,
                                        __ATOMIC_ACQ_REL);
    g_producer = (int)(prev & WSLOT_IDX_MASK);
    return ROTH_OK;
}

int32_t roth_present_withdraw_world_frame(const struct roth_api_v1 *api)
{
    if (!api)
        return ROTH_ERR_INVALID;
    if (__atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) != (uintptr_t)api)
        return ROTH_ERR_NOT_OWNER;
    __atomic_store_n(&g_active, 0u, __ATOMIC_RELEASE);
    return ROTH_OK;
}

int32_t roth_present_get_present_caps(const struct roth_api_v1 *api,
                                      struct roth_present_caps_v1 *caps)
{
    (void)api;
    if (!caps || caps->struct_size < (uint32_t)sizeof(*caps))
        return ROTH_ERR_INVALID;
    caps->struct_size = (uint32_t)sizeof(*caps);
    caps->max_w = ROTH_WORLD_MAX_W;
    caps->max_h = ROTH_WORLD_MAX_H;
    caps->formats = ROTH_PIXFMT_ARGB8888;
    return ROTH_OK;
}

void roth_present_release_owner(const struct roth_api_v1 *api)
{
    if (!api || __atomic_load_n(&g_owner, __ATOMIC_ACQUIRE) != (uintptr_t)api)
        return;
    __atomic_store_n(&g_active, 0u, __ATOMIC_RELEASE);
    uintptr_t expected = (uintptr_t)api;
    __atomic_compare_exchange_n(&g_owner, &expected, (uintptr_t)0, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

int roth_present_acquire(struct roth_present_frame *out)
{
    if (!out || !__atomic_load_n(&g_active, __ATOMIC_ACQUIRE))
        return 0;

    unsigned r = __atomic_load_n(&g_ready, __ATOMIC_ACQUIRE);
    if (r & WSLOT_FRESH) {
        unsigned got = __atomic_exchange_n(&g_ready, (unsigned)g_consumer, __ATOMIC_ACQ_REL);
        g_consumer = (int)(got & WSLOT_IDX_MASK);
        g_seen = 1;
        g_stale = 0;
    } else if (g_seen) {
        g_stale++;
    }
    if (!__atomic_load_n(&g_active, __ATOMIC_ACQUIRE))
        return 0;

    struct wslot *s = &g_slot[g_consumer];
    if (!g_seen || s->serial == 0 || g_stale >= WSTALE_LIMIT)
        return 0;
    out->rgba = s->px;
    out->w = s->w;
    out->h = s->h;
    out->vx = s->vx;
    out->vy = s->vy;
    out->vw = s->vw;
    out->vh = s->vh;
    out->composite_mode = s->composite_mode;
    out->transparent_index = s->transparent_index;
    return 1;
}

/* ============================================================ present pacing =================== */
static uint32_t g_pace_seq;
static uint32_t g_tick_serial;
static uint64_t g_prev_tick_ns;
static uint64_t g_last_tick_ns;
static uint32_t g_present_serial;           /* present thread only */

void roth_present_note_tick(void)
{
    uint64_t t = monotonic_ns();
    uint32_t seq = __atomic_load_n(&g_pace_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_pace_seq, seq + 1u, __ATOMIC_RELEASE);
    g_prev_tick_ns = g_last_tick_ns;
    g_last_tick_ns = t;
    g_tick_serial++;
    __atomic_store_n(&g_pace_seq, seq + 2u, __ATOMIC_RELEASE);
}

void roth_present_make_timing(struct roth_present_timing_v1 *out)
{
    uint32_t seq0, seq1, tick_serial;
    uint64_t prev, last;
    do {
        seq0 = __atomic_load_n(&g_pace_seq, __ATOMIC_ACQUIRE);
        prev = g_prev_tick_ns;
        last = g_last_tick_ns;
        tick_serial = g_tick_serial;
        seq1 = __atomic_load_n(&g_pace_seq, __ATOMIC_ACQUIRE);
    } while ((seq0 & 1u) || seq0 != seq1);

    uint64_t now = monotonic_ns();
    float alpha = 1.0f;
    if (tick_serial >= 2u && prev != 0 && last > prev) {
        double a = now > last ? (double)(now - last) / (double)(last - prev) : 0.0;
        if (a < 0.0) a = 0.0;
        if (a > 1.0) a = 1.0;
        alpha = (float)a;
    }

    out->struct_size = (uint32_t)sizeof(*out);
    out->tick_serial = tick_serial;
    out->present_serial = ++g_present_serial;
    out->prev_tick_ns_lo = (uint32_t)prev;
    out->prev_tick_ns_hi = (uint32_t)(prev >> 32);
    out->last_tick_ns_lo = (uint32_t)last;
    out->last_tick_ns_hi = (uint32_t)(last >> 32);
    out->now_ns_lo = (uint32_t)now;
    out->now_ns_hi = (uint32_t)(now >> 32);
    out->alpha = alpha;
}

void roth_present_mark_present_callback(int active)
{
    g_in_present_callback = active != 0;
}

/* ============================================================ window control ================== */
static uint32_t g_req_window_mode;
static uint32_t g_req_present_mode;
static uint32_t g_req_min_interval;
static uint32_t g_req_window_pending;
static uint32_t g_req_present_pending;
static uint32_t g_req_interval_pending;

static uint32_t g_window_state_seq;
static uint32_t g_window_generation;
static uint32_t g_window_mode = ROTH_WINMODE_WINDOWED;
static uint32_t g_present_mode = ROTH_PRESENT_FIFO;
static uint32_t g_min_interval_us;
static int32_t g_last_request_result = ROTH_OK;

#define WMODES_MAX 128
static struct roth_display_mode g_modes[WMODES_MAX];
static int32_t g_mode_count;
static uint32_t g_modes_seq;

int32_t roth_window_set_window_mode(const struct roth_api_v1 *api, uint32_t mode)
{
    (void)api;
    if (mode == ROTH_WINMODE_EXCLUSIVE_FULLSCREEN)
        return ROTH_ERR_UNSUPPORTED;
    if (mode != ROTH_WINMODE_WINDOWED && mode != ROTH_WINMODE_BORDERLESS_FULLSCREEN)
        return ROTH_ERR_INVALID;
    __atomic_store_n(&g_req_window_mode, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&g_req_window_pending, 1u, __ATOMIC_RELEASE);
    return ROTH_OK;
}

int32_t roth_window_set_present_mode(const struct roth_api_v1 *api, uint32_t mode)
{
    (void)api;
    if (mode != ROTH_PRESENT_FIFO && mode != ROTH_PRESENT_IMMEDIATE &&
        mode != ROTH_PRESENT_ADAPTIVE)
        return ROTH_ERR_INVALID;
    __atomic_store_n(&g_req_present_mode, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&g_req_present_pending, 1u, __ATOMIC_RELEASE);
    return ROTH_OK;
}

int32_t roth_window_set_min_present_interval_us(const struct roth_api_v1 *api, uint32_t usec)
{
    (void)api;
    __atomic_store_n(&g_req_min_interval, usec, __ATOMIC_RELAXED);
    __atomic_store_n(&g_req_interval_pending, 1u, __ATOMIC_RELEASE);
    return ROTH_OK;
}

int32_t roth_window_get_window_state(const struct roth_api_v1 *api,
                                     struct roth_window_state_v1 *state)
{
    (void)api;
    if (!state || state->struct_size < (uint32_t)sizeof(*state))
        return ROTH_ERR_INVALID;

    uint32_t seq0, seq1;
    struct roth_window_state_v1 copy;
    do {
        seq0 = __atomic_load_n(&g_window_state_seq, __ATOMIC_ACQUIRE);
        copy.struct_size = (uint32_t)sizeof(copy);
        copy.generation = g_window_generation;
        copy.window_mode = g_window_mode;
        copy.present_mode = g_present_mode;
        copy.min_present_interval_us = g_min_interval_us;
        copy.last_request_result = g_last_request_result;
        seq1 = __atomic_load_n(&g_window_state_seq, __ATOMIC_ACQUIRE);
    } while ((seq0 & 1u) || seq0 != seq1);
    *state = copy;
    return ROTH_OK;
}

int32_t roth_window_get_display_mode_count(const struct roth_api_v1 *api)
{
    (void)api;
    return __atomic_load_n(&g_mode_count, __ATOMIC_ACQUIRE);
}

int32_t roth_window_get_display_mode(const struct roth_api_v1 *api, int32_t idx,
                                     uint32_t *w, uint32_t *h, uint32_t *refresh_mhz)
{
    (void)api;
    struct roth_display_mode mode;
    uint32_t seq0, seq1;
    int32_t count;
    do {
        seq0 = __atomic_load_n(&g_modes_seq, __ATOMIC_ACQUIRE);
        count = __atomic_load_n(&g_mode_count, __ATOMIC_ACQUIRE);
        if (idx < 0 || idx >= count)
            return ROTH_ERR_RANGE;
        mode = g_modes[idx];
        seq1 = __atomic_load_n(&g_modes_seq, __ATOMIC_ACQUIRE);
    } while ((seq0 & 1u) || seq0 != seq1);
    if (w) *w = mode.w;
    if (h) *h = mode.h;
    if (refresh_mhz) *refresh_mhz = mode.refresh_mhz;
    return ROTH_OK;
}

int roth_window_take_window_mode_req(uint32_t *mode)
{
    if (!__atomic_exchange_n(&g_req_window_pending, 0u, __ATOMIC_ACQ_REL))
        return 0;
    *mode = __atomic_load_n(&g_req_window_mode, __ATOMIC_ACQUIRE);
    return 1;
}

int roth_window_take_present_mode_req(uint32_t *mode)
{
    if (!__atomic_exchange_n(&g_req_present_pending, 0u, __ATOMIC_ACQ_REL))
        return 0;
    *mode = __atomic_load_n(&g_req_present_mode, __ATOMIC_ACQUIRE);
    return 1;
}

int roth_window_take_min_interval_req(uint32_t *usec)
{
    if (!__atomic_exchange_n(&g_req_interval_pending, 0u, __ATOMIC_ACQ_REL))
        return 0;
    *usec = __atomic_load_n(&g_req_min_interval, __ATOMIC_ACQUIRE);
    return 1;
}

void roth_window_publish_state(uint32_t window_mode, uint32_t present_mode,
                               uint32_t min_interval_us, int32_t last_request_result,
                               int bump_generation)
{
    uint32_t seq = __atomic_load_n(&g_window_state_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_window_state_seq, seq + 1u, __ATOMIC_RELEASE);
    if (bump_generation)
        g_window_generation++;
    g_window_mode = window_mode;
    g_present_mode = present_mode;
    g_min_interval_us = min_interval_us;
    g_last_request_result = last_request_result;
    __atomic_store_n(&g_window_state_seq, seq + 2u, __ATOMIC_RELEASE);
}

void roth_window_publish_modes(const struct roth_display_mode *modes, int count)
{
    if (count < 0) count = 0;
    if (count > WMODES_MAX) count = WMODES_MAX;
    uint32_t seq = __atomic_load_n(&g_modes_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&g_modes_seq, seq + 1u, __ATOMIC_RELEASE);
    for (int i = 0; i < count; i++)
        g_modes[i] = modes[i];
    __atomic_store_n(&g_mode_count, (int32_t)count, __ATOMIC_RELEASE);
    __atomic_store_n(&g_modes_seq, seq + 2u, __ATOMIC_RELEASE);
}
