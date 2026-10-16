// color_mix.hpp - Color Mixer (HSL): eight hue bands x (Hue, Saturation, Luminance),
// baked into three per-hue tables (MixLut). Pure math; the band display
// colors used by the sliders stay with the UI.
#pragma once
#include <array>
#include <cstddef>

#include "pixel_math.hpp"

// Color Mixer (HSL): 8 hue bands x (Hue, Saturation, Luminance), each slider
// -100..+100 (0 = untouched). See the "colormix" block below.
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
// render request. The per-pixel work is then: RGB -> HSL, three table reads,
// HSL -> RGB (see applyColorPass).
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
constexpr size_t kMixBandCount = static_cast<size_t>(MixBand::Count);
constexpr size_t kMixChanCount = static_cast<size_t>(MixChan::Count);

// Flat storage: channel-major, so index = chan * kMixBandCount + band.
using MixValues = std::array<float, kMixBandCount * kMixChanCount>;
constexpr size_t mixIndex(MixChan c, size_t band)
{
  return static_cast<size_t>(c) * kMixBandCount + band;
}

// Hue (degrees) at the middle of each band; the last entry closes the circle.
constexpr std::array<float, kMixBandCount + 1> kMixCenters = {
    0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 320.0f, 360.0f};

inline const char *mixBandName(size_t band)
{
  static const char *const names[kMixBandCount] = {
      "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
  return names[band];
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
  for (size_t b = 0; b < kMixBandCount; ++b)
    if (m[mixIndex(c, b)] != 0.0f)
      return true;
  return false;
}

// Per-hue results, indexed by round(hue / 360 * kMixLutSize).
struct MixLut
{
  std::array<float, kMixLutSize> hueShift{}; // degrees to add to the hue
  std::array<float, kMixLutSize> satMul{};   // multiplier on HSL saturation
  std::array<float, kMixLutSize> lumAdj{};   // -1..+1 luminance amount
};

inline MixLut buildMixLut(const MixValues &m)
{
  MixLut lut;
  for (int i = 0; i < kMixLutSize; ++i)
  {
    const float hue = static_cast<float>(i) * 360.0f / kMixLutSize;
    size_t j = 0; // segment [kMixCenters[j], kMixCenters[j + 1])
    while (j < kMixBandCount - 1 && hue >= kMixCenters[j + 1])
      ++j;
    const size_t nx = (j + 1) % kMixBandCount; // Magenta blends back to Red
    const float t = smoothStep01(0.0f, 1.0f,
                                 (hue - kMixCenters[j]) /
                                     (kMixCenters[j + 1] - kMixCenters[j]));
    auto blend = [&](MixChan c)
    {
      return (m[mixIndex(c, j)] * (1.0f - t) + m[mixIndex(c, nx)] * t) / 100.0f;
    };
    const size_t k = static_cast<size_t>(i);
    lut.hueShift[k] = blend(MixChan::Hue) * kMixHueRangeDeg;
    lut.satMul[k] = 1.0f + blend(MixChan::Saturation);
    lut.lumAdj[k] = blend(MixChan::Luminance);
  }
  return lut;
}