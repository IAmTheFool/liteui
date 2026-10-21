// Build: g++ -std=c++20 -I.. geometry_test.cpp -o geometry_test && ./geometry_test
#include "core/geometry.hpp"
#include "check.hpp"

#include <random>

static void testSmallTypes()
{
  CHECK(CropRect{}.empty());
  CHECK(CropRect{1, 2, 0, 5}.empty() && CropRect{1, 2, 5, 0}.empty() && CropRect{1, 2, -3, 5}.empty());
  CHECK(!CropRect{0, 0, 1, 1}.empty());
  CHECK(Orient{}.identity() && !Orient{1, false}.identity() && !Orient{0, true}.identity());
  CHECK((Orient{2, true} == Orient{2, true}) && !(Orient{2, true} == Orient{2, false}));
}

static void testRotatedDims()
{
  CHECK((rotatedDims(300, 200, 0.0f, true).w == 300 && rotatedDims(300, 200, 0.0f, false).h == 200));
  // Exact quarter turns: the bounding box ("expand") swaps the sides with no
  // rounding up; "fit" keeps the picture's own aspect inside the turned frame, so
  // a 300 x 200 picture turned 90 degrees fits as 200 x 133.
  for (float q : {90.0f, -90.0f})
  {
    const Dims e = rotatedDims(300, 200, q, true);
    CHECK(e.w == 200 && e.h == 300);
    const Dims f = rotatedDims(300, 200, q, false);
    CHECK(f.w == 200 && f.h == 133);
  }
  for (bool expand : {true, false})
  {
    const Dims d180 = rotatedDims(300, 200, 180.0f, expand);
    CHECK(d180.w == 300 && d180.h == 200);
  }
  // A square at 45 degrees: bounding box is w * sqrt(2); the inscribed copy is w / sqrt(2).
  CHECK(rotatedDims(100, 100, 45.0f, true).w == 142 && rotatedDims(100, 100, 45.0f, true).h == 142);
  CHECK(rotatedDims(100, 100, 45.0f, false).w == 70 && rotatedDims(100, 100, 45.0f, false).h == 70);

  std::mt19937 rng(9);
  for (int i = 0; i < 3000; ++i)
  {
    const int w = 1 + static_cast<int>(rng() % 3000), h = 1 + static_cast<int>(rng() % 3000);
    const float deg = (static_cast<float>(rng() % 9001) - 4500.0f) / 100.0f; // -45..45
    for (bool expand : {true, false})
    {
      const Dims d = rotatedDims(w, h, deg, expand);
      CHECK(d.w >= 1 && d.h >= 1);
      // Symmetric in the sign of the angle and in (w, h) <-> (h, w): this is
      // what lets a crop follow a quarter turn by index math.
      const Dims n = rotatedDims(w, h, -deg, expand);
      CHECK(n.w == d.w && n.h == d.h);
      const Dims t = rotatedDims(h, w, deg, expand);
      CHECK(t.w == d.h && t.h == d.w);
      if (expand)
        CHECK(static_cast<double>(d.w) * d.h >= static_cast<double>(w) * h * 0.999); // the box contains the picture
      else
      {
        CHECK(d.w <= w && d.h <= h); // inscribed: never larger than the picture
        // same aspect ratio, to within the rounding of each side to a whole pixel
        if (d.w >= 20 && d.h >= 20)
          CHECK(std::fabs((static_cast<double>(d.w) / d.h) / (static_cast<double>(w) / h) - 1.0) <=
                1.0 / d.w + 1.0 / d.h + 1e-9);
      }
    }
  }
}

static void expectOrth(const GeoFrame &f)
{
  CHECK_NEAR(f.m00 * f.m00 + f.m10 * f.m10, 1.0, 1e-9);
  CHECK_NEAR(f.m01 * f.m01 + f.m11 * f.m11, 1.0, 1e-9);
  CHECK_NEAR(f.m00 * f.m01 + f.m10 * f.m11, 0.0, 1e-9);
  CHECK_NEAR(std::fabs(f.det()), 1.0, 1e-9);
}

static void testFrameIdentityAndUnknownSize()
{
  const GeoFrame u = makeGeoFrame(0, 0, Orient{1, true}, 20.0f, true, CropRect{1, 1, 5, 5});
  CHECK(u.m00 == 1 && u.m11 == 1 && u.m01 == 0 && u.m10 == 0 && u.tx == 0 && u.ty == 0); // size unknown
  const GeoFrame f = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(f.m00 == 1 && f.m11 == 1 && f.tx == 0 && f.ty == 0);
  CHECK(f.origW == 400 && f.origH == 300 && f.srcW == 400 && f.srcH == 300);
}

static void testFrameOrientations()
{
  const double W = 400, H = 300;
  for (int rot = 0; rot < 4; ++rot)
    for (bool flip : {false, true})
    {
      const GeoFrame f = makeGeoFrame(400, 300, Orient{rot, flip}, 0.0f, false, CropRect{});
      expectOrth(f);
      CHECK((f.det() < 0) == flip); // a mirror reverses orientation
      CHECK(f.srcW == (rot % 2 ? H : W) && f.srcH == (rot % 2 ? W : H));
      // Round trip, and the picture's corners land on the source's corners.
      for (double x : {0.0, 12.5, 399.0})
        for (double y : {0.0, 7.25, 299.0})
        {
          double sx, sy, bx, by;
          f.toSrc(x, y, sx, sy);
          f.toOrig(sx, sy, bx, by);
          CHECK_NEAR(bx, x, 1e-9);
          CHECK_NEAR(by, y, 1e-9);
        }
      double cx, cy;
      f.toSrc(W / 2, H / 2, cx, cy); // the center stays the center
      CHECK_NEAR(cx, f.srcW / 2, 1e-9);
      CHECK_NEAR(cy, f.srcH / 2, 1e-9);
    }
  // One concrete case: a clockwise quarter turn sends the top-left corner to the top-right.
  const GeoFrame r1 = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  double x, y;
  r1.toSrc(0, 0, x, y);
  CHECK_NEAR(x, 300, 1e-9);
  CHECK_NEAR(y, 0, 1e-9);
  r1.toSrc(400, 0, x, y); // top-right -> bottom-right
  CHECK_NEAR(x, 300, 1e-9);
  CHECK_NEAR(y, 400, 1e-9);
  // Mirror first: the left edge goes to the right edge.
  const GeoFrame fl = makeGeoFrame(400, 300, Orient{0, true}, 0.0f, false, CropRect{});
  fl.toSrc(0, 100, x, y);
  CHECK_NEAR(x, 400, 1e-9);
  CHECK_NEAR(y, 100, 1e-9);
}

static void testFrameStraightenAndCrop()
{
  for (float deg : {-33.0f, -7.5f, 12.0f, 45.0f})
    for (bool expand : {false, true})
    {
      const GeoFrame f = makeGeoFrame(400, 300, Orient{}, deg, expand, CropRect{});
      expectOrth(f);
      const Dims d = rotatedDims(400, 300, deg, expand);
      CHECK(f.srcW == d.w && f.srcH == d.h);
      double cx, cy;
      f.toSrc(200, 150, cx, cy); // rotation is about the center
      CHECK_NEAR(cx, d.w / 2.0, 1e-9);
      CHECK_NEAR(cy, d.h / 2.0, 1e-9);
    }
  // A crop is a pure translation applied last, with cropImage's clamping.
  const GeoFrame c = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{50, 20, 100, 80});
  double x, y;
  c.toSrc(60, 30, x, y);
  CHECK_NEAR(x, 10, 1e-9);
  CHECK_NEAR(y, 10, 1e-9);
  CHECK(c.srcW == 100 && c.srcH == 80);
  const GeoFrame over = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{350, 250, 500, 500});
  CHECK(over.srcW == 50 && over.srcH == 50); // clamped to the picture
  const GeoFrame neg = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{-20, -20, 100, 100});
  CHECK(neg.srcW == 80 && neg.srcH == 80);
  // Everything at once still round-trips.
  const GeoFrame all = makeGeoFrame(400, 300, Orient{3, true}, -12.0f, true, CropRect{10, 15, 200, 150});
  double sx, sy, bx, by;
  all.toSrc(123.0, 77.0, sx, sy);
  all.toOrig(sx, sy, bx, by);
  CHECK_NEAR(bx, 123.0, 1e-7);
  CHECK_NEAR(by, 77.0, 1e-7);
}

static void testFrameScalesAndAngles()
{
  // Radii are fractions of the SHORTER side: 300 in the original, 100 after the crop.
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{0, 0, 100, 200});
  CHECK_NEAR(f.radiusScale(), 300.0 / 100.0, 1e-9);
  std::mt19937 rng(21);
  for (int i = 0; i < 400; ++i)
  {
    const Orient o{static_cast<int>(rng() % 4), (rng() & 1) != 0};
    const float ang = (static_cast<float>(rng() % 9001) - 4500.0f) / 100.0f;
    const GeoFrame g = makeGeoFrame(400, 300, o, ang, (rng() & 1) != 0, CropRect{});
    const float a = static_cast<float>(static_cast<int>(rng() % 3600) - 1800) / 10.0f; // -180..180
    // Directions (arrows) round-trip modulo a full turn.
    float back = g.dirToOrig(g.dirToSrc(a));
    float diff = std::fmod(back - a + 540.0f, 360.0f) - 180.0f;
    CHECK(std::fabs(diff) < 1e-3f);
    // Axes (no head or tail) round-trip modulo half a turn and stay in [-90, 90].
    const float ax = static_cast<float>(static_cast<int>(rng() % 1800) - 900) / 10.0f; // -90..90
    const float s = g.axisToSrc(ax);
    CHECK(s >= -90.0f && s <= 90.0f);
    float ba = g.axisToOrig(s);
    float d2 = std::fmod(ba - ax + 270.0f, 180.0f) - 90.0f;
    CHECK(std::fabs(d2) < 1e-3f);
  }
  CHECK(GeoFrame::wrapAxis(100.0f) == -80.0f && GeoFrame::wrapAxis(-100.0f) == 80.0f && GeoFrame::wrapAxis(45.0f) == 45.0f);
}

static void testGeoStep()
{
  // Four clockwise turns, or a turn and its inverse, change nothing.
  {
    Orient o{};
    float ang = 7.0f;
    CropRect c{10, 5, 30, 20};
    int W = 100, H = 60;
    for (int i = 0; i < 4; ++i)
      geoStep(o, ang, c, W, H, GeoOp::RotateCW);
    CHECK(o.identity() && ang == 7.0f && W == 100 && H == 60);
    CHECK((c == CropRect{10, 5, 30, 20}));
    geoStep(o, ang, c, W, H, GeoOp::RotateCW);
    geoStep(o, ang, c, W, H, GeoOp::RotateCCW);
    CHECK(o.identity() && W == 100 && H == 60 && (c == CropRect{10, 5, 30, 20}));
  }
  // The crop follows the turn: explicit numbers.
  {
    Orient o{};
    float ang = 0;
    CropRect c{10, 5, 30, 20};
    int W = 100, H = 60;
    geoStep(o, ang, c, W, H, GeoOp::RotateCW);
    CHECK(o.rot == 1 && W == 60 && H == 100);
    CHECK((c == CropRect{60 - 5 - 20, 10, 20, 30}));
    geoStep(o, ang, c, W, H, GeoOp::RotateCCW);
    CHECK((c == CropRect{10, 5, 30, 20}));
  }
  // Mirrors: twice is nothing, a mirror negates the straighten angle, and FlipH
  // followed by FlipV is a half turn.
  {
    Orient o{};
    float ang = 5.0f;
    CropRect c{10, 5, 30, 20};
    int W = 100, H = 60;
    geoStep(o, ang, c, W, H, GeoOp::FlipH);
    CHECK(o.flip && ang == -5.0f && c.x == 100 - 10 - 30 && c.y == 5);
    geoStep(o, ang, c, W, H, GeoOp::FlipH);
    CHECK(o.identity() && ang == 5.0f && (c == CropRect{10, 5, 30, 20}));
    geoStep(o, ang, c, W, H, GeoOp::FlipV);
    geoStep(o, ang, c, W, H, GeoOp::FlipV);
    CHECK(o.identity() && ang == 5.0f && (c == CropRect{10, 5, 30, 20}));
    geoStep(o, ang, c, W, H, GeoOp::FlipH);
    geoStep(o, ang, c, W, H, GeoOp::FlipV);
    CHECK(o.rot == 2 && !o.flip && ang == 5.0f);
  }
  // No crop: it stays "no crop". A quarter turn leaves the angle alone.
  {
    Orient o{};
    float ang = 3.0f;
    CropRect c{};
    int W = 10, H = 20;
    geoStep(o, ang, c, W, H, GeoOp::RotateCCW);
    CHECK(c.empty() && ang == 3.0f && W == 20 && H == 10 && o.rot == 3);
  }
  // Random walks agree with the frame: after any sequence of ops, the Orient
  // reached builds the same GeoFrame as applying the ops one by one would.
  std::mt19937 rng(77);
  for (int trial = 0; trial < 300; ++trial)
  {
    Orient o{};
    float ang = 0.0f;
    CropRect c{};
    int W = 90, H = 50;
    for (int k = 0; k < 8; ++k)
      geoStep(o, ang, c, W, H, static_cast<GeoOp>(rng() % 4));
    CHECK(o.rot >= 0 && o.rot < 4);
    const GeoFrame f = makeGeoFrame(90, 50, o, 0.0f, false, CropRect{});
    CHECK(f.srcW == W && f.srcH == H);
  }
}

static void testSnapAngle()
{
  CHECK_NEAR(snapAngle(1.26f), 1.3f, 1e-5);
  CHECK_NEAR(snapAngle(-1.24f), -1.2f, 1e-5);
  CHECK(snapAngle(100.0f) == kAngleMax && snapAngle(-100.0f) == -kAngleMax);
  CHECK(snapAngle(0.04f) == 0.0f && !std::signbit(snapAngle(-0.04f))); // exactly +0 near zero
}

int main()
{
  testSmallTypes();
  testRotatedDims();
  testFrameIdentityAndUnknownSize();
  testFrameOrientations();
  testFrameStraightenAndCrop();
  testFrameScalesAndAngles();
  testGeoStep();
  testSnapAngle();
  return finish();
}
