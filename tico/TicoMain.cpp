/// @file TicoMain.cpp
/// @brief Entry point for the tico-integrated Flycast NRO (Vulkan).
///
/// v1 architecture: TicoVulkan owns the VkInstance/VkDevice/swapchain and
/// implements the libretro hw_render_interface_vulkan; TicoCore drives the
/// libretro core; per frame we Acquire → retro_run → Composite-and-Present.
/// The Tico overlay is rendered with ImGui's Vulkan backend over the composed
/// frame before present.

#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "TicoUtils.h"
#include "TicoLogger.h"
#include "TicoOverlay.h"
#include "TicoVulkan.h"

#include "imgui.h"

#include <SDL.h>
#include <SDL_mixer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/env.h>
#endif

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}

//==============================================================================
// Globals
//==============================================================================

static std::unique_ptr<TicoCore> g_core;
static std::unique_ptr<TicoOverlay> g_overlay;
static bool g_running = true;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static bool g_chainloadToTico = false;
static bool g_overlayRendererReady = false;
static float g_overlayBaseFontScale = 1.0f;

#ifdef __SWITCH__
static u8 g_lastOperationMode = 255;

/// Update nwindow crop on dock/undock. Always 1920×1080 surface; crop selects
/// the visible sub-region for handheld vs docked.
static bool UpdateScreenMode()
{
    u8 op = appletGetOperationMode();
    if (op == g_lastOperationMode)
        return false;

    if (op == AppletOperationMode_Handheld)
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
        LOG_INFO("DISPLAY", "Mode → Handheld (1280×720 crop)");
    }
    else
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        LOG_INFO("DISPLAY", "Mode → Docked (1920×1080)");
    }
    g_lastOperationMode = op;
    return true;
}
#endif

static float GetOverlayModeScale()
{
#ifdef __SWITCH__
    return appletGetOperationMode() == AppletOperationMode_Handheld ? 1.5f : 1.0f;
#else
    return 1.0f;
#endif
}

//==============================================================================
// Audio
//==============================================================================

static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t* data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
}

static bool InitAudio()
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

        g_audioDevice = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
            return false;
        }
        LOG_INFO("AUDIO", "SDL_QueueAudio initialized (deviceID=%u, freq=%d)",
                 g_audioDevice, have.freq);
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

//==============================================================================
// Overlay
//==============================================================================

static const char* FirstExistingPath(const char* const* paths, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        FILE* fp = std::fopen(paths[i], "rb");
        if (fp)
        {
            std::fclose(fp);
            return paths[i];
        }
    }
    return nullptr;
}

static std::string GameTitleFromPath(const std::string& path)
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

static bool InitOverlay(const std::string& romPath)
{
    if (!TicoVulkan::IsReady())
        return false;

    uint32_t width = 0, height = 0;
    TicoVulkan::GetSwapExtent(width, height);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    g_overlayBaseFontScale = 1.0f;

    const char* const titleFontPaths[] = {
        "romfs:/fonts/font.ttf",
        "sdmc:/tico/fonts/font.ttf",
        "sdmc:/tico/assets/fonts/font.ttf",
        "sdmc:/tico/assets/font.ttf",
    };
    const char* const descriptionFontPaths[] = {
        "romfs:/fonts/description.ttf",
        "sdmc:/tico/fonts/description.ttf",
        "sdmc:/tico/assets/fonts/description.ttf",
        "sdmc:/tico/assets/description.ttf",
    };

    const char* titleFont = FirstExistingPath(titleFontPaths, sizeof(titleFontPaths) / sizeof(titleFontPaths[0]));
    const char* descriptionFont = FirstExistingPath(descriptionFontPaths, sizeof(descriptionFontPaths) / sizeof(descriptionFontPaths[0]));
    if (titleFont)
        io.Fonts->AddFontFromFileTTF(titleFont, 30.0f);
    if (descriptionFont)
        io.Fonts->AddFontFromFileTTF(descriptionFont, 22.0f);
    if (io.Fonts->Fonts.Size == 0)
    {
        io.Fonts->AddFontDefault();
        g_overlayBaseFontScale = 1.55f;
    }
    io.FontGlobalScale = g_overlayBaseFontScale * GetOverlayModeScale();

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 18.0f;
    style.FrameRounding = 12.0f;
    style.GrabRounding = 12.0f;

    if (!TicoVulkan::InitOverlayRenderer())
    {
        ImGui::DestroyContext();
        LOG_WARN("OVERLAY", "ImGui Vulkan overlay renderer unavailable");
        return false;
    }

    g_overlay = std::make_unique<TicoOverlay>();
    g_overlay->SetCore(g_core.get());
    g_overlay->SetGameTitle(GameTitleFromPath(romPath));
    g_overlayRendererReady = true;
    LOG_INFO("OVERLAY", "Tico overlay initialized");
    return true;
}

static void ShutdownOverlay()
{
    g_overlay.reset();
    TicoVulkan::ShutdownOverlayRenderer();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    g_overlayRendererReady = false;
}

static void RenderOverlayFrame(float deltaTime)
{
    if (!g_overlayRendererReady || !g_overlay)
        return;

    uint32_t width = 0, height = 0;
    TicoVulkan::GetSwapExtent(width, height);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = deltaTime > 0.0f ? deltaTime : (1.0f / 60.0f);
    io.FontGlobalScale = g_overlayBaseFontScale * GetOverlayModeScale();

    TicoVulkan::BeginOverlayFrame();
    ImGui::NewFrame();
    g_overlay->Update(io.DeltaTime);
    g_overlay->Render(io.DisplaySize, 0, 4.0f / 3.0f,
                      static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(width), static_cast<int>(height));
    ImGui::Render();
    TicoVulkan::SetOverlayDrawData(ImGui::GetDrawData());
}

//==============================================================================
// Input — feed the overlay first; if it doesn't consume controls, feed core.
//==============================================================================

static SDL_GameController* OpenFirstController()
{
    for (int i = 0; i < SDL_NumJoysticks(); ++i)
    {
        if (SDL_IsGameController(i))
            return SDL_GameControllerOpen(i);
    }
    return nullptr;
}

static void HandleCoreInput(SDL_GameController* controller)
{
    if (!controller || !g_core)
        return;

    const bool start = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
    const bool back  = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);

    g_core->ClearInputs();

    auto btn = [&](SDL_GameControllerButton b) {
        return SDL_GameControllerGetButton(controller, b) != 0;
    };
    auto axis = [&](SDL_GameControllerAxis a) {
        return SDL_GameControllerGetAxis(controller, a);
    };

    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_A,      btn(SDL_CONTROLLER_BUTTON_A));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_B,      btn(SDL_CONTROLLER_BUTTON_B));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_X,      btn(SDL_CONTROLLER_BUTTON_X));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_Y,      btn(SDL_CONTROLLER_BUTTON_Y));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_START,  start);
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_SELECT, back);
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_UP,     btn(SDL_CONTROLLER_BUTTON_DPAD_UP));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_DOWN,   btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_LEFT,   btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L,      btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R,      btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L2,
                           axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000);
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R2,
                           axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_L3,
                           btn(SDL_CONTROLLER_BUTTON_LEFTSTICK));
    g_core->SetInputState(0, RETRO_DEVICE_ID_JOYPAD_R3,
                           btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK));

    g_core->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                            axis(SDL_CONTROLLER_AXIS_LEFTX));
    g_core->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                            axis(SDL_CONTROLLER_AXIS_LEFTY));
    g_core->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
                            axis(SDL_CONTROLLER_AXIS_RIGHTX));
    g_core->SetAnalogState(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
                            axis(SDL_CONTROLLER_AXIS_RIGHTY));
}

//==============================================================================
// Chainload back to tico.nro
//==============================================================================

#ifdef __SWITCH__
static void MaybeChainload()
{
    if (!g_chainloadToTico)
        return;

    const char* primary = "sdmc:/switch/tico.nro";
    const char* fallback = "sdmc:/switch/tico/tico.nro";
    const char* target = nullptr;

    struct stat st;
    if (stat(primary, &st) == 0)
        target = primary;
    else if (stat(fallback, &st) == 0)
        target = fallback;

    if (target)
    {
        char args[512];
        std::snprintf(args, sizeof(args), "%s --resume", target);
        envSetNextLoad(target, args);
        LOG_INFO("HOME", "Chainloading back to %s", target);
    }
    else
    {
        LOG_WARN("HOME", "No tico.nro found at %s or %s", primary, fallback);
    }
}
#endif

//==============================================================================
// Main
//==============================================================================

int main(int argc, char* argv[])
{
    LOG_INFO("HOME", "tico-flycast (Vulkan) starting");

#ifdef __SWITCH__
    appletLockExit();
    socketInitializeDefault();
    romfsInit();
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
    UpdateScreenMode();
#endif

    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    InitAudio();

    // 1) VkInstance + VkSurface — must exist before retro_set_environment so
    //    the negotiation interface (set during retro_set_environment) can
    //    refer to a real surface when the device is created.
    if (!TicoVulkan::CreateInstance())
    {
        LOG_ERROR("HOME", "TicoVulkan::CreateInstance failed");
        return 1;
    }

    // 2) Bring up the core. retro_set_environment runs inside Init() and
    //    registers SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE on us.
    g_core = std::make_unique<TicoCore>();
    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);
    if (!g_audio.Init(g_audioDevice))
        LOG_WARN("HOME", "TicoAudio init failed");

    if (!g_core->Init())
    {
        LOG_ERROR("HOME", "TicoCore::Init failed");
        return 1;
    }

    // 3) Pick the ROM and load it. LoadGame() runs retro_load_game() (which
    //    is when flycast registers SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE
    //    on us), then InitEGLDualContext() — which now uses that registered
    //    negotiation iface to create the device, then the swapchain — and
    //    finally context_reset() (core fetches our hw render iface).
    std::string romPath = TicoConfig::TEST_ROM;
    if (argc > 1)
        romPath = argv[1];
    LOG_INFO("HOME", "Loading ROM: %s", romPath.c_str());
    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
    }
    else if (!InitOverlay(romPath))
    {
        LOG_WARN("HOME", "Overlay init failed; continuing without Tico overlay");
    }

    SDL_GameController* controller = OpenFirstController();
    LOG_INFO("HOME", "Controller %s", controller ? "opened" : "not present");

    // 5) Frame loop.
    uint32_t lastTicks = SDL_GetTicks();
    while (g_running)
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
        {
            g_running = false;
            break;
        }
        UpdateScreenMode();
#endif
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                g_running = false;
            else if (ev.type == SDL_CONTROLLERDEVICEADDED && !controller)
                controller = OpenFirstController();
        }

        const uint32_t nowTicks = SDL_GetTicks();
        float deltaTime = static_cast<float>(nowTicks - lastTicks) / 1000.0f;
        lastTicks = nowTicks;
        if (deltaTime <= 0.0f || deltaTime > 0.25f)
            deltaTime = 1.0f / 60.0f;

        bool inputConsumedByOverlay = false;
        if (g_overlay)
        {
            inputConsumedByOverlay = g_overlay->HandleInput(controller);
            if (g_overlay->ShouldReset())
            {
                LOG_INFO("OVERLAY", "Reset requested");
                if (g_core)
                    g_core->Reset();
                g_overlay->ClearReset();
            }
            if (g_overlay->ShouldExit())
            {
                LOG_INFO("OVERLAY", "Exit requested");
                g_overlay->ClearExit();
                g_chainloadToTico = true;
                g_running = false;
            }
        }
        if (!g_running)
            break;

        const bool overlayVisible = g_overlay && g_overlay->IsVisible();
        if (g_core)
        {
            if (overlayVisible)
            {
                g_core->ClearInputs();
                g_core->Pause();
            }
            else
            {
                g_core->Resume();
                if (inputConsumedByOverlay)
                    g_core->ClearInputs();
                else
                    HandleCoreInput(controller);
            }
        }

        if (TicoVulkan::BeginFrame())
        {
            if (g_core)
                g_core->RunFrame();
            RenderOverlayFrame(deltaTime);
            TicoVulkan::EndFrame();
        }
    }

    LOG_INFO("HOME", "Shutting down");
    ShutdownOverlay();
    g_core.reset();
    TicoVulkan::Shutdown();

    g_audio.Shutdown();
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
        Mix_CloseAudio();
    SDL_Quit();

#ifdef __SWITCH__
    MaybeChainload();
    romfsExit();
    socketExit();
    appletUnlockExit();
#endif

    LOG_INFO("HOME", "Clean exit");
    return 0;
}
