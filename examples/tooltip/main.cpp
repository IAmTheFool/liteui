#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"
#include "topbar.hpp"

#include <memory>
#include <string>

// Shared by every handler. Views are copied by addChild, so the state lives
// in a shared_ptr that each lambda captures.
struct TipState {
  bool visible = false;
  std::string text;
  float x = 0, y = 0;     // window coordinates
  int timer = -1;         // pending "show after delay" interval, -1 = none
};

// Builds the tooltip bubble. Add it as the LAST child of the root.
View makeTooltipView(std::shared_ptr<TipState> st) {
  View tip;
  tip.style.position = Position::Absolute;
  tip.style.zIndex = 100;
  tip.style.backgroundColor = Color{30, 30, 30, 240};
  tip.style.borderColor = Color{90, 90, 90};      // border: not possible
  tip.style.borderWidth = 1.0f;                   // with the built-in tooltip
  tip.style.borderRadius = 6.0f;
  tip.style.padding = EdgeInsets{6, 10, 6, 10};   // top, right, bottom, left

  // Follow the pointer; hidden unless st->visible.
  tip.style.left = std::function<float()>([st] { return st->x + 12; });
  tip.style.top = std::function<float()>([st] { return st->y + 18; });
  tip.style.display = std::function<Display()>(
      [st] { return st->visible ? Display::Flex : Display::None; });

  Text label;
  label.label = std::function<std::string()>([st] { return st->text; });
  label.fontSize = 13;
  label.color = Color{255, 255, 255};
  label.wrap = TextWrap::Wrap;                    // multi-line works too
  tip.addChild(label);
  return tip;
}

// Gives `target` a tooltip: shows after `delayMs`, follows the pointer,
// hides on leave or press.
void attachTooltip(View &target, std::shared_ptr<TipState> st,
                   std::string text, int delayMs = 400) {
  auto hide = [st] {
    if (st->timer >= 0) {
      LiteUI::current()->removeInterval(st->timer);
      st->timer = -1;
    }
    st->visible = false;
  };

  target.onPointerEnter = [st, text, delayMs, hide](PointerEvent &e) {
    hide();
    st->text = text;
    st->x = e.windowX;
    st->y = e.windowY;
    st->timer = LiteUI::current()->addInterval(delayMs, [st] {
      st->visible = true;
      LiteUI::current()->removeInterval(st->timer); // intervals repeat
      st->timer = -1;
    });
  };
  target.onPointerMove = [st](PointerEvent &e) {
    st->x = e.windowX;
    st->y = e.windowY;
  };
  target.onPointerLeave = [hide](PointerEvent &) { hide(); };
  target.onPointerDown = [hide](PointerEvent &) { hide(); };
}

int main() {
  auto tipState = std::make_shared<TipState>();

  View save;
  save.style.width = Size::pixel(120);
  save.style.height = Size::pixel(36);
  save.style.backgroundColor = Color{70, 130, 220};
  attachTooltip(save, tipState, "Save the file\nShortcut: Ctrl+S");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(makeTopBar("Tooltip demo")); // window chrome is ours to draw
  root.addChild(std::move(save));
  root.addChild(makeTooltipView(tipState)); // last, so it draws on top

  LiteUI ui("Tooltip demo", 600, 400);
  ui.setRoot(root);
  ui.run();
}