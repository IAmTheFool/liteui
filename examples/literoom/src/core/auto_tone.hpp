// auto_tone.hpp - Auto Tone: turns a luma histogram into values for Exposure,
// Contrast, Whites and Blacks. A pure function of 256 counts: no UI, no pixels,
// no editor state, so it is tested on hand-made histograms.
//
// The idea: measure the picture's tonal range as the 0.5 % and 99.5 % points
// of its luma histogram (so a few stray specks cannot decide), then pick the
// sliders that stretch that range onto a black point and a white point. The
// sliders are the real Tone-stage ones, applied in the table's order
//
//   Exposure (gain)  ->  Contrast (stretch around 128)  ->  Whites  ->  Blacks
//
// and each rounds to a byte (see toneLutFromValues), so the solver follows the
// same order and rounds the two gains the way the table does.
//
// Why not aim the median at mid-gray? A dark night scene or a bright snow scene
// is *meant* to be dark or bright. Stretching the measured range keeps that
// character and only uses the available room. A well-exposed picture comes out
// almost untouched.
//
// Brightness, Highlights, Shadows and the curves are NOT read and NOT changed:
// the histogram it is given is the picture before any tone table, so the answer
// does not depend on the sliders already set (it is the same every time).
//
// Depends on: adjustments.hpp (to check its own result through the real tone
// table). Test: auto_tone_test.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/adjustments.hpp"

// Fewer samples than this (after skipping transparent pixels) is "nothing to
// measure".
constexpr uint64_t kAutoToneMinPixels = 64;
// The black and white points are the levels below / above which this fraction
// of the pixels lie.
constexpr double kAutoToneClipFraction = 0.005;
// A picture whose 0.5 %..99.5 % range is narrower than this (levels) is flat:
// stretching it would only amplify noise.
constexpr int kAutoToneMinSpread = 16;
// Where the black and white points are sent (0..255). Not 0 / 255 on purpose:
// that leaves a little room before clipping.
constexpr float kAutoToneTargetLo = 6.0f;
constexpr float kAutoToneTargetHi = 248.0f;
// Limits for the two gains. Exposure is a factor of 1 + slider / 100, so its
// factor range is 0..2; the lower limit keeps Auto from nearly blacking a
// picture out. Contrast's factor range is 0..2 as well.
constexpr float kAutoToneExposureFactorMin = 0.5f;
constexpr float kAutoToneExposureFactorMax = 2.0f;
constexpr float kAutoToneContrastFactorMin = 0.5f;
constexpr float kAutoToneContrastFactorMax = 2.0f;
// Whites and Blacks only act in a narrow band at the ends of the range. If a
// point sits where the band's weight is below this, the slider cannot move it
// and is left at 0 instead of being pushed to its limit for no effect.
constexpr float kAutoToneMinBandWeight = 0.1f;
// After solving, the result is run through the real tone table. If the black or
// white point still ends up further than this from its target (the sliders ran
// out of range), the status is Limited.
constexpr float kAutoToneLimitedTolerance = 24.0f;

enum class AutoToneStatus
{
  Ok,       // both points reach their targets
  Limited,  // usable, but the slider ranges stopped short of a target
  Flat,     // no tonal range to stretch: nothing is changed
  NoPixels, // too few pixels to measure: nothing is changed
};

struct AutoTone
{
  // Whole slider values, within their ranges. All 0 unless usable().
  float exposure = 0.0f, contrast = 0.0f, whites = 0.0f, blacks = 0.0f;
  AutoToneStatus status = AutoToneStatus::NoPixels;
  bool usable() const
  {
    return status == AutoToneStatus::Ok || status == AutoToneStatus::Limited;
  }
};

namespace auto_tone_detail
{
  // Smallest level whose running total (from level 0 up) reaches `k` pixels.
  inline int levelFromBottom(const std::array<uint32_t, 256> &h, uint64_t k)
  {
    uint64_t sum = 0;
    for (int i = 0; i < 256; ++i)
    {
      sum += h[static_cast<size_t>(i)];
      if (sum >= k)
        return i;
    }
    return 255;
  }
  // Largest level whose running total (from level 255 down) reaches `k` pixels.
  inline int levelFromTop(const std::array<uint32_t, 256> &h, uint64_t k)
  {
    uint64_t sum = 0;
    for (int i = 255; i >= 0; --i)
    {
      sum += h[static_cast<size_t>(i)];
      if (sum >= k)
        return i;
    }
    return 0;
  }
} // namespace auto_tone_detail

// `luma`: counts per level 0..255 of the picture BEFORE the tone tables
// (histogram::Counts::l of the source picture). Only the shape matters, so
// the total number of pixels does not change the answer.
inline AutoTone solveAutoTone(const std::array<uint32_t, 256> &luma)
{
  AutoTone out; // status starts as NoPixels
  uint64_t total = 0;
  for (uint32_t c : luma)
    total += c;
  if (total < kAutoToneMinPixels)
    return out;

  const uint64_t k = std::max<uint64_t>(
      1, static_cast<uint64_t>(std::ceil(kAutoToneClipFraction *
                                         static_cast<double>(total))));
  const int lo = auto_tone_detail::levelFromBottom(luma, k);
  const int hi = auto_tone_detail::levelFromTop(luma, k);
  if (hi - lo < kAutoToneMinSpread)
  {
    out.status = AutoToneStatus::Flat;
    return out;
  }

  const float range = kAutoToneTargetHi - kAutoToneTargetLo;

  // Exposure and Contrast together would map lo -> target lo and hi -> target
  // hi exactly if both gains were free. Their combined gain is
  // range / (hi - lo); the split between them comes from keeping the black
  // point where it should land after the stretch around 128.
  const float totalGain = range / static_cast<float>(hi - lo);
  const float contrastGain =
      (totalGain * static_cast<float>(lo) + 128.0f - kAutoToneTargetLo) / 128.0f;
  float f = totalGain / contrastGain; // contrastGain >= 0.95 here

  // Exposure is applied first and rounds + clamps, so never push the white
  // point past 255 (that would clip it for good), and stay inside the limits.
  const float fMax = std::min(kAutoToneExposureFactorMax,
                               255.0f / static_cast<float>(std::max(hi, 1)));
  f = std::clamp(f, kAutoToneExposureFactorMin, fMax);
  // Whole slider value (lround, not round: never a negative zero, which a
  // History label would print as "-0"); use the factor it really gives from here on.
  out.exposure = static_cast<float>(std::lround((f - 1.0f) * 100.0f));
  f = 1.0f + out.exposure / 100.0f;

  const float lo1 = static_cast<float>(lo) * f;
  const float hi1 = std::min(255.0f, static_cast<float>(hi) * f);

  // Contrast: stretch the remaining range onto the target width, without
  // clipping either point (its pivot is 128, so the end farthest from 128
  // limits the gain).
  float g = range / std::max(hi1 - lo1, 1.0f);
  g = std::clamp(g, kAutoToneContrastFactorMin, kAutoToneContrastFactorMax);
  if (hi1 > 128.0f)
    g = std::min(g, 127.0f / (hi1 - 128.0f));
  if (lo1 < 128.0f)
    g = std::min(g, 128.0f / (128.0f - lo1));
  out.contrast = static_cast<float>(std::lround((g - 1.0f) * 100.0f));
  g = 1.0f + out.contrast / 100.0f;

  const float lo2 = std::clamp(128.0f + (lo1 - 128.0f) * g, 0.0f, 255.0f);
  const float hi2 = std::clamp(128.0f + (hi1 - 128.0f) * g, 0.0f, 255.0f);

  // Whites / Blacks: close what is left. Each adds value * 0.4 * band weight at
  // its point (see kAdjustmentSpecs), so solve for the value that moves the
  // point to its target. Where the band weight is tiny the slider is skipped.
  const float wWeight = smoothStep01(0.75f, 1.0f, hi2 / 255.0f);
  if (wWeight >= kAutoToneMinBandWeight)
    out.whites = std::clamp(
        static_cast<float>(
            std::lround((kAutoToneTargetHi - hi2) / (0.4f * wWeight))),
        static_cast<float>(kBipolarMin), static_cast<float>(kBipolarMax));
  const float bWeight = 1.0f - smoothStep01(0.0f, 0.25f, lo2 / 255.0f);
  if (bWeight >= kAutoToneMinBandWeight)
    out.blacks = std::clamp(
        static_cast<float>(
            std::lround((kAutoToneTargetLo - lo2) / (0.4f * bWeight))),
        static_cast<float>(kBipolarMin), static_cast<float>(kBipolarMax));

  // Judge the result by running it through the real tone table.
  AdjValues v = defaultAdjValues();
  v[adjIndex(AdjId::Exposure)] = out.exposure;
  v[adjIndex(AdjId::Contrast)] = out.contrast;
  v[adjIndex(AdjId::Whites)] = out.whites;
  v[adjIndex(AdjId::Blacks)] = out.blacks;
  const auto lut = toneLutFromValues(v, nullptr);
  const bool reached =
      std::fabs(static_cast<float>(lut[static_cast<size_t>(lo)]) -
                kAutoToneTargetLo) <= kAutoToneLimitedTolerance &&
      std::fabs(static_cast<float>(lut[static_cast<size_t>(hi)]) -
                kAutoToneTargetHi) <= kAutoToneLimitedTolerance;
  out.status = reached ? AutoToneStatus::Ok : AutoToneStatus::Limited;
  return out;
}
