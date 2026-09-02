// include/liteui.hpp
//
// Single-header, PImpl-free cross-platform window.
// Platform members/methods are selected at compile time via #ifdef,
// so there's exactly one LiteUI definition per build — no vtable,
// no heap-allocated Impl, no indirection through a pointer.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "xdg-decoration-client-protocol.h" // Generated client bindings for the xdg-decoration protocol (server-side vs client-side decorations).
#include "xdg-shell-client-protocol.h" // Generated client bindings for the xdg-shell protocol (toplevel windows, configure events).
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h> // Core Wayland client protocol: displays, registries, surfaces, shm.
#include <wayland-cursor.h> // wl_cursor_theme_load / wl_cursor_theme_get_cursor, for showing resize/arrow cursors.
#endif

// Plain RGB color, one byte per channel.
struct Color {
  uint8_t r = 0, g = 0, b = 0;
};

// A static, axis-aligned filled rectangle the caller wants drawn on the
// window. Position is in window-local pixel coordinates, (0,0) at top-left.
struct Box {
  int width = 0;
  int height = 0;
  int pos_x = 0;
  int pos_y = 0;
  Color color;
};

// ---------------- Layout engine: Size / Style / View ----------------

// A size along one axis. Fixed/Percentage are self-explanatory; Fit sizes
// to the sum/max of children (like CSS's "auto" on a flex item); Full fills
// whatever space the parent gives along that axis (like width: 100%, but
// resolved from available space rather than the parent's own size).
struct Size {
  enum class Kind { Fixed, Percentage, Fit, Full };
  Kind kind = Kind::Fit;
  float value = 0; // pixels for Fixed, 0-100 for Percentage; unused otherwise

  static Size pixel(float v) { return {Kind::Fixed, v}; }
  static Size percentage(float v) { return {Kind::Percentage, v}; }
  static Size fit() { return {Kind::Fit, 0}; }
  static Size full() { return {Kind::Full, 0}; }
};

enum class FlexDirection { Row, Column };
enum class Justify {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly
};

enum class Align { Start, End, Center, Stretch };

// Whether children that overflow the main axis wrap onto additional lines.
enum class FlexWrap { NoWrap, Wrap };

// How multiple flex lines are distributed along the cross axis. Only
// meaningful when FlexWrap::Wrap actually produces more than one line;
// with a single line this reduces to Stretch filling crossAvail (matching
// the old un-wrapped behavior) or the others packing that one line at the
// start.
enum class AlignContent {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch
};

struct EdgeInsets {
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v) { return {v, v, v, v}; }
};

enum class Position { Static, Absolute };

struct Style {
  Size width = Size::fit();
  Size height = Size::fit();

  // Clamp bounds applied (in pixels) after width/height above are resolved.
  // Defaults impose no constraint. maxWidth < minWidth (or the height
  // equivalent) is treated as maxWidth == minWidth rather than producing a
  // negative range.
  float minWidth = 0;
  float maxWidth = std::numeric_limits<float>::infinity();
  float minHeight = 0;
  float maxHeight = std::numeric_limits<float>::infinity();

  EdgeInsets margin;
  EdgeInsets padding;

  FlexDirection direction = FlexDirection::Row;
  Justify justifyContent = Justify::Start;
  Align alignItems = Align::Stretch;
  float gap = 0;
  FlexWrap flexWrap = FlexWrap::NoWrap;
  AlignContent alignContent = AlignContent::Stretch;

  float flexGrow = 0;
  float flexShrink = 1;

  Color backgroundColor{255, 255, 255};
  float borderWidth = 0;
  Color borderColor{0, 0, 0};
  float borderRadius = 0;

  // Absolute children are pulled out of flex distribution entirely and
  // placed against the parent's content box using left/top/right/bottom.
  // NaN means "unset" for each edge. If width/height is Fit and both
  // opposing edges are set, size is derived from them (contentW - left -
  // right); otherwise size resolves normally (Fixed/Percentage/Fit/Full)
  // against the containing block, same as an in-flow child would.
  Position position = Position::Static;
  float left = std::numeric_limits<float>::quiet_NaN();
  float top = std::numeric_limits<float>::quiet_NaN();
  float right = std::numeric_limits<float>::quiet_NaN();
  float bottom = std::numeric_limits<float>::quiet_NaN();
  // Stacking order among all Absolute nodes tree-wide (not just siblings).
  // Ties break by document order — see collectAbsolutes().
  int zIndex = 0;
};

// A node in the retained layout tree. Set `style` and `children`; the engine
// fills in `computed` (absolute window pixel coordinates) during layout.
// Renderers only ever read `computed`, never re-derive it from `style`.
class View {
public:
  Style style;
  std::vector<View> children;

  // Fired on a left-click whose point lands on this view (see LiteUI::hitTest).
  // Bubbles: if a deeper view under the point has no handler, the nearest
  // containing ancestor's onClick fires instead.
  std::function<void()> onClick;

  struct Computed {
    float x = 0, y = 0, w = 0, h = 0; // border-box, absolute window coords
  } computed;

  void addChild(View child) { children.push_back(std::move(child)); }
};

// ---------------- Layout algorithm (flexbox subset) ----------------
//
// Two passes per subtree:
//  1. measureNatural() — bottom-up. Resolves a node's own border-box size:
//     Fixed/Percentage/Full resolve directly against the parent's available
//     space; Fit sums/maxes its children's own natural sizes.
//  2. placeNode() — top-down. Given a node's already-decided final box (from
//     step 1 at the root, or from the parent's flex distribution below it),
//     computes each child's basis size, distributes leftover space via
//     flexGrow/flexShrink, positions via justifyContent/alignItems, and
//     recurses.
//

// Known limitations (fine for v1, revisit if needed): percentages resolve to
// 0 when an ancestor's size is itself Fit (indefinite). flex-wrap only
// affects placeNode's placement pass — measureNatural's Fit sizing still
// assumes a single line, since a Fit main axis has no definite width to
// wrap against in the first place; give the container a definite/Full main
// size if you want Fit-height wrapping content to actually wrap.
namespace liteui_layout {

struct Natural {
  float w, h;
};

inline float resolveAxis(const Size &s, float available, bool definite,
                         float fitValue) {
  switch (s.kind) {
  case Size::Kind::Fixed:
    return s.value;
  case Size::Kind::Percentage:
    return definite ? available * s.value / 100.0f : fitValue;
  case Size::Kind::Full:
    return definite ? available : fitValue;
  case Size::Kind::Fit:
    return fitValue;
  }
  return fitValue;
}

// Clamps a resolved axis size into [minV, maxV], guarding against a
// misconfigured maxV < minV by falling back to minV.
inline float clampSize(float v, float minV, float maxV) {
  return std::clamp(v, minV, std::max(minV, maxV));
}

inline Natural measureNatural(const View &node, float availW, float availH,
                              bool wDefinite, bool hDefinite) {
  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  bool needW = node.style.width.kind == Size::Kind::Fit;
  bool needH = node.style.height.kind == Size::Kind::Fit;

  float w = clampSize(resolveAxis(node.style.width, availW, wDefinite, 0),
                      node.style.minWidth, node.style.maxWidth);
  float h = clampSize(resolveAxis(node.style.height, availH, hDefinite, 0),
                      node.style.minHeight, node.style.maxHeight);
  if ((!needW && !needH) || node.children.empty())
    return {w, h};

  float innerW =
      (wDefinite && !needW) ? std::max(0.0f, w - pad.left - pad.right) : availW;
  float innerH =
      (hDefinite && !needH) ? std::max(0.0f, h - pad.top - pad.bottom) : availH;

  float mainTotal = 0, crossMax = 0;
  bool firstFlow = true;
  for (size_t i = 0; i < node.children.size(); ++i) {
    const View &c = node.children[i];
    if (c.style.position == Position::Absolute)
      continue; // out of flow: doesn't affect the parent's Fit size at all
    Natural cn = measureNatural(c, innerW, innerH, wDefinite || !needW,
                                hDefinite || !needH);
    float mm = c.style.margin.left + c.style.margin.right;
    float mv = c.style.margin.top + c.style.margin.bottom;
    float childMain = horizontal ? cn.w + mm : cn.h + mv;
    float childCross = horizontal ? cn.h + mv : cn.w + mm;
    if (!firstFlow)
      mainTotal += node.style.gap;
    mainTotal += childMain;
    crossMax = std::max(crossMax, childCross);
    firstFlow = false;
  }
  if (needW)
    w = clampSize((horizontal ? mainTotal : crossMax) + pad.left + pad.right,
                  node.style.minWidth, node.style.maxWidth);

  if (needH)
    h = clampSize((horizontal ? crossMax : mainTotal) + pad.top + pad.bottom,
                  node.style.minHeight, node.style.maxHeight);
  return {w, h};
}

inline void placeNode(View &node, float x, float y, float w, float h) {
  node.computed = {x, y, w, h};
  if (node.children.empty())
    return;

  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  float contentX = x + pad.left, contentY = y + pad.top;
  float contentW = std::max(0.0f, w - pad.left - pad.right);
  float contentH = std::max(0.0f, h - pad.top - pad.bottom);
  float mainAvail = horizontal ? contentW : contentH;
  float crossAvail = horizontal ? contentH : contentW;

  size_t n = node.children.size();
  bool wrap = node.style.flexWrap == FlexWrap::Wrap;
  std::vector<size_t> flowIdx;
  flowIdx.reserve(node.children.size());
  for (size_t i = 0; i < node.children.size(); ++i)
    if (node.children[i].style.position != Position::Absolute)
      flowIdx.push_back(i);
  n = flowIdx.size();
  std::vector<float> basis(n), cross(n), mMainS(n), mMainE(n), mCrossS(n),
      mCrossE(n), minMain(n), maxMain(n), marginMain(n);

  for (size_t k = 0; k < n; ++k) {
    const View &c = node.children[flowIdx[k]];
    Natural cn = measureNatural(c, contentW, contentH, true, true);
    basis[k] = horizontal ? cn.w : cn.h;
    cross[k] = horizontal ? cn.h : cn.w;
    mMainS[k] = horizontal ? c.style.margin.left : c.style.margin.top;
    mMainE[k] = horizontal ? c.style.margin.right : c.style.margin.bottom;
    mCrossS[k] = horizontal ? c.style.margin.top : c.style.margin.left;
    mCrossE[k] = horizontal ? c.style.margin.bottom : c.style.margin.right;
    minMain[k] = horizontal ? c.style.minWidth : c.style.minHeight;
    maxMain[k] = horizontal ? c.style.maxWidth : c.style.maxHeight;
    marginMain[k] = mMainS[k] + mMainE[k];
  }

  // ---- Line breaking ----
  // With wrap disabled this is always one line spanning every child (the
  // original single-line behavior, byte-for-byte). With wrap enabled,
  // children are greedily packed onto a line until the next child's basis
  // would overflow mainAvail, at which point a new line starts. A line
  // always takes at least one child, even an oversized one, so a single
  // giant child can't stall the packer.
  struct Line {
    size_t begin, end; // half-open [begin, end) into node.children
  };
  std::vector<Line> lines;
  if (!wrap) {
    lines.push_back({0, n});
  } else {
    size_t start = 0;
    float used = 0;
    for (size_t i = 0; i < n; ++i) {
      float itemMain = basis[i] + marginMain[i];
      float withGap = (i > start) ? node.style.gap : 0.0f;
      if (i > start && used + withGap + itemMain > mainAvail) {
        lines.push_back({start, i});
        start = i;
        used = itemMain;
      } else {
        used += withGap + itemMain;
      }
    }
    lines.push_back({start, n});
  }

  // ---- Per-line main-axis flex resolution ----
  // Resolve flexGrow/flexShrink into final main-axis sizes, honoring each
  // child's own min/max — this is CSS flexbox's "resolve flexible lengths"
  // algorithm, scoped to one line's children at a time. A single pass
  // (basis + share of leftover, then clamp) would silently drop whatever a
  // clamped child couldn't absorb; instead, any item whose share would
  // violate its own bound gets frozen at that bound and removed from the
  // pool, and the remaining free space is recalculated and redistributed
  // among the still-flexible siblings. Repeats until nothing new freezes
  // (at most one extra item freezes per pass, so ln+1 passes always
  // suffices). Also tracks each line's cross size (max child cross extent)
  // for the cross-axis distribution pass below.
  std::vector<float> finalMain(n);
  std::vector<float> lineCross(lines.size());
  for (size_t li = 0; li < lines.size(); ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    std::vector<bool> frozen(ln, false);
    std::vector<float> lineFinal(ln);
    float gapTotal = ln > 1 ? node.style.gap * (ln - 1) : 0.0f;

    for (size_t pass = 0; pass <= ln; ++pass) {
      float used = gapTotal, gsum = 0, ssum = 0;
      for (size_t k = 0; k < ln; ++k) {
        size_t i = lb + k;
        used += (frozen[k] ? lineFinal[k] : basis[i]) + marginMain[i];
        if (!frozen[k]) {
          gsum += node.children[flowIdx[i]].style.flexGrow;
          ssum += node.children[flowIdx[i]].style.flexShrink;
        }
      }
      float leftover = mainAvail - used;
      if (leftover == 0 || (leftover > 0 && gsum <= 0) ||
          (leftover < 0 && ssum <= 0)) {
        for (size_t k = 0; k < ln; ++k)
          if (!frozen[k])
            lineFinal[k] =
                clampSize(basis[lb + k], minMain[lb + k], maxMain[lb + k]);
        break;
      }
      bool frozeAny = false;
      for (size_t k = 0; k < ln; ++k) {
        if (frozen[k])
          continue;
        size_t i = lb + k;
        const Style &cs = node.children[flowIdx[i]].style;
        float extra = leftover > 0 ? leftover * (cs.flexGrow / gsum)
                                   : leftover * (cs.flexShrink / ssum);
        float candidate = std::max(0.0f, basis[i] + extra);
        float clamped = clampSize(candidate, minMain[i], maxMain[i]);
        lineFinal[k] = clamped;
        if (clamped != candidate) {
          frozen[k] = true;
          frozeAny = true;
        }
      }
      if (!frozeAny)
        break; // this pass's candidates all satisfied their bounds — done
    }

    float maxCross = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      finalMain[i] = lineFinal[k];
      maxCross = std::max(maxCross, cross[i] + mCrossS[i] + mCrossE[i]);
    }
    lineCross[li] = maxCross;
  }

  // ---- Distribute lines along the cross axis (align-content) ----
  // With exactly one line this collapses to the old behavior: Stretch
  // grows that line to fill crossAvail (matching the previous unconditional
  // stretch-to-container-cross-size), everything else just packs the one
  // line at the start.
  size_t numLines = lines.size();
  float lineGapTotal = numLines > 1 ? node.style.gap * (numLines - 1) : 0.0f;
  float linesTotal = lineGapTotal;
  for (float lc : lineCross)
    linesTotal += lc;
  float crossFree = std::max(0.0f, crossAvail - linesTotal);

  std::vector<float> lineOffset(numLines), lineSize(numLines);
  float crossStart = 0, crossBetween = node.style.gap;
  switch (node.style.alignContent) {
  case AlignContent::Start:
    lineSize = lineCross;
    break;
  case AlignContent::End:
    crossStart = crossFree;
    lineSize = lineCross;
    break;
  case AlignContent::Center:
    crossStart = crossFree / 2;
    lineSize = lineCross;
    break;
  case AlignContent::SpaceBetween:
    if (numLines > 1)
      crossBetween += crossFree / (numLines - 1);
    lineSize = lineCross;
    break;
  case AlignContent::SpaceAround: {
    float each = numLines ? crossFree / numLines : 0;
    crossStart = each / 2;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::SpaceEvenly: {
    float each = crossFree / (numLines + 1);
    crossStart = each;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::Stretch: {
    float extra = numLines ? crossFree / numLines : 0;
    for (size_t li = 0; li < numLines; ++li)
      lineSize[li] = lineCross[li] + extra;
    break;
  }
  }
  {
    float pos = crossStart;
    for (size_t li = 0; li < numLines; ++li) {
      lineOffset[li] = pos;
      pos += lineSize[li] + crossBetween;
    }
  }

  // ---- Per-line: justify main axis, align children within the line's
  // cross extent, then recurse ----
  for (size_t li = 0; li < numLines; ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    float lineCrossAvail = lineSize[li];
    float lineCrossPos = (horizontal ? contentY : contentX) + lineOffset[li];

    float totalUsed = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      totalUsed += finalMain[i] + mMainS[i] + mMainE[i];
      if (k + 1 < ln)
        totalUsed += node.style.gap;
    }
    float freeSpace = std::max(0.0f, mainAvail - totalUsed);
    float startOffset = 0, between = node.style.gap;
    switch (node.style.justifyContent) {
    case Justify::Start:
      break;
    case Justify::End:
      startOffset = freeSpace;
      break;
    case Justify::Center:
      startOffset = freeSpace / 2;
      break;
    case Justify::SpaceBetween:
      if (ln > 1)
        between += freeSpace / (ln - 1);
      break;
    case Justify::SpaceAround: {
      float each = ln ? freeSpace / ln : 0;
      startOffset = each / 2;
      between += each;
      break;
    }
    case Justify::SpaceEvenly: {
      float each = freeSpace / (ln + 1);
      startOffset = each;
      between += each;
      break;
    }
    }

    float cursor = (horizontal ? contentX : contentY) + startOffset;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      View &ch = node.children[flowIdx[i]];
      cursor += mMainS[i];

      bool explicitCross = horizontal ? ch.style.height.kind != Size::Kind::Fit
                                      : ch.style.width.kind != Size::Kind::Fit;
      float finalCross = cross[i];
      if (node.style.alignItems == Align::Stretch && !explicitCross)
        finalCross = std::max(0.0f, lineCrossAvail - mCrossS[i] - mCrossE[i]);

      float minCross = horizontal ? ch.style.minHeight : ch.style.minWidth;
      float maxCross = horizontal ? ch.style.maxHeight : ch.style.maxWidth;
      finalCross = clampSize(finalCross, minCross, maxCross);

      float crossOffset;
      switch (node.style.alignItems) {
      case Align::End:
        crossOffset = lineCrossAvail - finalCross - mCrossE[i];
        break;
      case Align::Center:
        crossOffset = (lineCrossAvail - finalCross) / 2;
        break;
      default:
        crossOffset = mCrossS[i];
        break; // Start & Stretch
      }

      float cx = horizontal ? cursor : lineCrossPos + crossOffset;
      float cy = horizontal ? lineCrossPos + crossOffset : cursor;
      float cw = horizontal ? finalMain[i] : finalCross;
      float chh = horizontal ? finalCross : finalMain[i];

      placeNode(ch, cx, cy, cw, chh);
      cursor += finalMain[i] + mMainE[i] + between;
    }
  }

  // ---- Position::Absolute children ----
  // Placed against this node's content box, entirely independent of the
  // flex distribution above. Sizing reuses measureNatural: Fixed/
  // Percentage/Full resolve normally against contentW/contentH (always
  // definite here, since this node's own box is already finalized);
  // Fit falls back to natural content size unless both opposing edges
  // are set, in which case size is derived from them (CSS's "left+right
  // implies width" rule).
  for (auto &ch : node.children) {
    if (ch.style.position != Position::Absolute)
      continue;
    const Style &cs = ch.style;
    bool hasL = !std::isnan(cs.left), hasR = !std::isnan(cs.right);
    bool hasT = !std::isnan(cs.top), hasB = !std::isnan(cs.bottom);

    Natural probe = measureNatural(ch, contentW, contentH, true, true);
    float aw = (cs.width.kind == Size::Kind::Fit && hasL && hasR)
                   ? contentW - cs.left - cs.right
                   : probe.w;
    float ah = (cs.height.kind == Size::Kind::Fit && hasT && hasB)
                   ? contentH - cs.top - cs.bottom
                   : probe.h;
    aw = clampSize(aw, cs.minWidth, cs.maxWidth);
    ah = clampSize(ah, cs.minHeight, cs.maxHeight);

    float ax = hasL   ? contentX + cs.left + cs.margin.left
               : hasR ? contentX + contentW - cs.right - cs.margin.right - aw
                      : contentX + cs.margin.left;
    float ay = hasT   ? contentY + cs.top + cs.margin.top
               : hasB ? contentY + contentH - cs.bottom - cs.margin.bottom - ah
                      : contentY + cs.margin.top;

    placeNode(ch, ax, ay, aw, ah);
  }
}

inline void layoutRoot(View &root, float windowW, float windowH) {
  Natural n = measureNatural(root, windowW, windowH, true, true);
  placeNode(root, 0, 0, n.w, n.h);
}

} // namespace liteui_layout

class LiteUI {

public:
  // Constructor: explicit prevents accidental implicit conversions from a bare
  // int; defaults give an 800x600 "Window".
  explicit LiteUI(int width = 800, int height = 600,
                  const std::string &title = "Window");
  // Destructor: tears down whatever platform resources were created.
  ~LiteUI();
  // Copying is disabled — a LiteUI owns unique OS handles that can't be
  // safely duplicated.
  LiteUI(const LiteUI &) = delete;
  // Copy-assignment is disabled too.
  LiteUI &operator=(const LiteUI &) = delete;

  // Public entry point that blocks the calling thread and pumps the platform's
  // event loop.
  void run(); // blocks, runs the event loop

  // Adds a static box to be drawn on the window and triggers a repaint.
  void addBox(const Box &box);

  // Sets (or replaces) the root of the layout tree, lays it out immediately,
  // and triggers a repaint. Ownership of the tree is copied/moved in.
  void setRoot(View view);

  // Everything below is internal implementation detail.
private:
  // Requested window width in pixels, stored so pixel-drawing helpers can
  // bounds-check.
  int width_;
  // Requested window height in pixels, same purpose as width_.
  int height_;

  // Boxes queued for drawing, in the order addBox() was called (paint order).
  std::vector<Box> boxes_;
  View root_;
  bool hasRoot_ = false;

  // Re-runs the layout algorithm over root_ against the current window size.
  void relayout() {
    if (hasRoot_)
      liteui_layout::layoutRoot(root_, static_cast<float>(width_),
                                static_cast<float>(height_));
  }

  // Global z-index stacking, shared by both backends.
  struct AbsoluteEntry {
    const View *view;
    int order; // document/discovery order, for stable z-index ties
  };

  // Walks the whole tree (not just direct children) collecting every
  // Position::Absolute node, tagged with its pre-order discovery index.
  // Recurses into every node regardless of its own position, so nested
  // absolutes (an absolute inside another absolute's subtree) still get
  // their own top-level slot in the global list.
  void collectAbsolutes(const View &v, std::vector<AbsoluteEntry> &out) {
    for (const auto &child : v.children) {
      if (child.style.position == Position::Absolute)
        out.push_back({&child, static_cast<int>(out.size())});
      collectAbsolutes(child, out);
    }
  }

  // Sorts absolute entries by zIndex ascending, document order breaking
  // ties — shared by paintRoot() (Windows) and redraw() (Linux).
  static void sortAbsolutes(std::vector<AbsoluteEntry> &absolutes) {
    std::stable_sort(absolutes.begin(), absolutes.end(),
                     [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                       if (a.view->style.zIndex != b.view->style.zIndex)
                         return a.view->style.zIndex < b.view->style.zIndex;
                       return a.order < b.order;
                     });
  }

  // Returns whether (px, py) lies within v's already-computed border-box.
  static bool containsPoint(const View &v, float px, float py) {
    return px >= v.computed.x && px < v.computed.x + v.computed.w &&
           py >= v.computed.y && py < v.computed.y + v.computed.h;
  }

  // Recursively finds the topmost in-flow view under (x, y) with a
  // non-null onClick. Children are checked last-to-first (later siblings
  // paint on top), and Position::Absolute children are skipped here —
  // they're handled globally by hitTest(), same split as
  // collectAbsolutes()/renderView(). If the deepest matching view (or any
  // of its flow descendants) has no handler, the search falls through to
  // checking `v` itself, so a click bubbles up to the nearest ancestor
  // that does have one.
  static View *hitTestFlow(View &v, float x, float y) {
    if (!containsPoint(v, x, y))
      return nullptr;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute)
        continue;
      if (View *hit = hitTestFlow(*it, x, y))
        return hit;
    }
    return v.onClick ? &v : nullptr;
  }

  // Top-level hit test against the whole tree: absolutes take priority
  // over the flow tree, highest zIndex/latest doc-order first, mirroring
  // paint order (collectAbsolutes + sortAbsolutes are the same lists used
  // to paint on Windows/Linux).
  View *hitTest(float x, float y) {
    if (!hasRoot_)
      return nullptr;
    std::vector<AbsoluteEntry> absolutes;
    collectAbsolutes(root_, absolutes);
    sortAbsolutes(absolutes);
    for (auto it = absolutes.rbegin(); it != absolutes.rend(); ++it)
      if (View *hit = hitTestFlow(const_cast<View &>(*it->view), x, y))
        return hit;
    return hitTestFlow(root_, x, y);
  }

  // Invokes v's onClick if it has one; no-op for nullptr or an unset handler.
  static void dispatchClick(View *v) {
    if (v && v->onClick)
      v->onClick();
  }

  // Tracks which view (if any) most recently received a left-button press,
  // so a click only fires if the matching release lands back on the same
  // view — ordinary UI click semantics: press, drag off, release elsewhere
  // cancels; press and release both on the button fires it.
  View *pressedView_ = nullptr;

  // Records the view under (x, y) as the pending click's press target.
  void beginPress(float x, float y) { pressedView_ = hitTest(x, y); }

  // Completes a pending press: fires pressedView_'s onClick only if the
  // release also landed on that same view, then clears the pending state
  // unconditionally (a press that never resolves shouldn't linger and
  // affect some later, unrelated release).
  void endPress(float x, float y) {
    View *released = hitTest(x, y);
    if (released && released == pressedView_)
      dispatchClick(released);
    pressedView_ = nullptr;
  }

// Windows-only member/method block.
#if defined(_WIN32)
  // Native window handle; null until CreateWindowExW succeeds.
  HWND hwnd_ = nullptr;

  // Helper converting a UTF-8 std::string to the UTF-16 wide string Win32's *W
  // APIs require.
  static std::wstring toWide(const std::string &s) {
    // First pass: ask Windows how many wide characters the conversion will need
    // (including the null terminator).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    // Allocate a wide string of exactly that length, zero-initialized.
    std::wstring w(wlen, L'\0');
    // Second pass: actually perform the conversion into the allocated buffer.
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
    // Hand the converted string back to the caller.
    return w;
  }

  // The Win32 window procedure: Windows calls this for every message sent to
  // hwnd.
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Will hold the LiteUI instance associated with this hwnd, if any.
    LiteUI *self = nullptr;

    // WM_NCCREATE arrives before any other message and carries the creation
    // parameters.
    if (msg == WM_NCCREATE) {
      // WM_NCCREATE is the very first message a window receives, sent
      // during CreateWindowExW itself, before the window is usable.
      // Reinterpret the message's lParam as the CREATESTRUCTW Windows built for
      // us.
      auto cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      // Recover the `this` pointer we passed as the lpParam argument to
      // CreateWindowExW.
      self = static_cast<LiteUI *>(cs->lpCreateParams);
      // Stash that pointer in the window's user-data slot so future messages
      // can retrieve it.
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    // For every other message, the pointer was already stored by the
    // WM_NCCREATE branch above.
    else {
      // Fetch the previously stored `this` pointer back out of the window's
      // user-data slot.
      self = reinterpret_cast<LiteUI *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Dispatch on the specific message type.
    switch (msg) {
    // Repaint request: redraw all boxes using GDI.
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      if (self)
        self->paintBoxes(hdc);
      self->paintRoot(hdc);
      EndPaint(hwnd, &ps);
      return 0;
    }

    // Left mouse button pressed: record the hit-test target as the
    // pending click's press target (fired later on WM_LBUTTONUP, only if
    // the release lands on the same view). Capture the mouse so we still
    // get the matching WM_LBUTTONUP even if the cursor leaves the window
    // before the button is released.
    case WM_LBUTTONDOWN: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->beginPress(x, y);
        SetCapture(hwnd);
      }
      return 0;
    }

    // Left mouse button released: resolve the pending press against
    // whatever's under the cursor now, firing onClick only if it matches
    // the original press target.
    case WM_LBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->endPress(x, y);
      }
      ReleaseCapture();
      return 0;
    }

    // Capture was taken away from us mid-press (e.g. alt-tab, a system
    // dialog popping up) — the click can't complete normally, so drop the
    // pending press rather than let a later, unrelated release resolve it.
    case WM_CAPTURECHANGED:
      if (self)
        self->pressedView_ = nullptr;
      return 0;

    // Window was resized (including maximize/restore/snap): update our
    // stored dimensions and re-run layout against the new size. GDI needs
    // no buffer reallocation (it paints straight into the window's DC), so
    // this is just relayout + repaint.
    case WM_SIZE: {
      if (self) {
        self->width_ = LOWORD(lp);
        self->height_ = HIWORD(lp);
        self->relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    // When the window is being destroyed...
    case WM_DESTROY:
      // ...tell Windows to post a WM_QUIT message, which ends the GetMessage
      // loop in run().
      PostQuitMessage(0);
      // Indicate this message was fully handled.
      return 0;
    }
    // Any message not explicitly handled above falls through to Windows'
    // default behavior.
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // Draws every queued box into the given device context via GDI.
  void paintBoxes(HDC hdc) {
    for (const auto &b : boxes_) {
      RECT r{b.pos_x, b.pos_y, b.pos_x + b.width, b.pos_y + b.height};
      HBRUSH brush = CreateSolidBrush(RGB(b.color.r, b.color.g, b.color.b));
      FillRect(hdc, &r, brush);
      DeleteObject(brush);
    }
  }

  // Draws the layout tree (if any) using native GDI RoundRect, which handles
  // border-radius directly — no manual pixel math needed on this platform.
  void paintRoot(HDC hdc) {
    if (hasRoot_) {
      paintView(hdc, root_);
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      sortAbsolutes(absolutes);
      for (const auto &e : absolutes)
        paintView(hdc, *e.view);
    }
  }

  void paintView(HDC hdc, const View &v) {
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    HBRUSH bg = CreateSolidBrush(
        RGB(s.backgroundColor.r, s.backgroundColor.g, s.backgroundColor.b));
    HPEN pen =
        s.borderWidth > 0
            ? CreatePen(PS_SOLID, static_cast<int>(s.borderWidth),
                        RGB(s.borderColor.r, s.borderColor.g, s.borderColor.b))
            : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(hdc, bg);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int d = static_cast<int>(s.borderRadius) * 2;
    RoundRect(hdc, x, y, x + w, y + h, d, d);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bg);
    if (s.borderWidth > 0)
      DeleteObject(pen);
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute)
        paintView(hdc, child);
  }

#else // Linux / Wayland

  wl_display *display_ =
      nullptr; // Connection handle to the Wayland compositor; null until
               // wl_display_connect succeeds.
  wl_compositor *compositor_ =
      nullptr; // The compositor global, used to create surfaces.
  xdg_wm_base *wm_base_ = nullptr;  // The xdg-shell global, used to turn a raw
                                    // surface into a desktop window.
  wl_surface *surface_ = nullptr;   // The raw drawable surface for this window.
  xdg_surface *xdg_surf_ = nullptr; // The xdg_surface wrapping surface_, needed
                                    // for configure/ack handshakes.
  xdg_toplevel *toplevel_ =
      nullptr; // The xdg_toplevel representing this as a top-level
               // (movable/resizable/closable) window.
  wl_shm *shm_ =
      nullptr; // The shared-memory global used to allocate pixel buffers.
  wl_buffer *buffer_ =
      nullptr; // The currently attached pixel buffer shown on screen.
  wl_seat *seat_ = nullptr; // The seat global, representing one user's set of
                            // input devices (keyboard/mouse/etc.).

  wl_pointer *pointer_ =
      nullptr; // The pointer (mouse) device obtained from
               // the seat, once it announces pointer capability.
  wl_cursor_theme *cursorTheme_ =
      nullptr; // Loaded cursor theme (arrow, resize handles, etc.), used to
               // look up the pixel images for wl_pointer_set_cursor.
  wl_surface *cursorSurface_ =
      nullptr; // Dedicated surface that holds whichever cursor image is
               // currently active; the compositor renders this at the
               // pointer position once we call wl_pointer_set_cursor.
  uint32_t pointerEnterSerial_ =
      0; // Serial from the most recent pointer-enter event;
         // wl_pointer_set_cursor requires one and it's not resent on motion, so
         // we cache it.
  std::string currentCursorName_; // Name of the cursor image currently shown,
                                  // so we don't reissue set_cursor every
                                  // single motion event for no reason.

  zxdg_decoration_manager_v1 *decoration_manager_ =
      nullptr; // Global for negotiating who draws window decorations (server vs
               // client).

  zxdg_toplevel_decoration_v1 *toplevel_decoration_ =
      nullptr; // Per-window decoration-mode object obtained from
               // decoration_manager_.

  // software rendered CSD titlebar. No cairo here, so buttons are drawn with
  // raw pixel writes straight into the shm buffer.

  static constexpr int kTitlebarHeight =
      32; // Height in pixels reserved at the top of the window for the custom
          // titlebar.

  static constexpr int kButtonSize =
      18; // Width/height in pixels of each titlebar button's square hitbox.

  static constexpr int kButtonMargin =
      8; // Horizontal gap in pixels between buttons and between the last button
         // and the window edge.
  // Raw Linux input-event code for the left mouse button (from
  // linux/input-event-codes.h).
  static constexpr uint32_t BTN_LEFT_CODE = 0x110; // linux/input-event-codes.h

  double pointer_x_ = 0,
         pointer_y_ = 0; // Last known pointer position within
                         // the surface, in surface-local coordinates.

  bool maximized_ =
      false; // Tracks whether the window currently believes
             // itself to be maximized (for toggling and icon state).

  // keep mapped for the buffer's lifetime instead of mmap/unmap per attach,
  // since we now need to repaint(maximize toggle,etc) after the initial attach.
  // File descriptor for the anonymous shared-memory-backed buffer; -1 until
  // created.
  int bufferFd_ = -1;

  uint8_t *bufferData_ = nullptr; // Pointer to the mmap'd pixel data backing
                                  // bufferFd_; null until attachBuffer() runs.

  int bufferSize_ =
      0; // Total size in bytes of the mapped buffer (stride * height).

  bool configured_ =
      false; // Set true once the compositor has sent its first
             // configure event, meaning we're allowed to attach a buffer.
  int pendingWidth_ = 0;  // Size most recently suggested by
  int pendingHeight_ = 0; // toplevelConfigure; 0 means "no suggestion yet"
                          // (the compositor may send 0x0 to mean "you decide").
  bool running_ = true;   // Controls the event loop in run(); set false to
                          // request a clean exit.

  // xdg_wm_base ping handler: the compositor periodically checks we're alive.
  static void wmBasePing(void *, xdg_wm_base *base, uint32_t serial) {
    // Echo the serial straight back so the compositor knows we're responsive.
    xdg_wm_base_pong(base, serial);
  }
  // Listener struct binding the ping callback above to xdg_wm_base's single
  // event.
  static constexpr xdg_wm_base_listener wmBaseListener = {wmBasePing};

  // Called when the compositor wants us to (re)configure our xdg_surface.
  static void surfaceConfigure(void *data, xdg_surface *xs, uint32_t serial) {
    // Recover the LiteUI instance this callback belongs to.
    auto *self = static_cast<LiteUI *>(data);
    // Acknowledge the configure event with its serial, as the protocol
    // requires.
    xdg_surface_ack_configure(xs, serial);
    // Remember that we're now allowed to attach a buffer.
    self->configured_ = true;
    // If the compositor suggested a size different from what we have, resize
    // (reallocate the buffer + relayout) to match. Otherwise, on the very
    // first configure, no buffer exists yet, so create and attach one now.
    if (self->pendingWidth_ > 0 && self->pendingHeight_ > 0 &&
        (self->pendingWidth_ != self->width_ ||
         self->pendingHeight_ != self->height_)) {
      self->resize(self->pendingWidth_, self->pendingHeight_);
    } else if (!self->buffer_)
      self->attachBuffer();
  }
  // Listener struct binding surfaceConfigure to xdg_surface's single event.
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  // Called when the compositor suggests a new size/state for the toplevel.
  static void toplevelConfigure(void *data, xdg_toplevel *, int32_t width,
                                int32_t height, wl_array *) {
    // 0x0 means "you decide the size" — keep whatever we currently have.
    // The actual resize happens later, in surfaceConfigure, once this
    // configure is ack'd (that's the point at which the protocol allows us
    // to attach a differently-sized buffer).
    auto *self = static_cast<LiteUI *>(data);
    if (width > 0 && height > 0) {
      self->pendingWidth_ = width;
      self->pendingHeight_ = height;
    }
  }
  // Called when the compositor/user requests the window be closed (e.g. via a
  // taskbar close action).
  static void toplevelClose(void *data, xdg_toplevel *) {
    // Flip the running flag so run()'s dispatch loop exits on its next check.
    static_cast<LiteUI *>(data)->running_ = false;
  }
  // Listener struct binding the two callbacks above to xdg_toplevel's
  // configure/close events.
  static constexpr xdg_toplevel_listener toplevelListener = {toplevelConfigure,
                                                             toplevelClose};

  // wl_seat/wl_pointer

  // Called when the seat announces which input capabilities
  // (pointer/keyboard/touch) it has.
  static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Only act if the seat has a pointer and we haven't already grabbed one.
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
      // Request the pointer object from the seat.
      self->pointer_ = wl_seat_get_pointer(seat);
      // Register our pointer event handlers on it.
      wl_pointer_add_listener(self->pointer_, &pointerListener, self);
    }
  }

  // Called when the seat reports its human-readable name; unused here.
  static void seatName(void *, wl_seat *, const char *) {}
  // Listener struct binding the two seat callbacks above to wl_seat's events.
  static constexpr wl_seat_listener seatListener = {seatCapabilities, seatName};

  // Called when the pointer enters this surface.
  static void pointerEnter(void *data, wl_pointer *, uint32_t serial,
                           wl_surface *, wl_fixed_t sx, wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Convert Wayland's fixed-point x coordinate to a double and store it.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Same for the y coordinate.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // wl_pointer_set_cursor requires the serial of an enter event, and it
    // isn't resent on plain motion, so remember it for later calls.
    self->pointerEnterSerial_ = serial;
    // Force the next setCursor() call to actually apply, since re-entering
    // (e.g. after a resize/move) may land us back on the same cursor name
    // the compositor's default already reset to something else.
    self->currentCursorName_.clear();
    self->setCursor(cursorNameForEdge(
        self->resizeEdgeAt(self->pointer_x_, self->pointer_y_)));
  }
  // Called when the pointer leaves this surface. Reset the cached cursor
  // name so re-entering always re-applies one, rather than skipping the
  // very next setCursor() as a no-op change.
  static void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *) {
    static_cast<LiteUI *>(data)->currentCursorName_.clear();
  }
  // Called on every pointer movement while over this surface.
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Update the stored x position.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Update the stored y position.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // Re-derive which edge (if any) the pointer is over and update the
    // cursor image to match, so the user sees a resize cursor before they
    // even click.
    self->setCursor(cursorNameForEdge(
        self->resizeEdgeAt(self->pointer_x_, self->pointer_y_)));
  }
  // Called on scroll/axis events; not used by this minimal window.
  static void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t,
                          wl_fixed_t) {}
  // Called on every pointer button press/release.
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Ignore anything that isn't the left button (we don't handle
    // right-click, middle-click, etc.).
    if (button != BTN_LEFT_CODE)
      return;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
      // Route the press to the custom titlebar hit-testing logic, passing
      // the event serial (needed for interactive resize/move grabs).
      self->handlePress(serial);
    else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
      self->handleRelease();
  }
  // Listener struct binding all five pointer callbacks above to wl_pointer's
  // events, in the order the interface expects.
  static constexpr wl_pointer_listener pointerListener = {
      pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis};

  // Called once per global object the compositor advertises via the registry.
  static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // If this global is the compositor interface...
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
      // ...bind to it at version 4 and store the resulting proxy.
      self->compositor_ = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    // If instead this global is the xdg_wm_base (window-shell) interface...
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      // ...bind to it at version 1...
      self->wm_base_ = static_cast<xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      // ...and register the ping/pong listener on it immediately.
      xdg_wm_base_add_listener(self->wm_base_, &wmBaseListener, self);
    }
    // If instead this global is the shared-memory interface...
    else if (strcmp(interface, wl_shm_interface.name) == 0) {
      // ...bind to it so we can later allocate pixel buffers.
      self->shm_ = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    // If instead this global is the seat (input devices) interface...
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
      // ...bind to it...
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      // ...and register the capabilities/name listener so we learn about the
      // pointer.
      wl_seat_add_listener(self->seat_, &seatListener, self);
    }
    // If instead this global is the decoration-manager interface...
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0) {
      // ...bind to it so we can later request client-side (or server-side)
      // decorations.
      self->decoration_manager_ =
          static_cast<zxdg_decoration_manager_v1 *>(wl_registry_bind(
              registry, name, &zxdg_decoration_manager_v1_interface, 1));
    }
  }
  // Called when a global is removed; this minimal window doesn't need to react.
  static void registryRemove(void *, wl_registry *, uint32_t) {}
  // Listener struct binding the two registry callbacks above to wl_registry's
  // events.
  static constexpr wl_registry_listener registryListener = {registryGlobal,
                                                            registryRemove};

  // How close (in pixels) the pointer must be to the window's outer edge
  // before we treat it as a resize grab instead of ordinary titlebar
  // content. Kept well inside the button icons (which start ~7px into a
  // 32px titlebar) so it never steals clicks from minimize/maximize/close.
  static constexpr int kResizeMargin = 6;

  // Maps a pointer position to which edge (if any) an interactive resize
  // should grab, mirroring how most CSD toolkits treat a thin strip along
  // each window edge as a resize handle rather than ordinary content.
  uint32_t resizeEdgeAt(double px, double py) const {
    bool left = px < kResizeMargin;
    bool right = px >= width_ - kResizeMargin;
    bool top = py < kResizeMargin;
    bool bottom = py >= height_ - kResizeMargin;
    if (top && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (top && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (bottom && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (bottom && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (left)
      return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)
      return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    if (top)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bottom)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
  }

  // Standard XCursor names for each edge/corner; "left_ptr" is the ordinary
  // arrow shown everywhere else.
  static const char *cursorNameForEdge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
      return "top_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
      return "bottom_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
      return "left_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
      return "right_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
      return "top_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
      return "top_right_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
      return "bottom_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
      return "bottom_right_corner";
    default:
      return "left_ptr";
    }
  }

  // Swaps the pointer's visible cursor to the named XCursor image, skipping
  // the work entirely if it's already showing (motion events fire far more
  // often than the cursor actually needs to change).
  void setCursor(const char *name) {
    if (!cursorTheme_ || !pointer_ || currentCursorName_ == name)
      return;
    wl_cursor *cursor = wl_cursor_theme_get_cursor(cursorTheme_, name);
    if (!cursor)
      cursor = wl_cursor_theme_get_cursor(cursorTheme_, "default");
    if (!cursor || cursor->image_count == 0)
      return;
    wl_cursor_image *image = cursor->images[0];
    wl_buffer *cbuf = wl_cursor_image_get_buffer(image);
    if (!cbuf)
      return;
    currentCursorName_ = name;
    wl_surface_attach(cursorSurface_, cbuf, 0, 0);
    wl_surface_damage_buffer(cursorSurface_, 0, 0, image->width, image->height);
    wl_surface_commit(cursorSurface_);
    wl_pointer_set_cursor(pointer_, pointerEnterSerial_, cursorSurface_,
                          image->hotspot_x, image->hotspot_y);
  }

  // Handles a compositor-driven resize: tears down the old shm buffer,
  // updates width_/height_, re-runs layout against the new size, and
  // allocates+attaches a fresh buffer at the new dimensions. Buffers can't
  // be resized in place — wl_shm buffers are fixed-size — so this is a full
  // destroy/recreate rather than a realloc.
  void resize(int newWidth, int newHeight) {
    if (buffer_) {
      wl_buffer_destroy(buffer_);
      buffer_ = nullptr;
    }
    if (bufferData_) {
      munmap(bufferData_, bufferSize_);
      bufferData_ = nullptr;
    }
    if (bufferFd_ >= 0) {
      close(bufferFd_);
      bufferFd_ = -1;
    }
    width_ = newWidth;
    height_ = newHeight;
    relayout();
    attachBuffer(); // allocates the new buffer and calls redraw() itself
  }

  // Allocates the shared-memory pixel buffer and attaches it to the surface for
  // the first time.
  void attachBuffer() {
    // Each pixel is 4 bytes (XBGR8888), so a row's byte length is width * 4.
    const int stride = width_ * 4;
    // Total buffer size is one row's bytes times the number of rows.
    bufferSize_ = stride * height_;

    // Create an anonymous, memory-backed file descriptor to hold the pixel
    // data.
    bufferFd_ = memfd_create("gui-window-buf", 0);
    // Bail out if the kernel couldn't give us one.
    if (bufferFd_ < 0)
      throw std::runtime_error("memfd_create failed");
    // Resize that anonymous file to exactly the buffer size we need.
    if (ftruncate(bufferFd_, bufferSize_) < 0) {
      // Clean up the fd before propagating the error.
      close(bufferFd_);
      throw std::runtime_error("ftruncate failed");
    }
    // Kept mapped (not munmap'd) so redraw() can repaint after maximize
    // toggles etc, instead of only ever painting once at attach time.
    // Map the fd's memory into our address space, readable/writable and shared
    // with the compositor.
    bufferData_ = static_cast<uint8_t *>(mmap(nullptr, bufferSize_,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, bufferFd_, 0));
    // Bail out if the mapping failed.
    if (bufferData_ == MAP_FAILED) {
      // Clean up the fd before propagating the error.
      close(bufferFd_);
      throw std::runtime_error("mmap failed");
    }

    // Wrap the fd in a wl_shm_pool so the compositor can carve buffers out of
    // it.
    wl_shm_pool *pool = wl_shm_create_pool(shm_, bufferFd_, bufferSize_);
    // Create a single buffer covering the whole pool, describing
    // width/height/stride/pixel format.
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride,
                                        WL_SHM_FORMAT_XBGR8888);
    // The pool object itself isn't needed anymore once the buffer exists.
    wl_shm_pool_destroy(pool);

    // Paint the initial frame into the freshly mapped buffer and present it.
    redraw();
  }

  // ---- pixel helpers (XBGR8888: memory byte order R,G,B,X per pixel) ----
  // Writes a single opaque pixel at (x, y), silently ignoring out-of-bounds
  // coordinates.
  void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    // Skip drawing anything outside the buffer's bounds.
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return;
    // Compute the byte address of this pixel: row offset plus column offset,
    // four bytes per pixel.
    uint8_t *px = bufferData_ + (static_cast<size_t>(y) * width_ + x) * 4;
    // First byte in memory is red (per the XBGR8888 comment above).
    px[0] = r;
    // Second byte is green.
    px[1] = g;
    // Third byte is blue.
    px[2] = b;
    // Fourth byte is the unused/alpha channel; forced fully opaque.
    px[3] = 0xFF;
  }
  // Fills an axis-aligned rectangle with a solid color by calling setPixel for
  // every point inside it.
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    // Iterate every row of the rectangle.
    for (int y = y0; y < y0 + h; ++y)
      // Iterate every column of the current row.
      for (int x = x0; x < x0 + w; ++x)
        // Paint this pixel with the requested color.
        setPixel(x, y, r, g, b);
  }

  // Fills an axis-aligned rectangle with rounded corners, clamping the
  // radius so it can't exceed half the shorter side. Per-pixel distance
  // check against each corner's circle center — fine at this scale, not
  // meant for huge boxes.
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0)
      return;
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        int cx = -1, cy = -1;
        if (x < radius && y < radius) {
          cx = radius;
          cy = radius;
        } else if (x >= w - radius && y < radius) {
          cx = w - radius - 1;
          cy = radius;
        } else if (x < radius && y >= h - radius) {
          cx = radius;
          cy = h - radius - 1;
        } else if (x >= w - radius && y >= h - radius) {
          cx = w - radius - 1;
          cy = h - radius - 1;
        }
        bool inside = true;
        if (cx >= 0) {
          int dx = x - cx, dy = y - cy;
          inside = (dx * dx + dy * dy) <= radius * radius;
        }
        if (inside)
          setPixel(x0 + x, y0 + y, r, g, b);
      }
    }
  }

  // Draws one View (background + border) using its already-computed layout,
  // then recurses into children. Border is drawn as an outer rounded rect in
  // borderColor with an inner rounded rect in backgroundColor inset by
  // borderWidth — simple and correct for a uniform border on all sides.
  // Paints v's own background/border, then recurses into in-flow
  // children only. Any Position::Absolute descendant, at any depth, is
  // skipped here — it's collected separately and painted in a single
  // global-stacking pass afterward, so it can interleave correctly with
  // absolute nodes from entirely different subtrees.
  void renderView(const View &v) {
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    int radius = static_cast<int>(s.borderRadius);
    if (s.borderWidth > 0) {
      fillRoundedRect(x, y, w, h, radius, s.borderColor.r, s.borderColor.g,
                      s.borderColor.b);
      int bw = static_cast<int>(s.borderWidth);
      fillRoundedRect(x + bw, y + bw, std::max(0, w - 2 * bw),
                      std::max(0, h - 2 * bw), std::max(0, radius - bw),
                      s.backgroundColor.r, s.backgroundColor.g,
                      s.backgroundColor.b);
    } else {
      fillRoundedRect(x, y, w, h, radius, s.backgroundColor.r,
                      s.backgroundColor.g, s.backgroundColor.b);
    }
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute)
        renderView(child);
  }

  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  // Draws a crude line between two points by linear interpolation, stepping
  // once per pixel along the longer axis.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                uint8_t b) {
    // Horizontal distance to cover.
    int dx = x1 - x0, dy = y1 - y0;
    // Number of steps to take is the larger of the two axis distances, so we
    // don't skip pixels.
    int steps = std::max(std::abs(dx), std::abs(dy));
    // Degenerate case: start and end are the same point, so just plot it and
    // return.
    if (steps == 0) {
      setPixel(x0, y0, r, g, b);
      return;
    }
    // Walk from 0 to steps inclusive, interpolating along the way.
    for (int i = 0; i <= steps; ++i) {
      // Interpolated x position at this step (integer division truncates, which
      // is fine for icon-scale lines).
      int x = x0 + dx * i / steps;
      // Interpolated y position at this step.
      int y = y0 + dy * i / steps;
      // Plot the interpolated pixel.
      setPixel(x, y, r, g, b);
      // Also plot the pixel one to the right, giving the line a cheap 2px-wide
      // stroke.
      setPixel(x + 1, y, r, g, b); // cheap 2px-wide stroke
    }
  }

  // ---- titlebar button layout ----
  // Plain axis-aligned rectangle used for button hit-testing.
  struct Rect {
    int x, y, w, h;
  };
  // Returns whether point (px, py) falls within rectangle r (using half-open
  // bounds).
  static bool inside(const Rect &r, double px, double py) {
    // Standard axis-aligned bounding box containment test.
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }
  // Computes the close button's rectangle: flush against the right edge of the
  // titlebar.
  Rect closeRect() const {
    // x is inset from the right edge by the margin and the button's own width;
    // y centers it vertically in the titlebar.
    return {width_ - kButtonMargin - kButtonSize,
            (kTitlebarHeight - kButtonSize) / 2, kButtonSize, kButtonSize};
  }
  // Computes the maximize button's rectangle, positioned one button-width left
  // of the close button.
  Rect maximizeRect() const {
    // Start from the close button's rectangle as a reference point.
    Rect c = closeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {c.x - kButtonMargin - kButtonSize, c.y, kButtonSize, kButtonSize};
  }
  // Computes the minimize button's rectangle, positioned one button-width left
  // of the maximize button.
  Rect minimizeRect() const {
    // Start from the maximize button's rectangle as a reference point.
    Rect m = maximizeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {m.x - kButtonMargin - kButtonSize, m.y, kButtonSize, kButtonSize};
  }

  // Paints the custom titlebar background and its three buttons/icons.
  void drawTitlebar() {
    // Fill the whole titlebar strip with a dark gray background.
    fillRect(0, 0, width_, kTitlebarHeight, 0x2D, 0x2D, 0x2D);

    // Compute all three button rectangles up front for reuse below.
    Rect minr = minimizeRect(), maxr = maximizeRect(), clsr = closeRect();
    // Draw the minimize button's background in a lighter gray.
    fillRect(minr.x, minr.y, minr.w, minr.h, 0x50, 0x50, 0x50);
    // Draw the maximize button's background in the same lighter gray.
    fillRect(maxr.x, maxr.y, maxr.w, maxr.h, 0x50, 0x50, 0x50);
    // Draw the close button's background in red to visually distinguish it.
    fillRect(clsr.x, clsr.y, clsr.w, clsr.h, 0xC0, 0x39, 0x2B);

    // minimize: underscore
    // Draw a single horizontal line near the bottom of the minimize button to
    // form an underscore glyph.
    drawLine(minr.x + 4, minr.y + minr.h - 5, minr.x + minr.w - 4,
             minr.y + minr.h - 5, 0xFF, 0xFF, 0xFF);
    // maximize: square outline
    // Top edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + maxr.w - 4, maxr.y + 4, 0xFF,
             0xFF, 0xFF);
    // Bottom edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + maxr.h - 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // Left edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + 4, maxr.y + maxr.h - 4, 0xFF,
             0xFF, 0xFF);
    // Right edge of the square icon.
    drawLine(maxr.x + maxr.w - 4, maxr.y + 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // close: X
    // First diagonal stroke of the X, from top-left to bottom-right.
    drawLine(clsr.x + 4, clsr.y + 4, clsr.x + clsr.w - 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
    // Second diagonal stroke of the X, from top-right to bottom-left.
    drawLine(clsr.x + clsr.w - 4, clsr.y + 4, clsr.x + 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
  }

  // Repaints the entire window content and commits it to the compositor.
  void redraw() {
    // Nothing to draw into yet if the buffer hasn't been mapped.
    if (!bufferData_)
      return;
    // Clear the whole buffer to white as the plain content-area background.
    std::memset(bufferData_, 0xFF, bufferSize_); // content area
    // Paint queued boxes on top of the plain background, before the titlebar
    // so it stays on top.
    for (const auto &b : boxes_)
      fillRect(b.pos_x, b.pos_y, b.width, b.height, b.color.r, b.color.g,
               b.color.b);
    if (hasRoot_) {
      renderView(root_);
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      std::stable_sort(absolutes.begin(), absolutes.end(),
                       [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                         if (a.view->style.zIndex != b.view->style.zIndex)
                           return a.view->style.zIndex < b.view->style.zIndex;
                         return a.order < b.order;
                       });
      for (const auto &e : absolutes)
        renderView(*e.view);
    }
    // Paint the titlebar and its buttons on top of that background.
    drawTitlebar();
    // Tell the compositor this buffer is what the surface should display, at
    // offset (0,0).
    wl_surface_attach(surface_, buffer_, 0, 0);
    // Mark the whole buffer area as changed so the compositor knows to
    // re-composite it.
    wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
    // Submit the attach+damage as an atomic update to the compositor.
    wl_surface_commit(surface_);
  }

  // Hit-tests a left-button press against the titlebar buttons, resize
  // edges, and drag region. Resize/move grabs and the chrome buttons
  // (close/maximize/minimize) still act immediately on press, same as
  // before — only content-area widget clicks wait for a matching release
  // (see beginPress/endPress).
  void handlePress(uint32_t serial) {
    // Clicks within kResizeMargin of any outer edge start an interactive
    // resize instead — checked first since the resize strip along the top
    // overlaps the first few pixels of the titlebar itself.
    uint32_t edge = resizeEdgeAt(pointer_x_, pointer_y_);
    if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
      if (seat_)
        xdg_toplevel_resize(toplevel_, seat_, serial, edge);
      return;
    }

    // Presses below the titlebar strip start a pending widget click,
    // resolved later in handleRelease() rather than firing immediately.
    if (pointer_y_ >= kTitlebarHeight) {
      beginPress(static_cast<float>(pointer_x_),
                 static_cast<float>(pointer_y_));
      return;
    }

    // If the click landed on the close button...
    if (inside(closeRect(), pointer_x_, pointer_y_)) {
      // ...request the event loop to stop, ending run().
      running_ = false;
      return;
    }
    // If instead the click landed on the maximize button...
    if (inside(maximizeRect(), pointer_x_, pointer_y_)) {
      // If we're currently maximized, ask the compositor to restore the normal
      // size...
      if (maximized_)
        xdg_toplevel_unset_maximized(toplevel_);
      // ...otherwise ask it to maximize the window.
      else
        xdg_toplevel_set_maximized(toplevel_);
      // Flip our local tracking flag to match the new state.
      maximized_ = !maximized_;
      // Repaint immediately (icon/behavior may depend on this state later).
      redraw();
      return;
    }
    // If instead the click landed on the minimize button...
    if (inside(minimizeRect(), pointer_x_, pointer_y_)) {
      // ...ask the compositor to minimize the toplevel.
      xdg_toplevel_set_minimized(toplevel_);
      return;
    }
    // empty titlebar space -> drag to move
    // Any other click in the titlebar (empty space) starts an interactive move,
    // if we have a seat to drive it.
    if (seat_)
      xdg_toplevel_move(toplevel_, seat_, serial);
  }

  // Resolves a pending widget click on button release. Chrome buttons and
  // resize/move grabs don't need this — they already acted on press — so
  // this only matters for content-area clicks below the titlebar.
  void handleRelease() {
    if (pointer_y_ >= kTitlebarHeight)
      endPress(static_cast<float>(pointer_x_), static_cast<float>(pointer_y_));
    else
      pressedView_ = nullptr; // release moved back into the titlebar; cancel
  }

// Ends the Windows/Linux member block.
#endif
};

// Out-of-line constructor definition; inline because this is a single-header
// library.
inline LiteUI::LiteUI(int w, int h, const std::string &title)
    : width_(w), height_(h) {
// Windows-specific construction path.
#if defined(_WIN32)
  // Handle to the current executable module, needed to register a window class.
  HINSTANCE hInst = GetModuleHandleW(nullptr);
  // Name used to identify our window class to Windows.
  const wchar_t *className = L"LiteUIClass";

  // Descriptor for the window class we're about to register.
  WNDCLASSW wc = {};
  // Point the class at our static message-handling function.
  wc.lpfnWndProc = WndProc;
  // Associate the class with this module.
  wc.hInstance = hInst;
  // Give the class the name declared above.
  wc.lpszClassName = className;
  // Use the standard arrow cursor while hovering the window.
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Use the default window background brush/color.
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  // Register the class with the OS so CreateWindowExW can use it.
  RegisterClassW(&wc);

  // Convert the UTF-8 title into the wide string Win32 expects.
  std::wstring wtitle = toWide(title);

  // Create the actual window, passing `this` as the creation parameter so
  // WndProc can recover it.
  hwnd_ = CreateWindowExW(0, className, wtitle.c_str(), WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, width_, height_,
                          nullptr, nullptr, hInst, this);
  // If creation failed, surface it as an exception rather than continuing with
  // a null handle.
  if (!hwnd_) {
    throw std::runtime_error("CreateWindowExW failed");
  }
  // Make the window visible using the platform's default show behavior.
  ShowWindow(hwnd_, SW_SHOWDEFAULT);
  // Force an initial paint of the window.
  UpdateWindow(hwnd_);

// Linux/Wayland-specific construction path.
#else
  // Connect to the compositor via the default Wayland socket.
  display_ = wl_display_connect(nullptr);
  // Fail loudly if no compositor is reachable.
  if (!display_)
    throw std::runtime_error("Failed to connect to Wayland display");

  // Ask the display for its registry, the object through which globals are
  // advertised.
  wl_registry *registry = wl_display_get_registry(display_);
  // Register our global-announcement callbacks on it.
  wl_registry_add_listener(registry, &registryListener, this);
  // Round-trip blocks until the server has answered every request sent
  // so far — the only way to get bind() results synchronously, since
  // globals normally arrive as async events.
  // Block until the compositor has processed everything so far, guaranteeing
  // all globals are bound by now.
  wl_display_roundtrip(display_);

  // Make sure the globals this window absolutely needs were actually
  // advertised.
  if (!compositor_ || !wm_base_ || !shm_)
    throw std::runtime_error("Missing required Wayland globals");

  // Load a cursor theme so resize/arrow cursors can be shown over CSD
  // window edges — nothing does this automatically the way server-side
  // decorations would. nullptr picks the user's configured/default theme;
  // 24 is a conventional base cursor size in pixels. Non-fatal if it fails:
  // we just silently keep whatever cursor the compositor already set.
  cursorTheme_ = wl_cursor_theme_load(nullptr, 24, shm_);
  if (cursorTheme_)
    cursorSurface_ = wl_compositor_create_surface(compositor_);

  // Create the raw drawable surface from the compositor.
  surface_ = wl_compositor_create_surface(compositor_);
  // Wrap that surface as an xdg_surface so it can become a desktop window.
  xdg_surf_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  // Register the configure/ack handler on the xdg_surface.
  xdg_surface_add_listener(xdg_surf_, &surfListener, this);

  // Promote the xdg_surface to a toplevel (a normal, movable/resizable
  // top-level window).
  toplevel_ = xdg_surface_get_toplevel(xdg_surf_);
  // Register the configure/close handlers on the toplevel.
  xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
  // Set the window's title as shown in taskbars/window switchers.
  xdg_toplevel_set_title(toplevel_, title.c_str());

  // Commit the surface state now, which triggers the compositor's first
  // configure event.
  wl_surface_commit(surface_); // triggers the first configure event
// Ends the Windows/Linux construction branch.
#endif
}

// Out-of-line destructor definition.
inline LiteUI::~LiteUI() {
// Windows-specific teardown path.
#if defined(_WIN32)
  // Destroy the native window if it was successfully created.
  if (hwnd_)
    DestroyWindow(hwnd_);
// Linux/Wayland-specific teardown path.
#else
  // Release the shared pixel buffer object if one was created.
  if (buffer_)
    wl_buffer_destroy(buffer_);
  // Unmap the shared memory region if it was mapped.
  if (bufferData_)
    munmap(bufferData_, bufferSize_);
  // Close the backing file descriptor if one was created.
  if (bufferFd_ >= 0)
    close(bufferFd_);
  // Destroy the cursor surface and theme if they were created.
  if (cursorSurface_)
    wl_surface_destroy(cursorSurface_);
  if (cursorTheme_)
    wl_cursor_theme_destroy(cursorTheme_);
  // Destroy the decoration-mode object if one was requested.
  if (toplevel_decoration_)
    zxdg_toplevel_decoration_v1_destroy(toplevel_decoration_);
  // Release the pointer device if one was obtained.
  if (pointer_)
    wl_pointer_destroy(pointer_);
  // Release the seat object if one was bound.
  if (seat_)
    wl_seat_destroy(seat_);
  // Destroy the toplevel object if one was created.
  if (toplevel_)
    xdg_toplevel_destroy(toplevel_);
  // Destroy the xdg_surface wrapper if one was created.
  if (xdg_surf_)
    xdg_surface_destroy(xdg_surf_);
  // Destroy the raw surface if one was created.
  if (surface_)
    wl_surface_destroy(surface_);
  // Disconnect from the compositor if a connection was established.
  if (display_)
    wl_display_disconnect(display_);
// Ends the Windows/Linux teardown branch.
#endif
}
inline void LiteUI::setRoot(View view) {
  root_ = std::move(view);
  hasRoot_ = true;
  relayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (bufferData_)
    redraw();
#endif
}

inline void LiteUI::addBox(const Box &box) {
  boxes_.push_back(box);
#if defined(_WIN32)
  // Ask Windows to repaint; the actual drawing happens in WM_PAINT.
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  // Repaint immediately if the buffer already exists; if it doesn't yet,
  // attachBuffer()'s own redraw() will pick up boxes_ once configured.
  if (bufferData_)
    redraw();
#endif
}

// Out-of-line definition of the blocking event loop.
inline void LiteUI::run() {
// Windows-specific message loop.
#if defined(_WIN32)
  // Storage for each retrieved message.
  MSG msg;
  // GetMessage blocks until a message arrives and returns 0 on WM_QUIT, ending
  // the loop.
  while (GetMessage(&msg, nullptr, 0, 0)) {
    // Translate virtual-key messages into character messages (needed for text
    // input).
    TranslateMessage(&msg);
    // Send the message on to WndProc for handling.
    DispatchMessage(&msg);
  }
// Linux/Wayland-specific event loop.
#else
  // Keep dispatching Wayland events as long as we haven't been asked to stop
  // and the connection is healthy.
  while (running_ && wl_display_dispatch(display_) != -1) {
    // event loop
  }
// Ends the Windows/Linux run() branch.
#endif
}