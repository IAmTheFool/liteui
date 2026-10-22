// editor_example.cpp
//
// A minimal Notepad-style text editor: a top bar (filename + char count)
// and a full-window TextArea below it. Ctrl+S writes the current
// contents to "untitled.txt" in the working directory.

#include "liteui.hpp"

#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace theme {

enum class Theme { Dark, Light, Monokai };

struct Palette {
  Color appBg, topBarBg, titleText, statusText;
  Color editorBg, textColor, placeholderColor, caretColor;
  Color borderColor, focusedBorderColor, lineNumberColor, lineNumberBg;
};

inline const Palette &lightPalette() {
  static const Palette p{
      .appBg = {250, 250, 250},
      .topBarBg = {235, 235, 235},
      .titleText = {90, 90, 90},
      .statusText = {130, 130, 130},
      .editorBg = {255, 255, 255},
      .textColor = {20, 20, 20},
      .placeholderColor = {160, 160, 160},
      .caretColor = {20, 20, 20},
      .borderColor = {180, 180, 180},
      .focusedBorderColor = {50, 120, 220},
      .lineNumberColor = {170, 170, 170},
      .lineNumberBg = {245, 245, 245},
  };
  return p;
}

inline const Palette &darkPalette() {
  static const Palette p{
      .appBg = {30, 30, 30},
      .topBarBg = {45, 45, 45},
      .titleText = {200, 200, 200},
      .statusText = {150, 150, 150},
      .editorBg = {30, 30, 30},
      .textColor = {212, 212, 212},
      .placeholderColor = {120, 120, 120},
      .caretColor = {220, 220, 220},
      .borderColor = {70, 70, 70},
      .focusedBorderColor = {0, 122, 204},
      .lineNumberColor = {110, 110, 110},
      .lineNumberBg = {37, 37, 38},
  };
  return p;
}

inline const Palette &monokaiPalette() {
  static const Palette p{
      .appBg = {39, 40, 34},
      .topBarBg = {50, 51, 44},
      .titleText = {248, 248, 242},
      .statusText = {160, 160, 150},
      .editorBg = {39, 40, 34},
      .textColor = {248, 248, 242},
      .placeholderColor = {117, 113, 94},
      .caretColor = {248, 248, 242},
      .borderColor = {70, 71, 62},
      .focusedBorderColor = {249, 38, 114},
      .lineNumberColor = {117, 113, 94},
      .lineNumberBg = {45, 46, 40},
  };
  return p;
}

inline const Palette &paletteFor(Theme t) {
  switch (t) {
  case Theme::Dark:
    return darkPalette();
  case Theme::Monokai:
    return monokaiPalette();
  case Theme::Light:
  default:
    return lightPalette();
  }
}

inline const char *name(Theme t) {
  switch (t) {
  case Theme::Dark:
    return "Dark";
  case Theme::Monokai:
    return "Monokai";
  case Theme::Light:
  default:
    return "Light";
  }
}

inline Color appBg = lightPalette().appBg;
inline Color topBarBg = lightPalette().topBarBg;
inline Color titleText = lightPalette().titleText;
inline Color statusText = lightPalette().statusText;
inline Color editorBg = lightPalette().editorBg;
inline Color textColor = lightPalette().textColor;
inline Color placeholderColor = lightPalette().placeholderColor;
inline Color caretColor = lightPalette().caretColor;
inline Color borderColor = lightPalette().borderColor;
inline Color focusedBorderColor = lightPalette().focusedBorderColor;
inline Color lineNumberColor = lightPalette().lineNumberColor;
inline Color lineNumberBg = lightPalette().lineNumberBg;

inline Theme &current() {
  static Theme t = Theme::Light;
  return t;
}

inline void setTheme(Theme t) {
  current() = t;
  const Palette &p = paletteFor(t);
  appBg = p.appBg;
  topBarBg = p.topBarBg;
  titleText = p.titleText;
  statusText = p.statusText;
  editorBg = p.editorBg;
  textColor = p.textColor;
  placeholderColor = p.placeholderColor;
  caretColor = p.caretColor;
  borderColor = p.borderColor;
  focusedBorderColor = p.focusedBorderColor;
  lineNumberColor = p.lineNumberColor;
  lineNumberBg = p.lineNumberBg;
}

inline void cycleTheme() {
  switch (current()) {
  case Theme::Light:
    setTheme(Theme::Dark);
    break;
  case Theme::Dark:
    setTheme(Theme::Monokai);
    break;
  case Theme::Monokai:
    setTheme(Theme::Light);
    break;
  }
}

} // namespace theme

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
  float lastViewW = 0.0f, lastViewH = 0.0f;
  float fontSize = 15.0f;
};

inline std::string textAreaJoin(const TextAreaState &s) {
  std::string out;
  for (size_t i = 0; i < s.lines.size(); ++i) {
    if (i)
      out += '\n';
    out += s.lines[i];
  }
  return out;
}

struct TextArea {
  Style style;
  std::string text; // initial content; split on '\n'
  std::string placeholder;
  float fontSize = 15.0f;
  FontWeight fontWeight = FontWeight::Regular;
  std::string fontFamily;
  std::optional<Color> textColor;
  std::optional<Color> placeholderColor;
  std::optional<Color> caretColor;
  std::optional<Color> borderColor;
  std::optional<Color> focusedBorderColor;
  std::optional<Color> lineNumberColor;
  std::optional<Color> lineNumberBackground;
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
  ts.fontFamily = ta.fontFamily;
  ts.wrap = TextWrap::NoWrap;
  state->fontSize = ta.fontSize;
  FontWeight resolvedFontWeight = ta.fontWeight;
  FontStyle resolvedFontStyle =
      FontStyle::Normal; // not yet exposed on TextArea

  float lineHeightOverride = ta.lineHeight;
  auto lineH = [state, lineHeightOverride]() -> float {
    return lineHeightOverride > 0.0f ? lineHeightOverride
                                     : state->fontSize * 1.4f;
  };
  float pad = ta.padding;
  bool showNums = ta.showLineNumbers;
  int tabSpaces = std::max(1, ta.tabWidthSpaces);
  std::optional<Color> textColorOverride = ta.textColor;
  std::optional<Color> placeholderColorOverride = ta.placeholderColor;
  std::optional<Color> caretColorOverride = ta.caretColor;
  std::optional<Color> lineNumColorOverride = ta.lineNumberColor;
  std::optional<Color> lineNumBgOverride = ta.lineNumberBackground;
  std::string placeholder = ta.placeholder;
  auto onChange = ta.onChange;
  std::optional<Color> borderColorOverride = ta.borderColor;
  std::optional<Color> focusedBorderColorOverride = ta.focusedBorderColor;

  View v;
  v.style = std::move(ta.style);
  v.isCanvas = true;
  v.focusable = true;


  v.style.borderColor = [state, borderColorOverride,
                         focusedBorderColorOverride] {
    if (state->focused)
      return focusedBorderColorOverride.value_or(theme::focusedBorderColor);
    return borderColorOverride.value_or(theme::borderColor);
  };

  v.canvasDirtySource = [state] {
    bool d = state->dirty;
    state->dirty = false;
    return d;
  };


  auto gutterWidth = [showNums, ts, state, resolvedFontWeight,
                      resolvedFontStyle](size_t lineCount) -> float {
    if (!showNums)
      return 0.0f;
    int digits = 1;
    size_t n = lineCount;
    while (n >= 10) {
      n /= 10;
      ++digits;
    }
    liteui_text::Measurement m =
        liteui_text::measure(std::string(digits + 1, '0'), ts, state->fontSize,
                             resolvedFontWeight, resolvedFontStyle, -1);
    return m.width + 12.0f; // a little breathing room on each side
  };

  auto notifyChange = [state, onChange] {
    if (onChange)
      onChange(textAreaJoin(*state));
  };

  v.onPaint = [=](CanvasContext &ctx) {

    Color textColor = textColorOverride.value_or(theme::textColor);
    Color placeholderColor =
        placeholderColorOverride.value_or(theme::placeholderColor);
    Color caretColor = caretColorOverride.value_or(theme::caretColor);
    Color lineNumColor = lineNumColorOverride.value_or(theme::lineNumberColor);
    Color lineNumBg = lineNumBgOverride.value_or(theme::lineNumberBg);

    state->lastViewW = ctx.width();
    state->lastViewH = ctx.height();
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    float availW = std::max(0.0f, ctx.width() - textLeft - pad);
    float availH = std::max(0.0f, ctx.height() - pad * 2.0f);


    float caretTop = state->cursorLine * lineH();
    if (caretTop - state->scrollY < 0)
      state->scrollY = caretTop;
    if (caretTop + lineH() - state->scrollY > availH)
      state->scrollY = caretTop + lineH() - availH;
    float maxScrollY = std::max(0.0f, state->lines.size() * lineH() - availH);
    state->scrollY = std::clamp(state->scrollY, 0.0f, maxScrollY);


    const std::string &curLine = state->lines[state->cursorLine];
    liteui_text::Measurement caretM = liteui_text::measure(
        curLine.substr(0, state->cursorCol), ts, state->fontSize,
        resolvedFontWeight, resolvedFontStyle, -1);
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

    ctx.setFont(ts.fontFamily, state->fontSize, resolvedFontWeight,
                resolvedFontStyle);
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);

    int firstLine = std::max(0, static_cast<int>(state->scrollY / lineH()));
    int lastLine =
        std::min(static_cast<int>(state->lines.size()) - 1,
                 static_cast<int>((state->scrollY + availH) / lineH()) + 1);

    bool empty = state->lines.size() == 1 && state->lines[0].empty();
    if (empty && !state->focused && !placeholder.empty()) {
      ctx.setFillColor(placeholderColor);
      ctx.fillText(placeholder, textLeft, pad + lineH() / 2.0f);
    } else {
      for (int i = firstLine; i <= lastLine; ++i) {
        float y = pad + i * lineH() - state->scrollY + lineH() / 2.0f;
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
      float cy = pad + state->cursorLine * lineH() - state->scrollY;
      ctx.setFillColor(caretColor);
      ctx.fillRect(textLeft - state->scrollX + caretM.width, cy + 2.0f, 1.5f,
                   std::max(0.0f, lineH() - 4.0f));
    }

    ctx.restore();
  };

  v.onPressAt = [=](float lx, float ly) {
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    int line = static_cast<int>((ly - pad + state->scrollY) / lineH());
    line = std::clamp(line, 0, static_cast<int>(state->lines.size()) - 1);
    state->cursorLine = static_cast<size_t>(line);
    float localX = lx - textLeft + state->scrollX;
    state->cursorCol = liteui_text::caretIndexForX(
        state->lines[state->cursorLine], ts, state->fontSize,
        resolvedFontWeight, resolvedFontStyle, std::max(0.0f, localX));
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


  v.onScrollUp = [state, lineH] {
    state->scrollY = std::max(0.0f, state->scrollY - lineH() * 3.0f);
    state->dirty = true;
  };
  v.onScrollDown = [state, lineH] {
    float maxScrollY =
        std::max(0.0f, state->lines.size() * lineH() - state->lastViewH);
    state->scrollY = std::min(maxScrollY, state->scrollY + lineH() * 3.0f);
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
      float availH = std::max(lineH(), state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH()));
      ln = static_cast<size_t>(std::max(0, static_cast<int>(ln) - step));
      col = std::min(col, lines[ln].size());
      break;
    }
    case Key::PageDown: {
      float availH = std::max(lineH(), state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH()));
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
  LiteUI ui("Simple Text Editor", 900, 650);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = [] { return theme::appBg; };

  // ---- editor: built first so editorState exists before themeBtn's
  // onClick (further down) needs to capture it ----
  TextArea editor;
  editor.style.flexGrow = 1;
  editor.style.backgroundColor = [] { return theme::editorBg; };
  editor.placeholder = "Start typing...";
  editor.fontSize = 15;
  editor.fontFamily = ""; // set e.g. "Consolas" (Win) / "Monospace" (Linux)
                          // for a code-editor look
  editor.showLineNumbers = true;

  auto statusLabel = std::make_shared<std::string>("0 chars");
  editor.onChange = [statusLabel](const std::string &text) {
    *statusLabel = std::to_string(text.size()) + " chars";
  };

  // Keep a handle to the shared editing state so the Ctrl+S shortcut
  // and the theme toggle below can both use it after `editor` itself is
  // consumed by toTextAreaView() — editor.state is a shared_ptr, so this
  // alias stays valid regardless.
  auto editorState = editor.state;

  // ---- top bar: filename, live char count, theme toggle ----
  View topBar;
  topBar.style.direction = FlexDirection::Row;
  topBar.style.alignItems = Align::Center;
  topBar.style.justifyContent = Justify::SpaceBetween;
  topBar.style.padding = EdgeInsets{8, 12, 8, 12};
  topBar.style.width = Size::full();
  topBar.style.height = Size::pixel(36);
  topBar.style.backgroundColor = [] { return theme::topBarBg; };

  Text title;
  title.label = std::string("untitled.txt");
  title.fontSize = 13;
  title.color = [] { return theme::titleText; };
  topBar.addChild(title);

  // A plain shared string the status Text polls every dispatch cycle
  // (click/key/timer) via its Dynamic<std::string> label — same idiom
  // TextInput/TextArea use for their own dynamic fields.
  Text statusText;
  statusText.label = [statusLabel] { return *statusLabel; };
  statusText.fontSize = 12;
  statusText.color = [] { return theme::statusText; };
  topBar.addChild(statusText);

  // ---- font size controls: click to shrink/grow the editor's text ----
  // editorState->fontSize is read fresh by onPaint/onPressAt/onKeyDown
  // (see toTextAreaView) rather than captured once, so just mutating it
  // and flagging dirty is enough to make the change show up — no need
  // to rebuild the TextArea's View.
  constexpr float kFontSizeStep = 1.0f;
  constexpr float kMinFontSize = 8.0f;
  constexpr float kMaxFontSize = 48.0f;

  Text decreaseFontText;
  decreaseFontText.label = std::string("A-");
  decreaseFontText.fontSize = 12;
  decreaseFontText.color = [] { return theme::statusText; };

  View decreaseFontBtn;
  decreaseFontBtn.style.padding = EdgeInsets{2, 8, 2, 8};
  decreaseFontBtn.style.borderRadius = 4.0f;
  decreaseFontBtn.style.hoverColor = theme::topBarBg;
  decreaseFontBtn.addChild(decreaseFontText);
  decreaseFontBtn.onClick = [editorState, kMinFontSize, kFontSizeStep] {
    editorState->fontSize =
        std::max(kMinFontSize, editorState->fontSize - kFontSizeStep);
    editorState->dirty = true;
  };
  topBar.addChild(decreaseFontBtn);

  // Live label showing the current size — polled every dispatch cycle
  // via Text's Dynamic<std::string> label, same idiom as statusText.
  Text fontSizeLabel;
  fontSizeLabel.label = [editorState] {
    return std::to_string(static_cast<int>(editorState->fontSize)) + "px";
  };
  fontSizeLabel.fontSize = 12;
  fontSizeLabel.color = [] { return theme::statusText; };
  topBar.addChild(fontSizeLabel);

  Text increaseFontText;
  increaseFontText.label = std::string("A+");
  increaseFontText.fontSize = 12;
  increaseFontText.color = [] { return theme::statusText; };

  View increaseFontBtn;
  increaseFontBtn.style.padding = EdgeInsets{2, 8, 2, 8};
  increaseFontBtn.style.borderRadius = 4.0f;
  increaseFontBtn.style.hoverColor = theme::topBarBg;
  increaseFontBtn.addChild(increaseFontText);
  increaseFontBtn.onClick = [editorState, kMaxFontSize, kFontSizeStep] {
    editorState->fontSize =
        std::min(kMaxFontSize, editorState->fontSize + kFontSizeStep);
    editorState->dirty = true;
  };
  topBar.addChild(increaseFontBtn);

  // ---- theme toggle: click to cycle Light -> Dark -> Monokai -> ... ----
  auto themeLabel =
      std::make_shared<std::string>(theme::name(theme::current()));
  Text themeText;
  themeText.label = [themeLabel] { return "Theme: " + *themeLabel; };
  themeText.fontSize = 12;
  themeText.color = [] { return theme::statusText; };

  View themeBtn;
  themeBtn.style.padding = EdgeInsets{2, 8, 2, 8};
  themeBtn.style.borderRadius = 4.0f;
  themeBtn.style.hoverColor = theme::topBarBg;
  themeBtn.addChild(themeText);

  themeBtn.onClick = [themeLabel, editorState] {
    theme::cycleTheme();
    *themeLabel = theme::name(theme::current());
    editorState->dirty = true;
  };
  topBar.addChild(themeBtn);

  root.addChild(topBar);

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