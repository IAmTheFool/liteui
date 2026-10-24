// Build: g++ -std=c++20 -I.. exif_test.cpp -o exif_test && ./exif_test
#include "io/exif.hpp"
#include <cstdio>
#include <random>

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

int main()
{
    testStripGps();
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