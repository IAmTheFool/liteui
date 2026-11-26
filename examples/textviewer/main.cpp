// textviewer — open a text file and read it on a Canvas: a fast, virtualized
// viewer in the same spirit as imageviewer (Canvas + pointer/wheel/key events,
// custom scrollbars, drag-and-drop, app-global shortcuts).
//
//   textviewer [path/to/file.txt]
//
// Why it is built this way (the "best way" for LiteUI):
//
//  * ONE Canvas, not one Text view per line. A View tree with a node per line
//    would need layout, retained state and a scroll container for every line,
//    and falls over at a few thousand lines. A Canvas lets us draw only the
//    ~50 rows that are visible, so a 100 MB log opens as fast as a 1 KB note.
//  * The file is loaded once and INDEXED, not split: one std::string for the
//    bytes plus a vector of line-start offsets and per-line column counts.
//    Lines are string_views into the buffer; nothing is copied per line.
//  * MONOSPACE grid. With a fixed cell width, "which text is on screen" is
//    pure arithmetic (first/last row, first/last column), horizontal scroll
//    and word wrap need no text measurement, and long lines are cheap because
//    only the visible column range is ever shaped. Runs of plain ASCII are
//    drawn as ONE fillText; anything else (CJK, emoji, accents) is placed per
//    cell on the grid so a fallback font's odd advance can't shift a line.
//  * Scroll position is in LOGICAL units and rows are snapped to DEVICE pixels
//    (CanvasContext::devicePixelRatio()), so text stays crisp at 125% / 150%.
//  * Repaint only on change: handlers call state.markDirty(), which reports the
//    damage to the Canvas through Canvas::damage (same pattern as imageviewer).
//
// Features: Open (button, Ctrl+O, drop a file, drop text), line-number gutter,
// wheel / trackpad scroll (Shift+wheel = sideways), draggable scrollbars,
// Ctrl+wheel or Ctrl+= / Ctrl+- / Ctrl+0 font zoom, word wrap (Alt+Z),
// incremental search with highlights (Ctrl+F, Enter / F3 next, Shift+F3 prev),
// keyboard navigation (arrows, PageUp/Down, Home/End, Ctrl+Home/End) once the
// text area has focus (click it).
//
// Limits (deliberate, to keep it small): read-only, no selection, UTF-8 only
// (a UTF-8 BOM is skipped, bad bytes show as U+FFFD), files up to 512 MB,
// search is ASCII case-insensitive.

#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
  constexpr float kBar = kScrollbarThickness;
  constexpr float kMinThumb = 24.0f;
  constexpr int kTabWidth = 4;
  constexpr float kMinFont = 8.0f;
  constexpr float kMaxFont = 48.0f;
  constexpr float kDefaultFont = 14.0f;
  constexpr float kGutterPad = 8.0f;
  constexpr size_t kMaxFileBytes = size_t(512) << 20;
  constexpr size_t kCountLimitBytes = size_t(16) << 20; // no live match count above this

#if defined(_WIN32)
  const char *const kMono = "Consolas";
#else
  const char *const kMono = "Monospace"; // fontconfig alias, resolved by Pango
#endif

  // ---- UTF-8 / column helpers ------------------------------------------------

  struct Cp
  {
    uint32_t cp;
    int len;
  };

  // Lenient decoder: a bad or truncated sequence yields U+FFFD for ONE byte,
  // so the scan always makes progress and never reads past `end`.
  Cp decode(const char *p, const char *end)
  {
    const auto b0 = static_cast<unsigned char>(*p);
    if (b0 < 0x80)
      return {b0, 1};
    int n = 0;
    uint32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF)
      n = 1, cp = b0 & 0x1F;
    else if (b0 >= 0xE0 && b0 <= 0xEF)
      n = 2, cp = b0 & 0x0F;
    else if (b0 >= 0xF0 && b0 <= 0xF4)
      n = 3, cp = b0 & 0x07;
    else
      return {0xFFFD, 1};
    if (end - p <= n)
      return {0xFFFD, 1};
    for (int i = 1; i <= n; ++i)
    {
      const auto b = static_cast<unsigned char>(p[i]);
      if ((b & 0xC0) != 0x80)
        return {0xFFFD, 1};
      cp = (cp << 6) | (b & 0x3F);
    }
    return {cp, n + 1};
  }

  // Terminal-style cell width: 0 for combining / zero-width, 2 for East Asian
  // wide and emoji, 1 for everything else.
  int cpWidth(uint32_t c)
  {
    if (c < 0x300)
      return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) ||
        (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0xFE20 && c <= 0xFE2F) || c == 0xFEFF || (c >= 0xE0100 && c <= 0xE01EF))
      return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
      return 2;
    return 1;
  }

  // Columns a line occupies (tabs expanded). Stops at byte `stopAt` if given.
  int columnsOf(std::string_view line, size_t stopAt = std::string_view::npos)
  {
    const char *p = line.data();
    const char *end = p + line.size();
    const char *stop = stopAt < line.size() ? p + stopAt : end;
    int col = 0;
    while (p < stop)
    {
      const auto b = static_cast<unsigned char>(*p);
      if (b < 0x80)
      {
        col += (b == '\t') ? kTabWidth - col % kTabWidth : 1;
        ++p;
        continue;
      }
      Cp c = decode(p, end);
      col += cpWidth(c.cp);
      p += c.len;
    }
    return col;
  }

  void appendUtf8(std::string &out, uint32_t cp) // BMP only (control pictures)
  {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }

  // One visible cell: a character (or a tab / zero-width gap) on the grid.
  struct Cell
  {
    uint32_t off; // byte offset of the character inside its line
    int col, w;   // first column, width in columns
    std::string text;
  };

  // Cells that intersect columns [c0, c1) of `line`. Cost is O(c1), not O(line
  // length), so a huge minified line is only walked up to the visible edge.
  void buildCells(std::string_view line, int c0, int c1, std::vector<Cell> &out)
  {
    out.clear();
    const char *base = line.data();
    const char *p = base;
    const char *end = base + line.size();
    int col = 0;
    while (p < end && col < c1)
    {
      Cp c = decode(p, end);
      const int w = c.cp == '\t' ? kTabWidth - col % kTabWidth : cpWidth(c.cp);
      if (w == 0) // combining mark: rides on the previous cell
      {
        if (!out.empty())
          out.back().text.append(p, static_cast<size_t>(c.len));
        p += c.len;
        continue;
      }
      if (col + w > c0)
      {
        Cell cell{static_cast<uint32_t>(p - base), col, w, {}};
        if (c.cp == '\t')
        {
          // gap only
        }
        else if (c.cp < 0x20)
          appendUtf8(cell.text, 0x2400 + c.cp); // control picture
        else if (c.cp == 0x7F)
          appendUtf8(cell.text, 0x2421);
        else if (c.cp == 0xFFFD)
          cell.text = "\xEF\xBF\xBD";
        else
          cell.text.assign(p, static_cast<size_t>(c.len));
        out.push_back(std::move(cell));
      }
      col += w;
      p += c.len;
    }
  }

  // ---- The document: bytes + a line index ------------------------------------

  struct Doc
  {
    std::string data;
    std::vector<uint32_t> lineStart; // n+1 entries, the last is data.size()
    std::vector<int> lineCols;       // n entries
    int maxCols = 0;
    size_t n = 0; // number of lines (an empty file has one empty line)

    void build(std::string &&bytes)
    {
      data = std::move(bytes);
      if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
          static_cast<unsigned char>(data[1]) == 0xBB &&
          static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
      lineStart.clear();
      lineStart.push_back(0);
      size_t p = 0;
      while (p < data.size())
      {
        const void *q = std::memchr(data.data() + p, '\n', data.size() - p);
        if (!q)
          break;
        const size_t pos = static_cast<size_t>(static_cast<const char *>(q) - data.data());
        if (pos + 1 < data.size()) // a final "\n" doesn't start another line
          lineStart.push_back(static_cast<uint32_t>(pos + 1));
        p = pos + 1;
      }
      lineStart.push_back(static_cast<uint32_t>(data.size()));
      n = lineStart.size() - 1;
      lineCols.assign(n, 0);
      maxCols = 0;
      for (size_t i = 0; i < n; ++i)
      {
        lineCols[i] = columnsOf(line(i));
        maxCols = std::max(maxCols, lineCols[i]);
      }
    }

    std::string_view line(size_t i) const
    {
      size_t b = lineStart[i], e = lineStart[i + 1];
      if (e > b && data[e - 1] == '\n')
        --e;
      if (e > b && data[e - 1] == '\r')
        --e;
      return std::string_view(data.data() + b, e - b);
    }

    size_t lineOfByte(size_t pos) const
    {
      auto it = std::upper_bound(lineStart.begin(), lineStart.begin() + static_cast<std::ptrdiff_t>(n),
                                 static_cast<uint32_t>(std::min<size_t>(pos, 0xFFFFFFFFu)));
      return static_cast<size_t>(it - lineStart.begin()) - 1;
    }
  };

  // ---- ASCII case-insensitive search ------------------------------------------

  char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }
  bool ieq(char a, char b) { return lower(a) == lower(b); }

  size_t ifind(std::string_view hay, const std::string &q, size_t from)
  {
    if (q.empty() || from >= hay.size())
      return std::string::npos;
    auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(from), hay.end(),
                          q.begin(), q.end(), ieq);
    return it == hay.end() ? std::string::npos : static_cast<size_t>(it - hay.begin());
  }

  // Last match that STARTS before `before`.
  size_t irfind(std::string_view hay, const std::string &q, size_t before)
  {
    if (q.empty() || before == 0)
      return std::string::npos;
    const size_t limit = std::min(hay.size(), before - 1 + q.size());
    auto it = std::find_end(hay.begin(), hay.begin() + static_cast<std::ptrdiff_t>(limit),
                            q.begin(), q.end(), ieq);
    return it == hay.begin() + static_cast<std::ptrdiff_t>(limit)
               ? std::string::npos
               : static_cast<size_t>(it - hay.begin());
  }

  // ---- Viewer state -----------------------------------------------------------
  // Views are *copied* by addChild(), so callbacks capture this by reference.

  struct Viewer
  {
    Doc doc;
    bool loaded = false;
    std::string title;
    std::string status = "No file loaded";
    std::string notice;
    std::chrono::steady_clock::time_point noticeUntil{};

    // Font metrics. Estimated until the first paint measures the real font.
    float fontSize = kDefaultFont;
    float measuredFor = -1.0f; // fontSize the metrics below belong to
    float charW = kDefaultFont * 0.6f;
    float lineH = std::round(kDefaultFont * 1.45f);
    float dpr = 1.0f;

    float viewW = 0, viewH = 0; // canvas size, from onLayout
    float scrollX = 0, scrollY = 0;
    // The Canvas's damage handle: markDirty() repaints all of it.
    std::shared_ptr<CanvasDamage> damage = std::make_shared<CanvasDamage>();
    void markDirty() { damage->invalidate(DirtyRect::everything()); }
    bool dropHover = false;

    // Word wrap: rowsBefore[i] = visual rows above logical line i (n+1 entries).
    bool wrap = false;
    bool wrapValid = false;
    int wrapCols = 0;
    std::vector<uint32_t> rowsBefore;
    long pendingTopLine = -1; // line to keep at the top across a wrap change

    // Search.
    std::string query;
    bool hasCur = false;
    size_t curByte = 0;
    long matchCount = -1; // -1 = not counted

    enum class Drag
    {
      None,
      VThumb,
      HThumb
    } drag = Drag::None;
    float dragPos = 0, dragScroll = 0;
  };

  struct Geo
  {
    float gutterW = 0, textX = 0;
    float availW = 0, availH = 0;
    float contentW = 0, contentH = 0;
    float maxX = 0, maxY = 0;
    bool vbar = false, hbar = false;
    int wrapCols = 0;
    size_t rows = 0;
  };

  std::string commas(size_t v)
  {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3)
      s.insert(static_cast<size_t>(i), ",");
    return s;
  }

  std::string baseName(const std::string &path)
  {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  void setNotice(Viewer &s, std::string msg)
  {
    s.notice = std::move(msg);
    s.noticeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    s.markDirty();
  }

  // Logical line shown on visual row `row`.
  size_t lineOfRow(const Viewer &s, size_t row)
  {
    if (!s.wrap || !s.wrapValid)
      return std::min(row, s.doc.n - 1);
    auto it = std::upper_bound(s.rowsBefore.begin(), s.rowsBefore.end(), static_cast<uint32_t>(row));
    size_t idx = static_cast<size_t>(it - s.rowsBefore.begin());
    idx = idx == 0 ? 0 : idx - 1;
    return std::min(idx, s.doc.n - 1);
  }

  // (Re)builds the wrap map for `cols` columns, keeping the top line in place.
  void ensureWrap(Viewer &s, int cols)
  {
    if (s.wrapValid && s.wrapCols == cols)
      return;
    size_t top = 0;
    if (s.pendingTopLine >= 0)
      top = static_cast<size_t>(s.pendingTopLine);
    else if (s.wrapValid)
      top = lineOfRow(s, static_cast<size_t>(std::max(0.0f, s.scrollY / s.lineH)));
    s.pendingTopLine = -1;

    s.rowsBefore.assign(s.doc.n + 1, 0);
    for (size_t i = 0; i < s.doc.n; ++i)
    {
      const int rows = std::max(1, (s.doc.lineCols[i] + cols - 1) / cols);
      s.rowsBefore[i + 1] = s.rowsBefore[i] + static_cast<uint32_t>(rows);
    }
    s.wrapCols = cols;
    s.wrapValid = true;
    s.scrollY = static_cast<float>(s.rowsBefore[std::min(top, s.doc.n - 1)]) * s.lineH;
  }

  Geo geometry(Viewer &s)
  {
    Geo g;
    if (!s.loaded)
      return g;
    int digits = 3;
    for (size_t v = s.doc.n; v >= 1000; v /= 10)
      ++digits;
    g.gutterW = std::ceil(static_cast<float>(digits) * s.charW) + 2 * kGutterPad;
    g.textX = g.gutterW + 8.0f;

    if (s.wrap)
    {
      // Wrap width assumes the vertical bar is present, which avoids the
      // circular "bar changes width changes rows changes bar" problem.
      g.wrapCols = std::max(1, static_cast<int>(std::floor((s.viewW - g.textX - kBar - 6.0f) / s.charW)));
      ensureWrap(s, g.wrapCols);
      g.rows = s.rowsBefore.back();
      g.contentW = 0;
    }
    else
    {
      g.rows = s.doc.n;
      g.contentW = (static_cast<float>(s.doc.maxCols) + 2.0f) * s.charW;
    }
    g.contentH = static_cast<float>(g.rows) * s.lineH;

    g.availW = s.viewW - g.textX;
    g.availH = s.viewH;
    if (s.wrap)
    {
      g.availW -= kBar;
      g.vbar = g.contentH > g.availH + 0.5f;
    }
    else
    {
      for (int pass = 0; pass < 2; ++pass) // a bar eats the other axis
      {
        g.availW = s.viewW - g.textX - (g.vbar ? kBar : 0.0f);
        g.availH = s.viewH - (g.hbar ? kBar : 0.0f);
        g.hbar = g.contentW > g.availW + 0.5f;
        g.vbar = g.contentH > g.availH + 0.5f;
      }
      g.availW = s.viewW - g.textX - (g.vbar ? kBar : 0.0f);
      g.availH = s.viewH - (g.hbar ? kBar : 0.0f);
    }
    g.availW = std::max(0.0f, g.availW);
    g.availH = std::max(0.0f, g.availH);
    g.maxX = std::max(0.0f, g.contentW - g.availW);
    g.maxY = std::max(0.0f, g.contentH - g.availH);
    return g;
  }

  void clampScroll(Viewer &s)
  {
    Geo g = geometry(s);
    s.scrollX = std::clamp(s.scrollX, 0.0f, g.maxX);
    s.scrollY = std::clamp(s.scrollY, 0.0f, g.maxY);
    if (s.wrap)
      s.scrollX = 0;
  }

  void scrollBy(Viewer &s, float dx, float dy)
  {
    s.scrollX += dx;
    s.scrollY += dy;
    clampScroll(s);
    s.markDirty();
  }

  // ---- Loading ---------------------------------------------------------------

  void installDoc(Viewer &s, std::string &&bytes, const std::string &title)
  {
    s.doc.build(std::move(bytes));
    s.loaded = true;
    s.title = title;
    s.scrollX = s.scrollY = 0;
    s.wrapValid = false;
    s.pendingTopLine = 0;
    s.hasCur = false;
    s.matchCount = -1;
    s.markDirty();
  }

  bool loadFile(Viewer &s, const std::string &path)
  {
    namespace fs = std::filesystem;
    // Build the path from UTF-8 explicitly: a plain std::string path would be
    // read in the ANSI code page on Windows.
    const fs::path p{std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size())};
    std::error_code ec;
    if (fs::is_directory(p, ec))
    {
      setNotice(s, baseName(path) + " is a folder");
      return false;
    }
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f)
    {
      setNotice(s, "Could not open " + baseName(path));
      return false;
    }
    const std::streamoff sz = f.tellg();
    if (sz < 0 || static_cast<unsigned long long>(sz) > kMaxFileBytes)
    {
      setNotice(s, "Could not read " + baseName(path) + " (too large, limit 512 MB)");
      return false;
    }
    std::string bytes(static_cast<size_t>(sz), '\0');
    f.seekg(0);
    if (sz > 0 && !f.read(bytes.data(), sz))
    {
      setNotice(s, "Read error on " + baseName(path));
      return false;
    }
    if (std::memchr(bytes.data(), 0, std::min<size_t>(bytes.size(), 8192)))
    {
      setNotice(s, baseName(path) + " looks binary (or UTF-16), not opened");
      return false;
    }
    installDoc(s, std::move(bytes), baseName(path));
    s.status = path;
    return true;
  }

  void openDropped(Viewer &s, const DropEvent &e)
  {
    s.dropHover = false;
    s.markDirty();
    for (const std::string &p : e.paths)
      if (loadFile(s, p))
      {
        setNotice(s, "Opened " + baseName(p));
        return;
      }
    if (e.paths.empty() && !e.text.empty())
    {
      installDoc(s, std::string(e.text), "Dropped text");
      s.status = "Dropped text";
      setNotice(s, "Showing dropped text");
    }
  }

  // ---- Font zoom ---------------------------------------------------------------

  void setFontSize(Viewer &s, float px)
  {
    px = std::clamp(std::round(px), kMinFont, kMaxFont);
    if (px == s.fontSize)
      return;
    const float k = px / s.fontSize;
    s.fontSize = px;
    s.measuredFor = -1.0f; // re-measure on the next paint
    s.charW *= k;          // estimate until then
    s.lineH = std::round(px * 1.45f);
    s.scrollX *= k;
    s.scrollY *= k;
    s.wrapValid = false; // columns per row changed
    s.pendingTopLine = -1;
    clampScroll(s);
    s.markDirty();
  }

  void toggleWrap(Viewer &s)
  {
    if (s.loaded)
      s.pendingTopLine = static_cast<long>(
          lineOfRow(s, static_cast<size_t>(std::max(0.0f, s.scrollY / s.lineH))));
    s.wrap = !s.wrap;
    s.wrapValid = false;
    s.scrollX = 0;
    if (!s.wrap && s.loaded)
      s.scrollY = static_cast<float>(std::max(0L, s.pendingTopLine)) * s.lineH;
    s.pendingTopLine = s.wrap ? s.pendingTopLine : -1;
    clampScroll(s);
    s.markDirty();
  }

  // ---- Search ------------------------------------------------------------------

  void recount(Viewer &s)
  {
    s.matchCount = -1;
    if (s.query.empty() || !s.loaded || s.doc.data.size() > kCountLimitBytes)
      return;
    long n = 0;
    size_t pos = 0;
    while ((pos = ifind(s.doc.data, s.query, pos)) != std::string::npos && n < 1000000)
    {
      ++n;
      pos += s.query.size();
    }
    s.matchCount = n;
  }

  void revealByte(Viewer &s, size_t pos)
  {
    Geo g = geometry(s);
    const size_t line = s.doc.lineOfByte(pos);
    const int col = columnsOf(s.doc.line(line), pos - s.doc.lineStart[line]);
    const size_t row = s.wrap ? s.rowsBefore[line] + static_cast<size_t>(col / std::max(1, g.wrapCols))
                              : line;
    const float top = static_cast<float>(row) * s.lineH;
    if (top < s.scrollY || top + s.lineH > s.scrollY + g.availH)
      s.scrollY = top - g.availH / 2.0f + s.lineH / 2.0f;
    if (!s.wrap)
    {
      const float x = static_cast<float>(col) * s.charW;
      const float len = static_cast<float>(s.query.size()) * s.charW;
      if (x < s.scrollX || x + len > s.scrollX + g.availW)
        s.scrollX = x - g.availW / 3.0f;
    }
    clampScroll(s);
    s.markDirty();
  }

  void findNext(Viewer &s, bool forward)
  {
    if (!s.loaded || s.query.empty())
      return;
    const std::string_view hay = s.doc.data;
    size_t from;
    if (s.hasCur)
      from = forward ? s.curByte + 1 : s.curByte;
    else
      from = s.doc.lineStart[lineOfRow(s, static_cast<size_t>(std::max(0.0f, s.scrollY / s.lineH)))];
    size_t pos = forward ? ifind(hay, s.query, from) : irfind(hay, s.query, from);
    bool wrapped = false;
    if (pos == std::string::npos)
    {
      wrapped = true;
      pos = forward ? ifind(hay, s.query, 0) : irfind(hay, s.query, hay.size() + 1);
    }
    if (pos == std::string::npos)
    {
      s.hasCur = false;
      setNotice(s, "No matches for \"" + s.query + "\"");
      return;
    }
    s.hasCur = true;
    s.curByte = pos;
    if (wrapped)
      setNotice(s, "Search wrapped around");
    revealByte(s, pos);
  }

  // ---- Painting ----------------------------------------------------------------

  struct Thumb
  {
    float pos, len;
  };
  Thumb thumbFor(float track, float content, float scroll, float maxScroll)
  {
    float len = std::clamp(track * track / std::max(1.0f, content), kMinThumb, track);
    float pos = maxScroll > 0 ? scroll / maxScroll * (track - len) : 0.0f;
    return {pos, len};
  }

  const Color kBg{30, 30, 30}, kFg{212, 212, 212};
  const Color kGutterBg{37, 37, 38}, kGutterFg{133, 133, 133};
  const Color kMatch{105, 90, 25}, kMatchCur{190, 105, 20};
  const Color kTrack{45, 45, 48}, kThumbC{110, 110, 115};

  void paintViewer(Viewer &s, CanvasContext &ctx)
  {
    const float w = ctx.width(), h = ctx.height();
    s.dpr = ctx.devicePixelRatio();
    ctx.setFillColor(kBg);
    ctx.fillRect(0, 0, w, h);

    ctx.setFont(kMono, s.fontSize);
    if (s.measuredFor != s.fontSize) // learn the real cell size once per font size
    {
      const float cw = ctx.measureText("0123456789").width / 10.0f;
      if (cw > 1.0f)
        s.charW = cw;
      s.measuredFor = s.fontSize;
      s.wrapValid = false;
      clampScroll(s);
    }

    if (!s.loaded)
    {
      ctx.setFillColor(Color{150, 150, 150});
      ctx.setFont("", 15);
      ctx.setTextAlign(TextAlign::Center);
      ctx.setTextBaseline(TextBaseline::Middle);
      ctx.fillText("Click \"Open...\" (Ctrl+O) or drop a text file here.", w / 2, h / 2);
    }
    else
    {
      Geo g = geometry(s);
      const float d = s.dpr;
      const size_t firstRow = static_cast<size_t>(std::max(0.0f, std::floor(s.scrollY / s.lineH)));
      const size_t lastRow = std::min(g.rows, static_cast<size_t>(std::ceil((s.scrollY + g.availH) / s.lineH)) + 1);

      // ---- text area (clipped, so long lines never touch the gutter) ----
      ctx.save();
      ctx.beginPath();
      ctx.rect(g.textX, 0, g.availW, g.availH);
      ctx.clip();
      ctx.setFont(kMono, s.fontSize);
      ctx.setTextAlign(TextAlign::Start);
      ctx.setTextBaseline(TextBaseline::Top);

      std::vector<Cell> cells;
      std::string run;
      for (size_t row = firstRow; row < lastRow; ++row)
      {
        const size_t line = lineOfRow(s, row);
        const int sub = s.wrap ? static_cast<int>(row - s.rowsBefore[line]) : 0;
        const std::string_view sv = s.doc.line(line);
        const int c0 = s.wrap ? sub * g.wrapCols
                              : std::max(0, static_cast<int>(std::floor(s.scrollX / s.charW)));
        const int c1 = s.wrap ? c0 + g.wrapCols
                              : c0 + static_cast<int>(std::ceil(g.availW / s.charW)) + 2;
        // x of column c is baseX + c * charW (wrap: columns restart per row).
        const float baseX = s.wrap ? g.textX - static_cast<float>(c0) * s.charW : g.textX - s.scrollX;
        const float y = std::round((static_cast<float>(row) * s.lineH - s.scrollY) * d) / d;
        buildCells(sv, c0, c1, cells);
        if (cells.empty())
          continue;

        // Search highlights, found inside the visible byte range only.
        if (!s.query.empty())
        {
          const size_t lo = cells.front().off;
          const size_t hi = std::min(sv.size(), static_cast<size_t>(cells.back().off) + 4);
          const std::string_view seg = sv.substr(lo, hi - lo);
          size_t pos = 0;
          while ((pos = ifind(seg, s.query, pos)) != std::string::npos)
          {
            const size_t mb = lo + pos, me = mb + s.query.size();
            auto cmp = [](size_t v, const Cell &c)
            { return v < c.off; };
            auto a = std::upper_bound(cells.begin(), cells.end(), mb, cmp);
            auto b = std::upper_bound(cells.begin(), cells.end(), me - 1, cmp);
            if (a != cells.begin() && b != cells.begin())
            {
              const Cell &ca = *(a - 1);
              const Cell &cb = *(b - 1);
              const bool cur = s.hasCur && line == s.doc.lineOfByte(s.curByte) &&
                               mb == s.curByte - s.doc.lineStart[line];
              ctx.setFillColor(cur ? kMatchCur : kMatch);
              ctx.fillRect(baseX + static_cast<float>(ca.col) * s.charW, y,
                           static_cast<float>(cb.col + cb.w - ca.col) * s.charW, s.lineH);
            }
            pos += std::max<size_t>(1, s.query.size());
          }
        }

        // Text: plain-ASCII runs in one fillText, other cells one by one.
        ctx.setFillColor(kFg);
        run.clear();
        int runCol = 0, nextCol = 0;
        auto flush = [&]
        {
          if (!run.empty())
            ctx.fillText(run, baseX + static_cast<float>(runCol) * s.charW, y);
          run.clear();
        };
        for (const Cell &c : cells)
        {
          if (c.text.empty()) // tab gap
          {
            flush();
            continue;
          }
          const bool ascii = c.w == 1 && c.text.size() == 1 &&
                             static_cast<unsigned char>(c.text[0]) < 0x80;
          if (ascii)
          {
            if (!run.empty() && c.col != nextCol)
              flush();
            if (run.empty())
              runCol = c.col;
            run += c.text;
            nextCol = c.col + 1;
          }
          else
          {
            flush();
            ctx.fillText(c.text, baseX + static_cast<float>(c.col) * s.charW, y);
          }
        }
        flush();
      }
      ctx.restore();

      // ---- gutter ----
      ctx.setFillColor(kGutterBg);
      ctx.fillRect(0, 0, g.gutterW, g.availH);
      ctx.setFillColor(kGutterFg);
      ctx.setFont(kMono, s.fontSize);
      ctx.setTextAlign(TextAlign::End);
      ctx.setTextBaseline(TextBaseline::Top);
      for (size_t row = firstRow; row < lastRow; ++row)
      {
        const size_t line = lineOfRow(s, row);
        if (s.wrap && row != s.rowsBefore[line]) // number only the first row of a line
          continue;
        const float y = std::round((static_cast<float>(row) * s.lineH - s.scrollY) * d) / d;
        ctx.fillText(std::to_string(line + 1), g.gutterW - kGutterPad, y);
      }

      // ---- scrollbars ----
      if (g.vbar)
      {
        ctx.setFillColor(kTrack);
        ctx.fillRect(w - kBar, 0, kBar, g.availH);
        Thumb t = thumbFor(g.availH, g.contentH, s.scrollY, g.maxY);
        ctx.setFillColor(kThumbC);
        ctx.beginPath();
        ctx.roundRect(w - kBar + 2, t.pos + 1, kBar - 4, t.len - 2, 3);
        ctx.fill();
      }
      if (g.hbar)
      {
        ctx.setFillColor(kTrack);
        ctx.fillRect(g.textX, h - kBar, g.availW, kBar);
        Thumb t = thumbFor(g.availW, g.contentW, s.scrollX, g.maxX);
        ctx.setFillColor(kThumbC);
        ctx.beginPath();
        ctx.roundRect(g.textX + t.pos + 1, h - kBar + 2, t.len - 2, kBar - 4, 3);
        ctx.fill();
      }
      if (g.vbar && g.hbar)
      {
        ctx.setFillColor(kTrack);
        ctx.fillRect(w - kBar, h - kBar, kBar, kBar);
      }
    }

    if (s.dropHover)
    {
      ctx.setFillColor(Color{0, 110, 200, 60});
      ctx.fillRect(0, 0, w, h);
      ctx.setStrokeColor(Color{60, 160, 255});
      ctx.setLineWidth(4);
      ctx.strokeRect(2, 2, w - 4, h - 4);
    }
  }

  // ---- Pointer -------------------------------------------------------------------

  void pressAt(Viewer &s, float x, float y)
  {
    s.drag = Viewer::Drag::None;
    if (!s.loaded)
      return;
    Geo g = geometry(s);
    if (g.vbar && x >= s.viewW - kBar && y < g.availH)
    {
      Thumb t = thumbFor(g.availH, g.contentH, s.scrollY, g.maxY);
      if (y >= t.pos && y <= t.pos + t.len)
      {
        s.drag = Viewer::Drag::VThumb;
        s.dragPos = y;
        s.dragScroll = s.scrollY;
      }
      else // click on the track: one page up/down
        scrollBy(s, 0, y < t.pos ? -g.availH : g.availH);
    }
    else if (g.hbar && y >= s.viewH - kBar && x >= g.textX && x < g.textX + g.availW)
    {
      Thumb t = thumbFor(g.availW, g.contentW, s.scrollX, g.maxX);
      const float lx = x - g.textX;
      if (lx >= t.pos && lx <= t.pos + t.len)
      {
        s.drag = Viewer::Drag::HThumb;
        s.dragPos = x;
        s.dragScroll = s.scrollX;
      }
      else
        scrollBy(s, lx < t.pos ? -g.availW : g.availW, 0);
    }
  }

  void dragTo(Viewer &s, float x, float y)
  {
    if (!s.loaded || s.drag == Viewer::Drag::None)
      return;
    Geo g = geometry(s);
    if (s.drag == Viewer::Drag::VThumb)
    {
      Thumb t = thumbFor(g.availH, g.contentH, s.scrollY, g.maxY);
      const float range = g.availH - t.len;
      if (range > 0)
        s.scrollY = s.dragScroll + (y - s.dragPos) / range * g.maxY;
    }
    else
    {
      Thumb t = thumbFor(g.availW, g.contentW, s.scrollX, g.maxX);
      const float range = g.availW - t.len;
      if (range > 0)
        s.scrollX = s.dragScroll + (x - s.dragPos) / range * g.maxX;
    }
    clampScroll(s);
    s.markDirty();
  }

  // ---- UI helpers (same shape as imageviewer's) ----------------------------------

  View makeButton(std::function<std::string()> label, Color bg, Color hover, PointerCallback onClick)
  {
    Text t;
    t.label = std::move(label);
    t.fontSize = 14;
    t.color = Color{255, 255, 255};
    View b;
    b.style.padding = EdgeInsets{6, 14, 6, 14};
    b.style.backgroundColor = bg;
    b.style.hoverColor = hover;
    b.style.borderRadius = 4.0f;
    b.style.flexShrink = 0;
    b.onClick = std::move(onClick);
    b.addChild(t);
    return b;
  }

  View makeToolbar(float height)
  {
    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.gap = 10;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(height);
    bar.style.flexShrink = 0;
    bar.style.padding = EdgeInsets{0, 12, 0, 12};
    bar.style.backgroundColor = Color{45, 45, 48};
    return bar;
  }
} // namespace

int main(int argc, char **argv)
{
  Viewer state;
  if (argc > 1)
    loadFile(state, argv[1]);

  const Color blue{0, 110, 200}, blueHover{20, 130, 225};
  const Color grey{70, 70, 75}, greyHover{90, 90, 96};

  auto openDialog = [&state]
  {
    auto path = openFilePicker("Open Text File", {{"Text", "*.txt;*.log;*.md;*.cpp;*.hpp;*.h;*.c;*.json;*.xml;*.csv;*.cmake"},
                                                  {"All files", "*"}});
    if (path)
      loadFile(state, *path);
  };

  // ---- Toolbar: Open, Wrap, A-, A+, search box, Prev, Next, match info ---------
  View toolbar = makeToolbar(48);
  toolbar.addChild(makeButton([]
                              { return std::string("Open..."); },
                              blue, blueHover, openDialog));
  toolbar.addChild(makeButton([&state]
                              { return std::string("Wrap: ") + (state.wrap ? "on" : "off"); },
                              grey, greyHover, [&state]
                              { toggleWrap(state); }));
  toolbar.addChild(makeButton([]
                              { return std::string("A-"); },
                              grey, greyHover, [&state]
                              { setFontSize(state, state.fontSize - 1); }));
  toolbar.addChild(makeButton([]
                              { return std::string("A+"); },
                              grey, greyHover, [&state]
                              { setFontSize(state, state.fontSize + 1); }));

  TextInput search;
  search.placeholder = "Find (Ctrl+F)";
  search.fontSize = 14;
  search.textColor = Color{230, 230, 230};
  search.caretColor = Color{230, 230, 230};
  search.placeholderColor = Color{130, 130, 130};
  search.borderColor = Color{90, 90, 95};
  search.style.width = Size::pixel(240);
  search.style.height = Size::pixel(30);
  search.style.backgroundColor = Color{30, 30, 30};
  auto searchState = search.state; // shared with the copy addChild makes
  search.onChange = [&state](const std::string &t)
  {
    // Incremental search: re-count and jump to the first match from the top of the view.
    state.query = t;
    state.hasCur = false;
    recount(state);
    findNext(state, true);
    state.markDirty();
  };
  search.onSubmit = [&state](const std::string &)
  { findNext(state, true); };
  toolbar.addChild(std::move(search));

  toolbar.addChild(makeButton([]
                              { return std::string("Prev"); },
                              grey, greyHover, [&state]
                              { findNext(state, false); }));
  toolbar.addChild(makeButton([]
                              { return std::string("Next"); },
                              grey, greyHover, [&state]
                              { findNext(state, true); }));

  Text matchText;
  matchText.label = std::function<std::string()>(
      [&state]
      {
        if (state.query.empty())
          return std::string();
        if (state.matchCount < 0)
          return std::string("(count off for big files)");
        return commas(static_cast<size_t>(state.matchCount)) + (state.matchCount == 1 ? " match" : " matches");
      });
  matchText.fontSize = 14;
  matchText.color = Color{200, 200, 200};
  toolbar.addChild(matchText);

  // ---- Info row ------------------------------------------------------------------
  View infoBar = makeToolbar(28);
  Text infoText;
  infoText.label = std::function<std::string()>(
      [&state]
      {
        if (!state.notice.empty() && std::chrono::steady_clock::now() < state.noticeUntil)
          return state.notice;
        if (!state.loaded)
          return state.status;
        char sz[32];
        std::snprintf(sz, sizeof sz, "%.1f KB", static_cast<double>(state.doc.data.size()) / 1024.0);
        const size_t topLine = lineOfRow(state, static_cast<size_t>(std::max(0.0f, state.scrollY / state.lineH))) + 1;
        return state.title + "  -  Ln " + commas(topLine) + " / " + commas(state.doc.n) + "  -  " + sz +
               "  -  " + std::to_string(static_cast<int>(state.fontSize)) + " px";
      });
  infoText.fontSize = 13;
  infoText.color = Color{200, 200, 200};
  infoBar.addChild(infoText);

  // ---- Canvas ----------------------------------------------------------------------
  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = Size::full();
  canvas.style.flexGrow = 1;
  canvas.focusable = true; // click the text to give it keyboard focus
  canvas.onPaint = [&state](CanvasContext &ctx)
  { paintViewer(state, ctx); };
  canvas.canvasDamage = state.damage;
  canvas.onLayout = [&state](float, float, float w, float h)
  {
    state.viewW = w;
    state.viewH = h;
    clampScroll(state);
    state.markDirty();
  };
  canvas.onWheel = [&state](WheelEvent &e)
  {
    if (e.mods.ctrl) // Ctrl+wheel: font zoom
    {
      if (e.dy != 0)
        setFontSize(state, state.fontSize + (e.dy < 0 ? 1 : -1));
    }
    else
    {
      auto step = [&](float d)
      { return e.precise ? d : (d == 0 ? 0.0f : (d < 0 ? -3.0f : 3.0f) * state.lineH); };
      float dx = step(e.dx), dy = step(e.dy);
      if (e.mods.shift && dx == 0) // Shift+wheel scrolls sideways
        std::swap(dx, dy);
      scrollBy(state, dx, dy);
    }
    e.stopPropagation(); // also suppresses the built-in overflow scrolling
  };
  canvas.onPointerDown = [&state](PointerEvent &e)
  {
    if (e.button == MouseButton::Left)
      pressAt(state, e.x, e.y);
  };
  canvas.onPointerMove = [&state](PointerEvent &e)
  {
    if (e.held(MouseButton::Left))
      dragTo(state, e.x, e.y);
  };
  canvas.onPointerUp = [&state](PointerEvent &e)
  {
    if (e.button == MouseButton::Left)
      state.drag = Viewer::Drag::None;
  };
  canvas.onPointerCancel = [&state](PointerEvent &)
  { state.drag = Viewer::Drag::None; };
  canvas.onDragEnter = [&state](DropEvent &e)
  {
    state.dropHover = e.hasFiles || e.hasText;
    state.markDirty();
  };
  canvas.onDragLeave = [&state](DropEvent &)
  {
    state.dropHover = false;
    state.markDirty();
  };
  canvas.onDrop = [&state](DropEvent &e)
  { openDropped(state, e); };
  canvas.onKeyDown = [&state](KeyEvent e)
  {
    Geo g = geometry(state);
    const float page = std::max(state.lineH, g.availH - state.lineH);
    switch (e.key)
    {
    case Key::Up:
      scrollBy(state, 0, -state.lineH);
      break;
    case Key::Down:
      scrollBy(state, 0, state.lineH);
      break;
    case Key::PageUp:
      scrollBy(state, 0, -page);
      break;
    case Key::PageDown:
      scrollBy(state, 0, page);
      break;
    case Key::Left:
      scrollBy(state, -4 * state.charW, 0);
      break;
    case Key::Right:
      scrollBy(state, 4 * state.charW, 0);
      break;
    case Key::Home:
      if (e.mods.ctrl)
        state.scrollY = 0;
      state.scrollX = 0;
      clampScroll(state);
      state.markDirty();
      break;
    case Key::End:
      if (e.mods.ctrl)
      {
        state.scrollY = g.maxY;
        clampScroll(state);
        state.markDirty();
      }
      break;
    default:
      break;
    }
  };

  // ---- Window ----------------------------------------------------------------------
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(std::move(toolbar));
  root.addChild(std::move(infoBar));
  root.addChild(std::move(canvas));

  LiteUI ui("Text Viewer", 1000, 700);
  ui.setRoot(root);
  ui.setFileDropHandler([&state](DropEvent &e)
                        { openDropped(state, e); }); // drops on the toolbars

  {
    KeyModifiers ctrl, alt, shift;
    ctrl.ctrl = true;
    alt.alt = true;
    shift.shift = true;
    ui.addShortcut(ctrl, Key::O, openDialog);
    ui.addShortcut(ctrl, Key::F, [&state, searchState, &ui]
                   {
      searchState->requestFocus = true;
      state.markDirty();
      ui.requestRepaint(); });
    ui.addShortcut(KeyModifiers{}, Key::F3, [&state]
                   { findNext(state, true); });
    ui.addShortcut(shift, Key::F3, [&state]
                   { findNext(state, false); });
    ui.addShortcut(alt, Key::Z, [&state]
                   { toggleWrap(state); });
    ui.addShortcut(ctrl, Key::Equal, [&state]
                   { setFontSize(state, state.fontSize + 1); });
    ui.addShortcut(ctrl, Key::Minus, [&state]
                   { setFontSize(state, state.fontSize - 1); });
    ui.addShortcut(ctrl, Key::N0, [&state]
                   { setFontSize(state, kDefaultFont); });
  }
  ui.run(); // blocks until the window closes
  return 0;
}