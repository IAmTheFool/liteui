#include "liteui.hpp"
#include <iostream>
int main() {
  // Create the window first so the bar's buttons can talk to it.
  // 4th argument = hideTitlebar: no default titlebar, we draw our own.
  LiteUI ui("Custom Bar", 900, 600, true);

  const Color barBg{32, 32, 36};
  const Color white{235, 235, 235};

  // One reusable factory for the three square buttons.
  auto barButton = [&](const std::string &svgString, Color hover,
                       std::function<void()> onClick) {
    View b;
    b.style.width = Size::pixel(46);    // height stretches to the bar's
    b.style.alignItems = Align::Center; // center the glyph both ways
    b.style.justifyContent = Justify::Center;
    b.style.hoverColor = hover;
    b.onClick = std::move(onClick);

    Svg icon;
    icon.source = svgString;
    icon.style.width = Size::pixel(14);
    icon.style.height = Size::pixel(14);
    icon.fit = ObjectFit::Contain; // default anyway for Svg

    icon.onError = [](const std::string &err) {
      // fires if liteui_svg::parseString() fails
      fprintf(stderr, "SVG parse error: %s\n", err.c_str());
    };

    b.addChild(icon);
    return b;
  };
  // Left side: the title. It grows to fill the free space and doubles
  // as the drag handle.
  Text title;
  title.label = "Custom Bar";
  title.fontSize = 13;
  title.color = white;

  View dragArea;
  dragArea.style.flexGrow = 1;
  dragArea.style.alignItems = Align::Center; // vertically center the title
  dragArea.style.padding = EdgeInsets{0, 0, 0, 12}; // top, right, bottom, left
  dragArea.onPressAt = [&](float, float) {
    std::cout << "Moving" << std::endl;
    ui.requestMove();
  };
  dragArea.addChild(title);

  // The bar itself: title area, then minimize / maximize / close.
  View bar;
  bar.style.width = Size::full();
  bar.style.height = Size::pixel(36);
  bar.style.backgroundColor = barBg;
  bar.addChild(dragArea);
  bar.addChild(barButton(
      R"svg(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 -12 32 32" version="1.1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns:sketch="http://www.bohemiancoding.com/sketch/ns">
    
    <title>minus</title>
    <desc>Created with Sketch Beta.</desc>
    <defs>

</defs>
    <g id="Page-1" stroke="none" stroke-width="1" fill="none" fill-rule="evenodd" sketch:type="MSPage">
        <g id="Icon-Set-Filled" sketch:type="MSLayerGroup" transform="translate(-414.000000, -1049.000000)" fill="#000000">
            <path d="M442,1049 L418,1049 C415.791,1049 414,1050.79 414,1053 C414,1055.21 415.791,1057 418,1057 L442,1057 C444.209,1057 446,1055.21 446,1053 C446,1050.79 444.209,1049 442,1049" id="minus" sketch:type="MSShapeGroup">

</path>
        </g>
    </g>
</svg>)svg",
      Color{70, 70, 76}, [&] { ui.requestMinimize(); }));
  bar.addChild(barButton(
      R"svg(<?xml version="1.0" encoding="utf-8"?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
<path fill-rule="evenodd" clip-rule="evenodd" d="M22 5C22 3.34315 20.6569 2 19 2H5C3.34315 2 2 3.34315 2 5V19C2 20.6569 3.34315 22 5 22H19C20.6569 22 22 20.6569 22 19V5ZM20 5C20 4.44772 19.5523 4 19 4H5C4.44772 4 4 4.44772 4 5V19C4 19.5523 4.44772 20 5 20H19C19.5523 20 20 19.5523 20 19V5Z" fill="#0F0F0F"/>
</svg>)svg",
      Color{70, 70, 76}, [&] { ui.requestMaximize(); }));
  bar.addChild(barButton(
      R"svg(<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 -0.5 21 21" version="1.1" xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink">
    
    <title>close [#1511]</title>
    <desc>Created with Sketch.</desc>
    <defs>

</defs>
    <g id="Page-1" stroke="none" stroke-width="1" fill="none" fill-rule="evenodd">
        <g id="Dribbble-Light-Preview" transform="translate(-419.000000, -240.000000)" fill="#000000">
            <g id="icons" transform="translate(56.000000, 160.000000)">
                <polygon id="close-[#1511]" points="375.0183 90 384 98.554 382.48065 100 373.5 91.446 364.5183 100 363 98.554 371.98065 90 363 81.446 364.5183 80 373.5 88.554 382.48065 80 384 81.446">

</polygon>
            </g>
        </g>
    </g>
</svg>)svg",
      Color{196, 43, 28}, [&] { ui.requestClose(); }));

  // Everything below the bar.
  Text hello;
  hello.label = "Window content goes here";
  hello.fontSize = 20;

  View content;
  content.style.flexGrow = 1; // take all the space under the bar
  content.style.alignItems = Align::Center;
  content.style.justifyContent = Justify::Center;
  content.style.backgroundColor = Color{245, 245, 245};
  content.addChild(hello);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(bar);
  root.addChild(content);

  ui.setRoot(root);
  ui.run();
  return 0;
}