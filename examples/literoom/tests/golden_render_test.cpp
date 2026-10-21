// golden_render_test.cpp - the refactoring safety net.
//
// Renders a synthetic picture through the whole pipeline (tone LUTs, color
// pass, Mix, Grade, curves, detail / neighbor effects, every mask type, mask
// combining, geometry, sidecar, undo history, a few file helpers) and
// fingerprints every result. The fingerprints are stored in a "golden" file.
// While you move code between headers the test must keep printing "all ok":
// a moved function that changes even one output byte shows up as a MISMATCH
// with the name of the scenario that moved.
//
//   golden_render_test --record tests/golden/render_Release.txt   (once, BEFORE refactoring)
//   golden_render_test        tests/golden/render_Release.txt     (after every step)
//   golden_render_test --dump out_before/ --record x.txt          (optional: one PPM per scenario,
//                                                                  so two runs can be diffed with cmp / fc /b)
//
// Floating point results can differ between compilers, flags and CPUs, so
// record one golden file per build configuration (CMake passes $<CONFIG>)
// and only compare a run against a golden file recorded by the same build.
//
// Exit code: 0 = every scenario matches and every invariant holds.

// ---------------------------------------------------------------------------
// What is under test. Phase 0 (today): main.cpp itself, compiled as part of
// this translation unit with its main() renamed - everything in it is static,
// so this is the only way to reach it. After the split, build with
// -DGOLDEN_USE_ENGINE_HEADERS and point the include at the new aggregate
// header; nothing else in this file should need to change.
// ---------------------------------------------------------------------------
#ifdef GOLDEN_USE_ENGINE_HEADERS
#include "engine/all.hpp"
#else
#include "liteui.hpp"
#include "mask/sam_segmenter.hpp"
#include "io/exif.hpp"
#include "io/minijson.hpp"
#include "detail/detail_layer.hpp"
#include "detail/neighbor_fx.hpp"
// main.cpp's own includes, pulled in first so that the `main` macro below
// can't touch any of them.
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>
#define main literoom_app_main
#include "main.cpp"
#undef main
#endif

namespace golden
{

  // ------------------------------------------------------------ bookkeeping
  struct Fp
  {
    uint64_t hash = 0;
    int w = 0, h = 0;
    uint64_t sum[4] = {0, 0, 0, 0}; // per-channel byte sums (images) - a human-readable hint
    bool operator==(const Fp &o) const
    {
      return hash == o.hash && w == o.w && h == o.h &&
             std::memcmp(sum, o.sum, sizeof sum) == 0;
    }
  };

  static std::vector<std::pair<std::string, Fp>> gResults;
  static std::map<std::string, CanvasImage> gImages; // only filled with --dump
  static std::vector<std::string> gFailures, gNotes;
  static bool gKeepImages = false;

  static void fail(const std::string &where, const std::string &what)
  {
    gFailures.push_back(where + ": " + what);
  }
  static void note(const std::string &where, const std::string &what)
  {
    gNotes.push_back(where + ": " + what);
  }

  static uint64_t fnv(const void *data, size_t n, uint64_t h = 1469598103934665603ull)
  {
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < n; ++i)
      h = (h ^ p[i]) * 1099511628211ull;
    return h;
  }

  static Fp fpImage(const CanvasImage &img)
  {
    Fp f;
    f.w = img.width;
    f.h = img.height;
    f.hash = fnv(img.pixels.data(), img.pixels.size());
    for (size_t i = 0; i + 3 < img.pixels.size(); i += 4)
      for (int c = 0; c < 4; ++c)
        f.sum[c] += img.pixels[i + static_cast<size_t>(c)];
    return f;
  }
  static Fp fpText(const std::string &s)
  {
    Fp f;
    f.hash = fnv(s.data(), s.size());
    f.sum[0] = s.size();
    return f;
  }

  static void put(const std::string &name, const Fp &fp)
  {
    for (const auto &r : gResults)
      if (r.first == name)
      {
        fail(name, "scenario name used twice");
        return;
      }
    gResults.emplace_back(name, fp);
  }
  static void putImage(const std::string &name, const CanvasImage &img)
  {
    put(name, fpImage(img));
    if (gKeepImages)
      gImages[name] = img;
  }
  static void putText(const std::string &name, const std::string &text)
  {
    put(name, fpText(text));
  }

  // Every cache that outlives one render is emptied between scenarios, so a
  // scenario's result never depends on which scenarios ran before it. (The
  // warm-cache run inside renderScenario then proves caching changes nothing.)
  static void resetAllCaches()
  {
    detail_layer::cache().clear();
    neighbor_fx::clearCaches();
    clearMaskCaches();
  }

  // --------------------------------------------------------- synthetic image
  // Integer arithmetic only, so the INPUT is identical on every platform.
  struct Rgb
  {
    int r, g, b;
  };
  static Rgb hsv(int h, int s, int v)
  {
    const int region = h / 60, rem = (h % 60) * 255 / 60;
    const int p = v * (255 - s) / 255;
    const int q = v * (255 - s * rem / 255) / 255;
    const int t = v * (255 - s * (255 - rem) / 255) / 255;
    switch (region)
    {
    case 0:
      return {v, t, p};
    case 1:
      return {q, v, p};
    case 2:
      return {p, v, t};
    case 3:
      return {p, q, v};
    case 4:
      return {t, p, v};
    default:
      return {v, p, q};
    }
  }
  static uint32_t mix32(uint32_t x, uint32_t y, uint32_t seed)
  {
    uint32_t n = x * 374761393u + y * 668265263u + seed * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
  }
  static int cl(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

  // Five horizontal bands, each there to exercise something:
  //   0 hue sweep, fading to gray      (Color Mixer bands, Vibrance, Saturation)
  //   1 gray ramp (+ a tinted half)    (tone sliders, curves, clipping, Luminosity mask)
  //   2 pale bluish low contrast       (Dehaze)
  //   3 checker + lines + per-channel noise (Texture / Clarity / Sharpness / Noise / Blur)
  //   4 warm gradient, bright and dark discs, a green and a blue patch
  //                                     (Color Range, Object, Brush, Radial, Linear)
  // The last column has alpha 200 so a stray alpha change is caught.
  static CanvasImage makeScene(int W, int H, uint32_t seed = 1)
  {
    CanvasImage img(W, H);
    const int bandH = std::max(1, H / 5);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
      {
        const int band = std::min(4, y / bandH);
        const int by = std::min(bandH - 1, y - band * bandH);
        int r = 0, g = 0, b = 0;
        switch (band)
        {
        case 0:
        {
          const Rgb c = hsv((x * 360 / W) % 360, 255 - by * 150 / bandH,
                            120 + by * 100 / bandH);
          r = c.r;
          g = c.g;
          b = c.b;
          break;
        }
        case 1:
        {
          const int v = W > 1 ? x * 255 / (W - 1) : 128;
          r = g = b = v;
          if (by > bandH / 2)
          {
            r = cl(v + 25);
            b = cl(v - 25);
          }
          break;
        }
        case 2:
        {
          const int base = 175 + x * 30 / W + by * 10 / bandH;
          r = base - 6;
          g = base;
          b = base + 10;
          break;
        }
        case 3:
        {
          int base = (((x / 9) + (y / 9)) & 1) ? 150 : 70;
          if (((x + y) % 37) < 2)
            base = 235;
          const int n1 = static_cast<int>(mix32(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed) & 31) - 16;
          const int n2 = static_cast<int>(mix32(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed + 7) & 31) - 16;
          const int n3 = static_cast<int>(mix32(static_cast<uint32_t>(x), static_cast<uint32_t>(y), seed + 13) & 31) - 16;
          r = cl(base + 20 + n1);
          g = cl(base + n2);
          b = cl(base - 15 + n3);
          break;
        }
        default:
        {
          r = 110 + x * 110 / W;
          g = 70 + by * 80 / bandH + x * 20 / W;
          b = 60 + (bandH - by) * 40 / bandH;
          const int cy = band * bandH + bandH / 2;
          const long rad = std::max(3, bandH / 3);
          long dx = x - W * 3 / 10, dy = y - cy;
          if (dx * dx + dy * dy < rad * rad)
          {
            r = 245;
            g = 238;
            b = 215;
          }
          dx = x - W * 6 / 10;
          if (dx * dx + dy * dy < rad * rad)
          {
            r = 20;
            g = 25;
            b = 30;
          }
          if (x >= W * 72 / 100 && x < W * 82 / 100 && by >= bandH / 5 && by < bandH * 4 / 5)
          {
            r = 30;
            g = 200;
            b = 60;
          }
          if (x >= W * 84 / 100 && x < W * 94 / 100 && by >= bandH / 5 && by < bandH * 4 / 5)
          {
            r = 40;
            g = 70;
            b = 210;
          }
          break;
        }
        }
        uint8_t *p = &img.pixels[(static_cast<size_t>(y) * W + x) * 4];
        p[0] = static_cast<uint8_t>(cl(r));
        p[1] = static_cast<uint8_t>(cl(g));
        p[2] = static_cast<uint8_t>(cl(b));
        p[3] = (x == W - 1) ? 200 : 255;
      }
    return img;
  }

  // A coverage map for the Object mask: an ellipse with a hole and a
  // half-covered stripe. Integer math only.
  static std::shared_ptr<const ObjectMap> makeBlobMap(int w, int h)
  {
    auto m = std::make_shared<ObjectMap>();
    m->w = w;
    m->h = h;
    m->id = nextObjectMapId();
    m->a.assign(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
      {
        const long long dx = 20LL * x - 9LL * w, dy = 20LL * y - 10LL * h;
        const long long ax = 6LL * w, ay = 8LL * h;
        const bool inside = dx * dx * ay * ay + dy * dy * ax * ax <= ax * ax * ay * ay;
        const long long hole = std::min(w, h) * 2LL; // radius 0.1 * min * 20
        const bool holed = dx * dx + dy * dy < hole * hole;
        uint8_t v = inside && !holed ? 255 : 0;
        if (v && ((x + y) % 11) < 3)
          v = 128;
        m->a[static_cast<size_t>(y) * w + x] = v;
      }
    return m;
  }

  // ------------------------------------------------------------ edit helpers
  static void init(AdjustmentSet &s, int W, int H)
  {
    s.reset();
    s.setOriginalSize(W, H);
  }

  static size_t idx(AdjId id) { return static_cast<size_t>(id); }

  static std::string two(size_t i)
  {
    char b[8];
    std::snprintf(b, sizeof b, "%02zu", i);
    return b;
  }

  // Renders twice (cold, then with whatever the first render cached) and
  // fingerprints the first. Returns the cold result.
  static std::shared_ptr<CanvasImage> renderScenario(const std::string &name,
                                                     const AdjustmentSet &s,
                                                     const CanvasImage &src,
                                                     bool expectChange = true)
  {
    resetAllCaches();
    auto a = renderAdjusted(s, src);
    if (!a)
    {
      fail(name, "render returned null");
      return nullptr;
    }
    auto b = renderAdjusted(s, src);
    if (!b || a->pixels != b->pixels)
      fail(name, "warm-cache render differs from the cold render");
    if (a->width != src.width || a->height != src.height ||
        a->pixels.size() != src.pixels.size())
      fail(name, "render changed the picture's size");
    if (expectChange && a->pixels == src.pixels)
      note(name, "no visible effect on the test picture");
    putImage(name, *a);
    return a;
  }

  // ----------------------------------------------------------- mask builders
  static RadialShape radial()
  {
    RadialShape r;
    r.cx = 0.40f;
    r.cy = 0.45f;
    r.rx = 0.35f;
    r.ry = 0.22f;
    r.angle = 25.0f;
    r.feather = 0.5f;
    return r;
  }
  static LinearShape linear()
  {
    LinearShape l;
    l.cx = 0.5f;
    l.cy = 0.5f;
    l.len = 0.30f;
    l.angle = 35.0f;
    return l;
  }
  static BrushShape brush()
  {
    BrushShape b;
    BrushStroke s1;
    s1.size = 0.12f;
    s1.feather = 0.6f;
    for (int i = 0; i <= 24; ++i)
      s1.pts.push_back(BrushPt{0.10f + 0.80f * i / 24.0f, 0.20f + 0.60f * i / 24.0f});
    BrushStroke s2;
    s2.erase = true;
    s2.size = 0.06f;
    s2.feather = 0.3f;
    for (int i = 0; i <= 24; ++i)
      s2.pts.push_back(BrushPt{0.50f, 0.10f + 0.80f * i / 24.0f});
    BrushStroke s3;
    s3.size = 0.08f;
    s3.flow = 0.3f;
    s3.density = 0.7f;
    for (int i = 0; i <= 24; ++i)
      s3.pts.push_back(BrushPt{0.15f + 0.70f * i / 24.0f, 0.85f});
    b.strokes = {s1, s2, s3};
    return b;
  }
  static LuminosityShape luminosity()
  {
    LuminosityShape s;
    s.lo = 0.30f;
    s.hi = 0.80f;
    s.feather = 0.4f;
    return s;
  }
  static ColorRangeShape colorRange()
  {
    ColorRangeShape c;
    c.set = true;
    c.r = 30.0f / 255;
    c.g = 200.0f / 255;
    c.b = 60.0f / 255;
    c.range = 0.35f;
    c.feather = 0.5f;
    return c;
  }
  static ObjectShape object(const std::shared_ptr<const ObjectMap> &map)
  {
    ObjectShape o;
    o.map = map;
    o.prompts = {ObjectPrompt{0.45f, 0.5f, true}};
    o.feather = 0.5f;
    o.grow = 0.6f;
    return o;
  }

  // The "ordinary" effect every mask scenario applies, so each shape's
  // weights show up in the output: tone, color and detail sliders together.
  static void stdEffect(Mask &m)
  {
    m.adj[idx(AdjId::Exposure)] = 35;
    m.adj[idx(AdjId::Contrast)] = 25;
    m.adj[idx(AdjId::Saturation)] = -35;
    m.adj[idx(AdjId::Temperature)] = 30;
    m.adj[idx(AdjId::Texture)] = 40;
    m.adj[idx(AdjId::Clarity)] = 30;
  }
  // Every slider, Mix, Grade and the curves on one mask.
  static void fullEffect(Mask &m)
  {
    for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
    {
      const AdjustmentSpec &sp = kAdjustmentSpecs[i];
      m.adj[i] = (i % 2 && sp.min < 0) ? sp.min * 0.4f : sp.max * 0.4f;
    }
    m.mix[mixIndex(MixChan::Hue, 2)] = 30;
    m.mix[mixIndex(MixChan::Saturation, 3)] = -40;
    m.mix[mixIndex(MixChan::Luminance, 0)] = 25;
    m.grade.wheel[1].hue = 200;
    m.grade.wheel[1].sat = 40;
    m.grade.wheel[0].lum = -15;
    m.curves[0] = CurvePoints{{0, 0}, {120, 90}, {255, 255}};
    m.curves[2] = CurvePoints{{0, 0}, {100, 135}, {255, 255}};
  }
  static Mask makeMask(MaskShape shape, bool invert = false, float amount = 100.0f)
  {
    Mask m;
    m.parts.push_back(MaskPart{std::move(shape), MaskOp::Add});
    m.invert = invert;
    m.amount = amount;
    stdEffect(m);
    return m;
  }
  // radial, minus linear, plus brush, intersected with a luminosity range.
  static Mask composite(MaskOp second, MaskOp third, MaskOp fourth)
  {
    Mask m;
    m.parts = {MaskPart{radial(), MaskOp::Add}, MaskPart{linear(), second},
               MaskPart{brush(), third}, MaskPart{luminosity(), fourth}};
    stdEffect(m);
    return m;
  }

  // ---------------------------------------------------------------- scenarios
  static void sliders(const CanvasImage &img, int W, int H)
  {
    {
      AdjustmentSet s;
      init(s, W, H);
      auto out = renderScenario("adj/none", s, img, false);
      if (out && out->pixels != img.pixels)
        note("adj/none", "an untouched edit set does not reproduce the input exactly");
    }
    // Every slider on its own, up and (where the range has one) down.
    for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
    {
      const AdjustmentSpec &sp = kAdjustmentSpecs[i];
      std::string label(sp.label);
      std::replace(label.begin(), label.end(), ' ', '_');
      // dir 0: up, 1: down, 2: barely moved, 3: full range both ends. Small
      // and extreme values reach different branches (clamps, minimum radii).
      struct Step
      {
        const char *suffix;
        float v;
        bool needsNeg;
      };
      const Step steps[] = {{"/pos", sp.max * 0.6f, false},
                            {"/neg", sp.min * 0.6f, true},
                            {"/low", sp.max * 0.12f, false},
                            {"/full_pos", sp.max, false},
                            {"/full_neg", sp.min, true}};
      for (const Step &st : steps)
      {
        if (st.needsNeg && !(sp.min < 0))
          continue;
        AdjustmentSet s;
        init(s, W, H);
        s.set(sp.id, st.v);
        if (sp.id == AdjId::ColorizeHue) // Hue alone tints nothing
          s.set(AdjId::ColorizeAmount, 60.0f);
        renderScenario("adj/" + two(i) + "_" + label + st.suffix, s, img);
      }
    }
    for (int pol = 0; pol < 2; ++pol)
    {
      AdjustmentSet s;
      init(s, W, H);
      for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
      {
        const AdjustmentSpec &sp = kAdjustmentSpecs[i];
        s.set(sp.id, (pol == 1 && sp.min < 0) ? sp.min * 0.5f : sp.max * 0.5f);
      }
      renderScenario(pol == 0 ? "adj/all_pos" : "adj/all_neg", s, img);
    }
  }

  static void curvesMixGrade(const CanvasImage &img, int W, int H)
  {
    const CurvePoints master{{0, 0}, {64, 40}, {128, 150}, {200, 230}, {255, 255}};
    const CurvePoints red{{0, 0}, {90, 60}, {255, 255}};
    const CurvePoints green{{0, 0}, {100, 130}, {255, 255}};
    const CurvePoints blue{{0, 20}, {255, 235}};
    {
      AdjustmentSet s;
      init(s, W, H);
      s.setCurve(CurveChannel::Master, master, false);
      renderScenario("curve/master", s, img);
    }
    {
      AdjustmentSet s;
      init(s, W, H);
      s.setCurve(CurveChannel::Red, red, false);
      s.setCurve(CurveChannel::Green, green, false);
      s.setCurve(CurveChannel::Blue, blue, false);
      renderScenario("curve/rgb", s, img);
    }
    {
      AdjustmentSet s;
      init(s, W, H);
      s.setCurve(CurveChannel::Master, master, false);
      s.setCurve(CurveChannel::Red, red, false);
      s.setCurve(CurveChannel::Green, green, false);
      s.setCurve(CurveChannel::Blue, blue, false);
      s.set(AdjId::Contrast, 30);
      renderScenario("curve/all_plus_contrast", s, img);
    }

    const char *chanName[3] = {"hue", "sat", "lum"};
    for (size_t c = 0; c < kMixChanCount; ++c)
    {
      AdjustmentSet s;
      init(s, W, H);
      for (size_t b = 0; b < kMixBandCount; ++b)
        s.setMix(static_cast<MixChan>(c), b, (b % 2 ? 1.0f : -1.0f) * (20.0f + 8.0f * static_cast<float>(b)));
      renderScenario(std::string("mix/") + chanName[c], s, img);
    }
    {
      AdjustmentSet s;
      init(s, W, H);
      for (size_t c = 0; c < kMixChanCount; ++c)
        for (size_t b = 0; b < kMixBandCount; ++b)
          s.setMix(static_cast<MixChan>(c), b,
                   (((c + b) % 2) ? 1.0f : -1.0f) * (15.0f + 7.0f * static_cast<float>(b) + 5.0f * static_cast<float>(c)));
      renderScenario("mix/all", s, img);
    }

    const char *rangeName[kGradeRangeCount] = {"shadows", "midtones", "highlights", "global"};
    for (size_t r = 0; r < kGradeRangeCount; ++r)
    {
      AdjustmentSet s;
      init(s, W, H);
      ColorGrade g;
      g.wheel[r].hue = 20.0f + 90.0f * static_cast<float>(r);
      g.wheel[r].sat = 55.0f;
      g.wheel[r].lum = r % 2 ? 18.0f : -18.0f;
      s.setGrade(g, -1);
      renderScenario(std::string("grade/") + rangeName[r], s, img);
    }
    {
      ColorGrade g;
      for (size_t r = 0; r < kGradeRangeCount; ++r)
      {
        g.wheel[r].hue = 40.0f + 80.0f * static_cast<float>(r);
        g.wheel[r].sat = 25.0f + 15.0f * static_cast<float>(r);
        g.wheel[r].lum = r % 2 ? 12.0f : -12.0f;
      }
      g.blending = 70.0f;
      g.balance = -30.0f;
      AdjustmentSet s;
      init(s, W, H);
      s.setGrade(g, -1);
      renderScenario("grade/all_blend70_bal-30", s, img);
      g.wheel[1].visible = false; // the eye icon: this wheel is off
      AdjustmentSet t;
      init(t, W, H);
      t.setGrade(g, -1);
      renderScenario("grade/one_wheel_hidden", t, img);
    }
  }

  static void masks(const CanvasImage &img, int W, int H,
                    const std::shared_ptr<const ObjectMap> &blob, uint64_t segKey)
  {
    auto one = [&](const std::string &name, MaskSet ms, bool expectChange = true)
    {
      AdjustmentSet s;
      init(s, W, H);
      s.setMasks(ms, -1);
      renderScenario(name, s, img, expectChange);
    };

    // The stand-in segmenter's own output, used as an Object mask below.
    std::shared_ptr<const ObjectMap> wand;
    {
      std::string err;
      const std::vector<ObjectPrompt> pr = {{0.50f, 0.50f, true}, {0.20f, 0.90f, true}, {0.30f, 0.35f, false}};
      wand = segmentObjectWand(img, segKey, pr, &err, nullptr, 0);
      if (!wand)
        fail("seg/wand", "segmentObjectWand returned null: " + err);
      else
      {
        std::string t = std::to_string(wand->w) + "x" + std::to_string(wand->h) + ":";
        t.append(reinterpret_cast<const char *>(wand->a.data()), wand->a.size());
        putText("seg/wand_map", t);
      }
    }

    struct Kind
    {
      const char *name;
      MaskShape shape;
    };
    const Kind kinds[] = {
        {"radial", radial()},
        {"linear", linear()},
        {"brush", brush()},
        {"luminosity", luminosity()},
        {"colorrange", colorRange()},
        {"object_blob", object(blob)},
    };
    for (const Kind &k : kinds)
    {
      one(std::string("mask/") + k.name, {makeMask(k.shape)});
      one(std::string("mask/") + k.name + "_inv", {makeMask(k.shape, true)});
    }
    {
      ObjectShape o = object(blob);
      o.grow = 1.0f;
      o.feather = 0.0f;
      one("mask/object_blob_grow_max", {makeMask(o)});
      o.grow = -1.0f;
      one("mask/object_blob_shrink_max", {makeMask(o)});
      o.grow = 0.0f;
      o.feather = 1.0f;
      one("mask/object_blob_feather_max", {makeMask(o)});
      o.feather = 0.0f;
      one("mask/object_blob_plain", {makeMask(o)});
    }
    if (wand)
    {
      one("mask/object_wand", {makeMask(object(wand))});
      ObjectShape o = object(wand);
      o.grow = -0.5f;
      o.feather = 0.0f;
      one("mask/object_wand_shrunk", {makeMask(o)});
    }
    // Parameter extremes: hard and fully soft edges, the smallest and largest
    // radial masks, a one-point brush stroke, a zero-width luminosity range.
    {
      RadialShape r = radial();
      r.feather = 0.0f;
      one("mask/radial_hard_edge", {makeMask(r)});
      r.feather = 1.0f;
      one("mask/radial_fully_soft", {makeMask(r)});
      r = radial();
      r.rx = r.ry = kMaskMinRadius;
      one("mask/radial_min_radius", {makeMask(r)}, false);
      r.rx = r.ry = kMaskMaxRadius;
      one("mask/radial_max_radius", {makeMask(r)});
      LinearShape l = linear();
      l.len = 0.01f;
      one("mask/linear_hard", {makeMask(l)});
      l.len = 1.0f;
      l.angle = -150.0f;
      one("mask/linear_long_reversed", {makeMask(l)});
      BrushShape b = brush();
      for (BrushStroke &st : b.strokes)
        st.feather = 0.0f;
      one("mask/brush_hard", {makeMask(b)});
      BrushShape dot;
      BrushStroke d;
      d.pts = {BrushPt{0.5f, 0.5f}};
      dot.strokes = {d};
      one("mask/brush_single_dab", {makeMask(dot)});
      LuminosityShape lu = luminosity();
      lu.feather = 0.0f;
      one("mask/luminosity_hard", {makeMask(lu)});
      lu.lo = lu.hi = 0.5f;
      one("mask/luminosity_zero_width", {makeMask(lu)}, false);
      ColorRangeShape c = colorRange();
      c.feather = 0.0f;
      one("mask/colorrange_hard", {makeMask(c)});
      c.range = 1.0f;
      c.feather = 1.0f;
      one("mask/colorrange_widest", {makeMask(c)});
      c.set = false;
      one("mask/colorrange_unset", {makeMask(c)}, false);
    }
    one("mask/radial_amount40", {makeMask(radial(), false, 40.0f)});
    {
      Mask off = makeMask(radial());
      off.enabled = false;
      one("mask/disabled", {off}, false);
    }
    {
      Mask a = makeMask(radial());
      Mask b = makeMask(linear());
      b.adj[idx(AdjId::Exposure)] = -40;
      b.adj[idx(AdjId::Tint)] = 30;
      one("mask/two_overlapping", {a, b});
    }
    one("mask/composite_add_sub_add_isect",
        {composite(MaskOp::Subtract, MaskOp::Add, MaskOp::Intersect)});
    one("mask/composite_add_add_sub_isect",
        {composite(MaskOp::Add, MaskOp::Subtract, MaskOp::Intersect)});
    {
      Mask m = composite(MaskOp::Intersect, MaskOp::Subtract, MaskOp::Add);
      m.invert = true;
      m.amount = 70.0f;
      one("mask/composite_inverted_amount70", {m});
    }
    {
      Mask m = makeMask(radial());
      m.adj = AdjValues{};
      m.adj[idx(AdjId::Dehaze)] = 60;
      m.adj[idx(AdjId::NoiseLuma)] = 50;
      m.adj[idx(AdjId::NoiseColor)] = 50;
      m.adj[idx(AdjId::Blur)] = 40;
      m.adj[idx(AdjId::Sharpness)] = 50;
      one("mask/neighbor_fx_only", {m});
    }
    {
      Mask m = makeMask(radial());
      m.adj = AdjValues{};
      fullEffect(m);
      one("mask/every_effect", {m});
      Mask c = composite(MaskOp::Subtract, MaskOp::Add, MaskOp::Intersect);
      c.adj = AdjValues{};
      fullEffect(c);
      one("mask/composite_every_effect", {c});
    }
  }

  // ------------------------------------------------------------------ geometry
  // What syncSource() does, without the editor state: orient, straighten, crop.
  static std::shared_ptr<CanvasImage> geometryPipeline(const CanvasImage &original,
                                                       const AdjustmentSet &adj)
  {
    auto oriented = orientImage(std::make_shared<CanvasImage>(original), adj.orient());
    auto rotated = adj.angle() == 0.0f
                       ? oriented
                       : rotateArbitrary(*oriented, adj.angle(), adj.expand());
    return adj.crop().empty() ? rotated : cropImage(*rotated, adj.crop());
  }

  static void geometry(const CanvasImage &img, int W, int H,
                       const std::shared_ptr<const ObjectMap> &blob)
  {
    for (int i = 0; i < 8; ++i)
    {
      Orient o;
      o.rot = i % 4;
      o.flip = i >= 4;
      auto out = orientImage(std::make_shared<CanvasImage>(img), o);
      putImage("geo/orient_rot" + std::to_string(o.rot) + (o.flip ? "_flip" : ""), *out);
    }
    struct Rot
    {
      const char *name;
      float deg;
      bool expand;
    };
    for (const Rot &r : {Rot{"geo/rotate_7.5_fit", 7.5f, false}, Rot{"geo/rotate_7.5_expand", 7.5f, true},
                         Rot{"geo/rotate_-33_expand", -33.0f, true}, Rot{"geo/rotate_45_fit", 45.0f, false}})
    {
      auto out = rotateArbitrary(img, r.deg, r.expand);
      putImage(r.name, *out);
    }
    putImage("geo/crop", *cropImage(img, CropRect{W / 10, H / 8, W * 3 / 4, H * 3 / 4}));

    // The same mask set, rendered through three different geometries: the
    // masks are stored against the original and must follow the picture.
    MaskSet ms;
    ms.push_back(makeMask(radial()));
    ms.push_back(makeMask(linear(), false, 80.0f));
    ms.push_back(makeMask(brush()));
    ms.push_back(makeMask(object(blob)));
    ms.push_back(makeMask(luminosity()));
    ms.push_back(makeMask(colorRange()));

    struct Geo
    {
      const char *name;
      Orient o;
      float angle;
      bool expand;
      bool crop;
    };
    const Geo geos[] = {
        {"geo_masks/rot1_flip_-12_expand_crop", Orient{1, true}, -12.0f, true, true},
        {"geo_masks/rot3_20_fit", Orient{3, false}, 20.0f, false, false},
        {"geo_masks/rot2_crop", Orient{2, false}, 0.0f, false, true},
    };
    for (const Geo &g : geos)
    {
      AdjustmentSet s;
      init(s, W, H);
      const int ow = (g.o.rot % 2) ? H : W, oh = (g.o.rot % 2) ? W : H;
      const Dims d = rotatedDims(ow, oh, g.angle, g.expand);
      CropRect crop;
      if (g.crop)
        crop = CropRect{d.w / 10, d.h / 8, d.w * 3 / 4, d.h * 3 / 4};
      s.setExpand(g.expand);
      s.setGeometry(g.o, g.angle, crop);
      s.setMasks(ms, -1);
      auto src = geometryPipeline(img, s);
      if (!src)
      {
        fail(g.name, "geometry pipeline returned null");
        continue;
      }
      putImage(std::string(g.name) + "/source", *src);
      renderScenario(g.name, s, *src);
    }
  }

  // --------------------------------------------------------------- kitchen sink
  static void buildSink(AdjustmentSet &s, const std::shared_ptr<const ObjectMap> &blob)
  {
    for (size_t i = 0; i < kAdjustmentSpecs.size(); ++i)
    {
      const AdjustmentSpec &sp = kAdjustmentSpecs[i];
      s.set(sp.id, (i % 2 && sp.min < 0) ? sp.min * 0.5f : sp.max * 0.5f);
    }
    for (size_t c = 0; c < kMixChanCount; ++c)
      for (size_t b = 0; b < kMixBandCount; ++b)
        s.setMix(static_cast<MixChan>(c), b,
                 (((c + b) % 2) ? 1.0f : -1.0f) * (15.0f + 7.0f * static_cast<float>(b) + 5.0f * static_cast<float>(c)));
    ColorGrade g;
    for (size_t r = 0; r < kGradeRangeCount; ++r)
    {
      g.wheel[r].hue = 40.0f + 80.0f * static_cast<float>(r);
      g.wheel[r].sat = 25.0f + 15.0f * static_cast<float>(r);
      g.wheel[r].lum = r % 2 ? 12.0f : -12.0f;
    }
    g.blending = 65.0f;
    g.balance = -20.0f;
    s.setGrade(g, -1);
    s.setCurve(CurveChannel::Master, CurvePoints{{0, 0}, {64, 50}, {200, 220}, {255, 255}}, false);
    s.setCurve(CurveChannel::Red, CurvePoints{{0, 0}, {90, 70}, {255, 255}}, false);
    s.setCurve(CurveChannel::Blue, CurvePoints{{0, 10}, {255, 245}}, false);

    Mask m1 = composite(MaskOp::Subtract, MaskOp::Add, MaskOp::Intersect);
    m1.adj = AdjValues{};
    fullEffect(m1);
    Mask m2 = makeMask(object(blob), true, 70.0f);
    m2.adj[idx(AdjId::Dehaze)] = 30;
    m2.adj[idx(AdjId::Blur)] = 25;
    Mask m3 = makeMask(colorRange());
    s.setMasks({m1, m2, m3}, -1);
  }

  static void sink(const CanvasImage &img, int W, int H,
                   const std::shared_ptr<const ObjectMap> &blob, const std::string &tag,
                   bool extras)
  {
    AdjustmentSet s;
    init(s, W, H);
    buildSink(s, blob);
    auto ref = renderScenario("sink/" + tag, s, img);
    if (!ref || !extras)
      return;

    // The background path (latest / generation) must give the same bytes as
    // the synchronous one, and bail out once superseded.
    const RenderParams rp = s.buildRenderParams();
    resetAllCaches();
    std::atomic<uint64_t> latest{5};
    auto a = renderWith(img, rp, &latest, 5);
    if (!a || a->pixels != ref->pixels)
      fail("sink/" + tag, "async-path render differs from the synchronous render");
    latest.store(6);
    if (renderWith(img, rp, &latest, 5))
      fail("sink/" + tag, "a superseded render did not return null");

    // Sidecar: the JSON text itself, and that it loads back.
    const std::string text = s.toJson().dump();
    putText("sidecar/sink_json", text);
    mj::Json j;
    std::string err;
    if (!mj::Json::parse(text, &j, &err))
    {
      fail("sidecar", "own JSON does not parse: " + err);
      return;
    }
    AdjustmentSet t;
    init(t, W, H);
    if (!t.loadJson(j))
    {
      fail("sidecar", "own JSON is rejected by loadJson");
      return;
    }
    if (t.toJson().dump() != text)
      note("sidecar", "dump(load(dump)) is not identical to the first dump");
    resetAllCaches();
    auto back = renderAdjusted(t, img);
    if (!back || back->pixels != ref->pixels)
      note("sidecar", "render after a save/load round trip differs from the original render");
  }

  // ------------------------------------------------------------------- history
  static void history(const CanvasImage &img, int W, int H)
  {
    AdjustmentSet s;
    init(s, W, H);
    s.set(AdjId::Exposure, 20);
    s.set(AdjId::Exposure, 30); // same drag: folds into one step
    s.set(AdjId::Contrast, 10);
    s.setMix(MixChan::Hue, 1, 15);
    s.setCurve(CurveChannel::Master, CurvePoints{{0, 0}, {100, 80}, {255, 255}}, false);
    s.setMasks({makeMask(radial())}, -1);
    s.setGeometry(Orient{1, false}, 0.0f, CropRect{});
    s.undo();
    s.undo();
    s.redo();
    s.goToHistory(2);
    std::string t = "index=" + std::to_string(s.historyIndex()) + " size=" + std::to_string(s.historySize()) +
                    " undo=" + (s.canUndo() ? "1" : "0") + " redo=" + (s.canRedo() ? "1" : "0") + "\n";
    for (size_t i = 0; i < s.historySize(); ++i)
      t += s.historyLabel(i) + "\n";
    putText("history/labels", t);
    renderScenario("history/render_at_step2", s, img);
    s.goToHistory(s.historySize() - 1);
    putText("history/json_at_end", s.toJson().dump());
  }

  // ---------------------------------------------------------- helpers and misc
  static void helpers()
  {
    std::vector<std::string> names = {"IMG_10.jpg", "img_2.jpg", "IMG_1.jpg", "IMG_002.jpg", "a10b", "a9b",
                                      "A9c", "photo", "Photo2", "photo10", "007", "7", "08", "8", ""};
    std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b)
              { return naturalLess(a, b); });
    std::string t;
    for (const auto &n : names)
      t += n + "\n";
    putText("util/natural_order", t);

    std::string e;
    for (const char *p : {"a/b.c/IMG.JPG", "x.jpeg", "noext", "dir.d/file", "C:\\p\\q.PnG", ".hidden", "a.b.c.TIFF"})
      e += std::string(p) + " -> '" + fileExtLower(p) + "' jpeg=" + (isJpegPath(p) ? "1" : "0") +
           " base='" + baseName(p) + "'\n";
    putText("util/paths", e);
  }

  static void resampling(const CanvasImage &img)
  {
    if (auto th = makeThumb(img))
      putImage("thumb/scene", *th);
    else
      fail("thumb/scene", "makeThumb returned null");
    const CanvasImage wide = makeScene(1600, 24, 3); // long edge > 1.5 * kPreviewMaxEdge
    if (auto pv = makePreviewBase(wide))
      putImage("preview/wide_1600x24", *pv);
    else
      fail("preview/wide_1600x24", "makePreviewBase returned null for a large picture");
    if (makePreviewBase(img))
      fail("preview/small", "makePreviewBase returned a preview for a small picture");
  }

  // ------------------------------------------------ histogram and clip overlay
  // computeHistogram() (main.cpp) reads the editor state, so these scenarios
  // drive it through `state`: the picture on screen, the overlay switches, the
  // mid-drag preview. They fingerprint the four histograms, the clip bits and
  // the overlay picture, i.e. everything histogram.hpp produces.
  static std::string histogramText()
  {
    std::string t;
    auto row = [&t](const char *tag, const std::array<uint32_t, 256> &a)
    {
      t += tag;
      for (uint32_t v : a)
        t += ' ' + std::to_string(v);
      t += '\n';
    };
    row("R", state.histR);
    row("G", state.histG);
    row("B", state.histB);
    row("L", state.histL);
    t += "clipLo=" + std::to_string(static_cast<int>(state.histClipLo)) +
         " clipHi=" + std::to_string(static_cast<int>(state.histClipHi)) + "\n";
    return t;
  }

  static void histogramRecord(const std::string &name)
  {
    putText(name + "/counts", histogramText());
    if (state.clipOverlay)
      putImage(name + "/overlay", *state.clipOverlay);
    else
      putText(name + "/overlay", "null");
  }

  // flags: bit 0 shadows, bit 1 highlights, bit 2 per-channel
  static void histogramShot(const std::string &name, const CanvasImage &img, int flags)
  {
    state.image = std::make_shared<CanvasImage>(img);
    state.previewImage.reset();
    state.dragging = false;
    state.clipShadowsOn = (flags & 1) != 0;
    state.clipHighlightsOn = (flags & 2) != 0;
    state.clipPerChannel = (flags & 4) != 0;
    state.histStale = true;
    computeHistogram();
    histogramRecord(name);
  }

  // Every combination of clipped channels (columns 0..7 = channel set 0..7):
  // rows 0..7 clip high, rows 8..15 clip low, rows 16..19 are fully transparent
  // copies of the highs; column 8 holds a pixel that clips both ways.
  static CanvasImage clipPatterns()
  {
    CanvasImage p(72, 20);
    for (int y = 0; y < p.height; ++y)
      for (int x = 0; x < p.width; ++x)
      {
        uint8_t *q = &p.pixels[(static_cast<size_t>(y) * p.width + x) * 4];
        q[0] = q[1] = q[2] = 100;
        q[3] = y >= 16 ? 0 : 255;
        if (x >= 64)
        {
          if (y < 8)
          {
            q[0] = 255;
            q[2] = 0;
          }
          continue;
        }
        const int m = x / 8;
        const uint8_t level = (y < 8 || y >= 16) ? 255 : 0;
        if (m & 1)
          q[0] = level;
        if (m & 2)
          q[1] = level;
        if (m & 4)
          q[2] = level;
      }
    return p;
  }

  static void histogramScenarios(const CanvasImage &img)
  {
    const char *flagNames[] = {"off", "shadows", "highlights", "both", "perchannel_off", "perchannel_shadows",
                               "perchannel_highlights", "perchannel_both"};
    const int flagSets[] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 8; ++i)
    {
      histogramShot(std::string("histogram/scene_") + flagNames[i], img, flagSets[i]);
      histogramShot(std::string("histogram/patterns_") + flagNames[i], clipPatterns(), flagSets[i]);
    }

    // Larger than the 300000-sample budget: strided counting (stride 2). A
    // clipped pixel off the grid is missed by the histogram, not by the overlay.
    CanvasImage big = makeScene(1300, 1000, 7);
    for (int i = 0; i < 4; ++i)
    {
      uint8_t *q = &big.pixels[(static_cast<size_t>(1 + i * 40) * big.width + 1 + i * 40) * 4];
      q[0] = 255;
      q[1] = 0;
      q[2] = 128; // (1,1), (41,41), ...: odd coordinates, off the stride-2 grid
    }
    histogramShot("histogram/big_both", big, 3);
    histogramShot("histogram/big_perchannel", big, 7);

    // 768000 pixels: sqrt(768000 / 300000) = 1.6 truncates to stride 1.
    histogramShot("histogram/stride1_both", makeScene(1024, 750, 6), 3);

    // Wider than the 2048-pixel overlay cap: block reduction (2x2) as well.
    const CanvasImage wide = makeScene(2600, 600, 8);
    histogramShot("histogram/wide_both", wide, 3);
    histogramShot("histogram/wide_perchannel", wide, 7);

    // Nothing to count.
    histogramShot("histogram/empty_image", CanvasImage(), 3);
    state.image.reset();
    state.histStale = true;
    state.clipShadowsOn = state.clipHighlightsOn = true;
    computeHistogram();
    histogramRecord("histogram/no_image");

    // While a drag is in progress the preview stands in for the picture.
    state.image = std::make_shared<CanvasImage>(img);
    state.previewImage = std::make_shared<CanvasImage>(makeScene(100, 75, 9));
    state.dragging = true;
    state.clipShadowsOn = state.clipHighlightsOn = true;
    state.clipPerChannel = false;
    state.histStale = true;
    computeHistogram();
    histogramRecord("histogram/preview_dragging");

    // Leave the shared state as the other scenarios expect it.
    state.image.reset();
    state.previewImage.reset();
    state.dragging = false;
    state.clipShadowsOn = state.clipHighlightsOn = state.clipPerChannel = false;
    state.clipOverlay.reset();
    state.histStale = true;
  }

  static void runAll()
  {
    const int W = 200, H = 150; // 150 rows = two render blocks (128 + 22)
    const CanvasImage img = makeScene(W, H);
    const auto blob = makeBlobMap(256, 192);

    sliders(img, W, H);
    curvesMixGrade(img, W, H);
    masks(img, W, H, blob, 11);
    geometry(img, W, H, blob);
    sink(img, W, H, blob, "200x150", true);
    history(img, W, H);
    helpers();
    resampling(img);
    histogramScenarios(img);

    // A second, odd-sized picture: radii, blocks and maps that don't divide evenly.
    const int W2 = 97, H2 = 71;
    const CanvasImage odd = makeScene(W2, H2, 5);
    sink(odd, W2, H2, makeBlobMap(128, 96), "97x71", false);
    {
      AdjustmentSet s;
      init(s, W2, H2);
      s.setMasks({makeMask(radial()), makeMask(brush()), makeMask(linear())}, -1);
      s.set(AdjId::Clarity, 40);
      s.set(AdjId::Dehaze, 25);
      s.set(AdjId::NoiseLuma, 30);
      renderScenario("odd/masks_and_detail", s, odd);
    }
  }

  // ----------------------------------------------------------------- reporting
  static std::string hex(uint64_t v)
  {
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
  }
  static std::string line(const std::string &name, const Fp &f)
  {
    std::ostringstream o;
    o << name << ' ' << hex(f.hash) << ' ' << f.w << ' ' << f.h << ' ' << f.sum[0] << ' ' << f.sum[1]
      << ' ' << f.sum[2] << ' ' << f.sum[3];
    return o.str();
  }
  static std::string describe(const Fp &f)
  {
    std::ostringstream o;
    o << hex(f.hash) << "  " << f.w << "x" << f.h << "  sums R/G/B/A " << f.sum[0] << '/' << f.sum[1] << '/'
      << f.sum[2] << '/' << f.sum[3];
    return o.str();
  }

  static void dumpPpm(const std::string &dir, const std::string &name, const CanvasImage &img)
  {
    std::string file = name;
    std::replace(file.begin(), file.end(), '/', '_');
    std::ofstream o(std::filesystem::path(dir) / (file + ".ppm"), std::ios::binary);
    o << "P6\n"
      << img.width << ' ' << img.height << "\n255\n";
    for (size_t i = 0; i + 3 < img.pixels.size(); i += 4)
      o.write(reinterpret_cast<const char *>(&img.pixels[i]), 3);
  }

} // namespace golden

int main(int argc, char **argv)
{
  using namespace golden;
  bool record = false;
  std::string goldenFile, dumpDir;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    if (a == "--record")
      record = true;
    else if (a == "--dump" && i + 1 < argc)
      dumpDir = argv[++i];
    else
      goldenFile = a;
  }
  if (goldenFile.empty())
  {
    std::printf("usage: golden_render_test [--record] [--dump <dir>] <golden file>\n");
    return 2;
  }
  gKeepImages = !dumpDir.empty();

  runAll();

  if (gKeepImages)
  {
    std::filesystem::create_directories(dumpDir);
    for (const auto &kv : gImages)
      dumpPpm(dumpDir, kv.first, kv.second);
    std::printf("wrote %zu pictures to %s\n", gImages.size(), dumpDir.c_str());
  }

  int mismatches = 0;
  if (record)
  {
    std::filesystem::path p(goldenFile);
    if (p.has_parent_path())
      std::filesystem::create_directories(p.parent_path());
    std::ofstream o(p);
    o << "# literoom golden render fingerprints: name hash w h sumR sumG sumB sumA\n";
    for (const auto &r : gResults)
      o << line(r.first, r.second) << "\n";
    std::printf("recorded %zu scenarios to %s\n", gResults.size(), goldenFile.c_str());
  }
  else
  {
    std::ifstream in(goldenFile);
    if (!in)
    {
      std::printf("cannot open %s (record it first with --record)\n", goldenFile.c_str());
      return 2;
    }
    std::map<std::string, Fp> want;
    std::string ln;
    while (std::getline(in, ln))
    {
      if (ln.empty() || ln[0] == '#')
        continue;
      std::istringstream is(ln);
      std::string name, h;
      Fp f;
      is >> name >> h >> f.w >> f.h >> f.sum[0] >> f.sum[1] >> f.sum[2] >> f.sum[3];
      f.hash = std::strtoull(h.c_str(), nullptr, 16);
      want[name] = f;
    }
    for (const auto &r : gResults)
    {
      auto it = want.find(r.first);
      if (it == want.end())
      {
        std::printf("NEW      %s (not in the golden file; re-record if intended)\n", r.first.c_str());
        ++mismatches;
      }
      else if (!(it->second == r.second))
      {
        std::printf("MISMATCH %s\n   expected %s\n   got      %s\n", r.first.c_str(),
                    describe(it->second).c_str(), describe(r.second).c_str());
        ++mismatches;
      }
    }
    for (const auto &kv : want)
    {
      bool found = false;
      for (const auto &r : gResults)
        if (r.first == kv.first)
          found = true;
      if (!found)
      {
        std::printf("MISSING  %s (in the golden file, not produced now)\n", kv.first.c_str());
        ++mismatches;
      }
    }
  }

  for (const auto &f : gFailures)
    std::printf("INVARIANT FAILED  %s\n", f.c_str());
  for (const auto &n : gNotes)
    std::printf("note: %s\n", n.c_str());

  const bool ok = mismatches == 0 && gFailures.empty();
  std::printf("%zu scenarios, %d mismatches, %zu invariant failures, %zu notes\n", gResults.size(), mismatches,
              gFailures.size(), gNotes.size());
  std::printf(ok ? "all ok\n" : "FAILED\n");
  return ok ? 0 : 1;
}