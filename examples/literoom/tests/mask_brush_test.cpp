// Build: g++ -std=c++20 -I.. mask_brush_test.cpp -o mask_brush_test && ./mask_brush_test
#include "mask_brush.hpp"
#include "check.hpp"

#include <random>

static BrushStroke stroke(std::vector<BrushPt> pts, float size = 0.2f, float feather = 0.5f, float density = 1.0f,
                          float flow = 1.0f, bool erase = false)
{
  BrushStroke s;
  s.pts = std::move(pts);
  s.size = size;
  s.feather = feather;
  s.density = density;
  s.flow = flow;
  s.erase = erase;
  return s;
}
static BrushShape shape(std::vector<BrushStroke> strokes)
{
  BrushShape b;
  b.strokes = std::move(strokes);
  return b;
}
static int at(const BrushEval &e, int x, int y) { return (*e.map)[static_cast<size_t>(y) * e.mw + x]; }
static int centerOf(const BrushShape &b, int W = 100, int H = 100) { return at(b.rasterize(W, H), W / 2, H / 2); }

static void testDefaultsAndEmpty()
{
  const BrushStroke d;
  CHECK(!d.erase && d.size == kBrushDefaultSize && d.density == 1.0f && d.flow == 1.0f && d.pts.empty());
  CHECK(shape({stroke({{0.5f, 0.5f}})}) == shape({stroke({{0.5f, 0.5f}})}));
  CHECK(!(shape({stroke({{0.5f, 0.5f}})}) == shape({stroke({{0.5f, 0.6f}})})));
  // Nothing painted: a valid, empty map; no row has a span.
  const BrushEval e = BrushShape{}.rasterize(40, 30);
  CHECK(e.map && e.mw == 40 && e.mh == 30);
  for (uint8_t v : *e.map)
    CHECK(v == 0);
  int x0, x1;
  for (int y = 0; y < 30; ++y)
    CHECK(!e.rowSpan(y, 40, x0, x1));
  // A stroke without points paints nothing; a zero-sized picture gives no map.
  CHECK(centerOf(shape({stroke({})})) == 0);
  CHECK(!BrushShape{}.rasterize(0, 10).map && !BrushShape{}.rasterize(10, -1).map);
}

static void testSingleDab()
{
  const BrushEval e = shape({stroke({{0.5f, 0.5f}})}).rasterize(100, 100); // radius 0.2 * 100 = 20 px
  CHECK(at(e, 50, 50) == 255);
  // Round, symmetric, and nothing beyond the radius.
  int area = 0;
  for (int y = 0; y < 100; ++y)
    for (int x = 0; x < 100; ++x)
    {
      const int v = at(e, x, y);
      area += v > 0;
      const double d = std::hypot(x + 0.5 - 50.0, y + 0.5 - 50.0);
      if (d > 21.0)
        CHECK(v == 0);
      CHECK(v == at(e, 99 - x, y) && v == at(e, x, 99 - y)); // mirror symmetry
    }
  CHECK(area > 1100 && area < 1300); // ~ pi * 20^2
  // Falls monotonically from the center out along a row.
  int prev = 256;
  for (int x = 50; x < 72; ++x)
  {
    CHECK(at(e, x, 50) <= prev);
    prev = at(e, x, 50);
  }
  // Feather 0: solid up to nearly the rim. Feather 1: fades from the middle.
  const BrushEval hard = shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.0f)}).rasterize(100, 100);
  const BrushEval soft = shape({stroke({{0.5f, 0.5f}}, 0.2f, 1.0f)}).rasterize(100, 100);
  CHECK(at(hard, 50 + 17, 50) == 255 && at(soft, 50 + 17, 50) < 80);
  CHECK(at(soft, 50, 50) >= 250); // the pixel centers sit half a pixel off the dab center
}

static void testDensityAndFold()
{
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 0.5f)})) == 128); // the most a stroke adds
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 0.25f)})) == 64);
  // paint: m = m + (1 - m) c    two half-density strokes: 128 + (255 - 128) * 128 / 255
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 0.5f), stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 0.5f)})) == 192);
  // erase: m = m (1 - c)
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}), stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 0.5f, 1.0f, true)})) == 127);
  // Order matters: paint then erase leaves nothing; erase then paint leaves paint.
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}), stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 1.0f, 1.0f, true)})) == 0);
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 1.0f, 1.0f, true), stroke({{0.5f, 0.5f}})})) == 255);
  // Erasing where nothing was painted does nothing.
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 1.0f, 1.0f, true)})) == 0);
  // A stroke that crosses itself does not double up (max over dabs, not a sum).
  const BrushEval loop = shape({stroke({{0.3f, 0.5f}, {0.7f, 0.5f}, {0.3f, 0.5f}, {0.7f, 0.5f}}, 0.1f, 0.5f, 0.5f)})
                             .rasterize(100, 100);
  int maxv = 0;
  for (uint8_t v : *loop.map)
    maxv = std::max<int>(maxv, v);
  CHECK(maxv == 128);
}

static void testFlowAccumulates()
{
  for (float f : {0.1f, 0.3f, 0.6f, 0.9f})
  {
    // A click is about `flow` of the density cap.
    const int click = centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 1.0f, f)}));
    CHECK_NEAR(click, 255.0f * f, 2.0);
    // One pass across the center is a bit under that; a second pass builds up
    // exactly like opacity does: 1 - (1 - p)^2.
    const BrushStroke go = stroke({{0.2f, 0.5f}, {0.8f, 0.5f}}, 0.2f, 0.5f, 1.0f, f);
    const BrushStroke back = stroke({{0.8f, 0.5f}, {0.2f, 0.5f}}, 0.2f, 0.5f, 1.0f, f);
    const int one = centerOf(shape({go})), two = centerOf(shape({go, back}));
    CHECK(one > 0.65f * 255.0f * f && one <= 255.0f * f + 2);
    CHECK(two > one);
    const double p = one / 255.0;
    CHECK_NEAR(two, 255.0 * (1.0 - (1.0 - p) * (1.0 - p)), 3.0);
  }
  // Flow 1 is "set", not "accumulate": a pass is the full density.
  CHECK(centerOf(shape({stroke({{0.2f, 0.5f}, {0.8f, 0.5f}})})) == 255);
}

static void testMapSize()
{
  const BrushShape b = shape({stroke({{0.5f, 0.5f}})});
  const BrushEval big = b.rasterize(4000, 3000);
  CHECK(big.W == 4000 && big.H == 3000 && big.mw == kBrushMapEdge && big.mh == 1536); // long side capped, aspect kept
  const BrushEval tall = b.rasterize(1000, 5000);
  CHECK(tall.mh == kBrushMapEdge && tall.mw == 410);
  const BrushEval small = b.rasterize(300, 200);
  CHECK(small.mw == 300 && small.mh == 200);
  // The same strokes on a small and a big picture cover the same fraction of it.
  auto coverage = [](const BrushEval &e)
  {
    double sum = 0;
    for (uint8_t v : *e.map)
      sum += v;
    return sum / (255.0 * e.mw * e.mh);
  };
  CHECK_NEAR(coverage(b.rasterize(200, 200)), coverage(b.rasterize(4000, 4000)), 0.004);
}

static void testSpansAreSupersets()
{
  std::mt19937 rng(6);
  for (int trial = 0; trial < 30; ++trial)
  {
    BrushShape b;
    for (int s = 0; s < 3; ++s)
    {
      BrushStroke st;
      st.size = 0.03f + static_cast<float>(rng() % 15) / 100.0f;
      st.feather = static_cast<float>(rng() % 101) / 100.0f;
      st.erase = s == 2;
      st.flow = (s == 1) ? 0.4f : 1.0f;
      for (int p = 0; p < 6; ++p)
        st.pts.push_back(BrushPt{static_cast<float>(rng() % 110) / 100.0f - 0.05f, static_cast<float>(rng() % 110) / 100.0f - 0.05f});
      b.strokes.push_back(st);
    }
    const int W = 60 + static_cast<int>(rng() % 60), H = 40 + static_cast<int>(rng() % 60);
    const BrushEval e = b.rasterize(W, H);
    for (int y = 0; y < H; ++y)
    {
      std::vector<float> w(static_cast<size_t>(W));
      e.rowWeights(y, 0, W, w.data());
      int x0 = 0, x1 = 0;
      const bool any = e.rowSpan(y, W, x0, x1);
      for (int x = 0; x < W; ++x)
        if (w[static_cast<size_t>(x)] > 0.0f)
          CHECK(any && x >= x0 && x < x1);
    }
  }
}

static void testFrame()
{
  const BrushShape b = shape({stroke({{0.2f, 0.3f}, {0.6f, 0.7f}}, 0.1f), stroke({{0.9f, 0.1f}}, 0.05f)});
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(b.toSource(id) == b);
  // Sizes scale with the frame, points go through it.
  const GeoFrame crop = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{0, 0, 150, 100});
  const BrushShape v = b.toSource(crop);
  CHECK_NEAR(v.strokes[0].size, 0.1f * 3.0f, 1e-5); // shorter side 300 -> 100
  CHECK_NEAR(v.strokes[0].pts[0].x, 0.2f * 400.0f / 150.0f, 1e-5);
  CHECK(v.strokes[0].erase == b.strokes[0].erase && v.strokes[0].flow == b.strokes[0].flow);
  // The pieces of a new stroke map back to the stored form.
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, true}, 12.0f, true, CropRect{20, 10, 200, 150});
  const BrushPt q = BrushShape::pointFromSource(b.toSource(f).strokes[0].pts[1], f);
  CHECK_NEAR(q.x, 0.6f, 1e-4);
  CHECK_NEAR(q.y, 0.7f, 1e-4);
  CHECK_NEAR(BrushShape::sizeFromSource(b.toSource(f).strokes[0].size, f), 0.1f, 1e-5);
}

static void testIncrementalEqualsFull()
{
  const BrushStroke s1 = stroke({{0.2f, 0.2f}, {0.8f, 0.3f}}, 0.1f);
  const BrushStroke s2 = stroke({{0.3f, 0.8f}, {0.7f, 0.2f}}, 0.08f, 0.3f, 0.7f, 0.5f);
  const BrushStroke s3 = stroke({{0.5f, 0.5f}}, 0.15f, 0.5f, 1.0f, 1.0f, true);
  const BrushShape all = shape({s1, s2, s3});
  const BrushEval full = all.rasterize(120, 90);
  const BrushEval base = shape({s1}).rasterize(120, 90);
  const BrushEval inc = all.rasterize(120, 90, &base, 1); // only s2, s3 stamped onto s1's map
  CHECK(*inc.map == *full.map && *inc.lo == *full.lo && *inc.hi == *full.hi);
  // A base of the wrong size is ignored (rasterizes everything).
  const BrushEval wrong = shape({s1}).rasterize(60, 45);
  const BrushEval ignored = all.rasterize(120, 90, &wrong, 1);
  CHECK(*ignored.map == *full.map);
}

static void testCache()
{
  brushCache().clear();
  CHECK(brushCache().size() == 0);
  const BrushShape a = shape({stroke({{0.4f, 0.4f}, {0.6f, 0.6f}})});
  const BrushEval e1 = a.prepare(100, 80);
  const BrushEval e2 = a.prepare(100, 80);
  CHECK(e1.map == e2.map && brushCache().size() == 1); // a hit: the very same map
  const BrushEval e3 = a.prepare(200, 160);
  CHECK(e3.map != e1.map && brushCache().size() == 2); // another size is another entry
  CHECK(*a.prepare(100, 80).map == *a.rasterize(100, 80).map); // and a cached map is a correct one

  // Painting one more stroke reuses the map of the strokes before it.
  BrushShape more = a;
  more.strokes.push_back(stroke({{0.2f, 0.7f}}, 0.1f));
  const BrushEval grown = more.prepare(100, 80);
  CHECK(*grown.map == *more.rasterize(100, 80).map);

  // The cache holds the last kBrushCacheSize maps, most recent first.
  brushCache().clear();
  std::vector<BrushShape> shapes;
  for (int i = 0; i < static_cast<int>(kBrushCacheSize) + 2; ++i)
    shapes.push_back(shape({stroke({{0.1f + 0.1f * static_cast<float>(i), 0.5f}}, 0.05f + 0.01f * static_cast<float>(i))}));
  std::vector<BrushEval> evals;
  for (const BrushShape &s : shapes)
    evals.push_back(s.prepare(64, 64));
  CHECK(brushCache().size() == kBrushCacheSize);
  CHECK(shapes.back().prepare(64, 64).map == evals.back().map); // newest: still there
  CHECK(shapes.front().prepare(64, 64).map != evals.front().map); // oldest: evicted, rebuilt
  brushCache().clear();
  CHECK(brushCache().size() == 0);
}


static void testRoundingAndBaseReuse()
{
  // erase: m - round(m * c / 255), with whole-number steps: 200 - round(200 * 80 / 255) = 200 - 63
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 200.0f / 255.0f), stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 80.0f / 255.0f, 1.0f, true)})) == 137);
  // paint: m + round((255 - m) * c / 255): 100 + round(155 * 60 / 255) = 100 + 36
  CHECK(centerOf(shape({stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 100.0f / 255.0f), stroke({{0.5f, 0.5f}}, 0.2f, 0.5f, 60.0f / 255.0f)})) == 136);
  // A base map is only reused when it has the same size in BOTH directions.
  const BrushStroke s1 = stroke({{0.2f, 0.2f}, {0.8f, 0.3f}}, 0.1f);
  const BrushStroke s2 = stroke({{0.3f, 0.8f}, {0.7f, 0.2f}}, 0.08f);
  const BrushShape all = shape({s1, s2});
  const BrushEval full = all.rasterize(120, 90);
  const BrushEval sameWidth = shape({s1}).rasterize(120, 60);
  const BrushEval out = all.rasterize(120, 90, &sameWidth, 1);
  CHECK(out.mh == 90 && *out.map == *full.map);
  CHECK(kBrushDefaultSize == 0.05f && kBrushMinSize == 0.005f && kBrushMaxSize == 0.5f);
}

int main()
{
  testRoundingAndBaseReuse();

  testDefaultsAndEmpty();
  testSingleDab();
  testDensityAndFold();
  testFlowAccumulates();
  testMapSize();
  testSpansAreSupersets();
  testFrame();
  testIncrementalEqualsFull();
  testCache();
  return finish();
}
