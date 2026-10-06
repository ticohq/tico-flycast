/// @file TicoLsfg.h
/// @brief Frame generation (LSFG) for a tico Vulkan frontend.
///
/// Lossless Scaling's shaders, read from the user's own Lossless.dll
/// (tico/deps/LSFG-VK), put a generated frame between two of the game's, so a
/// 30 fps game shows 60. Only presenting changes; a game already at 50/60 fps
/// is left alone. Everything runs on the thread that presents, with the queue
/// lock held.
#pragma once

#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <vector>

namespace TicoLsfg
{

/// Where the user puts Lossless.dll (never shipped); Dolphin's copy is used
/// when tico's is missing.
constexpr const char *kDllPath = "sdmc:/tico/system/lsfg/Lossless.dll";
constexpr const char *kDolphinDllPath = "sdmc:/switch/dolphin/lsfg/Lossless.dll";

/// The core's settings: on/off, the motion estimation scale (0.25 or 0.5 of
/// the screen) and the lighter "performance" shaders. Read at every present.
void SetOptions(bool enabled, float flowScale, bool performanceMode);
/// On and usable: the game's repeated frames are then not presented, so a
/// 30 fps game hands frame generation 30 distinct frames.
bool SkipsRepeatedFrames();

/// Before the device and swapchain exist: true when Lossless.dll is installed;
/// they are then made ready for it (timeline semaphores, transfer usage, two
/// more images), so it can be switched on in game.
bool Prepare();
bool IsPrepared();
/// The device or swapchain cannot run it: presenting goes on as usual.
void Disable(const char *reason);

struct Device
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
};
void RegisterSwapchain(const Device &device, VkSwapchainKHR swapchain, VkExtent2D extent,
                       const std::vector<VkImage> &images);
void UnregisterSwapchain();

/// In place of vkQueuePresentKHR: true when it presented (the result in
/// `result`); false when the caller presents as usual.
bool Present(VkQueue queue, const VkPresentInfoKHR &info, VkResult &result);

} // namespace TicoLsfg
