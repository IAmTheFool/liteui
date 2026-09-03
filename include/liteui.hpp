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
#include <d2d1.h>
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM/GET_WHEEL_DELTA_WPARAM, used by the mouse-wheel handlers
#pragma comment(lib, "d2d1")
#else
#include "xdg-decoration-client-protocol.h" // Generated client bindings for the xdg-decoration protocol (server-side vs client-side decorations).
#include "xdg-shell-client-protocol.h"      // Generated client bindings for the xdg-shell protocol (toplevel windows, configure events).
#include <fcntl.h>

#include <unistd.h>
#include <wayland-client.h> // Core Wayland client protocol: displays, registries, surfaces, shm.
#include <wayland-cursor.h> // wl_cursor_theme_load / wl_cursor_theme_get_cursor, for showing resize/arrow cursors.
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <EGL/eglext.h>
#endif

// Plain RGB color, one byte per channel.
struct Color
{
  uint8_t r = 0, g = 0, b = 0;
};

// A static, axis-aligned filled rectangle the caller wants drawn on the
// window. Position is in window-local pixel coordinates, (0,0) at top-left.
struct Box
{
  int width = 0;
  int height = 0;
  int pos_x = 0;
  int pos_y = 0;
  Color color;
};

// Width (vertical bar) / height (horizontal bar) of a scrollbar's track, in
// pixels. Shared between the layout engine (which reserves this much space
// out of a scroll container's content box on whichever edges show a bar)
// and both renderers (which draw the track/thumb at exactly this size) —
// keeping it in one place means layout and painting can never disagree
// about how much room a bar takes up.
constexpr float kScrollbarThickness = 12.0f;

// ---------------- Layout engine: Size / Style / View ----------------

// A size along one axis. Fixed/Percentage are self-explanatory; Fit sizes
// to the sum/max of children (like CSS's "auto" on a flex item); Full fills
// whatever space the parent gives along that axis (like width: 100%, but
// resolved from available space rather than the parent's own size).
struct Size
{
  enum class Kind
  {
    Fixed,
    Percentage,
    Fit,
    Full
  };
  Kind kind = Kind::Fit;
  float value = 0; // pixels for Fixed, 0-100 for Percentage; unused otherwise

  static Size pixel(float v) { return {Kind::Fixed, v}; }
  static Size percentage(float v) { return {Kind::Percentage, v}; }
  static Size fit() { return {Kind::Fit, 0}; }
  static Size full() { return {Kind::Full, 0}; }
};

enum class FlexDirection
{
  Row,
  Column
};
enum class Justify
{
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly
};

enum class Align
{
  Start,
  End,
  Center,
  Stretch
};

// Whether children that overflow the main axis wrap onto additional lines.
enum class FlexWrap
{
  NoWrap,
  Wrap
};

// How multiple flex lines are distributed along the cross axis. Only
// meaningful when FlexWrap::Wrap actually produces more than one line;
// with a single line this reduces to Stretch filling crossAvail (matching
// the old un-wrapped behavior) or the others packing that one line at the
// start.
enum class AlignContent
{
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch
};

struct EdgeInsets
{
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v) { return {v, v, v, v}; }
};

enum class Position
{
  Static,
  Absolute
};

// CSS-style overflow behavior for one axis of a container. Visible (the
// default) behavior: children are never clipped and never
// scroll, regardless of how big they get. Hidden clips children to the
// container's box but offers no scrollbar/interaction. Scroll always
// clips *and* always shows that axis's scrollbar, even if content
// currently fits. Auto clips and shows the scrollbar only when content
// actually exceeds the viewport on that axis.
enum class Overflow
{
  Visible,
  Hidden,
  Scroll,
  Auto
};

struct Style
{
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

  // When either axis is non-Visible, this view becomes a scroll container:
  // its children are measured at their natural size (not squeezed to fit)
  // and clipped+offset by the view's live scroll position. See
  // View::Computed::scrollX/scrollY and the "Scrolling" section of the
  // layout engine below for how the two axes are handled independently.
  Overflow overflowX = Overflow::Visible;
  Overflow overflowY = Overflow::Visible;
};

// A node in the retained layout tree. Set `style` and `children`; the engine
// fills in `computed` (absolute window pixel coordinates) during layout.
// Renderers only ever read `computed`, never re-derive it from `style`.
class View
{
public:
  Style style;
  std::vector<View> children;

  // Fired on a left-click whose point lands on this view (see LiteUI::hitTest).
  // Bubbles: if a deeper view under the point has no handler, the nearest
  // containing ancestor's onClick fires instead.
  std::function<void()> onClick;

  struct Computed
  {
    float x = 0, y = 0, w = 0, h = 0; // border-box, absolute window coords

    // Only meaningful when style.overflowX/Y != Visible. contentW/contentH
    // is how big this view's children naturally want to be (the scrollable
    // extent); scrollX/scrollY is how far the content is currently
    // scrolled, always clamped to [0, max(0, content - viewport)]. A
    // non-scrollable view leaves these at 0 and they're simply unused.
    float contentW = 0, contentH = 0;
    float scrollX = 0, scrollY = 0;
  } computed;

  void addChild(View child) { children.push_back(std::move(child)); }

  // Whether this view clips/scrolls on the given axis.
  bool scrollsX() const { return style.overflowX != Overflow::Visible; }
  bool scrollsY() const { return style.overflowY != Overflow::Visible; }

  // Maximum scrollX/scrollY this view can currently have, given its last
  // computed content size vs its viewport size. 0 when content fits (or
  // the axis doesn't scroll).
  float maxScrollX() const
  {
    return std::max(0.0f, computed.contentW - computed.w);
  }
  float maxScrollY() const
  {
    return std::max(0.0f, computed.contentH - computed.h);
  }
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
namespace liteui_layout
{

  struct Natural
  {
    float w, h;
  };

  inline float resolveAxis(const Size &s, float available, bool definite,
                           float fitValue)
  {
    switch (s.kind)
    {
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
  inline float clampSize(float v, float minV, float maxV)
  {
    return std::clamp(v, minV, std::max(minV, maxV));
  }

  // Whether a given axis's scrollbar should actually be drawn/interactive:
  // Scroll always shows it, Auto shows it only once content overflows the
  // viewport (the "+ 0.5" is just slack against float rounding so a
  // perfectly-fitting container doesn't flicker a bar on and off), and
  // Hidden/Visible never do. Used by both placeNode (to decide whether to
  // reserve gutter space for it) and every renderer/hit-tester (to decide
  // whether to draw/click it) — kept as one function so those two places
  // can never disagree about whether a bar is showing.
  inline bool axisScrollbarVisible(Overflow ov, float content, float viewport)
  {
    if (ov == Overflow::Scroll)
      return true;
    if (ov == Overflow::Auto)
      return content > viewport + 0.5f;
    return false; // Visible, Hidden
  }

  // Takes node by non-const reference (unlike the rest of this "measure"
  // pass, which is conceptually read-only) for exactly one reason: a scroll
  // container's natural content size — the scrollable extent — needs to be
  // recorded somewhere for later use by placeNode() (to offset/clip
  // children), by the scrollbar-thumb sizing code, and by input handling
  // (to know how far a drag/wheel event is allowed to move the scroll
  // offset). node.computed.contentW/contentH is that somewhere. Everything
  // else this function does is still the same bottom-up size query it always
  // was.
  inline Natural measureNatural(View &node, float availW, float availH,
                                bool wDefinite, bool hDefinite)
  {
    bool horizontal = node.style.direction == FlexDirection::Row;
    const EdgeInsets &pad = node.style.padding;
    bool needW = node.style.width.kind == Size::Kind::Fit;
    bool needH = node.style.height.kind == Size::Kind::Fit;
    bool scrollX = node.scrollsX();
    bool scrollY = node.scrollsY();

    float w = clampSize(resolveAxis(node.style.width, availW, wDefinite, 0),
                        node.style.minWidth, node.style.maxWidth);
    float h = clampSize(resolveAxis(node.style.height, availH, hDefinite, 0),
                        node.style.minHeight, node.style.maxHeight);
    // A scroll container must still visit its children even when neither
    // axis is Fit (e.g. a fixed-size scrollable box) — that's the whole
    // point: we need to know how big the content *wants* to be so we know
    // how far it can scroll, even though the container's own box size
    // doesn't depend on that at all.
    if ((!needW && !needH && !scrollX && !scrollY) || node.children.empty())
      return {w, h};

    // Along an axis this node scrolls, children are measured against
    // effectively unbounded space so they report their true desired size
    // instead of being squeezed into the viewport — that natural total is
    // exactly the "scrollable extent". Along a non-scrolling axis, sizing is
    // unchanged from before (children measured against the resolved inner
    // box, or against availW/availH while this node's own size is still
    // being figured out).
    constexpr float kUnbounded = std::numeric_limits<float>::max() / 4;
    float innerW = scrollX ? kUnbounded
                   : (wDefinite && !needW)
                       ? std::max(0.0f, w - pad.left - pad.right)
                       : availW;
    float innerH = scrollY ? kUnbounded
                   : (hDefinite && !needH)
                       ? std::max(0.0f, h - pad.top - pad.bottom)
                       : availH;

    float mainTotal = 0, crossMax = 0;
    bool firstFlow = true;
    for (size_t i = 0; i < node.children.size(); ++i)
    {
      View &c = node.children[i];
      if (c.style.position == Position::Absolute)
        continue; // out of flow: doesn't affect the parent's Fit size at all
      Natural cn = measureNatural(c, innerW, innerH,
                                  scrollX ? false : (wDefinite || !needW),
                                  scrollY ? false : (hDefinite || !needH));
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

    // Content extent is deliberately NOT run through clampSize/min-max: a
    // container's max-width doesn't shrink its *content*, only its own box
    // (that's the entire reason overflow is a thing).
    if (scrollX)
      node.computed.contentW =
          (horizontal ? mainTotal : crossMax) + pad.left + pad.right;
    if (scrollY)
      node.computed.contentH =
          (horizontal ? crossMax : mainTotal) + pad.top + pad.bottom;
    return {w, h};
  }

  inline void placeNode(View &node, float x, float y, float w, float h)
  {
    // contentW/contentH here already existed as "content's natural size" on
    // node.computed before this call, set by the measureNatural() pass that
    // ran over this same node earlier (either from the parent's per-child
    // loop, or from layoutRoot() for the root). We're about to overwrite
    // x/y/w/h with the final viewport box; contentW/contentH and
    // scrollX/scrollY are left untouched by this assignment.
    node.computed.x = x;
    node.computed.y = y;
    node.computed.w = w;
    node.computed.h = h;
    if (node.children.empty())
    {
      // No children means nothing to scroll regardless of overflow setting;
      // pin the offset at 0 so a container that briefly had children (and
      // therefore a scroll offset) doesn't leave a stale one behind if it's
      // ever emptied out.
      node.computed.scrollX = node.computed.scrollY = 0;
      return;
    }

    bool horizontal = node.style.direction == FlexDirection::Row;
    const EdgeInsets &pad = node.style.padding;
    float contentW = std::max(0.0f, w - pad.left - pad.right);
    float contentH = std::max(0.0f, h - pad.top - pad.bottom);

    // ---- Scrolling: gutter reservation, offset clamping, content origin ----
    // Whether each axis's scrollbar is actually showing determines whether it
    // eats into the space available for children — same rule CSS uses (a
    // visible scrollbar shrinks the content box on the OTHER axis; the
    // scrolling axis itself is unbounded so its own bar doesn't need to
    // "make room" against itself).
    bool showVBar =
        node.scrollsY() &&
        axisScrollbarVisible(node.style.overflowY, node.computed.contentH, h);
    bool showHBar =
        node.scrollsX() &&
        axisScrollbarVisible(node.style.overflowX, node.computed.contentW, w);
    if (showVBar)
      contentW = std::max(0.0f, contentW - kScrollbarThickness);
    if (showHBar)
      contentH = std::max(0.0f, contentH - kScrollbarThickness);

    // Scroll offsets are user/input-driven state that can go stale the
    // instant content size or viewport size changes (a window resize, or
    // content shrinking), so every relayout re-clamps them into range rather
    // than trusting whatever a previous frame left behind.
    if (node.scrollsX())
      node.computed.scrollX =
          std::clamp(node.computed.scrollX, 0.0f, node.maxScrollX());
    else
      node.computed.scrollX = 0;
    if (node.scrollsY())
      node.computed.scrollY =
          std::clamp(node.computed.scrollY, 0.0f, node.maxScrollY());
    else
      node.computed.scrollY = 0;

    // The content origin simply shifts by the (clamped) scroll offset —
    // children are positioned exactly as they would be at scroll (0,0), then
    // this single subtraction slides the whole subtree. Clipping (handled by
    // the renderers/hit-testers, not here) is what actually hides the part
    // that scrolls out of view.
    float contentX = x + pad.left - node.computed.scrollX;
    float contentY = y + pad.top - node.computed.scrollY;

    float mainAvail = horizontal ? contentW : contentH;
    float crossAvail = horizontal ? contentH : contentW;
    // A scrolling main axis must never let flexShrink squeeze children below
    // their natural size just because the viewport is smaller than the
    // content — that's the entire point of scrolling instead of shrinking.
    // Widening mainAvail to at least the natural content total makes
    // "leftover" in the flex-resolution pass below >= 0, which keeps every
    // shrink factor's candidate at-or-above its unclamped basis.
    if (horizontal && node.scrollsX())
      mainAvail =
          std::max(mainAvail, node.computed.contentW - pad.left - pad.right);
    if (!horizontal && node.scrollsY())
      mainAvail =
          std::max(mainAvail, node.computed.contentH - pad.top - pad.bottom);

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

    for (size_t k = 0; k < n; ++k)
    {
      View &c = node.children[flowIdx[k]];
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
    struct Line
    {
      size_t begin, end; // half-open [begin, end) into node.children
    };
    std::vector<Line> lines;
    if (!wrap)
    {
      lines.push_back({0, n});
    }
    else
    {
      size_t start = 0;
      float used = 0;
      for (size_t i = 0; i < n; ++i)
      {
        float itemMain = basis[i] + marginMain[i];
        float withGap = (i > start) ? node.style.gap : 0.0f;
        if (i > start && used + withGap + itemMain > mainAvail)
        {
          lines.push_back({start, i});
          start = i;
          used = itemMain;
        }
        else
        {
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
    for (size_t li = 0; li < lines.size(); ++li)
    {
      size_t lb = lines[li].begin, le = lines[li].end;
      size_t ln = le - lb;
      std::vector<bool> frozen(ln, false);
      std::vector<float> lineFinal(ln);
      float gapTotal = ln > 1 ? node.style.gap * (ln - 1) : 0.0f;

      for (size_t pass = 0; pass <= ln; ++pass)
      {
        float used = gapTotal, gsum = 0, ssum = 0;
        for (size_t k = 0; k < ln; ++k)
        {
          size_t i = lb + k;
          used += (frozen[k] ? lineFinal[k] : basis[i]) + marginMain[i];
          if (!frozen[k])
          {
            gsum += node.children[flowIdx[i]].style.flexGrow;
            ssum += node.children[flowIdx[i]].style.flexShrink;
          }
        }
        float leftover = mainAvail - used;
        if (leftover == 0 || (leftover > 0 && gsum <= 0) ||
            (leftover < 0 && ssum <= 0))
        {
          for (size_t k = 0; k < ln; ++k)
            if (!frozen[k])
              lineFinal[k] =
                  clampSize(basis[lb + k], minMain[lb + k], maxMain[lb + k]);
          break;
        }
        bool frozeAny = false;
        for (size_t k = 0; k < ln; ++k)
        {
          if (frozen[k])
            continue;
          size_t i = lb + k;
          const Style &cs = node.children[flowIdx[i]].style;
          float extra = leftover > 0 ? leftover * (cs.flexGrow / gsum)
                                     : leftover * (cs.flexShrink / ssum);
          float candidate = std::max(0.0f, basis[i] + extra);
          float clamped = clampSize(candidate, minMain[i], maxMain[i]);
          lineFinal[k] = clamped;
          if (clamped != candidate)
          {
            frozen[k] = true;
            frozeAny = true;
          }
        }
        if (!frozeAny)
          break; // this pass's candidates all satisfied their bounds — done
      }

      float maxCross = 0;
      for (size_t k = 0; k < ln; ++k)
      {
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
    switch (node.style.alignContent)
    {
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
    case AlignContent::SpaceAround:
    {
      float each = numLines ? crossFree / numLines : 0;
      crossStart = each / 2;
      crossBetween += each;
      lineSize = lineCross;
      break;
    }
    case AlignContent::SpaceEvenly:
    {
      float each = crossFree / (numLines + 1);
      crossStart = each;
      crossBetween += each;
      lineSize = lineCross;
      break;
    }
    case AlignContent::Stretch:
    {
      float extra = numLines ? crossFree / numLines : 0;
      for (size_t li = 0; li < numLines; ++li)
        lineSize[li] = lineCross[li] + extra;
      break;
    }
    }
    {
      float pos = crossStart;
      for (size_t li = 0; li < numLines; ++li)
      {
        lineOffset[li] = pos;
        pos += lineSize[li] + crossBetween;
      }
    }

    // ---- Per-line: justify main axis, align children within the line's
    // cross extent, then recurse ----
    for (size_t li = 0; li < numLines; ++li)
    {
      size_t lb = lines[li].begin, le = lines[li].end;
      size_t ln = le - lb;
      float lineCrossAvail = lineSize[li];
      float lineCrossPos = (horizontal ? contentY : contentX) + lineOffset[li];

      float totalUsed = 0;
      for (size_t k = 0; k < ln; ++k)
      {
        size_t i = lb + k;
        totalUsed += finalMain[i] + mMainS[i] + mMainE[i];
        if (k + 1 < ln)
          totalUsed += node.style.gap;
      }
      float freeSpace = std::max(0.0f, mainAvail - totalUsed);
      float startOffset = 0, between = node.style.gap;
      switch (node.style.justifyContent)
      {
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
      case Justify::SpaceAround:
      {
        float each = ln ? freeSpace / ln : 0;
        startOffset = each / 2;
        between += each;
        break;
      }
      case Justify::SpaceEvenly:
      {
        float each = freeSpace / (ln + 1);
        startOffset = each;
        between += each;
        break;
      }
      }

      float cursor = (horizontal ? contentX : contentY) + startOffset;
      for (size_t k = 0; k < ln; ++k)
      {
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
        switch (node.style.alignItems)
        {
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
    for (auto &ch : node.children)
    {
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

  inline void layoutRoot(View &root, float windowW, float windowH)
  {
    Natural n = measureNatural(root, windowW, windowH, true, true);
    placeNode(root, 0, 0, n.w, n.h);
  }

} // namespace liteui_layout

class LiteUI
{

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
  void relayout()
  {
    if (hasRoot_)
      liteui_layout::layoutRoot(root_, static_cast<float>(width_),
                                static_cast<float>(height_));
  }

  // A rectangle used to accumulate the intersection of every scrollable
  // ancestor's viewport as we walk down the tree, so painting and
  // hit-testing can both tell "is this point/pixel actually visible, or
  // has it scrolled behind a clipping ancestor". Defaults to "the whole
  // plane" so the root of any walk starts unclipped.
  struct ClipRect
  {
    float x0 = -std::numeric_limits<float>::infinity();
    float y0 = -std::numeric_limits<float>::infinity();
    float x1 = std::numeric_limits<float>::infinity();
    float y1 = std::numeric_limits<float>::infinity();
    bool contains(float px, float py) const
    {
      return px >= x0 && px < x1 && py >= y0 && py < y1;
    }
    // Narrows this clip to also be inside the given box — used every time
    // we descend into a scroll container's children.
    ClipRect intersect(float bx, float by, float bw, float bh) const
    {
      return {std::max(x0, bx), std::max(y0, by), std::min(x1, bx + bw),
              std::min(y1, by + bh)};
    }
  };

  // Plain float rectangle for scrollbar geometry (track/thumb), kept
  // separate from Wayland's integer-pixel `Rect` below since scrollbar math
  // wants to stay in the same float space as View::Computed.
  struct PixRect
  {
    float x, y, w, h;
  };

  // Whether view v's vertical/horizontal scrollbar is currently showing —
  // thin wrappers around axisScrollbarVisible() using v's own already-
  // computed sizes, so callers don't have to repeat the h vs. contentH /
  // w vs. contentW pairing correctly every time.
  static bool wantVBar(const View &v)
  {
    return v.scrollsY() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowY, v.computed.contentH, v.computed.h);
  }
  static bool wantHBar(const View &v)
  {
    return v.scrollsX() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowX, v.computed.contentW, v.computed.w);
  }

  // Track rectangles run the full length of their edge, minus the corner
  // square where both bars would otherwise overlap (only relevant when
  // both axes scroll at once).
  static PixRect vTrackRect(const View &v)
  {
    float h = v.computed.h - (wantHBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x + v.computed.w - kScrollbarThickness, v.computed.y,
            kScrollbarThickness, std::max(0.0f, h)};
  }
  static PixRect hTrackRect(const View &v)
  {
    float w = v.computed.w - (wantVBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x, v.computed.y + v.computed.h - kScrollbarThickness,
            std::max(0.0f, w), kScrollbarThickness};
  }

  // Minimum thumb length so a very long scrollable area doesn't shrink the
  // thumb down to an unclickable sliver.
  static constexpr float kMinThumb = 20.0f;

  // Thumb length is proportional to viewport/content (how much of the
  // content is visible at once); thumb position is proportional to how far
  // through the scrollable range the current offset is.
  static PixRect vThumbRect(const View &v)
  {
    PixRect track = vTrackRect(v);
    float thumbH =
        v.computed.contentH > 0
            ? std::clamp(track.h * (v.computed.h / v.computed.contentH),
                         kMinThumb, track.h)
            : track.h;
    float maxScroll = v.maxScrollY();
    float pos = maxScroll > 0
                    ? (v.computed.scrollY / maxScroll) * (track.h - thumbH)
                    : 0.0f;
    return {track.x, track.y + pos, track.w, thumbH};
  }
  static PixRect hThumbRect(const View &v)
  {
    PixRect track = hTrackRect(v);
    float thumbW =
        v.computed.contentW > 0
            ? std::clamp(track.w * (v.computed.w / v.computed.contentW),
                         kMinThumb, track.w)
            : track.w;
    float maxScroll = v.maxScrollX();
    float pos = maxScroll > 0
                    ? (v.computed.scrollX / maxScroll) * (track.w - thumbW)
                    : 0.0f;
    return {track.x + pos, track.y, thumbW, track.h};
  }
  static bool pixRectContains(const PixRect &r, float px, float py)
  {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }

  // ---- Press/drag/wheel resolution against scrollbars & scrollable content
  // ---- What a press (or a wheel event, which reuses this to find its target)
  // landed on. VThumb/HThumb mean "start dragging that thumb"; VTrack/
  // HTrack mean "clicked empty track — jump the thumb to click position;
  // Content means "this is inside some scrollable view's content area
  // (not its scrollbar)" — used both to start a possible pan-to-scroll
  // drag and, for wheel events, as the view to scroll.
  enum class ScrollHit
  {
    None,
    VThumb,
    HThumb,
    VTrack,
    HTrack,
    Content
  };
  struct ScrollPress
  {
    ScrollHit kind = ScrollHit::None;
    View *view = nullptr;
    float trackFrac = 0; // 0..1 position along the track, for VTrack/HTrack
  };

  // Walks down from v looking for the deepest relevant scroll interaction
  // under (x, y). Each node's own scrollbars are checked before recursing
  // into its children, which is always correct because layout already
  // reserves the scrollbar's gutter out of the children's placement area —
  // a child box can never overlap its parent's own scrollbar. Known
  // limitation shared with hitTestFlow below: Position::Absolute
  // descendants aren't threaded through this clip-aware walk at all (they
  // bypass the normal recursion entirely, same as elsewhere in this file),
  // so an absolute view inside a scrolled-out region can still be found —
  // acceptable for v1, matching this file's existing absolute-positioning
  // trade-offs.
  static ScrollPress resolveScrollTarget(View &v, float x, float y,
                                         ClipRect clip)
  {
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return {};
    if (wantVBar(v))
    {
      PixRect track = vTrackRect(v);
      if (pixRectContains(track, x, y))
      {
        PixRect thumb = vThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::VThumb, &v, 0};
        float frac = track.h > 0 ? (y - track.y) / track.h : 0;
        return {ScrollHit::VTrack, &v, frac};
      }
    }
    if (wantHBar(v))
    {
      PixRect track = hTrackRect(v);
      if (pixRectContains(track, x, y))
      {
        PixRect thumb = hThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::HThumb, &v, 0};
        float frac = track.w > 0 ? (x - track.x) / track.w : 0;
        return {ScrollHit::HTrack, &v, frac};
      }
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it)
    {
      if (it->style.position == Position::Absolute)
        continue;
      ScrollPress r = resolveScrollTarget(*it, x, y, childClip);
      if (r.kind != ScrollHit::None)
        return r;
    }
    if (v.scrollsX() || v.scrollsY())
      return {ScrollHit::Content, &v, 0};
    return {};
  }

  // How a click-and-drag inside scrollable content is currently being
  // interpreted. Mirrors pressedView_'s press/release pairing, but for
  // scroll interactions instead of onClick.
  enum class DragMode
  {
    None,
    VThumb,
    HThumb,
    ContentPan
  };
  struct ScrollDrag
  {
    DragMode mode = DragMode::None;
    View *target = nullptr;
    float startPointerX = 0, startPointerY = 0;
    float startScrollX = 0, startScrollY = 0;
    // ContentPan only: whether the pointer has moved past the click/drag
    // threshold yet. Until it does, this might still turn out to be an
    // ordinary click (see beginScrollPress/updateScrollDrag/endScrollPress).
    bool moved = false;
  } scrollDrag_;

  // Pixels of pointer movement before a press-in-content is treated as a
  // pan-to-scroll drag rather than a click.
  static constexpr float kDragThreshold = 4.0f;

  // Call on every left-button press, before the existing beginPress(). If
  // this returns true, it fully owns the press (a scrollbar grab, or a
  // track-click jump) and the caller must NOT also call beginPress() —
  // there's nothing left to click. If it returns false, the caller should
  // fall through to its normal beginPress(x, y) — resolveScrollTarget()
  // found either nothing scrollable, or plain scrollable *content*, in
  // which case a ContentPan drag is armed here but the ordinary click path
  // still runs too, since a small movement should behave as a click, not a
  // pan (see updateScrollDrag/endScrollPress).
  bool beginScrollPress(float x, float y)
  {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    switch (r.kind)
    {
    case ScrollHit::VThumb:
      scrollDrag_ = {
          DragMode::VThumb, r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      return true;
    case ScrollHit::HThumb:
      scrollDrag_ = {
          DragMode::HThumb, r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      return true;
    case ScrollHit::VTrack:
    {
      PixRect track = vTrackRect(*r.view), thumb = vThumbRect(*r.view);
      float target = r.trackFrac * track.h - thumb.h / 2;
      float maxScroll = r.view->maxScrollY();
      float range = track.h - thumb.h;
      r.view->computed.scrollY = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::HTrack:
    {
      PixRect track = hTrackRect(*r.view), thumb = hThumbRect(*r.view);
      float target = r.trackFrac * track.w - thumb.w / 2;
      float maxScroll = r.view->maxScrollX();
      float range = track.w - thumb.w;
      r.view->computed.scrollX = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::Content:
      scrollDrag_ = {
          DragMode::ContentPan, r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, false};
      return false; // ordinary click press still proceeds too
    case ScrollHit::None:
      return false;
    }
    return false;
  }

  // Call on every pointer-motion event while a button is held. Advances an
  // in-progress scrollbar drag or content pan; a no-op if scrollDrag_ isn't
  // active. Returns true if it changed anything (caller should repaint).
  bool updateScrollDrag(float x, float y)
  {
    if (scrollDrag_.mode == DragMode::None)
      return false;
    View &v = *scrollDrag_.target;
    float dx = x - scrollDrag_.startPointerX;
    float dy = y - scrollDrag_.startPointerY;
    if (scrollDrag_.mode == DragMode::ContentPan)
    {
      if (!scrollDrag_.moved && std::abs(dx) < kDragThreshold &&
          std::abs(dy) < kDragThreshold)
        return false; // still within click tolerance — not a pan yet
      if (!scrollDrag_.moved)
      {
        scrollDrag_.moved = true;
        // It just became a drag, not a click — cancel any pending onClick
        // so the eventual release doesn't also fire it.
        pressedView_ = nullptr;
      }
      if (v.scrollsX())
        v.computed.scrollX =
            std::clamp(scrollDrag_.startScrollX - dx, 0.0f, v.maxScrollX());
      if (v.scrollsY())
        v.computed.scrollY =
            std::clamp(scrollDrag_.startScrollY - dy, 0.0f, v.maxScrollY());
    }
    else if (scrollDrag_.mode == DragMode::VThumb)
    {
      PixRect track = vTrackRect(v), thumb = vThumbRect(v);
      float range = track.h - thumb.h;
      float delta = range > 0 ? (dy / range) * v.maxScrollY() : 0.0f;
      v.computed.scrollY =
          std::clamp(scrollDrag_.startScrollY + delta, 0.0f, v.maxScrollY());
    }
    else if (scrollDrag_.mode == DragMode::HThumb)
    {
      PixRect track = hTrackRect(v), thumb = hThumbRect(v);
      float range = track.w - thumb.w;
      float delta = range > 0 ? (dx / range) * v.maxScrollX() : 0.0f;
      v.computed.scrollX =
          std::clamp(scrollDrag_.startScrollX + delta, 0.0f, v.maxScrollX());
    }
    relayout();
    return true;
  }

  // Call on every left-button release. Resolves a ContentPan that never
  // crossed the drag threshold back into an ordinary click via the
  // existing endPress(); anything that already became a real drag (a
  // scrollbar grab, or a pan that moved) just ends quietly. Always clears
  // scrollDrag_ so a stale target can't leak into some unrelated later
  // press.
  void endScrollPress(float x, float y)
  {
    if (scrollDrag_.mode == DragMode::ContentPan && !scrollDrag_.moved)
      endPress(x, y);
    scrollDrag_ = {};
  }

  // Call on every wheel/scroll event; deltaX/deltaY are in pixels (already
  // sign-adjusted so positive means "scroll right"/"scroll down" — each
  // platform's wheel callback is responsible for that conversion). Finds
  // the deepest scrollable view under the cursor via the same walk used
  // for press resolution — scrollbar vs. content doesn't matter for wheel
  // input, both count as "the pointer is over this scrollable view".
  // Returns true if it changed anything (caller should repaint).
  bool applyWheelScroll(float x, float y, float deltaX, float deltaY)
  {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    if (r.kind == ScrollHit::None || !r.view)
      return false;
    View &v = *r.view;
    bool changed = false;
    if (v.scrollsY() && deltaY != 0.0f)
    {
      float ns = std::clamp(v.computed.scrollY + deltaY, 0.0f, v.maxScrollY());
      changed |= ns != v.computed.scrollY;
      v.computed.scrollY = ns;
    }
    if (v.scrollsX() && deltaX != 0.0f)
    {
      float ns = std::clamp(v.computed.scrollX + deltaX, 0.0f, v.maxScrollX());
      changed |= ns != v.computed.scrollX;
      v.computed.scrollX = ns;
    }
    if (changed)
      relayout();
    return changed;
  }

  // Global z-index stacking, shared by both backends.
  struct AbsoluteEntry
  {
    const View *view;
    int order; // document/discovery order, for stable z-index ties
  };

  // Walks the whole tree (not just direct children) collecting every
  // Position::Absolute node, tagged with its pre-order discovery index.
  // Recurses into every node regardless of its own position, so nested
  // absolutes (an absolute inside another absolute's subtree) still get
  // their own top-level slot in the global list.
  void collectAbsolutes(const View &v, std::vector<AbsoluteEntry> &out)
  {
    for (const auto &child : v.children)
    {
      if (child.style.position == Position::Absolute)
        out.push_back({&child, static_cast<int>(out.size())});
      collectAbsolutes(child, out);
    }
  }

  // Sorts absolute entries by zIndex ascending, document order breaking
  // ties — shared by paintRoot() (Windows) and redraw() (Linux).
  static void sortAbsolutes(std::vector<AbsoluteEntry> &absolutes)
  {
    std::stable_sort(absolutes.begin(), absolutes.end(),
                     [](const AbsoluteEntry &a, const AbsoluteEntry &b)
                     {
                       if (a.view->style.zIndex != b.view->style.zIndex)
                         return a.view->style.zIndex < b.view->style.zIndex;
                       return a.order < b.order;
                     });
  }

  // Returns whether (px, py) lies within v's already-computed border-box.
  static bool containsPoint(const View &v, float px, float py)
  {
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
  //
  // `clip` is the accumulated intersection of every scrollable ancestor's
  // viewport seen so far — a point outside it means whatever's physically
  // there has scrolled out of view, so it can't be hit no matter what its
  // own box says. Only scrollable nodes narrow the clip further as we
  // descend, exactly mirroring how renderView() decides what to clip.
  static View *hitTestFlow(View &v, float x, float y, ClipRect clip)
  {
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return nullptr;
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it)
    {
      if (it->style.position == Position::Absolute)
        continue;
      if (View *hit = hitTestFlow(*it, x, y, childClip))
        return hit;
    }
    return v.onClick ? &v : nullptr;
  }

  // Top-level hit test against the whole tree: absolutes take priority
  // over the flow tree, highest zIndex/latest doc-order first, mirroring
  // paint order (collectAbsolutes + sortAbsolutes are the same lists used
  // to paint on Windows/Linux). Absolutes are tested unclipped — see the
  // "known limitation" note on resolveScrollTarget() above.
  View *hitTest(float x, float y)
  {
    if (!hasRoot_)
      return nullptr;
    std::vector<AbsoluteEntry> absolutes;
    collectAbsolutes(root_, absolutes);
    sortAbsolutes(absolutes);
    for (auto it = absolutes.rbegin(); it != absolutes.rend(); ++it)
      if (View *hit =
              hitTestFlow(const_cast<View &>(*it->view), x, y, ClipRect{}))
        return hit;
    return hitTestFlow(root_, x, y, ClipRect{});
  }

  // Invokes v's onClick if it has one; no-op for nullptr or an unset handler.
  static void dispatchClick(View *v)
  {
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
  void endPress(float x, float y)
  {
    View *released = hitTest(x, y);
    if (released && released == pressedView_)
      dispatchClick(released);
    pressedView_ = nullptr;
  }

// Windows-only member/method block.
#if defined(_WIN32)
  // Native window handle; null until CreateWindowExW succeeds.
  HWND hwnd_ = nullptr;

  // Device-independent: created once in the constructor, lives for the
  // process (well, the LiteUI instance's) lifetime.
  ID2D1Factory *d2dFactory_ = nullptr;

  // Device-dependent: bound to hwnd_'s current size. Torn down and
  // recreated if EndDraw() ever reports D2DERR_RECREATE_TARGET (e.g. after
  // a display driver reset) — everything that draws goes through this
  // pointer, never a raw HDC.
  ID2D1HwndRenderTarget *renderTarget_ = nullptr;

  // Lazily (re)creates renderTarget_ against hwnd_'s current client size.
  // A no-op once a valid target already exists; WM_SIZE calls Resize()
  // directly instead of tearing this down, so this only actually runs
  // once per (factory, hwnd) pair unless EndDraw() invalidates the target.
  void ensureRenderTarget()
  {
    if (renderTarget_)
      return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                    static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size), &renderTarget_);
    if (FAILED(hr))
      throw std::runtime_error("CreateHwndRenderTarget failed");
  }

  // Converts our own Color into the D2D1::ColorF Direct2D brushes want.
  static D2D1::ColorF toD2DColor(Color c)
  {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f);
  }

  // Draws a filled rectangle in one shot: create brush, fill, release.
  // Mirrors the old gdiFillRect's create-use-delete pattern 1:1.
  static void d2dFillRect(ID2D1RenderTarget *rt, float x, float y, float w,
                          float h, Color c)
  {
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(c), &brush);
    if (brush)
    {
      rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
      brush->Release();
    }
  }

  // Helper converting a UTF-8 std::string to the UTF-16 wide string Win32's *W
  // APIs require.
  static std::wstring toWide(const std::string &s)
  {
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
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    // Will hold the LiteUI instance associated with this hwnd, if any.
    LiteUI *self = nullptr;

    // WM_NCCREATE arrives before any other message and carries the creation
    // parameters.
    if (msg == WM_NCCREATE)
    {
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
    else
    {
      // Fetch the previously stored `this` pointer back out of the window's
      // user-data slot.
      self = reinterpret_cast<LiteUI *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Dispatch on the specific message type.
    switch (msg)
    {
    // Repaint request: paint everything into the off-screen back buffer,
    // then blit it to the screen in a single BitBlt — see memDC_'s comment
    // for why (this is the actual flicker fix).
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      if (self)
      {
        self->ensureRenderTarget();
        self->renderTarget_->BeginDraw();
        // Plain white background first — WM_ERASEBKGND below tells
        // Windows not to do this for us anymore, so we own it, matching
        // what the Linux/Wayland renderer already does for its content
        // area.
        self->renderTarget_->Clear(D2D1::ColorF(D2D1::ColorF::White));
        self->paintBoxes(self->renderTarget_);
        self->paintRoot(self->renderTarget_);
        HRESULT hr = self->renderTarget_->EndDraw();
        // D2DERR_RECREATE_TARGET means the underlying device is gone
        // (driver reset, GPU removal, etc.) — drop the target so the next
        // WM_PAINT's ensureRenderTarget() rebuilds it from scratch.
        if (hr == D2DERR_RECREATE_TARGET)
        {
          self->renderTarget_->Release();
          self->renderTarget_ = nullptr;
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }

    // Tells Windows we're handling the background ourselves (see
    // WM_PAINT), so it should skip its own default erase-to-brush pass —
    // that default erase is what caused a visible white/gray flash right
    // before every repaint.
    case WM_ERASEBKGND:
      return 1;

    // Left mouse button pressed: first give scrollbars/scrollable content a
    // chance to claim the press (beginScrollPress) — a thumb grab or track
    // click consumes it entirely; a press over plain scrollable content
    // arms a possible pan but still falls through to the ordinary
    // beginPress() below, since a small movement should still resolve as a
    // click (see updateScrollDrag/endScrollPress). Capture the mouse so we
    // still get the matching WM_MOUSEMOVE/WM_LBUTTONUP even if the cursor
    // leaves the window before the button is released.
    case WM_LBUTTONDOWN:
    {
      if (self)
      {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (!self->beginScrollPress(x, y))
          self->beginPress(x, y);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Pointer moved with a button held: advances an in-progress scrollbar
    // drag or content pan. No-op (returns false) if neither is active, so
    // this costs nothing on ordinary hover.
    case WM_MOUSEMOVE:
    {
      if (self)
      {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->updateScrollDrag(x, y) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Left mouse button released: resolve the pending press against
    // whatever's under the cursor now, firing onClick only if it matches
    // the original press target (endScrollPress handles the scroll-drag
    // side of this and defers to endPress() when a pan never actually
    // moved, i.e. it was really just a click).
    case WM_LBUTTONUP:
    {
      if (self)
      {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->endScrollPress(x, y);
      }
      ReleaseCapture();
      return 0;
    }

    // Vertical mouse-wheel rotation. WM_MOUSEWHEEL's cursor coordinates are
    // in *screen* space (unlike every other mouse message here, which are
    // client-space) — ScreenToClient converts before hit-testing. One
    // notch (WHEEL_DELTA = 120) scrolls a fixed 40px step; larger/precision
    // wheels report multiples/fractions of that.
    case WM_MOUSEWHEEL:
    {
      if (self)
      {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        // Wheel-up (positive notches) should scroll content up, i.e.
        // decrease scrollY — hence the negation.
        if (self->applyWheelScroll(static_cast<float>(pt.x),
                                   static_cast<float>(pt.y), 0.0f,
                                   -notches * 40.0f) &&
            self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Horizontal mouse-wheel rotation (tilt-wheel or shift+wheel on most
    // drivers). Same coordinate/notch handling as WM_MOUSEWHEEL, but
    // positive notches scroll right, so no negation here.
    case WM_MOUSEHWHEEL:
    {
      if (self)
      {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        if (self->applyWheelScroll(static_cast<float>(pt.x),
                                   static_cast<float>(pt.y), notches * 40.0f,
                                   0.0f) &&
            self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Capture was taken away from us mid-press (e.g. alt-tab, a system
    // dialog popping up) — the click/drag can't complete normally, so drop
    // both the pending click and any in-progress scroll drag rather than
    // let a later, unrelated event resolve them.
    case WM_CAPTURECHANGED:
      if (self)
      {
        self->pressedView_ = nullptr;
        self->scrollDrag_ = {};
      }
      return 0;

    // Window was resized (including maximize/restore/snap): update our
    // stored dimensions and re-run layout against the new size. GDI needs
    // no buffer reallocation (it paints straight into the window's DC), so
    // this is just relayout + repaint.
    case WM_SIZE:
    {
      if (self)
      {
        self->width_ = LOWORD(lp);
        self->height_ = HIWORD(lp);
        if (self->renderTarget_)
          self->renderTarget_->Resize(
              D2D1::SizeU(static_cast<UINT32>(self->width_),
                          static_cast<UINT32>(self->height_)));
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

  // Draws every queued box into the render target.
  void paintBoxes(ID2D1RenderTarget *rt)
  {
    for (const auto &b : boxes_)
      d2dFillRect(rt, static_cast<float>(b.pos_x), static_cast<float>(b.pos_y),
                  static_cast<float>(b.width), static_cast<float>(b.height),
                  b.color);
  }

  // Draws the layout tree (if any). Border-radius is handled natively by
  // D2D1_ROUNDED_RECT — no manual pixel math needed on this platform,
  // same as the old RoundRect approach, just anti-aliased for free.
  void paintRoot(ID2D1RenderTarget *rt)
  {
    if (hasRoot_)
    {
      paintView(rt, root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      sortAbsolutes(absolutes);
      for (const auto &e : absolutes)
        paintView(rt, *e.view, ClipRect{});
    }
  }

  // Draws v's vertical/horizontal scrollbar (whichever are currently
  // showing) using flat fills — same classic fixed track+thumb styling as
  // before, unclipped by the content clip beyond whatever `clip` already
  // restricts (a scrollbar always sits fully within its own view's box,
  // which is itself already visible or this function wouldn't have been
  // reached).
  void paintScrollbars(ID2D1RenderTarget *rt, const View &v)
  {
    if (wantVBar(v))
    {
      PixRect t = vTrackRect(v), th = vThumbRect(v);
      d2dFillRect(rt, t.x, t.y, t.w, t.h, {0xE0, 0xE0, 0xE0});
      d2dFillRect(rt, th.x, th.y, th.w, th.h, {0x90, 0x90, 0x90});
    }
    if (wantHBar(v))
    {
      PixRect t = hTrackRect(v), th = hThumbRect(v);
      d2dFillRect(rt, t.x, t.y, t.w, t.h, {0xE0, 0xE0, 0xE0});
      d2dFillRect(rt, th.x, th.y, th.w, th.h, {0x90, 0x90, 0x90});
    }
    // Corner filler where both bars would otherwise leave a gap/overlap.
    if (wantVBar(v) && wantHBar(v))
      d2dFillRect(rt, v.computed.x + v.computed.w - kScrollbarThickness,
                  v.computed.y + v.computed.h - kScrollbarThickness,
                  kScrollbarThickness, kScrollbarThickness, {0xE0, 0xE0, 0xE0});
  }

  // `clip` is the accumulated visible region from scrollable ancestors.
  // Direct2D's clip stack is push/pop rather than GDI's set-and-restore,
  // but since this function is itself called recursively (one call frame
  // per View), pushing on entry and popping on exit naturally nests
  // correctly with the call tree — no need to save/restore a previous
  // clip handle the way SelectClipRgn did.
  void paintView(ID2D1RenderTarget *rt, const View &v, ClipRect clip)
  {
    const Style &s = v.style;
    float x = v.computed.x, y = v.computed.y, w = v.computed.w,
          h = v.computed.h;

    bool clipped = clip.x0 != -std::numeric_limits<float>::infinity() ||
                   clip.y0 != -std::numeric_limits<float>::infinity() ||
                   clip.x1 != std::numeric_limits<float>::infinity() ||
                   clip.y1 != std::numeric_limits<float>::infinity();
    if (clipped)
      rt->PushAxisAlignedClip(D2D1::RectF(clip.x0, clip.y0, clip.x1, clip.y1),
                              D2D1_ANTIALIAS_MODE_ALIASED);

    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x + w, y + h), s.borderRadius,
                            s.borderRadius};
    ID2D1SolidColorBrush *bgBrush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(s.backgroundColor), &bgBrush);
    if (bgBrush)
    {
      rt->FillRoundedRectangle(rr, bgBrush);
      bgBrush->Release();
    }
    if (s.borderWidth > 0)
    {
      // D2D strokes are centered on the path (half in, half out), unlike
      // the old GDI approach of an outer full-color box plus an inset
      // background rect — visually equivalent for a uniform border, just
      // computed differently.
      ID2D1SolidColorBrush *borderBrush = nullptr;
      rt->CreateSolidColorBrush(toD2DColor(s.borderColor), &borderBrush);
      if (borderBrush)
      {
        rt->DrawRoundedRectangle(rr, borderBrush, s.borderWidth);
        borderBrush->Release();
      }
    }

    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute)
        paintView(rt, child, childClip);

    // Scrollbars are drawn after children, still under v's own (ancestor,
    // not child-narrowed) clip — we haven't popped it yet, so this is
    // still exactly `clip`, matching the old GDI re-selection semantics.
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them on top the way an overlay
    // scrollbar should be.
    if (v.scrollsX() || v.scrollsY())
      paintScrollbars(rt, v);

    if (clipped)
      rt->PopAxisAlignedClip();
  }

#else // Linux / Wayland

  wl_display *display_ =
      nullptr; // Connection handle to the Wayland compositor; null until
               // wl_display_connect succeeds.
  wl_compositor *compositor_ =
      nullptr;                      // The compositor global, used to create surfaces.
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
      0;                          // Serial from the most recent pointer-enter event;
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

  // ---- EGL / GLES2 ----
  // GPU-accelerated replacement for the old wl_shm double buffer. The
  // compositor owns presentation timing via eglSwapBuffers (backed by
  // wayland-egl's own internal buffer queue), so there's no manual
  // busy-tracking/retry logic to maintain anymore — swap is synchronous
  // from our point of view.
  wl_egl_window *eglWindow_ = nullptr;
  EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
  EGLContext eglContext_ = EGL_NO_CONTEXT;
  EGLSurface eglSurface_ = EGL_NO_SURFACE;
  bool eglReady_ = false; // true once initEgl() has completed

  GLuint rectProgram_ = 0, flatProgram_ = 0;
  GLuint quadVbo_ = 0; // static unit quad, reused by every rectProgram_ draw
  GLuint flatVbo_ = 0; // rewritten per-call for flatProgram_ (line quads)
  GLint rectAPos_ = -1, rectUPos_ = -1, rectUSize_ = -1, rectUScreen_ = -1,
        rectURadius_ = -1, rectUColor_ = -1;
  GLint flatAPos_ = -1, flatUScreen_ = -1, flatUColor_ = -1;

  bool configured_ =
      false;              // Set true once the compositor has sent its first
                          // configure event, meaning we're allowed to attach a buffer.
  int pendingWidth_ = 0;  // Size most recently suggested by
  int pendingHeight_ = 0; // toplevelConfigure; 0 means "no suggestion yet"
                          // (the compositor may send 0x0 to mean "you decide").
  bool running_ = true;   // Controls the event loop in run(); set false to
                          // request a clean exit.

  // ---- GLES2 rendering ----
  // Two tiny shader programs cover everything this UI draws:
  //  - rectProgram_: an axis-aligned box with an optional rounded-corner
  //    radius, via a signed-distance test in the fragment shader. Used for
  //    all backgrounds/borders/scrollbars (radius 0 == a plain rect).
  //  - flatProgram_: an arbitrary pixel-space quad with a flat color. Used
  //    only for drawLine's thick-line quads (titlebar icons).
  static constexpr const char *kRectVS = R"(
    precision mediump float;
    attribute vec2 a_pos;      // unit quad, 0..1
    uniform vec2 u_rectPos;    // top-left, window pixels
    uniform vec2 u_rectSize;   // pixels
    uniform vec2 u_screen;     // window size, pixels
    varying vec2 v_local;      // pixel offset within the rect
    void main() {
      v_local = a_pos * u_rectSize;
      vec2 px = u_rectPos + a_pos * u_rectSize;
      gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,
                         1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kRectFS = R"(
    precision mediump float;
    varying vec2 v_local;
    uniform vec2 u_rectSize;
    uniform float u_radius;
    uniform vec4 u_color;
    void main() {
      vec2 half_ = u_rectSize * 0.5;
      vec2 d = abs(v_local - half_) - (half_ - u_radius);
      float dist = length(max(d, 0.0)) - u_radius;
      float alpha = 1.0 - smoothstep(-1.0, 1.0, dist);
      gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
    }
  )";
  static constexpr const char *kFlatVS = R"(
    precision mediump float;
    attribute vec2 a_pos;   // window pixels, explicit per-vertex
    uniform vec2 u_screen;
    void main() {
      gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0,
                         1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kFlatFS = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() { gl_FragColor = u_color; }
  )";

  static GLuint compileShader(GLenum type, const char *src)
  {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
      char log[512];
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      fprintf(stderr, "shader compile error: %s\n", log);
      throw std::runtime_error("GLSL shader compile failed");
    }
    return s;
  }
  static GLuint linkProgram(const char *vs, const char *fs)
  {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok)
    {
      char log[512];
      glGetProgramInfoLog(p, sizeof(log), nullptr, log);
      fprintf(stderr, "program link error: %s\n", log);
      throw std::runtime_error("GLSL program link failed");
    }
    return p;
  }

  // xdg_wm_base ping handler: the compositor periodically checks we're alive.
  static void wmBasePing(void *, xdg_wm_base *base, uint32_t serial)
  {
    // Echo the serial straight back so the compositor knows we're responsive.
    xdg_wm_base_pong(base, serial);
  }
  // Listener struct binding the ping callback above to xdg_wm_base's single
  // event.
  static constexpr xdg_wm_base_listener wmBaseListener = {wmBasePing};

  // Called when the compositor wants us to (re)configure our xdg_surface.
  static void surfaceConfigure(void *data, xdg_surface *xs, uint32_t serial)
  {
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
         self->pendingHeight_ != self->height_))
    {
      self->resize(self->pendingWidth_, self->pendingHeight_);
    }
    else if (!self->eglReady_)
      self->initEgl();
  }
  // Listener struct binding surfaceConfigure to xdg_surface's single event.
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  // Called when the compositor suggests a new size/state for the toplevel.
  static void toplevelConfigure(void *data, xdg_toplevel *, int32_t width,
                                int32_t height, wl_array *)
  {
    // 0x0 means "you decide the size" — keep whatever we currently have.
    // The actual resize happens later, in surfaceConfigure, once this
    // configure is ack'd (that's the point at which the protocol allows us
    // to attach a differently-sized buffer).
    auto *self = static_cast<LiteUI *>(data);
    if (width > 0 && height > 0)
    {
      self->pendingWidth_ = width;
      self->pendingHeight_ = height;
    }
  }
  // Called when the compositor/user requests the window be closed (e.g. via a
  // taskbar close action).
  static void toplevelClose(void *data, xdg_toplevel *)
  {
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
  static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps)
  {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Only act if the seat has a pointer and we haven't already grabbed one.
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_)
    {
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
                           wl_surface *, wl_fixed_t sx, wl_fixed_t sy)
  {
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
  static void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *)
  {
    static_cast<LiteUI *>(data)->currentCursorName_.clear();
  }
  // Called on every pointer movement while over this surface.
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy)
  {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Update the stored x position.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Update the stored y position.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // Advance any in-progress scrollbar drag / content pan (see
    // beginScrollPress/handlePress). A no-op, and cheap, when nothing's
    // being dragged.
    if (self->updateScrollDrag(static_cast<float>(self->pointer_x_),
                               static_cast<float>(self->pointer_y_)))
      self->redraw();

    // Re-derive which edge (if any) the pointer is over and update the
    // cursor image to match, so the user sees a resize cursor before they
    // even click.
    self->setCursor(cursorNameForEdge(
        self->resizeEdgeAt(self->pointer_x_, self->pointer_y_)));
  }
  // Called on scroll/axis events — mouse wheel rotation or a touchpad's
  // continuous scroll gesture. `value` is already a relative-movement
  // amount in the same coordinate space as pointer motion (per the
  // wl_pointer protocol), so it's usable directly as a pixel delta with no
  // extra scaling, unlike Windows' notch-based WM_MOUSEWHEEL.
  static void pointerAxis(void *data, wl_pointer *, uint32_t, uint32_t axis,
                          wl_fixed_t value)
  {
    auto *self = static_cast<LiteUI *>(data);
    float delta = static_cast<float>(wl_fixed_to_double(value));
    float dx = 0, dy = 0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
      dy = delta;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
      dx = delta;
    if (self->applyWheelScroll(static_cast<float>(self->pointer_x_),
                               static_cast<float>(self->pointer_y_), dx, dy))
      self->redraw();
  }
  // Called on every pointer button press/release.
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state)
  {
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
                             const char *interface, uint32_t)
  {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // If this global is the compositor interface...
    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
      // ...bind to it at version 4 and store the resulting proxy.
      self->compositor_ = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    // If instead this global is the xdg_wm_base (window-shell) interface...
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
      // ...bind to it at version 1...
      self->wm_base_ = static_cast<xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      // ...and register the ping/pong listener on it immediately.
      xdg_wm_base_add_listener(self->wm_base_, &wmBaseListener, self);
    }
    // If instead this global is the shared-memory interface...
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
      // ...bind to it so we can later allocate pixel buffers.
      self->shm_ = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    // If instead this global is the seat (input devices) interface...
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
      // ...bind to it...
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      // ...and register the capabilities/name listener so we learn about the
      // pointer.
      wl_seat_add_listener(self->seat_, &seatListener, self);
    }
    // If instead this global is the decoration-manager interface...
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0)
    {
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
  uint32_t resizeEdgeAt(double px, double py) const
  {
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
  static const char *cursorNameForEdge(uint32_t edge)
  {
    switch (edge)
    {
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
  void setCursor(const char *name)
  {
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

  // Handles a compositor-driven resize: tears down the old shm buffers,
  // updates width_/height_, re-runs layout against the new size, and
  // allocates+attaches fresh buffers at the new dimensions. Buffers can't
  // be resized in place — wl_shm buffers are fixed-size — so this is a full
  // destroy/recreate rather than a realloc.
  void resize(int newWidth, int newHeight)
  {

    width_ = newWidth;
    height_ = newHeight;
    if (eglWindow_)
      wl_egl_window_resize(eglWindow_, width_, height_, 0, 0);
    relayout();
    if (eglReady_)
      redraw();
  }

  // Creates the EGL context/surface for this window's wl_surface and
  // compiles the two shader programs. Called once, from surfaceConfigure(),
  // the first time the compositor hands us a configure event — mirroring
  // when attachBuffer() used to run.
  void initEgl()
  {
    // eglGetDisplay() can't reliably tell "this pointer is a wl_display*"
    // apart from other native display types on multi-platform Mesa
    // builds — on some setups it silently falls back to the generic
    // "device" platform, which tries to open a DRM fd (fails with -1)
    // and then falls further back to a broken software/zink path. Asking
    // for EGL_PLATFORM_WAYLAND_KHR explicitly removes the guesswork.
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (eglGetPlatformDisplayEXT)
    {
      eglDisplay_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT,
                                             display_, nullptr);
    }
    else
    {
      eglDisplay_ =
          eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    }
    if (eglDisplay_ == EGL_NO_DISPLAY)
      throw std::runtime_error("eglGetDisplay failed");
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor))
      throw std::runtime_error("eglInitialize failed");
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfgAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE};
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, cfgAttribs, &config, 1, &numConfigs) ||
        numConfigs < 1)
      throw std::runtime_error("eglChooseConfig failed");

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    eglContext_ = eglCreateContext(eglDisplay_, config, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext_ == EGL_NO_CONTEXT)
      throw std::runtime_error("eglCreateContext failed");

    eglWindow_ = wl_egl_window_create(surface_, width_, height_);
    if (!eglWindow_)
      throw std::runtime_error("wl_egl_window_create failed");

    eglSurface_ = eglCreateWindowSurface(
        eglDisplay_, config, reinterpret_cast<EGLNativeWindowType>(eglWindow_), nullptr);
    if (eglSurface_ == EGL_NO_SURFACE)
      throw std::runtime_error("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_))
      throw std::runtime_error("eglMakeCurrent failed");
    eglSwapInterval(eglDisplay_, 1); // vsync — avoids tearing during scroll drags

    // Tell the compositor this whole surface is opaque, so it can skip
    // blending against the desktop entirely rather than relying solely on
    // the alpha-less EGL config above. INT32_MAX as the region size is the
    // standard "cover the whole surface regardless of actual dimensions"
    // idiom — Wayland clips it to the surface's real bounds internally.
    wl_region *opaque = wl_compositor_create_region(compositor_);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface_, opaque);
    wl_region_destroy(opaque);

    rectProgram_ = linkProgram(kRectVS, kRectFS);
    rectAPos_ = glGetAttribLocation(rectProgram_, "a_pos");
    rectUPos_ = glGetUniformLocation(rectProgram_, "u_rectPos");
    rectUSize_ = glGetUniformLocation(rectProgram_, "u_rectSize");
    rectUScreen_ = glGetUniformLocation(rectProgram_, "u_screen");
    rectURadius_ = glGetUniformLocation(rectProgram_, "u_radius");
    rectUColor_ = glGetUniformLocation(rectProgram_, "u_color");

    flatProgram_ = linkProgram(kFlatVS, kFlatFS);
    flatAPos_ = glGetAttribLocation(flatProgram_, "a_pos");
    flatUScreen_ = glGetUniformLocation(flatProgram_, "u_screen");
    flatUColor_ = glGetUniformLocation(flatProgram_, "u_color");

    static const float kUnitQuad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
    glGenBuffers(1, &flatVbo_); // contents supplied per-draw in drawLine()

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST); // ClipRect maps straight onto glScissor

    eglReady_ = true;
    redraw();
  }

  // Restricts subsequent draws to `clip`, intersected with the window
  // bounds. GL's scissor origin is bottom-left, so the y range is flipped
  // relative to ClipRect's top-left/y-down convention.
  void applyScissor(const ClipRect &clip)
  {
    float x0 = std::max(clip.x0, 0.0f), y0 = std::max(clip.y0, 0.0f);
    float x1 = std::min(clip.x1, static_cast<float>(width_));
    float y1 = std::min(clip.y1, static_cast<float>(height_));
    if (x1 < x0)
      x1 = x0;
    if (y1 < y0)
      y1 = y0;
    glScissor(static_cast<int>(x0), static_cast<int>(height_ - y1),
              static_cast<int>(x1 - x0), static_cast<int>(y1 - y0));
  }

  // Draws one filled, optionally rounded-corner rectangle. radius <= 0
  // renders a plain rect (same shader — the SDF collapses correctly).
  void drawRectGL(float x, float y, float w, float h, float radius, Color c,
                  const ClipRect &clip)
  {
    if (w <= 0 || h <= 0)
      return;
    applyScissor(clip);
    glUseProgram(rectProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(rectAPos_);
    glVertexAttribPointer(rectAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(rectUPos_, x, y);
    glUniform2f(rectUSize_, w, h);
    glUniform2f(rectUScreen_, static_cast<float>(width_), static_cast<float>(height_));
    glUniform1f(rectURadius_, std::max(0.0f, radius));
    glUniform4f(rectUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Thick line as an oriented quad — needed (rather than a bbox rect) since
  // drawTitlebar uses this for the X icon's two diagonal strokes.
  void drawLineGL(float x0, float y0, float x1, float y1, Color c,
                  float thickness = 2.0f)
  {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    float nx = len > 0.0001f ? -dy / len : 1.0f;
    float ny = len > 0.0001f ? dx / len : 0.0f;
    float hw = thickness / 2.0f;
    const float verts[8] = {x0 + nx * hw, y0 + ny * hw, x0 - nx * hw, y0 - ny * hw,
                            x1 + nx * hw, y1 + ny * hw, x1 - nx * hw, y1 - ny * hw};
    applyScissor(ClipRect{}); // titlebar icons are never clipped
    glUseProgram(flatProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, flatVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(flatAPos_);
    glVertexAttribPointer(flatAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(flatUScreen_, static_cast<float>(width_), static_cast<float>(height_));
    glUniform4f(flatUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Fills an axis-aligned rectangle with a solid color by calling setPixel for
  // every point inside it. Same unclipped/clipped overload split as setPixel.
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
  {
    fillRect(x0, y0, w, h, r, g, b, ClipRect{});
  }
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b,
                const ClipRect &clip)
  {
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h), 0.0f, {r, g, b}, clip);
  }

  // Fills an axis-aligned rectangle with rounded corners, clamping the
  // radius so it can't exceed half the shorter side. Per-pixel distance
  // check against each corner's circle center — fine at this scale, not
  // meant for huge boxes. Same unclipped/clipped overload split as setPixel.
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b)
  {
    fillRoundedRect(x0, y0, w, h, radius, r, g, b, ClipRect{});
  }
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b, const ClipRect &clip)
  {
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h),
               static_cast<float>(radius), {r, g, b}, clip);
  }

  // Draws v's vertical/horizontal scrollbar (whichever are currently
  // showing) as flat filled rectangles — classic fixed track+thumb
  // styling, matching the Windows/GDI renderer's look. `clip` is v's own
  // ancestor-level clip (not narrowed by v's own children), so a
  // scrollbar correctly disappears if v itself has scrolled out of some
  // outer ancestor's viewport, same reasoning as the GDI version.
  void renderScrollbars(const View &v, const ClipRect &clip)
  {
    if (wantVBar(v))
    {
      PixRect t = vTrackRect(v), th = vThumbRect(v);
      fillRect(static_cast<int>(t.x), static_cast<int>(t.y),
               static_cast<int>(t.w), static_cast<int>(t.h), 0xE0, 0xE0, 0xE0,
               clip);
      fillRect(static_cast<int>(th.x), static_cast<int>(th.y),
               static_cast<int>(th.w), static_cast<int>(th.h), 0x90, 0x90, 0x90,
               clip);
    }
    if (wantHBar(v))
    {
      PixRect t = hTrackRect(v), th = hThumbRect(v);
      fillRect(static_cast<int>(t.x), static_cast<int>(t.y),
               static_cast<int>(t.w), static_cast<int>(t.h), 0xE0, 0xE0, 0xE0,
               clip);
      fillRect(static_cast<int>(th.x), static_cast<int>(th.y),
               static_cast<int>(th.w), static_cast<int>(th.h), 0x90, 0x90, 0x90,
               clip);
    }
    if (wantVBar(v) && wantHBar(v))
      fillRect(
          static_cast<int>(v.computed.x + v.computed.w - kScrollbarThickness),
          static_cast<int>(v.computed.y + v.computed.h - kScrollbarThickness),
          static_cast<int>(kScrollbarThickness),
          static_cast<int>(kScrollbarThickness), 0xE0, 0xE0, 0xE0, clip);
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
  //
  // `clip` is the accumulated intersection of every scrollable ancestor's
  // viewport, exactly mirroring hitTestFlow()'s clip parameter — v's own
  // background/border is drawn under `clip` (the ancestor-level one, not
  // narrowed by v itself), while children are drawn under a further-
  // narrowed clip if v itself scrolls, which is what actually makes
  // scrolled-out content invisible instead of just mispositioned.
  void renderView(const View &v, ClipRect clip)
  {
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    int radius = static_cast<int>(s.borderRadius);
    if (s.borderWidth > 0)
    {
      fillRoundedRect(x, y, w, h, radius, s.borderColor.r, s.borderColor.g,
                      s.borderColor.b, clip);
      int bw = static_cast<int>(s.borderWidth);
      fillRoundedRect(x + bw, y + bw, std::max(0, w - 2 * bw),
                      std::max(0, h - 2 * bw), std::max(0, radius - bw),
                      s.backgroundColor.r, s.backgroundColor.g,
                      s.backgroundColor.b, clip);
    }
    else
    {
      fillRoundedRect(x, y, w, h, radius, s.backgroundColor.r,
                      s.backgroundColor.g, s.backgroundColor.b, clip);
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute)
        renderView(child, childClip);
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them visually on top.
    if (v.scrollsX() || v.scrollsY())
      renderScrollbars(v, clip);
  }

  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  // Draws a crude line between two points by linear interpolation, stepping
  // once per pixel along the longer axis.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                uint8_t b)
  {
    drawLineGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(x1), static_cast<float>(y1), {r, g, b});
  }

  // ---- titlebar button layout ----
  // Plain axis-aligned rectangle used for button hit-testing.
  struct Rect
  {
    int x, y, w, h;
  };
  // Returns whether point (px, py) falls within rectangle r (using half-open
  // bounds).
  static bool inside(const Rect &r, double px, double py)
  {
    // Standard axis-aligned bounding box containment test.
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }
  // Computes the close button's rectangle: flush against the right edge of the
  // titlebar.
  Rect closeRect() const
  {
    // x is inset from the right edge by the margin and the button's own width;
    // y centers it vertically in the titlebar.
    return {width_ - kButtonMargin - kButtonSize,
            (kTitlebarHeight - kButtonSize) / 2, kButtonSize, kButtonSize};
  }
  // Computes the maximize button's rectangle, positioned one button-width left
  // of the close button.
  Rect maximizeRect() const
  {
    // Start from the close button's rectangle as a reference point.
    Rect c = closeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {c.x - kButtonMargin - kButtonSize, c.y, kButtonSize, kButtonSize};
  }
  // Computes the minimize button's rectangle, positioned one button-width left
  // of the maximize button.
  Rect minimizeRect() const
  {
    // Start from the maximize button's rectangle as a reference point.
    Rect m = maximizeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {m.x - kButtonMargin - kButtonSize, m.y, kButtonSize, kButtonSize};
  }

  // Paints the custom titlebar background and its three buttons/icons.
  void drawTitlebar()
  {
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
  // Draws into whichever of the two buffers (drawBuf_) isn't currently
  // still owned by the compositor — see the "Double buffering" comment
  // block for why that matters. If it's still busy, the draw is skipped
  // entirely (never blocks) and redrawPending_ is set so bufferRelease()
  // retries automatically the moment that buffer frees up — a call to
  // redraw() should therefore be thought of as "request a repaint soon",
  // not "repaint synchronously right now".
  void redraw()
  {
    if (!eglReady_)
      return;
    glViewport(0, 0, width_, height_);
    applyScissor(ClipRect{});
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f); // plain white content-area background
    glClear(GL_COLOR_BUFFER_BIT);
    // Paint queued boxes on top of the plain background, before the titlebar
    // so it stays on top.
    for (const auto &b : boxes_)
      fillRect(b.pos_x, b.pos_y, b.width, b.height, b.color.r, b.color.g,
               b.color.b);
    if (hasRoot_)
    {
      renderView(root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      std::stable_sort(absolutes.begin(), absolutes.end(),
                       [](const AbsoluteEntry &a, const AbsoluteEntry &b)
                       {
                         if (a.view->style.zIndex != b.view->style.zIndex)
                           return a.view->style.zIndex < b.view->style.zIndex;
                         return a.order < b.order;
                       });
      for (const auto &e : absolutes)
        renderView(*e.view, ClipRect{});
    }
    // Paint the titlebar and its buttons on top of that background.
    drawTitlebar();
    eglSwapBuffers(eglDisplay_, eglSurface_);
  }

  // Hit-tests a left-button press against the titlebar buttons, resize
  // edges, and drag region. Resize/move grabs and the chrome buttons
  // (close/maximize/minimize) still act immediately on press, same as
  // before — only content-area widget clicks wait for a matching release
  // (see beginPress/endPress).
  void handlePress(uint32_t serial)
  {
    // Clicks within kResizeMargin of any outer edge start an interactive
    // resize instead — checked first since the resize strip along the top
    // overlaps the first few pixels of the titlebar itself.
    uint32_t edge = resizeEdgeAt(pointer_x_, pointer_y_);
    if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE)
    {
      if (seat_)
        xdg_toplevel_resize(toplevel_, seat_, serial, edge);
      return;
    }

    // Presses below the titlebar strip first give scrollbars/scrollable
    // content a chance to claim the press (beginScrollPress) — a thumb
    // grab or track click consumes it entirely; anything else falls
    // through to the ordinary pending-click press, resolved later in
    // handleRelease().
    if (pointer_y_ >= kTitlebarHeight)
    {
      float x = static_cast<float>(pointer_x_),
            y = static_cast<float>(pointer_y_);
      if (!beginScrollPress(x, y))
        beginPress(x, y);
      redraw();
      return;
    }

    // If the click landed on the close button...
    if (inside(closeRect(), pointer_x_, pointer_y_))
    {
      // ...request the event loop to stop, ending run().
      running_ = false;
      return;
    }
    // If instead the click landed on the maximize button...
    if (inside(maximizeRect(), pointer_x_, pointer_y_))
    {
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
    if (inside(minimizeRect(), pointer_x_, pointer_y_))
    {
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

  // Resolves a pending widget click (or in-progress scroll drag) on button
  // release. Chrome buttons and resize/move grabs don't need this — they
  // already acted on press — so this only matters for content-area
  // interactions below the titlebar.
  void handleRelease()
  {
    if (pointer_y_ >= kTitlebarHeight)
      endScrollPress(static_cast<float>(pointer_x_),
                     static_cast<float>(pointer_y_));
    else
      pressedView_ = nullptr; // release moved back into the titlebar; cancel
    scrollDrag_ = {};
  }

// Ends the Windows/Linux member block.
#endif
};

// Out-of-line constructor definition; inline because this is a single-header
// library.
inline LiteUI::LiteUI(int w, int h, const std::string &title)
    : width_(w), height_(h)
{
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
  if (!hwnd_)
  {
    throw std::runtime_error("CreateWindowExW failed");
  }

  // One factory per LiteUI instance is simplest here; a real app with many
  // windows would normally share a single process-wide factory instead.
  if (FAILED(
          D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_)))
    throw std::runtime_error("D2D1CreateFactory failed");
  ensureRenderTarget();

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
inline LiteUI::~LiteUI()
{
// Windows-specific teardown path.
#if defined(_WIN32)
  // Release Direct2D resources — order matters: the render target must
  // go before the factory that created it.
  if (renderTarget_)
    renderTarget_->Release();
  if (d2dFactory_)
    d2dFactory_->Release();
  // Destroy the native window if it was successfully created.
  if (hwnd_)
    DestroyWindow(hwnd_);
// Linux/Wayland-specific teardown path.
#else
  if (eglDisplay_ != EGL_NO_DISPLAY)
  {
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (rectProgram_)
      glDeleteProgram(rectProgram_);
    if (flatProgram_)
      glDeleteProgram(flatProgram_);
    if (quadVbo_)
      glDeleteBuffers(1, &quadVbo_);
    if (flatVbo_)
      glDeleteBuffers(1, &flatVbo_);
    if (eglSurface_ != EGL_NO_SURFACE)
      eglDestroySurface(eglDisplay_, eglSurface_);
    if (eglContext_ != EGL_NO_CONTEXT)
      eglDestroyContext(eglDisplay_, eglContext_);
    eglTerminate(eglDisplay_);
  }
  if (eglWindow_)
    wl_egl_window_destroy(eglWindow_);
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
inline void LiteUI::setRoot(View view)
{
  root_ = std::move(view);
  hasRoot_ = true;
  relayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (eglReady_)
    redraw();
#endif
}

inline void LiteUI::addBox(const Box &box)
{
  boxes_.push_back(box);
#if defined(_WIN32)
  // Ask Windows to repaint; the actual drawing happens in WM_PAINT.
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  // Repaint immediately if the buffer already exists; if it doesn't yet,
  // attachBuffer()'s own redraw() will pick up boxes_ once configured.
  if (eglReady_)
    redraw();
#endif
}

// Out-of-line definition of the blocking event loop.
inline void LiteUI::run()
{
// Windows-specific message loop.
#if defined(_WIN32)
  // Storage for each retrieved message.
  MSG msg;
  // GetMessage blocks until a message arrives and returns 0 on WM_QUIT, ending
  // the loop.
  while (GetMessage(&msg, nullptr, 0, 0))
  {
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
  while (running_ && wl_display_dispatch(display_) != -1)
  {
    // event loop
  }
// Ends the Windows/Linux run() branch.
#endif
}