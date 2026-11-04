// imagebench — times the high-precision image pipeline on a big image.
//
//   imagebench [WIDTHxHEIGHT]      default 6000x4000 (24 megapixels)
//
// Use a Release build. Each operation is run a few times and the best time
// is reported, so a cold cache or page-fault storm on the first run doesn't
// dominate. Numbers depend on the machine; compare runs on the SAME machine
// before and after a change.

#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
  uint32_t g_sink = 0; // keeps results observable so nothing is optimised away

  template <class F>
  double bestMs(F &&f, int reps = 3)
  {
    double best = 1e30;
    for (int i = 0; i < reps; ++i)
    {
      auto a = std::chrono::steady_clock::now();
      f();
      auto b = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double, std::milli>(b - a).count());
    }
    return best;
  }

  double g_megapixels = 0;
  void report(const char *name, double ms)
  {
    std::printf("  %-44s %9.1f ms  %8.1f MP/s\n", name, ms,
                ms > 0 ? g_megapixels / (ms / 1000.0) : 0.0);
  }

  ImageBuffer must(std::optional<ImageBuffer> b)
  {
    if (!b)
    {
      std::fprintf(stderr, "allocation failed\n");
      std::exit(1);
    }
    return std::move(*b);
  }

  ImageBuffer as(const ImageBuffer &src, PixelFormat f)
  {
    ImageBuffer d;
    d.format = f;
    if (!liteui_image::convertImage(src, d))
    {
      std::fprintf(stderr, "conversion failed\n");
      std::exit(1);
    }
    return d;
  }
} // namespace

int main(int argc, char **argv)
{
  uint32_t w = 6000, h = 4000;
  if (argc > 1)
  {
    unsigned a = 0, b = 0;
    if (std::sscanf(argv[1], "%ux%u", &a, &b) == 2 && a > 0 && b > 0)
    {
      w = a;
      h = b;
    }
  }
  g_megapixels = static_cast<double>(w) * h / 1e6;
  std::printf("liteui image pipeline benchmark: %u x %u (%.1f MP)\n\n", w, h, g_megapixels);

  std::printf("Memory for one image:\n");
  for (auto [name, f] : {std::pair{"RGBA8_UNORM ", PixelFormat::RGBA8_UNORM},
                         std::pair{"RGBA16_FLOAT", PixelFormat::RGBA16_FLOAT},
                         std::pair{"RGBA32_FLOAT", PixelFormat::RGBA32_FLOAT}})
  {
    auto bytes = imageByteSize(w, h, f);
    std::printf("  %s %9.1f MB\n", name, bytes ? *bytes / 1e6 : 0.0);
  }

  std::printf("\nTiming (best of 3):\n");
  report("allocate RGBA32_FLOAT (zero-filled)",
         bestMs([&]
                { g_sink += static_cast<uint32_t>(must(ImageBuffer::create(w, h, PixelFormat::RGBA32_FLOAT)).byteSize()); }));

  ImageBuffer f32 = must(ImageBuffer::create(w, h, PixelFormat::RGBA32_FLOAT));
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
    {
      const float px[4] = {1.2f * x / w, static_cast<float>(y) / h, 0.3f, 1.0f};
      f32.setPixel(x, y, px);
    }
  ImageBuffer f16 = as(f32, PixelFormat::RGBA16_FLOAT);
  ImageBuffer u8 = as(f32, PixelFormat::RGBA8_UNORM);

  report("convert RGBA32F -> RGBA8", bestMs([&]
                                            { g_sink += static_cast<uint32_t>(as(f32, PixelFormat::RGBA8_UNORM).pixels[7]); }));
  report("convert RGBA32F -> RGBA16F", bestMs([&]
                                              { g_sink += static_cast<uint32_t>(as(f32, PixelFormat::RGBA16_FLOAT).pixels[7]); }));
  report("convert RGBA16F -> RGBA32F", bestMs([&]
                                              { g_sink += static_cast<uint32_t>(as(f16, PixelFormat::RGBA32_FLOAT).pixels[7]); }));
  report("convert RGBA8   -> RGBA32F", bestMs([&]
                                              { g_sink += static_cast<uint32_t>(as(u8, PixelFormat::RGBA32_FLOAT).pixels[7]); }));

  DisplaySettings ds;
  ds.exposureEv = 0.5f;
  report("display RGBA32F (exposure +0.5)", bestMs([&]
                                                   { g_sink += static_cast<uint32_t>(must(liteui_image::makeDisplayImage(f32, ds)).pixels[7]); }));
  report("display RGBA16F (exposure +0.5)", bestMs([&]
                                                   { g_sink += static_cast<uint32_t>(must(liteui_image::makeDisplayImage(f16, ds)).pixels[7]); }));

  const uint32_t vw = std::min(w, 1920u), vh = std::min(h, 1080u);
  {
    double ms = bestMs([&]
                       { g_sink += static_cast<uint32_t>(must(liteui_image::makeDisplayImage(
                                                                  f32, ds, liteui_image::ImageRect{0, 0, vw, vh}))
                                                             .pixels[7]); });
    std::printf("  %-44s %9.1f ms  (viewport %ux%u)\n", "display RGBA32F region (viewport only)", ms, vw, vh);
  }
  for (uint32_t factor : {4u, 8u})
  {
    std::string name = "downscale RGBA32F by " + std::to_string(factor) + " (preview)";
    report(name.c_str(), bestMs([&]
                                { g_sink += static_cast<uint32_t>(must(liteui_image::downscaleImage(f32, factor)).pixels[7]); }));
  }
  std::printf("\n(checksum %u)\n", g_sink);
  return 0;
}
