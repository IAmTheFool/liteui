// cases/color_pass.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace color_pass_cases
// so they cannot collide with other cases.

#include "color/color_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace color_pass_cases
{
using Px = std::vector<uint8_t>; // RGBA8

static Px one(int r, int g, int b, int a = 255)
{
  return {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b), static_cast<uint8_t>(a)};
}
static Px run(const ColorPass &cp, Px p)
{
  applyColorPass(cp, p.data(), p.size());
  return p;
}
static ColorPass pass(float temperature = 0, float tint = 0, float vibrance = 0, float saturation = 0,
                      const MixValues &mix = MixValues{}, const ColorGrade &grade = ColorGrade{},
                      float hue = 0, float colorizeHue = 0, float colorizeAmount = 0,
                      const FilterValues &filters = FilterValues{}, const LutPass &lut = LutPass{})
{
  return makeColorPass(temperature, tint, vibrance, saturation, mix, grade, hue, colorizeHue, colorizeAmount, filters, lut);
}
// A pass with only the given Filters set.
static ColorPass filterPass(const FilterValues &f) { return pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, f); }
static constexpr float FilterValues::*kFilterFields[] = {&FilterValues::greyscale, &FilterValues::sepia, &FilterValues::vintage,
                                                         &FilterValues::fade, &FilterValues::negative, &FilterValues::posterize,
                                                         &FilterValues::solarize, &FilterValues::bleachBypass, &FilterValues::threshold,
                                                         &FilterValues::crossProcess};
static int luma(const Px &p) { return static_cast<int>(std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2])); }

// ---- random passes (used by the float / bulk / fingerprint tests at the end) ----
static float rnd(std::mt19937 &g, float lo, float hi)
{
  return lo + (hi - lo) * static_cast<float>(g() & 0xFFFFFF) / 16777215.0f;
}
static bool coin(std::mt19937 &g) { return (g() & 1) != 0; }

// Every stage is independently on or off, so the random passes cover the combinations.
static ColorPass randomPass(std::mt19937 &g)
{
  const float t = coin(g) ? rnd(g, -100, 100) : 0, ti = coin(g) ? rnd(g, -100, 100) : 0;
  const float v = coin(g) ? rnd(g, -100, 100) : 0, sa = coin(g) ? rnd(g, -100, 100) : 0;
  MixValues mix{};
  if (coin(g))
    for (float &m : mix)
      m = coin(g) ? rnd(g, -100, 100) : 0;
  ColorGrade gr;
  if (coin(g))
    for (auto &w : gr.wheel)
    {
      w.hue = rnd(g, 0, 359);
      w.sat = rnd(g, 0, 100);
      w.lum = rnd(g, -100, 100);
    }
  FilterValues f;
  if (coin(g))
  {
    f.greyscale = coin(g) ? rnd(g, 0, 100) : 0;
    f.sepia = coin(g) ? rnd(g, 0, 100) : 0;
    f.vintage = coin(g) ? rnd(g, 0, 100) : 0;
    f.fade = coin(g) ? rnd(g, 0, 100) : 0;
    f.negative = coin(g) ? rnd(g, 0, 100) : 0;
    f.posterize = coin(g) ? rnd(g, 0, 100) : 0;
    f.solarize = coin(g) ? rnd(g, 0, 100) : 0;
    f.bleachBypass = coin(g) ? rnd(g, 0, 100) : 0;
    f.threshold = coin(g) ? rnd(g, 0, 100) : 0;
    f.crossProcess = coin(g) ? rnd(g, 0, 100) : 0;
  }
  LutPass lut;
  if (coin(g))
  {
    auto L = std::make_shared<Lut3D>(identityLut(9));
    for (float &x : L->rgb)
      x = std::clamp(x + rnd(g, -20, 20), 0.0f, 255.0f);
    lut = makeLutPass(L, rnd(g, 10, 100));
  }
  return makeColorPass(t, ti, v, sa, mix, gr, coin(g) ? rnd(g, -100, 100) : 0,
                       rnd(g, 0, 360), coin(g) ? rnd(g, 0, 100) : 0, f, lut);
}

TEST_CASE("color_pass: activeFlags", "[color_pass]")
{
  CHECK(!pass().active);
  CHECK((pass(10).active && !pass(10).mix && !pass(10).grade && !pass(10).hue && !pass(10).colorize));
  CHECK(pass(0, 10).active);
  CHECK(pass(0, 0, 10).active);
  CHECK(pass(0, 0, 0, 10).active);
  CHECK(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 5).hue);
  // Colorize needs an amount; a hue alone does nothing.
  CHECK(!pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 120, 0).active);
  CHECK(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 120, 30).colorize);
  MixValues m{};
  m[mixIndex(MixChan::Hue, 2)] = 5;
  CHECK(pass(0, 0, 0, 0, m).mix);
  ColorGrade g;
  g.wheel[0].sat = 5;
  CHECK(pass(0, 0, 0, 0, MixValues{}, g).grade);
  // Each Filter alone makes the pass active (without that the fast path would skip it); none set = inactive.
  CHECK((!filterPass(FilterValues{}).active && !filterPass(FilterValues{}).filters.any));
  for (float FilterValues::*field : kFilterFields)
  {
    FilterValues f;
    f.*field = 40.0f;
    const ColorPass cp = filterPass(f);
    CHECK((cp.active && cp.filters.any));
  }
  // Posterize this low still rounds to 256 levels: no visible effect, so the pass stays off.
  FilterValues tiny;
  tiny.posterize = 0.01f;
  CHECK(!filterPass(tiny).active);
}

TEST_CASE("color_pass: neutralPassChangesNothing", "[color_pass]")
{
  const ColorPass cp = pass();
  std::mt19937 rng(3);
  for (int i = 0; i < 2000; ++i)
  {
    const Px p = one(static_cast<int>(rng() % 256), static_cast<int>(rng() % 256), static_cast<int>(rng() % 256),
                     static_cast<int>(rng() % 256));
    CHECK(run(cp, p) == p);
  }
}

TEST_CASE("color_pass: whiteBalance", "[color_pass]")
{
  const ColorPass warm = pass(60), cool = pass(-60), magentaPass = pass(0, 70), greenPass = pass(0, -70);
  // Gains are normalized by their own luma: brightness is kept.
  for (const ColorPass *c : {&warm, &cool, &magentaPass, &greenPass})
    CHECK(0.299f * c->gain[0] + 0.587f * c->gain[1] + 0.114f * c->gain[2] == within(1.0f, 1e-5));
  const Px mid = one(128, 128, 128);
  const Px w = run(warm, mid), c = run(cool, mid);
  CHECK((w[0] > 128 && w[2] < 128)); // warmer: R up, B down
  CHECK((c[0] < 128 && c[2] > 128)); // cooler: the opposite
  CHECK((std::abs(luma(w) - 128) <= 2 && std::abs(luma(c) - 128) <= 2));
  const Px magenta = run(magentaPass, mid), green = run(greenPass, mid);
  CHECK((magenta[1] < 128 && green[1] > 128)); // + tint = more magenta (less green)
  CHECK((w[3] == 255 && c[3] == 255));
}

TEST_CASE("color_pass: grayStaysGray", "[color_pass]")
{
  // These stages move only color: a neutral gray must stay neutral.
  MixValues mix{};
  for (size_t b = 0; b < kMixBandCount; ++b)
  {
    mix[mixIndex(MixChan::Hue, b)] = 40;
    mix[mixIndex(MixChan::Saturation, b)] = 60;
  }
  const ColorPass passes[] = {pass(0, 0, 80), pass(0, 0, 0, 90), pass(0, 0, 0, -90), pass(0, 0, 0, 0, mix),
                              pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 70)};
  for (const ColorPass &cp : passes)
    for (int v = 0; v <= 255; v += 5)
    {
      const Px out = run(cp, one(v, v, v));
      CHECK((std::abs(out[0] - out[1]) <= 1 && std::abs(out[1] - out[2]) <= 1));
      CHECK(std::abs(out[0] - v) <= 1);
    }
}

TEST_CASE("color_pass: saturation", "[color_pass]")
{
  const ColorPass gray = pass(0, 0, 0, -100);
  const ColorPass more = pass(0, 0, 0, 50);
  std::mt19937 rng(5);
  for (int i = 0; i < 500; ++i)
  {
    const Px p = one(static_cast<int>(rng() % 256), static_cast<int>(rng() % 256), static_cast<int>(rng() % 256));
    const Px g = run(gray, p);
    CHECK((std::abs(g[0] - g[1]) <= 1 && std::abs(g[1] - g[2]) <= 1)); // fully desaturated
    CHECK(std::abs(luma(g) - luma(p)) <= 2);                         // at the same brightness
    const Px m = run(more, p);
    const int spreadIn = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
    const int spreadOut = std::max({m[0], m[1], m[2]}) - std::min({m[0], m[1], m[2]});
    CHECK(spreadOut + 1 >= spreadIn); // never less colorful (clipping can only cap the growth)
  }
  // Saturation +100 doubles the distance from gray (before clipping).
  const Px dull = run(pass(0, 0, 0, 100), one(110, 100, 90));
  CHECK((dull[0] - dull[2] >= 38 && dull[0] - dull[2] <= 42));
}

TEST_CASE("color_pass: vibranceProtectsColorfulPixels", "[color_pass]")
{
  // Vibrance boosts dull colors more than colorful ones: the growth factor of
  // the color spread falls as the input gets more saturated, down to 1 for a
  // pure primary.
  const ColorPass v = pass(0, 0, 100);
  const int samples[][3] = {{130, 120, 110}, {160, 120, 100}, {200, 100, 60}, {250, 40, 30}, {255, 0, 0}};
  double prevFactor = 1e9;
  for (const auto &s : samples)
  {
    const Px out = run(v, one(s[0], s[1], s[2]));
    const double factor = static_cast<double>(out[0] - out[2]) / (s[0] - s[2]);
    CHECK(factor >= 1.0);
    CHECK(factor < prevFactor);
    prevFactor = factor;
  }
  CHECK(run(v, one(255, 0, 0)) == one(255, 0, 0));
  // Negative vibrance mutes colors.
  const Px duller = run(pass(0, 0, -100), one(130, 120, 110));
  CHECK((duller[0] - duller[2]) < (130 - 110));
}

TEST_CASE("color_pass: hueShift", "[color_pass]")
{
  const ColorPass cp = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 100);
  CHECK(cp.hue);
  // Rows of the rotation sum to 1: gray maps to gray (and luma is kept for pure rotations).
  for (int r = 0; r < 3; ++r)
    CHECK(cp.hueM[r * 3] + cp.hueM[r * 3 + 1] + cp.hueM[r * 3 + 2] == within(1.0f, 1e-5));
  // Opposite shifts undo each other's direction: red goes toward yellow for +, toward magenta for -.
  const Px up = run(cp, one(200, 0, 0));
  const Px dn = run(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, -100), one(200, 0, 0));
  CHECK(up[1] > up[2]); // green wins over blue
  CHECK(dn[2] > dn[1]); // blue wins over green
}

TEST_CASE("color_pass: colorize", "[color_pass]")
{
  const ColorPass cp = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 100);
  CHECK(cp.colorize);
  CHECK(0.299f * cp.tintVec[0] + 0.587f * cp.tintVec[1] + 0.114f * cp.tintVec[2] == within(0.0f, 1e-3));
  const Px out = run(cp, one(128, 128, 128));
  CHECK(out[2] > out[0]); // hue 200 is a cyan-blue cast
  CHECK(std::abs(luma(out) - 128) <= 2);
  // Its strength follows the amount.
  const ColorPass half = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 50);
  CHECK(half.tintVec[2] == within(cp.tintVec[2] * 0.5f, 1e-3));
  // Hue is taken modulo 360, negatives included.
  const ColorPass a = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 100);
  const ColorPass b = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, -160, 100);
  CHECK(a.tintVec[0] == within(b.tintVec[0], 1e-3));
  CHECK(a.tintVec[1] == within(b.tintVec[1], 1e-3));
  CHECK(a.tintVec[2] == within(b.tintVec[2], 1e-3));
}

TEST_CASE("color_pass: mixerAndGradeStages", "[color_pass]")
{
  // Red band at -100 saturation: a pure red loses its color, a pure blue keeps it.
  MixValues m{};
  m[mixIndex(MixChan::Saturation, static_cast<size_t>(MixBand::Red))] = -100;
  const ColorPass mp = pass(0, 0, 0, 0, m);
  const Px red = run(mp, one(255, 0, 0));
  const Px blue = run(mp, one(0, 0, 255));
  CHECK((std::abs(red[0] - red[1]) <= 2 && std::abs(red[1] - red[2]) <= 2));
  CHECK(blue == one(0, 0, 255));
  // Luminance up on Blue: a blue pixel gets lighter, a red one is left alone.
  MixValues l{};
  l[mixIndex(MixChan::Luminance, static_cast<size_t>(MixBand::Blue))] = 100;
  const ColorPass lp = pass(0, 0, 0, 0, l);
  CHECK(luma(run(lp, one(60, 60, 220))) > luma(one(60, 60, 220)) + 10);
  CHECK(run(lp, one(220, 60, 60)) == one(220, 60, 60));

  // Grade: a global luminance lift of +50 moves mid-gray up by half of kGradeLumAmount * 255.
  ColorGrade g;
  g.wheel[static_cast<size_t>(GradeRange::Global)].lum = 50;
  const Px lifted = run(pass(0, 0, 0, 0, MixValues{}, g), one(100, 100, 100));
  CHECK(std::abs(lifted[0] - (100 + static_cast<int>(std::lround(0.5f * kGradeLumAmount * 255.0f)))) <= 1);
  CHECK((lifted[0] == lifted[1] && lifted[1] == lifted[2]));
  // Shadows only: a bright pixel is untouched.
  ColorGrade s;
  s.wheel[static_cast<size_t>(GradeRange::Shadows)].lum = 100;
  CHECK(run(pass(0, 0, 0, 0, MixValues{}, s), one(250, 250, 250)) == one(250, 250, 250));
  CHECK(run(pass(0, 0, 0, 0, MixValues{}, s), one(10, 10, 10))[0] > 40);
}

// ---------------------------------------------------------------- Filters
TEST_CASE("color_pass: filterValuesAreSanitized", "[color_pass]")
{
  FilterValues v;
  v.greyscale = 50.0f;
  v.sepia = 250.0f;  // clamped to 100
  v.vintage = -5.0f; // clamped to 0
  v.fade = std::nanf("");
  v.negative = INFINITY;
  v.solarize = 100.0f;
  const FilterPass f = makeFilterPass(v);
  CHECK(f.grey == within(0.5f, 1e-6));
  CHECK((f.sepia == 1.0f && f.vintage == 0.0f && f.fade == 0.0f && f.negative == 0.0f && f.solarize == 1.0f));
  CHECK((f.any && f.posterLevels == 0));
  // Poster level count falls smoothly from 256 (off) to 2.
  FilterValues p;
  p.posterize = 100.0f;
  const FilterPass full = makeFilterPass(p);
  CHECK((full.posterLevels == 2 && full.any));
  CHECK(full.posterScale == within(1.0f / 255.0f, 1e-7));
  CHECK(full.posterInv == within(255.0f, 1e-3));
  p.posterize = 50.0f;
  CHECK(makeFilterPass(p).posterLevels == 23);
  int prev = 256;
  for (float s = 1.0f; s <= 100.0f; s += 1.0f)
  {
    p.posterize = s;
    const int n = makeFilterPass(p).posterLevels;
    CHECK((n >= 2 && n <= prev));
    prev = n;
  }
}

TEST_CASE("color_pass: filterOnlyPassTouchesNoOtherStage", "[color_pass]")
{
  FilterValues f;
  f.greyscale = 100.0f;
  const ColorPass cp = filterPass(f);
  CHECK((cp.active && !cp.mix && !cp.grade && !cp.hue && !cp.colorize && cp.vibrance == 0.0f && cp.saturation == 1.0f));
  for (int c = 0; c < 3; ++c)
    CHECK(cp.gain[c] == within(1.0f, 1e-6));
}

TEST_CASE("color_pass: greyscaleFilter", "[color_pass]")
{
  FilterValues f;
  f.greyscale = 100.0f;
  const ColorPass cp = filterPass(f);
  std::mt19937 rng(11);
  for (int i = 0; i < 500; ++i)
  {
    const Px p = one(static_cast<int>(rng() % 256), static_cast<int>(rng() % 256), static_cast<int>(rng() % 256));
    const Px g = run(cp, p);
    CHECK((std::abs(g[0] - g[1]) <= 1 && std::abs(g[1] - g[2]) <= 1));
    CHECK(std::abs(luma(g) - luma(p)) <= 1);
  }
  // Half strength: halfway between the pixel and its luma (luma of 200,100,0 is 118.5).
  f.greyscale = 50.0f;
  const Px h = run(filterPass(f), one(200, 100, 0));
  CHECK((std::abs(h[0] - 159) <= 1 && std::abs(h[1] - 109) <= 1 && std::abs(h[2] - 59) <= 1));
}

TEST_CASE("color_pass: negativeFilter", "[color_pass]")
{
  FilterValues f;
  f.negative = 100.0f;
  const ColorPass cp = filterPass(f);
  std::mt19937 rng(12);
  for (int i = 0; i < 500; ++i)
  {
    const int r = static_cast<int>(rng() % 256), g = static_cast<int>(rng() % 256), b = static_cast<int>(rng() % 256);
    CHECK(run(cp, one(r, g, b)) == one(255 - r, 255 - g, 255 - b));
  }
  // Half strength lands on mid-gray whatever the input (127.5 rounds either way).
  f.negative = 50.0f;
  for (int v : {0, 60, 200, 255})
  {
    const Px out = run(filterPass(f), one(v, v, v));
    CHECK((out[0] == 127 || out[0] == 128));
  }
}

TEST_CASE("color_pass: posterizeFilter", "[color_pass]")
{
  FilterValues f;
  f.posterize = 100.0f; // 2 levels: every channel ends at 0 or 255
  const ColorPass two = filterPass(f);
  for (int v = 0; v < 256; ++v)
  {
    const Px out = run(two, one(v, v, v));
    CHECK((out[0] == (v >= 128 ? 255 : 0) && out[1] == out[0] && out[2] == out[0]));
  }
  f.posterize = 50.0f; // 23 levels
  const ColorPass mid = filterPass(f);
  std::vector<int> seen;
  for (int v = 0; v < 256; ++v)
  {
    const int out = run(mid, one(v, v, v))[0];
    if (std::find(seen.begin(), seen.end(), out) == seen.end())
      seen.push_back(out);
  }
  CHECK(seen.size() == 23);
  CHECK((run(mid, one(0, 0, 0)) == one(0, 0, 0) && run(mid, one(255, 255, 255)) == one(255, 255, 255))); // the ends stay put
}

TEST_CASE("color_pass: solarizeFilter", "[color_pass]")
{
  FilterValues f;
  f.solarize = 100.0f;
  const ColorPass cp = filterPass(f);
  // Channel values above 128 are inverted, the rest are left alone.
  CHECK(run(cp, one(200, 200, 200)) == one(55, 55, 55));
  CHECK(run(cp, one(100, 100, 100)) == one(100, 100, 100));
  CHECK(run(cp, one(128, 128, 128)) == one(128, 128, 128));
  CHECK(run(cp, one(129, 129, 129)) == one(126, 126, 126));
  CHECK(run(cp, one(255, 0, 90)) == one(0, 0, 90)); // per channel
}

TEST_CASE("color_pass: sepiaFadeVintageFilters", "[color_pass]")
{
  FilterValues f;
  f.sepia = 100.0f;
  const ColorPass sep = filterPass(f);
  const Px s = run(sep, one(128, 128, 128)); // the classic matrix: 1.351, 1.203, 0.937 times 128
  CHECK((std::abs(s[0] - 173) <= 1 && std::abs(s[1] - 154) <= 1 && std::abs(s[2] - 120) <= 1));
  CHECK(run(sep, one(0, 0, 0)) == one(0, 0, 0));
  const Px sw = run(sep, one(255, 255, 255));
  CHECK((sw[0] == 255 && sw[1] == 255 && std::abs(sw[2] - 239) <= 1));

  FilterValues fd;
  fd.fade = 100.0f; // blacks lifted to 40, white stays 255
  const ColorPass fade = filterPass(fd);
  CHECK((run(fade, one(0, 0, 0)) == one(40, 40, 40) && run(fade, one(255, 255, 255)) == one(255, 255, 255)));
  CHECK(std::abs(run(fade, one(128, 128, 128))[0] - 148) <= 1);
  fd.fade = 50.0f;
  CHECK(std::abs(run(filterPass(fd), one(0, 0, 0))[0] - 20) <= 1);

  FilterValues fv;
  fv.vintage = 100.0f;
  const ColorPass vin = filterPass(fv);
  const Px black = run(vin, one(0, 0, 0)); // blacks lifted, warm cast: R > G > B > 0
  CHECK((black[0] > black[1] && black[1] > black[2] && black[2] > 0));
  const Px gray = run(vin, one(128, 128, 128));
  CHECK((gray[0] > gray[1] && gray[1] > gray[2]));
  const Px white = run(vin, one(255, 255, 255));
  CHECK((white[0] == 255 && white[2] < 255));
}

// ---- Bleach Bypass, Threshold, Cross Process ----
static int spread(const Px &p) { return std::max({int(p[0]), int(p[1]), int(p[2])}) - std::min({int(p[0]), int(p[1]), int(p[2])}); }

TEST_CASE("color_pass: newFilterValuesAreSanitized", "[color_pass]")
{
  FilterValues v;
  v.bleachBypass = 250.0f;     // clamped to 100
  v.threshold = std::nanf(""); // NaN becomes 0
  v.crossProcess = -5.0f;      // clamped to 0
  const FilterPass f = makeFilterPass(v);
  CHECK((f.bleach == 1.0f && f.threshold == 0.0f && f.cross == 0.0f && f.any));
  v.bleachBypass = 40.0f;
  v.threshold = 25.0f;
  v.crossProcess = 75.0f;
  const FilterPass g = makeFilterPass(v);
  CHECK(g.bleach == within(0.4f, 1e-6));
  CHECK(g.threshold == within(0.25f, 1e-6));
  CHECK(g.cross == within(0.75f, 1e-6));
  CHECK((g.grey == 0.0f && g.solarize == 0.0f && g.posterLevels == 0)); // the others stay off
  // Each one alone turns the block on, and only itself.
  for (float FilterValues::*field : {&FilterValues::bleachBypass, &FilterValues::threshold, &FilterValues::crossProcess})
  {
    FilterValues one1;
    one1.*field = 1.0f;
    CHECK(makeFilterPass(one1).any);
  }
}

TEST_CASE("color_pass: bleachBypassFilter", "[color_pass]")
{
  FilterValues f;
  f.bleachBypass = 100.0f;
  const ColorPass cp = filterPass(f);
  // Black and white map to themselves.
  CHECK((run(cp, one(0, 0, 0)) == one(0, 0, 0) && run(cp, one(255, 255, 255)) == one(255, 255, 255)));
  // More contrast: dark grays go darker, light grays go lighter, and grays stay gray.
  CHECK((run(cp, one(64, 64, 64))[0] < 64 && run(cp, one(192, 192, 192))[0] > 192));
  for (int v = 0; v <= 255; v += 5)
  {
    const Px out = run(cp, one(v, v, v));
    CHECK((std::abs(out[0] - out[1]) <= 1 && std::abs(out[1] - out[2]) <= 1));
  }
  // Colors lose much of their color (an orange-red keeps well under half its spread).
  const Px orange = one(200, 60, 30);
  CHECK(spread(run(cp, orange)) * 2 < spread(orange));
  // Half strength lands halfway between the pixel and the full effect.
  f.bleachBypass = 50.0f;
  const Px full = run(cp, orange), half = run(filterPass(f), orange);
  for (size_t c = 0; c < 3; ++c)
    CHECK(std::abs(half[c] - (orange[c] + full[c]) / 2) <= 1);
  CHECK(half[3] == 255);
}

TEST_CASE("color_pass: thresholdFilter", "[color_pass]")
{
  FilterValues f;
  f.threshold = 100.0f;
  const ColorPass cp = filterPass(f);
  // Gray: black below the cutoff, white above (the few levels around it are left out: the float luma sits on the line).
  for (int v = 0; v < 256; ++v)
  {
    if (v >= 126 && v <= 130)
      continue;
    const Px out = run(cp, one(v, v, v));
    CHECK((out[0] == (v > 128 ? 255 : 0) && out[1] == out[0] && out[2] == out[0]));
  }
  // It goes by luma, not by the brightest channel: yellow is bright, pure red and blue are dark.
  CHECK(run(cp, one(255, 255, 0)) == one(255, 255, 255));
  CHECK(run(cp, one(255, 0, 0)) == one(0, 0, 0));
  CHECK(run(cp, one(0, 0, 255)) == one(0, 0, 0));
  // Half strength: halfway between the pixel and black or white.
  f.threshold = 50.0f;
  const Px dark = run(filterPass(f), one(100, 100, 100)), light = run(filterPass(f), one(200, 200, 200));
  CHECK((std::abs(dark[0] - 50) <= 1 && std::abs(light[0] - 228) <= 1));
  // It runs before Negative: Threshold + Negative is the inverted black and white.
  FilterValues both;
  both.threshold = 100.0f;
  both.negative = 100.0f;
  CHECK(run(filterPass(both), one(200, 200, 200)) == one(0, 0, 0));
  CHECK(run(filterPass(both), one(20, 20, 20)) == one(255, 255, 255));
}

TEST_CASE("color_pass: crossProcessFilter", "[color_pass]")
{
  FilterValues f;
  f.crossProcess = 100.0f;
  const ColorPass cp = filterPass(f);
  // Blacks go blue (B lifted to 32), whites go yellow (B pulled down to 225).
  const Px black = run(cp, one(0, 0, 0)), white = run(cp, one(255, 255, 255));
  CHECK((black[0] == 0 && black[1] == 0 && black[2] == 32));
  CHECK((white[0] == 255 && white[1] == 255 && std::abs(white[2] - 225) <= 1));
  // Mid-gray keeps its brightness (the S-curves cross at the middle).
  const Px mid = run(cp, one(128, 128, 128));
  CHECK((std::abs(mid[0] - 128) <= 1 && std::abs(mid[1] - 128) <= 1 && std::abs(mid[2] - 129) <= 1));
  // R has the strongest contrast, G a milder one, B none (it is a straight lift).
  const Px d = run(cp, one(64, 64, 64)), l = run(cp, one(192, 192, 192));
  CHECK((d[0] < d[1] && d[1] < 64 && l[0] > l[1] && l[1] > 192));
  // No channel ever runs backwards, and B stays inside 32..225.
  Px prev = run(cp, one(0, 0, 0));
  for (int v = 1; v < 256; ++v)
  {
    const Px out = run(cp, one(v, v, v));
    CHECK((out[0] >= prev[0] && out[1] >= prev[1] && out[2] >= prev[2]));
    CHECK((out[2] >= 32 && out[2] <= 226));
    prev = out;
  }
  // Half strength lifts the black's blue halfway.
  f.crossProcess = 50.0f;
  CHECK(std::abs(run(filterPass(f), one(0, 0, 0))[2] - 16) <= 1);
}

TEST_CASE("color_pass: filterOrderAndPlacement", "[color_pass]")
{
  // Greyscale runs before Sepia: both on equals Sepia applied to the greyscaled pixel (not Sepia alone).
  FilterValues both;
  both.greyscale = 100.0f;
  both.sepia = 100.0f;
  FilterValues g;
  g.greyscale = 100.0f;
  FilterValues s;
  s.sepia = 100.0f;
  const Px p = one(200, 60, 30);
  const Px viaBoth = run(filterPass(both), p);
  const Px stepwise = run(filterPass(s), run(filterPass(g), p));
  for (int c = 0; c < 3; ++c)
    CHECK(std::abs(viaBoth[static_cast<size_t>(c)] - stepwise[static_cast<size_t>(c)]) <= 1);
  CHECK(viaBoth != run(filterPass(s), p));

  // Filters run after Color Grading: lift + Negative = 255 - lifted, not (255 - x) + lift.
  ColorGrade grade;
  grade.wheel[static_cast<size_t>(GradeRange::Global)].lum = 50;
  FilterValues neg;
  neg.negative = 100.0f;
  const Px lifted = run(pass(0, 0, 0, 0, MixValues{}, grade), one(100, 100, 100));
  const Px both2 = run(pass(0, 0, 0, 0, MixValues{}, grade, 0, 0, 0, neg), one(100, 100, 100));
  CHECK((lifted[0] > 101 && std::abs(both2[0] - (255 - lifted[0])) <= 1));
}

TEST_CASE("color_pass: filterBufferHandling", "[color_pass]")
{
  FilterValues f;
  for (float FilterValues::*field : kFilterFields)
    f.*field = 60.0f;
  const ColorPass cp = filterPass(f);
  Px buf = {10, 200, 90, 17, 250, 3, 77, 201, 99, 98, 97, 96};
  const Px before = buf;
  applyColorPass(cp, buf.data(), 8);                                 // two pixels of three
  CHECK((buf[3] == 17 && buf[7] == 201));                              // alpha never touched
  CHECK(std::equal(buf.begin() + 8, buf.end(), before.begin() + 8)); // third pixel untouched
  CHECK(!std::equal(buf.begin(), buf.begin() + 3, before.begin()));
  // Out-of-range values arriving from earlier stages (here a big saturation boost) are fine.
  FilterValues all;
  for (float FilterValues::*field : kFilterFields)
    all.*field = 100.0f;
  const ColorPass hot = pass(80, -60, 100, 100, MixValues{}, ColorGrade{}, 40, 0, 0, all);
  Px a = before, b = before;
  applyColorPass(hot, a.data(), a.size());
  applyColorPass(hot, b.data(), b.size());
  CHECK((a == b && a[3] == 17 && a[7] == 201 && a[11] == 96));
}

TEST_CASE("color_pass: bufferHandling", "[color_pass]")
{
  // Alpha is never touched, whatever the stage; only the first n bytes are.
  MixValues m{};
  m[mixIndex(MixChan::Hue, 0)] = 50;
  ColorGrade g;
  g.wheel[0].lum = 40;
  const ColorPass all = pass(30, -20, 40, 30, m, g, 25, 90, 40);
  Px buf = {10, 200, 90, 17, 250, 3, 77, 201, 99, 98, 97, 96};
  const Px before = buf;
  applyColorPass(all, buf.data(), 8); // two pixels of three
  CHECK((buf[3] == 17 && buf[7] == 201));
  CHECK(std::equal(buf.begin() + 8, buf.end(), before.begin() + 8)); // third pixel untouched
  CHECK(!std::equal(buf.begin(), buf.begin() + 3, before.begin()));  // first one did change
  Px untouched = before;
  applyColorPass(all, untouched.data(), 0); // n = 0: nothing at all
  CHECK(untouched == before);
  // Deterministic.
  Px a = before, b = before;
  applyColorPass(all, a.data(), a.size());
  applyColorPass(all, b.data(), b.size());
  CHECK(a == b);
}

// Runs the picker's answer through the real white-balance gains and returns the
// largest gap between the three output channels (0 = perfectly gray).
static int grayGapAfterPick(const WbPick &w, int r, int g, int b)
{
  const Px out = run(pass(w.temperature, w.tint), one(r, g, b));
  const int hi = std::max({int(out[0]), int(out[1]), int(out[2])});
  const int lo = std::min({int(out[0]), int(out[1]), int(out[2])});
  return hi - lo;
}

TEST_CASE("color_pass: whiteBalancePicker", "[color_pass]")
{
  // A warm gray (R > B) needs a cooler Temperature; a spot that is already neutral needs nothing.
  const WbPick warm = solveWhiteBalance(180, 150, 120);
  CHECK((warm.status == WbPickStatus::Ok && warm.usable()));
  CHECK((warm.temperature == -67 && warm.tint == 14));
  CHECK(grayGapAfterPick(warm, 180, 150, 120) <= 2);

  const WbPick neutral = solveWhiteBalance(128, 128, 128);
  CHECK((neutral.status == WbPickStatus::Ok && neutral.temperature == 0 && neutral.tint == 0));

  // Directions: a blue cast warms, a green cast adds magenta (positive Tint), a magenta cast adds green.
  CHECK(solveWhiteBalance(100, 128, 160).temperature > 0);
  CHECK(solveWhiteBalance(120, 150, 120).tint > 0);
  CHECK(solveWhiteBalance(140, 110, 140).tint < 0);

  // Round trip over a grid of mild casts: after the picked gains the spot is gray to within rounding.
  int solved = 0;
  for (int r = 60; r <= 220; r += 20)
    for (int g = 60; g <= 220; g += 20)
      for (int b = 60; b <= 220; b += 20)
      {
        const WbPick w = solveWhiteBalance(static_cast<float>(r), static_cast<float>(g), static_cast<float>(b));
        CHECK((w.status != WbPickStatus::TooDark && w.status != WbPickStatus::Clipped));
        CHECK((w.temperature >= -100 && w.temperature <= 100 && w.tint >= -100 && w.tint <= 100));
        if (w.status == WbPickStatus::Ok)
        {
          ++solved;
          CHECK(grayGapAfterPick(w, r, g, b) <= 3);
        }
      }
  CHECK(solved > 100); // plenty of the grid is reachable, so the check above really ran

  // Too colored to neutralize with +/-100: still usable, flagged, and clamped to the slider range.
  const WbPick extreme = solveWhiteBalance(230, 60, 20);
  CHECK((extreme.status == WbPickStatus::Limited && extreme.usable()));
  CHECK((std::fabs(extreme.temperature) == 100 || std::fabs(extreme.tint) == 100));
  CHECK(grayGapAfterPick(extreme, 230, 60, 20) < 230 - 20); // closer to gray than before

  // Unusable spots.
  CHECK(solveWhiteBalance(3, 3, 3).status == WbPickStatus::TooDark);
  CHECK(solveWhiteBalance(200, 200, 2).status == WbPickStatus::TooDark);
  CHECK(solveWhiteBalance(255, 255, 255).status == WbPickStatus::Clipped);
  CHECK(solveWhiteBalance(255, 200, 190).status == WbPickStatus::Clipped);
  CHECK((!solveWhiteBalance(255, 255, 255).usable() && !solveWhiteBalance(3, 3, 3).usable()));
  CHECK(solveWhiteBalance(std::nanf(""), 100, 100).status == WbPickStatus::TooDark);
  CHECK(solveWhiteBalance(100, INFINITY, 100).status == WbPickStatus::TooDark);
  // The boundaries themselves: just inside the readable range is fine.
  CHECK(solveWhiteBalance(kWbMinLevel, kWbMinLevel, kWbMinLevel).usable());
  CHECK(solveWhiteBalance(kWbMaxLevel - 1, kWbMaxLevel - 1, kWbMaxLevel - 1).usable());
}

// ------------------------------------------------------------------- 3D LUT
// A 2-point LUT that inverts every channel (an exact table for tetrahedral lookups).
static std::shared_ptr<const Lut3D> invertLut()
{
  Lut3D L = identityLut(2);
  for (float &v : L.rgb)
    v = 255.0f - v;
  return std::make_shared<const Lut3D>(std::move(L));
}
static LutPass lutPass(std::shared_ptr<const Lut3D> t, float amount) { return makeLutPass(std::move(t), amount); }
static ColorPass lutOnly(std::shared_ptr<const Lut3D> t, float amount)
{
  return pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, FilterValues{}, lutPass(std::move(t), amount));
}

TEST_CASE("color_pass: lutActiveFlag", "[color_pass]")
{
  const auto inv = invertLut();
  CHECK((lutOnly(inv, 100.0f).active && lutOnly(inv, 100.0f).lut.any()));
  CHECK(lutOnly(inv, 1.0f).active);
  // Nothing in play: no table, amount 0, a table that isn't valid. The pass stays off, so the fast path skips it.
  CHECK((!lutOnly(nullptr, 100.0f).active && !lutOnly(inv, 0.0f).active && !lutOnly(inv, -5.0f).active));
  CHECK(!lutOnly(std::make_shared<const Lut3D>(Lut3D{}), 100.0f).active);
  CHECK((!pass().lut.any() && pass().lut.lut == nullptr));
  // The other stages' flags are untouched by a LUT.
  const ColorPass cp = lutOnly(inv, 100.0f);
  CHECK((!cp.mix && !cp.grade && !cp.hue && !cp.colorize && !cp.filters.any));
  // Copying a pass shares the table (a RenderParams copy per slider frame stays cheap).
  const ColorPass copy = cp;
  CHECK((copy.lut.lut == cp.lut.lut && copy.lut.amount == 1.0f));
}

TEST_CASE("color_pass: lutIdentityChangesNothing", "[color_pass]")
{
  const ColorPass cp = lutOnly(std::make_shared<const Lut3D>(identityLut(33)), 100.0f);
  std::mt19937 rng(7);
  Px img(4 * 4000);
  for (auto &v : img)
    v = static_cast<uint8_t>(rng() & 255);
  Px want = img;
  const Px got = run(cp, img);
  for (size_t i = 0; i < got.size(); ++i)
  {
    if (i % 4 == 3)
      CHECK(got[i] == want[i]); // alpha is never touched
    else
      CHECK(std::abs(int(got[i]) - int(want[i])) <= 1);
  }
}

TEST_CASE("color_pass: lutAppliesAndBlends", "[color_pass]")
{
  const auto inv = invertLut();
  CHECK(run(lutOnly(inv, 100.0f), one(10, 128, 250, 77)) == one(245, 127, 5, 77));
  CHECK(run(lutOnly(inv, 100.0f), one(0, 0, 0)) == one(255, 255, 255));
  CHECK(run(lutOnly(inv, 100.0f), one(255, 255, 255)) == one(0, 0, 0));
  // Amount blends between the picture and the table's answer.
  {
    const Px h = run(lutOnly(inv, 50.0f), one(0, 100, 200)); // every channel lands on 127.5: 127 or 128 after float rounding
    for (int k = 0; k < 3; ++k)
      CHECK((h[static_cast<size_t>(k)] == 127 || h[static_cast<size_t>(k)] == 128));
  }
  const Px a = run(lutOnly(inv, 25.0f), one(0, 0, 0)), b = run(lutOnly(inv, 75.0f), one(0, 0, 0));
  CHECK((a[0] == 64 && b[0] == 191 && a[0] < b[0]));
  // Amount 0 is the untouched picture, byte for byte.
  CHECK(run(lutOnly(inv, 0.0f), one(12, 34, 56, 78)) == one(12, 34, 56, 78));
  // A transparent pixel keeps its alpha (the empty corners of a straightened picture).
  CHECK(run(lutOnly(inv, 100.0f), one(10, 20, 30, 0))[3] == 0);
  // Many pixels, one call: each is looked up on its own.
  Px two = {0, 0, 0, 255, 255, 255, 255, 255};
  applyColorPass(lutOnly(inv, 100.0f), two.data(), two.size());
  CHECK(two == (Px{255, 255, 255, 255, 0, 0, 0, 255}));
}

TEST_CASE("color_pass: lutRunsLast", "[color_pass]")
{
  const auto inv = invertLut();
  // After the Filters: Negative followed by an inverting LUT is the original picture.
  FilterValues neg;
  neg.negative = 100.0f;
  const ColorPass both = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, neg, lutPass(inv, 100.0f));
  for (int v : {0, 30, 128, 200, 255})
  {
    const Px got = run(both, one(v, 255 - v, v / 2));
    CHECK((std::abs(int(got[0]) - v) <= 1 && std::abs(int(got[1]) - (255 - v)) <= 1 && std::abs(int(got[2]) - v / 2) <= 1));
  }
  // Greyscale, then the LUT: the LUT sees the grey picture.
  FilterValues grey;
  grey.greyscale = 100.0f;
  const ColorPass gl = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, grey, lutPass(inv, 100.0f));
  const Px g = run(filterPass(grey), one(200, 40, 90));
  const Px o = run(gl, one(200, 40, 90));
  for (int k = 0; k < 3; ++k)
    CHECK(std::abs(int(o[static_cast<size_t>(k)]) - (255 - int(g[static_cast<size_t>(k)]))) <= 1);
  // After the other stages too: a warm white balance pushes red to the ceiling (255), and that is
  // what the LUT is handed, never a value above it.
  const Px warm = run(pass(100), one(250, 200, 100));
  CHECK(warm[0] == 255);
  const ColorPass wl = pass(100, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, FilterValues{}, lutPass(inv, 100.0f));
  CHECK(run(wl, one(250, 200, 100))[0] == 0);
}

TEST_CASE("color_pass: colorPassWithLut", "[color_pass]")
{
  const auto inv = invertLut();
  const LutPass on = lutPass(inv, 100.0f);
  // On a pass with nothing else in it, it is exactly the pass makeColorPass makes for a LUT alone.
  const ColorPass viaHelper = colorPassWithLut(pass(), on);
  const ColorPass direct = lutOnly(inv, 100.0f);
  CHECK((viaHelper.active && viaHelper.lut.lut == inv && viaHelper.lut.amount == 1.0f));
  Px a = {10, 130, 250, 255, 77, 77, 77, 9}, b = a;
  applyColorPass(viaHelper, a.data(), a.size());
  applyColorPass(direct, b.data(), b.size());
  CHECK(a == b);
  // On a pass that has other stages, those are untouched and the LUT is added (or swapped).
  const ColorPass busy = pass(20, -10, 30, 25);
  const ColorPass withIt = colorPassWithLut(busy, on);
  CHECK((withIt.active && withIt.lut.any() && withIt.gain[0] == busy.gain[0] && withIt.saturation == busy.saturation &&
        withIt.vibrance == busy.vibrance));
  const ColorPass made = pass(20, -10, 30, 25, MixValues{}, ColorGrade{}, 0, 0, 0, FilterValues{}, on);
  Px c = {200, 100, 50, 255, 33, 99, 240, 255}, d = c;
  applyColorPass(withIt, c.data(), c.size());
  applyColorPass(made, d.data(), d.size());
  CHECK(c == d); // the same bytes as a pass built with the LUT from the start
  // Swapping one LUT for another replaces it (never stacks), and the amount is the new pass's.
  const auto other = std::make_shared<const Lut3D>(identityLut(5));
  const ColorPass swapped = colorPassWithLut(withIt, lutPass(other, 40.0f));
  CHECK((swapped.lut.lut == other && swapped.lut.amount == 0.4f && swapped.active));
  // Removing it (an inactive LutPass): a pass that was only the LUT becomes inactive again;
  // one with other stages stays active.
  CHECK(!colorPassWithLut(pass(), LutPass{}).active);
  const ColorPass onlyLut = lutOnly(inv, 100.0f);
  const ColorPass removed = colorPassWithLut(onlyLut, LutPass{});
  CHECK(!removed.lut.any());
  CHECK((colorPassWithLut(withIt, LutPass{}).active && !colorPassWithLut(withIt, LutPass{}).lut.any()));
  // Stages other than the LUT keep their own flags: filters, mix, grade, hue, colorize.
  FilterValues f;
  f.sepia = 50.0f;
  CHECK(colorPassWithLut(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, f), on).filters.any);
  CHECK(colorPassWithLut(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 30), on).hue);
  // The source pass is never changed.
  CHECK((!busy.lut.any() && !pass().lut.any()));
}

// The float core on 8-bit values, rounded by hand, equals the byte entry point.
TEST_CASE("color_pass: colorPassFloatAgrees", "[color_pass]")
{
  std::mt19937 g(77);
  int active = 0;
  for (int round = 0; round < 100; ++round)
  {
    const ColorPass cp = randomPass(g);
    active += cp.active;
    const size_t px = 1 + g() % 50;
    std::vector<uint8_t> bytes(px * 4);
    std::vector<float> fl(px * 4);
    for (size_t i = 0; i < bytes.size(); ++i)
      fl[i] = static_cast<float>(bytes[i] = static_cast<uint8_t>(g()));
    applyColorPass(cp, bytes.data(), bytes.size());
    applyColorPassF(cp, fl.data(), fl.size());
    for (size_t i = 0; i < bytes.size(); ++i)
      CHECK(((i % 4 == 3) ? bytes[i] == static_cast<uint8_t>(fl[i]) : bytes[i] == toU8Clamped(fl[i])));
  }
  CHECK(active > 70); // the random passes really were active
  // Not clamped: a strong saturation boost leaves levels outside 0..255 for the caller to see.
  const ColorPass boost = makeColorPass(0, 0, 0, 100, MixValues{}, ColorGrade{});
  float px[4] = {250.0f, 20.0f, 20.0f, 255.0f};
  applyColorPassF(boost, px, 4);
  CHECK((px[0] > 255.0f || px[1] < 0.0f || px[2] < 0.0f));
  CHECK(px[3] == 255.0f); // alpha untouched
}

// Pixels are independent, so one big call (which works in 1024-pixel chunks) must give the same
// bytes as one call per pixel, whatever the buffer length; a trailing partial pixel is left alone.
TEST_CASE("color_pass: bulkEqualsPerPixel", "[color_pass]")
{
  std::mt19937 g(99);
  const size_t sizes[] = {1, 2, 17, 1023, 1024, 1025, 2047, 2048, 2049, 3000};
  int round = 0;
  for (int rep = 0; rep < 12; ++rep)
    for (size_t px : sizes)
    {
      const ColorPass cp = randomPass(g);
      const size_t extra = static_cast<size_t>(round++ % 3); // 0..2 stray bytes after the last pixel
      Px bulk(px * 4 + extra);
      for (uint8_t &v : bulk)
        v = static_cast<uint8_t>(g());
      if (round % 7 == 0) // extremes get their own share
        for (uint8_t &v : bulk)
          v = (g() & 1) ? 255 : 0;
      Px each = bulk;
      const Px original = bulk;
      applyColorPass(cp, bulk.data(), bulk.size());
      for (size_t i = 0; i < px; ++i)
        applyColorPass(cp, each.data() + i * 4, 4);
      CHECK(bulk == each);
      CHECK(std::equal(bulk.begin() + static_cast<std::ptrdiff_t>(px * 4), bulk.end(),
                       original.begin() + static_cast<std::ptrdiff_t>(px * 4))); // stray bytes untouched
      for (size_t i = 3; i < px * 4; i += 4)
        CHECK(bulk[i] == original[i]); // alpha untouched
    }
}

//// Golden fingerprints: a fixed-seed set of random passes over fixed random pixels, hashed.
//// Changing the pipeline on purpose changes these; update them then. Changing them by accident
//// is exactly what this test is for. The values depend on float behavior, so they are valid for
//// the compiler/flags you record them with (no -ffast-math; FMA contraction can change bytes).
//static std::uint64_t fnv1a(std::uint64_t h, const Px &v)
//{
//  for (uint8_t b : v)
//  {
//    h ^= b;
//    h *= 1099511628211ull;
//  }
//  return h;
//}
//TEST_CASE("color_pass: goldenFingerprints", "[color_pass][fingerprint]")
//{
//  constexpr int kGroups = 8, kPassesPerGroup = 8;
//  // Recorded from MSVC Release. If you change the pipeline on purpose, re-record them:
//  // set the entry to 0, run the test, and paste the printed value.
//  static const std::uint64_t kGolden[kGroups] = {
//      0x9e8e9e4de2178db1ull, 0x72264c1fb7b3b28eull, 0x1af4da7b2f08391eull, 0xb51a04329cd552a1ull,
//      0xaadea4010b57cebcull, 0x0c798b587c0529c3ull, 0x132cde7980658a07ull, 0x0555d21b5369cc6aull};
//  std::mt19937 g(2024);
//  for (int grp = 0; grp < kGroups; ++grp)
//  {
//    std::uint64_t h = 14695981039346656037ull;
//    for (int k = 0; k < kPassesPerGroup; ++k)
//    {
//      const ColorPass cp = randomPass(g);
//      Px img(4 * 997);
//      for (uint8_t &v : img)
//        v = static_cast<uint8_t>(g());
//      applyColorPass(cp, img.data(), img.size());
//      h = fnv1a(h, img);
//    }
//    if (kGolden[grp] == 0)
//    {
//      std::printf("GOLDEN group %d: 0x%016llxull\n", grp, static_cast<unsigned long long>(h));
//      CHECK(!"golden fingerprint not recorded yet");
//    }
//    else
//      CHECK(h == kGolden[grp]);
//  }
//}
} // namespace color_pass_cases
