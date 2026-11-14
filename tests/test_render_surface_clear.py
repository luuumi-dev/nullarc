"""Replay actual render surface preparation branches with a partial clear."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def preparation(source, start):
    begin = source.index('if (sr->_surf) {', start)
    tail = source.index('else if (sr->_bufCopy)', begin)
    end = source.index('}', tail) + 1
    return source[begin:end]


class RenderSurfaceClear(unittest.TestCase):
    def test_partial_clear_preserves_untouched_pixels(self):
        source = (ROOT / 'src/nmplugin/NVMTLObjects.m').read_text()
        secondary = preparation(source, source.index('if (i) {', source.index('uint32_t load = 0')))
        primary = preparation(source, source.index('- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:'))
        code = r'''
#include <cassert>
struct Texture { bool _surf, _bufCopy; int cpu[4]={1,2,3,4}; int gpu[4]={9,9,9,9}; };
struct Buffer { bool overwritten=false; };
struct Attachment { int loadAction; };
struct Descriptor { Attachment colorAttachments[1]; };
constexpr int MTLLoadActionClear=2;
void nvmtl_surface_in_for_cb(Buffer *cb, Texture *t) {
    if (!cb->overwritten) for (int i=0;i<4;i++) t->gpu[i]=t->cpu[i];
}
void nvmtl_surface_overwrite_recorded(Buffer *cb, Texture *) { cb->overwritten=true; }
void nvmtl_surf_dirty(Buffer *, Texture *) {}
'''
        code += 'void secondary(Buffer *cb, Texture *sr, Attachment ca) {\n' + secondary + '\n}\n'
        code += 'void primary(Buffer *self, Texture *sr, Descriptor d) {\n' + primary + '\n}\n'
        code += r'''
int main() {
    for (bool primaryPath : {false,true}) for (bool surface : {false,true}) for (int action : {1,2}) {
        Texture texture{surface,!surface}; Buffer cb; Descriptor d{{{action}}};
        if (primaryPath) primary(&cb,&texture,d); else secondary(&cb,&texture,d.colorAttachments[0]);
        // Vulkan clears only this pass's smaller render area, then another command reads the rest.
        if (action==MTLLoadActionClear) texture.gpu[0]=0;
        assert(texture.gpu[1]==2 && texture.gpu[2]==3 && texture.gpu[3]==4);
        assert(!cb.overwritten);
    }
}
'''
        code = '#include <initializer_list>\n' + code
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.cpp'
            path.write_text(code)
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', str(path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
