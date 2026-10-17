// geometry.hpp - Picture geometry: CropRect, Orient, rotated size, the GeoFrame that
// maps the original picture to the oriented / straightened / cropped one,
// and the rotate / flip algebra the buttons use. Pure math, no pixels.
#pragma once
#include <algorithm>
#include <cmath>

#include "pixel_math.hpp"

// Straighten (arbitrary-angle rotation) range in degrees; positive turns the
// picture clockwise. Combined with the 90-degree buttons this reaches any
// angle.
constexpr float kAngleMax = 45.0f;

// ---------------- Picture geometry (shared by masks and the pipeline) ----
// A crop region in pixels of the oriented, straightened image (see
// EditorState::rotated). A default-constructed CropRect (w == h == 0) means
// "no crop — use the whole image".
struct CropRect
{
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool operator==(const CropRect &) const = default;
};

// Orientation of the image: an optional horizontal mirror, then `rot`
// clockwise quarter turns. Together these cover all 8 ways to rotate/flip a
// picture, so any sequence of rotate/flip buttons collapses into one value
// (e.g. flip-H then flip-V is just rot = 2, no matter how often it's done).
struct Orient
{
  int rot = 0;       // 0..3 clockwise quarter turns, applied after the flip
  bool flip = false; // mirror left-right first
  bool identity() const { return rot == 0 && !flip; }
  bool operator==(const Orient &) const = default;
};

struct Dims
{
  int w, h;
};

// Size of a w x h picture after turning it `deg` degrees (either direction).
//   expand: the bounding box of the turned picture — nothing is lost, the
//           corners are empty.
//   fit:    the largest axis-aligned rectangle with the picture's own aspect
//           ratio that fits inside the turned picture — no empty corners.
// Both formulas are symmetric in (w, h), so a picture that is later turned a
// further quarter-turn gets exactly the transposed size — which is what
// lets a crop rectangle follow a 90-degree turn by simple index math.
// A non-positive size is returned as at least 1 x 1, and a NaN/infinite angle
// counts as 0 (no turn).
inline Dims rotatedDims(int w, int h, float deg, bool expand)
{
  if (w <= 0 || h <= 0)
    return {std::max(1, w), std::max(1, h)};
  if (deg == 0.0f || !std::isfinite(deg))
    return {w, h};
  const double rad = static_cast<double>(deg) * kDegToRad;
  const double c = std::abs(std::cos(rad)), sn = std::abs(std::sin(rad));
  const double dw = w, dh = h;
  constexpr double eps = 1e-6; // keeps exact quarter-turns from rounding up
  if (expand)
    return {std::max(1, static_cast<int>(std::ceil(dw * c + dh * sn - eps))),
            std::max(1, static_cast<int>(std::ceil(dw * sn + dh * c - eps)))};
  const double sc = std::min(dw / (dw * c + dh * sn), dh / (dw * sn + dh * c));
  return {std::max(1, static_cast<int>(std::floor(sc * dw + eps))),
          std::max(1, static_cast<int>(std::floor(sc * dh + eps)))};
}

// Where a point of the ORIGINAL picture ends up after orient -> straighten ->
// crop (the order syncSource runs them), as one similarity transform in
// continuous pixel coordinates (pixel centers at +0.5):
//
//     p_source = L * p_original + t
//
// L is orthogonal (a rotation, or a mirror followed by a rotation), so
// lengths are preserved and only positions and axis directions change. Masks
// are stored against the ORIGINAL picture and mapped through this to wherever
// the picture currently is, so no geometry change ever has to touch a mask.
struct GeoFrame
{
  double m00 = 1, m01 = 0, m10 = 0, m11 = 1, tx = 0, ty = 0;
  double origW = 1, origH = 1; // the untouched picture, px
  double srcW = 1, srcH = 1;   // after orient / straighten / crop, px

  double det() const { return m00 * m11 - m01 * m10; }
  void toSrc(double x, double y, double &ox, double &oy) const
  {
    ox = m00 * x + m01 * y + tx;
    oy = m10 * x + m11 * y + ty;
  }
  void toOrig(double x, double y, double &ox, double &oy) const
  {
    const double px = x - tx, py = y - ty, d = det();
    ox = (m11 * px - m01 * py) / d;
    oy = (-m10 * px + m00 * py) / d;
  }
  // Ratio of the two pictures' shorter sides: radii are stored as a fraction
  // of the original's, but shown against the current picture's.
  double radiusScale() const
  {
    return std::min(origW, origH) / std::min(srcW, srcH);
  }
  // Direction of an axis (degrees, clockwise) in the original -> in the
  // source. An axis has no head or tail, so the result is kept in [-90, 90].
  // Any finite angle is accepted (a NaN/infinite one gives 0). This used to be
  // a subtract-180 loop, which never terminated for huge or infinite angles.
  static float wrapAxis(float a)
  {
    if (!std::isfinite(a))
      return 0.0f;
    a = std::fmod(a, 180.0f); // now in (-180, 180)
    if (a > 90.0f)
      a -= 180.0f;
    else if (a < -90.0f)
      a += 180.0f;
    return a == 0.0f ? 0.0f : a; // never -0
  }
  // An arrow's direction kept in (-180, 180]; NaN/infinite gives 0.
  static float wrapDir(float a)
  {
    if (!std::isfinite(a))
      return 0.0f;
    return a <= -180.0f ? 180.0f : a;
  }
  float axisToSrc(float deg) const
  {
    const double phi = std::atan2(m10, m00) / kDegToRad; // where +x goes
    return wrapAxis(static_cast<float>(det() > 0 ? deg + phi : phi - deg));
  }
  float axisToOrig(float deg) const
  {
    const double phi = std::atan2(m10, m00) / kDegToRad;
    return wrapAxis(static_cast<float>(det() > 0 ? deg - phi : phi - deg));
  }
  // Direction of an arrow (degrees clockwise from +x, kept in (-180, 180]) in
  // the original -> in the source and back. Unlike an axis, an arrow has a
  // head, so a mirror or a half turn must not fold it onto its opposite: the
  // unit vector is pushed through the linear part and the angle re-read.
  float dirToSrc(float deg) const
  {
    const double a = static_cast<double>(deg) * kDegToRad;
    const double vx = std::cos(a), vy = std::sin(a);
    return wrapDir(static_cast<float>(
        std::atan2(m10 * vx + m11 * vy, m00 * vx + m01 * vy) / kDegToRad));
  }
  float dirToOrig(float deg) const
  {
    const double a = static_cast<double>(deg) * kDegToRad;
    const double vx = std::cos(a), vy = std::sin(a), d = det();
    return wrapDir(static_cast<float>(
        std::atan2((-m10 * vx + m00 * vy) / d, (m11 * vx - m01 * vy) / d) /
        kDegToRad));
  }
};

// Builds the frame for a picture of ow x oh with this geometry. With no
// original size known yet (ow <= 0) the frame is the identity and masks pass
// through unchanged. `o.rot` is taken modulo 4 and a NaN/infinite angle counts
// as 0, so no input can reach an undefined conversion.
inline GeoFrame makeGeoFrame(int ow, int oh, const Orient &o, float angle,
                             bool expand, const CropRect &crop)
{
  GeoFrame f;
  if (ow <= 0 || oh <= 0)
    return f;
  if (!std::isfinite(angle))
    angle = 0.0f;
  const int rot = ((o.rot % 4) + 4) % 4;
  f.origW = ow;
  f.origH = oh;
  int w = ow, h = oh;
  // Composes p' = A p + b after everything so far.
  auto post = [&f](double a00, double a01, double a10, double a11, double b0,
                   double b1)
  {
    const double n00 = a00 * f.m00 + a01 * f.m10;
    const double n01 = a00 * f.m01 + a01 * f.m11;
    const double n10 = a10 * f.m00 + a11 * f.m10;
    const double n11 = a10 * f.m01 + a11 * f.m11;
    const double t0 = a00 * f.tx + a01 * f.ty + b0;
    const double t1 = a10 * f.tx + a11 * f.ty + b1;
    f.m00 = n00, f.m01 = n01, f.m10 = n10, f.m11 = n11, f.tx = t0, f.ty = t1;
  };
  if (o.flip) // orientImage: mirror first...
    post(-1, 0, 0, 1, w, 0);
  for (int i = 0; i < rot; ++i)
  {                          // ...then clockwise quarter turns
    post(0, -1, 1, 0, h, 0); // (x, y) -> (h - y, x)
    std::swap(w, h);
  }
  if (angle != 0.0f)
  { // rotateArbitrary: clockwise about the center
    const Dims d = rotatedDims(w, h, angle, expand);
    const double rad = static_cast<double>(angle) * kDegToRad;
    const double c = std::cos(rad), s = std::sin(rad);
    post(c, -s, s, c, d.w / 2.0 - (c * w / 2.0 - s * h / 2.0),
         d.h / 2.0 - (s * w / 2.0 + c * h / 2.0));
    w = d.w, h = d.h;
  }
  if (!crop.empty())
  { // cropImage's clamping, exactly (the sums are done in 64 bits so a huge
    // crop can't overflow int)
    const int x0 = std::clamp(crop.x, 0, w), y0 = std::clamp(crop.y, 0, h);
    const long long sx = static_cast<long long>(crop.x) + crop.w;
    const long long sy = static_cast<long long>(crop.y) + crop.h;
    const int x1 = static_cast<int>(std::min<long long>(std::max<long long>(sx, x0), w));
    const int y1 = static_cast<int>(std::min<long long>(std::max<long long>(sy, y0), h));
    post(1, 0, 0, 1, -x0, -y0);
    w = x1 - x0, h = y1 - y0;
  }
  f.srcW = std::max(1, w);
  f.srcH = std::max(1, h);
  return f;
}

enum class GeoOp
{
  RotateCW,
  RotateCCW,
  FlipH,
  FlipV
};

// Applies one rotate/flip to an orientation, and moves the crop rectangle
// with it so the same part of the picture stays selected. (W, H) is the size
// of the straightened image BEFORE the op — the crop lives inside it — and is
// updated to the size after.
//
// Orientation algebra: a mirror reverses the direction of rotation, so
// mirroring a (flip, rot) result gives flip toggled and rot negated; a
// vertical mirror is a horizontal one plus a half turn. The straighten angle
// is a clockwise turn too, so a mirror also negates it (a picture tilted 5
// degrees clockwise, mirrored, is tilted 5 degrees counter-clockwise), while
// a quarter-turn leaves it alone — turns about the same center commute.
inline void geoStep(Orient &o, float &angle, CropRect &c, int &W, int &H,
                    GeoOp op)
{
  o.rot = ((o.rot % 4) + 4) % 4; // tolerate an out-of-range rot
  switch (op)
  {
  case GeoOp::RotateCW:
    o.rot = (o.rot + 1) % 4;
    if (!c.empty())
      c = CropRect{H - c.y - c.h, c.x, c.h, c.w};
    std::swap(W, H);
    break;
  case GeoOp::RotateCCW:
    o.rot = (o.rot + 3) % 4;
    if (!c.empty())
      c = CropRect{c.y, W - c.x - c.w, c.h, c.w};
    std::swap(W, H);
    break;
  case GeoOp::FlipH:
    o.rot = (4 - o.rot) % 4;
    o.flip = !o.flip;
    if (angle != 0.0f)
      angle = -angle;
    if (!c.empty())
      c.x = W - c.x - c.w;
    break;
  case GeoOp::FlipV:
    o.rot = (4 - o.rot + 2) % 4;
    o.flip = !o.flip;
    if (angle != 0.0f)
      angle = -angle;
    if (!c.empty())
      c.y = H - c.y - c.h;
    break;
  }
}

// Angles are kept to 0.1 degree, and to exactly 0 near zero. A NaN/infinite
// angle becomes 0.
inline float snapAngle(float deg)
{
  if (!std::isfinite(deg))
    return 0.0f;
  deg = std::clamp(std::round(deg * 10.0f) / 10.0f, -kAngleMax, kAngleMax);
  return deg == 0.0f ? 0.0f : deg;
}