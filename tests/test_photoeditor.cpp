#ifdef PHOTOEDITOR_TESTS
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
using Catch::Approx;

TEST_CASE("adjustment specs are well-formed") {
  for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i) {
    const auto &s = kAdjustmentSpecs[i];
    REQUIRE(static_cast<size_t>(s.id) == i);
    REQUIRE(s.min < s.max);
    if (s.stage == AdjustmentSpec::Stage::Tone)
      REQUIRE(s.apply);
  }
}

TEST_CASE("tone adjustments are identity at 0") {
  for (const auto &s : kAdjustmentSpecs) {
    if (s.stage != AdjustmentSpec::Stage::Tone) continue;
    for (int x = 0; x < 256; ++x)
      REQUIRE(s.apply(float(x), 0.0f) == Approx(float(x)));
  }
}

TEST_CASE("tone adjustments are monotonic at the extremes") {
  for (const auto &s : kAdjustmentSpecs) {
    if (s.stage != AdjustmentSpec::Stage::Tone) continue;
    for (float v : {s.min, s.max}) {
      float prev = -1e9f;
      for (int x = 0; x < 256; ++x) {
        float y = s.apply(float(x), v);
        INFO(s.label << " v=" << v << " x=" << x);
        REQUIRE(y >= prev - 1e-3f);
        prev = y;
      }
    }
  }
}

TEST_CASE("identity curve gives identity LUT") {
  auto lut = curveToLut(identityCurve());
  for (int i = 0; i < 256; ++i)
    REQUIRE(lut[i] == Approx(float(i)).margin(0.5));
  REQUIRE(curveIsIdentity(lut));
}

TEST_CASE("snapAngle rounds, clamps, never returns -0") {
  REQUIRE(snapAngle(12.34f) == Approx(12.3f));
  REQUIRE(snapAngle(99.0f) == kAngleMax);
  REQUIRE(snapAngle(-99.0f) == -kAngleMax);
  REQUIRE(!std::signbit(snapAngle(-0.01f)));
}

TEST_CASE("rotatedDims") {
  auto d0 = rotatedDims(400, 300, 0.0f, true);
  REQUIRE((d0.w == 400 && d0.h == 300));
  auto d90 = rotatedDims(400, 300, 90.0f, true);
  REQUIRE((d90.w == 300 && d90.h == 400));
}

TEST_CASE("four 90-degree rotations restore the image") {
  CanvasImage img(2, 3);
  for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = uint8_t(i);
  auto r = std::make_shared<CanvasImage>(img);
  for (int i = 0; i < 4; ++i) r = rotate90cw(*r);
  REQUIRE(r->width == 2);
  REQUIRE(r->pixels == img.pixels);
}
#endif // PHOTOEDITOR_TESTS