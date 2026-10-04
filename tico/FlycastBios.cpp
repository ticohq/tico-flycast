/// @file FlycastBios.cpp
/// @brief Checks the BIOS a game needs, so a missing one gets a message
/// instead of a black screen.

#include "FlycastBios.h"

#include "TicoConfig.h"
#include "TicoLogger.h"

#include "deps/md5/md5.h"

#include <cstdio>
#include <sys/stat.h>
#include <vector>

namespace FlycastBios
{
namespace
{
constexpr long kDcBootSize = 2 * 1024 * 1024;
// dc_boot.bin as the libretro core info lists it
const char *const kKnownDcBoot[] = {"e10c53c2f8b90bab96ead2d368858623"};

long FileSize(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

std::string Md5(const std::string &path)
{
    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp)
        return std::string();
    MD5_CTX ctx;
    MD5_Init(&ctx);
    std::vector<unsigned char> buffer(64 * 1024);
    size_t read;
    while ((read = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0)
        MD5_Update(&ctx, buffer.data(), (unsigned long)read);
    std::fclose(fp);
    unsigned char digest[16];
    MD5_Final(digest, &ctx);
    char hex[33];
    for (int i = 0; i < 16; ++i)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return hex;
}
} // namespace

Status Check(const std::string &slug, bool arcade, bool hleBios)
{
    Status status;
    status.folder = TicoConfig::SystemPath();
    if (arcade)
    {
        status.file = slug == "atomiswave" ? "awbios.zip" : "naomi.zip";
        status.ok = FileSize(status.folder + status.file) > 0;
        return status;
    }
    if (hleBios)
        return status; // Flycast's own BIOS needs no file
    status.file = "dc_boot.bin";
    const std::string path = status.folder + status.file;
    const long size = FileSize(path);
    if (size < 0)
    {
        status.ok = false;
        return status;
    }
    if (size != kDcBootSize)
    {
        status.ok = false;
        status.wrongSize = true;
        status.size = "2 MB";
        return status;
    }
    const std::string md5 = Md5(path);
    status.unknownDump = true;
    for (const char *known : kKnownDcBoot)
        if (md5 == known)
            status.unknownDump = false;
    if (status.unknownDump)
        LOG_WARN("BIOS", "%s has an unknown checksum (%s)", path.c_str(), md5.c_str());
    return status;
}
} // namespace FlycastBios
