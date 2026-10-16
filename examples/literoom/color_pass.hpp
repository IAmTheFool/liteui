// color_pass.hpp - The float per-pixel color pass: white balance, vibrance, saturation,
// hue, colorize, Color Mixer, Color Grading. Runs in place on RGBA8.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "color_grade.hpp"
#include "color_mix.hpp"
#include "pixel_math.hpp"

constexpr float kHueShiftDegrees = 90.0f; // Hue slider +/-100 = +/- this many degrees
constexpr float kColorizeStrength = 0.6f; // Colorize Amount 100 = this fraction of the tint's chroma

// ---------------- Color pass (float, per pixel) ----------------
// Runs on the LUT's 8-bit output. Everything here is computed in float and
// rounded/clamped once at the end, so stacking Temperature + Vibrance +
// Saturation doesn't accumulate 8-bit rounding the way chained LUT stages do.
// Order: white balance gains -> vibrance -> saturation -> color mixer -> color
// grading. Works on
// the gamma-encoded values directly (no linearization) — a standard, cheap
// approximation for a first version.
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
  GradeLut gradeLut;                           // only filled when `grade` is true
};

// UI values are the -100..+100 slider values.
inline ColorPass makeColorPass(float temperature, float tint, float vibrance,
                               float saturation, const MixValues &mix,
                               const ColorGrade &grade,
                               float hueShift = 0.0f, float colorizeHue = 0.0f,
                               float colorizeAmount = 0.0f)
{
  ColorPass cp;
  cp.mix = mixAnyNonZero(mix);
  cp.grade = grade.anyActive();
  cp.hue = hueShift != 0.0f;
  cp.colorize = colorizeAmount > 0.0f;
  cp.active = cp.mix || cp.grade || cp.hue || cp.colorize ||
              temperature != 0.0f || tint != 0.0f || vibrance != 0.0f ||
              saturation != 0.0f;
  if (!cp.active)
    return cp;
  if (cp.hue)
  {
    // Rotation about the gray axis (the same matrix as SVG's hueRotate).
    const float a = hueShift / 100.0f * kHueShiftDegrees * 0.017453292519943295f;
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
    float h = std::fmod(colorizeHue, 360.0f);
    if (h < 0.0f)
      h += 360.0f;
    const float hp = h / 60.0f;
    const int sector = std::min(5, static_cast<int>(hp));
    const float f = hp - static_cast<float>(sector);
    float t[3];
    switch (sector)
    {
    case 0:
      t[0] = 1;
      t[1] = f;
      t[2] = 0;
      break;
    case 1:
      t[0] = 1 - f;
      t[1] = 1;
      t[2] = 0;
      break;
    case 2:
      t[0] = 0;
      t[1] = 1;
      t[2] = f;
      break;
    case 3:
      t[0] = 0;
      t[1] = 1 - f;
      t[2] = 1;
      break;
    case 4:
      t[0] = f;
      t[1] = 0;
      t[2] = 1;
      break;
    default:
      t[0] = 1;
      t[1] = 0;
      t[2] = 1 - f;
      break;
    }
    const float lt = 0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2];
    const float k = colorizeAmount / 100.0f * kColorizeStrength * 255.0f;
    for (int i = 0; i < 3; ++i)
      cp.tintVec[i] = (t[i] - lt) * k;
  }
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

// Applies `cp` in place to `n` bytes of RGBA8 (alpha untouched). Raw pointers
// and no std::clamp/min/max calls in the loop, for the same Debug-build
// reason as AdjustmentSet::applyTo.
inline void applyColorPass(const ColorPass &cp, uint8_t *p, size_t n)
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
  const float *gdR = cp.gradeLut[0].data();
  const float *gdG = cp.gradeLut[1].data();
  const float *gdB = cp.gradeLut[2].data();
  const float vib = cp.vibrance, sat = cp.saturation;
  const float *lutHue = cp.mixLut.hueShift.data();
  const float *lutSat = cp.mixLut.satMul.data();
  const float *lutLum = cp.mixLut.lumAdj.data();
  for (size_t i = 0; i + 3 < n; i += 4)
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
      const float l = 0.299f * r + 0.587f * g + 0.114f * b;
      r = l + (r - l) * k;
      g = l + (g - l) * k;
      b = l + (b - l) * k;
    }
    if (doSat)
    {
      const float l = 0.299f * r + 0.587f * g + 0.114f * b;
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
      float l = (0.299f * r + 0.587f * g + 0.114f * b) * (1.0f / 255.0f);
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