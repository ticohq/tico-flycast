/// @file StandaloneRuntime.cpp
/// @brief Native-Flycast CoreRuntime. Orchestration ported verbatim from the
/// old monolithic TicoStandaloneMain.cpp; platform/loop/input/chainload now
/// live in the shared Tico::Main driver.
#ifndef LIBRETRO

#include "StandaloneRuntime.h"

#include "TicoConfig.h"
#include "TicoOverlay.h"
#include "TicoOverlayHost.h"

#include "nswitch.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui.h"
#include "log/LogManager.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "reios/reios.h"
#include "stdclass.h"
#include "ui/gui.h"
#include "ui/imgui_driver.h"
#include "ui/mainui.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <strings.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace Tico
{

namespace
{

LogCallback g_log;
std::string g_launchTitle;
int g_ticoSh4Clock = 0;
u8 g_lastOperationMode = 255;
bool g_osQuit = false;

void Log(const char *fmt, ...)
{
    if (fmt == nullptr)
        return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (g_log)
        g_log(buffer);
    else
        std::fprintf(stderr, "%s\n", buffer);
}

void SetCurrentThreadAffinity(const char *name, s32 preferredCore)
{
    u64 processMask = 0;
    Result rc = svcGetInfo(&processMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    if (R_FAILED(rc))
    {
        Log("%s affinity: svcGetInfo(CoreMask) failed rc=0x%x", name, (unsigned)rc);
        return;
    }
    if (preferredCore < 0 || preferredCore >= 4 || (processMask & (UINT64_C(1) << preferredCore)) == 0)
    {
        Log("%s affinity: core %d unavailable in process mask 0x%llx", name,
            (int)preferredCore, (unsigned long long)processMask);
        return;
    }
    const u32 affinityMask = 1u << preferredCore;
    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferredCore, affinityMask);
    if (R_FAILED(rc))
    {
        Log("%s affinity: svcSetThreadCoreMask core=%d mask=0x%x failed rc=0x%x", name,
            (int)preferredCore, affinityMask, (unsigned)rc);
        return;
    }
    Log("%s affinity: pinned to core %d (process mask 0x%llx)", name, (int)preferredCore,
        (unsigned long long)processMask);
}

void SetTicoSh4Clock(int clock, const char *source)
{
    if (clock < 100 || clock > 300)
    {
        Log("ignoring %s SH4 clock override: %d MHz", source, clock);
        return;
    }
    g_ticoSh4Clock = clock;
    Log("using %s SH4 clock override: %d MHz", source, g_ticoSh4Clock);
}

bool IsSyntheticLaunchArg(const char *arg)
{
    if (arg == nullptr || arg[0] == '\0')
        return true;
    return std::strcmp(arg, "--resume") == 0 || std::strcmp(arg, "-resume") == 0 ||
           std::strncmp(arg, "disk$", 5) == 0;
}

bool HasContentExtension(const char *arg)
{
    const char *extension = std::strrchr(arg, '.');
    if (extension == nullptr)
        return false;
    return strcasecmp(extension, ".cdi") == 0 || strcasecmp(extension, ".chd") == 0 ||
           strcasecmp(extension, ".gdi") == 0 || strcasecmp(extension, ".cue") == 0 ||
           strcasecmp(extension, ".elf") == 0 || strcasecmp(extension, ".zip") == 0 ||
           strcasecmp(extension, ".7z") == 0 || strcasecmp(extension, ".bin") == 0 ||
           strcasecmp(extension, ".dat") == 0 || strcasecmp(extension, ".lst") == 0;
}

bool LooksLikeContentPath(const char *arg)
{
    if (arg == nullptr || arg[0] == '\0' || arg[0] == '-')
        return false;
    return std::strncmp(arg, "sdmc:/", 6) == 0 || std::strncmp(arg, "romfs:/", 7) == 0 ||
           std::strchr(arg, '/') != nullptr || std::strchr(arg, '\\') != nullptr ||
           HasContentExtension(arg);
}

std::vector<char *> BuildFlycastArgv(int argc, char *argv[])
{
    std::vector<char *> filtered;
    if (argc > 0 && argv[0] != nullptr)
        filtered.push_back(argv[0]);
    else
        filtered.push_back(const_cast<char *>("tico-flycast-standalone"));

    char *contentPath = nullptr;
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i] == nullptr)
            continue;

        if (std::strcmp(argv[i], "--tico-rom") == 0)
        {
            if (i + 1 < argc && !IsSyntheticLaunchArg(argv[i + 1]))
            {
                contentPath = argv[i + 1];
                Log("selected --tico-rom content: %s", contentPath);
            }
            ++i;
            continue;
        }
        if (std::strcmp(argv[i], "--tico-hour") == 0 || std::strcmp(argv[i], "--tico-token") == 0)
        {
            ++i;
            continue;
        }
        if (std::strcmp(argv[i], "--tico-sh4clock") == 0)
        {
            if (i + 1 < argc && argv[i + 1] != nullptr)
                SetTicoSh4Clock(std::atoi(argv[i + 1]), "launch");
            ++i;
            continue;
        }
        if (IsSyntheticLaunchArg(argv[i]))
            continue;
        if (contentPath == nullptr && LooksLikeContentPath(argv[i]))
        {
            contentPath = argv[i];
            continue;
        }
        if (g_launchTitle.empty())
            g_launchTitle = argv[i];
    }

    if (contentPath != nullptr)
        filtered.push_back(contentPath);
    else
        Log("no content path found in launch arguments");

    filtered.push_back(nullptr);
    return filtered;
}

void SetupTicoDirectories()
{
    flycast::mkdir("sdmc:/tico", 0777);
    flycast::mkdir("sdmc:/tico/config", 0777);
    flycast::mkdir("sdmc:/tico/config/flycast", 0777);
    flycast::mkdir("sdmc:/tico/debug", 0777);
    flycast::mkdir("sdmc:/tico/system", 0777);
    flycast::mkdir("sdmc:/tico/system/dc", 0777);
    flycast::mkdir("sdmc:/tico/saves", 0777);
    flycast::mkdir("sdmc:/tico/saves/dc", 0777);
    flycast::mkdir("sdmc:/tico/states", 0777);
    flycast::mkdir("sdmc:/tico/states/dc", 0777);
    flycast::mkdir("sdmc:/tico/assets", 0777);

    set_user_config_dir("sdmc:/tico/config/flycast/");
    set_user_data_dir("sdmc:/tico/saves/dc/");

    add_system_config_dir("sdmc:/tico/config/flycast/");
    add_system_config_dir("sdmc:/tico/system/dc/");
    add_system_config_dir("romfs:/");
    add_system_config_dir("./");

    add_system_data_dir("sdmc:/tico/system/dc/");
    add_system_data_dir("sdmc:/tico/saves/dc/");
    add_system_data_dir("romfs:/");
    add_system_data_dir("./");
    add_system_data_dir("data/");
}

void ReadTicoSh4ClockOverride()
{
    if (g_ticoSh4Clock != 0)
        return;
    FILE *fp = std::fopen("sdmc:/tico/config/flycast/tico-sh4clock.txt", "r");
    if (fp == nullptr)
        return;
    char line[64] = {};
    if (std::fgets(line, sizeof(line), fp) != nullptr)
        SetTicoSh4Clock(std::atoi(line), "file");
    std::fclose(fp);
}

void ApplyTicoPerformanceDefaults(const char *stage, bool contentLoaded)
{
    config::RendererType.override(RenderType::Vulkan);
    config::ThreadedRendering.override(true);
    config::DynarecEnabled.override(true);
    config::MaxThreads.override(3);
    config::FastGDRomLoad.override(true);

    const bool windowsCe = contentLoaded && ip_meta.isWindowsCE();
    int sh4Clock = g_ticoSh4Clock;
    const char *sh4ClockSource = "config";
    if (sh4Clock != 0)
        sh4ClockSource = "tico";
    else if (windowsCe)
    {
        sh4Clock = 180;
        sh4ClockSource = "wince-default";
    }
    if (sh4Clock != 0)
        config::Sh4Clock.override(sh4Clock);

    Log("perf defaults (%s): wince=%d sh4=%d source=%s", stage, windowsCe ? 1 : 0,
        (int)config::Sh4Clock, sh4ClockSource);
}

void SanitizeContentPath()
{
    if (settings.content.path.rfind("disk$", 0) != 0)
        return;
    Log("ignoring synthetic content path: %s", settings.content.path.c_str());
    settings.content.path.clear();
    settings.content.fileName.clear();
    settings.content.title.clear();
}

void UpdateDisplayMode()
{
    const u8 mode = appletGetOperationMode();
    if (mode == g_lastOperationMode)
        return;
    if (mode == AppletOperationMode_Handheld)
    {
        nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1280, 720);
    }
    else
    {
        nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
    }
    g_lastOperationMode = mode;
}

void StopEmulation()
{
    try {
        emu.stop();
    } catch (const FlycastException &e) {
        Log("emu.stop failed: %s", e.what());
    }
    try {
        emu.unloadGame();
    } catch (const FlycastException &e) {
        Log("emu.unloadGame failed: %s", e.what());
    }
}

}  // namespace

// Lets the global os_DoEvents() hook set the anonymous-namespace quit flag.
void NotifyOsQuit() { g_osQuit = true; }

// Adapts native Flycast (emu / dc_savestate / imguiDriver) to the overlay's
// IOverlayHost. No RetroAchievements on the standalone path (RA() == nullptr).
class StandaloneOverlayHost final : public IOverlayHost
{
public:
    std::string GetGamePath() override { return settings.content.path; }
    bool IsGameLoaded() override { return !settings.content.path.empty(); }

    bool StateSlotExists(int slot) override
    {
        struct stat st;
        return stat(hostfs::getSavestatePath(slot, false).c_str(), &st) == 0;
    }
    void SaveStateSlot(int slot) override { dc_savestate(slot); }
    void LoadStateSlot(int slot) override { dc_loadstate(slot); }
    void SwapDisc(const std::string &path) override
    {
        try {
            emu.insertGdrom(path);
        } catch (const FlycastException &e) {
            Log("insertGdrom failed: %s", e.what());
        }
    }

    ImTextureID CreateTextureRGBA(const unsigned char *rgba, int width, int height) override
    {
        if (!imguiDriver)
            return ImTextureID();
        char name[32];
        std::snprintf(name, sizeof(name), "tico_ovl_%u", ++counter_);
        ImTextureID id = imguiDriver->updateTexture(name, rgba, width, height, false);
        if (id != ImTextureID())
            names_[id] = name;
        return id;
    }
    void DestroyTexture(ImTextureID tex) override
    {
        if (!imguiDriver || tex == ImTextureID())
            return;
        auto it = names_.find(tex);
        if (it != names_.end())
        {
            imguiDriver->deleteTexture(it->second);
            names_.erase(it);
        }
    }

private:
    unsigned counter_ = 0;
    std::map<ImTextureID, std::string> names_;
};

namespace {
ImFont *g_ticoTitleFont = nullptr;
ImFont *g_ticoDescFont = nullptr;

ImFont *LoadFirstFont(ImGuiIO &io, const char *const *paths, size_t count, float size, const char *tag)
{
    for (size_t i = 0; i < count; ++i)
    {
        FILE *f = std::fopen(paths[i], "rb");
        if (!f)
            continue;
        std::fclose(f);
        ImFont *font = io.Fonts->AddFontFromFileTTF(paths[i], size);
        if (font)
        {
            Log("tico overlay %s font: %s", tag, paths[i]);
            return font;
        }
    }
    Log("tico overlay %s font not found", tag);
    return nullptr;
}

// Load the launcher TTFs into the active ImGui atlas. Idempotent: safe whether
// called from gui_initFonts (atlas build) or lazily after the atlas is built
// (ImGui 1.92 rasterizes dynamically). font.ttf = menu UI, description.ttf = RA.
void LoadTicoOverlayFonts()
{
    if (g_ticoTitleFont != nullptr)
        return;
    ImGuiIO &io = ImGui::GetIO();
    const char *titlePaths[] = {
        "romfs:/fonts/font.ttf",
        "sdmc:/tico/fonts/font.ttf",
        "sdmc:/tico/assets/fonts/font.ttf",
        "sdmc:/tico/assets/font.ttf",
    };
    const char *descPaths[] = {
        "romfs:/fonts/description.ttf",
        "sdmc:/tico/fonts/description.ttf",
        "sdmc:/tico/assets/fonts/description.ttf",
        "sdmc:/tico/assets/description.ttf",
    };
    g_ticoTitleFont = LoadFirstFont(io, titlePaths, sizeof(titlePaths) / sizeof(titlePaths[0]), 30.0f, "title");
    g_ticoDescFont = LoadFirstFont(io, descPaths, sizeof(descPaths) / sizeof(descPaths[0]), 22.0f, "description");
}
}  // namespace

// Hook flycast calls at the end of gui_initFonts (before the atlas is built).
void TicoFontHook() { LoadTicoOverlayFonts(); }

namespace {
// Record the overlay's draw commands into the current ImGui frame. Scale matches
// the other tico cores (ppsspp): max(1.0, height/720) — 1.0 handheld, 1.5 docked.
void DrawOverlayContent(TicoOverlay *overlay)
{
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 d = io.DisplaySize;
    if (d.x <= 0.0f || d.y <= 0.0f)
        d = ImVec2(1920.0f, 1080.0f);

    float scale = d.y / 720.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 2.0f) scale = 2.0f;
    io.FontGlobalScale = scale;

    const bool pushed = g_ticoTitleFont != nullptr;
    if (pushed)
        ImGui::PushFont(g_ticoTitleFont);
    overlay->Render(d, 0, 4.0f / 3.0f,
                    static_cast<int>(d.x), static_cast<int>(d.y),
                    static_cast<int>(d.x), static_cast<int>(d.y));
    if (pushed)
        ImGui::PopFont();
}
}  // namespace

StandaloneRuntime::StandaloneRuntime(LogCallback log) : log_(std::move(log))
{
    g_log = log_;
}

StandaloneRuntime::~StandaloneRuntime() = default;

bool StandaloneRuntime::Configure(const LaunchInfo &launch)
{
    flycastArgv_ = BuildFlycastArgv(launch.argc, launch.argv);
    flycastArgc_ = static_cast<int>(flycastArgv_.size()) - 1;
    return true;
}

bool StandaloneRuntime::Initialize(const LaunchInfo &)
{
    SetCurrentThreadAffinity("Flycast-main/render", 2);
    LogManager::Init();
    SetupTicoDirectories();
    ReadTicoSh4ClockOverride();
    cfgSetVirtual("config", "pvr.rend", "4");

    if (flycast_init(flycastArgc_, flycastArgv_.data()))
    {
        Log("Flycast initialization failed");
        return false;
    }
    ApplyTicoPerformanceDefaults("post-init", false);

    // After flycast_init (so its own renderer/gui setup isn't disturbed) but
    // before mainui_init builds the atlas. EnsureOverlay() also loads the fonts
    // as a fallback if this hook never fires.
    gui_set_font_hook(TicoFontHook);

    // Only the tico overlay is shown — suppress flycast's native OSD entirely.
    gui_set_suppress_native_osd(true);

    SanitizeContentPath();
    if (!g_launchTitle.empty() && !IsSyntheticLaunchArg(g_launchTitle.c_str()))
        settings.content.title = g_launchTitle;
    gui_setState(GuiState::Closed);
    return true;
}

bool StandaloneRuntime::LoadContent(const std::string &)
{
    if (settings.content.path.empty())
    {
        Log("no content path supplied; chainloading back to tico");
        chainload_ = true;
        exitRequested_ = true;
        return true;
    }

    mainui_init();
    mainuiReady_ = true;

    try {
        Log("loading content: %s", settings.content.path.c_str());
        emu.loadGame(settings.content.path.c_str());
        ApplyTicoPerformanceDefaults("post-load", true);
        emu.start();
    } catch (const FlycastException &e) {
        Log("content load failed: %s", e.what());
        return false;
    }
    contentLoaded_ = true;

    // Create the overlay eagerly now that the renderer (and imguiDriver) is up,
    // so Plus+Minus reliably opens it instead of hitting any fallback path.
    EnsureOverlay();
    return true;
}

void StandaloneRuntime::HandleInput(const FrameInput &input)
{
    EnsureOverlay();

    if (overlay_)
    {
        // Plus+Minus only OPENS the overlay (handled inside the overlay). The
        // chainload back to tico happens solely via the overlay's Exit item
        // (ShouldExit), never directly from the button combo.
        overlay_->HandleInput(input);
        if (overlay_->ShouldReset())
        {
            Log("overlay reset requested");
            emu.requestReset();
            overlay_->ClearReset();
        }
        if (overlay_->ShouldExit())
        {
            Log("overlay exit requested");
            overlay_->ClearExit();
            chainload_ = true;
            exitRequested_ = true;
            try {
                emu.stop();
            } catch (const FlycastException &e) {
                Log("emu.stop failed during exit: %s", e.what());
            }
        }
        return;
    }
    // If the overlay isn't ready yet, ignore input (never exit on Plus+Minus).
}

void StandaloneRuntime::RunFrame()
{
    if (!contentLoaded_ || exitRequested_)
        return;

    UpdateDisplayMode();
    if (overlay_)
        overlay_->Update(1.0f / 60.0f); // advance open/close + element animation

    // Pause/resume the emulator on overlay open/close exactly like flycast does
    // for its own menu (emu.stop/emu.start). This puts the emulator in a clean
    // stopped state so save/load state is safe and audio is handled properly.
    const bool visible = OverlayVisible();
    if (visible && !paused_)
    {
        try {
            emu.stop();
        } catch (const FlycastException &e) {
            Log("emu.stop (pause) failed: %s", e.what());
        }
        paused_ = true;
    }
    else if (!visible && paused_)
    {
        try {
            emu.start();
        } catch (const FlycastException &e) {
            Log("emu.start (resume) failed: %s", e.what());
        }
        paused_ = false;
    }

    if (paused_)
    {
        // Emulation stopped; frozen frame + overlay are drawn in RenderFrame().
        frameOk_ = false;
        return;
    }

    os_UpdateInputState();
    try {
        frameOk_ = emu.render();
    } catch (const FlycastException &e) {
        Log("render failed: %s", e.what());
        frameOk_ = false;
        exitRequested_ = true;
    }
}

void StandaloneRuntime::RenderFrame()
{
    if (OverlayVisible())
    {
        RenderOverlayPaused();
        return;
    }
    // Normal gameplay: flycast's native OSD is suppressed, so this presents just
    // the game frame.
    if (frameOk_ && imguiDriver != nullptr)
        imguiDriver->present();
}

void StandaloneRuntime::RenderOverlayPaused()
{
    if (!overlay_ || imguiDriver == nullptr)
        return;

    // emu.render() is NOT running while paused, so this is the only swapchain
    // acquire of the frame (no double-acquire deadlock). renderDrawData()
    // re-presents the last game frame underneath, and we draw the overlay on top.
    imguiDriver->newFrame();
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    DrawOverlayContent(overlay_.get());
    ImGui::Render();
    imguiDriver->renderDrawData(ImGui::GetDrawData(), false);
    imguiDriver->present();
}

bool StandaloneRuntime::OverlayVisible() const
{
    return overlay_ && overlay_->IsVisible();
}

void StandaloneRuntime::EnsureOverlay()
{
    if (overlay_ || !contentLoaded_ || imguiDriver == nullptr)
        return;

    // Ensure fonts are loaded even if the gui_initFonts hook didn't fire (e.g.
    // the atlas was already built). ImGui 1.92 rasterizes added fonts on demand.
    LoadTicoOverlayFonts();

    overlayHost_ = std::make_unique<StandaloneOverlayHost>();
    overlay_ = std::make_unique<TicoOverlay>();
    overlay_->SetHost(overlayHost_.get());

    std::string title = settings.content.title;
    if (title.empty())
    {
        title = settings.content.path;
        size_t slash = title.find_last_of("/\\");
        if (slash != std::string::npos)
            title = title.substr(slash + 1);
        size_t dot = title.find_last_of('.');
        if (dot != std::string::npos)
            title = title.substr(0, dot);
    }
    overlay_->SetGameTitle(title.empty() ? "Flycast" : title);
    overlay_->SetDescriptionFont(g_ticoDescFont);
}

bool StandaloneRuntime::ShouldExit() const
{
    if (exitRequested_ || g_osQuit)
        return true;
    // While paused the emulator is intentionally stopped (emu.running()==false);
    // that must not be treated as the game having ended.
    return contentLoaded_ && !paused_ && !emu.running();
}

void StandaloneRuntime::Shutdown()
{
    // Free overlay textures via imguiDriver while it is still alive.
    gui_set_font_hook(nullptr);
    g_ticoTitleFont = nullptr;
    g_ticoDescFont = nullptr;
    overlay_.reset();
    overlayHost_.reset();

    if (contentLoaded_)
        StopEmulation();
    if (mainuiReady_)
        mainui_term();
    flycast_term();
    Log("tico-flycast standalone clean exit");
}

}  // namespace Tico

//==============================================================================
// Flycast host hooks the standalone executable must provide.
//==============================================================================

void os_DoEvents()
{
    if (!appletMainLoop())
        Tico::NotifyOsQuit();
}

namespace hostfs
{
void saveScreenshot(const std::string &name, const std::vector<u8> &data)
{
    throw FlycastException("Not supported on Switch");
}
}  // namespace hostfs

#endif // !LIBRETRO
