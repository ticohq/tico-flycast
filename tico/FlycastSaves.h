/// @file FlycastSaves.h
/// @brief Backups of the saves Flycast writes in place.
#pragma once

namespace FlycastSaves
{
/// Copies the game's VMUs and arcade saves (NVRAM, EEPROM, cards) into
/// backups/ beside them, keeping the last few sessions. Flycast writes these
/// files in place while the game runs, so the copy is taken as it starts.
void BackupForSession();
} // namespace FlycastSaves
