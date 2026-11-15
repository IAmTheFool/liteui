// image_source.hpp - The ONE place that knows how a picture file is opened for editing, exporting
// and the filmstrip. Before this header three workers each decided for themselves ("is it a 16-bit
// PNG? then decodeImage16 + orient, else decodeFile + orient"): the editor's decode job, the batch
// export and the sidecar-size read. A new kind of source (TIFF, later RAW) is now one more branch
// here, not three edits that have to agree.
//
//   pngIsTruncated   a PNG that ends before its IEND chunk (a failed copy or download)
//   depthOf          Narrow (8 bits per channel: JPEG, 8-bit PNG, ...) or Wide (more than 8 bits). Decided
//                    from the file's header, not its extension. Today only a 16-bit PNG is Wide.
//   openUpright      the picture turned upright: `wide` (Image16) for a Wide file, and `image` (its 8-bit
//                    form, which is also what an 8-bit file is) when asked for
//   uprightSize      the upright size, for a caller that needs only that (the sidecar sync): a header read
//   decodeForThumb   a picture to make a thumbnail from: 8-bit and NOT turned (the caller turns it)
//
// Everything here sits on liteui_image::probeFile (the header: format, size, bit depth, truncation) and
// decodeFileInto / decodeFile (the pixels, written straight into the picture's own memory). liteui decides the format from the file's CONTENT, refuses a
// truncated PNG or one with a bad chunk CRC on every platform, and keeps 16 bits. What stays here is Literoom's
// own wording of a truncated file (kTruncatedMessage), the Narrow / Wide decision and the turning.
//
// Orientation is passed in (`exifOrientationOf(path)` for now) so this header does not depend on
// image_meta.hpp and the filmstrip worker keeps injecting its own. A translation unit that includes
// this header needs LITEUI_IMPLEMENTATION defined before liteui.hpp (it includes image_codec16.hpp).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "liteui.hpp"
#include "core/geometry.hpp"        // Orient
#include "io/file_utils.hpp"        // fsPath
#include "core/image16.hpp"
#include "core/image16_convert.hpp" // narrowImage
#include "core/raster_ops.hpp"      // orientImage
#include "io/image_codec16.hpp"     // decodeImage16 (and the encoders the callers use)

namespace image_source
{
  enum class Depth
  {
    Narrow, // 8 bits per channel: the 8-bit chain is the whole picture
    Wide    // more than 8 bits: an Image16 twin carries the precision
  };

  inline constexpr const char *kTruncatedMessage = "the PNG file is incomplete (it ends before its last chunk)";

  namespace detail
  {
    // 8 (signature) + 21 (IHDR chunk header and data): the least a PNG header probe needs. A file that
    // starts with the PNG signature and is shorter than this was cut off inside its header.
    inline constexpr uint64_t kPngHeaderBytes = 29;

    // True for a file that begins with the 8-byte PNG signature and is shorter than its IHDR. liteui's
    // probe cannot read such a file at all (it reports a damaged header), but there is nothing damaged
    // about it: it is a PNG that was cut off.
    inline bool pngCutInsideHeader(const std::string &path)
    {
      static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
      std::ifstream f(fsPath(path), std::ios::binary | std::ios::ate);
      if (!f)
        return false;
      const std::streamoff size = f.tellg();
      if (size < 8 || static_cast<uint64_t>(size) >= kPngHeaderBytes)
        return false;
      unsigned char head[8] = {};
      f.seekg(0);
      f.read(reinterpret_cast<char *>(head), 8);
      return f && std::memcmp(head, kSignature, 8) == 0;
    }

    struct Probe
    {
      std::optional<liteui_image::ImageInfo> info; // set when the header could be read (even if truncated)
      bool truncated = false;                      // a PNG that ends early
      std::string error;                           // set when !info or truncated
    };

    // The header of `path`, with a PNG that ends early reported the same way however it ends.
    inline Probe probe(const std::string &path)
    {
      Probe r;
      std::string err;
      r.info = liteui_image::probeFile(path, &err);
      if (!r.info)
      {
        r.truncated = pngCutInsideHeader(path);
        r.error = r.truncated ? kTruncatedMessage : err;
        return r;
      }
      if (r.info->format == liteui_image::ImageFileFormat::Png && r.info->truncated)
      {
        r.truncated = true;
        r.error = kTruncatedMessage;
      }
      return r;
    }

    inline bool isWide(const liteui_image::ImageInfo &i)
    {
      return i.format == liteui_image::ImageFileFormat::Png && i.bitDepth == 16;
    }
  } // namespace detail

  // True for a PNG (by its 8-byte signature) that does not end with its IEND chunk: it was cut off.
  // The Windows decoder (WIC) used to hand back a picture with the missing rows blank for such a file,
  // which would have been edited and exported as if it were whole; liteui now refuses it on both
  // platforms, and this says so before a decode is started (and in Literoom's words). A file that is
  // not a PNG, or can't be read, is not truncated as far as this check can tell: the decoder reports
  // what is wrong with it.
  inline bool pngIsTruncated(const std::string &path) { return detail::probe(path).truncated; }

  // How a file is held, from its header. A file that can't be read or isn't recognized is Narrow
  // (the decoder then reports what is wrong with it). A truncated 16-bit PNG still says Wide: its
  // header does.
  inline Depth depthOf(const std::string &realPath)
  {
    const auto info = liteui_image::probeFile(realPath);
    return info && detail::isWide(*info) ? Depth::Wide : Depth::Narrow;
  }

  struct Opened
  {
    std::shared_ptr<CanvasImage> image; // 8-bit, upright. Null only for a Wide file opened with want8 = false
    std::shared_ptr<Image16> wide;      // upright; set for a Wide file, null for a Narrow one
  };

  // Opens `realPath` (a virtual copy's picture file, not the copy's own path) and turns it upright by
  // `orient`. A Wide file is read at full precision into `wide`; with `want8` its 8-bit form (what the
  // tools measure, the nearest byte of every sample) is made as well. A Narrow file gives `image` only.
  // The file is decoded once.
  //
  // nullopt with `*errorOut` set when nothing could be read.
  inline std::optional<Opened> openUpright(const std::string &realPath, const Orient &orient,
                                           bool want8, std::string *errorOut = nullptr)
  {
    const detail::Probe p = detail::probe(realPath);
    if (!p.info || p.truncated)
    {
      if (errorOut)
        *errorOut = p.error;
      return std::nullopt;
    }
    std::string error;
    Opened o;
    if (detail::isWide(*p.info))
    {
      // Decoded straight into the Image16's own samples: the picture exists once while it is read.
      std::optional<Image16> w = decodeImage16(realPath, &error);
      if (!w)
      {
        if (errorOut)
          *errorOut = error;
        return std::nullopt;
      }
      o.wide = orientImage(std::make_shared<Image16>(std::move(*w)), orient);
      if (want8)
        o.image = std::make_shared<CanvasImage>(narrowImage(*o.wide));
      return o;
    }
    std::optional<CanvasImage> raw = liteui_image::decodeFile(realPath, &error);
    if (!raw)
    {
      if (errorOut)
        *errorOut = error;
      return std::nullopt;
    }
    o.image = orientImage(std::make_shared<CanvasImage>(std::move(*raw)), orient);
    return o;
  }

  // The size the picture has once it is upright (a quarter turn swaps the sides). Read from the file's
  // header, no pixels are decoded: a corrupt picture whose header is fine reports its size here and
  // fails when it is opened. A header liteui's probe can't read (e.g. a JPEG whose frame header holds no
  // height) falls back to a full decode, which is what this always did. false with `*errorOut` on failure.
  inline bool uprightSize(const std::string &realPath, const Orient &orient, int *w, int *h,
                          std::string *errorOut = nullptr)
  {
    const detail::Probe p = detail::probe(realPath);
    if (p.truncated)
    {
      if (errorOut)
        *errorOut = p.error;
      return false;
    }
    constexpr uint32_t kMax = static_cast<uint32_t>(std::numeric_limits<int>::max());
    if (p.info && p.info->width <= kMax && p.info->height <= kMax)
    {
      *w = static_cast<int>(p.info->width);
      *h = static_cast<int>(p.info->height);
    }
    else
    {
      std::string error;
      std::optional<CanvasImage> img = liteui_image::decodeFile(realPath, &error);
      if (!img)
      {
        if (errorOut)
          *errorOut = error;
        return false;
      }
      *w = img->width;
      *h = img->height;
    }
    if (orient.rot & 1)
      std::swap(*w, *h);
    return true;
  }

  // The picture a thumbnail is made from: always 8-bit (a Wide file's 8-bit form, the nearest byte of
  // every sample, the same as the editor's), and not turned: the thumbnail worker turns it with its own
  // orientation function.
  inline std::optional<CanvasImage> decodeForThumb(const std::string &realPath, std::string *errorOut = nullptr)
  {
    const detail::Probe p = detail::probe(realPath);
    if (p.truncated)
    {
      if (errorOut)
        *errorOut = p.error;
      return std::nullopt;
    }
    return liteui_image::decodeFile(realPath, errorOut);
  }

} // namespace image_source