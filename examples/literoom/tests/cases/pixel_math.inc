// core/pixel_math.hpp
#include "core/pixel_math.hpp"

#include <cstdint>
#include <limits>

TEST_CASE("pixel_math: smoothStep01", "[pixel_math]")
{
  CHECK(smoothStep01(0.2f, 0.8f, -1.0f) == 0.0f); // below: 0
  CHECK(smoothStep01(0.2f, 0.8f, 0.2f) == 0.0f);
  CHECK(smoothStep01(0.2f, 0.8f, 0.8f) == 1.0f);
  CHECK(smoothStep01(0.2f, 0.8f, 5.0f) == 1.0f); // above: 1
  CHECK(smoothStep01(0.0f, 1.0f, 0.5f) == within(0.5, 1e-6));

  // Monotone non-decreasing, and symmetric: s(x) + s(1 - x) == 1.
  float prev = 0.0f;
  for (int i = 0; i <= 1000; ++i)
  {
    const float x = static_cast<float>(i) / 1000.0f;
    const float s = smoothStep01(0.0f, 1.0f, x);
    CHECK(s >= prev);
    CHECK(s + smoothStep01(0.0f, 1.0f, 1.0f - x) == within(1.0, 1e-5));
    prev = s;
  }

  // Flat at both ends (that is what "smooth" means here): much slower than linear.
  CHECK(smoothStep01(0.0f, 1.0f, 0.05f) < 0.01f);
  CHECK(smoothStep01(0.0f, 1.0f, 0.95f) > 0.99f);
  // Works on any interval, not just [0, 1].
  CHECK(smoothStep01(10.0f, 30.0f, 20.0f) == within(0.5, 1e-6));
  // Non-finite input: NaN maps to 0, infinities saturate.
  CHECK(smoothStep01(0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
  CHECK(smoothStep01(0.0f, 1.0f, std::numeric_limits<float>::infinity()) == 1.0f);
  CHECK(smoothStep01(0.0f, 1.0f, -std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("pixel_math: toU8Clamped", "[pixel_math]")
{
  for (int i = 0; i <= 255; ++i)
    CHECK(toU8Clamped(static_cast<float>(i)) == i); // exact on whole levels
  CHECK(toU8Clamped(-5.0f) == 0);
  CHECK(toU8Clamped(-0.4f) == 0);
  CHECK(toU8Clamped(255.4f) == 255);
  CHECK(toU8Clamped(1e9f) == 255);
  CHECK(toU8Clamped(127.4f) == 127); // rounds to nearest...
  CHECK(toU8Clamped(127.5f) == 128); // ...halves go up
  CHECK(toU8Clamped(254.5f) == 255);
  // Non-finite input: NaN -> 0, infinities saturate.
  CHECK(toU8Clamped(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(toU8Clamped(std::numeric_limits<float>::infinity()) == 255);
  CHECK(toU8Clamped(-std::numeric_limits<float>::infinity()) == 0);
}

TEST_CASE("pixel_math: 16-bit conversions", "[pixel_math]")
{
  // An 8-bit value k widened to k * 257 is exactly k levels again, and back.
  for (int k = 0; k <= 255; ++k)
  {
    CHECK(toU16Clamped(static_cast<float>(k)) == k * 257);
    CHECK(u16ToLevels(static_cast<uint16_t>(k * 257)) == static_cast<float>(k));
  }
  // Every 16-bit sample survives levels -> 16-bit unchanged.
  for (int v = 0; v <= 65535; ++v)
    CHECK(toU16Clamped(u16ToLevels(static_cast<uint16_t>(v))) == v);

  // Endpoints and clamping.
  CHECK(u16ToLevels(0) == 0.0f);
  CHECK(u16ToLevels(65535) == 255.0f);
  CHECK(toU16Clamped(255.0f) == 65535);
  CHECK(toU16Clamped(-1.0f) == 0);
  CHECK(toU16Clamped(-0.001f) == 0);
  CHECK(toU16Clamped(255.4f) == 65535);
  CHECK(toU16Clamped(1e9f) == 65535);
  // Rounds to nearest: just under / over half a 16-bit step (1/257 level).
  CHECK(toU16Clamped(100.0f + 0.4f / kU16PerLevel) == 100 * 257);
  CHECK(toU16Clamped(100.0f + 0.6f / kU16PerLevel) == 100 * 257 + 1);
  // Non-finite input: NaN -> 0, infinities saturate (same rule as toU8Clamped).
  CHECK(toU16Clamped(std::numeric_limits<float>::quiet_NaN()) == 0);
  CHECK(toU16Clamped(std::numeric_limits<float>::infinity()) == 65535);
  CHECK(toU16Clamped(-std::numeric_limits<float>::infinity()) == 0);
}

TEST_CASE("pixel_math: kDegToRad", "[pixel_math]")
{
  CHECK(180.0 * kDegToRad == within(3.14159265358979323846, 1e-12));
  CHECK(90.0 * kDegToRad == within(1.57079632679489661923, 1e-12));
}