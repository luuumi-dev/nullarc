/*
 * phxctl.c — talk to PhxBringup.kext (service PhxBringup, user client 'N48N') from the command line.
 *
 *   phxctl regs <dword> [<dword> ...]     ReadRegs, absolute dword offsets (root)
 *   phxctl gc                             the GC 11.0.1 status registers of step E2 (root)
 *   phxctl smu <msg> [<arg>]              one SMU 13.0.4 message from the kext's allow-list (root, phxdebug=1)
 *   phxctl smuinfo                        TestMessage, PMFW / driver-if versions, enabled features
 *   phxctl psp status|create <off>|destroy  PSP 13.0.4 KM ring in the UMA carve-out at byte offset <off> (root, phxdebug=1)
 *
 * Absolute offsets = IP discovery segment base + Linux register offset (hardware/dumps/firebat_u6c/ip_discovery.txt):
 * GC segment 0 = 0x1260, segment 1 = 0xA000.
 */
#include <IOKit/IOKitLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UC_TYPE 0x4E34384Eu
#define SEL_HELLO 0
#define SEL_READREGS 2
#define SEL_DBG_SMU 100
#define SEL_DBG_PSP_RING 101
#define SEL_DBG_VRAM_READ 102
#define SEL_DBG_PSP_SUBMIT 103
#define SEL_DBG_FW_STAGE 104
#define SEL_DBG_PSP_LOADIPFW 105
#define SEL_DBG_PSP_CTL 106
#define SEL_DBG_PSP_RAW 107
#define SEL_DBG_WRITE_REG 108
#define SEL_DBG_VRAM_WRITE 109

static io_connect_t conn;

static int open_kext(void)
{
   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
   if (!svc) {
      fprintf(stderr, "PhxBringup service not found\n");
      return -1;
   }
   kern_return_t kr = IOServiceOpen(svc, mach_task_self(), UC_TYPE, &conn);
   IOObjectRelease(svc);
   if (kr) {
      fprintf(stderr, "IOServiceOpen: 0x%08x\n", kr);
      return -1;
   }
   uint64_t in[2] = {1, 0}, out[4];
   uint32_t n = 4;
   kr = IOConnectCallScalarMethod(conn, SEL_HELLO, in, 2, out, &n);
   if (kr) {
      fprintf(stderr, "Hello: 0x%08x\n", kr);
      return -1;
   }
   return 0;
}

static int read_reg(uint32_t reg, uint32_t *val)
{
   uint64_t in[3] = {reg, 1, 0xFFFFFFFFull};
   size_t sz = 4;
   kern_return_t kr = IOConnectCallMethod(conn, SEL_READREGS, in, 3, NULL, 0, NULL, NULL, val, &sz);
   return kr ? (int)kr : 0;
}

static int smu(uint32_t msg, uint32_t arg, uint32_t *resp, uint32_t *after)
{
   uint64_t in[2] = {msg, arg}, out[2] = {0, 0};
   uint32_t n = 2;
   kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_SMU, in, 2, out, &n);
   *resp = (uint32_t)out[0];
   *after = (uint32_t)out[1];
   return (int)kr;
}

static const char *smu_resp_name(uint32_t r)
{
   switch (r) {
   case 0x01: return "OK";
   case 0xFF: return "Failed";
   case 0xFE: return "UnknownCmd";
   case 0xFD: return "CmdRejectedPrereq";
   case 0xFC: return "Busy";
   case 0x00: return "no response";
   default: return "?";
   }
}

static int do_smu(uint32_t msg, uint32_t arg, const char *what)
{
   uint32_t resp, after;
   int kr = smu(msg, arg, &resp, &after);
   printf("smu %-22s msg 0x%02x arg 0x%08x -> kr 0x%08x resp 0x%02x (%s) out 0x%08x\n", what, msg, arg, kr, resp,
          smu_resp_name(resp), after);
   return kr != 0 || resp != 0x01;
}

struct named_reg {
   const char *name;
   uint32_t reg;
};

static const struct named_reg gc_regs[] = {
   {"GRBM_STATUS", 0x1260 + 0x0da4},
   {"GRBM_STATUS2", 0x1260 + 0x0da2},
   {"GRBM_STATUS_SE0", 0x1260 + 0x0da5},
   {"CP_STAT", 0x1260 + 0x0f40},
   {"GB_ADDR_CONFIG", 0x1260 + 0x13de},
   {"CC_GC_SHADER_ARRAY_CONFIG", 0x1260 + 0x100f},
   {"GC_USER_SHADER_ARRAY_CONFIG", 0xA000 + 0x5b90},
   {"RLC_CNTL", 0xA000 + 0x4c00},
   {"RLC_STAT", 0xA000 + 0x4c04},
   {"RLC_GPM_STAT", 0xA000 + 0x4e6b},
   {"RLC_RLCS_BOOTLOAD_STATUS", 0xA000 + 0x4e82},
   {"CP_ME_CNTL", 0xA000 + 0x0803},
   {"CP_MEC_RS64_CNTL", 0xA000 + 0x2904},
   {"CP_MES_CNTL", 0xA000 + 0x2807},
   {"CP_RB0_CNTL", 0x1260 + 0x1de1},
   {"SDMA0_STATUS_REG", 0x1260 + 0x0025},
   {"SDMA0_F32_CNTL", 0xA000 + 0x589a},
};

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: phxctl regs <dword>... | gc | smu <msg> [arg] | smuinfo | "
                      "psp status|create <off>|destroy | vram <off> [count]\n");
      return 2;
   }
   if (open_kext())
      return 1;

   int failed = 0;
   if (!strcmp(argv[1], "regs")) {
      for (int i = 2; i < argc; i++) {
         uint32_t reg = (uint32_t)strtoul(argv[i], NULL, 0), v = 0;
         int kr = read_reg(reg, &v);
         failed |= kr != 0;
         printf("0x%05x: %s0x%08x\n", reg, kr ? "(error) " : "", v);
      }
   } else if (!strcmp(argv[1], "gc")) {
      for (size_t i = 0; i < sizeof(gc_regs) / sizeof(gc_regs[0]); i++) {
         uint32_t v = 0;
         int kr = read_reg(gc_regs[i].reg, &v);
         failed |= kr != 0;
         printf("%-28s 0x%05x: ", gc_regs[i].name, gc_regs[i].reg);
         if (kr)
            printf("error 0x%08x\n", kr);
         else
            printf("0x%08x\n", v);
      }
   } else if (!strcmp(argv[1], "smu") && argc >= 3) {
      failed |= do_smu((uint32_t)strtoul(argv[2], NULL, 0), argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0, "");
   } else if (!strcmp(argv[1], "psp") && argc >= 3) {
      uint64_t op = !strcmp(argv[2], "create") ? 1 : !strcmp(argv[2], "destroy") ? 2 : 3;
      uint64_t in[2] = {op, argc >= 4 ? strtoull(argv[3], NULL, 0) : 0}, out[4] = {0, 0, 0, 0};
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_RING, in, 2, out, &n);
      failed |= kr != 0;
      printf("psp %s -> kr 0x%08x  C2PMSG_64 0x%08x  ring MC 0x%llx  region-zero %llu  wptr 0x%llx\n", argv[2], kr,
             (uint32_t)out[0], (unsigned long long)out[1], (unsigned long long)out[2], (unsigned long long)out[3]);
   } else if (!strcmp(argv[1], "vram") && argc >= 3) {
      uint32_t off = (uint32_t)strtoul(argv[2], NULL, 0);
      uint32_t count = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 8;
      if (count < 1 || count > 256)
         count = 8;
      uint64_t in[2] = {off, count};
      uint32_t buf[256];
      size_t sz = count * 4;
      kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_VRAM_READ, in, 2, NULL, 0, NULL, NULL, buf, &sz);
      failed |= kr != 0;
      if (kr) {
         printf("vram read error 0x%08x\n", kr);
      } else {
         for (uint32_t i = 0; i < count; i++) {
            if (i % 8 == 0)
               printf("\nvram+0x%08x:", off + i * 4);
            printf(" %08x", buf[i]);
         }
         printf("\n");
      }
   } else if (!strcmp(argv[1], "submit") && argc >= 3) {
      uint64_t in[1] = {strtoull(argv[2], NULL, 0)}, out[4] = {0, 0, 0, 0};
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_SUBMIT, in, 1, out, &n);
      failed |= kr != 0 || out[0] != 0;
      printf("submit cmd 0x%llx -> kr 0x%08x  resp.status 0x%08x  fence %llu us  uresp 0x%08x 0x%08x\n",
             (unsigned long long)in[0], kr, (uint32_t)out[0], (unsigned long long)out[1], (uint32_t)out[2],
             (uint32_t)out[3]);
   } else if (!strcmp(argv[1], "tmr")) {
      uint64_t in[2] = {1, argc >= 3 ? strtoull(argv[2], NULL, 0) : 0}, out[4] = {0, 0, 0, 0};
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_CTL, in, 2, out, &n);
      failed |= kr != 0 || out[0] != 0;
      printf("tmr (size 0x%llx) -> kr 0x%08x  status 0x%08x  %llu us\n", (unsigned long long)in[1], kr,
             (uint32_t)out[0], (unsigned long long)out[1]);
   } else if (!strcmp(argv[1], "autoload")) {
      uint64_t in[2] = {2, 0}, out[4] = {0, 0, 0, 0};
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_CTL, in, 2, out, &n);
      failed |= kr != 0 || out[0] != 0;
      printf("autoload -> kr 0x%08x  status 0x%08x  %llu us\n", kr, (uint32_t)out[0], (unsigned long long)out[1]);
   } else if (!strcmp(argv[1], "loadfw") && argc >= 7) {
      const char *path = argv[2];
      uint32_t foff = (uint32_t)strtoul(argv[3], NULL, 0), size = (uint32_t)strtoul(argv[4], NULL, 0),
               fwtype = (uint32_t)strtoul(argv[5], NULL, 0), bufoff = (uint32_t)strtoul(argv[6], NULL, 0);
      FILE *f = fopen(path, "rb");
      if (!f) {
         perror("open");
         return 1;
      }
      if (fseek(f, foff, SEEK_SET) != 0) {
         perror("seek");
         fclose(f);
         return 1;
      }
      uint8_t buf[4096];
      uint32_t staged = 0;
      while (staged < size) {
         uint32_t chunk = size - staged > 4096 ? 4096 : size - staged;
         size_t got = fread(buf, 1, chunk, f);
         if (got != chunk) {
            if (ferror(f))
               perror("read");
            else
               fprintf(stderr, "short firmware read: requested %u bytes, got %u\n", size, staged + (uint32_t)got);
            fclose(f);
            return 1;
         }
         uint64_t in[1] = {bufoff + staged};
         kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_FW_STAGE, in, 1, buf, got, NULL, NULL, NULL, NULL);
         if (kr) {
            printf("stage @0x%x err 0x%08x\n", bufoff + staged, kr);
            fclose(f);
            return 1;
         }
         staged += (uint32_t)got;
      }
      fclose(f);
      uint64_t lin[3] = {fwtype, bufoff, staged}, out[4] = {0, 0, 0, 0};
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_LOADIPFW, lin, 3, out, &n);
      failed |= kr != 0 || out[0] != 0;
      printf("loadfw %s[0x%x:+%u] fwtype %u @fwbuf+0x%x -> kr 0x%08x  status 0x%08x  %llu us  fw_addr 0x%08x%08x\n",
             path, foff, staged, fwtype, bufoff, kr, (uint32_t)out[0], (unsigned long long)out[1], (uint32_t)out[3],
             (uint32_t)out[2]);
   } else if (!strcmp(argv[1], "stage") && argc >= 6) {
      const char *path = argv[2];
      uint32_t foff = (uint32_t)strtoul(argv[3], NULL, 0), size = (uint32_t)strtoul(argv[4], NULL, 0),
               bufoff = (uint32_t)strtoul(argv[5], NULL, 0);
      FILE *f = fopen(path, "rb");
      if (!f || fseek(f, foff, SEEK_SET) != 0) {
         perror("open/seek");
         return 1;
      }
      uint8_t buf[4096];
      uint32_t staged = 0;
      while (staged < size) {
         uint32_t chunk = size - staged > 4096 ? 4096 : size - staged;
         size_t got = fread(buf, 1, chunk, f);
         if (got != chunk) {
            if (ferror(f))
               perror("read");
            else
               fprintf(stderr, "short firmware read: requested %u bytes, got %u\n", size, staged + (uint32_t)got);
            fclose(f);
            return 1;
         }
         uint64_t in[1] = {bufoff + staged};
         kern_return_t kr = IOConnectCallMethod(conn, SEL_DBG_FW_STAGE, in, 1, buf, got, NULL, NULL, NULL, NULL);
         if (kr) {
            printf("stage @0x%x err 0x%08x\n", bufoff + staged, kr);
            fclose(f);
            return 1;
         }
         staged += (uint32_t)got;
      }
      fclose(f);
      printf("staged %s[0x%x:+%u] @fwbuf+0x%x\n", path, foff, staged, bufoff);
   } else if (!strcmp(argv[1], "wreg") && argc >= 4) {
      uint64_t in[2] = {strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0)}, out[1] = {0};
      uint32_t n = 1;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_WRITE_REG, in, 2, out, &n);
      failed |= kr != 0;
      printf("wreg 0x%llx = 0x%llx -> kr 0x%08x  readback 0x%08x\n", (unsigned long long)in[0],
             (unsigned long long)in[1], kr, (uint32_t)out[0]);
   } else if (!strcmp(argv[1], "vramw") && argc >= 4) {
      uint64_t in[2] = {strtoull(argv[2], NULL, 0), strtoull(argv[3], NULL, 0)};
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, in, 2, NULL, NULL);
      failed |= kr != 0;
      printf("vramw +0x%llx = 0x%llx -> kr 0x%08x\n", (unsigned long long)in[0], (unsigned long long)in[1], kr);
   } else if (!strcmp(argv[1], "pte") && argc >= 6) {
      // pte <tableoff> <n> <pa> <flags>: flat PTE array, pte[i] = (pa + i*4096) | flags
      uint64_t toff = strtoull(argv[2], NULL, 0), cnt = strtoull(argv[3], NULL, 0);
      uint64_t pa = strtoull(argv[4], NULL, 0), fl = strtoull(argv[5], NULL, 0);
      kern_return_t kr = 0;
      for (uint64_t i = 0; i < cnt && !kr; i++) {
         uint64_t v = (pa + i * 4096) | fl;
         uint64_t lo[2] = {toff + i * 8, v & 0xffffffffu}, hi[2] = {toff + i * 8 + 4, v >> 32};
         kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, lo, 2, NULL, NULL);
         if (!kr) kr = IOConnectCallScalarMethod(conn, SEL_DBG_VRAM_WRITE, hi, 2, NULL, NULL);
      }
      failed |= kr != 0;
      printf("pte table+0x%llx n=%llu pa=0x%llx flags=0x%llx -> kr 0x%08x\n", (unsigned long long)toff,
             (unsigned long long)cnt, (unsigned long long)pa, (unsigned long long)fl, kr);
   } else if (!strcmp(argv[1], "raw") && argc >= 3) {
      uint64_t in[7] = {0, 0, 0, 0, 0, 0, 0}, out[4] = {0, 0, 0, 0};
      in[0] = strtoull(argv[2], NULL, 0);
      for (int i = 3; i < argc && i < 9; i++)
         in[i - 2] = strtoull(argv[i], NULL, 0);
      uint32_t n = 4;
      kern_return_t kr = IOConnectCallScalarMethod(conn, SEL_DBG_PSP_RAW, in, 7, out, &n);
      failed |= kr != 0 || out[0] != 0;
      printf("raw cmd 0x%llx -> kr 0x%08x  status 0x%08x  %llu us  resp 0x%08x 0x%08x\n",
             (unsigned long long)in[0], kr, (uint32_t)out[0], (unsigned long long)out[1], (uint32_t)out[2],
             (uint32_t)out[3]);
   } else if (!strcmp(argv[1], "smuinfo")) {
      failed |= do_smu(0x01, 0x1234, "TestMessage");
      failed |= do_smu(0x02, 0, "GetPmfwVersion");
      failed |= do_smu(0x03, 0, "GetDriverIfVersion");
      failed |= do_smu(0x12, 0, "GetEnabledFeatures lo");
      failed |= do_smu(0x12, 1, "GetEnabledFeatures hi");
   } else {
      fprintf(stderr, "unknown command\n");
      return 2;
   }
   IOServiceClose(conn);
   return failed;
}
