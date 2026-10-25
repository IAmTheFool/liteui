// Build: g++ -std=c++20 -I.. adjustments_test.cpp -o adjustments_test && ./adjustments_test
#include "core/adjustments.hpp"
#include "check.hpp"

#include <set>
#include <string>

static constexpr size_t N = static_cast<size_t>(AdjId::Count);

static const AdjustmentSpec &spec(AdjId id) { return kAdjustmentSpecs[static_cast<size_t>(id)]; }
static AdjValues only(AdjId id, float v);

static void testTable()
{
  CHECK(kAdjustmentSpecs.size() == N);
  CHECK(AdjValues{}.size() == N);
  std::set<std::string> labels;
  int stage = 0; // stages must come in the order Tone, Color, Detail
  for (size_t i = 0; i < N; ++i)
  {
    const AdjustmentSpec &s = kAdjustmentSpecs[i];
    CHECK(static_cast<size_t>(s.id) == i);        // indexed by AdjId
    CHECK(s.min < s.max);
    CHECK(s.min <= 0.0f && 0.0f <= s.max);        // 0 (untouched) is always valid
    CHECK(!s.label.empty() && labels.insert(std::string(s.label)).second);
    CHECK(static_cast<int>(s.stage) >= stage);
    stage = static_cast<int>(s.stage);
    // Only Tone sliders carry a per-channel function; the others are consumed
    // by the color pass / detail layers.
    CHECK((s.stage == AdjustmentSpec::Stage::Tone) == static_cast<bool>(s.apply));
  }
  CHECK(spec(AdjId::Exposure).stage == AdjustmentSpec::Stage::Tone);
  CHECK(spec(AdjId::Blacks).stage == AdjustmentSpec::Stage::Tone);
  for (AdjId id : {AdjId::BlackPoint, AdjId::WhitePoint, AdjId::Gamma})
    CHECK(spec(id).stage == AdjustmentSpec::Stage::Tone);
  CHECK(spec(AdjId::Temperature).stage == AdjustmentSpec::Stage::Color);
  CHECK(spec(AdjId::ColorizeAmount).stage == AdjustmentSpec::Stage::Color);
  CHECK(spec(AdjId::Greyscale).stage == AdjustmentSpec::Stage::Color);
  CHECK(spec(AdjId::Solarize).stage == AdjustmentSpec::Stage::Color);
  CHECK(spec(AdjId::Texture).stage == AdjustmentSpec::Stage::Detail);
  CHECK(spec(AdjId::Blur).stage == AdjustmentSpec::Stage::Detail);
}

static void testRanges()
{
  CHECK(spec(AdjId::Exposure).min == kExposureMin && spec(AdjId::Exposure).max == kExposureMax);
  CHECK(spec(AdjId::Brightness).min == kBrightnessMin && spec(AdjId::Brightness).max == kBrightnessMax);
  CHECK(spec(AdjId::BlackPoint).min == 0.0f && spec(AdjId::BlackPoint).max == static_cast<float>(kBlackPointMax));
  CHECK(spec(AdjId::WhitePoint).min == 0.0f && spec(AdjId::WhitePoint).max == static_cast<float>(kWhitePointMax));
  CHECK(spec(AdjId::Contrast).min == kToneMin && spec(AdjId::Contrast).max == kToneMax);
  CHECK(spec(AdjId::ColorizeHue).min == 0.0f && spec(AdjId::ColorizeHue).max == 360.0f);
  CHECK(spec(AdjId::ColorizeAmount).min == 0.0f && spec(AdjId::ColorizeAmount).max == 100.0f);
  // One-sided sliders: they only add an effect.
  for (AdjId id : {AdjId::Sharpness, AdjId::NoiseLuma, AdjId::NoiseColor, AdjId::Blur})
    CHECK(spec(id).min == 0.0f && spec(id).max == 100.0f);
  // The Filters are strengths too: 0 (off) to 100.
  for (AdjId id : {AdjId::Greyscale, AdjId::Sepia, AdjId::Vintage, AdjId::Fade, AdjId::Negative, AdjId::Posterize, AdjId::Solarize,
                   AdjId::BleachBypass, AdjId::Threshold, AdjId::CrossProcess})
    CHECK(spec(id).min == 0.0f && spec(id).max == 100.0f);
  // Two-sided ones are symmetric.
  for (AdjId id : {AdjId::Contrast, AdjId::Highlights, AdjId::Shadows, AdjId::Whites, AdjId::Blacks,
                   AdjId::Gamma, AdjId::Temperature, AdjId::Tint, AdjId::Vibrance, AdjId::Saturation, AdjId::Hue,
                   AdjId::Texture, AdjId::Clarity, AdjId::Dehaze})
    CHECK(spec(id).min == -spec(id).max);
}

static void testToneFunctions()
{
  for (size_t i = 0; i < N; ++i)
  {
    const AdjustmentSpec &s = kAdjustmentSpecs[i];
    if (!s.apply)
      continue;
    // Value 0 is the identity at every level.
    for (int x = 0; x <= 255; ++x)
      CHECK_NEAR(s.apply(static_cast<float>(x), 0.0f), x, 1e-4);
    // At either end of the slider the curve never runs backwards (a darker
    // input must not come out brighter than a lighter one).
    for (float v : {s.min, s.max})
    {
      float prev = s.apply(0.0f, v);
      for (int x = 1; x <= 255; ++x)
      {
        const float y = s.apply(static_cast<float>(x), v);
        CHECK(y >= prev - 1e-3f);
        prev = y;
      }
    }
  }
}

static void testToneDirections()
{
  // Positive brightens, negative darkens.
  for (AdjId id : {AdjId::Exposure, AdjId::Brightness, AdjId::Highlights, AdjId::Shadows,
                   AdjId::Whites, AdjId::Blacks})
  {
    const AdjustmentSpec &s = spec(id);
    // pick a level inside each slider's range of influence
    const float x = id == AdjId::Highlights ? 220.0f : id == AdjId::Whites ? 245.0f : id == AdjId::Shadows ? 40.0f : id == AdjId::Blacks ? 5.0f : 100.0f;
    CHECK(s.apply(x, 50.0f) > x);
    CHECK(s.apply(x, -50.0f) < x);
  }
  // Exposure scales: +100 doubles, -100 zeroes.
  CHECK_NEAR(spec(AdjId::Exposure).apply(60.0f, 100.0f), 120.0f, 1e-4);
  CHECK_NEAR(spec(AdjId::Exposure).apply(60.0f, -100.0f), 0.0f, 1e-4);
  // Brightness is a flat offset.
  CHECK_NEAR(spec(AdjId::Brightness).apply(60.0f, 25.0f), 85.0f, 1e-4);
  // Contrast pivots on mid-gray; -100 flattens to it.
  CHECK_NEAR(spec(AdjId::Contrast).apply(128.0f, 70.0f), 128.0f, 1e-4);
  CHECK_NEAR(spec(AdjId::Contrast).apply(10.0f, -100.0f), 128.0f, 1e-4);
  CHECK_NEAR(spec(AdjId::Contrast).apply(228.0f, 100.0f), 328.0f, 1e-3); // unclamped: the LUT builder clamps
  // Each tonal slider leaves the opposite end of the range alone.
  CHECK_NEAR(spec(AdjId::Highlights).apply(100.0f, 100.0f), 100.0f, 1e-4); // below 0.4 * 255
  CHECK_NEAR(spec(AdjId::Shadows).apply(200.0f, 100.0f), 200.0f, 1e-4);    // above 0.6 * 255
  CHECK_NEAR(spec(AdjId::Whites).apply(150.0f, 100.0f), 150.0f, 1e-4);     // below 0.75 * 255
  CHECK_NEAR(spec(AdjId::Blacks).apply(100.0f, 100.0f), 100.0f, 1e-4);     // above 0.25 * 255
  // Their reach: Highlights/Shadows move up to 0.6 * value levels, Whites/Blacks 0.4 * value.
  CHECK_NEAR(spec(AdjId::Highlights).apply(255.0f, 100.0f), 255.0f + 60.0f, 1e-3);
  CHECK_NEAR(spec(AdjId::Shadows).apply(0.0f, 100.0f), 60.0f, 1e-3);
  CHECK_NEAR(spec(AdjId::Whites).apply(255.0f, 100.0f), 255.0f + 40.0f, 1e-3);
  CHECK_NEAR(spec(AdjId::Blacks).apply(0.0f, 100.0f), 40.0f, 1e-3);
}


// The Filters block: ten Color-stage strengths, kept together between
// Colorize Amount and the first Detail entry (main.cpp starts its "Filters"
// header at Greyscale, so the block must stay contiguous and in this order).
static void testFilterBlock()
{
  const AdjId filters[] = {AdjId::Greyscale, AdjId::Sepia, AdjId::Vintage, AdjId::Fade, AdjId::Negative, AdjId::Posterize, AdjId::Solarize,
                           AdjId::BleachBypass, AdjId::Threshold, AdjId::CrossProcess};
  const char *labels[] = {"Greyscale", "Sepia", "Vintage", "Fade", "Negative", "Posterize", "Solarize",
                          "Bleach Bypass", "Threshold", "Cross Process"};
  for (size_t i = 0; i < 10; ++i)
  {
    const AdjustmentSpec &s = spec(filters[i]);
    CHECK(s.stage == AdjustmentSpec::Stage::Color && s.apply == nullptr);
    CHECK(s.label == labels[i]); // the labels are sidecar keys: renaming one orphans saved values
    CHECK(adjIndex(filters[i]) == adjIndex(AdjId::Greyscale) + i); // contiguous, in order
  }
  CHECK(adjIndex(AdjId::Greyscale) == adjIndex(AdjId::ColorizeAmount) + 1);
  CHECK(adjIndex(AdjId::Solarize) + 1 == adjIndex(AdjId::BleachBypass)); // the newer three follow Solarize
  CHECK(adjIndex(AdjId::Texture) == adjIndex(AdjId::CrossProcess) + 1);
  // Values are sanitized into 0..100 like every other slider.
  CHECK(sanitizedValue(spec(AdjId::Greyscale), 250.0f) == 100.0f);
  CHECK(sanitizedValue(spec(AdjId::Greyscale), -5.0f) == 0.0f);
  CHECK(sanitizedValue(spec(AdjId::Greyscale), std::nanf("")) == 0.0f);
  // They are not part of the tone table, at any strength.
  AdjValues f{};
  for (AdjId id : filters)
    f[static_cast<size_t>(id)] = 100.0f;
  const auto lut = toneLutFromValues(f, nullptr);
  for (int i = 0; i < 256; ++i)
    CHECK(lut[static_cast<size_t>(i)] == i);
}

// ------------------------------------------------------------------- Levels
static void testLevels()
{
  const AdjustmentSpec &bp = spec(AdjId::BlackPoint), &wp = spec(AdjId::WhitePoint), &gm = spec(AdjId::Gamma);
  // The labels are sidecar keys, and the three sit together after Blacks, in this order.
  CHECK(bp.label == "Black Point" && wp.label == "White Point" && gm.label == "Gamma");
  CHECK(adjIndex(AdjId::BlackPoint) == adjIndex(AdjId::Blacks) + 1);
  CHECK(adjIndex(AdjId::WhitePoint) == adjIndex(AdjId::BlackPoint) + 1 && adjIndex(AdjId::Gamma) == adjIndex(AdjId::WhitePoint) + 1);
  CHECK(adjIndex(AdjId::Temperature) == adjIndex(AdjId::Gamma) + 1);

  // Black Point v: level v goes to 0, 255 stays 255, below v is negative (the LUT clamps).
  CHECK_NEAR(bp.apply(40.0f, 40.0f), 0.0f, 1e-3);
  CHECK_NEAR(bp.apply(255.0f, 40.0f), 255.0f, 1e-3);
  CHECK(bp.apply(10.0f, 40.0f) < 0.0f);
  CHECK_NEAR(bp.apply(147.5f, 40.0f), 127.5f, 1e-3); // halfway between 40 and 255 lands halfway
  // White Point v: level 255 - v goes to 255, 0 stays 0, above it exceeds 255.
  CHECK_NEAR(wp.apply(215.0f, 40.0f), 255.0f, 1e-3);
  CHECK_NEAR(wp.apply(0.0f, 40.0f), 0.0f, 1e-4);
  CHECK(wp.apply(250.0f, 40.0f) > 255.0f);
  // Gamma: the ends never move; positive brightens the midtones, negative darkens them.
  for (float v : {-100.0f, -37.0f, 25.0f, 100.0f})
  {
    CHECK_NEAR(gm.apply(0.0f, v), 0.0f, 1e-4);
    CHECK_NEAR(gm.apply(255.0f, v), 255.0f, 1e-2);
    CHECK((gm.apply(128.0f, v) > 128.0f) == (v > 0.0f));
  }
  // +100 is the exponent 0.25: a quarter-gray input comes out at 0.25^0.25 = sqrt(0.5) of white.
  CHECK_NEAR(gm.apply(255.0f * 0.25f, 100.0f), 255.0f * std::sqrt(0.5f), 1e-2);
  // A negative input (never produced by the LUT builder) must not give NaN.
  CHECK(!std::isnan(gm.apply(-5.0f, 50.0f)) && !std::isnan(gm.apply(-5.0f, -50.0f)));

  // Through the table: the identity at 0, and never decreasing at any strength.
  for (AdjId id : {AdjId::BlackPoint, AdjId::WhitePoint, AdjId::Gamma})
  {
    const AdjustmentSpec &s = spec(id);
    const auto idl = toneLutFromValues(only(id, 0.0f), nullptr);
    for (int i = 0; i < 256; ++i)
      CHECK(idl[static_cast<size_t>(i)] == i);
    for (float v = s.min; v <= s.max; v += 5.0f)
    {
      const auto l = toneLutFromValues(only(id, v), nullptr);
      for (int i = 1; i < 256; ++i)
        CHECK(l[static_cast<size_t>(i)] >= l[static_cast<size_t>(i) - 1]);
    }
  }
  // Black Point and White Point crush / clip exactly where they say.
  const auto lb = toneLutFromValues(only(AdjId::BlackPoint, 40.0f), nullptr);
  const auto lw = toneLutFromValues(only(AdjId::WhitePoint, 40.0f), nullptr);
  for (int i = 0; i <= 40; ++i)
    CHECK(lb[static_cast<size_t>(i)] == 0);
  for (int i = 215; i < 256; ++i)
    CHECK(lw[static_cast<size_t>(i)] == 255);

  // All three at once, at every combination of the slider extremes, stay monotonic,
  // and equal the one-slider tables run one after another in table order.
  for (float b : {0.0f, 100.0f})
    for (float w : {0.0f, 100.0f})
      for (float g : {-100.0f, 0.0f, 100.0f})
      {
        AdjValues v{};
        v[adjIndex(AdjId::BlackPoint)] = b;
        v[adjIndex(AdjId::WhitePoint)] = w;
        v[adjIndex(AdjId::Gamma)] = g;
        const auto l = toneLutFromValues(v, nullptr);
        const auto l1 = toneLutFromValues(only(AdjId::BlackPoint, b), nullptr);
        const auto l2 = toneLutFromValues(only(AdjId::WhitePoint, w), nullptr);
        const auto l3 = toneLutFromValues(only(AdjId::Gamma, g), nullptr);
        for (int i = 0; i < 256; ++i)
        {
          if (i > 0)
            CHECK(l[static_cast<size_t>(i)] >= l[static_cast<size_t>(i) - 1]);
          CHECK(l[static_cast<size_t>(i)] == l3[l2[l1[static_cast<size_t>(i)]]]);
        }
      }
  // They run after the older Tone sliders (Exposure first, then Black Point).
  AdjValues mix{};
  mix[adjIndex(AdjId::Exposure)] = 30.0f;
  mix[adjIndex(AdjId::BlackPoint)] = 20.0f;
  const auto lm = toneLutFromValues(mix, nullptr);
  const auto le = toneLutFromValues(only(AdjId::Exposure, 30.0f), nullptr);
  const auto lbp = toneLutFromValues(only(AdjId::BlackPoint, 20.0f), nullptr);
  for (int i = 0; i < 256; ++i)
    CHECK(lm[static_cast<size_t>(i)] == lbp[le[static_cast<size_t>(i)]]);
}

// ---------------------------------------------------------- toneLutFromValues
static AdjValues only(AdjId id, float v)
{
  AdjValues a{};
  a[static_cast<size_t>(id)] = v;
  return a;
}

static void testToneLut()
{
  // Nothing set: the identity, with or without a curve table that is itself the identity.
  const auto id = toneLutFromValues(AdjValues{}, nullptr);
  for (int i = 0; i < 256; ++i)
    CHECK(id[static_cast<size_t>(i)] == i);
  std::array<float, 256> ident{};
  for (int i = 0; i < 256; ++i)
    ident[static_cast<size_t>(i)] = static_cast<float>(i);
  CHECK(toneLutFromValues(AdjValues{}, &ident) == id);

  // One slider: its function, rounded and clamped to a byte.
  const auto exp50 = toneLutFromValues(only(AdjId::Exposure, 50.0f), nullptr);
  const auto bri20 = toneLutFromValues(only(AdjId::Brightness, 20.0f), nullptr);
  for (int i = 0; i < 256; ++i)
  {
    CHECK(exp50[static_cast<size_t>(i)] == std::min(255, static_cast<int>(std::lround(i * 1.5))));
    CHECK(bri20[static_cast<size_t>(i)] == std::min(255, i + 20));
  }
  const auto dark = toneLutFromValues(only(AdjId::Exposure, -100.0f), nullptr);
  const auto bright = toneLutFromValues(only(AdjId::Brightness, 100.0f), nullptr);
  CHECK(dark[255] == 0 && bright[0] == 100 && bright[200] == 255); // clamped, never wrapped

  // Several sliders are applied in table order, each stored as a byte before the
  // next: the result equals running the one-slider tables one after the other.
  AdjValues both{};
  both[static_cast<size_t>(AdjId::Exposure)] = 50.0f;
  both[static_cast<size_t>(AdjId::Brightness)] = -30.0f;
  const auto lboth = toneLutFromValues(both, nullptr);
  const auto lb = toneLutFromValues(only(AdjId::Brightness, -30.0f), nullptr);
  bool orderShows = false;
  for (int i = 0; i < 256; ++i)
  {
    CHECK(lboth[static_cast<size_t>(i)] == lb[exp50[static_cast<size_t>(i)]]); // Exposure first, then Brightness
    orderShows = orderShows || lboth[static_cast<size_t>(i)] != exp50[lb[static_cast<size_t>(i)]];
  }
  CHECK(orderShows); // the order is not a detail: the other order gives other bytes

  // Color-stage and Detail-stage sliders are not part of the tone table.
  AdjValues others{};
  for (size_t i = 0; i < N; ++i)
    if (kAdjustmentSpecs[i].stage != AdjustmentSpec::Stage::Tone)
      others[i] = kAdjustmentSpecs[i].max * 0.5f;
  CHECK(toneLutFromValues(others, nullptr) == id);

  // The curve table comes last, on the bytes the sliders left: lut = curve[sliders[i]].
  std::array<float, 256> invert{}, boost{};
  for (int i = 0; i < 256; ++i)
  {
    invert[static_cast<size_t>(i)] = static_cast<float>(255 - i);
    boost[static_cast<size_t>(i)] = static_cast<float>(i) * 1.2f - 10.0f; // runs out of range at both ends
  }
  const auto inv = toneLutFromValues(AdjValues{}, &invert);
  const auto invExp = toneLutFromValues(only(AdjId::Exposure, 50.0f), &invert);
  const auto clampd = toneLutFromValues(AdjValues{}, &boost);
  for (int i = 0; i < 256; ++i)
  {
    CHECK(inv[static_cast<size_t>(i)] == 255 - i);
    CHECK(invExp[static_cast<size_t>(i)] == 255 - exp50[static_cast<size_t>(i)]);
    CHECK(clampd[static_cast<size_t>(i)] == std::clamp(static_cast<int>(std::lround(i * 1.2 - 10.0)), 0, 255));
  }

  // Every slider at both ends builds a table without trouble (run under the
  // sanitizers), and a brighter exposure never makes any level darker.
  for (size_t i = 0; i < N; ++i)
    for (float v : {kAdjustmentSpecs[i].min, kAdjustmentSpecs[i].max})
    {
      const auto l = toneLutFromValues(only(kAdjustmentSpecs[i].id, v), nullptr);
      (void)l;
    }
  for (int i = 0; i < 256; ++i)
    CHECK(toneLutFromValues(only(AdjId::Exposure, 30.0f), nullptr)[static_cast<size_t>(i)] >= id[static_cast<size_t>(i)]);
}

int main()
{
  testToneLut();
  testTable();
  testRanges();
  testFilterBlock();
  testLevels();
  testToneFunctions();
  testToneDirections();
  return finish();
}