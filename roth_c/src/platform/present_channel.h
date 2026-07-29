/* present_channel.h — host-side plumbing for the SDK world-frame channel, present pacing, and
 * window control. The generated host table binds its public methods to present_channel.c; the SDL
 * present thread consumes frames, builds timing records, and applies window requests through this
 * private interface.
 *
 * The world path is a lock-free single-producer/single-consumer triple buffer. Timing, window state,
 * and display modes use short seqlocks. Request flags and ownership are 32-bit atomics, so neither
 * the game thread nor the present thread blocks on the other.
 */
#ifndef ROTH_PRESENT_CHANNEL_H
#define ROTH_PRESENT_CHANNEL_H

#include <stdint.h>

struct roth_api_v1;
struct roth_world_frame_v1;
struct roth_present_caps_v1;
struct roth_present_timing_v1;
struct roth_window_state_v1;

#define ROTH_WORLD_MAX_W 3840u
#define ROTH_WORLD_MAX_H 2160u

/* Public sub-table implementations bound by sdk/generated/host_adapters.c. */
int32_t roth_present_submit_world_frame(const struct roth_api_v1 *api,
                                        const struct roth_world_frame_v1 *frame);
int32_t roth_present_withdraw_world_frame(const struct roth_api_v1 *api);
int32_t roth_present_get_present_caps(const struct roth_api_v1 *api,
                                      struct roth_present_caps_v1 *caps);
int32_t roth_window_set_window_mode(const struct roth_api_v1 *api, uint32_t mode);
int32_t roth_window_set_present_mode(const struct roth_api_v1 *api, uint32_t mode);
int32_t roth_window_set_min_present_interval_us(const struct roth_api_v1 *api, uint32_t usec);
int32_t roth_window_get_window_state(const struct roth_api_v1 *api,
                                     struct roth_window_state_v1 *state);
int32_t roth_window_get_display_mode_count(const struct roth_api_v1 *api);
int32_t roth_window_get_display_mode(const struct roth_api_v1 *api, int32_t idx,
                                     uint32_t *w, uint32_t *h, uint32_t *refresh_mhz);

/* Present-thread view of the latest copied world frame. All returned pointers remain valid until
 * the next acquire on the same thread. The viewport has already been clipped to the current engine
 * dimensions. */
struct roth_present_frame {
    const uint32_t *rgba;
    uint32_t serial;            /* identifies the world image in rgba; never zero for a live frame */
    uint32_t w, h;
    int32_t vx, vy;
    uint32_t vw, vh;
    const uint8_t *engine_base;
    uint32_t engine_base_pitch;
    uint32_t composite_mode;
    uint32_t transparent_index;
    const uint8_t *paired_engine_pixels;
    const uint8_t *paired_engine_palette;
    uint32_t paired_engine_w, paired_engine_h;
    uint32_t paired_engine_serial;
};

/* The SDL host publishes the current engine pixel dimensions used to validate and clip subsequent
 * submissions. acquire() returns zero when no frame is active or the crash-safety stale limit hit. */
void roth_present_set_engine_dimensions(uint32_t w, uint32_t h);
int roth_present_acquire(struct roth_present_frame *out);

/* A submitted world frame is staged until the engine page it belongs to is both
 * finished and published. The completed page names itself here and reserves the
 * room its copy needs; the copy is then taken at the publish site, from the very
 * bytes that reach the shared frame, and only while that page is still the one
 * being published. The world reaches the consumer with that copy, so the engine
 * layer a composite tests against is the published image of the same frame,
 * overlays and all. The shared-frame publisher separately names the engine frame
 * its live copy currently holds, so a consumer can tell whether that copy is the
 * same frame the world was paired with before compositing the two together. */
void roth_present_note_engine_page(uint32_t serial, uint32_t w, uint32_t h);
void roth_present_pair_engine_frame(const uint8_t *pixels, uint32_t w, uint32_t h,
                                    const uint8_t *palette, uint32_t serial);
void roth_present_note_shared_engine_serial(uint32_t serial);
uint32_t roth_present_shared_engine_serial(void);

/* The present thread publishes whether it is currently consuming the centered
 * canvas mode. The frame producer uses this to keep the engine layer
 * cursor-free while the SDL host places the cursor over the full canvas. */
void roth_present_set_separate_cursor(int active);
int roth_present_separate_cursor(void);

/* Release the frame-channel lease held by this per-plugin API-table address. */
void roth_present_release_owner(const struct roth_api_v1 *api);

/* Game thread records observed ticks; present thread builds one complete callback record. */
void roth_present_note_tick(void);
void roth_present_make_timing(struct roth_present_timing_v1 *out);
void roth_present_mark_present_callback(int active);

struct roth_display_mode { uint32_t w, h, refresh_mhz; };

/* Present thread takes pending GAME-context requests. */
int roth_window_take_window_mode_req(uint32_t *mode);
int roth_window_take_present_mode_req(uint32_t *mode);
int roth_window_take_min_interval_req(uint32_t *usec);

/* Present thread initializes and then publishes applied state. bump_generation is non-zero for an
 * applied request, including a request the backend rejected (last_request_result records why). */
void roth_window_publish_state(uint32_t window_mode, uint32_t present_mode,
                               uint32_t min_interval_us, int32_t last_request_result,
                               int bump_generation);
void roth_window_publish_modes(const struct roth_display_mode *modes, int count);

#endif /* ROTH_PRESENT_CHANNEL_H */
