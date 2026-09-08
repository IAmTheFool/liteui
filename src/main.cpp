#include "liteui.hpp"
#include <algorithm>
#include <string>

// ---------------- Text field state ----------------
struct TextFieldState {
  std::string text = "Hello";
  size_t cursor = text.size(); // byte offset; ASCII-only assumption throughout
  bool focused = false;
  bool blinkOn = true;
  bool dirty = true; // consumed by canvasDirtySource
  int blinkTimerHandle = -1;
};

// Finds the character boundary whose x-position is closest to clickX
// (clickX measured from the text's own left edge, i.e. already offset by
// whatever left padding onPaint uses).
static size_t caretIndexForX(const std::string &text, const TextStyle &ts,
                             float clickX) {
  float best = 1e9f;
  size_t bestIdx = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    liteui_text::Measurement m =
        liteui_text::measure(text.substr(0, i), ts, -1);
    float d = std::abs(m.width - clickX);
    if (d < best) {
      best = d;
      bestIdx = i;
    }
  }
  return bestIdx;
}

int main() {
  LiteUI ui(480, 320, "Text Field Test");

  TextFieldState tf;

  // Shared font settings — must match exactly what onPaint uses, since
  // caretIndexForX and the caret's own x-position both re-measure text
  // against this same TextStyle to stay in sync with what's drawn.
  TextStyle ts;
  ts.fontSize = 16.0f;
  Color textColor{0, 0, 0};
  ts.color = textColor;
  constexpr float kLeftPad = 6.0f;

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

  // ---- the text field itself, built as a Canvas so we fully control
  // drawing (text + blinking caret) ----
  Canvas field;
  field.style.width = Size::pixel(240);
  field.style.height = Size::pixel(34);
  field.style.borderWidth = 1.5f;
  field.style.borderColor = [&tf] {
    return tf.focused ? Color{50, 120, 220} : Color{180, 180, 180};
  };
  field.style.backgroundColor = Color{255, 255, 255};
  field.focusable = true;

  field.canvasDirtySource = [&tf] {
    bool d = tf.dirty;
    tf.dirty = false;
    return d;
  };

  field.onPaint = [&tf, ts, textColor](CanvasContext &ctx) {
    ctx.setFont(ts.fontFamily, ts.fontSize, ts.fontWeight, ts.fontStyle);
    ctx.setFillColor(textColor);
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);
    ctx.fillText(tf.text, kLeftPad, ctx.height() / 2.0f);

    if (tf.focused && tf.blinkOn) {
      liteui_text::Measurement m =
          liteui_text::measure(tf.text.substr(0, tf.cursor), ts, -1);
      float cx = kLeftPad + m.width;
      ctx.setFillColor(Color{20, 20, 20});
      ctx.fillRect(cx, 5.0f, 1.5f, std::max(0.0f, ctx.height() - 10.0f));
    }
  };

  // Left-click positions the caret at the nearest character boundary.
  field.onPressAt = [&tf, ts](float lx, float) {
    tf.cursor = caretIndexForX(tf.text, ts, lx - kLeftPad);
    tf.blinkOn = true;
    tf.dirty = true;
  };

  field.onFocus = [&tf, &ui] {
    tf.focused = true;
    tf.blinkOn = true;
    tf.dirty = true;
    // Start blinking only while focused, and only one timer at a time.
    if (tf.blinkTimerHandle < 0) {
      tf.blinkTimerHandle = ui.addInterval(530, [&tf] {
        tf.blinkOn = !tf.blinkOn;
        tf.dirty = true;
      });
    }
  };
  field.onBlur = [&tf, &ui] {
    tf.focused = false;
    tf.dirty = true;
    if (tf.blinkTimerHandle >= 0) {
      ui.removeInterval(tf.blinkTimerHandle);
      tf.blinkTimerHandle = -1;
    }
  };

  field.onKeyDown = [&tf](KeyEvent e) {
    switch (e.key) {
    case Key::Left:
      if (tf.cursor > 0)
        tf.cursor--;
      break;
    case Key::Right:
      if (tf.cursor < tf.text.size())
        tf.cursor++;
      break;
    case Key::Home:
      tf.cursor = 0;
      break;
    case Key::End:
      tf.cursor = tf.text.size();
      break;
    case Key::Backspace:
      if (tf.cursor > 0) {
        tf.text.erase(tf.cursor - 1, 1);
        tf.cursor--;
      }
      break;
    case Key::Delete:
      if (tf.cursor < tf.text.size())
        tf.text.erase(tf.cursor, 1);
      break;
    default:
      return; // unhandled key: don't reset blink/dirty
    }
    tf.blinkOn = true;
    tf.dirty = true;
  };

  field.onTextInput = [&tf](uint32_t cp) {
    if (cp >= 0x20 && cp < 128) { // ASCII-only, matches caretIndexForX
      tf.text.insert(tf.cursor, 1, static_cast<char>(cp));
      tf.cursor++;
      tf.blinkOn = true;
      tf.dirty = true;
    }
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