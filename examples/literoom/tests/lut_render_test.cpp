// Build: links liteui (see CMakeLists.txt); needs only CanvasImage, no window.
//
// The 3D LUT through the real pixel pipeline (render.hpp): what it does to a
// picture, and where it sits among the other stages. Unlike the golden test this
// compares numbers (not fingerprints), so it also says WHY something is wrong.
#include "liteui.hpp"
#include "render/render.hpp"
#include "io/lut_library.hpp"
#include "check.hpp"

#include <cstdlib>
#include <filesystem>

static CanvasImage makeImage(int w, int h)
{
  CanvasImage img(w, h);
  img.pixels.resize(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
    {
      uint8_t *p = &img.pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
      p[1] = static_cast<uint8_t>(y * 255 / std::max(1, h - 1));
      p[2] = static_cast<uint8_t>((x * 7 + y * 3) % 256);
      p[3] = 255;
    }
  return img;
}

static AdjustmentSet fresh(int w, int h)
{
  AdjustmentSet s;
  s.reset();
  s.setOriginalSize(w, h);
  return s;
}

// Points the library at a folder that doesn't exist and caches two tables by hand.
struct LutEnv
{
  std::shared_ptr<const Lut3D> inv, ident;
  LutEnv()
  {
    lutDirOverride() = (std::filesystem::temp_directory_path() / "literoom_lut_render_nonexistent").string();
    clearLutCache();
    Lut3D L = identityLut(2);
    for (float &v : L.rgb)
      v = 255.0f - v;
    inv = std::make_shared<const Lut3D>(std::move(L));
    ident = std::make_shared<const Lut3D>(identityLut(33));
    lutCachePut("Invert", inv, 11);
    lutCachePut("Same", ident, 12);
  }
  ~LutEnv()
  {
    lutDirOverride().clear();
    clearLutCache();
  }
};

static int worst(const CanvasImage &a, const CanvasImage &b)
{
  if (a.pixels.size() != b.pixels.size())
    return 1000;
  int w = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i)
    w = std::max(w, std::abs(int(a.pixels[i]) - int(b.pixels[i])));
  return w;
}

static Mask wholePictureMask()
{
  Mask m;
  RadialShape r;
  r.cx = 0.5f;
  r.cy = 0.5f;
  r.rx = 0.4f;
  r.ry = 0.3f;
  r.feather = 0.5f;
  m.parts.push_back({r, MaskOp::Add});
  m.adj[static_cast<size_t>(AdjId::Exposure)] = 30.0f;
  m.adj[static_cast<size_t>(AdjId::Saturation)] = -25.0f;
  return m;
}

static void testLutAppliesToThePicture()
{
  LutEnv env;
  const int W = 200, H = 150; // 150 rows = two render blocks (128 + 22)
  const CanvasImage src = makeImage(W, H);
  AdjustmentSet s = fresh(W, H);
  CHECK(s.setLut(LutRef{"Invert", 11}));
  const auto out = renderAdjusted(s, src);
  CHECK(out && out->width == W && out->height == H);
  if (!out)
    return;
  for (size_t i = 0; i < src.pixels.size(); ++i)
  {
    if (i % 4 == 3)
      CHECK(out->pixels[i] == src.pixels[i]); // alpha untouched
    else
      CHECK(std::abs(int(out->pixels[i]) - (255 - int(src.pixels[i]))) <= 1);
  }
  // Half amount: halfway between the picture and its negative.
  s.set(AdjId::LutAmount, 50.0f);
  const auto half = renderAdjusted(s, src);
  CHECK(half != nullptr);
  if (half)
    for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
      for (size_t k = 0; k < 3; ++k)
      {
        const float want = 0.5f * static_cast<float>(src.pixels[i + k]) + 0.5f * static_cast<float>(255 - src.pixels[i + k]);
        CHECK(std::abs(static_cast<float>(half->pixels[i + k]) - want) <= 1.01f);
      }
  // The source picture is never modified.
  CHECK(src.pixels == makeImage(W, H).pixels);
}

static void testNothingToDoMeansNothingChanges()
{
  LutEnv env;
  const int W = 64, H = 48;
  const CanvasImage src = makeImage(W, H);
  const auto plain = renderAdjusted(fresh(W, H), src);
  CHECK(plain && plain->pixels == src.pixels);

  AdjustmentSet zero = fresh(W, H); // chosen, amount 0
  zero.setLut(LutRef{"Invert", 11});
  zero.set(AdjId::LutAmount, 0.0f);
  const auto a = renderAdjusted(zero, src);
  CHECK(a && a->pixels == src.pixels);

  AdjustmentSet missing = fresh(W, H); // not in the library
  missing.setLut(LutRef{"Nowhere", 1});
  CHECK(missing.lutStatus() == LutStatus::Missing);
  const auto m = renderAdjusted(missing, src);
  CHECK(m && m->pixels == src.pixels);

  AdjustmentSet noLut = fresh(W, H); // an amount alone
  noLut.set(AdjId::LutAmount, 100.0f);
  const auto n = renderAdjusted(noLut, src);
  CHECK(n && n->pixels == src.pixels);

  AdjustmentSet same = fresh(W, H); // an identity table
  same.setLut(LutRef{"Same", 12});
  const auto id = renderAdjusted(same, src);
  CHECK(id && worst(*id, src) <= 1);
}

static void testLutIsTheLastColorStep()
{
  LutEnv env;
  const int W = 64, H = 48;
  const CanvasImage src = makeImage(W, H);
  // After the Filters: Negative, then an inverting LUT, is the picture again.
  AdjustmentSet s = fresh(W, H);
  s.set(AdjId::Negative, 100.0f);
  s.setLut(LutRef{"Invert", 11});
  const auto both = renderAdjusted(s, src);
  CHECK(both && worst(*both, src) <= 2);

  // After the other color and tone stages: LUT(render without LUT) == render with LUT.
  AdjustmentSet base = fresh(W, H);
  base.set(AdjId::Exposure, 20.0f);
  base.set(AdjId::Contrast, 25.0f);
  base.set(AdjId::Saturation, 30.0f);
  base.set(AdjId::Temperature, 15.0f);
  base.set(AdjId::Sepia, 25.0f);
  const auto withoutLut = renderAdjusted(base, src);
  AdjustmentSet withLut = base;
  withLut.setLut(LutRef{"Invert", 11});
  const auto withIt = renderAdjusted(withLut, src);
  CHECK(withoutLut && withIt);
  if (withoutLut && withIt)
    for (size_t i = 0; i + 3 < src.pixels.size(); i += 4)
      for (size_t k = 0; k < 3; ++k)
        CHECK(std::abs(int(withIt->pixels[i + k]) - (255 - int(withoutLut->pixels[i + k]))) <= 1);
}

static void testLutComesBeforeMasksAndFilm()
{
  LutEnv env;
  const int W = 120, H = 90;
  const CanvasImage src = makeImage(W, H);

  // LUT, then a mask: the mask sees the LUT'd picture. Rendering "LUT only" and then running
  // the mask on THAT must give the same bytes as doing both in one go.
  AdjustmentSet lutOnly = fresh(W, H);
  lutOnly.setLut(LutRef{"Invert", 11});
  const auto step1 = renderAdjusted(lutOnly, src);
  AdjustmentSet maskOnly = fresh(W, H);
  maskOnly.setMasks({wholePictureMask()}, -1);
  AdjustmentSet one = maskOnly;
  one.setLut(LutRef{"Invert", 11});
  const auto together = renderAdjusted(one, src);
  CHECK(step1 && together);
  if (step1 && together)
  {
    const auto step2 = renderAdjusted(maskOnly, *step1);
    CHECK(step2 && step2->pixels == together->pixels);
    CHECK(together->pixels != step1->pixels); // the mask really did something
  }

  // LUT, then Vignette and Grain (the last step of all).
  AdjustmentSet film = fresh(W, H);
  film.set(AdjId::Vignette, -50.0f);
  film.set(AdjId::Grain, 30.0f);
  AdjustmentSet filmAndLut = film;
  filmAndLut.setLut(LutRef{"Invert", 11});
  const auto fa = renderAdjusted(filmAndLut, src);
  const auto fb = step1 ? renderAdjusted(film, *step1) : nullptr;
  CHECK(fa && fb && fa->pixels == fb->pixels);
}

static void testBlocksAndPreviewAgree()
{
  LutEnv env;
  // A LUT is per pixel, so how the picture is cut into blocks of rows changes nothing: a tall
  // picture (several blocks) and the same rows rendered as separate pictures agree.
  const int W = 40, H = 300;
  const CanvasImage tall = makeImage(W, H);
  AdjustmentSet s = fresh(W, H);
  s.setLut(LutRef{"Invert", 11});
  s.set(AdjId::LutAmount, 70.0f);
  s.set(AdjId::Saturation, 20.0f);
  const auto whole = renderAdjusted(s, tall);
  CHECK(whole);
  if (!whole)
    return;
  for (int y0 : {0, 100, 250})
  {
    CanvasImage part(W, 50);
    part.pixels.assign(tall.pixels.begin() + static_cast<std::ptrdiff_t>(y0) * W * 4,
                       tall.pixels.begin() + static_cast<std::ptrdiff_t>(y0 + 50) * W * 4);
    AdjustmentSet t = fresh(W, 50);
    t.setLut(LutRef{"Invert", 11});
    t.set(AdjId::LutAmount, 70.0f);
    t.set(AdjId::Saturation, 20.0f);
    const auto r = renderAdjusted(t, part);
    CHECK(r && std::equal(r->pixels.begin(), r->pixels.end(), whole->pixels.begin() + static_cast<std::ptrdiff_t>(y0) * W * 4));
  }
}

int main()
{
  testLutAppliesToThePicture();
  testNothingToDoMeansNothingChanges();
  testLutIsTheLastColorStep();
  testLutComesBeforeMasksAndFilm();
  testBlocksAndPreviewAgree();
  return finish();
}
