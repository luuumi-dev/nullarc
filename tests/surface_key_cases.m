static NVMTLTexture *texture(FakeSurface *surface, NSUInteger plane, BOOL shared) {
    NVMTLTexture *t = [NVMTLTexture new];
    t->_surf = surface; t->_plane = plane; t->_vramOn = shared;
    return t;
}
static NVMTLCommandBuffer *buffer(id queue, NVMTLTexture *t) {
    NVMTLCommandBuffer *cb = [NVMTLCommandBuffer new]; cb->_queue = queue;
    NVMTLSubmissionPart *part = [NVMTLSubmissionPart new];
    part->surfaces = [NSSet setWithObject:t]; cb->_parts = @[part];
    return cb;
}
int main(void) { @autoreleasepool {
    const uint32_t ids[] = {0, 1, 255, 256, 65535, 0x80000000u, UINT32_MAX};
    const NSUInteger planes[] = {0, 1, 2, 3, 254, 255, 256, 257, UINT32_MAX,
                                (NSUInteger)UINT32_MAX + 1, NSUIntegerMax};
    NSMutableDictionary *keys = [NSMutableDictionary new];
    for (unsigned i = 0; i < sizeof ids / sizeof *ids; ++i)
        for (unsigned j = 0; j < sizeof planes / sizeof *planes; ++j) {
            id key = nvmtl_surface_key(ids[i], planes[j]);
            NSString *legacy = [NSString stringWithFormat:@"%u:%lu", ids[i], (unsigned long)planes[j]];
            assert(!keys[key]); keys[key] = legacy;
            assert([keys[nvmtl_surface_key(ids[i], planes[j])] isEqual:legacy]);
            if (planes[j] > 255) assert([key isEqual:legacy]);
            else assert([key isKindOfClass:[NSNumber class]]);
        }
    assert(keys.count == 77);
    FakeSurface surface = {12345, 42}, other = {12346, 42};
    NSObject *queue = [NSObject new], *otherQueue = [NSObject new];
    NVMTLTexture *t = texture(&surface, 0, YES), *alias = texture(&surface, 0, YES);
    NVMTLCommandBuffer *first = buffer(queue, t), *second = buffer(queue, t);
    NVMTLCommandBuffer *reader = buffer(otherQueue, t);
    assert(!nvmtl_surface_pending_write(nil, t));
    assert(!nvmtl_surface_pending_write(reader, nil));
    assert(!nvmtl_surface_pending_write(reader, texture(NULL, 0, YES)));
    nvmtl_surface_register_writes(first);
    assert(nvmtl_surface_pending_write(reader, t));
    assert(nvmtl_surface_pending_write(reader, alias));
    assert(!nvmtl_surface_pending_write(first, t));
    assert(!nvmtl_surface_pending_write(reader, texture(&surface, 256, YES)));
    assert(!nvmtl_surface_pending_write(reader, texture(&surface, 0, NO)));
    assert(!nvmtl_surface_pending_write(reader, texture(&other, 0, YES)));
    surface.seed++; assert(!nvmtl_surface_pending_write(reader, t)); surface.seed--;
    nvmtl_surface_register_writes(second); nvmtl_surface_finish_writes(first);
    assert(nvmtl_surface_pending_write(reader, t));
    nvmtl_surface_finish_writes(second);
    assert(!nvmtl_surface_pending_write(reader, t));
    assert(![nvmtl_surface_writers objectForKey:queue]);
    @autoreleasepool {
        NVMTLCommandBuffer *abandoned = buffer(queue, t);
        nvmtl_surface_register_writes(abandoned);
        assert(nvmtl_surface_pending_write(reader, t));
    }
    assert(!nvmtl_surface_pending_write(reader, t));
    nvmtl_surface_finish_writes(first);
    // Large planes must keep their full-width identity in both production users.
    NVMTLTexture *large = texture(&surface, NSUIntegerMax, YES);
    NVMTLCommandBuffer *largeWriter = buffer(queue, large);
    nvmtl_surface_register_writes(largeWriter);
    assert(nvmtl_surface_pending_write(reader, texture(&surface, NSUIntegerMax, YES)));
    assert(!nvmtl_surface_pending_write(reader, texture(&surface, 255, YES)));
    nvmtl_surface_finish_writes(largeWriter);
    assert(nvmtl_surface_sync(t) == nvmtl_surface_sync(alias));
    assert(nvmtl_surface_sync(t) != nvmtl_surface_sync(large));
    NVMTLTexture *largeAlias = texture(&surface, NSUIntegerMax, YES);
    assert(nvmtl_surface_sync(large) == nvmtl_surface_sync(largeAlias));
    __weak id releasedMonitor;
    @autoreleasepool {
        NVMTLTexture *temporary = texture(&other, 255, YES);
        releasedMonitor = nvmtl_surface_sync(temporary);
        assert(releasedMonitor);
    }
    assert(!releasedMonitor);
    NSMutableArray *workers = [NSMutableArray new];
    NSMutableArray *aliases = [NSMutableArray new];
    for (unsigned i = 0; i < 8; ++i) {
        [workers addObject:buffer([NSObject new], texture(&surface, i, YES))];
        [aliases addObject:texture(&surface, 0, YES)];
    }
    id monitor = nvmtl_surface_sync(t);
    dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t worker) {
        NVMTLCommandBuffer *cb = workers[worker];
        NVMTLTexture *tx = [((NVMTLSubmissionPart *)cb->_parts[0])->surfaces anyObject];
        for (unsigned n = 0; n < 2048; ++n) { @autoreleasepool {
            nvmtl_surface_register_writes(cb);
            assert(nvmtl_surface_pending_write(reader, tx));
            assert(!nvmtl_surface_pending_write(cb, tx));
            assert(nvmtl_surface_sync(aliases[worker]) == monitor);
            nvmtl_surface_finish_writes(cb);
        }}
    });
    assert(nvmtl_surface_writers.count == 0);
    puts("surface keys: 77 unique boundary keys, 16384 concurrent registry/monitor cycles passed");
    return 0;
}}
