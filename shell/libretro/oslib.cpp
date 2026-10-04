/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "oslib/oslib.h"
#include "stdclass.h"
#include "file/file_path.h"
#include "oslib/i18n.h"
#include <cctype>
#include <sys/stat.h>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

const char *retro_get_system_directory();

extern char game_dir_no_slash[1024];
extern char vmu_dir_no_slash[PATH_MAX];
extern char content_name[PATH_MAX];
extern char g_roms_dir[PATH_MAX];
extern unsigned per_content_vmus;
extern std::string arcadeFlashPath;
extern retro_environment_t environ_cb;

unsigned retro_disc_count();

namespace hostfs
{

// A game on several discs shares one VMU, named after the game without its
// disc tag ("Grandia II (USA) (Disc 2)" -> "Grandia II (USA)"; an .m3u's own
// name), so a save made on disc 1 is there on disc 2. Empty for a single disc.
static std::string sharedDiscName()
{
	std::string name = content_name;
	std::string lower = name;
	for (char &c : lower)
		c = (char)std::tolower((unsigned char)c);
	for (const char *tag : { "(disc", "(disk", "(cd" })
	{
		const size_t open = lower.find(tag);
		if (open == std::string::npos)
			continue;
		const size_t close = lower.find(')', open);
		size_t start = open;
		while (start > 0 && name[start - 1] == ' ')
			start--;
		name.erase(start, close == std::string::npos ? std::string::npos : close + 1 - start);
		return name;
	}
	return retro_disc_count() > 1 ? name : std::string();
}

// The newest of the VMUs a disc of this game used on its own before they were
// shared: the disc's game ID, its file name, or a "(Disc N)" sibling's name.
static std::string newestDiscVmu(const std::string& vmuDir, const std::string& shared,
		const std::string& port, const std::string& gameIdPath)
{
	std::vector<std::string> candidates { gameIdPath,
		vmuDir + std::string(content_name) + "." + port + ".bin" };
	for (int disc = 1; disc <= 4; disc++)
		candidates.push_back(vmuDir + shared + " (Disc " + std::to_string(disc) + ")." + port + ".bin");
	std::string newest;
	time_t newestTime = 0;
	for (const std::string& path : candidates)
	{
		struct stat st;
		if (!path.empty() && stat(path.c_str(), &st) == 0 && (newest.empty() || st.st_mtime > newestTime))
		{
			newest = path;
			newestTime = st.st_mtime;
		}
	}
	return newest;
}

std::string getVmuPath(const std::string& port, bool save)
{
	if ((per_content_vmus == 1 && port == "A1")
			|| per_content_vmus == 2)
	{
		std::string vmuDir = vmu_dir_no_slash + std::string(PATH_DEFAULT_SLASH());
		const std::string shared = settings.platform.isConsole() ? sharedDiscName() : std::string();
		if (!shared.empty())
		{
			const std::string wpath = vmuDir + shared + "." + port + ".bin";
			if (save || file_exists(wpath.c_str()))
				return wpath;
			// the first time: read the newest per-disc VMU; the VMU then
			// saves it under the shared name (see maple_sega_vmu::OnSetup)
			std::string gameIdPath;
			if (!settings.content.gameId.empty())
			{
				constexpr std::string_view INVALID_CHARS { " /\\:*?|<>" };
				gameIdPath = settings.content.gameId;
				for (char &c: gameIdPath)
					if (INVALID_CHARS.find(c) != INVALID_CHARS.npos)
						c = '_';
				gameIdPath = vmuDir + gameIdPath + "." + port + ".bin";
			}
			const std::string old = newestDiscVmu(vmuDir, shared, port, gameIdPath);
			return old.empty() ? wpath : old;
		}
		if (settings.platform.isConsole() && !settings.content.gameId.empty())
		{
			constexpr std::string_view INVALID_CHARS { " /\\:*?|<>" };
			std::string vmuName = settings.content.gameId;
			for (char &c: vmuName)
				if (INVALID_CHARS.find(c) != INVALID_CHARS.npos)
					c = '_';
			vmuName += "." + port + ".bin";
			std::string wpath = vmuDir + vmuName;
			if (save || file_exists(wpath.c_str()))
				return wpath;
			// Legacy path with rom name
			std::string rpath = vmuDir + std::string(content_name) + "." + port + ".bin";
			if (file_exists(rpath.c_str()))
				return rpath;
			else
				return wpath;
		}
		return vmuDir + std::string(content_name) + "." + port + ".bin";
	}
	else {
		return std::string(game_dir_no_slash) + std::string(PATH_DEFAULT_SLASH()) + "vmu_save_" + port + ".bin";
	}
}

std::string getArcadeFlashPath()
{
	return arcadeFlashPath;
}

std::string findFlash(const std::string& prefix, const std::string& names_ro)
{
   std::string root(game_dir_no_slash);
   root += "/";

	char base[512];
	char temp[512];
	char names[512];
	strcpy(names,names_ro.c_str());
	sprintf(base,"%s",root.c_str());

	char* curr=names;
	char* next;
	do
	{
		next=strstr(curr,";");
		if(next) *next=0;
		if (curr[0]=='%')
		{
			sprintf(temp,"%s%s%s",base,prefix.c_str(),curr+1);
		}
		else
		{
			sprintf(temp,"%s%s",base,curr);
		}

		curr=next+1;

		if (path_is_valid(temp))
			return temp;
	} while(next);

	return "";
}

std::string getFlashSavePath(const std::string& prefix, const std::string& name)
{
   std::string root(game_dir_no_slash);

	return root + PATH_DEFAULT_SLASH() + prefix + name;
}

std::string findNaomiBios(const std::string& name)
{
	std::string basepath(game_dir_no_slash);
	basepath += PATH_DEFAULT_SLASH() + name;
	if (!file_exists(basepath))
	{
		// File not found in system dir, try game dir instead
		basepath = g_roms_dir + name;
		if (!file_exists(basepath))
			return "";
	}
	return basepath;
}

std::string getSavestatePath(int index, bool writable)
{
	// Not used
	return "";
}

std::string getShaderCachePath(const std::string& filename)
{
	return std::string(game_dir_no_slash) + std::string(PATH_DEFAULT_SLASH()) + filename;
}

std::string getTextureLoadPath(const std::string& gameId)
{
#ifdef USE_TICO
	return std::string(game_dir_no_slash) + "/textures/" + gameId + PATH_DEFAULT_SLASH();
#else
	return std::string(retro_get_system_directory()) + "/dc/textures/"
						+ gameId + PATH_DEFAULT_SLASH();
#endif
}

std::string getTextureDumpPath()
{
	return std::string(game_dir_no_slash) + std::string(PATH_DEFAULT_SLASH())
			+ "texdump" + std::string(PATH_DEFAULT_SLASH());
}

std::string getScreenshotsPath()
{
	// Unfortunately retroarch doesn't expose its "screenshots" path
#ifdef USE_TICO
	return std::string(game_dir_no_slash);
#else
	return std::string(retro_get_system_directory()) + "/dc";
#endif
}

void saveScreenshot(const std::string& name, const std::vector<u8>& data)
{
	std::string path = getScreenshotsPath();
	path += "/" + name;
	FILE *f = nowide::fopen(path.c_str(), "wb");
	if (f == nullptr)
		throw FlycastException(path);
	if (std::fwrite(&data[0], data.size(), 1, f) != 1) {
		std::fclose(f);
		nowide::remove(path.c_str());
		throw FlycastException(path);
	}
	std::fclose(f);
}

}

#if defined(_WIN32) || defined(__APPLE__)
void os_SetThreadName(const char *name) {
}
#endif

namespace i18n
{

std::string getCurrentLocale()
{
	unsigned language;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language))
		return "en";
	switch (language)
	{
	case RETRO_LANGUAGE_JAPANESE: return "ja";
	case RETRO_LANGUAGE_FRENCH: return "fr";
	case RETRO_LANGUAGE_SPANISH: return "es";
	case RETRO_LANGUAGE_GERMAN: return "de";
	case RETRO_LANGUAGE_ITALIAN: return "it";
	case RETRO_LANGUAGE_DUTCH: return "nl";
	case RETRO_LANGUAGE_PORTUGUESE_BRAZIL: return "pt_BR";
	case RETRO_LANGUAGE_PORTUGUESE_PORTUGAL: return "pt_PT";
	case RETRO_LANGUAGE_RUSSIAN: return "uk";
	case RETRO_LANGUAGE_KOREAN: return "ko";
	case RETRO_LANGUAGE_CHINESE_TRADITIONAL: return "zh_TW";
	case RETRO_LANGUAGE_CHINESE_SIMPLIFIED: return "zh_CN";
	case RETRO_LANGUAGE_ESPERANTO: return "eo";
	case RETRO_LANGUAGE_POLISH: return "pl";
	case RETRO_LANGUAGE_VIETNAMESE: return "vi";
	case RETRO_LANGUAGE_ARABIC: return "ar";
	case RETRO_LANGUAGE_GREEK: return "el";
	case RETRO_LANGUAGE_TURKISH: return "tr";
	case RETRO_LANGUAGE_SLOVAK: return "sk";
	case RETRO_LANGUAGE_PERSIAN: return "fa";
	case RETRO_LANGUAGE_HEBREW: return "he";
	case RETRO_LANGUAGE_FINNISH: return "fi";
	case RETRO_LANGUAGE_INDONESIAN: return "id";
	case RETRO_LANGUAGE_SWEDISH: return "sv";
	case RETRO_LANGUAGE_UKRAINIAN: return "uk";
	case RETRO_LANGUAGE_CZECH: return "cs";
	case RETRO_LANGUAGE_CATALAN_VALENCIA: return "ca";
	case RETRO_LANGUAGE_CATALAN: return "ca";
	case RETRO_LANGUAGE_BRITISH_ENGLISH: return "en_GB";
	case RETRO_LANGUAGE_HUNGARIAN: return "hu";
	default: return "en";
	}
}

}
