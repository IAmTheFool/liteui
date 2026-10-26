// film_fx.hpp - Vignette and Grain: two effects that depend on WHERE a pixel is,
// not on its neighbors (detail layers) or on its color alone (color pass).
// No UI, no editor state; plain functions on plain values, so it is tested
// without a window (tests/film_fx_test.cpp).
//
// Both are the LAST step of the pixel pipeline, after the masks (see
// renderWith), and global only: masks do not get them.
//
// Vignette. The picture's own frame is the reference, i.e. the vignette is
//   "post-crop": it is centered on the picture as it is shown / exported and
//   follows its aspect ratio, so a crop (or a rotation, or a flip) simply
//   gives the vignette of the new frame. The renderer is always handed the
//   already oriented, straightened and cropped picture, so no GeoFrame is
//   needed here: the frame IS the picture. Distance d is 0 at the center and
//   1 at the corners (an ellipse that touches all four corners). A smoothstep
//   ramp, centered on `midpoint` with a half-width set by `feather`, turns d
//   into the weight t (0 = untouched, 1 = full effect).
//     amount < 0: darken   p' = p (1 - k |a| t)
//     amount > 0: lighten  p' = p (1 - k a t) + 255 k a t
//
// Grain. A deterministic function of the pixel's position (x, y) in the
//   picture being rendered, built from an integer hash: no random number
//   generator, no state, no dependence on render order, thread, block size,
//   or compiler. Rendering the same picture twice, in blocks, on a worker or
//   for export, gives the same bytes. The offset is added equally to R, G and
//   B (monochrome grain, like film), scaled by a weight that is full in the
//   midtones and 0.4 at black and white, so grain does not wash out the ends.
//   `size` sets the grain cell as a fraction of the picture's long edge (so
//   a 1024 px preview and the full picture look alike, not identical: the
//   pixel grids differ); `roughness` blends smooth value noise (per cell)
//   with crisp per-pixel noise.
//
// Both effects are computed in float on the finished pixel and rounded once.
// Pixels with alpha 0 (the empty corners of a straightened picture) are left
// exactly as they are; alpha is never changed.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/pixel_math.hpp"

namespace film_fx
{

// Vignette at |amount| = 1 and full weight moves a channel 90 % of the way to
// black (or to white), never all the way.
constexpr float kVignetteStrength = 0.9f;
// Midpoint slider +-100 moves the center of the ramp (in units of corner
// distance) around 0.5 by +-0.45.
constexpr float kMidpointSpan = 0.45f;
// Half-width of the ramp at Feather 0 / 100 (same units).
constexpr float kRampHalfMin = 0.05f;
constexpr float kRampHalfMax = 0.50f;
// Grain at 100 adds up to about this many levels (before the smooth-noise gain).
constexpr float kGrainLevels = 48.0f;
// Grain cell = long edge * (kCellFracMin + kCellFracSpan * size), at least 1 px.
constexpr float kCellFracMin = 0.0004f;
constexpr float kCellFracSpan = 0.0026f;
// Value noise has less spread than per-pixel noise; wider cells get up to this
// much gain so Size does not also mean "weaker".
constexpr float kSmoothGainMax = 1.5f;
// Weight of the grain at black and white (it is 1 at mid-gray).
constexpr float kGrainEndWeight = 0.4f;
constexpr uint32_t kSeedCrisp = 0x6C697465u;
constexpr uint32_t kSeedSmooth = 0x726F6F6Du;

// The six sliders, sanitized and scaled. A default Params does nothing.
struct Params
{
  float vignette = 0.0f;  // -1 (dark) .. +1 (light), 0 = off
  float midpoint = 0.5f;  // 0.05 .. 0.95, ramp center in corner distance
  float feather = 0.5f;   // 0 .. 1 (0 = narrow ramp, 1 = widest)
  float grain = 0.0f;     // 0 .. 1, 0 = off
  float size = 0.0f;      // 0 .. 1
  float roughness = 0.5f; // 0 .. 1
  bool hasVignette() const { return vignette != 0.0f; }
  bool hasGrain() const { return grain != 0.0f; }
  bool any() const { return hasVignette() || hasGrain(); }
  bool operator==(const Params &) const = default;
};

inline float finiteClamp(float v, float lo, float hi)
{
  return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; // NaN/inf = untouched
}

// From the slider values (Vignette, Midpoint, Feather: -100..100; Grain, Size:
// 0..100; Roughness: -100..100). Midpoint, Feather and Roughness are 0 at
// their default, so a file with only "Vignette" set is a normal vignette.
// Every input is clamped and NaN/infinity count as 0, so the per-pixel code
// only ever sees finite values.
inline Params paramsFromValues(float vignette, float midpoint, float feather,
                               float grain, float size, float roughness)
{
  Params p;
  p.vignette = finiteClamp(vignette, -100.0f, 100.0f) / 100.0f;
  p.midpoint = 0.5f + finiteClamp(midpoint, -100.0f, 100.0f) / 100.0f * kMidpointSpan;
  p.feather = 0.5f + finiteClamp(feather, -100.0f, 100.0f) / 200.0f;
  p.grain = finiteClamp(grain, 0.0f, 100.0f) / 100.0f;
  p.size = finiteClamp(size, 0.0f, 100.0f) / 100.0f;
  p.roughness = 0.5f + finiteClamp(roughness, -100.0f, 100.0f) / 200.0f;
  return p;
}

// Everything the per-pixel loop needs for one picture size.
struct Prepared
{
  Params p;
  int W = 0, H = 0;
  bool vig = false, grain = false;
  float invHalfW = 0, invHalfH = 0; // 2 / W, 2 / H
  float inner = 0, invRamp = 1;     // vignette ramp [inner, inner + 1/invRamp]
  float cell = 1, invCell = 1;      // grain cell, px
  float smoothGain = 1;
  bool any() const { return vig || grain; }
};

inline Prepared prepare(const Params &p, int W, int H)
{
  Prepared f;
  f.p = p;
  if (W <= 0 || H <= 0)
    return f;
  f.W = W;
  f.H = H;
  f.vig = p.hasVignette();
  f.grain = p.hasGrain();
  f.invHalfW = 2.0f / static_cast<float>(W);
  f.invHalfH = 2.0f / static_cast<float>(H);
  const float half = kRampHalfMin + (kRampHalfMax - kRampHalfMin) * p.feather;
  f.inner = std::max(0.0f, p.midpoint - half);
  f.invRamp = 1.0f / (p.midpoint + half - f.inner); // at least 0.1, never 0
  const float longEdge = static_cast<float>(std::max(W, H));
  f.cell = std::max(1.0f, longEdge * (kCellFracMin + kCellFracSpan * p.size));
  f.invCell = 1.0f / f.cell;
  f.smoothGain = 1.0f + (kSmoothGainMax - 1.0f) * std::min(1.0f, f.cell - 1.0f);
  return f;
}

// 32-bit integer hash of a position. Unsigned arithmetic only, so the result
// is the same on every compiler and platform.
inline uint32_t hash2(uint32_t x, uint32_t y, uint32_t seed)
{
  uint32_t h = (x * 0x9E3779B1u) ^ (y * 0x85EBCA77u + seed);
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return h;
}
// A hash as a float in [-1, 1).
inline float hashUnit(uint32_t x, uint32_t y, uint32_t seed)
{
  return static_cast<float>(hash2(x, y, seed) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// Smooth value noise at (u, v) >= 0 (grain-cell units): the four surrounding
// lattice hashes, blended with a smoothstep. At integer (u, v) it is exactly
// the hash there.
inline float valueNoise(float u, float v, uint32_t seed)
{
  const uint32_t ix = static_cast<uint32_t>(u), iy = static_cast<uint32_t>(v);
  float fx = u - static_cast<float>(ix), fy = v - static_cast<float>(iy);
  fx = fx * fx * (3.0f - 2.0f * fx);
  fy = fy * fy * (3.0f - 2.0f * fy);
  const float a = hashUnit(ix, iy, seed), b = hashUnit(ix + 1, iy, seed);
  const float c = hashUnit(ix, iy + 1, seed), d = hashUnit(ix + 1, iy + 1, seed);
  const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
  return top + (bot - top) * fy;
}

// The grain value at pixel (x, y), roughly -1.5..1.5, before Amount and the
// tone weight. Depends only on (x, y), the size and the roughness.
inline float grainAt(const Prepared &f, int x, int y)
{
  const float crisp = hashUnit(static_cast<uint32_t>(x), static_cast<uint32_t>(y), kSeedCrisp);
  const float smooth = f.smoothGain * valueNoise(static_cast<float>(x) * f.invCell,
                                                 static_cast<float>(y) * f.invCell, kSeedSmooth);
  return smooth + (crisp - smooth) * f.p.roughness;
}

// The vignette weight (0..1) of pixel (x, y).
inline float vignetteWeight(const Prepared &f, int x, int y)
{
  const float nx = (static_cast<float>(x) + 0.5f) * f.invHalfW - 1.0f;
  const float ny = (static_cast<float>(y) + 0.5f) * f.invHalfH - 1.0f;
  float t = (std::sqrt((nx * nx + ny * ny) * 0.5f) - f.inner) * f.invRamp;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
}

// Applies both effects, in place, to `count` consecutive RGBA8 pixels of a
// picture `W` wide. `first` is the index (y * W + x) of the first of them in
// the WHOLE picture, so a block of rows gives the same bytes as the whole
// picture in one go. Vignette first, then grain. Raw pointers and no std::
// helpers in the loop (apart from sqrt), for Debug-build speed.
inline void apply(const Prepared &f, uint8_t *p, size_t count, size_t first)
{
  if (!f.any())
    return;
  const int W = f.W;
  int x = static_cast<int>(first % static_cast<size_t>(W));
  int y = static_cast<int>(first / static_cast<size_t>(W));
  const float a = f.p.vignette;
  const float mAbs = (a < 0.0f ? -a : a) * kVignetteStrength;
  const float white = a > 0.0f ? 255.0f : 0.0f;
  const float grainLevels = f.p.grain * kGrainLevels;
  float ny = (static_cast<float>(y) + 0.5f) * f.invHalfH - 1.0f;
  float ny2 = ny * ny;
  for (size_t i = 0; i < count; ++i)
  {
    uint8_t *q = p + i * 4;
    if (q[3] != 0)
    {
      float r = static_cast<float>(q[0]), g = static_cast<float>(q[1]), b = static_cast<float>(q[2]);
      if (f.vig)
      {
        const float nx = (static_cast<float>(x) + 0.5f) * f.invHalfW - 1.0f;
        float t = (std::sqrt((nx * nx + ny2) * 0.5f) - f.inner) * f.invRamp;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        t = t * t * (3.0f - 2.0f * t);
        const float m = mAbs * t, keep = 1.0f - m, add = white * m;
        r = r * keep + add;
        g = g * keep + add;
        b = b * keep + add;
      }
      if (f.grain)
      {
        float l = (54.0f * r + 183.0f * g + 19.0f * b) * (1.0f / (256.0f * 255.0f));
        l = l < 0.0f ? 0.0f : (l > 1.0f ? 1.0f : l);
        const float w = kGrainEndWeight + (1.0f - kGrainEndWeight) * 4.0f * l * (1.0f - l);
        const float k = grainLevels * w * grainAt(f, x, y);
        r += k;
        g += k;
        b += k;
      }
      q[0] = toU8Clamped(r);
      q[1] = toU8Clamped(g);
      q[2] = toU8Clamped(b);
    }
    if (++x == W)
    {
      x = 0;
      ++y;
      ny = (static_cast<float>(y) + 0.5f) * f.invHalfH - 1.0f;
      ny2 = ny * ny;
    }
  }
}

} // namespace film_fx
