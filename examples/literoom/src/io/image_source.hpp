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
//   uprightSize      the upright size, for a caller that needs only that (the sidecar sync)
//   decodeForThumb   a picture to make a thumbnail from: 8-bit and NOT turned (the caller turns it)
//
// Orientation is passed in (`exifOrientationOf(path)` for now) so this header does not depend on
// image_meta.hpp and the filmstrip worker keeps injecting its own. A translation unit that includes
// this header needs LITEUI_IMPLEMENTATION defined before liteui.hpp (it includes image_codec16.hpp).
#pragma once
#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "liteui.hpp"
#include "core/geometry.hpp"        // Orient
#include "io/file_utils.hpp"        // fsPath
#include "core/image16.hpp"
#include "core/image16_convert.hpp" // narrowImage
#include "core/raster_ops.hpp"      // orientImage
#include "io/image_codec16.hpp"     // readPngHeader, decodeImage16

namespace image_source
{
  enum class Depth
  {
    Narrow, // 8 bits per channel: the 8-bit chain is the whole picture
    Wide    // more than 8 bits: an Image16 twin carries the precision
  };

  // True for a PNG (by its 8-byte signature) that does not end with its IEND chunk: it was cut off.
  // The Linux decoder (libpng) refuses such a file, but the Windows one (WIC) hands back a picture
  // with the missing rows blank, which would be edited and exported as if it were whole. So every PNG
  // is checked here, the same way on both platforms. Bytes after IEND are tolerated (the last 4 KB are
  // searched). A file that is not a PNG, or can't be read, is not truncated as far as this check can
  // tell: the decoder reports what is wrong with it.
  inline bool pngIsTruncated(const std::string &path)
  {
    std::ifstream f(fsPath(path), std::ios::binary | std::ios::ate);
    if (!f)
      return false;
    const std::streamoff size = f.tellg();
    static const unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    unsigned char head[8] = {};
    f.seekg(0);
    f.read(reinterpret_cast<char *>(head), 8);
    if (!f || std::memcmp(head, kSignature, 8) != 0)
      return false;
    const std::streamoff n = std::min<std::streamoff>(size, 4096);
    std::vector<unsigned char> tail(static_cast<size_t>(n));
    f.seekg(size - n);
    f.read(reinterpret_cast<char *>(tail.data()), n);
    if (!f)
      return false;
    static const unsigned char kIend[8] = {'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82}; // type + its CRC
    return std::search(tail.begin(), tail.end(), kIend, kIend + 8) == tail.end();
  }

  inline constexpr const char *kTruncatedMessage = "the PNG file is incomplete (it ends before its last chunk)";

  // How a file is held, from its header. A file that can't be read or isn't recognized is Narrow
  // (the 8-bit decoder then reports what is wrong with it).
  inline Depth depthOf(const std::string &realPath)
  {
    const PngHeader h = readPngHeader(realPath);
    return h.ok() && h.bitDepth == 16 ? Depth::Wide : Depth::Narrow;
  }

  struct Opened
  {
    std::shared_ptr<CanvasImage> image; // 8-bit, upright. Null only for a Wide file opened with want8 = false
    std::shared_ptr<Image16> wide;      // upright; set for a Wide file, null for a Narrow one
  };

  // Opens `realPath` (a virtual copy's picture file, not the copy's own path) and turns it upright by
  // `orient`. A Wide file is read at full precision into `wide`; with `want8` its 8-bit form (what the
  // tools measure, the nearest byte of every sample) is made as well. A Narrow file gives `image` only.
  //
  // A Wide file whose full-precision read fails is retried through the 8-bit decoder, which may read
  // what the 16-bit one could not (and which says what is wrong if it fails too). So the editor and
  // the export always agree on what a file is.
  //
  // nullopt with `*errorOut` set when nothing could be read.
  inline std::optional<Opened> openUpright(const std::string &realPath, const Orient &orient,
                                           bool want8, std::string *errorOut = nullptr)
  {
    if (pngIsTruncated(realPath))
    {
      if (errorOut)
        *errorOut = kTruncatedMessage;
      return std::nullopt;
    }
    if (depthOf(realPath) == Depth::Wide)
    {
      std::string wideError;
      if (auto w = decodeImage16(realPath, &wideError))
      {
        Opened o;
        o.wide = orientImage(std::make_shared<Image16>(std::move(*w)), orient);
        if (want8)
          o.image = std::make_shared<CanvasImage>(narrowImage(*o.wide));
        return o;
      }
      // fall through to the 8-bit decoder
    }
    std::string error;
    std::optional<CanvasImage> raw = liteui_image::decodeFile(realPath, &error);
    if (!raw)
    {
      if (errorOut)
        *errorOut = error;
      return std::nullopt;
    }
    Opened o;
    o.image = orientImage(std::make_shared<CanvasImage>(std::move(*raw)), orient);
    return o;
  }

  // The size the picture has once it is upright (a quarter turn swaps the sides). Today it decodes the
  // file; a header-only read for JPEG, TIFF or RAW belongs here. false with `*errorOut` on failure.
  inline bool uprightSize(const std::string &realPath, const Orient &orient, int *w, int *h,
                          std::string *errorOut = nullptr)
  {
    if (pngIsTruncated(realPath))
    {
      if (errorOut)
        *errorOut = kTruncatedMessage;
      return false;
    }
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
    if (orient.rot & 1)
      std::swap(*w, *h);
    return true;
  }

  // The picture a thumbnail is made from: always 8-bit (a Wide file's 8-bit decode is what the
  // filmstrip shows), and not turned: the thumbnail worker turns it with its own orientation function.
  inline std::optional<CanvasImage> decodeForThumb(const std::string &realPath, std::string *errorOut = nullptr)
  {
    if (pngIsTruncated(realPath))
    {
      if (errorOut)
        *errorOut = kTruncatedMessage;
      return std::nullopt;
    }
    return liteui_image::decodeFile(realPath, errorOut);
  }

} // namespace image_source