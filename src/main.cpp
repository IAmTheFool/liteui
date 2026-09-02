#include "liteui.hpp"

int main() {
  LiteUI window(300, 200, "MinWidth + Full + Padding");

  View root;
  root.style.width = Size::full();
  root.style.height = Size::pixel(200);
  root.style.padding = EdgeInsets::all(20);   // 260px of content width left
  root.style.backgroundColor = {0xF8, 0xF9, 0xFA};

  View box;
  box.style.width = Size::full();   // would normally resolve to 260 (parent's content width)
  box.style.height = Size::pixel(60);
  box.style.minWidth = 100;         // has no visible effect here since 260 > 100 already
  box.style.backgroundColor = {40, 167, 69};
  box.style.borderWidth = 1;
  box.style.borderColor = {0, 0, 0};

  root.addChild(box);
  window.setRoot(root);
  window.run();
}