/// @file TicoCore.cpp
/// @brief Simplified libretro frontend for Flycast with tico overlay
/// Based on LibretroCoreStatic.cpp but stripped of multi-core switching

#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "json.hpp"
#include <SDL.h>
#include <SDL_mixer.h>
#include <cstring>
#include <fstream>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <cctype>
#include "TicoLogger.h"
#include <curl/curl.h>
#include <thread>
#include "rc_client.h"
#include "deps/stb/stb_image.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "TicoVulkan.h"
#include <libretro_vulkan.h>

// Forward declarations from imgread/common.h (full include deferred to avoid Event conflict)
struct Disc;
Disc* OpenDisc(const std::string& path, std::vector<unsigned char>* digest);

// NAOMI/Atomiswave are detected by extension (matches flycast's libretro
// frontend). Used to keep arcade ROMs away from the disc-only code paths.
static bool TicoIsArcadeRom(const std::string& path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext == ".lst" || ext == ".bin" || ext == ".dat" ||
           ext == ".zip" || ext == ".7z";
}

//==============================================================================
// SRAM Handling
//==============================================================================

void TicoCore::LoadSRAM()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    // Extract ROM name
    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    std::string savePathVMU = std::string(TicoConfig::SAVES_PATH) + filename + ".vmu";
    std::string savePathSRM = std::string(TicoConfig::SAVES_PATH) + filename + ".srm";

    std::ifstream file(savePathVMU, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        LOG_CORE("Loaded SRAM from %s", savePathVMU.c_str());
        return;
    }
    
    file.open(savePathSRM, std::ios::binary);
    if (file)
    {
        file.read((char *)data, size);
        LOG_CORE("Loaded SRAM from %s", savePathSRM.c_str());
        return;
    }

    LOG_WARN("CORE", "No SRAM file found (.vmu or .srm) at %s", TicoConfig::SAVES_PATH);
}

void TicoCore::SaveSRAM()
{
    size_t size = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (!size)
        return;

    void *data = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return;

    // Extract ROM name
    std::string filename = m_gamePath;
    size_t lastSlash = filename.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        filename = filename.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(".");
    if (lastDot != std::string::npos)
        filename = filename.substr(0, lastDot);

    // Ensure directory exists
    struct stat st = {0};
    if (stat(TicoConfig::SAVES_PATH, &st) == -1)
    {
#ifdef __SWITCH__
        mkdir(TicoConfig::SAVES_PATH, 0777);
#else
        mkdir(TicoConfig::SAVES_PATH, 0777);
#endif
    }

    std::string savePath = std::string(TicoConfig::SAVES_PATH) + filename + ".vmu";

    std::ofstream file(savePath, std::ios::binary);
    if (file)
    {
        file.write((const char *)data, size);
        LOG_CORE("Saved SRAM to %s", savePath.c_str());
    }
    else
    {
        LOG_ERROR("CORE", "Failed to save SRAM to %s", savePath.c_str());
    }
}

// Include libretro API
#include "libretro.h"

// Forward declarations for Flycast core functions
extern "C"
{
    void retro_init(void);
    void retro_deinit(void);
    void retro_set_environment(retro_environment_t);
    void retro_set_video_refresh(retro_video_refresh_t);
    void retro_set_audio_sample(retro_audio_sample_t);
    void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
    void retro_set_input_poll(retro_input_poll_t);
    void retro_set_input_state(retro_input_state_t);
    void retro_get_system_info(struct retro_system_info *info);
    void retro_get_system_av_info(struct retro_system_av_info *info);
    void retro_set_controller_port_device(unsigned port, unsigned device);
    void retro_reset(void);
    void retro_run(void);
    bool retro_load_game(const struct retro_game_info *game);
    void retro_unload_game(void);
    size_t retro_serialize_size(void);
    bool retro_serialize(void *data, size_t size);
    bool retro_unserialize(const void *data, size_t size);
    void *retro_get_memory_data(unsigned id);
    size_t retro_get_memory_size(unsigned id);
}

// Audio buffer flush - C++ linkage (defined in audiostream.cpp)
extern void retro_audio_flush_buffer(void);

// Static instance for callbacks
static TicoCore *s_instance = nullptr;

// HW render callback storage
static retro_hw_render_callback s_hwRenderCallback = {};

//==============================================================================
// RetroAchievements Callbacks
//==============================================================================
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    if (!s_instance) return 0;

    // Flycast/Dreamcast: SYSTEM_RAM is the main 16MB SDRAM
    // rcheevos addresses map directly to physical Dreamcast memory
    uint8_t* ram = (uint8_t*)retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
    size_t ram_size = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if (ram && address + num_bytes <= ram_size) {
        memcpy(buffer, ram + address, num_bytes);
        return num_bytes;
    }

    return 0;
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Persistent RA worker thread entry point
void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;

    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });

            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty())
                break;

            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }

        // Handle badge download jobs specially
        if (job.url == "__badge__") {
            self->DownloadAndCacheBadge(job.post_data, true);
            continue;
        }

        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;

        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            if (!job.post_data.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            } else {
                errorMsg = curl_easy_strerror(res);
                http_code = 500;
            }
            curl_easy_cleanup(curl);
        }

        // Queue result callback for main thread
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back(
                [job, http_code, readBuffer, errorMsg, requestUrl]() {
                    LOG_CORE("RA: HTTP Request -> %s", requestUrl.c_str());
                    if (!errorMsg.empty()) {
                        LOG_CORE("RA HTTP Error: %s", errorMsg.c_str());
                    }
                    LOG_CORE("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());

                    rc_api_server_response_t response;
                    memset(&response, 0, sizeof(response));
                    response.body = readBuffer.c_str();
                    response.body_length = readBuffer.size();
                    response.http_status_code = http_code;

                    rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                    if (cb) {
                        cb(&response, job.callback_data);
                    }
                }
            );
        }
    }
}

void TicoCore::StartRAWorker() {
#ifdef __SWITCH__
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_raThread);
        if (R_SUCCEEDED(rc)) {
            m_raThreadCreated = true;
            LOG_CORE("RA: Worker thread started (core 0, 256KB stack)");
        } else {
            LOG_CORE("RA: threadStart failed: 0x%x", rc);
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        LOG_CORE("RA: threadCreate failed: 0x%x", rc);
        m_raWorkerRunning = false;
    }
#endif
}

void TicoCore::StopRAWorker() {
#ifdef __SWITCH__
    if (!m_raThreadCreated) return;

    {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raWorkerRunning = false;
    }
    m_raJobCond.notify_one();

    threadWaitForExit(&m_raThread);
    threadClose(&m_raThread);
    m_raThreadCreated = false;
    LOG_CORE("RA: Worker thread stopped");
#endif
}

static void RC_CCONV RAServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    if (!s_instance) return;

    TicoCore::RAJob job;
    job.url = request->url;
    if (request->post_data) job.post_data = request->post_data;
    job.callback = (void*)callback;
    job.callback_data = callback_data;

#ifdef __SWITCH__
    if (s_instance->m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(s_instance->m_raJobMutex);
        s_instance->m_raJobQueue.push_back(std::move(job));
        s_instance->m_raJobCond.notify_one();
    } else {
        // Fallback: synchronous if worker not running
        LOG_CORE("RA: HTTP Request (sync) -> %s", request->url);
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, request->url);
            if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            else { LOG_CORE("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
            curl_easy_cleanup(curl);
        }
        LOG_CORE("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
        rc_api_server_response_t response;
        memset(&response, 0, sizeof(response));
        response.body = readBuffer.c_str();
        response.body_length = readBuffer.size();
        response.http_status_code = http_code;
        if (callback) callback(&response, callback_data);
    }
#else
    // On non-Switch: just do it synchronously
    LOG_CORE("RA: HTTP Request -> %s", request->url);
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    long http_code = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, request->url);
        if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else { LOG_CORE("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
        curl_easy_cleanup(curl);
    }
    LOG_CORE("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
#endif
}

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));

    m_systemDir = TicoConfig::SYSTEM_PATH;
    m_saveDir = TicoConfig::SAVES_PATH;

    // Per-game VMUs (overridable in flycast_settings.json). The core persists
    // them in retro_load/unload_game, so we don't do manual SRAM handling.
    m_configOptions["reicast_per_content_vmus"] = "All VMUs";

    // 4 Dreamcast controllers, each with a VMU + rumble (flycast defaults ports
    // 2-4 to None). Ignored for arcade.
    m_configOptions["reicast_device_port1_slot1"] = "VMU";
    m_configOptions["reicast_device_port2_slot1"] = "VMU";
    m_configOptions["reicast_device_port3_slot1"] = "VMU";
    m_configOptions["reicast_device_port4_slot1"] = "VMU";
    m_configOptions["reicast_device_port1_slot2"] = "Purupuru";
    m_configOptions["reicast_device_port2_slot2"] = "Purupuru";
    m_configOptions["reicast_device_port3_slot2"] = "Purupuru";
    m_configOptions["reicast_device_port4_slot2"] = "Purupuru";

    // Run SH4 on its own core.
    m_configOptions["reicast_threaded_rendering"] = "enabled";
}

TicoCore::~TicoCore()
{
    LOG_CORE("~TicoCore: destroying (gameLoaded=%d, initialized=%d, hwRender=%d)",
             m_gameLoaded, m_initialized, m_hwRender);

    UnloadGame();
    DestroyHWContext();

    if (m_initialized)
    {
        retro_deinit();
        m_initialized = false;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    StopRAWorker();
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

    // Load configuration to ensure variables are ready for init
    LoadConfig();
    LoadRAConfig();
    LOG_CORE("Config loaded, %lu options", m_configOptions.size());

    // Load trophy sound if audio is enabled
    bool soundEnabled = false;
#ifdef __SWITCH__
    std::string audioConfigPath = "sdmc:/tico/config/audio.jsonc";
#else
    std::string audioConfigPath = "tico/config/audio.jsonc";
#endif
    std::ifstream audioIn(audioConfigPath);
    if (audioIn.is_open()) {
        nlohmann::json j = nlohmann::json::parse(audioIn, nullptr, false, true);
        if (!j.is_discarded() && j.contains("sound_enabled")) {
            if (j["sound_enabled"].is_boolean()) {
                soundEnabled = j["sound_enabled"].get<bool>();
            }
        }
        audioIn.close();
    }
    if (soundEnabled) {
        // The chime is mixed into the game's audio stream by TicoAudio (the
        // Switch can't open a second audio device), so it must be a WAV that
        // SDL_LoadWAV can decode device-free. See assets/trophy.wav.
#ifdef __SWITCH__
        const char *trophyPath = "romfs:/assets/trophy.wav";
#else
        const char *trophyPath = "tico/assets/trophy.wav";
#endif
        if (TicoAudio::LoadTrophyGlobal(trophyPath)) LOG_CORE("RA: Loaded trophy.wav successfully.");
        else LOG_CORE("RA: Failed to load trophy.wav");
    }

    // Set environment callback before init
    retro_set_environment(EnvironmentCallback);

    // Initialize the core
    retro_init();

    // Initialize RetroAchievements when enabled for this platform.
    if (m_raEnabled) {
        m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
        if (m_rcClient) {
            LOG_CORE("RA: Client created");
            rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
                if (!s_instance) return;
                switch (event->type) {
                    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                        if (event->achievement) {
                            std::string title = event->achievement->title;
                            std::string desc = event->achievement->description;
                            std::string badge = event->achievement->badge_name;
                            s_instance->PushRANotification(title, desc, badge);
                            TicoAudio::PlayTrophyGlobal();
                            LOG_CORE("RA: Achievement triggered: %s (badge: %s)", title.c_str(), badge.c_str());
                        }
                        break;
                    case RC_CLIENT_EVENT_GAME_COMPLETED:
                        s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                        TicoAudio::PlayTrophyGlobal();
                        break;
                    case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                        if (event->leaderboard) {
                            s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                        }
                        break;
                    case RC_CLIENT_EVENT_SERVER_ERROR:
                        if (event->server_error) {
                            LOG_CORE("RA: Server error: %s", event->server_error->error_message);
                        }
                        break;
                    default:
                        break;
                }
            });
            StartRAWorker();
        }
    }

    // Set all callbacks
    retro_set_video_refresh(VideoRefreshCallback);
    retro_set_audio_sample(AudioSampleCallback);
    retro_set_audio_sample_batch(AudioSampleBatchCallback);
    retro_set_input_poll(InputPollCallback);
    retro_set_input_state(InputStateCallback);

    // Get core info
    struct retro_system_info sysInfo = {};
    retro_get_system_info(&sysInfo);

    LOG_CORE("Initialized: %s %s",
             sysInfo.library_name ? sysInfo.library_name : "Unknown",
             sysInfo.library_version ? sysInfo.library_version : "");

    m_initialized = true;
    return true;
}

bool TicoCore::InitEGLDualContext()
{
    // Called by LoadGame() AFTER retro_load_game() returns. By now flycast
    // has registered SET_HW_RENDER (Vulkan) and the negotiation interface
    // via our env handler. Bring up the device via that interface so flycast
    // gets the device IT wants (with VK_EXT_provoking_vertex, anisotropy,
    // etc.), then create the swapchain. After this returns, context_reset
    // can call GET_HW_RENDER_INTERFACE on us.
    if (TicoVulkan::IsReady())
        return true;
    if (!TicoVulkan::CreateDeviceAndSwapchain())
    {
        LOG_ERROR("CORE", "TicoVulkan::CreateDeviceAndSwapchain failed");
        return false;
    }
    LOG_CORE("Vulkan device + swapchain ready");
    return true;
}

void TicoCore::BindHWContext(bool /*enable*/)
{
    // No-op for Vulkan.
}

//==============================================================================
// Game Loading
//==============================================================================

// Debug logging helper removed - using TicoLogger.h

bool TicoCore::LoadGame(const std::string &path)
{
    LOG_CORE("LoadGame: Enter: %s", path.c_str());

    // Keyed by save states and RA hashing.
    m_gamePath = path;

    if (!m_initialized)
    {
        LOG_CORE("LoadGame: Not initialized, calling Init()");
        if (!Init())
        {
            LOG_ERROR("CORE", "LoadGame: Init() failed");
            return false;
        }
    }

    LOG_CORE("LoadGame: Opening file...");

    // Read ROM into memory
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        LOG_ERROR("CORE", "LoadGame: Failed to open ROM: %s", path.c_str());
        return false;
    }

    std::streamsize size = file.tellg();
    LOG_CORE("LoadGame: ROM size: %lld bytes", (long long)size);
    file.seekg(0, std::ios::beg);

    std::vector<char> romData(size);
    if (!file.read(romData.data(), size))
    {
        LOG_ERROR("CORE", "LoadGame: Failed to read ROM data");
        return false;
    }
    LOG_CORE("LoadGame: ROM data read successfully");

    // Load the game
    struct retro_game_info gameInfo = {};
    gameInfo.path = path.c_str();
    gameInfo.data = romData.data();
    gameInfo.size = romData.size();

    LOG_CORE("LoadGame: Calling retro_load_game...");
    LOG_CORE("  gameInfo.path = %s", gameInfo.path);
    LOG_CORE("  gameInfo.size = %zu", gameInfo.size);
    LOG_CORE("  gameInfo.data = %p", gameInfo.data);

    // Pre-open a separate disc (own FILE* handles) for RA hashing before the
    // emulator starts. Arcade ROMs have no disc -- they hash by name in
    // RAIdentifyGame, and OpenDisc() would throw on them.
    if (m_raEnabled && TicoIsArcadeRom(path)) {
        m_raHashDisc = nullptr;
    }
    else if (m_raEnabled) {
        try {
            m_raHashDisc = (void*)OpenDisc(path, nullptr);
            LOG_CORE("RA: Pre-opened disc for hashing: %p", m_raHashDisc);
        } catch (...) {
            LOG_CORE("RA: Failed to pre-open disc for hashing (non-fatal)");
            m_raHashDisc = nullptr;
        }
    }

    if (!retro_load_game(&gameInfo))
    {
        LOG_ERROR("CORE", "LoadGame: retro_load_game failed");
        return false;
    }
    LOG_CORE("LoadGame: retro_load_game succeeded");

    // Get AV info
    LOG_CORE("LoadGame: Getting AV info...");
    struct retro_system_av_info avInfo = {};
    retro_get_system_av_info(&avInfo);

    m_frameWidth = avInfo.geometry.base_width;
    m_frameHeight = avInfo.geometry.base_height;
    m_aspectRatio = avInfo.geometry.aspect_ratio > 0
                        ? avInfo.geometry.aspect_ratio
                        : (float)m_frameWidth / m_frameHeight;
    m_fps = avInfo.timing.fps > 0 ? avInfo.timing.fps : 60.0;

    // Use max dimensions for FBO so Flycast's internal resolution fits
    int fboW = avInfo.geometry.max_width > 0 ? avInfo.geometry.max_width : m_frameWidth;
    int fboH = avInfo.geometry.max_height > 0 ? avInfo.geometry.max_height : m_frameHeight;
    m_fboWidth = fboW;
    m_fboHeight = fboH;

    LOG_CORE("LoadGame: Game loaded: base %dx%d, FBO %dx%d @ %.2f fps",
             m_frameWidth, m_frameHeight, m_fboWidth, m_fboHeight, m_fps);

    // Initialize HW rendering if needed
    if (m_hwRender)
    {
        LOG_CORE("LoadGame: Initializing HW render context...");
        if (InitEGLDualContext())
        {
            if (s_hwRenderCallback.context_reset)
            {
                LOG_CORE("LoadGame: Calling context_reset...");
                s_hwRenderCallback.context_reset();
                m_hwContextActive = true;
                LOG_CORE("LoadGame: context_reset done");
            }
        }
    }

    // Set controller
    LOG_CORE("LoadGame: Setting controller port device...");
    for (unsigned port = 0; port < 4; ++port)
        retro_set_controller_port_device(port, RETRO_DEVICE_JOYPAD);

    m_gameLoaded = true;
    m_paused = false;
    LOG_CORE("LoadGame: Complete!");

    // (VMU/flash already loaded by retro_load_game; no manual SRAM load.)

    // Start RetroAchievements if enabled
    if (m_rcClient && m_raEnabled && !m_raUsername.empty()) {
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);

        if (!m_raToken.empty()) {
            LOG_CORE("RA: Beginning login with token...");
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* core = (TicoCore*)ud;
                    if (res == RC_OK) {
                        LOG_CORE("RA: Token login successful!");
                        RAIdentifyGame(c, core);
                    } else {
                        LOG_CORE("RA: Token login failed: %s. Retrying with password...", err ? err : "Unknown");
                        RALoginWithPassword(c, core);
                    }
                }, this);
        } else if (!m_raPassword.empty()) {
            LOG_CORE("RA: No token, logging in with password...");
            RALoginWithPassword(m_rcClient, this);
        } else {
            LOG_CORE("RA: No token or password configured. Skipping RA.");
        }
    }

    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    // (VMU/flash persisted by retro_unload_game; no manual SRAM save.)
    DestroyHWContext();
    retro_unload_game();
    m_gameLoaded = false;
}

//==============================================================================
// Frame execution
//==============================================================================

void TicoCore::RunFrame()
{
    if (!m_gameLoaded || m_paused)
        return;

    // Process RA callbacks on the main thread
    {
        std::vector<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_raCallbackMutex);
            if (!m_raPendingCallbacks.empty()) {
                callbacks = std::move(m_raPendingCallbacks);
            }
        }
        for (auto& cb : callbacks) {
            cb();
        }
    }

    // Process pending badge texture uploads (must happen on GL thread)
    ProcessPendingBadgeUploads();

    if (m_swapPending && m_swapDelayFrames > 0)
    {
        m_swapDelayFrames--;
        if (m_swapDelayFrames == 0)
        {
            retro_game_info info = {m_pendingSwapPath.c_str(), nullptr, 0, ""};
            if (m_diskControl.replace_image_index(0, &info) && m_diskControl.set_image_index(0))
            {
                m_diskControl.set_eject_state(false);
                LOG_CORE("Delayed SwapDisk executed successfully.");
            }
            else
            {
                LOG_ERROR("CORE", "Delayed SwapDisk failed during replace/set index.");
                m_diskControl.set_eject_state(false);
            }
            m_swapPending = false;
        }
    }

    retro_run();

    if (m_rcClient && m_gameLoaded) {
        rc_client_do_frame(m_rcClient);
    }
}

void TicoCore::ResizeFBO(int /*width*/, int /*height*/)
{
    // Vulkan path: flycast core manages its own offscreen images via
    // VulkanContext::PresentFrame. There's no frontend-side FBO to size.
}

void TicoCore::Reset()
{
    if (m_gameLoaded)
    {
        retro_reset();
    }
}

void TicoCore::Pause() { m_paused = true; }
void TicoCore::Resume() { m_paused = false; }

void TicoCore::DestroyHWContext()
{
    if (!m_hwContextActive)
        return;

    if (s_hwRenderCallback.context_destroy)
    {
        LOG_CORE("Calling context_destroy...");
        s_hwRenderCallback.context_destroy();
        LOG_CORE("context_destroy done");
    }
    m_hwContextActive = false;
}

//==============================================================================
// Input
//==============================================================================

void TicoCore::SetInputState(unsigned port, unsigned id, bool pressed)
{
    if (port < 4 && id < 16)
    {
        m_inputState[port][id] = pressed;
    }
}

void TicoCore::SetAnalogState(unsigned port, unsigned index, unsigned id, int16_t value)
{
    if (port < 4 && index < 2 && id < 2)
    {
        m_analogState[port][index][id] = value;
    }
}

void TicoCore::ClearInputs()
{
    memset(m_inputState, 0, sizeof(m_inputState));
    memset(m_analogState, 0, sizeof(m_analogState));
}

//==============================================================================
// Save States
//==============================================================================

void TicoCore::SaveState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    size_t size = retro_serialize_size();
    if (size == 0)
    {
        LOG_WARN("CORE", "SaveState: size 0");
        return;
    }

    std::vector<uint8_t> data(size);
    bool success = retro_serialize(data.data(), size);

    if (success)
    {
        FILE *fp = fopen(path.c_str(), "wb");
        if (fp)
        {
            fwrite(data.data(), 1, size, fp);
            fclose(fp);
            LOG_CORE("Saved state to %s", path.c_str());
        }
        else
        {
            LOG_ERROR("CORE", "Failed to open file for save state: %s", path.c_str());
        }
    }
    else
    {
        LOG_ERROR("CORE", "retro_serialize failed");
    }
}

void TicoCore::LoadState(const std::string &path)
{
    if (!m_gameLoaded)
        return;

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        LOG_WARN("CORE", "LoadState: File not found: %s", path.c_str());
        return;
    }

    fseek(fp, 0, SEEK_END);
    size_t fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fileSize == 0)
    {
        fclose(fp);
        return;
    }

    std::vector<uint8_t> data(fileSize);
    if (fread(data.data(), 1, fileSize, fp) != fileSize)
    {
        fclose(fp);
        return;
    }
    fclose(fp);

    // Flush audio before unserialize so we don't keep playing stale samples.
    LOG_CORE("Flushing Flycast internal audio buffer...");
    retro_audio_flush_buffer();
    if (m_audioFlushCallback)
    {
        LOG_CORE("Resetting SDL audio device...");
        m_audioFlushCallback();
    }

    bool success = retro_unserialize(data.data(), fileSize);

    if (success)
    {
        LOG_CORE("Loaded state from %s", path.c_str());

        // CRITICAL FIX: Run one frame to force display update
        // The core's DoState does NOT call UpdateDisplay.
        // This means the GPU's VRAM is restored but the FBO output is stale.
        LOG_CORE("Running one frame to force display update...");
        retro_run();
    }
    else
    {
        LOG_ERROR("CORE", "retro_unserialize failed");
    }
}

//==============================================================================
// Libretro Callbacks
//==============================================================================

bool TicoCore::EnvironmentCallback(unsigned cmd, void *data)
{
    if (!s_instance)
        return false;
    return s_instance->HandleEnvironment(cmd, data);
}

void TicoCore::VideoRefreshCallback(const void *data, unsigned width,
                                    unsigned height, size_t pitch)
{
    if (!s_instance)
        return;
    s_instance->HandleVideoRefresh(data, width, height, pitch);
}

void TicoCore::AudioSampleCallback(int16_t left, int16_t right)
{
    if (s_instance && s_instance->m_audioSampleCallback)
    {
        s_instance->m_audioSampleCallback(left, right);
    }
}

size_t TicoCore::AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    if (s_instance && s_instance->m_audioSampleBatchCallback)
    {
        return s_instance->m_audioSampleBatchCallback(data, frames);
    }
    return frames;
}

void TicoCore::InputPollCallback()
{
    // Input is polled externally
}

int16_t TicoCore::InputStateCallback(unsigned port, unsigned device,
                                     unsigned index, unsigned id)
{
    if (!s_instance)
        return 0;
    return s_instance->HandleInputState(port, device, index, id);
}

void TicoCore::LogCallback(enum retro_log_level level, const char *fmt, ...)
{
    // Map retro level to Tico level
    const char *levelStr = "CORE";
    if (level == RETRO_LOG_ERROR)
        levelStr = "CORE_ERR";
    else if (level == RETRO_LOG_WARN)
        levelStr = "CORE_WARN";
    else if (level == RETRO_LOG_INFO)
        levelStr = "CORE_INFO";
    else if (level == RETRO_LOG_DEBUG)
        levelStr = "CORE_DBG";

    // Use our logger
    va_list args;
    va_start(args, fmt);

    // Format string locally first
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Remove trailing newline if present since logger adds one
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';

    Logger::Instance().Log(Logger::Level::DEBUG, levelStr, Logger::None, "%s", buffer);
}

//==============================================================================
// Instance Callbacks
//==============================================================================

bool TicoCore::HandleEnvironment(unsigned cmd, void *data)
{
    switch (cmd)
    {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    {
        auto *cb = (struct retro_log_callback *)data;
        cb->log = LogCallback;
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    {
        *(const char **)data = m_systemDir.c_str();
        return true;
    }

    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    {
        *(const char **)data = m_saveDir.c_str();
        return true;
    }

    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    {
        // Accept any format
        return true;
    }

    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    {
        auto *hw = (struct retro_hw_render_callback *)data;
        if (hw->context_type != RETRO_HW_CONTEXT_VULKAN)
        {
            LOG_WARN("CORE", "SET_HW_RENDER rejected: only Vulkan is supported (got %d)",
                     hw->context_type);
            return false;
        }
        s_hwRenderCallback = *hw;
        m_hwRender = true;
        // get_current_framebuffer / get_proc_address are OpenGL-only — leave
        // them whatever the core set; flycast doesn't read them on the Vulkan path.
        LOG_CORE("HW render requested: Vulkan %u.%u",
                 hw->version_major, hw->version_minor);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
    {
        // Pin flycast's renderer selection to Vulkan. Without this the core
        // falls back to its hard-coded preferred order (D3D11 → Vulkan → GL),
        // which on Switch means it picks GL.
        *(unsigned *)data = RETRO_HW_CONTEXT_VULKAN;
        return true;
    }

    // The libretro standard spells this as GET_… (the frontend "gets" support
    // info from the core). Some private forks renamed it to SET_…; we use the
    // upstream name from libretro-common.
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
        return true;

    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    {
        const auto *iface =
            (const retro_hw_render_context_negotiation_interface_vulkan *)data;
        if (!iface ||
            iface->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
        {
            LOG_WARN("CORE", "Negotiation interface rejected (wrong type)");
            return false;
        }
        TicoVulkan::SetNegotiationInterface(iface);
        LOG_CORE("Vulkan negotiation interface registered (ver=%u)", iface->interface_version);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
    {
        const auto *hwIface = TicoVulkan::GetHwRenderInterface();
        if (!hwIface)
        {
            LOG_WARN("CORE", "GET_HW_RENDER_INTERFACE called before Vulkan ready");
            return false;
        }
        *(const retro_hw_render_interface **)data =
            reinterpret_cast<const retro_hw_render_interface *>(hwIface);
        return true;
    }

    case RETRO_ENVIRONMENT_GET_VARIABLE:
    {
        auto *var = (struct retro_variable *)data;
        if (!var || !var->key)
            return false;

        // Ensure config is loaded
        if (!m_configLoaded)
            LoadConfig();

        // Handle reicast_ options
        if (strncmp(var->key, "reicast_", 8) == 0)
        {
            auto it = m_configOptions.find(var->key);
            if (it != m_configOptions.end())
            {
                var->value = it->second.c_str();
                // printf("[TicoCore] GET_VARIABLE: %s -> %s\n", var->key, var->value);
                return true;
            }
        }

        // Check for legacy "flycast_" prefix mapping
        if (strncmp(var->key, "flycast_", 8) == 0)
        {
            std::string reicastKey = "reicast_" + std::string(var->key + 8);
            auto it = m_configOptions.find(reicastKey);
            if (it != m_configOptions.end())
            {
                var->value = it->second.c_str();
                return true;
            }
        }

        var->value = nullptr;
        return false;
    }

    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    {
        auto *avInfo = (struct retro_system_av_info *)data;
        m_frameWidth = avInfo->geometry.base_width;
        m_frameHeight = avInfo->geometry.base_height;
        if (avInfo->geometry.aspect_ratio > 0)
        {
            m_aspectRatio = avInfo->geometry.aspect_ratio;
        }
        m_fps = avInfo->timing.fps > 0 ? avInfo->timing.fps : 60.0;
        // Resize FBO if max dimensions changed
        int newMaxW = avInfo->geometry.max_width > 0 ? (int)avInfo->geometry.max_width : m_frameWidth;
        int newMaxH = avInfo->geometry.max_height > 0 ? (int)avInfo->geometry.max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        LOG_CORE("SET_SYSTEM_AV_INFO: base %dx%d, FBO %dx%d @ %.2f fps",
                 m_frameWidth, m_frameHeight, m_fboWidth, m_fboHeight, m_fps);
        return true;
    }

    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    {
        auto *geom = (struct retro_game_geometry *)data;
        m_frameWidth = geom->base_width;
        m_frameHeight = geom->base_height;
        if (geom->aspect_ratio > 0)
        {
            m_aspectRatio = geom->aspect_ratio;
        }
        // Resize FBO if max dimensions changed
        int newMaxW = geom->max_width > 0 ? (int)geom->max_width : m_frameWidth;
        int newMaxH = geom->max_height > 0 ? (int)geom->max_height : m_frameHeight;
        if (newMaxW != m_fboWidth || newMaxH != m_fboHeight)
        {
            m_fboWidth = newMaxW;
            m_fboHeight = newMaxH;
            ResizeFBO(m_fboWidth, m_fboHeight);
        }
        return true;
    }

    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;

    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = m_variablesUpdated;
        m_variablesUpdated = false;
        return true;

    default:
        break;
    }

    // Disc control interfaces (not in switch because cmd values may have flags)
    unsigned maskedCmd = cmd & 0xFFFF;
    if (maskedCmd == RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE)
    {
        const auto *cb = (const retro_disk_control_callback *)data;
        if (cb)
        {
            memset(&m_diskControl, 0, sizeof(m_diskControl));
            m_diskControl.set_eject_state = cb->set_eject_state;
            m_diskControl.get_eject_state = cb->get_eject_state;
            m_diskControl.get_image_index = cb->get_image_index;
            m_diskControl.set_image_index = cb->set_image_index;
            m_diskControl.get_num_images = cb->get_num_images;
            m_diskControl.replace_image_index = cb->replace_image_index;
            m_diskControl.add_image_index = cb->add_image_index;
            m_hasDiskControl = true;
        }
        return true;
    }
    if (maskedCmd == RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE)
    {
        const auto *cb = (const retro_disk_control_ext_callback *)data;
        if (cb)
        {
            m_diskControl = *cb;
            m_hasDiskControl = true;
        }
        return true;
    }

    return false;
}

void TicoCore::HandleVideoRefresh(const void *data, unsigned width,
                                  unsigned height, size_t /*pitch*/)
{
    // Vulkan path: data is RETRO_HW_FRAME_BUFFER_VALID for hw-rendered
    // frames, and the actual image arrives via TicoVulkan's set_image
    // callback. We track the dimensions both for aspect ratio AND so
    // TicoVulkan knows the source extent for the swapchain blit.
    if (!data && !m_hwRender)
        return;
    m_frameWidth = width;
    m_frameHeight = height;
    TicoVulkan::SetSourceExtent(width, height);
}

int16_t TicoCore::HandleInputState(unsigned port, unsigned device,
                                   unsigned index, unsigned id)
{
    if (port >= 4)
        return 0;

    if (device == RETRO_DEVICE_JOYPAD)
    {
        if (id < 16)
        {
            return m_inputState[port][id] ? 1 : 0;
        }
    }
    else if (device == RETRO_DEVICE_ANALOG)
    {
        if (index < 2 && id < 2)
        {
            return m_analogState[port][index][id];
        }
    }

    return 0;
}

//==============================================================================
// Configuration
//==============================================================================

void TicoCore::LoadConfig()
{
    if (m_configLoaded)
        return;

    const char *configPath;
#ifdef __SWITCH__
    configPath = "sdmc:/tico/config/cores/flycast.jsonc";
#else
    configPath = "tico/config/cores/flycast.jsonc";
#endif

    std::ifstream f(configPath);
    if (!f.good())
    {
        LOG_WARN("CORE", "No config found at %s. Creating exhaustive defaults.", configPath);
        nlohmann::json defaultJ;
        defaultJ["reicast_renderer"] = "vulkan";
        defaultJ["reicast_internal_resolution"] = "640x480";
        defaultJ["reicast_region"] = "USA";
        defaultJ["reicast_language"] = "English";
        defaultJ["reicast_hle_bios"] = "disabled";
        defaultJ["reicast_boot_to_bios"] = "disabled";
        defaultJ["reicast_enable_dsp"] = "enabled";
        defaultJ["reicast_allow_service_buttons"] = "disabled";
        defaultJ["reicast_force_freeplay"] = "enabled";
        defaultJ["reicast_emulate_bba"] = "disabled";
        defaultJ["reicast_upnp"] = "enabled";
        defaultJ["reicast_dcnet"] = "disabled";
        defaultJ["reicast_cable_type"] = "TV (Composite)";
        defaultJ["reicast_broadcast"] = "NTSC";
        defaultJ["reicast_screen_rotation"] = "horizontal";
        defaultJ["reicast_alpha_sorting"] = "per-triangle (normal)";
        defaultJ["reicast_oit_abuffer_size"] = "512MB";
        defaultJ["reicast_oit_layers"] = "32";
        defaultJ["reicast_emulate_framebuffer"] = "disabled";
        defaultJ["reicast_enable_rttb"] = "disabled";
        defaultJ["reicast_mipmapping"] = "enabled";
        defaultJ["reicast_fog"] = "enabled";
        defaultJ["reicast_volume_modifier_enable"] = "enabled";
        defaultJ["reicast_anisotropic_filtering"] = "4";
        defaultJ["reicast_texture_filtering"] = "0";
        defaultJ["reicast_delay_frame_swapping"] = "enabled";
        defaultJ["reicast_detect_vsync_swap_interval"] = "disabled";
        defaultJ["reicast_pvr2_filtering"] = "disabled";
        defaultJ["reicast_texupscale"] = "1";
        defaultJ["reicast_texupscale_max_filtered_texture_size"] = "256";
        defaultJ["reicast_native_depth_interpolation"] = "disabled";
        defaultJ["reicast_fix_upscale_bleeding_edge"] = "enabled";
        defaultJ["reicast_threaded_rendering"] = "enabled";
        defaultJ["reicast_auto_skip_frame"] = "disabled";
        defaultJ["reicast_frame_skipping"] = "disabled";
        defaultJ["reicast_widescreen_cheats"] = "disabled";
        defaultJ["reicast_widescreen_hack"] = "disabled";
        defaultJ["reicast_gdrom_fast_loading"] = "disabled";
        defaultJ["reicast_dc_32mb_mod"] = "disabled";
        defaultJ["reicast_sh4clock"] = "200";
        defaultJ["reicast_custom_textures"] = "disabled";
        defaultJ["reicast_dump_textures"] = "disabled";
        defaultJ["reicast_analog_stick_deadzone"] = "15%";
        defaultJ["reicast_trigger_deadzone"] = "0%";
        defaultJ["reicast_digital_triggers"] = "disabled";
        defaultJ["reicast_network_output"] = "disabled";
        defaultJ["reicast_show_lightgun_settings"] = "disabled";
        defaultJ["reicast_lightgun_crosshair_size_scaling"] = "100%";
        defaultJ["reicast_lightgun1_crosshair"] = "disabled";
        defaultJ["reicast_lightgun2_crosshair"] = "disabled";
        defaultJ["reicast_lightgun3_crosshair"] = "disabled";
        defaultJ["reicast_lightgun4_crosshair"] = "disabled";
        defaultJ["reicast_device_port1_slot1"] = "VMU";
        defaultJ["reicast_device_port1_slot2"] = "Purupuru";
        defaultJ["reicast_device_port2_slot1"] = "VMU";
        defaultJ["reicast_device_port2_slot2"] = "Purupuru";
        defaultJ["reicast_device_port3_slot1"] = "VMU";
        defaultJ["reicast_device_port3_slot2"] = "Purupuru";
        defaultJ["reicast_device_port4_slot1"] = "VMU";
        defaultJ["reicast_device_port4_slot2"] = "Purupuru";
        defaultJ["reicast_per_content_vmus"] = "All VMUs";
        defaultJ["reicast_vmu_sound"] = "disabled";
        defaultJ["reicast_show_vmu_screen_settings"] = "disabled";
        defaultJ["reicast_vmu1_screen_display"] = "disabled";
        defaultJ["reicast_vmu1_screen_position"] = "Upper Left";
        defaultJ["reicast_vmu1_screen_size_mult"] = "1x";
        defaultJ["reicast_vmu1_pixel_on_color"] = "DEFAULT_ON 00";
        defaultJ["reicast_vmu1_pixel_off_color"] = "DEFAULT_OFF 01";
        defaultJ["reicast_vmu1_screen_opacity"] = "100%";
        defaultJ["reicast_vmu2_screen_display"] = "disabled";
        defaultJ["reicast_vmu2_screen_position"] = "Upper Right";
        defaultJ["reicast_vmu2_screen_size_mult"] = "1x";
        defaultJ["reicast_vmu2_pixel_on_color"] = "DEFAULT_ON 00";
        defaultJ["reicast_vmu2_pixel_off_color"] = "DEFAULT_OFF 01";
        defaultJ["reicast_vmu2_screen_opacity"] = "100%";
        defaultJ["reicast_vmu3_screen_display"] = "disabled";
        defaultJ["reicast_vmu3_screen_position"] = "Lower Left";
        defaultJ["reicast_vmu3_screen_size_mult"] = "1x";
        defaultJ["reicast_vmu3_pixel_on_color"] = "DEFAULT_ON 00";
        defaultJ["reicast_vmu3_pixel_off_color"] = "DEFAULT_OFF 01";
        defaultJ["reicast_vmu3_screen_opacity"] = "100%";
        defaultJ["reicast_vmu4_screen_display"] = "disabled";
        defaultJ["reicast_vmu4_screen_position"] = "Lower Right";
        defaultJ["reicast_vmu4_screen_size_mult"] = "1x";
        defaultJ["reicast_vmu4_pixel_on_color"] = "DEFAULT_ON 00";
        defaultJ["reicast_vmu4_pixel_off_color"] = "DEFAULT_OFF 01";
        defaultJ["reicast_vmu4_screen_opacity"] = "100%";

        // Write the file
        std::ofstream out(configPath);
        if (out.good()) {
            out << defaultJ.dump(4);
            out.close();
            LOG_CORE("Created default config at %s", configPath);
        }

        // Apply defaults immediately
        for (auto &el : defaultJ.items()) {
            if (el.value().is_string()) {
                m_configOptions[el.key()] = el.value().get<std::string>();
            }
        }
        m_configLoaded = true;
        return;
    }

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
    if (j.is_discarded())
    {
        LOG_ERROR("CORE", "Failed to parse config at %s", configPath);
        m_configLoaded = true;
        return;
    }

    for (auto &el : j.items())
    {
        if (el.value().is_string())
        {
            m_configOptions[el.key()] = el.value().get<std::string>();
        }
        else if (el.value().is_number())
        {
            m_configOptions[el.key()] = std::to_string(el.value().get<float>());
        }
    }

    m_configLoaded = true;
    LOG_CORE("Loaded %lu options from %s", m_configOptions.size(), configPath);
}

std::string TicoCore::GetConfigValue(const std::string &key, const std::string &defaultVal)
{
    auto it = m_configOptions.find(key);
    if (it != m_configOptions.end())
    {
        return it->second;
    }
    return defaultVal;
}

//==============================================================================
// Disk Control
//==============================================================================

unsigned TicoCore::GetDiskCount() const
{
    if (!m_hasDiskControl || !m_diskControl.get_num_images)
        return 0;
    return m_diskControl.get_num_images();
}

unsigned TicoCore::GetCurrentDiskIndex() const
{
    if (!m_hasDiskControl || !m_diskControl.get_image_index)
        return 0;
    return m_diskControl.get_image_index();
}

bool TicoCore::SwapDisk(unsigned index)
{
    if (!m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.set_image_index)
        return false;

    LOG_CORE("SwapDisk: index=%u", index);

    if (!m_diskControl.set_eject_state(true))
    {
        LOG_ERROR("CORE", "SwapDisk: Eject failed");
        return false;
    }

    if (!m_diskControl.set_image_index(index))
    {
        LOG_ERROR("CORE", "SwapDisk: set_image_index(%u) failed", index);
        m_diskControl.set_eject_state(false);
        return false;
    }

    if (!m_diskControl.set_eject_state(false))
    {
        LOG_ERROR("CORE", "SwapDisk: Insert failed");
        return false;
    }

    LOG_CORE("SwapDisk: Successfully swapped to disc %u", index);
    return true;
}

bool TicoCore::GetDiskLabel(unsigned index, std::string &label) const
{
    if (!m_hasDiskControl || !m_diskControl.get_image_label)
        return false;

    char buf[256];
    if (m_diskControl.get_image_label(index, buf, sizeof(buf)))
    {
        label = buf;
        return true;
    }
    return false;
}

bool TicoCore::SwapDiskByPath(const std::string &discPath)
{
    if (!m_hasDiskControl || !m_diskControl.set_eject_state ||
        !m_diskControl.replace_image_index || !m_diskControl.set_image_index)
    {
        return false;
    }

    if (!m_diskControl.set_eject_state(true))
    {
        LOG_ERROR("CORE", "SwapDiskByPath: Eject failed");
        return false;
    }

    m_swapPending = true;
    m_swapDelayFrames = 120; // Wait 2 seconds of emulated time (at 60fps) before inserting
    m_pendingSwapPath = discPath;

    LOG_CORE("SwapDiskByPath: Ejected tray, queued replacement");
    return true;
}

//==============================================================================
// RetroAchievements Functionality
//==============================================================================

void TicoCore::LoadRAConfig() {
    m_raEnabled = false;
    m_raHardcore = false;
    m_raUsername = "";
    m_raToken = "";
    m_raPassword = "";

#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/accounts.jsonc";
#else
    std::string configPath = "tico/config/accounts.jsonc";
#endif

    std::ifstream file(configPath);
    if (!file.is_open()) {
        LOG_CORE("WARN: accounts.jsonc not found at %s", configPath.c_str());
        return;
    }

    LOG_CORE("RA: Found accounts.jsonc at %s", configPath.c_str());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false, true);
    if (!j.is_discarded() && j.is_object()) {
        m_raEnabled = j.value("ra_enabled", false);
        m_raUsername = j.value("ra_username", "");
        m_raToken = j.value("ra_token", "");
        m_raPassword = j.value("ra_password", "");
        m_raHardcore = j.value("ra_hardcore_mode", false);

        // Read alert position
        std::string posStr = j.value("ra_alert_position", "top_right");
        if (posStr == "top_left") m_raAlertPosition = RAAlertPosition::TopLeft;
        else if (posStr == "top_right") m_raAlertPosition = RAAlertPosition::TopRight;
        else if (posStr == "bottom_left") m_raAlertPosition = RAAlertPosition::BottomLeft;
        else if (posStr == "bottom_right") m_raAlertPosition = RAAlertPosition::BottomRight;

        LOG_CORE("RA: Config loaded (Enabled: %d, User: %s, HasToken: %d, HasPassword: %d)",
            m_raEnabled, m_raUsername.c_str(), !m_raToken.empty(), !m_raPassword.empty());
    } else {
        LOG_CORE("WARN: Failed to parse accounts.jsonc for RA settings.");
    }
}

void TicoCore::SaveRAToken(const std::string& token) {
    m_raToken = token;

#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/accounts.jsonc";
#else
    std::string configPath = "tico/config/accounts.jsonc";
#endif

    std::ifstream in(configPath);
    nlohmann::json j;
    if (in.is_open()) {
        auto parsed = nlohmann::json::parse(in, nullptr, false, true);
        if (!parsed.is_discarded()) j = parsed;
        in.close();
    }

    j["ra_token"] = token;

    std::ofstream out(configPath);
    if (out.is_open()) {
        out << j.dump(4);
        out.close();
        LOG_CORE("RA: Saved token to config.");
    }
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core) {
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* cc, void* ud) {
            TicoCore* self = (TicoCore*)ud;
            if (res == RC_OK) {
                LOG_CORE("RA: Password login successful!");
                const rc_client_user_t* user = rc_client_get_user_info(cc);
                if (user && user->token) {
                    self->SaveRAToken(user->token);
                }
                RAIdentifyGame(cc, self);
            } else {
                LOG_CORE("RA: Password login failed: %s", err ? err : "Unknown");
            }
        }, core);
}

#include "imgread/common.h"
#include <rc_hash.h>

static Disc* s_hashDisk = nullptr;

// --- RA CD reader: track handle with file-relative addressing ---
// rcheevos expects CD reader callbacks that work with file-relative sectors.
// The ISO9660 filesystem on a GD-ROM uses LBAs relative to the HD area start (45000),
// but the BIN file starts at FAD 45150 (45000 + 150 pregap). The default rcheevos
// cdreader handles this by computing track_first_sector from the raw sector header MSF.
// We replicate that behavior here, reading directly from the track BIN file.

struct TicoRATrack {
    hostfs::File* file;    // The track's BIN file (borrowed from RawTrackFile)
    uint32_t sector_size;  // 2352, 2048, 2336
    uint32_t header_size;  // Bytes to skip to get 2048-byte user data (16 for MODE1/2352, 24 for MODE2/2352, 0 for 2048)
    uint32_t first_sector; // LBA base computed from raw sector header MSF
    bool     owns_file;    // false — we borrow from the Disc object
};

// Convert BCD MSF in a raw sector header to an LBA sector number
static uint32_t msf_to_lba(const uint8_t header[16]) {
    int minutes = (header[12] >> 4) * 10 + (header[12] & 0x0F);
    int seconds = (header[13] >> 4) * 10 + (header[13] & 0x0F);
    int frames  = (header[14] >> 4) * 10 + (header[14] & 0x0F);
    return (uint32_t)(((minutes * 60) + seconds) * 75 + frames - 150);
}

static void* tico_cdreader_open_track(const char* path, uint32_t track) {
    if (!s_hashDisk) return nullptr;

    // Find the Track object
    const Track* t = nullptr;
    if (track == RC_HASH_CDTRACK_FIRST_DATA) {
        for (const Track& tr : s_hashDisk->tracks) {
            if (tr.isDataTrack()) { t = &tr; break; }
        }
    } else if (track > 0 && track <= s_hashDisk->tracks.size()) {
        t = &s_hashDisk->tracks[track - 1];
    }
    if (!t || !t->file) return nullptr;

    // Get the underlying RawTrackFile to access its FILE* and sector format
    RawTrackFile* rtf = dynamic_cast<RawTrackFile*>(t->file);
    if (!rtf) return nullptr;

    TicoRATrack* rat = new TicoRATrack();
    rat->file = rtf->file;
    rat->sector_size = rtf->fmt;
    rat->owns_file = false;

    // Determine header size and compute first_sector from raw sector header
    if (rat->sector_size == 2352) {
        // Raw sector — read sector 16 (PVD) to determine header size and first_sector
        uint8_t header[32];
        // Sector 16 is at file offset 16 * 2352
        rat->file->seek(16 * (s64)rat->sector_size, SEEK_SET);
        size_t rd = rat->file->read(header, 1, sizeof(header));
        if (rd >= 32) {
            // Check for sync pattern 00 FF FF FF FF FF FF FF FF FF FF 00
            static const uint8_t sync[] = {0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00};
            if (memcmp(header, sync, 12) == 0) {
                // MODE1/2352: header is 16 bytes (sync 12 + MSF 3 + mode 1)
                // MODE2/2352: header is 24 bytes (sync 12 + MSF 3 + mode 1 + subheader 8)
                if (memcmp(&header[25], "CD001", 5) == 0)
                    rat->header_size = 24; // MODE2/2352
                else
                    rat->header_size = 16; // MODE1/2352
                
                // Compute first_sector from MSF in sector 16's header
                uint32_t sector_lba = msf_to_lba(header);
                rat->first_sector = sector_lba - 16;
            } else {
                rat->header_size = 16;
                rat->first_sector = t->StartFAD;
            }
        } else {
            rat->header_size = 16;
            rat->first_sector = t->StartFAD;
        }
    } else if (rat->sector_size == 2048) {
        rat->header_size = 0;
        rat->first_sector = t->StartFAD;
    } else if (rat->sector_size == 2336) {
        rat->header_size = 8;
        rat->first_sector = t->StartFAD;
    } else {
        rat->header_size = 0;
        rat->first_sector = t->StartFAD;
    }

    LOG_CORE("RA: Opened track %u: sector_size=%u, header=%u, first_sector=%u (StartFAD=%u)",
             track, rat->sector_size, rat->header_size, rat->first_sector, t->StartFAD);

    return rat;
}

static size_t tico_cdreader_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes) {
    TicoRATrack* rat = static_cast<TicoRATrack*>(track_handle);
    if (!rat || !rat->file) return 0;

    if (sector < rat->first_sector) return 0;

    // Compute file offset: (sector - first_sector) * sector_size + header_size
    long file_offset = (long)(sector - rat->first_sector) * (long)rat->sector_size + rat->header_size;
    
    // Read user data (up to 2048 bytes per sector)
    uint32_t data_size = rat->sector_size - rat->header_size;
    if (rat->sector_size == 2352 && rat->header_size == 16)
        data_size = 2048; // MODE1: 2352 - 16 header - 288 footer = 2048
    else if (rat->sector_size == 2352 && rat->header_size == 24)
        data_size = 2048; // MODE2 Form1: 2352 - 24 header - 280 footer = 2048
    else if (rat->sector_size == 2048)
        data_size = 2048;
    else if (rat->sector_size == 2336)
        data_size = 2048; // 2336 - 8 header - 280 footer

    requested_bytes = std::min<size_t>(requested_bytes, data_size);

    rat->file->seek((s64)file_offset, SEEK_SET);
    size_t nread = rat->file->read(buffer, 1, requested_bytes);
    return nread;
}

static void tico_cdreader_close_track(void* track_handle) {
    TicoRATrack* rat = static_cast<TicoRATrack*>(track_handle);
    if (rat) {
        // Don't close the file — it belongs to the Disc object
        delete rat;
    }
}

static uint32_t tico_cdreader_first_track_sector(void* track_handle) {
    TicoRATrack* rat = static_cast<TicoRATrack*>(track_handle);
    return rat ? rat->first_sector : 0;
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core) {
    if (!c || !core || !core->m_gameLoaded) return;

    // Arcade hashes by rom name (RC_CONSOLE_ARCADE); Dreamcast hashes the disc.
    const bool arcade = TicoIsArcadeRom(core->m_gamePath);
    const uint32_t console_id = arcade ? 27u   // RC_CONSOLE_ARCADE
                                       : 40u;  // RC_CONSOLE_DREAMCAST

    Disc* hashDisc = nullptr;
    if (arcade) {
        LOG_CORE("RA: Identifying arcade game (console %u)...", console_id);
    } else {
        hashDisc = (Disc*)core->m_raHashDisc;
        if (!hashDisc) {
            LOG_CORE("RA: No pre-opened disc for hashing, skipping identification");
            return;
        }
        s_hashDisk = hashDisc;

        // Set custom CD hooks (disc hashing only)
        rc_hash_callbacks_t callbacks = {};
        callbacks.cdreader.open_track = tico_cdreader_open_track;
        callbacks.cdreader.read_sector = tico_cdreader_read_sector;
        callbacks.cdreader.close_track = tico_cdreader_close_track;
        callbacks.cdreader.first_track_sector = tico_cdreader_first_track_sector;
        rc_client_set_hash_callbacks(c, &callbacks);

        LOG_CORE("RA: Identifying game for console ID %u...", console_id);
    }

    // Hashing is synchronous, so s_hashDisk only needs to be valid for this call.
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(),
        nullptr, 0,
        [](int res, const char* err, rc_client_t* cc, void* ud) {
            TicoCore* self = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_game_t* game = rc_client_get_game_info(cc);
                LOG_CORE("RA: Game identified and loaded successfully: %s", game ? game->title : "Unknown");
                std::string gameTitle = (game && game->title) ? game->title : "Unknown";
                self->PushRANotification("RetroAchievements", "Playing: " + gameTitle, "ra_icon");
                self->PreloadRABadges();
            } else {
                LOG_CORE("RA: Game identification failed: %s", err ? err : "Unknown");
                self->PushRANotification("RetroAchievements", "Rom hash doesn't match or unable to recognize the game.", "ra_icon");
            }
        }, core);

    // Hashing is done (synchronous). Clean up the disc (disc path only).
    if (!arcade) {
        s_hashDisk = nullptr;
        delete hashDisc;
        core->m_raHashDisc = nullptr;
    }
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc, const std::string& badge) {
    std::lock_guard<std::mutex> lock(m_raCallbackMutex);
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    n.timer = 0.0f;
    n.textureId = 0; // Resolved by overlay's ResolveNotificationTextures()
    m_raNotifications.push_back(n);

    LOG_CORE("RA: Notification queued: '%s' (badge: %s, total: %zu)",
        title.c_str(), badge.c_str(), m_raNotifications.size());

    // If it's a specific badge we haven't cached yet, trigger a download
    if (badge != "ra_icon" && !badge.empty() && m_raBadgeCache.find(badge) == m_raBadgeCache.end()) {
        LOG_CORE("RA: Badge '%s' not in cache, requesting download", badge.c_str());
        DownloadAndCacheBadge(badge);
    }
}

void TicoCore::PreloadRABadges() {
    if (!m_rcClient) return;
    LOG_CORE("RA: Badge preloading skipped (lazy-load on demand)");
}

ImTextureID TicoCore::GetRABadgeTexture(const std::string& badge_name) {
    // Check in-memory cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;
    // Vulkan path: no overlay in v1, so we don't materialise badge textures.
    return 0;
}

void TicoCore::DownloadAndCacheBadge(const std::string& badge_name, bool execute_now) {
#ifdef __SWITCH__
    std::string badgePath = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    struct stat st = {0};
    if (stat("sdmc:/tico/assets/ra", &st) == -1) mkdir("sdmc:/tico/assets/ra", 0777);
#else
    std::string badgePath = "tico/assets/ra/" + badge_name + ".png";
    struct stat st = {0};
    if (stat("tico/assets/ra", &st) == -1) mkdir("tico/assets/ra", 0777);
#endif

    // If file exists locally, load via pending callback directly
    if (stat(badgePath.c_str(), &st) == 0 && st.st_size > 0) {
        std::ifstream file(badgePath, std::ios::binary);
        if (file) {
            std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)), {});
            if (!buffer.empty()) {
                std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
                m_raPendingBadgeUploads.push_back({badge_name, std::move(buffer)});
                return;
            }
        }
    }

    // Otherwise thread will fetch it
    std::string url = "https://media.retroachievements.org/Badge/" + badge_name + ".png";
    RAJob job;
    job.url = "__badge__"; // special sentinel
    job.post_data = badge_name; // use post_data to pass the name

#ifdef __SWITCH__
    if (!execute_now && m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raJobQueue.push_back(std::move(job));
        m_raJobCond.notify_one();
    } else {
        // Fallback for sync environment
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK && readBuffer.size() > 0) {
                std::ofstream badgeFile(badgePath, std::ios::binary);
                badgeFile.write(readBuffer.c_str(), readBuffer.size());
                std::vector<unsigned char> data(readBuffer.begin(), readBuffer.end());
                std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
                m_raPendingBadgeUploads.push_back({badge_name, std::move(data)});
            }
            curl_easy_cleanup(curl);
        }
    }
#endif
}

void TicoCore::ProcessPendingBadgeUploads() {
    // Runs on the main thread (RunFrame), the only place the Vulkan overlay
    // texture path is safe to touch. Decode each downloaded badge PNG and
    // upload it as an overlay texture so RA toasts show the achievement art.
    std::vector<std::pair<std::string, std::vector<unsigned char>>> uploads;
    {
        std::lock_guard<std::mutex> lock(m_raBadgeUploadMutex);
        if (m_raPendingBadgeUploads.empty())
            return;
        // Cap per-frame work so a burst of unlocks doesn't stall the GPU.
        size_t count = std::min(m_raPendingBadgeUploads.size(), (size_t)2);
        uploads.assign(std::make_move_iterator(m_raPendingBadgeUploads.begin()),
                       std::make_move_iterator(m_raPendingBadgeUploads.begin() + count));
        m_raPendingBadgeUploads.erase(m_raPendingBadgeUploads.begin(),
                                      m_raPendingBadgeUploads.begin() + count);
    }

    for (auto& upload : uploads) {
        const std::string& name = upload.first;
        if (upload.second.empty() || m_raBadgeCache.count(name))
            continue;

        int w = 0, h = 0, comp = 0;
        unsigned char* pixels = stbi_load_from_memory(
            upload.second.data(), (int)upload.second.size(), &w, &h, &comp, 4);
        if (!pixels) {
            LOG_CORE("RA: Badge decode failed: %s (%s)", name.c_str(), stbi_failure_reason());
            continue;
        }

        ImTextureID tex = 0;
#ifdef __SWITCH__
        tex = TicoVulkan::CreateOverlayTextureRGBA(pixels, (uint32_t)w, (uint32_t)h);
#endif
        stbi_image_free(pixels);

        if (tex) {
            m_raBadgeCache[name] = tex;
            LOG_CORE("RA: Badge uploaded: %s (%dx%d)", name.c_str(), w, h);
        } else {
            LOG_CORE("RA: Badge texture creation failed: %s", name.c_str());
        }
    }
}
