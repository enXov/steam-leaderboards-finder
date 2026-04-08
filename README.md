# Steam Leaderboards Finder

Automatically discovers Steam leaderboard names used by any game by hooking the `ISteamUserStats` interface at runtime.

## Why This Project Exists

To use the Steam API's `UploadLeaderboardScore` function, you need a leaderboard handle — which you get by calling `FindLeaderboard` with the **exact leaderboard name**. But Steam doesn't provide a way to list all leaderboards for a game, and the names are only known to the game developer.

This tool solves that by intercepting the game's own calls to `FindLeaderboard` and logging the names it uses. Once you have the names, you can use them in your own project to upload scores, download entries, or interact with the leaderboards programmatically.

👉 **See [steam-leaderboards](https://github.com/enXov/steam-leaderboards) for actually uploading scores and interacting with leaderboards.**

## How It Works

1. Gets loaded by the game at startup via a commonly loaded Windows DLL
2. Patches `SteamInternal_FindOrCreateUserInterface` to intercept `ISteamUserStats` creation
3. Swaps vtable entries for `FindLeaderboard` (index 22) and `FindOrCreateLeaderboard` (index 21) with hooks
4. Writes discovered leaderboard names to `leaderboards.txt`

## Output Format

```
Normal Leaderboard | SortMethod=N/A | DisplayType=N/A
Hard Leaderboard | SortMethod=Ascending | DisplayType=Numeric
```

## Building

Requires MSVC (x64). Build via GitHub Actions:

1. Go to **Actions** → **Build DLL Proxy**
2. Select `winmm` as DLL type and `runtime` as proxy method
3. Download the artifact

Or build locally:

```bash
cmake -B build -DDLL_TYPE=winmm -DPROXY_METHOD=runtime
cmake --build build --config Release
```

## Usage

1. Place the built DLL in the game's executable directory (next to the `.exe`)
2. Add the following to Steam launch options:
   ```
   WINEDLLOVERRIDES="winmm=n,b" %command%
   ```
   *(On native Windows, no launch options are needed — the DLL loads automatically)*
3. Launch the game normally
4. Play through the game (navigate to leaderboard screens, different modes, etc.)
5. Check `leaderboards.txt` next to the game executable for discovered leaderboard names

## Why This Approach?

`FindLeaderboard` is a C++ virtual method — it doesn't have a fixed address you can hook directly. It only exists as a pointer inside the `ISteamUserStats` vtable. To hook it, you first need the `ISteamUserStats*` pointer so you can swap its vtable entry.

The naive approach is to poll for the interface from a background thread. But this creates a **race condition**: the game creates `ISteamUserStats` and calls `FindLeaderboard` on the main thread so fast that a background thread can't install the hook in time.

The solution is to hook `SteamInternal_FindOrCreateUserInterface` — the function that *creates* `ISteamUserStats`. This way, the moment the game requests the interface, our hook fires first, swaps the vtable, and returns the modified interface to the game. By the time the game calls `FindLeaderboard`, our hook is already in place.

```
Game:  SteamAPI_Init() → SteamInternal_FindOrCreateUserInterface("STEAMUSERSTATS...")
                              ↓
                         OUR HOOK → get ISteamUserStats* → swap vtable[22] → return to game
                              ↓
Game:  FindLeaderboard("Normal Leaderboard") → hits our hook ✅ → logged to file
```

## Technical Details

- **Interface**: `STEAMUSERSTATS_INTERFACE_VERSION013`
- **VTable Indices**: `FindOrCreateLeaderboard` at 21 (offset 168), `FindLeaderboard` at 22 (offset 176)
- **Hooking**: Inline hook (x64 absolute JMP) on `SteamInternal_FindOrCreateUserInterface` + vtable pointer swap
- **SDK**: Only public headers included for type definitions — no library linking

## Credits

This project was built using **[dll-proxy](https://github.com/enXov/dll-proxy)**. This README intentionally does not go into detail about how the DLL injection mechanism works — for a full explanation of the DLL proxy concept, how export forwarding works, and how to use it for your own projects, refer to that repository.

## License

MIT — see [LICENSE](LICENSE) for details.
