// Detail layers for Texture, Clarity and Sharpness.
//
// Those three effects are not pointwise: a pixel's result depends on its
// neighbors. They all work the same way, though, so they share one idea, the
// "detail layer":
//
//     detail = luma(pixel) - blur(luma)      (what the blur threw away)
//     result = pixel + gain * detail         (add some of it back, or take it out)
//
// The three effects only differ in HOW BIG the blur is:
//     Sharpness: tiny blur   -> only the finest edges 
//     Texture:   small blur  -> fine surface detail (skin pores, bark, fabric)
//     Clarity:   large blur  -> local contrast (the "punch" of mid-size shapes)
//
// Design choices (all of them in service of the interactive preview):
//
//  * The blur is of the SOURCE picture's luma, not of the picture after the
//    tone and color sliders. That makes a blur plane depend on nothing but the
//    image, so it is computed once and cached: dragging Exposure, Contrast or
//    any other slider never recomputes it, and neither does dragging Texture
//    itself (that only changes `gain`). The trade-off: detail is measured
//    before tone mapping, so a strong Exposure boost does not amplify it the
//    way sharpening an already-brightened picture would.
//
//  * The blur plane covers the WHOLE image, so no pixel ever lacks neighbors:
//    the render loop's 128-row blocks and the masks need no halo rows. A mask
//    just blends "pixel + gain * detail", a per-pixel formula like any other.
//
//  * Only luma is blurred, and the same amount is added to R, G and B. Adding
//    it per channel would shift hues along edges (color fringes).
//
//  * Blur sizes are fractions of the picture's long edge, so the 1024 px drag
//    preview and the full-resolution render blur roughly the same *part of the
//    picture* and look alike. (Roughly: the box radius is an integer >= 1, so
//    the smallest blurs, notably Sharpness on anything under ~6000 px, are
//    wider than their nominal sigma and don't shrink any further. See boxRadius.)
//
// This header has no dependency on the UI library: it works on plain RGBA8
// (or RGBA 16-bit) pointers, so it can be unit-tested on its own
// (tests/detail_layer_test.cpp).
//
// SOURCE SAMPLE TYPES. The planes and the per-pixel passes read the source picture as
// uint8_t samples (RGBA8) or uint16_t samples (a 16-bit picture, core/image16.hpp); SrcTraits<S>
// (below) is the one place that knows the difference. For 8 bits everything is exactly what it
// always was (integer-rounded luma, planes in 1/16 level). For 16 bits a pixel's luma is NOT
// rounded to a level (the rounding was a +-0.5 level sawtooth on every gradient that Texture,
// Clarity and Sharpness then multiplied) and the planes keep 1/64 level, so the detail measured on
// a smooth 16-bit gradient is zero, as it should be.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace detail_layer
{

enum Kind : int
{
  Texture = 0,
  Clarity = 1,
  Sharpness = 2
};
constexpr int kKindCount = 3;

// Gaussian sigma of each layer's blur = fraction of the long edge, but never
// below a floor (a blur smaller than that stops being a blur).
constexpr float kSigmaFrac[kKindCount] = {0.0012f, 0.006f, 0.0003f};
constexpr float kSigmaMin[kKindCount] = {1.0f, 3.0f, 0.6f};

// How much of the detail layer slider value +100 adds. Texture and Clarity
// also work for negative values (they then smooth); Sharpness is 0..100.
constexpr float kGain[kKindCount] = {0.8f, 1.0f, 1.5f};

// Clarity's large-radius detail is the one that makes halos around edges. It
// is soft-limited: d / (1 + |d| / kClarityLimit) keeps slope 1 near 0 and
// saturates toward +/- kClarityLimit levels.
constexpr float kClarityLimit = 64.0f;

constexpr int kMaxBoxRadius = 1000; // keeps every running sum exact in float for 8-bit sources
// (a box sums at most 2 * 1000 + 1 values of at most 255 * 16 = 4080, i.e. about
// 8.2 million, below the 2^24 float can hold exactly). For 16-bit sources the planes hold
// 255 * 64 = 16320 per value and a sum can pass 2^24 (the running sums are uint32_t, so they are
// still exact); only the conversion of such a sum to float rounds, by less than 0.01 plane count,
// which can't change a rounded result except at an exact tie (and a flat picture stays flat).

// One blurred luma plane: width * height values, each luma * kFix (see SrcTraits: 4 fractional
// bits for an 8-bit source, 6 for a 16-bit one, so the blur isn't rounded to whole levels before
// it is subtracted).
using Plane = std::vector<uint16_t>;

// 8-bit luma weights, /256. They sum to exactly 256, so the result is 0..255.
// Used both to build the blurred plane and to measure each pixel's own luma;
// the two must agree, hence one set of constants.
constexpr int kLumaR = 54, kLumaG = 183, kLumaB = 19;
static_assert(kLumaR + kLumaG + kLumaB == 256, "luma weights must sum to 256");

inline int lumaOf(const uint8_t *p)
{
  return (kLumaR * p[0] + kLumaG * p[1] + kLumaB * p[2] + 128) >> 8;
}

// Midtone weight 4 l (1 - l) by luma: Clarity leaves the ends alone, so it
// can't clip highlights or crush shadows.
inline const float *midWeights()
{
  static const std::array<float, 256> t = []
  {
    std::array<float, 256> a{};
    for (int i = 0; i < 256; ++i)
    {
      const float l = static_cast<float>(i) / 255.0f;
      a[static_cast<size_t>(i)] = 4.0f * l * (1.0f - l);
    }
    return a;
  }();
  return t.data();
}

// ---- Source sample types ----
// How the planes and passes read one pixel of the SOURCE picture, per sample type S:
//   kFix / kInvFix  plane units per level (a plane or map value is level * kFix) and its inverse
//   level(v)        one sample in levels (0..255)
//   lumaFix(p)      the pixel's luma in plane units
//   lumaMid(p,..)   lumaFix plus Clarity's midtone weight 4 l (1 - l)
//   chroma(p,..)    R - luma and B - luma, in levels
//   fixFromSum      the average of `cnt` samples (their sum) in plane units, rounded
//   darkFix(m)      a sample (the smallest of R, G, B) in plane units
// The uint8_t version is exactly the integer arithmetic these passes always used: every 8-bit
// result is bit-identical to before. The uint16_t version works in float: a 16-bit sample is
// v / 257 levels (a division, so a widened 8-bit value k * 257 is exactly k again).
template <class S>
struct SrcTraits;

template <>
struct SrcTraits<uint8_t>
{
  static constexpr float kFix = 16.0f;
  static constexpr float kInvFix = 1.0f / 16.0f;
  static float level(uint8_t v) { return static_cast<float>(v); }
  static float lumaFix(const uint8_t *p) { return static_cast<float>(lumaOf(p) << 4); }
  static void lumaMid(const uint8_t *p, float *lumaFixOut, float *mid)
  {
    const int l = lumaOf(p);
    *lumaFixOut = static_cast<float>(l << 4);
    *mid = midWeights()[l];
  }
  static void chroma(const uint8_t *p, float *cr, float *cb)
  {
    const int l = lumaOf(p);
    *cr = static_cast<float>(p[0] - l);
    *cb = static_cast<float>(p[2] - l);
  }
  static uint16_t fixFromSum(uint64_t sum, uint32_t cnt)
  {
    return static_cast<uint16_t>((sum * 16 + cnt / 2) / cnt);
  }
  static uint16_t darkFix(uint8_t m) { return static_cast<uint16_t>(m << 4); }
};

template <>
struct SrcTraits<uint16_t>
{
  static constexpr float kFix = 64.0f;
  static constexpr float kInvFix = 1.0f / 64.0f;
  static float level(uint16_t v) { return static_cast<float>(v) / 257.0f; }
  // Rec. 709 luma in levels, NOT rounded. The weighted sum of samples is below 2^24, so it is exact
  // in float; one multiply by 1 / (256 * 257) turns it into levels.
  static float lumaLevels(const uint16_t *p)
  {
    return (static_cast<float>(kLumaR) * static_cast<float>(p[0]) +
            static_cast<float>(kLumaG) * static_cast<float>(p[1]) +
            static_cast<float>(kLumaB) * static_cast<float>(p[2])) *
           (1.0f / (256.0f * 257.0f));
  }
  static float lumaFix(const uint16_t *p) { return lumaLevels(p) * kFix; }
  static void lumaMid(const uint16_t *p, float *lumaFixOut, float *mid)
  {
    const float lv = lumaLevels(p);
    *lumaFixOut = lv * kFix;
    const float t = lv * (1.0f / 255.0f);
    *mid = 4.0f * t * (1.0f - t);
  }
  static void chroma(const uint16_t *p, float *cr, float *cb)
  {
    const float lv = lumaLevels(p);
    *cr = level(p[0]) - lv;
    *cb = level(p[2]) - lv;
  }
  static uint16_t fixFromSum(uint64_t sum, uint32_t cnt)
  {
    const uint64_t d = static_cast<uint64_t>(cnt) * 257u;
    return static_cast<uint16_t>((sum * 64 + d / 2) / d);
  }
  static uint16_t darkFix(uint16_t m) { return static_cast<uint16_t>((static_cast<uint32_t>(m) * 64u + 128u) / 257u); }
};

// Three passes of a box blur approximate a Gaussian. For n passes of width w
// the variance is n (w^2 - 1) / 12, so for n = 3: w = sqrt(4 sigma^2 + 1).
// Returns the box radius (w = 2r + 1) for this layer on a w x h picture.
// The radius is rounded to an integer and is at least 1 (sigma about 1.41), so
// requested sigmas below that are widened, and the effective sigma only changes
// in coarse steps.
inline int boxRadius(int kind, int w, int h)
{
  const float longEdge = static_cast<float>(std::max(w, h));
  const float sigma = std::max(kSigmaMin[kind], kSigmaFrac[kind] * longEdge);
  const float width = std::sqrt(4.0f * sigma * sigma + 1.0f);
  const int r = static_cast<int>(std::lround((width - 1.0f) * 0.5f));
  return std::clamp(r, 1, kMaxBoxRadius);
}

inline bool cancelled(const std::atomic<uint64_t> *latest, uint64_t gen)
{
  return latest && latest->load(std::memory_order_relaxed) != gen;
}

// Horizontal box blur of one row, in place. Edges replicate the border pixel,
// so a flat picture stays exactly flat. `pad` has room for W + 2r values.
inline void hboxRow(uint16_t *row, size_t W, int r, uint16_t *pad, float inv)
{
  if (W == 0)
    return;
  const size_t R = static_cast<size_t>(r);
  for (size_t i = 0; i < R; ++i)
    pad[i] = row[0];
  std::memcpy(pad + R, row, W * sizeof(uint16_t));
  for (size_t i = 0; i < R; ++i)
    pad[R + W + i] = row[W - 1];
  uint32_t sum = 0;
  for (size_t i = 0; i <= 2 * R; ++i)
    sum += pad[i];
  for (size_t x = 0; x < W; ++x)
  {
    row[x] = static_cast<uint16_t>(static_cast<float>(sum) * inv + 0.5f);
    if (x + 1 < W)
      sum = sum + pad[x + 2 * R + 1] - pad[x];
  }
}

// Vertical box blur, `in` -> `out`. Row-wise on purpose: `col` keeps one
// running sum per column, so memory is read in order (a column-by-column
// pass would stride through the whole image for every pixel).
inline bool vbox(const uint16_t *in, uint16_t *out, size_t W, size_t H, int r,
                 uint32_t *col, float inv, const std::atomic<uint64_t> *latest,
                 uint64_t gen)
{
  const long Hl = static_cast<long>(H);
  auto rowp = [&](long y)
  {
    y = y < 0 ? 0 : (y > Hl - 1 ? Hl - 1 : y);
    return in + static_cast<size_t>(y) * W;
  };
  std::fill(col, col + W, 0u);
  for (long y = -r; y <= r; ++y)
  {
    const uint16_t *s = rowp(y);
    for (size_t x = 0; x < W; ++x)
      col[x] += s[x];
  }
  for (size_t y = 0; y < H; ++y)
  {
    if ((y & 15) == 0 && cancelled(latest, gen))
      return false;
    uint16_t *o = out + y * W;
    for (size_t x = 0; x < W; ++x)
      o[x] = static_cast<uint16_t>(static_cast<float>(col[x]) * inv + 0.5f);
    if (y + 1 < H)
    {
      const uint16_t *add = rowp(static_cast<long>(y) + r + 1);
      const uint16_t *sub = rowp(static_cast<long>(y) - r);
      for (size_t x = 0; x < W; ++x)
        col[x] = col[x] + add[x] - sub[x];
    }
  }
  return true;
}

// Blurs the luma of an RGBA8 picture (3 box passes each way). Returns false
// if `latest` stopped matching `gen` part-way (the same cancellation contract
// as renderWith), or if the size or radius is invalid; `out` is then garbage
// and must not be used.
template <class S>
inline bool blurLumaT(const S *rgba, int w, int h, int r, Plane &out,
                      const std::atomic<uint64_t> *latest, uint64_t gen)
{
  if (!rgba || w <= 0 || h <= 0 || r < 1)
    return false;
  const size_t W = static_cast<size_t>(w), H = static_cast<size_t>(h);
  out.resize(W * H);
  for (size_t y = 0; y < H; ++y)
  {
    const S *s = rgba + y * W * 4;
    uint16_t *o = out.data() + y * W;
    for (size_t x = 0; x < W; ++x)
      o[x] = static_cast<uint16_t>(SrcTraits<S>::lumaFix(s + x * 4) + 0.5f);
  }
  const float inv = 1.0f / static_cast<float>(2 * r + 1);
  std::vector<uint16_t> pad(W + 2 * static_cast<size_t>(r));
  for (int pass = 0; pass < 3; ++pass)
    for (size_t y = 0; y < H; ++y)
    {
      if ((y & 15) == 0 && cancelled(latest, gen))
        return false;
      hboxRow(out.data() + y * W, W, r, pad.data(), inv);
    }
  Plane tmp(W * H);
  std::vector<uint32_t> col(W);
  for (int pass = 0; pass < 3; ++pass)
  {
    if (!vbox(out.data(), tmp.data(), W, H, r, col.data(), inv, latest, gen))
      return false;
    out.swap(tmp);
  }
  return true;
}

// The 8-bit and 16-bit versions (the 8-bit one is a plain function, so a null pointer or a pointer
// of another constness still finds it).
inline bool blurLuma(const uint8_t *rgba, int w, int h, int r, Plane &out,
                     const std::atomic<uint64_t> *latest, uint64_t gen)
{
  return blurLumaT<uint8_t>(rgba, w, h, r, out, latest, gen);
}
inline bool blurLuma(const uint16_t *rgba, int w, int h, int r, Plane &out,
                     const std::atomic<uint64_t> *latest, uint64_t gen)
{
  return blurLumaT<uint16_t>(rgba, w, h, r, out, latest, gen);
}

// About 1024 bytes spread over the picture: cheap, and two different pictures
// will almost surely differ at some sampled byte. It is NOT a full hash: a
// small edit that falls between the samples goes unnoticed, which is why the
// Cache below assumes pixels are never edited in place.
template <class S>
inline uint64_t fingerprint(const S *rgba, int w, int h)
{
  uint64_t hash = 1469598103934665603ULL ^
                  (static_cast<uint64_t>(static_cast<uint32_t>(w)) << 32 |
                   static_cast<uint32_t>(h));
  if (sizeof(S) != 1) // a 16-bit picture never shares a key with an 8-bit one
    hash ^= static_cast<uint64_t>(sizeof(S)) * 0x9e3779b97f4a7c15ULL;
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  if (n == 0)
    return hash;
  const size_t stride = (n / 1024) | 1; // odd, so every channel gets sampled
  for (size_t i = 0; i < n; i += stride)
  {
    hash ^= static_cast<uint64_t>(rgba[i]);
    hash *= 1099511628211ULL;
  }
  hash ^= static_cast<uint64_t>(rgba[n - 1]);
  hash *= 1099511628211ULL;
  return hash;
}

// Remembers blurred planes so they are built once per picture, not once per
// render. Keyed on the picture (its pixel pointer, size and a fingerprint, so
// a new image that happens to reuse a freed buffer's address is still told
// apart) and the layer kind; the radius follows from size + kind. Shared by
// the UI thread (drag preview) and the render workers, hence the mutex.
//
// ASSUMPTION: a CanvasImage's pixels are not edited in place once it is being
// rendered (the app builds a new image for every orient / straighten / crop).
// An in-place edit (a retouch brush, say) would leave stale planes: call
// clear() after one.
class Cache
{
public:
  using PlanePtr = std::shared_ptr<const Plane>;

  // Null when cancelled (nothing is cached then) or when the arguments are
  // invalid (no pixels, non-positive size, unknown kind). The picture is RGBA8 or 16-bit RGBA:
  // the plane is built from whatever it is handed (see SrcTraits).
  PlanePtr get(const uint8_t *rgba, int w, int h, int kind,
               const std::atomic<uint64_t> *latest, uint64_t gen)
  {
    return getT<uint8_t>(rgba, w, h, kind, latest, gen);
  }
  PlanePtr get(const uint16_t *rgba, int w, int h, int kind,
               const std::atomic<uint64_t> *latest, uint64_t gen)
  {
    return getT<uint16_t>(rgba, w, h, kind, latest, gen);
  }

private:
  template <class S>
  PlanePtr getT(const S *rgba, int w, int h, int kind,
                const std::atomic<uint64_t> *latest, uint64_t gen)
  {
    if (!rgba || w <= 0 || h <= 0 || kind < 0 || kind >= kKindCount)
      return nullptr;
    const uint64_t fp = fingerprint(rgba, w, h);
    {
      std::lock_guard<std::mutex> lock(m_);
      if (Entry *e = find(rgba, w, h, kind, fp))
      {
        e->used = ++tick_;
        return e->plane;
      }
    }
    // Built outside the lock: a slow blur must not stall the other thread's
    // cache hits. Two threads may rarely build the same plane; one wins.
    auto plane = std::make_shared<Plane>();
    if (!blurLumaT<S>(rgba, w, h, boxRadius(kind, w, h), *plane, latest, gen))
      return nullptr;
    std::lock_guard<std::mutex> lock(m_);
    if (Entry *e = find(rgba, w, h, kind, fp))
    {
      e->used = ++tick_;
      return e->plane;
    }
    bytes_ += plane->size() * sizeof(uint16_t);
    entries_.push_back(Entry{rgba, w, h, kind, fp, plane, ++tick_});
    evict();
    return plane;
  }

public:
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
    const void *rgba;
    int w, h, kind;
    uint64_t fp;
    PlanePtr plane;
    uint64_t used;
  };
  static constexpr size_t kMaxBytes = 384u * 1024u * 1024u;
  static constexpr size_t kMaxEntries = 9;

  Entry *find(const void *rgba, int w, int h, int kind, uint64_t fp)
  {
    for (Entry &e : entries_)
      if (e.rgba == rgba && e.w == w && e.h == h && e.kind == kind &&
          e.fp == fp)
        return &e;
    return nullptr;
  }
  // Drops the least recently used planes (never the newest one) until the
  // cache is under its limits. A plane still held by a render stays alive
  // through its shared_ptr; only the cache's claim on it ends.
  void evict()
  {
    while (entries_.size() > 1 &&
           (bytes_ > kMaxBytes || entries_.size() > kMaxEntries))
    {
      size_t oldest = 0;
      for (size_t i = 1; i < entries_.size(); ++i)
        if (entries_[i].used < entries_[oldest].used)
          oldest = i;
      if (oldest == entries_.size() - 1)
        break;
      bytes_ -= entries_[oldest].plane->size() * sizeof(uint16_t);
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
  }

  mutable std::mutex m_;
  std::vector<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t tick_ = 0;
};

inline Cache &cache()
{
  static Cache c;
  return c;
}

// ---- Applying the layers ----

// Slider values turned into multipliers of the detail layers.
struct Gains
{
  float g[kKindCount] = {0.0f, 0.0f, 0.0f};
  bool any() const { return g[0] != 0.0f || g[1] != 0.0f || g[2] != 0.0f; }
  // Bit k set = this render needs plane k.
  unsigned need() const
  {
    unsigned n = 0;
    for (int k = 0; k < kKindCount; ++k)
      if (g[k] != 0.0f)
        n |= 1u << k;
    return n;
  }
  bool operator==(const Gains &) const = default;
};

// UI values: texture / clarity in -100..100, sharpness in 0..100. Values are
// clamped to those ranges and NaN/inf count as 0 (untouched), so the gains are
// always finite: a NaN gain would turn into a NaN pixel and an undefined cast.
inline Gains gainsFromValues(float texture, float clarity, float sharpness)
{
  auto sane = [](float v, float lo, float hi)
  { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0f; };
  Gains r;
  r.g[Texture] = sane(texture, -100.0f, 100.0f) / 100.0f * kGain[Texture];
  r.g[Clarity] = sane(clarity, -100.0f, 100.0f) / 100.0f * kGain[Clarity];
  r.g[Sharpness] = sane(sharpness, 0.0f, 100.0f) / 100.0f * kGain[Sharpness];
  return r;
}

// The planes one render uses (null = that layer isn't needed).
struct Planes
{
  Cache::PlanePtr p[kKindCount];
};

// Fetches the planes in `need` for the picture at `rgba`. False = cancelled.
// (No pixels or a non-positive size means there is nothing to do: true, with
// the planes left null.)
template <class S>
inline bool acquireT(Planes &out, const S *rgba, int w, int h, unsigned need,
                     const std::atomic<uint64_t> *latest, uint64_t gen)
{
  if (!rgba || w <= 0 || h <= 0)
    return true;
  for (int k = 0; k < kKindCount; ++k)
    if ((need >> k) & 1u)
    {
      out.p[k] = cache().get(rgba, w, h, k, latest, gen);
      if (!out.p[k])
        return false;
    }
  return true;
}
inline bool acquire(Planes &out, const uint8_t *rgba, int w, int h, unsigned need,
                    const std::atomic<uint64_t> *latest, uint64_t gen)
{
  return acquireT<uint8_t>(out, rgba, w, h, need, latest, gen);
}
inline bool acquire(Planes &out, const uint16_t *rgba, int w, int h, unsigned need,
                    const std::atomic<uint64_t> *latest, uint64_t gen)
{
  return acquireT<uint16_t>(out, rgba, w, h, need, latest, gen);
}

// Stores one channel of a finished pixel: a byte gets the value clamped to 0..255 and rounded
// to nearest; a float gets it clamped to 0..255 and NOT rounded (the float pipeline rounds
// once, at the very end). Plain comparisons, no std:: calls, same Debug-build reason as above.
inline void putLevel(uint8_t *d, float v)
{
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  *d = static_cast<uint8_t>(v + 0.5f);
}
inline void putLevel(float *d, float v)
{
  *d = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
}

// Adds the detail term to `count` consecutive pixels of `dst` (RGBA, alpha untouched).
// `Dst` is the destination's sample type: uint8_t (RGBA8 bytes) or float (levels 0..255). `S` is
// the SOURCE's (uint8_t or uint16_t) and is deduced; Dst stays the first template parameter so
// addPixels<Dst>(...) keeps working.
// `src` holds the SOURCE pixels for the same positions (they give each pixel's own luma) and
// `first` is the index, in the whole picture, of the first of them (the planes are indexed
// y * width + x). A plane that doesn't cover [first, first + count) (built for a different
// picture size, say) is skipped instead of being read out of range. Raw pointers and no
// std:: helpers in the loop: same Debug-build reason as applyColorPass.
template <class Dst, class S>
inline void addPixels(const Planes &pl, const Gains &gn, const S *src,
                      Dst *dst, size_t count, size_t first)
{
  using T = SrcTraits<S>;
  auto usable = [&](int k) -> const uint16_t *
  {
    const Cache::PlanePtr &p = pl.p[k];
    return (p && first <= p->size() && count <= p->size() - first)
               ? p->data() + first
               : nullptr;
  };
  const uint16_t *b0 = usable(Texture);
  const uint16_t *b1 = usable(Clarity);
  const uint16_t *b2 = usable(Sharpness);
  const float g0 = b0 ? gn.g[Texture] : 0.0f;
  const float g1 = b1 ? gn.g[Clarity] : 0.0f;
  const float g2 = b2 ? gn.g[Sharpness] : 0.0f;
  if (g0 == 0.0f && g1 == 0.0f && g2 == 0.0f)
    return;
  const float inv16 = T::kInvFix; // (named for the 8-bit case it began as: plane units -> levels)
  const float invLimit = 1.0f / kClarityLimit;
  for (size_t i = 0; i < count; ++i)
  {
    const S *s = src + i * 4;
    float l16, mid; // the pixel's luma in plane units, and Clarity's midtone weight
    T::lumaMid(s, &l16, &mid);
    float k = 0.0f;
    if (g0 != 0.0f)
      k += g0 * (l16 - static_cast<float>(b0[i])) * inv16;
    if (g1 != 0.0f)
    {
      float d = (l16 - static_cast<float>(b1[i])) * inv16;
      d = d / (1.0f + (d < 0.0f ? -d : d) * invLimit);
      k += g1 * mid * d;
    }
    if (g2 != 0.0f)
      k += g2 * (l16 - static_cast<float>(b2[i])) * inv16;
    Dst *d8 = dst + i * 4;
    for (int c = 0; c < 3; ++c)
      putLevel(&d8[c], static_cast<float>(d8[c]) + k);
  }
}

} // namespace detail_layer