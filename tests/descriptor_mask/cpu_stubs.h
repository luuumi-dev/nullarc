/* Vulkan calls are CPU-only test doubles. The descriptor writer is production code. */
#define NVMTL_MAX_POOLS 64
#define NVMTL_TABLES_PER_PAGE 256
static VkDevice g_dev;
static VkDescriptorSetLayout g_setLayout;
static VkDescriptorPool g_descPools[NVMTL_MAX_POOLS];
static unsigned g_ndescPools = 1;
static pthread_mutex_t g_desc_lock = PTHREAD_MUTEX_INITIALIZER;
static nvk_buffer g_dummyBuf;
static nvk_image g_dummyImg, g_dummyStex = { .view = (void *)(uintptr_t)0x222 };
static VkSampler g_dummySampler;
static void *g_dummyTexView;
static int g_mutable = 1, g_mutable_stex = 1;
enum { NVMTL_E78_H3 = 1, E78C_WRITES, E78C_SETNEW, E78C_SETREUSE };
typedef struct nvmtl_address_page { nvk_buffer buffer; struct nvmtl_address_page *next; } nvmtl_address_page;
static unsigned calls, writes, copies, allocated;
static int fail_alloc, fail_address, h3 = 1, ubo = 1;
static uint32_t observed_bindings[1024];
static VkDescriptorType observed_types[1024];
static uint64_t observed_address_offset;
static void reset_observation(void) {
    calls = writes = copies = 0;
    memset(observed_bindings, 0, sizeof observed_bindings);
    memset(observed_types, 0, sizeof observed_types);
    observed_address_offset = UINT64_MAX;
}
static int nvmtl_enc78_on(int bit) { (void)bit; return h3; }
static void nvmtl_enc78_count(int key, uint64_t value) { (void)key; (void)value; }
static int nvmtl_table_ubo(void) { return ubo; }
static int ensure_descriptors_locked(void) { return 0; }
static int nvmtl_desc_pool_open(void) { return -1; }
static VkResult pvkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *a, VkDescriptorSet *out) {
    (void)d;
    if (fail_alloc) return VK_ERROR_OUT_OF_POOL_MEMORY;
    for (uint32_t i = 0; i < a->descriptorSetCount; ++i) out[i] = (VkDescriptorSet)(uintptr_t)(0x1000 + ++allocated);
    return VK_SUCCESS;
}
static VkResult pvkFreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) {
    (void)d; (void)p; (void)n; (void)s; return VK_SUCCESS;
}
static void pvkUpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)d; ++calls; writes += nw; copies += nc;
    assert(nw + nc <= 1024);
    for (uint32_t i = 0; i < nw; ++i) {
        observed_bindings[i] = w[i].dstBinding; observed_types[i] = w[i].descriptorType;
        if (w[i].dstBinding == NVMTL_ADDRESS_BINDING_NUMBER) { assert(w[i].pBufferInfo); observed_address_offset = w[i].pBufferInfo->offset; }
    }
    for (uint32_t i = 0; i < nc; ++i) { assert(c[i].srcBinding == c[i].dstBinding); observed_bindings[nw + i] = c[i].dstBinding; }
}
static int nvmtl_address_snapshot(nvk_cmdbuf *c, uint32_t which, VkDescriptorBufferInfo *out) {
    (void)which;
    if (fail_address) return -1;
    *out = (VkDescriptorBufferInfo){ (VkBuffer)(uintptr_t)0x111, 256u * c->address_count++, 256 };
    return 0;
}
