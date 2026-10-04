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

#include <array>

#include "imgui.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class TicoCore;
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
    bool ShouldRelaunch() const override { return relaunch_; }
    void RequestExit() override { exitRequested_ = true; }
    void Shutdown() override;

private:
    bool InitAudio();
    bool InitOverlay(const std::string &romPath);
    void ShutdownOverlay();
    void RenderOverlayFrame(float deltaTime);
    void ApplyCoreInput(const FrameInput &input);
    void OpenMenu();
    void CloseMenu();
    void OfferResume();
    std::string RestartMarkerPath() const;
    /// Feeds the open menu; returns false when the menu is closed.
    bool FeedMenu(const FrameInput &input);
    /// Carries out what the menu chose on the last built frame.
    void RunMenuAction();
    void ApplySettingsToCore();
    void UpdateGameViewport();
    void UpdateHud(float deltaTime);

    LogCallback log_;
    std::unique_ptr<TicoCore> core_;
    std::unique_ptr<FlycastOverlayHost> overlayHost_;
    std::unique_ptr<TicoAudio> audio_;
    SDL_AudioDeviceID audioDevice_ = 0;

    bool overlayReady_ = false;
    bool exitRequested_ = false;
    bool chainload_ = false;
    bool relaunch_ = false;   // Restart: start this NRO again on exit
    bool biosMissing_ = false; // the game's BIOS is missing: only the notice runs
    bool standalone_ = false;  // started without a game: the library is the menu
    bool fromLibrary_ = false; // launched from the library: Exit goes back to it
    std::string argv0_;
    bool offerResume_ = false; // ask to continue from the auto save after the first frame
    bool frameInFlight_ = false;
    std::string romPath_;
    std::string titleArg_;     // Display title from the launcher (argv[2])
    bool isArcade_ = false;    // NAOMI / Atomiswave (directionals -> analog axis)
    bool portConnected_[MaxPlayers] = {true, true, true, true}; // as LoadGame leaves them
    uint32_t lastTicks_ = 0;

    // Quick menu
    bool menuOpen_ = false;
    std::vector<std::string> discPaths_;   // Change Disc rows, in menu order
    std::array<ImTextureID, 6> slotPictures_{}; // Save/Load State pictures, slots 1-6
    uint64_t navHeldPrev_ = 0;             // directional hold-repeat
    int navRepeatFrames_ = 0;
    static constexpr int kNavInitialDelayFrames = 14;
    static constexpr int kNavRepeatFrames = 6;

    // HUD frame counter
    int hudFrames_ = 0;
    float hudSeconds_ = 0.0f;
    float hudFps_ = 0.0f;
};

}  // namespace Tico
