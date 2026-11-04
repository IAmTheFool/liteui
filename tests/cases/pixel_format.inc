// Tests for PixelFormat and its size helpers. (Included from
// liteui_tests.cpp — not a standalone .cpp.)

#include <limits>

TEST_CASE("PixelFormat: bytesPerPixel", "[pixel]") {
  CHECK(bytesPerPixel(PixelFormat::RGBA8_UNORM) == 4);
  CHECK(bytesPerPixel(PixelFormat::RGBA16_UNORM) == 8);
  CHECK(bytesPerPixel(PixelFormat::RGBA16_FLOAT) == 8);
  CHECK(bytesPerPixel(PixelFormat::RGBA32_FLOAT) == 16);
  CHECK(bytesPerPixel(static_cast<PixelFormat>(200)) == 0);
}

TEST_CASE("PixelFormat: minimumRowBytes", "[pixel]") {
  CHECK(minimumRowBytes(10, PixelFormat::RGBA8_UNORM) == 40);
  CHECK(minimumRowBytes(10, PixelFormat::RGBA32_FLOAT) == 160);
  CHECK(minimumRowBytes(0, PixelFormat::RGBA32_FLOAT) == 0);
  CHECK_FALSE(minimumRowBytes(10, static_cast<PixelFormat>(200)).has_value());
}

TEST_CASE("PixelFormat: imageByteSize tight and padded", "[pixel]") {
  // Tight (stride 0) and an explicit stride equal to the minimum agree.
  CHECK(imageByteSize(3, 2, PixelFormat::RGBA8_UNORM) == 24);
  CHECK(imageByteSize(3, 2, PixelFormat::RGBA8_UNORM, 12) == 24);
  // Padded rows are allowed and counted.
  CHECK(imageByteSize(3, 2, PixelFormat::RGBA8_UNORM, 16) == 32);
  CHECK(imageByteSize(3, 2, PixelFormat::RGBA32_FLOAT) == 96);
  // A stride shorter than one row is rejected.
  CHECK_FALSE(imageByteSize(3, 2, PixelFormat::RGBA8_UNORM, 11).has_value());
}

TEST_CASE("PixelFormat: imageByteSize empty images", "[pixel]") {
  CHECK(imageByteSize(0, 0, PixelFormat::RGBA8_UNORM) == 0);
  CHECK(imageByteSize(0, 5, PixelFormat::RGBA32_FLOAT) == 0);
  CHECK(imageByteSize(5, 0, PixelFormat::RGBA32_FLOAT) == 0);
  CHECK_FALSE(imageByteSize(1, 1, static_cast<PixelFormat>(200)).has_value());
}

TEST_CASE("PixelFormat: imageByteSize overflow", "[pixel]") {
  constexpr auto big = std::numeric_limits<uint32_t>::max();
  // A 24-megapixel FP32 image is about 384 MB, and must work.
  CHECK(imageByteSize(6000, 4000, PixelFormat::RGBA32_FLOAT) ==
        size_t{6000} * 4000 * 16);
  // 2^32-1 x 2^32-1 x 16 overflows a 64-bit size_t; on a 32-bit size_t
  // even far smaller images overflow. Either way it must not wrap.
  if constexpr (sizeof(size_t) == 8) {
    CHECK_FALSE(imageByteSize(big, big, PixelFormat::RGBA32_FLOAT).has_value());
  } else {
    CHECK_FALSE(imageByteSize(big, 1, PixelFormat::RGBA32_FLOAT).has_value());
  }
  // A huge stride times the height overflows.
  CHECK_FALSE(imageByteSize(1, 4, PixelFormat::RGBA8_UNORM,
                            std::numeric_limits<size_t>::max() / 2)
                  .has_value());
}
