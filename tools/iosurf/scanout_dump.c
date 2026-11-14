// Dumps the panel's scan-out (VRAM offset 0, 2560x1600 XRGB, pitch 10240) through PhxBringup's DBG_VRAM_READ
// (selector 102, 256 dwords a call) to a raw file: what the panel shows, for checking rendering without a camera.
// Usage (root): scanout_dump out.raw [width height pitch]
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>

static unsigned dimension(const char *text)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    return text[0] >= '0' && text[0] <= '9' && !*end && !errno && value <= UINT_MAX ? (unsigned)value : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 4 && argc != 5) { fprintf(stderr, "usage: scanout_dump out.raw [w h pitch]\n"); return 2; }
    const unsigned W = argc >= 4 ? dimension(argv[2]) : 2560;
    const unsigned H = argc >= 4 ? dimension(argv[3]) : 1600;
    const unsigned P = argc == 5 ? dimension(argv[4]) : 10240;
    if (!W || !H || !P || W > UINT_MAX / 4 || W * 4 > P || (P & 3)) {
        fprintf(stderr, "invalid width, height or pitch\n"); return 2;
    }
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    io_connect_t c = 0;
    if (!s) { fprintf(stderr, "service missing\n"); return 1; }
    kern_return_t opened = IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c);
    IOObjectRelease(s);
    if (opened) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); IOServiceClose(c); return 1; }
    FILE *f = fopen(argv[1], "wb");
    if (!f) { perror("output"); IOServiceClose(c); return 1; }
    int result = 1;
    uint32_t buf[256];
    for (unsigned y = 0; y < H; y++) {
        for (unsigned x = 0; x < W; x += 256) {
            const unsigned n = W - x < 256 ? W - x : 256;
            uint64_t in[2] = {(uint64_t)y * P + x * 4ull, n};
            size_t outsz = n * 4;
            kern_return_t kr = IOConnectCallMethod(c, 102, in, 2, NULL, 0, NULL, NULL, buf, &outsz);
            if (kr || outsz != n * 4u) {
                fprintf(stderr, "read y %u x %u: 0x%x, bytes %zu/%u\n", y, x, kr, outsz, n * 4u);
                goto done;
            }
            if (fwrite(buf, 4, n, f) != n) { perror("write"); goto done; }
        }
    }
    result = 0;
done:
    if (fclose(f)) { perror("close output"); result = 1; }
    IOServiceClose(c);
    return result;
}
