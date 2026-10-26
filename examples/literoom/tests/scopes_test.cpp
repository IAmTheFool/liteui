// scopes_test.cpp - the waveform and the vectorscope on small hand-made
// pictures: the counts (which column, which level, which cell), the drawn
// pictures (position, brightness, colors), the sample grid and the view cycle.
// Links liteui for CanvasImage / Color, like histogram_test does.
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>

#include "check.hpp"
#include "render/scopes.hpp"

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

constexpr Color kBg{scopes::kBgR, scopes::kBgG, scopes::kBgB, 255};

// Pixels of a scope picture that are not the plain background.
static int lit(const CanvasImage &img)
{
  int n = 0;
  for (int y = 0; y < img.height; ++y)
    for (int x = 0; x < img.width; ++x)
      n += !(at(img, x, y) == kBg);
  return n;
}

// ------------------------------------------------------------------ the cycle
static void testKinds()
{
  using scopes::Kind;
  CHECK(scopes::nextKind(Kind::Histogram) == Kind::Waveform);
  CHECK(scopes::nextKind(Kind::Waveform) == Kind::Vectorscope);
  CHECK(scopes::nextKind(Kind::Vectorscope) == Kind::Histogram); // wraps
  CHECK(scopes::nextKind(Kind::Count) == Kind::Histogram);       // out of range
  CHECK(scopes::nextKind(static_cast<Kind>(-1)) == Kind::Histogram);
  CHECK(scopes::nextKind(static_cast<Kind>(99)) == Kind::Histogram);
  CHECK(std::string(scopes::kindName(Kind::Histogram)) == "Histogram");
  CHECK(std::string(scopes::kindName(Kind::Waveform)) == "Waveform");
  CHECK(std::string(scopes::kindName(Kind::Vectorscope)) == "Vectorscope");
  CHECK(std::string(scopes::kindName(Kind::Count)) == "Histogram");
}

// -------------------------------------------------------------------- helpers
static void testSampleStep()
{
  CHECK(scopes::sampleStep(600, 500) == 1);   // exactly kHistMaxSamples
  CHECK(scopes::sampleStep(1000, 1000) == 1); // sqrt(3.33) = 1.8
  CHECK(scopes::sampleStep(1300, 1000) == 2); // sqrt(4.33) = 2.08
  CHECK(scopes::sampleStep(2000, 2000) == 3);
  CHECK(scopes::sampleStep(1, 1) == 1);
}

static void testDensity()
{
  CHECK(scopes::density(0, 10, 0.04f) == 0.0f);
  CHECK(scopes::density(5, 0, 0.04f) == 0.0f);
  CHECK(scopes::density(10, 10, 0.04f) == 1.0f); // share 1 is far above the saturation
  CHECK_NEAR(scopes::density(1, 100, 0.04f), 0.5, 1e-6);
  CHECK_NEAR(scopes::density(4, 100, 0.04f), 1.0, 1e-6); // exactly at the saturation
}

static void testCbCr()
{
  int cb, cr;
  scopes::rgbToCbCr(0, 0, 0, &cb, &cr);
  CHECK(cb == 128 && cr == 128);
  scopes::rgbToCbCr(255, 255, 255, &cb, &cr);
  CHECK(cb == 128 && cr == 128);
  scopes::rgbToCbCr(77, 77, 77, &cb, &cr);
  CHECK(cb == 128 && cr == 128); // any gray is the center
  scopes::rgbToCbCr(0, 0, 255, &cb, &cr);
  CHECK(cb == 255 && cr == 107);
  scopes::rgbToCbCr(255, 0, 0, &cb, &cr);
  CHECK(cb == 85 && cr == 255);
  scopes::rgbToCbCr(0, 255, 0, &cb, &cr);
  CHECK(cb == 43 && cr == 21);
  // Every corner of the RGB cube stays inside 0..255.
  for (int i = 0; i < 8; ++i)
  {
    scopes::rgbToCbCr((i & 1) * 255, ((i >> 1) & 1) * 255, ((i >> 2) & 1) * 255, &cb, &cr);
    CHECK(cb >= 0 && cb <= 255 && cr >= 0 && cr <= 255);
  }
}

// ------------------------------------------------------------------- waveform
static void testWaveformEmpty()
{
  CHECK(scopes::waveformCounts(nullptr).cols == 0);
  const CanvasImage none;
  CHECK(scopes::waveformCounts(&none).cols == 0);
  CHECK(!scopes::waveformImage(scopes::waveformCounts(&none)));

  // Only transparent pixels: nothing was counted.
  const CanvasImage clear = solid(4, 4, 200, 100, 50, 0);
  const scopes::Waveform wf = scopes::waveformCounts(&clear);
  CHECK(wf.cols == 0 && wf.total == 0);
  CHECK(!scopes::waveformImage(wf));
}

static void testWaveformCounts()
{
  CanvasImage img = solid(6, 3, 100, 150, 200);
  const scopes::Waveform wf = scopes::waveformCounts(&img);
  CHECK(wf.cols == 6 && wf.total == 18); // a narrow picture: one column per pixel column
  for (int c = 0; c < wf.cols; ++c)
  {
    CHECK(wf.colTotal[c] == 3);
    CHECK(wf.r[c * 256 + 100] == 3 && wf.g[c * 256 + 150] == 3 && wf.b[c * 256 + 200] == 3);
    CHECK(wf.r[c * 256 + 101] == 0);
  }
  const CanvasImage three = solid(3, 2, 9, 9, 9);
  CHECK(scopes::waveformCounts(&three).cols == 3);
}

static void testWaveformImage()
{
  CanvasImage img = solid(6, 3, 100, 150, 200);
  const auto pic = scopes::waveformImage(scopes::waveformCounts(&img));
  CHECK(pic && pic->width == 6 && pic->height == 256);
  if (!pic)
    return;
  for (int c = 0; c < 6; ++c)
  {
    // Level v is drawn at row 255 - v, in its own channel's color only.
    CHECK(at(*pic, c, 255 - 100) == (Color{255, scopes::kBgG, scopes::kBgB, 255}));
    CHECK(at(*pic, c, 255 - 150) == (Color{scopes::kBgR, 255, scopes::kBgB, 255}));
    CHECK(at(*pic, c, 255 - 200) == (Color{scopes::kBgR, scopes::kBgG, 255, 255}));
  }
  CHECK(lit(*pic) == 6 * 3);
  CHECK(at(*pic, 0, 0) == kBg && at(*pic, 5, 255) == kBg);
}

// Black on the left, white on the right: black sits on the bottom row of the
// left columns, white on the top row of the right ones, all channels together.
static void testWaveformPosition()
{
  CanvasImage img(8, 4);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 8; ++x)
      put(img, x, y, x < 4 ? 0 : 255, x < 4 ? 0 : 255, x < 4 ? 0 : 255);
  const auto pic = scopes::waveformImage(scopes::waveformCounts(&img));
  CHECK(pic && pic->width == 8);
  if (!pic)
    return;
  const Color white{255, 255, 255, 255};
  CHECK(at(*pic, 0, 255) == white && at(*pic, 3, 255) == white);
  CHECK(at(*pic, 4, 0) == white && at(*pic, 7, 0) == white);
  CHECK(at(*pic, 0, 0) == kBg && at(*pic, 7, 255) == kBg);
  CHECK(lit(*pic) == 8);
}

// A wide picture is folded into 256 columns by sample index, so the split is
// exact: the left 500 of 1000 pixel columns are scope columns 0..127.
static void testWaveformFolding()
{
  CanvasImage img(1000, 1000);
  for (int y = 0; y < 1000; ++y)
    for (int x = 0; x < 1000; ++x)
      put(img, x, y, x < 500 ? 10 : 240, x < 500 ? 10 : 240, x < 500 ? 10 : 240);
  const scopes::Waveform wf = scopes::waveformCounts(&img);
  CHECK(wf.cols == scopes::kWaveMaxCols && wf.total == 1000000u);
  uint64_t sum = 0;
  for (uint32_t t : wf.colTotal)
  {
    CHECK(t > 0);
    sum += t;
  }
  CHECK(sum == wf.total);
  CHECK(wf.r[127 * 256 + 10] > 0 && wf.r[127 * 256 + 240] == 0);
  CHECK(wf.r[128 * 256 + 240] > 0 && wf.r[128 * 256 + 10] == 0);
  const auto pic = scopes::waveformImage(wf);
  CHECK(pic && pic->width == 256 && pic->height == 256);
  if (pic)
  {
    CHECK(at(*pic, 127, 255 - 10).r == 255 && at(*pic, 128, 255 - 10) == kBg);
    CHECK(at(*pic, 128, 255 - 240).r == 255 && at(*pic, 127, 255 - 240) == kBg);
  }
}

// A few pixels of a column are dimmer than a whole column's worth.
static void testWaveformBrightness()
{
  CanvasImage img = solid(1, 100, 50, 50, 50);
  put(img, 0, 0, 200, 200, 200); // 1 of 100: share 0.01 -> sqrt(0.01 / 0.04) = 0.5
  const auto pic = scopes::waveformImage(scopes::waveformCounts(&img));
  CHECK(pic && pic->width == 1);
  if (!pic)
    return;
  CHECK_NEAR(at(*pic, 0, 255 - 200).r, 145, 1); // 34 + (255 - 34) * 0.5
  CHECK(at(*pic, 0, 255 - 50).r == 255);        // the other 99 % is full
}

static void testWaveformAlpha()
{
  CanvasImage img(2, 1);
  put(img, 0, 0, 5, 5, 5, 0); // straighten's empty corner
  put(img, 1, 0, 6, 6, 6, 1); // barely there, still counts
  const scopes::Waveform wf = scopes::waveformCounts(&img);
  CHECK(wf.cols == 2 && wf.total == 1);
  CHECK(wf.colTotal[0] == 0 && wf.colTotal[1] == 1);
  const auto pic = scopes::waveformImage(wf);
  CHECK(pic && lit(*pic) == 1);
}

// Past kHistMaxSamples the grid has a stride, like the histogram's.
static void testWaveformSampling()
{
  const CanvasImage big = solid(2000, 2000, 90, 90, 90); // stride 3: 667 x 667 samples
  const scopes::Waveform wf = scopes::waveformCounts(&big);
  CHECK(wf.cols == 256 && wf.total == 667u * 667u);
  for (uint32_t t : wf.colTotal)
    CHECK(t >= 1000);

  // A pixel off the grid is invisible to the scope (as to the histogram).
  CanvasImage off = solid(2000, 2000, 90, 90, 90);
  put(off, 1, 1, 255, 255, 255);
  CHECK(scopes::waveformCounts(&off).r[255] == 0);
  put(off, 3, 3, 255, 255, 255); // on the grid
  const scopes::Waveform on = scopes::waveformCounts(&off);
  uint32_t seen = 0;
  for (int c = 0; c < on.cols; ++c)
    seen += on.r[c * 256 + 255];
  CHECK(seen == 1);
}

// ---------------------------------------------------------------- vectorscope
static void testVectorEmpty()
{
  CHECK(scopes::vectorCounts(nullptr).total == 0);
  const CanvasImage none;
  CHECK(scopes::vectorCounts(&none).total == 0);
  const CanvasImage clear = solid(4, 4, 200, 100, 50, 0);
  const scopes::Vector v = scopes::vectorCounts(&clear);
  CHECK(v.total == 0 && v.n.empty());
  CHECK(!scopes::vectorImage(v));
  CHECK(!scopes::vectorImage(scopes::Vector{}));
}

static void testVectorCounts()
{
  CanvasImage img = solid(2, 2, 128, 128, 128);
  put(img, 1, 1, 255, 0, 0, 255);
  put(img, 0, 1, 10, 10, 10, 0); // transparent: not counted
  const scopes::Vector v = scopes::vectorCounts(&img);
  CHECK(v.total == 3 && v.n.size() == 128u * 128u);
  CHECK(v.n[64 * 128 + 64] == 2);  // gray: the center cell (Cb 128, Cr 128)
  CHECK(v.n[127 * 128 + 42] == 1); // red: Cb 85 -> 42, Cr 255 -> 127
  CHECK(std::accumulate(v.n.begin(), v.n.end(), 0u) == 3u);
}

static void testVectorImage()
{
  const CanvasImage gray = solid(4, 4, 128, 128, 128);
  const auto pic = scopes::vectorImage(scopes::vectorCounts(&gray));
  CHECK(pic && pic->width == 128 && pic->height == 128);
  if (!pic)
    return;
  CHECK(lit(*pic) == 1);
  // The center cell (x 64, y 64) is drawn at row 127 - 64, in the cell's own
  // color: gray at luma 180, full brightness.
  CHECK(at(*pic, 64, 63) == (Color{180, 180, 180, 255}));
}

// A lone red pixel among thousands of grays is at least kVecFloor bright, and
// red sits at the top, left of blue.
static void testVectorFloorAndHue()
{
  CanvasImage img = solid(100, 100, 128, 128, 128);
  put(img, 0, 0, 255, 0, 0);
  const auto pic = scopes::vectorImage(scopes::vectorCounts(&img));
  CHECK(pic);
  if (!pic)
    return;
  CHECK(lit(*pic) == 2);
  // Red: cell (42, 127) -> row 0. Floor 0.25 of the way from the background.
  const Color red = at(*pic, 42, 0);
  CHECK_NEAR(red.r, 89, 1); // 34 + (255 - 34) * 0.25
  CHECK_NEAR(red.g, 52, 1);
  CHECK_NEAR(red.b, 54, 1);
  CHECK(red.r > red.g && red.r > red.b);

  CanvasImage blue = solid(100, 100, 128, 128, 128);
  put(blue, 0, 0, 0, 0, 255);
  const auto pb = scopes::vectorImage(scopes::vectorCounts(&blue));
  CHECK(pb);
  if (pb)
  {
    // Blue: Cb 255 -> cell x 127, Cr 107 -> cell y 53 -> row 127 - 53.
    const Color c = at(*pb, 127, 127 - 53);
    CHECK(!(c == kBg) && c.b > c.r && c.b > c.g);
  }
}

// ------------------------------------------------------------------ render()
static void testRender()
{
  using scopes::Kind;
  const CanvasImage img = solid(6, 3, 100, 150, 200);
  CHECK(!scopes::render(Kind::Histogram, &img)); // the histogram is drawn from counts
  CHECK(!scopes::render(Kind::Count, &img));
  const auto w = scopes::render(Kind::Waveform, &img);
  CHECK(w && w->width == 6 && w->height == 256);
  const auto v = scopes::render(Kind::Vectorscope, &img);
  CHECK(v && v->width == 128 && v->height == 128);
  CHECK(!scopes::render(Kind::Waveform, nullptr));
  CHECK(!scopes::render(Kind::Vectorscope, nullptr));
  const CanvasImage none;
  CHECK(!scopes::render(Kind::Waveform, &none));
}

int main()
{
  testKinds();
  testSampleStep();
  testDensity();
  testCbCr();
  testWaveformEmpty();
  testWaveformCounts();
  testWaveformImage();
  testWaveformPosition();
  testWaveformFolding();
  testWaveformBrightness();
  testWaveformAlpha();
  testWaveformSampling();
  testVectorEmpty();
  testVectorCounts();
  testVectorImage();
  testVectorFloorAndHue();
  testRender();
  return finish();
}
