/// @file TicoOverlay.h
/// @brief Overlay UI for tico-integrated Flycast
#pragma once

#include "imgui.h"
#include "TicoMain.h"        // Tico::FrameInput / PadButton
#include "TicoOverlayHost.h" // IOverlayHost / RANotification / RAAlertPosition
#include <string>
#include <vector>
#include <memory>

/// @brief Overlay menu types
enum class OverlayMenu
{
    None,
    QuickMenu,
    SaveStates,
    Settings,
    DiscSelect
};

/// @brief Display mode for the emulator viewport
enum class FlycastDisplayMode
{
    Integer = 0, // Integer pixel scaling
    Display = 1, // Aspect-ratio based display
    COUNT = 2
};

/// @brief Display size (context-dependent on FlycastDisplayMode)
///   Integer → 1x, 2x, Auto
///   Display → Stretch, 4:3, 16:9, Original
enum class FlycastDisplaySize
{
    // Display sizes (0-3)
    Stretch = 0,
    _4_3 = 1,
    _16_9 = 2,
    Original = 3,
    // Integer sizes (4-6)
    _1x = 4,
    _2x = 5,
    Auto = 6
};

/// @brief Overlay UI for Flycast with tico styling
class TicoOverlay
{
public:
    TicoOverlay();
    ~TicoOverlay();

    /// @brief Update overlay animation
    void Update(float deltaTime);

    /// @brief Render the overlay
    void Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                int frameWidth, int frameHeight, int fboWidth = 0, int fboHeight = 0);

    /// @brief Compute the on-screen game viewport for the current display
    /// mode/size. Returns the rect (x,y,w,h) in pixels within a screenW×screenH
    /// surface; coreAspect is the core's reported aspect ratio. The libretro
    /// path uses this to letterbox the Vulkan blit, since the game image is
    /// composited by TicoVulkan rather than drawn through ImGui.
    void GetGameViewport(float screenW, float screenH, float coreAspect,
                         float &outX, float &outY, float &outW, float &outH) const;

    /// @brief Handle neutralized input
    /// @return true if input was consumed by overlay
    bool HandleInput(const Tico::FrameInput &input);

    /// @brief Show/hide overlay
    void Show();
    void Hide();
    bool IsVisible() const { return m_currentMenu != OverlayMenu::None; }

    /// @brief Set game title for title card
    void SetGameTitle(const std::string &title) { m_gameTitle = title; }

    /// @brief Set the backend host (emulator + renderer adapter). Triggers a
    /// reload of host-backed assets (avatar texture) now that a renderer exists.
    void SetHost(IOverlayHost *host)
    {
        m_host = host;
        if (m_host)
            LoadAccountData();
    }

    /// @brief Font used for RA alert descriptions (description.ttf). When unset,
    /// the overlay falls back to the atlas' second font / current font.
    void SetDescriptionFont(ImFont *font) { m_descFont = font; }

    /// @brief Check if user wants to exit
    bool ShouldExit() const { return m_shouldExit; }
    void ClearExit() { m_shouldExit = false; }

    /// @brief Check if user wants to reset
    bool ShouldReset() const { return m_shouldReset; }
    void ClearReset() { m_shouldReset = false; }

private:
    void RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                    float aspectRatio, int width, int height,
                    int fboWidth, int fboHeight);
    void RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize);
    void RenderTitleCard(ImDrawList *dl, ImVec2 displaySize);
    void RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize);
    void RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderStatusBar(ImDrawList *dl, ImVec2 displaySize);
    void RenderDiscMenu(ImDrawList *dl, ImVec2 displaySize);
    void ScanForDiscs();
    void RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime);
    void EnsureRAIconLoaded();
    void ResolveNotificationTextures();
    bool m_raIconLoadAttempted = false;

    OverlayMenu m_currentMenu = OverlayMenu::None;
    std::string m_gameTitle;
    IOverlayHost *m_host = nullptr;
    ImFont *m_descFont = nullptr; // description.ttf for RA alert descriptions

    // Animation
    float m_animTimer = 0.0f;

    // Menu state
    int m_quickMenuSelection = 0;
    int m_saveStateSlot = 0;
    bool m_isSaveMode = true;
    int m_settingsSelection = 0;
    FlycastDisplayMode m_displayMode = FlycastDisplayMode::Display;
    FlycastDisplaySize m_displaySize = FlycastDisplaySize::_4_3;
    int m_discSelection = 0;
    float m_discScrollY = 0.0f;
    float m_discTargetScrollY = 0.0f;

    struct DiscEntry {
        std::string displayName;
        std::string romPath;
    };
    std::vector<DiscEntry> m_discs;

    // Settings persistence
    void LoadCoreSettings();
    void SaveCoreSettings();
    void ApplyScalingSettings(bool save = true);

    // Triangle texture
    ImTextureID m_triangleTexture = 0;
    int m_triangleWidth = 0;
    int m_triangleHeight = 0;

    // Directional nav repeat (frame-based; replaces SDL time debounce).
    // Discrete buttons use FrameInput edge bits and need no repeat state.
    uint64_t m_navHeldPrev = 0;
    int m_navRepeatFrames = 0;
    static constexpr int NAV_INITIAL_DELAY_FRAMES = 14;
    static constexpr int NAV_REPEAT_FRAMES = 6;

    // Start+Select opens the overlay only; it never closes it (Back closes), so
    // no combo-edge/flicker state is needed.

    // Exit/Reset flags
    bool m_shouldExit = false;
    bool m_shouldReset = false;
    // Gates the Minus=reset shortcut so the open combo (Start+Select) can't
    // trigger it immediately.
    float m_quickMenuOpenTime = 0.0f;

    // Battery Status
    uint32_t m_batteryLevel = 100;
    bool m_isCharging = false;
    float m_batteryTimer = 0.0f;
    float m_chargingStateProgress = 0.0f;
    ImTextureID m_boltTexture = 0;
    int m_boltWidth = 0;
    int m_boltHeight = 0;

    // Config
    bool m_isDarkMode = true;
    bool m_showNickname = false; // Added
    std::string m_hourFormat = "24h";
    void LoadConfig();
    void LoadGeneralConfig();
    void LoadSVGIcon();

    // Social Area
    ImTextureID m_avatarTexture = 0;
    int m_avatarWidth = 0;
    int m_avatarHeight = 0;
    std::string m_nickname;
    void LoadAccountData();
    bool LoadAvatarTextureFromFile(const char *path);
    bool LoadAvatarTextureFromMemory(const unsigned char *data, size_t size, const char *tag);
    void ReleaseAvatarTexture();
    void RenderSocialArea(ImDrawList *dl, ImVec2 displaySize);
};
