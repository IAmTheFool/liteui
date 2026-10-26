// scopes.hpp - The two scopes that can replace the histogram in the pinned
// canvas above the panes: a WAVEFORM (every picture column drawn as a column
// of levels) and a VECTORSCOPE (every pixel's chroma drawn as a point on a
// Cb / Cr plane). Pure functions of an image: the editor's `state` is not
// touched, so they can be tested on small synthetic pictures. main.cpp's
// computeHistogram() picks the image on screen, calls render() and stores the
// picture where the canvas painter reads it; the canvas stretches it to its
// own size, so the result does not depend on how wide the sidebar is.
// Each scope is built in two steps (counting, then drawing) so the counts can
// be tested without looking at pixels. Samples the same regular grid as
// histogram::compute (about kHistMaxSamples pixels) and skips fully
// transparent pixels, so a scope never costs more than the histogram.
// Takes liteui's CanvasImage, hence liteui.hpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "liteui.hpp"
#include "render/histogram.hpp" // kHistMaxSamples

namespace scopes
{

  // What the pinned canvas shows. A click cycles through them in this order.
  enum class Kind : int
  {
    Histogram,
    Waveform,
    Vectorscope,
    Count
  };

  inline const char *kindName(Kind k)
  {
    switch (k)
    {
    case Kind::Histogram:
      return "Histogram";
    case Kind::Waveform:
      return "Waveform";
    case Kind::Vectorscope:
      return "Vectorscope";
    default:
      return "Histogram";
    }
  }

  // The next view in the cycle; anything out of range goes back to Histogram.
  inline Kind nextKind(Kind k)
  {
    const int i = static_cast<int>(k);
    if (i < 0 || i >= static_cast<int>(Kind::Count))
      return Kind::Histogram;
    return static_cast<Kind>((i + 1) % static_cast<int>(Kind::Count));
  }

  // Waveform: at most this many columns (fewer for a narrow picture, see
  // waveformCounts), 256 levels tall.
  constexpr int kWaveMaxCols = 256;
  constexpr int kWaveLevels = 256;
  // Vectorscope: a kVecSize x kVecSize grid; Cb and Cr (0..255) are halved.
  constexpr int kVecSize = 128;

  // Brightness is sqrt(share / saturation), capped at 1, where "share" is the
  // fraction of a column's pixels (waveform) or of all pixels (vectorscope)
  // that land on the point. A share is used, not a count, so a small preview
  // and the full picture look alike.
  constexpr float kWaveSat = 0.04f; // 4 % of a column on one level = full
  constexpr float kVecSat = 0.01f;  // 1 % of the picture on one cell = full
  // A vectorscope cell that has any pixel at all is at least this bright, so a
  // lone saturated color stays visible among thousands of grays.
  constexpr float kVecFloor = 0.25f;
  // Brightness of the cell colors on the vectorscope (YCbCr luma, 0..255).
  constexpr float kVecCellLuma = 180.0f;
  // The plot background (the same dark gray as the histogram canvas).
  constexpr uint8_t kBgR = 34, kBgG = 34, kBgB = 38;

  // The stride of the sample grid, the same formula as histogram::compute.
  inline int sampleStep(int w, int h)
  {
    const double total = static_cast<double>(w) * h;
    return std::max(1, static_cast<int>(std::sqrt(total / kHistMaxSamples)));
  }

  // Brightness 0..1 of a point holding `count` of `total` samples.
  inline float density(uint32_t count, uint32_t total, float saturation)
  {
    if (count == 0 || total == 0)
      return 0.0f;
    const float share = static_cast<float>(count) / static_cast<float>(total);
    return std::min(1.0f, std::sqrt(share / saturation));
  }

  // `bg` moved toward `target` by `v` (0..1), rounded.
  inline uint8_t blendTo(uint8_t bg, float target, float v)
  {
    const float t = std::min(255.0f, std::max(0.0f, target));
    return static_cast<uint8_t>(static_cast<float>(bg) + (t - bg) * v + 0.5f);
  }

  // ----------------------------------------------------------------- waveform

  struct Waveform
  {
    int cols = 0;      // 0 = nothing was counted
    uint64_t total = 0; // samples counted (transparent ones are skipped)
    // Index col * kWaveLevels + level. Each holds how many sampled pixels of
    // that picture column have that level in that channel.
    std::vector<uint32_t> r, g, b;
    std::vector<uint32_t> colTotal; // samples counted per column
  };

  // Counts the R/G/B levels of every picture column. The sample grid has
  // nx = ceil(width / step) columns; they are folded into
  // cols = min(kWaveMaxCols, nx) scope columns by index, so every scope column
  // gets at least one sample (a narrow picture never leaves gaps). A null or
  // empty picture, or one with only transparent pixels, gives cols == 0.
  inline Waveform waveformCounts(const CanvasImage *img)
  {
    Waveform wf;
    if (!img || img->width <= 0 || img->height <= 0)
      return wf;
    const int step = sampleStep(img->width, img->height);
    const int nx = (img->width + step - 1) / step;
    const int cols = std::min(kWaveMaxCols, nx);
    const size_t n = static_cast<size_t>(cols) * kWaveLevels;
    std::vector<uint32_t> r(n, 0), g(n, 0), b(n, 0), ct(cols, 0);
    uint64_t total = 0;
    const uint8_t *p = img->pixels.data();
    for (int y = 0; y < img->height; y += step)
    {
      const uint8_t *row = p + static_cast<size_t>(y) * img->width * 4;
      int ix = 0;
      for (int x = 0; x < img->width; x += step, ++ix)
      {
        const uint8_t *px = row + static_cast<size_t>(x) * 4;
        if (px[3] == 0)
          continue;
        const int col = static_cast<int>(static_cast<int64_t>(ix) * cols / nx);
        const size_t base = static_cast<size_t>(col) * kWaveLevels;
        ++r[base + px[0]];
        ++g[base + px[1]];
        ++b[base + px[2]];
        ++ct[col];
        ++total;
      }
    }
    if (total == 0)
      return wf;
    wf.cols = cols;
    wf.total = total;
    wf.r = std::move(r);
    wf.g = std::move(g);
    wf.b = std::move(b);
    wf.colTotal = std::move(ct);
    return wf;
  }

  // The waveform as a picture: cols wide, kWaveLevels tall, level 255 at the
  // top. Each channel lights its own color component on the dark background,
  // so where R, G and B coincide the point is white. Null if cols == 0.
  inline std::shared_ptr<CanvasImage> waveformImage(const Waveform &wf)
  {
    if (wf.cols <= 0)
      return nullptr;
    auto out = std::make_shared<CanvasImage>(wf.cols, kWaveLevels);
    for (int c = 0; c < wf.cols; ++c)
    {
      const uint32_t total = wf.colTotal[c];
      for (int lv = 0; lv < kWaveLevels; ++lv)
      {
        const size_t i = static_cast<size_t>(c) * kWaveLevels + lv;
        uint8_t *o = out->pixels.data() +
                     (static_cast<size_t>(kWaveLevels - 1 - lv) * wf.cols + c) * 4;
        o[0] = blendTo(kBgR, 255.0f, density(wf.r[i], total, kWaveSat));
        o[1] = blendTo(kBgG, 255.0f, density(wf.g[i], total, kWaveSat));
        o[2] = blendTo(kBgB, 255.0f, density(wf.b[i], total, kWaveSat));
        o[3] = 255;
      }
    }
    return out;
  }

  // --------------------------------------------------------------- vectorscope

  // Rec. 601 full-range chroma in integer math (the same family as the
  // histogram's luma). Cb = blue - luma, Cr = red - luma, each centered on 128
  // and always within 0..255: gray is (128, 128), pure blue (255, 107), pure
  // red (85, 255), pure green (43, 21).
  inline void rgbToCbCr(int r, int g, int b, int *cb, int *cr)
  {
    *cb = std::min(255, std::max(0, 128 + ((-43 * r - 85 * g + 128 * b) >> 8)));
    *cr = std::min(255, std::max(0, 128 + ((128 * r - 107 * g - 21 * b) >> 8)));
  }

  struct Vector
  {
    uint32_t total = 0; // samples counted (transparent ones are skipped)
    // kVecSize * kVecSize cells, index y * kVecSize + x with x = Cb / 2 and
    // y = Cr / 2. Empty when total == 0.
    std::vector<uint32_t> n;
  };

  inline Vector vectorCounts(const CanvasImage *img)
  {
    Vector v;
    if (!img || img->width <= 0 || img->height <= 0)
      return v;
    const int step = sampleStep(img->width, img->height);
    std::vector<uint32_t> n(static_cast<size_t>(kVecSize) * kVecSize, 0);
    uint32_t total = 0;
    const uint8_t *p = img->pixels.data();
    for (int y = 0; y < img->height; y += step)
    {
      const uint8_t *row = p + static_cast<size_t>(y) * img->width * 4;
      for (int x = 0; x < img->width; x += step)
      {
        const uint8_t *px = row + static_cast<size_t>(x) * 4;
        if (px[3] == 0)
          continue;
        int cb, cr;
        rgbToCbCr(px[0], px[1], px[2], &cb, &cr);
        ++n[static_cast<size_t>(cr >> 1) * kVecSize + (cb >> 1)];
        ++total;
      }
    }
    if (total == 0)
      return v;
    v.total = total;
    v.n = std::move(n);
    return v;
  }

  // The vectorscope as a kVecSize x kVecSize picture: Cb grows to the right,
  // Cr upward (red is at the top). A cell with pixels is drawn in its own hue
  // (the cell's Cb / Cr at luma kVecCellLuma), so you can see which color
  // each cluster is; how bright it is says how many pixels it holds. Cells
  // with no pixels are the background. Null if nothing was counted.
  inline std::shared_ptr<CanvasImage> vectorImage(const Vector &v)
  {
    if (v.total == 0 || v.n.size() != static_cast<size_t>(kVecSize) * kVecSize)
      return nullptr;
    auto out = std::make_shared<CanvasImage>(kVecSize, kVecSize);
    for (int y = 0; y < kVecSize; ++y)
    {
      for (int x = 0; x < kVecSize; ++x)
      {
        const uint32_t count = v.n[static_cast<size_t>(y) * kVecSize + x];
        uint8_t *o = out->pixels.data() +
                     (static_cast<size_t>(kVecSize - 1 - y) * kVecSize + x) * 4;
        o[3] = 255;
        if (count == 0)
        {
          o[0] = kBgR;
          o[1] = kBgG;
          o[2] = kBgB;
          continue;
        }
        const float b = static_cast<float>(x * 2 - 128);
        const float r = static_cast<float>(y * 2 - 128);
        const float lv = std::max(kVecFloor, density(count, v.total, kVecSat));
        o[0] = blendTo(kBgR, kVecCellLuma + 1.402f * r, lv);
        o[1] = blendTo(kBgG, kVecCellLuma - 0.344136f * b - 0.714136f * r, lv);
        o[2] = blendTo(kBgB, kVecCellLuma + 1.772f * b, lv);
      }
    }
    return out;
  }

  // ------------------------------------------------------------- one call

  // The scope picture for `kind`, or null for Histogram (which is drawn from
  // the counts, not from a picture) and for an empty / fully transparent
  // image.
  inline std::shared_ptr<CanvasImage> render(Kind kind, const CanvasImage *img)
  {
    switch (kind)
    {
    case Kind::Waveform:
      return waveformImage(waveformCounts(img));
    case Kind::Vectorscope:
      return vectorImage(vectorCounts(img));
    default:
      return nullptr;
    }
  }

} // namespace scopes
