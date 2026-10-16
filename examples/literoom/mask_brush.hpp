// mask_brush.hpp - The Brush mask: strokes of dabs rasterized into a coverage map, with a
// memo of the last few maps so a slider drag or one more stroke does not
// start over.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "geometry.hpp"
#include "mask_raster.hpp"

// Brush masks (see the "Brush mask" section below). A brush's size is its
// RADIUS as a fraction of the picture's shorter side, like the radial
// mask's radii.
constexpr float kBrushMinSize = 0.005f;    // smallest brush radius
constexpr float kBrushMaxSize = 0.50f;     // largest
constexpr float kBrushDefaultSize = 0.05f; // radius of a fresh brush
constexpr float kBrushSpacing = 0.20f;     // render dab spacing, x radius
constexpr float kBrushPointGap = 0.08f;    // min gap between recorded
                                           // points, x radius
constexpr size_t kBrushMaxStrokes = 512;   // per mask
constexpr size_t kBrushMaxPoints = 20000;  // per stroke
constexpr size_t kBrushLutSize = 1024;     // falloff table resolution
constexpr int kBrushMapEdge = 2048;        // longest side the coverage map
                                           // is rasterized at; bigger
                                           // images upsample it
constexpr size_t kBrushCacheSize = 6;      // memoized coverage maps

// ---- Brush mask ----
// Painted by hand: an ordered list of strokes, each a polyline of points
// drawn with a round brush (its own size / feather / density) that either
// ADDS to the mask or ERASES from it, in the order they were painted. Unlike
// the gradients there is no closed form for the weight at a pixel, so the
// shape is rasterized into a coverage map (BrushEval) that rowSpan /
// rowWeights then just read. Points are stored like the other shapes' centers
// (fractions of the ORIGINAL picture's width / height) and sizes like their
// radii (fraction of its shorter side), so the GeoFrame maps a whole brush
// into the current picture and the same strokes render identically on the
// small preview and the full-size image.
//
// One stroke's coverage is the MAX over its dabs (not a sum), so a stroke
// that crosses itself does not double up. With Flow < 1 the dabs instead
// ACCUMULATE toward the stroke's density cap (so crossing itself does build
// up); strokes then combine in order:
//     paint:  m = m + (1 - m) * c        erase:  m = m * (1 - c)
// where c is the stroke's coverage times its density.
//
// Cost: rasterizing is proportional to the AREA painted, so on a big picture
// the map is built at reduced size (longest side kBrushMapEdge) and read back
// with bilinear interpolation; a mask is smooth anyway, and a 24 MP image
// costs no more than a 3 MP one. The result is memoized (see prepare), so
// dragging an Adjust slider re-rasterizes nothing.
struct BrushPt
{
  float x = 0, y = 0; // fraction of the picture's width / height
  bool operator==(const BrushPt &) const = default;
};

struct BrushStroke
{
  bool erase = false;
  float size = kBrushDefaultSize; // radius, fraction of the SHORTER side
  float feather = 0.5f;           // 0 = hard edge .. 1 = fades from the center
  float density = 1.0f;           // 0..1, the most a stroke can add / remove
  float flow = 1.0f;              // 0..1: 1 = a dab sets coverage (max), lower =
                                  // dabs build up toward `density` (one pass ~ flow)
  std::vector<BrushPt> pts;
  bool operator==(const BrushStroke &) const = default;
};

struct BrushEval : RasterEval
{
};

struct BrushShape
{
  std::vector<BrushStroke> strokes;
  bool operator==(const BrushShape &) const = default;

  // Stored (against the original picture) -> as it sits in the current one.
  // Positions go through the frame, sizes scale with it.
  BrushShape toSource(const GeoFrame &f) const
  {
    BrushShape s = *this;
    const double k = f.radiusScale();
    for (BrushStroke &st : s.strokes)
    {
      st.size = static_cast<float>(st.size * k);
      for (BrushPt &p : st.pts)
      {
        double px, py;
        f.toSrc(p.x * f.origW, p.y * f.origH, px, py);
        p.x = static_cast<float>(px / f.srcW);
        p.y = static_cast<float>(py / f.srcH);
      }
    }
    return s;
  }
  // The inverse, for the pieces a new stroke is made of: a point / a radius
  // drawn on the current picture -> stored form. (No applySourceEdit: strokes
  // are only ever appended, never edited in place, so nothing needs to fold
  // an edit of the in-view form back field by field.)
  static BrushPt pointFromSource(const BrushPt &p, const GeoFrame &f)
  {
    double ox, oy;
    f.toOrig(p.x * f.srcW, p.y * f.srcH, ox, oy);
    return BrushPt{static_cast<float>(ox / f.origW),
                   static_cast<float>(oy / f.origH)};
  }
  static float sizeFromSource(float size, const GeoFrame &f)
  {
    return static_cast<float>(size / f.radiusScale());
  }

  // Memoized (defined below, after the cache): the same strokes at the same
  // size come back without re-rasterizing.
  BrushEval prepare(int W, int H) const;

  // Rasterizes the strokes, in order, into a coverage map. With a `base` (the
  // finished map of this brush's first `first` strokes, same size), only the
  // strokes after those are stamped on top of a copy of it: strokes fold in
  // order, so the result is identical to rasterizing them all.
  BrushEval rasterize(int W, int H, const BrushEval *base = nullptr,
                      size_t first = 0) const
  {
    BrushEval e;
    e.W = W;
    e.H = H;
    if (W <= 0 || H <= 0)
      return e;
    // Map size: the image's, shrunk (aspect kept) so its long side is at most
    // kBrushMapEdge.
    const int longSide = W > H ? W : H;
    const float sc =
        longSide > kBrushMapEdge
            ? static_cast<float>(kBrushMapEdge) / static_cast<float>(longSide)
            : 1.0f;
    const int mw = std::max(1, static_cast<int>(std::lround(W * sc)));
    const int mh = std::max(1, static_cast<int>(std::lround(H * sc)));
    e.mw = mw;
    e.mh = mh;
    const size_t sw = static_cast<size_t>(mw);
    std::shared_ptr<std::vector<uint8_t>> map;
    std::shared_ptr<std::vector<int>> lo, hi;
    if (base && base->map && base->mw == mw && base->mh == mh)
    {
      map = std::make_shared<std::vector<uint8_t>>(*base->map);
      lo = std::make_shared<std::vector<int>>(*base->lo);
      hi = std::make_shared<std::vector<int>>(*base->hi);
    }
    else
    {
      first = 0; // no usable base: start from an empty map
      map = std::make_shared<std::vector<uint8_t>>(
          sw * static_cast<size_t>(mh), uint8_t{0});
      lo = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), mw);
      hi = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), 0);
    }
    const float S = static_cast<float>(mw < mh ? mw : mh);
    const float fW = static_cast<float>(mw), fH = static_cast<float>(mh);
    std::vector<uint8_t> cov; // one stroke's coverage, over its bbox only
    std::array<uint8_t, kBrushLutSize + 1> lut{};
    std::vector<float> covF;                     // Flow < 1: the same box, accumulated in float
    std::array<float, kBrushLutSize + 1> lutA{}; // falloff * per-dab opacity

    for (size_t si = first; si < strokes.size(); ++si)
    {
      const BrushStroke &st = strokes[si];
      if (st.pts.empty())
        continue;
      const float rad = (st.size < kBrushMinSize ? kBrushMinSize : st.size) * S;
      const float r2 = rad * rad;
      const float step = std::max(1.0f, rad * kBrushSpacing);

      // Flow < 1: each dab adds (cap - cov) * falloff * aF. aF is chosen so
      // that one straight pass (about 2 rad / step dabs across the center)
      // adds up to ~flow, however the pointer happened to sample the path.
      const float flow = std::clamp(st.flow, 0.01f, 1.0f);
      const bool accum = flow < 0.999f;
      const float passDabs = std::max(1.0f, 2.0f * rad / step);
      const float aF =
          accum ? 1.0f - std::pow(1.0f - flow, 1.0f / passDabs) : 0.0f;

      // Falloff over squared distance / r^2, density folded in: 1 inside
      // `inner`, easing (smoothstep) to 0 at the rim.
      const float inner = std::clamp(1.0f - st.feather, 0.0f, 0.99f);
      const float dens = std::clamp(st.density, 0.0f, 1.0f);
      const float cap = 255.0f * dens; // accumulation ceiling (flow < 1)
      for (size_t i = 0; i <= kBrushLutSize; ++i)
      {
        const float d = std::sqrt(static_cast<float>(i) /
                                  static_cast<float>(kBrushLutSize));
        float w;
        if (d <= inner)
        {
          w = 1.0f;
        }
        else if (d >= 1.0f)
        {
          w = 0.0f;
        }
        else
        {
          const float t = (d - inner) / (1.0f - inner);
          w = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        lut[i] = static_cast<uint8_t>(std::lround(255.0f * w * dens));
        lutA[i] = w * aF;
      }

      // The stroke's bounding box (points +- radius), clamped to the map.
      float minX = st.pts[0].x * fW, maxX = minX;
      float minY = st.pts[0].y * fH, maxY = minY;
      for (const BrushPt &p : st.pts)
      {
        const float x = p.x * fW, y = p.y * fH;
        minX = x < minX ? x : minX;
        maxX = x > maxX ? x : maxX;
        minY = y < minY ? y : minY;
        maxY = y > maxY ? y : maxY;
      }
      const int bx0 =
          static_cast<int>(std::floor(std::clamp(minX - rad, 0.0f, fW))) - 1;
      const int by0 =
          static_cast<int>(std::floor(std::clamp(minY - rad, 0.0f, fH))) - 1;
      const int bx1 =
          static_cast<int>(std::ceil(std::clamp(maxX + rad, 0.0f, fW))) + 1;
      const int by1 =
          static_cast<int>(std::ceil(std::clamp(maxY + rad, 0.0f, fH))) + 1;
      const int cx0 = bx0 < 0 ? 0 : bx0, cy0 = by0 < 0 ? 0 : by0;
      const int cx1 = bx1 > mw ? mw : bx1, cy1 = by1 > mh ? mh : by1;
      if (cx1 <= cx0 || cy1 <= cy0)
        continue;
      const int bw = cx1 - cx0, bh = cy1 - cy0;
      cov.assign(static_cast<size_t>(bw) * static_cast<size_t>(bh), 0);
      if (accum)
        covF.assign(cov.size(), 0.0f);

      // Stamp one round dab: coverage = max(existing, falloff), or accumulate when flow < 1.
      const float lutScale = static_cast<float>(kBrushLutSize) / r2;
      const auto stamp = [&](float cx, float cy)
      {
        if (cx + rad < static_cast<float>(cx0) ||
            cx - rad > static_cast<float>(cx1) ||
            cy + rad < static_cast<float>(cy0) ||
            cy - rad > static_cast<float>(cy1))
          return; // wholly outside this stroke's box
        int ix0 = static_cast<int>(std::floor(cx - rad - 0.5f));
        int ix1 = static_cast<int>(std::ceil(cx + rad + 0.5f));
        int iy0 = static_cast<int>(std::floor(cy - rad - 0.5f));
        int iy1 = static_cast<int>(std::ceil(cy + rad + 0.5f));
        ix0 = ix0 < cx0 ? cx0 : ix0;
        iy0 = iy0 < cy0 ? cy0 : iy0;
        ix1 = ix1 > cx1 ? cx1 : ix1;
        iy1 = iy1 > cy1 ? cy1 : iy1;
        for (int y = iy0; y < iy1; ++y)
        {
          const float dy = (static_cast<float>(y) + 0.5f) - cy;
          const float dy2 = dy * dy;
          uint8_t *crow = cov.data() + static_cast<size_t>(y - cy0) *
                                           static_cast<size_t>(bw);
          for (int x = ix0; x < ix1; ++x)
          {
            const float dx = (static_cast<float>(x) + 0.5f) - cx;
            const float d2 = dx * dx + dy2;
            if (d2 >= r2)
              continue;
            const size_t li = static_cast<size_t>(d2 * lutScale);
            if (accum)
            {
              float &f = covF[static_cast<size_t>(y - cy0) *
                                  static_cast<size_t>(bw) +
                              static_cast<size_t>(x - cx0)];
              f += (cap - f) * lutA[li];
              continue;
            }
            const uint8_t c = lut[li];
            uint8_t &dst = crow[x - cx0];
            if (c > dst)
              dst = c;
          }
        }
      };

      float px = st.pts[0].x * fW, py = st.pts[0].y * fH;
      if (accum)
      {
        // Dabs at an exact spacing along the path, carried across segments,
        // so the build-up does not depend on how often points were recorded.
        // A stationary click gets one pass worth of dabs.
        const int reps =
            st.pts.size() == 1 ? static_cast<int>(passDabs + 0.5f) : 1;
        for (int r = 0; r < reps; ++r)
          stamp(px, py);
        float next = step; // distance from the last dab to the next one
        for (size_t k = 1; k < st.pts.size(); ++k)
        {
          const float qx = st.pts[k].x * fW, qy = st.pts[k].y * fH;
          const float len = std::hypot(qx - px, qy - py);
          if (len > 0.0f)
          {
            float pos = next;
            for (int guard = 0; pos <= len && guard < (1 << 16); ++guard)
            {
              const float t = pos / len;
              stamp(px + (qx - px) * t, py + (qy - py) * t);
              pos += step;
            }
            next = pos - len;
          }
          px = qx;
          py = qy;
        }
        for (size_t i = 0; i < cov.size(); ++i)
          cov[i] = static_cast<uint8_t>(covF[i] + 0.5f);
      }
      else
      {
        // Walk the polyline in steps of a fraction of the radius (as before).
        stamp(px, py);
        for (size_t k = 1; k < st.pts.size(); ++k)
        {
          const float qx = st.pts[k].x * fW, qy = st.pts[k].y * fH;
          const float len = std::hypot(qx - px, qy - py);
          const int n =
              std::min(static_cast<int>(std::ceil(len / step)), 1 << 16);
          for (int j = 1; j <= n; ++j)
          {
            const float t = static_cast<float>(j) / static_cast<float>(n);
            stamp(px + (qx - px) * t, py + (qy - py) * t);
          }
          px = qx;
          py = qy;
        }
      }
      // Fold this stroke into the mask, and widen the rows' spans.
      for (int y = 0; y < bh; ++y)
      {
        const uint8_t *crow = cov.data() + static_cast<size_t>(y) *
                                               static_cast<size_t>(bw);
        uint8_t *mrow = map->data() + static_cast<size_t>(cy0 + y) * sw +
                        static_cast<size_t>(cx0);
        int first = -1, last = -1;
        for (int x = 0; x < bw; ++x)
        {
          const int c = crow[x];
          if (c == 0)
            continue;
          const int cur = mrow[x];
          if (st.erase)
          {
            mrow[x] = static_cast<uint8_t>(cur - (cur * c + 127) / 255);
          }
          else
          {
            mrow[x] = static_cast<uint8_t>(cur + ((255 - cur) * c + 127) / 255);
            if (first < 0)
              first = x;
            last = x;
          }
        }
        if (first >= 0)
        {
          int &l = (*lo)[static_cast<size_t>(cy0 + y)];
          int &h = (*hi)[static_cast<size_t>(cy0 + y)];
          l = std::min(l, cx0 + first);
          h = std::max(h, cx0 + last + 1);
        }
      }
    }
    e.map = std::move(map);
    e.lo = std::move(lo);
    e.hi = std::move(hi);
    return e;
  }
};

// The last few coverage maps. Two things reuse them:
//  * a render that only changed an Adjust slider (same strokes, same size)
//    gets the very same map back;
//  * a render after painting more (same first strokes, one more or one
//    longer) starts from the map of those first strokes and stamps only what
//    is new, so a pointer event costs one stroke, not the whole brush.
// Keyed on the strokes themselves and the size; the render threads and the UI
// thread all come through here, hence the lock. Maps are immutable once
// built, so sharing them is safe.
struct BrushCacheEntry
{
  BrushShape shape;
  int W = 0, H = 0;
  BrushEval eval;
};
struct BrushCache
{
  std::mutex mutex;
  std::vector<BrushCacheEntry> entries; // most recent first
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
// The one cache of the process; tests empty it between cases.
inline BrushCache &brushCache()
{
  static BrushCache c;
  return c;
}

inline BrushEval BrushShape::prepare(int W, int H) const
{
  BrushCache &cache = brushCache();
  BrushEval base; // the longest cached run of our leading strokes, if any
  size_t baseCount = 0;
  bool haveBase = false;
  {
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for (size_t i = 0; i < cache.entries.size(); ++i)
    {
      const BrushCacheEntry &c = cache.entries[i];
      if (c.W != W || c.H != H)
        continue;
      if (c.shape == *this)
      {
        BrushEval hit = c.eval;
        if (i != 0) // keep the most recently used first
          std::rotate(cache.entries.begin(), cache.entries.begin() + i,
                      cache.entries.begin() + i + 1);
        return hit;
      }
      const size_t n = c.shape.strokes.size();
      if (n < strokes.size() && (!haveBase || n > baseCount) &&
          std::equal(c.shape.strokes.begin(), c.shape.strokes.end(),
                     strokes.begin()))
      {
        base = c.eval; // cheap: shares the maps
        baseCount = n;
        haveBase = true;
        if (i != 0) // ...and keep it alive while it is being built on
          std::rotate(cache.entries.begin(), cache.entries.begin() + i,
                      cache.entries.begin() + i + 1);
        break; // entries are most-recent-first; good enough
      }
    }
  }
  BrushEval ev = haveBase ? rasterize(W, H, &base, baseCount)
                          : rasterize(W, H); // slow part: outside the lock
  const std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries.insert(cache.entries.begin(), BrushCacheEntry{*this, W, H, ev});
  if (cache.entries.size() > kBrushCacheSize)
    cache.entries.pop_back();
  return ev;
}
