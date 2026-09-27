
#include "liteui.hpp"
#include <algorithm>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace demo {

// ---------------- shared visual constants ----------------
constexpr Color kBorderGray{200, 200, 200};
constexpr Color kTextGray{90, 90, 90};
constexpr Color kAccent{60, 120, 235};
constexpr Color kAccentSoft{225, 235, 252};
constexpr Color kPanelBg{250, 250, 250};
constexpr Color kTransparent{0, 0, 0, 0};

inline View buildCheckbox(bool &checked, const std::string &labelText) {
  View row;
  row.style.direction = FlexDirection::Row;
  row.style.alignItems = Align::Center;
  row.style.gap = 10;
  row.onClick = [&checked]() { checked = !checked; };

  View box;
  box.style.width = Size::pixel(20);
  box.style.height = Size::pixel(20);
  box.style.borderWidth = 2.0f;
  box.style.borderRadius = 4.0f;
  box.style.alignItems = Align::Center;
  box.style.justifyContent = Justify::Center;
  box.style.borderColor = [&checked]() -> Color {
    return checked ? kAccent : kBorderGray;
  };
  box.style.backgroundColor = [&checked]() -> Color {
    return checked ? kAccent : Color{255, 255, 255};
  };

  Text mark;
  mark.color = Color{255, 255, 255};
  mark.fontSize = 13;
  mark.label = [&checked]() -> std::string {
    return checked ? std::string("\xE2\x9C\x93") : std::string();
  };
  box.addChild(mark);

  Text label;
  label.label = labelText;
  label.fontSize = 15;

  row.addChild(box);
  row.addChild(label);
  return row;
}

// ================= Radio group =================
struct RadioOption {
  std::string id;
  std::string label;
};

inline View buildRadioGroup(const std::vector<RadioOption> &options,
                            std::string &selected) {
  View group;
  group.style.direction = FlexDirection::Column;
  group.style.gap = 10;

  for (const auto &opt : options) {
    std::string id = opt.id; // own copy per lambda closure

    View row;
    row.style.direction = FlexDirection::Row;
    row.style.alignItems = Align::Center;
    row.style.gap = 10;
    row.onClick = [&selected, id]() { selected = id; };

    View outer;
    outer.style.width = Size::pixel(18);
    outer.style.height = Size::pixel(18);
    outer.style.borderRadius = 9.0f;
    outer.style.borderWidth = 2.0f;
    outer.style.borderColor = [&selected, id]() -> Color {
      return selected == id ? kAccent : kBorderGray;
    };
    outer.style.alignItems = Align::Center;
    outer.style.justifyContent = Justify::Center;

    View dot;
    dot.style.width = Size::pixel(8);
    dot.style.height = Size::pixel(8);
    dot.style.borderRadius = 4.0f;
    dot.style.backgroundColor = kAccent;
    dot.style.display = [&selected, id]() -> Display {
      return selected == id ? Display::Flex : Display::None;
    };
    outer.addChild(dot);

    Text label;
    label.label = opt.label;
    label.fontSize = 15;

    row.addChild(outer);
    row.addChild(label);
    group.addChild(row);
  }
  return group;
}

inline View buildToggle(bool &value) {
  constexpr float kTrackW = 44, kTrackH = 24, kThumb = 20;
  constexpr float kPad = (kTrackH - kThumb) / 2.0f;

  View track;
  track.style.width = Size::pixel(kTrackW);
  track.style.height = Size::pixel(kTrackH);
  track.style.borderRadius = kTrackH / 2.0f;
  track.style.backgroundColor = [&value]() -> Color {
    return value ? kAccent : Color{210, 210, 210};
  };
  track.onClick = [&value]() { value = !value; };

  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.top = kPad;
  thumb.style.width = Size::pixel(kThumb);
  thumb.style.height = Size::pixel(kThumb);
  thumb.style.borderRadius = kThumb / 2.0f;
  thumb.style.backgroundColor = Color{255, 255, 255};
  thumb.style.left = [&value]() -> float {
    return value ? kTrackW - kThumb - kPad : kPad;
  };
  track.addChild(thumb);
  return track;
}

inline View buildStepper(int &value, int minV, int maxV) {
  View row;
  row.style.direction = FlexDirection::Row;
  row.style.alignItems = Align::Center;
  row.style.gap = 12;

  auto makeButton = [](const std::string &glyph, std::function<void()> fn) {
    View btn;
    btn.style.width = Size::pixel(28);
    btn.style.height = Size::pixel(28);
    btn.style.borderRadius = 6.0f;
    btn.style.backgroundColor = Color{235, 235, 235};
    btn.style.hoverColor = Color{220, 220, 220};
    btn.style.alignItems = Align::Center;
    btn.style.justifyContent = Justify::Center;
    btn.onClick = std::move(fn);
    Text t;
    t.label = glyph;
    t.fontSize = 16;
    btn.addChild(t);
    return btn;
  };

  row.addChild(
      makeButton("-", [&value, minV]() { value = std::max(minV, value - 1); }));

  View displayBox;
  displayBox.style.width = Size::pixel(32);
  displayBox.style.alignItems = Align::Center;
  displayBox.style.justifyContent = Justify::Center;
  Text display;
  display.label = [&value]() -> std::string { return std::to_string(value); };
  display.fontSize = 16;
  displayBox.addChild(display);
  row.addChild(displayBox);

  row.addChild(
      makeButton("+", [&value, maxV]() { value = std::min(maxV, value + 1); }));

  return row;
}

inline View buildDropdown(const std::vector<std::string> &options,
                          std::string &selected, bool &open) {
  View wrapper; // containing block the absolute list is positioned against
  wrapper.style.direction = FlexDirection::Column;
  wrapper.style.width = Size::pixel(180);

  View button;
  button.style.width = Size::full();
  button.style.height = Size::pixel(36);
  button.style.borderWidth = 1.5f;
  button.style.borderColor = kBorderGray;
  button.style.borderRadius = 6.0f;
  button.style.direction = FlexDirection::Row;
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::SpaceBetween;
  button.style.padding = EdgeInsets{0, 10, 0, 10};
  button.onClick = [&open]() { open = !open; };

  Text current;
  current.label = [&selected]() -> std::string { return selected; };
  current.fontSize = 14;
  button.addChild(current);

  Text arrow;
  arrow.label = [&open]() -> std::string { return open ? "^" : "v"; };
  arrow.fontSize = 12;
  arrow.color = kTextGray;
  button.addChild(arrow);

  wrapper.addChild(button);

  View list;
  list.style.position = Position::Absolute;
  list.style.top = 40.0f; // just below the 36px button, with a small gap
  list.style.left = 0.0f;
  list.style.width = Size::full();
  list.style.direction = FlexDirection::Column;
  list.style.backgroundColor = Color{255, 255, 255};
  list.style.borderWidth = 1.5f;
  list.style.borderColor = kBorderGray;
  list.style.borderRadius = 6.0f;
  list.style.zIndex = 10;
  list.style.display = [&open]() -> Display {
    return open ? Display::Flex : Display::None;
  };

  for (const auto &opt : options) {
    std::string val = opt;
    View item;
    item.style.width = Size::full();
    item.style.height = Size::pixel(32);
    item.style.alignItems = Align::Center;
    item.style.padding = EdgeInsets{0, 10, 0, 10};
    item.style.hoverColor = kAccentSoft;
    item.onClick = [&selected, &open, val]() {
      selected = val;
      open = false;
    };
    Text label;
    label.label = val;
    label.fontSize = 14;
    item.addChild(label);
    list.addChild(item);
  }

  wrapper.addChild(list);
  return wrapper;
}

inline View buildTabs(const std::vector<std::string> &titles,
                      const std::vector<std::function<View()>> &panels,
                      int &activeIndex) {
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::pixel(300);

  View bar;
  bar.style.direction = FlexDirection::Row;
  bar.style.gap = 4;

  for (size_t i = 0; i < titles.size(); ++i) {
    int idx = static_cast<int>(i);
    View tab;
    tab.style.padding = EdgeInsets{8, 14, 8, 14};
    tab.style.borderRadius = 6.0f;
    tab.style.backgroundColor = [&activeIndex, idx]() -> Color {
      return activeIndex == idx ? kAccentSoft : kTransparent;
    };
    tab.onClick = [&activeIndex, idx]() { activeIndex = idx; };

    Text label;
    label.label = titles[i];
    label.fontSize = 14;
    label.color = [&activeIndex, idx]() -> Color {
      return activeIndex == idx ? kAccent : kTextGray;
    };
    tab.addChild(label);
    bar.addChild(tab);
  }
  root.addChild(bar);

  View panelHost;
  panelHost.style.padding = EdgeInsets::all(14);
  panelHost.keysSource = [&activeIndex]() -> std::vector<std::string> {
    return {std::to_string(activeIndex)};
  };
  panelHost.itemBuilder = [&panels](const std::string &key) -> View {
    return panels[static_cast<size_t>(std::stoi(key))]();
  };
  root.addChild(panelHost);

  return root;
}

inline View buildTextField(std::string &value, const std::string &placeholder,
                           float width = 240.0f) {
  TextInput field;
  field.style.width = Size::full();
  field.style.height = Size::pixel(36);
  field.style.borderWidth = 1.5f;
  field.style.borderRadius = 6.0f;
  field.borderColor = kBorderGray;
  field.focusedBorderColor = kAccent;
  field.text = value;
  field.placeholder = placeholder;
  field.onChange = [&value](const std::string &s) { value = s; };

  View wrapper;
  wrapper.style.width = Size::pixel(width);
  wrapper.addChild(field);
  return wrapper;
}

inline View buildSlider(float &value, float min = 0.0f, float max = 100.0f) {
  constexpr float kTrackWidth = 300.0f, kTrackHeight = 6.0f;
  constexpr float kThumbSize = 20.0f;
  constexpr float kMaxThumbX = kTrackWidth - kThumbSize;

  // Maps the current value to a thumb x-position. A plain lambda (not a
  // View callback) so both the fill bar and the thumb can share the math.
  auto thumbXFor = [min, max, kMaxThumbX](float v) {
    return std::clamp((v - min) / (max - min), 0.0f, 1.0f) * kMaxThumbX;
  };

  // Shared so onPressAt and onDragTo -- two separate lambdas -- read and
  // write the same grab offset across the life of one drag gesture.
  auto grabX = std::make_shared<float>(0.0f);

  // Where the fill should start growing from. For a normal 0-based range
  // this is the low end (min), same as before. For a range that straddles
  // zero (e.g. -100..100) it's 0 itself, so the fill grows outward from
  // the center instead of always starting at the left edge.
  const bool isBipolar = (min < 0.0f && max > 0.0f);
  const float anchor = isBipolar ? 0.0f : min;

  View track;
  track.style.width = Size::pixel(kTrackWidth);
  track.style.height = Size::pixel(kTrackHeight);
  track.style.backgroundColor = Color{210, 210, 210};
  track.style.borderRadius = kTrackHeight / 2;

  // Filled portion, absolutely positioned inside the track so it can grow
  // independently of the thumb without disturbing flex layout. Spans from
  // the anchor's thumb-center position to the current value's -- for a
  // normal slider the anchor is at x=0 so this reduces to "fill from the
  // left edge", exactly like before.
  View fill;
  fill.style.position = Position::Absolute;
  fill.style.top = 0.0f;
  fill.style.height = Size::pixel(kTrackHeight);
  fill.style.left = [&value, thumbXFor, kThumbSize, anchor, isBipolar] {
    // At a hard boundary (anchor == min) there's a real track edge to sit
    // flush against, so use the raw, un-offset position (0). At the
    // bipolar zero point there's no physical edge -- only a notional
    // thumb -- so anchor to its center instead, same as the value side.
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumbSize / 2 : thumbXFor(anchor);
    float valueCenter = thumbXFor(value) + kThumbSize / 2;
    return std::min(anchorX, valueCenter);
  };
  fill.style.width = [&value, thumbXFor, kThumbSize, anchor, isBipolar] {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumbSize / 2 : thumbXFor(anchor);
    float valueCenter = thumbXFor(value) + kThumbSize / 2;
    return Size::pixel(std::abs(valueCenter - anchorX));
  };
  fill.style.backgroundColor = Color{80, 140, 220};
  fill.style.borderRadius = kTrackHeight / 2;
  track.addChild(fill);

  // Draggable thumb -- same press/drag pattern as the standalone draggable
  // box: onPressAt remembers where inside the thumb the grab happened,
  // onDragTo moves it (clamped) and writes the result back into `value`.
  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.left = [&value, thumbXFor] { return thumbXFor(value); };
  thumb.style.top =
      -(kThumbSize - kTrackHeight) / 2; // vertically centered on the track
  thumb.style.width = Size::pixel(kThumbSize);
  thumb.style.height = Size::pixel(kThumbSize);
  thumb.style.backgroundColor = Color{255, 255, 255};
  thumb.style.borderWidth = 2.0f;
  thumb.style.borderColor = Color{80, 140, 220};
  thumb.style.borderRadius = kThumbSize / 2;

  thumb.onPressAt = [grabX](float lx, float) { *grabX = lx; };
  thumb.onDragTo = [&value, grabX, thumbXFor, min, max, kMaxThumbX](float lx,
                                                                    float) {
    float newThumbX =
        std::clamp(thumbXFor(value) + (lx - *grabX), 0.0f, kMaxThumbX);
    value = min + (newThumbX / kMaxThumbX) * (max - min);
  };
  track.addChild(thumb);

  return track;
}

struct DialogButton {
  std::string label;
  Color background;
  Color hoverBackground;
  Color textColor = Color{0, 0, 0};
};

inline View buildDialogTrigger(bool &open, const std::string &label,
                               Color background = Color{230, 230, 230},
                               Color hoverBackground = Color{210, 210, 210}) {
  View trigger;
  trigger.style.width = Size::pixel(160);
  trigger.style.height = Size::pixel(36);
  trigger.style.backgroundColor = background;
  trigger.style.hoverColor = hoverBackground;
  trigger.style.borderRadius = 4.0f;
  trigger.style.alignItems = Align::Center;
  trigger.style.justifyContent = Justify::Center;
  trigger.onClick = [&open]() { open = true; };

  Text triggerLabel;
  triggerLabel.label = label;
  trigger.addChild(triggerLabel);
  return trigger;
}

inline View buildConfirmDialog(bool &open, const std::string &title,
                               const std::string &message,
                               const DialogButton &cancelBtn,
                               const DialogButton &confirmBtn,
                               std::function<void()> onConfirm) {
  View dialogBox;
  dialogBox.style.direction = FlexDirection::Column;
  dialogBox.style.width = Size::pixel(320);
  dialogBox.style.padding = EdgeInsets::all(20);
  dialogBox.style.gap = 16;
  dialogBox.style.backgroundColor = Color{255, 255, 255};
  dialogBox.style.borderRadius = 8.0f;
  dialogBox.onClick = [] {};

  Text titleText;
  titleText.label = title;
  titleText.fontSize = 18;
  titleText.fontWeight = FontWeight::SemiBold;

  Text messageText;
  messageText.label = message;
  messageText.wrap = TextWrap::Wrap;
  messageText.style.width = Size::full();
  messageText.color = kTextGray;

  View buttonRow;
  buttonRow.style.direction = FlexDirection::Row;
  buttonRow.style.justifyContent = Justify::End;
  buttonRow.style.gap = 10;

  auto makeDialogButton = [&open](const DialogButton &spec,
                                  std::function<void()> onClick) {
    View btn;
    btn.style.width = Size::pixel(80);
    btn.style.height = Size::pixel(34);
    btn.style.backgroundColor = spec.background;
    btn.style.hoverColor = spec.hoverBackground;
    btn.style.borderRadius = 4.0f;
    btn.style.alignItems = Align::Center;
    btn.style.justifyContent = Justify::Center;
    btn.onClick = std::move(onClick);
    Text label;
    label.label = spec.label;
    label.color = spec.textColor;
    btn.addChild(label);
    return btn;
  };

  buttonRow.addChild(makeDialogButton(cancelBtn, [&open]() { open = false; }));
  buttonRow.addChild(makeDialogButton(confirmBtn, [&open, onConfirm]() {
    if (onConfirm)
      onConfirm();
    open = false;
  }));

  dialogBox.addChild(titleText);
  dialogBox.addChild(messageText);
  dialogBox.addChild(buttonRow);

  View backdrop;
  backdrop.style.position = Position::Absolute;
  backdrop.style.left = 0.0f;
  backdrop.style.top = 0.0f;
  backdrop.style.right = 0.0f;
  backdrop.style.bottom = 0.0f;
  backdrop.style.zIndex = 100;
  backdrop.style.backgroundColor = Color{0, 0, 0, 90};
  backdrop.style.alignItems = Align::Center;
  backdrop.style.justifyContent = Justify::Center;
  backdrop.style.display = [&open]() -> Display {
    return open ? Display::Flex : Display::None;
  };
  backdrop.onClick = [&open]() { open = false; }; // click outside -> cancel
  backdrop.addChild(dialogBox);

  return backdrop;
}

} // namespace demo

int main() {
  using namespace demo;

  // ---- shared widget state; must outlive ui.run()'s blocking loop ----
  std::string nameValue = "Ada";
  bool subscribeValue = false;
  std::string radioChoice = "small";
  bool toggleValue = true;
  int stepperValue = 3;
  float sliderValue = 0.5f;
  std::string dropdownChoice = "Option A";
  bool dropdownOpen = false;
  int activeTab = 0;
  bool deleteDialogOpen = false;

  static float volume = 50.0f;
  static float brightness = 20.0f;
  // Negative-to-positive range: with min/max symmetric around 0 and the
  // value starting at 0, thumbXFor((0 - -100) / (100 - -100)) = 0.5, so
  // the thumb lands exactly in the middle of the track on first render.
  static float balance = 0.0f;

  auto heading = [](const std::string &text) {
    Text t;
    t.label = text;
    t.fontSize = 13;
    t.color = kTextGray;
    return t;
  };

  // Plain Text struct + field assignment, same as the counter example --
  // `label` is a Dynamic<std::string>, so handing it a lambda instead of
  // a plain string makes it re-read every frame, the same polling that
  // keeps style.left/top live during a drag.
  Text volumeLabel;
  volumeLabel.label = [&]() {
    return "Volume: " + std::to_string((int)volume);
  };
  volumeLabel.fontSize = 16;

  Text brightnessLabel;
  brightnessLabel.label = [&]() {
    return "Brightness: " + std::to_string((int)brightness);
  };
  brightnessLabel.fontSize = 16;

  Text balanceLabel;
  balanceLabel.label = [&]() {
    return "Balance: " + std::to_string((int)balance);
  };
  balanceLabel.fontSize = 16;

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.padding = EdgeInsets::all(24);
  root.style.gap = 22;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.overflowY = Overflow::Auto;
  root.style.backgroundColor = kPanelBg;

  root.addChild(heading("Name"));
  root.addChild(buildTextField(nameValue, "Enter your name..."));

  root.addChild(heading("Subscribe"));
  root.addChild(buildCheckbox(subscribeValue, "Email me about updates"));

  root.addChild(heading("Size"));
  root.addChild(buildRadioGroup(
      {{"small", "Small"}, {"medium", "Medium"}, {"large", "Large"}},
      radioChoice));

  root.addChild(heading("Notifications"));
  root.addChild(buildToggle(toggleValue));

  root.addChild(heading("Quantity"));
  root.addChild(buildStepper(stepperValue, 0, 10));

  root.addChild(heading("Volume"));
  root.addChild(buildSlider(sliderValue));
  root.addChild(volumeLabel);
  root.addChild(buildSlider(volume));
  root.addChild(brightnessLabel);
  root.addChild(buildSlider(brightness, 0.0f, 255.0f));
  root.addChild(balanceLabel);
  root.addChild(buildSlider(balance, -100.0f, 100.0f));

  root.addChild(heading("Country"));
  root.addChild(buildDropdown({"Option A", "Option B", "Option C"},
                              dropdownChoice, dropdownOpen));

  root.addChild(heading("Details"));
  std::vector<std::string> tabTitles = {"Overview", "Specs", "Reviews"};
  auto makePanel = [](const std::string &text) {
    Text t;
    t.label = text;
    t.fontSize = 14;
    View panel;
    panel.addChild(t);
    return panel;
  };
  std::vector<std::function<View()>> tabPanels = {
      [makePanel] { return makePanel("This is the overview panel."); },
      [makePanel] { return makePanel("Specs: 128GB, 8GB RAM, USB-C."); },
      [makePanel] {
        return makePanel("Works great, according to a happy user.");
      },
  };
  root.addChild(buildTabs(tabTitles, tabPanels, activeTab));

  root.addChild(heading("Danger Zone"));
  root.addChild(buildDialogTrigger(deleteDialogOpen, "Delete Account"));

  root.addChild(buildConfirmDialog(
      deleteDialogOpen, "Delete Account?",
      "This will permanently delete your account and all associated "
      "data. This action cannot be undone.",
      DialogButton{"Cancel", Color{240, 240, 240}, Color{225, 225, 225}},
      DialogButton{"Delete", Color{0xC0, 0x39, 0x2B}, Color{0xA8, 0x2F, 0x23},
                   Color{255, 255, 255}},
      []() { std::cout << "Account deleted" << std::endl; }));

  LiteUI ui("Input Widgets Demo", 420, 760);
  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}