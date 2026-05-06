/// @file TicoVulkan.h
/// @brief Vulkan frontend that backs the libretro hw_render interface.
///
/// Owns the VkInstance/VkDevice/VkSurface/swapchain. Flycast's libretro core
/// renders into images we hand it via `set_image`; per frame we blit that
/// image onto the current swapchain image and present.
#pragma once

#include "imgui.h"

#include <vulkan/vulkan.hpp>
#include <libretro_vulkan.h>

#include <array>
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

}  // namespace TicoVulkan
