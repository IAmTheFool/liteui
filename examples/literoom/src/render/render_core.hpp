// render_core.hpp - The pixel pass in float: tone tables -> color pass -> neighbor effects -> masks
// -> vignette and grain, in blocks of rows that a background render can abandon.
//
// A block of rows is loaded from the source (RGBA8 or RGBA 16-bit) into float "levels" (0..255, the
// unit every constant of the passes is written in), goes through every stage as floats, and is
// rounded ONCE, when it is stored into the result. Between the stages nothing is rounded: the
// byte pipeline (render_bytes.hpp, the old code) rounded to a byte after the tone sliders, the
// color pass, every neighbor pass, every mask and the film effects. Where the old code clamped to
// 0..255 so does this (after the color pass, inside every neighbor pass, after each mask), so
// highlights are not allowed to run past white between stages: that decision can be revisited
// stage by stage later.
//
// For a picture that started as 8-bit the result is the old result, up to the rounding that is no
// longer done between stages: exactly equal when only one stage did anything (a single tone slider,
// the color pass alone, one neighbor effect, the film effects), and within a few levels when
// stages are stacked. A 16-bit picture keeps its extra precision through every stage.
//
// Nothing the picture is measured with is rounded to 8 bits any more:
//   * the neighbor effects' planes and the source pixels they compare against are read from the
//     SOURCE at its own precision (RGBA8 or 16-bit: see neighbor_fx.hpp and SrcTraits),
//   * a Luminosity / Color Range mask picks its weights from the row as it stands in float levels
//     (see MaskComposite::rowWeightsF).
//
// Each pixel is independent, so blocking does not change the result: the preview, the background
// render and the batch export agree (see the tests).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "liteui.hpp"

#include "color/color_pass.hpp"
#include "core/image16.hpp"
#include "core/pixel_math.hpp"
#include "core/tone_table.hpp"
#include "detail/detail_layer.hpp"
#include "detail/film_fx.hpp"
#include "detail/neighbor_fx.hpp"
#include "edit/render_params.hpp"
#include "mask/mask.hpp"

namespace render_core
{

  // Rows per block (the same as the byte pipeline, so cancellation latency is unchanged).
  constexpr int kBlockRows = 128;

  // Clamps R, G and B of `n` floats (4 per pixel) to 0..255; NaN -> 0. Alpha is left alone. Plain
  // comparisons, no std:: helpers, for Debug-build speed like the other per-pixel loops.
  inline void clampRgb(float *p, size_t n)
  {
    for (size_t i = 0; i + 3 < n; i += 4)
      for (int c = 0; c < 3; ++c)
      {
        const float v = p[i + static_cast<size_t>(c)];
        p[i + static_cast<size_t>(c)] = !(v >= 0.0f) ? 0.0f : (v > 255.0f ? 255.0f : v);
      }
  }

  // The tone stage on `n` floats (4 per pixel): every R, G, B through its channel's table.
  inline void applyTone(const ToneSet &ts, float *p, size_t n)
  {
    const float *t0 = ts.at(0).y.data();
    const float *t1 = ts.at(1).y.data();
    const float *t2 = ts.at(2).y.data();
    for (size_t i = 0; i + 3 < n; i += 4)
    {
      p[i] = toneLookupWide(t0, p[i]);
      p[i + 1] = toneLookupWide(t1, p[i + 1]);
      p[i + 2] = toneLookupWide(t2, p[i + 2]);
    }
  }

  // The byte tables, for a RenderParams that has no float ones (built by hand): each value is
  // rounded to a byte first, exactly as the byte pipeline does.
  inline void applyByteLuts(const std::array<std::array<uint8_t, 256>, 3> &luts, float *p, size_t n)
  {
    for (size_t i = 0; i + 3 < n; i += 4)
      for (size_t c = 0; c < 3; ++c)
        p[i + c] = static_cast<float>(luts[c][toU8Clamped(p[i + c])]);
  }

  // Does any part of this mask pick its weights from the picture's own values?
  inline bool partsReadPixels(const std::vector<MaskPassPart> &parts)
  {
    for (const MaskPassPart &p : parts)
      if (maskTypeUsesPixels(static_cast<MaskType>(p.shape.index())))
        return true;
    return false;
  }

  // Applies one mask to the rows held in `q` (`rows` whole rows of floats, the first of which is
  // image row `y0`). Per row: find the span the shape touches, copy it to `scratch`, run the mask's
  // own tone + color pass + neighbor effects on the copy, and blend the result back by the weights.
  // `scratch` holds one row of RGBA floats, `wbuf` / `wbuf2` one row of weights each. A mask that
  // reads the picture (`readsPixels`) is shown the row itself, as float levels. `ref` holds the
  // SOURCE samples (RGBA8 or 16-bit, the type the planes in `dp` were built from) of the same rows
  // (null when no effect needs them). See applyMaskToBlockBytes for the old, byte version.
  template <class S>
  inline void applyMaskToBlock(const MaskPass &mp, const MaskComposite &ev, bool readsPixels,
                               float *q, int rows, int y0, int W, float *scratch, float *wbuf,
                               float *wbuf2, const S *ref, const neighbor_fx::Planes &dp)
  {
    const size_t rowFloats = static_cast<size_t>(W) * 4;
    for (int r = 0; r < rows; ++r)
    {
      int x0 = 0, x1 = W;
      // Inverted masks affect the whole row (weight = 1 - w), so no span cut.
      if (!mp.invert && !ev.rowSpan(y0 + r, W, x0, x1))
        continue;
      const int len = x1 - x0;
      if (len <= 0)
        continue;
      float *row = q + static_cast<size_t>(r) * rowFloats + static_cast<size_t>(x0) * 4;
      const size_t n = static_cast<size_t>(len) * 4;
      // The weights see the row BEFORE this mask edits it, so a mask's own adjustment never
      // changes what it selects. A Luminosity / Color Range mask reads it as float levels.
      ev.rowWeightsF(y0 + r, x0, x1, wbuf, readsPixels ? row : nullptr, wbuf2);
      std::copy_n(row, n, scratch);
      if (mp.tone)
      {
        if (mp.toneF)
          applyTone(*mp.toneF, scratch, n);
        else
          applyByteLuts(mp.luts, scratch, n);
      }
      applyColorPassF(mp.color, scratch, n);
      if (mp.color.active)
        clampRgb(scratch, n);
      if (mp.fx.any())
        neighbor_fx::apply(dp, mp.fx,
                           ref + static_cast<size_t>(r) * static_cast<size_t>(W) * 4 + static_cast<size_t>(x0) * 4,
                           scratch, static_cast<size_t>(len),
                           static_cast<size_t>(y0 + r) * static_cast<size_t>(W) + static_cast<size_t>(x0), W);
      for (int i = 0; i < len; ++i)
      {
        float w = wbuf[i];
        if (mp.invert)
          w = 1.0f - w;
        w *= mp.amount;
        if (w <= 0.0f)
          continue;
        float *p = row + static_cast<size_t>(i) * 4;
        const float *t = scratch + static_cast<size_t>(i) * 4;
        for (int k = 0; k < 3; ++k)
          p[k] += (t[k] - p[k]) * w;
      }
    }
  }

  // The whole pipeline. `SrcImg` is CanvasImage (RGBA8) or Image16; `OutImg` is what is produced
  // (CanvasImage for the screen, Image16 for a 16-bit export). `src` is never modified.
  //
  // `view8`: no longer used. The neighbor effects used to measure an 8-bit view of a 16-bit source;
  // they now read the source itself (their planes are cached per buffer address, so a source that
  // lives as long as the slider drag keeps them cached). The parameter stays so the callers that
  // pass a view keep compiling.
  //
  // `latest` / `gen`: when `latest` is given it is re-read before every block, and if it no longer
  // equals `gen` (a newer request superseded this one) the function stops and returns null. With
  // no `latest` it always runs to completion.
  template <class SrcImg, class OutImg>
  inline std::shared_ptr<OutImg>
  renderPipeline(const SrcImg &src, const CanvasImage *view8, const RenderParams &rp,
                 const std::atomic<uint64_t> *latest, uint64_t gen)
  {
    const int W = src.width, H = src.height;
    auto out = std::make_shared<OutImg>(W, H);
    if (W <= 0 || H <= 0 || src.pixels.size() < static_cast<size_t>(W) * static_cast<size_t>(H) * 4)
      return out;
    const size_t rowSamples = static_cast<size_t>(W) * 4;

    // Masks are resolved against THIS image's size (the preview is smaller than the full picture;
    // the shapes are normalized so both agree).
    std::vector<MaskComposite> maskEvals;
    std::vector<char> maskReads;
    std::vector<float> maskScratch, maskWeights, maskWeights2;
    if (!rp.masks.empty())
    {
      maskEvals.reserve(rp.masks.size());
      for (const MaskPass &m : rp.masks)
      {
        maskEvals.push_back(prepareComposite(m.parts, W, H));
        maskReads.push_back(partsReadPixels(m.parts) ? 1 : 0);
      }
      maskScratch.resize(rowSamples);
      maskWeights.resize(static_cast<size_t>(W));
      maskWeights2.resize(static_cast<size_t>(W));
    }

    // Neighbor effects: fetch (or build, the first time) the planes this render needs, from the
    // source itself (RGBA8 or 16-bit). A cancelled build means this render is superseded.
    neighbor_fx::Planes fxPlanes;
    uint32_t fxNeed = rp.fx.need();
    bool anyFx = rp.fx.any();
    for (const MaskPass &m : rp.masks)
    {
      fxNeed |= m.fx.need(); // masks use the same planes
      anyFx = anyFx || m.fx.any();
    }
    (void)view8;
    // The source's own samples (uint8_t for RGBA8, uint16_t for 16-bit) are what the effects measure.
    using Sample = typename std::decay_t<decltype(src.pixels)>::value_type;
    const Sample *ref = nullptr;
    if (anyFx) // (negative Dehaze is an effect that needs no planes but still gets the source pointer)
    {
      ref = src.pixels.data();
      if (fxNeed != 0 && !neighbor_fx::acquire(fxPlanes, ref, W, H, fxNeed, latest, gen))
        return nullptr;
    }

    // Vignette and Grain are resolved against THIS picture's size, like the masks.
    const film_fx::Prepared film = film_fx::prepare(rp.film, W, H);

    const bool floatTone = rp.toneF != nullptr;
    const bool toneOn = floatTone ? !rp.toneF->identity : true;
    std::vector<float> fb(rowSamples * static_cast<size_t>(std::min(H, kBlockRows)));

    for (int y = 0; y < H; y += kBlockRows)
    {
      if (latest && latest->load(std::memory_order_relaxed) != gen)
        return nullptr; // superseded: abandon
      const int rows = std::min(kBlockRows, H - y);
      const size_t n = static_cast<size_t>(rows) * rowSamples;
      const size_t off = static_cast<size_t>(y) * rowSamples;
      float *q = fb.data();
      samplesToLevels(src.pixels.data() + off, q, n);
      if (toneOn)
      {
        if (floatTone)
          applyTone(*rp.toneF, q, n);
        else
          applyByteLuts(rp.luts, q, n);
      }
      if (rp.color.active) // a no-op when all the color sliders are 0
      {
        applyColorPassF(rp.color, q, n);
        clampRgb(q, n);
      }
      if (rp.fx.any()) // blocks are whole rows, so pixels are contiguous
        neighbor_fx::apply(fxPlanes, rp.fx, ref + off, q, n / 4, off / 4, W);
      for (size_t k = 0; k < maskEvals.size(); ++k)
        applyMaskToBlock(rp.masks[k], maskEvals[k], maskReads[k] != 0, q, rows, y, W,
                         maskScratch.data(), maskWeights.data(), maskWeights2.data(),
                         ref ? ref + off : nullptr, fxPlanes);
      if (film.any()) // last: after the masks, on whole rows (off / 4 = first pixel)
        film_fx::applyF(film, q, n / 4, off / 4);
      levelsToSamples(q, out->pixels.data() + off, n);
    }
    return out;
  }

} // namespace render_core

// ---------------- Entry points ----------------

// Renders a picture to RGBA8 (what the screen shows). The signature the app, the workers and the
// golden test have always called: an 8-bit picture in, an 8-bit picture out, never modified.
inline std::shared_ptr<CanvasImage>
renderWith(const CanvasImage &src, const RenderParams &rp,
           const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
{
  return render_core::renderPipeline<CanvasImage, CanvasImage>(src, &src, rp, latest, gen);
}

// A 16-bit picture to RGBA8 for the screen. `view8` is not used any more (the neighbor effects read
// the 16-bit source itself; see renderPipeline) and may be null.
inline std::shared_ptr<CanvasImage>
renderToDisplay(const Image16 &src, const CanvasImage *view8, const RenderParams &rp,
                const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
{
  return render_core::renderPipeline<Image16, CanvasImage>(src, view8, rp, latest, gen);
}

// A 16-bit picture to 16 bits (for a 16-bit export): the same pipeline, rounded to 16 bits once.
inline std::shared_ptr<Image16>
renderToImage16(const Image16 &src, const CanvasImage *view8, const RenderParams &rp,
                const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
{
  return render_core::renderPipeline<Image16, Image16>(src, view8, rp, latest, gen);
}

// An RGBA8 picture to 16 bits: the same pipeline with the result rounded to 16 bits instead of 8, so a
// heavy edit of an 8-bit photo comes out as smooth gradients instead of re-quantized steps (a 16-bit
// PNG export).
inline std::shared_ptr<Image16>
renderToImage16(const CanvasImage &src, const RenderParams &rp,
                const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
{
  return render_core::renderPipeline<CanvasImage, Image16>(src, &src, rp, latest, gen);
}