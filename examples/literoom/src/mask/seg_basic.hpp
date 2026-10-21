// seg_basic.hpp - The stand-in segmenter: a magic wand that grows over similar colors. It
// has the contract the real model has (picture + prompts in, coverage map
// out, cancellable), so everything around it can be built and tested
// without a model.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "liteui.hpp"
#include "mask/mask_object.hpp"

constexpr int kSegPrepEdge = 512; // long side the stand-in segmenter works at
constexpr int kSegTolerance = 40; // stand-in: RGB distance for "same color"

// ---- Stand-in segmenter (replaced by MobileSAM in step 3) ----
// Magic wand: grow from each foreground point over pixels whose color is
// within kSegTolerance of the seed's color; background points grow the same
// way and are subtracted. Same contract the real model will have: image +
// prompts in, coverage map out, cancellable, runs on the worker thread.
struct SegPrepared
{ // what a model would cache per image (an embedding)
  int w = 0, h = 0;
  std::vector<uint8_t> rgb; // w*h*3
};

inline std::shared_ptr<const SegPrepared> prepareSegImage(const CanvasImage &s)
{
  const int k = std::max(
      1, (std::max(s.width, s.height) + kSegPrepEdge - 1) / kSegPrepEdge);
  auto out = std::make_shared<SegPrepared>();
  out->w = std::max(1, s.width / k);
  out->h = std::max(1, s.height / k);
  out->rgb.resize(static_cast<size_t>(out->w) * out->h * 3);
  const int area = k * k;
  for (int y = 0; y < out->h; ++y)
    for (int x = 0; x < out->w; ++x)
    {
      int sum[3] = {0, 0, 0};
      for (int dy = 0; dy < k; ++dy)
        for (int dx = 0; dx < k; ++dx)
        {
          const uint8_t *p =
              &s.pixels[(static_cast<size_t>(y * k + dy) * s.width +
                         static_cast<size_t>(x * k + dx)) *
                        4];
          sum[0] += p[0];
          sum[1] += p[1];
          sum[2] += p[2];
        }
      uint8_t *o = &out->rgb[(static_cast<size_t>(y) * out->w + x) * 3];
      for (int c = 0; c < 3; ++c)
        o[c] = static_cast<uint8_t>((sum[c] + area / 2) / area);
    }
  return out;
}

// False if cancelled.
inline bool segGrow(const SegPrepared &im, int sx, int sy,
                    std::vector<uint8_t> &mark,
                    const std::atomic<uint64_t> *latest, uint64_t gen)
{
  const int w = im.w, h = im.h;
  int ref[3] = {0, 0, 0}, n = 0; // seed color: mean of a 5x5 patch
  for (int y = std::max(0, sy - 2); y <= std::min(h - 1, sy + 2); ++y)
    for (int x = std::max(0, sx - 2); x <= std::min(w - 1, sx + 2); ++x)
    {
      const uint8_t *p = &im.rgb[(static_cast<size_t>(y) * w + x) * 3];
      ref[0] += p[0];
      ref[1] += p[1];
      ref[2] += p[2];
      ++n;
    }
  for (int c = 0; c < 3; ++c)
    ref[c] /= n;
  const int tolSq = kSegTolerance * kSegTolerance;
  std::vector<int> stack;
  const auto tryPush = [&](int x, int y)
  {
    if (x < 0 || y < 0 || x >= w || y >= h)
      return;
    const size_t i = static_cast<size_t>(y) * w + x;
    if (mark[i])
      return;
    const uint8_t *p = &im.rgb[i * 3];
    const int dr = p[0] - ref[0], dg = p[1] - ref[1], db = p[2] - ref[2];
    if (dr * dr + dg * dg + db * db > tolSq)
      return;
    mark[i] = 1;
    stack.push_back(static_cast<int>(i));
  };
  tryPush(sx, sy);
  size_t pops = 0;
  while (!stack.empty())
  {
    if ((++pops & 4095) == 0 && latest &&
        latest->load(std::memory_order_relaxed) != gen)
      return false;
    const int i = stack.back();
    stack.pop_back();
    const int x = i % w, y = i / w;
    tryPush(x - 1, y);
    tryPush(x + 1, y);
    tryPush(x, y - 1);
    tryPush(x, y + 1);
  }
  return true;
}

// Worker thread only (the cache below is not locked). Returns null if
// cancelled or if `*err` is set.
inline std::shared_ptr<const ObjectMap>
segmentObjectWand(const CanvasImage &img, uint64_t key,
                  const std::vector<ObjectPrompt> &prompts, std::string *err,
                  const std::atomic<uint64_t> *latest, uint64_t gen)
{
  static uint64_t cachedKey = 0;
  static std::shared_ptr<const SegPrepared> cached;
  if (!cached || cachedKey != key)
  {
    cached = prepareSegImage(img);
    cachedKey = key;
  }
  const SegPrepared &im = *cached;
  const size_t n = static_cast<size_t>(im.w) * im.h;
  std::vector<uint8_t> fg(n, 0), bg(n, 0);
  for (const ObjectPrompt &p : prompts)
  {
    const int sx = std::clamp(static_cast<int>(p.x * im.w), 0, im.w - 1);
    const int sy = std::clamp(static_cast<int>(p.y * im.h), 0, im.h - 1);
    if (!segGrow(im, sx, sy, p.fg ? fg : bg, latest, gen))
      return nullptr; // cancelled
  }
  auto m = std::make_shared<ObjectMap>();
  m->w = im.w;
  m->h = im.h;
  m->id = nextObjectMapId();
  m->a.resize(n);
  for (size_t i = 0; i < n; ++i)
    m->a[i] = (fg[i] && !bg[i]) ? 255 : 0;
  (void)err;
  return m;
}
