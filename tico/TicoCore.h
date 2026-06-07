/// @file TicoCore.h
/// @brief Simplified libretro frontend for Flycast with tico overlay
#pragma once

#include <string>
#include <map>
#include <cstdint>
#include <SDL.h>
#include <SDL_mixer.h>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <deque>
#include "libretro.h"

#ifdef __SWITCH__
#define Event SwitchEvent
#include <switch.h>
#undef Event
#endif

struct rc_client_t;

class IRenderer;

// RANotification / RAAlertPosition now live in the backend-agnostic overlay
// host header so the overlay need not include TicoCore.h.
#include "TicoOverlayHost.h"

/// @brief Simplified libretro core wrapper for Flycast
class TicoCore
{
public:
    TicoCore();
    ~TicoCore();

    /// @brief Initialize the core
    bool Init();

    /// @brief Load a game ROM
    bool LoadGame(const std::string &path);

    /// @brief Unload current game
    void UnloadGame();

    /// @brief Run a single frame
    void RunFrame();

    /// @brief Reset the game
    void Reset();

    /// @brief Pause/Resume
    void Pause();
    void Resume();
    bool IsPaused() const { return m_paused; }
    bool IsGameLoaded() const { return m_gameLoaded; }

    /// @brief Input handling
    void SetInputState(unsigned port, unsigned id, bool pressed);
    void SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value);
    void ClearInputs();

    /// @brief Video/Audio info
    unsigned int GetFrameTextureID() const { return m_frameTexture; }
    float GetAspectRatio() const { return m_aspectRatio; }
    int GetFrameWidth() const { return m_frameWidth; }
    int GetFrameHeight() const { return m_frameHeight; }
    int GetFBOWidth() const { return m_fboWidth; }
    int GetFBOHeight() const { return m_fboHeight; }
    double GetFPS() const { return m_fps; }
    bool IsHWRender() const { return m_hwRender; }

    /// @brief Get current game path
    std::string GetGamePath() const { return m_gamePath; }

    /// @brief Disk control
    bool HasDiskControl() const { return m_hasDiskControl; }
    unsigned GetDiskCount() const;
    unsigned GetCurrentDiskIndex() const;
    bool SwapDisk(unsigned index);
    bool SwapDiskByPath(const std::string &discPath);
    bool GetDiskLabel(unsigned index, std::string &label) const;

    /// @brief Save states
    void SaveState(const std::string &path);
    void LoadState(const std::string &path);

    /// @brief Set renderer for texture creation
    void SetRenderer(IRenderer *renderer) { m_renderer = renderer; }

    // Audio callbacks
    typedef void (*AudioSampleCallback_t)(int16_t left, int16_t right);
    typedef size_t (*AudioSampleBatchCallback_t)(const int16_t *data, size_t frames);
    typedef void (*AudioFlushCallback_t)();

    void SetAudioCallbacks(AudioSampleCallback_t sampleCb, AudioSampleBatchCallback_t batchCb, AudioFlushCallback_t flushCb = nullptr)
    {
        m_audioSampleCallback = sampleCb;
        m_audioSampleBatchCallback = batchCb;
        m_audioFlushCallback = flushCb;
    }

    void SetAudioFlushCallback(AudioFlushCallback_t flushCb)
    {
        m_audioFlushCallback = flushCb;
    }

private:
    void InitializeCore();
    void SetupCallbacks();
    bool InitEGLDualContext();
    void BindHWContext(bool enable);
    void DestroyHWContext();

    // SRAM handling (SaveRAM)
    void LoadSRAM();
    void SaveSRAM();

    // Libretro callbacks
    static bool EnvironmentCallback(unsigned cmd, void *data);
    static void VideoRefreshCallback(const void *data, unsigned width, unsigned height, size_t pitch);
    static void AudioSampleCallback(int16_t left, int16_t right);
    static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames);
    static void InputPollCallback();
    static int16_t InputStateCallback(unsigned port, unsigned device, unsigned index, unsigned id);
    static void LogCallback(enum retro_log_level level, const char *fmt, ...);

    // Instance callbacks
    bool HandleEnvironment(unsigned cmd, void *data);
    void HandleVideoRefresh(const void *data, unsigned width, unsigned height, size_t pitch);
    void HandleAudioBatch(const int16_t *data, size_t frames);
    int16_t HandleInputState(unsigned port, unsigned device, unsigned index, unsigned id);

    // State
    bool m_initialized = false;
    bool m_gameLoaded = false;
    bool m_paused = false;
    bool m_hwRender = false;
    bool m_hwContextActive = false;
    bool m_variablesUpdated = true;

    // Video
    unsigned int m_frameTexture = 0;
    unsigned int m_fbo = 0;
    unsigned int m_fbo_rbo = 0; // Depth/Stencil Renderbuffer
    int m_frameWidth = 640;
    int m_frameHeight = 480;
    int m_fboWidth = 0; // Actual FBO texture dimensions
    int m_fboHeight = 0;
    float m_aspectRatio = 4.0f / 3.0f;
    double m_fps = 60.0;
    void ResizeFBO(int width, int height);

    // Audio
    AudioSampleCallback_t m_audioSampleCallback = nullptr;
    AudioSampleBatchCallback_t m_audioSampleBatchCallback = nullptr;
    AudioFlushCallback_t m_audioFlushCallback = nullptr;

    // Input
    bool m_inputState[4][16] = {};
    int16_t m_analogState[4][2][2] = {};

    // Renderer
    IRenderer *m_renderer = nullptr;

    // Paths
    std::string m_systemDir;
    std::string m_saveDir;
    std::string m_gamePath;

    // Disk Control
    retro_disk_control_ext_callback m_diskControl = {};
    bool m_hasDiskControl = false;

    // Delayed Disk Swap
    bool m_swapPending = false;
    int m_swapDelayFrames = 0;
    std::string m_pendingSwapPath;

    // Configuration
    std::string GetConfigValue(const std::string &key, const std::string &defaultVal = "");
    void LoadConfig();
    std::map<std::string, std::string> m_configOptions;
    bool m_configLoaded = false;

    // RetroAchievements Client
    rc_client_t* m_rcClient = nullptr;
    bool m_raEnabled = false;
    std::string m_raUsername = "";
    std::string m_raToken = "";
    std::string m_raPassword = "";
    bool m_raHardcore = false;
    void LoadRAConfig();
    void SaveRAToken(const std::string& token);
    static void RAIdentifyGame(rc_client_t* c, TicoCore* core);

    static void RALoginWithPassword(rc_client_t* c, TicoCore* core);

public:
    // RA notifications queue (public for overlay access)
    std::vector<RANotification> m_raNotifications;
    RAAlertPosition m_raAlertPosition = RAAlertPosition::TopRight;
    void PushRANotification(const std::string& title, const std::string& desc,
                           const std::string& badge = "");

    // RA badge cache (badge_name -> overlay texture). ImTextureID is 64-bit so
    // it can hold a Vulkan VkDescriptorSet handle without truncation.
    std::map<std::string, ImTextureID> m_raBadgeCache;
    ImTextureID m_raIconTexture = 0;         // ra.svg icon
    void* m_raHashDisc = nullptr;            // Pre-opened Disc* for RA hashing (avoids concurrent file access)
    ImTextureID GetRABadgeTexture(const std::string& badge_name);
    void DownloadAndCacheBadge(const std::string& badge_name, bool execute_now = false); // runs on worker
    void PreloadRABadges();                   // called after game identification
    std::vector<std::pair<std::string, std::vector<unsigned char>>> m_raPendingBadgeUploads;
    std::mutex m_raBadgeUploadMutex;
    void ProcessPendingBadgeUploads();        // called from main thread (RunFrame)

public:
    // RA Worker Thread (persistent, proper libnx lifecycle)
    struct RAJob {
        std::string url;
        std::string post_data;
        void* callback;       // rc_client_server_callback_t (cast in .cpp)
        void* callback_data;
    };
    std::mutex m_raJobMutex;
    std::condition_variable m_raJobCond;
    std::deque<RAJob> m_raJobQueue;
    bool m_raWorkerRunning = false;

    std::mutex m_raCallbackMutex;
    std::vector<std::function<void()>> m_raPendingCallbacks;

#ifdef __SWITCH__
    Thread m_raThread;
    bool m_raThreadCreated = false;
#endif
    void StartRAWorker();
    void StopRAWorker();
    static void RAWorkerEntry(void* arg);
};
