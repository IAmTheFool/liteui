// color_mix.hpp - Color Mixer (HSL): eight hue bands x (Hue, Saturation, Luminance),
// baked into three per-hue tables (MixLut). Pure math; the band display
// colors used by the sliders stay with the UI.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include "pixel_math.hpp"

// Color Mixer (HSL): 8 hue bands x (Hue, Saturation, Luminance), each slider
// -100..+100 (0 = untouched). See the "Color Mixer (HSL)" block below.
constexpr int kMixMin = -100;
constexpr int kMixMax = 100;
constexpr float kMixHueRangeDeg = 30.0f;  // hue slider at +/-100 = +/-30 degrees
constexpr float kMixLumAmount = 0.5f;     // luminance +100 = halfway to white
constexpr float kMixChromaFull = 0.10f;   // chroma (0..1) at which a pixel is
                                          // "colored enough" for full effect
constexpr int kMixLutSize = 720;          // hue lookup resolution (0.5 degree)

// ---------------- Color Mixer (HSL) ----------------
// Lightroom-style per-color adjustment: eight hue bands, each with a Hue,
// Saturation and Luminance slider. A pixel's own hue decides how much of each
// band's setting it gets: between two neighboring band centers the two
// settings are blended with a smoothstep, so a pixel exactly at a center gets
// that band's value alone and there are no seams between bands.
//
// Because the result depends only on a pixel's hue, all of that blending is
// baked into three small tables indexed by hue (MixLut), built once per
// render request. The per-pixel work is then: RGB -> HSL, three table reads
// (index via mixLutIndex), HSL -> RGB (see applyColorPass).
//
// Slider values are sanitized when the table is built (clamped to
// kMixMin..kMixMax, NaN treated as 0), so the tables are always finite and
// satMul stays within 0..2.
enum class MixBand : int
{
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
enum class MixChan : int
{
  Hue,
  Saturation,
  Luminance,
  Count
};
constexpr std::size_t kMixBandCount = static_cast<std::size_t>(MixBand::Count);
constexpr std::size_t kMixChanCount = static_cast<std::size_t>(MixChan::Count);

// Flat storage: channel-major, so index = chan * kMixBandCount + band.
using MixValues = std::array<float, kMixBandCount * kMixChanCount>;
constexpr std::size_t mixIndex(MixChan c, std::size_t band)
{
  return static_cast<std::size_t>(c) * kMixBandCount + band;
}

// All sliders untouched.
inline MixValues defaultMixValues() { return MixValues{}; }

// Hue (degrees) at the middle of each band; the last entry closes the circle.
constexpr std::array<float, kMixBandCount + 1> kMixCenters = {
    0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 320.0f, 360.0f};

// kMixCenters must start at 0, end at 360 and be strictly increasing: a
// zero-length segment would make buildMixLut divide by zero. Checked at
// compile time, so adding a band without extending the array fails the build.
constexpr bool mixCentersValid()
{
  if (kMixCenters.front() != 0.0f || kMixCenters.back() != 360.0f)
    return false;
  for (std::size_t i = 0; i + 1 < kMixCenters.size(); ++i)
    if (!(kMixCenters[i] < kMixCenters[i + 1]))
      return false;
  return true;
}
static_assert(mixCentersValid(),
              "kMixCenters must start at 0, end at 360 and be strictly "
              "increasing (one entry per band, plus the closing 360)");

inline const char *mixBandName(std::size_t band)
{
  static const char *const names[kMixBandCount] = {
      "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
  return band < kMixBandCount ? names[band] : "";
}

inline bool mixAnyNonZero(const MixValues &m)
{
  for (float v : m)
    if (v != 0.0f)
      return true;
  return false;
}
inline bool mixChanAnyNonZero(const MixValues &m, MixChan c)
{
  for (std::size_t b = 0; b < kMixBandCount; ++b)
    if (m[mixIndex(c, b)] != 0.0f)
      return true;
  return false;
}

// A slider value made safe to use: NaN/inf -> 0 (untouched), otherwise clamped
// to the slider range.
inline float mixClamped(float v)
{
  return std::isfinite(v)
             ? std::clamp(v, static_cast<float>(kMixMin),
                          static_cast<float>(kMixMax))
             : 0.0f;
}

// Per-hue results, indexed by mixLutIndex(hue).
struct MixLut
{
  std::array<float, kMixLutSize> hueShift{}; // degrees to add to the hue
  std::array<float, kMixLutSize> satMul{};   // multiplier on HSL saturation
  std::array<float, kMixLutSize> lumAdj{};   // -1..+1 luminance amount
};

// The table index for a hue in degrees: nearest entry, wrapping around the
// circle (359.9 -> 0, -0.1 -> 719), always in 0..kMixLutSize-1. A NaN or
// infinite hue maps to 0. Always use this instead of rounding by hand, since
// round(hue / 360 * kMixLutSize) reaches kMixLutSize for hues near 360.
inline std::size_t mixLutIndex(float hueDeg)
{
  if (!std::isfinite(hueDeg))
    return 0;
  float h = std::fmod(hueDeg, 360.0f);
  if (h < 0.0f)
    h += 360.0f;
  const std::size_t i =
      static_cast<std::size_t>(h * (kMixLutSize / 360.0f) + 0.5f);
  return i % static_cast<std::size_t>(kMixLutSize); // entry 720 == entry 0
}

inline MixLut buildMixLut(const MixValues &m)
{
  MixLut lut;
  for (int i = 0; i < kMixLutSize; ++i)
  {
    const float hue = static_cast<float>(i) * 360.0f / kMixLutSize;
    std::size_t j = 0; // segment [kMixCenters[j], kMixCenters[j + 1])
    while (j < kMixBandCount - 1 && hue >= kMixCenters[j + 1])
      ++j;
    const std::size_t nx = (j + 1) % kMixBandCount; // Magenta blends back to Red
    const float t = smoothStep01(0.0f, 1.0f,
                                 (hue - kMixCenters[j]) /
                                     (kMixCenters[j + 1] - kMixCenters[j]));
    auto blend = [&](MixChan c)
    {
      return (mixClamped(m[mixIndex(c, j)]) * (1.0f - t) +
              mixClamped(m[mixIndex(c, nx)]) * t) /
             100.0f;
    };
    const std::size_t k = static_cast<std::size_t>(i);
    lut.hueShift[k] = blend(MixChan::Hue) * kMixHueRangeDeg;
    lut.satMul[k] = 1.0f + blend(MixChan::Saturation);
    lut.lumAdj[k] = blend(MixChan::Luminance);
  }
  return lut;
}