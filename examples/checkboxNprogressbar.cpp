#include "liteui.hpp"

int main() {
  int count = 0;
  float progress = 1.0f;
  bool checked = false;
  Text label;
  label.label = [&]() { return "Count: " + std::to_string(count); };
  label.fontSize = 20;

  View button;
  button.style.width = Size::pixel(140);
  button.style.height = Size::pixel(44);
  button.style.borderRadius = 6.0f;
  button.style.backgroundColor = Color{230, 230, 230};
  button.style.hoverColor = {210, 210, 210};
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::Center;
  button.disabled = [&]() { return checked; };
  button.onClick = [&]() {
    count++;
    progress += 1.0;
  };

  Text buttonLabel;
  buttonLabel.label = "Increment";
  button.addChild(buttonLabel);

  View box;
  box.style.width = Size::pixel(20);
  box.style.height = Size::pixel(20);
  box.style.borderWidth = 2.0f;
  box.style.borderColor = Color{120, 120, 120};
  box.style.borderRadius = 4.0f;
  box.style.backgroundColor = [&]() {
    return checked ? Color{60, 130, 246} : Color{255, 255, 255};
  };
  box.onClick = [&]() { checked = !checked; };

  Text mark;
  mark.color = {255, 255, 255};
  mark.label = [&]() {
    return checked ? std::string("\xE2\x9C\x93") : std::string();
  };
  box.addChild(mark);

  View track;
  track.style.width = Size::full();
  track.style.height = Size::pixel(8);
  track.style.backgroundColor = Color{230, 230, 230};
  track.style.borderRadius = 4.0f;

  View fill;
  fill.style.height = Size::full();
  fill.style.backgroundColor = Color{60, 130, 246};
  fill.style.borderRadius = 4.0f;
  fill.style.width = [&]() { return Size::pixel(progress); };
  track.addChild(fill);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.gap = 16;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(label);
  root.addChild(button);
  root.addChild(box);
  root.addChild(track);

  LiteUI ui(400, 200, "Counter");
  ui.setRoot(root);
  ui.run();
  return 0;
}