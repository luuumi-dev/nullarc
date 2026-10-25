// xlatefc: translates one AIR text with the function constants a link dump recorded (NVMTL_LINK_DUMP's <fn>.fc.txt:
// "stage <s>" then "<index> <size> <bytes...>" lines), as the plugin does for a linked kernel, and times it (B-29).
// Usage: xlatefc <file.ll> <file.fc.txt>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef int (*fc_fn)(const char *, const char *, const uint32_t *, const uint32_t *, const uint8_t *const *, size_t, uint8_t **, size_t *, char *, size_t);

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: xlatefc file.ll file.fc.txt\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *ll = malloc(sz + 1); fread(ll, 1, sz, f); ll[sz] = 0; fclose(f);
    FILE *c = fopen(argv[2], "r"); if (!c) { perror(argv[2]); return 1; }
    char stage[32] = "kernel", line[512]; uint32_t idx[64], sizes[64]; uint8_t vals[64][16]; const uint8_t *pay[64]; size_t n = 0;
    while (fgets(line, sizeof line, c)) {
        if (!strncmp(line, "stage ", 6)) { sscanf(line + 6, "%31s", stage); continue; }
        char *p = line; unsigned i, s; int k;
        if (n < 64 && sscanf(p, "%u %u%n", &i, &s, &k) == 2 && s <= 16) {
            p += k; idx[n] = i; sizes[n] = s;
            for (unsigned b = 0; b < s; b++) { unsigned v; int kk; if (sscanf(p, "%x%n", &v, &kk) == 1) { vals[n][b] = (uint8_t)v; p += kk; } }
            pay[n] = vals[n]; n++;
        }
    }
    fclose(c);
    void *h = dlopen("/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/libnvmtl_translate.dylib", RTLD_NOW);
    if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    fc_fn fc = (fc_fn)dlsym(h, "nvmtl_translate_fc");
    void (*setcaps)(uint32_t) = (void (*)(uint32_t))dlsym(h, "nvmtl_translate_set_caps"); if (setcaps) setcaps(3u);
    char err[1024]; uint8_t *out = NULL; size_t len = 0;
    const double t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6;
    int r = fc(ll, stage, idx, sizes, pay, n, &out, &len, err, sizeof err);
    printf("%s: %zu constants, rc %d, %.0f ms, %zu B SPIR-V%s%s\n", stage, n, r, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1e6 - t0,
           len, r ? " - " : "", r ? err : "");
    return r != 0;
}
