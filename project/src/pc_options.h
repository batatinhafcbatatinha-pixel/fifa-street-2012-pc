#pragma once

#include <filesystem>
#include <string>
#include <array>
#include <utility>

namespace street4::pc {
struct Options {
  int window_width = 1280;
  int window_height = 720;
  bool fullscreen = true;
  bool vsync = true;
  bool mute = false;
  bool keyboard = false;
};

std::filesystem::path ExecutableDirectory();
std::filesystem::path OptionsPath();
Options ReadOptions(const std::filesystem::path& path);
bool SaveOptions(const std::filesystem::path& path, const Options& options,
                 std::wstring& error);
using RuntimeFlags = std::array<std::pair<const char*, std::string>, 6>;
RuntimeFlags RuntimeFlagValues(const Options& options);
// Displays only the native options window; never launches the game.
int ShowPcOptions(int show = 1);
// Called in ReXApp::OnPreSetup, after LoadConfig and before window creation.
// Requires pc_options_runtime.cpp in the game target; never starts game code.
bool ApplyOptionsToRuntime(std::string& failed_flags);
}  // namespace street4::pc
