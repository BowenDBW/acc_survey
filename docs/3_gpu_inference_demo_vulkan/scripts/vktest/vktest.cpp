// vktest.cpp — 最小 Vulkan compute 管线诊断工具
// 目的: 定位 Adreno 740 上 llama.cpp createComputePipeline:ErrorUnknown 的根因
//   依次测试四个变体, 分别报告 createComputePipeline 是否成功 + 实际执行是否可读回:
//     trivial       基线(纯 FP32, 无扩展)
//     subgroup64    + VK_EXT_subgroup_size_control requiredSubgroupSize=64
//     bit16         + GL_EXT_shader_16bit_storage (f16vec2 写入)
//     controlflow   + GL_EXT_control_flow_attributes ([[unroll]], SPIR-V 1.6)
//
// 交叉编译: NDK clang -std=c++17 -lvulkan
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---- SPIR-V 数组, 由 build_vktest.sh 从 .comp 生成 ----
// 注意: #include 必须独占一行开头 (预处理指令要求), 所以换行书写
static const uint32_t trivial_spv[] = {
#include "trivial.spv.h"
};
static const uint32_t subgroup64_spv[] = {
#include "subgroup64.spv.h"
};
static const uint32_t bit16_spv[] = {
#include "bit16.spv.h"
};
static const uint32_t controlflow_spv[] = {
#include "controlflow.spv.h"
};
// llama.cpp 编译出的真实崩溃 shader (quantize_q8_1_x4 USE_SUBGROUPS 变体)
static const uint32_t quantize_x4_subgroup_spv[] = {
#include "quantize_x4_subgroup.spv.h"
};
static const uint32_t i8vec4_spv[] = {
#include "i8vec4.spv.h"
};
static const uint32_t clustered_spv[] = {
#include "clustered.spv.h"
};
// llama.cpp 实际崩溃的 shader: quantize_q8_1_x4 非 subgroup 变体 (从 build 产物原样拷贝)
// 真实报错: "Compute pipeline creation failed for quantize_q8_1_x4" VK_ERROR_UNKNOWN
static const uint32_t quantize_x4_nonsubgroup_spv[] = {
#include "quantize_x4_nonsubgroup.spv.h"
};
// 二分定位: 忠实复刻 vs 位运算打包 (无 Int8)
static const uint32_t qx4_min_spv[] = {
#include "qx4_min.spv.h"
};
static const uint32_t qx4_min_noi8_spv[] = {
#include "qx4_min_noi8.spv.h"
};
// 真实崩溃 shader 原样 (mul_mat_vec_q4_k_f32_f32, 17:27 build)
// 注意: 设备支持 subgroup arithmetic (basic vote arithmetic ballot shuffle quad),
//       所以 llama.cpp 实际加载的是 SUBGROUP_NO_SHMEM 变体 (reduc16=SUBGROUP, w=0),
//       mvq4k_real.spv 是 base/SHMEM 变体 — 不是真正崩溃的那个!
static const uint32_t mvq4k_real_spv[] = {
#include "mvq4k_real.spv.h"
};
// llama.cpp 在真机实际使用的变体: mul_mat_vec_q4_k_f32_f32_subgroup_no_shmem.spv
static const uint32_t mvq4k_nsm_spv[] = {
#include "mvq4k_nsm.spv.h"
};
// 对照组: Q3_K 同款 subgroup_no_shmem (llama.cpp 创建 Q4_K 之前 Q3_K 成功, 但 Q4_K 崩溃)
static const uint32_t mvq3k_nsm_spv[] = {
#include "mvq3k_nsm.spv.h"
};
// HYBRID 变体 (USE_SUBGROUP_ADD, w=1 DMMV_WG_SIZE_LARGE)
static const uint32_t mvq4k_hb_spv[] = {
#include "mvq4k_hb.spv.h"
};
// 最小 subgroupAdd 复现 (无 SSBO 结构体, 无 8/16bit 存储): 驱动是否连基础 subgroupAdd 都拒绝
static const uint32_t sgadd_min_spv[] = {
#include "sgadd_min.spv.h"
};
// FLOAT subgroupAdd (OpGroupNonUniformFAdd): 假设 Adreno 只拒绝 float subgroup 归约
static const uint32_t sgadd_fadd_min_spv[] = {
#include "sgadd_fadd_min.spv.h"
};
// mul_mat_vec_q4_k 崩溃根因二分: 16 位 SSBO 加载 与 8 位 struct 成员 的组合
static const uint32_t bisect_f16_from_8bit_struct_spv[] = {
#include "bisect_f16_from_8bit_struct.spv.h"
};
static const uint32_t bisect_u16_from_8bit_struct_spv[] = {
#include "bisect_u16_from_8bit_struct.spv.h"
};
static const uint32_t bisect_f16_no8bit_spv[] = {
#include "bisect_f16_no8bit.spv.h"
};
static const uint32_t bisect_decl_only_8bit_spv[] = {
#include "bisect_decl_only_8bit.spv.h"
};

struct TestCase {
    const char* name;
    const uint32_t* spv;
    size_t sz;
    bool use_subgroup_size_control; // requiredSubgroupSize=64
    bool full_subgroups;            // VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT
    bool creation_only;             // 只测管线创建 (quantize 输出布局不同, 不做 dispatch 读回)
    int expected_data0;             // data[0] 期望值 (-1 = 不校验)
    const std::vector<uint32_t>* dyn; // 若非空, 用动态 SPIR-V (打补丁用)
    // llama.cpp 传给 mul_mat_vec 的 specialization constants:
    //   {wg_size_subgroup16, rm_kq, i+1} -> constant_id 0=BLOCK_SIZE/local_size_x,
    //   constant_id 1=NUM_ROWS, constant_id 2=NUM_COLS
    bool has_spec;                  // 是否传 spec constants
    uint32_t spec[3];
    // 复刻 llama.cpp createComputePipeline 差异项 (见 VKDBG 实机输出:
    //   caps=64(eCaptureStatisticsKHR) push=52 spec={64,2,1} layout=dsl+pcr)
    int  layout_bindings;           // 0=空 layout, 12=llama.cpp device->dsl
    int  pcr_size;                  // 0=无 push constant range, 52=llama.cpp sizeof(vk_mat_vec_push_constants)
    bool capture_stats;             // VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR
};

// ---- 复刻 llama.cpp 的 SPIR-V 补丁: 注入 SPV_KHR_float_controls RoundingModeRTE FP16 ----
// 对应 ggml-vulkan.cpp ggml_vk_create_pipeline_func 中 device->float_controls_rte_fp16 分支
static std::vector<uint32_t> patch_for_rte(const uint32_t* src, size_t word_count) {
    std::vector<uint32_t> spirv(src, src + word_count);
    size_t pos = 5; // skip header
    size_t cap_insert_pos = pos, ext_insert_pos = pos, exec_insert_pos = pos;
    uint32_t entry_point_id = 0;
    while (pos < spirv.size()) {
        uint32_t opcode = spirv[pos] & 0xffffu;
        uint32_t len = spirv[pos] >> 16;
        if (len == 0) break;
        if (opcode == 17 /*OpCapability*/) { cap_insert_pos = pos + len; ext_insert_pos = pos + len; }
        else if (opcode == 10 /*OpExtension*/) { ext_insert_pos = pos + len; }
        else if (opcode == 15 /*OpEntryPoint*/) { entry_point_id = spirv[pos + 2]; exec_insert_pos = pos + len; }
        else if (opcode == 16 /*OpExecutionMode*/ || opcode == 331 /*OpExecutionModeId*/) { exec_insert_pos = pos + len; }
        else if (entry_point_id != 0) break;
        pos += len;
    }
    if (entry_point_id == 0) return spirv;
    // OpExecutionMode %entrypoint RoundingModeRTE 16
    uint32_t exec_mode[] = { (4u << 16) | 16, entry_point_id, 4462 /*RoundingModeRTE*/, 16 };
    spirv.insert(spirv.begin() + exec_insert_pos, exec_mode, exec_mode + 4);
    // OpExtension "SPV_KHR_float_controls"
    const char ext_str[] = "SPV_KHR_float_controls";
    size_t ext_str_words = (sizeof(ext_str) + 3) / 4;
    std::vector<uint32_t> extension(1 + ext_str_words, 0);
    extension[0] = ((uint32_t)(1 + ext_str_words) << 16) | 10;
    memcpy(&extension[1], ext_str, sizeof(ext_str));
    spirv.insert(spirv.begin() + ext_insert_pos, extension.begin(), extension.end());
    // OpCapability RoundingModeRTE
    uint32_t capability[] = { (2u << 16) | 17, 4467 /*CapabilityRoundingModeRTE*/ };
    spirv.insert(spirv.begin() + cap_insert_pos, capability, capability + 2);
    return spirv;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0); // 禁用缓冲, 崩溃前输出不丢失
    // ---- Instance ----
    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        printf("FAIL vkCreateInstance\n"); return 1;
    }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) { printf("FAIL no physical device\n"); return 1; }
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(instance, &n, phys.data());
    VkPhysicalDevice dev = phys[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    VkPhysicalDeviceSubgroupProperties sp{};
    sp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceVulkan12Properties v12{};
    v12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    sp.pNext = &v12;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sp;
    vkGetPhysicalDeviceProperties2(dev, &p2);
    printf("GPU: %s\n  apiVersion: %u.%u.%u | driverVersion: 0x%x\n  maxPushConstantsSize: %u | subgroupSize: %u\n",
           props.deviceName,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
           props.driverVersion, props.limits.maxPushConstantsSize, sp.subgroupSize);
    printf("  vk12: shaderRoundingModeRTEFloat16=%u | shaderDenormPreserveFloat16=%u  (llama.cpp 据此给每个 shader 打补丁)\n",
           v12.shaderRoundingModeRTEFloat16, v12.shaderDenormPreserveFloat16);
    const char* op_names[] = { "basic", "vote", "arithmetic", "ballot", "shuffle", "quad", "clustered" };
    printf("  subgroupSupportedOperations:");
    for (int b = 0; b < 7; b++) {
        if (sp.supportedOperations & (VkSubgroupFeatureFlagBits)(1u << b)) printf(" %s", op_names[b]);
    }
    printf("\n  (llama.cpp 据此开 clustered shader: %s)\n",
           (sp.supportedOperations & VK_SUBGROUP_FEATURE_CLUSTERED_BIT) ? "会开" : "不开");

    // ---- 队列族 ----
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qp.data());
    int qfam = -1;
    for (uint32_t i = 0; i < qn; i++) {
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = (int)i; break; }
    }
    if (qfam < 0) { printf("FAIL no compute queue\n"); return 1; }

    // ---- 特性(尽量全开, 与 llama.cpp 一致) ----
    VkPhysicalDeviceSubgroupSizeControlFeatures ssc{};
    ssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    ssc.subgroupSizeControl = VK_TRUE;
    VkPhysicalDevice16BitStorageFeatures b16{};
    b16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    b16.storageBuffer16BitAccess = VK_TRUE;
    b16.uniformAndStorageBuffer16BitAccess = VK_TRUE;
    // Vulkan 1.2 core feature: shaderInt8. llama.cpp 从不显式置 VK_TRUE,
    // 只把查询结果原样回传; 这里显式开启验证 quantize(OpCapability Int8) 是否依赖它
    VkPhysicalDeviceVulkan12Features v12f{};
    v12f.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    b16.pNext = &v12f;
    v12f.pNext = &ssc;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &b16;
    vkGetPhysicalDeviceFeatures2(dev, &f2);
    printf("  vk12features: shaderInt8=%u shaderFloat16=%u\n",
           v12f.shaderInt8, v12f.shaderFloat16);
    v12f.shaderInt8 = VK_TRUE;   // ← 显式开启 Int8 (诊断: 若修复 quantize 则根因确认)

    // ---- Device ----
    const char* ext_names[] = {
        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
        // llama.cpp 若检测到 VK_KHR_pipeline_executable_properties 会传
        // VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR (caps=64)。需启用扩展才能用该 flag。
        VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
    };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = ext_names;
    dci.pNext = &f2;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(dev, &dci, nullptr, &device) != VK_SUCCESS) {
        printf("FAIL vkCreateDevice\n"); return 1;
    }
    printf("createDevice OK (compute queue %d)\n", qfam);
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, (uint32_t)qfam, 0, &queue);

    // ---- 缓冲区: storage(写入, 4KB) + 第二个 storage(bit16 测试用) ----
    const VkDeviceSize BUF = 4096;
    auto make_buf = [&](VkBuffer& b, VkDeviceMemory& m, VkBufferUsageFlags use) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = BUF; bci.usage = use; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bci, nullptr, &b);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, b, &mr);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(dev, &mp);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = mr.size;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
                mai.memoryTypeIndex = i; break;
            }
        }
        vkAllocateMemory(device, &mai, nullptr, &m);
        vkBindBufferMemory(device, b, m, 0);
    };
    VkBuffer sbuf0, sbuf1; VkDeviceMemory smem0, smem1;
    make_buf(sbuf0, smem0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    make_buf(sbuf1, smem1, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);

    VkCommandPool cpool;
    VkCommandPoolCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci.queueFamilyIndex = (uint32_t)qfam;
    vkCreateCommandPool(device, &cpci, nullptr, &cpool);

    auto run_test = [&](const TestCase& t) {
        printf("\n--- %s ---\n", t.name);
        const uint32_t* pcode = t.dyn ? t.dyn->data() : t.spv;
        size_t psz = t.dyn ? t.dyn->size() * sizeof(uint32_t) : t.sz;
        VkShaderModule mod = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = psz; sm.pCode = pcode;
        VkResult r = vkCreateShaderModule(device, &sm, nullptr, &mod);
        printf("  [step] createShaderModule: %s\n", r == VK_SUCCESS ? "OK" : "FAIL");
        if (r != VK_SUCCESS) { printf("FAIL createShaderModule (%d)\n", r); return false; }

        VkPipelineLayout pl = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkDescriptorSetLayout full_dsl = VK_NULL_HANDLE;
        VkPushConstantRange full_pcr{};
        VkDescriptorSetLayoutBinding full_bind[12]{};
        if (t.layout_bindings > 0) {
            // 复刻 llama.cpp device->dsl: MAX_PARAMETER_COUNT=12 个 storage buffer binding
            for (int b = 0; b < t.layout_bindings; b++) {
                full_bind[b].binding = (uint32_t)b;
                full_bind[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                full_bind[b].descriptorCount = 1;
                full_bind[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dsli{};
            dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dsli.bindingCount = (uint32_t)t.layout_bindings; dsli.pBindings = full_bind;
            vkCreateDescriptorSetLayout(device, &dsli, nullptr, &full_dsl);
            pli.setLayoutCount = 1; pli.pSetLayouts = &full_dsl;
        }
        if (t.pcr_size > 0) {
            // 复刻 llama.cpp pcr: [0, push_constant_size)
            full_pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            full_pcr.offset = 0;
            full_pcr.size = (uint32_t)t.pcr_size;
            pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &full_pcr;
        }
        r = vkCreatePipelineLayout(device, &pli, nullptr, &pl);
        printf("  [step] createPipelineLayout: %s (bind=%d pcr=%d)\n", r == VK_SUCCESS ? "OK" : "FAIL", t.layout_bindings, t.pcr_size);
        if (r != VK_SUCCESS) { printf("FAIL createPipelineLayout (%d)\n", r); return false; }

        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        if (t.full_subgroups) {
            stage.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT_EXT;
        }
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT req{};
        if (t.use_subgroup_size_control) {
            req.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT;
            req.requiredSubgroupSize = 64;
            stage.pNext = &req;
        }
        // llama.cpp 的 specialization constants (见 ggml-vulkan.cpp line 5311):
        //   {wg_size_subgroup16, rm_kq, i+1} 对应 constant_id 0/1/2
        VkSpecializationMapEntry smap[3]{};
        VkSpecializationInfo sinfo{};
        if (t.has_spec) {
            for (int k = 0; k < 3; k++) {
                smap[k].constantID = k;
                smap[k].offset = k * sizeof(uint32_t);
                smap[k].size = sizeof(uint32_t);
            }
            sinfo.mapEntryCount = 3;
            sinfo.pMapEntries = smap;
            sinfo.dataSize = 3 * sizeof(uint32_t);
            sinfo.pData = t.spec;
            stage.pSpecializationInfo = &sinfo;
        }
        VkComputePipelineCreateInfo cp{};
        cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cp.stage = stage;
        cp.layout = pl;
        if (t.capture_stats) cp.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        if (t.has_spec) printf("  [step] spec={%u,%u,%u} req64=%d full=%d caps=%d\n", t.spec[0], t.spec[1], t.spec[2], t.use_subgroup_size_control, t.full_subgroups, t.capture_stats);
        printf("  [step] createComputePipelines...\n");
        fflush(stdout);
        VkPipeline pipe = VK_NULL_HANDLE;
        r = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe);
        printf("  [step] createComputePipelines returned: %d\n", (int)r);
        fflush(stdout);
        if (r != VK_SUCCESS) {
            printf("FAIL createComputePipeline (%d)  <-- 崩溃点同款!\n", r);
            return false;
        }
        printf("createComputePipeline OK\n");
        if (t.creation_only) {
            vkDestroyPipeline(device, pipe, nullptr);
            vkDestroyPipelineLayout(device, pl, nullptr);
            if (full_dsl) vkDestroyDescriptorSetLayout(device, full_dsl, nullptr);
            vkDestroyShaderModule(device, mod, nullptr);
            return true;
        }

        // ---- 实际执行 + 读回 ----
        VkCommandBuffer cb;
        VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cai, &cb);
        VkCommandBufferBeginInfo bbi{}; bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bbi);
        VkDescriptorSetLayoutBinding dslb[2]{};
        dslb[0].binding = 0; dslb[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dslb[0].descriptorCount = 1; dslb[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        dslb[1].binding = 1; dslb[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; dslb[1].descriptorCount = 1; dslb[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dsli{}; dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsli.bindingCount = 2; dsli.pBindings = dslb;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device, &dsli, nullptr, &dsl);
        VkPipelineLayout pl2 = VK_NULL_HANDLE;
        VkPipelineLayoutCreateInfo pli2{}; pli2.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli2.setLayoutCount = 1; pli2.pSetLayouts = &dsl;
        vkCreatePipelineLayout(device, &pli2, nullptr, &pl2);
        VkPipeline pipe2 = VK_NULL_HANDLE;
        VkComputePipelineCreateInfo cp2 = cp; cp2.layout = pl2;
        vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp2, nullptr, &pipe2);

        VkDescriptorPoolCreateInfo dpci{}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = 1;
        dpci.poolSizeCount = 2;
        VkDescriptorPoolSize dps[2]{ {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2} };
        dpci.pPoolSizes = dps;
        VkDescriptorPool dpool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(device, &dpci, nullptr, &dpool);
        VkDescriptorSet dset = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl;
        vkAllocateDescriptorSets(device, &dsai, &dset);
        VkDescriptorBufferInfo dbi0{ sbuf0, 0, BUF }, dbi1{ sbuf1, 0, BUF };
        VkWriteDescriptorSet wds[2]{};
        wds[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wds[0].dstSet = dset; wds[0].dstBinding = 0; wds[0].descriptorCount = 1; wds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wds[0].pBufferInfo = &dbi0;
        wds[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wds[1].dstSet = dset; wds[1].dstBinding = 1; wds[1].descriptorCount = 1; wds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wds[1].pBufferInfo = &dbi1;
        vkUpdateDescriptorSets(device, 2, wds, 0, nullptr);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe2);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl2, 0, 1, &dset, 0, nullptr);
        vkCmdDispatch(cb, 1, 1, 1);
        vkEndCommandBuffer(cb);

        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r != VK_SUCCESS) { printf("FAIL queueSubmit (%d)\n", r); return false; }
        vkQueueWaitIdle(queue);

        void* p = nullptr;
        vkMapMemory(device, smem0, 0, BUF, 0, &p);
        uint32_t* v = (uint32_t*)p;
        for (int i = 0; i < 8; i++) {
            printf("  data[%d] = %u\n", i, v[i]);
        }
        // 校验 buffer0 单调递增, 说明 GPU 真正跑起来了
        bool monotonic = true;
        for (int i = 1; i < 64; i++) if (v[i] <= v[i-1] || v[i] == 0) { monotonic = false; break; }
        vkUnmapMemory(device, smem0);
        printf("  GPU 实际执行: %s\n", monotonic ? "OK (数据非零且递增, 计算真实发生)" : "数据异常");

        vkDestroyDescriptorPool(device, dpool, nullptr);
        vkDestroyDescriptorSetLayout(device, dsl, nullptr);
        vkDestroyPipeline(device, pipe2, nullptr);
        vkDestroyPipelineLayout(device, pl2, nullptr);
        vkFreeCommandBuffers(device, cpool, 1, &cb);
        vkDestroyPipeline(device, pipe, nullptr);
        vkDestroyPipelineLayout(device, pl, nullptr);
        if (full_dsl) vkDestroyDescriptorSetLayout(device, full_dsl, nullptr);
        vkDestroyShaderModule(device, mod, nullptr);
        return true;
    };

    // 复刻 llama.cpp 对 shader 的 RTE 补丁 (驱动报告 shaderRoundingModeRTEFloat16=1, llama.cpp 会注入)
    std::vector<uint32_t> patched_rte = patch_for_rte(trivial_spv, sizeof(trivial_spv) / sizeof(uint32_t));
    size_t quant_sz = sizeof(quantize_x4_subgroup_spv);
    std::vector<uint32_t> quant_raw(quantize_x4_subgroup_spv, quantize_x4_subgroup_spv + quant_sz / sizeof(uint32_t));
    std::vector<uint32_t> quant_patched = patch_for_rte(quantize_x4_subgroup_spv, quant_sz / sizeof(uint32_t));
    std::vector<uint32_t> mvq4k_patched = patch_for_rte(mvq4k_real_spv, sizeof(mvq4k_real_spv) / sizeof(uint32_t));
    std::vector<uint32_t> nsm_patched = patch_for_rte(mvq4k_nsm_spv, sizeof(mvq4k_nsm_spv) / sizeof(uint32_t));
    std::vector<uint32_t> hb_patched  = patch_for_rte(mvq4k_hb_spv,  sizeof(mvq4k_hb_spv)  / sizeof(uint32_t));
    std::vector<uint32_t> q3k_patched = patch_for_rte(mvq3k_nsm_spv, sizeof(mvq3k_nsm_spv) / sizeof(uint32_t));

    TestCase tests[] = {
        // name, spv, sz, subgroup_ctl, full_subgroups, creation_only, expected, dyn
        // 修复方案验证放最前: 强制用 base/SHMEM 变体 + llama.cpp 的 spec {64,1,1}
        { "BASE +RTE +spec{64,1,1}",            nullptr, 0,                            false, false, true, -1, &mvq4k_patched, true, {64,1,1} },
        { "BASE +RTE +spec{64,1,1} +req64+full", nullptr, 0,                           true,  true,  true, -1, &mvq4k_patched, true, {64,1,1} },
        { "BASE +RTE +spec{256,1,1} +req64+full", nullptr, 0,                          true,  true,  true, -1, &mvq4k_patched, true, {256,1,1} },
        // === llama.cpp 实机调用完整复刻 (VKDBG: caps=64 push=52 spec={64,2,1}) ===
        // 已确认: capture_stats 不是触发点; full_layout(12 bind + 52B pcr) + NUM_COLS=1 才触发 -13。
        // 继续二分: bindings 数 / pcr 尺寸 / 交互
        { "BASE +RTE +spec{64,2,1} +bind12",      nullptr, 0,                            false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12,  0, false },
        { "BASE +RTE +spec{64,2,1} +pcr52",       nullptr, 0,                            false, false, true, -1, &mvq4k_patched, true, {64,2,1},  0, 52, false },
        { "BASE +RTE +spec{64,2,1} +bind12+pcr32", nullptr, 0,                           false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12, 32, false },
        { "BASE +RTE +spec{64,2,1} +bind12+pcr48", nullptr, 0,                           false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12, 48, false },
        { "BASE +RTE +spec{64,2,1} +bind12+pcr52", nullptr, 0,                           false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12, 52, false },
        { "BASE +RTE +spec{64,2,1} +bind12+pcr64", nullptr, 0,                           false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12, 64, false },
        { "BASE +RTE +spec{64,2,1} +bind5+pcr52",  nullptr, 0,                           false, false, true, -1, &mvq4k_patched, true, {64,2,1},  5, 52, false },
        { "BASE +RTE +spec{64,2,1} +bind12+pcr52+caps", nullptr, 0,                     false, false, true, -1, &mvq4k_patched, true, {64,2,1}, 12, 52, true },
        { "BASE +RTE +spec{64,1,1} +bind12+pcr52+caps", nullptr, 0,                     false, false, true, -1, &mvq4k_patched, true, {64,1,1}, 12, 52, true },
        { "BASE +RTE +spec{64,2,2} +bind12+pcr52+caps", nullptr, 0,                     false, false, true, -1, &mvq4k_patched, true, {64,2,2}, 12, 52, true },
        // === 映射安全区: wg_size 与 NUM_COLS 的边界 (找 num_cols=1 的可用 wg_size) ===
        { "BASE +RTE +spec{128,2,1} +bind12+pcr52", nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {128,2,1}, 12, 52, false },
        { "BASE +RTE +spec{256,2,1} +bind12+pcr52", nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {256,2,1}, 12, 52, false },
        { "BASE +RTE +spec{64,2,3} +bind12+pcr52",  nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {64,2,3}, 12, 52, false },
        { "BASE +RTE +spec{64,2,8} +bind12+pcr52",  nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {64,2,8}, 12, 52, false },
        { "BASE +RTE +spec{256,2,2} +bind12+pcr52", nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {256,2,2}, 12, 52, false },
        { "BASE +RTE +spec{256,2,3} +bind12+pcr52", nullptr, 0,                         false, false, true, -1, &mvq4k_patched, true, {256,2,3}, 12, 52, false },
        { "sgadd_IAdd (subgroupAdd uint)",   sgadd_min_spv,       sizeof(sgadd_min_spv),       false, false, false, -1, nullptr, false, {0,0,0} },
        { "sgadd_FAdd (subgroupAdd float)",  sgadd_fadd_min_spv,  sizeof(sgadd_fadd_min_spv),  false, false, false, -1, nullptr, false, {0,0,0} },
        { "trivial(baseline FP32)",         trivial_spv,     sizeof(trivial_spv),     false, false, false, -1, nullptr },
        { "subgroup64(size_control=64)",    subgroup64_spv,  sizeof(subgroup64_spv),  true,  false, false, -1, nullptr },
        { "bit16(16bit storage)",           bit16_spv,       sizeof(bit16_spv),       false, false, false, -1, nullptr },
        { "controlflow([[unroll]])",        controlflow_spv, sizeof(controlflow_spv), false, false, false, -1, nullptr },
        { "patched_rte(SPV_KHR_float_ctrl)", nullptr,        0,                       false, false, false, -1, &patched_rte },
        // mul_mat_vec_q4_k 崩溃根因二分 (放前面, 后面的 quantize/clustered 会段错误终止进程)
        { "REAL mvq4k raw",                  mvq4k_real_spv, sizeof(mvq4k_real_spv), false, false, true, -1, nullptr, false, {0,0,0} },
        { "REAL mvq4k +RTE",                 nullptr, 0,                            false, false, true, -1, &mvq4k_patched, false, {0,0,0} },
        // === 对照组放最前: Q3_K 同款 subgroup_no_shmem ===
        // llama.cpp 在 Q4_K 之前先创建 Q3_K 成功, 后创建 Q4_K 崩溃 -> 触发点在 Q4_K shader 内容
        { "q3k_nsm raw",                     mvq3k_nsm_spv, sizeof(mvq3k_nsm_spv), false, false, true, -1, nullptr, false, {0,0,0} },
        { "q3k_nsm +RTE +spec{64,1,1} +req64+full", nullptr, 0,                     true,  true,  true, -1, &q3k_patched, true, {64,1,1} },
        { "q3k_nsm +RTE +spec{256,1,1} +req64+full", nullptr, 0,                    true,  true,  true, -1, &q3k_patched, true, {256,1,1} },
        // === 真机实际变体 (subgroup_no_shmem): 完整复刻 llama.cpp 管线创建 ===
        // 设备支持 subgroup arithmetic + subgroup_size_control(full_subgroups), llama.cpp w=0 用此变体
        { "nsm raw",                         mvq4k_nsm_spv, sizeof(mvq4k_nsm_spv), false, false, true, -1, nullptr, false, {0,0,0} },
        { "nsm +RTE",                        nullptr, 0,                            false, false, true, -1, &nsm_patched, false, {0,0,0} },
        { "nsm +RTE +spec{64,1,1}",          nullptr, 0,                            false, false, true, -1, &nsm_patched, true, {64,1,1} },
        { "nsm +RTE +spec{64,1,1} +req64",   nullptr, 0,                            true,  false, true, -1, &nsm_patched, true, {64,1,1} },
        { "nsm +RTE +spec{64,1,1} +req64+full", nullptr, 0,                         true,  true,  true, -1, &nsm_patched, true, {64,1,1} },
        { "nsm +RTE +spec{256,1,1} +req64+full", nullptr, 0,                        true,  true,  true, -1, &nsm_patched, true, {256,1,1} },
        { "nsm +RTE +spec{64,1,2} +req64+full",  nullptr, 0,                        true,  true,  true, -1, &nsm_patched, true, {64,1,2} },
        { "nsm +RTE +spec{64,1,8} +req64+full",  nullptr, 0,                        true,  true,  true, -1, &nsm_patched, true, {64,1,8} },
        // HYBRID 变体 (w=1)
        { "hybrid +RTE +spec{256,1,1} +req64+full", nullptr, 0,                     true,  true,  true, -1, &hb_patched, true, {256,1,1} },
        { "bisect f16load from 8bit-struct", bisect_f16_from_8bit_struct_spv, sizeof(bisect_f16_from_8bit_struct_spv), false, false, true, -1, nullptr },
        { "bisect u16load from 8bit-struct", bisect_u16_from_8bit_struct_spv, sizeof(bisect_u16_from_8bit_struct_spv), false, false, true, -1, nullptr },
        { "bisect f16load no-8bit",          bisect_f16_no8bit_spv,          sizeof(bisect_f16_no8bit_spv),          false, false, true, -1, nullptr },
        { "bisect decl-only 8bit",           bisect_decl_only_8bit_spv,      sizeof(bisect_decl_only_8bit_spv),      false, false, true, -1, nullptr },
        // llama.cpp 真实崩溃的 shader: quantize_q8_1_x4 非subgroup (build 原样)
        { "quantize_x4_nonsubgroup 原样",    quantize_x4_nonsubgroup_spv, sizeof(quantize_x4_nonsubgroup_spv), false, false, true, -1, nullptr },
        // 二分: 忠实复刻(Int8) vs 位运算(无Int8)
        { "qx4_min(复刻, 含Int8)",           qx4_min_spv,        sizeof(qx4_min_spv),        false, false, true, -1, nullptr },
        { "qx4_min_noi8(位运算, 无Int8)",    qx4_min_noi8_spv,   sizeof(qx4_min_noi8_spv),   false, false, true, -1, nullptr },
        // 真实崩溃 shader 先测 (creation_only), 即使后面 clustered 段错误也能拿到结论
        { "quantize_x4_subgroup 原始",       nullptr,        0,                       false, false, true,  -1, &quant_raw },
        { "quantize_x4_subgroup +RTE",      nullptr,        0,                       false, false, true,  -1, &quant_patched },
        { "quantize_x4_subgroup +RTE+full", nullptr,        0,                       false, true,  true,  -1, &quant_patched },
        // 拆分 quantize 的独有特性 (放在最后, 已知 clustered 会段错误)
        { "i8vec4(8bit int storage)",       i8vec4_spv,      sizeof(i8vec4_spv),     false, false, false, -1, nullptr },
        { "clustered(subgroup_clustered)",  clustered_spv,   sizeof(clustered_spv),  false, false, false, -1, nullptr },
    };
    int pass = 0;
    for (auto& t : tests) {
        bool r = run_test(t);
        printf("==> %s: %s\n", t.name, r ? "通过" : "失败");
        if (r) pass++;
    }
    printf("\n汇总: %d/%zu 通过\n", pass, sizeof(tests)/sizeof(tests[0]));
    return pass == (int)(sizeof(tests)/sizeof(tests[0])) ? 0 : 2;
}
