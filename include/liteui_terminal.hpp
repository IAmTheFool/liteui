// liteui_terminal.hpp
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
// Requires liteui.hpp to be included first.

#pragma once

#include "liteui.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

namespace liteui_terminal {

// ==================== color palette ====================
//
// The 16-color ANSI palette and the 256-color cube/ramp built on top of
// it, chosen to match VS Code's own default dark-theme terminal palette
// so the widget looks at home next to the editor.
inline Color ansi16(int idx, bool bright) {
  static const Color kNormal[8] = {
      {0, 0, 0, 255},      {205, 49, 49, 255},   {13, 188, 121, 255},
      {229, 229, 16, 255}, {36, 114, 200, 255},  {188, 63, 188, 255},
      {17, 168, 205, 255}, {229, 229, 229, 255}};
  static const Color kBright[8] = {
      {102, 102, 102, 255}, {241, 76, 76, 255},   {35, 209, 139, 255},
      {245, 245, 67, 255},  {59, 142, 234, 255},  {214, 112, 214, 255},
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
           faint == o.faint && underline == o.underline &&
           reverse == o.reverse;
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

// ==================== VT/xterm-subset parser + screen state ====================
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
      scrollBottom_ = std::clamp(
          params.size() > 1 ? n(1, rows_) - 1 : rows_ - 1, scrollTop_, rows_ - 1);
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