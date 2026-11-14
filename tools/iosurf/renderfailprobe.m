// Native B-63 check: failed render encoding must retire without a GPU submission.
// Compile with -fobjc-arc -framework Foundation -framework Metal -framework IOKit -I../../src/nmplugin.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include "nvmtl_vk.h"
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>

@interface NSObject (RenderFailDeviceInit)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        void *bundle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!bundle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) __typeof__(&nvmtl_vk_##name) name = dlsym(bundle, "nvmtl_vk_" #name); if (!name) return 1
        LOAD(queue_create); LOAD(queue_destroy); LOAD(cmd_begin); LOAD(cmd_begin_pass);
        LOAD(submit_wait_at); LOAD(cmd_counts); LOAD(cmd_abandon);
#undef LOAD
        nvk_queue queue = {0}; nvk_cmdbuf commands = {0};
        if (queue_create(&queue)) return 1;
        if (cmd_begin(&queue, &commands)) { queue_destroy(&queue); return 1; }
        int control = submit_wait_at(&commands, "renderfailprobe-control");
        if (control) { cmd_abandon(&commands); queue_destroy(&queue); return 1; }
        if (cmd_begin(&queue, &commands)) { queue_destroy(&queue); return 1; }
        uint64_t begun, submittedBefore, abandoned, submittedAfter;
        cmd_counts(&begun, &submittedBefore, &abandoned);
        // No VkRenderPass handle: rejected before recording any Vulkan render command.
        nvk_pass pass = {0}; nvk_pipeline pipeline = {0};
        int begin = cmd_begin_pass(&commands, &pass, &pipeline);
        unsigned sticky = commands.encoding_error;
        int submit = submit_wait_at(&commands, "renderfailprobe-refused");
        cmd_counts(&begun, &submittedAfter, &abandoned);
        int bad = begin == 0 || !sticky || submit != -1 || commands.cb || submittedAfter != submittedBefore;
        printf("control=%d begin=%d encoding_error=%u submit=%d retired=%d GPU submissions=%llu -> %llu\n",
               control, begin, sticky, submit, commands.cb == NULL,
               (unsigned long long)submittedBefore, (unsigned long long)submittedAfter);
        cmd_abandon(&commands); queue_destroy(&queue);
        if (bad) return 1;
        io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
        if (!accel) return 1;
        id<MTLDevice> device = [[NSClassFromString(@"NVMTLDevice") alloc] initWithAcceleratorPort:accel];
        IOObjectRelease(accel);
        id<MTLCommandQueue> metalQueue = [device newCommandQueue];
        id<MTLCommandBuffer> cb = [metalQueue commandBuffer];
        if (!cb) return 1;
        // The test is built against this candidate's private ABI; reject a mismatched object layout.
        Ivar commandsIvar = class_getInstanceVariable(object_getClass(cb), "_c");
        if (!commandsIvar || ivar_getOffset(commandsIvar) < 0 ||
            (size_t)ivar_getOffset(commandsIvar) + sizeof(nvk_cmdbuf) > class_getInstanceSize(object_getClass(cb))) return 1;
        nvk_cmdbuf *metalCommands = (nvk_cmdbuf *)((char *)(__bridge void *)cb + ivar_getOffset(commandsIvar));
        if (cmd_begin_pass(metalCommands, &pass, &pipeline) == 0) return 1;
        [cb commit]; [cb waitUntilCompleted];
        printf("Metal status=%lu error=%s\n", (unsigned long)cb.status, cb.error.description.UTF8String ?: "none");
        return cb.status == MTLCommandBufferStatusError && cb.error &&
            [cb.error.domain isEqualToString:MTLCommandBufferErrorDomain] ? 0 : 1;
    }
}
