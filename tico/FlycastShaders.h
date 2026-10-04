/// @file FlycastShaders.h
/// @brief RetroArch slang presets over Flycast's GPU frames.
#pragma once

class TicoCore;

namespace FlycastShaders
{
/// Creates the shader chain, adds the Shaders category to Settings and puts
/// the chain between the core's image and the screen. Call once the Vulkan
/// device is up and the overlay is initialized.
void Init(TicoCore *core);
/// Loads the preset the settings name when it changes (outside a frame).
void Update();
void Shutdown();
} // namespace FlycastShaders
