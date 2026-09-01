#include "liteui.hpp"
#include <iostream>

int main() {
  LiteUI window(800, 600, "A Bare Window");

  Box newBox;
  newBox.width = 200;
  newBox.height = 100;
  newBox.pos_x = 50;
  newBox.pos_y = 60;
  newBox.color = {0x33, 0x99, 0xEE};

  window.addBox(newBox);
  window.run();

  window.run();
}
