// hdadump: walks the HDA codec through AppleALC's ALCUserClientProvider (boot-arg alcverbs=1) and prints what a layout
// needs: every widget's type and caps, and for pins the config default, pin caps, pin control, EAPD and connections;
// the function group's GPIOs. Verbs as AppleALC's alc-verb sends them: 12-bit verb, 8-bit parameter.
// Usage: hdadump
#include <IOKit/IOKitLib.h>
#include <stdio.h>

static io_connect_t c;
static long verb(unsigned nid, unsigned v, unsigned param)
{
    uint64_t in[3] = {nid, v, param}, out = 0; uint32_t n = 1;
    return IOConnectCallScalarMethod(c, 0, in, 3, &out, &n) ? -1 : (long)(uint32_t)out;
}
static long par(unsigned nid, unsigned p) { return verb(nid, 0xF00, p); }

int main(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("ALCUserClientProvider"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0, &c)) { fprintf(stderr, "no ALCUserClientProvider (boot-arg alcverbs=1?)\n"); return 1; }
    printf("codec: vendor 0x%08lx revision 0x%08lx\n", par(0, 0x00), par(0, 0x02));
    const long sub = par(0, 0x04);
    if (sub < 0) { fprintf(stderr, "verbs refused\n"); return 1; }
    for (unsigned fg = (sub >> 16) & 0xff, fe = fg + (sub & 0xff); fg < fe; fg++) {
        printf("function group 0x%02x: type 0x%lx, GPIO caps 0x%08lx, GPIO data 0x%02lx mask 0x%02lx dir 0x%02lx\n", fg,
               par(fg, 0x05) & 0xff, par(fg, 0x11), verb(fg, 0xF15, 0), verb(fg, 0xF16, 0), verb(fg, 0xF17, 0));
        const long ws = par(fg, 0x04);
        static const char *kType[16] = { "out", "in", "mixer", "selector", "pin", "power", "knob", "beep" };
        for (unsigned n = (ws >> 16) & 0xff, ne = n + (ws & 0xff); n < ne; n++) {
            const long caps = par(n, 0x09); const unsigned type = (caps >> 20) & 0xf;
            printf("0x%02x %-8s caps 0x%08lx", n, kType[type] ? kType[type] : "?", caps);
            if (type == 4) printf(" config 0x%08lx pincaps 0x%08lx ctl 0x%02lx eapd 0x%02lx", verb(n, 0xF1C, 0), par(n, 0x0C),
                                  verb(n, 0xF07, 0), verb(n, 0xF0C, 0));
            if (caps & (1 << 8)) {   // connection list
                const long len = par(n, 0x0E); printf(" conn[%ld]", len & 0x7f);
                for (unsigned i = 0; i < (unsigned)(len & 0x7f); i += 4) {
                    const long l = verb(n, 0xF02, i);
                    for (unsigned k = 0; k < 4 && i + k < (unsigned)(len & 0x7f); k++) printf(" 0x%02lx", (l >> (8 * k)) & 0xff);
                }
                printf(" sel %ld", verb(n, 0xF01, 0));
            }
            printf("\n");
        }
    }
    return 0;
}
