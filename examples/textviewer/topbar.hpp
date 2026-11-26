// topbar.hpp — a ready-made window top bar for liteui examples.
//
// LiteUI draws no window chrome by default (hideTitlebar = true), so every
// app supplies its own. This is the simple one the examples share:
//
//   [ title ............ drag area ............ ][-][□][x]
//
//   #define LITEUI_IMPLEMENTATION
//   #include "liteui.hpp"
//   #include "topbar.hpp"
//   ...
//   root.style.direction = FlexDirection::Column;
//   root.addChild(makeTopBar("My App"));   // first child, then your content
//
// It talks to the window through LiteUI::current(), so it can be built
// before the LiteUI object exists. Dragging the empty part moves the window;
// a quick second press toggles maximize. Glyph icons for now.
#pragma once

#include "liteui.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace topbar_detail {
constexpr const char *ICON_MINIMIZE = "\u23AF";
constexpr const char *ICON_MAXIMIZE = "\u25A1";
constexpr const char *ICON_RESTORE = "\u25A3";
constexpr const char *ICON_CLOSE = "\u00D7";

// One caption button: fixed-size box with a centered glyph. The glyph is a
// function so the maximize button can flip to "restore" by itself.
inline View makeCaptionButton(std::function<std::string()> glyph, Color hover,
                              std::function<void()> action) {
  Text t;
  t.label = std::move(glyph);
  t.fontSize = 16;
  t.color = Color{255, 255, 255};

  View b;
  b.style.direction = FlexDirection::Row;
  b.style.justifyContent = Justify::Center;
  b.style.alignItems = Align::Center;
  b.style.width = Size::pixel(46);
  b.style.height = Size::pixel(30);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.hoverColor = hover;
  // Acts on click; the press must not reach the drag area's move grab.
  b.onPointerDown = [](PointerEvent &e) { e.stopPropagation(); };
  b.onClick = [action = std::move(action)] { action(); };
  b.addChild(t);
  return b;
}
} // namespace topbar_detail

// Build the bar once and add it as the FIRST child of a column root. The top
// and right 6px are left free: they are the window's resize handles.
inline View makeTopBar(const std::string &title) {
  using namespace topbar_detail;

  Text titleText;
  titleText.label = std::function<std::string()>([title] { return title; });
  titleText.fontSize = 13;
  titleText.color = Color{210, 210, 210};

  View drag;
  drag.style.direction = FlexDirection::Row;
  drag.style.alignItems = Align::Center;
  drag.style.flexGrow = 1;
  drag.style.height = Size::full();
  drag.addChild(titleText);
  // Double-click is done by hand: Windows' native move loop would swallow
  // the release that a normal double-click needs.
  auto lastPress = std::make_shared<std::chrono::steady_clock::time_point>();
  drag.onPointerDown = [lastPress](PointerEvent &e) {
    LiteUI *ui = LiteUI::current();
    if (e.button != MouseButton::Left || !ui)
      return;
    const auto now = std::chrono::steady_clock::now();
    const bool second = now - *lastPress < std::chrono::milliseconds(400);
    *lastPress = second ? std::chrono::steady_clock::time_point{} : now;
    if (second)
      ui->requestMaximize();
    else
      ui->requestMove();
  };

  const Color hover{75, 75, 80}, closeHover{232, 17, 35};
  View bar;
  bar.style.direction = FlexDirection::Row;
  bar.style.alignItems = Align::Center;
  bar.style.gap = 2;
  bar.style.width = Size::full();
  bar.style.height = Size::pixel(36);
  bar.style.flexShrink = 0;
  bar.style.padding = EdgeInsets{6, 6, 0, 12}; // top, right, bottom, left
  bar.style.backgroundColor = Color{32, 32, 35};
  bar.addChild(std::move(drag));
  bar.addChild(makeCaptionButton(
      [] { return std::string(ICON_MINIMIZE); }, hover, [] {
        if (LiteUI *ui = LiteUI::current())
          ui->requestMinimize();
      }));
  bar.addChild(makeCaptionButton(
      [] {
        LiteUI *ui = LiteUI::current();
        return std::string(ui && ui->isMaximized() ? ICON_RESTORE
                                                   : ICON_MAXIMIZE);
      },
      hover, [] {
        if (LiteUI *ui = LiteUI::current())
          ui->requestMaximize();
      }));
  bar.addChild(makeCaptionButton(
      [] { return std::string(ICON_CLOSE); }, closeHover, [] {
        if (LiteUI *ui = LiteUI::current())
          ui->requestClose();
      }));
  return bar;
}
