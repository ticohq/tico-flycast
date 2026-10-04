/// @file FlycastCheats.cpp
/// @brief Hands the game's cheat files to Flycast's CheatManager. The libretro
/// build leaves its own .cht loader out, but GameShark codes still go in
/// through addGameSharkCheat; each of our cheats becomes a run of the
/// manager's entries, toggled together.

#include "FlycastCheats.h"

#include "TicoConfig.h"
#include "TicoLogger.h"
#include "TicoUtils.h"

#include "cheats.h"

#include <cctype>
#include <cstdio>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <map>

namespace FlycastCheats
{
namespace
{
struct Loaded
{
    Entry entry;
    std::string codes;
    // the manager's entries [first, last) this cheat became
    size_t first = 0;
    size_t last = 0;
};

std::vector<Loaded> s_cheats;
std::vector<Entry> s_entries;
std::string s_loadedPath;

// The name without its disc tag: "Game (USA) (Disc 2)" -> "Game (USA)".
std::string WithoutDiscTag(const std::string &name)
{
    std::string lower = name;
    for (char &c : lower)
        c = (char)std::tolower((unsigned char)c);
    for (const char *tag : {"(disc", "(disk", "(cd"})
    {
        const size_t open = lower.find(tag);
        if (open == std::string::npos)
            continue;
        const size_t close = lower.find(')', open);
        size_t start = open;
        while (start > 0 && name[start - 1] == ' ')
            start--;
        std::string out = name;
        out.erase(start, close == std::string::npos ? std::string::npos : close + 1 - start);
        return out;
    }
    return name;
}

bool Exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// The disc's own cheat files, else the game's (shared by its discs).
std::string BasePath(const std::string &gamePath, const std::string &slug)
{
    std::string name = gamePath;
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    const std::string dir = "sdmc:/tico/cheats/" + slug + "/";
    TicoConfig::MakeDirs(dir);
    const std::string own = dir + name;
    if (Exists(own + ".cht") || Exists(own + ".cheats"))
        return own;
    return dir + WithoutDiscTag(name);
}

// Codes joined with + or ; become one space-separated line for the manager.
std::string JoinCodes(const std::string &text)
{
    std::string out;
    for (const char c : text)
        out += (c == '+' || c == ';' || c == ',') ? ' ' : c;
    return TicoUtils::Trim(out);
}

void ReadCht(const std::string &path, std::vector<Loaded> &out)
{
    std::ifstream file(path);
    if (!file.is_open())
        return;
    std::map<int, Loaded> byIndex;
    std::string line;
    while (std::getline(file, line))
    {
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = TicoUtils::Trim(line.substr(0, eq));
        std::string value = TicoUtils::Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        int index = -1;
        char field[16] = {0};
        if (sscanf(key.c_str(), "cheat%d_%15s", &index, field) != 2 || index < 0)
            continue;
        if (!strcmp(field, "desc"))
            byIndex[index].entry.name = value;
        else if (!strcmp(field, "code"))
            byIndex[index].codes = JoinCodes(value);
    }
    for (auto &item : byIndex)
    {
        if (item.second.codes.empty())
            continue;
        if (item.second.entry.name.empty())
            item.second.entry.name = "Cheat " + std::to_string(item.first + 1);
        out.push_back(item.second);
    }
}

void ReadCheats(const std::string &path, std::vector<Loaded> &out)
{
    std::ifstream file(path);
    if (!file.is_open())
        return;
    std::string line;
    while (std::getline(file, line))
    {
        const std::string text = TicoUtils::Trim(line);
        if (text.empty() || text[0] == '!')
            continue;
        if (text[0] == '#')
        {
            out.emplace_back();
            out.back().entry.name = TicoUtils::Trim(text.substr(1));
            continue;
        }
        if (out.empty())
        {
            out.emplace_back();
            out.back().entry.name = "Cheat";
        }
        Loaded &cheat = out.back();
        cheat.codes += (cheat.codes.empty() ? "" : " ") + JoinCodes(text);
    }
}

void SetRange(const Loaded &cheat, bool enabled)
{
    for (size_t i = cheat.first; i < cheat.last && i < cheatManager.cheatCount(); ++i)
        cheatManager.enableCheat(i, enabled);
}

// Whether the manager still holds this cheat's entries: a disc of another
// game resets it, taking them with it.
bool StillLoaded(const Loaded &cheat)
{
    return cheat.last <= cheatManager.cheatCount() &&
           cheatManager.cheatDescription(cheat.first) == cheat.entry.name;
}

void RebuildEntries()
{
    s_entries.clear();
    for (const Loaded &cheat : s_cheats)
        s_entries.push_back(cheat.entry);
}
} // namespace

void Load(const std::string &gamePath, const std::string &slug)
{
    Clear();
    s_loadedPath = gamePath;
    const std::string base = BasePath(gamePath, slug);
    std::vector<Loaded> found;
    ReadCht(base + ".cht", found);
    ReadCheats(base + ".cheats", found);
    for (Loaded &cheat : found)
    {
        cheat.first = cheatManager.cheatCount();
        try
        {
            cheatManager.addGameSharkCheat(cheat.entry.name, cheat.codes);
        }
        catch (const std::exception &e)
        {
            LOG_WARN("CHEATS", "skipping \"%s\": %s", cheat.entry.name.c_str(), e.what());
            continue;
        }
        cheat.last = cheatManager.cheatCount();
        if (cheat.last == cheat.first)
            continue;
        cheat.entry.enabled = false;
        SetRange(cheat, false);
        s_cheats.push_back(cheat);
    }
    RebuildEntries();
    LOG_INFO("CHEATS", "%zu for %s", s_cheats.size(), base.c_str());
}

const std::vector<Entry> &List()
{
    return s_entries;
}

bool NeedsReload(const std::string &gamePath)
{
    if (gamePath != s_loadedPath)
        return true;
    for (const Loaded &cheat : s_cheats)
        if (!StillLoaded(cheat))
            return true;
    return false;
}

void Toggle(size_t index)
{
    if (index >= s_cheats.size())
        return;
    Loaded &cheat = s_cheats[index];
    if (!StillLoaded(cheat))
    {
        LOG_WARN("CHEATS", "cheat list was reset by the core");
        Clear();
        return;
    }
    cheat.entry.enabled = !cheat.entry.enabled;
    SetRange(cheat, cheat.entry.enabled);
    RebuildEntries();
}

void Clear()
{
    for (const Loaded &cheat : s_cheats)
        if (StillLoaded(cheat))
            SetRange(cheat, false);
    s_cheats.clear();
    s_entries.clear();
}
} // namespace FlycastCheats
