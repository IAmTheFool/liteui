// image16_convert.hpp - CanvasImage (RGBA8, liteui) <-> Image16 (RGBA 16-bit), and the pictures -> liteui's
// ImageBuffer, which is what liteui_image::encodePng / encodeJpeg take.
// Needs liteui.hpp only for the CanvasImage / ImageBuffer structs (declarations, no LITEUI_IMPLEMENTATION): no
// platform code, so any translation unit may include it. The codecs (io/image_codec16.hpp) pull in the platform
// libraries and need LITEUI_IMPLEMENTATION.
#pragma once
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "core/image16.hpp"
#include "liteui.hpp"

// 8 -> 16 bit: every sample v becomes v * 257, so narrowImage(widenImage(x)) == x exactly.
inline Image16 widenImage(const CanvasImage &src)
{
  Image16 out(src.width, src.height);
  widenSamples(src.pixels.data(), out.pixels.data(), std::min(out.pixels.size(), src.pixels.size()));
  return out;
}
// 16 -> 8 bit: the nearest byte.
inline CanvasImage narrowImage(const Image16 &src)
{
  CanvasImage out(src.width, src.height);
  narrowSamples(src.pixels.data(), out.pixels.data(), std::min(out.pixels.size(), src.pixels.size()));
  return out;
}

// ---- Pictures -> liteui's ImageBuffer, for liteui_image::encodePng / encodeJpegMemory ----
// The encoders take an ImageBuffer (tightly packed, RGBA8_UNORM or RGBA16_UNORM, straight alpha, the
// same sample layout as CanvasImage / Image16), so an export copies the picture into one first.
// nullopt for an empty picture, one whose pixel vector is shorter than width * height * 4 samples, or
// when the copy can't be allocated.
inline std::optional<ImageBuffer> toImageBuffer(const CanvasImage &src)
{
  if (src.width <= 0 || src.height <= 0 ||
      src.pixels.size() < static_cast<size_t>(src.width) * static_cast<size_t>(src.height) * 4)
    return std::nullopt;
  auto buf = ImageBuffer::create(static_cast<uint32_t>(src.width), static_cast<uint32_t>(src.height),
                                 PixelFormat::RGBA8_UNORM);
  if (buf)
    std::memcpy(buf->pixels.data(), src.pixels.data(), buf->pixels.size());
  return buf;
}
inline std::optional<ImageBuffer> toImageBuffer(const Image16 &src)
{
  if (src.empty() || src.pixels.size() < src.pixelCount() * 4)
    return std::nullopt;
  auto buf = ImageBuffer::create(static_cast<uint32_t>(src.width), static_cast<uint32_t>(src.height),
                                 PixelFormat::RGBA16_UNORM);
  if (buf) // samples are host-order uint16, the layout RGBA16_UNORM stores
    std::memcpy(buf->pixels.data(), src.pixels.data(), buf->pixels.size());
  return buf;
}

// How Literoom asks liteui to write a PNG: zlib level 3 (liteui's default: on a 24 MP 16-bit picture level 6
// took about 6x as long for a file about 1.4 % smaller) and always RGBA, whatever the alpha holds, so
// an export has the same channels as the picture it came from (liteui would write RGB for a picture whose
// alpha is everywhere opaque).
inline liteui_image::PngEncodeOptions exportPngOptions()
{
  liteui_image::PngEncodeOptions o;
  o.compression = 3;
  o.dropOpaqueAlpha = false;
  return o;
}