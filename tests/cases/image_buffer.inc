// Tests for ImageBuffer: allocation, validation, pixel access, copy/move.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <cstring>
#include <limits>

TEST_CASE("ImageBuffer: default is empty and valid", "[pixel][imagebuffer]") {
  ImageBuffer b;
  CHECK(b.empty());
  CHECK(b.valid());
  CHECK(b.byteSize() == 0);
  CHECK(b.row(0) == nullptr);
}

TEST_CASE("ImageBuffer: create RGBA8 and RGBA32F", "[pixel][imagebuffer]") {
  auto a = ImageBuffer::create(3, 2, PixelFormat::RGBA8_UNORM);
  REQUIRE(a.has_value());
  CHECK(a->valid());
  CHECK(a->rowStride == 12);
  CHECK(a->byteSize() == 24);
  for (std::byte v : a->pixels) CHECK(v == std::byte{0}); // zero-filled

  auto f = ImageBuffer::create(3, 2, PixelFormat::RGBA32_FLOAT);
  REQUIRE(f.has_value());
  CHECK(f->valid());
  CHECK(f->rowStride == 48);
  CHECK(f->byteSize() == 96);
}

TEST_CASE("ImageBuffer: padded stride", "[pixel][imagebuffer]") {
  auto b = ImageBuffer::create(3, 2, PixelFormat::RGBA8_UNORM, 16);
  REQUIRE(b.has_value());
  CHECK(b->valid());
  CHECK(b->rowStride == 16);
  CHECK(b->byteSize() == 32);
  CHECK(b->row(1) == b->pixels.data() + 16);

  const float px[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  CHECK(b->setPixel(2, 1, px));
  float out[4];
  CHECK(b->getPixel(2, 1, out));
  CHECK(out[0] == 1.0f);
  // Padding bytes stay untouched.
  CHECK(b->pixels[12 + 16] == std::byte{0});
}

TEST_CASE("ImageBuffer: invalid creation", "[pixel][imagebuffer]") {
  CHECK_FALSE(ImageBuffer::create(2, 2, static_cast<PixelFormat>(200)).has_value());
  CHECK_FALSE(ImageBuffer::create(3, 2, PixelFormat::RGBA8_UNORM, 11).has_value());
  constexpr auto big = std::numeric_limits<uint32_t>::max();
  CHECK_FALSE(ImageBuffer::create(big, big, PixelFormat::RGBA32_FLOAT).has_value());
}

TEST_CASE("ImageBuffer: empty images", "[pixel][imagebuffer]") {
  auto a = ImageBuffer::create(0, 0, PixelFormat::RGBA32_FLOAT);
  REQUIRE(a.has_value());
  CHECK(a->empty());
  CHECK(a->valid());
  auto b = ImageBuffer::create(0, 7, PixelFormat::RGBA8_UNORM);
  REQUIRE(b.has_value());
  CHECK(b->empty());
  CHECK(b->valid());
  float out[4];
  CHECK_FALSE(b->getPixel(0, 0, out));
}

TEST_CASE("ImageBuffer: valid() detects inconsistency", "[pixel][imagebuffer]") {
  auto b = ImageBuffer::create(4, 4, PixelFormat::RGBA8_UNORM);
  REQUIRE(b.has_value());
  ImageBuffer shortBuf = *b;
  shortBuf.pixels.pop_back();
  CHECK_FALSE(shortBuf.valid());
  ImageBuffer badStride = *b;
  badStride.rowStride = 8; // less than one row
  CHECK_FALSE(badStride.valid());
  ImageBuffer unsupported = *b;
  unsupported.format = static_cast<PixelFormat>(200);
  CHECK_FALSE(unsupported.valid());
  // Accessors refuse an invalid buffer instead of reading out of bounds.
  float out[4];
  CHECK_FALSE(shortBuf.getPixel(0, 0, out));
  CHECK(shortBuf.row(0) == nullptr);
}

TEST_CASE("ImageBuffer: RGBA32F pixels are stored untouched", "[pixel][imagebuffer]") {
  auto b = ImageBuffer::create(2, 2, PixelFormat::RGBA32_FLOAT);
  REQUIRE(b.has_value());
  const float in[4] = {4.5f, -0.25f, 0.0f, 0.5f}; // HDR and negative values
  CHECK(b->setPixel(1, 1, in));
  float out[4];
  CHECK(b->getPixel(1, 1, out));
  CHECK(std::memcmp(in, out, sizeof in) == 0);
  float nan[4] = {std::numeric_limits<float>::quiet_NaN(),
                  std::numeric_limits<float>::infinity(), 0, 1};
  CHECK(b->setPixel(0, 0, nan));
  CHECK(b->getPixel(0, 0, out));
  CHECK(std::isnan(out[0]));
  CHECK(std::isinf(out[1]));
}

TEST_CASE("ImageBuffer: RGBA8 pixel access", "[pixel][imagebuffer]") {
  auto b = ImageBuffer::create(2, 1, PixelFormat::RGBA8_UNORM);
  REQUIRE(b.has_value());
  const float in[4] = {1.0f, 0.5f, 2.0f, -1.0f}; // above 1 and below 0 clamp
  CHECK(b->setPixel(0, 0, in));
  CHECK(b->pixels[0] == std::byte{255});
  CHECK(b->pixels[1] == std::byte{128}); // 0.5 * 255 = 127.5, rounds up
  CHECK(b->pixels[2] == std::byte{255});
  CHECK(b->pixels[3] == std::byte{0});
  const float nan[4] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 1};
  CHECK(b->setPixel(1, 0, nan));
  CHECK(b->pixels[4] == std::byte{0}); // NaN becomes 0
  float out[4];
  CHECK(b->getPixel(0, 0, out));
  CHECK(out[0] == 1.0f);
  CHECK(out[1] == Catch::Approx(128.0 / 255.0)); // plain /255, no sRGB decode
}

TEST_CASE("ImageBuffer: out-of-range access", "[pixel][imagebuffer]") {
  auto b = ImageBuffer::create(2, 2, PixelFormat::RGBA8_UNORM);
  REQUIRE(b.has_value());
  float v[4] = {0, 0, 0, 0};
  CHECK_FALSE(b->getPixel(2, 0, v));
  CHECK_FALSE(b->getPixel(0, 2, v));
  CHECK_FALSE(b->setPixel(2, 0, v));
  CHECK_FALSE(b->setPixel(0, 2, v));
  CHECK(b->row(2) == nullptr);
}

TEST_CASE("ImageBuffer: copy and move", "[pixel][imagebuffer]") {
  auto a = ImageBuffer::create(2, 2, PixelFormat::RGBA32_FLOAT);
  REQUIRE(a.has_value());
  const float in[4] = {1, 2, 3, 4};
  a->setPixel(1, 1, in);

  ImageBuffer copy = *a; // deep copy
  copy.setPixel(1, 1, in);
  const float other[4] = {9, 9, 9, 9};
  copy.setPixel(0, 0, other);
  float out[4];
  a->getPixel(0, 0, out);
  CHECK(out[0] == 0.0f); // original unaffected

  ImageBuffer moved = std::move(*a);
  CHECK(moved.valid());
  CHECK(moved.byteSize() == 64);
  CHECK(moved.getPixel(1, 1, out));
  CHECK(out[2] == 3.0f);
  // The source is left empty and consistent, not half-described.
  CHECK(a->empty());
  CHECK(a->valid());
  CHECK(a->byteSize() == 0);

  ImageBuffer assigned;
  assigned = std::move(moved);
  CHECK(assigned.valid());
  CHECK(moved.empty());
  CHECK(moved.valid());
}
