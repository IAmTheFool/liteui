#include "liteui.hpp"
#include <iostream>
int main() {
  bool menuOpen = false;

  // Captured live from onLayout below — the real, current on-screen
  // position of the trigger button and the backdrop it's measured
  // against. Starts at 0 and gets filled in on the very first relayout.
  float triggerX = 0, triggerY = 0, triggerH = 0;
  float backdropX = 0, backdropY = 0;

  // --- trigger button ---
  Text triggerLabel;
  triggerLabel.label = "Options";

  View trigger;
  trigger.style.width = Size::pixel(100);
  trigger.style.height = Size::pixel(36);
  trigger.style.backgroundColor = {230, 230, 230};
  trigger.style.hoverColor = {210, 210, 210};
  trigger.style.borderRadius = 4;
  trigger.style.alignItems = Align::Center;
  trigger.style.justifyContent = Justify::Center;
  trigger.onClick = [&]() { menuOpen = !menuOpen; };
  trigger.onLayout = [&](float x, float y, float w, float h) {
    triggerX = x;
    triggerY = y;
    triggerH = h;
  };
  trigger.addChild(triggerLabel);

  View menu;
  menu.style.position = Position::Absolute;
  menu.style.direction = FlexDirection::Column;

  menu.style.width = Size::pixel(160);
  menu.style.backgroundColor = {255, 255, 255};
  menu.style.borderWidth = 1;
  menu.style.borderColor = {200, 200, 200};
  menu.style.borderRadius = 6;
  menu.style.zIndex = 200;
  // Anchor menu's top-left corner to just below the trigger button's
  // bottom-left corner, expressed relative to backdrop's own origin
  // (since that's what Absolute positioning here actually resolves
  // against).
  menu.positionSource = [&]() { return triggerX - backdropX; };
  menu.topSource = [&]() {
    constexpr float kGap = 6.0f; // small visual gap under the button
    return (triggerY - backdropY) + triggerH + kGap;
  };

  Text deleteItem;
  deleteItem.label = "Delete";
  deleteItem.style.padding = EdgeInsets::all(10);

  View deleteRow;
  deleteRow.style.hoverColor = {245, 245, 245};
  deleteRow.onClick = [&]() {
    std::cout << "Delete clicked" << std::endl;
    menuOpen = false;
    // ... actual delete logic here ...
  };
  deleteRow.addChild(deleteItem);

  Text renameItem;
  renameItem.label = "Rename";
  renameItem.style.padding = EdgeInsets::all(10);

  View renameRow;
  renameRow.style.hoverColor = {245, 245, 245};
  renameRow.onClick = [&]() {
    std::cout << "Rename clicked" << std::endl;
    menuOpen = false;

    // ... actual rename logic here ...
  };
  renameRow.addChild(renameItem);

  menu.addChild(renameRow); // menu is fully built ...
  menu.addChild(deleteRow); // ... before it goes anywhere else

  // --- backdrop: full-window, catches "click outside to close", and now
  // owns menu directly. Its own displaySource gates both of them at once —
  // collectAbsolutes still finds menu as its own top-level absolute entry
  // (it recurses into every node's children regardless of the node's own
  // position/display), so z-ordering and paint-once semantics are
  // unaffected by the nesting; only backdrop's displaySource needs setting.
  View backdrop;
  backdrop.style.position = Position::Absolute;
  backdrop.style.left = 0;
  backdrop.style.top = 0;
  backdrop.style.right = 0;
  backdrop.style.bottom = 0;
  backdrop.style.zIndex = 100;
  backdrop.style.backgroundColor = {255, 255, 255, 2};
  backdrop.displaySource = [&]() { return menuOpen; };
  backdrop.onClick = [&]() { menuOpen = false; };
  backdrop.onLayout = [&](float x, float y, float w, float h) {
    backdropX = x;
    backdropY = y;
  };
  backdrop.addChild(menu); // menu inherits backdrop's display gating —
                           // no separate displaySource needed on menu

  // --- root ---
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(trigger);
  root.addChild(backdrop);

  LiteUI ui(400, 300, "Context Menu");
  ui.setRoot(root);
  ui.run();
  return 0;
}