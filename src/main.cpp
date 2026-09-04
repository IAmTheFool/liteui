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

  // --- menu itself: still Position::Absolute, still display:none when
  // closed — just resolved against backdrop's content box now instead of
  // root's (they're numerically the same box, since backdrop fills the
  // window). Built fully BEFORE it's nested into backdrop below — addChild
  // takes View by value and moves it in, so anything added to `menu` after
  // that point would silently vanish (it'd land on this now-orphaned local,
  // not on the copy backdrop is holding).
  View menu;
  menu.style.position = Position::Absolute;
  menu.style.top = 60;
  menu.style.width = Size::pixel(160);
  menu.style.backgroundColor = {255, 255, 255};
  menu.style.borderWidth = 1;
  menu.style.borderColor = {200, 200, 200};
  menu.style.borderRadius = 6;

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

  menu.addChild(renameRow);   // menu is fully built ...
  menu.addChild(deleteRow);   // ... before it goes anywhere else

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
  backdrop.addChild(menu);   // menu inherits backdrop's display gating —
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