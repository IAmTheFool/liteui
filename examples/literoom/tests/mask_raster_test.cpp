// Build: g++ -std=c++20 -I.. mask_raster_test.cpp -o mask_raster_test && ./mask_raster_test
#include "mask_raster.hpp"
#include "check.hpp"

#include <random>

// A RasterEval over a hand-made map; lo / hi are the exact per-row extents.
static RasterEval make(int W, int H, int mw, int mh, const std::vector<uint8_t> &map)
{
  RasterEval e;
  e.W = W;
  e.H = H;
  e.mw = mw;
  e.mh = mh;
  e.map = std::make_shared<const std::vector<uint8_t>>(map);
  auto lo = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), mw);
  auto hi = std::make_shared<std::vector<int>>(static_cast<size_t>(mh), 0);
  for (int y = 0; y < mh; ++y)
    for (int x = 0; x < mw; ++x)
      if (map[static_cast<size_t>(y) * mw + x])
      {
        (*lo)[static_cast<size_t>(y)] = std::min((*lo)[static_cast<size_t>(y)], x);
        (*hi)[static_cast<size_t>(y)] = std::max((*hi)[static_cast<size_t>(y)], x + 1);
      }
  e.lo = lo;
  e.hi = hi;
  return e;
}

static std::vector<float> row(const RasterEval &e, int y, int W)
{
  std::vector<float> w(static_cast<size_t>(W));
  e.rowWeights(y, 0, W, w.data());
  return w;
}

static void testSameSizeIsExact()
{
  const int W = 13, H = 9;
  std::vector<uint8_t> map(static_cast<size_t>(W) * H);
  for (size_t i = 0; i < map.size(); ++i)
    map[i] = static_cast<uint8_t>((i * 37) & 255);
  const RasterEval e = make(W, H, W, H, map);
  for (int y = 0; y < H; ++y)
  {
    const auto w = row(e, y, W);
    for (int x = 0; x < W; ++x)
      CHECK_NEAR(w[static_cast<size_t>(x)], map[static_cast<size_t>(y) * W + x] / 255.0, 1e-6);
  }
  // Reading a sub-range [x0, x1) gives the same numbers, written from out[0].
  float part[5];
  e.rowWeights(4, 3, 8, part);
  const auto full = row(e, 4, W);
  for (int i = 0; i < 5; ++i)
    CHECK(part[i] == full[static_cast<size_t>(3 + i)]);
}

static void testScaledMapInterpolates()
{
  // A map half the image's width and height holding a linear ramp: reading it
  // at image resolution is bilinear, so interior values follow the ramp.
  const int W = 40, H = 20, mw = 20, mh = 10;
  std::vector<uint8_t> map(static_cast<size_t>(mw) * mh);
  for (int y = 0; y < mh; ++y)
    for (int x = 0; x < mw; ++x)
      map[static_cast<size_t>(y) * mw + x] = static_cast<uint8_t>(x * 10 + y * 3);
  const RasterEval e = make(W, H, mw, mh, map);
  for (int y = 2; y < H - 2; ++y)
  {
    const auto w = row(e, y, W);
    for (int x = 2; x < W - 2; ++x)
    {
      const double mx = (x + 0.5) * mw / W - 0.5, my = (y + 0.5) * mh / H - 0.5;
      CHECK_NEAR(w[static_cast<size_t>(x)] * 255.0, mx * 10 + my * 3, 0.01);
    }
  }
  // Edges clamp to the border row / column of the map (no read outside it).
  const auto top = row(e, 0, W);
  CHECK(top[0] >= 0.0f && top[W - 1] <= 1.0f);
}

static void testSpanCoversEveryNonZero()
{
  std::mt19937 rng(41);
  const int sizes[][4] = {{50, 40, 50, 40}, {50, 40, 25, 20}, {100, 60, 37, 23}, {64, 64, 16, 16}};
  for (const auto &sz : sizes)
    for (int trial = 0; trial < 40; ++trial)
    {
      const int W = sz[0], H = sz[1], mw = sz[2], mh = sz[3];
      std::vector<uint8_t> map(static_cast<size_t>(mw) * mh, 0);
      for (int b = 0; b < 3; ++b) // a few blobs
      {
        const int bx = static_cast<int>(rng() % static_cast<unsigned>(mw)), by = static_cast<int>(rng() % static_cast<unsigned>(mh));
        for (int y = std::max(0, by - 3); y < std::min(mh, by + 4); ++y)
          for (int x = std::max(0, bx - 4); x < std::min(mw, bx + 5); ++x)
            map[static_cast<size_t>(y) * mw + x] = static_cast<uint8_t>(60 + rng() % 190);
      }
      const RasterEval e = make(W, H, mw, mh, map);
      for (int y = 0; y < H; ++y)
      {
        const auto w = row(e, y, W);
        int x0 = 0, x1 = 0;
        const bool any = e.rowSpan(y, W, x0, x1);
        for (int x = 0; x < W; ++x)
          if (w[static_cast<size_t>(x)] > 0.0f)
          {
            CHECK(any && x >= x0 && x < x1);
            if (!(any && x >= x0 && x < x1))
              goto next; // one report per case is enough
          }
        if (any)
          CHECK(x0 >= 0 && x1 <= W && x1 > x0);
      }
    next:;
    }
}

static void testEmptyAndOutOfRange()
{
  RasterEval none;
  float w[4] = {9, 9, 9, 9};
  int x0, x1;
  CHECK(!none.rowSpan(0, 10, x0, x1));
  none.rowWeights(0, 0, 4, w);
  CHECK(w[0] == 0 && w[3] == 0);
  const RasterEval e = make(4, 4, 4, 4, std::vector<uint8_t>(16, 255));
  CHECK(e.rowSpan(0, 4, x0, x1) && x0 == 0 && x1 == 4);
  CHECK(!e.rowSpan(-1, 4, x0, x1) && !e.rowSpan(4, 4, x0, x1));
  w[0] = 9;
  e.rowWeights(-1, 0, 1, w);
  CHECK(w[0] == 0);
  e.rowWeights(7, 0, 1, w);
  CHECK(w[0] == 0);
  // A map with nothing in it has no span on any row.
  const RasterEval blank = make(4, 4, 4, 4, std::vector<uint8_t>(16, 0));
  for (int y = 0; y < 4; ++y)
    CHECK(!blank.rowSpan(y, 4, x0, x1));
}

static void testCopiesShareTheMap()
{
  const RasterEval a = make(4, 4, 4, 4, std::vector<uint8_t>(16, 7));
  const RasterEval b = a; // Evals live in a std::variant and are copied freely
  CHECK(a.map == b.map && a.lo == b.lo && a.hi == b.hi);
}

int main()
{
  testSameSizeIsExact();
  testScaledMapInterpolates();
  testSpanCoversEveryNonZero();
  testEmptyAndOutOfRange();
  testCopiesShareTheMap();
  return finish();
}
