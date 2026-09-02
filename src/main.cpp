#include "liteui.hpp"

int main() {
  LiteUI window(300, 220, "FlexWrap + AlignContent");

  View root;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(20);
  root.style.backgroundColor = {0xF8, 0xF9, 0xFA};

  root.style.direction = FlexDirection::Row;
  root.style.flexWrap = FlexWrap::Wrap;          // items overflow onto new lines
  root.style.gap = 10;

  // 260px of content width (300 - 2*20). Six 90px-wide boxes with a 10px
  // gap won't fit on one line, so this wraps into multiple lines: 2 boxes
  // per line (90+10+90 = 190 fits, a third 90 would push past 260).
  root.style.alignContent = AlignContent::SpaceBetween; // push lines apart vertically

  for (int i = 0; i < 6; ++i) {
    View box;
    box.style.width = Size::pixel(90);
    box.style.height = Size::pixel(50);
    box.style.backgroundColor = {40, 167, 69};
    box.style.borderWidth = 1;
    box.style.borderColor = {0, 0, 0};
    root.addChild(box);
  }

  window.setRoot(root);
  window.run();
}