"""Execute the production format-view full-clear branch with a recording backend."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FormatViewClear(unittest.TestCase):
    def test_full_view_clear_then_load(self):
        source = (ROOT / 'src/nmplugin/nvmtl_vk.c').read_text()
        start = source.index('            if (!c->in_rp && img->mtl_type != 7 && ps->colfmt[i]')
        end = source.index('\n            if (c->in_rp', start)
        branch = source[start:end]
        code = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#define NVMTL_LOAD_COLOUR(i) (1u << (i))
struct nvk_image { unsigned mtl_type=2, fmt=44; };
struct nvk_cmdbuf { bool in_rp=false; };
struct nvk_pass {
    nvk_image *col[8]{}; void *colview[8]{};
    unsigned colfmt[8]{}, colw[8]{}, colh[8]{}, col_level[8]{}, col_layer[8]{};
    unsigned w=0,h=0,layers=0; float clear[8][4]{};
};
static nvk_pass recorded; static int calls, fail;
int nvmtl_vk_cmd_empty_pass(nvk_cmdbuf *, const nvk_pass *p) { recorded=*p; calls++; return fail; }
int run(nvk_cmdbuf *c, const nvk_pass *ps, unsigned &add) {
    const unsigned nl=ps->layers>1?ps->layers:1;
    for (unsigned i=0;i<8;i++) {
        auto *img=ps->col[i]; if (!img) continue;
'''
        code += branch
        code += r'''
    }
    return 0;
}
int main() {
    nvk_image image; nvk_cmdbuf c; nvk_pass p;
    p.col[3]=&image; p.colview[3]=(void *)123; p.colfmt[3]=50;
    p.colw[3]=8; p.colh[3]=4; p.w=4; p.h=2;
    p.col_level[3]=2; p.col_layer[3]=5; p.layers=2; p.clear[3][0]=0.5f; p.clear[3][3]=1;
    unsigned add=0;
    assert(run(&c,&p,add)==0 && calls==1 && add==(1u<<3));
    assert(recorded.w==8 && recorded.h==4 && recorded.layers==2);
    assert(recorded.col[0]==&image && recorded.colview[0]==(void *)123 && recorded.colfmt[0]==50);
    assert(recorded.col_level[0]==2 && recorded.col_layer[0]==5 && recorded.clear[0][0]==0.5f);
    assert(p.w==4 && p.h==2); // The original draw area stays unchanged.
    fail=1; add=0; assert(run(&c,&p,add)==-1 && add==0);
    calls=0; c.in_rp=true; assert(run(&c,&p,add)==0 && calls==0);
    c.in_rp=false; image.mtl_type=7; assert(run(&c,&p,add)==0 && calls==0);
    image.mtl_type=2; p.colfmt[3]=image.fmt; assert(run(&c,&p,add)==0 && calls==0);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.cpp'; path.write_text(code)
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', str(path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
