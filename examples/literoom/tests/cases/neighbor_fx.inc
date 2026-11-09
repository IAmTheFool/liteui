// cases/neighbor_fx.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace neighbor_fx_cases
// so they cannot collide with other cases.
//
//
// Standalone test for neighbor_fx.hpp (no UI library needed).

#include "detail/neighbor_fx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace neighbor_fx_cases
{
using namespace neighbor_fx;

using Img = std::vector<uint8_t>;

struct Rng
{
  uint32_t s;
  explicit Rng(uint32_t seed) : s(seed * 2654435761u + 1) {}
  int next(int n) // 0..n-1
  {
    s = s * 1664525u + 1013904223u;
    return static_cast<int>((s >> 8) % static_cast<uint32_t>(n));
  }
};

static Img makeImg(int w, int h, uint8_t (*f)(int x, int y, int c))
{
  Img img(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        img[(static_cast<size_t>(y) * w + x) * 4 + c] = c == 3 ? 255 : f(x, y, c);
  return img;
}

// Runs apply() over a whole picture and returns the result.
static Img run(const Img &src, int w, int h, const Gains &g)
{
  clearCaches();
  Planes pl;
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  Img dst = src;
  apply(pl, g, src.data(), dst.data(), static_cast<size_t>(w) * h, 0, w);
  return dst;
}

static double stddev(const Img &im, int w, int x0, int x1, int y0, int y1, int ch)
{
  double sum = 0, sq = 0, n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
    {
      const double v = im[(static_cast<size_t>(y) * w + x) * 4 + ch];
      sum += v;
      sq += v * v;
      n += 1;
    }
  const double m = sum / n;
  return std::sqrt(std::max(0.0, sq / n - m * m));
}
static double meanOf(const Img &im, int w, int x0, int x1, int y0, int y1, int ch)
{
  double sum = 0, n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
    {
      sum += im[(static_cast<size_t>(y) * w + x) * 4 + ch];
      n += 1;
    }
  return sum / n;
}

// ---------------------------------------------------------------- general

TEST_CASE("neighbor_fx: zeroGainsDoNothing", "[neighbor_fx]")
{
  Rng r(1);
  const int w = 70, h = 50;
  Img src(static_cast<size_t>(w) * h * 4);
  for (auto &v : src)
    v = static_cast<uint8_t>(r.next(256));
  const Img out = run(src, w, h, Gains{});
  CHECK(out == src);
  CHECK((Gains{}.need() == 0 && !Gains{}.any()));
}

TEST_CASE("neighbor_fx: needBits", "[neighbor_fx]")
{
  CHECK(gainsFromValues(10, 0, 0).need() == (1u << dl::Texture));
  CHECK(gainsFromValues(0, 0, 0, 0, 30).need() == (1u << dl::Sharpness)); // NR reads Sharpness's plane
  CHECK(gainsFromValues(0, 0, 0, 0, 0, 30).need() == (1u << 3));
  CHECK(gainsFromValues(0, 0, 0, 50).need() == (1u << 4));
  CHECK(gainsFromValues(0, 0, 0, -50).need() == 0); // adding haze needs no map
  // Blur: slider 0..33 reads level 0, 34..66 levels 0+1, 67..100 levels 1+2.
  CHECK(gainsFromValues(0, 0, 0, 0, 0, 0, 20).need() == (1u << 5));
  CHECK(gainsFromValues(0, 0, 0, 0, 0, 0, 50).need() == (3u << 5));
  CHECK(gainsFromValues(0, 0, 0, 0, 0, 0, 100).need() == (6u << 5));
  // detail + the new effects add up
  const Gains g = gainsFromValues(5, 5, 5, 5, 5, 5, 5);
  CHECK(g.need() == (7u | (1u << 3) | (1u << 4) | (1u << 5)));
}

TEST_CASE("neighbor_fx: detailPartMatchesDetailLayer", "[neighbor_fx]")
{
  // With only Texture/Clarity/Sharpness set, apply() == detail_layer::addPixels.
  const int w = 64, h = 40;
  const Img src = makeImg(w, h, [](int x, int y, int c) -> uint8_t
                          { return static_cast<uint8_t>(((x / 9 + y / 7) & 1) * 100 + 40 + c * 5 + (x * y) % 13); });
  const Gains g = gainsFromValues(50, -30, 40);
  const Img a = run(src, w, h, g);
  dl::cache().clear();
  dl::Planes pl;
  CHECK(dl::acquire(pl, src.data(), w, h, g.detail.need(), nullptr, 0));
  Img b = src;
  dl::addPixels(pl, g.detail, src.data(), b.data(), static_cast<size_t>(w) * h, 0);
  CHECK(a == b);
}

// ---------------------------------------------------------- noise reduction

TEST_CASE("neighbor_fx: lumaNoiseReduction", "[neighbor_fx]")
{
  Rng r(2);
  const int w = 120, h = 60;
  Img src(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      const int base = x < w / 2 ? 60 : 180; // a hard edge in the middle
      const int n = r.next(17) - 8;          // luminance noise, +/-8
      for (int c = 0; c < 3; ++c)
        src[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<uint8_t>(base + n);
      src[(static_cast<size_t>(y) * w + x) * 4 + 3] = 255;
    }
  const double before = stddev(src, w, 5, 40, 5, h - 5, 1);
  const Img out = run(src, w, h, gainsFromValues(0, 0, 0, 0, 100));
  const double after = stddev(out, w, 5, 40, 5, h - 5, 1);
  CHECK(after < before * 0.5); // flat area: noise at least halved
  // ...while the edge keeps its height.
  const double lo = meanOf(out, w, w / 2 - 8, w / 2 - 3, 5, h - 5, 1);
  const double hi = meanOf(out, w, w / 2 + 3, w / 2 + 8, 5, h - 5, 1);
  CHECK(hi - lo > 0.9 * 120.0);
  // Strength scales: half the slider removes less than the full slider.
  const Img half = run(src, w, h, gainsFromValues(0, 0, 0, 0, 40));
  const double mid = stddev(half, w, 5, 40, 5, h - 5, 1);
  CHECK((mid < before && mid > after));
  // All three channels move together (no hue shift).
  for (size_t i = 0; i < out.size(); i += 4)
    CHECK((out[i] == out[i + 1] && out[i + 1] == out[i + 2]));
}

TEST_CASE("neighbor_fx: colorNoiseReduction", "[neighbor_fx]")
{
  Rng r(3);
  const int w = 120, h = 60;
  Img src(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &src[(static_cast<size_t>(y) * w + x) * 4];
      if (x < w / 2)
      { // left: gray with chroma noise at constant luma
        const int R = 128 + r.next(21) - 10, B = 128 + r.next(21) - 10;
        const int G = std::clamp((128 * 256 - 54 * R - 19 * B) / 183, 0, 255);
        p[0] = static_cast<uint8_t>(R);
        p[1] = static_cast<uint8_t>(G);
        p[2] = static_cast<uint8_t>(B);
      }
      else
      { // right half: a clean, hard color edge (red | blue) of similar luma
        const bool left = x < w * 3 / 4;
        p[0] = left ? 200 : 60;
        p[1] = 60;
        p[2] = left ? 60 : 200;
      }
      p[3] = 255;
    }
  auto chromaR = [&](const Img &im, int x, int y)
  {
    const uint8_t *p = &im[(static_cast<size_t>(y) * w + x) * 4];
    return static_cast<int>(p[0]) - dl::lumaOf(p);
  };
  auto chromaSd = [&](const Img &im)
  {
    double s = 0, q = 0, n = 0;
    for (int y = 5; y < h - 5; ++y)
      for (int x = 5; x < w / 2 - 5; ++x)
      {
        const double v = chromaR(im, x, y);
        s += v;
        q += v * v;
        n += 1;
      }
    const double m = s / n;
    return std::sqrt(std::max(0.0, q / n - m * m));
  };
  const Img out = run(src, w, h, gainsFromValues(0, 0, 0, 0, 0, 100));
  CHECK(chromaSd(out) < chromaSd(src) * 0.4); // blotches mostly gone
  // Luma is not touched (up to 8-bit rounding of three channels).
  int maxLuma = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      maxLuma = std::max(maxLuma, std::abs(dl::lumaOf(&out[(static_cast<size_t>(y) * w + x) * 4]) -
                                           dl::lumaOf(&src[(static_cast<size_t>(y) * w + x) * 4])));
  CHECK(maxLuma <= 3);
  // The red | blue edge keeps most of its color difference: sample a few
  // pixels away on both sides.
  const int ex = w * 3 / 4;
  const int before = chromaR(src, ex - 3, h / 2) - chromaR(src, ex + 3, h / 2);
  const int after = chromaR(out, ex - 3, h / 2) - chromaR(out, ex + 3, h / 2);
  CHECK(after > before * 0.8);
}

// ------------------------------------------------------------------- dehaze

TEST_CASE("neighbor_fx: dehaze", "[neighbor_fx]")
{
  // A haze-free scene with many near-black pixels (so the dark channel prior
  // holds), a bright sky band on top, then veiled with I = J t + A (1 - t).
  Rng r(4);
  const int w = 600, h = 400, skyRows = 80;
  const float tTrue = 0.55f, Atrue = 235.0f;
  Img clear(static_cast<size_t>(w) * h * 4), hazy = clear;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
      {
        const size_t i = (static_cast<size_t>(y) * w + x) * 4 + c;
        const int J = c == 3 ? 255 : (y < skyRows ? 250 : r.next(256));
        clear[i] = static_cast<uint8_t>(J);
        hazy[i] = c == 3 ? 255 : static_cast<uint8_t>(std::lround(J * tTrue + Atrue * (1.0f - tTrue)));
      }
  const Img out = run(hazy, w, h, gainsFromValues(0, 0, 0, 100));
  auto mae = [&](const Img &a, const Img &b) // ground region only
  {
    double s = 0, n = 0;
    for (int y = skyRows + 10; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int c = 0; c < 3; ++c)
        {
          s += std::abs(static_cast<int>(a[(static_cast<size_t>(y) * w + x) * 4 + c]) -
                        b[(static_cast<size_t>(y) * w + x) * 4 + c]);
          n += 1;
        }
    return s / n;
  };
  const double errHazy = mae(hazy, clear), errOut = mae(out, clear);
  std::printf("  dehaze: error vs. clear scene %.1f -> %.1f\n", errHazy, errOut);
  CHECK(errOut < errHazy * 0.6); // clearly closer to the true scene
  CHECK(stddev(out, w, 0, w, skyRows + 10, h, 1) > stddev(hazy, w, 0, w, skyRows + 10, h, 1) * 1.4);
  // Half strength lands between none and full.
  const Img half = run(hazy, w, h, gainsFromValues(0, 0, 0, 50));
  const double errHalf = mae(half, clear);
  CHECK((errHalf < errHazy && errHalf > errOut));
  // Negative adds haze: contrast drops, mean moves toward the light gray.
  const Img fog = run(clear, w, h, gainsFromValues(0, 0, 0, -100));
  CHECK(stddev(fog, w, 0, w, skyRows + 10, h, 1) < stddev(clear, w, 0, w, skyRows + 10, h, 1) * 0.6);
  CHECK(meanOf(fog, w, 0, w, skyRows + 10, h, 1) > meanOf(clear, w, 0, w, skyRows + 10, h, 1));
  // A picture with no haze at all (dark channel ~ 0 everywhere) is nearly
  // untouched, apart from the tone change dehaze is allowed to make.
  const Img none = run(clear, w, h, gainsFromValues(0, 0, 0, 100));
  CHECK(mae(none, clear) < 25.0);
}

// --------------------------------------------------------------------- blur

TEST_CASE("neighbor_fx: blur", "[neighbor_fx]")
{
  Rng r(5);
  const int w = 300, h = 200;
  Img src(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        src[(static_cast<size_t>(y) * w + x) * 4 + c] = c == 3 ? 255 : static_cast<uint8_t>(r.next(256));
  const double s0 = stddev(src, w, 0, w, 0, h, 1);
  double prev = s0;
  for (int v : {20, 50, 100})
  {
    const Img out = run(src, w, h, gainsFromValues(0, 0, 0, 0, 0, 0, static_cast<float>(v)));
    const double sd = stddev(out, w, 20, w - 20, 20, h - 20, 1);
    std::printf("  blur %3d: stddev %.1f -> %.1f\n", v, s0, sd);
    CHECK(sd < prev); // monotone: more slider, more blur
    prev = sd;
    // Mean brightness is preserved (a blur moves detail, not light).
    CHECK(std::fabs(meanOf(out, w, 20, w - 20, 20, h - 20, 1) - meanOf(src, w, 20, w - 20, 20, h - 20, 1)) < 2.0);
  }
  CHECK(prev < s0 * 0.1); // slider 100: nearly flat
  // Flat picture: untouched. Slider 0: untouched.
  const Img flat = makeImg(50, 40, [](int, int, int) -> uint8_t
                           { return 90; });
  CHECK(run(flat, 50, 40, gainsFromValues(0, 0, 0, 0, 0, 0, 100)) == flat);
  CHECK(run(src, w, h, gainsFromValues(0, 0, 0, 0, 0, 0, 0)) == src);
  // The 3 stored levels join smoothly: just below and above a level boundary
  // (slider 33.3 / 66.7) look the same.
  const Img a = run(src, w, h, gainsFromValues(0, 0, 0, 0, 0, 0, 33.2f));
  const Img b = run(src, w, h, gainsFromValues(0, 0, 0, 0, 0, 0, 33.5f));
  double diff = 0;
  for (size_t i = 0; i < a.size(); ++i)
    diff += std::abs(static_cast<int>(a[i]) - b[i]);
  CHECK(diff / static_cast<double>(a.size()) < 1.5);
  // Color blur: a red | blue edge of equal luma becomes soft, not stay hard
  // (a luma-only blur would leave it hard).
  const Img edge = makeImg(200, 60, [](int x, int, int c) -> uint8_t
                           { const bool l = x < 100; return c == 0 ? (l ? 220 : 40) : (c == 1 ? 60 : (l ? 40 : 220)); });
  const Img eo = run(edge, 200, 60, gainsFromValues(0, 0, 0, 0, 0, 0, 100));
  const int redJump = std::abs(static_cast<int>(eo[(30 * 200 + 101) * 4]) - eo[(30 * 200 + 98) * 4]);
  CHECK(redJump < 60); // was 180
}

// ---------------------------------------------- caches, cancel, odd sizes

TEST_CASE("neighbor_fx: cachesAndCancel", "[neighbor_fx]")
{
  clearCaches();
  const int w = 200, h = 150;
  Rng r(6);
  Img src(static_cast<size_t>(w) * h * 4);
  for (auto &v : src)
    v = static_cast<uint8_t>(r.next(256));
  const Gains g = gainsFromValues(0, 0, 0, 50, 50, 50, 50);
  Planes a, b;
  CHECK(acquire(a, src.data(), w, h, g.need(), nullptr, 0));
  CHECK(acquire(b, src.data(), w, h, g.need(), nullptr, 0));
  CHECK((a.chroma[0] && a.chroma[0] == b.chroma[0] && a.chroma[1] == b.chroma[1]));
  CHECK((a.veil && a.veil == b.veil));
  CHECK((a.blur[0] && a.blur[0] == b.blur[0] && a.blur[1] == b.blur[1]));
  CHECK(!a.blur[2]); // slider 50 reads levels 0 and 1 only
  CHECK(a.luma.p[dl::Sharpness] == b.luma.p[dl::Sharpness]);
  // A slider drag (different gains, same need) builds nothing new.
  const size_t n1 = chromaStore().size() + mapStore().size();
  Planes c;
  CHECK(acquire(c, src.data(), w, h, gainsFromValues(0, 0, 0, 80, 20, 70, 60).need(), nullptr, 0));
  CHECK(chromaStore().size() + mapStore().size() == n1);

  // Cancelled build: returns false and caches nothing.
  clearCaches();
  std::atomic<uint64_t> latest{2};
  Planes d;
  CHECK(!acquire(d, src.data(), w, h, g.need(), &latest, 1));
  CHECK((chromaStore().size() == 0 && mapStore().size() == 0 && dl::cache().size() == 0));
  Planes e;
  CHECK(acquire(e, src.data(), w, h, g.need(), &latest, 2));
  clearCaches();
}

TEST_CASE("neighbor_fx: tinyAndOddSizes", "[neighbor_fx]")
{
  Rng r(7);
  const Gains all = gainsFromValues(40, 40, 40, 60, 60, 60, 100);
  const Gains haze = gainsFromValues(0, 0, 0, 100);
  for (auto sz : {std::pair{1, 1}, std::pair{1, 9}, std::pair{9, 1}, std::pair{2, 2},
                  std::pair{3, 2}, std::pair{17, 5}, std::pair{500, 3}, std::pair{5, 500}})
  {
    Img src(static_cast<size_t>(sz.first) * sz.second * 4);
    for (auto &v : src)
      v = static_cast<uint8_t>(r.next(256));
    const Img o1 = run(src, sz.first, sz.second, all);
    const Img o2 = run(src, sz.first, sz.second, haze);
    CHECK((o1.size() == src.size() && o2.size() == src.size()));
    for (size_t i = 3; i < src.size(); i += 4)
      CHECK((o1[i] == src[i] && o2[i] == src[i])); // alpha untouched
  }
}

TEST_CASE("neighbor_fx: partialRangesMatchWhole", "[neighbor_fx]")
{
  // The render loop and the masks call apply() on pieces of the picture
  // (row blocks, row spans). Pieces must give exactly the whole-picture result.
  Rng r(8);
  const int w = 97, h = 61;
  Img src(static_cast<size_t>(w) * h * 4);
  for (auto &v : src)
    v = static_cast<uint8_t>(r.next(256));
  const Gains g = gainsFromValues(30, 20, 30, 60, 40, 40, 70);
  clearCaches();
  Planes pl;
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  Img whole = src, pieces = src;
  apply(pl, g, src.data(), whole.data(), static_cast<size_t>(w) * h, 0, w);
  // uneven pieces that start and end mid-row
  size_t pos = 0;
  const size_t total = static_cast<size_t>(w) * h;
  const size_t steps[] = {1, 96, 97, 300, 7, 1000};
  size_t si = 0;
  while (pos < total)
  {
    const size_t n = std::min(steps[si++ % 6], total - pos);
    apply(pl, g, src.data() + pos * 4, pieces.data() + pos * 4, n, pos, w);
    pos += n;
  }
  CHECK(whole == pieces);
  clearCaches();
}

TEST_CASE("neighbor_fx: threads", "[neighbor_fx][threaded]")
{
  clearCaches();
  const int w = 240, h = 160;
  Rng r(9);
  Img src(static_cast<size_t>(w) * h * 4);
  for (auto &v : src)
    v = static_cast<uint8_t>(r.next(256));
  const Gains g = gainsFromValues(10, 10, 10, 50, 50, 50, 90);
  std::vector<std::thread> ts;
  std::vector<Img> outs(6);
  for (int t = 0; t < 6; ++t)
    ts.emplace_back([&, t]
                    {
      Planes pl;
      if (!acquire(pl, src.data(), w, h, g.need(), nullptr, 0))
        return;
      Img dst = src;
      apply(pl, g, src.data(), dst.data(), static_cast<size_t>(w) * h, 0, w);
      outs[static_cast<size_t>(t)] = std::move(dst); });
  for (auto &t : ts)
    t.join();
  for (int t = 1; t < 6; ++t)
    CHECK((!outs[0].empty() && outs[0] == outs[static_cast<size_t>(t)]));
  clearCaches();
}

// A busy test picture: gradients, blocks and noise, so every effect has something to do.
static Img scene(int w, int h, uint32_t seed)
{
  Rng r(seed);
  Img p(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *q = &p[(static_cast<size_t>(y) * w + x) * 4];
      const bool blk = ((x / 17) + (y / 13)) % 2 == 0;
      q[0] = static_cast<uint8_t>(std::min(255, (x * 255) / std::max(1, w - 1) / 2 + (blk ? 60 : 0) + r.next(24)));
      q[1] = static_cast<uint8_t>(std::min(255, (y * 255) / std::max(1, h - 1) / 2 + (blk ? 0 : 70) + r.next(24)));
      q[2] = static_cast<uint8_t>(60 + r.next(120));
      q[3] = 255;
    }
  return p;
}

// Random slider values: mode 0 = every effect on, 1 = detail layers + negative dehaze (needs no
// map), 2 = one effect at a time.
static Gains randomGains(Rng &r, int mode)
{
  auto v = [&](int lo, int hi)
  { return static_cast<float>(lo + r.next(hi - lo + 1)); };
  float s[7] = {0, 0, 0, 0, 0, 0, 0}; // texture, clarity, sharpness, dehaze, nr luma, nr color, blur
  if (mode == 0)
  {
    s[0] = v(-100, 100);
    s[1] = v(-100, 100);
    for (int i = 2; i < 7; ++i)
      s[i] = v(10, 100);
  }
  else if (mode == 1)
  {
    s[0] = v(-100, 100);
    s[1] = v(-100, 100);
    s[2] = v(10, 100);
    s[3] = v(-100, -10);
  }
  else
    s[static_cast<size_t>(r.next(7))] = v(30, 100);
  return gainsFromValues(s[0], s[1], s[2], s[3], s[4], s[5], s[6]);
}

// The float instantiation of apply() must agree with the byte one to within the rounding the byte
// passes do after every pass, leave alpha alone, stay inside 0..255, and really change the picture.
TEST_CASE("neighbor_fx: floatAgreesWithBytes", "[neighbor_fx]")
{
  uint32_t seed = 100;
  for (int mode = 0; mode < 3; ++mode)
    for (const auto &sz : {std::pair{37, 29}, std::pair{160, 120}, std::pair{301, 97}})
      for (int rep = 0; rep < (mode == 2 ? 6 : 2); ++rep, ++seed)
      {
        const int w = sz.first, h = sz.second;
        Rng r(seed);
        const Img src = scene(w, h, seed);
        const Gains g = randomGains(r, mode);
        clearCaches();
        Planes pl;
        CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
        // The picture after "tone and color": the source nudged, so dst differs from src.
        Img bytes = src;
        for (size_t i = 0; i < bytes.size(); i += 4)
          for (int c = 0; c < 3; ++c)
            bytes[i + c] = static_cast<uint8_t>(std::min(255, bytes[i + c] + 9));
        const Img nudged = bytes;
        std::vector<float> fl(bytes.begin(), bytes.end());
        const size_t total = static_cast<size_t>(w) * h;
        apply(pl, g, src.data(), bytes.data(), total, 0, w);
        apply(pl, g, src.data(), fl.data(), total, 0, w);
        int worst = 0;
        bool changed = false;
        for (size_t i = 0; i < bytes.size(); ++i)
        {
          if (i % 4 == 3)
          {
            CHECK((fl[i] == 255.0f && bytes[i] == 255)); // alpha untouched
            continue;
          }
          CHECK((fl[i] >= 0.0f && fl[i] <= 255.0f));
          worst = std::max(worst, static_cast<int>(std::lround(std::fabs(fl[i] - static_cast<float>(bytes[i])))));
          changed |= bytes[i] != nudged[i];
        }
        CHECK(worst <= 3); // at most the rounding of five byte passes
        CHECK(changed);    // the effects really did something
      }
  clearCaches();
}

// Pieces must give exactly the whole-picture result for floats too (the render loop and the masks
// work in row blocks and spans).
TEST_CASE("neighbor_fx: floatPartialRangesMatchWhole", "[neighbor_fx]")
{
  const int w = 97, h = 61;
  Rng r(31);
  const Img src = scene(w, h, 31);
  const Gains g = gainsFromValues(30, 20, 30, 60, 40, 40, 70);
  clearCaches();
  Planes pl;
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  std::vector<float> whole(src.begin(), src.end()), pieces = whole;
  const size_t total = static_cast<size_t>(w) * h;
  apply(pl, g, src.data(), whole.data(), total, 0, w);
  size_t pos = 0, si = 0;
  const size_t steps[] = {1, 96, 97, 300, 7, 1000};
  while (pos < total)
  {
    const size_t n = std::min(steps[si++ % 6], total - pos);
    apply(pl, g, src.data() + pos * 4, pieces.data() + pos * 4, n, pos, w);
    pos += n;
  }
  CHECK(whole == pieces);
  clearCaches();
}

// The float instantiation never rounds, but it does clamp to 0..255.
TEST_CASE("neighbor_fx: floatKeepsPrecisionAndClamps", "[neighbor_fx]")
{
  Planes none;
  // No rounding: a dehaze that adds haze to 100.0 gives the exact blend, which no byte could hold.
  const Gains hz = gainsFromValues(0, 0, 0, -50.0f);
  const uint8_t src1[4] = {100, 100, 100, 255};
  float px[4] = {100.0f, 100.0f, 100.0f, 255.0f};
  apply(none, hz, src1, px, 1, 0, 1);
  const float expect = 100.0f + (kHazeLight - 100.0f) * (kHazeAdd * 0.5f);
  CHECK(std::fabs(px[0] - expect) < 1e-4f);
  CHECK(px[0] != std::floor(px[0]));
  uint8_t b[4] = {100, 100, 100, 255};
  apply(none, hz, src1, b, 1, 0, 1);
  CHECK(b[0] == static_cast<uint8_t>(px[0] + 0.5f)); // the byte is the rounded float

  // Clamping: Sharpness 100 on a hard 20 | 240 step overshoots on both sides of the edge. The
  // float result must stop at exactly 0 and 255 (it is not rounded, but it is clamped).
  const int w = 80, h = 8;
  Img step(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *q = &step[(static_cast<size_t>(y) * w + x) * 4];
      q[0] = q[1] = q[2] = x < 40 ? 20 : 240;
      q[3] = 255;
    }
  const Gains sh = gainsFromValues(0, 0, 100);
  clearCaches();
  Planes pl;
  CHECK(acquire(pl, step.data(), w, h, sh.need(), nullptr, 0));
  std::vector<float> f(step.begin(), step.end());
  apply(pl, sh, step.data(), f.data(), static_cast<size_t>(w) * h, 0, w);
  float lo = 1e9f, hi = -1e9f;
  for (size_t i = 0; i < f.size(); ++i)
    if (i % 4 != 3)
    {
      lo = std::min(lo, f[i]);
      hi = std::max(hi, f[i]);
    }
  CHECK((lo == 0.0f && hi == 255.0f)); // it overshot, and was clamped to the ends
  CHECK((f[(4 * w + 39) * 4] == 0.0f && f[(4 * w + 40) * 4] == 255.0f));
  // Far from the edge nothing moved.
  CHECK((f[(4 * w + 2) * 4] == 20.0f && f[(4 * w + 77) * 4] == 240.0f));
  clearCaches();
}


} // namespace neighbor_fx_cases
