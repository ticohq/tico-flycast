/// @file FlycastCheats.h
/// @brief The quick menu's cheats, run by Flycast's own cheat engine.
#pragma once

#include <string>
#include <vector>

namespace FlycastCheats
{
struct Entry
{
    std::string name;
    bool enabled = false;
};

/// Reads sdmc:/tico/cheats/<slug>/<game>.cht (RetroArch's format, cheatN_desc
/// and cheatN_code) and <game>.cheats ("# Name" then its codes): Dreamcast
/// GameShark / CodeBreaker codes, 8 hex digits each. Call after the game has
/// loaded; every cheat starts off.
void Load(const std::string &gamePath, const std::string &slug);
const std::vector<Entry> &List();
/// Whether the cheats loaded are not those of the disc at @p gamePath, or the
/// core reset them (a disc swap does).
bool NeedsReload(const std::string &gamePath);
void Toggle(size_t index);
void Clear();
} // namespace FlycastCheats
