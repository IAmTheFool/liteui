// cases/image_codec.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace image_codec_cases
// so they cannot collide with other cases.
//
// image_codec_test.cpp - how Literoom writes pictures through liteui's encoders (libpng / libjpeg on Linux,
// WIC on Windows): the CanvasImage / Image16 -> ImageBuffer copy (toImageBuffer), the PNG options an export
// uses (exportPngOptions) and the encoders' output, checked by decoding what they write with liteui's own
// decoders. There is no image_codec.hpp any more (Literoom's own encoders were replaced by liteui's); the file
// keeps its name so literoom_test.cpp's include line stays as it is. Links liteui like raster_ops_test does.

#include "cases/encode_helpers.hpp"
#include "core/image16_convert.hpp"
#include "io/color_tag.hpp"
#include "io/image_meta.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace image_codec_cases
{
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

// ------------------------------------------------------- toImageBuffer / options
TEST_CASE("image_codec: toImageBuffer", "[image_codec]")
{
  CanvasImage img(3, 2);
  for (size_t i = 0; i < img.pixels.size(); ++i)
    img.pixels[i] = static_cast<uint8_t>(i * 7 + 1);
  const auto b8 = toImageBuffer(img);
  REQUIRE(b8.has_value());
  CHECK((b8->width == 3 && b8->height == 2 && b8->format == PixelFormat::RGBA8_UNORM && b8->valid()));
  CHECK(b8->rowStride == 12); // tightly packed
  CHECK(std::memcmp(b8->pixels.data(), img.pixels.data(), img.pixels.size()) == 0);

  Image16 wide(2, 3);
  for (size_t i = 0; i < wide.pixels.size(); ++i)
    wide.pixels[i] = static_cast<uint16_t>(i * 2749 + 5);
  const auto b16 = toImageBuffer(wide);
  REQUIRE(b16.has_value());
  CHECK((b16->width == 2 && b16->height == 3 && b16->format == PixelFormat::RGBA16_UNORM && b16->valid()));
  CHECK(b16->rowStride == 16);
  CHECK(std::memcmp(b16->pixels.data(), wide.pixels.data(), wide.pixels.size() * 2) == 0); // host-order samples

  // Nothing to copy: empty, or a pixel vector shorter than the size says.
  CHECK(!toImageBuffer(CanvasImage()));
  CHECK(!toImageBuffer(Image16()));
  CanvasImage shortImg(4, 4);
  shortImg.pixels.resize(10);
  CHECK(!toImageBuffer(shortImg));
  Image16 shortWide(4, 4);
  shortWide.pixels.resize(10);
  CHECK(!toImageBuffer(shortWide));
}

TEST_CASE("image_codec: exportPngOptions", "[image_codec]")
{
  const auto o = exportPngOptions();
  CHECK(o.compression == 3);
  CHECK(!o.dropOpaqueAlpha);

  // A picture whose alpha is everywhere opaque: liteui's own default writes RGB (colour type 2), the
  // options an export uses keep RGBA (colour type 6).
  TempDir d("opts");
  const auto buf = toImageBuffer(solid(4, 4, 1, 2, 3));
  REQUIRE(buf.has_value());
  std::string err;
  CHECK(liteui_image::encodePng(d.file("default.png"), *buf, &err));
  CHECK(liteui_image::encodePng(d.file("export.png"), *buf, &err, exportPngOptions()));
  const Bytes a = slurp(d.file("default.png")), b = slurp(d.file("export.png"));
  REQUIRE((a.size() > 26 && b.size() > 26));
  CHECK(a[25] == 2);
  CHECK(b[25] == 6);
}

TEST_CASE("image_codec: jpegLimits", "[image_codec]")
{
  using liteui_image::jpegSizeOk;
  const uint32_t M = liteui_image::kJpegMaxSide;
  CHECK(M == 65500);
  CHECK((jpegSizeOk(1, 1) && jpegSizeOk(M, 1) && jpegSizeOk(1, M) && jpegSizeOk(M, M)));
  CHECK((!jpegSizeOk(M + 1, 1) && !jpegSizeOk(1, M + 1) && !jpegSizeOk(M + 1, M + 1)));
  CHECK((!jpegSizeOk(0, 5) && !jpegSizeOk(5, 0)));
  // A negative int (a bad size) turns into a huge unsigned one, which is refused too: the callers cast
  // an int size straight to uint32_t.
  CHECK((!jpegSizeOk(static_cast<uint32_t>(-1), 5) && !jpegSizeOk(5, static_cast<uint32_t>(-1))));
}

// -------------------------------------------------------------------- PNG
TEST_CASE("image_codec: pngRoundTrip", "[image_codec]")
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
  CHECK(encode_helpers::writePng8(p, img, &err));

  const Bytes b = slurp(p);
  const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  CHECK((b.size() > 33 && std::equal(sig, sig + 8, b.begin())));
  CHECK(std::equal(b.begin() + 12, b.begin() + 16, "IHDR")); // first chunk
  auto be32 = [&b](size_t o) { return (uint32_t{b[o]} << 24) | (uint32_t{b[o + 1]} << 16) | (uint32_t{b[o + 2]} << 8) | b[o + 3]; };
  CHECK((be32(16) == 7 && be32(20) == 5)); // width, height
  CHECK((b[24] == 8 && b[25] == 6));       // 8 bit, colour type 6 = RGBA

  std::string derr;
  const auto back = liteui_image::decodeFile(p, &derr);
  CHECK(back.has_value());
  if (back)
  {
    CHECK((back->width == 7 && back->height == 5));
    CHECK((back->pixels == img.pixels)); // lossless, alpha included
  }
}

TEST_CASE("image_codec: pngFailure", "[image_codec]")
{
  TempDir d("pngfail");
  const CanvasImage img = solid(4, 4, 1, 2, 3);
  std::string err;
  CHECK(!encode_helpers::writePng8(d.file("no_such_dir/out.png"), img, &err));
  CHECK(!err.empty());
  CHECK(!encode_helpers::writePng8(d.file("no_such_dir/out.png"), img, nullptr)); // null errorOut is fine
}

// ------------------------------------------------------------------- JPEG
TEST_CASE("image_codec: jpegRoundTrip", "[image_codec]")
{
  const CanvasImage img = gradient(64, 48);
  Bytes jpeg;
  std::string err;
  CHECK(encode_helpers::jpegBytes(img, 95, &jpeg, &err));
  CHECK(isJpegStream(jpeg));

  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size(), &err);
  CHECK(back.has_value());
  if (!back)
    return;
  CHECK((back->width == 64 && back->height == 48));
  int worst = 0;
  long sum = 0;
  for (size_t i = 0; i < img.pixels.size(); ++i)
  {
    if (i % 4 == 3)
    {
      CHECK((back->pixels[i] == 255)); // JPEG has no alpha
      continue;
    }
    const int e = std::abs(int{img.pixels[i]} - int{back->pixels[i]});
    worst = std::max(worst, e);
    sum += e;
  }
  CHECK(worst <= 12);
  CHECK(sum / (64 * 48 * 3) <= 3); // mean error
}

TEST_CASE("image_codec: jpegChannelOrder", "[image_codec]")
{
  Bytes jpeg;
  CHECK(encode_helpers::jpegBytes(solid(16, 16, 200, 40, 60), 95, &jpeg, nullptr));
  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK(back.has_value());
  if (!back)
    return;
  const uint8_t *p = at(*back, 8, 8);
  CHECK(p[0] == within(200, 10)); // a red/blue swap (WIC packs BGR) would show here
  CHECK(p[1] == within(40, 10));
  CHECK(p[2] == within(60, 10));
}

TEST_CASE("image_codec: jpegFlattensAlphaOnWhite", "[image_codec]")
{
  CanvasImage img = solid(16, 16, 30, 60, 90);
  for (int y = 0; y < 16; ++y)
    for (int x = 8; x < 16; ++x)
      put(img, x, y, 0, 0, 0, 0); // the right half is transparent
  Bytes jpeg;
  CHECK(encode_helpers::jpegBytes(img, 100, &jpeg, nullptr));
  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK(back.has_value());
  if (!back)
    return;
  const uint8_t *l = at(*back, 2, 8), *r = at(*back, 13, 8);
  CHECK(l[0] == within(30, 8));
  CHECK(l[2] == within(90, 8));
  CHECK((r[0] >= 245 && r[1] >= 245 && r[2] >= 245)); // white, not black

  Bytes half;
  CHECK(encode_helpers::jpegBytes(solid(16, 16, 255, 0, 0, 128), 100, &half, nullptr));
  const auto hb = liteui_image::decodeMemory(half.data(), half.size());
  CHECK(hb.has_value());
  if (hb)
  {
    const uint8_t *p = at(*hb, 8, 8); // 50 % red over white = (255, 127, 127)
    CHECK(p[0] == within(255, 8));
    CHECK(p[1] == within(127, 10));
    CHECK(p[2] == within(127, 10));
  }
}

TEST_CASE("image_codec: jpegQualityAndChroma", "[image_codec]")
{
  const CanvasImage img = noise(64, 64);
  // The quality argument is really used. Each pair is on one side of the 4:4:4
  // threshold (90), so chroma handling can't be what makes the difference.
  Bytes q100, q95, q80, q30;
  CHECK(encode_helpers::jpegBytes(img, 100, &q100, nullptr));
  CHECK(encode_helpers::jpegBytes(img, 95, &q95, nullptr));
  CHECK(encode_helpers::jpegBytes(img, 80, &q80, nullptr));
  CHECK(encode_helpers::jpegBytes(img, 30, &q30, nullptr));
  CHECK((isJpegStream(q100) && isJpegStream(q95) && isJpegStream(q80) && isJpegStream(q30)));
  CHECK(q100.size() > q95.size() * 23 / 20); // measured about 1.7x
  CHECK(q80.size() > q30.size() * 13 / 10);  // measured about 1.9x

  // One-pixel red / blue stripes: only full-resolution chroma (4:4:4, used from
  // quality 90 up) keeps them apart.
  CanvasImage stripes(32, 8);
  for (int y = 0; y < 8; ++y)
    for (int x = 0; x < 32; ++x)
      x % 2 == 0 ? put(stripes, x, y, 255, 0, 0) : put(stripes, x, y, 0, 0, 255);
  Bytes sj;
  CHECK(encode_helpers::jpegBytes(stripes, 95, &sj, nullptr));
  const auto back = liteui_image::decodeMemory(sj.data(), sj.size());
  CHECK(back.has_value());
  if (back)
  {
    const uint8_t *red = at(*back, 10, 4), *blue = at(*back, 11, 4);
    CHECK(int{red[0]} - int{red[2]} > 120);
    CHECK(int{blue[2]} - int{blue[0]} > 120);
  }
}

// The size limit is enforced by liteui's encoder itself, so no caller can skip it.
TEST_CASE("image_codec: jpegSizeLimit", "[image_codec]")
{
  const int M = static_cast<int>(liteui_image::kJpegMaxSide);
  // Exactly at the limit still encodes (1 pixel thick, so the buffers stay small).
  for (const CanvasImage &img : {solid(M, 1, 10, 20, 30), solid(1, M, 10, 20, 30)})
  {
    Bytes jpeg;
    std::string err;
    CHECK(encode_helpers::jpegBytes(img, 90, &jpeg, &err));
    CHECK(isJpegStream(jpeg));
  }

  // One past the limit, in either direction: refused, with an error and no output.
  for (const CanvasImage &img : {solid(M + 1, 1, 10, 20, 30), solid(1, M + 1, 10, 20, 30)})
  {
    Bytes jpeg;
    std::string err;
    CHECK(!encode_helpers::jpegBytes(img, 90, &jpeg, &err));
    CHECK(!err.empty());
    CHECK(jpeg.empty());
    CHECK(!encode_helpers::jpegBytes(img, 90, &jpeg, nullptr)); // null errorOut is fine
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
    CHECK((!encode_helpers::jpegBytes(w, 90, &jpeg, &err) && !err.empty() && jpeg.empty()));
    err.clear();
    CHECK((!encode_helpers::jpegBytes(h, 90, &jpeg, &err) && !err.empty() && jpeg.empty()));
  }
}

// A real encoder stream plus a source photo's metadata: still decodes, and the
// EXIF now says "upright".
TEST_CASE("image_codec: embedIntoRealEncoderOutput", "[image_codec]")
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
  CHECK((exifOrientationOf(srcPath).rot == 1 && !exifOrientationOf(srcPath).flip)); // reads as "turn 90 cw"

  Bytes jpeg;
  CHECK(encode_helpers::jpegBytes(gradient(40, 30), 92, &jpeg, nullptr));
  const Bytes plain = jpeg;
  CHECK(embedSourceMetadata(&jpeg, srcPath, 40, 30));
  CHECK(jpeg.size() > plain.size());
  CHECK(isJpegStream(jpeg));

  exif::JpegMeta meta;
  CHECK(exif::readJpegMeta(jpeg.data(), jpeg.size(), &meta));
  CHECK((meta.orientation == 1 && !meta.exif.empty()));

  const auto back = liteui_image::decodeMemory(jpeg.data(), jpeg.size());
  CHECK((back.has_value() && back->width == 40 && back->height == 30));
  const auto before = liteui_image::decodeMemory(plain.data(), plain.size());
  CHECK((back.has_value() && before.has_value() && back->pixels == before->pixels)); // pixels untouched
}

// A JPEG source's ICC profile becomes the iCCP chunk of a PNG export (io/color_tag.hpp), and the
// pixels of the export are untouched: liteui's decoder still reads the tagged file.
TEST_CASE("image_codec: colorTagsSurviveTheEncoder", "[image_codec]")
{
  TempDir d("tags");
  const CanvasImage img = noise(31, 17);
  std::string err;

  // A JPEG carrying an ICC profile (the bytes are made up: only that it is tagged matters here).
  Bytes jpeg;
  CHECK(encode_helpers::jpegBytes(gradient(40, 30), 92, &jpeg, nullptr));
  exif::JpegMeta meta;
  Bytes payload{'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, 1, 1};
  payload.insert(payload.end(), 200, 0x37);
  meta.icc.push_back(payload);
  jpeg = exif::embed(jpeg, meta, 40, 30);
  const std::string srcJpg = d.file("src.jpg");
  {
    std::ofstream f(fsPath(srcJpg), std::ios::binary);
    f.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }
  CHECK(color_tag::colorTagOf(srcJpg).text == "ICC profile (embedded profile, not sRGB)");

  // JPEG source -> PNG export.
  const std::string out = d.file("out.png");
  CHECK(encode_helpers::writePng8(out, img, &err));
  CHECK(color_tag::colorTagOf(out).text == "untagged (assumed sRGB)");
  CHECK(color_tag::carryColorToPng(out, srcJpg) == "ICC profile (embedded profile, not sRGB)");
  CHECK(color_tag::colorTagOf(out).text == "ICC profile (embedded profile, not sRGB)");
  const auto back = liteui_image::decodeFile(out, &err);
  CHECK((back.has_value() && back->width == 31 && back->height == 17));
  if (back)
    CHECK((back->pixels == img.pixels)); // lossless, and the tag changed no pixel

  // PNG source -> PNG export: its chunks are copied.
  const std::string out2 = d.file("out2.png");
  CHECK(encode_helpers::writePng8(out2, img, &err));
  CHECK(color_tag::carryColorToPng(out2, out) == "ICC profile (embedded profile, not sRGB)");
  const auto back2 = liteui_image::decodeFile(out2, &err);
  CHECK((back2.has_value() && back2->pixels == img.pixels));

  // An untagged source writes nothing: the export stays byte for byte what the encoder wrote.
  const std::string plain = d.file("plain.png"), out3 = d.file("out3.png");
  CHECK((encode_helpers::writePng8(plain, img, &err) && encode_helpers::writePng8(out3, img, &err)));
  const Bytes before = slurp(out3);
  CHECK(color_tag::carryColorToPng(out3, plain).empty());
  CHECK(slurp(out3) == before);
}
} // namespace image_codec_cases
