// Build: g++ -std=c++20 -I.. film_fx_test.cpp -o film_fx_test && ./film_fx_test
#include "detail/film_fx.hpp"
#include "check.hpp"

#include <vector>

using namespace film_fx;

// A flat RGBA picture of one gray level, alpha 255.
static std::vector<uint8_t> flat(int w, int h, int level, int alpha = 255)
{
  std::vector<uint8_t> v(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < v.size(); i += 4)
  {
    v[i] = v[i + 1] = v[i + 2] = static_cast<uint8_t>(level);
    v[i + 3] = static_cast<uint8_t>(alpha);
  }
  return v;
}
static int at(const std::vector<uint8_t> &v, int w, int x, int y, int c = 0)
{
  return v[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(c)];
}
static std::vector<uint8_t> run(const Params &p, std::vector<uint8_t> px, int w, int h)
{
  apply(prepare(p, w, h), px.data(), static_cast<size_t>(w) * h, 0);
  return px;
}

static void testParams()
{
  // All sliders at 0: a vignette-free, grain-free default.
  const Params d = paramsFromValues(0, 0, 0, 0, 0, 0);
  CHECK(!d.any() && d == Params{});
  CHECK(d.midpoint == 0.5f && d.feather == 0.5f && d.roughness == 0.5f);
  // Ranges: amounts scale to -1..1 / 0..1 and are clamped.
  CHECK(paramsFromValues(250, 0, 0, 0, 0, 0).vignette == 1.0f);
  CHECK(paramsFromValues(-250, 0, 0, 0, 0, 0).vignette == -1.0f);
  CHECK(paramsFromValues(0, 0, 0, 250, 0, 0).grain == 1.0f);
  CHECK(paramsFromValues(0, 0, 0, -5, 0, 0).grain == 0.0f);
  CHECK(paramsFromValues(0, 0, 0, 0, 500, 0).size == 1.0f);
  // Midpoint and Feather stay inside the range the ramp needs.
  CHECK_NEAR(paramsFromValues(0, 100, 0, 0, 0, 0).midpoint, 0.95, 1e-6);
  CHECK_NEAR(paramsFromValues(0, -100, 0, 0, 0, 0).midpoint, 0.05, 1e-6);
  CHECK(paramsFromValues(0, 0, 100, 0, 0, 0).feather == 1.0f && paramsFromValues(0, 0, -100, 0, 0, 0).feather == 0.0f);
  CHECK(paramsFromValues(0, 0, 0, 0, 0, 100).roughness == 1.0f && paramsFromValues(0, 0, 0, 0, 0, -100).roughness == 0.0f);
  // NaN and infinity count as 0 (untouched), so nothing non-finite reaches the loop.
  const float nan = std::nanf(""), inf = INFINITY;
  const Params bad = paramsFromValues(nan, inf, -inf, nan, inf, nan);
  CHECK(!bad.any() && bad.midpoint == 0.5f && bad.feather == 0.5f && bad.roughness == 0.5f && bad.size == 0.0f);
}

static void testPrepare()
{
  CHECK(!prepare(Params{}, 100, 100).any());
  CHECK(!prepare(paramsFromValues(50, 0, 0, 50, 0, 0), 0, 10).any()); // no size: nothing to do
  CHECK(!prepare(paramsFromValues(50, 0, 0, 50, 0, 0), 10, -1).any());
  const Prepared f = prepare(paramsFromValues(-50, 0, 0, 40, 0, 0), 200, 100);
  CHECK(f.vig && f.grain && f.any() && f.W == 200 && f.H == 100);
  // The ramp is never empty, at any Midpoint / Feather.
  for (float mid : {-100.0f, 0.0f, 100.0f})
    for (float fea : {-100.0f, 0.0f, 100.0f})
    {
      const Prepared q = prepare(paramsFromValues(10, mid, fea, 0, 0, 0), 64, 48);
      CHECK(std::isfinite(q.invRamp) && q.invRamp > 0.0f && q.inner >= 0.0f);
    }
  // Grain cells grow with Size, in proportion to the long edge, never below 1 px.
  const Prepared small = prepare(paramsFromValues(0, 0, 0, 50, 0, 0), 100, 80);
  CHECK(small.cell == 1.0f);
  const float c1 = prepare(paramsFromValues(0, 0, 0, 50, 100, 0), 2000, 1000).cell;
  const float c2 = prepare(paramsFromValues(0, 0, 0, 50, 100, 0), 4000, 2000).cell;
  CHECK_NEAR(c2, 2.0f * c1, 1e-3);
  CHECK(prepare(paramsFromValues(0, 0, 0, 50, 100, 0), 4000, 2000).cell > prepare(paramsFromValues(0, 0, 0, 50, 0, 0), 4000, 2000).cell);
}

static void testHash()
{
  // Pinned: integer arithmetic only, so these are identical on every compiler.
  CHECK(hash2(0, 0, kSeedCrisp) == 0x63d17e20u);
  CHECK(hash2(1, 0, kSeedCrisp) == 0xb4e56be3u);
  CHECK(hash2(0, 1, kSeedCrisp) == 0x5abcbc3bu);
  CHECK(hash2(123, 456, kSeedSmooth) == 0x12477187u);
  // hashUnit stays in [-1, 1) and is roughly zero-mean.
  double sum = 0;
  float lo = 2, hi = -2;
  for (uint32_t y = 0; y < 128; ++y)
    for (uint32_t x = 0; x < 128; ++x)
    {
      const float v = hashUnit(x, y, kSeedCrisp);
      sum += v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  CHECK(lo >= -1.0f && hi < 1.0f && lo < -0.99f && hi > 0.99f);
  CHECK_NEAR(sum / (128.0 * 128.0), 0.0, 0.03);
  // Value noise passes through the lattice hashes exactly.
  CHECK(valueNoise(7.0f, 9.0f, kSeedSmooth) == hashUnit(7, 9, kSeedSmooth));
  CHECK(valueNoise(0.0f, 0.0f, kSeedSmooth) == hashUnit(0, 0, kSeedSmooth));
}

static void testIdentity()
{
  // Off means off: the bytes are untouched, whatever the other sliders say.
  const auto src = flat(20, 10, 100);
  CHECK(run(paramsFromValues(0, 50, 50, 0, 80, 20), src, 20, 10) == src);
  CHECK(run(Params{}, src, 20, 10) == src);
  // Nothing happens to an empty buffer or a zero-count call.
  std::vector<uint8_t> none;
  apply(prepare(paramsFromValues(50, 0, 0, 50, 0, 0), 10, 10), none.data(), 0, 0);
}

static void testVignette()
{
  const int W = 200, H = 100;
  const auto src = flat(W, H, 128);
  const auto dark = run(paramsFromValues(-100, 0, 0, 0, 0, 0), src, W, H);
  const auto light = run(paramsFromValues(100, 0, 0, 0, 0, 0), src, W, H);
  // The center is untouched, the corners go most of the way to black / white.
  CHECK(at(dark, W, W / 2, H / 2) == 128 && at(light, W, W / 2, H / 2) == 128);
  CHECK(at(dark, W, 0, 0) < 20 && at(light, W, 0, 0) > 235);
  // 90 % toward black: 128 * 0.1 = 12.8 at full weight.
  CHECK(std::abs(at(dark, W, 0, 0) - 13) <= 3);
  // All four corners are the same (the frame is symmetric), and so are the
  // middles of the four sides: the ellipse follows the frame's aspect ratio,
  // not a circle, so the left edge and the top edge are equally far out.
  CHECK(at(dark, W, 0, 0) == at(dark, W, W - 1, 0) && at(dark, W, 0, 0) == at(dark, W, 0, H - 1) && at(dark, W, 0, 0) == at(dark, W, W - 1, H - 1));
  CHECK(std::abs(at(dark, W, 0, H / 2) - at(dark, W, W / 2, 0)) <= 1);
  // Monotonic from the center out along a row and a column; never brighter than the source.
  for (int x = W / 2; x + 1 < W; ++x)
    CHECK(at(dark, W, x + 1, H / 2) <= at(dark, W, x, H / 2) && at(light, W, x + 1, H / 2) >= at(light, W, x, H / 2));
  for (int y = H / 2; y + 1 < H; ++y)
    CHECK(at(dark, W, W / 2, y + 1) <= at(dark, W, W / 2, y));
  for (size_t i = 0; i < dark.size(); i += 4)
    CHECK(dark[i] <= 128 && light[i] >= 128 && dark[i] == dark[i + 1] && dark[i] == dark[i + 2] && dark[i + 3] == 255);
  // Amount scales the effect.
  const auto half = run(paramsFromValues(-50, 0, 0, 0, 0, 0), src, W, H);
  CHECK(at(half, W, 0, 0) > at(dark, W, 0, 0) && at(half, W, 0, 0) < 128);
  // Midpoint moves the ramp out; a high Midpoint leaves more of the picture alone.
  const auto wide = run(paramsFromValues(-100, 100, 0, 0, 0, 0), src, W, H);
  const auto tight = run(paramsFromValues(-100, -100, 0, 0, 0, 0), src, W, H);
  CHECK(at(wide, W, W / 4, H / 4) > at(dark, W, W / 4, H / 4) && at(dark, W, W / 4, H / 4) > at(tight, W, W / 4, H / 4));
  // Feather: a hard ramp is a sharper step than a soft one.
  const Prepared hard = prepare(paramsFromValues(-100, 0, -100, 0, 0, 0), W, H);
  const Prepared soft = prepare(paramsFromValues(-100, 0, 100, 0, 0, 0), W, H);
  CHECK(hard.invRamp > soft.invRamp);
  // vignetteWeight is 0 at the center, 1 only far out, and always in 0..1.
  const Prepared f = prepare(paramsFromValues(-100, 0, 0, 0, 0, 0), W, H);
  CHECK(vignetteWeight(f, W / 2, H / 2) == 0.0f && vignetteWeight(f, 0, 0) == 1.0f);
  for (int y = 0; y < H; y += 7)
    for (int x = 0; x < W; x += 7)
      CHECK(vignetteWeight(f, x, y) >= 0.0f && vignetteWeight(f, x, y) <= 1.0f);
}

static void testVignetteFollowsTheFrame()
{
  // The vignette is a function of the frame's SIZE only: the same sliders on two
  // different pictures of one size give the same weights. A crop is a picture of
  // another size, so its vignette is centered on the crop.
  const Params p = paramsFromValues(-80, 10, -10, 0, 0, 0);
  const auto a = run(p, flat(120, 60, 200), 120, 60);
  auto other = flat(120, 60, 100);
  const auto b = run(p, other, 120, 60);
  for (int y = 0; y < 60; ++y)
    for (int x = 0; x < 120; ++x)
      CHECK(std::abs(at(a, 120, x, y) / 2.0 - at(b, 120, x, y)) <= 1.0);
  // A 60 x 60 crop of the left half has its own center and its own dark corners.
  const auto c = run(p, flat(60, 60, 200), 60, 60);
  CHECK(at(c, 60, 30, 30) == 200 && at(c, 60, 59, 30) < 200 && at(a, 120, 59, 30) > at(c, 60, 59, 30));
}

static void testGrain()
{
  const int W = 160, H = 120;
  const auto src = flat(W, H, 128);
  const Params p = paramsFromValues(0, 0, 0, 60, 0, 0);
  const auto g = run(p, src, W, H);
  // Deterministic: same bytes every time, and not the source.
  CHECK(run(p, src, W, H) == g && g != src);
  // Zero-mean, within the amplitude the Amount allows, monochrome, alpha untouched.
  double sum = 0;
  int lo = 255, hi = 0;
  for (size_t i = 0; i < g.size(); i += 4)
  {
    CHECK(g[i] == g[i + 1] && g[i] == g[i + 2] && g[i + 3] == 255);
    sum += g[i] - 128;
    lo = std::min(lo, static_cast<int>(g[i]));
    hi = std::max(hi, static_cast<int>(g[i]));
  }
  CHECK_NEAR(sum / (static_cast<double>(W) * H), 0.0, 1.0);
  CHECK(lo < 128 - 10 && hi > 128 + 10);
  CHECK(lo >= 128 - 0.6 * kGrainLevels * kSmoothGainMax - 1 && hi <= 128 + 0.6 * kGrainLevels * kSmoothGainMax + 1);
  // More Amount, more spread.
  auto spread = [&](float amount)
  {
    const auto o = run(paramsFromValues(0, 0, 0, amount, 0, 0), src, W, H);
    double s2 = 0;
    for (size_t i = 0; i < o.size(); i += 4)
      s2 += (o[i] - 128.0) * (o[i] - 128.0);
    return s2;
  };
  CHECK(spread(20) < spread(60) && spread(60) < spread(100));
  // A function of the position only: the same pixel gets the same grain in a
  // picture of the same size whatever its color is (up to the tone weight), and
  // changing one pixel's color changes no other pixel's grain.
  auto edited = src;
  edited[(static_cast<size_t>(5) * W + 5) * 4] = 10;
  const auto ge = run(p, edited, W, H);
  for (size_t i = 0; i < g.size(); i += 4)
    if (i != (static_cast<size_t>(5) * W + 5) * 4)
      CHECK(ge[i] == g[i]);
}

static void testGrainBlocks()
{
  // The renderer hands over blocks of whole rows with `first` set; every split of
  // the picture gives the same bytes as one call. This is what makes the preview
  // path, the background render and the export agree.
  const int W = 97, H = 71; // odd sizes
  auto src = flat(W, H, 90);
  for (size_t i = 0; i < src.size(); i += 4)
    src[i] = static_cast<uint8_t>(i % 251);
  const Params p = paramsFromValues(-60, 20, 10, 45, 30, -20);
  const Prepared f = prepare(p, W, H);
  auto whole = src;
  apply(f, whole.data(), static_cast<size_t>(W) * H, 0);
  CHECK(whole != src);
  for (int rowsPerBlock : {1, 2, 7, 13, 70, 71})
  {
    auto parts = src;
    for (int y0 = 0; y0 < H; y0 += rowsPerBlock)
    {
      const int rows = std::min(rowsPerBlock, H - y0);
      apply(f, parts.data() + static_cast<size_t>(y0) * W * 4, static_cast<size_t>(rows) * W, static_cast<size_t>(y0) * W);
    }
    CHECK(parts == whole);
  }
  // Even a split in the middle of a row (first not a multiple of W) agrees.
  auto split = src;
  const size_t cut = static_cast<size_t>(W) * 30 + 41;
  apply(f, split.data(), cut, 0);
  apply(f, split.data() + cut * 4, static_cast<size_t>(W) * H - cut, cut);
  CHECK(split == whole);
}

static void testGrainShape()
{
  const int W = 1200, H = 800; // big enough that Size 100 is a real multi-pixel cell (3.6 px)
  const auto src = flat(W, H, 128);
  // Neighboring pixels agree more when Size is large (and Roughness low): the
  // mean absolute difference to the right-hand neighbor drops.
  auto roughness = [&](float size, float rough)
  {
    const auto o = run(paramsFromValues(0, 0, 0, 80, size, rough), src, W, H);
    double d = 0;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x + 1 < W; ++x)
        d += std::abs(at(o, W, x, y) - at(o, W, x + 1, y));
    return d / (static_cast<double>(W - 1) * H);
  };
  CHECK(roughness(100, 0) < roughness(0, 0) * 0.8);
  CHECK(roughness(100, -100) < roughness(100, 100));
  // Midtones get the full grain, black and white less (and can only go one way).
  auto spreadAt = [&](int level)
  {
    const auto o = run(paramsFromValues(0, 0, 0, 100, 0, 100), flat(W, H, level), W, H);
    double s2 = 0;
    for (size_t i = 0; i < o.size(); i += 4)
      s2 += (o[i] - static_cast<double>(level)) * (o[i] - static_cast<double>(level));
    return s2;
  };
  CHECK(spreadAt(128) > spreadAt(250) && spreadAt(128) > spreadAt(8));
}

static void testAlphaAndTransparency()
{
  const int W = 40, H = 30;
  auto src = flat(W, H, 120, 200);
  // A transparent corner (as straightening leaves) stays exactly as it was.
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 5; ++x)
    {
      uint8_t *p = &src[(static_cast<size_t>(y) * W + x) * 4];
      p[0] = p[1] = p[2] = p[3] = 0;
    }
  const auto o = run(paramsFromValues(-100, 0, 0, 100, 0, 0), src, W, H);
  for (int y = 0; y < 5; ++y)
    for (int x = 0; x < 5; ++x)
      CHECK(at(o, W, x, y, 0) == 0 && at(o, W, x, y, 1) == 0 && at(o, W, x, y, 2) == 0 && at(o, W, x, y, 3) == 0);
  // Alpha is never changed, anywhere.
  for (size_t i = 3; i < o.size(); i += 4)
    CHECK(o[i] == src[i]);
}

int main()
{
  testParams();
  testPrepare();
  testHash();
  testIdentity();
  testVignette();
  testVignetteFollowsTheFrame();
  testGrain();
  testGrainBlocks();
  testGrainShape();
  testAlphaAndTransparency();
  return finish();
}
