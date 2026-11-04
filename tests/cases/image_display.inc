// Tests for liteui_image::makeDisplayImage / DisplaySettings.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <limits>

using liteui_image::makeDisplayImage;

namespace {
// One-pixel linear FP32 image.
ImageBuffer onePx(float r, float g, float b, float a) {
  auto buf = ImageBuffer::create(1, 1, PixelFormat::RGBA32_FLOAT);
  REQUIRE(buf.has_value());
  const float px[4] = {r, g, b, a};
  buf->setPixel(0, 0, px);
  return std::move(*buf);
}
// RGBA8 result of displaying one pixel, as {r,g,b,a} byte values.
std::array<int, 4> shown(const ImageBuffer &src, DisplaySettings s = {}) {
  auto out = makeDisplayImage(src, s);
  REQUIRE(out.has_value());
  REQUIRE(out->format == PixelFormat::RGBA8_UNORM);
  REQUIRE(out->valid());
  const std::byte *p = out->row(0);
  return {static_cast<int>(p[0]), static_cast<int>(p[1]),
          static_cast<int>(p[2]), static_cast<int>(p[3])};
}
} // namespace

TEST_CASE("DisplaySettings: defaults", "[pixel][display]") {
  DisplaySettings d;
  CHECK(d.exposureEv == 0.0f);
  CHECK(d.clamp == ClampMode::PerChannel);
  CHECK(d == DisplaySettings{});
  DisplaySettings e;
  e.exposureEv = 1.0f;
  CHECK(d != e);
}

TEST_CASE("makeDisplayImage: default settings match convertImage", "[pixel][display]") {
  auto src = onePx(0.5f, 0.0f, 1.0f, 0.5f);
  ImageBuffer viaConvert;
  viaConvert.format = PixelFormat::RGBA8_UNORM;
  REQUIRE(liteui_image::convertImage(src, viaConvert));
  auto viaDisplay = makeDisplayImage(src);
  REQUIRE(viaDisplay.has_value());
  CHECK(viaDisplay->pixels == viaConvert.pixels);
  CHECK(shown(src) == std::array<int, 4>{188, 0, 255, 128});
}

TEST_CASE("makeDisplayImage: exposure in stops", "[pixel][display]") {
  DisplaySettings s;
  s.exposureEv = 1.0f; // linear 0.25 -> 0.5
  CHECK(shown(onePx(0.25f, 0.25f, 0.25f, 1), s)[0] == 188);
  s.exposureEv = -1.0f; // linear 1.0 -> 0.5
  CHECK(shown(onePx(1, 1, 1, 1), s)[0] == 188);
  s.exposureEv = 1.0f; // linear 0.5 -> 1.0 -> white
  CHECK(shown(onePx(0.5f, 0.5f, 0.5f, 1), s)[0] == 255);
  // Alpha is never exposed.
  s.exposureEv = 3.0f;
  CHECK(shown(onePx(0, 0, 0, 0.5f), s)[3] == 128);
  // Non-finite exposure is ignored; absurd values are limited, not UB.
  s.exposureEv = std::numeric_limits<float>::quiet_NaN();
  CHECK(shown(onePx(0.5f, 0.5f, 0.5f, 1), s)[0] == 188);
  s.exposureEv = std::numeric_limits<float>::infinity();
  CHECK(shown(onePx(0.5f, 0.5f, 0.5f, 1), s)[0] == 188);
  s.exposureEv = 1000.0f;
  CHECK(shown(onePx(0.5f, 0.0f, 0.5f, 1), s) == std::array<int, 4>{255, 0, 255, 255});
  s.exposureEv = -1000.0f;
  CHECK(shown(onePx(1, 1, 1, 1), s)[0] == 0);
}

TEST_CASE("makeDisplayImage: clamp modes", "[pixel][display]") {
  auto hdr = onePx(4.0f, 2.0f, 1.0f, 1.0f);
  DisplaySettings s;
  s.clamp = ClampMode::PerChannel;
  CHECK(shown(hdr, s) == std::array<int, 4>{255, 255, 255, 255}); // hue lost
  s.clamp = ClampMode::PreserveHue; // scaled by 1/4: 1, 0.5, 0.25
  CHECK(shown(hdr, s) == std::array<int, 4>{255, 188, 137, 255});
  // In range: both modes agree.
  auto ok = onePx(0.5f, 0.25f, 0.1f, 1.0f);
  s.clamp = ClampMode::PerChannel;
  auto a = shown(ok, s);
  s.clamp = ClampMode::PreserveHue;
  CHECK(shown(ok, s) == a);
}

TEST_CASE("makeDisplayImage: negative, NaN and infinite values", "[pixel][display]") {
  constexpr float nan = std::numeric_limits<float>::quiet_NaN();
  constexpr float inf = std::numeric_limits<float>::infinity();
  for (ClampMode m : {ClampMode::PerChannel, ClampMode::PreserveHue}) {
    DisplaySettings s;
    s.clamp = m;
    CHECK(shown(onePx(-1.0f, -inf, 0.5f, -2.0f), s) == std::array<int, 4>{0, 0, 188, 0});
    CHECK(shown(onePx(nan, nan, nan, nan), s) == std::array<int, 4>{0, 0, 0, 0});
    // +inf is "very bright": red saturates; with PreserveHue the finite
    // channels become negligible next to it instead of turning into NaN.
    auto r = shown(onePx(inf, 1.0f, 0.0f, 1.0f), s);
    CHECK(r[0] == 255);
    if (m == ClampMode::PreserveHue) {
      CHECK(r[1] == 0);
    } else {
      CHECK(r[1] == 255);
    }
  }
}

TEST_CASE("makeDisplayImage: RGBA8 passes through unchanged", "[pixel][display]") {
  auto b = ImageBuffer::create(2, 2, PixelFormat::RGBA8_UNORM, 12); // padded
  REQUIRE(b.has_value());
  const float px[4] = {1.0f, 0.5f, 0.25f, 0.75f};
  b->setPixel(1, 1, px);
  DisplaySettings s;
  s.exposureEv = 4.0f;
  s.clamp = ClampMode::PreserveHue;
  auto out = makeDisplayImage(*b, s);
  REQUIRE(out.has_value());
  CHECK(out->rowStride == 8);
  float o[4], i[4];
  out->getPixel(1, 1, o);
  b->getPixel(1, 1, i);
  CHECK(std::memcmp(o, i, sizeof o) == 0); // exposure ignored
}

TEST_CASE("makeDisplayImage: size, empty and invalid", "[pixel][display]") {
  auto big = ImageBuffer::create(5, 3, PixelFormat::RGBA32_FLOAT);
  REQUIRE(big.has_value());
  auto out = makeDisplayImage(*big);
  REQUIRE(out.has_value());
  CHECK(out->width == 5);
  CHECK(out->height == 3);
  CHECK(out->byteSize() == 60);

  auto e = makeDisplayImage(ImageBuffer{});
  REQUIRE(e.has_value());
  CHECK(e->empty());

  big->pixels.pop_back();
  CHECK_FALSE(makeDisplayImage(*big).has_value());
}

TEST_CASE("toCanvasImage: honours display settings", "[pixel][display]") {
  auto src = onePx(0.25f, 0.25f, 0.25f, 1.0f);
  DisplaySettings s;
  s.exposureEv = 1.0f;
  auto img = liteui_image::toCanvasImage(src, s);
  REQUIRE(img.has_value());
  CHECK(img->pixels[0] == 188);
  CHECK(liteui_image::toCanvasImage(src)->pixels[0] == 137); // default: 0.25 -> 137
}
