// exif.hpp - JPEG metadata helpers with no dependencies:
//   * the EXIF orientation tag (what camera rotation a photo was shot with),
//   * carrying a source JPEG's EXIF block and ICC profile into a re-encoded
//     JPEG, with the orientation reset (the pixels are already upright), the
//     embedded thumbnail erased, and the picture's new size filled in,
//   * the camera data the Info panel shows (readPhotoInfo): make, model, lens,
//     date taken, ISO, shutter, aperture and focal length, plus the functions
//     that turn them into display text.
// Everything works on byte buffers and bounds-checks every read, so a damaged
// or hostile file can't read out of range; it just yields "no metadata".
//
// Not carried over: XMP and IPTC (so captions, copyright fields and any
// develop settings stored in XMP are dropped), and thumbnails/previews stored
// anywhere other than IFD1 (e.g. inside a MakerNote). GPS data in the EXIF
// block IS carried over verbatim, unless the caller asks for stripGps() on the
// JpegMeta before embedding it (Export's "Remove location" option).
//
// Embedding the source ICC profile is only right if the pixels are still in
// that profile's color space. If the pipeline converts to sRGB (or any working
// space) before encoding, don't embed the source profile.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace exif
{

    // How to turn a picture upright: mirror left-right first (if `flip`), then
    // turn `rot` clockwise quarter turns. Same convention as literoom's Orient.
    struct Transform
    {
        int rot = 0;
        bool flip = false;
    };

    // EXIF orientation value (1..8) -> the transform that makes it upright.
    //   1 normal   2 mirror        3 turn 180   4 mirror vertically
    //   5 transpose  6 turn 90 cw  7 transverse  8 turn 270 cw
    inline Transform orientationToTransform(int o)
    {
        switch (o)
        {
        case 2:
            return {0, true};
        case 3:
            return {2, false};
        case 4:
            return {2, true};
        case 5:
            return {3, true};
        case 6:
            return {1, false};
        case 7:
            return {1, true};
        case 8:
            return {3, false};
        default:
            return {0, false};
        }
    }

    struct JpegMeta
    {
        int orientation = 1; // 1..8 as stored in the file (1 if absent or invalid)
        // The EXIF APP1 payload ("Exif\0\0" + TIFF data) as it should be written to
        // an upright re-encode: orientation set to 1 and the thumbnail erased and
        // unlinked. Empty if the file had none.
        std::vector<uint8_t> exif;
        // The ICC profile APP2 payloads ("ICC_PROFILE\0" + chunk header + data),
        // verbatim, in sequence order. Empty if the file had none, or if its
        // chunks were incomplete, inconsistent or oversized (a broken profile is
        // worse than none).
        std::vector<std::vector<uint8_t>> icc;
    };

    namespace detail
    {

        // Upper bound on the total ICC data we are willing to carry.
        constexpr size_t kMaxIccBytes = size_t{16} << 20;

        // A bounds-checked, endian-aware view over a TIFF block.
        struct Tiff
        {
            std::vector<uint8_t> &b;
            bool le = true;

            bool ok(uint64_t off, uint64_t len) const
            {
                return off <= b.size() && len <= b.size() - off;
            }
            bool rd16(uint64_t off, uint16_t *v) const
            {
                if (!ok(off, 2))
                    return false;
                *v = le ? static_cast<uint16_t>(b[off] | (b[off + 1] << 8))
                        : static_cast<uint16_t>((b[off] << 8) | b[off + 1]);
                return true;
            }
            bool rd32(uint64_t off, uint32_t *v) const
            {
                if (!ok(off, 4))
                    return false;
                const uint32_t a = b[off], c = b[off + 1], d = b[off + 2], e = b[off + 3];
                *v = le ? (a | (c << 8) | (d << 16) | (e << 24))
                        : ((a << 24) | (c << 16) | (d << 8) | e);
                return true;
            }
            bool wr16(uint64_t off, uint16_t v)
            {
                if (!ok(off, 2))
                    return false;
                if (le)
                {
                    b[off] = static_cast<uint8_t>(v);
                    b[off + 1] = static_cast<uint8_t>(v >> 8);
                }
                else
                {
                    b[off] = static_cast<uint8_t>(v >> 8);
                    b[off + 1] = static_cast<uint8_t>(v);
                }
                return true;
            }
            bool wr32(uint64_t off, uint32_t v)
            {
                if (!ok(off, 4))
                    return false;
                for (int i = 0; i < 4; ++i)
                {
                    const int shift = le ? 8 * i : 8 * (3 - i);
                    b[off + static_cast<uint64_t>(i)] = static_cast<uint8_t>(v >> shift);
                }
                return true;
            }
            // Zeroes [off, off+len) if it lies inside the block.
            void zero(uint64_t off, uint64_t len)
            {
                if (!ok(off, len))
                    return;
                std::fill_n(b.begin() + static_cast<std::ptrdiff_t>(off),
                            static_cast<std::ptrdiff_t>(len), uint8_t{0});
            }
        };

        // Finds `tag` in the IFD at `ifd`. On success *entry is the offset of its
        // 12-byte entry. *count / *end (if asked) describe the IFD and are filled
        // in whenever the IFD itself is readable, even if the tag is absent;
        // *end is where the "next IFD" pointer lives.
        inline bool findEntry(const Tiff &t, uint32_t ifd, uint16_t tag, uint64_t *entry,
                              uint16_t *count = nullptr, uint64_t *end = nullptr)
        {
            uint16_t n;
            if (!t.rd16(ifd, &n) || !t.ok(uint64_t{ifd} + 2, uint64_t{n} * 12))
                return false;
            if (count)
                *count = n;
            if (end)
                *end = uint64_t{ifd} + 2 + uint64_t{n} * 12;
            for (uint16_t i = 0; i < n; ++i)
            {
                const uint64_t off = uint64_t{ifd} + 2 + uint64_t{i} * 12;
                uint16_t tg;
                if (t.rd16(off, &tg) && tg == tag)
                {
                    *entry = off;
                    return true;
                }
            }
            return false;
        }

        // Reads the TIFF header: byte order and the offset of IFD0.
        inline bool tiffHeader(Tiff &t, uint32_t *ifd0)
        {
            if (t.b.size() < 8)
                return false;
            if (t.b[0] == 'I' && t.b[1] == 'I')
                t.le = true;
            else if (t.b[0] == 'M' && t.b[1] == 'M')
                t.le = false;
            else
                return false;
            uint16_t magic;
            // An IFD can't overlap the 8-byte header (writes there would clobber it).
            return t.rd16(2, &magic) && magic == 42 && t.rd32(4, ifd0) && *ifd0 >= 8;
        }

        // Erases the thumbnail that IFD1 describes (its JPEG data and its IFD) and
        // unlinks IFD1. Merely unlinking would leave the original, uncropped
        // thumbnail sitting in the file for anyone to extract.
        // `nextIfdPtr` is the offset of IFD0's "next IFD" field.
        inline void dropThumbnail(Tiff &t, uint64_t nextIfdPtr)
        {
            uint32_t ifd1;
            if (!t.rd32(nextIfdPtr, &ifd1) || ifd1 == 0)
                return;
            if (ifd1 >= 8)
            {
                // Read everything we need before zeroing anything.
                uint64_t e = 0, ifdEnd = 0;
                uint32_t off = 0, len = 0;
                bool haveThumb = findEntry(t, ifd1, 0x0201, &e, nullptr, &ifdEnd) &&
                                 t.rd32(e + 8, &off) && off >= 8;
                if (haveThumb)
                {
                    uint64_t e2 = 0;
                    haveThumb = findEntry(t, ifd1, 0x0202, &e2) && t.rd32(e2 + 8, &len);
                }
                if (haveThumb)
                    t.zero(off, len);
                if (ifdEnd != 0)
                    t.zero(ifd1, ifdEnd + 4 - ifd1); // IFD1's entries + its next pointer
            }
            t.wr32(nextIfdPtr, 0); // IFD0's "next IFD" -> none
        }

        // Sets the orientation to 1, erases and unlinks IFD1 (the thumbnail, whose
        // own orientation we'd otherwise have to fix), and returns the original
        // orientation value. The thumbnail is dropped whether or not an
        // orientation tag exists.
        inline int resetOrientation(Tiff &t, uint32_t ifd0)
        {
            int original = 1;
            uint64_t entry = 0, end = 0;
            uint16_t count = 0;
            if (findEntry(t, ifd0, 0x0112, &entry, &count, &end))
            {
                uint16_t type;
                uint32_t cnt;
                uint16_t v;
                if (t.rd16(entry + 2, &type) && type == 3 && t.rd32(entry + 4, &cnt) &&
                    cnt == 1 && t.rd16(entry + 8, &v))
                {
                    if (v >= 1 && v <= 8)
                        original = v;
                    t.wr16(entry + 8, 1);
                }
            }
            if (end != 0) // IFD0 was readable, tag or not
                dropThumbnail(t, end);
            return original;
        }

        // Rewrites the entry for `tag` in the IFD at `ifd` (if present) as a single
        // LONG holding `value`. Entries are 12 bytes with the value stored inline,
        // so changing the type in place is always safe.
        inline void setLong(Tiff &t, uint32_t ifd, uint16_t tag, uint32_t value)
        {
            uint64_t e;
            if (findEntry(t, ifd, tag, &e))
            {
                t.wr16(e + 2, 4); // LONG: room for any size
                t.wr32(e + 4, 1); // count
                t.wr32(e + 8, value);
            }
        }

        // Writes the picture's size into IFD0's ImageWidth / ImageLength and the
        // EXIF sub-IFD's PixelXDimension / PixelYDimension, where present.
        inline void setDimensions(Tiff &t, uint32_t ifd0, uint32_t w, uint32_t h)
        {
            setLong(t, ifd0, 0x0100, w);
            setLong(t, ifd0, 0x0101, h);

            uint64_t e;
            uint16_t type;
            uint32_t cnt, sub;
            if (findEntry(t, ifd0, 0x8769, &e) && t.rd16(e + 2, &type) && type == 4 &&
                t.rd32(e + 4, &cnt) && cnt == 1 && t.rd32(e + 8, &sub) && sub >= 8)
            {
                setLong(t, sub, 0xA002, w);
                setLong(t, sub, 0xA003, h);
            }
        }

        // Bytes one value of an IFD entry takes, by TIFF type (0 for unknown types).
        inline uint64_t tiffTypeSize(uint16_t type)
        {
            switch (type)
            {
            case 1:  // BYTE
            case 2:  // ASCII
            case 6:  // SBYTE
            case 7:  // UNDEFINED
                return 1;
            case 3:  // SHORT
            case 8:  // SSHORT
                return 2;
            case 4:  // LONG
            case 9:  // SLONG
            case 11: // FLOAT
                return 4;
            case 5:  // RATIONAL
            case 10: // SRATIONAL
            case 12: // DOUBLE
                return 8;
            default:
                return 0;
            }
        }

        // Removes the GPS block: the GPS IFD that IFD0's GPSInfo tag (0x8825)
        // points to is zeroed (its entries, its next pointer and every value it
        // keeps outside the entries), and the tag itself is cut out of IFD0, with
        // the entries after it moving up. Merely dropping the tag would leave the
        // coordinates in the file. Returns true if there was a GPSInfo tag.
        // Only this standard GPS IFD is handled: location a camera hides inside
        // its own MakerNote is not touched.
        inline bool dropGps(Tiff &t, uint32_t ifd0)
        {
            uint64_t entry = 0, end = 0;
            uint16_t count = 0;
            if (!findEntry(t, ifd0, 0x8825, &entry, &count, &end))
                return false;

            // Read everything first, then zero (the GPS IFD of a hostile file may
            // overlap other structures; this keeps the reads honest).
            uint16_t type = 0;
            uint32_t cnt = 0, gps = 0;
            const bool wellFormed = t.rd16(entry + 2, &type) && type == 4 &&
                                    t.rd32(entry + 4, &cnt) && cnt == 1 &&
                                    t.rd32(entry + 8, &gps) && gps >= 8;
            if (wellFormed)
            {
                uint16_t n = 0;
                if (t.rd16(gps, &n) && t.ok(uint64_t{gps} + 2, uint64_t{n} * 12 + 4))
                {
                    for (uint16_t i = 0; i < n; ++i)
                    {
                        const uint64_t e = uint64_t{gps} + 2 + uint64_t{i} * 12;
                        uint16_t vt = 0;
                        uint32_t vc = 0, off = 0;
                        if (t.rd16(e + 2, &vt) && t.rd32(e + 4, &vc) && t.rd32(e + 8, &off))
                        {
                            const uint64_t bytes = tiffTypeSize(vt) * vc;
                            if (bytes > 4 && off >= 8)
                                t.zero(off, bytes); // the value lives outside the entry
                        }
                    }
                    t.zero(gps, uint64_t{n} * 12 + 6); // count + entries + next pointer
                }
            }

            // Cut the tag out of IFD0: entries after it (and the next-IFD pointer)
            // move up 12 bytes, the freed tail is zeroed, the count goes down by 1.
            const uint64_t blockEnd = end + 4; // just past IFD0's next pointer
            if (!t.ok(entry, blockEnd - entry) || count == 0)
                return true; // can't happen after findEntry, but never go out of range
            std::copy(t.b.begin() + static_cast<std::ptrdiff_t>(entry + 12),
                      t.b.begin() + static_cast<std::ptrdiff_t>(blockEnd),
                      t.b.begin() + static_cast<std::ptrdiff_t>(entry));
            t.zero(blockEnd - 12, 12);
            t.wr16(ifd0, static_cast<uint16_t>(count - 1));
            return true;
        }

        inline bool isExifPayload(const uint8_t *p, size_t n)
        {
            return p && n > 14 && std::memcmp(p, "Exif\0\0", 6) == 0;
        }
        inline bool isIccPayload(const uint8_t *p, size_t n)
        {
            return p && n > 14 && std::memcmp(p, "ICC_PROFILE\0", 12) == 0;
        }

        // Checks that the ICC chunks form one complete profile: every chunk agrees
        // on the count, and the sequence numbers are exactly 1..count, each once.
        // On success the chunks are put in sequence order. On failure returns false.
        inline bool normalizeIcc(std::vector<std::vector<uint8_t>> &icc)
        {
            const size_t n = icc.size();
            if (n == 0)
                return true;
            if (n > 255)
                return false;
            std::vector<const std::vector<uint8_t> *> slot(n, nullptr);
            for (const auto &c : icc)
            {
                if (!isIccPayload(c.data(), c.size()))
                    return false;
                const size_t seq = c[12], cnt = c[13];
                if (cnt != n || seq < 1 || seq > n || slot[seq - 1])
                    return false;
                slot[seq - 1] = &c;
            }
            std::vector<std::vector<uint8_t>> sorted;
            sorted.reserve(n);
            for (const auto *c : slot)
                sorted.push_back(*c);
            icc = std::move(sorted);
            return true;
        }

    } // namespace detail

    // Reads the metadata of a JPEG held in memory (only the header part up to the
    // first scan is looked at, so passing a prefix of the file is fine). False if
    // it isn't a JPEG at all; true otherwise, even when there is no metadata.
    inline bool readJpegMeta(const uint8_t *data, size_t size, JpegMeta *out)
    {
        *out = JpegMeta{};
        if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
            return false;
        size_t i = 2;
        size_t iccBytes = 0;
        bool iccBad = false;
        while (i + 4 <= size)
        {
            if (data[i] != 0xFF) // lost sync: stop with what we have
                break;
            const uint8_t marker = data[i + 1];
            if (marker == 0xFF)
            { // fill byte
                ++i;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) // end of image / start of scan
                break;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
            { // no length
                i += 2;
                continue;
            }
            const size_t len = (static_cast<size_t>(data[i + 2]) << 8) | data[i + 3];
            if (len < 2 || i + 2 + len > size)
                break;
            const uint8_t *payload = data + i + 4;
            const size_t plen = len - 2;
            if (marker == 0xE1 && out->exif.empty() && detail::isExifPayload(payload, plen))
            {
                std::vector<uint8_t> block(payload, payload + plen);
                std::vector<uint8_t> tiffBytes(block.begin() + 6, block.end());
                detail::Tiff t{tiffBytes};
                uint32_t ifd0;
                if (detail::tiffHeader(t, &ifd0))
                {
                    out->orientation = detail::resetOrientation(t, ifd0);
                    std::copy(tiffBytes.begin(), tiffBytes.end(), block.begin() + 6);
                    out->exif = std::move(block);
                }
            }
            else if (marker == 0xE2 && !iccBad && detail::isIccPayload(payload, plen))
            {
                iccBytes += plen;
                if (iccBytes > detail::kMaxIccBytes)
                    iccBad = true; // truncating would leave a broken profile
                else
                    out->icc.emplace_back(payload, payload + plen);
            }
            i += 2 + len;
        }
        if (iccBad || !detail::normalizeIcc(out->icc))
            out->icc.clear();
        return true;
    }

    // Removes the GPS location from `meta->exif` (see detail::dropGps), leaving
    // everything else in it alone. Returns true if there was a GPS block. Does
    // nothing (false) for an empty or malformed EXIF block.
    inline bool stripGps(JpegMeta *meta)
    {
        if (!detail::isExifPayload(meta->exif.data(), meta->exif.size()))
            return false;
        std::vector<uint8_t> tiffBytes(meta->exif.begin() + 6, meta->exif.end());
        detail::Tiff t{tiffBytes};
        uint32_t ifd0;
        if (!detail::tiffHeader(t, &ifd0) || !detail::dropGps(t, ifd0))
            return false;
        std::copy(tiffBytes.begin(), tiffBytes.end(), meta->exif.begin() + 6);
        return true;
    }

    // Returns `jpeg` with the EXIF block (sized for a w x h picture) and ICC
    // profile of `meta` inserted after the SOI marker (and after a JFIF APP0 if
    // the JPEG starts with one, as the JFIF spec requires). A buffer that isn't a
    // JPEG is returned unchanged. The EXIF segment is left out when `meta.exif`
    // is malformed or w/h aren't positive (its size fields would be stale), and
    // invalid ICC chunks are skipped.
    inline std::vector<uint8_t> embed(const std::vector<uint8_t> &jpeg,
                                      const JpegMeta &meta, int w, int h)
    {
        if (jpeg.size() < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
            return jpeg;

        // Insert point: right after SOI, or after a leading JFIF APP0.
        size_t at = 2;
        if (jpeg.size() >= 6 && jpeg[2] == 0xFF && jpeg[3] == 0xE0)
        {
            const size_t len = (static_cast<size_t>(jpeg[4]) << 8) | jpeg[5];
            if (len >= 2 && 4 + len <= jpeg.size())
                at = 4 + len;
        }

        std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + static_cast<std::ptrdiff_t>(at));
        auto segment = [&out](uint8_t marker, const std::vector<uint8_t> &payload)
        {
            if (payload.size() + 2 > 0xFFFF)
                return;
            const size_t len = payload.size() + 2;
            out.push_back(0xFF);
            out.push_back(marker);
            out.push_back(static_cast<uint8_t>(len >> 8));
            out.push_back(static_cast<uint8_t>(len));
            out.insert(out.end(), payload.begin(), payload.end());
        };

        if (detail::isExifPayload(meta.exif.data(), meta.exif.size()) && w > 0 && h > 0)
        {
            std::vector<uint8_t> block = meta.exif;
            std::vector<uint8_t> tiffBytes(block.begin() + 6, block.end());
            detail::Tiff t{tiffBytes};
            uint32_t ifd0;
            if (detail::tiffHeader(t, &ifd0))
            {
                detail::setDimensions(t, ifd0, static_cast<uint32_t>(w),
                                      static_cast<uint32_t>(h));
                std::copy(tiffBytes.begin(), tiffBytes.end(), block.begin() + 6);
                segment(0xE1, block);
            }
        }
        for (const auto &chunk : meta.icc)
            if (detail::isIccPayload(chunk.data(), chunk.size()))
                segment(0xE2, chunk);

        out.insert(out.end(), jpeg.begin() + static_cast<std::ptrdiff_t>(at), jpeg.end());
        return out;
    }

    // ------------------------------------------------------------ Photo info
    // The camera data the Info panel shows. Read from the ORIGINAL EXIF block of
    // the file (readJpegMeta rewrites its copy for an upright re-encode, so the
    // two never share a buffer). Every field is optional: text is empty, numbers
    // are 0 when the file does not say, or says something implausible.
    //
    // GPS is deliberately not read: the Info panel never shows a location.
    struct PhotoInfo
    {
        std::string make;      // IFD0 Make, e.g. "Canon"
        std::string model;     // IFD0 Model, e.g. "Canon EOS R5"
        std::string lens;      // LensModel (Exif tag 0xA434); empty if not recorded
        std::string dateTaken; // "YYYY-MM-DD HH:MM:SS" (DateTimeOriginal), or empty
        int iso = 0;           // 1..kInfoMaxIso, or 0
        double exposureTime = 0.0; // shutter, seconds (0, 86400], or 0
        double fNumber = 0.0;      // (0, 1000], or 0
        double focalLength = 0.0;  // millimetres, (0, 10000], or 0

        bool empty() const
        {
            return make.empty() && model.empty() && lens.empty() && dateTaken.empty() &&
                   iso == 0 && exposureTime == 0.0 && fNumber == 0.0 && focalLength == 0.0;
        }
    };

    // Text fields are cut to this many characters (a hostile file can claim a
    // multi-gigabyte string). ISO values above this are not believed.
    constexpr size_t kInfoMaxText = 128;
    constexpr int kInfoMaxIso = 10000000;

    namespace detail
    {

        // The payload ("Exif\0\0" + TIFF data) of the first EXIF APP1 segment, found
        // with the same marker walk as readJpegMeta. The pointer points into `data`.
        inline bool findExifSegment(const uint8_t *data, size_t size,
                                    const uint8_t **payload, size_t *payloadLen)
        {
            if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
                return false;
            size_t i = 2;
            while (i + 4 <= size)
            {
                if (data[i] != 0xFF)
                    break;
                const uint8_t marker = data[i + 1];
                if (marker == 0xFF)
                {
                    ++i;
                    continue;
                }
                if (marker == 0xD9 || marker == 0xDA)
                    break;
                if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8))
                {
                    i += 2;
                    continue;
                }
                const size_t len = (static_cast<size_t>(data[i + 2]) << 8) | data[i + 3];
                if (len < 2 || i + 2 + len > size)
                    break;
                if (marker == 0xE1 && isExifPayload(data + i + 4, len - 2))
                {
                    *payload = data + i + 4;
                    *payloadLen = len - 2;
                    return true;
                }
                i += 2 + len;
            }
            return false;
        }

        // EXIF "ASCII" bytes made safe to show: reading stops at the first NUL;
        // spaces, tabs and line breaks become single spaces (none at either end);
        // other control characters are dropped; anything outside 7-bit ASCII becomes
        // '?'; at most kInfoMaxText characters.
        inline std::string cleanText(const uint8_t *p, size_t n)
        {
            std::string s;
            bool pendingSpace = false;
            for (size_t i = 0; i < n && p[i] != 0 && s.size() < kInfoMaxText; ++i)
            {
                const unsigned char c = p[i];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                {
                    pendingSpace = !s.empty();
                    continue;
                }
                if (c < 0x20 || c == 0x7F)
                    continue;
                if (pendingSpace)
                {
                    s.push_back(' ');
                    pendingSpace = false;
                    if (s.size() >= kInfoMaxText)
                        break;
                }
                s.push_back(c < 0x7F ? static_cast<char>(c) : '?');
            }
            return s;
        }

        // Where the `count` values of type `type` of the entry at `entry` live: in
        // the entry's own 4 value bytes if they fit, else at the offset stored
        // there. False if that is out of range.
        inline bool valueOffset(const Tiff &t, uint64_t entry, uint64_t bytes, uint64_t *off)
        {
            if (bytes <= 4)
            {
                *off = entry + 8;
                return t.ok(*off, bytes);
            }
            uint32_t o = 0;
            if (!t.rd32(entry + 8, &o) || o < 8 || !t.ok(o, bytes))
                return false;
            *off = o;
            return true;
        }

        // An ASCII entry as clean text; false if the type is wrong, the value is out
        // of range, or nothing printable is left.
        inline bool readText(const Tiff &t, uint64_t entry, std::string *out)
        {
            uint16_t type = 0;
            uint32_t cnt = 0;
            uint64_t off = 0;
            if (!t.rd16(entry + 2, &type) || type != 2 || !t.rd32(entry + 4, &cnt) || cnt == 0 ||
                !valueOffset(t, entry, cnt, &off))
                return false;
            *out = cleanText(t.b.data() + off, std::min<uint64_t>(cnt, 4096));
            return !out->empty();
        }

        // The first value of a RATIONAL entry as a double; false for the wrong type,
        // out-of-range data or a zero denominator.
        inline bool readRational(const Tiff &t, uint64_t entry, double *out)
        {
            uint16_t type = 0;
            uint32_t cnt = 0, num = 0, den = 0;
            uint64_t off = 0;
            if (!t.rd16(entry + 2, &type) || type != 5 || !t.rd32(entry + 4, &cnt) || cnt == 0 ||
                !valueOffset(t, entry, uint64_t{cnt} * 8, &off) || !t.rd32(off, &num) ||
                !t.rd32(off + 4, &den) || den == 0)
                return false;
            *out = static_cast<double>(num) / static_cast<double>(den);
            return true;
        }

        // The first value of a SHORT or LONG entry.
        inline bool readFirstUInt(const Tiff &t, uint64_t entry, uint32_t *out)
        {
            uint16_t type = 0;
            uint32_t cnt = 0;
            uint64_t off = 0;
            if (!t.rd16(entry + 2, &type) || (type != 3 && type != 4) || !t.rd32(entry + 4, &cnt) ||
                cnt == 0)
                return false;
            const uint64_t size = type == 3 ? 2 : 4;
            if (!valueOffset(t, entry, uint64_t{cnt} * size, &off))
                return false;
            uint16_t v16 = 0;
            if (type == 3)
            {
                if (!t.rd16(off, &v16))
                    return false;
                *out = v16;
                return true;
            }
            return t.rd32(off, out);
        }

        inline bool textTag(const Tiff &t, uint32_t ifd, uint16_t tag, std::string *out)
        {
            uint64_t e = 0;
            return findEntry(t, ifd, tag, &e) && readText(t, e, out);
        }
        inline bool rationalTag(const Tiff &t, uint32_t ifd, uint16_t tag, double *out)
        {
            uint64_t e = 0;
            return findEntry(t, ifd, tag, &e) && readRational(t, e, out);
        }
        inline bool uintTag(const Tiff &t, uint32_t ifd, uint16_t tag, uint32_t *out)
        {
            uint64_t e = 0;
            return findEntry(t, ifd, tag, &e) && readFirstUInt(t, e, out);
        }

        // "2024:06:15 14:30:05" (the EXIF form; '-' also accepted between the date
        // parts, anything after the first 19 characters ignored) -> "2024-06-15
        // 14:30:05". Empty for anything that is not a real date and time (cameras
        // write "0000:00:00 00:00:00" or blanks when the clock was never set).
        inline std::string normalizeDate(const std::string &s)
        {
            if (s.size() < 19)
                return {};
            auto dig = [&s](size_t i)
            { return s[i] >= '0' && s[i] <= '9'; };
            auto num = [&s](size_t i, size_t n)
            {
                int v = 0;
                for (size_t k = 0; k < n; ++k)
                    v = v * 10 + (s[i + k] - '0');
                return v;
            };
            static const size_t kDigits[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
            for (size_t i : kDigits)
                if (!dig(i))
                    return {};
            if ((s[4] != ':' && s[4] != '-') || (s[7] != ':' && s[7] != '-') || s[10] != ' ' ||
                s[13] != ':' || s[16] != ':')
                return {};
            const int y = num(0, 4), mo = num(5, 2), d = num(8, 2), h = num(11, 2), mi = num(14, 2),
                      se = num(17, 2);
            if (y < 1 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59)
                return {};
            std::string out = s.substr(0, 19);
            out[4] = out[7] = '-';
            return out;
        }

        // Fixed-point text with trailing zeros (and a trailing '.') removed.
        inline std::string trimmedFixed(double v, int decimals)
        {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
            std::string s = buf;
            if (s.find('.') != std::string::npos)
            {
                while (!s.empty() && s.back() == '0')
                    s.pop_back();
                if (!s.empty() && s.back() == '.')
                    s.pop_back();
            }
            return s;
        }

    } // namespace detail

    // Reads the camera data of a JPEG held in memory (a prefix of the file is
    // fine, like readJpegMeta). False if it isn't a JPEG at all; true otherwise,
    // even when it has no EXIF block or none of the fields (see PhotoInfo::empty).
    // Reads the first EXIF APP1 only. Every read is bounds-checked, and every
    // value is range-checked, so a damaged or hostile file gives empty fields,
    // never an out-of-range read or a value the UI could not show.
    inline bool readPhotoInfo(const uint8_t *data, size_t size, PhotoInfo *out)
    {
        *out = PhotoInfo{};
        if (!data || size < 4 || data[0] != 0xFF || data[1] != 0xD8)
            return false;
        const uint8_t *payload = nullptr;
        size_t plen = 0;
        if (!detail::findExifSegment(data, size, &payload, &plen))
            return true;
        std::vector<uint8_t> tiffBytes(payload + 6, payload + plen); // Tiff wants a mutable vector
        detail::Tiff t{tiffBytes};
        uint32_t ifd0 = 0;
        if (!detail::tiffHeader(t, &ifd0))
            return true;

        detail::textTag(t, ifd0, 0x010F, &out->make);
        detail::textTag(t, ifd0, 0x0110, &out->model);

        uint64_t e = 0;
        uint16_t type = 0;
        uint32_t cnt = 0, sub = 0;
        if (!(detail::findEntry(t, ifd0, 0x8769, &e) && t.rd16(e + 2, &type) && type == 4 &&
              t.rd32(e + 4, &cnt) && cnt == 1 && t.rd32(e + 8, &sub) && sub >= 8))
            return true; // no Exif sub-IFD: make and model only

        detail::textTag(t, sub, 0xA434, &out->lens);

        std::string date;
        if (detail::textTag(t, sub, 0x9003, &date))
            out->dateTaken = detail::normalizeDate(date);
        if (out->dateTaken.empty() && detail::textTag(t, sub, 0x9004, &date)) // DateTimeDigitized
            out->dateTaken = detail::normalizeDate(date);

        double v = 0.0;
        if (detail::rationalTag(t, sub, 0x829A, &v) && v > 0.0 && v <= 86400.0)
            out->exposureTime = v;
        if (detail::rationalTag(t, sub, 0x829D, &v) && v > 0.0 && v <= 1000.0)
            out->fNumber = v;
        if (detail::rationalTag(t, sub, 0x920A, &v) && v > 0.0 && v <= 10000.0)
            out->focalLength = v;

        // ISO: PhotographicSensitivity (0x8827); 65535 or 0 there means "see
        // ISOSpeed" (0x8833, a LONG that can hold values above 65534).
        uint32_t iso = 0;
        const bool haveIso = detail::uintTag(t, sub, 0x8827, &iso);
        if (!haveIso || iso == 0 || iso == 65535)
        {
            uint32_t wide = 0;
            iso = detail::uintTag(t, sub, 0x8833, &wide) ? wide : 0;
        }
        if (iso >= 1 && iso <= static_cast<uint32_t>(kInfoMaxIso))
            out->iso = static_cast<int>(iso);
        return true;
    }

    // ----------------------------------------------------------- Display text
    // All of these return an empty string for "unknown", so a caller can skip the
    // row. Plain ASCII, no locale.

    // "1/250 s" for fast shutters, "0.4 s" when the time is not close to 1/N,
    // "2.5 s" / "30 s" for long ones. Empty for 0, negative, NaN or under a microsecond.
    inline std::string describeShutter(double seconds)
    {
        if (!(seconds >= 1e-6) || !std::isfinite(seconds))
            return {};
        if (seconds >= 1.0)
            return detail::trimmedFixed(seconds, seconds < 10.0 ? 1 : 0) + " s";
        const double n = 1.0 / seconds;
        const double r = std::round(n);
        if (r >= 2.0 && std::fabs(n - r) <= 0.03 * r)
            return "1/" + std::to_string(static_cast<long long>(r)) + " s";
        return detail::trimmedFixed(seconds, 2) + " s";
    }

    // "f/2.8", "f/11".
    inline std::string describeAperture(double f)
    {
        if (!(f > 0.0) || !std::isfinite(f))
            return {};
        return "f/" + detail::trimmedFixed(f, 1);
    }

    // "85 mm", "24.5 mm".
    inline std::string describeFocalLength(double mm)
    {
        if (!(mm > 0.0) || !std::isfinite(mm))
            return {};
        return detail::trimmedFixed(mm, 1) + " mm";
    }

    // "ISO 400".
    inline std::string describeIso(int iso)
    {
        return iso > 0 ? "ISO " + std::to_string(iso) : std::string();
    }

    // "1/250 s, f/2.8, ISO 400" with the unknown parts left out.
    inline std::string describeExposure(const PhotoInfo &p)
    {
        std::string s;
        for (const std::string &part :
             {describeShutter(p.exposureTime), describeAperture(p.fNumber), describeIso(p.iso)})
        {
            if (part.empty())
                continue;
            if (!s.empty())
                s += ", ";
            s += part;
        }
        return s;
    }

    // "Canon EOS R5" for make "Canon" + model "Canon EOS R5" (models usually repeat
    // the make's first word); "SONY ILCE-7M3" when they don't. Either part may be
    // missing.
    inline std::string describeCamera(const PhotoInfo &p)
    {
        if (p.make.empty())
            return p.model;
        if (p.model.empty())
            return p.make;
        const size_t sp = p.make.find(' ');
        const std::string first = p.make.substr(0, sp);
        bool repeated = p.model.size() >= first.size();
        for (size_t i = 0; repeated && i < first.size(); ++i)
            repeated = std::tolower(static_cast<unsigned char>(p.model[i])) ==
                       std::tolower(static_cast<unsigned char>(first[i]));
        return repeated ? p.model : p.make + " " + p.model;
    }

} // namespace exif