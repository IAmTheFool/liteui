// mask_range.hpp - The two mask kinds that select by the picture's own values: Luminosity
// range and Color Range. Neither is spatial, so neither maps through a
// GeoFrame.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/geometry.hpp"

// Luminosity range masks.
constexpr float kLumMaxRamp = 0.5f; // soft edge width at Smoothness = 100%,
                                    // as a fraction of the 0..1 luminance

// Color Range: distance (in the opponent space of ColorRangeShape) that
// Range = 100% reaches, and how much brightness differences count.
constexpr float kColorRangeMaxDist = 0.75f;
constexpr float kColorRangeLumWeight = 0.35f;

// ---- Luminosity range mask ----
// Selects by brightness instead of by place: pixels whose luminance is inside
// [lo, hi] get full weight, easing to zero over a soft edge OUTSIDE each end.
// It is evaluated on the picture as it stands when the mask is reached (after
// the global adjustments and the masks listed before it) but before its own
// adjustments, so darkening the shadows does not change which pixels are
// "shadows". There is nothing spatial about it, so it needs no GeoFrame
// mapping: the in-view form is the stored form.
struct LuminosityEval
{
  // Weight for each 8-bit luminance, built once per render request.
  std::array<float, 256> lut{};

  // Every pixel can be selected, so every row is in play.
  bool rowSpan(int /*y*/, int W, int &x0, int &x1) const
  {
    x0 = 0;
    x1 = W;
    return W > 0;
  }

  // Rec. 709 luma in integers (54 + 183 + 19 = 256). Plain loop, no std
  // helpers, same Debug-build reason as RadialEval.
  void rowWeights(int /*y*/, int x0, int x1, float *out,
                  const uint8_t *pix = nullptr) const
  {
    if (!pix)
    { // cannot happen through the pipeline; stay safe
      for (int x = x0; x < x1; ++x)
        out[x - x0] = 0.0f;
      return;
    }
    const float *t = lut.data();
    const int len = x1 - x0;
    for (int i = 0; i < len; ++i)
    {
      const uint8_t *p = pix + static_cast<size_t>(i) * 4;
      const int l = (54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8;
      out[i] = t[l];
    }
  }
};

struct LuminosityShape
{
  float lo = 0.0f;      // luminance (0..1) where full weight begins
  float hi = 0.35f;     // ...and ends. A new mask picks out the shadows.
  float feather = 0.4f; // 0 = hard cut .. 1 = widest soft edge
  bool operator==(const LuminosityShape &) const = default;

  // The range's two ends push each other: Min can never pass Max, nor Max go
  // below Min (the one being dragged wins, the other follows).
  void setLo(float v)
  {
    lo = std::clamp(v, 0.0f, 1.0f);
    if (hi < lo)
      hi = lo;
  }
  void setHi(float v)
  {
    hi = std::clamp(v, 0.0f, 1.0f);
    if (lo > hi)
      lo = hi;
  }

  // Not spatial: nothing to map through the frame.
  LuminosityShape toSource(const GeoFrame &) const { return *this; }
  void applySourceEdit(const LuminosityShape & /*before*/,
                       const LuminosityShape &after, const GeoFrame &)
  {
    *this = after;
  }

  LuminosityEval prepare(int /*W*/, int /*H*/) const
  {
    LuminosityEval e;
    const float a = std::clamp(std::min(lo, hi), 0.0f, 1.0f);
    const float b = std::clamp(std::max(lo, hi), 0.0f, 1.0f);
    const float f = std::clamp(feather, 0.0f, 1.0f) * kLumMaxRamp;
    const auto smooth = [](float x)
    {
      x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
      return x * x * (3.0f - 2.0f * x);
    };
    for (size_t i = 0; i < 256; ++i)
    {
      const float t = static_cast<float>(i) / 255.0f;
      float w;
      if (f <= 1e-4f)
      {
        w = (t >= a && t <= b) ? 1.0f : 0.0f;
      }
      else
      {
        const float up = smooth((t - (a - f)) / f);  // rises into [a, b]
        const float dn = 1.0f - smooth((t - b) / f); // falls after b
        w = up * dn;
      }
      e.lut[i] = w;
    }
    return e;
  }
};

// ---- Color Range mask ----
// Selects pixels whose color is close to a sampled one. Like Luminosity it is
// not spatial (no GeoFrame mapping) and reads the row's pixels as they are
// when the mask is reached. Distance is measured in a cheap opponent space:
//   y = luma, cb = b - y, cr = r - y   (all 0..1)
// d = sqrt((wl*dy)^2 + dcb^2 + dcr^2). Full weight inside the inner radius,
// smoothstep to 0 at the outer one.
struct ColorRangeEval
{
  bool set = false;
  float y0 = 0, cb0 = 0, cr0 = 0; // the sample
  float inner = 0, outer = 0;     // distance where the falloff begins / ends
  float invFall = 1;

  bool rowSpan(int /*y*/, int W, int &x0, int &x1) const
  {
    x0 = 0;
    x1 = W;
    return set && W > 0;
  }

  void rowWeights(int /*y*/, int x0, int x1, float *out,
                  const uint8_t *pix = nullptr) const
  {
    const int len = x1 - x0;
    if (!pix || !set)
    {
      for (int i = 0; i < len; ++i)
        out[i] = 0.0f;
      return;
    }
    const float k = 1.0f / 255.0f;
    for (int i = 0; i < len; ++i)
    {
      const uint8_t *p = pix + static_cast<size_t>(i) * 4;
      const float r = p[0] * k, g = p[1] * k, b = p[2] * k;
      const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
      const float dy = (y - y0) * kColorRangeLumWeight;
      const float dcb = (b - y) - cb0;
      const float dcr = (r - y) - cr0;
      const float d = std::sqrt(dy * dy + dcb * dcb + dcr * dcr);
      float w;
      if (d <= inner)
        w = 1.0f;
      else if (d >= outer)
        w = 0.0f;
      else
      {
        const float t = (d - inner) * invFall;
        w = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      out[i] = w;
    }
  }
};

struct ColorRangeShape
{
  bool set = false;          // false until a color was clicked
  float r = 0, g = 0, b = 0; // the sample, 0..1 (as it was when clicked)
  float range = 0.30f;       // 0..1 tolerance
  float feather = 0.5f;      // 0 = hard edge .. 1 = widest soft edge
  bool operator==(const ColorRangeShape &) const = default;

  ColorRangeShape toSource(const GeoFrame &) const { return *this; }
  void applySourceEdit(const ColorRangeShape & /*before*/,
                       const ColorRangeShape &after, const GeoFrame &)
  {
    *this = after;
  }

  ColorRangeEval prepare(int /*W*/, int /*H*/) const
  {
    ColorRangeEval e;
    e.set = set;
    const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    e.y0 = y;
    e.cb0 = b - y;
    e.cr0 = r - y;
    e.outer = std::clamp(range, 0.0f, 1.0f) * kColorRangeMaxDist;
    e.inner = e.outer * (1.0f - std::clamp(feather, 0.0f, 1.0f));
    e.invFall = 1.0f / std::max(1e-4f, e.outer - e.inner);
    return e;
  }
};
