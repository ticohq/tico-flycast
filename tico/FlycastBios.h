/// @file FlycastBios.h
/// @brief Whether the BIOS a game needs is there, before Flycast boots it.
#pragma once

#include <string>

namespace FlycastBios
{
struct Status
{
    bool ok = true;
    std::string file;      // the BIOS file in question
    std::string folder;    // where it has to be
    bool wrongSize = false;
    std::string size;      // the size it should be, when wrongSize
    bool unknownDump = false; // dc_boot.bin of the right size but not a known dump
};

/// The console's BIOS in TicoConfig::SystemPath(): dc_boot.bin (2 MB) for a
/// Dreamcast game unless HLE BIOS is on, naomi.zip or awbios.zip for the
/// arcade boards. Without it Flycast only shows a black screen.
Status Check(const std::string &slug, bool arcade, bool hleBios);
} // namespace FlycastBios
