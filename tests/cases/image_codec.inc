// Tests for the PNG / JPEG <-> ImageBuffer functions: probeFile/probeMemory,
// decodeFileBuffer/decodeMemoryBuffer, encodePng(Memory), encodeJpeg(Memory).
// (Included from liteui_tests.cpp — not a standalone .cpp.)
//
// The PNG inputs are built in memory by a tiny writer below (stored deflate
// blocks, so no zlib is needed). That covers every colour type / bit depth /
// tRNS / interlace combination without fixture files, and the same expected
// pixels hold for the libpng (Linux) and WIC (Windows) backends. Real-world
// compressed + filtered data comes from a few tiny files made by another
// encoder (Pillow), embedded as byte arrays, and from encode -> decode round
// trips.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace codec_test {

using Bytes = std::vector<uint8_t>;
using Samples = std::vector<std::vector<uint32_t>>; // [y][x * channels + c], raw file samples

// ---- a minimal PNG writer --------------------------------------------------

inline uint32_t crc32(const uint8_t *p, size_t n) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

inline uint32_t adler32(const Bytes &d) {
  uint32_t a = 1, b = 0;
  for (uint8_t v : d) { a = (a + v) % 65521; b = (b + a) % 65521; }
  return (b << 16) | a;
}

inline void put32(Bytes &b, uint32_t v) {
  b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));  b.push_back(uint8_t(v));
}

inline void addChunk(Bytes &png, const char *type, const Bytes &data) {
  put32(png, uint32_t(data.size()));
  Bytes td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  png.insert(png.end(), td.begin(), td.end());
  put32(png, crc32(td.data(), td.size()));
}

// zlib stream made of stored (uncompressed) deflate blocks.
inline Bytes zlibStore(const Bytes &raw) {
  Bytes z{0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    const uint16_t len = uint16_t(n), nlen = uint16_t(~len);
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(len & 0xFF)); z.push_back(uint8_t(len >> 8));
    z.push_back(uint8_t(nlen & 0xFF)); z.push_back(uint8_t(nlen >> 8));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  } while (pos < raw.size());
  put32(z, adler32(raw));
  return z;
}

// One scanline (without its filter byte): `depth` bits per sample, MSB first.
inline Bytes packScanline(const std::vector<uint32_t> &s, int depth) {
  Bytes out;
  if (depth == 8) { for (uint32_t v : s) out.push_back(uint8_t(v)); return out; }
  if (depth == 16) { for (uint32_t v : s) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); } return out; }
  uint32_t acc = 0; int bits = 0;
  for (uint32_t v : s) {
    acc = (acc << depth) | v; bits += depth;
    if (bits == 8) { out.push_back(uint8_t(acc)); acc = 0; bits = 0; }
  }
  if (bits) out.push_back(uint8_t(acc << (8 - bits)));
  return out;
}

struct PngSpec {
  int colorType = 6; // 0 gray, 2 RGB, 3 palette, 4 gray+alpha, 6 RGBA
  int depth = 8;
  bool interlace = false;
  Bytes plte; // RGB triples
  Bytes trns;
};

inline int channelsOf(int colorType) {
  switch (colorType) { case 0: case 3: return 1; case 4: return 2; case 2: return 3; default: return 4; }
}

inline Bytes makePng(uint32_t w, uint32_t h, const PngSpec &s, const Samples &px) {
  const int ch = channelsOf(s.colorType);
  Bytes raw;
  auto emitRow = [&](uint32_t y, uint32_t x0, uint32_t dx) {
    std::vector<uint32_t> row;
    for (uint32_t x = x0; x < w; x += dx)
      for (int c = 0; c < ch; ++c) row.push_back(px[y][x * ch + c]);
    raw.push_back(0); // filter type None
    const Bytes packed = packScanline(row, s.depth);
    raw.insert(raw.end(), packed.begin(), packed.end());
  };
  if (!s.interlace) {
    for (uint32_t y = 0; y < h; ++y) emitRow(y, 0, 1);
  } else {
    static const uint32_t adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                         {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    for (const auto &p : adam7) {
      if (p[0] >= w || p[1] >= h) continue; // an empty pass writes nothing, not even filter bytes
      for (uint32_t y = p[1]; y < h; y += p[3]) emitRow(y, p[0], p[2]);
    }
  }
  Bytes png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes ihdr;
  put32(ihdr, w); put32(ihdr, h);
  ihdr.push_back(uint8_t(s.depth)); ihdr.push_back(uint8_t(s.colorType));
  ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(s.interlace ? 1 : 0);
  addChunk(png, "IHDR", ihdr);
  if (!s.plte.empty()) addChunk(png, "PLTE", s.plte);
  if (!s.trns.empty()) addChunk(png, "tRNS", s.trns);
  const Bytes z = zlibStore(raw);
  const size_t half = z.size() / 2; // two IDAT chunks, so chunk walking is exercised
  addChunk(png, "IDAT", Bytes(z.begin(), z.begin() + half));
  addChunk(png, "IDAT", Bytes(z.begin() + half, z.end()));
  addChunk(png, "IEND", Bytes());
  return png;
}

// What every PNG variant must decode to: straight RGBA, 8 or 16 bits per channel.
inline std::vector<uint32_t> expectedRgba(uint32_t w, uint32_t h, const PngSpec &s, const Samples &px) {
  const bool wide = s.depth == 16;
  const uint32_t full = wide ? 65535u : 255u;
  const uint32_t maxv = (1u << s.depth) - 1;
  auto gray = [&](uint32_t v) { return s.depth < 8 ? v * 255 / maxv : v; };
  auto key16 = [&](size_t i) { return uint32_t(s.trns[i * 2] << 8 | s.trns[i * 2 + 1]); };
  std::vector<uint32_t> out;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t r, g, b, a = full;
      switch (s.colorType) {
        case 0: {
          r = g = b = gray(px[y][x]);
          if (!s.trns.empty() && px[y][x] == key16(0)) a = 0;
          break;
        }
        case 2: {
          r = px[y][x * 3]; g = px[y][x * 3 + 1]; b = px[y][x * 3 + 2];
          if (!s.trns.empty() && r == key16(0) && g == key16(1) && b == key16(2)) a = 0;
          break;
        }
        case 3: {
          const uint32_t i = px[y][x];
          r = s.plte[i * 3]; g = s.plte[i * 3 + 1]; b = s.plte[i * 3 + 2];
          if (i < s.trns.size()) a = s.trns[i];
          break;
        }
        case 4: r = g = b = px[y][x * 2]; a = px[y][x * 2 + 1]; break;
        default: r = px[y][x * 4]; g = px[y][x * 4 + 1]; b = px[y][x * 4 + 2]; a = px[y][x * 4 + 3]; break;
      }
      out.push_back(r); out.push_back(g); out.push_back(b); out.push_back(a);
    }
  return out;
}

struct Case {
  const char *name;
  int colorType, depth;
  bool interlace;
  int trns; // 0 none, 1 colour key (gray / RGB), 2 per-palette-entry alpha
};

// Builds the PNG for `c` at w x h from deterministic pseudo-random samples.
struct Built { Bytes png; PngSpec spec; Samples px; };
inline Built build(const Case &c, uint32_t w, uint32_t h, uint32_t seed) {
  Built b;
  b.spec.colorType = c.colorType; b.spec.depth = c.depth; b.spec.interlace = c.interlace;
  std::mt19937 rng(seed);
  const int ch = channelsOf(c.colorType);
  const uint32_t maxv = (1u << c.depth) - 1;
  uint32_t range = maxv;
  if (c.colorType == 3) {
    const uint32_t n = std::min<uint32_t>(1u << c.depth, 16);
    for (uint32_t i = 0; i < n * 3; ++i) b.spec.plte.push_back(uint8_t(rng()));
    range = n - 1;
    if (c.trns == 2) {
      for (uint32_t i = 0; i < std::max<uint32_t>(1, n / 2); ++i) b.spec.trns.push_back(uint8_t(rng()));
      b.spec.trns[0] = 0; // make sure a fully transparent entry exists
    }
  }
  if (c.trns == 1) { // colour key: tiny sample range so the key is hit often
    range = std::min<uint32_t>(range, 2);
    if (c.colorType == 0) b.spec.trns = {0, 1};
    else b.spec.trns = {0, 1, 0, 0, 0, 2};
  }
  b.px.assign(h, std::vector<uint32_t>(size_t(w) * ch));
  for (auto &row : b.px)
    for (auto &v : row) v = rng() % (range + 1);
  b.png = makePng(w, h, b.spec, b.px);
  return b;
}

// ---- helpers on decoded buffers --------------------------------------------

inline uint32_t sampleAt(const ImageBuffer &b, uint32_t x, uint32_t y, int c) {
  if (b.format == PixelFormat::RGBA16_UNORM) {
    uint16_t v;
    std::memcpy(&v, b.row(y) + (size_t(x) * 4 + c) * 2, 2);
    return v;
  }
  return static_cast<uint8_t>(b.row(y)[size_t(x) * 4 + c]);
}

// Index of the first differing sample, or -1 when `img` equals `expect`.
// With ignoreHiddenColor, a pixel that should be fully transparent only has to
// get its alpha right: the RGB under alpha 0 is invisible, and a platform codec
// may return black there (WIC does for gray + tRNS at 8 bits or fewer).
inline long firstMismatch(const ImageBuffer &img, const std::vector<uint32_t> &expect,
                          bool ignoreHiddenColor = false) {
  for (uint32_t y = 0; y < img.height; ++y)
    for (uint32_t x = 0; x < img.width; ++x) {
      const size_t base = (size_t(y) * img.width + x) * 4;
      const bool hidden = ignoreHiddenColor && expect[base + 3] == 0;
      for (int c = hidden ? 3 : 0; c < 4; ++c)
        if (sampleAt(img, x, y, c) != expect[base + c]) return long(base + c);
    }
  return -1;
}

// "x=3 y=0 channel 0: expected 1234 got 1233" for the first differing sample (or "none").
inline std::string describeMismatch(const ImageBuffer &img, const std::vector<uint32_t> &expect, long idx) {
  if (idx < 0) return "none";
  const size_t pix = size_t(idx) / 4;
  const uint32_t x = uint32_t(pix % img.width), y = uint32_t(pix / img.width);
  const int c = int(idx % 4);
  return "x=" + std::to_string(x) + " y=" + std::to_string(y) + " channel " + std::to_string(c) +
         ": expected " + std::to_string(expect[size_t(idx)]) + " got " + std::to_string(sampleAt(img, x, y, c));
}

inline bool sameImage(const ImageBuffer &a, const ImageBuffer &b) {
  if (a.width != b.width || a.height != b.height || a.format != b.format) return false;
  const size_t rb = size_t(a.width) * bytesPerPixel(a.format);
  for (uint32_t y = 0; y < a.height; ++y)
    if (std::memcmp(a.row(y), b.row(y), rb) != 0) return false;
  return true;
}

inline ImageBuffer randomImage(uint32_t w, uint32_t h, PixelFormat f, bool opaque, uint32_t seed) {
  auto img = ImageBuffer::create(w, h, f);
  std::mt19937 rng(seed);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c) {
        if (f == PixelFormat::RGBA16_UNORM) {
          const uint16_t v = (c == 3 && opaque) ? 0xFFFF : uint16_t(rng());
          std::memcpy(img->row(y) + (size_t(x) * 4 + c) * 2, &v, 2);
        } else {
          img->row(y)[size_t(x) * 4 + c] = std::byte((c == 3 && opaque) ? 255 : uint8_t(rng()));
        }
      }
  return std::move(*img);
}

inline std::optional<ImageBuffer> convertTo(const ImageBuffer &src, PixelFormat f) {
  auto dst = ImageBuffer::create(src.width, src.height, f);
  if (!dst || !liteui_image::convertImage(src, *dst)) return std::nullopt;
  return dst;
}

// The paths liteui takes are UTF-8 strings.
inline std::string utf8(const std::filesystem::path &p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
inline std::filesystem::path fsPath(const std::string &utf8Path) {
  return std::filesystem::path(std::u8string(utf8Path.begin(), utf8Path.end()));
}
inline bool fileExists(const std::string &p) { return std::filesystem::exists(fsPath(p)); }
inline Bytes readAll(const std::string &p) {
  std::ifstream f(fsPath(p), std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Offset of the nth chunk of `type` in a PNG, or npos.
inline size_t chunkOffset(const Bytes &png, const char *type, int nth = 0) {
  size_t off = 8;
  while (off + 12 <= png.size()) {
    const uint32_t len = uint32_t(png[off]) << 24 | uint32_t(png[off + 1]) << 16 | uint32_t(png[off + 2]) << 8 | png[off + 3];
    if (std::memcmp(&png[off + 4], type, 4) == 0 && nth-- == 0) return off;
    off += 12 + len;
  }
  return std::string::npos;
}

// Sampling factors of the first (luma) component of a JPEG, read from its SOF
// segment: {2, 2} is 4:2:0, {1, 1} is 4:4:4. {0, 0} if there is no frame header.
inline std::pair<int, int> lumaSampling(const Bytes &jpg) {
  size_t pos = 2;
  while (pos + 4 <= jpg.size()) {
    if (jpg[pos] != 0xFF) return {0, 0};
    const uint8_t code = jpg[pos + 1];
    if (code == 0xFF) { ++pos; continue; }
    const size_t len = size_t(jpg[pos + 2]) << 8 | jpg[pos + 3];
    const bool sof = code >= 0xC0 && code <= 0xCF && code != 0xC4 && code != 0xC8 && code != 0xCC;
    if (sof) {
      // length(2) precision(1) height(2) width(2) components(1) then {id, h<<4|v, quant} per component
      if (pos + 2 + 8 + 2 >= jpg.size()) return {0, 0};
      const uint8_t hv = jpg[pos + 2 + 8 + 1];
      return {hv >> 4, hv & 15};
    }
    pos += 2 + len;
  }
  return {0, 0};
}

// The sizes every PNG case is run at: a size that touches all 7 Adam7 passes,
// a single pixel, and a width that is not a multiple of 8 for the packed depths.
inline const std::vector<std::pair<uint32_t, uint32_t>> &caseSizes() {
  static const std::vector<std::pair<uint32_t, uint32_t>> v = {{13, 9}, {1, 1}, {5, 3}};
  return v;
}

// ---- third-party files (Pillow), embedded -----------------------------------
// kPngRgba8:  9x7 RGBA 8-bit, pixel(x,y) = see expectedPillowRgba8
// kPngGray16: 11x6 16-bit gray, value(x,y) = (x*7919 + y*10007) & 0xFFFF
// kJpegSolid: 16x16 (200,100,50) 4:2:0;  kJpegGray: 24x10, gray = x*10
// kJpegProgressive: 16x16 (30,160,220), progressive
    static const uint8_t kPngRgba8[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x07, 0x08, 0x06, 0x00, 0x00, 0x00, 0xDA, 0x9B, 0x67,
      0x60, 0x00, 0x00, 0x00, 0x3B, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0x64, 0x60, 0x60, 0xF8,
      0x2F, 0xCB, 0xCA, 0xF0, 0x19, 0x1F, 0x66, 0x61, 0x97, 0x67, 0xF8, 0xCB, 0xCE, 0xCA, 0x8D, 0x17,
      0x23, 0x29, 0x12, 0xC3, 0x89, 0xD1, 0x14, 0x29, 0x62, 0xC5, 0x58, 0x14, 0xE9, 0xA0, 0xE1, 0x08,
      0x5C, 0x8A, 0xCC, 0xFF, 0xB2, 0xB3, 0x3A, 0x41, 0x69, 0x73, 0x5C, 0x8A, 0x9C, 0x50, 0x30, 0x00,
      0x33, 0x7D, 0x48, 0x3C, 0xD5, 0x9C, 0x82, 0xDD, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
      0xAE, 0x42, 0x60, 0x82,
    };

    static const uint8_t kPngGray16[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x0B, 0x00, 0x00, 0x00, 0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x60, 0xC9, 0xE7,
      0x67, 0x00, 0x00, 0x00, 0x3E, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x65, 0x8C, 0xB1, 0x0D, 0xC0,
      0x30, 0x0C, 0xC3, 0x12, 0x20, 0xE0, 0x0B, 0xDE, 0xAC, 0x51, 0xFD, 0xFF, 0x31, 0xBF, 0x90, 0xA9,
      0xAE, 0x81, 0x82, 0x1B, 0x05, 0x6A, 0xAF, 0x95, 0xA5, 0x1F, 0xC7, 0xA1, 0xC8, 0x12, 0x1F, 0x46,
      0x1C, 0x87, 0x98, 0x83, 0x51, 0x6B, 0x14, 0xAA, 0x2C, 0x77, 0xF1, 0x6A, 0xC4, 0x33, 0x9A, 0xD6,
      0x66, 0x9E, 0x5D, 0x72, 0xF4, 0x19, 0x00, 0x2B, 0x28, 0x97, 0xD6, 0x00, 0x00, 0x00, 0x00, 0x49,
      0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };

    static const uint8_t kJpegSolid[] = {
      0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
      0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
      0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
      0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
      0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0B, 0x08, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08,
      0x0B, 0x0C, 0x0B, 0x0A, 0x0C, 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x02, 0x02,
      0x02, 0x02, 0x02, 0x02, 0x05, 0x03, 0x03, 0x05, 0x0A, 0x07, 0x06, 0x07, 0x0A, 0x0A, 0x0A, 0x0A,
      0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
      0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A,
      0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0xFF, 0xC0,
      0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
      0x01, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
      0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05,
      0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
      0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23,
      0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17,
      0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A,
      0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
      0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
      0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
      0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7,
      0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5,
      0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1,
      0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00, 0x1F, 0x01, 0x00, 0x03,
      0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
      0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00,
      0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00,
      0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13,
      0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15,
      0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27,
      0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
      0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
      0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
      0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
      0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4,
      0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2,
      0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9,
      0xFA, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xCF,
      0xA2, 0x8A, 0x2B, 0xF9, 0x5C, 0xFE, 0xD8, 0x3F, 0xFF, 0xD9,
    };

    static const uint8_t kJpegGray[] = {
      0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
      0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x02,
      0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02, 0x02, 0x04, 0x03, 0x02, 0x02, 0x02, 0x02, 0x05, 0x04,
      0x04, 0x03, 0x04, 0x06, 0x05, 0x06, 0x06, 0x06, 0x05, 0x06, 0x06, 0x06, 0x07, 0x09, 0x08, 0x06,
      0x07, 0x09, 0x07, 0x06, 0x06, 0x08, 0x0B, 0x08, 0x09, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x06, 0x08,
      0x0B, 0x0C, 0x0B, 0x0A, 0x0C, 0x09, 0x0A, 0x0A, 0x0A, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x0A,
      0x00, 0x18, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01,
      0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04,
      0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03,
      0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00,
      0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32,
      0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72,
      0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35,
      0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55,
      0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75,
      0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94,
      0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2,
      0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9,
      0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6,
      0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xDA,
      0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00, 0xFC, 0x8B, 0xFD, 0x92, 0x7F, 0xE5, 0xD7, 0xF0,
      0xAF, 0xD4, 0x0F, 0xD9, 0x27, 0xFE, 0x5D, 0x7F, 0x0A, 0xFD, 0x40, 0xFD, 0x92, 0x7F, 0xE5, 0xD7,
      0xF0, 0xAF, 0xE5, 0xFF, 0x00, 0xF6, 0x49, 0xFF, 0x00, 0x97, 0x5F, 0xC2, 0xBF, 0x50, 0x3F, 0x64,
      0x9F, 0xF9, 0x75, 0xFC, 0x2B, 0xF5, 0x03, 0xF6, 0x49, 0xFF, 0x00, 0x97, 0x5F, 0xC2, 0xBF, 0xFF,
      0xD9,
    };

    static const uint8_t kJpegProgressive[] = {
      0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01,
      0x00, 0x01, 0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03,
      0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05, 0x05, 0x04, 0x04, 0x05, 0x0A, 0x07,
      0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C, 0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D,
      0x0E, 0x11, 0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0C, 0x0F,
      0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x03, 0x04,
      0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0D, 0x0B, 0x0D, 0x14, 0x14, 0x14, 0x14,
      0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
      0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
      0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xFF, 0xC2,
      0x00, 0x11, 0x08, 0x00, 0x10, 0x00, 0x10, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11,
      0x01, 0xFF, 0xC4, 0x00, 0x14, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xC4, 0x00, 0x14, 0x01, 0x01, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xFF, 0xDA, 0x00,
      0x0C, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0x04, 0x52, 0x1F, 0xFF, 0xC4,
      0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x20, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01, 0x05, 0x02, 0x1F, 0xFF,
      0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x03, 0x01, 0x01, 0x3F, 0x01, 0x7F,
      0xFF, 0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x02, 0x01, 0x01, 0x3F, 0x01,
      0x7F, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x06, 0x3F,
      0x02, 0x1F, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x01,
      0x3F, 0x21, 0x1F, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00,
      0x10, 0x0F, 0xFF, 0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x03, 0x01, 0x01,
      0x3F, 0x10, 0x7F, 0xFF, 0xC4, 0x00, 0x14, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x02, 0x01,
      0x01, 0x3F, 0x10, 0x7F, 0xFF, 0xC4, 0x00, 0x14, 0x10, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01,
      0x00, 0x01, 0x3F, 0x10, 0x1F, 0xFF, 0xD9,
    };

} // namespace codec_test


// ============================================================================

TEST_CASE("Image codec: every PNG colour type and depth decodes exactly", "[image][codec][png]") {
  static const codec_test::Case cases[] = {
      {"gray 1-bit", 0, 1, false, 0},         {"gray 2-bit", 0, 2, false, 0},
      {"gray 4-bit", 0, 4, false, 0},         {"gray 8-bit", 0, 8, false, 0},
      {"gray 16-bit", 0, 16, false, 0},       {"gray 8-bit interlaced", 0, 8, true, 0},
      {"gray 16-bit interlaced", 0, 16, true, 0},
      {"gray+alpha 8-bit", 4, 8, false, 0},   {"gray+alpha 16-bit", 4, 16, false, 0},
      {"palette 1-bit", 3, 1, false, 0},      {"palette 2-bit", 3, 2, false, 0},
      {"palette 4-bit", 3, 4, false, 0},      {"palette 8-bit", 3, 8, false, 0},
      {"palette 2-bit interlaced", 3, 2, true, 0},
      {"RGB 8-bit", 2, 8, false, 0},          {"RGB 16-bit", 2, 16, false, 0},
      {"RGB 8-bit interlaced", 2, 8, true, 0}, {"RGB 16-bit interlaced", 2, 16, true, 0},
      {"RGBA 8-bit", 6, 8, false, 0},         {"RGBA 16-bit", 6, 16, false, 0},
      {"RGBA 8-bit interlaced", 6, 8, true, 0}, {"RGBA 16-bit interlaced", 6, 16, true, 0},
  };
  uint32_t seed = 1;
  for (const auto &c : cases)
    for (auto [w, h] : codec_test::caseSizes()) {
      INFO(c.name << " " << w << "x" << h);
      auto b = codec_test::build(c, w, h, seed++);
      const bool wide = c.depth == 16;

      std::string err;
      auto info = liteui_image::probeMemory(b.png.data(), b.png.size(), &err);
      REQUIRE(info.has_value());
      CHECK(info->format == liteui_image::ImageFileFormat::Png);
      CHECK(info->width == w);
      CHECK(info->height == h);
      CHECK(info->bitDepth == c.depth);
      CHECK(info->hasAlpha == (c.colorType == 4 || c.colorType == 6));
      CHECK_FALSE(info->truncated);
      CHECK(info->bufferFormat() == (wide ? PixelFormat::RGBA16_UNORM : PixelFormat::RGBA8_UNORM));

      auto img = liteui_image::decodeMemoryBuffer(b.png.data(), b.png.size(), &err);
      INFO(err);
      REQUIRE(img.has_value());
      CHECK(img->valid());
      CHECK(img->format == info->bufferFormat());
      CHECK(img->width == w);
      CHECK(img->height == h);
      const auto expect = codec_test::expectedRgba(w, h, b.spec, b.px);
      const long bad = codec_test::firstMismatch(*img, expect);
      INFO("first mismatch: " << codec_test::describeMismatch(*img, expect, bad));
      CHECK(bad == -1);
    }
}

TEST_CASE("Image codec: PNG tRNS (colour key and palette alpha) becomes alpha", "[image][codec][png]") {
  static const codec_test::Case cases[] = {
      {"gray 2-bit + key", 0, 2, false, 1},    {"gray 8-bit + key", 0, 8, false, 1},
      {"gray 16-bit + key", 0, 16, false, 1},  {"RGB 8-bit + key", 2, 8, false, 1},
      {"RGB 16-bit + key", 2, 16, false, 1},   {"palette 4-bit + alpha", 3, 4, false, 2},
      {"palette 8-bit + alpha", 3, 8, false, 2}, {"palette 2-bit + alpha interlaced", 3, 2, true, 2},
  };
  uint32_t seed = 500;
  for (const auto &c : cases)
    for (auto [w, h] : codec_test::caseSizes()) {
      INFO(c.name << " " << w << "x" << h);
      auto b = codec_test::build(c, w, h, seed++);
      std::string err;
      auto info = liteui_image::probeMemory(b.png.data(), b.png.size(), &err);
      REQUIRE(info.has_value());
      CHECK(info->hasAlpha); // a tRNS chunk counts as alpha
      auto img = liteui_image::decodeMemoryBuffer(b.png.data(), b.png.size(), &err);
      INFO(err);
      REQUIRE(img.has_value());
      const auto expect = codec_test::expectedRgba(w, h, b.spec, b.px);
      // Alpha must be exact everywhere; the colour only where the pixel is visible.
      const long bad = codec_test::firstMismatch(*img, expect, /*ignoreHiddenColor=*/true);
      INFO("first mismatch: " << codec_test::describeMismatch(*img, expect, bad));
      CHECK(bad == -1);
    }
}

TEST_CASE("Image codec: 16-bit samples keep their value and byte order", "[image][codec][png]") {
  codec_test::PngSpec spec;
  spec.colorType = 6; spec.depth = 16;
  codec_test::Samples px = {{0x1234, 0xABCD, 0x00FF, 0xFF00, 0x0001, 0x0100, 0xFFFF, 0x8000}};
  auto png = codec_test::makePng(2, 1, spec, px);
  auto img = liteui_image::decodeMemoryBuffer(png.data(), png.size());
  REQUIRE(img.has_value());
  REQUIRE(img->format == PixelFormat::RGBA16_UNORM);
  const uint32_t want[8] = {0x1234, 0xABCD, 0x00FF, 0xFF00, 0x0001, 0x0100, 0xFFFF, 0x8000};
  for (int i = 0; i < 8; ++i) CHECK(codec_test::sampleAt(*img, i / 4, 0, i % 4) == want[i]);
}

TEST_CASE("Image codec: files made by another encoder (Pillow) decode correctly", "[image][codec][png]") {
  std::string err;
  { // RGBA 8-bit with real deflate + adaptive filters
    auto info = liteui_image::probeMemory(codec_test::kPngRgba8, sizeof codec_test::kPngRgba8, &err);
    REQUIRE(info.has_value());
    CHECK(info->width == 9);
    CHECK(info->height == 7);
    CHECK(info->bitDepth == 8);
    CHECK(info->hasAlpha);
    auto img = liteui_image::decodeMemoryBuffer(codec_test::kPngRgba8, sizeof codec_test::kPngRgba8, &err);
    INFO(err);
    REQUIRE(img.has_value());
    REQUIRE(img->format == PixelFormat::RGBA8_UNORM);
    bool ok = true;
    for (uint32_t y = 0; y < 7; ++y)
      for (uint32_t x = 0; x < 9; ++x) {
        const uint32_t want[4] = {(x * 29 + y * 7) & 255, (x * 5 + y * 31) & 255, (x * y * 11) & 255,
                                  255 - ((x * 13 + y * 3) & 255)};
        for (int c = 0; c < 4; ++c) ok &= codec_test::sampleAt(*img, x, y, c) == want[c];
      }
    CHECK(ok);
  }
  { // 16-bit gray
    auto info = liteui_image::probeMemory(codec_test::kPngGray16, sizeof codec_test::kPngGray16, &err);
    REQUIRE(info.has_value());
    CHECK(info->bitDepth == 16);
    CHECK_FALSE(info->hasAlpha);
    auto img = liteui_image::decodeMemoryBuffer(codec_test::kPngGray16, sizeof codec_test::kPngGray16, &err);
    INFO(err);
    REQUIRE(img.has_value());
    REQUIRE(img->format == PixelFormat::RGBA16_UNORM);
    bool ok = true;
    for (uint32_t y = 0; y < 6; ++y)
      for (uint32_t x = 0; x < 11; ++x) {
        const uint32_t v = (x * 7919 + y * 10007) & 0xFFFF;
        ok &= codec_test::sampleAt(*img, x, y, 0) == v && codec_test::sampleAt(*img, x, y, 1) == v &&
              codec_test::sampleAt(*img, x, y, 2) == v && codec_test::sampleAt(*img, x, y, 3) == 65535;
      }
    CHECK(ok);
  }
}

TEST_CASE("Image codec: decodeFileBuffer and probeFile read from disk", "[image][codec][png]") {
  const codec_test::Case c{"RGBA 16-bit interlaced", 6, 16, true, 0};
  auto b = codec_test::build(c, 13, 9, 77);
  TempFile f(std::string(b.png.begin(), b.png.end()), "liteui_codec_read.png");
  const std::string path = codec_test::utf8(f.path);
  std::string err;
  auto info = liteui_image::probeFile(path, &err);
  REQUIRE(info.has_value());
  CHECK(info->bitDepth == 16);
  CHECK(info->width == 13);
  CHECK_FALSE(info->truncated);
  auto fromFile = liteui_image::decodeFileBuffer(path, &err);
  auto fromMem = liteui_image::decodeMemoryBuffer(b.png.data(), b.png.size(), &err);
  REQUIRE(fromFile.has_value());
  REQUIRE(fromMem.has_value());
  CHECK(codec_test::sameImage(*fromFile, *fromMem));
}

TEST_CASE("Image codec: JPEG files decode to RGBA8", "[image][codec][jpeg]") {
  struct J { const char *name; const uint8_t *data; size_t size; uint32_t w, h; };
  const J files[] = {
      {"solid 4:2:0", codec_test::kJpegSolid, sizeof codec_test::kJpegSolid, 16, 16},
      {"gray", codec_test::kJpegGray, sizeof codec_test::kJpegGray, 24, 10},
      {"progressive", codec_test::kJpegProgressive, sizeof codec_test::kJpegProgressive, 16, 16},
  };
  for (const auto &j : files) {
    INFO(j.name);
    std::string err;
    auto info = liteui_image::probeMemory(j.data, j.size, &err);
    REQUIRE(info.has_value());
    CHECK(info->format == liteui_image::ImageFileFormat::Jpeg);
    CHECK(info->width == j.w);
    CHECK(info->height == j.h);
    CHECK(info->bitDepth == 8);
    CHECK_FALSE(info->hasAlpha);
    CHECK_FALSE(info->truncated);
    auto img = liteui_image::decodeMemoryBuffer(j.data, j.size, &err);
    INFO(err);
    REQUIRE(img.has_value());
    CHECK(img->format == PixelFormat::RGBA8_UNORM);
    CHECK(img->width == j.w);
    CHECK(img->height == j.h);
    // JPEG is lossy and decoders differ slightly (IDCT, chroma upsampling), so compare with a tolerance.
    auto within = [](uint32_t got, int want, int tol) { return std::abs(int(got) - want) <= tol; };
    bool ok = true;
    for (uint32_t y = 0; y < j.h; ++y)
      for (uint32_t x = 0; x < j.w; ++x) {
        const uint32_t r = codec_test::sampleAt(*img, x, y, 0), g = codec_test::sampleAt(*img, x, y, 1),
                       b = codec_test::sampleAt(*img, x, y, 2), a = codec_test::sampleAt(*img, x, y, 3);
        ok &= a == 255;
        if (j.data == codec_test::kJpegSolid) ok &= within(r, 200, 3) && within(g, 100, 3) && within(b, 50, 3);
        if (j.data == codec_test::kJpegProgressive) ok &= within(r, 30, 3) && within(g, 160, 3) && within(b, 220, 3);
        if (j.data == codec_test::kJpegGray) ok &= within(r, int(x) * 10, 6) && within(g, int(r), 1) && within(b, int(r), 1);
      }
    CHECK(ok);
  }
}

TEST_CASE("Image codec: truncated PNG is rejected", "[image][codec][png]") {
  const codec_test::Case cases[] = {{"RGBA 8-bit", 6, 8, false, 0},
                                    {"gray 16-bit interlaced", 0, 16, true, 0},
                                    {"palette 4-bit + alpha", 3, 4, false, 2}};
  uint32_t seed = 900;
  for (const auto &c : cases) {
    auto b = codec_test::build(c, 13, 9, seed++);
    const size_t n = b.png.size();
    // before IEND, in the middle, just after IHDR (8 + 25 bytes), inside IHDR, inside the signature
    for (size_t cut : {n - 12, n / 2, size_t(33), size_t(20), size_t(5)}) {
      INFO(c.name << " cut at " << cut << " of " << n);
      std::string err;
      auto info = liteui_image::probeMemory(b.png.data(), cut, &err);
      if (cut >= 33) {
        REQUIRE(info.has_value());
        CHECK(info->truncated);
      } else {
        CHECK_FALSE(info.has_value());
      }
      err.clear();
      CHECK_FALSE(liteui_image::decodeMemoryBuffer(b.png.data(), cut, &err).has_value());
      CHECK_FALSE(err.empty());
      if (cut >= 33) CHECK(err == "PNG is truncated");
    }
  }
}

TEST_CASE("Image codec: garbage and missing input fail cleanly", "[image][codec]") {
  std::string err;
  CHECK_FALSE(liteui_image::decodeMemoryBuffer(nullptr, 0, &err).has_value());
  CHECK_FALSE(err.empty());
  CHECK_FALSE(liteui_image::probeMemory(nullptr, 0, &err).has_value());

  codec_test::Bytes junk(1000);
  std::mt19937 rng(5);
  for (auto &b : junk) b = uint8_t(rng());
  err.clear();
  CHECK_FALSE(liteui_image::decodeMemoryBuffer(junk.data(), junk.size(), &err).has_value());
  CHECK_FALSE(err.empty());

  const char *text = "hello world, not an image";
  err.clear();
  CHECK_FALSE(liteui_image::decodeMemoryBuffer(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &err)
                  .has_value());
  CHECK_FALSE(err.empty());

  // a JPEG signature with the rest cut away / corrupted
  err.clear();
  CHECK_FALSE(liteui_image::probeMemory(codec_test::kJpegSolid, 4, &err).has_value());
  CHECK_FALSE(err.empty());
  codec_test::Bytes notJpeg(codec_test::kJpegSolid, codec_test::kJpegSolid + sizeof codec_test::kJpegSolid);
  notJpeg[2] = 0x00; // FF D8 00: no longer a JPEG signature
  CHECK_FALSE(liteui_image::probeMemory(notJpeg.data(), notJpeg.size(), &err).has_value());

  const std::string missing = codec_test::utf8(std::filesystem::temp_directory_path() / "liteui_codec_no_such_file.png");
  err.clear();
  CHECK_FALSE(liteui_image::decodeFileBuffer(missing, &err).has_value());
  CHECK(err == "failed to open file");
  err.clear();
  CHECK_FALSE(liteui_image::probeFile(missing, &err).has_value());
  CHECK(err == "failed to open file");

  TempFile empty("", "liteui_codec_empty.png");
  err.clear();
  CHECK_FALSE(liteui_image::decodeFileBuffer(codec_test::utf8(empty.path), &err).has_value());
  CHECK(err == "empty file");
  err.clear();
  CHECK_FALSE(liteui_image::probeFile(codec_test::utf8(empty.path), &err).has_value());
  CHECK(err == "empty file");
}

TEST_CASE("Image codec: damaged files never crash the decoder", "[image][codec]") {
  // The point is that decoding returns (with a value or an error) for any input;
  // it is most useful under the sanitizer build.
  std::mt19937 rng(42);
  std::vector<codec_test::Bytes> sources;
  const codec_test::Case cases[] = {{"RGBA 16-bit", 6, 16, false, 0},
                                    {"palette 4-bit + alpha", 3, 4, false, 2},
                                    {"RGB 8-bit interlaced", 2, 8, true, 0},
                                    {"gray 16-bit interlaced", 0, 16, true, 0}};
  uint32_t seed = 31;
  for (const auto &c : cases) sources.push_back(codec_test::build(c, 13, 9, seed++).png);
  sources.emplace_back(codec_test::kPngRgba8, codec_test::kPngRgba8 + sizeof codec_test::kPngRgba8);
  sources.emplace_back(codec_test::kJpegProgressive, codec_test::kJpegProgressive + sizeof codec_test::kJpegProgressive);
  sources.emplace_back(codec_test::kJpegSolid, codec_test::kJpegSolid + sizeof codec_test::kJpegSolid);
  int runs = 0;
  for (const auto &src : sources)
    for (int k = 0; k < 150; ++k) {
      codec_test::Bytes b = src;
      for (int n = 0; n < 1 + k % 4; ++n) b[rng() % b.size()] ^= uint8_t(1 + rng() % 255);
      std::string err;
      (void)liteui_image::decodeMemoryBuffer(b.data(), b.size(), &err);
      (void)liteui_image::probeMemory(b.data(), b.size(), &err);
      ++runs;
    }
  CHECK(runs == int(sources.size()) * 150);
}

TEST_CASE("Image codec: PNG encode -> decode round trips", "[image][codec][png]") {
  int combos = 0;
  for (PixelFormat f : {PixelFormat::RGBA8_UNORM, PixelFormat::RGBA16_UNORM})
    for (bool opaque : {false, true})
      for (auto [w, h] : {std::pair<uint32_t, uint32_t>{1, 1}, {13, 7}, {256, 3}, {3, 120}})
        for (int level : {0, 3, 9})
          for (bool drop : {true, false}) {
            INFO((f == PixelFormat::RGBA16_UNORM ? "16-bit " : "8-bit ")
                 << (opaque ? "opaque " : "alpha ") << w << "x" << h << " level " << level
                 << (drop ? " dropOpaqueAlpha" : " keepAlpha"));
            auto img = codec_test::randomImage(w, h, f, opaque, w * 131 + h * 7 + level);
            liteui_image::PngEncodeOptions o;
            o.compression = level;
            o.dropOpaqueAlpha = drop;
            codec_test::Bytes out;
            std::string err;
            REQUIRE(liteui_image::encodePngMemory(out, img, &err, o));
            INFO(err);
            auto info = liteui_image::probeMemory(out.data(), out.size(), &err);
            REQUIRE(info.has_value());
            CHECK(info->bitDepth == (f == PixelFormat::RGBA16_UNORM ? 16 : 8));
            CHECK(info->hasAlpha == !(opaque && drop)); // opaque images are written as RGB unless told otherwise
            CHECK_FALSE(info->truncated);
            CHECK(info->width == w);
            CHECK(info->height == h);
            auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err);
            INFO(err);
            REQUIRE(back.has_value());
            CHECK(codec_test::sameImage(img, *back));
            ++combos;
          }
  CHECK(combos == 96);

  // An opaque picture is smaller as RGB than as RGBA.
  auto op = codec_test::randomImage(64, 64, PixelFormat::RGBA8_UNORM, true, 1);
  liteui_image::PngEncodeOptions keep;
  keep.dropOpaqueAlpha = false;
  codec_test::Bytes rgb, rgba;
  REQUIRE(liteui_image::encodePngMemory(rgb, op));
  REQUIRE(liteui_image::encodePngMemory(rgba, op, nullptr, keep));
  CHECK(rgb.size() < rgba.size());

  // A larger 16-bit picture, to exercise row handling at a realistic size.
  auto big = codec_test::randomImage(640, 480, PixelFormat::RGBA16_UNORM, false, 8);
  codec_test::Bytes out;
  REQUIRE(liteui_image::encodePngMemory(out, big));
  auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size());
  REQUIRE(back.has_value());
  CHECK(codec_test::sameImage(big, *back));
}

TEST_CASE("Image codec: PNG encode refuses unsupported images", "[image][codec][png]") {
  codec_test::Bytes out{1, 2, 3};
  std::string err;
  for (PixelFormat f : {PixelFormat::RGBA16_FLOAT, PixelFormat::RGBA32_FLOAT}) {
    auto fp = ImageBuffer::create(4, 4, f);
    REQUIRE(fp.has_value());
    out = {1, 2, 3};
    err.clear();
    CHECK_FALSE(liteui_image::encodePngMemory(out, *fp, &err));
    CHECK_FALSE(err.empty());
    CHECK(out.empty()); // a failed encode leaves nothing behind
  }
  ImageBuffer empty;
  err.clear();
  CHECK_FALSE(liteui_image::encodePngMemory(out, empty, &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("Image codec: encodePng / encodeJpeg write files atomically", "[image][codec]") {
  const auto dir = std::filesystem::temp_directory_path();
  const std::string png = codec_test::utf8(dir / "liteui_codec_out.png");
  const std::string jpg = codec_test::utf8(dir / "liteui_codec_out.jpg");
  struct Cleanup {
    std::vector<std::string> paths;
    ~Cleanup() {
      std::error_code ec;
      for (auto &p : paths) {
        std::filesystem::remove(codec_test::fsPath(p), ec);
        std::filesystem::remove(codec_test::fsPath(p + ".tmp"), ec);
      }
    }
  } cleanup{{png, jpg}};

  std::string err;
  auto img16 = codec_test::randomImage(40, 30, PixelFormat::RGBA16_UNORM, false, 9);
  REQUIRE(liteui_image::encodePng(png, img16, &err));
  CHECK(codec_test::fileExists(png));
  CHECK_FALSE(codec_test::fileExists(png + ".tmp")); // the temp file was renamed over the target
  auto back = liteui_image::decodeFileBuffer(png, &err);
  REQUIRE(back.has_value());
  CHECK(codec_test::sameImage(img16, *back));

  // Overwriting replaces the old file.
  auto img8 = codec_test::randomImage(5, 5, PixelFormat::RGBA8_UNORM, false, 10);
  REQUIRE(liteui_image::encodePng(png, img8, &err));
  auto back8 = liteui_image::decodeFileBuffer(png, &err);
  REQUIRE(back8.has_value());
  CHECK(codec_test::sameImage(img8, *back8));
  CHECK_FALSE(codec_test::fileExists(png + ".tmp"));

  // A failing encode must not touch an existing file.
  const codec_test::Bytes before = codec_test::readAll(png);
  auto fp = ImageBuffer::create(2, 2, PixelFormat::RGBA16_FLOAT);
  err.clear();
  CHECK_FALSE(liteui_image::encodePng(png, *fp, &err));
  CHECK(codec_test::readAll(png) == before);

  // An unwritable location reports an error and leaves no temp file behind.
  const std::string badDir = codec_test::utf8(dir / "liteui_codec_no_such_dir" / "x.png");
  err.clear();
  CHECK_FALSE(liteui_image::encodePng(badDir, img8, &err));
  CHECK_FALSE(err.empty());
  CHECK_FALSE(codec_test::fileExists(badDir + ".tmp"));

  // JPEG goes through the same path.
  REQUIRE(liteui_image::encodeJpeg(jpg, img8, 90, &err));
  CHECK(codec_test::fileExists(jpg));
  CHECK_FALSE(codec_test::fileExists(jpg + ".tmp"));
  auto j = liteui_image::decodeFileBuffer(jpg, &err);
  REQUIRE(j.has_value());
  CHECK(j->width == 5);
  CHECK(j->format == PixelFormat::RGBA8_UNORM);
}

TEST_CASE("Image codec: JPEG encode -> decode", "[image][codec][jpeg]") {
  const uint32_t w = 96, h = 64;
  auto img = ImageBuffer::create(w, h, PixelFormat::RGBA8_UNORM);
  REQUIRE(img.has_value());
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      img->row(y)[x * 4 + 0] = std::byte(x * 255 / (w - 1));
      img->row(y)[x * 4 + 1] = std::byte(y * 255 / (h - 1));
      img->row(y)[x * 4 + 2] = std::byte(128 + int(100 * std::sin(x * 0.1)));
      img->row(y)[x * 4 + 3] = std::byte(x % 2 ? 0 : 255); // Discard must ignore this
    }
  codec_test::Bytes out;
  std::string err;
  size_t prevSize = 0;
  for (int q : {10, 50, 90, 100}) {
    INFO("quality " << q);
    liteui_image::JpegEncodeOptions opts;
    opts.quality = q;
    opts.alpha = liteui_image::JpegAlpha::Discard; // this test compares against the stored RGB
    REQUIRE(liteui_image::encodeJpegMemory(out, *img, opts, &err));
    INFO(err);
    auto info = liteui_image::probeMemory(out.data(), out.size(), &err);
    REQUIRE(info.has_value());
    CHECK(info->format == liteui_image::ImageFileFormat::Jpeg);
    CHECK(info->width == w);
    CHECK(info->height == h);
    auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err);
    REQUIRE(back.has_value());
    CHECK(back->format == PixelFormat::RGBA8_UNORM);
    double sum = 0;
    bool opaque = true;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c)
          sum += std::abs(int(codec_test::sampleAt(*back, x, y, c)) - int(codec_test::sampleAt(*img, x, y, c)));
        opaque &= codec_test::sampleAt(*back, x, y, 3) == 255;
      }
    const double mean = sum / (double(w) * h * 3);
    CHECK(opaque);
    if (q == 100) CHECK(mean < 3.0);
    if (q == 90) CHECK(mean < 6.0);
    if (q >= 50) CHECK(out.size() > prevSize); // higher quality -> larger file
    prevSize = out.size();
  }
  // Quality outside 1..100 is clamped, not refused.
  CHECK(liteui_image::encodeJpegMemory(out, *img, 0, &err));
  CHECK(liteui_image::encodeJpegMemory(out, *img, 5000, &err));
  // Odd sizes and a single pixel.
  for (auto [ow, oh] : {std::pair<uint32_t, uint32_t>{1, 1}, {33, 17}, {17, 33}}) {
    auto tiny = codec_test::randomImage(ow, oh, PixelFormat::RGBA8_UNORM, true, ow);
    REQUIRE(liteui_image::encodeJpegMemory(out, tiny, 90, &err));
    auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err);
    REQUIRE(back.has_value());
    CHECK(back->width == ow);
    CHECK(back->height == oh);
  }
}

TEST_CASE("Image codec: JPEG encode refuses anything but RGBA8", "[image][codec][jpeg]") {
  codec_test::Bytes out{9};
  std::string err;
  for (PixelFormat f : {PixelFormat::RGBA16_UNORM, PixelFormat::RGBA16_FLOAT, PixelFormat::RGBA32_FLOAT}) {
    auto img = ImageBuffer::create(4, 4, f);
    REQUIRE(img.has_value());
    out = {9};
    err.clear();
    CHECK_FALSE(liteui_image::encodeJpegMemory(out, *img, 90, &err));
    CHECK_FALSE(err.empty());
    CHECK(out.empty());
  }
  ImageBuffer empty;
  err.clear();
  CHECK_FALSE(liteui_image::encodeJpegMemory(out, empty, 90, &err));
  CHECK_FALSE(err.empty());
}

TEST_CASE("Image codec: a 16-bit PNG survives the float formats", "[image][codec][png][convert]") {
  auto src = codec_test::randomImage(64, 48, PixelFormat::RGBA16_UNORM, false, 21);
  codec_test::Bytes png;
  REQUIRE(liteui_image::encodePngMemory(png, src));
  auto img = liteui_image::decodeMemoryBuffer(png.data(), png.size());
  REQUIRE(img.has_value());
  REQUIRE(img->format == PixelFormat::RGBA16_UNORM);

  // FP32 holds every 16-bit code, so UNORM16 -> FP32 -> UNORM16 is the identity.
  auto f32 = codec_test::convertTo(*img, PixelFormat::RGBA32_FLOAT);
  REQUIRE(f32.has_value());
  auto back32 = codec_test::convertTo(*f32, PixelFormat::RGBA16_UNORM);
  REQUIRE(back32.has_value());
  CHECK(codec_test::sameImage(*img, *back32));

  // FP16 (linear light, 10-bit mantissa) keeps only about 12 bits near white.
  auto f16 = codec_test::convertTo(*img, PixelFormat::RGBA16_FLOAT);
  REQUIRE(f16.has_value());
  auto back16 = codec_test::convertTo(*f16, PixelFormat::RGBA16_UNORM);
  REQUIRE(back16.has_value());
  int maxDiff = 0;
  for (uint32_t y = 0; y < img->height; ++y)
    for (uint32_t x = 0; x < img->width; ++x)
      for (int c = 0; c < 4; ++c)
        maxDiff = std::max(maxDiff, std::abs(int(codec_test::sampleAt(*img, x, y, c)) -
                                             int(codec_test::sampleAt(*back16, x, y, c))));
  CHECK(maxDiff <= 16); // of 65535
}

TEST_CASE("Image codec: JPEG alpha is flattened over white or discarded on request", "[image][codec][jpeg]") {
  const uint32_t w = 64, h = 16;
  auto img = ImageBuffer::create(w, h, PixelFormat::RGBA8_UNORM);
  REQUIRE(img.has_value());
  auto src = [](uint32_t x, int c) { return c == 0 ? 200u : c == 1 ? 60u + x : 20u; }; // smooth, so JPEG error stays small
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      for (int c = 0; c < 3; ++c) img->row(y)[x * 4 + c] = std::byte(src(x, c));
      img->row(y)[x * 4 + 3] = std::byte(x * 255 / (w - 1)); // alpha ramps 0 -> 255 across the row
    }
  codec_test::Bytes out;
  std::string err;
  auto decode = [&](liteui_image::JpegAlpha alpha) {
    liteui_image::JpegEncodeOptions o;
    o.quality = 100;
    o.chroma = liteui_image::JpegChroma::S444;
    o.alpha = alpha;
    REQUIRE(liteui_image::encodeJpegMemory(out, *img, o, &err));
    auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err);
    REQUIRE(back.has_value());
    return std::move(*back);
  };

  { // composite over white: c' = (c*a + 255*(255-a) + 127) / 255
    const ImageBuffer back = decode(liteui_image::JpegAlpha::FlattenOverWhite);
    int worst = 0;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const uint32_t a = x * 255 / (w - 1);
        for (int c = 0; c < 3; ++c) {
          const int want = int((src(x, c) * a + 255 * (255 - a) + 127) / 255);
          worst = std::max(worst, std::abs(int(codec_test::sampleAt(back, x, y, c)) - want));
        }
      }
    CHECK(worst <= 4);
    CHECK(codec_test::sampleAt(back, 0, 0, 0) >= 252);     // alpha 0 -> white
    CHECK(codec_test::sampleAt(back, 0, 0, 2) >= 252);
    CHECK(std::abs(int(codec_test::sampleAt(back, w - 1, 0, 0)) - 200) <= 3); // alpha 255 -> unchanged
  }
  { // discard: the stored colour is written as it is
    const ImageBuffer back = decode(liteui_image::JpegAlpha::Discard);
    int worst = 0;
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x)
        for (int c = 0; c < 3; ++c)
          worst = std::max(worst, std::abs(int(codec_test::sampleAt(back, x, y, c)) - int(src(x, c))));
    CHECK(worst <= 4);
  }
  { // the default options flatten
    REQUIRE(liteui_image::encodeJpegMemory(out, *img, 100, &err));
    auto back = liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err);
    REQUIRE(back.has_value());
    CHECK(codec_test::sampleAt(*back, 0, 0, 1) >= 250);
  }
}

TEST_CASE("Image codec: JPEG chroma subsampling follows the options", "[image][codec][jpeg]") {
  auto img = ImageBuffer::create(48, 40, PixelFormat::RGBA8_UNORM);
  REQUIRE(img.has_value());
  for (uint32_t y = 0; y < 40; ++y)
    for (uint32_t x = 0; x < 48; ++x) {
      img->row(y)[x * 4 + 0] = std::byte(x * 5);
      img->row(y)[x * 4 + 1] = std::byte(y * 6);
      img->row(y)[x * 4 + 2] = std::byte((x * y) & 255);
      img->row(y)[x * 4 + 3] = std::byte(255);
    }
  struct C { const char *name; int quality; liteui_image::JpegChroma chroma; int luma; };
  const C cases[] = {
      {"Auto at 95 -> 4:4:4", 95, liteui_image::JpegChroma::Auto, 1},
      {"Auto at 90 -> 4:4:4", 90, liteui_image::JpegChroma::Auto, 1},
      {"Auto at 89 -> 4:2:0", 89, liteui_image::JpegChroma::Auto, 2},
      {"Auto at 50 -> 4:2:0", 50, liteui_image::JpegChroma::Auto, 2},
      {"S420 at 95", 95, liteui_image::JpegChroma::S420, 2},
      {"S444 at 30", 30, liteui_image::JpegChroma::S444, 1},
  };
  for (const auto &c : cases) {
    INFO(c.name);
    liteui_image::JpegEncodeOptions o;
    o.quality = c.quality;
    o.chroma = c.chroma;
    codec_test::Bytes out;
    std::string err;
    REQUIRE(liteui_image::encodeJpegMemory(out, *img, o, &err));
    INFO(err);
    const auto s = codec_test::lumaSampling(out);
    CHECK(s.first == c.luma);
    CHECK(s.second == c.luma);
    CHECK(liteui_image::decodeMemoryBuffer(out.data(), out.size(), &err).has_value());
  }
  // At the same quality, full chroma costs more bytes than 4:2:0.
  liteui_image::JpegEncodeOptions a, b;
  a.quality = b.quality = 80;
  a.chroma = liteui_image::JpegChroma::S420;
  b.chroma = liteui_image::JpegChroma::S444;
  codec_test::Bytes small420, full444;
  REQUIRE(liteui_image::encodeJpegMemory(small420, *img, a));
  REQUIRE(liteui_image::encodeJpegMemory(full444, *img, b));
  CHECK(full444.size() > small420.size());
}

TEST_CASE("Image codec: jpegSizeOk and the JPEG size limit", "[image][codec][jpeg]") {
  CHECK(liteui_image::kJpegMaxSide == 65500);
  CHECK(liteui_image::jpegSizeOk(1, 1));
  CHECK(liteui_image::jpegSizeOk(65500, 65500));
  CHECK_FALSE(liteui_image::jpegSizeOk(0, 10));
  CHECK_FALSE(liteui_image::jpegSizeOk(10, 0));
  CHECK_FALSE(liteui_image::jpegSizeOk(65501, 1));
  CHECK_FALSE(liteui_image::jpegSizeOk(1, 65501));
  auto wide = ImageBuffer::create(65501, 1, PixelFormat::RGBA8_UNORM);
  REQUIRE(wide.has_value());
  codec_test::Bytes out{1};
  std::string err;
  CHECK_FALSE(liteui_image::encodeJpegMemory(out, *wide, 90, &err));
  CHECK(err == "JPEG can't hold a picture this size");
  CHECK(out.empty());
  // PNG has no such limit
  CHECK(liteui_image::encodePngMemory(out, *wide, &err));
}

TEST_CASE("Image codec: PNG chunk CRCs and IDAT are enforced on every platform", "[image][codec][png]") {
  const codec_test::Case plain{"RGBA 8-bit", 6, 8, false, 0};
  auto b = codec_test::build(plain, 13, 9, 4242);
  std::string err;
  REQUIRE(liteui_image::decodeMemoryBuffer(b.png.data(), b.png.size(), &err).has_value());

  auto refused = [&](const codec_test::Bytes &png, const char *expect) {
    std::string e;
    auto img = liteui_image::decodeMemoryBuffer(png.data(), png.size(), &e);
    CHECK_FALSE(img.has_value());
    CHECK(e == expect);
  };
  const char *crcMsg = "PNG is corrupt (chunk CRC mismatch)";

  { // a pixel byte inside the first IDAT chunk
    auto bad = b.png;
    const size_t idat = codec_test::chunkOffset(bad, "IDAT");
    REQUIRE(idat != std::string::npos);
    bad[idat + 8 + 5] ^= 0x01;
    refused(bad, crcMsg);
    // probeMemory only reads the header, so it does not notice (decoding does)
    auto info = liteui_image::probeMemory(bad.data(), bad.size());
    REQUIRE(info.has_value());
    CHECK_FALSE(info->truncated);
  }
  { // the CRC of the second IDAT chunk
    auto bad = b.png;
    const size_t idat2 = codec_test::chunkOffset(bad, "IDAT", 1);
    REQUIRE(idat2 != std::string::npos);
    const size_t len = size_t(bad[idat2]) << 24 | size_t(bad[idat2 + 1]) << 16 | size_t(bad[idat2 + 2]) << 8 | bad[idat2 + 3];
    bad[idat2 + 8 + len + 3] ^= 0x80;
    refused(bad, crcMsg);
  }
  { // IHDR's CRC (the header itself still parses)
    auto bad = b.png;
    bad[8 + 8 + 13 + 3] ^= 0x10;
    refused(bad, crcMsg);
  }
  { // IEND's CRC
    auto bad = b.png;
    bad[bad.size() - 1] ^= 0x01;
    refused(bad, crcMsg);
  }
  { // an ancillary chunk counts too: tRNS of a gray + colour key file
    const codec_test::Case key{"gray 8-bit + key", 0, 8, false, 1};
    auto k = codec_test::build(key, 13, 9, 7);
    REQUIRE(liteui_image::decodeMemoryBuffer(k.png.data(), k.png.size(), &err).has_value());
    auto bad = k.png;
    const size_t trns = codec_test::chunkOffset(bad, "tRNS");
    REQUIRE(trns != std::string::npos);
    bad[trns + 8] ^= 0x01;
    refused(bad, crcMsg);
  }
  { // PLTE
    const codec_test::Case pal{"palette 4-bit", 3, 4, false, 0};
    auto p = codec_test::build(pal, 13, 9, 8);
    auto bad = p.png;
    const size_t plte = codec_test::chunkOffset(bad, "PLTE");
    REQUIRE(plte != std::string::npos);
    bad[plte + 8 + 2] ^= 0x01;
    refused(bad, crcMsg);
  }
  { // no IDAT at all: signature + IHDR + IEND, each with a valid CRC
    codec_test::Bytes png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    codec_test::Bytes ihdr;
    codec_test::put32(ihdr, 1); codec_test::put32(ihdr, 1);
    for (uint8_t v : {8, 6, 0, 0, 0}) ihdr.push_back(v);
    codec_test::addChunk(png, "IHDR", ihdr);
    codec_test::addChunk(png, "IEND", codec_test::Bytes());
    refused(png, "PNG has no image data");
  }
  { // an impossible chunk length is refused (at the header walk, before any decoding)
    auto bad = b.png;
    const size_t idat = codec_test::chunkOffset(bad, "IDAT");
    bad[idat] = 0xFF;
    std::string e;
    CHECK_FALSE(liteui_image::decodeMemoryBuffer(bad.data(), bad.size(), &e).has_value());
    CHECK_FALSE(e.empty());
  }
  { // bytes after IEND are ignored
    auto extra = b.png;
    for (int i = 0; i < 10; ++i) extra.push_back(uint8_t(i));
    CHECK(liteui_image::decodeMemoryBuffer(extra.data(), extra.size(), &err).has_value());
  }
  // files written by the encoders always pass the check
  auto img = codec_test::randomImage(33, 21, PixelFormat::RGBA16_UNORM, false, 5);
  codec_test::Bytes png;
  REQUIRE(liteui_image::encodePngMemory(png, img));
  CHECK(liteui_image::decodeMemoryBuffer(png.data(), png.size(), &err).has_value());
}

