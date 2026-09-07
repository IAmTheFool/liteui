#include "liteui.hpp"
#include <algorithm>
#include <cstdio>
#include <iostream>

int main() {
  float sliderValue = 0.4f; // 0..1, the single source of truth
  float trackWidth = 0;     // captured live via track.onLayout below
  constexpr float kThumbSize = 18.0f;

  // --- label showing the current percentage ---
  Text valueLabel;
  valueLabel.label = [&]() {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%",
                  static_cast<int>(sliderValue * 100 + 0.5f));
    return std::string(buf);
  };
  valueLabel.fontSize = 14;
  valueLabel.color = {90, 90, 90};

  // --- track: the clickable strip the thumb rides along ---
  View track;
  track.style.height = Size::pixel(6);
  track.style.width = Size::full();
  track.style.backgroundColor = Color{225, 225, 225};
  track.style.borderRadius = 3.0f;
  track.onLayout = [&](float x, float y, float w, float h) { trackWidth = w; };
  auto updateFromLocalX = [&](float localX) {
    if (trackWidth > 0)
      sliderValue = std::clamp(localX / trackWidth, 0.0f, 1.0f);
  };
  // Initial click still jumps the thumb there immediately...
  track.onPressAt = [&](float localX, float) {
    std::cout << "Pressed at" << std::endl;
    updateFromLocalX(localX);
  };
  // ...and now the drag itself tracks the pointer continuously too.
  track.onDragTo = [&](float localX, float) { updateFromLocalX(localX); };

  // --- fill: an ordinary flow child of track, width driven by
  // valueSource (0..1 -> percentage), same mechanism this file already
  // uses for progress bars. ---
  View fill;
  fill.style.height = Size::full();
  fill.style.backgroundColor = Color{60, 120, 235};
  fill.style.borderRadius = 3.0f;
  fill.style.width = [&]() { return Size::pixel(sliderValue * trackWidth); };
  track.addChild(fill);

  // --- thumb: Position::Absolute so it isn't squeezed by track's own
  // flex layout; positionSource re-centers it on sliderValue every
  // relayout, same idiom as the earlier slider-thumb comment in
  // View::positionSource's own doc. ---
  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.top = -6.0f; // (18 - 6) / 2, centers it vertically on the track
  thumb.style.width = Size::pixel(kThumbSize);
  thumb.style.height = Size::pixel(kThumbSize);
  thumb.style.backgroundColor =Color {255, 255, 255};
  thumb.style.borderWidth = 2.0f;
  thumb.style.borderColor =Color {60, 120, 235};
  thumb.style.borderRadius = kThumbSize / 2;
  thumb.style.left = [&]() {
    return sliderValue * trackWidth - kThumbSize / 2;
  };

  track.addChild(thumb);

  // --- wrapper: gives the track some breathing room + the label below ---
  View sliderRow;
  sliderRow.style.direction = FlexDirection::Column;
  sliderRow.style.width = Size::pixel(240);
  sliderRow.style.gap = 10;
  sliderRow.addChild(track);
  sliderRow.addChild(valueLabel);

  // --- root ---
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(sliderRow);

  LiteUI ui(320, 200, "Slider");
  ui.setRoot(root);
  ui.run();
  return 0;
}