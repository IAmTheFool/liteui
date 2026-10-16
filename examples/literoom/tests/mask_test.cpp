// Build: g++ -std=c++20 -I.. mask_test.cpp -o mask_test && ./mask_test
#include "mask.hpp"
#include "check.hpp"

#include <cstring>
#include <random>

using Px = std::vector<uint8_t>;

static Px grays(std::initializer_list<int> v)
{
  Px p;
  for (int g : v)
  {
    p.push_back(static_cast<uint8_t>(g));
    p.push_back(static_cast<uint8_t>(g));
    p.push_back(static_cast<uint8_t>(g));
    p.push_back(255);
  }
  return p;
}
static LuminosityShape lumRange(float lo, float hi, float feather = 0.0f)
{
  LuminosityShape s;
  s.lo = lo;
  s.hi = hi;
  s.feather = feather;
  return s;
}
static MaskComposite compose(std::vector<MaskPassPart> parts, int W = 8, int H = 8)
{
  return prepareComposite(parts, W, H);
}
static std::vector<float> weightsOf(const MaskComposite &c, const Px &pix)
{
  const int n = static_cast<int>(pix.size() / 4);
  std::vector<float> out(static_cast<size_t>(n)), tmp(static_cast<size_t>(n));
  c.rowWeights(0, 0, n, out.data(), pix.data(), tmp.data());
  return out;
}
static std::vector<float> weightsOfOne(const MaskShape &s, const Px &pix)
{
  const MaskEval e = prepareMask(s, 8, 8);
  std::vector<float> out(pix.size() / 4);
  maskRowWeights(e, 0, 0, static_cast<int>(out.size()), out.data(), pix.data());
  return out;
}

static void testKinds()
{
  CHECK(static_cast<int>(MaskType::Count) == 6);
  CHECK(std::variant_size_v<MaskShape> == 6 && std::variant_size_v<MaskEval> == 6);
  // The variant alternatives are in the same order as MaskType.
  // (MaskPart::type() is the variant's index; an Object part once reported itself
  // as Color Range and the other way round because the two orders differed.)
  const MaskShape shapes[] = {RadialShape{}, LinearShape{}, BrushShape{}, LuminosityShape{}, ColorRangeShape{}, ObjectShape{}};
  const MaskType types[] = {MaskType::Radial, MaskType::Linear, MaskType::Brush, MaskType::Luminosity, MaskType::ColorRange, MaskType::Object};
  for (int i = 0; i < 6; ++i)
  {
    CHECK(MaskPart{shapes[i], MaskOp::Add}.type() == types[i]);
    CHECK(static_cast<int>(types[i]) == i);
  }
  CHECK(std::strcmp(maskTypeName(MaskType::Radial), "Radial") == 0);
  CHECK(std::strcmp(maskTypeName(MaskType::ColorRange), "Color Range") == 0);
  CHECK(std::strcmp(maskTypeName(MaskType::Count), "Mask") == 0);
  CHECK(std::strcmp(maskOpName(MaskOp::Add), "Add") == 0 && std::strcmp(maskOpName(MaskOp::Subtract), "Subtract") == 0 &&
        std::strcmp(maskOpName(MaskOp::Intersect), "Intersect") == 0);
  for (int i = 0; i < 6; ++i)
    CHECK((types[i] == MaskType::Luminosity || types[i] == MaskType::ColorRange) == maskTypeUsesPixels(types[i]));
}

static void testDispatch()
{
  // prepareMask / maskRowSpan / maskRowWeights hand over to the shape's own eval.
  RadialShape r;
  r.cx = 0.4f;
  r.cy = 0.5f;
  r.rx = 0.3f;
  r.ry = 0.2f;
  r.angle = 20.0f;
  LinearShape l;
  l.angle = 55.0f;
  const int W = 40, H = 30;
  for (const MaskShape &shape : {MaskShape{r}, MaskShape{l}})
  {
    const MaskEval e = prepareMask(shape, W, H);
    for (int y = 0; y < H; y += 3)
    {
      int a0 = 0, a1 = 0, b0 = 0, b1 = 0;
      const bool ra = maskRowSpan(e, y, W, a0, a1);
      const bool rb = std::holds_alternative<RadialEval>(e) ? std::get<RadialEval>(e).rowSpan(y, W, b0, b1)
                                                            : std::get<LinearEval>(e).rowSpan(y, W, b0, b1);
      CHECK(ra == rb && (!ra || (a0 == b0 && a1 == b1)));
      std::vector<float> w1(W), w2(W);
      maskRowWeights(e, y, 0, W, w1.data(), nullptr);
      if (std::holds_alternative<RadialEval>(e))
        std::get<RadialEval>(e).rowWeights(y, 0, W, w2.data());
      else
        std::get<LinearEval>(e).rowWeights(y, 0, W, w2.data());
      CHECK(w1 == w2);
    }
  }
  // Identity frame: shapes come through the mapping unchanged (an Object gets a warped map).
  const GeoFrame id = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});
  for (const MaskShape &s : {MaskShape{r}, MaskShape{l}, MaskShape{BrushShape{}}, MaskShape{LuminosityShape{}}, MaskShape{ColorRangeShape{}}})
    CHECK(maskShapeToSource(s, id) == s);
  CHECK(maskShapeToSource(MaskShape{ObjectShape{}}, id).index() == static_cast<size_t>(MaskType::Object));
}

static void testComposeFolds()
{
  // Soft luminosity ranges give fractional weights; the composite must equal the
  // fold of its parts' own weights, for every operation and any order.
  const MaskShape a = lumRange(0.0f, 0.5f, 0.6f), b = lumRange(0.3f, 0.9f, 0.5f), c = lumRange(0.45f, 0.55f, 1.0f);
  Px pix;
  for (int g = 0; g <= 255; g += 5)
  {
    const Px one = grays({g});
    pix.insert(pix.end(), one.begin(), one.end());
  }
  const MaskOp ops[] = {MaskOp::Add, MaskOp::Subtract, MaskOp::Intersect};
  for (MaskOp o1 : ops)
    for (MaskOp o2 : ops)
    {
      const auto got = weightsOf(compose({{a, MaskOp::Add}, {b, o1}, {c, o2}}), pix);
      const auto wa = weightsOfOne(a, pix), wb = weightsOfOne(b, pix), wc = weightsOfOne(c, pix);
      for (size_t i = 0; i < got.size(); ++i)
      {
        float m = wa[i];
        const float t1 = wb[i], t2 = wc[i];
        m = o1 == MaskOp::Add ? m + t1 - m * t1 : o1 == MaskOp::Subtract ? m * (1 - t1) : m * t1;
        m = o2 == MaskOp::Add ? m + t2 - m * t2 : o2 == MaskOp::Subtract ? m * (1 - t2) : m * t2;
        CHECK_NEAR(got[i], m, 1e-5);
      }
    }
  // Hard 0 / 1 weights make the set algebra visible.
  const Px px = grays({50, 110, 200}); // in P only / in both / in Q only
  const MaskShape P = lumRange(0.0f, 0.5f), Q = lumRange(0.4f, 1.0f);
  auto w = [&](MaskOp op) { return weightsOf(compose({{P, MaskOp::Add}, {Q, op}}), px); };
  CHECK((w(MaskOp::Add) == std::vector<float>{1, 1, 1}));
  CHECK((w(MaskOp::Subtract) == std::vector<float>{1, 0, 0}));
  CHECK((w(MaskOp::Intersect) == std::vector<float>{0, 1, 0}));
  // The first part's own op is ignored: it starts the mask.
  CHECK((weightsOf(compose({{P, MaskOp::Subtract}}), px) == std::vector<float>{1, 1, 0}));
  // Nothing in it: nothing selected.
  CHECK((weightsOf(compose({}), px) == std::vector<float>{0, 0, 0}));
  int x0, x1;
  CHECK(!compose({}).rowSpan(0, 8, x0, x1));
  CHECK(compose({{P, MaskOp::Add}, {Q, MaskOp::Add}}).evals.size() == 2);
}

static RadialShape disc(float cx, float cy, float r)
{
  RadialShape s;
  s.cx = cx;
  s.cy = cy;
  s.rx = s.ry = r;
  s.feather = 0.3f;
  return s;
}

static void testSpans()
{
  const int W = 200, H = 100;
  const RadialShape left = disc(0.2f, 0.5f, 0.15f), right = disc(0.8f, 0.5f, 0.15f), mid = disc(0.5f, 0.5f, 0.15f);
  int x0, x1, l0, l1, r0, r1;
  const MaskEval el = prepareMask(left, W, H), er = prepareMask(right, W, H);
  CHECK(maskRowSpan(el, 50, W, l0, l1) && maskRowSpan(er, 50, W, r0, r1));
  // Add widens to the union, Subtract leaves the span alone.
  const auto add = compose({{left, MaskOp::Add}, {right, MaskOp::Add}}, W, H);
  CHECK(add.rowSpan(50, W, x0, x1) && x0 == l0 && x1 == r1);
  const auto sub = compose({{left, MaskOp::Add}, {right, MaskOp::Subtract}}, W, H);
  CHECK(sub.rowSpan(50, W, x0, x1) && x0 == l0 && x1 == l1);
  // Intersect narrows; disjoint parts leave nothing at all.
  const auto inter = compose({{left, MaskOp::Add}, {mid, MaskOp::Intersect}}, W, H);
  CHECK(!inter.rowSpan(50, W, x0, x1));
  const auto self = compose({{left, MaskOp::Add}, {left, MaskOp::Intersect}}, W, H);
  CHECK(self.rowSpan(50, W, x0, x1) && x0 == l0 && x1 == l1);
  // Parts that do not reach a row do not widen it.
  CHECK(!add.rowSpan(2, W, x0, x1));
  const auto lonely = compose({{left, MaskOp::Add}, {right, MaskOp::Add}}, W, H);
  CHECK(lonely.rowSpan(50, W, x0, x1));
  // A leading part that misses the row, then an Add that hits it.
  RadialShape top = disc(0.5f, 0.1f, 0.05f);
  const auto lateHit = compose({{top, MaskOp::Add}, {left, MaskOp::Add}}, W, H);
  CHECK(lateHit.rowSpan(50, W, x0, x1) && x0 == l0 && x1 == l1);
}

static void testSpansCoverEverythingForRandomComposites()
{
  std::mt19937 rng(31);
  const MaskOp ops[] = {MaskOp::Add, MaskOp::Subtract, MaskOp::Intersect};
  for (int trial = 0; trial < 80; ++trial)
  {
    std::vector<MaskPassPart> parts;
    const int n = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < n; ++i)
    {
      MaskShape s;
      switch (rng() % 3)
      {
      case 0:
        s = disc(static_cast<float>(rng() % 100) / 100.0f, static_cast<float>(rng() % 100) / 100.0f, 0.05f + static_cast<float>(rng() % 30) / 100.0f);
        break;
      case 1:
      {
        LinearShape l;
        l.cx = static_cast<float>(rng() % 100) / 100.0f;
        l.cy = static_cast<float>(rng() % 100) / 100.0f;
        l.angle = static_cast<float>(static_cast<int>(rng() % 360) - 180);
        l.len = 0.05f + static_cast<float>(rng() % 40) / 100.0f;
        s = l;
        break;
      }
      default:
      {
        BrushShape b;
        BrushStroke st;
        st.size = 0.05f + static_cast<float>(rng() % 15) / 100.0f;
        st.pts = {BrushPt{static_cast<float>(rng() % 100) / 100.0f, static_cast<float>(rng() % 100) / 100.0f},
                  BrushPt{static_cast<float>(rng() % 100) / 100.0f, static_cast<float>(rng() % 100) / 100.0f}};
        b.strokes = {st};
        s = b;
        break;
      }
      }
      parts.push_back({s, ops[rng() % 3]});
    }
    const int W = 50 + static_cast<int>(rng() % 50), H = 30 + static_cast<int>(rng() % 40);
    const MaskComposite c = prepareComposite(parts, W, H);
    for (int y = 0; y < H; ++y)
    {
      std::vector<float> w(static_cast<size_t>(W)), tmp(static_cast<size_t>(W));
      c.rowWeights(y, 0, W, w.data(), nullptr, tmp.data());
      int x0 = 0, x1 = 0;
      const bool any = c.rowSpan(y, W, x0, x1);
      for (int x = 0; x < W; ++x)
        if (w[static_cast<size_t>(x)] > 0.0f)
        {
          CHECK(any && x >= x0 && x < x1);
          if (!(any && x >= x0 && x < x1))
            goto next;
        }
    }
  next:;
  }
}

static void testMaskAndList()
{
  const Mask m;
  CHECK(m.parts.empty() && m.name.empty() && m.enabled && !m.invert && m.amount == 100.0f);
  CHECK(m.type() == MaskType::Radial); // no parts: Radial
  for (float v : m.adj)
    CHECK(v == 0.0f);
  for (float v : m.mix)
    CHECK(v == 0.0f);
  CHECK(!m.grade.anyActive());
  CHECK(m.curves.size() == kCurveChannelCount);
  for (const CurvePoints &c : m.curves)
    CHECK(c == identityCurve());
  Mask a;
  a.parts.push_back(MaskPart{BrushShape{}, MaskOp::Add});
  a.parts.push_back(MaskPart{RadialShape{}, MaskOp::Subtract});
  CHECK(a.type() == MaskType::Brush); // the first part's kind
  Mask b = a;
  CHECK(a == b);
  b.invert = true;
  CHECK(!(a == b));
  b = a;
  b.adj[static_cast<size_t>(AdjId::Exposure)] = 1.0f;
  CHECK(!(a == b));
  b = a;
  b.parts[1].op = MaskOp::Add;
  CHECK(!(a == b));
  b = a;
  b.curves[2] = CurvePoints{{0, 0}, {100, 140}, {255, 255}};
  CHECK(!(a == b));
  const MaskSet set{a, b};
  CHECK(set.size() == 2);
  CHECK(kMaxMasks >= 1 && kMaxMaskParts >= 2);
}

static void testNames()
{
  CHECK(sanitizeMaskName("Sky") == "Sky");
  CHECK(sanitizeMaskName("a b  c") == "a b  c");
  CHECK(sanitizeMaskName("t\xC3\xA9st\n\t\x7F") == "tst"); // only printable ASCII survives
  const std::string longName(100, 'x');
  CHECK(sanitizeMaskName(longName).size() == kMaskNameMax && kMaskNameMax == 32);
  CHECK(sanitizeMaskName("").empty());
  CHECK(maskNameBlank("") && maskNameBlank("   ") && !maskNameBlank(" a ") && !maskNameBlank("\t"));
}

static void testClearCaches()
{
  BrushShape b;
  BrushStroke st;
  st.pts = {BrushPt{0.5f, 0.5f}};
  b.strokes = {st};
  b.prepare(30, 30);
  ObjectShape o;
  auto map = std::make_shared<ObjectMap>();
  map->w = map->h = 4;
  map->a.assign(16, 255);
  map->id = nextObjectMapId();
  o.map = map;
  o.prepare(10, 10);
  const GeoFrame f = makeGeoFrame(40, 40, Orient{}, 0.0f, false, CropRect{});
  warpObjectMap(map, f);
  CHECK(brushCache().size() > 0 && objectEvalCache().size() > 0 && objectWarpCache().size() > 0);
  clearMaskCaches();
  CHECK(brushCache().size() == 0 && objectEvalCache().size() == 0 && objectWarpCache().size() == 0);
}

int main()
{
  testKinds();
  testDispatch();
  testComposeFolds();
  testSpans();
  testSpansCoverEverythingForRandomComposites();
  testMaskAndList();
  testNames();
  testClearCaches();
  return finish();
}
