// Dehaze, Noise Reduction (luminance + color) and Blur, built on the same idea
// as detail_layer.hpp: every effect is a per-pixel formula that reads
// precomputed, cached planes of the SOURCE picture, so it needs no halo rows,
// works the same inside masks, and never recomputes anything while a slider is
// dragged. Read the top of detail_layer.hpp first; this file only adds what is
// different per effect:
//
//  Noise reduction, luminance. Noise is pixel-scale, so it lives in the
//    smallest detail layer (the Sharpness plane: pixel minus a ~1.4 px blur).
//    Instead of adding a share of that detail back (sharpening), take a share
//    away, but only the SMALL part: weight 1 / (1 + (d / T)^2) removes
//    differences near zero (noise) and spares large ones (real edges).
//
//  Noise reduction, color. Color noise is blotchy and low-frequency, so it
//    needs a blur of its own, of the CHROMA channels (R - luma and B - luma;
//    G follows because chroma has zero luma by construction). Each pixel moves
//    toward its blurred chroma, weighted 1 / (1 + (m / T)^2) with m the size
//    of the move, so a genuine color edge (a big move) is left alone.
//
//  Dehaze. Haze veils the picture: I = J t + A (1 - t). The "dark channel
//    prior" says a haze-free picture has some near-black channel in every small
//    patch, so the dark channel of the HAZY picture measures the veil:
//    t = 1 - w * dark / A. That map is smooth, so it is built tiny (about 384
//    px on its long edge: min-pool, erode, blur) and sampled bilinearly. Then
//    J = A + (I - A) / t. Negative values add haze: a flat blend toward a
//    light gray.
//
//  Blur. A blurred copy of the picture in color (a luma-only blur would leave
//    hard color edges). Three fixed strengths are stored at REDUCED resolution
//    (a blur that wide has no fine detail to keep, so memory is tiny) and the
//    slider interpolates between them; 0 is the picture itself.
//
// Like the detail layers, noise reduction and blur are applied as a DELTA
// measured on the source ("blurred minus source"), added to the current pixel,
// so they sit on top of the tone and color sliders instead of undoing them.
//
// Order inside apply(): Dehaze (it rescales the pixel, so it goes first), then
// the detail layers, then noise reduction, then blur (last, so it wins).
//
// Slider values are sanitized in gainsFromValues (clamped to their ranges,
// NaN/inf treated as 0), so the per-pixel code only ever sees finite values.
//
// Like detail_layer.hpp this has no UI dependency and is unit-tested alone
// (tests/neighbor_fx_test.cpp).

#pragma once

#include "detail/detail_layer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace neighbor_fx
{
namespace dl = detail_layer;

// ---- tuning constants ----
constexpr float kChromaSigmaFrac = 0.0025f; // of the long edge...
constexpr float kChromaSigmaMin = 2.0f;     // ...but never below this (px)
constexpr float kNrLumaTMin = 2.0f;         // luminance NR threshold, levels,
constexpr float kNrLumaTMax = 16.0f;        //   at slider 0 / 100
constexpr float kNrColorT = 20.0f;          // chroma move that gets half weight

constexpr int kVeilEdge = 384;           // long edge of the veil map, px
constexpr int kVeilPatch = 3;            // erosion radius on that map
constexpr float kDehazeStrength = 0.85f; // "omega": how much of the veil to remove
constexpr float kDehazeMinT = 0.25f;     // transmission floor (limits blow-up)
constexpr float kLightMin = 180.0f;      // estimated airlight is clamped to
                                         //   [kLightMin, 255]
constexpr float kHazeAdd = 0.6f;         // negative Dehaze: blend at -100
constexpr float kHazeLight = 235.0f;     // ...toward this gray

constexpr float kBlurSigmaFrac[3] = {0.004f, 0.012f, 0.035f}; // x long edge
constexpr float kBlurSigmaMin = 1.5f;
constexpr float kBlurTargetSigma = 2.5f; // sigma, in reduced px, of the stored blur

// ---- a small multi-channel map (reduced resolution) ----
// Values are stored * 16 (4 fractional bits), like detail_layer::Plane.
struct Map
{
  int w = 0, h = 0, ch = 1;
  float scale = 1.0f; // reduced px per source px (1 / reduction factor)
  float aux = 0.0f;   // Veil: the estimated airlight A
  std::vector<uint16_t> v;
};
using MapPtr = std::shared_ptr<const Map>;

inline size_t byteSize(const dl::Plane &p) { return p.size() * sizeof(uint16_t); }
inline size_t byteSize(const Map &m) { return m.v.size() * sizeof(uint16_t); }

// Same idea as detail_layer::Cache, for any plane type: keyed on the picture
// (pointer, size, fingerprint) and a kind number, LRU with a byte cap, built
// outside the lock, never caches a cancelled build.
template <class T>
class Store
{
public:
  using Ptr = std::shared_ptr<const T>;
  Store(size_t maxBytes, size_t maxEntries)
      : maxBytes_(maxBytes), maxEntries_(maxEntries) {}

  // `build` returns a std::shared_ptr<T>, or null when cancelled.
  template <class Build>
  Ptr get(const uint8_t *rgba, int w, int h, int kind, Build &&build)
  {
    const uint64_t fp = dl::fingerprint(rgba, w, h);
    {
      std::lock_guard<std::mutex> lock(m_);
      if (Entry *e = find(rgba, w, h, kind, fp))
      {
        e->used = ++tick_;
        return e->v;
      }
    }
    std::shared_ptr<T> made = build();
    if (!made)
      return nullptr;
    std::lock_guard<std::mutex> lock(m_);
    if (Entry *e = find(rgba, w, h, kind, fp))
    {
      e->used = ++tick_;
      return e->v;
    }
    bytes_ += byteSize(*made);
    entries_.push_back(Entry{rgba, w, h, kind, fp, made, ++tick_});
    evict();
    return made;
  }
  void clear()
  {
    std::lock_guard<std::mutex> lock(m_);
    entries_.clear();
    bytes_ = 0;
  }
  size_t size() const
  {
    std::lock_guard<std::mutex> lock(m_);
    return entries_.size();
  }

private:
  struct Entry
  {
    const uint8_t *rgba;
    int w, h, kind;
    uint64_t fp;
    Ptr v;
    uint64_t used;
  };
  Entry *find(const uint8_t *rgba, int w, int h, int kind, uint64_t fp)
  {
    for (Entry &e : entries_)
      if (e.rgba == rgba && e.w == w && e.h == h && e.kind == kind &&
          e.fp == fp)
        return &e;
    return nullptr;
  }
  void evict()
  {
    while (entries_.size() > 1 &&
           (bytes_ > maxBytes_ || entries_.size() > maxEntries_))
    {
      size_t oldest = 0;
      for (size_t i = 1; i < entries_.size(); ++i)
        if (entries_[i].used < entries_[oldest].used)
          oldest = i;
      if (oldest == entries_.size() - 1)
        break;
      bytes_ -= byteSize(*entries_[oldest].v);
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
  }
  mutable std::mutex m_;
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t tick_ = 0;
  size_t maxBytes_, maxEntries_;
};

// Note: with detail_layer's own cache (384 MB) these add up to a worst case of
// about 700 MB of cached planes on very large pictures.
inline Store<dl::Plane> &chromaStore()
{
  static Store<dl::Plane> s(256u * 1024u * 1024u, 6); // full-size planes
  return s;
}
inline Store<Map> &mapStore()
{
  static Store<Map> s(64u * 1024u * 1024u, 40); // reduced-size maps
  return s;
}
inline void clearCaches()
{
  dl::cache().clear();
  chromaStore().clear();
  mapStore().clear();
}

// ---- blur helpers ----

// Box radius whose 3-pass blur approximates a Gaussian of `sigma` (see
// detail_layer::boxRadius for the derivation).
inline int boxRadiusForSigma(float sigma)
{
  const float width = std::sqrt(4.0f * sigma * sigma + 1.0f);
  const int r = static_cast<int>(std::lround((width - 1.0f) * 0.5f));
  return std::clamp(r, 1, dl::kMaxBoxRadius);
}

// 3 box passes each way, in place. False = cancelled.
inline bool blurPlane(dl::Plane &p, size_t W, size_t H, int r,
                      const std::atomic<uint64_t> *latest, uint64_t gen)
{
  const float inv = 1.0f / static_cast<float>(2 * r + 1);
  std::vector<uint16_t> pad(W + 2 * static_cast<size_t>(r));
  for (int pass = 0; pass < 3; ++pass)
    for (size_t y = 0; y < H; ++y)
    {
      if ((y & 15) == 0 && dl::cancelled(latest, gen))
        return false;
      dl::hboxRow(p.data() + y * W, W, r, pad.data(), inv);
    }
  dl::Plane tmp(W * H);
  std::vector<uint32_t> col(W);
  for (int pass = 0; pass < 3; ++pass)
  {
    if (!dl::vbox(p.data(), tmp.data(), W, H, r, col.data(), inv, latest, gen))
      return false;
    p.swap(tmp);
  }
  return true;
}

// ---- chroma planes (color noise reduction) ----

inline float chromaSigma(int w, int h)
{
  return std::max(kChromaSigmaMin,
                  kChromaSigmaFrac * static_cast<float>(std::max(w, h)));
}

// which: 0 = blurred (R - luma), 1 = blurred (B - luma). Stored as
// (chroma + 255) * 16 so the signed value fits an unsigned plane.
inline bool buildChroma(const uint8_t *rgba, int w, int h, int which,
                        dl::Plane &out, const std::atomic<uint64_t> *latest,
                        uint64_t gen)
{
  const size_t W = static_cast<size_t>(w), H = static_cast<size_t>(h);
  out.resize(W * H);
  for (size_t y = 0; y < H; ++y)
  {
    const uint8_t *s = rgba + y * W * 4;
    uint16_t *o = out.data() + y * W;
    for (size_t x = 0; x < W; ++x)
    {
      const uint8_t *p = s + x * 4;
      const int l = dl::lumaOf(p);
      const int c = which == 0 ? p[0] - l : p[2] - l;
      o[x] = static_cast<uint16_t>((c + 255) << 4);
    }
  }
  return blurPlane(out, W, H, boxRadiusForSigma(chromaSigma(w, h)), latest, gen);
}

// ---- veil map (dehaze) ----

// Min filter, radius r, edges clamped, separable; on a small 8-bit map.
inline void erode(std::vector<uint8_t> &m, int w, int h, int r)
{
  std::vector<uint8_t> tmp(m.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t v = 255;
      for (int k = -r; k <= r; ++k)
        v = std::min(v, m[static_cast<size_t>(y) * w + std::clamp(x + k, 0, w - 1)]);
      tmp[static_cast<size_t>(y) * w + x] = v;
    }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t v = 255;
      for (int k = -r; k <= r; ++k)
        v = std::min(v, tmp[static_cast<size_t>(std::clamp(y + k, 0, h - 1)) * w + x]);
      m[static_cast<size_t>(y) * w + x] = v;
    }
}

inline std::shared_ptr<Map> buildVeil(const uint8_t *rgba, int w, int h,
                                      const std::atomic<uint64_t> *latest,
                                      uint64_t gen)
{
  const int f = std::max(
      1, static_cast<int>(std::ceil(static_cast<float>(std::max(w, h)) /
                                    static_cast<float>(kVeilEdge))));
  const int vw = (w + f - 1) / f, vh = (h + f - 1) / f;
  // 1. Dark channel, min-pooled to the small size: smallest of R, G, B over
  //    each f x f block.
  std::vector<uint8_t> dark(static_cast<size_t>(vw) * vh, 255);
  for (int y = 0; y < h; ++y)
  {
    if ((y & 31) == 0 && dl::cancelled(latest, gen))
      return nullptr;
    const uint8_t *s = rgba + static_cast<size_t>(y) * w * 4;
    uint8_t *row = dark.data() + static_cast<size_t>(y / f) * vw;
    int cx = 0, run = 0;
    for (int x = 0; x < w; ++x)
    {
      const uint8_t *p = s + static_cast<size_t>(x) * 4;
      const uint8_t m = std::min({p[0], p[1], p[2]});
      if (m < row[cx])
        row[cx] = m;
      if (++run == f)
      {
        run = 0;
        ++cx;
      }
    }
  }
  // 2. Patch minimum (erosion): the "dark channel" proper.
  erode(dark, vw, vh, kVeilPatch);
  // 3. Airlight: the brightest dark channel. A hazy scene has a region (sky,
  //    distance) where it is close to the light itself.
  float A = 0.0f;
  for (uint8_t d : dark)
    A = std::max(A, static_cast<float>(d));
  A = std::clamp(A, kLightMin, 255.0f);
  // 4. Smooth it into a transmission-ready veil (precision: * 16).
  auto m = std::make_shared<Map>();
  m->w = vw;
  m->h = vh;
  m->ch = 1;
  m->scale = 1.0f / static_cast<float>(f);
  m->aux = A;
  m->v.resize(dark.size());
  for (size_t i = 0; i < dark.size(); ++i)
    m->v[i] = static_cast<uint16_t>(dark[i] << 4);
  if (!blurPlane(m->v, static_cast<size_t>(vw), static_cast<size_t>(vh),
                 kVeilPatch, latest, gen))
    return nullptr;
  return m;
}

// ---- blurred copy (blur) ----

// level 0..2: a color blur of sigma kBlurSigmaFrac[level] * long edge, stored
// at reduced size (box average, then a small blur at that size).
inline std::shared_ptr<Map> buildBlurLevel(const uint8_t *rgba, int w, int h,
                                           int level,
                                           const std::atomic<uint64_t> *latest,
                                           uint64_t gen)
{
  const float sigma = std::max(
      kBlurSigmaMin, kBlurSigmaFrac[level] * static_cast<float>(std::max(w, h)));
  const int f = std::max(1, static_cast<int>(std::lround(sigma / kBlurTargetSigma)));
  const int rw = (w + f - 1) / f, rh = (h + f - 1) / f;
  const size_t n = static_cast<size_t>(rw) * rh;
  std::array<dl::Plane, 3> ch;
  for (auto &p : ch)
    p.resize(n);
  for (int ry = 0; ry < rh; ++ry)
  {
    if ((ry & 7) == 0 && dl::cancelled(latest, gen))
      return nullptr;
    const int y0 = ry * f, y1 = std::min(h, y0 + f);
    for (int rx = 0; rx < rw; ++rx)
    {
      const int x0 = rx * f, x1 = std::min(w, x0 + f);
      uint32_t sum[3] = {0, 0, 0};
      for (int y = y0; y < y1; ++y)
      {
        const uint8_t *p = rgba + (static_cast<size_t>(y) * w + x0) * 4;
        for (int x = x0; x < x1; ++x, p += 4)
        {
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
        }
      }
      const uint32_t cnt = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
      for (int c = 0; c < 3; ++c)
        // 64-bit: sum * 16 would overflow 32 bits for blocks over ~1M pixels.
        ch[static_cast<size_t>(c)][static_cast<size_t>(ry) * rw + rx] =
            static_cast<uint16_t>(
                (static_cast<uint64_t>(sum[c]) * 16 + cnt / 2) / cnt);
    }
  }
  const int r = boxRadiusForSigma(std::max(0.8f, sigma / static_cast<float>(f)));
  for (auto &p : ch)
    if (!blurPlane(p, static_cast<size_t>(rw), static_cast<size_t>(rh), r, latest, gen))
      return nullptr;
  auto m = std::make_shared<Map>();
  m->w = rw;
  m->h = rh;
  m->ch = 3;
  m->scale = 1.0f / static_cast<float>(f);
  m->v.resize(n * 3);
  for (size_t i = 0; i < n; ++i)
    for (size_t c = 0; c < 3; ++c)
      m->v[i * 3 + c] = ch[c][i];
  return m;
}

// Bilinear sample of a map at SOURCE pixel (x, y); `out` gets `ch` values
// (still * 16). Coordinates are clamped to the map.
inline void sampleMap(const Map &m, int x, int y, float *out)
{
  float fx = (static_cast<float>(x) + 0.5f) * m.scale - 0.5f;
  float fy = (static_cast<float>(y) + 0.5f) * m.scale - 0.5f;
  const float mx = static_cast<float>(m.w - 1), my = static_cast<float>(m.h - 1);
  fx = fx < 0.0f ? 0.0f : (fx > mx ? mx : fx);
  fy = fy < 0.0f ? 0.0f : (fy > my ? my : fy);
  const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
  const int x1 = x0 + 1 < m.w ? x0 + 1 : x0, y1 = y0 + 1 < m.h ? y0 + 1 : y0;
  const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
  const int ch = m.ch;
  const uint16_t *a = m.v.data() + (static_cast<size_t>(y0) * m.w + x0) * ch;
  const uint16_t *b = m.v.data() + (static_cast<size_t>(y0) * m.w + x1) * ch;
  const uint16_t *c = m.v.data() + (static_cast<size_t>(y1) * m.w + x0) * ch;
  const uint16_t *d = m.v.data() + (static_cast<size_t>(y1) * m.w + x1) * ch;
  for (int k = 0; k < ch; ++k)
  {
    const float top = static_cast<float>(a[k]) + (static_cast<float>(b[k]) - static_cast<float>(a[k])) * tx;
    const float bot = static_cast<float>(c[k]) + (static_cast<float>(d[k]) - static_cast<float>(c[k])) * tx;
    out[k] = top + (bot - top) * ty;
  }
}

// ---- slider values -> gains ----

struct Gains
{
  dl::Gains detail;     // Texture / Clarity / Sharpness
  float dehaze = 0.0f;  // -1 (add haze) .. +1 (remove haze)
  float nrLuma = 0.0f;  // 0..1
  float nrColor = 0.0f; // 0..1
  float blur = 0.0f;    // 0..1
  bool any() const
  {
    return detail.any() || dehaze != 0.0f || nrLuma != 0.0f ||
           nrColor != 0.0f || blur != 0.0f;
  }

  // Which blur levels this setting reads: position 0..3 along
  // [source, level0, level1, level2]; returns a bitmask over levels 0..2.
  unsigned blurLevels() const
  {
    if (blur <= 0.0f)
      return 0;
    const float pos = std::min(blur, 1.0f) * 3.0f;
    const int seg = std::min(static_cast<int>(pos), 2);
    return seg == 0 ? 1u : (seg == 1 ? 3u : 6u);
  }

  // Bits: 0..2 the luma detail planes (Texture, Clarity, Sharpness; noise
  // reduction reads Sharpness's), 3 chroma planes, 4 veil, 5..7 blur levels.
  uint32_t need() const
  {
    uint32_t n = detail.need();
    if (nrLuma > 0.0f)
      n |= 1u << dl::Sharpness;
    if (nrColor > 0.0f)
      n |= 1u << 3;
    if (dehaze > 0.0f)
      n |= 1u << 4;
    n |= blurLevels() << 5;
    return n;
  }
  bool operator==(const Gains &) const = default;
};

// UI values: texture / clarity / dehaze in -100..100, the rest 0..100. Each is
// clamped to its range and NaN/inf count as 0 (untouched), so every gain is
// finite: a NaN would otherwise turn into a NaN pixel and an undefined cast.
inline Gains gainsFromValues(float texture, float clarity, float sharpness,
                             float dehaze = 0.0f, float noiseLuma = 0.0f,
                             float noiseColor = 0.0f, float blur = 0.0f)
{
  auto sane = [](float v, float lo, float hi)
  { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
  Gains g;
  g.detail = dl::gainsFromValues(texture, clarity, sharpness);
  g.dehaze = sane(dehaze, -100.0f, 100.0f) / 100.0f;
  g.nrLuma = sane(noiseLuma, 0.0f, 100.0f) / 100.0f;
  g.nrColor = sane(noiseColor, 0.0f, 100.0f) / 100.0f;
  g.blur = sane(blur, 0.0f, 100.0f) / 100.0f;
  return g;
}

// ---- the planes one render uses ----

struct Planes
{
  dl::Planes luma;
  dl::Cache::PlanePtr chroma[2];
  MapPtr veil;
  MapPtr blur[3];
};

// Fetches (or builds, the first time) what `need` asks for. False = cancelled.
// (No pixels or a non-positive size means nothing to do: true, planes null.)
inline bool acquire(Planes &out, const uint8_t *rgba, int w, int h,
                    uint32_t need, const std::atomic<uint64_t> *latest,
                    uint64_t gen)
{
  if (!rgba || w <= 0 || h <= 0)
    return true;
  if (!dl::acquire(out.luma, rgba, w, h, need & 7u, latest, gen))
    return false;
  if (need & (1u << 3))
    for (int k = 0; k < 2; ++k)
    {
      out.chroma[k] = chromaStore().get(
          rgba, w, h, 10 + k,
          [&]() -> std::shared_ptr<dl::Plane>
          {
            auto p = std::make_shared<dl::Plane>();
            return buildChroma(rgba, w, h, k, *p, latest, gen) ? p : nullptr;
          });
      if (!out.chroma[k])
        return false;
    }
  if (need & (1u << 4))
  {
    out.veil = mapStore().get(rgba, w, h, 20,
                              [&]() -> std::shared_ptr<Map>
                              { return buildVeil(rgba, w, h, latest, gen); });
    if (!out.veil)
      return false;
  }
  for (int k = 0; k < 3; ++k)
    if (need & (1u << (5 + k)))
    {
      out.blur[k] = mapStore().get(
          rgba, w, h, 30 + k,
          [&]() -> std::shared_ptr<Map>
          { return buildBlurLevel(rgba, w, h, k, latest, gen); });
      if (!out.blur[k])
        return false;
    }
  return true;
}

// ---- applying ----

// Rounds and clamps to a byte: the same rule as toU8Clamped in pixel_math.hpp
// (NaN -> 0, +/-inf saturate, halfway rounds up), written without std:: calls
// for the same Debug-build reason as the other per-pixel loops.
inline uint8_t toU8(float v)
{
  if (!(v >= 0.0f)) // also true for NaN
    return 0;
  if (v > 255.0f)
    return 255;
  return static_cast<uint8_t>(v + 0.5f);
}

// True when plane `p` covers pixels [first, first + count).
inline bool covers(const dl::Plane *p, size_t first, size_t count)
{
  return p && first <= p->size() && count <= p->size() - first;
}

// Steps through consecutive pixels of a picture W wide, keeping (x, y).
// W must be positive.
struct Walker
{
  int W, x, y;
  Walker(size_t first, int width)
      : W(width), x(static_cast<int>(first % static_cast<size_t>(width))),
        y(static_cast<int>(first / static_cast<size_t>(width))) {}
  void next()
  {
    if (++x == W)
    {
      x = 0;
      ++y;
    }
  }
};

inline void dehazePass(const Map *veil, float amount, uint8_t *dst,
                       size_t count, size_t first, int W)
{
  if (amount < 0.0f)
  { // add haze: a flat blend toward light gray; needs no map
    const float h = kHazeAdd * -amount;
    for (size_t i = 0; i < count; ++i)
      for (int c = 0; c < 3; ++c)
      {
        const float v = static_cast<float>(dst[i * 4 + c]);
        dst[i * 4 + c] = toU8(v + (kHazeLight - v) * h);
      }
    return;
  }
  if (!veil)
    return;
  const float omega = kDehazeStrength * amount;
  const float A = veil->aux, invA = 1.0f / A;
  Walker wk(first, W);
  for (size_t i = 0; i < count; ++i, wk.next())
  {
    float d;
    sampleMap(*veil, wk.x, wk.y, &d);
    float t = 1.0f - omega * (d * (1.0f / 16.0f)) * invA;
    t = t < kDehazeMinT ? kDehazeMinT : (t > 1.0f ? 1.0f : t);
    const float invT = 1.0f / t;
    for (int c = 0; c < 3; ++c)
    {
      const float v = static_cast<float>(dst[i * 4 + c]);
      dst[i * 4 + c] = toU8(A + (v - A) * invT);
    }
  }
}

inline void nrLumaPass(const dl::Plane *sharp, float s, const uint8_t *src,
                       uint8_t *dst, size_t count, size_t first)
{
  if (!covers(sharp, first, count))
    return;
  const uint16_t *b = sharp->data() + first;
  const float T = kNrLumaTMin + (kNrLumaTMax - kNrLumaTMin) * s;
  const float invT2 = 1.0f / (T * T);
  for (size_t i = 0; i < count; ++i)
  {
    const uint8_t *p = src + i * 4;
    const int l = (dl::kLumaR * p[0] + dl::kLumaG * p[1] + dl::kLumaB * p[2] + 128) >> 8;
    const float d = (static_cast<float>(l << 4) - static_cast<float>(b[i])) * (1.0f / 16.0f);
    const float k = -s * d / (1.0f + d * d * invT2);
    uint8_t *o = dst + i * 4;
    o[0] = toU8(static_cast<float>(o[0]) + k);
    o[1] = toU8(static_cast<float>(o[1]) + k);
    o[2] = toU8(static_cast<float>(o[2]) + k);
  }
}

inline void nrColorPass(const dl::Plane *cr, const dl::Plane *cb, float s,
                        const uint8_t *src, uint8_t *dst, size_t count,
                        size_t first)
{
  if (!covers(cr, first, count) || !covers(cb, first, count))
    return;
  const uint16_t *br = cr->data() + first;
  const uint16_t *bb = cb->data() + first;
  const float invT2 = 1.0f / (kNrColorT * kNrColorT);
  const float wR = static_cast<float>(dl::kLumaR);
  const float wB = static_cast<float>(dl::kLumaB);
  const float invWG = 1.0f / static_cast<float>(dl::kLumaG);
  for (size_t i = 0; i < count; ++i)
  {
    const uint8_t *p = src + i * 4;
    const int l = (dl::kLumaR * p[0] + dl::kLumaG * p[1] + dl::kLumaB * p[2] + 128) >> 8;
    // Move toward the blurred chroma. dg follows from dr and db because the
    // luma weights of the three chroma moves must sum to zero.
    const float dr = static_cast<float>(br[i]) * (1.0f / 16.0f) - 255.0f - static_cast<float>(p[0] - l);
    const float db = static_cast<float>(bb[i]) * (1.0f / 16.0f) - 255.0f - static_cast<float>(p[2] - l);
    const float dg = -(wR * dr + wB * db) * invWG;
    const float w = s / (1.0f + (dr * dr + db * db + dg * dg) * invT2);
    uint8_t *o = dst + i * 4;
    o[0] = toU8(static_cast<float>(o[0]) + w * dr);
    o[1] = toU8(static_cast<float>(o[1]) + w * dg);
    o[2] = toU8(static_cast<float>(o[2]) + w * db);
  }
}

inline void blurPass(const MapPtr *levels, float amount, const uint8_t *src,
                     uint8_t *dst, size_t count, size_t first, int W)
{
  const float pos = std::min(amount, 1.0f) * 3.0f;
  const int seg = std::min(static_cast<int>(pos), 2);
  const float t = pos - static_cast<float>(seg);
  const Map *lo = seg == 0 ? nullptr : levels[seg - 1].get(); // null = the source itself
  const Map *hi = levels[seg].get();
  if (!hi || (seg > 0 && !lo))
    return;
  Walker wk(first, W);
  for (size_t i = 0; i < count; ++i, wk.next())
  {
    float a[3], b[3];
    sampleMap(*hi, wk.x, wk.y, b);
    const uint8_t *p = src + i * 4;
    if (lo)
      sampleMap(*lo, wk.x, wk.y, a);
    else
      for (int c = 0; c < 3; ++c)
        a[c] = static_cast<float>(p[c]) * 16.0f;
    uint8_t *o = dst + i * 4;
    for (int c = 0; c < 3; ++c)
    {
      const float blurred = (a[c] + (b[c] - a[c]) * t) * (1.0f / 16.0f);
      o[c] = toU8(static_cast<float>(o[c]) + (blurred - static_cast<float>(p[c])));
    }
  }
}

// All the neighbor effects for `count` consecutive pixels of `dst` (RGBA8,
// alpha untouched). `src` holds the SOURCE pixels for the same positions and
// `first` is their index in the whole picture of width W (which must be
// positive; otherwise nothing is done).
inline void apply(const Planes &pl, const Gains &g, const uint8_t *src,
                  uint8_t *dst, size_t count, size_t first, int W)
{
  if (W <= 0)
    return;
  if (g.dehaze != 0.0f)
    dehazePass(pl.veil.get(), g.dehaze, dst, count, first, W);
  dl::addPixels(pl.luma, g.detail, src, dst, count, first);
  if (g.nrLuma > 0.0f)
    nrLumaPass(pl.luma.p[dl::Sharpness].get(), g.nrLuma, src, dst, count, first);
  if (g.nrColor > 0.0f)
    nrColorPass(pl.chroma[0].get(), pl.chroma[1].get(), g.nrColor, src, dst,
                count, first);
  if (g.blur > 0.0f)
    blurPass(pl.blur, g.blur, src, dst, count, first, W);
}

} // namespace neighbor_fx