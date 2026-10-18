#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "pc_options.h"

namespace street4::pc {
std::filesystem::path ExecutableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (!size || size >= path.size()) return {};
  path.resize(size);
  return std::filesystem::path(path).parent_path();
}

std::filesystem::path OptionsPath() {
  return ExecutableDirectory() / L"fifastreet4-pc.ini";
}

Options ReadOptions(const std::filesystem::path& path) {
  Options result;
  auto read = [&](const wchar_t* name, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(L"PC", name, fallback, path.c_str()));
  };
  const int width = read(L"WindowWidth", result.window_width);
  const int height = read(L"WindowHeight", result.window_height);
  if (width >= 640 && width <= 7680 && height >= 480 && height <= 4320) {
    result.window_width = width;
    result.window_height = height;
  }
  result.fullscreen = read(L"Fullscreen", result.fullscreen) != 0;
  result.vsync = read(L"Vsync", result.vsync) != 0;
  result.mute = read(L"Mute", result.mute) != 0;
  result.keyboard = read(L"Keyboard", result.keyboard) != 0;
  return result;
}

RuntimeFlags RuntimeFlagValues(const Options& options) {
  return {{{"window_width", std::to_string(options.window_width)},
           {"window_height", std::to_string(options.window_height)},
           {"fullscreen", options.fullscreen ? "true" : "false"},
           {"vsync", options.vsync ? "true" : "false"},
           {"audio_mute", options.mute ? "true" : "false"},
           {"mnk_mode", options.keyboard ? "true" : "false"}}};
}

bool SaveOptions(const std::filesystem::path& path, const Options& options,
                 std::wstring& error) {
  if (options.window_width < 640 || options.window_width > 7680 ||
      options.window_height < 480 || options.window_height > 4320) {
    error = L"Tamanho da janela fora do intervalo permitido.";
    return false;
  }
  // A single section write avoids partially updating a collection of keys.
  const std::wstring entries[] = {
      L"WindowWidth=" + std::to_wstring(options.window_width),
      L"WindowHeight=" + std::to_wstring(options.window_height),
      L"Fullscreen=" + std::to_wstring(options.fullscreen),
      L"Vsync=" + std::to_wstring(options.vsync),
      L"Mute=" + std::to_wstring(options.mute),
      L"Keyboard=" + std::to_wstring(options.keyboard)};
  std::wstring section;
  for (const auto& entry : entries) { section += entry; section += L'\0'; }
  section += L'\0';
  if (!WritePrivateProfileSectionW(L"PC", section.c_str(), path.c_str())) {
    error = L"N\u00e3o foi poss\u00edvel salvar a configura\u00e7\u00e3o. Erro Windows: " +
            std::to_wstring(GetLastError());
    return false;
  }
  error.clear();
  return true;
}
}  // namespace street4::pc
