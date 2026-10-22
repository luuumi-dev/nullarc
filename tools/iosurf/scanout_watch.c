// Watches a rectangle of the panel's scan-out through PhxBringup's DBG_VRAM_READ (selector 102, as scanout_dump) and
// prints, every period, how many of its pixels stand out from the rectangle's median brightness by more than 60 of 255:
// for text on a flat or glass background, the text's pixels. "<seconds> <count>" per line. Typed text must only grow.
// Usage (root): scanout_watch x y w h period_ms seconds [dump.raw]   (panel 2560x1600 XRGB, pitch 10240, top-left origin)
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static double now_s(void) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * (double)tb.numer / tb.denom / 1e9; }

int main(int argc, char **argv)
{
    if (argc < 7) { fprintf(stderr, "usage: scanout_watch x y w h period_ms seconds [dump.raw]\n"); return 2; }
    const unsigned X = atoi(argv[1]), Y = atoi(argv[2]), W = atoi(argv[3]), H = atoi(argv[4]), P = 10240;
    const double period = atof(argv[5]) / 1e3, secs = atof(argv[6]);
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    uint32_t *px = malloc((size_t)W * H * 4);
    FILE *dump = argc > 7 ? fopen(argv[7], "wb") : NULL;   // every sample's rectangle, back to back
    const double t0 = now_s();
    for (double next = t0; now_s() - t0 < secs; next += period) {
        for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < W; x += 256) {
                const unsigned n = W - x < 256 ? W - x : 256;
                uint64_t in[2] = {(uint64_t)(Y + y) * P + (X + x) * 4ull, n};
                size_t outsz = n * 4;
                if (IOConnectCallMethod(c, 102, in, 2, NULL, 0, NULL, NULL, px + (size_t)y * W + x, &outsz)) { fprintf(stderr, "read failed\n"); return 1; }
            }
        unsigned hist[256] = {0}, count = 0;
        for (size_t i = 0; i < (size_t)W * H; i++) { const uint32_t v = px[i]; hist[((v >> 16 & 255) * 2 + (v >> 8 & 255) * 5 + (v & 255)) / 8]++; }
        unsigned med = 0; for (size_t acc = 0; med < 255 && (acc += hist[med]) < (size_t)W * H / 2; med++) {}
        for (unsigned l = 0; l < 256; l++) if (l + 60 < med || l > med + 60) count += hist[l];
        printf("%.3f %u\n", now_s() - t0, count); fflush(stdout);
        if (dump) fwrite(px, 4, (size_t)W * H, dump);
        const double wait = next + period - now_s(); if (wait > 0) usleep((useconds_t)(wait * 1e6));
    }
    if (dump) fclose(dump);
    return 0;
}
