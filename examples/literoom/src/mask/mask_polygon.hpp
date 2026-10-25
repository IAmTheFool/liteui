// mask_polygon.hpp - The Polygon mask (click-placed polygon or freehand lasso): a closed outline
// of points, filled with the even-odd rule, with an optional soft edge.
// Like the Brush there is no closed form for the weight at a pixel, so the
// outline is rasterized into a coverage map (PolygonEval is a RasterEval) that
// rowSpan / rowWeights read, and the last few maps are memoized so a slider
// drag does not start over. rowSpan comes from the map itself: per map row it
// is exactly the first to the last non-zero pixel, so it is as tight as the
// feathered shape really is. No UI.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "core/geometry.hpp"
#include "mask/mask_raster.hpp"

// Limits and tuning.
constexpr size_t kPolyMaxPoints = 2048; // vertices per polygon
constexpr float kPolyMaxFeather = 0.08f; // soft-edge half width at Feather = 100%,
                                         // as a fraction of the shorter side
constexpr int kPolyMapEdge = 2048;       // longest side the coverage map is
                                         // rasterized at (bigger images upsample)
constexpr size_t kPolyCacheSize = 6;     // memoized coverage maps
constexpr int kPolySubRows = 4;          // scanlines per map row (vertical AA)

// A vertex: fraction of the ORIGINAL picture's width / height when stored,
// of the current picture once mapped. (The geometry helpers below work on any
// unit, so the UI also uses them on document pixels.)
struct PolyPt
{
  float x = 0, y = 0;
  bool operator==(const PolyPt &) const = default;
};

// ---- Geometry helpers (pure, unit-independent) ----

// Even-odd point-in-polygon, the same rule the rasterizer fills with. The
// outline is closed implicitly (last point back to the first).
inline bool polyContains(const std::vector<PolyPt> &pts, float x, float y)
{
  const size_t n = pts.size();
  if (n < 3)
    return false;
  bool in = false;
  for (size_t i = 0, j = n - 1; i < n; j = i++)
  {
    const PolyPt &a = pts[i], &b = pts[j];
    if ((a.y > y) != (b.y > y) &&
        x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y))
      in = !in;
  }
  return in;
}

// Distance from (px, py) to the segment a-b, and the closest point on it.
inline float polySegDist(float px, float py, const PolyPt &a, const PolyPt &b,
                         float *cx = nullptr, float *cy = nullptr)
{
  const float vx = b.x - a.x, vy = b.y - a.y;
  const float l2 = vx * vx + vy * vy;
  float t = l2 > 0.0f ? ((px - a.x) * vx + (py - a.y) * vy) / l2 : 0.0f;
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  const float qx = a.x + vx * t, qy = a.y + vy * t;
  if (cx)
    *cx = qx;
  if (cy)
    *cy = qy;
  return std::hypot(px - qx, py - qy);
}

// The closest edge of the CLOSED outline to (x, y): its index (edge i joins
// point i and point i + 1, the last joins back to point 0), the distance, and
// the closest point on it. -1 with fewer than two points.
inline float polyNearestEdge(const std::vector<PolyPt> &pts, float x, float y,
                             int *edge, float *cx = nullptr,
                             float *cy = nullptr)
{
  const size_t n = pts.size();
  if (edge)
    *edge = -1;
  if (n < 2)
    return -1.0f;
  const size_t edges = n == 2 ? 1 : n;
  float best = 1e30f;
  for (size_t i = 0; i < edges; ++i)
  {
    float qx = 0, qy = 0;
    const float d = polySegDist(x, y, pts[i], pts[(i + 1) % n], &qx, &qy);
    if (d < best)
    {
      best = d;
      if (edge)
        *edge = static_cast<int>(i);
      if (cx)
        *cx = qx;
      if (cy)
        *cy = qy;
    }
  }
  return best;
}

// Ramer-Douglas-Peucker on an OPEN polyline (a freehand lasso): keeps the
// first and last point and every point that sticks out more than `tol` from
// the line through its neighbours. Iterative (no recursion), O(n log n) typical.
inline std::vector<PolyPt> polySimplify(const std::vector<PolyPt> &pts,
                                        float tol)
{
  const size_t n = pts.size();
  if (n < 3)
    return pts;
  std::vector<char> keep(n, 0);
  keep[0] = keep[n - 1] = 1;
  std::vector<std::pair<size_t, size_t>> stack;
  stack.push_back({0, n - 1});
  while (!stack.empty())
  {
    const auto [a, b] = stack.back();
    stack.pop_back();
    if (b <= a + 1)
      continue;
    float worst = -1.0f;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i)
    {
      const float d = polySegDist(pts[i].x, pts[i].y, pts[a], pts[b]);
      if (d > worst)
      {
        worst = d;
        at = i;
      }
    }
    if (worst > tol)
    {
      keep[at] = 1;
      stack.push_back({a, at});
      stack.push_back({at, b});
    }
  }
  std::vector<PolyPt> out;
  for (size_t i = 0; i < n; ++i)
    if (keep[i])
      out.push_back(pts[i]);
  return out;
}

// ---- Polygon mask ----
struct PolygonEval : RasterEval
{
};

struct PolygonShape
{
  std::vector<PolyPt> pts; // fewer than 3 points select nothing
  float feather = 0.1f;    // 0 = hard edge .. 1 = widest soft edge
  bool operator==(const PolygonShape &) const = default;

  // Stored (against the original picture) -> as it sits in the current one.
  PolygonShape toSource(const GeoFrame &f) const
  {
    PolygonShape s = *this;
    for (PolyPt &p : s.pts)
    {
      double px, py;
      f.toSrc(p.x * f.origW, p.y * f.origH, px, py);
      p.x = static_cast<float>(px / f.srcW);
      p.y = static_cast<float>(py / f.srcH);
    }
    return s;
  }
  // A point drawn on the current picture -> stored form.
  static PolyPt pointFromSource(const PolyPt &p, const GeoFrame &f)
  {
    double ox, oy;
    f.toOrig(p.x * f.srcW, p.y * f.srcH, ox, oy);
    return PolyPt{static_cast<float>(ox / f.origW),
                  static_cast<float>(oy / f.origH)};
  }
  // Folds an edit made on the in-view form (`before` -> `after`) into this
  // stored shape. Only what changed is touched: an unrelated slider never
  // nudges a vertex through float round-off, a dragged vertex moves alone, and
  // an unchanged shape compares equal (so it makes no undo step). If the
  // number of points changed (a vertex was added or removed) every point is
  // mapped back from `after`.
  void applySourceEdit(const PolygonShape &before, const PolygonShape &after,
                       const GeoFrame &f)
  {
    if (after.feather != before.feather)
      feather = after.feather;
    if (after.pts.size() != before.pts.size() ||
        after.pts.size() != pts.size())
    {
      pts.clear();
      pts.reserve(after.pts.size());
      for (const PolyPt &p : after.pts)
        pts.push_back(pointFromSource(p, f));
      return;
    }
    for (size_t i = 0; i < pts.size(); ++i)
      if (!(after.pts[i] == before.pts[i]))
        pts[i] = pointFromSource(after.pts[i], f);
  }

  // Memoized (defined below, after the cache).
  PolygonEval prepare(int W, int H) const;

  // Rasterizes the outline into a coverage map (see the file comment).
  PolygonEval rasterize(int W, int H) const;
};

// ---- Rasterizing ----
namespace polydetail
{
  // Sizes of three box blurs that together approximate a Gaussian of `sigma`
  // (the standard "boxes for gauss" recipe); returns their RADII.
  inline void boxRadii(float sigma, int out[3])
  {
    const float n = 3.0f;
    const float wIdeal = std::sqrt(12.0f * sigma * sigma / n + 1.0f);
    int wl = static_cast<int>(std::floor(wIdeal));
    if (wl % 2 == 0)
      --wl;
    if (wl < 1)
      wl = 1;
    const int wu = wl + 2;
    const float fl = static_cast<float>(wl);
    const float mIdeal = (12.0f * sigma * sigma - n * fl * fl - 4.0f * n * fl - 3.0f * n) /
                         (-4.0f * fl - 4.0f);
    const int m = static_cast<int>(std::lround(mIdeal));
    for (int i = 0; i < 3; ++i)
      out[i] = ((i < m ? wl : wu) - 1) / 2;
  }

  // One box blur pass of radius r (window 2r + 1, zero outside), along rows
  // (stride 1) or columns (stride w). Running sum, so the cost is independent
  // of r.
  inline void boxBlur(const float *src, float *dst, int w, int h, int r,
                      bool horizontal)
  {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const int lines = horizontal ? h : w;
    const int len = horizontal ? w : h;
    const size_t step = horizontal ? 1 : static_cast<size_t>(w);
    for (int l = 0; l < lines; ++l)
    {
      const size_t base = horizontal ? static_cast<size_t>(l) * static_cast<size_t>(w)
                                     : static_cast<size_t>(l);
      const float *s = src + base;
      float *d = dst + base;
      float sum = 0.0f;
      for (int i = 0; i <= r && i < len; ++i)
        sum += s[static_cast<size_t>(i) * step];
      for (int i = 0; i < len; ++i)
      {
        d[static_cast<size_t>(i) * step] = sum * inv;
        const int add = i + r + 1, rem = i - r;
        if (add < len)
          sum += s[static_cast<size_t>(add) * step];
        if (rem >= 0)
          sum -= s[static_cast<size_t>(rem) * step];
      }
    }
  }
} // namespace polydetail

inline PolygonEval PolygonShape::rasterize(int W, int H) const
{
  PolygonEval e;
  e.W = W;
  e.H = H;
  if (W <= 0 || H <= 0)
    return e;
  // Map size: the image's, shrunk (aspect kept) so its long side is at most
  // kPolyMapEdge.
  const int longSide = W > H ? W : H;
  const float sc = longSide > kPolyMapEdge
                       ? static_cast<float>(kPolyMapEdge) / static_cast<float>(longSide)
                       : 1.0f;
  const int mw = std::max(1, static_cast<int>(std::lround(W * sc)));
  const int mh = std::max(1, static_cast<int>(std::lround(H * sc)));
  e.mw = mw;
  e.mh = mh;
  if (pts.size() < 3)
    return e; // nothing selected: no map, rowSpan is false everywhere

  const size_t n = pts.size();
  const float fW = static_cast<float>(mw), fH = static_cast<float>(mh);
  std::vector<float> vx(n), vy(n);
  float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
  for (size_t i = 0; i < n; ++i)
  {
    vx[i] = pts[i].x * fW;
    vy[i] = pts[i].y * fH;
    minX = vx[i] < minX ? vx[i] : minX;
    maxX = vx[i] > maxX ? vx[i] : maxX;
    minY = vy[i] < minY ? vy[i] : minY;
    maxY = vy[i] > maxY ? vy[i] : maxY;
  }

  // Soft edge: a Gaussian-like blur whose reach (about 2 sigma) is `r` px.
  const float S = static_cast<float>(mw < mh ? mw : mh);
  const float r = std::clamp(feather, 0.0f, 1.0f) * kPolyMaxFeather * S;
  int radii[3] = {0, 0, 0};
  const bool blur = r >= 0.5f;
  int pad = 0;
  if (blur)
  {
    polydetail::boxRadii(r * 0.5f, radii);
    pad = radii[0] + radii[1] + radii[2] + 2;
  }

  // The canvas the outline is drawn on and blurred in: the part of the plane
  // that can influence the map ([-pad, map + pad)) and that the outline's
  // bounding box (+ pad) can reach. Wider than the map so a polygon that runs
  // off the picture does not fade out at the picture's edge.
  const int ox = std::max(-pad, static_cast<int>(std::floor(minX)) - pad);
  const int oy = std::max(-pad, static_cast<int>(std::floor(minY)) - pad);
  const int ex = std::min(mw + pad, static_cast<int>(std::ceil(maxX)) + pad + 1);
  const int ey = std::min(mh + pad, static_cast<int>(std::ceil(maxY)) + pad + 1);
  const int cw = ex - ox, ch = ey - oy;
  if (cw <= 0 || ch <= 0)
    return e;

  std::vector<float> cov(static_cast<size_t>(cw) * static_cast<size_t>(ch), 0.0f);
  {
    std::vector<float> xs;
    const float sub = 1.0f / static_cast<float>(kPolySubRows);
    for (int j = 0; j < ch; ++j)
    {
      float *row = cov.data() + static_cast<size_t>(j) * static_cast<size_t>(cw);
      for (int k = 0; k < kPolySubRows; ++k)
      {
        const float yc = static_cast<float>(oy + j) + (static_cast<float>(k) + 0.5f) * sub;
        if (yc < minY || yc > maxY)
          continue;
        xs.clear();
        for (size_t i = 0, p = n - 1; i < n; p = i++)
        {
          const float y0 = vy[p], y1 = vy[i];
          if ((y0 <= yc) != (y1 <= yc))
            xs.push_back(vx[p] + (yc - y0) / (y1 - y0) * (vx[i] - vx[p]));
        }
        std::sort(xs.begin(), xs.end());
        // Even-odd: fill between crossing 0-1, 2-3, ... with x coverage.
        for (size_t q = 0; q + 1 < xs.size(); q += 2)
        {
          float a = xs[q] - static_cast<float>(ox);
          float b = xs[q + 1] - static_cast<float>(ox);
          a = a < 0.0f ? 0.0f : a;
          b = b > static_cast<float>(cw) ? static_cast<float>(cw) : b;
          if (b <= a)
            continue;
          const int ia = static_cast<int>(std::floor(a));
          const int ib = static_cast<int>(std::floor(b));
          if (ia == ib)
          {
            row[ia] += (b - a) * sub;
            continue;
          }
          row[ia] += (static_cast<float>(ia + 1) - a) * sub;
          for (int x = ia + 1; x < ib; ++x)
            row[x] += sub;
          if (ib < cw)
            row[ib] += (b - static_cast<float>(ib)) * sub;
        }
      }
    }
  }

  if (blur)
  {
    std::vector<float> tmp(cov.size());
    for (int pass = 0; pass < 3; ++pass)
    {
      polydetail::boxBlur(cov.data(), tmp.data(), cw, ch, radii[pass], true);
      polydetail::boxBlur(tmp.data(), cov.data(), cw, ch, radii[pass], false);
    }
  }

  // Quantize into the map, and record each row's exact non-zero extent.
  auto map = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(mw) * static_cast<size_t>(mh), uint8_t{0});
  auto lo = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), mw);
  auto hi = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), 0);
  const int xa = std::max(0, ox), xb = std::min(mw, ox + cw);
  for (int y = std::max(0, oy); y < std::min(mh, oy + ch); ++y)
  {
    const float *crow = cov.data() + static_cast<size_t>(y - oy) * static_cast<size_t>(cw);
    uint8_t *mrow = map->data() + static_cast<size_t>(y) * static_cast<size_t>(mw);
    int first = -1, last = -1;
    for (int x = xa; x < xb; ++x)
    {
      float v = crow[x - ox];
      v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      const uint8_t q = static_cast<uint8_t>(v * 255.0f + 0.5f);
      if (q == 0)
        continue;
      mrow[x] = q;
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
  e.map = std::move(map);
  e.lo = std::move(lo);
  e.hi = std::move(hi);
  return e;
}

// The last few coverage maps (the same idea as the Brush's cache): a render
// that only changed an Adjust slider gets the very same map back. Keyed on the
// outline, the feather and the size; the render threads and the UI thread all
// come through here, hence the lock. Maps are immutable once built.
struct PolygonCacheEntry
{
  PolygonShape shape;
  int W = 0, H = 0;
  PolygonEval eval;
};
struct PolygonCache
{
  std::mutex mutex;
  std::vector<PolygonCacheEntry> entries; // most recent first
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
inline PolygonCache &polygonCache()
{
  static PolygonCache c;
  return c;
}

inline PolygonEval PolygonShape::prepare(int W, int H) const
{
  PolygonCache &cache = polygonCache();
  {
    const std::lock_guard<std::mutex> lock(cache.mutex);
    for (size_t i = 0; i < cache.entries.size(); ++i)
    {
      const PolygonCacheEntry &c = cache.entries[i];
      if (c.W != W || c.H != H || !(c.shape == *this))
        continue;
      PolygonEval hit = c.eval;
      if (i != 0) // keep the most recently used first
        std::rotate(cache.entries.begin(), cache.entries.begin() + i,
                    cache.entries.begin() + i + 1);
      return hit;
    }
  }
  PolygonEval ev = rasterize(W, H); // slow part: outside the lock
  const std::lock_guard<std::mutex> lock(cache.mutex);
  cache.entries.insert(cache.entries.begin(), PolygonCacheEntry{*this, W, H, ev});
  if (cache.entries.size() > kPolyCacheSize)
    cache.entries.pop_back();
  return ev;
}