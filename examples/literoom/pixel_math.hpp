// pixel_math.hpp - Small numeric helpers shared by the pixel pipeline. No dependencies.
// smoothStep01 builds the tonal-range weights and hue blends; toU8Clamped
// is the one rounding rule the per-pixel passes use; kDegToRad is shared by
// the geometry and the mask shapes.
#pragma once
#include <algorithm>
#include <cstdint>

// Hermite smoothstep on [e0, e1]: 0 below e0, 1 above e1, smooth between.
// Used to build the tonal-range weights for Highlights/Shadows/Whites/Blacks.
inline float smoothStep01(float e0, float e1, float x)
{
  const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

inline uint8_t toU8Clamped(float v)
{
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  return static_cast<uint8_t>(v + 0.5f);
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;