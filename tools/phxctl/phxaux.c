// phxaux: native DisplayPort AUX transactions on the display block's AUX engine (DCN 3.1.4 DP_AUX<n>), the way Linux's
// dce_aux does them: take the engine for software, write header and payload into AUX_SW_DATA, AUX_SW_GO, poll
// AUX_SW_DONE, read the reply. Each byte's index is given explicitly. For the eDP panel's DPCD (backlight caps 0x701..,
// mode 0x721, brightness 0x722). Through PhxBringup's READREGS (2) and DBG_WRITE_REG (108, root with phxdebug).
// Usage (root): phxaux [-a <aux 0..4>] scan                  AUX_CONTROL of every engine
//               phxaux [-a <aux>] read <addr> [len]          DPCD bytes (len 1..16, default 16)
//               phxaux [-a <aux>] write <addr> <byte>...     DPCD bytes
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { DMU_BASE2 = 0x34c0, AUX0 = DMU_BASE2 + 0x1f50, AUX_STRIDE = 0x1c,
       CONTROL = 0, SW_CONTROL = 1, ARB_CONTROL = 2, INTERRUPT_CONTROL = 3, SW_STATUS = 4, SW_DATA = 6 };
static io_connect_t c;
static uint32_t aux = AUX0;

static uint32_t rd(uint32_t reg)
{
    uint64_t in[3] = {reg, 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
    if (IOConnectCallMethod(c, 2, in, 3, NULL, 0, NULL, NULL, &v, &sz)) { fprintf(stderr, "read 0x%x failed\n", reg); exit(1); }
    return v;
}
// returns the register read back after the write
static uint32_t wr(uint32_t reg, uint32_t v)
{
    uint64_t in[2] = {reg, v}, out[1] = {0}; uint32_t n = 1;
    if (IOConnectCallMethod(c, 108, in, 2, NULL, 0, out, &n, NULL, NULL)) { fprintf(stderr, "write 0x%x failed (root and phxdebug=1?)\n", reg); exit(1); }
    return (uint32_t)out[0];
}
static uint32_t R(int r) { return rd(aux + r); }
static uint32_t W(int r, uint32_t v) { return wr(aux + r, v); }

// AUX_SW_DATA: DATA_RW bit 0, DATA 15:8, INDEX 20:16, AUTOINCREMENT_DISABLE bit 31. Every read of it in read mode
// takes the next reply byte, and the debug write reads the register back: selecting index i with a write consumes
// byte i (returned here), and the next plain read gives byte i + 1.
static void put(unsigned i, uint8_t b) { W(SW_DATA, 0x80000000u | (i << 16) | ((uint32_t)b << 8)); }
static uint8_t pick(unsigned i) { return (uint8_t)(W(SW_DATA, 0x80000000u | (i << 16) | 1u) >> 8); }
static uint8_t next(void) { return (uint8_t)(R(SW_DATA) >> 8); }

// one native request (cmd 9 read, 8 write); returns the reply's data bytes (-1: no reply, -2: NACK/DEFER)
static int xfer(int cmd, uint32_t addr, uint8_t *data, unsigned len)
{
    if (((R(ARB_CONTROL) >> 2) & 3) == 2) { fprintf(stderr, "AUX taken by the display microcontroller\n"); return -1; }
    if (!(R(CONTROL) & 1)) { fprintf(stderr, "AUX engine disabled (AUX_CONTROL 0x%08x)\n", R(CONTROL)); return -1; }
    W(ARB_CONTROL, R(ARB_CONTROL) | (1u << 16));                       // AUX_SW_USE_AUX_REG_REQ
    if (((R(ARB_CONTROL) >> 2) & 3) != 1) { fprintf(stderr, "AUX not granted (ARB 0x%08x)\n", R(ARB_CONTROL)); return -1; }
    W(INTERRUPT_CONTROL, R(INTERRUPT_CONTROL) | 2u);                   // AUX_SW_DONE_ACK
    for (int i = 0; i < 100 && (R(SW_STATUS) & 1); i++) usleep(100);
    uint8_t req[4 + 16]; unsigned n = 0;
    req[n++] = (uint8_t)((cmd << 4) | ((addr >> 16) & 0xf)); req[n++] = (uint8_t)(addr >> 8); req[n++] = (uint8_t)addr;
    req[n++] = (uint8_t)(len - 1);
    if (cmd == 8) { memcpy(req + n, data, len); n += len; }
    W(SW_CONTROL, (R(SW_CONTROL) & ~(0x1fu << 16 | 0xfu << 4)) | (n << 16));   // WR_BYTES, no start delay
    for (unsigned i = 0; i < n; i++) put(i, req[i]);
    W(SW_CONTROL, R(SW_CONTROL) | 1u);                                 // AUX_SW_GO
    uint32_t st = 0;
    for (int i = 0; i < 500 && !((st = R(SW_STATUS)) & 1); i++) usleep(100);
    int ret = -1;
    if (!(st & 1) || (st & 0x80) || (st & 0x7f7f00u)) fprintf(stderr, "AUX failed: SW_STATUS 0x%08x\n", st);
    else {
        const unsigned got = (st >> 24) & 0x1f, reply = pick(0) >> 4;
        if (reply != 0) { fprintf(stderr, "AUX reply 0x%x (%s), %u bytes\n", reply, reply & 2 ? "DEFER" : "NACK", got); ret = -2; }
        else { ret = got ? (int)got - 1 : 0; if (cmd == 9) for (int i = 0; i < ret && i < (int)len; i++) data[i] = next(); }
    }
    W(INTERRUPT_CONTROL, R(INTERRUPT_CONTROL) | 2u);
    W(ARB_CONTROL, R(ARB_CONTROL) | (1u << 17));                       // AUX_SW_DONE_USING_AUX_REG
    return ret;
}

int main(int argc, char **argv)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    if (argc > 2 && !strcmp(argv[1], "-a")) { aux = AUX0 + AUX_STRIDE * (uint32_t)atoi(argv[2]); argc -= 2; argv += 2; }
    if (argc < 2) { fprintf(stderr, "usage: phxaux [-a n] scan | read <addr> [len] | write <addr> <byte>...\n"); return 2; }
    if (!strcmp(argv[1], "scan")) {
        for (int n = 0; n < 5; n++) printf("AUX%d: CONTROL 0x%08x ARB 0x%08x STATUS 0x%08x\n", n, rd(AUX0 + AUX_STRIDE * n),
                                           rd(AUX0 + AUX_STRIDE * n + ARB_CONTROL), rd(AUX0 + AUX_STRIDE * n + SW_STATUS));
        return 0;
    }
    if (argc < 3) return 2;
    const uint32_t addr = (uint32_t)strtoul(argv[2], NULL, 0);
    uint8_t buf[16];
    if (!strcmp(argv[1], "read")) {
        unsigned len = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 16;
        if (len < 1 || len > 16) return 2;
        int got = xfer(9, addr, buf, len);
        if (got < 0) return 1;
        printf("0x%05x:", addr); for (int i = 0; i < got; i++) printf(" %02x", buf[i]); printf("\n");
        return 0;
    }
    if (!strcmp(argv[1], "write") && argc > 3 && argc - 3 <= 16) {
        unsigned len = (unsigned)(argc - 3);
        for (unsigned i = 0; i < len; i++) buf[i] = (uint8_t)strtoul(argv[3 + i], NULL, 0);
        return xfer(8, addr, buf, len) < 0;
    }
    return 2;
}
