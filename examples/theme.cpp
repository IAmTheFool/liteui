#include "liteui.hpp"

int main() {
  enum class Theme { Light, Dark, Sepia };
  Theme theme = Theme::Light;

  auto themeName = [&]() {
    switch (theme) {
      case Theme::Light: return "Light mode";
      case Theme::Dark:  return "Dark mode";
      case Theme::Sepia: return "Sepia mode";
    }
    return "";
  };

  auto bgColor = [&]() {
    switch (theme) {
      case Theme::Light: return Color{245, 245, 245};
      case Theme::Dark:  return Color{30, 30, 30};
      case Theme::Sepia: return Color{240, 224, 194};
    }
    return Color{};
  };
  auto textColor = [&]() {
    switch (theme) {
      case Theme::Light: return Color{20, 20, 20};
      case Theme::Dark:  return Color{230, 230, 230};
      case Theme::Sepia: return Color{75, 55, 35};
    }
    return Color{};
  };
  auto cardColor = [&]() {
    switch (theme) {
      case Theme::Light: return Color{255, 255, 255};
      case Theme::Dark:  return Color{50, 50, 50};
      case Theme::Sepia: return Color{232, 212, 178};
    }
    return Color{};
  };
  auto borderColor = [&]() {
    switch (theme) {
      case Theme::Light: return Color{210, 210, 210};
      case Theme::Dark:  return Color{80, 80, 80};
      case Theme::Sepia: return Color{194, 168, 128};
    }
    return Color{};
  };

  Text label;
  label.label = [&]() { return std::string(themeName()); };
  label.fontSize = 20;
  label.color = textColor;

  Text buttonLabel;
  buttonLabel.label = "Cycle theme";
  buttonLabel.color = textColor;

  View button;
  button.style.width = Size::pixel(160);
  button.style.height = Size::pixel(44);
  button.style.borderRadius = 6.0f;
  button.style.borderWidth = 1.0f;
  button.style.borderColor = borderColor;
  button.style.backgroundColor = cardColor;
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::Center;
  button.onClick = [&]() {
    theme = static_cast<Theme>((static_cast<int>(theme) + 1) % 3);
  };
  button.addChild(buttonLabel);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.gap = 16;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = bgColor;
  root.addChild(label);
  root.addChild(button);

  LiteUI ui("Theme Toggle", 400, 200);
  ui.setRoot(root);
  ui.run();
  return 0;
}