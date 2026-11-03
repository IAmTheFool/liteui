// image16_convert.hpp - CanvasImage (RGBA8, liteui) <-> Image16 (RGBA 16-bit). Needs liteui.hpp only for the
// CanvasImage struct (declarations, no LITEUI_IMPLEMENTATION): no platform code, so any translation unit may
// include it. The codecs (io/image_codec16.hpp) pull in the platform libraries and need LITEUI_IMPLEMENTATION.
#pragma once
#include <algorithm>

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
