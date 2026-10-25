// hangprobe: hangs one compute wave on purpose (it spins until a flag the CPU never sets) and reports whether, and
// when, its command buffer completes. For testing GPU hang recovery (B-30): something outside has to kill the wave.
// Usage: hangprobe [seconds to wait, default 20]
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>

static NSString *const kSrc = @"#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void spin(device atomic_uint *flag [[buffer(0)]], device atomic_uint *count [[buffer(1)]]) {\n"
    "    while (atomic_load_explicit(flag, memory_order_relaxed) == 0u)\n"
    "        atomic_fetch_add_explicit(count, 1u, memory_order_relaxed);\n"
    "}\n";

int main(int argc, char **argv)
{
    @autoreleasepool {
        const double wait = argc > 1 ? atof(argv[1]) : 20;
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
        id<MTLComputePipelineState> ps = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&err] : nil;
        if (!ps) { fprintf(stderr, "pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLBuffer> flag = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> count = [dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
        *(uint32_t *)flag.contents = 0; *(uint32_t *)count.contents = 0;
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:ps];
        [e setBuffer:flag offset:0 atIndex:0];
        [e setBuffer:count offset:0 atIndex:1];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [e endEncoding];
        const double t0 = CACurrentMediaTime();
        [cb commit];
        printf("committed: one wave spinning\n"); fflush(stdout);
        while (cb.status < MTLCommandBufferStatusCompleted && CACurrentMediaTime() - t0 < wait) usleep(50000);
        printf("after %.2f s: status %ld, error %s, spins %u\n", CACurrentMediaTime() - t0, (long)cb.status,
               cb.error ? cb.error.localizedDescription.UTF8String : "none", *(volatile uint32_t *)count.contents);
        return cb.status == MTLCommandBufferStatusCompleted ? 0 : 1;
    }
}
