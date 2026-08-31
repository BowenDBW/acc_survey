// bwbench.cpp — 原始 GPU 读带宽测试 (Adreno 740)
// 线性合并读取, 验证 matmul 0.3 GB/s 是访问模式问题还是 GPU/内存本身慢。
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdio>
#include <vector>

using ClockT = std::chrono::steady_clock;
static double now_ms() { return std::chrono::duration<double, std::milli>(ClockT::now().time_since_epoch()).count(); }

static const uint32_t bwbench_spv[] = {
#include "bwbench.spv.h"
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    VkInstance instance; VkApplicationInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO; ai.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo=&ai;
    if (vkCreateInstance(&ici,nullptr,&instance)!=VK_SUCCESS){printf("FAIL inst\n");return 1;}
    uint32_t n=0; vkEnumeratePhysicalDevices(instance,&n,nullptr);
    std::vector<VkPhysicalDevice> phys(n); vkEnumeratePhysicalDevices(instance,&n,phys.data());
    VkPhysicalDevice dev=phys[0]; VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(dev,&props);
    printf("GPU: %s\n", props.deviceName);

    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(dev,&qn,nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn); vkGetPhysicalDeviceQueueFamilyProperties(dev,&qn,qp.data());
    int qfam=-1; for(uint32_t i=0;i<qn;i++) if(qp[i].queueFlags&VK_QUEUE_COMPUTE_BIT){qfam=(int)i;break;}
    float prio=1.0f;
    VkDeviceQueueCreateInfo qci{}; qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qci.queueFamilyIndex=(uint32_t)qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dci{}; dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
    VkDevice device; if(vkCreateDevice(dev,&dci,nullptr,&device)!=VK_SUCCESS){printf("FAIL dev\n");return 1;}
    VkQueue queue; vkGetDeviceQueue(device,(uint32_t)qfam,0,&queue);

    // 大缓冲: 16M floats = 64MB
    const VkDeviceSize A_FLOATS = 16u<<20;
    const VkDeviceSize A_SIZE = A_FLOATS*4;
    VkBuffer abuf; VkDeviceMemory amem;
    VkBufferCreateInfo abci{}; abci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; abci.size=A_SIZE; abci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; abci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device,&abci,nullptr,&abuf);
    VkMemoryRequirements amr; vkGetBufferMemoryRequirements(device,abuf,&amr);
    VkPhysicalDeviceMemoryProperties amp; vkGetPhysicalDeviceMemoryProperties(dev,&amp);
    VkMemoryAllocateInfo amai{}; amai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; amai.allocationSize=amr.size;
    for(uint32_t i=0;i<amp.memoryTypeCount;i++) if((amr.memoryTypeBits&(1u<<i))&&(amp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){amai.memoryTypeIndex=i;break;}
    vkAllocateMemory(device,&amai,nullptr,&amem); vkBindBufferMemory(device,abuf,amem,0);
    // 填充
    float* p; vkMapMemory(device,amem,0,A_SIZE,0,(void**)&p);
    for(VkDeviceSize i=0;i<A_FLOATS;i++) p[i]=(float)(i&255);
    vkUnmapMemory(device,amem);

    VkBuffer obuf; VkDeviceMemory omem; // 输出 8KB
    VkBufferCreateInfo obci{}; obci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; obci.size=8192; obci.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT; obci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device,&obci,nullptr,&obuf);
    VkMemoryRequirements omr; vkGetBufferMemoryRequirements(device,obuf,&omr);
    VkMemoryAllocateInfo omai{}; omai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; omai.allocationSize=omr.size;
    for(uint32_t i=0;i<amp.memoryTypeCount;i++) if((omr.memoryTypeBits&(1u<<i))&&(amp.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)){omai.memoryTypeIndex=i;break;}
    vkAllocateMemory(device,&omai,nullptr,&omem); vkBindBufferMemory(device,obuf,omem,0);

    VkShaderModule mod; VkShaderModuleCreateInfo sm{}; sm.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO; sm.codeSize=sizeof(bwbench_spv); sm.pCode=bwbench_spv;
    vkCreateShaderModule(device,&sm,nullptr,&mod);
    VkDescriptorSetLayoutBinding db[2]{};
    db[0].binding=0; db[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; db[0].descriptorCount=1; db[0].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    db[1].binding=1; db[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; db[1].descriptorCount=1; db[1].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayout dsl; VkDescriptorSetLayoutCreateInfo dsli{}; dsli.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO; dsli.bindingCount=2; dsli.pBindings=db;
    vkCreateDescriptorSetLayout(device,&dsli,nullptr,&dsl);
    VkPushConstantRange pcr{}; pcr.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; pcr.offset=0; pcr.size=4;
    VkPipelineLayout pl; VkPipelineLayoutCreateInfo pli{}; pli.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO; pli.setLayoutCount=1; pli.pSetLayouts=&dsl; pli.pushConstantRangeCount=1; pli.pPushConstantRanges=&pcr;
    vkCreatePipelineLayout(device,&pli,nullptr,&pl);
    VkPipelineShaderStageCreateInfo st{}; st.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; st.stage=VK_SHADER_STAGE_COMPUTE_BIT; st.module=mod; st.pName="main";
    VkComputePipelineCreateInfo cp{}; cp.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO; cp.stage=st; cp.layout=pl;
    VkPipeline pipe; vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipe);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}; VkDescriptorPool dp; VkDescriptorPoolCreateInfo dpci{}; dpci.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO; dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&ps;
    vkCreateDescriptorPool(device,&dpci,nullptr,&dp);
    VkDescriptorSet dset; VkDescriptorSetAllocateInfo dsai{}; dsai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO; dsai.descriptorPool=dp; dsai.descriptorSetCount=1; dsai.pSetLayouts=&dsl;
    vkAllocateDescriptorSets(device,&dsai,&dset);
    VkDescriptorBufferInfo dba{abuf,0,A_SIZE}, dbo{obuf,0,8192};
    VkWriteDescriptorSet wd[2]{};
    wd[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[0].dstSet=dset; wd[0].dstBinding=0; wd[0].descriptorCount=1; wd[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wd[0].pBufferInfo=&dba;
    wd[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wd[1].dstSet=dset; wd[1].dstBinding=1; wd[1].descriptorCount=1; wd[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; wd[1].pBufferInfo=&dbo;
    vkUpdateDescriptorSets(device,2,wd,0,nullptr);

    VkCommandPool cpool; VkCommandPoolCreateInfo cpci{}; cpci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; cpci.queueFamilyIndex=(uint32_t)qfam;
    vkCreateCommandPool(device,&cpci,nullptr,&cpool);
    VkCommandBuffer cb; VkCommandBufferAllocateInfo cai{}; cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; cai.commandPool=cpool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
    vkAllocateCommandBuffers(device,&cai,&cb);

    // 网格: 16384 wgs x 256 = 4M 线程
    const uint32_t THREADS = 4u<<20;
    const uint32_t WGS = THREADS/256;
    const int iters_list[] = {1, 4, 16};
    const uint32_t R = 5;

    printf("\n=== 纯合并读带宽 (R=%u, %u 线程) ===\n", R, THREADS);
    printf("%-10s %-14s %-12s\n", "per_thread", "read MB", "GB/s");
    for (int it : iters_list) {
        VkCommandBufferBeginInfo bi{}; bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb,&bi);
        vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pipe);
        vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_COMPUTE,pl,0,1,&dset,0,nullptr);
        for (uint32_t r=0;r<R;r++) { vkCmdPushConstants(cb,pl,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&it); vkCmdDispatch(cb,WGS,1,1); }
        vkEndCommandBuffer(cb);
        // 预热
        VkSubmitInfo si{}; si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO; si.commandBufferCount=1; si.pCommandBuffers=&cb;
        vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        double t0=now_ms(); vkQueueSubmit(queue,1,&si,VK_NULL_HANDLE); vkQueueWaitIdle(queue);
        double ms = now_ms()-t0;
        double bytes = (double)THREADS*it*4.0; // 每线程读 it 个 float
        printf("%-10d %-14.1f %-12.2f\n", it, bytes/1e6, bytes/(ms/1000.0)/1e9);
    }
    printf("DONE\n");
    return 0;
}
