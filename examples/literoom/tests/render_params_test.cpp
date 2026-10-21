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
  CHECK(!rp.color.active && !rp.fx.any() && rp.masks.empty());
  const MaskPass mp{};
  CHECK(mp.parts.empty() && !mp.invert && mp.amount == 1.0f && !mp.tone);
}

int main()
{
  testWhichMasksAreKept();
  testWhatEachPassHolds();
  testShapesAreMappedThroughTheFrame();
  testRenderParamsDefaults();
  return finish();
}
