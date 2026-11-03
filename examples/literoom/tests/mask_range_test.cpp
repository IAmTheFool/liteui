// mask_range_test.cpp - Luminosity range and Color Range masks (mask/mask_range.hpp), header only.
// Covers both the byte path (rowWeights) and the float-level path (rowWeightsF) that the 16-bit
// pipeline uses.
// Build: g++ -std=c++20 -I.. mask_range_test.cpp -o mask_range_test && ./mask_range_test
#include "check.hpp"
#include "mask/mask_range.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <utility>
#include <vector>

using Px = std::vector<uint8_t>;

// ---------------------------------------------------------------- helpers

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
static std::vector<float> weightsF(const LuminosityEval &e, const std::vector<float> &px)
{
  const int n = static_cast<int>(px.size() / 4);
  std::vector<float> w(static_cast<size_t>(n));
  e.rowWeightsF(0, 0, n, w.data(), px.data());
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
static float smooth01(float x)
{
  x = std::clamp(x, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

// ---------------------------------------------------------------- Luminosity (byte path)

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

// ---------------------------------------------------------------- Luminosity (float path)

static void testFineTable()
{
  LuminosityShape s;
  s.lo = 0.2f;
  s.hi = 0.6f;
  s.feather = 0.4f;
  const LuminosityEval e = s.prepare(1, 1);
  CHECK(e.fine && e.fine->size() == static_cast<size_t>(kLumFineSize));
  CHECK(kLumFineSize == 255 * kLumSub + 1);
  // The fine table agrees with the byte table at every whole level.
  for (int k = 0; k < 256; ++k)
    CHECK(e.lut[static_cast<size_t>(k)] == (*e.fine)[static_cast<size_t>(k * kLumSub)]);
  // A default-constructed eval has no fine table.
  CHECK(!LuminosityEval{}.fine);
}

static void testWholeLevelGraysMatchTheByteVersionExactly()
{
  for (const float feather : {0.0f, 0.2f, 0.4f, 1.0f})
  {
    LuminosityShape s;
    s.lo = 0.2f;
    s.hi = 0.6f;
    s.feather = feather;
    const LuminosityEval e = s.prepare(1, 1);
    Px pb(256 * 4);
    std::vector<float> pf(256 * 4);
    for (int k = 0; k < 256; ++k)
      for (int c = 0; c < 4; ++c)
      {
        const size_t i = static_cast<size_t>(k * 4 + c);
        pb[i] = static_cast<uint8_t>(c == 3 ? 255 : k);
        pf[i] = static_cast<float>(c == 3 ? 255 : k);
      }
    std::vector<float> wb(256);
    e.rowWeights(0, 0, 256, wb.data(), pb.data());
    const std::vector<float> wf = weightsF(e, pf);
    for (int k = 0; k < 256; ++k)
      CHECK(wb[static_cast<size_t>(k)] == wf[static_cast<size_t>(k)]);
  }
}

static void testLumaIsNotRoundedToAByte()
{
  // A soft mask: between two whole levels the weight lies between the two level weights (it is
  // interpolated), where the byte version would give one of the two.
  LuminosityShape s;
  s.lo = 0.0f;
  s.hi = 0.3f;
  s.feather = 1.0f;
  const LuminosityEval e = s.prepare(1, 1);
  const float lv = 120.0f;
  const std::vector<float> px = {lv + 0.5f, lv + 0.5f, lv + 0.5f, 255.0f};
  const float w = weightsF(e, px)[0];
  const float w0 = e.lut[120], w1 = e.lut[121];
  CHECK(w >= std::min(w0, w1) - 1e-6f && w <= std::max(w0, w1) + 1e-6f);
  CHECK(w0 != w1); // the test only means something where the table is not flat
  // A hard edge is placed by the real luma: lo = 0.2 is 51 levels. Luma 50.9 is outside, 51.0 inside
  // (a byte version would round 50.9 up to 51 and select it).
  LuminosityShape hard;
  hard.lo = 0.2f;
  hard.hi = 1.0f;
  hard.feather = 0.0f;
  const LuminosityEval eh = hard.prepare(1, 1);
  CHECK(weightsF(eh, {50.9f, 50.9f, 50.9f, 255.0f})[0] == 0.0f);
  CHECK(weightsF(eh, {51.0f, 51.0f, 51.0f, 255.0f})[0] == 1.0f);
}

static void testSixteenBitLevelsGiveTheByteResult()
{
  // A 16-bit sample k * 257 is exactly k levels (u16ToLevels), so a picture that started as 8-bit
  // selects the same pixels through the float path as it did through the byte path.
  LuminosityShape s;
  s.lo = 0.1f;
  s.hi = 0.5f;
  s.feather = 0.6f;
  const LuminosityEval e = s.prepare(1, 1);
  Px pb(256 * 4);
  std::vector<float> pf(256 * 4);
  for (int k = 0; k < 256; ++k)
    for (int c = 0; c < 4; ++c)
    {
      const size_t i = static_cast<size_t>(k * 4 + c);
      const int v = c == 3 ? 255 : k;
      pb[i] = static_cast<uint8_t>(v);
      pf[i] = u16ToLevels(static_cast<uint16_t>(v * 257));
      CHECK(pf[i] == static_cast<float>(v));
    }
  CHECK(weights(e, pb) == weightsF(e, pf));
}

static void testNullAndOddInputs()
{
  const LuminosityEval e = LuminosityShape{}.prepare(1, 1);
  float out = 9.0f;
  e.rowWeightsF(0, 0, 1, &out, nullptr);
  CHECK(out == 0.0f);
  const LuminosityEval blank; // never prepared: no table
  const float px[4] = {10.0f, 10.0f, 10.0f, 255.0f};
  out = 9.0f;
  blank.rowWeightsF(0, 0, 1, &out, px);
  CHECK(out == 0.0f);
  const float odd[2][4] = {{std::nanf(""), 0, 0, 255}, {1e9f, 1e9f, 1e9f, 255}};
  for (const auto &p : odd)
  {
    e.rowWeightsF(0, 0, 1, &out, p);
    CHECK(out >= 0.0f && out <= 1.0f);
  }
  // A span that starts at x0 > 0 is indexed from the pointer it is given, like the byte version.
  const std::vector<float> two = {0, 0, 0, 255, 255, 255, 255, 255};
  float w2[2];
  e.rowWeightsF(0, 5, 7, w2, two.data());
  CHECK(w2[0] == e.lut[0] && w2[1] == e.lut[255]);
}

// ---------------------------------------------------------------- Color Range

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

static void testColorRangeFloatEqualsByte()
{
  ColorRangeShape c;
  c.set = true;
  c.r = 0.8f;
  c.g = 0.3f;
  c.b = 0.2f;
  c.range = 0.4f;
  c.feather = 0.5f;
  const ColorRangeEval e = c.prepare(1, 1);
  std::mt19937 g(7);
  const int n = 3000;
  Px pb(static_cast<size_t>(n) * 4);
  std::vector<float> pf(pb.size());
  for (size_t i = 0; i < pb.size(); ++i)
    pf[i] = static_cast<float>(pb[i] = static_cast<uint8_t>(g()));
  std::vector<float> a(static_cast<size_t>(n)), b(static_cast<size_t>(n));
  e.rowWeights(0, 0, n, a.data(), pb.data());
  e.rowWeightsF(0, 0, n, b.data(), pf.data());
  CHECK(a == b); // byte-valued input: the very same arithmetic
  // Fractional levels are used as they are.
  std::vector<float> half = pf;
  for (float &v : half)
    v = std::min(255.0f, v + 0.5f);
  std::vector<float> c2(static_cast<size_t>(n));
  e.rowWeightsF(0, 0, n, c2.data(), half.data());
  int differ = 0;
  for (int i = 0; i < n; ++i)
    differ += (c2[static_cast<size_t>(i)] != b[static_cast<size_t>(i)]);
  CHECK(differ > 0);
  // Not set / null.
  const ColorRangeEval unset = ColorRangeShape{}.prepare(1, 1);
  float out = 9.0f;
  unset.rowWeightsF(0, 0, 1, &out, pf.data());
  CHECK(out == 0.0f);
  e.rowWeightsF(0, 0, 1, &out, nullptr);
  CHECK(out == 0.0f);
}

// ---------------------------------------------------------------- The written-out numbers

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

  // Luminosity, byte path
  testLuminosityHardRange();
  testLuminositySoftEdges();
  testLuminosityEdits();
  testLuminosityRowWeights();

  // Luminosity, float path
  testFineTable();
  testWholeLevelGraysMatchTheByteVersionExactly();
  testLumaIsNotRoundedToAByte();
  testSixteenBitLevelsGiveTheByteResult();
  testNullAndOddInputs();

  // Color Range
  testColorRange();
  testColorRangeShape();
  testColorRangeFloatEqualsByte();
  return finish();
}