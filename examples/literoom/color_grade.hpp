// color_grade.hpp - Color Grading: 3-way + global wheels baked into one per-luma table
// of RGB offsets (GradeLut). Pure math.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>

#include "pixel_math.hpp"

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
enum class GradeRange : int
{
  Shadows,
  Midtones,
  Highlights,
  Global,
  Count
};
constexpr size_t kGradeRangeCount = static_cast<size_t>(GradeRange::Count);
constexpr float kGradeTintStrength = 0.30f; // sat 100 shifts a channel by 30%
constexpr float kGradeLumAmount = 0.30f;    // lum +/-100 = +/-30% of 255

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

// Fully saturated RGB (0..1) for a hue in degrees.
inline void gradeHueToRgb(float h, float *rgb)
{
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
  for (int i = 0; i < 256; ++i)
  {
    const float y = static_cast<float>(i) / 255.0f;
    float wS = 1.0f - smoothStep01(cs - hw, cs + hw, y);
    float wH = smoothStep01(ch - hw, ch + hw, y);
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
      for (int c = 0; c < 3; ++c)
        lut[c][i] += wt[r] * (tint[r][c] + lumOff[r]);
  }
  return lut;
}