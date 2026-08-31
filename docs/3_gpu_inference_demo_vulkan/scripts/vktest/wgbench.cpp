// wgbench.cpp — 衡量 Adreno 740 (或任何设备) 每个 compute dispatch 的固定开销
// 随 workgroup 数量的变化。复刻 llama.cpp rms_norm 的 dispatch 模式:
//   - workgroup 尺寸 512
//   - 可选: dispatch 之间的完整 pipeline+memory barrier (复刻 ggml_vk_sync_buffers)
// 输出: 每个 dispatch 的平均墙钟耗时 (µs), 用 vkQueueWaitIdle 收口。
//
// 构建 (与 vktest 相同):
//   glslc -fshader-stage=compute --target-env=vulkan1.2 shaders/wgbench.comp -o shaders/wgbench.spv
//   clang++ --target=aarch64-linux-android28 -std=c++17 -O2 -Ishaders wgbench.cpp -o wgbench -lvulkan -llog
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

static const uint32_t wgbench_spv[] = {
#include "wgbench.spv.h"
};

using ClockT = std::chrono::steady_clock;
static double now_ms() {
    return std::chrono::duration<double, std::milli>(ClockT::now().time_since_epoch()).count();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    VkInstance instance = VK_NULL_HANDLE;
    VkApplicationInfo ai{}; ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{}; ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo = &ai;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { printf("FAIL vkCreateInstance\n"); return 1; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    if (n == 0) { printf("FAIL no device\n"); return 1; }
    std::vector<VkPhysicalDevice> phys(n);
    vkEnumeratePhysicalDevices(instance, &n, phys.data());
    VkPhysicalDevice dev = phys[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    printf("GPU: %s\n", props.deviceName);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &qn, qp.data());
    int qfam = -1;
    for (uint32_t i = 0; i < qn; i++) if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfam = (int)i; break; }
    if (qfam < 0) { printf("FAIL no compute queue\n"); return 1; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueFamilyIndex = (uint32_t)qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{}; dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(dev, &dci, nullptr, &device) != VK_SUCCESS) { printf("FAIL vkCreateDevice\n"); return 1; }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, (uint32_t)qfam, 0, &queue);

    // storage buffer (4KB)
    VkBuffer buf; VkDeviceMemory mem;
    {
        VkBufferCreateInfo bci{}; bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bci.size = 4096; bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bci, nullptr, &buf);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, buf, &mr);
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(dev, &mp);
        VkMemoryAllocateInfo mai{}; mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; mai.allocationSize = mr.size;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) { mai.memoryTypeIndex = i; break; }
        vkAllocateMemory(device, &mai, nullptr, &mem);
        vkBindBufferMemory(device, buf, mem, 0);
    }

    // shader
    const size_t wgbench_spv_size = sizeof(wgbench_spv);
    VkShaderModule mod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo sm{}; sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; sm.codeSize = wgbench_spv_size; sm.pCode = wgbench_spv;
        if (vkCreateShaderModule(device, &sm, nullptr, &mod) != VK_SUCCESS) { printf("FAIL createShaderModule\n"); return 1; }
    }
    VkDescriptorSetLayoutBinding db{}; db.binding = 0; db.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; db.descriptorCount = 1; db.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    { VkDescriptorSetLayoutCreateInfo dsli{}; dsli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dsli.bindingCount = 1; dsli.pBindings = &db; vkCreateDescriptorSetLayout(device, &dsli, nullptr, &dsl); }
    VkPipelineLayout pl = VK_NULL_HANDLE;
    { VkPipelineLayoutCreateInfo pli{}; pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; vkCreatePipelineLayout(device, &pli, nullptr, &pl); }
    VkPipeline pipe = VK_NULL_HANDLE;
    {
        VkPipelineShaderStageCreateInfo st{}; st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st.stage = VK_SHADER_STAGE_COMPUTE_BIT; st.module = mod; st.pName = "main";
        VkComputePipelineCreateInfo cp{}; cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cp.stage = st; cp.layout = pl;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe) != VK_SUCCESS) { printf("FAIL createComputePipeline\n"); return 1; }
    }
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    { VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }; VkDescriptorPoolCreateInfo dpci{}; dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &ps; vkCreateDescriptorPool(device, &dpci, nullptr, &dpool); }
    VkDescriptorSet dset = VK_NULL_HANDLE;
    { VkDescriptorSetAllocateInfo dsai{}; dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.descriptorPool = dpool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &dsl; vkAllocateDescriptorSets(device, &dsai, &dset); }
    VkDescriptorBufferInfo dbi{ buf, 0, 4096 };
    { VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = dset; w.dstBinding = 0; w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &dbi; vkUpdateDescriptorSets(device, 1, &w, 0, nullptr); }

    VkCommandPool cpool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo cpci{}; cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci.queueFamilyIndex = (uint32_t)qfam; vkCreateCommandPool(device, &cpci, nullptr, &cpool); }

    // 复刻 ggml_vk_sync_buffers 的 barrier
    VkMemoryBarrier mb{}; mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    VkAccessFlags acc = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.srcAccessMask = acc; mb.dstAccessMask = acc;
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

    const uint32_t ITERS = 100;      // 每个配置重复 dispatch 次数 (在同一个 command buffer 里, 一次 submit)
    const int wgs_list[] = {1, 2, 3, 4, 6, 8, 12, 16, 32, 64, 128, 256};

    VkCommandBuffer cb = VK_NULL_HANDLE;
    { VkCommandBufferAllocateInfo cai{}; cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool = cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1; vkAllocateCommandBuffers(device, &cai, &cb); }

    printf("\n=== per-dispatch wall time (µs), ITERS=%u in one cmd buffer ===\n", ITERS);
    printf("%-12s %-20s %-20s %-20s\n", "wgs", "no-barrier", "barrier(each)", "barrier(diff-buf)");

    for (int G : wgs_list) {
        double t_nb = 0, t_b = 0, t_bd = 0;
        // 预热
        {
            VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cb, &bi);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
            for (uint32_t i = 0; i < ITERS; i++) vkCmdDispatch(cb, G, 1, 1);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        }
        // 1) 无 barrier
        {
            VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cb, &bi);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
            for (uint32_t i = 0; i < ITERS; i++) vkCmdDispatch(cb, G, 1, 1);
            vkEndCommandBuffer(cb);
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            double t0 = now_ms();
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
            t_nb = now_ms() - t0;
        }
        // 2) 每个 dispatch 前加 barrier (复刻 sync_buffers)
        {
            VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cb, &bi);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
            for (uint32_t i = 0; i < ITERS; i++) {
                vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
                vkCmdDispatch(cb, G, 1, 1);
            }
            vkEndCommandBuffer(cb);
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            double t0 = now_ms();
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
            t_b = now_ms() - t0;
        }
        // 3) barrier + 两个不同 buffer 轮流绑定 (模拟 rms_norm 读A写D 的依赖链)
        {
            VkDescriptorBufferInfo dbi2{ buf, 0, 4096 }; // 同一 buffer, 模拟读写别名
            VkWriteDescriptorSet w2{}; w2.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w2.dstSet = dset; w2.dstBinding = 0; w2.descriptorCount = 1; w2.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w2.pBufferInfo = &dbi2;
            vkUpdateDescriptorSets(device, 1, &w2, 0, nullptr);
            VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            vkBeginCommandBuffer(cb, &bi);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
            for (uint32_t i = 0; i < ITERS; i++) {
                vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
                vkCmdDispatch(cb, G, 1, 1);
            }
            vkEndCommandBuffer(cb);
            VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            double t0 = now_ms();
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
            t_bd = now_ms() - t0;
        }

        printf("%-12d %-20.2f %-20.2f %-20.2f\n", G,
               t_nb / ITERS * 1000.0, t_b / ITERS * 1000.0, t_bd / ITERS * 1000.0);
        fflush(stdout);
    }

    // 对照: rms_norm 真实形状单测 — 1 wg vs 6 wg 做一次真正的循环工作 (2560 列, 模拟 n=1 vs n=6)
    printf("\n=== 模拟 rms_norm(2560): %u 次 dispatch, 每 wg 512 线程, 16 次迭代 ===\n", ITERS);
    {
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
        for (uint32_t i = 0; i < ITERS; i++) {
            vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdDispatch(cb, 1, 1, 1);
        }
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        double t0 = now_ms();
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        printf("  1 wg  (n=1):  %7.2f µs/dispatch\n", (now_ms() - t0) / ITERS * 1000.0);
    }
    {
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
        for (uint32_t i = 0; i < ITERS; i++) {
            vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdDispatch(cb, 6, 1, 1);
        }
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        double t0 = now_ms();
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        printf("  6 wg  (n=6):  %7.2f µs/dispatch\n", (now_ms() - t0) / ITERS * 1000.0);
    }
    {
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
        for (uint32_t i = 0; i < ITERS; i++) {
            vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdDispatch(cb, 32, 1, 1);
        }
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        double t0 = now_ms();
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        printf("  32 wg (n=32): %7.2f µs/dispatch\n", (now_ms() - t0) / ITERS * 1000.0);
    }
    {
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &bi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &dset, 0, nullptr);
        for (uint32_t i = 0; i < ITERS; i++) {
            vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdDispatch(cb, 304, 1, 1);
        }
        vkEndCommandBuffer(cb);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        double t0 = now_ms();
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        printf("  304 wg (gate): %6.2f µs/dispatch\n", (now_ms() - t0) / ITERS * 1000.0);
    }

    printf("\nDONE\n");
    return 0;
}
