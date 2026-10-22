// phxbl: reads or sets the eDP backlight through the display block's PWM (DCN 3.1.4 PWRSEQ0, as Linux's panel_cntl does
// without DMUB): lock the group, write the active count, unlock, wait for the pending update. The count is a fraction of
// the period: (level in 16.16 x BL_PWM_PERIOD) >> BL_PWM_PERIOD_BITCNT, 0xfa00 for 100 % at the firmware's period 4000
// and 12 bits. Through PhxBringup's READREGS (2) and DBG_WRITE_REG (108, root with phxdebug).
// Usage (root): phxbl            prints the level
//               phxbl <percent>  sets it (1..100)
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { DMU_BASE2 = 0x34c0, BL_PWM_CNTL = DMU_BASE2 + 0x2f19, BL_PWM_PERIOD_CNTL = DMU_BASE2 + 0x2f1b, BL_PWM_GRP1_REG_LOCK = DMU_BASE2 + 0x2f1c };
static io_connect_t c;

static uint32_t rd(uint32_t reg)
{
    uint64_t in[3] = {reg, 1, 0xFFFFFFFFull}; uint32_t v = 0; size_t sz = 4;
    if (IOConnectCallMethod(c, 2, in, 3, NULL, 0, NULL, NULL, &v, &sz)) { fprintf(stderr, "read 0x%x failed\n", reg); exit(1); }
    return v;
}
static void wr(uint32_t reg, uint32_t v)
{
    uint64_t in[2] = {reg, v}, out[1]; uint32_t n = 1;
    if (IOConnectCallMethod(c, 108, in, 2, NULL, 0, out, &n, NULL, NULL)) { fprintf(stderr, "write 0x%x failed (root and phxdebug=1?)\n", reg); exit(1); }
}

int main(int argc, char **argv)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!s || IOServiceOpen(s, mach_task_self(), 0x4E34384Eu, &c)) { fprintf(stderr, "open failed\n"); return 1; }
    uint64_t hin[2] = {1, 1u << 8}, hout[4]; uint32_t hn = 4;
    if (IOConnectCallMethod(c, 0, hin, 2, NULL, 0, hout, &hn, NULL, NULL)) { fprintf(stderr, "hello failed\n"); return 1; }
    const uint32_t pc = rd(BL_PWM_PERIOD_CNTL), period = pc & 0xffff, bits = (pc >> 16) & 0xf, cntl = rd(BL_PWM_CNTL);
    const uint32_t full = bits ? (uint32_t)(((uint64_t)0x10000 * period) >> bits) : 0;
    if (!period || !bits || !full) { fprintf(stderr, "PWM not set up: PERIOD_CNTL 0x%08x CNTL 0x%08x\n", pc, cntl); return 1; }
    printf("backlight: %u / %u (%.1f %%), PWM %s, period %u, %u bits\n", cntl & 0xffff, full, 100.0 * (cntl & 0xffff) / full,
           (cntl >> 31) ? "on" : "OFF", period, bits);
    if (argc < 2) return 0;
    int pct = atoi(argv[1]); if (pct < 1) pct = 1; if (pct > 100) pct = 100;
    const uint32_t level = (uint32_t)(0x10000ull * pct / 100);
    const uint64_t duty = (uint64_t)level * period;
    uint32_t cnt = (uint32_t)((duty >> bits) & 0xffff) + (uint32_t)((duty >> (bits - 1)) & 1);
    if (cnt > 0xffff) cnt = 0xffff;
    const uint32_t lock = rd(BL_PWM_GRP1_REG_LOCK);
    wr(BL_PWM_GRP1_REG_LOCK, lock | 0x80000000u | 1u);          // IGNORE_MASTER_LOCK_EN, REG_LOCK
    wr(BL_PWM_CNTL, (rd(BL_PWM_CNTL) & ~0xffffu) | cnt);         // BL_ACTIVE_INT_FRAC_CNT
    wr(BL_PWM_GRP1_REG_LOCK, (rd(BL_PWM_GRP1_REG_LOCK) | 0x80000000u) & ~1u);
    for (int i = 0; i < 100 && (rd(BL_PWM_GRP1_REG_LOCK) & 0x100); i++) usleep(1000);   // REG_UPDATE_PENDING
    const uint32_t now = rd(BL_PWM_CNTL);
    printf("set %d %%: count %u -> CNTL 0x%08x, LOCK 0x%08x\n", pct, cnt, now, rd(BL_PWM_GRP1_REG_LOCK));
    return (now & 0xffff) == cnt ? 0 : 1;
}
