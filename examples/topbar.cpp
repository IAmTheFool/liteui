#include "liteui.hpp"
#include <iostream>
int main() {
  // Create the window first so the bar's buttons can talk to it.
  // 4th argument = hideTitlebar: no default titlebar, we draw our own.
  LiteUI ui("Custom Bar", 900, 600, true);

  const Color barBg{32, 32, 36};
  const Color white{235, 235, 235};

  // One reusable factory for the three square buttons.
  auto barButton = [&](const std::string &glyph, Color hover,
                       std::function<void()> onClick) {
    View b;
    b.style.width = Size::pixel(46);    // height stretches to the bar's
    b.style.alignItems = Align::Center; // center the glyph both ways
    b.style.justifyContent = Justify::Center;
    b.style.hoverColor = hover;
    b.onClick = std::move(onClick);

    Text t;
    t.label = glyph;
    t.fontSize = 14;
    t.color = white;
    b.addChild(t);
    return b;
  };

  // Left side: the title. It grows to fill the free space and doubles
  // as the drag handle.
  Text title;
  title.label = "Custom Bar";
  title.fontSize = 13;
  title.color = white;

  View dragArea;
  dragArea.style.flexGrow = 1;
  dragArea.style.alignItems = Align::Center; // vertically center the title
  dragArea.style.padding = EdgeInsets{0, 0, 0, 12}; // top, right, bottom, left
  dragArea.onPressAt = [&](float, float) {
    std::cout << "Moving" << std::endl;
    ui.requestMove();
  };
  dragArea.addChild(title);

  // The bar itself: title area, then minimize / maximize / close.
  View bar;
  bar.style.width = Size::full();
  bar.style.height = Size::pixel(36);
  bar.style.backgroundColor = barBg;
  bar.addChild(dragArea);
  bar.addChild(barButton("\xE2\x80\x93", Color{70, 70, 76},
                         [&] { ui.requestMinimize(); }));
  bar.addChild(barButton("\xE2\x96\xA1", Color{70, 70, 76},
                         [&] { ui.requestMaximize(); }));
  bar.addChild(barButton("\xE2\x9C\x95", Color{196, 43, 28},
                         [&] { ui.requestClose(); }));

  // Everything below the bar.
  Text hello;
  hello.label = "Window content goes here";
  hello.fontSize = 20;

  View content;
  content.style.flexGrow = 1; // take all the space under the bar
  content.style.alignItems = Align::Center;
  content.style.justifyContent = Justify::Center;
  content.style.backgroundColor = Color{245, 245, 245};
  content.addChild(hello);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(bar);
  root.addChild(content);

  ui.setRoot(root);
  ui.run();
  return 0;
}