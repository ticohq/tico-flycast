/// @file StandaloneRuntime.h
/// @brief Native-Flycast implementation of Tico::CoreRuntime.
///
/// Drives Flycast's native emulator directly (flycast_init / emu.loadGame /
/// emu.start / emu.render / mainui), bypassing Flycast's standalone UI loop.
/// This is the flycast analogue of tico-ppsspp's PpssppRuntime. All the
/// orchestration that used to live in the monolithic TicoStandaloneMain.cpp
/// lives here, behind the agnostic CoreRuntime interface Tico::Main drives.
#pragma once

#include "TicoMain.h"

#include <memory>
#include <string>
#include <vector>

class TicoOverlay;

namespace Tico
{

class StandaloneOverlayHost; // native-flycast IOverlayHost adapter (in the .cpp)

class StandaloneRuntime final : public CoreRuntime
{
public:
    explicit StandaloneRuntime(LogCallback log = {});
    ~StandaloneRuntime() override;

    const char *Name() const override { return "flycast-standalone"; }
    bool Configure(const LaunchInfo &launch) override;
    bool Initialize(const LaunchInfo &launch) override;
    bool LoadContent(const std::string &path) override;
    void HandleInput(const FrameInput &input) override;
    void RunFrame() override;
    void RenderFrame() override;
    bool ShouldExit() const override;
    bool ShouldChainloadLauncher() const override { return chainload_; }
    void RequestExit() override { exitRequested_ = true; }
    void Shutdown() override;

private:
    void EnsureOverlay();       // lazily create the overlay once the renderer is up
    bool OverlayVisible() const;
    void RenderOverlayPaused(); // present frozen frame + overlay in its own pass

    LogCallback log_;
    std::vector<char *> flycastArgv_;
    int flycastArgc_ = 0;
    bool contentLoaded_ = false;
    bool mainuiReady_ = false;
    bool exitRequested_ = false;
    bool chainload_ = false;
    bool frameOk_ = false;
    bool paused_ = false; // emulation stopped (emu.stop) while the overlay is up

    std::unique_ptr<TicoOverlay> overlay_;
    std::unique_ptr<StandaloneOverlayHost> overlayHost_;
};

}  // namespace Tico
