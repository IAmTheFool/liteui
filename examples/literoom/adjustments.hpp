// adjustments.hpp - The Adjust sliders: AdjId, the AdjustmentSpec table (range + tone
// function per slider) and AdjValues (one value per slider, used by the
// global edit set and by every mask). No UI, no pixels.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <string>

#include "pixel_math.hpp"

// Exposure range (percentage; scales each R/G/B channel multiplicatively,
// applied before brightness's flat offset — see kAdjustmentSpecs below).
constexpr int kExposureMin = -100;
constexpr int kExposureMax = 100;

// Brightness range (flat offset added to each R/G/B channel).
constexpr int kBrightnessMin = -100;
constexpr int kBrightnessMax = 100;

// Contrast / Highlights / Shadows / Whites / Blacks all share one -100..+100
// range (0 = untouched).
constexpr int kToneMin = -100;
constexpr int kToneMax = 100;

// ---------------- Adjustments ----------------
// Every slider in the "Adjust" pane is one entry in kAdjustmentSpecs below.
// Adding a new one (Contrast, Saturation, ...) means adding one entry to
// that table — AdjustmentSet, the sidebar loop, and the pixel pipeline all
// pick it up automatically; nothing else needs to change.

// Order must match kAdjustmentSpecs below (it is indexed by AdjId).
enum class AdjId : int
{
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
  Hue,
  ColorizeHue,
  ColorizeAmount,
  Texture,
  Clarity,
  Sharpness,
  Dehaze,
  NoiseLuma,
  NoiseColor,
  Blur,
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
//   Stage::Detail — needs neighbor pixels (Texture, Clarity, Sharpness).
//                  `apply` is unused (null) here too: these are consumed by
//                  detail_layer::addPixels (detail_layer.hpp), which adds a share of
//                  "pixel minus its blur" after the color pass.
// The slider UI, undo/coalescing, and preview path treat all stages alike.
struct AdjustmentSpec
{
  enum class Stage
  {
    Tone,
    Color,
    Detail
  };
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
inline const std::array<AdjustmentSpec, static_cast<size_t>(AdjId::Count)>
    kAdjustmentSpecs = {{
        {AdjId::Exposure, "Exposure", static_cast<float>(kExposureMin),
         static_cast<float>(kExposureMax),
         [](float x, float value)
         { return x * (1.0f + value / 100.0f); }},
        {AdjId::Brightness, "Brightness", static_cast<float>(kBrightnessMin),
         static_cast<float>(kBrightnessMax),
         [](float x, float value)
         { return x + std::round(value); }},
        // Contrast: linear stretch/squash around mid-gray. -100 flattens to
        // gray, +100 doubles the distance from 128 (clipping the ends).
        {AdjId::Contrast, "Contrast", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value)
         {
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
         [](float x, float value)
         {
           return x + value * 0.6f * smoothStep01(0.4f, 1.0f, x / 255.0f);
         }},
        {AdjId::Shadows, "Shadows", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value)
         {
           return x +
                  value * 0.6f * (1.0f - smoothStep01(0.0f, 0.6f, x / 255.0f));
         }},
        {AdjId::Whites, "Whites", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value)
         {
           return x + value * 0.4f * smoothStep01(0.75f, 1.0f, x / 255.0f);
         }},
        {AdjId::Blacks, "Blacks", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax),
         [](float x, float value)
         {
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
        // Hue: rotates all colors around the gray axis. Colorize: adds a
        // luma-neutral cast of ColorizeHue, strength ColorizeAmount (0 = off).
        {AdjId::Hue, "Hue", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::ColorizeHue, "Colorize Hue", 0.0f, 360.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::ColorizeAmount, "Colorize Amount", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        // Detail stage (see detail_layer.hpp). Texture and Clarity soften when
        // negative; Sharpness only sharpens.
        {AdjId::Texture, "Texture", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Detail},
        {AdjId::Clarity, "Clarity", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Detail},
        {AdjId::Sharpness, "Sharpness", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        // Dehaze removes (+) or adds (-) atmospheric haze; see neighbor_fx.hpp.
        {AdjId::Dehaze, "Dehaze", static_cast<float>(kToneMin),
         static_cast<float>(kToneMax), nullptr, AdjustmentSpec::Stage::Detail},
        // Noise reduction: luminance (grain) and color (blotches), 0..100.
        {AdjId::NoiseLuma, "Noise Reduction", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        {AdjId::NoiseColor, "Color Noise", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        // Blur: a color blur from none to very soft, 0..100 (mostly for masks).
        {AdjId::Blur, "Blur", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
    }};

using AdjValues = std::array<float, static_cast<size_t>(AdjId::Count)>;

// The Tone-stage sliders composed into one 256-entry table (per-channel, so
// the same table serves R, G and B), with an optional curve table applied
// last. Shared by the global pipeline (AdjustmentSet::buildLut) and masks.
inline std::array<uint8_t, 256>
toneLutFromValues(const AdjValues &values,
                  const std::array<float, 256> *curveLut)
{
  std::array<uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i)
  {
    float x = static_cast<float>(i);
    for (const auto &spec : kAdjustmentSpecs)
    {
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