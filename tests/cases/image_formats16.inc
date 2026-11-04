// Tests for the 16-bit formats (RGBA16_FLOAT, RGBA16_UNORM), half-float
// helpers, region (tiled) display, downscaling, and quality checks of the
// reduced-precision paths against the FP32 reference.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <array>
#include <limits>

namespace {
ImageBuffer make16(uint32_t w, uint32_t h, PixelFormat f) {
  auto b = ImageBuffer::create(w, h, f);
  REQUIRE(b.has_value());
  return std::move(*b);
}
ImageBuffer as(const ImageBuffer &src, PixelFormat f) {
  ImageBuffer d;
  d.format = f;
  REQUIRE(liteui_image::convertImage(src, d));
  return d;
}
// Smooth FP32 test image with HDR values, a toe region and varying alpha.
ImageBuffer gradient(uint32_t w, uint32_t h) {
  ImageBuffer b = make16(w, h, PixelFormat::RGBA32_FLOAT);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      const float px[4] = {1.5f * x / (w - 1), static_cast<float>(y) / (h - 1),
                           0.002f * x / (w - 1), 0.25f + 0.75f * x / (w - 1)};
      b.setPixel(x, y, px);
    }
  return b;
}
} // namespace

TEST_CASE("half float: known encodings", "[pixel][half]") {
  CHECK(floatToHalf(0.0f) == 0x0000);
  CHECK(floatToHalf(-0.0f) == 0x8000);
  CHECK(floatToHalf(1.0f) == 0x3C00);
  CHECK(floatToHalf(-2.0f) == 0xC000);
  CHECK(floatToHalf(0.5f) == 0x3800);
  CHECK(floatToHalf(65504.0f) == 0x7BFF);       // largest finite half
  CHECK(floatToHalf(65520.0f) == 0x7C00);       // rounds up to infinity
  CHECK(floatToHalf(1e9f) == 0x7C00);
  CHECK(floatToHalf(-1e9f) == 0xFC00);
  CHECK(floatToHalf(std::ldexp(1.0f, -24)) == 0x0001); // smallest subnormal
  CHECK(floatToHalf(std::ldexp(1.0f, -14)) == 0x0400); // smallest normal
  CHECK(floatToHalf(std::ldexp(1.0f, -26)) == 0x0000); // underflows to zero
  // Round to nearest, ties to even.
  CHECK(floatToHalf(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);        // tie -> even
  CHECK(floatToHalf(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02); // tie -> even
  CHECK(floatToHalf(1.0f + std::ldexp(1.0f, -11) * 1.01f) == 0x3C01);
  CHECK(std::isinf(halfToFloat(0x7C00)));
  CHECK(halfToFloat(0xFC00) < 0);
  CHECK(std::isnan(halfToFloat(0x7E00)));
  CHECK(std::isnan(halfToFloat(floatToHalf(std::numeric_limits<float>::quiet_NaN()))));
  CHECK(halfToFloat(0x3C00) == 1.0f);
  CHECK(halfToFloat(0x0001) == std::ldexp(1.0f, -24));
  CHECK(std::signbit(halfToFloat(0x8000)));
}

TEST_CASE("half float: every finite half round-trips exactly", "[pixel][half]") {
  for (uint32_t h = 0; h < 65536; ++h) {
    const uint16_t half = static_cast<uint16_t>(h);
    const float f = halfToFloat(half);
    if (std::isnan(f)) {
      CHECK(std::isnan(halfToFloat(floatToHalf(f))));
      continue;
    }
    if (floatToHalf(f) != half) {
      FAIL("half 0x" << std::hex << h << " did not round trip");
    }
  }
}

TEST_CASE("half float: conversion error stays within half precision", "[pixel][half]") {
  // Normal range: relative error <= 2^-11.
  for (float v = 0.001f; v < 60000.0f; v *= 1.037f) {
    const float r = halfToFloat(floatToHalf(v));
    CHECK(std::fabs(r - v) <= v * std::ldexp(1.0f, -11) * 1.0001f);
  }
}

TEST_CASE("16-bit formats: storage and pixel access", "[pixel][fmt16]") {
  CHECK(isStorageSupported(PixelFormat::RGBA16_UNORM));
  CHECK(isStorageSupported(PixelFormat::RGBA16_FLOAT));
  CHECK_FALSE(isStorageSupported(static_cast<PixelFormat>(200)));

  ImageBuffer h = make16(3, 2, PixelFormat::RGBA16_FLOAT);
  CHECK(h.valid());
  CHECK(h.rowStride == 24);
  CHECK(h.byteSize() == 48);
  const float hdr[4] = {8.0f, -0.5f, 0.25f, 1.0f}; // all exact in half
  CHECK(h.setPixel(2, 1, hdr));
  float o[4];
  CHECK(h.getPixel(2, 1, o));
  CHECK(std::memcmp(hdr, o, sizeof o) == 0);
  const float big[4] = {1e9f, -1e9f, std::numeric_limits<float>::quiet_NaN(), 0};
  h.setPixel(0, 0, big);
  h.getPixel(0, 0, o);
  CHECK(std::isinf(o[0]));
  CHECK(o[1] < 0);
  CHECK(std::isnan(o[2]));

  ImageBuffer u = make16(2, 1, PixelFormat::RGBA16_UNORM);
  CHECK(u.rowStride == 16);
  const float in[4] = {1.0f, 0.5f, 2.0f, -1.0f}; // clamps like RGBA8
  u.setPixel(0, 0, in);
  u.getPixel(0, 0, o);
  CHECK(o[0] == 1.0f);
  CHECK(o[1] == approx(0.5));
  CHECK(o[2] == 1.0f);
  CHECK(o[3] == 0.0f);
  const float nan[4] = {std::numeric_limits<float>::quiet_NaN(), 0, 0, 1};
  u.setPixel(1, 0, nan);
  u.getPixel(1, 0, o);
  CHECK(o[0] == 0.0f);
}

TEST_CASE("16-bit formats: 24-megapixel memory footprints", "[pixel][fmt16]") {
  CHECK(imageByteSize(6000, 4000, PixelFormat::RGBA8_UNORM) == size_t{96'000'000});
  CHECK(imageByteSize(6000, 4000, PixelFormat::RGBA16_FLOAT) == size_t{192'000'000});
  CHECK(imageByteSize(6000, 4000, PixelFormat::RGBA16_UNORM) == size_t{192'000'000});
  CHECK(imageByteSize(6000, 4000, PixelFormat::RGBA32_FLOAT) == size_t{384'000'000});
}

TEST_CASE("convertImage: UNORM 8 <-> 16 is exact rescaling", "[pixel][fmt16][convert]") {
  ImageBuffer s = make16(256, 1, PixelFormat::RGBA8_UNORM);
  for (uint32_t i = 0; i < 256; ++i) {
    std::byte *p = s.row(0) + i * 4;
    p[0] = static_cast<std::byte>(i);
    p[1] = static_cast<std::byte>(255 - i);
    p[2] = static_cast<std::byte>(i ^ 0x33);
    p[3] = static_cast<std::byte>((i * 5) & 0xFF);
  }
  ImageBuffer w = as(s, PixelFormat::RGBA16_UNORM);
  uint16_t v;
  std::memcpy(&v, w.row(0) + 255 * 8, 2);
  CHECK(v == 65535);
  std::memcpy(&v, w.row(0) + 128 * 8, 2);
  CHECK(v == 128 * 257);
  ImageBuffer back = as(w, PixelFormat::RGBA8_UNORM);
  CHECK(back.pixels == s.pixels);
}

TEST_CASE("convertImage: every 8-bit code survives FP16 and RGBA16", "[pixel][fmt16][convert]") {
  ImageBuffer s = make16(256, 1, PixelFormat::RGBA8_UNORM);
  for (uint32_t i = 0; i < 256; ++i) {
    std::byte *p = s.row(0) + i * 4;
    p[0] = static_cast<std::byte>(i);
    p[1] = static_cast<std::byte>(255 - i);
    p[2] = static_cast<std::byte>(i ^ 0x55);
    p[3] = static_cast<std::byte>((i * 7) & 0xFF);
  }
  // Half precision (11 bits) is finer than one 8-bit step everywhere.
  ImageBuffer viaHalf = as(as(s, PixelFormat::RGBA16_FLOAT), PixelFormat::RGBA8_UNORM);
  CHECK(viaHalf.pixels == s.pixels);
  ImageBuffer via16 = as(as(s, PixelFormat::RGBA16_UNORM), PixelFormat::RGBA8_UNORM);
  CHECK(via16.pixels == s.pixels);
  // 8 -> FP32 -> 16 -> FP16 -> 8 across all four formats.
  ImageBuffer chain = as(as(as(as(s, PixelFormat::RGBA32_FLOAT), PixelFormat::RGBA16_UNORM),
                            PixelFormat::RGBA16_FLOAT),
                         PixelFormat::RGBA8_UNORM);
  CHECK(chain.pixels == s.pixels);
}

TEST_CASE("convertImage: FP32 <-> RGBA16 UNORM tolerance", "[pixel][fmt16][convert]") {
  ImageBuffer f = gradient(64, 8);
  // Clamp to the displayable range first: UNORM cannot hold HDR values.
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      float px[4];
      f.getPixel(x, y, px);
      px[0] = std::min(px[0], 1.0f);
      f.setPixel(x, y, px);
    }
  ImageBuffer back = as(as(f, PixelFormat::RGBA16_UNORM), PixelFormat::RGBA32_FLOAT);
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 64; ++x) {
      float a[4], b[4];
      f.getPixel(x, y, a);
      back.getPixel(x, y, b);
      for (int c = 0; c < 3; ++c)
        CHECK(std::fabs(a[c] - b[c]) <= 1e-4f + 1e-3f * a[c]); // 16-bit sRGB step
      CHECK(std::fabs(a[3] - b[3]) <= 1.0f / 65535.0f);
    }
}

TEST_CASE("quality: FP16 working image displays like FP32 within 1 LSB", "[pixel][fmt16][quality]") {
  ImageBuffer f32 = gradient(96, 32);
  ImageBuffer f16 = as(f32, PixelFormat::RGBA16_FLOAT);
  for (ClampMode m : {ClampMode::PerChannel, ClampMode::PreserveHue})
    for (float ev : {0.0f, 1.0f, -2.0f}) {
      DisplaySettings s;
      s.clamp = m;
      s.exposureEv = ev;
      auto ref = liteui_image::makeDisplayImage(f32, s);
      auto got = liteui_image::makeDisplayImage(f16, s);
      REQUIRE(ref.has_value());
      REQUIRE(got.has_value());
      REQUIRE(ref->pixels.size() == got->pixels.size());
      int worst = 0;
      for (size_t i = 0; i < ref->pixels.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<int>(ref->pixels[i]) -
                                         static_cast<int>(got->pixels[i])));
      CHECK(worst <= 1);
    }
}

TEST_CASE("quality: RGBA16 UNORM displays as an 8-bit rescale", "[pixel][fmt16][quality]") {
  ImageBuffer s8 = make16(256, 1, PixelFormat::RGBA8_UNORM);
  for (uint32_t i = 0; i < 256; ++i)
    for (int c = 0; c < 4; ++c) s8.row(0)[i * 4 + c] = static_cast<std::byte>(i);
  ImageBuffer s16 = as(s8, PixelFormat::RGBA16_UNORM);
  DisplaySettings ev;
  ev.exposureEv = 3.0f; // display-referred: ignored, like RGBA8
  auto d = liteui_image::makeDisplayImage(s16, ev);
  REQUIRE(d.has_value());
  CHECK(d->pixels == s8.pixels);
}

TEST_CASE("makeDisplayImage: region equals the crop of the full image", "[pixel][region]") {
  for (PixelFormat f : {PixelFormat::RGBA32_FLOAT, PixelFormat::RGBA16_FLOAT,
                        PixelFormat::RGBA16_UNORM, PixelFormat::RGBA8_UNORM}) {
    ImageBuffer src = as(gradient(70, 50), f);
    DisplaySettings s;
    s.exposureEv = 0.5f;
    auto full = liteui_image::makeDisplayImage(src, s);
    REQUIRE(full.has_value());
    // Stitch 16x16 tiles (partial at the right/bottom edge) and compare.
    ImageBuffer stitched = make16(70, 50, PixelFormat::RGBA8_UNORM);
    for (uint32_t ty = 0; ty < 50; ty += 16)
      for (uint32_t tx = 0; tx < 70; tx += 16) {
        liteui_image::ImageRect r{tx, ty, std::min(16u, 70 - tx), std::min(16u, 50 - ty)};
        auto tile = liteui_image::makeDisplayImage(src, s, r);
        REQUIRE(tile.has_value());
        CHECK(tile->width == r.width);
        CHECK(tile->height == r.height);
        for (uint32_t y = 0; y < r.height; ++y)
          std::memcpy(stitched.row(ty + y) + tx * 4, tile->row(y), r.width * 4);
      }
    CHECK(stitched.pixels == full->pixels);
  }
}

TEST_CASE("makeDisplayImage: region bounds", "[pixel][region]") {
  ImageBuffer src = gradient(10, 8);
  DisplaySettings s;
  using R = liteui_image::ImageRect;
  CHECK(liteui_image::makeDisplayImage(src, s, R{0, 0, 10, 8}).has_value());
  CHECK(liteui_image::makeDisplayImage(src, s, R{9, 7, 1, 1}).has_value());
  auto empty = liteui_image::makeDisplayImage(src, s, R{10, 8, 0, 0});
  REQUIRE(empty.has_value());
  CHECK(empty->empty());
  CHECK_FALSE(liteui_image::makeDisplayImage(src, s, R{0, 0, 11, 8}).has_value());
  CHECK_FALSE(liteui_image::makeDisplayImage(src, s, R{5, 0, 6, 1}).has_value());
  CHECK_FALSE(liteui_image::makeDisplayImage(src, s, R{0, 7, 1, 2}).has_value());
  CHECK_FALSE(liteui_image::makeDisplayImage(src, s, R{11, 0, 0, 0}).has_value());
  constexpr auto big = std::numeric_limits<uint32_t>::max();
  CHECK_FALSE(liteui_image::makeDisplayImage(src, s, R{big, 0, big, 1}).has_value()); // no overflow
}

TEST_CASE("downscaleImage: box average in linear light", "[pixel][downscale]") {
  // Black/white checkerboard in linear light averages to 0.5 linear.
  ImageBuffer f = make16(4, 4, PixelFormat::RGBA32_FLOAT);
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const float v = ((x + y) & 1) ? 1.0f : 0.0f;
      const float px[4] = {v, v, v, 1.0f};
      f.setPixel(x, y, px);
    }
  auto d = liteui_image::downscaleImage(f, 2);
  REQUIRE(d.has_value());
  CHECK(d->width == 2);
  CHECK(d->height == 2);
  CHECK(d->format == PixelFormat::RGBA32_FLOAT);
  float o[4];
  d->getPixel(1, 1, o);
  CHECK(o[0] == approx(0.5));
  CHECK(o[3] == approx(1.0));

  // The same checkerboard stored as sRGB 8-bit: averaged in LINEAR light
  // (0.5 linear = sRGB 188), not in the encoded values (which would give 128).
  ImageBuffer u8 = as(f, PixelFormat::RGBA8_UNORM);
  auto du = liteui_image::downscaleImage(u8, 2);
  REQUIRE(du.has_value());
  CHECK(du->format == PixelFormat::RGBA8_UNORM);
  CHECK(du->row(0)[0] == std::byte{188});
}

TEST_CASE("downscaleImage: sizes, partial blocks and alpha weighting", "[pixel][downscale]") {
  ImageBuffer f = gradient(5, 3);
  auto d = liteui_image::downscaleImage(f, 2); // ceil(5/2) x ceil(3/2)
  REQUIRE(d.has_value());
  CHECK(d->width == 3);
  CHECK(d->height == 2);
  CHECK(d->valid());
  auto one = liteui_image::downscaleImage(f, 100);
  REQUIRE(one.has_value());
  CHECK(one->width == 1);
  CHECK(one->height == 1);
  auto same = liteui_image::downscaleImage(f, 1);
  REQUIRE(same.has_value());
  CHECK(same->pixels == f.pixels);
  CHECK_FALSE(liteui_image::downscaleImage(f, 0).has_value());
  auto e = liteui_image::downscaleImage(ImageBuffer{}, 4);
  REQUIRE(e.has_value());
  CHECK(e->empty());
  ImageBuffer broken = f;
  broken.pixels.pop_back();
  CHECK_FALSE(liteui_image::downscaleImage(broken, 2).has_value());

  // One opaque white pixel next to a transparent one: colour is weighted by
  // alpha (stays white), alpha is the plain average.
  ImageBuffer pair = make16(2, 1, PixelFormat::RGBA32_FLOAT);
  const float opaque[4] = {1, 1, 1, 1}, clear[4] = {0, 0, 0, 0};
  pair.setPixel(0, 0, opaque);
  pair.setPixel(1, 0, clear);
  auto m = liteui_image::downscaleImage(pair, 2);
  REQUIRE(m.has_value());
  float o[4];
  m->getPixel(0, 0, o);
  CHECK(o[0] == approx(1.0));
  CHECK(o[3] == approx(0.5));
}

TEST_CASE("downscaleImage: works on every format, keeps the format", "[pixel][downscale]") {
  for (PixelFormat f : {PixelFormat::RGBA32_FLOAT, PixelFormat::RGBA16_FLOAT,
                        PixelFormat::RGBA16_UNORM, PixelFormat::RGBA8_UNORM}) {
    ImageBuffer src = as(gradient(33, 17), f);
    auto d = liteui_image::downscaleImage(src, 4);
    REQUIRE(d.has_value());
    CHECK(d->format == f);
    CHECK(d->width == 9);
    CHECK(d->height == 5);
    CHECK(d->valid());
  }
}

TEST_CASE("Image widget accepts 16-bit sources", "[image][hdr][fmt16]") {
  Image im;
  im.hdrSource = std::make_shared<ImageBuffer>(as(gradient(8, 4), PixelFormat::RGBA16_FLOAT));
  int lw = 0, lh = 0, errors = 0;
  im.onLoad = [&](int w, int h) { lw = w; lh = h; };
  im.onError = [&](const std::string &) { ++errors; };
  View parent;
  parent.addChild(std::move(im));
  CHECK(lw == 8);
  CHECK(lh == 4);
  CHECK(errors == 0);
}
