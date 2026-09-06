// A tiny MS-Paint-style app: a color palette, three brush sizes, a clear
// button, and a fixed-size "document" page you draw on, sitting inside a
// scrollable gray viewport (the same document-vs-viewport split MS Paint
// and Photoshop use — see the comment above kDocW/kDocH below).
//
// All drawing state (the list of strokes, current color/size) lives in
// `PaintState` below and is captured by reference into the view tree's
// lambdas. The canvas itself never keeps its own picture between mouse
// moves — every redraw just replays `state.strokes` from scratch onto a
// fresh transparent surface (see the canvas's onPaint). That's simple and
// plenty fast for an app like this; a resolution-independent "redraw the
// whole scene" is the same approach real vector-graphics apps use.

#include "liteui.hpp"

#include <cmath>
#include <vector>

// The document's fixed pixel size — this is the actual "page" (like
// Paint's or Photoshop's canvas size), independent of the window size.
// A4 at 96 DPI is roughly 816x1056; pick whatever fits your use case.
// Everything else in this file (the gray viewport around it, scrolling)
// stays the same regardless of what you set this to.
constexpr float kDocW = 816.0f;
constexpr float kDocH = 1056.0f;

// Zoom range/step for the viewport controls added below.
constexpr float kZoomMin = 0.25f;
constexpr float kZoomMax = 4.0f;
constexpr float kZoomStep = 1.25f;

struct Point {
  float x, y;
};

struct Stroke {
  std::vector<Point> pts;
  Color color;
  float width;
};

struct PaintState {
  std::vector<Stroke> strokes;
  Color currentColor{20, 20, 20, 255};
  float currentWidth = 4.0f;
  float zoom = 1.0f;    // 1.0 = 100%; scales the doc at paint time
  bool panMode = false; // when true, drags pan instead of drawing

  // Polled by the canvas's canvasDirtySource (see liteui.hpp's own note on
  // View::canvasDirtySource) since onPressAt/onDragTo/onClick have no
  // direct reference back into the tree to call requestCanvasRedraw()
  // themselves.
  bool dirty = true;
  void markDirty() { dirty = true; }
};

// File-scope so buildRoot()/setZoom() (added below) can both reach it —
// previously this lived as a local `static` inside main().
static PaintState state;
static LiteUI *g_ui = nullptr;

// Draws every accumulated stroke. A single-point "stroke" (a plain click,
// no drag) is drawn as a filled dot so a tap still leaves a visible mark.
static void paintCanvas(CanvasContext &ctx, const PaintState &state) {
  ctx.setFillColor({255, 255, 255, 255});
  ctx.fillRect(0, 0, ctx.width(), ctx.height());
  // Everything below is drawn in fixed document units; scaling here
  // (rather than scaling stroke coordinates themselves) means strokes
  // never need to be rewritten when zoom changes.
  ctx.save();
  ctx.scale(state.zoom, state.zoom);
  for (const Stroke &s : state.strokes) {
    if (s.pts.empty())
      continue;
    if (s.pts.size() == 1) {
      ctx.beginPath();
      ctx.arc(s.pts[0].x, s.pts[0].y, s.width / 2.0f, 0, 6.2831853f);
      ctx.setFillColor(s.color);
      ctx.fill();
      continue;
    }
    ctx.beginPath();
    ctx.moveTo(s.pts[0].x, s.pts[0].y);
    for (size_t i = 1; i < s.pts.size(); ++i)
      ctx.lineTo(s.pts[i].x, s.pts[i].y);
    ctx.setStrokeColor(s.color);
    ctx.setLineWidth(s.width);
    ctx.setLineCap(LineCap::Round);
    ctx.setLineJoin(LineJoin::Round);
    ctx.stroke();
  }
  ctx.restore();
}

// A small square swatch button. Clicking it makes `state.currentColor`
// this swatch's color; `selected` gets a slightly heavier border so the
// active color is easy to spot.
static View colorSwatch(Color color, PaintState &state, bool selected) {
  View v;
  v.style.width = Size::pixel(28);
  v.style.height = Size::pixel(28);
  v.style.backgroundColor = color;
  v.style.borderRadius = 6;
  v.style.borderWidth = selected ? 3 : 1;
  v.style.borderColor =
      selected ? Color{40, 120, 220, 255} : Color{160, 160, 160, 255};
  v.style.margin = EdgeInsets::all(3);
  v.onClick = [&state, color] {
    state.currentColor = color;
    state.markDirty();
  };
  return v;
}

// A round "brush size" button: bigger dot == thicker brush. `selected`
// gets the same blue-ring treatment as colorSwatch above.
static View sizeButton(float diameter, float width, PaintState &state,
                       bool selected) {
  View outer;
  outer.style.width = Size::pixel(32);
  outer.style.height = Size::pixel(32);
  outer.style.justifyContent = Justify::Center;
  outer.style.alignItems = Align::Center;
  outer.style.borderRadius = 16;
  outer.style.borderWidth = selected ? 2 : 1;
  outer.style.borderColor =
      selected ? Color{40, 120, 220, 255} : Color{200, 200, 200, 255};
  outer.style.backgroundColor = {250, 250, 250, 255};
  outer.style.margin = EdgeInsets::all(3);

  View dot;
  dot.style.width = Size::pixel(diameter);
  dot.style.height = Size::pixel(diameter);
  dot.style.borderRadius = diameter / 2.0f;
  dot.style.backgroundColor = {40, 40, 40, 255};
  outer.addChild(std::move(dot));

  outer.onClick = [&state, width] {
    state.currentWidth = width;
    state.markDirty();
  };
  return outer;
}

// A plain rectangular text button (Clear).
static View textButton(const std::string &label,
                       std::function<void()> onClick) {
  View v;
  v.style.padding = EdgeInsets{8, 14, 8, 14};
  v.style.margin = EdgeInsets{3, 3, 3, 12};
  v.style.backgroundColor = {245, 245, 245, 255};
  v.style.hoverColor = Color{230, 230, 230, 255};
  v.style.borderWidth = 1;
  v.style.borderColor = {190, 190, 190, 255};
  v.style.borderRadius = 6;
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.onClick = std::move(onClick);

  Text t;
  t.label = label;
  t.fontSize = 14;
  t.color = {30, 30, 30, 255};
  v.addChild(std::move(t));
  return v;
}

// Rebuilds the whole view tree. Called once from main(), and again from
// setZoom() below whenever zoom changes — the canvas's on-screen size has
// to actually change, and this library has no widthSource/heightSource to
// mutate that in place, so a full setRoot() relayout is the way to do it.
static View buildRoot();

// Changes zoom and asks LiteUI to relayout against the new canvas size.
// Kept as a free function (rather than inline in the button's onClick) so
// both the "-"/"+" buttons below can share it.
static void setZoom(float z) {
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
  if (g_ui)
    g_ui->setRoot(buildRoot());
}

static View buildRoot() {

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = {235, 235, 235, 255};

  // ---- Toolbar ----
  View toolbar;
  toolbar.style.direction = FlexDirection::Row;
  toolbar.style.alignItems = Align::Center;
  toolbar.style.padding = EdgeInsets::all(8);
  toolbar.style.gap = 2;
  toolbar.style.backgroundColor = {245, 245, 245, 255};
  toolbar.style.borderWidth = 0;
  toolbar.style.height = Size::pixel(52);
  toolbar.style.flexShrink = 0;

  static const Color kPalette[] = {
      {20, 20, 20, 255},   {255, 255, 255, 255}, {220, 50, 50, 255},
      {245, 166, 35, 255}, {245, 220, 60, 255},  {80, 180, 90, 255},
      {50, 110, 220, 255}, {150, 80, 200, 255},  {230, 120, 170, 255},
      {120, 80, 50, 255},
  };
  for (Color c : kPalette)
    toolbar.addChild(colorSwatch(c, state,
                                 c.r == state.currentColor.r &&
                                     c.g == state.currentColor.g &&
                                     c.b == state.currentColor.b));

  View spacer1;
  spacer1.style.width = Size::pixel(16);
  toolbar.addChild(std::move(spacer1));

  toolbar.addChild(sizeButton(6, 2, state, false));
  toolbar.addChild(sizeButton(12, 4, state, true));
  toolbar.addChild(sizeButton(20, 8, state, false));

  View spacer2;
  spacer2.style.flexGrow = 1;
  toolbar.addChild(std::move(spacer2));

  // ---- Zoom controls ----
  toolbar.addChild(textButton("-", [] { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.source = [] {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 14;
  zoomLabel.color = {30, 30, 30, 255};
  zoomLabel.style.margin = EdgeInsets{0, 6, 0, 6};
  toolbar.addChild(std::move(zoomLabel));

  toolbar.addChild(textButton("+", [] { setZoom(state.zoom * kZoomStep); }));

  // Toggles pan mode: while on, drags on the canvas pan the viewport
  // (via the viewport's own built-in ContentPan handling) instead of
  // drawing a stroke. backgroundColorSource is polled each frame so the
  // button's own fill reflects whether panMode is currently on.
  View panBtn = textButton("Pan", [] {
    state.panMode = !state.panMode;
    state.markDirty();
  });
  panBtn.backgroundColorSource = [] {
    return state.panMode ? Color{190, 215, 250, 255}
                         : Color{245, 245, 245, 255};
  };
  toolbar.addChild(std::move(panBtn));

  toolbar.addChild(textButton("Clear", [] {
    state.strokes.clear();
    state.markDirty();
  }));

  root.addChild(std::move(toolbar));

  // ---- Viewport ----
  // This is the "desk" — the scrollable gray area the window shows you,
  // as opposed to the document itself (kDocW x kDocH, added below). It
  // takes the window's remaining height (flexGrow:1, same reasoning as
  // the previous version's canvas did), and centers its one child
  // (the document) when the document is smaller than the viewport,
  // exactly like Paint/Photoshop's page-on-a-desk look.
  //
  // overflowX/Y = Auto means: no scrollbar and no clipping while the
  // document fits, but the instant the window gets smaller than the
  // document, both a scrollbar AND clipping kick in automatically — you
  // don't have to handle that yourself.
  View viewport;
  viewport.style.height = Size::full();
  viewport.style.width = Size::full();
  viewport.style.backgroundColor = {200, 200, 200, 255};
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24); // breathing room once scrolled

  // ---- Document (the actual page you draw on) ----
  // Fixed size, NOT flexGrow/full — this is what makes it a "document"
  // rather than "whatever space is left", and what makes onPressAt/
  // onDragTo's local coordinates always mean the same (x,y) in the
  // picture regardless of scroll position or window size.
  View canvas;
  canvas.isCanvas = true;
  // On-screen size follows zoom; onPaint below still draws in fixed
  // kDocW x kDocH document units and scales internally (see paintCanvas),
  // so this is purely how big the "page" appears in the viewport.
  canvas.style.width = Size::pixel(kDocW * state.zoom);
  canvas.style.height = Size::pixel(kDocH * state.zoom);
  canvas.style.borderWidth = 1;
  canvas.style.borderColor = {150, 150, 150, 255};
  canvas.style.backgroundColor = {255, 255, 255, 255};

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx, state); };
  canvas.onPressAt = [](float x, float y) {
    if (state.panMode)
      return; // let the viewport's own ContentPan drag handle this instead
    state.strokes.push_back(Stroke{{{x / state.zoom, y / state.zoom}},
                                   state.currentColor,
                                   state.currentWidth});
    state.markDirty();
  };
  canvas.onDragTo = [](float x, float y) {
    if (state.panMode)
      return;
    if (!state.strokes.empty()) {
      state.strokes.back().pts.push_back({x / state.zoom, y / state.zoom});
      state.markDirty();
    }
  };
  canvas.canvasDirtySource = [] {
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };

  viewport.addChild(std::move(canvas));
  root.addChild(std::move(viewport));

  return root;
}

int main() {
  LiteUI ui(900, 650, "Paint");
  g_ui = &ui;
  ui.setRoot(buildRoot());
  ui.run();
  return 0;
}