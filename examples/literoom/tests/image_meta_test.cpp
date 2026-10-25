// image_meta_test.cpp - exifOrientationOf, embedSourceMetadata and photoInfoOf against
// synthetic JPEG files (hand-built EXIF / ICC segments, no encoder, no liteui)
// written to a throw-away folder under the system temp directory.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "check.hpp"
#include "io/image_meta.hpp"

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
constexpr uint8_t kGpsByte = 0xCD; // fills the GPS latitude data, so any leftover shows

struct ExifSpec
{
  bool le = true;
  unsigned orientation = 1; // the value stored in the Orientation tag
  uint16_t orientType = 3;  // 3 = SHORT (the only valid type)
  bool thumbnail = true;    // IFD1 + 16 bytes of kThumbByte
  bool gps = false;         // a GPSInfo tag in IFD0 and a GPS IFD with 24 bytes of kGpsByte
  uint32_t width = 4000, height = 3000;
};

// TIFF block: IFD0 {ImageWidth, ImageLength, Orientation, ExifIFD}, the Exif
// sub-IFD {PixelXDimension, PixelYDimension}, optionally IFD1 + thumbnail, and
// optionally (IFD0 then also holds GPSInfo) a GPS IFD at the very end.
static Bytes makeTiff(const ExifSpec &s)
{
  const bool le = s.le;
  Bytes t;
  t.push_back(le ? 'I' : 'M');
  t.push_back(le ? 'I' : 'M');
  put16(t, 42, le);
  put32(t, 8, le);
  const uint32_t ifd0 = 8;
  const unsigned n0 = s.gps ? 5 : 4; // entries in IFD0
  const uint32_t sub = ifd0 + 2 + n0 * 12 + 4;
  const uint32_t ifd1 = sub + 2 + 2 * 12 + 4;
  const uint32_t thumb = ifd1 + 2 + 2 * 12 + 4;
  const uint32_t gpsOff = s.thumbnail ? thumb + 16 : ifd1; // where the GPS IFD goes
  put16(t, n0, le);
  putEntry(t, 0x0100, 4, 1, s.width, le);
  putEntry(t, 0x0101, 4, 1, s.height, le);
  putEntry(t, 0x0112, s.orientType, 1, s.orientation, le);
  putEntry(t, 0x8769, 4, 1, sub, le);
  if (s.gps)
    putEntry(t, 0x8825, 4, 1, gpsOff, le);
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
  if (s.gps)
  {
    put16(t, 2, le); // GPS IFD: LatitudeRef (inline) and Latitude (24 bytes after the IFD)
    putEntry(t, 0x0001, 2, 2, 0x4E000000, le);
    putEntry(t, 0x0002, 5, 3, gpsOff + 2 + 2 * 12 + 4, le);
    put32(t, 0, le);
    t.insert(t.end(), 24, kGpsByte);
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

// ----------------------------------------------- embed: removing the location
static bool hasByte(const Bytes &b, uint8_t v) { return std::find(b.begin(), b.end(), v) != b.end(); }

static bool hasGpsTag(const Bytes &jpeg)
{
  Bytes tiff = tiffOf(jpeg);
  exif::detail::Tiff t{tiff};
  uint32_t ifd0 = 0;
  uint64_t e = 0;
  return exif::detail::tiffHeader(t, &ifd0) && exif::detail::findEntry(t, ifd0, 0x8825, &e);
}

static void testEmbedRemovesGps()
{
  TempDir d("gps");
  for (bool le : {true, false})
    for (bool thumb : {true, false})
    {
      ExifSpec s;
      s.le = le;
      s.thumbnail = thumb;
      s.orientation = 6;
      s.gps = true;
      const std::string src = d.file("gps.jpg");
      writeFile(src, jpegOf({exifSegment(s)}));

      // Default: the location is carried over, as before.
      Bytes kept = encodedJpeg();
      CHECK(embedSourceMetadata(&kept, src, 100, 80));
      CHECK(hasByte(tiffOf(kept), kGpsByte) && hasGpsTag(kept));

      // Asked to remove it: the tag and every coordinate byte are gone...
      Bytes clean = encodedJpeg();
      CHECK(embedSourceMetadata(&clean, src, 100, 80, true));
      const Bytes tiff = tiffOf(clean);
      CHECK(!tiff.empty() && !hasByte(tiff, kGpsByte) && !hasGpsTag(clean));
      CHECK(clean.size() == kept.size()); // zeroed and shifted, not shortened

      // ...and everything else is as with the default: upright, new size, thumbnail erased.
      exif::JpegMeta m;
      CHECK(exif::readJpegMeta(clean.data(), clean.size(), &m) && m.orientation == 1);
      uint32_t v = 0;
      CHECK(readLong(clean, false, 0x0100, &v) && v == 100);
      CHECK(readLong(clean, false, 0x0101, &v) && v == 80);
      CHECK(readLong(clean, true, 0xA002, &v) && v == 100);
      CHECK(readLong(clean, true, 0xA003, &v) && v == 80);
      CHECK(!hasByte(tiff, kThumbByte));
      const std::string outPath = d.file("clean.jpg");
      writeFile(outPath, clean);
      CHECK(exifOrientationOf(outPath).identity());

      // The source file is only read: asking again for the location still finds it.
      Bytes again = encodedJpeg();
      CHECK(embedSourceMetadata(&again, src, 100, 80));
      CHECK(again == kept);

      // A source with no GPS block: the option changes nothing.
      ExifSpec p = s;
      p.gps = false;
      writeFile(d.file("nogps.jpg"), jpegOf({exifSegment(p)}));
      Bytes a = encodedJpeg(), b = encodedJpeg();
      CHECK(embedSourceMetadata(&a, d.file("nogps.jpg"), 100, 80));
      CHECK(embedSourceMetadata(&b, d.file("nogps.jpg"), 100, 80, true));
      CHECK(a == b && !hasGpsTag(a));
    }

  // The option is ignored where there is nothing to embed.
  writeFile(d.file("plain.jpg"), jpegOf({jfifSegment()}));
  Bytes plain = encodedJpeg();
  const Bytes target = plain;
  CHECK(embedSourceMetadata(&plain, d.file("plain.jpg"), 10, 10, true) && plain == target);
  Bytes refused = target;
  CHECK(!embedSourceMetadata(&refused, d.file("missing.jpg"), 10, 10, true) && refused == target);
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

// ------------------------------------------------------------ photo info
// A TIFF block with camera data: IFD0 {Make, Model, ExifIFD} and the Exif
// sub-IFD {ExposureTime, FNumber, ISO, DateTimeOriginal, FocalLength, LensModel}.
// Layout: IFD0 at 8 (3 entries) -> sub-IFD at 50 (6 entries) -> data from 128.
static Bytes makeInfoTiff(bool le)
{
  const std::string make = "Canon", model = "Canon EOS R5", lens = "RF85mm F1.2 L USM";
  const std::string date = "2024:06:15 14:30:05";
  uint32_t next = 128;
  auto take = [&next](size_t n)
  {
    const uint32_t o = next;
    next += static_cast<uint32_t>(n);
    return o;
  };
  const uint32_t oMake = take(make.size() + 1), oModel = take(model.size() + 1), oExp = take(8),
                 oF = take(8), oDate = take(date.size() + 1), oFocal = take(8),
                 oLens = take(lens.size() + 1);
  Bytes t;
  t.push_back(le ? 'I' : 'M');
  t.push_back(le ? 'I' : 'M');
  put16(t, 42, le);
  put32(t, 8, le);
  put16(t, 3, le);
  putEntry(t, 0x010F, 2, static_cast<uint32_t>(make.size() + 1), oMake, le);
  putEntry(t, 0x0110, 2, static_cast<uint32_t>(model.size() + 1), oModel, le);
  putEntry(t, 0x8769, 4, 1, 50, le);
  put32(t, 0, le);
  put16(t, 6, le); // at 50
  putEntry(t, 0x829A, 5, 1, oExp, le);
  putEntry(t, 0x829D, 5, 1, oF, le);
  putEntry(t, 0x8827, 3, 1, 400, le);
  putEntry(t, 0x9003, 2, static_cast<uint32_t>(date.size() + 1), oDate, le);
  putEntry(t, 0x920A, 5, 1, oFocal, le);
  putEntry(t, 0xA434, 2, static_cast<uint32_t>(lens.size() + 1), oLens, le);
  put32(t, 0, le);
  auto text = [&t](const std::string &s)
  {
    t.insert(t.end(), s.begin(), s.end());
    t.push_back(0);
  };
  auto rat = [&t, le](uint32_t n, uint32_t d)
  {
    put32(t, n, le);
    put32(t, d, le);
  };
  text(make);
  text(model);
  rat(1, 250);
  rat(28, 10);
  text(date);
  rat(85, 1);
  text(lens);
  return t;
}

static Bytes infoSegment(bool le = true)
{
  Bytes p{'E', 'x', 'i', 'f', 0, 0};
  const Bytes tiff = makeInfoTiff(le);
  p.insert(p.end(), tiff.begin(), tiff.end());
  return segment(0xE1, p);
}

static bool isCanonInfo(const exif::PhotoInfo &p)
{
  return p.make == "Canon" && p.model == "Canon EOS R5" && p.lens == "RF85mm F1.2 L USM" && p.iso == 400 &&
         p.dateTaken == "2024-06-15 14:30:05" && std::fabs(p.exposureTime - 0.004) < 1e-12 &&
         std::fabs(p.fNumber - 2.8) < 1e-12 && p.focalLength == 85.0;
}

static void testPhotoInfoOf()
{
  TempDir d("info");
  const Bytes full = jpegOf({jfifSegment(), infoSegment(true)});
  const Bytes fullBe = jpegOf({jfifSegment(), infoSegment(false)});

  // Read from disk, in both byte orders and any case of the extension.
  exif::PhotoInfo p;
  writeFile(d.file("a.jpg"), full);
  CHECK(photoInfoOf(d.file("a.jpg"), &p) && isCanonInfo(p));
  writeFile(d.file("b.JPEG"), fullBe);
  CHECK(photoInfoOf(d.file("b.JPEG"), &p) && isCanonInfo(p));
  writeFile(d.file("c.Jpg"), full);
  CHECK(photoInfoOf(d.file("c.Jpg"), &p) && isCanonInfo(p));

  // A virtual copy has no file of its own: it reads the picture behind it.
  CHECK(photoInfoOf(vcPath(d.file("a.jpg"), 2), &p) && isCanonInfo(p));
  CHECK(photoInfoOf(vcPath(d.file("b.JPEG"), 9), &p) && isCanonInfo(p));
  CHECK(!photoInfoOf(vcPath(d.file("missing.jpg"), 1), &p) && p.empty());

  // The camera data does not disturb the orientation reader, and vice versa.
  ExifSpec s;
  s.orientation = 6;
  writeFile(d.file("rot.jpg"), jpegOf({exifSegment(s)}));
  CHECK(photoInfoOf(d.file("rot.jpg"), &p) && p.empty()); // an EXIF block with no camera fields
  CHECK(isOrient(exifOrientationOf(d.file("rot.jpg")), 1, false));
  CHECK(exifOrientationOf(d.file("a.jpg")).identity());

  // A JPEG with no EXIF at all: readable, nothing in it.
  writeFile(d.file("plain.jpg"), encodedJpeg());
  p.make = "stale";
  CHECK(photoInfoOf(d.file("plain.jpg"), &p) && p.empty());

  // Refused, and `out` comes back empty even if it held something.
  writeFile(d.file("pic.png"), full); // right bytes, wrong name
  const std::string text = "not a jpeg";
  writeFile(d.file("junk.jpg"), Bytes(text.begin(), text.end()));
  writeFile(d.file("empty.jpg"), {});
  for (const char *name : {"pic.png", "junk.jpg", "empty.jpg", "missing.jpg", ""})
  {
    p.make = "stale";
    p.iso = 123;
    CHECK(!photoInfoOf(name[0] ? d.file(name) : std::string(), &p));
    CHECK(p.empty());
  }

  // Only the first 256 KiB are searched (the same window as the orientation).
  CHECK(kInfoPrefixBytes == (256u << 10));
  writeFile(d.file("near.jpg"), jpegOf({comSegment(65533), comSegment(65533), comSegment(65533), infoSegment()}));
  CHECK(photoInfoOf(d.file("near.jpg"), &p) && isCanonInfo(p)); // ends near 197 KB
  writeFile(d.file("late.jpg"), jpegOf({comSegment(65533), comSegment(65533), comSegment(65533), comSegment(65533),
                                        comSegment(65533), infoSegment()}));
  CHECK(photoInfoOf(d.file("late.jpg"), &p) && p.empty()); // starts past 327 KB: not found, still a JPEG

  // A damaged file gives a partial answer, never a crash or a wrong one.
  Bytes cut = full;
  for (size_t keep = 0; keep < cut.size(); keep += 7)
  {
    writeFile(d.file("cut.jpg"), Bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(keep)));
    p = exif::PhotoInfo{};
    (void)photoInfoOf(d.file("cut.jpg"), &p);
    CHECK(p.make.empty() || p.make == "Canon");
    CHECK(p.iso == 0 || p.iso == 400);
  }
}

int main()
{
  testOrientationTable();
  testOrientationFileNames();
  testOrientationIdentityCases();
  testOrientationWindow();
  testEmbedCarriesMetadata();
  testEmbedBigEndianNoThumbnail();
  testEmbedRemovesGps();
  testEmbedLeavesJpegAlone();
  testPhotoInfoOf();
  return finish();
}