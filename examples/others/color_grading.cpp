// color_grading.cpp
//
// Standalone liteui example: a Lightroom-style "Color Grading" panel.
//
//   * Adjust row      : 3-Way | Shadows | Midtones | Highlights | Global
//   * 3-Way mode      : big Midtones wheel on top, Shadows + Highlights below,
//                       each with an eye (preview on/off) and a Luminance
//                       slider
//   * Single-range    : one large wheel + Hue / Saturation / Luminance sliders
//   * Blending / Balance sliders shared by every mode
//   * Readout line    : "H:13 S:72 L:+1" for the wheel touched last
//
// Interaction
//   * click / drag a wheel     -> sets hue (angle) and saturation (radius)
//   * click / drag a slider    -> sets the value
//   * right-click wheel/slider -> reset that wheel's hue+sat / that slider
//   * click the eye            -> toggle that wheel's preview
//   * click the title / arrow  -> collapse / expand the panel
//
// Integration into the photo editor: everything the editor needs is in
// cg::State. Build the panel with cg::buildPanel(state) and set
// state.onChange to re-render the image whenever a value changes.
// The State object must outlive ui.run().

#include "liteui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

constexpr float kPi = 3.14159265358979f;

// ---------------- theme (dark, Lightroom-like) ----------------
constexpr Color kPanelBg{68, 68, 68};
constexpr Color kHeaderBg{40, 40, 40};
constexpr Color kTitle{176, 176, 176};
constexpr Color kLabel{160, 160, 160};
constexpr Color kValue{232, 232, 232};
constexpr Color kDivider{46, 46, 46};
constexpr Color kTrackFill{32, 32, 32};
constexpr Color kTrackEdge{104, 104, 104};
constexpr Color kThumbFill{196, 196, 196};
constexpr Color kThumbEdge{28, 28, 28};
constexpr Color kIconRing{205, 205, 205};
constexpr Color kIconActiveBg{92, 92, 92};
constexpr Color kTransparent{0, 0, 0, 0};

// ---------------- geometry ----------------
constexpr float kWheelPad = 24.0f; // room around the wheel for the hue dot
constexpr float kSliderH = 24.0f;  // slider canvas height
constexpr float kSliderPad = 7.0f; // thumb travel inset (half thumb width + 1)
constexpr float kTrackY = 6.0f;
constexpr float kTrackH = 6.0f;
constexpr int kMainWheel = 150;   // midtones wheel in 3-way mode
constexpr int kSmallWheel = 132;  // shadows / highlights in 3-way mode
constexpr int kSingleWheel = 210; // wheel in single-range modes
constexpr float kRowLabelW = 84.0f;
constexpr float kRowValueW = 40.0f;
constexpr float kRowSliderW = 224.0f;

// ---------------- state ----------------
enum Range { Shadows = 0, Midtones = 1, Highlights = 2, Global = 3 };

// Wheel brightness is purely cosmetic (Lightroom dims the shadow wheel).
constexpr float kWheelBrightness[4] = {0.62f, 1.0f, 0.80f, 1.0f};

struct Wheel {
  float hue = 0; // 0..359 degrees
  float sat = 0; // 0..100
  float lum = 0; // -100..100
  bool visible = true;
  bool isNeutral() const { return sat == 0 && lum == 0; }
};

struct State {
  std::array<Wheel, 4> wheel; // indexed by Range
  float blending = 50;        // 0..100
  float balance = 0;          // -100..100
  bool enabled = true;        // master on/off (header checkbox)

  // UI-only state
  int mode = 0;          // 0 = 3-way, 1..4 = Shadows..Global
  int active = Midtones; // wheel shown in the H/S/L readout
  bool collapsed = false;

  // Fired on every value change (live, including while dragging).
  std::function<void()> onChange;
  void changed() {
    if (onChange)
      onChange();
  }
};

// ---------------- small helpers ----------------
inline size_t hf(float v) { return std::hash<float>{}(v); }
inline size_t mix(size_t a, size_t b) {
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

// Builds a canvasDirtySource that reports true once whenever `stamp` changes.
// Every canvas gets its own, so a drag only repaints what actually moved.
inline std::function<bool()> makeDirty(std::function<size_t()> stamp) {
  auto last = std::make_shared<size_t>(stamp());
  return [stamp, last]() -> bool {
    size_t now = stamp();
    if (now == *last)
      return false;
    *last = now;
    return true;
  };
}

inline Color hsv(float h, float s, float v, uint8_t a = 255) {
  h = std::fmod(h, 360.0f);
  if (h < 0)
    h += 360.0f;
  float c = v * s;
  float hp = h / 60.0f;
  float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
  float m = v - c;
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(hp) % 6) {
  case 0:
    r = c;
    g = x;
    break;
  case 1:
    r = x;
    g = c;
    break;
  case 2:
    g = c;
    b = x;
    break;
  case 3:
    g = x;
    b = c;
    break;
  case 4:
    r = x;
    b = c;
    break;
  default:
    r = c;
    b = x;
    break;
  }
  auto to8 = [m](float f) {
    return static_cast<uint8_t>(
        std::lround(std::clamp((f + m) * 255.0f, 0.0f, 255.0f)));
  };
  return Color{to8(r), to8(g), to8(b), a};
}

inline std::string fmtSigned(float v) {
  char buf[16];
  int i = static_cast<int>(std::lround(v));
  std::snprintf(buf, sizeof buf, i > 0 ? "+%d" : "%d", i);
  return buf;
}

inline Text makeText(const std::string &s, float size, Color color,
                     float width = -1.0f, TextAlign align = TextAlign::Start) {
  Text t;
  t.label = s;
  t.fontSize = size;
  t.color = color;
  t.wrap = TextWrap::NoWrap;
  t.align = align;
  if (width > 0)
    t.style.width = Size::pixel(width);
  return t;
}

// Pre-renders the hue/saturation disc once per (diameter, brightness):
// angle = hue (0 at 3 o'clock, counter-clockwise), radius = saturation,
// white/grey in the centre. Cached in a map so the CanvasImage address is
// stable (liteui caches the converted surface per image).
inline const CanvasImage &wheelImage(int d, float brightness) {
  static std::map<std::pair<int, int>, CanvasImage> cache;
  auto key = std::make_pair(d, static_cast<int>(std::lround(brightness * 100)));
  auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  CanvasImage img(d, d);
  const float R = d / 2.0f;
  for (int y = 0; y < d; ++y) {
    for (int x = 0; x < d; ++x) {
      float dx = x + 0.5f - R, dy = y + 0.5f - R;
      float dist = std::sqrt(dx * dx + dy * dy);
      float alpha = std::clamp(R - dist + 0.5f, 0.0f, 1.0f); // AA rim
      if (alpha <= 0.0f)
        continue;
      float hue = std::atan2(-dy, dx) * 180.0f / kPi;
      Color c = hsv(hue, std::min(1.0f, dist / R), brightness);
      size_t i = (static_cast<size_t>(y) * d + x) * 4;
      img.pixels[i + 0] = c.r;
      img.pixels[i + 1] = c.g;
      img.pixels[i + 2] = c.b;
      img.pixels[i + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
    }
  }
  return cache.emplace(key, std::move(img)).first->second;
}

// ================= slider =================
enum class Track { Plain, Hue, Saturation };

struct SliderSpec {
  float width = 150;
  float lo = 0, hi = 100, def = 0;
  std::function<float()> get;
  std::function<void(float)> set;
  Track track = Track::Plain;
  std::function<float()> hueForSat; // Saturation track: colour at the far end
  bool ticks = false;               // small tick marks (Blending / Balance)
};

inline Canvas buildSlider(const SliderSpec &sp) {
  Canvas c;
  c.style.width = Size::pixel(sp.width);
  c.style.height = Size::pixel(kSliderH);

  auto apply = [sp](float lx, float) {
    float t =
        std::clamp((lx - kSliderPad) / (sp.width - 2 * kSliderPad), 0.0f, 1.0f);
    sp.set(std::round(sp.lo + t * (sp.hi - sp.lo)));
  };
  c.onPressAt = apply;
  c.onDragTo = apply;
  c.onRightClick = [sp]() { sp.set(sp.def); };

  c.onPaint = [sp](CanvasContext &ctx) {
    const float w = ctx.width();
    const float x0 = kSliderPad, x1 = w - kSliderPad;

    // track
    if (sp.track == Track::Hue) {
      CanvasGradient g = CanvasGradient::linear(x0, 0, x1, 0);
      for (int i = 0; i <= 6; ++i)
        g.addColorStop(i / 6.0f, hsv(i * 60.0f, 1, 1));
      ctx.setFillGradient(g);
    } else if (sp.track == Track::Saturation) {
      CanvasGradient g = CanvasGradient::linear(x0, 0, x1, 0);
      g.addColorStop(0.0f, Color{128, 128, 128});
      g.addColorStop(1.0f, hsv(sp.hueForSat ? sp.hueForSat() : 0, 1, 1));
      ctx.setFillGradient(g);
    } else {
      ctx.setFillColor(kTrackFill);
    }
    ctx.beginPath();
    ctx.roundRect(3, kTrackY, w - 6, kTrackH, kTrackH / 2);
    ctx.fill();
    ctx.setStrokeColor(kTrackEdge);
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    ctx.roundRect(3, kTrackY, w - 6, kTrackH, kTrackH / 2);
    ctx.stroke();

    // ticks
    if (sp.ticks) {
      ctx.setStrokeColor(Color{112, 112, 112});
      ctx.setLineWidth(1.0f);
      for (int i = 0; i <= 10; ++i) {
        float x = std::round(x0 + (x1 - x0) * i / 10.0f) + 0.5f;
        ctx.beginPath();
        ctx.moveTo(x, kTrackY + kTrackH + 2);
        ctx.lineTo(x, kTrackY + kTrackH + 5);
        ctx.stroke();
      }
    }

    // thumb: small "house" pointing up at the track
    float t = std::clamp((sp.get() - sp.lo) / (sp.hi - sp.lo), 0.0f, 1.0f);
    float x = x0 + t * (x1 - x0);
    float tip = kTrackY + kTrackH - 1.0f;
    ctx.beginPath();
    ctx.moveTo(x, tip);
    ctx.lineTo(x + 6, tip + 6);
    ctx.lineTo(x + 6, tip + 11);
    ctx.lineTo(x - 6, tip + 11);
    ctx.lineTo(x - 6, tip + 6);
    ctx.closePath();
    ctx.setFillColor(kThumbFill);
    ctx.fill();
    ctx.setStrokeColor(kThumbEdge);
    ctx.setLineWidth(1.0f);
    ctx.stroke();
  };

  c.canvasDirtySource = makeDirty([sp]() -> size_t {
    size_t h = hf(sp.get());
    if (sp.hueForSat)
      h = mix(h, hf(sp.hueForSat()));
    return h;
  });
  return c;
}

// Slider bound to one field of a Wheel.
inline SliderSpec wheelSlider(State &s, int range, float Wheel::*field,
                              float width, float lo, float hi, Track track) {
  SliderSpec sp;
  sp.width = width;
  sp.lo = lo;
  sp.hi = hi;
  sp.def = 0;
  sp.track = track;
  sp.get = [&s, range, field]() { return s.wheel[range].*field; };
  sp.set = [&s, range, field](float v) {
    s.wheel[range].*field = v;
    s.active = range;
    s.changed();
  };
  if (track == Track::Saturation)
    sp.hueForSat = [&s, range]() { return s.wheel[range].hue; };
  return sp;
}

// "Label ---slider--- value" row.
inline View buildSliderRow(const std::string &label, const SliderSpec &sp,
                           bool signedValue) {
  View row;
  row.style.direction = FlexDirection::Row;
  row.style.alignItems = Align::Center;
  row.style.gap = 8;

  row.addChild(makeText(label, 15, kLabel, kRowLabelW));
  row.addChild(buildSlider(sp));

  Text value = makeText("", 15, kValue, kRowValueW, TextAlign::End);
  auto get = sp.get;
  value.label = [get, signedValue]() -> std::string {
    return signedValue ? fmtSigned(get())
                       : std::to_string(static_cast<int>(std::lround(get())));
  };
  row.addChild(value);
  return row;
}

// ================= color wheel =================
inline Canvas buildWheel(State &s, int range, int diameter) {
  const float size = diameter + 2 * kWheelPad;

  Canvas c;
  c.style.width = Size::pixel(size);
  c.style.height = Size::pixel(size);

  auto apply = [&s, range, diameter, size](float lx, float ly) {
    Wheel &w = s.wheel[range];
    const float R = diameter / 2.0f, cx = size / 2, cy = size / 2;
    float dx = lx - cx, dy = ly - cy;
    float dist = std::sqrt(dx * dx + dy * dy);
    w.sat = std::round(std::clamp(dist / R, 0.0f, 1.0f) * 100.0f);
    if (dist > 2.0f) { // keep the old hue while sitting on the centre
      float hue = std::atan2(-dy, dx) * 180.0f / kPi;
      if (hue < 0)
        hue += 360.0f;
      w.hue = std::fmod(std::round(hue), 360.0f);
    }
    s.active = range;
    s.changed();
  };
  c.onPressAt = apply;
  c.onDragTo = apply;
  c.onRightClick = [&s, range]() { // reset hue + sat, keep luminance
    s.wheel[range].hue = 0;
    s.wheel[range].sat = 0;
    s.active = range;
    s.changed();
  };

  c.onPaint = [&s, range, diameter](CanvasContext &ctx) {
    const Wheel &w = s.wheel[range];
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    const float R = diameter / 2.0f;

    ctx.setGlobalAlpha((w.visible && s.enabled) ? 1.0f : 0.3f);
    ctx.drawImage(wheelImage(diameter, kWheelBrightness[range]), cx - R,
                  cy - R);
    ctx.setGlobalAlpha(1.0f);

    const float ang = -w.hue * kPi / 180.0f;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float dist = w.sat / 100.0f * R;
    const float px = cx + ca * dist, py = cy + sa * dist;

    // spoke from centre to puck
    ctx.setStrokeColor(hsv(w.hue, 0.30f, 1.0f, 200));
    ctx.setLineWidth(2.0f);
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(px, py);
    ctx.stroke();

    // hue dot outside the rim (grey while saturation is 0)
    float k = std::clamp(w.sat / 100.0f * 3.0f, 0.0f, 1.0f);
    Color full = hsv(w.hue, 1, 1);
    Color grey{70, 70, 70};
    auto lerp8 = [k](uint8_t a, uint8_t b) {
      return static_cast<uint8_t>(std::lround(a + (b - a) * k));
    };
    ctx.setFillColor(Color{lerp8(grey.r, full.r), lerp8(grey.g, full.g),
                           lerp8(grey.b, full.b)});
    ctx.beginPath();
    ctx.arc(cx + ca * (R + 14), cy + sa * (R + 14), 6.5f, 0, 2 * kPi);
    ctx.fill();

    // neutral point
    ctx.setFillColor(Color{50, 50, 50});
    ctx.beginPath();
    ctx.arc(cx, cy, 2.0f, 0, 2 * kPi);
    ctx.fill();

    // puck: dark ring with a faint light halo so it reads on any colour
    ctx.setStrokeColor(Color{255, 255, 255, 90});
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    ctx.arc(px, py, 10.5f, 0, 2 * kPi);
    ctx.stroke();
    ctx.setStrokeColor(Color{50, 50, 50});
    ctx.setLineWidth(2.2f);
    ctx.beginPath();
    ctx.arc(px, py, 9.0f, 0, 2 * kPi);
    ctx.stroke();
  };

  c.canvasDirtySource = makeDirty([&s, range]() -> size_t {
    const Wheel &w = s.wheel[range];
    size_t h = hf(w.hue);
    h = mix(h, hf(w.sat));
    h = mix(h, (w.visible ? 1u : 0u) | (s.enabled ? 2u : 0u));
    return h;
  });
  return c;
}

// Eye icon: toggles that wheel's preview.
inline Canvas buildEye(State &s, int range) {
  Canvas c;
  c.style.width = Size::pixel(22);
  c.style.height = Size::pixel(14);
  c.onClick = [&s, range]() {
    s.wheel[range].visible = !s.wheel[range].visible;
    s.changed();
  };
  c.onPaint = [&s, range](CanvasContext &ctx) {
    const bool on = s.wheel[range].visible;
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    Color col = on ? Color{190, 190, 190} : Color{118, 118, 118};
    ctx.setStrokeColor(col);
    ctx.setFillColor(col);
    ctx.setLineWidth(1.4f);
    ctx.beginPath();
    ctx.moveTo(cx - 8, cy);
    ctx.quadraticCurveTo(cx, cy - 9, cx + 8, cy);
    ctx.quadraticCurveTo(cx, cy + 9, cx - 8, cy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, 2.2f, 0, 2 * kPi);
    ctx.fill();
    if (!on) {
      ctx.beginPath();
      ctx.moveTo(cx - 8, cy + 6);
      ctx.lineTo(cx + 8, cy - 6);
      ctx.stroke();
    }
  };
  c.canvasDirtySource =
      makeDirty([&s, range]() -> size_t { return s.wheel[range].visible; });
  return c;
}

// wheel + eye row + luminance slider, all as one column
inline View buildWheelBlock(State &s, int range, int diameter) {
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 2;

  col.addChild(buildWheel(s, range, diameter));

  View eyeRow;
  eyeRow.style.direction = FlexDirection::Row;
  eyeRow.style.justifyContent = Justify::End;
  eyeRow.style.width = Size::pixel(static_cast<float>(diameter));
  eyeRow.addChild(buildEye(s, range));
  col.addChild(eyeRow);

  col.addChild(buildSlider(wheelSlider(s, range, &Wheel::lum,
                                       static_cast<float>(diameter), -100, 100,
                                       Track::Plain)));
  return col;
}

// ================= mode-specific bodies =================
inline View buildThreeWay(State &s) {
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 6;

  col.addChild(buildWheelBlock(s, Midtones, kMainWheel));

  View row;
  row.style.direction = FlexDirection::Row;
  row.style.justifyContent = Justify::Center;
  row.style.gap = 6;

  auto labeled = [&s](const std::string &name, int range) {
    View v;
    v.style.direction = FlexDirection::Column;
    v.style.alignItems = Align::Center;
    v.addChild(makeText(name, 15, kLabel));
    v.addChild(buildWheelBlock(s, range, kSmallWheel));
    return v;
  };
  row.addChild(labeled("Shadows", Shadows));
  row.addChild(labeled("Highlights", Highlights));
  col.addChild(row);
  return col;
}

inline View buildSingle(State &s, int range) {
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 6;

  col.addChild(buildWheel(s, range, kSingleWheel));

  auto row = [&](const std::string &label, float Wheel::*f, float lo, float hi,
                 Track t, bool sgn) {
    col.addChild(buildSliderRow(
        label, wheelSlider(s, range, f, kRowSliderW, lo, hi, t), sgn));
  };
  row("Hue", &Wheel::hue, 0, 359, Track::Hue, false);
  row("Saturation", &Wheel::sat, 0, 100, Track::Saturation, false);
  row("Luminance", &Wheel::lum, -100, 100, Track::Plain, true);
  return col;
}

// ================= "Adjust :" icons =================
inline Canvas buildModeIcon(State &s, int mode) {
  Canvas c;
  c.style.width = Size::pixel(28);
  c.style.height = Size::pixel(30);
  c.onClick = [&s, mode]() {
    s.mode = mode;
    if (mode > 0)
      s.active = mode - 1;
  };

  c.onPaint = [&s, mode](CanvasContext &ctx) {
    const float cx = 14, cy = 12;
    if (s.mode == mode) {
      ctx.setFillColor(kIconActiveBg);
      ctx.beginPath();
      ctx.roundRect(0, 0, 28, 24, 5);
      ctx.fill();
    }
    ctx.setStrokeColor(kIconRing);
    ctx.setLineWidth(1.6f);

    if (mode == 0) { // three overlapping circles
      const float pts[3][2] = {
          {cx - 4.2f, cy + 2.4f}, {cx + 4.2f, cy + 2.4f}, {cx, cy - 4.6f}};
      for (auto &p : pts) {
        ctx.beginPath();
        ctx.arc(p[0], p[1], 3.6f, 0, 2 * kPi);
        ctx.stroke();
      }
    } else if (mode == 4) { // half dark / half light
      ctx.setFillColor(Color{28, 28, 28});
      ctx.beginPath();
      ctx.arc(cx, cy, 6.5f, kPi / 2, 3 * kPi / 2);
      ctx.closePath();
      ctx.fill();
      ctx.setFillColor(Color{240, 240, 240});
      ctx.beginPath();
      ctx.arc(cx, cy, 6.5f, -kPi / 2, kPi / 2);
      ctx.closePath();
      ctx.fill();
      ctx.beginPath();
      ctx.arc(cx, cy, 6.5f, 0, 2 * kPi);
      ctx.stroke();
    } else { // shadows / midtones / highlights
      static const Color fills[3] = {
          {28, 28, 28}, {128, 128, 128}, {240, 240, 240}};
      ctx.setFillColor(fills[mode - 1]);
      ctx.beginPath();
      ctx.arc(cx, cy, 6.5f, 0, 2 * kPi);
      ctx.fill();
      ctx.beginPath();
      ctx.arc(cx, cy, 6.5f, 0, 2 * kPi);
      ctx.stroke();
    }

    // "this range has been adjusted" dot
    if (mode > 0 && !s.wheel[mode - 1].isNeutral()) {
      ctx.setFillColor(Color{170, 170, 170});
      ctx.beginPath();
      ctx.arc(cx, 27, 1.8f, 0, 2 * kPi);
      ctx.fill();
    }
  };

  c.canvasDirtySource = makeDirty([&s, mode]() -> size_t {
    size_t h = (s.mode == mode) ? 1u : 0u;
    if (mode > 0)
      h = mix(h, s.wheel[mode - 1].isNeutral() ? 1u : 0u);
    return h;
  });
  return c;
}

// ================= whole panel =================
inline View buildPanel(State &s) {
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.overflowY = Overflow::Auto;
  root.style.backgroundColor = kPanelBg;

  // ---- header ----
  View header;
  header.style.direction = FlexDirection::Row;
  header.style.alignItems = Align::Center;
  header.style.height = Size::pixel(38);
  header.style.padding = EdgeInsets{0, 12, 0, 10};
  header.style.backgroundColor = kHeaderBg;

  View enableBox; // top-left master on/off
  enableBox.style.width = Size::pixel(14);
  enableBox.style.height = Size::pixel(14);
  enableBox.style.borderWidth = 1.5f;
  enableBox.style.borderRadius = 2.0f;
  enableBox.style.borderColor = Color{150, 150, 150};
  enableBox.style.backgroundColor = [&s]() -> Color {
    return s.enabled ? Color{150, 150, 150} : kTransparent;
  };
  enableBox.onClick = [&s]() {
    s.enabled = !s.enabled;
    s.changed();
  };
  header.addChild(enableBox);

  auto toggleCollapse = [&s]() { s.collapsed = !s.collapsed; };

  // Title takes the free space and right-aligns its text; clicking the title
  // or the arrow collapses / expands the panel.
  View titleBox;
  titleBox.style.flexGrow = 1;
  titleBox.style.justifyContent = Justify::End;
  titleBox.onClick = toggleCollapse;
  titleBox.addChild(makeText("Color Grading", 20, kTitle));
  header.addChild(titleBox);

  View arrowBox;
  arrowBox.style.width = Size::pixel(26);
  arrowBox.style.alignItems = Align::Center;
  arrowBox.style.justifyContent = Justify::Center;
  arrowBox.onClick = toggleCollapse;
  Text arrow = makeText("", 11, kLabel, 14.0f, TextAlign::Center);
  arrow.label = [&s]() -> std::string {
    return s.collapsed ? "\xE2\x96\xB6" : "\xE2\x96\xBC"; // right / down
  };
  arrowBox.addChild(arrow);
  header.addChild(arrowBox);
  root.addChild(header);

  // ---- body ----
  View body;
  body.style.direction = FlexDirection::Column;
  body.style.alignItems = Align::Center;
  body.style.gap = 10;
  body.style.padding = EdgeInsets{14, 12, 16, 12};
  body.style.display = [&s]() -> Display {
    return s.collapsed ? Display::None : Display::Flex;
  };

  // Adjust : [icons]
  View adjust;
  adjust.style.direction = FlexDirection::Row;
  adjust.style.alignItems = Align::Center;
  adjust.style.gap = 4;
  adjust.addChild(makeText("Adjust :", 15, kLabel));
  for (int m = 0; m < 5; ++m)
    adjust.addChild(buildModeIcon(s, m));
  body.addChild(adjust);

  // H:13 S:72 L:+1
  Text readout = makeText("", 15, kValue, 220, TextAlign::Center);
  readout.label = [&s]() -> std::string {
    const Wheel &w = s.wheel[s.mode > 0 ? s.mode - 1 : s.active];
    char buf[48];
    std::snprintf(
        buf, sizeof buf, "H:%d S:%d L:%s", static_cast<int>(std::lround(w.hue)),
        static_cast<int>(std::lround(w.sat)), fmtSigned(w.lum).c_str());
    return buf;
  };
  body.addChild(readout);

  // mode-specific content: rebuilt whenever s.mode changes
  View host;
  host.style.direction = FlexDirection::Column;
  host.style.alignItems = Align::Center;
  host.keysSource = [&s]() -> std::vector<std::string> {
    return {std::to_string(s.mode)};
  };
  host.itemBuilder = [&s](const std::string &key) -> View {
    int mode = std::stoi(key);
    return mode == 0 ? buildThreeWay(s) : buildSingle(s, mode - 1);
  };
  body.addChild(host);

  // divider
  View divider;
  divider.style.width = Size::full();
  divider.style.height = Size::pixel(1);
  divider.style.backgroundColor = kDivider;
  body.addChild(divider);

  // Blending / Balance
  SliderSpec blending;
  blending.width = kRowSliderW;
  blending.lo = 0;
  blending.hi = 100;
  blending.def = 50;
  blending.ticks = true;
  blending.get = [&s]() { return s.blending; };
  blending.set = [&s](float v) {
    s.blending = v;
    s.changed();
  };
  body.addChild(buildSliderRow("Blending", blending, false));

  SliderSpec balance = blending;
  balance.lo = -100;
  balance.hi = 100;
  balance.def = 0;
  balance.get = [&s]() { return s.balance; };
  balance.set = [&s](float v) {
    s.balance = v;
    s.changed();
  };
  body.addChild(buildSliderRow("Balance", balance, true));

  root.addChild(body);
  return root;
}

int main() {
  // Must outlive ui.run()'s blocking loop.
  static State state;

  // Demo hook: the photo editor would re-render the image here instead.
  state.onChange = []() {
    const auto &w = state.wheel;
    std::printf("\rSH %3.0f/%3.0f/%+4.0f  MID %3.0f/%3.0f/%+4.0f  "
                "HI %3.0f/%3.0f/%+4.0f  GL %3.0f/%3.0f/%+4.0f  "
                "blend %3.0f  bal %+4.0f   ",
                w[0].hue, w[0].sat, w[0].lum, w[1].hue, w[1].sat, w[1].lum,
                w[2].hue, w[2].sat, w[2].lum, w[3].hue, w[3].sat, w[3].lum,
                state.blending, state.balance);
    std::fflush(stdout);
  };

  // Start from the values in the reference screenshot.
  state.wheel[Midtones] = {13, 72, 1, true};
  state.wheel[Shadows] = {0, 45, 0, true};
  state.balance = 2;

  LiteUI ui("Color Grading", 400, 750);
  ui.setRoot(buildPanel(state));
  ui.run();
  return 0;
}