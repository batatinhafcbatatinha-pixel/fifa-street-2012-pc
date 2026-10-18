// fifastreet4 - ReXGlue Recompiled Project

#pragma once

#include <filesystem>
#include <string>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <rex/input/input_system.h>

#include "native/street4_audio.h"
#include "script_input.h"
#include "native/street4_graphics.h"
#include "pc_options.h"
#include "street4_layout.h"

class Fifastreet4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    // Report the relocated launcher table location (see street4_layout.h).
    rex::PPCImageInfo info = PPCImageConfig;
    info.image_base = STREET4_EXE_LAYOUT_IMAGE_BASE;
    info.image_size = STREET4_EXE_LAYOUT_IMAGE_SIZE;
    return std::unique_ptr<Fifastreet4App>(new Fifastreet4App(ctx, "fifastreet4", info));
  }

 protected:
  // Game files: --game_data_root wins; otherwise <exe>/game_files, <exe>/game, then the
  // extracted disc next to the project.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto has_xex = [](const std::filesystem::path& p) {
      std::error_code ec;
      return !p.empty() && std::filesystem::is_regular_file(p / "default.xex", ec);
    };
    if (has_xex(paths.game_data_root)) return;
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    for (auto candidate : {exe_dir / "game_files", exe_dir / "game",
                           exe_dir / "../../../../game_files"}) {
      if (has_xex(candidate)) {
        paths.game_data_root = std::filesystem::weakly_canonical(candidate);
        return;
      }
    }
  }

  // Native graphics only: no GPU plugin, no Xenos command processor.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    std::string failed;
    if (!street4::pc::ApplyOptionsToRuntime(failed))
      REXLOG_WARN("street4: PC options not applied: {}", failed);
    config.gpu_plugin.clear();
    config.graphics = std::make_unique<street4::NativeGraphicsSystem>();
    // Native audio only: no APU / XMA register emulation.
    // FS4_INPUT=<file>: scripted pad for automated tests (script_input.h).
    if (const char* script = std::getenv("FS4_INPUT"); script && *script) {
      config.input_factory = [path = std::string(script)](bool tool_mode)
          -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        auto driver = std::make_unique<ScriptInputDriver>(path);
        driver->Setup();
        input->AddDriver(std::move(driver));
        return input;
      };
    }
    config.audio_factory = [](rex::runtime::FunctionDispatcher*)
        -> std::unique_ptr<rex::system::IAudioSystem> {
      return std::make_unique<street4::NativeAudioSystem>();
    };
  }

  // The launcher loads the game module as "fifadllzf.xex.dll"; the kernel resolves it
  // next to default.xex, i.e. \Device\Harddisk0\Partition1\fifadllzf.xex.dll. Register
  // that path too so the recompiled module is wired to the load.
  void OnPostSetup() override {
    runtime()->kernel_state()->RegisterRecompiledModule(
        "fifadllzf.xex.dll", "\\Device\\Harddisk0\\Partition1\\fifadllzf.xex.dll",
        "fifastreet4_fifadllzf_xex");
  }
};
