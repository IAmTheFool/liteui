// Tests for liteui_image::convertImage and the sRGB transfer functions.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <limits>

using liteui_image::convertImage;
using liteui_image::linearToSrgb;
using liteui_image::srgbToLinear;

namespace {
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

ImageBuffer makeF(uint32_t w, uint32_t h) {
  auto b = ImageBuffer::create(w, h, PixelFormat::RGBA32_FLOAT);
  REQUIRE(b.has_value());
  return std::move(*b);
}
ImageBuffer makeU8(uint32_t w, uint32_t h, size_t stride = 0) {
  auto b = ImageBuffer::create(w, h, PixelFormat::RGBA8_UNORM, stride);
  REQUIRE(b.has_value());
  return std::move(*b);
}
ImageBuffer target(PixelFormat f) {
  ImageBuffer d;
  d.format = f;
  return d;
}
std::byte u8(const ImageBuffer &b, uint32_t x, int c) {
  return b.row(0)[x * 4 + c];
}
} // namespace

TEST_CASE("sRGB transfer functions", "[pixel][convert]") {
  CHECK(srgbToLinear(0.0f) == 0.0f);
  CHECK(srgbToLinear(1.0f) == approx(1.0));
  CHECK(srgbToLinear(0.5f) == approx(0.214041));   // not 0.5
  CHECK(srgbToLinear(0.04045f) == approx(0.04045 / 12.92));
  CHECK(linearToSrgb(0.5f) == approx(0.735357));
  CHECK(linearToSrgb(0.0031308f) == approx(0.0031308 * 12.92));
  // Round trip across the whole 0..1 range, including the linear toe.
  for (int i = 0; i <= 1000; ++i) {
    float v = static_cast<float>(i) / 1000.0f;
    CHECK(srgbToLinear(linearToSrgb(v)) == approx(v));
  }
  // Not clamped: HDR values pass through, negatives mirror about zero.
  CHECK(srgbToLinear(2.0f) > 1.0f);
  CHECK(linearToSrgb(4.0f) > 1.0f);
  CHECK(srgbToLinear(-0.5f) == approx(-0.214041));
  CHECK(std::isnan(srgbToLinear(kNaN)));
}

TEST_CASE("convertImage: RGBA8 -> RGBA32F decodes sRGB, keeps alpha linear", "[pixel][convert]") {
  ImageBuffer s = makeU8(3, 1);
  const float px[3][4] = {{0, 0, 0, 0}, {1, 1, 1, 1}, {128 / 255.f, 0, 1, 128 / 255.f}};
  for (uint32_t x = 0; x < 3; ++x) s.setPixel(x, 0, px[x]);

  ImageBuffer d = target(PixelFormat::RGBA32_FLOAT);
  REQUIRE(convertImage(s, d));
  CHECK(d.format == PixelFormat::RGBA32_FLOAT);
  CHECK(d.width == 3);
  CHECK(d.height == 1);
  CHECK(d.valid());

  float o[4];
  d.getPixel(0, 0, o);
  CHECK(o[0] == 0.0f);
  CHECK(o[3] == 0.0f);
  d.getPixel(1, 0, o);
  CHECK(o[0] == approx(1.0));
  CHECK(o[3] == approx(1.0));
  d.getPixel(2, 0, o);
  CHECK(o[0] == approx(srgbToLinear(128 / 255.f))); // colour is decoded
  CHECK(o[0] == approx(0.2158605));
  CHECK(o[2] == approx(1.0));
  CHECK(o[3] == approx(128.0 / 255.0)); // alpha is NOT decoded
}

TEST_CASE("convertImage: RGBA8 -> RGBA32F -> RGBA8 round trips every code", "[pixel][convert]") {
  ImageBuffer s = makeU8(256, 1);
  for (uint32_t i = 0; i < 256; ++i) {
    std::byte *p = s.row(0) + i * 4;
    p[0] = static_cast<std::byte>(i);
    p[1] = static_cast<std::byte>(255 - i);
    p[2] = static_cast<std::byte>(i ^ 0x55);
    p[3] = static_cast<std::byte>((i * 7) & 0xFF);
  }
  ImageBuffer f = target(PixelFormat::RGBA32_FLOAT);
  ImageBuffer back = target(PixelFormat::RGBA8_UNORM);
  REQUIRE(convertImage(s, f));
  REQUIRE(convertImage(f, back));
  CHECK(back.width == 256);
  CHECK(back.pixels == s.pixels); // bit-exact: no colour shift, no alpha error
}

TEST_CASE("convertImage: RGBA32F -> RGBA8 clamps and sanitises", "[pixel][convert]") {
  ImageBuffer s = makeF(5, 1);
  const float px[5][4] = {
      {4.0f, 4.0f, 4.0f, 3.0f},     // HDR clamps to white / opaque
      {-1.0f, -0.001f, -kInf, -2.f}, // negatives clamp to 0
      {kNaN, kNaN, kNaN, kNaN},      // NaN becomes 0
      {kInf, 1.0f, 0.5f, kInf},      // +inf clamps to 255
      {0.5f, 0.0f, 1.0f, 0.5f}};
  for (uint32_t x = 0; x < 5; ++x) s.setPixel(x, 0, px[x]);

  ImageBuffer d = target(PixelFormat::RGBA8_UNORM);
  REQUIRE(convertImage(s, d));
  CHECK(u8(d, 0, 0) == std::byte{255});
  CHECK(u8(d, 0, 3) == std::byte{255});
  for (int c = 0; c < 4; ++c) CHECK(u8(d, 1, c) == std::byte{0});
  for (int c = 0; c < 4; ++c) CHECK(u8(d, 2, c) == std::byte{0});
  CHECK(u8(d, 3, 0) == std::byte{255});
  CHECK(u8(d, 3, 3) == std::byte{255});
  // Linear 0.5 encodes to sRGB 0.7354 -> 188; alpha 0.5 is only quantised.
  CHECK(u8(d, 4, 0) == std::byte{188});
  CHECK(u8(d, 4, 3) == std::byte{128});
}

TEST_CASE("convertImage: alpha is separate from colour", "[pixel][convert]") {
  // Straight alpha: a fully transparent pixel keeps its colour.
  ImageBuffer s = makeU8(1, 1);
  const float px[4] = {1.0f, 0.5f, 0.25f, 0.0f};
  s.setPixel(0, 0, px);
  ImageBuffer f = target(PixelFormat::RGBA32_FLOAT);
  REQUIRE(convertImage(s, f));
  float o[4];
  f.getPixel(0, 0, o);
  CHECK(o[0] == approx(1.0));
  CHECK(o[1] > 0.0f);
  CHECK(o[3] == 0.0f);
}

TEST_CASE("convertImage: same format copies and drops padding", "[pixel][convert]") {
  ImageBuffer s = makeU8(3, 2, 16); // padded
  const float px[4] = {1.0f, 0.0f, 1.0f, 1.0f};
  s.setPixel(2, 1, px);
  ImageBuffer d = target(PixelFormat::RGBA8_UNORM);
  REQUIRE(convertImage(s, d));
  CHECK(d.rowStride == 12);
  CHECK(d.byteSize() == 24);
  float o[4];
  d.getPixel(2, 1, o);
  CHECK(o[0] == 1.0f);
  CHECK(o[2] == 1.0f);

  ImageBuffer f = makeF(2, 2);
  const float hdr[4] = {8.0f, -1.0f, kNaN, 2.0f};
  f.setPixel(1, 1, hdr);
  ImageBuffer g = target(PixelFormat::RGBA32_FLOAT);
  REQUIRE(convertImage(f, g));
  g.getPixel(1, 1, o);
  CHECK(o[0] == 8.0f); // FP32 -> FP32 does not clamp
  CHECK(o[1] == -1.0f);
  CHECK(std::isnan(o[2]));
}

TEST_CASE("convertImage: empty images and in-place use", "[pixel][convert]") {
  ImageBuffer e;
  ImageBuffer d = target(PixelFormat::RGBA32_FLOAT);
  REQUIRE(convertImage(e, d));
  CHECK(d.empty());
  CHECK(d.valid());
  CHECK(d.format == PixelFormat::RGBA32_FLOAT);

  ImageBuffer a = makeU8(2, 2);
  CHECK(convertImage(a, a)); // converting into itself is a no-op
  CHECK(a.valid());
  CHECK(a.format == PixelFormat::RGBA8_UNORM);
}

TEST_CASE("convertImage: failures leave the destination untouched", "[pixel][convert]") {
  ImageBuffer good = makeU8(2, 2);
  ImageBuffer dst = makeF(1, 1);
  const auto before = dst.pixels;

  ImageBuffer broken = good;
  broken.pixels.pop_back();
  dst.format = PixelFormat::RGBA32_FLOAT;
  CHECK_FALSE(convertImage(broken, dst));
  CHECK(dst.pixels == before);
  CHECK(dst.width == 1);

  ImageBuffer wantsHalf = dst;
  wantsHalf.format = static_cast<PixelFormat>(200); // not a real format
  CHECK_FALSE(convertImage(good, wantsHalf));
}
