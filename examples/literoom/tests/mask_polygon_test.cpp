// Build: g++ -std=c++20 -I.. mask_polygon_test.cpp -o mask_polygon_test && ./mask_polygon_test
#include "mask/mask_polygon.hpp"
#include "check.hpp"

#include <random>
#include <vector>

static std::vector<float> fullRow(const PolygonEval &e, int y, int W)
{
  std::vector<float> w(static_cast<size_t>(W));
  e.rowWeights(y, 0, W, w.data());
  return w;
}

// rowSpan must contain EVERY pixel that has a non-zero weight. Returns the
// pixels the spans hold beyond the non-zero ones (how loose they are).
static long checkSpanCoversAll(const PolygonEval &e, int W, int H, const char *what)
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

static double totalWeight(const PolygonEval &e, int W, int H)
{
  double sum = 0;
  for (int y = 0; y < H; ++y)
    for (float v : fullRow(e, y, W))
      sum += v;
  return sum;
}

static PolygonShape square(float x0, float y0, float x1, float y1, float feather = 0.0f)
{
  PolygonShape s;
  s.pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  s.feather = feather;
  return s;
}

static void testHelpers()
{
  const std::vector<PolyPt> sq = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
  CHECK(polyContains(sq, 5, 5) && !polyContains(sq, 15, 5) && !polyContains(sq, 5, -1));
  CHECK(!polyContains({{0, 0}, {10, 10}}, 5, 5)); // fewer than 3 points: nothing inside
  // Even-odd: the overlap of a self-crossing outline is a hole.
  const std::vector<PolyPt> bow = {{0, 0}, {10, 10}, {10, 0}, {0, 10}}; // a bow tie
  CHECK(polyContains(bow, 2, 5) && polyContains(bow, 8, 5) && !polyContains(bow, 5, 1));
  float cx, cy;
  CHECK_NEAR(polySegDist(5, 3, PolyPt{0, 0}, PolyPt{10, 0}, &cx, &cy), 3.0, 1e-5);
  CHECK_NEAR(cx, 5.0, 1e-5);
  CHECK_NEAR(cy, 0.0, 1e-5);
  CHECK_NEAR(polySegDist(-4, 3, PolyPt{0, 0}, PolyPt{10, 0}), 5.0, 1e-5); // clamps to the end point
  CHECK_NEAR(polySegDist(3, 4, PolyPt{1, 1}, PolyPt{1, 1}), std::hypot(2.0f, 3.0f), 1e-5); // zero-length segment
  int edge = -2;
  CHECK_NEAR(polyNearestEdge(sq, 5, 9, &edge, &cx, &cy), 1.0, 1e-5);
  CHECK(edge == 2); // the top edge... y = 10 is edge 2 (points 2 -> 3)
  CHECK_NEAR(polyNearestEdge(sq, 0.5f, 5, &edge), 0.5, 1e-5);
  CHECK(edge == 3); // the closing edge, point 3 -> point 0
  CHECK(polyNearestEdge({{1, 1}}, 0, 0, &edge) < 0.0f && edge == -1);
  CHECK(polyNearestEdge({{0, 0}, {4, 0}}, 2, 3, &edge) == 3.0f && edge == 0);

  // Simplify: noise around a line collapses to its ends, a real corner survives.
  std::vector<PolyPt> line;
  for (int i = 0; i <= 50; ++i)
    line.push_back({static_cast<float>(i), (i % 2) ? 0.2f : -0.2f});
  const auto two = polySimplify(line, 1.0f);
  CHECK(two.size() == 2 && two.front() == line.front() && two.back() == line.back());
  std::vector<PolyPt> ell;
  for (int i = 0; i <= 20; ++i)
    ell.push_back({static_cast<float>(i), 0.0f});
  for (int i = 1; i <= 20; ++i)
    ell.push_back({20.0f, static_cast<float>(i)});
  const auto corner = polySimplify(ell, 0.5f);
  CHECK(corner.size() == 3 && corner[1] == (PolyPt{20.0f, 0.0f}));
  CHECK(polySimplify(line, 0.0f).size() == line.size()); // zero tolerance keeps the zigzag
  CHECK(polySimplify({{0, 0}, {1, 1}}, 5.0f).size() == 2);
  CHECK(polySimplify({}, 5.0f).empty());
  // A circle sampled finely keeps most of its shape at a small tolerance.
  std::vector<PolyPt> circ;
  for (int i = 0; i <= 200; ++i)
    circ.push_back({100.0f * std::cos(i * 0.0314159f), 100.0f * std::sin(i * 0.0314159f)});
  const auto cs = polySimplify(circ, 0.5f);
  CHECK(cs.size() > 20 && cs.size() < circ.size());
}

static void testTooFewPoints()
{
  PolygonShape s;
  for (size_t n = 0; n < 3; ++n)
  {
    s.pts.assign(n, PolyPt{0.5f, 0.5f});
    const PolygonEval e = s.prepare(40, 30);
    for (int y = 0; y < 30; ++y)
    {
      int x0, x1;
      CHECK(!e.rowSpan(y, 40, x0, x1));
      for (float v : fullRow(e, y, 40))
        CHECK(v == 0.0f);
    }
  }
}

static void testHardSquare()
{
  const PolygonShape s = square(0.25f, 0.25f, 0.75f, 0.75f);
  const int W = 200, H = 100;
  const PolygonEval e = s.prepare(W, H); // covers x 50..150, y 25..75
  const auto mid = fullRow(e, 50, W);
  CHECK(mid[100] == 1.0f && mid[60] == 1.0f && mid[140] == 1.0f);
  CHECK(mid[30] == 0.0f && mid[170] == 0.0f);
  CHECK(fullRow(e, 10, W)[100] == 0.0f && fullRow(e, 90, W)[100] == 0.0f);
  // The area is the polygon's: 100 x 50 px (anti-aliased edges keep it).
  CHECK_NEAR(totalWeight(e, W, H), 5000.0, 50.0);
  // A half pixel of coverage gives half weight (anti-aliasing).
  const PolygonShape half = square(0.2525f, 0.25f, 0.75f, 0.75f); // left edge at x = 50.5
  const auto row = fullRow(half.prepare(W, H), 50, W);
  CHECK_NEAR(row[50], 0.5, 0.02);
  CHECK(row[49] == 0.0f && row[51] == 1.0f);
  // Tight spans: a row through the square spans the square plus the padding only.
  int x0, x1;
  CHECK(e.rowSpan(50, W, x0, x1) && x0 >= 50 - 3 && x1 <= 150 + 3 && x1 - x0 >= 100);
  CHECK(!e.rowSpan(10, W, x0, x1) && !e.rowSpan(90, W, x0, x1));
  CHECK(e.rowSpan(26, W, x0, x1) && e.rowSpan(74, W, x0, x1));
  // Winding does not matter: the same square drawn the other way round.
  PolygonShape rev = s;
  std::reverse(rev.pts.begin(), rev.pts.end());
  CHECK(fullRow(rev.prepare(W, H), 50, W) == mid);
  // Where the outline starts does not matter either.
  PolygonShape rot = s;
  std::rotate(rot.pts.begin(), rot.pts.begin() + 2, rot.pts.end());
  CHECK(fullRow(rot.prepare(W, H), 50, W) == mid);
}

static void testShapes()
{
  const int W = 120, H = 120;
  // A triangle: the row's width follows the sides.
  PolygonShape tri;
  tri.pts = {{0.5f, 0.1f}, {0.9f, 0.9f}, {0.1f, 0.9f}};
  const PolygonEval et = tri.prepare(W, H);
  int a0, a1, b0, b1;
  CHECK(et.rowSpan(30, W, a0, a1) && et.rowSpan(100, W, b0, b1));
  CHECK((b1 - b0) > (a1 - a0) * 2);
  CHECK_NEAR(totalWeight(et, W, H), 0.5 * (0.8 * W) * (0.8 * H), 60.0); // base 96 x height 96 / 2
  // Even-odd: a bow tie leaves its crossing pinched, and an outline drawn
  // twice round (a star) has no hole through the middle because of winding.
  PolygonShape bow;
  bow.pts = {{0.1f, 0.1f}, {0.9f, 0.9f}, {0.9f, 0.1f}, {0.1f, 0.9f}};
  const PolygonEval eb = bow.prepare(W, H);
  CHECK(fullRow(eb, 60, W)[20] > 0.9f && fullRow(eb, 60, W)[100] > 0.9f); // left and right lobes
  CHECK(fullRow(eb, 12, W)[60] == 0.0f && fullRow(eb, 108, W)[60] == 0.0f); // above / below the pinch
  PolygonShape star; // a pentagram: the central pentagon is inside twice -> a hole (even-odd)
  for (int i = 0; i < 5; ++i)
  {
    const float a = -1.5707963f + static_cast<float>(i) * 2.0f * 6.2831853f / 5.0f;
    star.pts.push_back({0.5f + 0.4f * std::cos(a), 0.5f + 0.4f * std::sin(a)});
  }
  CHECK(fullRow(star.prepare(W, H), 60, W)[60] == 0.0f);
  // A concave "U": the notch is empty.
  PolygonShape u;
  u.pts = {{0.1f, 0.1f}, {0.3f, 0.1f}, {0.3f, 0.7f}, {0.7f, 0.7f}, {0.7f, 0.1f}, {0.9f, 0.1f}, {0.9f, 0.9f}, {0.1f, 0.9f}};
  const PolygonEval eu = u.prepare(W, H);
  CHECK(fullRow(eu, 30, W)[60] == 0.0f && fullRow(eu, 30, W)[20] == 1.0f && fullRow(eu, 100, W)[60] == 1.0f);
}

static void testFeather()
{
  const int W = 200, H = 200;
  const PolygonShape hard = square(0.25f, 0.25f, 0.75f, 0.75f, 0.0f);
  PolygonShape soft = hard;
  soft.feather = 0.5f;
  const PolygonEval eh = hard.prepare(W, H), es = soft.prepare(W, H);
  const auto rh = fullRow(eh, 100, W), rs = fullRow(es, 100, W);
  // Centered on the outline: about half weight at the edge, falling off smoothly both ways.
  CHECK_NEAR(rs[50], 0.5, 0.08);
  CHECK(rs[100] > 0.99f && rs[0] == 0.0f);
  float prev = 0.0f;
  for (int x = 20; x <= 100; ++x)
  {
    CHECK(rs[static_cast<size_t>(x)] >= prev - 0.01f); // rises into the shape
    prev = rs[static_cast<size_t>(x)];
  }
  CHECK(rs[40] > 0.0f && rh[40] == 0.0f); // reaches outside the hard outline
  CHECK(rs[52] < 1.0f && rh[52] == 1.0f); // and eats into it
  // The blur moves weight around, it does not create or lose it (the shape is well inside).
  CHECK_NEAR(totalWeight(es, W, H), totalWeight(eh, W, H), 100.0);
  // The span grows with the feather, exactly as far as the weights do.
  int h0, h1, s0, s1;
  CHECK(eh.rowSpan(100, W, h0, h1) && es.rowSpan(100, W, s0, s1));
  CHECK(s0 < h0 && s1 > h1);
  const long extra = checkSpanCoversAll(es, W, H, "soft square");
  CHECK(extra < static_cast<long>(H) * 8); // a few pixels of padding per row
  // More feather, wider reach.
  PolygonShape softer = hard;
  softer.feather = 1.0f;
  int w0, w1;
  CHECK(softer.prepare(W, H).rowSpan(100, W, w0, w1) && w0 < s0 && w1 > s1);
  // Feather beyond 0..1 is clamped, not an error.
  PolygonShape wild = hard;
  wild.feather = 7.0f;
  CHECK(wild.prepare(W, H).rowSpan(100, W, w0, w1));
  wild.feather = -3.0f;
  CHECK(fullRow(wild.prepare(W, H), 100, W) == rh);
}

static void testRunsOffThePicture()
{
  // A polygon bigger than the picture must not fade out at the picture's edge.
  PolygonShape big = square(-0.5f, -0.5f, 1.5f, 1.5f, 1.0f);
  const int W = 100, H = 80;
  const PolygonEval e = big.prepare(W, H);
  for (int y : {0, 40, 79})
  {
    const auto row = fullRow(e, y, W);
    CHECK(row[0] > 0.99f && row[W - 1] > 0.99f && row[50] > 0.99f);
  }
  // One that only half-covers: still ~half weight on the border pixel of its own outline.
  PolygonShape half = square(0.5f, -0.5f, 1.5f, 1.5f, 0.5f);
  const auto row = fullRow(half.prepare(W, H), 40, W);
  CHECK_NEAR(row[49] + row[50], 1.0, 0.05); // the two pixels beside the outline share one pixel of weight
  CHECK(row[W - 1] > 0.99f && row[0] == 0.0f);
  // Entirely off the picture: nothing.
  PolygonShape off = square(1.2f, 1.2f, 1.4f, 1.4f, 0.0f);
  int x0, x1;
  for (int y = 0; y < H; ++y)
    CHECK(!off.prepare(W, H).rowSpan(y, W, x0, x1));
}

static void testSpanIsExactForRandomPolygons()
{
  std::mt19937 rng(41);
  for (int i = 0; i < 40; ++i)
  {
    PolygonShape s;
    const int n = 3 + static_cast<int>(rng() % 8);
    for (int k = 0; k < n; ++k)
      s.pts.push_back({static_cast<float>(static_cast<int>(rng() % 140) - 20) / 100.0f,
                       static_cast<float>(static_cast<int>(rng() % 140) - 20) / 100.0f});
    s.feather = static_cast<float>(rng() % 101) / 100.0f;
    const int W = 30 + static_cast<int>(rng() % 90), H = 30 + static_cast<int>(rng() % 90);
    checkSpanCoversAll(s.prepare(W, H), W, H, "random polygon");
  }
  // A big picture: the map is smaller than the image and read back interpolated.
  PolygonShape s;
  s.pts = {{0.1f, 0.2f}, {0.8f, 0.1f}, {0.9f, 0.7f}, {0.4f, 0.9f}, {0.2f, 0.6f}};
  s.feather = 0.3f;
  const int W = 3000, H = 40; // wide: 3000 px -> map 2048
  const PolygonEval e = s.prepare(W, H);
  CHECK(e.mw == kPolyMapEdge && e.mh < H);
  checkSpanCoversAll(e, W, H, "downscaled map");
}

static void testFrame()
{
  PolygonShape s;
  s.pts = {{0.2f, 0.3f}, {0.7f, 0.2f}, {0.6f, 0.8f}};
  s.feather = 0.4f;
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  CHECK(s.toSource(id) == s);
  std::mt19937 rng(9);
  for (int i = 0; i < 100; ++i)
  {
    const Orient o{static_cast<int>(rng() % 4), (rng() & 1) != 0};
    const GeoFrame f = makeGeoFrame(400, 300, o, static_cast<float>(static_cast<int>(rng() % 4000) - 2000) / 100.0f,
                                    (rng() & 1) != 0, CropRect{});
    const PolygonShape view = s.toSource(f);
    CHECK(view.pts.size() == s.pts.size() && view.feather == s.feather);
    for (size_t k = 0; k < s.pts.size(); ++k)
    {
      const PolyPt back = PolygonShape::pointFromSource(view.pts[k], f);
      CHECK_NEAR(back.x, s.pts[k].x, 1e-4);
      CHECK_NEAR(back.y, s.pts[k].y, 1e-4);
    }
  }
  // A quarter turn clockwise: a point in the top-left ends up in the top-right.
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  PolygonShape tl;
  tl.pts = {{0.1f, 0.1f}, {0.2f, 0.1f}, {0.2f, 0.2f}};
  const PolygonShape moved = tl.toSource(rot);
  CHECK(moved.pts[0].x > 0.8f && moved.pts[0].y < 0.2f);
}

static void testApplySourceEdit()
{
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, true}, 7.0f, true, CropRect{});
  PolygonShape stored;
  stored.pts = {{0.2f, 0.3f}, {0.7f, 0.2f}, {0.6f, 0.8f}};
  const PolygonShape before = stored.toSource(f);
  // Only the feather: no vertex is touched (no float drift).
  PolygonShape after = before;
  after.feather = 0.9f;
  PolygonShape s2 = stored;
  s2.applySourceEdit(before, after, f);
  CHECK(s2.pts == stored.pts && s2.feather == 0.9f);
  // No change at all: equal (so no undo step).
  PolygonShape s3 = stored;
  s3.applySourceEdit(before, before, f);
  CHECK(s3 == stored);
  // One vertex moved: only that one changes, and it lands where the frame says.
  PolygonShape moved = before;
  moved.pts[1].x += 0.05f;
  PolygonShape s4 = stored;
  s4.applySourceEdit(before, moved, f);
  CHECK(s4.pts[0] == stored.pts[0] && s4.pts[2] == stored.pts[2] && !(s4.pts[1] == stored.pts[1]));
  CHECK_NEAR(s4.toSource(f).pts[1].x, moved.pts[1].x, 1e-4);
  // A vertex added: every point is mapped back, in order.
  PolygonShape grown = before;
  grown.pts.insert(grown.pts.begin() + 1, PolyPt{0.5f, 0.1f});
  PolygonShape s5 = stored;
  s5.applySourceEdit(before, grown, f);
  CHECK(s5.pts.size() == 4);
  const PolygonShape view5 = s5.toSource(f);
  for (size_t k = 0; k < 4; ++k)
  {
    CHECK_NEAR(view5.pts[k].x, grown.pts[k].x, 1e-4);
    CHECK_NEAR(view5.pts[k].y, grown.pts[k].y, 1e-4);
  }
  // A vertex removed.
  PolygonShape shrunk = before;
  shrunk.pts.erase(shrunk.pts.begin());
  PolygonShape s6 = stored;
  s6.applySourceEdit(before, shrunk, f);
  CHECK(s6.pts.size() == 2);
  // From empty: an edit that creates the first points.
  PolygonShape empty;
  const PolygonShape emptyView = empty.toSource(f);
  PolygonShape made = emptyView;
  made.pts.push_back(PolyPt{0.5f, 0.5f});
  PolygonShape s7 = empty;
  s7.applySourceEdit(emptyView, made, f);
  CHECK(s7.pts.size() == 1);
}

static void testCache()
{
  polygonCache().clear();
  CHECK(polygonCache().size() == 0);
  const PolygonShape a = square(0.2f, 0.2f, 0.6f, 0.6f, 0.3f);
  const PolygonEval e1 = a.prepare(80, 60);
  CHECK(polygonCache().size() == 1);
  const PolygonEval e2 = a.prepare(80, 60); // same shape, same size: the very same map back
  CHECK(polygonCache().size() == 1 && e1.map.get() == e2.map.get());
  const PolygonEval e3 = a.prepare(40, 30); // other size: another map
  CHECK(polygonCache().size() == 2 && e3.map.get() != e1.map.get());
  PolygonShape b = a;
  b.feather = 0.6f; // other feather: another map
  const PolygonEval e4 = b.prepare(80, 60);
  CHECK(polygonCache().size() == 3 && e4.map.get() != e1.map.get());
  // The cache is bounded, most recent first.
  for (int i = 0; i < 20; ++i)
    square(0.1f, 0.1f, 0.2f + 0.01f * static_cast<float>(i), 0.5f).prepare(30, 30);
  CHECK(polygonCache().size() == kPolyCacheSize);
  // Fewer than 3 points is cached too (and still selects nothing).
  PolygonShape none;
  none.prepare(10, 10);
  CHECK(polygonCache().size() <= kPolyCacheSize);
  polygonCache().clear();
  CHECK(polygonCache().size() == 0);
  // Evals are immutable: an old one stays valid after the cache is emptied.
  CHECK(fullRow(e1, 20, 80)[24] > 0.9f);
}

int main()
{
  testHelpers();
  testTooFewPoints();
  testHardSquare();
  testShapes();
  testFeather();
  testRunsOffThePicture();
  testSpanIsExactForRandomPolygons();
  testFrame();
  testApplySourceEdit();
  testCache();
  return finish();
}