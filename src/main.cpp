#include "liteui.hpp"
#include <iostream>

int main() {
  LiteUI window(800, 600, "A Bare Window");

  View root;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.direction = FlexDirection::Row;
  root.style.justifyContent = Justify::Center;
  root.style.alignItems = Align::Center;
  root.style.padding = EdgeInsets::all(20);
  root.style.gap = 12;
  root.style.backgroundColor = {0xEE, 0xEE, 0xEE};

  View card;
  card.style.width = Size::pixel(150);
  card.style.height = Size::pixel(100);
  card.style.backgroundColor = {0x33, 0x99, 0xEE};
  card.style.borderRadius = 12;
  card.style.borderWidth = 2;
  card.style.borderColor = {0x11, 0x33, 0x55};

  root.addChild(card);

  window.setRoot(root);
  window.run();
}
