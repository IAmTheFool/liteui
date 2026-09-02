#include "liteui.hpp"

int main() {
  LiteUI window(300, 220, "Absolute Positioning Demo");

  View root;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(20);
  root.style.backgroundColor = {0xF8, 0xF9, 0xFA};

  // A normal in-flow "card" — absolute positioning is always relative to
  // this box's content area, not the window.
  View card;
  card.style.width = Size::pixel(200);
  card.style.height = Size::pixel(120);
  card.style.backgroundColor = {255, 255, 255};
  card.style.borderWidth = 1;
  card.style.borderColor = {0, 0, 0};
  card.style.borderRadius = 8;
  card.style.padding = EdgeInsets::all(12);

  // Some ordinary flex content inside the card.
  View label;
  label.style.width = Size::full();
  label.style.height = Size::pixel(20);
  label.style.backgroundColor = {0xE9, 0xEC, 0xEF};
  card.addChild(label);

  // The badge: pulled out of flex flow, pinned to the card's top-right
  // corner regardless of what else is in the card.
  View badge;
  badge.style.position = Position::Absolute;
  badge.style.top = -8;     // negative offsets work too, like CSS
  badge.style.right = -2;
  badge.style.width = Size::pixel(24);
  badge.style.height = Size::pixel(24);
  badge.style.backgroundColor = {0xDC, 0x35, 0x45};
  badge.style.borderRadius = 12; // full circle at 24x24
  badge.style.zIndex = 1;        // paint above the card's own border/content
  card.addChild(badge);

  root.addChild(card);
  window.setRoot(root);
  window.run();
}