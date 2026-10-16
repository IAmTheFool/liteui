// color_grade.hpp - Color Grading: 3-way + global wheels baked into one per-luma table
// of RGB offsets (GradeLut). Pure math.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "pixel_math.hpp"

// ---------------- Color Grading (3-way + global wheels) ----------------
// Lightroom-style split toning: four wheels (Shadows, Midtones, Highlights,
// Global), each with a hue + saturation (the wheel puck) and a luminance
// offset. Blending sets how softly the three tonal ranges overlap; Balance
// shifts the crossover points: positive Balance moves them toward the shadows,
// so the highlights wheel covers more of the image (negative does the
// opposite, favoring the shadows wheel).
//
// The result depends only on a pixel's luma, so all of it (weights, tint
// vectors, luminance offsets of all four wheels) is baked into one small
// per-luma table of RGB offsets (GradeLut). The per-pixel work is: luma,
// one table read, three adds (see applyColorPass).
//
// All inputs are sanitized in buildGradeLut (clamped to their ranges, NaN
// treated as neutral), and the luminance offsets are kept from inverting
// tones (see the end of buildGradeLut).
enum class GradeRange : int
{
  Shadows,
  Midtones,
  Highlights,
  Global,
  Count
};
constexpr size_t kGradeRangeCount = static_cast<size_t>(GradeRange::Count);
// A saturation-100 tint is the hue's color minus its own luma, scaled by this
// fraction of 255 (so the most-shifted channel moves by about 27% of 255).
constexpr float kGradeTintStrength = 0.30f;
constexpr float kGradeLumAmount = 0.30f; // lum +/-100 = +/-30% of 255

// The luma weights (Rec. 601) the table is built around: the tint is made
// luma-neutral with these, and the table is indexed by luma computed with
// them. applyColorPass must use the SAME weights to pick the table row, or the
// tint will shift brightness and read the wrong entry.
constexpr float kGradeLumaR = 0.299f;
constexpr float kGradeLumaG = 0.587f;
constexpr float kGradeLumaB = 0.114f;

struct GradeWheel
{
  float hue = 0.0f;    // 0..359 degrees
  float sat = 0.0f;    // 0..100
  float lum = 0.0f;    // -100..100
  bool visible = true; // the eye icon: preview this wheel on/off
  bool neutral() const { return sat == 0.0f && lum == 0.0f; }
  bool operator==(const GradeWheel &) const = default;
};

struct ColorGrade
{
  std::array<GradeWheel, kGradeRangeCount> wheel{};
  float blending = 50.0f; // 0..100
  float balance = 0.0f;   // -100..100
  bool operator==(const ColorGrade &) const = default;
  bool anyActive() const
  {
    for (const auto &w : wheel)
      if (w.visible && !w.neutral())
        return true;
    return false;
  }
};

// Per-luma RGB offsets (in 0..255 units), indexed by 8-bit luma.
using GradeLut = std::array<std::array<float, 256>, 3>;

// Fully saturated RGB (0..1) for a hue in degrees. Any hue is fine (it wraps);
// a NaN or infinite hue is treated as 0.
inline void gradeHueToRgb(float h, float *rgb)
{
  if (!std::isfinite(h))
    h = 0.0f;
  h = std::fmod(h, 360.0f);
  if (h < 0.0f)
    h += 360.0f;
  const float hp = h / 60.0f;
  const float x = 1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f);
  switch (static_cast<int>(hp) % 6)
  {
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

// Clamp to [lo, hi]; NaN/inf become `def` (the neutral value).
inline float gradeClampOr(float v, float lo, float hi, float def)
{
  return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
}

inline GradeLut buildGradeLut(const ColorGrade &g)
{
  // Tint vector + luminance offset per wheel. The tint is the hue's color
  // minus its own luma, so it shifts color without changing brightness.
  float tint[kGradeRangeCount][3] = {};
  float lumOff[kGradeRangeCount] = {};
  for (size_t r = 0; r < kGradeRangeCount; ++r)
  {
    const GradeWheel &w = g.wheel[r];
    if (!w.visible)
      continue;
    const float sat = gradeClampOr(w.sat, 0.0f, 100.0f, 0.0f);
    const float lum = gradeClampOr(w.lum, -100.0f, 100.0f, 0.0f);
    float c[3];
    gradeHueToRgb(w.hue, c);
    const float l = kGradeLumaR * c[0] + kGradeLumaG * c[1] + kGradeLumaB * c[2];
    const float k = sat / 100.0f * kGradeTintStrength * 255.0f;
    for (int chan = 0; chan < 3; ++chan)
      tint[r][chan] = (c[chan] - l) * k;
    lumOff[r] = lum / 100.0f * kGradeLumAmount * 255.0f;
  }

  const float blending = gradeClampOr(g.blending, 0.0f, 100.0f, 50.0f);
  const float balance = gradeClampOr(g.balance, -100.0f, 100.0f, 0.0f);

  // Crossovers: shadows/mids at pivot - 0.25, mids/highlights at pivot + 0.25.
  // Positive balance lowers the pivot (crossovers move toward the shadows, so
  // the highlights wheel gets more of the image). Blending widens the
  // smoothstep around each; hw is always >= 0.05, so the edges never collapse.
  const float pivot = 0.5f - 0.2f * (balance / 100.0f);
  const float crossLo = pivot - 0.25f, crossHi = pivot + 0.25f;
  const float hw = 0.05f + 0.30f * (blending / 100.0f);

  GradeLut lut{};                     // tint part of the offsets, per channel
  std::array<float, 256> lumCurve{};  // luminance part (same for R, G, B)
  for (size_t i = 0; i < 256; ++i)
  {
    const float y = static_cast<float>(i) / 255.0f;
    float wS = 1.0f - smoothStep01(crossLo - hw, crossLo + hw, y);
    float wH = smoothStep01(crossHi - hw, crossHi + hw, y);
    float wM = 1.0f - wS - wH;
    if (wM < 0.0f)
      wM = 0.0f;
    const float sum = wS + wM + wH; // > 1 only when the ranges overlap a lot
    if (sum > 1.0f)
    {
      wS /= sum;
      wM /= sum;
      wH /= sum;
    }
    const float wt[kGradeRangeCount] = {wS, wM, wH, 1.0f};
    for (size_t r = 0; r < kGradeRangeCount; ++r)
    {
      for (size_t c = 0; c < 3; ++c)
        lut[c][i] += wt[r] * tint[r][c];
      lumCurve[i] += wt[r] * lumOff[r];
    }
  }

  // Keep the luminance curve from inverting tones. The output level is
  // i + lumCurve[i]; with large offsets and low Blending the weights change so
  // fast that this can slope downward (darker input -> brighter output). Hold
  // it at a plateau instead. Settings that were already monotonic are
  // untouched (their offsets are copied through unchanged).
  float floorY = -1e30f;
  for (size_t i = 0; i < 256; ++i)
  {
    const float outY = static_cast<float>(i) + lumCurve[i];
    float off = lumCurve[i];
    if (outY < floorY)
      off = floorY - static_cast<float>(i);
    else
      floorY = outY;
    for (size_t c = 0; c < 3; ++c)
      lut[c][i] += off;
  }
  return lut;
}