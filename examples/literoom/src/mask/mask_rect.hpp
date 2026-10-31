// mask_rect.hpp - The Rectangle mask: a rotated rectangle with a soft edge. Same contract
// as the Radial mask in mask_geom.hpp (center, half-sizes, angle, feather),
// but the outline is a rectangle. Its Eval has an EXACT rowSpan: for a row at
// offset dy from the center, the rectangle's two pairs of edges are two
// intervals of dx, and the span is their intersection. No UI, no pixels of
// its own.
#pragma once
#include <cmath>
#include <cstdint>

#include "core/geometry.hpp"
#include "mask/mask_geom.hpp" // kMaskMinRadius / kMaskMaxRadius

// ---- Rectangle mask ----
// The shape resolved for one image size. A pixel's normalized distance is
//     d = max(|u|, |v|)
// where (u, v) are its coordinates in the rectangle's own axes divided by the
// half-sizes. d <= inner -> weight 1, d >= 1 -> weight 0, smoothstep between
// (the same falloff as RadialEval, so the two feel alike).
struct RectEval
{
  float cx = 0, cy = 0;       // center, px
  float c = 1, s = 0;         // cos / sin of the clockwise rotation
  float rx = 1, ry = 1;       // half-sizes, px
  float invRx = 1, invRy = 1; // 1 / half-size
  float inner = 0;            // normalized distance where the falloff begins
  float invFall = 1;          // 1 / (1 - inner)

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? False if none.
  // The weight is non-zero exactly where |u| < 1 and |v| < 1, and with
  //   u = (dx c + dy s) / rx      v = (-dx s + dy c) / ry
  // each of those is a linear function of dx, so each gives an interval of dx
  // and the row's span is their intersection. Exact up to a tiny rounding
  // tolerance (see `clip`), then padded by a pixel.
  bool rowSpan(int y, int W, int &x0, int &x1) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    float lo = -1e30f, hi = 1e30f;
    // |a dx + b| < R
    const auto clip = [&](float a, float b, float R) -> bool
    {
      // The span must be a SUPERSET of the pixels rowWeights gives weight > 0
      // (MaskComposite::rowSpan and applyMaskToBlock rely on it). The edge is
      // solved here as a linear equation, but rowWeights evaluates the same
      // edge per pixel in another order of operations (FMA, vectorization), so
      // a pixel sitting exactly on the edge can get a ~1e-13 weight while this
      // says "outside". A tiny tolerance on R absorbs that rounding.
      R = R * 1.00001f + 1e-4f;
      if (std::fabs(a) < 1e-6f)
        return std::fabs(b) < R; // the row is parallel to this pair of edges
      float l = (-R - b) / a, h = (R - b) / a;
      if (l > h)
      {
        const float t = l;
        l = h;
        h = t;
      }
      lo = l > lo ? l : lo;
      hi = h < hi ? h : hi;
      return true;
    };
    if (!clip(c, dy * s, rx) || !clip(-s, dy * c, ry) || hi <= lo)
      return false;
    // A pixel is the row's x-th sample at x + 0.5; pad by a pixel each side.
    x0 = static_cast<int>(std::floor(cx + lo)) - 1;
    x1 = static_cast<int>(std::ceil(cx + hi)) + 1;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > W ? W : x1;
    return x1 > x0;
  }

  // Plain loop, no std helpers (same Debug-build reason as RadialEval).
  void rowWeights(int y, int x0, int x1, float *out,
                  const uint8_t * /*pix*/ = nullptr) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    for (int x = x0; x < x1; ++x)
    {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      float u = (dx * c + dy * s) * invRx;
      float v = (-dx * s + dy * c) * invRy;
      u = u < 0.0f ? -u : u;
      v = v < 0.0f ? -v : v;
      const float d = u > v ? u : v;
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
        const float t = (d - inner) * invFall;
        w = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      out[x - x0] = w;
    }
  }
};

struct RectShape
{
  float cx = 0.5f, cy = 0.5f; // center, fraction of image width / height
  float rx = 0.3f, ry = 0.2f; // half-sizes, fraction of the SHORTER image side
  float angle = 0.0f;         // rotation of the rx axis, degrees clockwise
  float feather = 0.2f;       // 0 = hard edge .. 1 = fades from the center
  bool operator==(const RectShape &) const = default;

  // Inner edge of the falloff, normalized: 1 - feather, kept a hair short of
  // 1 so a "hard" edge is still a (one-percent) ramp and never divides by 0.
  float innerFrac() const
  {
    const float in = 1.0f - feather;
    return in < 0.0f ? 0.0f : (in > 0.99f ? 0.99f : in);
  }

  // Stored (against the original picture) -> as it sits in the current one.
  RectShape toSource(const GeoFrame &f) const
  {
    RectShape s = *this;
    double px, py;
    f.toSrc(cx * f.origW, cy * f.origH, px, py);
    s.cx = static_cast<float>(px / f.srcW);
    s.cy = static_cast<float>(py / f.srcH);
    const double k = f.radiusScale();
    s.rx = static_cast<float>(rx * k);
    s.ry = static_cast<float>(ry * k);
    s.angle = f.axisToSrc(angle);
    return s;
  }
  // The inverse: a shape drawn on the current picture -> stored form.
  static RectShape fromSource(const RectShape &s, const GeoFrame &f)
  {
    RectShape r = s;
    double ox, oy;
    f.toOrig(s.cx * f.srcW, s.cy * f.srcH, ox, oy);
    r.cx = static_cast<float>(ox / f.origW);
    r.cy = static_cast<float>(oy / f.origH);
    const double k = f.radiusScale();
    r.rx = static_cast<float>(s.rx / k);
    r.ry = static_cast<float>(s.ry / k);
    r.angle = f.axisToOrig(s.angle);
    return r;
  }
  // Same contract as RadialShape::applySourceEdit: only the changed fields.
  void applySourceEdit(const RectShape &before, const RectShape &after,
                       const GeoFrame &f)
  {
    if (after.cx != before.cx || after.cy != before.cy)
    {
      const RectShape p = fromSource(after, f);
      cx = p.cx;
      cy = p.cy;
    }
    const double k = f.radiusScale();
    if (after.rx != before.rx)
      rx = static_cast<float>(after.rx / k);
    if (after.ry != before.ry)
      ry = static_cast<float>(after.ry / k);
    if (after.angle != before.angle)
      angle = f.axisToOrig(after.angle);
    if (after.feather != before.feather)
      feather = after.feather;
  }

  RectEval prepare(int W, int H) const
  {
    const float S = static_cast<float>(W < H ? W : H);
    const float rad = angle * (3.14159265358979f / 180.0f);
    RectEval e;
    e.cx = cx * static_cast<float>(W);
    e.cy = cy * static_cast<float>(H);
    e.c = std::cos(rad);
    e.s = std::sin(rad);
    e.rx = (rx < kMaskMinRadius ? kMaskMinRadius : rx) * S;
    e.ry = (ry < kMaskMinRadius ? kMaskMinRadius : ry) * S;
    e.invRx = 1.0f / e.rx;
    e.invRy = 1.0f / e.ry;
    e.inner = innerFrac();
    e.invFall = 1.0f / (1.0f - e.inner);
    return e;
  }
};