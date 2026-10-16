// curves.hpp - Tone curves: control points, the monotone cubic (PCHIP) that turns
// them into a 256-entry table, and the edit operations (insert, move,
// remove) that keep the point list valid.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

constexpr float kCurveMinGap = 2.0f;  // min x distance between points (0..255)
constexpr size_t kCurveMaxPoints = 16;

// ---- Curve types (used by the masks, so they live here) ----
struct CurvePoint
{
  float x = 0.0f, y = 0.0f;
  bool operator==(const CurvePoint &) const = default;
};
using CurvePoints = std::vector<CurvePoint>;

inline CurvePoints identityCurve() { return {{0.0f, 0.0f}, {255.0f, 255.0f}}; }

// Which curve is being edited / stored. Master applies to all three channels
// (first); Red/Green/Blue then apply to their own channel only.
enum class CurveChannel : int
{
  Master,
  Red,
  Green,
  Blue,
  Count
};
constexpr size_t kCurveChannelCount = static_cast<size_t>(CurveChannel::Count);
using CurveSet = std::array<CurvePoints, kCurveChannelCount>;

inline CurveSet identityCurves()
{
  return {identityCurve(), identityCurve(), identityCurve(), identityCurve()};
}

// ---------------- Tone curve ----------------
// Four curves — master (RGB) plus one each for R, G, B. Each is a list of
// control points (input level, output level), both in
// 0..255, sorted by x. The first and last points sit at x = 0 and x = 255
// (their y can move, e.g. to lift blacks). Interpolated with a monotone cubic
// (Fritsch-Carlson / PCHIP): it passes through every point, never overshoots
// between neighbors, and stays monotone wherever the points are — so dragging
// a point can't make the curve ring or invert by accident.
// (CurvePoint, CurvePoints, CurveChannel and CurveSet are defined further up,
// before the masks block: a Mask holds its own CurveSet.)

// Evaluates the curve at every input level 0..255 (unrounded, clamped).
inline std::array<float, 256> curveToLut(const CurvePoints &pts)
{
  std::array<float, 256> lut{};
  const size_t n = pts.size();
  if (n < 2)
  {
    for (int i = 0; i < 256; ++i)
      lut[i] = static_cast<float>(i);
    return lut;
  }
  std::vector<float> h(n - 1), d(n - 1), m(n);
  for (size_t k = 0; k + 1 < n; ++k)
  {
    h[k] = std::max(pts[k + 1].x - pts[k].x, 1e-3f);
    d[k] = (pts[k + 1].y - pts[k].y) / h[k];
  }
  if (n == 2)
  {
    m[0] = m[1] = d[0];
  }
  else
  {
    for (size_t k = 1; k + 1 < n; ++k)
    {
      if (d[k - 1] * d[k] <= 0.0f)
      {
        m[k] = 0.0f; // local extremum or flat: no overshoot
      }
      else
      {
        const float w1 = 2.0f * h[k] + h[k - 1];
        const float w2 = h[k] + 2.0f * h[k - 1];
        m[k] = (w1 + w2) / (w1 / d[k - 1] + w2 / d[k]);
      }
    }
    // Shape-preserving one-sided end slopes.
    auto endSlope = [](float h0, float h1, float d0, float d1)
    {
      float sl = ((2.0f * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
      if (sl * d0 <= 0.0f)
        sl = 0.0f;
      else if (d0 * d1 <= 0.0f && std::fabs(sl) > 3.0f * std::fabs(d0))
        sl = 3.0f * d0;
      return sl;
    };
    m[0] = endSlope(h[0], h[1], d[0], d[1]);
    m[n - 1] = endSlope(h[n - 2], h[n - 3], d[n - 2], d[n - 3]);
  }
  size_t k = 0;
  for (int i = 0; i < 256; ++i)
  {
    const float x = static_cast<float>(i);
    float v;
    if (x <= pts.front().x)
    {
      v = pts.front().y;
    }
    else if (x >= pts.back().x)
    {
      v = pts.back().y;
    }
    else
    {
      while (k + 2 < n && x > pts[k + 1].x)
        ++k;
      const float t = (x - pts[k].x) / h[k];
      const float t2 = t * t, t3 = t2 * t;
      v = (2.0f * t3 - 3.0f * t2 + 1.0f) * pts[k].y +
          (t3 - 2.0f * t2 + t) * h[k] * m[k] +
          (-2.0f * t3 + 3.0f * t2) * pts[k + 1].y + (t3 - t2) * h[k] * m[k + 1];
    }
    lut[i] = std::clamp(v, 0.0f, 255.0f);
  }
  return lut;
}

// True when rounding the curve to 8 bits gives the identity mapping — the
// pipeline skips the curve stage entirely then (exactly equivalent).
inline bool curveIsIdentity(const std::array<float, 256> &lut)
{
  for (int i = 0; i < 256; ++i)
    if (std::lround(lut[i]) != i)
      return false;
  return true;
}

// Inserts an interior point at (x, y), keeping the list sorted. Returns its
// index, or -1 if it would sit on/next to an existing point or an end.
inline int curveInsert(CurvePoints &pts, float x, float y)
{
  x = std::clamp(x, 0.0f, 255.0f);
  y = std::clamp(y, 0.0f, 255.0f);
  size_t i = 0;
  while (i < pts.size() && pts[i].x < x)
    ++i;
  if (i == 0 || i >= pts.size())
    return -1;
  if (x - pts[i - 1].x < kCurveMinGap || pts[i].x - x < kCurveMinGap)
    return -1;
  pts.insert(pts.begin() + static_cast<std::ptrdiff_t>(i), CurvePoint{x, y});
  return static_cast<int>(i);
}

// Moves point `idx`. End points keep their x; interior points stay strictly
// between their neighbors (kCurveMinGap apart), so order never changes.
inline void curveMovePoint(CurvePoints &pts, int idx, float x, float y)
{
  if (idx < 0 || idx >= static_cast<int>(pts.size()))
    return;
  pts[idx].y = std::clamp(y, 0.0f, 255.0f);
  if (idx == 0 || idx == static_cast<int>(pts.size()) - 1)
    return;
  const float lo = pts[idx - 1].x + kCurveMinGap;
  const float hi = pts[idx + 1].x - kCurveMinGap;
  if (lo <= hi)
    pts[idx].x = std::clamp(x, lo, hi);
}

// Removes an interior point; the two end points can't be removed.
inline bool curveRemove(CurvePoints &pts, int idx)
{
  if (idx <= 0 || idx >= static_cast<int>(pts.size()) - 1)
    return false;
  pts.erase(pts.begin() + idx);
  return true;
}