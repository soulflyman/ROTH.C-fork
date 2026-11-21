/* Headless validation helper: read the shared framebuffer state and optionally
 * push scancodes, so the host<->shm<->input path can be checked without a GUI.
 *
 *   roth-inject              -> print frame counter + non-black pixel count
 *   roth-inject key <hex>... -> push set-1 scancodes (make), then break codes
 *   roth-inject hold <hex> <ms> -> make code, hold, break code (auto-repeat tests)
 *   roth-inject filter <0|1> -> set g_shm->filter_mode (0=nearest, 1=linear),
 *                               exactly what roth_api_v1->set_display_filter() does
 */
#include "../shared_fb.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    int fd = shm_open(ROTH_SHM_NAME, O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    struct roth_shm *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    if (s == MAP_FAILED || s->magic != ROTH_SHM_MAGIC) {
        fprintf(stderr, "shm not ready\n");
        return 1;
    }

    if (argc >= 3 && !strcmp(argv[1], "key")) {
        for (int i = 2; i < argc; i++) {
            uint8_t mk = (uint8_t)strtol(argv[i], NULL, 16);
            s->key_ring[s->key_head & ROTH_KEY_MASK] = mk;
            s->key_head++;
        }
        usleep(60000); /* let it register as held */
        for (int i = 2; i < argc; i++) {
            uint8_t mk = (uint8_t)strtol(argv[i], NULL, 16);
            s->key_ring[s->key_head & ROTH_KEY_MASK] = mk | 0x80;
            s->key_head++;
        }
        printf("sent %d key(s)\n", argc - 2);
        return 0;
    }

    /* roth-inject hold <hex> <ms>: make code, hold for <ms>, then the break code (typematic/auto-repeat tests) */
    if (argc == 4 && !strcmp(argv[1], "hold")) {
        uint8_t mk = (uint8_t)strtol(argv[2], NULL, 16);
        s->key_ring[s->key_head & ROTH_KEY_MASK] = mk;
        s->key_head++;
        usleep((useconds_t)strtol(argv[3], NULL, 10) * 1000);
        s->key_ring[s->key_head & ROTH_KEY_MASK] = mk | 0x80;
        s->key_head++;
        printf("held %s for %s ms\n", argv[2], argv[3]);
        return 0;
    }

    /* mouse relative motion: roth-inject move <dx> <dy> */
    if (argc == 4 && !strcmp(argv[1], "move")) {
        s->mouse_dx += (int32_t)strtol(argv[2], NULL, 10);
        s->mouse_dy += (int32_t)strtol(argv[3], NULL, 10);
        printf("moved dx=%s dy=%s\n", argv[2], argv[3]);
        return 0;
    }
    /* mouse buttons: roth-inject btn <mask>  (bit0 L, bit1 R, bit2 M); 0 = release */
    if (argc == 3 && !strcmp(argv[1], "btn")) {
        s->mouse_buttons = (uint32_t)strtol(argv[2], NULL, 0);
        printf("btn=%s\n", argv[2]);
        return 0;
    }
    /* display filter: roth-inject filter <0|1> */
    if (argc == 3 && !strcmp(argv[1], "filter")) {
        s->filter_mode = (uint32_t)strtol(argv[2], NULL, 0);
        printf("filter_mode=%u\n", s->filter_mode);
        return 0;
    }

    int nz = 0;
    for (int i = 0; i < ROTH_FB_W * ROTH_FB_H; i++)
        if (s->pixels[i])
            nz++;
    printf("frame=%u host_alive=%u viewer_alive=%u nonblack=%d/%d "
           "probe[mid=%u special=%u worldblend=%u] filter_mode=%u cur_w=%u cur_h=%u\n",
           s->frame, s->host_alive, s->viewer_alive, nz, ROTH_FB_W * ROTH_FB_H,
           s->probe[0], s->probe[1], s->probe[2], s->filter_mode, s->cur_w, s->cur_h);
    return 0;
}
