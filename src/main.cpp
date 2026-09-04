// src/main.cpp
//
// Minimal example: a window with a fixed-size scrollable list.

#include "liteui.hpp"
#include <cstdio>

int main() {
  LiteUI ui(400, 300, "Scrolling demo");

  // The whole window content, laid out top-to-bottom.
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(16);
  root.style.gap = 12;
  root.style.backgroundColor = {0xF2, 0xF2, 0xF2};

  // A plain heading above the scrollable area.
  View heading;
  heading.style.width = Size::full();
  heading.style.height = Size::pixel(24);

  Text headingText;
  headingText.label = "Scrollable list";
  headingText.fontSize = 18;
  headingText.fontWeight = FontWeight::SemiBold;
  headingText.color = {0x22, 0x22, 0x22};
  heading.addChild(std::move(headingText));

  root.addChild(heading);

  // The scrollable list itself: a fixed-height box holding more rows than
  // fit at once. overflowY = Scroll clips it and always shows the
  // vertical scrollbar; the rows below are simply added at their natural
  // height and the container takes care of the rest.
  View list;
  list.style.direction = FlexDirection::Column;
  list.style.width = Size::full();
  list.style.height =
      Size::pixel(200); // fixed viewport — content will overflow it
  list.style.gap = 8;
  list.style.padding = EdgeInsets::all(8);
  list.style.backgroundColor = {255, 255, 255};
  list.style.borderWidth = 1;
  list.style.borderColor = {0xCC, 0xCC, 0xCC};
  list.style.borderRadius = 6;
  list.style.overflowY = Overflow::Scroll;

  const Color rowColors[] = {
      {0x4C, 0xAF, 0x50}, {0x21, 0x96, 0xF3}, {0xFF, 0x98, 0x00},
      {0x9C, 0x27, 0xB0}, {0xF4, 0x43, 0x36},
  };

  for (int i = 0; i < 20; ++i) {
    View row;
    row.style.width = Size::full();
    row.style.height =
        Size::pixel(36); // natural height; list scrolls once these overflow it
    row.style.backgroundColor = rowColors[i % 5];
    row.style.borderRadius = 4;
    // Each row just prints its own index when clicked — swap in whatever
    // you'd actually do (open an item, toggle a selection, etc.).
    row.onClick = [i]() { std::printf("Row %d clicked\n", i); };
    Text label;
    label.label = "Item " + std::to_string(i);
    label.color = {255, 255, 255};
    label.style.padding = EdgeInsets::all(8);
    row.addChild(std::move(label));
    list.addChild(std::move(row));
  }

  root.addChild(std::move(list));
  ui.setRoot(std::move(root));

  ui.run();
  return 0;
}