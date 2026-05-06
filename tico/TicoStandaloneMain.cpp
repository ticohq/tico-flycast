/*
    Minimal native Flycast entry point for Tico on Nintendo Switch.

    This intentionally bypasses Flycast's standalone UI loop. The Tico
    launcher supplies a content path, then this file initializes Flycast,
    loads that content directly, renders frames, and exits back to Tico.
 */
#ifndef LIBRETRO

#include "nswitch.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/LogManager.h"
#include "oslib/directory.h"
#include "oslib/oslib.h"
#include "reios/reios.h"
#include "stdclass.h"
#include "ui/gui.h"
#include "ui/imgui_driver.h"
#include "ui/mainui.h"

#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <strings.h>
#include <vector>
#include <switch/runtime/env.h>

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
size_t __nx_heap_size = 0;
}

namespace {

PadState g_pad;
bool g_running = true;
bool g_chainloadToTico = false;
bool g_exitLocked = false;
bool g_socketReady = false;
bool g_romfsReady = false;
u8 g_lastOperationMode = 255;
FILE *g_logFile = nullptr;
std::string g_launchTitle;
int g_ticoSh4Clock = 0;

void Log(const char *fmt, ...);

void SetCurrentThreadAffinity(const char *name, s32 preferredCore)
{
	u64 processMask = 0;
	Result rc = svcGetInfo(&processMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
	if (R_FAILED(rc))
	{
		Log("%s affinity: svcGetInfo(CoreMask) failed rc=0x%x", name, (unsigned)rc);
		return;
	}

	if (preferredCore < 0 || preferredCore >= 4 || (processMask & (UINT64_C(1) << preferredCore)) == 0)
	{
		Log("%s affinity: core %d unavailable in process mask 0x%llx",
				name, (int)preferredCore, (unsigned long long)processMask);
		return;
	}

	s32 previousPreferred = -1;
	u64 previousMask = 0;
	rc = svcGetThreadCoreMask(&previousPreferred, &previousMask, CUR_THREAD_HANDLE);
	if (R_FAILED(rc))
		Log("%s affinity: svcGetThreadCoreMask failed rc=0x%x", name, (unsigned)rc);

	const u32 affinityMask = 1u << preferredCore;
	rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, preferredCore, affinityMask);
	if (R_FAILED(rc))
	{
		Log("%s affinity: svcSetThreadCoreMask core=%d mask=0x%x failed rc=0x%x",
				name, (int)preferredCore, affinityMask, (unsigned)rc);
		return;
	}

	s32 currentPreferred = -1;
	u64 currentMask = 0;
	rc = svcGetThreadCoreMask(&currentPreferred, &currentMask, CUR_THREAD_HANDLE);
	if (R_FAILED(rc))
	{
		Log("%s affinity: pinned to core %d mask=0x%x; readback failed rc=0x%x",
				name, (int)preferredCore, affinityMask, (unsigned)rc);
		return;
	}

	Log("%s affinity: process=0x%llx previous=%d/0x%llx current=%d/0x%llx running=%u",
			name,
			(unsigned long long)processMask,
			(int)previousPreferred,
			(unsigned long long)previousMask,
			(int)currentPreferred,
			(unsigned long long)currentMask,
			(unsigned)svcGetCurrentProcessorNumber());
}

void Log(const char *fmt, ...)
{
	if (fmt == nullptr)
		return;

	va_list args;
	va_start(args, fmt);
	std::vfprintf(stderr, fmt, args);
	std::fputc('\n', stderr);
	va_end(args);

	if (g_logFile == nullptr)
		return;

	va_start(args, fmt);
	std::vfprintf(g_logFile, fmt, args);
	std::fputc('\n', g_logFile);
	std::fflush(g_logFile);
	va_end(args);
}

void SetTicoSh4Clock(int clock, const char *source)
{
	if (clock < 100 || clock > 300)
	{
		Log("ignoring %s SH4 clock override: %d MHz", source, clock);
		return;
	}
	g_ticoSh4Clock = clock;
	Log("using %s SH4 clock override: %d MHz", source, g_ticoSh4Clock);
}

void OpenBootLog()
{
	g_logFile = std::fopen("sdmc:/switch/tico-flycast-standalone.log", "w");
	if (g_logFile == nullptr)
		g_logFile = std::fopen("sdmc:/tico/debug/flycast-standalone.log", "w");
}

void CloseBootLog()
{
	if (g_logFile != nullptr)
	{
		std::fclose(g_logFile);
		g_logFile = nullptr;
	}
}

bool IsSyntheticLaunchArg(const char *arg)
{
	if (arg == nullptr || arg[0] == '\0')
		return true;
	return std::strcmp(arg, "--resume") == 0
		|| std::strcmp(arg, "-resume") == 0
		|| std::strncmp(arg, "disk$", 5) == 0;
}

bool HasContentExtension(const char *arg)
{
	const char *extension = std::strrchr(arg, '.');
	if (extension == nullptr)
		return false;

	return strcasecmp(extension, ".cdi") == 0
		|| strcasecmp(extension, ".chd") == 0
		|| strcasecmp(extension, ".gdi") == 0
		|| strcasecmp(extension, ".cue") == 0
		|| strcasecmp(extension, ".elf") == 0
		|| strcasecmp(extension, ".zip") == 0
		|| strcasecmp(extension, ".7z") == 0
		|| strcasecmp(extension, ".bin") == 0
		|| strcasecmp(extension, ".dat") == 0
		|| strcasecmp(extension, ".lst") == 0;
}

bool LooksLikeContentPath(const char *arg)
{
	if (arg == nullptr || arg[0] == '\0' || arg[0] == '-')
		return false;
	return std::strncmp(arg, "sdmc:/", 6) == 0
		|| std::strncmp(arg, "romfs:/", 7) == 0
		|| std::strchr(arg, '/') != nullptr
		|| std::strchr(arg, '\\') != nullptr
		|| HasContentExtension(arg);
}

std::vector<char *> BuildFlycastArgv(int argc, char *argv[])
{
	std::vector<char *> filtered;
	if (argc > 0 && argv[0] != nullptr)
		filtered.push_back(argv[0]);
	else
		filtered.push_back(const_cast<char *>("tico-flycast-standalone"));

	Log("raw argc=%d", argc);
	for (int i = 0; i < argc; ++i)
		Log("raw argv[%d]=%s", i, argv[i] ? argv[i] : "(null)");

	char *contentPath = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		if (argv[i] == nullptr)
			continue;

		if (std::strcmp(argv[i], "--tico-rom") == 0)
		{
			if (i + 1 < argc && !IsSyntheticLaunchArg(argv[i + 1]))
			{
				contentPath = argv[i + 1];
				Log("selected --tico-rom content: %s", contentPath);
			}
			++i;
			continue;
		}
		if (std::strcmp(argv[i], "--tico-hour") == 0 || std::strcmp(argv[i], "--tico-token") == 0)
		{
			++i;
			continue;
		}
		if (std::strcmp(argv[i], "--tico-sh4clock") == 0)
		{
			if (i + 1 < argc && argv[i + 1] != nullptr)
				SetTicoSh4Clock(std::atoi(argv[i + 1]), "launch");
			++i;
			continue;
		}
		if (IsSyntheticLaunchArg(argv[i]))
		{
			Log("ignoring synthetic launch argument: %s", argv[i]);
			continue;
		}
		if (contentPath == nullptr && LooksLikeContentPath(argv[i]))
		{
			contentPath = argv[i];
			Log("selected positional content: %s", contentPath);
			continue;
		}
		if (g_launchTitle.empty())
		{
			g_launchTitle = argv[i];
			Log("selected launch title: %s", g_launchTitle.c_str());
		}
	}

	if (contentPath != nullptr)
		filtered.push_back(contentPath);
	else
		Log("no content path found in launch arguments");

	filtered.push_back(nullptr);
	Log("filtered argc=%d", static_cast<int>(filtered.size()) - 1);
	for (size_t i = 0; i + 1 < filtered.size(); ++i)
		Log("filtered argv[%zu]=%s", i, filtered[i] ? filtered[i] : "(null)");
	return filtered;
}

void SetupTicoDirectories()
{
	flycast::mkdir("sdmc:/tico", 0777);
	flycast::mkdir("sdmc:/tico/config", 0777);
	flycast::mkdir("sdmc:/tico/config/flycast", 0777);
	flycast::mkdir("sdmc:/tico/debug", 0777);
	flycast::mkdir("sdmc:/tico/system", 0777);
	flycast::mkdir("sdmc:/tico/system/dc", 0777);
	flycast::mkdir("sdmc:/tico/saves", 0777);
	flycast::mkdir("sdmc:/tico/saves/dc", 0777);
	flycast::mkdir("sdmc:/tico/states", 0777);
	flycast::mkdir("sdmc:/tico/states/dc", 0777);
	flycast::mkdir("sdmc:/tico/assets", 0777);

	set_user_config_dir("sdmc:/tico/config/flycast/");
	set_user_data_dir("sdmc:/tico/saves/dc/");

	add_system_config_dir("sdmc:/tico/config/flycast/");
	add_system_config_dir("sdmc:/tico/system/dc/");
	add_system_config_dir("romfs:/");
	add_system_config_dir("./");

	add_system_data_dir("sdmc:/tico/system/dc/");
	add_system_data_dir("sdmc:/tico/saves/dc/");
	add_system_data_dir("romfs:/");
	add_system_data_dir("./");
	add_system_data_dir("data/");
}

void ReadTicoSh4ClockOverride()
{
	if (g_ticoSh4Clock != 0)
		return;

	FILE *fp = std::fopen("sdmc:/tico/config/flycast/tico-sh4clock.txt", "r");
	if (fp == nullptr)
		return;

	char line[64] = {};
	if (std::fgets(line, sizeof(line), fp) != nullptr)
		SetTicoSh4Clock(std::atoi(line), "file");
	std::fclose(fp);
}

void ApplyTicoPerformanceDefaults(const char *stage, bool contentLoaded)
{
	config::RendererType.override(RenderType::Vulkan);
	config::ThreadedRendering.override(true);
	config::DynarecEnabled.override(true);
	config::MaxThreads.override(3);
	config::FastGDRomLoad.override(true);

	const bool windowsCe = contentLoaded && ip_meta.isWindowsCE();
	int sh4Clock = g_ticoSh4Clock;
	const char *sh4ClockSource = "config";
	if (sh4Clock != 0)
		sh4ClockSource = "tico";
	else if (windowsCe)
	{
		sh4Clock = 180;
		sh4ClockSource = "wince-default";
	}
	if (sh4Clock != 0)
		config::Sh4Clock.override(sh4Clock);

	Log("perf defaults (%s): wince=%d renderer=%d threaded=%d dynarec=%d maxThreads=%d fastGD=%d sh4=%d source=%s",
			stage,
			windowsCe ? 1 : 0,
			(int)(RenderType)config::RendererType,
			(bool)config::ThreadedRendering ? 1 : 0,
			(bool)config::DynarecEnabled ? 1 : 0,
			(int)config::MaxThreads,
			(bool)config::FastGDRomLoad ? 1 : 0,
			(int)config::Sh4Clock,
			sh4ClockSource);
}

void SanitizeContentPath()
{
	if (settings.content.path.rfind("disk$", 0) != 0)
		return;

	Log("ignoring synthetic content path: %s", settings.content.path.c_str());
	settings.content.path.clear();
	settings.content.fileName.clear();
	settings.content.title.clear();
}

void UpdateDisplayMode()
{
	const u8 mode = appletGetOperationMode();
	if (mode == g_lastOperationMode)
		return;

	if (mode == AppletOperationMode_Handheld)
	{
		nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
		nwindowSetCrop(nwindowGetDefault(), 0, 0, 1280, 720);
	}
	else
	{
		nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
		nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
	}
	g_lastOperationMode = mode;
}

bool InitPlatform()
{
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		Log("Unable to ignore SIGPIPE");

	appletLockExit();
	g_exitLocked = true;

	Result rc = socketInitializeDefault();
	if (R_SUCCEEDED(rc))
		g_socketReady = true;
	else
		Log("socketInitializeDefault failed rc=0x%x", (unsigned)rc);

	rc = romfsInit();
	if (R_SUCCEEDED(rc))
		g_romfsReady = true;
	else
	{
		Log("romfsInit failed rc=0x%x", (unsigned)rc);
		return false;
	}

	nxlinkStdio();
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&g_pad);
	UpdateDisplayMode();
	return true;
}

void ShutdownPlatform()
{
	if (g_romfsReady)
	{
		romfsExit();
		g_romfsReady = false;
	}
	if (g_socketReady)
	{
		socketExit();
		g_socketReady = false;
	}
	if (g_exitLocked)
	{
		appletUnlockExit();
		g_exitLocked = false;
	}
}

void MaybeChainloadTico()
{
	if (!g_chainloadToTico)
		return;

	const char *primary = "sdmc:/switch/tico.nro";
	const char *fallback = "sdmc:/switch/tico/tico.nro";
	const char *target = nullptr;

	FILE *fp = std::fopen(primary, "rb");
	if (fp != nullptr)
	{
		std::fclose(fp);
		target = primary;
	}
	else
	{
		fp = std::fopen(fallback, "rb");
		if (fp != nullptr)
		{
			std::fclose(fp);
			target = fallback;
		}
	}

	if (target == nullptr)
	{
		Log("no tico launcher found for chainload");
		return;
	}

	char args[512];
	std::snprintf(args, sizeof(args), "%s --resume", target);
	envSetNextLoad(target, args);
	Log("chainloading back to %s", target);
}

void PollSystemInput()
{
	padUpdate(&g_pad);
	const u64 buttons = padGetButtons(&g_pad);
	const u64 pressed = padGetButtonsDown(&g_pad);
	const bool exitHeld = (buttons & HidNpadButton_Plus) && (buttons & HidNpadButton_Minus);
	const bool exitPressed = exitHeld && ((pressed & HidNpadButton_Plus) || (pressed & HidNpadButton_Minus));
	if (!exitPressed)
		return;

	Log("Plus+Minus exit requested");
	g_chainloadToTico = true;
	g_running = false;
	try {
		emu.stop();
	} catch (const FlycastException& e) {
		Log("emu.stop failed during exit: %s", e.what());
	}
}

void StopEmulation()
{
	try {
		emu.stop();
	} catch (const FlycastException& e) {
		Log("emu.stop failed: %s", e.what());
	}
	try {
		emu.unloadGame();
	} catch (const FlycastException& e) {
		Log("emu.unloadGame failed: %s", e.what());
	}
}

} // namespace

static int RunStandalone(int argc, char *argv[])
{
	Log("tico-flycast standalone starting");

	std::vector<char *> flycastArgv = BuildFlycastArgv(argc, argv);
	const int flycastArgc = static_cast<int>(flycastArgv.size()) - 1;

	if (!InitPlatform())
	{
		ShutdownPlatform();
		return 1;
	}
	SetCurrentThreadAffinity("Flycast-main/render", 2);

	LogManager::Init();
	SetupTicoDirectories();
	ReadTicoSh4ClockOverride();
	cfgSetVirtual("config", "pvr.rend", "4");

	if (flycast_init(flycastArgc, flycastArgv.data()))
	{
		Log("Flycast initialization failed");
		ShutdownPlatform();
		return 1;
	}
	ApplyTicoPerformanceDefaults("post-init", false);

	SanitizeContentPath();
	if (!g_launchTitle.empty() && !IsSyntheticLaunchArg(g_launchTitle.c_str()))
		settings.content.title = g_launchTitle;
	gui_setState(GuiState::Closed);

	if (settings.content.path.empty())
	{
		Log("no content path supplied; exiting standalone runner");
		g_chainloadToTico = true;
		flycast_term();
		ShutdownPlatform();
		MaybeChainloadTico();
		return 0;
	}

	mainui_init();

	try {
		Log("loading content: %s", settings.content.path.c_str());
		emu.loadGame(settings.content.path.c_str());
		ApplyTicoPerformanceDefaults("post-load", true);
		emu.start();
	} catch (const FlycastException& e) {
		Log("content load failed: %s", e.what());
		mainui_term();
		flycast_term();
		ShutdownPlatform();
		return 1;
	}

	while (g_running && appletMainLoop() && emu.running())
	{
		UpdateDisplayMode();
		PollSystemInput();
		if (!g_running)
			break;

		os_UpdateInputState();
		try {
			emu.render();
		} catch (const FlycastException& e) {
			Log("render failed: %s", e.what());
			break;
		}

		if (imguiDriver != nullptr)
			imguiDriver->present();
	}

	StopEmulation();
	mainui_term();
	flycast_term();
	ShutdownPlatform();
	MaybeChainloadTico();
	Log("tico-flycast standalone clean exit");
	return 0;
}

int main(int argc, char *argv[])
{
	OpenBootLog();
	int rc = 1;
	try {
		rc = RunStandalone(argc, argv);
	} catch (const std::exception& e) {
		Log("unhandled std::exception: %s", e.what());
		ShutdownPlatform();
	} catch (...) {
		Log("unhandled unknown exception");
		ShutdownPlatform();
	}
	CloseBootLog();
	return rc;
}

void os_DoEvents()
{
	if (!appletMainLoop())
		g_running = false;
}

namespace hostfs
{

void saveScreenshot(const std::string& name, const std::vector<u8>& data)
{
	throw FlycastException("Not supported on Switch");
}

}

#endif // !LIBRETRO
