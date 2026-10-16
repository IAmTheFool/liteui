// Build: g++ -std=c++20 -I.. color_pass_test.cpp -o color_pass_test && ./color_pass_test
#include "color_pass.hpp"
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
                      float hue = 0, float colorizeHue = 0, float colorizeAmount = 0)
{
  return makeColorPass(temperature, tint, vibrance, saturation, mix, grade, hue, colorizeHue, colorizeAmount);
}
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
  testBufferHandling();
  return finish();
}
