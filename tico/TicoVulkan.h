/// @file TicoVulkan.h
/// @brief Vulkan frontend that backs the libretro hw_render interface.
///
/// Owns the VkInstance/VkDevice/VkSurface/swapchain. Flycast's libretro core
/// renders into images we hand it via `set_image`; per frame we blit that
/// image onto the current swapchain image and present.
#pragma once

#include <vector>

#include "imgui.h"

#include <vulkan/vulkan.hpp>
#include <libretro_vulkan.h>

#include <array>
#include <functional>
#include <mutex>

struct ImDrawData;

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace TicoVulkan
{

// Bring up VkInstance + VkSurface. Must be called BEFORE retro_init() so the
// negotiation callback registered via SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE
// can use our instance/surface to pick a queue family.
bool CreateInstance();

// Negotiate the device with the core (via the negotiation interface) and bring
// up the swapchain. Must be called AFTER retro_set_environment() so the core
// has had a chance to register its negotiation callbacks.
bool CreateDeviceAndSwapchain();

// Tear everything down. Safe to call from anywhere.
void Shutdown();

// Acquire the next swapchain image. Returns true on success; on false the
// caller should skip the frame (typically after OOD swapchain — recreated
// internally on the next call).
bool BeginFrame();

// Composite the core's `set_image` result onto the current swap image,
// submit, and present. Always paired with BeginFrame().
void EndFrame();

// True between BeginFrame() and EndFrame(). retro_run() must run inside.
bool IsFrameInFlight();

// Filled in once the device is up. Pass to the core via
// RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE.
const retro_hw_render_interface_vulkan* GetHwRenderInterface();

// Stored from RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE.
void SetNegotiationInterface(const retro_hw_render_context_negotiation_interface_vulkan* iface);

// Currently presented size (in swapchain pixels). 0/0 if not initialised.
void GetSwapExtent(uint32_t& width, uint32_t& height);

// Tell the frontend the size of the next image the core will hand it via
// set_image. retro_vulkan_image doesn't carry the source extent, so we have
// to take it from retro_video_refresh_t (called right after set_image).
void SetSourceExtent(uint32_t width, uint32_t height);

// Destination rect (in swapchain pixels) the game image is blitted into. The
// area outside it is cleared to black (letterbox/pillarbox). Pass w<=0 or h<=0
// to fill the whole swapchain (the default). Lets the overlay's screen-size /
// display-mode selection actually affect the presented game.
void SetGameViewport(int x, int y, int width, int height);

// True once the device + swapchain are ready (post-CreateDeviceAndSwapchain).
bool IsReady();

// ImGui/Tico overlay renderer. Init must be called after an ImGui context
// exists and after CreateDeviceAndSwapchain().
bool InitOverlayRenderer();
void ShutdownOverlayRenderer();
void BeginOverlayFrame();
void SetOverlayDrawData(ImDrawData* drawData);
ImTextureID CreateOverlayTextureRGBA(const unsigned char* rgba, uint32_t width, uint32_t height);
void DestroyOverlayTexture(ImTextureID texture);

/// The core's last frame, shrunk to fit maxWidth x maxHeight (aspect kept),
/// as tightly packed RGBA. False when there is no frame yet.
bool CaptureGameImage(uint32_t maxWidth, uint32_t maxHeight, std::vector<uint8_t>& rgba,
                      uint32_t& width, uint32_t& height);

// --- For the shader chain (TicoShaderChain/TicoSlang, shared with the other
// cores' frontends): plain-Vulkan helpers on this device and queue. ---

/// Upper bound on frame slots (one per swapchain image here); per-frame
/// resources are allocated this many times and indexed by FrameIndex().
constexpr uint32_t kFramesInFlight = 4;

struct Context
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceMemoryProperties memProps = {};
};
const Context& Ctx();

/// Slot of the frame being recorded, 0..kFramesInFlight-1.
uint32_t FrameIndex();

/// Run `fn` once the GPU can no longer be using what it frees.
void DeferDestroy(std::function<void()> fn);
void WaitIdle();

/// Submit one-shot work synchronously (uploads outside the frame).
VkCommandBuffer BeginOneShot();
void EndOneShot(VkCommandBuffer cmd);

/// Layout transition over the whole colour image.
void TransitionImage(VkCommandBuffer cmd, VkImage image, uint32_t mipLevels,
                     VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                     VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

/// A sampled 2D colour image with memory and view.
struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
/// `swizzleAlphaOne` forces alpha to 1 in the view, for XRGB/RGB formats.
bool CreateImage(Image& out, uint32_t width, uint32_t height, VkFormat format,
                 VkImageUsageFlags usage, uint32_t mipLevels = 1, bool swizzleAlphaOne = false);
void DestroyImage(Image& img);
void DeferDestroyImage(Image& img);

/// Host-visible, coherent buffer.
struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};
bool CreateBuffer(Buffer& out, VkDeviceSize size, VkBufferUsageFlags usage);
void DestroyBuffer(Buffer& buf);
void DeferDestroyBuffer(Buffer& buf);

/// The chain's output goes to the screen by blit here, not through ImGui:
/// these only keep its interface (no texture is registered).
ImTextureID RegisterImage(VkImageView view);
void UnregisterImage(ImTextureID tex);

/// Optional pass over the game image before it reaches the screen (a shader
/// preset): given the core's image (in `layout`, to be left in it) and the
/// size it is shown at, returns an image of that size in
/// SHADER_READ_ONLY_OPTIMAL, or null to show the core's image as it is.
using GameFilter = std::function<const Image*(VkCommandBuffer cmd, VkImage image, VkImageLayout layout,
                                              uint32_t srcWidth, uint32_t srcHeight,
                                              uint32_t dstWidth, uint32_t dstHeight)>;
void SetGameFilter(GameFilter filter);

}  // namespace TicoVulkan
