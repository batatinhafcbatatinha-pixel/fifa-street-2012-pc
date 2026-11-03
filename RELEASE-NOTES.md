Work-in-progress source kit (pre-release): no game, no XEX, no generated code and no executable. Bring your own Xbox 360 disc.

**v0.2.4**

- **One-click `Setup.cmd`:** asks before installing anything, installs Python and Microsoft's C++ Build Tools through winget if missing, downloads pinned, SHA-256-checked LLVM/Clang, CMake and Ninja into `.local/tools` only, fetches missing parts, opens the game-file installer and builds the port; `Play-<game>.cmd` starts it. Run it again after a failure: finished steps are skipped.
- **README:** a *How to play* guide for people new to this, and the same section layout as the other ports.
- The source-kit ZIP now includes the game-file installer (`tools/port-installer`); `Install-Files.cmd` did not work from the ZIP before.

`Setup.cmd` has not been tried from a fresh download with this game yet (only with FIFA Street 3). Please report problems with `setup-log.txt`.

**v0.2.3**

- **Controller vibration** on/off in the F1 PC settings.
- The game-data installer launcher is now `Install-Files.cmd` (English name).
- The shared game-data installer (`tools/port-installer`) ships in the repository.

Vibration has been compiled but not felt on a real controller yet. Nobody has played this exact build through.

**v0.2.2**

- Render-to-texture results are stored only when the CPU touches them, instead of being copied every frame. At 4K, 16x anisotropic filtering, no frame limit, in a match (Ryzen 5 3600, RTX 3050 Laptop), without MSAA / 2x / 4x / 8x: 92/76/67/55 fps before, 156/116/99/73 fps now.
- Fixed the tile depth clear, which erased depth the next tile still had to resolve.
- Native animation pose sampler, SIMD shader-constant copy, write-watched texture, vertex and index caches.
- PC settings menu reads every SDL pad; controller mappings sit next to the executable.
- The game keeps running when the audio device stops consuming.
- Experimental macOS build files (not compiled or run yet) and a native Linux build path (not built for this release).

This changes how lightmaps reach the game's memory. It was checked in one arena only: if stands or crowd turn red, please report it. Nobody has played this exact build through.

**Earlier**

- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Fixed: the crowd, the stands and the ground around the court were drawn red (load-time lightmaps were compressed from stale memory).
- Settings moved into the game: **F1** opens them; the separate settings tool is gone.
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Reaches menus, team selection and a running match on Windows 11 x64 with the 3D court, players, lighting and HUD drawn (60 fps in matches, correct team colors); crashes may remain.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify both executables by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates both modules locally and builds the port.
