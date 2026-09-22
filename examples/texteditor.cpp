// editor_example.cpp
//
// A minimal Notepad-style text editor: a top bar (filename + char count)
// and a full-window TextArea below it. Ctrl+S writes the current
// contents to "untitled.txt" in the working directory.

#include "liteui.hpp"

#include <fstream>
#include <memory>
#include <string>
#include <vector>

// Mutable state for a TextArea, held by shared_ptr so every copy of the
// View a TextArea flattens into (addChild copies Views, same reasoning
// as TextInputState) still shares one live document/cursor/scroll state.
struct TextAreaState {
  std::vector<std::string> lines{std::string()}; // always >= 1 entry
  size_t cursorLine = 0;
  size_t cursorCol = 0; // byte offset into lines[cursorLine]
  bool focused = false;
  bool blinkOn = true;
  bool dirty = true;
  int blinkTimerHandle = -1;
  float scrollX = 0.0f; // pixels, applied uniformly to every visible line
  float scrollY = 0.0f; // pixels
  // Cached from the most recent onPaint call, so onKeyDown/onScrollUp/
  // onScrollDown (which have no CanvasContext of their own) can still
  // clamp scrolling and compute page-up/page-down step sizes.
  float lastViewW = 0.0f, lastViewH = 0.0f;
};

// Joins state->lines back into the single '\n'-separated string shape
// TextArea::onChange (and any app code reading the current contents)
// wants.
inline std::string textAreaJoin(const TextAreaState &s) {
  std::string out;
  for (size_t i = 0; i < s.lines.size(); ++i) {
    if (i)
      out += '\n';
    out += s.lines[i];
  }
  return out;
}

// Author-facing struct. addChild-style usage: pass to toTextAreaView()
// and add the resulting View, e.g. `parent.addChild(toTextAreaView(ta));`
// — TextArea isn't wired into View::addChild overloads directly (this
// header ships separately from liteui.hpp), so call toTextAreaView()
// explicitly instead of the addChild(Text)/addChild(Canvas) shorthand.
struct TextArea {
  Style style;
  std::string text; // initial content; split on '\n'
  std::string placeholder;
  float fontSize = 15.0f;
  FontWeight fontWeight = FontWeight::Regular;
  std::string fontFamily; // empty = platform default; set e.g. "Consolas"
                          // or "Monospace" for a code-editor feel
  Color textColor = Color{20, 20, 20};
  Color placeholderColor = Color{160, 160, 160};
  Color caretColor = Color{20, 20, 20};
  Color borderColor = Color{180, 180, 180};
  Color focusedBorderColor = Color{50, 120, 220};
  Color lineNumberColor = Color{170, 170, 170};
  Color lineNumberBackground = Color{245, 245, 245};
  bool showLineNumbers = true;
  float padding = 8.0f;
  float lineHeight = 0.0f; // 0 = auto (fontSize * 1.4)
  int tabWidthSpaces = 4;
  std::function<void(const std::string &)> onChange;

  std::shared_ptr<TextAreaState> state = std::make_shared<TextAreaState>();
};

inline View toTextAreaView(TextArea ta) {
  auto state = ta.state;

  // Split initial text into lines.
  state->lines.clear();
  {
    std::string cur;
    for (char c : ta.text) {
      if (c == '\n') {
        state->lines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    state->lines.push_back(cur);
  }
  state->cursorLine = state->lines.size() - 1;
  state->cursorCol = state->lines.back().size();

  TextStyle ts;
  ts.fontSize = ta.fontSize;
  ts.fontWeight = ta.fontWeight;
  ts.fontFamily = ta.fontFamily;
  ts.wrap = TextWrap::NoWrap;

  float lineH = ta.lineHeight > 0 ? ta.lineHeight : ta.fontSize * 1.4f;
  float pad = ta.padding;
  bool showNums = ta.showLineNumbers;
  int tabSpaces = std::max(1, ta.tabWidthSpaces);
  Color textColor = ta.textColor;
  Color placeholderColor = ta.placeholderColor;
  Color caretColor = ta.caretColor;
  Color lineNumColor = ta.lineNumberColor;
  Color lineNumBg = ta.lineNumberBackground;
  std::string placeholder = ta.placeholder;
  auto onChange = ta.onChange;
  Color borderColor = ta.borderColor;
  Color focusedBorderColor = ta.focusedBorderColor;

  View v;
  v.style = std::move(ta.style);
  v.isCanvas = true;
  v.focusable = true;

  // Border color reflects focus automatically, same idiom TextInput uses.
  v.style.borderColor = [state, borderColor, focusedBorderColor] {
    return state->focused ? focusedBorderColor : borderColor;
  };

  v.canvasDirtySource = [state] {
    bool d = state->dirty;
    state->dirty = false;
    return d;
  };

  // Width of the line-number gutter, given how many lines currently
  // exist — grows as the document gets longer ("9" -> "10" -> "100").
  auto gutterWidth = [showNums, ts](size_t lineCount) -> float {
    if (!showNums)
      return 0.0f;
    int digits = 1;
    size_t n = lineCount;
    while (n >= 10) {
      n /= 10;
      ++digits;
    }
    liteui_text::Measurement m =
        liteui_text::measure(std::string(digits + 1, '0'), ts, -1);
    return m.width + 12.0f; // a little breathing room on each side
  };

  auto notifyChange = [state, onChange] {
    if (onChange)
      onChange(textAreaJoin(*state));
  };

  v.onPaint = [=](CanvasContext &ctx) {
    state->lastViewW = ctx.width();
    state->lastViewH = ctx.height();
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    float availW = std::max(0.0f, ctx.width() - textLeft - pad);
    float availH = std::max(0.0f, ctx.height() - pad * 2.0f);

    // Keep the caret's line vertically in view.
    float caretTop = state->cursorLine * lineH;
    if (caretTop - state->scrollY < 0)
      state->scrollY = caretTop;
    if (caretTop + lineH - state->scrollY > availH)
      state->scrollY = caretTop + lineH - availH;
    float maxScrollY = std::max(0.0f, state->lines.size() * lineH - availH);
    state->scrollY = std::clamp(state->scrollY, 0.0f, maxScrollY);

    // Keep the caret's column horizontally in view. scrollX is applied
    // uniformly to every visible line (see the file-level limitations
    // note) — only the current line's caret position drives it.
    const std::string &curLine = state->lines[state->cursorLine];
    liteui_text::Measurement caretM =
        liteui_text::measure(curLine.substr(0, state->cursorCol), ts, -1);
    if (caretM.width - state->scrollX > availW)
      state->scrollX = caretM.width - availW;
    if (caretM.width - state->scrollX < 0)
      state->scrollX = caretM.width;
    state->scrollX = std::max(0.0f, state->scrollX);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, ctx.width(), ctx.height());
    ctx.clip();

    if (gutter > 0) {
      ctx.setFillColor(lineNumBg);
      ctx.fillRect(0, 0, gutter + pad, ctx.height());
    }

    ctx.setFont(ts.fontFamily, ts.fontSize, ts.fontWeight, ts.fontStyle);
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);

    int firstLine = std::max(0, static_cast<int>(state->scrollY / lineH));
    int lastLine =
        std::min(static_cast<int>(state->lines.size()) - 1,
                 static_cast<int>((state->scrollY + availH) / lineH) + 1);

    bool empty = state->lines.size() == 1 && state->lines[0].empty();
    if (empty && !state->focused && !placeholder.empty()) {
      ctx.setFillColor(placeholderColor);
      ctx.fillText(placeholder, textLeft, pad + lineH / 2.0f);
    } else {
      for (int i = firstLine; i <= lastLine; ++i) {
        float y = pad + i * lineH - state->scrollY + lineH / 2.0f;
        if (gutter > 0) {
          ctx.setFillColor(lineNumColor);
          ctx.setTextAlign(TextAlign::End);
          ctx.fillText(std::to_string(i + 1), textLeft - 8.0f, y);
          ctx.setTextAlign(TextAlign::Start);
        }
        ctx.setFillColor(textColor);
        ctx.fillText(state->lines[static_cast<size_t>(i)],
                     textLeft - state->scrollX, y);
      }
    }

    if (state->focused && state->blinkOn) {
      float cy = pad + state->cursorLine * lineH - state->scrollY;
      ctx.setFillColor(caretColor);
      ctx.fillRect(textLeft - state->scrollX + caretM.width, cy + 2.0f, 1.5f,
                   std::max(0.0f, lineH - 4.0f));
    }

    ctx.restore();
  };

  v.onPressAt = [=](float lx, float ly) {
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    int line = static_cast<int>((ly - pad + state->scrollY) / lineH);
    line = std::clamp(line, 0, static_cast<int>(state->lines.size()) - 1);
    state->cursorLine = static_cast<size_t>(line);
    float localX = lx - textLeft + state->scrollX;
    state->cursorCol = liteui_text::caretIndexForX(
        state->lines[state->cursorLine], ts, std::max(0.0f, localX));
    state->blinkOn = true;
    state->dirty = true;
  };

  v.onFocus = [state] {
    state->focused = true;
    state->blinkOn = true;
    state->dirty = true;
    if (state->blinkTimerHandle < 0 && LiteUI::current()) {
      state->blinkTimerHandle = LiteUI::current()->addInterval(530, [state] {
        state->blinkOn = !state->blinkOn;
        state->dirty = true;
      });
    }
  };
  v.onBlur = [state] {
    state->focused = false;
    state->dirty = true;
    if (state->blinkTimerHandle >= 0 && LiteUI::current()) {
      LiteUI::current()->removeInterval(state->blinkTimerHandle);
      state->blinkTimerHandle = -1;
    }
  };

  // Discrete wheel notches scroll a fixed number of lines — see the
  // file-level note on why this Canvas node doesn't get the library's
  // pixel-based wheel scrolling automatically.
  v.onScrollUp = [state, lineH] {
    state->scrollY = std::max(0.0f, state->scrollY - lineH * 3.0f);
    state->dirty = true;
  };
  v.onScrollDown = [state, lineH] {
    float maxScrollY =
        std::max(0.0f, state->lines.size() * lineH - state->lastViewH);
    state->scrollY = std::min(maxScrollY, state->scrollY + lineH * 3.0f);
    state->dirty = true;
  };

  v.onKeyDown = [=](KeyEvent e) {
    auto &lines = state->lines;
    size_t &ln = state->cursorLine;
    size_t &col = state->cursorCol;
    bool handled = true;
    switch (e.key) {
    case Key::Left:
      if (col > 0)
        --col;
      else if (ln > 0) {
        --ln;
        col = lines[ln].size();
      }
      break;
    case Key::Right:
      if (col < lines[ln].size())
        ++col;
      else if (ln + 1 < lines.size()) {
        ++ln;
        col = 0;
      }
      break;
    case Key::Up:
      if (ln > 0) {
        --ln;
        col = std::min(col, lines[ln].size());
      }
      break;
    case Key::Down:
      if (ln + 1 < lines.size()) {
        ++ln;
        col = std::min(col, lines[ln].size());
      }
      break;
    case Key::Home:
      col = 0;
      break;
    case Key::End:
      col = lines[ln].size();
      break;
    case Key::PageUp: {
      float availH = std::max(lineH, state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH));
      ln = static_cast<size_t>(std::max(0, static_cast<int>(ln) - step));
      col = std::min(col, lines[ln].size());
      break;
    }
    case Key::PageDown: {
      float availH = std::max(lineH, state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH));
      ln = std::min(lines.size() - 1, ln + static_cast<size_t>(step));
      col = std::min(col, lines[ln].size());
      break;
    }
    case Key::Backspace:
      if (col > 0) {
        lines[ln].erase(col - 1, 1);
        --col;
        notifyChange();
      } else if (ln > 0) {
        size_t prevLen = lines[ln - 1].size();
        lines[ln - 1] += lines[ln];
        lines.erase(lines.begin() + static_cast<long>(ln));
        --ln;
        col = prevLen;
        notifyChange();
      }
      break;
    case Key::Delete:
      if (col < lines[ln].size()) {
        lines[ln].erase(col, 1);
        notifyChange();
      } else if (ln + 1 < lines.size()) {
        lines[ln] += lines[ln + 1];
        lines.erase(lines.begin() + static_cast<long>(ln) + 1);
        notifyChange();
      }
      break;
    case Key::Enter: {
      std::string rest = lines[ln].substr(col);
      lines[ln].erase(col);
      lines.insert(lines.begin() + static_cast<long>(ln) + 1, rest);
      ++ln;
      col = 0;
      notifyChange();
      break;
    }
    case Key::Tab:
      lines[ln].insert(col, std::string(static_cast<size_t>(tabSpaces), ' '));
      col += static_cast<size_t>(tabSpaces);
      notifyChange();
      break;
    default:
      handled = false;
      break;
    }
    if (handled) {
      state->blinkOn = true;
      state->dirty = true;
    }
  };

  v.onTextInput = [=](uint32_t cp) {
    if (cp >= 0x20 && cp < 128) {
      auto &lines = state->lines;
      size_t &ln = state->cursorLine;
      size_t &col = state->cursorCol;
      lines[ln].insert(col, 1, static_cast<char>(cp));
      ++col;
      state->blinkOn = true;
      state->dirty = true;
      notifyChange();
    }
  };

  return v;
}

int main() {
  LiteUI ui("Simple Text Editor",900, 650);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{250, 250, 250};

  // ---- top bar: filename on the left, live char count on the right ----
  View topBar;
  topBar.style.direction = FlexDirection::Row;
  topBar.style.alignItems = Align::Center;
  topBar.style.justifyContent = Justify::SpaceBetween;
  topBar.style.padding = EdgeInsets{8, 12, 8, 12};
  topBar.style.width = Size::full();
  topBar.style.height = Size::pixel(36);
  topBar.style.backgroundColor = Color{235, 235, 235};

  Text title;
  title.label = std::string("untitled.txt");
  title.fontSize = 13;
  title.color = Color{90, 90, 90};
  topBar.addChild(title);

  // A plain shared string the status Text polls every dispatch cycle
  // (click/key/timer) via its Dynamic<std::string> label — same idiom
  // TextInput/TextArea use for their own dynamic fields.
  auto statusLabel = std::make_shared<std::string>("0 chars");
  Text statusText;
  statusText.label = [statusLabel] { return *statusLabel; };
  statusText.fontSize = 12;
  statusText.color = Color{130, 130, 130};
  topBar.addChild(statusText);

  root.addChild(topBar);

  // ---- editor: fills all remaining vertical space ----
  TextArea editor;

  editor.style.flexGrow = 1;
  editor.style.backgroundColor = Color{255, 255, 255};
  editor.placeholder = "Start typing...";
  editor.fontSize = 15;
  editor.fontFamily = ""; // set e.g. "Consolas" (Win) / "Monospace" (Linux)
                          // for a code-editor look
  editor.showLineNumbers = true;
  editor.onChange = [statusLabel](const std::string &text) {
    *statusLabel = std::to_string(text.size()) + " chars";
  };

  // Keep a handle to the shared editing state so the Ctrl+S shortcut
  // below can read the document's current contents — editor.state is a
  // shared_ptr, so this alias stays valid regardless of what happens to
  // the (now-consumed) `editor` value passed into toTextAreaView.
  auto editorState = editor.state;

  View editorWrap;
  editorWrap.style.width = Size::full();
  editorWrap.style.flexGrow = 1;
  editorWrap.addChild(toTextAreaView(editor));
  root.addChild(editorWrap);

  ui.setRoot(std::move(root));

  KeyModifiers ctrlOnly;
  ctrlOnly.ctrl = true;
  ui.addShortcut(ctrlOnly, Key::S, [editorState] {
    std::ofstream out("untitled.txt", std::ios::binary);
    out << textAreaJoin(*editorState);
  });

  ui.run();
}