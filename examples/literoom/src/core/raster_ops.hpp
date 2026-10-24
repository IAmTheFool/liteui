// raster_ops.hpp - Pixel-buffer operations on a CanvasImage (RGBA8): crop, rotate by
// quarter turns, mirror, orient, straighten by any angle, the box
// downsample behind the interactive preview, and the area-average resize used
// by Export's "long edge" option. Needs liteui.hpp only for the CanvasImage
// struct.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/geometry.hpp"
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

// ---- Resize (Export: "Resize to long edge") ----

// The size `w` x `h` becomes when scaled so its long side is `longEdge`, with
// the aspect ratio kept (the short side is rounded, and is at least 1). It
// never enlarges: a picture whose long side is already `longEdge` or less, a
// `longEdge` below 1 and a non-positive size all come back unchanged.
inline Dims fitLongEdge(int w, int h, int longEdge)
{
  Dims d{};
  d.w = w;
  d.h = h;
  const int longSide = std::max(w, h);
  if (w < 1 || h < 1 || longEdge < 1 || longSide <= longEdge)
    return d;
  const double s = static_cast<double>(longEdge) / longSide;
  if (w >= h)
  {
    d.w = longEdge;
    d.h = std::max(1, static_cast<int>(std::lround(h * s)));
  }
  else
  {
    d.h = longEdge;
    d.w = std::max(1, static_cast<int>(std::lround(w * s)));
  }
  return d;
}

// `src` resampled to dw x dh by area averaging: every output pixel is the
// average of the part of the source it covers, with fractional weights at the
// edges, so any ratio works and nothing aliases. That is the right filter for
// shrinking; enlarging works but looks blocky. Color is averaged weighted by
// alpha (like rotateArbitrary), so a transparent corner never darkens its
// neighbors. A fully opaque, flat area keeps its exact value.
// An invalid size (dw or dh below 1, or an empty `src`) returns a copy of `src`.
inline std::shared_ptr<CanvasImage> resizeArea(const CanvasImage &src, int dw,
                                               int dh)
{
  const int sw = src.width, sh = src.height;
  if (dw < 1 || dh < 1 || sw < 1 || sh < 1)
    return std::make_shared<CanvasImage>(src);
  if (dw == sw && dh == sh)
    return std::make_shared<CanvasImage>(src);
  auto out = std::make_shared<CanvasImage>(dw, dh);
  const double xr = static_cast<double>(sw) / dw;
  const double yr = static_cast<double>(sh) / dh;
  // One source row's worth of alpha-weighted sums: (c0 a, c1 a, c2 a, a) per column.
  std::vector<double> acc(static_cast<size_t>(sw) * 4);
  for (int dy = 0; dy < dh; ++dy)
  {
    const double sy0 = dy * yr;
    const double sy1 = std::min(static_cast<double>(sh), (dy + 1) * yr);
    const int iy0 = static_cast<int>(std::floor(sy0));
    const int iy1 = std::min(sh, static_cast<int>(std::ceil(sy1)));
    std::fill(acc.begin(), acc.end(), 0.0);
    double wyTotal = 0.0;
    for (int iy = iy0; iy < iy1; ++iy)
    {
      const double wy = std::min(iy + 1.0, sy1) - std::max(static_cast<double>(iy), sy0);
      if (wy <= 0.0)
        continue;
      wyTotal += wy;
      const uint8_t *p = &src.pixels[static_cast<size_t>(iy) * sw * 4];
      double *a = acc.data();
      for (int x = 0; x < sw; ++x, p += 4, a += 4)
      {
        const double wa = wy * p[3];
        a[0] += p[0] * wa;
        a[1] += p[1] * wa;
        a[2] += p[2] * wa;
        a[3] += wa;
      }
    }
    uint8_t *o = &out->pixels[static_cast<size_t>(dy) * dw * 4];
    for (int dx = 0; dx < dw; ++dx, o += 4)
    {
      const double sx0 = dx * xr;
      const double sx1 = std::min(static_cast<double>(sw), (dx + 1) * xr);
      const int ix0 = static_cast<int>(std::floor(sx0));
      const int ix1 = std::min(sw, static_cast<int>(std::ceil(sx1)));
      double s0 = 0, s1 = 0, s2 = 0, sa = 0, wxTotal = 0;
      for (int ix = ix0; ix < ix1; ++ix)
      {
        const double wx = std::min(ix + 1.0, sx1) - std::max(static_cast<double>(ix), sx0);
        if (wx <= 0.0)
          continue;
        wxTotal += wx;
        const double *a = &acc[static_cast<size_t>(ix) * 4];
        s0 += a[0] * wx;
        s1 += a[1] * wx;
        s2 += a[2] * wx;
        sa += a[3] * wx;
      }
      const double area = wxTotal * wyTotal;
      if (sa <= 0.0 || area <= 0.0)
      { // nothing visible under this pixel
        o[0] = o[1] = o[2] = o[3] = 0;
        continue;
      }
      o[0] = static_cast<uint8_t>(std::clamp(s0 / sa + 0.5, 0.0, 255.0));
      o[1] = static_cast<uint8_t>(std::clamp(s1 / sa + 0.5, 0.0, 255.0));
      o[2] = static_cast<uint8_t>(std::clamp(s2 / sa + 0.5, 0.0, 255.0));
      o[3] = static_cast<uint8_t>(std::clamp(sa / area + 0.5, 0.0, 255.0));
    }
  }
  return out;
}

// `src` scaled so its long side is `longEdge` (see fitLongEdge). When nothing
// changes (already small enough, or `longEdge` < 1) it returns `src` itself, so
// a picture that needs no resize costs no copy.
inline std::shared_ptr<CanvasImage>
resizeToLongEdge(const std::shared_ptr<CanvasImage> &src, int longEdge)
{
  if (!src)
    return src;
  const Dims d = fitLongEdge(src->width, src->height, longEdge);
  if (d.w == src->width && d.h == src->height)
    return src;
  return resizeArea(*src, d.w, d.h);
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