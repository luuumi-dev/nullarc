/* SPDX-License-Identifier: MIT
 * clang -arch x86_64 -Wall -Wextra -Werror phxprofile.c -framework IOKit -framework CoreFoundation -o phxprofile
 * ./phxprofile prints captured metadata only. No registers are written.
 */
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include "../../src/amd/phx_topology.h"
int main(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PhxBringup"));
    if (!svc) { fprintf(stderr, "PhxBringup is not loaded\n"); return 1; }
    io_connect_t conn = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0x4e34384e, &conn);
    IOObjectRelease(svc);
    uint64_t in[2] = {1, 0}, out[4]; uint32_t count = 4;
    if (!kr) kr = IOConnectCallScalarMethod(conn, 0, in, 2, out, &count);
    struct phx_topology p = {0}; size_t size = sizeof(p);
    if (!kr && count != 4) kr = kIOReturnBadArgument;
    if (!kr) kr = IOConnectCallStructMethod(conn, PHX_SEL_TOPOLOGY, NULL, 0, &p, &size);
    if (conn) IOServiceClose(conn);
    if (kr || size != sizeof(p) || !phx_topology_valid(&p)) {
        fprintf(stderr, "captured profile unavailable or invalid (0x%x); older 780M kernels do not provide it\n", kr);
        return 1;
    }
    printf("PCI %04x:%04x rev %02x subsystem %04x:%04x\n", p.vendor, p.device, p.revision, p.subvendor, p.subdevice);
    printf("CAPTURED metadata, not a live fuse read: %u CUs, %u SE, %u SA/SE, %u physical CU slots/SA, RB mask 0x%x\n",
           p.cu_active_number, p.num_shader_engines, p.num_shader_arrays_per_engine, p.num_cu_per_sh, p.enabled_rb_pipes_mask);
    for (unsigned se = 0; se < p.num_shader_engines; ++se)
        for (unsigned sa = 0; sa < p.num_shader_arrays_per_engine; ++sa)
            printf("SE%u/SA%u CU mask 0x%08x\n", se, sa, p.cu_bitmap[se][sa]);
    printf("Runtime readiness: %s\n", p.flags & PHX_TOPOLOGY_RUNTIME_READY ? "reported by kernel" : "NOT VALIDATED");
    return 0;
}
