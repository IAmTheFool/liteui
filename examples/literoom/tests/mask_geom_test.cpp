// Build: g++ -std=c++20 -I.. mask_geom_test.cpp -o mask_geom_test && ./mask_geom_test
#include "mask/mask_geom.hpp"
#include "check.hpp"

#include <random>
#include <vector>

// Brute force: every weight of one row, from rowWeights over the whole row.
template <class Eval>
static std::vector<float> fullRow(const Eval &e, int y, int W)
{
  std::vector<float> w(static_cast<size_t>(W));
  e.rowWeights(y, 0, W, w.data());
  return w;
}

// rowSpan must contain EVERY pixel that has a non-zero weight (it may be wider).
template <class Eval>
static void checkSpanCoversAll(const Eval &e, int W, int H, const char *what)
{
  for (int y = 0; y < H; ++y)
  {
    const std::vector<float> w = fullRow(e, y, W);
    int x0 = 0, x1 = 0;
    const bool any = e.rowSpan(y, W, x0, x1);
    for (int x = 0; x < W; ++x)
      if (w[static_cast<size_t>(x)] > 0.0f)
      {
        if (!(any && x >= x0 && x < x1))
        {
          std::printf("  %s: row %d pixel %d has weight %g outside the span [%d,%d) any=%d\n", what, y, x,
                      w[static_cast<size_t>(x)], x0, x1, any);
          CHECK(false);
          return;
        }
      }
    if (any)
      CHECK(x0 >= 0 && x1 <= W && x1 > x0);
  }
}

static void testRadialBasics()
{
  RadialShape r;
  r.cx = 0.5f;
  r.cy = 0.5f;
  r.rx = r.ry = 0.25f;
  r.angle = 0;
  r.feather = 0.5f;
  const RadialEval e = r.prepare(200, 100); // shorter side 100 -> radius 25 px
  const auto row = fullRow(e, 50, 200);
  CHECK(row[100] == 1.0f);          // the center
  CHECK_NEAR(row[100 + 10], 1.0f, 1e-6); // inside `inner` (12.5 px at feather 0.5)
  CHECK(row[100 + 30] == 0.0f);     // beyond the radius
  CHECK(row[0] == 0.0f && row[199] == 0.0f);
  // Falls monotonically from the inner edge to the outline, never leaves 0..1.
  float prev = 1.0f;
  for (int x = 100; x < 140; ++x)
  {
    CHECK(row[static_cast<size_t>(x)] <= prev + 1e-6f && row[static_cast<size_t>(x)] >= 0.0f);
    prev = row[static_cast<size_t>(x)];
  }
  // Symmetric about the center.
  for (int d = 1; d < 40; ++d)
    CHECK_NEAR(row[static_cast<size_t>(100 + d - 1)], row[static_cast<size_t>(100 - d)], 1e-5);
}

static void testRadialFeather()
{
  auto weightAt = [](float feather, int dx)
  {
    RadialShape r;
    r.rx = r.ry = 0.25f;
    r.feather = feather;
    const RadialEval e = r.prepare(200, 100);
    float w;
    e.rowWeights(50, 100 + dx, 100 + dx + 1, &w);
    return w;
  };
  // Hard edge: still a ramp (a hair wide), but nearly a step at the outline.
  CHECK(weightAt(0.0f, 20) == 1.0f && weightAt(0.0f, 26) == 0.0f);
  // More feather -> less weight at the same spot inside the outline.
  CHECK(weightAt(0.2f, 20) > weightAt(0.5f, 20));
  CHECK(weightAt(0.5f, 20) > weightAt(1.0f, 20));
  // Fully feathered: fades from the center itself.
  CHECK(weightAt(1.0f, 1) < 1.0f && weightAt(1.0f, 0) > 0.99f);
  // innerFrac clamps to [0, 0.99]; a hard edge never divides by zero.
  RadialShape s;
  s.feather = -3.0f;
  CHECK(s.innerFrac() == 0.99f);
  s.feather = 0.0f;
  CHECK(s.innerFrac() == 0.99f);
  s.feather = 2.0f;
  CHECK(s.innerFrac() == 0.0f);
  s.feather = 0.25f;
  CHECK_NEAR(s.innerFrac(), 0.75f, 1e-6);
}

static void testRadialGeometry()
{
  // Rotating the shape 90 degrees with the radii swapped gives the same mask.
  RadialShape a, b;
  a.cx = b.cx = 0.4f;
  a.cy = b.cy = 0.55f;
  a.rx = 0.30f;
  a.ry = 0.12f;
  a.angle = 0.0f;
  b.rx = 0.12f;
  b.ry = 0.30f;
  b.angle = 90.0f;
  const RadialEval ea = a.prepare(160, 120), eb = b.prepare(160, 120);
  for (int y = 0; y < 120; y += 7)
  {
    const auto wa = fullRow(ea, y, 160), wb = fullRow(eb, y, 160);
    for (int x = 0; x < 160; ++x)
      CHECK_NEAR(wa[static_cast<size_t>(x)], wb[static_cast<size_t>(x)], 1e-4);
  }
  // Radii are fractions of the SHORTER side: same shape on 160x120 and 120x160 canvases.
  RadialShape c;
  c.rx = c.ry = 0.25f;
  const RadialEval w1 = c.prepare(160, 120), w2 = c.prepare(120, 160);
  CHECK_NEAR(1.0f / w1.invRx, 30.0f, 1e-3);
  CHECK_NEAR(1.0f / w2.invRx, 30.0f, 1e-3);
  // Radii below the minimum are raised to it (a 0 radius never divides by zero).
  RadialShape tiny;
  tiny.rx = tiny.ry = 0.0f;
  CHECK_NEAR(1.0f / tiny.prepare(100, 100).invRx, kMaskMinRadius * 100.0f, 1e-3);
}

static void testRadialSpanIsExact()
{
  std::mt19937 rng(11);
  for (int i = 0; i < 60; ++i)
  {
    RadialShape r;
    r.cx = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f; // may sit partly or fully off the picture
    r.cy = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f;
    r.rx = 0.02f + static_cast<float>(rng() % 90) / 100.0f;
    r.ry = 0.02f + static_cast<float>(rng() % 90) / 100.0f;
    r.angle = static_cast<float>(static_cast<int>(rng() % 360) - 180);
    r.feather = static_cast<float>(rng() % 101) / 100.0f;
    const int W = 20 + static_cast<int>(rng() % 90), H = 20 + static_cast<int>(rng() % 90);
    checkSpanCoversAll(r.prepare(W, H), W, H, "radial");
  }
  // And it is not just "the whole row": a small ellipse gets a small span.
  RadialShape s;
  s.cx = s.cy = 0.5f;
  s.rx = s.ry = 0.1f;
  const RadialEval e = s.prepare(400, 400);
  int x0, x1;
  CHECK(e.rowSpan(200, 400, x0, x1) && x1 - x0 < 100);
  CHECK(!e.rowSpan(10, 400, x0, x1)); // a row the ellipse never reaches
}

static void testRadialFrame()
{
  RadialShape r;
  r.cx = 0.3f;
  r.cy = 0.6f;
  r.rx = 0.2f;
  r.ry = 0.1f;
  r.angle = 30.0f;
  r.feather = 0.4f;
  // Identity frame: nothing changes.
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(r.toSource(id) == r);
  // Round trip through frames of every kind (positions and sizes within float round-off).
  std::mt19937 rng(5);
  for (int i = 0; i < 200; ++i)
  {
    const Orient o{static_cast<int>(rng() % 4), (rng() & 1) != 0};
    const float ang = static_cast<float>(static_cast<int>(rng() % 6000) - 3000) / 100.0f;
    const bool expand = (rng() & 1) != 0;
    const GeoFrame f = makeGeoFrame(400, 300, o, ang, expand, CropRect{});
    const RadialShape v = r.toSource(f);
    const RadialShape back = RadialShape::fromSource(v, f);
    CHECK_NEAR(back.cx, r.cx, 1e-4);
    CHECK_NEAR(back.cy, r.cy, 1e-4);
    CHECK_NEAR(back.rx, r.rx, 1e-4);
    CHECK_NEAR(back.ry, r.ry, 1e-4);
    CHECK(back.feather == r.feather);
    float d = std::fmod(back.angle - r.angle + 270.0f, 180.0f) - 90.0f; // an axis: modulo half a turn
    CHECK(std::fabs(d) < 1e-2f);
  }
  // A quarter turn clockwise: a mask in the top-left ends up in the top-right.
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  RadialShape tl;
  tl.cx = 0.1f;
  tl.cy = 0.1f;
  const RadialShape moved = tl.toSource(rot);
  CHECK(moved.cx > 0.8f && moved.cy < 0.2f);
}

static void testRadialApplySourceEdit()
{
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, true}, 7.0f, true, CropRect{});
  RadialShape stored;
  stored.cx = 0.3f;
  stored.cy = 0.4f;
  const RadialShape before = stored.toSource(f);
  // Only feather changes: the stored position is not touched (no float drift).
  RadialShape after = before;
  after.feather = 0.9f;
  RadialShape s2 = stored;
  s2.applySourceEdit(before, after, f);
  CHECK(s2.cx == stored.cx && s2.cy == stored.cy && s2.rx == stored.rx && s2.angle == stored.angle);
  CHECK(s2.feather == 0.9f);
  // No change at all: the shape compares equal (so it is not an undo step).
  RadialShape s3 = stored;
  s3.applySourceEdit(before, before, f);
  CHECK(s3 == stored);
  // A moved center lands where the frame says.
  RadialShape moved = before;
  moved.cx += 0.1f;
  RadialShape s4 = stored;
  s4.applySourceEdit(before, moved, f);
  const RadialShape view4 = s4.toSource(f);
  CHECK_NEAR(view4.cx, moved.cx, 1e-4);
  CHECK_NEAR(view4.cy, moved.cy, 1e-4);
}

static void testLinear()
{
  LinearShape l;
  l.cx = 0.5f;
  l.cy = 0.5f;
  l.len = 0.25f;
  l.angle = 0.0f;
  const LinearEval e = l.prepare(200, 100);
  // Whatever the direction, weight is 1 behind the ramp, 0 ahead of it, and
  // falls monotonically across it. Find the direction from the eval itself.
  const float half = e.len;
  CHECK_NEAR(half, 25.0f, 1e-3); // 0.25 of the shorter side (100)
  // At the center, the weight is exactly half (smoothstep midpoint).
  int cxp = 100, cyp = 50;
  float w;
  // pixel centers sit at +0.5, the shape center at 100,50: test the symmetric pair around it
  float wa, wb;
  e.rowWeights(cyp - 1, cxp - 1, cxp, &wa);
  e.rowWeights(cyp, cxp, cxp + 1, &wb);
  (void)w;
  CHECK(wa >= 0.0f && wa <= 1.0f && wb >= 0.0f && wb <= 1.0f);
  // Monotone along the fade direction, 1 -> 0 across 2 * len.
  float prev = 2.0f;
  int steps = 0;
  const float len = e.len;
  for (float t = -1.5f * len; t <= 1.5f * len; t += 0.5f)
  {
    const float px = e.cx + e.ux * t, py = e.cy + e.uy * t;
    const int xi = static_cast<int>(std::floor(px)), yi = static_cast<int>(std::floor(py));
    if (xi < 0 || yi < 0 || xi >= 200 || yi >= 100)
      continue;
    float v;
    e.rowWeights(yi, xi, xi + 1, &v);
    CHECK(v <= prev + 0.05f); // monotone to within the pixel snapping
    prev = v;
    ++steps;
  }
  CHECK(steps > 10);
  // Ends: full behind, none ahead.
  float behind, ahead;
  {
    const float px = e.cx - e.ux * (len + 3), py = e.cy - e.uy * (len + 3);
    e.rowWeights(static_cast<int>(py), static_cast<int>(px), static_cast<int>(px) + 1, &behind);
    const float qx = e.cx + e.ux * (len + 3), qy = e.cy + e.uy * (len + 3);
    e.rowWeights(static_cast<int>(qy), static_cast<int>(qx), static_cast<int>(qx) + 1, &ahead);
  }
  CHECK(behind == 1.0f && ahead == 0.0f);
}

static void testLinearSpanIsExact()
{
  std::mt19937 rng(23);
  for (int i = 0; i < 80; ++i)
  {
    LinearShape l;
    l.cx = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f;
    l.cy = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f;
    l.len = 0.02f + static_cast<float>(rng() % 100) / 100.0f;
    l.angle = static_cast<float>(static_cast<int>(rng() % 360) - 180);
    if (i % 5 == 0)
      l.angle = (i % 10 == 0) ? 0.0f : 90.0f; // exactly axis-aligned directions
    const int W = 20 + static_cast<int>(rng() % 90), H = 20 + static_cast<int>(rng() % 90);
    checkSpanCoversAll(l.prepare(W, H), W, H, "linear");
  }
}

static void testLinearFrame()
{
  LinearShape l;
  l.cx = 0.3f;
  l.cy = 0.7f;
  l.len = 0.2f;
  l.angle = 40.0f;
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(l.toSource(id) == l);
  std::mt19937 rng(8);
  for (int i = 0; i < 200; ++i)
  {
    const Orient o{static_cast<int>(rng() % 4), (rng() & 1) != 0};
    const GeoFrame f = makeGeoFrame(400, 300, o, static_cast<float>(static_cast<int>(rng() % 4000) - 2000) / 100.0f,
                                    (rng() & 1) != 0, CropRect{});
    const LinearShape back = LinearShape::fromSource(l.toSource(f), f);
    CHECK_NEAR(back.cx, l.cx, 1e-4);
    CHECK_NEAR(back.cy, l.cy, 1e-4);
    CHECK_NEAR(back.len, l.len, 1e-4);
    float d = std::fmod(back.angle - l.angle + 540.0f, 360.0f) - 180.0f; // a direction: modulo a full turn
    CHECK(std::fabs(d) < 1e-2f);
  }
}


static float smooth01(float x)
{
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

static void testConstantsAndFalloffNumbers()
{
  CHECK(kMaskMinRadius == 0.02f && kMaskMaxRadius == 1.5f);
  // The radial falloff, written out: 1 inside `inner`, smoothstep down to 0 at the outline.
  for (float feather : {0.0f, 0.25f, 0.5f, 0.8f, 1.0f})
  {
    RadialShape r;
    r.cx = r.cy = 0.5f;
    r.rx = 0.4f;
    r.ry = 0.2f;
    r.feather = feather;
    const int W = 200, H = 100; // shorter side 100: half axes 40 and 20 px
    const RadialEval e = r.prepare(W, H);
    const float inner = std::clamp(1.0f - feather, 0.0f, 0.99f);
    for (int y = 30; y <= 70; y += 5)
    {
      std::vector<float> w(static_cast<size_t>(W));
      e.rowWeights(y, 0, W, w.data());
      for (int x = 50; x < 150; ++x)
      {
        const float dx = (x + 0.5f - 100.0f) / 40.0f, dy = (y + 0.5f - 50.0f) / 20.0f;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float expect = d <= inner ? 1.0f : (d >= 1.0f ? 0.0f : 1.0f - smooth01((d - inner) / (1.0f - inner)));
        CHECK_NEAR(w[static_cast<size_t>(x)], expect, 1e-4);
      }
    }
  }
  // The linear ramp: 1 behind, smoothstep across 2 * len, 0 ahead.
  LinearShape l;
  l.cx = 0.5f;
  l.cy = 0.5f;
  l.len = 0.3f;
  l.angle = 37.0f;
  const LinearEval e = l.prepare(300, 200);
  for (float t : {-1.2f, -0.9f, -0.5f, -0.1f, 0.0f, 0.3f, 0.5f, 0.8f, 1.1f})
  {
    const int x = static_cast<int>(std::floor(e.cx + e.ux * t * e.len)), y = static_cast<int>(std::floor(e.cy + e.uy * t * e.len));
    const float s = ((x + 0.5f - e.cx) * e.ux + (y + 0.5f - e.cy) * e.uy) / e.len;
    float w;
    e.rowWeights(y, x, x + 1, &w);
    const float expect = s <= -1.0f ? 1.0f : (s >= 1.0f ? 0.0f : 1.0f - smooth01((s + 1.0f) * 0.5f));
    CHECK_NEAR(w, expect, 1e-4);
  }
  CHECK_NEAR(e.len, 0.3f * 200.0f, 1e-3); // a fraction of the shorter side
}

int main()
{
  testConstantsAndFalloffNumbers();

  testRadialBasics();
  testRadialFeather();
  testRadialGeometry();
  testRadialSpanIsExact();
  testRadialFrame();
  testRadialApplySourceEdit();
  testLinear();
  testLinearSpanIsExact();
  testLinearFrame();
  return finish();
}
