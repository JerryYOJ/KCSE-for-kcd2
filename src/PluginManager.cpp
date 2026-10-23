#include "PluginManager.h"
#include "Interfaces.h"
#include <Windows.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <spdlog/spdlog.h>

namespace fs = std::filesystem;

// ---- Internal types ----

struct PluginListener {
    PluginHandle                         listener;
    KCSE::IMessagingInterface::EventCallback   callback;
};

struct LoadedPlugin {
    std::string                 dllPath;
    std::string                 dllName;
    std::string                 name;
    PluginHandle                handle = KCSE::kPluginHandle_Invalid;
    HMODULE                     hModule = nullptr;
    KCSE::PluginVersionData       version{};
    KCSE::PluginLoadFn           loadFn = nullptr;
    KCSE::PluginInfo              info{};
    bool                        loaded = false;
    uint32_t                    errorCode = 0;
    std::unique_ptr<KCSEInterfaceImpl> kcseInterface;  // bound to `handle`; kept alive for the plugin's whole lifetime (it caches KCSE::g_kcse/g_messaging from this at load time, for use from callbacks fired long after)
};

static std::vector<LoadedPlugin>                s_plugins;
static std::vector<std::vector<PluginListener>> s_listeners;
static std::mutex                               s_messagingLock;
static PluginHandle                             s_nextHandle = 1;

extern uint32_t g_kcseVersion;
extern uint32_t g_gameVersion;

// ---- Helpers ----

static bool CheckCompatibility(const KCSE::PluginVersionData& ver)
{
    if (ver.dataVersion != KCSE::PluginVersionData::kDataVersion)
        return false;

    if (ver.name[0] == 0)
        return false;

    if (ver.kcseVersionRequired > g_kcseVersion) {
        spdlog::warn("{} requires KCSE v{}, have v{}", ver.name, ver.kcseVersionRequired, g_kcseVersion);
        return false;
    }

    if (ver.compatibleGameVersions[0] != 0) {
        bool found = false;
        for (int i = 0; i < 16 && ver.compatibleGameVersions[i]; ++i) {
            if (ver.compatibleGameVersions[i] == g_gameVersion) {
                found = true;
                break;
            }
        }
        if (!found && !(ver.versionIndependence & KCSE::PluginVersionData::kVersionIndependent_Signatures)) {
            spdlog::warn("{} is not compatible with game version {}", ver.name, g_gameVersion);
            return false;
        }
    }

    return true;
}

static bool SafeCallPluginLoad(KCSE::PluginLoadFn loadFn, const KCSE::IKCSEInterface* kcse, DWORD* pExCode)
{
    *pExCode = 0;
    __try {
        return loadFn(kcse);
    } __except (*pExCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static HMODULE SafeLoadLibrary(const char* path, DWORD* pExCode)
{
    *pExCode = 0;
    __try {
        return LoadLibraryA(path);
    } __except (*pExCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// ---- Public API ----

namespace PluginManager {

PluginHandle LookupHandleFromName(const char* name)
{
    if (!name) return KCSE::kPluginHandle_Invalid;
    if (_stricmp(name, "KCSE") == 0) return KCSE::kPluginHandle_KCSE;
    for (auto& p : s_plugins)
        if (_stricmp(p.name.c_str(), name) == 0)
            return p.handle;
    return KCSE::kPluginHandle_Invalid;
}

const char* LookupNameFromHandle(PluginHandle handle)
{
    if (handle == KCSE::kPluginHandle_KCSE) return "KCSE";
    for (auto& p : s_plugins)
        if (p.handle == handle)
            return p.name.c_str();
    return nullptr;
}

const KCSE::PluginInfo* GetPluginInfo(const char* name)
{
    for (auto& p : s_plugins)
        if (_stricmp(p.name.c_str(), name) == 0 && p.loaded)
            return &p.info;
    return nullptr;
}

static void ScanDirectory(const fs::path& dir)
{
    spdlog::info("Scanning: {}", dir.string());
    if (!fs::exists(dir)) {
        spdlog::info("  Directory does not exist, skipping");
        return;
    }

    int fileCount = 0;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file())
            continue;
        if (_stricmp(entry.path().extension().string().c_str(), ".dll") != 0)
            continue;

        auto path = entry.path().string();
        auto filename = entry.path().filename().string();
        ++fileCount;
        spdlog::info("  Found DLL: {}", filename);

        HMODULE hTemp = LoadLibraryA(path.c_str());
        if (!hTemp) {
            spdlog::warn("  Failed to load {}: error {}", filename, GetLastError());
            continue;
        }

        auto* versionData = reinterpret_cast<KCSE::PluginVersionData*>(
            GetProcAddress(hTemp, "KCSEPlugin_Version"));

        if (!versionData) {
            spdlog::info("  {} has no KCSEPlugin_Version export, skipping", filename);
            FreeLibrary(hTemp);
            continue;
        }

        if (!CheckCompatibility(*versionData)) {
            FreeLibrary(hTemp);
            continue;
        }

        if (LookupHandleFromName(versionData->name) != KCSE::kPluginHandle_Invalid) {
            spdlog::warn("Skipping duplicate plugin: {} from {}", versionData->name, path);
            FreeLibrary(hTemp);
            continue;
        }

        LoadedPlugin plugin;
        plugin.dllPath = path;
        plugin.dllName = filename;
        plugin.version = *versionData;
        plugin.name = versionData->name;
        plugin.handle = s_nextHandle++;
        plugin.hModule = hTemp;

        spdlog::info("  Registered plugin: {} v{} by {}", plugin.name, plugin.version.pluginVersion, plugin.version.author);

        s_plugins.push_back(std::move(plugin));
        s_listeners.resize(s_nextHandle);
    }

    if (!fileCount)
        spdlog::info("  No DLLs found in directory");
}

// Steam Workshop mods load in place from each subscribed item's install folder, which the game asks ISteamUGC for
// (CryGamePlatformSteam). KCSE asks the same way, through the flat API of the steam_api64.dll the game has loaded;
// the signatures are those of the game's Steamworks SDK, whose UGC interface is version 018.
static void ScanWorkshop() {
    using GetHSteamUserFn = int32_t (*)();
    using SteamUGCFn = void *(*)();
    using GetNumSubscribedItemsFn = uint32_t (*)(void *ugc);
    using GetSubscribedItemsFn = uint32_t (*)(void *ugc, uint64_t *ids, uint32_t maxEntries);
    using GetItemStateFn = uint32_t (*)(void *ugc, uint64_t id);
    using GetItemInstallInfoFn =
        bool (*)(void *ugc, uint64_t id, uint64_t *sizeOnDisk, char *folder, uint32_t folderSize, uint32_t *timestamp);
    using ReleaseCurrentThreadMemoryFn = void (*)();

    HMODULE steamApi = GetModuleHandleW(L"steam_api64.dll");
    if (!steamApi) {
        spdlog::info("No Steam API loaded, skipping Workshop items");
        return;
    }

    auto getUser = reinterpret_cast<GetHSteamUserFn>(GetProcAddress(steamApi, "SteamAPI_GetHSteamUser"));
    auto getUgc = reinterpret_cast<SteamUGCFn>(GetProcAddress(steamApi, "SteamAPI_SteamUGC_v018"));
    auto getCount =
        reinterpret_cast<GetNumSubscribedItemsFn>(GetProcAddress(steamApi, "SteamAPI_ISteamUGC_GetNumSubscribedItems"));
    auto getItems =
        reinterpret_cast<GetSubscribedItemsFn>(GetProcAddress(steamApi, "SteamAPI_ISteamUGC_GetSubscribedItems"));
    auto getState = reinterpret_cast<GetItemStateFn>(GetProcAddress(steamApi, "SteamAPI_ISteamUGC_GetItemState"));
    auto getInstallInfo =
        reinterpret_cast<GetItemInstallInfoFn>(GetProcAddress(steamApi, "SteamAPI_ISteamUGC_GetItemInstallInfo"));
    auto releaseThreadMemory =
        reinterpret_cast<ReleaseCurrentThreadMemoryFn>(GetProcAddress(steamApi, "SteamAPI_ReleaseCurrentThreadMemory"));
    if (!getUser || !getUgc || !getCount || !getItems || !getState || !getInstallInfo || !releaseThreadMemory) {
        spdlog::warn("Steam API has no ISteamUGC v018 flat calls, skipping Workshop items");
        return;
    }

    // KCSE starts as soon as CCryAction exists, before the engine's GamePlatform plugin calls SteamAPI_Init. The user
    // handle is the last thing SteamAPI_Init sets, so once it is non-zero the UGC interface can be had.
    constexpr DWORD kSteamWaitMs = 10000;
    void *ugc = nullptr;
    for (DWORD waited = 0;; waited += 100) {
        if (getUser())
            ugc = getUgc();
        if (ugc)
            break;
        if (waited >= kSteamWaitMs) {
            spdlog::warn("Steam did not start within {} ms, skipping Workshop items", kSteamWaitMs);
            return;
        }
        Sleep(100);
    }

    constexpr uint32_t kItemStateInstalled = 4;
    std::vector<uint64_t> items(getCount(ugc));
    items.resize(getItems(ugc, items.data(), static_cast<uint32_t>(items.size())));
    spdlog::info("Searching {} subscribed Workshop items", items.size());
    for (auto id : items) {
        if (!(getState(ugc, id) & kItemStateInstalled)) {
            spdlog::info("  Workshop item {} is not installed, skipping", id);
            continue;
        }
        uint64_t sizeOnDisk = 0;
        uint32_t timestamp = 0;
        char folder[1024]{};
        if (!getInstallInfo(ugc, id, &sizeOnDisk, folder, sizeof(folder), &timestamp)) {
            spdlog::warn("  Workshop item {} has no install folder, skipping", id);
            continue;
        }
        spdlog::info("  Workshop item {}: {}", id, folder);
        // Steam returns UTF-8 paths, not the ANSI code page a plain fs::path(const char*) assumes.
        ScanDirectory(fs::u8path(folder) / "KCSE" / "Plugins");
    }

    // Steam keeps per-thread memory for calls made off the main thread until it is released.
    releaseThreadMemory();
}

bool Init()
{
    s_listeners.resize(1);

    auto gameRoot = fs::current_path();
    spdlog::info("Game root: {}", gameRoot.string());

    auto modsDir = gameRoot / "mods";
    if (fs::exists(modsDir)) {
        spdlog::info("Searching mods directory: {}", modsDir.string());
        int modCount = 0;
        for (auto& modDir : fs::directory_iterator(modsDir)) {
            if (!modDir.is_directory())
                continue;
            ++modCount;
            spdlog::info("  Mod folder: {}", modDir.path().filename().string());
            ScanDirectory(modDir.path() / "KCSE" / "Plugins");
        }
        if (!modCount)
            spdlog::info("  No mod folders found");
    } else {
        spdlog::info("No mods directory at {}", modsDir.string());
    }

    ScanWorkshop();

    ScanDirectory(gameRoot / "KCSE" / "Plugins");

    spdlog::info("Total plugins found: {}", s_plugins.size());
    return true;
}

void LoadAll()
{
    for (auto& plugin : s_plugins) {
        plugin.loadFn = reinterpret_cast<KCSE::PluginLoadFn>(
            GetProcAddress(plugin.hModule, "KCSEPlugin_Load"));

        if (!plugin.loadFn) {
            spdlog::error("{} has no KCSEPlugin_Load export", plugin.name);
            continue;
        }

        // Bound to plugin.handle at construction -- see Interfaces.h. The plugin
        // caches this pointer (and derived interfaces from it) at load time and may
        // call through it from callbacks fired long after load returns, so it must
        // outlive the load call; owned by the LoadedPlugin entry for that reason.
        plugin.kcseInterface = std::make_unique<KCSEInterfaceImpl>(plugin.handle);

        DWORD exCode = 0;
        plugin.loaded = SafeCallPluginLoad(plugin.loadFn, plugin.kcseInterface.get(), &exCode);

        if (exCode) {
            spdlog::error("{} crashed during load (exception 0x{:08X})", plugin.name, exCode);
            continue;
        }

        if (plugin.loaded) {
            plugin.info.infoVersion = 1;
            plugin.info.name = plugin.name.c_str();
            plugin.info.version = plugin.version.pluginVersion;
            spdlog::info("Loaded {}", plugin.name);
        } else {
            spdlog::error("{} returned false from KCSEPlugin_Load", plugin.name);
        }
    }

    // Bare-minimum ASI support: LoadLibrary every .asi next to dinput8.dll
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    auto asiDir = fs::path(exePath).parent_path();
    for (auto& entry : fs::directory_iterator(asiDir)) {
        if (!entry.is_regular_file())
            continue;
        if (_stricmp(entry.path().extension().string().c_str(), ".asi") != 0)
            continue;

        auto path = entry.path().string();
        auto filename = entry.path().filename().string();

        DWORD exCode = 0;
        HMODULE hAsi = SafeLoadLibrary(path.c_str(), &exCode);

        if (exCode)
            spdlog::error("{} crashed during load (exception 0x{:08X})", filename, exCode);
        else if (hAsi)
            spdlog::info("Loaded ASI: {}", filename);
        else
            spdlog::warn("Failed to load ASI {}: error {}", filename, GetLastError());
    }
}

void DispatchPostLoad()
{
    Dispatch(KCSE::kPluginHandle_KCSE, KCSE::IMessagingInterface::kMessage_AllPluginsLoaded, nullptr, 0, nullptr);
}

bool RegisterListener(PluginHandle listener, const char* sender, KCSE::IMessagingInterface::EventCallback handler)
{
    if (!handler)
        return false;

    std::lock_guard lock(s_messagingLock);

    auto addListener = [&](PluginHandle targetHandle) {
        if (targetHandle >= s_listeners.size())
            return;
        auto& list = s_listeners[targetHandle];
        for (auto& l : list)
            if (l.listener == listener)
                return;
        list.push_back({listener, handler});
    };

    if (sender) {
        auto handle = LookupHandleFromName(sender);
        if (handle == KCSE::kPluginHandle_Invalid)
            return false;
        addListener(handle);
    } else {
        addListener(KCSE::kPluginHandle_KCSE);
        for (auto& p : s_plugins)
            if (p.handle != listener)
                addListener(p.handle);
    }

    return true;
}

bool Dispatch(PluginHandle sender, uint32_t messageType, void* data, uint32_t dataLen, const char* receiver)
{
    std::vector<PluginListener> localList;
    {
        std::lock_guard lock(s_messagingLock);
        if (sender >= s_listeners.size())
            return false;
        localList = s_listeners[sender];
    }

    if (localList.empty())
        return false;

    const char* senderName = LookupNameFromHandle(sender);
    KCSE::Message msg{senderName, messageType, dataLen, data};
    bool dispatched = false;

    for (auto& l : localList) {
        if (receiver) {
            auto* name = LookupNameFromHandle(l.listener);
            if (!name || _stricmp(name, receiver) != 0)
                continue;
        }
        l.callback(&msg);
        dispatched = true;
    }

    return dispatched;
}

}  // namespace PluginManager
