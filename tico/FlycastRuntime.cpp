/// @file FlycastRuntime.cpp
/// @brief Flycast/libretro CoreRuntime. Orchestration extracted from the old
/// monolithic TicoMain.cpp; the v1 bring-up sequence and frame ordering are
/// preserved (Acquire → retro_run → composite overlay → present).

#include "FlycastRuntime.h"

#include "FlycastDiscs.h"
#include "TicoAudio.h"
#include "TicoConfig.h"
#include "TicoCore.h"
#include "TicoLogger.h"
#include "TicoOverlayHost.h"
#include "TicoVulkan.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include "imgui.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;

namespace Tico
{

// Adapts the libretro TicoCore + TicoVulkan renderer to the overlay's
// backend-agnostic IOverlayHost / IOverlayRAHost interfaces.
class FlycastOverlayHost final : public IOverlayHost, public IOverlayRAHost
{
public:
    explicit FlycastOverlayHost(TicoCore *core) : core_(core) {}

    std::string GetGamePath() override { return core_ ? core_->GetGamePath() : std::string(); }
    bool IsGameLoaded() override { return core_ && core_->IsGameLoaded(); }

    bool StateSlotExists(int slot) override
    {
        struct stat st;
        return stat(StatePath(slot).c_str(), &st) == 0;
    }
    void SaveStateSlot(int slot) override
    {
        if (core_) core_->SaveState(StatePath(slot));
    }
    void LoadStateSlot(int slot) override
    {
        if (core_) core_->LoadState(StatePath(slot));
    }
    void SwapDisc(const std::string &path) override
    {
        if (core_) core_->SwapDiskByPath(path);
    }

    ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height) override
    {
        return TicoVulkan::CreateOverlayTextureRGBA(rgba, static_cast<uint32_t>(width),
                                                    static_cast<uint32_t>(height));
    }
    void DestroyTexture(ImTextureID tex) override
    {
        if (tex) TicoVulkan::DestroyOverlayTexture(tex);
    }

    IOverlayRAHost *RA() override { return this; }

    // IOverlayRAHost — backed by TicoCore's RA state.
    std::mutex &Mutex() override { return core_->m_raCallbackMutex; }
    std::vector<RANotification> &Notifications() override { return core_->m_raNotifications; }
    RAAlertPosition AlertPosition() const override { return core_->m_raAlertPosition; }
    ImTextureID IconTexture() const override { return core_->m_raIconTexture; }
    void SetIconTexture(ImTextureID tex) override { core_->m_raIconTexture = tex; }
    ImTextureID BadgeTexture(const std::string &badge) const override
    {
        auto it = core_->m_raBadgeCache.find(badge);
        return it != core_->m_raBadgeCache.end() ? it->second : (ImTextureID)0;
    }

private:
    std::string StatePath(int slot) const
    {
        const std::string romPath = core_ ? core_->GetGamePath() : std::string();

        // Game name = basename without extension.
        std::string name = romPath;
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);

        const std::string stateDir = TicoConfig::StatesPath();
        TicoConfig::MakeDirs(stateDir);

        return stateDir + name + ".state" + std::to_string(slot);
    }

    TicoCore *core_ = nullptr;
};

namespace
{

#ifdef __SWITCH__
u8 s_lastOperationMode = 255;

/// Always 1920×1080 surface; crop selects the visible sub-region for handheld
/// vs docked. (Owned by the runtime now that Tico::Main is display-agnostic.)
void UpdateScreenMode()
{
    u8 op = appletGetOperationMode();
    if (op == s_lastOperationMode)
        return;
    if (op == AppletOperationMode_Handheld)
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
    else
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
    s_lastOperationMode = op;
}
#else
void UpdateScreenMode() {}
#endif

// TicoCore::SetAudioCallbacks takes plain function pointers, so route them
// through a file-static audio sink set up in Initialize().
TicoAudio *s_audio = nullptr;

void AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_audio)
        s_audio->PushSample(left, right);
}

size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return s_audio ? s_audio->PushSamples(data, frames) : frames;
}

void AudioFlushCallback()
{
    if (s_audio)
        s_audio->Flush();
}

// The overlay's message for key in the language tico is set to, with one %d
// filled in.
std::string TrFormat(const char *key, int value)
{
    const std::string format = SwitchFrontend::OverlayTranslation::tr(key);
    char text[256];
    std::snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

#ifdef __SWITCH__
bool PromptKeyboard(const char *header, const std::string &initial, size_t maxLength,
                    std::string &output)
{
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0)))
        return false;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, header);
    if (!initial.empty())
        swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, static_cast<u32>(maxLength));
    std::vector<char> text(maxLength + 1);
    const Result result = swkbdShow(&keyboard, text.data(), text.size());
    swkbdClose(&keyboard);
    if (R_FAILED(result))
        return false;
    output = text.data();
    return true;
}
#endif

/// Where the game goes on a screenW x screenH surface for the Display tab's
/// display_mode ("Integer" | "Display") and display_size. Integer scales the
/// Dreamcast's 640x480 by 1x, 2x or the largest that fits ("Auto"); Display
/// fits an aspect ratio (4:3, 16:9, the core's own "Original") or stretches.
void ComputeGameViewport(float screenW, float screenH, float coreAspect,
                         float &outX, float &outY, float &outW, float &outH)
{
    static constexpr int kBaseW = 640;
    static constexpr int kBaseH = 480;
    const std::string mode = OverlayConfig::GetConfigValue("display_mode", "Display");
    const std::string size = OverlayConfig::GetConfigValue("display_size", "4:3");

    float dstWidth = screenW;
    float dstHeight = screenH;
    if (mode == "Integer")
    {
        int scale;
        if (size == "1x")
            scale = 1;
        else if (size == "2x")
            scale = 2;
        else
            scale = std::max(1, std::min(static_cast<int>(screenW) / kBaseW,
                                         static_cast<int>(screenH) / kBaseH));
        dstWidth = std::min(screenW, static_cast<float>(kBaseW * scale));
        dstHeight = std::min(screenH, static_cast<float>(kBaseH * scale));
    }
    else if (size != "Stretch")
    {
        float ar = 4.0f / 3.0f;
        if (size == "16:9")
            ar = 16.0f / 9.0f;
        else if (size == "Original")
            ar = coreAspect > 0.0f ? coreAspect : 4.0f / 3.0f;
        if (ar > screenW / screenH)
        {
            dstWidth = screenW;
            dstHeight = screenW / ar;
        }
        else
        {
            dstHeight = screenH;
            dstWidth = screenH * ar;
        }
    }

    outW = dstWidth;
    outH = dstHeight;
    outX = (screenW - dstWidth) / 2.0f;
    outY = (screenH - dstHeight) / 2.0f;
}

std::string GameTitleFromPath(const std::string &path)
{
    std::string title = path;
    const size_t slash = title.find_last_of("/\\");
    if (slash != std::string::npos)
        title = title.substr(slash + 1);
    const size_t dot = title.find_last_of('.');
    if (dot != std::string::npos)
        title = title.substr(0, dot);
    return title.empty() ? "Flycast" : title;
}

}  // namespace

FlycastRuntime::FlycastRuntime(LogCallback log) : log_(std::move(log)) {}

FlycastRuntime::~FlycastRuntime() = default;

// NAOMI/Atomiswave, detected by extension (matches flycast's libretro frontend).
static bool IsArcadePath(const std::string &path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".lst" || ext == ".bin" || ext == ".dat" ||
           ext == ".zip" || ext == ".7z";
}

bool FlycastRuntime::Configure(const LaunchInfo &launch)
{
    romPath_ = launch.contentPath.empty() ? TicoConfig::TEST_ROM : launch.contentPath;
    titleArg_ = launch.title;
    TicoConfig::SetSlug(launch.argc > 3 && launch.argv[3] ? launch.argv[3] : "", romPath_);
    isArcade_ = IsArcadePath(romPath_);
    return true;
}

bool FlycastRuntime::InitAudio()
{
    if (TicoConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = TicoAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = TicoAudio::CHANNELS;
        want.samples = 2048;
        want.callback = nullptr;

        audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (audioDevice_ == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
            return false;
        }
        LOG_INFO("AUDIO", "SDL_QueueAudio initialized (deviceID=%u, freq=%d)", audioDevice_, have.freq);
    }
    else
    {
        if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
            return false;
        }
        LOG_INFO("AUDIO", "SDL_mixer initialized");
    }
    return true;
}

bool FlycastRuntime::Initialize(const LaunchInfo &)
{
    // Tico::Main is SDL-free (shared with the standalone target), so the
    // libretro path initializes the SDL subsystems it needs (audio + timer).
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
        LOG_WARN("HOME", "SDL_Init(AUDIO|TIMER) failed: %s", SDL_GetError());

#ifdef __SWITCH__
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
    UpdateScreenMode();
#endif

    InitAudio();

    // VkInstance + VkSurface must exist before retro_set_environment so the
    // negotiation interface can refer to a real surface at device creation.
    if (!TicoVulkan::CreateInstance())
    {
        LOG_ERROR("HOME", "TicoVulkan::CreateInstance failed");
        return false;
    }

    core_ = std::make_unique<TicoCore>();
    core_->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);
    // settings.json is the one settings definition: every option it lists
    // reaches the core with its default when the config file does not set it.
    // The overlay reads the file after the core, which writes the defaults
    // (renderer included) on a first run, so its saves keep them.
    core_->EnsureConfigLoaded();
    OverlayConfig::ReloadConfig();
    ApplySettingsToCore();

    audio_ = std::make_unique<TicoAudio>();
    s_audio = audio_.get();
    if (!audio_->Init(audioDevice_))
        LOG_WARN("HOME", "TicoAudio init failed");

    if (!core_->Init())
    {
        LOG_ERROR("HOME", "TicoCore::Init failed");
        return false;
    }
    return true;
}

bool FlycastRuntime::LoadContent(const std::string &path)
{
    romPath_ = path.empty() ? romPath_ : path;
    LOG_INFO("HOME", "Loading ROM: %s", romPath_.c_str());

    if (!core_->LoadGame(romPath_))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
    }
    else if (!InitOverlay(romPath_))
    {
        LOG_WARN("HOME", "Overlay init failed; continuing without Tico overlay");
    }

    lastTicks_ = SDL_GetTicks();
    return true; // Idle (matching v1) even if the game failed to load.
}

bool FlycastRuntime::InitOverlay(const std::string &romPath)
{
    if (!TicoVulkan::IsReady())
        return false;

    overlayHost_ = std::make_unique<FlycastOverlayHost>(core_.get());
    if (!ImGuiOverlay::Init(overlayHost_.get()))
    {
        overlayHost_.reset();
        return false;
    }

    // Prefer the launcher-supplied title; fall back to the rom filename.
    OverlayUI::SetGameTitle(titleArg_.empty() ? GameTitleFromPath(romPath) : titleArg_);
    // the menu's slots 1..4 are the state files .state0 .. .state3
    FlycastOverlayHost *host = overlayHost_.get();
    OverlayUI::SetSlotOccupiedCallback([host](int slot) {
        return slot >= 1 && host->StateSlotExists(slot - 1);
    });
    OverlayUI::SetDiscCallback([this] {
        std::vector<OverlayUI::DiscMenuEntry> entries;
        discPaths_.clear();
        if (!core_)
            return entries;
        std::string current = core_->GetGamePath();
        if (current.size() >= 2 && current.front() == '"' && current.back() == '"')
            current = current.substr(1, current.size() - 2);
        current = NormalizeDiscPath(current);
        for (const DiscEntry &disc : ScanDiscs(current))
        {
            entries.push_back({disc.displayName, disc.romPath == current});
            discPaths_.push_back(disc.romPath);
        }
        return entries;
    });
    OverlayUI::ReloadSettings();
    overlayReady_ = true;
    return true;
}

void FlycastRuntime::ShutdownOverlay()
{
    if (overlayReady_)
    {
        OverlayUI::SetSlotOccupiedCallback(nullptr);
        OverlayUI::SetDiscCallback(nullptr);
        ImGuiOverlay::Shutdown(); // frees textures via overlayHost_ (still alive)
    }
    overlayHost_.reset();
    overlayReady_ = false;
    menuOpen_ = false;
}

void FlycastRuntime::RenderOverlayFrame(float deltaTime)
{
    if (!overlayReady_)
        return;

    uint32_t width = 0, height = 0;
    TicoVulkan::GetSwapExtent(width, height);
    UpdateHud(deltaTime);
    TicoVulkan::SetOverlayDrawData(ImGuiOverlay::BuildFrame(static_cast<float>(width),
                                                            static_cast<float>(height),
                                                            deltaTime));
}

void FlycastRuntime::ApplySettingsToCore()
{
    if (!core_)
        return;
    OverlayConfig::ApplyToCore([this](const std::string &key, const std::string &value) {
        core_->SetOption(key, value);
    });
}

void FlycastRuntime::UpdateHud(float deltaTime)
{
    hudFrames_++;
    hudSeconds_ += deltaTime;
    if (hudSeconds_ >= 0.5f)
    {
        hudFps_ = static_cast<float>(hudFrames_) / hudSeconds_;
        hudFrames_ = 0;
        hudSeconds_ = 0.0f;
    }
    OverlayUI::HudStats stats;
    stats.fps = hudFps_;
    if (core_)
    {
        stats.rendered_width = core_->GetFrameWidth();
        stats.rendered_height = core_->GetFrameHeight();
    }
    OverlayUI::SetHudStats(stats);
}

void FlycastRuntime::OpenMenu()
{
    if (!overlayReady_ || menuOpen_)
        return;
    menuOpen_ = true;
    navHeldPrev_ = 0;
    navRepeatFrames_ = 0;
    ImGuiOverlay::SetVisible(true);
}

void FlycastRuntime::CloseMenu()
{
    if (!menuOpen_)
        return;
    menuOpen_ = false;
    ImGuiOverlay::SetVisible(false);
}

bool FlycastRuntime::FeedMenu(const FrameInput &input)
{
    if (!menuOpen_)
        return false;

    // Directional navigation: D-pad + left stick, edge plus hold-repeat.
    const uint64_t buttons = input.buttons;
    uint64_t dirHeld = 0;
    if ((buttons & Pad_Up) || input.leftStickY < -16000) dirHeld |= Pad_Up;
    if ((buttons & Pad_Down) || input.leftStickY > 16000) dirHeld |= Pad_Down;
    if ((buttons & Pad_Left) || input.leftStickX < -16000) dirHeld |= Pad_Left;
    if ((buttons & Pad_Right) || input.leftStickX > 16000) dirHeld |= Pad_Right;

    uint64_t dirFire = dirHeld & ~navHeldPrev_; // new presses fire instantly
    if (dirHeld != 0 && dirHeld == navHeldPrev_)
    {
        if (--navRepeatFrames_ <= 0)
        {
            dirFire |= dirHeld;
            navRepeatFrames_ = kNavRepeatFrames;
        }
    }
    else if (dirFire != 0)
    {
        navRepeatFrames_ = kNavInitialDelayFrames;
    }
    navHeldPrev_ = dirHeld;

    // Pad_B is the Switch A button (east), Pad_A the Switch B button (south).
    ImGuiOverlay::FeedNav({
        .up = (dirFire & Pad_Up) != 0,
        .down = (dirFire & Pad_Down) != 0,
        .left = (dirFire & Pad_Left) != 0,
        .right = (dirFire & Pad_Right) != 0,
        .accept = (input.pressed & Pad_B) != 0,
        .cancel = (input.pressed & Pad_A) != 0,
    });
    return true;
}

void FlycastRuntime::RunMenuAction()
{
    using OverlayUI::Action;
    const Action action = ImGuiOverlay::ConsumeAction();
    if (OverlayUI::ConsumeSettingsChanged())
        ApplySettingsToCore();

    switch (action)
    {
    case Action::None:
        return;
    case Action::Resume:
        CloseMenu();
        return;
    case Action::Exit:
        LOG_INFO("OVERLAY", "Exit requested");
        CloseMenu();
        chainload_ = true;
        exitRequested_ = true;
        return;
    case Action::Reset:
        LOG_INFO("OVERLAY", "Reset requested");
        if (core_)
            core_->Reset();
        CloseMenu();
        return;
    case Action::SwapDisc:
    {
        const int index = OverlayUI::ConsumeDiscIndex();
        if (core_ && index >= 0 && index < static_cast<int>(discPaths_.size()))
            core_->SwapDiskByPath(discPaths_[static_cast<size_t>(index)]);
        CloseMenu();
        return;
    }
    case Action::EditText:
    {
        const OverlayConfig::OptionDef *option = OverlayUI::ConsumeTextEditOption();
#ifdef __SWITCH__
        std::string value;
        const size_t length = option && option->max_length > 0 ? option->max_length : 64;
        if (option &&
            PromptKeyboard(SwitchFrontend::OverlayTranslation::tr(option->label_key).c_str(),
                           OverlayConfig::GetOptionValue(*option), length, value))
        {
            OverlayConfig::SetOptionValue(*option, value);
            OverlayUI::NotifyOptionEdited(*option);
        }
#else
        (void)option;
#endif
        return;
    }
    default:
        break;
    }

    if (OverlayUI::IsSaveStateAction(action) && overlayHost_)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        overlayHost_->SaveStateSlot(slot - 1);
        OverlayUI::ShowToast(TrFormat("emulator_state_saved", slot));
        CloseMenu();
    }
    else if (OverlayUI::IsLoadStateAction(action) && overlayHost_)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        overlayHost_->LoadStateSlot(slot - 1);
        OverlayUI::ShowToast(TrFormat("emulator_state_loaded", slot));
        CloseMenu();
    }
}

void FlycastRuntime::UpdateGameViewport()
{
    uint32_t sw = 0, sh = 0;
    TicoVulkan::GetSwapExtent(sw, sh);
    if (sw == 0 || sh == 0)
    {
        TicoVulkan::SetGameViewport(0, 0, 0, 0); // full screen
        return;
    }
    const float coreAspect = core_ ? core_->GetAspectRatio() : (4.0f / 3.0f);
    float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
    ComputeGameViewport(static_cast<float>(sw), static_cast<float>(sh), coreAspect, vx, vy, vw, vh);
    TicoVulkan::SetGameViewport(static_cast<int>(vx + 0.5f), static_cast<int>(vy + 0.5f),
                                static_cast<int>(vw + 0.5f), static_cast<int>(vh + 0.5f));
}

void FlycastRuntime::ApplyCoreInput(const FrameInput &input)
{
    if (!core_)
        return;

    core_->ClearInputs();

    for (unsigned port = 0; port < MaxPlayers; ++port)
    {
        const PlayerInput &player = input.players[port];
        const uint64_t b = player.buttons;
        auto down = [&](PadButton bit) { return (b & bit) != 0; };

        if (isArcade_)
        {
            // Arcade layout based on observed game actions:
            // Switch Y uses the low-kick source, Switch X uses the high-kick source.
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_A));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_B));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_Y));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_R));
        }
        else
        {
            // Dreamcast follows physical disposition through the neutral pad map.
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_A));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_B));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_Y));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
            core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_R));
        }
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_START, down(Pad_Start));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_SELECT, down(Pad_Select));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_UP, down(Pad_Up));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_DOWN, down(Pad_Down));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_LEFT, down(Pad_Left));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, down(Pad_Right));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L2, down(Pad_L2));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R2, down(Pad_R2));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_L3, down(Pad_L3));
        core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_R3, down(Pad_R3));

        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, player.leftStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, player.leftStickY);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, player.rightStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, player.rightStickY);

        // Arcade games read directions from either the digital JVS stick
        // (fighters) or the analog axis (racers), so cross-feed d-pad <-> stick.
        // Dreamcast keeps its native d-pad + analog and is left alone.
        if (isArcade_)
        {
            // d-pad -> axis
            if (down(Pad_Left) || down(Pad_Right))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                      down(Pad_Left) ? -0x7fff : 0x7fff);
            if (down(Pad_Up) || down(Pad_Down))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                      down(Pad_Up) ? -0x7fff : 0x7fff);

            // stick -> d-pad (lenient threshold to keep diagonals)
            const int16_t kDirThreshold = 0x2800;
            if (player.leftStickX <= -kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_LEFT, true);
            if (player.leftStickX >= kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_RIGHT, true);
            if (player.leftStickY <= -kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_UP, true);
            if (player.leftStickY >= kDirThreshold)
                core_->SetInputState(port, RETRO_DEVICE_ID_JOYPAD_DOWN, true);
        }
    }
}

void FlycastRuntime::HandleInput(const FrameInput &input)
{
    RunMenuAction();
    if (exitRequested_)
        return;

    // Start+Select only ever opens the menu; B closes it. While the combo is
    // held it is kept from the game.
    const bool comboDown = (input.buttons & Pad_Start) && (input.buttons & Pad_Select);
    if (comboDown && !menuOpen_)
        OpenMenu();
    const bool consumed = FeedMenu(input) || comboDown;

    if (core_)
    {
        if (menuOpen_)
        {
            core_->ClearInputs();
            core_->Pause();
        }
        else
        {
            core_->Resume();
            if (consumed)
                core_->ClearInputs();
            else
                ApplyCoreInput(input);
        }
    }
}

void FlycastRuntime::RunFrame()
{
    UpdateScreenMode();
    frameInFlight_ = TicoVulkan::BeginFrame();
    if (frameInFlight_ && core_)
        core_->RunFrame();
}

void FlycastRuntime::RenderFrame()
{
    if (!frameInFlight_)
        return;

    const uint32_t now = SDL_GetTicks();
    float deltaTime = static_cast<float>(now - lastTicks_) / 1000.0f;
    lastTicks_ = now;
    if (deltaTime <= 0.0f || deltaTime > 0.25f)
        deltaTime = 1.0f / 60.0f;

    RenderOverlayFrame(deltaTime);
    // The game image is composited by TicoVulkan (not ImGui), so the Display
    // tab's screen size has to be handed to it as the blit's destination.
    UpdateGameViewport();

    TicoVulkan::EndFrame();
    frameInFlight_ = false;
}

void FlycastRuntime::Shutdown()
{
    LOG_INFO("HOME", "Shutting down");
    ShutdownOverlay();
    core_.reset();
    TicoVulkan::Shutdown();

    if (audio_)
        audio_->Shutdown();
    s_audio = nullptr;
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
        Mix_CloseAudio();
    SDL_Quit();
}

}  // namespace Tico
