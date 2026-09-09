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



  Image car;
  car.style.width = Size::pixel(400);
  car.style.height = Size::pixel(260);
  car.path = "car.jpg";
  car.fit = ObjectFit::Contain;
  car.backgroundColor = Color{240, 240, 240};
  car.onError = [](const std::string &err) {
    fprintf(stderr, "image load failed: %s\n", err.c_str());
  };
  root.addChild(std::move(car));

  ui.setRoot(std::move(root));
  ui.run();
}