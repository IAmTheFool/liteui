// A tiny MS-Paint-style app: a color palette, three brush sizes, a clear
// button, save/open buttons, and a fixed-size "document" page you draw on,
// sitting inside a scrollable gray viewport (the same document-vs-viewport
// split MS Paint and Photoshop use — see the comment above kDocW/kDocH
// below).
//
// All drawing state (the list of strokes, current color/size) lives in
// `PaintState` below and is captured by reference into the view tree's
// lambdas. The canvas itself never keeps its own picture between mouse
// moves — every redraw just replays `state.strokes` from scratch onto a
// fresh transparent surface (see the canvas's onPaint). That's simple and
// plenty fast for an app like this; a resolution-independent "redraw the
// whole scene" is the same approach real vector-graphics apps use.
//
// Save/Open persist that same stroke list to/from a tiny custom text
// format (see saveDocument/loadDocument below) rather than pixels — this
// keeps things simple, fully cross-platform (no dependency on
// CanvasContext::getImageData, which liteui.hpp notes is unsupported on
// Windows), and means a saved file re-opens at full quality at any zoom.

#include "liteui.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

constexpr float kDocW = 816.0f;
constexpr float kDocH = 1056.0f;

// Zoom range/step for the viewport controls added below.
constexpr float kZoomMin = 0.25f;
constexpr float kZoomMax = 4.0f;
constexpr float kZoomStep = 1.25f;

// Magic header written/checked by saveDocument/loadDocument, so an open
// on a garbage/unrelated file fails cleanly instead of parsing nonsense.
constexpr const char *kFileMagic = "LITEPAINT1";

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
  float zoom = 1.0f; // 1.0 = 100%; scales the doc at paint time
  bool dirty = true;
  void markDirty() { dirty = true; }
};

static PaintState state;

// ---------------- Save / Open ----------------
//
// Plain whitespace-separated text, one stroke per block:
//   LITEPAINT1
//   <strokeCount>
//   <r> <g> <b> <a> <width> <pointCount>
//   <x> <y>
//   <x> <y>
//   ...
// repeated per stroke. Coordinates are stored in fixed document units
// (not scaled by zoom), matching what onPressAt/onDragTo already store in
// state.strokes — so a save/open round-trip is zoom-independent.

static bool saveDocument(const std::string &path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out << kFileMagic << "\n";
  out << state.strokes.size() << "\n";
  for (const Stroke &s : state.strokes) {
    out << static_cast<int>(s.color.r) << ' ' << static_cast<int>(s.color.g)
        << ' ' << static_cast<int>(s.color.b) << ' '
        << static_cast<int>(s.color.a) << ' ' << s.width << ' ' << s.pts.size()
        << "\n";
    for (const Point &p : s.pts)
      out << p.x << ' ' << p.y << "\n";
  }
  return static_cast<bool>(out);
}

static bool loadDocument(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;

  std::string magic;
  if (!(in >> magic) || magic != kFileMagic)
    return false;

  size_t strokeCount = 0;
  if (!(in >> strokeCount))
    return false;

  std::vector<Stroke> loaded;
  loaded.reserve(strokeCount);
  for (size_t i = 0; i < strokeCount; ++i) {
    int r = 0, g = 0, b = 0, a = 255;
    float width = 1.0f;
    size_t pointCount = 0;
    if (!(in >> r >> g >> b >> a >> width >> pointCount))
      return false;

    Stroke s;
    s.color = Color{static_cast<uint8_t>(std::clamp(r, 0, 255)),
                    static_cast<uint8_t>(std::clamp(g, 0, 255)),
                    static_cast<uint8_t>(std::clamp(b, 0, 255)),
                    static_cast<uint8_t>(std::clamp(a, 0, 255))};
    s.width = width;
    s.pts.reserve(pointCount);
    for (size_t j = 0; j < pointCount; ++j) {
      Point p;
      if (!(in >> p.x >> p.y))
        return false;
      s.pts.push_back(p);
    }
    loaded.push_back(std::move(s));
  }

  state.strokes = std::move(loaded);
  state.markDirty();
  return true;
}

static void paintCanvas(CanvasContext &ctx, const PaintState &state) {
  ctx.setFillColor({255, 255, 255, 255});
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

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

static View colorSwatch(Color color, PaintState &state, bool selected) {
  View v;
  v.style.width = Size::pixel(28);
  v.style.height = Size::pixel(28);
  v.style.backgroundColor = color;
  v.style.borderRadius = 6.0f;
  v.style.borderWidth = [&state, color] {
    return state.currentColor == color ? 3.0f : 1.0f;
  };
  v.style.borderColor = [&state, color] {
    return state.currentColor == color ? Color{40, 120, 220, 255}
                                       : Color{160, 160, 160, 255};
  };
  v.style.margin = EdgeInsets::all(3);
  v.onClick = [&state, color] {
    state.currentColor = color;
    state.markDirty();
  };
  return v;
}

static View sizeButton(float diameter, float width, PaintState &state) {
  View outer;
  outer.style.width = Size::pixel(32);
  outer.style.height = Size::pixel(32);
  outer.style.justifyContent = Justify::Center;
  outer.style.alignItems = Align::Center;
  outer.style.borderRadius = 16.0f;
  outer.style.borderWidth = [&state, width] {
    return state.currentWidth == width ? 2.0f : 1.0f;
  };
  outer.style.borderColor = [&state, width] {
    return state.currentWidth == width ? Color{40, 120, 220, 255}
                                       : Color{200, 200, 200, 255};
  };
  outer.style.backgroundColor = Color{250, 250, 250, 255};
  outer.style.margin = EdgeInsets::all(3);

  View dot;
  dot.style.width = Size::pixel(diameter);
  dot.style.height = Size::pixel(diameter);
  dot.style.borderRadius = diameter / 2.0f;
  dot.style.backgroundColor = Color{40, 40, 40, 255};
  outer.addChild(std::move(dot));

  outer.onClick = [&state, width] {
    state.currentWidth = width;
    state.markDirty();
  };
  return outer;
}

static View textButton(const std::string &label,
                       std::function<void()> onClick) {
  View v;
  v.style.padding = EdgeInsets{8, 14, 8, 14};
  v.style.margin = EdgeInsets{3, 3, 3, 3};
  v.style.backgroundColor = Color{245, 245, 245, 255};
  v.style.hoverColor = Color{230, 230, 230, 255};
  v.style.borderWidth = 1.0f;
  v.style.borderColor = Color{190, 190, 190, 255};
  v.style.borderRadius = 6.0f;
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.onClick = std::move(onClick);

  Text t;
  t.label = label;
  t.fontSize = 14;
  t.color = Color{30, 30, 30, 255};
  v.addChild(std::move(t));
  return v;
}

static void setZoom(float z) {
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
}

int main() {
  LiteUI ui(900, 650, "Paint");
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{235, 235, 235, 255};

  // ---- Toolbar ----
  View toolbar;
  toolbar.style.direction = FlexDirection::Row;
  toolbar.style.alignItems = Align::Center;
  toolbar.style.padding = EdgeInsets::all(8);
  toolbar.style.gap = 2;
  toolbar.style.backgroundColor = Color{245, 245, 245, 255};
  toolbar.style.borderWidth = 0.0f;
  toolbar.style.height = Size::pixel(52);
  toolbar.style.flexShrink = 0;

  static const Color kPalette[] = {
      {20, 20, 20, 255},   {255, 255, 255, 255}, {220, 50, 50, 255},
      {245, 166, 35, 255}, {245, 220, 60, 255},  {80, 180, 90, 255},
      {50, 110, 220, 255}, {150, 80, 200, 255},  {230, 120, 170, 255},
      {120, 80, 50, 255},
  };
  for (Color c : kPalette)
    toolbar.addChild(colorSwatch(c, state, c == state.currentColor));

  View spacer1;
  spacer1.style.width = Size::pixel(16);
  toolbar.addChild(std::move(spacer1));

  toolbar.addChild(sizeButton(6, 2, state));
  toolbar.addChild(sizeButton(12, 4, state));
  toolbar.addChild(sizeButton(20, 8, state));

  View spacer2;
  spacer2.style.flexGrow = 1;
  toolbar.addChild(std::move(spacer2));

  // ---- Zoom controls ----
  toolbar.addChild(textButton("-", [] { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.label = [] {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 14;
  zoomLabel.color = Color{30, 30, 30, 255};
  zoomLabel.style.margin = EdgeInsets{0, 6, 0, 6};
  toolbar.addChild(std::move(zoomLabel));

  toolbar.addChild(textButton("+", [] { setZoom(state.zoom * kZoomStep); }));

  View spacer3;
  spacer3.style.width = Size::pixel(12);
  toolbar.addChild(std::move(spacer3));

  toolbar.addChild(textButton("Open", [] {
    auto path =
        openFilePicker("Open Painting", {{"LiteUI Paint", "*.litepaint"}});
    if (!path)
      return; // user cancelled
    loadDocument(*path);
  }));

  toolbar.addChild(textButton("Save", [] {
    auto path = saveFilePicker("Save Painting", "untitled.litepaint",
                               {{"LiteUI Paint", "*.litepaint"}}, "litepaint");
    if (path)
      saveDocument(*path);
  }));

  toolbar.addChild(textButton("Clear", [] {
    state.strokes.clear();
    state.markDirty();
  }));

  root.addChild(std::move(toolbar));

  View viewport;
  viewport.style.height = Size::full();
  viewport.style.width = Size::full();
  viewport.style.backgroundColor = Color{200, 200, 200, 255};
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24);
  viewport.style.contentPanEnabled = false;
  viewport.style.wheelScrollEnabled = false;

  View canvas;
  canvas.isCanvas = true;
  canvas.style.width =
      std::function<Size()>([] { return Size::pixel(kDocW * state.zoom); });
  canvas.style.height =
      std::function<Size()>([] { return Size::pixel(kDocH * state.zoom); });
  canvas.style.borderWidth = 1.0f;
  canvas.style.borderColor = Color{150, 150, 150, 255};
  canvas.style.backgroundColor = Color{255, 255, 255, 255};

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx, state); };
  canvas.onPressAt = [](float x, float y) {
    state.strokes.push_back(Stroke{{{x / state.zoom, y / state.zoom}},
                                   state.currentColor,
                                   state.currentWidth});
    state.markDirty();
  };
  canvas.onDragTo = [](float x, float y) {
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
  canvas.onScrollUp = []() { setZoom(state.zoom * kZoomStep); };
  canvas.onScrollDown = []() { setZoom(state.zoom / kZoomStep); };

  viewport.addChild(std::move(canvas));
  root.addChild(std::move(viewport));
  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}