// FIFA Street (2012) look for the PC settings: the game's EXTRA STUFF > SETTINGS screen, with
// its own fonts read from the user's disc (pc_font.h). Measured on that screen at 1152x648 and
// placed on the 1280x720 canvas: a white slanted header tag (small caption, stencil title), a
// black block with a white stripe on the left of the list, 36-unit rows split by faint lines,
// condensed bold labels, a centred value column whose selected cell is a yellow box with black
// text, a help panel on the right (title box, drop cap) and the button hints at the bottom right.
#include "pc_font.h"
#include "pc_settings.h"

#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>

namespace {

constexpr int kEm = 48;  // atlas size of every face; the screen scales it

constexpr uint32_t kWhite = 0xFFFFFFFFu, kBlack = 0xFF000000u, kYellow = 0xFFF5EC00u;
constexpr uint32_t kGrey = 0xFF8C8C8Cu, kInk = 0xFF1E1E1Eu;

float Scale(int em) { return float(em) / float(kEm); }

void LoadTheme(pcs::Theme& t) {
  try {
    auto root = pcfont::GameRoot("game_files");
    pcfont::LoadTtf(root, "data/ui/fonts/eahelveticaneueconbol-mod.ttf", kEm, t.font);
    pcfont::LoadTtf(root, "data/ui/fonts/fs4_header.ttf", kEm, t.title_font);
    pcfont::LoadTtf(root, "data/ui/fonts/fs4_body.ttf", kEm, t.extra_font);
  } catch (const std::exception& e) {
    t.font = {};
    REXLOG_WARN("[pc] FIFA Street menu fonts: {} (the menu keeps its own look)", e.what());
  }
}

// One drawn line of the list: a group heading or a menu row.
struct Line {
  const pcs::MenuRow* row;
  const std::wstring* heading;
};

void Draw(const pcs::Theme& t, pcs::Canvas& c, pcs::MenuView& v) {
  const pcs::ThemeFont& body = t.font;
  const pcs::ThemeFont& stencil = t.title_font.glyphs.empty() ? body : t.title_font;
  const pcs::ThemeFont& light = t.extra_font.glyphs.empty() ? body : t.extra_font;
  auto text_y = [&](const pcs::ThemeFont& f, int em, int top, int height) {
    return top + (height - int(f.line_height * Scale(em))) / 2;
  };

  // The game's screens sit on a dark concrete wall; the paused frame stays faintly visible.
  c.Fill(0, 0, 1280, 720, 0xEA14171Au);

  // Header tag: grey stripes, then the white slanted plate with caption and stencil title.
  c.Fill(0, 61, 120, 6, 0xFF6E7174u);
  c.Fill(0, 71, 104, 4, 0xFF6E7174u);
  // The plate grows with the title (the game's are short words; ours are longer in some languages).
  const int title_w = std::min(760, c.Measure(stencil, v.title, Scale(30), true));
  const int right = std::max(340, 140 + title_w + 40);
  const int tag[] = {108, 52, right, 52, right - 28, 96, 96, 96};
  c.Polygon(tag, 4, 0xFFF2F2F2u);
  c.Text(light, v.subtitle, 140, 52, right - 170, Scale(13), kInk, 0, true);
  c.Text(stencil, v.title, 140, 64, title_w + 4, Scale(30), kInk, 0, true);

  // The list: a window of lines around the selection.
  std::vector<Line> lines;
  int selected_line = 0;
  for (const pcs::MenuRow& r : v.rows) {
    if (!r.section.empty()) lines.push_back({nullptr, &r.section});
    if (r.selected) selected_line = int(lines.size());
    lines.push_back({&r, nullptr});
  }
  constexpr int kTop = 150, kRow = 36, kVisible = 12;
  static int first = 0;
  if (selected_line < first + 1) first = std::max(0, selected_line - 1);
  if (selected_line > first + kVisible - 2) first = selected_line - kVisible + 2;
  first = std::clamp(first, 0, std::max(0, int(lines.size()) - kVisible));
  const int shown = std::min(kVisible, int(lines.size()) - first);
  const int x0 = 89, x1 = 720, value_x = 400;
  c.Fill(0, kTop, 81, shown * kRow, kBlack);
  c.Fill(81, kTop, 7, shown * kRow, kWhite);
  for (int i = 0; i < shown; ++i) {
    const Line& line = lines[first + i];
    const int y = kTop + i * kRow;
    if (i) c.Fill(x0, y, x1 - x0, 1, 0x50FFFFFFu);
    if (line.heading) {
      c.Text(body, *line.heading, x0 + 23, text_y(body, 16, y, kRow), 300, Scale(16), kYellow, 0, true);
      continue;
    }
    const pcs::MenuRow& r = *line.row;
    const uint32_t ink = r.greyed ? kGrey : kWhite;
    c.Text(body, r.label, x0 + 23, text_y(body, 21, y, kRow), value_x - x0 - 30, Scale(21), ink, 0, false);
    std::wstring value = r.value + (r.restart ? L" *" : L"");
    if (r.selected) {
      if (r.value.empty()) {
        // An action line: the whole row is the yellow bar.
        c.Fill(x0, y + 1, x1 - x0, kRow - 1, kYellow);
        c.Text(body, r.label, x0 + 23, text_y(body, 21, y, kRow), x1 - x0 - 40, Scale(21), kBlack, 0, false);
      } else {
        c.Fill(value_x, y + 1, x1 - value_x, kRow - 1, kYellow);
        c.Text(body, value, value_x + 34, text_y(body, 20, y, kRow), x1 - value_x - 68, Scale(20), kBlack, 2, true);
        // The game's arrow boxes at both ends (only the right one on screen for one-way lists).
        for (int side = 0; side < 2; ++side) {
          int bx = side ? x1 - 30 : value_x + 6;
          c.Fill(bx, y + 9, 24, 18, kBlack);
          c.Fill(bx + 2, y + 11, 20, 14, kWhite);
          const int left[] = {bx + 15, y + 13, bx + 15, y + 23, bx + 7, y + 18};
          const int right[] = {bx + 9, y + 13, bx + 9, y + 23, bx + 17, y + 18};
          c.Polygon(side ? right : left, 3, kBlack);
        }
        v.hits.push_back({r.id, value_x, y, value_x + 40, y + kRow, true});
      }
    } else if (!r.value.empty()) {
      c.Text(body, value, value_x + 20, text_y(body, 18, y, kRow), x1 - value_x - 40, Scale(18), ink, 2, true);
    }
    v.hits.push_back({r.id, x0, y, x1, y + kRow, false});
  }
  // More lines above or below: small yellow markers on the black block.
  if (first > 0) {
    const int up[] = {28, kTop + 24, 52, kTop + 24, 40, kTop + 10};
    c.Polygon(up, 3, kYellow);
  }
  if (first + shown < int(lines.size())) {
    const int bottom = kTop + shown * kRow;
    const int down[] = {28, bottom - 24, 52, bottom - 24, 40, bottom - 10};
    c.Polygon(down, 3, kYellow);
  }

  // Help panel: the line's name in a dark box, then the text with a white drop-cap box.
  if (!v.help_title.empty()) {
    int w = c.Measure(body, v.help_title, Scale(30), false) + 14;
    c.Fill(764, 268, std::min(w, 440), 34, 0xFF3C3C3Cu);
    c.Text(body, v.help_title, 771, text_y(body, 30, 268, 34), 426, Scale(30), kWhite, 0, false);
  }
  if (!v.help.empty()) {
    std::wstring rest = v.help.substr(1);
    c.Fill(792, 306, 58, 62, kWhite);
    c.Text(body, v.help.substr(0, 1), 792, 300, 58, Scale(70), kBlack, 2, true);
    // Greedy word wrap: three lines beside the drop cap, then the full width.
    int y = 306, x = 858, width = 1180 - 858;
    size_t p = 0;
    while (p < rest.size() && y < 600) {
      size_t end = p, fit = p;
      while (end <= rest.size()) {
        size_t space = rest.find(L' ', end);
        if (space == std::wstring::npos) space = rest.size();
        if (c.Measure(light, rest.substr(p, space - p), Scale(18), false) > width && fit > p) break;
        fit = space;
        end = space + 1;
        if (space == rest.size()) break;
      }
      if (fit == p) fit = std::min(rest.size(), p + 1);
      c.Text(light, rest.substr(p, fit - p), x, y, width, Scale(18), kWhite, 0, false);
      p = fit + (fit < rest.size() && rest[fit] == L' ' ? 1 : 0);
      y += 22;
      if (y >= 372) { x = 792; width = 1180 - 792; }
    }
  }

  // Bottom: keyboard line and the restart note on the left, the A / B hints on the right.
  c.Text(light, v.keys, 98, 650, 560, Scale(15), 0xFFB4B4B4u, 0, false);
  if (!v.restart_note.empty()) c.Text(body, v.restart_note, 98, 622, 560, Scale(17), kYellow, 0, true);
  int hx = 1172;
  auto hint = [&](wchar_t letter, const std::wstring& caption, int id) {
    int tw = c.Measure(body, caption, Scale(21), true);
    hx -= tw;
    c.Text(body, caption, hx, text_y(body, 21, 640, 32), tw + 2, Scale(21), kWhite, 0, true);
    hx -= 30;
    // The game's button glyph: a white ring with the letter.
    auto disc = [&](int cx, int cy, int r, uint32_t argb) {
      int xy[32];
      for (int k = 0; k < 16; ++k) {
        xy[2 * k] = cx + int(std::lround(r * std::cos(k * 3.14159265 / 8)));
        xy[2 * k + 1] = cy + int(std::lround(r * std::sin(k * 3.14159265 / 8)));
      }
      c.Polygon(xy, 16, argb);
    };
    disc(hx + 14, 656, 13, kWhite);
    disc(hx + 14, 656, 11, kBlack);
    c.Text(body, std::wstring(1, letter), hx + 2, text_y(body, 18, 643, 26), 24, Scale(18), kWhite, 2, true);
    v.hits.push_back({id, hx, 640, hx + 30 + tw, 672, false});
    hx -= 18;
  };
  hint(L'A', v.select, -1);
  hint(L'B', v.back, -2);
}

}  // namespace

const pcs::Theme* PcStreet4Theme() {
  static pcs::Theme theme = [] {
    pcs::Theme t;
    t.load = LoadTheme;
    t.draw = Draw;
    return t;
  }();
  return &theme;
}
