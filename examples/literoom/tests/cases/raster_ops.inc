// cases/raster_ops.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace raster_ops_cases
// so they cannot collide with other cases.
//
// raster_ops.hpp only needs liteui.hpp for the CanvasImage struct, so in CMake
// this test links liteui::liteui like the app does.
//
// Everything raster_ops.hpp does is tested here, on both picture types it is templated over:
// CanvasImage (RGBA8) in the first half, Image16 (RGBA 16-bit) after it, and golden fingerprints
// of the exact output bytes at the end.

#include "core/raster_ops.hpp"
#include "io/exif.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <utility>
#include <vector>

namespace raster_ops_cases
{
using Img = CanvasImage;

// Every pixel carries its own coordinates, so any move shows up exactly.
static Img coords(int w, int h)
{
  Img im(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &im.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x);
      p[1] = static_cast<uint8_t>(y);
      p[2] = static_cast<uint8_t>(x * 7 + y * 3);
      p[3] = 255;
    }
  return im;
}
static const uint8_t *px(const Img &im, int x, int y) { return &im.pixels[(static_cast<size_t>(y) * im.width + x) * 4]; }
static bool samePixel(const uint8_t *a, const uint8_t *b) { return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; }

TEST_CASE("raster_ops: crop", "[raster_ops]")
{
  const Img src = coords(40, 30);
  auto c = cropImage(src, CropRect{5, 7, 12, 9});
  CHECK((c->width == 12 && c->height == 9));
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 12; ++x)
      CHECK(samePixel(px(*c, x, y), px(src, x + 5, y + 7)));
  auto whole = cropImage(src, CropRect{0, 0, 40, 30});
  CHECK((whole->pixels == src.pixels));
  // Clamped to the picture: past the edge, and from a negative corner.
  auto over = cropImage(src, CropRect{30, 25, 100, 100});
  CHECK((over->width == 10 && over->height == 5 && samePixel(px(*over, 0, 0), px(src, 30, 25))));
  auto neg = cropImage(src, CropRect{-5, -5, 20, 20});
  CHECK((neg->width == 15 && neg->height == 15 && samePixel(px(*neg, 0, 0), px(src, 0, 0))));
  auto none = cropImage(src, CropRect{100, 100, 10, 10}); // entirely outside
  CHECK((none->width == 0 || none->height == 0));
  CHECK(none->pixels.empty());
}

TEST_CASE("raster_ops: quarterTurnsAndMirror", "[raster_ops]")
{
  const Img src = coords(7, 4);
  auto r = rotate90cw(src);
  CHECK((r->width == 4 && r->height == 7));
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 7; ++x)
      CHECK(samePixel(px(*r, 4 - 1 - y, x), px(src, x, y))); // (x, y) -> (h - 1 - y, x)
  auto r4 = r;
  for (int i = 0; i < 3; ++i)
    r4 = rotate90cw(*r4);
  CHECK((r4->width == 7 && r4->pixels == src.pixels));
  auto f = flipHorizontal(src);
  CHECK((f->width == 7 && f->height == 4));
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 7; ++x)
      CHECK(samePixel(px(*f, 7 - 1 - x, y), px(src, x, y)));
  CHECK((flipHorizontal(*f)->pixels == src.pixels));
}

TEST_CASE("raster_ops: orientMatchesTheGeoFrameAndExif", "[raster_ops]")
{
  const int W = 9, H = 5;
  auto src = std::make_shared<Img>(coords(W, H));
  CHECK(orientImage(src, Orient{}) == src); // identity: the very same image, no copy
  // A null picture comes back null (it used to be dereferenced), for a bare nullptr too.
  CHECK(orientImage(nullptr, Orient{1, true}) == nullptr);
  CHECK(orientImage(std::shared_ptr<Img>(), Orient{1, true}) == nullptr);
  for (int rot = 0; rot < 4; ++rot)
    for (bool flip : {false, true})
    {
      const Orient o{rot, flip};
      auto out = orientImage(src, o);
      const GeoFrame f = makeGeoFrame(W, H, o, 0.0f, false, CropRect{});
      CHECK((out->width == f.srcW && out->height == f.srcH));
      // Every original pixel center lands, per the frame, on the pixel that
      // orientImage put it in. This ties geometry.hpp to the pixel code.
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
          double sx, sy;
          f.toSrc(x + 0.5, y + 0.5, sx, sy);
          const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
          CHECK((ix >= 0 && ix < out->width && iy >= 0 && iy < out->height));
          CHECK(samePixel(px(*out, ix, iy), px(*src, x, y)));
        }
    }
  // EXIF orientation 6 ("rotate 90 clockwise to view") is rot = 1: the top-left
  // pixel ends up in the top-right corner.
  const exif::Transform t = exif::orientationToTransform(6);
  auto six = orientImage(src, Orient{t.rot, t.flip});
  CHECK(samePixel(px(*six, six->width - 1, 0), px(*src, 0, 0)));
  // Every EXIF value 1..8 gives a distinct orientation of the picture.
  std::vector<std::vector<uint8_t>> seen;
  for (int o = 1; o <= 8; ++o)
  {
    const exif::Transform tr = exif::orientationToTransform(o);
    auto img = orientImage(src, Orient{tr.rot, tr.flip});
    for (const auto &s : seen)
      CHECK((s != img->pixels || (img->width == 0)));
    seen.push_back(img->pixels);
  }
}

TEST_CASE("raster_ops: cropMatchesTheGeoFrame", "[raster_ops]")
{
  const int W = 40, H = 30;
  auto src = std::make_shared<Img>(coords(W, H));
  const CropRect crop{6, 4, 20, 15};
  const Orient o{1, true};
  auto oriented = orientImage(src, o);
  auto cropped = cropImage(*oriented, crop);
  const GeoFrame f = makeGeoFrame(W, H, o, 0.0f, false, crop);
  CHECK((cropped->width == f.srcW && cropped->height == f.srcH));
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
    {
      double sx, sy;
      f.toSrc(x + 0.5, y + 0.5, sx, sy);
      if (sx < 0 || sy < 0 || sx >= cropped->width || sy >= cropped->height)
        continue; // outside the crop
      CHECK(samePixel(px(*cropped, static_cast<int>(sx), static_cast<int>(sy)), px(*src, x, y)));
    }
}

TEST_CASE("raster_ops: rotateArbitrary", "[raster_ops]")
{
  const Img src = coords(60, 40);
  // Zero degrees: an exact copy (not the same object).
  auto same = rotateArbitrary(src, 0.0f, true);
  CHECK((same->pixels == src.pixels && same->width == 60));
  for (float deg : {-30.0f, 7.5f, 45.0f})
    for (bool expand : {false, true})
    {
      auto out = rotateArbitrary(src, deg, expand);
      const Dims d = rotatedDims(60, 40, deg, expand);
      CHECK((out->width == d.w && out->height == d.h));
    }
  // Expand: the corners are empty, the middle is opaque and is the picture's middle.
  auto e = rotateArbitrary(src, 30.0f, true);
  CHECK((px(*e, 0, 0)[3] == 0 && px(*e, e->width - 1, e->height - 1)[3] == 0));
  CHECK(px(*e, e->width / 2, e->height / 2)[3] == 255);
  // Fit: no empty corners at all.
  auto fit = rotateArbitrary(src, 20.0f, false);
  bool opaque = true;
  for (int y = 0; y < fit->height; ++y)
    for (int x = 0; x < fit->width; ++x)
      opaque = opaque && px(*fit, x, y)[3] == 255;
  CHECK(opaque);
  // ...and, the real property of fit: every output pixel centre maps back onto the picture (the
  // opaque check above cannot show it, because fit mode clamps every sample to the picture).
  {
    const GeoFrame ff = makeGeoFrame(60, 40, Orient{}, 20.0f, false, CropRect{});
    bool inside = true;
    for (int y = 0; y < fit->height; ++y)
      for (int x = 0; x < fit->width; ++x)
      {
        double ox, oy;
        ff.toOrig(x + 0.5, y + 0.5, ox, oy);
        inside = inside && ox >= -1.0 && ox <= 61.0 && oy >= -1.0 && oy <= 41.0;
      }
    CHECK(inside);
    // It is also a real fit, not a tiny one: the canvas takes up most of the picture.
    CHECK((fit->width * 2 > 60 && fit->height * 2 > 40));
  }
  // A smooth gradient comes through a turn in the right place: red = 4 * x of the
  // source, so a sampled output pixel must show the source column the frame maps
  // it to (bilinear on a linear ramp is exact, apart from rounding).
  Img ramp(60, 40);
  for (int y = 0; y < 40; ++y)
    for (int x = 0; x < 60; ++x)
    {
      uint8_t *p = &ramp.pixels[(static_cast<size_t>(y) * 60 + x) * 4];
      p[0] = static_cast<uint8_t>(x * 4);
      p[1] = static_cast<uint8_t>(y * 6);
      p[2] = 100;
      p[3] = 255;
    }
  for (float deg : {-25.0f, 12.0f})
  {
    auto out = rotateArbitrary(ramp, deg, true);
    const GeoFrame f = makeGeoFrame(60, 40, Orient{}, deg, true, CropRect{});
    for (int y = 4; y < out->height - 4; y += 3)
      for (int x = 4; x < out->width - 4; x += 3)
      {
        double ox, oy;
        f.toOrig(x + 0.5, y + 0.5, ox, oy);
        if (ox < 1.0 || oy < 1.0 || ox > 59.0 || oy > 39.0)
          continue; // near the border: partial coverage
        CHECK(px(*out, x, y)[0] == within((ox - 0.5) * 4.0, 2.0));
        CHECK(px(*out, x, y)[1] == within((oy - 0.5) * 6.0, 2.0));
      }
  }
  // Superseded work is abandoned: a stale generation returns null, a current one runs.
  std::atomic<uint64_t> latest{5};
  CHECK(rotateArbitrary(src, 10.0f, true, &latest, 4) == nullptr);
  CHECK(rotateArbitrary(src, 10.0f, true, &latest, 5) != nullptr);
}

TEST_CASE("raster_ops: preview", "[raster_ops]")
{
  CHECK(!isLargeImage(Img(1536, 100)));
  CHECK(isLargeImage(Img(1537, 100)));
  CHECK(isLargeImage(Img(100, 1537)));
  CHECK(makePreviewBase(Img(1536, 1000)) == nullptr); // not worth it
  // 1600 x 24: factor 2 -> 800 x 12. A 2 x 2 pattern averages with rounding.
  Img big(1600, 24);
  for (int y = 0; y < 24; ++y)
    for (int x = 0; x < 1600; ++x)
    {
      uint8_t *p = &big.pixels[(static_cast<size_t>(y) * 1600 + x) * 4];
      const bool odd = ((x + y) & 1) != 0;
      p[0] = odd ? 10 : 0;
      p[1] = odd ? 255 : 254;
      p[2] = 77;
      p[3] = odd ? 255 : 0;
    }
  auto pv = makePreviewBase(big);
  CHECK((pv && pv->width == 800 && pv->height == 12));
  if (pv)
  {
    const uint8_t *p = px(*pv, 10, 3);
    CHECK(p[0] == 5);   // (0 + 10 + 10 + 0 + 2) / 4
    CHECK(p[1] == 255); // (254 + 255 + 255 + 254 + 2) / 4
    CHECK(p[2] == 77);
    CHECK(p[3] == 128); // alpha is averaged too: (0 + 255 + 255 + 0 + 2) / 4
  }
  // The factor is the smallest whole number that gets the long side to 1024 or less.
  auto k3 = makePreviewBase(Img(3000, 10));
  CHECK((k3 && k3->width == 1000 && k3->height == 3));
  auto k2 = makePreviewBase(Img(2048, 100));
  CHECK((k2 && k2->width == 1024 && k2->height == 50));
  auto tall = makePreviewBase(Img(60, 4000));
  CHECK((tall && tall->width == 15 && tall->height == 1000));
}

// ---- Resize (Export: "Resize to long edge") ----
static Img solidImg(int w, int h, int r, int g, int b, int a = 255)
{
  Img im(w, h);
  for (int i = 0; i < w * h; ++i)
  {
    uint8_t *p = &im.pixels[static_cast<size_t>(i) * 4];
    p[0] = static_cast<uint8_t>(r);
    p[1] = static_cast<uint8_t>(g);
    p[2] = static_cast<uint8_t>(b);
    p[3] = static_cast<uint8_t>(a);
  }
  return im;
}

TEST_CASE("raster_ops: fitLongEdge", "[raster_ops]")
{
  auto fit = [](int w, int h, int e)
  { const Dims d = fitLongEdge(w, h, e); return std::make_pair(d.w, d.h); };
  using P = std::pair<int, int>;
  CHECK(fit(6000, 4000, 2048) == P(2048, 1365)); // 4000 * 2048 / 6000 = 1365.33
  CHECK(fit(4000, 6000, 2048) == P(1365, 2048)); // portrait: the height is the long side
  CHECK(fit(4000, 4000, 1000) == P(1000, 1000));
  CHECK(fit(3, 2, 1) == P(1, 1));        // 2 / 3 rounds to 1
  CHECK(fit(1000, 1, 100) == P(100, 1)); // the short side never reaches 0
  CHECK(fit(1, 1000, 100) == P(1, 100));
  // Never enlarges: already small enough, or exactly the size.
  CHECK(fit(3000, 2000, 3000) == P(3000, 2000));
  CHECK(fit(3000, 2000, 5000) == P(3000, 2000));
  CHECK(fit(2000, 3000, 3000) == P(2000, 3000));
  // Nonsense input comes back as it was.
  CHECK((fit(3000, 2000, 0) == P(3000, 2000) && fit(3000, 2000, -5) == P(3000, 2000)));
  CHECK((fit(0, 10, 5) == P(0, 10) && fit(10, 0, 5) == P(10, 0) && fit(-4, 10, 5) == P(-4, 10)));
  // The long side is exactly the target, and the shape is kept to within a pixel.
  for (int w : {7, 640, 4032, 6016, 9999})
    for (int h : {5, 480, 3024, 4000, 10001})
      for (int e : {1, 64, 1080, 2048})
      {
        const Dims d = fitLongEdge(w, h, e);
        if (std::max(w, h) <= e)
        {
          CHECK((d.w == w && d.h == h));
          continue;
        }
        CHECK((std::max(d.w, d.h) == e && d.w >= 1 && d.h >= 1));
        CHECK(((w >= h) == (d.w >= d.h) || d.w == d.h)); // orientation of the picture is kept
        // The short side is the exact scaled value to within half a pixel (or 1 at least).
        const double exactShort = static_cast<double>(std::min(w, h)) * e / std::max(w, h);
        const int gotShort = std::min(d.w, d.h);
        CHECK((gotShort == 1 || std::fabs(gotShort - exactShort) <= 0.5 + 1e-9));
      }
}

TEST_CASE("raster_ops: resizeArea", "[raster_ops]")
{
  // A flat opaque area keeps its exact value at any ratio, even awkward ones.
  for (int dw : {1, 2, 5, 11, 36, 37})
    for (int dh : {1, 3, 7, 22, 23})
    {
      const Img flat = solidImg(37, 23, 201, 99, 3);
      const auto out = resizeArea(flat, dw, dh);
      CHECK((out->width == dw && out->height == dh));
      bool same = true;
      for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
          same = same && px(*out, x, y)[0] == 201 && px(*out, x, y)[1] == 99 &&
                 px(*out, x, y)[2] == 3 && px(*out, x, y)[3] == 255;
      CHECK(same);
    }

  // Half the size is the plain 2 x 2 average, rounded half up.
  const Img src = coords(8, 6);
  const auto half = resizeArea(src, 4, 3);
  CHECK((half->width == 4 && half->height == 3));
  for (int y = 0; y < 3; ++y)
    for (int x = 0; x < 4; ++x)
      for (int c = 0; c < 4; ++c)
      {
        const int sum = px(src, 2 * x, 2 * y)[c] + px(src, 2 * x + 1, 2 * y)[c] +
                        px(src, 2 * x, 2 * y + 1)[c] + px(src, 2 * x + 1, 2 * y + 1)[c];
        CHECK(px(*half, x, y)[c] == (sum + 2) / 4);
      }

  // A fractional ratio weights the edge pixels: 3 -> 2 columns, each output pixel
  // covers 1.5 source pixels. (0 + 0.5 * 90) / 1.5 = 30 and (0.5 * 90 + 180) / 1.5 = 150.
  Img three(3, 1);
  const uint8_t v3[3] = {0, 90, 180};
  for (int x = 0; x < 3; ++x)
  {
    uint8_t *p = &three.pixels[static_cast<size_t>(x) * 4];
    p[0] = p[1] = p[2] = v3[x];
    p[3] = 255;
  }
  const auto two = resizeArea(three, 2, 1);
  CHECK((two->width == 2 && px(*two, 0, 0)[0] == 30 && px(*two, 1, 0)[0] == 150));
  CHECK((px(*two, 0, 0)[3] == 255 && px(*two, 1, 0)[1] == 150));

  // Transparent pixels add no color: red next to nothing stays red, at half the alpha.
  Img edge(2, 1);
  uint8_t *l = &edge.pixels[0], *r = &edge.pixels[4];
  l[0] = 255;
  l[1] = 0;
  l[2] = 0;
  l[3] = 255;
  r[0] = 0;
  r[1] = 0;
  r[2] = 0;
  r[3] = 0;
  const auto one = resizeArea(edge, 1, 1);
  CHECK((px(*one, 0, 0)[0] == 255 && px(*one, 0, 0)[1] == 0 && px(*one, 0, 0)[2] == 0));
  CHECK(px(*one, 0, 0)[3] == 128); // 127.5 rounds up
  // ...and a region with nothing visible comes out as empty, not as stray color.
  const Img clear = solidImg(6, 6, 200, 100, 50, 0);
  const auto cleared = resizeArea(clear, 2, 2);
  CHECK((px(*cleared, 0, 0)[0] == 0 && px(*cleared, 1, 1)[3] == 0));

  // A smooth ramp stays a ramp, in the same direction, with the same mean.
  Img ramp(90, 30);
  for (int y = 0; y < 30; ++y)
    for (int x = 0; x < 90; ++x)
    {
      uint8_t *p = &ramp.pixels[(static_cast<size_t>(y) * 90 + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / 89);
      p[1] = static_cast<uint8_t>(y * 255 / 29);
      p[2] = 10;
      p[3] = 255;
    }
  const auto smallImg = resizeArea(ramp, 31, 13);
  double meanBefore = 0, meanAfter = 0;
  for (int i = 0; i < 90 * 30; ++i)
    meanBefore += ramp.pixels[static_cast<size_t>(i) * 4];
  for (int i = 0; i < 31 * 13; ++i)
    meanAfter += smallImg->pixels[static_cast<size_t>(i) * 4];
  CHECK(meanBefore / (90 * 30) == within(meanAfter / (31 * 13), 1.5));
  for (int y = 0; y < 13; ++y)
    for (int x = 1; x < 31; ++x)
      CHECK(px(*smallImg, x, y)[0] >= px(*smallImg, x - 1, y)[0]);
  for (int x = 0; x < 31; ++x)
    for (int y = 1; y < 13; ++y)
      CHECK(px(*smallImg, x, y)[1] >= px(*smallImg, x, y - 1)[1]);
  CHECK((px(*smallImg, 0, 0)[0] < 12 && px(*smallImg, 30, 0)[0] > 243)); // not flipped

  // Same size: an equal copy, not the same object. Bad sizes: a copy of the source.
  const Img same = coords(5, 4);
  const auto copy = resizeArea(same, 5, 4);
  CHECK((copy->pixels == same.pixels && copy->width == 5));
  CHECK((resizeArea(same, 0, 3)->pixels == same.pixels));
  CHECK(resizeArea(same, 3, -1)->width == 5);
  CHECK(resizeArea(Img(0, 0), 4, 4)->pixels.empty());

  // Enlarging works too (blocky but in range, corners keep their color).
  const Img tiny = coords(3, 3);
  const auto big = resizeArea(tiny, 7, 5);
  CHECK((big->width == 7 && big->height == 5));
  CHECK((samePixel(px(*big, 0, 0), px(tiny, 0, 0)) && samePixel(px(*big, 6, 4), px(tiny, 2, 2))));
}

TEST_CASE("raster_ops: resizeToLongEdge", "[raster_ops]")
{
  auto src = std::make_shared<Img>(coords(120, 80));
  // Nothing to do: the very same image comes back (no copy).
  CHECK(resizeToLongEdge(src, 120) == src);
  CHECK(resizeToLongEdge(src, 500) == src);
  CHECK(resizeToLongEdge(src, 0) == src);
  CHECK(resizeToLongEdge(nullptr, 100) == nullptr);
  CHECK(resizeToLongEdge(std::shared_ptr<Img>(), 10) == nullptr); // a null picture of a known type
  const auto out = resizeToLongEdge(src, 60);
  CHECK((out != src && out->width == 60 && out->height == 40));
  CHECK((src->width == 120 && src->height == 80)); // the original is left alone
  const auto tall = resizeToLongEdge(std::make_shared<Img>(coords(80, 120)), 30);
  CHECK((tall->width == 20 && tall->height == 30));
  // The result is the same as asking resizeArea for the size fitLongEdge gives.
  const Dims d = fitLongEdge(120, 80, 45);
  CHECK((resizeToLongEdge(src, 45)->pixels == resizeArea(*src, d.w, d.h)->pixels));
}

// ---- Cancellation ----
TEST_CASE("raster_ops: cancellation", "[raster_ops]")
{
  const Img im = coords(40, 255);
  std::atomic<uint64_t> latest{5};
  // A superseded generation returns null, a current one runs; both for straighten modes.
  CHECK(rotateArbitrary(im, 7.0f, true, &latest, 4) == nullptr);
  CHECK(rotateArbitrary(im, 7.0f, false, &latest, 4) == nullptr);
  CHECK(rotateArbitrary(im, 7.0f, true, &latest, 5) != nullptr);
  CHECK(rotateArbitrary(im, 0.0f, false, &latest, 4) != nullptr); // 0 degrees is a copy: never checked
  // The same for a 16-bit picture.
  const Image16 w16(40, 255);
  CHECK(rotateArbitrary(w16, 7.0f, true, &latest, 4) == nullptr);
  CHECK(rotateArbitrary(w16, 7.0f, true, &latest, 5) != nullptr);
  // A generation that is already stale when the work starts is abandoned at the first check.
  std::atomic<uint64_t> moving{1};
  const Img tall = coords(8, 300);
  CHECK(rotateArbitrary(tall, 3.0f, false, &moving, 1) != nullptr);
  moving = 2;
  CHECK(rotateArbitrary(tall, 3.0f, false, &moving, 1) == nullptr);
}

// ====================================================================== Image16
// Pictures with 16-bit samples go through the same templates. Pure moves must be exact, averages
// exact to the 16-bit rounding, and detail below one 8-bit step must survive.
static Img randomImage(int w, int h, uint32_t seed, int alphaMode)
{
  Img im(w, h);
  std::mt19937 g(seed);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &im.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(g());
      p[1] = static_cast<uint8_t>(g());
      p[2] = static_cast<uint8_t>(g());
      // 0: opaque, 1: any alpha, 2: a quarter fully transparent, the rest mostly opaque
      p[3] = alphaMode == 0 ? 255 : alphaMode == 1 ? static_cast<uint8_t>(g())
                                                   : ((g() % 4) == 0 ? 0 : static_cast<uint8_t>(g() | 0x80));
    }
  return im;
}
static Image16 widen(const Img &a)
{
  Image16 o(a.width, a.height);
  widenSamples(a.pixels.data(), o.pixels.data(), a.pixels.size());
  return o;
}
static int maxDiff16(const Image16 &a, const Image16 &b)
{
  if (a.width != b.width || a.height != b.height || a.pixels.size() != b.pixels.size())
    return 1 << 30;
  int m = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i)
    m = std::max(m, std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i])));
  return m;
}

TEST_CASE("raster_ops: movesAreExact16", "[raster_ops]")
{
  const Img im = randomImage(23, 17, 11, 1);
  const Image16 w = widen(im);
  const auto shared = std::make_shared<Image16>(w);
  const auto shared8 = std::make_shared<Img>(im);
  for (const CropRect &r : {CropRect{3, 2, 9, 8}, CropRect{-5, -5, 12, 9}, CropRect{20, 15, 50, 50}, CropRect{1, 1, 0, 0}})
    CHECK(maxDiff16(*cropImage(w, r), widen(*cropImage(im, r))) == 0);
  CHECK(maxDiff16(*rotate90cw(w), widen(*rotate90cw(im))) == 0);
  CHECK(maxDiff16(*flipHorizontal(w), widen(*flipHorizontal(im))) == 0);
  for (int flip = 0; flip < 2; ++flip)
    for (int rot = 0; rot < 4; ++rot)
    {
      const Orient o{rot, flip != 0};
      const auto a = orientImage(shared, o);
      CHECK(maxDiff16(*a, widen(*orientImage(shared8, o))) == 0);
      CHECK((a == shared) == o.identity());
    }
  // rotateArbitrary by 0 is a copy, exactly.
  CHECK(maxDiff16(*rotateArbitrary(w, 0.0f, true), w) == 0);
  // Moves keep every low bit: a picture whose samples differ by 1 at 16 bits.
  Image16 fine(9, 6);
  for (size_t i = 0; i < fine.pixels.size(); ++i)
    fine.pixels[i] = static_cast<uint16_t>(30000 + i * 3 + (i % 5));
  const auto turned = rotate90cw(fine);
  for (int y = 0; y < fine.height; ++y)
    for (int x = 0; x < fine.width; ++x)
      for (int c = 0; c < 4; ++c)
        CHECK((turned->pixels[(static_cast<size_t>(x) * fine.height + (fine.height - 1 - y)) * 4 + c] ==
              fine.pixels[(static_cast<size_t>(y) * fine.width + x) * 4 + c]));
}

TEST_CASE("raster_ops: previewBase16", "[raster_ops]")
{
  const Img im = randomImage(2000, 30, 8, 1);
  const Image16 w = widen(im);
  const auto p = makePreviewBase(w);
  CHECK((p && p->width == 1000 && p->height == 15)); // k = 2
  if (!p)
    return;
  const int k = 2;
  for (int y = 0; y < p->height; ++y)
    for (int x = 0; x < p->width; ++x)
      for (int c = 0; c < 4; ++c)
      {
        int64_t s = 0; // the 8-bit sum: the 16-bit sum is exactly 257 times it
        for (int dy = 0; dy < k; ++dy)
          for (int dx = 0; dx < k; ++dx)
            s += im.pixels[(static_cast<size_t>(y * k + dy) * im.width + x * k + dx) * 4 + c];
        const int64_t area = k * k;
        CHECK((p->pixels[(static_cast<size_t>(y) * p->width + x) * 4 + c] == (257 * s + area / 2) / area));
      }
  CHECK(!makePreviewBase(Image16(1536, 4)));
  CHECK((isLargeImage(Image16(1537, 1)) && !isLargeImage(Image16(1536, 1))));
  // Detail below one 8-bit step: the average of 20000, 20001, 20002, 20003 is 20001.5 -> 20002 (8-bit
  // storage could only have said 78 * 257 = 20046 for all four).
  Image16 fine(1600, 2);
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 1600; ++x)
      for (int c = 0; c < 4; ++c)
        fine.pixels[(static_cast<size_t>(y) * 1600 + x) * 4 + c] = static_cast<uint16_t>(20000 + (c == 3 ? 0 : (x % 2) + 2 * y));
  const auto pf = makePreviewBase(fine);
  CHECK((pf && pf->pixels[0] == 20002 && pf->pixels[3] == 20000));
}

TEST_CASE("raster_ops: resizeAndRotate16", "[raster_ops]")
{
  for (int alpha = 0; alpha < 3; ++alpha)
  {
    const Img im = randomImage(31, 23, 40 + alpha, alpha);
    const Image16 w = widen(im);
    // The 16-bit result is the exact average; the 8-bit one is that, rounded to a byte (half a
    // level is 128.5 units of 16 bits, so a difference of up to 129 is the byte's own rounding).
    for (const auto &d : {std::pair{10, 7}, std::pair{15, 12}, std::pair{62, 46}, std::pair{1, 1}})
      CHECK(maxDiff16(*resizeArea(w, d.first, d.second), widen(*resizeArea(im, d.first, d.second))) <= 130);
    for (float deg : {0.1f, 5.0f, -12.3f, 30.0f, 45.0f})
      for (bool expand : {false, true})
      {
        const auto a = rotateArbitrary(w, deg, expand);
        const auto b = rotateArbitrary(im, deg, expand);
        CHECK((a && a->width == b->width && a->height == b->height));
        CHECK(maxDiff16(*a, widen(*b)) <= 131);
      }
  }
  // Flat opaque areas keep their exact 16-bit value, through both operations.
  Image16 flat(40, 30);
  for (size_t i = 0; i < flat.pixels.size(); i += 4)
  {
    flat.pixels[i] = 12345;
    flat.pixels[i + 1] = 40001;
    flat.pixels[i + 2] = 777;
    flat.pixels[i + 3] = 65535;
  }
  const auto rs = resizeArea(flat, 13, 9);
  for (size_t i = 0; i < rs->pixels.size(); i += 4)
    CHECK((rs->pixels[i] == 12345 && rs->pixels[i + 1] == 40001 && rs->pixels[i + 2] == 777 && rs->pixels[i + 3] == 65535));
  const auto rot = rotateArbitrary(flat, 3.7f, false); // "fit": every pixel lands inside the picture
  for (size_t i = 0; i < rot->pixels.size(); i += 4)
    CHECK((std::abs(rot->pixels[i] - 12345) <= 1 && std::abs(rot->pixels[i + 1] - 40001) <= 1 && std::abs(rot->pixels[i + 2] - 777) <= 1 && rot->pixels[i + 3] == 65535));
  // Expand: the empty corners are fully transparent (the new picture starts out zero-filled), the
  // centre is opaque.
  const auto ex = rotateArbitrary(flat, 30.0f, true);
  CHECK((ex->pixels[3] == 0));
  CHECK((ex->pixels[((static_cast<size_t>(ex->height / 2)) * ex->width + ex->width / 2) * 4 + 3] == 65535));
  // Detail below one 8-bit step survives a resize: a 2 x 1 picture 20000 | 20003 averaged to 1 x 1.
  Image16 two(2, 1);
  for (int x = 0; x < 2; ++x)
  {
    two.pixels[static_cast<size_t>(x) * 4] = static_cast<uint16_t>(20000 + x * 3);
    two.pixels[static_cast<size_t>(x) * 4 + 1] = two.pixels[static_cast<size_t>(x) * 4 + 2] = 100;
    two.pixels[static_cast<size_t>(x) * 4 + 3] = 65535;
  }
  CHECK((resizeArea(two, 1, 1)->pixels[0] == 20002)); // 20001.5 rounds up; an 8-bit pipeline would give 78 * 257
  // Out-of-range requests behave like the 8-bit ones.
  CHECK((resizeArea(two, 0, 3)->pixels == two.pixels));
  const auto none = resizeToLongEdge(std::make_shared<Image16>(two), 5);
  CHECK((none && none->width == 2));
  CHECK(resizeToLongEdge(std::shared_ptr<Image16>(), 5) == nullptr);
  const auto smallImg = resizeToLongEdge(std::make_shared<Image16>(flat), 20);
  CHECK((smallImg && smallImg->width == 20 && smallImg->height == 15));
}


} // namespace raster_ops_cases
