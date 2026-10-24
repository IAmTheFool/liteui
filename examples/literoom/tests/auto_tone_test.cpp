// auto_tone_test.cpp - Auto Tone on hand-made luma histograms: the status for
// empty and flat input, the direction of each slider, the result run back
// through the real tone table (the end points land near their targets, nothing
// is clipped that was not already, the table stays monotonic), that stray
// specks and the total pixel count do not change the answer, and a sweep over
// many ranges for the invariants. Needs no UI.
#include <array>
#include <cmath>
#include <cstdint>

#include "check.hpp"
#include "core/auto_tone.hpp"

using Hist = std::array<uint32_t, 256>;

// `perLevel` pixels at every level in [lo, hi].
static Hist uniform(int lo, int hi, uint32_t perLevel = 100)
{
  Hist h{};
  for (int i = lo; i <= hi; ++i)
    h[static_cast<size_t>(i)] = perLevel;
  return h;
}

// The four solved values run through the real tone table.
static std::array<uint8_t, 256> lutOf(const AutoTone &a)
{
  AdjValues v = defaultAdjValues();
  v[adjIndex(AdjId::Exposure)] = a.exposure;
  v[adjIndex(AdjId::Contrast)] = a.contrast;
  v[adjIndex(AdjId::Whites)] = a.whites;
  v[adjIndex(AdjId::Blacks)] = a.blacks;
  return toneLutFromValues(v, nullptr);
}

static bool monotone(const std::array<uint8_t, 256> &lut)
{
  for (size_t i = 0; i + 1 < lut.size(); ++i)
    if (lut[i] > lut[i + 1])
      return false;
  return true;
}

static bool allZero(const AutoTone &a)
{
  return a.exposure == 0.0f && a.contrast == 0.0f && a.whites == 0.0f &&
         a.blacks == 0.0f;
}

static bool inRangeAndWhole(const AutoTone &a)
{
  const float vals[4] = {a.exposure, a.contrast, a.whites, a.blacks};
  const AdjId ids[4] = {AdjId::Exposure, AdjId::Contrast, AdjId::Whites,
                        AdjId::Blacks};
  for (int i = 0; i < 4; ++i)
  {
    const AdjustmentSpec &s = kAdjustmentSpecs[adjIndex(ids[i])];
    if (!(vals[i] >= s.min && vals[i] <= s.max) ||
        vals[i] != std::round(vals[i]) ||
        (vals[i] == 0.0f && std::signbit(vals[i]))) // no "-0" in a History label
      return false;
  }
  return true;
}

// ------------------------------------------------------------------ status
static void testNothingToMeasure()
{
  const AutoTone none = solveAutoTone(Hist{});
  CHECK(none.status == AutoToneStatus::NoPixels && !none.usable() && allZero(none));

  Hist few{};
  few[100] = kAutoToneMinPixels - 1; // one short
  CHECK(solveAutoTone(few).status == AutoToneStatus::NoPixels);
  few[100] = kAutoToneMinPixels; // enough pixels, but all one level: flat, not "no pixels"
  CHECK(solveAutoTone(few).status == AutoToneStatus::Flat);
}

static void testFlat()
{
  // One gray, a narrow band, and a band just under the minimum spread.
  const AutoTone one = solveAutoTone(uniform(128, 128, 5000));
  CHECK(one.status == AutoToneStatus::Flat && !one.usable() && allZero(one));
  CHECK(solveAutoTone(uniform(100, 110)).status == AutoToneStatus::Flat);
  CHECK(solveAutoTone(uniform(100, 100 + kAutoToneMinSpread - 1)).status ==
        AutoToneStatus::Flat);
  // Exactly the minimum spread is measurable.
  CHECK(solveAutoTone(uniform(100, 100 + kAutoToneMinSpread)).usable());
}

// ------------------------------------------------------------- the result
// A picture that already uses the whole range is left almost alone, and its
// end points sit on the targets.
static void testWellExposedIsNearlyUntouched()
{
  const Hist ramp = uniform(0, 255, 80);
  const AutoTone a = solveAutoTone(ramp);
  CHECK(a.status == AutoToneStatus::Ok);
  CHECK(std::fabs(a.exposure) <= 10.0f && std::fabs(a.contrast) <= 10.0f);

  const auto lut = lutOf(a);
  CHECK_NEAR(lut[1], kAutoToneTargetLo, 6);   // the 0.5 % point of a 0..255 ramp is level 1
  CHECK_NEAR(lut[254], kAutoToneTargetHi, 6); // ... and the 99.5 % point is 254
  CHECK(monotone(lut));
}

// A low-contrast picture is stretched: the sliders go the right way and the end
// points land near the targets.
static void testLowContrastIsStretched()
{
  const Hist h = uniform(80, 180);
  const AutoTone a = solveAutoTone(h);
  CHECK(a.status == AutoToneStatus::Ok);
  CHECK(a.contrast > 0.0f); // widening the range needs more contrast

  const auto lut = lutOf(a);
  CHECK_NEAR(lut[80], kAutoToneTargetLo, 12);
  CHECK_NEAR(lut[180], kAutoToneTargetHi, 12);
  CHECK(lut[180] - lut[80] > 180 - 80 + 100); // range grew a lot
  CHECK(monotone(lut));
}

// Too bright: highlights come down (Exposure < 0), and the white point is not
// pushed into clipping.
static void testBrightPictureIsPulledDown()
{
  const Hist h = uniform(150, 254);
  const AutoTone a = solveAutoTone(h);
  CHECK(a.usable() && a.exposure < 0.0f);
  const auto lut = lutOf(a);
  CHECK(lut[254] < 255);
  CHECK(lut[254] < 254); // genuinely darker at the top
  CHECK(monotone(lut));

  // Highlights already clipped: Exposure never raises the gain (it would clip
  // more); Whites may pull the top back down.
  Hist clipped = uniform(60, 200);
  clipped[255] = 4000; // a big pile at pure white
  const AutoTone c = solveAutoTone(clipped);
  CHECK(c.usable() && c.exposure <= 0.0f);
  CHECK(monotone(lutOf(c)));
}

// Dark picture: Exposure goes up, and it is Limited when the slider range runs
// out before the white point reaches its target.
static void testDarkPictureIsBrightenedAndLimited()
{
  const Hist h = uniform(10, 90);
  const AutoTone a = solveAutoTone(h);
  CHECK(a.usable() && a.exposure > 0.0f);
  CHECK(a.exposure <= static_cast<float>(kExposureMax));
  const auto lut = lutOf(a);
  CHECK(lut[90] > 90);                          // brighter at the top
  CHECK(a.status == AutoToneStatus::Limited);   // 90 cannot reach 248 with these sliders
  CHECK(monotone(lut));
}

// ----------------------------------------------- robustness of the measure
// Single stray pixels at 0 and 255 must not decide the black and white points.
static void testStraySpecksAreIgnored()
{
  const Hist base = uniform(80, 180, 200);
  Hist specks = base;
  specks[0] = 1;
  specks[255] = 1;
  const AutoTone a = solveAutoTone(base), b = solveAutoTone(specks);
  CHECK(a.exposure == b.exposure && a.contrast == b.contrast &&
        a.whites == b.whites && a.blacks == b.blacks);

  // ... but a real pile (well over 0.5 %) does count.
  Hist pile = base;
  pile[0] = 2000; // ~10 % of the pixels
  const AutoTone c = solveAutoTone(pile);
  CHECK(c.exposure != a.exposure || c.contrast != a.contrast ||
        c.whites != a.whites || c.blacks != a.blacks);
}

// Only the shape matters, not how many pixels there are.
static void testScaleInvariance()
{
  const AutoTone a = solveAutoTone(uniform(60, 190, 100));
  const AutoTone b = solveAutoTone(uniform(60, 190, 100 * 37));
  CHECK(a.exposure == b.exposure && a.contrast == b.contrast &&
        a.whites == b.whites && a.blacks == b.blacks && a.status == b.status);
}

// Deterministic: the same histogram always gives the same answer.
static void testRepeatable()
{
  const Hist h = uniform(30, 220, 17);
  const AutoTone a = solveAutoTone(h), b = solveAutoTone(h);
  CHECK(a.exposure == b.exposure && a.contrast == b.contrast &&
        a.whites == b.whites && a.blacks == b.blacks && a.status == b.status);
}

// -------------------------------------------------------------- the sweep
// Over many tonal ranges: whole values inside the slider ranges, a monotonic
// table, and the end points never pushed into clipping when they were not
// clipped to begin with.
static void testSweep()
{
  int usable = 0, ok = 0;
  for (int lo = 0; lo <= 200; lo += 10)
    for (int hi = lo + kAutoToneMinSpread; hi <= 255; hi += 13)
    {
      const AutoTone a = solveAutoTone(uniform(lo, hi, 50));
      CHECK(a.usable());
      if (!a.usable())
        continue;
      ++usable;
      ok += a.status == AutoToneStatus::Ok;
      CHECK(inRangeAndWhole(a));
      const auto lut = lutOf(a);
      CHECK(monotone(lut));
      // The 0.5 % / 99.5 % points of this histogram are ~lo and ~hi.
      if (hi < 255)
        CHECK(lut[static_cast<size_t>(hi)] < 255);
      if (lo > 0)
        CHECK(lut[static_cast<size_t>(lo)] > 0);
    }
  CHECK(usable > 100);
  CHECK(ok > usable / 4); // the sweep really exercised the "reaches the targets" case
}

int main()
{
  testNothingToMeasure();
  testFlat();
  testWellExposedIsNearlyUntouched();
  testLowContrastIsStretched();
  testBrightPictureIsPulledDown();
  testDarkPictureIsBrightenedAndLimited();
  testStraySpecksAreIgnored();
  testScaleInvariance();
  testRepeatable();
  testSweep();
  return finish();
}
