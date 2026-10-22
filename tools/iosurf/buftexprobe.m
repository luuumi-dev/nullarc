// buftexprobe: a texture made from a buffer with a row pitch RADV cannot alias (CoreAnimation's 234x18 text line, rows
// of 960 bytes, at offset 256) must exist and must show what the CPU last wrote into the buffer, before and after a
// rewrite. Blits the texture into a second buffer and compares. Prints "PASS" or the first difference.
#import <Metal/Metal.h>
#include <stdio.h>

static int check(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t, const uint32_t *want, NSUInteger w, NSUInteger h, NSUInteger bpr, const char *what)
{
    id<MTLBuffer> out = [dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint32_t *got = out.contents;
    for (NSUInteger y = 0; y < h; y++) for (NSUInteger x = 0; x < w; x++)
        if (got[y * w + x] != want[y * (bpr / 4) + x]) { printf("%s: FAIL at %lu,%lu: 0x%08x want 0x%08x\n", what, (unsigned long)x, (unsigned long)y, got[y * w + x], want[y * (bpr / 4) + x]); return 1; }
    printf("%s: ok\n", what); return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
        const NSUInteger w = argc > 2 ? atoi(argv[1]) : 234, h = argc > 2 ? atoi(argv[2]) : 18, bpr = argc > 3 ? atoi(argv[3]) : 960, off = 256;
        id<MTLBuffer> b = [dev newBufferWithLength:off + bpr * h options:MTLResourceStorageModeShared];
        uint32_t *px = (uint32_t *)((char *)b.contents + off);
        for (NSUInteger i = 0; i < bpr / 4 * h; i++) px[i] = 0xff000000u | (uint32_t)(i * 2654435761u >> 8);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [b newTextureWithDescriptor:d offset:off bytesPerRow:bpr];
        printf("%s: %lux%lu bytesPerRow %lu at offset %lu -> %s\n", dev.name.UTF8String, (unsigned long)w, (unsigned long)h, (unsigned long)bpr, (unsigned long)off, t ? "texture" : "nil");
        if (!t) { printf("FAIL\n"); return 1; }
        int bad = check(dev, q, t, px, w, h, bpr, "first contents");
        for (NSUInteger i = 0; i < bpr / 4 * h; i++) px[i] ^= 0x00ffffffu;   // the CPU rewrites the layer, as the next keystroke does
        bad |= check(dev, q, t, px, w, h, bpr, "after a rewrite");
        printf("%s\n", bad ? "FAIL" : "PASS");
        return bad;
    }
}
