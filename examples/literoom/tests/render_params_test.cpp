// Build: g++ -std=c++20 -I.. render_params_test.cpp -o render_params_test && ./render_params_test
#include "edit/render_params.hpp"
#include "check.hpp"

static Mask maskWith(AdjId id, float v)
{
  Mask m;
  m.parts.push_back({RadialShape{}, MaskOp::Add});
  m.adj[static_cast<size_t>(id)] = v;
  return m;
}
static const GeoFrame kId = makeGeoFrame(400, 300, Orient{}, 0.0f, false, CropRect{});

static void testWhichMasksAreKept()
{
  CHECK(buildMaskPasses({}, kId).empty());
  // Only masks that are on, have a part, a non-zero amount and some effect survive.
  Mask off = maskWith(AdjId::Exposure, 30.0f);
  off.enabled = false;
  Mask empty;
  empty.adj[static_cast<size_t>(AdjId::Exposure)] = 30.0f;
  Mask amount0 = maskWith(AdjId::Exposure, 30.0f);
  amount0.amount = 0.0f;
  Mask nothing;
  nothing.parts.push_back({RadialShape{}, MaskOp::Add}); // all sliders 0, curves straight, no grade
  const Mask good = maskWith(AdjId::Exposure, 30.0f);
  const auto passes = buildMaskPasses({off, empty, amount0, nothing, good}, kId);
  CHECK(passes.size() == 1);
  // The order of the survivors is the order of the list.
  const auto two = buildMaskPasses({maskWith(AdjId::Contrast, 10.0f), good, maskWith(AdjId::Tint, 5.0f)}, kId);
  CHECK(two.size() == 3 && two[0].tone && two[1].tone && !two[2].tone && two[2].color.active);
}

static void testWhatEachPassHolds()
{
  Mask m = maskWith(AdjId::Exposure, 40.0f);
  m.invert = true;
  m.amount = 35.0f;
  m.parts.push_back({LinearShape{}, MaskOp::Subtract});
  const auto p = buildMaskPasses({m}, kId);
  CHECK(p.size() == 1 && p[0].invert && p[0].amount == 0.35f && p[0].parts.size() == 2 && p[0].parts[1].op == MaskOp::Subtract);
  CHECK(p[0].parts[0].shape.index() == static_cast<size_t>(MaskType::Radial) && p[0].parts[1].shape.index() == static_cast<size_t>(MaskType::Linear));
  // The amount is a percentage clamped to 0..100.
  Mask big = maskWith(AdjId::Exposure, 40.0f);
  big.amount = 250.0f;
  CHECK(buildMaskPasses({big}, kId)[0].amount == 1.0f);

  // Tone: a table is built only when a tone slider or a curve is in play.
  const auto tone = buildMaskPasses({maskWith(AdjId::Exposure, 50.0f)}, kId)[0];
  CHECK(tone.tone && !tone.color.active && !tone.fx.any());
  AdjValues v{};
  v[static_cast<size_t>(AdjId::Exposure)] = 50.0f;
  CHECK(tone.luts[0] == toneLutFromValues(v, nullptr) && tone.luts[1] == tone.luts[0] && tone.luts[2] == tone.luts[0]);
  const auto color = buildMaskPasses({maskWith(AdjId::Saturation, 30.0f)}, kId)[0];
  CHECK(!color.tone && color.color.active && !color.fx.any());
  const auto fx = buildMaskPasses({maskWith(AdjId::Texture, 30.0f)}, kId)[0];
  CHECK(!fx.tone && !fx.color.active && fx.fx.any());
  // Color Mixer and Color Grading alone count as color effects, a curve alone as tone.
  Mask mix;
  mix.parts.push_back({RadialShape{}, MaskOp::Add});
  mix.mix[mixIndex(MixChan::Hue, 1)] = 20.0f;
  CHECK(buildMaskPasses({mix}, kId)[0].color.mix);
  Mask grade;
  grade.parts.push_back({RadialShape{}, MaskOp::Add});
  grade.grade.wheel[0].sat = 30.0f;
  CHECK(buildMaskPasses({grade}, kId)[0].color.grade);
  Mask curve;
  curve.parts.push_back({RadialShape{}, MaskOp::Add});
  curve.curves[2] = CurvePoints{{0, 0}, {128, 90}, {255, 255}}; // green only
  const auto cp = buildMaskPasses({curve}, kId)[0];
  CHECK(cp.tone && cp.luts[0] == cp.luts[2] && !(cp.luts[0] == cp.luts[1]));
}

static void testFilters()
{
  // filterValuesFromAdj picks each Filter slider out of an AdjValues, and nothing else.
  CHECK(filterValuesFromAdj(AdjValues{}).greyscale == 0.0f && !makeFilterPass(filterValuesFromAdj(AdjValues{})).any);
  AdjValues a{};
  a[static_cast<size_t>(AdjId::Greyscale)] = 10.0f;
  a[static_cast<size_t>(AdjId::Sepia)] = 20.0f;
  a[static_cast<size_t>(AdjId::Vintage)] = 30.0f;
  a[static_cast<size_t>(AdjId::Fade)] = 40.0f;
  a[static_cast<size_t>(AdjId::Negative)] = 50.0f;
  a[static_cast<size_t>(AdjId::Posterize)] = 60.0f;
  a[static_cast<size_t>(AdjId::Solarize)] = 70.0f;
  a[static_cast<size_t>(AdjId::BleachBypass)] = 80.0f;
  a[static_cast<size_t>(AdjId::Threshold)] = 90.0f;
  a[static_cast<size_t>(AdjId::CrossProcess)] = 100.0f;
  a[static_cast<size_t>(AdjId::Saturation)] = 99.0f; // not a filter
  const FilterValues f = filterValuesFromAdj(a);
  CHECK(f.greyscale == 10.0f && f.sepia == 20.0f && f.vintage == 30.0f && f.fade == 40.0f);
  CHECK(f.negative == 50.0f && f.posterize == 60.0f && f.solarize == 70.0f);
  CHECK(f.bleachBypass == 80.0f && f.threshold == 90.0f && f.crossProcess == 100.0f);

  // A mask with only a Filter slider set is kept (the filter makes its color pass active), and it
  // holds exactly what makeFilterPass makes of the values.
  for (AdjId id : {AdjId::Greyscale, AdjId::Sepia, AdjId::Vintage, AdjId::Fade, AdjId::Negative, AdjId::Posterize, AdjId::Solarize,
                   AdjId::BleachBypass, AdjId::Threshold, AdjId::CrossProcess})
  {
    const auto p = buildMaskPasses({maskWith(id, 40.0f)}, kId);
    CHECK(p.size() == 1 && !p[0].tone && !p[0].fx.any() && p[0].color.active && p[0].color.filters.any);
  }
  Mask mk = maskWith(AdjId::Greyscale, 40.0f);
  mk.adj[static_cast<size_t>(AdjId::Posterize)] = 60.0f;
  mk.adj[static_cast<size_t>(AdjId::BleachBypass)] = 30.0f;
  const auto mp = buildMaskPasses({mk}, kId);
  AdjValues mv{};
  mv[static_cast<size_t>(AdjId::Greyscale)] = 40.0f;
  mv[static_cast<size_t>(AdjId::Posterize)] = 60.0f;
  mv[static_cast<size_t>(AdjId::BleachBypass)] = 30.0f;
  const FilterPass want = makeFilterPass(filterValuesFromAdj(mv));
  CHECK(mp.size() == 1 && mp[0].color.filters.grey == want.grey && mp[0].color.filters.posterLevels == want.posterLevels && mp[0].color.filters.bleach == want.bleach);
  CHECK(want.bleach == 0.3f);
  CHECK(want.grey == 0.4f && want.posterLevels >= 2 && want.posterLevels < 256);
  // Out-of-range values are clamped on the way in.
  CHECK(buildMaskPasses({maskWith(AdjId::Sepia, 250.0f)}, kId)[0].color.filters.sepia == 1.0f);
  // A filter that rounds to no visible effect (Posterize this low is still 256 levels) leaves the mask empty.
  CHECK(buildMaskPasses({maskWith(AdjId::Posterize, 0.01f)}, kId).empty());
  // Filters do not leak into the tone table or the neighbor effects.
  const auto both = buildMaskPasses({maskWith(AdjId::Exposure, 30.0f), maskWith(AdjId::Negative, 100.0f)}, kId);
  CHECK(both.size() == 2 && both[0].tone && !both[0].color.filters.any && !both[1].tone && both[1].color.filters.any);
}

static void testFilm()
{
  // filmFromAdj picks the six Effects sliders out of an AdjValues, and nothing else.
  CHECK(!filmFromAdj(AdjValues{}).any() && filmFromAdj(AdjValues{}) == film_fx::Params{});
  AdjValues a{};
  a[static_cast<size_t>(AdjId::Vignette)] = -30.0f;
  a[static_cast<size_t>(AdjId::VignetteMidpoint)] = 20.0f;
  a[static_cast<size_t>(AdjId::VignetteFeather)] = -10.0f;
  a[static_cast<size_t>(AdjId::Grain)] = 50.0f;
  a[static_cast<size_t>(AdjId::GrainSize)] = 40.0f;
  a[static_cast<size_t>(AdjId::GrainRoughness)] = -60.0f;
  a[static_cast<size_t>(AdjId::Saturation)] = 99.0f; // not an effect
  CHECK(filmFromAdj(a) == film_fx::paramsFromValues(-30, 20, -10, 50, 40, -60));
  // Vignette and Grain are global only: a mask that carries them (a hand-edited
  // sidecar) is dropped as one with no adjustments, and they change nothing else.
  for (AdjId id : {AdjId::Vignette, AdjId::VignetteMidpoint, AdjId::VignetteFeather, AdjId::Grain, AdjId::GrainSize, AdjId::GrainRoughness})
    CHECK(buildMaskPasses({maskWith(id, 40.0f)}, kId).empty());
  Mask m = maskWith(AdjId::Exposure, 30.0f);
  m.adj[static_cast<size_t>(AdjId::Vignette)] = -50.0f;
  m.adj[static_cast<size_t>(AdjId::Grain)] = 40.0f;
  const auto with = buildMaskPasses({m}, kId), without = buildMaskPasses({maskWith(AdjId::Exposure, 30.0f)}, kId);
  CHECK(with.size() == 1 && with[0].tone && !with[0].color.active && !with[0].fx.any() && with[0].luts == without[0].luts);
}

static void testLutIsGlobalOnly()
{
  // The 3D LUT belongs to the picture, not to a mask: a mask that carries LUT Amount (a hand-edited
  // sidecar) is dropped as one with no adjustments, and the value changes nothing else about it.
  CHECK(buildMaskPasses({maskWith(AdjId::LutAmount, 80.0f)}, kId).empty());
  Mask m = maskWith(AdjId::Exposure, 30.0f);
  m.adj[static_cast<size_t>(AdjId::LutAmount)] = 100.0f;
  const auto with = buildMaskPasses({m}, kId), without = buildMaskPasses({maskWith(AdjId::Exposure, 30.0f)}, kId);
  CHECK(with.size() == 1 && with[0].tone && !with[0].color.active && !with[0].color.lut.any() && !with[0].fx.any());
  CHECK(with[0].luts == without[0].luts);
  // Nothing in a fresh RenderParams / MaskPass holds a table.
  CHECK(!RenderParams{}.color.lut.any() && !MaskPass{}.color.lut.any());
}

static void testShapesAreMappedThroughTheFrame()
{
  // A mask stored in the top-left of the original ends up in the top-right of a
  // picture turned a quarter turn clockwise.
  Mask m = maskWith(AdjId::Exposure, 30.0f);
  RadialShape r;
  r.cx = 0.1f;
  r.cy = 0.1f;
  m.parts[0].shape = r;
  const GeoFrame rot = makeGeoFrame(400, 300, Orient{1, false}, 0.0f, false, CropRect{});
  const RadialShape moved = std::get<RadialShape>(buildMaskPasses({m}, rot)[0].parts[0].shape);
  CHECK(moved.cx > 0.8f && moved.cy < 0.2f);
  // With the identity frame nothing moves.
  CHECK(std::get<RadialShape>(buildMaskPasses({m}, kId)[0].parts[0].shape) == r);
}

static void testRenderParamsDefaults()
{
  const RenderParams rp{};
  for (const auto &l : rp.luts)
    for (int i = 0; i < 256; ++i)
      CHECK(l[static_cast<size_t>(i)] == 0); // value-initialized: callers fill the tables
  CHECK(!rp.color.active && !rp.fx.any() && rp.masks.empty() && !rp.film.any());
  const MaskPass mp{};
  CHECK(mp.parts.empty() && !mp.invert && mp.amount == 1.0f && !mp.tone);
}

int main()
{
  testWhichMasksAreKept();
  testWhatEachPassHolds();
  testFilters();
  testFilm();
  testLutIsGlobalOnly();
  testShapesAreMappedThroughTheFrame();
  testRenderParamsDefaults();
  return finish();
}