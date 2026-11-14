"""The real render-pass selection must never turn a failed LOAD into CLEAR."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RenderLoadFailure(unittest.TestCase):
    def test_variant_failure_refuses_pass(self):
        source = (ROOT / 'src/nmplugin/nvmtl_vk.c').read_text()
        start = source.index('    void *rpUse = rpShape ?')
        end = source.index('\n    for (uint32_t i = 0; i < ncol;', start)
        selection = source[start:end]
        code = r'''
#include <cassert>
#define nvlog(...) ((void)0)
struct nvk_pipeline { void *rp=(void *)1; };
struct nvk_pass { unsigned load=1; };
static void *variant;
void *nvmtl_rp_variant(nvk_pipeline *, unsigned, unsigned) { return variant; }
int select(nvk_pipeline *p, const nvk_pass *ps, void *rpShape, void **selected) {
    unsigned was=0;
'''
        code += selection
        code += r'''
    *selected=rpUse; return 0;
}
int main() {
    nvk_pipeline p; nvk_pass ps; void *selected=nullptr;
    variant=nullptr;
    assert(select(&p,&ps,nullptr,&selected)==-1 && selected==nullptr);
    variant=(void *)2;
    assert(select(&p,&ps,nullptr,&selected)==0 && selected==variant);
    variant=nullptr;
    assert(select(&p,&ps,(void *)3,&selected)==0 && selected==(void *)3);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.cpp'; path.write_text(code)
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', str(path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
