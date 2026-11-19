static NVMTLCommandBuffer *makeBuffer(BOOL surface) {
    NVMTLCommandBuffer *cb = [NVMTLCommandBuffer new];
    NVMTLTexture *t = [NVMTLTexture new]; t->_surf = surface ? (void *)1 : NULL;
    NVMTLSubmissionPart *part = [NVMTLSubmissionPart new]; part->surfaces = [NSSet setWithObject:t];
    cb->_parts = [NSMutableArray arrayWithObjects:[NSNull null], part, nil];
    cb->_completionCondition = [NSCondition new]; cb->_handlers = [NSMutableArray new];
    cb->resources = [NSObject new]; cb->reservation = [NSObject new]; cb->status = 2;
    nvmtl_surface_commit_prepare(cb);
    return cb;
}
static void complete(NVMTLCommandBuffer *cb) {
    [cb->_parts removeAllObjects];
    [cb nvmtlFinishCallbacks];
}
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc >= 3); const char *mode = argv[1]; programName = argv[2];
    if (!strcmp(mode, "policy")) {
        assert(argc == 4); BOOL expected = atoi(argv[3]);
        assert(nvmtl_surface_commit_wait_on() == expected);
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        assert((cb->_surfaceCommitGroup != nil) == expected); complete(cb);
    } else if (!strcmp(mode, "budget")) {
        assert(argc == 4 && nvmtl_surface_commit_wait_ms() == (unsigned)atoi(argv[3]));
    } else if (!strcmp(mode, "no-surface")) {
        NVMTLCommandBuffer *cb = makeBuffer(NO);
        assert(!cb->_surfaceCommitGroup && !nvmtl_surface_commit_wait(cb));
        assert(!atomic_load(&waitCalls));
    } else if (!strcmp(mode, "completion")) {
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        __block _Atomic int copyPublished = 0, callbackRan = 0;
        [cb->_handlers addObject:^{ atomic_store(&callbackRan, 1); }];
        started = dispatch_semaphore_create(0);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            assert(!dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
            atomic_store(&copyPublished, 1); complete(cb);
        });
        assert(nvmtl_surface_commit_wait(cb));
        assert(cb->_completionReady && atomic_load(&copyPublished) && !atomic_load(&callbackRan));
        assert(queuedHandlers.count == 1); ((dispatch_block_t)queuedHandlers[0])();
        assert(atomic_load(&callbackRan));
    } else if (!strcmp(mode, "timeout") || !strcmp(mode, "clock")) {
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        id resources = cb->resources, reservation = cb->reservation;
        injectedClock = !strcmp(mode, "clock");
        const uint64_t begin = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        assert(!nvmtl_surface_commit_wait(cb));
        const uint64_t elapsed = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - begin;
        assert(elapsed >= 8 * NSEC_PER_MSEC && elapsed < 2 * NSEC_PER_SEC);
        assert(!cb->_completionReady && cb->status == 2 && !cb->error);
        assert(cb->resources == resources && cb->reservation == reservation && cb->_parts.count == 2);
        if (injectedClock) assert(atomic_load(&lastElapsed) == 75000);
        injectedClock = NO; complete(cb); assert(nvmtl_surface_commit_wait(cb));
    } else if (!strcmp(mode, "worker")) {
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        dispatch_queue_t worker = dispatch_queue_create("cpu.worker", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(worker, &nvmtl_execution_queue_key, &nvmtl_execution_queue_key, NULL);
        dispatch_queue_t child = dispatch_queue_create("cpu.child", DISPATCH_QUEUE_SERIAL);
        dispatch_set_target_queue(child, worker);
        dispatch_sync(child, ^{ assert(!nvmtl_surface_commit_wait(cb)); });
        dispatch_sync(worker, ^{ assert(!nvmtl_surface_commit_wait(cb)); });
        assert(!atomic_load(&waitCalls));
        dispatch_queue_t collision = dispatch_queue_create("nvmtl.execution", DISPATCH_QUEUE_SERIAL);
        dispatch_sync(collision, ^{ assert(!nvmtl_surface_commit_wait(cb)); });
        assert(atomic_load(&waitCalls) == 1); complete(cb);
    } else if (!strcmp(mode, "many")) {
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        __block _Atomic unsigned ready = 0;
        started = dispatch_semaphore_create(0); dispatch_group_t done = dispatch_group_create();
        for (unsigned i = 0; i < 8; ++i) dispatch_group_async(done, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
            if (nvmtl_surface_commit_wait(cb)) atomic_fetch_add(&ready, 1);
        });
        for (unsigned i = 0; i < 8; ++i)
            assert(!dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        complete(cb);
        assert(!dispatch_group_wait(done, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
        assert(atomic_load(&ready) == 8);
    } else if (!strcmp(mode, "error")) {
        NVMTLCommandBuffer *cb = makeBuffer(YES);
        cb->status = 5; cb->error = [NSObject new]; id error = cb->error;
        complete(cb); assert(nvmtl_surface_commit_wait(cb));
        assert(cb->status == 5 && cb->error == error);
    } else { assert(!"unknown case"); }
    printf("surface commit wait %s: PASS\n", mode);
} return 0; }
