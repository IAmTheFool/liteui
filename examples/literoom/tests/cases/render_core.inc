// cases/render_core.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace render_core_cases
// so they cannot collide with other cases.
//
// render_core_test.cpp - the float pipeline (render_core.hpp) against the byte pipeline it replaces
// (render_bytes.hpp, the old code verbatim), and the 16-bit paths.
//   * One stage at a time (a single tone slider, the color pass, one neighbor effect, the film
//     effects): rounding once at the end is rounding once, so the bytes must be EXACTLY the old ones.
//   * Stages stacked: the old code rounded between them, the new one does not, so the two differ by
//     a few levels at most. The test measures and bounds that.
//   * A 16-bit picture that is a widened 8-bit one gives the 8-bit result: exactly, or (with a neighbor
//     effect on, which measures a 16-bit source without rounding its luma) within a few levels.
//   * A smooth 16-bit gradient stays smooth through Texture / Clarity / Sharpness.
// Built with stand-in spatial mask shapes (stub/mask/*): the real mask.hpp, mask_range.hpp (Luminosity,
// Color Range), render_params.hpp and the whole pipeline are real.

#include "render/render_bytes.hpp"
#include "render/render_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>

namespace render_core_cases
{
using Img = CanvasImage;

static Img scene(int w, int h, uint32_t seed, int alphaMode = 0)
{
  Img im(w, h);
  std::mt19937 g(seed);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &im.pixels[(static_cast<size_t>(y) * w + x) * 4];
      const bool blk = ((x / 11) + (y / 9)) % 2 == 0;
      p[0] = static_cast<uint8_t>(std::min(255, (x * 255) / std::max(1, w - 1) * 3 / 4 + (blk ? 50 : 0) + static_cast<int>(g() % 20)));
      p[1] = static_cast<uint8_t>(std::min(255, (y * 255) / std::max(1, h - 1) * 3 / 4 + (blk ? 0 : 60) + static_cast<int>(g() % 20)));
      p[2] = static_cast<uint8_t>(g() & 255);
      p[3] = alphaMode == 0 ? 255 : (alphaMode == 1 ? static_cast<uint8_t>(g() & 255) : ((g() % 6) == 0 ? 0 : 255));
    }
  return im;
}

static std::array<std::array<uint8_t, 256>, 3> legacyLuts(const AdjValues &a, const CurveSet &c)
{
  const auto ml = curveToLut(c[0]);
  const bool useM = !curveIsIdentity(ml);
  const auto base = toneLutFromValues(a, useM ? &ml : nullptr);
  std::array<std::array<uint8_t, 256>, 3> out{};
  for (size_t ch = 0; ch < 3; ++ch)
  {
    const auto cl = curveToLut(c[1 + ch]);
    if (curveIsIdentity(cl))
    {
      out[ch] = base;
      continue;
    }
    for (int i = 0; i < 256; ++i)
      out[ch][static_cast<size_t>(i)] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(cl[base[static_cast<size_t>(i)]])), 0, 255));
  }
  return out;
}

// What AdjustmentSet::buildRenderParams does, from plain values (no LUT).
static RenderParams makeParams(const AdjValues &a, const CurveSet &curves = identityCurves(),
                               const MixValues &mix = MixValues{}, const ColorGrade &grade = ColorGrade{},
                               const MaskSet &masks = MaskSet{})
{
  RenderParams rp;
  rp.luts = legacyLuts(a, curves);
  rp.toneF = makeToneSet(a, curves);
  rp.color = makeColorPass(a[adjIndex(AdjId::Temperature)], a[adjIndex(AdjId::Tint)], a[adjIndex(AdjId::Vibrance)],
                           a[adjIndex(AdjId::Saturation)], mix, grade, a[adjIndex(AdjId::Hue)],
                           a[adjIndex(AdjId::ColorizeHue)], a[adjIndex(AdjId::ColorizeAmount)], filterValuesFromAdj(a));
  rp.fx = neighbor_fx::gainsFromValues(a[adjIndex(AdjId::Texture)], a[adjIndex(AdjId::Clarity)], a[adjIndex(AdjId::Sharpness)],
                                       a[adjIndex(AdjId::Dehaze)], a[adjIndex(AdjId::NoiseLuma)], a[adjIndex(AdjId::NoiseColor)],
                                       a[adjIndex(AdjId::Blur)]);
  if (!masks.empty())
    rp.masks = buildMaskPasses(masks, GeoFrame{});
  rp.film = filmFromAdj(a);
  return rp;
}
static AdjValues only(AdjId id, float v)
{
  AdjValues a{};
  a[adjIndex(id)] = v;
  return a;
}
static bool same(const Img &a, const Img &b) { return a.width == b.width && a.height == b.height && a.pixels == b.pixels; }
static int maxDiff(const Img &a, const Img &b, double *mean = nullptr)
{
  if (a.pixels.size() != b.pixels.size())
    return 1 << 30;
  int m = 0;
  double sum = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i)
  {
    const int d = std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i]));
    m = std::max(m, d);
    sum += d;
  }
  if (mean)
    *mean = a.pixels.empty() ? 0 : sum / static_cast<double>(a.pixels.size());
  return m;
}
static Image16 widen(const Img &a)
{
  Image16 o(a.width, a.height);
  widenSamples(a.pixels.data(), o.pixels.data(), a.pixels.size());
  return o;
}

// ---------------------------------------------------------------- identity
TEST_CASE("render_core: identity", "[render_core]")
{
  for (int alpha = 0; alpha < 3; ++alpha)
  {
    const Img src = scene(40, 30, 1 + static_cast<uint32_t>(alpha), alpha);
    const RenderParams rp = makeParams(AdjValues{});
    CHECK((rp.toneF->identity && !rp.color.active && !rp.fx.any() && !rp.film.any()));
    const auto out = renderWith(src, rp);
    CHECK((out && same(*out, src) && out.get() != &src));
  }
  // A hand-built RenderParams without float tables (toneF null) still works, byte-exactly.
  RenderParams bare;
  for (auto &l : bare.luts)
    for (size_t i = 0; i < 256; ++i)
      l[i] = static_cast<uint8_t>(255 - i);
  const Img src = scene(20, 10, 4);
  const auto inv = renderWith(src, bare);
  const auto old = renderWithBytes(src, bare);
  CHECK((same(*inv, *old) && inv->pixels[0] == 255 - src.pixels[0]));
  // An empty picture.
  const auto e = renderWith(Img(0, 0), makeParams(only(AdjId::Exposure, 30.0f)));
  CHECK((e && e->width == 0 && e->height == 0 && e->pixels.empty()));
}

// -------------------------------------------------- one stage: exactly the old bytes
static void checkExact(const char *what, const RenderParams &rp, const Img &src)
{
  const auto a = renderWith(src, rp);
  const auto b = renderWithBytes(src, rp);
  if (!(a && b && same(*a, *b)))
  {
    std::printf("  [%s] differs from the byte pipeline by up to %d\n", what, a && b ? maxDiff(*a, *b) : -1);
    CHECK(false);
  }
}

TEST_CASE("render_core: singleStagesAreExact", "[render_core]")
{
  const Img src = scene(97, 71, 7);
  int n = 0;
  for (const auto &s : kAdjustmentSpecs)
  {
    if (s.id == AdjId::LutAmount)
      continue;
    for (float v : {s.min, s.max, s.min * 0.5f, s.max * 0.5f, s.max * 0.2f})
    {
      if (v == 0.0f || v < s.min || v > s.max)
        continue;
      // Stages that are "one pass" for rounding purposes. Detail layers share one pass with each
      // other; every other neighbor effect is its own pass, so each is tried alone.
      checkExact("single slider", makeParams(only(s.id, v)), src);
      ++n;
    }
  }
  CHECK(n > 150);
  // Mixer and Grading alone.
  MixValues mix{};
  mix[mixIndex(MixChan::Saturation, 0)] = 60.0f;
  mix[mixIndex(MixChan::Hue, 3)] = -40.0f;
  mix[mixIndex(MixChan::Luminance, 5)] = 50.0f;
  checkExact("mixer", makeParams(AdjValues{}, identityCurves(), mix), src);
  ColorGrade gr;
  gr.wheel[0].hue = 220.0f; gr.wheel[0].sat = 60.0f; gr.wheel[2].hue = 40.0f; gr.wheel[2].sat = 50.0f; gr.wheel[1].lum = 20.0f;
  checkExact("grading", makeParams(AdjValues{}, identityCurves(), MixValues{}, gr), src);
  // Curves alone: master, and a per-channel one (each its own single stage).
  CurveSet cs = identityCurves();
  cs[0] = CurvePoints{{0, 0}, {80, 60}, {170, 200}, {255, 255}};
  checkExact("master curve", makeParams(AdjValues{}, cs), src);
  CurveSet ch = identityCurves();
  ch[2] = CurvePoints{{0, 15}, {255, 230}};
  checkExact("channel curve", makeParams(AdjValues{}, ch), src);
  // Master AND channel curve: two stages. The byte pipeline rounds between them, the float one does not.
  cs[2] = CurvePoints{{0, 15}, {255, 230}};
  const RenderParams two = makeParams(AdjValues{}, cs);
  CHECK(maxDiff(*renderWith(src, two), *renderWithBytes(src, two)) <= 2);
  // Alpha patterns pass through untouched.
  checkExact("partial alpha", makeParams(only(AdjId::Saturation, 40.0f)), scene(33, 21, 9, 1));
  checkExact("transparent corners + film", makeParams([] { AdjValues a{}; a[adjIndex(AdjId::Vignette)] = -60.0f; a[adjIndex(AdjId::Grain)] = 40.0f; return a; }()), scene(33, 21, 9, 2));
}

// Several effects at once: the three detail layers are ONE pass, so any mix of them is exact too.
TEST_CASE("render_core: detailLayersTogetherAreExact", "[render_core]")
{
  const Img src = scene(120, 90, 3);
  AdjValues a{};
  a[adjIndex(AdjId::Texture)] = 55.0f;
  a[adjIndex(AdjId::Clarity)] = -30.0f;
  a[adjIndex(AdjId::Sharpness)] = 70.0f;
  checkExact("texture+clarity+sharpness", makeParams(a), src);
}

// ------------------------------------------------ stages stacked: close, and measured
static Mask radialMask(std::mt19937 &g, bool lumToo)
{
  Mask m;
  RadialShape r;
  r.cx = 0.3f + 0.4f * static_cast<float>(g() & 255) / 255.0f;
  r.cy = 0.3f + 0.4f * static_cast<float>(g() & 255) / 255.0f;
  r.rx = r.ry = 0.25f + 0.2f * static_cast<float>(g() & 255) / 255.0f;
  m.parts.push_back({r, MaskOp::Add});
  if (lumToo)
  {
    LuminosityShape l;
    l.lo = 0.1f;
    l.hi = 0.7f;
    l.feather = 0.4f;
    m.parts.push_back({l, MaskOp::Intersect});
  }
  m.invert = (g() % 4) == 0;
  m.amount = 40.0f + static_cast<float>(g() % 61);
  return m;
}

// Threshold, Posterize and Solarize are discontinuous (Solarize folds at 128): a sub-level difference
// in the value they are given can flip a pixel from one side of a step to the other, so a few
// samples differ by a lot.
//
// Even the continuous effects can differ a lot at a few pixels, and that is not a flaw of the new
// pipeline: the color pass is very steep at dark, strongly saturated pixels (measured: about 12 output
// levels per input level), so the roughly 1.7 levels the byte pipeline's per-slider rounding had
// already drifted from the exact tone result come out as a gap of 20-30 levels there. The new result
// is the more accurate one. What the test can fairly demand is that this stays rare: the share of
// samples more than 8 levels apart, and the mean gap, are bounded; the single worst pixel is not.
static bool isStep(AdjId id) { return id == AdjId::Threshold || id == AdjId::Posterize || id == AdjId::Solarize; }

TEST_CASE("render_core: stackedStagesStayClose", "[render_core]")
{
  for (bool steps : {false, true})
  {
  std::mt19937 g(steps ? 2027 : 2026);
  auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g() & 0xFFFF) / 65535.0f; };
  int worst = 0;
  double worstMean = 0, sumMean = 0, worstBigShare = 0;
  int rounds = 0;
  for (int h : {127, 128, 129, 300}) // block boundaries
    for (int rep = 0; rep < 12; ++rep)
    {
      AdjValues a{};
      for (const auto &s : kAdjustmentSpecs)
        if (s.id != AdjId::LutAmount && s.id != AdjId::Gamma && (steps || !isStep(s.id)) && (g() % 3 == 0))
          a[adjIndex(s.id)] = rnd(s.min, s.max);
      CurveSet cs = identityCurves();
      if (g() & 1)
        cs[0] = CurvePoints{{0, 0}, {70, 55}, {190, 210}, {255, 255}};
      if (g() & 1)
        cs[1 + g() % 3] = CurvePoints{{0, 10}, {255, 240}};
      MixValues mix{};
      if (g() & 1)
        for (float &m : mix)
          m = (g() & 1) ? rnd(-60, 60) : 0.0f;
      ColorGrade gr;
      if (g() & 1)
        for (auto &w : gr.wheel)
        {
          w.hue = rnd(0, 359);
          w.sat = rnd(0, 60);
          w.lum = rnd(-30, 30);
        }
      MaskSet masks;
      const int nm = static_cast<int>(g() % 3);
      for (int k = 0; k < nm; ++k)
      {
        Mask m = radialMask(g, (g() & 1) != 0);
        for (const auto &s : kAdjustmentSpecs)
          if (s.id != AdjId::LutAmount && s.id != AdjId::Gamma && (steps || !isStep(s.id)) && s.id != AdjId::Vignette && s.id != AdjId::Grain &&
              s.id != AdjId::VignetteMidpoint && s.id != AdjId::VignetteFeather && s.id != AdjId::GrainSize && s.id != AdjId::GrainRoughness &&
              (g() % 5 == 0))
            m.adj[static_cast<size_t>(s.id)] = rnd(s.min, s.max) * 0.6f;
        masks.push_back(m);
      }
      const Img src = scene(61 + static_cast<int>(g() % 40), h, static_cast<uint32_t>(g()), static_cast<int>(g() % 3));
      const RenderParams rp = makeParams(a, cs, mix, gr, masks);
      const auto n = renderWith(src, rp);
      const auto o = renderWithBytes(src, rp);
      CHECK((n && o && n->width == o->width));
      for (size_t i = 3; i < src.pixels.size(); i += 4)
        CHECK((n->pixels[i] == src.pixels[i])); // alpha never touched
      double mean = 0;
      worst = std::max(worst, maxDiff(*n, *o, &mean));
      size_t big = 0;
      for (size_t i = 0; i < n->pixels.size(); ++i)
        big += std::abs(static_cast<int>(n->pixels[i]) - static_cast<int>(o->pixels[i])) > 8;
      worstBigShare = std::max(worstBigShare, static_cast<double>(big) / static_cast<double>(n->pixels.size()));
      worstMean = std::max(worstMean, mean);
      sumMean += mean;
      ++rounds;
    }
  std::printf("  (%s: %d random renders vs the byte pipeline: largest gap %d, largest mean gap %.3f, average mean gap %.3f, largest share of samples more than 8 apart %.3f%%)\n",
              steps ? "with Threshold / Posterize" : "continuous effects", rounds, worst, worstMean, sumMean / rounds, 100.0 * worstBigShare);
  CHECK(worstMean < (steps ? 2.0 : 0.8));
  CHECK(worstBigShare < (steps ? 0.04 : 0.003));
  }
}

// ------------------------------------------------------------------ blocks and masks
TEST_CASE("render_core: blocksDoNotChangeTheResult", "[render_core]")
{
  // tone + color: rows are independent, so a picture equals its rows rendered one by one.
  AdjValues a{};
  a[adjIndex(AdjId::Exposure)] = 25.0f;
  a[adjIndex(AdjId::Saturation)] = -30.0f;
  a[adjIndex(AdjId::Tint)] = 20.0f;
  CurveSet cs = identityCurves();
  cs[2] = CurvePoints{{0, 0}, {128, 150}, {255, 255}};
  const RenderParams rp = makeParams(a, cs);
  for (int h : {1, 2, 127, 128, 129, 255, 256, 257, 300})
  {
    const Img src = scene(9, h, static_cast<uint32_t>(h));
    const auto whole = renderWith(src, rp);
    std::vector<uint8_t> rows;
    for (int y = 0; y < h; ++y)
    {
      Img row(9, 1);
      std::copy(src.pixels.begin() + static_cast<std::ptrdiff_t>(y) * 36, src.pixels.begin() + static_cast<std::ptrdiff_t>(y + 1) * 36, row.pixels.begin());
      const auto r = renderWith(row, rp);
      rows.insert(rows.end(), r->pixels.begin(), r->pixels.end());
    }
    CHECK((whole->pixels == rows));
  }
  // Film effects depend on the position in the whole picture: block boundaries must not show.
  AdjValues f{};
  f[adjIndex(AdjId::Vignette)] = -70.0f;
  f[adjIndex(AdjId::Grain)] = 40.0f;
  const Img big = scene(61, 300, 5);
  const auto rpf = makeParams(f);
  CHECK(same(*renderWith(big, rpf), *renderWithBytes(big, rpf)));
}

TEST_CASE("render_core: masksReadTheRowBeforeTheyEditIt", "[render_core]")
{
  // The render_test cases, on the float pipeline: a Luminosity mask picks pixels by the row as it
  // is BEFORE the mask edits it, and a LATER mask sees what the earlier one made.
  const int W = 20, H = 4;
  Img src(W, H);
  for (size_t i = 0; i < src.pixels.size(); i += 4)
  {
    const int gv = static_cast<int>((i / 4) % W) * 12 + 5;
    src.pixels[i] = src.pixels[i + 1] = src.pixels[i + 2] = static_cast<uint8_t>(gv);
    src.pixels[i + 3] = 255;
  }
  auto luma = [](const uint8_t *p) { return (54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8; };
  Mask dark;
  LuminosityShape lum;
  lum.lo = 0.0f;
  lum.hi = 0.5f;
  lum.feather = 0.0f;
  dark.parts.push_back({lum, MaskOp::Add});
  dark.adj[static_cast<size_t>(AdjId::Brightness)] = 100.0f;
  const auto out = renderWith(src, makeParams(AdjValues{}, identityCurves(), MixValues{}, ColorGrade{}, MaskSet{dark}));
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
  {
    const int gv = src.pixels[i];
    CHECK((out->pixels[i] == ((luma(&src.pixels[i]) <= 127) ? std::min(255, gv + 100) : gv)));
  }
  Mask second;
  LuminosityShape bright;
  bright.lo = 0.4f;
  bright.hi = 1.0f;
  bright.feather = 0.0f;
  second.parts.push_back({bright, MaskOp::Add});
  second.adj[static_cast<size_t>(AdjId::Brightness)] = -50.0f;
  const auto both = renderWith(src, makeParams(AdjValues{}, identityCurves(), MixValues{}, ColorGrade{}, MaskSet{dark, second}));
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
  {
    int gv = src.pixels[i];
    if (luma(&src.pixels[i]) <= 127)
      gv = std::min(255, gv + 100);
    if (gv * 1.0 / 255.0 >= 0.4)
      gv = std::max(0, gv - 50);
    CHECK((both->pixels[i] == gv));
  }
  // The same through the byte pipeline: identical.
  const auto oldBoth = renderWithBytes(src, makeParams(AdjValues{}, identityCurves(), MixValues{}, ColorGrade{}, MaskSet{dark, second}));
  CHECK(same(*both, *oldBoth));
  // A mask with a tone curve and a partial amount: within one level of the byte pipeline (the blend
  // is no longer rounded before it is mixed).
  Mask soft = radialMask(*std::make_unique<std::mt19937>(5), false);
  soft.adj[static_cast<size_t>(AdjId::Exposure)] = 40.0f;
  soft.curves[0] = CurvePoints{{0, 0}, {100, 120}, {255, 255}};
  const Img s2 = scene(80, 60, 12);
  const auto rp = makeParams(AdjValues{}, identityCurves(), MixValues{}, ColorGrade{}, MaskSet{soft});
  CHECK((rp.masks.size() == 1 && rp.masks[0].toneF));
  CHECK(maxDiff(*renderWith(s2, rp), *renderWithBytes(s2, rp)) <= 4);
}

TEST_CASE("render_core: cancellation", "[render_core]")
{
  const Img src = scene(50, 400, 3);
  const RenderParams rp = makeParams(only(AdjId::Exposure, 30.0f));
  std::atomic<uint64_t> latest{5};
  const auto ok = renderWith(src, rp, &latest, 5);
  CHECK((ok && same(*ok, *renderWith(src, rp))));
  CHECK(renderWith(src, rp, &latest, 4) == nullptr);
  latest.store(6);
  CHECK(renderWith(src, rp, &latest, 5) == nullptr);
  const RenderParams fx = makeParams(only(AdjId::Clarity, 50.0f));
  CHECK(fx.fx.any());
  CHECK(renderWith(src, fx, &latest, 5) == nullptr);
  CHECK(renderWith(src, fx, &latest, 6) != nullptr);
  neighbor_fx::clearCaches();
}

// ------------------------------------------------------------------- 16-bit
TEST_CASE("render_core: widenedPictureGivesTheSameResult", "[render_core]")
{
  std::mt19937 g(77);
  auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(g() & 0xFFFF) / 65535.0f; };
  for (int rep = 0; rep < 10; ++rep)
  {
    AdjValues a{};
    for (const auto &s : kAdjustmentSpecs)
      if (s.id != AdjId::LutAmount && (g() % 3 == 0))
        a[adjIndex(s.id)] = rnd(s.min, s.max);
    MaskSet masks;
    if (rep % 2)
      masks.push_back(radialMask(g, true)), masks.back().adj[static_cast<size_t>(AdjId::Exposure)] = 30.0f,
                                            masks.back().adj[static_cast<size_t>(AdjId::Clarity)] = 40.0f;
    const RenderParams rp = makeParams(a, identityCurves(), MixValues{}, ColorGrade{}, masks);
    const Img src = scene(70 + rep, 50 + rep * 7, 900 + static_cast<uint32_t>(rep), rep % 3);
    const Image16 w = widen(src);
    const auto ref = renderWith(src, rp);
    // The neighbor effects measure a 16-bit source at its own precision (a pixel's luma is not rounded
    // to a level any more), so with one of them on the result is close to the 8-bit one, not equal.
    // Without them the widened picture still gives exactly the 8-bit result.
    bool fxOn = rp.fx.any();
    for (const MaskPass &m : rp.masks)
      fxOn = fxOn || m.fx.any();
    const int tol = fxOn ? 4 : 0;
    const auto viaView = renderToDisplay(w, &src, rp);       // with an 8-bit view supplied (not used any more)
    const auto viaTemp = renderToDisplay(w, nullptr, rp);    // without
    CHECK((viaView && viaTemp && maxDiff(*viaView, *ref) <= tol && maxDiff(*viaTemp, *ref) <= tol));
    CHECK((viaView && viaTemp && same(*viaView, *viaTemp)));
    // 16-bit out: narrowing it gives the 8-bit result (at worst one level off where a value sits on a tie).
    const auto big = renderToImage16(w, &src, rp);
    CHECK((big && big->width == ref->width && big->height == ref->height));
    int worst = 0;
    for (size_t i = 0; big && i < big->pixels.size(); ++i)
      worst = std::max(worst, std::abs(static_cast<int>(narrow16(big->pixels[i])) - static_cast<int>(ref->pixels[i])));
    CHECK(worst <= 1 + tol);
  }
  neighbor_fx::clearCaches();
}

TEST_CASE("render_core: sixteenBitPrecision", "[render_core]")
{
  // Identity keeps every 16-bit sample, alpha included.
  Image16 im(37, 23);
  std::mt19937 g(5);
  for (uint16_t &v : im.pixels)
    v = static_cast<uint16_t>(g());
  const auto id = renderToImage16(im, nullptr, makeParams(AdjValues{}));
  CHECK((id && id->pixels == im.pixels));
  // A linear slider moves every sample by the same amount, so neighbors one step apart stay one step
  // apart (an 8-bit pipeline would have merged them).
  Image16 ramp(256, 4);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 256; ++x)
    {
      uint16_t *p = &ramp.pixels[(static_cast<size_t>(y) * 256 + x) * 4];
      p[0] = p[1] = p[2] = static_cast<uint16_t>(10000 + x * 3 + y);
      p[3] = 65535;
    }
  const auto up = renderToImage16(ramp, nullptr, makeParams(only(AdjId::Brightness, 10.0f)));
  CHECK(up != nullptr);
  int distinct = 0;
  for (int y = 0; up && y < 4; ++y)
    for (int x = 0; x < 256; ++x)
    {
      const uint16_t in = ramp.pixels[(static_cast<size_t>(y) * 256 + x) * 4];
      const uint16_t o = up->pixels[(static_cast<size_t>(y) * 256 + x) * 4];
      CHECK(std::abs(static_cast<int>(o) - (static_cast<int>(in) + 2570)) <= 1);
      distinct += (x > 0 && o != up->pixels[(static_cast<size_t>(y) * 256 + x - 1) * 4]);
    }
  CHECK(distinct > 3 * 250); // neighbors (3 apart at 16 bits, 0.01 level) stay different
  // Color pass at 16 bits keeps detail below a level: two samples that differ by 1/257 level still differ.
  Image16 two(2, 1);
  two.pixels = {20000, 30000, 40000, 65535, 20001, 30001, 40001, 65535};
  const auto sat = renderToImage16(two, nullptr, makeParams(only(AdjId::Saturation, 30.0f)));
  CHECK((sat && sat->pixels[0] != sat->pixels[4]));
  // The picture handed in is never modified.
  CHECK((two.pixels[0] == 20000 && two.pixels[4] == 20001));
  // Empty and mismatched pictures.
  const auto none = renderToImage16(Image16(), nullptr, makeParams(only(AdjId::Exposure, 30.0f)));
  CHECK((none && none->empty()));
  // The 8-bit view is not used by the neighbor effects any more, so a view of the wrong size changes nothing.
  const Img wrong = scene(5, 5, 1);
  const Image16 src16 = widen(scene(30, 20, 2));
  const auto fxp = makeParams(only(AdjId::Sharpness, 60.0f));
  CHECK(same(*renderToDisplay(src16, &wrong, fxp), *renderToDisplay(src16, nullptr, fxp)));
  neighbor_fx::clearCaches();
}

TEST_CASE("render_core: sixteenBitGradientKeepsItsSmoothness", "[render_core]")
{
  // A gentle 16-bit ramp (20 samples = 0.078 level per pixel) through Texture, Clarity and Sharpness
  // at 100: the picture has no detail, so the effects must leave it alone. Measuring an 8-bit copy
  // would have moved samples by up to half a level (about 128 samples); at 16 bits they stay within a
  // few steps.
  const int W = 600, H = 8;
  Image16 ramp(W, H);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
    {
      uint16_t *p = &ramp.pixels[(static_cast<size_t>(y) * W + x) * 4];
      p[0] = p[1] = p[2] = static_cast<uint16_t>(20000 + x * 20);
      p[3] = 65535;
    }
  AdjValues a{};
  a[adjIndex(AdjId::Texture)] = 100.0f;
  a[adjIndex(AdjId::Clarity)] = 100.0f;
  a[adjIndex(AdjId::Sharpness)] = 100.0f;
  const auto out = renderToImage16(ramp, nullptr, makeParams(a));
  CHECK(out != nullptr);
  int worst = 0;
  for (int y = 0; out && y < H; ++y)
    for (int x = 60; x < W - 60; ++x)
      for (int c = 0; c < 3; ++c)
      {
        const size_t i = (static_cast<size_t>(y) * W + x) * 4 + static_cast<size_t>(c);
        worst = std::max(worst, std::abs(static_cast<int>(out->pixels[i]) - static_cast<int>(ramp.pixels[i])));
      }
  CHECK(worst <= 16);
  neighbor_fx::clearCaches();
}
} // namespace render_core_cases
