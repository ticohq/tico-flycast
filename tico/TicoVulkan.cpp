/// @file TicoVulkan.cpp
/// @brief Vulkan frontend implementation.
///
/// Lifecycle:
///   CreateInstance()                  // VkInstance + VkSurface
///   retro_set_environment(...)        // core registers SET_HW_RENDER + neg iface
///   retro_init()
///   retro_load_game()                 // core does its non-GPU init
///   CreateDeviceAndSwapchain()        // negotiation->create_device + swapchain
///   hw_render_callback.context_reset()// core calls GET_HW_RENDER_INTERFACE on us
///   loop:
///     BeginFrame() -> retro_run() -> EndFrame()
///
/// The libretro Vulkan interface is sparsely documented; the canonical
/// reference is core/deps/libretro-common/include/libretro_vulkan.h.

#include "TicoVulkan.h"
#include "TicoLogger.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

// Static ICD entry points exposed by Mesa's loaderless libvulkan.a (NVK).
// Same shape dolphin uses on Switch — there's no shared libvulkan to dlopen.
#if defined(__SWITCH__)
extern "C" {
PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);
VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion);
}
#endif

// Note: VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE is defined in
// core/rend/vulkan/vk_context_lr.cpp; we share the same dispatcher.

namespace TicoVulkan
{
namespace
{

constexpr const char* TAG = "VK";

// --- State ---------------------------------------------------------------

struct PerFrame
{
    vk::CommandBuffer cmd;
    vk::Fence inflightFence;        // signalled when GPU finishes this frame
    vk::Semaphore acquireSemaphore; // signalled by vkAcquireNextImage
    vk::Semaphore renderSemaphore;  // signalled by our submit, waited by present

    // Set by core via set_image() each frame. Pointer ownership stays with
    // the core; we read it from EndFrame.
    retro_vulkan_image image{};
    bool imageValid = false;

    // Optional command buffers the core asked us to submit (set_command_buffers).
    std::vector<vk::CommandBuffer> coreCommandBuffers;

    // Optional semaphore the core wants signalled when we finish using the image.
    vk::Semaphore signalSemaphore = VK_NULL_HANDLE;
};

struct OverlayTextureResource
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    vk::Sampler sampler;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
};

vk::Instance        s_instance;
vk::PhysicalDevice  s_gpu;
vk::Device          s_device;
vk::Queue           s_queue;
uint32_t            s_queueFamilyIndex = 0;

vk::SurfaceKHR      s_surface;
vk::SwapchainKHR    s_swapchain;
vk::Format          s_swapFormat = vk::Format::eUndefined;
vk::Extent2D        s_swapExtent;
vk::Extent2D        s_sourceExtent;  // last frame extent from video_refresh
vk::Rect2D          s_gameViewport{{0, 0}, {0, 0}};  // game blit dst; 0 extent => full swap
std::vector<vk::Image>     s_swapImages;
std::vector<vk::ImageView> s_swapImageViews;
vk::RenderPass     s_overlayRenderPass;
std::vector<vk::Framebuffer> s_overlayFramebuffers;

vk::CommandPool     s_commandPool;
std::vector<PerFrame> s_frames;
uint32_t            s_currentFrame = 0;   // rotating frame slot (== libretro sync_index)
uint32_t            s_currentImage = 0;   // current swap image returned by acquire
bool                s_frameInFlight = false;
bool                s_ready = false;
bool                s_overlayReady = false;
vk::DescriptorPool s_overlayDescriptorPool;
ImDrawData*        s_overlayDrawData = nullptr;
retro_vulkan_image s_lastImage{};
bool               s_lastImageValid = false;
uint32_t           s_lastImageFrame = 0;
std::vector<OverlayTextureResource> s_overlayTextures;

const retro_hw_render_context_negotiation_interface_vulkan* s_negIface = nullptr;
retro_hw_render_interface_vulkan s_hwIface{};

std::mutex s_queueMutex;

#ifdef __SWITCH__
NWindow* s_nwindow = nullptr;
#endif

// --- Logging -------------------------------------------------------------

#define VK_LOG_INFO(fmt, ...)  LOG_INFO(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_WARN(fmt, ...)  LOG_WARN(TAG, fmt, ##__VA_ARGS__)
#define VK_LOG_ERROR(fmt, ...) LOG_ERROR(TAG, fmt, ##__VA_ARGS__)

// --- Helpers -------------------------------------------------------------

void TransitionLayout(vk::CommandBuffer cmd, vk::Image image,
                      vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                      vk::AccessFlags srcAccess, vk::AccessFlags dstAccess,
                      vk::PipelineStageFlags srcStage, vk::PipelineStageFlags dstStage)
{
    vk::ImageMemoryBarrier barrier;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    cmd.pipelineBarrier(srcStage, dstStage, vk::DependencyFlags(), nullptr, nullptr, barrier);
}

PFN_vkGetInstanceProcAddr GetInstanceProcAddrFunc()
{
#if defined(__SWITCH__)
    uint32_t icdVersion = 5;
    vk_icdNegotiateLoaderICDInterfaceVersion(&icdVersion);
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
#else
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr);
#endif
}

PFN_vkVoidFunction ImGuiVulkanLoader(const char* functionName, void* /*userData*/)
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (!getInstProcAddr)
        return nullptr;
    return getInstProcAddr(static_cast<VkInstance>(s_instance), functionName);
}

bool CreateOverlayRenderTargets()
{
    try
    {
        vk::AttachmentDescription colorAttachment;
        colorAttachment.format = s_swapFormat;
        colorAttachment.samples = vk::SampleCountFlagBits::e1;
        colorAttachment.loadOp = vk::AttachmentLoadOp::eLoad;
        colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
        colorAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        colorAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        colorAttachment.initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
        colorAttachment.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::AttachmentReference colorRef;
        colorRef.attachment = 0;
        colorRef.layout = vk::ImageLayout::eColorAttachmentOptimal;

        vk::SubpassDescription subpass;
        subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        vk::RenderPassCreateInfo rpci;
        rpci.attachmentCount = 1;
        rpci.pAttachments = &colorAttachment;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &subpass;
        s_overlayRenderPass = s_device.createRenderPass(rpci);

        s_overlayFramebuffers.resize(s_swapImageViews.size());
        for (size_t i = 0; i < s_swapImageViews.size(); ++i)
        {
            vk::ImageView attachment = s_swapImageViews[i];
            vk::FramebufferCreateInfo fbci;
            fbci.renderPass = s_overlayRenderPass;
            fbci.attachmentCount = 1;
            fbci.pAttachments = &attachment;
            fbci.width = s_swapExtent.width;
            fbci.height = s_swapExtent.height;
            fbci.layers = 1;
            s_overlayFramebuffers[i] = s_device.createFramebuffer(fbci);
        }
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay render target creation failed: %s", e.what());
        return false;
    }

    return true;
}

bool FindMemoryType(uint32_t typeBits, vk::MemoryPropertyFlags properties, uint32_t& typeIndex)
{
    vk::PhysicalDeviceMemoryProperties memoryProperties = s_gpu.getMemoryProperties();
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            typeIndex = i;
            return true;
        }
    }
    return false;
}

void DestroyOverlayTextureResource(OverlayTextureResource& texture, bool removeDescriptor)
{
    if (removeDescriptor && s_overlayReady && texture.descriptor != VK_NULL_HANDLE)
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
    texture.descriptor = VK_NULL_HANDLE;

    if (texture.sampler) s_device.destroySampler(texture.sampler);
    if (texture.view) s_device.destroyImageView(texture.view);
    if (texture.image) s_device.destroyImage(texture.image);
    if (texture.memory) s_device.freeMemory(texture.memory);
    texture = {};
}

void DestroyOverlayTextureResources()
{
    if (!s_device)
    {
        s_overlayTextures.clear();
        return;
    }

    try { s_device.waitIdle(); } catch (...) {}
    for (auto& texture : s_overlayTextures)
        DestroyOverlayTextureResource(texture, true);
    s_overlayTextures.clear();
}

void DestroyOverlayRenderTargets()
{
    for (auto& fb : s_overlayFramebuffers)
        if (fb) s_device.destroyFramebuffer(fb);
    s_overlayFramebuffers.clear();

    if (s_overlayRenderPass) s_device.destroyRenderPass(s_overlayRenderPass);
    s_overlayRenderPass = nullptr;
}

void ShutdownOverlayRendererInternal()
{
    DestroyOverlayTextureResources();

    if (s_overlayReady)
    {
        ImGui_ImplVulkan_Shutdown();
        s_overlayReady = false;
    }

    if (s_overlayDescriptorPool)
    {
        s_device.destroyDescriptorPool(s_overlayDescriptorPool);
        s_overlayDescriptorPool = nullptr;
    }

    s_overlayDrawData = nullptr;
}

// --- libretro Vulkan interface callbacks ---------------------------------

void RETRO_CALLCONV cb_set_image(void* /*handle*/,
                                 const retro_vulkan_image* image,
                                 uint32_t /*num_semaphores*/,
                                 const VkSemaphore* /*semaphores*/,
                                 uint32_t /*src_queue_family*/)
{
    if (!image)
        return;
    PerFrame& f = s_frames[s_currentFrame];
    f.image = *image;
    f.imageValid = true;
    s_lastImage = *image;
    s_lastImageValid = image->create_info.image != VK_NULL_HANDLE;
    s_lastImageFrame = s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index(void* /*handle*/)
{
    return s_currentFrame;
}

uint32_t RETRO_CALLCONV cb_get_sync_index_mask(void* /*handle*/)
{
    // Bitmask over all currently used sync indices.
    return s_frames.empty() ? 1u : ((1u << s_frames.size()) - 1u);
}

void RETRO_CALLCONV cb_set_command_buffers(void* /*handle*/,
                                           uint32_t num_cmd,
                                           const VkCommandBuffer* cmd)
{
    PerFrame& f = s_frames[s_currentFrame];
    f.coreCommandBuffers.assign(cmd, cmd + num_cmd);
}

void RETRO_CALLCONV cb_wait_sync_index(void* /*handle*/)
{
    if (s_frames.empty() || !s_device)
        return;
    PerFrame& f = s_frames[s_currentFrame];
    if (f.inflightFence)
        (void)s_device.waitForFences(f.inflightFence, VK_TRUE, UINT64_MAX);
}

void RETRO_CALLCONV cb_lock_queue(void* /*handle*/)
{
    s_queueMutex.lock();
}

void RETRO_CALLCONV cb_unlock_queue(void* /*handle*/)
{
    s_queueMutex.unlock();
}

void RETRO_CALLCONV cb_set_signal_semaphore(void* /*handle*/, VkSemaphore semaphore)
{
    PerFrame& f = s_frames[s_currentFrame];
    f.signalSemaphore = semaphore;
}

// --- Bringup -------------------------------------------------------------

bool CreateInstanceInternal()
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    if (!getInstProcAddr)
    {
        VK_LOG_ERROR("vk_icdGetInstanceProcAddr unavailable");
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstProcAddr);
#endif

    vk::ApplicationInfo appInfo("tico-flycast", 1, "Flycast", 1, VK_API_VERSION_1_1);

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(__SWITCH__) && defined(VK_NN_VI_SURFACE_EXTENSION_NAME)
        VK_NN_VI_SURFACE_EXTENSION_NAME,
#endif
    };

    vk::InstanceCreateInfo createInfo({}, &appInfo, nullptr, extensions);

    try
    {
        s_instance = vk::createInstance(createInfo);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateInstance failed: %s", e.what());
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VULKAN_HPP_DEFAULT_DISPATCHER.init(s_instance);
#endif

    VK_LOG_INFO("VkInstance created (%zu extensions)", extensions.size());
    return true;
}

bool CreateSurfaceInternal()
{
#if defined(__SWITCH__)
    // VK_NN_vi_surface — the Switch surface extension.
    vk::ViSurfaceCreateInfoNN createInfo;
    createInfo.window = s_nwindow ? s_nwindow : nwindowGetDefault();

    try
    {
        s_surface = s_instance.createViSurfaceNN(createInfo);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateViSurfaceNN failed: %s", e.what());
        return false;
    }
    VK_LOG_INFO("VkSurfaceKHR created via VI_NN");
    return true;
#else
    VK_LOG_ERROR("Surface creation not supported on this platform");
    return false;
#endif
}

bool CreateDeviceInternal()
{
    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();

    // Path A: core registered a negotiation interface — let it create the device.
    if (s_negIface && s_negIface->create_device)
    {
        retro_vulkan_context ctx{};
        const bool ok = s_negIface->create_device(&ctx,
                                                  static_cast<VkInstance>(s_instance),
                                                  VK_NULL_HANDLE,
                                                  static_cast<VkSurfaceKHR>(s_surface),
                                                  getInstProcAddr,
                                                  nullptr, 0,
                                                  nullptr, 0,
                                                  nullptr);
        if (!ok)
        {
            VK_LOG_ERROR("Core create_device returned false");
            return false;
        }

        s_gpu = vk::PhysicalDevice(ctx.gpu);
        s_device = vk::Device(ctx.device);
        s_queue = vk::Queue(ctx.queue);
        s_queueFamilyIndex = ctx.queue_family_index;

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
        VULKAN_HPP_DEFAULT_DISPATCHER.init(s_device);
#endif
        VK_LOG_INFO("Device created via core negotiation (qfi=%u)", s_queueFamilyIndex);
        return true;
    }

    // Path B: pick a GPU + create a device ourselves. Used if the core didn't
    // register a negotiation interface (shouldn't happen for flycast, but
    // libretro spec allows it).
    auto gpus = s_instance.enumeratePhysicalDevices();
    if (gpus.empty())
    {
        VK_LOG_ERROR("No Vulkan physical devices");
        return false;
    }
    s_gpu = gpus[0];
    for (const auto& g : gpus)
    {
        auto props = g.getProperties();
        if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
        {
            s_gpu = g;
            break;
        }
    }

    auto qprops = s_gpu.getQueueFamilyProperties();
    s_queueFamilyIndex = static_cast<uint32_t>(qprops.size());
    for (uint32_t i = 0; i < qprops.size(); ++i)
    {
        const bool gfx = (qprops[i].queueFlags & vk::QueueFlagBits::eGraphics) ==
                         vk::QueueFlagBits::eGraphics;
        const bool present = s_gpu.getSurfaceSupportKHR(i, s_surface);
        if (gfx && present)
        {
            s_queueFamilyIndex = i;
            break;
        }
    }
    if (s_queueFamilyIndex >= qprops.size())
    {
        VK_LOG_ERROR("No graphics+present queue family");
        return false;
    }

    float prio = 1.0f;
    vk::DeviceQueueCreateInfo qci({}, s_queueFamilyIndex, 1, &prio);
    std::vector<const char*> deviceExt = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vk::DeviceCreateInfo dci({}, qci, nullptr, deviceExt);

    try
    {
        s_device = s_gpu.createDevice(dci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateDevice failed: %s", e.what());
        return false;
    }

#if VULKAN_HPP_DISPATCH_LOADER_DYNAMIC == 1
    VULKAN_HPP_DEFAULT_DISPATCHER.init(s_device);
#endif
    s_queue = s_device.getQueue(s_queueFamilyIndex, 0);
    VK_LOG_INFO("Device created via fallback path (qfi=%u)", s_queueFamilyIndex);
    return true;
}

bool CreateSwapchainInternal()
{
    auto caps = s_gpu.getSurfaceCapabilitiesKHR(s_surface);
    auto formats = s_gpu.getSurfaceFormatsKHR(s_surface);
    auto modes = s_gpu.getSurfacePresentModesKHR(s_surface);

    if (formats.empty())
    {
        VK_LOG_ERROR("No surface formats");
        return false;
    }

    vk::SurfaceFormatKHR fmt = formats[0];
    for (const auto& f : formats)
    {
        if (f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm)
        {
            fmt = f;
            break;
        }
    }
    s_swapFormat = fmt.format;

    s_swapExtent = caps.currentExtent;
    if (s_swapExtent.width == UINT32_MAX)
    {
        // Switch nwindow always reports a real extent; this is a safety net.
        s_swapExtent.width = std::clamp<uint32_t>(1280, caps.minImageExtent.width, caps.maxImageExtent.width);
        s_swapExtent.height = std::clamp<uint32_t>(720, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    vk::PresentModeKHR mode = vk::PresentModeKHR::eFifo;  // vsync, always supported
    (void)modes;

    uint32_t imageCount = std::max<uint32_t>(3, caps.minImageCount);
    if (caps.maxImageCount != 0)
        imageCount = std::min(imageCount, caps.maxImageCount);

    vk::SwapchainCreateInfoKHR sci;
    sci.surface = s_surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = s_swapExtent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST so we can blit core's image onto it.
    sci.imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferDst;
    sci.imageSharingMode = vk::SharingMode::eExclusive;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;

    try
    {
        s_swapchain = s_device.createSwapchainKHR(sci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("vkCreateSwapchainKHR failed: %s", e.what());
        return false;
    }

    s_swapImages = s_device.getSwapchainImagesKHR(s_swapchain);
    s_swapImageViews.resize(s_swapImages.size());
    for (size_t i = 0; i < s_swapImages.size(); ++i)
    {
        vk::ImageViewCreateInfo vci;
        vci.image = s_swapImages[i];
        vci.viewType = vk::ImageViewType::e2D;
        vci.format = s_swapFormat;
        vci.components = vk::ComponentMapping();
        vci.subresourceRange = vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        s_swapImageViews[i] = s_device.createImageView(vci);
    }

    VK_LOG_INFO("Swapchain: %ux%u format=%d images=%zu",
                s_swapExtent.width, s_swapExtent.height,
                static_cast<int>(s_swapFormat), s_swapImages.size());
    return true;
}

bool CreateFrameResources()
{
    vk::CommandPoolCreateInfo cpci(vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
                                   s_queueFamilyIndex);
    s_commandPool = s_device.createCommandPool(cpci);

    const uint32_t frameCount = static_cast<uint32_t>(s_swapImages.size());
    s_frames.resize(frameCount);

    vk::CommandBufferAllocateInfo cbai(s_commandPool, vk::CommandBufferLevel::ePrimary, frameCount);
    auto cmds = s_device.allocateCommandBuffers(cbai);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        s_frames[i].cmd = cmds[i];
        s_frames[i].inflightFence = s_device.createFence(
            vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
        s_frames[i].acquireSemaphore = s_device.createSemaphore({});
        s_frames[i].renderSemaphore = s_device.createSemaphore({});
    }
    return true;
}

void PopulateHwInterface()
{
    s_hwIface.interface_type = RETRO_HW_RENDER_INTERFACE_VULKAN;
    s_hwIface.interface_version = RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION;
    s_hwIface.handle = nullptr;
    s_hwIface.instance = static_cast<VkInstance>(s_instance);
    s_hwIface.gpu = static_cast<VkPhysicalDevice>(s_gpu);
    s_hwIface.device = static_cast<VkDevice>(s_device);
    s_hwIface.queue = static_cast<VkQueue>(s_queue);
    s_hwIface.queue_index = s_queueFamilyIndex;

    PFN_vkGetInstanceProcAddr getInstProcAddr = GetInstanceProcAddrFunc();
    s_hwIface.get_instance_proc_addr = getInstProcAddr;
    s_hwIface.get_device_proc_addr =
        getInstProcAddr
            ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                  getInstProcAddr(static_cast<VkInstance>(s_instance), "vkGetDeviceProcAddr"))
            : nullptr;

    s_hwIface.set_image = &cb_set_image;
    s_hwIface.get_sync_index = &cb_get_sync_index;
    s_hwIface.get_sync_index_mask = &cb_get_sync_index_mask;
    s_hwIface.set_command_buffers = &cb_set_command_buffers;
    s_hwIface.wait_sync_index = &cb_wait_sync_index;
    s_hwIface.lock_queue = &cb_lock_queue;
    s_hwIface.unlock_queue = &cb_unlock_queue;
    s_hwIface.set_signal_semaphore = &cb_set_signal_semaphore;
}

}  // namespace

// =========================================================================
// Public API
// =========================================================================

bool CreateInstance()
{
#ifdef __SWITCH__
    s_nwindow = nwindowGetDefault();
#endif

    if (!CreateInstanceInternal())
        return false;
    if (!CreateSurfaceInternal())
        return false;
    return true;
}

bool CreateDeviceAndSwapchain()
{
    if (!CreateDeviceInternal())
        return false;
    if (!CreateSwapchainInternal())
        return false;
    if (!CreateOverlayRenderTargets())
        return false;
    if (!CreateFrameResources())
        return false;
    PopulateHwInterface();
    s_ready = true;
    return true;
}

void Shutdown()
{
    if (s_device)
    {
        try { s_device.waitIdle(); } catch (...) {}
    }

    ShutdownOverlayRendererInternal();

    if (s_negIface && s_negIface->destroy_device)
        s_negIface->destroy_device();
    s_negIface = nullptr;

    for (auto& f : s_frames)
    {
        if (f.inflightFence)   s_device.destroyFence(f.inflightFence);
        if (f.acquireSemaphore) s_device.destroySemaphore(f.acquireSemaphore);
        if (f.renderSemaphore)  s_device.destroySemaphore(f.renderSemaphore);
    }
    s_frames.clear();

    if (s_commandPool) s_device.destroyCommandPool(s_commandPool);
    s_commandPool = nullptr;

    DestroyOverlayRenderTargets();

    for (auto& v : s_swapImageViews)
        if (v) s_device.destroyImageView(v);
    s_swapImageViews.clear();
    s_swapImages.clear();

    if (s_swapchain) s_device.destroySwapchainKHR(s_swapchain);
    s_swapchain = nullptr;

    if (s_device) s_device.destroy();
    s_device = nullptr;

    if (s_surface) s_instance.destroySurfaceKHR(s_surface);
    s_surface = nullptr;

    if (s_instance) s_instance.destroy();
    s_instance = nullptr;

    s_ready = false;
    s_frameInFlight = false;
    s_lastImageValid = false;
}

bool BeginFrame()
{
    if (!s_ready)
        return false;

    // s_currentFrame is the rotating frame slot — it owns our per-frame sync
    // (fence + acquire semaphore + render semaphore + cmd buffer) AND it's
    // what we return to the libretro core via get_sync_index. The acquired
    // swap image index is independent and tracked in s_currentImage.
    PerFrame& f = s_frames[s_currentFrame];

    // Wait for the previous use of this frame slot to complete on the GPU.
    (void)s_device.waitForFences(f.inflightFence, VK_TRUE, UINT64_MAX);
    s_device.resetFences(f.inflightFence);

    try
    {
        auto rv = s_device.acquireNextImageKHR(s_swapchain, UINT64_MAX,
                                               f.acquireSemaphore, VK_NULL_HANDLE);
        s_currentImage = rv.value;
        if (rv.result == vk::Result::eSuboptimalKHR)
            VK_LOG_WARN("acquireNextImage suboptimal");
    }
    catch (const vk::OutOfDateKHRError&)
    {
        VK_LOG_WARN("acquireNextImage OUT_OF_DATE — TODO recreate swapchain");
        return false;
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("acquireNextImage failed: %s", e.what());
        return false;
    }

    f.imageValid = false;
    f.coreCommandBuffers.clear();
    f.signalSemaphore = VK_NULL_HANDLE;

    // Begin our cmd buffer for the post-core composite + present transitions.
    f.cmd.reset();
    f.cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    s_frameInFlight = true;
    return true;
}

void EndFrame()
{
    if (!s_frameInFlight)
        return;

    PerFrame& f = s_frames[s_currentFrame];
    vk::Image swapImage = s_swapImages[s_currentImage];

    // Transition swap image to TRANSFER_DST regardless — even if the core
    // didn't produce an image, we still clear-and-present so the screen
    // doesn't deadlock waiting on us.
    TransitionLayout(f.cmd, swapImage,
                     vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                     {}, vk::AccessFlagBits::eTransferWrite,
                     vk::PipelineStageFlagBits::eTopOfPipe,
                     vk::PipelineStageFlagBits::eTransfer);

    const retro_vulkan_image* sourceImage = nullptr;
    bool reusingLastImage = false;
    if (f.imageValid && f.image.create_info.image != VK_NULL_HANDLE)
    {
        sourceImage = &f.image;
    }
    else if (s_lastImageValid && s_lastImage.create_info.image != VK_NULL_HANDLE)
    {
        sourceImage = &s_lastImage;
        reusingLastImage = true;
    }

    if (sourceImage)
    {
        if (reusingLastImage && s_lastImageFrame < s_frames.size() &&
            s_lastImageFrame != s_currentFrame && s_frames[s_lastImageFrame].inflightFence)
        {
            (void)s_device.waitForFences(s_frames[s_lastImageFrame].inflightFence,
                                         VK_TRUE, UINT64_MAX);
        }

        vk::Image coreImage(sourceImage->create_info.image);
        vk::ImageLayout coreLayout = static_cast<vk::ImageLayout>(sourceImage->image_layout);

        // Bring core's image into TRANSFER_SRC. Core writes to it via the
        // graphics pipeline (color attachment + final-layout transition to
        // SHADER_READ_ONLY_OPTIMAL) on a separate submit; we wait on
        // ALL_COMMANDS / MEMORY_READ|WRITE so anything it did is visible.
        TransitionLayout(f.cmd, coreImage,
                         coreLayout, vk::ImageLayout::eTransferSrcOptimal,
                         vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                         vk::AccessFlagBits::eTransferRead,
                         vk::PipelineStageFlagBits::eAllCommands,
                         vk::PipelineStageFlagBits::eTransfer);

        // Blit core's image → swap image. Image-to-image blit handles
        // R8G8B8A8 ↔ B8G8R8A8 swizzles automatically. Source extent comes
        // from the most recent video_refresh callback (retro_vulkan_image
        // itself doesn't carry it).
        const uint32_t srcW = s_sourceExtent.width  ? s_sourceExtent.width  : s_swapExtent.width;
        const uint32_t srcH = s_sourceExtent.height ? s_sourceExtent.height : s_swapExtent.height;

        // Destination rect: the overlay's screen-size/display-mode selection,
        // or the full swapchain when none is set. When it doesn't cover the
        // whole image we clear to black first so the border bars are clean.
        int32_t dstX0 = 0, dstY0 = 0;
        int32_t dstX1 = static_cast<int32_t>(s_swapExtent.width);
        int32_t dstY1 = static_cast<int32_t>(s_swapExtent.height);
        const bool letterbox =
            s_gameViewport.extent.width > 0 && s_gameViewport.extent.height > 0 &&
            (s_gameViewport.offset.x != 0 || s_gameViewport.offset.y != 0 ||
             s_gameViewport.extent.width != s_swapExtent.width ||
             s_gameViewport.extent.height != s_swapExtent.height);
        if (letterbox)
        {
            dstX0 = s_gameViewport.offset.x;
            dstY0 = s_gameViewport.offset.y;
            dstX1 = dstX0 + static_cast<int32_t>(s_gameViewport.extent.width);
            dstY1 = dstY0 + static_cast<int32_t>(s_gameViewport.extent.height);

            vk::ClearColorValue clear(std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
            vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
            f.cmd.clearColorImage(swapImage, vk::ImageLayout::eTransferDstOptimal, clear, range);

            // The clear writes the whole image and the blit overwrites the
            // inner rect — order the two transfer writes.
            vk::MemoryBarrier mb(vk::AccessFlagBits::eTransferWrite,
                                 vk::AccessFlagBits::eTransferWrite);
            f.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                  vk::PipelineStageFlagBits::eTransfer,
                                  {}, mb, {}, {});
        }

        vk::ImageBlit blit;
        blit.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        blit.srcSubresource.mipLevel = 0;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.srcOffsets[1] = vk::Offset3D(static_cast<int32_t>(srcW),
                                          static_cast<int32_t>(srcH), 1);
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0] = vk::Offset3D(dstX0, dstY0, 0);
        blit.dstOffsets[1] = vk::Offset3D(dstX1, dstY1, 1);

        f.cmd.blitImage(coreImage, vk::ImageLayout::eTransferSrcOptimal,
                        swapImage, vk::ImageLayout::eTransferDstOptimal,
                        blit, vk::Filter::eLinear);

        // Restore core's image layout so the core can keep using it.
        TransitionLayout(f.cmd, coreImage,
                         vk::ImageLayout::eTransferSrcOptimal, coreLayout,
                         vk::AccessFlagBits::eTransferRead,
                         vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eAllCommands);
    }
    else
    {
        // Core didn't produce a frame: clear to black so the swap image is valid.
        vk::ClearColorValue clear(std::array<float, 4>{0.f, 0.f, 0.f, 1.f});
        vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        f.cmd.clearColorImage(swapImage, vk::ImageLayout::eTransferDstOptimal, clear, range);
    }

    if (s_overlayReady && s_overlayDrawData && s_overlayDrawData->TotalVtxCount > 0 &&
        s_currentImage < s_overlayFramebuffers.size())
    {
        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eColorAttachmentOptimal,
                         vk::AccessFlagBits::eTransferWrite,
                         vk::AccessFlagBits::eColorAttachmentWrite,
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eColorAttachmentOutput);

        vk::RenderPassBeginInfo rpbi;
        rpbi.renderPass = s_overlayRenderPass;
        rpbi.framebuffer = s_overlayFramebuffers[s_currentImage];
        rpbi.renderArea.offset = vk::Offset2D(0, 0);
        rpbi.renderArea.extent = s_swapExtent;
        f.cmd.beginRenderPass(rpbi, vk::SubpassContents::eInline);
        ImGui_ImplVulkan_RenderDrawData(s_overlayDrawData, static_cast<VkCommandBuffer>(f.cmd));
        f.cmd.endRenderPass();

        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eColorAttachmentOptimal,
                         vk::ImageLayout::ePresentSrcKHR,
                         vk::AccessFlagBits::eColorAttachmentWrite, {},
                         vk::PipelineStageFlagBits::eColorAttachmentOutput,
                         vk::PipelineStageFlagBits::eBottomOfPipe);
    }
    else
    {
        // Transition swap image to PRESENT_SRC.
        TransitionLayout(f.cmd, swapImage,
                         vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::ePresentSrcKHR,
                         vk::AccessFlagBits::eTransferWrite, {},
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eBottomOfPipe);
    }
    s_overlayDrawData = nullptr;

    f.cmd.end();

    // Submit: any core-supplied cmd buffers run first, then ours.
    std::vector<vk::CommandBuffer> submitCmds;
    submitCmds.reserve(f.coreCommandBuffers.size() + 1);
    for (auto& cb : f.coreCommandBuffers)
        submitCmds.push_back(cb);
    submitCmds.push_back(f.cmd);

    vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;

    std::vector<vk::Semaphore> signalSems = {f.renderSemaphore};
    if (f.signalSemaphore != VK_NULL_HANDLE)
        signalSems.push_back(vk::Semaphore(f.signalSemaphore));

    vk::SubmitInfo submit;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &f.acquireSemaphore;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = static_cast<uint32_t>(submitCmds.size());
    submit.pCommandBuffers = submitCmds.data();
    submit.signalSemaphoreCount = static_cast<uint32_t>(signalSems.size());
    submit.pSignalSemaphores = signalSems.data();

    {
        std::lock_guard<std::mutex> guard(s_queueMutex);
        s_queue.submit(submit, f.inflightFence);
    }

    // Present the acquired swap image (index returned by acquireNextImageKHR,
    // not the rotating frame slot).
    vk::PresentInfoKHR present;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &f.renderSemaphore;
    present.swapchainCount = 1;
    present.pSwapchains = &s_swapchain;
    present.pImageIndices = &s_currentImage;

    try
    {
        std::lock_guard<std::mutex> guard(s_queueMutex);
        (void)s_queue.presentKHR(present);
    }
    catch (const vk::OutOfDateKHRError&)
    {
        VK_LOG_WARN("presentKHR OUT_OF_DATE — TODO recreate swapchain");
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("presentKHR failed: %s", e.what());
    }

    s_frameInFlight = false;
    s_currentFrame = (s_currentFrame + 1) % static_cast<uint32_t>(s_frames.size());
}

bool IsFrameInFlight() { return s_frameInFlight; }
bool IsReady()         { return s_ready; }

bool InitOverlayRenderer()
{
    if (s_overlayReady)
        return true;
    if (!s_ready || !s_device || !s_overlayRenderPass)
    {
        VK_LOG_WARN("Overlay renderer init skipped: Vulkan not ready");
        return false;
    }

    try
    {
        vk::DescriptorPoolSize poolSize(vk::DescriptorType::eCombinedImageSampler, 64);
        vk::DescriptorPoolCreateInfo dpci;
        dpci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        dpci.maxSets = 64;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        s_overlayDescriptorPool = s_device.createDescriptorPool(dpci);
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay descriptor pool creation failed: %s", e.what());
        return false;
    }

#ifdef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    if (!ImGui_ImplVulkan_LoadFunctions(ImGuiVulkanLoader, nullptr))
    {
        VK_LOG_ERROR("ImGui Vulkan function loading failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
#endif

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = static_cast<VkInstance>(s_instance);
    info.PhysicalDevice = static_cast<VkPhysicalDevice>(s_gpu);
    info.Device = static_cast<VkDevice>(s_device);
    info.QueueFamily = s_queueFamilyIndex;
    info.Queue = static_cast<VkQueue>(s_queue);
    info.DescriptorPool = static_cast<VkDescriptorPool>(s_overlayDescriptorPool);
    info.RenderPass = static_cast<VkRenderPass>(s_overlayRenderPass);
    info.MinImageCount = 2;
    info.ImageCount = static_cast<uint32_t>(s_swapImages.size());
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&info))
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_Init failed");
        ShutdownOverlayRendererInternal();
        return false;
    }
    s_overlayReady = true;

    if (!ImGui_ImplVulkan_CreateFontsTexture())
    {
        VK_LOG_ERROR("ImGui_ImplVulkan_CreateFontsTexture failed");
        ShutdownOverlayRendererInternal();
        return false;
    }

    VK_LOG_INFO("Overlay renderer initialized");
    return true;
}

void ShutdownOverlayRenderer()
{
    if (s_device)
        try { s_device.waitIdle(); } catch (...) {}
    ShutdownOverlayRendererInternal();
}

void BeginOverlayFrame()
{
    if (s_overlayReady)
        ImGui_ImplVulkan_NewFrame();
}

void SetOverlayDrawData(ImDrawData* drawData)
{
    s_overlayDrawData = drawData;
}

ImTextureID CreateOverlayTextureRGBA(const unsigned char* rgba, uint32_t width, uint32_t height)
{
    if (!s_overlayReady || !s_device || !s_commandPool || !rgba || width == 0 || height == 0)
        return 0;

    const vk::DeviceSize uploadSize = static_cast<vk::DeviceSize>(width) *
                                      static_cast<vk::DeviceSize>(height) * 4;
    OverlayTextureResource texture{};
    vk::Buffer stagingBuffer;
    vk::DeviceMemory stagingMemory;
    vk::CommandBuffer commandBuffer;

    try
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo.size = uploadSize;
        bufferInfo.usage = vk::BufferUsageFlagBits::eTransferSrc;
        bufferInfo.sharingMode = vk::SharingMode::eExclusive;
        stagingBuffer = s_device.createBuffer(bufferInfo);

        vk::MemoryRequirements bufferReq = s_device.getBufferMemoryRequirements(stagingBuffer);
        uint32_t bufferMemoryType = 0;
        if (!FindMemoryType(bufferReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible |
                            vk::MemoryPropertyFlagBits::eHostCoherent,
                            bufferMemoryType) &&
            !FindMemoryType(bufferReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eHostVisible,
                            bufferMemoryType))
        {
            VK_LOG_ERROR("No host-visible memory type for overlay texture upload");
            throw std::runtime_error("overlay texture staging memory");
        }

        vk::MemoryAllocateInfo bufferAlloc;
        bufferAlloc.allocationSize = bufferReq.size;
        bufferAlloc.memoryTypeIndex = bufferMemoryType;
        stagingMemory = s_device.allocateMemory(bufferAlloc);
        s_device.bindBufferMemory(stagingBuffer, stagingMemory, 0);

        void* mapped = s_device.mapMemory(stagingMemory, 0, uploadSize);
        std::memcpy(mapped, rgba, static_cast<size_t>(uploadSize));
        vk::MappedMemoryRange flushRange;
        flushRange.memory = stagingMemory;
        flushRange.size = uploadSize;
        s_device.flushMappedMemoryRanges(flushRange);
        s_device.unmapMemory(stagingMemory);

        vk::ImageCreateInfo imageInfo;
        imageInfo.imageType = vk::ImageType::e2D;
        imageInfo.format = vk::Format::eR8G8B8A8Unorm;
        imageInfo.extent = vk::Extent3D(width, height, 1);
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = vk::SampleCountFlagBits::e1;
        imageInfo.tiling = vk::ImageTiling::eOptimal;
        imageInfo.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
        imageInfo.sharingMode = vk::SharingMode::eExclusive;
        imageInfo.initialLayout = vk::ImageLayout::eUndefined;
        texture.image = s_device.createImage(imageInfo);

        vk::MemoryRequirements imageReq = s_device.getImageMemoryRequirements(texture.image);
        uint32_t imageMemoryType = 0;
        if (!FindMemoryType(imageReq.memoryTypeBits,
                            vk::MemoryPropertyFlagBits::eDeviceLocal,
                            imageMemoryType))
        {
            VK_LOG_ERROR("No device-local memory type for overlay texture");
            throw std::runtime_error("overlay texture image memory");
        }

        vk::MemoryAllocateInfo imageAlloc;
        imageAlloc.allocationSize = imageReq.size;
        imageAlloc.memoryTypeIndex = imageMemoryType;
        texture.memory = s_device.allocateMemory(imageAlloc);
        s_device.bindImageMemory(texture.image, texture.memory, 0);

        vk::ImageViewCreateInfo viewInfo;
        viewInfo.image = texture.image;
        viewInfo.viewType = vk::ImageViewType::e2D;
        viewInfo.format = vk::Format::eR8G8B8A8Unorm;
        viewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        texture.view = s_device.createImageView(viewInfo);

        vk::SamplerCreateInfo samplerInfo;
        samplerInfo.magFilter = vk::Filter::eLinear;
        samplerInfo.minFilter = vk::Filter::eLinear;
        samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
        samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = 0.0f;
        texture.sampler = s_device.createSampler(samplerInfo);

        vk::CommandBufferAllocateInfo commandAlloc(s_commandPool, vk::CommandBufferLevel::ePrimary, 1);
        commandBuffer = s_device.allocateCommandBuffers(commandAlloc)[0];
        commandBuffer.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

        TransitionLayout(commandBuffer, texture.image,
                         vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
                         {}, vk::AccessFlagBits::eTransferWrite,
                         vk::PipelineStageFlagBits::eTopOfPipe,
                         vk::PipelineStageFlagBits::eTransfer);

        vk::BufferImageCopy region;
        region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = vk::Extent3D(width, height, 1);
        commandBuffer.copyBufferToImage(stagingBuffer, texture.image,
                                        vk::ImageLayout::eTransferDstOptimal,
                                        region);

        TransitionLayout(commandBuffer, texture.image,
                         vk::ImageLayout::eTransferDstOptimal,
                         vk::ImageLayout::eShaderReadOnlyOptimal,
                         vk::AccessFlagBits::eTransferWrite,
                         vk::AccessFlagBits::eShaderRead,
                         vk::PipelineStageFlagBits::eTransfer,
                         vk::PipelineStageFlagBits::eFragmentShader);

        commandBuffer.end();

        vk::SubmitInfo submit;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        {
            std::lock_guard<std::mutex> guard(s_queueMutex);
            s_queue.submit(submit, VK_NULL_HANDLE);
            s_queue.waitIdle();
        }

        s_device.freeCommandBuffers(s_commandPool, commandBuffer);
        commandBuffer = nullptr;
        s_device.destroyBuffer(stagingBuffer);
        stagingBuffer = nullptr;
        s_device.freeMemory(stagingMemory);
        stagingMemory = nullptr;

        texture.descriptor = ImGui_ImplVulkan_AddTexture(static_cast<VkSampler>(texture.sampler),
                                                         static_cast<VkImageView>(texture.view),
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        s_overlayTextures.push_back(texture);
        return (ImTextureID)texture.descriptor;
    }
    catch (const vk::SystemError& e)
    {
        VK_LOG_ERROR("Overlay texture creation failed: %s", e.what());
    }
    catch (const std::exception& e)
    {
        VK_LOG_ERROR("Overlay texture creation failed: %s", e.what());
    }

    if (commandBuffer) s_device.freeCommandBuffers(s_commandPool, commandBuffer);
    if (stagingBuffer) s_device.destroyBuffer(stagingBuffer);
    if (stagingMemory) s_device.freeMemory(stagingMemory);
    DestroyOverlayTextureResource(texture, false);
    return 0;
}

void DestroyOverlayTexture(ImTextureID textureId)
{
    if (!textureId || !s_device)
        return;

    VkDescriptorSet descriptor = (VkDescriptorSet)textureId;
    auto it = std::find_if(s_overlayTextures.begin(), s_overlayTextures.end(),
                           [descriptor](const OverlayTextureResource& texture) {
                               return texture.descriptor == descriptor;
                           });
    if (it == s_overlayTextures.end())
        return;

    try { s_device.waitIdle(); } catch (...) {}
    DestroyOverlayTextureResource(*it, true);
    s_overlayTextures.erase(it);
}

const retro_hw_render_interface_vulkan* GetHwRenderInterface()
{
    return s_ready ? &s_hwIface : nullptr;
}

void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan* iface)
{
    s_negIface = iface;
}

void GetSwapExtent(uint32_t& width, uint32_t& height)
{
    width = s_swapExtent.width;
    height = s_swapExtent.height;
}

void SetSourceExtent(uint32_t width, uint32_t height)
{
    s_sourceExtent.width = width;
    s_sourceExtent.height = height;
}

void SetGameViewport(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        s_gameViewport = vk::Rect2D{{0, 0}, {0, 0}};  // 0 extent => full swapchain
    else
        s_gameViewport = vk::Rect2D{{x, y}, {static_cast<uint32_t>(width),
                                             static_cast<uint32_t>(height)}};
}

}  // namespace TicoVulkan
