// Native Windows configuration UI. This executable never starts the game.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "pc_options.h"
#include <array>
#include <cwchar>

namespace {
using street4::pc::Options;
constexpr int kSize = 101, kFullscreen = 102, kVsync = 103, kMute = 104,
              kKeyboard = 105, kSave = 110, kDiagnostics = 111;
constexpr std::array<std::pair<int, int>, 6> kSizes = {{{1280, 720}, {1600, 900},
    {1920, 1080}, {2560, 1440}, {3840, 2160}, {7680, 4320}}};
HFONT font = nullptr;
HWND status_label = nullptr;
std::array<std::pair<int, int>, 7> available_sizes;
int size_count = 0;

HWND Add(HWND parent, const wchar_t* kind, const wchar_t* text, DWORD style,
         int x, int y, int width, int height, int id = 0) {
  HWND control = CreateWindowExW(0, kind, text, WS_CHILD | WS_VISIBLE | style,
      x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
      GetModuleHandleW(nullptr), nullptr);
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  return control;
}

bool Checked(HWND window, int id) {
  return SendDlgItemMessageW(window, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void Diagnostics(HWND window) {
  const auto root = street4::pc::ExecutableDirectory();
  std::wstring text = L"Pasta do programa:\r\n" + root.wstring() +
      L"\r\n\r\nConfigura\u00e7\u00e3o PC:\r\n" + street4::pc::OptionsPath().wstring();
  auto report = [&](const wchar_t* name) {
    std::error_code error;
    text += L"\r\n";
    text += name;
    text += std::filesystem::is_regular_file(root / name, error) ? L": presente" : L": ausente";
  };
  text += L"\r\n\r\nArquivos junto ao menu:";
  report(L"fifastreet4.exe");
  report(L"fifastreet4_fifadllzf_xex.dll");
  text += L"\r\n\r\nPresen\u00e7a de arquivos n\u00e3o comprova funcionamento."
          L"\r\nBoot, renderiza\u00e7\u00e3o, \u00e1udio e partida ainda exigem valida\u00e7\u00e3o."
          L"\r\nO renderer nativo espec\u00edfico do jogo continua pendente."
          L"\r\nEste menu n\u00e3o comprova a exist\u00eancia de um port jog\u00e1vel.";
  MessageBoxW(window, text.c_str(), L"Diagn\u00f3stico local", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
          L"Segoe UI");
      Add(window, L"STATIC", L"FIFA STREET 4  |  Configura\u00e7\u00e3o PC", 0, 26, 20, 660, 28);
      Add(window, L"STATIC", L"Port em desenvolvimento. Gameplay ainda n\u00e3o validado.",
          0, 26, 55, 660, 24);
      Add(window, L"STATIC", L"Tamanho da janela", 0, 26, 104, 250, 24);
      HWND size = Add(window, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST |
          WS_VSCROLL, 310, 99, 340, 240, kSize);
      auto options = street4::pc::ReadOptions(street4::pc::OptionsPath());
      size_count = 0;
      int selected = -1;
      for (auto pair : kSizes) {
        available_sizes[size_count] = pair;
        std::wstring label = std::to_wstring(pair.first) + L" \u00d7 " + std::to_wstring(pair.second);
        SendMessageW(size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (pair.first == options.window_width && pair.second == options.window_height)
          selected = size_count;
        ++size_count;
      }
      if (selected < 0) {
        selected = size_count;
        available_sizes[size_count++] = {options.window_width, options.window_height};
        std::wstring label = std::to_wstring(options.window_width) + L" \u00d7 " +
            std::to_wstring(options.window_height) + L" (salvo)";
        SendMessageW(size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      }
      SendMessageW(size, CB_SETCURSEL, selected, 0);
      const struct { int id; const wchar_t* text; bool checked; } checks[] = {
          {kFullscreen, L"Tela cheia sem bordas (usa o tamanho do desktop)", options.fullscreen},
          {kVsync, L"Sincroniza\u00e7\u00e3o vertical", options.vsync},
          {kMute, L"Silenciar sa\u00edda de \u00e1udio", options.mute},
          {kKeyboard, L"Habilitar teclado como controle", options.keyboard}};
      int y = 148;
      for (const auto& check : checks) {
        HWND control = Add(window, L"BUTTON", check.text,
            WS_TABSTOP | BS_AUTOCHECKBOX, 26, y, 640, 28, check.id);
        SendMessageW(control, BM_SETCHECK, check.checked ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 37;
      }
      Add(window, L"STATIC", L"Teclado: WASD = anal\u00f3gico esquerdo; setas = direito;"
          L"\r\nEspa\u00e7o = A; Backspace = B; L = X; P = Y; Enter = Start."
          L"\r\nControles f\u00edsicos s\u00e3o detectados pelo runtime."
          L"\r\nO tamanho da janela n\u00e3o altera a resolu\u00e7\u00e3o interna do jogo.",
          0, 26, 307, 660, 90);
      Add(window, L"BUTTON", L"Salvar op\u00e7\u00f5es", WS_TABSTOP | BS_DEFPUSHBUTTON,
          26, 412, 170, 36, kSave);
      Add(window, L"BUTTON", L"Diagn\u00f3stico", WS_TABSTOP | BS_PUSHBUTTON,
          211, 412, 150, 36, kDiagnostics);
      Add(window, L"BUTTON", L"Jogar (em desenvolvimento)", WS_DISABLED | BS_PUSHBUTTON,
          376, 412, 278, 36);
      status_label = Add(window, L"STATIC", L"As op\u00e7\u00f5es salvas ser\u00e3o aplicadas na pr\u00f3xima inicializa\u00e7\u00e3o."
          L"\r\nEste menu n\u00e3o executa o jogo.", 0, 26, 465, 660, 56);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kSave && HIWORD(wparam) == BN_CLICKED) {
        const int index = static_cast<int>(SendDlgItemMessageW(window, kSize, CB_GETCURSEL, 0, 0));
        if (index < 0 || index >= size_count) return 0;
        Options options;
        options.window_width = available_sizes[index].first;
        options.window_height = available_sizes[index].second;
        options.fullscreen = Checked(window, kFullscreen);
        options.vsync = Checked(window, kVsync);
        options.mute = Checked(window, kMute);
        options.keyboard = Checked(window, kKeyboard);
        std::wstring error;
        if (street4::pc::SaveOptions(street4::pc::OptionsPath(), options, error)) {
          SetWindowTextW(status_label, L"Op\u00e7\u00f5es salvas em fifastreet4-pc.ini."
              L"\r\nSer\u00e3o aplicadas na pr\u00f3xima inicializa\u00e7\u00e3o.");
        } else MessageBoxW(window, error.c_str(), L"Falha ao salvar", MB_OK | MB_ICONERROR);
        return 0;
      }
      if (LOWORD(wparam) == kDiagnostics && HIWORD(wparam) == BN_CLICKED) {
        Diagnostics(window);
        return 0;
      }
      break;
    case WM_DESTROY:
      DeleteObject(font);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

int SelfTest() {
  wchar_t temp[MAX_PATH], file[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, temp) || !GetTempFileNameW(temp, L"s4p", 0, file)) return 10;
  const auto path = std::filesystem::path(file);
  Options options;
  options.window_width = 2560; options.window_height = 1440;
  options.fullscreen = false; options.vsync = false; options.mute = true; options.keyboard = true;
  std::wstring error;
  bool passed = street4::pc::SaveOptions(path, options, error);
  const auto actual = street4::pc::ReadOptions(path);
  passed = passed && actual.window_width == 2560 && actual.window_height == 1440 &&
      !actual.fullscreen && !actual.vsync && actual.mute && actual.keyboard;
  const auto flags = street4::pc::RuntimeFlagValues(actual);
  const std::array<std::pair<std::string, std::string>, 6> expected = {{{"window_width", "2560"},
      {"window_height", "1440"}, {"fullscreen", "false"}, {"vsync", "false"},
      {"audio_mute", "true"}, {"mnk_mode", "true"}}};
  for (size_t i = 0; i < flags.size(); ++i)
    passed = passed && flags[i].first == expected[i].first && flags[i].second == expected[i].second;
  options.window_width = -1;
  passed = passed && !street4::pc::SaveOptions(path, options, error);
  WritePrivateProfileStringW(L"PC", L"WindowHeight", L"999999", file);
  const auto clamped = street4::pc::ReadOptions(path);
  passed = passed && clamped.window_width == 1280 && clamped.window_height == 720;
  DeleteFileW(file);
  return passed ? 0 : 11;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool self_test = argc == 2 && wcscmp(argv[1], L"--self-test") == 0;
  LocalFree(argv);
  if (self_test) return SelfTest();
  return street4::pc::ShowPcOptions(show);
}

int street4::pc::ShowPcOptions(int show) {
  HINSTANCE instance = GetModuleHandleW(nullptr);
  // Keep logical layout consistent; Windows scales this native dialog on high DPI displays.
  WNDCLASSW cls{};
  cls.lpfnWndProc = WindowProc;
  cls.hInstance = instance;
  cls.lpszClassName = L"Street4NativePcOptions";
  cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  if (!RegisterClassW(&cls)) return 1;
  constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT size{0, 0, 690, 530};
  AdjustWindowRectEx(&size, style, FALSE, 0);
  HWND window = CreateWindowExW(0, cls.lpszClassName, L"FIFA Street 4 - Op\u00e7\u00f5es PC",
      style, CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
      nullptr, nullptr, instance, nullptr);
  if (!window) return 2;
  ShowWindow(window, show);
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return static_cast<int>(message.wParam);
}
