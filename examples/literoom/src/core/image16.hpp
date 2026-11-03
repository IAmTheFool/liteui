// image16.hpp - Image16: the 16-bit-per-channel RGBA picture the float pipeline starts from,
// and the row conversions between it, the 8-bit RGBA that liteui's CanvasImage holds, and
// the float "levels" (0..255) the pixel passes work in. No liteui, no editor state: plain
// buffers, so it is tested without a window (tests/image16_test.cpp).
//
// Why 16 bits and not float for storage: a 24 MP picture is 192 MB at 16 bits and 384 MB as
// float, and the photo editor keeps several stages of it (original, oriented, rotated,
// source). 16 bits holds everything a 16-bit PNG / TIFF or a 12-14 bit RAW can say; the passes
// themselves compute in float, a block of rows at a time (render_core.hpp).
//
// Layout: R, G, B, A interleaved, straight (not premultiplied) alpha, row-major, no padding;
// the same as CanvasImage but with uint16_t samples. Alpha is 16-bit too (0 = transparent,
// 65535 = opaque).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/pixel_math.hpp"

struct Image16
{
  int width = 0, height = 0;
  std::vector<uint16_t> pixels; // width * height * 4 samples
  Image16() = default;
  Image16(int w, int h)
      : width(w < 0 ? 0 : w), height(h < 0 ? 0 : h),
        pixels(static_cast<size_t>(w < 0 ? 0 : w) * static_cast<size_t>(h < 0 ? 0 : h) * 4, 0)
  {
  }
  bool empty() const { return width <= 0 || height <= 0; }
  size_t pixelCount() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
};

// ---- 8-bit <-> 16-bit ----
// v * 257 spreads 0..255 over 0..65535 exactly (255 * 257 = 65535).
inline uint16_t widen8(uint8_t v) { return static_cast<uint16_t>(v * 257u); }
// Nearest 8-bit value (halfway rounds up); widen8 followed by narrow16 is the identity.
inline uint8_t narrow16(uint16_t v) { return static_cast<uint8_t>((v + 128u) / 257u); }

// `n` samples (not pixels).
inline void widenSamples(const uint8_t *src, uint16_t *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = widen8(src[i]);
}
inline void narrowSamples(const uint16_t *src, uint8_t *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = narrow16(src[i]);
}

// ---- samples <-> float levels (0..255) ----
// Loads: exact for 8-bit data and for the widened form of it (see kU16PerLevel).
inline void samplesToLevels(const uint8_t *src, float *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = static_cast<float>(src[i]);
}
inline void samplesToLevels(const uint16_t *src, float *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = u16ToLevels(src[i]);
}
// Stores: round to nearest and clamp to the sample's range (NaN -> 0).
inline void levelsToSamples(const float *src, uint8_t *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = toU8Clamped(src[i]);
}
inline void levelsToSamples(const float *src, uint16_t *dst, size_t n)
{
  for (size_t i = 0; i < n; ++i)
    dst[i] = toU16Clamped(src[i]);
}
