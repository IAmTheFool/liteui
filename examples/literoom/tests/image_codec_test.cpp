// image_codec_test.cpp - flattenRow (pure) and the platform encoders (libpng /
// libjpeg on Linux, WIC on Windows), checked by decoding what they write with
// liteui's own decoders. Links liteui like raster_ops_test does.
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "io/image_codec.hpp"
#include "io/image_meta.hpp"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

struct TempDir
{
  fs::path path;
  explicit TempDir(const char *tag)
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           (std::string("literoom_ic_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string file(const std::string &name) const { return u8ToString(path / fsPath(name)); }
};

static Bytes slurp(const std::string &path)
{
  std::ifstream f(fsPath(path), std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static void put(CanvasImage &img, int x, int y, int r, int g, int b, int a = 255)
{
  uint8_t *p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
  p[0] = static_cast<uint8_t>(r);
  p[1] = static_cast<uint8_t>(g);
  p[2] = static_cast<uint8_t>(b);
  p[3] = static_cast<uint8_t>(a);
}

static const uint8_t *at(const CanvasImage &img, int x, int y)
{
  return &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
}

static CanvasImage solid(int w, int h, int r, int g, int b, int a = 255)
{
  CanvasImage img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      put(img, x, y, r, g, b, a);
  return img;
}

// Smooth ramps in all three channels: easy for JPEG, wrong channel order shows.
static CanvasImage gradient(int w, int h)
{
  CanvasImage img(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      put(img, x, y, x * 255 / (w - 1), y * 255 / (h - 1), 60 + (x + y) * 100 / (w + h));
  return img;
}

// Deterministic pseudo-random noise: hard for JPEG, so quality changes the size.
static CanvasImage noise(int w, int h)
{
  CanvasImage img(w, h);
  uint32_t s = 12345;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      s = s * 1664525u + 1013904223u;
      put(img, x, y, (s >> 8) & 255, (s >> 16) & 255, (s >> 24) & 255);
    }
  return img;
}

static bool isJpegStream(const Bytes &b)
{
  return b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8 && b[b.size() - 2] == 0xFF && b[b.size() - 1] == 0xD9;
}

// ------------------------------------------------------------- flattenRow
static void testFlattenRow()
{
  CanvasImage img(5, 2);
  put(img, 0, 0, 10, 20, 30);              // opaque
  put(img, 1, 0, 11, 22, 33, 0);           // fully transparent -> white
  put(img, 2, 0, 0, 100, 200, 128);        // half transparent
  put(img, 3, 0, 255, 128, 0, 254);        // almost opaque
  put(img, 4, 0, 1, 2, 3, 128);            // rounds up: (v*128 + 255*127 + 127) / 255
  put(img, 0, 1, 1, 2, 3);                 // second row: only used when asked for
  put(img, 1, 1, 0, 0, 0, 1);              // nearly invisible
  put(img, 2, 1, 255, 255, 255, 1);
  put(img, 3, 1, 200, 100, 50);
  put(img, 4, 1, 0, 0, 0);

  uint8_t rgb[5 * 3 + 4];
  std::fill(std::begin(rgb), std::end(rgb), uint8_t{0xEE}); // sentinels past the row
  flattenRow(img, 0, rgb, false);
  const uint8_t want0[] = {10, 20, 30,   255, 255, 255,   127, 177, 227,   255, 128, 1,   128, 128, 129};
  CHECK(std::equal(want0, want0 + 15, rgb));
  CHECK(rgb[15] == 0xEE && rgb[18] == 0xEE); // writes exactly 3 bytes per pixel

  uint8_t bgr[5 * 3];
  flattenRow(img, 0, bgr, true);
  for (int x = 0; x < 5; ++x)
  {
    CHECK(bgr[x * 3 + 0] == rgb[x * 3 + 2]); // B and R swapped...
    CHECK(bgr[x * 3 + 1] == rgb[x * 3 + 1]); // ...G stays
    CHECK(bgr[x * 3 + 2] == rgb[x * 3 + 0]);
  }

  flattenRow(img, 1, rgb, false); // picks the requested row
  const uint8_t want1[] = {1, 2, 3,   254, 254, 254,   255, 255, 255,   200, 100, 50,   0, 0, 0};
  CHECK(std::equal(want1, want1 + 15, rgb));

  CHECK(jpegFullChroma(100) && jpegFullChroma(92) && jpegFullChroma(90));
  CHECK(!jpegFullChroma(89) && !jpegFullChroma(50) && !jpegFullChroma(1));
  CHECK(kJpegQualityDefault == 92 && kJpegMaxSide == 65500);

  const int M = kJpegMaxSide;
  CHECK(jpegSizeOk(1, 1) && jpegSizeOk(M, 1) && jpegSizeOk(1, M) && jpegSizeOk(M, M));
  CHECK(!jpegSizeOk(M + 1, 1) && !jpegSizeOk(1, M + 1) && !jpegSizeOk(M + 1, M + 1));
  CHECK(!jpegSizeOk(0, 5) && !jpegSizeOk(5, 0) && !jpegSizeOk(-1, 5) && !jpegSizeOk(5, -1));
}

// -------------------------------------------------------------------- PNG
static void testPngRoundTrip()
{
  TempDir d("png");
  CanvasImage img(7, 5); // odd sizes on purpose
  uint32_t s = 99;
  for (int y = 0; y < img.height; ++y)
    for (int x = 0; x < img.width; ++x)
    {
      s = s * 1664525u + 1013904223u;
      put(img, x, y, (s >> 8) & 255, (s >> 16) & 255, (s >> 24) & 255, (s >> 4) & 255);
    }
  put(img, 0, 0, 12, 34, 56, 0);   // transparent pixel: colour kept (straight alpha)
  put(img, 1, 0, 200, 150, 100, 255);
  put(img, 2, 0, 200, 150, 100, 128);

  const std::string p = d.file("out.png");
  std::string err;
  CHECK(encodePngFile(p, img, &err));

  const Bytes b = slurp(p);
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CHECK(b.size() > 33 && std::equal(sig, sig + 8, b.begin()));
  CHECK(std::equal(b.begin() + 12, b.begin() + 16, "IHDR")); // first chunk
  auto be32 = [&b](size_t o) { return (uint32_t{b[o]} << 24) | (uint32_t{b[o + 1]} << 16) | (uint32_t{b[o + 2]} << 8) | b[o + 3]; };
  CHECK(be32(16) == 7 && be32(20) == 5); // width, height
  CHECK(b[24] == 8 && b[25] == 6);       // 8 bit, colour type 6 = RGBA

  std::string derr;
  const auto back = liteui_image::decodeFile(p, &derr);
  CHECK(back.has_value());
  if (back)
  {
    CHECK(back->width == 7 && back->height == 5);
    CHECK(back->pixels == img.pixels); // lossless, alpha included
  }
}

static void testPngFailure()
{
  TempDir d("pngfail");
  const CanvasImage img = solid(4, 4, 1, 2, 3);
  std::string err;
  CHECK(!encodePngFile(d.file("no_such_dir/out.png"), img, &err));
  CHECK(!err.empty());
  CHECK(!encodePngFile(d.file("no_such_dir/out.png"), img, nullptr)); // null errorOut is fine
}

// ------------------------------------------------------------------- JPEG
static void testJpegRoundTrip()
{
  const CanvasImage img = gradient(64, 48);
  Bytes jpeg;
  std::string err;
  CHECK(encodeJpegMemory(img, 95, &jpeg, &err));
  CHECK(isJpegStream(jpeg));

  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size(), &err);
  CHECK(back.has_value());
  if (!back)
    return;
  CHECK(back->width == 64 && back->height == 48);
  int worst = 0;
  long sum = 0;
  for (size_t i = 0; i < img.pixels.size(); ++i)
  {
    if (i % 4 == 3)
    {
      CHECK(back->pixels[i] == 255); // JPEG has no alpha
      continue;
    }
    const int e = std::abs(int{img.pixels[i]} - int{back->pixels[i]});
    worst = std::max(worst, e);
    sum += e;
  }
  CHECK(worst <= 12);
  CHECK(sum / (64 * 48 * 3) <= 3); // mean error
}

static void testJpegChannelOrder()
{
  Bytes jpeg;
  CHECK(encodeJpegMemory(solid(16, 16, 200, 40, 60), 95, &jpeg, nullptr));
  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK(back.has_value());
  if (!back)
    return;
  const uint8_t *p = at(*back, 8, 8);
  CHECK_NEAR(p[0], 200, 10); // a red/blue swap (the WIC path flattens to BGR) would show here
  CHECK_NEAR(p[1], 40, 10);
  CHECK_NEAR(p[2], 60, 10);
}

static void testJpegFlattensAlphaOnWhite()
{
  CanvasImage img = solid(16, 16, 30, 60, 90);
  for (int y = 0; y < 16; ++y)
    for (int x = 8; x < 16; ++x)
      put(img, x, y, 0, 0, 0, 0); // the right half is transparent
  Bytes jpeg;
  CHECK(encodeJpegMemory(img, 100, &jpeg, nullptr));
  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK(back.has_value());
  if (!back)
    return;
  const uint8_t *l = at(*back, 2, 8), *r = at(*back, 13, 8);
  CHECK_NEAR(l[0], 30, 8);
  CHECK_NEAR(l[2], 90, 8);
  CHECK(r[0] >= 245 && r[1] >= 245 && r[2] >= 245); // white, not black

  Bytes half;
  CHECK(encodeJpegMemory(solid(16, 16, 255, 0, 0, 128), 100, &half, nullptr));
  const auto hb = liteui_image::decodeMemory(half.data(), half.size());
  CHECK(hb.has_value());
  if (hb)
  {
    const uint8_t *p = at(*hb, 8, 8); // 50 % red over white = (255, 127, 127)
    CHECK_NEAR(p[0], 255, 8);
    CHECK_NEAR(p[1], 127, 10);
    CHECK_NEAR(p[2], 127, 10);
  }
}

static void testJpegQualityAndChroma()
{
  const CanvasImage img = noise(64, 64);
  // The quality argument is really used. Each pair is on one side of the 4:4:4
  // threshold (90), so chroma handling can't be what makes the difference.
  Bytes q100, q95, q80, q30;
  CHECK(encodeJpegMemory(img, 100, &q100, nullptr));
  CHECK(encodeJpegMemory(img, 95, &q95, nullptr));
  CHECK(encodeJpegMemory(img, 80, &q80, nullptr));
  CHECK(encodeJpegMemory(img, 30, &q30, nullptr));
  CHECK(isJpegStream(q100) && isJpegStream(q95) && isJpegStream(q80) && isJpegStream(q30));
  CHECK(q100.size() > q95.size() * 23 / 20); // measured about 1.7x
  CHECK(q80.size() > q30.size() * 13 / 10);  // measured about 1.9x

  // One-pixel red / blue stripes: only full-resolution chroma (4:4:4, used from
  // quality 90 up) keeps them apart.
  CanvasImage stripes(32, 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 32; ++x)
      x % 2 == 0 ? put(stripes, x, y, 255, 0, 0) : put(stripes, x, y, 0, 0, 255);
  Bytes sj;
  CHECK(encodeJpegMemory(stripes, 95, &sj, nullptr));
  const auto back = liteui_image::decodeMemory(sj.data(), sj.size());
  CHECK(back.has_value());
  if (back)
  {
    const uint8_t *red = at(*back, 10, 4), *blue = at(*back, 11, 4);
    CHECK(int{red[0]} - int{red[2]} > 120);
    CHECK(int{blue[2]} - int{blue[0]} > 120);
  }
}

// The size limit is enforced by the encoder itself, so no caller can skip it.
static void testJpegSizeLimit()
{
  const int M = kJpegMaxSide;
  // Exactly at the limit still encodes (1 pixel thick, so the buffers stay small).
  for (const CanvasImage &img : {solid(M, 1, 10, 20, 30), solid(1, M, 10, 20, 30)})
  {
    Bytes jpeg;
    std::string err;
    CHECK(encodeJpegMemory(img, 90, &jpeg, &err));
    CHECK(isJpegStream(jpeg));
  }

  // One past the limit, in either direction: refused, with an error and no output.
  for (const CanvasImage &img : {solid(M + 1, 1, 10, 20, 30), solid(1, M + 1, 10, 20, 30)})
  {
    Bytes jpeg;
    std::string err;
    CHECK(!encodeJpegMemory(img, 90, &jpeg, &err));
    CHECK(!err.empty());
    CHECK(jpeg.empty());
    CHECK(!encodeJpegMemory(img, 90, &jpeg, nullptr)); // null errorOut is fine
  }

  // Empty or negative sizes: the guard runs before any pixel is read, so the
  // buffer behind a bogus width / height is never touched.
  for (int bad : {0, -1})
  {
    CanvasImage w = solid(4, 4, 1, 2, 3), h = solid(4, 4, 1, 2, 3);
    w.width = bad;
    h.height = bad;
    Bytes jpeg;
    std::string err;
    CHECK(!encodeJpegMemory(w, 90, &jpeg, &err) && !err.empty() && jpeg.empty());
    err.clear();
    CHECK(!encodeJpegMemory(h, 90, &jpeg, &err) && !err.empty() && jpeg.empty());
  }
}

// A real encoder stream plus a source photo's metadata: still decodes, and the
// EXIF now says "upright".
static void testEmbedIntoRealEncoderOutput()
{
  TempDir d("embed");
  // Smallest EXIF that matters: one IFD0 entry, Orientation = 6 (turn 90 cw).
  Bytes tiff{'I', 'I', 42, 0, 8, 0, 0, 0,  1, 0,  0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0,  0, 0, 0, 0};
  Bytes payload{'E', 'x', 'i', 'f', 0, 0};
  payload.insert(payload.end(), tiff.begin(), tiff.end());
  const size_t len = payload.size() + 2;
  Bytes src{0xFF, 0xD8, 0xFF, 0xE1, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
  src.insert(src.end(), payload.begin(), payload.end());
  src.push_back(0xFF);
  src.push_back(0xD9);
  const std::string srcPath = d.file("photo.jpg");
  {
    std::ofstream f(fsPath(srcPath), std::ios::binary);
    f.write(reinterpret_cast<const char *>(src.data()), static_cast<std::streamsize>(src.size()));
  }
  CHECK(exifOrientationOf(srcPath).rot == 1 && !exifOrientationOf(srcPath).flip); // reads as "turn 90 cw"

  Bytes jpeg;
  CHECK(encodeJpegMemory(gradient(40, 30), 92, &jpeg, nullptr));
  const Bytes plain = jpeg;
  CHECK(embedSourceMetadata(&jpeg, srcPath, 40, 30));
  CHECK(jpeg.size() > plain.size());
  CHECK(isJpegStream(jpeg));

  exif::JpegMeta meta;
  CHECK(exif::readJpegMeta(jpeg.data(), jpeg.size(), &meta));
  CHECK(meta.orientation == 1 && !meta.exif.empty());

  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK(back.has_value() && back->width == 40 && back->height == 30);
  const auto before = liteui_image::decodeMemory(plain.data(), plain.size());
  CHECK(back.has_value() && before.has_value() && back->pixels == before->pixels); // pixels untouched
}

int main()
{
  testFlattenRow();
  testPngRoundTrip();
  testPngFailure();
  testJpegRoundTrip();
  testJpegChannelOrder();
  testJpegFlattensAlphaOnWhite();
  testJpegQualityAndChroma();
  testJpegSizeLimit();
  testEmbedIntoRealEncoderOutput();
  return finish();
}