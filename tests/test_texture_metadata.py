"""Regressions for drawable metadata and IOSurface cache keys (PRs #22 and #16)."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / 'src/nmplugin/NVMTLDevice.m'
OBJECTS = ROOT / 'src/nmplugin/NVMTLObjects.m'


class TextureMetadataTest(unittest.TestCase):
    def test_descriptor_and_view_paths_preserve_drawable(self):
        device, objects = DEVICE.read_text(), OBJECTS.read_text()
        self.assertEqual(device.count('t->_drawable = nvmtl_desc_drawable(d);'), 2)
        self.assertIn('alias->_drawable = t->_drawable;', device)
        self.assertEqual(objects.count('v->_drawable = _drawable;'), 2)
        self.assertIn('- (BOOL)isDrawable { return _drawable; }', objects)
        self.assertNotIn('NSArray *key = surf ?', device)
        self.assertIn('static void nvmtl_surface_keep(id dev, NSString *key,', device)

    @unittest.skipUnless(platform.system() == 'Darwin', 'requires Foundation / Objective-C runtime')
    def test_native_descriptor_flag_hashes_and_weak_cache_sweep(self):
        device, objects = DEVICE.read_text(), OBJECTS.read_text()
        key_start = device.index('NSString *key = surf ?')
        key = device[key_start:device.index(';', key_start) + 1]
        sweep = method(device, 'if (key && cache.count > 256)')
        code = r'''
#import <Foundation/Foundation.h>
#import <objc/message.h>
#include <assert.h>
@interface NullarcTestTextureDescriptor : NSObject
@property NSUInteger pixelFormat;
- (BOOL)isDrawable;
@end
@implementation NullarcTestTextureDescriptor
- (BOOL)isDrawable { return YES; }
@end
'''
        code += method(device, 'static BOOL nvmtl_desc_drawable(').replace('MTLTextureDescriptor *', 'NullarcTestTextureDescriptor *')
        code += r'''
@interface NVMTLTexture : NSObject { @public BOOL _drawable, _fbo; NSUInteger _ropt; }
- (BOOL)isDrawable;
@end
@implementation NVMTLTexture
'''
        code += method(objects, '- (BOOL)isDrawable ')
        code += r'''
@end
static NSString *surface_key(unsigned sid, NSUInteger plane, size_t w, size_t h, NSUInteger format, NSUInteger options, BOOL fbo) {
    BOOL surf=YES;
    NullarcTestTextureDescriptor *d=[NullarcTestTextureDescriptor new]; d.pixelFormat=format;
    NVMTLTexture *t=[NVMTLTexture new]; t->_ropt=options; t->_fbo=fbo;
#define IOSurfaceGetID(s) sid
'''
        code += key + '\nreturn key;\n}\n'
        code += r'''
int main() { @autoreleasepool {
    assert(!nvmtl_desc_drawable(nil));
    assert(!nvmtl_desc_drawable((id)[NSObject new]));
    assert(nvmtl_desc_drawable([NullarcTestTextureDescriptor new]));
    NVMTLTexture *texture=[NVMTLTexture new];
    assert(![texture isDrawable]); texture->_drawable=YES; assert([texture isDrawable]);
    NSString *base=surface_key(100, 0, 640, 480, 80, 0, NO);
    assert([base isEqual:surface_key(100, 0, 640, 480, 80, 0, NO)]);
    assert(![base isEqual:surface_key(101, 0, 640, 480, 80, 0, NO)]);
    assert(![base isEqual:surface_key(100, 1, 640, 480, 80, 0, NO)]);
    assert(![base isEqual:surface_key(100, 0, 641, 480, 80, 0, NO)]);
    assert(![base isEqual:surface_key(100, 0, 640, 481, 80, 0, NO)]);
    assert(![base isEqual:surface_key(100, 0, 640, 480, 81, 0, NO)]);
    assert(![base isEqual:surface_key(100, 0, 640, 480, 80, 1, NO)]);
    assert(![base isEqual:surface_key(100, 0, 640, 480, 80, 0, YES)]);
    NSMutableSet *hashes=[NSMutableSet new];
    NSMapTable *cache=[NSMapTable strongToWeakObjectsMapTable];
    NSObject *live=[NSObject new];
    NSString *key=@"live"; [cache setObject:live forKey:key];
    @autoreleasepool {
        for (unsigned i=0; i<300; ++i) {
            NSString *k=surface_key(i, 0, 640, 480, 80, 0, NO);
            [hashes addObject:@(k.hash)];
            [cache setObject:[NSObject new] forKey:k];
        }
    }
    assert(hashes.count > 250);
'''
        code += sweep
        code += r'''
    assert([cache objectForKey:key] == live);
    assert(cache.count <= 256);
} }
'''
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / 'metadata.m', Path(directory) / 'metadata'
            source.write_text(code)
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror', str(source), '-framework', 'Foundation', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
