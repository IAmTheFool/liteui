#include "liteui.hpp"

int main() {
  bool menuOpen = false;

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
  trigger.addChild(triggerLabel);

  // --- backdrop: full-window, catches "click outside to close" ---
  // Caveat: disabledSource only stops onClick from firing when closed;
  // hitTestFlow still finds this node (it only checks "has onClick", not
  // "enabled") and it still physically covers the window, so anything
  // underneath is unclickable even while the menu is closed. Needs a
  // visibleSource-style skip in hitTestFlow/paint to fix properly.
  View backdrop;
  backdrop.style.position = Position::Absolute;
  backdrop.style.left = 0;
  backdrop.style.top = 0;
  backdrop.style.right = 0;
  backdrop.style.bottom = 0;
  backdrop.style.zIndex = 100;
  backdrop.style.backgroundColor = {255, 255, 255,2};
  backdrop.disabledSource = [&]() { return !menuOpen; };
  backdrop.onClick = [&]() { menuOpen = false; };

  // --- menu itself: slid off-screen via positionSource when closed ---
  View menu;
  menu.style.position = Position::Absolute;
  menu.style.top = 60;
  menu.style.width = Size::pixel(160);
  menu.style.backgroundColor = {255, 255, 255};
  menu.style.borderWidth = 1;
  menu.style.borderColor = {200, 200, 200};
  menu.style.borderRadius = 6;
  menu.style.zIndex = 101;
  menu.positionSource = [&]() { return menuOpen ? 100.0f : -9999.0f; };

  Text deleteItem;
  deleteItem.label = "Delete";
  deleteItem.style.padding = EdgeInsets::all(10);

  View deleteRow;
  deleteRow.style.hoverColor = {245, 245, 245};
  deleteRow.onClick = [&]() {
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
    menuOpen = false;
    // ... actual rename logic here ...
  };
  renameRow.addChild(renameItem);

  menu.addChild(renameRow);
  menu.addChild(deleteRow);

  // --- root ---
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(trigger);
  root.addChild(backdrop);
  root.addChild(menu);

  LiteUI ui(400, 300, "Context Menu");
  ui.setRoot(root);
  ui.run();
  return 0;
}