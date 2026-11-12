// Tests for the GL canvas context (Linux, OpenGL ES 3.0): paths, fill rules,
// strokes, clips, transforms, gradients, images, ImageData, text, shadows and
// compositing — the logic that decides WHAT gets drawn, checked against the
// software renderer in canvas_gl_mock.hpp (no GPU, window or fonts needed).
// What the real GPU does with those draw calls is covered by the hardware
// cases in canvas_hardware.inc.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#if defined(LITEUI_CANVAS_GL) && !defined(_WIN32)

#include <array>

#include "canvas_gl_mock.hpp"

namespace canvas_gl_context_test {

using Mock = canvas_gl_test::MockGl;
using Ctx = GlCanvasContext<Mock>;
constexpr float kPi = 3.14159265358979f;

// A canvas of w x h pixels: the mock renderer plus a context drawing into it.
struct Fx {
  Mock gl;
  View::Computed owner; // stands in for the canvas node (readback + caches)
  Ctx c;
  explicit Fx(int w = 64, int h = 64)
      : gl(w, h), c(gl, static_cast<float>(w), static_cast<float>(h), &owner) {
    owner.canvasFbo = 1; // "has a GPU target": enables getImageData
    Mock::resetCounters();
  }
  // Pixels with alpha above `minAlpha`.
  int count(float minAlpha = 0.01f) const {
    int n = 0;
    for (float a : gl.alpha)
      n += a > minAlpha;
    return n;
  }
  float a(int x, int y) const { return gl.alpha[static_cast<size_t>(y) * gl.W + x]; }
  // One premultiplied channel (0 r, 1 g, 2 b, 3 a) scaled to 0..255.
  float ch(int x, int y, int k) const {
    return gl.col[(static_cast<size_t>(y) * gl.W + x) * 4 + k] * 255.0f;
  }
  int strayScratchBits() const {
    int n = 0;
    for (uint8_t s : gl.stencil)
      n += (s & 0x0F) != 0;
    return n;
  }
};

inline Catch::Approx within(double v, double tol) {
  return Catch::Approx(v).margin(tol);
}

const Color kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255}, kBlue{0, 0, 255, 255},
    kBlack{0, 0, 0, 255}, kWhite{255, 255, 255, 255};

inline CanvasGradient redToBlue(float x0, float y0, float x1, float y1) {
  CanvasGradient g = CanvasGradient::linear(x0, y0, x1, y1);
  g.addColorStop(0, kRed);
  g.addColorStop(1, kBlue);
  return g;
}

// A 2x2 test image: red, green / blue, white.
inline CanvasImage quadImage() {
  CanvasImage img(2, 2);
  const uint8_t px[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                          0,   0, 255, 255, 255, 255, 255, 255};
  std::copy(px, px + 16, img.pixels.begin());
  return img;
}

} // namespace canvas_gl_context_test

TEST_CASE("GL canvas: fill rules", "[canvas][gl][fill]") {
  using namespace canvas_gl_context_test;

  SECTION("a rectangle path covers exactly its area") {
    Fx f;
    f.c.setFillColor(kRed);
    f.c.beginPath();
    f.c.rect(10, 10, 20, 20);
    f.c.fill();
    CHECK(f.count() == 400);
    CHECK(f.a(10, 10) == 1.0f);
    CHECK(f.a(29, 29) == 1.0f);
    CHECK(f.a(30, 30) == 0.0f);
    CHECK(f.strayScratchBits() == 0); // the stencil is left clean
  }

  SECTION("EvenOdd leaves a hole in a nested shape") {
    Fx f;
    f.c.beginPath();
    f.c.rect(4, 4, 40, 40);
    f.c.rect(14, 14, 20, 20);
    f.c.fill(FillRule::EvenOdd);
    CHECK(f.count() == 1600 - 400);
    CHECK(f.a(24, 24) == 0.0f);
    CHECK(f.a(6, 6) == 1.0f);
  }

  SECTION("NonZero fills a nested shape wound the same way") {
    Fx f;
    f.c.beginPath();
    f.c.rect(4, 4, 40, 40);
    f.c.rect(14, 14, 20, 20);
    f.c.fill(FillRule::NonZero);
    CHECK(f.count() == 1600);
  }

  SECTION("NonZero leaves a hole when the inner shape winds the other way") {
    Fx f;
    f.c.beginPath();
    f.c.rect(4, 4, 40, 40);
    f.c.moveTo(14, 14);
    f.c.lineTo(14, 34);
    f.c.lineTo(34, 34);
    f.c.lineTo(34, 14);
    f.c.closePath();
    f.c.fill();
    CHECK(f.count() == 1200);
    CHECK(f.a(24, 24) == 0.0f);
  }

  SECTION("an empty or one-point path draws nothing") {
    Fx f;
    f.c.beginPath();
    f.c.fill();
    f.c.moveTo(5, 5);
    f.c.fill();
    CHECK(f.count() == 0);
  }
}

TEST_CASE("GL canvas: arcs, ellipses and rounded rectangles have the right area",
          "[canvas][gl][fill][arc]") {
  // Regression guard for the arc control-point formula (tan(sweep/2), not
  // tan(sweep/4)): with the wrong formula a circle came out ~10% too small.
  using namespace canvas_gl_context_test;

  SECTION("circle") {
    Fx f;
    f.c.beginPath();
    f.c.arc(32, 32, 16, 0, 2 * kPi);
    f.c.fill();
    const double ideal = kPi * 16 * 16; // 804
    CHECK(f.count() > ideal * 0.97);
    CHECK(f.count() < ideal * 1.01);
  }

  SECTION("half circle, counter-clockwise") {
    Fx f;
    f.c.beginPath();
    f.c.arc(32, 32, 20, 0, kPi, true); // upper half (y is down)
    f.c.fill();
    const double ideal = kPi * 20 * 20 / 2;
    CHECK(f.count() > ideal * 0.96);
    CHECK(f.count() < ideal * 1.02);
    CHECK(f.a(32, 22) == 1.0f);
    CHECK(f.a(32, 42) == 0.0f);
  }

  SECTION("ellipse") {
    Fx f;
    f.c.beginPath();
    f.c.ellipse(32, 32, 24, 12, 0, 0, 2 * kPi);
    f.c.fill();
    const double ideal = kPi * 24 * 12;
    CHECK(f.count() > ideal * 0.97);
    CHECK(f.count() < ideal * 1.01);
  }

  SECTION("rounded rectangle") {
    Fx f;
    f.c.beginPath();
    f.c.roundRect(10, 10, 40, 30, 10);
    f.c.fill();
    const double ideal = 40.0 * 30 - (4 - kPi) * 10 * 10; // corners cut
    CHECK(f.count() > ideal * 0.985);
    CHECK(f.count() < ideal * 1.01);
    CHECK(f.a(10, 10) == 0.0f); // the corner pixel itself is cut away
    CHECK(f.a(30, 25) == 1.0f);
  }

  SECTION("a rounded rectangle's radius is limited to half its shorter side") {
    Fx f;
    f.c.beginPath();
    f.c.roundRect(10, 10, 40, 10, 50); // radius 50 -> 5: a stadium shape
    f.c.fill();
    const double ideal = 40.0 * 10 - (4 - kPi) * 5 * 5;
    CHECK(f.count() > ideal * 0.97);
    CHECK(f.count() < ideal * 1.02);
    CHECK(f.a(10, 10) == 0.0f);  // rounded corner
    CHECK(f.a(30, 15) == 1.0f);
    CHECK(f.a(9, 15) == 0.0f);   // nothing bulges out of the rectangle
    CHECK(f.a(50, 15) == 0.0f);
  }

  SECTION("arcTo rounds a corner") {
    Fx f;
    f.c.beginPath();
    f.c.moveTo(10, 10);
    f.c.lineTo(50, 10);
    f.c.arcTo(60, 10, 60, 20, 10); // quarter circle into the right edge
    f.c.lineTo(60, 50);
    f.c.lineTo(10, 50);
    f.c.closePath();
    f.c.fill();
    CHECK(f.a(40, 30) == 1.0f);
    CHECK(f.a(59, 11) == 0.0f); // the rounded-off corner
    CHECK(f.a(50, 12) == 1.0f);
  }
}

TEST_CASE("GL canvas: clipping", "[canvas][gl][clip]") {
  using namespace canvas_gl_context_test;

  SECTION("a circular clip confines drawing, and restore() lifts it") {
    Fx f;
    f.c.save();
    f.c.beginPath();
    f.c.arc(32, 32, 16, 0, 2 * kPi);
    f.c.clip();
    f.c.fillRect(0, 0, 64, 64);
    const int inside = f.count();
    CHECK(inside > 780);
    CHECK(inside < 830); // about pi * 16^2
    CHECK(f.a(32, 32) == 1.0f);
    CHECK(f.a(2, 2) == 0.0f);
    f.c.restore();
    f.c.setFillColor(kBlue);
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.a(1, 1) == 1.0f); // no clip any more
  }

  SECTION("nested clips intersect") {
    Fx f;
    f.c.beginPath();
    f.c.rect(0, 0, 40, 40);
    f.c.clip();
    f.c.beginPath();
    f.c.rect(20, 20, 40, 40);
    f.c.clip();
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.count() == 400); // the 20x20 overlap
    CHECK(f.a(25, 25) == 1.0f);
    CHECK(f.a(10, 10) == 0.0f);
    CHECK(f.a(50, 50) == 0.0f);
  }

  SECTION("restore() pops only the clips made since save()") {
    Fx f;
    f.c.beginPath();
    f.c.rect(0, 0, 40, 64);
    f.c.clip(); // stays for the whole test
    f.c.save();
    f.c.beginPath();
    f.c.rect(0, 0, 20, 64);
    f.c.clip();
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.count() == 20 * 64);
    f.c.restore();
    f.c.setFillColor(kBlue);
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.a(30, 10) == 1.0f); // inside the outer clip again
    CHECK(f.a(45, 10) == 0.0f); // still outside it
    // The part that was inside the inner clip must be drawable again too (the
    // clip level of those pixels drops back to the outer clip's level).
    CHECK(f.ch(10, 10, 2) == 255.0f); // repainted blue over the earlier red
    CHECK(f.ch(10, 10, 0) == 0.0f);
  }

  SECTION("an empty path clips everything away") {
    Fx f;
    f.c.beginPath();
    f.c.clip();
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.count() == 0);
  }

  SECTION("clearRect honours the clip") {
    Fx f;
    f.c.fillRect(0, 0, 64, 64);
    f.c.save();
    f.c.beginPath();
    f.c.rect(0, 0, 25, 25);
    f.c.clip();
    f.c.clearRect(0, 0, 64, 64);
    f.c.restore();
    CHECK(f.a(10, 10) == 0.0f);  // cleared inside the clip
    CHECK(f.a(40, 40) == 1.0f);  // untouched outside it
  }

  SECTION("at most 15 clips nest; a 16th is ignored") {
    Fx f;
    for (int i = 0; i < 15; ++i) {
      f.c.beginPath();
      f.c.rect(0, 0, 64, 64);
      f.c.clip();
    }
    f.c.beginPath();
    f.c.rect(0, 0, 8, 8); // would shrink the area to 64 pixels if applied
    f.c.clip();
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.count() == 64 * 64);
  }
}

TEST_CASE("GL canvas: strokes", "[canvas][gl][stroke]") {
  using namespace canvas_gl_context_test;

  SECTION("a translucent closed stroke does not darken at its joins") {
    Fx f;
    f.c.setStrokeColor(kBlack);
    f.c.setGlobalAlpha(0.5f);
    f.c.setLineWidth(6);
    f.c.setLineJoin(LineJoin::Miter);
    f.c.beginPath();
    f.c.rect(16, 16, 32, 32);
    f.c.stroke();
    float most = 0;
    for (float a : f.gl.alpha)
      most = std::max(most, a);
    CHECK(most == within(0.5, 1e-5)); // never more than the single-layer alpha
    CHECK(f.a(16, 16) > 0.49f);       // the mitred corner is filled
    CHECK(f.a(32, 32) == 0.0f);       // interior untouched
    CHECK(f.count() == 768);          // (32+6)^2 - (32-6)^2
  }

  SECTION("bevel joins cut the outer corner") {
    Fx f;
    f.c.setLineWidth(8);
    f.c.setLineJoin(LineJoin::Bevel);
    f.c.beginPath();
    f.c.moveTo(10, 10);
    f.c.lineTo(40, 10);
    f.c.lineTo(40, 40);
    f.c.stroke();
    CHECK(f.a(43, 6) == 0.0f);   // where a miter would reach
    CHECK(f.a(38, 8) == 1.0f);   // inside the bevel
  }

  SECTION("miter joins fall back to a bevel past the miter limit") {
    Fx f;
    f.c.setLineWidth(8);
    f.c.setLineJoin(LineJoin::Miter);
    f.c.setMiterLimit(1.0f); // every corner exceeds it
    f.c.beginPath();
    f.c.moveTo(10, 10);
    f.c.lineTo(40, 10);
    f.c.lineTo(40, 40);
    f.c.stroke();
    CHECK(f.a(43, 6) == 0.0f);
  }

  SECTION("round joins fill the outer corner with an arc") {
    Fx f;
    f.c.setLineWidth(10);
    f.c.setLineJoin(LineJoin::Round);
    f.c.beginPath();
    f.c.moveTo(10, 30);
    f.c.lineTo(40, 30);
    f.c.lineTo(40, 55);
    f.c.stroke();
    CHECK(f.a(44, 26) == 0.0f); // the square miter corner is not drawn
    CHECK(f.a(43, 29) == 1.0f); // but the rounded part is
  }

  SECTION("line caps") {
    {
      Fx f;
      f.c.setLineWidth(8);
      f.c.beginPath();
      f.c.moveTo(20, 32);
      f.c.lineTo(40, 32);
      f.c.stroke(); // butt (default)
      CHECK(f.a(19, 32) == 0.0f);
      CHECK(f.a(20, 32) == 1.0f);
    }
    {
      Fx f;
      f.c.setLineWidth(8);
      f.c.setLineCap(LineCap::Square);
      f.c.beginPath();
      f.c.moveTo(20, 32);
      f.c.lineTo(40, 32);
      f.c.stroke();
      CHECK(f.a(16, 32) == 1.0f);
      CHECK(f.a(15, 32) == 0.0f);
      CHECK(f.a(43, 32) == 1.0f);
    }
    {
      Fx f;
      f.c.setLineWidth(10);
      f.c.setLineCap(LineCap::Round);
      f.c.beginPath();
      f.c.moveTo(20, 32);
      f.c.lineTo(40, 32);
      f.c.stroke();
      CHECK(f.a(16, 32) == 1.0f);  // the cap extends by the radius...
      CHECK(f.a(15, 28) == 0.0f);  // ...but is round, not square
    }
  }

  SECTION("round caps and joins are solid whichever way the path runs") {
    // The pieces of a stroke (segment quads, cap discs, join wedges) wind in
    // different directions depending on the path direction; they must be
    // counted as a union, not cancel each other into holes.
    for (int dir = 0; dir < 2; ++dir) {
      Fx f;
      f.c.setLineWidth(12);
      f.c.setLineCap(LineCap::Round);
      f.c.setLineJoin(LineJoin::Round);
      f.c.beginPath();
      if (dir == 0) {
        f.c.moveTo(12, 20);
        f.c.lineTo(50, 20);
        f.c.lineTo(50, 50);
      } else { // the same shape drawn from the other end
        f.c.moveTo(50, 50);
        f.c.lineTo(50, 20);
        f.c.lineTo(12, 20);
      }
      f.c.stroke();
      CAPTURE(dir);
      int holes = 0;
      for (int y = 15; y <= 25; ++y)         // the horizontal run, away from edges
        for (int x = 14; x <= 48; ++x)
          holes += f.a(x, y) != 1.0f;
      for (int y = 22; y <= 52; ++y)         // the vertical run
        for (int x = 46; x <= 53; ++x)
          holes += f.a(x, y) != 1.0f;
      CHECK(holes == 0);
      CHECK(f.a(50, 20) == 1.0f);            // the join itself
      CHECK(f.a(8, 20) == 1.0f);             // start cap
      CHECK(f.a(50, 54) == 1.0f);            // end cap
    }
  }

  SECTION("any number of overlapping pieces still draws (coverage never wraps)") {
    // Exactly 16 identical pieces would wrap a 4-bit counter back to zero and
    // punch a hole; the stroke keeps a set/unset flag instead of a count.
    for (int pieces : {2, 15, 16, 17, 32, 48}) {
      Fx f(32, 16);
      f.c.setLineWidth(6);
      f.c.beginPath();
      for (int i = 0; i < pieces; ++i) {
        f.c.moveTo(4, 8);
        f.c.lineTo(28, 8);
      }
      f.c.stroke();
      CAPTURE(pieces);
      CHECK(f.a(16, 8) == 1.0f);
      CHECK(f.count() == 24 * 6);
    }
  }

  SECTION("a wide stroke around a curve is solid in both directions") {
    // Flattening a circle gives short segments; with a wide pen the join
    // wedges overlap their neighbours' quads. A wedge's winding depends on the
    // turn direction, so only a union (not a winding count) keeps the ring
    // from being eaten away by cancellation.
    for (int ccw = 0; ccw < 2; ++ccw)
      for (LineJoin join : {LineJoin::Miter, LineJoin::Bevel}) {
        Fx f(72, 72);
        f.c.setLineWidth(16);
        f.c.setLineJoin(join);
        f.c.beginPath();
        // A full circle in either direction (anticlockwise needs end - start
        // <= -2*pi); closed so the start/end seam is joined, not capped.
        if (ccw)
          f.c.arc(36, 36, 22, 2 * kPi, 0, true);
        else
          f.c.arc(36, 36, 22, 0, 2 * kPi, false);
        f.c.closePath(); // ring from radius 14 to 30
        f.c.stroke();
        CAPTURE(ccw);
        int holes = 0;
        for (int y = 0; y < 72; ++y)
          for (int x = 0; x < 72; ++x) {
            const float r = std::hypot(x + 0.5f - 36, y + 0.5f - 36);
            if (r > 15.5f && r < 28.5f && f.a(x, y) != 1.0f)
              ++holes;
          }
        CHECK(holes == 0);
      }
  }

  SECTION("a point a hair away from the start does not break a closed contour") {
    Fx f(40, 40);
    f.c.setLineWidth(8);
    f.c.beginPath();
    f.c.moveTo(10, 10);
    f.c.lineTo(30, 10);
    f.c.lineTo(30, 30);
    f.c.lineTo(10, 30);
    f.c.lineTo(10, 10.000004f); // ~ the start again, but not exactly
    f.c.closePath();
    f.c.stroke();
    CHECK(f.a(7, 7) == 1.0f);   // the corner at the seam is joined
    CHECK(f.a(10, 14) == 1.0f); // and the left edge has no gap
    CHECK(f.a(20, 20) == 0.0f);
  }

  SECTION("a zero-length subpath is a dot with round or square caps only") {
    {
      Fx f;
      f.c.setLineWidth(10);
      f.c.setLineCap(LineCap::Round);
      f.c.beginPath();
      f.c.moveTo(32, 32);
      f.c.lineTo(32, 32);
      f.c.stroke();
      CHECK(f.a(32, 32) == 1.0f);
      CHECK(f.count() > 60);
    }
    {
      Fx f;
      f.c.setLineWidth(10); // butt caps: nothing to draw
      f.c.beginPath();
      f.c.moveTo(32, 32);
      f.c.lineTo(32, 32);
      f.c.stroke();
      CHECK(f.count() == 0);
    }
  }

  SECTION("a line width of zero draws nothing") {
    Fx f;
    f.c.setLineWidth(0);
    f.c.beginPath();
    f.c.moveTo(0, 0);
    f.c.lineTo(63, 63);
    f.c.stroke();
    CHECK(f.count() == 0);
  }

  SECTION("dashes: on/off pattern and offset") {
    Fx f(64, 8);
    f.c.setLineWidth(2);
    f.c.setLineDash({8, 8});
    f.c.beginPath();
    f.c.moveTo(0, 4);
    f.c.lineTo(64, 4);
    f.c.stroke();
    CHECK(f.a(4, 3) == 1.0f);
    CHECK(f.a(12, 3) == 0.0f);
    CHECK(f.a(20, 3) == 1.0f);
    CHECK(f.a(28, 3) == 0.0f);
    int on = 0;
    for (int x = 0; x < 64; ++x)
      on += f.a(x, 3) > 0;
    CHECK(on == 32);

    Fx g(64, 8);
    g.c.setLineWidth(2);
    g.c.setLineDash({8, 8});
    g.c.setLineDashOffset(8); // shifts the pattern by one dash
    g.c.beginPath();
    g.c.moveTo(0, 4);
    g.c.lineTo(64, 4);
    g.c.stroke();
    CHECK(g.a(4, 3) == 0.0f);
    CHECK(g.a(12, 3) == 1.0f);
  }

  SECTION("an odd-length dash list repeats itself") {
    Fx f(60, 8);
    f.c.setLineWidth(2);
    f.c.setLineDash({10}); // means {10, 10}
    f.c.beginPath();
    f.c.moveTo(0, 4);
    f.c.lineTo(60, 4);
    f.c.stroke();
    CHECK(f.a(5, 3) == 1.0f);
    CHECK(f.a(15, 3) == 0.0f);
    CHECK(f.a(25, 3) == 1.0f);

    Fx g(70, 8);
    g.c.setLineWidth(2);
    g.c.setLineDash({5, 10, 15}); // becomes {5,10,15, 5,10,15}
    g.c.beginPath();
    g.c.moveTo(0, 4);
    g.c.lineTo(70, 4);
    g.c.stroke();
    CHECK(g.a(2, 3) == 1.0f);   //   0..5   on
    CHECK(g.a(12, 3) == 0.0f);  //   5..15  off
    CHECK(g.a(20, 3) == 1.0f);  //  15..30  on
    CHECK(g.a(32, 3) == 0.0f);  //  30..35  off (second round starts)
    CHECK(g.a(37, 3) == 1.0f);  //  35..45  on
  }

  SECTION("invalid dash lists are ignored (the line stays solid)") {
    Fx f(40, 8);
    f.c.setLineWidth(2);
    f.c.setLineDash({-5, 5});
    f.c.beginPath();
    f.c.moveTo(0, 4);
    f.c.lineTo(40, 4);
    f.c.stroke();
    CHECK(f.count() == 80);
  }

  SECTION("strokeRect draws an outline and leaves the current path alone") {
    Fx f;
    f.c.setLineWidth(2);
    f.c.setStrokeColor(kRed);
    f.c.setFillColor(kBlue);
    f.c.beginPath();
    f.c.rect(40, 40, 10, 10); // the path to be filled afterwards
    f.c.strokeRect(5, 5, 20, 20);
    CHECK(f.a(5, 15) == 1.0f);   // on the outline's left edge
    CHECK(f.a(15, 15) == 0.0f);  // hollow
    CHECK(f.a(45, 45) == 0.0f);  // the path was not drawn by strokeRect
    f.c.fill();
    CHECK(f.a(45, 45) == 1.0f);  // ...and is still intact for fill()
  }
}

TEST_CASE("GL canvas: transforms and state", "[canvas][gl][transform]") {
  using namespace canvas_gl_context_test;

  SECTION("translate and scale compose in canvas order") {
    Fx f;
    f.c.translate(10, 10);
    f.c.scale(2, 2);
    f.c.beginPath();
    f.c.rect(0, 0, 10, 10);
    f.c.fill();
    CHECK(f.count() == 400);
    CHECK(f.a(10, 10) == 1.0f);
    CHECK(f.a(29, 29) == 1.0f);
    CHECK(f.a(30, 30) == 0.0f);
  }

  SECTION("rotate turns the drawing about the origin") {
    Fx f;
    f.c.translate(40, 10);
    f.c.rotate(kPi / 2);
    f.c.fillRect(0, 0, 20, 10); // x' = 40 - y, y' = 10 + x
    CHECK(f.a(35, 20) == 1.0f);
    CHECK(f.a(45, 20) == 0.0f);
    CHECK(f.count() == 200);
  }

  SECTION("setTransform replaces and resetTransform clears it") {
    Fx f;
    f.c.translate(30, 30);
    f.c.setTransform(1, 0, 0, 1, 5, 5); // not 35, 35
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.a(5, 5) == 1.0f);
    CHECK(f.a(35, 35) == 0.0f);
    f.c.resetTransform();
    f.c.fillRect(20, 20, 4, 4);
    CHECK(f.a(20, 20) == 1.0f);
  }

  SECTION("transformBy multiplies onto the current matrix") {
    Fx f;
    f.c.translate(10, 0);
    f.c.transformBy(1, 0, 0, 1, 0, 20); // a further translate by (0, 20)
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.a(10, 20) == 1.0f);
  }

  SECTION("save/restore bring back transform, colours, alpha and dashes") {
    Fx f;
    f.c.setFillColor(kRed);
    f.c.setGlobalAlpha(1.0f);
    f.c.save();
    f.c.translate(20, 20);
    f.c.setFillColor(kBlue);
    f.c.setGlobalAlpha(0.5f);
    f.c.setLineDash({2, 2});
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.ch(20, 20, 2) == within(128, 1));
    CHECK(f.ch(20, 20, 3) == within(128, 1));
    f.c.restore();
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.ch(1, 1, 0) == within(255, 0.5)); // red, opaque, at the origin
    CHECK(f.ch(1, 1, 3) == within(255, 0.5));
    // The dash list is also restored: this stroke is solid.
    f.c.setLineWidth(2);
    f.c.beginPath();
    f.c.moveTo(0, 50);
    f.c.lineTo(40, 50);
    f.c.stroke();
    CHECK(f.a(3, 50) == 1.0f);
    CHECK(f.a(7, 50) == 1.0f);
  }

  SECTION("restore() with nothing saved is harmless") {
    Fx f;
    f.c.restore();
    f.c.fillRect(0, 0, 4, 4);
    CHECK(f.count() == 16);
  }

  SECTION("clearRect goes through the transform") {
    Fx f;
    f.c.fillRect(0, 0, 64, 64);
    f.c.translate(10, 10);
    f.c.clearRect(0, 0, 10, 10);
    CHECK(f.a(15, 15) == 0.0f);
    CHECK(f.a(5, 5) == 1.0f);
    CHECK(f.a(25, 25) == 1.0f);
    CHECK(f.count() == 64 * 64 - 100);
  }
}

TEST_CASE("GL canvas: gradients", "[canvas][gl][gradient]") {
  using namespace canvas_gl_context_test;

  SECTION("linear: red to blue across the canvas") {
    Fx f(64, 8);
    f.c.setFillGradient(redToBlue(0, 0, 64, 0));
    f.c.fillRect(0, 0, 64, 8);
    CHECK(f.ch(0, 3, 0) == within(252, 6));
    CHECK(f.ch(0, 3, 2) == within(3, 6));
    CHECK(f.ch(63, 3, 0) == within(3, 6));
    CHECK(f.ch(63, 3, 2) == within(252, 6));
    CHECK(f.ch(32, 3, 0) == within(127, 6));
    CHECK(f.ch(32, 3, 2) == within(127, 6));
    CHECK(f.ch(32, 3, 3) == within(255, 1));
  }

  SECTION("translucent stops are premultiplied and scale with globalAlpha") {
    Fx f(64, 8);
    CanvasGradient g = CanvasGradient::linear(0, 0, 64, 0);
    g.addColorStop(0, Color{255, 255, 255, 0});
    g.addColorStop(1, kWhite);
    f.c.setFillGradient(g);
    f.c.setGlobalAlpha(0.5f);
    f.c.fillRect(0, 0, 64, 8);
    CHECK(f.ch(32, 3, 3) == within(64, 5)); // half of a half
    CHECK(f.ch(32, 3, 0) == within(f.ch(32, 3, 3), 1)); // white: colour == alpha
    CHECK(f.ch(63, 3, 3) == within(128, 3));
  }

  SECTION("radial: inner colour, middle, padded outside") {
    Fx f;
    CanvasGradient g = CanvasGradient::radial(32, 32, 4, 32, 32, 20);
    g.addColorStop(0, kRed);
    g.addColorStop(1, kBlue);
    f.c.setFillGradient(g);
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.ch(32, 32, 0) == within(255, 4));          // inside r0: first colour
    CHECK(f.ch(44, 32, 0) == within(119, 4));          // t = 0.53 at the pixel centre
    CHECK(f.ch(44, 32, 2) == within(136, 4));
    CHECK(f.ch(60, 32, 2) == within(255, 4));          // beyond r1: last colour
    CHECK(f.ch(0, 0, 2) == within(255, 4));
  }

  SECTION("two-circle cone (circles of different centre and radius)") {
    Fx f;
    CanvasGradient g = CanvasGradient::radial(20, 32, 0, 44, 32, 12);
    g.addColorStop(0, kRed);
    g.addColorStop(1, kBlue);
    f.c.setFillGradient(g);
    f.c.fillRect(0, 0, 64, 64);
    CHECK(f.ch(24, 32, 0) == within(160, 4)); // s = 0.373 at the pixel centre
    CHECK(f.ch(24, 32, 2) == within(95, 4));
    CHECK(f.ch(32, 32, 2) == within(255, 6)); // on the end circle: s = 1
    CHECK(f.ch(5, 5, 3) == 0.0f);             // outside the cone: nothing
  }

  SECTION("gradient coordinates follow the transform in force when drawing") {
    Fx f(80, 4);
    f.c.translate(10, 0);
    f.c.scale(2, 1);
    f.c.setFillGradient(redToBlue(0, 0, 32, 0)); // device x 10..74
    f.c.fillRect(0, 0, 32, 4);
    CHECK(f.ch(10, 1, 0) == within(250, 8));
    CHECK(f.ch(42, 1, 0) == within(127, 8));
    CHECK(f.ch(73, 1, 2) == within(250, 8));
    CHECK(f.ch(5, 1, 3) == 0.0f);
  }

  SECTION("a translate in y moves the gradient with the shape") {
    Fx f(8, 40);
    f.c.translate(0, 20);
    f.c.setFillGradient(redToBlue(0, 0, 0, 16)); // vertical, device rows 20..36
    f.c.fillRect(0, 0, 8, 16);
    CHECK(f.ch(4, 20, 0) == within(250, 8)); // top: red
    CHECK(f.ch(4, 28, 0) == within(127, 8)); // middle
    CHECK(f.ch(4, 35, 2) == within(250, 8)); // bottom: blue
    CHECK(f.a(4, 10) == 0.0f);
  }

  SECTION("strokes and fills accept gradients") {
    Fx f(64, 16);
    f.c.setStrokeGradient(redToBlue(0, 0, 64, 0));
    f.c.setLineWidth(4);
    f.c.beginPath();
    f.c.moveTo(0, 8);
    f.c.lineTo(64, 8);
    f.c.stroke();
    CHECK(f.ch(2, 8, 0) == within(246, 10));
    CHECK(f.ch(61, 8, 2) == within(246, 10));
  }

  SECTION("no stops, or a singular transform, paint nothing") {
    Fx f(16, 16);
    f.c.setFillGradient(CanvasGradient::linear(0, 0, 16, 0)); // no stops
    f.c.fillRect(0, 0, 16, 16);
    CHECK(f.count() == 0);
    f.c.setFillGradient(redToBlue(0, 0, 16, 0));
    f.c.scale(0, 1); // collapses the plane
    f.c.fillRect(0, 0, 16, 16);
    CHECK(f.count() == 0);
  }

  SECTION("a zero-length linear gradient paints nothing") {
    Fx f(16, 16);
    f.c.setFillGradient(redToBlue(5, 5, 5, 5));
    f.c.fillRect(0, 0, 16, 16);
    CHECK(f.count() == 0);
  }
}

TEST_CASE("GL canvas: drawImage", "[canvas][gl][image]") {
  using namespace canvas_gl_context_test;

  SECTION("scaling picks the right texel for each destination pixel") {
    Fx f;
    f.c.drawImage(quadImage(), 10, 10, 4, 4);
    CHECK(f.ch(10, 10, 0) == within(255, 1)); // red
    CHECK(f.ch(12, 10, 1) == within(255, 1)); // green
    CHECK(f.ch(10, 12, 2) == within(255, 1)); // blue
    CHECK(f.ch(13, 13, 0) == within(255, 1)); // white
    CHECK(f.count() == 16);
  }

  SECTION("the source rectangle crops the image") {
    Fx f;
    f.c.drawImage(quadImage(), 1, 0, 1, 2, 0, 0, 8, 16); // right column only
    CHECK(f.count() == 128);
    CHECK(f.ch(3, 3, 1) == within(255, 1));   // green (top right texel)
    CHECK(f.ch(3, 12, 0) == within(255, 1));  // white (bottom right texel)
    CHECK(f.ch(3, 12, 2) == within(255, 1));
  }

  SECTION("a source rectangle past the image edge is clipped, shrinking the destination") {
    Fx f;
    f.c.drawImage(quadImage(), -1, -1, 2, 2, 0, 0, 20, 20);
    CHECK(f.count() == 100); // only the 1x1 corner texel, at 10x10 pixels
    CHECK(f.ch(15, 15, 0) == within(255, 1));
    CHECK(f.a(5, 5) == 0.0f);
  }

  SECTION("degenerate rectangles draw nothing") {
    Fx f;
    f.c.drawImage(quadImage(), 0, 0, 0, 2, 0, 0, 8, 8);  // empty source
    f.c.drawImage(quadImage(), 0, 0, 2, 2, 0, 0, 0, 8);  // empty destination
    f.c.drawImage(quadImage(), 5, 5, 2, 2, 0, 0, 8, 8);  // source outside the image
    CHECK(f.count() == 0);
    CanvasImage empty(0, 0);
    f.c.drawImage(empty, 0, 0);
    CHECK(f.count() == 0);
  }

  SECTION("the texture is cached per canvas, keyed on the pixel buffer") {
    Fx f;
    const CanvasImage img = quadImage();
    f.c.drawImage(img, 0, 0);
    f.c.drawImage(img, 8, 8, 4, 4);
    CHECK(f.gl.uploads == 1); // same buffer: uploaded once

    const CanvasImage other = img; // identical pixels, different buffer
    f.c.drawImage(other, 20, 20);
    CHECK(f.gl.uploads == 2);
    CHECK(f.gl.deleted.size() == 1); // the old texture is retired, not leaked
  }

  SECTION("without a canvas node every draw uploads, and retires its texture") {
    Mock gl(32, 32);
    Ctx c(gl, 32, 32); // no node to cache on
    const CanvasImage img = quadImage();
    c.drawImage(img, 0, 0);
    c.drawImage(img, 8, 8);
    CHECK(gl.uploads == 2);
    CHECK(gl.deleted.size() == 2);
  }

  SECTION("straight alpha is premultiplied; globalAlpha and clip apply") {
    Fx f(32, 32);
    CanvasImage half(1, 1);
    half.pixels = {255, 0, 0, 128};
    f.c.drawImage(half, 0, 0);
    CHECK(f.ch(0, 0, 3) == within(128, 1));
    CHECK(f.ch(0, 0, 0) == within(128, 1)); // premultiplied red
    f.c.setGlobalAlpha(0.5f);
    f.c.drawImage(half, 4, 0);
    CHECK(f.ch(4, 0, 3) == within(64, 1));
    f.c.setGlobalAlpha(1);
    f.c.save();
    f.c.beginPath();
    f.c.rect(0, 8, 4, 4);
    f.c.clip();
    f.c.drawImage(quadImage(), 0, 8, 8, 8);
    f.c.restore();
    CHECK(f.ch(2, 10, 3) == 255.0f);
    CHECK(f.ch(6, 10, 3) == 0.0f); // clipped away at x >= 4
  }

  SECTION("the transform applies to the destination rectangle") {
    Fx f;
    f.c.translate(20, 20);
    f.c.scale(2, 2);
    f.c.drawImage(quadImage(), 0, 0, 4, 4); // 8x8 on screen at (20,20)
    CHECK(f.count() == 64);
    CHECK(f.ch(21, 21, 0) == within(255, 1));
    CHECK(f.ch(26, 26, 0) == within(255, 1)); // white texel
  }
}

TEST_CASE("GL canvas: ImageData", "[canvas][gl][imagedata]") {
  using namespace canvas_gl_context_test;

  SECTION("putImageData replaces pixels, alpha included, ignoring clip/alpha/transform") {
    Fx f(16, 16);
    f.c.fillRect(0, 0, 16, 16); // opaque black everywhere
    CanvasImage p(2, 1);
    p.pixels = {255, 0, 0, 255, 9, 9, 9, 0};
    f.c.save();
    f.c.beginPath();
    f.c.rect(0, 0, 1, 1);
    f.c.clip(); // would hide everything if it applied
    f.c.setGlobalAlpha(0.1f);
    f.c.scale(3, 3);
    f.c.putImageData(p, 5.4f, 5.6f); // rounds to (5, 6)
    f.c.restore();
    CHECK(f.ch(5, 6, 0) == within(255, 1));
    CHECK(f.ch(5, 6, 3) == 255.0f);
    CHECK(f.ch(6, 6, 3) == 0.0f);  // the transparent pixel punched through
    CHECK(f.ch(4, 6, 3) == 255.0f); // neighbours untouched
    CHECK(f.ch(7, 6, 3) == 255.0f);
  }

  SECTION("getImageData returns straight (un-premultiplied) RGBA") {
    Fx f(8, 8);
    f.c.setFillColor(Color{255, 0, 0, 128});
    f.c.fillRect(0, 0, 8, 8);
    auto im = f.c.getImageData(0, 0, 2, 2);
    REQUIRE(im.has_value());
    CHECK(im->width == 2);
    CHECK(im->height == 2);
    CHECK(im->pixels[0] == within(255, 1));
    CHECK(im->pixels[1] == 0);
    CHECK(im->pixels[3] == within(128, 1));
  }

  SECTION("regions off the canvas read as transparent black") {
    Fx f(8, 8);
    f.c.setFillColor(Color{255, 0, 0, 128});
    f.c.fillRect(0, 0, 8, 8);
    auto im = f.c.getImageData(-2, -2, 4, 4);
    REQUIRE(im.has_value());
    CHECK(im->pixels[0] == 0); // (-2,-2)
    CHECK(im->pixels[3] == 0);
    const size_t canvasOrigin = (2 * 4 + 2) * 4; // (0,0) inside the region
    CHECK(im->pixels[canvasOrigin] == within(255, 1));
    CHECK(im->pixels[canvasOrigin + 3] == within(128, 1));

    auto far = f.c.getImageData(100, 100, 3, 3);
    REQUIRE(far.has_value());
    CHECK(far->pixels[3] == 0);
  }

  SECTION("a context with no canvas node cannot read back") {
    Mock gl(8, 8);
    Ctx c(gl, 8, 8);
    CHECK_FALSE(c.getImageData(0, 0, 1, 1).has_value());
  }

  SECTION("put then get round-trips, including partial alpha") {
    Fx f(16, 16);
    CanvasImage img(4, 4);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        uint8_t *p = &img.pixels[(y * 4 + x) * 4];
        p[0] = static_cast<uint8_t>(x * 60);
        p[1] = static_cast<uint8_t>(y * 60);
        p[2] = 90;
        p[3] = (x + y) % 2 ? 255 : 128;
      }
    f.c.putImageData(img, 6, 6);
    auto back = f.c.getImageData(6, 6, 4, 4);
    REQUIRE(back.has_value());
    int worst = 0;
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        const uint8_t *p = &back->pixels[(y * 4 + x) * 4];
        worst = std::max({worst, std::abs(p[0] - x * 60), std::abs(p[1] - y * 60),
                          std::abs(p[2] - 90),
                          std::abs(p[3] - ((x + y) % 2 ? 255 : 128))});
      }
    CHECK(worst <= 3); // premultiply / un-premultiply rounding
  }
}

TEST_CASE("GL canvas: text placement", "[canvas][gl][text]") {
  // The mock lays out text as 6 px per character on a 12 px line with the
  // baseline at 10 and ink rows 2..9 (see canvas_gl_mock.hpp), so positions
  // can be checked exactly without installed fonts.
  using namespace canvas_gl_context_test;

  SECTION("alphabetic baseline: layout top = y - 10") {
    Fx f(64, 32);
    f.c.setFillColor(kRed);
    f.c.fillText("abc", 10, 20);
    CHECK(f.count() == 16 * 8);
    CHECK(f.a(10, 12) == 1.0f);
    CHECK(f.a(25, 19) == 1.0f);
    CHECK(f.a(9, 12) == 0.0f);
    CHECK(f.a(10, 11) == 0.0f);
    CHECK(f.a(26, 12) == 0.0f);
    CHECK(f.a(10, 20) == 0.0f);
  }

  SECTION("textAlign") {
    {
      Fx f(64, 32);
      f.c.setTextAlign(TextAlign::Center);
      f.c.fillText("abc", 30, 20); // advance 18: starts at x = 21
      CHECK(f.a(21, 15) == 1.0f);
      CHECK(f.a(20, 15) == 0.0f);
      CHECK(f.a(36, 15) == 1.0f);
      CHECK(f.a(37, 15) == 0.0f);
    }
    {
      Fx f(64, 32);
      f.c.setTextAlign(TextAlign::End);
      f.c.fillText("abc", 40, 20); // ends at x = 40
      CHECK(f.a(22, 15) == 1.0f);
      CHECK(f.a(21, 15) == 0.0f);
    }
  }

  SECTION("textBaseline") {
    {
      Fx f(64, 32);
      f.c.setTextBaseline(TextBaseline::Top);
      f.c.fillText("abc", 0, 4);
      CHECK(f.a(2, 6) == 1.0f);
      CHECK(f.a(2, 5) == 0.0f);
    }
    {
      Fx f(64, 32);
      f.c.setTextBaseline(TextBaseline::Middle);
      f.c.fillText("abc", 0, 20); // top = 20 - 6
      CHECK(f.a(2, 16) == 1.0f);
      CHECK(f.a(2, 15) == 0.0f);
    }
    {
      Fx f(64, 32);
      f.c.setTextBaseline(TextBaseline::Bottom);
      f.c.fillText("abc", 0, 20); // top = 20 - 12
      CHECK(f.a(2, 10) == 1.0f);
      CHECK(f.a(2, 9) == 0.0f);
    }
  }

  SECTION("bitmaps are rasterized at the transform's scale and land 1:1 on pixels") {
    Fx f(64, 40);
    f.c.scale(2, 2);
    f.c.fillText("abc", 5, 10);
    CHECK(Mock::lastScale == 2.0f);
    CHECK(f.count() == 32 * 16);
    CHECK(f.a(10, 4) == 1.0f);
    CHECK(f.a(9, 4) == 0.0f);
    CHECK(f.a(41, 19) == 1.0f);
    CHECK(f.a(42, 19) == 0.0f);
  }

  SECTION("fractional positions snap to the pixel grid (axis-aligned transforms)") {
    Fx f(64, 32);
    f.c.fillText("abc", 10.4f, 20.4f);
    CHECK(f.a(10, 12) == 1.0f);
    CHECK(f.a(9, 12) == 0.0f);
    CHECK(f.count() == 128);
  }

  SECTION("the glyph quad's corners land on whole device pixels after snapping") {
    for (float off : {0.0f, 0.25f, 0.5f, 0.75f}) {
      Fx f(64, 32);
      f.c.fillText("abc", 10 + off, 20 + off);
      REQUIRE(f.gl.lastDraw.size() == 6);
      for (const auto &v : f.gl.lastDraw) {
        CHECK(v.x == within(std::round(v.x), 1e-4));
        CHECK(v.y == within(std::round(v.y), 1e-4));
      }
    }
    // A rotated transform cannot be snapped: the quad keeps its fractional corners.
    Fx g(64, 32);
    g.c.rotate(0.3f);
    g.c.fillText("abc", 10.3f, 20.3f);
    REQUIRE(g.gl.lastDraw.size() == 6);
    bool fractional = false;
    for (const auto &v : g.gl.lastDraw)
      fractional = fractional || std::fabs(v.x - std::round(v.x)) > 1e-3;
    CHECK(fractional);
  }

  SECTION("glyph bitmaps are cached per string and style") {
    Fx f(64, 32);
    f.c.fillText("abc", 10, 20);
    f.c.fillText("abc", 10, 20);
    CHECK(Mock::rasterCalls == 1);
    f.c.fillText("abd", 10, 20);
    CHECK(Mock::rasterCalls == 2);
    // A mask is shared by every fill colour of the same text.
    f.c.setFillColor(kRed);
    f.c.fillText("xyz", 10, 20);
    f.c.setFillColor(kBlue);
    f.c.fillText("xyz", 10, 20);
    CHECK(Mock::rasterCalls == 3);
  }

  SECTION("spaces draw nothing; an empty string is not even shaped") {
    Fx f(64, 32);
    f.c.fillText("   ", 10, 20);
    CHECK(f.count() == 0);
    CHECK(Mock::rasterCalls == 1); // shaped once (to learn it has no ink)
    f.c.fillText("", 10, 20);
    CHECK(Mock::rasterCalls == 1);
  }

  SECTION("gradient fills tint the glyph mask") {
    Fx f(64, 32);
    f.c.setFillGradient(redToBlue(0, 0, 64, 0));
    f.c.fillText("abc", 10, 20);
    CHECK(f.ch(10, 15, 0) == within(214, 6));
    CHECK(f.ch(25, 15, 0) == within(153, 6));
    CHECK(f.ch(25, 15, 2) == within(101, 6));
    CHECK(f.count() == 128);
  }

  SECTION("shadow, globalAlpha and clip apply to text") {
    {
      Fx f(64, 32);
      f.c.setShadow(kBlue, 0, 4, 4);
      f.c.setFillColor(kRed);
      f.c.fillText("abc", 10, 20);
      CHECK(f.ch(28, 23, 2) == within(255, 1)); // shadow only
      CHECK(f.ch(28, 23, 0) == 0.0f);
      CHECK(f.ch(12, 15, 0) == within(255, 1)); // text over its own shadow
      CHECK(f.ch(12, 15, 2) == 0.0f);
    }
    {
      Fx f(64, 32);
      f.c.setGlobalAlpha(0.5f);
      f.c.fillText("abc", 10, 20);
      CHECK(f.ch(12, 15, 3) == within(128, 1));
    }
    {
      Fx f(64, 32);
      f.c.save();
      f.c.beginPath();
      f.c.rect(0, 0, 18, 32);
      f.c.clip();
      f.c.fillText("abc", 10, 20);
      f.c.restore();
      CHECK(f.a(17, 15) == 1.0f);
      CHECK(f.a(18, 15) == 0.0f);
    }
  }

  SECTION("strokeText strokes the glyph outlines, not filled glyphs") {
    Fx f(64, 32);
    f.c.setStrokeColor(kRed);
    f.c.setLineWidth(2);
    f.c.strokeText("a", 10, 20); // outline box x 10..14, y 12..20
    CHECK(f.a(9, 15) == 1.0f);
    CHECK(f.a(10, 15) == 1.0f);
    CHECK(f.a(12, 15) == 0.0f); // hollow inside
    CHECK(f.a(13, 15) == 1.0f);
    CHECK(f.a(15, 15) == 0.0f);
    CHECK(f.a(12, 11) == 1.0f); // top edge
    CHECK(f.a(12, 13) == 0.0f);
    CHECK(f.a(9, 11) == 1.0f);  // mitred corner
  }
}

TEST_CASE("GL canvas: colour glyphs (emoji)", "[canvas][gl][text][emoji]") {
  using namespace canvas_gl_context_test;
  const char *emoji = "\xF0\x9F\x98\x80"; // U+1F600; the mock lays out 4 "chars"

  SECTION("a solid fill is baked in; the glyph keeps its own colours") {
    Fx f(64, 32);
    f.c.setFillColor(kRed);
    f.c.fillText(emoji, 10, 20);
    CHECK(Mock::lastBake == 1);
    CHECK(f.ch(12, 15, 0) == within(255, 1)); // left half: the fill colour
    CHECK(f.ch(12, 15, 1) == 0.0f);
    CHECK(f.ch(22, 15, 1) == within(255, 1)); // right half: its own green
    CHECK(f.ch(22, 15, 0) == 0.0f);           // not tinted red
  }

  SECTION("each fill colour gets its own cached bitmap") {
    Fx f(64, 32);
    f.c.setFillColor(kRed);
    f.c.fillText(emoji, 10, 20);
    f.c.fillText(emoji, 10, 20);
    CHECK(Mock::rasterCalls == 1);
    f.c.setFillColor(kBlue);
    f.c.fillText(emoji, 10, 20);
    CHECK(Mock::rasterCalls == 2);
  }

  SECTION("globalAlpha scales a colour glyph") {
    Fx f(64, 32);
    f.c.setGlobalAlpha(0.5f);
    f.c.fillText(emoji, 10, 20);
    CHECK(f.ch(12, 15, 3) == within(128, 1));
    CHECK(f.ch(22, 15, 3) == within(128, 1));
  }

  SECTION("the shadow is the glyph's silhouette in the shadow colour") {
    Fx f(64, 32);
    f.c.setShadow(kBlue, 0, 0, 12);
    f.c.setFillColor(kRed);
    f.c.fillText(emoji, 10, 8); // text rows 0..8, shadow rows 12..20
    CHECK(f.ch(12, 16, 2) == within(255, 1));
    CHECK(f.ch(12, 16, 0) == 0.0f);
    CHECK(f.ch(28, 16, 2) == within(255, 1)); // even over the "green" half
    CHECK(f.ch(28, 16, 1) == 0.0f);
  }

  SECTION("a gradient fill cannot be baked: the glyph draws as a tinted silhouette") {
    Fx f(64, 32);
    f.c.setFillGradient(redToBlue(0, 0, 64, 0));
    f.c.fillText(emoji, 10, 20);
    CHECK(Mock::lastBake == 0);
    CHECK(f.ch(22, 15, 1) == 0.0f);   // no green: tinted by the gradient only
    CHECK(f.ch(22, 15, 3) > 250.0f);
  }

  SECTION("ordinary text never takes the colour path") {
    Fx f(64, 32);
    f.c.setFillColor(kRed);
    f.c.fillText("abc", 10, 20);
    CHECK(Mock::lastBake == 0);
  }
}

TEST_CASE("GL canvas: globalCompositeOperation", "[canvas][gl][blend]") {
  using namespace canvas_gl_context_test;

  // Fills the canvas with `dst`, switches to `op`, then draws `src` over it;
  // returns the resulting premultiplied pixel at (1, 1) as 0..255 floats.
  auto run = [](CompositeOp op, Color dst, Color src) {
    Fx f(4, 4);
    f.c.setFillColor(dst);
    f.c.fillRect(0, 0, 4, 4);
    f.c.setGlobalCompositeOperation(op);
    f.c.setFillColor(src);
    f.c.fillRect(0, 0, 4, 4);
    return std::array<float, 4>{f.ch(1, 1, 0), f.ch(1, 1, 1), f.ch(1, 1, 2),
                                f.ch(1, 1, 3)};
  };

  SECTION("source-over is the default") {
    const auto r = run(CompositeOp::SourceOver, kBlue, Color{255, 0, 0, 128});
    CHECK(r[0] == within(128, 1));
    CHECK(r[2] == within(127, 1));
    CHECK(r[3] == within(255, 1));
  }

  SECTION("lighter adds and clamps") {
    const auto r = run(CompositeOp::Lighter, Color{100, 0, 0, 255}, Color{100, 50, 0, 255});
    CHECK(r[0] == within(200, 1));
    CHECK(r[1] == within(50, 1));
  }

  SECTION("copy replaces the backdrop, alpha included") {
    const auto r = run(CompositeOp::Copy, kRed, Color{0, 0, 255, 0});
    CHECK(r[3] == 0.0f);
  }

  SECTION("destination-out erases, xor of two opaque shapes is empty") {
    CHECK(run(CompositeOp::DestinationOut, Color{10, 20, 30, 255}, kBlack)[3] == 0.0f);
    CHECK(run(CompositeOp::Xor, kBlack, kWhite)[3] == 0.0f);
  }

  SECTION("source-atop keeps the backdrop's alpha") {
    const auto r = run(CompositeOp::SourceAtop, kBlue, kRed);
    CHECK(r[0] == within(255, 1));
    CHECK(r[2] == within(0, 1));
    CHECK(r[3] == within(255, 1));
  }

  SECTION("screen: Cs + Cb - Cs*Cb") {
    const auto r = run(CompositeOp::Screen, Color{128, 128, 128, 255}, Color{0, 128, 0, 255});
    CHECK(r[0] == within(128, 1));
    CHECK(r[1] == within(192, 1.5));
    CHECK(r[2] == within(128, 1));
  }

  SECTION("multiply: grey x red, over opaque") {
    const auto r = run(CompositeOp::Multiply, Color{128, 128, 128, 255}, kRed);
    CHECK(r[0] == within(128, 1));
    CHECK(r[1] == within(0, 1));
    CHECK(r[2] == within(0, 1));
    CHECK(r[3] == within(255, 1));
  }

  SECTION("multiply onto an empty canvas shows the source; white changes nothing") {
    Fx f(8, 8);
    f.c.setGlobalCompositeOperation(CompositeOp::Multiply);
    f.c.setFillColor(kBlue);
    f.c.fillRect(0, 0, 8, 8);
    CHECK(f.ch(1, 1, 2) == within(255, 1));
    CHECK(f.ch(1, 1, 3) == within(255, 1));
    f.c.setFillColor(kWhite);
    f.c.fillRect(0, 0, 8, 8);
    CHECK(f.ch(1, 1, 2) == within(255, 1));
    CHECK(f.ch(1, 1, 0) == within(0, 1));
  }

  SECTION("multiply: overlapping shapes each see the previous result") {
    Fx f(8, 8);
    f.c.setFillColor(kBlue);
    f.c.fillRect(0, 0, 8, 8);
    f.c.setGlobalCompositeOperation(CompositeOp::Multiply);
    f.c.setFillColor(Color{128, 128, 128, 255});
    f.c.fillRect(0, 0, 8, 8);
    f.c.fillRect(0, 0, 8, 8);
    CHECK(f.ch(1, 1, 2) == within(64, 2)); // 255 * 0.5 * 0.5
  }

  SECTION("the operation is part of the saved state") {
    Fx f(4, 4);
    f.c.setFillColor(kRed);
    f.c.fillRect(0, 0, 4, 4);
    f.c.save();
    f.c.setGlobalCompositeOperation(CompositeOp::DestinationOut);
    f.c.restore();
    f.c.setFillColor(kBlue);
    f.c.fillRect(0, 0, 4, 4); // source-over again: opaque blue on top
    CHECK(f.ch(1, 1, 2) == within(255, 1));
    CHECK(f.ch(1, 1, 3) == within(255, 1));
  }
}

TEST_CASE("GL canvas: shadows", "[canvas][gl][shadow]") {
  using namespace canvas_gl_context_test;

  SECTION("fillRect draws an offset copy underneath") {
    Fx f;
    f.c.setShadow(kBlue, 0, 10, 10);
    f.c.setFillColor(kRed);
    f.c.fillRect(10, 10, 20, 20);
    CHECK(f.ch(15, 15, 0) == within(255, 1));  // the shape itself
    CHECK(f.ch(35, 35, 2) == within(255, 1));  // shadow-only region
    CHECK(f.ch(35, 35, 0) == 0.0f);
  }

  SECTION("path fills and strokes cast shadows too") {
    Fx f;
    f.c.setShadow(kBlue, 0, 12, 0);
    f.c.setFillColor(kRed);
    f.c.beginPath();
    f.c.rect(5, 20, 10, 10);
    f.c.fill();
    CHECK(f.ch(20, 25, 2) == within(255, 1)); // 12 px to the right
    Fx g;
    g.c.setShadow(kBlue, 0, 0, 12);
    g.c.setStrokeColor(kRed);
    g.c.setLineWidth(4);
    g.c.beginPath();
    g.c.moveTo(10, 10);
    g.c.lineTo(50, 10);
    g.c.stroke();
    CHECK(g.ch(30, 22, 2) == within(255, 1)); // 12 px below the line
  }

  SECTION("no offset, or a transparent shadow colour, casts nothing") {
    Fx f;
    f.c.setShadow(kBlue, 0, 0, 0);
    f.c.setFillColor(kRed);
    f.c.fillRect(10, 10, 10, 10);
    CHECK(f.count() == 100);
    Fx g;
    g.c.setShadow(Color{0, 0, 255, 0}, 0, 10, 10);
    g.c.setFillColor(kRed);
    g.c.fillRect(10, 10, 10, 10);
    CHECK(g.count() == 100);
  }
}

TEST_CASE("GL canvas: a path with thousands of segments fills correctly",
          "[canvas][gl][fill]") {
  using namespace canvas_gl_context_test;
  Fx f(32, 32);
  f.c.beginPath();
  f.c.moveTo(16, 2);
  for (int i = 1; i <= 3000; ++i) { // a 3000-gon approximating a circle
    const float a = 2 * kPi * static_cast<float>(i) / 3000.0f;
    f.c.lineTo(16 + 14 * std::sin(a), 16 - 14 * std::cos(a));
  }
  f.c.closePath();
  f.c.fill();
  const double ideal = kPi * 14 * 14;
  CHECK(f.count() > ideal * 0.97);
  CHECK(f.count() < ideal * 1.02);
  CHECK(f.a(16, 16) == 1.0f);
}

#endif // LITEUI_CANVAS_GL && !_WIN32
