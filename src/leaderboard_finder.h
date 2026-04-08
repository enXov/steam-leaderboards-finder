#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <set>

// Steamworks SDK — type definitions only (no linking required)
#define STEAM_API_NODLL
#define VALVE_CALLBACK_PACK_LARGE
#include "steam/isteamuserstats.h"

namespace LeaderboardFinder {

// Vtable indices (verified via IDA Pro)
//   FindOrCreateLeaderboard: index 21 (offset 168)
//   FindLeaderboard:         index 22 (offset 176)
constexpr int VTIDX_FIND_OR_CREATE = 21;
constexpr int VTIDX_FIND           = 22;

// Runtime-resolved function types
using FindOrCreateInterface_fn     = void* (__cdecl *)(HSteamUser, const char*);
using FindOrCreateLeaderboard_fn   = SteamAPICall_t (*)(void*, const char*, ELeaderboardSortMethod, ELeaderboardDisplayType);
using FindLeaderboard_fn           = SteamAPICall_t (*)(void*, const char*);

// Globals
static FindOrCreateLeaderboard_fn g_origFindOrCreate = nullptr;
static FindLeaderboard_fn         g_origFind         = nullptr;
static FindOrCreateInterface_fn   g_origFindIface    = nullptr;
static CRITICAL_SECTION           g_cs;
static std::set<std::string>      g_seen;
static FILE*                      g_file             = nullptr;
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

// ---- Logging ----

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
    }

    LeaveCriticalSection(&g_cs);
}

// ---- VTable Hooks ----

static SteamAPICall_t HookedFindOrCreateLeaderboard(
    void* thisptr, const char* pchName,
    ELeaderboardSortMethod sortMethod, ELeaderboardDisplayType displayType)
{
    LogLeaderboard(pchName, SortMethodStr(sortMethod), DisplayTypeStr(displayType));
    return g_origFindOrCreate(thisptr, pchName, sortMethod, displayType);
}

static SteamAPICall_t HookedFindLeaderboard(void* thisptr, const char* pchName) {
    LogLeaderboard(pchName, "N/A", "N/A");
    return g_origFind(thisptr, pchName);
}

static bool InstallVTableHook(void* pInterface) {
    if (g_vtableHooked) return true;

    void** vtable = *reinterpret_cast<void***>(pInterface);

    g_origFindOrCreate = reinterpret_cast<FindOrCreateLeaderboard_fn>(vtable[VTIDX_FIND_OR_CREATE]);
    g_origFind         = reinterpret_cast<FindLeaderboard_fn>(vtable[VTIDX_FIND]);

    if (!g_origFindOrCreate || !g_origFind) return false;

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[VTIDX_FIND_OR_CREATE], sizeof(void*) * 2, PAGE_READWRITE, &oldProtect))
        return false;

    vtable[VTIDX_FIND_OR_CREATE] = reinterpret_cast<void*>(&HookedFindOrCreateLeaderboard);
    vtable[VTIDX_FIND]           = reinterpret_cast<void*>(&HookedFindLeaderboard);

    VirtualProtect(&vtable[VTIDX_FIND_OR_CREATE], sizeof(void*) * 2, oldProtect, &oldProtect);

    g_vtableHooked = true;
    return true;
}

// ---- Inline Hook on SteamInternal_FindOrCreateUserInterface ----

static void* __cdecl HookedFindOrCreateUserInterface(HSteamUser hUser, const char* pszVersion) {
    void* result = g_origFindIface(hUser, pszVersion);
    if (result && pszVersion && strcmp(pszVersion, STEAMUSERSTATS_INTERFACE_VERSION) == 0)
        InstallVTableHook(result);
    return result;
}

static bool PatchFindOrCreateUserInterface(HMODULE hSteamAPI) {
    auto fnTarget = reinterpret_cast<unsigned char*>(
        GetProcAddress(hSteamAPI, "SteamInternal_FindOrCreateUserInterface"));
    if (!fnTarget) return false;

    constexpr int PATCH_SIZE = 14; // x64 absolute JMP: FF 25 00 00 00 00 [8-byte addr]

    auto trampoline = reinterpret_cast<unsigned char*>(
        VirtualAlloc(NULL, PATCH_SIZE + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;

    memcpy(trampoline, fnTarget, PATCH_SIZE);
    trampoline[PATCH_SIZE]     = 0xFF;
    trampoline[PATCH_SIZE + 1] = 0x25;
    *reinterpret_cast<uint32_t*>(&trampoline[PATCH_SIZE + 2]) = 0;
    *reinterpret_cast<uint64_t*>(&trampoline[PATCH_SIZE + 6]) =
        reinterpret_cast<uint64_t>(fnTarget + PATCH_SIZE);

    g_origFindIface = reinterpret_cast<FindOrCreateInterface_fn>(trampoline);

    DWORD oldProtect;
    VirtualProtect(fnTarget, PATCH_SIZE, PAGE_EXECUTE_READWRITE, &oldProtect);
    fnTarget[0] = 0xFF;
    fnTarget[1] = 0x25;
    *reinterpret_cast<uint32_t*>(&fnTarget[2]) = 0;
    *reinterpret_cast<uint64_t*>(&fnTarget[6]) =
        reinterpret_cast<uint64_t>(&HookedFindOrCreateUserInterface);
    VirtualProtect(fnTarget, PATCH_SIZE, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), fnTarget, PATCH_SIZE);

    return true;
}

// ---- Public API ----

static void EarlyInit(HMODULE hOurDll) {
    InitBasePath(hOurDll);
    InitializeCriticalSection(&g_cs);
    g_file = fopen(MakePath("leaderboards.txt").c_str(), "w");
}

static void Run() {
    if (!g_file) return;

    // Wait for steam_api64.dll to load
    HMODULE hSteamAPI = nullptr;
    for (int i = 0; i < 600; i++) {
        hSteamAPI = GetModuleHandleA("steam_api64.dll");
        if (hSteamAPI) break;
        Sleep(10);
    }
    if (!hSteamAPI) return;

    // Patch SteamInternal_FindOrCreateUserInterface before SteamAPI_Init
    if (!PatchFindOrCreateUserInterface(hSteamAPI)) return;

    // Wait for vtable hook to be installed via intercept
    for (int i = 0; i < 600 && !g_vtableHooked; i++)
        Sleep(100);

    // Fallback: try direct access
    if (!g_vtableHooked) {
        auto fnGetUser = reinterpret_cast<HSteamUser (__cdecl *)()>(
            GetProcAddress(hSteamAPI, "SteamAPI_GetHSteamUser"));
        if (fnGetUser && g_origFindIface) {
            HSteamUser hUser = fnGetUser();
            if (hUser != 0) {
                void* pStats = g_origFindIface(hUser, STEAMUSERSTATS_INTERFACE_VERSION);
                if (pStats) InstallVTableHook(pStats);
            }
        }
    }
}

} // namespace LeaderboardFinder
