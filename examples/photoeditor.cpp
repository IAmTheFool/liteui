// Simple Photo Editor.

#include "liteui.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <variant>
#include <vector>

// ---------------- Layout constants ----------------
constexpr float kTitlebarH = 36.0f;
constexpr float kStatusBarH = 26.0f;
constexpr float kActivityBarW = 48.0f;
constexpr float kDividerW = 4.0f;
constexpr float kMinSidebarW = 180.0f;
constexpr float kMaxSidebarW = 420.0f;

// Zoom range/step.
constexpr float kZoomMin = 0.1f;
constexpr float kZoomMax = 8.0f;
constexpr float kZoomStep = 1.25f;

// Crop tool. Sizes marked "screen px" are constant on screen regardless of
// zoom (the overlay divides by state.zoom when drawing/hit-testing).
constexpr float kCropMinPx = 16.0f;   // smallest selection edge, image px
constexpr float kCropHandlePx = 8.0f; // drawn handle size, screen px
constexpr float kCropHitPx = 10.0f;   // handle grab tolerance, screen px

// Straighten (arbitrary-angle rotation) range in degrees; positive turns the
// picture clockwise. Combined with the 90-degree buttons this reaches any
// angle.
constexpr float kAngleMax = 45.0f;

// Brightness range (flat offset added to each R/G/B channel).
constexpr int kBrightnessMin = -100;
constexpr int kBrightnessMax = 100;

// Exposure range (percentage; scales each R/G/B channel multiplicatively,
// applied before brightness's flat offset — see kAdjustmentSpecs below).
// Interactive-preview sizing (see EditorState::previewBase): the preview's
// long side is at most kPreviewMaxEdge px, and images whose long side is
// within kPreviewMaxEdge * kPreviewMinRatio skip the preview entirely.
constexpr int kPreviewMaxEdge = 1024;
constexpr float kPreviewMinRatio = 1.5f;

constexpr int kExposureMin = -100;
constexpr int kExposureMax = 100;

// Contrast / Highlights / Shadows / Whites / Blacks all share one -100..+100
// range (0 = untouched).
constexpr int kToneMin = -100;
constexpr int kToneMax = 100;

// Color Mixer (HSL): 8 hue bands x (Hue, Saturation, Luminance), each slider
// -100..+100 (0 = untouched). See the "colormix" block below.
constexpr int kMixMin = -100;
constexpr int kMixMax = 100;
constexpr float kMixHueRangeDeg = 30.0f; // hue slider at +/-100 = +/-30 degrees
constexpr float kMixLumAmount = 0.5f;    // luminance +100 = halfway to white
constexpr float kMixChromaFull = 0.10f;  // chroma (0..1) at which a pixel is
                                         // "colored enough" for full effect
constexpr int kMixLutSize = 720;         // hue lookup resolution (0.5 degree)
constexpr float kMixTabW = 46.0f; // Hue/Sat/Lum tab width (3 + gaps = 150)

// Histogram: at most ~kHistMaxSamples pixels are visited per recompute
// (strided grid), so it stays cheap on full-resolution images.
constexpr int kHistMaxSamples = 300000;
constexpr float kHistHeightPx = 80.0f;

// Tone curve editor (canvas is square, kCurveSizePx on a side; the plot area
// is inset by kCurvePadPx so the end points aren't clipped).
constexpr float kCurveSizePx = 180.0f;
constexpr float kCurvePadPx = 8.0f;
constexpr float kCurveHitPx = 10.0f;  // point grab tolerance, screen px
constexpr float kCurvePointPx = 8.0f; // drawn point size, screen px
constexpr float kCurveMinGap = 2.0f;  // min x distance between points (0..255)
constexpr size_t kCurveMaxPoints = 16;
constexpr float kCurveTabW =
    42.0f; // channel tab width (4 tabs + gaps = canvas width)

// Document size before any image has been opened, so there's a visible
// page to scroll/zoom around from the start.
constexpr float kPlaceholderW = 800.0f;
constexpr float kPlaceholderH = 600.0f;

// Undo history: at most this many steps can be undone; older ones are
// dropped as new edits come in. (A drag on one slider is a single step.)
constexpr size_t kMaxUndoSteps = 200;

// Masks (local adjustments; see the "masks" block below). Radii are a
// fraction of the picture's SHORTER side (the original's when stored, the
// current picture's once mapped).
constexpr size_t kMaxMasks = 8;         // rows in the Masks pane's list
constexpr float kMaskMinRadius = 0.02f; // smallest radial-mask radius
constexpr float kMaskMaxRadius = 1.50f; // largest
constexpr float kMaskHandlePx = 9.0f;   // overlay handle size, screen px
constexpr float kMaskHitPx = 12.0f;     // overlay handle grab tolerance, px

// ---------------- Palette (dark, Lightroom-like) ----------------
// Base swatches.
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

// Semantic names used throughout the UI, mapped onto the swatches above.
constexpr Color kColorTransparent = kTransparent;
constexpr Color kColorWhite{255, 255, 255, 255}; // real white (image, glyphs)

// Chrome backgrounds.
constexpr Color kColorRootBg = kHeaderBg;
constexpr Color kColorPanelBg = kPanelBg;         // sidebar
constexpr Color kColorTitlebarBg = kHeaderBg;     // titlebar
constexpr Color kColorActivityBarBg = kHeaderBg;  // activity bar, idle buttons
constexpr Color kColorMenuHover{62, 62, 62, 255}; // menu / win-btn hover
constexpr Color kColorActivityHover{84, 84, 84, 255}; // button / tab hover
constexpr Color kColorActivityActive = kIconActiveBg; // selected activity btn
constexpr Color kColorDivider = kDivider;             // divider bg
constexpr Color kColorTrack = kTrackFill;             // slider + scroll track
constexpr Color kColorScrollThumb = kTrackEdge;

// Text.
constexpr Color kColorTextDark = kValue;    // titles, menu labels
constexpr Color kColorTextMid = kTitle;     // info pane, win-btn glyphs
constexpr Color kColorTextIcon = kIconRing; // activity bar icons
constexpr Color kColorTextMuted = kLabel;   // section titles
constexpr Color kColorTextFaint{120, 120, 120, 255}; // placeholders

// Dropdown menu.
constexpr Color kColorDropdownBg = kHeaderBg;
constexpr Color kColorDropdownBorder = kThumbEdge;
constexpr Color kColorDropdownRowHover = kIconActiveBg; // also "selected"

// Window controls / accents.
constexpr Color kColorCloseHover{232, 17, 35, 255};
constexpr Color kColorDividerHover{100, 140, 220, 255}; // accent (drag, fills)
constexpr Color kColorAccent{0, 122, 204, 255};         // primary button
constexpr Color kColorAccentHover{30, 140, 220, 255};

// Canvas / viewport.
constexpr Color kColorCanvasPlaceholderBg{52, 52, 52, 255};
constexpr Color kColorViewportBg{30, 30, 30, 255};
constexpr Color kColorCanvasBorder = kTrackEdge;
constexpr Color kColorEmptyCorner{50, 50, 50, 255}; // behind transparent
                                                    // corners of a
                                                    // straightened image

// Status bar.
constexpr Color kColorStatusBarBg = kHeaderBg;
constexpr Color kColorStatusHover = kPanelBg;
constexpr Color kColorStatusError{255, 214, 102, 255};

// A sidebar tab in the activity bar.
struct Activity {
  int id;
  std::string title;
  std::string icon; // single glyph
};

// ---------------- Adjustments ----------------
// Every slider in the "Adjust" pane is one entry in kAdjustmentSpecs below.
// Adding a new one (Contrast, Saturation, ...) means adding one entry to
// that table — AdjustmentSet, the sidebar loop, and the pixel pipeline all
// pick it up automatically; nothing else needs to change.

// Order must match kAdjustmentSpecs below (it is indexed by AdjId).
enum class AdjId : int {
  Exposure,
  Brightness,
  Contrast,
  Highlights,
  Shadows,
  Whites,
  Blacks,
  Temperature,
  Tint,
  Vibrance,
  Saturation,
  Count
};

// One slider's identity, range, and per-channel transform. `apply` takes a
// single channel's 0..255 value (not the whole image) and returns the
// transformed value, unclamped and unrounded — buildLut below rounds and
// clamps after each spec while composing them into one lookup table. This means
// every current adjustment must be pointwise per-channel: it can't be written
// this way if it needs neighbor-pixel access (a future Clarity/sharpen).
//
// Two stages:
//   Stage::Tone  — pointwise per-channel; `apply` is composed into the shared
//                  LUT (Exposure ... Blacks).
//   Stage::Color — needs per-channel gains or cross-channel math
//                  (Temperature, Tint, Vibrance, Saturation). `apply` is
//                  unused (null); these are consumed by the float per-pixel
//                  pass built in ColorPass / applyColorPass below, which runs
//                  after the LUT and rounds to 8 bits only once at the end.
// The slider UI, undo/coalescing, and preview path treat both stages alike.
struct AdjustmentSpec {
  enum class Stage { Tone, Color };
  AdjId id;
  std::string label;
  float min, max;
  std::function<float(float channelValue, float value)> apply;
  Stage stage = Stage::Tone; // trailing default: Tone entries omit it
};

// Declaration order here is both the sidebar's display order and the
// pipeline's processing order — each spec's `apply` runs on the previous
// one's output. Exposure before Brightness mirrors Lightroom's own Basic
// panel ordering, and keeps exposure's multiplicative scale from being
// thrown off by an offset that was meant to be applied after it.
// Hermite smoothstep on [e0, e1]: 0 below e0, 1 above e1, smooth between.
// Used to build the tonal-range weights for Highlights/Shadows/Whites/Blacks.
static inline float smoothStep01(float e0, float e1, float x) {
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

static const std::array<AdjustmentSpec, static_cast<size_t>(AdjId::Count)>
    kAdjustmentSpecs = {{
        {AdjId::Exposure, "Exposure", static_cast<float>(kExposureMin),
         static_cast<float>(kExposureMax),
         [](float x, float value) { return x * (1.0f + value / 100.0f); }},
        {AdjId::Brightness, "Brightness", static_cast<float>(kBrightnessMin),
         static_cast<float>(kBrightnessMax),
         [](float x, float value) { return x + std::round(value); }},
        // Contrast: linear stretch/squash around mid-gray. -100 flattens to
        // gray, +100 doubles the distance from 128 (clipping the ends).
        {AdjId::Contrast, "Contrast", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value) {
           return 128.0f + (x - 128.0f) * (1.0f + value / 100.0f);
         }},
        // Highlights / Shadows / Whites / Blacks: add up to +/- (scale * 100)
        // levels, weighted by a smooth tonal-range mask so the rest of the
        // tonal range is left alone. Positive always brightens the range.
        // Amount and band width are chosen so the curve stays monotonic
        // (max |slope of weight| * amount / 255 < 1) at both extremes.
        // NOTE: applied per channel via the shared LUT, so it is a tone-curve
        // approximation, not a luminance-preserving recovery.
        {AdjId::Highlights, "Highlights", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value) {
           return x + value * 0.6f * smoothStep01(0.4f, 1.0f, x / 255.0f);
         }},
        {AdjId::Shadows, "Shadows", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value) {
           return x +
                  value * 0.6f * (1.0f - smoothStep01(0.0f, 0.6f, x / 255.0f));
         }},
        {AdjId::Whites, "Whites", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value) {
           return x + value * 0.4f * smoothStep01(0.75f, 1.0f, x / 255.0f);
         }},
        {AdjId::Blacks, "Blacks", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value) {
           return x +
                  value * 0.4f * (1.0f - smoothStep01(0.0f, 0.25f, x / 255.0f));
         }},
        // Color stage (see ColorPass below). Positive Temperature = warmer,
        // positive Tint = more magenta (negative = greener).
        {AdjId::Temperature, "Temperature", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Tint, "Tint", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Vibrance, "Vibrance", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Saturation, "Saturation", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Color},
    }};

// BEGIN colormix
// ---------------- Color Mixer (HSL) ----------------
// Lightroom-style per-color adjustment: eight hue bands, each with a Hue,
// Saturation and Luminance slider. A pixel's own hue decides how much of each
// band's setting it gets: between two neighboring band centers the two
// settings are blended with a smoothstep, so a pixel exactly at a center gets
// that band's value alone and there are no seams between bands.
//
// Because the result depends only on a pixel's hue, all of that blending is
// baked into three small tables indexed by hue (MixLut), built once per
// render request. The per-pixel work is then: RGB -> HSL, three table reads,
// HSL -> RGB (see applyColorPass).
enum class MixBand : int {
  Red,
  Orange,
  Yellow,
  Green,
  Aqua,
  Blue,
  Purple,
  Magenta,
  Count
};
enum class MixChan : int { Hue, Saturation, Luminance, Count };
constexpr size_t kMixBandCount = static_cast<size_t>(MixBand::Count);
constexpr size_t kMixChanCount = static_cast<size_t>(MixChan::Count);

// Flat storage: channel-major, so index = chan * kMixBandCount + band.
using MixValues = std::array<float, kMixBandCount * kMixChanCount>;
constexpr size_t mixIndex(MixChan c, size_t band) {
  return static_cast<size_t>(c) * kMixBandCount + band;
}

// Hue (degrees) at the middle of each band; the last entry closes the circle.
constexpr std::array<float, kMixBandCount + 1> kMixCenters = {
    0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 320.0f, 360.0f};

static const char *mixBandName(size_t band) {
  static const char *const names[kMixBandCount] = {
      "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
  return names[band];
}
static Color mixBandColor(size_t band) {
  static const Color colors[kMixBandCount] = {
      {230, 60, 60, 255},  {240, 140, 50, 255}, {225, 195, 40, 255},
      {70, 180, 80, 255},  {50, 185, 190, 255}, {60, 110, 230, 255},
      {140, 80, 210, 255}, {215, 70, 170, 255}};
  return colors[band];
}

static bool mixAnyNonZero(const MixValues &m) {
  for (float v : m)
    if (v != 0.0f)
      return true;
  return false;
}
static bool mixChanAnyNonZero(const MixValues &m, MixChan c) {
  for (size_t b = 0; b < kMixBandCount; ++b)
    if (m[mixIndex(c, b)] != 0.0f)
      return true;
  return false;
}

// Per-hue results, indexed by round(hue / 360 * kMixLutSize).
struct MixLut {
  std::array<float, kMixLutSize> hueShift{}; // degrees to add to the hue
  std::array<float, kMixLutSize> satMul{};   // multiplier on HSL saturation
  std::array<float, kMixLutSize> lumAdj{};   // -1..+1 luminance amount
};

static MixLut buildMixLut(const MixValues &m) {
  MixLut lut;
  for (int i = 0; i < kMixLutSize; ++i) {
    const float hue = static_cast<float>(i) * 360.0f / kMixLutSize;
    size_t j = 0; // segment [kMixCenters[j], kMixCenters[j + 1])
    while (j < kMixBandCount - 1 && hue >= kMixCenters[j + 1])
      ++j;
    const size_t nx = (j + 1) % kMixBandCount; // Magenta blends back to Red
    const float t = smoothStep01(0.0f, 1.0f,
                                 (hue - kMixCenters[j]) /
                                     (kMixCenters[j + 1] - kMixCenters[j]));
    auto blend = [&](MixChan c) {
      return (m[mixIndex(c, j)] * (1.0f - t) + m[mixIndex(c, nx)] * t) / 100.0f;
    };
    const size_t k = static_cast<size_t>(i);
    lut.hueShift[k] = blend(MixChan::Hue) * kMixHueRangeDeg;
    lut.satMul[k] = 1.0f + blend(MixChan::Saturation);
    lut.lumAdj[k] = blend(MixChan::Luminance);
  }
  return lut;
}
// END colormix

// BEGIN colorgrade
// ---------------- Color Grading (3-way + global wheels) ----------------
// Lightroom-style split toning: four wheels (Shadows, Midtones, Highlights,
// Global), each with a hue + saturation (the wheel puck) and a luminance
// offset. Blending sets how softly the three tonal ranges overlap; Balance
// moves the crossover points toward the shadows (-) or the highlights (+).
//
// The result depends only on a pixel's luma, so all of it (weights, tint
// vectors, luminance offsets of all four wheels) is baked into one small
// per-luma table of RGB offsets (GradeLut). The per-pixel work is: luma,
// one table read, three adds (see applyColorPass).
enum class GradeRange : int { Shadows, Midtones, Highlights, Global, Count };
constexpr size_t kGradeRangeCount = static_cast<size_t>(GradeRange::Count);
constexpr float kGradeTintStrength = 0.30f; // sat 100 shifts a channel by 30%
constexpr float kGradeLumAmount = 0.30f;    // lum +/-100 = +/-30% of 255

struct GradeWheel {
  float hue = 0.0f;    // 0..359 degrees
  float sat = 0.0f;    // 0..100
  float lum = 0.0f;    // -100..100
  bool visible = true; // the eye icon: preview this wheel on/off
  bool neutral() const { return sat == 0.0f && lum == 0.0f; }
  bool operator==(const GradeWheel &) const = default;
};

struct ColorGrade {
  std::array<GradeWheel, kGradeRangeCount> wheel{};
  float blending = 50.0f; // 0..100
  float balance = 0.0f;   // -100..100
  bool operator==(const ColorGrade &) const = default;
  bool anyActive() const {
    for (const auto &w : wheel)
      if (w.visible && !w.neutral())
        return true;
    return false;
  }
};

// Per-luma RGB offsets (in 0..255 units), indexed by 8-bit luma.
using GradeLut = std::array<std::array<float, 256>, 3>;

// Fully saturated RGB (0..1) for a hue in degrees.
static void gradeHueToRgb(float h, float *rgb) {
  h = std::fmod(h, 360.0f);
  if (h < 0.0f)
    h += 360.0f;
  const float hp = h / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f);
  switch (static_cast<int>(hp) % 6) {
  case 0:
    rgb[0] = 1;
    rgb[1] = x;
    rgb[2] = 0;
    break;
  case 1:
    rgb[0] = x;
    rgb[1] = 1;
    rgb[2] = 0;
    break;
  case 2:
    rgb[0] = 0;
    rgb[1] = 1;
    rgb[2] = x;
    break;
  case 3:
    rgb[0] = 0;
    rgb[1] = x;
    rgb[2] = 1;
    break;
  case 4:
    rgb[0] = x;
    rgb[1] = 0;
    rgb[2] = 1;
    break;
  default:
    rgb[0] = 1;
    rgb[1] = 0;
    rgb[2] = x;
    break;
  }
}

static GradeLut buildGradeLut(const ColorGrade &g) {
  // Tint vector + luminance offset per wheel. The tint is the hue's color
  // minus its own luma, so it shifts color without changing brightness.
  float tint[kGradeRangeCount][3] = {};
  float lumOff[kGradeRangeCount] = {};
  for (size_t r = 0; r < kGradeRangeCount; ++r) {
    const GradeWheel &w = g.wheel[r];
    if (!w.visible)
      continue;
    float c[3];
    gradeHueToRgb(w.hue, c);
    const float l = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    const float k = w.sat / 100.0f * kGradeTintStrength * 255.0f;
    for (int ch = 0; ch < 3; ++ch)
      tint[r][ch] = (c[ch] - l) * k;
    lumOff[r] = w.lum / 100.0f * kGradeLumAmount * 255.0f;
  }

  // Crossovers: shadows/mids at pivot - 0.25, mids/highlights at pivot + 0.25.
  // Balance slides the pivot; Blending widens the smoothstep around each.
  const float pivot = 0.5f - 0.2f * (g.balance / 100.0f);
  const float cs = pivot - 0.25f, ch = pivot + 0.25f;
  const float hw = 0.05f + 0.30f * (g.blending / 100.0f);

  GradeLut lut{};
  for (int i = 0; i < 256; ++i) {
    const float y = static_cast<float>(i) / 255.0f;
    float wS = 1.0f - smoothStep01(cs - hw, cs + hw, y);
    float wH = smoothStep01(ch - hw, ch + hw, y);
    float wM = 1.0f - wS - wH;
    if (wM < 0.0f)
      wM = 0.0f;
    const float sum = wS + wM + wH; // > 1 only when the ranges overlap a lot
    if (sum > 1.0f) {
      wS /= sum;
      wM /= sum;
      wH /= sum;
    }
    const float wt[kGradeRangeCount] = {wS, wM, wH, 1.0f};
    for (size_t r = 0; r < kGradeRangeCount; ++r)
      for (int c = 0; c < 3; ++c)
        lut[c][i] += wt[r] * (tint[r][c] + lumOff[r]);
  }
  return lut;
}
// END colorgrade

// BEGIN colorpass
// ---------------- Color pass (float, per pixel) ----------------
// Runs on the LUT's 8-bit output. Everything here is computed in float and
// rounded/clamped once at the end, so stacking Temperature + Vibrance +
// Saturation doesn't accumulate 8-bit rounding the way chained LUT stages do.
// Order: white balance gains -> vibrance -> saturation -> color mixer -> color
// grading. Works on
// the gamma-encoded values directly (no linearization) — a standard, cheap
// approximation for a first version.
struct ColorPass {
  bool active = false;       // false = skip the pass entirely (fast path)
  float gain[3] = {1, 1, 1}; // per-channel white-balance gains (R,G,B)
  float vibrance = 0.0f;     // -1..+1
  float saturation = 1.0f;   // multiplier: 0 = gray, 1 = untouched, 2 = double
  bool mix = false;          // Color Mixer stage enabled (mixLut is valid)
  MixLut mixLut;             // only filled when `mix` is true
  bool grade = false;        // Color Grading stage enabled (gradeLut is valid)
  GradeLut gradeLut;         // only filled when `grade` is true
};

// UI values are the -100..+100 slider values.
static ColorPass makeColorPass(float temperature, float tint, float vibrance,
                               float saturation, const MixValues &mix,
                               const ColorGrade &grade) {
  ColorPass cp;
  cp.mix = mixAnyNonZero(mix);
  cp.grade = grade.anyActive();
  cp.active = cp.mix || cp.grade || temperature != 0.0f || tint != 0.0f ||
              vibrance != 0.0f || saturation != 0.0f;
  if (!cp.active)
    return cp;
  if (cp.mix)
    cp.mixLut = buildMixLut(mix);
  if (cp.grade)
    cp.gradeLut = buildGradeLut(grade);
  const float t = temperature / 100.0f, n = tint / 100.0f;
  // Warm: R up / B down. Magenta: G down, R and B slightly up.
  const float gR = (1.0f + 0.3f * t) * (1.0f + 0.1f * n);
  const float gG = 1.0f - 0.2f * n;
  const float gB = (1.0f - 0.3f * t) * (1.0f + 0.1f * n);
  // Normalize by the gains' luma so a white-balance shift changes color, not
  // overall brightness.
  const float gl = 0.299f * gR + 0.587f * gG + 0.114f * gB;
  cp.gain[0] = gR / gl;
  cp.gain[1] = gG / gl;
  cp.gain[2] = gB / gl;
  cp.vibrance = vibrance / 100.0f;
  cp.saturation = 1.0f + saturation / 100.0f;
  return cp;
}

static inline uint8_t toU8Clamped(float v) {
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  return static_cast<uint8_t>(v + 0.5f);
}

// Applies `cp` in place to `n` bytes of RGBA8 (alpha untouched). Raw pointers
// and no std::clamp/min/max calls in the loop, for the same Debug-build
// reason as AdjustmentSet::applyTo.
static void applyColorPass(const ColorPass &cp, uint8_t *p, size_t n) {
  if (!cp.active)
    return;
  const float gR = cp.gain[0], gG = cp.gain[1], gB = cp.gain[2];
  const bool doVib = cp.vibrance != 0.0f;
  const bool doSat = cp.saturation != 1.0f;
  const bool doMix = cp.mix;
  const bool doGrade = cp.grade;
  const float *gdR = cp.gradeLut[0].data();
  const float *gdG = cp.gradeLut[1].data();
  const float *gdB = cp.gradeLut[2].data();
  const float vib = cp.vibrance, sat = cp.saturation;
  const float *lutHue = cp.mixLut.hueShift.data();
  const float *lutSat = cp.mixLut.satMul.data();
  const float *lutLum = cp.mixLut.lumAdj.data();
  for (size_t i = 0; i + 3 < n; i += 4) {
    float r = static_cast<float>(p[i]) * gR;
    float g = static_cast<float>(p[i + 1]) * gG;
    float b = static_cast<float>(p[i + 2]) * gB;
    if (doVib) {
      // Positive vibrance boosts muted colors most (weight 1 - s); negative
      // mutes already-saturated colors most (weight s). s = HSV saturation.
      const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const float s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
      const float k = vib > 0.0f ? 1.0f + vib * (1.0f - s) : 1.0f + vib * s;
      const float l = 0.299f * r + 0.587f * g + 0.114f * b;
      r = l + (r - l) * k;
      g = l + (g - l) * k;
      b = l + (b - l) * k;
    }
    if (doSat) {
      const float l = 0.299f * r + 0.587f * g + 0.114f * b;
      r = l + (r - l) * sat;
      g = l + (g - l) * sat;
      b = l + (b - l) * sat;
    }
    if (doMix) {
      // Color Mixer. Work in HSL on values clamped to 0..255.
      r = r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r);
      g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
      b = b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b);
      const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const float d = mx - mn;
      if (d > 0.0f) { // d == 0 is a pure gray: no hue, nothing to mix
        float h;      // hue in sextants, 0..6
        if (mx == r) {
          h = (g - b) / d;
          if (h < 0.0f)
            h += 6.0f;
        } else if (mx == g) {
          h = (b - r) / d + 2.0f;
        } else {
          h = (r - g) / d + 4.0f;
        }
        h *= 60.0f; // degrees
        int li = static_cast<int>(h * (kMixLutSize / 360.0f) + 0.5f);
        if (li >= kMixLutSize)
          li -= kMixLutSize;

        // Near-grays have an unstable hue; fade the effect in with chroma so
        // it can't turn sensor noise in neutral areas into colored speckle.
        const float chroma = d * (1.0f / 255.0f);
        float w = chroma >= kMixChromaFull ? 1.0f : chroma / kMixChromaFull;
        w = w * w * (3.0f - 2.0f * w);

        float l = (mx + mn) * (0.5f / 255.0f);
        const float den = 1.0f - (l > 0.5f ? 2.0f * l - 1.0f : 1.0f - 2.0f * l);
        float s = den > 1e-6f ? chroma / den : 0.0f;
        if (s > 1.0f)
          s = 1.0f;

        h += lutHue[li] * w;
        if (h < 0.0f)
          h += 360.0f;
        else if (h >= 360.0f)
          h -= 360.0f;
        s *= 1.0f + (lutSat[li] - 1.0f) * w;
        s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
        const float a = lutLum[li] * w * kMixLumAmount;
        l = a >= 0.0f ? l + (1.0f - l) * a : l + l * a; // toward white / black

        // HSL -> RGB.
        const float c =
            (1.0f - (l > 0.5f ? 2.0f * l - 1.0f : 1.0f - 2.0f * l)) * s;
        const float hp = h * (1.0f / 60.0f);
        int sector = static_cast<int>(hp);
        if (sector > 5)
          sector = 5;
        float f =
            hp - 2.0f * static_cast<float>(sector >> 1); // 0..2 within pair
        f = f - 1.0f;
        const float x = c * (1.0f - (f < 0.0f ? -f : f));
        const float m0 = l - 0.5f * c;
        float r1, g1, b1;
        switch (sector) {
        case 0:
          r1 = c;
          g1 = x;
          b1 = 0;
          break;
        case 1:
          r1 = x;
          g1 = c;
          b1 = 0;
          break;
        case 2:
          r1 = 0;
          g1 = c;
          b1 = x;
          break;
        case 3:
          r1 = 0;
          g1 = x;
          b1 = c;
          break;
        case 4:
          r1 = x;
          g1 = 0;
          b1 = c;
          break;
        default:
          r1 = c;
          g1 = 0;
          b1 = x;
          break;
        }
        r = (r1 + m0) * 255.0f;
        g = (g1 + m0) * 255.0f;
        b = (b1 + m0) * 255.0f;
      }
    }
    if (doGrade) {
      // Color Grading: offsets depend only on the pixel's luma.
      r = r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r);
      g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
      b = b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b);
      const int yi =
          static_cast<int>(0.299f * r + 0.587f * g + 0.114f * b + 0.5f);
      r += gdR[yi];
      g += gdG[yi];
      b += gdB[yi];
    }
    p[i] = toU8Clamped(r);
    p[i + 1] = toU8Clamped(g);
    p[i + 2] = toU8Clamped(b);
  }
}
// END colorpass

// BEGIN geometry
// ---------------- Picture geometry (shared by masks and the pipeline) ----
// A crop region in pixels of the oriented, straightened image (see
// EditorState::rotated). A default-constructed CropRect (w == h == 0) means
// "no crop — use the whole image".
struct CropRect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool operator==(const CropRect &) const = default;
};

// Orientation of the image: an optional horizontal mirror, then `rot`
// clockwise quarter turns. Together these cover all 8 ways to rotate/flip a
// picture, so any sequence of rotate/flip buttons collapses into one value
// (e.g. flip-H then flip-V is just rot = 2, no matter how often it's done).
struct Orient {
  int rot = 0;       // 0..3 clockwise quarter turns, applied after the flip
  bool flip = false; // mirror left-right first
  bool identity() const { return rot == 0 && !flip; }
  bool operator==(const Orient &) const = default;
};

struct Dims {
  int w, h;
};

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

// Size of a w x h picture after turning it `deg` degrees (either direction).
//   expand: the bounding box of the turned picture — nothing is lost, the
//           corners are empty.
//   fit:    the largest axis-aligned rectangle with the picture's own aspect
//           ratio that fits inside the turned picture — no empty corners.
// Both formulas are symmetric in (w, h), so a picture that is later turned a
// further quarter-turn gets exactly the transposed size — which is what
// lets a crop rectangle follow a 90-degree turn by simple index math.
static Dims rotatedDims(int w, int h, float deg, bool expand) {
  if (deg == 0.0f)
    return {w, h};
  const double rad = static_cast<double>(deg) * kDegToRad;
  const double c = std::abs(std::cos(rad)), sn = std::abs(std::sin(rad));
  const double dw = w, dh = h;
  constexpr double eps = 1e-6; // keeps exact quarter-turns from rounding up
  if (expand)
    return {std::max(1, static_cast<int>(std::ceil(dw * c + dh * sn - eps))),
            std::max(1, static_cast<int>(std::ceil(dw * sn + dh * c - eps)))};
  const double sc = std::min(dw / (dw * c + dh * sn), dh / (dw * sn + dh * c));
  return {std::max(1, static_cast<int>(std::floor(sc * dw + eps))),
          std::max(1, static_cast<int>(std::floor(sc * dh + eps)))};
}


// Where a point of the ORIGINAL picture ends up after orient -> straighten ->
// crop (the order syncSource runs them), as one similarity transform in
// continuous pixel coordinates (pixel centers at +0.5):
//
//     p_source = L * p_original + t
//
// L is orthogonal (a rotation, or a mirror followed by a rotation), so
// lengths are preserved and only positions and axis directions change. Masks
// are stored against the ORIGINAL picture and mapped through this to wherever
// the picture currently is, so no geometry change ever has to touch a mask.
struct GeoFrame {
  double m00 = 1, m01 = 0, m10 = 0, m11 = 1, tx = 0, ty = 0;
  double origW = 1, origH = 1; // the untouched picture, px
  double srcW = 1, srcH = 1;   // after orient / straighten / crop, px

  double det() const { return m00 * m11 - m01 * m10; }
  void toSrc(double x, double y, double &ox, double &oy) const {
    ox = m00 * x + m01 * y + tx;
    oy = m10 * x + m11 * y + ty;
  }
  void toOrig(double x, double y, double &ox, double &oy) const {
    const double px = x - tx, py = y - ty, d = det();
    ox = (m11 * px - m01 * py) / d;
    oy = (-m10 * px + m00 * py) / d;
  }
  // Ratio of the two pictures' shorter sides: radii are stored as a fraction
  // of the original's, but shown against the current picture's.
  double radiusScale() const {
    return std::min(origW, origH) / std::min(srcW, srcH);
  }
  // Direction of an axis (degrees, clockwise) in the original -> in the
  // source. An axis has no head or tail, so the result is kept in [-90, 90].
  static float wrapAxis(float a) {
    while (a > 90.0f)
      a -= 180.0f;
    while (a < -90.0f)
      a += 180.0f;
    return a;
  }
  float axisToSrc(float deg) const {
    const double phi = std::atan2(m10, m00) / kDegToRad; // where +x goes
    return wrapAxis(static_cast<float>(det() > 0 ? deg + phi : phi - deg));
  }
  float axisToOrig(float deg) const {
    const double phi = std::atan2(m10, m00) / kDegToRad;
    return wrapAxis(static_cast<float>(det() > 0 ? deg - phi : phi - deg));
  }
  // Direction of an arrow (degrees clockwise from +x, kept in (-180, 180]) in
  // the original -> in the source and back. Unlike an axis, an arrow has a
  // head, so a mirror or a half turn must not fold it onto its opposite: the
  // unit vector is pushed through the linear part and the angle re-read.
  float dirToSrc(float deg) const {
    const double a = static_cast<double>(deg) * kDegToRad;
    const double vx = std::cos(a), vy = std::sin(a);
    return static_cast<float>(
        std::atan2(m10 * vx + m11 * vy, m00 * vx + m01 * vy) / kDegToRad);
  }
  float dirToOrig(float deg) const {
    const double a = static_cast<double>(deg) * kDegToRad;
    const double vx = std::cos(a), vy = std::sin(a), d = det();
    return static_cast<float>(
        std::atan2((-m10 * vx + m00 * vy) / d, (m11 * vx - m01 * vy) / d) /
        kDegToRad);
  }
};

// Builds the frame for a picture of ow x oh with this geometry. With no
// original size known yet (ow <= 0) the frame is the identity and masks pass
// through unchanged.
static GeoFrame makeGeoFrame(int ow, int oh, const Orient &o, float angle,
                             bool expand, const CropRect &crop) {
  GeoFrame f;
  if (ow <= 0 || oh <= 0)
    return f;
  f.origW = ow;
  f.origH = oh;
  int w = ow, h = oh;
  // Composes p' = A p + b after everything so far.
  auto post = [&f](double a00, double a01, double a10, double a11, double b0,
                   double b1) {
    const double n00 = a00 * f.m00 + a01 * f.m10;
    const double n01 = a00 * f.m01 + a01 * f.m11;
    const double n10 = a10 * f.m00 + a11 * f.m10;
    const double n11 = a10 * f.m01 + a11 * f.m11;
    const double t0 = a00 * f.tx + a01 * f.ty + b0;
    const double t1 = a10 * f.tx + a11 * f.ty + b1;
    f.m00 = n00, f.m01 = n01, f.m10 = n10, f.m11 = n11, f.tx = t0, f.ty = t1;
  };
  if (o.flip) // orientImage: mirror first...
    post(-1, 0, 0, 1, w, 0);
  for (int i = 0; i < o.rot; ++i) { // ...then clockwise quarter turns
    post(0, -1, 1, 0, h, 0);        // (x, y) -> (h - y, x)
    std::swap(w, h);
  }
  if (angle != 0.0f) { // rotateArbitrary: clockwise about the center
    const Dims d = rotatedDims(w, h, angle, expand);
    const double rad = static_cast<double>(angle) * kDegToRad;
    const double c = std::cos(rad), s = std::sin(rad);
    post(c, -s, s, c, d.w / 2.0 - (c * w / 2.0 - s * h / 2.0),
         d.h / 2.0 - (s * w / 2.0 + c * h / 2.0));
    w = d.w, h = d.h;
  }
  if (!crop.empty()) { // cropImage's clamping, exactly
    const int x0 = std::clamp(crop.x, 0, w), y0 = std::clamp(crop.y, 0, h);
    const int x1 = std::clamp(crop.x + crop.w, x0, w);
    const int y1 = std::clamp(crop.y + crop.h, y0, h);
    post(1, 0, 0, 1, -x0, -y0);
    w = x1 - x0, h = y1 - y0;
  }
  f.srcW = std::max(1, w);
  f.srcH = std::max(1, h);
  return f;
}
// END geometry

// BEGIN masks
// ---------------- Masks (local adjustments) ----------------
// A mask is a shape plus its own set of Adjust sliders (the same table as the
// global ones: kAdjustmentSpecs). The picture is rendered as usual first
// (global sliders, curves, mixer, grading); then, for each enabled mask in list
// order, every pixel is run through THAT mask's adjustments and blended back
// in by the mask's weight (0..1) at that pixel:
//
//     out = base + (adjusted(base) - base) * weight
//
// Weight 1 = the full local adjustment, 0 = pixel untouched. Later masks
// stack on top of earlier ones.
//
// Mask geometry is stored against the ORIGINAL picture: centers as a fraction
// of its width / height, radii as a fraction of its shorter side, angles as
// drawn on it. A GeoFrame (see "geometry") maps that to the picture as it is
// now (after orient / straighten / crop) whenever a mask is drawn, edited or
// rendered — so rotating, flipping, straightening or cropping never has to
// touch a mask, undo and redo stay consistent, and removing a crop brings
// back a mask that was outside it. Being normalized also makes the same mask
// render identically on the small drag-preview image and the full-size one.
// The shape structs below have two forms: the stored one, and a "source"
// one (same struct, same fields) already mapped into the current picture,
// which is what rendering and the on-canvas editing work with.
//
// ADDING ANOTHER KIND OF MASK (linear gradient, brush, luminosity range...):
//   1. a shape struct with its parameters (like RadialShape) and its three
//      GeoFrame conversions (toSource, fromSource, applySourceEdit),
//   2. an "Eval" struct (like RadialEval): the shape resolved against one
//      concrete image size, able to say which part of a row it touches
//      (rowSpan) and to fill in that row's weights (rowWeights),
//   3. one more alternative in MaskShape and MaskEval (same order as MaskType)
//      and one more line in maskTypeName and maskShapeToSource,
//   4. its overlay + handles in the UI (paintMaskOverlay / maskPress / ...).
// The pixel pipeline, undo, the preview path, both worker threads, export and
// the per-mask sliders all work on the variants and need no changes.
using AdjValues = std::array<float, static_cast<size_t>(AdjId::Count)>;

// Same order as the alternatives of MaskShape / MaskEval below.
enum class MaskType : int { Radial, Linear, Count };

static const char *maskTypeName(MaskType t) {
  switch (t) {
  case MaskType::Radial:
    return "Radial";
  case MaskType::Linear:
    return "Linear";
  default:
    return "Mask";
  }
}

// ---- Radial (elliptical) mask ----
// The shape resolved for one image size: pixel-space center, rotation and the
// constants the per-row work needs.
struct RadialEval {
  float cx = 0, cy = 0;    // center, px
  float c = 1, s = 0;      // cos / sin of the clockwise rotation
  float invRx = 0, invRy = 0; // 1 / radius, px
  float inner = 0;         // normalized distance where the falloff begins
  float invFall = 1;       // 1 / (1 - inner)
  float qa = 0, qb = 0;    // 1/rx^2, 1/ry^2 (row-span quadratic)

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? False if none
  // (the ellipse doesn't reach this row). Exact for the rotated ellipse: for
  // a row at offset dy from the center the outline is the quadratic
  //   A dx^2 + B dy dx + C dy^2 = 1   with det = A C - B^2/4 = qa qb,
  // so its two roots are  dx = (-B dy/2 +- sqrt(A - qa qb dy^2)) / A.
  bool rowSpan(int y, int W, int &x0, int &x1) const {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float A = c * c * qa + s * s * qb;
    const float B = 2.0f * c * s * (qa - qb);
    const float disc = A - qa * qb * dy * dy;
    if (disc < 0.0f)
      return false;
    const float root = std::sqrt(disc);
    const float lo = (-0.5f * B * dy - root) / A;
    const float hi = (-0.5f * B * dy + root) / A;
    // A pixel is the row's x-th sample at x + 0.5; pad by a pixel each side.
    x0 = static_cast<int>(std::floor(cx + lo)) - 1;
    x1 = static_cast<int>(std::ceil(cx + hi)) + 1;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > W ? W : x1;
    return x1 > x0;
  }

  // Weights for pixels [x0, x1) of row `y`: 1 inside `inner`, easing to 0 at
  // the outline (smoothstep), 0 outside. Plain loop, no std helpers, same
  // Debug-build reason as applyColorPass.
  void rowWeights(int y, int x0, int x1, float *out) const {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    for (int x = x0; x < x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      const float u = (dx * c + dy * s) * invRx;
      const float v = (-dx * s + dy * c) * invRy;
      const float d = std::sqrt(u * u + v * v);
      float w;
      if (d <= inner) {
        w = 1.0f;
      } else if (d >= 1.0f) {
        w = 0.0f;
      } else {
        const float t = (d - inner) * invFall;
        w = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      out[x - x0] = w;
    }
  }
};

struct RadialShape {
  float cx = 0.5f, cy = 0.5f; // center, fraction of image width / height
  float rx = 0.3f, ry = 0.3f; // half-axes, fraction of the SHORTER image side
  float angle = 0.0f;         // rotation of the rx axis, degrees clockwise
  float feather = 0.5f;       // 0 = hard edge .. 1 = fades from the center
  bool operator==(const RadialShape &) const = default;

  // Inner edge of the falloff, normalized: 1 - feather, kept a hair short of
  // 1 so a "hard" edge is still a (one-percent) ramp and never divides by 0.
  float innerFrac() const {
    const float in = 1.0f - feather;
    return in < 0.0f ? 0.0f : (in > 0.99f ? 0.99f : in);
  }

  // Stored (against the original picture) -> as it sits in the current one.
  RadialShape toSource(const GeoFrame &f) const {
    RadialShape s = *this;
    double px, py;
    f.toSrc(cx * f.origW, cy * f.origH, px, py);
    s.cx = static_cast<float>(px / f.srcW);
    s.cy = static_cast<float>(py / f.srcH);
    const double k = f.radiusScale();
    s.rx = static_cast<float>(rx * k);
    s.ry = static_cast<float>(ry * k);
    s.angle = f.axisToSrc(angle);
    return s;
  }
  // The inverse: a shape drawn on the current picture -> stored form.
  static RadialShape fromSource(const RadialShape &s, const GeoFrame &f) {
    RadialShape r = s;
    double ox, oy;
    f.toOrig(s.cx * f.srcW, s.cy * f.srcH, ox, oy);
    r.cx = static_cast<float>(ox / f.origW);
    r.cy = static_cast<float>(oy / f.origH);
    const double k = f.radiusScale();
    r.rx = static_cast<float>(s.rx / k);
    r.ry = static_cast<float>(s.ry / k);
    r.angle = f.axisToOrig(s.angle);
    return r;
  }
  // Folds an edit made on the source form (`before` -> `after`) into this
  // stored shape, touching only the fields that changed: an unrelated
  // slider never nudges the position through float round-off, and an
  // unchanged mask compares equal (so it makes no undo step).
  void applySourceEdit(const RadialShape &before, const RadialShape &after,
                       const GeoFrame &f) {
    if (after.cx != before.cx || after.cy != before.cy) {
      const RadialShape p = fromSource(after, f);
      cx = p.cx;
      cy = p.cy;
    }
    const double k = f.radiusScale();
    if (after.rx != before.rx)
      rx = static_cast<float>(after.rx / k);
    if (after.ry != before.ry)
      ry = static_cast<float>(after.ry / k);
    if (after.angle != before.angle)
      angle = f.axisToOrig(after.angle);
    if (after.feather != before.feather)
      feather = after.feather;
  }

  RadialEval prepare(int W, int H) const {
    const float S = static_cast<float>(W < H ? W : H);
    const float rad = angle * (3.14159265358979f / 180.0f);
    const float pxRx = (rx < kMaskMinRadius ? kMaskMinRadius : rx) * S;
    const float pxRy = (ry < kMaskMinRadius ? kMaskMinRadius : ry) * S;
    RadialEval e;
    e.cx = cx * static_cast<float>(W);
    e.cy = cy * static_cast<float>(H);
    e.c = std::cos(rad);
    e.s = std::sin(rad);
    e.invRx = 1.0f / pxRx;
    e.invRy = 1.0f / pxRy;
    e.qa = e.invRx * e.invRx;
    e.qb = e.invRy * e.invRy;
    e.inner = innerFrac();
    e.invFall = 1.0f / (1.0f - e.inner);
    return e;
  }
};

// ---- Linear (graduated) mask ----
// Weight 1 on one side, easing (smoothstep) to 0 on the other. The shape is a
// center point, a direction `u` (the way the effect fades OUT) and a half
// length: the ramp runs from the line `len` behind the center (weight 1)
// to the line `len` ahead of it (weight 0), perpendicular to `u`.
struct LinearEval {
  float cx = 0, cy = 0;     // center, px
  float ux = 0, uy = 1;     // unit direction of fade-out
  float len = 1;            // half length of the ramp, px
  float invLen = 1;         // 1 / len

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? The weight is
  // non-zero exactly where s < 1, and s is linear in x, so that is a
  // half-line of the row (or all / none of it when the axis is vertical).
  bool rowSpan(int y, int W, int &x0, int &x1) const {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float bound = len - dy * uy; // need  dx * ux < bound
    x0 = 0;
    x1 = W;
    if (std::fabs(ux) < 1e-6f)
      return bound > 0.0f;
    const float edge = bound / ux; // dx = edge is the weight-0 line
    if (ux > 0.0f) {
      const int e = static_cast<int>(std::ceil(cx + edge)) + 1;
      x1 = e > W ? W : e;
    } else {
      const int e = static_cast<int>(std::floor(cx + edge)) - 1;
      x0 = e < 0 ? 0 : e;
    }
    return x1 > x0;
  }

  // Plain loop, no std helpers (same Debug-build reason as RadialEval).
  void rowWeights(int y, int x0, int x1, float *out) const {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float base = dy * uy;
    for (int x = x0; x < x1; ++x) {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      const float s = (dx * ux + base) * invLen; // -1 .. +1 across the ramp
      float w;
      if (s <= -1.0f) {
        w = 1.0f;
      } else if (s >= 1.0f) {
        w = 0.0f;
      } else {
        const float t = (s + 1.0f) * 0.5f;
        w = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      out[x - x0] = w;
    }
  }
};

struct LinearShape {
  float cx = 0.5f, cy = 0.5f; // center, fraction of image width / height
  float len = 0.25f;          // half length of the ramp, fraction of the
                              // SHORTER image side
  float angle = 90.0f;        // direction of fade-out, degrees clockwise from
                              // +x in (-180, 180]; 90 = full at the top,
                              // fading towards the bottom
  bool operator==(const LinearShape &) const = default;

  LinearShape toSource(const GeoFrame &f) const {
    LinearShape s = *this;
    double px, py;
    f.toSrc(cx * f.origW, cy * f.origH, px, py);
    s.cx = static_cast<float>(px / f.srcW);
    s.cy = static_cast<float>(py / f.srcH);
    s.len = static_cast<float>(len * f.radiusScale());
    s.angle = f.dirToSrc(angle);
    return s;
  }
  static LinearShape fromSource(const LinearShape &s, const GeoFrame &f) {
    LinearShape r = s;
    double ox, oy;
    f.toOrig(s.cx * f.srcW, s.cy * f.srcH, ox, oy);
    r.cx = static_cast<float>(ox / f.origW);
    r.cy = static_cast<float>(oy / f.origH);
    r.len = static_cast<float>(s.len / f.radiusScale());
    r.angle = f.dirToOrig(s.angle);
    return r;
  }
  // Same contract as RadialShape::applySourceEdit: only changed fields.
  void applySourceEdit(const LinearShape &before, const LinearShape &after,
                       const GeoFrame &f) {
    if (after.cx != before.cx || after.cy != before.cy) {
      const LinearShape p = fromSource(after, f);
      cx = p.cx;
      cy = p.cy;
    }
    if (after.len != before.len)
      len = static_cast<float>(after.len / f.radiusScale());
    if (after.angle != before.angle)
      angle = f.dirToOrig(after.angle);
  }

  LinearEval prepare(int W, int H) const {
    const float S = static_cast<float>(W < H ? W : H);
    const float rad = angle * (3.14159265358979f / 180.0f);
    LinearEval e;
    e.cx = cx * static_cast<float>(W);
    e.cy = cy * static_cast<float>(H);
    e.ux = std::cos(rad);
    e.uy = std::sin(rad);
    e.len = (len < kMaskMinRadius ? kMaskMinRadius : len) * S;
    e.invLen = 1.0f / e.len;
    return e;
  }
};

// ---- Shape variants: one alternative per MaskType ----
using MaskShape = std::variant<RadialShape, LinearShape>;
using MaskEval = std::variant<RadialEval, LinearEval>;

static MaskShape maskShapeToSource(const MaskShape &shape,
                                   const GeoFrame &f) {
  return std::visit([&](const auto &s) -> MaskShape { return s.toSource(f); },
                    shape);
}
static MaskEval prepareMask(const MaskShape &shape, int W, int H) {
  return std::visit(
      [&](const auto &s) -> MaskEval { return s.prepare(W, H); }, shape);
}
static bool maskRowSpan(const MaskEval &e, int y, int W, int &x0, int &x1) {
  return std::visit(
      [&](const auto &ev) { return ev.rowSpan(y, W, x0, x1); }, e);
}
static void maskRowWeights(const MaskEval &e, int y, int x0, int x1,
                           float *out) {
  std::visit([&](const auto &ev) { ev.rowWeights(y, x0, x1, out); }, e);
}

// One mask as the user sees it: a shape, whether it is on / inverted, and its
// own slider values (indexed by AdjId, 0 = untouched).
struct Mask {
  MaskShape shape;
  bool enabled = true;
  bool invert = false; // adjust everything OUTSIDE the shape instead
  AdjValues adj{};
  MaskType type() const { return static_cast<MaskType>(shape.index()); }
  bool operator==(const Mask &) const = default;
};
using MaskSet = std::vector<Mask>;

// The Tone-stage sliders composed into one 256-entry table (per-channel, so
// the same table serves R, G and B), with an optional curve table applied
// last. Shared by the global pipeline (AdjustmentSet::buildLut) and masks.
static std::array<uint8_t, 256>
toneLutFromValues(const AdjValues &values,
                  const std::array<float, 256> *curveLut) {
  std::array<uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    float x = static_cast<float>(i);
    for (const auto &spec : kAdjustmentSpecs) {
      if (spec.stage != AdjustmentSpec::Stage::Tone)
        continue; // color-stage sliders are handled by applyColorPass
      const float v = values[static_cast<size_t>(spec.id)];
      if (v != 0.0f)
        // Round + clamp after EACH spec, not just once at the end, so the
        // result is bit-identical to running the specs as separate per-pixel
        // passes that each stored an 8-bit result.
        x = static_cast<float>(std::clamp(
            static_cast<int>(std::lround(spec.apply(x, v))), 0, 255));
    }
    if (curveLut) // x is an integral 0..255 here
      x = static_cast<float>(std::clamp(
          static_cast<int>(std::lround((*curveLut)[static_cast<int>(x)])), 0,
          255));
    lut[static_cast<size_t>(i)] = static_cast<uint8_t>(x);
  }
  return lut;
}

// A mask frozen for the render thread: its shape and everything its sliders
// boil down to. Built on the UI thread (O(256)), copied into RenderParams.
struct MaskPass {
  MaskShape shape;
  bool invert = false;
  bool tone = false;                // any Tone slider non-zero (lut is valid)
  std::array<uint8_t, 256> lut{};
  ColorPass color;
};

// Only masks that are on AND would change something are included. Shapes are
// mapped into the current picture here, once, on the UI thread.
static std::vector<MaskPass> buildMaskPasses(const MaskSet &masks,
                                             const GeoFrame &frame) {
  std::vector<MaskPass> out;
  for (const Mask &m : masks) {
    if (!m.enabled)
      continue;
    MaskPass mp;
    mp.shape = maskShapeToSource(m.shape, frame);
    mp.invert = m.invert;
    for (const auto &spec : kAdjustmentSpecs)
      if (spec.stage == AdjustmentSpec::Stage::Tone &&
          m.adj[static_cast<size_t>(spec.id)] != 0.0f)
        mp.tone = true;
    mp.color = makeColorPass(m.adj[static_cast<size_t>(AdjId::Temperature)],
                             m.adj[static_cast<size_t>(AdjId::Tint)],
                             m.adj[static_cast<size_t>(AdjId::Vibrance)],
                             m.adj[static_cast<size_t>(AdjId::Saturation)],
                             MixValues{}, ColorGrade{});
    if (!mp.tone && !mp.color.active)
      continue; // a mask with no adjustments does nothing
    if (mp.tone)
      mp.lut = toneLutFromValues(m.adj, nullptr);
    out.push_back(std::move(mp));
  }
  return out;
}

// Applies one mask to the rows held in `q` (`n` bytes = whole rows, the first
// of which is image row `y0`). Per row: find the span the shape touches, copy
// it to `scratch`, run the mask's own LUT + color pass on the copy, and blend
// the result back by the weights. `scratch` holds one row of RGBA, `wbuf` one
// row of weights.
static void applyMaskToBlock(const MaskPass &mp, const MaskEval &ev,
                             uint8_t *q, size_t n, int y0, int W,
                             uint8_t *scratch, float *wbuf) {
  const size_t rowBytes = static_cast<size_t>(W) * 4;
  const int rows = static_cast<int>(n / rowBytes);
  const uint8_t *lut = mp.lut.data();
  for (int r = 0; r < rows; ++r) {
    int x0 = 0, x1 = W;
    // Inverted masks affect the whole row (weight = 1 - w), so no span cut.
    if (!mp.invert && !maskRowSpan(ev, y0 + r, W, x0, x1))
      continue;
    const int len = x1 - x0;
    if (len <= 0)
      continue;
    maskRowWeights(ev, y0 + r, x0, x1, wbuf);
    uint8_t *row = q + static_cast<size_t>(r) * rowBytes +
                   static_cast<size_t>(x0) * 4;
    const size_t bytes = static_cast<size_t>(len) * 4;
    std::copy_n(row, bytes, scratch);
    if (mp.tone)
      for (size_t i = 0; i + 3 < bytes; i += 4) {
        scratch[i] = lut[scratch[i]];
        scratch[i + 1] = lut[scratch[i + 1]];
        scratch[i + 2] = lut[scratch[i + 2]];
      }
    applyColorPass(mp.color, scratch, bytes);
    for (int i = 0; i < len; ++i) {
      float w = wbuf[i];
      if (mp.invert)
        w = 1.0f - w;
      if (w <= 0.0f)
        continue;
      uint8_t *p = row + static_cast<size_t>(i) * 4;
      const uint8_t *t = scratch + static_cast<size_t>(i) * 4;
      for (int k = 0; k < 3; ++k) {
        const float a = static_cast<float>(p[k]);
        const float b = static_cast<float>(t[k]);
        p[k] = toU8Clamped(a + (b - a) * w);
      }
    }
  }
}
// END masks

// BEGIN render
// ---------------- Render parameters + the pixel pass ----------------
// Everything the per-pixel pipeline needs, frozen into one small value:
// three 256-entry tables (tone sliders + tone curves, per channel) and the
// float color pass. Building it is O(256) (AdjustmentSet::buildRenderParams),
// so the UI thread builds one and hands a *copy* to the render worker — the
// worker never touches the AdjustmentSet, which the UI thread keeps mutating.
struct RenderParams {
  std::array<std::array<uint8_t, 256>, 3> luts{}; // R, G, B tables
  ColorPass color;                                // float pass, after the LUTs
  std::vector<MaskPass> masks; // local adjustments, applied after all of it
};

// Runs the whole per-pixel pipeline on a copy of `src` (which is never
// modified). Works in blocks of rows so that a background render can be
// abandoned quickly: when `latest` is given, it is re-read before every
// block, and if it no longer equals `gen` (a newer request or an
// invalidate() superseded this one) the function stops and returns null.
// With no `latest` it always runs to completion, which is the synchronous
// path. Each pixel is independent, so blocking does not change the result.
//
// Raw pointers and no std:: helpers in the inner loops, same Debug-build
// reason as applyColorPass.
static std::shared_ptr<CanvasImage>
renderWith(const CanvasImage &src, const RenderParams &rp,
           const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0) {
  auto img = std::make_shared<CanvasImage>(src);
  uint8_t *p = img->pixels.data();
  const size_t total = img->pixels.size();
  const size_t rowBytes = static_cast<size_t>(img->width) * 4;
  constexpr size_t kBlockRows = 128;
  const size_t blockBytes = rowBytes * kBlockRows;
  const bool sharedLut = rp.luts[0] == rp.luts[1] && rp.luts[1] == rp.luts[2];
  const uint8_t *lr = rp.luts[0].data();
  const uint8_t *lg = rp.luts[1].data();
  const uint8_t *lb = rp.luts[2].data();

  // Masks are resolved against THIS image's size (the preview is smaller
  // than the full picture; the shapes are normalized so both agree).
  std::vector<MaskEval> maskEvals;
  std::vector<uint8_t> maskScratch;
  std::vector<float> maskWeights;
  if (!rp.masks.empty() && img->width > 0) {
    maskEvals.reserve(rp.masks.size());
    for (const MaskPass &m : rp.masks)
      maskEvals.push_back(prepareMask(m.shape, img->width, img->height));
    maskScratch.resize(rowBytes);
    maskWeights.resize(static_cast<size_t>(img->width));
  }

  for (size_t off = 0; off < total; off += blockBytes) {
    if (latest && latest->load(std::memory_order_relaxed) != gen)
      return nullptr; // superseded: abandon
    const size_t n = total - off < blockBytes ? total - off : blockBytes;
    uint8_t *q = p + off;
    if (sharedLut) { // fast path (no per-channel curves): one shared table
      for (size_t i = 0; i + 3 < n; i += 4) {
        q[i] = lr[q[i]];
        q[i + 1] = lr[q[i + 1]];
        q[i + 2] = lr[q[i + 2]];
      }
    } else {
      for (size_t i = 0; i + 3 < n; i += 4) {
        q[i] = lr[q[i]];
        q[i + 1] = lg[q[i + 1]];
        q[i + 2] = lb[q[i + 2]];
      }
    }
    applyColorPass(rp.color, q, n); // no-op when all four color sliders are 0
    for (size_t k = 0; k < maskEvals.size(); ++k)
      applyMaskToBlock(rp.masks[k], maskEvals[k], q, n,
                       static_cast<int>(off / rowBytes), img->width,
                       maskScratch.data(), maskWeights.data());
  }
  return img;
}
// END render

// BEGIN curve
// ---------------- Tone curve ----------------
// Four curves — master (RGB) plus one each for R, G, B. Each is a list of
// control points (input level, output level), both in
// 0..255, sorted by x. The first and last points sit at x = 0 and x = 255
// (their y can move, e.g. to lift blacks). Interpolated with a monotone cubic
// (Fritsch-Carlson / PCHIP): it passes through every point, never overshoots
// between neighbors, and stays monotone wherever the points are — so dragging
// a point can't make the curve ring or invert by accident.
struct CurvePoint {
  float x = 0.0f, y = 0.0f;
  bool operator==(const CurvePoint &) const = default;
};
using CurvePoints = std::vector<CurvePoint>;

static CurvePoints identityCurve() { return {{0.0f, 0.0f}, {255.0f, 255.0f}}; }

// Which curve is being edited / stored. Master applies to all three channels
// (first); Red/Green/Blue then apply to their own channel only.
enum class CurveChannel : int { Master, Red, Green, Blue, Count };
constexpr size_t kCurveChannelCount = static_cast<size_t>(CurveChannel::Count);
using CurveSet = std::array<CurvePoints, kCurveChannelCount>;

static CurveSet identityCurves() {
  return {identityCurve(), identityCurve(), identityCurve(), identityCurve()};
}

// Evaluates the curve at every input level 0..255 (unrounded, clamped).
static std::array<float, 256> curveToLut(const CurvePoints &pts) {
  std::array<float, 256> lut{};
  const size_t n = pts.size();
  if (n < 2) {
    for (int i = 0; i < 256; ++i)
      lut[i] = static_cast<float>(i);
    return lut;
  }
  std::vector<float> h(n - 1), d(n - 1), m(n);
  for (size_t k = 0; k + 1 < n; ++k) {
    h[k] = std::max(pts[k + 1].x - pts[k].x, 1e-3f);
    d[k] = (pts[k + 1].y - pts[k].y) / h[k];
  }
  if (n == 2) {
    m[0] = m[1] = d[0];
  } else {
    for (size_t k = 1; k + 1 < n; ++k) {
      if (d[k - 1] * d[k] <= 0.0f) {
        m[k] = 0.0f; // local extremum or flat: no overshoot
      } else {
        const float w1 = 2.0f * h[k] + h[k - 1];
        const float w2 = h[k] + 2.0f * h[k - 1];
        m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
      }
    }
    // Shape-preserving one-sided end slopes.
    auto endSlope = [](float h0, float h1, float d0, float d1) {
      float sl = ((2.0f * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
      if (sl * d0 <= 0.0f)
        sl = 0.0f;
      else if (d0 * d1 <= 0.0f && std::fabs(sl) > 3.0f * std::fabs(d0))
        sl = 3.0f * d0;
      return sl;
    };
    m[0] = endSlope(h[0], h[1], d[0], d[1]);
    m[n - 1] = endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
  }
  size_t k = 0;
  for (int i = 0; i < 256; ++i) {
    const float x = static_cast<float>(i);
    float v;
    if (x <= pts.front().x) {
      v = pts.front().y;
    } else if (x >= pts.back().x) {
      v = pts.back().y;
    } else {
      while (k + 2 < n && x > pts[k + 1].x)
        ++k;
      const float t = (x - pts[k].x) / h[k];
      const float t2 = t * t, t3 = t2 * t;
      v = (2.0f * t3 - 3.0f * t2 + 1.0f) * pts[k].y +
          (t3 - 2.0f * t2 + t) * h[k] * m[k] +
          (-2.0f * t3 + 3.0f * t2) * pts[k + 1].y + (t3 - t2) * h[k] * m[k + 1];
    }
    lut[i] = std::clamp(v, 0.0f, 255.0f);
  }
  return lut;
}

// True when rounding the curve to 8 bits gives the identity mapping — the
// pipeline skips the curve stage entirely then (exactly equivalent).
static bool curveIsIdentity(const std::array<float, 256> &lut) {
  for (int i = 0; i < 256; ++i)
    if (std::lround(lut[i]) != i)
      return false;
  return true;
}

// Inserts an interior point at (x, y), keeping the list sorted. Returns its
// index, or -1 if it would sit on/next to an existing point or an end.
static int curveInsert(CurvePoints &pts, float x, float y) {
  x = std::clamp(x, 0.0f, 255.0f);
  y = std::clamp(y, 0.0f, 255.0f);
  size_t i = 0;
  while (i < pts.size() && pts[i].x < x)
    ++i;
  if (i == 0 || i >= pts.size())
    return -1;
  if (x - pts[i - 1].x < kCurveMinGap || pts[i].x - x < kCurveMinGap)
    return -1;
  pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(i), CurvePoint{x, y});
  return static_cast<int>(i);
}

// Moves point `idx`. End points keep their x; interior points stay strictly
// between their neighbors (kCurveMinGap apart), so order never changes.
static void curveMovePoint(CurvePoints &pts, int idx, float x, float y) {
  if (idx < 0 || idx >= static_cast<int>(pts.size()))
    return;
  pts[idx].y = std::clamp(y, 0.0f, 255.0f);
  if (idx == 0 || idx == static_cast<int>(pts.size()) - 1)
    return;
  const float lo = pts[idx - 1].x + kCurveMinGap;
  const float hi = pts[idx + 1].x - kCurveMinGap;
  if (lo <= hi)
    pts[idx].x = std::clamp(x, lo, hi);
}

// Removes an interior point; the two end points can't be removed.
static bool curveRemove(CurvePoints &pts, int idx) {
  if (idx <= 0 || idx >= static_cast<int>(pts.size()) - 1)
    return false;
  pts.erase(pts.begin() + idx);
  return true;
}
// END curve

// Owns the live value of every slider, the committed crop and orientation,
// their undo/redo history, and the logic to render them onto a copy of the
// original image.
// A slider drags continuously (many `set()` calls per second while held)
// with no drag-end event from LiteUI to mark "the gesture is over" — so
// `set` coalesces repeated calls for the *same* slider into one history
// entry instead of growing it every frame; touching a different slider (or
// an undo/redo, or a crop) starts a fresh entry. Dragging Exposure from -20
// to +40 is one undo step, not hundreds. A crop is always exactly one step.
class AdjustmentSet {
public:
  float get(AdjId id) const { return values_[static_cast<size_t>(id)]; }
  const CropRect &crop() const { return crop_; }
  const Orient &orient() const { return orient_; }
  float angle() const { return angle_; }  // straighten angle, degrees CW
  bool expand() const { return expand_; } // straighten: keep the full canvas?
  // One of the four tone curves (default: the master RGB curve).
  const CurvePoints &curve(CurveChannel ch = CurveChannel::Master) const {
    return curves_[static_cast<size_t>(ch)];
  }

  void set(AdjId id, float value) {
    values_[static_cast<size_t>(id)] = value;
    if (id == lastEdited_ && historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
    } else {
      appendSnapshot();
      lastEdited_ = id;
    }
  }

  // Commits a new crop (in original-image coordinates; an empty CropRect
  // removes the crop). Always its own undo step.
  void setCrop(const CropRect &c) {
    if (c == crop_)
      return;
    crop_ = c;
    appendSnapshot();
    lastEdited_ = AdjId::Count; // never coalesce a slider drag into a crop
  }

  // Commits a new orientation and straighten angle together with the crop
  // rectangle remapped into them (the crop is stored relative to the
  // oriented, straightened image, so all three must change together). One
  // undo step.
  void setGeometry(const Orient &o, float angle, const CropRect &c) {
    if (o == orient_ && angle == angle_ && c == crop_)
      return;
    orient_ = o;
    angle_ = angle;
    crop_ = c;
    pushStep();
  }

  // Sets the straighten angle. Changing it changes the size of the picture,
  // so any committed crop is dropped (undo brings it back). `coalesce` = true
  // for a slider drag (many calls per second): repeated calls fold into one
  // undo step. false for a discrete action (a nudge button, reset).
  void setAngle(float deg, bool coalesce) {
    if (deg == angle_)
      return;
    angle_ = deg;
    crop_ = CropRect{};
    if (coalesce && lastEdited_ == kEditAngle &&
        historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    lastEdited_ = coalesce ? kEditAngle : AdjId::Count;
  }

  // Straighten's corner handling: false = auto-crop away the empty corners,
  // true = grow the canvas so nothing is lost (corners transparent). Only
  // affects the picture when the angle is non-zero, but is always its own
  // undo step so undo/redo stays predictable.
  void setExpand(bool expand) {
    if (expand == expand_)
      return;
    expand_ = expand;
    if (angle_ != 0.0f)
      crop_ = CropRect{}; // the picture just changed size
    pushStep();
  }

  // Sets one tone curve. `coalesce` = true while a point is being
  // added/dragged (many calls per second): repeated calls to the SAME channel
  // fold into ONE undo step, until endCurveEdit() closes it. false for a
  // discrete action (delete a point, reset the curve).
  void setCurve(CurveChannel ch, const CurvePoints &c, bool coalesce) {
    CurvePoints &cur = curves_[static_cast<size_t>(ch)];
    if (c == cur)
      return;
    cur = c;
    if (coalesce && lastEdited_ == kEditCurve && lastCurveCh_ == ch &&
        historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same gesture
      return;
    }
    pushStep();
    lastEdited_ = coalesce ? kEditCurve : AdjId::Count;
    lastCurveCh_ = ch;
  }
  // Master-curve shorthand.
  void setCurve(const CurvePoints &c, bool coalesce) {
    setCurve(CurveChannel::Master, c, coalesce);
  }
  // Resets all four curves to straight lines as ONE undo step (no-op if they
  // already are).
  void resetAllCurves() {
    const CurvePoints id = identityCurve();
    bool any = false;
    for (const auto &c : curves_)
      any = any || !(c == id);
    if (!any)
      return;
    curves_ = identityCurves();
    pushStep();
  }
  // The point drag ended: the next curve edit starts a fresh undo step.
  void endCurveEdit() {
    if (lastEdited_ == kEditCurve)
      lastEdited_ = AdjId::Count;
  }

  // ---- Color Mixer ----
  float mix(MixChan c, size_t band) const { return mix_[mixIndex(c, band)]; }
  const MixValues &mixValues() const { return mix_; }
  // Sets one Color Mixer slider. Like set(): repeated calls for the SAME
  // slider (a drag) fold into one undo step; touching another slider, or any
  // other edit / undo / redo, starts a fresh one.
  void setMix(MixChan c, size_t band, float value) {
    const size_t idx = mixIndex(c, band);
    if (value == mix_[idx])
      return;
    mix_[idx] = value;
    if (lastEdited_ == kEditMix && lastMixIdx_ == idx &&
        historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    lastEdited_ = kEditMix;
    lastMixIdx_ = idx;
  }
  // Zeroes all 24 Color Mixer sliders as ONE undo step (no-op if already 0).
  void resetMix() {
    if (!mixAnyNonZero(mix_))
      return;
    mix_.fill(0.0f);
    pushStep();
  }

  // ---- Color Grading ----
  const ColorGrade &grade() const { return grade_; }
  // Commits a new grade. `key` identifies the control being dragged: repeated
  // calls with the SAME key fold into one undo step (until endGradeEdit()).
  // key < 0 = a discrete action (eye toggle, reset): always its own step.
  void setGrade(const ColorGrade &g, int key) {
    if (g == grade_)
      return;
    grade_ = g;
    if (key >= 0 && lastEdited_ == kEditGrade && lastGradeKey_ == key &&
        historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    if (key >= 0) {
      lastEdited_ = kEditGrade;
      lastGradeKey_ = key;
    }
  }
  // The drag ended: the next grading edit starts a fresh undo step.
  void endGradeEdit() {
    if (lastEdited_ == kEditGrade)
      lastEdited_ = AdjId::Count;
  }

  // ---- Masks ----
  // The untouched picture's size: masks are stored against it, and mapped
  // into the current orient / straighten / crop through geoFrame(). Set once
  // per opened file, after reset().
  void setOriginalSize(int w, int h) {
    origW_ = w;
    origH_ = h;
  }
  GeoFrame geoFrame() const {
    return makeGeoFrame(origW_, origH_, orient_, angle_, expand_, crop_);
  }
  const MaskSet &masks() const { return masks_; }
  // Commits a new mask list (add, delete, move, edit a slider...). `key`
  // identifies the control being dragged, as in setGrade: repeated calls with
  // the SAME key fold into one undo step (until endMaskEdit()); key < 0 = a
  // discrete action (add, delete, toggle): always its own step. Callers pack
  // the mask index into the key so two masks never share a step.
  void setMasks(const MaskSet &m, int key) {
    if (m == masks_)
      return;
    masks_ = m;
    if (key >= 0 && lastEdited_ == kEditMask && lastMaskKey_ == key &&
        historyIndex_ + 1 == history_.size()) {
      history_[historyIndex_] = snapshot(); // still the same drag: overwrite
      return;
    }
    pushStep();
    if (key >= 0) {
      lastEdited_ = kEditMask;
      lastMaskKey_ = key;
    }
  }
  // The drag ended: the next mask edit starts a fresh undo step.
  void endMaskEdit() {
    if (lastEdited_ == kEditMask)
      lastEdited_ = AdjId::Count;
  }

  // Fresh baseline for a newly opened image: every slider back to 0, no
  // crop, no rotation/flip, no history to undo into.
  void reset() {
    values_.fill(0.0f);
    mix_.fill(0.0f);
    grade_ = ColorGrade{};
    masks_.clear();
    crop_ = CropRect{};
    orient_ = Orient{};
    angle_ = 0.0f;
    expand_ = false;
    curves_ = identityCurves();
    history_ = {snapshot()};
    historyIndex_ = 0;
    lastEdited_ = AdjId::Count; // sentinel: no in-progress edit
  }

  bool canUndo() const { return historyIndex_ > 0; }
  bool canRedo() const { return historyIndex_ + 1 < history_.size(); }

  void undo() {
    if (!canUndo())
      return;
    restore(history_[--historyIndex_]);
    lastEdited_ = AdjId::Count; // next slider touch starts a new entry
  }
  void redo() {
    if (!canRedo())
      return;
    restore(history_[++historyIndex_]);
    lastEdited_ = AdjId::Count;
  }

  // Composes every non-zero adjustment's pointwise transform into a single
  // 256-entry lookup table, applied identically to R/G/B. Building the
  // table is O(256) regardless of image size — the expensive part of a
  // slider drag is applying it, and that's now one array lookup per
  // channel instead of a float multiply/add + lround + clamp per spec per
  // channel.
  std::array<uint8_t, 256> buildLut() const {
    // The master curve runs after the tone sliders (Lightroom's order). It's
    // skipped outright when it rounds to identity, so the untouched-curve
    // result stays bit-identical to before the curve existed.
    const auto curveLut = curveToLut(curves_[0]);
    const bool useCurve = !curveIsIdentity(curveLut);
    return toneLutFromValues(values_, useCurve ? &curveLut : nullptr);
  }

  // Per-channel R/G/B tables: buildLut() (tone sliders + master curve), then
  // each channel's own curve on top. A channel whose curve is a straight line
  // just reuses the shared table, so with no per-channel curves all three
  // tables are identical (see applyTo's fast path).
  std::array<std::array<uint8_t, 256>, 3> buildChannelLuts() const {
    const auto base = buildLut();
    std::array<std::array<uint8_t, 256>, 3> out{};
    for (size_t c = 0; c < 3; ++c) {
      const auto cl = curveToLut(curves_[1 + c]);
      if (curveIsIdentity(cl)) {
        out[c] = base;
        continue;
      }
      for (int i = 0; i < 256; ++i)
        out[c][i] = static_cast<uint8_t>(
            std::clamp(static_cast<int>(std::lround(cl[base[i]])), 0, 255));
    }
    return out;
  }

  // Freezes the current slider/curve state into a self-contained value that
  // another thread can use (see RenderParams).
  RenderParams buildRenderParams() const {
    RenderParams rp;
    rp.luts = buildChannelLuts();
    rp.color = buildColorPass();
    if (!masks_.empty())
      rp.masks = buildMaskPasses(masks_, geoFrame());
    return rp;
  }

  // Non-destructive: always starts from `original`'s untouched pixels, so
  // re-running after any value change never compounds onto a prior result.
  // Synchronous; the full-resolution path for big images goes through
  // RenderWorker instead (see updateImageFromAdjustments).
  std::shared_ptr<CanvasImage> applyTo(const CanvasImage &original) const {
    return renderWith(original, buildRenderParams());
  }

  ColorPass buildColorPass() const {
    return makeColorPass(get(AdjId::Temperature), get(AdjId::Tint),
                         get(AdjId::Vibrance), get(AdjId::Saturation), mix_,
                         grade_);
  }

private:
  // Everything one undo step restores: slider values, crop, orientation,
  // straighten angle and corner mode.
  struct Snapshot {
    std::array<float, static_cast<size_t>(AdjId::Count)> values{};
    CropRect crop;
    Orient orient;
    float angle = 0.0f;
    bool expand = false;
    CurveSet curves = identityCurves();
    MixValues mix{};
    ColorGrade grade;
    MaskSet masks;
  };
  Snapshot snapshot() const {
    return {values_, crop_,  orient_, angle_, expand_,
            curves_, mix_,   grade_,  masks_};
  }
  void restore(const Snapshot &s) {
    values_ = s.values;
    crop_ = s.crop;
    orient_ = s.orient;
    angle_ = s.angle;
    expand_ = s.expand;
    curves_ = s.curves;
    mix_ = s.mix;
    grade_ = s.grade;
    masks_ = s.masks;
  }
  // The single place a new undo step is added: drops any redo tail, appends
  // the current state, and forgets the oldest steps beyond kMaxUndoSteps
  // (history_ holds the baseline plus at most that many steps). Leaves
  // lastEdited_ to the caller, since slider drags set it and everything
  // else clears it.
  void appendSnapshot() {
    history_.resize(historyIndex_ + 1); // discard any redo tail
    history_.push_back(snapshot());
    ++historyIndex_;
    if (history_.size() > kMaxUndoSteps + 1) {
      const size_t drop = history_.size() - (kMaxUndoSteps + 1);
      history_.erase(history_.begin(),
                     history_.begin() + static_cast<std::ptrdiff_t>(drop));
      historyIndex_ -= drop;
    }
  }
  // Appends the current state as a new undo step (dropping any redo tail)
  // and ends any in-progress slider/angle drag coalescing.
  void pushStep() {
    appendSnapshot();
    lastEdited_ = AdjId::Count;
  }

  // Pseudo-AdjId for `lastEdited_` marking an in-progress angle drag (the
  // angle isn't one of the pointwise sliders in kAdjustmentSpecs).
  static constexpr AdjId kEditAngle =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 1);
  // ...and an in-progress tone-curve point drag.
  static constexpr AdjId kEditCurve =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 2);
  // ...and an in-progress Color Mixer slider drag (which one: lastMixIdx_).
  static constexpr AdjId kEditMix =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 3);

  // ...and an in-progress Color Grading drag (which control: lastGradeKey_).
  static constexpr AdjId kEditGrade =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 4);
  // ...and an in-progress mask edit (which control: lastMaskKey_).
  static constexpr AdjId kEditMask =
      static_cast<AdjId>(static_cast<int>(AdjId::Count) + 5);

  std::array<float, static_cast<size_t>(AdjId::Count)> values_{};
  CropRect crop_;
  Orient orient_;
  float angle_ = 0.0f;
  bool expand_ = false;
  CurveSet curves_ = identityCurves();
  CurveChannel lastCurveCh_ =
      CurveChannel::Master; // channel of the open curve drag
  MixValues mix_{};
  size_t lastMixIdx_ = 0; // slider of the open Color Mixer drag
  ColorGrade grade_;
  int lastGradeKey_ = -1; // control of the open Color Grading drag
  MaskSet masks_;
  int lastMaskKey_ = -1; // control of the open mask drag
  int origW_ = 0, origH_ = 0; // see setOriginalSize
  std::vector<Snapshot> history_{Snapshot{}};
  size_t historyIndex_ = 0;
  AdjId lastEdited_ = AdjId::Count;
};

// BEGIN renderworker
// ---------------- Background render worker ----------------
// Runs the full-resolution pixel pass off the UI thread so releasing a slider
// on a 24-megapixel photo doesn't freeze the window.
//
// Thread model: everything public here is called from the UI thread only. The
// one worker thread (started lazily on the first submit, so small images
// never spawn it) sleeps until a job is waiting, renders it, and parks the
// result in a slot. It touches nothing else in the program: no `state`, no
// LiteUI calls. Its only inputs are an immutable source image and a
// RenderParams copy; its only output is the parked result.
//
// Stale work is handled with a generation number. Every submit() and
// invalidate() bumps it; a result is only ever accepted if its generation
// still equals the newest one, and the running render polls the same number
// (see renderWith) so it abandons itself within a few milliseconds of being
// superseded. Only the newest request can win, so jobs are never queued up:
// a newer submit simply replaces one still waiting.
class RenderWorker {
public:
  struct Result {
    uint64_t gen = 0;
    std::shared_ptr<CanvasImage> image;
  };

  RenderWorker() = default;
  RenderWorker(const RenderWorker &) = delete;
  RenderWorker &operator=(const RenderWorker &) = delete;

  ~RenderWorker() {
    // Makes a render in progress bail at its next block; thread_ (declared
    // last, so destroyed first) then requests stop and joins.
    latest_.store(kShutdown);
  }

  // Called from the worker thread each time a result has been parked, so the
  // UI thread can be woken to collect it (main() wires this to LiteUI::wake,
  // the one LiteUI call that is safe off the UI thread). Set it before the
  // first submit(): the thread is created by submit, so it then sees the
  // value without further synchronization.
  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }

  // Stops the worker and joins it; afterwards notify_ will never run again.
  // This worker is a static and so outlives main()'s LiteUI — call this
  // before that LiteUI is destroyed, or a late notify would wake a dead
  // window.
  void shutdown() {
    latest_.store(kShutdown); // a render in progress bails at its next block
    if (thread_.joinable()) {
      thread_.request_stop(); // also wakes a worker sleeping for a job
      thread_.join();
    }
  }

  // Queues a render of `src` and supersedes any earlier one. `src` must never
  // be modified afterwards (the editor's source images are replaced, never
  // edited in place).
  void submit(std::shared_ptr<const CanvasImage> src,
              const RenderParams &params) {
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st) { run(st); });
    const uint64_t gen = ++counter_;
    latest_.store(gen);
    outstanding_ = true;
    {
      std::lock_guard lk(m_);
      pending_ = Job{gen, std::move(src), params};
      done_.reset();
    }
    workCv_.notify_one();
  }

  // Forgets whatever is waiting or running; its result will never be taken.
  void invalidate() {
    latest_.store(++counter_);
    outstanding_ = false;
    std::lock_guard lk(m_);
    pending_.reset();
    done_.reset();
  }

  // True from submit() until its result has been taken or invalidate().
  bool outstanding() const { return outstanding_; }

  // Non-blocking: the finished result of the newest request, if it is ready.
  std::optional<Result> collect() {
    if (!outstanding_)
      return std::nullopt;
    std::lock_guard lk(m_);
    if (done_ && done_->gen != latest_.load())
      done_.reset(); // stale (defensive: run() already filters these)
    if (!done_)
      return std::nullopt;
    Result r = std::move(*done_);
    done_.reset();
    outstanding_ = false;
    return r;
  }

  // Blocking: waits for the newest request to finish. Only call while
  // outstanding() (nothing else could ever wake it).
  Result waitForLatest() {
    std::unique_lock lk(m_);
    doneCv_.wait(lk, [this] { return done_.has_value(); });
    Result r = std::move(*done_);
    done_.reset();
    outstanding_ = false;
    return r;
  }

private:
  struct Job {
    uint64_t gen = 0;
    std::shared_ptr<const CanvasImage> src;
    RenderParams params;
  };
  static constexpr uint64_t kShutdown = ~uint64_t{0};

  void run(std::stop_token st) {
    for (;;) {
      Job job;
      {
        std::unique_lock lk(m_);
        if (!workCv_.wait(lk, st, [this] { return pending_.has_value(); }))
          return; // stop requested and nothing waiting
        job = std::move(*pending_);
        pending_.reset();
      }
      auto img = renderWith(*job.src, job.params, &latest_, job.gen);
      if (!img)
        continue; // abandoned part-way: superseded
      {
        std::lock_guard lk(m_);
        // Re-check under the lock: if invalidate() ran while we finished, it
        // has already cleared the slot, and must not be refilled.
        if (latest_.load() != job.gen)
          continue;
        done_ = Result{job.gen, std::move(img)};
      }
      doneCv_.notify_all();
      if (notify_)
        notify_(); // outside the lock: it must not be able to deadlock on m_
    }
  }

  std::mutex m_;                       // guards pending_ and done_
  std::condition_variable_any workCv_; // "a job is waiting" (stop-aware)
  std::condition_variable doneCv_;     // "a result is parked"
  std::optional<Job> pending_;
  std::optional<Result> done_;
  std::function<void()> notify_;    // see setNotify; read by the worker
  std::atomic<uint64_t> latest_{0}; // newest generation; read by the worker
  uint64_t counter_ = 0;            // UI thread only
  bool outstanding_ = false;        // UI thread only
  std::jthread thread_;             // keep last: joined first on destruction
};
// END renderworker

static RenderWorker renderWorker;

// ---------------- Crop tool state ----------------

// Selection-drag "handle" bitmask: which edges of the selection a drag is
// moving. Two bits (e.g. L|T) = a corner; one bit = an edge; kEdgeMove =
// the whole rectangle.
constexpr int kEdgeL = 1, kEdgeR = 2, kEdgeT = 4, kEdgeB = 8;
constexpr int kEdgeMove = 16;

// Aspect-ratio presets in the Crop pane. ratio (width / height): 0 = free,
// negative = "same as the image being cropped".
struct CropAspect {
  const char *label;
  float ratio;
};
static const std::array<CropAspect, 6> kCropAspects = {{
    {"Free", 0.0f},
    {"Original", -1.0f},
    {"1:1", 1.0f},
    {"4:3", 4.0f / 3.0f},
    {"3:2", 3.0f / 2.0f},
    {"16:9", 16.0f / 9.0f},
}};

// The live (not yet applied) selection rectangle and the drag currently
// editing it. Coordinates are in pixels of the image being shown
// (EditorState::image), not the original.
struct CropUi {
  float l = 0, t = 0, r = 0, b = 0;
  size_t aspectIdx = 0; // index into kCropAspects

  enum class Drag { None, New, Move, Resize } drag = Drag::None;
  int mask = 0;               // Resize: which edges (kEdge* bits)
  float ax = 0, ay = 0;       // New: the fixed anchor corner
  float grabX = 0, grabY = 0; // Move: pointer offset from selection's l/t
};

// Mask-overlay drag in progress (see maskPress / maskDrag).
struct MaskUi {
  enum class Drag { None, Move, ResizeX, ResizeY, LinEnd, LinStart } drag =
      Drag::None;
  float grabX = 0, grabY = 0; // Move: pointer offset from the mask's center
};

struct EditorState {
  std::shared_ptr<CanvasImage> original; // untouched decoded pixels; null
                                         // until something is opened
  std::shared_ptr<CanvasImage> oriented; // original rotated/flipped per
                                         // adjust.orient() (== original if
                                         // none); crops are measured on this
  Orient orientedFor;                    // the orientation `oriented` has
  std::shared_ptr<CanvasImage>
      orientedPreview;                  // `oriented` downsampled for
                                        // live angle drags (null if small)
  std::shared_ptr<CanvasImage> rotated; // oriented, straightened by the
                                        // angle (== oriented at 0 degrees);
                                        // crops are measured on this
  float rotatedAngle = 0.0f;            // the angle `rotated` was built with
  bool rotatedExpand = false;           // ...and its corner mode
  std::shared_ptr<CanvasImage> source;  // rotated with the committed crop
                                        // applied (== rotated if none) —
                                        // what adjustments are run on
  CropRect sourceCrop;                  // the crop `source` was built with
  std::shared_ptr<CanvasImage> image;   // source + adjustments applied —
                                        // this is what's drawn and exported
  std::string imagePath;                // full path of the opened file
  float zoom = 1.0f;
  AdjustmentSet adjust; // current slider values + their undo/redo history
  bool dirty = true;

  // Set by a slider drag (many events per second while held). The actual
  // pixel recompute is deferred until canvasDirtySource is polled — which
  // happens at most once per rendered frame — instead of running once per
  // drag event, so a flurry of mouse-move events between two frames only
  // costs one recompute, not one per event.
  bool adjustDirty = false;

  // Interactive-preview path for large images. While a slider is being
  // dragged, adjustments are applied to `previewBase` (`source`
  // box-downsampled to roughly kPreviewMaxEdge on its long side) instead
  // of the full-resolution original, and the result (`previewImage`) is
  // drawn stretched to the document's full size. One full-resolution
  // recompute runs when the drag ends (View::onDragEnd). `previewBase` is
  // null for images already small enough that a preview wouldn't be
  // cheaper, in which case dragging just uses the full-resolution path.
  std::shared_ptr<CanvasImage> previewBase;
  std::shared_ptr<CanvasImage> previewImage;
  bool dragging = false;

  CropUi crop; // live crop selection; only shown/editable while the Crop pane
               // (activity 2) is open

  // Masks pane (activity 7). The masks themselves live in `adjust` (so they
  // are undoable); this is only which one is selected and how it is shown.
  int maskSel = -1; // index into adjust.masks(); read it through selMask()
  bool maskShowTint = false; // paint the selected mask's weight in red
  MaskUi mask;

  int activeActivity = 0; // -1 = sidebar closed
  float sidebarWidth = 220.0f;
  int openMenu = -1;      // -1 = no dropdown open, else its menu id
  bool maximized = false; // mirrors our own toggle (no getter in LiteUI)

  // One-line message shown in the status bar after the image info: open and
  // export failures, and export success. Cleared by the next open, export or
  // edit (see clearStatus's callers).
  std::string statusMsg;
  bool statusIsError = false;

  // While an angle drag shows its low-res preview, the picture's size is
  // changing every frame but `image` (full-res) isn't rebuilt until the drag
  // ends — so the document size comes from these overrides instead.
  float previewDocW = 0.0f, previewDocH = 0.0f;
  bool dragIsAngle = false; // the drag in progress is the angle slider

  // True while `previewImage` stands in for `image`: during a drag, and after
  // it until the background full-resolution render lands (renderWorker).
  bool showPreview() const {
    return previewImage && (dragging || renderWorker.outstanding());
  }

  bool previewDocActive() const {
    return dragging && previewImage && previewDocW > 0.0f;
  }
  float docWidth() const {
    if (previewDocActive())
      return previewDocW;
    return image ? static_cast<float>(image->width) : kPlaceholderW;
  }
  float docHeight() const {
    if (previewDocActive())
      return previewDocH;
    return image ? static_cast<float>(image->height) : kPlaceholderH;
  }
  void markDirty() { dirty = true; }
  void markAdjustDirty() { adjustDirty = true; }

  // Histogram of what is currently shown (the mid-drag preview while a
  // slider is held, else the full-resolution result). Recomputed lazily by
  // the histogram canvas's dirty poll when `histStale` is set; `histVersion`
  // bumps on every recompute so that canvas knows to repaint.
  std::array<uint32_t, 256> histR{}, histG{}, histB{}, histL{};
  bool histStale = true;
  uint32_t histVersion = 0;
};

static EditorState state;

// Shows `msg` in the status bar. Kept to one short line: newlines (library
// error strings sometimes have them) become spaces, and long text is cut on
// a UTF-8 boundary with an ellipsis.
static void setStatus(std::string msg, bool isError) {
  constexpr size_t kMaxLen = 110;
  for (char &c : msg)
    if (c == '\n' || c == '\r')
      c = ' ';
  if (msg.size() > kMaxLen) {
    size_t cut = kMaxLen - 3;
    while (cut > 0 && (static_cast<unsigned char>(msg[cut]) & 0xC0) == 0x80)
      --cut; // don't split a multi-byte character
    msg.resize(cut);
    msg += "...";
  }
  state.statusMsg = std::move(msg);
  state.statusIsError = isError;
}
static void clearStatus() {
  state.statusMsg.clear();
  state.statusIsError = false;
}

static std::string baseName(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void setZoom(float z) {
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
}

static void syncSource(bool force);

static void updatePreviewFromAdjustments();

// Defined after GeometryWorker (needs its type); forward-declared here
// because updateImageFromAdjustments (right below) is written once, in its
// natural place, even though it now has to decide between the synchronous
// pipeline and GeometryWorker before either exists yet in the file.
static bool geometryDirty();
static void submitGeometryJob();

// ---- Handing the worker's result back to the UI thread ----
// The event loop sleeps until the OS hands it an event, and a worker finishing
// is not one — so the worker announces itself: RenderWorker::notify_ (wired
// in main() to LiteUI::wake, which is safe from any thread) wakes the loop,
// which polls the canvases, and collectFullRender() below picks the result up
// from there. Nothing runs while no render is outstanding, so an idle editor
// costs nothing.

static void adoptFullRender(RenderWorker::Result &&r) {
  state.image = std::move(r.image);
  state.previewImage.reset(); // the real thing replaces the stand-in
  state.histStale = true;
  state.markDirty();
}

// Non-blocking; polled once per frame from flushPendingAdjust.
static void collectFullRender() {
  if (auto r = renderWorker.collect())
    adoptFullRender(std::move(*r));
}

// Blocking: makes `state.image` current. For the few places that need the
// real pixels *now* (export).
static void finishFullRender() {
  if (!renderWorker.outstanding())
    return;
  adoptFullRender(renderWorker.waitForLatest());
}

// A drag starts (or continues) on the preview path. A full-resolution render
// still in flight would be for values the drag is about to change, so drop it.
static void enterDrag(bool isAngle) {
  if (!state.dragging)
    renderWorker.invalidate();
  state.dragging = true; // switches recompute+paint to the preview path
  state.dragIsAngle = isAngle;
}

// True for photos big enough that a full-resolution geometry rebuild
// (orient -> straighten -> crop -> render) is worth moving off the UI
// thread. Mirrors makePreviewBase's own threshold, so "this photo is large"
// means the same thing everywhere in the file. Gated on `original` (the
// decoded file), not `source` (today's cropped/rotated working image), so
// a single editing session for one big photo always takes the same path —
// it doesn't flip to synchronous just because a crop happened to shrink
// `source` below the threshold.
static bool isLargeImage(const CanvasImage &img) {
  return std::max(img.width, img.height) >
         static_cast<int>(kPreviewMaxEdge * kPreviewMinRatio);
}

// Rebuilds `state.image` (what's drawn and exported) from the untouched
// `state.source` pixels (the original, cropped if a crop is committed)
// using the current adjustment values. Called after every change to
// `state.adjust` — a slider drag, undo, redo, a crop, or opening a new
// image.
//
// Two paths:
//  * Deferred (large images, adjustment-only changes): render the cheap
//    preview now so the picture updates immediately, and let the worker
//    produce the full-resolution `image` in the background. paintCanvas keeps
//    showing the preview (EditorState::showPreview) until it arrives.
//  * Synchronous (everything else): small images, where the pass is faster
//    than the hand-off, and any change that alters the picture's geometry
//    (crop, rotate, straighten, a newly opened file), because then
//    `source`, `image`, the document size and the crop selection must all
//    change together.
static void updateImageFromAdjustments() {
  if (!state.original)
    return;
  clearStatus(); // the image is about to change: an old message is stale
  renderWorker.invalidate(); // whatever was in flight is now out of date

  // Orient/straighten/crop are comparatively expensive at full resolution —
  // the bilinear straighten especially — so on a large photo they run on
  // geometryWorker instead of blocking here. geometryDirty() folds in
  // "a job is already running" (see its definition): either way, submit()
  // replaces whatever's in flight, so this is safe to call every time.
  if (isLargeImage(*state.original) && geometryDirty()) {
    submitGeometryJob();
    return; // keep showing the current picture until the bundle lands
  }

  const std::shared_ptr<CanvasImage> sourceBefore = state.source;
  syncSource(false); // picks up a crop change from apply/undo/redo
  const bool sourceChanged = state.source != sourceBefore;
  const bool canDefer = state.previewBase && !sourceChanged && state.image &&
                        state.image->width == state.source->width &&
                        state.image->height == state.source->height;
  if (canDefer) {
    updatePreviewFromAdjustments(); // cheap stand-in, shown immediately
    renderWorker.submit(state.source, state.adjust.buildRenderParams());
    return;
  }
  state.image = state.adjust.applyTo(*state.source);
  state.previewImage.reset(); // superseded by the full-resolution result
  state.previewDocW = state.previewDocH = 0.0f;
  state.histStale = true;
  state.markDirty();
}

// Box-downsamples `src` by the smallest integer factor that brings its
// long side to at most kPreviewMaxEdge. Returns null when `src` is already
// within kPreviewMaxEdge * kPreviewMinRatio — below that, a preview
// wouldn't be meaningfully cheaper than the full image, so the drag path
// just uses the full-resolution one. Runs once per opened image.
static std::shared_ptr<CanvasImage> makePreviewBase(const CanvasImage &src) {
  if (!isLargeImage(src))
    return nullptr;
  int longEdge = std::max(src.width, src.height);
  int k = (longEdge + kPreviewMaxEdge - 1) / kPreviewMaxEdge; // ceil
  int w = src.width / k, h = src.height / k;
  if (w <= 0 || h <= 0)
    return nullptr;
  auto out = std::make_shared<CanvasImage>(w, h);
  const int area = k * k;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int sum[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < k; ++dy) {
        const uint8_t *row =
            &src.pixels[(static_cast<size_t>(y * k + dy) * src.width + x * k) *
                        4];
        for (int dx = 0; dx < k; ++dx)
          for (int c = 0; c < 4; ++c)
            sum[c] += row[dx * 4 + c];
      }
      uint8_t *o = &out->pixels[(static_cast<size_t>(y) * w + x) * 4];
      for (int c = 0; c < 4; ++c)
        o[c] = static_cast<uint8_t>((sum[c] + area / 2) / area);
    }
  return out;
}

// Like updateImageFromAdjustments(), but applied to the small preview
// instead of the full-resolution original. Only touches state.previewImage;
// state.image (and so the document size, export, and info labels) is left
// alone until the drag ends.
static void updatePreviewFromAdjustments() {
  if (!state.previewBase)
    return;
  state.previewImage = state.adjust.applyTo(*state.previewBase);
  state.histStale = true;
  state.markDirty();
}

// ---------------- Crop ----------------

// Copies the `r` region of `src` into a new image (clamped to src's bounds).
static std::shared_ptr<CanvasImage> cropImage(const CanvasImage &src,
                                              const CropRect &r) {
  const int x0 = std::clamp(r.x, 0, src.width);
  const int y0 = std::clamp(r.y, 0, src.height);
  const int x1 = std::clamp(r.x + r.w, x0, src.width);
  const int y1 = std::clamp(r.y + r.h, y0, src.height);
  const int w = x1 - x0, h = y1 - y0;
  auto out = std::make_shared<CanvasImage>(w, h);
  for (int y = 0; y < h; ++y) {
    const uint8_t *from =
        src.pixels.data() + (static_cast<size_t>(y0 + y) * src.width + x0) * 4;
    std::copy_n(from, static_cast<size_t>(w) * 4,
                out->pixels.data() + static_cast<size_t>(y) * w * 4);
  }
  return out;
}

// Returns `src` rotated 90 degrees clockwise (w x h becomes h x w).
static std::shared_ptr<CanvasImage> rotate90cw(const CanvasImage &src) {
  const int w = src.width, h = src.height;
  auto out = std::make_shared<CanvasImage>(h, w);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      // Source (x, y) lands at column h-1-y, row x of the result.
      const uint8_t *from = &src.pixels[(static_cast<size_t>(y) * w + x) * 4];
      uint8_t *to =
          &out->pixels[(static_cast<size_t>(x) * h + (h - 1 - y)) * 4];
      std::copy_n(from, 4, to);
    }
  return out;
}

// Returns `src` mirrored left-right.
static std::shared_ptr<CanvasImage> flipHorizontal(const CanvasImage &src) {
  const int w = src.width, h = src.height;
  auto out = std::make_shared<CanvasImage>(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      std::copy_n(&src.pixels[(static_cast<size_t>(y) * w + x) * 4], 4,
                  &out->pixels[(static_cast<size_t>(y) * w + (w - 1 - x)) * 4]);
  return out;
}

// Applies `o` to `src`. The identity orientation returns `src` itself (no
// copy), so an unrotated image costs nothing extra.
static std::shared_ptr<CanvasImage>
orientImage(const std::shared_ptr<CanvasImage> &src, const Orient &o) {
  if (o.identity())
    return src;
  std::shared_ptr<CanvasImage> cur = src;
  if (o.flip)
    cur = flipHorizontal(*cur);
  for (int i = 0; i < o.rot; ++i)
    cur = rotate90cw(*cur);
  return cur;
}

// ---- Arbitrary-angle rotation ("straighten") ----

// Rotates `src` clockwise by `deg` degrees about its center into a canvas of
// rotatedDims() size, with bilinear resampling (in premultiplied alpha, so
// pictures with transparency blend correctly).
//   fit:    every output pixel lands inside the picture; samples that graze
//           the edge just repeat the edge pixel.
//   expand: the empty corners come out fully transparent, and the picture's
//           rotated edge is anti-aliased by a one-pixel coverage falloff.
// `latest`/`gen`: same cancellation contract as renderWith (see there) — the
// bilinear resample is the most expensive stage of an async geometry
// rebuild (GeometryWorker), so a straighten superseded by a further drag or
// button click abandons within a handful of scanlines instead of finishing
// unwanted work. Checked every kRotateCheckRows rows rather than every row:
// the check itself is cheap, but so is one scanline, so batching it keeps
// the overhead negligible either way.
static std::shared_ptr<CanvasImage>
rotateArbitrary(const CanvasImage &src, float deg, bool expand,
                const std::atomic<uint64_t> *latest = nullptr,
                uint64_t gen = 0) {
  if (deg == 0.0f)
    return std::make_shared<CanvasImage>(src);
  constexpr int kRotateCheckRows = 64;
  const Dims d = rotatedDims(src.width, src.height, deg, expand);
  auto out = std::make_shared<CanvasImage>(d.w, d.h);
  const double rad = static_cast<double>(deg) * kDegToRad;
  const double cs = std::cos(rad), sn = std::sin(rad);
  const int sw = src.width, sh = src.height;
  const double scx = sw / 2.0, scy = sh / 2.0;   // source center
  const double dcx = d.w / 2.0, dcy = d.h / 2.0; // output center
  const uint8_t *sp = src.pixels.data();
  uint8_t *op = out->pixels.data();
  auto clampi = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };

  for (int y = 0; y < d.h; ++y) {
    if (latest && (y % kRotateCheckRows) == 0 &&
        latest->load(std::memory_order_relaxed) != gen)
      return nullptr; // superseded: abandon
    const double oy = y + 0.5 - dcy;
    for (int x = 0; x < d.w; ++x) {
      const double ox = x + 0.5 - dcx;
      // Inverse of a clockwise rotation (y points down): where in the
      // source does this output pixel's center come from?
      const double sx = ox * cs + oy * sn + scx;
      const double sy = -ox * sn + oy * cs + scy;

      float cov = 1.0f;
      if (expand) {
        const double edge =
            std::min(std::min(sx, sw - sx), std::min(sy, sh - sy));
        if (edge <= -0.5)
          continue; // fully outside the picture: stays transparent
        cov = static_cast<float>(std::min(1.0, edge + 0.5));
      }

      const double u = sx - 0.5, v = sy - 0.5; // pixel centers sit at +0.5
      const int x0 = static_cast<int>(std::floor(u));
      const int y0 = static_cast<int>(std::floor(v));
      const float fx = static_cast<float>(u - x0),
                  fy = static_cast<float>(v - y0);
      const int xa = clampi(x0, sw - 1), xb = clampi(x0 + 1, sw - 1);
      const int ya = clampi(y0, sh - 1), yb = clampi(y0 + 1, sh - 1);
      const uint8_t *p00 = sp + (static_cast<size_t>(ya) * sw + xa) * 4;
      const uint8_t *p10 = sp + (static_cast<size_t>(ya) * sw + xb) * 4;
      const uint8_t *p01 = sp + (static_cast<size_t>(yb) * sw + xa) * 4;
      const uint8_t *p11 = sp + (static_cast<size_t>(yb) * sw + xb) * 4;
      const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
      const float w01 = (1 - fx) * fy, w11 = fx * fy;
      // Alpha-weighted taps: transparent pixels contribute no color.
      const float a00 = p00[3] * w00, a10 = p10[3] * w10;
      const float a01 = p01[3] * w01, a11 = p11[3] * w11;
      const float A = a00 + a10 + a01 + a11;
      if (A <= 0.0f)
        continue;
      uint8_t *o = op + (static_cast<size_t>(y) * d.w + x) * 4;
      for (int ch = 0; ch < 3; ++ch) {
        const float c =
            (p00[ch] * a00 + p10[ch] * a10 + p01[ch] * a01 + p11[ch] * a11) / A;
        o[ch] = static_cast<uint8_t>(std::clamp(c + 0.5f, 0.0f, 255.0f));
      }
      o[3] = static_cast<uint8_t>(std::clamp(A * cov + 0.5f, 0.0f, 255.0f));
    }
  }
  return out;
}

static bool cropToolActive() {
  return state.activeActivity == 2 && state.image != nullptr;
}

// Width/height ratio the selection is locked to, or 0 for free-form.
static float cropAspect() {
  const float ratio = kCropAspects[state.crop.aspectIdx].ratio;
  if (ratio >= 0.0f)
    return ratio;
  return (state.source && state.source->height > 0)
             ? static_cast<float>(state.source->width) /
                   static_cast<float>(state.source->height)
             : 0.0f;
}

// Shrinks the selection (about its center) to the locked aspect ratio, so
// it always stays inside where it already was.
static void cropFitToAspect() {
  const float a = cropAspect();
  if (a <= 0.0f)
    return;
  auto &c = state.crop;
  float w = c.r - c.l, h = c.b - c.t;
  if (w <= 0.0f || h <= 0.0f)
    return;
  const float cx = (c.l + c.r) / 2.0f, cy = (c.t + c.b) / 2.0f;
  if (w / h > a)
    w = h * a;
  else
    h = w / a;
  c.l = cx - w / 2.0f;
  c.r = cx + w / 2.0f;
  c.t = cy - h / 2.0f;
  c.b = cy + h / 2.0f;
}

// Selection = the whole image being cropped (then fitted to the aspect lock).
static void resetCropSelection() {
  auto &c = state.crop;
  c.l = 0.0f;
  c.t = 0.0f;
  c.r = state.source ? static_cast<float>(state.source->width) : 0.0f;
  c.b = state.source ? static_cast<float>(state.source->height) : 0.0f;
  c.drag = CropUi::Drag::None;
  cropFitToAspect();
}

static void setCropAspect(size_t idx) {
  state.crop.aspectIdx = idx;
  cropFitToAspect();
  state.markDirty();
}

// Rebuilds `state.oriented` / `state.source` (and the interactive-preview
// base derived from it) when the committed crop or orientation no longer
// matches what they were built with — after applying/removing a crop, a
// rotate/flip/straighten, an undo/redo across any of them, or (force) when a
// new image is opened. Also re-seeds the selection, since the image the
// selection is measured against just changed size.
static void syncSource(bool force) {
  if (!state.original)
    return;
  const CropRect crop = state.adjust.crop();
  const Orient orient = state.adjust.orient();
  const float angle = state.adjust.angle();
  const bool expand = state.adjust.expand();

  const bool orientChanged =
      force || !state.oriented || orient != state.orientedFor;
  // The corner mode only matters while there's an angle to straighten.
  const bool rotateChanged = orientChanged || !state.rotated ||
                             angle != state.rotatedAngle ||
                             (angle != 0.0f && expand != state.rotatedExpand);
  if (!force && state.source && !rotateChanged && crop == state.sourceCrop)
    return;

  if (orientChanged) {
    state.oriented = orientImage(state.original, orient);
    state.orientedFor = orient;
    state.orientedPreview = makePreviewBase(*state.oriented);
  }
  if (rotateChanged) {
    state.rotated = angle == 0.0f
                        ? state.oriented
                        : rotateArbitrary(*state.oriented, angle, expand);
    state.rotatedAngle = angle;
    state.rotatedExpand = expand;
  }
  state.sourceCrop = crop;
  state.source = crop.empty() ? state.rotated : cropImage(*state.rotated, crop);
  state.previewBase = makePreviewBase(*state.source);
  state.previewImage.reset();
  resetCropSelection();
}

// BEGIN geometryworker
// ---------------- Background geometry worker ----------------
// Runs the same pipeline syncSource() + AdjustmentSet::applyTo run
// synchronously (orient -> straighten -> crop -> render) off the UI thread
// for a large photo, so clicking Rotate or Straighten's nudge buttons on a
// 24-megapixel image doesn't freeze the window the way releasing a slider
// used to before RenderWorker.
//
// Same thread model and cancellation contract as RenderWorker (one lazily
// started thread; a generation number supersedes stale work; the running
// job polls it and abandons within a few scanlines of being superseded).
// The one real difference: RenderWorker only re-renders pixels for a source
// that hasn't changed, so it can start from `state.source`. A geometry job
// might be changing the source itself, so it always rebuilds the WHOLE
// pipeline from `original` — even a job that only changed the crop redoes
// an unchanged straighten. That wastes some work when angle is non-zero and
// only the crop is edited afterwards, but keeps a job a pure function of
// its five inputs, with nothing that can drift out of sync — and the extra
// work only costs latency on a background thread, never a UI freeze.
class GeometryWorker {
public:
  // Exactly the geometry-derived EditorState fields, adopted as one atomic
  // swap on the UI thread (see adoptGeometryBundle) so the picture, its
  // document size, and the crop selection always change together.
  struct Bundle {
    std::shared_ptr<CanvasImage> oriented;
    Orient orientedFor;
    std::shared_ptr<CanvasImage> orientedPreview;
    std::shared_ptr<CanvasImage> rotated;
    float rotatedAngle = 0.0f;
    bool rotatedExpand = false;
    std::shared_ptr<CanvasImage> source;
    CropRect sourceCrop;
    std::shared_ptr<CanvasImage> previewBase;
    std::shared_ptr<CanvasImage> image;
  };
  struct Result {
    uint64_t gen = 0;
    Bundle bundle;
  };

  GeometryWorker() = default;
  GeometryWorker(const GeometryWorker &) = delete;
  GeometryWorker &operator=(const GeometryWorker &) = delete;
  ~GeometryWorker() { latest_.store(kShutdown); }

  // See RenderWorker::setNotify / ::shutdown — same contract.
  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }
  void shutdown() {
    latest_.store(kShutdown);
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
  }

  // Queues one full rebuild, superseding any earlier one. `original` must
  // never be modified afterwards (the editor's images are replaced, never
  // edited in place — the same rule RenderWorker::submit relies on).
  void submit(std::shared_ptr<CanvasImage> original, const Orient &orient,
              float angle, bool expand, const CropRect &crop,
              const RenderParams &renderParams) {
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st) { run(st); });
    const uint64_t gen = ++counter_;
    latest_.store(gen);
    outstanding_ = true;
    {
      std::lock_guard lk(m_);
      pending_ = Job{gen,  std::move(original), orient, angle, expand,
                     crop, renderParams};
      done_.reset();
    }
    workCv_.notify_one();
  }

  void invalidate() {
    latest_.store(++counter_);
    outstanding_ = false;
    std::lock_guard lk(m_);
    pending_.reset();
    done_.reset();
  }

  bool outstanding() const { return outstanding_; }

  std::optional<Result> collect() {
    if (!outstanding_)
      return std::nullopt;
    std::lock_guard lk(m_);
    if (done_ && done_->gen != latest_.load())
      done_.reset(); // stale (defensive: run() already filters these)
    if (!done_)
      return std::nullopt;
    Result r = std::move(*done_);
    done_.reset();
    outstanding_ = false;
    return r;
  }

  Result waitForLatest() {
    std::unique_lock lk(m_);
    doneCv_.wait(lk, [this] { return done_.has_value(); });
    Result r = std::move(*done_);
    done_.reset();
    outstanding_ = false;
    return r;
  }

private:
  struct Job {
    uint64_t gen = 0;
    // Non-const, unlike RenderWorker::Job::src: orientImage (identity case)
    // and the rest of this pipeline are written in terms of the same
    // shared_ptr<CanvasImage> type `state.original` already is, so this
    // just carries that type through — never a license to mutate it.
    std::shared_ptr<CanvasImage> original;
    Orient orient;
    float angle = 0.0f;
    bool expand = false;
    CropRect crop;
    RenderParams renderParams;
  };
  static constexpr uint64_t kShutdown = ~uint64_t{0};

  // The pipeline itself. Checked for cancellation between every stage, and
  // by row/block inside the two per-pixel passes (rotateArbitrary,
  // renderWith) — so a job superseded mid-straighten or mid-render abandons
  // within a few milliseconds rather than finishing unwanted work.
  std::optional<Bundle> buildBundle(const Job &job) {
    auto cancelled = [&] { return latest_.load() != job.gen; };
    if (cancelled())
      return std::nullopt;
    Bundle b;
    b.oriented = orientImage(job.original, job.orient); // identity: no copy
    b.orientedFor = job.orient;
    if (cancelled())
      return std::nullopt;
    b.orientedPreview = makePreviewBase(*b.oriented);
    if (cancelled())
      return std::nullopt;
    if (job.angle == 0.0f) {
      b.rotated = b.oriented;
    } else {
      b.rotated = rotateArbitrary(*b.oriented, job.angle, job.expand, &latest_,
                                  job.gen);
      if (!b.rotated)
        return std::nullopt; // abandoned mid-straighten
    }
    b.rotatedAngle = job.angle;
    b.rotatedExpand = job.expand;
    if (cancelled())
      return std::nullopt;
    b.sourceCrop = job.crop;
    b.source = job.crop.empty() ? b.rotated : cropImage(*b.rotated, job.crop);
    if (cancelled())
      return std::nullopt;
    b.previewBase = makePreviewBase(*b.source);
    b.image = renderWith(*b.source, job.renderParams, &latest_, job.gen);
    if (!b.image)
      return std::nullopt; // abandoned mid-render
    return b;
  }

  void run(std::stop_token st) {
    for (;;) {
      Job job;
      {
        std::unique_lock lk(m_);
        if (!workCv_.wait(lk, st, [this] { return pending_.has_value(); }))
          return; // stop requested and nothing waiting
        job = std::move(*pending_);
        pending_.reset();
      }
      auto bundle = buildBundle(job);
      if (!bundle)
        continue; // abandoned part-way: superseded
      {
        std::lock_guard lk(m_);
        if (latest_.load() != job.gen)
          continue; // invalidate() ran while we finished
        done_ = Result{job.gen, std::move(*bundle)};
      }
      doneCv_.notify_all();
      if (notify_)
        notify_();
    }
  }

  std::mutex m_;
  std::condition_variable_any workCv_;
  std::condition_variable doneCv_;
  std::optional<Job> pending_;
  std::optional<Result> done_;
  std::function<void()> notify_;
  std::atomic<uint64_t> latest_{0};
  uint64_t counter_ = 0;
  bool outstanding_ = false;
  std::jthread thread_; // keep last: joined first on destruction
};
// END geometryworker

static GeometryWorker geometryWorker;

// True when state.adjust's committed orientation/angle/expand/crop hasn't
// been rendered into state.oriented/rotated/source yet — mirrors syncSource's
// own dirty checks, but as a read-only predicate (no rebuilding). Folds in
// "a geometry job is already running" too: that can only be true right after
// this function itself returned true and a submit followed, so the two are
// never actually independent — but checking both directly, rather than
// relying on that invariant, means a bug elsewhere fails safe (one wasted
// resubmit) instead of silently letting a stale in-flight job get adopted.
static bool geometryDirty() {
  if (geometryWorker.outstanding())
    return true;
  if (!state.oriented || !state.rotated)
    return true;
  const Orient orient = state.adjust.orient();
  const float angle = state.adjust.angle();
  const bool expand = state.adjust.expand();
  if (orient != state.orientedFor)
    return true;
  if (angle != state.rotatedAngle)
    return true;
  if (angle != 0.0f && expand != state.rotatedExpand)
    return true;
  return state.adjust.crop() != state.sourceCrop;
}

static void submitGeometryJob() {
  geometryWorker.submit(state.original, state.adjust.orient(),
                        state.adjust.angle(), state.adjust.expand(),
                        state.adjust.crop(), state.adjust.buildRenderParams());
  setStatus("Applying changes...", /*isError=*/false);
}

// Swaps in every field of a finished geometry rebuild together, so the
// picture, its document size, and the crop selection never show a
// combination that belonged to two different requests.
static void adoptGeometryBundle(GeometryWorker::Result &&r) {
  GeometryWorker::Bundle &b = r.bundle;
  state.oriented = std::move(b.oriented);
  state.orientedFor = b.orientedFor;
  state.orientedPreview = std::move(b.orientedPreview);
  state.rotated = std::move(b.rotated);
  state.rotatedAngle = b.rotatedAngle;
  state.rotatedExpand = b.rotatedExpand;
  state.source = std::move(b.source);
  state.sourceCrop = b.sourceCrop;
  state.previewBase = std::move(b.previewBase);
  state.image = std::move(b.image);
  state.previewImage.reset(); // the real thing replaces any stand-in
  state.previewDocW = state.previewDocH = 0.0f;
  resetCropSelection(); // the crop UI must match the new source's dimensions
  state.histStale = true;
  clearStatus(); // the "Applying changes..." message is no longer true
  state.markDirty();
}

// Non-blocking; polled once per frame from flushPendingAdjust, alongside
// collectFullRender.
static void collectGeometryJob() {
  if (auto r = geometryWorker.collect())
    adoptGeometryBundle(std::move(*r));
}

// Blocking: makes every field a geometry rebuild touches current. For the
// one place that needs the real pixels *now* (export).
static void finishGeometryJob() {
  if (!geometryWorker.outstanding())
    return;
  adoptGeometryBundle(geometryWorker.waitForLatest());
}

// True while a crop/rotate/flip/straighten rebuild is still running. Until
// it lands, state.rotated / state.source / the crop selection describe the
// PREVIOUS geometry while state.adjust already holds the new one, so any
// edit that measures against them (a second rotate remapping the crop rect,
// Apply Crop reading the selection) would compute from stale sizes. Those
// edits are ignored while this is true; their buttons are dimmed to match.
static bool geometryBusy() { return geometryWorker.outstanding(); }

// Which part of the selection is under (px, py) (image coordinates)?
// Returns kEdge* bits for a handle, kEdgeMove for the interior, 0 for none.
static int cropHitTest(float px, float py) {
  const auto &c = state.crop;
  const float tol = kCropHitPx / state.zoom;
  const bool inX = px >= c.l - tol && px <= c.r + tol;
  const bool inY = py >= c.t - tol && py <= c.b + tol;
  int m = 0;
  if (inY) {
    if (std::abs(px - c.l) <= tol)
      m |= kEdgeL;
    else if (std::abs(px - c.r) <= tol)
      m |= kEdgeR;
  }
  if (inX) {
    if (std::abs(py - c.t) <= tol)
      m |= kEdgeT;
    else if (std::abs(py - c.b) <= tol)
      m |= kEdgeB;
  }
  if (m)
    return m;
  if (px > c.l && px < c.r && py > c.t && py < c.b)
    return kEdgeMove;
  return 0;
}

// Sets the selection to the rectangle spanned by a fixed corner (ax, ay)
// and the pointer, honoring the aspect lock and the image bounds. Used for
// both "drag out a new selection" and "drag a corner handle".
static void cropDragCorner(float ax, float ay, float px, float py) {
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float a = cropAspect();
  const float sx = px >= ax ? 1.0f : -1.0f;
  const float sy = py >= ay ? 1.0f : -1.0f;
  float w = std::abs(px - ax), h = std::abs(py - ay);
  if (a > 0.0f) {
    w = std::max(w, h * a); // grow to whichever dimension asks for more
    h = w / a;
    const float maxW = sx > 0.0f ? W - ax : ax;
    const float maxH = sy > 0.0f ? H - ay : ay;
    if (w > maxW) {
      w = maxW;
      h = w / a;
    }
    if (h > maxH) {
      h = maxH;
      w = h * a;
    }
  }
  const float x2 = ax + sx * w, y2 = ay + sy * h;
  c.l = std::min(ax, x2);
  c.r = std::max(ax, x2);
  c.t = std::min(ay, y2);
  c.b = std::max(ay, y2);
}

// Drags one edge or corner of the existing selection to the pointer.
static void cropResize(int mask, float px, float py) {
  auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float minSz = std::min({kCropMinPx, W, H});
  const bool horiz = (mask & (kEdgeL | kEdgeR)) != 0;
  const bool vert = (mask & (kEdgeT | kEdgeB)) != 0;

  if (horiz && vert) { // corner: the opposite corner stays put
    cropDragCorner((mask & kEdgeL) ? c.r : c.l, (mask & kEdgeT) ? c.b : c.t, px,
                   py);
    return;
  }

  const float a = cropAspect();
  if (a <= 0.0f) { // free-form edge
    if (mask & kEdgeL)
      c.l = std::clamp(px, 0.0f, c.r - minSz);
    if (mask & kEdgeR)
      c.r = std::clamp(px, c.l + minSz, W);
    if (mask & kEdgeT)
      c.t = std::clamp(py, 0.0f, c.b - minSz);
    if (mask & kEdgeB)
      c.b = std::clamp(py, c.t + minSz, H);
    return;
  }

  // Aspect-locked edge: the dragged edge sets the primary dimension, and
  // the other dimension follows, centered on where the selection was.
  if (horiz) {
    const float cy = (c.t + c.b) / 2.0f;
    const bool left = (mask & kEdgeL) != 0;
    const float room = left ? c.r : W - c.l;
    const float roomByH = 2.0f * std::min(cy, H - cy) * a;
    float w = left ? c.r - px : px - c.l;
    w = std::clamp(w, minSz, std::max(minSz, std::min(room, roomByH)));
    const float h = w / a;
    if (left)
      c.l = c.r - w;
    else
      c.r = c.l + w;
    c.t = cy - h / 2.0f;
    c.b = cy + h / 2.0f;
  } else {
    const float cx = (c.l + c.r) / 2.0f;
    const bool top = (mask & kEdgeT) != 0;
    const float room = top ? c.b : H - c.t;
    const float roomByW = 2.0f * std::min(cx, W - cx) / a;
    float h = top ? c.b - py : py - c.t;
    h = std::clamp(h, minSz, std::max(minSz, std::min(room, roomByW)));
    const float w = h * a;
    if (top)
      c.t = c.b - h;
    else
      c.b = c.t + h;
    c.l = cx - w / 2.0f;
    c.r = cx + w / 2.0f;
  }
}

// Canvas mouse handlers. (lx, ly) are canvas-local screen px; dividing by
// the zoom gives image pixels.
static void cropPress(float lx, float ly) {
  state.openMenu = -1; // the canvas now swallows presses that used to fall
                       // through to the workspace's "close menu" handler
  if (!cropToolActive())
    return;
  auto &c = state.crop;
  const float px = std::clamp(lx / state.zoom, 0.0f, state.docWidth());
  const float py = std::clamp(ly / state.zoom, 0.0f, state.docHeight());
  const int hit = cropHitTest(px, py);
  if (hit == kEdgeMove) {
    c.drag = CropUi::Drag::Move;
    c.grabX = px - c.l;
    c.grabY = py - c.t;
  } else if (hit) {
    c.drag = CropUi::Drag::Resize;
    c.mask = hit;
  } else {
    // Empty area: start a new selection here. The old one stays until the
    // pointer actually moves, so a plain click doesn't wipe it out.
    c.drag = CropUi::Drag::New;
    c.ax = px;
    c.ay = py;
  }
}

static void cropDrag(float lx, float ly) {
  auto &c = state.crop;
  if (!cropToolActive() || c.drag == CropUi::Drag::None)
    return;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = std::clamp(lx / state.zoom, 0.0f, W);
  const float py = std::clamp(ly / state.zoom, 0.0f, H);
  switch (c.drag) {
  case CropUi::Drag::New:
    cropDragCorner(c.ax, c.ay, px, py);
    break;
  case CropUi::Drag::Resize:
    cropResize(c.mask, px, py);
    break;
  case CropUi::Drag::Move: {
    const float w = c.r - c.l, h = c.b - c.t;
    c.l = std::clamp(px - c.grabX, 0.0f, W - w);
    c.t = std::clamp(py - c.grabY, 0.0f, H - h);
    c.r = c.l + w;
    c.b = c.t + h;
    break;
  }
  case CropUi::Drag::None:
    break;
  }
  state.markDirty();
}

// Commits the current selection as the crop (one undo step).
static void applyCrop() {
  if (!state.source || geometryBusy())
    return;
  const auto &c = state.crop;
  const int sw = state.source->width, sh = state.source->height;
  const int x0 = std::clamp(static_cast<int>(std::lround(c.l)), 0, sw);
  const int y0 = std::clamp(static_cast<int>(std::lround(c.t)), 0, sh);
  const int x1 = std::clamp(static_cast<int>(std::lround(c.r)), x0, sw);
  const int y1 = std::clamp(static_cast<int>(std::lround(c.b)), y0, sh);
  const int w = x1 - x0, h = y1 - y0;
  if (w < 1 || h < 1)
    return;
  if (w == sw && h == sh)
    return; // selection covers everything: nothing to crop
  // The selection is relative to the already-cropped `source`; the stored
  // crop is relative to the untouched original, so add the existing offset.
  const CropRect base = state.adjust.crop(); // 0,0 when there's no crop yet
  state.adjust.setCrop(CropRect{base.x + x0, base.y + y0, w, h});
  updateImageFromAdjustments(); // syncSource() rebuilds source + selection
}

// Drops the committed crop, restoring the full original (one undo step).
static void removeCrop() {
  if (geometryBusy() || state.adjust.crop().empty())
    return;
  state.adjust.setCrop(CropRect{});
  updateImageFromAdjustments();
}

// ---------------- Rotate / flip ----------------

enum class GeoOp { RotateCW, RotateCCW, FlipH, FlipV };

// Applies one rotate/flip to an orientation, and moves the crop rectangle
// with it so the same part of the picture stays selected. (W, H) is the size
// of the straightened image BEFORE the op — the crop lives inside it — and is
// updated to the size after.
//
// Orientation algebra: a mirror reverses the direction of rotation, so
// mirroring a (flip, rot) result gives flip toggled and rot negated; a
// vertical mirror is a horizontal one plus a half turn. The straighten angle
// is a clockwise turn too, so a mirror also negates it (a picture tilted 5
// degrees clockwise, mirrored, is tilted 5 degrees counter-clockwise), while
// a quarter-turn leaves it alone — turns about the same center commute.
static void geoStep(Orient &o, float &angle, CropRect &c, int &W, int &H,
                    GeoOp op) {
  switch (op) {
  case GeoOp::RotateCW:
    o.rot = (o.rot + 1) % 4;
    if (!c.empty())
      c = CropRect{H - c.y - c.h, c.x, c.h, c.w};
    std::swap(W, H);
    break;
  case GeoOp::RotateCCW:
    o.rot = (o.rot + 3) % 4;
    if (!c.empty())
      c = CropRect{c.y, W - c.x - c.w, c.h, c.w};
    std::swap(W, H);
    break;
  case GeoOp::FlipH:
    o.rot = (4 - o.rot) % 4;
    o.flip = !o.flip;
    if (angle != 0.0f)
      angle = -angle;
    if (!c.empty())
      c.x = W - c.x - c.w;
    break;
  case GeoOp::FlipV:
    o.rot = (4 - o.rot + 2) % 4;
    o.flip = !o.flip;
    if (angle != 0.0f)
      angle = -angle;
    if (!c.empty())
      c.y = H - c.y - c.h;
    break;
  }
}

// One undo step. `state.rotated` is what crops are measured against, so its
// size is the (W, H) geoStep needs.
static void applyGeoOp(GeoOp op) {
  if (!state.original || !state.rotated || geometryBusy())
    return;
  Orient o = state.adjust.orient();
  float angle = state.adjust.angle();
  CropRect c = state.adjust.crop();
  int W = state.rotated->width, H = state.rotated->height;
  geoStep(o, angle, c, W, H, op);
  state.adjust.setGeometry(o, angle, c);
  updateImageFromAdjustments(); // syncSource() rebuilds oriented/rotated/source
}

// Undoes every 90-degree turn and flip in one step (the crop follows,
// staying on the same part of the picture; the straighten angle is kept,
// still tilting the picture the same way relative to its own edges).
static void resetOrientation() {
  if (!state.original || !state.rotated || geometryBusy() ||
      state.adjust.orient().identity())
    return;
  Orient o = state.adjust.orient();
  float angle = state.adjust.angle();
  CropRect c = state.adjust.crop();
  int W = state.rotated->width, H = state.rotated->height;
  if (o.flip)
    geoStep(o, angle, c, W, H, GeoOp::FlipH); // leaves a pure rotation
  while (o.rot != 0)
    geoStep(o, angle, c, W, H, GeoOp::RotateCCW);
  state.adjust.setGeometry(o, angle, c);
  updateImageFromAdjustments();
}

// ---------------- Straighten (arbitrary-angle rotation) ----------------

// Angles are kept to 0.1 degree, and to exactly 0 near zero.
static float snapAngle(float deg) {
  deg = std::clamp(std::round(deg * 10.0f) / 10.0f, -kAngleMax, kAngleMax);
  return deg == 0.0f ? 0.0f : deg;
}

// Slider drag: one undo step per drag. Only records the value — the
// resample is deferred to canvasDirtySource (see there), once per frame.
static void setAngleFromSlider(float deg) {
  state.adjust.setAngle(snapAngle(deg), /*coalesce=*/true);
}

// A discrete change (nudge button / reset): its own undo step, resampled at
// full resolution on the next frame.
static void setAngleDiscrete(float deg) {
  if (!state.original)
    return;
  state.adjust.setAngle(snapAngle(deg), /*coalesce=*/false);
  state.markAdjustDirty();
}
static void nudgeAngle(float delta) {
  setAngleDiscrete(state.adjust.angle() + delta);
}

static void setCornerMode(bool expand) {
  if (!state.original)
    return;
  state.adjust.setExpand(expand);
  state.markAdjustDirty();
}

// Interactive path for an angle drag on a large image: turns the small
// `orientedPreview` instead of the full-resolution picture, drawn stretched
// to the size the full-resolution result will have (previewDoc*). The
// full-resolution turn runs once, when the drag ends (View::onDragEnd).
static void updateAnglePreview() {
  if (!state.orientedPreview || !state.oriented)
    return;
  const float a = state.adjust.angle();
  const bool ex = state.adjust.expand();
  auto turned = rotateArbitrary(*state.orientedPreview, a, ex);
  state.previewImage = state.adjust.applyTo(*turned);
  state.histStale = true;
  const Dims d =
      rotatedDims(state.oriented->width, state.oriented->height, a, ex);
  state.previewDocW = static_cast<float>(d.w);
  state.previewDocH = static_cast<float>(d.h);
  state.markDirty();
}

static void undo() {
  if (!state.adjust.canUndo())
    return;
  state.adjust.undo();
  updateImageFromAdjustments();
}

static void redo() {
  if (!state.adjust.canRedo())
    return;
  state.adjust.redo();
  updateImageFromAdjustments();
}

// ---------------- Opening a file (background decode) ----------------
// Decoding a big PNG/JPEG takes long enough to freeze the window, so it runs
// on its own thread. Same contract as RenderWorker / GeometryWorker: submit()
// supersedes any earlier request, the finished result is parked until the UI
// thread collect()s it, and notify_ wakes the sleeping event loop. Until the
// result lands the previously open image stays fully usable; nothing about
// the editor state changes until adoptDecoded() swaps the new file in.
//
// A decode already running can't be interrupted (the image libraries have no
// cancel hook), so opening a second file queues behind it and the first
// file's result is simply dropped when it finishes.
class DecodeWorker {
public:
  struct Result {
    uint64_t gen = 0;
    std::string path;
    std::optional<CanvasImage> image; // empty on failure
    std::string error;                // library message when image is empty
  };

  DecodeWorker() = default;
  DecodeWorker(const DecodeWorker &) = delete;
  DecodeWorker &operator=(const DecodeWorker &) = delete;
  ~DecodeWorker() { latest_.store(kShutdown); }

  void setNotify(std::function<void()> fn) { notify_ = std::move(fn); }
  // Joins the thread; blocks until a decode already in progress finishes.
  void shutdown() {
    latest_.store(kShutdown);
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
  }

  void submit(std::string path) {
    if (!thread_.joinable())
      thread_ = std::jthread([this](std::stop_token st) { run(st); });
    const uint64_t gen = ++counter_;
    latest_.store(gen);
    outstanding_ = true;
    pendingPath_ = path;
    {
      std::lock_guard lk(m_);
      pending_ = Job{gen, std::move(path)};
      done_.reset();
    }
    workCv_.notify_one();
  }

  // True from submit() until its result has been taken.
  bool outstanding() const { return outstanding_; }
  // The file the newest request is decoding (UI thread; valid while
  // outstanding()).
  const std::string &pendingPath() const { return pendingPath_; }

  // Non-blocking: the newest request's result, if it has finished.
  std::optional<Result> collect() {
    if (!outstanding_)
      return std::nullopt;
    std::lock_guard lk(m_);
    if (done_ && done_->gen != latest_.load())
      done_.reset(); // stale (defensive: run() already filters these)
    if (!done_)
      return std::nullopt;
    Result r = std::move(*done_);
    done_.reset();
    outstanding_ = false;
    return r;
  }

private:
  struct Job {
    uint64_t gen = 0;
    std::string path;
  };
  static constexpr uint64_t kShutdown = ~uint64_t{0};

  void run(std::stop_token st) {
    for (;;) {
      Job job;
      {
        std::unique_lock lk(m_);
        if (!workCv_.wait(lk, st, [this] { return pending_.has_value(); }))
          return; // stop requested and nothing waiting
        job = std::move(*pending_);
        pending_.reset();
      }
      if (latest_.load() != job.gen)
        continue; // superseded while queued: don't even start
      Result r;
      r.gen = job.gen;
      r.path = job.path;
      r.image = liteui_image::decodeFile(job.path, &r.error);
      {
        std::lock_guard lk(m_);
        if (latest_.load() != job.gen)
          continue; // a newer open (or shutdown) overtook this one
        done_ = std::move(r);
      }
      if (notify_)
        notify_(); // outside the lock: it must not be able to deadlock on m_
    }
  }

  std::mutex m_; // guards pending_ and done_
  std::condition_variable_any workCv_;
  std::optional<Job> pending_;
  std::optional<Result> done_;
  std::function<void()> notify_;
  std::atomic<uint64_t> latest_{0};
  uint64_t counter_ = 0;     // UI thread only
  bool outstanding_ = false; // UI thread only
  std::string pendingPath_;  // UI thread only
  std::jthread thread_;      // keep last: joined first on destruction
};

static DecodeWorker decodeWorker;

// Swaps a finished decode into the editor (UI thread only).
static void adoptDecoded(DecodeWorker::Result &&r) {
  if (!r.image) {
    // Leave whatever image is already open untouched and say what happened.
    setStatus("Couldn't open " + baseName(r.path) + ": " +
                  (r.error.empty() ? std::string("unsupported or damaged image")
                                   : r.error),
              true);
    state.markDirty();
    return;
  }
  clearStatus();

  // Anything still in flight belongs to the previous picture; its result
  // must never be adopted over the new one.
  renderWorker.invalidate();
  geometryWorker.invalidate();

  state.original = std::make_shared<CanvasImage>(std::move(*r.image));
  state.imagePath = std::move(r.path);
  state.zoom = 1.0f;    // start fresh at 100% on every newly opened image
  state.adjust.reset(); // and with no adjustment, crop or rotation applied yet
  state.adjust.setOriginalSize(state.original->width, state.original->height);
  state.dragging = false;
  state.dragIsAngle = false;
  state.image.reset(); // the old file's pixels: force the synchronous path
  syncSource(true); // source = original; builds previewBase, resets selection
  updateImageFromAdjustments(); // builds state.image from state.source
}

// Non-blocking; polled once per frame from flushPendingAdjust.
static void collectDecodeJob() {
  if (auto r = decodeWorker.collect())
    adoptDecoded(std::move(*r));
}

static void openImage() {
  auto path = openFilePicker(
      "Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}, {"All Files", "*.*"}});
  if (!path)
    return; // user cancelled
  setStatus("Opening " + baseName(*path) + "...", /*isError=*/false);
  decodeWorker.submit(*path);
}

// ---------------- PNG export ----------------
// liteui.hpp only ships image *decoding* (see liteui_image::decodeFile
// above) — no encoder — so exporting is a small amount of platform code
// here, using the same libraries liteui.hpp already links in for us
// (libpng on Linux via CMakeLists' PkgConfig::PNG, WIC on Windows).

#if defined(_WIN32)

static bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut) {
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool weInitialized = coHr == S_OK;

  IWICImagingFactory *factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: CoCreateInstance failed";
    return false;
  }

  IWICStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  bool ok = false;
  do {
    if (FAILED(factory->CreateStream(&stream)))
      break;
    if (FAILED(stream->InitializeFromFilename(toWide(path).c_str(),
                                              GENERIC_WRITE)))
      break;
    if (FAILED(
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
      break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)))
      break;
    if (FAILED(frame->Initialize(nullptr)))
      break;
    if (FAILED(frame->SetSize(static_cast<UINT>(img.width),
                              static_cast<UINT>(img.height))))
      break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt)))
      break;

    const BYTE *src = img.pixels.data();
    std::vector<BYTE> swizzled;

    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
      // Encoder substituted BGRA — swap R and B.
      swizzled.resize(img.pixels.size());
      for (size_t i = 0; i < img.pixels.size(); i += 4) {
        swizzled[i + 0] = img.pixels[i + 2]; // B
        swizzled[i + 1] = img.pixels[i + 1]; // G
        swizzled[i + 2] = img.pixels[i + 0]; // R
        swizzled[i + 3] = img.pixels[i + 3]; // A
      }
      src = swizzled.data();
    } else if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA)) {
      break; // some other format we don't handle
    }

    if (FAILED(frame->WritePixels(
            static_cast<UINT>(img.height), static_cast<UINT>(img.width) * 4,
            static_cast<UINT>(img.pixels.size()), const_cast<BYTE *>(src))))
      break;
    if (FAILED(frame->Commit()))
      break;
    ok = SUCCEEDED(encoder->Commit());
  } while (false);

  if (frame)
    frame->Release();
  if (encoder)
    encoder->Release();
  if (stream)
    stream->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();

  if (!ok && errorOut)
    *errorOut = "WIC: PNG encode failed";
  return ok;
}

#else // Linux

static bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut) {
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    if (errorOut)
      *errorOut = "failed to open file for writing";
    return false;
  }

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info) {
    if (png)
      png_destroy_write_struct(&png, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: failed to create write struct";
    return false;
  }

  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: error during write";
    return false;
  }

  png_init_io(png, fp);
  png_set_IHDR(png, info, static_cast<png_uint_32>(img.width),
               static_cast<png_uint_32>(img.height), 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_bytep> rows(img.height);
  for (int y = 0; y < img.height; ++y)
    rows[y] = const_cast<png_bytep>(img.pixels.data() +
                                    static_cast<size_t>(y) * img.width * 4);
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);

  png_destroy_write_struct(&png, &info);
  fclose(fp);
  return true;
}

#endif

static void exportImage() {
  if (!state.image)
    return;            // nothing open to export
  finishGeometryJob(); // never export the stale (pre-rebuild) picture
  finishFullRender();  // never export the stale frame of a pending render

  auto path = saveFilePicker("Export as PNG", "export.png",
                             {{"PNG Image", "*.png"}}, "png");
  if (!path)
    return; // user cancelled

  std::string err;
  if (encodePngFile(*path, *state.image, &err))
    setStatus("Exported " + baseName(*path), false);
  else
    setStatus("Export failed: " +
                  (err.empty() ? "couldn't write " + baseName(*path) : err),
              true);
}

static void toggleSidebar() {
  state.activeActivity = (state.activeActivity == -1) ? 0 : -1;
  state.markDirty(); // the crop overlay shows only while the Crop pane is open
}

// ---------------- Small reusable pieces ----------------

// One row of a dropdown: label, action, and an optional shortcut hint shown
// right-aligned (display only — the real bindings are in main()).
struct MenuItem {
  std::string label;
  std::function<void()> action;
  std::string shortcut = {}; // optional
};

// A titlebar menu button ("File", "View", ...) that opens a dropdown of
// MenuItems below it when clicked. Only one menu is open at a time
// (EditorState::openMenu); picking an item runs its action and closes the
// menu.
static View menuButton(const std::string &label, int id,
                       std::vector<MenuItem> items) {
  View btn;
  btn.style.height = Size::full();
  btn.style.padding = EdgeInsets{0, 12, 0, 12};
  btn.style.justifyContent = Justify::Center;
  btn.style.alignItems = Align::Center;
  btn.style.hoverColor = kColorMenuHover;
  btn.style.backgroundColor = std::function<Color()>([id] {
    return state.openMenu == id ? kColorMenuHover : kColorTransparent;
  });
  btn.onClick = [id] { state.openMenu = (state.openMenu == id) ? -1 : id; };

  Text label_;
  label_.label = label;
  label_.fontSize = 13.0f;
  label_.color = kColorTextDark;
  btn.addChild(std::move(label_));

  View dropdown;
  dropdown.style.position = Position::Absolute;
  dropdown.style.top = kTitlebarH;
  dropdown.style.left = 0.0f;
  dropdown.style.width = Size::pixel(250);
  dropdown.style.direction = FlexDirection::Column;
  dropdown.style.backgroundColor = kColorDropdownBg;
  dropdown.style.borderWidth = 1.0f;
  dropdown.style.borderColor = kColorDropdownBorder;
  dropdown.style.borderRadius = 4.0f;
  dropdown.style.padding = EdgeInsets::all(4.0f);
  dropdown.style.zIndex = 100;
  dropdown.style.display = std::function<Display()>(
      [id] { return state.openMenu == id ? Display::Flex : Display::None; });

  for (auto &item : items) {
    View row;
    row.style.height = Size::pixel(30);
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.justifyContent = Justify::SpaceBetween;
    row.style.padding = EdgeInsets{0, 10, 0, 10};
    row.style.borderRadius = 3.0f;
    row.style.hoverColor = kColorDropdownRowHover;
    std::function<void()> action = item.action;
    row.onClick = [action] {
      action();
      state.openMenu = -1;
    };

    Text t;
    t.label = item.label;
    t.fontSize = 13.0f;
    t.color = kColorTextDark;
    row.addChild(std::move(t));
    if (!item.shortcut.empty()) {
      Text hint;
      hint.label = item.shortcut;
      hint.fontSize = 12.0f;
      hint.color = kColorTextFaint;
      row.addChild(std::move(hint));
    }
    dropdown.addChild(std::move(row));
  }

  btn.addChild(std::move(dropdown));
  return btn;
}

// One of the three window-control buttons (minimize/maximize/close).
static View winButton(const std::string &glyph, Color hoverBg,
                      std::function<void()> onClick) {
  View v;
  v.style.width = Size::pixel(46);
  v.style.height = Size::full();
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.style.hoverColor = hoverBg;
  v.onClick = std::move(onClick);

  Text t;
  t.label = glyph;
  t.fontSize = 13.0f;
  t.color = kColorTextMid;
  v.addChild(std::move(t));
  return v;
}

// A small square titlebar icon button (undo/redo, etc). `enabled` is
// optional: pass it to dim the glyph when the action isn't currently
// available — the click itself is always safe to wire up as a no-op in
// that case (undo()/redo() already check before doing anything).
static View titlebarIconBtn(const std::string &glyph,
                            std::function<void()> onClick,
                            std::function<bool()> enabled = {}) {
  View v;
  v.style.width = Size::pixel(28);
  v.style.height = Size::pixel(28);
  v.style.borderRadius = 4.0f;
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.style.hoverColor = kColorMenuHover;
  v.onClick = std::move(onClick);

  Text t;
  t.label = glyph;
  t.fontSize = 14.0f;
  t.color = enabled ? Dynamic<Color>(std::function<Color()>([enabled] {
    return enabled() ? kColorTextDark : kColorTextFaint;
  }))
                    : Dynamic<Color>(kColorTextDark);
  v.addChild(std::move(t));
  return v;
}

// A full-width sidebar button. `selected` (optional) draws it highlighted —
// used for the aspect-ratio presets, which behave like radio buttons.
// `primary` gives it the accent fill for the pane's main action.
static View sidebarButton(const std::string &label,
                          std::function<void()> onClick,
                          std::function<bool()> selected = {},
                          bool primary = false,
                          std::function<bool()> enabled = {}) {
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(28);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.hoverColor = primary ? kColorAccentHover : kColorActivityHover;
  b.style.backgroundColor =
      std::function<Color()>([selected, primary, enabled]() -> Color {
        if (enabled && !enabled())
          return kColorActivityBarBg; // dimmed: no primary fill while busy
        if (primary)
          return kColorAccent;
        return (selected && selected()) ? kColorDropdownRowHover
                                        : kColorActivityBarBg;
      });
  b.style.borderWidth = std::function<float()>(
      [selected] { return (selected && selected()) ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  // A disabled button ignores clicks (the action functions also re-check, so
  // shortcuts are covered) and shows muted text.
  b.onClick = [onClick = std::move(onClick), enabled] {
    if (!enabled || enabled())
      onClick();
  };

  Text t;
  t.label = label;
  t.fontSize = 13.0f;
  const Color liveColor = primary ? kColorWhite : kColorTextDark;
  if (enabled)
    t.color = std::function<Color()>([enabled, liveColor]() -> Color {
      return enabled() ? liveColor : kColorTextMuted;
    });
  else
    t.color = liveColor;
  b.addChild(std::move(t));
  return b;
}

// What a slider row needs to know: its label, range, how to read/write the
// value, and how to print it. Shared by the Adjust sliders (via
// buildAdjustmentSlider) and the Straighten angle slider.
struct SliderSpec {
  std::string name;
  float lo = 0.0f, hi = 1.0f;
  std::function<float()> get;
  std::function<void(float)> set; // called with each new value while dragging
  std::function<std::string(float)> format; // value -> text after "name: "
  bool isAngle = false;   // drag drives the angle preview instead of the
                          // pixel-adjustment preview (see canvasDirtySource)
  bool hasAccent = false; // tint the fill + thumb border (Color Mixer bands)
  Color accent{};
  float trackW = 150.0f;       // track length in px
  std::function<void()> onEnd; // optional: fired when a drag on it ends
};

// A single Lightroom-style slider row: a label with the live value, then a
// draggable track. Reads and writes go through the spec's get/set rather
// than binding a raw float& the way a standalone slider widget would —
// that's what lets AdjustmentSet coalesce a whole drag into one undo step
// instead of one per frame.
static View buildSliderRow(const SliderSpec &spec) {
  const float kTrackW = spec.trackW;
  constexpr float kTrackH = 4.0f;
  constexpr float kThumb = 16.0f;
  const float kMaxThumbX = kTrackW - kThumb;

  const SliderSpec s = spec;
  const float lo = s.lo;
  const float hi = s.hi;

  // Same bipolar-fill treatment as LiteUI's slider example: a range that
  // straddles zero (like ours, -100..100) fills outward from the center
  // instead of always from the left edge.
  const bool isBipolar = (lo < 0.0f && hi > 0.0f);
  const float anchor = isBipolar ? 0.0f : lo;

  auto thumbXFor = [lo, hi, kMaxThumbX](float v) {
    return std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) * kMaxThumbX;
  };

  View group;
  group.style.direction = FlexDirection::Column;
  group.style.gap = 6.0f;

  Text rowLabel;
  rowLabel.label = std::function<std::string()>(
      [s] { return s.name + ": " + s.format(s.get()); });
  rowLabel.fontSize = 13.0f;
  rowLabel.color = kColorTextMid;
  group.addChild(std::move(rowLabel));

  View track;
  track.style.width = Size::pixel(kTrackW);
  track.style.height = Size::pixel(kTrackH);
  track.style.backgroundColor = kTrackFill;
  track.style.borderWidth = 1.0f;
  track.style.borderColor = kTrackEdge;
  track.style.borderRadius = kTrackH / 2.0f;

  // Filled portion, absolutely positioned so it can grow independently of
  // the thumb. Spans from the anchor's thumb-center position to the
  // current value's.
  View fill;
  fill.style.position = Position::Absolute;
  fill.style.top = 0.0f;
  fill.style.height = Size::pixel(kTrackH);
  fill.style.left = std::function<float()>([s, thumbXFor, anchor, isBipolar] {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(s.get()) + kThumb / 2.0f;
    return std::min(anchorX, valueCenter);
  });
  fill.style.width = std::function<Size()>([s, thumbXFor, anchor, isBipolar] {
    float anchorX =
        isBipolar ? thumbXFor(anchor) + kThumb / 2.0f : thumbXFor(anchor);
    float valueCenter = thumbXFor(s.get()) + kThumb / 2.0f;
    return Size::pixel(std::abs(valueCenter - anchorX));
  });
  fill.style.backgroundColor = s.hasAccent ? s.accent : kTrackEdge;
  fill.style.borderRadius = kTrackH / 2.0f;
  track.addChild(std::move(fill));

  // Shared between onPressAt and onDragTo across one drag gesture, same
  // pattern as the standalone slider widget this is adapted from.
  auto grabX = std::make_shared<float>(0.0f);

  View thumb;
  thumb.style.position = Position::Absolute;
  thumb.style.left =
      std::function<float()>([s, thumbXFor] { return thumbXFor(s.get()); });
  thumb.style.top = -(kThumb - kTrackH) / 2.0f; // vertically centered
  thumb.style.width = Size::pixel(kThumb);
  thumb.style.height = Size::pixel(kThumb);
  thumb.style.backgroundColor = kThumbFill;
  thumb.style.borderWidth = 2.0f;
  thumb.style.borderColor = s.hasAccent ? s.accent : kThumbEdge;
  thumb.style.borderRadius = kThumb / 2.0f;

  thumb.onPressAt = [grabX](float lx, float) { *grabX = lx; };
  thumb.onDragTo = [grabX, s, thumbXFor, lo, hi, kMaxThumbX](float lx, float) {
    if (!state.original)
      return; // no image loaded yet — nothing to adjust
    float newThumbX =
        std::clamp(thumbXFor(s.get()) + (lx - *grabX), 0.0f, kMaxThumbX);
    s.set(lo + (newThumbX / kMaxThumbX) * (hi - lo));
    enterDrag(s.isAngle); // switches recompute+paint to the preview path
    // Cheap: just records the new value and asks for a recompute before
    // the next frame paints (see canvasDirtySource) — doesn't itself touch
    // a single pixel, so it stays fast no matter how often the drag fires.
    state.markAdjustDirty();
  };
  // Drag finished: leave preview mode and request one full-resolution
  // recompute (deferred to the next canvasDirtySource poll like any other
  // adjustment change — endPress polls right after this fires, so it lands
  // in the same frame the mouse is released).
  thumb.onDragEnd = [s] {
    if (s.onEnd)
      s.onEnd();
    if (!state.dragging)
      return;
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  };
  track.addChild(std::move(thumb));

  group.addChild(std::move(track));
  return group;
}

// One Adjust-pane slider for one AdjustmentSpec: adding a new adjustment to
// kAdjustmentSpecs needs no new UI code at all.
static View buildAdjustmentSlider(const AdjustmentSpec &spec) {
  const AdjId id = spec.id;
  SliderSpec s;
  s.name = spec.label;
  s.lo = spec.min;
  s.hi = spec.max;
  s.get = [id] { return state.adjust.get(id); };
  s.set = [id](float v) { state.adjust.set(id, v); };
  s.format = [](float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  return buildSliderRow(s);
}

// ---------------- Color Mixer UI ----------------
static MixChan gMixChan = MixChan::Hue; // which of Hue / Sat / Lum is shown

// One Color Mixer slider (a band's Hue, Saturation or Luminance), tinted in
// the band's own color. Same undo/preview behavior as every Adjust slider.
static View buildMixSlider(MixChan chan, size_t band) {
  SliderSpec s;
  s.name = mixBandName(band);
  s.lo = static_cast<float>(kMixMin);
  s.hi = static_cast<float>(kMixMax);
  s.get = [chan, band] { return state.adjust.mix(chan, band); };
  s.set = [chan, band](float v) { state.adjust.setMix(chan, band, v); };
  s.format = [](float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.hasAccent = true;
  s.accent = mixBandColor(band);
  return buildSliderRow(s);
}

// Zeroes all 24 mixer sliders (one undo step).
static void resetColorMix() {
  state.adjust.resetMix();
  state.markAdjustDirty();
}

// ---------------- Color Grading UI ----------------
constexpr float kGradePaneW =
    250.0f; // sidebar width the wheels are laid out for
constexpr float kGradePi = 3.14159265358979f;
constexpr int kGradeMainWheel = 130;   // Midtones wheel in 3-way mode
constexpr int kGradeSmallWheel = 84;   // Shadows / Highlights in 3-way mode
constexpr int kGradeSingleWheel = 170; // wheel in the single-range modes
constexpr float kGradeTabW = 40.0f;

static int gGradeMode = 0;   // 0 = 3-way, 1..4 = Shadows..Global
static int gGradeActive = 1; // wheel shown in the H/S/L readout (Midtones)

static const char *gradeRangeName(int r) {
  static const char *const n[kGradeRangeCount] = {"Shadows", "Midtones",
                                                  "Highlights", "Global"};
  return n[r];
}

// Wheel brightness is cosmetic (Lightroom dims the shadow wheel).
constexpr float kGradeWheelBrightness[kGradeRangeCount] = {0.62f, 1.0f, 0.80f,
                                                           1.0f};

static Color gradeHsv(float h, float s, float v, uint8_t a = 255) {
  float c[3];
  gradeHueToRgb(h, c);
  auto to8 = [&](float f) {
    const float o = (f * s + (1.0f - s)) * v * 255.0f; // blend toward white
    return static_cast<uint8_t>(std::lround(std::clamp(o, 0.0f, 255.0f)));
  };
  return Color{to8(c[0]), to8(c[1]), to8(c[2]), a};
}

static size_t gradeHash(float v) { return std::hash<float>{}(v); }
static size_t gradeMix(size_t a, size_t b) {
  return a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2));
}

// Pre-renders the hue/saturation disc once per (diameter, brightness):
// angle = hue (0 at 3 o'clock, counter-clockwise), radius = saturation,
// white in the centre. Cached in a map so the CanvasImage address stays
// stable (liteui caches the converted surface per image).
static const CanvasImage &gradeWheelImage(int d, float brightness) {
  static std::map<std::pair<int, int>, CanvasImage> cache;
  const auto key =
      std::make_pair(d, static_cast<int>(std::lround(brightness * 100)));
  const auto it = cache.find(key);
  if (it != cache.end())
    return it->second;

  CanvasImage img(d, d);
  const float R = d / 2.0f;
  for (int y = 0; y < d; ++y) {
    for (int x = 0; x < d; ++x) {
      const float dx = x + 0.5f - R, dy = y + 0.5f - R;
      const float dist = std::sqrt(dx * dx + dy * dy);
      const float alpha = std::clamp(R - dist + 0.5f, 0.0f, 1.0f); // AA rim
      if (alpha <= 0.0f)
        continue;
      const float hue = std::atan2(-dy, dx) * 180.0f / kGradePi;
      const Color c = gradeHsv(hue, std::min(1.0f, dist / R), brightness);
      const size_t i = (static_cast<size_t>(y) * d + x) * 4;
      img.pixels[i + 0] = c.r;
      img.pixels[i + 1] = c.g;
      img.pixels[i + 2] = c.b;
      img.pixels[i + 3] = static_cast<uint8_t>(std::lround(alpha * 255.0f));
    }
  }
  return cache.emplace(key, std::move(img)).first->second;
}

// Applies `fn` to a copy of the current grade and commits it (key: see
// AdjustmentSet::setGrade).
static void gradeEdit(const std::function<void(ColorGrade &)> &fn, int key) {
  ColorGrade g = state.adjust.grade();
  fn(g);
  state.adjust.setGrade(g, key);
}

// Leaves the low-res drag preview and asks for one full-resolution render.
static void gradeEndDrag() {
  state.adjust.endGradeEdit();
  if (state.dragging) {
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

// A slider bound to one field (hue / sat / lum) of one wheel.
static View buildGradeWheelSlider(const std::string &name, int range,
                                  float GradeWheel::*field, float lo, float hi,
                                  int keyBase, float trackW, bool isSigned) {
  SliderSpec s;
  s.name = name;
  s.lo = lo;
  s.hi = hi;
  s.trackW = trackW;
  s.get = [range, field] { return state.adjust.grade().wheel[range].*field; };
  s.set = [range, field, keyBase](float v) {
    gradeEdit([&](ColorGrade &g) { g.wheel[range].*field = std::round(v); },
              keyBase + range);
    gGradeActive = range;
  };
  s.format = [isSigned](float v) {
    const int i = static_cast<int>(std::lround(v));
    return isSigned && i > 0 ? "+" + std::to_string(i) : std::to_string(i);
  };
  s.onEnd = [] { state.adjust.endGradeEdit(); };
  return buildSliderRow(s);
}

// One hue/saturation wheel: click or drag sets hue (angle) + saturation
// (radius); right-click resets both.
static View buildGradeWheel(int range, int diameter, float pad) {
  const float size = static_cast<float>(diameter) + 2.0f * pad;
  View c;
  c.isCanvas = true;
  c.style.width = Size::pixel(size);
  c.style.height = Size::pixel(size);
  c.style.flexShrink = 0;

  auto apply = [range, diameter, size](float lx, float ly, bool drag) {
    if (!state.original)
      return; // no image loaded yet — nothing to grade
    const float R = diameter / 2.0f, cx = size / 2.0f, cy = size / 2.0f;
    const float dx = lx - cx, dy = ly - cy;
    const float dist = std::sqrt(dx * dx + dy * dy);
    gradeEdit(
        [&](ColorGrade &g) {
          GradeWheel &w = g.wheel[range];
          w.sat = std::round(std::clamp(dist / R, 0.0f, 1.0f) * 100.0f);
          if (dist > 2.0f) { // keep the old hue while sitting on the centre
            float hue = std::atan2(-dy, dx) * 180.0f / kGradePi;
            if (hue < 0.0f)
              hue += 360.0f;
            w.hue = std::fmod(std::round(hue), 360.0f);
          }
        },
        range);
    gGradeActive = range;
    if (drag)
      enterDrag(false); // switches recompute+paint to the preview path
    state.markAdjustDirty();
  };
  c.onPressAt = [apply](float lx, float ly) { apply(lx, ly, false); };
  c.onDragTo = [apply](float lx, float ly) { apply(lx, ly, true); };
  c.onDragEnd = [] { gradeEndDrag(); };
  c.onRightClick = [range] { // reset hue + sat, keep luminance
    if (!state.original)
      return;
    gradeEdit(
        [&](ColorGrade &g) {
          g.wheel[range].hue = 0.0f;
          g.wheel[range].sat = 0.0f;
        },
        -1);
    gGradeActive = range;
    state.markAdjustDirty();
  };

  c.onPaint = [range, diameter](CanvasContext &ctx) {
    const GradeWheel &w = state.adjust.grade().wheel[range];
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    const float R = diameter / 2.0f;

    ctx.setGlobalAlpha(w.visible ? 1.0f : 0.3f);
    ctx.drawImage(gradeWheelImage(diameter, kGradeWheelBrightness[range]),
                  cx - R, cy - R);
    ctx.setGlobalAlpha(1.0f);

    const float ang = -w.hue * kGradePi / 180.0f;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float dist = w.sat / 100.0f * R;
    const float px = cx + ca * dist, py = cy + sa * dist;

    // spoke from the centre to the puck
    ctx.setStrokeColor(gradeHsv(w.hue, 0.30f, 1.0f, 200));
    ctx.setLineWidth(2.0f);
    ctx.beginPath();
    ctx.moveTo(cx, cy);
    ctx.lineTo(px, py);
    ctx.stroke();

    // hue dot outside the rim (grey while saturation is 0)
    const float k = std::clamp(w.sat / 100.0f * 3.0f, 0.0f, 1.0f);
    const Color full = gradeHsv(w.hue, 1.0f, 1.0f);
    const Color grey{70, 70, 70};
    auto lerp8 = [k](uint8_t a, uint8_t b) {
      return static_cast<uint8_t>(std::lround(a + (b - a) * k));
    };
    ctx.setFillColor(Color{lerp8(grey.r, full.r), lerp8(grey.g, full.g),
                           lerp8(grey.b, full.b)});
    ctx.beginPath();
    ctx.arc(cx + ca * (R + 11), cy + sa * (R + 11), 5.0f, 0, 2 * kGradePi);
    ctx.fill();

    // puck: dark ring with a faint light halo so it reads on any colour
    ctx.setStrokeColor(Color{255, 255, 255, 90});
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    ctx.arc(px, py, 9.0f, 0, 2 * kGradePi);
    ctx.stroke();
    ctx.setStrokeColor(Color{50, 50, 50});
    ctx.setLineWidth(2.0f);
    ctx.beginPath();
    ctx.arc(px, py, 7.5f, 0, 2 * kGradePi);
    ctx.stroke();
  };

  // Repaint only when this wheel changed (drag, undo/redo, reset, new image).
  auto last = std::make_shared<size_t>(~size_t{0});
  c.canvasDirtySource = [range, last]() -> bool {
    const GradeWheel &w = state.adjust.grade().wheel[range];
    size_t h = gradeHash(w.hue);
    h = gradeMix(h, gradeHash(w.sat));
    h = gradeMix(h, w.visible ? 1u : 0u);
    if (h == *last)
      return false;
    *last = h;
    return true;
  };
  return c;
}

// Eye icon: toggles that wheel's preview (one undo step).
static View buildGradeEye(int range) {
  View c;
  c.isCanvas = true;
  c.style.width = Size::pixel(22);
  c.style.height = Size::pixel(14);
  c.style.flexShrink = 0;
  c.onClick = [range] {
    if (!state.original)
      return;
    gradeEdit(
        [&](ColorGrade &g) {
          g.wheel[range].visible = !g.wheel[range].visible;
        },
        -1);
    state.markAdjustDirty();
  };
  c.onPaint = [range](CanvasContext &ctx) {
    const bool on = state.adjust.grade().wheel[range].visible;
    const float cx = ctx.width() / 2, cy = ctx.height() / 2;
    const Color col = on ? Color{190, 190, 190} : Color{118, 118, 118};
    ctx.setStrokeColor(col);
    ctx.setFillColor(col);
    ctx.setLineWidth(1.4f);
    ctx.beginPath();
    ctx.moveTo(cx - 8, cy);
    ctx.quadraticCurveTo(cx, cy - 9, cx + 8, cy);
    ctx.quadraticCurveTo(cx, cy + 9, cx - 8, cy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(cx, cy, 2.2f, 0, 2 * kGradePi);
    ctx.fill();
    if (!on) {
      ctx.beginPath();
      ctx.moveTo(cx - 8, cy + 6);
      ctx.lineTo(cx + 8, cy - 6);
      ctx.stroke();
    }
  };
  auto last = std::make_shared<int>(-1);
  c.canvasDirtySource = [range, last]() -> bool {
    const int now = state.adjust.grade().wheel[range].visible ? 1 : 0;
    if (now == *last)
      return false;
    *last = now;
    return true;
  };
  return c;
}

// wheel + eye row + luminance slider, as one column
static View buildGradeWheelBlock(int range, int diameter, float pad) {
  const float size = static_cast<float>(diameter) + 2.0f * pad;
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 2.0f;
  col.addChild(buildGradeWheel(range, diameter, pad));

  View eyeRow;
  eyeRow.style.direction = FlexDirection::Row;
  eyeRow.style.justifyContent = Justify::End;
  eyeRow.style.width = Size::pixel(static_cast<float>(diameter));
  eyeRow.addChild(buildGradeEye(range));
  col.addChild(std::move(eyeRow));

  col.addChild(buildGradeWheelSlider("Lum", range, &GradeWheel::lum, -100, 100,
                                     4, std::min(150.0f, size - 8.0f), true));
  return col;
}

// 3-way mode: big Midtones wheel on top, Shadows + Highlights below.
static View buildGradeThreeWay() {
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 6.0f;
  col.addChild(buildGradeWheelBlock(static_cast<int>(GradeRange::Midtones),
                                    kGradeMainWheel, 20.0f));

  View row;
  row.style.direction = FlexDirection::Row;
  row.style.justifyContent = Justify::Center;
  row.style.gap = 2.0f;
  auto labeled = [](int range) {
    View v;
    v.style.direction = FlexDirection::Column;
    v.style.alignItems = Align::Center;
    Text t;
    t.label = gradeRangeName(range);
    t.fontSize = 12.0f;
    t.color = kColorTextMuted;
    v.addChild(std::move(t));
    v.addChild(buildGradeWheelBlock(range, kGradeSmallWheel, 14.0f));
    return v;
  };
  row.addChild(labeled(static_cast<int>(GradeRange::Shadows)));
  row.addChild(labeled(static_cast<int>(GradeRange::Highlights)));
  col.addChild(std::move(row));
  return col;
}

// Single-range mode: one large wheel + Hue / Saturation / Luminance sliders.
static View buildGradeSingle(int range) {
  View col;
  col.style.direction = FlexDirection::Column;
  col.style.alignItems = Align::Center;
  col.style.gap = 8.0f;
  col.addChild(buildGradeWheel(range, kGradeSingleWheel, 22.0f));
  col.addChild(buildGradeWheelSlider("Hue", range, &GradeWheel::hue, 0, 359, 8,
                                     150.0f, false));
  col.addChild(buildGradeWheelSlider("Saturation", range, &GradeWheel::sat, 0,
                                     100, 12, 150.0f, false));
  col.addChild(buildGradeWheelSlider("Luminance", range, &GradeWheel::lum, -100,
                                     100, 4, 150.0f, true));
  return col;
}

// Resets every wheel, Blending and Balance (one undo step).
static void resetColorGrade() {
  state.adjust.setGrade(ColorGrade{}, -1);
  state.markAdjustDirty();
}

// Draws the crop selection over the image: dimmed surround, rule-of-thirds
// guides, border, and eight drag handles. Expects the CTM to already be
// scaled by state.zoom (so everything here is in image pixels), and divides
// line widths / handle sizes by the zoom to keep them constant on screen.
// BEGIN maskui
// ---------------- Masks: selection, editing, overlay ----------------
// The Masks pane (activity 7) edits `state.adjust.masks()`. Sliders go through
// buildSliderRow like everywhere else; the shape can also be dragged on the
// canvas (press inside = move, press a square handle = resize).
constexpr int kMaskCtlMove = 90;   // undo-coalescing control ids (see maskEdit)
constexpr int kMaskCtlResizeX = 91;
constexpr int kMaskCtlResizeY = 92;
constexpr int kMaskCtlLinEnd = 93;
constexpr int kMaskCtlLinStart = 94;
constexpr int kMaskCtlFeather = 1;
constexpr int kMaskCtlAngle = 2;
constexpr int kMaskCtlWidth = 3;
constexpr int kMaskCtlHeight = 4;
constexpr int kMaskCtlLength = 5;
constexpr int kMaskCtlAdj = 10; // + AdjId

static bool maskToolActive() {
  return state.activeActivity == 7 && state.image != nullptr;
}

// The selected mask's index, or -1. Clamped here instead of at every place
// that can shrink the list (undo, redo, open, delete).
static int selMask() {
  const int n = static_cast<int>(state.adjust.masks().size());
  if (state.maskSel >= n)
    state.maskSel = n - 1;
  if (state.maskSel < -1)
    state.maskSel = -1;
  return state.maskSel;
}

static const Mask *selectedMask() {
  const int i = selMask();
  return i < 0 ? nullptr : &state.adjust.masks()[static_cast<size_t>(i)];
}

// The mask's radial shape as it sits in the picture being shown (see the
// "masks" block: masks are STORED against the original picture). Everything
// on the canvas — outlines, hit tests, drags — works on this form.
template <class Shape>
static std::optional<Shape> shapeInView(const Mask &m, const GeoFrame &f) {
  if (const auto *r = std::get_if<Shape>(&m.shape))
    return r->toSource(f);
  return std::nullopt;
}
static std::optional<RadialShape> radialInView(const Mask &m,
                                               const GeoFrame &f) {
  return shapeInView<RadialShape>(m, f);
}
static std::optional<LinearShape> linearInView(const Mask &m,
                                               const GeoFrame &f) {
  return shapeInView<LinearShape>(m, f);
}

// Edits a mask's radial shape on its in-view form and stores the result back
// (only the fields `fn` changed; see RadialShape::applySourceEdit).
template <class Shape>
static void editShapeInView(Mask &m, const GeoFrame &f,
                            const std::function<void(Shape &)> &fn) {
  auto *stored = std::get_if<Shape>(&m.shape);
  if (!stored)
    return;
  const Shape before = stored->toSource(f);
  Shape after = before;
  fn(after);
  stored->applySourceEdit(before, after, f);
}
static void editRadialInView(Mask &m, const GeoFrame &f,
                             const std::function<void(RadialShape &)> &fn) {
  editShapeInView<RadialShape>(m, f, fn);
}
static void editLinearInView(Mask &m, const GeoFrame &f,
                             const std::function<void(LinearShape &)> &fn) {
  editShapeInView<LinearShape>(m, f, fn);
}

// Edits mask `idx` in a copy of the list and commits it. `control` >= 0 names
// the control being dragged: repeated edits of the same control of the same
// mask fold into one undo step (see AdjustmentSet::setMasks). -1 = discrete.
// Does NOT recompute the picture — slider / canvas drags do that through the
// usual markAdjustDirty path; discrete actions call maskCommitDiscrete.
static void maskEdit(int idx, const std::function<void(Mask &)> &fn,
                     int control) {
  MaskSet ms = state.adjust.masks();
  if (idx < 0 || idx >= static_cast<int>(ms.size()))
    return;
  fn(ms[static_cast<size_t>(idx)]);
  state.adjust.setMasks(ms, control < 0 ? -1 : idx * 100 + control);
}

// A one-off change to the selected mask (toggle, reset): its own undo step and
// an immediate recompute.
static void maskEditDiscrete(const std::function<void(Mask &)> &fn) {
  const int i = selMask();
  if (i < 0)
    return;
  maskEdit(i, fn, -1);
  updateImageFromAdjustments();
  state.markDirty();
}

static void addMask(MaskType t) {
  if (!state.original)
    return;
  MaskSet ms = state.adjust.masks();
  if (ms.size() >= kMaxMasks) {
    setStatus("Mask limit reached (" + std::to_string(kMaxMasks) + ")", true);
    return;
  }
  Mask m;
  switch (t) {
  case MaskType::Radial:
    // The default shape, centered on the picture as it is shown now.
    m.shape = RadialShape::fromSource(RadialShape{}, state.adjust.geoFrame());
    break;
  case MaskType::Linear:
    m.shape = LinearShape::fromSource(LinearShape{}, state.adjust.geoFrame());
    break;
  default:
    return;
  }
  ms.push_back(std::move(m));
  state.adjust.setMasks(ms, -1);
  state.maskSel = static_cast<int>(ms.size()) - 1;
  // A new mask has no sliders moved yet, so no pixel changes: overlay only.
  state.markDirty();
}

static void deleteSelectedMask() {
  const int i = selMask();
  if (i < 0)
    return;
  MaskSet ms = state.adjust.masks();
  ms.erase(ms.begin() + i);
  state.adjust.setMasks(ms, -1);
  state.maskSel = std::min(i, static_cast<int>(ms.size()) - 1);
  updateImageFromAdjustments();
  state.markDirty();
}

// A drag on the canvas or a mask slider ended: leave the low-res preview and
// ask for one full-resolution render (same job as gradeEndDrag).
static void maskEndDrag() {
  state.mask.drag = MaskUi::Drag::None;
  state.adjust.endMaskEdit();
  if (state.dragging) {
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

// ---- Radial mask geometry on the canvas (document pixels) ----
struct MaskGeom {
  float cx, cy;   // center
  float rx, ry;   // half-axes
  float ang;      // radians, clockwise
  float c, s;     // cos, sin of ang
  float S;        // the shorter document side (radius unit)
};

static MaskGeom radialGeom(const RadialShape &r) {
  MaskGeom g{};
  const float W = state.docWidth(), H = state.docHeight();
  g.S = std::min(W, H);
  g.cx = r.cx * W;
  g.cy = r.cy * H;
  g.rx = r.rx * g.S;
  g.ry = r.ry * g.S;
  g.ang = r.angle * (3.14159265358979f / 180.0f);
  g.c = std::cos(g.ang);
  g.s = std::sin(g.ang);
  return g;
}

enum {
  kMaskHitNone = 0,
  kMaskHitBody,
  kMaskHitX,
  kMaskHitY,
  kMaskHitLinEnd,
  kMaskHitLinStart
};

// What is under (px, py)? Handles (only for the selected mask) win over the
// body. The four handles sit on the rotated axes at the ellipse outline.
static int maskHit(const RadialShape &r, float px, float py, bool handles) {
  const MaskGeom g = radialGeom(r);
  const float tol = kMaskHitPx / state.zoom;
  if (handles) {
    const float hx[2] = {g.cx + g.rx * g.c, g.cx - g.rx * g.c};
    const float hy[2] = {g.cy + g.rx * g.s, g.cy - g.rx * g.s};
    const float vx[2] = {g.cx - g.ry * g.s, g.cx + g.ry * g.s};
    const float vy[2] = {g.cy + g.ry * g.c, g.cy - g.ry * g.c};
    for (int k = 0; k < 2; ++k) {
      if (std::hypot(px - hx[k], py - hy[k]) <= tol)
        return kMaskHitX;
      if (std::hypot(px - vx[k], py - vy[k]) <= tol)
        return kMaskHitY;
    }
  }
  const float dx = px - g.cx, dy = py - g.cy;
  const float u = (dx * g.c + dy * g.s) / g.rx;
  const float v = (-dx * g.s + dy * g.c) / g.ry;
  return (u * u + v * v <= 1.0f) ? kMaskHitBody : kMaskHitNone;
}

// ---- Linear mask geometry on the canvas (document pixels) ----
struct LinGeom {
  float cx, cy;   // center
  float ux, uy;   // unit direction of fade-out
  float len;      // half length of the ramp
  float ang;      // radians
  float S;        // the shorter document side
  // The weight-1 line's and the weight-0 line's axis points.
  float sx() const { return cx - ux * len; }
  float sy() const { return cy - uy * len; }
  float ex() const { return cx + ux * len; }
  float ey() const { return cy + uy * len; }
};

static LinGeom linearGeom(const LinearShape &l) {
  LinGeom g{};
  const float W = state.docWidth(), H = state.docHeight();
  g.S = std::min(W, H);
  g.cx = l.cx * W;
  g.cy = l.cy * H;
  g.len = std::max(l.len, kMaskMinRadius) * g.S;
  g.ang = l.angle * (3.14159265358979f / 180.0f);
  g.ux = std::cos(g.ang);
  g.uy = std::sin(g.ang);
  return g;
}

// Handles (selected mask only) sit where the axis crosses the two lines; the
// body is the band between the lines.
static int linearHit(const LinearShape &l, float px, float py, bool handles) {
  const LinGeom g = linearGeom(l);
  const float tol = kMaskHitPx / state.zoom;
  if (handles) {
    if (std::hypot(px - g.ex(), py - g.ey()) <= tol)
      return kMaskHitLinEnd;
    if (std::hypot(px - g.sx(), py - g.sy()) <= tol)
      return kMaskHitLinStart;
  }
  const float s = (px - g.cx) * g.ux + (py - g.cy) * g.uy;
  return std::fabs(s) <= g.len ? kMaskHitBody : kMaskHitNone;
}

static void maskPress(float lx, float ly) {
  if (!maskToolActive())
    return;
  state.mask.drag = MaskUi::Drag::None;
  const MaskSet &ms = state.adjust.masks();
  const GeoFrame frame = state.adjust.geoFrame();
  const float px = lx / state.zoom, py = ly / state.zoom;
  const int sel = selMask();
  // Tries mask `i`; on a hit selects it and starts the matching drag.
  auto tryMask = [&](int i, bool isSelected) -> bool {
    const Mask &m = ms[static_cast<size_t>(i)];
    int hit = kMaskHitNone;
    float shapeCx = 0, shapeCy = 0; // center, fraction of the picture
    if (const auto rv = radialInView(m, frame)) {
      hit = maskHit(*rv, px, py, isSelected);
      shapeCx = rv->cx;
      shapeCy = rv->cy;
    } else if (const auto lv = linearInView(m, frame)) {
      hit = linearHit(*lv, px, py, isSelected);
      shapeCx = lv->cx;
      shapeCy = lv->cy;
    }
    if (hit == kMaskHitNone)
      return false;
    state.maskSel = i;
    if (hit == kMaskHitX) {
      state.mask.drag = MaskUi::Drag::ResizeX;
    } else if (hit == kMaskHitY) {
      state.mask.drag = MaskUi::Drag::ResizeY;
    } else if (hit == kMaskHitLinEnd) {
      state.mask.drag = MaskUi::Drag::LinEnd;
    } else if (hit == kMaskHitLinStart) {
      state.mask.drag = MaskUi::Drag::LinStart;
    } else {
      state.mask.drag = MaskUi::Drag::Move;
      state.mask.grabX = px - shapeCx * state.docWidth();
      state.mask.grabY = py - shapeCy * state.docHeight();
    }
    state.markDirty();
    return true;
  };
  if (sel >= 0 && tryMask(sel, true))
    return;
  // Otherwise clicking inside another mask selects it (topmost first).
  for (int i = static_cast<int>(ms.size()) - 1; i >= 0; --i)
    if (i != sel && tryMask(i, false))
      return;
}

static void maskDrag(float lx, float ly) {
  if (!maskToolActive() || state.mask.drag == MaskUi::Drag::None)
    return;
  const int i = selMask();
  if (i < 0)
    return;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = lx / state.zoom, py = ly / state.zoom;
  const MaskUi::Drag d = state.mask.drag;
  const float grabX = state.mask.grabX, grabY = state.mask.grabY;
  const GeoFrame frame = state.adjust.geoFrame();
  maskEdit(
      i,
      [&](Mask &m) {
        editRadialInView(m, frame, [&](RadialShape &r) {
          if (d == MaskUi::Drag::Move) {
            r.cx = std::clamp((px - grabX) / W, 0.0f, 1.0f);
            r.cy = std::clamp((py - grabY) / H, 0.0f, 1.0f);
          } else {
            // Distance of the pointer from the center along the rotated axis.
            const MaskGeom g = radialGeom(r);
            const float dx = px - g.cx, dy = py - g.cy;
            if (d == MaskUi::Drag::ResizeX)
              r.rx = std::clamp(std::fabs(dx * g.c + dy * g.s) / g.S,
                                kMaskMinRadius, kMaskMaxRadius);
            else
              r.ry = std::clamp(std::fabs(-dx * g.s + dy * g.c) / g.S,
                                kMaskMinRadius, kMaskMaxRadius);
          }
        });
        editLinearInView(m, frame, [&](LinearShape &l) {
          if (d == MaskUi::Drag::Move) {
            l.cx = std::clamp((px - grabX) / W, 0.0f, 1.0f);
            l.cy = std::clamp((py - grabY) / H, 0.0f, 1.0f);
          } else {
            // Dragging a line's handle sets the half length AND turns the
            // axis so the handle stays under the pointer (the start handle
            // is on the opposite side of the center, so its pull is negated).
            const LinGeom g = linearGeom(l);
            const float sign = d == MaskUi::Drag::LinEnd ? 1.0f : -1.0f;
            const float dx = (px - g.cx) * sign, dy = (py - g.cy) * sign;
            const float dist = std::hypot(dx, dy);
            l.len = std::clamp(dist / g.S, kMaskMinRadius, kMaskMaxRadius);
            if (dist > 1e-3f)
              l.angle = std::atan2(dy, dx) * (180.0f / 3.14159265358979f);
          }
        });
      },
      d == MaskUi::Drag::Move       ? kMaskCtlMove
      : d == MaskUi::Drag::ResizeX  ? kMaskCtlResizeX
      : d == MaskUi::Drag::ResizeY  ? kMaskCtlResizeY
      : d == MaskUi::Drag::LinEnd   ? kMaskCtlLinEnd
                                    : kMaskCtlLinStart);
  enterDrag(false); // recompute on the cheap preview path while held
  state.markAdjustDirty();
  state.markDirty(); // the overlay follows the pointer
}

// Red tint showing the mask's weight (1 = solid, 0 = clear) over the picture.
static void paintRadialTint(CanvasContext &ctx, const RadialShape &r,
                            bool invert) {
  const MaskGeom g = radialGeom(r);
  const float W = state.docWidth(), H = state.docHeight();
  const float inner = r.innerFrac();
  const Color on{255, 40, 40, 120}, off{255, 40, 40, 0};
  ctx.save();
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.clip();
  // Unit-circle gradient under the ellipse's own transform.
  ctx.translate(g.cx, g.cy);
  ctx.rotate(g.ang);
  ctx.scale(g.rx, g.ry);
  CanvasGradient grad = CanvasGradient::radial(0, 0, 0, 0, 0, 1);
  grad.addColorStop(0.0f, invert ? off : on);
  grad.addColorStop(inner, invert ? off : on);
  grad.addColorStop(1.0f, invert ? on : off);
  ctx.setFillGradient(grad);
  const float R = (W + H) / std::min(g.rx, g.ry) + 2.0f; // covers the document
  ctx.fillRect(-R, -R, 2.0f * R, 2.0f * R);
  ctx.restore();
}

// Outline of one radial mask; the selected one also gets its feather ring
// and handles.
static void paintRadialOutline(CanvasContext &ctx, const RadialShape &r,
                               bool selected) {
  const MaskGeom g = radialGeom(r);
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  constexpr float kTwoPi = 6.28318530717959f;

  ctx.beginPath();
  ctx.ellipse(g.cx, g.cy, g.rx, g.ry, g.ang, 0.0f, kTwoPi);
  ctx.setStrokeColor(Color{0, 0, 0, selected ? uint8_t{150} : uint8_t{90}});
  ctx.setLineWidth(px * 3.0f);
  ctx.stroke();
  ctx.setStrokeColor(
      Color{255, 255, 255, selected ? uint8_t{255} : uint8_t{140}});
  ctx.setLineWidth(px * (selected ? 1.5f : 1.0f));
  ctx.stroke();
  if (!selected)
    return;

  // Where the feather begins (skipped when it would sit on the outline or
  // collapse into the center).
  const float inner = r.innerFrac();
  if (r.feather > 0.02f && inner > 0.02f) {
    ctx.beginPath();
    ctx.ellipse(g.cx, g.cy, g.rx * inner, g.ry * inner, g.ang, 0.0f, kTwoPi);
    ctx.setLineDash({4.0f * px, 4.0f * px});
    ctx.setStrokeColor(Color{255, 255, 255, 170});
    ctx.setLineWidth(px);
    ctx.stroke();
    ctx.setLineDash({});
  }

  // Four square handles on the axes, and a ring at the center.
  const float hs = kMaskHandlePx * px;
  const float hx[4] = {g.cx + g.rx * g.c, g.cx - g.rx * g.c, g.cx - g.ry * g.s,
                       g.cx + g.ry * g.s};
  const float hy[4] = {g.cy + g.rx * g.s, g.cy - g.rx * g.s, g.cy + g.ry * g.c,
                       g.cy - g.ry * g.c};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int k = 0; k < 4; ++k) {
    ctx.fillRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
    ctx.strokeRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
  }
  ctx.beginPath();
  ctx.arc(g.cx, g.cy, hs * 0.55f, 0.0f, kTwoPi);
  ctx.setFillColor(Color{255, 255, 255, 60});
  ctx.fill();
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
}

// Red tint of a linear mask: a gradient along the axis from the weight-1 line
// to the weight-0 line, with stops sampled from the same smoothstep the
// renderer uses (the gradient itself is piecewise linear).
static void paintLinearTint(CanvasContext &ctx, const LinearShape &l,
                            bool invert) {
  const LinGeom g = linearGeom(l);
  const float W = state.docWidth(), H = state.docHeight();
  const auto alphaAt = [invert](float t) -> Color {
    float w = 1.0f - t * t * (3.0f - 2.0f * t);
    if (invert)
      w = 1.0f - w;
    return Color{255, 40, 40, static_cast<uint8_t>(std::lround(120.0f * w))};
  };
  CanvasGradient grad = CanvasGradient::linear(g.sx(), g.sy(), g.ex(), g.ey());
  constexpr int kStops = 8;
  for (int k = 0; k <= kStops; ++k) {
    const float t = static_cast<float>(k) / kStops;
    grad.addColorStop(t, alphaAt(t));
  }
  ctx.save();
  ctx.setFillGradient(grad);
  ctx.fillRect(0, 0, W, H); // the gradient pads beyond both lines
  ctx.restore();
}

// Outline of one linear mask: the two lines of the ramp; the selected one
// also gets the center line, the axis with an arrowhead, and its handles.
static void paintLinearOutline(CanvasContext &ctx, const LinearShape &l,
                               bool selected) {
  const LinGeom g = linearGeom(l);
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  constexpr float kTwoPi = 6.28318530717959f;
  const float W = state.docWidth(), H = state.docHeight();
  const float R = W + H;      // long enough to cross the document
  const float vx = -g.uy, vy = g.ux; // along the lines
  const auto line = [&](float ax, float ay) {
    ctx.beginPath();
    ctx.moveTo(ax - vx * R, ay - vy * R);
    ctx.lineTo(ax + vx * R, ay + vy * R);
  };
  const auto strokeTwice = [&](float wOuter, float wInner, uint8_t a1,
                               uint8_t a2) {
    ctx.setStrokeColor(Color{0, 0, 0, a1});
    ctx.setLineWidth(px * wOuter);
    ctx.stroke();
    ctx.setStrokeColor(Color{255, 255, 255, a2});
    ctx.setLineWidth(px * wInner);
    ctx.stroke();
  };

  // The lines are clipped to the picture; handles are drawn outside the clip.
  ctx.save();
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.clip();
  const uint8_t a1 = selected ? uint8_t{150} : uint8_t{90};
  const uint8_t a2 = selected ? uint8_t{255} : uint8_t{140};
  line(g.sx(), g.sy());
  strokeTwice(3.0f, selected ? 1.5f : 1.0f, a1, a2);
  line(g.ex(), g.ey());
  strokeTwice(3.0f, selected ? 1.5f : 1.0f, a1, a2);
  if (selected) {
    line(g.cx, g.cy); // the 50% line
    ctx.setLineDash({4.0f * px, 4.0f * px});
    ctx.setStrokeColor(Color{255, 255, 255, 170});
    ctx.setLineWidth(px);
    ctx.stroke();
    ctx.setLineDash({});
  }
  ctx.restore();
  if (!selected)
    return;

  // The axis between the handles, with an arrowhead showing the fade-out.
  const float hs = kMaskHandlePx * px;
  ctx.beginPath();
  ctx.moveTo(g.sx(), g.sy());
  ctx.lineTo(g.ex(), g.ey());
  ctx.setStrokeColor(Color{255, 255, 255, 200});
  ctx.setLineWidth(px);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(g.ex() + g.ux * hs * 1.6f, g.ey() + g.uy * hs * 1.6f);
  ctx.lineTo(g.ex() + g.ux * hs * 0.7f + vx * hs * 0.7f,
             g.ey() + g.uy * hs * 0.7f + vy * hs * 0.7f);
  ctx.lineTo(g.ex() + g.ux * hs * 0.7f - vx * hs * 0.7f,
             g.ey() + g.uy * hs * 0.7f - vy * hs * 0.7f);
  ctx.closePath();
  ctx.setFillColor(kColorWhite);
  ctx.fill();

  // Square handles on the two lines, and a ring at the center.
  const float hx[2] = {g.sx(), g.ex()}, hy[2] = {g.sy(), g.ey()};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int k = 0; k < 2; ++k) {
    ctx.fillRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
    ctx.strokeRect(hx[k] - hs / 2.0f, hy[k] - hs / 2.0f, hs, hs);
  }
  ctx.beginPath();
  ctx.arc(g.cx, g.cy, hs * 0.55f, 0.0f, kTwoPi);
  ctx.setFillColor(Color{255, 255, 255, 60});
  ctx.fill();
  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.stroke();
}

static void paintMaskOverlay(CanvasContext &ctx) {
  const MaskSet &ms = state.adjust.masks();
  const int sel = selMask();
  const GeoFrame frame = state.adjust.geoFrame();
  // Unselected masks first (faint), the selected one on top.
  for (int i = 0; i < static_cast<int>(ms.size()); ++i) {
    if (i == sel)
      continue;
    const Mask &um = ms[static_cast<size_t>(i)];
    if (const auto r = radialInView(um, frame))
      paintRadialOutline(ctx, *r, false);
    else if (const auto l = linearInView(um, frame))
      paintLinearOutline(ctx, *l, false);
  }
  if (sel < 0)
    return;
  const Mask &m = ms[static_cast<size_t>(sel)];
  if (const auto r = radialInView(m, frame)) {
    if (state.maskShowTint)
      paintRadialTint(ctx, *r, m.invert);
    paintRadialOutline(ctx, *r, true);
  } else if (const auto l = linearInView(m, frame)) {
    if (state.maskShowTint)
      paintLinearTint(ctx, *l, m.invert);
    paintLinearOutline(ctx, *l, true);
  }
}

// ---- Masks pane widgets ----

// One row of the mask list: "1  Radial", highlighted when selected. There are
// kMaxMasks of these, shown only for the masks that exist (the view tree is
// built once, so the list is a fixed set of slots).
static View buildMaskRow(size_t i) {
  View b;
  b.style.width = Size::full();
  b.style.height = Size::pixel(28);
  b.style.flexShrink = 0;
  b.style.borderRadius = 4.0f;
  b.style.alignItems = Align::Center;
  b.style.padding = EdgeInsets{0, 10, 0, 10};
  b.style.hoverColor = kColorActivityHover;
  b.style.display = std::function<Display()>([i] {
    return i < state.adjust.masks().size() ? Display::Flex : Display::None;
  });
  const auto isSel = [i] { return selMask() == static_cast<int>(i); };
  b.style.backgroundColor = std::function<Color()>([isSel]() -> Color {
    return isSel() ? kColorDropdownRowHover : kColorActivityBarBg;
  });
  b.style.borderWidth =
      std::function<float()>([isSel] { return isSel() ? 1.0f : 0.0f; });
  b.style.borderColor = kColorDividerHover;
  b.onClick = [i] {
    state.maskSel = static_cast<int>(i);
    state.markDirty(); // move the overlay to it
  };

  Text t;
  t.label = std::function<std::string()>([i]() -> std::string {
    const MaskSet &ms = state.adjust.masks();
    if (i >= ms.size())
      return std::string();
    const Mask &m = ms[i];
    std::string s = std::to_string(i + 1) + "   " + maskTypeName(m.type());
    if (m.invert)
      s += "  (inverted)";
    if (!m.enabled)
      s += "  (off)";
    return s;
  });
  t.fontSize = 13.0f;
  t.color = kColorTextDark;
  b.addChild(std::move(t));
  return b;
}

// A shape slider for the selected radial mask. `get` / `set` see the shape
// itself; `control` names the undo-coalescing control.
template <class Shape>
static SliderSpec maskShapeSlider(const char *name, float lo, float hi,
                                  std::function<float(const Shape &)> get,
                                  std::function<void(Shape &, float)> set,
                                  int control) {
  SliderSpec s;
  s.name = name;
  s.lo = lo;
  s.hi = hi;
  // The sliders show and edit the shape as it sits in the picture on screen.
  s.get = [get] {
    const Mask *m = selectedMask();
    const auto r =
        m ? shapeInView<Shape>(*m, state.adjust.geoFrame()) : std::nullopt;
    return r ? get(*r) : 0.0f;
  };
  s.set = [set, control](float v) {
    const GeoFrame frame = state.adjust.geoFrame();
    maskEdit(selMask(),
             [&](Mask &m) {
               editShapeInView<Shape>(m, frame, [&](Shape &r) { set(r, v); });
             },
             control);
  };
  s.format = [](float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.onEnd = [] { state.adjust.endMaskEdit(); };
  return s;
}
static SliderSpec
maskRadialSlider(const char *name, float lo, float hi,
                 std::function<float(const RadialShape &)> get,
                 std::function<void(RadialShape &, float)> set, int control) {
  return maskShapeSlider<RadialShape>(name, lo, hi, std::move(get),
                                      std::move(set), control);
}
static SliderSpec
maskLinearSlider(const char *name, float lo, float hi,
                 std::function<float(const LinearShape &)> get,
                 std::function<void(LinearShape &, float)> set, int control) {
  return maskShapeSlider<LinearShape>(name, lo, hi, std::move(get),
                                      std::move(set), control);
}

// The mask's own copy of one Adjust slider (Exposure, Contrast, ...): one per
// kAdjustmentSpecs entry, so a new global adjustment appears here too.
static View buildMaskAdjustmentSlider(const AdjustmentSpec &spec) {
  const AdjId id = spec.id;
  SliderSpec s;
  s.name = spec.label;
  s.lo = spec.min;
  s.hi = spec.max;
  s.get = [id] {
    const Mask *m = selectedMask();
    return m ? m->adj[static_cast<size_t>(id)] : 0.0f;
  };
  s.set = [id](float v) {
    maskEdit(selMask(),
             [&](Mask &m) { m.adj[static_cast<size_t>(id)] = std::round(v); },
             kMaskCtlAdj + static_cast<int>(id));
  };
  s.format = [](float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  s.onEnd = [] { state.adjust.endMaskEdit(); };
  return buildSliderRow(s);
}
// END maskui

static void paintCropOverlay(CanvasContext &ctx) {
  const auto &c = state.crop;
  const float W = state.docWidth(), H = state.docHeight();
  const float px = 1.0f / state.zoom; // one screen pixel, in image units
  const float sw = c.r - c.l, sh = c.b - c.t;

  ctx.setFillColor(Color{0, 0, 0, 120});
  ctx.fillRect(0, 0, W, c.t);          // above
  ctx.fillRect(0, c.b, W, H - c.b);    // below
  ctx.fillRect(0, c.t, c.l, sh);       // left
  ctx.fillRect(c.r, c.t, W - c.r, sh); // right

  ctx.setLineWidth(px);
  ctx.setStrokeColor(Color{255, 255, 255, 110});
  ctx.beginPath();
  for (int i = 1; i <= 2; ++i) {
    const float x = c.l + sw * static_cast<float>(i) / 3.0f;
    const float y = c.t + sh * static_cast<float>(i) / 3.0f;
    ctx.moveTo(x, c.t);
    ctx.lineTo(x, c.b);
    ctx.moveTo(c.l, y);
    ctx.lineTo(c.r, y);
  }
  ctx.stroke();

  ctx.setStrokeColor(kColorWhite);
  ctx.setLineWidth(px * 1.5f);
  ctx.strokeRect(c.l, c.t, sw, sh);

  const float hs = kCropHandlePx * px;
  const bool midX = sw > hs * 4.0f, midY = sh > hs * 4.0f;
  const float xs[3] = {c.l, (c.l + c.r) / 2.0f, c.r};
  const float ys[3] = {c.t, (c.t + c.b) / 2.0f, c.b};
  ctx.setFillColor(kColorWhite);
  ctx.setStrokeColor(Color{60, 60, 60, 255});
  ctx.setLineWidth(px);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      if (i == 1 && j == 1)
        continue; // center: no handle
      if ((i == 1 && !midX) || (j == 1 && !midY))
        continue; // edge midpoints crowd together on tiny selections
      ctx.fillRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
      ctx.strokeRect(xs[i] - hs / 2.0f, ys[j] - hs / 2.0f, hs, hs);
    }
}

static void paintCanvas(CanvasContext &ctx) {
  LITEUI_PROF_START(profAppPaint);
  ctx.setFillColor(state.image ? kColorWhite : kColorCanvasPlaceholderBg);
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

  if (!state.image || state.image->width <= 0 || state.image->height <= 0)
    return;

  // Drawn at the document's natural pixel size; the canvas itself is
  // already sized to docWidth*zoom / docHeight*zoom (see canvas.style
  // below), so a plain ctx.scale(zoom, zoom) here is all that's needed.
  // A straightened image with "Keep full canvas" has transparent corners;
  // a neutral backdrop keeps them from reading as part of a white picture.
  if (state.adjust.expand() && state.adjust.angle() != 0.0f) {
    ctx.setFillColor(kColorEmptyCorner);
    ctx.fillRect(0, 0, ctx.width(), ctx.height());
  }

  ctx.save();
  ctx.scale(state.zoom, state.zoom);
  if (state.showPreview()) {
    // Mid-drag (or awaiting the background full-res render) preview: smaller
    // image stretched to the full document size
    // (bilinear), so layout/zoom are unaffected by its lower resolution.
    ctx.drawImage(*state.previewImage, 0, 0, state.docWidth(),
                  state.docHeight());
  } else {
    ctx.drawImage(*state.image, 0, 0);
  }
  if (cropToolActive())
    paintCropOverlay(ctx);
  else if (maskToolActive())
    paintMaskOverlay(ctx);
  ctx.restore();
  LITEUI_PROF_ADD(appPaint, profAppPaint);
}

// Runs the deferred, expensive part of a slider drag (rebuilding the shown
// image) exactly once per pending change. Idempotent: the main canvas and the
// histogram canvas both call it from their dirty polls, and whichever is
// polled first (the sidebar, so usually the histogram) does the work — so the
// histogram never lags a frame behind the picture.
static void flushPendingAdjust() {
  collectFullRender();  // a finished background render, if any
  collectGeometryJob(); // a finished background geometry rebuild, if any
  collectDecodeJob();   // a finished background file decode, if any
  if (!state.adjustDirty)
    return;
  state.adjustDirty = false;
  LITEUI_PROF_START(profRecompute);
  if (state.dragging && state.dragIsAngle && state.orientedPreview)
    updateAnglePreview(); // cheap: turns the small preview, mid-drag
  else if (state.dragging && !state.dragIsAngle && state.previewBase)
    updatePreviewFromAdjustments(); // cheap: small image, mid-drag
  else
    updateImageFromAdjustments(); // full-res: rebuilds state.image
  LITEUI_PROF_ADD(recompute, profRecompute);
}

// Brings the shown image (and, if stale, the histogram) up to date. Several
// canvases poll this; only the first poll of a frame does any work.
static void refreshHistogramIfStale();

// Rebuilds the R/G/B/luma histograms from the image currently on screen,
// visiting at most ~kHistMaxSamples pixels on a regular grid. Fully
// transparent pixels (straighten's empty corners) are skipped.
static void computeHistogram() {
  state.histR.fill(0);
  state.histG.fill(0);
  state.histB.fill(0);
  state.histL.fill(0);
  const CanvasImage *img =
      state.showPreview() ? state.previewImage.get() : state.image.get();
  if (img && img->width > 0 && img->height > 0) {
    const double total = static_cast<double>(img->width) * img->height;
    const int step =
        std::max(1, static_cast<int>(std::sqrt(total / kHistMaxSamples)));
    const uint8_t *p = img->pixels.data();
    for (int y = 0; y < img->height; y += step) {
      const uint8_t *row = p + static_cast<size_t>(y) * img->width * 4;
      for (int x = 0; x < img->width; x += step) {
        const uint8_t *px = row + static_cast<size_t>(x) * 4;
        if (px[3] == 0)
          continue;
        ++state.histR[px[0]];
        ++state.histG[px[1]];
        ++state.histB[px[2]];
        // Rec. 601 luma, integer math.
        ++state.histL[(px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8];
      }
    }
  }
  state.histStale = false;
  ++state.histVersion;
}

// Luma as gray bars, R/G/B as thin colored outlines on top. Bar heights use
// sqrt scaling and are normalized to the tallest bin among 1..254, so a pile
// of clipped pixels in bin 0/255 doesn't flatten the rest of the graph.
static void paintHistogram(CanvasContext &ctx) {
  const float W = ctx.width(), H = ctx.height();
  ctx.setFillColor(Color{34, 34, 38, 255});
  ctx.fillRect(0, 0, W, H);

  uint32_t peak = 1;
  for (int i = 1; i < 255; ++i)
    peak = std::max(
        {peak, state.histL[i], state.histR[i], state.histG[i], state.histB[i]});
  const float invPeak = 1.0f / std::sqrt(static_cast<float>(peak));
  const float binW = W / 256.0f;
  auto heightOf = [&](uint32_t v) {
    return std::min(1.0f, std::sqrt(static_cast<float>(v)) * invPeak) *
           (H - 2.0f);
  };

  ctx.setFillColor(Color{170, 170, 175, 255});
  for (int i = 0; i < 256; ++i) {
    const float h = heightOf(state.histL[i]);
    if (h > 0.0f)
      ctx.fillRect(static_cast<float>(i) * binW, H - h, binW + 0.5f, h);
  }

  auto outline = [&](const std::array<uint32_t, 256> &bins, Color c) {
    ctx.setStrokeColor(c);
    ctx.setLineWidth(1.0f);
    ctx.beginPath();
    for (int i = 0; i < 256; ++i) {
      const float x = (static_cast<float>(i) + 0.5f) * binW;
      const float y = H - heightOf(bins[i]);
      if (i == 0)
        ctx.moveTo(x, y);
      else
        ctx.lineTo(x, y);
    }
    ctx.stroke();
  };
  outline(state.histR, Color{235, 70, 70, 200});
  outline(state.histG, Color{70, 200, 90, 200});
  outline(state.histB, Color{80, 130, 255, 200});
}

static void refreshHistogramIfStale() {
  flushPendingAdjust();
  if (state.histStale)
    computeHistogram();
}

// ---------------- Tone curve editor (canvas side) ----------------
// Mapping between canvas-local screen px and curve units (0..255, y up).
static float curvePlotPx() { return kCurveSizePx - 2.0f * kCurvePadPx; }

static CurvePoint curveFromLocal(float lx, float ly) {
  const float plot = curvePlotPx();
  return {
      std::clamp((lx - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f),
      std::clamp(255.0f - (ly - kCurvePadPx) / plot * 255.0f, 0.0f, 255.0f)};
}

static void curveToLocal(float x, float y, float &lx, float &ly) {
  const float plot = curvePlotPx();
  lx = kCurvePadPx + x / 255.0f * plot;
  ly = kCurvePadPx + (255.0f - y) / 255.0f * plot;
}

// Index of the point within kCurveHitPx of (lx, ly), nearest wins; -1 if none.
static int curveHit(const CurvePoints &pts, float lx, float ly) {
  int hit = -1;
  float best = kCurveHitPx * kCurveHitPx;
  for (size_t i = 0; i < pts.size(); ++i) {
    float px, py;
    curveToLocal(pts[i].x, pts[i].y, px, py);
    const float d2 = (px - lx) * (px - lx) + (py - ly) * (py - ly);
    if (d2 <= best) {
      best = d2;
      hit = static_cast<int>(i);
    }
  }
  return hit;
}

static int gCurveActive = -1; // point being dragged, or -1
static CurveChannel gCurveChannel = CurveChannel::Master; // curve being edited
// Set by UI-only changes (channel switch, selecting a point) that don't
// change the picture, so the curve canvas still repaints; consumed by its
// dirty poll.
static bool gCurveUiDirty = false;

static const CurvePoints &curveActiveCurve() {
  return state.adjust.curve(gCurveChannel);
}
// (point - mouse) in curve units at press, so a grabbed point doesn't jump
// to the cursor's exact position.
static float gCurveGrabDx = 0.0f, gCurveGrabDy = 0.0f;

static void curvePress(float lx, float ly) {
  if (!state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  int hit = curveHit(pts, lx, ly);
  gCurveGrabDx = gCurveGrabDy = 0.0f;
  if (hit >= 0) {
    const CurvePoint m = curveFromLocal(lx, ly);
    gCurveGrabDx = pts[hit].x - m.x;
    gCurveGrabDy = pts[hit].y - m.y;
  } else {
    if (pts.size() >= kCurveMaxPoints)
      return;
    const CurvePoint m = curveFromLocal(lx, ly);
    hit = curveInsert(pts, m.x, m.y);
    if (hit < 0)
      return;
    // coalesce = true: the drag that follows folds into this same undo step.
    state.adjust.setCurve(gCurveChannel, pts, true);
    // One full-resolution recompute for the click itself; the preview path
    // only starts once the point actually moves (see curveDrag), so a plain
    // click can never leave the view stuck on the low-res preview.
    state.markAdjustDirty();
  }
  gCurveActive = hit;
  gCurveUiDirty = true; // show the selected point
}

static void curveDrag(float lx, float ly) {
  if (gCurveActive < 0 || !state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  if (gCurveActive >= static_cast<int>(pts.size()))
    return;
  const CurvePoint m = curveFromLocal(lx, ly);
  curveMovePoint(pts, gCurveActive, m.x + gCurveGrabDx, m.y + gCurveGrabDy);
  if (pts == curveActiveCurve())
    return;
  state.adjust.setCurve(gCurveChannel, pts, true);
  enterDrag(false); // switches recompute+paint to the preview path
  state.markAdjustDirty();
}

static void curveEnd() {
  gCurveActive = -1;
  gCurveUiDirty = true;
  state.adjust.endCurveEdit();
  if (state.dragging) { // leave preview mode: one full-resolution recompute
    state.dragging = false;
    state.dragIsAngle = false;
    state.markAdjustDirty();
  }
}

static void curveRightPress(float lx, float ly) {
  if (!state.original)
    return;
  CurvePoints pts = curveActiveCurve();
  if (curveRemove(pts, curveHit(pts, lx, ly))) {
    state.adjust.setCurve(gCurveChannel, pts, false);
    state.markAdjustDirty();
  }
}

static void setCurveChannel(CurveChannel ch) {
  gCurveActive = -1;
  gCurveChannel = ch;
  gCurveUiDirty = true;
}

// Resets the curve being edited (one undo step).
static void resetCurve() {
  gCurveActive = -1;
  state.adjust.setCurve(gCurveChannel, identityCurve(), false);
  gCurveUiDirty = true;
  state.markAdjustDirty();
}

// Resets all four curves (one undo step).
static void resetAllCurves() {
  gCurveActive = -1;
  state.adjust.resetAllCurves();
  gCurveUiDirty = true;
  state.markAdjustDirty();
}

static const char *curveChannelName(CurveChannel ch) {
  switch (ch) {
  case CurveChannel::Red:
    return "Red";
  case CurveChannel::Green:
    return "Green";
  case CurveChannel::Blue:
    return "Blue";
  default:
    return "RGB";
  }
}

// Line/tint color for a curve: white for the master, channel color otherwise.
static Color curveChannelColor(CurveChannel ch, uint8_t alpha) {
  switch (ch) {
  case CurveChannel::Red:
    return Color{235, 70, 70, alpha};
  case CurveChannel::Green:
    return Color{70, 200, 90, alpha};
  case CurveChannel::Blue:
    return Color{80, 130, 255, alpha};
  default:
    return Color{255, 255, 255, alpha};
  }
}

// The histogram that matches a curve: luma for the master, else that channel.
static const std::array<uint32_t, 256> &curveHistogram(CurveChannel ch) {
  switch (ch) {
  case CurveChannel::Red:
    return state.histR;
  case CurveChannel::Green:
    return state.histG;
  case CurveChannel::Blue:
    return state.histB;
  default:
    return state.histL;
  }
}

// Dark plot with the edited channel's histogram faintly behind it, a quarter
// grid, the identity diagonal, every non-straight curve (the others dimmed in
// their own color), and the edited curve's control points.
static void paintCurve(CanvasContext &ctx) {
  const float W = ctx.width(), H = ctx.height();
  const float pad = kCurvePadPx, plot = curvePlotPx();
  ctx.setFillColor(Color{34, 34, 38, 255});
  ctx.fillRect(0, 0, W, H);

  const auto &hist = curveHistogram(gCurveChannel);
  uint32_t peak = 1;
  for (int i = 1; i < 255; ++i)
    peak = std::max(peak, hist[i]);
  const float invPeak = 1.0f / std::sqrt(static_cast<float>(peak));
  ctx.setFillColor(curveChannelColor(gCurveChannel, 45));
  for (int i = 0; i < 256; ++i) {
    const float hh =
        std::min(1.0f, std::sqrt(static_cast<float>(hist[i])) * invPeak) * plot;
    if (hh > 0.0f)
      ctx.fillRect(pad + static_cast<float>(i) / 256.0f * plot, pad + plot - hh,
                   plot / 256.0f + 0.5f, hh);
  }

  ctx.setLineWidth(1.0f);
  ctx.setStrokeColor(Color{255, 255, 255, 30});
  ctx.beginPath();
  for (int q = 0; q <= 4; ++q) {
    const float t = pad + plot * static_cast<float>(q) / 4.0f;
    ctx.moveTo(t, pad);
    ctx.lineTo(t, pad + plot);
    ctx.moveTo(pad, t);
    ctx.lineTo(pad + plot, t);
  }
  ctx.stroke();

  ctx.setStrokeColor(Color{255, 255, 255, 70});
  ctx.beginPath();
  ctx.moveTo(pad, pad + plot);
  ctx.lineTo(pad + plot, pad);
  ctx.stroke();

  // Draws one curve as a polyline. Non-edited, non-straight curves first (dim),
  // so the edited curve ends up on top.
  auto strokeCurve = [&](CurveChannel ch, uint8_t alpha, float width) {
    const auto lut = curveToLut(state.adjust.curve(ch));
    ctx.setStrokeColor(curveChannelColor(ch, alpha));
    ctx.setLineWidth(width);
    ctx.beginPath();
    for (int i = 0; i < 256; ++i) {
      float lx, ly;
      curveToLocal(static_cast<float>(i), lut[i], lx, ly);
      if (i == 0)
        ctx.moveTo(lx, ly);
      else
        ctx.lineTo(lx, ly);
    }
    ctx.stroke();
  };
  const CurvePoints straight = identityCurve();
  for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c) {
    const auto ch = static_cast<CurveChannel>(c);
    if (ch != gCurveChannel && !(state.adjust.curve(ch) == straight))
      strokeCurve(ch, 110, 1.0f);
  }
  strokeCurve(gCurveChannel, 255, 1.75f);

  const CurvePoints &pts = curveActiveCurve();
  ctx.setLineWidth(1.0f);
  for (size_t i = 0; i < pts.size(); ++i) {
    float lx, ly;
    curveToLocal(pts[i].x, pts[i].y, lx, ly);
    const bool active = static_cast<int>(i) == gCurveActive;
    ctx.setFillColor(active ? kColorDividerHover : kColorWhite);
    ctx.setStrokeColor(Color{20, 20, 20, 255});
    ctx.fillRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                 kCurvePointPx, kCurvePointPx);
    ctx.strokeRect(lx - kCurvePointPx / 2.0f, ly - kCurvePointPx / 2.0f,
                   kCurvePointPx, kCurvePointPx);
  }
}

// The pipeline indexes kAdjustmentSpecs by AdjId (AdjustmentSet::buildLut, the
// sidebar loop, the color pass), so entry i must describe AdjId i. This can't
// be a static_assert: the table holds std::function and std::string members,
// which aren't usable in constant expressions. So it's checked once at
// startup instead, and a mistake aborts with a message naming the bad entry
// rather than silently moving the wrong slider.
static void verifyAdjustmentSpecs() {
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i) {
    const AdjustmentSpec &s = kAdjustmentSpecs[i];
    const char *problem = nullptr;
    if (static_cast<size_t>(s.id) != i)
      problem = "is out of order: its id doesn't match its position in AdjId "
                "(or an entry is missing)";
    else if (!(s.min < s.max))
      problem = "has min >= max";
    else if (s.stage == AdjustmentSpec::Stage::Tone && !s.apply)
      problem = "is a Tone adjustment with no apply function";
    if (problem) {
      std::fprintf(stderr, "photoeditor: kAdjustmentSpecs[%zu] (\"%s\") %s\n",
                   i, s.label.c_str(), problem);
      std::abort();
    }
  }
}

int main() {
  verifyAdjustmentSpecs();
  LiteUI ui("Photo Editor", 1100, 700, /*hideTitlebar=*/true);
  // A finished background render wakes the (otherwise sleeping) event loop.
  renderWorker.setNotify([&ui] { ui.wake(); });
  geometryWorker.setNotify([&ui] { ui.wake(); });
  decodeWorker.setNotify([&ui] { ui.wake(); });

  // ---- Keyboard shortcuts ----
  // LiteUI's addShortcut is global: it fires whatever has focus, so these work
  // after clicking a sidebar button too. Modifiers must match exactly.
  // Zoom uses the unshifted Ctrl+= / Ctrl+-: on Linux LiteUI reports shifted
  // symbols like '+' as Key::Unknown, so Ctrl+Shift+= can't be bound there.
  {
    auto withMods = [](bool ctrl, bool shift) {
      KeyModifiers m;
      m.ctrl = ctrl;
      m.shift = shift;
      return m;
    };
    const KeyModifiers none = withMods(false, false);
    const KeyModifiers ctrl = withMods(true, false);
    const KeyModifiers ctrlShift = withMods(true, true);
    // Edits and dialogs are ignored mid-drag (a slider or crop handle is
    // being held): changing the image under the drag would fight it.
    auto idle = [](std::function<void()> fn) {
      return [fn = std::move(fn)] {
        if (!state.dragging)
          fn();
      };
    };
    auto zoomIn = [] { setZoom(state.zoom * kZoomStep); };
    auto zoomOut = [] { setZoom(state.zoom / kZoomStep); };

    ui.addShortcut(ctrl, Key::Z, idle(undo));
    ui.addShortcut(ctrl, Key::Y, idle(redo));
    ui.addShortcut(ctrlShift, Key::Z, idle(redo));
    ui.addShortcut(ctrl, Key::O, idle(openImage));
    ui.addShortcut(ctrl, Key::E, idle(exportImage));
    ui.addShortcut(ctrl, Key::Q, [&ui] { ui.requestClose(); });
    ui.addShortcut(ctrl, Key::B, toggleSidebar);
    ui.addShortcut(ctrl, Key::Equal, zoomIn);
    ui.addShortcut(ctrlShift, Key::Equal,
                   zoomIn); // '+' on layouts that report it
    ui.addShortcut(ctrl, Key::Minus, zoomOut);
    ui.addShortcut(ctrl, Key::N0, [] { setZoom(1.0f); });
    ui.addShortcut(ctrl, Key::LeftBracket,
                   idle([] { applyGeoOp(GeoOp::RotateCCW); }));
    ui.addShortcut(ctrl, Key::RightBracket,
                   idle([] { applyGeoOp(GeoOp::RotateCW); }));
    ui.addShortcut(none, Key::Escape, [] { state.openMenu = -1; });
    ui.addShortcut(none, Key::Delete, idle([] {
                     if (maskToolActive())
                       deleteSelectedMask();
                   }));
    ui.addShortcut(none, Key::Enter, idle([] {
                     if (cropToolActive())
                       applyCrop(); // commit the selection, like the button
                   }));
  }

  const std::vector<Activity> activities = {
      {0, "Info", "I"},      {1, "Adjust", "A"},        {4, "Curve", "~"},
      {5, "Color Mix", "M"}, {6, "Color Grading", "G"},
      {7, "Masks", "\u25D0"}, {2, "Crop", "C"},
      {3, "Rotate", "R"},
  };

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = kColorRootBg;

  // Titlebar: title, File/View menus, window controls.

  View titlebar;
  titlebar.style.direction = FlexDirection::Row;
  titlebar.style.width = Size::full();
  titlebar.style.height = Size::pixel(kTitlebarH);
  titlebar.style.flexShrink = 0;
  titlebar.style.alignItems = Align::Center;
  titlebar.style.backgroundColor = kColorTitlebarBg;
  titlebar.style.borderWidth = 0.0f;
  // Empty space in the titlebar (i.e. not over a button) drags the
  // window, and also closes any open dropdown menu.
  titlebar.onPressAt = [&](float, float) {
    state.openMenu = -1;
    ui.requestMove();
  };

  Text appTitle;
  appTitle.label = std::string("Photo Editor");
  appTitle.fontSize = 13.0f;
  appTitle.fontWeight = FontWeight::SemiBold;
  appTitle.color = kColorTextDark;
  appTitle.style.margin = EdgeInsets{0, 16, 0, 14};
  titlebar.addChild(std::move(appTitle));

  titlebar.addChild(
      menuButton("File", 0,
                 {
                     {"Open Image...", openImage, "Ctrl+O"},
                     {"Export as PNG...", exportImage, "Ctrl+E"},
                     {"Exit", [&] { ui.requestClose(); }, "Ctrl+Q"},
                 }));
  titlebar.addChild(menuButton(
      "View", 1,
      {
          {"Zoom In", [] { setZoom(state.zoom * kZoomStep); }, "Ctrl+="},
          {"Zoom Out", [] { setZoom(state.zoom / kZoomStep); }, "Ctrl+-"},
          {"Actual Size (100%)", [] { setZoom(1.0f); }, "Ctrl+0"},
          {"Toggle Sidebar", toggleSidebar, "Ctrl+B"},
      }));

  titlebar.addChild(
      menuButton("Image", 2,
                 {
                     {"Rotate Left 90\u00B0",
                      [] { applyGeoOp(GeoOp::RotateCCW); }, "Ctrl+["},
                     {"Rotate Right 90\u00B0",
                      [] { applyGeoOp(GeoOp::RotateCW); }, "Ctrl+]"},
                     {"Flip Horizontal", [] { applyGeoOp(GeoOp::FlipH); }},
                     {"Flip Vertical", [] { applyGeoOp(GeoOp::FlipV); }},
                 }));

  View undoRedoGap;
  undoRedoGap.style.width = Size::pixel(8);
  titlebar.addChild(std::move(undoRedoGap));

  titlebar.addChild(
      titlebarIconBtn("\u21B6", undo, [] { return state.adjust.canUndo(); }));
  titlebar.addChild(
      titlebarIconBtn("\u21B7", redo, [] { return state.adjust.canRedo(); }));

  View titlebarSpacer;
  titlebarSpacer.style.flexGrow = 1;
  titlebarSpacer.style.height = Size::full();
  titlebar.addChild(std::move(titlebarSpacer));

  titlebar.addChild(
      winButton("\u2500", kColorMenuHover, [&] { ui.requestMinimize(); }));
  titlebar.addChild(winButton("\u25A1", kColorMenuHover, [&] {
    ui.requestMaximize();
    state.maximized = !state.maximized;
  }));
  titlebar.addChild(
      winButton("\u00D7", kColorCloseHover, [&] { ui.requestClose(); }));

  root.addChild(std::move(titlebar));

  // Workspace: activity bar, sidebar, divider, canvas viewport.

  View workspace;
  workspace.style.direction = FlexDirection::Row;
  workspace.style.width = Size::full();
  workspace.style.flexGrow = 1;
  // Clicking anywhere in the workspace that isn't itself a button
  // (e.g. the canvas, or blank sidebar space) closes an open menu too.
  workspace.onPressAt = [](float, float) { state.openMenu = -1; };

  // ---- Activity bar ----
  View activityBar;
  activityBar.style.width = Size::pixel(kActivityBarW);
  activityBar.style.height = Size::full();
  activityBar.style.flexShrink = 0;
  activityBar.style.direction = FlexDirection::Column;
  activityBar.style.alignItems = Align::Center;
  activityBar.style.backgroundColor = kColorActivityBarBg;
  activityBar.style.padding = EdgeInsets::all(6.0f);
  activityBar.style.gap = 10.0f;

  for (const auto &item : activities) {
    View button;
    button.style.width = Size::pixel(32);
    button.style.height = Size::pixel(32);
    button.style.borderRadius = 6.0f;
    button.style.justifyContent = Justify::Center;
    button.style.alignItems = Align::Center;
    button.style.hoverColor = kColorActivityHover;

    int id = item.id;
    button.style.backgroundColor = std::function<Color()>([id] {
      return state.activeActivity == id ? kColorActivityActive
                                        : kColorActivityBarBg;
    });
    button.onClick = [id] {
      state.activeActivity = (state.activeActivity == id) ? -1 : id;
      // The wheels are laid out for a minimum sidebar width.
      if (state.activeActivity == 6 && state.sidebarWidth < kGradePaneW)
        state.sidebarWidth = kGradePaneW;
      state.markDirty(); // show/hide the crop overlay
    };

    Text icon;
    icon.label = item.icon;
    icon.fontSize = 15.0f;
    icon.fontWeight = FontWeight::SemiBold;
    icon.color = kColorTextIcon;
    button.addChild(std::move(icon));

    activityBar.addChild(std::move(button));
  }

  workspace.addChild(std::move(activityBar));

  // ---- Sidebar (width driven by activeActivity: 0 when closed) ----
  View sidebar;
  sidebar.style.height = Size::full();
  sidebar.style.flexShrink = 0;
  sidebar.style.direction = FlexDirection::Column;
  sidebar.style.backgroundColor = kColorPanelBg;
  sidebar.style.padding = EdgeInsets::all(12.0f);
  sidebar.style.overflowX = Overflow::Hidden;
  sidebar.style.overflowY = Overflow::Auto;
  sidebar.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? state.sidebarWidth : 0.0f);
  });

  Text sidebarTitle;
  sidebarTitle.label =
      std::function<std::string()>([activities]() -> std::string {
        for (const auto &a : activities)
          if (a.id == state.activeActivity)
            return a.title;
        return "";
      });
  sidebarTitle.fontSize = 12.0f;
  sidebarTitle.fontWeight = FontWeight::SemiBold;
  sidebarTitle.color = kColorTextMuted;
  sidebarTitle.style.margin = EdgeInsets{0, 0, 10, 0};
  sidebar.addChild(std::move(sidebarTitle));

  // -- "Info" pane: real data about the open image.
  View infoPane;
  infoPane.style.direction = FlexDirection::Column;
  infoPane.style.width = Size::full();
  infoPane.style.gap = 6.0f;
  infoPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 0 ? Display::Flex : Display::None; });

  Text infoName;
  infoName.label = std::function<std::string()>([] {
    return state.image ? ("File: " + baseName(state.imagePath))
                       : std::string("No image open");
  });
  infoName.fontSize = 13.0f;
  infoName.color = kColorTextMid;
  infoPane.addChild(std::move(infoName));

  Text infoDims;
  infoDims.label = std::function<std::string()>([] {
    if (!state.image)
      return std::string();
    return "Size: " + std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px";
  });
  infoDims.fontSize = 13.0f;
  infoDims.color = kColorTextMid;
  infoPane.addChild(std::move(infoDims));

  Text infoZoom;
  infoZoom.label = std::function<std::string()>([] {
    return "Zoom: " +
           std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  });
  infoZoom.fontSize = 13.0f;
  infoZoom.color = kColorTextMid;
  infoPane.addChild(std::move(infoZoom));

  sidebar.addChild(std::move(infoPane));

  // -- "Adjust" pane: one slider row per entry in kAdjustmentSpecs. Adding
  // a new adjustment later is a one-line addition to that table — this
  // loop, AdjustmentSet::applyTo, and buildAdjustmentSlider all need no
  // changes.
  View adjustPane;
  adjustPane.style.width = Size::full();
  adjustPane.style.direction = FlexDirection::Column;
  adjustPane.style.gap = 16.0f;
  adjustPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 1 ? Display::Flex : Display::None; });

  // Histogram of the picture as currently shown (see computeHistogram).
  View histogram;
  histogram.isCanvas = true;
  histogram.style.width = Size::full();
  histogram.style.height = Size::pixel(kHistHeightPx);
  histogram.style.flexShrink = 0;
  histogram.onPaint = [](CanvasContext &ctx) { paintHistogram(ctx); };
  histogram.canvasDirtySource = [] {
    static uint32_t lastPainted = 0;
    static int lastActivity = -2;
    // Sidebar is polled before the canvas: do the pending recompute now.
    refreshHistogramIfStale();
    // Also repaint when this tab is (re)shown, in case it was hidden while
    // the picture changed.
    const bool tabChanged = state.activeActivity != lastActivity;
    lastActivity = state.activeActivity;
    if (state.histVersion == lastPainted && !tabChanged)
      return false;
    lastPainted = state.histVersion;
    return true;
  };
  adjustPane.addChild(std::move(histogram));

  auto sectionLabel = [](const char *text) {
    Text h;
    h.label = std::string(text);
    h.fontSize = 12.0f;
    h.color = kColorTextMuted;
    return h;
  };
  bool colorHeaderAdded = false;
  adjustPane.addChild(sectionLabel("Tone"));
  for (const auto &spec : kAdjustmentSpecs) {
    if (!colorHeaderAdded && spec.stage == AdjustmentSpec::Stage::Color) {
      adjustPane.addChild(sectionLabel("Color"));
      colorHeaderAdded = true;
    }
    adjustPane.addChild(buildAdjustmentSlider(spec));
  }

  sidebar.addChild(std::move(adjustPane));

  // -- "Curve" pane: tone curves — RGB (master) plus per-channel R, G, B, one
  // tab each. Click the plot to add a point, drag to move it, right-click a
  // point to delete it. The whole gesture (add + drag, or a plain drag) is one
  // undo step; the edited channel's histogram sits behind the curve, and
  // other channels' curves that have been changed stay visible, dimmed.
  View curvePane;
  curvePane.style.width = Size::full();
  curvePane.style.direction = FlexDirection::Column;
  curvePane.style.gap = 8.0f;
  curvePane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 4 ? Display::Flex : Display::None; });

  // Channel tabs. A bullet after a name marks a curve that has been changed,
  // so an edit on a channel you've since switched away from isn't forgotten.
  View curveTabs;
  curveTabs.style.direction = FlexDirection::Row;
  curveTabs.style.gap = 4.0f;
  curveTabs.style.width = Size::pixel(kCurveSizePx);
  curveTabs.style.flexShrink = 0;
  for (int c = 0; c < static_cast<int>(kCurveChannelCount); ++c) {
    const auto ch = static_cast<CurveChannel>(c);
    View tab;
    tab.style.width = Size::pixel(kCurveTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([ch]() -> Color {
      return gCurveChannel == ch ? kColorDropdownRowHover : kColorActivityBarBg;
    });
    tab.style.borderWidth = std::function<float()>(
        [ch] { return gCurveChannel == ch ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [ch] { setCurveChannel(ch); };

    Text tabText;
    tabText.label = std::function<std::string()>([ch]() -> std::string {
      std::string name = ch == CurveChannel::Master  ? "RGB"
                         : ch == CurveChannel::Red   ? "R"
                         : ch == CurveChannel::Green ? "G"
                                                     : "B";
      if (!(state.adjust.curve(ch) == identityCurve()))
        name += " \u2022";
      return name;
    });
    tabText.fontSize = 12.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = ch == CurveChannel::Master  ? kColorTextDark
                    : ch == CurveChannel::Red   ? Color{235, 95, 95, 255}
                    : ch == CurveChannel::Green ? Color{95, 200, 115, 255}
                                                : Color{115, 155, 255, 255};
    tab.addChild(std::move(tabText));
    curveTabs.addChild(std::move(tab));
  }
  curvePane.addChild(std::move(curveTabs));

  View curveCanvas;
  curveCanvas.isCanvas = true;
  curveCanvas.style.width = Size::pixel(kCurveSizePx);
  curveCanvas.style.height = Size::pixel(kCurveSizePx);
  curveCanvas.style.flexShrink = 0;
  curveCanvas.onPaint = [](CanvasContext &ctx) { paintCurve(ctx); };
  curveCanvas.canvasDirtySource = [] {
    static uint32_t lastPainted = 0;
    static int lastActivity = -2;
    // Every image recompute bumps histVersion, which covers curve edits,
    // undo/redo, and a newly opened image.
    refreshHistogramIfStale();
    const bool tabChanged = state.activeActivity != lastActivity;
    lastActivity = state.activeActivity;
    const bool uiDirty = gCurveUiDirty; // channel switch / point selection
    gCurveUiDirty = false;
    if (state.histVersion == lastPainted && !tabChanged && !uiDirty)
      return false;
    lastPainted = state.histVersion;
    return true;
  };
  curveCanvas.onPressAt = [](float lx, float ly) { curvePress(lx, ly); };
  curveCanvas.onDragTo = [](float lx, float ly) { curveDrag(lx, ly); };
  curveCanvas.onDragEnd = [] { curveEnd(); };
  curveCanvas.onRightPressAt = [](float lx, float ly) {
    curveRightPress(lx, ly);
  };
  curvePane.addChild(std::move(curveCanvas));

  Text curveInfo;
  curveInfo.label = std::function<std::string()>([]() -> std::string {
    const auto &p = curveActiveCurve();
    const std::string name = curveChannelName(gCurveChannel);
    if (gCurveActive >= 0 && gCurveActive < static_cast<int>(p.size()))
      return name + ": In " +
             std::to_string(static_cast<int>(std::lround(p[gCurveActive].x))) +
             "  Out " +
             std::to_string(static_cast<int>(std::lround(p[gCurveActive].y)));
    return name + ": " + std::to_string(p.size()) + " points";
  });
  curveInfo.fontSize = 13.0f;
  curveInfo.color = kColorTextMid;
  curvePane.addChild(std::move(curveInfo));

  Text curveHint1;
  curveHint1.label = std::string("Click: add point   Drag: move");
  curveHint1.fontSize = 11.0f;
  curveHint1.color = kColorTextFaint;
  curvePane.addChild(std::move(curveHint1));
  Text curveHint2;
  curveHint2.label = std::string("Right-click: delete point");
  curveHint2.fontSize = 11.0f;
  curveHint2.color = kColorTextFaint;
  curvePane.addChild(std::move(curveHint2));

  curvePane.addChild(sidebarButton("Reset Curve", resetCurve));
  curvePane.addChild(sidebarButton("Reset All Curves", resetAllCurves));

  sidebar.addChild(std::move(curvePane));

  // -- "Color Mix" pane: HSL mixer. Eight color bands, each with Hue,
  // Saturation and Luminance; the tabs pick which of the three sets of eight
  // sliders is shown (a bullet marks a tab that has been changed, so an edit
  // on a tab you've since switched away from isn't forgotten).
  View mixPane;
  mixPane.style.width = Size::full();
  mixPane.style.direction = FlexDirection::Column;
  mixPane.style.gap = 12.0f;
  mixPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 5 ? Display::Flex : Display::None; });

  View mixTabs;
  mixTabs.style.direction = FlexDirection::Row;
  mixTabs.style.gap = 6.0f;
  mixTabs.style.width = Size::pixel(3.0f * kMixTabW + 2.0f * 6.0f);
  mixTabs.style.flexShrink = 0;
  for (int c = 0; c < static_cast<int>(kMixChanCount); ++c) {
    const auto chan = static_cast<MixChan>(c);
    View tab;
    tab.style.width = Size::pixel(kMixTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([chan]() -> Color {
      return gMixChan == chan ? kColorDropdownRowHover : kColorActivityBarBg;
    });
    tab.style.borderWidth = std::function<float()>(
        [chan] { return gMixChan == chan ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [chan] { gMixChan = chan; };

    Text tabText;
    tabText.label = std::function<std::string()>([chan]() -> std::string {
      std::string name = chan == MixChan::Hue          ? "Hue"
                         : chan == MixChan::Saturation ? "Sat"
                                                       : "Lum";
      if (mixChanAnyNonZero(state.adjust.mixValues(), chan))
        name += " \u2022";
      return name;
    });
    tabText.fontSize = 12.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = kColorTextDark;
    tab.addChild(std::move(tabText));
    mixTabs.addChild(std::move(tab));
  }
  mixPane.addChild(std::move(mixTabs));

  // One group of eight sliders per tab; only the selected tab's is displayed.
  for (int c = 0; c < static_cast<int>(kMixChanCount); ++c) {
    const auto chan = static_cast<MixChan>(c);
    View group;
    group.style.width = Size::full();
    group.style.direction = FlexDirection::Column;
    group.style.gap = 12.0f;
    group.style.display = std::function<Display()>(
        [chan] { return gMixChan == chan ? Display::Flex : Display::None; });
    for (size_t band = 0; band < kMixBandCount; ++band)
      group.addChild(buildMixSlider(chan, band));
    mixPane.addChild(std::move(group));
  }

  mixPane.addChild(sidebarButton("Reset Color Mix", resetColorMix));

  sidebar.addChild(std::move(mixPane));

  // -- "Color Grade" pane: 3-way / single-range color wheels (Shadows,
  // Midtones, Highlights, Global) plus shared Blending and Balance sliders.
  View gradePane;
  gradePane.style.width = Size::full();
  gradePane.style.direction = FlexDirection::Column;
  gradePane.style.gap = 10.0f;
  gradePane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 6 ? Display::Flex : Display::None; });

  // Mode tabs: 3-Way | Shadows | Mid | High | Global. A bullet marks a range
  // that has been adjusted.
  View gradeTabs;
  gradeTabs.style.direction = FlexDirection::Row;
  gradeTabs.style.gap = 4.0f;
  gradeTabs.style.flexShrink = 0;
  for (int m = 0; m < 5; ++m) {
    View tab;
    tab.style.width = Size::pixel(kGradeTabW);
    tab.style.height = Size::pixel(26);
    tab.style.flexShrink = 0;
    tab.style.borderRadius = 4.0f;
    tab.style.justifyContent = Justify::Center;
    tab.style.alignItems = Align::Center;
    tab.style.hoverColor = kColorActivityHover;
    tab.style.backgroundColor = std::function<Color()>([m]() -> Color {
      return gGradeMode == m ? kColorDropdownRowHover : kColorActivityBarBg;
    });
    tab.style.borderWidth =
        std::function<float()>([m] { return gGradeMode == m ? 1.0f : 0.0f; });
    tab.style.borderColor = kColorDividerHover;
    tab.onClick = [m] {
      gGradeMode = m;
      if (m > 0)
        gGradeActive = m - 1;
    };

    Text tabText;
    tabText.label = std::function<std::string()>([m]() -> std::string {
      static const char *const names[5] = {"3-Way", "Shad", "Mid", "High",
                                           "Glob"};
      std::string name = names[m];
      if (m > 0 && !state.adjust.grade().wheel[m - 1].neutral())
        name += "\u2022";
      return name;
    });
    tabText.fontSize = 11.0f;
    tabText.fontWeight = FontWeight::SemiBold;
    tabText.color = kColorTextDark;
    tab.addChild(std::move(tabText));
    gradeTabs.addChild(std::move(tab));
  }
  gradePane.addChild(std::move(gradeTabs));

  // H:13 S:72 L:+1 for the wheel touched last
  Text gradeReadout;
  gradeReadout.label = std::function<std::string()>([]() -> std::string {
    const int r = gGradeMode > 0 ? gGradeMode - 1 : gGradeActive;
    const GradeWheel &w = state.adjust.grade().wheel[r];
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s   H:%d S:%d L:%+d", gradeRangeName(r),
                  static_cast<int>(std::lround(w.hue)),
                  static_cast<int>(std::lround(w.sat)),
                  static_cast<int>(std::lround(w.lum)));
    return buf;
  });
  gradeReadout.fontSize = 12.0f;
  gradeReadout.color = kColorTextMid;
  gradePane.addChild(std::move(gradeReadout));

  // One body per mode; only the selected one is displayed.
  auto modeBody = [](int m, View body) {
    body.style.display = std::function<Display()>(
        [m] { return gGradeMode == m ? Display::Flex : Display::None; });
    return body;
  };
  gradePane.addChild(modeBody(0, buildGradeThreeWay()));
  for (int m = 1; m <= 4; ++m)
    gradePane.addChild(modeBody(m, buildGradeSingle(m - 1)));

  // Blending / Balance, shared by every mode.
  SliderSpec blending;
  blending.name = "Blending";
  blending.lo = 0;
  blending.hi = 100;
  blending.get = [] { return state.adjust.grade().blending; };
  blending.set = [](float v) {
    gradeEdit([&](ColorGrade &g) { g.blending = std::round(v); }, 16);
  };
  blending.format = [](float v) {
    return std::to_string(static_cast<int>(std::lround(v)));
  };
  blending.onEnd = [] { state.adjust.endGradeEdit(); };
  gradePane.addChild(buildSliderRow(blending));

  SliderSpec balance = blending;
  balance.name = "Balance";
  balance.lo = -100;
  balance.hi = 100;
  balance.get = [] { return state.adjust.grade().balance; };
  balance.set = [](float v) {
    gradeEdit([&](ColorGrade &g) { g.balance = std::round(v); }, 17);
  };
  balance.format = [](float v) {
    const int i = static_cast<int>(std::lround(v));
    return i > 0 ? "+" + std::to_string(i) : std::to_string(i);
  };
  gradePane.addChild(buildSliderRow(balance));

  gradePane.addChild(sidebarButton("Reset Color Grading", resetColorGrade));
  sidebar.addChild(std::move(gradePane));

  // -- "Masks" pane: local adjustments. Each mask is a shape plus its own copy
  // of the Adjust sliders, applied only where the shape is (see the "masks"
  // block). Pick or add a mask in the list; the selected one is drawn on the
  // image, where it can be dragged (inside = move, square handles = resize).
  View maskPane;
  maskPane.style.width = Size::full();
  maskPane.style.direction = FlexDirection::Column;
  maskPane.style.gap = 6.0f;
  maskPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 7 ? Display::Flex : Display::None; });

  Text maskHint;
  maskHint.label = std::function<std::string()>([]() -> std::string {
    return state.image
               ? "Add a mask, then adjust it. Drag the shape on the image to "
                 "move it, or its square handles to resize (on a linear "
                 "gradient they also turn it)."
               : "Open an image to add masks.";
  });
  maskHint.fontSize = 12.0f;
  maskHint.color = kColorTextMuted;
  maskHint.style.margin = EdgeInsets{0, 0, 6, 0};
  maskPane.addChild(std::move(maskHint));

  // One button per kind of mask.
  maskPane.addChild(sidebarButton(
      "+ Radial Gradient", [] { addMask(MaskType::Radial); }, {}, true,
      [] { return state.original != nullptr; }));
  maskPane.addChild(sidebarButton(
      "+ Linear Gradient", [] { addMask(MaskType::Linear); }, {}, true,
      [] { return state.original != nullptr; }));

  for (size_t i = 0; i < kMaxMasks; ++i)
    maskPane.addChild(buildMaskRow(i));

  // Everything below applies to the selected mask and is hidden without one.
  View maskEditor;
  maskEditor.style.width = Size::full();
  maskEditor.style.direction = FlexDirection::Column;
  maskEditor.style.gap = 12.0f;
  maskEditor.style.margin = EdgeInsets{8, 0, 0, 0};
  maskEditor.style.display = std::function<Display()>(
      [] { return selMask() >= 0 ? Display::Flex : Display::None; });

  maskEditor.addChild(sidebarButton(
      "Enabled",
      [] { maskEditDiscrete([](Mask &m) { m.enabled = !m.enabled; }); },
      [] {
        const Mask *m = selectedMask();
        return m && m->enabled;
      }));
  maskEditor.addChild(sidebarButton(
      "Invert",
      [] { maskEditDiscrete([](Mask &m) { m.invert = !m.invert; }); },
      [] {
        const Mask *m = selectedMask();
        return m && m->invert;
      }));
  maskEditor.addChild(sidebarButton(
      "Show mask tint",
      [] {
        state.maskShowTint = !state.maskShowTint;
        state.markDirty();
      },
      [] { return state.maskShowTint; }));

  // Shape sliders, per mask type (shown only for that type).
  View radialShape;
  radialShape.style.width = Size::full();
  radialShape.style.direction = FlexDirection::Column;
  radialShape.style.gap = 12.0f;
  radialShape.style.display = std::function<Display()>([]() -> Display {
    const Mask *m = selectedMask();
    return m && m->type() == MaskType::Radial ? Display::Flex : Display::None;
  });
  radialShape.addChild(sectionLabel("Shape"));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Width", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const RadialShape &r) { return r.rx * 100.0f; },
      [](RadialShape &r, float v) { r.rx = v / 100.0f; }, kMaskCtlWidth)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Height", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const RadialShape &r) { return r.ry * 100.0f; },
      [](RadialShape &r, float v) { r.ry = v / 100.0f; }, kMaskCtlHeight)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Angle", -90.0f, 90.0f, [](const RadialShape &r) { return r.angle; },
      [](RadialShape &r, float v) { r.angle = v; }, kMaskCtlAngle)));
  radialShape.addChild(buildSliderRow(maskRadialSlider(
      "Feather", 0.0f, 100.0f,
      [](const RadialShape &r) { return r.feather * 100.0f; },
      [](RadialShape &r, float v) { r.feather = v / 100.0f; },
      kMaskCtlFeather)));
  maskEditor.addChild(std::move(radialShape));

  View linearShape;
  linearShape.style.width = Size::full();
  linearShape.style.direction = FlexDirection::Column;
  linearShape.style.gap = 12.0f;
  linearShape.style.display = std::function<Display()>([]() -> Display {
    const Mask *m = selectedMask();
    return m && m->type() == MaskType::Linear ? Display::Flex : Display::None;
  });
  linearShape.addChild(sectionLabel("Shape"));
  linearShape.addChild(buildSliderRow(maskLinearSlider(
      "Length", kMaskMinRadius * 100.0f, kMaskMaxRadius * 100.0f,
      [](const LinearShape &l) { return l.len * 100.0f; },
      [](LinearShape &l, float v) { l.len = v / 100.0f; }, kMaskCtlLength)));
  linearShape.addChild(buildSliderRow(maskLinearSlider(
      "Angle", -180.0f, 180.0f, [](const LinearShape &l) { return l.angle; },
      [](LinearShape &l, float v) { l.angle = v; }, kMaskCtlAngle)));
  maskEditor.addChild(std::move(linearShape));

  // The mask's own adjustments: same table as the Adjust pane.
  bool maskColorHeaderAdded = false;
  maskEditor.addChild(sectionLabel("Tone"));
  for (const auto &spec : kAdjustmentSpecs) {
    if (!maskColorHeaderAdded && spec.stage == AdjustmentSpec::Stage::Color) {
      maskEditor.addChild(sectionLabel("Color"));
      maskColorHeaderAdded = true;
    }
    maskEditor.addChild(buildMaskAdjustmentSlider(spec));
  }

  maskEditor.addChild(sidebarButton("Reset Mask Sliders", [] {
    maskEditDiscrete([](Mask &m) { m.adj.fill(0.0f); });
  }));
  maskEditor.addChild(sidebarButton("Delete Mask", deleteSelectedMask));
  maskPane.addChild(std::move(maskEditor));

  sidebar.addChild(std::move(maskPane));

  // -- "Crop" pane: aspect-ratio presets, a live readout, and the
  // Apply/Reset/Remove buttons. The selection itself is drawn and dragged
  // directly on the canvas (paintCropOverlay / cropPress / cropDrag).
  View cropPane;
  cropPane.style.width = Size::full();
  cropPane.style.direction = FlexDirection::Column;
  cropPane.style.gap = 6.0f;
  cropPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 2 ? Display::Flex : Display::None; });

  Text cropHint;
  cropHint.label = std::function<std::string()>([]() -> std::string {
    return state.image
               ? "Drag on the image to choose an area. Drag the handles to "
                 "resize, or the inside to move."
               : "Open an image to crop it.";
  });
  cropHint.fontSize = 12.0f;
  cropHint.color = kColorTextMuted;
  cropHint.style.margin = EdgeInsets{0, 0, 6, 0};
  cropPane.addChild(std::move(cropHint));

  Text cropAspectTitle;
  cropAspectTitle.label = std::string("Aspect ratio");
  cropAspectTitle.fontSize = 12.0f;
  cropAspectTitle.fontWeight = FontWeight::SemiBold;
  cropAspectTitle.color = kColorTextMuted;
  cropPane.addChild(std::move(cropAspectTitle));

  for (size_t i = 0; i < kCropAspects.size(); ++i)
    cropPane.addChild(sidebarButton(
        kCropAspects[i].label, [i] { setCropAspect(i); },
        [i] { return state.crop.aspectIdx == i; }));

  Text cropInfo;
  cropInfo.label = std::function<std::string()>([]() -> std::string {
    if (!state.image)
      return std::string();
    const int w = static_cast<int>(std::lround(state.crop.r - state.crop.l));
    const int h = static_cast<int>(std::lround(state.crop.b - state.crop.t));
    return "Selection: " + std::to_string(w) + " x " + std::to_string(h) +
           " px";
  });
  cropInfo.fontSize = 13.0f;
  cropInfo.color = kColorTextMid;
  cropInfo.style.margin = EdgeInsets{8, 0, 4, 0};
  cropPane.addChild(std::move(cropInfo));

  cropPane.addChild(sidebarButton("Apply Crop", applyCrop, {}, true,
                                  [] { return !geometryBusy(); }));
  cropPane.addChild(sidebarButton("Reset Selection", [] {
    resetCropSelection();
    state.markDirty();
  }));
  cropPane.addChild(sidebarButton("Remove Crop", removeCrop, {}, false,
                                  [] { return !geometryBusy(); }));

  sidebar.addChild(std::move(cropPane));

  // -- "Rotate" pane: 90-degree turns, mirrors, and a reset. Lossless (pure
  // pixel moves) and each button press is one undo step.
  View rotatePane;
  rotatePane.style.width = Size::full();
  rotatePane.style.direction = FlexDirection::Column;
  rotatePane.style.gap = 6.0f;
  rotatePane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 3 ? Display::Flex : Display::None; });

  Text rotateHint;
  rotateHint.label = std::function<std::string()>([]() -> std::string {
    return state.image ? "Turn or flip the whole image, or straighten it with "
                         "the angle slider. Straightening clears any crop."
                       : "Open an image to rotate it.";
  });
  rotateHint.fontSize = 12.0f;
  rotateHint.color = kColorTextMuted;
  rotateHint.style.margin = EdgeInsets{0, 0, 6, 0};
  rotatePane.addChild(std::move(rotateHint));

  const auto geoIdle = [] { return !geometryBusy(); };
  rotatePane.addChild(sidebarButton(
      "\u21BA  Rotate Left 90\u00B0", [] { applyGeoOp(GeoOp::RotateCCW); }, {},
      false, geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u21BB  Rotate Right 90\u00B0", [] { applyGeoOp(GeoOp::RotateCW); }, {},
      false, geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u2194  Flip Horizontal", [] { applyGeoOp(GeoOp::FlipH); }, {}, false,
      geoIdle));
  rotatePane.addChild(sidebarButton(
      "\u2195  Flip Vertical", [] { applyGeoOp(GeoOp::FlipV); }, {}, false,
      geoIdle));
  rotatePane.addChild(sidebarButton("Reset Turns & Flips", resetOrientation, {},
                                    false, geoIdle));

  // ---- Straighten: arbitrary angle ----
  Text straightenTitle;
  straightenTitle.label = std::string("Straighten");
  straightenTitle.fontSize = 12.0f;
  straightenTitle.fontWeight = FontWeight::SemiBold;
  straightenTitle.color = kColorTextMuted;
  straightenTitle.style.margin = EdgeInsets{10, 0, 0, 0};
  rotatePane.addChild(std::move(straightenTitle));

  SliderSpec angleSlider;
  angleSlider.name = "Angle";
  angleSlider.lo = -kAngleMax;
  angleSlider.hi = kAngleMax;
  angleSlider.get = [] { return state.adjust.angle(); };
  angleSlider.set = setAngleFromSlider;
  angleSlider.format = [](float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f\u00B0", static_cast<double>(v));
    return std::string(buf);
  };
  angleSlider.isAngle = true;
  rotatePane.addChild(buildSliderRow(angleSlider));

  // Fine adjustment: the track is ~0.6 degrees per pixel, too coarse for
  // levelling a horizon, so step buttons sit right under it.
  auto nudgeBtn = [](const std::string &label, float delta) {
    View v;
    v.style.width = Size::pixel(36);
    v.style.height = Size::pixel(26);
    v.style.flexShrink = 0;
    v.style.borderRadius = 4.0f;
    v.style.justifyContent = Justify::Center;
    v.style.alignItems = Align::Center;
    v.style.backgroundColor = kColorActivityBarBg;
    v.style.hoverColor = kColorActivityHover;
    v.onClick = [delta] { nudgeAngle(delta); };
    Text t;
    t.label = label;
    t.fontSize = 11.0f;
    t.color = kColorTextDark;
    v.addChild(std::move(t));
    return v;
  };
  View nudgeRow;
  nudgeRow.style.direction = FlexDirection::Row;
  nudgeRow.style.gap = 3.0f;
  nudgeRow.addChild(nudgeBtn("-1\u00B0", -1.0f));
  nudgeRow.addChild(nudgeBtn("-0.1\u00B0", -0.1f));
  nudgeRow.addChild(nudgeBtn("+0.1\u00B0", 0.1f));
  nudgeRow.addChild(nudgeBtn("+1\u00B0", 1.0f));
  rotatePane.addChild(std::move(nudgeRow));

  Text cornersTitle;
  cornersTitle.label = std::string("Corners");
  cornersTitle.fontSize = 12.0f;
  cornersTitle.fontWeight = FontWeight::SemiBold;
  cornersTitle.color = kColorTextMuted;
  cornersTitle.style.margin = EdgeInsets{4, 0, 0, 0};
  rotatePane.addChild(std::move(cornersTitle));

  rotatePane.addChild(sidebarButton(
      "Auto-crop empty corners", [] { setCornerMode(false); },
      [] { return !state.adjust.expand(); }));
  rotatePane.addChild(sidebarButton(
      "Keep full canvas", [] { setCornerMode(true); },
      [] { return state.adjust.expand(); }));
  rotatePane.addChild(
      sidebarButton("Reset Angle", [] { setAngleDiscrete(0.0f); }));

  sidebar.addChild(std::move(rotatePane));

  workspace.addChild(std::move(sidebar));

  // ---- Vertical divider — drag to resize the sidebar ----
  View vDivider;
  vDivider.style.height = Size::full();
  vDivider.style.flexShrink = 0;
  vDivider.style.backgroundColor = kColorDivider;
  vDivider.style.hoverColor = kColorDividerHover;
  vDivider.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? kDividerW : 0.0f);
  });

  auto updateSidebarWidth = [](float localX) {
    float next = state.sidebarWidth + localX - kDividerW / 2.0f;
    state.sidebarWidth = std::clamp(next, kMinSidebarW, kMaxSidebarW);
  };
  vDivider.onPressAt = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };
  vDivider.onDragTo = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };

  workspace.addChild(std::move(vDivider));

  // ---- Scrollable canvas viewport ----
  View viewport;
  viewport.style.height = Size::full();
  viewport.style.flexGrow = 1;
  viewport.style.backgroundColor = kColorViewportBg;
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24);
  viewport.style.wheelScrollEnabled = false;
  viewport.style.scrollThumbColor = kColorScrollThumb;
  viewport.style.scrollTrackColor = kColorTrack;
  viewport.style.scrollbarAutoHide = true;
  // Only occupies the slot in `workspace` while an image is open; the
  // "no image" placeholder below is workspace's other child for that
  // same slot and shows exactly when this one doesn't.
  viewport.style.display = std::function<Display()>(
      [] { return state.image ? Display::Flex : Display::None; });

  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = std::function<Size()>(
      [] { return Size::pixel(state.docWidth() * state.zoom); });
  canvas.style.height = std::function<Size()>(
      [] { return Size::pixel(state.docHeight() * state.zoom); });
  canvas.style.borderWidth = 1.0f;
  canvas.style.borderColor = kColorCanvasBorder;
  canvas.style.backgroundColor = kColorWhite;

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx); };
  canvas.canvasDirtySource = [] {
    // Polled once per frame (see liteui.hpp's checkForUpdates) — the right
    // place to do the deferred, expensive part of a slider drag exactly
    // once per frame, no matter how many onDragTo events fired since the
    // last poll.
    flushPendingAdjust();
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };
  // Crop-selection dragging (no-ops unless the Crop pane is open).
  // Each is a no-op unless its own pane is open (crop: 2, masks: 7).
  canvas.onPressAt = [](float lx, float ly) {
    cropPress(lx, ly);
    maskPress(lx, ly);
  };
  canvas.onDragTo = [](float lx, float ly) {
    cropDrag(lx, ly);
    maskDrag(lx, ly);
  };
  canvas.onDragEnd = [] {
    state.crop.drag = CropUi::Drag::None;
    if (state.mask.drag != MaskUi::Drag::None)
      maskEndDrag();
  };
  canvas.onScrollUp = [] { setZoom(state.zoom * kZoomStep); };
  canvas.onScrollDown = [] { setZoom(state.zoom / kZoomStep); };

  viewport.addChild(std::move(canvas));

  workspace.addChild(std::move(viewport));

  View noImagePlaceholder;
  noImagePlaceholder.style.height = Size::full();
  noImagePlaceholder.style.flexGrow = 1;
  noImagePlaceholder.style.backgroundColor = kColorViewportBg;
  noImagePlaceholder.style.justifyContent = Justify::Center;
  noImagePlaceholder.style.alignItems = Align::Center;
  noImagePlaceholder.style.display = std::function<Display()>(
      [] { return state.image ? Display::None : Display::Flex; });

  Text noImageText;
  noImageText.label = std::string("No image selected");
  noImageText.fontSize = 14.0f;
  noImageText.color = kColorTextFaint;
  noImagePlaceholder.addChild(std::move(noImageText));

  workspace.addChild(std::move(noImagePlaceholder));

  root.addChild(std::move(workspace));

  // Status bar: image info on the left, zoom controls on the right.

  View statusBar;
  statusBar.style.width = Size::full();
  statusBar.style.height = Size::pixel(kStatusBarH);
  statusBar.style.flexShrink = 0;
  statusBar.style.direction = FlexDirection::Row;
  statusBar.style.alignItems = Align::Center;
  statusBar.style.justifyContent = Justify::SpaceBetween;
  statusBar.style.backgroundColor = kColorStatusBarBg;
  statusBar.style.padding = EdgeInsets{0, 10, 0, 12};

  Text statusLeft;
  statusLeft.label = std::function<std::string()>([] {
    std::string s = state.image
                        ? baseName(state.imagePath) + "  —  " +
                              std::to_string(state.image->width) + " x " +
                              std::to_string(state.image->height) + " px"
                        : std::string("No image open");
    if (!state.statusMsg.empty())
      s += "     •     " + state.statusMsg;
    return s;
  });
  statusLeft.fontSize = 12.0f;
  statusLeft.wrap = TextWrap::NoWrap; // one line, even with a long message
  statusLeft.color = std::function<Color()>(
      [] { return state.statusIsError ? kColorStatusError : kColorWhite; });
  statusBar.addChild(std::move(statusLeft));

  // Right side of the status bar: zoom controls.
  auto statusGlyphBtn = [](const std::string &label,
                           std::function<void()> onClick) {
    View v;
    v.style.width = Size::pixel(20);
    v.style.height = Size::pixel(20);
    v.style.borderRadius = 3.0f;
    v.style.justifyContent = Justify::Center;
    v.style.alignItems = Align::Center;
    v.style.hoverColor = kColorStatusHover;
    v.onClick = std::move(onClick);
    Text t;
    t.label = label;
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    v.addChild(std::move(t));
    return v;
  };

  View zoomControls;
  zoomControls.style.direction = FlexDirection::Row;
  zoomControls.style.alignItems = Align::Center;
  zoomControls.style.gap = 4.0f;

  zoomControls.addChild(
      statusGlyphBtn("-", [] { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.label = [] {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 12.0f;
  zoomLabel.color = kColorWhite;
  zoomLabel.style.margin = EdgeInsets{0, 4, 0, 4};
  zoomControls.addChild(std::move(zoomLabel));

  zoomControls.addChild(
      statusGlyphBtn("+", [] { setZoom(state.zoom * kZoomStep); }));

  View zoomResetSpacer;
  zoomResetSpacer.style.width = Size::pixel(6);
  zoomControls.addChild(std::move(zoomResetSpacer));

  View zoomResetBtn;
  zoomResetBtn.style.justifyContent = Justify::Center;
  zoomResetBtn.style.alignItems = Align::Center;
  zoomResetBtn.style.hoverColor = kColorStatusHover;
  zoomResetBtn.style.borderRadius = 3.0f;
  zoomResetBtn.style.padding = EdgeInsets{2, 6, 2, 6};
  zoomResetBtn.onClick = [] { setZoom(1.0f); };
  {
    Text t;
    t.label = std::string("100%");
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    zoomResetBtn.addChild(std::move(t));
  }
  zoomControls.addChild(std::move(zoomResetBtn));

  statusBar.addChild(std::move(zoomControls));

  root.addChild(std::move(statusBar));

  ui.setRoot(std::move(root));
  ui.run();
  renderWorker.shutdown();   // join the worker before `ui` goes out of scope
  geometryWorker.shutdown(); // ditto
  decodeWorker.shutdown();   // ditto (waits out a decode still running)
  return 0;
}