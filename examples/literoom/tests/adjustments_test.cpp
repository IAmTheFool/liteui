// Build: g++ -std=c++20 -I.. adjustments_test.cpp -o adjustments_test && ./adjustments_test
#include "core/adjustments.hpp"
#include "check.hpp"

#include <set>
#include <string>

static constexpr size_t N = static_cast<size_t>(AdjId::Count);

static const AdjustmentSpec &spec(AdjId id) { return kAdjustmentSpecs[static_cast<size_t>(id)]; }

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
                   AdjId::Temperature, AdjId::Tint, AdjId::Vibrance, AdjId::Saturation, AdjId::Hue,
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
  testToneFunctions();
  testToneDirections();
  return finish();
}