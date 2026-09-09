#include "liteui.hpp"
#include <string>

int main() {
  LiteUI ui(480, 320, "Text Field Test");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.padding = EdgeInsets::all(20);
  root.style.gap = 12;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{245, 245, 245};

  Text title;
  title.label = std::string("Click the box, type, use arrow keys");
  title.fontSize = 15;
  title.color = Color{80, 80, 80};
  root.addChild(title);

  TextInput field;
  field.style.width = Size::pixel(240);
  field.style.height = Size::pixel(34);
  field.style.borderWidth = 1.5f;
  field.text = "Hello";
  field.placeholder = "Type something...";
  field.onChange = [](const std::string &s) {
    // e.g. update some other UI, validate, etc.
  };
  field.onSubmit = [](const std::string &s) {
    // fires on Enter
  };
  root.addChild(field);

  Text hint;
  hint.label =
      std::string("Left/Right/Home/End move the caret; Backspace/Delete edit");
  hint.fontSize = 13;
  hint.color = Color{120, 120, 120};
  root.addChild(hint);

  ui.setRoot(std::move(root));
  ui.run();
}