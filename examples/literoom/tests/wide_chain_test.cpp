// wide_chain_test.cpp - the 16-bit twin of the editor's picture chain (core/wide_chain.hpp): every stage has
// the size of its 8-bit namesake, equals it (widened) wherever the operation only moves pixels, and is
// within one 8-bit rounding of it where it resamples. Links liteui like raster_ops_generic_test.
#include "check.hpp"
#include "core/wide_chain.hpp"

#include <random>

using Img = CanvasImage;

static Img scene(int w, int h, uint32_t seed)
{
  Img im(w, h);
  std::mt19937 g(seed);
  for (uint8_t &v : im.pixels)
    v = static_cast<uint8_t>(g());
  return im;
}
static Image16 widen(const Img &a)
{
  Image16 o(a.width, a.height);
  widenSamples(a.pixels.data(), o.pixels.data(), a.pixels.size());
  return o;
}
static int maxDiff(const Image16 &a, const Image16 &b)
{
  if (a.pixels.size() != b.pixels.size())
    return 1 << 30;
  int m = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i)
    m = std::max(m, std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i])));
  return m;
}

// The 8-bit chain exactly as syncSource builds it.
struct Chain8
{
  std::shared_ptr<Img> oriented, rotated, source;
};
static Chain8 build8(const std::shared_ptr<Img> &original, const Orient &o, float angle, bool expand, const CropRect &crop)
{
  Chain8 c;
  c.oriented = orientImage(original, o);
  c.rotated = angle == 0.0f ? c.oriented : rotateArbitrary(*c.oriented, angle, expand);
  c.source = crop.empty() ? c.rotated : cropImage(*c.rotated, crop);
  return c;
}

static void testTwinMirrorsTheEightBitChain()
{
  std::mt19937 g(11);
  int combos = 0;
  for (int rep = 0; rep < 60; ++rep)
  {
    const int w = 20 + static_cast<int>(g() % 60), h = 15 + static_cast<int>(g() % 50);
    const auto o8 = std::make_shared<Img>(scene(w, h, 100 + static_cast<uint32_t>(rep)));
    const auto o16 = std::make_shared<Image16>(widen(*o8));
    const Orient o{static_cast<int>(g() % 4), (g() & 1) != 0};
    const float angle = (g() % 3 == 0) ? 0.0f : (static_cast<float>(g() % 900) / 10.0f - 45.0f);
    const bool expand = (g() & 1) != 0;
    CropRect crop;
    if (g() % 3 != 0)
      crop = CropRect{static_cast<int>(g() % 8), static_cast<int>(g() % 8), 5 + static_cast<int>(g() % 30), 5 + static_cast<int>(g() % 30)};

    const Chain8 c8 = build8(o8, o, angle, expand, crop);
    const auto t_or = wide_chain::oriented(o16, o);
    const auto t_ro = wide_chain::rotated(t_or, angle, expand);
    const auto t_so = wide_chain::cropped(t_ro, crop);
    CHECK(t_or && t_ro && t_so);
    if (!(t_or && t_ro && t_so))
      continue;
    // The sizes are the whole contract: the 8-bit chain is the view of the twin.
    CHECK(t_or->width == c8.oriented->width && t_or->height == c8.oriented->height);
    CHECK(t_ro->width == c8.rotated->width && t_ro->height == c8.rotated->height);
    CHECK(t_so->width == c8.source->width && t_so->height == c8.source->height);
    // Content: exact where pixels only move, within one 8-bit rounding where they are resampled.
    CHECK(maxDiff(*t_or, widen(*c8.oriented)) == 0);
    const int tol = angle == 0.0f ? 0 : 131;
    CHECK(maxDiff(*t_ro, widen(*c8.rotated)) <= tol);
    CHECK(maxDiff(*t_so, widen(*c8.source)) <= tol);
    // Identity stages hand back the very same object (no copy), like the 8-bit chain.
    if (o.identity())
      CHECK(t_or == o16);
    if (angle == 0.0f)
      CHECK(t_ro == t_or);
    if (crop.empty())
      CHECK(t_so == t_ro);
    ++combos;
  }
  CHECK(combos == 60);
}

static void testNullAndCancellation()
{
  const wide_chain::Wide none;
  CHECK(wide_chain::oriented(none, Orient{1, true}) == nullptr);
  CHECK(wide_chain::rotated(none, 12.0f, true) == nullptr);
  CHECK(wide_chain::cropped(none, CropRect{1, 1, 4, 4}) == nullptr);
  // An 8-bit file has no twin: every stage stays null from the original on.
  CHECK(wide_chain::cropped(wide_chain::rotated(wide_chain::oriented(none, Orient{}), 0.0f, false), CropRect{}) == nullptr);
  // A superseded straighten gives null from a non-null input (so a caller must check its input first).
  const auto big = std::make_shared<Image16>(widen(scene(40, 300, 3)));
  std::atomic<uint64_t> latest{5};
  CHECK(wide_chain::rotated(big, 7.0f, true, &latest, 5) != nullptr);
  CHECK(wide_chain::rotated(big, 7.0f, true, &latest, 4) == nullptr);
  CHECK(wide_chain::rotated(big, 0.0f, true, &latest, 4) == big); // 0 degrees: nothing to supersede
}

int main()
{
  testTwinMirrorsTheEightBitChain();
  testNullAndCancellation();
  return finish();
}
