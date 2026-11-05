// cases/color_tag.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace color_tag_cases
// so they cannot collide with other cases.
//
// color_tag_test.cpp - io/color_tag.hpp: reading a picture's color tags, describing them, and writing
// them into an exported PNG. Synthetic PNG / JPEG / ICC bytes in a temp folder; header only, no liteui.

#include "io/color_tag.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace color_tag_cases
{
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using namespace color_tag;

struct TempDir
{
  fs::path path;
  explicit TempDir(const char *tag)
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() / (std::string("literoom_ct_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string file(const std::string &name) const { return u8ToString(path / fsPath(name)); }
  size_t count() const
  {
    size_t n = 0;
    for (const auto &e : fs::directory_iterator(path))
    {
      (void)e;
      ++n;
    }
    return n;
  }
};

static Bytes slurp(const std::string &p)
{
  std::ifstream f(fsPath(p), std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
static void spit(const std::string &p, const Bytes &b)
{
  std::ofstream f(fsPath(p), std::ios::binary);
  f.write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}
static Bytes bytesOf(const char *s) { return Bytes(s, s + std::strlen(s)); }

// ---- synthetic PNG: signature, IHDR (1x1, 8-bit RGB), the given chunks, IDAT, IEND
struct Ch
{
  std::string type;
  Bytes data;
};
static Bytes appendChunk(Bytes b, const std::string &type, const Bytes &data)
{
  const Bytes c = makeChunk(type.c_str(), data.data(), data.size());
  b.insert(b.end(), c.begin(), c.end());
  return b;
}
static Bytes makePng(const std::vector<Ch> &before, size_t idatBytes = 20, const std::vector<Ch> &after = {})
{
  Bytes b{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes ihdr;
  putBe32(ihdr, 1);
  putBe32(ihdr, 1);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  b = appendChunk(b, "IHDR", ihdr);
  for (const Ch &c : before)
    b = appendChunk(b, c.type, c.data);
  Bytes idat(idatBytes);
  for (size_t i = 0; i < idat.size(); ++i)
    idat[i] = static_cast<uint8_t>(i * 7 + 1);
  b = appendChunk(b, "IDAT", idat);
  for (const Ch &c : after)
    b = appendChunk(b, c.type, c.data);
  return appendChunk(b, "IEND", {});
}
static std::vector<std::string> typesOf(const Bytes &png)
{
  std::vector<std::string> t;
  forEachPngChunk(png.data(), png.size(), [&](const uint8_t *type, const uint8_t *data, size_t len, size_t, bool)
                  {
    CHECK(crc32(type, len + 4) == be32(data + len)); // every chunk has a valid CRC
    t.emplace_back(reinterpret_cast<const char *>(type), 4);
    return true; });
  return t;
}

static Bytes iccpPayload(const std::string &name, const Bytes &profile)
{
  Bytes d(name.begin(), name.end());
  d.push_back(0);
  d.push_back(0);
  const Bytes z = zlibStored(profile.data(), profile.size());
  d.insert(d.end(), z.begin(), z.end());
  return d;
}
static Bytes gamaPayload(uint32_t v)
{
  Bytes d;
  putBe32(d, v);
  return d;
}

// ---- synthetic ICC profiles with a 'desc' tag (v2 text, v4 multi-localized)
static Bytes iccWithDesc(const std::string &name, bool v4)
{
  Bytes tag;
  if (!v4)
  {
    tag = bytesOf("desc");
    putBe32(tag, 0);
    putBe32(tag, static_cast<uint32_t>(name.size() + 1));
    tag.insert(tag.end(), name.begin(), name.end());
    tag.push_back(0);
    tag.resize(tag.size() + 20, 0); // the Unicode and Mac parts, unused
  }
  else
  {
    tag = bytesOf("mluc");
    putBe32(tag, 0);
    putBe32(tag, 1);  // one record
    putBe32(tag, 12); // record size
    tag.insert(tag.end(), {'e', 'n', 'U', 'S'});
    putBe32(tag, static_cast<uint32_t>(name.size() * 2));
    putBe32(tag, 28); // offset of the text from the tag start
    for (const char c : name)
    {
      tag.push_back(0);
      tag.push_back(static_cast<uint8_t>(c));
    }
  }
  Bytes icc(128, 0);
  std::memcpy(&icc[36], "acsp", 4);
  putBe32(icc, 1); // one tag
  icc.insert(icc.end(), {'d', 'e', 's', 'c'});
  putBe32(icc, 144);
  putBe32(icc, static_cast<uint32_t>(tag.size()));
  icc.insert(icc.end(), tag.begin(), tag.end());
  const uint32_t total = static_cast<uint32_t>(icc.size());
  icc[0] = static_cast<uint8_t>(total >> 24);
  icc[1] = static_cast<uint8_t>(total >> 16);
  icc[2] = static_cast<uint8_t>(total >> 8);
  icc[3] = static_cast<uint8_t>(total);
  return icc;
}

// A JPEG (SOI, ICC split over two APP2 segments, EOI).
static Bytes jpegWithIcc(const Bytes &icc)
{
  Bytes j{0xFF, 0xD8};
  const size_t half = icc.size() / 2;
  const Bytes parts[2] = {Bytes(icc.begin(), icc.begin() + static_cast<std::ptrdiff_t>(half)),
                          Bytes(icc.begin() + static_cast<std::ptrdiff_t>(half), icc.end())};
  for (int i = 0; i < 2; ++i)
  {
    Bytes payload = bytesOf("ICC_PROFILE");
    payload.push_back(0);
    payload.push_back(static_cast<uint8_t>(i + 1)); // sequence number
    payload.push_back(2);                           // chunk count
    payload.insert(payload.end(), parts[i].begin(), parts[i].end());
    const size_t len = payload.size() + 2;
    j.insert(j.end(), {0xFF, 0xE2, static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)});
    j.insert(j.end(), payload.begin(), payload.end());
  }
  j.push_back(0xFF);
  j.push_back(0xD9);
  return j;
}

// The stored zlib stream, read back (a tiny inflater for stored blocks only).
static bool inflateStored(const uint8_t *z, size_t n, Bytes *out)
{
  out->clear();
  if (n < 6 || z[0] != 0x78 || z[1] != 0x01)
    return false;
  size_t pos = 2;
  for (;;)
  {
    if (pos + 5 > n)
      return false;
    const bool last = (z[pos] & 1) != 0;
    if ((z[pos] >> 1) != 0)
      return false; // not a stored block
    const size_t len = z[pos + 1] | (size_t{z[pos + 2]} << 8);
    const size_t nlen = z[pos + 3] | (size_t{z[pos + 4]} << 8);
    if ((len ^ 0xFFFF) != nlen || pos + 5 + len > n)
      return false;
    out->insert(out->end(), z + pos + 5, z + pos + 5 + len);
    pos += 5 + len;
    if (last)
      break;
  }
  return pos + 4 == n && be32(z + pos) == adler32(out->data(), out->size());
}

// ------------------------------------------------------------------ tests
TEST_CASE("color_tag: checksums", "[color_tag]")
{
  const Bytes digits = bytesOf("123456789");
  CHECK(crc32(digits.data(), digits.size()) == 0xCBF43926u);
  const Bytes iend = bytesOf("IEND");
  CHECK(crc32(iend.data(), iend.size()) == 0xAE426082u);
  // incremental, like zlib
  CHECK(crc32(digits.data() + 4, 5, crc32(digits.data(), 4)) == 0xCBF43926u);
  const Bytes wiki = bytesOf("Wikipedia");
  CHECK(adler32(wiki.data(), wiki.size()) == 0x11E60398u);
  CHECK(adler32(nullptr, 0) == 1u);
  Bytes big(200000, 0xFF); // longer than the overflow-safe block
  CHECK(adler32(big.data(), big.size()) == 0x14D06057u); // the value zlib.adler32 gives for 200000 x 0xFF
}

TEST_CASE("color_tag: zlibStored", "[color_tag]")
{
  for (const size_t n : {size_t{0}, size_t{1}, size_t{1000}, size_t{65535}, size_t{65536}, size_t{200000}})
  {
    Bytes data(n);
    for (size_t i = 0; i < n; ++i)
      data[i] = static_cast<uint8_t>(i * 31 + 7);
    const Bytes z = zlibStored(data.data(), data.size());
    Bytes back;
    CHECK(inflateStored(z.data(), z.size(), &back));
    CHECK(back == data);
  }
}

TEST_CASE("color_tag: readPngColor", "[color_tag]")
{
  const Bytes profile = bytesOf("pretend this is a profile");
  const Bytes srgbOne{0};
  Bytes chrm(32, 1);
  const Bytes png = makePng({{"gAMA", gamaPayload(45455)}, {"cHRM", chrm}, {"sRGB", srgbOne}, {"iCCP", iccpPayload("Display P3", profile)}, {"tEXt", bytesOf("k\0v")}},
                            20, {{"gAMA", gamaPayload(100000)}});
  PngColor c;
  CHECK(readPngColor(png.data(), png.size(), &c));
  CHECK((c.any() && c.srgb == srgbOne && c.chrm == chrm && c.gama == gamaPayload(45455) && !c.iccp.empty()));
  CHECK(profileNameOf(c) == "Display P3");
  // The chunk AFTER the image data is ignored (the first gAMA wins, and nothing past IDAT is read).
  CHECK(be32(c.gama.data()) == 45455);

  // Wrong sizes and bad CRCs are skipped.
  Bytes bad = makePng({{"sRGB", Bytes{0, 0}}, {"gAMA", Bytes{1, 2, 3}}, {"cHRM", Bytes(31, 0)}});
  CHECK((readPngColor(bad.data(), bad.size(), &c) && !c.any()));
  Bytes corrupt = makePng({{"gAMA", gamaPayload(45455)}});
  corrupt[corrupt.size() - 1] ^= 0xFF; // damage IEND, harmless
  Bytes dmg = makePng({{"gAMA", gamaPayload(45455)}});
  // find the gAMA chunk's data and flip a bit: the CRC no longer matches
  forEachPngChunk(dmg.data(), dmg.size(), [&](const uint8_t *type, const uint8_t *data, size_t, size_t, bool)
                  {
    if (isType(type, "gAMA"))
      const_cast<uint8_t *>(data)[0] ^= 0x01;
    return true; });
  CHECK((readPngColor(dmg.data(), dmg.size(), &c) && !c.any()));

  // Not a PNG / truncated mid-chunk.
  const Bytes junk = bytesOf("hello world, not a png");
  CHECK((!readPngColor(junk.data(), junk.size(), &c) && !c.any()));
  CHECK(!readPngColor(nullptr, 0, &c));
  Bytes cut(png.begin(), png.begin() + 40); // inside the chunks
  CHECK(readPngColor(cut.data(), cut.size(), &c)); // a PNG, whatever fits
  // A huge length field must not run off the buffer.
  Bytes huge = png;
  huge[8] = 0x7F;
  huge[9] = 0xFF; // the IHDR length
  CHECK((readPngColor(huge.data(), huge.size(), &c) && !c.any()));
}

TEST_CASE("color_tag: describe", "[color_tag]")
{
  PngColor c;
  CHECK((describePng(c).text == "untagged (assumed sRGB)" && describePng(c).srgb));
  c.srgb = {0};
  CHECK((describePng(c).text == "sRGB" && describePng(c).srgb));
  c = PngColor{};
  c.gama = gamaPayload(45455);
  CHECK((describePng(c).text == "gamma 2.2 (assumed sRGB)" && describePng(c).srgb));
  c.gama = gamaPayload(100000); // linear
  CHECK((!describePng(c).srgb && describePng(c).text.find("gamma 1.00") == 0));
  c = PngColor{};
  c.chrm.assign(32, 1);
  CHECK(!describePng(c).srgb);
  c = PngColor{};
  c.iccp = iccpPayload("sRGB IEC61966-2.1", bytesOf("x"));
  CHECK((describePng(c).srgb && describePng(c).text == "sRGB (embedded profile)"));
  c.iccp = iccpPayload("Adobe RGB (1998)", bytesOf("x"));
  CHECK((!describePng(c).srgb && describePng(c).text == "Adobe RGB (1998) (embedded profile, not sRGB)"));
  c.iccp = iccpPayload("", bytesOf("x"));
  CHECK((!describePng(c).srgb && describePng(c).text.find("unnamed profile") == 0));
  // iCCP wins over everything else.
  c.srgb = {0};
  c.iccp = iccpPayload("ProPhoto RGB", bytesOf("x"));
  CHECK(!describePng(c).srgb);
}

TEST_CASE("color_tag: iccDescription", "[color_tag]")
{
  CHECK(iccDescription(iccWithDesc("Display P3", false).data(), iccWithDesc("Display P3", false).size()) == "Display P3");
  CHECK(iccDescription(iccWithDesc("Display P3", true).data(), iccWithDesc("Display P3", true).size()) == "Display P3");
  const Bytes v4 = iccWithDesc("sRGB v4 test", true);
  CHECK(iccDescription(v4.data(), v4.size()) == "sRGB v4 test");
  // Damage: every truncation and a few corruptions must be safe and give nothing or the name.
  const Bytes v2 = iccWithDesc("Adobe RGB (1998)", false);
  for (size_t n = 0; n < v2.size(); ++n)
  {
    const std::string s = iccDescription(v2.data(), n);
    CHECK((s.empty() || s == "Adobe RGB (1998)"));
  }
  Bytes bad = v2;
  bad[36] = 'x'; // no 'acsp'
  CHECK(iccDescription(bad.data(), bad.size()).empty());
  bad = v2;
  bad[128 + 3] = 200; // an absurd tag count
  CHECK(iccDescription(bad.data(), bad.size()).empty());
  bad = v2;
  bad[132 + 8] = 0x7F; // an absurd tag size
  CHECK(iccDescription(bad.data(), bad.size()).empty());
  CHECK(iccDescription(nullptr, 0).empty());
  // Control and non-ASCII bytes become '?', the name is trimmed and capped.
  CHECK(sanitizeName("  a\x01z\xC3  ") == "a?z?");
  CHECK(sanitizeName(std::string(200, 'x')).size() == kProfileNameMax);
  CHECK(sanitizeName("   ").empty());
}

TEST_CASE("color_tag: insertInMemory", "[color_tag]")
{
  const Bytes profile = bytesOf("profile bytes");
  PngColor c;
  c.iccp = iccpPayload("Display P3", profile);
  c.gama = gamaPayload(45455);
  c.srgb = {0}; // loses to iccp
  // The picture already had its own (different) tags and a tEXt chunk: the old tags go, tEXt stays.
  Bytes png = makePng({{"sRGB", Bytes{1}}, {"gAMA", gamaPayload(1)}, {"tEXt", bytesOf("k\0v")}}, 50);
  const Bytes before = png;
  CHECK(insertColor(&png, c));
  const std::vector<std::string> want = {"IHDR", "iCCP", "gAMA", "tEXt", "IDAT", "IEND"};
  CHECK(typesOf(png) == want);
  PngColor back;
  CHECK(readPngColor(png.data(), png.size(), &back));
  CHECK((back.iccp == c.iccp && back.gama == c.gama && back.srgb.empty() && back.chrm.empty()));
  // The image data is byte for byte the same (everything from the IDAT on).
  const Bytes tailBefore(before.end() - (12 + 50 + 12), before.end());
  CHECK(Bytes(png.end() - tailBefore.size(), png.end()) == tailBefore);
  // Inserting the same thing again changes nothing.
  Bytes again = png;
  CHECK((insertColor(&again, c) && again == png));
  // sRGB alone, then no tags at all (strips what was there).
  PngColor s;
  s.srgb = {0};
  CHECK(insertColor(&png, s));
  CHECK((typesOf(png) == std::vector<std::string>{"IHDR", "sRGB", "tEXt", "IDAT", "IEND"}));
  CHECK(insertColor(&png, PngColor{}));
  CHECK((typesOf(png) == std::vector<std::string>{"IHDR", "tEXt", "IDAT", "IEND"}));
  // Not a PNG, no IHDR, no IDAT: untouched, false.
  Bytes junk = bytesOf("not a png at all, really not");
  const Bytes junkCopy = junk;
  CHECK((!insertColor(&junk, c) && junk == junkCopy));
  Bytes noIdat{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  noIdat = appendChunk(noIdat, "IEND", {});
  const Bytes noIdatCopy = noIdat;
  CHECK((!insertColor(&noIdat, c) && noIdat == noIdatCopy));
}

TEST_CASE("color_tag: insertIntoFile", "[color_tag]")
{
  TempDir d("insert");
  PngColor c;
  c.iccp = iccpPayload("Adobe RGB (1998)", Bytes(3000, 0x5A));
  c.gama = gamaPayload(45455);
  // A small file and one whose image data is bigger than the block the copy uses (3 MB).
  for (const size_t idat : {size_t{20}, size_t{3} << 20})
  {
    const std::string p = d.file("pic.png");
    const Bytes png = makePng({{"tEXt", bytesOf("k\0v")}}, idat);
    spit(p, png);
    CHECK(insertColorIntoFile(p, c));
    Bytes expect = png;
    CHECK(insertColor(&expect, c));
    CHECK(slurp(p) == expect);
    CHECK(d.count() == 1); // the temporary file is gone
  }
  // Failures leave the file alone and clean up: missing file, not a PNG.
  CHECK(!insertColorIntoFile(d.file("missing.png"), c));
  const std::string junk = d.file("junk.png");
  spit(junk, bytesOf("this is not a png"));
  CHECK(!insertColorIntoFile(junk, c));
  CHECK(slurp(junk) == bytesOf("this is not a png"));
  CHECK(d.count() == 2);
}

TEST_CASE("color_tag: colorTagOf", "[color_tag]")
{
  TempDir d("tagof");
  // PNG variants (found by signature, whatever the extension says).
  spit(d.file("none.png"), makePng({}));
  spit(d.file("srgb.dat"), makePng({{"sRGB", Bytes{0}}}));
  spit(d.file("p3.png"), makePng({{"iCCP", iccpPayload("Display P3", bytesOf("x"))}}));
  spit(d.file("lin.png"), makePng({{"gAMA", gamaPayload(100000)}}));
  CHECK(colorTagOf(d.file("none.png")).text == "untagged (assumed sRGB)");
  CHECK(colorTagOf(d.file("srgb.dat")).text == "sRGB");
  CHECK((!colorTagOf(d.file("p3.png")).srgb && colorTagOf(d.file("p3.png")).text.find("Display P3") == 0));
  CHECK(!colorTagOf(d.file("lin.png")).srgb);
  // JPEG with and without a profile.
  const Bytes p3 = iccWithDesc("Display P3", false);
  spit(d.file("p3.jpg"), jpegWithIcc(p3));
  spit(d.file("plain.jpg"), Bytes{0xFF, 0xD8, 0xFF, 0xD9});
  CHECK(colorTagOf(d.file("p3.jpg")).text == "Display P3 (embedded profile, not sRGB)");
  CHECK(colorTagOf(d.file("plain.jpg")).text == "untagged (assumed sRGB)");
  // Anything else: untagged; unreadable: empty.
  spit(d.file("x.bmp"), bytesOf("BM......"));
  CHECK(colorTagOf(d.file("x.bmp")).text == "untagged (assumed sRGB)");
  CHECK(colorTagOf(d.file("missing.png")).text.empty());
}

TEST_CASE("color_tag: carryColorToPng", "[color_tag]")
{
  TempDir d("carry");
  const Bytes exported = makePng({}, 100); // what an encoder wrote: untagged

  // PNG source -> its chunks are copied verbatim (as payloads).
  const Bytes profile = bytesOf("a profile");
  spit(d.file("src.png"), makePng({{"iCCP", iccpPayload("ProPhoto RGB", profile)}, {"gAMA", gamaPayload(45455)}}));
  spit(d.file("out1.png"), exported);
  CHECK(carryColorToPng(d.file("out1.png"), d.file("src.png")) == "ProPhoto RGB (embedded profile, not sRGB)");
  PngColor in, out;
  const Bytes srcBytes = slurp(d.file("src.png")), outBytes = slurp(d.file("out1.png"));
  CHECK((readPngColor(srcBytes.data(), srcBytes.size(), &in) && readPngColor(outBytes.data(), outBytes.size(), &out)));
  CHECK((in.iccp == out.iccp && in.gama == out.gama));
  CHECK((typesOf(outBytes) == std::vector<std::string>{"IHDR", "iCCP", "gAMA", "IDAT", "IEND"}));

  // JPEG source with a profile -> an iCCP whose profile decodes back to the very same bytes.
  const Bytes icc = iccWithDesc("Display P3", true);
  spit(d.file("src.jpg"), jpegWithIcc(icc));
  spit(d.file("out2.png"), exported);
  CHECK(carryColorToPng(d.file("out2.png"), d.file("src.jpg")) == "Display P3 (embedded profile, not sRGB)");
  const Bytes o2 = slurp(d.file("out2.png"));
  CHECK((readPngColor(o2.data(), o2.size(), &out) && profileNameOf(out) == "Display P3"));
  const size_t nameLen = std::strlen("Display P3");
  CHECK((out.iccp[nameLen] == 0 && out.iccp[nameLen + 1] == 0)); // end of name, method 0
  Bytes decoded;
  CHECK(inflateStored(out.iccp.data() + nameLen + 2, out.iccp.size() - nameLen - 2, &decoded));
  CHECK(decoded == icc);

  // Untagged sources, and sources of other kinds or missing: nothing is written, nothing invented.
  spit(d.file("plain.png"), makePng({}));
  spit(d.file("plain.jpg"), Bytes{0xFF, 0xD8, 0xFF, 0xD9});
  for (const char *src : {"plain.png", "plain.jpg", "missing.png"})
  {
    spit(d.file("out3.png"), exported);
    CHECK(carryColorToPng(d.file("out3.png"), d.file(src)).empty());
    CHECK(slurp(d.file("out3.png")) == exported);
  }
  // The export can't be rewritten (not a PNG): empty result, file untouched.
  spit(d.file("notpng.png"), bytesOf("garbage"));
  CHECK(carryColorToPng(d.file("notpng.png"), d.file("src.png")).empty());
  CHECK(slurp(d.file("notpng.png")) == bytesOf("garbage"));
}
} // namespace color_tag_cases
