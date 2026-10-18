Work-in-progress source kit (pre-release): no game, no XEX, no generated code and no executable. Bring your own Xbox 360 disc.

- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- Reaches menus, team selection and a running match on Windows 11 x64, but the 3D pitch renders black (HUD only); slow frames, an audio-heap issue and crashes remain.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify both executables by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates both modules locally and builds the port and its settings tool.
