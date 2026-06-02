/// @file TicoChainload.h
/// @brief Chainload back to the tico launcher NRO. Emulator-agnostic.
#pragma once

#include "TicoLogger.h"

namespace Tico
{

/// Queue the tico launcher (sdmc:/switch/tico.nro, with fallback) as the next
/// homebrew to load via envSetNextLoad. Call after the runtime has shut down.
/// No-op off Switch.
void ChainloadLauncher(const LogCallback &log = {});

}  // namespace Tico
