
#include "liteui.hpp"

int main() {
  LiteUI ui(900, 650, "Paint");
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignContent = AlignContent::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{235, 235, 235, 255};

  View icon;
  icon.addChild(Svg{
      .style = {.width = Size::pixel(64), .height = Size::pixel(64)},
      .path = "chevron.svg",
      .fit = ObjectFit::Contain,
  });
  root.addChild(std::move(icon));

  View iconTwo;
  iconTwo.addChild(Svg{
      .style = {.width = Size::pixel(64), .height = Size::pixel(64)},
      .path = "settings-gear-svgrepo-com.svg",
      .fit = ObjectFit::Contain,
  });
  root.addChild(std::move(iconTwo));

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}