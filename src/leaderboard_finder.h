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
using GetHSteamUser_fn         = HSteamUser (__cdecl *)();
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
static bool                       g_hookInstalled  = false;

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
    if (g_hookInstalled) return true;

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

    g_hookInstalled = true;
    DebugLog("[HOOK] Installed! vtable[%d] swapped: %p -> %p\n",
        VTABLE_INDEX, g_original, &HookedFindOrCreateLeaderboard);
    return true;
}

// ---- IAT/Trampoline hook on SteamInternal_FindOrCreateUserInterface ----
// This intercepts the moment the game requests ISteamUserStats,
// letting us install our vtable hook BEFORE the game uses the interface.
static void* __cdecl HookedFindOrCreateUserInterface(HSteamUser hUser, const char* pszVersion) {
    void* result = g_origFindIface(hUser, pszVersion);

    DebugLog("[INTERCEPT] FindOrCreateUserInterface(\"%s\") -> %p\n", pszVersion ? pszVersion : "null", result);

    // When the game requests ISteamUserStats, install our vtable hook immediately
    if (result && pszVersion && strstr(pszVersion, "SteamUserStats")) {
        DebugLog("[INTERCEPT] ISteamUserStats detected — installing vtable hook now\n");
        InstallVTableHook(result);
    }

    return result;
}

// ---- Patch the import/call to SteamInternal_FindOrCreateUserInterface ----
// We overwrite the function's first bytes with a JMP to our hook (inline hook)
// This is necessary because the game calls it very early, before our polling could catch it.
static bool HookFindOrCreateUserInterface(HMODULE hSteamAPI) {
    auto fnTarget = reinterpret_cast<unsigned char*>(
        GetProcAddress(hSteamAPI, "SteamInternal_FindOrCreateUserInterface"));
    if (!fnTarget) {
        DebugLog("[ERROR] Cannot find SteamInternal_FindOrCreateUserInterface\n");
        return false;
    }

    DebugLog("[PATCH] SteamInternal_FindOrCreateUserInterface at %p\n", fnTarget);

    // Allocate a trampoline: save original bytes + JMP back
    // x64 absolute JMP: FF 25 00 00 00 00 [8-byte address] = 14 bytes
    constexpr int PATCH_SIZE = 14;

    // Allocate executable memory for trampoline
    unsigned char* trampoline = reinterpret_cast<unsigned char*>(
        VirtualAlloc(NULL, PATCH_SIZE + 14, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        DebugLog("[ERROR] VirtualAlloc for trampoline failed\n");
        return false;
    }

    // Copy original bytes to trampoline
    memcpy(trampoline, fnTarget, PATCH_SIZE);

    // Append JMP back to original function + PATCH_SIZE
    trampoline[PATCH_SIZE]     = 0xFF;
    trampoline[PATCH_SIZE + 1] = 0x25;
    *reinterpret_cast<uint32_t*>(&trampoline[PATCH_SIZE + 2]) = 0; // RIP-relative 0
    *reinterpret_cast<uint64_t*>(&trampoline[PATCH_SIZE + 6]) =
        reinterpret_cast<uint64_t>(fnTarget + PATCH_SIZE);

    // Save trampoline as original function
    g_origFindIface = reinterpret_cast<FindOrCreateInterface_fn>(trampoline);

    // Now patch the original function to JMP to our hook
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

// ---- Main Entry Point (called from Payload thread) ----
static void Run(HMODULE hOurDll) {
    InitBasePath(hOurDll);
    InitializeCriticalSection(&g_cs);

    g_debugLog = fopen(MakePath("leaderboard_finder.log").c_str(), "w");
    DebugLog("[INIT] LeaderboardFinder starting\n");
    DebugLog("[INIT] Base path: %s\n", g_basePath);

    g_file = fopen(MakePath("leaderboards.txt").c_str(), "w");
    if (!g_file) {
        DebugLog("[ERROR] Cannot create leaderboards.txt\n");
        return;
    }
    DebugLog("[INIT] leaderboards.txt opened\n");

    // Step 1: Wait for steam_api64.dll
    HMODULE hSteamAPI = nullptr;
    DebugLog("[WAIT] Looking for steam_api64.dll...\n");
    for (int i = 0; i < 600; i++) {
        hSteamAPI = GetModuleHandleA("steam_api64.dll");
        if (hSteamAPI) break;
        Sleep(100);
    }
    if (!hSteamAPI) {
        DebugLog("[ERROR] steam_api64.dll not found (timeout)\n");
        fclose(g_file);
        return;
    }
    DebugLog("[FOUND] steam_api64.dll at %p\n", hSteamAPI);

    // Step 2: Hook SteamInternal_FindOrCreateUserInterface
    // This fires BEFORE the game gets ISteamUserStats, so we catch the very first call
    if (!HookFindOrCreateUserInterface(hSteamAPI)) {
        DebugLog("[ERROR] Failed to hook SteamInternal_FindOrCreateUserInterface\n");
        fclose(g_file);
        return;
    }

    DebugLog("[READY] Waiting for game to request ISteamUserStats...\n");

    // Step 3: Also try polling as a fallback (in case the interface was already created)
    auto fnGetUser = reinterpret_cast<GetHSteamUser_fn>(
        GetProcAddress(hSteamAPI, "SteamAPI_GetHSteamUser"));
    auto fnFindInterface = reinterpret_cast<FindOrCreateInterface_fn>(
        GetProcAddress(hSteamAPI, "SteamInternal_FindOrCreateUserInterface"));

    if (fnGetUser) {
        for (int i = 0; i < 300 && !g_hookInstalled; i++) {
            HSteamUser hUser = fnGetUser();
            if (hUser != 0) {
                // Use the ORIGINAL function (trampoline), not the hooked one
                void* pStats = g_origFindIface(hUser, STEAMUSERSTATS_INTERFACE_VERSION);
                if (pStats) {
                    DebugLog("[FALLBACK] Got ISteamUserStats at %p via polling\n", pStats);
                    InstallVTableHook(pStats);
                    break;
                }
            }
            Sleep(100);
        }
    }

    if (g_hookInstalled) {
        DebugLog("[READY] Hook active — monitoring FindOrCreateLeaderboard calls\n");
    } else {
        DebugLog("[WARN] Hook was NOT installed — ISteamUserStats never became available\n");
    }
}

} // namespace LeaderboardFinder
