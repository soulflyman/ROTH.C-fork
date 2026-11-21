/* Headless validation helper: dump the shared framebuffer (indexed pixels + VGA DAC
 * palette) to a .ppm so rendering changes (resolution, stride, projection) can be
 * inspected visually without a window — pairs with roth-inject for driving input.
 *
 *   dumpfb [out.ppm]   -> writes the current cur_w x cur_h frame as a P6 PPM
 */
#include "../shared_fb.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "fb.ppm";
    int fd = shm_open(ROTH_SHM_NAME, O_RDONLY, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    struct roth_shm *s = mmap(NULL, sizeof *s, PROT_READ, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED || s->magic != ROTH_SHM_MAGIC) { fprintf(stderr, "shm not ready\n"); return 1; }
    uint32_t w = s->cur_w ? s->cur_w : ROTH_FB_W, h = s->cur_h ? s->cur_h : ROTH_FB_H;
    FILE *f = fopen(out, "wb");
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t idx = s->pixels[i];
        uint8_t r = s->palette[idx*3+0] * 255 / 63;
        uint8_t g = s->palette[idx*3+1] * 255 / 63;
        uint8_t b = s->palette[idx*3+2] * 255 / 63;
        fputc(r, f); fputc(g, f); fputc(b, f);
    }
    fclose(f);
    printf("wrote %s (%ux%u)\n", out, w, h);
    return 0;
}
