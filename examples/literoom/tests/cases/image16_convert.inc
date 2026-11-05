// cases/image16_convert.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace image16_convert_cases
// so they cannot collide with other cases.

#include "core/image16_convert.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace image16_convert_cases
{
// What "the nearest byte" means for a 16-bit sample v, written without the library's code:
// round(v / 257). 257 is odd, so there is never a tie. Integer form: floor((2v + 257) / 514).
static int nearestByte(int v) { return (2 * v + 257) / 514; }

TEST_CASE("image16_convert: widenValues", "[image16_convert]")
{
  // 16 x 16 pixels x 4 samples = 1024 samples; sample i holds the byte value i % 256, so
  // every byte value 0..255 appears four times, in every channel position.
  CanvasImage src(16, 16);
  REQUIRE(src.pixels.size() == 16u * 16u * 4u);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = static_cast<uint8_t>(i % 256);

  const Image16 wide = widenImage(src);
  REQUIRE(wide.pixels.size() == src.pixels.size());

  bool allExact = true;
  for (size_t i = 0; i < wide.pixels.size(); ++i)
    if (static_cast<int>(wide.pixels[i]) != static_cast<int>(src.pixels[i]) * 257)
      allExact = false;
  CHECK(allExact); // every sample v became exactly v * 257

  // The end points are the whole point of 257: black stays black, white becomes full scale.
  CHECK(static_cast<int>(wide.pixels[0]) == 0);
  CHECK(static_cast<int>(wide.pixels[255]) == 65535);
}

TEST_CASE("image16_convert: sizeAndLayout", "[image16_convert]")
{
  // An odd, non-square size catches a swapped width/height or a wrong row stride.
  CanvasImage src(7, 3);
  REQUIRE(src.pixels.size() == 7u * 3u * 4u);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = static_cast<uint8_t>(i * 5 + 1); // distinct neighbours, wraps past 255

  const Image16 wide = widenImage(src);
  CHECK((wide.width == 7 && wide.height == 3));
  CHECK(wide.pixels.size() == 7u * 3u * 4u);

  // Every sample stays where it was: channel order and pixel order are not touched.
  bool sameSpot = true;
  for (size_t i = 0; i < src.pixels.size(); ++i)
    if (static_cast<int>(wide.pixels[i]) != static_cast<int>(src.pixels[i]) * 257)
      sameSpot = false;
  CHECK(sameSpot);

  const CanvasImage back = narrowImage(wide);
  CHECK((back.width == 7 && back.height == 3));
  CHECK(back.pixels.size() == src.pixels.size());

  // A one-pixel-wide and a one-pixel-high picture (degenerate strides).
  const Image16 col = widenImage(CanvasImage(1, 5));
  CHECK((col.width == 1 && col.height == 5 && col.pixels.size() == 20u));
  const Image16 row = widenImage(CanvasImage(5, 1));
  CHECK((row.width == 5 && row.height == 1 && row.pixels.size() == 20u));
}

TEST_CASE("image16_convert: narrowRounding", "[image16_convert]")
{
  // 128 x 128 pixels x 4 samples = 65536 samples: sample i holds the 16-bit value i, so one
  // call covers every possible 16-bit input exactly once.
  Image16 src(128, 128);
  REQUIRE(src.pixels.size() == 65536u);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = static_cast<uint16_t>(i);

  const CanvasImage out = narrowImage(src);
  REQUIRE(out.pixels.size() == src.pixels.size());

  int firstBad = -1;
  for (int v = 0; v < 65536 && firstBad < 0; ++v)
    if (static_cast<int>(out.pixels[static_cast<size_t>(v)]) != nearestByte(v))
      firstBad = v;
  CHECK(firstBad == -1); // every one of the 65536 inputs gave the nearest byte

  // The decision points, spelled out: byte b covers 257*b - 128 .. 257*b + 128.
  CHECK(static_cast<int>(out.pixels[0]) == 0);
  CHECK(static_cast<int>(out.pixels[128]) == 0);     // just below half of 257: down
  CHECK(static_cast<int>(out.pixels[129]) == 1);     // just above half: up
  CHECK(static_cast<int>(out.pixels[257]) == 1);     // exactly 1 * 257
  CHECK(static_cast<int>(out.pixels[257 + 128]) == 1);
  CHECK(static_cast<int>(out.pixels[257 + 129]) == 2);
  CHECK(static_cast<int>(out.pixels[65535]) == 255); // full scale does not overflow the byte
  CHECK(static_cast<int>(out.pixels[65535 - 128]) == 255);
  CHECK(static_cast<int>(out.pixels[65535 - 129]) == 254);
}

TEST_CASE("image16_convert: roundTrip", "[image16_convert]")
{
  // The promise in the header: narrowImage(widenImage(x)) == x exactly, for every byte value
  // in every channel position (alpha included - it is converted like the colours).
  CanvasImage src(16, 16);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = static_cast<uint8_t>(i % 256);

  const CanvasImage back = narrowImage(widenImage(src));
  REQUIRE(back.pixels.size() == src.pixels.size());
  CHECK((back.width == src.width && back.height == src.height));

  size_t mismatches = 0;
  for (size_t i = 0; i < src.pixels.size(); ++i)
    if (back.pixels[i] != src.pixels[i])
      ++mismatches;
  CHECK(mismatches == 0u);

  // Going the other way is allowed to lose information: 16 -> 8 -> 16 snaps to a multiple of 257.
  Image16 odd(1, 1);
  odd.pixels[0] = 1000; // not a multiple of 257 (257 * 4 = 1028 is the nearest)
  odd.pixels[1] = 0;
  odd.pixels[2] = 65535;
  odd.pixels[3] = 32896; // 128 * 257 exactly: survives
  const Image16 snapped = widenImage(narrowImage(odd));
  CHECK(static_cast<int>(snapped.pixels[0]) == 4 * 257);
  CHECK(static_cast<int>(snapped.pixels[1]) == 0);
  CHECK(static_cast<int>(snapped.pixels[2]) == 65535);
  CHECK(static_cast<int>(snapped.pixels[3]) == 32896);
}

TEST_CASE("image16_convert: emptyAndIndependent", "[image16_convert]")
{
  // A picture with no pixels converts to a picture with no pixels, without touching memory.
  const Image16 none = widenImage(CanvasImage(0, 0));
  CHECK((none.width == 0 && none.height == 0 && none.pixels.empty()));
  const CanvasImage none8 = narrowImage(Image16(0, 0));
  CHECK((none8.width == 0 && none8.height == 0 && none8.pixels.empty()));

  // The result is its own copy: changing the source afterwards does not change it.
  CanvasImage src(2, 2);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = 10;
  const Image16 wide = widenImage(src);
  for (size_t i = 0; i < src.pixels.size(); ++i)
    src.pixels[i] = 200;
  CHECK(static_cast<int>(wide.pixels[0]) == 10 * 257);
  CHECK(static_cast<int>(wide.pixels[15]) == 10 * 257);
}
} // namespace image16_convert_cases
