// tone_table.hpp - The Tone stage for the float pipeline: the same sliders and curves as
// toneLutFromValues (adjustments.hpp), composed into ONE table of floats instead of 256 bytes,
// and WITHOUT the rounding after every slider.
//
// toneLutFromValues rounds to a byte after each Tone slider (so that one table equals separate
// 8-bit passes). In float that rounding is gone: each slider's function is applied to the
// previous one's float result, clamped to 0..255 (the clamp is kept: it is what makes the
// Highlights / Whites / Contrast curves behave at the ends), and the answer is rounded once, by
// whoever consumes it. The old table stays as it is: it is the reference the tests compare
// against, and Auto Tone still reasons with it.
//
// The table samples the composed curve every 1/kToneSub level, so a lookup is one multiply, one
// float -> int and a linear interpolation, and an 8-bit input (an integer level) lands exactly
// on a grid point and gets the exact value (no interpolation error at all for 8-bit pictures).
//
// 16-bit input: a 16-bit sample is v / 257 levels, and the 1/16-level grid is too coarse right above
// black for a curve with a steep start (Gamma +100 is 255 * (x / 255)^0.25, whose slope is huge at 0):
// the first cell was off by up to 15 levels. So each table also carries a HEAD: the same curve
// sampled at every 1/257 level over the first kToneHeadLevels levels, i.e. exactly at every 16-bit
// sample there. toneLookupWide reads the head below kToneHeadLevels and the main grid above. With the
// head the worst error over all 65536 samples is about 0.002 level for Gamma +100 (under one 16-bit
// step, 1/257 = 0.0039) and, for the other sliders, at most about 0.03 level (Blacks -100, whose
// smoothstep band is the steepest). Integer levels stay exact: a level 0 or >= kToneHeadLevels is a
// main-grid point, and 0 gives the same value in both.
//
// Per channel: the master curve runs after the sliders, then each channel's own curve on top
// (the order of AdjustmentSet::buildChannelLuts), so building the R, G and B tables is three
// calls that differ only in `channel`.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>

#include "color/curves.hpp"
#include "core/adjustments.hpp"

constexpr int kToneSub = 16;                        // grid points per level
constexpr int kToneSize = 255 * kToneSub + 1;       // 4081: 0, 1/16, ..., 255

// The head: the first kToneHeadLevels levels again, at 1/257 level (one 16-bit sample) per step.
constexpr float kToneHeadPerLevel = 257.0f;         // head cells per level (= 16-bit samples per level)
constexpr int kToneHeadLevels = 2;                  // the head covers 0 .. 2 levels
constexpr int kToneHeadCells = kToneHeadLevels * 257; // 514
constexpr int kToneHeadSize = kToneHeadCells + 1;     // 515 entries: 0, 1/257, ..., 2
constexpr int kToneTableSize = kToneSize + kToneHeadSize; // a table: the main grid, then the head

// x clamped to 0..255; NaN -> 0.
inline float toneClamp(float v)
{
  if (!(v > 0.0f))
    return 0.0f;
  return v > 255.0f ? 255.0f : v;
}

// Linear interpolation in a kToneSize table at `x` levels (clamped to 0..255). Plain grid only: this
// is what the curve tables (curveSample) use. A ToneTable is read with toneLookupWide.
inline float toneLookup(const float *tbl, float x)
{
  const float p = toneClamp(x) * static_cast<float>(kToneSub);
  const int i = static_cast<int>(p);
  if (i >= kToneSize - 1)
    return tbl[kToneSize - 1];
  const float f = p - static_cast<float>(i);
  return tbl[i] + (tbl[i + 1] - tbl[i]) * f;
}

// Reads a kToneTableSize table (main grid, then head) at `x` levels: below kToneHeadLevels through
// the head (exact at every 16-bit sample), above through the main grid. x is clamped to 0..255 first
// (NaN gives 0). `tbl` must hold kToneTableSize floats (ToneTable::y.data()).
inline float toneLookupWide(const float *tbl, float x)
{
  x = toneClamp(x);
  if (x < static_cast<float>(kToneHeadLevels))
  {
    const float p = x * kToneHeadPerLevel;
    int i = static_cast<int>(p);
    if (i >= kToneHeadCells) // p can round up to the cell count
      i = kToneHeadCells - 1;
    const float *h = tbl + kToneSize;
    const float f = p - static_cast<float>(i);
    return h[i] + (h[i + 1] - h[i]) * f;
  }
  return toneLookup(tbl, x);
}

struct ToneTable
{
  std::array<float, kToneTableSize> y{}; // [0, kToneSize): main grid; [kToneSize, ...): head
  float at(float x) const { return toneLookupWide(y.data(), x); }
  // True when every entry is exactly its own input level (the stage can be skipped).
  bool isIdentity() const
  {
    for (int i = 0; i < kToneSize; ++i)
      if (y[static_cast<size_t>(i)] != static_cast<float>(i) * (1.0f / static_cast<float>(kToneSub)))
        return false;
    for (int j = 0; j < kToneHeadSize; ++j)
      if (y[static_cast<size_t>(kToneSize + j)] != static_cast<float>(j) / kToneHeadPerLevel)
        return false;
    return true;
  }
};

// The Tone sliders of `values`, then `master` (the master curve, may be null), then `channel`
// (one channel's own curve, may be null). Slider values are sanitized like toneLutFromValues
// does (NaN / out of range handled by sanitizedValue). A curve that is not evaluable gives the
// identity, like curveToLut.
inline ToneTable toneTableFromValues(const AdjValues &values, const CurvePoints *master,
                                     const CurvePoints *channel)
{
  constexpr float kStep = 1.0f / static_cast<float>(kToneSub);
  std::array<float, kToneSize> mt{}, ct{};
  if (master)
    curveSample(*master, mt.data(), kToneSize, kStep);
  if (channel)
    curveSample(*channel, ct.data(), kToneSize, kStep);
  // The whole chain at one input level: sliders, master curve, channel curve.
  const auto chain = [&](float x)
  {
    for (const auto &spec : kAdjustmentSpecs)
    {
      if (spec.stage != AdjustmentSpec::Stage::Tone || !spec.apply)
        continue;
      const float v = sanitizedValue(spec, values[adjIndex(spec.id)]);
      if (v != 0.0f)
        x = toneClamp(spec.apply(x, v));
    }
    if (master)
      x = toneLookup(mt.data(), x);
    if (channel)
      x = toneLookup(ct.data(), x);
    return x;
  };
  ToneTable t;
  for (int i = 0; i < kToneSize; ++i)
    t.y[static_cast<size_t>(i)] = chain(static_cast<float>(i) * kStep);
  for (int j = 0; j < kToneHeadSize; ++j) // the head: one entry per 16-bit sample
    t.y[static_cast<size_t>(kToneSize + j)] = chain(static_cast<float>(j) / kToneHeadPerLevel);
  return t;
}

// ---------------- The three channels together ----------------
// What the render loop holds: the R, G and B tables. With no per-channel curve all three are the
// same table, so only ch[0] is built and `shared` is set (the loop then reads one table, like the
// byte pipeline's shared-LUT fast path). `identity` is true when the whole stage does nothing and
// can be skipped; it uses the same rule as the byte pipeline for deciding a curve is a straight
// line (curveIsIdentity: the curve rounds to the identity mapping), so an untouched curve never
// changes a pixel.
struct ToneSet
{
  std::array<ToneTable, 3> ch{};
  bool shared = true;
  bool identity = true;
  const ToneTable &at(int c) const { return shared ? ch[0] : ch[static_cast<size_t>(c)]; }
};

// The Tone sliders of `values` and the four curves (master, R, G, B), in the order of
// AdjustmentSet::buildLut / buildChannelLuts: sliders, then the master curve, then the channel's
// own curve.
inline std::shared_ptr<const ToneSet> makeToneSet(const AdjValues &values, const CurveSet &curves)
{
  auto set = std::make_shared<ToneSet>();
  const bool useMaster = !curveIsIdentity(curveToLut(curves[0]));
  const CurvePoints *master = useMaster ? &curves[0] : nullptr;
  bool useChan[3];
  bool anyChan = false;
  for (size_t c = 0; c < 3; ++c)
  {
    useChan[c] = !curveIsIdentity(curveToLut(curves[1 + c]));
    anyChan = anyChan || useChan[c];
  }
  if (!anyChan)
  {
    set->shared = true;
    set->ch[0] = toneTableFromValues(values, master, nullptr);
    set->identity = set->ch[0].isIdentity();
    return set;
  }
  set->shared = false;
  set->identity = true;
  for (size_t c = 0; c < 3; ++c)
  {
    set->ch[c] = toneTableFromValues(values, master, useChan[c] ? &curves[1 + c] : nullptr);
    set->identity = set->identity && set->ch[c].isIdentity();
  }
  return set;
}