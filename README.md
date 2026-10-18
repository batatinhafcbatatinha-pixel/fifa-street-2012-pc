# FIFA Street (2012) — Native PC Port (work in progress) | No Emulated GPU or Audio Hardware

> [!WARNING]
> This repository does **not** include the game, an ISO/XEX, EA assets, generated game code or a precompiled executable.
> You need your own Xbox 360 disc (USA/Europe). See [the full notice](#notice) at the end.

An experimental port of the Xbox 360 FIFA Street (2012, sometimes called "FIFA Street 4"), built through static recompilation with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), with its own Direct3D 11 renderer and its own XMA audio pipeline. **This public repository is a source and local-build kit, not a ready-to-play game download.** The game's PowerPC code (the launcher `default.xex` and the main game library `fifadllzf.xex.dll`) is translated locally from your own verified disc; no game executable or generated game code is distributed here.

## Status — work in progress

- Reached on Windows 11 x64 with a build made from this kit: splash screens, videos, title, menus, team selection and a running match (clock, score, HUD and replay prompt update). **The 3D view of the pitch currently renders black** during matches: only the HUD is drawn. This is the main open bug.
- Graphics: the Direct3D library inside the game's DLL is hooked and drawn by a native Direct3D 11 renderer; Xenos shader microcode is translated to HLSL SM5; textures are untiled and converted on load. No GPU command processor or emulation plugin.
- Audio: XMA contexts live in guest memory and are decoded with FFmpeg's XMA decoder; `XAudio` render-driver frames go to XAudio2 with a 5.1 → stereo downmix. No audio hardware model.
- The Xbox 360 kernel and XAM services come from the ReXGlue runtime as an operating-system compatibility layer.
- Other known issues: occasional very slow frames in some runs and an intermittent audio-heap exhaustion; 47 launcher functions referenced only through data are not yet listed. Expect crashes. DLC is not supported.
- Windows x64 only.

## Why a native port instead of an emulator

An emulator such as [Xenia](https://xenia.jp) recreates the Xbox 360: it JIT-translates the PowerPC code at run time, emulates the Xenos GPU by processing the console's command buffers and models the audio hardware. That is the right approach for the whole library. This port takes a different route for one game:

- **Ahead-of-time code.** Both PowerPC modules become C++ compiled by a modern optimizing compiler before you play.
- **No GPU emulation in the hot path.** The game's D3D library builds its command segments in memory that nothing reads; draws, clears, resolves and presents are hooked and executed directly on Direct3D 11.
- **Native audio** decoding and mixing on the PC.
- **Readable and fixable.** Every hook, renderer path and audio routine is plain C++ in `project/src`.

The trade-off: it covers only this game and revision, it is unfinished, and an emulator is the more general and better-tested option.

## Your game files

Only the **Xbox 360 USA/Europe** revision (TitleID `4541097B`, version 3) is accepted. Extract your own disc image into a new folder and validate it:

```sh
python tools/extract_iso.py "FIFA Street (USA, Europe).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xex        f4902ab0a2515ca64675544852a0263ba429863dad13dc19b7c2d989c97aa2b1
fifadllzf.xex.dll  9d2fb47c3caf279d55962f671afc5b8eb7a597717d44ccb5995dd002a3c26201
```

Do not upload the ISO, extracted files, generated code or the built executables.

## Build on Windows

Requirements: Python 3.10+, Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang 20 or newer (a complete install that includes the clang resource headers), CMake 3.25+ and Ninja.

```powershell
python tools/build_local.py --game-files game_files --llvm-dir "C:\Program Files\LLVM"
python tools/play.py --game-files game_files
```

`build_local.py` validates your files, downloads the pinned ReXGlue SDK nightly (`0.10.0.24-dev.gbd833a2`, SHA-256 checked) and applies the FPSCR patch in `patches/`, downloads the FFmpeg headers matching the SDK's `libavcodec`, translates both modules into private C++ under `.local/`, compiles the port and the `fifastreet4_options` settings tool, and removes any GPU emulation plugin from the output. The translation of the 29 MB game library is large: expect a long first build and several GB of disk space in `.local/`.

## Settings

`fifastreet4_options.exe` (built next to the game in `.local/build/`) sets window size, borderless fullscreen, VSync and mute before you start the game. Xbox controllers work as on the console.

## Credits and licenses

- Port integration, native renderer and audio: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Tom Clay and contributors, BSD-3-Clause; derived from [Xenia](https://github.com/xenia-project/xenia) (Ben Vanik and contributors). External dependency, downloaded by the build script; license in `licenses/`. The texture untiling and format rules follow Xenia's BSD-licensed `texture_util`; shader-microcode semantics follow the SDK's Xenia-derived headers.
- [FFmpeg](https://ffmpeg.org) (Xenia fork by wmarti), LGPL-2.1: the SDK's prebuilt `libavcodec`/`libavutil` and their public headers, downloaded at build time.
- Microsoft Visual C++ runtime, Windows SDK, Direct3D and XAudio2: Microsoft; obtained separately.
- FIFA Street, its game code, trademarks and assets belong to Electronic Arts and the respective rights holders. This project is not affiliated with or endorsed by EA or Microsoft.

See [NOTICE.md](NOTICE.md) for the publication boundary and [LICENSE](LICENSE) for the license of the port's own code.

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XEX, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED XBOX 360 GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
