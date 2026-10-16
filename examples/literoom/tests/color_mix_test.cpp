// Build: g++ -std=c++20 -I.. color_mix_test.cpp -o color_mix_test && ./color_mix_test
#include "color_mix.hpp"
#include "check.hpp"

#include <cstring>
#include <random>

static int lutIndex(float hueDeg)
{
  return static_cast<int>(std::lround(hueDeg / 360.0f * kMixLutSize)) % kMixLutSize;
}

static void testLayout()
{
  CHECK(kMixBandCount == 8 && kMixChanCount == 3);
  CHECK(MixValues{}.size() == kMixBandCount * kMixChanCount);
  // Channel-major, every slot distinct and in range.
  bool seen[kMixBandCount * kMixChanCount] = {};
  for (size_t c = 0; c < kMixChanCount; ++c)
    for (size_t b = 0; b < kMixBandCount; ++b)
    {
      const size_t i = mixIndex(static_cast<MixChan>(c), b);
      CHECK(i == c * kMixBandCount + b);
      CHECK(i < seen[0] + kMixBandCount * kMixChanCount && !seen[i]);
      seen[i] = true;
    }
  // Centers: ascending, close the circle.
  CHECK(kMixCenters.front() == 0.0f && kMixCenters.back() == 360.0f);
  // The band positions themselves (Red .. Magenta, then back to Red at 360).
  const float expected[] = {0, 30, 60, 120, 180, 240, 270, 320, 360};
  for (size_t i = 0; i < kMixCenters.size(); ++i)
    CHECK(kMixCenters[i] == expected[i]);
  for (size_t i = 0; i + 1 < kMixCenters.size(); ++i)
    CHECK(kMixCenters[i] < kMixCenters[i + 1]);
  CHECK(std::strcmp(mixBandName(0), "Red") == 0);
  CHECK(std::strcmp(mixBandName(3), "Green") == 0);
  CHECK(std::strcmp(mixBandName(7), "Magenta") == 0);
}

static void testNonZero()
{
  MixValues m{};
  CHECK(!mixAnyNonZero(m));
  m[mixIndex(MixChan::Luminance, 4)] = 1.0f;
  CHECK(mixAnyNonZero(m));
  CHECK(mixChanAnyNonZero(m, MixChan::Luminance));
  CHECK(!mixChanAnyNonZero(m, MixChan::Hue));
  CHECK(!mixChanAnyNonZero(m, MixChan::Saturation));
}

static void testNeutralIsIdentity()
{
  const MixLut lut = buildMixLut(MixValues{});
  for (int i = 0; i < kMixLutSize; ++i)
  {
    CHECK(lut.hueShift[static_cast<size_t>(i)] == 0.0f);
    CHECK(lut.satMul[static_cast<size_t>(i)] == 1.0f);
    CHECK(lut.lumAdj[static_cast<size_t>(i)] == 0.0f);
  }
}

static void testBandCentersGetExactlyTheirOwnValue()
{
  // At a band's own center the blend is that band alone (t = 0).
  for (size_t b = 0; b < kMixBandCount; ++b)
    for (size_t c = 0; c < kMixChanCount; ++c)
    {
      MixValues m{};
      m[mixIndex(static_cast<MixChan>(c), b)] = 100.0f;
      const MixLut lut = buildMixLut(m);
      // kMixCenters are whole degrees; a LUT step is 0.5 deg, so the center
      // lands exactly on an entry.
      const size_t i = static_cast<size_t>(lutIndex(kMixCenters[b]));
      CHECK_NEAR(lut.hueShift[i], c == 0 ? kMixHueRangeDeg : 0.0f, 1e-4);
      CHECK_NEAR(lut.satMul[i], c == 1 ? 2.0f : 1.0f, 1e-4);
      CHECK_NEAR(lut.lumAdj[i], c == 2 ? 1.0f : 0.0f, 1e-4);
      // ...and the other bands' centers are untouched by it.
      for (size_t o = 0; o < kMixBandCount; ++o)
        if (o != b && std::fabs(kMixCenters[o] - kMixCenters[b]) > 1.0f)
        {
          const size_t j = static_cast<size_t>(lutIndex(kMixCenters[o]));
          CHECK_NEAR(lut.hueShift[j], 0.0f, 1e-4);
          CHECK_NEAR(lut.satMul[j], 1.0f, 1e-4);
          CHECK_NEAR(lut.lumAdj[j], 0.0f, 1e-4);
        }
    }
}

static void testScalingAndSign()
{
  MixValues pos{}, neg{};
  for (size_t b = 0; b < kMixBandCount; ++b)
  {
    pos[mixIndex(MixChan::Hue, b)] = 10.0f * static_cast<float>(b + 1);
    neg[mixIndex(MixChan::Hue, b)] = -10.0f * static_cast<float>(b + 1);
    pos[mixIndex(MixChan::Luminance, b)] = 7.0f * static_cast<float>(b);
    neg[mixIndex(MixChan::Luminance, b)] = -7.0f * static_cast<float>(b);
  }
  const MixLut p = buildMixLut(pos), n = buildMixLut(neg);
  for (size_t i = 0; i < kMixLutSize; ++i)
  {
    CHECK_NEAR(p.hueShift[i], -n.hueShift[i], 1e-4);
    CHECK_NEAR(p.lumAdj[i], -n.lumAdj[i], 1e-4);
  }
  // Extreme saturation: -100 everywhere removes all of it.
  MixValues allMinus{};
  for (size_t b = 0; b < kMixBandCount; ++b)
    allMinus[mixIndex(MixChan::Saturation, b)] = -100.0f;
  const MixLut z = buildMixLut(allMinus);
  for (size_t i = 0; i < kMixLutSize; ++i)
    CHECK_NEAR(z.satMul[i], 0.0f, 1e-5);
}

static void testNoSeams()
{
  // Neighboring entries (including the wrap from Magenta back to Red) never
  // jump: the blend between bands is smooth.
  std::mt19937 rng(7);
  for (int round = 0; round < 50; ++round)
  {
    MixValues m{};
    for (float &v : m)
      v = static_cast<float>(static_cast<int>(rng() % 201) - 100);
    const MixLut lut = buildMixLut(m);
    for (size_t i = 0; i < kMixLutSize; ++i)
    {
      const size_t j = (i + 1) % kMixLutSize;
      CHECK(std::fabs(lut.hueShift[i] - lut.hueShift[j]) < 2.5f); // degrees per half degree
      CHECK(std::fabs(lut.satMul[i] - lut.satMul[j]) < 0.1f);
      CHECK(std::fabs(lut.lumAdj[i] - lut.lumAdj[j]) < 0.1f);
    }
  }
  // The wrap specifically: Red = +100 hue, everything else 0.
  MixValues m{};
  m[mixIndex(MixChan::Hue, 0)] = 100.0f;
  const MixLut lut = buildMixLut(m);
  CHECK_NEAR(lut.hueShift[0], kMixHueRangeDeg, 1e-4);
  CHECK(lut.hueShift[kMixLutSize - 1] > kMixHueRangeDeg * 0.99f); // just before 360 deg: ~Red's value
}

int main()
{
  testLayout();
  testNonZero();
  testNeutralIsIdentity();
  testBandCentersGetExactlyTheirOwnValue();
  testScalingAndSign();
  testNoSeams();
  return finish();
}
