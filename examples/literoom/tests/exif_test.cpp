// Build: g++ -std=c++20 -I.. exif_test.cpp -o exif_test && ./exif_test
#include "io/exif.hpp"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

static int failures = 0;
#define CHECK(c)                                             \
    do                                                       \
    {                                                        \
        if (!(c))                                            \
        {                                                    \
            std::printf("FAIL line %d: %s\n", __LINE__, #c); \
            ++failures;                                      \
        }                                                    \
    } while (0)

using Bytes = std::vector<uint8_t>;

struct W
{ // endian-aware byte writer
    Bytes b;
    bool le;
    void u8(int v) { b.push_back(static_cast<uint8_t>(v)); }
    void u16(int v) { le ? (u8(v), u8(v >> 8)) : (u8(v >> 8), u8(v)); }
    void u32(uint32_t v)
    {
        if (le)
        {
            u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24);
        }
        else
        {
            u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v);
        }
    }
};

// A TIFF block: IFD0 {orientation, ExifIFD ptr, Make}, next IFD -> a fake
// thumbnail IFD; ExifIFD {PixelXDimension SHORT, PixelYDimension SHORT}.
static Bytes tiff(bool le, int orientation)
{
    W w{{}, le};
    w.u8(le ? 'I' : 'M');
    w.u8(le ? 'I' : 'M');
    w.u16(42);
    w.u32(8);
    // IFD0 at 8: 3 entries -> ends at 8+2+36+4 = 50
    w.u16(3);
    w.u16(0x0112);
    w.u16(3);
    w.u32(1);
    w.u16(orientation);
    w.u16(0);
    w.u16(0x8769);
    w.u16(4);
    w.u32(1);
    w.u32(70); // sub-IFD at 70
    w.u16(0x010F);
    w.u16(2);
    w.u32(4);
    w.u8('C');
    w.u8('a');
    w.u8('m');
    w.u8(0);
    w.u32(60); // next IFD (thumbnail) at 60
    while (w.b.size() < 60)
        w.u8(0);
    w.u16(0);
    w.u32(0); // empty IFD1 at 60 (6 bytes)
    while (w.b.size() < 70)
        w.u8(0);
    w.u16(2);
    w.u16(0xA002);
    w.u16(3);
    w.u32(1);
    w.u16(111);
    w.u16(0);
    w.u16(0xA003);
    w.u16(3);
    w.u32(1);
    w.u16(222);
    w.u16(0);
    w.u32(0);
    return w.b;
}

static Bytes segment(int marker, const Bytes &payload)
{
    Bytes s{0xFF, static_cast<uint8_t>(marker), static_cast<uint8_t>((payload.size() + 2) >> 8),
            static_cast<uint8_t>(payload.size() + 2)};
    s.insert(s.end(), payload.begin(), payload.end());
    return s;
}
static Bytes withPrefix(const char *tag, size_t n, const Bytes &rest)
{
    Bytes p(tag, tag + n);
    p.insert(p.end(), rest.begin(), rest.end());
    return p;
}
static Bytes jpeg(const Bytes &app1Tiff, bool exifFirst, bool icc)
{
    Bytes j{0xFF, 0xD8};
    auto add = [&j](const Bytes &s)
    { j.insert(j.end(), s.begin(), s.end()); };
    add(segment(0xE0, withPrefix("JFIF\0", 5, {1, 1, 0, 0, 1, 0, 1, 0, 0})));
    if (exifFirst && !app1Tiff.empty())
        add(segment(0xE1, withPrefix("Exif\0\0", 6, app1Tiff)));
    if (icc)
        add(segment(0xE2, withPrefix("ICC_PROFILE\0", 12, {1, 1, 'p', 'r', 'o', 'f'})));
    add(segment(0xDB, Bytes(65, 8)));
    add({0xFF, 0xDA, 0x00, 0x02, 0x11, 0x22, 0x33});
    add({0xFF, 0xD9});
    return j;
}

// ---- GPS removal (Export: "Remove location") ----
constexpr uint8_t kGpsByte = 0xCD; // fills the GPS latitude data, so any leftover shows

// A TIFF block with a GPS IFD: IFD0 {Make, Orientation, ExifIFD, GPSInfo}, the
// Exif sub-IFD {PixelXDimension 111, PixelYDimension 222}, and at 100 the GPS
// IFD {VersionID, LatitudeRef, Latitude}, whose RATIONAL latitude sits outside
// the entry at 160 (24 bytes of kGpsByte). `gpsPtr` / `gpsType` let a test aim
// the GPSInfo tag somewhere bad.
static Bytes tiffGps(bool le, uint32_t gpsPtr = 100, int gpsType = 4)
{
    W w{{}, le};
    auto entry = [&w](int tag, int type, uint32_t cnt, uint32_t val)
    { w.u16(tag); w.u16(type); w.u32(cnt); w.u32(val); };
    w.u8(le ? 'I' : 'M');
    w.u8(le ? 'I' : 'M');
    w.u16(42);
    w.u32(8);
    w.u16(4); // IFD0 at 8: 2 + 4 * 12 + 4 = 54 -> ends at 62
    entry(0x010F, 2, 4, 0x43616D00);
    w.u16(0x0112);
    w.u16(3);
    w.u32(1);
    w.u16(6);
    w.u16(0);
    entry(0x8769, 4, 1, 70);
    entry(0x8825, gpsType, 1, gpsPtr);
    w.u32(0); // no IFD1
    while (w.b.size() < 70)
        w.u8(0);
    w.u16(2); // Exif sub-IFD at 70, ends at 100
    entry(0xA002, 4, 1, 111);
    entry(0xA003, 4, 1, 222);
    w.u32(0);
    w.u16(3); // GPS IFD at 100, ends at 142
    entry(0x0000, 1, 4, 0x02020000);
    entry(0x0001, 2, 2, 0x4E000000);
    entry(0x0002, 5, 3, 160);
    w.u32(0);
    while (w.b.size() < 160)
        w.u8(0);
    for (int i = 0; i < 24; ++i)
        w.u8(kGpsByte);
    return w.b;
}

static bool hasGpsBytes(const Bytes &b) { return std::find(b.begin(), b.end(), kGpsByte) != b.end(); }

static void testStripGps()
{
    for (bool le : {true, false})
    {
        const Bytes src = jpeg(tiffGps(le), true, true);
        exif::JpegMeta m;
        CHECK(exif::readJpegMeta(src.data(), src.size(), &m));
        CHECK(m.orientation == 6 && hasGpsBytes(m.exif)); // carried over unless asked otherwise
        const Bytes before = m.exif;

        CHECK(exif::stripGps(&m));
        CHECK(!hasGpsBytes(m.exif));            // the coordinates are gone, not just unlinked
        CHECK(m.exif.size() == before.size());  // same size: only zeroed and shifted
        CHECK(m.icc.size() == 1);               // the rest of the metadata is untouched

        Bytes tb(m.exif.begin() + 6, m.exif.end());
        exif::detail::Tiff t{tb};
        uint32_t ifd0 = 0, nextIfd = 7, sub = 0, v = 0;
        uint64_t e = 0, end = 0;
        uint16_t cnt = 0, o = 0;
        CHECK(exif::detail::tiffHeader(t, &ifd0));
        CHECK(!exif::detail::findEntry(t, ifd0, 0x8825, &e));              // the tag is cut out
        CHECK(exif::detail::findEntry(t, ifd0, 0x0112, &e, &cnt, &end) && cnt == 3);
        CHECK(t.rd16(e + 8, &o) && o == 1);                                // still upright
        CHECK(t.rd32(end, &nextIfd) && nextIfd == 0);                      // next pointer moved with it
        CHECK(exif::detail::findEntry(t, ifd0, 0x010F, &e));               // Make survived
        CHECK(exif::detail::findEntry(t, ifd0, 0x8769, &e) && t.rd32(e + 8, &sub));
        CHECK(exif::detail::findEntry(t, sub, 0xA002, &e) && t.rd32(e + 8, &v) && v == 111);
        CHECK(exif::detail::findEntry(t, sub, 0xA003, &e) && t.rd32(e + 8, &v) && v == 222);
        for (size_t i = 100; i < 184; ++i) // the whole GPS IFD and its data
            CHECK(tb[i] == 0);

        CHECK(!exif::stripGps(&m)); // nothing left to remove

        // Through embed: still a good file, size tags set, no location.
        const Bytes out = exif::embed(jpeg({}, false, false), m, 640, 480);
        exif::JpegMeta m2;
        CHECK(exif::readJpegMeta(out.data(), out.size(), &m2));
        CHECK(m2.orientation == 1 && !m2.exif.empty() && !hasGpsBytes(m2.exif) && m2.icc.size() == 1);
    }

    // No GPS block, no EXIF at all, or garbage: nothing happens.
    {
        exif::JpegMeta m;
        const Bytes plain = jpeg(tiff(true, 6), true, false);
        CHECK(exif::readJpegMeta(plain.data(), plain.size(), &m));
        const Bytes same = m.exif;
        CHECK(!exif::stripGps(&m) && m.exif == same);
        exif::JpegMeta none;
        CHECK(!exif::stripGps(&none) && none.exif.empty());
        exif::JpegMeta junk;
        junk.exif = Bytes(40, 0x55);
        CHECK(!exif::stripGps(&junk) && junk.exif == Bytes(40, 0x55));
    }

    // A GPSInfo tag that points nowhere, or has the wrong type, is still removed,
    // and nothing is read or written out of range.
    for (bool le : {true, false})
        for (uint32_t ptr : {0u, 4u, 0xFFFFFFF0u, 70u, 5000u})
            for (int type : {4, 3, 2})
            {
                const Bytes src = jpeg(tiffGps(le, ptr, type), true, false);
                exif::JpegMeta m;
                CHECK(exif::readJpegMeta(src.data(), src.size(), &m));
                CHECK(exif::stripGps(&m));
                Bytes tb(m.exif.begin() + 6, m.exif.end());
                exif::detail::Tiff t{tb};
                uint32_t ifd0 = 0;
                uint64_t e = 0;
                CHECK(exif::detail::tiffHeader(t, &ifd0) && !exif::detail::findEntry(t, ifd0, 0x8825, &e));
            }
}

// ---- Photo info (the Info panel) ----
// One IFD entry: `raw` holds the value bytes in the file's byte order. Up to 4
// bytes stay inside the entry; longer values go to the data area.
struct Ent
{
    int tag, type;
    uint32_t count;
    Bytes raw;
};
static Bytes rawU16(bool le, std::initializer_list<int> v)
{
    W w{{}, le};
    for (int x : v)
        w.u16(x);
    return w.b;
}
static Bytes rawU32(bool le, std::initializer_list<uint32_t> v)
{
    W w{{}, le};
    for (uint32_t x : v)
        w.u32(x);
    return w.b;
}
static Ent eText(int tag, const std::string &s)
{
    Bytes r(s.begin(), s.end());
    r.push_back(0);
    return {tag, 2, static_cast<uint32_t>(r.size()), r};
}
static Ent eRat(bool le, int tag, uint32_t n, uint32_t d) { return {tag, 5, 1, rawU32(le, {n, d})}; }
static Ent eShort(bool le, int tag, int v) { return {tag, 3, 1, rawU16(le, {v})}; }
static Ent eLong(bool le, int tag, uint32_t v) { return {tag, 4, 1, rawU32(le, {v})}; }

// TIFF block: IFD0 {ifd0 entries, then the ExifIFD pointer unless !withSub}, the
// Exif sub-IFD {sub entries}, then the data area. `subPtr` >= 0 aims the ExifIFD
// pointer somewhere else (a bad offset).
static Bytes tiffInfo(bool le, const std::vector<Ent> &ifd0, const std::vector<Ent> &sub,
                      bool withSub = true, int64_t subPtr = -1)
{
    W w{{}, le};
    const uint32_t n0 = static_cast<uint32_t>(ifd0.size()) + (withSub ? 1 : 0);
    const uint32_t subOff = 8 + 2 + n0 * 12 + 4;
    const uint32_t dataOff = subOff + 2 + static_cast<uint32_t>(sub.size()) * 12 + 4;
    Bytes data;
    auto emit = [&](const Ent &e)
    {
        w.u16(e.tag);
        w.u16(e.type);
        w.u32(e.count);
        if (e.raw.size() <= 4)
            for (size_t i = 0; i < 4; ++i)
                w.u8(i < e.raw.size() ? e.raw[i] : 0);
        else
        {
            w.u32(dataOff + static_cast<uint32_t>(data.size()));
            data.insert(data.end(), e.raw.begin(), e.raw.end());
            if (data.size() & 1)
                data.push_back(0);
        }
    };
    w.u8(le ? 'I' : 'M');
    w.u8(le ? 'I' : 'M');
    w.u16(42);
    w.u32(8);
    w.u16(static_cast<int>(n0));
    for (const Ent &e : ifd0)
        emit(e);
    if (withSub)
    {
        w.u16(0x8769);
        w.u16(4);
        w.u32(1);
        w.u32(subPtr >= 0 ? static_cast<uint32_t>(subPtr) : subOff);
    }
    w.u32(0);
    w.u16(static_cast<int>(sub.size()));
    for (const Ent &e : sub)
        emit(e);
    w.u32(0);
    w.b.insert(w.b.end(), data.begin(), data.end());
    return w.b;
}

static bool closeTo(double a, double b) { return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b)); }

static exif::PhotoInfo infoOf(const Bytes &tiffBlock)
{
    const Bytes j = jpeg(tiffBlock, true, false);
    exif::PhotoInfo p;
    CHECK(exif::readPhotoInfo(j.data(), j.size(), &p));
    return p;
}
// A one-entry Make / sub-IFD helper for the single-field cases below.
static exif::PhotoInfo infoSub(const std::vector<Ent> &sub, bool le = true)
{
    return infoOf(tiffInfo(le, {eText(0x010F, "Cam")}, sub));
}
static bool printable(const std::string &s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

static std::vector<Ent> fullSub(bool le)
{
    return {eRat(le, 0x829A, 1, 250), eRat(le, 0x829D, 28, 10), eShort(le, 0x8827, 400),
            eText(0x9003, "2024:06:15 14:30:05"), eRat(le, 0x920A, 85, 1),
            eText(0xA434, "RF85mm F1.2 L USM")};
}

static void testPhotoInfo()
{
    // Everything, both byte orders (ASCII longer than 4 bytes, RATIONALs, an inline SHORT).
    for (bool le : {true, false})
    {
        const exif::PhotoInfo p =
            infoOf(tiffInfo(le, {eText(0x010F, "Canon"), eText(0x0110, "Canon EOS R5")}, fullSub(le)));
        CHECK(p.make == "Canon" && p.model == "Canon EOS R5" && p.lens == "RF85mm F1.2 L USM");
        CHECK(p.iso == 400 && closeTo(p.exposureTime, 1.0 / 250) && closeTo(p.fNumber, 2.8) && closeTo(p.focalLength, 85));
        CHECK(p.dateTaken == "2024-06-15 14:30:05" && !p.empty());
        CHECK(exif::describeShutter(p.exposureTime) == "1/250 s" && exif::describeAperture(p.fNumber) == "f/2.8");
        CHECK(exif::describeFocalLength(p.focalLength) == "85 mm" && exif::describeIso(p.iso) == "ISO 400");
        CHECK(exif::describeExposure(p) == "1/250 s, f/2.8, ISO 400" && exif::describeCamera(p) == "Canon EOS R5");
    }

    // Reading never changes anything about orientation or the thumbnail handling
    // of the other readers, and the old fixture (Make "Cam" in 4 bytes, inline) works.
    {
        const Bytes j = jpeg(tiff(true, 6), true, true);
        exif::PhotoInfo p;
        exif::JpegMeta m;
        CHECK(exif::readPhotoInfo(j.data(), j.size(), &p) && p.make == "Cam" && p.model.empty() && p.lens.empty());
        CHECK(p.iso == 0 && p.exposureTime == 0.0 && p.dateTaken.empty());
        CHECK(exif::readJpegMeta(j.data(), j.size(), &m) && m.orientation == 6);
    }

    // No EXIF, not a JPEG, too short: only "not a JPEG" is false, and `out` is always reset.
    {
        exif::PhotoInfo p;
        p.make = "stale";
        p.iso = 100;
        const Bytes plain = jpeg({}, false, false);
        CHECK(exif::readPhotoInfo(plain.data(), plain.size(), &p) && p.empty());
        p.make = "stale";
        const Bytes png{0x89, 'P', 'N', 'G', 0, 0, 0, 0};
        CHECK(!exif::readPhotoInfo(png.data(), png.size(), &p) && p.empty());
        p.iso = 100;
        CHECK(!exif::readPhotoInfo(nullptr, 0, &p) && p.empty());
        const Bytes tiny{0xFF, 0xD8};
        CHECK(!exif::readPhotoInfo(tiny.data(), tiny.size(), &p));
        const Bytes noTiff = jpeg({}, false, false); // EXIF segment with a bad TIFF header
        Bytes junk(40, 0x55);
        const Bytes bad = jpeg(junk, true, false);
        CHECK(exif::readPhotoInfo(bad.data(), bad.size(), &p) && p.empty());
        (void)noTiff;
    }

    // No Exif sub-IFD, or a pointer to it that leads nowhere: Make and Model survive.
    for (bool le : {true, false})
    {
        exif::PhotoInfo p = infoOf(tiffInfo(le, {eText(0x010F, "Canon"), eText(0x0110, "R5")}, fullSub(le), false));
        CHECK(p.make == "Canon" && p.model == "R5" && p.lens.empty() && p.iso == 0 && p.exposureTime == 0.0);
        for (int64_t ptr : {0, 4, 7, 5000, 0x7FFFFFF0})
        {
            p = infoOf(tiffInfo(le, {eText(0x010F, "Canon")}, fullSub(le), true, ptr));
            CHECK(p.make == "Canon" && p.lens.empty() && p.iso == 0 && p.fNumber == 0.0 && p.dateTaken.empty());
        }
        // A pointer of the wrong type is ignored too.
        std::vector<Ent> ifd0 = {eText(0x010F, "Canon"), eShort(le, 0x8769, 50)};
        p = infoOf(tiffInfo(le, ifd0, fullSub(le), false));
        CHECK(p.make == "Canon" && p.iso == 0);
    }

    // Text: padding, control characters, non-ASCII, embedded NULs, length cap, wrong type, lies about the count.
    {
        auto make = [](const std::string &s)
        { return infoOf(tiffInfo(true, {eText(0x010F, s)}, {})).make; };
        CHECK(make("  Nikon   Corp \t ") == "Nikon Corp");
        CHECK(make("NIKON CORPORATION        ") == "NIKON CORPORATION"); // the usual space padding
        CHECK(make("A\x01" "B") == "AB");
        CHECK(make("Caf\xC3\xA9") == "Caf??");
        CHECK(make("a\nb\r\nc") == "a b c");
        CHECK(make("     ").empty() && make("").empty());
        CHECK(make(std::string("Ab\0cd", 5)) == "Ab"); // reading stops at the first NUL
        const std::string big = make(std::string(300, 'x'));
        CHECK(big.size() == exif::kInfoMaxText && big == std::string(exif::kInfoMaxText, 'x'));
        const std::string spaced = make(std::string(200, 'a') + " " + std::string(200, 'b'));
        CHECK(spaced.size() <= exif::kInfoMaxText && printable(spaced) && spaced.back() != ' ');
        // A SHORT where text should be, and an ASCII count far beyond the file: ignored.
        CHECK(infoOf(tiffInfo(true, {Ent{0x010F, 3, 1, rawU16(true, {5})}}, {})).make.empty());
        CHECK(infoOf(tiffInfo(true, {Ent{0x010F, 2, 0x7FFFFFFFu, Bytes{'a', 'b', 'c', 'd', 'e', 'f'}}}, {})).make.empty());
        CHECK(infoOf(tiffInfo(true, {Ent{0x010F, 2, 0, Bytes{}}}, {})).make.empty());
    }

    // Rational fields: zero denominator, absurd values, wrong type.
    {
        CHECK(infoSub({eRat(true, 0x829A, 1, 0)}).exposureTime == 0.0);
        CHECK(infoSub({eRat(true, 0x829A, 0, 1)}).exposureTime == 0.0);
        CHECK(infoSub({eRat(true, 0x829A, 100000, 1)}).exposureTime == 0.0); // over a day
        CHECK(closeTo(infoSub({eRat(true, 0x829A, 30, 1)}).exposureTime, 30.0));
        CHECK(infoSub({eRat(true, 0x829D, 5000, 1)}).fNumber == 0.0);
        CHECK(closeTo(infoSub({eRat(true, 0x829D, 14, 10)}).fNumber, 1.4));
        CHECK(infoSub({eRat(true, 0x920A, 0, 1)}).focalLength == 0.0);
        CHECK(infoSub({eRat(true, 0x920A, 4000000000u, 1)}).focalLength == 0.0);
        CHECK(infoSub({eShort(true, 0x829A, 3)}).exposureTime == 0.0); // not a RATIONAL
        // Two values: the first counts.
        Ent two{0x829D, 5, 2, rawU32(true, {28, 10, 56, 10})};
        CHECK(closeTo(infoSub({two}).fNumber, 2.8));
        // A RATIONAL whose offset points past the end.
        Ent farRat{0x829D, 5, 1, rawU32(true, {1, 1})};
        const Bytes tb = tiffInfo(true, {eText(0x010F, "Cam")}, {farRat});
        Bytes cut(tb.begin(), tb.end() - 4); // the data area is too short now
        CHECK(infoOf(cut).fNumber == 0.0);
    }

    // ISO: SHORT, LONG, two values, the 65535 escape to ISOSpeed, and junk.
    for (bool le : {true, false})
    {
        CHECK(infoSub({eShort(le, 0x8827, 100)}, le).iso == 100);
        CHECK(infoSub({eLong(le, 0x8827, 800)}, le).iso == 800);
        CHECK(infoSub({Ent{0x8827, 3, 2, rawU16(le, {200, 300})}}, le).iso == 200);
        CHECK(infoSub({Ent{0x8827, 3, 3, rawU16(le, {640, 1, 2})}}, le).iso == 640); // offset form
        CHECK(infoSub({eShort(le, 0x8827, 65535), eLong(le, 0x8833, 102400)}, le).iso == 102400);
        CHECK(infoSub({eShort(le, 0x8827, 0), eLong(le, 0x8833, 3200)}, le).iso == 3200);
        CHECK(infoSub({eLong(le, 0x8833, 6400)}, le).iso == 6400);   // only ISOSpeed
        CHECK(infoSub({eShort(le, 0x8827, 65535)}, le).iso == 0);    // the escape with nothing behind it
        CHECK(infoSub({eShort(le, 0x8827, 0)}, le).iso == 0);
        CHECK(infoSub({eLong(le, 0x8827, 4000000000u)}, le).iso == 0);
        CHECK(infoSub({eLong(le, 0x8827, static_cast<uint32_t>(exif::kInfoMaxIso))}, le).iso == exif::kInfoMaxIso);
        CHECK(infoSub({eLong(le, 0x8827, static_cast<uint32_t>(exif::kInfoMaxIso) + 1)}, le).iso == 0);
        CHECK(infoSub({eRat(le, 0x8827, 100, 1)}, le).iso == 0);     // wrong type
    }

    // Date: DateTimeOriginal first, DateTimeDigitized as the fallback, real dates only.
    {
        auto d = [](const std::string &s)
        { return infoSub({eText(0x9003, s)}).dateTaken; };
        CHECK(d("2024:06:15 14:30:05") == "2024-06-15 14:30:05");
        CHECK(d("2024-06-15 14:30:05") == "2024-06-15 14:30:05");
        CHECK(d("2024:06:15 14:30:05.123") == "2024-06-15 14:30:05"); // extra characters ignored
        CHECK(d("0000:00:00 00:00:00").empty() && d("    :  :     :  :  ").empty());
        CHECK(d("2024:13:01 00:00:00").empty() && d("2024:00:10 00:00:00").empty());
        CHECK(d("2024:06:32 00:00:00").empty() && d("2024:06:15 24:00:00").empty());
        CHECK(d("2024:06:15 14:60:00").empty() && d("2024:06:15 14:30:60").empty());
        CHECK(d("2024:06:15").empty() && d("abcdefghijklmnopqrs").empty() && d("").empty());
        CHECK(d("2024/06/15 14:30:05").empty()); // not a form cameras write
        CHECK(infoSub({eText(0x9004, "2023:01:02 03:04:05")}).dateTaken == "2023-01-02 03:04:05");
        CHECK(infoSub({eText(0x9003, "2024:06:15 14:30:05"), eText(0x9004, "2023:01:02 03:04:05")}).dateTaken ==
              "2024-06-15 14:30:05");
        CHECK(infoSub({eText(0x9003, "0000:00:00 00:00:00"), eText(0x9004, "2023:01:02 03:04:05")}).dateTaken ==
              "2023-01-02 03:04:05"); // an unset clock in the first falls back to the second
    }

    // Display text.
    {
        CHECK(exif::describeShutter(1.0 / 250) == "1/250 s" && exif::describeShutter(1.0 / 8000) == "1/8000 s");
        CHECK(exif::describeShutter(1.0 / 60) == "1/60 s" && exif::describeShutter(0.5) == "1/2 s");
        CHECK(exif::describeShutter(0.4) == "0.4 s" && exif::describeShutter(0.3) == "0.3 s");
        CHECK(exif::describeShutter(0.0133) == "1/75 s");
        CHECK(exif::describeShutter(1.0) == "1 s" && exif::describeShutter(1.3) == "1.3 s");
        CHECK(exif::describeShutter(2.5) == "2.5 s" && exif::describeShutter(30.0) == "30 s");
        CHECK(exif::describeShutter(0.0).empty() && exif::describeShutter(-1.0).empty());
        CHECK(exif::describeShutter(std::nan("")).empty() && exif::describeShutter(1e-9).empty());
        CHECK(exif::describeShutter(HUGE_VAL).empty());
        CHECK(exif::describeAperture(2.8) == "f/2.8" && exif::describeAperture(11.0) == "f/11");
        CHECK(exif::describeAperture(1.0) == "f/1" && exif::describeAperture(0.0).empty());
        CHECK(exif::describeFocalLength(85.0) == "85 mm" && exif::describeFocalLength(24.5) == "24.5 mm");
        CHECK(exif::describeFocalLength(0.0).empty() && exif::describeFocalLength(std::nan("")).empty());
        CHECK(exif::describeIso(0).empty() && exif::describeIso(-5).empty() && exif::describeIso(12800) == "ISO 12800");

        exif::PhotoInfo p;
        CHECK(exif::describeExposure(p).empty());
        p.iso = 200;
        CHECK(exif::describeExposure(p) == "ISO 200");
        p.fNumber = 4.0;
        CHECK(exif::describeExposure(p) == "f/4, ISO 200");
        p.exposureTime = 1.0 / 100;
        CHECK(exif::describeExposure(p) == "1/100 s, f/4, ISO 200");

        auto cam = [](const char *make, const char *model)
        {
            exif::PhotoInfo q;
            q.make = make;
            q.model = model;
            return exif::describeCamera(q);
        };
        CHECK(cam("Canon", "Canon EOS R5") == "Canon EOS R5");
        CHECK(cam("NIKON CORPORATION", "NIKON D850") == "NIKON D850"); // first word of the make
        CHECK(cam("canon", "CANON EOS 5D") == "CANON EOS 5D");          // case does not matter
        CHECK(cam("SONY", "ILCE-7M3") == "SONY ILCE-7M3");
        CHECK(cam("Apple", "iPhone 15 Pro") == "Apple iPhone 15 Pro");
        CHECK(cam("", "EOS R5") == "EOS R5" && cam("Canon", "") == "Canon" && cam("", "").empty());
        CHECK(cam("Leica Camera AG", "Q") == "Leica Camera AG Q"); // a model shorter than the first word
    }
}

// Damaged headers never crash the info reader, and what it returns is always
// something the UI can show.
static void testPhotoInfoFuzz()
{
    std::mt19937 rng(23);
    const Bytes base = jpeg(tiffInfo(true, {eText(0x010F, "Canon"), eText(0x0110, "Canon EOS R5")}, fullSub(true)), true, true);
    const Bytes baseBe = jpeg(tiffInfo(false, {eText(0x010F, "Canon"), eText(0x0110, "Canon EOS R5")}, fullSub(false)), true, true);
    for (int it = 0; it < 100000; ++it)
    {
        Bytes b = (it & 1) ? baseBe : base;
        for (int k = 1 + static_cast<int>(rng() % 5); k > 0; --k)
            b[rng() % b.size()] = static_cast<uint8_t>(rng());
        if (rng() % 4 == 0)
            b.resize(rng() % b.size());
        exif::PhotoInfo p;
        if (!exif::readPhotoInfo(b.data(), b.size(), &p))
            continue;
        CHECK(printable(p.make) && printable(p.model) && printable(p.lens));
        CHECK(p.make.size() <= exif::kInfoMaxText && p.model.size() <= exif::kInfoMaxText && p.lens.size() <= exif::kInfoMaxText);
        CHECK(p.iso >= 0 && p.iso <= exif::kInfoMaxIso);
        CHECK(p.exposureTime == 0.0 || (p.exposureTime > 0.0 && p.exposureTime <= 86400.0));
        CHECK(p.fNumber == 0.0 || (p.fNumber > 0.0 && p.fNumber <= 1000.0));
        CHECK(p.focalLength == 0.0 || (p.focalLength > 0.0 && p.focalLength <= 10000.0));
        CHECK(p.dateTaken.empty() || (p.dateTaken.size() == 19 && printable(p.dateTaken) && p.dateTaken[4] == '-'));
        const std::string text = exif::describeCamera(p) + exif::describeExposure(p) +
                                 exif::describeFocalLength(p.focalLength);
        CHECK(printable(text));
    }
    // Any numbers at all go through the formatters without a crash.
    for (int it = 0; it < 20000; ++it)
    {
        double v;
        uint64_t bits = (static_cast<uint64_t>(rng()) << 32) | rng();
        std::memcpy(&v, &bits, sizeof v);
        CHECK(printable(exif::describeShutter(v)) && printable(exif::describeAperture(v)) &&
              printable(exif::describeFocalLength(v)));
        CHECK(printable(exif::describeIso(static_cast<int>(rng()))));
    }
}

int main()
{
    testStripGps();
    testPhotoInfo();
    testPhotoInfoFuzz();
    for (bool le : {true, false})
        for (int o = 1; o <= 8; ++o)
        {
            const Bytes src = jpeg(tiff(le, o), true, true);
            exif::JpegMeta m;
            CHECK(exif::readJpegMeta(src.data(), src.size(), &m));
            CHECK(m.orientation == o);
            CHECK(!m.exif.empty());
            CHECK(m.icc.size() == 1);

            // Re-embed into a plain JPEG as a 640x480 picture.
            const Bytes plain = jpeg({}, false, false);
            const Bytes out = exif::embed(plain, m, 640, 480);
            exif::JpegMeta m2;
            CHECK(exif::readJpegMeta(out.data(), out.size(), &m2));
            CHECK(m2.orientation == 1); // reset: the pixels are upright
            CHECK(m2.icc.size() == 1 && m2.icc[0] == m.icc[0]);
            CHECK(out.size() > plain.size());
            // Thumbnail IFD unlinked:
            Bytes tiffOnly(m2.exif.begin() + 6, m2.exif.end());
            exif::detail::Tiff tt{tiffOnly};
            uint32_t ifd0, sub, v;
            uint64_t e, end;
            uint16_t cnt;
            CHECK(exif::detail::tiffHeader(tt, &ifd0));
            CHECK(exif::detail::findEntry(tt, ifd0, 0x0112, &e, &cnt, &end));
            CHECK(tt.rd32(end, &v) && v == 0); // next IFD removed
            CHECK(exif::detail::findEntry(tt, ifd0, 0x8769, &e) && tt.rd32(e + 8, &sub));
            // The size tags are patched by embed() itself: read them from its output.
            std::vector<uint8_t> tl;
            for (size_t i = 2; i + 4 < out.size();)
            {
                if (out[i] == 0xFF && out[i + 1] == 0xE1)
                {
                    const size_t len = (out[i + 2] << 8) | out[i + 3];
                    tl.assign(out.begin() + i + 4 + 6, out.begin() + i + 2 + len);
                    break;
                }
                i += 2 + ((out[i + 2] << 8) | out[i + 3]);
            }
            exif::detail::Tiff tp{tl};
            CHECK(exif::detail::tiffHeader(tp, &ifd0));
            CHECK(exif::detail::findEntry(tp, ifd0, 0x8769, &e) && tp.rd32(e + 8, &sub));
            uint32_t px = 0, py = 0;
            CHECK(exif::detail::findEntry(tp, sub, 0xA002, &e) && tp.rd32(e + 8, &px));
            CHECK(exif::detail::findEntry(tp, sub, 0xA003, &e) && tp.rd32(e + 8, &py));
            // SHORT values sit in the low half on little-endian, but we retyped to
            // LONG, so both byte orders read back the plain numbers.
            CHECK(px == 640 && py == 480);
        }

    // orientation -> transform table (mirror first, then clockwise turns)
    const int rot[9] = {0, 0, 0, 2, 2, 3, 1, 1, 3};
    const bool flip[9] = {false, false, true, false, true, true, false, true, false};
    for (int o = 1; o <= 8; ++o)
    {
        const exif::Transform t = exif::orientationToTransform(o);
        CHECK(t.rot == rot[o] && t.flip == flip[o]);
    }
    CHECK(exif::orientationToTransform(0).rot == 0 && !exif::orientationToTransform(9).flip);

    // No EXIF / not a JPEG / out-of-range orientation.
    {
        exif::JpegMeta m;
        Bytes plain = jpeg({}, false, false);
        CHECK(exif::readJpegMeta(plain.data(), plain.size(), &m) && m.orientation == 1 && m.exif.empty());
        Bytes png{0x89, 'P', 'N', 'G', 0, 0, 0, 0};
        CHECK(!exif::readJpegMeta(png.data(), png.size(), &m));
        Bytes bad = jpeg(tiff(true, 9), true, false);
        CHECK(exif::readJpegMeta(bad.data(), bad.size(), &m) && m.orientation == 1);
    }

    // Fuzz: damaged headers must never crash or read out of range.
    std::mt19937 rng(11);
    const Bytes base = jpeg(tiff(true, 6), true, true);
    for (int it = 0; it < 100000; ++it)
    {
        Bytes b = base;
        for (int k = 1 + static_cast<int>(rng() % 5); k > 0; --k)
            b[rng() % b.size()] = static_cast<uint8_t>(rng());
        if (rng() % 4 == 0)
            b.resize(rng() % b.size());
        exif::JpegMeta m;
        if (exif::readJpegMeta(b.data(), b.size(), &m))
            (void)exif::embed(base, m, static_cast<int>(rng() % 70000), static_cast<int>(rng() % 70000));
    }
    // The same for the GPS stripper: damaged headers never crash it.
    const Bytes gpsBase = jpeg(tiffGps(true), true, true);
    for (int it = 0; it < 100000; ++it)
    {
        Bytes b = gpsBase;
        for (int k = 1 + static_cast<int>(rng() % 5); k > 0; --k)
            b[rng() % b.size()] = static_cast<uint8_t>(rng());
        if (rng() % 4 == 0)
            b.resize(rng() % b.size());
        exif::JpegMeta m;
        if (exif::readJpegMeta(b.data(), b.size(), &m))
        {
            exif::stripGps(&m);
            (void)exif::embed(base, m, 640, 480);
        }
    }

    std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}