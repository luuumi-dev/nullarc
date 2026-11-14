// Verify Metal clears the entire attachment despite a smaller render area.
// Usage: clearprobe [view]; NULLARC_TEST_PLUGIN=/absolute/path selects an isolated build.
// Compile with -fobjc-arc -framework Foundation -framework Metal -framework IOSurface -framework IOKit.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>

@interface NSObject (ClearProbeDeviceInit)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end

static id<MTLDevice> testDevice(void) {
    const char *path = getenv("NULLARC_TEST_PLUGIN");
    if (!path) return MTLCreateSystemDefaultDevice();
    if (objc_getClass("NVMTLDevice")) { fprintf(stderr, "plugin class already loaded; refusing ambiguous test\n"); return nil; }
    if (!dlopen(path, RTLD_NOW | RTLD_LOCAL)) { fprintf(stderr, "plugin load failed: %s\n", dlerror()); return nil; }
    Class cls = NSClassFromString(@"NVMTLDevice");
    if (!cls || ![cls instancesRespondToSelector:@selector(initWithAcceleratorPort:)]) return nil;
    io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!accel) return nil;
    id<MTLDevice> device = [[cls alloc] initWithAcceleratorPort:accel];
    IOObjectRelease(accel);
    printf("test plugin=%s\n", path);
    return device;
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        const NSUInteger width = 8, height = 4;
        const BOOL view = argc == 2 && !strcmp(argv[1], "view");
        if (argc > 2 || (argc == 2 && !view)) return 2;
        id<MTLDevice> device = testDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!device || !queue) { fprintf(stderr, "no Metal device/queue\n"); return 1; }
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(height),
            (id)kIOSurfaceBytesPerElement: @4, (id)kIOSurfaceBytesPerRow: @32,
            (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') });
        if (!surface) { fprintf(stderr, "no IOSurface\n"); return 1; }
        if (IOSurfaceLock(surface, 0, NULL)) { CFRelease(surface); return 1; }
        for (NSUInteger y = 0; y < height; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface));
            for (NSUInteger x = 0; x < width; x++) row[x] = 0xff336699u;
        }
        if (IOSurfaceUnlock(surface, 0, NULL)) { CFRelease(surface); return 1; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width height:height mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> texture = [device newTextureWithDescriptor:td iosurface:surface plane:0];
        id<MTLBuffer> output = [device newBufferWithLength:256 * height options:MTLResourceStorageModeShared];
        if (!texture || !output) { fprintf(stderr, "allocation failed\n"); CFRelease(surface); return 1; }
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> target = view ? [texture newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm_sRGB] : texture;
        if (!target) { CFRelease(surface); return 1; }
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
        pass.renderTargetWidth = 4; pass.renderTargetHeight = 2;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
        if (!render) { fprintf(stderr, "no render encoder\n"); CFRelease(surface); return 1; }
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        if (!blit) { fprintf(stderr, "no blit encoder\n"); CFRelease(surface); return 1; }
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(width,height,1) toBuffer:output destinationOffset:0
            destinationBytesPerRow:256 destinationBytesPerImage:256 * height];
        [blit endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "command failed: %s\n", cb.error.description.UTF8String); CFRelease(surface); return 1;
        }
        unsigned bad = 0;
        for (NSUInteger y = 0; y < height; y++) {
            const uint32_t *row = (const uint32_t *)((const uint8_t *)output.contents + y * 256);
            for (NSUInteger x = 0; x < width; x++) {
                uint32_t wanted = 0xffff0000u;
                if (row[x] != wanted) {
                    if (bad < 4) printf("pixel %lu,%lu: %08x wanted %08x\n", (unsigned long)x, (unsigned long)y, row[x], wanted);
                    bad++;
                }
            }
        }
        printf("device=%s, row=%zu, wrong pixels=%u/32\n", device.name.UTF8String, IOSurfaceGetBytesPerRow(surface), bad);
        CFRelease(surface);
        return bad ? 1 : 0;
    }
}
