// src/main.cpp

#include "liteui.hpp"
#include <cstdio>

int main() {
  LiteUI ui(800, 600, "Scrolling demo");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(16);
  root.style.gap = 12;
  root.style.backgroundColor = Color{0xF2, 0xF2, 0xF2};

  View heading;
  heading.style.width = Size::full();
  heading.style.height = Size::pixel(24);
  heading.style.flexShrink = 0;
  root.addChild(heading);

  View list;
  list.style.direction = FlexDirection::Column;
  list.style.width = Size::full();
  list.style.height = Size::full();
  list.style.gap = 8;
  list.style.padding = EdgeInsets::all(8);
  list.style.backgroundColor = Color{255, 255, 255};
  list.style.borderWidth = 1.0f;
  list.style.borderColor = Color{0xCC, 0xCC, 0xCC};
  list.style.borderRadius = 6.0f;
  list.style.overflowY = Overflow::Scroll;

  const Color rowColors[] = {
      {0x4C, 0xAF, 0x50}, {0x21, 0x96, 0xF3}, {0xFF, 0x98, 0x00},
      {0x9C, 0x27, 0xB0}, {0xF4, 0x43, 0x36},
  };

  for (int i = 0; i < 20; ++i) {
    Image car;
    car.style.width = Size::pixel(400);
    car.style.height = Size::pixel(260);
    car.path = "car.jpg";
    car.fit = ObjectFit::Contain;
    car.backgroundColor = Color{240, 240, 240};
    car.onError = [](const std::string &err) {
      fprintf(stderr, "image load failed: %s\n", err.c_str());
    };

    list.addChild(std::move(car));
  }

  root.addChild(std::move(list));
  ui.setRoot(std::move(root));

  ui.run();
  return 0;
}