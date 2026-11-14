// Native B-63 check: failed render encoding must retire without a GPU submission.
// Compile with -fobjc-arc -framework Foundation -framework Metal -I../../src/nmplugin.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include "nvmtl_vk.h"

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        void *bundle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!bundle) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) __typeof__(&nvmtl_vk_##name) name = dlsym(bundle, "nvmtl_vk_" #name); if (!name) return 1
        LOAD(queue_create); LOAD(queue_destroy); LOAD(cmd_begin); LOAD(cmd_begin_pass);
        LOAD(submit_wait_at); LOAD(cmd_counts); LOAD(cmd_abandon);
#undef LOAD
        nvk_queue queue = {0}; nvk_cmdbuf commands = {0};
        if (queue_create(&queue)) return 1;
        if (cmd_begin(&queue, &commands)) { queue_destroy(&queue); return 1; }
        int control = submit_wait_at(&commands, "renderfailprobe-control");
        if (control) { cmd_abandon(&commands); queue_destroy(&queue); return 1; }
        if (cmd_begin(&queue, &commands)) { queue_destroy(&queue); return 1; }
        uint64_t begun, submittedBefore, abandoned, submittedAfter;
        cmd_counts(&begun, &submittedBefore, &abandoned);
        // No VkRenderPass handle: rejected before recording any Vulkan render command.
        nvk_pass pass = {0}; nvk_pipeline pipeline = {0};
        int begin = cmd_begin_pass(&commands, &pass, &pipeline);
        unsigned sticky = commands.encoding_error;
        int submit = submit_wait_at(&commands, "renderfailprobe-refused");
        cmd_counts(&begun, &submittedAfter, &abandoned);
        int bad = begin == 0 || !sticky || submit != -1 || commands.cb || submittedAfter != submittedBefore;
        printf("control=%d begin=%d encoding_error=%u submit=%d retired=%d GPU submissions=%llu -> %llu\n",
               control, begin, sticky, submit, commands.cb == NULL,
               (unsigned long long)submittedBefore, (unsigned long long)submittedAfter);
        cmd_abandon(&commands); queue_destroy(&queue);
        return bad ? 1 : 0;
    }
}
