// buftexprobe: a texture made from a buffer with a row pitch RADV cannot alias (CoreAnimation's 234x18 text line, rows
// of 960 bytes, at offset 256) must exist and must show what the CPU last wrote into the buffer, before and after a
// rewrite. Blits the texture into a second buffer and compares. Prints "PASS" or the first difference.
// Usage: buftexprobe [w h bytesPerRow [a8|rt]]   (a8: an A8Unorm glyph mask, one byte a texel, as CoreText's;
//        rt: a render target - a cleared pass must land in the buffer's bytes)
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static int check(id<MTLDevice> dev, id<MTLCommandQueue> q, id<MTLTexture> t, const uint8_t *want, NSUInteger w, NSUInteger h, NSUInteger bpr, NSUInteger bpp, const char *what)
{
    id<MTLBuffer> out = [dev newBufferWithLength:w * h * bpp options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:out destinationOffset:0 destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp];
    [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *got = out.contents;
    for (NSUInteger y = 0; y < h; y++)
        if (memcmp(got + y * w * bpp, want + y * bpr, w * bpp)) { printf("%s: FAIL in row %lu\n", what, (unsigned long)y); return 1; }
    printf("%s: ok\n", what); return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
        const NSUInteger w = argc > 2 ? atoi(argv[1]) : 234, h = argc > 2 ? atoi(argv[2]) : 18, bpr = argc > 3 ? atoi(argv[3]) : 960, off = 256;
        const BOOL a8 = argc > 4 && !strcmp(argv[4], "a8"); const NSUInteger bpp = a8 ? 1 : 4;
        id<MTLBuffer> b = [dev newBufferWithLength:off + bpr * h options:MTLResourceStorageModeShared];
        uint8_t *px = (uint8_t *)b.contents + off;
        for (NSUInteger i = 0; i < bpr * h; i++) px[i] = (uint8_t)(i * 2654435761u >> 13);
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:a8 ? MTLPixelFormatA8Unorm : MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeShared;
        const BOOL rt = argc > 4 && !strcmp(argv[4], "rt");
        if (rt) d.usage |= MTLTextureUsageRenderTarget;
        id<MTLTexture> t = [b newTextureWithDescriptor:d offset:off bytesPerRow:bpr];
        if (t && rt) {   // the GPU draws (clears) into it: the CPU must then read the colour from the buffer
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
            id<MTLCommandBuffer> cb = [q commandBuffer]; [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
            NSUInteger bad = 0;
            for (NSUInteger y = 0; y < h; y++) for (NSUInteger x = 0; x < w; x++) if (((uint32_t *)(px + y * bpr))[x] != 0xffff0000u) bad++;
            printf("render target: %lu of %lu texels not red in the buffer\n%s\n", (unsigned long)bad, (unsigned long)(w * h), bad ? "FAIL" : "PASS");
            return bad != 0;
        }
        printf("%s: %lux%lu bytesPerRow %lu at offset %lu -> %s\n", dev.name.UTF8String, (unsigned long)w, (unsigned long)h, (unsigned long)bpr, (unsigned long)off, t ? "texture" : "nil");
        if (!t) { printf("FAIL\n"); return 1; }
        int bad = check(dev, q, t, px, w, h, bpr, bpp, "first contents");
        for (NSUInteger i = 0; i < bpr * h; i++) px[i] ^= 0x5au;   // the CPU rewrites the layer, as the next keystroke does
        bad |= check(dev, q, t, px, w, h, bpr, bpp, "after a rewrite");
        printf("%s\n", bad ? "FAIL" : "PASS");
        return bad;
    }
}
