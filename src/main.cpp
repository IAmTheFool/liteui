#include "liteui.hpp"

int main() {
  int count = 0;
  float progress = 0.35f;
  bool checked = false;
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
  button.disabledSource = [&]() { return checked; };
  button.onClick = [&]() {
    count++;
    progress += 0.1;
  };

  Text buttonLabel;
  buttonLabel.label = "Increment";
  button.addChild(buttonLabel);

  View box;
  box.style.width = Size::pixel(20);
  box.style.height = Size::pixel(20);
  box.style.borderWidth = 2;
  box.style.borderColor = {120, 120, 120};
  box.style.borderRadius = 4;
  box.backgroundColorSource = [&]() {
    return checked ? Color{60, 130, 246} : Color{255, 255, 255};
  };
  box.onClick = [&]() { checked = !checked; };

  Text mark;
  mark.color = {255, 255, 255};
  mark.source = [&]() {
    return checked ? std::string("\xE2\x9C\x93") : std::string();
  };
  box.addChild(mark);

  View track;
  track.style.width = Size::full();
  track.style.height = Size::pixel(8);
  track.style.backgroundColor = {230, 230, 230};
  track.style.borderRadius = 4;

  View fill;
  fill.style.height = Size::full();
  fill.style.backgroundColor = {60, 130, 246};
  fill.style.borderRadius = 4;
  fill.valueSource = [&]() { return progress; };
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