/// @file TicoLsfg.cpp
/// @brief Frame generation (LSFG) for a tico Vulkan frontend. See TicoLsfg.h.

#include "TicoLsfg.h"
#include "TicoLogger.h"

#include "lsfg_bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>

#define LSFG_TAG "LSFG"

namespace TicoLsfg
{
namespace
{

constexpr const char *kPipelineCachePath = "sdmc:/tico/cache/lsfg-pipeline-cache.bin";

// The game's own frame rate decides: frames are generated below ~40 fps and
// stop above ~50 fps (a 60 fps game, or the paused menu), with room between
// the two so a rate near the edge does not flip every frame.
constexpr double kGenerateAboveMs = 25.0;
constexpr double kPassBelowMs = 20.0;
constexpr uint32_t kMinSamples = 8;

std::atomic_bool s_enabled{false};
std::atomic<float> s_flowScale{0.25f};
std::atomic_bool s_performanceMode{true};

bool s_prepared = false;
std::string s_dllPath;

Device s_device;
VkSwapchainKHR s_swapchain = VK_NULL_HANDLE;
VkExtent2D s_extent{};
std::vector<VkImage> s_images;

LsfgNxRuntime *s_runtime = nullptr;
bool s_runtimeFailed = false; // not tried again until the swapchain or the options change
float s_runtimeFlowScale = 0.0f;
bool s_runtimePerformanceMode = false;

std::chrono::steady_clock::time_point s_lastPresent{};
bool s_havePresent = false;
double s_intervalMs = 0.0;
uint32_t s_samples = 0;
bool s_generating = false;

bool FileExists(const char *path)
{
    if (FILE *f = std::fopen(path, "rb"))
    {
        std::fclose(f);
        return true;
    }
    return false;
}

void DestroyRuntime()
{
    if (!s_runtime)
        return;
    lsfg_nx_destroy(s_runtime);
    s_runtime = nullptr;
}

void ResetRate()
{
    s_havePresent = false;
    s_intervalMs = 0.0;
    s_samples = 0;
    s_generating = false;
}

// One of the game's frames is being presented: follow its rate.
void ObserveFrame()
{
    const auto now = std::chrono::steady_clock::now();
    if (s_havePresent)
    {
        const double interval = std::chrono::duration<double, std::milli>(now - s_lastPresent).count();
        if (interval >= 4.0 && interval <= 100.0)
        {
            s_intervalMs = s_samples == 0 ? interval : s_intervalMs * 0.875 + interval * 0.125;
            if (s_samples < kMinSamples)
                s_samples++;
        }
    }
    s_lastPresent = now;
    s_havePresent = true;

    if (s_samples < kMinSamples)
        s_generating = false;
    else if (!s_generating && s_intervalMs >= kGenerateAboveMs)
        s_generating = true;
    else if (s_generating && s_intervalMs < kPassBelowMs)
        s_generating = false;
}

bool EnsureRuntime(VkQueue queue)
{
    const float flowScale = s_flowScale.load(std::memory_order_acquire);
    const bool performanceMode = s_performanceMode.load(std::memory_order_acquire);
    if (s_runtime && (flowScale != s_runtimeFlowScale || performanceMode != s_runtimePerformanceMode))
    {
        DestroyRuntime();
        s_runtimeFailed = false;
    }
    if (s_runtime)
        return true;
    if (s_runtimeFailed)
        return false;

    LsfgNxCreateInfo info{};
    info.instance = s_device.instance;
    info.physical_device = s_device.gpu;
    info.device = s_device.device;
    info.queue = queue;
    info.queue_family_index = s_device.queueFamily;
    info.get_instance_proc_addr = s_device.getInstanceProcAddr;
    info.swapchain = s_swapchain;
    info.extent = s_extent;
    info.swapchain_images = s_images.data();
    info.swapchain_image_count = static_cast<uint32_t>(s_images.size());
    info.shader_dll_path = s_dllPath.c_str();
    info.pipeline_cache_path = kPipelineCachePath;
    info.flow_scale = flowScale;
    info.performance_mode = performanceMode;
    s_runtime = lsfg_nx_create(&info);
    s_runtimeFlowScale = flowScale;
    s_runtimePerformanceMode = performanceMode;
    if (!s_runtime)
    {
        LOG_ERROR(LSFG_TAG, "Cannot start frame generation with %s (a Lossless Scaling DLL it can read?)",
                  s_dllPath.c_str());
        s_runtimeFailed = true;
        return false;
    }

    LOG_INFO(LSFG_TAG, "Frame generation started (%ux%u, flow scale %.2f, %s mode)", s_extent.width,
             s_extent.height, flowScale, performanceMode ? "performance" : "quality");
    return true;
}

} // namespace

void SetOptions(bool enabled, float flowScale, bool performanceMode)
{
    s_enabled.store(enabled, std::memory_order_release);
    s_flowScale.store(flowScale == 0.5f ? 0.5f : 0.25f, std::memory_order_release);
    s_performanceMode.store(performanceMode, std::memory_order_release);
}

bool SkipsRepeatedFrames()
{
    return s_prepared && s_swapchain != VK_NULL_HANDLE && !s_runtimeFailed &&
           s_enabled.load(std::memory_order_acquire);
}

bool Prepare()
{
    s_prepared = false;
    s_dllPath.clear();
    for (const char *path : {kDllPath, kDolphinDllPath})
    {
        if (FileExists(path))
        {
            s_dllPath = path;
            break;
        }
    }
    if (s_dllPath.empty())
    {
        LOG_INFO(LSFG_TAG, "No Lossless.dll in %s, frame generation is unavailable", kDllPath);
        return false;
    }

    LOG_INFO(LSFG_TAG, "Using %s", s_dllPath.c_str());
    s_prepared = true;
    return true;
}

bool IsPrepared()
{
    return s_prepared;
}

void Disable(const char *reason)
{
    if (s_prepared)
        LOG_WARN(LSFG_TAG, "Frame generation is unavailable: %s", reason);
    DestroyRuntime();
    s_prepared = false;
}

void RegisterSwapchain(const Device &device, VkSwapchainKHR swapchain, VkExtent2D extent,
                       const std::vector<VkImage> &images)
{
    UnregisterSwapchain();
    if (!s_prepared)
        return;
    if (images.size() < 3)
    {
        Disable("the swapchain has fewer than three images");
        return;
    }
    s_device = device;
    s_swapchain = swapchain;
    s_extent = extent;
    s_images = images;
}

void UnregisterSwapchain()
{
    DestroyRuntime();
    s_runtimeFailed = false;
    s_swapchain = VK_NULL_HANDLE;
    s_extent = {};
    s_images.clear();
    ResetRate();
}

bool Present(VkQueue queue, const VkPresentInfoKHR &info, VkResult &result)
{
    const bool ours = s_prepared && s_swapchain != VK_NULL_HANDLE && info.swapchainCount == 1 &&
                      info.pSwapchains[0] == s_swapchain;
    if (!ours || !s_enabled.load(std::memory_order_acquire))
    {
        if (s_runtime)
        {
            // switched off: its images and pipelines go until it is wanted again
            DestroyRuntime();
            s_runtimeFailed = false;
            ResetRate();
        }
        return false;
    }

    ObserveFrame();
    if (!s_generating || !EnsureRuntime(queue))
        return false;

    result = VK_ERROR_INITIALIZATION_FAILED;
    if (!lsfg_nx_present(s_runtime, queue, &info, &result))
    {
        // not consumed: this frame goes out as usual
        LOG_ERROR(LSFG_TAG, "The swapchain was not accepted, frame generation stops");
        DestroyRuntime();
        s_runtimeFailed = true;
        return false;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        DestroyRuntime();
        if (result != VK_ERROR_OUT_OF_DATE_KHR)
        {
            LOG_ERROR(LSFG_TAG, "Presenting failed (%d), frame generation stops", static_cast<int>(result));
            s_runtimeFailed = true;
        }
    }
    return true;
}

} // namespace TicoLsfg
