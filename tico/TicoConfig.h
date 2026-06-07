/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay
#pragma once

#include <string>

namespace TicoConfig {
    // Hardcoded test ROM for easy testing
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/dc/Sonic Adventure (USA).cue";
    
    // Asset paths
    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* IMAGES_PATH = "romfs:/images/";
    constexpr const char* SYSTEM_PATH = "sdmc:/tico/system/dc/";
    constexpr const char* SAVES_PATH = "sdmc:/tico/saves/dc/";
    constexpr const char* STATES_PATH = "sdmc:/tico/states/dc/";
    
    // Window settings
    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    // Audio backend configuration
    // true  = SDL_QueueAudio (Push model). Used on Flycast: the DC+Vulkan load
    //         starves the SDL_mixer pull-callback in Callback mode, causing
    //         choppy game audio. The RA trophy chime is mixed into this queue
    //         manually by TicoAudio (no second device, which the Switch can't do).
    // false = Mix_HookMusic + RingBuffer (Callback model, used by lighter cores).
    constexpr bool USE_SDLQUEUEAUDIO = true;
}

// Emulator-agnostic paths consumed by the generic Tico layer (Tico::Main,
// Tico::ChainloadLauncher). Core-specific subpaths (dc states/saves/system)
// stay in TicoConfig above and remain a flycast concern.
namespace Tico { namespace Paths {
    constexpr const char* Root              = "sdmc:/tico";
    constexpr const char* Debug             = "sdmc:/tico/debug";
    constexpr const char* Assets            = "sdmc:/tico/assets";
    constexpr const char* Lang              = "sdmc:/tico/lang";
    constexpr const char* System            = "sdmc:/tico/system";
    constexpr const char* SavesRoot         = "sdmc:/tico/saves";
    constexpr const char* StatesRoot        = "sdmc:/tico/states";
    constexpr const char* CoreConfigDir     = "sdmc:/tico/config/cores";
    constexpr const char* LauncherNro       = "sdmc:/switch/tico.nro";
    constexpr const char* LauncherNroFallback = "sdmc:/switch/tico/tico.nro";
    constexpr const char* DefaultTitleFont       = "romfs:/fonts/font.ttf";
    constexpr const char* DefaultDescriptionFont = "romfs:/fonts/description.ttf";
}}  // namespace Tico::Paths

// UI Actions for HelpersBar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
