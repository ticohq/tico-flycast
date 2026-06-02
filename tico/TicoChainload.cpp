/// @file TicoChainload.cpp
#include "TicoChainload.h"

#include "TicoConfig.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#include <switch/runtime/env.h>
#endif

namespace Tico
{

void ChainloadLauncher(const LogCallback &log)
{
#ifdef __SWITCH__
    const char *primary = Paths::LauncherNro;
    const char *fallback = Paths::LauncherNroFallback;
    const char *target = nullptr;

    struct stat st;
    if (stat(primary, &st) == 0)
        target = primary;
    else if (stat(fallback, &st) == 0)
        target = fallback;

    if (target)
    {
        char args[512];
        std::snprintf(args, sizeof(args), "%s --resume", target);
        envSetNextLoad(target, args);
        if (log)
            log(std::string("Chainloading back to ") + target);
    }
    else if (log)
    {
        log(std::string("No tico.nro found at ") + primary + " or " + fallback);
    }
#else
    (void)log;
#endif
}

}  // namespace Tico
