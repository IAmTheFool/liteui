// adjustments.hpp - The Adjust sliders: AdjId, the AdjustmentSpec table (range + tone
// function per slider) and AdjValues (one value per slider, used by the
// global edit set and by every mask). No UI, no pixels.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/pixel_math.hpp"

// Levels. Black Point / White Point are 0..100 "levels" moved in from either
// end of the 0..255 range; the limits keep the stretch denominators (255 - 100)
// well above zero, so both curves stay finite and increasing. Gamma is bipolar:
// +100 brightens the midtones (exponent 2^-2 = 0.25), -100 darkens them
// (exponent 2^2 = 4). Gamma = 0 is exactly the identity.
constexpr int kBlackPointMax = 100;
constexpr int kWhitePointMax = 100;
constexpr float kGammaOctaves = 2.0f; // exponent = 2^(-value / 100 * kGammaOctaves)

// Exposure range (percentage; scales each R/G/B channel multiplicatively,
// applied before brightness's flat offset — see kAdjustmentSpecs below).
constexpr int kExposureMin = -100;
constexpr int kExposureMax = 100;

// Brightness range (flat offset added to each R/G/B channel).
constexpr int kBrightnessMin = -100;
constexpr int kBrightnessMax = 100;

// The shared -100..+100 range (0 = untouched) used by Contrast, Highlights,
// Shadows, Whites, Blacks, and also by Temperature, Tint, Vibrance, Saturation,
// Hue, Texture, Clarity and Dehaze (and Gamma).
constexpr int kBipolarMin = -100;
constexpr int kBipolarMax = 100;
// Older names for the same range; kept so existing code keeps compiling.
constexpr int kToneMin = kBipolarMin;
constexpr int kToneMax = kBipolarMax;

// ---------------- Adjustments ----------------
// Every slider in the "Adjust" pane is one entry in kAdjustmentSpecs below.
// Adding a new one means adding an AdjId and one entry to that table (the
// static_assert after the table fails the build if the two don't line up).
// AdjustmentSet, the sidebar loop and the Tone-stage LUT pick it up
// automatically; a Color or Detail slider also needs its pass (ColorPass,
// detail_layer, ...) to read its value.

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
  // Levels, as three Tone sliders: BlackPoint and WhitePoint clip and stretch
  // the input range, Gamma bends the midtones. They run last in the Tone
  // block (after Blacks), in this order.
  BlackPoint,
  WhitePoint,
  Gamma,
  Temperature,
  Tint,
  Vibrance,
  Saturation,
  Hue,
  ColorizeHue,
  ColorizeAmount,
  // Filters: strength 0..100 each (0 = off). Still Stage::Color; the "look"
  // block at the end of the color pass. Keep them inside the Color block.
  Greyscale,
  Sepia,
  Vintage,
  Fade,
  Negative,
  Posterize,
  Solarize,
  BleachBypass,
  Threshold,
  CrossProcess,
  Texture,
  Clarity,
  Sharpness,
  Dehaze,
  NoiseLuma,
  NoiseColor,
  Blur,
  Count
};

constexpr size_t adjIndex(AdjId id) { return static_cast<size_t>(id); }

// One slider's identity, range, and per-channel transform. `apply` takes a
// single channel's 0..255 value (not the whole image) and returns the
// transformed value, unclamped and unrounded — toneLutFromValues below rounds
// and clamps after each spec while composing them into one lookup table. This
// means every Tone adjustment must be pointwise per-channel: it can't be
// written this way if it needs neighbor-pixel access.
//
// Three stages:
//   Stage::Tone   — pointwise per-channel; `apply` is composed into the shared
//                   LUT (Exposure ... Blacks). Runs first.
//   Stage::Color  — needs per-channel gains or cross-channel math
//                   (Temperature, Tint, Vibrance, Saturation, Hue, Colorize).
//                   `apply` is unused (null); these are consumed by the float
//                   per-pixel pass built in ColorPass / applyColorPass, which
//                   runs after the LUT and rounds to 8 bits only once at the
//                   end.
//   Stage::Detail — needs neighbor pixels (Texture, Clarity, Sharpness,
//                   Dehaze, noise reduction, Blur). `apply` is unused (null)
//                   here too: these are consumed by the neighbor-pixel passes
//                   after the color pass (see detail_layer.hpp and
//                   neighbor_fx.hpp).
// The slider UI, undo/coalescing, and preview path treat all stages alike.
//
// `apply` is a plain function pointer (a captureless lambda converts to it) and
// `label` a string_view over a literal, so the table below is constant-
// initialized: no startup code, no static-initialization-order hazards.
struct AdjustmentSpec
{
  enum class Stage
  {
    Tone,
    Color,
    Detail
  };
  AdjId id;
  std::string_view label;
  float min, max;
  float (*apply)(float channelValue, float value);
  Stage stage = Stage::Tone; // trailing default: Tone entries omit it
};

// Declaration order here is the sidebar's display order. It is also the
// processing order within the Tone stage — each Tone spec's `apply` runs on the
// previous one's output. Across stages the order is fixed: Tone (via the LUT),
// then Color, then Detail. Exposure before Brightness mirrors Lightroom's own
// Basic panel ordering, and keeps exposure's multiplicative scale from being
// thrown off by an offset that was meant to be applied after it.
inline constexpr std::array<AdjustmentSpec, adjIndex(AdjId::Count)>
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
        {AdjId::Contrast, "Contrast", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           return 128.0f + (x - 128.0f) * (1.0f + value / 100.0f);
         }},
        // Highlights / Shadows / Whites / Blacks: add up to +/- (scale * 100)
        // levels, weighted by a smooth tonal-range mask so the rest of the
        // tonal range is left alone. Positive always brightens the range.
        // Amount and band width are chosen so the curve stays monotonic
        // (max |slope of weight| * amount / 255 < 1) at both extremes. The
        // margin is thinnest for Whites/Blacks (about 0.94), so raising their
        // 0.4 scale much past 0.42 would break monotonicity.
        // NOTE: applied per channel via the shared LUT, so it is a tone-curve
        // approximation, not a luminance-preserving recovery.
        {AdjId::Highlights, "Highlights", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           return x + value * 0.6f * smoothStep01(0.4f, 1.0f, x / 255.0f);
         }},
        {AdjId::Shadows, "Shadows", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           return x +
                  value * 0.6f * (1.0f - smoothStep01(0.0f, 0.6f, x / 255.0f));
         }},
        {AdjId::Whites, "Whites", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           return x + value * 0.4f * smoothStep01(0.75f, 1.0f, x / 255.0f);
         }},
        {AdjId::Blacks, "Blacks", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           return x +
                  value * 0.4f * (1.0f - smoothStep01(0.0f, 0.25f, x / 255.0f));
         }},
        // Levels. Pointwise per channel, so Tone sliders. Each is the identity
        // at 0 (early return: no pow(x, 1) or x / 255 * 255 float noise).
        // Black Point: input levels 0..value become black, the rest is
        // stretched back to 255. Linear with slope 255 / (255 - value) >= 1,
        // so monotonic over the whole range (value <= 100 < 255). Negative
        // results are clamped by the LUT builder.
        {AdjId::BlackPoint, "Black Point", 0.0f,
         static_cast<float>(kBlackPointMax),
         [](float x, float value)
         {
           if (value == 0.0f)
             return x;
           return (x - value) * (255.0f / (255.0f - value));
         }},
        // White Point: input level 255 - value becomes white, everything above
        // clips. Linear with slope 255 / (255 - value), monotonic.
        {AdjId::WhitePoint, "White Point", 0.0f,
         static_cast<float>(kWhitePointMax),
         [](float x, float value)
         {
           if (value == 0.0f)
             return x;
           return x * (255.0f / (255.0f - value));
         }},
        // Gamma: 255 * (x / 255)^e with e = 2^(-value / 100 * kGammaOctaves),
        // so positive brightens the midtones and 0 and 255 never move.
        // Monotonic for any e > 0; the input is floored at 0 first so pow never
        // sees a negative base (NaN). The LUT builder maps a NaN to 0 anyway.
        {AdjId::Gamma, "Gamma", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax),
         [](float x, float value)
         {
           if (value == 0.0f)
             return x;
           const float e = std::exp2(-value / 100.0f * kGammaOctaves);
           return 255.0f * std::pow(std::max(x, 0.0f) / 255.0f, e);
         }},
        // Color stage (see ColorPass below). Positive Temperature = warmer,
        // positive Tint = more magenta (negative = greener).
        {AdjId::Temperature, "Temperature", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Tint, "Tint", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Vibrance, "Vibrance", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::Saturation, "Saturation", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Color},
        // Hue: rotates all colors around the gray axis (how -100..100 maps to
        // an angle is up to ColorPass). Colorize: adds a luma-neutral cast of
        // ColorizeHue (degrees), strength ColorizeAmount (0 = off).
        {AdjId::Hue, "Hue", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Color},
        {AdjId::ColorizeHue, "Colorize Hue", 0.0f, 360.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::ColorizeAmount, "Colorize Amount", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        // Filters (see FilterPass in color_pass.hpp). Strength 0..100: the
        // result is lerp(pixel, filtered pixel, value / 100). NOTE: these
        // labels are sidecar keys; never rename them once released.
        {AdjId::Greyscale, "Greyscale", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Sepia, "Sepia", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Vintage, "Vintage", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Fade, "Fade", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Negative, "Negative", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Posterize, "Posterize", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Solarize, "Solarize", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::BleachBypass, "Bleach Bypass", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::Threshold, "Threshold", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        {AdjId::CrossProcess, "Cross Process", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Color},
        // Detail stage (see detail_layer.hpp). Texture and Clarity soften when
        // negative; Sharpness only sharpens.
        {AdjId::Texture, "Texture", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Detail},
        {AdjId::Clarity, "Clarity", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Detail},
        {AdjId::Sharpness, "Sharpness", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        // Dehaze removes (+) or adds (-) atmospheric haze; see neighbor_fx.hpp.
        {AdjId::Dehaze, "Dehaze", static_cast<float>(kBipolarMin),
         static_cast<float>(kBipolarMax), nullptr, AdjustmentSpec::Stage::Detail},
        // Noise reduction: luminance (grain) and color (blotches), 0..100.
        {AdjId::NoiseLuma, "Noise Reduction", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        {AdjId::NoiseColor, "Color Noise", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
        // Blur: a color blur from none to very soft, 0..100 (mostly for masks).
        {AdjId::Blur, "Blur", 0.0f, 100.0f, nullptr,
         AdjustmentSpec::Stage::Detail},
    }};

// Checks that the table lines up with AdjId (entry i has id i), every range is
// non-empty, and every Tone entry has an `apply`. Evaluated at compile time just
// below, so adding an AdjId without a table entry (or vice versa) fails the
// build instead of silently reading the wrong slider at run time.
constexpr bool adjustmentSpecsValid()
{
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
  {
    const AdjustmentSpec &s = kAdjustmentSpecs[i];
    if (adjIndex(s.id) != i || !(s.min < s.max))
      return false;
    if (s.stage == AdjustmentSpec::Stage::Tone && s.apply == nullptr)
      return false;
  }
  return true;
}
static_assert(adjustmentSpecsValid(),
              "kAdjustmentSpecs must have one entry per AdjId, in AdjId order, "
              "with min < max and an `apply` for every Tone entry");

using AdjValues = std::array<float, adjIndex(AdjId::Count)>;

// All sliders untouched (every slider's neutral value is 0).
inline AdjValues defaultAdjValues() { return AdjValues{}; }

// A slider value made safe to feed into a transform: NaN/inf become 0
// (untouched), and anything else is clamped to the slider's range. The tone
// curves' monotonicity was only worked out within the range, so out-of-range
// values (a corrupt sidecar, a UI bug) must not reach them.
inline float sanitizedValue(const AdjustmentSpec &spec, float v)
{
  return std::isfinite(v) ? std::clamp(v, spec.min, spec.max) : 0.0f;
}

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
      if (spec.stage != AdjustmentSpec::Stage::Tone || !spec.apply)
        continue; // color/detail-stage sliders are handled by their own passes
      const float v = sanitizedValue(spec, values[adjIndex(spec.id)]);
      if (v != 0.0f)
        // Round + clamp after EACH spec, not just once at the end, so the
        // result is bit-identical to running the specs as separate per-pixel
        // passes that each stored an 8-bit result (toU8Clamped is the shared
        // rounding rule, and also maps NaN to 0).
        x = static_cast<float>(toU8Clamped(spec.apply(x, v)));
    }
    if (curveLut) // x is an integral 0..255 here
      x = static_cast<float>(
          toU8Clamped((*curveLut)[static_cast<size_t>(x)]));
    lut[static_cast<size_t>(i)] = static_cast<uint8_t>(x);
  }
  return lut;
}