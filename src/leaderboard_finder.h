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
//
// Verified via IDA Pro decompilation of steam_api64.dll:
//   GetNumAchievements          → vtable +104  (index 13) ✓
//   GetUserAchievementAndUnlock → vtable +152  (index 19) ✓
//   DownloadLeaderboardEntries  → vtable +216  (index 27) ✓
//   GetDownloadedLeaderboard    → vtable +232  (index 29) ✓
//   UploadLeaderboardScore      → vtable +240  (index 30) ✓
//
// Interface version: STEAMUSERSTATS_INTERFACE_VERSION013
// ============================================================
constexpr int VTABLE_INDEX = 21;

// ---- Runtime-resolved function types ----
using GetHSteamUser_fn         = HSteamUser (__cdecl *)();
using FindOrCreateInterface_fn = void* (__cdecl *)(HSteamUser, const char*);

// Vtable entry type for FindOrCreateLeaderboard.
// On x64 Windows there is only one calling convention;
// 'this' is passed as the first argument in rcx.
using FindOrCreateLeaderboard_fn = SteamAPICall_t (*)(
    void*                    /* this */,
    const char*              /* pchLeaderboardName */,
    ELeaderboardSortMethod   /* eLeaderboardSortMethod */,
    ELeaderboardDisplayType  /* eLeaderboardDisplayType */
);

// ---- Globals ----
static FindOrCreateLeaderboard_fn g_original    = nullptr;
static CRITICAL_SECTION           g_cs;
static std::set<std::string>      g_seen;
static FILE*                      g_file        = nullptr;

// ---- Helpers ----
static void DebugLog(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
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

// ---- Hook Function ----
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
                    pchName,
                    SortMethodStr(sortMethod),
                    DisplayTypeStr(displayType));
                fflush(g_file);
            }

            DebugLog("[LeaderboardFinder] Found: %s | Sort=%s | Display=%s\n",
                pchName, SortMethodStr(sortMethod), DisplayTypeStr(displayType));
        }

        LeaveCriticalSection(&g_cs);
    }

    // Forward to the original function
    return g_original(thisptr, pchName, sortMethod, displayType);
}

// ---- VTable Hook Installation ----
static bool InstallVTableHook(void* pInterface) {
    void** vtable = *reinterpret_cast<void***>(pInterface);

    // Save the original function pointer
    g_original = reinterpret_cast<FindOrCreateLeaderboard_fn>(vtable[VTABLE_INDEX]);
    if (!g_original) {
        DebugLog("[LeaderboardFinder] ERROR: vtable[%d] is null\n", VTABLE_INDEX);
        return false;
    }

    // Make the vtable entry writable
    DWORD oldProtect;
    if (!VirtualProtect(&vtable[VTABLE_INDEX], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        DebugLog("[LeaderboardFinder] ERROR: VirtualProtect failed (%lu)\n", GetLastError());
        return false;
    }

    // Swap the pointer
    vtable[VTABLE_INDEX] = reinterpret_cast<void*>(&HookedFindOrCreateLeaderboard);

    // Restore original protection
    VirtualProtect(&vtable[VTABLE_INDEX], sizeof(void*), oldProtect, &oldProtect);

    DebugLog("[LeaderboardFinder] Hook installed at vtable[%d] (offset %d)\n",
        VTABLE_INDEX, VTABLE_INDEX * 8);
    return true;
}

// ---- Main Entry Point (called from Payload thread) ----
static void Run() {
    DebugLog("[LeaderboardFinder] Starting...\n");
    InitializeCriticalSection(&g_cs);

    // Open output file
    g_file = fopen("leaderboards.txt", "w");
    if (!g_file) {
        DebugLog("[LeaderboardFinder] ERROR: Cannot create leaderboards.txt\n");
        return;
    }

    // Step 1: Wait for steam_api64.dll (loaded by the game)
    HMODULE hSteamAPI = nullptr;
    for (int i = 0; i < 600; i++) { // 60s timeout
        hSteamAPI = GetModuleHandleA("steam_api64.dll");
        if (hSteamAPI) break;
        Sleep(100);
    }
    if (!hSteamAPI) {
        DebugLog("[LeaderboardFinder] ERROR: steam_api64.dll not found (timeout)\n");
        fclose(g_file);
        return;
    }
    DebugLog("[LeaderboardFinder] steam_api64.dll at %p\n", hSteamAPI);

    // Step 2: Resolve Steam API functions via GetProcAddress
    auto fnGetUser = reinterpret_cast<GetHSteamUser_fn>(
        GetProcAddress(hSteamAPI, "SteamAPI_GetHSteamUser"));
    auto fnFindInterface = reinterpret_cast<FindOrCreateInterface_fn>(
        GetProcAddress(hSteamAPI, "SteamInternal_FindOrCreateUserInterface"));

    if (!fnGetUser || !fnFindInterface) {
        DebugLog("[LeaderboardFinder] ERROR: Cannot resolve Steam API exports\n");
        fclose(g_file);
        return;
    }

    // Step 3: Wait for SteamAPI_Init to complete
    // We poll SteamAPI_GetHSteamUser() — returns 0 before init
    void* pUserStats = nullptr;
    for (int i = 0; i < 600; i++) { // 60s timeout
        HSteamUser hUser = fnGetUser();
        if (hUser != 0) {
            pUserStats = fnFindInterface(hUser, STEAMUSERSTATS_INTERFACE_VERSION);
            if (pUserStats) break;
        }
        Sleep(100);
    }
    if (!pUserStats) {
        DebugLog("[LeaderboardFinder] ERROR: ISteamUserStats not available (timeout)\n");
        fclose(g_file);
        return;
    }
    DebugLog("[LeaderboardFinder] ISteamUserStats at %p\n", pUserStats);

    // Step 4: Install vtable hook
    if (!InstallVTableHook(pUserStats)) {
        fclose(g_file);
        return;
    }

    DebugLog("[LeaderboardFinder] Ready - monitoring FindOrCreateLeaderboard calls\n");
    // Hook is installed via vtable pointer swap — persists without this thread.
    // Log file remains open for the process lifetime.
}

} // namespace LeaderboardFinder
