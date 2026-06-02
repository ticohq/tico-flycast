/// @file TicoMain.h
/// @brief Emulator-agnostic Tico driver and core-runtime interface.
///
/// Mirrors tico-ppsspp/tico/TicoMain.h: `Tico::Main` owns the Switch platform
/// bring-up, the frame loop, and libnx pad polling, and drives an opaque
/// `Tico::CoreRuntime`. The only emulator-specific code lives behind that
/// interface (FlycastRuntime for the libretro path, StandaloneRuntime for the
/// native path). This header pulls in no flycast/libretro/SDL types so it can
/// back any core, and crucially no SDL — the standalone target does not link
/// SDL, so input here is libnx pad only.
#pragma once

#include "TicoLogger.h"

#include <cstdint>
#include <string>

namespace Tico
{

/// What to launch — argv plus the resolved content (ROM) path.
struct LaunchInfo
{
    int argc = 0;
    char **argv = nullptr;
    std::string contentPath;
};

/// Core-agnostic controller buttons, positional like SDL (A=south, B=east,
/// X=west, Y=north) so consumers written against the SDL libretro path keep
/// working. Tico::Main maps libnx HidNpadButton into these.
enum PadButton : uint64_t
{
    Pad_A      = 1ull << 0,  // south
    Pad_B      = 1ull << 1,  // east
    Pad_X      = 1ull << 2,  // west
    Pad_Y      = 1ull << 3,  // north
    Pad_Up     = 1ull << 4,
    Pad_Down   = 1ull << 5,
    Pad_Left   = 1ull << 6,
    Pad_Right  = 1ull << 7,
    Pad_L      = 1ull << 8,
    Pad_R      = 1ull << 9,
    Pad_L2     = 1ull << 10, // ZL
    Pad_R2     = 1ull << 11, // ZR
    Pad_L3     = 1ull << 12,
    Pad_R3     = 1ull << 13,
    Pad_Start  = 1ull << 14, // Plus
    Pad_Select = 1ull << 15, // Minus
    Pad_Guide  = 1ull << 16, // synthesized from Start+Select
};

/// One frame's worth of neutralized input. `pressed`/`released` are the edges
/// computed by Main this frame (replaces per-consumer debounce). Stick values
/// use the SDL convention: range -32768..32767, +Y is down.
struct FrameInput
{
    uint64_t buttons = 0;
    uint64_t pressed = 0;
    uint64_t released = 0;
    int leftStickX = 0;
    int leftStickY = 0;
    int rightStickX = 0;
    int rightStickY = 0;
};

/// Implemented by each emulator. Lifecycle mirrors PPSSPP's CoreRuntime.
class CoreRuntime
{
public:
    virtual ~CoreRuntime() = default;

    virtual const char *Name() const = 0;
    virtual bool Configure(const LaunchInfo &) { return true; }
    virtual bool Initialize(const LaunchInfo &) = 0;
    virtual bool LoadContent(const std::string &path) = 0;
    virtual void HandleInput(const FrameInput &) {}
    virtual void RunFrame() = 0;
    virtual void RenderFrame() = 0;
    virtual bool ShouldExit() const = 0;
    virtual bool ShouldChainloadLauncher() const { return false; }
    virtual void RequestExit() = 0;
    virtual void Shutdown() = 0;
};

/// Generic platform/loop driver. Construct with a runtime, call Run().
class Main
{
public:
    explicit Main(CoreRuntime &runtime, LogCallback log = {});

    int Run(int argc, char **argv);

private:
    bool InitPlatform();
    void ShutdownPlatform();
    FrameInput PollInput();
    void Log(const char *fmt, ...) const;

    CoreRuntime &runtime_;
    LogCallback log_;
    bool platformReady_ = false;
    bool exitLocked_ = false;
    bool socketReady_ = false;
    uint64_t prevButtons_ = 0;
};

/// Overlay font/scale factor for the current Switch operation mode (handheld
/// vs docked). A platform-display helper the runtimes call when sizing the
/// overlay.
float OverlayModeScale();

}  // namespace Tico
