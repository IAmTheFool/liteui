#include "liteui.hpp"
#include <iostream>

int main() {
  bool dialogOpen = false;

  // --- trigger button ---
  Text triggerLabel;
  triggerLabel.label = "Delete Account";

  View trigger;
  trigger.style.width = Size::pixel(160);
  trigger.style.height = Size::pixel(36);
  trigger.style.backgroundColor = {230, 230, 230};
  trigger.style.hoverColor = {210, 210, 210};
  trigger.style.borderRadius = 4;
  trigger.style.alignItems = Align::Center;
  trigger.style.justifyContent = Justify::Center;
  trigger.onClick = [&]() { dialogOpen = true; };
  trigger.addChild(triggerLabel);

  // --- dialog box ---
  // Note this is Position::Static (the default), NOT Absolute. It's an
  // ordinary flow child of backdrop; backdrop's own alignItems/
  // justifyContent (set below) center it within the full-window backdrop
  // automatically, every relayout, with zero app-side position tracking.
  View dialogBox;
  dialogBox.style.direction = FlexDirection::Column;
  dialogBox.style.width = Size::pixel(320);
  dialogBox.style.padding = EdgeInsets::all(20);
  dialogBox.style.gap = 16;
  dialogBox.style.backgroundColor = {255, 255, 255};
  dialogBox.style.borderRadius = 8;
  // Swallows any click that lands inside the dialog's own padding/gaps
  // (i.e. not on a specific button below). Without this, such a click
  // would find no onClick on dialogBox or any of its flow descendants
  // at that exact point, and hitTestFlow's bubbling would walk all the
  // way up to backdrop's onClick — silently closing the dialog just
  // because the user clicked its whitespace instead of a button. An
  // empty handler is enough: hitTestFlow only checks `v.onClick &&
  // !v.disabled`, so this stops the search right here and backdrop
  // never gets asked.
  dialogBox.onClick = [] {};

  Text title;
  title.label = "Delete Account?";
  title.fontSize = 18;
  title.fontWeight = FontWeight::SemiBold;

  Text message;
  message.label =
      "This will permanently delete your account and all associated "
      "data. This action cannot be undone.";
  message.wrap = TextWrap::Wrap;
  message.style.width = Size::full();
  message.color = {90, 90, 90};

  // --- button row ---
  View buttonRow;
  buttonRow.style.direction = FlexDirection::Row;
  buttonRow.style.justifyContent = Justify::End;
  buttonRow.style.gap = 10;

  Text cancelLabel;
  cancelLabel.label = "Cancel";

  View cancelButton;
  cancelButton.style.width = Size::pixel(80);
  cancelButton.style.height = Size::pixel(34);
  cancelButton.style.backgroundColor = {240, 240, 240};
  cancelButton.style.hoverColor = {225, 225, 225};
  cancelButton.style.borderRadius = 4;
  cancelButton.style.alignItems = Align::Center;
  cancelButton.style.justifyContent = Justify::Center;
  cancelButton.onClick = [&]() { dialogOpen = false; };
  cancelButton.addChild(cancelLabel);

  Text confirmLabel;
  confirmLabel.label = "Delete";
  confirmLabel.color = {255, 255, 255};

  View confirmButton;
  confirmButton.style.width = Size::pixel(80);
  confirmButton.style.height = Size::pixel(34);
  confirmButton.style.backgroundColor = {0xC0, 0x39, 0x2B};
  confirmButton.style.hoverColor = {0xA8, 0x2F, 0x23};
  confirmButton.style.borderRadius = 4;
  confirmButton.style.alignItems = Align::Center;
  confirmButton.style.justifyContent = Justify::Center;
  confirmButton.onClick = [&]() {
    std::cout << "Account deleted" << std::endl;
    // ... actual delete logic here ...
    dialogOpen = false;
  };
  confirmButton.addChild(confirmLabel);

  buttonRow.addChild(cancelButton);
  buttonRow.addChild(confirmButton);

  dialogBox.addChild(title);
  dialogBox.addChild(message);
  dialogBox.addChild(buttonRow);

  // --- backdrop: full-window, dims the page, closes on outside click ---
  View backdrop;
  backdrop.style.position = Position::Absolute;
  backdrop.style.left = 0;
  backdrop.style.top = 0;
  backdrop.style.right = 0;
  backdrop.style.bottom = 0;
  backdrop.style.zIndex = 100;
  backdrop.style.backgroundColor = {0, 0, 0, 90}; // dimmed, unlike the
                                                   // near-invisible menu
                                                   // backdrop
  backdrop.style.alignItems = Align::Center;      // <-- centers dialogBox
  backdrop.style.justifyContent = Justify::Center; // <-- vertically & horizontally
  backdrop.displaySource = [&]() { return dialogOpen; };
  backdrop.onClick = [&]() { dialogOpen = false; }; // click outside -> cancel
  backdrop.addChild(dialogBox);

  // --- root ---
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Center;
  root.style.justifyContent = Justify::Center;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(trigger);
  root.addChild(backdrop);

  LiteUI ui(400, 300, "Confirm Dialog");
  ui.setRoot(root);
  ui.run();
  return 0;
}