// Build: g++ -std=c++20 -I.. mask_range_test.cpp -o mask_range_test && ./mask_range_test
#include "mask/mask_range.hpp"
#include "check.hpp"

#include <vector>

using Px = std::vector<uint8_t>;

static std::vector<float> weights(const LuminosityEval &e, const Px &rgba)
{
  std::vector<float> w(rgba.size() / 4);
  e.rowWeights(0, 0, static_cast<int>(w.size()), w.data(), rgba.data());
  return w;
}
static std::vector<float> weights(const ColorRangeEval &e, const Px &rgba)
{
  std::vector<float> w(rgba.size() / 4);
  e.rowWeights(0, 0, static_cast<int>(w.size()), w.data(), rgba.data());
  return w;
}
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

static void testLuminosityHardRange()
{
  LuminosityShape s;
  s.lo = 0.2f;
  s.hi = 0.6f;
  s.feather = 0.0f;
  const LuminosityEval e = s.prepare(10, 10);
  for (int i = 0; i < 256; ++i)
  {
    const float t = static_cast<float>(i) / 255.0f;
    if (t > 0.21f && t < 0.59f)
      CHECK(e.lut[static_cast<size_t>(i)] == 1.0f);
    if (t < 0.19f || t > 0.61f)
      CHECK(e.lut[static_cast<size_t>(i)] == 0.0f);
  }
  // The ends are swapped if given the wrong way round.
  LuminosityShape r = s;
  r.lo = 0.6f;
  r.hi = 0.2f;
  CHECK(r.prepare(1, 1).lut == e.lut);
}

static void testLuminositySoftEdges()
{
  LuminosityShape s;
  s.lo = 0.3f;
  s.hi = 0.7f;
  s.feather = 1.0f; // widest: kLumMaxRamp (0.5) of the luminance range
  const LuminosityEval e = s.prepare(1, 1);
  const float f = kLumMaxRamp;
  for (int i = 0; i < 256; ++i)
  {
    const float t = static_cast<float>(i) / 255.0f, w = e.lut[static_cast<size_t>(i)];
    CHECK(w >= 0.0f && w <= 1.0f);
    if (t >= 0.3f && t <= 0.7f)
      CHECK_NEAR(w, 1.0f, 1e-5); // full weight inside the range
    if (t <= 0.3f - f || t >= 0.7f + f)
      CHECK_NEAR(w, 0.0f, 1e-5); // none beyond the soft edge
  }
  // The soft edge rises monotonically into the range and falls monotonically out of it.
  for (int i = 1; i <= 76; ++i) // up to 0.3
    CHECK(e.lut[static_cast<size_t>(i)] >= e.lut[static_cast<size_t>(i - 1)] - 1e-6f);
  for (int i = 179; i < 256; ++i) // from 0.7
    CHECK(e.lut[static_cast<size_t>(i)] <= e.lut[static_cast<size_t>(i - 1)] + 1e-6f);
  // Less feather means a narrower edge.
  LuminosityShape narrow = s;
  narrow.feather = 0.3f;
  CHECK(narrow.prepare(1, 1).lut[60] < e.lut[60]); // t = 0.235, just outside the range
  // Out-of-range settings are clamped, not trusted.
  LuminosityShape wild;
  wild.lo = -3.0f;
  wild.hi = 9.0f;
  wild.feather = 7.0f;
  for (float w : wild.prepare(1, 1).lut)
    CHECK(w >= 0.0f && w <= 1.0f);
}

static void testLuminosityEdits()
{
  LuminosityShape s;
  s.lo = 0.2f;
  s.hi = 0.5f;
  s.setLo(0.8f); // pushes Max up
  CHECK(s.lo == 0.8f && s.hi == 0.8f);
  s.setHi(0.1f); // pushes Min down
  CHECK(s.hi == 0.1f && s.lo == 0.1f);
  s.setLo(-1.0f);
  s.setHi(2.0f);
  CHECK(s.lo == 0.0f && s.hi == 1.0f);
  // Not spatial: the frame changes nothing, an edit replaces the whole shape.
  const GeoFrame f = makeGeoFrame(400, 300, Orient{1, true}, 9.0f, true, CropRect{});
  CHECK(s.toSource(f) == s);
  LuminosityShape before = s, after = s, stored = s;
  after.hi = 0.4f;
  stored.applySourceEdit(before, after, f);
  CHECK(stored == after);
}

static void testLuminosityRowWeights()
{
  LuminosityShape s;
  s.lo = 0.0f;
  s.hi = 1.0f;
  s.feather = 0.0f;
  const LuminosityEval all = s.prepare(1, 1);
  float w[3] = {9, 9, 9};
  int x0, x1;
  CHECK(all.rowSpan(5, 7, x0, x1) && x0 == 0 && x1 == 7);
  CHECK(!all.rowSpan(0, 0, x0, x1));
  for (float v : weights(all, grays({0, 100, 255})))
    CHECK(v == 1.0f); // the full range selects every level
  // Without pixels it cannot select anything.
  all.rowWeights(0, 0, 3, w, nullptr);
  CHECK(w[0] == 0 && w[2] == 0);
  // The weight comes from the pixel's Rec. 709 luma in whole numbers (54 + 183 + 19 = 256):
  // use a tent-shaped table so every level has its own value.
  LuminosityEval probe;
  for (size_t i = 0; i < 256; ++i)
    probe.lut[i] = static_cast<float>(i);
  const Px px = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255, 77, 77, 77, 255, 0, 0, 0, 255};
  float out[6];
  probe.rowWeights(0, 0, 6, out, px.data());
  CHECK(out[0] == 54 && out[1] == 182 && out[2] == 19);
  CHECK(out[3] == 255 && out[4] == 77 && out[5] == 0);
  // x0 only shifts where the results are written, not which pixels are read.
  float part[2];
  probe.rowWeights(0, 3, 5, part, px.data() + 3 * 4);
  CHECK(part[0] == 255 && part[1] == 77);
}

static void testColorRange()
{
  ColorRangeShape unset;
  const ColorRangeEval none = unset.prepare(1, 1);
  int x0, x1;
  CHECK(!none.rowSpan(0, 10, x0, x1));
  const Px any = {10, 200, 30, 255, 200, 10, 30, 255};
  for (float w : weights(none, any))
    CHECK(w == 0.0f);

  ColorRangeShape s;
  s.set = true;
  s.r = 30.0f / 255;
  s.g = 200.0f / 255;
  s.b = 60.0f / 255;
  s.range = 0.30f;
  s.feather = 0.5f;
  const ColorRangeEval e = s.prepare(1, 1);
  CHECK(e.rowSpan(3, 9, x0, x1) && x0 == 0 && x1 == 9);
  const Px px = {30, 200, 60, 255,   // the sample itself
                 36, 196, 66, 255,   // a close green
                 255, 0, 0, 255,     // red
                 40, 70, 210, 255,   // blue
                 128, 128, 128, 255, // gray
                 0, 0, 0, 255};
  const auto w = weights(e, px);
  CHECK(w[0] == 1.0f);
  CHECK(w[1] > 0.9f);
  CHECK(w[2] == 0.0f && w[3] == 0.0f && w[4] == 0.0f && w[5] == 0.0f);
  for (float v : w)
    CHECK(v >= 0.0f && v <= 1.0f);
  // Without pixels there is nothing to compare.
  float o[1] = {7};
  e.rowWeights(0, 0, 1, o, nullptr);
  CHECK(o[0] == 0.0f);
}

static void testColorRangeShape()
{
  // Brightness differences count for less than color differences (kColorRangeLumWeight).
  ColorRangeShape s;
  s.set = true;
  s.r = s.g = s.b = 128.0f / 255;
  s.range = 0.12f;
  s.feather = 0.0f; // hard: weight is 1 while the distance is within `outer`
  const ColorRangeEval e = s.prepare(1, 1);
  const float outer = 0.12f * kColorRangeMaxDist;
  // A gray that is brighter by delta is at distance kColorRangeLumWeight * delta / 255.
  const int maxGrayDelta = static_cast<int>(outer / kColorRangeLumWeight * 255.0f); // still inside
  const auto inside = weights(e, grays({128 + maxGrayDelta - 3, 128 - (maxGrayDelta - 3)}));
  CHECK(inside[0] == 1.0f && inside[1] == 1.0f);
  const auto outside = weights(e, grays({128 + maxGrayDelta + 4, 128 - (maxGrayDelta + 4)}));
  CHECK(outside[0] == 0.0f && outside[1] == 0.0f);
  // The same distance in a color channel is worth more: a tint just as "far" as
  // the brightness step above is already outside.
  const Px tinted = {static_cast<uint8_t>(128 + maxGrayDelta - 3), 128, 128, 255};
  CHECK(weights(e, tinted)[0] < 1.0f);

  // More range means at least as much weight; more feather lowers weight inside the edge.
  ColorRangeShape wide = s, soft = s;
  wide.range = 0.5f;
  soft.feather = 1.0f;
  const Px probe = grays({150});
  CHECK(weights(wide.prepare(1, 1), probe)[0] >= weights(e, probe)[0]);
  const Px edge = grays({128 + maxGrayDelta - 3});
  CHECK(weights(soft.prepare(1, 1), edge)[0] <= weights(e, edge)[0]);
  // Falling away from the sample, the weight never rises again.
  float prev = 1.0f;
  ColorRangeShape m = s;
  m.feather = 1.0f;
  m.range = 0.6f;
  const ColorRangeEval me = m.prepare(1, 1);
  for (int g = 128; g < 256; ++g)
  {
    const float w = weights(me, grays({g}))[0];
    CHECK(w <= prev + 1e-6f);
    prev = w;
  }
  // Not spatial.
  const GeoFrame f = makeGeoFrame(400, 300, Orient{3, false}, -4.0f, true, CropRect{});
  CHECK(s.toSource(f) == s);
}


static float smooth01(float x)
{
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

static void testWrittenOutNumbers()
{
  CHECK(kLumMaxRamp == 0.5f && kColorRangeMaxDist == 0.75f && kColorRangeLumWeight == 0.35f);
  // Luminosity: 1 inside [a, b], smoothstep over `feather * 0.5` outside each end.
  for (float feather : {0.0f, 0.2f, 0.6f, 1.0f})
    for (auto range : {std::pair<float, float>{0.3f, 0.7f}, std::pair<float, float>{0.0f, 0.4f}, std::pair<float, float>{0.55f, 1.0f}})
    {
      LuminosityShape s;
      s.lo = range.first;
      s.hi = range.second;
      s.feather = feather;
      const LuminosityEval e = s.prepare(1, 1);
      const float f = feather * 0.5f;
      for (int i = 0; i < 256; ++i)
      {
        const float t = static_cast<float>(i) / 255.0f;
        float expect;
        if (f <= 1e-4f)
          expect = (t >= range.first && t <= range.second) ? 1.0f : 0.0f;
        else
          expect = smooth01((t - (range.first - f)) / f) * (1.0f - smooth01((t - range.second) / f));
        CHECK_NEAR(e.lut[static_cast<size_t>(i)], expect, 1e-4);
      }
    }
  // Color Range on grays: distance = 0.35 * |dY|, outer = range * 0.75, inner = outer * (1 - feather).
  for (float feather : {0.0f, 0.5f, 1.0f})
  {
    ColorRangeShape s;
    s.set = true;
    s.r = s.g = s.b = 128.0f / 255;
    s.range = 0.4f;
    s.feather = feather;
    const ColorRangeEval e = s.prepare(1, 1);
    for (int g = 128; g < 256; g += 3)
    {
      const float d = 0.35f * static_cast<float>(g - 128) / 255.0f;
      const float outer = 0.4f * 0.75f, inner = outer * (1.0f - feather);
      const float expect = d <= inner ? 1.0f : (d >= outer ? 0.0f : 1.0f - smooth01((d - inner) / (outer - inner)));
      CHECK_NEAR(weights(e, grays({g}))[0], expect, 2e-3);
    }
  }
}

int main()
{
  testWrittenOutNumbers();

  testLuminosityHardRange();
  testLuminositySoftEdges();
  testLuminosityEdits();
  testLuminosityRowWeights();
  testColorRange();
  testColorRangeShape();
  return finish();
}
