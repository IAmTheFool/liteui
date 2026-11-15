// cases/image_source.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace image_source_cases
// so they cannot collide with other cases.
//
// image_source_test.cpp - how a picture file is opened (io/image_source.hpp): wide or narrow by the
// file's header, upright by the orientation it is given, one decode per open, the header-only size,
// the same refusal of a damaged PNG on every platform, and the errors. Real files in the temp folder; links LiteUI like
// image_codec16_test.

#include "io/image_codec16.hpp"
#include "cases/encode_helpers.hpp"
#include "io/image_source.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace image_source_cases
{
namespace fs = std::filesystem;

static std::string tmp(const char *name)
{
  return (fs::temp_directory_path() / (std::string("literoom_image_source_test_") + name)).string();
}

// A file that is removed when the test is done, whatever happens.
struct TempFile
{
  std::string path;
  explicit TempFile(const char *name) : path(tmp(name)) {}
  ~TempFile()
  {
    std::error_code ec;
    fs::remove(fs::path(path), ec);
  }
};

// Samples that are NOT multiples of 257, so a picture that went through 8 bits would be caught.
static Image16 widePicture(int w, int h)
{
  Image16 im(w, h);
  for (size_t i = 0; i < im.pixels.size(); ++i)
    im.pixels[i] = static_cast<uint16_t>((i * 2654435761u >> 7) & 0xFFFF);
  for (size_t i = 3; i < im.pixels.size(); i += 4)
    im.pixels[i] = 65535; // opaque
  return im;
}
static CanvasImage narrowPicture(int w, int h)
{
  CanvasImage im(w, h);
  for (size_t i = 0; i < im.pixels.size(); ++i)
    im.pixels[i] = static_cast<uint8_t>(i * 37u + 11u);
  for (size_t i = 3; i < im.pixels.size(); i += 4)
    im.pixels[i] = 255;
  return im;
}

TEST_CASE("image_source: depthFollowsTheHeader", "[image_source]")
{
  TempFile wide("depth16.png"), narrow("depth8.png"), junk("depth_junk.png");
  std::string err;
  CHECK(encode_helpers::writePng16(wide.path, widePicture(7, 5), &err));
  CHECK(encode_helpers::writePng8(narrow.path, narrowPicture(7, 5), &err));
  {
    std::ofstream f(junk.path, std::ios::binary);
    f << "this is not a picture";
  }
  CHECK(image_source::depthOf(wide.path) == image_source::Depth::Wide);
  CHECK(image_source::depthOf(narrow.path) == image_source::Depth::Narrow);
  CHECK(image_source::depthOf(junk.path) == image_source::Depth::Narrow);
  CHECK(image_source::depthOf(tmp("does_not_exist.png")) == image_source::Depth::Narrow);
}

TEST_CASE("image_source: wideFileKeepsItsPrecision", "[image_source]")
{
  TempFile f("wide.png");
  const Image16 src = widePicture(7, 5);
  std::string err;
  CHECK(encode_helpers::writePng16(f.path, src, &err));
  const auto o = image_source::openUpright(f.path, Orient{}, /*want8=*/true, &err);
  CHECK(o.has_value());
  if (!o)
    return;
  CHECK((o->wide && o->image));
  if (!(o->wide && o->image))
    return;
  CHECK((o->wide->width == 7 && o->wide->height == 5));
  CHECK((o->wide->pixels == src.pixels)); // the file's 16-bit samples, untouched
  // The 8-bit form is the nearest byte of the 16-bit samples.
  const CanvasImage expect = narrowImage(*o->wide);
  CHECK((o->image->width == 7 && o->image->height == 5 && o->image->pixels == expect.pixels));
  // Without want8 no 8-bit copy is made.
  const auto o2 = image_source::openUpright(f.path, Orient{}, /*want8=*/false, &err);
  CHECK((o2 && o2->wide && !o2->image));
}

TEST_CASE("image_source: narrowFileHasNoTwin", "[image_source]")
{
  TempFile f("narrow.png");
  const CanvasImage src = narrowPicture(7, 5);
  std::string err;
  CHECK(encode_helpers::writePng8(f.path, src, &err));
  for (const bool want8 : {true, false})
  {
    const auto o = image_source::openUpright(f.path, Orient{}, want8, &err);
    CHECK((o && !o->wide && o->image));
    if (o && o->image)
      CHECK((o->image->width == 7 && o->image->height == 5 && o->image->pixels == src.pixels));
  }
}

TEST_CASE("image_source: orientationIsApplied", "[image_source]")
{
  TempFile w("orient16.png"), n("orient8.png");
  std::string err;
  CHECK(encode_helpers::writePng16(w.path, widePicture(7, 5), &err));
  CHECK(encode_helpers::writePng8(n.path, narrowPicture(7, 5), &err));
  const Orient turn{1, false}; // a quarter turn: 7 x 5 becomes 5 x 7
  const auto ow = image_source::openUpright(w.path, turn, true, &err);
  CHECK((ow && ow->wide && ow->image));
  if (ow && ow->wide && ow->image)
  {
    CHECK((ow->wide->width == 5 && ow->wide->height == 7));
    CHECK((ow->image->width == 5 && ow->image->height == 7));
    CHECK((ow->image->pixels == narrowImage(*ow->wide).pixels)); // the 8-bit form is of the turned picture
  }
  const auto on = image_source::openUpright(n.path, turn, true, &err);
  CHECK((on && on->image && on->image->width == 5 && on->image->height == 7));
}

TEST_CASE("image_source: failures", "[image_source]")
{
  // Missing file, and a file that is no picture: nullopt with a message.
  std::string err;
  CHECK(!image_source::openUpright(tmp("does_not_exist.png"), Orient{}, true, &err));
  CHECK(!err.empty());
  TempFile junk("junk.png");
  {
    std::ofstream f(junk.path, std::ios::binary);
    f << "not a picture at all";
  }
  err.clear();
  CHECK(!image_source::openUpright(junk.path, Orient{}, true, &err));
  CHECK(!err.empty());
  // A 16-bit PNG cut off after its header: refused, no crash, a message (and a null error pointer is
  // allowed).
  TempFile whole("whole16.png"), cut("cut16.png");
  CHECK(encode_helpers::writePng16(whole.path, widePicture(16, 16), &err));
  {
    std::ifstream in(whole.path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() > 80);
    std::ofstream out(cut.path, std::ios::binary);
    out.write(bytes.data(), 70);
  }
  CHECK(image_source::depthOf(cut.path) == image_source::Depth::Wide); // the header still says 16-bit
  err.clear();
  CHECK(!image_source::openUpright(cut.path, Orient{}, true, &err));
  CHECK(!err.empty());
  CHECK(!image_source::openUpright(cut.path, Orient{}, true, nullptr));
}

static std::vector<char> slurp(const std::string &path)
{
  std::ifstream in(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static void spit(const std::string &path, const std::vector<char> &bytes, size_t n)
{
  std::ofstream out(path, std::ios::binary);
  out.write(bytes.data(), static_cast<std::streamsize>(n));
}

TEST_CASE("image_source: truncatedPngIsRefusedOnEveryPlatform", "[image_source]")
{
  // WIC (Windows) hands back a blank picture for a PNG that was cut off; libpng refuses it. Every PNG is
  // checked for its IEND chunk first, so both platforms give the same answer.
  TempFile whole("trunc_whole.png"), cut("trunc_cut.png");
  std::string err;
  CHECK(encode_helpers::writePng16(whole.path, widePicture(16, 16), &err));
  const std::vector<char> bytes = slurp(whole.path);
  CHECK(bytes.size() > 100);
  CHECK(!image_source::pngIsTruncated(whole.path));
  CHECK(image_source::openUpright(whole.path, Orient{}, true, &err).has_value());
  // Cut at several places: right after the signature, mid-header, mid-data, inside IEND, one byte short.
  for (const size_t n : {size_t{8}, size_t{9}, size_t{33}, bytes.size() / 2, bytes.size() - 13, bytes.size() - 12, bytes.size() - 1})
  {
    spit(cut.path, bytes, n);
    CHECK(image_source::pngIsTruncated(cut.path));
    err.clear();
    CHECK(!image_source::openUpright(cut.path, Orient{}, true, &err));
    CHECK(err == image_source::kTruncatedMessage);
    int w = 0, h = 0;
    err.clear();
    CHECK((!image_source::uprightSize(cut.path, Orient{}, &w, &h, &err) && err == image_source::kTruncatedMessage));
    err.clear();
    CHECK((!image_source::decodeForThumb(cut.path, &err) && err == image_source::kTruncatedMessage));
  }
  // Bytes after IEND are tolerated: the file is whole.
  std::vector<char> plus = bytes;
  for (int i = 0; i < 100; ++i)
    plus.push_back(static_cast<char>(i));
  spit(cut.path, plus, plus.size());
  CHECK(!image_source::pngIsTruncated(cut.path));
  CHECK(image_source::openUpright(cut.path, Orient{}, true, &err).has_value());
  // Not a PNG (or unreadable or empty): not this check's business, the decoder reports it.
  const std::vector<char> text = {'h', 'e', 'l', 'l', 'o'};
  spit(cut.path, text, text.size());
  CHECK(!image_source::pngIsTruncated(cut.path));
  spit(cut.path, text, 0);
  CHECK(!image_source::pngIsTruncated(cut.path));
  CHECK(!image_source::pngIsTruncated(tmp("does_not_exist.png")));
}

TEST_CASE("image_source: uprightSize", "[image_source]")
{
  TempFile f("size.png");
  std::string err;
  CHECK(encode_helpers::writePng8(f.path, narrowPicture(7, 5), &err));
  int w = 0, h = 0;
  CHECK((image_source::uprightSize(f.path, Orient{}, &w, &h, &err) && w == 7 && h == 5));
  CHECK((image_source::uprightSize(f.path, Orient{1, false}, &w, &h, &err) && w == 5 && h == 7));
  CHECK((image_source::uprightSize(f.path, Orient{3, true}, &w, &h, &err) && w == 5 && h == 7));
  CHECK((image_source::uprightSize(f.path, Orient{2, false}, &w, &h, &err) && w == 7 && h == 5)); // a half turn keeps the sides
  err.clear();
  CHECK(!image_source::uprightSize(tmp("does_not_exist.png"), Orient{}, &w, &h, &err));
  CHECK(!err.empty());
}

TEST_CASE("image_source: decodeForThumb", "[image_source]")
{
  TempFile w("thumb16.png");
  std::string err;
  CHECK(encode_helpers::writePng16(w.path, widePicture(7, 5), &err));
  // A wide file's thumbnail source is its plain 8-bit decode, not turned.
  const auto t = image_source::decodeForThumb(w.path, &err);
  CHECK((t && t->width == 7 && t->height == 5));
  CHECK(!image_source::decodeForThumb(tmp("does_not_exist.png"), &err));
}

TEST_CASE("image_source: damagedPngIsRefusedButItsSizeIsStillReadFromTheHeader", "[image_source]")
{
  // A flipped byte inside the pixel data: the chunk CRC no longer matches. Opening and thumbnailing refuse
  // it (liteui checks every chunk CRC on both platforms); uprightSize reads only the header and still gives
  // the size, which is what the sidecar sync needs.
  TempFile whole("crc_whole.png"), bad("crc_bad.png");
  std::string err;
  CHECK(encode_helpers::writePng16(whole.path, widePicture(16, 16), &err));
  std::vector<char> bytes = slurp(whole.path);
  bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 0x5A);
  spit(bad.path, bytes, bytes.size());
  CHECK(!image_source::pngIsTruncated(bad.path));
  err.clear();
  CHECK(!image_source::openUpright(bad.path, Orient{}, true, &err));
  CHECK(!err.empty());
  CHECK(err != image_source::kTruncatedMessage);
  err.clear();
  CHECK(!image_source::decodeForThumb(bad.path, &err));
  CHECK(!err.empty());
  int w = 0, h = 0;
  CHECK((image_source::uprightSize(bad.path, Orient{}, &w, &h, &err) && w == 16 && h == 16));
}

TEST_CASE("image_source: thumbOfAWideFileIsTheNarrowedWideRead", "[image_source]")
{
  // The filmstrip and the editor show the same 8-bit picture for a 16-bit file: the nearest byte.
  TempFile f("thumb_eq.png");
  std::string err;
  CHECK(encode_helpers::writePng16(f.path, widePicture(23, 11), &err));
  const auto t = image_source::decodeForThumb(f.path, &err);
  const auto o = image_source::openUpright(f.path, Orient{}, true, &err);
  CHECK((t && o && o->image));
  if (t && o && o->image)
    CHECK((t->width == o->image->width && t->height == o->image->height && t->pixels == o->image->pixels));
}

TEST_CASE("image_source: theFormatComesFromTheContentNotTheName", "[image_source]")
{
  // A PNG called .jpg (and the other way round) is read as what it is.
  TempFile asJpg("really_png.jpg"), asPng("really_jpeg.png");
  std::string err;
  CHECK(encode_helpers::writePng8(asJpg.path, narrowPicture(9, 4), &err));
  std::vector<uint8_t> jpeg;
  CHECK(encode_helpers::jpegBytes(narrowPicture(9, 4), 90, &jpeg, &err));
  {
    std::ofstream f(asPng.path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }
  const auto a = image_source::openUpright(asJpg.path, Orient{}, true, &err);
  const auto b = image_source::openUpright(asPng.path, Orient{}, true, &err);
  CHECK((a && a->image && a->image->width == 9 && a->image->height == 4));
  CHECK((b && b->image && b->image->width == 9 && b->image->height == 4 && !b->wide));
  CHECK(image_source::depthOf(asJpg.path) == image_source::Depth::Narrow);
}

TEST_CASE("image_source: uprightSizeOfAJpegIsItsHeader", "[image_source]")
{
  TempFile f("size.jpg");
  std::string err;
  std::vector<uint8_t> jpeg;
  CHECK(encode_helpers::jpegBytes(narrowPicture(13, 6), 90, &jpeg, &err));
  {
    std::ofstream o(f.path, std::ios::binary);
    o.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
  }
  int w = 0, h = 0;
  CHECK((image_source::uprightSize(f.path, Orient{}, &w, &h, &err) && w == 13 && h == 6));
  CHECK((image_source::uprightSize(f.path, Orient{1, false}, &w, &h, &err) && w == 6 && h == 13));
  // Same answer as an actual open.
  const auto o = image_source::openUpright(f.path, Orient{1, false}, true, &err);
  CHECK((o && o->image && o->image->width == 6 && o->image->height == 13));
}

TEST_CASE("image_source: aPngCutInsideItsHeaderIsTruncatedToo", "[image_source]")
{
  // Shorter than the IHDR (8..28 bytes): liteui's probe cannot read it, but it is a cut-off PNG, not a
  // damaged one, and says so in Literoom's words. A file with garbage after the signature is NOT truncated.
  TempFile whole("hdrcut_whole.png"), cut("hdrcut.png");
  std::string err;
  CHECK(encode_helpers::writePng8(whole.path, narrowPicture(5, 5), &err));
  const std::vector<char> bytes = slurp(whole.path);
  for (const size_t n : {size_t{8}, size_t{9}, size_t{20}, size_t{28}})
  {
    spit(cut.path, bytes, n);
    CHECK(image_source::pngIsTruncated(cut.path));
  }
  std::vector<char> junk = bytes;
  for (size_t i = 8; i < 40; ++i)
    junk[i] = static_cast<char>(0x11 * (i % 7)); // no IHDR where it belongs
  spit(cut.path, junk, junk.size());
  CHECK(!image_source::pngIsTruncated(cut.path));
  err.clear();
  CHECK((!image_source::openUpright(cut.path, Orient{}, true, &err) && !err.empty() && err != image_source::kTruncatedMessage));
}
} // namespace image_source_cases
