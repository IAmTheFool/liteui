// Build: g++ -std=c++20 -I.. color_pass_test.cpp -o color_pass_test && ./color_pass_test
#include "color/color_pass.hpp"
#include "check.hpp"

#include <random>
#include <vector>

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
                      const FilterValues &filters = FilterValues{})
{
  return makeColorPass(temperature, tint, vibrance, saturation, mix, grade, hue, colorizeHue, colorizeAmount, filters);
}
// A pass with only the given Filters set.
static ColorPass filterPass(const FilterValues &f) { return pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 0, 0, f); }
static constexpr float FilterValues::*kFilterFields[] = {&FilterValues::greyscale, &FilterValues::sepia, &FilterValues::vintage,
                                                          &FilterValues::fade, &FilterValues::negative, &FilterValues::posterize,
                                                          &FilterValues::solarize};
static int luma(const Px &p) { return static_cast<int>(std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2])); }

static void testActiveFlags()
{
  CHECK(!pass().active);
  CHECK(pass(10).active && !pass(10).mix && !pass(10).grade && !pass(10).hue && !pass(10).colorize);
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
  CHECK(!filterPass(FilterValues{}).active && !filterPass(FilterValues{}).filters.any);
  for (float FilterValues::*field : kFilterFields)
  {
    FilterValues f;
    f.*field = 40.0f;
    const ColorPass cp = filterPass(f);
    CHECK(cp.active && cp.filters.any);
  }
  // Posterize this low still rounds to 256 levels: no visible effect, so the pass stays off.
  FilterValues tiny;
  tiny.posterize = 0.01f;
  CHECK(!filterPass(tiny).active);
}

static void testNeutralPassChangesNothing()
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

static void testWhiteBalance()
{
  const ColorPass warm = pass(60), cool = pass(-60), magentaPass = pass(0, 70), greenPass = pass(0, -70);
  // Gains are normalized by their own luma: brightness is kept.
  for (const ColorPass *c : {&warm, &cool, &magentaPass, &greenPass})
    CHECK_NEAR(0.299f * c->gain[0] + 0.587f * c->gain[1] + 0.114f * c->gain[2], 1.0f, 1e-5);
  const Px mid = one(128, 128, 128);
  const Px w = run(warm, mid), c = run(cool, mid);
  CHECK(w[0] > 128 && w[2] < 128); // warmer: R up, B down
  CHECK(c[0] < 128 && c[2] > 128); // cooler: the opposite
  CHECK(std::abs(luma(w) - 128) <= 2 && std::abs(luma(c) - 128) <= 2);
  const Px magenta = run(magentaPass, mid), green = run(greenPass, mid);
  CHECK(magenta[1] < 128 && green[1] > 128); // + tint = more magenta (less green)
  CHECK(w[3] == 255 && c[3] == 255);
}

static void testGrayStaysGray()
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
      CHECK(std::abs(out[0] - out[1]) <= 1 && std::abs(out[1] - out[2]) <= 1);
      CHECK(std::abs(out[0] - v) <= 1);
    }
}

static void testSaturation()
{
  const ColorPass gray = pass(0, 0, 0, -100);
  const ColorPass more = pass(0, 0, 0, 50);
  std::mt19937 rng(5);
  for (int i = 0; i < 500; ++i)
  {
    const Px p = one(static_cast<int>(rng() % 256), static_cast<int>(rng() % 256), static_cast<int>(rng() % 256));
    const Px g = run(gray, p);
    CHECK(std::abs(g[0] - g[1]) <= 1 && std::abs(g[1] - g[2]) <= 1); // fully desaturated
    CHECK(std::abs(luma(g) - luma(p)) <= 2);                         // at the same brightness
    const Px m = run(more, p);
    const int spreadIn = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]});
    const int spreadOut = std::max({m[0], m[1], m[2]}) - std::min({m[0], m[1], m[2]});
    CHECK(spreadOut + 1 >= spreadIn); // never less colorful (clipping can only cap the growth)
  }
  // Saturation +100 doubles the distance from gray (before clipping).
  const Px dull = run(pass(0, 0, 0, 100), one(110, 100, 90));
  CHECK(dull[0] - dull[2] >= 38 && dull[0] - dull[2] <= 42);
}

static void testVibranceProtectsColorfulPixels()
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

static void testHueShift()
{
  const ColorPass cp = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 100);
  CHECK(cp.hue);
  // Rows of the rotation sum to 1: gray maps to gray (and luma is kept for pure rotations).
  for (int r = 0; r < 3; ++r)
    CHECK_NEAR(cp.hueM[r * 3] + cp.hueM[r * 3 + 1] + cp.hueM[r * 3 + 2], 1.0f, 1e-5);
  // Opposite shifts undo each other's direction: red goes toward yellow for +, toward magenta for -.
  const Px up = run(cp, one(200, 0, 0));
  const Px dn = run(pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, -100), one(200, 0, 0));
  CHECK(up[1] > up[2]); // green wins over blue
  CHECK(dn[2] > dn[1]); // blue wins over green
}

static void testColorize()
{
  const ColorPass cp = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 100);
  CHECK(cp.colorize);
  CHECK_NEAR(0.299f * cp.tintVec[0] + 0.587f * cp.tintVec[1] + 0.114f * cp.tintVec[2], 0.0f, 1e-3);
  const Px out = run(cp, one(128, 128, 128));
  CHECK(out[2] > out[0]); // hue 200 is a cyan-blue cast
  CHECK(std::abs(luma(out) - 128) <= 2);
  // Its strength follows the amount.
  const ColorPass half = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 50);
  CHECK_NEAR(half.tintVec[2], cp.tintVec[2] * 0.5f, 1e-3);
  // Hue is taken modulo 360, negatives included.
  const ColorPass a = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, 200, 100);
  const ColorPass b = pass(0, 0, 0, 0, MixValues{}, ColorGrade{}, 0, -160, 100);
  CHECK_NEAR(a.tintVec[0], b.tintVec[0], 1e-3);
  CHECK_NEAR(a.tintVec[1], b.tintVec[1], 1e-3);
  CHECK_NEAR(a.tintVec[2], b.tintVec[2], 1e-3);
}

static void testMixerAndGradeStages()
{
  // Red band at -100 saturation: a pure red loses its color, a pure blue keeps it.
  MixValues m{};
  m[mixIndex(MixChan::Saturation, static_cast<size_t>(MixBand::Red))] = -100;
  const ColorPass mp = pass(0, 0, 0, 0, m);
  const Px red = run(mp, one(255, 0, 0));
  const Px blue = run(mp, one(0, 0, 255));
  CHECK(std::abs(red[0] - red[1]) <= 2 && std::abs(red[1] - red[2]) <= 2);
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
  CHECK(lifted[0] == lifted[1] && lifted[1] == lifted[2]);
  // Shadows only: a bright pixel is untouched.
  ColorGrade s;
  s.wheel[static_cast<size_t>(GradeRange::Shadows)].lum = 100;
  CHECK(run(pass(0, 0, 0, 0, MixValues{}, s), one(250, 250, 250)) == one(250, 250, 250));
  CHECK(run(pass(0, 0, 0, 0, MixValues{}, s), one(10, 10, 10))[0] > 40);
}

// ---------------------------------------------------------------- Filters
static void testFilterValuesAreSanitized()
{
  FilterValues v;
  v.greyscale = 50.0f;
  v.sepia = 250.0f;   // clamped to 100
  v.vintage = -5.0f;  // clamped to 0
  v.fade = std::nanf("");
  v.negative = INFINITY;
  v.solarize = 100.0f;
  const FilterPass f = makeFilterPass(v);
  CHECK_NEAR(f.grey, 0.5f, 1e-6);
  CHECK(f.sepia == 1.0f && f.vintage == 0.0f && f.fade == 0.0f && f.negative == 0.0f && f.solarize == 1.0f);
  CHECK(f.any && f.posterLevels == 0);
  // Poster level count falls smoothly from 256 (off) to 2.
  FilterValues p;
  p.posterize = 100.0f;
  const FilterPass full = makeFilterPass(p);
  CHECK(full.posterLevels == 2 && full.any);
  CHECK_NEAR(full.posterScale, 1.0f / 255.0f, 1e-7);
  CHECK_NEAR(full.posterInv, 255.0f, 1e-3);
  p.posterize = 50.0f;
  CHECK(makeFilterPass(p).posterLevels == 23);
  int prev = 256;
  for (float s = 1.0f; s <= 100.0f; s += 1.0f)
  {
    p.posterize = s;
    const int n = makeFilterPass(p).posterLevels;
    CHECK(n >= 2 && n <= prev);
    prev = n;
  }
}

static void testFilterOnlyPassTouchesNoOtherStage()
{
  FilterValues f;
  f.greyscale = 100.0f;
  const ColorPass cp = filterPass(f);
  CHECK(cp.active && !cp.mix && !cp.grade && !cp.hue && !cp.colorize && cp.vibrance == 0.0f && cp.saturation == 1.0f);
  for (int c = 0; c < 3; ++c)
    CHECK_NEAR(cp.gain[c], 1.0f, 1e-6);
}

static void testGreyscaleFilter()
{
  FilterValues f;
  f.greyscale = 100.0f;
  const ColorPass cp = filterPass(f);
  std::mt19937 rng(11);
  for (int i = 0; i < 500; ++i)
  {
    const Px p = one(static_cast<int>(rng() % 256), static_cast<int>(rng() % 256), static_cast<int>(rng() % 256));
    const Px g = run(cp, p);
    CHECK(std::abs(g[0] - g[1]) <= 1 && std::abs(g[1] - g[2]) <= 1);
    CHECK(std::abs(luma(g) - luma(p)) <= 1);
  }
  // Half strength: halfway between the pixel and its luma (luma of 200,100,0 is 118.5).
  f.greyscale = 50.0f;
  const Px h = run(filterPass(f), one(200, 100, 0));
  CHECK(std::abs(h[0] - 159) <= 1 && std::abs(h[1] - 109) <= 1 && std::abs(h[2] - 59) <= 1);
}

static void testNegativeFilter()
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
    CHECK(out[0] == 127 || out[0] == 128);
  }
}

static void testPosterizeFilter()
{
  FilterValues f;
  f.posterize = 100.0f; // 2 levels: every channel ends at 0 or 255
  const ColorPass two = filterPass(f);
  for (int v = 0; v < 256; ++v)
  {
    const Px out = run(two, one(v, v, v));
    CHECK(out[0] == (v >= 128 ? 255 : 0) && out[1] == out[0] && out[2] == out[0]);
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
  CHECK(run(mid, one(0, 0, 0)) == one(0, 0, 0) && run(mid, one(255, 255, 255)) == one(255, 255, 255)); // the ends stay put
}

static void testSolarizeFilter()
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

static void testSepiaFadeVintageFilters()
{
  FilterValues f;
  f.sepia = 100.0f;
  const ColorPass sep = filterPass(f);
  const Px s = run(sep, one(128, 128, 128)); // the classic matrix: 1.351, 1.203, 0.937 times 128
  CHECK(std::abs(s[0] - 173) <= 1 && std::abs(s[1] - 154) <= 1 && std::abs(s[2] - 120) <= 1);
  CHECK(run(sep, one(0, 0, 0)) == one(0, 0, 0));
  const Px sw = run(sep, one(255, 255, 255));
  CHECK(sw[0] == 255 && sw[1] == 255 && std::abs(sw[2] - 239) <= 1);

  FilterValues fd;
  fd.fade = 100.0f; // blacks lifted to 40, white stays 255
  const ColorPass fade = filterPass(fd);
  CHECK(run(fade, one(0, 0, 0)) == one(40, 40, 40) && run(fade, one(255, 255, 255)) == one(255, 255, 255));
  CHECK(std::abs(run(fade, one(128, 128, 128))[0] - 148) <= 1);
  fd.fade = 50.0f;
  CHECK(std::abs(run(filterPass(fd), one(0, 0, 0))[0] - 20) <= 1);

  FilterValues fv;
  fv.vintage = 100.0f;
  const ColorPass vin = filterPass(fv);
  const Px black = run(vin, one(0, 0, 0)); // blacks lifted, warm cast: R > G > B > 0
  CHECK(black[0] > black[1] && black[1] > black[2] && black[2] > 0);
  const Px gray = run(vin, one(128, 128, 128));
  CHECK(gray[0] > gray[1] && gray[1] > gray[2]);
  const Px white = run(vin, one(255, 255, 255));
  CHECK(white[0] == 255 && white[2] < 255);
}

static void testFilterOrderAndPlacement()
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
  CHECK(lifted[0] > 101 && std::abs(both2[0] - (255 - lifted[0])) <= 1);
}

static void testFilterBufferHandling()
{
  FilterValues f;
  for (float FilterValues::*field : kFilterFields)
    f.*field = 60.0f;
  const ColorPass cp = filterPass(f);
  Px buf = {10, 200, 90, 17, 250, 3, 77, 201, 99, 98, 97, 96};
  const Px before = buf;
  applyColorPass(cp, buf.data(), 8); // two pixels of three
  CHECK(buf[3] == 17 && buf[7] == 201); // alpha never touched
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
  CHECK(a == b && a[3] == 17 && a[7] == 201 && a[11] == 96);
}

static void testBufferHandling()
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
  CHECK(buf[3] == 17 && buf[7] == 201);
  CHECK(std::equal(buf.begin() + 8, buf.end(), before.begin() + 8)); // third pixel untouched
  CHECK(!std::equal(buf.begin(), buf.begin() + 3, before.begin()));  // first one did change
  applyColorPass(all, buf.data(), 0);                                // n = 0: nothing
  // Deterministic.
  Px a = before, b = before;
  applyColorPass(all, a.data(), a.size());
  applyColorPass(all, b.data(), b.size());
  CHECK(a == b);
}

int main()
{
  testActiveFlags();
  testNeutralPassChangesNothing();
  testWhiteBalance();
  testGrayStaysGray();
  testSaturation();
  testVibranceProtectsColorfulPixels();
  testHueShift();
  testColorize();
  testMixerAndGradeStages();
  testFilterValuesAreSanitized();
  testFilterOnlyPassTouchesNoOtherStage();
  testGreyscaleFilter();
  testNegativeFilter();
  testPosterizeFilter();
  testSolarizeFilter();
  testSepiaFadeVintageFilters();
  testFilterOrderAndPlacement();
  testFilterBufferHandling();
  testBufferHandling();
  return finish();
}