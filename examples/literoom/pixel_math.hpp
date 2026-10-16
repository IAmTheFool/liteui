#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>

// Hermite smoothstep on [e0, e1]: 0 below e0, 1 above e1, smooth between.
// Used to build the tonal-range weights for Highlights/Shadows/Whites/Blacks.
// Requires e1 > e0. NaN x maps to 0.
inline float smoothStep01(float e0, float e1, float x)
{
  assert(e1 > e0);
  if (std::isnan(x))
    return 0.0f;
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// NaN -> 0; +/-inf saturate; finite values round to nearest (halfway up,
// up to float precision of the +0.5 step).
inline uint8_t toU8Clamped(float v)
{
  if (std::isnan(v))
    return 0;
  v = std::clamp(v, 0.0f, 255.0f);
  return static_cast<uint8_t>(v + 0.5f);
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;