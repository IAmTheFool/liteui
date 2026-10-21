// Build: g++ -std=c++20 -I.. -I<folder with liteui.hpp> raster_ops_test.cpp -o raster_ops_test
// raster_ops.hpp only needs liteui.hpp for the CanvasImage struct, so in CMake
// this test links liteui::liteui like the app does.
#include "core/raster_ops.hpp"
#include "io/exif.hpp"
#include "check.hpp"

#include <atomic>
#include <random>

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

int main()
{
  testCrop();
  testQuarterTurnsAndMirror();
  testOrientMatchesTheGeoFrameAndExif();
  testCropMatchesTheGeoFrame();
  testRotateArbitrary();
  testPreview();
  return finish();
}
