// cases/export_render.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace export_render_cases
// so they cannot collide with other cases.
//
// export_render_test.cpp - the pixel half of exporting a picture (workers/export_render.hpp): orient ->
// straighten -> crop -> render, from an 8-bit or a 16-bit original, to an 8-bit or a 16-bit result.
// Built with stand-in spatial mask shapes like render_core_test (no masks are used here).

#include "core/image16_convert.hpp" // widenImage / narrowImage
#include "workers/export_render.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <set>

namespace export_render_cases
{
using Img = CanvasImage;

static Img scene(int w, int h, uint32_t seed)
{
  Img im(w, h);
  std::mt19937 g(seed);
  for (size_t i = 0; i < im.pixels.size(); ++i)
    im.pixels[i] = static_cast<uint8_t>(g());
  for (size_t i = 3; i < im.pixels.size(); i += 4)
    im.pixels[i] = 255;
  return im;
}
static Image16 scene16(int w, int h, uint32_t seed)
{
  Image16 im(w, h);
  std::mt19937 g(seed);
  for (uint16_t &v : im.pixels)
    v = static_cast<uint16_t>(g());
  return im;
}
static RenderParams paramsFor(const AdjValues &a)
{
  RenderParams rp;
  rp.toneF = makeToneSet(a, identityCurves());
  rp.film = filmFromAdj(a);
  rp.color = makeColorPass(a[adjIndex(AdjId::Temperature)], a[adjIndex(AdjId::Tint)], a[adjIndex(AdjId::Vibrance)],
                           a[adjIndex(AdjId::Saturation)], MixValues{}, ColorGrade{}, a[adjIndex(AdjId::Hue)],
                           a[adjIndex(AdjId::ColorizeHue)], a[adjIndex(AdjId::ColorizeAmount)], filterValuesFromAdj(a));
  return rp;
}
static AdjValues only(AdjId id, float v)
{
  AdjValues a{};
  a[adjIndex(id)] = v;
  return a;
}

TEST_CASE("export_render: identityKeepsEverySample", "[export_render]")
{
  const Img o8 = scene(37, 23, 1);
  const RenderParams id = paramsFor(AdjValues{});
  ExportPixels px;
  CHECK(renderForExport(std::make_shared<Img>(o8), ExportGeometry{}, id, false, nullptr, 0, &px));
  CHECK((px.img8 && !px.img16 && !px.is16() && px.img8->pixels == o8.pixels && px.width() == 37 && px.height() == 23));
  CHECK(renderForExport(std::make_shared<Img>(o8), ExportGeometry{}, id, true, nullptr, 0, &px));
  CHECK((px.img16 && !px.img8 && px.is16() && px.img16->pixels == widenImage(o8).pixels)); // 8-bit original, 16-bit result
  const Image16 o16 = scene16(31, 29, 2);
  CHECK(renderForExport(std::make_shared<Image16>(o16), ExportGeometry{}, id, true, nullptr, 0, &px));
  CHECK((px.img16 && !px.img8 && px.img16->pixels == o16.pixels)); // 16-bit in, 16-bit out: every sample
  CHECK(renderForExport(std::make_shared<Image16>(o16), ExportGeometry{}, id, false, nullptr, 0, &px));
  CHECK((px.img8 && !px.img16 && px.img8->pixels == narrowImage(o16).pixels)); // 16-bit in, 8-bit out: the nearest byte
}

TEST_CASE("export_render: geometryChain", "[export_render]")
{
  const Img o8 = scene(40, 30, 3);
  const Image16 o16 = widenImage(o8);
  ExportGeometry g;
  g.orient = Orient{1, true};
  g.angle = 7.5f;
  g.expand = false;
  g.crop = CropRect{3, 2, 20, 15};
  const RenderParams id = paramsFor(AdjValues{});
  // What the chain must equal: the same steps done by hand with the raster operations.
  auto want8 = [&]
  {
    auto a = orientImage(std::make_shared<Img>(o8), g.orient);
    auto b = rotateArbitrary(*a, g.angle, g.expand);
    return cropImage(*b, g.crop);
  }();
  ExportPixels px;
  CHECK(renderForExport(std::make_shared<Img>(o8), g, id, false, nullptr, 0, &px));
  CHECK((px.img8 && px.img8->width == want8->width && px.img8->height == want8->height && px.img8->pixels == want8->pixels));
  CHECK((px.width() == 20 && px.height() == 15));
  CHECK(renderForExport(std::make_shared<Image16>(o16), g, id, true, nullptr, 0, &px));
  CHECK((px.img16 && px.width() == 20 && px.height() == 15));
  // The 16-bit chain of a widened 8-bit picture is the widened 8-bit chain, to within the rounding of the resample.
  const Image16 w = widenImage(*want8);
  int worst = 0;
  for (size_t i = 0; px.img16 && i < w.pixels.size(); ++i)
    worst = std::max(worst, std::abs(static_cast<int>(px.img16->pixels[i]) - static_cast<int>(w.pixels[i])));
  CHECK(worst <= 131);
  // Quarter turns and a crop only: exact moves.
  ExportGeometry m;
  m.orient = Orient{3, false};
  m.crop = CropRect{1, 2, 10, 9};
  CHECK(renderForExport(std::make_shared<Image16>(o16), m, id, true, nullptr, 0, &px));
  const auto moved = cropImage(*orientImage(std::make_shared<Image16>(o16), m.orient), m.crop);
  CHECK((px.img16 && px.img16->pixels == moved->pixels));
}

TEST_CASE("export_render: editsAndPrecision", "[export_render]")
{
  // A flat gray picture with a strong vignette: position-dependent, so the result has many more distinct
  // values than 8 bits can hold. The 16-bit result must keep them, and narrow to the 8-bit result (within one).
  Img flat(96, 96);
  for (size_t i = 0; i < flat.pixels.size(); i += 4)
  {
    flat.pixels[i] = flat.pixels[i + 1] = flat.pixels[i + 2] = 140;
    flat.pixels[i + 3] = 255;
  }
  const RenderParams rp = paramsFor(only(AdjId::Vignette, -90.0f));
  ExportPixels e8, e16;
  CHECK(renderForExport(std::make_shared<Img>(flat), ExportGeometry{}, rp, false, nullptr, 0, &e8));
  CHECK(renderForExport(std::make_shared<Img>(flat), ExportGeometry{}, rp, true, nullptr, 0, &e16));
  std::set<uint8_t> d8;
  std::set<uint16_t> d16;
  int worst = 0;
  for (size_t i = 0; e8.img8 && e16.img16 && i < e8.img8->pixels.size(); i += 4)
  {
    d8.insert(e8.img8->pixels[i]);
    d16.insert(e16.img16->pixels[i]);
    worst = std::max(worst, std::abs(static_cast<int>(narrow16(e16.img16->pixels[i])) - static_cast<int>(e8.img8->pixels[i])));
  }
  std::printf("  (vignette on flat gray: %zu distinct 8-bit values, %zu distinct 16-bit values)\n", d8.size(), d16.size());
  CHECK(worst <= 1);
  CHECK((d8.size() >= 10 && d16.size() > d8.size() * 4)); // the 16-bit file keeps the smooth gradient (about 5.7x here)
  // A tone edit of a 16-bit picture keeps sub-8-bit differences: two samples 3 apart stay apart.
  Image16 two(2, 1);
  two.pixels = {30000, 30000, 30000, 65535, 30003, 30003, 30003, 65535};
  ExportPixels t;
  CHECK(renderForExport(std::make_shared<Image16>(two), ExportGeometry{}, paramsFor(only(AdjId::Brightness, 12.0f)), true, nullptr, 0, &t));
  CHECK((t.img16 && t.img16->pixels[4] != t.img16->pixels[0] && std::abs(static_cast<int>(t.img16->pixels[4]) - static_cast<int>(t.img16->pixels[0]) - 3) <= 1));
}

TEST_CASE("export_render: superseded", "[export_render]")
{
  const Img o8 = scene(60, 300, 4);
  const Image16 o16 = widenImage(o8);
  const RenderParams rp = paramsFor(only(AdjId::Exposure, 25.0f));
  std::atomic<uint64_t> latest{5};
  ExportPixels px;
  for (bool angle : {false, true})
  {
    ExportGeometry g;
    g.angle = angle ? 4.0f : 0.0f;
    for (bool want16 : {false, true})
    {
      CHECK(renderForExport(std::make_shared<Img>(o8), g, rp, want16, &latest, 5, &px));       // current: runs
      CHECK(!renderForExport(std::make_shared<Img>(o8), g, rp, want16, &latest, 4, &px));      // older: gives up
      CHECK(renderForExport(std::make_shared<Image16>(o16), g, rp, want16, &latest, 5, &px));
      CHECK(!renderForExport(std::make_shared<Image16>(o16), g, rp, want16, &latest, 4, &px));
    }
  }
  latest.store(6);
  CHECK(!renderForExport(std::make_shared<Img>(o8), ExportGeometry{}, rp, false, &latest, 5, &px));
}

TEST_CASE("export_render: resize", "[export_render]")
{
  const Img o8 = scene(100, 60, 5);
  const RenderParams id = paramsFor(AdjValues{});
  ExportPixels a, b;
  CHECK(renderForExport(std::make_shared<Img>(o8), ExportGeometry{}, id, false, nullptr, 0, &a));
  CHECK(renderForExport(std::make_shared<Img>(o8), ExportGeometry{}, id, true, nullptr, 0, &b));
  a.resizeToLongEdge(50);
  b.resizeToLongEdge(50);
  CHECK((a.width() == 50 && a.height() == 30 && b.width() == 50 && b.height() == 30 && a.img8 && b.img16));
  // 16-bit resize of a widened picture ~ the 8-bit resize (the 16-bit average is the exact one)
  int worst = 0;
  for (size_t i = 0; i < a.img8->pixels.size(); ++i)
    worst = std::max(worst, std::abs(static_cast<int>(b.img16->pixels[i]) - static_cast<int>(a.img8->pixels[i]) * 257));
  CHECK(worst <= 130);
  const auto keep = b.img16;
  b.resizeToLongEdge(0); // < 1: nothing changes
  CHECK(b.img16 == keep);
  b.resizeToLongEdge(500); // never enlarges
  CHECK((b.img16 == keep && b.width() == 50));
  ExportPixels none;
  none.resizeToLongEdge(10); // empty: no crash
  CHECK((none.width() == 0 && none.height() == 0 && !none.is16()));
}
} // namespace export_render_cases
