// color_tag.hpp - What a picture file says about its COLOR SPACE, and carrying it into an export.
//
// The editor does its arithmetic on the file's numbers as they are (it assumes sRGB-encoded values and
// converts nothing). That is right while the export says the same thing the source did, and wrong when
// the tag is lost: a Display P3 or Adobe RGB picture exported as an untagged PNG is read as sRGB by every
// other program, and looks dull. So the tags travel with the picture:
//
//   * PNG source -> PNG export: its iCCP / sRGB / gAMA / cHRM chunks are copied (carryColorToPng),
//   * JPEG source -> PNG export: its ICC profile becomes an iCCP chunk,
//   * JPEG source -> JPEG export: already done by image_meta.hpp (embedSourceMetadata),
//   * the Info pane and the status bar say what the open file is (colorTagOf).
// A source with no tag stays untagged: nothing is ever invented. No pixel is converted anywhere.
//
// Not carried: the profile of a PNG source exported as JPEG (it is stored deflate-compressed; reading it
// would need an inflater).
//
// The PNG work is a byte splice like exif::embed: no libpng, no zlib, no liteui, so it behaves the same
// on Windows and Linux and is tested with synthetic files. The chunks go right after IHDR, as the PNG
// specification wants (before PLTE and IDAT; iCCP and sRGB never both, iCCP wins).
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "io/exif.hpp"
#include "io/file_utils.hpp" // fsPath, readFilePrefix, isJpegPath

namespace color_tag
{
  // How much of the file's start is searched for tags: the Info pane's read (a ProPhoto profile is
  // about 0.8 MB, spread over many JPEG APP2 segments) and an export's read (the same limit as
  // embedSourceMetadata).
  constexpr size_t kInfoPrefixBytes = size_t{2} << 20;
  constexpr size_t kCarryPrefixBytes = size_t{8} << 20;
  constexpr size_t kMaxIccBytes = size_t{16} << 20;
  // How much of an exported PNG's start is searched for its first IDAT chunk when tags are inserted.
  constexpr size_t kPngHeadBytes = size_t{1} << 20;
  constexpr size_t kProfileNameMax = 79; // the PNG limit for an iCCP profile name

  // ------------------------------------------------------------ checksums
  // CRC-32 as PNG uses it. `crc` is the value so far (0 to start), like zlib's crc32().
  inline uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0)
  {
    static const std::array<uint32_t, 256> table = []
    {
      std::array<uint32_t, 256> t{};
      for (uint32_t i = 0; i < 256; ++i)
      {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
          c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        t[i] = c;
      }
      return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i)
      crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
  }

  inline uint32_t adler32(const uint8_t *p, size_t n)
  {
    uint32_t a = 1, b = 0;
    while (n > 0)
    {
      const size_t block = std::min<size_t>(n, 5552); // the most that can't overflow 32 bits
      for (size_t i = 0; i < block; ++i)
      {
        a += p[i];
        b += a;
      }
      a %= 65521u;
      b %= 65521u;
      p += block;
      n -= block;
    }
    return (b << 16) | a;
  }

  inline uint32_t be32(const uint8_t *p)
  {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
  }
  inline void putBe32(std::vector<uint8_t> &v, uint32_t x)
  {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
  }

  // `data` as a zlib stream made of STORED (uncompressed) deflate blocks: valid zlib, needs no
  // compressor. A profile is a few hundred bytes to a few hundred KB, so size is no concern.
  inline std::vector<uint8_t> zlibStored(const uint8_t *data, size_t n)
  {
    std::vector<uint8_t> out{0x78, 0x01};
    size_t pos = 0;
    do
    {
      const size_t len = std::min<size_t>(n - pos, 65535);
      const bool last = pos + len >= n;
      out.push_back(last ? 1 : 0); // BFINAL, BTYPE = 00 (stored)
      out.push_back(static_cast<uint8_t>(len & 0xFF));
      out.push_back(static_cast<uint8_t>(len >> 8));
      out.push_back(static_cast<uint8_t>(~len & 0xFF));
      out.push_back(static_cast<uint8_t>((~len >> 8) & 0xFF));
      out.insert(out.end(), data + pos, data + pos + len);
      pos += len;
    } while (pos < n);
    putBe32(out, adler32(data, n));
    return out;
  }

  // ---------------------------------------------------------------- PNG
  inline bool pngSignature(const uint8_t *p, size_t n)
  {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return p && n >= 8 && std::memcmp(p, sig, 8) == 0;
  }

  inline bool isType(const uint8_t *type, const char *name) { return std::memcmp(type, name, 4) == 0; }
  inline bool isColorChunk(const uint8_t *type)
  {
    return isType(type, "iCCP") || isType(type, "sRGB") || isType(type, "gAMA") || isType(type, "cHRM");
  }

  // Calls f(type, data, length, offsetOfChunk, fits) for each chunk after the signature whose 8-byte
  // header is in the buffer, in order, until f returns false or the buffer ends. `fits` says whether
  // the whole chunk (data and CRC) is in the buffer; when it is not, f is called once more for that
  // chunk (it may still want the type, as for a huge IDAT) and the walk stops: f must not read `data`.
  template <class F>
  inline void forEachPngChunk(const uint8_t *p, size_t n, F &&f)
  {
    size_t pos = 8;
    while (pos + 8 <= n)
    {
      const uint32_t len = be32(p + pos);
      if (len > 0x7FFFFFFFu)
        return;
      const bool fits = size_t{len} + 4 <= n - pos - 8;
      if (!f(p + pos + 4, p + pos + 8, static_cast<size_t>(len), pos, fits) || !fits)
        return;
      pos += 12 + static_cast<size_t>(len);
    }
  }

  inline std::vector<uint8_t> makeChunk(const char *type, const uint8_t *data, size_t n)
  {
    std::vector<uint8_t> c;
    c.reserve(n + 12);
    putBe32(c, static_cast<uint32_t>(n));
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data, data + n);
    putBe32(c, crc32(c.data() + 4, n + 4)); // over the type and the data
    return c;
  }

  // The color chunks of a PNG, payloads only (the CRCs are made again on the way out).
  struct PngColor
  {
    std::vector<uint8_t> iccp; // profile name, NUL, compression method, compressed profile
    std::vector<uint8_t> srgb; // 1 byte: the rendering intent
    std::vector<uint8_t> gama; // 4 bytes: image gamma * 100000
    std::vector<uint8_t> chrm; // 32 bytes: white point and primaries
    bool any() const { return !iccp.empty() || !srgb.empty() || !gama.empty() || !chrm.empty(); }
  };

  // Reads the color chunks that come before the image data. Only the first of each kind counts; a
  // chunk with a wrong size or CRC is skipped (a damaged tag is worse than none). False if the buffer
  // does not start like a PNG.
  inline bool readPngColor(const uint8_t *p, size_t n, PngColor *out)
  {
    *out = PngColor{};
    if (!pngSignature(p, n))
      return false;
    forEachPngChunk(p, n, [&](const uint8_t *type, const uint8_t *data, size_t len, size_t, bool fits)
                    {
      if (isType(type, "IDAT") || isType(type, "IEND"))
        return false; // the color chunks all come first
      if (!isColorChunk(type))
        return true;
      if (!fits)
        return false;
      if (crc32(type, len + 4) != be32(data + len))
        return true;
      std::vector<uint8_t> *dst = nullptr;
      bool sizeOk = false;
      if (isType(type, "iCCP"))
      {
        dst = &out->iccp;
        sizeOk = len >= 4 && len <= kMaxIccBytes;
      }
      else if (isType(type, "sRGB"))
      {
        dst = &out->srgb;
        sizeOk = len == 1;
      }
      else if (isType(type, "gAMA"))
      {
        dst = &out->gama;
        sizeOk = len == 4;
      }
      else
      {
        dst = &out->chrm;
        sizeOk = len == 32;
      }
      if (sizeOk && dst->empty())
        dst->assign(data, data + len);
      return true; });
    return true;
  }

  // The chunks to write, in the order the specification wants: iCCP or sRGB (iCCP wins), gAMA, cHRM.
  inline std::vector<uint8_t> colorChunkBytes(const PngColor &c)
  {
    std::vector<uint8_t> out;
    const auto add = [&](const char *type, const std::vector<uint8_t> &payload)
    {
      const std::vector<uint8_t> chunk = makeChunk(type, payload.data(), payload.size());
      out.insert(out.end(), chunk.begin(), chunk.end());
    };
    if (!c.iccp.empty())
      add("iCCP", c.iccp);
    else if (!c.srgb.empty())
      add("sRGB", c.srgb);
    if (!c.gama.empty())
      add("gAMA", c.gama);
    if (!c.chrm.empty())
      add("cHRM", c.chrm);
    return out;
  }

  // Builds the start of a PNG (signature, IHDR, then `c`'s chunks, then the other chunks that came
  // before the first IDAT, without any color chunk of their own) from the first `n` bytes `p` of a PNG.
  // `*idatAt` is the offset of the first IDAT chunk in `p`: everything from there on is copied as it is.
  // False if `p` does not start with a PNG signature and a 13-byte IHDR, or no IDAT starts inside it.
  inline bool buildColoredHead(const uint8_t *p, size_t n, const PngColor &c,
                               std::vector<uint8_t> *head, size_t *idatAt)
  {
    if (!pngSignature(p, n))
      return false;
    size_t ihdrEnd = 0, idat = 0;
    bool first = true, ihdrOk = false;
    std::vector<std::pair<size_t, size_t>> keep; // [begin, end) of the chunks to keep after the new ones
    forEachPngChunk(p, n, [&](const uint8_t *type, const uint8_t *, size_t len, size_t at, bool fits)
                    {
      if (first)
      {
        first = false;
        if (!isType(type, "IHDR") || len != 13 || !fits)
          return false;
        ihdrOk = true;
        ihdrEnd = at + 12 + len;
        return true;
      }
      if (isType(type, "IDAT")) // only its start is needed: the data is copied from here on as it is
      {
        idat = at;
        return false;
      }
      if (isType(type, "IEND") || !fits)
        return false;
      if (!isColorChunk(type))
        keep.emplace_back(at, at + 12 + len);
      return true; });
    if (!ihdrOk || idat == 0)
      return false;
    head->assign(p, p + ihdrEnd);
    const std::vector<uint8_t> chunks = colorChunkBytes(c);
    head->insert(head->end(), chunks.begin(), chunks.end());
    for (const auto &k : keep)
      head->insert(head->end(), p + k.first, p + k.second);
    *idatAt = idat;
    return true;
  }

  // The whole PNG in memory with `c`'s chunks in place of any it had. False (png untouched) if it is
  // not a PNG with an IHDR and an IDAT.
  inline bool insertColor(std::vector<uint8_t> *png, const PngColor &c)
  {
    std::vector<uint8_t> head;
    size_t idat = 0;
    if (!buildColoredHead(png->data(), png->size(), c, &head, &idat))
      return false;
    head.insert(head.end(), png->begin() + static_cast<std::ptrdiff_t>(idat), png->end());
    *png = std::move(head);
    return true;
  }

  // The same for a file, rewritten through a temporary file next to it and renamed over it (the image
  // data, which can be hundreds of MB, is streamed, never held). False with the file untouched if
  // anything fails.
  inline bool insertColorIntoFile(const std::string &path, const PngColor &c)
  {
    namespace fs = std::filesystem;
    const fs::path target = fsPath(path);
    fs::path tmp = target;
    tmp += ".colortag.tmp";
    bool ok = false;
    {
      std::ifstream in(target, std::ios::binary);
      if (!in)
        return false;
      std::vector<uint8_t> prefix(kPngHeadBytes);
      in.read(reinterpret_cast<char *>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
      prefix.resize(static_cast<size_t>(in.gcount()));
      in.clear();
      std::vector<uint8_t> head;
      size_t idat = 0;
      if (!buildColoredHead(prefix.data(), prefix.size(), c, &head, &idat))
        return false;
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out)
        return false;
      out.write(reinterpret_cast<const char *>(head.data()), static_cast<std::streamsize>(head.size()));
      in.seekg(static_cast<std::streamoff>(idat));
      std::vector<char> block(size_t{1} << 20);
      while (in)
      {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
          out.write(block.data(), got);
      }
      out.flush();
      ok = static_cast<bool>(out) && in.eof();
    } // both files are closed here (a rename over an open file fails on Windows)
    std::error_code ec;
    if (ok)
    {
      fs::rename(tmp, target, ec);
      ok = !ec;
    }
    if (!ok)
      fs::remove(tmp, ec);
    return ok;
  }

  // ---------------------------------------------------------------- ICC
  // A readable name for an ICC profile: its 'desc' tag (v2 textDescription or v4 multiLocalizedUnicode,
  // first record), printable ASCII only, at most kProfileNameMax characters. Empty if it can't be read.
  inline std::string sanitizeName(const std::string &s)
  {
    std::string o;
    for (const char ch : s)
    {
      const unsigned char c = static_cast<unsigned char>(ch);
      if (o.size() >= kProfileNameMax)
        break;
      o.push_back(c >= 32 && c < 127 ? static_cast<char>(c) : '?');
    }
    while (!o.empty() && o.back() == ' ')
      o.pop_back();
    const size_t b = o.find_first_not_of(' ');
    return b == std::string::npos ? std::string() : o.substr(b);
  }

  inline std::string iccDescription(const uint8_t *icc, size_t n)
  {
    if (!icc || n < 132 || std::memcmp(icc + 36, "acsp", 4) != 0)
      return {};
    const uint32_t count = be32(icc + 128);
    if (count > 1000 || 132 + size_t{12} * count > n)
      return {};
    for (uint32_t i = 0; i < count; ++i)
    {
      const uint8_t *e = icc + 132 + size_t{12} * i;
      if (std::memcmp(e, "desc", 4) != 0)
        continue;
      const uint32_t off = be32(e + 4), size = be32(e + 8);
      if (off > n || size > n - off || size < 12)
        return {};
      const uint8_t *tag = icc + off;
      if (std::memcmp(tag, "desc", 4) == 0) // v2: ASCII count (with NUL), then the text
      {
        const uint32_t cnt = be32(tag + 8);
        if (cnt == 0 || cnt > size - 12)
          return {};
        return sanitizeName(std::string(reinterpret_cast<const char *>(tag + 12), cnt - 1));
      }
      if (std::memcmp(tag, "mluc", 4) == 0 && size >= 28) // v4: records of UTF-16BE text
      {
        const uint32_t recs = be32(tag + 8), recSize = be32(tag + 12);
        if (recs < 1 || recSize < 12)
          return {};
        const uint32_t len = be32(tag + 20), at = be32(tag + 24);
        if (at > size || len > size - at)
          return {};
        std::string s;
        for (uint32_t k = 0; k + 1 < len; k += 2)
        {
          const uint8_t hi = tag[at + k], lo = tag[at + k + 1];
          s.push_back(hi == 0 && lo >= 32 && lo < 127 ? static_cast<char>(lo) : '?');
        }
        return sanitizeName(s);
      }
      return {};
    }
    return {};
  }

  // The ICC profile of a JPEG, whole: its APP2 payloads (see exif::JpegMeta::icc) without their
  // "ICC_PROFILE\0" + sequence header (14 bytes each).
  inline std::vector<uint8_t> assembleIcc(const std::vector<std::vector<uint8_t>> &chunks)
  {
    std::vector<uint8_t> icc;
    for (const auto &c : chunks)
      if (c.size() > 14)
        icc.insert(icc.end(), c.begin() + 14, c.end());
    return icc;
  }

  // ---------------------------------------------------------------- describing
  struct ColorTag
  {
    std::string text; // for the Info pane; empty when the file could not be read
    bool srgb = true; // false: the file is tagged as something else (the editor still treats it as sRGB)
  };

  inline bool containsNoCase(const std::string &hay, const char *needle)
  {
    const std::string n = needle;
    return std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b)
                       { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != hay.end();
  }

  // An embedded profile called `name` (empty: unnamed).
  inline ColorTag describeProfile(const std::string &name)
  {
    ColorTag t;
    if (containsNoCase(name, "srgb"))
    {
      t.text = "sRGB (embedded profile)";
      return t;
    }
    t.srgb = false;
    t.text = (name.empty() ? std::string("unnamed profile") : name) + " (embedded profile, not sRGB)";
    return t;
  }

  inline std::string profileNameOf(const PngColor &c)
  {
    std::string raw;
    for (size_t i = 0; i < c.iccp.size() && c.iccp[i] != 0 && i < kProfileNameMax + 1; ++i)
      raw.push_back(static_cast<char>(c.iccp[i]));
    return sanitizeName(raw);
  }

  inline ColorTag describePng(const PngColor &c)
  {
    if (!c.iccp.empty())
      return describeProfile(profileNameOf(c));
    ColorTag t;
    if (!c.srgb.empty())
    {
      t.text = "sRGB";
      return t;
    }
    if (!c.gama.empty())
    {
      const double g = be32(c.gama.data()) / 100000.0; // the file's gamma, 0.45455 for sRGB-like
      char buf[64];
      if (g > 0.0 && std::abs(g - 0.45455) <= 0.005)
      {
        t.text = "gamma 2.2 (assumed sRGB)";
        return t;
      }
      std::snprintf(buf, sizeof buf, "gamma %.2f, not sRGB", g > 0.0 ? 1.0 / g : 0.0);
      t.srgb = false;
      t.text = buf;
      if (!c.chrm.empty())
        t.text += " (with primaries)";
      return t;
    }
    if (!c.chrm.empty())
    {
      t.srgb = false;
      t.text = "custom primaries, not sRGB";
      return t;
    }
    t.text = "untagged (assumed sRGB)";
    return t;
  }

  // What the file at `path` is tagged as: a PNG by its signature (not its extension), a JPEG by its
  // ICC profile. Anything else, or a picture with no tag, is "untagged (assumed sRGB)". An unreadable
  // file gives an empty text.
  inline ColorTag colorTagOf(const std::string &path)
  {
    ColorTag t;
    std::vector<uint8_t> head;
    if (!readFilePrefix(path, kInfoPrefixBytes, &head) || head.empty())
      return t;
    t.text = "untagged (assumed sRGB)";
    if (pngSignature(head.data(), head.size()))
    {
      PngColor c;
      if (readPngColor(head.data(), head.size(), &c))
        return describePng(c);
      return t;
    }
    if (isJpegPath(path))
    {
      exif::JpegMeta meta;
      if (exif::readJpegMeta(head.data(), head.size(), &meta) && !meta.icc.empty())
      {
        const std::vector<uint8_t> icc = assembleIcc(meta.icc);
        std::string name = iccDescription(icc.data(), icc.size());
        if (name.empty())
          name = "ICC profile"; // the same fallback colorOfSource uses
        return describeProfile(name);
      }
    }
    return t;
  }

  // The tags `sourcePath` (a PNG or a JPEG) has, as the chunks a PNG export gets: its own for a PNG, an
  // iCCP made from the profile for a JPEG. Empty (any() false) for an untagged or unreadable source.
  // `*text` gets the description for the status line.
  inline PngColor colorOfSource(const std::string &sourcePath, std::string *text)
  {
    PngColor c;
    std::vector<uint8_t> head;
    if (!readFilePrefix(sourcePath, kCarryPrefixBytes, &head) || head.empty())
      return c;
    if (pngSignature(head.data(), head.size()))
    {
      if (readPngColor(head.data(), head.size(), &c) && c.any() && text)
        *text = describePng(c).text;
      return c;
    }
    if (isJpegPath(sourcePath))
    {
      exif::JpegMeta meta;
      if (exif::readJpegMeta(head.data(), head.size(), &meta) && !meta.icc.empty())
      {
        const std::vector<uint8_t> icc = assembleIcc(meta.icc);
        if (icc.empty() || icc.size() > kMaxIccBytes)
          return c;
        std::string name = iccDescription(icc.data(), icc.size());
        if (name.empty())
          name = "ICC profile";
        c.iccp.assign(name.begin(), name.end());
        c.iccp.push_back(0); // end of the name
        c.iccp.push_back(0); // compression method 0 (deflate)
        const std::vector<uint8_t> z = zlibStored(icc.data(), icc.size());
        c.iccp.insert(c.iccp.end(), z.begin(), z.end());
        if (text)
          *text = describeProfile(name).text;
      }
    }
    return c;
  }

  // Writes the source's color tags into the PNG that was just exported at `pngPath`. Returns the
  // description of what was written ("Display P3 (embedded profile, not sRGB)"), or "" when the source
  // has no tag or the file could not be rewritten (the export itself stays valid either way).
  inline std::string carryColorToPng(const std::string &pngPath, const std::string &sourcePath)
  {
    std::string text;
    const PngColor c = colorOfSource(sourcePath, &text);
    if (!c.any() || !insertColorIntoFile(pngPath, c))
      return {};
    return text;
  }

} // namespace color_tag