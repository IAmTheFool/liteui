// render.hpp - The pixel pass: tone tables -> color pass -> neighbor effects -> masks
// -> vignette and grain, in blocks of rows that a background render can abandon. Takes liteui's
// CanvasImage, so this is the one header of the render chain that needs it.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "liteui.hpp"

#include "color/color_pass.hpp"
#include "core/pixel_math.hpp"
#include "detail/detail_layer.hpp"
#include "detail/film_fx.hpp"
#include "detail/neighbor_fx.hpp"
#include "edit/adjustment_set.hpp"
#include "edit/render_params.hpp"
#include "mask/mask.hpp"

// Applies one mask to the rows held in `q` (`n` bytes = whole rows, the first
// of which is image row `y0`). Per row: find the span the shape touches, copy
// it to `scratch`, run the mask's own LUT + color pass on the copy, and blend
// the result back by the weights. `scratch` holds one row of RGBA, `wbuf` one
// row of weights, `wbuf2` a second one for combining parts.
// The mask's neighbor effects (Texture, Clarity, Sharpness, Dehaze, Noise
// Reduction, Blur: neighbor_fx.hpp) are part of "the mask's own adjustment":
// applied to `scratch` after the color pass from the whole-image planes `dp`
// (so they have all the neighbors they need, whatever the span), then blended
// by weight like everything else. `ref` holds the SOURCE pixels of the same
// rows as `q`; the effects measure themselves against those.
inline void applyMaskToBlock(const MaskPass &mp, const MaskComposite &ev,
                             uint8_t *q, size_t n, int y0, int W,
                             uint8_t *scratch, float *wbuf, float *wbuf2,
                             const uint8_t *ref,
                             const neighbor_fx::Planes &dp)
{
  const size_t rowBytes = static_cast<size_t>(W) * 4;
  const int rows = static_cast<int>(n / rowBytes);
  const uint8_t *lr = mp.luts[0].data();
  const uint8_t *lg = mp.luts[1].data();
  const uint8_t *lb = mp.luts[2].data();
  for (int r = 0; r < rows; ++r)
  {
    int x0 = 0, x1 = W;
    // Inverted masks affect the whole row (weight = 1 - w), so no span cut.
    if (!mp.invert && !ev.rowSpan(y0 + r, W, x0, x1))
      continue;
    const int len = x1 - x0;
    if (len <= 0)
      continue;
    uint8_t *row = q + static_cast<size_t>(r) * rowBytes +
                   static_cast<size_t>(x0) * 4;
    // The weights see the row BEFORE this mask edits it, so a mask's own
    // adjustment never changes what it selects.
    ev.rowWeights(y0 + r, x0, x1, wbuf, row, wbuf2);
    const size_t bytes = static_cast<size_t>(len) * 4;
    std::copy_n(row, bytes, scratch);
    if (mp.tone)
      for (size_t i = 0; i + 3 < bytes; i += 4)
      {
        scratch[i] = lr[scratch[i]];
        scratch[i + 1] = lg[scratch[i + 1]];
        scratch[i + 2] = lb[scratch[i + 2]];
      }
    applyColorPass(mp.color, scratch, bytes);
    if (mp.fx.any())
      neighbor_fx::apply(
          dp, mp.fx,
          ref + static_cast<size_t>(r) * rowBytes + static_cast<size_t>(x0) * 4,
          scratch, static_cast<size_t>(len),
          static_cast<size_t>(y0 + r) * static_cast<size_t>(W) +
              static_cast<size_t>(x0),
          W);
    for (int i = 0; i < len; ++i)
    {
      float w = wbuf[i];
      if (mp.invert)
        w = 1.0f - w;
      w *= mp.amount;
      if (w <= 0.0f)
        continue;
      uint8_t *p = row + static_cast<size_t>(i) * 4;
      const uint8_t *t = scratch + static_cast<size_t>(i) * 4;
      for (int k = 0; k < 3; ++k)
      {
        const float a = static_cast<float>(p[k]);
        const float b = static_cast<float>(t[k]);
        p[k] = toU8Clamped(a + (b - a) * w);
      }
    }
  }
}

// Runs the whole per-pixel pipeline on a copy of `src` (which is never
// modified). Works in blocks of rows so that a background render can be
// abandoned quickly: when `latest` is given, it is re-read before every
// block, and if it no longer equals `gen` (a newer request or an
// invalidate() superseded this one) the function stops and returns null.
// With no `latest` it always runs to completion, which is the synchronous
// path. Each pixel is independent, so blocking does not change the result.
//
// Order per pixel: tone LUTs -> color pass -> neighbor effects (Dehaze,
// Texture / Clarity / Sharpness, Noise Reduction, Blur) -> masks, each mask
// doing the same three steps on its own copy -> Vignette and Grain (film_fx.hpp,
// global only, on the finished pixel).
// Vignette and Grain depend on the pixel's position in THIS picture, so they are
// handed the index of the block's first pixel; a block of rows therefore gives the
// same bytes as the whole picture at once, and the preview, the background render
// and the batch export agree (the preview picture is smaller, so its grain is the
// same kind but not the same pixels).
// The detail layers need neighbors, which the 128-row blocks don't have, so
// they come from whole-image blurred planes built once per picture (see
// detail_layer.hpp) instead of from halo rows; `src` stays untouched, which
// is what keeps those planes valid.
//
// Raw pointers and no std:: helpers in the inner loops, same Debug-build
// reason as applyColorPass.
inline std::shared_ptr<CanvasImage>
renderWith(const CanvasImage &src, const RenderParams &rp,
           const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
{
  auto img = std::make_shared<CanvasImage>(src);
  uint8_t *p = img->pixels.data();
  const size_t total = img->pixels.size();
  const size_t rowBytes = static_cast<size_t>(img->width) * 4;
  constexpr size_t kBlockRows = 128;
  const size_t blockBytes = rowBytes * kBlockRows;
  const bool sharedLut = rp.luts[0] == rp.luts[1] && rp.luts[1] == rp.luts[2];
  const uint8_t *lr = rp.luts[0].data();
  const uint8_t *lg = rp.luts[1].data();
  const uint8_t *lb = rp.luts[2].data();

  // Masks are resolved against THIS image's size (the preview is smaller
  // than the full picture; the shapes are normalized so both agree).
  std::vector<MaskComposite> maskEvals;
  std::vector<uint8_t> maskScratch;
  std::vector<float> maskWeights, maskWeights2;
  if (!rp.masks.empty() && img->width > 0)
  {
    maskEvals.reserve(rp.masks.size());
    for (const MaskPass &m : rp.masks)
      maskEvals.push_back(prepareComposite(m.parts, img->width, img->height));
    maskScratch.resize(rowBytes);
    maskWeights.resize(static_cast<size_t>(img->width));
    maskWeights2.resize(static_cast<size_t>(img->width));
  }

  // Neighbor effects: fetch (or build, the first time) the planes this render
  // needs. A cancelled build means this render is superseded.
  neighbor_fx::Planes fxPlanes;
  uint32_t fxNeed = rp.fx.need();
  for (const MaskPass &m : rp.masks)
    fxNeed |= m.fx.need(); // masks use the same planes
  if (fxNeed != 0 &&
      !neighbor_fx::acquire(fxPlanes, src.pixels.data(), src.width, src.height,
                            fxNeed, latest, gen))
    return nullptr;

  // Vignette and Grain are resolved against THIS picture's size, like the masks.
  const film_fx::Prepared film = film_fx::prepare(rp.film, img->width, img->height);

  for (size_t off = 0; off < total; off += blockBytes)
  {
    if (latest && latest->load(std::memory_order_relaxed) != gen)
      return nullptr; // superseded: abandon
    const size_t n = total - off < blockBytes ? total - off : blockBytes;
    uint8_t *q = p + off;
    if (sharedLut)
    { // fast path (no per-channel curves): one shared table
      for (size_t i = 0; i + 3 < n; i += 4)
      {
        q[i] = lr[q[i]];
        q[i + 1] = lr[q[i + 1]];
        q[i + 2] = lr[q[i + 2]];
      }
    }
    else
    {
      for (size_t i = 0; i + 3 < n; i += 4)
      {
        q[i] = lr[q[i]];
        q[i + 1] = lg[q[i + 1]];
        q[i + 2] = lb[q[i + 2]];
      }
    }
    applyColorPass(rp.color, q, n); // no-op when all four color sliders are 0
    if (rp.fx.any()) // blocks are whole rows, so pixels are contiguous
      neighbor_fx::apply(fxPlanes, rp.fx, src.pixels.data() + off, q, n / 4,
                         off / 4, img->width);
    for (size_t k = 0; k < maskEvals.size(); ++k)
      applyMaskToBlock(rp.masks[k], maskEvals[k], q, n,
                       static_cast<int>(off / rowBytes), img->width,
                       maskScratch.data(), maskWeights.data(),
                       maskWeights2.data(), src.pixels.data() + off,
                       fxPlanes);
    if (film.any()) // last: after the masks, on whole rows (off / 4 = first pixel)
      film_fx::apply(film, q, n / 4, off / 4);
  }
  return img;
}

// Renders a copy of `original` with the adjustments in `set`.
// Non-destructive: always starts from `original`'s untouched pixels, so
// re-running after any value change never compounds onto a prior result.
// Synchronous; the full-resolution path for big images goes through
// RenderWorker instead (see updateImageFromAdjustments).
inline std::shared_ptr<CanvasImage>
renderAdjusted(const AdjustmentSet &set, const CanvasImage &original)
{
  return renderWith(original, set.buildRenderParams());
}