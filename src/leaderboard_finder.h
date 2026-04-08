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
// FindOrCreateLeaderboard vtable index = 21 (byte offset 168)
// Verified via IDA Pro decompilation of steam_api64.dll
// Interface version: STEAMUSERSTATS_INTERFACE_VERSION013
// ============================================================
constexpr int VTABLE_INDEX = 21;

// ---- Runtime-resolved function types ----
using FindOrCreateInterface_fn = void* (__cdecl *)(HSteamUser, const char*);

// Vtable entry type for FindOrCreateLeaderboard
using FindOrCreateLeaderboard_fn = SteamAPICall_t (*)(
    void*, const char*, ELeaderboardSortMethod, ELeaderboardDisplayType
);

// ---- Globals ----
static FindOrCreateLeaderboard_fn g_original       = nullptr;
static FindOrCreateInterface_fn   g_origFindIface  = nullptr;
static CRITICAL_SECTION           g_cs;
static std::set<std::string>      g_seen;
static FILE*                      g_file           = nullptr;
static FILE*                      g_debugLog       = nullptr;
static char                       g_basePath[MAX_PATH] = {};
static bool                       g_vtableHooked   = false;

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

// ---- VTable Hook on FindOrCreateLeaderboard ----
static SteamAPICall_t HookedFindOrCreateLeaderboard(
    void*                   thisptr,
    const char*             pchName,
    ELeaderboardSortMethod  sortMethod,
    ELeaderboardDisplayType displayType)
{
    if (pchName) {
        EnterCriticalSection(&g_cs);

        std::string name(pchName);
        if (g_seen.find(name) == g_seen.end()) {
            g_seen.insert(name);

            if (g_file) {
                fprintf(g_file, "%s | SortMethod=%s | DisplayType=%s\n",
                    pchName, SortMethodStr(sortMethod), DisplayTypeStr(displayType));
                fflush(g_file);
            }

            DebugLog("[HOOK] Found: %s | Sort=%s | Display=%s\n",
                pchName, SortMethodStr(sortMethod), DisplayTypeStr(displayType));
        }

        LeaveCriticalSection(&g_cs);
    }

    return g_original(thisptr, pchName, sortMethod, displayType);
}

// ---- Install vtable hook on an ISteamUserStats instance ----
static bool InstallVTableHook(void* pInterface) {
    if (g_vtableHooked) return true;

    void** vtable = *reinterpret_cast<void***>(pInterface);

    DebugLog("[HOOK] VTable base at %p\n", vtable);
    DebugLog("[HOOK] vtable[%d] = %p\n", VTABLE_INDEX, vtable[VTABLE_INDEX]);

    g_original = reinterpret_cast<FindOrCreateLeaderboard_fn>(vtable[VTABLE_INDEX]);
    if (!g_original) {
        DebugLog("[ERROR] vtable[%d] is null\n", VTABLE_INDEX);
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(&vtable[VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        DebugLog("[ERROR] VirtualProtect failed (%lu)\n", GetLastError());
        return false;
    }

    vtable[VTABLE_INDEX] = reinterpret_cast<void*>(&HookedFindOrCreateLeaderboard);
    VirtualProtect(&vtable[VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);

    g_vtableHooked = true;
    DebugLog("[HOOK] Installed! vtable[%d] swapped: %p -> %p\n",
        VTABLE_INDEX, g_original, &HookedFindOrCreateLeaderboard);
    return true;
}

// ---- Hooked SteamInternal_FindOrCreateUserInterface ----
// Intercepts interface creation; installs vtable hook the instant
// ISteamUserStats is created — before the game can use it.
static void* __cdecl HookedFindOrCreateUserInterface(HSteamUser hUser, const char* pszVersion) {
    void* result = g_origFindIface(hUser, pszVersion);

    DebugLog("[INTERCEPT] FindOrCreateUserInterface(\"%s\") -> %p\n",
        pszVersion ? pszVersion : "null", result);

    if (result && pszVersion && strstr(pszVersion, "SteamUserStats")) {
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
// EarlyInit — called SYNCHRONOUSLY from DllMain DLL_PROCESS_ATTACH
// This runs on the main thread BEFORE the game's WinMain,
// so we can hook SteamInternal_FindOrCreateUserInterface
// before SteamAPI_Init is ever called.
// ============================================================
static void EarlyInit(HMODULE hOurDll) {
    InitBasePath(hOurDll);
    InitializeCriticalSection(&g_cs);

    g_debugLog = fopen(MakePath("leaderboard_finder.log").c_str(), "w");
    DebugLog("[INIT] LeaderboardFinder EarlyInit (synchronous, from DllMain)\n");
    DebugLog("[INIT] Base path: %s\n", g_basePath);

    g_file = fopen(MakePath("leaderboards.txt").c_str(), "w");
    if (!g_file) {
        DebugLog("[ERROR] Cannot create leaderboards.txt\n");
        return;
    }

    // steam_api64.dll should already be loaded (game links against it)
    HMODULE hSteamAPI = GetModuleHandleA("steam_api64.dll");
    if (!hSteamAPI) {
        DebugLog("[WARN] steam_api64.dll not loaded yet, will try LoadLibrary\n");
        // Try to load it — the game directory should have it or it's in the search path
        hSteamAPI = LoadLibraryA("steam_api64.dll");
    }
    if (!hSteamAPI) {
        DebugLog("[ERROR] Cannot find steam_api64.dll\n");
        return;
    }
    DebugLog("[FOUND] steam_api64.dll at %p\n", hSteamAPI);

    // Hook SteamInternal_FindOrCreateUserInterface BEFORE the game calls SteamAPI_Init
    if (!PatchFindOrCreateUserInterface(hSteamAPI)) {
        DebugLog("[ERROR] Failed to patch SteamInternal_FindOrCreateUserInterface\n");
        return;
    }

    DebugLog("[READY] Intercept armed — will hook ISteamUserStats on first access\n");
}

// ============================================================
// Run — called from background Payload thread
// Only used as a safety net: if EarlyInit's intercept didn't
// fire (edge case), poll and install the vtable hook.
// ============================================================
static void Run() {
    // Give the game time to start
    Sleep(5000);

    if (g_vtableHooked) {
        DebugLog("[THREAD] VTable hook already active, nothing to do\n");
        return;
    }

    DebugLog("[THREAD] VTable hook not installed yet, trying fallback polling\n");

    HMODULE hSteamAPI = GetModuleHandleA("steam_api64.dll");
    if (!hSteamAPI) return;

    auto fnGetUser = reinterpret_cast<HSteamUser (__cdecl *)()>(
        GetProcAddress(hSteamAPI, "SteamAPI_GetHSteamUser"));
    if (!fnGetUser) return;

    for (int i = 0; i < 300 && !g_vtableHooked; i++) {
        HSteamUser hUser = fnGetUser();
        if (hUser != 0 && g_origFindIface) {
            void* pStats = g_origFindIface(hUser, STEAMUSERSTATS_INTERFACE_VERSION);
            if (pStats) {
                DebugLog("[THREAD] Fallback: got ISteamUserStats at %p\n", pStats);
                InstallVTableHook(pStats);
                break;
            }
        }
        Sleep(100);
    }
}

} // namespace LeaderboardFinder
