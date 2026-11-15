// image_codec16.hpp - Reading any picture as a 16-bit one (Image16), through liteui's decoders (which is why this
// header needs liteui.hpp, and a translation unit that includes it must define LITEUI_IMPLEMENTATION before
// liteui.hpp).
//   decodeImage16     any picture liteui can read -> Image16. liteui's decodeFileInto does the reading
//                     (content-sniffed, PNG chunks validated, a 16-bit PNG kept at full precision and written
//                     straight into the Image16); an 8-bit picture (JPEG, 8-bit PNG, ...) comes back widened
//                     (v * 257), so it is exactly the 8-bit picture the editor shows
//   (widenImage / narrowImage, CanvasImage <-> Image16, live in core/image16_convert.hpp, which needs no
//   platform code.)
//
// There is no encoder here any more. Literoom's own PNG and JPEG encoders (libpng / libjpeg / WIC) are gone:
// a picture is written with liteui_image::encodePng / encodeJpegMemory, after toImageBuffer and with
// exportPngOptions() (core/image16_convert.hpp). TIFF and RAW would be more branches of decodeImage16 (they
// need libtiff / LibRaw, which Literoom does not link yet).
#pragma once
#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/image16.hpp"
#include "core/image16_convert.hpp" // widenImage / narrowImage (no platform code)
#include "io/file_utils.hpp"
#include "liteui.hpp"

// ---------------- Reading any picture as Image16 ----------------
// A 16-bit PNG at full precision; anything else widened (so the 16-bit pipeline starts from exactly the pixels
// the editor shows for it). `errorOut` says what went wrong.
//
// liteui_image::decodeFileInto decodes straight into the Image16's own samples (a 16-bit PNG: they are RGBA16 in
// host byte order, exactly Image16's layout), so the picture exists once while it is read, not twice. A file of
// 8 bits is decoded into a CanvasImage and widened (v * 257), as it always was.
inline std::optional<Image16> decodeImage16(const std::string &path, std::string *errorOut = nullptr)
{
  std::optional<Image16> wide;
  std::optional<CanvasImage> narrow;
  bool tooLarge = false;
  auto alloc = [&](uint32_t w, uint32_t h, PixelFormat f) -> std::byte *
  {
    if (w > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        h > static_cast<uint32_t>(std::numeric_limits<int>::max()))
    {
      tooLarge = true;
      return nullptr;
    }
    try
    {
      if (f == PixelFormat::RGBA16_UNORM)
      {
        wide.emplace(static_cast<int>(w), static_cast<int>(h));
        return reinterpret_cast<std::byte *>(wide->pixels.data());
      }
      if (f == PixelFormat::RGBA8_UNORM)
      {
        narrow.emplace(static_cast<int>(w), static_cast<int>(h));
        return reinterpret_cast<std::byte *>(narrow->pixels.data());
      }
    }
    catch (const std::exception &)
    {
    }
    return nullptr;
  };
  if (!liteui_image::decodeFileInto(path, alloc, errorOut))
  {
    if (tooLarge && errorOut)
      *errorOut = "image is too large";
    return std::nullopt;
  }
  if (wide)
    return wide;
  if (narrow)
    return widenImage(*narrow);
  if (errorOut)
    *errorOut = "decoder returned no image";
  return std::nullopt;
}