/// @file FlycastRuntime.cpp
/// @brief Flycast/libretro CoreRuntime. Orchestration extracted from the old
/// monolithic TicoMain.cpp; the v1 bring-up sequence and frame ordering are
/// preserved (Acquire → retro_run → composite overlay → present).

#include "FlycastRuntime.h"

#include "FlycastBios.h"
#include "FlycastCheats.h"
#include "FlycastDiscs.h"
#include "FlycastLibrary.h"
#include "FlycastShaders.h"
#include "TicoChainload.h"
#include "FlycastSaves.h"
#include "TicoAudio.h"
#include "TicoConfig.h"
#include "TicoCore.h"
#include "TicoLogger.h"
#include "TicoLsfg.h"
#include "TicoOverlayHost.h"
#include "TicoVulkan.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include "imgui.h"
#include "deps/stb/stb_image.h"
#include "../core/deps/stb/stb_image_write.h" // built with the core

#include <SDL.h>
#include <SDL_mixer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iterator>
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
        if (!core_)
            return;
        const std::string path = StatePath(slot);
        core_->SaveState(path);
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return;
        // a small picture of the game beside the state, for the Save/Load panel
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (TicoVulkan::CaptureGameImage(256, 192, rgba, w, h))
            stbi_write_png((path + ".png").c_str(), (int)w, (int)h, 4, rgba.data(), (int)w * 4);
    }

    std::string SlotStatePath(int slot) const { return StatePath(slot); }
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

// The same with two %s, filled in order (not snprintf: a translation with
// another % sequence must not read off the stack).
std::string TrFormat2(const char *key, const std::string &first, const std::string &second)
{
    std::string text = SwitchFrontend::OverlayTranslation::tr(key);
    for (const std::string *value : {&first, &second})
    {
        const size_t at = text.find("%s");
        if (at == std::string::npos)
            break;
        text.replace(at, 2, *value);
    }
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

// A Switch button by the name settings.json stores, as tico's positional
// PadButton bit; 0 for "None".
uint64_t PadBitFor(const std::string &name)
{
    static const std::pair<const char *, uint64_t> kNames[] = {
        {"A", Pad_B}, {"B", Pad_A}, {"X", Pad_Y}, {"Y", Pad_X},
        {"L", Pad_L}, {"R", Pad_R}, {"ZL", Pad_L2}, {"ZR", Pad_R2},
        {"Plus", Pad_Start}, {"Minus", Pad_Select}, {"StickL", Pad_L3}, {"StickR", Pad_R3},
        {"Up", Pad_Up}, {"Down", Pad_Down}, {"Left", Pad_Left}, {"Right", Pad_Right},
    };
    for (const auto &entry : kNames)
        if (name == entry.first)
            return entry.second;
    return 0;
}

// Input > button mapping: each pad input with the Switch button it sits on
// by default. Flycast reads the RetroPad by name on the Dreamcast (RetroPad
// B is the Dreamcast A) and by number on NAOMI / Atomiswave (RetroPad B is
// button 1); the defaults keep the layout tico-flycast always had.
struct ButtonMapping
{
    const char *key;
    const char *fallback;
    unsigned retroId;
};
const ButtonMapping kDreamcastMappings[] = {
    {"dc_map_a", "A", RETRO_DEVICE_ID_JOYPAD_B},
    {"dc_map_b", "B", RETRO_DEVICE_ID_JOYPAD_A},
    {"dc_map_x", "X", RETRO_DEVICE_ID_JOYPAD_Y},
    {"dc_map_y", "Y", RETRO_DEVICE_ID_JOYPAD_X},
    {"dc_map_l_trigger", "ZL", RETRO_DEVICE_ID_JOYPAD_L2},
    {"dc_map_r_trigger", "ZR", RETRO_DEVICE_ID_JOYPAD_R2},
    {"dc_map_start", "Plus", RETRO_DEVICE_ID_JOYPAD_START},
    {"dc_map_up", "Up", RETRO_DEVICE_ID_JOYPAD_UP},
    {"dc_map_down", "Down", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"dc_map_left", "Left", RETRO_DEVICE_ID_JOYPAD_LEFT},
    {"dc_map_right", "Right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
};
const ButtonMapping kArcadeMappings[] = {
    {"arcade_map_button_1", "B", RETRO_DEVICE_ID_JOYPAD_B},
    {"arcade_map_button_2", "A", RETRO_DEVICE_ID_JOYPAD_A},
    {"arcade_map_button_3", "R", RETRO_DEVICE_ID_JOYPAD_Y},
    {"arcade_map_button_4", "Y", RETRO_DEVICE_ID_JOYPAD_X},
    {"arcade_map_button_5", "X", RETRO_DEVICE_ID_JOYPAD_R},
    {"arcade_map_button_6", "L", RETRO_DEVICE_ID_JOYPAD_L},
    {"arcade_map_button_7", "ZR", RETRO_DEVICE_ID_JOYPAD_R2},
    {"arcade_map_button_8", "ZL", RETRO_DEVICE_ID_JOYPAD_L2},
    {"arcade_map_start", "Plus", RETRO_DEVICE_ID_JOYPAD_START},
    {"arcade_map_coin", "Minus", RETRO_DEVICE_ID_JOYPAD_SELECT},
    {"arcade_map_test", "StickL", RETRO_DEVICE_ID_JOYPAD_L3},
    {"arcade_map_service", "StickR", RETRO_DEVICE_ID_JOYPAD_R3},
    {"arcade_map_up", "Up", RETRO_DEVICE_ID_JOYPAD_UP},
    {"arcade_map_down", "Down", RETRO_DEVICE_ID_JOYPAD_DOWN},
    {"arcade_map_left", "Left", RETRO_DEVICE_ID_JOYPAD_LEFT},
    {"arcade_map_right", "Right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
};

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
    // Without a game (e.g. from the homebrew menu) the library lists the ROM
    // folders; a game picked there runs in a fresh launch of this NRO, marked
    // --from-library so Exit Game comes back to the list.
    romPath_ = launch.contentPath;
    standalone_ = romPath_.empty();
    argv0_ = launch.argc > 0 && launch.argv[0] ? launch.argv[0] : "";
    for (int i = 1; i < launch.argc && launch.argv[i]; ++i)
        if (std::string(launch.argv[i]) == "--from-library")
            fromLibrary_ = true;
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
    // this game's own settings (Settings > This Game), if it has them, over
    // the core's; none in the library
    OverlayConfig::SetGame(romPath_);
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
    if (standalone_)
    {
        if (InitOverlay(std::string()))
        {
            FlycastLibrary::Register([this](const std::string &game, const std::string &slug) {
                LaunchSelf(argv0_.c_str(), {game, std::string(), slug, "--from-library"}, log_);
                exitRequested_ = true;
            });
            OverlayUI::SetGameTitle("Flycast");
            OverlayUI::SetLibraryMode(true);
            OpenMenu();
        }
        lastTicks_ = SDL_GetTicks();
        return true;
    }
    LOG_INFO("HOME", "Loading ROM: %s", romPath_.c_str());

    // Without its BIOS a game only shows a black screen: say what is missing
    // and where it goes instead of booting it.
    const FlycastBios::Status bios = FlycastBios::Check(
        TicoConfig::Slug(), isArcade_, OverlayConfig::GetConfigValue("reicast_hle_bios", "disabled") == "enabled");
    if (!bios.ok)
    {
        biosMissing_ = true;
        if (InitOverlay(romPath_))
        {
            const std::string text = bios.wrongSize
                ? TrFormat2("emulator_bios_wrong_size", bios.file, bios.size) + "\n" + bios.folder
                : TrFormat2("emulator_bios_missing", bios.file, bios.folder);
            std::vector<std::string> choices;
            if (!isArcade_)
                choices.push_back(SwitchFrontend::OverlayTranslation::tr("emulator_bios_use_hle"));
            choices.push_back(SwitchFrontend::OverlayTranslation::tr("emulator_exit_game"));
            OpenMenu();
            OverlayUI::ShowNotice(text, choices);
        }
        lastTicks_ = SDL_GetTicks();
        return true;
    }
    if (bios.unknownDump)
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_bios_unknown"),
                             OverlayUI::ToastCorner::TopRight);

    if (!core_->LoadGame(romPath_))
    {
        LOG_ERROR("HOME", "LoadGame failed; idling");
    }
    else
    {
        FlycastSaves::BackupForSession();
        FlycastCheats::Load(core_->CurrentDiscPath(), TicoConfig::Slug());
        if (!InitOverlay(romPath_))
            LOG_WARN("HOME", "Overlay init failed; continuing without Tico overlay");
        else
        {
            FlycastShaders::Init(core_.get());
            offerResume_ = true;
        }
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
    // the menu's slots 1..6 are the state files .state0 .. .state5
    FlycastOverlayHost *host = overlayHost_.get();
    OverlayUI::SetSlotOccupiedCallback([host](int slot) {
        return slot >= 1 && host->StateSlotExists(slot - 1);
    });
    // Save/Load State show each slot's picture and when it was saved.
    slotPictures_ = {};
    OverlayUI::SetSlotPreviewCallback([this, host](int slot) {
        OverlayUI::SlotPreview preview;
        if (slot < 1 || slot > (int)slotPictures_.size())
            return preview;
        ImTextureID &picture = slotPictures_[slot - 1];
        host->DestroyTexture(picture); // the slot may have been saved again
        picture = 0;
        const std::string path = host->SlotStatePath(slot - 1);
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return preview;
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
        preview.saved_at = when;
        int w = 0, h = 0, channels = 0;
        if (unsigned char *rgba = stbi_load((path + ".png").c_str(), &w, &h, &channels, 4))
        {
            picture = host->CreateTextureRGBA(rgba, w, h);
            stbi_image_free(rgba);
        }
        preview.texture = (unsigned long long)picture;
        if (core_ && core_->GetAspectRatio() > 0.1f)
            preview.aspect = core_->GetAspectRatio();
        return preview;
    });
    OverlayUI::SetDiscCallback([this] {
        std::vector<OverlayUI::DiscMenuEntry> entries;
        discPaths_.clear();
        if (!core_)
            return entries;
        // the discs are found from the launched game (an .m3u lists them all);
        // the current one is whatever is in the drive now
        std::string game = core_->GetGamePath();
        if (game.size() >= 2 && game.front() == '"' && game.back() == '"')
            game = game.substr(1, game.size() - 2);
        std::string current = core_->CurrentDiscPath();
        if (current.size() >= 2 && current.front() == '"' && current.back() == '"')
            current = current.substr(1, current.size() - 2);
        current = NormalizeDiscPath(current);
        for (const DiscEntry &disc : ScanDiscs(NormalizeDiscPath(game)))
        {
            entries.push_back({disc.displayName, disc.romPath == current});
            discPaths_.push_back(disc.romPath);
        }
        return entries;
    });
    // Cheats from the game's .cht/.cheats file; the menu hides them in hardcore.
    OverlayUI::SetCheatCallbacks(
        [this] {
            std::vector<OverlayUI::CheatMenuEntry> entries;
            // another disc resets the core's cheats: read that disc's file
            if (core_ && !core_->IsSwapPending() &&
                FlycastCheats::NeedsReload(core_->CurrentDiscPath()))
                FlycastCheats::Load(core_->CurrentDiscPath(), TicoConfig::Slug());
            const auto &cheats = FlycastCheats::List();
            for (size_t i = 0; i < cheats.size(); ++i)
                entries.push_back({cheats[i].name, cheats[i].enabled, true, (int)i, false});
            return entries;
        },
        [this](int index) {
            if (index < 0 || (core_ && core_->IsHardcoreActive()))
                return false;
            FlycastCheats::Toggle((size_t)index);
            return true;
        });
    // Settings > Players: what each port has, and the system's screen to
    // choose who is which player
    OverlayUI::PlayerCallbacks players;
    players.ports = [] { return ControllerNames(); };
    players.note = [this] {
        return isArcade_ ? SwitchFrontend::OverlayTranslation::tr("emulator_players_arcade") : std::string();
    };
    OverlayUI::SetPlayerCallbacks(std::move(players));
    OverlayUI::ReloadSettings();
    overlayReady_ = true;
    return true;
}

void FlycastRuntime::ShutdownOverlay()
{
    if (overlayReady_)
    {
        OverlayUI::SetSlotOccupiedCallback(nullptr);
        OverlayUI::SetSlotPreviewCallback(nullptr);
        slotPictures_ = {}; // freed with the overlay's textures below
        OverlayUI::SetDiscCallback(nullptr);
        OverlayUI::SetCheatCallbacks(nullptr, nullptr);
        OverlayUI::SetPlayerCallbacks({});
        FlycastLibrary::Unregister();
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
    TicoCore::StopRumble(); // the game is paused
    navHeldPrev_ = 0;
    navRepeatFrames_ = 0;
    OverlayUI::SetHardcoreMode(core_ && core_->IsHardcoreActive());
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
    ImGuiOverlay::FeedTouch({input.touchDown, input.touchX, input.touchY});
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
        if (fromLibrary_)
            LaunchSelf(argv0_.c_str(), {}, log_); // back to the library
        else if (!standalone_)
            chainload_ = true; // the library itself just quits
        exitRequested_ = true;
        return;
    case Action::Restart:
        // Starting the Dreamcast core afresh in this process is fragile (its
        // Vulkan context and globals), so the NRO starts itself again with the
        // same arguments; the game is saved on the way out.
        LOG_INFO("OVERLAY", "Restart requested");
        CloseMenu();
        // the relaunched NRO finds this and skips the resume prompt: Restart
        // means from the start
        if (overlayHost_)
        {
            if (FILE *marker = std::fopen(RestartMarkerPath().c_str(), "wb"))
                std::fclose(marker);
        }
        relaunch_ = true;
        exitRequested_ = true;
        return;
    case Action::Reset:
        LOG_INFO("OVERLAY", "Reset requested");
        if (core_)
            core_->Reset();
        CloseMenu();
        return;
    case Action::ControllerOrder:
        if (!ShowControllerOrder())
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_controllers_failed"),
                                 OverlayUI::ToastCorner::TopRight);
        return;
    case Action::NoticeChoice:
    {
        // the missing-BIOS notice: HLE BIOS (Dreamcast only) or leave
        const int choice = OverlayUI::ConsumeNoticeChoice();
        if (!isArcade_ && choice == 0)
        {
            OverlayConfig::SetConfigValue("reicast_hle_bios", "enabled");
            OverlayConfig::SaveConfig();
            relaunch_ = true;
        }
        else if (fromLibrary_)
            LaunchSelf(argv0_.c_str(), {}, log_);
        else if (!standalone_)
            chainload_ = true;
        CloseMenu();
        exitRequested_ = true;
        return;
    }
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
        if (slot == OverlayUI::kAutoStateSlot)
            OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_auto_loaded"));
        else
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

    // Resolve the mapping once per frame; every port shares it.
    struct ResolvedMapping
    {
        uint64_t bit;
        unsigned retroId;
    };
    ResolvedMapping mappings[std::size(kArcadeMappings)];
    size_t mappingCount = 0;
    if (isArcade_)
        for (const ButtonMapping &mapping : kArcadeMappings)
            mappings[mappingCount++] = {PadBitFor(OverlayConfig::GetConfigValue(mapping.key, mapping.fallback)),
                                        mapping.retroId};
    else
        for (const ButtonMapping &mapping : kDreamcastMappings)
            mappings[mappingCount++] = {PadBitFor(OverlayConfig::GetConfigValue(mapping.key, mapping.fallback)),
                                        mapping.retroId};

    for (unsigned port = 0; port < MaxPlayers; ++port)
    {
        const PlayerInput &player = input.players[port];
        const uint64_t b = player.buttons;

        // Several pad inputs may share a Switch button, so only ever press.
        uint32_t pressed = 0; // RetroPad ids, as bits
        for (size_t i = 0; i < mappingCount; ++i)
            if (b & mappings[i].bit)
            {
                core_->SetInputState(port, mappings[i].retroId, true);
                pressed |= 1u << mappings[i].retroId;
            }
        auto down = [&](unsigned retroId) { return (pressed & (1u << retroId)) != 0; };

        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, player.leftStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, player.leftStickY);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, player.rightStickX);
        core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, player.rightStickY);

        // Arcade games read directions from either the digital JVS stick
        // (fighters) or the analog axis (racers), so cross-feed d-pad <-> stick.
        // Dreamcast keeps its native d-pad + analog and is left alone.
        if (isArcade_)
        {
            // joystick directions -> axis
            if (down(RETRO_DEVICE_ID_JOYPAD_LEFT) || down(RETRO_DEVICE_ID_JOYPAD_RIGHT))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                      down(RETRO_DEVICE_ID_JOYPAD_LEFT) ? -0x7fff : 0x7fff);
            if (down(RETRO_DEVICE_ID_JOYPAD_UP) || down(RETRO_DEVICE_ID_JOYPAD_DOWN))
                core_->SetAnalogState(port, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                      down(RETRO_DEVICE_ID_JOYPAD_UP) ? -0x7fff : 0x7fff);

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

    // A Dreamcast sees a controller in every port the core has one plugged
    // into, and some games (Hoyle Casino) then wait for all four players to
    // join. Plug in only the Switch controllers that are connected, following
    // them as they come and go; player 1's port always has one. Arcade boards
    // read both players through the JVS I/O board and keep all their ports.
    if (core_ && !isArcade_)
        for (unsigned port = 1; port < MaxPlayers; ++port)
            if (input.players[port].connected != portConnected_[port])
            {
                portConnected_[port] = input.players[port].connected;
                core_->SetPortConnected(port, portConnected_[port]);
            }

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
    FlycastShaders::Update(); // a preset compiles outside the frame
    // Display > Frame Generation, applied in game
    TicoLsfg::SetOptions(OverlayConfig::GetConfigValue("lsfg_enabled", "disabled") == "enabled",
                         OverlayConfig::GetConfigValue("lsfg_flow_scale", "0.25") == "0.5" ? 0.5f : 0.25f,
                         OverlayConfig::GetConfigValue("lsfg_performance_mode", "enabled") == "enabled");
    frameInFlight_ = TicoVulkan::BeginFrame();
    if (frameInFlight_ && core_)
    {
        core_->RunFrame();
        if (offerResume_)
            OfferResume();
    }
}

// Once the game's first frame has run, the menu asks whether to continue from
// the auto save, if there is one (not after Restart, nor in hardcore).
void FlycastRuntime::OfferResume()
{
    offerResume_ = false;
    if (!overlayHost_ || !core_)
        return;
    if (std::remove(RestartMarkerPath().c_str()) == 0)
        return;
    if (core_->IsHardcoreActive() ||
        !overlayHost_->StateSlotExists(OverlayUI::kAutoStateSlot - 1))
        return;
    // tico's General > Continue Last Game
    const std::string mode = OverlayConfig::ResumeOnLaunch();
    if (mode == "never")
        return;
    if (mode == "always")
    {
        overlayHost_->LoadStateSlot(OverlayUI::kAutoStateSlot - 1);
        OverlayUI::ShowToast(SwitchFrontend::OverlayTranslation::tr("emulator_auto_loaded"));
        return;
    }
    OpenMenu();
    if (menuOpen_)
        OverlayUI::ShowResumePrompt();
}

std::string FlycastRuntime::RestartMarkerPath() const
{
    return overlayHost_->SlotStatePath(OverlayUI::kAutoStateSlot - 1) + ".restart";
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
    // the state the game is left in goes to the auto slot (listed first in
    // Load State), whatever closed it: Exit, Restart or HOME
    if (overlayHost_ && overlayHost_->IsGameLoaded())
        overlayHost_->SaveStateSlot(OverlayUI::kAutoStateSlot - 1);
    FlycastShaders::Shutdown();
    ShutdownOverlay();
    core_.reset();
    TicoCore::StopRumble();
    TicoVulkan::Shutdown();

    if (audio_)
        audio_->Shutdown();
    s_audio = nullptr;
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
        Mix_CloseAudio();
    SDL_Quit();
}

}  // namespace Tico
