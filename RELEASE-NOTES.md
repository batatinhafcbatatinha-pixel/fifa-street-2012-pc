Work-in-progress source kit (pre-release): no game, no XEX, no generated code and no executable. Bring your own Xbox 360 disc.

- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Fixed: the crowd, the stands and the ground around the court were drawn red (load-time lightmaps were compressed from stale memory).
- Settings moved into the game: **F1** opens them; the separate settings tool is gone.
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Reaches menus, team selection and a running match on Windows 11 x64 with the 3D court, players, lighting and HUD drawn (60 fps in matches, correct team colors); crashes may remain.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify both executables by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates both modules locally and builds the port.
