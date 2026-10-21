// mask_raster.hpp - RasterEval: a 0..255 coverage map resolved for one image size, read back
// with bilinear interpolation. The Brush and the Object masks both work
// this way (they only differ in how the map is made).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// A coverage map resolved for one image size, shared by the Brush and Object
// masks: the map plus, per map row, the pixels that can be non-zero.
struct RasterEval
{
  int W = 0, H = 0;   // the image this is resolved for
  int mw = 0, mh = 0; // the map's own size: W x H, or smaller for a big image
  // 0..255 per map pixel, row-major mw x mh. Shared so copying an Eval (they
  // live in a std::variant) stays cheap.
  std::shared_ptr<const std::vector<uint8_t>> map;
  // Per MAP row, [lo, hi) covers every non-zero pixel. A superset: an erase
  // stroke does not shrink it.
  std::shared_ptr<const std::vector<int>> lo, hi;

  // The two map rows image row `y` interpolates between, and the blend.
  void mapRows(int y, int &r0, int &r1, float &t) const
  {
    const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(mh) /
                         static_cast<float>(H) -
                     0.5f;
    const int a = static_cast<int>(std::floor(fy));
    t = fy - static_cast<float>(a);
    r0 = a < 0 ? 0 : (a > mh - 1 ? mh - 1 : a);
    r1 = a + 1 < 0 ? 0 : (a + 1 > mh - 1 ? mh - 1 : a + 1);
  }

  bool rowSpan(int y, int imgW, int &x0, int &x1) const
  {
    if (!map || y < 0 || y >= H)
      return false;
    int r0, r1;
    float t;
    mapRows(y, r0, r1, t);
    const int l = std::min((*lo)[static_cast<size_t>(r0)],
                           (*lo)[static_cast<size_t>(r1)]);
    const int h = std::max((*hi)[static_cast<size_t>(r0)],
                           (*hi)[static_cast<size_t>(r1)]);
    if (h <= l)
      return false;
    // Map pixels -> image pixels, padded for the interpolation's reach.
    const float k = static_cast<float>(W) / static_cast<float>(mw);
    const int pad = static_cast<int>(std::ceil(k)) + 1;
    x0 = static_cast<int>(std::floor(static_cast<float>(l) * k)) - pad;
    x1 = static_cast<int>(std::ceil(static_cast<float>(h) * k)) + pad;
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 > imgW ? imgW : x1;
    return x1 > x0;
  }

  // Plain loops, no std helpers (same Debug-build reason as RadialEval).
  void rowWeights(int y, int x0, int x1, float *out,
                  const uint8_t * /*pix*/ = nullptr) const
  {
    if (!map || y < 0 || y >= H)
    {
      for (int x = x0; x < x1; ++x)
        out[x - x0] = 0.0f;
      return;
    }
    int r0, r1;
    float ty;
    mapRows(y, r0, r1, ty);
    const uint8_t *row0 = map->data() + static_cast<size_t>(r0) *
                                            static_cast<size_t>(mw);
    const uint8_t *row1 = map->data() + static_cast<size_t>(r1) *
                                            static_cast<size_t>(mw);
    const float inv255 = 1.0f / 255.0f;
    if (mw == W)
    { // map and image agree horizontally: no x interpolation
      for (int x = x0; x < x1; ++x)
      {
        const float a = static_cast<float>(row0[x]);
        const float b = static_cast<float>(row1[x]);
        out[x - x0] = (a + (b - a) * ty) * inv255;
      }
      return;
    }
    const float k = static_cast<float>(mw) / static_cast<float>(W);
    for (int x = x0; x < x1; ++x)
    {
      const float fx = (static_cast<float>(x) + 0.5f) * k - 0.5f;
      const int a = static_cast<int>(std::floor(fx));
      const float tx = fx - static_cast<float>(a);
      const int c0 = a < 0 ? 0 : (a > mw - 1 ? mw - 1 : a);
      const int c1 = a + 1 < 0 ? 0 : (a + 1 > mw - 1 ? mw - 1 : a + 1);
      const float top = static_cast<float>(row0[c0]) * (1.0f - tx) +
                        static_cast<float>(row0[c1]) * tx;
      const float bot = static_cast<float>(row1[c0]) * (1.0f - tx) +
                        static_cast<float>(row1[c1]) * tx;
      out[x - x0] = (top + (bot - top) * ty) * inv255;
    }
  }
};
