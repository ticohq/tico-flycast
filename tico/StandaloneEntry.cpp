/// @file StandaloneEntry.cpp
/// @brief Process entry point for the native tico-flycast standalone NRO.
/// Mirrors PpssppEntry.cpp: open a log sink, then drive StandaloneRuntime
/// through the generic Tico::Main loop.
#ifndef LIBRETRO

#include "StandaloneRuntime.h"
#include "TicoMain.h"

#include <cstdio>
#include <exception>
#include <string>

#ifdef __SWITCH__
#include <switch.h>

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;
}
#endif

namespace
{
FILE *OpenBootLog()
{
    FILE *fp = std::fopen("sdmc:/switch/tico-flycast-standalone.log", "w");
    if (fp == nullptr)
        fp = std::fopen("sdmc:/tico/debug/flycast-standalone.log", "w");
    return fp;
}
}  // namespace

int main(int argc, char *argv[])
{
    FILE *logFile = OpenBootLog();

    Tico::LogCallback log = [logFile](const std::string &line) {
        std::fprintf(stderr, "%s\n", line.c_str());
        if (logFile != nullptr)
        {
            std::fprintf(logFile, "%s\n", line.c_str());
            std::fflush(logFile);
        }
    };

    int rc = 1;
    try
    {
        Tico::StandaloneRuntime runtime(log);
        Tico::Main app(runtime, log);
        rc = app.Run(argc, argv);
    }
    catch (const std::exception &e)
    {
        log(std::string("unhandled std::exception: ") + e.what());
    }
    catch (...)
    {
        log("unhandled unknown exception");
    }

    if (logFile != nullptr)
        std::fclose(logFile);
    return rc;
}

#endif // !LIBRETRO
