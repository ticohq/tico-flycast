/// @file TicoOverlay.cpp
/// @brief Overlay UI for tico-integrated Flycast
/// Based on EmulatorScreen overlay rendering

// The overlay uses ImVec2 math operators. The libretro target defines this as a
// compile flag; the standalone target does not (and forcing it project-wide
// clashes with implot's own #define), so define it per-TU before imgui.h.
#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "TicoOverlay.h"
#include "TicoConfig.h"
#include "TicoTranslationManager.h"
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <map>

// Use relative path to json.hpp
#include "../core/deps/json/json.hpp"

#include "../core/deps/stb/stb_image.h"

#include <sys/stat.h>
#include <string>

static float OverlayScale()
{
    const float scale = ImGui::GetIO().FontGlobalScale;
    return scale > 0.0f ? scale : 1.0f;
}

#ifdef __SWITCH__
#include <switch.h>
#endif

// Nanosvg for vector icons. Backend-agnostic now: the overlay decodes to RGBA
// and uploads through IOverlayHost::CreateTextureRGBA (TicoVulkan on libretro,
// imguiDriver on the standalone), so there is no GL/Vulkan #ifdef here.
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

// Save-state slot handling now lives behind IOverlayHost (libretro builds a
// per-ROM .state path + calls TicoCore; the standalone uses flycast's native
// save states), so the overlay no longer constructs paths itself.

//==============================================================================
// UI Style Helpers (simplified from UIStyle.h)
//==============================================================================

namespace UIStyle
{

    // Draw text with shadow
    inline void DrawTextWithShadow(ImDrawList *dl, ImVec2 pos, ImU32 color,
                                   const char *text, float shadowOffset = 1.5f)
    {
        ImU32 shadowColor = IM_COL32(0, 0, 0, 50);
        dl->AddText(ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), shadowColor, text);
        dl->AddText(pos, color, text);
    }

    // Draw switch button prompt - Forced Dark Mode Logic (Filled Style)
    static void DrawSwitchButton(ImDrawList *dl, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha, bool isDark)
    {
        // Dark Mode = Filled Light Button with Dark Text
        ImU32 fillCol = IM_COL32(220, 220, 220, (int)(255 * alpha)); // Light Grey Fill
        ImU32 textCol = IM_COL32(40, 40, 40, (int)(255 * alpha));    // Dark Text

        // Filled Circle
        dl->AddCircleFilled(center, size * 0.5f, fillCol, 12);

        // Text inside
        float symSize = fontSize * 0.75f;
        ImVec2 textSize = font->CalcTextSizeA(symSize, FLT_MAX, 0.0f, symbol);

        dl->AddText(font, symSize, center - (textSize * 0.5f), textCol, symbol);
    }

} // namespace UIStyle

//==============================================================================
// Construction
//==============================================================================

TicoOverlay::TicoOverlay()
{
    m_gameTitle = "Flycast";
    LoadConfig();
    LoadGeneralConfig();
    LoadAccountData(); // Load custom avatar/account data

    LoadCoreSettings();
    TicoTranslationManager::Instance().Init();

#ifdef __SWITCH__
    psmInitialize();
#endif
}

TicoOverlay::~TicoOverlay()
{
    ReleaseAvatarTexture();
}

void TicoOverlay::ReleaseAvatarTexture()
{
    if (m_avatarTexture && m_host)
        m_host->DestroyTexture(m_avatarTexture);
    m_avatarTexture = 0;
    m_avatarWidth = 0;
    m_avatarHeight = 0;
}

bool TicoOverlay::LoadAvatarTextureFromMemory(const unsigned char *data, size_t size, const char * /*tag*/)
{
    if (!data || size == 0)
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *rgba = stbi_load_from_memory(data, static_cast<int>(size),
                                                &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0)
    {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }

    ReleaseAvatarTexture();
    ImTextureID texture = m_host ? m_host->CreateTextureRGBA(rgba, width, height) : 0;
    stbi_image_free(rgba);
    if (!texture)
        return false;
    m_avatarTexture = texture;
    m_avatarWidth = width;
    m_avatarHeight = height;
    return true;
}

bool TicoOverlay::LoadAvatarTextureFromFile(const char *path)
{
    if (!path)
        return false;

    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char *rgba = stbi_load(path, &width, &height, &channels, 4);
    if (!rgba || width <= 0 || height <= 0)
    {
        if (rgba)
            stbi_image_free(rgba);
        return false;
    }

    ReleaseAvatarTexture();
    ImTextureID texture = m_host ? m_host->CreateTextureRGBA(rgba, width, height) : 0;
    stbi_image_free(rgba);
    if (!texture)
        return false;
    m_avatarTexture = texture;
    m_avatarWidth = width;
    m_avatarHeight = height;
    return true;
}

void TicoOverlay::LoadConfig()
{
    const char *configPaths[] = {
        "sdmc:/tiicu/config/display.jsonc", // Correct path
        "sdmc:/tico/config/display.jsonc",  // Legacy path
        "tico/config/display.jsonc",
        "assets/config/display.jsonc",
        "../assets/config/display.jsonc"};

    // Default to dark
    m_isDarkMode = true;
    m_showNickname = false;

    FILE *fp = nullptr;
    for (const char *path : configPaths)
    {
        fp = fopen(path, "rb");
        if (fp)
            break;
    }

    // ... rest of LoadConfig ...
    if (fp)
    {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size > 0)
        {
            std::string content;
            content.resize(size);
            fread(&content[0], 1, size, fp);

            try
            {
                nlohmann::json j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded())
                {
                    // Check snake_case (standard) first, then camelCase fallback
                    if (j.contains("dark_mode") && j["dark_mode"].is_boolean())
                    {
                        m_isDarkMode = j["dark_mode"].get<bool>();
                    }
                    else if (j.contains("darkMode") && j["darkMode"].is_boolean())
                    {
                        m_isDarkMode = j["darkMode"].get<bool>();
                    }

                    if (j.contains("show_nickname") && j["show_nickname"].is_boolean())
                    {
                        m_showNickname = j["show_nickname"].get<bool>();
                    }
                    else if (j.contains("showNickname") && j["showNickname"].is_boolean())
                    {
                        m_showNickname = j["showNickname"].get<bool>();
                    }
                }
            }
            catch (...)
            {
                // Parse error
            }
        }
        fclose(fp);
    }
}

void TicoOverlay::LoadGeneralConfig()
{
    const char *configPaths[] = {
        "sdmc:/tiicu/config/general.jsonc",
        "sdmc:/tico/config/general.jsonc",
        "tico/config/general.jsonc",
        "assets/config/general.jsonc",
        "../assets/config/general.jsonc"};

    m_hourFormat = "24h";

    FILE *fp = nullptr;
    for (const char *path : configPaths)
    {
        fp = fopen(path, "rb");
        if (fp)
            break;
    }

    if (fp)
    {
        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (size > 0)
        {
            std::string content;
            content.resize(size);
            fread(&content[0], 1, size, fp);

            try
            {
                nlohmann::json j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded())
                {
                    if (j.contains("hour_format") && j["hour_format"].is_string())
                    {
                        m_hourFormat = j["hour_format"].get<std::string>();
                    }
                }
            }
            catch (...)
            {
            }
        }
        fclose(fp);
    }
}

void TicoOverlay::LoadAccountData()
{
    m_nickname = "Player 1";

#ifdef __SWITCH__
    const char *const avatarPaths[] = {
        "sdmc:/tico/assets/avatar.jpg",
        "sdmc:/tico/assets/avatar.jpeg",
        "sdmc:/tico/assets/avatar.png",
        "sdmc:/tiicu/assets/avatar.jpg",
    };

    for (const char *path : avatarPaths)
    {
        if (LoadAvatarTextureFromFile(path))
            return;
    }

    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc))
        return;

    AccountUid uid = {0};
    bool found = false;

    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid))
        found = true;
    if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid))
        found = true;

    if (!found)
    {
        s32 userCount = 0;
        if (R_SUCCEEDED(accountGetUserCount(&userCount)) && userCount > 0)
        {
            AccountUid uids[ACC_USER_LIST_SIZE];
            s32 actualTotal = 0;
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0)
            {
                uid = uids[0];
                found = accountUidIsValid(&uid);
            }
        }
    }

    if (found)
    {
        AccountProfile profile;
        AccountProfileBase profileBase;
        if (R_SUCCEEDED(accountGetProfile(&profile, uid)))
        {
            if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profileBase)))
            {
                std::string profileName(profileBase.nickname);
                if (!profileName.empty())
                    m_nickname = profileName;
            }

            u32 imageSize = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0)
            {
                std::vector<unsigned char> jpegData(imageSize);
                u32 actualSize = 0;
                if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegData.data(), imageSize, &actualSize)) && actualSize > 0)
                {
                    LoadAvatarTextureFromMemory(jpegData.data(), actualSize, "switch-account-avatar");
                }
            }
            accountProfileClose(&profile);
        }
    }
    accountExit();
#else
    const char *const avatarPaths[] = {
        "tico/assets/avatar.jpg",
        "tico/assets/avatar.jpeg",
        "tico/assets/avatar.png",
        "assets/images/avatar.jpg",
        "assets/images/avatar.jpeg",
        "assets/images/avatar.png",
    };
    for (const char *path : avatarPaths)
    {
        if (LoadAvatarTextureFromFile(path))
            return;
    }
#endif
}

//==============================================================================
// Update
//==============================================================================

void TicoOverlay::Update(float deltaTime)
{
    // Pre-load RA icon texture BEFORE rendering (GL-safe: outside ImGui frame)
    EnsureRAIconLoaded();

    // Resolve any pending notification badge textures from cache (no GL calls)
    ResolveNotificationTextures();

    if (m_currentMenu != OverlayMenu::None)
    {
        m_animTimer += deltaTime;

#ifdef __SWITCH__
        m_batteryTimer += deltaTime;
        if (m_batteryTimer >= 3.0f)
        {
            m_batteryTimer = 0.0f;
            psmGetBatteryChargePercentage(&m_batteryLevel);
            PsmChargerType chargerType;
            psmGetChargerType(&chargerType);
            m_isCharging = (chargerType != PsmChargerType_Unconnected);
        }

        float target = m_isCharging ? 1.0f : 0.0f;
        float diff = target - m_chargingStateProgress;
        if (std::abs(diff) > 0.001f)
        {
            m_chargingStateProgress += diff * deltaTime * 8.0f;
            if (m_chargingStateProgress < 0.0f)
                m_chargingStateProgress = 0.0f;
            if (m_chargingStateProgress > 1.0f)
                m_chargingStateProgress = 1.0f;
        }
#endif
    }
}

//==============================================================================
// Show/Hide
//==============================================================================

void TicoOverlay::Show()
{
    if (m_currentMenu == OverlayMenu::None)
    {
        m_currentMenu = OverlayMenu::QuickMenu;
        m_animTimer = 0.0f;
        m_quickMenuSelection = 0;
        LoadConfig();        // Reload config on open
        LoadGeneralConfig(); // Reload hour format on open
        ScanForDiscs();      // Scan for multi-disc
    }
}

void TicoOverlay::Hide()
{
    m_currentMenu = OverlayMenu::None;
}

//==============================================================================
// Rendering
//==============================================================================

void TicoOverlay::Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                         int frameWidth, int frameHeight, int fboWidth, int fboHeight)
{
    ImDrawList *bgDrawList = ImGui::GetBackgroundDrawList();
    ImDrawList *fgDrawList = ImGui::GetForegroundDrawList();

    // Always render the game
    RenderGame(bgDrawList, displaySize, gameTexture, aspectRatio, frameWidth, frameHeight, fboWidth, fboHeight);

    // Render overlay if visible
    if (m_currentMenu != OverlayMenu::None)
    {
        RenderOverlayBackground(fgDrawList, displaySize);
        RenderTitleCard(fgDrawList, displaySize);

        switch (m_currentMenu)
        {
        case OverlayMenu::QuickMenu:
            RenderQuickMenu(fgDrawList, displaySize);
            break;
        case OverlayMenu::SaveStates:
            RenderSaveStatesMenu(fgDrawList, displaySize);
            break;
        case OverlayMenu::Settings:
            RenderSettingsMenu(fgDrawList, displaySize);
            break;
        case OverlayMenu::DiscSelect:
            RenderDiscMenu(fgDrawList, displaySize);
            break;
        default:
            break;
        }

        RenderHelpersBar(fgDrawList, displaySize);
        RenderSocialArea(fgDrawList, displaySize);
        RenderStatusBar(fgDrawList, displaySize);
    }

    // RA alerts always render (even during gameplay, not just when menu is open)
    RenderRAAlerts(fgDrawList, displaySize, ImGui::GetIO().DeltaTime);
}

void TicoOverlay::RenderSocialArea(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    // Animation: Slide from left (200px to 0px)
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);

    if (ease < 0.01f)
        return;

    const float scale = OverlayScale();
    float startOffset = 200.0f * scale;
    float currentOffset = startOffset * (1.0f - ease); // Moves closer to 0 as ease -> 1

    float AVATAR_SIZE = 72.0f * scale;
    float sideMargin = 32.0f * scale; // As per EmulatorScreen logic for consistency
    float topMargin = 32.0f * scale;
    float barHeight = 50.0f * scale; // Matching StatusBar height

    // Center Y
    ImVec2 avatarCenter(sideMargin + AVATAR_SIZE * 0.5f - currentOffset,
                        topMargin + barHeight * 0.5f);

    // Draw Avatar Circle (NO Shadow per request)
    float radius = AVATAR_SIZE * 0.5f;

    ImU32 baseCol;
    if (m_isDarkMode)
    {
        baseCol = IM_COL32(45, 45, 45, (int)(255 * ease));
    }
    else
    {
        baseCol = IM_COL32(245, 247, 250, (int)(200 * ease));
    }

    dl->AddCircleFilled(avatarCenter, radius, baseCol);

    // Draw Image
    if (m_avatarTexture != 0)
    {
        float imgRadius = radius - 4.0f * scale;
        ImVec2 p_min = ImVec2(avatarCenter.x - imgRadius, avatarCenter.y - imgRadius);
        ImVec2 p_max = ImVec2(avatarCenter.x + imgRadius, avatarCenter.y + imgRadius);

        ImTextureID avatarTexture = m_avatarTexture;
        dl->AddImageRounded(avatarTexture, p_min, p_max,
                            ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, imgRadius);

        dl->AddCircle(avatarCenter, imgRadius, IM_COL32(255, 255, 255, 60), 0, scale);
    }
    else
    {
        float imgRadius = radius - 4.0f * scale;
        dl->AddCircleFilled(avatarCenter, imgRadius, IM_COL32(200, 200, 210, 255));
    }

    // Draw Nickname
    if (m_showNickname && !m_nickname.empty())
    {
        // ... (Nickname drawing logic if needed)
    }
}

void TicoOverlay::RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                             float aspectRatio, int width, int height,
                             int fboWidth, int fboHeight)
{
    if (texture == 0)
        return;

    // Dreamcast base resolution (most common mode)
    static constexpr int DC_BASE_W = 640;
    static constexpr int DC_BASE_H = 480;

    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    float offsetX = 0;
    float offsetY = 0;

    if (m_displayMode == FlycastDisplayMode::Integer)
    {
        // ── Integer Scaling ──────────────────────────────────────────────
        int scale;
        if (m_displaySize == FlycastDisplaySize::Auto)
        {
            int scaleX = (int)displaySize.x / DC_BASE_W;
            int scaleY = (int)displaySize.y / DC_BASE_H;
            scale = (scaleX < scaleY) ? scaleX : scaleY;
            if (scale < 1)
                scale = 1;
        }
        else
        {
            // _1x=4 → scale 1, _2x=5 → scale 2
            scale = (int)m_displaySize - 3;
            if (scale < 1)
                scale = 1;
        }

        dstWidth = DC_BASE_W * scale;
        dstHeight = DC_BASE_H * scale;

        // Clamp to screen
        if (dstWidth > displaySize.x)
            dstWidth = displaySize.x;
        if (dstHeight > displaySize.y)
            dstHeight = displaySize.y;
    }
    else
    {
        // ── Display Mode (aspect-ratio based) ────────────────────────────
        switch (m_displaySize)
        {
        case FlycastDisplaySize::Stretch:
            // Fill entire screen (no aspect correction)
            dstWidth = displaySize.x;
            dstHeight = displaySize.y;
            break;
        case FlycastDisplaySize::_4_3:
        {
            float ar = 4.0f / 3.0f;
            float dstAspect = displaySize.x / displaySize.y;
            if (ar > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / ar;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * ar;
            }
            break;
        }
        case FlycastDisplaySize::_16_9:
        {
            float ar = 16.0f / 9.0f;
            float dstAspect = displaySize.x / displaySize.y;
            if (ar > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / ar;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * ar;
            }
            break;
        }
        case FlycastDisplaySize::Original:
        default:
        {
            // Use the core's reported aspect ratio (fit)
            float dstAspect = displaySize.x / displaySize.y;
            if (aspectRatio > dstAspect)
            {
                dstWidth = displaySize.x;
                dstHeight = displaySize.x / aspectRatio;
            }
            else
            {
                dstHeight = displaySize.y;
                dstWidth = displaySize.y * aspectRatio;
            }
            break;
        }
        }
    }

    // Center on screen
    offsetX = (displaySize.x - dstWidth) / 2.0f;
    offsetY = (displaySize.y - dstHeight) / 2.0f;

    // Black background
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 255));

    // Calculate UV sub-region: core renders to bottom-left of FBO
    // (FBO may be larger than the rendered area, e.g. square max dims)
    float u_max = (fboWidth > 0 && width > 0) ? (float)width / fboWidth : 1.0f;
    float v_max = (fboHeight > 0 && height > 0) ? (float)height / fboHeight : 1.0f;

    // Game texture (V-flipped for OpenGL FBO on Switch)
#ifdef __SWITCH__
    dl->AddImage((ImTextureID)(intptr_t)texture,
                 ImVec2(offsetX, offsetY),
                 ImVec2(offsetX + dstWidth, offsetY + dstHeight),
                 ImVec2(0.0f, v_max), ImVec2(u_max, 0.0f));
#else
    dl->AddImage((ImTextureID)(intptr_t)texture,
                 ImVec2(offsetX, offsetY),
                 ImVec2(offsetX + dstWidth, offsetY + dstHeight),
                 ImVec2(0.0f, 0.0f), ImVec2(u_max, v_max));
#endif
}

void TicoOverlay::RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize)
{
    // Animation ease
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);

    // 3-Part Gradient (20% Top, 60% Center, 20% Bottom)
    // Top/Bottom: Fade to Opaque (250)
    // Center: Base Transparency (200)

    float baseAlphaVal = 200.0f;
    float maxAlphaVal = 250.0f;

    int baseAlpha = (int)(baseAlphaVal * ease);
    int maxAlpha = (int)(maxAlphaVal * ease);

    if (baseAlpha > 0)
    {
        float topH = displaySize.y * 0.20f;
        float botH = displaySize.y * 0.20f;
        float centerH = displaySize.y - topH - botH;

        ImU32 colMax = IM_COL32(0, 0, 0, maxAlpha);
        ImU32 colBase = IM_COL32(0, 0, 0, baseAlpha);

        // Top Band
        dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(displaySize.x, topH),
                                    colMax, colMax, colBase, colBase);

        // Center Band
        dl->AddRectFilled(ImVec2(0, topH), ImVec2(displaySize.x, topH + centerH), colBase);

        // Bottom Band
        dl->AddRectFilledMultiColor(ImVec2(0, displaySize.y - botH), ImVec2(displaySize.x, displaySize.y),
                                    colBase, colBase, colMax, colMax);
    }
}

void TicoOverlay::RenderTitleCard(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    std::string titleStr = m_gameTitle;
    if (m_currentMenu == OverlayMenu::SaveStates)
    {
        titleStr = m_isSaveMode ? tr("emulator_save_state") : tr("emulator_load_state");
    }
    else if (m_currentMenu == OverlayMenu::Settings)
    {
        titleStr = tr("emulator_settings");
    }
    else if (m_currentMenu == OverlayMenu::DiscSelect)
    {
        titleStr = tr("emulator_select_disc");
    }

    // Trim
    titleStr.erase(titleStr.find_last_not_of(" \n\r\t") + 1);
    if (titleStr.length() > 50)
    {
        titleStr = titleStr.substr(0, 47) + "...";
    }

    const float scale = OverlayScale();
    const float TITLE_HEIGHT = 72.0f * scale;
    const float AVAILABLE_TOP_SPACE = 110.0f * scale;

    float cardWidth = std::min(displaySize.x * 0.8f, displaySize.x * 0.4f * scale);
    float cardX = (displaySize.x - cardWidth) * 0.5f;
    float cardY = (AVAILABLE_TOP_SPACE - TITLE_HEIGHT) * 0.5f;

    // Animation: Slide from top
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float startY = -150.0f * scale;
    float currentY = startY + (cardY - startY) * easeOut;

    // NO Shadow, NO Background, NO Border

    // Text: Light Gray (200, 200, 200)
    ImU32 textColor = IM_COL32(200, 200, 200, 255);
    ImVec2 textSize = ImGui::CalcTextSize(titleStr.c_str());

    float textX = cardX + (cardWidth - textSize.x) * 0.5f;
    float textY = currentY + (TITLE_HEIGHT - textSize.y) * 0.5f;

    UIStyle::DrawTextWithShadow(dl, ImVec2(textX, textY), textColor, titleStr.c_str());
}

void TicoOverlay::RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 320.0f * scale;

    std::string menuItems[] = {
        tr("emulator_save_state"),
        tr("emulator_load_state"),
        tr("emulator_settings"),
        tr("emulator_exit_game")};
    const int numItems = 4;

    const float itemHeight = 64.0f * scale;
    const float contentHeight = numItems * itemHeight; // Flush

    ImVec2 menuSize(menuWidth, contentHeight);

    // Animation: Slide from bottom
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    // Opaque Background (Neumorphic)
    ImU32 containerColor;
    if (m_isDarkMode)
    {
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    }
    else
    {
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));
    }

    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    // No Border, No Shadow

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    // Items
    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = (m_quickMenuSelection == i);
        float itemY = menuPos.y + i * itemHeight;
        float inset = 0.0f; // Flush
        ImVec2 itemMin(menuPos.x + inset, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x - inset, itemY + itemHeight);

        // Selection highlight
        if (isSelected)
        {
            ImDrawFlags corners = 0;
            float itemRadius = 0.0f; // Use corner radius if handled by flag, else 0
            if (i == 0)
            {
                corners = ImDrawFlags_RoundCornersTop;
                itemRadius = cornerRadius;
            }
            else if (i == numItems - 1)
            {
                corners = ImDrawFlags_RoundCornersBottom;
                itemRadius = cornerRadius;
            }

            ImU32 selectedColor;
            if (m_isDarkMode)
            {
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            }
            else
            {
                // Light Mode Selected: Darker Grey/White (220, 224, 228) for visibility
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));
            }

            dl->AddRectFilled(itemMin, itemMax, selectedColor, itemRadius, corners);
        }

        // No Dividers

        // Text
        ImVec2 textSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, menuItems[i].c_str());
        float textX = itemMin.x + 20.0f * scale;
        float textY = itemMin.y + (itemHeight - textSize.y) / 2;

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, menuItems[i].c_str());
    }
}

void TicoOverlay::RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 320.0f * scale;
    const int numSlots = 4;
    const float itemHeight = 64.0f * scale;
    const float contentHeight = numSlots * itemHeight; // Flush

    ImVec2 menuSize(menuWidth, contentHeight);

    // Animation: Slide from bottom
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    // Opaque Background
    ImU32 containerColor;
    if (m_isDarkMode)
    {
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    }
    else
    {
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));
    }
    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    for (int i = 0; i < numSlots; i++)
    {
        bool isSelected = (m_saveStateSlot == i);
        float itemY = menuPos.y + i * itemHeight;
        float inset = 0.0f;
        ImVec2 itemMin(menuPos.x + inset, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x - inset, itemY + itemHeight);

        if (isSelected)
        {
            ImDrawFlags corners = 0;
            float itemRadius = 0.0f;
            if (i == 0)
            {
                corners = ImDrawFlags_RoundCornersTop;
                itemRadius = cornerRadius;
            }
            else if (i == numSlots - 1)
            {
                corners = ImDrawFlags_RoundCornersBottom;
                itemRadius = cornerRadius;
            }

            ImU32 selectedColor;
            if (m_isDarkMode)
            {
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            }
            else
            {
                // Light Mode Selected: Darker Grey/White (220, 224, 228) for visibility
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));
            }
            dl->AddRectFilled(itemMin, itemMax, selectedColor, itemRadius, corners);
        }

        // No Dividers

        bool exists = false;
        if (m_host && m_host->IsGameLoaded())
            exists = m_host->StateSlotExists(i);

        char slotText[64];
        snprintf(slotText, sizeof(slotText), tr("emulator_slot").c_str(), i + 1, exists ? tr("emulator_in_use").c_str() : tr("emulator_empty").c_str());

        ImVec2 sz = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, slotText);
        float textX = itemMin.x + 20.0f * scale;
        float textY = itemMin.y + (itemHeight - sz.y) / 2;

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, slotText);
    }
}

// Copied logic from EmulatorScreen
void TicoOverlay::RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 320.0f * scale;
    int numItems = 2; // Display Mode + Size

    const float itemHeight = 64.0f * scale;
    const float contentHeight = numItems * itemHeight;

    ImVec2 menuSize(menuWidth, contentHeight);

    // Animation: Slide from bottom
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    // Opaque Background
    ImU32 containerColor;
    if (m_isDarkMode)
    {
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    }
    else
    {
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));
    }
    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = (m_settingsSelection == i);
        float itemY = menuPos.y + i * itemHeight;
        float inset = 0.0f;
        ImVec2 itemMin(menuPos.x + inset, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x - inset, itemY + itemHeight);

        // Selection Highlight
        if (isSelected)
        {
            ImDrawFlags corners = 0;
            float itemRadius = 0.0f;
            if (i == 0)
            {
                corners = ImDrawFlags_RoundCornersTop;
                itemRadius = cornerRadius;
            }
            else if (i == numItems - 1)
            {
                corners = ImDrawFlags_RoundCornersBottom;
                itemRadius = cornerRadius;
            }

            ImU32 selectedColor;
            if (m_isDarkMode)
            {
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            }
            else
            {
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));
            }
            dl->AddRectFilled(itemMin, itemMax, selectedColor, itemRadius, corners);
        }

        // Label + Value
        std::string label;
        std::string value;

        if (i == 0)
        {
            label = tr("emulator_display_mode");
            value = (m_displayMode == FlycastDisplayMode::Integer) ? tr("emulator_integer") : tr("emulator_display");
        }
        else if (i == 1)
        {
            label = tr("emulator_size");
            if (m_displayMode == FlycastDisplayMode::Integer)
            {
                switch (m_displaySize)
                {
                case FlycastDisplaySize::_1x:
                    value = "1x";
                    break;
                case FlycastDisplaySize::_2x:
                    value = "2x";
                    break;
                case FlycastDisplaySize::Auto:
                    value = tr("emulator_auto");
                    break;
                default:
                    value = tr("emulator_auto");
                    break;
                }
            }
            else
            {
                switch (m_displaySize)
                {
                case FlycastDisplaySize::Stretch:
                    value = tr("emulator_stretch");
                    break;
                case FlycastDisplaySize::_4_3:
                    value = "4:3";
                    break;
                case FlycastDisplaySize::_16_9:
                    value = "16:9";
                    break;
                case FlycastDisplaySize::Original:
                    value = tr("emulator_original");
                    break;
                default:
                    value = "4:3";
                    break;
                }
            }
        }

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        float textX = itemMin.x + 20.0f * scale;
        ImVec2 labelSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, label.c_str());
        float textY = itemMin.y + (itemHeight - labelSize.y) / 2;
        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, label.c_str());

        ImVec2 valueSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, value.c_str());
        float valueX = itemMax.x - valueSize.x - 40.0f * scale;
        dl->AddText(font, smallFontSize, ImVec2(valueX, textY), textColor, value.c_str());

        // Draw Triangle Arrows
        if (isSelected)
        {
            float arrowSize = 12.0f * scale;
            float arrowY = itemMin.y + (itemHeight - arrowSize) / 2;

            // Left Arrow
            float leftArrowX = valueX - arrowSize - 12.0f * scale;
            ImVec2 lp1 = ImVec2(leftArrowX, arrowY + arrowSize / 2);
            ImVec2 lp2 = ImVec2(leftArrowX + arrowSize, arrowY);
            ImVec2 lp3 = ImVec2(leftArrowX + arrowSize, arrowY + arrowSize);
            dl->AddTriangleFilled(lp1, lp2, lp3, textColor);

            // Right Arrow
            float rightArrowX = valueX + valueSize.x + 12.0f * scale;
            ImVec2 rp1 = ImVec2(rightArrowX + arrowSize, arrowY + arrowSize / 2);
            ImVec2 rp2 = ImVec2(rightArrowX, arrowY);
            ImVec2 rp3 = ImVec2(rightArrowX, arrowY + arrowSize);
            dl->AddTriangleFilled(rp1, rp2, rp3, textColor);
        }
    }
}

void TicoOverlay::RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize)
{
    const float scale = OverlayScale();

    // Animation
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    // Config
    const float BAR_HEIGHT = 48.0f * scale;
    const float MARGIN_BOTTOM = 24.0f * scale;
    const float PADDING = 16.0f * scale;
    const float BUTTON_SIZE = 22.0f * scale;
    const float ITEM_SPACING = 12.0f * scale;

    ImFont *font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize() * 0.78f;

    // Helpers definitions
    struct Helper
    {
        const char *btn;
        std::string desc;
    };

    std::vector<Helper> helpers;

    if (m_currentMenu == OverlayMenu::QuickMenu)
    {
        helpers.push_back({"-", tr("emulator_reset")});
        if (!m_discs.empty())
        {
            helpers.push_back({"X", tr("emulator_disc")});
        }
    }
    else if (m_currentMenu == OverlayMenu::Settings)
    {
        // Settings helpers
        helpers.push_back({"DPAD", tr("emulator_change")});
    }

    helpers.push_back({"B", tr("emulator_back")});
    helpers.push_back({"A", tr("emulator_select")});

    // Calculate total width
    float totalWidth = PADDING * 2;
    for (size_t i = 0; i < helpers.size(); i++)
    {
        float textW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, helpers[i].desc.c_str()).x;
        totalWidth += BUTTON_SIZE + 8.0f * scale + textW;
        if (i < helpers.size() - 1)
            totalWidth += ITEM_SPACING;
    }

    // Animate from right
    float startOffset = 400.0f * scale;
    float currentOffset = startOffset * (1.0f - easeOut);

    float barX = displaySize.x - totalWidth - 20.0f * scale + currentOffset;
    float barY = displaySize.y - MARGIN_BOTTOM - BAR_HEIGHT;

    // NO Background
    // NO Border

    // Draw items
    float cursorX = barX + PADDING;
    float centerY = barY + BAR_HEIGHT * 0.5f;

    // FORCED DARK MODE: User request "HELPERS BARSS. I WANT ALWAYS DARK MODE"
    // Because Overlay is Black/Dark.

    for (const auto &h : helpers)
    {
        // Draw Button
        ImVec2 btnPos(cursorX + BUTTON_SIZE * 0.5f, centerY);
        // Force isDark = true
        UIStyle::DrawSwitchButton(dl, font, fontSize, btnPos, BUTTON_SIZE, h.btn, easeOut, true);

        cursorX += BUTTON_SIZE + 8.0f * scale;

        // Draw Text
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, h.desc.c_str());
        float textY = centerY - textSize.y * 0.5f;

        // Force Light Text
        ImU32 textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));

        dl->AddText(font, fontSize, ImVec2(cursorX, textY), textColor, h.desc.c_str());

        cursorX += textSize.x + ITEM_SPACING;
    }
}

//==============================================================================
// Input Handling
//==============================================================================

bool TicoOverlay::HandleInput(const Tico::FrameInput &input)
{
    using namespace Tico;

    const uint64_t buttons = input.buttons;
    const uint64_t pressed = input.pressed;

    // Start+Select ONLY ever opens the overlay — it never closes it (closing is
    // done with the Back button). This makes accidental closes from input-poll
    // flicker impossible. While the combo is held we consume it so it can't be
    // misread by the menu handlers below.
    const bool comboDown = (buttons & Pad_Start) && (buttons & Pad_Select);
    if (comboDown)
    {
        if (m_currentMenu == OverlayMenu::None)
            Show();
        return true;
    }

    // If overlay not visible, don't consume input
    if (m_currentMenu == OverlayMenu::None)
        return false;

    // Directional navigation: merge D-pad + left stick into a held bitmask,
    // then derive edge + hold-repeat without any time API.
    uint64_t dirHeld = 0;
    if ((buttons & Pad_Up) || input.leftStickY < -16000) dirHeld |= Pad_Up;
    if ((buttons & Pad_Down) || input.leftStickY > 16000) dirHeld |= Pad_Down;
    if ((buttons & Pad_Left) || input.leftStickX < -16000) dirHeld |= Pad_Left;
    if ((buttons & Pad_Right) || input.leftStickX > 16000) dirHeld |= Pad_Right;

    uint64_t dirFire = dirHeld & ~m_navHeldPrev; // new presses fire instantly
    if (dirHeld != 0 && dirHeld == m_navHeldPrev)
    {
        if (--m_navRepeatFrames <= 0)
        {
            dirFire |= dirHeld;
            m_navRepeatFrames = NAV_REPEAT_FRAMES;
        }
    }
    else if (dirFire != 0)
    {
        m_navRepeatFrames = NAV_INITIAL_DELAY_FRAMES;
    }
    m_navHeldPrev = dirHeld;

    bool upPressed = (dirFire & Pad_Up) != 0;
    bool downPressed = (dirFire & Pad_Down) != 0;
    bool leftPressed = (dirFire & Pad_Left) != 0;
    bool rightPressed = (dirFire & Pad_Right) != 0;

    // Confirm = B, Back = A, disc select = Y (edge-triggered).
    bool confirmPressed = (pressed & Pad_B) != 0;
    bool backPressed = (pressed & Pad_A) != 0;
    bool xPressed = (pressed & Pad_Y) != 0;

    // X button for disc select
    if (xPressed && m_currentMenu == OverlayMenu::QuickMenu && !m_discs.empty())
    {
        m_currentMenu = OverlayMenu::DiscSelect;
        m_discSelection = 0;
        m_discScrollY = 0.0f;
        m_discTargetScrollY = 0.0f;
        m_animTimer = 0.0f;
        return true;
    }

    // Up
    if (upPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            m_quickMenuSelection = (m_quickMenuSelection + 3) % 4; // 4 items (Settings added)
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            m_saveStateSlot = (m_saveStateSlot + 3) % 4;
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            m_settingsSelection = (m_settingsSelection + 1) % 2;
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect && !m_discs.empty())
        {
            m_discSelection--;
            if (m_discSelection < 0) m_discSelection = (int)m_discs.size() - 1;
            // Scroll
            float itemHeight = 64.0f * OverlayScale();
            int maxVisible = 4;
            float contentHeight = maxVisible * itemHeight;
            float maxScroll = (float)m_discs.size() * itemHeight - contentHeight;
            if (maxScroll < 0) maxScroll = 0;
            m_discTargetScrollY = m_discSelection * itemHeight - contentHeight / 2 + itemHeight / 2;
            if (m_discTargetScrollY < 0) m_discTargetScrollY = 0;
            if (m_discTargetScrollY > maxScroll) m_discTargetScrollY = maxScroll;
            m_discScrollY = m_discTargetScrollY;
        }
    }
    // Down
    if (downPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            m_quickMenuSelection = (m_quickMenuSelection + 1) % 4;
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            m_saveStateSlot = (m_saveStateSlot + 1) % 4;
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            m_settingsSelection = (m_settingsSelection + 1) % 2;
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect && !m_discs.empty())
        {
            m_discSelection++;
            if (m_discSelection >= (int)m_discs.size()) m_discSelection = 0;
            // Scroll
            float itemHeight = 64.0f * OverlayScale();
            int maxVisible = 4;
            float contentHeight = maxVisible * itemHeight;
            float maxScroll = (float)m_discs.size() * itemHeight - contentHeight;
            if (maxScroll < 0) maxScroll = 0;
            m_discTargetScrollY = m_discSelection * itemHeight - contentHeight / 2 + itemHeight / 2;
            if (m_discTargetScrollY < 0) m_discTargetScrollY = 0;
            if (m_discTargetScrollY > maxScroll) m_discTargetScrollY = maxScroll;
            m_discScrollY = m_discTargetScrollY;
        }
    }
    // Left/Right (Settings Only)
    bool directionChanged = false;
    int direction = 0;

    if (leftPressed)
    {
        direction = -1;
        directionChanged = true;
    }
    if (rightPressed)
    {
        direction = 1;
        directionChanged = true;
    }

    if (directionChanged && m_currentMenu == OverlayMenu::Settings)
    {
        if (m_settingsSelection == 0)
        {
            // Display Mode: toggle Integer ↔ Display
            if (m_displayMode == FlycastDisplayMode::Display)
                m_displayMode = FlycastDisplayMode::Integer;
            else
                m_displayMode = FlycastDisplayMode::Display;
            // Reset Size to first valid option for new mode
            if (m_displayMode == FlycastDisplayMode::Integer)
                m_displaySize = FlycastDisplaySize::Auto;
            else
                m_displaySize = FlycastDisplaySize::_4_3;
            ApplyScalingSettings(true);
        }
        else if (m_settingsSelection == 1)
        {
            // Size: cycle through context-dependent options
            if (m_displayMode == FlycastDisplayMode::Integer)
            {
                // Integer sizes: _1x(4), _2x(5), Auto(6)
                int s = (int)m_displaySize;
                s += direction;
                if (s < 4)
                    s = 6; // wrap to Auto
                if (s > 6)
                    s = 4; // wrap to _1x
                m_displaySize = (FlycastDisplaySize)s;
            }
            else
            {
                // Display sizes: Stretch(0), 4:3(1), 16:9(2), Original(3)
                int s = (int)m_displaySize;
                s += direction;
                if (s < 0)
                    s = 3; // wrap to Original
                if (s > 3)
                    s = 0; // wrap to Stretch
                m_displaySize = (FlycastDisplaySize)s;
            }
            ApplyScalingSettings(true);
        }
    }

    // Confirm (Logic for entering submenus or toggling)
    if (confirmPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            switch (m_quickMenuSelection)
            {
            case 0: // Save State
                m_isSaveMode = true;
                m_currentMenu = OverlayMenu::SaveStates;
                m_animTimer = 0.0f;
                break;
            case 1: // Load State
                m_isSaveMode = false;
                m_currentMenu = OverlayMenu::SaveStates;
                m_animTimer = 0.0f;
                break;
            case 2: // Settings
                m_currentMenu = OverlayMenu::Settings;
                m_settingsSelection = 0;
                m_animTimer = 0.0f;
                break;
            case 3: // Exit
                m_shouldExit = true;
                break;
            }
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            if (m_host)
            {
                if (m_isSaveMode)
                {
                    m_host->SaveStateSlot(m_saveStateSlot);
                    m_currentMenu = OverlayMenu::QuickMenu;
                }
                else
                {
                    m_host->LoadStateSlot(m_saveStateSlot);
                    Hide();
                    m_animTimer = 0.4f;
                    return true;
                }
            }
            else
            {
                m_currentMenu = OverlayMenu::QuickMenu;
            }
            m_animTimer = 0.4f;
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            // Confirm acts like Right
            if (m_settingsSelection == 0)
            {
                if (m_displayMode == FlycastDisplayMode::Display)
                    m_displayMode = FlycastDisplayMode::Integer;
                else
                    m_displayMode = FlycastDisplayMode::Display;
                if (m_displayMode == FlycastDisplayMode::Integer)
                    m_displaySize = FlycastDisplaySize::Auto;
                else
                    m_displaySize = FlycastDisplaySize::_4_3;
                ApplyScalingSettings(true);
            }
            else if (m_settingsSelection == 1)
            {
                if (m_displayMode == FlycastDisplayMode::Integer)
                {
                    int s = (int)m_displaySize;
                    s = (s >= 6) ? 4 : s + 1;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                else
                {
                    int s = (int)m_displaySize;
                    s = (s >= 3) ? 0 : s + 1;
                    m_displaySize = (FlycastDisplaySize)s;
                }
                ApplyScalingSettings(true);
            }
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            // Swap disc
            if (m_host && !m_discs.empty())
            {
                m_host->SwapDisc(m_discs[m_discSelection].romPath);
                Hide();
                m_animTimer = 0.4f;
                return true;
            }
        }
    }
    // Back
    if (backPressed)
    {
        if (m_currentMenu == OverlayMenu::QuickMenu)
        {
            Hide();
        }
        else if (m_currentMenu == OverlayMenu::SaveStates)
        {
            m_currentMenu = OverlayMenu::QuickMenu;
            m_animTimer = 0.4f;
        }
        else if (m_currentMenu == OverlayMenu::Settings)
        {
            m_currentMenu = OverlayMenu::QuickMenu;
            m_animTimer = 0.4f;
        }
        else if (m_currentMenu == OverlayMenu::DiscSelect)
        {
            m_currentMenu = OverlayMenu::QuickMenu;
            m_animTimer = 0.4f;
        }
    }

    return true;
}

//==============================================================================
// Disc Scanning & Menu
//==============================================================================

static std::string NormalizeDiscPath(const std::string &path)
{
    std::string result = path;
    for (char &c : result)
    {
        if (c == '\\')
            c = '/';
    }
    return result;
}

static int GetDiscExtensionPriority(const std::string &ext)
{
    if (ext == ".chd") return 1;
    if (ext == ".cue") return 2;
    if (ext == ".gdi") return 3;
    if (ext == ".cdi") return 4;
    if (ext == ".iso") return 5;
    return 99;
}

void TicoOverlay::ScanForDiscs()
{
    m_discs.clear();
    m_discSelection = 0;
    m_discScrollY = 0.0f;
    m_discTargetScrollY = 0.0f;

    if (!m_host) return;
    std::string currentPath = m_host->GetGamePath();
    if (currentPath.empty()) return;

    // Strip quotes if present
    if (currentPath.front() == '"' && currentPath.back() == '"')
    {
        currentPath = currentPath.substr(1, currentPath.size() - 2);
    }

    currentPath = NormalizeDiscPath(currentPath);

    std::string dirname;
    std::string basename;
    size_t lastSlash = currentPath.find_last_of("/\\");
    if (lastSlash != std::string::npos)
    {
        dirname = currentPath.substr(0, lastSlash);
        basename = currentPath.substr(lastSlash + 1);
    }
    else
    {
        dirname = ".";
        basename = currentPath;
    }

    std::string lowerBase = basename;
    std::transform(lowerBase.begin(), lowerBase.end(), lowerBase.begin(), ::tolower);

    // M3U Parsing
    if (lowerBase.length() >= 4 && lowerBase.substr(lowerBase.length() - 4) == ".m3u")
    {
        FILE *fp = fopen(currentPath.c_str(), "r");
        if (fp)
        {
            char line[1024];
            int discIndex = 1;
            while (fgets(line, sizeof(line), fp))
            {
                std::string strLine(line);
                strLine.erase(strLine.find_last_not_of(" \n\r\t") + 1);
                size_t startpos = strLine.find_first_not_of(" \n\r\t");
                if (std::string::npos != startpos)
                    strLine = strLine.substr(startpos);
                else
                    strLine.clear();

                if (strLine.empty() || strLine[0] == '#') continue;

                std::string discRelPath = dirname + "/" + strLine;
                std::string normalizedPath = NormalizeDiscPath(discRelPath);

                DiscEntry entry;
                entry.displayName = tr("emulator_disc") + " " + std::to_string(discIndex);
                entry.romPath = normalizedPath;
                m_discs.push_back(entry);
                discIndex++;
            }
            fclose(fp);
            return;
        }
    }

    // Check for disc pattern in current filename
    const char *keywords[] = {"disc", "disk", "cd"};
    bool foundPat = false;
    for (const char *kw : keywords)
    {
        for (int n = 1; n <= 10; n++)
        {
            std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
            if (lowerBase.find(pattern) != std::string::npos)
            {
                foundPat = true;
                break;
            }
        }
        if (foundPat) break;
    }

    if (!foundPat) return;

    // Extract prefix (everything before first parenthesis)
    size_t firstParen = lowerBase.find('(');
    std::string prefix = lowerBase;
    if (firstParen != std::string::npos)
        prefix = lowerBase.substr(0, firstParen);
    prefix.erase(prefix.find_last_not_of(" \n\r\t") + 1);

    // Determine directories to scan
    std::vector<std::string> scanDirs;
    scanDirs.push_back(dirname);

    std::string lowerDir = dirname;
    std::transform(lowerDir.begin(), lowerDir.end(), lowerDir.begin(), ::tolower);
    bool isNested = false;
    for (const char *kw : keywords)
    {
        if (lowerDir.find(kw) != std::string::npos)
        {
            isNested = true;
            break;
        }
    }
    if (isNested)
        scanDirs.push_back(dirname + "/..");

    // Scan directories
    std::map<std::string, DiscEntry> bestDiscs;

    for (const auto &scanDir : scanDirs)
    {
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir(scanDir.c_str())) != NULL)
        {
            while ((ent = readdir(dir)) != NULL)
            {
                std::string filename = ent->d_name;
                if (filename == "." || filename == "..") continue;

                std::string currentFileDir = scanDir;
                bool isSubDirItem = false;

                if (ent->d_type == DT_DIR)
                {
                    currentFileDir = scanDir + "/" + filename;
                    isSubDirItem = true;
                }

                DIR *subDir = NULL;
                struct dirent *subEnt = NULL;
                bool hasSubDir = false;

                if (isSubDirItem)
                {
                    subDir = opendir(currentFileDir.c_str());
                    if (subDir)
                    {
                        hasSubDir = true;
                        subEnt = readdir(subDir);
                    }
                    else
                        continue;
                }
                else
                {
                    subEnt = ent;
                }

                while (subEnt != NULL)
                {
                    std::string actualFilename = subEnt->d_name;
                    if (actualFilename == "." || actualFilename == "..")
                    {
                        if (hasSubDir) { subEnt = readdir(subDir); continue; }
                        else break;
                    }

                    std::string lowerFilename = actualFilename;
                    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);

                    std::string extFound = "";
                    size_t lastDot = lowerFilename.find_last_of('.');
                    if (lastDot != std::string::npos)
                        extFound = lowerFilename.substr(lastDot);

                    int priority = GetDiscExtensionPriority(extFound);

                    // Strip extension for prefix comparison
                    std::string lowerNameNoExt = lowerFilename;
                    if (lastDot != std::string::npos) lowerNameNoExt = lowerFilename.substr(0, lastDot);

                    // Must start with prefix (not just contain it)
                    if (priority <= 5 && lowerNameNoExt.find(prefix) == 0)
                    {
                        // Reject extra title words between prefix and first '('
                        std::string afterPrefix = lowerNameNoExt.substr(prefix.size());

                        // Remove anything inside [] entirely
                        int bracketDepth = 0;
                        std::string cleanAfterPrefix;
                        for (size_t i = 0; i < afterPrefix.length(); i++)
                        {
                            if (afterPrefix[i] == '[') bracketDepth++;
                            else if (afterPrefix[i] == ']') bracketDepth = std::max(0, bracketDepth - 1);
                            else if (bracketDepth == 0) cleanAfterPrefix += afterPrefix[i];
                        }

                        size_t parenPos = cleanAfterPrefix.find('(');
                        std::string beforeParen = (parenPos != std::string::npos)
                                                      ? cleanAfterPrefix.substr(0, parenPos)
                                                      : cleanAfterPrefix;
                        beforeParen.erase(beforeParen.find_last_not_of(" \t") + 1);
                        beforeParen.erase(0, beforeParen.find_first_not_of(" \t"));

                        if (beforeParen.empty())
                        {
                            bool foundDiscForFile = false;
                            for (const char *kw : keywords)
                            {
                                for (int n = 1; n <= 10; n++)
                                {
                                    std::string pattern = std::string("(") + kw + " " + std::to_string(n) + ")";
                                    if (lowerFilename.find(pattern) != std::string::npos)
                                    {
                                        DiscEntry entry;
                                        entry.displayName = tr("emulator_disc") + " " + std::to_string(n);
                                        entry.romPath = NormalizeDiscPath(currentFileDir + "/" + actualFilename);

                                        if (bestDiscs.find(entry.displayName) == bestDiscs.end())
                                        {
                                            bestDiscs[entry.displayName] = entry;
                                        }
                                        else
                                        {
                                            std::string existingExt = "";
                                            std::string existingLower = bestDiscs[entry.displayName].romPath;
                                            std::transform(existingLower.begin(), existingLower.end(), existingLower.begin(), ::tolower);
                                            size_t extDot = existingLower.find_last_of('.');
                                            if (extDot != std::string::npos) existingExt = existingLower.substr(extDot);

                                            if (priority < GetDiscExtensionPriority(existingExt))
                                                bestDiscs[entry.displayName] = entry;
                                        }
                                        foundDiscForFile = true;
                                        break;
                                    }
                                }
                                if (foundDiscForFile) break;
                            }
                        }
                    }

                    if (hasSubDir)
                        subEnt = readdir(subDir);
                    else
                        break;
                }

                if (hasSubDir && subDir)
                    closedir(subDir);
            }
            closedir(dir);
        }
    }

    for (const auto &pair : bestDiscs)
        m_discs.push_back(pair.second);

    // Sort by display name
    std::sort(m_discs.begin(), m_discs.end(), [](const DiscEntry &a, const DiscEntry &b) {
        return a.displayName < b.displayName;
    });

    // Set selection to current disc
    for (int i = 0; i < (int)m_discs.size(); i++)
    {
        if (m_discs[i].romPath == currentPath)
        {
            m_discSelection = i;
            break;
        }
    }
}

void TicoOverlay::RenderDiscMenu(ImDrawList *dl, ImVec2 displaySize)
{
    float scale = OverlayScale();
    const float menuWidth = 320.0f * scale;
    const float itemHeight = 64.0f * scale;
    const int numItems = m_discs.empty() ? 1 : (int)m_discs.size();

    const int maxVisible = 4;
    const int visibleItems = std::min(numItems, maxVisible);
    const float paddingY = 16.0f * scale;
    const float contentHeight = visibleItems * itemHeight + (paddingY * 2.0f);

    ImVec2 menuSize(menuWidth, contentHeight);

    float t = m_animTimer / 0.4f;
    if (t > 1.0f) t = 1.0f;
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);

    float targetY = (displaySize.y - menuSize.y) / 2.0f;
    float startY = displaySize.y + (100.0f * scale);
    float currentY = startY + (targetY - startY) * easeOut;

    ImVec2 menuPos((displaySize.x - menuSize.x) / 2.0f, currentY);
    ImVec2 p0 = menuPos;
    ImVec2 p1 = ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y);

    const float cornerRadius = 16.0f * scale;

    ImU32 containerColor;
    if (m_isDarkMode)
        containerColor = IM_COL32(45, 45, 45, (int)(255 * easeOut));
    else
        containerColor = IM_COL32(242, 245, 248, (int)(255 * easeOut));

    dl->AddRectFilled(p0, p1, containerColor, cornerRadius);

    ImFont *font = ImGui::GetFont();
    float baseFontSize = ImGui::GetFontSize();
    float smallFontSize = baseFontSize * 0.85f;

    ImVec2 clipP0(p0.x, p0.y + paddingY);
    ImVec2 clipP1(p1.x, p1.y - paddingY);
    dl->PushClipRect(clipP0, clipP1, true);

    for (int i = 0; i < numItems; i++)
    {
        bool isSelected = m_discs.empty() ? true : (m_discSelection == i);
        float itemY = clipP0.y + i * itemHeight - m_discScrollY;

        if (itemY + itemHeight < clipP0.y || itemY > clipP1.y)
            continue;

        ImVec2 itemMin(menuPos.x, itemY);
        ImVec2 itemMax(menuPos.x + menuSize.x, itemY + itemHeight);

        if (isSelected)
        {
            ImU32 selectedColor;
            if (m_isDarkMode)
                selectedColor = IM_COL32(60, 60, 60, (int)(255 * easeOut));
            else
                selectedColor = IM_COL32(190, 195, 205, (int)(255 * easeOut));

            dl->AddRectFilled(itemMin, itemMax, selectedColor);
        }

        std::string displayName = m_discs.empty() ? tr("emulator_no_discs") : m_discs[i].displayName;
        ImVec2 textSize = font->CalcTextSizeA(smallFontSize, FLT_MAX, 0.0f, displayName.c_str());
        float textX = itemMin.x + 20.0f * scale;
        float textY = itemMin.y + (itemHeight - textSize.y) / 2;

        ImU32 textColor;
        if (m_isDarkMode)
        {
            if (isSelected)
                textColor = IM_COL32(255, 255, 255, (int)(255 * easeOut));
            else
                textColor = IM_COL32(200, 200, 200, (int)(255 * easeOut));
        }
        else
        {
            if (isSelected)
                textColor = IM_COL32(60, 60, 70, (int)(255 * easeOut));
            else
                textColor = IM_COL32(90, 90, 100, (int)(255 * easeOut));
        }

        dl->AddText(font, smallFontSize, ImVec2(textX, textY), textColor, displayName.c_str());
    }

    dl->PopClipRect();

    // Scroll shadow indicators
    bool needsScroll = numItems > maxVisible;
    if (needsScroll)
    {
        float maxScroll = (float)m_discs.size() * itemHeight - contentHeight + (paddingY * 2.0f);
        float shadowH = 20.0f * scale;
        ImU32 shadowStart = IM_COL32(0, 0, 0, (int)(40 * easeOut));
        ImU32 shadowEnd = IM_COL32(0, 0, 0, 0);

        if (m_discScrollY > 1.0f)
        {
            float y1 = clipP0.y;
            float y2 = clipP0.y + shadowH;
            dl->AddRectFilledMultiColor(ImVec2(menuPos.x, y1), ImVec2(menuPos.x + menuSize.x, y2),
                                        shadowStart, shadowStart, shadowEnd, shadowEnd);
        }

        if (m_discScrollY < maxScroll - 1.0f)
        {
            float y1 = clipP1.y - shadowH;
            float y2 = clipP1.y;
            dl->AddRectFilledMultiColor(ImVec2(menuPos.x, y1), ImVec2(menuPos.x + menuSize.x, y2),
                                        shadowEnd, shadowEnd, shadowStart, shadowStart);
        }
    }
}

//==============================================================================
// Settings Persistence
//==============================================================================

void TicoOverlay::LoadCoreSettings()
{
    // Read from the shared core config (same file the launcher settings uses)
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/flycast.jsonc";
#else
    std::string configPath = "tico/config/cores/flycast.jsonc";
#endif

    std::ifstream file(configPath);
    if (file.is_open())
    {
        try
        {
            auto j = nlohmann::json::parse(file, nullptr, false, true);
            file.close();

            if (!j.is_discarded())
            {
                // display_mode: "Integer" or "Display"
                if (j.contains("display_mode") && j["display_mode"].is_string())
                {
                    std::string v = j["display_mode"].get<std::string>();
                    if (v == "Integer")
                        m_displayMode = FlycastDisplayMode::Integer;
                    else
                        m_displayMode = FlycastDisplayMode::Display;
                }
                else
                {
                    m_displayMode = FlycastDisplayMode::Display;
                }

                // display_size: context-dependent string
                if (j.contains("display_size") && j["display_size"].is_string())
                {
                    std::string v = j["display_size"].get<std::string>();
                    if (v == "Stretch")
                        m_displaySize = FlycastDisplaySize::Stretch;
                    else if (v == "16:9")
                        m_displaySize = FlycastDisplaySize::_16_9;
                    else if (v == "Original")
                        m_displaySize = FlycastDisplaySize::Original;
                    else if (v == "1x")
                        m_displaySize = FlycastDisplaySize::_1x;
                    else if (v == "2x")
                        m_displaySize = FlycastDisplaySize::_2x;
                    else if (v == "Auto")
                        m_displaySize = FlycastDisplaySize::Auto;
                    else
                        m_displaySize = FlycastDisplaySize::_4_3;
                }
                else
                {
                    m_displaySize = FlycastDisplaySize::_4_3;
                }
            }
            else
            {
                m_displayMode = FlycastDisplayMode::Display;
                m_displaySize = FlycastDisplaySize::_4_3;
            }
        }
        catch (...)
        {
            m_displayMode = FlycastDisplayMode::Display;
            m_displaySize = FlycastDisplaySize::_4_3;
        }
    }
    else
    {
        m_displayMode = FlycastDisplayMode::Display;
        m_displaySize = FlycastDisplaySize::_4_3;
    }

    ApplyScalingSettings(false);
}

void TicoOverlay::SaveCoreSettings()
{
    // Merge into the shared core config (same file the launcher settings uses)
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/flycast.jsonc";
#else
    std::string configPath = "tico/config/cores/flycast.jsonc";
#endif

    try
    {
        // Read existing config first so we only update our keys
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream in(configPath);
            if (in.is_open())
            {
                auto parsed = nlohmann::json::parse(in, nullptr, false, true);
                in.close();
                if (!parsed.is_discarded())
                    j = parsed;
            }
        }

        // display_mode → string
        j["display_mode"] = (m_displayMode == FlycastDisplayMode::Integer) ? "Integer" : "Display";

        // display_size → string
        const char *sizeStr = "4:3";
        switch (m_displaySize)
        {
        case FlycastDisplaySize::Stretch:
            sizeStr = "Stretch";
            break;
        case FlycastDisplaySize::_4_3:
            sizeStr = "4:3";
            break;
        case FlycastDisplaySize::_16_9:
            sizeStr = "16:9";
            break;
        case FlycastDisplaySize::Original:
            sizeStr = "Original";
            break;
        case FlycastDisplaySize::_1x:
            sizeStr = "1x";
            break;
        case FlycastDisplaySize::_2x:
            sizeStr = "2x";
            break;
        case FlycastDisplaySize::Auto:
            sizeStr = "Auto";
            break;
        default:
            break;
        }
        j["display_size"] = sizeStr;

        std::ofstream out(configPath);
        if (out.is_open())
        {
            out << j.dump(4);
            out.close();
        }
    }
    catch (...)
    {
    }
}

void TicoOverlay::ApplyScalingSettings(bool save)
{
    if (save)
    {
        SaveCoreSettings();
    }
}

// Helper to load SVG
void TicoOverlay::LoadSVGIcon()
{
    if (!m_host)
        return;

    // Embed SVG to avoid file path issues
    const char *svgContent = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><path fill="#FFFFFF" d="M338.8-9.9c11.9 8.6 16.3 24.2 10.9 37.8L271.3 224 416 224c13.5 0 25.5 8.4 30.1 21.1s.7 26.9-9.6 35.5l-288 240c-11.3 9.4-27.4 9.9-39.3 1.3s-16.3-24.2-10.9-37.8L176.7 288 32 288c-13.5 0-25.5-8.4-30.1-21.1s-.7-26.9 9.6-35.5l288-240c11.3-9.4 27.4-9.9 39.3-1.3z"/></svg>)";

    char *input = strdup(svgContent);
    if (!input)
        return;

    NSVGimage *image = nsvgParse(input, "px", 96);
    free(input);
    if (!image)
        return;

    float scale = 64.0f / image->height;
    int w = (int)(image->width * scale);
    int h = (int)(image->height * scale);

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast)
    {
        nsvgDelete(image);
        return;
    }

    unsigned char *img = (unsigned char *)malloc(w * h * 4);
    if (!img)
    {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(image);
        return;
    }

    nsvgRasterize(rast, image, 0, 0, scale, img, w, h, w * 4);

    if (m_boltTexture)
        m_host->DestroyTexture(m_boltTexture);
    m_boltTexture = m_host->CreateTextureRGBA(img, w, h);
    m_boltWidth = w;
    m_boltHeight = h;

    free(img);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
}

// ... existing RenderStatusBar ...

void TicoOverlay::RenderStatusBar(ImDrawList *dl, ImVec2 displaySize)
{
    if (m_animTimer <= 0.0f)
        return;

    const float scale = OverlayScale();

    // Animation: Fade in
    float t = m_animTimer / 0.4f;
    if (t > 1.0f)
        t = 1.0f;
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    float alpha = ease;

    // Config
    const float BAR_HEIGHT = 50.0f * scale;
    const float TOP_MARGIN = 32.0f * scale;
    const float SIDE_MARGIN = 32.0f * scale;
    const float ITEM_SPACING = 20.0f * scale;

    ImFont *font = ImGui::GetFont();
    float fontSize = ImGui::GetFontSize();

    // FORCE DARK MODE per user request

    // Time
    std::time_t now = std::time(nullptr);
    std::tm *localTime = std::localtime(&now);
    char timeStr[16];
    char periodStr[16];

    bool is24h = (m_hourFormat == "24h");

    float timeW = 0.0f;
    float periodFontSize = fontSize * 0.55f;
    float periodW = 0.0f;

    if (is24h)
    {
        std::strftime(timeStr, sizeof(timeStr), "%H:%M", localTime);
        timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x;
        periodStr[0] = '\0';
    }
    else
    {
        std::strftime(timeStr, sizeof(timeStr), "%I:%M", localTime);
        std::strftime(periodStr, sizeof(periodStr), "%p", localTime);
        timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x;
        periodW = font->CalcTextSizeA(periodFontSize, FLT_MAX, 0.0f, periodStr).x;
    }

    float totalWidth = timeW;
    if (!is24h)
        totalWidth += 4.0f * scale + periodW;

    // Battery
    bool showBattery = true;
    if (showBattery)
    {
        totalWidth += ITEM_SPACING + 34.0f * scale; // Battery icon width + space
    }

    // Margins inside bar
    float PADDING = 20.0f * scale;
    totalWidth += PADDING * 2;

    // Position
    float offsetY = (1.0f - ease) * -20.0f * scale;
    float barX = displaySize.x - totalWidth - SIDE_MARGIN;
    float barY = TOP_MARGIN + offsetY;

    // Text Color
    ImU32 textColor = IM_COL32(200, 200, 200, (int)(255 * alpha)); // Light Grey for Dark Mode
    ImU32 iconColor = textColor;

    float cursorX = barX + PADDING;
    float centerY = barY + BAR_HEIGHT * 0.5f;

    // Draw Time
    ImVec2 timePos(cursorX, centerY - fontSize * 0.5f);
    dl->AddText(font, fontSize, timePos, textColor, timeStr);
    cursorX += timeW;

    if (!is24h)
    {
        cursorX += 4.0f * scale;
        ImVec2 periodPos(cursorX, centerY - fontSize * 0.5f + (fontSize - periodFontSize) * 0.9f);
        dl->AddText(font, periodFontSize, periodPos, textColor, periodStr);
        cursorX += periodW;
    }

    // Draw Battery
    if (showBattery)
    {
        cursorX += ITEM_SPACING;

        float bodyWidth = 32.0f * scale;
        float bodyHeight = 20.0f * scale;
        float tipWidth = 4.0f * scale;
        float tipHeight = 10.0f * scale;

        ImVec2 batteryPos(cursorX, centerY - bodyHeight * 0.5f);
        ImVec2 bodyMin = batteryPos;
        ImVec2 bodyMax = bodyMin + ImVec2(bodyWidth, bodyHeight);

        // Stroke
        dl->AddRect(bodyMin, bodyMax, iconColor, 3.0f * scale, 0, 2.0f * scale);

        // Tip
        ImVec2 tipMin = ImVec2(bodyMax.x, batteryPos.y + (bodyHeight - tipHeight) * 0.5f);
        ImVec2 tipMax = tipMin + ImVec2(tipWidth, tipHeight);
        dl->AddRectFilled(tipMin, tipMax, iconColor, 2.0f * scale, ImDrawFlags_RoundCornersRight);

        // Fill
        float pct = m_batteryLevel / 100.0f;
        if (pct > 1.0f)
            pct = 1.0f;
        if (pct < 0.0f)
            pct = 0.0f;

        float pad = 4.0f * scale;
        float fillMaxW = bodyWidth - pad * 2;
        float currentFillW = fillMaxW * pct;
        if (currentFillW < 2.0f * scale && pct > 0)
            currentFillW = 2.0f * scale;

        if (currentFillW > 0)
        {
            ImVec2 fillMin = bodyMin + ImVec2(pad, pad);
            ImVec2 fillMax = fillMin + ImVec2(currentFillW, bodyHeight - pad * 2);
            dl->AddRectFilled(fillMin, fillMax, iconColor, 1.0f * scale);
        }

        // Bolt if charging: TEXTURE BASED
        if (m_isCharging)
        {
            // Load texture if needed (Lazy load or init check)
            if (m_boltTexture == 0)
            {
                LoadSVGIcon();
            }

            if (m_boltTexture != 0)
            {
                float iconH = 16.0f * scale;
                float iconW = iconH * ((float)m_boltWidth / (float)m_boltHeight);

                ImVec2 iconPos = ImVec2(tipMax.x + 6.0f * scale, batteryPos.y + (bodyHeight - iconH) * 0.5f);

                // Fade in based on charging progress
                float fadeProgress = (m_chargingStateProgress - 0.5f) * 2.0f;
                if (fadeProgress < 0.0f)
                    fadeProgress = 0.0f;

                int alphaBolt = (int)(255 * fadeProgress * ease);

                if (alphaBolt > 0)
                {
                    ImVec2 p_min = iconPos;
                    ImVec2 p_max = p_min + ImVec2(iconW, iconH);

                    // Tint
                    ImU32 tint = IM_COL32(235, 235, 235, alphaBolt);

                    dl->AddImage(m_boltTexture,
                                 p_min, p_max,
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 tint);
                }
            }
        }
    }
}

//==============================================================================
// RetroAchievements Overlay
//==============================================================================

void TicoOverlay::EnsureRAIconLoaded() {
    // The RA icon/badge textures are owned and uploaded by the host (libretro
    // TicoCore); the overlay just resolves cached ids in ResolveNotificationTextures.
    m_raIconLoadAttempted = true;
}

void TicoOverlay::ResolveNotificationTextures() {
    IOverlayRAHost *ra = m_host ? m_host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    for (auto& n : notifications) {
        if (n.textureId != 0) continue;
        if (n.badge_name.empty()) continue;

        if (n.badge_name == "ra_icon") {
            n.textureId = ra->IconTexture();
        } else {
            // Only check the in-memory texture cache — NO disk I/O, NO stbi_load.
            unsigned int t = ra->BadgeTexture(n.badge_name);
            if (t != 0)
                n.textureId = t;
            // If not cached yet, leave textureId=0; the badge appears once the
            // host uploads it (next frame).
        }
    }
}

void TicoOverlay::RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
    IOverlayRAHost *ra = m_host ? m_host->RA() : nullptr;
    if (!ra) return;
    std::lock_guard<std::mutex> lock(ra->Mutex());
    auto& notifications = ra->Notifications();
    if (notifications.empty()) return;

    float scale = OverlayScale();
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = m_descFont ? m_descFont : font;
    if (!m_descFont && ImGui::GetIO().Fonts->Fonts.Size > 1) {
        descFont = ImGui::GetIO().Fonts->Fonts[1];
    }
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale;
    float alertH = 100.0f * scale;
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale;
    float badgeRadius = 4.0f * scale;
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = ra->AlertPosition();
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = m_isDarkMode
            ? IM_COL32(35, 35, 40, alpha)
            : IM_COL32(245, 248, 252, alpha);
        ImU32 borderColor = m_isDarkMode
            ? IM_COL32(70, 70, 80, (int)(180 * slideProgress))
            : IM_COL32(200, 205, 215, (int)(200 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != 0) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded((ImTextureID)(uintptr_t)n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Text colors
        ImU32 descColor = m_isDarkMode
            ? IM_COL32(185, 185, 195, alpha)
            : IM_COL32(80, 80, 95, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = m_isDarkMode
            ? IM_COL32(255, 255, 255, alpha)
            : IM_COL32(30, 30, 40, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f;
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        // Title shadow + text
        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        // Description
        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}
