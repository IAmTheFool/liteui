// src/main.cpp
//
// Text feature showcase: one row per feature, stacked in a scrollable list.

#include "liteui.hpp"

// Small helper: a labeled row so each example is easy to read in the window.
// `caption` describes what's being demonstrated; `t` is the Text itself.
static View makeRow(const std::string &caption, Text t) {
  View row;
  row.style.direction = FlexDirection::Column;
  row.style.width = Size::full();
  row.style.padding = EdgeInsets::all(10);
  row.style.gap = 4;
  row.style.backgroundColor = {255, 255, 255};
  row.style.borderWidth = 1;
  row.style.borderColor = {0xE0, 0xE0, 0xE0};
  row.style.borderRadius = 6;

  Text label;
  label.label = caption;
  label.fontSize = 12;
  label.color = {0x88, 0x88, 0x88};
  row.addChild(std::move(label));

  row.addChild(std::move(t));
  return row;
}

int main() {
  LiteUI ui(480, 600, "Text feature showcase");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(16);
  root.style.gap = 10;
  root.style.overflowY = Overflow::Scroll; // whole window scrolls if content overflows
  root.style.backgroundColor = {0xF2, 0xF2, 0xF2};

  // ---- Font size ----
  {
    Text t;
    t.label = "Font size 24";
    t.fontSize = 24;
    root.addChild(makeRow("fontSize = 24", std::move(t)));
  }

  // ---- Font weight ----
  {
    Text t;
    t.label = "Bold weight";
    t.fontSize = 18;
    t.fontWeight = FontWeight::Bold;
    root.addChild(makeRow("fontWeight = Bold", std::move(t)));
  }
  {
    Text t;
    t.label = "Light weight";
    t.fontSize = 18;
    t.fontWeight = FontWeight::Light;
    root.addChild(makeRow("fontWeight = Light", std::move(t)));
  }

  // ---- Font style ----
  {
    Text t;
    t.label = "Italic text";
    t.fontSize = 18;
    t.fontStyle = FontStyle::Italic;
    root.addChild(makeRow("fontStyle = Italic", std::move(t)));
  }

  // ---- Color ----
  {
    Text t;
    t.label = "Colored text";
    t.fontSize = 18;
    t.color = {0x21, 0x96, 0xF3};
    root.addChild(makeRow("color = blue", std::move(t)));
  }

  // ---- Underline / strikethrough ----
  {
    Text t;
    t.label = "Underlined text";
    t.fontSize = 18;
    t.underline = true;
    root.addChild(makeRow("underline = true", std::move(t)));
  }
  {
    Text t;
    t.label = "Struck-through text";
    t.fontSize = 18;
    t.strikethrough = true;
    root.addChild(makeRow("strikethrough = true", std::move(t)));
  }

  // ---- Text alignment (needs a full-width box to see it) ----
  {
    Text t;
    t.label = "Centered text";
    t.fontSize = 18;
    t.align = TextAlign::Center;
    t.style.width = Size::full();
    root.addChild(makeRow("align = Center", std::move(t)));
  }

  // ---- Letter spacing ----
  {
    Text t;
    t.label = "Spaced out letters";
    t.fontSize = 18;
    t.letterSpacing = 4;
    root.addChild(makeRow("letterSpacing = 4", std::move(t)));
  }

  // ---- Wrap vs NoWrap (fixed width so the effect is visible) ----
  {
    Text t;
    t.label = "This is a longer sentence that will wrap onto multiple lines.";
    t.fontSize = 16;
    t.wrap = TextWrap::Wrap;
    t.style.width = Size::pixel(200);
    root.addChild(makeRow("wrap = Wrap, width = 200", std::move(t)));
  }
  {
    Text t;
    t.label = "This long sentence will NOT wrap and instead gets clipped.";
    t.fontSize = 16;
    t.wrap = TextWrap::NoWrap;
    t.overflow = TextOverflow::Clip;
    t.style.width = Size::pixel(200);
    root.addChild(makeRow("wrap = NoWrap, overflow = Clip, width = 200", std::move(t)));
  }

  // ---- Overflow: Ellipsis ----
  {
    Text t;
    t.label = "This long sentence gets truncated with an ellipsis at the end.";
    t.fontSize = 16;
    t.wrap = TextWrap::NoWrap;
    t.overflow = TextOverflow::Ellipsis;
    t.style.width = Size::pixel(200);
    root.addChild(makeRow("overflow = Ellipsis, width = 200", std::move(t)));
  }

  // ---- maxLines (wraps, then truncates after N lines) ----
  {
    Text t;
    t.label = "This paragraph is long enough to wrap across several lines, but "
              "maxLines caps how many are actually shown before truncating.";
    t.fontSize = 16;
    t.wrap = TextWrap::Wrap;
    t.overflow = TextOverflow::Ellipsis;
    t.maxLines = 2;
    t.style.width = Size::pixel(220);
    root.addChild(makeRow("maxLines = 2, width = 220", std::move(t)));
  }

  // ---- Line height ----
  {
    Text t;
    t.label = "Line one\nLine two\nLine three";
    t.fontSize = 16;
    t.lineHeight = 30; // extra-tall lines
    t.style.width = Size::pixel(200);
    root.addChild(makeRow("lineHeight = 30", std::move(t)));
  }

  // ---- Font family (falls back to platform default if not installed) ----
  {
    Text t;
    t.label = "Custom font family";
    t.fontSize = 18;
    t.fontFamily = "Georgia";
    root.addChild(makeRow("fontFamily = \"Georgia\"", std::move(t)));
  }

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}