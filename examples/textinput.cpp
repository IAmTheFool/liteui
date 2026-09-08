#include "liteui.hpp"
#include <string>

struct AppState {
  std::string typed;
  std::string lastKeyInfo = "No key pressed yet";
  std::string status = "Click the box below, then type";
  bool fieldFocused = false;
};

// Turns a Key enum value into a display string. Only covers what the demo
// can actually produce, but written as a full switch so it's easy to
// extend later.
std::string keyName(Key k) {
  if (k >= Key::A && k <= Key::Z)
    return std::string(1,
                       'A' + (static_cast<int>(k) - static_cast<int>(Key::A)));
  if (k >= Key::N0 && k <= Key::N9)
    return std::string(1,
                       '0' + (static_cast<int>(k) - static_cast<int>(Key::N0)));
  switch (k) {
  case Key::Enter:
    return "Enter";
  case Key::Escape:
    return "Escape";
  case Key::Backspace:
    return "Backspace";
  case Key::Tab:
    return "Tab";
  case Key::Space:
    return "Space";
  case Key::Delete:
    return "Delete";
  case Key::Left:
    return "Left";
  case Key::Right:
    return "Right";
  case Key::Up:
    return "Up";
  case Key::Down:
    return "Down";
  default:
    return "?";
  }
}

int main() {
  AppState state;

  LiteUI ui(520, 380, "Keyboard Input Test");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.padding = EdgeInsets::all(20);
  root.style.gap = 12;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{245, 245, 245};

  Text title;
  title.label = std::string("Keyboard Input Test");
  title.fontSize = 20;
  title.fontWeight = FontWeight::Bold;
  root.addChild(title);

  Text instructions;
  instructions.label = [&state] { return state.status; };
  instructions.fontSize = 14;
  instructions.color = Color{80, 80, 80};
  root.addChild(instructions);

  // ---- the focusable text field ----
  View field;
  field.style.width = Size::pixel(420);
  field.style.height = Size::pixel(40);
  field.style.padding = EdgeInsets::all(8);
  field.style.borderWidth = 2.0f;
  field.style.borderColor = [&state] {
    return state.fieldFocused ? Color{50, 120, 220} : Color{180, 180, 180};
  };
  field.style.backgroundColor = Color{255, 255, 255};
  field.focusable = true;

  // Required so hitTest() can actually find this view on click — see the
  // note above the example. Without a click/press handler, hitTestFlow
  // never returns it, so it can never receive focus.
  field.onPressAt = [](float, float) {};

  field.onFocus = [&state] {
    state.fieldFocused = true;
    state.status = "Field focused - start typing";
  };
  field.onBlur = [&state] {
    state.fieldFocused = false;
    state.status = "Field blurred - click it to focus again";
  };

  field.onTextInput = [&state](uint32_t cp) {
    if (cp >= 0x20 && cp < 128) // printable ASCII only, for this demo
      state.typed += static_cast<char>(cp);
  };
  field.onKeyDown = [&state](KeyEvent e) {
    if (e.key == Key::Backspace && !state.typed.empty())
      state.typed.pop_back();
    else if (e.key == Key::Enter)
      state.typed.clear();

    std::string mods;
    if (e.mods.ctrl)
      mods += "Ctrl+";
    if (e.mods.shift)
      mods += "Shift+";
    if (e.mods.alt)
      mods += "Alt+";
    state.lastKeyInfo = "Last key: " + mods + keyName(e.key);
  };

  Text fieldText;
  fieldText.label = [&state] {
    return state.typed.empty() ? std::string("(click here and type)")
                               : state.typed;
  };
  fieldText.fontSize = 16;
  field.addChild(fieldText);

  root.addChild(field);

  Text keyInfo;
  keyInfo.label = [&state] { return state.lastKeyInfo; };
  keyInfo.fontSize = 13;
  keyInfo.color = Color{100, 100, 100};
  root.addChild(keyInfo);

  Text shortcutHint;
  shortcutHint.label = std::string(
      "Press Ctrl+S anywhere — global shortcut, works even without focus");
  shortcutHint.fontSize = 13;
  shortcutHint.color = Color{100, 100, 100};
  root.addChild(shortcutHint);

  // ---- global shortcut: fires regardless of what's focused ----
  ui.addShortcut({.ctrl = true}, Key::S, [&state] {
    state.status = "Ctrl+S pressed! (global shortcut fired)";
  });

  ui.setRoot(std::move(root));
  ui.run();
}