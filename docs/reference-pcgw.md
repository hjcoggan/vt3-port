# Reference: PCGamingWiki, Virtua Tennis 3

Source: https://www.pcgamingwiki.com/wiki/Virtua_Tennis_3 (read 2026-10-14).

Facts relevant to the port:
- Engine: Sunshine (Sumo Digital's in-house engine, also used for OutRun conversions and Sega Superstars Tennis).
- Middleware: CRIWARE for audio and cutscenes. This matches the CRI ADX, Sofdec and ACB formats we found.
- Graphics: Direct3D 9.0c, Shader Model 2.0 minimum. The game is 32-bit. The wiki notes the LAA flag may be needed on modern machines.
- Configuration: `VT3.ini` in the game folder. `Config.exe` edits the settings, and the executable also reads `default.ini`.
- Saves: stored in the game folder.
- Last patch: 1.01. The NoCD 1.0.1 build matches it.
- DRM: SafeDisc 4. It doesn't work on Windows Vista and later, so the NoCD build is the practical target.
- Game speed: the wiki says the slow-speed fix on modern Windows is to enable DirectPlay. That suggests game speed depends on a timing or system detail we need to get right in the runtime.
- Multiplayer: local play for up to 4 players.
- No skip-intro setting is documented on the wiki. The debug menu's "Tennis (Skip Intro)" entry is the route being investigated.
