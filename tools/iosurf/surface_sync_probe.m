#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOKit/IOKitLib.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface NSObject (LocalDeviceInit)
- (instancetype)initWithAcceleratorPort:(mach_port_t)port;
@end
static void fail(NSString *message) { fprintf(stderr, "%s\n", message.UTF8String); exit(2); }
static void require(BOOL value, NSString *message) { if (!value) fail(message); }
static BOOL hostBacked(id object) {
    Ivar parent = class_getInstanceVariable(object_getClass(object), "_parent");
    require(parent != NULL, @"No parent ivar");
    object = object_getIvar(object, parent) ?: object;
    Ivar ivar = class_getInstanceVariable(object_getClass(object), "_hostOn");
    require(ivar && !strcmp(ivar_getTypeEncoding(ivar), @encode(BOOL)), @"Unexpected host import ABI");
    BOOL value = NO; memcpy(&value, (const char *)(__bridge const void *)object + ivar_getOffset(ivar), sizeof value); return value;
}
static id<MTLCommandBuffer> clear(id<MTLCommandQueue> q, id<MTLTexture> t, unsigned color) {
    id<MTLCommandBuffer> cb = [q commandBuffer]; require(cb != nil, @"No clear command buffer");
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t; p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake((color & 1) != 0, (color & 2) != 0, (color & 4) != 0, 1);
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p]; require(e != nil, @"No clear encoder");
    [e endEncoding]; [cb commit]; return cb;
}
static void completed(id<MTLCommandBuffer> cb) {
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error, [NSString stringWithFormat:@"GPU completion failed: %@", cb.error]);
}
static NSUInteger checkPixels(const void *base, NSUInteger width, NSUInteger row, unsigned color) {
    require(base != NULL, @"Missing readback");
    uint8_t expected[4] = {(color & 4) ? 255 : 0, (color & 2) ? 255 : 0, (color & 1) ? 255 : 0, 255};
    for (NSUInteger y = 0; y < width; y++) for (NSUInteger x = 0; x < width; x++) {
        const uint8_t *v = (const uint8_t *)base + y * row + x * 4;
        for (unsigned k = 0; k < 4; k++) if (v[k] != expected[k])
            fail([NSString stringWithFormat:@"Pixel mismatch at %lu,%lu channel %u: %u != %u", x, y, k, v[k], expected[k]]);
    }
    return width * width;
}
static NSUInteger checkPlane(const void *base, NSUInteger width, NSUInteger row, NSUInteger bpp, unsigned color) {
    require(base != NULL, @"Missing plane readback");
    const uint8_t expected[2] = {(color & 1) ? 255 : 0, (color & 2) ? 255 : 0};
    for (NSUInteger y = 0; y < width; y++) for (NSUInteger x = 0; x < width; x++) {
        const uint8_t *v = (const uint8_t *)base + y * row + x * bpp;
        for (NSUInteger k = 0; k < bpp; k++) if (v[k] != expected[k])
            fail([NSString stringWithFormat:@"Plane pixel mismatch at %lu,%lu channel %lu: %u != %u", x, y, k, v[k], expected[k]]);
    }
    return width * width;
}
static NSDictionary *exercisePlanes(id<MTLDevice> dev, id<MTLCommandQueue> queue) {
    const NSUInteger width = 64, row = 256;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth: @(width), (id)kIOSurfaceHeight: @(width),
        (id)kIOSurfacePixelFormat: @((uint32_t)0x34323076),
        (id)kIOSurfacePlaneInfo: @[
            @{(id)kIOSurfacePlaneWidth: @(width), (id)kIOSurfacePlaneHeight: @(width),
              (id)kIOSurfacePlaneBytesPerElement: @1, (id)kIOSurfacePlaneBytesPerRow: @(row)},
            @{(id)kIOSurfacePlaneWidth: @(width/2), (id)kIOSurfacePlaneHeight: @(width/2),
              (id)kIOSurfacePlaneBytesPerElement: @2, (id)kIOSurfacePlaneBytesPerRow: @(row)}]});
    require(surface && IOSurfaceGetPlaneCount(surface) == 2, @"No two-plane NV12 surface");
    NSMutableArray *textures = [NSMutableArray new];
    for (NSUInteger plane = 0; plane < 2; plane++) {
        NSUInteger w = plane ? width / 2 : width;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:plane ? MTLPixelFormatRG8Unorm : MTLPixelFormatR8Unorm width:w height:w mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:surface plane:plane];
        require(t && !hostBacked(t), @"Planar texture did not use the private path");
        [textures addObject:t];
    }
    NSUInteger pixels = 0;
    for (unsigned round = 0; round < 4; round++) { @autoreleasepool {
        NSMutableArray *writers = [NSMutableArray new];
        unsigned colors[2] = {(round & 1) ? 1u : 0u, (round & 1) ? 2u : 3u};
        for (NSUInteger plane = 0; plane < 2; plane++) [writers addObject:clear(queue, textures[plane], colors[plane])];
        for (NSUInteger plane = 0; plane < 2; plane++) {
            NSUInteger w = plane ? width / 2 : width, bpp = plane ? 2 : 1;
            id<MTLBuffer> out = [dev newBufferWithLength:row*w options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> read = [queue commandBuffer];
            id<MTLBlitCommandEncoder> e = [read blitCommandEncoder]; require(out && read && e, @"No planar readback");
            [e copyFromTexture:textures[plane] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(w,w,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*w];
            [e endEncoding]; [read commit]; completed(read);
            pixels += checkPlane(out.contents, w, row, bpp, colors[plane]);
        }
        for (id<MTLCommandBuffer> cb in writers) completed(cb);
        require(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, @"Planar CPU lock failed");
        for (NSUInteger plane = 0; plane < 2; plane++)
            pixels += checkPlane(IOSurfaceGetBaseAddressOfPlane(surface, plane), plane ? width/2 : width,
                                 IOSurfaceGetBytesPerRowOfPlane(surface, plane), plane ? 2 : 1, colors[plane]);
        require(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, @"Planar CPU unlock failed");
    }}
    CFRelease(surface);
    return @{@"passed": @YES, @"pixels_checked": @(pixels), @"cases": @[
        @{@"size": @64, @"plane": @0, @"host_import": @NO, @"gpu_readback_and_cpu_lock_correct": @YES},
        @{@"size": @32, @"plane": @1, @"host_import": @NO, @"gpu_readback_and_cpu_lock_correct": @YES}],
        @"scope": @"Two-plane IOSurface production fallback correctness; not desktop FPS"};
}
int main(int argc, const char **argv) { @autoreleasepool {
    require((argc == 2 || (argc == 3 && !strcmp(argv[2], "planar"))) && !objc_getClass("NVMTLDevice"), @"Expected isolated plugin path");
    // Release constructors clear inherited tuning variables. Restore only the explicit test control.
    NSString *privatePath = getenv("NVMTL_NO_SURFACE_HOST") ? @(getenv("NVMTL_NO_SURFACE_HOST")) : nil;
    if (!dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) fail(@(dlerror()));
    if (privatePath) require(!setenv("NVMTL_NO_SURFACE_HOST", privatePath.UTF8String, 1), @"Cannot restore test control");
    Class cls = objc_getClass("NVMTLDevice");
    require(cls && !strcmp(class_getImageName(cls), argv[1]), @"Wrong plugin image");
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    require(service != 0, @"No accelerator");
    id<MTLDevice> dev = [[cls alloc] initWithAcceleratorPort:service]; IOObjectRelease(service);
    require(dev != nil, @"No device");
    id<MTLCommandQueue> first = [dev newCommandQueue], second = [dev newCommandQueue];
    require(first && second, @"No queues");
    if (argc == 3) {
        NSDictionary *result = exercisePlanes(dev, first);
        NSData *data = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:NULL];
        fwrite(data.bytes, 1, data.length, stdout); return 0;
    }
    NSMutableArray *cases = [NSMutableArray new]; NSUInteger pixels = 0;
    for (NSNumber *size in @[@64, @256]) { @autoreleasepool {
        NSUInteger w = size.unsignedIntegerValue, row = w * 4;
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth: @(w), (id)kIOSurfaceHeight: @(w), (id)kIOSurfaceBytesPerElement: @4,
            (id)kIOSurfaceBytesPerRow: @(row), (id)kIOSurfaceAllocSize: @(row * w),
            (id)kIOSurfacePixelFormat: @((uint32_t)0x42475241)});
        require(surface != NULL, @"No surface");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:w mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [dev newTextureWithDescriptor:td iosurface:surface plane:0];
        id<MTLTexture> alias = [dev newTextureWithDescriptor:td iosurface:surface plane:0];
        require(t && alias, @"No surface textures");
        BOOL host = hostBacked(t);
        require(host == (getenv("NVMTL_NO_SURFACE_HOST") == NULL), @"Expected memory path not active");
        for (unsigned round = 0; round < 4; round++) { @autoreleasepool {
            // Four ordered outstanding writers; the last owns the registry entry.
            NSMutableArray *writers = [NSMutableArray new];
            for (unsigned n = 0; n < 4; n++) [writers addObject:clear(first, t, (round + n) % 7 + 1)];
            unsigned expected = (round + 3) % 7 + 1;
            // First two rounds read on the producer queue before any host wait.
            // Later rounds cross queues only after an explicit dependency.
            if (round >= 2) for (id<MTLCommandBuffer> cb in writers) completed(cb);
            id<MTLCommandQueue> readQueue = round >= 2 ? second : first;
            id<MTLCommandBuffer> read = [readQueue commandBuffer];
            id<MTLBuffer> out = [dev newBufferWithLength:row*w options:MTLResourceStorageModeShared];
            id<MTLBlitCommandEncoder> blit = [read blitCommandEncoder]; require(read && out && blit, @"No readback resources");
            [blit copyFromTexture:alias sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(w,w,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*w];
            [blit endEncoding]; [read commit]; completed(read);
            for (id<MTLCommandBuffer> cb in writers) completed(cb);
            pixels += checkPixels(out.contents, w, row, expected);
            require(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, @"CPU surface lock failed");
            pixels += checkPixels(IOSurfaceGetBaseAddress(surface), w, IOSurfaceGetBytesPerRow(surface), expected);
            require(IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL) == kIOReturnSuccess, @"CPU surface unlock failed");
        }}
        [cases addObject:@{@"size": @(w), @"host_import": @(host), @"rounds": @4, @"writers_per_round": @4,
                          @"gpu_readback_and_cpu_lock_correct": @YES}];
        CFRelease(surface);
    }}
    NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"passed": @YES, @"plugin": @(argv[1]),
        @"pixels_checked": @(pixels), @"cases": cases, @"completion_waits_used": @YES,
        @"scope": @"Independent process IOSurface correctness; not WindowServer stability or FPS"}
        options:NSJSONWritingPrettyPrinted error:NULL];
    fwrite(json.bytes, 1, json.length, stdout); return 0;
}}
