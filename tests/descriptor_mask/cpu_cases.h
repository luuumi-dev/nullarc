static unsigned cases;
static void start(nvk_cmdbuf *c, nvmtl_bindtable *t) {
    memset(c, 0, sizeof *c); memset(t, 0, sizeof *t); c->table = t;
    fail_alloc = fail_address = 0; h3 = ubo = 1; reset_observation();
}
static void finish(nvk_cmdbuf *c) { free(c->sets); c->sets = NULL; ++cases; }
static int seen(uint32_t binding) {
    for (unsigned i = 0; i < writes + copies; ++i) if (observed_bindings[i] == binding) return 1;
    return 0;
}
static void check_snapshot_cases(void) {
    uint32_t vs[] = {0x07230203, 0x10000, 0, 8, 0,
        (4u<<16)|71u, 1, 34, 0, (4u<<16)|71u, 1, 33, 5};
    uint32_t fs[] = {0x07230203, 0x10000, 0, 8, 0,
        (4u<<16)|71u, 1, 34, 1, (4u<<16)|71u, 1, 33, 0,
        (4u<<16)|71u, 2, 34, 1, (4u<<16)|71u, 2, 33, 32,
        (4u<<16)|71u, 3, 34, 1, (4u<<16)|71u, 3, 33, 160,
        (4u<<16)|71u, 4, 34, 1, (4u<<16)|71u, 4, 33, 640,
        (4u<<16)|71u, 5, 34, 3, (4u<<16)|71u, 5, 33, 192};
    nvk_pipeline base = {0}, p; nvk_cmdbuf c; nvmtl_bindtable t;
    graphics_masks(vs, sizeof vs, fs, sizeof fs, NULL, &base);
    assert(!base.gmask_ok && base.gmask2_ok);
    const int enabled = nvmtl_desc_mask_on();
    const unsigned expected = enabled ? 4 : NVMTL_STEX_BASE;
    start(&c, &t);
    VkDescriptorSet first = nvmtl_snapshot_set_c(&c, 1, &base);
    assert(first && calls == 1 && writes == expected && !copies);
    assert(seen(0) && seen(32) && seen(160) && seen(640));
    if (enabled) assert(!seen(192));
    assert(observed_address_offset == 0);
    ++cases;
    reset_observation();
    assert(nvmtl_snapshot_set_c(&c, 1, &base) == first && !calls);
    ++cases;
    t.dirty[1] = 1; t.chg[1][0] = 1; reset_observation();
    assert(nvmtl_snapshot_set_c(&c, 1, &base) && calls == 1 && writes == expected);
    ++cases;
    p = base; p.gmask2[1][0] |= 1ull << 33; reset_observation();
    assert(nvmtl_snapshot_set_c(&c, 1, &p));
    if (enabled) { assert(calls == 1 && writes == 3 && copies == 2); assert(seen(0) && seen(32) && seen(33) && seen(160) && seen(640)); }
    else assert(!calls);
    ++cases;
    p.tbany = 1; p.tbm[1][0] = 1; reset_observation();
    assert(nvmtl_snapshot_set_c(&c, 1, &p) && calls == 1 && !copies);
    int found_texel = 0;
    for (unsigned i = 0; i < writes; ++i) if (observed_bindings[i] == 32 && observed_types[i] == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) found_texel = 1;
    assert(found_texel); finish(&c);

    start(&c, &t); p = base; p.stex_hi[1] = 3;
    assert(nvmtl_snapshot_set_c(&c, 1, &p) && writes == expected + 3);
    assert(seen(480) && seen(481) && seen(482)); finish(&c);

    start(&c, &t); p = (nvk_pipeline){0}; p.gmask_ok = 1; p.gmask[1][0] = 1ull << 7;
    assert(nvmtl_snapshot_set_c(&c, 1, &p) && writes == 1 && seen(7)); finish(&c);

    start(&c, &t); p = (nvk_pipeline){0};
    assert(nvmtl_snapshot_set_c(&c, 1, &p) && writes == NVMTL_STEX_BASE); finish(&c);

    start(&c, &t); p = (nvk_pipeline){0};
    compute_masks(vs, sizeof vs, &p); assert(p.bmask_ok && p.bmask2_ok);
    assert(nvmtl_snapshot_set_c(&c, 0, &p) && writes == 1 && seen(5)); finish(&c);

    start(&c, &t); p = (nvk_pipeline){0};
    graphics_masks(vs, sizeof vs, fs, sizeof fs, (void *)(uintptr_t)1, &p);
    assert(!p.gmask_ok && !p.gmask2_ok);
    assert(nvmtl_snapshot_set_c(&c, 1, &p) && writes == NVMTL_STEX_BASE); finish(&c);

    start(&c, &t); c.table = NULL;
    assert(nvmtl_snapshot_set_c(&c, 1, &base) && writes == NVMTL_STEX_BASE); finish(&c);

    start(&c, &t); h3 = 0;
    assert(nvmtl_snapshot_set_c(&c, 1, &base) && writes == NVMTL_STEX_BASE); finish(&c);

    start(&c, &t); fail_alloc = 1;
    assert(!nvmtl_snapshot_set_c(&c, 1, &base) && c.skipped == 1 && !calls); finish(&c);

    start(&c, &t); fail_address = 1;
    assert(!nvmtl_snapshot_set_c(&c, 1, &base) && c.skipped == 1 && !calls); finish(&c);

    start(&c, &t); ubo = 0;
    assert(nvmtl_snapshot_set_c(&c, 1, &base));
    reset_observation(); t.dirty[1] = 1; t.chg[1][0] = 1;
    assert(nvmtl_snapshot_set_c(&c, 1, &base) && writes == expected);
    int storage_address = 0;
    for (unsigned i = 0; i < writes; ++i) if (observed_bindings[i] == 640 && observed_types[i] == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) storage_address = 1;
    assert(storage_address && observed_address_offset > 0); finish(&c);
}
static void check_bad_inputs(void) {
    uint64_t out[4];
    uint32_t bad[] = {0x07230203, 0x10000, 0, 8, 0, (5u<<16)|71u, 1, 33, 1};
    assert(!nvmtl_spirv_bmask_x(NULL, 0, out, 1));
    assert(!nvmtl_spirv_bmask_x(bad, 4, out, 1));
    assert(!nvmtl_spirv_bmask_x(bad, sizeof bad, out, 1));
    bad[5] = 71; assert(!nvmtl_spirv_bmask_x(bad, sizeof bad, out, 1));
    bad[5] = (4u<<16)|71u; bad[8] = 300; assert(!nvmtl_spirv_bmask_x(bad, sizeof bad, out, 1));
    ++cases;
}
static void check_parser_boundaries(void) {
    // Independent expected masks, including descriptor decorations in either order.
    const uint32_t bindings[] = {0, 31, 32, 63, 64, 127, 128, 159, 160, 191,
                                 192, 199, 200, 479, 480, 607, 608, 640, UINT32_MAX};
    for (unsigned set = 0; set <= 3; ++set)
        for (unsigned j = 0; j < sizeof bindings / sizeof *bindings; ++j)
            for (unsigned reverse = 0; reverse < 2; ++reverse) {
                uint32_t b = bindings[j];
                uint32_t module[] = {0x07230203, 0x10000, 0, 2, 0,
                    (4u << 16) | 71u, 1, reverse ? 33u : 34u, reverse ? b : set,
                    (4u << 16) | 71u, 1, reverse ? 34u : 33u, reverse ? set : b};
                for (int skip = 0; skip <= 1; ++skip) {
                    uint64_t actual[4], expected[4] = {0};
                    int excluded = set == 2 || (skip && set == 3);
                    int in_stage = b < NVMTL_ADDRESS_BINDING || b == NVMTL_ADDRESS_BINDING_NUMBER;
                    int storage = b >= NVMTL_STEX_BINDING_NUMBER && b < NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX;
                    int ok = excluded || in_stage || storage;
                    assert(nvmtl_spirv_bmask_x(module, sizeof module, actual, skip) == ok);
                    if (ok) {
                        if (!excluded && in_stage) {
                            uint32_t slot = b == NVMTL_ADDRESS_BINDING_NUMBER ? NVMTL_ADDRESS_BINDING : b;
                            expected[slot >> 6] |= 1ull << (slot & 63);
                        }
                        assert(!memcmp(actual, expected, sizeof actual));
                    }
                    ++cases;
                }
            }
    uint32_t invalid_bound[] = {0x07230203, 0x10000, 0, 0, 0};
    uint64_t out[4];
    assert(!nvmtl_spirv_bmask_x(invalid_bound, sizeof invalid_bound, out, 1));
    invalid_bound[3] = 1u << 24;
    assert(!nvmtl_spirv_bmask_x(invalid_bound, sizeof invalid_bound, out, 1));
    ++cases;
}
int main(int argc, char **argv) {
    check_snapshot_cases(); check_bad_inputs(); check_parser_boundaries();
    unsigned files = 0, improved = 0, mismatches = 0;
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb"); assert(f);
        assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n >= 20 && n % 4 == 0);
        rewind(f); void *data = malloc((size_t)n); assert(data);
        assert(fread(data, 1, (size_t)n, f) == (size_t)n); fclose(f);
        uint64_t old[4], legacy[4], next[4];
        int a = nvmtl_spirv_bmask_x(data, (size_t)n, old, 0);
        int b = nvmtl_spirv_bmask(data, (size_t)n, legacy);
        int c = nvmtl_spirv_bmask_x(data, (size_t)n, next, 1);
        if (a != b || memcmp(old, legacy, sizeof old)) ++mismatches;
        if (!a && c) ++improved;
        if (a) assert(c && !memcmp(old, next, sizeof old));
        free(data); ++files;
    }
    assert(!mismatches);
    printf("cases=%u files=%u improved=%u legacy_mismatches=%u mode_enabled=%d synthetic_first_write_count=%u",
           cases, files, improved, mismatches, nvmtl_desc_mask_on(), nvmtl_desc_mask_on() ? 4u : NVMTL_STEX_BASE);
    return 0;
}
