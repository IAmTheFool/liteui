// cases/tone_table.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace tone_table_cases
// so they cannot collide with other cases.
//
// tone_table_test.cpp - everything core/tone_table.hpp does. Header only, no UI.
//  * the float tone table against the byte table it replaces (toneLutFromValues): same sliders,
//    same curves, same order; the only intended difference is that the sliders are no longer
//    rounded to a byte one after the other;
//  * the HEAD of a table: the curve sampled at every 16-bit sample over the first kToneHeadLevels
//    levels, so a steep start (Gamma +100) is not interpolated across a 1/16-level cell.

#include "core/tone_table.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <utility>
#include <vector>

namespace tone_table_cases
{
static AdjValues only(AdjId id, float v)
{
  AdjValues a{};
  a[adjIndex(id)] = v;
  return a;
}
static bool isTone(const AdjustmentSpec &s) { return s.stage == AdjustmentSpec::Stage::Tone; }

TEST_CASE("tone_table: identity", "[tone_table]")
{
  const ToneTable t = toneTableFromValues(AdjValues{}, nullptr, nullptr);
  CHECK(t.isIdentity());
  for (int x = 0; x < 256; ++x)
    CHECK(t.at(static_cast<float>(x)) == static_cast<float>(x));
  CHECK((t.at(-5.0f) == 0.0f && t.at(300.0f) == 255.0f && t.at(std::nanf("")) == 0.0f));
  CHECK(t.at(100.37f) == within(100.37f, 1e-4));
  // Non-tone sliders never touch it, at any value.
  AdjValues others{};
  for (const auto &s : kAdjustmentSpecs)
    if (!isTone(s))
      others[adjIndex(s.id)] = s.max;
  CHECK(toneTableFromValues(others, nullptr, nullptr).isIdentity());
  // Curves that can't be evaluated count as the identity, like curveToLut.
  const CurvePoints bad{{0, 0}};
  CHECK(toneTableFromValues(AdjValues{}, &bad, &bad).isIdentity());
}

// One slider: rounding the float result once is EXACTLY the old byte table.
TEST_CASE("tone_table: singleSliderIsExact", "[tone_table]")
{
  int checked = 0;
  for (const auto &s : kAdjustmentSpecs)
  {
    if (!isTone(s))
      continue;
    for (float v : {s.min, s.max, s.min * 0.37f, s.max * 0.37f, s.max * 0.9f, s.min * 0.05f, s.max * 0.05f, 1.0f})
    {
      if (v < s.min || v > s.max)
        continue;
      const AdjValues a = only(s.id, v);
      const auto old = toneLutFromValues(a, nullptr);
      const ToneTable t = toneTableFromValues(a, nullptr, nullptr);
      for (int x = 0; x < 256; ++x)
      {
        CHECK(toU8Clamped(t.at(static_cast<float>(x))) == old[static_cast<size_t>(x)]);
        ++checked;
      }
    }
  }
  CHECK(checked > 3000);
}

// A curve alone, or a channel curve alone: the sample lies on the grid, so it is exact too.
TEST_CASE("tone_table: curvesAloneAreExact", "[tone_table]")
{
  const CurvePoints m{{0, 0}, {64, 40}, {128, 150}, {200, 230}, {255, 255}};
  const CurvePoints c{{0, 20}, {255, 235}};
  const auto lm = curveToLut(m), lc = curveToLut(c);
  const ToneTable tm = toneTableFromValues(AdjValues{}, &m, nullptr);
  const ToneTable tc = toneTableFromValues(AdjValues{}, nullptr, &c);
  for (int x = 0; x < 256; ++x)
  {
    CHECK(toU8Clamped(tm.at(static_cast<float>(x))) == toneLutFromValues(AdjValues{}, &lm)[static_cast<size_t>(x)]);
    CHECK(tm.at(static_cast<float>(x)) == lm[static_cast<size_t>(x)]);
    CHECK(tc.at(static_cast<float>(x)) == lc[static_cast<size_t>(x)]);
  }
}

// Several sliders and curves: close to the old table, and the table never runs backwards.
// Gamma is left out of the closeness check on purpose: its slope is nearly vertical at black
// (255 * (x / 255)^0.25 jumps from 0 to 64 between level 0 and level 1), so the half level the
// byte table used to round away between two sliders becomes tens of levels after it. That is
// the precision the float table is there to keep; testFloatChain covers Gamma by hand.
TEST_CASE("tone_table: combinationsStayClose", "[tone_table]")
{
  std::mt19937 g(2024);
  auto rnd = [&](float lo, float hi)
  { return lo + (hi - lo) * static_cast<float>(g() & 0xFFFF) / 65535.0f; };
  const CurvePoints m{{0, 0}, {64, 50}, {200, 220}, {255, 255}};
  const CurvePoints c{{0, 10}, {255, 245}};
  int worst = 0;
  for (int round = 0; round < 300; ++round)
  {
    AdjValues a{};
    for (const auto &s : kAdjustmentSpecs)
      if (isTone(s) && s.id != AdjId::Gamma && (g() % 3 == 0))
        a[adjIndex(s.id)] = rnd(s.min, s.max);
    const bool useM = g() & 1, useC = g() & 1;
    std::array<float, 256> ml = curveToLut(m);
    const auto old = toneLutFromValues(a, useM ? &ml : nullptr);
    const ToneTable t = toneTableFromValues(a, useM ? &m : nullptr, nullptr);
    float prev = -1.0f;
    for (int i = 0; i < kToneSize; ++i)
    {
      CHECK(t.y[static_cast<size_t>(i)] >= prev);
      prev = t.y[static_cast<size_t>(i)];
    }
    for (int x = 0; x < 256; ++x)
    {
      const int d = std::abs(static_cast<int>(toU8Clamped(t.at(static_cast<float>(x)))) - static_cast<int>(old[static_cast<size_t>(x)]));
      worst = std::max(worst, d);
    }
    if (useC) // the channel curve rides on top of the master one, monotonic as well
    {
      const ToneTable tc = toneTableFromValues(a, useM ? &m : nullptr, &c);
      float p2 = -1.0f;
      for (int i = 0; i < kToneSize; ++i)
      {
        CHECK(tc.y[static_cast<size_t>(i)] >= p2);
        p2 = tc.y[static_cast<size_t>(i)];
      }
      // c is a straight line from (0, 10) to (255, 245), so the channel stage is exactly
      // y = 10 + x * 235 / 255 applied to what the table without the channel curve gives.
      for (int x = 0; x < 256; x += 5)
        CHECK(tc.at(static_cast<float>(x)) == within(10.0f + t.at(static_cast<float>(x)) * (235.0f / 255.0f), 1e-2));
    }
  }
  std::printf("  (largest gap to the byte table over random combinations: %d level(s))\n", worst);
  CHECK(worst <= 6);
}

// The interpolation between grid points, on a function that is linear there.
TEST_CASE("tone_table: interpolation", "[tone_table]")
{
  const ToneTable t = toneTableFromValues(only(AdjId::Brightness, 20.0f), nullptr, nullptr);
  for (float x : {0.0f, 0.03f, 1.5f, 77.77f, 200.1f, 234.9f})
    CHECK(t.at(x) == within(x + 20.0f, 1e-3));
  CHECK(t.at(240.0f) == 255.0f); // clamped at the top, like the byte table
  // Exposure: a 16-bit value between two levels lands between the two byte results.
  const ToneTable e = toneTableFromValues(only(AdjId::Exposure, 50.0f), nullptr, nullptr);
  CHECK(e.at(10.5f) == within(15.75f, 1e-3));
  CHECK(e.at(255.0f / 257.0f) == within(255.0f / 257.0f * 1.5f, 1e-3));
}

// What the new table changes on purpose: no byte rounding between sliders. Exposure +33 then
// Contrast +57: the old table rounds to a byte in between, the float table does not.
TEST_CASE("tone_table: noRoundingBetweenSliders", "[tone_table]")
{
  AdjValues a{};
  a[adjIndex(AdjId::Exposure)] = 33.0f; // x * 1.33
  a[adjIndex(AdjId::Contrast)] = 57.0f; // 128 + (x - 128) * 1.57
  const auto old = toneLutFromValues(a, nullptr);
  const ToneTable t = toneTableFromValues(a, nullptr, nullptr);
  int differs = 0, exactFloat = 0;
  for (int x = 0; x < 256; ++x)
  {
    const float e = std::min(255.0f, std::max(0.0f, x * 1.33f));
    const float want = std::min(255.0f, std::max(0.0f, 128.0f + (e - 128.0f) * 1.57f));
    exactFloat += std::fabs(t.at(static_cast<float>(x)) - want) < 1e-3f;
    differs += toU8Clamped(t.at(static_cast<float>(x))) != old[static_cast<size_t>(x)];
  }
  CHECK(exactFloat == 256); // the float chain is what the table holds
  CHECK(differs > 0);       // and it really is different from the rounded chain
}

// The table is the sliders' functions chained in table order, each clamped to 0..255 and none
// rounded, Gamma included (written out here independently of the builder).
TEST_CASE("tone_table: floatChain", "[tone_table]")
{
  std::mt19937 g(99);
  auto rnd = [&](float lo, float hi)
  { return lo + (hi - lo) * static_cast<float>(g() & 0xFFFF) / 65535.0f; };
  for (int round = 0; round < 200; ++round)
  {
    AdjValues a{};
    for (const auto &s : kAdjustmentSpecs)
      if (isTone(s) && (g() % 2 == 0))
        a[adjIndex(s.id)] = rnd(s.min, s.max);
    const ToneTable t = toneTableFromValues(a, nullptr, nullptr);
    for (int x = 0; x < 256; x += 3)
    {
      float v = static_cast<float>(x);
      for (const auto &s : kAdjustmentSpecs)
        if (isTone(s) && a[adjIndex(s.id)] != 0.0f)
          v = std::clamp(s.apply(v, a[adjIndex(s.id)]), 0.0f, 255.0f);
      CHECK(t.at(static_cast<float>(x)) == v);
    }
  }
}

// The three channels together: one shared table unless a channel has its own curve, and a curve that
// rounds to the straight line counts as none (the byte pipeline's rule), so it never changes a pixel.
TEST_CASE("tone_table: toneSet", "[tone_table]")
{
  const CurveSet id = identityCurves();
  const auto a = makeToneSet(AdjValues{}, id);
  CHECK((a->identity && a->shared && a->ch[0].isIdentity()));
  CurveSet c = id;
  c[0] = CurvePoints{{0, 0}, {100, 140}, {255, 255}};
  const auto b = makeToneSet(AdjValues{}, c);
  CHECK((!b->identity && b->shared && &b->at(0) == &b->at(2) && &b->at(0) == &b->ch[0]));
  const ToneTable direct = toneTableFromValues(AdjValues{}, &c[0], nullptr);
  CHECK(b->ch[0].y == direct.y);
  c[2] = CurvePoints{{0, 30}, {255, 200}};
  const auto d = makeToneSet(only(AdjId::Exposure, 20.0f), c);
  CHECK((!d->identity && !d->shared));
  // curves[2] is the GREEN curve, i.e. channel 1: R (0) and B (2) have only the master curve.
  CHECK((d->at(0).y == d->at(2).y && d->at(0).y != d->at(1).y));
  CHECK(d->at(1).y == toneTableFromValues(only(AdjId::Exposure, 20.0f), &c[0], &c[2]).y);
  CHECK(d->at(0).y == toneTableFromValues(only(AdjId::Exposure, 20.0f), &c[0], nullptr).y);
  // A curve a hair off the straight line still rounds to the identity: not used.
  CurveSet nearSet = id;
  nearSet[1] = CurvePoints{{0, 0.2f}, {255, 254.8f}};
  const auto n = makeToneSet(AdjValues{}, nearSet);
  CHECK((n->identity && n->shared));
  // Sliders only: identical to the plain table, shared across channels.
  const auto s1 = makeToneSet(only(AdjId::Contrast, 40.0f), id);
  CHECK((s1->shared && s1->ch[0].y == toneTableFromValues(only(AdjId::Contrast, 40.0f), nullptr, nullptr).y));
  // Sliders that cancel to nothing at every level (a zero value) leave the stage skippable.
  CHECK(makeToneSet(only(AdjId::Exposure, 0.0f), id)->identity);
}

TEST_CASE("tone_table: sanitizing", "[tone_table]")
{
  AdjValues a{};
  a[adjIndex(AdjId::Exposure)] = std::nanf("");
  a[adjIndex(AdjId::Contrast)] = 5000.0f; // clamped into the slider range
  AdjValues b{};
  b[adjIndex(AdjId::Contrast)] = 100.0f;
  const ToneTable ta = toneTableFromValues(a, nullptr, nullptr), tb = toneTableFromValues(b, nullptr, nullptr);
  CHECK(ta.y == tb.y); // the main grid and the head
  // Not-a-number is "untouched" for every Tone slider.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (const auto &s : kAdjustmentSpecs)
    if (isTone(s))
      CHECK(toneTableFromValues(only(s.id, nan), nullptr, nullptr).isIdentity());
}

// ============================================================== the head
static const AdjustmentSpec &specOf(AdjId id) { return kAdjustmentSpecs[adjIndex(id)]; }

// The slider alone, as the table is meant to compute it (clamped like the table does).
static float reference(AdjId id, float v, float x) { return toneClamp(specOf(id).apply(x, v)); }

// Largest |table - reference| over EVERY 16-bit sample (x = sample / 257 levels).
static double maxErrOverAllSamples(AdjId id, float v)
{
  const auto ts = makeToneSet(only(id, v), identityCurves());
  const ToneTable &t = ts->at(0);
  double worst = 0;
  for (int s = 0; s < 65536; ++s)
  {
    const float x = u16ToLevels(static_cast<uint16_t>(s));
    worst = std::max(worst, std::fabs(static_cast<double>(t.at(x)) - static_cast<double>(reference(id, v, x))));
  }
  return worst;
}

TEST_CASE("tone_table: layout", "[tone_table]")
{
  static_assert(kToneTableSize == kToneSize + kToneHeadSize);
  static_assert(kToneHeadCells == kToneHeadLevels * 257);
  ToneTable t;
  CHECK(t.y.size() == static_cast<size_t>(kToneTableSize));
}

TEST_CASE("tone_table: identityAndSkip", "[tone_table]")
{
  const auto ts = makeToneSet(AdjValues{}, identityCurves());
  CHECK((ts->identity && ts->shared && ts->ch[0].isIdentity()));
  // An identity table gives its input back (to float precision) at every 16-bit sample.
  for (int s = 0; s < 65536; s += 7)
  {
    const float x = u16ToLevels(static_cast<uint16_t>(s));
    CHECK(std::fabs(ts->at(0).at(x) - x) < 1e-4f);
  }
  // Any slider that does something is not the identity (head included).
  CHECK(!makeToneSet(only(AdjId::Gamma, 100.0f), identityCurves())->identity);
}

TEST_CASE("tone_table: gammaLiftHasNoSteepStartError", "[tone_table]")
{
  const double oneSixteenBitStep = 1.0 / 257.0;
  // Before the head, Gamma +100 was off by up to 15 levels in the first cell and 0.4 in the next.
  CHECK(maxErrOverAllSamples(AdjId::Gamma, 100.0f) < oneSixteenBitStep);
  CHECK(maxErrOverAllSamples(AdjId::Gamma, 50.0f) < oneSixteenBitStep);
  CHECK(maxErrOverAllSamples(AdjId::Gamma, -100.0f) < oneSixteenBitStep);
  // Spot check the old failure: sample 1 (x = 1/257) of Gamma +100 is about 16 levels, not ~1.
  const auto ts = makeToneSet(only(AdjId::Gamma, 100.0f), identityCurves());
  const float x1 = u16ToLevels(1);
  CHECK(std::fabs(ts->at(0).at(x1) - reference(AdjId::Gamma, 100.0f, x1)) < 1e-3f);
  CHECK(ts->at(0).at(x1) > 15.0f);
  // The plain 1/16 grid alone still has the old error: the test would be vacuous if it did not.
  CHECK(std::fabs(toneLookup(ts->at(0).y.data(), u16ToLevels(8)) - reference(AdjId::Gamma, 100.0f, u16ToLevels(8))) > 1.0f);
}

TEST_CASE("tone_table: otherSlidersStayWithinTheirDocumentedError", "[tone_table]")
{
  // Blacks -100 has the steepest smoothstep band: about 0.03 level, a smooth deviation (no banding).
  CHECK(maxErrOverAllSamples(AdjId::Blacks, -100.0f) < 0.035);
  CHECK(maxErrOverAllSamples(AdjId::Shadows, 100.0f) < 0.01);
}

// The head with curves: the master and channel curves are applied inside the head as well, so the
// steep start of a slider (Gamma +100) followed by a curve with a steep start of its own must come out
// right at every 16-bit sample, not just at integer levels.
TEST_CASE("tone_table: headWithCurves", "[tone_table]")
{
  const CurvePoints master{{0, 0}, {10, 60}, {128, 150}, {255, 255}}; // steep start
  const CurvePoints chan{{0, 10}, {255, 245}};                         // a straight line
  CurveSet cs = identityCurves();
  cs[0] = master;
  cs[2] = chan; // the green channel
  // An independent fine sampling of the master curve: one entry per 16-bit sample.
  std::vector<float> fine(255 * 257 + 1);
  curveSample(master, fine.data(), static_cast<int>(fine.size()), 1.0f / kToneHeadPerLevel);
  auto masterAt = [&](float x)
  {
    const float p = toneClamp(x) * kToneHeadPerLevel;
    const size_t i = std::min(static_cast<size_t>(p), fine.size() - 2);
    return fine[i] + (fine[i + 1] - fine[i]) * (p - static_cast<float>(i));
  };
  for (const float gamma : {0.0f, 100.0f, 40.0f})
  {
    const auto ts = makeToneSet(only(AdjId::Gamma, gamma), cs);
    CHECK((!ts->shared && !ts->identity));
    const ToneTable &r = ts->at(0); // master curve only
    const ToneTable &g = ts->at(1); // master, then the green curve
    CHECK((&ts->at(0) == &ts->ch[0] && &ts->at(1) == &ts->ch[1]));
    float prevR = -1.0f, prevG = -1.0f;
    double worstR = 0, worstG = 0;
    for (int s = 0; s < 65536; ++s)
    {
      const float x = u16ToLevels(static_cast<uint16_t>(s));
      const float yr = r.at(x), yg = g.at(x);
      CHECK((yr >= prevR - 1e-4f && yg >= prevG - 1e-4f)); // never runs backwards
      prevR = yr;
      prevG = yg;
      CHECK((yr >= 0.0f && yr <= 255.0f && yg >= 0.0f && yg <= 255.0f));
      const float afterSlider = reference(AdjId::Gamma, gamma, x);
      const float wantR = masterAt(afterSlider);
      const float wantG = 10.0f + wantR * (235.0f / 255.0f);
      worstR = std::max(worstR, static_cast<double>(std::fabs(yr - wantR)));
      worstG = std::max(worstG, static_cast<double>(std::fabs(yg - wantG)));
    }
    CHECK((worstR < 0.02 && worstG < 0.02)); // a small fraction of an 8-bit level, in the head and out of it
  }
}

TEST_CASE("tone_table: eightBitLevelsAreExact", "[tone_table]")
{
  // An integer level is a main-grid point (or level 0 / 1, head points): exactly the slider's value,
  // and the same as the plain grid lookup, so an 8-bit picture is untouched by the head.
  for (const auto &c : {std::pair<AdjId, float>{AdjId::Gamma, 100.0f}, {AdjId::Gamma, -60.0f}, {AdjId::Blacks, -100.0f}})
  {
    const auto ts = makeToneSet(only(c.first, c.second), identityCurves());
    for (int k = 0; k < 256; ++k)
    {
      const float x = static_cast<float>(k);
      CHECK(ts->at(0).at(x) == reference(c.first, c.second, x));
      CHECK(ts->at(0).at(x) == toneLookup(ts->at(0).y.data(), x));
    }
  }
}

TEST_CASE("tone_table: edgesAreSafe", "[tone_table]")
{
  const auto ts = makeToneSet(only(AdjId::Gamma, 100.0f), identityCurves());
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  // Around the head / grid boundary (kToneHeadLevels) and the cell count. (The clamp on the head's cell
  // index is defensive: for every float below kToneHeadLevels the product stays below the cell count.)
  const float two = static_cast<float>(kToneHeadLevels);
  const float probes[] = {0.0f, 1e-9f, 0.99999994f, 1.0f, std::nextafter(two, 0.0f), two, std::nextafter(two, 3.0f),
                          254.99998f, 255.0f, 300.0f, -5.0f, nan, inf, -inf};
  for (const float x : probes)
  {
    const float y = ts->at(0).at(x);
    CHECK((y >= 0.0f && y <= 255.0f)); // finite, in range (NaN -> 0 like toneClamp)
  }
  CHECK(ts->at(0).at(nan) == ts->at(0).at(0.0f));
  // Continuous across the head / grid boundary: no jump at kToneHeadLevels.
  CHECK(std::fabs(ts->at(0).at(std::nextafter(two, 0.0f)) - ts->at(0).at(two)) < 1e-3f);
}
} // namespace tone_table_cases
