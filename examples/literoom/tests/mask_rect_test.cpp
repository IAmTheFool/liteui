// Build: g++ -std=c++20 -I.. mask_rect_test.cpp -o mask_rect_test && ./mask_rect_test
#include "mask/mask_rect.hpp"
#include "check.hpp"

#include <random>
#include <vector>

template <class Eval>
static std::vector<float> fullRow(const Eval &e, int y, int W)
{
  std::vector<float> w(static_cast<size_t>(W));
  e.rowWeights(y, 0, W, w.data());
  return w;
}

// rowSpan must contain EVERY pixel that has a non-zero weight. Returns how many
// pixels the spans hold beyond the non-zero ones (how loose they are).
template <class Eval>
static long checkSpanCoversAll(const Eval &e, int W, int H, const char *what)
{
  long extra = 0;
  for (int y = 0; y < H; ++y)
  {
    const std::vector<float> w = fullRow(e, y, W);
    int x0 = 0, x1 = 0;
    const bool any = e.rowSpan(y, W, x0, x1);
    int first = -1, last = -1;
    for (int x = 0; x < W; ++x)
      if (w[static_cast<size_t>(x)] > 0.0f)
      {
        if (first < 0)
          first = x;
        last = x;
        if (!(any && x >= x0 && x < x1))
        {
          std::printf("  %s: row %d pixel %d has weight %g outside the span [%d,%d) any=%d\n", what, y, x,
                      w[static_cast<size_t>(x)], x0, x1, any);
          CHECK(false);
          return extra;
        }
      }
    if (any)
    {
      CHECK(x0 >= 0 && x1 <= W && x1 > x0);
      extra += (x1 - x0) - (first < 0 ? 0 : last - first + 1);
    }
  }
  return extra;
}

static void testBasics()
{
  RectShape r;
  r.cx = r.cy = 0.5f;
  r.rx = 0.4f;
  r.ry = 0.2f;
  r.angle = 0.0f;
  r.feather = 0.5f;
  const RectEval e = r.prepare(200, 100); // shorter side 100: half sizes 40 x 20 px
  CHECK_NEAR(e.rx, 40.0f, 1e-3);
  CHECK_NEAR(e.ry, 20.0f, 1e-3);
  const auto row = fullRow(e, 50, 200);
  CHECK(row[100] == 1.0f);
  CHECK(row[100 + 15] == 1.0f && row[100 + 30] < 1.0f); // inside `inner` (20 px at feather 0.5), then falling
  CHECK(row[100 + 45] == 0.0f);   // beyond the half width
  CHECK(row[0] == 0.0f && row[199] == 0.0f);
  // Falls monotonically from the inner edge to the edge, never leaves 0..1.
  float prev = 1.0f;
  for (int x = 100; x < 145; ++x)
  {
    CHECK(row[static_cast<size_t>(x)] <= prev + 1e-6f && row[static_cast<size_t>(x)] >= 0.0f);
    prev = row[static_cast<size_t>(x)];
  }
  // Symmetric about the center.
  for (int d = 1; d < 45; ++d)
    CHECK_NEAR(row[static_cast<size_t>(100 + d - 1)], row[static_cast<size_t>(100 - d)], 1e-5);
  // The corner of the rectangle is inside it (a rectangle, not an ellipse).
  const auto top = fullRow(e, 50 - 19, 200);
  CHECK(top[100 + 30] > 0.0f && top[100 + 30] <= 1.0f);
  RadialShape el;
  el.cx = el.cy = 0.5f;
  el.rx = 0.4f;
  el.ry = 0.2f;
  const RadialEval ee = el.prepare(200, 100);
  float w;
  ee.rowWeights(50 - 19, 130, 131, &w);
  CHECK(w == 0.0f); // the same spot is outside the ellipse
}

static void testFeather()
{
  auto weightAt = [](float feather, int dx)
  {
    RectShape r;
    r.rx = r.ry = 0.25f;
    r.feather = feather;
    const RectEval e = r.prepare(200, 100);
    float w;
    e.rowWeights(50, 100 + dx, 100 + dx + 1, &w);
    return w;
  };
  CHECK(weightAt(0.0f, 20) == 1.0f && weightAt(0.0f, 26) == 0.0f); // hard edge: nearly a step
  CHECK(weightAt(0.2f, 20) > weightAt(0.5f, 20));
  CHECK(weightAt(0.5f, 20) > weightAt(1.0f, 20));
  CHECK(weightAt(1.0f, 1) < 1.0f && weightAt(1.0f, 0) > 0.99f);
  RectShape s;
  s.feather = -3.0f;
  CHECK(s.innerFrac() == 0.99f);
  s.feather = 2.0f;
  CHECK(s.innerFrac() == 0.0f);
  s.feather = 0.25f;
  CHECK_NEAR(s.innerFrac(), 0.75f, 1e-6);
  // The falloff, written out.
  for (float feather : {0.0f, 0.3f, 0.7f, 1.0f})
  {
    RectShape q;
    q.cx = q.cy = 0.5f;
    q.rx = 0.4f;
    q.ry = 0.2f;
    q.angle = 0.0f;
    q.feather = feather;
    const RectEval e = q.prepare(200, 100);
    const float inner = std::clamp(1.0f - feather, 0.0f, 0.99f);
    for (int y = 35; y <= 65; y += 6)
    {
      const auto w = fullRow(e, y, 200);
      for (int x = 55; x < 145; ++x)
      {
        const float u = std::fabs((x + 0.5f - 100.0f) / 40.0f), v = std::fabs((y + 0.5f - 50.0f) / 20.0f);
        const float d = std::max(u, v);
        float expect;
        if (d <= inner)
          expect = 1.0f;
        else if (d >= 1.0f)
          expect = 0.0f;
        else
        {
          const float t = (d - inner) / (1.0f - inner);
          expect = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        CHECK_NEAR(w[static_cast<size_t>(x)], expect, 1e-4);
      }
    }
  }
}

static void testGeometry()
{
  // Turning it 90 degrees with the half sizes swapped gives the same mask.
  RectShape a, b;
  a.cx = b.cx = 0.4f;
  a.cy = b.cy = 0.55f;
  a.rx = 0.30f;
  a.ry = 0.12f;
  b.rx = 0.12f;
  b.ry = 0.30f;
  b.angle = 90.0f;
  const RectEval ea = a.prepare(160, 120), eb = b.prepare(160, 120);
  for (int y = 0; y < 120; y += 7)
  {
    const auto wa = fullRow(ea, y, 160), wb = fullRow(eb, y, 160);
    for (int x = 0; x < 160; ++x)
      CHECK_NEAR(wa[static_cast<size_t>(x)], wb[static_cast<size_t>(x)], 1e-4);
  }
  // Sizes are fractions of the SHORTER side.
  RectShape c;
  c.rx = c.ry = 0.25f;
  CHECK_NEAR(c.prepare(160, 120).rx, 30.0f, 1e-3);
  CHECK_NEAR(c.prepare(120, 160).rx, 30.0f, 1e-3);
  RectShape tiny;
  tiny.rx = tiny.ry = 0.0f; // raised to the minimum, never divides by zero
  CHECK_NEAR(tiny.prepare(100, 100).rx, kMaskMinRadius * 100.0f, 1e-3);
}

static void testSpanIsExact()
{
  std::mt19937 rng(17);
  long extraSum = 0, pixelsSum = 0;
  for (int i = 0; i < 120; ++i)
  {
    RectShape r;
    r.cx = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f; // may sit partly or fully off the picture
    r.cy = static_cast<float>(static_cast<int>(rng() % 120) - 10) / 100.0f;
    r.rx = 0.02f + static_cast<float>(rng() % 90) / 100.0f;
    r.ry = 0.02f + static_cast<float>(rng() % 90) / 100.0f;
    r.angle = static_cast<float>(static_cast<int>(rng() % 360) - 180);
    if (i % 6 == 0)
      r.angle = static_cast<float>(90 * static_cast<int>(rng() % 4)); // exactly axis-aligned
    r.feather = static_cast<float>(rng() % 101) / 100.0f;
    const int W = 20 + static_cast<int>(rng() % 90), H = 20 + static_cast<int>(rng() % 90);
    extraSum += checkSpanCoversAll(r.prepare(W, H), W, H, "rect");
    pixelsSum += static_cast<long>(W) * H;
  }
  (void)pixelsSum;
  // Tight, not "the whole row": at most the padding (2 px) plus the pixel rounding per row.
  RectShape s;
  s.cx = s.cy = 0.5f;
  s.rx = s.ry = 0.1f;
  s.angle = 0.0f;
  const RectEval e = s.prepare(400, 400);
  int x0, x1;
  CHECK(e.rowSpan(200, 400, x0, x1) && x1 - x0 <= 80 + 4); // 2 * 40 px + pad
  CHECK(!e.rowSpan(10, 400, x0, x1));                      // a row the rectangle never reaches
  CHECK(!e.rowSpan(399, 400, x0, x1));
  // A turned rectangle: a row through the middle spans its full diagonal width, narrower near the tips.
  s.angle = 45.0f;
  const RectEval d = s.prepare(400, 400);
  int m0, m1, t0, t1;
  CHECK(d.rowSpan(200, 400, m0, m1) && d.rowSpan(200 - 50, 400, t0, t1));
  CHECK((m1 - m0) > (t1 - t0));
  // Exactness for an axis-aligned rectangle: the span is the rectangle's width plus the padding only.
  s.angle = 0.0f;
  s.rx = 0.25f;
  s.ry = 0.1f;
  const RectEval f = s.prepare(400, 400);
  CHECK(f.rowSpan(200, 400, x0, x1) && x0 >= 100 - 2 && x1 <= 300 + 2 && x1 - x0 >= 200);
}

static void testFrame()
{
  RectShape r;
  r.cx = 0.3f;
  r.cy = 0.6f;
  r.rx = 0.2f;
  r.ry = 0.1f;
  r.angle = 30.0f;
  r.feather = 0.4f;
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(r.toSource(id) == r);
  std::mt19937 rng(5);
  for (int i = 0; i < 200; ++i)
  {
    const Orient o{static_cast<int>(rng() % 4), (rng() & 1) != 0};
    const float ang = static_cast<float>(static_cast<int>(rng() % 6000) - 3000) / 100.0f;
    const GeoFrame f = makeGeoFrame(400, 300, o, ang, (rng() & 1) != 0, CropRect{});
    const RectShape back = RectShape::fromSource(r.toSource(f), f);
    CHECK_NEAR(back.cx, r.cx, 1e-4);
    CHECK_NEAR(back.cy, r.cy, 1e-4);
    CHECK_NEAR(back.rx, r.rx, 1e-4);
    CHECK_NEAR(back.ry, r.ry, 1e-4);
    CHECK(back.feather == r.feather);
    const float d = std::fmod(back.angle - r.angle + 270.0f, 180.0f) - 90.0f; // an axis: modulo half a turn
    CHECK(std::fabs(d) < 1e-2f);
  }
  // A quarter turn clockwise: a rectangle in the top-left ends up in the top-right.
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  RectShape tl;
  tl.cx = 0.1f;
  tl.cy = 0.1f;
  const RectShape moved = tl.toSource(rot);
  CHECK(moved.cx > 0.8f && moved.cy < 0.2f);
}

static void testApplySourceEdit()
{
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, true}, 7.0f, true, CropRect{});
  RectShape stored;
  stored.cx = 0.3f;
  stored.cy = 0.4f;
  const RectShape before = stored.toSource(f);
  RectShape after = before;
  after.feather = 0.9f; // only the feather: nothing else is touched (no float drift)
  RectShape s2 = stored;
  s2.applySourceEdit(before, after, f);
  CHECK(s2.cx == stored.cx && s2.cy == stored.cy && s2.rx == stored.rx && s2.ry == stored.ry && s2.angle == stored.angle);
  CHECK(s2.feather == 0.9f);
  RectShape s3 = stored; // no change at all: equal (so no undo step)
  s3.applySourceEdit(before, before, f);
  CHECK(s3 == stored);
  RectShape moved = before; // a moved center lands where the frame says
  moved.cx += 0.1f;
  RectShape s4 = stored;
  s4.applySourceEdit(before, moved, f);
  const RectShape view4 = s4.toSource(f);
  CHECK_NEAR(view4.cx, moved.cx, 1e-4);
  CHECK_NEAR(view4.cy, moved.cy, 1e-4);
  RectShape grown = before; // a resized half size maps back through the radius scale
  grown.rx += 0.05f;
  RectShape s5 = stored;
  s5.applySourceEdit(before, grown, f);
  CHECK_NEAR(s5.toSource(f).rx, grown.rx, 1e-4);
  CHECK(s5.ry == stored.ry);
}

int main()
{
  testBasics();
  testFeather();
  testGeometry();
  testSpanIsExact();
  testFrame();
  testApplySourceEdit();
  return finish();
}