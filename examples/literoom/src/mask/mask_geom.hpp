// mask_geom.hpp - The two gradient masks: Radial (rotated ellipse) and Linear (graduated).
// Each shape has its stored form, its mapping through a GeoFrame, and an
// Eval resolved for one image size that says which pixels of a row it
// touches (rowSpan) and their weights (rowWeights). No UI, no pixels of
// its own.
#pragma once
#include <cmath>
#include <cstdint>

#include "core/geometry.hpp"

// Radii are a fraction of the picture's SHORTER side (the original's when
// stored, the current picture's once mapped).
constexpr float kMaskMinRadius = 0.02f; // smallest radial-mask radius
constexpr float kMaskMaxRadius = 1.50f; // largest

// ---- Radial (elliptical) mask ----
// The shape resolved for one image size: pixel-space center, rotation and the
// constants the per-row work needs.
struct RadialEval
{
  float cx = 0, cy = 0;       // center, px
  float c = 1, s = 0;         // cos / sin of the clockwise rotation
  float invRx = 0, invRy = 0; // 1 / radius, px
  float inner = 0;            // normalized distance where the falloff begins
  float invFall = 1;          // 1 / (1 - inner)
  float qa = 0, qb = 0;       // 1/rx^2, 1/ry^2 (row-span quadratic)

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? False if none
  // (the ellipse doesn't reach this row). Exact for the rotated ellipse: for
  // a row at offset dy from the center the outline is the quadratic
  //   A dx^2 + B dy dx + C dy^2 = 1   with det = A C - B^2/4 = qa qb,
  // so its two roots are  dx = (-B dy/2 +- sqrt(A - qa qb dy^2)) / A.
  bool rowSpan(int y, int W, int &x0, int &x1) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float A = c * c * qa + s * s * qb;
    const float B = 2.0f * c * s * (qa - qb);
    const float disc = A - qa * qb * dy * dy;
    if (disc < 0.0f)
      return false;
    const float root = std::sqrt(disc);
    const float lo = (-0.5f * B * dy - root) / A;
    const float hi = (-0.5f * B * dy + root) / A;
    // A pixel is the row's x-th sample at x + 0.5; pad by a pixel each side.
    x0 = static_cast<int>(std::floor(cx + lo)) - 1;
    x1 = static_cast<int>(std::ceil(cx + hi)) + 1;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > W ? W : x1;
    return x1 > x0;
  }

  // Weights for pixels [x0, x1) of row `y`: 1 inside `inner`, easing to 0 at
  // the outline (smoothstep), 0 outside. Plain loop, no std helpers, same
  // Debug-build reason as applyColorPass.
  void rowWeights(int y, int x0, int x1, float *out,
                  const uint8_t * /*pix*/ = nullptr) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    for (int x = x0; x < x1; ++x)
    {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      const float u = (dx * c + dy * s) * invRx;
      const float v = (-dx * s + dy * c) * invRy;
      const float d = std::sqrt(u * u + v * v);
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

struct RadialShape
{
  float cx = 0.5f, cy = 0.5f; // center, fraction of image width / height
  float rx = 0.3f, ry = 0.3f; // half-axes, fraction of the SHORTER image side
  float angle = 0.0f;         // rotation of the rx axis, degrees clockwise
  float feather = 0.5f;       // 0 = hard edge .. 1 = fades from the center
  bool operator==(const RadialShape &) const = default;

  // Inner edge of the falloff, normalized: 1 - feather, kept a hair short of
  // 1 so a "hard" edge is still a (one-percent) ramp and never divides by 0.
  float innerFrac() const
  {
    const float in = 1.0f - feather;
    return in < 0.0f ? 0.0f : (in > 0.99f ? 0.99f : in);
  }

  // Stored (against the original picture) -> as it sits in the current one.
  RadialShape toSource(const GeoFrame &f) const
  {
    RadialShape s = *this;
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
  static RadialShape fromSource(const RadialShape &s, const GeoFrame &f)
  {
    RadialShape r = s;
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
  // Folds an edit made on the source form (`before` -> `after`) into this
  // stored shape, touching only the fields that changed: an unrelated
  // slider never nudges the position through float round-off, and an
  // unchanged mask compares equal (so it makes no undo step).
  void applySourceEdit(const RadialShape &before, const RadialShape &after,
                       const GeoFrame &f)
  {
    if (after.cx != before.cx || after.cy != before.cy)
    {
      const RadialShape p = fromSource(after, f);
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

  RadialEval prepare(int W, int H) const
  {
    const float S = static_cast<float>(W < H ? W : H);
    const float rad = angle * (3.14159265358979f / 180.0f);
    const float pxRx = (rx < kMaskMinRadius ? kMaskMinRadius : rx) * S;
    const float pxRy = (ry < kMaskMinRadius ? kMaskMinRadius : ry) * S;
    RadialEval e;
    e.cx = cx * static_cast<float>(W);
    e.cy = cy * static_cast<float>(H);
    e.c = std::cos(rad);
    e.s = std::sin(rad);
    e.invRx = 1.0f / pxRx;
    e.invRy = 1.0f / pxRy;
    e.qa = e.invRx * e.invRx;
    e.qb = e.invRy * e.invRy;
    e.inner = innerFrac();
    e.invFall = 1.0f / (1.0f - e.inner);
    return e;
  }
};

// ---- Linear (graduated) mask ----
// Weight 1 on one side, easing (smoothstep) to 0 on the other. The shape is a
// center point, a direction `u` (the way the effect fades OUT) and a half
// length: the ramp runs from the line `len` behind the center (weight 1)
// to the line `len` ahead of it (weight 0), perpendicular to `u`.
struct LinearEval
{
  float cx = 0, cy = 0; // center, px
  float ux = 0, uy = 1; // unit direction of fade-out
  float len = 1;        // half length of the ramp, px
  float invLen = 1;     // 1 / len

  // Which pixels [x0, x1) of row `y` can have a non-zero weight? The weight is
  // non-zero exactly where s < 1, and s is linear in x, so that is a
  // half-line of the row (or all / none of it when the axis is vertical).
  bool rowSpan(int y, int W, int &x0, int &x1) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float bound = len - dy * uy; // need  dx * ux < bound
    x0 = 0;
    x1 = W;
    if (std::fabs(ux) < 1e-6f)
      return bound > 0.0f;
    const float edge = bound / ux; // dx = edge is the weight-0 line
    if (ux > 0.0f)
    {
      const int e = static_cast<int>(std::ceil(cx + edge)) + 1;
      x1 = e > W ? W : e;
    }
    else
    {
      const int e = static_cast<int>(std::floor(cx + edge)) - 1;
      x0 = e < 0 ? 0 : e;
    }
    return x1 > x0;
  }

  // Plain loop, no std helpers (same Debug-build reason as RadialEval).
  void rowWeights(int y, int x0, int x1, float *out,
                  const uint8_t * /*pix*/ = nullptr) const
  {
    const float dy = (static_cast<float>(y) + 0.5f) - cy;
    const float base = dy * uy;
    for (int x = x0; x < x1; ++x)
    {
      const float dx = (static_cast<float>(x) + 0.5f) - cx;
      const float s = (dx * ux + base) * invLen; // -1 .. +1 across the ramp
      float w;
      if (s <= -1.0f)
      {
        w = 1.0f;
      }
      else if (s >= 1.0f)
      {
        w = 0.0f;
      }
      else
      {
        const float t = (s + 1.0f) * 0.5f;
        w = 1.0f - t * t * (3.0f - 2.0f * t);
      }
      out[x - x0] = w;
    }
  }
};

struct LinearShape
{
  float cx = 0.5f, cy = 0.5f; // center, fraction of image width / height
  float len = 0.25f;          // half length of the ramp, fraction of the
                              // SHORTER image side
  float angle = 90.0f;        // direction of fade-out, degrees clockwise from
                              // +x in (-180, 180]; 90 = full at the top,
                              // fading towards the bottom
  bool operator==(const LinearShape &) const = default;

  LinearShape toSource(const GeoFrame &f) const
  {
    LinearShape s = *this;
    double px, py;
    f.toSrc(cx * f.origW, cy * f.origH, px, py);
    s.cx = static_cast<float>(px / f.srcW);
    s.cy = static_cast<float>(py / f.srcH);
    s.len = static_cast<float>(len * f.radiusScale());
    s.angle = f.dirToSrc(angle);
    return s;
  }
  static LinearShape fromSource(const LinearShape &s, const GeoFrame &f)
  {
    LinearShape r = s;
    double ox, oy;
    f.toOrig(s.cx * f.srcW, s.cy * f.srcH, ox, oy);
    r.cx = static_cast<float>(ox / f.origW);
    r.cy = static_cast<float>(oy / f.origH);
    r.len = static_cast<float>(s.len / f.radiusScale());
    r.angle = f.dirToOrig(s.angle);
    return r;
  }
  // Same contract as RadialShape::applySourceEdit: only changed fields.
  void applySourceEdit(const LinearShape &before, const LinearShape &after,
                       const GeoFrame &f)
  {
    if (after.cx != before.cx || after.cy != before.cy)
    {
      const LinearShape p = fromSource(after, f);
      cx = p.cx;
      cy = p.cy;
    }
    if (after.len != before.len)
      len = static_cast<float>(after.len / f.radiusScale());
    if (after.angle != before.angle)
      angle = f.dirToOrig(after.angle);
  }

  LinearEval prepare(int W, int H) const
  {
    const float S = static_cast<float>(W < H ? W : H);
    const float rad = angle * (3.14159265358979f / 180.0f);
    LinearEval e;
    e.cx = cx * static_cast<float>(W);
    e.cy = cy * static_cast<float>(H);
    e.ux = std::cos(rad);
    e.uy = std::sin(rad);
    e.len = (len < kMaskMinRadius ? kMaskMinRadius : len) * S;
    e.invLen = 1.0f / e.len;
    return e;
  }
};
