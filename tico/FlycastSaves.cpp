/// @file FlycastSaves.cpp
/// @brief Backs up the VMU and arcade save files of the game being started.

#include "FlycastSaves.h"

#include "TicoLogger.h"
#include "TicoSafeFile.h"

#include "emulator.h"
#include "oslib/oslib.h"

#include <dirent.h>
#include <string>

namespace FlycastSaves
{
namespace
{
constexpr int kKeep = 3;

void Backup(const std::string &path)
{
    if (path.empty() || !TicoSafeFile::Exists(path))
        return;
    TicoSafeFile::BackupCopy(path, kKeep);
    LOG_INFO("SAVES", "backed up %s", path.c_str());
}
} // namespace

void BackupForSession()
{
    if (settings.platform.isConsole())
    {
        for (const char *port : {"A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2"})
            Backup(hostfs::getVmuPath(port, false));
        return;
    }
    // arcade saves are <flash base> plus a suffix (.nvmem, .eeprom, .card...)
    const std::string base = hostfs::getArcadeFlashPath();
    const size_t slash = base.find_last_of('/');
    if (base.empty() || slash == std::string::npos)
        return;
    const std::string dir = base.substr(0, slash);
    const std::string prefix = base.substr(slash + 1);
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (dirent *entry = readdir(d))
    {
        const std::string name = entry->d_name;
        if (entry->d_type == DT_REG && name.compare(0, prefix.size(), prefix) == 0 &&
            name.size() > prefix.size() && name.find(".tmp") == std::string::npos)
            Backup(dir + "/" + name);
    }
    closedir(d);
}
} // namespace FlycastSaves
