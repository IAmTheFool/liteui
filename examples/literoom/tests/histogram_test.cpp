// histogram_test.cpp - the histogram counts, the clip bits and the clipping
// overlay on small hand-made pictures, plus the behaviours that are easy to
// break by accident (sampling, block reduction, highlight priority). Links
// liteui for CanvasImage / Color, like raster_ops_test does.
#include <cstdint>
#include <memory>
#include <numeric>

#include "check.hpp"
#include "histogram.hpp"

static void put(CanvasImage &img, int x, int y, int r, int g, int b, int a = 255)
{
  uint8_t *p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
  p[0] = static_cast<uint8_t>(r);
  p[1] = static_cast<uint8_t>(g);
  p[2] = static_cast<uint8_t>(b);
  p[3] = static_cast<uint8_t>(a);
}

static CanvasImage solid(int w, int h, int r, int g, int b, int a = 255)
{
  CanvasImage img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      put(img, x, y, r, g, b, a);
  return img;
}

static Color at(const CanvasImage &img, int x, int y)
{
  const uint8_t *p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
  return Color{p[0], p[1], p[2], p[3]};
}

static uint32_t sum(const std::array<uint32_t, 256> &a)
{
  return std::accumulate(a.begin(), a.end(), uint32_t{0});
}

static int tinted(const CanvasImage &ov)
{
  int n = 0;
  for (size_t i = 3; i < ov.pixels.size(); i += 4)
    n += ov.pixels[i] != 0;
  return n;
}

constexpr Color kClear{0, 0, 0, 0}; // an overlay pixel nothing was drawn on

// ----------------------------------------------------------------- counts
static void testEmptyInput()
{
  const histogram::Counts a = histogram::compute(nullptr);
  CHECK(sum(a.r) == 0 && sum(a.g) == 0 && sum(a.b) == 0 && sum(a.l) == 0);
  CHECK(a.clipLo == 0 && a.clipHi == 0);

  const CanvasImage none;
  const histogram::Counts b = histogram::compute(&none);
  CHECK(sum(b.l) == 0 && b.clipLo == 0 && b.clipHi == 0);
}

static void testCounts()
{
  CanvasImage img(4, 2);
  put(img, 0, 0, 0, 0, 0);          // black
  put(img, 1, 0, 255, 255, 255);    // white
  put(img, 2, 0, 255, 0, 0);        // red
  put(img, 3, 0, 0, 255, 0);        // green
  put(img, 0, 1, 0, 0, 255);        // blue
  put(img, 1, 1, 128, 128, 128);    // mid grey
  put(img, 2, 1, 10, 20, 30, 0);    // fully transparent: not counted
  put(img, 3, 1, 200, 100, 50);

  const histogram::Counts c = histogram::compute(&img);
  CHECK(sum(c.r) == 7 && sum(c.g) == 7 && sum(c.b) == 7 && sum(c.l) == 7);
  CHECK(c.r[0] == 3 && c.r[255] == 2 && c.r[128] == 1 && c.r[200] == 1 && c.r[10] == 0);
  CHECK(c.g[0] == 3 && c.g[255] == 2 && c.g[128] == 1 && c.g[100] == 1 && c.g[20] == 0);
  CHECK(c.b[0] == 3 && c.b[255] == 2 && c.b[128] == 1 && c.b[50] == 1 && c.b[30] == 0);
  // luma = (77 R + 150 G + 29 B) >> 8
  CHECK(c.l[0] == 1 && c.l[255] == 1 && c.l[128] == 1);
  CHECK(c.l[76] == 1 && c.l[149] == 1 && c.l[28] == 1 && c.l[124] == 1);
  CHECK(c.clipLo == (kChanR | kChanG | kChanB) && c.clipHi == (kChanR | kChanG | kChanB));
}

static void testAlphaSkip()
{
  CanvasImage img(2, 1);
  put(img, 0, 0, 5, 5, 5, 0); // straighten's empty corner
  put(img, 1, 0, 6, 6, 6, 1); // barely there, still counts
  const histogram::Counts c = histogram::compute(&img);
  CHECK(sum(c.l) == 1 && c.r[5] == 0 && c.r[6] == 1);
}

static void testClipBits()
{
  auto clip = [](int r, int g, int b)
  {
    CanvasImage img = solid(3, 3, r, g, b);
    return histogram::compute(&img);
  };
  CHECK(clip(1, 254, 100).clipLo == 0 && clip(1, 254, 100).clipHi == 0);
  CHECK(clip(0, 100, 100).clipLo == kChanR && clip(0, 100, 100).clipHi == 0);
  CHECK(clip(100, 0, 100).clipLo == kChanG);
  CHECK(clip(100, 100, 0).clipLo == kChanB);
  CHECK(clip(100, 255, 255).clipHi == (kChanG | kChanB) && clip(100, 255, 255).clipLo == 0);
  CHECK(clip(255, 0, 255).clipHi == (kChanR | kChanB) && clip(255, 0, 255).clipLo == kChanG);

  // Transparent pixels never clip.
  CanvasImage img = solid(3, 3, 100, 100, 100);
  put(img, 1, 1, 255, 0, 255, 0);
  const histogram::Counts c = histogram::compute(&img);
  CHECK(c.clipLo == 0 && c.clipHi == 0);
}

// At most ~300000 pixels are visited (stride = floor(sqrt(total / 300000))).
static void testSampling()
{
  const CanvasImage exact = solid(600, 500, 90, 90, 90); // exactly 300000: every pixel
  CHECK(sum(histogram::compute(&exact).l) == 300000);

  const CanvasImage million = solid(1000, 1000, 90, 90, 90); // sqrt(3.33) = 1.82 -> stride 1
  CHECK(sum(histogram::compute(&million).l) == 1000000);

  CanvasImage big = solid(1300, 1000, 100, 100, 100); // sqrt(4.33) = 2.08 -> stride 2
  CHECK(sum(histogram::compute(&big).l) == 650u * 500u);

  // The pixels off the sample grid are invisible to the histogram (and so to
  // its clip triangles) but not to the overlay, which looks at every pixel.
  put(big, 1, 1, 255, 100, 100);
  const histogram::Counts c = histogram::compute(&big);
  CHECK(c.r[255] == 0 && c.clipHi == 0);
  const auto ov = histogram::clipOverlay(&big, {false, true, false});
  CHECK(ov && tinted(*ov) == 1 && at(*ov, 1, 1) == kColorClipHighlight);
  put(big, 2, 2, 255, 100, 100); // on the grid: counted
  CHECK(histogram::compute(&big).clipHi == kChanR);
}

// ---------------------------------------------------------------- helpers
static void testChannelsAt()
{
  const uint8_t px[4] = {255, 0, 255, 255};
  CHECK(histogram::channelsAt(px, 255) == (kChanR | kChanB));
  CHECK(histogram::channelsAt(px, 0) == kChanG);
  CHECK(histogram::channelsAt(px, 7) == 0);
}

static void testClipChannelColor()
{
  const Color want[8] = {{0, 0, 0, 0},        {235, 50, 50, 255},  {50, 205, 80, 255},  {245, 220, 40, 255},
                         {70, 120, 255, 255}, {235, 70, 220, 255}, {40, 210, 235, 255}, {235, 235, 235, 255}};
  for (uint8_t m = 0; m < 8; ++m)
  {
    CHECK(histogram::clipChannelColor(m, false) == want[m]);
    const Color p = histogram::clipChannelColor(m, true); // lightened: (c + 255) / 2, alpha as is
    CHECK(p.r == (want[m].r + 255) / 2 && p.g == (want[m].g + 255) / 2 && p.b == (want[m].b + 255) / 2);
    CHECK(p.a == want[m].a);
  }
  CHECK(histogram::clipChannelColor(8, false) == want[0]); // only the low 3 bits count
  CHECK(histogram::clipChannelColor(0xF9, false) == want[1]);
  CHECK(kColorClipHighlight == want[1]); // the classic colours are the R / B entries
}

// ---------------------------------------------------------------- overlay
static void testOverlayOffOrEmpty()
{
  const CanvasImage img = solid(4, 4, 255, 0, 255);
  CHECK(!histogram::clipOverlay(&img, {false, false, false}));
  CHECK(!histogram::clipOverlay(&img, {false, false, true})); // per-channel alone shows nothing
  CHECK(!histogram::clipOverlay(nullptr, {true, true, false}));
  const CanvasImage none;
  CHECK(!histogram::clipOverlay(&none, {true, true, false}));
  CHECK(histogram::clipOverlay(&img, {true, false, false}));
}

static void testOverlayClassic()
{
  CanvasImage img = solid(4, 1, 100, 100, 100);
  put(img, 1, 0, 255, 100, 100); // highlight in one channel
  put(img, 2, 0, 100, 0, 100);   // shadow in one channel
  put(img, 3, 0, 255, 0, 100);   // both

  auto hi = histogram::clipOverlay(&img, {false, true, false});
  CHECK(hi && hi->width == 4 && hi->height == 1);
  CHECK(at(*hi, 0, 0) == kClear && at(*hi, 1, 0) == kColorClipHighlight);
  CHECK(at(*hi, 2, 0) == kClear && at(*hi, 3, 0) == kColorClipHighlight);

  auto lo = histogram::clipOverlay(&img, {true, false, false});
  CHECK(lo && at(*lo, 1, 0) == kClear && at(*lo, 2, 0) == kColorClipShadow && at(*lo, 3, 0) == kColorClipShadow);

  auto both = histogram::clipOverlay(&img, {true, true, false});
  CHECK(at(*both, 1, 0) == kColorClipHighlight && at(*both, 2, 0) == kColorClipShadow);
  CHECK(at(*both, 3, 0) == kColorClipHighlight); // highlights win over shadows
  CHECK(tinted(*both) == 3);

  // A pixel clipped in all three channels is tinted in classic mode.
  const CanvasImage white = solid(2, 2, 255, 255, 255);
  CHECK(tinted(*histogram::clipOverlay(&white, {false, true, false})) == 4);
}

static void testOverlayPerChannel()
{
  CanvasImage img = solid(6, 1, 100, 100, 100);
  put(img, 0, 0, 255, 100, 100); // R high
  put(img, 1, 0, 100, 255, 255); // G+B high -> cyan
  put(img, 2, 0, 100, 0, 100);   // G low  -> pastel green
  put(img, 3, 0, 255, 255, 255); // all three: left untinted
  put(img, 4, 0, 0, 0, 0);       // all three low: left untinted
  put(img, 5, 0, 255, 255, 0);   // R+G high (yellow) AND B low

  auto ov = histogram::clipOverlay(&img, {true, true, true});
  CHECK(at(*ov, 0, 0) == histogram::clipChannelColor(kChanR, false));
  CHECK(at(*ov, 1, 0) == histogram::clipChannelColor(kChanG | kChanB, false));
  CHECK(at(*ov, 2, 0) == histogram::clipChannelColor(kChanG, true));
  CHECK(at(*ov, 3, 0) == kClear);
  CHECK(at(*ov, 4, 0) == kClear);
  CHECK(at(*ov, 5, 0) == histogram::clipChannelColor(kChanR | kChanG, false)); // highlights win

  auto shadowsOnly = histogram::clipOverlay(&img, {true, false, true});
  CHECK(at(*shadowsOnly, 5, 0) == histogram::clipChannelColor(kChanB, true));
  CHECK(at(*shadowsOnly, 0, 0) == kClear);
}

static void testOverlayAlpha()
{
  CanvasImage img = solid(3, 1, 100, 100, 100);
  put(img, 1, 0, 255, 255, 0, 0); // clipped, but transparent
  CHECK(tinted(*histogram::clipOverlay(&img, {true, true, false})) == 0);
  CHECK(tinted(*histogram::clipOverlay(&img, {true, true, true})) == 0);
}

// Pictures wider/taller than 2048 are reduced in blocks; a block shows if ANY
// pixel in it is clipped.
static void testOverlayBlocks()
{
  {
    const CanvasImage atCap = solid(2048, 8, 100, 100, 100);
    auto ov = histogram::clipOverlay(&atCap, {true, true, false});
    CHECK(ov && ov->width == 2048 && ov->height == 8); // at the cap: 1:1
  }
  {
    const CanvasImage justOver = solid(2049, 8, 100, 100, 100);
    auto ov = histogram::clipOverlay(&justOver, {true, true, false});
    CHECK(ov && ov->width == 1025 && ov->height == 4); // just over: blocks of 2
  }
  CanvasImage wide = solid(4100, 10, 100, 100, 100); // blocks of 3 (ceil(4100 / 2048))
  put(wide, 2050, 5, 255, 100, 100);                 // block (683, 1)
  auto ov = histogram::clipOverlay(&wide, {false, true, false});
  CHECK(ov && ov->width == 1367 && ov->height == 4);
  CHECK(tinted(*ov) == 1 && at(*ov, 683, 1) == kColorClipHighlight);

  CanvasImage tall = solid(1, 4200, 100, 100, 100);
  put(tall, 0, 4199, 0, 100, 100); // the last, partial block
  auto ot = histogram::clipOverlay(&tall, {true, false, false});
  CHECK(ot && ot->width == 1 && ot->height == 1400 && tinted(*ot) == 1);
  CHECK(at(*ot, 0, 1399).a == 255);
}

// Per-channel mode on reduced blocks: a block can hold several clip sets; the
// lowest-numbered set present wins (R < G < R+G < B < R+B < G+B), and the
// all-three set never counts.
static void testOverlayPerChannelBlocks()
{
  CanvasImage img = solid(4100, 10, 100, 100, 100); // blocks of 3 pixels
  put(img, 0, 0, 100, 255, 100); // block 0: G high (set 2) ...
  put(img, 1, 0, 100, 100, 255); //          ... and B high (set 4)  -> G
  put(img, 3, 0, 100, 100, 255); // block 1: B high (4) ...
  put(img, 4, 0, 100, 255, 255); //          ... and G+B high (6)    -> B
  put(img, 6, 0, 255, 255, 100); // block 2: R+G high (3) ...
  put(img, 7, 0, 255, 100, 255); //          ... and R+B high (5)    -> R+G
  put(img, 9, 0, 255, 255, 255); // block 3: all three (skipped) ...
  put(img, 10, 0, 255, 100, 100); //         ... and R high (1)      -> R
  put(img, 12, 0, 255, 255, 255); // block 4: all three only         -> untinted

  auto ov = histogram::clipOverlay(&img, {false, true, true});
  CHECK(ov && ov->width == 1367);
  CHECK(at(*ov, 0, 0) == histogram::clipChannelColor(kChanG, false));
  CHECK(at(*ov, 1, 0) == histogram::clipChannelColor(kChanB, false));
  CHECK(at(*ov, 2, 0) == histogram::clipChannelColor(kChanR | kChanG, false));
  CHECK(at(*ov, 3, 0) == histogram::clipChannelColor(kChanR, false));
  CHECK(at(*ov, 4, 0) == kClear);
  CHECK(tinted(*ov) == 4);
}

int main()
{
  testEmptyInput();
  testCounts();
  testAlphaSkip();
  testClipBits();
  testSampling();
  testChannelsAt();
  testClipChannelColor();
  testOverlayOffOrEmpty();
  testOverlayClassic();
  testOverlayPerChannel();
  testOverlayAlpha();
  testOverlayBlocks();
  testOverlayPerChannelBlocks();
  return finish();
}
