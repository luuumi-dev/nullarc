/* SPDX-License-Identifier: MIT
 * Load the RADV ICD directly: mock inspection and ACO compilation only.
 * No Vulkan loader installation and no physical GPU connection required.
 */
#include <vulkan/vulkan.h>
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    assert(argc == 4 && getenv("RADV_DARWIN_MOCK") && !strcmp(getenv("RADV_DARWIN_MOCK"), "1"));
    unsigned expected = (unsigned)atoi(argv[2]);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    assert(gpa);
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gpa(NULL, "vkCreateInstance");
    const VkApplicationInfo app = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion=VK_API_VERSION_1_2};
    const VkInstanceCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo=&app};
    VkInstance instance; assert(create(&ci, NULL, &instance) == VK_SUCCESS);
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gpa(instance, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 properties = (PFN_vkGetPhysicalDeviceProperties2)gpa(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gpa(instance, "vkDestroyInstance");
    uint32_t count=1; VkPhysicalDevice device;
    assert(enumerate(instance, &count, &device) == VK_SUCCESS && count == 1);
    VkPhysicalDevicePCIBusInfoPropertiesEXT pci = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT};
    VkPhysicalDeviceShaderCoreProperties2AMD core2 = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD, .pNext=&pci};
    VkPhysicalDeviceShaderCorePropertiesAMD core = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD, .pNext=&core2};
    VkPhysicalDeviceProperties2 p = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext=&core};
    properties(device, &p);
    assert(p.properties.vendorID == 0x1002 && p.properties.deviceID == 0x1900);
    assert(core2.activeComputeUnitCount == expected);
    assert(core.shaderEngineCount == 1 && core.shaderArraysPerEngineCount == 2);
    assert(strstr(p.properties.deviceName, expected == 8 ? "760M" : "780M"));
    printf("[PASS] %s: %u CUs, SE=%u SA=%u CU/SA=%u (mock, no execution)\n", p.properties.deviceName,
           core2.activeComputeUnitCount, core.shaderEngineCount, core.shaderArraysPerEngineCount, core.computeUnitsPerShaderArray);
    // Compile a real storage-buffer shader using RADV/ACO, without dispatching.
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)gpa(instance, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr dgpa = (PFN_vkGetDeviceProcAddr)gpa(instance, "vkGetDeviceProcAddr");
    float priority = 1;
    VkDeviceQueueCreateInfo qi = {.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex=0, .queueCount=1, .pQueuePriorities=&priority};
    VkDeviceCreateInfo di = {.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount=1, .pQueueCreateInfos=&qi};
    VkDevice logical; assert(create_device(device, &di, NULL, &logical) == VK_SUCCESS);
    #define FN(type, name) type name = (type)dgpa(logical, #name)
    FN(PFN_vkCreateShaderModule, vkCreateShaderModule);
    FN(PFN_vkCreateDescriptorSetLayout, vkCreateDescriptorSetLayout);
    FN(PFN_vkCreatePipelineLayout, vkCreatePipelineLayout);
    FN(PFN_vkCreateComputePipelines, vkCreateComputePipelines);
    FN(PFN_vkDestroyPipeline, vkDestroyPipeline);
    FN(PFN_vkDestroyPipelineLayout, vkDestroyPipelineLayout);
    FN(PFN_vkDestroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
    FN(PFN_vkDestroyShaderModule, vkDestroyShaderModule);
    FN(PFN_vkDestroyDevice, vkDestroyDevice);
    #undef FN
    FILE *f = fopen(argv[3], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long bytes = ftell(f); assert(bytes > 0 && !(bytes % 4));
    rewind(f); uint32_t *code = malloc((size_t)bytes); assert(code);
    assert(fread(code, (size_t)bytes, 1, f) == 1); fclose(f);
    VkShaderModuleCreateInfo mi = {.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize=(size_t)bytes, .pCode=code};
    VkShaderModule module; assert(vkCreateShaderModule(logical, &mi, NULL, &module) == VK_SUCCESS); free(code);
    VkDescriptorSetLayoutBinding binding = {.binding=0, .descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount=1, .stageFlags=VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si = {.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount=1, .pBindings=&binding};
    VkDescriptorSetLayout set; assert(vkCreateDescriptorSetLayout(logical, &si, NULL, &set) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo li = {.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount=1, .pSetLayouts=&set};
    VkPipelineLayout layout; assert(vkCreatePipelineLayout(logical, &li, NULL, &layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pi = {.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage=VK_SHADER_STAGE_COMPUTE_BIT, .module=module, .pName="main"}, .layout=layout};
    VkPipeline pipeline; assert(vkCreateComputePipelines(logical, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS);
    puts("[PASS] gfx1103 ACO compute pipeline compiled; no commands submitted");
    vkDestroyPipeline(logical, pipeline, NULL); vkDestroyPipelineLayout(logical, layout, NULL);
    vkDestroyDescriptorSetLayout(logical, set, NULL); vkDestroyShaderModule(logical, module, NULL);
    vkDestroyDevice(logical, NULL);
    destroy(instance, NULL); dlclose(lib);
}
