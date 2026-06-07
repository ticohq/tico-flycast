/// @file FlycastRuntime.cpp
/// @brief Flycast/libretro CoreRuntime. Orchestration extracted from the old
/// monolithic TicoMain.cpp; the v1 bring-up sequence and frame ordering are
/// preserved (Acquire → retro_run → composite overlay → present).

#include "FlycastRuntime.h"

#include "TicoAudio.h"
#include "TicoConfig.h"
#include "TicoCore.h"
#include "TicoLogger.h"
#include "TicoOverlay.h"
#include "TicoVulkan.h"

#include "imgui.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <cstdio>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

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
        std::string name = core_ ? core_->GetGamePath() : std::string();
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);

        struct stat st;
        if (stat(TicoConfig::STATES_PATH, &st) == -1)
            mkdir(TicoConfig::STATES_PATH, 0777);
        return std::string(TicoConfig::STATES_PATH) + name + ".state" + std::to_string(slot);
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

const char *FirstExistingPath(const char *const *paths, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        FILE *fp = std::fopen(paths[i], "rb");
        if (fp)
        {
            std::fclose(fp);
            return paths[i];
        }
    }
    return nullptr;
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

bool FlycastRuntime::Configure(const LaunchInfo &launch)
{
    romPath_ = launch.contentPath.empty() ? TicoConfig::TEST_ROM : launch.contentPath;
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

    uint32_t width = 0, height = 0;
    TicoVulkan::GetSwapExtent(width, height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    overlayBaseFontScale_ = 1.0f;

    const char *const titleFontPaths[] = {
        "romfs:/fonts/font.ttf",
        "sdmc:/tico/fonts/font.ttf",
        "sdmc:/tico/assets/fonts/font.ttf",
        "sdmc:/tico/assets/font.ttf",
    };
    const char *const descriptionFontPaths[] = {
        "romfs:/fonts/description.ttf",
        "sdmc:/tico/fonts/description.ttf",
        "sdmc:/tico/assets/fonts/description.ttf",
        "sdmc:/tico/assets/description.ttf",
    };

    const char *titleFont = FirstExistingPath(titleFontPaths, sizeof(titleFontPaths) / sizeof(titleFontPaths[0]));
    const char *descriptionFont = FirstExistingPath(descriptionFontPaths, sizeof(descriptionFontPaths) / sizeof(descriptionFontPaths[0]));
    if (titleFont)
        io.Fonts->AddFontFromFileTTF(titleFont, 30.0f);
    if (descriptionFont)
        io.Fonts->AddFontFromFileTTF(descriptionFont, 22.0f);
    if (io.Fonts->Fonts.Size == 0)
    {
        io.Fonts->AddFontDefault();
        overlayBaseFontScale_ = 1.55f;
    }
    io.FontGlobalScale = overlayBaseFontScale_ * OverlayModeScale();

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.FrameRounding = 12.0f;
    style.GrabRounding = 12.0f;

    if (!TicoVulkan::InitOverlayRenderer())
    {
        ImGui::DestroyContext();
        LOG_WARN("OVERLAY", "ImGui Vulkan overlay renderer unavailable");
        return false;
    }

    overlay_ = std::make_unique<TicoOverlay>();
    overlayHost_ = std::make_unique<FlycastOverlayHost>(core_.get());
    overlay_->SetHost(overlayHost_.get());
    overlay_->SetGameTitle(GameTitleFromPath(romPath));
    overlayReady_ = true;
    LOG_INFO("OVERLAY", "Tico overlay initialized");
    return true;
}

void FlycastRuntime::ShutdownOverlay()
{
    overlay_.reset();       // dtor frees textures via overlayHost_ (still alive)
    overlayHost_.reset();
    TicoVulkan::ShutdownOverlayRenderer();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    overlayReady_ = false;
}

void FlycastRuntime::RenderOverlayFrame(float deltaTime)
{
    if (!overlayReady_ || !overlay_)
        return;

    uint32_t width = 0, height = 0;
    TicoVulkan::GetSwapExtent(width, height);

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);
    io.FontGlobalScale = overlayBaseFontScale_ * OverlayModeScale();

    TicoVulkan::BeginOverlayFrame();
    ImGui::NewFrame();
    overlay_->Update(io.DeltaTime);
    overlay_->Render(io.DisplaySize, 0, 4.0f / 3.0f,
                     static_cast<int>(width), static_cast<int>(height),
                     static_cast<int>(width), static_cast<int>(height));
    ImGui::Render();
    TicoVulkan::SetOverlayDrawData(ImGui::GetDrawData());
}

void FlycastRuntime::ApplyCoreInput(const FrameInput &input)
{
    if (!core_)
        return;

    const uint64_t b = input.buttons;
    auto down = [&](PadButton bit) { return (b & bit) != 0; };

    core_->ClearInputs();
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_A, down(Pad_A));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_B, down(Pad_B));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_X, down(Pad_X));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_Y, down(Pad_Y));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_START, down(Pad_Start));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_SELECT, down(Pad_Select));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_UP, down(Pad_Up));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_DOWN, down(Pad_Down));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_LEFT, down(Pad_Left));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_RIGHT, down(Pad_Right));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L, down(Pad_L));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R, down(Pad_R));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L2, down(Pad_L2));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R2, down(Pad_R2));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L3, down(Pad_L3));
    core_->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R3, down(Pad_R3));

    core_->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, input.leftStickX);
    core_->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, input.leftStickY);
    core_->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, input.rightStickX);
    core_->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, input.rightStickY);
}

void FlycastRuntime::HandleInput(const FrameInput &input)
{
    bool consumed = false;
    if (overlay_)
    {
        consumed = overlay_->HandleInput(input);
        if (overlay_->ShouldReset())
        {
            LOG_INFO("OVERLAY", "Reset requested");
            if (core_)
                core_->Reset();
            overlay_->ClearReset();
        }
        if (overlay_->ShouldExit())
        {
            LOG_INFO("OVERLAY", "Exit requested");
            overlay_->ClearExit();
            chainload_ = true;
            exitRequested_ = true;
        }
    }
    if (exitRequested_)
        return;

    const bool overlayVisible = overlay_ && overlay_->IsVisible();
    if (core_)
    {
        if (overlayVisible)
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

    // Apply the overlay's screen-size / display-mode selection to the game blit.
    // The game image is composited by TicoVulkan (not ImGui), so the destination
    // rect has to be handed to it here; otherwise it always fills the screen.
    if (overlay_)
    {
        uint32_t sw = 0, sh = 0;
        TicoVulkan::GetSwapExtent(sw, sh);
        if (sw > 0 && sh > 0)
        {
            const float coreAspect = core_ ? core_->GetAspectRatio() : (4.0f / 3.0f);
            float vx = 0.0f, vy = 0.0f, vw = 0.0f, vh = 0.0f;
            overlay_->GetGameViewport(static_cast<float>(sw), static_cast<float>(sh),
                                      coreAspect, vx, vy, vw, vh);
            TicoVulkan::SetGameViewport(static_cast<int>(vx + 0.5f), static_cast<int>(vy + 0.5f),
                                        static_cast<int>(vw + 0.5f), static_cast<int>(vh + 0.5f));
        }
    }
    else
    {
        TicoVulkan::SetGameViewport(0, 0, 0, 0); // full screen
    }

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
