#include "render/vk_renderer.h"

#include <android/log.h>
#include <android/native_window.h>
#include <vulkan/vulkan_android.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "quad_frag_spv.h"
#include "quad_vert_spv.h"
#include "mesh_frag_spv.h"
#include "mesh_vert_spv.h"

#include <cmath>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ina", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ina", __VA_ARGS__)

namespace ina {

namespace {
constexpr uint32_t kMaxSets = 2000;   // one per shared texture, plus the image quad and the grey fallback
}

VkRenderer::~VkRenderer() { shutdown(); }

bool VkRenderer::fail(const char* what, VkResult r) {
    error_ = std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")";
    LOGE("%s", error_.c_str());
    return false;
}

uint32_t VkRenderer::memoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool VkRenderer::init(ANativeWindow* window) {
    window_ = window;
    VkResult r;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "infamous-native-android";
    app.apiVersion = VK_API_VERSION_1_0;
    const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    if ((r = vkCreateInstance(&ici, nullptr, &instance_)) != VK_SUCCESS) return fail("vkCreateInstance", r);

    VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = window_;
    if ((r = vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface_)) != VK_SUCCESS)
        return fail("vkCreateAndroidSurfaceKHR", r);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    bool found = false;
    for (VkPhysicalDevice g : gpus) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t i = 0; i < qn && !found; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface_, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                gpu_ = g;
                queueFamily_ = i;
                found = true;
            }
        }
        if (found) break;
    }
    if (!found) {
        error_ = "no Vulkan device can draw to the screen";
        return false;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu_, &props);
    LOGI("GPU: %s, Vulkan %u.%u.%u", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    if ((r = vkCreateDevice(gpu_, &dci, nullptr, &dev_)) != VK_SUCCESS) return fail("vkCreateDevice", r);
    vkGetDeviceQueue(dev_, queueFamily_, 0, &queue_);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if ((r = vkCreateCommandPool(dev_, &pci, nullptr, &pool_)) != VK_SUCCESS) return fail("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(dev_, &cai, &cmd_)) != VK_SUCCESS) return fail("vkAllocateCommandBuffers", r);

    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev_, &semi, nullptr, &acquired_);
    vkCreateSemaphore(dev_, &semi, nullptr, &rendered_);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev_, &fi, nullptr, &fence_);

    // Surface format: prefer an 8-bit UNORM format (the quad shader writes display-ready colour).
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu_, surface_, &n, formats.data());
    if (formats.empty()) {
        error_ = "the surface reports no formats";
        return false;
    }
    format_ = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) {
            format_ = f;
            break;
        }

    VkAttachmentDescription att{};
    att.format = format_.format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // Depth: 32-bit float where the device can use it as an attachment, else 16-bit (always available).
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu_, VK_FORMAT_D32_SFLOAT, &fp);
    depthFormat_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
    VkAttachmentDescription datt{};
    datt.format = depthFormat_;
    datt.samples = VK_SAMPLE_COUNT_1_BIT;
    datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    datt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    datt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    datt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    datt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    datt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    const VkAttachmentDescription atts[2] = {att, datt};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2;
    rpi.pAttachments = atts;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if ((r = vkCreateRenderPass(dev_, &rpi, nullptr, &pass_)) != VK_SUCCESS) return fail("vkCreateRenderPass", r);

    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 1;
    sli.pBindings = &bind;
    if ((r = vkCreateDescriptorSetLayout(dev_, &sli, nullptr, &setLayout_)) != VK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout", r);
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    if ((r = vkCreatePipelineLayout(dev_, &pli, nullptr, &layout_)) != VK_SUCCESS) return fail("vkCreatePipelineLayout", r);

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(quad_vert_spv);
    smi.pCode = quad_vert_spv;
    if ((r = vkCreateShaderModule(dev_, &smi, nullptr, &vs)) != VK_SUCCESS) return fail("vertex shader module", r);
    smi.codeSize = sizeof(quad_frag_spv);
    smi.pCode = quad_frag_spv;
    if ((r = vkCreateShaderModule(dev_, &smi, nullptr, &fs)) != VK_SUCCESS) {
        vkDestroyShaderModule(dev_, vs, nullptr);
        return fail("fragment shader module", r);
    }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &dyn;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    gpi.pDepthStencilState = &ds;   // the image quad ignores depth
    gpi.layout = layout_;
    gpi.renderPass = pass_;
    r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_);
    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    if (r != VK_SUCCESS) return fail("vkCreateGraphicsPipelines", r);

    // Mesh pipeline: position + normal vertices, one matrix as a push constant, depth tested, two-sided.
    // vertex: the matrix; fragment: the draw mode (opaque, cutout, decal)
    const VkPushConstantRange mpc[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 64}, {VK_SHADER_STAGE_FRAGMENT_BIT, 64, 4}};
    VkPipelineLayoutCreateInfo mli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    mli.setLayoutCount = 1;
    mli.pSetLayouts = &setLayout_;
    mli.pushConstantRangeCount = 2;
    mli.pPushConstantRanges = mpc;
    if ((r = vkCreatePipelineLayout(dev_, &mli, nullptr, &meshLayout_)) != VK_SUCCESS) return fail("mesh pipeline layout", r);
    smi.codeSize = sizeof(mesh_vert_spv);
    smi.pCode = mesh_vert_spv;
    if ((r = vkCreateShaderModule(dev_, &smi, nullptr, &vs)) != VK_SUCCESS) return fail("mesh vertex shader", r);
    smi.codeSize = sizeof(mesh_frag_spv);
    smi.pCode = mesh_frag_spv;
    if ((r = vkCreateShaderModule(dev_, &smi, nullptr, &fs)) != VK_SUCCESS) {
        vkDestroyShaderModule(dev_, vs, nullptr);
        return fail("mesh fragment shader", r);
    }
    stages[0].module = vs;
    stages[1].module = fs;
    const VkVertexInputBindingDescription vbind{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription vattr[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                         {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
    VkPipelineVertexInputStateCreateInfo mvi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    mvi.vertexBindingDescriptionCount = 1;
    mvi.pVertexBindingDescriptions = &vbind;
    mvi.vertexAttributeDescriptionCount = 3;
    mvi.pVertexAttributeDescriptions = vattr;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    gpi.pVertexInputState = &mvi;
    gpi.layout = meshLayout_;
    r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gpi, nullptr, &meshPipeline_);
    if (r == VK_SUCCESS) {
        // Decals: blended over what is already drawn, pulled slightly towards the camera, no depth writes.
        VkPipelineColorBlendAttachmentState dba{};
        dba.blendEnable = VK_TRUE;
        dba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        dba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        dba.colorBlendOp = VK_BLEND_OP_ADD;
        dba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        dba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        dba.alphaBlendOp = VK_BLEND_OP_ADD;
        dba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo dcb = cb;
        dcb.pAttachments = &dba;
        VkPipelineRasterizationStateCreateInfo drs = rs;
        drs.depthBiasEnable = VK_TRUE;
        drs.depthBiasConstantFactor = -2.0f;
        drs.depthBiasSlopeFactor = -2.0f;
        VkPipelineDepthStencilStateCreateInfo dds = ds;
        dds.depthWriteEnable = VK_FALSE;
        VkGraphicsPipelineCreateInfo dpi2 = gpi;
        dpi2.pColorBlendState = &dcb;
        dpi2.pRasterizationState = &drs;
        dpi2.pDepthStencilState = &dds;
        r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &dpi2, nullptr, &decalPipeline_);
    }
    vkDestroyShaderModule(dev_, vs, nullptr);
    vkDestroyShaderModule(dev_, fs, nullptr);
    if (r != VK_SUCCESS) return fail("mesh pipeline", r);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = kMaxSets;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if ((r = vkCreateDescriptorPool(dev_, &dpi, nullptr, &descPool_)) != VK_SUCCESS) return fail("vkCreateDescriptorPool", r);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout_;
    if ((r = vkAllocateDescriptorSets(dev_, &dai, &set_)) != VK_SUCCESS) return fail("vkAllocateDescriptorSets", r);

    VkSamplerCreateInfo sai{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sai.magFilter = VK_FILTER_NEAREST;
    sai.minFilter = VK_FILTER_LINEAR;
    sai.addressModeU = sai.addressModeV = sai.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if ((r = vkCreateSampler(dev_, &sai, nullptr, &sampler_)) != VK_SUCCESS) return fail("vkCreateSampler", r);

    sai.magFilter = VK_FILTER_LINEAR;
    sai.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sai.addressModeU = sai.addressModeV = sai.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sai.maxLod = VK_LOD_CLAMP_NONE;
    if ((r = vkCreateSampler(dev_, &sai, nullptr, &repeatSampler_)) != VK_SUCCESS) return fail("repeat sampler", r);
    if (!uploadTexture(1, 1, {{188, 184, 174, 255}}, repeatSampler_, &greyTexture_)) return false;

    if (!createSwapchain()) return false;
    // Start with a 1x1 dark texel so the descriptor is always valid.
    return setImage(1, 1, {32, 32, 32, 255});
}

bool VkRenderer::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu_, surface_, &caps);
    if (r != VK_SUCCESS) return fail("surface capabilities", r);
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width = uint32_t(ANativeWindow_getWidth(window_));
        extent_.height = uint32_t(ANativeWindow_getHeight(window_));
    }
    if (extent_.width == 0 || extent_.height == 0) {
        error_ = "the surface has no size yet";
        return false;
    }
    // Let the compositor rotate: we always draw in the surface's own orientation.
    transform_ = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                    : caps.currentTransform;
    uint32_t count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface_;
    sci.minImageCount = count;
    sci.imageFormat = format_.format;
    sci.imageColorSpace = format_.colorSpace;
    sci.imageExtent = extent_;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = transform_;
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    if ((r = vkCreateSwapchainKHR(dev_, &sci, nullptr, &swapchain_)) != VK_SUCCESS) return fail("vkCreateSwapchainKHR", r);

    VkImageCreateInfo dici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    dici.imageType = VK_IMAGE_TYPE_2D;
    dici.format = depthFormat_;
    dici.extent = {extent_.width, extent_.height, 1};
    dici.mipLevels = 1;
    dici.arrayLayers = 1;
    dici.samples = VK_SAMPLE_COUNT_1_BIT;
    dici.tiling = VK_IMAGE_TILING_OPTIMAL;
    dici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((r = vkCreateImage(dev_, &dici, nullptr, &depth_)) != VK_SUCCESS) return fail("depth image", r);
    VkMemoryRequirements dmr;
    vkGetImageMemoryRequirements(dev_, depth_, &dmr);
    VkMemoryAllocateInfo dmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    dmai.allocationSize = dmr.size;
    dmai.memoryTypeIndex = memoryType(dmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &dmai, nullptr, &depthMem_)) != VK_SUCCESS)
        return fail("depth memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkBindImageMemory(dev_, depth_, depthMem_, 0);
    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = depth_;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = depthFormat_;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if ((r = vkCreateImageView(dev_, &dvci, nullptr, &depthView_)) != VK_SUCCESS) return fail("depth view", r);

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
    std::vector<VkImage> images(n);
    vkGetSwapchainImagesKHR(dev_, swapchain_, &n, images.data());
    for (VkImage img : images) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format_.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if ((r = vkCreateImageView(dev_, &vci, nullptr, &view)) != VK_SUCCESS) return fail("swapchain image view", r);
        views_.push_back(view);
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = pass_;
        const VkImageView fbViews[2] = {view, depthView_};
        fci.attachmentCount = 2;
        fci.pAttachments = fbViews;
        fci.width = extent_.width;
        fci.height = extent_.height;
        fci.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if ((r = vkCreateFramebuffer(dev_, &fci, nullptr, &fb)) != VK_SUCCESS) return fail("framebuffer", r);
        framebuffers_.push_back(fb);
    }
    LOGI("swapchain %ux%u, %u images, format %d", extent_.width, extent_.height, n, int(format_.format));
    return true;
}

void VkRenderer::destroySwapchain() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(dev_, f, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(dev_, v, nullptr);
    framebuffers_.clear();
    views_.clear();
    if (depthView_) vkDestroyImageView(dev_, depthView_, nullptr);
    if (depth_) vkDestroyImage(dev_, depth_, nullptr);
    if (depthMem_) vkFreeMemory(dev_, depthMem_, nullptr);
    depthView_ = VK_NULL_HANDLE;
    depth_ = VK_NULL_HANDLE;
    depthMem_ = VK_NULL_HANDLE;
    if (swapchain_) vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

bool VkRenderer::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* src, VkBuffer* buf, VkDeviceMemory* mem) {
    VkResult r;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    if ((r = vkCreateBuffer(dev_, &bci, nullptr, buf)) != VK_SUCCESS) return fail("mesh buffer", r);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev_, *buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    // Host-visible memory keeps this simple; a staging copy into device-local memory can come with streaming.
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &mai, nullptr, mem)) != VK_SUCCESS)
        return fail("mesh memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    vkBindBufferMemory(dev_, *buf, *mem, 0);
    void* p = nullptr;
    if ((r = vkMapMemory(dev_, *mem, 0, size, 0, &p)) != VK_SUCCESS) return fail("map mesh memory", r);
    memcpy(p, src, size_t(size));
    vkUnmapMemory(dev_, *mem);
    return true;
}

void VkRenderer::destroyGpuTexture(GpuTexture& t) {
    if (!dev_) return;
    if (t.set) vkFreeDescriptorSets(dev_, descPool_, 1, &t.set);
    if (t.view) vkDestroyImageView(dev_, t.view, nullptr);
    if (t.image) vkDestroyImage(dev_, t.image, nullptr);
    if (t.memory) vkFreeMemory(dev_, t.memory, nullptr);
    t = GpuTexture{};
}

// Creates an RGBA8 texture with the given mip levels and a descriptor set that samples it.
bool VkRenderer::uploadTexture(uint32_t w, uint32_t h, const std::vector<std::vector<uint8_t>>& levels, VkSampler sampler, GpuTexture* out) {
    VkResult r;
    if (levels.empty() || w == 0 || h == 0) return false;
    VkDeviceSize total = 0;
    for (size_t m = 0; m < levels.size(); m++) {
        const VkDeviceSize need = VkDeviceSize(std::max(1u, w >> m)) * std::max(1u, h >> m) * 4;
        if (levels[m].size() < need) return false;
        total += need;
    }
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if ((r = vkCreateBuffer(dev_, &bci, nullptr, &staging)) != VK_SUCCESS) return fail("texture staging buffer", r);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev_, staging, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto dropStaging = [&] {
        vkDestroyBuffer(dev_, staging, nullptr);
        if (stagingMem) vkFreeMemory(dev_, stagingMem, nullptr);
    };
    if (mai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &mai, nullptr, &stagingMem)) != VK_SUCCESS) {
        dropStaging();
        return fail("texture staging memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    vkBindBufferMemory(dev_, staging, stagingMem, 0);
    void* p = nullptr;
    vkMapMemory(dev_, stagingMem, 0, total, 0, &p);
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize off = 0;
    for (size_t m = 0; m < levels.size(); m++) {
        const uint32_t mw = std::max(1u, w >> m), mh = std::max(1u, h >> m);
        memcpy(static_cast<uint8_t*>(p) + off, levels[m].data(), size_t(mw) * mh * 4);
        VkBufferImageCopy c{};
        c.bufferOffset = off;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(m), 0, 1};
        c.imageExtent = {mw, mh, 1};
        copies.push_back(c);
        off += VkDeviceSize(mw) * mh * 4;
    }
    vkUnmapMemory(dev_, stagingMem);

    GpuTexture t;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = uint32_t(levels.size());
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((r = vkCreateImage(dev_, &ici, nullptr, &t.image)) != VK_SUCCESS) {
        dropStaging();
        return fail("scene texture image", r);
    }
    vkGetImageMemoryRequirements(dev_, t.image, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &mai, nullptr, &t.memory)) != VK_SUCCESS) {
        dropStaging();
        destroyGpuTexture(t);
        return fail("scene texture memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    vkBindImageMemory(dev_, t.image, t.memory, 0);

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(levels.size()), 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(cmd_, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()), copies.data());
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    dropStaging();
    if (r != VK_SUCCESS) {
        destroyGpuTexture(t);
        return fail("scene texture upload", r);
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(levels.size()), 0, 1};
    if ((r = vkCreateImageView(dev_, &vci, nullptr, &t.view)) != VK_SUCCESS) {
        destroyGpuTexture(t);
        return fail("scene texture view", r);
    }
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descPool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &setLayout_;
    if ((r = vkAllocateDescriptorSets(dev_, &dai, &t.set)) != VK_SUCCESS) {
        t.set = VK_NULL_HANDLE;
        destroyGpuTexture(t);
        return fail("scene texture descriptor", r);
    }
    VkDescriptorImageInfo dii{sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = t.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    *out = t;
    return true;
}

// The fence covers the one frame that can be in flight; once it is signalled no buffer or texture is in use.
void VkRenderer::waitIdleFrame() {
    if (dev_ && fence_) vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
}

void VkRenderer::destroyChunk(Chunk& ch) {
    if (ch.vbuf) vkDestroyBuffer(dev_, ch.vbuf, nullptr);
    if (ch.ibuf) vkDestroyBuffer(dev_, ch.ibuf, nullptr);
    if (ch.vmem) vkFreeMemory(dev_, ch.vmem, nullptr);
    if (ch.imem) vkFreeMemory(dev_, ch.imem, nullptr);
    ch = Chunk{};
}

bool VkRenderer::hasTexture(uint64_t key) const { return textures_.count(key) != 0; }

bool VkRenderer::addTexture(uint64_t key, const SceneTexture& t) {
    if (!dev_ || key == 0 || textures_.count(key)) return false;
    if (textures_.size() + 8 >= kMaxSets) return false;   // the pool is full: batches using it draw grey
    GpuTexture g;
    if (!uploadTexture(t.width, t.height, t.levels, repeatSampler_, &g)) return false;
    SharedTexture& s = textures_[key];
    s.gpu = g;
    for (const auto& l : t.levels) s.bytes += l.size();
    textureBytes_ += s.bytes;
    return true;
}

std::vector<uint64_t> VkRenderer::textureKeys() const {
    std::vector<uint64_t> out;
    for (const auto& [k, t] : textures_) out.push_back(k);
    return out;
}

void VkRenderer::trimTextures(size_t budgetBytes) {
    if (textureBytes_ <= budgetBytes && textures_.size() + 64 < kMaxSets) return;
    std::vector<std::pair<uint64_t, uint64_t>> unused;   // last used, key
    for (const auto& [k, t] : textures_)
        if (t.users == 0) unused.push_back({t.lastUsed, k});
    std::sort(unused.begin(), unused.end());
    bool waited = false;
    for (const auto& [when, k] : unused) {
        if (textureBytes_ <= budgetBytes && textures_.size() + 64 < kMaxSets) break;
        if (!waited) {
            waitIdleFrame();
            waited = true;
        }
        auto it = textures_.find(k);
        textureBytes_ -= std::min(textureBytes_, it->second.bytes);
        destroyGpuTexture(it->second.gpu);
        textures_.erase(it);
    }
}

void VkRenderer::releaseTexture(uint64_t key) {
    auto it = textures_.find(key);
    if (it == textures_.end()) return;
    if (it->second.users > 0) it->second.users--;
    if (it->second.users == 0) it->second.lastUsed = ++releaseCounter_;   // kept until trimTextures needs the room
}

uint32_t VkRenderer::addChunk(const std::vector<float>& vertices, const std::vector<uint32_t>& indices, const std::vector<ChunkBatch>& batches,
                              const float boxMin[3], const float boxMax[3]) {
    if (!dev_ || vertices.empty() || indices.empty() || batches.empty()) return 0;
    Chunk ch;
    if (!createBuffer(vertices.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices.data(), &ch.vbuf, &ch.vmem) ||
        !createBuffer(indices.size() * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices.data(), &ch.ibuf, &ch.imem)) {
        destroyChunk(ch);
        return 0;
    }
    ch.indexCount = uint32_t(indices.size());
    ch.vertexBytes = vertices.size() * sizeof(float);
    ch.batches = batches;
    for (int k = 0; k < 3; k++) {
        ch.boxMin[k] = boxMin[k];
        ch.boxMax[k] = boxMax[k];
    }
    // Count the textures this chunk uses, once each.
    std::vector<uint64_t> keys;
    for (const ChunkBatch& b : batches)
        if (b.texture && textures_.count(b.texture) && std::find(keys.begin(), keys.end(), b.texture) == keys.end()) keys.push_back(b.texture);
    for (uint64_t k : keys) textures_[k].users++;
    const uint32_t id = nextChunk_++;
    chunks_[id] = std::move(ch);
    return id;
}

bool VkRenderer::updateChunkVertices(uint32_t id, const std::vector<float>& vertices) {
    auto it = chunks_.find(id);
    if (it == chunks_.end() || vertices.size() * sizeof(float) != it->second.vertexBytes) return false;
    waitIdleFrame();   // the last frame may still be reading the buffer
    void* p = nullptr;
    if (vkMapMemory(dev_, it->second.vmem, 0, it->second.vertexBytes, 0, &p) != VK_SUCCESS) return false;
    memcpy(p, vertices.data(), it->second.vertexBytes);
    vkUnmapMemory(dev_, it->second.vmem);
    return true;
}

void VkRenderer::setChunkBox(uint32_t id, const float boxMin[3], const float boxMax[3]) {
    auto it = chunks_.find(id);
    if (it == chunks_.end()) return;
    for (int k = 0; k < 3; k++) {
        it->second.boxMin[k] = boxMin[k];
        it->second.boxMax[k] = boxMax[k];
    }
}

void VkRenderer::removeChunk(uint32_t id) {
    auto it = chunks_.find(id);
    if (it == chunks_.end()) return;
    waitIdleFrame();
    std::vector<uint64_t> keys;
    for (const ChunkBatch& b : it->second.batches)
        if (b.texture && std::find(keys.begin(), keys.end(), b.texture) == keys.end()) keys.push_back(b.texture);
    destroyChunk(it->second);
    chunks_.erase(it);
    for (uint64_t k : keys) releaseTexture(k);
}

void VkRenderer::setChunkVisible(uint32_t id, bool visible) {
    auto it = chunks_.find(id);
    if (it != chunks_.end()) it->second.visible = visible;
}

void VkRenderer::clearChunks() {
    waitIdleFrame();
    for (auto& [id, ch] : chunks_) destroyChunk(ch);
    chunks_.clear();
    for (auto& [key, t] : textures_) destroyGpuTexture(t.gpu);
    textures_.clear();
    textureBytes_ = 0;
}

// The single-scene form: one chunk whose textures are numbered by their position in `textures`.
bool VkRenderer::setScene(const std::vector<float>& vertices, const std::vector<uint32_t>& indices, const std::vector<Batch>& batches,
                          const std::vector<SceneTexture>& textures) {
    if (!dev_) return false;
    clearChunks();
    if (vertices.empty() || indices.empty() || batches.empty()) return true;
    // (the scene's textures are freed with it: clearChunks above removes everything)
    // Keys for the scene's own textures: high bit set so they never meet the keys of streamed chunks.
    const uint64_t base = 1ull << 63;
    for (size_t i = 0; i < textures.size(); i++) addTexture(base + i + 1, textures[i]);
    std::vector<ChunkBatch> cb;
    for (const Batch& b : batches) cb.push_back({b.firstIndex, b.indexCount, b.texture == 0xFFFFFFFFu ? 0 : base + b.texture + 1, b.mode});
    float lo[3] = {-1e30f, -1e30f, -1e30f}, hi[3] = {1e30f, 1e30f, 1e30f};   // never culled
    return addChunk(vertices, indices, cb, lo, hi) != 0;
}

void VkRenderer::setCamera(const float target[3], float yaw, float pitch, float distance) {
    camTarget_[0] = target[0]; camTarget_[1] = target[1]; camTarget_[2] = target[2];
    camYaw_ = yaw;
    camPitch_ = pitch;
    camDist_ = distance;
}

void VkRenderer::destroyTexture() {
    if (!dev_) return;
    if (textureView_) vkDestroyImageView(dev_, textureView_, nullptr);
    if (texture_) vkDestroyImage(dev_, texture_, nullptr);
    if (textureMem_) vkFreeMemory(dev_, textureMem_, nullptr);
    textureView_ = VK_NULL_HANDLE;
    texture_ = VK_NULL_HANDLE;
    textureMem_ = VK_NULL_HANDLE;
}

bool VkRenderer::setImage(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    if (!dev_ || w == 0 || h == 0 || rgba.size() < size_t(w) * h * 4) return false;
    VkResult r;
    vkDeviceWaitIdle(dev_);
    destroyTexture();

    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if ((r = vkCreateBuffer(dev_, &bci, nullptr, &staging)) != VK_SUCCESS) return fail("staging buffer", r);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev_, staging, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto dropStaging = [&] {
        vkDestroyBuffer(dev_, staging, nullptr);
        if (stagingMem) vkFreeMemory(dev_, stagingMem, nullptr);
    };
    if (mai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &mai, nullptr, &stagingMem)) != VK_SUCCESS) {
        dropStaging();
        return fail("staging memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    vkBindBufferMemory(dev_, staging, stagingMem, 0);
    void* p = nullptr;
    vkMapMemory(dev_, stagingMem, 0, bytes, 0, &p);
    memcpy(p, rgba.data(), size_t(bytes));
    vkUnmapMemory(dev_, stagingMem);

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if ((r = vkCreateImage(dev_, &ici, nullptr, &texture_)) != VK_SUCCESS) {
        dropStaging();
        return fail("texture image", r);
    }
    vkGetImageMemoryRequirements(dev_, texture_, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || (r = vkAllocateMemory(dev_, &mai, nullptr, &textureMem_)) != VK_SUCCESS) {
        dropStaging();
        return fail("texture memory", VK_ERROR_OUT_OF_DEVICE_MEMORY);
    }
    vkBindImageMemory(dev_, texture_, textureMem_, 0);

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = texture_;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd_, staging, texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    r = vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    dropStaging();
    if (r != VK_SUCCESS) return fail("texture upload", r);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = texture_;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if ((r = vkCreateImageView(dev_, &vci, nullptr, &textureView_)) != VK_SUCCESS) return fail("texture view", r);
    VkDescriptorImageInfo dii{sampler_, textureView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = set_;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(dev_, 1, &wr, 0, nullptr);
    texW_ = w;
    texH_ = h;
    return true;
}

bool VkRenderer::drawFrame() {
    if (!dev_) return false;
    const auto t0 = std::chrono::steady_clock::now();
    if (!swapchain_ && !createSwapchain()) return true;   // surface not ready: try again next frame
    vkWaitForFences(dev_, 1, &fence_, VK_TRUE, UINT64_MAX);
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        destroySwapchain();
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return fail("vkAcquireNextImageKHR", r);
    vkResetFences(dev_, 1, &fence_);

    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd_, &bi);
    VkClearValue clear[2] = {};
    if (!chunks_.empty()) clear[0].color = {{0.36f, 0.45f, 0.58f, 1.0f}};
    else clear[0].color = {{0.05f, 0.05f, 0.07f, 1.0f}};
    clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = pass_;
    rbi.framebuffer = framebuffers_[index];
    rbi.renderArea.extent = extent_;
    rbi.clearValueCount = 2;
    rbi.pClearValues = clear;
    vkCmdBeginRenderPass(cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetViewport(cmd_, 0, 1, &vp);
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    const float screen = float(extent_.width) / float(extent_.height);
    if (!chunks_.empty()) {
        // Orbit camera, Z up. Column-major matrices: clip = proj * view * world.
        const float cp = std::cos(camPitch_), sp = std::sin(camPitch_);
        const float eye[3] = {camTarget_[0] + camDist_ * cp * std::cos(camYaw_), camTarget_[1] + camDist_ * cp * std::sin(camYaw_),
                              camTarget_[2] + camDist_ * sp};
        float f[3] = {camTarget_[0] - eye[0], camTarget_[1] - eye[1], camTarget_[2] - eye[2]};
        const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (float& v : f) v /= fl;
        float s[3] = {f[1], -f[0], 0.0f};   // forward x up(0,0,1)
        const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1]);
        s[0] /= sl; s[1] /= sl;
        const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
        const float zn = std::max(0.05f, camDist_ * 0.01f), zf = std::max(camDist_ * 40.0f, 60000.0f);
        const float t = 1.0f / std::tan(0.5f * 1.0472f);   // 60 degree vertical field of view
        const float A = zf / (zn - zf), B = zn * zf / (zn - zf);
        // rows of the view matrix: s, u, -f; projection flips Y for Vulkan and maps depth to 0..1
        const float vr[3][4] = {{s[0], s[1], s[2], -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2])},
                                {u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2])},
                                {-f[0], -f[1], -f[2], f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]}};
        float m[16];
        for (int c = 0; c < 4; c++) {
            m[c * 4 + 0] = (t / screen) * vr[0][c];
            m[c * 4 + 1] = -t * vr[1][c];
            m[c * 4 + 2] = A * vr[2][c] + (c == 3 ? B : 0.0f);
            m[c * 4 + 3] = -vr[2][c];
        }
        // Frustum planes from the clip matrix (column-major m: row r is m[r], m[4 + r], m[8 + r], m[12 + r]).
        auto row = [&](int r, int k) { return m[k * 4 + r]; };
        float planes[5][4];
        for (int k = 0; k < 4; k++) {
            planes[0][k] = row(3, k) + row(0, k);   // left
            planes[1][k] = row(3, k) - row(0, k);   // right
            planes[2][k] = row(3, k) + row(1, k);   // top / bottom
            planes[3][k] = row(3, k) - row(1, k);
            planes[4][k] = row(3, k) - row(2, k);   // far
        }
        auto visible = [&](const Chunk& ch) {
            for (const auto& pl : planes) {
                // the box corner furthest along the plane normal
                const float x = pl[0] >= 0 ? ch.boxMax[0] : ch.boxMin[0];
                const float y = pl[1] >= 0 ? ch.boxMax[1] : ch.boxMin[1];
                const float z = pl[2] >= 0 ? ch.boxMax[2] : ch.boxMin[2];
                if (pl[0] * x + pl[1] * y + pl[2] * z + pl[3] < 0) return false;
            }
            return true;
        };
        std::vector<const Chunk*> drawList;
        for (const auto& [id, ch] : chunks_)
            if (ch.visible && visible(ch)) drawList.push_back(&ch);
        drawnChunks_ = drawList.size();
        drawnTriangles_ = 0;
        // Three passes over the chunks: opaque, cutout, then decals (blended, after all solid geometry).
        for (uint32_t pass = 0; pass < 3; pass++) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pass == 2 && decalPipeline_ ? decalPipeline_ : meshPipeline_);
            vkCmdPushConstants(cmd_, meshLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, m);
            const int32_t mode = int32_t(pass);
            vkCmdPushConstants(cmd_, meshLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 64, 4, &mode);
            for (const Chunk* ch : drawList) {
                bool bound = false;
                for (const ChunkBatch& b : ch->batches) {
                    if (b.mode != pass) continue;
                    if (!bound) {
                        const VkDeviceSize zero = 0;
                        vkCmdBindVertexBuffers(cmd_, 0, 1, &ch->vbuf, &zero);
                        vkCmdBindIndexBuffer(cmd_, ch->ibuf, 0, VK_INDEX_TYPE_UINT32);
                        bound = true;
                    }
                    VkDescriptorSet set = greyTexture_.set;
                    if (b.texture) {
                        const auto it = textures_.find(b.texture);
                        if (it != textures_.end() && it->second.gpu.set) set = it->second.gpu.set;
                    }
                    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 1, &set, 0, nullptr);
                    if (b.firstIndex < ch->indexCount && b.indexCount <= ch->indexCount - b.firstIndex) {
                        vkCmdDrawIndexed(cmd_, b.indexCount, 1, b.firstIndex, 0, 0);
                        drawnTriangles_ += b.indexCount / 3;
                    }
                }
            }
        }
    } else {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
        // Fit the image inside the screen with a small margin, keeping its shape.
        const float image = texH_ ? float(texW_) / float(texH_) : 1.0f;
        float scale[2] = {0.94f, 0.94f};
        if (image > screen) scale[1] *= screen / image; else scale[0] *= image / screen;
        vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, scale);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd_);
    vkEndCommandBuffer(cmd_);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired_;
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &rendered_;
    if ((r = vkQueueSubmit(queue_, 1, &si, fence_)) != VK_SUCCESS) return fail("vkQueueSubmit", r);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &rendered_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        destroySwapchain();
    } else if (r != VK_SUCCESS) {
        return fail("vkQueuePresentKHR", r);
    }
    frameMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void VkRenderer::shutdown() {
    if (dev_) {
        vkDeviceWaitIdle(dev_);
        destroySwapchain();
        destroyTexture();
        clearChunks();
        destroyGpuTexture(greyTexture_);
        if (repeatSampler_) vkDestroySampler(dev_, repeatSampler_, nullptr);
        repeatSampler_ = VK_NULL_HANDLE;
        if (meshPipeline_) vkDestroyPipeline(dev_, meshPipeline_, nullptr);
        if (decalPipeline_) vkDestroyPipeline(dev_, decalPipeline_, nullptr);
        decalPipeline_ = VK_NULL_HANDLE;
        if (meshLayout_) vkDestroyPipelineLayout(dev_, meshLayout_, nullptr);
        meshPipeline_ = VK_NULL_HANDLE;
        meshLayout_ = VK_NULL_HANDLE;
        if (sampler_) vkDestroySampler(dev_, sampler_, nullptr);
        if (descPool_) vkDestroyDescriptorPool(dev_, descPool_, nullptr);
        if (pipeline_) vkDestroyPipeline(dev_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(dev_, layout_, nullptr);
        if (setLayout_) vkDestroyDescriptorSetLayout(dev_, setLayout_, nullptr);
        if (pass_) vkDestroyRenderPass(dev_, pass_, nullptr);
        if (fence_) vkDestroyFence(dev_, fence_, nullptr);
        if (acquired_) vkDestroySemaphore(dev_, acquired_, nullptr);
        if (rendered_) vkDestroySemaphore(dev_, rendered_, nullptr);
        if (pool_) vkDestroyCommandPool(dev_, pool_, nullptr);
        vkDestroyDevice(dev_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    dev_ = VK_NULL_HANDLE;
    surface_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    sampler_ = VK_NULL_HANDLE; descPool_ = VK_NULL_HANDLE; pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE; pass_ = VK_NULL_HANDLE; fence_ = VK_NULL_HANDLE; acquired_ = VK_NULL_HANDLE;
    rendered_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
}

}  // namespace ina
