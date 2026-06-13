/// @file FlycastRuntime.h
/// @brief Flycast/libretro implementation of Tico::CoreRuntime.
///
/// Owns the libretro driver (TicoCore), the Vulkan host (TicoVulkan), SDL audio
/// (TicoAudio), and the overlay. All flycast-specific orchestration that used
/// to live in the monolithic TicoMain.cpp lives here, behind the agnostic
/// CoreRuntime interface that Tico::Main drives.
#pragma once

#include "TicoMain.h"

#include <SDL.h>

#include <cstdint>
#include <memory>
#include <string>

class TicoCore;
class TicoOverlay;
class TicoAudio;

namespace Tico
{

class FlycastOverlayHost;  // libretro IOverlayHost adapter (defined in the .cpp)

class FlycastRuntime final : public CoreRuntime
{
public:
    explicit FlycastRuntime(LogCallback log = {});
    ~FlycastRuntime() override;

    const char *Name() const override { return "flycast"; }
    bool Configure(const LaunchInfo &launch) override;
    bool Initialize(const LaunchInfo &launch) override;
    bool LoadContent(const std::string &path) override;
    void HandleInput(const FrameInput &input) override;
    void RunFrame() override;
    void RenderFrame() override;
    bool ShouldExit() const override { return exitRequested_; }
    bool ShouldChainloadLauncher() const override { return chainload_; }
    void RequestExit() override { exitRequested_ = true; }
    void Shutdown() override;

private:
    bool InitAudio();
    bool InitOverlay(const std::string &romPath);
    void ShutdownOverlay();
    void RenderOverlayFrame(float deltaTime);
    void ApplyCoreInput(const FrameInput &input);

    LogCallback log_;
    std::unique_ptr<TicoCore> core_;
    std::unique_ptr<TicoOverlay> overlay_;
    std::unique_ptr<FlycastOverlayHost> overlayHost_;
    std::unique_ptr<TicoAudio> audio_;
    SDL_AudioDeviceID audioDevice_ = 0;

    bool overlayReady_ = false;
    bool exitRequested_ = false;
    bool chainload_ = false;
    bool frameInFlight_ = false;
    std::string romPath_;
    std::string titleArg_;     // Display title from the launcher (argv[2])
    bool isArcade_ = false;    // NAOMI / Atomiswave (directionals -> analog axis)
    uint32_t lastTicks_ = 0;
    float overlayBaseFontScale_ = 1.0f;
};

}  // namespace Tico
