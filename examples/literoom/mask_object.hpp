// mask_object.hpp - The Object mask: a coverage bitmap (from a segmentation) stored against
// the original picture, warped into the current one, grown / shrunk and
// feathered. Maps are immutable and shared; the warp and the grown /
// feathered result are memoized.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "geometry.hpp"
#include "mask_raster.hpp"

// Object masks
constexpr int kObjectMapEdge = 1024;       // long side of a stored/warped map
constexpr float kObjectMaxGrow = 0.03f;    // grow = +/-1 -> 3% of long side
constexpr float kObjectMaxFeather = 0.02f; // feather = 1 -> 2% of long side
constexpr size_t kObjectCacheSize = 6;

// ---- Object mask ----
// A coverage bitmap produced by a segmentation (hand-made test map now, a
// SAM-style model later). STORED against the ORIGINAL picture; toSource()
// warps it into the picture on screen. Maps are immutable and shared, and
// equality compares their id, so undo snapshots and setMasks' `m == masks_`
// stay cheap.
struct ObjectMap
{
  int w = 0, h = 0;
  std::vector<uint8_t> a; // coverage 0..255, row-major
  uint64_t id = 0;        // unique per result (and per warp, see below)
};
struct ObjectPrompt
{
  float x = 0, y = 0; // fraction of the picture
  bool fg = true;
  bool operator==(const ObjectPrompt &) const = default;
};

inline uint64_t nextObjectMapId()
{
  static std::atomic<uint64_t> n{1};
  return n++;
}
inline uint64_t hashMix(uint64_t h, double v)
{
  uint64_t b;
  std::memcpy(&b, &v, sizeof b);
  return h ^ (b + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2));
}

// Box-based morphology / blur along one axis. Plain loops, same Debug-build
// reason as the other per-pixel code.
inline void objAxisMorph(const std::vector<uint8_t> &in,
                         std::vector<uint8_t> &out, int w, int h, int r,
                         bool dilate, bool horizontal)
{
  out.resize(in.size());
  const int len = horizontal ? w : h, lines = horizontal ? h : w;
  const size_t step = horizontal ? 1 : static_cast<size_t>(w);
  const size_t lineStep = horizontal ? static_cast<size_t>(w) : 1;
  for (int l = 0; l < lines; ++l)
  {
    const uint8_t *s = in.data() + static_cast<size_t>(l) * lineStep;
    uint8_t *d = out.data() + static_cast<size_t>(l) * lineStep;
    for (int i = 0; i < len; ++i)
    {
      const int lo = i - r < 0 ? 0 : i - r;
      const int hi = i + r > len - 1 ? len - 1 : i + r;
      uint8_t v = dilate ? 0 : 255;
      for (int k = lo; k <= hi; ++k)
      {
        const uint8_t p = s[static_cast<size_t>(k) * step];
        v = dilate ? (p > v ? p : v) : (p < v ? p : v);
      }
      d[static_cast<size_t>(i) * step] = v;
    }
  }
}
inline void objAxisBlur(const std::vector<uint8_t> &in,
                        std::vector<uint8_t> &out, int w, int h, int r,
                        bool horizontal)
{
  out.resize(in.size());
  const int len = horizontal ? w : h, lines = horizontal ? h : w;
  const size_t step = horizontal ? 1 : static_cast<size_t>(w);
  const size_t lineStep = horizontal ? static_cast<size_t>(w) : 1;
  const int cnt = 2 * r + 1;
  for (int l = 0; l < lines; ++l)
  {
    const uint8_t *s = in.data() + static_cast<size_t>(l) * lineStep;
    uint8_t *d = out.data() + static_cast<size_t>(l) * lineStep;
    const auto at = [&](int i)
    {
      i = i < 0 ? 0 : (i > len - 1 ? len - 1 : i);
      return static_cast<int>(s[static_cast<size_t>(i) * step]);
    };
    int sum = 0;
    for (int k = -r; k <= r; ++k)
      sum += at(k);
    for (int i = 0; i < len; ++i)
    {
      d[static_cast<size_t>(i) * step] =
          static_cast<uint8_t>((sum + cnt / 2) / cnt);
      sum += at(i + r + 1) - at(i - r);
    }
  }
}

// Warps a stored map (original orientation) into the picture on screen.
// The result's id mixes in the frame, so anything that caches on the in-view
// shape (compositeTintImage's `compParts == view`) is invalidated by a
// rotate / flip / straighten / crop, not just by a new segmentation.
struct ObjectWarpCache
{
  std::mutex mutex;
  std::vector<std::shared_ptr<const ObjectMap>> entries; // most recent first
  void clear()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }
  size_t size()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return entries.size();
  }
};
inline ObjectWarpCache &objectWarpCache()
{
  static ObjectWarpCache c;
  return c;
}

inline std::shared_ptr<const ObjectMap>
warpObjectMap(const std::shared_ptr<const ObjectMap> &src,
              const GeoFrame &f)
{
  ObjectWarpCache &cache = objectWarpCache();
  uint64_t id = src->id;
  for (double v : {f.m00, f.m01, f.m10, f.m11, f.tx, f.ty, f.origW, f.origH,
                   f.srcW, f.srcH})
    id = hashMix(id, v);
  {
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for (const auto &c : cache.entries)
      if (c->id == id)
        return c;
  }
  const double sc =
      std::min(1.0, static_cast<double>(kObjectMapEdge) /
                        std::max(f.srcW, f.srcH));
  const int dw = std::max(1, static_cast<int>(std::lround(f.srcW * sc)));
  const int dh = std::max(1, static_cast<int>(std::lround(f.srcH * sc)));
  auto out = std::make_shared<ObjectMap>();
  out->w = dw;
  out->h = dh;
  out->id = id;
  out->a.assign(static_cast<size_t>(dw) * static_cast<size_t>(dh), 0);
  const int sw = src->w, sh = src->h;
  const auto at = [&](int x, int y)
  {
    x = x < 0 ? 0 : (x > sw - 1 ? sw - 1 : x);
    y = y < 0 ? 0 : (y > sh - 1 ? sh - 1 : y);
    return static_cast<float>(
        src->a[static_cast<size_t>(y) * static_cast<size_t>(sw) +
               static_cast<size_t>(x)]);
  };
  for (int y = 0; y < dh; ++y)
    for (int x = 0; x < dw; ++x)
    {
      double ox, oy;
      f.toOrig((x + 0.5) * f.srcW / dw, (y + 0.5) * f.srcH / dh, ox, oy);
      if (ox < 0 || oy < 0 || ox >= f.origW || oy >= f.origH)
        continue; // outside the original (straighten's empty corners)
      const double u = ox / f.origW * sw - 0.5, v = oy / f.origH * sh - 0.5;
      const int x0 = static_cast<int>(std::floor(u));
      const int y0 = static_cast<int>(std::floor(v));
      const float fx = static_cast<float>(u - x0),
                  fy = static_cast<float>(v - y0);
      const float top = at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx;
      const float bot = at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx;
      out->a[static_cast<size_t>(y) * static_cast<size_t>(dw) +
             static_cast<size_t>(x)] =
          static_cast<uint8_t>(std::lround(top + (bot - top) * fy));
    }
  const std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries.insert(cache.entries.begin(), out);
  if (cache.entries.size() > kObjectCacheSize)
    cache.entries.pop_back();
  return out;
}

struct ObjectEval : RasterEval
{
};

struct ObjectCacheEntry
{
  uint64_t id;
  float grow, feather;
  ObjectEval eval;
};
struct ObjectEvalCache
{
  std::mutex mutex;
  std::vector<ObjectCacheEntry> entries; // most recent first
  void clear()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
  }
  size_t size()
  {
    const std::lock_guard<std::mutex> lock(mutex);
    return entries.size();
  }
};
inline ObjectEvalCache &objectEvalCache()
{
  static ObjectEvalCache c;
  return c;
}

struct ObjectShape
{
  std::shared_ptr<const ObjectMap> map; // stored: original orientation;
                                        // in-view form: warped
  std::vector<ObjectPrompt> prompts;    // for refinement (step 2)
  float feather = 0.0f;                 // 0..1
  float grow = 0.0f;                    // -1 (shrink) .. +1 (grow)
  bool operator==(const ObjectShape &o) const
  {
    return (map ? map->id : 0) == (o.map ? o.map->id : 0) &&
           prompts == o.prompts && feather == o.feather && grow == o.grow;
  }

  ObjectShape toSource(const GeoFrame &f) const
  {
    ObjectShape s = *this;
    for (ObjectPrompt &p : s.prompts)
    {
      double px, py;
      f.toSrc(p.x * f.origW, p.y * f.origH, px, py);
      p.x = static_cast<float>(px / f.srcW);
      p.y = static_cast<float>(py / f.srcH);
    }
    if (map && map->w > 0 && map->h > 0)
      s.map = warpObjectMap(map, f);
    return s;
  }
  // Only the sliders edit an object; the bitmap and prompts are replaced
  // wholesale by setSelectedObjectMap.
  void applySourceEdit(const ObjectShape & /*before*/,
                       const ObjectShape &after, const GeoFrame &)
  {
    feather = after.feather;
    grow = after.grow;
  }

  // Resolves for one image size: grow/shrink, then feather, then per-row
  // spans. Independent of W/H except for the stored sizes, so the preview
  // and full-size renders share one cached result.
  ObjectEval prepare(int W, int H) const
  {
    ObjectEvalCache &cache = objectEvalCache();
    ObjectEval e;
    e.W = W;
    e.H = H;
    if (!map || map->w <= 0 || map->h <= 0)
      return e;
    {
      const std::lock_guard<std::mutex> lock(cache.mutex);
      for (const auto &c : cache.entries)
        if (c.id == map->id && c.grow == grow && c.feather == feather)
        {
          ObjectEval hit = c.eval;
          hit.W = W;
          hit.H = H;
          return hit;
        }
    }
    const int mw = map->w, mh = map->h, longSide = std::max(mw, mh);
    std::vector<uint8_t> a = map->a, tmp;
    const int rg = static_cast<int>(
        std::lround(std::fabs(grow) * kObjectMaxGrow * longSide));
    if (rg > 0)
    {
      objAxisMorph(a, tmp, mw, mh, rg, grow > 0, true);
      objAxisMorph(tmp, a, mw, mh, rg, grow > 0, false);
    }
    const int rf = static_cast<int>(
        std::lround(feather * kObjectMaxFeather * longSide));
    if (rf > 0)
      for (int pass = 0; pass < 2; ++pass)
      { // two box passes ~ gaussian
        objAxisBlur(a, tmp, mw, mh, rf, true);
        objAxisBlur(tmp, a, mw, mh, rf, false);
      }
    auto lo = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), mw);
    auto hi = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), 0);
    for (int y = 0; y < mh; ++y)
    {
      const uint8_t *row = a.data() + static_cast<size_t>(y) * mw;
      int first = -1, last = -1;
      for (int x = 0; x < mw; ++x)
        if (row[x])
        {
          if (first < 0)
            first = x;
          last = x;
        }
      if (first >= 0)
      {
        (*lo)[static_cast<size_t>(y)] = first;
        (*hi)[static_cast<size_t>(y)] = last + 1;
      }
    }
    e.mw = mw;
    e.mh = mh;
    e.map = std::make_shared<const std::vector<uint8_t>>(std::move(a));
    e.lo = std::move(lo);
    e.hi = std::move(hi);
    const std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries.insert(cache.entries.begin(),
                         ObjectCacheEntry{map->id, grow, feather, e});
    if (cache.entries.size() > kObjectCacheSize)
      cache.entries.pop_back();
    return e;
  }
};
