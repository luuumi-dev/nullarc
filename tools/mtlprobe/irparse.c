// irparse: reads LLVM text IR with the plugin's own libLLVM (air-runtime), optionally runs a pass pipeline, and reports
// the reader's error or the result's size: the linked-kernel retry's steps, offline (B-29).
// Usage: irparse <libLLVM.dylib> <file.ll> [pipeline [out.ll]]
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: irparse <libLLVM.dylib> <file.ll> [pipeline [out.ll]]\n"); return 2; }
    void *L = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!L) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    void *(*ctx_create)(void) = dlsym(L, "LLVMContextCreate");
    void *(*buf_copy)(const char *, size_t, const char *) = dlsym(L, "LLVMCreateMemoryBufferWithMemoryRangeCopy");
    int (*parse_ir)(void *, void *, void **, char **) = dlsym(L, "LLVMParseIRInContext");
    void *(*run_passes)(void *, const char *, void *, void *) = dlsym(L, "LLVMRunPasses");
    void *(*pbo)(void) = dlsym(L, "LLVMCreatePassBuilderOptions");
    char *(*errmsg)(void *) = dlsym(L, "LLVMGetErrorMessage");
    char *(*print)(void *) = dlsym(L, "LLVMPrintModuleToString");
    if (!ctx_create || !buf_copy || !parse_ir) { fprintf(stderr, "libLLVM lacks the C API needed\n"); return 1; }
    FILE *f = fopen(argv[2], "rb"); if (!f) { perror(argv[2]); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = malloc(n + 1); fread(text, 1, n, f); text[n] = 0; fclose(f);
    void *ctx = ctx_create(), *mod = NULL; char *msg = NULL;
    if (parse_ir(ctx, buf_copy(text, n, "in"), &mod, &msg) || !mod) { printf("parse FAILED: %s\n", msg ? msg : "?"); return 1; }
    printf("parse ok (%ld bytes)\n", n);
    if (argc > 3 && run_passes && pbo) {
        void *e = run_passes(mod, argv[3], NULL, pbo());
        if (e) { printf("passes FAILED: %s\n", errmsg ? errmsg(e) : "?"); return 1; }
        char *out = print(mod); printf("passes ok -> %zu bytes\n", strlen(out));
        if (argc > 4) { FILE *o = fopen(argv[4], "wb"); fputs(out, o); fclose(o); }
    }
    return 0;
}
