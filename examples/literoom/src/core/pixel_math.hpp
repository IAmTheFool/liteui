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

// ---- 16-bit storage ----
// The working scale of the float pipeline is "levels": 0..255, the same unit
// every constant in the color pass, the filters and the film effects is written
// in. A 16-bit sample v (0..65535) is v / 257 levels, so an 8-bit value k
// widened to k * 257 is EXACTLY k levels again (257 * k / 257 is exact in float):
// a picture that started as 8-bit goes through the float pipeline unchanged.
constexpr float kU16PerLevel = 257.0f;

// Levels -> 16-bit sample, rounded to nearest and clamped. NaN -> 0 (the same
// rule as toU8Clamped). 255.0f gives exactly 65535.
inline uint16_t toU16Clamped(float v)
{
  if (std::isnan(v))
    return 0;
  v = std::clamp(v, 0.0f, 255.0f);
  return static_cast<uint16_t>(v * kU16PerLevel + 0.5f);
}

// 16-bit sample -> levels. A division (not a multiply by 1/257), because only a
// division makes every widened 8-bit value come back as the exact integer.
inline float u16ToLevels(uint16_t v) { return static_cast<float>(v) / kU16PerLevel; }

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;