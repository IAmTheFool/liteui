#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "counter_model.hpp"

int main() {
  CounterModel model;

  Text label;
  label.label = [&]() { return "Count: " + std::to_string(model.value); };
  label.fontSize = 20;

  View button;
  button.style.width = Size::pixel(140);
  button.style.height = Size::pixel(44);
  button.style.borderRadius = 6.0f;
  button.style.backgroundColor = Color{230, 230, 230};
  button.style.hoverColor = {210, 210, 210};
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::Center;
  button.style.cursor = Cursor::Pointer;

  // Left click. The PointerEvent form bubbles to ancestors unless the
  // handler calls e.stopPropagation(); the old `[&] { ... }` form still
  // works and ends bubbling after it runs.
  button.onClick = [&](PointerEvent &e) {
    int step = e.mods.shift ? 10 : 1;
    for (int i = 0; i < step; ++i)
      model.increment();
    e.stopPropagation(); // nothing above the button needs this click
  };

  // Right click (fired on release). Middle click would be onAuxClick.
  button.onContextMenu = [&](PointerEvent &e) {
    model.value -= 1;
    e.stopPropagation();
  };

  // Wheel over the button. Without stopPropagation() the wheel would fall
  // through to the built-in overflow scrolling.
  button.onWheel = [&](WheelEvent &e) {
    model.value += (e.dy < 0) ? 1 : -1; // dy < 0 = wheel up
    e.stopPropagation();
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

  LiteUI ui("Counter", 400, 200);
  ui.setRoot(root);
  ui.run(); // blocks, pumps the platform event loop
  return 0;
}