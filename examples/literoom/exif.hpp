// exif.hpp - JPEG metadata helpers with no dependencies:
//   * the EXIF orientation tag (what camera rotation a photo was shot with),
//   * carrying a source JPEG's EXIF block and ICC profile into a re-encoded
//     JPEG, with the orientation reset (the pixels are already upright) and
//     the picture's new size filled in.
// Everything works on byte buffers and bounds-checks every read, so a damaged
// or hostile file can't read out of range; it just yields "no metadata".
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        // an upright re-encode: orientation set to 1 and the thumbnail unlinked.
        // Empty if the file had none.
        std::vector<uint8_t> exif;
        // Every ICC profile APP2 payload ("ICC_PROFILE\0" + chunk header + data),
        // verbatim and in file order. Empty if none.
        std::vector<std::vector<uint8_t>> icc;
    };

    namespace detail
    {

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
        };

        // Finds `tag` in the IFD at `ifd`. On success *entry is the offset of its
        // 12-byte entry, and (if asked) *count / *end describe the IFD.
        inline bool findEntry(const Tiff &t, uint32_t ifd, uint16_t tag, uint64_t *entry,
                              uint16_t *count = nullptr, uint64_t *end = nullptr)
        {
            uint16_t n;
            if (!t.rd16(ifd, &n) || !t.ok(uint64_t{ifd} + 2, uint64_t{n} * 12))
                return false;
            if (count)
                *count = n;
            if (end)
                *end = uint64_t{ifd} + 2 + uint64_t{n} * 12; // where "next IFD" lives
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
            return t.rd16(2, &magic) && magic == 42 && t.rd32(4, ifd0);
        }

        // Sets the orientation to 1, unlinks IFD1 (the thumbnail, whose own
        // orientation we'd otherwise have to fix), and returns the original value.
        inline int resetOrientation(Tiff &t, uint32_t ifd0)
        {
            int original = 1;
            uint64_t entry, end;
            uint16_t count;
            if (!findEntry(t, ifd0, 0x0112, &entry, &count, &end))
                return original;
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
            t.wr32(end, 0); // IFD0's "next IFD" -> none
            return original;
        }

        // Writes the picture's size into the EXIF sub-IFD's PixelXDimension /
        // PixelYDimension, if present.
        inline void setDimensions(Tiff &t, uint32_t ifd0, uint32_t w, uint32_t h)
        {
            uint64_t e;
            uint32_t sub;
            if (!findEntry(t, ifd0, 0x8769, &e) || !t.rd32(e + 8, &sub))
                return;
            const struct
            {
                uint16_t tag;
                uint32_t value;
            } dims[2] = {{0xA002, w}, {0xA003, h}};
            for (const auto &d : dims)
                if (findEntry(t, sub, d.tag, &e))
                {
                    t.wr16(e + 2, 4); // LONG: room for any size
                    t.wr32(e + 4, 1); // count
                    t.wr32(e + 8, d.value);
                }
        }

        inline bool isExifPayload(const uint8_t *p, size_t n)
        {
            return n > 14 && std::memcmp(p, "Exif\0\0", 6) == 0;
        }
        inline bool isIccPayload(const uint8_t *p, size_t n)
        {
            return n > 14 && std::memcmp(p, "ICC_PROFILE\0", 12) == 0;
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
            else if (marker == 0xE2 && detail::isIccPayload(payload, plen) &&
                     (iccBytes += plen) <= (16u << 20))
            {
                out->icc.emplace_back(payload, payload + plen);
            }
            i += 2 + len;
        }
        return true;
    }

    // Returns `jpeg` with the EXIF block (sized for a w x h picture) and ICC
    // profile of `meta` inserted right after the SOI marker. A buffer that isn't
    // a JPEG, or an EXIF block too big for one segment, is left out / unchanged.
    inline std::vector<uint8_t> embed(const std::vector<uint8_t> &jpeg,
                                      const JpegMeta &meta, int w, int h)
    {
        if (jpeg.size() < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8)
            return jpeg;
        std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + 2);
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
        if (!meta.exif.empty())
        {
            std::vector<uint8_t> block = meta.exif;
            std::vector<uint8_t> tiffBytes(block.begin() + 6, block.end());
            detail::Tiff t{tiffBytes};
            uint32_t ifd0;
            if (detail::tiffHeader(t, &ifd0) && w > 0 && h > 0)
            {
                detail::setDimensions(t, ifd0, static_cast<uint32_t>(w),
                                      static_cast<uint32_t>(h));
                std::copy(tiffBytes.begin(), tiffBytes.end(), block.begin() + 6);
            }
            segment(0xE1, block);
        }
        for (const auto &chunk : meta.icc)
            segment(0xE2, chunk);
        out.insert(out.end(), jpeg.begin() + 2, jpeg.end());
        return out;
    }

} // namespace exif