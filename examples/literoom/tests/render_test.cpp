// Build: g++ -std=c++20 -I.. -I<folder with liteui.hpp> render_test.cpp -o render_test
// render.hpp takes liteui's CanvasImage, so in CMake this test links liteui.
#include "render/render.hpp"
#include "check.hpp"

#include <random>

using Img = CanvasImage;

static Img scene(int w, int h, uint32_t seed = 1)
{
  Img im(w, h);
  std::mt19937 rng(seed);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &im.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>((x * 255) / std::max(1, w - 1));
      p[1] = static_cast<uint8_t>((y * 255) / std::max(1, h - 1));
      p[2] = static_cast<uint8_t>(rng() & 255);
      p[3] = static_cast<uint8_t>(200 + (rng() & 31));
    }
  return im;
}
static AdjustmentSet setFor(int w, int h)
{
  AdjustmentSet s;
  s.reset();
  s.setOriginalSize(w, h);
  return s;
}
static int luma(const uint8_t *p) { return (54 * p[0] + 183 * p[1] + 19 * p[2] + 128) >> 8; }

static void testIdentity()
{
  const Img src = scene(40, 30);
  AdjustmentSet s = setFor(40, 30);
  const Img before = src;
  const auto out = renderAdjusted(s, src);
  CHECK(out && out.get() != &src && out->pixels == src.pixels && out->width == 40 && out->height == 30);
  CHECK(src.pixels == before.pixels); // the source is never touched
  // The same for a heavily edited set: size kept, alpha kept, source untouched.
  s.set(AdjId::Exposure, 60.0f);
  s.set(AdjId::Saturation, 40.0f);
  s.set(AdjId::Temperature, 30.0f);
  s.setCurve(CurveChannel::Red, CurvePoints{{0, 20}, {255, 230}}, false);
  const auto edited = renderAdjusted(s, src);
  CHECK(edited && edited->width == 40 && edited->height == 30 && edited->pixels != src.pixels);
  for (size_t i = 3; i < src.pixels.size(); i += 4)
    CHECK(edited->pixels[i] == src.pixels[i]);
  CHECK(src.pixels == before.pixels);
  // renderAdjusted is renderWith on the frozen parameters.
  CHECK(renderWith(src, s.buildRenderParams())->pixels == edited->pixels);
}

static void testToneTablesAreExactLookups()
{
  const Img src = scene(33, 21, 4);
  AdjustmentSet s = setFor(33, 21);
  s.set(AdjId::Contrast, 35.0f);
  s.setCurve(CurveChannel::Master, CurvePoints{{0, 0}, {100, 140}, {255, 255}}, false);
  s.setCurve(CurveChannel::Blue, CurvePoints{{0, 30}, {255, 200}}, false); // per-channel tables differ
  const RenderParams rp = s.buildRenderParams();
  CHECK(!(rp.luts[0] == rp.luts[2]) && !rp.color.active && !rp.fx.any() && rp.masks.empty());
  const auto out = renderWith(src, rp);
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
  {
    CHECK(out->pixels[i] == rp.luts[0][src.pixels[i]]);
    CHECK(out->pixels[i + 1] == rp.luts[1][src.pixels[i + 1]]);
    CHECK(out->pixels[i + 2] == rp.luts[2][src.pixels[i + 2]]);
    CHECK(out->pixels[i + 3] == src.pixels[i + 3]);
  }
  // The shared-table fast path gives the same bytes as separate equal tables would.
  AdjustmentSet same = setFor(33, 21);
  same.set(AdjId::Contrast, 35.0f);
  const RenderParams shared = same.buildRenderParams();
  const auto a = renderWith(src, shared);
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
    CHECK(a->pixels[i] == shared.luts[0][src.pixels[i]] && a->pixels[i + 2] == shared.luts[0][src.pixels[i + 2]]);
}

static void testBlocksDoNotChangeTheResult()
{
  // Rows are processed in blocks; pixels are independent, so a picture of any
  // height equals its rows rendered one by one, and the whole-buffer passes.
  AdjustmentSet s = setFor(9, 300);
  s.set(AdjId::Exposure, 25.0f);
  s.set(AdjId::Saturation, -30.0f);
  s.set(AdjId::Tint, 20.0f);
  s.setCurve(CurveChannel::Green, CurvePoints{{0, 0}, {128, 150}, {255, 255}}, false);
  const RenderParams rp = s.buildRenderParams();
  CHECK(rp.color.active && !rp.fx.any() && rp.masks.empty());
  for (int h : {1, 2, 127, 128, 129, 255, 256, 257, 300})
  {
    const Img src = scene(9, h, static_cast<uint32_t>(h));
    const auto whole = renderWith(src, rp);
    std::vector<uint8_t> manual = src.pixels; // the passes by hand, over the whole buffer
    for (size_t i = 0; i + 3 < manual.size(); i += 4)
      for (int c = 0; c < 3; ++c)
        manual[i + static_cast<size_t>(c)] = rp.luts[static_cast<size_t>(c)][manual[i + static_cast<size_t>(c)]];
    applyColorPass(rp.color, manual.data(), manual.size());
    CHECK(whole->pixels == manual);
    std::vector<uint8_t> rows;
    for (int y = 0; y < h; ++y)
    {
      Img row(9, 1);
      std::copy(src.pixels.begin() + static_cast<std::ptrdiff_t>(y) * 36, src.pixels.begin() + static_cast<std::ptrdiff_t>(y + 1) * 36, row.pixels.begin());
      const auto r = renderWith(row, rp);
      rows.insert(rows.end(), r->pixels.begin(), r->pixels.end());
    }
    CHECK(whole->pixels == rows);
  }
}

static void testCancellation()
{
  const Img src = scene(50, 400);
  AdjustmentSet s = setFor(50, 400);
  s.set(AdjId::Exposure, 30.0f);
  const RenderParams rp = s.buildRenderParams();
  std::atomic<uint64_t> latest{5};
  const auto ok = renderWith(src, rp, &latest, 5);
  CHECK(ok && ok->pixels == renderWith(src, rp)->pixels); // the current request runs to the end, same bytes
  CHECK(renderWith(src, rp, &latest, 4) == nullptr);      // an older request gives up at once
  latest.store(6);
  CHECK(renderWith(src, rp, &latest, 5) == nullptr);
  // Also when the neighbor planes have to be built first.
  AdjustmentSet f = setFor(50, 400);
  f.set(AdjId::Clarity, 50.0f);
  const RenderParams fx = f.buildRenderParams();
  CHECK(fx.fx.any());
  CHECK(renderWith(src, fx, &latest, 5) == nullptr);
  CHECK(renderWith(src, fx, &latest, 6) != nullptr);
}

static void testEmptyPicture()
{
  AdjustmentSet s = setFor(1, 1);
  s.set(AdjId::Exposure, 30.0f);
  s.setMasks(MaskSet{[]
                     {
                       Mask m;
                       m.parts.push_back({RadialShape{}, MaskOp::Add});
                       m.adj[static_cast<size_t>(AdjId::Exposure)] = 20.0f;
                       return m;
                     }()},
             -1);
  const auto out = renderAdjusted(s, Img(0, 0));
  CHECK(out && out->width == 0 && out->height == 0 && out->pixels.empty());
}

// --------------------------------------------------------------------- masks
static Mask radialExposure(float exposure, float amount = 100.0f, bool invert = false)
{
  Mask m;
  RadialShape r;
  r.cx = 0.5f;
  r.cy = 0.5f;
  r.rx = r.ry = 0.3f;
  r.feather = 0.5f;
  m.parts.push_back({r, MaskOp::Add});
  m.adj[static_cast<size_t>(AdjId::Exposure)] = exposure;
  m.amount = amount;
  m.invert = invert;
  return m;
}

// What a mask with only an Exposure change must do, written out per pixel:
// out = src + (exposed - src) * weight, where the weight comes from the shape alone.
static void checkExposureMask(const Mask &mask, int W, int H, const char *tag)
{
  const Img src = scene(W, H, 9);
  AdjustmentSet s = setFor(W, H);
  s.setMasks(MaskSet{mask}, -1);
  const RenderParams rp = s.buildRenderParams();
  CHECK(rp.masks.size() == 1);
  const auto out = renderWith(src, rp);
  const RadialShape shape = std::get<RadialShape>(mask.parts[0].shape);
  const RadialEval e = shape.prepare(W, H);
  const float amount = std::clamp(mask.amount, 0.0f, 100.0f) / 100.0f;
  AdjValues v{};
  v[static_cast<size_t>(AdjId::Exposure)] = mask.adj[static_cast<size_t>(AdjId::Exposure)];
  const auto lut = toneLutFromValues(v, nullptr);
  int changed = 0, untouched = 0;
  for (int y = 0; y < H; ++y)
  {
    std::vector<float> w(static_cast<size_t>(W));
    e.rowWeights(y, 0, W, w.data());
    for (int x = 0; x < W; ++x)
    {
      float wt = w[static_cast<size_t>(x)];
      if (mask.invert)
        wt = 1.0f - wt;
      wt *= amount;
      const size_t i = (static_cast<size_t>(y) * W + x) * 4;
      for (int c = 0; c < 3; ++c)
      {
        const int s0 = src.pixels[i + static_cast<size_t>(c)];
        const int want = wt <= 0.0f ? s0 : static_cast<int>(toU8Clamped(static_cast<float>(s0) + static_cast<float>(lut[static_cast<size_t>(s0)] - s0) * wt));
        if (out->pixels[i + static_cast<size_t>(c)] != want)
        {
          std::printf("  [%s] pixel (%d,%d) channel %d: got %d, want %d (weight %g)\n", tag, x, y, c, out->pixels[i + static_cast<size_t>(c)], want, wt);
          CHECK(false);
          return;
        }
        changed += out->pixels[i + static_cast<size_t>(c)] != s0;
      }
      untouched += wt <= 0.0f && out->pixels[i] == src.pixels[i];
      CHECK(out->pixels[i + 3] == src.pixels[i + 3]);
    }
  }
  CHECK(changed > 0 && untouched > 0); // the mask does something, and not everywhere
}

static void testRadialMaskBlends()
{
  checkExposureMask(radialExposure(60.0f), 80, 60, "radial");
  checkExposureMask(radialExposure(-50.0f), 80, 60, "darker");
  checkExposureMask(radialExposure(60.0f, 40.0f), 80, 60, "amount 40");
  checkExposureMask(radialExposure(60.0f, 100.0f, true), 80, 60, "inverted");
  checkExposureMask(radialExposure(60.0f), 130, 140, "two blocks"); // 140 rows: crosses the block boundary
  // Pixels outside the mask are bit-identical to the source; a disabled mask changes nothing.
  const Img src = scene(60, 40, 3);
  AdjustmentSet s = setFor(60, 40);
  Mask off = radialExposure(60.0f);
  off.enabled = false;
  s.setMasks(MaskSet{off}, -1);
  CHECK(renderAdjusted(s, src)->pixels == src.pixels);
}

static void testMasksSeeTheRowTheyAreGiven()
{
  // A Luminosity mask picks pixels by the row as it is BEFORE that mask edits it...
  const int W = 20, H = 4;
  Img src(W, H);
  for (size_t i = 0; i < src.pixels.size(); i += 4)
  {
    const int g = static_cast<int>((i / 4) % W) * 12 + 5; // 5 .. 233, a ramp
    src.pixels[i] = src.pixels[i + 1] = src.pixels[i + 2] = static_cast<uint8_t>(g);
    src.pixels[i + 3] = 255;
  }
  Mask dark;
  LuminosityShape lum;
  lum.lo = 0.0f;
  lum.hi = 0.5f;
  lum.feather = 0.0f;
  dark.parts.push_back({lum, MaskOp::Add});
  dark.adj[static_cast<size_t>(AdjId::Brightness)] = 100.0f; // dark pixels jump up by 100, past the 0.5 limit
  AdjustmentSet s = setFor(W, H);
  s.setMasks(MaskSet{dark}, -1);
  const auto out = renderAdjusted(s, src);
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
  {
    const int g = src.pixels[i];
    const bool selected = luma(&src.pixels[i]) <= 127;
    CHECK(out->pixels[i] == (selected ? std::min(255, g + 100) : g));
  }
  // ...and a LATER mask sees what the earlier ones made of it.
  Mask second;
  LuminosityShape bright;
  bright.lo = 0.4f;
  bright.hi = 1.0f;
  bright.feather = 0.0f;
  second.parts.push_back({bright, MaskOp::Add});
  second.adj[static_cast<size_t>(AdjId::Brightness)] = -50.0f;
  AdjustmentSet t = setFor(W, H);
  t.setMasks(MaskSet{dark, second}, -1);
  const auto both = renderAdjusted(t, src);
  for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
  {
    int g = src.pixels[i];
    if (luma(&src.pixels[i]) <= 127)
      g = std::min(255, g + 100);
    const int gv = g;
    if (gv * 1.0 / 255.0 >= 0.4 && gv <= 255)
      g = std::max(0, g - 50);
    CHECK(both->pixels[i] == g);
  }
}

int main()
{
  testIdentity();
  testToneTablesAreExactLookups();
  testBlocksDoNotChangeTheResult();
  testCancellation();
  testEmptyPicture();
  testRadialMaskBlends();
  testMasksSeeTheRowTheyAreGiven();
  return finish();
}
