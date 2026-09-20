// liteui_editor_ext.hpp

#pragma once

#include "liteui.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

// ==================== Clipboard ====================
//
// liteui.hpp has no clipboard primitive today, so this adds one. Real OS
// clipboard access on Linux means either implementing the Wayland
// wl_data_device protocol (a fair amount of extra protocol plumbing) or
// shelling out to a helper binary — this takes the second, pragmatic route,
// the same way liteui.hpp's own openFilePicker/saveFilePicker shell out to
// zenity/kdialog rather than writing a native file-chooser dialog.
namespace liteui_clipboard {

// Same-process fallback: used when no OS clipboard mechanism is available
// (e.g. a container with no wl-copy/xclip/xsel installed) so copy/paste
// between two CodeEditors in the same app still works.
inline std::string &internalFallback() {
  static std::string s;
  return s;
}

#if defined(_WIN32)

inline bool setText(const std::string &utf8) {
  if (!OpenClipboard(nullptr))
    return false;
  EmptyClipboard();
  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (wlen <= 0) {
    CloseClipboard();
    return false;
  }
  HGLOBAL hMem =
      GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(wchar_t));
  if (!hMem) {
    CloseClipboard();
    return false;
  }
  wchar_t *dst = static_cast<wchar_t *>(GlobalLock(hMem));
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, dst, wlen);
  GlobalUnlock(hMem);
  SetClipboardData(CF_UNICODETEXT, hMem); // clipboard now owns hMem
  CloseClipboard();
  return true;
}

inline std::string getText() {
  if (!OpenClipboard(nullptr))
    return {};
  std::string result;
  HANDLE h = GetClipboardData(CF_UNICODETEXT);
  if (h) {
    wchar_t *w = static_cast<wchar_t *>(GlobalLock(h));
    if (w) {
      int len =
          WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
      std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
      if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
      result = std::move(s);
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return result;
}

#else // Linux: shell out to whatever clipboard tool is on PATH.

namespace detail {
inline bool commandExists(const char *name) {
  std::string check = std::string("command -v ") + name + " >/dev/null 2>&1";
  return std::system(check.c_str()) == 0;
}
} // namespace detail

inline bool setText(const std::string &utf8) {
  std::string cmd;
  if (detail::commandExists("wl-copy"))
    cmd = "wl-copy";
  else if (detail::commandExists("xclip"))
    cmd = "xclip -selection clipboard";
  else if (detail::commandExists("xsel"))
    cmd = "xsel --clipboard --input";
  else
    return false;
  FILE *pipe = popen((cmd + " 2>/dev/null").c_str(), "w");
  if (!pipe)
    return false;
  if (!utf8.empty())
    fwrite(utf8.data(), 1, utf8.size(), pipe);
  pclose(pipe); // wl-copy/xclip/xsel fork and detach to hold ownership, so
                // this returns promptly rather than blocking until someone
                // else claims the selection
  return true;
}

inline std::string getText() {
  std::string cmd;
  if (detail::commandExists("wl-paste"))
    cmd = "wl-paste --no-newline";
  else if (detail::commandExists("xclip"))
    cmd = "xclip -selection clipboard -o";
  else if (detail::commandExists("xsel"))
    cmd = "xsel --clipboard --output";
  else
    return {};
  FILE *pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
  if (!pipe)
    return {};
  std::string result;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0)
    result.append(buf, n);
  pclose(pipe);
  return result;
}

#endif

// What CodeEditor actually calls: try the real clipboard, keep the
// in-process fallback in sync either way so paste still works if the OS
// clipboard round-trip silently failed (e.g. sandboxed/headless).
inline void setTextWithFallback(const std::string &s) {
  internalFallback() = s;
  setText(s);
}
inline std::string getTextWithFallback() {
  std::string s = getText();
  if (!s.empty())
    return s;
  return internalFallback();
}

} // namespace liteui_clipboard

// ==================== UTF-8 helpers ====================
//
// Every cursor/selection byte offset used below is required to sit on a
// codepoint boundary. These two functions are the only place that ever
// moves a byte offset by "one character" — everything else (arrow keys,
// backspace, click-to-position rounding) goes through them, so a stray
// continuation byte can never end up as a cursor position.
namespace liteui_utf8 {

inline bool isContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

inline size_t prevBoundary(const std::string &s, size_t pos) {
  if (pos == 0)
    return 0;
  --pos;
  while (pos > 0 && isContinuation(static_cast<unsigned char>(s[pos])))
    --pos;
  return pos;
}

inline size_t nextBoundary(const std::string &s, size_t pos) {
  if (pos >= s.size())
    return s.size();
  ++pos;
  while (pos < s.size() && isContinuation(static_cast<unsigned char>(s[pos])))
    ++pos;
  return pos;
}

// Rounds an arbitrary byte offset down to the nearest codepoint boundary —
// used after operations (like line-splicing) that could otherwise land a
// cursor mid-sequence.
inline size_t snapToBoundary(const std::string &s, size_t pos) {
  pos = std::min(pos, s.size());
  while (pos > 0 && isContinuation(static_cast<unsigned char>(s[pos])))
    --pos;
  return pos;
}

inline void appendCodepoint(std::string &out, uint32_t cp) {
  liteui_xml::appendUtf8(out, cp);
}

} // namespace liteui_utf8

// ==================== Buffer + selection state ====================

struct EditPos {
  size_t line = 0, col = 0; // col is a byte offset, always codepoint-aligned
  bool operator==(const EditPos &o) const {
    return line == o.line && col == o.col;
  }
  bool operator!=(const EditPos &o) const { return !(*this == o); }
  bool operator<(const EditPos &o) const {
    return line < o.line || (line == o.line && col < o.col);
  }
};

struct CodeEditorSnapshot {
  std::vector<std::string> lines;
  EditPos cursor;
};

// ==================== bracket matching ====================
//
// Free functions rather than CodeEditorState methods since they only need
// read access to `lines` — used both for bracket auto-close/pairing and
// for the matching-bracket highlight in onPaint.

inline bool isOpenBracket(char c) { return c == '(' || c == '[' || c == '{'; }
inline bool isCloseBracket(char c) { return c == ')' || c == ']' || c == '}'; }
inline char matchingCloseFor(char open) {
  switch (open) {
  case '(':
    return ')';
  case '[':
    return ']';
  case '{':
    return '}';
  default:
    return '\0';
  }
}
inline char matchingOpenFor(char close) {
  switch (close) {
  case ')':
    return '(';
  case ']':
    return '[';
  case '}':
    return '{';
  default:
    return '\0';
  }
}

// Scans forward (for an open bracket) or backward (for a close bracket)
// from `from`, tracking nesting depth so e.g. "(a(b)c|)" from the second
// '(' correctly skips the inner "(b)" pair rather than matching its ')'.
// Capped at kMaxScan characters so a file with a genuinely unmatched
// bracket can't turn every keystroke into an unbounded whole-file scan.
inline std::optional<EditPos>
findMatchingBracket(const std::vector<std::string> &lines, EditPos from) {
  if (from.line >= lines.size() || from.col >= lines[from.line].size())
    return std::nullopt;
  char c = lines[from.line][from.col];
  constexpr int kMaxScan = 50000;
  int scanned = 0;
  if (isOpenBracket(c)) {
    char close = matchingCloseFor(c);
    int depth = 0;
    for (size_t li = from.line; li < lines.size(); ++li) {
      size_t startCol = (li == from.line) ? from.col : 0;
      const std::string &line = lines[li];
      for (size_t ci = startCol; ci < line.size(); ++ci) {
        if (++scanned > kMaxScan)
          return std::nullopt;
        char cc = line[ci];
        if (cc == c)
          ++depth;
        else if (cc == close) {
          --depth;
          if (depth == 0)
            return EditPos{li, ci};
        }
      }
    }
    return std::nullopt;
  }
  if (isCloseBracket(c)) {
    char open = matchingOpenFor(c);
    int depth = 0;
    for (size_t li = from.line + 1; li-- > 0;) { // safe reverse over size_t
      const std::string &line = lines[li];
      size_t endCol = (li == from.line) ? from.col : line.size();
      for (size_t ci = endCol; ci-- > 0;) {
        if (++scanned > kMaxScan)
          return std::nullopt;
        char cc = line[ci];
        if (cc == c)
          ++depth;
        else if (cc == open) {
          --depth;
          if (depth == 0)
            return EditPos{li, ci};
        }
      }
      if (li == 0)
        break;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

// One document's find/replace session. Kept as part of CodeEditorState
// (like selectionAnchor/undo history) so it persists across tab-rebuild
// cycles exactly like everything else about the document, and so
// Ctrl+F/Ctrl+H can be a single per-document toggle rather than needing
// any separate widget or focus-management machinery — see the note on
// CodeEditor::onKeyDown's search-mode handling for why it's implemented
// this way instead of as a real separate TextInput field.
struct SearchState {
  bool active = false;
  bool replaceMode = false;
  int fieldFocus = 0; // 0 = query field, 1 = replacement field
  std::string query, replacement;
  std::vector<EditPos> matches; // match start positions; length == query.size()
  int currentMatch = -1;
  // Cursor position captured once, when Ctrl+F/Ctrl+H is pressed — used as
  // the fixed reference point for "jump to the nearest match" every time
  // the query is edited. Deliberately NOT the live cursor: jumpToMatch()
  // itself moves the cursor to select each match, so using the live
  // cursor as the reference would make it drift forward through matches
  // as more of the query is typed, chasing whatever the previous
  // character's jump just landed on instead of staying anchored to where
  // the search actually started.
  EditPos origin;
};

inline std::string asciiLower(const std::string &s) {
  std::string out = s;
  for (char &c : out)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  return out;
}

struct CodeEditorState {
  std::vector<std::string> lines{std::string()}; // always >= 1 entry
  EditPos cursor;
  // Anchor of an in-progress or completed selection. No selection is
  // represented as either nullopt or (rarely) an anchor equal to cursor —
  // hasSelection() below is the single source of truth for "is anything
  // actually selected", so callers never need to check both.
  std::optional<EditPos> selectionAnchor;
  // Where the most recent mouse press landed, tracked independently of
  // selectionAnchor. A plain click clears selectionAnchor entirely (no
  // selection) but still needs to remember its own position in case the
  // next event is a drag rather than a release — that's what this is for.
  // Keeping it separate from selectionAnchor is what prevents a bug where
  // a click leaves anchor == cursor (meant to mean "no selection"), and a
  // subsequent edit that moves cursor but never touches anchor makes them
  // unequal again — i.e. a selection appearing out of a stale anchor with
  // no drag ever having happened.
  EditPos dragAnchor;

  bool focused = false;
  bool blinkOn = true;
  bool dirty = true;
  int blinkTimerHandle = -1;
  float scrollX = 0.0f, scrollY = 0.0f;
  float lastViewW = 0.0f, lastViewH = 0.0f;

  bool manualScroll = false; // true while the user is scrolling via wheel/
                             // scrollbar rather than moving the caret —
                             // suppresses onPaint's caret-follow snap

  bool scrollbarDragging = false;
  float scrollbarDragStartY = 0.0f;
  float scrollbarDragStartScrollY = 0.0f;

  bool hScrollbarDragging = false;
  float hScrollbarDragStartX = 0.0f;
  float hScrollbarDragStartScrollX = 0.0f;

  // ---- undo/redo ----
  // Snapshot-based: correct and simple, at the cost of copying the whole
  // buffer per undo step. Fine up to the sizes a text-editing session
  // realistically coalesces down to (see kCoalesceLimit); a piece-table
  // buffer with structural diffs is the natural upgrade if this ever
  // shows up as a real cost on huge files.
  std::vector<CodeEditorSnapshot> undoStack, redoStack;
  enum class LastEdit { None, TypingChars, Other } lastEdit = LastEdit::None;
  int coalesceCount = 0;
  static constexpr size_t kMaxUndo = 1000;
  static constexpr int kCoalesceLimit = 40; // chars per undo step while typing

  CodeEditorSnapshot snapshot() const { return {lines, cursor}; }

  void restore(const CodeEditorSnapshot &s) {
    lines = s.lines;
    if (lines.empty())
      lines.push_back(std::string());
    cursor = s.cursor;
    clampCursor();
    selectionAnchor.reset();
  }

  void clampCursor() {
    if (cursor.line >= lines.size())
      cursor.line = lines.size() - 1;
    cursor.col = liteui_utf8::snapToBoundary(
        lines[cursor.line], std::min(cursor.col, lines[cursor.line].size()));
  }

  void pushUndo() {
    undoStack.push_back(snapshot());
    if (undoStack.size() > kMaxUndo)
      undoStack.erase(undoStack.begin());
  }

  // Call before mutating the buffer. `coalescable` should be true only for
  // plain single-character typing (onTextInput with no selection) — every
  // other edit (paste, cut, newline, backspace/delete, indent) is its own
  // undo step so undo doesn't merge unrelated actions together.
  void beginEdit(bool coalescable) {
    manualScroll = false;
    redoStack.clear(); // a fresh edit invalidates the redo history
    if (!coalescable) {
      pushUndo();
      lastEdit = LastEdit::Other;
      coalesceCount = 0;
      return;
    }
    if (lastEdit != LastEdit::TypingChars || coalesceCount >= kCoalesceLimit)
      pushUndo();
    lastEdit = LastEdit::TypingChars;
    ++coalesceCount;
  }

  // Call after a pure cursor move (arrow keys, click) that isn't itself an
  // edit, so the next keystroke doesn't get coalesced into whatever typing
  // happened before the cursor moved elsewhere.
  void noteCursorMoved() {
    lastEdit = LastEdit::None;
    manualScroll = false;
  }

  void undo() {
    if (undoStack.empty())
      return;
    manualScroll = false;
    redoStack.push_back(snapshot());
    restore(undoStack.back());
    undoStack.pop_back();
    lastEdit = LastEdit::None;
    dirty = true;
  }

  void redo() {
    if (redoStack.empty())
      return;
    manualScroll = false;
    undoStack.push_back(snapshot());
    restore(redoStack.back());
    redoStack.pop_back();
    lastEdit = LastEdit::None;
    dirty = true;
  }

  // ---- selection ----

  bool hasSelection() const {
    return selectionAnchor.has_value() && *selectionAnchor != cursor;
  }

  void selectionRange(EditPos &start, EditPos &end) const {
    EditPos a = selectionAnchor.value_or(cursor);
    if (a < cursor) {
      start = a;
      end = cursor;
    } else {
      start = cursor;
      end = a;
    }
  }

  std::string selectedText() const {
    if (!hasSelection())
      return {};
    EditPos s, e;
    selectionRange(s, e);
    if (s.line == e.line)
      return lines[s.line].substr(s.col, e.col - s.col);
    std::string out = lines[s.line].substr(s.col);
    for (size_t l = s.line + 1; l < e.line; ++l) {
      out += '\n';
      out += lines[l];
    }
    out += '\n';
    out += lines[e.line].substr(0, e.col);
    return out;
  }

  // Removes the selected range (if any) and leaves the cursor at the
  // deletion point. Caller must call beginEdit() first — this only ever
  // touches the buffer, never the undo stack, so it composes cleanly with
  // "delete selection, then insert X" as a single undo step.
  void deleteSelectionRaw() {
    if (!hasSelection())
      return;
    EditPos s, e;
    selectionRange(s, e);
    if (s.line == e.line) {
      lines[s.line].erase(s.col, e.col - s.col);
    } else {
      lines[s.line] =
          lines[s.line].substr(0, s.col) + lines[e.line].substr(e.col);
      lines.erase(lines.begin() + static_cast<long>(s.line) + 1,
                  lines.begin() + static_cast<long>(e.line) + 1);
    }
    cursor = s;
    selectionAnchor.reset();
  }

  void selectAll() {
    manualScroll = false;
    selectionAnchor = EditPos{0, 0};
    cursor = EditPos{lines.size() - 1, lines.back().size()};
  }

  // Inserts `text` (which may itself contain '\n') at the cursor,
  // replacing the current selection if any. Caller must call beginEdit()
  // first. Leaves the cursor after the inserted text. A CodeEditorState
  // method (not just a toCodeEditorView-local lambda) specifically so
  // search/replace below can reuse it for inserting a replacement.
  void insertTextRaw(const std::string &text) {
    deleteSelectionRaw();
    size_t start = 0;
    bool first = true;
    while (true) {
      size_t nl = text.find('\n', start);
      std::string piece = text.substr(
          start, nl == std::string::npos ? std::string::npos : nl - start);
      if (first) {
        lines[cursor.line].insert(cursor.col, piece);
        cursor.col += piece.size();
        first = false;
      } else {
        std::string rest = lines[cursor.line].substr(cursor.col);
        lines[cursor.line].erase(cursor.col);
        lines.insert(lines.begin() + static_cast<long>(cursor.line) + 1,
                     piece + rest);
        ++cursor.line;
        cursor.col = piece.size();
      }
      if (nl == std::string::npos)
        break;
      start = nl + 1;
    }
    selectionAnchor.reset();
  }

  // ---- find/replace ----

  SearchState search;

  void enterSearch(bool replaceMode) {
    search.active = true;
    search.replaceMode = replaceMode;
    search.fieldFocus = 0;
    search.origin = cursor; // fixed reference point — see SearchState::origin
    if (hasSelection() && !selectedText().empty() &&
        selectedText().find('\n') == std::string::npos)
      search.query = selectedText(); // seed with the current selection, a
                                     // small but expected quality-of-life
                                     // touch ("select a word, hit Ctrl+F")
    recomputeMatches();
  }

  void exitSearch() {
    search.active = false;
    search.matches.clear();
    search.currentMatch = -1;
    // Deliberately keep search.query/replacement so reopening Ctrl+F
    // remembers the last search.
  }

  // Case-insensitive (ASCII only — see asciiLower's own limitation),
  // non-overlapping substring search across the whole buffer. Re-run on
  // every query edit; simple linear scan rather than an incremental
  // index, which is fine for interactive typing on realistic file sizes
  // but would be the thing to revisit for a "search across a huge file on
  // every keystroke" complaint.
  void recomputeMatches() {
    search.matches.clear();
    search.currentMatch = -1;
    if (search.query.empty())
      return;
    std::string needle = asciiLower(search.query);
    for (size_t li = 0; li < lines.size(); ++li) {
      std::string hay = asciiLower(lines[li]);
      size_t pos = 0;
      while ((pos = hay.find(needle, pos)) != std::string::npos) {
        search.matches.push_back(EditPos{li, pos});
        pos += needle.size();
      }
    }
    if (!search.matches.empty())
      jumpToMatch(closestMatchTo(search.origin));
  }

  // Index of the match at-or-after `pos` (wrapping to 0 if `pos` is past
  // the last match) — used so recomputing matches while typing a query
  // jumps to the nearest match ahead of the cursor rather than always
  // snapping back to the very first one in the file.
  int closestMatchTo(EditPos pos) const {
    for (size_t i = 0; i < search.matches.size(); ++i)
      if (!(search.matches[i] < pos))
        return static_cast<int>(i);
    return 0;
  }

  // Selects match `idx` using the ordinary selection mechanism — the
  // current match is deliberately just a normal selection under the
  // hood, so it's already rendered by the existing selection-highlight
  // code and already works with Ctrl+C/replace without any special-casing.
  void jumpToMatch(int idx) {
    manualScroll = false;
    if (idx < 0 || idx >= static_cast<int>(search.matches.size())) {
      search.currentMatch = -1;
      return;
    }
    search.currentMatch = idx;
    EditPos start = search.matches[static_cast<size_t>(idx)];
    EditPos end = {start.line, start.col + search.query.size()};
    selectionAnchor = start;
    cursor = end;
  }

  void nextMatch() {
    if (search.matches.empty())
      return;
    jumpToMatch((search.currentMatch + 1) %
                static_cast<int>(search.matches.size()));
  }
  void prevMatch() {
    if (search.matches.empty())
      return;
    int n = static_cast<int>(search.matches.size());
    jumpToMatch((search.currentMatch - 1 + n) % n);
  }

  // Replaces the current match (selection) with search.replacement, as
  // one undo step, then re-syncs so the next Enter can advance again
  // (the replaced text has shifted every later match's stored position).
  void replaceCurrentMatch() {
    if (search.currentMatch < 0 || search.matches.empty())
      return;
    beginEdit(false);
    insertTextRaw(search.replacement);
    recomputeMatches();
  }

  // All matches, as a single undo step. Applied from last to first so
  // each replacement's own position never shifts an earlier match's
  // still-pending stored EditPos out from under it. Matches are captured
  // fresh at the start and NOT re-scanned mid-loop, so a replacement that
  // happens to contain the search text again is deliberately not visited
  // a second time (avoids e.g. replacing "a" with "aa" looping forever).
  void replaceAllMatches() {
    recomputeMatches();
    if (search.matches.empty())
      return;
    beginEdit(false);
    for (size_t i = search.matches.size(); i-- > 0;) {
      EditPos start = search.matches[i];
      lines[start.line].erase(start.col, search.query.size());
      lines[start.line].insert(start.col, search.replacement);
    }
    selectionAnchor.reset();
    clampCursor();
    search.matches.clear();
    search.currentMatch = -1;
  }
};

inline std::string joinLines(const std::vector<std::string> &lines) {
  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out += '\n';
    out += lines[i];
  }
  return out;
}

// ==================== Author-facing CodeEditor ====================

struct HighlightSpan {
  size_t start = 0, length = 0;
  Color color;
};

struct CodeEditor {
  Style style;
  std::string text; // initial content; split on '\n'
  std::string placeholder;
  float fontSize = 14.0f;
  FontWeight fontWeight = FontWeight::Regular;
  std::string fontFamily = "Monospace"; // code wants a monospace default,
                                        // unlike editor_example.cpp's TextArea
  Color textColor = Color{20, 20, 20};
  Color placeholderColor = Color{160, 160, 160};
  Color caretColor = Color{20, 20, 20};
  Color selectionColor =
      Color{51, 153, 255, 90}; // translucent selection highlight
  Color borderColor = Color{180, 180, 180};
  Color focusedBorderColor = Color{50, 120, 220};
  Color lineNumberColor = Color{170, 170, 170};
  Color lineNumberBackground = Color{245, 245, 245};
  bool showLineNumbers = true;
  float padding = 8.0f;
  float lineHeight = 0.0f; // 0 = auto (fontSize * 1.4)
  int tabWidthSpaces = 4;

  bool showScrollbar = true;
  float scrollbarWidth = 10.0f;
  Color scrollbarTrackColor = Color{0, 0, 0, 20};
  Color scrollbarThumbColor = Color{0, 0, 0, 90};

  std::function<void(const std::string &)> onChange;

  std::function<void(const std::vector<std::string> &lines)> beforePaintSync;
  std::function<void(size_t lineIndex, const std::string &lineText,
                     std::vector<HighlightSpan> &outSpans)>
      highlightLine;

  std::function<bool(KeyEvent)> preKeyDown;
  std::function<bool(uint32_t)> preTextInput;
  std::function<void(CanvasContext &)> postPaint;
  bool resetStateFromText = true;

  std::shared_ptr<CodeEditorState> state = std::make_shared<CodeEditorState>();
};

inline View toCodeEditorView(CodeEditor ed) {
  auto state = ed.state;

  if (ed.resetStateFromText) {
    state->lines.clear();
    {
      std::string cur;
      for (char c : ed.text) {
        if (c == '\n') {
          state->lines.push_back(cur);
          cur.clear();
        } else {
          cur += c;
        }
      }
      state->lines.push_back(cur);
    }
    state->cursor = {state->lines.size() - 1, state->lines.back().size()};
  } else {
    state->clampCursor(); // defensive: guards against a state handed in
                          // with a cursor left pointing past the end of
                          // a since-shortened buffer
  }

  TextStyle ts;
  ts.fontSize = ed.fontSize;
  ts.fontWeight = ed.fontWeight;
  ts.fontFamily = ed.fontFamily;
  ts.wrap = TextWrap::NoWrap;

  float lineH = ed.lineHeight > 0 ? ed.lineHeight : ed.fontSize * 1.4f;
  float pad = ed.padding;
  bool showNums = ed.showLineNumbers;
  int tabSpaces = std::max(1, ed.tabWidthSpaces);
  Color textColor = ed.textColor;
  Color placeholderColor = ed.placeholderColor;
  Color caretColor = ed.caretColor;
  Color selectionColor = ed.selectionColor;
  Color lineNumColor = ed.lineNumberColor;
  Color lineNumBg = ed.lineNumberBackground;
  std::string placeholder = ed.placeholder;
  auto onChange = ed.onChange;
  Color borderColor = ed.borderColor;
  Color focusedBorderColor = ed.focusedBorderColor;
  bool showScrollbar = ed.showScrollbar;
  float scrollbarWidth = ed.scrollbarWidth;
  Color scrollbarTrackColor = ed.scrollbarTrackColor;
  Color scrollbarThumbColor = ed.scrollbarThumbColor;
  auto preKeyDown = ed.preKeyDown;
  auto preTextInput = ed.preTextInput;
  auto postPaint = ed.postPaint;

  View v;
  v.style = std::move(ed.style);
  v.isCanvas = true;
  v.focusable = true;

  v.style.borderColor = [state, borderColor, focusedBorderColor] {
    return state->focused ? focusedBorderColor : borderColor;
  };

  v.canvasDirtySource = [state] {
    bool d = state->dirty;
    state->dirty = false;
    return d;
  };

  auto vScrollbarGeometry = [](CodeEditorState *state, float lineH, float viewH,
                               float viewW, float barW) {
    struct Geo {
      bool visible;
      float trackY, trackH, thumbY, thumbH, x;
    };
    float contentH = state->lines.size() * lineH;
    if (contentH <= viewH)
      return Geo{false, 0, 0, 0, 0, 0};
    float trackY = 0, trackH = viewH;
    float thumbH = std::max(20.0f, trackH * (viewH / contentH));
    float maxScroll = std::max(0.0f, contentH - viewH);
    float thumbY =
        maxScroll > 0 ? (state->scrollY / maxScroll) * (trackH - thumbH) : 0;
    return Geo{true, trackY, trackH, thumbY, thumbH, viewW - barW};
  };

  auto hScrollbarGeometry = [](CodeEditorState *state, const TextStyle &ts,
                               float textLeft, float viewW, float viewH,
                               float barH) {
    struct Geo {
      bool visible;
      float trackX, trackW, thumbX, thumbW, y;
    };
    float maxLineW = 0.0f;
    for (const auto &l : state->lines) {
      float w = liteui_text::measure(l, ts, -1).width;
      if (w > maxLineW)
        maxLineW = w;
    }
    float viewportW = std::max(0.0f, viewW - textLeft);
    if (maxLineW <= viewportW)
      return Geo{false, 0, 0, 0, 0, 0};
    float trackX = textLeft, trackW = viewportW;
    float thumbW = std::max(20.0f, trackW * (viewportW / maxLineW));
    float maxScroll = std::max(0.0f, maxLineW - viewportW);
    float thumbX =
        maxScroll > 0 ? (state->scrollX / maxScroll) * (trackW - thumbW) : 0;
    return Geo{true, trackX, trackW, thumbX, thumbW, viewH - barH};
  };

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
    return m.width + 12.0f;
  };

  auto notifyChange = [state, onChange] {
    if (onChange)
      onChange(joinLines(state->lines));
  };
  auto beforePaintSync = ed.beforePaintSync;
  auto highlightLine = ed.highlightLine;

  // ---- shared editing primitives, used by both keyboard and clipboard paths
  // ----

  // Thin wrapper so every existing call site below (Ctrl+V, Enter, Tab,
  // onTextInput) keeps working unchanged — the real logic now lives on
  // CodeEditorState itself (see its own insertTextRaw), so search/replace
  // can reuse it too.
  auto insertTextRaw = [state](const std::string &text) {
    state->insertTextRaw(text);
  };

  auto copySelection = [state] {
    if (state->hasSelection())
      liteui_clipboard::setTextWithFallback(state->selectedText());
  };

  // ---- cursor motion (UTF-8 aware) ----

  auto moveLeft = [state] {
    EditPos &c = state->cursor;
    if (c.col > 0) {
      c.col = liteui_utf8::prevBoundary(state->lines[c.line], c.col);
    } else if (c.line > 0) {
      --c.line;
      c.col = state->lines[c.line].size();
    }
  };
  auto moveRight = [state] {
    EditPos &c = state->cursor;
    if (c.col < state->lines[c.line].size()) {
      c.col = liteui_utf8::nextBoundary(state->lines[c.line], c.col);
    } else if (c.line + 1 < state->lines.size()) {
      ++c.line;
      c.col = 0;
    }
  };
  // Column tracking across Up/Down uses the on-screen pixel x position
  // rather than the byte offset, so moving through lines with different
  // multi-byte content still keeps the cursor visually aligned.
  auto pixelColOf = [state, ts](const EditPos &p) {
    return liteui_text::measure(state->lines[p.line].substr(0, p.col), ts, -1)
        .width;
  };
  auto colAtPixel = [state, ts](size_t line, float px) {
    return liteui_utf8::snapToBoundary(
        state->lines[line], liteui_text::caretIndexForX(state->lines[line], ts,
                                                        std::max(0.0f, px)));
  };
  auto moveUp = [state, pixelColOf, colAtPixel] {
    EditPos &c = state->cursor;
    if (c.line == 0)
      return;
    float px = pixelColOf(c);
    --c.line;
    c.col = colAtPixel(c.line, px);
  };
  auto moveDown = [state, pixelColOf, colAtPixel] {
    EditPos &c = state->cursor;
    if (c.line + 1 >= state->lines.size())
      return;
    float px = pixelColOf(c);
    ++c.line;
    c.col = colAtPixel(c.line, px);
  };
  auto moveHome = [state] { state->cursor.col = 0; };
  auto moveEnd = [state] {
    state->cursor.col = state->lines[state->cursor.line].size();
  };

  // Ctrl+Left/Right word jump: skip any run of whitespace, then a run of
  // "word" characters (alnum/underscore) or, failing that, one run of
  // punctuation — enough to feel like every other editor's word-jump
  // without pulling in real Unicode word-break rules.
  auto isWordByte = [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80; // treat UTF-8
                                                     // continuation/lead
                                                     // bytes as word bytes
                                                     // so multi-byte text
                                                     // doesn't fragment
                                                     // mid-codepoint
  };
  auto moveWordLeft = [state, isWordByte] {
    EditPos &c = state->cursor;
    if (c.col == 0) {
      if (c.line > 0) {
        --c.line;
        c.col = state->lines[c.line].size();
      }
      return;
    }
    const std::string &s = state->lines[c.line];
    size_t i = c.col;
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1])))
      --i;
    if (i > 0) {
      bool word = isWordByte(static_cast<unsigned char>(s[i - 1]));
      while (i > 0 &&
             isWordByte(static_cast<unsigned char>(s[i - 1])) == word &&
             !std::isspace(static_cast<unsigned char>(s[i - 1])))
        --i;
    }
    c.col = i;
  };
  auto moveWordRight = [state, isWordByte] {
    EditPos &c = state->cursor;
    const std::string &s = state->lines[c.line];
    if (c.col >= s.size()) {
      if (c.line + 1 < state->lines.size()) {
        ++c.line;
        c.col = 0;
      }
      return;
    }
    size_t i = c.col;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
      ++i;
    if (i < s.size()) {
      bool word = isWordByte(static_cast<unsigned char>(s[i]));
      while (i < s.size() &&
             isWordByte(static_cast<unsigned char>(s[i])) == word &&
             !std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    }
    c.col = i;
  };

  v.onPaint = [=](CanvasContext &ctx) {
    state->lastViewW = ctx.width();
    state->lastViewH = ctx.height();
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    float availW = std::max(0.0f, ctx.width() - textLeft - pad);
    float availH = std::max(0.0f, ctx.height() - pad * 2.0f);

    float caretTop = state->cursor.line * lineH;
    if (!state->manualScroll) {
      if (caretTop - state->scrollY < 0)
        state->scrollY = caretTop;
      if (caretTop + lineH - state->scrollY > availH)
        state->scrollY = caretTop + lineH - availH;
    }
    float maxScrollY = std::max(0.0f, state->lines.size() * lineH - availH);
    state->scrollY = std::clamp(state->scrollY, 0.0f, maxScrollY);

    const std::string &curLine = state->lines[state->cursor.line];
    liteui_text::Measurement caretM =
        liteui_text::measure(curLine.substr(0, state->cursor.col), ts, -1);
    if (!state->manualScroll) {
      if (caretM.width - state->scrollX > availW)
        state->scrollX = caretM.width - availW;
      if (caretM.width - state->scrollX < 0)
        state->scrollX = caretM.width;
    }
    float maxLineW = 0.0f;
    for (const auto &l : state->lines) {
      float w = liteui_text::measure(l, ts, -1).width;
      if (w > maxLineW)
        maxLineW = w;
    }
    float maxScrollX = std::max(0.0f, maxLineW - availW);
    state->scrollX = std::clamp(state->scrollX, 0.0f, maxScrollX);

    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, ctx.width(), ctx.height());
    ctx.clip();

    if (gutter > 0) {
      ctx.setFillColor(lineNumBg);
      ctx.fillRect(0, 0, gutter + pad, ctx.height());
    }

    int firstLine = std::max(0, static_cast<int>(state->scrollY / lineH));
    int lastLine =
        std::min(static_cast<int>(state->lines.size()) - 1,
                 static_cast<int>((state->scrollY + availH) / lineH) + 1);

    // ---- selection highlight, drawn under the text ----
    if (state->hasSelection()) {
      EditPos s, e;
      state->selectionRange(s, e);
      ctx.setFillColor(selectionColor);
      for (int i = std::max(firstLine, static_cast<int>(s.line));
           i <= std::min(lastLine, static_cast<int>(e.line)); ++i) {
        const std::string &line = state->lines[static_cast<size_t>(i)];
        size_t from = (static_cast<size_t>(i) == s.line) ? s.col : 0;
        size_t to = (static_cast<size_t>(i) == e.line) ? e.col : line.size();
        float x0 = liteui_text::measure(line.substr(0, from), ts, -1).width;
        // A selection spanning past this line's own text (i.e. every line
        // except the last) visually extends to a fixed width past the
        // line's own end, matching how most editors show a selected
        // newline rather than stopping the highlight dead at the last
        // glyph.
        float x1 =
            (static_cast<size_t>(i) == e.line)
                ? liteui_text::measure(line.substr(0, to), ts, -1).width
                : liteui_text::measure(line, ts, -1).width + ts.fontSize * 0.5f;
        float y = pad + i * lineH - state->scrollY;
        ctx.fillRect(textLeft - state->scrollX + x0, y, std::max(1.0f, x1 - x0),
                     lineH);
      }
    }

    // ---- other search matches (the current one is already covered by
    // the selection highlight above, since jumpToMatch() just selects it)
    if (state->search.active && !state->search.matches.empty()) {
      ctx.setFillColor(Color{255, 230, 90, 130});
      for (size_t mi = 0; mi < state->search.matches.size(); ++mi) {
        if (static_cast<int>(mi) == state->search.currentMatch)
          continue;
        const EditPos &m = state->search.matches[mi];
        if (static_cast<int>(m.line) < firstLine ||
            static_cast<int>(m.line) > lastLine)
          continue;
        const std::string &line = state->lines[m.line];
        float x0 = liteui_text::measure(line.substr(0, m.col), ts, -1).width;
        float x1 =
            liteui_text::measure(
                line.substr(0, m.col + state->search.query.size()), ts, -1)
                .width;
        float y = pad + m.line * lineH - state->scrollY;
        ctx.fillRect(textLeft - state->scrollX + x0, y, std::max(1.0f, x1 - x0),
                     lineH);
      }
    }

    // ---- matching-bracket highlight: when the cursor sits immediately
    // before or after a bracket, outline both it and its pair ----
    if (!state->hasSelection()) {
      auto highlightBracketAt = [&](EditPos p) {
        auto match = findMatchingBracket(state->lines, p);
        if (!match)
          return;
        for (const EditPos &bp : {p, *match}) {
          if (static_cast<int>(bp.line) < firstLine ||
              static_cast<int>(bp.line) > lastLine)
            continue;
          const std::string &line = state->lines[bp.line];
          float x0 = liteui_text::measure(line.substr(0, bp.col), ts, -1).width;
          float x1 =
              liteui_text::measure(line.substr(0, bp.col + 1), ts, -1).width;
          float y = pad + bp.line * lineH - state->scrollY;
          ctx.setFillColor(Color{140, 170, 230, 110});
          ctx.fillRect(textLeft - state->scrollX + x0, y,
                       std::max(1.0f, x1 - x0), lineH);
        }
      };
      const std::string &curLineText = state->lines[state->cursor.line];
      if (state->cursor.col < curLineText.size() &&
          (isOpenBracket(curLineText[state->cursor.col]) ||
           isCloseBracket(curLineText[state->cursor.col])))
        highlightBracketAt(state->cursor);
      else if (state->cursor.col > 0 &&
               (isOpenBracket(curLineText[state->cursor.col - 1]) ||
                isCloseBracket(curLineText[state->cursor.col - 1])))
        highlightBracketAt(EditPos{state->cursor.line, state->cursor.col - 1});
    }

    ctx.setFont(ts.fontFamily, ts.fontSize, ts.fontWeight);
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);

    if (beforePaintSync)
      beforePaintSync(state->lines);

    bool empty = state->lines.size() == 1 && state->lines[0].empty();
    if (empty && !state->focused && !placeholder.empty()) {
      ctx.setFillColor(placeholderColor);
      ctx.fillText(placeholder, textLeft, pad + lineH / 2.0f);
    } else {
      std::vector<HighlightSpan>
          spans; // reused per line; cleared each iteration
      for (int i = firstLine; i <= lastLine; ++i) {
        float y = pad + i * lineH - state->scrollY + lineH / 2.0f;
        if (gutter > 0) {
          ctx.setFillColor(lineNumColor);
          ctx.setTextAlign(TextAlign::End);
          ctx.fillText(std::to_string(i + 1), textLeft - 8.0f, y);
          ctx.setTextAlign(TextAlign::Start);
        }
        const std::string &lineText = state->lines[static_cast<size_t>(i)];
        if (highlightLine) {
          spans.clear();
          highlightLine(static_cast<size_t>(i), lineText, spans);
          float x = textLeft - state->scrollX;
          size_t pos = 0;
          for (const auto &sp : spans) {
            if (sp.start > pos) {
              std::string gap = lineText.substr(pos, sp.start - pos);
              ctx.setFillColor(textColor);
              ctx.fillText(gap, x, y);
              x += liteui_text::measure(gap, ts, -1).width;
            }
            std::string tok = lineText.substr(sp.start, sp.length);
            ctx.setFillColor(sp.color);
            ctx.fillText(tok, x, y);
            x += liteui_text::measure(tok, ts, -1).width;
            pos = sp.start + sp.length;
          }
          if (pos < lineText.size()) {
            ctx.setFillColor(textColor);
            ctx.fillText(lineText.substr(pos), x, y);
          }
        } else {
          ctx.setFillColor(textColor);
          ctx.fillText(lineText, textLeft - state->scrollX, y);
        }
      }
    }

    if (state->focused && state->blinkOn) {
      float cy = pad + state->cursor.line * lineH - state->scrollY;
      liteui_text::Measurement cm = liteui_text::measure(
          state->lines[state->cursor.line].substr(0, state->cursor.col), ts,
          -1);
      ctx.setFillColor(caretColor);
      ctx.fillRect(textLeft - state->scrollX + cm.width, cy + 2.0f, 1.5f,
                   std::max(0.0f, lineH - 4.0f));
    }

    ctx.restore();

    // ---- find/replace overlay: drawn last, unclipped, so it floats over
    // the document regardless of scroll position ----
    if (state->search.active) {
      const auto &search = state->search;
      float boxW = 280.0f, rowH = 20.0f;
      int numRows =
          2 + (search.replaceMode ? 1 : 0); // query [+ replace] + hint
      float boxH = rowH * numRows + 10.0f;
      float bx = ctx.width() - boxW - 10.0f;
      float by = 8.0f;
      ctx.setFillColor(Color{35, 35, 35, 235});
      ctx.fillRect(bx, by, boxW, boxH);

      ctx.setFont(ts.fontFamily, 12.5f, FontWeight::Regular);
      ctx.setTextBaseline(TextBaseline::Middle);
      ctx.setTextAlign(TextAlign::Start);
      std::string caretMark = state->blinkOn ? "_" : " ";
      float ty = by + rowH / 2.0f + 5.0f;

      std::string matchInfo =
          search.matches.empty()
              ? (search.query.empty() ? std::string()
                                      : std::string("no matches"))
              : (std::to_string(search.currentMatch + 1) + "/" +
                 std::to_string(search.matches.size()));
      ctx.setFillColor(Color{255, 255, 255});
      ctx.fillText("Find: " + search.query +
                       (search.fieldFocus == 0 ? caretMark : "") + "   " +
                       matchInfo,
                   bx + 8.0f, ty);
      ty += rowH;

      if (search.replaceMode) {
        ctx.setFillColor(Color{255, 255, 255});
        ctx.fillText("Replace: " + search.replacement +
                         (search.fieldFocus == 1 ? caretMark : ""),
                     bx + 8.0f, ty);
        ty += rowH;
      }

      ctx.setFillColor(Color{175, 175, 175});
      std::string hint =
          search.replaceMode
              ? "Enter=replace  Tab=field  Ctrl+Enter=all  Esc=close"
              : "Enter=next  Shift+Enter=prev  Esc=close";
      ctx.fillText(hint, bx + 8.0f, ty);
    }

    if (showScrollbar) {
      auto vGeo = vScrollbarGeometry(state.get(), lineH, ctx.height(),
                                     ctx.width(), scrollbarWidth);
      if (vGeo.visible) {
        ctx.setFillColor(scrollbarTrackColor);
        ctx.fillRect(vGeo.x, vGeo.trackY, scrollbarWidth, vGeo.trackH);
        ctx.setFillColor(scrollbarThumbColor);
        ctx.fillRect(vGeo.x, vGeo.trackY + vGeo.thumbY, scrollbarWidth,
                     vGeo.thumbH);
      }
      auto hGeo = hScrollbarGeometry(state.get(), ts, textLeft, ctx.width(),
                                     ctx.height(), scrollbarWidth);
      if (hGeo.visible) {
        ctx.setFillColor(scrollbarTrackColor);
        ctx.fillRect(hGeo.trackX, hGeo.y, hGeo.trackW, scrollbarWidth);
        ctx.setFillColor(scrollbarThumbColor);
        ctx.fillRect(hGeo.trackX + hGeo.thumbX, hGeo.y, hGeo.thumbW,
                     scrollbarWidth);
      }
    }

    if (postPaint)
      postPaint(ctx);
  };

  auto posAtPoint = [=](float lx, float ly) -> EditPos {
    float gutter = gutterWidth(state->lines.size());
    float textLeft = pad + gutter;
    int line = static_cast<int>((ly - pad + state->scrollY) / lineH);
    line = std::clamp(line, 0, static_cast<int>(state->lines.size()) - 1);
    float localX = lx - textLeft + state->scrollX;
    size_t col = colAtPixel(static_cast<size_t>(line), localX);
    return {static_cast<size_t>(line), col};
  };

  v.onPressAt = [=](float lx, float ly) {
    float textLeft = pad + gutterWidth(state->lines.size());
    state->scrollbarDragging = false;
    state->hScrollbarDragging = false;
    if (showScrollbar) {
      auto hGeo =
          hScrollbarGeometry(state.get(), ts, textLeft, state->lastViewW,
                             state->lastViewH, scrollbarWidth);
      // Check the horizontal band first: in the bottom-right corner where
      // both bars could claim the point, the bottom strip visually belongs
      // to the horizontal bar, so it gets priority.
      if (hGeo.visible && ly >= hGeo.y) {
        state->manualScroll = true;
        state->hScrollbarDragging = true;
        state->hScrollbarDragStartX = lx;
        state->hScrollbarDragStartScrollX = state->scrollX;
        if (lx < hGeo.trackX + hGeo.thumbX ||
            lx > hGeo.trackX + hGeo.thumbX + hGeo.thumbW) {
          float maxLineW = 0.0f;
          for (const auto &l : state->lines) {
            float w = liteui_text::measure(l, ts, -1).width;
            if (w > maxLineW)
              maxLineW = w;
          }
          float viewportW = std::max(0.0f, state->lastViewW - textLeft);
          float maxScroll = std::max(0.0f, maxLineW - viewportW);
          float frac = (lx - hGeo.trackX - hGeo.thumbW / 2.0f) /
                       std::max(1.0f, hGeo.trackW - hGeo.thumbW);
          state->scrollX = std::clamp(frac * maxScroll, 0.0f, maxScroll);
        }
        state->dirty = true;
        return;
      }
      auto vGeo = vScrollbarGeometry(state.get(), lineH, state->lastViewH,
                                     state->lastViewW, scrollbarWidth);
      if (vGeo.visible && lx >= vGeo.x) {
        state->manualScroll = true;
        state->scrollbarDragging = true;
        state->scrollbarDragStartY = ly;
        state->scrollbarDragStartScrollY = state->scrollY;
        if (ly < vGeo.trackY + vGeo.thumbY ||
            ly > vGeo.trackY + vGeo.thumbY + vGeo.thumbH) {
          float contentH = state->lines.size() * lineH;
          float maxScroll = std::max(0.0f, contentH - state->lastViewH);
          float frac = (ly - vGeo.thumbH / 2.0f) /
                       std::max(1.0f, vGeo.trackH - vGeo.thumbH);
          state->scrollY = std::clamp(frac * maxScroll, 0.0f, maxScroll);
        }
        state->dirty = true;
        return;
      }
    }
    EditPos p = posAtPoint(lx, ly);
    state->cursor = p;
    state->dragAnchor = p;
    state->selectionAnchor.reset();
    state->noteCursorMoved();
    state->blinkOn = true;
    state->dirty = true;
  };

  v.onDragTo = [=](float lx, float ly) {
    float textLeft = pad + gutterWidth(state->lines.size());
    if (state->hScrollbarDragging) {
      float maxLineW = 0.0f;
      for (const auto &l : state->lines) {
        float w = liteui_text::measure(l, ts, -1).width;
        if (w > maxLineW)
          maxLineW = w;
      }
      float viewportW = std::max(0.0f, state->lastViewW - textLeft);
      float maxScroll = std::max(0.0f, maxLineW - viewportW);
      auto hGeo =
          hScrollbarGeometry(state.get(), ts, textLeft, state->lastViewW,
                             state->lastViewH, scrollbarWidth);
      float range = std::max(1.0f, hGeo.trackW - hGeo.thumbW);
      float delta = (lx - state->hScrollbarDragStartX) / range * maxScroll;
      state->scrollX = std::clamp(state->hScrollbarDragStartScrollX + delta,
                                  0.0f, maxScroll);
      state->manualScroll = true;
      state->dirty = true;
      return;
    }
    if (state->scrollbarDragging) {
      state->manualScroll = true;
      float contentH = state->lines.size() * lineH;
      float maxScroll = std::max(0.0f, contentH - state->lastViewH);
      auto vGeo = vScrollbarGeometry(state.get(), lineH, state->lastViewH,
                                     state->lastViewW, scrollbarWidth);
      float range = std::max(1.0f, vGeo.trackH - vGeo.thumbH);
      float delta = (ly - state->scrollbarDragStartY) / range * maxScroll;
      state->scrollY =
          std::clamp(state->scrollbarDragStartScrollY + delta, 0.0f, maxScroll);
      state->dirty = true;
      return;
    }
    EditPos p = posAtPoint(lx, ly);
    state->cursor = p;
    if (p != state->dragAnchor)
      state->selectionAnchor = state->dragAnchor;
    else
      state->selectionAnchor.reset();
    state->noteCursorMoved();
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
    state->manualScroll = true;
    state->scrollY = std::max(0.0f, state->scrollY - lineH * 3.0f);
    state->dirty = true;
  };
  v.onScrollDown = [state, lineH] {
    state->manualScroll = true;
    float maxScrollY =
        std::max(0.0f, state->lines.size() * lineH - state->lastViewH);
    state->scrollY = std::min(maxScrollY, state->scrollY + lineH * 3.0f);
    state->dirty = true;
  };

  v.onKeyDown = [=](KeyEvent e) {
    if (preKeyDown && preKeyDown(e)) {
      state->blinkOn = true;
      state->dirty = true;
      return;
    }

    EditPos &cur = state->cursor;
    bool shift = e.mods.shift;
    bool ctrl = e.mods.ctrl;
    bool handled = true;

    // ---- find/replace mode intercepts all keyboard input while active ----
    if (state->search.active) {
      auto &search = state->search;
      switch (e.key) {
      case Key::Escape:
        state->exitSearch();
        break;
      case Key::Tab:
        if (search.replaceMode)
          search.fieldFocus = 1 - search.fieldFocus;
        break;
      case Key::Backspace: {
        std::string &field =
            search.fieldFocus == 0 ? search.query : search.replacement;
        if (!field.empty())
          field.erase(liteui_utf8::prevBoundary(field, field.size()));
        if (search.fieldFocus == 0)
          state->recomputeMatches();
        break;
      }
      case Key::Enter:
        if (ctrl && search.replaceMode) {
          state->replaceAllMatches();
        } else if (search.fieldFocus == 1 && search.replaceMode) {
          state->replaceCurrentMatch();
        } else if (shift) {
          state->prevMatch();
        } else {
          state->nextMatch();
        }
        break;
      default:
        break;
      }
      state->blinkOn = true;
      state->dirty = true;
      return; // never falls through to normal document editing below
    }
    if (ctrl && !shift && e.key == Key::F) {
      state->enterSearch(false);
      state->blinkOn = true;
      state->dirty = true;
      return;
    }
    if (ctrl && !shift && e.key == Key::H) {
      state->enterSearch(true);
      state->blinkOn = true;
      state->dirty = true;
      return;
    }

    // Wraps a pure-motion key: applies shift-extends-selection semantics
    // uniformly, since every arrow/Home/End/PageUp/PageDown key follows
    // the same rule (start an anchor if shift is newly held, drop it
    // otherwise).
    auto move = [&](std::function<void()> fn) {
      if (shift) {
        if (!state->selectionAnchor)
          state->selectionAnchor = cur;
      } else {
        state->selectionAnchor.reset();
      }
      fn();
      state->noteCursorMoved();
    };

    switch (e.key) {
    case Key::Left:
      move(ctrl ? std::function<void()>(moveWordLeft)
                : std::function<void()>(moveLeft));
      break;
    case Key::Right:
      move(ctrl ? std::function<void()>(moveWordRight)
                : std::function<void()>(moveRight));
      break;
    case Key::Up:
      move(moveUp);
      break;
    case Key::Down:
      move(moveDown);
      break;
    case Key::Home:
      move(moveHome);
      break;
    case Key::End:
      move(moveEnd);
      break;
    case Key::PageUp: {
      float availH = std::max(lineH, state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH));
      move([&] {
        cur.line =
            static_cast<size_t>(std::max(0, static_cast<int>(cur.line) - step));
        cur.col = std::min(cur.col, state->lines[cur.line].size());
      });
      break;
    }
    case Key::PageDown: {
      float availH = std::max(lineH, state->lastViewH - pad * 2.0f);
      int step = std::max(1, static_cast<int>(availH / lineH));
      move([&] {
        cur.line = std::min(state->lines.size() - 1,
                            cur.line + static_cast<size_t>(step));
        cur.col = std::min(cur.col, state->lines[cur.line].size());
      });
      break;
    }

    case Key::A:
      if (ctrl) {
        state->selectAll();
        state->noteCursorMoved();
      } else
        handled = false;
      break;
    case Key::C:
      if (ctrl)
        copySelection();
      else
        handled = false;
      break;
    case Key::X:
      if (ctrl) {
        if (state->hasSelection()) {
          copySelection();
          state->beginEdit(false);
          state->deleteSelectionRaw();
          notifyChange();
        }
      } else
        handled = false;
      break;
    case Key::V:
      if (ctrl) {
        std::string clip = liteui_clipboard::getTextWithFallback();
        if (!clip.empty()) {
          state->beginEdit(false);
          insertTextRaw(clip);
          notifyChange();
        }
      } else
        handled = false;
      break;
    case Key::Z:
      if (ctrl) {
        if (shift)
          state->redo();
        else
          state->undo();
        notifyChange();
      } else
        handled = false;
      break;
    case Key::Y:
      if (ctrl) {
        state->redo();
        notifyChange();
      } else
        handled = false;
      break;

    case Key::Backspace:
      if (state->hasSelection()) {
        state->beginEdit(false);
        state->deleteSelectionRaw();
        notifyChange();
      } else if (cur.col > 0) {
        // Deleting right between a bracket pair that's still empty (e.g.
        // the "()" an auto-close just inserted, with nothing typed inside
        // yet) removes both characters as one edit, not just the open
        // one — matches every editor that does bracket auto-close.
        char before = state->lines[cur.line][cur.col - 1];
        bool deletePair = false;
        if (isOpenBracket(before) && cur.col < state->lines[cur.line].size() &&
            state->lines[cur.line][cur.col] == matchingCloseFor(before)) {
          deletePair = true;
        }
        state->beginEdit(!deletePair); // pair-deletion is its own undo step
        size_t start =
            liteui_utf8::prevBoundary(state->lines[cur.line], cur.col);
        state->lines[cur.line].erase(start, cur.col - start);
        if (deletePair)
          state->lines[cur.line].erase(start, 1);
        cur.col = start;
        notifyChange();
      } else if (cur.line > 0) {
        state->beginEdit(false);
        size_t prevLen = state->lines[cur.line - 1].size();
        state->lines[cur.line - 1] += state->lines[cur.line];
        state->lines.erase(state->lines.begin() + static_cast<long>(cur.line));
        --cur.line;
        cur.col = prevLen;
        notifyChange();
      }
      break;
    case Key::Delete:
      if (state->hasSelection()) {
        state->beginEdit(false);
        state->deleteSelectionRaw();
        notifyChange();
      } else if (cur.col < state->lines[cur.line].size()) {
        state->beginEdit(true);
        size_t end = liteui_utf8::nextBoundary(state->lines[cur.line], cur.col);
        state->lines[cur.line].erase(cur.col, end - cur.col);
        notifyChange();
      } else if (cur.line + 1 < state->lines.size()) {
        state->beginEdit(false);
        state->lines[cur.line] += state->lines[cur.line + 1];
        state->lines.erase(state->lines.begin() + static_cast<long>(cur.line) +
                           1);
        notifyChange();
      }
      break;
    case Key::Enter: {
      state->beginEdit(false);
      // Auto-indent: carry the current line's leading whitespace onto the
      // new line, plus one extra indent level if the line ends with an
      // open bracket. If the cursor sits exactly between a bracket pair
      // ("{|}"), split it onto three lines with the cursor indented one
      // level further than the braces, matching the common "press Enter
      // inside empty braces" convention.
      const std::string &line = state->lines[cur.line];
      size_t indentLen = 0;
      while (indentLen < line.size() &&
             (line[indentLen] == ' ' || line[indentLen] == '\t'))
        ++indentLen;
      std::string indent = line.substr(0, indentLen);
      std::string tabStr(static_cast<size_t>(tabSpaces), ' ');

      bool betweenPair = cur.col > 0 && cur.col < line.size() &&
                         isOpenBracket(line[cur.col - 1]) &&
                         line[cur.col] == matchingCloseFor(line[cur.col - 1]);
      if (betweenPair) {
        insertTextRaw("\n" + indent + tabStr + "\n" + indent);
        cur.line -= 1;
        cur.col = indent.size() + tabStr.size();
      } else {
        bool afterOpenBracket = cur.col > 0 && isOpenBracket(line[cur.col - 1]);
        insertTextRaw("\n" + indent +
                      (afterOpenBracket ? tabStr : std::string()));
      }
      notifyChange();
      break;
    }
    case Key::Tab:
      state->beginEdit(false);
      insertTextRaw(std::string(static_cast<size_t>(tabSpaces), ' '));
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
    if (preTextInput && preTextInput(cp)) {
      state->blinkOn = true;
      state->dirty = true;
      return;
    }

    if (cp < 0x20)
      return; // control characters (Enter/Tab handled separately above)

    // ---- find/replace mode absorbs typed characters into the query or
    // replacement field instead of the document ----
    if (state->search.active) {
      if (cp < 0x80) { // fields are plain ASCII text; skip anything else
        std::string &field = state->search.fieldFocus == 0
                                 ? state->search.query
                                 : state->search.replacement;
        field += static_cast<char>(cp);
        if (state->search.fieldFocus == 0)
          state->recomputeMatches();
        state->blinkOn = true;
        state->dirty = true;
      }
      return;
    }

    std::string encoded;
    liteui_utf8::appendCodepoint(encoded, cp);

    // ---- bracket/quote auto-close ----

    if (encoded.size() == 1 && !state->hasSelection()) {
      char c = encoded[0];
      const std::string &line = state->lines[state->cursor.line];
      size_t col = state->cursor.col;
      if (isCloseBracket(c) && col < line.size() && line[col] == c) {
        state->noteCursorMoved();
        state->cursor.col = col + 1;
        state->blinkOn = true;
        state->dirty = true;
        return;
      }
      if ((c == '"' || c == '\'') && col < line.size() && line[col] == c) {
        state->noteCursorMoved();
        state->cursor.col = col + 1;
        state->blinkOn = true;
        state->dirty = true;
        return;
      }
      char autoClose = '\0';
      if (isOpenBracket(c)) {
        autoClose = matchingCloseFor(c);
      } else if (c == '"' || c == '\'') {
        bool afterWordChar =
            col > 0 &&
            (std::isalnum(static_cast<unsigned char>(line[col - 1])) ||
             line[col - 1] == '_');
        if (!afterWordChar)
          autoClose = c;
      }
      if (autoClose != '\0') {
        state->beginEdit(false); // auto-close pair insertion is its own
                                 // undo step, not coalesced with plain typing
        state->insertTextRaw(std::string(1, c) + std::string(1, autoClose));
        state->cursor.col -= 1; // land between the two inserted characters
        state->blinkOn = true;
        state->dirty = true;
        notifyChange();
        return;
      }
    }

    state->beginEdit(!state->hasSelection()); // replacing a selection is
                                              // always its own undo step;
                                              // plain typing coalesces
    insertTextRaw(encoded);
    state->blinkOn = true;
    state->dirty = true;
    notifyChange();
  };

  return v;
}