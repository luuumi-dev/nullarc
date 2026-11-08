/* SPDX-License-Identifier: MIT
 * Exercises the actual patched Mesa marshaller, not a separate decoder.
 */
#include "ac_darwin_kext.h"
#include "phx_hp_760m.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int acd_mock_open(struct acd_kext **out) { (void)out; return -ENODEV; }
int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }
static struct phx_topology response;
static size_t wire_size;
static int kr;
static int call(void *ctx, uint32_t sel, const uint64_t *si, uint32_t ni,
                const void *bi, size_t nbi, uint64_t *so, uint32_t *no, void *bo, size_t *nbo) {
    (void)ctx; (void)si; (void)bi; (void)so; (void)no;
    assert(sel == PHX_SEL_TOPOLOGY && ni == 0 && nbi == 0 && *nbo == 128);
    if (kr) return kr;
    memcpy(bo, &response, sizeof(response)); *nbo = wire_size;
    return 0;
}
int main(void) {
    const struct acd_kext_ops ops = {.call = call};
    struct acd_kext *k = acd_kext_create(&ops, NULL);
    assert(k);
    struct phx_topology p;
    response = phx_hp_760m; wire_size = sizeof(response);
    assert(!acd_kext_query_topology(k, &p));
    assert(p.cu_active_number == 8 && p.enabled_rb_pipes_mask == 10);
    response.cu_bitmap[0][0] = 0x33; response.cu_bitmap[0][1] = 0x2d;
    assert(!acd_kext_query_topology(k, &p) && p.cu_bitmap[0][0] == 0x33);
    response.cu_active_number = 12;
    assert(acd_kext_query_topology(k, &p) == -EPROTO);
    response = phx_hp_760m; wire_size--;
    assert(acd_kext_query_topology(k, &p) == -EPROTO);
    wire_size = sizeof(response) + 4;
    assert(acd_kext_query_topology(k, &p) == -EPROTO);
    wire_size = sizeof(response); response.version = 99;
    assert(acd_kext_query_topology(k, &p) == -EPROTO);
    kr = (int)ACD_KR_UNSUPPORTED;
    assert(acd_kext_query_topology(k, &p) == -EINVAL);
    kr = (int)ACD_KR_NOT_READY;
    assert(acd_kext_query_topology(k, &p) == -ENODEV);
    acd_kext_destroy(k);
    puts("[PASS] Mesa topology transport, malformed responses and legacy fallback");
}
