// Build: g++ -std=c++20 -I.. -I<folder with liteui.hpp> raster_ops_test.cpp -o raster_ops_test
// raster_ops.hpp only needs liteui.hpp for the CanvasImage struct, so in CMake
// this test links liteui::liteui like the app does.
#include "core/raster_ops.hpp"
#include "io/exif.hpp"
#include "check.hpp"

#include <atomic>
#include <random>
#include <utility>

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

static void testCrop()
{
  const Img src = coords(40, 30);
  auto c = cropImage(src, CropRect{5, 7, 12, 9});
  CHECK(c->width == 12 && c->height == 9);
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 12; ++x)
      CHECK(samePixel(px(*c, x, y), px(src, x + 5, y + 7)));
  auto whole = cropImage(src, CropRect{0, 0, 40, 30});
  CHECK(whole->pixels == src.pixels);
  // Clamped to the picture: past the edge, and from a negative corner.
  auto over = cropImage(src, CropRect{30, 25, 100, 100});
  CHECK(over->width == 10 && over->height == 5 && samePixel(px(*over, 0, 0), px(src, 30, 25)));
  auto neg = cropImage(src, CropRect{-5, -5, 20, 20});
  CHECK(neg->width == 15 && neg->height == 15 && samePixel(px(*neg, 0, 0), px(src, 0, 0)));
  auto none = cropImage(src, CropRect{100, 100, 10, 10}); // entirely outside
  CHECK(none->width == 0 || none->height == 0);
  CHECK(none->pixels.empty());
}

static void testQuarterTurnsAndMirror()
{
  const Img src = coords(7, 4);
  auto r = rotate90cw(src);
  CHECK(r->width == 4 && r->height == 7);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 7; ++x)
      CHECK(samePixel(px(*r, 4 - 1 - y, x), px(src, x, y))); // (x, y) -> (h - 1 - y, x)
  auto r4 = r;
  for (int i = 0; i < 3; ++i)
    r4 = rotate90cw(*r4);
  CHECK(r4->width == 7 && r4->pixels == src.pixels);
  auto f = flipHorizontal(src);
  CHECK(f->width == 7 && f->height == 4);
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 7; ++x)
      CHECK(samePixel(px(*f, 7 - 1 - x, y), px(src, x, y)));
  CHECK(flipHorizontal(*f)->pixels == src.pixels);
}

static void testOrientMatchesTheGeoFrameAndExif()
{
  const int W = 9, H = 5;
  auto src = std::make_shared<Img>(coords(W, H));
  CHECK(orientImage(src, Orient{}) == src); // identity: the very same image, no copy
  for (int rot = 0; rot < 4; ++rot)
    for (bool flip : {false, true})
    {
      const Orient o{rot, flip};
      auto out = orientImage(src, o);
      const GeoFrame f = makeGeoFrame(W, H, o, 0.0f, false, CropRect{});
      CHECK(out->width == f.srcW && out->height == f.srcH);
      // Every original pixel center lands, per the frame, on the pixel that
      // orientImage put it in. This ties geometry.hpp to the pixel code.
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
          double sx, sy;
          f.toSrc(x + 0.5, y + 0.5, sx, sy);
          const int ix = static_cast<int>(std::floor(sx)), iy = static_cast<int>(std::floor(sy));
          CHECK(ix >= 0 && ix < out->width && iy >= 0 && iy < out->height);
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
      CHECK(s != img->pixels || (img->width == 0));
    seen.push_back(img->pixels);
  }
}

static void testCropMatchesTheGeoFrame()
{
  const int W = 40, H = 30;
  auto src = std::make_shared<Img>(coords(W, H));
  const CropRect crop{6, 4, 20, 15};
  const Orient o{1, true};
  auto oriented = orientImage(src, o);
  auto cropped = cropImage(*oriented, crop);
  const GeoFrame f = makeGeoFrame(W, H, o, 0.0f, false, crop);
  CHECK(cropped->width == f.srcW && cropped->height == f.srcH);
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

static void testRotateArbitrary()
{
  const Img src = coords(60, 40);
  // Zero degrees: an exact copy (not the same object).
  auto same = rotateArbitrary(src, 0.0f, true);
  CHECK(same->pixels == src.pixels && same->width == 60);
  for (float deg : {-30.0f, 7.5f, 45.0f})
    for (bool expand : {false, true})
    {
      auto out = rotateArbitrary(src, deg, expand);
      const Dims d = rotatedDims(60, 40, deg, expand);
      CHECK(out->width == d.w && out->height == d.h);
    }
  // Expand: the corners are empty, the middle is opaque and is the picture's middle.
  auto e = rotateArbitrary(src, 30.0f, true);
  CHECK(px(*e, 0, 0)[3] == 0 && px(*e, e->width - 1, e->height - 1)[3] == 0);
  CHECK(px(*e, e->width / 2, e->height / 2)[3] == 255);
  // Fit: no empty corners at all.
  auto fit = rotateArbitrary(src, 20.0f, false);
  bool opaque = true;
  for (int y = 0; y < fit->height; ++y)
    for (int x = 0; x < fit->width; ++x)
      opaque = opaque && px(*fit, x, y)[3] == 255;
  CHECK(opaque);
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
        CHECK_NEAR(px(*out, x, y)[0], (ox - 0.5) * 4.0, 2.0);
        CHECK_NEAR(px(*out, x, y)[1], (oy - 0.5) * 6.0, 2.0);
      }
  }
  // Superseded work is abandoned: a stale generation returns null, a current one runs.
  std::atomic<uint64_t> latest{5};
  CHECK(rotateArbitrary(src, 10.0f, true, &latest, 4) == nullptr);
  CHECK(rotateArbitrary(src, 10.0f, true, &latest, 5) != nullptr);
}

static void testPreview()
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
  CHECK(pv && pv->width == 800 && pv->height == 12);
  if (pv)
  {
    const uint8_t *p = px(*pv, 10, 3);
    CHECK(p[0] == 5);     // (0 + 10 + 10 + 0 + 2) / 4
    CHECK(p[1] == 255);   // (254 + 255 + 255 + 254 + 2) / 4
    CHECK(p[2] == 77);
    CHECK(p[3] == 128);   // alpha is averaged too: (0 + 255 + 255 + 0 + 2) / 4
  }
  // The factor is the smallest whole number that gets the long side to 1024 or less.
  auto k3 = makePreviewBase(Img(3000, 10));
  CHECK(k3 && k3->width == 1000 && k3->height == 3);
  auto k2 = makePreviewBase(Img(2048, 100));
  CHECK(k2 && k2->width == 1024 && k2->height == 50);
  auto tall = makePreviewBase(Img(60, 4000));
  CHECK(tall && tall->width == 15 && tall->height == 1000);
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

static void testFitLongEdge()
{
  auto fit = [](int w, int h, int e)
  { const Dims d = fitLongEdge(w, h, e); return std::make_pair(d.w, d.h); };
  using P = std::pair<int, int>;
  CHECK(fit(6000, 4000, 2048) == P(2048, 1365)); // 4000 * 2048 / 6000 = 1365.33
  CHECK(fit(4000, 6000, 2048) == P(1365, 2048)); // portrait: the height is the long side
  CHECK(fit(4000, 4000, 1000) == P(1000, 1000));
  CHECK(fit(3, 2, 1) == P(1, 1));                // 2 / 3 rounds to 1
  CHECK(fit(1000, 1, 100) == P(100, 1));         // the short side never reaches 0
  CHECK(fit(1, 1000, 100) == P(1, 100));
  // Never enlarges: already small enough, or exactly the size.
  CHECK(fit(3000, 2000, 3000) == P(3000, 2000));
  CHECK(fit(3000, 2000, 5000) == P(3000, 2000));
  CHECK(fit(2000, 3000, 3000) == P(2000, 3000));
  // Nonsense input comes back as it was.
  CHECK(fit(3000, 2000, 0) == P(3000, 2000) && fit(3000, 2000, -5) == P(3000, 2000));
  CHECK(fit(0, 10, 5) == P(0, 10) && fit(10, 0, 5) == P(10, 0) && fit(-4, 10, 5) == P(-4, 10));
  // The long side is exactly the target, and the shape is kept to within a pixel.
  for (int w : {7, 640, 4032, 6016, 9999})
    for (int h : {5, 480, 3024, 4000, 10001})
      for (int e : {1, 64, 1080, 2048})
      {
        const Dims d = fitLongEdge(w, h, e);
        if (std::max(w, h) <= e)
        {
          CHECK(d.w == w && d.h == h);
          continue;
        }
        CHECK(std::max(d.w, d.h) == e && d.w >= 1 && d.h >= 1);
        CHECK((w >= h) == (d.w >= d.h) || d.w == d.h); // orientation of the picture is kept
        // The short side is the exact scaled value to within half a pixel (or 1 at least).
        const double exactShort = static_cast<double>(std::min(w, h)) * e / std::max(w, h);
        const int gotShort = std::min(d.w, d.h);
        CHECK(gotShort == 1 || std::fabs(gotShort - exactShort) <= 0.5 + 1e-9);
      }
}

static void testResizeArea()
{
  // A flat opaque area keeps its exact value at any ratio, even awkward ones.
  for (int dw : {1, 2, 5, 11, 36, 37})
    for (int dh : {1, 3, 7, 22, 23})
    {
      const Img flat = solidImg(37, 23, 201, 99, 3);
      const auto out = resizeArea(flat, dw, dh);
      CHECK(out->width == dw && out->height == dh);
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
  CHECK(half->width == 4 && half->height == 3);
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
  CHECK(two->width == 2 && px(*two, 0, 0)[0] == 30 && px(*two, 1, 0)[0] == 150);
  CHECK(px(*two, 0, 0)[3] == 255 && px(*two, 1, 0)[1] == 150);

  // Transparent pixels add no color: red next to nothing stays red, at half the alpha.
  Img edge(2, 1);
  uint8_t *l = &edge.pixels[0], *r = &edge.pixels[4];
  l[0] = 255; l[1] = 0; l[2] = 0; l[3] = 255;
  r[0] = 0; r[1] = 0; r[2] = 0; r[3] = 0;
  const auto one = resizeArea(edge, 1, 1);
  CHECK(px(*one, 0, 0)[0] == 255 && px(*one, 0, 0)[1] == 0 && px(*one, 0, 0)[2] == 0);
  CHECK(px(*one, 0, 0)[3] == 128); // 127.5 rounds up
  // ...and a region with nothing visible comes out as empty, not as stray color.
  const Img clear = solidImg(6, 6, 200, 100, 50, 0);
  const auto cleared = resizeArea(clear, 2, 2);
  CHECK(px(*cleared, 0, 0)[0] == 0 && px(*cleared, 1, 1)[3] == 0);

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
  const auto small = resizeArea(ramp, 31, 13);
  double meanBefore = 0, meanAfter = 0;
  for (int i = 0; i < 90 * 30; ++i)
    meanBefore += ramp.pixels[static_cast<size_t>(i) * 4];
  for (int i = 0; i < 31 * 13; ++i)
    meanAfter += small->pixels[static_cast<size_t>(i) * 4];
  CHECK_NEAR(meanBefore / (90 * 30), meanAfter / (31 * 13), 1.5);
  for (int y = 0; y < 13; ++y)
    for (int x = 1; x < 31; ++x)
      CHECK(px(*small, x, y)[0] >= px(*small, x - 1, y)[0]);
  for (int x = 0; x < 31; ++x)
    for (int y = 1; y < 13; ++y)
      CHECK(px(*small, x, y)[1] >= px(*small, x, y - 1)[1]);
  CHECK(px(*small, 0, 0)[0] < 12 && px(*small, 30, 0)[0] > 243); // not flipped

  // Same size: an equal copy, not the same object. Bad sizes: a copy of the source.
  const Img same = coords(5, 4);
  const auto copy = resizeArea(same, 5, 4);
  CHECK(copy->pixels == same.pixels && copy->width == 5);
  CHECK(resizeArea(same, 0, 3)->pixels == same.pixels);
  CHECK(resizeArea(same, 3, -1)->width == 5);
  CHECK(resizeArea(Img(0, 0), 4, 4)->pixels.empty());

  // Enlarging works too (blocky but in range, corners keep their color).
  const Img tiny = coords(3, 3);
  const auto big = resizeArea(tiny, 7, 5);
  CHECK(big->width == 7 && big->height == 5);
  CHECK(samePixel(px(*big, 0, 0), px(tiny, 0, 0)) && samePixel(px(*big, 6, 4), px(tiny, 2, 2)));
}

static void testResizeToLongEdge()
{
  auto src = std::make_shared<Img>(coords(120, 80));
  // Nothing to do: the very same image comes back (no copy).
  CHECK(resizeToLongEdge(src, 120) == src);
  CHECK(resizeToLongEdge(src, 500) == src);
  CHECK(resizeToLongEdge(src, 0) == src);
  CHECK(resizeToLongEdge(nullptr, 100) == nullptr);
  const auto out = resizeToLongEdge(src, 60);
  CHECK(out != src && out->width == 60 && out->height == 40);
  CHECK(src->width == 120 && src->height == 80); // the original is left alone
  const auto tall = resizeToLongEdge(std::make_shared<Img>(coords(80, 120)), 30);
  CHECK(tall->width == 20 && tall->height == 30);
  // The result is the same as asking resizeArea for the size fitLongEdge gives.
  const Dims d = fitLongEdge(120, 80, 45);
  CHECK(resizeToLongEdge(src, 45)->pixels == resizeArea(*src, d.w, d.h)->pixels);
}

int main()
{
  testCrop();
  testQuarterTurnsAndMirror();
  testOrientMatchesTheGeoFrameAndExif();
  testCropMatchesTheGeoFrame();
  testRotateArbitrary();
  testPreview();
  testFitLongEdge();
  testResizeArea();
  testResizeToLongEdge();
  return finish();
}