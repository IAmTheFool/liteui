// cases/image_source.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace image_source_cases
// so they cannot collide with other cases.
//
// image_source_test.cpp - how a picture file is opened (io/image_source.hpp): wide or narrow by the
// file's header, upright by the orientation it is given, the 8-bit fallback for a wide file whose
// full-precision read fails, and the errors. Real files in the temp folder; links LiteUI like
// image_codec16_test.

#include "io/image_codec.hpp"
#include "io/image_codec16.hpp"
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
  CHECK(encodePng16File(wide.path, widePicture(7, 5), &err));
  CHECK(encodePngFile(narrow.path, narrowPicture(7, 5), &err));
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
  CHECK(encodePng16File(f.path, src, &err));
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
  CHECK(encodePngFile(f.path, src, &err));
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
  CHECK(encodePng16File(w.path, widePicture(7, 5), &err));
  CHECK(encodePngFile(n.path, narrowPicture(7, 5), &err));
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
  // A 16-bit PNG cut off after its header: both readers fail, no crash, a message (and a null error
  // pointer is allowed).
  TempFile whole("whole16.png"), cut("cut16.png");
  CHECK(encodePng16File(whole.path, widePicture(16, 16), &err));
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
  CHECK(encodePng16File(whole.path, widePicture(16, 16), &err));
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
  CHECK(encodePngFile(f.path, narrowPicture(7, 5), &err));
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
  CHECK(encodePng16File(w.path, widePicture(7, 5), &err));
  // A wide file's thumbnail source is its plain 8-bit decode, not turned.
  const auto t = image_source::decodeForThumb(w.path, &err);
  CHECK((t && t->width == 7 && t->height == 5));
  CHECK(!image_source::decodeForThumb(tmp("does_not_exist.png"), &err));
}
} // namespace image_source_cases
