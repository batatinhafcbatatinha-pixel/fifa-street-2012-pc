// FIFA Street (2012) for the in-game PC settings (pc_settings.h). Used by the launcher and
// by the game module, which each keep a copy of the settings state.
#pragma once

#include "pc_settings.h"

inline pcs::GameInfo PcGameInfo() {
  // data/loc/locale.big ships eng_us, fre_fr, ger_de, ita_it and spa_es (XC_LANGUAGE ids).
  static const pcs::Language kLanguages[] = {
      {1, L"English"}, {4, L"Français"}, {3, L"Deutsch"}, {6, L"Italiano"}, {5, L"Español"}};
  pcs::GameInfo info;
  info.title = L"FIFA STREET";
  info.window_title = "FIFA Street";
  info.ini_name = L"fifastreet4-pc.ini";
  info.languages = kLanguages;
  info.language_count = int(sizeof(kLanguages) / sizeof(kLanguages[0]));
  info.internal_resolution = true;
  info.football_keyboard = true;
  return info;
}
