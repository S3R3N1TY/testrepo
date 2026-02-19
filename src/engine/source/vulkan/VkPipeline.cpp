#include <fstream>
#include <optional>
#include <cstring>

#include "VkPipeline.h"
#include "VkUtils.h"
#include "DeferredDeletionService.h"

namespace {
struct PipelineCacheHeader {
    uint32_t headerSize;
    uint32_t headerVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint8_t pipelineCacheUUID[VK_UUID_SIZE];
};

bool cacheHeaderMatches(const std::vector<char>& cacheData, VkPhysicalDevice physicalDevice)
{
    if (physicalDevice == VK_NULL_HANDLE) {
        return false;
    }
    if (cacheData.size() < sizeof(PipelineCacheHeader)) {
        return false;
    }

    PipelineCacheHeader header{};
    std::memcpy(&header, cacheData.data(), sizeof(PipelineCacheHeader));
    if (header.headerSize < sizeof(PipelineCacheHeader)) {
        return false;
    }
    if (header.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);

    if (header.vendorID != props.vendorID || header.deviceID != props.deviceID) {
        return false;
    }

    return std::memcmp(header.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}
}

PipelineCacheManager::PipelineCacheManager(VkDevice device, VkPhysicalDevice physicalDevice, const std::string& cachePath)
    : cachePath_(cachePath)
    , physicalDevice_(physicalDevice)
    , cache_()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("PipelineCacheManager: device is VK_NULL_HANDLE");
    }

    std::vector<char> initialData{};
    if (!cachePath_.empty()) {
        std::ifstream in(cachePath_, std::ios::binary | std::ios::ate);
        if (in) {
            const std::streamsize size = in.tellg();
            if (size > 0) {
                initialData.resize(static_cast<size_t>(size));
                in.seekg(0, std::ios::beg);
                if (!in.read(initialData.data(), size) || !cacheHeaderMatches(initialData, physicalDevice_)) {
                    initialData.clear();
                }
            }
        }
    }

    VkPipelineCacheCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = initialData.size();
    ci.pInitialData = initialData.empty() ? nullptr : initialData.data();

    VkPipelineCache created = VK_NULL_HANDLE;
    const VkResult res = vkCreatePipelineCache(device, &ci, nullptr, &created);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreatePipelineCache", res);
    }

    cache_ = DeferredDeletionService::instance().makeDeferredHandle<VkPipelineCache, PFN_vkDestroyPipelineCache>(
        device,
        created,
        vkDestroyPipelineCache);
}

PipelineCacheManager::~PipelineCacheManager() noexcept
{
    if (!valid()) {
        return;
    }

    try {
        save();
    } catch (const vkutil::VkException& ex) {
        vkutil::reportDiagnostic(vkutil::VkDiagnosticMessage{
            .subsystem = "pipeline-cache",
            .operation = "PipelineCacheManager::save",
            .result = ex.result(),
            .text = ex.what() });
    } catch (const std::exception& ex) {
        vkutil::reportDiagnostic(vkutil::VkDiagnosticMessage{
            .subsystem = "pipeline-cache",
            .operation = "PipelineCacheManager::save",
            .result = VK_ERROR_UNKNOWN,
            .text = ex.what() });
    } catch (...) {
        vkutil::reportDiagnostic(vkutil::VkDiagnosticMessage{
            .subsystem = "pipeline-cache",
            .operation = "PipelineCacheManager::save",
            .result = vkutil::exceptionToVkResult(),
            .text = "unknown exception while saving pipeline cache" });
    }
}

void PipelineCacheManager::save() const
{
    if (!valid() || cachePath_.empty()) {
        return;
    }

    size_t dataSize = 0;
    VkResult res = vkGetPipelineCacheData(cache_.getDevice(), cache_.get(), &dataSize, nullptr);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkGetPipelineCacheData", res);
    }
    if (dataSize == 0) {
        return;
    }

    std::vector<char> data(dataSize);
    res = vkGetPipelineCacheData(cache_.getDevice(), cache_.get(), &dataSize, data.data());
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkGetPipelineCacheData", res);
    }

    std::ofstream out(cachePath_, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("PipelineCacheManager: unable to open cache path for writing");
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

vkutil::VkExpected<VulkanRenderPass> VulkanRenderPass::createResult(
    VkDevice device,
    VkFormat colorFormat,
    VkFormat depthFormat,
    VkImageLayout finalColorLayout,
    VkAttachmentLoadOp colorLoadOp,
    VkImageLayout initialColorLayout)
{
    try {
        return VulkanRenderPass(device, colorFormat, depthFormat, finalColorLayout, colorLoadOp, initialColorLayout);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanRenderPass>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanRenderPass>(vkutil::exceptionToVkResult());
    }
}

vkutil::VkExpected<VulkanRenderPass> VulkanRenderPass::createResult(
    VkDevice device,
    const std::vector<VkFormat>& colorFormats,
    VkFormat depthFormat,
    VkImageLayout finalColorLayout,
    VkAttachmentLoadOp colorLoadOp,
    VkImageLayout initialColorLayout)
{
    try {
        return VulkanRenderPass(device, colorFormats, depthFormat, finalColorLayout, colorLoadOp, initialColorLayout);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanRenderPass>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanRenderPass>(vkutil::exceptionToVkResult());
    }
}

vkutil::VkExpected<VulkanRenderPass> VulkanRenderPass::createResult(VkDevice device, const VulkanRenderPassDescription& description)
{
    try {
        return VulkanRenderPass(device, description);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanRenderPass>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanRenderPass>(vkutil::exceptionToVkResult());
    }
}

VulkanRenderPass::VulkanRenderPass(
    VkDevice device,
    VkFormat colorFormat,
    VkFormat depthFormat,
    VkImageLayout finalColorLayout,
    VkAttachmentLoadOp colorLoadOp,
    VkImageLayout initialColorLayout)
    : VulkanRenderPass(device,
        std::vector<VkFormat>{ colorFormat },
        depthFormat,
        finalColorLayout,
        colorLoadOp,
        initialColorLayout)
{
}

VulkanRenderPass::VulkanRenderPass(
    VkDevice device,
    const std::vector<VkFormat>& colorFormats,
    VkFormat depthFormat,
    VkImageLayout finalColorLayout,
    VkAttachmentLoadOp colorLoadOp,
    VkImageLayout initialColorLayout)
    : handle()
{
    VulkanRenderPassDescription description{};
    for (VkFormat colorFormat : colorFormats) {
        VulkanRenderPassAttachmentDesc colorAttachment{};
        colorAttachment.format = colorFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = colorLoadOp;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = initialColorLayout;
        colorAttachment.finalLayout = finalColorLayout;
        description.attachments.push_back(colorAttachment);
    }

    VulkanRenderPassSubpassDesc subpass{};
    for (uint32_t i = 0; i < colorFormats.size(); ++i) {
        subpass.colorAttachments.push_back(VulkanRenderPassAttachmentRef{
            .attachment = i,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
    }

    if (depthFormat != VK_FORMAT_UNDEFINED) {
        VulkanRenderPassAttachmentDesc depthAttachment{};
        depthAttachment.format = depthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        description.attachments.push_back(depthAttachment);

        subpass.depthStencilAttachment = VulkanRenderPassAttachmentRef{
            .attachment = static_cast<uint32_t>(colorFormats.size()),
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }

    description.subpasses.push_back(subpass);

    VkSubpassDependency defaultDependency{};
    defaultDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    defaultDependency.dstSubpass = 0;
    defaultDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    defaultDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    defaultDependency.srcAccessMask = 0;
    defaultDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    if (depthFormat != VK_FORMAT_UNDEFINED) {
        defaultDependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        defaultDependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        defaultDependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    }
    description.dependencies.push_back(defaultDependency);

    *this = VulkanRenderPass(device, description);
}

VulkanRenderPass::VulkanRenderPass(VkDevice device, const VulkanRenderPassDescription& description)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanRenderPass: device is VK_NULL_HANDLE");
    }
    if (description.attachments.empty()) {
        throw std::runtime_error("VulkanRenderPass: at least one attachment is required");
    }
    if (description.subpasses.empty()) {
        throw std::runtime_error("VulkanRenderPass: at least one subpass is required");
    }

    std::vector<VkAttachmentDescription> attachments{};
    attachments.reserve(description.attachments.size());
    for (const VulkanRenderPassAttachmentDesc& attachmentDesc : description.attachments) {
        if (attachmentDesc.format == VK_FORMAT_UNDEFINED) {
            throw std::runtime_error("VulkanRenderPass: attachment format cannot be VK_FORMAT_UNDEFINED");
        }

        VkAttachmentDescription attachment{};
        attachment.flags = attachmentDesc.transient ? VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT : 0;
        attachment.format = attachmentDesc.format;
        attachment.samples = attachmentDesc.samples;
        attachment.loadOp = attachmentDesc.loadOp;
        attachment.storeOp = attachmentDesc.storeOp;
        attachment.stencilLoadOp = attachmentDesc.stencilLoadOp;
        attachment.stencilStoreOp = attachmentDesc.stencilStoreOp;
        attachment.initialLayout = attachmentDesc.initialLayout;
        attachment.finalLayout = attachmentDesc.finalLayout;
        attachments.push_back(attachment);
    }

    std::vector<VkSubpassDescription> subpasses{};
    subpasses.reserve(description.subpasses.size());

    std::vector<std::vector<VkAttachmentReference>> inputRefs(description.subpasses.size());
    std::vector<std::vector<VkAttachmentReference>> colorRefs(description.subpasses.size());
    std::vector<std::vector<VkAttachmentReference>> resolveRefs(description.subpasses.size());
    std::vector<std::vector<uint32_t>> preserveRefs(description.subpasses.size());
    std::vector<std::optional<VkAttachmentReference>> depthRefs(description.subpasses.size());

    auto convertRef = [attachmentCount = attachments.size()](const VulkanRenderPassAttachmentRef& ref) -> VkAttachmentReference {
        if (ref.attachment != VK_ATTACHMENT_UNUSED && ref.attachment >= attachmentCount) {
            throw std::runtime_error("VulkanRenderPass: attachment reference index out of range");
        }
        VkAttachmentReference converted{};
        converted.attachment = ref.attachment;
        converted.layout = ref.layout;
        return converted;
    };

    for (size_t subpassIndex = 0; subpassIndex < description.subpasses.size(); ++subpassIndex) {
        const VulkanRenderPassSubpassDesc& subpassDesc = description.subpasses[subpassIndex];

        for (const VulkanRenderPassAttachmentRef& ref : subpassDesc.inputAttachments) {
            inputRefs[subpassIndex].push_back(convertRef(ref));
        }
        for (const VulkanRenderPassAttachmentRef& ref : subpassDesc.colorAttachments) {
            colorRefs[subpassIndex].push_back(convertRef(ref));
        }
        for (const VulkanRenderPassAttachmentRef& ref : subpassDesc.resolveAttachments) {
            resolveRefs[subpassIndex].push_back(convertRef(ref));
        }
        if (!resolveRefs[subpassIndex].empty() && resolveRefs[subpassIndex].size() != colorRefs[subpassIndex].size()) {
            throw std::runtime_error("VulkanRenderPass: resolve attachment count must match color attachment count");
        }

        for (uint32_t attachmentIndex : subpassDesc.preserveAttachments) {
            if (attachmentIndex >= attachments.size()) {
                throw std::runtime_error("VulkanRenderPass: preserve attachment index out of range");
            }
            preserveRefs[subpassIndex].push_back(attachmentIndex);
        }

        if (subpassDesc.depthStencilAttachment.has_value()) {
            depthRefs[subpassIndex] = convertRef(subpassDesc.depthStencilAttachment.value());
        }

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = subpassDesc.bindPoint;
        subpass.inputAttachmentCount = static_cast<uint32_t>(inputRefs[subpassIndex].size());
        subpass.pInputAttachments = inputRefs[subpassIndex].empty() ? nullptr : inputRefs[subpassIndex].data();
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs[subpassIndex].size());
        subpass.pColorAttachments = colorRefs[subpassIndex].empty() ? nullptr : colorRefs[subpassIndex].data();
        subpass.pResolveAttachments = resolveRefs[subpassIndex].empty() ? nullptr : resolveRefs[subpassIndex].data();
        subpass.pDepthStencilAttachment = depthRefs[subpassIndex].has_value() ? &depthRefs[subpassIndex].value() : nullptr;
        subpass.preserveAttachmentCount = static_cast<uint32_t>(preserveRefs[subpassIndex].size());
        subpass.pPreserveAttachments = preserveRefs[subpassIndex].empty() ? nullptr : preserveRefs[subpassIndex].data();
        subpasses.push_back(subpass);
    }

    std::vector<VkSubpassDependency> dependencies = description.dependencies;

    VkRenderPassCreateInfo rpci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = static_cast<uint32_t>(subpasses.size());
    rpci.pSubpasses = subpasses.data();
    rpci.dependencyCount = static_cast<uint32_t>(dependencies.size());
    rpci.pDependencies = dependencies.empty() ? nullptr : dependencies.data();

    VkRenderPass rp = VK_NULL_HANDLE;
    const VkResult res = vkCreateRenderPass(device, &rpci, nullptr, &rp);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateRenderPass", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkRenderPass, PFN_vkDestroyRenderPass>(device, rp, vkDestroyRenderPass);
}

VulkanPipelineLayout::VulkanPipelineLayout(
    VkDevice device,
    const std::vector<VkDescriptorSetLayout>& setLayouts,
    const std::vector<VkPushConstantRange>& pushConstants,
    const void* pNext)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPipelineLayout: device is VK_NULL_HANDLE");
    }

    VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    ci.pNext = pNext;
    ci.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    ci.pSetLayouts = setLayouts.empty() ? nullptr : setLayouts.data();
    ci.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    ci.pPushConstantRanges = pushConstants.empty() ? nullptr : pushConstants.data();

    VkPipelineLayout pl = VK_NULL_HANDLE;
    const VkResult res = vkCreatePipelineLayout(device, &ci, nullptr, &pl);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreatePipelineLayout", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkPipelineLayout, PFN_vkDestroyPipelineLayout>(device, pl, vkDestroyPipelineLayout);
}

vkutil::VkExpected<VulkanPipeline> VulkanPipeline::createResult(
    VkDevice device,
    const std::vector<VkPipelineShaderStageCreateInfo>& shaderStages,
    const VkGraphicsPipelineCreateInfo& pipelineCreateInfo,
    const VulkanPipelineBuildInfo& buildInfo)
{
    try {
        return VulkanPipeline(device, shaderStages, pipelineCreateInfo, buildInfo);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanPipeline>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanPipeline>(vkutil::exceptionToVkResult());
    }
}

VulkanPipeline::VulkanPipeline(
    VkDevice device,
    const std::vector<VkPipelineShaderStageCreateInfo>& shaderStages,
    const VkGraphicsPipelineCreateInfo& pipelineCreateInfo,
    const VulkanPipelineBuildInfo& buildInfo)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPipeline: device is VK_NULL_HANDLE");
    }
    if (buildInfo.pipelineLayout == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanPipeline: pipelineLayout is VK_NULL_HANDLE");
    }
    if (shaderStages.empty()) {
        throw std::runtime_error("VulkanPipeline: shaderStages is empty");
    }

    VkGraphicsPipelineCreateInfo ci = pipelineCreateInfo;
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = static_cast<uint32_t>(shaderStages.size());
    ci.pStages = shaderStages.data();
    ci.layout = buildInfo.pipelineLayout;
    ci.subpass = buildInfo.subpass;
    ci.flags |= buildInfo.createFlags;

    VkPipelineRenderingCreateInfo renderingCI{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    if (buildInfo.useDynamicRendering) {
        if (buildInfo.colorFormats.empty()) {
            throw std::runtime_error("VulkanPipeline: dynamic rendering requires at least one color format");
        }
        for (VkFormat fmt : buildInfo.colorFormats) {
            if (fmt == VK_FORMAT_UNDEFINED) {
                throw std::runtime_error("VulkanPipeline: dynamic rendering color formats cannot contain VK_FORMAT_UNDEFINED");
            }
        }

        renderingCI.colorAttachmentCount = static_cast<uint32_t>(buildInfo.colorFormats.size());
        renderingCI.pColorAttachmentFormats = buildInfo.colorFormats.data();
        renderingCI.depthAttachmentFormat = buildInfo.depthFormat;
        renderingCI.stencilAttachmentFormat = buildInfo.stencilFormat;

        renderingCI.pNext = ci.pNext;
        ci.pNext = &renderingCI;
        ci.renderPass = VK_NULL_HANDLE;
    } else {
        if (buildInfo.renderPass == VK_NULL_HANDLE) {
            throw std::runtime_error("VulkanPipeline: renderPass is VK_NULL_HANDLE for non-dynamic rendering");
        }
        ci.renderPass = buildInfo.renderPass;
    }

    VkPipeline p = VK_NULL_HANDLE;
    const VkResult res = vkCreateGraphicsPipelines(device, buildInfo.pipelineCache, 1, &ci, nullptr, &p);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateGraphicsPipelines", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkPipeline, PFN_vkDestroyPipeline>(device, p, vkDestroyPipeline);
}

vkutil::VkExpected<VulkanComputePipeline> VulkanComputePipeline::createResult(
    VkDevice device,
    const VkComputePipelineCreateInfo& pipelineCreateInfo,
    VkPipelineCache pipelineCache,
    VkPipelineCreateFlags createFlags)
{
    try {
        return VulkanComputePipeline(device, pipelineCreateInfo, pipelineCache, createFlags);
    } catch (const vkutil::VkException& ex) {
        return vkutil::VkExpected<VulkanComputePipeline>(ex.result());
    } catch (...) {
        return vkutil::VkExpected<VulkanComputePipeline>(vkutil::exceptionToVkResult());
    }
}

VulkanComputePipeline::VulkanComputePipeline(
    VkDevice device,
    const VkComputePipelineCreateInfo& pipelineCreateInfo,
    VkPipelineCache pipelineCache,
    VkPipelineCreateFlags createFlags)
    : handle()
{
    if (device == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanComputePipeline: device is VK_NULL_HANDLE");
    }
    if (pipelineCreateInfo.layout == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanComputePipeline: layout is VK_NULL_HANDLE");
    }
    if (pipelineCreateInfo.stage.module == VK_NULL_HANDLE) {
        throw std::runtime_error("VulkanComputePipeline: shader module is VK_NULL_HANDLE");
    }

    VkComputePipelineCreateInfo ci = pipelineCreateInfo;
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.flags |= createFlags;

    VkPipeline p = VK_NULL_HANDLE;
    const VkResult res = vkCreateComputePipelines(device, pipelineCache, 1, &ci, nullptr, &p);
    if (res != VK_SUCCESS) {
        vkutil::throwVkError("vkCreateComputePipelines", res);
    }

    handle = DeferredDeletionService::instance().makeDeferredHandle<VkPipeline, PFN_vkDestroyPipeline>(device, p, vkDestroyPipeline);
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::setShaderStages(std::vector<VkPipelineShaderStageCreateInfo> stages)
{
    shaderStages_ = std::move(stages);
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::setCreateInfo(const VkGraphicsPipelineCreateInfo& ci)
{
    createInfo_ = ci;
    return *this;
}

GraphicsPipelineBuilder& GraphicsPipelineBuilder::setBuildInfo(const VulkanPipelineBuildInfo& buildInfo)
{
    buildInfo_ = buildInfo;
    return *this;
}

VulkanPipeline GraphicsPipelineBuilder::build(VkDevice device) const
{
    return VulkanPipeline(device, shaderStages_, createInfo_, buildInfo_);
}

ComputePipelineBuilder& ComputePipelineBuilder::setCreateInfo(const VkComputePipelineCreateInfo& ci)
{
    createInfo_ = ci;
    return *this;
}

ComputePipelineBuilder& ComputePipelineBuilder::setPipelineCache(VkPipelineCache cache) noexcept
{
    pipelineCache_ = cache;
    return *this;
}

ComputePipelineBuilder& ComputePipelineBuilder::setCreateFlags(VkPipelineCreateFlags flags) noexcept
{
    createFlags_ = flags;
    return *this;
}

VulkanComputePipeline ComputePipelineBuilder::build(VkDevice device) const
{
    return VulkanComputePipeline(device, createInfo_, pipelineCache_, createFlags_);
}
