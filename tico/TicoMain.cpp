/// @file TicoMain.cpp
/// @brief Emulator-agnostic Tico driver: Switch platform bring-up, the frame
/// loop, and libnx pad → FrameInput polling. Drives a Tico::CoreRuntime and
/// knows nothing about flycast/libretro/SDL. Mirrors tico-ppsspp's TicoMain.cpp.

#include "TicoMain.h"

#include "TicoChainload.h"
#include "TicoConfig.h"

#include <cstdarg>
#include <cstdio>
#include <csignal>
#include <sys/stat.h>
#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace Tico
{

namespace
{

#ifdef __SWITCH__
PadState g_pad;

/// Map libnx HidNpadButton bits to the positional PadButton scheme. Nintendo
/// physical layout (A=east, B=south, X=north, Y=west) is translated to SDL
/// positional (A=south, B=east, X=west, Y=north) so consumers behave the same
/// as on the SDL libretro path.
uint64_t MapButtons(u64 hid)
{
    uint64_t b = 0;
    if (hid & HidNpadButton_B)      b |= Pad_A;   // south
    if (hid & HidNpadButton_A)      b |= Pad_B;   // east
    if (hid & HidNpadButton_Y)      b |= Pad_X;   // west
    if (hid & HidNpadButton_X)      b |= Pad_Y;   // north
    if (hid & HidNpadButton_Up)     b |= Pad_Up;
    if (hid & HidNpadButton_Down)   b |= Pad_Down;
    if (hid & HidNpadButton_Left)   b |= Pad_Left;
    if (hid & HidNpadButton_Right)  b |= Pad_Right;
    if (hid & HidNpadButton_L)      b |= Pad_L;
    if (hid & HidNpadButton_R)      b |= Pad_R;
    if (hid & HidNpadButton_ZL)     b |= Pad_L2;
    if (hid & HidNpadButton_ZR)     b |= Pad_R2;
    if (hid & HidNpadButton_StickL) b |= Pad_L3;
    if (hid & HidNpadButton_StickR) b |= Pad_R3;
    if (hid & HidNpadButton_Plus)   b |= Pad_Start;
    if (hid & HidNpadButton_Minus)  b |= Pad_Select;

    // Synthesize Guide from Plus+Minus so the overlay toggle / exit combo works.
    if ((b & Pad_Start) && (b & Pad_Select))
        b |= Pad_Guide;
    return b;
}
#endif

}  // namespace

float OverlayModeScale()
{
#ifdef __SWITCH__
    return appletGetOperationMode() == AppletOperationMode_Handheld ? 1.5f : 1.0f;
#else
    return 1.0f;
#endif
}

Main::Main(CoreRuntime &runtime, LogCallback log)
    : runtime_(runtime), log_(std::move(log))
{
}

int Main::Run(int argc, char **argv)
{
    LaunchInfo launch{};
    launch.argc = argc;
    launch.argv = argv;
    if (argc > 1 && argv[1])
        launch.contentPath = argv[1];

    if (!InitPlatform())
        return 1;

    Log("tico main start core=%s argc=%d content=%s", runtime_.Name(), argc,
        launch.contentPath.empty() ? "(default)" : launch.contentPath.c_str());

    bool started = runtime_.Configure(launch) && runtime_.Initialize(launch) &&
                   runtime_.LoadContent(launch.contentPath);
    if (!started)
    {
        Log("tico main init failed core=%s", runtime_.Name());
        runtime_.Shutdown();
        ShutdownPlatform();
        return 1;
    }

    Log("tico main loop enter core=%s", runtime_.Name());
    while (!runtime_.ShouldExit())
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
            break;
#endif
        const FrameInput input = PollInput();
        runtime_.HandleInput(input);
        runtime_.RunFrame();
        runtime_.RenderFrame();
    }
    Log("tico main loop exit core=%s", runtime_.Name());

    const bool chainload = runtime_.ShouldChainloadLauncher();
    runtime_.Shutdown();
    ShutdownPlatform();
    if (chainload)
        ChainloadLauncher(log_);
    return 0;
}

bool Main::InitPlatform()
{
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        Log("Unable to ignore SIGPIPE");

#ifdef __SWITCH__
    mkdir(Paths::Root, 0777);
    mkdir(Paths::Debug, 0777);
    mkdir(Paths::Assets, 0777);
    mkdir(Paths::Lang, 0777);
    mkdir(Paths::System, 0777);
    mkdir(Paths::SavesRoot, 0777);
    mkdir(Paths::StatesRoot, 0777);
    mkdir("sdmc:/tico/config", 0777);
    mkdir(Paths::CoreConfigDir, 0777);

    appletLockExit();
    exitLocked_ = true;

    if (R_SUCCEEDED(socketInitializeDefault()))
        socketReady_ = true;
    else
        Log("socketInitializeDefault failed");

    if (R_FAILED(romfsInit()))
    {
        Log("romfsInit failed");
        ShutdownPlatform();
        return false;
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
#endif

    platformReady_ = true;
    return true;
}

void Main::ShutdownPlatform()
{
    platformReady_ = false;
#ifdef __SWITCH__
    romfsExit();
    if (socketReady_)
    {
        socketExit();
        socketReady_ = false;
    }
    if (exitLocked_)
    {
        appletUnlockExit();
        exitLocked_ = false;
    }
#endif
}

FrameInput Main::PollInput()
{
    FrameInput in{};
#ifdef __SWITCH__
    padUpdate(&g_pad);
    in.buttons = MapButtons(padGetButtons(&g_pad));
    const HidAnalogStickState left = padGetStickPos(&g_pad, 0);
    const HidAnalogStickState right = padGetStickPos(&g_pad, 1);
    // libnx reports +Y up; convert to the SDL convention (+Y down) consumers use.
    in.leftStickX = left.x;
    in.leftStickY = -left.y;
    in.rightStickX = right.x;
    in.rightStickY = -right.y;
#endif
    in.pressed = in.buttons & ~prevButtons_;
    in.released = prevButtons_ & ~in.buttons;
    prevButtons_ = in.buttons;
    return in;
}

void Main::Log(const char *fmt, ...) const
{
    if (!log_ || !fmt)
        return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    log_(buffer);
}

}  // namespace Tico
