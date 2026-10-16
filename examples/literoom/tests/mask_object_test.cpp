// Build: g++ -std=c++20 -I.. mask_object_test.cpp -o mask_object_test && ./mask_object_test
#include "mask_object.hpp"
#include "check.hpp"

#include <random>

using Bytes = std::vector<uint8_t>;

static std::shared_ptr<ObjectMap> blockMap(int w, int h, int bx0, int by0, int bx1, int by1)
{
  auto m = std::make_shared<ObjectMap>();
  m->w = w;
  m->h = h;
  m->id = nextObjectMapId();
  m->a.assign(static_cast<size_t>(w) * h, 0);
  for (int y = by0; y < by1; ++y)
    for (int x = bx0; x < bx1; ++x)
      m->a[static_cast<size_t>(y) * w + x] = 255;
  return m;
}
static int at(const ObjectMap &m, int x, int y) { return m.a[static_cast<size_t>(y) * m.w + x]; }
static long total(const Bytes &v)
{
  long s = 0;
  for (uint8_t b : v)
    s += b;
  return s;
}

// ------------------------------------------------------------ naive references
static Bytes refMorph(const Bytes &in, int w, int h, int r, bool dilate, bool horizontal)
{
  Bytes out(in.size());
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      int v = dilate ? 0 : 255;
      for (int k = -r; k <= r; ++k)
      {
        const int xx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
        const int yy = horizontal ? y : std::clamp(y + k, 0, h - 1);
        // the real code only looks inside the picture (no clamped repeats), same result for min / max
        if ((horizontal ? x + k : y + k) < 0 || (horizontal ? x + k : y + k) >= (horizontal ? w : h))
          continue;
        const int p = in[static_cast<size_t>(yy) * w + xx];
        v = dilate ? std::max(v, p) : std::min(v, p);
      }
      out[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(v);
    }
  return out;
}
static Bytes refBlur(const Bytes &in, int w, int h, int r, bool horizontal)
{
  Bytes out(in.size());
  const int cnt = 2 * r + 1;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      int sum = 0;
      for (int k = -r; k <= r; ++k)
      {
        const int xx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
        const int yy = horizontal ? y : std::clamp(y + k, 0, h - 1);
        sum += in[static_cast<size_t>(yy) * w + xx];
      }
      out[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>((sum + cnt / 2) / cnt);
    }
  return out;
}

static void testIdsAndHash()
{
  const uint64_t a = nextObjectMapId(), b = nextObjectMapId(), c = nextObjectMapId();
  CHECK(a > 0 && b == a + 1 && c == b + 1);
  CHECK(hashMix(5, 1.5) == hashMix(5, 1.5));
  CHECK(hashMix(5, 1.5) != hashMix(5, 2.5));
  CHECK(hashMix(5, 1.5) != hashMix(6, 1.5));
  CHECK(hashMix(hashMix(1, 2.0), 3.0) != hashMix(hashMix(1, 3.0), 2.0)); // order matters
  CHECK(hashMix(0, 0.0) != hashMix(0, -0.0)); // compares bit patterns
}

static void testMorphologyAndBlurMatchReferences()
{
  std::mt19937 rng(17);
  for (int trial = 0; trial < 60; ++trial)
  {
    const int w = 1 + static_cast<int>(rng() % 23), h = 1 + static_cast<int>(rng() % 19);
    Bytes img(static_cast<size_t>(w) * h);
    for (uint8_t &v : img)
      v = (rng() % 3 == 0) ? static_cast<uint8_t>(rng() % 256) : 0;
    const int r = static_cast<int>(rng() % 5);
    for (bool horizontal : {true, false})
    {
      Bytes out;
      for (bool dilate : {true, false})
      {
        objAxisMorph(img, out, w, h, r, dilate, horizontal);
        CHECK(out == refMorph(img, w, h, r, dilate, horizontal));
      }
      objAxisBlur(img, out, w, h, r, horizontal);
      CHECK(out == refBlur(img, w, h, r, horizontal));
    }
  }
  // Known values: a 1-D line.
  const Bytes line = {0, 0, 255, 0, 0, 0, 255, 255, 0};
  Bytes out;
  objAxisMorph(line, out, 9, 1, 1, true, true);
  CHECK((out == Bytes{0, 255, 255, 255, 0, 255, 255, 255, 255}));
  objAxisMorph(line, out, 9, 1, 1, false, true);
  CHECK((out == Bytes{0, 0, 0, 0, 0, 0, 0, 0, 0}));
  // A flat map is unchanged by a blur, whatever the radius.
  const Bytes flat(40, 99);
  objAxisBlur(flat, out, 8, 5, 3, true);
  CHECK(out == flat);
}

static void testWarp()
{
  objectWarpCache().clear();
  auto src = blockMap(64, 48, 16, 12, 48, 36); // the middle half of the picture
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  const auto w = warpObjectMap(src, id);
  CHECK(w->w == 400 && w->h == 300); // the picture's size (up to kObjectMapEdge)
  CHECK(at(*w, 200, 150) == 255 && at(*w, 5, 5) == 0 && at(*w, 395, 295) == 0);
  CHECK(at(*w, 120, 100) == 255 && at(*w, 100, 60) == 0); // block edges at 0.25 / 0.75 of the picture
  CHECK(w->id != src->id);

  // A quarter turn clockwise moves the top-left corner to the top-right.
  auto corner = blockMap(64, 48, 0, 0, 16, 12);
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  const auto r = warpObjectMap(corner, rot);
  CHECK(r->w == 300 && r->h == 400);
  CHECK(at(*r, 280, 10) == 255);            // top-right
  CHECK(at(*r, 10, 10) == 0 && at(*r, 10, 390) == 0 && at(*r, 280, 390) == 0);

  // Straightening with expand leaves empty corners that are outside the original: zero.
  auto full = blockMap(64, 48, 0, 0, 64, 48);
  const GeoFrame str = makeGeoFrame(400, 300, Orient{}, 30.0f, true, CropRect{});
  const auto s = warpObjectMap(full, str);
  CHECK(at(*s, 0, 0) == 0 && at(*s, s->w - 1, s->h - 1) == 0);
  CHECK(at(*s, s->w / 2, s->h / 2) == 255);

  // Big pictures get a map no bigger than kObjectMapEdge on the long side.
  const GeoFrame huge = makeGeoFrame(6000, 4000, Orient{}, 0.0f, false, CropRect{});
  const auto h = warpObjectMap(src, huge);
  CHECK(h->w == kObjectMapEdge && h->h == 683);
}

static void testWarpCache()
{
  objectWarpCache().clear();
  auto src = blockMap(32, 32, 8, 8, 24, 24);
  const GeoFrame f1 = makeGeoFrame(100, 100, Orient{}, 0.0f, false, CropRect{});
  const GeoFrame f2 = makeGeoFrame(100, 100, Orient{2, false}, 0.0f, false, CropRect{});
  const auto a = warpObjectMap(src, f1);
  CHECK(warpObjectMap(src, f1) == a && objectWarpCache().size() == 1); // same map, same frame: a hit
  const auto b = warpObjectMap(src, f2);
  CHECK(b != a && b->id != a->id && objectWarpCache().size() == 2); // the frame is part of the id
  CHECK(warpObjectMap(src, makeGeoFrame(100, 100, Orient{}, 0.0f, false, CropRect{}))->id == a->id);
  auto other = blockMap(32, 32, 8, 8, 24, 24); // a different segmentation result
  CHECK(warpObjectMap(other, f1)->id != a->id);
  // At most kObjectCacheSize entries are kept.
  for (int i = 0; i < static_cast<int>(kObjectCacheSize) + 4; ++i)
    warpObjectMap(blockMap(8, 8, 1, 1, 5, 5), f1);
  CHECK(objectWarpCache().size() == kObjectCacheSize);
  objectWarpCache().clear();
  CHECK(objectWarpCache().size() == 0);
}

static ObjectShape shapeOf(const std::shared_ptr<const ObjectMap> &m, float grow = 0, float feather = 0)
{
  ObjectShape s;
  s.map = m;
  s.grow = grow;
  s.feather = feather;
  return s;
}

static void testShapeEquality()
{
  auto m = blockMap(10, 10, 2, 2, 6, 6);
  ObjectShape a = shapeOf(m, 0.3f, 0.2f);
  CHECK(a == shapeOf(m, 0.3f, 0.2f));
  CHECK(!(a == shapeOf(m, 0.4f, 0.2f)) && !(a == shapeOf(m, 0.3f, 0.1f)));
  CHECK(!(a == shapeOf(blockMap(10, 10, 2, 2, 6, 6), 0.3f, 0.2f))); // another result, even if identical pixels
  ObjectShape p = a;
  p.prompts.push_back(ObjectPrompt{0.5f, 0.5f, true});
  CHECK(!(p == a));
  CHECK(ObjectShape{} == ObjectShape{} && !(ObjectShape{} == a));
}

static void testGrowShrinkFeather()
{
  objectEvalCache().clear();
  auto m = blockMap(100, 100, 30, 30, 70, 70); // 40 x 40
  auto areaOf = [&](float grow, float feather)
  {
    return total(*shapeOf(m, grow, feather).prepare(100, 100).map) / 255.0;
  };
  const double base = areaOf(0, 0);
  CHECK_NEAR(base, 1600.0, 0.5);
  // grow +1 = 3% of the long side (3 px) on each edge; -1 shrinks the same amount.
  CHECK_NEAR(areaOf(1.0f, 0), 46.0 * 46.0, 0.5);
  CHECK_NEAR(areaOf(-1.0f, 0), 34.0 * 34.0, 0.5);
  CHECK(areaOf(0.5f, 0) > base && areaOf(-0.5f, 0) < base);
  // Feather blurs: soft edge values appear, the peak stays, the total is kept.
  const ObjectEval f = shapeOf(m, 0, 1.0f).prepare(100, 100);
  int soft = 0, peak = 0;
  for (uint8_t v : *f.map)
  {
    soft += v > 0 && v < 255;
    peak = std::max<int>(peak, v);
  }
  CHECK(soft > 100 && peak == 255);
  CHECK_NEAR(total(*f.map) / 255.0, base, base * 0.02);
  // Spans: every row's [lo, hi) is exactly the first / last non-zero column.
  for (int y = 0; y < f.mh; ++y)
  {
    int first = -1, last = -1;
    for (int x = 0; x < f.mw; ++x)
      if ((*f.map)[static_cast<size_t>(y) * f.mw + x])
      {
        if (first < 0)
          first = x;
        last = x;
      }
    if (first < 0)
      CHECK((*f.lo)[static_cast<size_t>(y)] >= (*f.hi)[static_cast<size_t>(y)]);
    else
      CHECK((*f.lo)[static_cast<size_t>(y)] == first && (*f.hi)[static_cast<size_t>(y)] == last + 1);
  }
}

static void testEvalCaching()
{
  objectEvalCache().clear();
  auto m = blockMap(60, 40, 10, 10, 30, 30);
  const ObjectShape s = shapeOf(m, 0.4f, 0.3f);
  const ObjectEval a = s.prepare(60, 40);
  CHECK(a.W == 60 && a.H == 40 && objectEvalCache().size() == 1);
  // Another image size shares the result (the map is resolved at its own size).
  const ObjectEval b = s.prepare(600, 400);
  CHECK(b.W == 600 && b.H == 400 && b.map == a.map && objectEvalCache().size() == 1);
  // Another grow / feather / map is another entry.
  shapeOf(m, 0.5f, 0.3f).prepare(60, 40);
  shapeOf(m, 0.4f, 0.0f).prepare(60, 40);
  shapeOf(blockMap(60, 40, 10, 10, 30, 30), 0.4f, 0.3f).prepare(60, 40);
  CHECK(objectEvalCache().size() == 4);
  for (int i = 0; i < static_cast<int>(kObjectCacheSize) + 3; ++i)
    shapeOf(blockMap(8, 8, 1, 1, 4, 4)).prepare(8, 8);
  CHECK(objectEvalCache().size() == kObjectCacheSize);
  // No map: an empty eval that selects nothing.
  const ObjectEval none = ObjectShape{}.prepare(50, 50);
  int x0, x1;
  CHECK(!none.map && !none.rowSpan(10, 50, x0, x1));
  objectEvalCache().clear();
}

static void testEvalSpansCoverEverything()
{
  objectEvalCache().clear();
  std::mt19937 rng(2);
  for (int trial = 0; trial < 20; ++trial)
  {
    auto m = blockMap(50, 40, static_cast<int>(rng() % 20), static_cast<int>(rng() % 15), 25 + static_cast<int>(rng() % 25), 20 + static_cast<int>(rng() % 20));
    const ObjectEval e = shapeOf(m, static_cast<float>(rng() % 201) / 100.0f - 1.0f, static_cast<float>(rng() % 101) / 100.0f)
                             .prepare(100, 80);
    for (int y = 0; y < 80; ++y)
    {
      std::vector<float> w(100);
      e.rowWeights(y, 0, 100, w.data());
      int x0 = 0, x1 = 0;
      const bool any = e.rowSpan(y, 100, x0, x1);
      for (int x = 0; x < 100; ++x)
        if (w[static_cast<size_t>(x)] > 0.0f)
          CHECK(any && x >= x0 && x < x1);
    }
  }
}

static void testFrameAndEdit()
{
  objectWarpCache().clear();
  auto m = blockMap(40, 30, 5, 5, 20, 20);
  ObjectShape s = shapeOf(m, 0.2f, 0.1f);
  s.prompts = {ObjectPrompt{0.1f, 0.2f, true}, ObjectPrompt{0.7f, 0.9f, false}};
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  const ObjectShape same = s.toSource(id);
  CHECK(same.prompts == s.prompts && same.feather == s.feather && same.grow == s.grow);
  CHECK(same.map && same.map->id != m->id && same.map->w == 400); // the warped map
  // Prompts follow the frame (a quarter turn: top-left -> top-right).
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  const ObjectShape v = s.toSource(rot);
  CHECK(v.prompts[0].x > 0.7f && v.prompts[0].y < 0.2f && v.prompts[0].fg && !v.prompts[1].fg);
  // A shape without a map has nothing to warp.
  CHECK(!ObjectShape{}.toSource(rot).map);
  // Only the sliders are edited through the view; the bitmap and prompts stay.
  ObjectShape stored = s, after = s;
  after.feather = 0.9f;
  after.grow = -0.5f;
  after.prompts.clear();
  stored.applySourceEdit(s, after, id);
  CHECK(stored.feather == 0.9f && stored.grow == -0.5f && stored.prompts == s.prompts && stored.map == m);
}


static void testPrepareMatchesTheNaiveSteps()
{
  // The recipe, written out with the naive helpers and the design numbers:
  // grow/shrink by round(|grow| * 3% of the long side) (square, separable), then
  // feather: two (horizontal + vertical) box blurs of round(feather * 2% of the long side).
  std::mt19937 rng(5);
  for (int trial = 0; trial < 12; ++trial)
  {
    objectEvalCache().clear();
    const int w = 90 + static_cast<int>(rng() % 60), h = 60 + static_cast<int>(rng() % 50);
    auto m = std::make_shared<ObjectMap>();
    m->w = w;
    m->h = h;
    m->id = nextObjectMapId();
    m->a.assign(static_cast<size_t>(w) * h, 0);
    for (int b = 0; b < 4; ++b)
    {
      const int bx = static_cast<int>(rng() % static_cast<unsigned>(w - 12)), by = static_cast<int>(rng() % static_cast<unsigned>(h - 12));
      for (int y = by; y < by + 5 + static_cast<int>(rng() % 8); ++y)
        for (int x = bx; x < bx + 5 + static_cast<int>(rng() % 8) && x < w; ++x)
          if (y < h)
            m->a[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(100 + rng() % 156);
    }
    const float grow = static_cast<float>(static_cast<int>(rng() % 201) - 100) / 100.0f;
    const float feather = static_cast<float>(rng() % 101) / 100.0f;
    const int longSide = std::max(w, h);
    const int rg = static_cast<int>(std::lround(std::fabs(grow) * 0.03 * longSide));
    const int rf = static_cast<int>(std::lround(feather * 0.02 * longSide));
    Bytes a = m->a, t;
    if (rg > 0)
    {
      t = refMorph(a, w, h, rg, grow > 0, true);
      a = refMorph(t, w, h, rg, grow > 0, false);
    }
    if (rf > 0)
      for (int pass = 0; pass < 2; ++pass)
      {
        t = refBlur(a, w, h, rf, true);
        a = refBlur(t, w, h, rf, false);
      }
    CHECK(*shapeOf(m, grow, feather).prepare(w, h).map == a);
  }
  CHECK(kObjectMapEdge == 1024 && kObjectMaxGrow == 0.03f && kObjectMaxFeather == 0.02f);
}

static void testWarpInterpolationRounding()
{
  objectWarpCache().clear();
  auto m = std::make_shared<ObjectMap>();
  m->w = 2;
  m->h = 1;
  m->id = nextObjectMapId();
  m->a = {0, 255};
  const GeoFrame f = makeGeoFrame(8, 4, Orient{}, 0.0f, false, CropRect{});
  const auto w = warpObjectMap(m, f);
  CHECK(w->w == 8 && w->h == 4);
  // Bilinear on a 0 -> 255 step, rounded to nearest (63.75 -> 64, not 63), edges clamped.
  const Bytes expect = {0, 0, 32, 96, 159, 223, 255, 255};
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x)
      CHECK(at(*w, x, y) == expect[static_cast<size_t>(x)]);
}

int main()
{
  testPrepareMatchesTheNaiveSteps();
  testWarpInterpolationRounding();

  testIdsAndHash();
  testMorphologyAndBlurMatchReferences();
  testWarp();
  testWarpCache();
  testShapeEquality();
  testGrowShrinkFeather();
  testEvalCaching();
  testEvalSpansCoverEverything();
  testFrameAndEdit();
  return finish();
}
