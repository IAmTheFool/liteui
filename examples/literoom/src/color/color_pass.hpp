// color_pass.hpp - The float per-pixel color pass: white balance, vibrance, saturation,
// hue, colorize, Color Mixer, Color Grading. Runs in place on RGBA8.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "color/color_grade.hpp"
#include "color/color_mix.hpp"
#include "core/pixel_math.hpp"

constexpr float kHueShiftDegrees = 90.0f; // Hue slider +/-100 = +/- this many degrees
constexpr float kColorizeStrength = 0.6f; // Colorize Amount 100 = this fraction of the tint's chroma

// ---------------- Filters (the "look" block, last in the color pass) ----------------
// Every filter is a strength 0..100 (0 = untouched). At strength s the result
// is lerp(pixel, filtered(pixel), s / 100), so 0 is exactly the identity and
// existing renders (and the golden fingerprints) cannot change. Fixed order:
// Vintage -> Fade -> Greyscale -> Sepia -> (clamp) -> Solarize -> Posterize -> Negative.
struct FilterValues
{
  float greyscale = 0, sepia = 0, vintage = 0, fade = 0, negative = 0,
        posterize = 0, solarize = 0;
};

struct FilterPass
{
  bool any = false; // false = skip the filter block entirely
  float grey = 0, sepia = 0, vintage = 0, fade = 0, negative = 0, solarize = 0; // 0..1
  int posterLevels = 0;                                                         // 0 = off, else 2..255
  float posterScale = 0, posterInv = 0;                                         // (L-1)/255 and 255/(L-1)
};

inline FilterPass makeFilterPass(const FilterValues &v)
{
  // NaN/inf -> 0, everything else clamped to 0..100, then scaled to 0..1.
  auto amt = [](float x)
  { return std::isfinite(x) ? std::clamp(x, 0.0f, 100.0f) / 100.0f : 0.0f; };
  FilterPass f;
  f.grey = amt(v.greyscale);
  f.sepia = amt(v.sepia);
  f.vintage = amt(v.vintage);
  f.fade = amt(v.fade);
  f.negative = amt(v.negative);
  f.solarize = amt(v.solarize);
  const float p = amt(v.posterize);
  if (p > 0.0f)
  {
    // Level count falls smoothly from 256 (p = 0) to 2 (p = 1), so the slider
    // has no dead zone and no sudden jump. 256 levels or more = no visible effect.
    int levels = static_cast<int>(
        std::lround(256.0 * std::pow(2.0 / 256.0, static_cast<double>(p))));
    if (levels < 2)
      levels = 2;
    if (levels < 256)
    {
      f.posterLevels = levels;
      f.posterScale = static_cast<float>(levels - 1) / 255.0f;
      f.posterInv = 255.0f / static_cast<float>(levels - 1);
    }
  }
  f.any = f.grey > 0.0f || f.sepia > 0.0f || f.vintage > 0.0f || f.fade > 0.0f ||
          f.negative > 0.0f || f.solarize > 0.0f || f.posterLevels != 0;
  return f;
}

inline float filterLerp(float a, float b, float t) { return a + (b - a) * t; }
inline float filterClamp255(float x) { return x < 0.0f ? 0.0f : (x > 255.0f ? 255.0f : x); }

// One pixel, float in and out (0..255, may leave the range; the caller rounds
// and clamps once). Plain code, no std:: helpers, for the Debug-build reason
// stated at applyColorPass.
inline void applyFilterPixel(const FilterPass &f, float &r, float &g, float &b)
{
  r = filterClamp255(r);
  g = filterClamp255(g);
  b = filterClamp255(b);
  if (f.vintage > 0.0f)
  {
    // Partial desaturation, warm cast, lifted blacks, slightly flatter contrast.
    const float l = kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b;
    float vr = filterLerp(r, l, 0.35f) * 1.06f + 8.0f;
    float vg = filterLerp(g, l, 0.35f) * 1.00f + 4.0f;
    float vb = filterLerp(b, l, 0.35f) * 0.88f + 0.0f;
    vr = 128.0f + (vr - 128.0f) * 0.92f;
    vg = 128.0f + (vg - 128.0f) * 0.92f;
    vb = 128.0f + (vb - 128.0f) * 0.92f;
    r = filterLerp(r, vr, f.vintage);
    g = filterLerp(g, vg, f.vintage);
    b = filterLerp(b, vb, f.vintage);
  }
  if (f.fade > 0.0f)
  {
    // Lift the blacks to 40 and compress the range (white stays at 255).
    const float k = 215.0f / 255.0f;
    r = filterLerp(r, 40.0f + r * k, f.fade);
    g = filterLerp(g, 40.0f + g * k, f.fade);
    b = filterLerp(b, 40.0f + b * k, f.fade);
  }
  if (f.grey > 0.0f)
  {
    const float l = kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b;
    r = filterLerp(r, l, f.grey);
    g = filterLerp(g, l, f.grey);
    b = filterLerp(b, l, f.grey);
  }
  if (f.sepia > 0.0f)
  {
    const float sr = 0.393f * r + 0.769f * g + 0.189f * b;
    const float sg = 0.349f * r + 0.686f * g + 0.168f * b;
    const float sb = 0.272f * r + 0.534f * g + 0.131f * b;
    r = filterLerp(r, sr, f.sepia);
    g = filterLerp(g, sg, f.sepia);
    b = filterLerp(b, sb, f.sepia);
  }
  // Everything below needs 0..255 input (earlier steps can overshoot).
  r = filterClamp255(r);
  g = filterClamp255(g);
  b = filterClamp255(b);
  if (f.solarize > 0.0f)
  {
    // Invert only the channel values above mid-gray.
    r = filterLerp(r, r > 128.0f ? 255.0f - r : r, f.solarize);
    g = filterLerp(g, g > 128.0f ? 255.0f - g : g, f.solarize);
    b = filterLerp(b, b > 128.0f ? 255.0f - b : b, f.solarize);
  }
  if (f.posterLevels != 0)
  {
    // Hard quantize (the slider moves the level count, not a blend). Inputs are
    // >= 0 here, so truncating (x + 0.5) is a correct round.
    r = static_cast<float>(static_cast<int>(r * f.posterScale + 0.5f)) * f.posterInv;
    g = static_cast<float>(static_cast<int>(g * f.posterScale + 0.5f)) * f.posterInv;
    b = static_cast<float>(static_cast<int>(b * f.posterScale + 0.5f)) * f.posterInv;
  }
  if (f.negative > 0.0f)
  {
    r = filterLerp(r, 255.0f - r, f.negative);
    g = filterLerp(g, 255.0f - g, f.negative);
    b = filterLerp(b, 255.0f - b, f.negative);
  }
}

// ---------------- Color pass (float, per pixel) ----------------
// Runs on the LUT's 8-bit output. Everything here is computed in float and
// rounded/clamped once at the end, so stacking Temperature + Vibrance +
// Saturation doesn't accumulate 8-bit rounding the way chained LUT stages do.
// Order: white balance gains -> vibrance -> saturation -> hue -> color mixer ->
// colorize -> color grading. Works on the gamma-encoded values directly (no
// linearization) — a standard, cheap approximation for a first version.
//
// makeColorPass sanitizes every slider value (clamped to its range, NaN/inf
// treated as 0), so the per-pixel loop only ever sees finite values and its
// table indices (mixer hue, grade luma) are always in range.
struct ColorPass
{
  bool active = false;                         // false = skip the pass entirely (fast path)
  float gain[3] = {1, 1, 1};                   // per-channel white-balance gains (R,G,B)
  float vibrance = 0.0f;                       // -1..+1
  float saturation = 1.0f;                     // multiplier: 0 = gray, 1 = untouched, 2 = double
  bool hue = false;                            // Hue shift enabled (hueM is valid)
  float hueM[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // row-major RGB rotation about gray
  bool colorize = false;                       // Colorize enabled (tintVec is valid)
  float tintVec[3] = {0, 0, 0};                // luma-neutral RGB offset at full midtone weight
  bool mix = false;                            // Color Mixer stage enabled (mixLut is valid)
  MixLut mixLut;                               // only filled when `mix` is true
  bool grade = false;                          // Color Grading stage enabled (gradeLut is valid)
  GradeLut gradeLut{};                         // only filled when `grade` is true
  FilterPass filters;                          // Filters block (filters.any = enabled), runs last
};

// UI values are the -100..+100 slider values (Colorize Hue is 0..360 degrees,
// Colorize Amount 0..100).
inline ColorPass makeColorPass(float temperature, float tint, float vibrance,
                               float saturation, const MixValues &mix,
                               const ColorGrade &grade,
                               float hueShift = 0.0f, float colorizeHue = 0.0f,
                               float colorizeAmount = 0.0f,
                               const FilterValues &filters = FilterValues{})
{
  // Sanitize first: NaN/inf -> 0, everything else clamped to its range. A NaN
  // here would otherwise reach static_cast<int> and then a table index.
  auto sane = [](float v, float lo, float hi)
  { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
  temperature = sane(temperature, -100.0f, 100.0f);
  tint = sane(tint, -100.0f, 100.0f);
  vibrance = sane(vibrance, -100.0f, 100.0f);
  saturation = sane(saturation, -100.0f, 100.0f);
  hueShift = sane(hueShift, -100.0f, 100.0f);
  colorizeHue = std::isfinite(colorizeHue) ? colorizeHue : 0.0f;
  colorizeAmount = sane(colorizeAmount, 0.0f, 100.0f);
  MixValues mixv = mix;
  for (float &v : mixv)
    v = mixClamped(v);
  // `grade` is sanitized inside buildGradeLut.

  ColorPass cp;
  cp.mix = mixAnyNonZero(mixv);
  cp.grade = grade.anyActive();
  cp.hue = hueShift != 0.0f;
  cp.colorize = colorizeAmount > 0.0f;
  cp.filters = makeFilterPass(filters); // sanitizes its own values
  cp.active = cp.mix || cp.grade || cp.hue || cp.colorize || cp.filters.any ||
              temperature != 0.0f || tint != 0.0f || vibrance != 0.0f ||
              saturation != 0.0f;
  if (!cp.active)
    return cp;
  if (cp.hue)
  {
    // Rotation about the gray axis (the same matrix as SVG's hueRotate).
    const float a = hueShift / 100.0f * kHueShiftDegrees * static_cast<float>(kDegToRad);
    const float c = std::cos(a), s = std::sin(a);
    cp.hueM[0] = 0.213f + c * 0.787f - s * 0.213f;
    cp.hueM[1] = 0.715f - c * 0.715f - s * 0.715f;
    cp.hueM[2] = 0.072f - c * 0.072f + s * 0.928f;
    cp.hueM[3] = 0.213f - c * 0.213f + s * 0.143f;
    cp.hueM[4] = 0.715f + c * 0.285f + s * 0.140f;
    cp.hueM[5] = 0.072f - c * 0.072f - s * 0.283f;
    cp.hueM[6] = 0.213f - c * 0.213f - s * 0.787f;
    cp.hueM[7] = 0.715f - c * 0.715f + s * 0.715f;
    cp.hueM[8] = 0.072f + c * 0.928f + s * 0.072f;
  }
  if (cp.colorize)
  {
    // The pure color of that hue (HSL s = 1, l = 0.5), then only its chroma:
    // subtracting its own luma makes the offset luma-neutral.
    float cz[3];
    gradeHueToRgb(colorizeHue, cz);
    const float lt = kGradeLumaR * cz[0] + kGradeLumaG * cz[1] + kGradeLumaB * cz[2];
    const float k = colorizeAmount / 100.0f * kColorizeStrength * 255.0f;
    for (int i = 0; i < 3; ++i)
      cp.tintVec[i] = (cz[i] - lt) * k;
  }
  if (cp.mix)
    cp.mixLut = buildMixLut(mixv);
  if (cp.grade)
    cp.gradeLut = buildGradeLut(grade);
  const float t = temperature / 100.0f, n = tint / 100.0f;
  // Warm: R up / B down. Magenta: G down, R and B slightly up.
  const float gR = (1.0f + 0.3f * t) * (1.0f + 0.1f * n);
  const float gG = 1.0f - 0.2f * n;
  const float gB = (1.0f - 0.3f * t) * (1.0f + 0.1f * n);
  // Normalize by the gains' luma so a white-balance shift changes color, not
  // overall brightness. (Within the clamped ranges every gain is > 0.6, so gl
  // is never near zero.)
  const float gl = kGradeLumaR * gR + kGradeLumaG * gG + kGradeLumaB * gB;
  cp.gain[0] = gR / gl;
  cp.gain[1] = gG / gl;
  cp.gain[2] = gB / gl;
  cp.vibrance = vibrance / 100.0f;
  cp.saturation = 1.0f + saturation / 100.0f;
  return cp;
}

// ---------------- White-balance picker (the eyedropper's math) ----------------
// Given the color of a spot that should be neutral gray, finds the Temperature
// and Tint slider values whose gains (the ones makeColorPass builds above)
// turn that spot gray. The input must be what REACHES the color pass: the
// picture after the tone tables and BEFORE any white balance. That value does
// not depend on the current Temperature / Tint, so the answer is absolute, not
// a nudge on top of what is set now.
//
// With t = Temperature/100 and n = Tint/100 the gains are proportional to
//   R: (1 + 0.3 t)(1 + 0.1 n)   G: (1 - 0.2 n)   B: (1 - 0.3 t)(1 + 0.1 n)
// (the luma normalization in makeColorPass scales all three alike, so it does
// not change ratios). Gray means R*gR = G*gG = B*gB, which solves in closed
// form: R(1 + 0.3 t) = B(1 - 0.3 t) gives t, then G(1 - 0.2 n) = m(1 + 0.1 n)
// gives n, where m is the red/blue level after the temperature gains.
enum class WbPickStatus
{
  Ok,      // solved exactly (to slider rounding)
  Limited, // needs more than +/-100: clamped, so the spot ends up close to gray, not exactly gray
  TooDark, // too little signal to read a color from
  Clipped  // a channel is at its maximum: its true value is lost
};

struct WbPick
{
  float temperature = 0.0f, tint = 0.0f; // whole slider values, -100..100
  WbPickStatus status = WbPickStatus::TooDark;
  bool usable() const { return status == WbPickStatus::Ok || status == WbPickStatus::Limited; }
};

constexpr float kWbMinLevel = 6.0f;   // every channel must be at least this (0..255)
constexpr float kWbMaxLevel = 254.0f; // a channel at or above this counts as clipped

// r, g, b: 0..255, after the tone tables (an average over a small patch is fine).
inline WbPick solveWhiteBalance(float r, float g, float b)
{
  WbPick out; // status starts as TooDark: NaN/inf end up there
  if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
    return out;
  if (r >= kWbMaxLevel || g >= kWbMaxLevel || b >= kWbMaxLevel)
  {
    out.status = WbPickStatus::Clipped;
    return out;
  }
  if (r < kWbMinLevel || g < kWbMinLevel || b < kWbMinLevel)
    return out;
  bool limited = false;
  float t = (b - r) / (0.3f * (r + b));
  if (t > 1.0f)
  {
    t = 1.0f;
    limited = true;
  }
  else if (t < -1.0f)
  {
    t = -1.0f;
    limited = true;
  }
  // Red and blue after the temperature gains. Equal unless t was clamped; the
  // average then splits the remaining error between them.
  const float m = 0.5f * (r * (1.0f + 0.3f * t) + b * (1.0f - 0.3f * t));
  float n = (g - m) / (0.2f * g + 0.1f * m); // the denominator is always > 0 here
  if (n > 1.0f)
  {
    n = 1.0f;
    limited = true;
  }
  else if (n < -1.0f)
  {
    n = -1.0f;
    limited = true;
  }
  out.temperature = std::round(t * 100.0f);
  out.tint = std::round(n * 100.0f);
  out.status = limited ? WbPickStatus::Limited : WbPickStatus::Ok;
  return out;
}

// Applies `cp` in place to `n` bytes of RGBA8 (alpha untouched). Raw pointers
// and no std::clamp/min/max calls in the loop, for the same Debug-build
// reason as AdjustmentSet::applyTo.
inline void applyColorPass(const ColorPass &cp, std::uint8_t *p, std::size_t n)
{
  if (!cp.active)
    return;
  const float gR = cp.gain[0], gG = cp.gain[1], gB = cp.gain[2];
  const bool doVib = cp.vibrance != 0.0f;
  const bool doSat = cp.saturation != 1.0f;
  const bool doHue = cp.hue;
  const bool doTint = cp.colorize;
  const float *hm = cp.hueM;
  const float tvR = cp.tintVec[0], tvG = cp.tintVec[1], tvB = cp.tintVec[2];
  const bool doMix = cp.mix;
  const bool doGrade = cp.grade;
  const bool doFilt = cp.filters.any;
  const FilterPass &fp = cp.filters;
  const float *gdR = cp.gradeLut[0].data();
  const float *gdG = cp.gradeLut[1].data();
  const float *gdB = cp.gradeLut[2].data();
  const float vib = cp.vibrance, sat = cp.saturation;
  const float *lutHue = cp.mixLut.hueShift.data();
  const float *lutSat = cp.mixLut.satMul.data();
  const float *lutLum = cp.mixLut.lumAdj.data();
  for (std::size_t i = 0; i + 3 < n; i += 4)
  {
    float r = static_cast<float>(p[i]) * gR;
    float g = static_cast<float>(p[i + 1]) * gG;
    float b = static_cast<float>(p[i + 2]) * gB;
    if (doVib)
    {
      // Positive vibrance boosts muted colors most (weight 1 - s); negative
      // mutes already-saturated colors most (weight s). s = HSV saturation.
      const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const float s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
      const float k = vib > 0.0f ? 1.0f + vib * (1.0f - s) : 1.0f + vib * s;
      const float l = kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b;
      r = l + (r - l) * k;
      g = l + (g - l) * k;
      b = l + (b - l) * k;
    }
    if (doSat)
    {
      const float l = kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b;
      r = l + (r - l) * sat;
      g = l + (g - l) * sat;
      b = l + (b - l) * sat;
    }
    if (doHue)
    {
      const float r0 = r, g0 = g, b0 = b;
      r = hm[0] * r0 + hm[1] * g0 + hm[2] * b0;
      g = hm[3] * r0 + hm[4] * g0 + hm[5] * b0;
      b = hm[6] * r0 + hm[7] * g0 + hm[8] * b0;
    }
    if (doMix)
    {
      // Color Mixer. Work in HSL on values clamped to 0..255.
      r = r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r);
      g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
      b = b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b);
      const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      const float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
      const float d = mx - mn;
      if (d > 0.0f)
      {          // d == 0 is a pure gray: no hue, nothing to mix
        float h; // hue in sextants, 0..6
        if (mx == r)
        {
          h = (g - b) / d;
          if (h < 0.0f)
            h += 6.0f;
        }
        else if (mx == g)
        {
          h = (b - r) / d + 2.0f;
        }
        else
        {
          h = (r - g) / d + 4.0f;
        }
        h *= 60.0f; // degrees
        int li = static_cast<int>(h * (kMixLutSize / 360.0f) + 0.5f);
        if (li >= kMixLutSize) // hue just below 360 rounds up to the table size
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
        switch (sector)
        {
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
    if (doTint)
    {
      // Weight 4 l (1 - l): strongest in the midtones, zero at pure black
      // and white, so the cast can't push the ends out of range.
      float l = (kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b) * (1.0f / 255.0f);
      l = l < 0.0f ? 0.0f : (l > 1.0f ? 1.0f : l);
      const float w = 4.0f * l * (1.0f - l);
      r += tvR * w;
      g += tvG * w;
      b += tvB * w;
    }
    if (doGrade)
    {
      // Color Grading: offsets depend only on the pixel's luma.
      r = r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r);
      g = g < 0.0f ? 0.0f : (g > 255.0f ? 255.0f : g);
      b = b < 0.0f ? 0.0f : (b > 255.0f ? 255.0f : b);
      const int yi = static_cast<int>(
          kGradeLumaR * r + kGradeLumaG * g + kGradeLumaB * b + 0.5f);
      r += gdR[yi];
      g += gdG[yi];
      b += gdB[yi];
    }
    if (doFilt)
      applyFilterPixel(fp, r, g, b); // the "look" block, still float
    p[i] = toU8Clamped(r);
    p[i + 1] = toU8Clamped(g);
    p[i + 2] = toU8Clamped(b);
  }
}