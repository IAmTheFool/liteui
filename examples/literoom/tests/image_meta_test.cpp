// image_meta_test.cpp - exifOrientationOf and embedSourceMetadata against
// synthetic JPEG files (hand-built EXIF / ICC segments, no encoder, no liteui)
// written to a throw-away folder under the system temp directory.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "image_meta.hpp"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

// ------------------------------------------------------------ test files
struct TempDir
{
  fs::path path;
  explicit TempDir(const char *tag)
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           (std::string("literoom_im_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string file(const std::string &name) const { return u8ToString(path / fsPath(name)); }
};

static void writeFile(const std::string &path, const Bytes &b)
{
  std::ofstream f(fsPath(path), std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

// ------------------------------------------------------- JPEG / EXIF builders
static void put16(Bytes &b, unsigned v, bool le)
{
  const uint8_t lo = static_cast<uint8_t>(v), hi = static_cast<uint8_t>(v >> 8);
  if (le)
    b.insert(b.end(), {lo, hi});
  else
    b.insert(b.end(), {hi, lo});
}

static void put32(Bytes &b, uint32_t v, bool le)
{
  if (le)
  {
    put16(b, v & 0xFFFF, true);
    put16(b, v >> 16, true);
  }
  else
  {
    put16(b, v >> 16, false);
    put16(b, v & 0xFFFF, false);
  }
}

// One 12-byte IFD entry; a SHORT keeps its value in the first two bytes of the
// 4-byte value field, anything else uses all four.
static void putEntry(Bytes &b, uint16_t tag, uint16_t type, uint32_t count, uint32_t value, bool le)
{
  put16(b, tag, le);
  put16(b, type, le);
  put32(b, count, le);
  if (type == 3)
  {
    put16(b, value, le);
    put16(b, 0, le);
  }
  else
    put32(b, value, le);
}

constexpr uint8_t kThumbByte = 0xAB;

struct ExifSpec
{
  bool le = true;
  unsigned orientation = 1; // the value stored in the Orientation tag
  uint16_t orientType = 3;  // 3 = SHORT (the only valid type)
  bool thumbnail = true;    // IFD1 + 16 bytes of kThumbByte
  uint32_t width = 4000, height = 3000;
};

// TIFF block: IFD0 {ImageWidth, ImageLength, Orientation, ExifIFD}, the Exif
// sub-IFD {PixelXDimension, PixelYDimension}, and optionally IFD1 + thumbnail.
static Bytes makeTiff(const ExifSpec &s)
{
  const bool le = s.le;
  Bytes t;
  t.push_back(le ? 'I' : 'M');
  t.push_back(le ? 'I' : 'M');
  put16(t, 42, le);
  put32(t, 8, le);
  const uint32_t ifd0 = 8;
  const uint32_t sub = ifd0 + 2 + 4 * 12 + 4;
  const uint32_t ifd1 = sub + 2 + 2 * 12 + 4;
  const uint32_t thumb = ifd1 + 2 + 2 * 12 + 4;
  put16(t, 4, le);
  putEntry(t, 0x0100, 4, 1, s.width, le);
  putEntry(t, 0x0101, 4, 1, s.height, le);
  putEntry(t, 0x0112, s.orientType, 1, s.orientation, le);
  putEntry(t, 0x8769, 4, 1, sub, le);
  put32(t, s.thumbnail ? ifd1 : 0, le);
  put16(t, 2, le);
  putEntry(t, 0xA002, 4, 1, s.width, le);
  putEntry(t, 0xA003, 4, 1, s.height, le);
  put32(t, 0, le);
  if (s.thumbnail)
  {
    put16(t, 2, le);
    putEntry(t, 0x0201, 4, 1, thumb, le);
    putEntry(t, 0x0202, 4, 1, 16, le);
    put32(t, 0, le);
    t.insert(t.end(), 16, kThumbByte);
  }
  return t;
}

static Bytes segment(uint8_t marker, const Bytes &payload)
{
  Bytes s{0xFF, marker};
  const size_t len = payload.size() + 2;
  s.push_back(static_cast<uint8_t>(len >> 8));
  s.push_back(static_cast<uint8_t>(len));
  s.insert(s.end(), payload.begin(), payload.end());
  return s;
}

static Bytes exifSegment(const ExifSpec &s)
{
  Bytes p{'E', 'x', 'i', 'f', 0, 0};
  const Bytes tiff = makeTiff(s);
  p.insert(p.end(), tiff.begin(), tiff.end());
  return segment(0xE1, p);
}

static Bytes iccSegment(int seq, int count, size_t dataLen, uint8_t fill)
{
  Bytes p{'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0,
          static_cast<uint8_t>(seq), static_cast<uint8_t>(count)};
  p.insert(p.end(), dataLen, fill);
  return segment(0xE2, p);
}

static Bytes jfifSegment()
{
  return segment(0xE0, Bytes{'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
}

static Bytes comSegment(size_t payload) { return segment(0xFE, Bytes(payload, 'c')); }

// SOI + segments + EOI.
static Bytes jpegOf(const std::vector<Bytes> &segs)
{
  Bytes j{0xFF, 0xD8};
  for (const Bytes &s : segs)
    j.insert(j.end(), s.begin(), s.end());
  j.push_back(0xFF);
  j.push_back(0xD9);
  return j;
}

// A JPEG carrying only an EXIF block with this orientation.
static Bytes exifJpeg(unsigned orientation, bool le = true)
{
  ExifSpec s;
  s.orientation = orientation;
  s.le = le;
  return jpegOf({exifSegment(s)});
}

// What a fresh encode looks like to embedSourceMetadata: SOI, JFIF, a table, EOI.
static Bytes encodedJpeg() { return jpegOf({jfifSegment(), segment(0xDB, Bytes(10, 0x11))}); }

// ------------------------------------------------ reading results back
static Bytes tiffOf(const Bytes &jpeg)
{
  size_t i = 2;
  while (i + 4 <= jpeg.size() && jpeg[i] == 0xFF)
  {
    const uint8_t m = jpeg[i + 1];
    if (m == 0xD9 || m == 0xDA)
      break;
    const size_t len = (static_cast<size_t>(jpeg[i + 2]) << 8) | jpeg[i + 3];
    if (m == 0xE1 && len > 8 && std::memcmp(&jpeg[i + 4], "Exif\0\0", 6) == 0)
      return Bytes(jpeg.begin() + static_cast<std::ptrdiff_t>(i + 10),
                   jpeg.begin() + static_cast<std::ptrdiff_t>(i + 2 + len));
    i += 2 + len;
  }
  return {};
}

// A LONG tag of IFD0 (or of the Exif sub-IFD) in the EXIF block of `jpeg`.
static bool readLong(const Bytes &jpeg, bool inExifIfd, uint16_t tag, uint32_t *v)
{
  Bytes tiff = tiffOf(jpeg);
  exif::detail::Tiff t{tiff};
  uint32_t ifd = 0;
  uint64_t e = 0;
  if (!exif::detail::tiffHeader(t, &ifd))
    return false;
  if (inExifIfd && !(exif::detail::findEntry(t, ifd, 0x8769, &e) && t.rd32(e + 8, &ifd)))
    return false;
  return exif::detail::findEntry(t, ifd, tag, &e) && t.rd32(e + 8, v);
}

static bool readOrientationTag(const Bytes &jpeg, uint16_t *v)
{
  Bytes tiff = tiffOf(jpeg);
  exif::detail::Tiff t{tiff};
  uint32_t ifd0 = 0;
  uint64_t e = 0;
  return exif::detail::tiffHeader(t, &ifd0) && exif::detail::findEntry(t, ifd0, 0x0112, &e) &&
         t.rd16(e + 8, v);
}

static bool isOrient(const Orient &o, int rot, bool flip) { return o.rot == rot && o.flip == flip; }

// ------------------------------------------------------------- orientation
static void testOrientationTable()
{
  TempDir d("orient");
  struct Row
  {
    unsigned tag;
    int rot;
    bool flip;
  };
  // EXIF value -> the transform that makes the picture upright (mirror first,
  // then clockwise quarter turns).
  const Row rows[] = {{1, 0, false}, {2, 0, true}, {3, 2, false}, {4, 2, true},
                      {5, 3, true},  {6, 1, false}, {7, 1, true},  {8, 3, false}};
  for (bool le : {true, false})
    for (const Row &r : rows)
    {
      const std::string p = d.file("o.jpg");
      writeFile(p, exifJpeg(r.tag, le));
      CHECK(isOrient(exifOrientationOf(p), r.rot, r.flip));
    }
}

static void testOrientationFileNames()
{
  TempDir d("names");
  const Bytes six = exifJpeg(6);
  for (const char *name : {"a.jpg", "a.JPG", "a.jpeg", "a.JpEg"})
  {
    writeFile(d.file(name), six);
    CHECK(isOrient(exifOrientationOf(d.file(name)), 1, false));
  }
  // Same bytes, not a JPEG name: never looked at.
  for (const char *name : {"a.png", "a.tif", "a"})
  {
    writeFile(d.file(name), six);
    CHECK(exifOrientationOf(d.file(name)).identity());
  }
  // A UTF-8 file name works (fsPath).
  const std::string uni = "\xC3\xA9t\xC3\xA9.jpg";
  writeFile(d.file(uni), six);
  CHECK(isOrient(exifOrientationOf(d.file(uni)), 1, false));
}

static void testOrientationIdentityCases()
{
  TempDir d("ident");
  const std::string p = d.file("x.jpg");

  CHECK(exifOrientationOf(d.file("missing.jpg")).identity());

  writeFile(p, {});
  CHECK(exifOrientationOf(p).identity());

  const std::string text = "this is not a jpeg at all";
  writeFile(p, Bytes(text.begin(), text.end()));
  CHECK(exifOrientationOf(p).identity());

  writeFile(p, jpegOf({jfifSegment()})); // a JPEG with no EXIF
  CHECK(exifOrientationOf(p).identity());

  for (unsigned bad : {0u, 9u, 255u}) // values outside 1..8
  {
    writeFile(p, exifJpeg(bad));
    CHECK(exifOrientationOf(p).identity());
  }

  ExifSpec wrongType; // Orientation stored as LONG instead of SHORT: ignored
  wrongType.orientation = 6;
  wrongType.orientType = 4;
  writeFile(p, jpegOf({exifSegment(wrongType)}));
  CHECK(exifOrientationOf(p).identity());

  Bytes cut = exifJpeg(6); // cut off in the middle of the EXIF segment
  cut.resize(cut.size() - 30);
  writeFile(p, cut);
  CHECK(exifOrientationOf(p).identity());
}

// Only the first 256 KiB of the file is searched.
static void testOrientationWindow()
{
  TempDir d("window");
  const std::string p = d.file("x.jpg");
  ExifSpec s;
  s.orientation = 6;

  // COM segments of 65535 bytes each (the maximum) push the EXIF block back.
  writeFile(p, jpegOf({comSegment(65533), comSegment(65533), comSegment(65533), exifSegment(s)}));
  CHECK(isOrient(exifOrientationOf(p), 1, false)); // ends near 197 KB: found

  writeFile(p, jpegOf({comSegment(65533), comSegment(65533), comSegment(65533), comSegment(65533),
                       comSegment(65533), exifSegment(s)}));
  CHECK(exifOrientationOf(p).identity()); // starts past 327 KB: not looked for
}

// -------------------------------------------------------- embed: happy path
static void testEmbedCarriesMetadata()
{
  TempDir d("embed");
  ExifSpec s;
  s.orientation = 6;
  s.width = 4000;
  s.height = 3000;
  const Bytes icc1 = iccSegment(1, 2, 20, 0x21), icc2 = iccSegment(2, 2, 10, 0x22);
  const Bytes srcBytes = jpegOf({jfifSegment(), exifSegment(s), icc1, icc2});
  const std::string src = d.file("src.jpg");
  writeFile(src, srcBytes);

  exif::JpegMeta srcMeta;
  CHECK(exif::readJpegMeta(srcBytes.data(), srcBytes.size(), &srcMeta));
  CHECK(srcMeta.orientation == 6 && srcMeta.icc.size() == 2 && !srcMeta.exif.empty());

  const Bytes target = encodedJpeg();
  Bytes jpeg = target;
  CHECK(embedSourceMetadata(&jpeg, src, 1200, 800));
  CHECK(jpeg.size() > target.size());

  // Layout: SOI, the encoder's JFIF stays first, then EXIF, then ICC, then the rest.
  CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8);
  CHECK(std::equal(target.begin(), target.begin() + 20, jpeg.begin())); // SOI + JFIF (18 bytes)
  CHECK(jpeg[20] == 0xFF && jpeg[21] == 0xE1);
  const Bytes tail(target.begin() + 20, target.end());
  CHECK(Bytes(jpeg.end() - static_cast<std::ptrdiff_t>(tail.size()), jpeg.end()) == tail);

  exif::JpegMeta out;
  CHECK(exif::readJpegMeta(jpeg.data(), jpeg.size(), &out));
  CHECK(out.orientation == 1);   // reset: the pixels are already upright
  CHECK(!out.exif.empty());
  CHECK(out.icc == srcMeta.icc); // verbatim, in order

  uint16_t o = 0;
  CHECK(readOrientationTag(jpeg, &o) && o == 1);
  uint32_t v = 0;
  CHECK(readLong(jpeg, false, 0x0100, &v) && v == 1200); // ImageWidth
  CHECK(readLong(jpeg, false, 0x0101, &v) && v == 800);  // ImageLength
  CHECK(readLong(jpeg, true, 0xA002, &v) && v == 1200);  // PixelXDimension
  CHECK(readLong(jpeg, true, 0xA003, &v) && v == 800);   // PixelYDimension

  // The old (uncropped) thumbnail is erased, not just unlinked.
  const Bytes tiff = tiffOf(jpeg);
  CHECK(!tiff.empty() && std::find(tiff.begin(), tiff.end(), kThumbByte) == tiff.end());
  CHECK(std::find(srcMeta.exif.begin(), srcMeta.exif.end(), kThumbByte) == srcMeta.exif.end());

  // End to end: the exported file reads as upright, the source still as rotated.
  const std::string outPath = d.file("out.jpg");
  writeFile(outPath, jpeg);
  CHECK(exifOrientationOf(outPath).identity());
  CHECK(isOrient(exifOrientationOf(src), 1, false));
}

static void testEmbedBigEndianNoThumbnail()
{
  TempDir d("embedbe");
  ExifSpec s;
  s.le = false;
  s.orientation = 8;
  s.thumbnail = false;
  const std::string src = d.file("src.jpeg");
  writeFile(src, jpegOf({exifSegment(s)}));

  Bytes jpeg = encodedJpeg();
  CHECK(embedSourceMetadata(&jpeg, src, 640, 480));
  uint16_t o = 0;
  uint32_t v = 0;
  CHECK(readOrientationTag(jpeg, &o) && o == 1);
  CHECK(readLong(jpeg, false, 0x0100, &v) && v == 640);
  CHECK(readLong(jpeg, true, 0xA003, &v) && v == 480);
}

// ------------------------------------------------- embed: nothing to carry
static void testEmbedLeavesJpegAlone()
{
  TempDir d("embednone");
  const Bytes target = encodedJpeg();
  const Bytes six = exifJpeg(6);

  // A JPEG source without any EXIF / ICC: embed ran, nothing changed.
  writeFile(d.file("plain.jpg"), jpegOf({jfifSegment()}));
  Bytes a = target;
  CHECK(embedSourceMetadata(&a, d.file("plain.jpg"), 100, 100));
  CHECK(a == target);

  // Everything below is refused, and refused without touching the bytes.
  writeFile(d.file("src.png"), six); // not a JPEG name
  const std::string text = "not a jpeg";
  writeFile(d.file("junk.jpg"), Bytes(text.begin(), text.end()));
  writeFile(d.file("empty.jpg"), {});
  for (const char *name : {"src.png", "junk.jpg", "empty.jpg", "missing.jpg", ""})
  {
    Bytes b = target;
    CHECK(!embedSourceMetadata(&b, name[0] ? d.file(name) : std::string(), 100, 100));
    CHECK(b == target);
  }

  // A target that is not a JPEG comes back as it was (exif::embed's rule).
  writeFile(d.file("src.jpg"), six);
  Bytes notJpeg{'h', 'e', 'l', 'l', 'o'};
  const Bytes before = notJpeg;
  CHECK(embedSourceMetadata(&notJpeg, d.file("src.jpg"), 100, 100));
  CHECK(notJpeg == before);

  CHECK(kMetaPrefixBytes == (8u << 20));
}

int main()
{
  testOrientationTable();
  testOrientationFileNames();
  testOrientationIdentityCases();
  testOrientationWindow();
  testEmbedCarriesMetadata();
  testEmbedBigEndianNoThumbnail();
  testEmbedLeavesJpegAlone();
  return finish();
}
