#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <set>

// ============================================================
// Steamworks SDK — included for type definitions only.
// We resolve all function calls via GetProcAddress at runtime,
// so no linking against steam_api64.lib is required.
// ============================================================
#define STEAM_API_NODLL
#define VALVE_CALLBACK_PACK_LARGE
#include "steam/isteamuserstats.h"

namespace LeaderboardFinder {

// ============================================================
// Vtable indices (verified via IDA Pro decompilation)
//   FindOrCreateLeaderboard: index 21 (offset 168)
//   FindLeaderboard:         index 22 (offset 176)
// Interface version: STEAMUSERSTATS_INTERFACE_VERSION013
// ============================================================
constexpr int VTIDX_FIND_OR_CREATE = 21;
constexpr int VTIDX_FIND           = 22;

// ---- Runtime-resolved function types ----
using FindOrCreateInterface_fn = void* (__cdecl *)(HSteamUser, const char*);

// Vtable entry: FindOrCreateLeaderboard(this, name, sort, display) -> SteamAPICall_t
using FindOrCreateLeaderboard_fn = SteamAPICall_t (*)(void*, const char*, ELeaderboardSortMethod, ELeaderboardDisplayType);

// Vtable entry: FindLeaderboard(this, name) -> SteamAPICall_t
using FindLeaderboard_fn = SteamAPICall_t (*)(void*, const char*);

// ---- Globals ----
static FindOrCreateLeaderboard_fn g_origFindOrCreate = nullptr;
static FindLeaderboard_fn         g_origFind         = nullptr;
static FindOrCreateInterface_fn   g_origFindIface    = nullptr;
static CRITICAL_SECTION           g_cs;
static std::set<std::string>      g_seen;
static FILE*                      g_file             = nullptr;
static FILE*                      g_debugLog         = nullptr;
static char                       g_basePath[MAX_PATH] = {};
static bool                       g_vtableHooked     = false;

// ---- Helpers ----
static void InitBasePath(HMODULE hOurDll) {
    GetModuleFileNameA(hOurDll, g_basePath, MAX_PATH);
    char* lastSlash = strrchr(g_basePath, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
}

static std::string MakePath(const char* filename) {
    return std::string(g_basePath) + filename;
}

static void DebugLog(const char* fmt, ...) {
    if (!g_debugLog) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_debugLog, fmt, args);
    va_end(args);
    fflush(g_debugLog);
}

static const char* SortMethodStr(ELeaderboardSortMethod m) {
    switch (m) {
        case k_ELeaderboardSortMethodAscending:  return "Ascending";
        case k_ELeaderboardSortMethodDescending: return "Descending";
        default:                                 return "None";
    }
}

static const char* DisplayTypeStr(ELeaderboardDisplayType t) {
    switch (t) {
        case k_ELeaderboardDisplayTypeNumeric:          return "Numeric";
        case k_ELeaderboardDisplayTypeTimeSeconds:      return "TimeSeconds";
        case k_ELeaderboardDisplayTypeTimeMilliSeconds:  return "TimeMilliSeconds";
        default:                                        return "None";
    }
}

// ---- Shared logging helper ----
static void LogLeaderboard(const char* pchName, const char* sortStr, const char* displayStr) {
    if (!pchName) return;
    EnterCriticalSection(&g_cs);

    std::string name(pchName);
    if (g_seen.find(name) == g_seen.end()) {
        g_seen.insert(name);

        if (g_file) {
            fprintf(g_file, "%s | SortMethod=%s | DisplayType=%s\n",
                pchName, sortStr, displayStr);
            fflush(g_file);
        }

        DebugLog("[HOOK] Found: %s | Sort=%s | Display=%s\n",
            pchName, sortStr, displayStr);
    }

    LeaveCriticalSection(&g_cs);
}

// ---- Hook: FindOrCreateLeaderboard (index 21) ----
static SteamAPICall_t HookedFindOrCreateLeaderboard(
    void*                   thisptr,
    const char*             pchName,
    ELeaderboardSortMethod  sortMethod,
    ELeaderboardDisplayType displayType)
{
    LogLeaderboard(pchName, SortMethodStr(sortMethod), DisplayTypeStr(displayType));
    return g_origFindOrCreate(thisptr, pchName, sortMethod, displayType);
}

// ---- Hook: FindLeaderboard (index 22) ----
static SteamAPICall_t HookedFindLeaderboard(
    void*       thisptr,
    const char* pchName)
{
    LogLeaderboard(pchName, "N/A", "N/A");
    return g_origFind(thisptr, pchName);
}

// ---- Install vtable hooks on an ISteamUserStats instance ----
static bool InstallVTableHook(void* pInterface) {
    if (g_vtableHooked) return true;

    void** vtable = *reinterpret_cast<void***>(pInterface);

    DebugLog("[HOOK] VTable base at %p\n", vtable);
    DebugLog("[HOOK] vtable[%d] (FindOrCreate) = %p\n", VTIDX_FIND_OR_CREATE, vtable[VTIDX_FIND_OR_CREATE]);
    DebugLog("[HOOK] vtable[%d] (Find)         = %p\n", VTIDX_FIND, vtable[VTIDX_FIND]);

    // Save originals
    g_origFindOrCreate = reinterpret_cast<FindOrCreateLeaderboard_fn>(vtable[VTIDX_FIND_OR_CREATE]);
    g_origFind         = reinterpret_cast<FindLeaderboard_fn>(vtable[VTIDX_FIND]);

    if (!g_origFindOrCreate || !g_origFind) {
        DebugLog("[ERROR] vtable entries are null\n");
        return false;
    }

    // Make both entries writable (they're adjacent)
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[VTIDX_FIND_OR_CREATE], sizeof(void*) * 2, PAGE_READWRITE, &oldProtect)) {
        DebugLog("[ERROR] VirtualProtect failed (%lu)\n", GetLastError());
        return false;
    }

    vtable[VTIDX_FIND_OR_CREATE] = reinterpret_cast<void*>(&HookedFindOrCreateLeaderboard);
    vtable[VTIDX_FIND]           = reinterpret_cast<void*>(&HookedFindLeaderboard);

    VirtualProtect(&vtable[VTIDX_FIND_OR_CREATE], sizeof(void*) * 2, oldProtect, &oldProtect);

    g_vtableHooked = true;
    DebugLog("[HOOK] Both hooks installed! FindOrCreate[%d] + Find[%d]\n",
        VTIDX_FIND_OR_CREATE, VTIDX_FIND);
    return true;
}

// ---- Hooked SteamInternal_FindOrCreateUserInterface ----
// Intercepts interface creation; installs vtable hook the instant
// ISteamUserStats is created — before the game can use it.
static void* __cdecl HookedFindOrCreateUserInterface(HSteamUser hUser, const char* pszVersion) {
    void* result = g_origFindIface(hUser, pszVersion);

    DebugLog("[INTERCEPT] FindOrCreateUserInterface(\"%s\") -> %p\n",
        pszVersion ? pszVersion : "null", result);

    if (result && pszVersion && strcmp(pszVersion, STEAMUSERSTATS_INTERFACE_VERSION) == 0) {
        DebugLog("[INTERCEPT] ISteamUserStats detected — installing vtable hook\n");
        InstallVTableHook(result);
    }

    return result;
}

// ---- Inline hook (x64 JMP) on SteamInternal_FindOrCreateUserInterface ----
static bool PatchFindOrCreateUserInterface(HMODULE hSteamAPI) {
    auto fnTarget = reinterpret_cast<unsigned char*>(
        GetProcAddress(hSteamAPI, "SteamInternal_FindOrCreateUserInterface"));
    if (!fnTarget) {
        DebugLog("[ERROR] Cannot find SteamInternal_FindOrCreateUserInterface\n");
        return false;
    }

    DebugLog("[PATCH] Target function at %p\n", fnTarget);

    // x64 absolute JMP: FF 25 00 00 00 00 [8-byte addr] = 14 bytes
    constexpr int PATCH_SIZE = 14;

    // Allocate trampoline (original bytes + JMP back)
    auto trampoline = reinterpret_cast<unsigned char*>(
        VirtualAlloc(NULL, PATCH_SIZE + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        DebugLog("[ERROR] VirtualAlloc for trampoline failed\n");
        return false;
    }

    // Copy original bytes to trampoline
    memcpy(trampoline, fnTarget, PATCH_SIZE);

    // Append JMP back to original + PATCH_SIZE
    trampoline[PATCH_SIZE]     = 0xFF;
    trampoline[PATCH_SIZE + 1] = 0x25;
    *reinterpret_cast<uint32_t*>(&trampoline[PATCH_SIZE + 2]) = 0;
    *reinterpret_cast<uint64_t*>(&trampoline[PATCH_SIZE + 6]) =
        reinterpret_cast<uint64_t>(fnTarget + PATCH_SIZE);

    g_origFindIface = reinterpret_cast<FindOrCreateInterface_fn>(trampoline);

    // Patch original function → JMP to our hook
    DWORD oldProtect;
    VirtualProtect(fnTarget, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);

    fnTarget[0] = 0xFF;
    fnTarget[1] = 0x25;
    *reinterpret_cast<uint32_t*>(&fnTarget[2]) = 0;
    *reinterpret_cast<uint64_t*>(&fnTarget[6]) =
        reinterpret_cast<uint64_t>(&HookedFindOrCreateUserInterface);

    VirtualProtect(fnTarget, PATCH_SIZE, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), fnTarget, PATCH_SIZE);

    DebugLog("[PATCH] Inline hook installed, trampoline at %p\n", trampoline);
    return true;
}

// ============================================================
// EarlyInit — called from DllMain DLL_PROCESS_ATTACH
// Only sets up paths, files, and critical section.
// Cannot do hooking here because steam_api64.dll isn't loaded yet.
// ============================================================
static void EarlyInit(HMODULE hOurDll) {
    InitBasePath(hOurDll);
    InitializeCriticalSection(&g_cs);

    g_debugLog = fopen(MakePath("leaderboard_finder.log").c_str(), "w");
    DebugLog("[INIT] LeaderboardFinder EarlyInit\n");
    DebugLog("[INIT] Base path: %s\n", g_basePath);

    g_file = fopen(MakePath("leaderboards.txt").c_str(), "w");
    if (!g_file) {
        DebugLog("[ERROR] Cannot create leaderboards.txt\n");
    }
}

// ============================================================
// Run — called from background Payload thread
// Waits for steam_api64.dll to load, then immediately patches
// SteamInternal_FindOrCreateUserInterface BEFORE the game calls
// SteamAPI_Init, ensuring we intercept ISteamUserStats creation.
// ============================================================
static void Run() {
    if (!g_debugLog) return; // EarlyInit failed

    // Step 1: Wait for steam_api64.dll to be loaded by the game
    HMODULE hSteamAPI = nullptr;
    DebugLog("[WAIT] Looking for steam_api64.dll...\n");
    for (int i = 0; i < 600; i++) { // 60s timeout
        hSteamAPI = GetModuleHandleA("steam_api64.dll");
        if (hSteamAPI) break;
        Sleep(10); // Poll fast — we need to beat SteamAPI_Init
    }
    if (!hSteamAPI) {
        DebugLog("[ERROR] steam_api64.dll not found (timeout)\n");
        return;
    }
    DebugLog("[FOUND] steam_api64.dll at %p\n", hSteamAPI);

    // Step 2: Patch SteamInternal_FindOrCreateUserInterface immediately
    // This must happen BEFORE the game calls SteamAPI_Init
    if (!PatchFindOrCreateUserInterface(hSteamAPI)) {
        DebugLog("[ERROR] Failed to patch\n");
        return;
    }
    DebugLog("[READY] Intercept armed — waiting for ISteamUserStats creation\n");

    // Step 3: Wait and verify the hook gets installed
    for (int i = 0; i < 600 && !g_vtableHooked; i++) {
        Sleep(100);
    }

    if (g_vtableHooked) {
        DebugLog("[DONE] VTable hook active — monitoring FindOrCreateLeaderboard\n");
    } else {
        // Fallback: try to get the interface directly
        DebugLog("[WARN] Intercept didn't fire for ISteamUserStats, trying direct access\n");
        auto fnGetUser = reinterpret_cast<HSteamUser (__cdecl *)()>(
            GetProcAddress(hSteamAPI, "SteamAPI_GetHSteamUser"));
        if (fnGetUser && g_origFindIface) {
            HSteamUser hUser = fnGetUser();
            if (hUser != 0) {
                void* pStats = g_origFindIface(hUser, STEAMUSERSTATS_INTERFACE_VERSION);
                if (pStats) {
                    DebugLog("[FALLBACK] Got ISteamUserStats at %p\n", pStats);
                    InstallVTableHook(pStats);
                }
            }
        }
    }
}

} // namespace LeaderboardFinder

