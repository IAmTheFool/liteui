// cl /std:c++20 /EHsc /O2 /W3 /Isrc tests\detail_layer_test.cpp /Fe:tests\detail_layer_test.exe /Fo:tests\detail_layer_test.obj
// tests\detail_layer_test.exe
// g++ -std=c++20 -O1 -Wall -Wextra -pthread -I src tests/detail_layer_test.cpp -o dl_test && ./dl_test
// (add -fsanitize=thread, or -fsanitize=address,undefined, for the sanitizer runs)
#include "detail/detail_layer.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

static int gFailures = 0;
#define CHECK(cond)                                                     \
  do                                                                    \
  {                                                                     \
    if (!(cond))                                                        \
    {                                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
      ++gFailures;                                                      \
    }                                                                   \
  } while (0)

using Img = std::vector<uint8_t>;

static Img makeImg(int w, int h, uint8_t (*f)(int x, int y, int c))
{
  Img img(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        img[(static_cast<size_t>(y) * w + x) * 4 + c] =
            c == 3 ? 255 : f(x, y, c);
  return img;
}

// Naive reference: the same 3 x (horizontal, vertical) box passes with
// replicated edges and rounding after every pass, written the obvious slow way.
static std::vector<int> refBlur(const Img &img, int w, int h, int r)
{
  std::vector<int> a(static_cast<size_t>(w) * h), b(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    a[i] = detail_layer::lumaOf(&img[i * 4]) << 4;
  const int n = 2 * r + 1;
  auto pass = [&](bool horiz)
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        long sum = 0;
        for (int k = -r; k <= r; ++k)
        {
          const int xx = horiz ? std::clamp(x + k, 0, w - 1) : x;
          const int yy = horiz ? y : std::clamp(y + k, 0, h - 1);
          sum += a[static_cast<size_t>(yy) * w + xx];
        }
        b[static_cast<size_t>(y) * w + x] =
            static_cast<int>(std::lround(static_cast<double>(sum) / n));
      }
    a.swap(b);
  };
  for (int i = 0; i < 3; ++i)
    pass(true);
  for (int i = 0; i < 3; ++i)
    pass(false);
  return a;
}

static uint8_t pattern(int x, int y, int c)
{
  // Edges, a gradient and some per-channel texture.
  const int v = ((x / 17 + y / 23) & 1) * 120 + (x * 3 + y * 2) % 90 + c * 7;
  return static_cast<uint8_t>(v & 255);
}

static void testBlurMatchesReference()
{
  const int w = 61, h = 47; // odd sizes, smaller than a big radius on purpose
  const Img img = makeImg(w, h, pattern);
  for (int r : {1, 2, 5, 30, 80}) // 80 > both dimensions: all-edge case
  {
    detail_layer::Plane p;
    CHECK(detail_layer::blurLuma(img.data(), w, h, r, p, nullptr, 0));
    const auto ref = refBlur(img, w, h, r);
    int maxDiff = 0;
    for (size_t i = 0; i < ref.size(); ++i)
      maxDiff = std::max(maxDiff, std::abs(static_cast<int>(p[i]) - ref[i]));
    // Float rounding at exact .5 ties may differ by one unit (1/16 level).
    if (maxDiff > 1)
      std::printf("  radius %d: max diff %d\n", r, maxDiff);
    CHECK(maxDiff <= 1);
  }
}

static void testFlatImageStaysFlat()
{
  const Img img = makeImg(50, 40, [](int, int, int) -> uint8_t { return 100; });
  detail_layer::Plane p;
  CHECK(detail_layer::blurLuma(img.data(), 50, 40, 9, p, nullptr, 0));
  const int expect = detail_layer::lumaOf(&img[0]) << 4;
  for (uint16_t v : p)
    CHECK(v == expect); // replicated edges: nothing darkens at the border
}

static void testRadiusScalesWithSize()
{
  using namespace detail_layer;
  // Same fraction of the picture on the preview and on the full image.
  const int full = boxRadius(Clarity, 6000, 4000);
  const int prev = boxRadius(Clarity, 1000, 667);
  CHECK(full >= 5 * prev && full <= 7 * prev);
  CHECK(boxRadius(Texture, 6000, 4000) < full);
  CHECK(boxRadius(Sharpness, 6000, 4000) <= boxRadius(Texture, 6000, 4000));
  CHECK(boxRadius(Sharpness, 10, 10) >= 1);
  CHECK(boxRadius(Clarity, 100000, 100000) <= kMaxBoxRadius);
}

static void testCache()
{
  using namespace detail_layer;
  cache().clear();
  Img img = makeImg(64, 64, pattern);
  auto a = cache().get(img.data(), 64, 64, Texture, nullptr, 0);
  auto b = cache().get(img.data(), 64, 64, Texture, nullptr, 0);
  CHECK(a && a == b); // second request is a hit: the very same plane
  auto c = cache().get(img.data(), 64, 64, Clarity, nullptr, 0);
  CHECK(c && c != a);
  CHECK(cache().size() == 2);

  // Same address, different picture (what happens when a buffer is freed and
  // reallocated): the fingerprint must tell them apart.
  for (size_t i = 0; i < img.size(); ++i)
    img[i] = static_cast<uint8_t>(img[i] ^ 0x55);
  auto d = cache().get(img.data(), 64, 64, Texture, nullptr, 0);
  CHECK(d && d != a);

  // Cancelled build: null, and nothing is cached.
  cache().clear();
  std::atomic<uint64_t> latest{2};
  CHECK(!cache().get(img.data(), 64, 64, Texture, &latest, 1));
  CHECK(cache().size() == 0);
  CHECK(cache().get(img.data(), 64, 64, Texture, &latest, 2));
  CHECK(cache().size() == 1);
}

static void testCacheEvicts()
{
  using namespace detail_layer;
  cache().clear();
  std::vector<Img> imgs;
  for (int i = 0; i < 12; ++i)
    imgs.push_back(makeImg(32 + i, 32, pattern));
  for (auto &im : imgs)
    CHECK(cache().get(im.data(), static_cast<int>(im.size() / 4 / 32), 32,
                      Texture, nullptr, 0));
  CHECK(cache().size() <= 9); // entry cap; the newest is always kept
  cache().clear();
}

static void testAddPixels()
{
  using namespace detail_layer;
  cache().clear();
  // A vertical step edge: dark left half, bright right half.
  const int w = 80, h = 20;
  const Img src = makeImg(w, h, [](int x, int, int) -> uint8_t
                          { return x < 40 ? 60 : 180; });
  auto run = [&](float tex, float cla, float shp)
  {
    Planes pl;
    CHECK(acquire(pl, src.data(), w, h, gainsFromValues(tex, cla, shp).need(),
                  nullptr, 0));
    Img dst = src;
    addPixels(pl, gainsFromValues(tex, cla, shp), src.data(), dst.data(),
              static_cast<size_t>(w) * h, 0);
    return dst;
  };
  auto at = [&](const Img &im, int x, int y)
  { return static_cast<int>(im[(static_cast<size_t>(y) * w + x) * 4 + 1]); };

  // All zero: untouched, and no plane was even built.
  CHECK(run(0, 0, 0) == src);
  CHECK(cache().size() == 0);

  for (int kind = 0; kind < 3; ++kind)
  {
    const float v[3] = {kind == 0 ? 100.0f : 0.0f, kind == 1 ? 100.0f : 0.0f,
                        kind == 2 ? 100.0f : 0.0f};
    const Img up = run(v[0], v[1], v[2]);
    // Contrast across the edge grows: dark side darker, bright side brighter.
    CHECK(at(up, 39, 5) < at(src, 39, 5));
    CHECK(at(up, 40, 5) > at(src, 40, 5));
    // Far from the edge (a flat region) nothing changes.
    CHECK(at(up, 2, 5) == at(src, 2, 5));
    CHECK(at(up, 77, 5) == at(src, 77, 5));
    // Alpha untouched.
    CHECK(up[3] == 255);
    if (kind < 2)
    {
      // Negative Texture / Clarity smooth: contrast across the edge shrinks.
      const Img dn = run(-v[0], -v[1], 0);
      CHECK(at(dn, 39, 5) > at(src, 39, 5));
      CHECK(at(dn, 40, 5) < at(src, 40, 5));
    }
  }
  // Negative sharpness is clamped to 0.
  CHECK(run(0, 0, -50) == src);
  // Equal R=G=B gain: detail is added to all channels equally (no hue shift).
  const Img up = run(100, 0, 0);
  const int dR = static_cast<int>(up[(5 * w + 40) * 4]) - src[(5 * w + 40) * 4];
  const int dG = static_cast<int>(up[(5 * w + 40) * 4 + 1]) - src[(5 * w + 40) * 4 + 1];
  CHECK(dR == dG);
  cache().clear();
}

static void testClarityLeavesEndsAlone()
{
  using namespace detail_layer;
  cache().clear();
  // The same step edge, but near-white: the midtone weight is ~0 there.
  const int w = 80, h = 8;
  const Img src = makeImg(w, h, [](int x, int, int) -> uint8_t
                          { return x < 40 ? 250 : 255; });
  Planes pl;
  const Gains g = gainsFromValues(0, 100, 0);
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  Img dst = src;
  addPixels(pl, g, src.data(), dst.data(), static_cast<size_t>(w) * h, 0);
  int maxChange = 0;
  for (size_t i = 0; i < src.size(); ++i)
    maxChange = std::max(maxChange, std::abs(dst[i] - src[i]));
  CHECK(maxChange <= 1);
  cache().clear();
}

static void testThreads()
{
  using namespace detail_layer;
  cache().clear();
  const Img img = makeImg(300, 200, pattern);
  std::vector<std::thread> ts;
  std::vector<Cache::PlanePtr> got(8);
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t]
                    { got[static_cast<size_t>(t)] = cache().get(
                          img.data(), 300, 200, t % 3, nullptr, 0); });
  for (auto &t : ts)
    t.join();
  for (int t = 0; t < 8; ++t)
  {
    CHECK(got[static_cast<size_t>(t)]);
    for (int u = t + 1; u < 8; ++u)
      if (t % 3 == u % 3) // same key => same plane, however the race went
        CHECK(*got[static_cast<size_t>(t)] == *got[static_cast<size_t>(u)]);
  }
  CHECK(cache().size() == 3);
  cache().clear();
}

// ------------------------------------------------------------------ gains, arguments
static void testGainsFromValues()
{
  using namespace detail_layer;
  const Gains full = gainsFromValues(100, 100, 100);
  CHECK(full.g[Texture] == kGain[Texture] && full.g[Clarity] == kGain[Clarity] && full.g[Sharpness] == kGain[Sharpness]);
  // Out-of-range values clamp to the slider range.
  CHECK(gainsFromValues(500, 500, 500) == full);
  const Gains neg = gainsFromValues(-100, -100, -100);
  CHECK(neg.g[Texture] == -kGain[Texture] && neg.g[Clarity] == -kGain[Clarity]);
  CHECK(neg.g[Sharpness] == 0.0f); // sharpness is 0..100: negative means 0
  CHECK(gainsFromValues(-500, -500, -500) == neg);
  // NaN and infinity count as "untouched": the gains are always finite.
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  CHECK(gainsFromValues(nan, nan, nan) == Gains{});
  CHECK(gainsFromValues(inf, -inf, inf) == Gains{});
  CHECK(!gainsFromValues(nan, inf, -inf).any() && gainsFromValues(nan, inf, -inf).need() == 0);
  // need() has one bit per layer, any() follows the gains.
  CHECK(gainsFromValues(50, 0, 0).need() == (1u << Texture));
  CHECK(gainsFromValues(0, 50, 0).need() == (1u << Clarity));
  CHECK(gainsFromValues(0, 0, 50).need() == (1u << Sharpness));
  CHECK(gainsFromValues(-1, 1, 1).need() == 7u);
  CHECK(!Gains{}.any() && Gains{}.need() == 0 && gainsFromValues(0, 0, 1).any());
}

static void testInvalidArguments()
{
  using namespace detail_layer;
  cache().clear();
  const Img img = makeImg(8, 8, pattern);
  const uint8_t *none = nullptr;
  Plane p;
  CHECK(!blurLuma(none, 8, 8, 1, p, nullptr, 0));
  CHECK(!blurLuma(img.data(), 0, 8, 1, p, nullptr, 0));
  CHECK(!blurLuma(img.data(), 8, 0, 1, p, nullptr, 0));
  CHECK(!blurLuma(img.data(), 8, 8, 0, p, nullptr, 0));
  CHECK(!blurLuma(img.data(), 8, 8, -3, p, nullptr, 0));
  // The cache refuses them too, and caches nothing.
  CHECK(!cache().get(none, 8, 8, Texture, nullptr, 0));
  CHECK(!cache().get(img.data(), 0, 8, Texture, nullptr, 0));
  CHECK(!cache().get(img.data(), 8, 0, Texture, nullptr, 0));
  CHECK(!cache().get(img.data(), 8, 8, -1, nullptr, 0));
  CHECK(!cache().get(img.data(), 8, 8, kKindCount, nullptr, 0));
  CHECK(cache().size() == 0);
  // acquire(): no pixels or no size is "nothing to do" (true, planes left null), not a failure.
  Planes pl;
  CHECK(acquire(pl, none, 8, 8, 7u, nullptr, 0));
  CHECK(acquire(pl, img.data(), 0, 8, 7u, nullptr, 0));
  CHECK(acquire(pl, img.data(), 8, -1, 7u, nullptr, 0));
  CHECK(!pl.p[0] && !pl.p[1] && !pl.p[2] && cache().size() == 0);
  // need = 0 builds nothing.
  CHECK(acquire(pl, img.data(), 8, 8, 0u, nullptr, 0) && cache().size() == 0);
  cache().clear();
}

// ------------------------------------------------------------- applying in pieces
static void testAddPixelsPiecesMatchWhole()
{
  using namespace detail_layer;
  cache().clear();
  const int w = 61, h = 47;
  const size_t total = static_cast<size_t>(w) * h;
  const Img src = makeImg(w, h, pattern);
  const Gains g = gainsFromValues(40, -30, 50);
  Planes pl;
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  const size_t steps[] = {1, 60, 61, 300, 7, 1000}; // pieces that start and end mid-row

  Img whole = src, pieces = src;
  addPixels(pl, g, src.data(), whole.data(), total, 0);
  std::vector<float> wholeF(src.begin(), src.end()), piecesF = wholeF;
  addPixels(pl, g, src.data(), wholeF.data(), total, 0);
  size_t pos = 0, si = 0;
  while (pos < total)
  {
    const size_t n = std::min(steps[si++ % 6], total - pos);
    addPixels(pl, g, src.data() + pos * 4, pieces.data() + pos * 4, n, pos);
    addPixels(pl, g, src.data() + pos * 4, piecesF.data() + pos * 4, n, pos);
    pos += n;
  }
  CHECK(whole != src); // the effect did something
  CHECK(whole == pieces && wholeF == piecesF);
  // The bytes are the rounded floats (and alpha is never touched).
  for (size_t i = 0; i < whole.size(); ++i)
  {
    if (i % 4 == 3)
      CHECK(whole[i] == 255 && wholeF[i] == 255.0f);
    else
      CHECK(whole[i] == static_cast<uint8_t>(wholeF[i] + 0.5f));
  }

  // A plane that doesn't cover [first, first + count) is skipped, not read out of range.
  Img guard = src;
  addPixels(pl, g, src.data(), guard.data(), 10, total - 3); // runs 7 pixels past the plane
  addPixels(pl, g, src.data(), guard.data(), 1, total + 5);  // starts past the plane
  CHECK(guard == src);
  cache().clear();
}

// -------------------------------------------------------------------- Clarity's limit
static void testClaritySoftLimit()
{
  using namespace detail_layer;
  cache().clear();
  // A big step: the detail at the edge is far beyond kClarityLimit, so the soft limit matters.
  const int w = 80, h = 8;
  const Img src = makeImg(w, h, [](int x, int, int) -> uint8_t
                          { return x < 40 ? 40 : 230; });
  const Gains g = gainsFromValues(0, 100, 0);
  Planes pl;
  CHECK(acquire(pl, src.data(), w, h, g.need(), nullptr, 0));
  std::vector<float> dst(src.begin(), src.end());
  addPixels(pl, g, src.data(), dst.data(), static_cast<size_t>(w) * h, 0);
  const Plane &blur = *pl.p[Clarity];
  double maxD = 0;
  for (int x = 30; x < 50; ++x)
  {
    const size_t px = static_cast<size_t>(4) * w + x; // row 4
    const uint8_t *s = &src[px * 4];
    const int l = lumaOf(s);
    const double d = (static_cast<double>(l << 4) - blur[px]) / 16.0;
    maxD = std::max(maxD, std::fabs(d));
    // The formula: k = gain * midWeight * d / (1 + |d| / limit), added to every channel, clamped.
    const double k = g.g[Clarity] * midWeights()[l] * d / (1.0 + std::fabs(d) / kClarityLimit);
    for (int c = 0; c < 3; ++c)
    {
      const double want = std::clamp(static_cast<double>(s[c]) + k, 0.0, 255.0);
      CHECK(std::fabs(dst[px * 4 + static_cast<size_t>(c)] - want) < 1e-3);
    }
  }
  CHECK(maxD > kClarityLimit); // the limit really was in play on this edge
  // ...and it bounds the change: never more than gain * limit levels.
  for (size_t i = 0; i < dst.size(); ++i)
    if (i % 4 != 3)
      CHECK(std::fabs(dst[i] - static_cast<float>(src[i])) <= g.g[Clarity] * kClarityLimit + 1e-3);
  cache().clear();
}

// -------------------------------------------------------------- 16-bit source blur
// The naive reference for a 16-bit picture: the plane starts as the unrounded luma in 1/64 level
// (rounded once to a plane unit), then the same 3 + 3 box passes with replicated edges.
static std::vector<int> refBlur16(const std::vector<uint16_t> &img, int w, int h, int r)
{
  std::vector<int> a(static_cast<size_t>(w) * h), b(a.size());
  for (size_t i = 0; i < a.size(); ++i)
    a[i] = static_cast<int>(detail_layer::SrcTraits<uint16_t>::lumaFix(&img[i * 4]) + 0.5f);
  const int n = 2 * r + 1;
  auto pass = [&](bool horiz)
  {
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        long sum = 0;
        for (int k = -r; k <= r; ++k)
        {
          const int xx = horiz ? std::clamp(x + k, 0, w - 1) : x;
          const int yy = horiz ? y : std::clamp(y + k, 0, h - 1);
          sum += a[static_cast<size_t>(yy) * w + xx];
        }
        b[static_cast<size_t>(y) * w + x] = static_cast<int>(std::lround(static_cast<double>(sum) / n));
      }
    a.swap(b);
  };
  for (int i = 0; i < 3; ++i)
    pass(true);
  for (int i = 0; i < 3; ++i)
    pass(false);
  return a;
}

static void testBlurMatchesReference16()
{
  const int w = 61, h = 47;
  std::vector<uint16_t> img(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        img[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(c)] =
            c == 3 ? 65535 : static_cast<uint16_t>(pattern(x, y, c) * 257 + (x * 7 + y * 3 + c) % 200);
  for (int r : {1, 2, 5, 30, 80})
  {
    detail_layer::Plane p;
    CHECK(detail_layer::blurLuma(img.data(), w, h, r, p, nullptr, 0));
    const auto ref = refBlur16(img, w, h, r);
    int maxDiff = 0;
    for (size_t i = 0; i < ref.size(); ++i)
      maxDiff = std::max(maxDiff, std::abs(static_cast<int>(p[i]) - ref[i]));
    if (maxDiff > 1)
      std::printf("  16-bit radius %d: max diff %d\n", r, maxDiff);
    CHECK(maxDiff <= 1); // one plane unit = 1/64 level
  }
  // A flat 16-bit picture stays exactly flat (replicated edges), at its own precision.
  std::vector<uint16_t> flat(static_cast<size_t>(50) * 40 * 4, 33333);
  detail_layer::Plane p;
  CHECK(detail_layer::blurLuma(flat.data(), 50, 40, 9, p, nullptr, 0));
  for (uint16_t v : p)
    CHECK(v == p[0]);
}

int main()
{
  testBlurMatchesReference();
  testFlatImageStaysFlat();
  testRadiusScalesWithSize();
  testCache();
  testCacheEvicts();
  testAddPixels();
  testClarityLeavesEndsAlone();
  testThreads();
  testGainsFromValues();
  testInvalidArguments();
  testAddPixelsPiecesMatchWhole();
  testClaritySoftLimit();
  testBlurMatchesReference16();
  std::printf(gFailures ? "%d FAILED\n" : "all passed\n", gFailures);
  return gFailures ? 1 : 0;
}