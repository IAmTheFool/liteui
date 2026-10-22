
#include "liteui.hpp"

int main() {
  LiteUI ui("Paint",900, 650);
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignContent = AlignContent::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{235, 235, 235, 255};

  View icon;
  icon.addChild(Svg{
      .style = {.width = Size::pixel(64), .height = Size::pixel(64)},
      .path = "chevron.svg",
      .fit = ObjectFit::Contain,
  });
  root.addChild(std::move(icon));

  View iconTwo;
  iconTwo.addChild(Svg{
      .style = {.width = Size::pixel(64), .height = Size::pixel(64)},
      .path = "settings-gear-svgrepo-com.svg",
      .fit = ObjectFit::Contain,
  });
  root.addChild(std::move(iconTwo));

  Svg logo;
  logo.source =
      R"(<?xml version="1.0" encoding="utf-8"?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 0 48 48" xmlns="http://www.w3.org/2000/svg"><defs><style>.a{fill:none;stroke:#000000;stroke-linecap:round;stroke-linejoin:round;}</style></defs><path class="a" d="M41.6783,13.0436H24.77c-1.9628-.1072-5.9311-4.2372-8.1881-4.2372H6.6806V8.8046A2.1762,2.1762,0,0,0,4.5,10.9763v7.3063h39V14.8652A1.8217,1.8217,0,0,0,41.6783,13.0436Z"/><path class="a" d="M43.5,18.2826H4.5V37.0165a2.1762,2.1762,0,0,0,2.1735,2.1789H41.3194A2.1762,2.1762,0,0,0,43.5,37.0237V18.2826Z"/></svg>)";
  logo.style.width = Size::pixel(48);
  logo.style.height = Size::pixel(48);
  logo.fit = ObjectFit::Contain; // default anyway for Svg
  logo.onError = [](const std::string &err) {
    // fires if liteui_svg::parseString() fails
    fprintf(stderr, "SVG parse error: %s\n", err.c_str());
  };

  root.addChild(std::move(logo));

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}