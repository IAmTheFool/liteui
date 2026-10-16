// raster_ops.hpp - Pixel-buffer operations on a CanvasImage (RGBA8): crop, rotate by
// quarter turns, mirror, orient, straighten by any angle, and the box
// downsample behind the interactive preview. Needs liteui.hpp only for
// the CanvasImage struct.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>

#include "geometry.hpp"
#include "liteui.hpp"

// Interactive-preview sizing (see EditorState::previewBase): the preview's
// long side is at most kPreviewMaxEdge px, and images whose long side is
// within kPreviewMaxEdge * kPreviewMinRatio skip the preview entirely.
constexpr int kPreviewMaxEdge = 1024;
constexpr float kPreviewMinRatio = 1.5f;

// True for photos big enough that a full-resolution geometry rebuild
// (orient -> straighten -> crop -> render) is worth moving off the UI
// thread. Mirrors makePreviewBase's own threshold, so "this photo is large"
// means the same thing everywhere in the file. Gated on `original` (the
// decoded file), not `source` (today's cropped/rotated working image), so
// a single editing session for one big photo always takes the same path —
// it doesn't flip to synchronous just because a crop happened to shrink
// `source` below the threshold.
inline bool isLargeImage(const CanvasImage &img)
{
  return std::max(img.width, img.height) >
         static_cast<int>(kPreviewMaxEdge * kPreviewMinRatio);
}

// Box-downsamples `src` by the smallest integer factor that brings its
// long side to at most kPreviewMaxEdge. Returns null when `src` is already
// within kPreviewMaxEdge * kPreviewMinRatio — below that, a preview
// wouldn't be meaningfully cheaper than the full image, so the drag path
// just uses the full-resolution one. Runs once per opened image.
inline std::shared_ptr<CanvasImage> makePreviewBase(const CanvasImage &src)
{
  if (!isLargeImage(src))
    return nullptr;
  int longEdge = std::max(src.width, src.height);
  int k = (longEdge + kPreviewMaxEdge - 1) / kPreviewMaxEdge; // ceil
  int w = src.width / k, h = src.height / k;
  if (w <= 0 || h <= 0)
    return nullptr;
  auto out = std::make_shared<CanvasImage>(w, h);
  const int area = k * k;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      int sum[4] = {0, 0, 0, 0};
      for (int dy = 0; dy < k; ++dy)
      {
        const uint8_t *row =
            &src.pixels[(static_cast<size_t>(y * k + dy) * src.width + x * k) *
                        4];
        for (int dx = 0; dx < k; ++dx)
          for (int c = 0; c < 4; ++c)
            sum[c] += row[dx * 4 + c];
      }
      uint8_t *o = &out->pixels[(static_cast<size_t>(y) * w + x) * 4];
      for (int c = 0; c < 4; ++c)
        o[c] = static_cast<uint8_t>((sum[c] + area / 2) / area);
    }
  return out;
}

// Copies the `r` region of `src` into a new image (clamped to src's bounds).
inline std::shared_ptr<CanvasImage> cropImage(const CanvasImage &src,
                                              const CropRect &r)
{
  const int x0 = std::clamp(r.x, 0, src.width);
  const int y0 = std::clamp(r.y, 0, src.height);
  const int x1 = std::clamp(r.x + r.w, x0, src.width);
  const int y1 = std::clamp(r.y + r.h, y0, src.height);
  const int w = x1 - x0, h = y1 - y0;
  auto out = std::make_shared<CanvasImage>(w, h);
  for (int y = 0; y < h; ++y)
  {
    const uint8_t *from =
        src.pixels.data() + (static_cast<size_t>(y0 + y) * src.width + x0) * 4;
    std::copy_n(from, static_cast<size_t>(w) * 4,
                out->pixels.data() + static_cast<size_t>(y) * w * 4);
  }
  return out;
}

// Returns `src` rotated 90 degrees clockwise (w x h becomes h x w).
inline std::shared_ptr<CanvasImage> rotate90cw(const CanvasImage &src)
{
  const int w = src.width, h = src.height;
  auto out = std::make_shared<CanvasImage>(h, w);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      // Source (x, y) lands at column h-1-y, row x of the result.
      const uint8_t *from = &src.pixels[(static_cast<size_t>(y) * w + x) * 4];
      uint8_t *to =
          &out->pixels[(static_cast<size_t>(x) * h + (h - 1 - y)) * 4];
      std::copy_n(from, 4, to);
    }
  return out;
}

// Returns `src` mirrored left-right.
inline std::shared_ptr<CanvasImage> flipHorizontal(const CanvasImage &src)
{
  const int w = src.width, h = src.height;
  auto out = std::make_shared<CanvasImage>(w, h);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      std::copy_n(&src.pixels[(static_cast<size_t>(y) * w + x) * 4], 4,
                  &out->pixels[(static_cast<size_t>(y) * w + (w - 1 - x)) * 4]);
  return out;
}

// Applies `o` to `src`. The identity orientation returns `src` itself (no
// copy), so an unrotated image costs nothing extra.
inline std::shared_ptr<CanvasImage>
orientImage(const std::shared_ptr<CanvasImage> &src, const Orient &o)
{
  if (o.identity())
    return src;
  std::shared_ptr<CanvasImage> cur = src;
  if (o.flip)
    cur = flipHorizontal(*cur);
  for (int i = 0; i < o.rot; ++i)
    cur = rotate90cw(*cur);
  return cur;
}

// ---- Arbitrary-angle rotation ("straighten") ----

// Rotates `src` clockwise by `deg` degrees about its center into a canvas of
// rotatedDims() size, with bilinear resampling (in premultiplied alpha, so
// pictures with transparency blend correctly).
//   fit:    every output pixel lands inside the picture; samples that graze
//           the edge just repeat the edge pixel.
//   expand: the empty corners come out fully transparent, and the picture's
//           rotated edge is anti-aliased by a one-pixel coverage falloff.
// `latest`/`gen`: same cancellation contract as renderWith (see there) — the
// bilinear resample is the most expensive stage of an async geometry
// rebuild (GeometryWorker), so a straighten superseded by a further drag or
// button click abandons within a handful of scanlines instead of finishing
// unwanted work. Checked every kRotateCheckRows rows rather than every row:
// the check itself is cheap, but so is one scanline, so batching it keeps
// the overhead negligible either way.
inline std::shared_ptr<CanvasImage>
rotateArbitrary(const CanvasImage &src, float deg, bool expand,
                const std::atomic<uint64_t> *latest = nullptr,
                uint64_t gen = 0)
{
  if (deg == 0.0f)
    return std::make_shared<CanvasImage>(src);
  constexpr int kRotateCheckRows = 64;
  const Dims d = rotatedDims(src.width, src.height, deg, expand);
  auto out = std::make_shared<CanvasImage>(d.w, d.h);
  const double rad = static_cast<double>(deg) * kDegToRad;
  const double cs = std::cos(rad), sn = std::sin(rad);
  const int sw = src.width, sh = src.height;
  const double scx = sw / 2.0, scy = sh / 2.0;   // source center
  const double dcx = d.w / 2.0, dcy = d.h / 2.0; // output center
  const uint8_t *sp = src.pixels.data();
  uint8_t *op = out->pixels.data();
  auto clampi = [](int v, int hi)
  { return v < 0 ? 0 : (v > hi ? hi : v); };

  for (int y = 0; y < d.h; ++y)
  {
    if (latest && (y % kRotateCheckRows) == 0 &&
        latest->load(std::memory_order_relaxed) != gen)
      return nullptr; // superseded: abandon
    const double oy = y + 0.5 - dcy;
    for (int x = 0; x < d.w; ++x)
    {
      const double ox = x + 0.5 - dcx;
      // Inverse of a clockwise rotation (y points down): where in the
      // source does this output pixel's center come from?
      const double sx = ox * cs + oy * sn + scx;
      const double sy = -ox * sn + oy * cs + scy;

      float cov = 1.0f;
      if (expand)
      {
        const double edge =
            std::min(std::min(sx, sw - sx), std::min(sy, sh - sy));
        if (edge <= -0.5)
          continue; // fully outside the picture: stays transparent
        cov = static_cast<float>(std::min(1.0, edge + 0.5));
      }

      const double u = sx - 0.5, v = sy - 0.5; // pixel centers sit at +0.5
      const int x0 = static_cast<int>(std::floor(u));
      const int y0 = static_cast<int>(std::floor(v));
      const float fx = static_cast<float>(u - x0),
                  fy = static_cast<float>(v - y0);
      const int xa = clampi(x0, sw - 1), xb = clampi(x0 + 1, sw - 1);
      const int ya = clampi(y0, sh - 1), yb = clampi(y0 + 1, sh - 1);
      const uint8_t *p00 = sp + (static_cast<size_t>(ya) * sw + xa) * 4;
      const uint8_t *p10 = sp + (static_cast<size_t>(ya) * sw + xb) * 4;
      const uint8_t *p01 = sp + (static_cast<size_t>(yb) * sw + xa) * 4;
      const uint8_t *p11 = sp + (static_cast<size_t>(yb) * sw + xb) * 4;
      const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy);
      const float w01 = (1 - fx) * fy, w11 = fx * fy;
      // Alpha-weighted taps: transparent pixels contribute no color.
      const float a00 = p00[3] * w00, a10 = p10[3] * w10;
      const float a01 = p01[3] * w01, a11 = p11[3] * w11;
      const float A = a00 + a10 + a01 + a11;
      if (A <= 0.0f)
        continue;
      uint8_t *o = op + (static_cast<size_t>(y) * d.w + x) * 4;
      for (int ch = 0; ch < 3; ++ch)
      {
        const float c =
            (p00[ch] * a00 + p10[ch] * a10 + p01[ch] * a01 + p11[ch] * a11) / A;
        o[ch] = static_cast<uint8_t>(std::clamp(c + 0.5f, 0.0f, 255.0f));
      }
      o[3] = static_cast<uint8_t>(std::clamp(A * cov + 0.5f, 0.0f, 255.0f));
    }
  }
  return out;
}