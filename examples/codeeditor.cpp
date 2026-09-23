

#include "liteui.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

//=====================Theme =============================

namespace liteui_theme {

// surfaces
inline constexpr Color kEditorBg{30, 30, 30, 255};
inline constexpr Color kSideBarBg{37, 37, 38, 255};
inline constexpr Color kTabBarBg{37, 37, 38, 255};
inline constexpr Color kTabInactiveBg{45, 45, 45, 255};
inline constexpr Color kMenuBarBg{60, 60, 60, 255};
inline constexpr Color kMenuBarOpenBg{95, 95, 95, 255};
inline constexpr Color kStatusBarBg{0, 122, 204, 255};
inline constexpr Color kMenuBg{37, 37, 38, 255};
inline constexpr Color kMenuBorder{69, 69, 69, 255};
inline constexpr Color kInputBg{60, 60, 60, 255};
inline constexpr Color kButtonBg{60, 60, 60, 255};
inline constexpr Color kButtonHoverBg{78, 78, 78, 255};
inline constexpr Color kDivider{60, 60, 60, 255};

// interaction states
inline constexpr Color kHoverBg{42, 45, 46, 255};
inline constexpr Color kMenuHoverBg{9, 71, 113, 255};
inline constexpr Color kSelectedBg{4, 57, 94, 255};
inline constexpr Color kActiveItemBg{55, 55, 61, 255};
inline constexpr Color kTabHoverBg{55, 55, 55, 255};
inline constexpr Color kAccent{0, 122, 204, 255};
inline constexpr Color kDanger{0xC0, 0x39, 0x2B, 255};
inline constexpr Color kDangerHover{0xA8, 0x2F, 0x23, 255};

// text
inline constexpr Color kText{204, 204, 204, 255};
inline constexpr Color kTextMuted{157, 157, 157, 255};
inline constexpr Color kTextDim{128, 128, 128, 255};
inline constexpr Color kTextBright{255, 255, 255, 255};
inline constexpr Color kTextInactive{150, 150, 150, 255};
inline constexpr Color kLink{55, 148, 255, 255};
inline constexpr Color kOnAccent{255, 255, 255, 255};
inline constexpr Color kOnAccentDim{190, 220, 245, 255};

// editor text
inline constexpr Color kEditorText{212, 212, 212, 255};
inline constexpr Color kPlaceholder{130, 130, 130, 255};
inline constexpr Color kCaret{174, 175, 173, 255};

// misc
inline constexpr Color kTransparent{0, 0, 0, 0};
inline constexpr Color kOverlay{0, 0, 0, 120};    // modal dimmer
inline constexpr Color kOverlayClear{0, 0, 0, 2}; // click-away catcher
inline constexpr Color kScrollTrack{37, 37, 38, 255};
inline constexpr Color kScrollThumb{90, 90, 90, 255};

} // namespace liteui_theme

namespace th = liteui_theme;

//======================Terminal=========================

//
// A VS-Code-style integrated terminal: spawns a real shell behind a
// pseudo-terminal (forkpty on Linux, ConPTY on Windows), parses the byte
// stream it produces as a (deliberately partial) xterm/VT100, and renders
// the resulting character grid through a Canvas-backed View — the same
// monospace-cell approach liteui_editor_ext.hpp's CodeEditor uses, and the
// same shared_ptr<State> pattern (CodeEditorState, TextInputState, ...) so
// the terminal's grid, scrollback and live shell process all survive tab
// rebuilds untouched.
//
// What's implemented: printable UTF-8 text, cursor motion, erase in
// line/display, insert/delete char/line, the common SGR attributes
// (16/256/truecolor fg+bg, bold, faint, underline, reverse), scrolling
// with a capped scrollback buffer, the alternate screen buffer (so
// full-screen programs like vim/less/htop behave), cursor visibility,
// device-status/attribute queries (enough for programs that probe before
// drawing), and mouse-wheel scrollback navigation.
//
// Known v1 limitations, deliberately not implemented: double-width CJK
// glyphs (every cell is width 1), mouse reporting, real text
// selection/copy, tab stops other than every 8 columns, non-default
// scroll regions beyond simple top/bottom tracking, and a blinking
// cursor (drawn as a steady translucent block instead).
//
// Threading: the PTY is read on a dedicated background thread (since the
// read is blocking) into a mutex-guarded byte queue; the UI thread drains
// that queue on a timer (see toTerminalView's use of addInterval) and is
// the ONLY thread that ever touches the parsed grid — so the grid itself
// needs no locking of its own.
//

namespace liteui_terminal {

// ==================== color palette ====================
//
// The 16-color ANSI palette and the 256-color cube/ramp built on top of
// it, chosen to match VS Code's own default dark-theme terminal palette
// so the widget looks at home next to the editor.
inline Color ansi16(int idx, bool bright) {
  static const Color kNormal[8] = {{0, 0, 0, 255},      {205, 49, 49, 255},
                                   {13, 188, 121, 255}, {229, 229, 16, 255},
                                   {36, 114, 200, 255}, {188, 63, 188, 255},
                                   {17, 168, 205, 255}, {229, 229, 229, 255}};
  static const Color kBright[8] = {{102, 102, 102, 255}, {241, 76, 76, 255},
                                   {35, 209, 139, 255},  {245, 245, 67, 255},
                                   {59, 142, 234, 255},  {214, 112, 214, 255},
                                   {41, 184, 219, 255},  {255, 255, 255, 255}};
  idx = std::clamp(idx, 0, 7);
  return bright ? kBright[idx] : kNormal[idx];
}

inline Color ansi256(int n) {
  if (n < 16)
    return ansi16(n % 8, n >= 8);
  if (n < 232) {
    n -= 16;
    int r = n / 36, g = (n / 6) % 6, b = n % 6;
    auto lvl = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
    return Color{static_cast<uint8_t>(lvl(r)), static_cast<uint8_t>(lvl(g)),
                 static_cast<uint8_t>(lvl(b)), 255};
  }
  int gray = 8 + (n - 232) * 10;
  return Color{static_cast<uint8_t>(gray), static_cast<uint8_t>(gray),
               static_cast<uint8_t>(gray), 255};
}

inline constexpr Color kDefaultBg{30, 30, 30, 255};    // VS Code's #1e1e1e
inline constexpr Color kDefaultFg{204, 204, 204, 255}; // VS Code's #cccccc

// "Monospace" is Pango's generic family name on Linux; DirectWrite on
// Windows doesn't recognize it and silently substitutes a non-monospace
// font, which breaks the terminal's fixed cellW grid math (every column
// is placed by column*cellW, not remeasured per glyph — see onPaint's
// own per-glyph drawing note for the other half of that fix). Consolas
// has shipped with Windows since Vista, so it's a safe concrete choice.
#if defined(_WIN32)
inline std::string defaultMonospaceFont() { return "Consolas"; }
#else
inline std::string defaultMonospaceFont() { return "Monospace"; }
#endif

// ==================== grid model ====================

// One color slot: either "the theme default" (so it tracks kDefaultFg/Bg
// even if the theme colors ever change) or a resolved RGB value.
struct TermColor {
  bool isDefault = true;
  Color rgb{0, 0, 0, 255};
  static TermColor makeDefault() { return TermColor{}; }
  static TermColor rgbColor(Color c) { return TermColor{false, c}; }
};

struct TermCell {
  std::string ch = " "; // raw UTF-8 bytes for this cell's glyph — usually
                        // one codepoint; see the file header's note on
                        // why double-width glyphs aren't modeled
  TermColor fg = TermColor::makeDefault();
  TermColor bg = TermColor::makeDefault();
  bool bold = false, faint = false, italic = false, underline = false,
       reverse = false;

  // Whether two cells can share one drawn run — deliberately ignores
  // `italic` since nothing currently renders it (see the file header),
  // so including it here would only fragment runs for no visual benefit.
  bool sameAttrs(const TermCell &o) const {
    return fg.isDefault == o.fg.isDefault &&
           (fg.isDefault || fg.rgb == o.fg.rgb) &&
           bg.isDefault == o.bg.isDefault &&
           (bg.isDefault || bg.rgb == o.bg.rgb) && bold == o.bold &&
           faint == o.faint && underline == o.underline && reverse == o.reverse;
  }
};

struct TermRow {
  std::vector<TermCell> cells;
  explicit TermRow(int cols = 0)
      : cells(static_cast<size_t>(std::max(0, cols))) {}
};

struct TermPos {
  int row = 0, col = 0;
};

// ==================== home directory ====================
//
// Where the shell starts when no workspace folder is open yet. Reuses
// liteui.hpp's own toWide() on Windows (already included transitively).
#if defined(_WIN32)
inline std::string userHomeDirectory() {
  wchar_t buf[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return std::string();
  int len =
      WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
  std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
  if (len > 0)
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), len, nullptr, nullptr);
  return s;
}
#else
inline std::string userHomeDirectory() {
  const char *home = getenv("HOME");
  return home && *home ? std::string(home) : std::string("/");
}
#endif

// ==================== VT/xterm-subset parser + screen state
// ====================
//
// One instance owns: the PTY (spawn/read-thread/write/resize/shutdown),
// the parser state machine, and the resulting character grid — plus a
// capped scrollback and an alternate-screen buffer for full-screen
// programs. Held by shared_ptr, same pattern as CodeEditorState: every
// closure in the View this produces needs to keep working across tab
// rebuilds, and the owner (e.g. TabbedEditor) keeps its own copy of the
// shared_ptr alongside the docs_ it already tracks.
class TerminalState {
public:
  TerminalState() { resizeGrid(80, 24); }
  ~TerminalState() { shutdown(); }
  TerminalState(const TerminalState &) = delete;
  TerminalState &operator=(const TerminalState &) = delete;

  // ---- lifecycle ----

  // Spawns the user's shell behind a PTY of the given initial size,
  // starting in `startDir` — or the user's home directory if `startDir`
  // is empty, which is what "no workspace folder open yet" should look
  // like. Safe to call once; a second call is a no-op (the real size
  // arrives soon after via resize(), once the widget's first layout pass
  // runs).
  void spawn(int cols, int rows, const std::string &startDir = std::string()) {
    if (spawned_)
      return;
    spawned_ = true;
    resizeGrid(cols, rows);
    std::string dir = startDir.empty() ? userHomeDirectory() : startDir;
#if defined(_WIN32)
    spawnWindows(cols, rows, dir);
#else
    spawnPosix(cols, rows, dir);
#endif
    if (ptyLive())
      startReaderThread();
  }

  // Sends a `cd` to the already-running shell — used when the app's
  // workspace folder changes after the terminal has already been
  // spawned. A live process's CWD can't be changed from outside, but a
  // real terminal user wouldn't expect that either; this sends exactly
  // what a person would type themselves, after clearing any partial
  // input on the line so it can't get appended to garbage.
  void changeDirectory(const std::string &path) {
    if (path.empty() || !ptyLive())
      return;
    std::string cmd = "\x15"; // Ctrl+U: kill the current input line first
#if defined(_WIN32)
    cmd += "cd /d \"" + path + "\"\r";
#else
    cmd += "cd \"" + path + "\"\r";
#endif
    writeInput(cmd);
  }

  // Terminates the child and reader thread. Safe to call more than once
  // (e.g. once explicitly from the owner's destructor, once implicitly
  // from ~TerminalState) — the second call is a no-op.
  void shutdown() {
    if (!spawned_)
      return;
    readerRunning_ = false;
#if defined(_WIN32)
    if (process_.hProcess) {
      TerminateProcess(process_.hProcess, 0);
      CloseHandle(process_.hProcess);
      process_.hProcess = nullptr;
    }
    if (process_.hThread) {
      CloseHandle(process_.hThread);
      process_.hThread = nullptr;
    }
    if (hpc_) {
      ClosePseudoConsole(hpc_);
      hpc_ = nullptr;
    }
    if (hOutRead_) {
      CloseHandle(hOutRead_); // unblocks the reader thread's ReadFile
      hOutRead_ = nullptr;
    }
    if (hInWrite_) {
      CloseHandle(hInWrite_);
      hInWrite_ = nullptr;
    }
#else
    if (childPid_ > 0) {
      kill(childPid_, SIGHUP);
      childPid_ = -1;
    }
    if (masterFd_ >= 0) {
      close(masterFd_); // unblocks the reader thread's read()
      masterFd_ = -1;
    }
#endif
    if (readerThread_.joinable())
      readerThread_.join();
    spawned_ = false;
  }

  // ---- input (called from the UI thread) ----

  void writeInput(const std::string &bytes) {
    if (bytes.empty())
      return;
    // Any keystroke jumps back to the live tail, matching every
    // terminal's convention that typing cancels a scrollback view.
    scrollOffset_ = 0;
#if defined(_WIN32)
    if (!hInWrite_)
      return;
    DWORD written = 0;
    WriteFile(hInWrite_, bytes.data(), static_cast<DWORD>(bytes.size()),
              &written, nullptr);
#else
    if (masterFd_ < 0)
      return;
    size_t off = 0;
    while (off < bytes.size()) {
      ssize_t n = write(masterFd_, bytes.data() + off, bytes.size() - off);
      if (n <= 0)
        break;
      off += static_cast<size_t>(n);
    }
#endif
  }

  // Resizes both the PTY and the visible grid. Cheap to call on every
  // layout pass — it no-ops when the size hasn't actually changed.
  void resize(int cols, int rows) {
    cols = std::max(2, cols);
    rows = std::max(2, rows);
    if (cols == cols_ && rows == rows_)
      return;
    resizeGrid(cols, rows);
#if defined(_WIN32)
    if (hpc_) {
      COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
      ResizePseudoConsole(hpc_, size);
    }
#else
    if (masterFd_ >= 0) {
      winsize ws{};
      ws.ws_col = static_cast<unsigned short>(cols);
      ws.ws_row = static_cast<unsigned short>(rows);
      ioctl(masterFd_, TIOCSWINSZ, &ws); // also raises SIGWINCH for us
    }
#endif
  }

  // Called on a UI-thread timer (see toTerminalView's addInterval).
  // Drains whatever the reader thread has queued since the last poll,
  // feeds it through the parser, and reports whether anything actually
  // changed so the caller can skip a redraw when nothing did.
  bool pollOutput() {
    std::vector<uint8_t> chunk;
    {
      std::lock_guard<std::mutex> lock(outputMutex_);
      if (pendingOutput_.empty() && !exitedByReader_)
        return false;
      chunk.swap(pendingOutput_);
    }
    bool changed = false;
    if (!chunk.empty()) {
      feed(chunk.data(), chunk.size());
      changed = true;
    }
    if (exitedByReader_ && !exitedReported_) {
      exitedReported_ = true;
      static const char kMsg[] = "\r\n[process exited]\r\n";
      feed(reinterpret_cast<const uint8_t *>(kMsg), sizeof(kMsg) - 1);
      changed = true;
    }
    if (changed)
      dirty_ = true;
    return changed;
  }

  // ---- scrollback navigation ----
  void scrollBy(int lines) {
    int maxOff = static_cast<int>(scrollback_.size());
    scrollOffset_ = std::clamp(scrollOffset_ + lines, 0, maxOff);
    dirty_ = true;
  }

  // ---- accessors used by the renderer ----
  int cols() const { return cols_; }
  int rows() const { return rows_; }
  int scrollOffset() const { return scrollOffset_; }
  TermPos cursor() const { return {cursorRow_, cursorCol_}; }
  bool cursorVisible() const { return cursorVisible_; }
  bool appCursorKeys() const { return appCursorKeys_; }

  // Row `i` of the currently visible viewport (0 = top), accounting for
  // scrollOffset_ into the scrollback.
  const TermRow &visibleRow(int i) const {
    int total = static_cast<int>(scrollback_.size());
    int start = total - scrollOffset_;
    int idx = start + i;
    if (idx < 0)
      return blankRow_;
    if (idx < total)
      return scrollback_[static_cast<size_t>(idx)];
    int gridIdx = idx - total;
    if (gridIdx >= 0 && gridIdx < static_cast<int>(grid_.size()))
      return grid_[static_cast<size_t>(gridIdx)];
    return blankRow_;
  }

  bool consumeDirty() {
    bool d = dirty_;
    dirty_ = false;
    return d;
  }

private:
  // ---- grid/scrollback management ----

  void resizeGrid(int cols, int rows) {
    std::vector<TermRow> newGrid(static_cast<size_t>(rows), TermRow(cols));
    for (int r = 0; r < std::min(rows, static_cast<int>(grid_.size())); ++r) {
      int copyCols = std::min(cols, cols_);
      for (int c = 0; c < copyCols; ++c)
        newGrid[static_cast<size_t>(r)].cells[static_cast<size_t>(c)] =
            grid_[static_cast<size_t>(r)].cells[static_cast<size_t>(c)];
    }
    grid_ = std::move(newGrid);
    altGrid_.assign(static_cast<size_t>(rows), TermRow(cols));
    cols_ = cols;
    rows_ = rows;
    scrollTop_ = 0;
    scrollBottom_ = rows_ - 1;
    cursorRow_ = std::min(cursorRow_, rows_ - 1);
    cursorCol_ = std::min(cursorCol_, cols_ - 1);
    blankRow_ = TermRow(cols);
    dirty_ = true;
  }

  TermRow &row(int r) {
    return grid_[static_cast<size_t>(std::clamp(r, 0, rows_ - 1))];
  }

  void pushScrollback(TermRow r) {
    scrollback_.push_back(std::move(r));
    if (scrollback_.size() > kMaxScrollback)
      scrollback_.pop_front();
  }

  // Scrolls the region [scrollTop_, scrollBottom_] up by n rows. Rows
  // scrolled off the top only join the real scrollback when the region
  // is the actual top of the screen and we're on the primary buffer —
  // exactly matching how xterm treats scroll-region output.
  void scrollUpRegion(int n) {
    for (int i = 0; i < n; ++i) {
      if (scrollTop_ == 0 && !usingAltScreen_)
        pushScrollback(row(scrollTop_));
      for (int r = scrollTop_; r < scrollBottom_; ++r)
        row(r) = row(r + 1);
      row(scrollBottom_) = TermRow(cols_);
    }
  }
  void scrollDownRegion(int n) {
    for (int i = 0; i < n; ++i) {
      for (int r = scrollBottom_; r > scrollTop_; --r)
        row(r) = row(r - 1);
      row(scrollTop_) = TermRow(cols_);
    }
  }

  void lineFeed() {
    if (cursorRow_ == scrollBottom_)
      scrollUpRegion(1);
    else if (cursorRow_ < rows_ - 1)
      ++cursorRow_;
  }

  void putGlyph(const std::string &utf8Glyph) {
    if (cursorCol_ >= cols_) {
      cursorCol_ = 0;
      lineFeed();
    }
    TermCell &cell = row(cursorRow_).cells[static_cast<size_t>(cursorCol_)];
    cell.ch = utf8Glyph;
    cell.fg = curFg_;
    cell.bg = curBg_;
    cell.bold = curBold_;
    cell.faint = curFaint_;
    cell.italic = curItalic_;
    cell.underline = curUnderline_;
    cell.reverse = curReverse_;
    ++cursorCol_;
  }

  // ---- erase helpers ----
  void eraseInDisplay(int mode) {
    if (mode == 0) {
      eraseInLine(0);
      for (int r = cursorRow_ + 1; r < rows_; ++r)
        row(r) = TermRow(cols_);
    } else if (mode == 1) {
      eraseInLine(1);
      for (int r = 0; r < cursorRow_; ++r)
        row(r) = TermRow(cols_);
    } else {
      for (int r = 0; r < rows_; ++r)
        row(r) = TermRow(cols_);
      if (mode == 3 && !usingAltScreen_)
        scrollback_.clear();
    }
    dirty_ = true;
  }
  void eraseInLine(int mode) {
    TermRow &r = row(cursorRow_);
    int from = 0, to = cols_;
    if (mode == 0)
      from = cursorCol_;
    else if (mode == 1)
      to = cursorCol_ + 1;
    for (int c = std::max(0, from); c < std::min(cols_, to); ++c)
      r.cells[static_cast<size_t>(c)] = TermCell();
  }
  void deleteChars(int n) {
    TermRow &r = row(cursorRow_);
    for (int i = cursorCol_; i < cols_; ++i) {
      int src = i + n;
      r.cells[static_cast<size_t>(i)] =
          src < cols_ ? r.cells[static_cast<size_t>(src)] : TermCell();
    }
  }
  void insertChars(int n) {
    TermRow &r = row(cursorRow_);
    for (int i = cols_ - 1; i >= cursorCol_; --i) {
      int src = i - n;
      r.cells[static_cast<size_t>(i)] =
          src >= cursorCol_ ? r.cells[static_cast<size_t>(src)] : TermCell();
    }
  }
  void eraseChars(int n) {
    TermRow &r = row(cursorRow_);
    for (int c = cursorCol_; c < std::min(cols_, cursorCol_ + n); ++c)
      r.cells[static_cast<size_t>(c)] = TermCell();
  }
  // Approximated as a full-region shift rather than "shift only rows
  // below the cursor" — close enough for the common case (full-screen
  // apps issue IL/DL with the cursor already sitting at scrollTop_).
  void insertLines(int n) {
    for (int i = 0; i < n; ++i)
      scrollDownRegion(1);
  }
  void deleteLines(int n) {
    for (int i = 0; i < n; ++i)
      scrollUpRegion(1);
  }

  // ---- SGR ----
  void resetAttrs() {
    curFg_ = TermColor::makeDefault();
    curBg_ = TermColor::makeDefault();
    curBold_ = curFaint_ = curItalic_ = curUnderline_ = curReverse_ = false;
  }
  void handleSgr(const std::vector<int> &params) {
    if (params.empty()) {
      resetAttrs();
      return;
    }
    for (size_t i = 0; i < params.size(); ++i) {
      int p = params[i];
      if (p == 0)
        resetAttrs();
      else if (p == 1)
        curBold_ = true;
      else if (p == 2)
        curFaint_ = true;
      else if (p == 3)
        curItalic_ = true;
      else if (p == 4)
        curUnderline_ = true;
      else if (p == 7)
        curReverse_ = true;
      else if (p == 22)
        curBold_ = curFaint_ = false;
      else if (p == 23)
        curItalic_ = false;
      else if (p == 24)
        curUnderline_ = false;
      else if (p == 27)
        curReverse_ = false;
      else if (p >= 30 && p <= 37)
        curFg_ = TermColor::rgbColor(ansi16(p - 30, false));
      else if (p == 38 || p == 48) {
        bool isFg = (p == 38);
        if (i + 1 < params.size() && params[i + 1] == 5 &&
            i + 2 < params.size()) {
          Color c = ansi256(params[i + 2]);
          (isFg ? curFg_ : curBg_) = TermColor::rgbColor(c);
          i += 2;
        } else if (i + 1 < params.size() && params[i + 1] == 2 &&
                   i + 4 < params.size()) {
          Color c{static_cast<uint8_t>(params[i + 2]),
                  static_cast<uint8_t>(params[i + 3]),
                  static_cast<uint8_t>(params[i + 4]), 255};
          (isFg ? curFg_ : curBg_) = TermColor::rgbColor(c);
          i += 4;
        }
      } else if (p == 39)
        curFg_ = TermColor::makeDefault();
      else if (p >= 40 && p <= 47)
        curBg_ = TermColor::rgbColor(ansi16(p - 40, false));
      else if (p == 49)
        curBg_ = TermColor::makeDefault();
      else if (p >= 90 && p <= 97)
        curFg_ = TermColor::rgbColor(ansi16(p - 90, true));
      else if (p >= 100 && p <= 107)
        curBg_ = TermColor::rgbColor(ansi16(p - 100, true));
    }
  }

  // ---- CSI dispatch ----
  void dispatchCsi(char finalByte, bool priv, const std::vector<int> &params) {
    auto n = [&](size_t idx, int def) {
      return (idx < params.size() && params[idx] != 0) ? params[idx] : def;
    };
    switch (finalByte) {
    case 'A':
      cursorRow_ = std::max(0, cursorRow_ - n(0, 1));
      break;
    case 'B':
      cursorRow_ = std::min(rows_ - 1, cursorRow_ + n(0, 1));
      break;
    case 'C':
      cursorCol_ = std::min(cols_ - 1, cursorCol_ + n(0, 1));
      break;
    case 'D':
      cursorCol_ = std::max(0, cursorCol_ - n(0, 1));
      break;
    case 'E':
      cursorRow_ = std::min(rows_ - 1, cursorRow_ + n(0, 1));
      cursorCol_ = 0;
      break;
    case 'F':
      cursorRow_ = std::max(0, cursorRow_ - n(0, 1));
      cursorCol_ = 0;
      break;
    case 'G':
      cursorCol_ = std::clamp(n(0, 1) - 1, 0, cols_ - 1);
      break;
    case 'd':
      cursorRow_ = std::clamp(n(0, 1) - 1, 0, rows_ - 1);
      break;
    case 'H':
    case 'f':
      cursorRow_ = std::clamp(n(0, 1) - 1, 0, rows_ - 1);
      cursorCol_ = std::clamp(n(1, 1) - 1, 0, cols_ - 1);
      break;
    case 'J':
      eraseInDisplay(params.empty() ? 0 : params[0]);
      break;
    case 'K':
      eraseInLine(params.empty() ? 0 : params[0]);
      break;
    case 'S':
      scrollUpRegion(n(0, 1));
      break;
    case 'T':
      scrollDownRegion(n(0, 1));
      break;
    case 'P':
      deleteChars(n(0, 1));
      break;
    case '@':
      insertChars(n(0, 1));
      break;
    case 'L':
      insertLines(n(0, 1));
      break;
    case 'M':
      deleteLines(n(0, 1));
      break;
    case 'X':
      eraseChars(n(0, 1));
      break;
    case 'm':
      handleSgr(params);
      break;
    case 'r':
      scrollTop_ = std::clamp(n(0, 1) - 1, 0, rows_ - 1);
      scrollBottom_ =
          std::clamp(params.size() > 1 ? n(1, rows_) - 1 : rows_ - 1,
                     scrollTop_, rows_ - 1);
      cursorRow_ = 0;
      cursorCol_ = 0;
      break;
    case 's':
      savedCursor_ = {cursorRow_, cursorCol_};
      break;
    case 'u':
      cursorRow_ = savedCursor_.row;
      cursorCol_ = savedCursor_.col;
      break;
    case 'h':
    case 'l':
      handleModeSet(priv, params, finalByte == 'h');
      break;
    case 'n':
      if (!params.empty() && params[0] == 6) {
        std::string resp = "\x1b[" + std::to_string(cursorRow_ + 1) + ";" +
                           std::to_string(cursorCol_ + 1) + "R";
        writeInput(resp);
      }
      break;
    case 'c':
      if (!priv)
        writeInput("\x1b[?1;2c");
      break;
    default:
      break; // unhandled final byte — safely ignored
    }
  }

  void handleModeSet(bool priv, const std::vector<int> &params, bool set) {
    if (!priv)
      return;
    for (int p : params) {
      if (p == 25)
        cursorVisible_ = set;
      else if (p == 1)
        appCursorKeys_ = set;
      else if (p == 1049 || p == 47 || p == 1047)
        setAltScreen(set);
      // 2004 (bracketed paste), 12 (cursor blink rate), etc. are parsed
      // and silently ignored — see the file header's known limitations.
    }
  }

  void setAltScreen(bool on) {
    if (on == usingAltScreen_)
      return;
    usingAltScreen_ = on;
    std::swap(grid_, altGrid_);
    if (on)
      for (auto &r : grid_)
        r = TermRow(cols_);
    dirty_ = true;
  }

  // ---- byte-stream feed / parse ----
  void feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
      uint8_t b = data[i];
      switch (parseState_) {
      case ParseState::Normal:
        feedNormal(b);
        break;
      case ParseState::Esc:
        feedEsc(b);
        break;
      case ParseState::EscCharset:
        parseState_ = ParseState::Normal; // consume+ignore the charset id
        break;
      case ParseState::Csi:
        feedCsi(b);
        break;
      case ParseState::Osc:
        feedOsc(b);
        break;
      case ParseState::OscEsc:
        parseState_ = ParseState::Normal; // consumes the '\' of ST, if any
        if (b != '\\')
          feedNormal(b);
        break;
      }
    }
  }

  void feedNormal(uint8_t b) {
    if (b == 0x1B) {
      parseState_ = ParseState::Esc;
      csiParams_.clear();
      csiCur_.clear();
      csiPrivate_ = false;
      return;
    }
    if (b == '\r') {
      cursorCol_ = 0;
      return;
    }
    if (b == '\n') {
      lineFeed();
      return;
    }
    if (b == '\b') {
      if (cursorCol_ > 0)
        --cursorCol_;
      return;
    }
    if (b == '\t') {
      cursorCol_ = std::min(cols_ - 1, (cursorCol_ / 8 + 1) * 8);
      return;
    }
    if (b == '\a' || b == 0x00 || b == 0x0E || b == 0x0F)
      return; // bell / NUL / shift-out / shift-in — no-ops here

    // Printable byte: assemble a UTF-8 codepoint, which may span
    // multiple feed() calls if a read chunk split it mid-sequence.
    if (utf8Pending_.empty()) {
      int glyphLen = utf8Len(b);
      utf8Pending_.push_back(static_cast<char>(b));
      utf8Need_ = glyphLen - 1;
      if (utf8Need_ <= 0) {
        putGlyph(utf8Pending_);
        utf8Pending_.clear();
      }
    } else {
      utf8Pending_.push_back(static_cast<char>(b));
      if (--utf8Need_ <= 0) {
        putGlyph(utf8Pending_);
        utf8Pending_.clear();
      }
    }
  }

  static int utf8Len(uint8_t lead) {
    if ((lead & 0x80) == 0x00)
      return 1;
    if ((lead & 0xE0) == 0xC0)
      return 2;
    if ((lead & 0xF0) == 0xE0)
      return 3;
    if ((lead & 0xF8) == 0xF0)
      return 4;
    return 1; // stray continuation byte — treat as its own (invalid but
              // harmless) single-byte glyph rather than desyncing forever
  }

  void feedEsc(uint8_t b) {
    if (b == '[') {
      parseState_ = ParseState::Csi;
    } else if (b == ']') {
      parseState_ = ParseState::Osc;
      oscBuf_.clear();
    } else if (b == '(' || b == ')') {
      parseState_ = ParseState::EscCharset;
    } else if (b == '=' || b == '>') {
      parseState_ = ParseState::Normal; // keypad mode — ignored
    } else if (b == 'M') {
      if (cursorRow_ == scrollTop_)
        scrollDownRegion(1);
      else if (cursorRow_ > 0)
        --cursorRow_;
      parseState_ = ParseState::Normal;
    } else if (b == 'c') {
      eraseInDisplay(2);
      cursorRow_ = cursorCol_ = 0;
      resetAttrs();
      parseState_ = ParseState::Normal;
    } else if (b == '7') {
      savedCursor_ = {cursorRow_, cursorCol_};
      parseState_ = ParseState::Normal;
    } else if (b == '8') {
      cursorRow_ = savedCursor_.row;
      cursorCol_ = savedCursor_.col;
      parseState_ = ParseState::Normal;
    } else {
      parseState_ = ParseState::Normal;
    }
  }

  void feedCsi(uint8_t b) {
    if (b == '?' && csiCur_.empty() && csiParams_.empty()) {
      csiPrivate_ = true;
      return;
    }
    if (b >= '0' && b <= '9') {
      csiCur_.push_back(static_cast<char>(b));
      return;
    }
    if (b == ';') {
      csiParams_.push_back(csiCur_.empty() ? 0 : std::atoi(csiCur_.c_str()));
      csiCur_.clear();
      return;
    }
    if (b >= 0x40 && b <= 0x7E) {
      csiParams_.push_back(csiCur_.empty() ? 0 : std::atoi(csiCur_.c_str()));
      dispatchCsi(static_cast<char>(b), csiPrivate_, csiParams_);
      parseState_ = ParseState::Normal;
      return;
    }
    // Intermediate bytes (0x20-0x2F) and any other prefix bytes: no CSI
    // sequence this terminal needs to handle uses them, so they're just
    // skipped while more of the sequence is collected.
  }

  void feedOsc(uint8_t b) {
    if (b == 0x07) { // BEL terminates OSC
      parseState_ = ParseState::Normal;
      return;
    }
    if (b == 0x1B) {
      parseState_ = ParseState::OscEsc;
      return;
    }
    oscBuf_.push_back(static_cast<char>(b));
    // oscBuf_ (window-title / hyperlink / shell-integration payloads) is
    // parsed just far enough to find its terminator and then discarded —
    // this terminal has no window-title UI to feed it into.
  }

  // ---- PTY spawn/read-thread, per platform ----
  bool ptyLive() const {
#if defined(_WIN32)
    return hpc_ != nullptr;
#else
    return masterFd_ >= 0;
#endif
  }

  void startReaderThread() {
    readerRunning_ = true;
    readerThread_ = std::thread([this] { readerLoop(); });
  }

  void readerLoop() {
    uint8_t buf[4096];
    while (readerRunning_) {
#if defined(_WIN32)
      DWORD n = 0;
      BOOL ok = hOutRead_ && ReadFile(hOutRead_, buf, sizeof(buf), &n, nullptr);
      if (!ok || n == 0)
        break;
#else
      ssize_t n = masterFd_ >= 0 ? read(masterFd_, buf, sizeof(buf)) : -1;
      if (n <= 0) {
        if (n < 0 && errno == EINTR)
          continue;
        break;
      }
#endif
      std::lock_guard<std::mutex> lock(outputMutex_);
      pendingOutput_.insert(pendingOutput_.end(), buf,
                            buf + static_cast<size_t>(n));
    }
    exitedByReader_ = true;
  }

#if defined(_WIN32)
  // Requires a Windows 10 1809+ SDK for the ConPTY APIs
  // (CreatePseudoConsole/ResizePseudoConsole/ClosePseudoConsole).
  void spawnWindows(int cols, int rows, const std::string &dir) {
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0))
      return;
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HRESULT hr = CreatePseudoConsole(size, inR, outW, 0, &hpc_);
    CloseHandle(inR);
    CloseHandle(outW);
    if (FAILED(hr)) {
      CloseHandle(inW);
      CloseHandle(outR);
      return;
    }
    hInWrite_ = inW;
    hOutRead_ = outR;

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    si.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0,
                              PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_,
                              sizeof(HPCON), nullptr, nullptr);

    wchar_t comspec[MAX_PATH];
    if (!GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH))
      wcscpy_s(comspec, L"cmd.exe");
    std::vector<wchar_t> cmdline(comspec, comspec + wcslen(comspec) + 1);

    // toWide() comes from liteui.hpp (included transitively); an empty
    // dir means "use CreateProcessW's own default", which is fine since
    // spawn() already resolves an empty startDir to the home directory
    // before we ever get here.
    std::wstring wdir = dir.empty() ? std::wstring() : toWide(dir);

    CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                   EXTENDED_STARTUPINFO_PRESENT, nullptr,
                   wdir.empty() ? nullptr : wdir.c_str(), &si.StartupInfo,
                   &process_);

    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
  }
#else
  void spawnPosix(int cols, int rows, const std::string &dir) {
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    pid_t pid = forkpty(&masterFd_, nullptr, nullptr, &ws);
    if (pid < 0) {
      masterFd_ = -1;
      return;
    }
    if (pid == 0) {
      // Child: exec the user's shell. setsid/controlling-tty wiring is
      // already done for us by forkpty(). chdir() failing (e.g. a
      // deleted folder) just leaves us in whatever directory we forked
      // from — no different from a real terminal in that situation.
      if (!dir.empty())
        chdir(dir.c_str());
      const char *shell = getenv("SHELL");
      if (!shell || !*shell)
        shell = "/bin/bash";
      setenv("TERM", "xterm-256color", 1);
      execl(shell, shell, "-i", static_cast<char *>(nullptr));
      _exit(127); // execl only returns on failure
    }
    childPid_ = pid;
    fcntl(masterFd_, F_SETFD, FD_CLOEXEC);
  }
#endif

  // ---- parser state ----
  enum class ParseState { Normal, Esc, EscCharset, Csi, Osc, OscEsc };
  ParseState parseState_ = ParseState::Normal;
  std::vector<int> csiParams_;
  std::string csiCur_;
  bool csiPrivate_ = false;
  std::string oscBuf_;
  std::string utf8Pending_;
  int utf8Need_ = 0;

  // ---- current SGR attribute state (applied to the next glyph) ----
  TermColor curFg_ = TermColor::makeDefault();
  TermColor curBg_ = TermColor::makeDefault();
  bool curBold_ = false, curFaint_ = false, curItalic_ = false,
       curUnderline_ = false, curReverse_ = false;

  // ---- screen state ----
  int cols_ = 80, rows_ = 24;
  int cursorRow_ = 0, cursorCol_ = 0;
  int scrollTop_ = 0, scrollBottom_ = 23;
  bool cursorVisible_ = true;
  bool appCursorKeys_ = false;
  bool usingAltScreen_ = false;
  TermPos savedCursor_;
  std::vector<TermRow> grid_, altGrid_;
  std::deque<TermRow> scrollback_;
  TermRow blankRow_{0};
  static constexpr size_t kMaxScrollback = 5000;
  int scrollOffset_ = 0; // 0 = viewing the live tail
  bool dirty_ = true;

  // ---- PTY / threading ----
  bool spawned_ = false;
  std::atomic<bool> readerRunning_{false};
  std::atomic<bool> exitedByReader_{false};
  bool exitedReported_ = false;
  std::thread readerThread_;
  std::mutex outputMutex_;
  std::vector<uint8_t> pendingOutput_;

#if defined(_WIN32)
  HPCON hpc_ = nullptr;
  HANDLE hInWrite_ = nullptr, hOutRead_ = nullptr;
  PROCESS_INFORMATION process_{};
#else
  int masterFd_ = -1;
  pid_t childPid_ = -1;
#endif
};

// ==================== author-facing widget ====================

// Encodes a key press into the byte sequence a real terminal would send
// for it. Returns empty for keys that don't map onto anything (plain
// modifier presses, function keys beyond what's wired up here, etc).
inline std::string encodeKey(const KeyEvent &e, bool appCursorKeys) {
  if (e.mods.ctrl && !e.mods.alt && !e.mods.super && e.key >= Key::A &&
      e.key <= Key::Z) {
    char c = static_cast<char>(static_cast<int>(e.key) -
                               static_cast<int>(Key::A) + 1);
    return std::string(1, c);
  }
  switch (e.key) {
  case Key::Enter:
    return "\r";
  case Key::Backspace:
    return "\x7f";
  case Key::Tab:
    return "\t";
  case Key::Escape:
    return "\x1b";
  case Key::Delete:
    return "\x1b[3~";
  case Key::Up:
    return appCursorKeys ? "\x1bOA" : "\x1b[A";
  case Key::Down:
    return appCursorKeys ? "\x1bOB" : "\x1b[B";
  case Key::Right:
    return appCursorKeys ? "\x1bOC" : "\x1b[C";
  case Key::Left:
    return appCursorKeys ? "\x1bOD" : "\x1b[D";
  case Key::Home:
    return "\x1b[H";
  case Key::End:
    return "\x1b[F";
  case Key::PageUp:
    return "\x1b[5~";
  case Key::PageDown:
    return "\x1b[6~";
  default:
    return {};
  }
}

struct Terminal {
  Style style;
  float fontSize = 13.0f;
  std::string fontFamily = defaultMonospaceFont();
  Color backgroundColor = kDefaultBg;
  std::shared_ptr<TerminalState> state = std::make_shared<TerminalState>();
};

inline View toTerminalView(Terminal t) {
  auto state = t.state;
  TextStyle ts;
  ts.fontSize = t.fontSize;
  ts.fontFamily = t.fontFamily;
  ts.wrap = TextWrap::NoWrap;

  // Monospace assumption: every glyph advances the same width as "M".
  liteui_text::Measurement cellM = liteui_text::measure("M", ts, -1);
  float cellW = std::max(1.0f, cellM.width);
  float lineH = t.fontSize * 1.4f;
  Color bg = t.backgroundColor;

  // Padding is read once here (rather than through resolveDynamic on
  // every layout) so onLayout's cols/rows math can subtract exactly what
  // the framework's own canvas-sizing code will subtract before calling
  // onPaint — this only stays correct for a fixed (non-callback) padding,
  // which is the only kind this widget is meant to be given.
  EdgeInsets pad = std::holds_alternative<EdgeInsets>(t.style.padding)
                       ? std::get<EdgeInsets>(t.style.padding)
                       : EdgeInsets{};

  View v;
  v.style = std::move(t.style);
  v.style.backgroundColor = bg;
  v.isCanvas = true;
  v.focusable = true;

  v.canvasDirtySource = [state] { return state->consumeDirty(); };

  v.onLayout = [state, cellW, lineH, pad](float, float, float w, float h) {
    float innerW = std::max(0.0f, w - pad.left - pad.right);
    float innerH = std::max(0.0f, h - pad.top - pad.bottom);
    int cols = std::max(2, static_cast<int>(innerW / cellW));
    int rows = std::max(2, static_cast<int>(innerH / lineH));
    state->resize(cols, rows);
  };

  v.onPaint = [state, ts, cellW, lineH, bg](CanvasContext &ctx) {
    ctx.setFillColor(bg);
    ctx.fillRect(0, 0, ctx.width(), ctx.height());
    ctx.setTextBaseline(TextBaseline::Middle);
    ctx.setTextAlign(TextAlign::Start);

    int rows = state->rows(), cols = state->cols();
    for (int r = 0; r < rows; ++r) {
      const TermRow &line = state->visibleRow(r);
      float y = r * lineH + lineH / 2.0f;
      int c = 0;
      while (c < cols && c < static_cast<int>(line.cells.size())) {
        const TermCell &first = line.cells[static_cast<size_t>(c)];
        int runEnd = c + 1;
        while (runEnd < cols && runEnd < static_cast<int>(line.cells.size()) &&
               line.cells[static_cast<size_t>(runEnd)].sameAttrs(first))
          ++runEnd;

        Color fg = first.fg.isDefault ? kDefaultFg : first.fg.rgb;
        Color cellBg = first.bg.isDefault ? bg : first.bg.rgb;
        if (first.reverse)
          std::swap(fg, cellBg);
        if (first.faint) {
          fg.r = static_cast<uint8_t>(fg.r * 0.6f);
          fg.g = static_cast<uint8_t>(fg.g * 0.6f);
          fg.b = static_cast<uint8_t>(fg.b * 0.6f);
        }

        float runX = c * cellW;
        float runW = (runEnd - c) * cellW;
        if (cellBg.r != bg.r || cellBg.g != bg.g || cellBg.b != bg.b) {
          ctx.setFillColor(cellBg);
          ctx.fillRect(runX, r * lineH, runW, lineH);
        }
        ctx.setFont(ts.fontFamily, ts.fontSize,
                    first.bold ? FontWeight::Bold : FontWeight::Regular);
        ctx.setFillColor(fg);
        // One fillText per glyph, each pinned to i*cellW, rather than
        // the whole run as one string — keeps every column exactly on
        // its grid slot (and therefore in sync with the cursor's own
        // column*cellW math below) even if the chosen font turns out
        // not to be perfectly fixed-pitch.
        for (int i = c; i < runEnd; ++i)
          ctx.fillText(line.cells[static_cast<size_t>(i)].ch, i * cellW, y);
        if (first.underline)
          ctx.fillRect(runX, r * lineH + lineH - 2.0f, runW, 1.0f);
        c = runEnd;
      }
    }

    // Cursor: only meaningful while looking at the live tail (see the
    // file header for why this is a steady block rather than blinking).
    if (state->cursorVisible() && state->scrollOffset() == 0) {
      TermPos cur = state->cursor();
      ctx.setFillColor(Color{200, 200, 200, 120});
      ctx.fillRect(cur.col * cellW, cur.row * lineH, cellW, lineH);
    }
  };

  // Clicking to type jumps back to the live tail, same as any keystroke.
  v.onPressAt = [state](float, float) { state->scrollBy(-1000000); };
  v.onScrollUp = [state] { state->scrollBy(3); };
  v.onScrollDown = [state] { state->scrollBy(-3); };

  v.onKeyDown = [state](KeyEvent e) {
    std::string bytes = encodeKey(e, state->appCursorKeys());
    if (!bytes.empty())
      state->writeInput(bytes);
  };
  v.onTextInput = [state](uint32_t cp) {
    if (cp < 0x20) // Tab/Enter/etc. leak through as text input on some
      return;      // platforms — onKeyDown above already sent them
    std::string encoded;
    liteui_xml::appendUtf8(encoded, cp);
    state->writeInput(encoded);
  };

  return v;
}

} // namespace liteui_terminal

//=====================Editor Ext ========================

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
  Color textColor = liteui_theme::kEditorText;
  Color placeholderColor = liteui_theme::kPlaceholder;
  Color caretColor = liteui_theme::kCaret;
  Color selectionColor = Color{38, 79, 120, 220};
  Color borderColor = liteui_theme::kDivider;
  Color focusedBorderColor = liteui_theme::kAccent;
  Color lineNumberColor = Color{133, 133, 133};
  Color lineNumberBackground = liteui_theme::kEditorBg;

  bool showLineNumbers = true;
  float padding = 8.0f;
  float lineHeight = 0.0f; // 0 = auto (fontSize * 1.4)
  int tabWidthSpaces = 4;

  bool showScrollbar = true;
  float scrollbarWidth = 10.0f;
  Color scrollbarTrackColor = Color{255, 255, 255, 12};
  Color scrollbarThumbColor = Color{255, 255, 255, 60};

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
      ctx.setFillColor(Color{234, 192, 0, 90});
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
          ctx.setFillColor(Color{255, 255, 255, 50});
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
      ctx.setFillColor(Color{60, 60, 64, 245});
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

//==============Syntax ==========================

// This is a single generic, config-driven tokenizer (keywords/strings/
// comments/numbers/preprocessor lines) rather than a bespoke lexer per
// language — LanguageDef below just parameterizes it (comment syntax,
// string quote char, keyword set). It's not a real grammar engine (no
// TextMate-style nested scopes, no per-language edge cases like Python's
// triple-quoted strings or C++'s raw string literals) but it's a genuine
// tokenizer producing genuine colored spans, and it retokenizes
// incrementally rather than re-scanning the whole file on every keystroke
// — see SyntaxHighlighter::sync() for how.
//
// Requires liteui_editor_ext.hpp to be included first (for HighlightSpan
// and Color).

enum class SyntaxTokenType {
  Plain,
  Keyword,
  Comment,
  String,
  Number,
  Preprocessor
};

struct SyntaxToken {
  size_t start = 0, length = 0;
  SyntaxTokenType kind = SyntaxTokenType::Plain;
};

// Parameterizes the generic tokenizer below for one language. Not a real
// grammar — just enough knobs (keyword set, comment syntax, string quote)
// to make the same scanning loop work across a handful of C-like and
// scripting languages.
struct LanguageDef {
  std::string name;
  std::vector<std::string> extensions; // e.g. {".cpp", ".hpp"}, dot included
  std::unordered_set<std::string> keywords;
  std::string lineComment;       // e.g. "//" or "#"; empty = none
  std::string blockCommentStart; // e.g. "/*"; empty = no block comments
  std::string blockCommentEnd;   // e.g. "*/"
  bool hasStrings = true;
  char stringQuote = '"';
  bool hasChars = false; // 'x' style char literals, distinct from strings
  char charQuote = '\'';
  bool hasHashPreprocessor = false; // a line whose first non-space char is
                                    // '#' is colored as one Preprocessor
                                    // span for its whole remainder
};

inline bool syntaxIsIdentStart(unsigned char c) {
  return std::isalpha(c) || c == '_' || c >= 0x80; // >=0x80: UTF-8
                                                   // lead/continuation
                                                   // bytes count as
                                                   // identifier bytes so
                                                   // multi-byte identifiers
                                                   // don't get fragmented
}
inline bool syntaxIsIdentCont(unsigned char c) {
  return std::isalnum(c) || c == '_' || c >= 0x80;
}

// Tokenizes one line. `startInBlockComment` is the state carried in from
// the previous line (see SyntaxHighlighter for how that's threaded
// through a whole file); `endInBlockComment` is set to whatever state
// should carry into the *next* line.
//
// Known simplifications single-char string/char delimiters only
// (no Python triple-quotes, no C++ raw strings); a whole "starts with #"
// line is one Preprocessor span rather than parsing includes/macros
// specially; no template/generic-aware bracket matching.
inline std::vector<SyntaxToken> tokenizeLine(const std::string &line,
                                             const LanguageDef &lang,
                                             bool startInBlockComment,
                                             bool &endInBlockComment) {
  std::vector<SyntaxToken> tokens;
  size_t i = 0, n = line.size();
  bool inBlockComment = startInBlockComment;
  auto push = [&](size_t start, size_t len, SyntaxTokenType t) {
    if (len > 0)
      tokens.push_back(SyntaxToken{start, len, t});
  };

  if (inBlockComment) {
    size_t end = lang.blockCommentEnd.empty()
                     ? std::string::npos
                     : line.find(lang.blockCommentEnd, 0);
    if (end == std::string::npos) {
      push(0, n, SyntaxTokenType::Comment);
      endInBlockComment = true;
      return tokens;
    }
    push(0, end + lang.blockCommentEnd.size(), SyntaxTokenType::Comment);
    i = end + lang.blockCommentEnd.size();
    inBlockComment = false;
  }

  if (lang.hasHashPreprocessor) {
    size_t j = i;
    while (j < n && (line[j] == ' ' || line[j] == '\t'))
      ++j;
    if (j < n && line[j] == '#') {
      push(i, n - i, SyntaxTokenType::Preprocessor);
      endInBlockComment = false;
      return tokens;
    }
  }

  while (i < n) {
    unsigned char c = static_cast<unsigned char>(line[i]);

    if (!lang.lineComment.empty() &&
        line.compare(i, lang.lineComment.size(), lang.lineComment) == 0) {
      push(i, n - i, SyntaxTokenType::Comment);
      i = n;
      break;
    }
    if (!lang.blockCommentStart.empty() &&
        line.compare(i, lang.blockCommentStart.size(),
                     lang.blockCommentStart) == 0) {
      size_t end =
          line.find(lang.blockCommentEnd, i + lang.blockCommentStart.size());
      if (end == std::string::npos) {
        push(i, n - i, SyntaxTokenType::Comment);
        inBlockComment = true;
        i = n;
        break;
      }
      push(i, end + lang.blockCommentEnd.size() - i, SyntaxTokenType::Comment);
      i = end + lang.blockCommentEnd.size();
      continue;
    }
    if (lang.hasStrings && c == static_cast<unsigned char>(lang.stringQuote)) {
      size_t start = i++;
      while (i < n) {
        if (line[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        bool closing = static_cast<unsigned char>(line[i]) ==
                       static_cast<unsigned char>(lang.stringQuote);
        ++i;
        if (closing)
          break;
      }
      push(start, i - start, SyntaxTokenType::String);
      continue;
    }
    if (lang.hasChars && c == static_cast<unsigned char>(lang.charQuote)) {
      size_t start = i++;
      while (i < n) {
        if (line[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        bool closing = static_cast<unsigned char>(line[i]) ==
                       static_cast<unsigned char>(lang.charQuote);
        ++i;
        if (closing)
          break;
      }
      push(start, i - start, SyntaxTokenType::String);
      continue;
    }
    if (std::isdigit(c)) {
      size_t start = i++;
      while (i < n && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                       line[i] == '.' || line[i] == '_'))
        ++i; // crude but covers hex/float/suffix cases well enough to color
             // a number as a number
      push(start, i - start, SyntaxTokenType::Number);
      continue;
    }
    if (syntaxIsIdentStart(c)) {
      size_t start = i++;
      while (i < n && syntaxIsIdentCont(static_cast<unsigned char>(line[i])))
        ++i;
      std::string word = line.substr(start, i - start);
      push(start, i - start,
           lang.keywords.count(word) ? SyntaxTokenType::Keyword
                                     : SyntaxTokenType::Plain);
      continue;
    }
    // Punctuation/operators/whitespace: coalesce a run of "everything
    // else" bytes into one Plain span so we're not pushing one token per
    // character.
    size_t start = i++;
    while (i < n) {
      unsigned char cc = static_cast<unsigned char>(line[i]);
      if (syntaxIsIdentStart(cc) || std::isdigit(cc))
        break;
      if (lang.hasStrings && cc == static_cast<unsigned char>(lang.stringQuote))
        break;
      if (lang.hasChars && cc == static_cast<unsigned char>(lang.charQuote))
        break;
      if (!lang.lineComment.empty() &&
          line.compare(i, lang.lineComment.size(), lang.lineComment) == 0)
        break;
      if (!lang.blockCommentStart.empty() &&
          line.compare(i, lang.blockCommentStart.size(),
                       lang.blockCommentStart) == 0)
        break;
      ++i;
    }
    push(start, i - start, SyntaxTokenType::Plain);
  }

  endInBlockComment = inBlockComment;
  return tokens;
}

struct SyntaxTheme {
  Color plainColor = Color{212, 212, 212};
  Color keywordColor = Color{86, 156, 214};
  Color commentColor = Color{106, 153, 85};
  Color stringColor = Color{206, 145, 120};
  Color numberColor = Color{181, 206, 168};
  Color preprocessorColor = Color{197, 134, 192};
};

inline Color colorForToken(SyntaxTokenType t, const SyntaxTheme &theme) {
  switch (t) {
  case SyntaxTokenType::Keyword:
    return theme.keywordColor;
  case SyntaxTokenType::Comment:
    return theme.commentColor;
  case SyntaxTokenType::String:
    return theme.stringColor;
  case SyntaxTokenType::Number:
    return theme.numberColor;
  case SyntaxTokenType::Preprocessor:
    return theme.preprocessorColor;
  case SyntaxTokenType::Plain:
  default:
    return theme.plainColor;
  }
}

struct LineHighlight {
  bool valid = false;      // false means "never tokenized" or "explicitly
                           // invalidated" — distinct from an empty-but-valid
                           // token list for a blank line
  std::string sourceText;  // the line text this was computed from
  bool startState = false; // block-comment state entering this line
  bool endState = false;   // block-comment state leaving this line
  std::vector<SyntaxToken> tokens;
};

// Holds one document's per-line token cache and keeps it in sync with the
// buffer incrementally: sync() only retokenizes lines whose own text
// changed, or whose *incoming* block-comment state changed as a
// consequence of an earlier line's edit (e.g. typing "/*" partway through
// a file needs to recolor everything after it as a comment, until a
// matching "*/" is found) — every other line is a cheap string-equality
// cache hit.
//
// On a structural edit (a line inserted/removed/split/merged — Enter,
// Backspace/Delete across a line boundary, or a multi-line paste), the
// cache is realigned to the new line count by trimming the longest
// matching prefix and suffix against the old cache first, so only the
// (usually small) region actually touched needs recomputation rather than
// the whole file.
struct SyntaxHighlighter {
  std::shared_ptr<LanguageDef> lang;
  SyntaxTheme theme;
  std::vector<LineHighlight> cache;

  void sync(const std::vector<std::string> &lines) {
    if (!lang) {
      cache.clear();
      return;
    }
    if (cache.size() != lines.size())
      realign(lines);

    bool state = false; // block-comment state entering line 0
    for (size_t i = 0; i < lines.size(); ++i) {
      LineHighlight &lc = cache[i];
      if (lc.valid && lc.sourceText == lines[i] && lc.startState == state) {
        state = lc.endState;
        continue;
      }
      bool endState = false;
      lc.tokens = tokenizeLine(lines[i], *lang, state, endState);
      lc.sourceText = lines[i];
      lc.startState = state;
      lc.endState = endState;
      lc.valid = true;
      state = endState;
    }
  }

  // Converts cache[lineIndex]'s tokens into colored spans. Assumes sync()
  // has already been called for the current buffer this paint — see
  // CodeEditor::beforePaintSync/highlightLine in liteui_editor_ext.hpp,
  // which is exactly what calls these two in the right order.
  void spansFor(size_t lineIndex, std::vector<HighlightSpan> &out) const {
    out.clear();
    if (!lang || lineIndex >= cache.size())
      return;
    for (const SyntaxToken &tok : cache[lineIndex].tokens)
      out.push_back({tok.start, tok.length, colorForToken(tok.kind, theme)});
  }

private:
  void realign(const std::vector<std::string> &lines) {
    size_t oldN = cache.size(), newN = lines.size();
    size_t prefix = 0;
    while (prefix < oldN && prefix < newN && cache[prefix].valid &&
           cache[prefix].sourceText == lines[prefix])
      ++prefix;
    size_t maxSuffix = std::min(oldN - prefix, newN - prefix);
    size_t suffix = 0;
    while (suffix < maxSuffix && cache[oldN - 1 - suffix].valid &&
           cache[oldN - 1 - suffix].sourceText == lines[newN - 1 - suffix])
      ++suffix;

    std::vector<LineHighlight> rebuilt(newN);
    for (size_t i = 0; i < prefix; ++i)
      rebuilt[i] = cache[i];
    for (size_t i = 0; i < suffix; ++i)
      rebuilt[newN - 1 - i] = cache[oldN - 1 - i];
    // Everything strictly between prefix and (newN - suffix) is left
    // default-constructed (valid = false) and will be picked up by the
    // main sync() loop above, along with anything after it whose
    // effective incoming state turns out to have changed.
    cache = std::move(rebuilt);
  }
};

inline std::shared_ptr<SyntaxHighlighter>
makeHighlighter(std::shared_ptr<LanguageDef> lang, SyntaxTheme theme = {}) {
  auto h = std::make_shared<SyntaxHighlighter>();
  h->lang = std::move(lang);
  h->theme = theme;
  return h;
}

// Wires an existing SyntaxHighlighter into a CodeEditor's paint hooks.
// No-ops (leaves highlighting off) if the highlighter has no language set.
inline void applyHighlighting(CodeEditor &ed,
                              std::shared_ptr<SyntaxHighlighter> highlighter) {
  if (!highlighter || !highlighter->lang)
    return;
  ed.beforePaintSync = [highlighter](const std::vector<std::string> &lines) {
    highlighter->sync(lines);
  };
  ed.highlightLine = [highlighter](size_t i, const std::string &,
                                   std::vector<HighlightSpan> &out) {
    highlighter->spansFor(i, out);
  };
}

// ==================== built-in language definitions ====================

inline std::shared_ptr<LanguageDef> languageCpp() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "C++";
  l->extensions = {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx"};
  l->lineComment = "//";
  l->blockCommentStart = "/*";
  l->blockCommentEnd = "*/";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = true;
  l->charQuote = '\'';
  l->hasHashPreprocessor = true;
  l->keywords = {
      "alignas",       "alignof",     "and",
      "and_eq",        "asm",         "auto",
      "bitand",        "bitor",       "bool",
      "break",         "case",        "catch",
      "char",          "char8_t",     "char16_t",
      "char32_t",      "class",       "compl",
      "concept",       "const",       "consteval",
      "constexpr",     "constinit",   "const_cast",
      "continue",      "co_await",    "co_return",
      "co_yield",      "decltype",    "default",
      "delete",        "do",          "double",
      "dynamic_cast",  "else",        "enum",
      "explicit",      "export",      "extern",
      "false",         "float",       "for",
      "friend",        "goto",        "if",
      "inline",        "int",         "long",
      "mutable",       "namespace",   "new",
      "noexcept",      "not",         "not_eq",
      "nullptr",       "operator",    "or",
      "or_eq",         "private",     "protected",
      "public",        "register",    "reinterpret_cast",
      "requires",      "return",      "short",
      "signed",        "sizeof",      "static",
      "static_assert", "static_cast", "struct",
      "switch",        "template",    "this",
      "thread_local",  "throw",       "true",
      "try",           "typedef",     "typeid",
      "typename",      "union",       "unsigned",
      "using",         "virtual",     "void",
      "volatile",      "wchar_t",     "while",
      "xor",           "xor_eq",      "override",
      "final",
  };
  return l;
}

inline std::shared_ptr<LanguageDef> languagePython() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "Python";
  l->extensions = {".py", ".pyw"};
  l->lineComment = "#";
  l->blockCommentStart = ""; // no block comments; triple-quoted strings
                             // spanning lines aren't modeled (see file
                             // header's known-simplifications note)
  l->blockCommentEnd = "";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = false;
  l->hasHashPreprocessor = false;
  l->keywords = {
      "False",  "None",   "True",    "and",      "as",       "assert", "async",
      "await",  "break",  "class",   "continue", "def",      "del",    "elif",
      "else",   "except", "finally", "for",      "from",     "global", "if",
      "import", "in",     "is",      "lambda",   "nonlocal", "not",    "or",
      "pass",   "raise",  "return",  "try",      "while",    "with",   "yield",
  };
  return l;
}

inline std::shared_ptr<LanguageDef> languageJavaScript() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "JavaScript";
  l->extensions = {".js", ".jsx", ".mjs", ".ts", ".tsx"};
  l->lineComment = "//";
  l->blockCommentStart = "/*";
  l->blockCommentEnd = "*/";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = false;
  l->hasHashPreprocessor = false;
  l->keywords = {
      "break",     "case",     "catch",   "class",      "const",
      "continue",  "debugger", "default", "delete",     "do",
      "else",      "export",   "extends", "finally",    "for",
      "function",  "if",       "import",  "in",         "instanceof",
      "new",       "return",   "super",   "switch",     "this",
      "throw",     "try",      "typeof",  "var",        "let",
      "void",      "while",    "with",    "yield",      "async",
      "await",     "static",   "get",     "set",        "of",
      "interface", "type",     "enum",    "implements", "namespace",
      "as",        "from",
  };
  return l;
}

inline std::shared_ptr<LanguageDef>
languageForExtension(const std::string &path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return nullptr;
  std::string ext = path.substr(dot);
  for (auto &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::vector<std::shared_ptr<LanguageDef>> kLanguages = {
      languageCpp(),
      languagePython(),
      languageJavaScript(),
  };
  for (const auto &lang : kLanguages)
    for (const auto &e : lang->extensions)
      if (e == ext)
        return lang;
  return nullptr;
}

//================Editor Tabs =========================

inline std::string editorTitleFromPath(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// liteui.hpp ships openFilePicker/saveFilePicker but had no folder-picker
// when this file was first written — this adds one the same way (shell
// out to zenity/kdialog on Linux, the COM file dialog with
// FOS_PICKFOLDERS on Windows), reusing liteui_filepicker's existing
// commandExists/shellQuote helpers rather than duplicating them.
//
// Named openWorkspaceFolderDialog rather than the more obvious
// "openFolderPicker" specifically to avoid colliding with a same-named
// function some projects' own liteui.hpp may already have added
// independently — a plain function name this generic is exactly the kind
// of thing two people extending the same library end up picking
// separately, and a duplicate-definition error from that is a lot more
// confusing to debug than this comment is to read.
inline std::optional<std::string>
openWorkspaceFolderDialog(const std::string &title = "Open Folder") {
#if defined(_WIN32)
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                             COINIT_DISABLE_OLE1DDE);
  bool weInitialized = coHr == S_OK;
  IFileOpenDialog *dlg = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
  if (FAILED(hr)) {
    if (weInitialized)
      CoUninitialize();
    return std::nullopt;
  }
  dlg->SetTitle(toWide(title).c_str());
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts |
                  FOS_PICKFOLDERS); // the one difference from openFilePicker
  std::optional<std::string> result;
  hr = dlg->Show(nullptr);
  if (SUCCEEDED(hr)) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr,
                                      nullptr);
        std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
        if (len > 0)
          WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr,
                              nullptr);
        result = std::move(s);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  if (weInitialized)
    CoUninitialize();
  return result;
#else
  using namespace liteui_filepicker;
  std::string cmd;
  if (commandExists("zenity"))
    cmd = "zenity --file-selection --directory --title=" + shellQuote(title);
  else if (commandExists("kdialog"))
    cmd = "kdialog --getexistingdirectory . --title " + shellQuote(title);
  else
    return std::nullopt;
  cmd += " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return std::nullopt;
  std::string result;
  char buf[1024];
  while (fgets(buf, sizeof(buf), pipe))
    result += buf;
  int rc = pclose(pipe);
  if (rc != 0 || result.empty())
    return std::nullopt;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
#endif
}

// One entry in the workspace folder tree. Owned by TabbedEditor and
// persisted across rebuild()s (unlike the View tree itself, which is torn
// down and rebuilt every time) — see the note on rebuild() for why that
// makes raw FileTreeNode* safe to capture in click handlers.
struct FileTreeNode {
  std::string name;
  std::string fullPath;
  bool isDir = false;
  bool expanded = false;
  bool childrenLoaded = false; // directories are scanned lazily, on first
                               // expand, so opening a large workspace
                               // folder doesn't recursively walk the
                               // entire tree (or loop forever on a
                               // symlink cycle) up front
  std::vector<FileTreeNode> children;
};

// One open buffer. Kept alive independently of the View tree (which gets
// torn down and rebuilt on every structural change — see the file header)
// so a document's content and editing state are never tied to any
// particular tree instance.
struct EditorDocument {
  std::string title = "untitled";
  std::string path;
  bool hasPath = false;
  // Soft-delete rather than actually erasing from the vector: several
  // Dynamic<> closures captured in the *previous* tree's Views (which may
  // still be mid-callback when a close happens — e.g. a close button's
  // own onClick) close over indices into docs_. Erasing would invalidate
  // those indices out from under a callback that's still running: rebuild()
  // simply skips closed documents rather than shrinking the vector.
  bool closed = false;

  // True for the VS-Code-style landing tab (see TabbedEditor::
  // newWelcomeTab()). Such a document has no real buffer or backing
  // file — buildEditor() renders it via buildWelcomePane() instead of a
  // CodeEditor, and saveActive()/the status bar skip it accordingly.
  bool isWelcome = false;
  std::shared_ptr<CodeEditorState> state = std::make_shared<CodeEditorState>();
  std::shared_ptr<bool> modified = std::make_shared<bool>(false);
  // Always non-null (even for a plain-text/unknown extension, where it
  // just has lang == nullptr and applyHighlighting() no-ops on it) so
  // callers never need to null-check it. Kept per-document rather than
  // rebuilt from scratch each time so its incremental token cache
  // (SyntaxHighlighter::cache) survives tab open/close/switch rebuilds —
  // the same reasoning as keeping `state` per-document instead of
  // recreating it.
  std::shared_ptr<SyntaxHighlighter> highlighter = makeHighlighter(nullptr);
};

// Scans one directory level into node.children, directories first then
// alphabetical (case-insensitive). No-op if already loaded. Free function
// (not a TabbedEditor method) so it's independently testable and reusable.
inline void loadFileTreeChildren(FileTreeNode &node) {
  if (node.childrenLoaded)
    return;
  node.childrenLoaded = true;
  node.children.clear();
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(node.fullPath, ec)) {
    if (ec)
      break;
    FileTreeNode child;
    child.name = entry.path().filename().string();
    child.fullPath = entry.path().string();
    std::error_code ec2;
    child.isDir = entry.is_directory(ec2);
    node.children.push_back(std::move(child));
  }
  std::sort(node.children.begin(), node.children.end(),
            [](const FileTreeNode &a, const FileTreeNode &b) {
              if (a.isDir != b.isDir)
                return a.isDir > b.isDir; // directories before files
              std::string al = a.name, bl = b.name;
              std::transform(al.begin(), al.end(), al.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              std::transform(bl.begin(), bl.end(), bl.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              return al < bl;
            });
}

class TabbedEditor {
public:
  TabbedEditor(const std::string &windowTitle = "CODE")
      : ui_(windowTitle, -1, -1, true), title_(windowTitle),
        activeIndex_(std::make_shared<int>(-1)) {
    ui_.setWindowBackground(th::kEditorBg);
    ui_.setScrollbarColors(th::kScrollTrack, th::kScrollThumb);
    newWelcomeTab();
    spawnNewTerminal(); // start with one terminal, like VS Code's default
    ui_.addInterval(33, [this] {
      for (auto &t : terminals_)
        t.state->pollOutput();
    });
    buildRoot(); // built once; tab strip + editor stack reconcile themselves
    setupShortcuts();
  }

  ~TabbedEditor() {
    for (auto &t : terminals_)
      t.state->shutdown();
  }

  // One poll before the loop starts, so any openFile()/newDocument() the
  // caller made between construction and run() is reconciled into the tree
  // before the first paint. Once the loop is running, LiteUI's own
  // pollAndRelayout() (after every dispatched event) handles it. Safe here
  // precisely because it is NOT inside a handler — see closeTab().
  void run() {
    ui_.requestRepaint();
    ui_.run();
  }

  // Opens a file into a new tab, or focuses it if it's already open.
  void openFile(const std::string &path) {
    for (size_t i = 0; i < docs_.size(); ++i) {
      if (!docs_[i].closed && docs_[i].hasPath && docs_[i].path == path) {
        *activeIndex_ = static_cast<int>(i);
        return;
      }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      showErrorDialog("Couldn't open \"" + editorTitleFromPath(path) +
                      "\" — it may have been moved, deleted, or you may "
                      "not have permission to read it.");
      return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();

    EditorDocument doc;
    doc.path = path;
    doc.hasPath = true;
    doc.title = editorTitleFromPath(path);
    doc.highlighter = makeHighlighter(languageForExtension(path));
    splitLinesInto(ss.str(), doc.state->lines);
    doc.state->cursor = {0, 0};

    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // Opens `path` (or focuses its existing tab) and moves the cursor to
  // `line` (0-based). Used by workspace search results — a plain
  // openFile() either resets the cursor to (0,0) for a fresh tab or
  // leaves it untouched for an already-open one, neither of which is
  // what a "jump to this match" click wants.
  void openFileAtLine(const std::string &path, size_t line) {
    openFile(path);
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath || doc.path != path)
        continue;
      if (line < doc.state->lines.size()) {
        doc.state->cursor = {line, 0};
        doc.state->selectionAnchor.reset();
        doc.state->noteCursorMoved();
        doc.state->dirty = true;
      }
      break;
    }
  }

  void newDocument() {
    EditorDocument doc;
    doc.title = "untitled-" + std::to_string(++untitledCounter_);
    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // A VS-Code-style landing tab shown on first launch (and again if every
  // other tab gets closed — see closeTab()'s all-closed fallback below):
  // no backing file, just Open File / Open Folder / New File shortcuts.
  // Rendered by buildWelcomePane() rather than a CodeEditor — see
  // buildEditor()'s isWelcome branch.
  void newWelcomeTab() {
    EditorDocument doc;
    doc.title = "Welcome";
    doc.isWelcome = true;
    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // Returns false if the doc needed a path and the picker was cancelled
  // — lets confirmUnsavedDialog() know not to close the tab in that case.
  bool saveDocument(size_t idx, bool forcePickPath = false) {
    if (idx >= docs_.size())
      return false;
    EditorDocument &doc = docs_[idx];
    if (doc.isWelcome)
      return true;
    if (!doc.hasPath || forcePickPath) {
      auto picked =
          saveFilePicker("Save File", doc.title,
                         {{"Text", "*.txt"}, {"All Files", "*"}}, "txt");
      if (!picked)
        return false;
      doc.path = *picked;
      doc.hasPath = true;
      doc.title = editorTitleFromPath(doc.path);
      doc.highlighter = makeHighlighter(languageForExtension(doc.path));
    }
    std::ofstream out(doc.path, std::ios::binary);
    out << joinLines(doc.state->lines);
    *doc.modified = false;
    return true;
  }

  void saveActive(bool forcePickPath = false) {
    if (*activeIndex_ >= 0)
      saveDocument(static_cast<size_t>(*activeIndex_), forcePickPath);
  }

  // ---- Edit menu actions (act on the active tab, not a right-click target)

  void editUndo() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome)
      return;
    doc->state->undo();
    doc->state->dirty = true;
    *doc->modified = true;
  }
  void editRedo() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome)
      return;
    doc->state->redo();
    doc->state->dirty = true;
    *doc->modified = true;
  }
  void editCopy() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome || !doc->state->hasSelection())
      return;
    liteui_clipboard::setTextWithFallback(doc->state->selectedText());
  }
  void editCut() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome || !doc->state->hasSelection())
      return;
    liteui_clipboard::setTextWithFallback(doc->state->selectedText());
    doc->state->beginEdit(false);
    doc->state->deleteSelectionRaw();
    doc->state->dirty = true;
    *doc->modified = true;
  }
  void editPaste() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome)
      return;
    std::string clip = liteui_clipboard::getTextWithFallback();
    if (clip.empty())
      return;
    doc->state->beginEdit(false);
    doc->state->insertTextRaw(clip);
    doc->state->dirty = true;
    *doc->modified = true;
  }
  void editSelectAll() {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome)
      return;
    doc->state->selectAll();
    doc->state->noteCursorMoved();
    doc->state->dirty = true;
  }

  // Public entry point: if the tab has unsaved changes, prompts before
  // closing instead of closing immediately.
  void closeTab(size_t i) {
    if (i >= docs_.size() || docs_[i].closed)
      return;
    if (!docs_[i].isWelcome && *docs_[i].modified) {
      unsavedDialogDocIndex_ = i;
      *unsavedDialogOpen_ = true;
      return;
    }
    closeTabForce(i);
  }

  // Unconditional close — this is the old closeTab() body verbatim, now
  // only reached once the modified-check above is satisfied (or
  // bypassed via "Don't Save").
  void closeTabForce(size_t i) {
    if (i >= docs_.size() || docs_[i].closed)
      return;
    docs_[i].closed = true;
    if (docs_[i].state->blinkTimerHandle >= 0) {
      ui_.removeInterval(docs_[i].state->blinkTimerHandle);
      docs_[i].state->blinkTimerHandle = -1;
    }
    docs_[i].state->focused = false;

    if (static_cast<int>(i) == *activeIndex_) {
      int next = -1;
      for (int k = static_cast<int>(i) - 1; k >= 0; --k)
        if (!docs_[static_cast<size_t>(k)].closed) {
          next = k;
          break;
        }
      if (next < 0)
        for (size_t k = i + 1; k < docs_.size(); ++k)
          if (!docs_[k].closed) {
            next = static_cast<int>(k);
            break;
          }
      *activeIndex_ = next;
    }
    if (std::all_of(docs_.begin(), docs_.end(),
                    [](const EditorDocument &d) { return d.closed; }))
      newWelcomeTab();
  }

  // ---- introspection (status bar, tests, etc.) ----
  int activeIndex() const { return *activeIndex_; }
  size_t openDocumentCount() const {
    return static_cast<size_t>(
        std::count_if(docs_.begin(), docs_.end(),
                      [](const EditorDocument &d) { return !d.closed; }));
  }
  const EditorDocument *documentAt(size_t i) const {
    return i < docs_.size() ? &docs_[i] : nullptr;
  }
  const EditorDocument *activeDocument() const {
    return (*activeIndex_ >= 0 &&
            static_cast<size_t>(*activeIndex_) < docs_.size())
               ? &docs_[static_cast<size_t>(*activeIndex_)]
               : nullptr;
  }

  void nextTab(int direction) { // +1 or -1, wraps, skips closed tabs
    std::vector<size_t> open;
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed)
        open.push_back(i);
    if (open.size() < 2)
      return;
    auto it =
        std::find(open.begin(), open.end(), static_cast<size_t>(*activeIndex_));
    size_t pos =
        (it == open.end()) ? 0 : static_cast<size_t>(it - open.begin());
    size_t n = open.size();
    pos = (pos + static_cast<size_t>(direction) + n) % n;
    *activeIndex_ = static_cast<int>(open[pos]);
  }

private:
  LiteUI ui_;
  std::string title_;
  std::vector<EditorDocument> docs_;
  std::shared_ptr<int> activeIndex_; // shared so Dynamic<> closures in the
                                     // *current* tree can read it without
                                     // caring that the tree gets replaced
                                     // out from under them on every rebuild
  int untitledCounter_ = 0;

  std::shared_ptr<int> openMenuIndex_ = std::make_shared<int>(-1);

  // Which activity-bar item is selected: 0 = Explorer, 1 = Search, -1 =
  // panel collapsed. shared_ptr for the same reason activeIndex_ is —
  // every Dynamic<> in the tree reads it live.
  std::shared_ptr<int> activityIndex_ = std::make_shared<int>(0);

  // Terminal panel height, resizable via its own horizontal divider — same
  // shared_ptr/clamp pattern as sidePanelWidth_, just on the vertical axis.
  std::shared_ptr<float> terminalHeight_ = std::make_shared<float>(180.0f);
  static constexpr float kMinTerminalHeight = 0.0f;
  static constexpr float kMaxTerminalHeight = 400.0f;
  static constexpr float kHDividerHeight = 6.0f;

  // One terminal instance: its own PTY-backed state (see
  // liteui_terminal.hpp) plus the label shown in the list on the right.
  // Index-based keys are fine for now since terminals_ is append-only —
  // switch to a stable per-tab id (like docs_ does) once close support
  // lands and entries can actually disappear from the middle.
  struct TerminalTab {
    std::shared_ptr<liteui_terminal::TerminalState> state =
        std::make_shared<liteui_terminal::TerminalState>();
    std::string label = "powershell";
    bool closed = false;
  };
  std::vector<TerminalTab> terminals_;
  std::shared_ptr<int> activeTerminalIndex_ = std::make_shared<int>(0);

  // Resizable side panel width, shared with the divider's drag handler and
  // the panel's own Dynamic<Size> width — same reasoning as activityIndex_:
  // every closure that reads it needs to survive this tree being rebuilt.
  std::shared_ptr<float> sidePanelWidth_ = std::make_shared<float>(300.0f);
  static constexpr float kMinSidePanelWidth = 0.0f;
  static constexpr float kMaxSidePanelWidth = 580.0f;
  static constexpr float kSideDividerWidth = 6.0f;

  // Right-click menu for the code editor (Cut / Copy / Paste).
  std::shared_ptr<bool> editorMenuOpen_ = std::make_shared<bool>(false);
  std::shared_ptr<float> editorMenuX_ = std::make_shared<float>(0.0f);
  std::shared_ptr<float> editorMenuY_ = std::make_shared<float>(0.0f);
  size_t editorMenuDocIndex_ = 0;

  // Named so the pane-visibility checks in buildExplorerPane()/
  // buildSearchPane() below can't silently drift out of sync with
  // activityItems()'s ids if the list is ever reordered or extended.
  static constexpr int kExplorerActivityId = 0;
  static constexpr int kSearchActivityId = 1;

  // One entry per sidebar view. Shared by buildActivityBar() (which
  // renders the icon column) and buildSidePanel() (whose header needs the
  // matching full name) so the id -> name mapping lives in exactly one
  // place — previously the panel header had its own hardcoded ternary
  // that would silently show the wrong name for any activity beyond the
  // first two.
  struct ActivityItem {
    int id;
    std::string label;   // short glyph shown in the bar
    std::string tooltip; // full name shown in the bar's tooltip and as
    // the side panel's header
    std::string icons;
  };
  static const std::vector<ActivityItem> &activityItems() {
    static const std::vector<ActivityItem> items = {
        {kExplorerActivityId, "E", "Explorer",
         R"(<?xml version="1.0" encoding="utf-8"?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 0 48 48" xmlns="http://www.w3.org/2000/svg"><defs><style>.a{fill:none;stroke:#000000;stroke-linecap:round;stroke-linejoin:round;}</style></defs><path class="a" d="M41.6783,13.0436H24.77c-1.9628-.1072-5.9311-4.2372-8.1881-4.2372H6.6806V8.8046A2.1762,2.1762,0,0,0,4.5,10.9763v7.3063h39V14.8652A1.8217,1.8217,0,0,0,41.6783,13.0436Z"/><path class="a" d="M43.5,18.2826H4.5V37.0165a2.1762,2.1762,0,0,0,2.1735,2.1789H41.3194A2.1762,2.1762,0,0,0,43.5,37.0237V18.2826Z"/></svg>)"},
        {kSearchActivityId, "S", "Search",
         R"(<?xml version="1.0" encoding="utf-8"?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg fill="#000000" width="800px" height="800px" viewBox="0 0 1920 1920" xmlns="http://www.w3.org/2000/svg">
    <path d="M790.588 1468.235c-373.722 0-677.647-303.924-677.647-677.647 0-373.722 303.925-677.647 677.647-677.647 373.723 0 677.647 303.925 677.647 677.647 0 373.723-303.924 677.647-677.647 677.647Zm596.781-160.715c120.396-138.692 193.807-319.285 193.807-516.932C1581.176 354.748 1226.428 0 790.588 0S0 354.748 0 790.588s354.748 790.588 790.588 790.588c197.647 0 378.24-73.411 516.932-193.807l516.028 516.142 79.963-79.963-516.142-516.028Z" fill-rule="evenodd"/>
</svg>)"},
    };
    return items;
  }

  // Explorer state: a real expand/collapse tree rooted at the opened
  // workspace folder, matching VS Code's explorer. explorerRoot_ is
  // nullopt until a folder is opened; each FileTreeNode lazily loads its
  // own children the first time it's expanded (see loadFileTreeChildren,
  // already used this way by loadFileTreeChildren's own callers).
  std::string workspaceRoot_;
  std::optional<FileTreeNode> explorerRoot_;
  // Currently selected explorer item (a full path — either a file or a
  // folder). Defaults to the workspace root itself once one is opened,
  // matching VS Code's "root is selected until you click something
  // else" behavior. New File/New Folder target this (see
  // selectedDirectory()) and buildExplorerRow()/buildExplorerHeaderRow()
  // use it to paint the selection highlight.
  std::string selectedPath_;

  // ---- workspace text search (Ctrl+Shift+F pane) ----
  struct WorkspaceSearchMatch {
    std::string path;
    size_t line = 0;      // 0-based
    std::string lineText; // untrimmed, for the snippet preview
  };
  std::string workspaceSearchQuery_;
  std::vector<WorkspaceSearchMatch> workspaceSearchResults_;
  std::shared_ptr<TextInputState> searchInputState_ =
      std::make_shared<TextInputState>();

  std::string workspaceReplaceText_;
  std::shared_ptr<TextInputState> replaceInputState_ =
      std::make_shared<TextInputState>();
  static constexpr size_t kMaxSearchResults = 500;
  static constexpr uintmax_t kMaxSearchFileBytes = 2 * 1024 * 1024; // 2 MB

  void openEditorContextMenu(size_t docIdx, float x, float y) {
    editorMenuDocIndex_ = docIdx;
    *editorMenuX_ = x;
    *editorMenuY_ = y;
    *editorMenuOpen_ = true;
  }

  bool editorMenuHasSelection() const {
    return editorMenuDocIndex_ < docs_.size() &&
           docs_[editorMenuDocIndex_].state->hasSelection();
  }

  void editorContextCopy() {
    if (editorMenuDocIndex_ >= docs_.size())
      return;
    auto &st = docs_[editorMenuDocIndex_].state;
    if (st->hasSelection())
      liteui_clipboard::setTextWithFallback(st->selectedText());
  }

  void editorContextCut() {
    if (editorMenuDocIndex_ >= docs_.size())
      return;
    EditorDocument &doc = docs_[editorMenuDocIndex_];
    if (!doc.state->hasSelection())
      return;
    liteui_clipboard::setTextWithFallback(doc.state->selectedText());
    doc.state->beginEdit(false); // its own undo step
    doc.state->deleteSelectionRaw();
    doc.state->dirty = true;
    *doc.modified = true;
  }

  void editorContextPaste() {
    if (editorMenuDocIndex_ >= docs_.size())
      return;
    EditorDocument &doc = docs_[editorMenuDocIndex_];
    std::string clip = liteui_clipboard::getTextWithFallback();
    if (clip.empty())
      return;
    doc.state->beginEdit(false);
    doc.state->insertTextRaw(clip);
    doc.state->dirty = true;
    *doc.modified = true;
  }

  // Directory names never worth descending into for a text search —
  // typically huge, generated, or binary-heavy.
  static bool isSearchExcludedDir(const std::string &name) {
    static const std::unordered_set<std::string> kExcluded = {
        ".git", "node_modules", "build", "out", ".cache", "dist", ".vs"};
    return kExcluded.count(name) != 0;
  }

  // Recursively walks workspaceRoot_, doing a case-insensitive substring
  // search of `query` across every line of every file it can read as
  // text. Deliberately simple (phase 1): no regex, no whole-word/case
  // toggle, no incremental indexing — a linear rescan on every keystroke,
  // capped by kMaxSearchResults/kMaxSearchFileBytes so a huge or
  // pathological workspace can't hang the UI.
  void runWorkspaceSearch(const std::string &query) {
    workspaceSearchQuery_ = query;
    workspaceSearchResults_.clear();
    if (query.empty() || workspaceRoot_.empty())
      return;
    std::string needle = asciiLower(query);

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        workspaceRoot_,
        std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (workspaceSearchResults_.size() >= kMaxSearchResults)
        break;
      const std::filesystem::directory_entry &entry = *it;

      std::error_code typeEc;
      if (entry.is_directory(typeEc)) {
        if (isSearchExcludedDir(entry.path().filename().string()))
          it.disable_recursion_pending();
        continue;
      }
      if (typeEc || !entry.is_regular_file(typeEc))
        continue;

      std::error_code sizeEc;
      uintmax_t sz = entry.file_size(sizeEc);
      if (sizeEc || sz > kMaxSearchFileBytes)
        continue;

      std::ifstream in(entry.path(), std::ios::binary);
      if (!in)
        continue;
      std::ostringstream ss;
      ss << in.rdbuf();
      std::string content = ss.str();
      // Crude binary-file guard: real text files essentially never
      // contain a NUL byte.
      if (content.find('\0') != std::string::npos)
        continue;

      std::vector<std::string> lines;
      splitLinesInto(content, lines);
      std::string path = entry.path().string();
      for (size_t li = 0; li < lines.size(); ++li) {
        std::string hay = asciiLower(lines[li]);
        if (hay.find(needle) == std::string::npos)
          continue;
        workspaceSearchResults_.push_back({path, li, lines[li]});
        if (workspaceSearchResults_.size() >= kMaxSearchResults)
          break;
      }
    }
  }

  // Replaces every case-insensitive occurrence of `needle` in `line`
  // with `replacement`, preserving everything else in the line
  // byte-for-byte. Mirrors the matching logic runWorkspaceSearch already
  // uses (asciiLower + find), so "what counts as a match" stays
  // consistent between the search pass and the replace pass.
  static std::string caseInsensitiveReplaceAll(const std::string &line,
                                               const std::string &needle,
                                               const std::string &replacement) {
    if (needle.empty())
      return line;
    std::string hay = asciiLower(line);
    std::string lneedle = asciiLower(needle);
    std::string result;
    size_t pos = 0;
    while (true) {
      size_t found = hay.find(lneedle, pos);
      if (found == std::string::npos) {
        result += line.substr(pos);
        break;
      }
      result += line.substr(pos, found - pos);
      result += replacement;
      pos = found + needle.size();
    }
    return result;
  }

  // Replaces the query text on one specific search-result line, either
  // through the file's already-open CodeEditorState (so a user with
  // unsaved edits doesn't get overwritten from under them, and undo
  // still works via beginEdit) or, if the file isn't open, directly on
  // disk. Either way the line count can't change (neither the query nor
  // the replacement text can contain '\n' — TextInput is single-line),
  // so every other result's stored line number stays valid; only this
  // line's own content changes. Finishes with a full rescan rather than
  // patching workspaceSearchResults_ in place — replacing may make this
  // line stop matching, or start matching a second time, or (if the
  // replacement text itself contains the query) matter for later
  // results too, and a rescan is the simplest way to stay correct.
  void replaceMatch(size_t idx) {
    if (idx >= workspaceSearchResults_.size() || workspaceSearchQuery_.empty())
      return;
    WorkspaceSearchMatch m = workspaceSearchResults_[idx]; // copy: the
                                                           // vector gets
                                                           // cleared by
                                                           // the rescan
                                                           // below
    bool handledInOpenDoc = false;
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath || doc.path != m.path)
        continue;
      if (m.line < doc.state->lines.size()) {
        std::string replaced = caseInsensitiveReplaceAll(
            doc.state->lines[m.line], workspaceSearchQuery_,
            workspaceReplaceText_);
        if (replaced != doc.state->lines[m.line]) {
          doc.state->beginEdit(false); // its own undo step, like paste/cut
          doc.state->lines[m.line] = replaced;
          doc.state->clampCursor();
          doc.state->dirty = true;
        }
      }
      std::ofstream out(doc.path, std::ios::binary);
      out << joinLines(doc.state->lines);
      *doc.modified = false; // matches saveActive()'s own bookkeeping
      handledInOpenDoc = true;
      break;
    }

    if (!handledInOpenDoc) {
      std::ifstream in(m.path, std::ios::binary);
      if (!in)
        return; // TODO surface a real error dialog/status message
      std::ostringstream ss;
      ss << in.rdbuf();
      std::vector<std::string> lines;
      splitLinesInto(ss.str(), lines);
      if (m.line >= lines.size())
        return;
      lines[m.line] = caseInsensitiveReplaceAll(
          lines[m.line], workspaceSearchQuery_, workspaceReplaceText_);
      std::ofstream out(m.path, std::ios::binary);
      out << joinLines(lines);
    }

    runWorkspaceSearch(workspaceSearchQuery_);
  }

  // Applies the current query -> replacement across every visible search
  // result in one pass, grouped by file so each file is read/written (or
  // its open CodeEditorState edited) exactly once rather than once per
  // match. Unlike replaceMatch(), this deliberately does NOT call
  // runWorkspaceSearch() after every single replacement — for a result
  // set spanning many files that would mean re-walking the entire
  // workspace once per match. Instead it walks workspaceSearchResults_
  // once up front to build a path -> line-numbers map, applies each
  // file's replacements in one shot, and rescans exactly once at the end.
  void replaceAllMatches() {
    if (workspaceSearchResults_.empty() || workspaceSearchQuery_.empty())
      return;

    // path -> the distinct lines within it that matched, in first-seen
    // order. Each WorkspaceSearchMatch is already one entry per matching
    // line (runWorkspaceSearch stops scanning a line the moment it finds
    // one hit — see its inner loop), so no further de-duplication of
    // line numbers is needed here, only grouping by path.
    std::vector<std::string> orderedPaths;
    std::unordered_map<std::string, std::vector<size_t>> linesByPath;
    for (const auto &m : workspaceSearchResults_) {
      auto [it, inserted] = linesByPath.try_emplace(m.path);
      if (inserted)
        orderedPaths.push_back(m.path);
      it->second.push_back(m.line);
    }

    for (const std::string &path : orderedPaths) {
      const std::vector<size_t> &targetLines = linesByPath[path];

      bool handledInOpenDoc = false;
      for (auto &doc : docs_) {
        if (doc.closed || !doc.hasPath || doc.path != path)
          continue;
        bool changedAny = false;
        for (size_t line : targetLines) {
          if (line >= doc.state->lines.size())
            continue;
          std::string replaced = caseInsensitiveReplaceAll(
              doc.state->lines[line], workspaceSearchQuery_,
              workspaceReplaceText_);
          if (replaced != doc.state->lines[line]) {
            if (!changedAny)
              doc.state->beginEdit(false); // one undo step per file
            doc.state->lines[line] = replaced;
            changedAny = true;
          }
        }
        if (changedAny) {
          doc.state->clampCursor();
          doc.state->dirty = true;
        }
        std::ofstream out(doc.path, std::ios::binary);
        out << joinLines(doc.state->lines);
        *doc.modified = false; // matches saveActive()'s own bookkeeping
        handledInOpenDoc = true;
        break;
      }
      if (handledInOpenDoc)
        continue;

      std::ifstream in(path, std::ios::binary);
      if (!in)
        continue; // TODO surface a real error dialog/status message
      std::ostringstream ss;
      ss << in.rdbuf();
      std::vector<std::string> lines;
      splitLinesInto(ss.str(), lines);
      for (size_t line : targetLines) {
        if (line >= lines.size())
          continue;
        lines[line] = caseInsensitiveReplaceAll(
            lines[line], workspaceSearchQuery_, workspaceReplaceText_);
      }
      std::ofstream out(path, std::ios::binary);
      out << joinLines(lines);
    }

    runWorkspaceSearch(workspaceSearchQuery_);
  }

  // One result *slot* (not a keyed row): built once per index, up to
  // kMaxSearchResults of them, and kept mounted for the life of the pane.
  // Everything it shows is a Dynamic<> callback reading
  // workspaceSearchResults_[idx] live, and it hides itself via
  // Display::None once idx falls outside the current result count.
  //
  // Deliberately NOT a keyed list: the previous version rebuilt the
  // results as a keysSource/itemBuilder list, whose key count changes on
  // every keystroke. Any keyed-list reconcile anywhere in the tree makes
  // LiteUI::pollAndRelayout() call invalidateViewPointers() as a safety
  // measure (see its comment in liteui.hpp) — which unconditionally
  // nulls focusedView_, since it can't tell which cached View* pointers
  // are still valid. That silently kicked focus off the search
  // TextInput after every single character, even though the TextInput's
  // own View never moved. A fixed, always-mounted pool of slots sidesteps
  // the problem entirely: nothing structural ever changes while typing.
  View buildSearchResultSlot(size_t idx) {
    View row;
    row.style.direction = FlexDirection::Row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding = EdgeInsets{5, 12, 5, 12};
    row.style.gap = 8;
    row.style.backgroundColor = th::kSideBarBg;
    row.style.hoverColor = th::kHoverBg;
    row.style.display = [this, idx]() -> Display {
      return idx < workspaceSearchResults_.size() ? Display::Flex
                                                  : Display::None;
    };
    row.onClick = [this, idx] {
      if (idx >= workspaceSearchResults_.size())
        return;
      const WorkspaceSearchMatch &m = workspaceSearchResults_[idx];
      openFileAtLine(m.path, m.line);
    };

    View textCol;
    textCol.style.backgroundColor = th::kTransparent;
    textCol.style.direction = FlexDirection::Column;
    textCol.style.flexGrow = 1;
    textCol.style.gap = 2;

    Text top;
    top.label = std::function<std::string()>([this, idx]() -> std::string {
      if (idx >= workspaceSearchResults_.size())
        return std::string();
      const WorkspaceSearchMatch &m = workspaceSearchResults_[idx];
      return editorTitleFromPath(m.path) + ":" + std::to_string(m.line + 1);
    });
    top.fontSize = 12;
    top.fontWeight = FontWeight::SemiBold;
    top.color = th::kText;
    textCol.addChild(top);

    Text snippetText;
    snippetText.label =
        std::function<std::string()>([this, idx]() -> std::string {
          if (idx >= workspaceSearchResults_.size())
            return std::string(" ");
          std::string snippet =
              trimWhitespace(workspaceSearchResults_[idx].lineText);
          if (snippet.size() > 140)
            snippet = snippet.substr(0, 140) + "...";
          return snippet.empty() ? std::string(" ") : snippet;
        });
    snippetText.fontSize = 12;
    snippetText.color = th::kTextMuted;
    snippetText.overflow = TextOverflow::Ellipsis;
    snippetText.wrap = TextWrap::NoWrap;
    textCol.addChild(snippetText);

    row.addChild(std::move(textCol));

    // "R" = replace just this match. Own onClick, so it consumes the
    // click before it ever reaches row.onClick's "open the file" — same
    // pattern as buildTab's closeBtn sitting inside a clickable tab.
    View replaceBtn;
    replaceBtn.style.width = Size::pixel(22);
    replaceBtn.style.height = Size::pixel(22);
    replaceBtn.style.flexShrink = 0;
    replaceBtn.style.justifyContent = Justify::Center;
    replaceBtn.style.alignItems = Align::Center;
    replaceBtn.style.borderRadius = 3.0f;
    replaceBtn.style.backgroundColor = Color{0, 0, 0, 0};
    replaceBtn.style.hoverColor = th::kButtonHoverBg;
    replaceBtn.tooltip = "Replace this match";
    replaceBtn.onClick = [this, idx] { replaceMatch(idx); };
    Text replaceLabel;
    replaceLabel.label = std::string("R");
    replaceLabel.fontSize = 11;
    replaceLabel.fontWeight = FontWeight::SemiBold;
    replaceLabel.color = th::kTextMuted;
    replaceBtn.addChild(replaceLabel);
    row.addChild(std::move(replaceBtn));

    return row;
  }

  // Inline create-new-item state: while active, the explorer shows a
  // TextInput row (see buildPendingCreateRow) inside parentDir instead of
  // immediately creating a file/folder with a placeholder name. Enter
  // submits (see submitPendingCreate); clicking anywhere else discards it
  // (see checkPendingCreateBlur/cancelPendingCreate).
  struct PendingCreate {
    bool active = false;
    bool isDir = false;
    std::string parentDir;
  };
  PendingCreate pendingCreate_;
  // Whether the input has ever actually received focus since it was
  // created. Clicking N/F doesn't itself focus the new TextInput (liteui
  // has no public API to force keyboard focus — see beginPendingCreate),
  // so the very first "not focused" poll after creation must NOT be
  // read as a blur, or the row would vanish before anyone could click it.
  bool pendingCreateEverFocused_ = false;
  // Rebuilt fresh every time a creation starts or ends (see
  // resetPendingCreate), so a stale blinking caret or leftover typed text
  // from a previously discarded input can never bleed into the next one.
  std::shared_ptr<TextInputState> pendingInputState_ =
      std::make_shared<TextInputState>();
  // Sentinel key spliced into explorerKeys() while a creation is active;
  // chosen so it can never collide with a real filesystem path.
  static constexpr const char *kPendingCreateKey =
      "\x01__liteui_pending_create__";

  // Inline rename state — same shape as PendingCreate, but replaces an
  // *existing* row's rendering instead of adding a new one. explorerKeys()
  // swaps the target node's own key for a rename-sentinel key (see
  // collectExplorerKeys) so reconcileChildren actually notices the change
  // and calls itemBuilder again for that row, rather than reusing the
  // already-built plain row untouched.
  struct PendingRename {
    bool active = false;
    bool isDir = false;
    std::string originalPath;
  };
  PendingRename pendingRename_;
  // Same "hasn't been clicked into yet" guard as pendingCreateEverFocused_.
  bool pendingRenameEverFocused_ = false;
  std::shared_ptr<TextInputState> pendingRenameInputState_ =
      std::make_shared<TextInputState>();
  // Prefix rather than a single fixed sentinel (unlike kPendingCreateKey)
  // since a rename target is a specific existing path, not a generic
  // "one pending slot" — the prefix plus path makes each rename key unique
  // and keeps it out of the way of any real filesystem path.
  static constexpr const char *kPendingRenameKeyPrefix =
      "\x02__liteui_pending_rename__:";

  // Right-click context menu for explorer rows (Open/Rename/Delete for a
  // file, New File/New Folder/Rename/Delete for a folder).
  std::shared_ptr<bool> explorerMenuOpen_ = std::make_shared<bool>(false);
  std::string explorerMenuTargetPath_;
  bool explorerMenuTargetIsDir_ = false;
  std::shared_ptr<float> explorerMenuX_ = std::make_shared<float>(0.0f);
  std::shared_ptr<float> explorerMenuY_ = std::make_shared<float>(0.0f);

  // Delete-confirmation dialog state. deleteDialogOpen_ is a shared_ptr
  // for the same reason activeIndex_/activityIndex_ are — the dialog's
  // own Dynamic<Display>/onClick closures need to read it live across
  // rebuilds. deleteTargetPath_/deleteTargetIsDir_ are captured once,
  // when the dialog opens, so the confirm/cancel buttons always act on
  // whatever was selected at that moment rather than whatever
  // selectedPath_ has drifted to since (which can't actually change
  // while the modal backdrop is up, but this keeps the two concerns
  // separate regardless).
  std::shared_ptr<bool> deleteDialogOpen_ = std::make_shared<bool>(false);
  std::string deleteTargetPath_;
  bool deleteTargetIsDir_ = false;

  std::shared_ptr<bool> unsavedDialogOpen_ = std::make_shared<bool>(false);
  size_t unsavedDialogDocIndex_ = 0;

  std::shared_ptr<bool> errorDialogOpen_ = std::make_shared<bool>(false);
  std::string errorDialogMessage_;

  void showErrorDialog(std::string message) {
    errorDialogMessage_ = std::move(message);
    *errorDialogOpen_ = true;
  }

  void setWorkspaceRoot(const std::string &p) {
    if (p.empty())
      return;
    workspaceRoot_ = p;
    FileTreeNode root;
    root.fullPath = p;
    root.name = editorTitleFromPath(p);
    root.isDir = true;
    root.expanded = true; // VS Code opens a freshly-added folder expanded
    loadFileTreeChildren(root);
    explorerRoot_ = std::move(root);
    selectedPath_ = p; // root selected by default
  }

  void openWorkspaceFolder() {
    auto picked = openWorkspaceFolderDialog("Open Folder");
    if (picked)
      setWorkspaceRoot(*picked);
  }

  // Directory New File/New Folder should target: the selected folder
  // itself, or the parent directory of the selected file — falling back
  // to the workspace root when nothing is selected. This is what makes
  // "select a folder, hit N" create inside that folder rather than
  // always at the root, matching VS Code.
  std::string selectedDirectory() {
    if (selectedPath_.empty())
      return workspaceRoot_;
    int depth = 0;
    FileTreeNode *node = findExplorerNode(selectedPath_, depth);
    if (!node)
      return workspaceRoot_;
    if (node->isDir)
      return node->fullPath;
    return std::filesystem::path(node->fullPath).parent_path().string();
  }

  static std::string trimWhitespace(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
      return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }

  // Row's displayed indent depth for a given full path — same "shift by
  // one to hide the root row" adjustment buildExplorerRow already does,
  // factored out so the pending-create row can indent one level deeper
  // than whatever folder it's being created in.
  int explorerDepthForPath(const std::string &path) {
    if (path == workspaceRoot_)
      return -1; // so a direct child of the root lands at depth 0
    int depth = 0;
    FileTreeNode *node = findExplorerNode(path, depth);
    if (!node)
      return -1;
    return std::max(0, depth - 1);
  }

  // Stops the input's blink timer (if it ever started — mirrors the same
  // leak-guard closeTab() already applies to a closed CodeEditorState)
  // and clears all pending-create state, discarding whatever was typed.
  void resetPendingCreate() {
    if (pendingInputState_->blinkTimerHandle >= 0) {
      ui_.removeInterval(pendingInputState_->blinkTimerHandle);
      pendingInputState_->blinkTimerHandle = -1;
    }
    pendingInputState_->focused = false;
    pendingCreate_.active = false;
    pendingCreate_.isDir = false;
    pendingCreate_.parentDir.clear();
    pendingCreateEverFocused_ = false;
    pendingInputState_ = std::make_shared<TextInputState>();
  }

  void cancelPendingCreate() { resetPendingCreate(); }

  // Enter handler: trims the typed name and, if anything's left, creates
  // the file/folder; a trimmed-empty name is treated as a no-op rather
  // than an error. Either way the input closes.
  void submitPendingCreate(const std::string &rawText) {
    if (!pendingCreate_.active)
      return;
    std::string name = trimWhitespace(rawText);
    bool isDir = pendingCreate_.isDir;
    std::string dir = pendingCreate_.parentDir;
    resetPendingCreate(); // close the input either way
    if (name.empty())
      return;

    if (isDir) {
      std::error_code ec;
      std::filesystem::path newDir = std::filesystem::path(dir) / name;
      // create_directory's return value is distinct from ec: it returns
      // false with no error set when the directory already exists, which
      // is silently wrong to treat as success here.
      bool created = std::filesystem::create_directory(newDir, ec);
      explorerRefresh();
      if (ec) {
        showErrorDialog("Couldn't create folder \"" + name +
                        "\": " + ec.message());
        return;
      }
      if (!created) {
        showErrorDialog("A file or folder named \"" + name +
                        "\" already exists.");
        return;
      }
      selectedPath_ = newDir.string();
    } else {
      std::string path = (std::filesystem::path(dir) / name).string();
      if (std::filesystem::exists(path)) {
        showErrorDialog("A file or folder named \"" + name +
                        "\" already exists.");
        return;
      }
      std::ofstream out(path, std::ios::binary);
      bool ok = static_cast<bool>(out);
      out.close();
      explorerRefresh();
      if (!ok) {
        showErrorDialog("Couldn't create file \"" + name + "\".");
        return;
      }
      selectedPath_ = path;
      openFile(path);
    }
  }

  // Polled every dispatched event via buildPendingCreateRow's
  // style.disabled hook — checkForUpdates() already re-evaluates every
  // view's Dynamic<bool> disabled field after each click/key, so this
  // rides that instead of needing its own timer. Always returns false;
  // the row is never actually disabled, this is purely a polling hook.
  void checkPendingCreateBlur() {
    if (!pendingCreate_.active)
      return;
    if (pendingInputState_->focused) {
      pendingCreateEverFocused_ = true;
      return;
    }
    // Hasn't been clicked into yet — nothing to blur from.
    if (pendingCreateEverFocused_)
      cancelPendingCreate();
  }

  void beginPendingCreate(bool isDir) {
    if (workspaceRoot_.empty())
      return;
    std::string dir = selectedDirectory();
    // Auto-expand the target folder so the new input row is actually
    // visible where it's being created — matches the old immediate-create
    // behavior of the new item appearing in an already-open directory.
    if (dir != workspaceRoot_) {
      int depth = 0;
      FileTreeNode *node = findExplorerNode(dir, depth);
      if (node && node->isDir) {
        if (!node->childrenLoaded)
          loadFileTreeChildren(*node);
        node->expanded = true;
      }
    }
    resetPendingCreate(); // discard any previous unfinished creation first
    resetPendingRename(); // ...and cancel any in-progress rename too
    pendingCreate_.active = true;
    pendingCreate_.isDir = isDir;
    pendingCreate_.parentDir = dir;
    pendingInputState_->requestFocus = true; 
  }

  void explorerNewFile() { beginPendingCreate(false); }
  void explorerNewFolder() { beginPendingCreate(true); }

  // ---- inline rename ----

  void resetPendingRename() {
    if (pendingRenameInputState_->blinkTimerHandle >= 0) {
      ui_.removeInterval(pendingRenameInputState_->blinkTimerHandle);
      pendingRenameInputState_->blinkTimerHandle = -1;
    }
    pendingRenameInputState_->focused = false;
    pendingRename_.active = false;
    pendingRename_.isDir = false;
    pendingRename_.originalPath.clear();
    pendingRenameEverFocused_ = false;
    pendingRenameInputState_ = std::make_shared<TextInputState>();
  }

  void cancelPendingRename() { resetPendingRename(); }

  // Same polling trick as checkPendingCreateBlur — piggybacks on the
  // generic Dynamic<bool> disabled poll so losing focus (clicking
  // elsewhere) cancels the rename.
  void checkPendingRenameBlur() {
    if (!pendingRename_.active)
      return;
    if (pendingRenameInputState_->focused) {
      pendingRenameEverFocused_ = true;
      return;
    }
    if (pendingRenameEverFocused_)
      cancelPendingRename();
  }

  void beginPendingRename(const std::string &path, bool isDir) {
    resetPendingCreate(); // discard any pending N/F creation first
    resetPendingRename();
    pendingRename_.active = true;
    pendingRename_.isDir = isDir;
    pendingRename_.originalPath = path;
    pendingRenameInputState_->requestFocus = true;
  }

  // Updates any open tab(s) whose backing path sits at or under oldPath so
  // a rename doesn't leave a tab silently pointing at a now-missing file —
  // same reasoning/shape as closeTabsUnderPath's use of pathIsUnderOrEqual.
  void renamePathInTabs(const std::string &oldPath, const std::string &newPath,
                        bool isDir) {
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath)
        continue;
      if (!isDir) {
        if (doc.path != oldPath)
          continue;
        doc.path = newPath;
      } else {
        if (!pathIsUnderOrEqual(doc.path, oldPath))
          continue;
        doc.path = newPath + doc.path.substr(oldPath.size());
      }
      doc.title = editorTitleFromPath(doc.path);
      doc.highlighter = makeHighlighter(languageForExtension(doc.path));
    }
  }

  // Enter handler for the rename TextInput: trims the typed name and, if
  // anything's left and it actually differs from the current name,
  // renames on disk, fixes up any open tabs, and refreshes the tree.
  void submitPendingRename(const std::string &rawText) {
    if (!pendingRename_.active)
      return;
    std::string name = trimWhitespace(rawText);
    std::string oldPath = pendingRename_.originalPath;
    bool isDir = pendingRename_.isDir;
    resetPendingRename(); // close the input either way
    if (name.empty())
      return;
    std::string newPath =
        (std::filesystem::path(oldPath).parent_path() / name).string();
    if (newPath == oldPath)
      return;
    if (std::filesystem::exists(newPath)) {
      showErrorDialog("A file or folder named \"" + name +
                      "\" already exists.");
      return;
    }
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) {
      showErrorDialog("Couldn't rename \"" + editorTitleFromPath(oldPath) +
                      "\" to \"" + name + "\": " + ec.message());
      return;
    }
    renamePathInTabs(oldPath, newPath, isDir);
    explorerRefresh();
    selectedPath_ = newPath;
  }

  // ---- right-click context menu ----

  void openExplorerContextMenu(const std::string &path, bool isDir, float x,
                               float y) {
    explorerMenuTargetPath_ = path;
    explorerMenuTargetIsDir_ = isDir;
    *explorerMenuX_ = x;
    *explorerMenuY_ = y;
    *explorerMenuOpen_ = true;
  }
  void closeExplorerContextMenu() { *explorerMenuOpen_ = false; }

  // Each action reuses the same logic the toolbar buttons/keyboard
  // shortcut already use, just pointed at whatever was right-clicked
  // (selectedPath_ drives selectedDirectory()/beginDeleteConfirm() already,
  // so setting it first is enough — no new targeting logic needed).
  void explorerContextOpen() {
    if (!explorerMenuTargetIsDir_)
      openFile(explorerMenuTargetPath_);
  }
  void explorerContextRename() {
    beginPendingRename(explorerMenuTargetPath_, explorerMenuTargetIsDir_);
  }
  void explorerContextDelete() {
    selectedPath_ = explorerMenuTargetPath_;
    beginDeleteConfirm();
  }
  void explorerContextNewFile() {
    selectedPath_ = explorerMenuTargetPath_;
    explorerNewFile();
  }
  void explorerContextNewFolder() {
    selectedPath_ = explorerMenuTargetPath_;
    explorerNewFolder();
  }

  // Whether `path` is `ancestor` itself or lives somewhere underneath it —
  // used to find tabs open on files inside a folder that's about to be
  // deleted, and to tell whether the current selection just got deleted.
  static bool pathIsUnderOrEqual(const std::string &path,
                                 const std::string &ancestor) {
    if (path == ancestor)
      return true;
    std::string prefix = ancestor;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
      prefix += static_cast<char>(std::filesystem::path::preferred_separator);
    return path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0;
  }

  // Closes every open tab whose backing file sits at or under `path`,
  // before the actual filesystem delete runs — otherwise a deleted
  // document would keep its tab open pointing at a now-missing file.
  void closeTabsUnderPath(const std::string &path) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed && docs_[i].hasPath &&
          pathIsUnderOrEqual(docs_[i].path, path))
        closeTab(i);
  }

  // Opens the confirmation dialog for whatever's currently selected.
  // No-op if nothing is selected, the workspace root itself is selected
  // (deleting the open folder from under itself isn't supported here),
  // or the dialog is already open.
  void beginDeleteConfirm() {
    if (*deleteDialogOpen_ || selectedPath_.empty() ||
        selectedPath_ == workspaceRoot_)
      return;
    int depth = 0;
    FileTreeNode *node = findExplorerNode(selectedPath_, depth);
    deleteTargetPath_ = selectedPath_;
    deleteTargetIsDir_ = node && node->isDir;
    *deleteDialogOpen_ = true;
  }

  void cancelDeleteConfirm() {
    *deleteDialogOpen_ = false;
    deleteTargetPath_.clear();
    deleteTargetIsDir_ = false;
  }

  // Actually deletes deleteTargetPath_ (recursively, if it's a folder),
  // closing any tabs open under it first and refreshing the tree
  // afterward. Falls back to selecting the workspace root if the
  // deleted path was (or contained) the current selection.
  void confirmDelete() {
    if (deleteTargetPath_.empty()) {
      cancelDeleteConfirm();
      return;
    }
    std::string path = deleteTargetPath_;
    bool isDir = deleteTargetIsDir_;
    cancelDeleteConfirm();

    closeTabsUnderPath(path);

    std::error_code ec;
    if (isDir)
      std::filesystem::remove_all(path, ec);
    else
      std::filesystem::remove(path, ec);

    explorerRefresh();
    if (pathIsUnderOrEqual(selectedPath_, path))
      selectedPath_ = workspaceRoot_;

    if (ec)
      showErrorDialog("Couldn't delete \"" + editorTitleFromPath(path) +
                      "\": " + ec.message());
  }

  // Re-scans a directory node from disk, but only if it was already
  // expanded/loaded — untouched (collapsed, lazy) subtrees are left alone.
  // Expand state is carried over for children that still exist after the
  // rescan, so refreshing doesn't visually collapse everything.
  static void refreshNode(FileTreeNode &node) {
    if (!node.isDir)
      return;
    bool wasLoaded = node.childrenLoaded;
    bool wasExpanded = node.expanded;
    std::vector<FileTreeNode> old = std::move(node.children);
    node.children.clear();
    node.childrenLoaded = false;
    if (wasLoaded) {
      loadFileTreeChildren(node);
      for (auto &c : node.children)
        for (auto &o : old)
          if (o.isDir && c.isDir && o.fullPath == c.fullPath) {
            c.expanded = o.expanded;
            c.childrenLoaded = o.childrenLoaded;
            c.children = std::move(o.children);
            break;
          }
      for (auto &c : node.children)
        if (c.isDir && c.expanded)
          refreshNode(c);
    }
    node.expanded = wasExpanded;
  }

  void explorerRefresh() {
    if (!explorerRoot_)
      return;
    refreshNode(*explorerRoot_);
  }

  static void collapseAllNode(FileTreeNode &node) {
    if (!node.isDir)
      return;
    node.expanded = false;
    for (auto &c : node.children)
      collapseAllNode(c);
  }

  void explorerCollapseAll() {
    if (!explorerRoot_)
      return;
    // Collapse every descendant folder; leave the workspace root itself
    // expanded, matching VS Code's "Collapse All".
    for (auto &c : explorerRoot_->children)
      collapseAllNode(c);
  }

  // DFS by full path from explorerRoot_, reporting the match's depth
  // (root = 0) via outDepth so buildExplorerRow can indent without
  // tracking depth itself. Returns a non-owning pointer straight into
  // explorerRoot_'s tree, so the caller can flip ->expanded on it.
  static FileTreeNode *findExplorerNodeIn(FileTreeNode &node,
                                          const std::string &path, int depth,
                                          int &outDepth) {
    if (node.fullPath == path) {
      outDepth = depth;
      return &node;
    }
    for (auto &c : node.children)
      if (FileTreeNode *hit = findExplorerNodeIn(c, path, depth + 1, outDepth))
        return hit;
    return nullptr;
  }
  FileTreeNode *findExplorerNode(const std::string &path, int &outDepth) {
    if (!explorerRoot_)
      return nullptr;
    return findExplorerNodeIn(*explorerRoot_, path, 0, outDepth);
  }

  // When a creation is pending inside a given folder, kPendingCreateKey is
  // spliced in as that folder's first "child" key, so the input row always
  // renders as the top item of the folder it's being created in.
  // Root-level creations are handled the same way in explorerKeys() below,
  // since the root itself never gets a collectExplorerKeys() call of its
  // own.
  void collectExplorerKeys(const FileTreeNode &node,
                           std::vector<std::string> &out) const {
    bool renamingThis =
        pendingRename_.active && pendingRename_.originalPath == node.fullPath;
    out.push_back(renamingThis
                      ? std::string(kPendingRenameKeyPrefix) + node.fullPath
                      : node.fullPath);
    if (node.isDir && node.expanded) {
      if (pendingCreate_.active && pendingCreate_.parentDir == node.fullPath)
        out.push_back(kPendingCreateKey);
      for (const auto &c : node.children)
        collectExplorerKeys(c, out);
    }
  }
  std::vector<std::string> explorerKeys() const {
    std::vector<std::string> keys;
    if (!explorerRoot_)
      return keys;
    if (pendingCreate_.active && pendingCreate_.parentDir == workspaceRoot_)
      keys.push_back(kPendingCreateKey);
    // The root folder itself is never shown as a row — only its
    // contents — so start recursion at its children instead of at the
    // root node.
    for (const auto &c : explorerRoot_->children)
      collectExplorerKeys(c, keys);
    return keys;
  }

  EditorDocument *activeDoc() {
    if (*activeIndex_ < 0 || static_cast<size_t>(*activeIndex_) >= docs_.size())
      return nullptr;
    return &docs_[static_cast<size_t>(*activeIndex_)];
  }

  static void splitLinesInto(const std::string &text,
                             std::vector<std::string> &out) {
    out.clear();
    std::string cur;
    auto pushLine = [&] {
      if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
      out.push_back(cur);
      cur.clear();
    };
    for (char c : text) {
      if (c == '\n')
        pushLine();
      else
        cur += c;
    }
    pushLine();
    if (out.empty())
      out.push_back(std::string());
  }

  // One row in the explorer tree. `key` is a node's full path, looked up
  // fresh on every build (rather than captured by value) so a row that
  // reconcileChildren reuses across an unrelated expand/collapse
  // elsewhere in the tree still reflects its own current depth/expanded
  // state and the active tab's highlight correctly.
  View buildExplorerRow(const std::string &key) {
    int depth = 0;
    FileTreeNode *node = findExplorerNode(key, depth);
    // Root itself is never shown as a row (see explorerKeys), so shift
    // every visible depth up by one to compensate — a top-level file/
    // folder should render at indent 0, not 1.
    depth = std::max(0, depth - 1);
    bool isDir = node && node->isDir;
    bool expanded = node && node->expanded;
    std::string name = node ? node->name : key;
    const EditorDocument *active = activeDocument();
    bool isActive = !isDir && active && active->hasPath && active->path == key;
    bool isSelected = (key == selectedPath_);

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};

    // was: computed once from isSelected/isActive captured at build time.
    // now: recomputed on every checkForUpdates() poll, so a click that only
    // changes selectedPath_ (with no key-list change) still repaints this row.
    row.style.backgroundColor = [this, key, isDir]() -> Color {
      const EditorDocument *active = activeDocument();
      bool isActive =
          !isDir && active && active->hasPath && active->path == key;
      bool isSelected = (key == selectedPath_);
      return isSelected ? th::kSelectedBg
             : isActive ? th::kActiveItemBg
                        : th::kSideBarBg;
    };
    row.style.hoverColor = th::kHoverBg;
    row.onClick = [this, key] {
      selectedPath_ = key;
      int d = 0;
      FileTreeNode *n = findExplorerNode(key, d);
      if (!n)
        return;
      if (n->isDir) {
        if (!n->childrenLoaded)
          loadFileTreeChildren(*n);
        n->expanded = !n->expanded;
      } else {
        openFile(key);
      }
    };

    // Captured via onLayout (same pattern buildMenuBar's own triggers use)
    // so the right-click handler below can turn a local press point into
    // a window-absolute position for the context menu.
    auto rowX = std::make_shared<float>(0.0f);
    auto rowY = std::make_shared<float>(0.0f);
    row.onLayout = [rowX, rowY](float x, float y, float, float) {
      *rowX = x;
      *rowY = y;
    };
    row.onRightPressAt = [this, key, isDir, rowX, rowY](float lx, float ly) {
      selectedPath_ = key;
      openExplorerContextMenu(key, isDir, *rowX + lx, *rowY + ly);
    };

    Text chevron;
    chevron.label = isDir ? (expanded ? std::string("\xE2\x96\xBE ")  // ▾
                                      : std::string("\xE2\x96\xB8 ")) // ▸
                          : std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = th::kTextDim;
    row.addChild(chevron);

    Text label;
    label.label = name;
    label.fontSize = 13;
    label.color = th::kText;
    row.addChild(label);
    return row;
  }

  // The inline TextInput row shown while a creation is pending — sits at
  // whatever depth is one deeper than its parent folder's own rows.
  View buildPendingCreateRow() {
    bool isDir = pendingCreate_.isDir;
    int depth = explorerDepthForPath(pendingCreate_.parentDir) + 1;

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};
    row.style.backgroundColor = th::kSideBarBg;
    // Piggybacks on the generic Dynamic<bool> polling checkForUpdates()
    // already does for every view's `disabled` field — re-evaluated
    // after every dispatched event — purely to detect focus loss on the
    // TextInput below. The row is never actually disabled; this always
    // returns false.
    row.disabled = [this]() -> bool {
      checkPendingCreateBlur();
      return false;
    };

    Text chevron;
    chevron.label = std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = th::kTextDim;
    row.addChild(chevron);

    TextInput input;
    input.style.flexGrow = 1;
    input.style.height = Size::pixel(20);
    input.fontSize = 13;
    input.placeholder = isDir ? "Folder name" : "File name";
    input.textColor = th::kText;
    input.borderColor = th::kDivider;
    input.focusedBorderColor = th::kAccent;
    input.style.backgroundColor = th::kInputBg;
    input.style.borderWidth = 1.0f;
    input.placeholderColor = th::kPlaceholder;
    input.caretColor = th::kCaret;
    input.leftPadding = 4.0f;
    input.onSubmit = [this](const std::string &text) {
      submitPendingCreate(text);
    };
    input.state = pendingInputState_;
    row.addChild(input);
    return row;
  }

  // Inline rename row: replaces an existing row's content (via the
  // rename-sentinel key) with a TextInput pre-filled with the item's
  // current name, at the same indent depth the row already had.
  View buildPendingRenameRow(const std::string &originalPath) {
    bool isDir = pendingRename_.isDir;
    int depth = explorerDepthForPath(originalPath);
    int d = 0;
    FileTreeNode *node = findExplorerNode(originalPath, d);
    bool expanded = node && node->expanded;

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};
    row.style.backgroundColor = th::kSideBarBg;
    row.disabled = [this]() -> bool {
      checkPendingRenameBlur();
      return false;
    };

    Text chevron;
    chevron.label = isDir ? (expanded ? std::string("\xE2\x96\xBE ")  // ▾
                                      : std::string("\xE2\x96\xB8 ")) // ▸
                          : std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = th::kTextDim;
    row.addChild(chevron);

    TextInput input;
    input.style.flexGrow = 1;
    input.style.height = Size::pixel(20);
    input.fontSize = 13;
    input.text =
        editorTitleFromPath(originalPath); // prefilled with current name
    input.textColor = th::kText;
    input.borderColor = th::kDivider;
    input.focusedBorderColor = th::kAccent;
    input.style.backgroundColor = th::kInputBg;
    input.style.borderWidth = 1.0f;
    input.placeholderColor = th::kPlaceholder;
    input.caretColor = th::kCaret;
    input.leftPadding = 4.0f;
    input.onSubmit = [this](const std::string &text) {
      submitPendingRename(text);
    };
    input.state = pendingRenameInputState_;
    row.addChild(input);
    return row;
  }

  // Row shown just above the file tree, next to the folder name: New File
  // (N), New Folder (F), Refresh (R), Collapse All (C). Text labels for
  // now instead of icons. Hidden entirely until a workspace folder is open
  // (matches openBtn's own visibility condition, just inverted).
  View buildExplorerHeaderRow() {
    View row;
    row.style.direction = FlexDirection::Row;
    row.style.alignItems = Align::Center;
    row.style.width = Size::full();
    row.style.padding = EdgeInsets{4, 8, 4, 12};
    row.style.gap = 2;
    // Highlighted the same way a selected child row is, whenever the
    // root itself is the current selection (its default state).
    row.style.backgroundColor = [this]() -> Color {
      return selectedPath_ == workspaceRoot_ ? th::kSelectedBg : th::kSideBarBg;
    };
    row.style.hoverColor = th::kHoverBg;
    row.onClick = [this] { selectedPath_ = workspaceRoot_; };
    row.style.display = [this]() -> Display {
      return workspaceRoot_.empty() ? Display::None : Display::Flex;
    };

    Text nameLabel;
    nameLabel.label = std::function<std::string()>([this]() -> std::string {
      std::string name = editorTitleFromPath(workspaceRoot_);
      for (auto &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return name;
    });
    nameLabel.fontSize = 11;
    nameLabel.fontWeight = FontWeight::SemiBold;
    nameLabel.color = th::kTextDim;
    nameLabel.style.flexGrow = 1;
    row.addChild(nameLabel);

    auto makeActionBtn = [](const std::string &label,
                            const std::string &tooltip,
                            std::function<void()> onClick) {
      View btn;
      btn.style.width = Size::pixel(20);
      btn.style.height = Size::pixel(20);
      btn.style.justifyContent = Justify::Center;
      btn.style.alignItems = Align::Center;
      btn.style.borderRadius = 3.0f;
      btn.style.hoverColor = th::kButtonHoverBg;
      btn.style.backgroundColor = Color{0, 0, 0, 0};
      btn.tooltip = tooltip;
      btn.onClick = std::move(onClick);
      Text t;
      t.label = label;
      t.fontSize = 11;
      t.fontWeight = FontWeight::SemiBold;
      t.color = th::kTextMuted;
      btn.addChild(t);
      return btn;
    };

    row.addChild(makeActionBtn("N", "New File", [this] { explorerNewFile(); }));
    row.addChild(
        makeActionBtn("F", "New Folder", [this] { explorerNewFolder(); }));
    row.addChild(makeActionBtn("R", "Refresh", [this] { explorerRefresh(); }));
    row.addChild(
        makeActionBtn("C", "Collapse All", [this] { explorerCollapseAll(); }));
    return row;
  }

  View buildExplorerPane() {
    auto act = activityIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.width = Size::full();
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = th::kSideBarBg;
    pane.style.display = [act]() -> Display {
      return *act == kExplorerActivityId ? Display::Flex : Display::None;
    };

    View openBtn;
    openBtn.style.width = Size::full();
    openBtn.style.padding = EdgeInsets{6, 12, 6, 12};
    openBtn.style.alignItems = Align::Center;
    openBtn.style.backgroundColor = th::kSideBarBg;
    openBtn.style.hoverColor = th::kHoverBg;
    openBtn.style.display = [this]() -> Display {
      return workspaceRoot_.empty() ? Display::Flex : Display::None;
    };
    openBtn.onClick = [this] { openWorkspaceFolder(); };
    Text openLabel;
    openLabel.label = std::string("Open Folder...");
    openLabel.fontSize = 13;
    openLabel.color = th::kText;
    openBtn.addChild(openLabel);
    pane.addChild(std::move(openBtn));
    pane.addChild(buildExplorerHeaderRow());

    // Keyed the same way the tab strip is: navigating into a folder
    // changes the key list, and only the rows that actually differ get
    // built. Scrolls on its own so a big directory doesn't stretch the
    // panel past the window.
    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.backgroundColor = th::kSideBarBg;
    list.style.overflowY = Overflow::Auto;
    list.keysSource = [this] { return explorerKeys(); };
    list.itemBuilder = [this](const std::string &key) {
      if (key == kPendingCreateKey)
        return buildPendingCreateRow();
      if (pendingRename_.active && key == std::string(kPendingRenameKeyPrefix) +
                                              pendingRename_.originalPath)
        return buildPendingRenameRow(pendingRename_.originalPath);
      return buildExplorerRow(key);
    };
    pane.addChild(std::move(list));
    return pane;
  }

  View buildSearchPane() {
    auto act = activityIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.width = Size::full();
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = th::kSideBarBg;
    pane.style.display = [act]() -> Display {
      return *act == kSearchActivityId ? Display::Flex : Display::None;
    };

    View inputRow;
    inputRow.style.width = Size::full();
    inputRow.style.padding = EdgeInsets{8, 12, 4, 12};
    inputRow.style.flexShrink = 0;
    inputRow.style.backgroundColor = th::kSideBarBg;

    TextInput input;
    input.style.width = Size::full();
    input.style.height = Size::pixel(28);
    input.style.flexShrink = 0;
    input.fontSize = 13;
    input.placeholder = "Search";
    input.textColor = th::kText;
    input.borderColor = th::kDivider;
    input.focusedBorderColor = th::kAccent;
    input.style.backgroundColor = th::kInputBg;
    input.style.borderWidth = 1.0f;
    input.placeholderColor = th::kPlaceholder;
    input.caretColor = th::kCaret;
    input.leftPadding = 6.0f;
    input.state = searchInputState_;
    input.onChange = [this](const std::string &text) {
      runWorkspaceSearch(text);
    };
    inputRow.addChild(input);
    pane.addChild(std::move(inputRow));

    View replaceRow;
    replaceRow.style.direction = FlexDirection::Row;
    replaceRow.style.width = Size::full();
    replaceRow.style.padding = EdgeInsets{0, 12, 4, 12};
    replaceRow.style.flexShrink = 0; // same reasoning as inputRow: never
                                     // let the panel's shrink math eat this
    replaceRow.style.gap = 6;
    replaceRow.style.alignItems = Align::Center;
    replaceRow.style.backgroundColor = th::kSideBarBg;

    TextInput replaceInput;
    replaceInput.style.flexGrow = 1;
    replaceInput.style.height = Size::pixel(28);
    replaceInput.style.flexShrink = 0;
    replaceInput.fontSize = 13;
    replaceInput.placeholder = "Replace";
    replaceInput.textColor = th::kText;
    replaceInput.borderColor = th::kDivider;
    replaceInput.focusedBorderColor = th::kAccent;
    replaceInput.leftPadding = 6.0f;
    replaceInput.state = replaceInputState_;
    replaceInput.onChange = [this](const std::string &text) {
      workspaceReplaceText_ = text;
    };
    replaceRow.addChild(std::move(replaceInput));

    View replaceAllBtn;
    replaceAllBtn.style.height = Size::pixel(28);
    replaceAllBtn.style.flexShrink = 0;
    replaceAllBtn.style.padding = EdgeInsets{0, 10, 0, 10};
    replaceAllBtn.style.justifyContent = Justify::Center;
    replaceAllBtn.style.alignItems = Align::Center;
    replaceAllBtn.style.borderRadius = 3.0f;
    replaceAllBtn.style.backgroundColor = th::kButtonBg;
    replaceAllBtn.style.hoverColor = th::kButtonHoverBg;

    replaceAllBtn.tooltip = "Replace all current matches";
    replaceAllBtn.onClick = [this] { replaceAllMatches(); };
    Text replaceAllLabel;
    replaceAllLabel.label = std::string("Replace All");
    replaceAllLabel.fontSize = 12;
    replaceAllLabel.color = th::kText;
    replaceAllBtn.addChild(replaceAllLabel);
    replaceRow.addChild(std::move(replaceAllBtn));

    pane.addChild(std::move(replaceRow));

    Text hintOrCount;
    hintOrCount.label = std::function<std::string()>([this]() -> std::string {
      if (workspaceRoot_.empty())
        return "Open a folder to search its files.";
      if (workspaceSearchQuery_.empty())
        return "";
      return std::to_string(workspaceSearchResults_.size()) +
             (workspaceSearchResults_.size() >= kMaxSearchResults
                  ? "+ results"
                  : (workspaceSearchResults_.size() == 1 ? " result"
                                                         : " results"));
    });
    hintOrCount.style.padding = EdgeInsets{0, 12, 6, 12};
    hintOrCount.style.flexShrink = 0;
    hintOrCount.fontSize = 11;
    hintOrCount.color = th::kTextDim;
    pane.addChild(hintOrCount);

    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.overflowY = Overflow::Auto;
    list.style.backgroundColor = th::kSideBarBg;
    // Fixed pool, not a keyed list — see buildSearchResultSlot's comment
    // for why. Hidden slots (idx >= current result count) cost nothing:
    // measureNatural() skips Display::None children entirely, so an
    // empty or short result set doesn't pay for the unused rows.
    for (size_t i = 0; i < kMaxSearchResults; ++i)
      list.addChild(buildSearchResultSlot(i));
    pane.addChild(std::move(list));
    return pane;
  }

  // The expanding half of the sidebar. Hidden entirely (Display::None, so
  // it reserves no space) while no activity item is selected.
  View buildSidePanel() {
    auto act = activityIndex_;

    View panel;
    panel.style.direction = FlexDirection::Column;
    {
      auto widthPtr = sidePanelWidth_;
      panel.style.width = [widthPtr]() -> Size {
        return Size::pixel(*widthPtr);
      };
    }
    panel.style.height = Size::full();
    panel.style.backgroundColor = th::kSideBarBg;
    panel.style.padding = EdgeInsets{8, 0, 8, 0};
    panel.style.gap = 2;

    panel.style.display = [act]() -> Display {
      return *act >= 0 ? Display::Flex : Display::None;
    };

    Text header;
    header.label = std::function<std::string()>([act]() -> std::string {
      for (const auto &activity : activityItems())
        if (activity.id == *act) {
          std::string name = activity.tooltip;
          for (auto &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          return name;
        }
      return "";
    });
    header.style.padding = EdgeInsets{10, 12, 6, 12};
    header.style.width = Size::full();
    header.style.flexShrink = 0;
    header.fontSize = 11;
    header.color = th::kTextDim;
    panel.addChild(header);

    panel.addChild(buildExplorerPane());
    panel.addChild(buildSearchPane());
    return panel;
  }

  // Drag handle between the side panel and the editor area. Collapses to
  // 0 width alongside the panel itself (same activityIndex_ check) so it's
  // neither visible nor hit-testable while the panel is closed — matching
  // vDivider's behavior in the plain liteui example.
  View buildSideDivider() {
    auto act = activityIndex_;
    auto widthPtr = sidePanelWidth_;

    View divider;
    divider.style.height = Size::full();
    divider.style.flexShrink = 0;
    divider.style.backgroundColor = th::kDivider;
    divider.style.hoverColor = th::kAccent;

    divider.style.width = [act]() -> Size {
      return Size::pixel(*act >= 0 ? kSideDividerWidth : 0.0f);
    };

    // Same self-centering pattern as main.cpp's updateSidebarWidth: each
    // onPressAt/onDragTo call reports the pointer's position local to the
    // divider, and a relayout runs after every one, so nudging the width
    // by (localX - half the divider's own width) converges to a normal
    // drag-to-resize even though only local coordinates are available.
    auto updateWidth = [widthPtr](float localX) {
      float next = *widthPtr + localX - kSideDividerWidth / 2.0f;
      *widthPtr = std::clamp(next, kMinSidePanelWidth, kMaxSidePanelWidth);
    };
    divider.onPressAt = [updateWidth](float localX, float) {
      updateWidth(localX);
    };
    divider.onDragTo = [updateWidth](float localX, float) {
      updateWidth(localX);
    };
    return divider;
  }
  // Drag handle between the editor stack and the terminal panel. Always
  // visible/draggable (the terminal itself has no open/close toggle here,
  // matching the plain liteui example's rightColumn layout) — see
  // buildTerminalPanel() below for the panel it resizes.
  View buildHDivider() {
    auto heightPtr = terminalHeight_;

    View divider;
    divider.style.width = Size::full();
    divider.style.flexGrow = 0;
    divider.style.flexShrink = 0;
    divider.style.height = Size::pixel(kHDividerHeight);
    divider.style.backgroundColor = th::kDivider;
    divider.style.hoverColor = th::kAccent;

    // Same self-centering drag pattern as buildSideDivider()/main.cpp's
    // updateTerminalHeight, just on the vertical axis: the terminal grows
    // upward as the pointer moves down, hence the subtraction.
    auto updateHeight = [heightPtr](float localY) {
      float next = *heightPtr - (localY - kHDividerHeight / 2.0f);
      *heightPtr = std::clamp(next, kMinTerminalHeight, kMaxTerminalHeight);
    };
    divider.onPressAt = [updateHeight](float, float localY) {
      updateHeight(localY);
    };
    divider.onDragTo = [updateHeight](float, float localY) {
      updateHeight(localY);
    };
    return divider;
  }

  // Spawns a new terminal tab: home directory if no workspace folder is
  // open, the workspace root otherwise (spawn()'s startDir already
  // treats an empty string as "use the home directory" — see
  // liteui_terminal.hpp — so passing workspaceRoot_ straight through
  // covers both cases without a branch here).
  void spawnNewTerminal() {
    TerminalTab tab;
    tab.state->spawn(80, 24, workspaceRoot_);
    terminals_.push_back(std::move(tab));
    *activeTerminalIndex_ = static_cast<int>(terminals_.size()) - 1;
  }

  // Closes terminal `idx`: shuts down its PTY/reader thread, reassigns
  // the active index the same way closeTab() reassigns activeIndex_, and
  // — since this panel always expects at least one terminal, unlike the
  // editor's welcome-tab fallback being optional — spawns a fresh one if
  // that was the last one open.
  void closeTerminal(size_t idx) {
    if (idx >= terminals_.size() || terminals_[idx].closed)
      return;
    terminals_[idx].closed = true;
    terminals_[idx].state->shutdown();

    if (static_cast<size_t>(*activeTerminalIndex_) == idx) {
      int next = -1;
      for (int k = static_cast<int>(idx) - 1; k >= 0; --k)
        if (!terminals_[static_cast<size_t>(k)].closed) {
          next = k;
          break;
        }
      if (next < 0)
        for (size_t k = idx + 1; k < terminals_.size(); ++k)
          if (!terminals_[k].closed) {
            next = static_cast<int>(k);
            break;
          }
      *activeTerminalIndex_ = next;
    }
    if (std::all_of(terminals_.begin(), terminals_.end(),
                    [](const TerminalTab &t) { return t.closed; }))
      spawnNewTerminal(); // keeps the panel non-empty; also fixes up
                          // activeTerminalIndex_ to the new one
  }

  std::vector<std::string> terminalKeys() const {
    std::vector<std::string> keys;
    keys.reserve(terminals_.size());
    for (size_t i = 0; i < terminals_.size(); ++i)
      if (!terminals_[i].closed)
        keys.push_back(std::to_string(i));
    return keys;
  }
  static size_t terminalKeyToIndex(const std::string &key) {
    return static_cast<size_t>(std::stoul(key));
  }

  // One terminal's canvas, kept mounted and hidden via Display::None
  // when it isn't the active one — same pattern as the editor's own
  // per-document CodeEditor stack, so switching terminals never tears
  // down or rebuilds a live PTY.
  View buildTerminalOutput(size_t idx) {
    auto activeIdx = activeTerminalIndex_;
    liteui_terminal::Terminal shell;
    shell.state = terminals_[idx].state;
    shell.fontSize = 13.0f;
    shell.style.width = Size::full();
    shell.style.height = Size::full();
    shell.style.padding = EdgeInsets{4, 10, 6, 10};
    shell.style.display = [activeIdx, idx]() -> Display {
      return static_cast<size_t>(*activeIdx) == idx ? Display::Flex
                                                    : Display::None;
    };
    return liteui_terminal::toTerminalView(std::move(shell));
  }

  View buildTerminalOutputStack() {
    View stack;
    stack.style.width = Size::full();
    stack.style.flexGrow = 1;
    stack.style.backgroundColor = liteui_terminal::kDefaultBg;
    stack.keysSource = [this] { return terminalKeys(); };
    stack.itemBuilder = [this](const std::string &key) {
      return buildTerminalOutput(terminalKeyToIndex(key));
    };
    return stack;
  }

  View buildTerminalRow(size_t idx) {
    auto activeIdx = activeTerminalIndex_;

    View row;
    row.style.direction = FlexDirection::Row;
    row.style.alignItems = Align::Center;
    row.style.width = Size::full();
    row.style.padding = EdgeInsets{6, 8, 6, 12};
    row.style.gap = 6;
    row.style.backgroundColor = [activeIdx, idx]() -> Color {
      return static_cast<size_t>(*activeIdx) == idx ? Color{55, 55, 55}
                                                    : Color{30, 30, 30};
    };
    row.style.hoverColor = Color{45, 45, 45};
    row.onClick = [activeIdx, idx] { *activeIdx = static_cast<int>(idx); };

    Text label;
    label.label = std::function<std::string()>([this, idx]() -> std::string {
      return idx < terminals_.size() ? terminals_[idx].label : std::string();
    });
    label.fontSize = 13;
    label.color = Color{210, 210, 210};
    label.style.flexGrow = 1;
    row.addChild(label);

    View closeBtn;
    closeBtn.style.width = Size::pixel(18);
    closeBtn.style.height = Size::pixel(18);
    closeBtn.style.justifyContent = Justify::Center;
    closeBtn.style.alignItems = Align::Center;
    closeBtn.style.borderRadius = 3.0f;
    closeBtn.style.hoverColor = Color{70, 70, 70};
    closeBtn.style.backgroundColor = Color{0, 0, 0, 0};
    closeBtn.onClick = [this, idx] { closeTerminal(idx); };
    Text closeLabel;
    closeLabel.label = std::string("x");
    closeLabel.fontSize = 12;
    closeLabel.color = Color{150, 150, 150};
    closeBtn.addChild(closeLabel);
    row.addChild(closeBtn);
    return row;
  }

  // Right-hand column: a "+" header (New Terminal) above the keyed list
  // of terminal rows — the split VS Code itself uses.
  View buildTerminalListPanel() {
    View panel;
    panel.style.direction = FlexDirection::Column;
    panel.style.width = Size::pixel(180.0f);
    panel.style.flexShrink = 0;
    panel.style.height = Size::full();
    panel.style.backgroundColor = liteui_terminal::kDefaultBg;
    panel.style.borderWidth = 1.0f;
    panel.style.borderColor = Color{50, 50, 50};

    View header;
    header.style.direction = FlexDirection::Row;
    header.style.width = Size::full();
    header.style.height = Size::pixel(30);
    header.style.alignItems = Align::Center;
    header.style.justifyContent = Justify::End;
    header.style.padding = EdgeInsets{0, 8, 0, 8};
    header.style.backgroundColor = liteui_terminal::kDefaultBg;
    header.style.flexShrink = 0;

    View addBtn;
    addBtn.style.width = Size::pixel(22);
    addBtn.style.height = Size::pixel(22);
    addBtn.style.justifyContent = Justify::Center;
    addBtn.style.alignItems = Align::Center;
    addBtn.style.borderRadius = 3.0f;
    addBtn.style.hoverColor = Color{60, 60, 60};
    addBtn.style.backgroundColor = Color{0, 0, 0, 0};
    addBtn.tooltip = "New Terminal";
    addBtn.onClick = [this] { spawnNewTerminal(); };
    Text plus;
    plus.label = std::string("+");
    plus.fontSize = 16;
    plus.color = Color{200, 200, 200};
    addBtn.addChild(plus);
    header.addChild(addBtn);
    panel.addChild(std::move(header));

    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.overflowY = Overflow::Auto;
    list.style.backgroundColor = liteui_terminal::kDefaultBg;
    list.keysSource = [this] { return terminalKeys(); };
    list.itemBuilder = [this](const std::string &key) {
      return buildTerminalRow(terminalKeyToIndex(key));
    };
    panel.addChild(std::move(list));
    return panel;
  }

  View buildTerminalPanel() {
    auto heightPtr = terminalHeight_;

    View terminal;
    terminal.style.width = Size::full();
    terminal.style.flexGrow = 0;
    terminal.style.flexShrink = 0;
    terminal.style.direction = FlexDirection::Column;
    terminal.style.backgroundColor = liteui_terminal::kDefaultBg;
    terminal.style.overflowY = Overflow::Hidden;
    terminal.style.height = [heightPtr]() -> Size {
      return Size::pixel(*heightPtr);
    };

    Text title;
    title.label = std::string("TERMINAL");
    title.fontSize = 12;
    title.fontWeight = FontWeight::SemiBold;
    title.color = Color{180, 180, 180};
    title.style.padding = EdgeInsets{6, 10, 4, 10};
    title.style.flexShrink = 0;
    terminal.addChild(title);

    // Active terminal's output on the left, the terminal list (with its
    // own "+") on the right — the same row-split VS Code itself uses.
    View body;
    body.style.direction = FlexDirection::Row;
    body.style.width = Size::full();
    body.style.flexGrow = 1;
    body.style.backgroundColor = liteui_terminal::kDefaultBg;
    body.addChild(buildTerminalOutputStack());
    body.addChild(buildTerminalListPanel());
    terminal.addChild(std::move(body));
    return terminal;
  }

  // ---- activity bar: always visible, one row per sidebar view. Text
  // labels rather than icons for now, so the column is wide enough to
  // read them. Clicking the selected item again collapses the panel,
  // matching VS Code.
  View buildActivityBar() {
    auto act = activityIndex_;

    const auto &activities = activityItems();

    View bar;
    bar.style.direction = FlexDirection::Column;
    bar.style.width = Size::pixel(44);
    bar.style.flexShrink = 0;
    bar.style.height = Size::full();
    bar.style.backgroundColor = Color{55, 55, 60};
    bar.style.padding = EdgeInsets{0, 0, 0, 8};
    bar.style.gap = 2;

    for (const auto &activity : activities) {
      int idx = activity.id;
      View row;
      row.style.width = Size::full();
      row.style.height = Size::pixel(36);
      row.style.justifyContent = Justify::Center;
      row.style.alignItems = Align::Center;
      row.style.hoverColor = Color{75, 75, 80};
      row.style.backgroundColor = [act, idx]() -> Color {
        return *act == idx ? Color{40, 40, 45} : Color{55, 55, 60};
      };
      row.tooltip = activity.tooltip;
      row.onClick = [act, idx] { *act = (*act == idx) ? -1 : idx; };

      Svg label;
      // label.label = activity.label;
      label.source = activity.icons;
      // label.fontSize = 12;
      // label.color = [act, idx]() -> Color {
      //   return *act == idx ? Color{255, 255, 255} : Color{185, 185, 190};
      // };
      label.style.width = Size::pixel(28);
      label.style.height = Size::pixel(28);
      label.fit = ObjectFit::Contain; // default anyway for Svg
      label.onError = [](const std::string &err) {
        // fires if liteui_svg::parseString() fails
        fprintf(stderr, "SVG parse error: %s\n", err.c_str());
      };
      row.addChild(label);
      bar.addChild(std::move(row));
    }
    return bar;
  }

  // ---- status bar ----
  // Built entirely from this TabbedEditor's own state — line:col of the
  // active document's cursor, its detected language (falling back to
  // "Plain Text"), a fixed "UTF-8" (the only encoding this editor ever
  // reads/writes — see EditorDocument/openFile/saveActive), and how many
  // tabs are open. All four fields are Dynamic<std::string>, so they
  // update live via LiteUI's normal checkForUpdates() polling with no
  // rebuild needed on every keystroke — only opening/closing a tab (a
  // real structural change) goes through rebuild().
  View buildStatusBar() {
    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(24);
    bar.style.backgroundColor = th::kStatusBarBg;
    bar.style.padding = EdgeInsets{0, 12, 0, 12};
    bar.style.gap = 16;
    bar.style.flexShrink = 0;

    Text posLabel;
    posLabel.label = std::function<std::string()>([this]() -> std::string {
      const EditorDocument *doc = activeDocument();
      if (!doc || doc->isWelcome)
        return "";
      return "Ln " + std::to_string(doc->state->cursor.line + 1) + ", Col " +
             std::to_string(doc->state->cursor.col + 1);
    });
    posLabel.fontSize = 12;
    posLabel.color = th::kOnAccent;
    bar.addChild(posLabel);

    Text langLabel;
    langLabel.label = std::function<std::string()>([this]() -> std::string {
      const EditorDocument *doc = activeDocument();
      if (!doc || doc->isWelcome)
        return "";
      if (!doc->highlighter || !doc->highlighter->lang)
        return "Plain Text";
      return doc->highlighter->lang->name;
    });
    langLabel.fontSize = 12;
    langLabel.color = th::kOnAccent;
    bar.addChild(langLabel);

    Text encLabel;
    encLabel.label = std::string("UTF-8");
    encLabel.fontSize = 12;
    encLabel.color = th::kOnAccent;
    bar.addChild(encLabel);

    Text countLabel;
    countLabel.label = std::function<std::string()>([this]() -> std::string {
      return std::to_string(openDocumentCount()) + " open";
    });
    countLabel.fontSize = 12;
    countLabel.color = th::kOnAccentDim;
    bar.addChild(countLabel);

    return bar;
  }

  // ---- top menu bar: File/Edit/Selection/View/Go/Run/Terminal/Help,
  // each a real dropdown. One shared backdrop (absolute, full-window)
  // catches "click outside to close" for whichever menu is open, exactly
  // the pattern from the standalone context-menu example: trigger
  // captures its own on-screen position via onLayout, the menu's
  // left/top are computed relative to the backdrop's own captured
  // position, and a single Dynamic<Display>/zIndex pair handles both
  // showing the right menu and keeping it on top.
  View buildMenuBar() {
    struct MenuItemSpec {
      std::string label;
      std::function<void()>
          action; // nullptr = no-op placeholder, still closes the menu
    };
    struct MenuDef {
      std::string title;
      std::vector<MenuItemSpec> items;
    };

    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(38);
    bar.style.backgroundColor = th::kMenuBarBg;
    bar.style.padding = EdgeInsets{0, 0, 0, 4};
    bar.style.gap = 2;
    bar.style.flexShrink = 0;

    View titleBox;
    titleBox.style.height = Size::full();
    titleBox.style.flexShrink = 0;
    titleBox.style.alignItems = Align::Center;
    titleBox.style.padding = EdgeInsets{0, 14, 0, 6}; // gap before "File"
    titleBox.style.backgroundColor = th::kMenuBarBg;
    titleBox.onPressAt = [this](float, float) { ui_.requestMove(); };

    Text titleText;
    titleText.label = title_;
    titleText.fontSize = 14;
    titleText.fontWeight = FontWeight::Bold;
    titleText.color = th::kTextMuted;
    titleText.wrap = TextWrap::NoWrap;
    titleBox.addChild(titleText);
    bar.addChild(std::move(titleBox));

    auto openIdx = openMenuIndex_;
    auto backdropX = std::make_shared<float>(0.0f);
    auto backdropY = std::make_shared<float>(0.0f);

    // Full-window backdrop. Transparent (alpha 2, matching the reference)
    // so it's still hit-testable for the outside-click-to-close without
    // visibly dimming anything. Only shown while some menu is open.
    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 100;
    backdrop.style.backgroundColor = th::kOverlayClear;
    backdrop.style.display = [openIdx]() -> Display {
      return *openIdx >= 0 ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openIdx] { *openIdx = -1; };
    backdrop.onLayout = [backdropX, backdropY](float x, float y, float, float) {
      *backdropX = x;
      *backdropY = y;
    };

    std::vector<MenuDef> menus;
    menus.push_back({"File",
                     {{"New Tab", [this] { newDocument(); }},
                      {"Open File...",
                       [this] {
                         auto p =
                             openFilePicker("Open File", {{"All Files", "*"}});
                         if (p)
                           openFile(*p);
                       }},
                      {"Open Folder...",
                       [this] {
                         openWorkspaceFolder();
                         *activityIndex_ = 0; // reveal the Explorer
                       }},
                      {"Save", [this] { saveActive(false); }},
                      {"Save As...", [this] { saveActive(true); }},
                      {"Close Tab", [this] {
                         if (*activeIndex_ >= 0)
                           closeTab(static_cast<size_t>(*activeIndex_));
                       }}}});
    menus.push_back({"Edit",
                     {{"Undo", [this] { editUndo(); }},
                      {"Redo", [this] { editRedo(); }},
                      {"Cut", [this] { editCut(); }},
                      {"Copy", [this] { editCopy(); }},
                      {"Paste", [this] { editPaste(); }}}});
    menus.push_back({"Selection",
                     {{"Select All", [this] { editSelectAll(); }},
                      {"Expand Selection", nullptr},
                      {"Shrink Selection", nullptr}}});
    menus.push_back({"View", {}});
    menus.push_back({"Go",
                     {{"Next Tab", [this] { nextTab(1); }},
                      {"Previous Tab", [this] { nextTab(-1); }}}});
    menus.push_back(
        {"Run",
         {{"Start Debugging", nullptr}, {"Run Without Debugging", nullptr}}});
    menus.push_back({"Terminal", {{"New Terminal", nullptr}}});
    menus.push_back({"Help", {{"About", nullptr}}});

    for (size_t mi = 0; mi < menus.size(); ++mi) {
      const MenuDef &def = menus[mi];
      size_t idx = mi;

      // Captured live from this trigger's own onLayout, same idea as the
      // reference's triggerX/triggerY/triggerH.
      auto triggerX = std::make_shared<float>(0.0f);
      auto triggerY = std::make_shared<float>(0.0f);
      auto triggerH = std::make_shared<float>(0.0f);

      View trigger;
      trigger.style.height = Size::full();
      trigger.style.padding = EdgeInsets{0, 10, 0, 10};
      trigger.style.alignItems = Align::Center;
      trigger.style.justifyContent = Justify::Center;
      trigger.style.hoverColor = th::kButtonHoverBg;
      trigger.style.backgroundColor = [openIdx, idx]() -> Color {
        return static_cast<size_t>(*openIdx) == idx ? th::kMenuBarOpenBg
                                                    : th::kMenuBarBg;
      };
      trigger.onClick = [openIdx, idx] {
        *openIdx =
            (static_cast<size_t>(*openIdx) == idx) ? -1 : static_cast<int>(idx);
      };
      trigger.onLayout = [triggerX, triggerY, triggerH](float x, float y, float,
                                                        float h) {
        *triggerX = x;
        *triggerY = y;
        *triggerH = h;
      };
      Text label;
      label.label = def.title;
      label.fontSize = 13;
      label.color = th::kText;
      trigger.addChild(label);
      bar.addChild(trigger);

      // Dropdown itself: absolute, positioned relative to the backdrop's
      // own captured origin exactly as in the reference (left/top =
      // trigger position minus backdrop position, plus trigger height
      // for a small drop below the bar).
      View menu;
      menu.style.position = Position::Absolute;
      menu.style.direction = FlexDirection::Column;
      menu.style.width = Size::pixel(200);
      menu.style.backgroundColor = th::kMenuBg;
      menu.style.borderWidth = 1.0f;
      menu.style.borderColor = th::kMenuBorder;
      menu.style.borderRadius = 4.0f;
      menu.style.zIndex = 200;
      menu.style.display = [openIdx, idx]() -> Display {
        return static_cast<size_t>(*openIdx) == idx ? Display::Flex
                                                    : Display::None;
      };
      menu.style.left = [triggerX, backdropX]() {
        return *triggerX - *backdropX;
      };
      menu.style.top = [triggerY, triggerH, backdropY]() {
        return (*triggerY - *backdropY) + *triggerH;
      };

      for (const auto &item : def.items) {
        Text itemLabel;
        itemLabel.label = item.label;
        itemLabel.style.padding = EdgeInsets::all(8);
        itemLabel.fontSize = 13;
        itemLabel.color = th::kText;

        View row;
        row.style.hoverColor = th::kMenuHoverBg;
        row.style.backgroundColor = th::kMenuBg;
        auto action = item.action;
        row.onClick = [openIdx, action] {
          *openIdx = -1;
          if (action)
            action();
        };
        row.addChild(itemLabel);
        menu.addChild(row);
      }

      backdrop.addChild(menu);
    }

    // ---- drag area: takes all free space to the right of the menus ----
    View dragArea;
    dragArea.style.height = Size::full();
    dragArea.style.flexGrow = 1;
    dragArea.style.backgroundColor = th::kMenuBarBg;
    dragArea.onPressAt = [this](float, float) { ui_.requestMove(); };
    bar.addChild(std::move(dragArea));

    // ---- window buttons: minimize / maximize / close ----
    auto makeWindowBtn = [](const std::string &glyph, Color hover,
                            std::function<void()> onClick) {
      View b;
      b.style.width = Size::pixel(46);
      b.style.height = Size::full();
      b.style.flexShrink = 0;
      b.style.justifyContent = Justify::Center;
      b.style.alignItems = Align::Center;
      b.style.backgroundColor = th::kMenuBarBg;
      b.style.hoverColor = hover;
      b.onClick = std::move(onClick);

      Svg icon;
      icon.source = glyph;
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

    bar.addChild(makeWindowBtn(
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
        th::kButtonHoverBg, [this] { ui_.requestMinimize(); }));
    bar.addChild(makeWindowBtn(
        R"svg(<?xml version="1.0" encoding="utf-8"?><!-- Uploaded to: SVG Repo, www.svgrepo.com, Generator: SVG Repo Mixer Tools -->
<svg width="800px" height="800px" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg">
<path fill-rule="evenodd" clip-rule="evenodd" d="M22 5C22 3.34315 20.6569 2 19 2H5C3.34315 2 2 3.34315 2 5V19C2 20.6569 3.34315 22 5 22H19C20.6569 22 22 20.6569 22 19V5ZM20 5C20 4.44772 19.5523 4 19 4H5C4.44772 4 4 4.44772 4 5V19C4 19.5523 4.44772 20 5 20H19C19.5523 20 20 19.5523 20 19V5Z" fill="#0F0F0F"/>
</svg>)svg",
        th::kButtonHoverBg, [this] { ui_.requestMaximize(); }));
    bar.addChild(makeWindowBtn(
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
        Color{196, 43, 28}, [this] { ui_.requestClose(); }));

    bar.addChild(backdrop);
    return bar;
  }

  // A document's identity for keyed reconciliation. docs_ is append-only
  // (closing soft-deletes — see EditorDocument::closed), so a document's
  // index never changes for its whole lifetime, which is exactly the
  // stability a key needs. Closed documents are simply absent from the
  // list, and reconcileChildren() frees their rows.
  std::vector<std::string> documentKeys() const {
    std::vector<std::string> keys;
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed)
        keys.push_back(std::to_string(i));
    return keys;
  }
  static size_t keyToIndex(const std::string &key) {
    return static_cast<size_t>(std::stoul(key));
  }

  // One tab in the strip. Built once per document, when its key first
  // appears; everything that can change afterwards (title, modified dot,
  // active background) is a Dynamic<> reading docs_/activeIndex_ live.
  View buildTab(size_t idx) {
    auto activeIndexPtr = activeIndex_;

    View tab;
    tab.style.direction = FlexDirection::Row;
    tab.style.alignItems = Align::Center;
    tab.style.padding = EdgeInsets{6, 10, 6, 10};
    tab.style.gap = 8;
    tab.style.height = Size::full();
    tab.style.backgroundColor = [activeIndexPtr, idx]() -> Color {
      return *activeIndexPtr == static_cast<int>(idx) ? th::kEditorBg
                                                      : th::kTabInactiveBg;
    };
    tab.style.hoverColor = th::kTabHoverBg;
    tab.onClick = [activeIndexPtr, idx] {
      *activeIndexPtr = static_cast<int>(idx);
    };

    Text label;
    // Reads docs_[idx] on every poll rather than capturing the title by
    // value: Save As renames a document in place, and without a rebuild
    // there is nothing else left to refresh the label.
    label.label = std::function<std::string()>([this, idx]() -> std::string {
      if (idx >= docs_.size())
        return std::string();
      const EditorDocument &d = docs_[idx];
      return d.title + (*d.modified ? " *" : "");
    });
    label.fontSize = 13;
    label.color = std::function<Color()>([activeIndexPtr, idx]() -> Color {
      return *activeIndexPtr == static_cast<int>(idx) ? th::kTextBright
                                                      : th::kTextInactive;
    });
    tab.addChild(label);

    View closeBtn;
    closeBtn.style.width = Size::pixel(16);
    closeBtn.style.height = Size::pixel(16);
    closeBtn.style.justifyContent = Justify::Center;
    closeBtn.style.alignItems = Align::Center;
    closeBtn.style.borderRadius = 3.0f;
    closeBtn.style.hoverColor = th::kButtonHoverBg;
    closeBtn.style.backgroundColor = Color{0, 0, 0, 0};
    closeBtn.onClick = [this, idx] { closeTab(idx); };
    Text closeLabel;
    closeLabel.label = std::string("x");
    closeLabel.fontSize = 12;
    closeLabel.color = th::kTextDim;
    closeBtn.addChild(closeLabel);
    tab.addChild(closeBtn);
    return tab;
  }

  // One document's CodeEditor. Hidden rather than destroyed when inactive,
  // same as before — switching tabs only flips this Dynamic<Display>, so
  // the keyed list never reconciles on a tab switch at all.
  View buildEditor(size_t idx) {
    auto activeIndexPtr = activeIndex_;
    EditorDocument &doc = docs_[idx];
    if (doc.isWelcome)
      return buildWelcomePane(idx);

    CodeEditor ed;
    ed.style.flexGrow = 1;
    ed.style.display = [activeIndexPtr, idx]() -> Display {
      return *activeIndexPtr == static_cast<int>(idx) ? Display::Flex
                                                      : Display::None;
    };
    ed.style.backgroundColor = th::kEditorBg;
    ed.showLineNumbers = true;
    ed.fontFamily = "Monospace";
    ed.resetStateFromText = false; // reuse doc.state as-is
    ed.state = doc.state;
    std::shared_ptr<bool> modifiedFlag = doc.modified;
    ed.onChange = [modifiedFlag](const std::string &) { *modifiedFlag = true; };
    applyHighlighting(ed, doc.highlighter);
    View view = toCodeEditorView(std::move(ed));

    // Capture the editor's on-screen origin so the menu can appear at the
    // click point (onRightPressAt only gives coordinates local to the view).
    auto edX = std::make_shared<float>(0.0f);
    auto edY = std::make_shared<float>(0.0f);
    view.onLayout = [edX, edY](float x, float y, float, float) {
      *edX = x;
      *edY = y;
    };
    view.onRightPressAt = [this, idx, edX, edY](float lx, float ly) {
      openEditorContextMenu(idx, *edX + lx, *edY + ly);
    };
    return view;
  }

  // VS-Code-style landing tab: shown in place of a blank untitled buffer
  // (see newWelcomeTab()). Lives in the same keyed `stack` container as
  // every CodeEditor, so it switches tabs the same way — a plain
  // Dynamic<Display> keyed off activeIndex_, not a rebuild.
  View buildWelcomePane(size_t idx) {
    auto activeIndexPtr = activeIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = th::kEditorBg;
    pane.style.padding = EdgeInsets::all(48.0f);
    pane.style.gap = 4;
    pane.style.display = [activeIndexPtr, idx]() -> Display {
      return *activeIndexPtr == static_cast<int>(idx) ? Display::Flex
                                                      : Display::None;
    };

    Text title;
    title.label = std::string("liteui code editor");
    title.fontSize = 28;
    title.fontWeight = FontWeight::SemiBold;
    title.color = th::kText;
    pane.addChild(title);

    Text subtitle;
    subtitle.label = std::string("A lightweight editor.");
    subtitle.fontSize = 13;
    subtitle.color = th::kTextMuted;
    subtitle.style.margin = EdgeInsets{4, 0, 24, 0};
    pane.addChild(subtitle);

    Text startHeader;
    startHeader.label = std::string("Start");
    startHeader.fontSize = 12;
    startHeader.fontWeight = FontWeight::SemiBold;
    startHeader.color = th::kTextDim;
    startHeader.style.margin = EdgeInsets{0, 0, 4, 0};
    pane.addChild(startHeader);

    auto makeLink = [](const std::string &label,
                       std::function<void()> onClick) {
      View row;
      row.style.padding = EdgeInsets{4, 0, 4, 0};
      row.style.hoverColor = th::kHoverBg;
      row.style.backgroundColor = th::kEditorBg;
      row.onClick = std::move(onClick);
      Text t;
      t.label = label;
      t.fontSize = 13;
      t.color = th::kLink;
      row.addChild(t);
      return row;
    };

    pane.addChild(makeLink("New File", [this] { newDocument(); }));
    pane.addChild(makeLink("Open File...", [this] {
      auto p = openFilePicker("Open File", {{"All Files", "*"}});
      if (p)
        openFile(*p);
    }));
    pane.addChild(makeLink("Open Folder...", [this] {
      openWorkspaceFolder();
      *activityIndex_ = kExplorerActivityId; // reveal the Explorer
    }));

    return pane;
  }

  // Same shape as buildDeleteDialog() — full-window dimmed backdrop,
  // centered box, click on the box swallowed — with a third button.
  View buildUnsavedChangesDialog() {
    auto openPtr = unsavedDialogOpen_;

    View dialogBox;
    dialogBox.style.direction = FlexDirection::Column;
    dialogBox.style.width = Size::pixel(360);
    dialogBox.style.padding = EdgeInsets::all(20);
    dialogBox.style.gap = 16;
    dialogBox.style.backgroundColor = th::kSideBarBg;
    dialogBox.style.borderWidth = 1.0f;
    dialogBox.style.borderColor = th::kMenuBorder;
    dialogBox.style.borderRadius = 8.0f;
    dialogBox.onClick = [] {};

    Text title;
    title.label = std::function<std::string()>([this]() -> std::string {
      return "Save changes to \"" +
             (unsavedDialogDocIndex_ < docs_.size()
                  ? docs_[unsavedDialogDocIndex_].title
                  : std::string()) +
             "\"?";
    });
    title.fontSize = 18;
    title.fontWeight = FontWeight::SemiBold;
    title.color = th::kTextBright;
    dialogBox.addChild(title);

    Text message;
    message.label =
        std::string("Your changes will be lost if you don't save them.");
    message.wrap = TextWrap::Wrap;
    message.style.width = Size::full();
    message.color = th::kTextMuted;
    dialogBox.addChild(message);

    View buttonRow;
    buttonRow.style.direction = FlexDirection::Row;
    buttonRow.style.justifyContent = Justify::End;
    buttonRow.style.backgroundColor = th::kTransparent;
    buttonRow.style.gap = 10;

    auto makeBtn = [](const std::string &label, Color bg, Color hover,
                      Color textColor, std::function<void()> onClick) {
      Text t;
      t.label = label;
      t.color = textColor;
      View b;
      b.style.width = Size::pixel(90);
      b.style.height = Size::pixel(34);
      b.style.backgroundColor = bg;
      b.style.hoverColor = hover;
      b.style.borderRadius = 4.0f;
      b.style.alignItems = Align::Center;
      b.style.justifyContent = Justify::Center;
      b.onClick = std::move(onClick);
      b.addChild(t);
      return b;
    };

    buttonRow.addChild(makeBtn("Cancel", th::kButtonBg, th::kButtonHoverBg,
                               th::kText, [this] { cancelUnsavedDialog(); }));
    buttonRow.addChild(makeBtn("Don't Save", th::kButtonBg, th::kButtonHoverBg,
                               th::kText,
                               [this] { confirmUnsavedDialog(false); }));
    buttonRow.addChild(makeBtn("Save", th::kAccent, th::kMenuHoverBg,
                               th::kOnAccent,
                               [this] { confirmUnsavedDialog(true); }));
    dialogBox.addChild(std::move(buttonRow));

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 300; // same tier as buildDeleteDialog — the
                                 // two can never be open together
    backdrop.style.backgroundColor = th::kOverlay;
    backdrop.style.alignItems = Align::Center;
    backdrop.style.justifyContent = Justify::Center;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [this] { cancelUnsavedDialog(); };
    backdrop.addChild(dialogBox);
    return backdrop;
  }

  void cancelUnsavedDialog() { *unsavedDialogOpen_ = false; }

  // save=true ("Save"): try to save first, and only close if the save
  // actually went through — a cancelled Save-As picker leaves the tab
  // open rather than silently discarding the buffer. save=false
  // ("Don't Save"): close unconditionally.
  void confirmUnsavedDialog(bool save) {
    size_t idx = unsavedDialogDocIndex_;
    *unsavedDialogOpen_ = false;
    if (idx >= docs_.size())
      return;
    if (save && !saveDocument(idx))
      return;
    closeTabForce(idx);
  }

  View buildErrorDialog() {
    auto openPtr = errorDialogOpen_;

    View dialogBox;
    dialogBox.style.direction = FlexDirection::Column;
    dialogBox.style.width = Size::pixel(360);
    dialogBox.style.padding = EdgeInsets::all(20);
    dialogBox.style.gap = 16;
    dialogBox.style.backgroundColor = th::kSideBarBg;
    dialogBox.style.borderWidth = 1.0f;
    dialogBox.style.borderColor = th::kMenuBorder;
    dialogBox.style.borderRadius = 8.0f;
    dialogBox.onClick = [] {};

    Text title;
    title.label = std::string("Error");
    title.fontSize = 18;
    title.fontWeight = FontWeight::SemiBold;
    title.color = th::kTextBright;
    dialogBox.addChild(title);

    // Dynamic label: dialogBox is built once and reused, so this must
    // re-poll errorDialogMessage_ rather than capture it by value —
    // otherwise a second error would still show the first one's text.
    Text message;
    message.label = std::function<std::string()>(
        [this]() -> std::string { return errorDialogMessage_; });
    message.wrap = TextWrap::Wrap;
    message.style.width = Size::full();
    message.color = th::kTextMuted;
    dialogBox.addChild(message);

    View buttonRow;
    buttonRow.style.direction = FlexDirection::Row;
    buttonRow.style.justifyContent = Justify::End;
    buttonRow.style.backgroundColor = th::kTransparent;

    Text okLabel;
    okLabel.label = std::string("OK");
    okLabel.color = th::kOnAccent;

    View okButton;
    okButton.style.width = Size::pixel(80);
    okButton.style.height = Size::pixel(34);
    okButton.style.backgroundColor = th::kAccent;
    okButton.style.hoverColor = th::kMenuHoverBg;
    okButton.style.borderRadius = 4.0f;
    okButton.style.alignItems = Align::Center;
    okButton.style.justifyContent = Justify::Center;
    okButton.onClick = [openPtr] { *openPtr = false; };
    okButton.addChild(okLabel);
    buttonRow.addChild(std::move(okButton));
    dialogBox.addChild(std::move(buttonRow));

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    // Above the delete/unsaved dialogs' 300 — an I/O error dialog should
    // never end up hidden behind one of those, even though in practice
    // they can't currently be open at the same time.
    backdrop.style.zIndex = 350;
    backdrop.style.backgroundColor = th::kOverlay;
    backdrop.style.alignItems = Align::Center;
    backdrop.style.justifyContent = Justify::Center;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openPtr] { *openPtr = false; }; // click outside = OK
    backdrop.addChild(dialogBox);
    return backdrop;
  }

  // Delete-confirmation dialog, built the same way as a standalone
  // confirm dialog: a dimmed, absolute, full-window backdrop that
  // centers dialogBox via alignItems/justifyContent, closes on an
  // outside click, and swallows clicks that land on the dialog itself
  // so they don't bubble up to that same outside-click handler.
  View buildDeleteDialog() {
    auto openPtr = deleteDialogOpen_;

    View dialogBox;
    dialogBox.style.direction = FlexDirection::Column;
    dialogBox.style.width = Size::pixel(340);
    dialogBox.style.padding = EdgeInsets::all(20);
    dialogBox.style.gap = 16;

    dialogBox.style.backgroundColor = th::kSideBarBg;
    dialogBox.style.borderWidth = 1.0f;
    dialogBox.style.borderColor = th::kMenuBorder;
    dialogBox.style.borderRadius = 8.0f;
    dialogBox.onClick = [] {};
    Text title;
    title.label = std::function<std::string()>([this]() -> std::string {
      return deleteTargetIsDir_ ? "Delete Folder?" : "Delete File?";
    });
    title.fontSize = 18;
    title.fontWeight = FontWeight::SemiBold;
    title.color = th::kTextBright;
    dialogBox.addChild(title);

    Text message;
    message.label = std::function<std::string()>([this]() -> std::string {
      std::string name = deleteTargetPath_.empty()
                             ? std::string()
                             : editorTitleFromPath(deleteTargetPath_);
      if (deleteTargetIsDir_)
        return "\"" + name +
               "\" and everything inside it will be permanently deleted. "
               "This action cannot be undone.";
      return "\"" + name +
             "\" will be permanently deleted. This action cannot be undone.";
    });
    message.wrap = TextWrap::Wrap;
    message.style.width = Size::full();
    message.color = th::kTextMuted;
    dialogBox.addChild(message);

    View buttonRow;
    buttonRow.style.direction = FlexDirection::Row;
    buttonRow.style.justifyContent = Justify::End;
    buttonRow.style.backgroundColor = th::kTransparent;
    buttonRow.style.gap = 10;

    Text cancelLabel;
    cancelLabel.label = std::string("Cancel");
    cancelLabel.color = th::kText;

    View cancelButton;
    cancelButton.style.width = Size::pixel(80);
    cancelButton.style.height = Size::pixel(34);
    cancelButton.style.backgroundColor = th::kButtonBg;
    cancelButton.style.hoverColor = th::kButtonHoverBg;
    cancelButton.style.borderRadius = 4.0f;
    cancelButton.style.alignItems = Align::Center;
    cancelButton.style.justifyContent = Justify::Center;
    cancelButton.onClick = [this] { cancelDeleteConfirm(); };
    cancelButton.addChild(cancelLabel);

    Text confirmLabel;
    confirmLabel.label = std::string("Delete");
    confirmLabel.color = Color{255, 255, 255};
    View confirmButton;
    confirmButton.style.width = Size::pixel(80);
    confirmButton.style.height = Size::pixel(34);
    confirmButton.style.borderRadius = 4.0f;
    confirmButton.style.alignItems = Align::Center;
    confirmButton.style.justifyContent = Justify::Center;
    confirmButton.style.backgroundColor = th::kDanger;
    confirmButton.style.hoverColor = th::kDangerHover;
    confirmButton.onClick = [this] { confirmDelete(); };
    confirmButton.addChild(confirmLabel);

    buttonRow.addChild(cancelButton);
    buttonRow.addChild(confirmButton);
    dialogBox.addChild(buttonRow);

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 300; // above the menu dropdowns (200)
    backdrop.style.backgroundColor = th::kOverlay;
    backdrop.style.alignItems = Align::Center;
    backdrop.style.justifyContent = Justify::Center;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [this] { cancelDeleteConfirm(); };
    backdrop.addChild(dialogBox);
    return backdrop;
  }

  View buildEditorContextMenu() {
    auto openPtr = editorMenuOpen_;
    auto xPtr = editorMenuX_;
    auto yPtr = editorMenuY_;

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 250;
    backdrop.style.backgroundColor = th::kOverlayClear;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openPtr] { *openPtr = false; };

    View menu;
    menu.style.position = Position::Absolute;
    menu.style.direction = FlexDirection::Column;
    menu.style.width = Size::pixel(150);
    menu.style.backgroundColor = th::kMenuBg;
    menu.style.borderColor = th::kMenuBorder;
    menu.style.borderWidth = 1.0f;
    menu.style.borderRadius = 4.0f;
    menu.style.zIndex = 260;
    menu.style.left = [xPtr]() { return *xPtr; };
    menu.style.top = [yPtr]() { return *yPtr; };

    auto makeItem = [openPtr](const std::string &label,
                              std::function<void()> action,
                              std::function<bool()> enabled) {
      Text t;
      t.label = label;
      t.style.padding = EdgeInsets::all(8);
      t.fontSize = 13;
      t.color = std::function<Color()>([enabled]() -> Color {
        return enabled() ? th::kText : th::kTextDim;
      });

      View row;
      row.style.backgroundColor = th::kMenuBg;
      row.style.hoverColor = th::kMenuHoverBg;
      // A disabled row isn't clickable; the click falls through to the
      // backdrop, which just closes the menu.
      row.disabled = [enabled]() -> bool { return !enabled(); };
      row.onClick = [openPtr, action] {
        *openPtr = false;
        action();
      };
      row.addChild(t);
      return row;
    };

    auto hasSel = [this] { return editorMenuHasSelection(); };
    auto always = [] { return true; };

    menu.addChild(makeItem("Cut", [this] { editorContextCut(); }, hasSel));
    menu.addChild(makeItem("Copy", [this] { editorContextCopy(); }, hasSel));
    menu.addChild(makeItem("Paste", [this] { editorContextPaste(); }, always));

    backdrop.addChild(menu);
    return backdrop;
  }

  // Right-click context menu, same backdrop+absolute-menu shape as the
  // top menu bar's dropdowns. Item visibility toggles per target (file vs
  // folder) via Dynamic<Display> reading explorerMenuTargetIsDir_, since
  // the menu itself is built once and never rebuilt.
  View buildExplorerContextMenu() {
    auto openPtr = explorerMenuOpen_;
    auto xPtr = explorerMenuX_;
    auto yPtr = explorerMenuY_;

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 250; // above menu-bar dropdowns (200), below
                                 // the delete-confirmation dialog (300)
    backdrop.style.backgroundColor = th::kOverlayClear;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openPtr] { *openPtr = false; };

    View menu;
    menu.style.position = Position::Absolute;
    menu.style.direction = FlexDirection::Column;
    menu.style.width = Size::pixel(170);
    menu.style.backgroundColor = th::kMenuBg;
    menu.style.borderColor = th::kMenuBorder;
    menu.style.borderWidth = 1.0f;
    menu.style.borderRadius = 4.0f;
    menu.style.zIndex = 260;
    menu.style.left = [xPtr]() { return *xPtr; };
    menu.style.top = [yPtr]() { return *yPtr; };

    auto makeItem = [this](const std::string &label,
                           std::function<void()> action,
                           std::function<Display()> display = nullptr) {
      Text t;
      t.label = label;
      t.style.padding = EdgeInsets::all(8);
      t.fontSize = 13;
      t.color = th::kText;

      View row;
      row.style.backgroundColor = th::kMenuBg;
      row.style.hoverColor = th::kMenuHoverBg;
      if (display)
        row.style.display = std::move(display);
      auto openPtr = explorerMenuOpen_;
      row.onClick = [openPtr, action] {
        *openPtr = false;
        if (action)
          action();
      };
      row.addChild(t);
      return row;
    };

    auto fileOnly = [this]() -> Display {
      return explorerMenuTargetIsDir_ ? Display::None : Display::Flex;
    };
    auto dirOnly = [this]() -> Display {
      return explorerMenuTargetIsDir_ ? Display::Flex : Display::None;
    };

    // Order matches the request for both cases: a file sees
    // Open/Rename/Delete; a folder sees New Folder/New File/Rename/Delete
    // (Open simply hides itself for a folder, and New File/New Folder
    // hide themselves for a file).
    menu.addChild(
        makeItem("Open", [this] { explorerContextOpen(); }, fileOnly));
    menu.addChild(makeItem(
        "New Folder", [this] { explorerContextNewFolder(); }, dirOnly));
    menu.addChild(
        makeItem("New File", [this] { explorerContextNewFile(); }, dirOnly));
    menu.addChild(makeItem("Rename", [this] { explorerContextRename(); }));
    menu.addChild(makeItem("Delete", [this] { explorerContextDelete(); }));

    backdrop.addChild(menu);
    return backdrop;
  }

  // Builds the tree once. The tab strip and the editor stack are keyed
  // containers: their children come from keysSource/itemBuilder, and
  // LiteUI's checkForUpdates() reconciles them after every dispatched
  // event. Opening or closing a document costs one build, not a
  // re-rasterization of every tab and every editor in the window.
  void buildRoot() {
    View root;
    root.style.direction = FlexDirection::Column;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.style.backgroundColor = th::kEditorBg;
    root.addChild(buildMenuBar());
    View mainArea;
    mainArea.style.direction = FlexDirection::Row;
    mainArea.style.width = Size::full();
    mainArea.style.flexGrow = 1;
    mainArea.style.backgroundColor = th::kEditorBg;

    mainArea.addChild(buildActivityBar());

    mainArea.addChild(buildSidePanel());
    mainArea.addChild(buildSideDivider());

    View editorArea;
    editorArea.style.direction = FlexDirection::Column;
    editorArea.style.height = Size::full();
    editorArea.style.flexGrow = 1;
    editorArea.style.backgroundColor = th::kEditorBg;

    // ---- tab strip ----
    View tabBar;
    tabBar.style.direction = FlexDirection::Row;
    tabBar.style.alignItems = Align::Center;
    tabBar.style.width = Size::full();
    tabBar.style.height = Size::pixel(34);
    tabBar.style.overflowX = Overflow::Auto;
    tabBar.style.gap = 2;
    tabBar.style.backgroundColor = th::kTabBarBg;

    // Defensive: keep the tab strip un-shrinkable, the same way
    // buildHDivider()/buildTerminalPanel() already protect themselves.
    // Without this, any sibling whose Fit-sizing balloons (as
    // buildWelcomePane's did before the fix above) can eat into the
    // tab bar's height via this file's flex-shrink pool.
    tabBar.style.flexShrink = 0;

    // The keyed tabs live in their own container rather than directly in
    // tabBar: reconcileChildren() replaces a node's children wholesale, so
    // a keyed container can't also hold fixed siblings like the "+" button.
    View tabList;
    tabList.style.direction = FlexDirection::Row;
    tabList.style.alignItems = Align::Center;
    tabList.style.height = Size::full();
    tabList.style.gap = 2;
    tabList.style.backgroundColor = th::kTabBarBg;
    tabList.keysSource = [this] { return documentKeys(); };
    tabList.itemBuilder = [this](const std::string &key) {
      return buildTab(keyToIndex(key));
    };
    tabBar.addChild(std::move(tabList));

    View newTabBtn;
    newTabBtn.style.width = Size::pixel(28);
    newTabBtn.style.height = Size::full();
    newTabBtn.style.justifyContent = Justify::Center;
    newTabBtn.style.alignItems = Align::Center;
    newTabBtn.style.backgroundColor = th::kTabBarBg;
    newTabBtn.style.hoverColor = th::kButtonHoverBg;
    newTabBtn.onClick = [this] { newDocument(); };

    Text plus;
    plus.label = std::string("+");
    plus.fontSize = 16;
    plus.color = th::kTextMuted;
    newTabBtn.addChild(plus);
    tabBar.addChild(newTabBtn);

    editorArea.addChild(tabBar);

    // ---- editor stack: one CodeEditor per open document, keyed the same
    // way as the tabs, all but the active one hidden via Display::None ----
    View stack;
    stack.style.width = Size::full();
    stack.style.flexGrow = 1;
    stack.style.backgroundColor = th::kEditorBg;

    stack.keysSource = [this] { return documentKeys(); };
    stack.itemBuilder = [this](const std::string &key) {
      return buildEditor(keyToIndex(key));
    };
    editorArea.addChild(stack);

    editorArea.addChild(buildHDivider());
    editorArea.addChild(buildTerminalPanel());

    mainArea.addChild(editorArea);

    root.addChild(mainArea);
    root.addChild(buildStatusBar());
    root.addChild(buildDeleteDialog());
    root.addChild(buildUnsavedChangesDialog());
    root.addChild(buildErrorDialog());
    root.addChild(buildExplorerContextMenu());
    root.addChild(buildEditorContextMenu());
    ui_.setRoot(std::move(root));
  }

  bool shortcutsInstalled_ = false;
  void setupShortcuts() {
    if (shortcutsInstalled_)
      return;
    shortcutsInstalled_ = true;

    KeyModifiers ctrl;
    ctrl.ctrl = true;
    KeyModifiers ctrlShift;
    ctrlShift.ctrl = true;
    ctrlShift.shift = true;

    ui_.addShortcut(ctrl, Key::T, [this] { newDocument(); });
    ui_.addShortcut(ctrl, Key::W, [this] {
      if (*activeIndex_ >= 0)
        closeTab(static_cast<size_t>(*activeIndex_));
    });
    ui_.addShortcut(ctrl, Key::O, [this] {
      auto picked = openFilePicker("Open File", {{"All Files", "*"}});
      if (picked)
        openFile(*picked);
    });
    ui_.addShortcut(ctrl, Key::S, [this] { saveActive(false); });
    ui_.addShortcut(ctrlShift, Key::S, [this] { saveActive(true); });
    ui_.addShortcut(ctrl, Key::Tab, [this] { nextTab(1); });
    ui_.addShortcut(ctrlShift, Key::Tab, [this] { nextTab(-1); });
    ui_.addShortcut(ctrl, Key::PageDown, [this] { nextTab(1); });
    ui_.addShortcut(ctrl, Key::PageUp, [this] { nextTab(-1); });

    // Global shortcuts fire before a focused view's own onKeyDown (see
    // LiteUI::dispatchKeyDown), so this guards against stealing Delete
    // away from an in-progress rename (the pending-create TextInput) or
    // from a CodeEditor that's actually focused and mid-edit — in both
    // those cases Delete should do its normal "delete the next
    // character" thing instead of popping this dialog.
    ui_.addShortcut(KeyModifiers{}, Key::Delete, [this] {
      if (pendingCreate_.active || pendingRename_.active)
        return;
      const EditorDocument *active = activeDocument();
      if (active && !active->isWelcome && active->state &&
          active->state->focused)
        return;

      beginDeleteConfirm();
    });
  }

  bool commandsInstalled_ = false;
};

int main(int argc, char **argv) {
  TabbedEditor editor("CODE");
  for (int i = 1; i < argc; ++i)
    editor.openFile(argv[i]);

  editor.run();
  return 0;
}