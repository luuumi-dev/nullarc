"""Execute actual render error recording and submit refusal against fake Vulkan."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_native_bo_lifetime import method

ROOT = Path(__file__).resolve().parents[1]


class RenderEncodingFailure(unittest.TestCase):
    def test_failed_begin_cannot_be_submitted(self):
        source = (ROOT / 'src/nmplugin/nvmtl_vk.c').read_text()
        wrapper = method(source, 'int nvmtl_vk_cmd_begin_pass(')
        guard_start = source.index('    if (c->encoding_error || nvmtl_counter_flush(c))')
        guard_end = source.index('\n', guard_start)
        code = r'''
#include <cassert>
struct nvk_cmdbuf { unsigned encoding_error=0; };
struct nvk_pass {};
struct nvk_pipeline {};
static int failure, retired, submitted;
int nvmtl_vk_cmd_begin_pass_impl(nvk_cmdbuf *, const nvk_pass *, nvk_pipeline *) { return failure; }
int nvmtl_counter_flush(nvk_cmdbuf *) { return 0; }
void nvmtl_cmd_retire(nvk_cmdbuf *) { retired++; }
'''
        code += wrapper
        code += '\nint submit(nvk_cmdbuf *c) {\n' + source[guard_start:guard_end] + '\nsubmitted++; return 0; }\n'
        code += r'''
int main() {
    nvk_cmdbuf c; nvk_pass pass; nvk_pipeline pipeline;
    assert(nvmtl_vk_cmd_begin_pass(&c,&pass,&pipeline)==0 && !c.encoding_error);
    assert(submit(&c)==0 && submitted==1);
    failure=-1;
    assert(nvmtl_vk_cmd_begin_pass(&c,&pass,&pipeline)==-1 && c.encoding_error);
    failure=0;
    assert(nvmtl_vk_cmd_begin_pass(&c,&pass,&pipeline)==0 && c.encoding_error);
    assert(submit(&c)==-1 && retired==1 && submitted==1);
}
'''
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'test.cpp'; path.write_text(code)
            binary = Path(temp) / 'test'
            subprocess.run(['clang++', '-std=c++17', str(path), '-o', str(binary)], check=True, capture_output=True)
            subprocess.run([str(binary)], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
