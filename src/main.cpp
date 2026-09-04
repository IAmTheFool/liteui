#include "liteui.hpp"

int main() {
  int count = 0;

  Text label;
  label.source = [&]() { return "Count: " + std::to_string(count); };
  label.fontSize = 20;

  View button;
  button.style.width = Size::pixel(140);
  button.style.height = Size::pixel(44);
  button.style.borderRadius = 6;
  button.style.backgroundColor = {230, 230, 230};
  button.style.hoverColor = {210, 210, 210};
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::Center;
  button.onClick = [&]() {
    count++;
  };

  Text buttonLabel;
  buttonLabel.label = "Increment";
  button.addChild(buttonLabel);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.gap = 16;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(label);
  root.addChild(button);

  LiteUI ui(400, 200, "Counter");
  ui.setRoot(root);
  ui.run();
  return 0;
}