// cases/image16.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace image16_cases
// so they cannot collide with other cases.
//
// image16_test.cpp - the 16-bit storage and its conversions: every value round-trips, an 8-bit
// picture widened and brought back is untouched, and 8-bit data is EXACT float levels.

#include "core/image16.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

namespace image16_cases
{
TEST_CASE("image16: widenNarrow", "[image16]")
{
  for (int v = 0; v < 256; ++v)
  {
    const uint16_t w = widen8(static_cast<uint8_t>(v));
    CHECK((w == v * 257 && narrow16(w) == v));
    CHECK(u16ToLevels(w) == static_cast<float>(v)); // exact, not just close
    CHECK(toU16Clamped(static_cast<float>(v)) == w);
  }
  CHECK((widen8(255) == 65535 && widen8(0) == 0));
  // narrow16 is the nearest byte, and the same rule as the float path's toU8Clamped.
  for (uint32_t v = 0; v <= 65535; ++v)
  {
    const uint16_t s = static_cast<uint16_t>(v);
    CHECK(narrow16(s) == toU8Clamped(u16ToLevels(s)));
    // 16-bit -> levels -> 16-bit is lossless for every sample.
    CHECK(toU16Clamped(u16ToLevels(s)) == s);
  }
}

TEST_CASE("image16: roundingRule", "[image16]")
{
  CHECK((toU16Clamped(255.0f) == 65535 && toU16Clamped(0.0f) == 0));
  CHECK((toU16Clamped(-5.0f) == 0 && toU16Clamped(300.0f) == 65535));
  CHECK(toU16Clamped(std::nanf("")) == 0);
  CHECK((toU16Clamped(INFINITY) == 65535 && toU16Clamped(-INFINITY) == 0));
  CHECK((toU16Clamped(0.5f / 257.0f) == 1 || toU16Clamped(0.5f / 257.0f) == 0)); // a tie lands on one side, never elsewhere
  CHECK(toU16Clamped(1.0f / 257.0f) == 1);
  CHECK(toU16Clamped(128.0f) == 128 * 257);
}

TEST_CASE("image16: rowConversions", "[image16]")
{
  std::vector<uint8_t> b(256);
  for (int i = 0; i < 256; ++i)
    b[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
  std::vector<uint16_t> w(256);
  std::vector<uint8_t> back(256);
  widenSamples(b.data(), w.data(), 256);
  narrowSamples(w.data(), back.data(), 256);
  CHECK(back == b);
  // 8-bit -> float -> 8-bit, and 16-bit -> float -> 16-bit, through the row helpers.
  std::vector<float> f(256);
  samplesToLevels(b.data(), f.data(), 256);
  std::vector<uint8_t> b2(256);
  levelsToSamples(f.data(), b2.data(), 256);
  CHECK(b2 == b);
  samplesToLevels(w.data(), f.data(), 256); // widened 8-bit data is exactly the same levels
  for (int i = 0; i < 256; ++i)
    CHECK(f[static_cast<size_t>(i)] == static_cast<float>(i));
  std::vector<uint16_t> w2(256);
  levelsToSamples(f.data(), w2.data(), 256);
  CHECK(w2 == w);
  // Out-of-range floats clamp instead of wrapping.
  const float wild[4] = {-40.0f, 255.4f, 1e9f, std::nanf("")};
  uint8_t o8[4];
  uint16_t o16[4];
  levelsToSamples(wild, o8, 4);
  levelsToSamples(wild, o16, 4);
  CHECK((o8[0] == 0 && o8[1] == 255 && o8[2] == 255 && o8[3] == 0));
  CHECK((o16[0] == 0 && o16[1] == 65535 && o16[2] == 65535 && o16[3] == 0));
}

TEST_CASE("image16: image16", "[image16]")
{
  const Image16 a(5, 3);
  CHECK((a.width == 5 && a.height == 3 && a.pixels.size() == 60 && a.pixelCount() == 15 && !a.empty()));
  CHECK((a.pixels[0] == 0 && a.pixels[59] == 0));
  const Image16 none;
  CHECK((none.empty() && none.pixels.empty()));
  const Image16 neg(-3, 4); // a bogus size is an empty picture, not a huge allocation
  CHECK((neg.width == 0 && neg.height == 4 && neg.pixels.empty() && neg.empty()));
  const Image16 neg2(4, -1);
  CHECK((neg2.empty() && neg2.pixels.empty()));
}
} // namespace image16_cases
