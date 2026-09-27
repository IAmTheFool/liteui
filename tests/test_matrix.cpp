// tests/test_matrix.cpp
//
// Unit tests for liteui_svg::Mat2x3 and parseTransform() -- the 2D
// affine matrix type and SVG transform="" parser used when rendering
// parsed SVG documents.

#include "liteui.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using liteui_svg::Mat2x3;
using liteui_svg::parseTransform;

namespace {
constexpr float kPi = 3.14159265358979f;

// Applies m to a point the same way the renderer does -- shape.transform
// is fed straight into CanvasContext::transformBy(a,b,c,d,e,f) -- i.e.
// the standard SVG matrix(a,b,c,d,e,f) convention:
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
struct Pt {
  float x, y;
};
Pt apply(const Mat2x3 &m, float x, float y) {
  return {m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
}
} // namespace

TEST_CASE("Mat2x3::identity leaves points unchanged", "[matrix]") {
  auto p = apply(Mat2x3::identity(), 3.0f, -7.0f);
  CHECK(p.x == Approx(3.0f));
  CHECK(p.y == Approx(-7.0f));
}

TEST_CASE("Mat2x3::translate shifts points", "[matrix]") {
  auto p = apply(Mat2x3::translate(10.0f, -5.0f), 1.0f, 1.0f);
  CHECK(p.x == Approx(11.0f));
  CHECK(p.y == Approx(-4.0f));
}

TEST_CASE("Mat2x3::scaleM scales each axis independently", "[matrix]") {
  auto p = apply(Mat2x3::scaleM(2.0f, 3.0f), 4.0f, 5.0f);
  CHECK(p.x == Approx(8.0f));
  CHECK(p.y == Approx(15.0f));
}

TEST_CASE("Mat2x3::rotate by 90 degrees", "[matrix]") {
  auto p = apply(Mat2x3::rotate(kPi / 2.0f), 1.0f, 0.0f);
  CHECK(p.x == Approx(0.0f).margin(1e-4));
  CHECK(p.y == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Mat2x3::rotate by 180 degrees negates both axes", "[matrix]") {
  auto p = apply(Mat2x3::rotate(kPi), 2.0f, 3.0f);
  CHECK(p.x == Approx(-2.0f).margin(1e-4));
  CHECK(p.y == Approx(-3.0f).margin(1e-4));
}

TEST_CASE("Mat2x3::skewX shifts x proportionally to y", "[matrix]") {
  auto p = apply(Mat2x3::skewX(kPi / 4.0f), 0.0f, 2.0f); // 45deg, tan == 1
  CHECK(p.x == Approx(2.0f).margin(1e-4));
  CHECK(p.y == Approx(2.0f));
}

TEST_CASE("Mat2x3::skewY shifts y proportionally to x", "[matrix]") {
  auto p = apply(Mat2x3::skewY(kPi / 4.0f), 2.0f, 0.0f);
  CHECK(p.x == Approx(2.0f));
  CHECK(p.y == Approx(2.0f).margin(1e-4));
}

TEST_CASE("Mat2x3::multiply composes so the right-hand matrix applies first",
          "[matrix]") {
  // translate(10,0).multiply(rotate(90deg)) means "rotate first, then
  // translate", matching transform="translate(10,0) rotate(90)" (the
  // rightmost/innermost transform is closest to the point).
  auto m = Mat2x3::translate(10.0f, 0.0f).multiply(Mat2x3::rotate(kPi / 2.0f));
  auto p = apply(m, 1.0f, 0.0f);
  CHECK(p.x == Approx(10.0f).margin(1e-4));
  CHECK(p.y == Approx(1.0f).margin(1e-4));
}

TEST_CASE("Mat2x3::multiply with identity is a no-op on either side",
          "[matrix]") {
  auto m = Mat2x3::translate(4.0f, 5.0f).multiply(Mat2x3::scaleM(2.0f, 2.0f));
  auto expected = apply(m, 3.0f, 3.0f);
  auto lhs = apply(m.multiply(Mat2x3::identity()), 3.0f, 3.0f);
  auto rhs = apply(Mat2x3::identity().multiply(m), 3.0f, 3.0f);
  CHECK(lhs.x == Approx(expected.x));
  CHECK(lhs.y == Approx(expected.y));
  CHECK(rhs.x == Approx(expected.x));
  CHECK(rhs.y == Approx(expected.y));
}

TEST_CASE("parseTransform: translate() with one argument defaults ty to 0",
          "[matrix][parse]") {
  auto p = apply(parseTransform("translate(5)"), 0.0f, 0.0f);
  CHECK(p.x == Approx(5.0f));
  CHECK(p.y == Approx(0.0f));
}

TEST_CASE("parseTransform: translate() with two arguments", "[matrix][parse]") {
  auto p = apply(parseTransform("translate(5, 7)"), 0.0f, 0.0f);
  CHECK(p.x == Approx(5.0f));
  CHECK(p.y == Approx(7.0f));
}

TEST_CASE("parseTransform: scale() with one argument applies uniformly",
          "[matrix][parse]") {
  auto p = apply(parseTransform("scale(3)"), 2.0f, 4.0f);
  CHECK(p.x == Approx(6.0f));
  CHECK(p.y == Approx(12.0f));
}

TEST_CASE("parseTransform: scale() with two arguments", "[matrix][parse]") {
  auto p = apply(parseTransform("scale(2,3)"), 2.0f, 4.0f);
  CHECK(p.x == Approx(4.0f));
  CHECK(p.y == Approx(12.0f));
}

TEST_CASE("parseTransform: rotate() about the origin", "[matrix][parse]") {
  auto p = apply(parseTransform("rotate(90)"), 1.0f, 0.0f);
  CHECK(p.x == Approx(0.0f).margin(1e-4));
  CHECK(p.y == Approx(1.0f).margin(1e-4));
}

TEST_CASE("parseTransform: rotate() about an explicit pivot leaves it fixed",
          "[matrix][parse]") {
  auto p = apply(parseTransform("rotate(90,10,10)"), 10.0f, 10.0f);
  CHECK(p.x == Approx(10.0f).margin(1e-3));
  CHECK(p.y == Approx(10.0f).margin(1e-3));
}

TEST_CASE("parseTransform: matrix() applies raw coefficients directly",
          "[matrix][parse]") {
  auto p = apply(parseTransform("matrix(1,0,0,1,10,20)"), 1.0f, 1.0f);
  CHECK(p.x == Approx(11.0f));
  CHECK(p.y == Approx(21.0f));
}

TEST_CASE("parseTransform: chained transforms compose right-to-left",
          "[matrix][parse]") {
  auto p = apply(parseTransform("translate(10,0) rotate(90)"), 1.0f, 0.0f);
  CHECK(p.x == Approx(10.0f).margin(1e-4));
  CHECK(p.y == Approx(1.0f).margin(1e-4));
}

TEST_CASE("parseTransform: empty/unrecognized input yields identity",
          "[matrix][parse]") {
  auto p = apply(parseTransform(""), 3.0f, -2.0f);
  CHECK(p.x == Approx(3.0f));
  CHECK(p.y == Approx(-2.0f));
}