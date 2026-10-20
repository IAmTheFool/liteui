// histogram.hpp - Counting the pixels of a picture: the R/G/B/luma histograms,
// which channels are clipped, and the clipping overlay (the blue / red tint on
// the canvas). Pure functions of an image and a few flags; the editor's
// `state` is not touched, so it can be tested on small synthetic pictures.
// main.cpp's computeHistogram() picks the image on screen, calls these and
// stores the results where the histogram canvas and the overlay read them.
// Takes liteui's CanvasImage and Color, hence liteui.hpp.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "liteui.hpp"

// Histogram: at most ~kHistMaxSamples pixels are visited per recompute
// (strided grid), so it stays cheap on full-resolution images.
constexpr int kHistMaxSamples = 300000;

// Clipping warnings: canvas overlay.
constexpr int kClipOverlayMaxEdge = 2048; // overlay resolution cap (long edge)
constexpr Color kColorClipShadow{60, 110, 255, 255};
constexpr Color kColorClipHighlight{235, 50, 50, 255};

// Channel bits for clipping masks.
constexpr uint8_t kChanR = 1, kChanG = 2, kChanB = 4;

namespace histogram
{

  struct Counts
  {
    std::array<uint32_t, 256> r{}, g{}, b{}, l{};
    // Channels (kChanR | kChanG | kChanB) that have any pixel at 0 / at 255,
    // read from the end bins of the histograms above - so on a picture bigger
    // than kHistMaxSamples they only reflect the sampled pixels (the overlay,
    // by contrast, looks at every pixel).
    uint8_t clipLo = 0, clipHi = 0;
  };

  // Counts the R/G/B/luma histograms of `img`, visiting at most
  // ~kHistMaxSamples pixels on a regular grid. Fully transparent pixels
  // (straighten's empty corners) are skipped. A null or empty image gives
  // all zeros.
  inline Counts compute(const CanvasImage *img)
  {
    Counts c;
    if (img && img->width > 0 && img->height > 0)
    {
      const double total = static_cast<double>(img->width) * img->height;
      const int step =
          std::max(1, static_cast<int>(std::sqrt(total / kHistMaxSamples)));
      const uint8_t *p = img->pixels.data();
      for (int y = 0; y < img->height; y += step)
      {
        const uint8_t *row = p + static_cast<size_t>(y) * img->width * 4;
        for (int x = 0; x < img->width; x += step)
        {
          const uint8_t *px = row + static_cast<size_t>(x) * 4;
          if (px[3] == 0)
            continue;
          ++c.r[px[0]];
          ++c.g[px[1]];
          ++c.b[px[2]];
          // Rec. 601 luma, integer math.
          ++c.l[(px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8];
        }
      }
    }
    // Channels with any clipped pixel, from the end bins of the histograms.
    c.clipLo = static_cast<uint8_t>((c.r[0] ? kChanR : 0) |
                                    (c.g[0] ? kChanG : 0) |
                                    (c.b[0] ? kChanB : 0));
    c.clipHi = static_cast<uint8_t>((c.r[255] ? kChanR : 0) |
                                    (c.g[255] ? kChanG : 0) |
                                    (c.b[255] ? kChanB : 0));
    return c;
  }

  // Which of R/G/B (bit 0/1/2) equal `level` in this pixel.
  inline uint8_t channelsAt(const uint8_t *px, uint8_t level)
  {
    return static_cast<uint8_t>((px[0] == level ? kChanR : 0) |
                                (px[1] == level ? kChanG : 0) |
                                (px[2] == level ? kChanB : 0));
  }

  // Additive mix of the channels in `mask` (1=R, 2=G, 4=B). `pastel` lightens
  // it, so shadow clipping reads differently from highlight clipping. This
  // table is the one place to tweak the palette.
  inline Color clipChannelColor(uint8_t mask, bool pastel)
  {
    static constexpr Color kMix[8] = {
        {0, 0, 0, 0},         // none
        {235, 50, 50, 255},   // R
        {50, 205, 80, 255},   // G
        {245, 220, 40, 255},  // R+G  yellow
        {70, 120, 255, 255},  // B
        {235, 70, 220, 255},  // R+B  magenta
        {40, 210, 235, 255},  // G+B  cyan
        {235, 235, 235, 255}, // R+G+B
    };
    Color c = kMix[mask & 7];
    if (pastel)
    {
      c.r = static_cast<uint8_t>((c.r + 255) / 2);
      c.g = static_cast<uint8_t>((c.g + 255) / 2);
      c.b = static_cast<uint8_t>((c.b + 255) / 2);
    }
    return c;
  }

  // Which overlays are on, and how they are colored.
  struct ClipOptions
  {
    bool shadows = false, highlights = false;
    bool perChannel = false; // color by exactly which channels clip
  };

  // Doc-sized RGBA overlay for the overlays that are on, or null when both are
  // off or the image is null / empty. Classic mode: red / blue where any
  // channel is 255 / 0. Per-channel mode: colored by exactly which channels
  // clip (pixels clipped in all three are left untinted). Where a block holds
  // both kinds, highlights win.
  // Images above kClipOverlayMaxEdge are reduced by blocks: a block is marked
  // if ANY pixel in it is clipped, so small specular highlights don't vanish.
  // Fully transparent pixels never count.
  inline std::shared_ptr<CanvasImage> clipOverlay(const CanvasImage *img,
                                                  const ClipOptions &opt)
  {
    if ((!opt.shadows && !opt.highlights) || !img || img->width <= 0 ||
        img->height <= 0)
      return nullptr;
    const int w = img->width, h = img->height;
    const int step = std::max(
        1, (std::max(w, h) + kClipOverlayMaxEdge - 1) / kClipOverlayMaxEdge);
    const int ow = (w + step - 1) / step, oh = (h + step - 1) / step;
    auto out = std::make_shared<CanvasImage>(ow, oh);
    const uint8_t *src = img->pixels.data();
    for (int oy = 0; oy < oh; ++oy)
    {
      const int y0 = oy * step, y1 = std::min(y0 + step, h);
      for (int ox = 0; ox < ow; ++ox)
      {
        const int x0 = ox * step, x1 = std::min(x0 + step, w);
        // Bit m set = some pixel in the block is clipped in exactly the
        // channel set m (bit 0 = R, 1 = G, 2 = B; m = 0 means unclipped).
        uint8_t hiSeen = 0, loSeen = 0;
        for (int y = y0; y < y1; ++y)
        {
          const uint8_t *row = src + static_cast<size_t>(y) * w * 4;
          for (int x = x0; x < x1; ++x)
          {
            const uint8_t *px = row + static_cast<size_t>(x) * 4;
            if (px[3] == 0)
              continue;
            hiSeen |= static_cast<uint8_t>(1u << channelsAt(px, 255));
            loSeen |= static_cast<uint8_t>(1u << channelsAt(px, 0));
          }
        }
        Color col;
        auto pick = [&](uint8_t seen, bool pastel, const Color &classic)
        {
          if (opt.perChannel)
          {
            const uint8_t partial = seen & 0x7E; // masks 1..6; 7 = all, skipped
            if (!partial)
              return false;
            uint8_t m = 1;
            while (!(partial & (1u << m)))
              ++m; // lowest clipped-channel set present
            col = clipChannelColor(m, pastel);
            return true;
          }
          if (!(seen & 0xFE)) // any clipped channel at all
            return false;
          col = classic;
          return true;
        };
        if (!(opt.highlights && pick(hiSeen, false, kColorClipHighlight)) &&
            !(opt.shadows && pick(loSeen, true, kColorClipShadow)))
          continue;
        uint8_t *o = out->pixels.data() + (static_cast<size_t>(oy) * ow + ox) * 4;
        o[0] = col.r;
        o[1] = col.g;
        o[2] = col.b;
        o[3] = 255;
      }
    }
    return out;
  }

} // namespace histogram
