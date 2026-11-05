// cases/render_bytes.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace render_bytes_cases
// so they cannot collide with other cases.
//
// renderWithBytes is the original byte pipeline, kept as the reference for the float one. These
// cases pin what it promises on its own: it never touches the source, a default RenderParams is NOT
// the identity (its tone tables are all zero), each stage lands where the header says, a picture
// rendered in 128-row blocks equals the same picture rendered in any other pieces, a cancelled
// render returns null, and masks and film effects are blended / applied in the documented order.
// Masks here have no shapes: an INVERTED mask with no parts weighs 1 - 0 = 1 on every pixel, which
// tests the blend without needing a shape. Real shapes are covered by the mask_* cases.

#include "render/render_bytes.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "color/color_pass.hpp"
#include "detail/film_fx.hpp"
#include "edit/render_params.hpp"
#include "mask/mask.hpp"

namespace render_bytes_cases
{
// A picture whose colour samples are spread over 0..maxLevel and whose alpha is a different pattern,
// so a stage that touches alpha, or swaps channels, shows up.
static CanvasImage makeImage(int w, int h, int maxLevel)
{
  CanvasImage img(w, h);
  for (size_t i = 0; i < img.pixels.size(); ++i)
  {
    if (i % 4 == 3)
      img.pixels[i] = static_cast<uint8_t>(255 - (i / 4) % 200);
    else
      img.pixels[i] = static_cast<uint8_t>((i * 37 + 11) % static_cast<size_t>(maxLevel + 1));
  }
  return img;
}

static bool same(const CanvasImage &a, const CanvasImage &b)
{
  return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

// Every colour sample of `out` equals f(source sample, channel); alpha equals the source's.
template <class F>
static bool mapsTo(const CanvasImage &out, const CanvasImage &src, F f)
{
  if (out.width != src.width || out.height != src.height || out.pixels.size() != src.pixels.size())
    return false;
  for (size_t i = 0; i < src.pixels.size(); ++i)
  {
    const int want = (i % 4 == 3) ? static_cast<int>(src.pixels[i])
                                  : f(static_cast<int>(src.pixels[i]), static_cast<int>(i % 4));
    if (static_cast<int>(out.pixels[i]) != want)
      return false;
  }
  return true;
}

// Sets the three tone tables of `luts` (RenderParams::luts or MaskPass::luts) to f(v, channel).
template <class Luts, class F>
static void fillLuts(Luts &luts, F f)
{
  for (int c = 0; c < 3; ++c)
    for (int k = 0; k < 256; ++k)
      luts[static_cast<size_t>(c)][static_cast<size_t>(k)] = static_cast<uint8_t>(f(k, c));
}

// A RenderParams that changes nothing. (RenderParams{} itself would paint everything black.)
static RenderParams identityParams()
{
  RenderParams rp;
  fillLuts(rp.luts, [](int k, int) { return k; });
  rp.color = makeColorPass(0, 0, 0, 0, MixValues{}, ColorGrade{});
  return rp;
}

// v + d, stopping at 255.
static int addSat(int v, int d) { return v + d > 255 ? 255 : v + d; }

// A mask with no shapes that is inverted: weight 1 everywhere, scaled by `amount`. Its tone tables
// add `d` (saturating) to every colour sample when `tone` is set.
static MaskPass wholeMask(int d, float amount = 1.0f, bool tone = true)
{
  MaskPass mp;
  mp.invert = true;
  mp.amount = amount;
  mp.tone = tone;
  fillLuts(mp.luts, [d](int k, int) { return addSat(k, d); });
  mp.color = makeColorPass(0, 0, 0, 0, MixValues{}, ColorGrade{});
  return mp;
}

// Rows [y0, y1) of `s` as a picture of their own.
static CanvasImage rowsOf(const CanvasImage &s, int y0, int y1)
{
  CanvasImage o(s.width, y1 - y0);
  const size_t rb = static_cast<size_t>(s.width) * 4;
  const uint8_t *from = s.pixels.data() + static_cast<size_t>(y0) * rb;
  for (size_t i = 0; i < o.pixels.size(); ++i)
    o.pixels[i] = from[i];
  return o;
}

TEST_CASE("render_bytes: identityAndCopy", "[render_bytes]")
{
  // 300 rows = blocks of 128 + 128 + 44: the last block is short, which is where a block loop breaks.
  const CanvasImage src = makeImage(5, 300, 255);
  const CanvasImage before = src;

  const auto out = renderWithBytes(src, identityParams());
  REQUIRE(out != nullptr);
  CHECK(same(*out, src)); // nothing to do: the same bytes, alpha included
  CHECK(out.get() != &src);

  CHECK(same(src, before)); // the source is never modified
  out->pixels[0] = static_cast<uint8_t>(out->pixels[0] + 1);
  CHECK(same(src, before)); // and the result is a copy, not a view of it
}

TEST_CASE("render_bytes: defaultParamsAreNotIdentity", "[render_bytes]")
{
  // The tables of a default RenderParams are all zero (value-initialised arrays), so every colour
  // sample becomes 0 while alpha stays. Anything that wants "no edit" must fill the tables - the
  // app does, in AdjustmentSet::buildRenderParams. This pins that, so nobody relies on the default.
  const CanvasImage src = makeImage(4, 4, 255);
  const auto out = renderWithBytes(src, RenderParams{});
  REQUIRE(out != nullptr);
  CHECK(mapsTo(*out, src, [](int, int) { return 0; }));
}

TEST_CASE("render_bytes: toneTables", "[render_bytes]")
{
  const CanvasImage src = makeImage(7, 150, 255);

  // Three equal tables take the "shared table" fast path.
  RenderParams invert = identityParams();
  fillLuts(invert.luts, [](int k, int) { return 255 - k; });
  const auto a = renderWithBytes(src, invert);
  REQUIRE(a != nullptr);
  CHECK(mapsTo(*a, src, [](int v, int) { return 255 - v; }));

  // Different tables per channel take the other path: R halves, G inverts, B stays.
  RenderParams mixed = identityParams();
  fillLuts(mixed.luts, [](int k, int c) { return c == 0 ? k / 2 : c == 1 ? 255 - k : k; });
  const auto b = renderWithBytes(src, mixed);
  REQUIRE(b != nullptr);
  CHECK(mapsTo(*b, src, [](int v, int c) { return c == 0 ? v / 2 : c == 1 ? 255 - v : v; }));
}

TEST_CASE("render_bytes: colorPassRunsAfterTone", "[render_bytes]")
{
  const CanvasImage src = makeImage(6, 140, 255);
  RenderParams rp = identityParams();
  fillLuts(rp.luts, [](int k, int) { return 255 - k; });
  rp.color = makeColorPass(0, 0, 0, 100, MixValues{}, ColorGrade{}); // saturation +100
  REQUIRE(rp.color.active);

  // Expected: the tone tables first, then the colour pass on the whole buffer.
  CanvasImage want = src;
  for (size_t i = 0; i + 3 < want.pixels.size(); i += 4)
    for (size_t c = 0; c < 3; ++c)
      want.pixels[i + c] = static_cast<uint8_t>(255 - want.pixels[i + c]);
  applyColorPass(rp.color, want.pixels.data(), want.pixels.size());

  const auto out = renderWithBytes(src, rp);
  REQUIRE(out != nullptr);
  CHECK(same(*out, want));
  CHECK(!same(*out, src)); // the pass really did something here
}

TEST_CASE("render_bytes: cancellation", "[render_bytes]")
{
  const CanvasImage src = makeImage(5, 300, 255);
  RenderParams rp = identityParams();
  fillLuts(rp.luts, [](int k, int) { return 255 - k; });
  const auto plain = renderWithBytes(src, rp);
  REQUIRE(plain != nullptr);

  std::atomic<uint64_t> latest{7};
  // The request is still the newest one: it runs to the end, with the same bytes as without `latest`.
  const auto current = renderWithBytes(src, rp, &latest, 7);
  REQUIRE(current != nullptr);
  CHECK(same(*current, *plain));

  // A newer request (or an invalidate) has moved `latest` on: this one is abandoned.
  CHECK(renderWithBytes(src, rp, &latest, 6) == nullptr);
  CHECK(renderWithBytes(src, rp, &latest, 8) == nullptr);

  // gen defaults to 0, so a counter at 0 means "current" for the default arguments too.
  std::atomic<uint64_t> zero{0};
  const auto fromZero = renderWithBytes(src, rp, &zero);
  REQUIRE(fromZero != nullptr);
  CHECK(same(*fromZero, *plain));
}

TEST_CASE("render_bytes: blocksDoNotChangeTheResult", "[render_bytes]")
{
  // Header: "Each pixel is independent, so blocking does not change the result." Everything here
  // is per pixel (tone tables per channel, colour pass, a whole-picture mask), so rendering a tall
  // picture in one go must equal rendering pieces of it cut at awkward rows and putting them back.
  const CanvasImage src = makeImage(6, 300, 100);
  RenderParams rp = identityParams();
  fillLuts(rp.luts, [](int k, int c) { return c == 0 ? 255 - k : c == 1 ? k / 2 : k; });
  rp.color = makeColorPass(0, 0, 0, 60, MixValues{}, ColorGrade{});
  rp.masks.push_back(wholeMask(30, 0.5f));

  const auto whole = renderWithBytes(src, rp);
  REQUIRE(whole != nullptr);

  const int cuts[] = {0, 1, 100, 128, 129, 255, 256, 300}; // across and on the 128-row block edges
  const size_t rb = static_cast<size_t>(src.width) * 4;
  size_t mismatches = 0;
  for (size_t k = 0; k + 1 < sizeof(cuts) / sizeof(cuts[0]); ++k)
  {
    const auto piece = renderWithBytes(rowsOf(src, cuts[k], cuts[k + 1]), rp);
    REQUIRE(piece != nullptr);
    const uint8_t *expect = whole->pixels.data() + static_cast<size_t>(cuts[k]) * rb;
    for (size_t i = 0; i < piece->pixels.size(); ++i)
      if (piece->pixels[i] != expect[i])
        ++mismatches;
  }
  CHECK(mismatches == 0u);
}

TEST_CASE("render_bytes: filmIsLastAndPositionAware", "[render_bytes]")
{
  // Vignette and Grain depend on where a pixel is in THIS picture, so each block is handed the index
  // of its first pixel. Rendering 6 x 300 (three blocks) must equal: tone tables, then ONE film pass
  // over the whole picture starting at pixel 0. A block that restarted its index would repeat the grain.
  const CanvasImage src = makeImage(6, 300, 255);
  RenderParams rp = identityParams();
  fillLuts(rp.luts, [](int k, int) { return 255 - k; });
  rp.film = film_fx::paramsFromValues(-50, 0, 0, 40, 0, 0);
  REQUIRE(film_fx::prepare(rp.film, src.width, src.height).any());

  CanvasImage want = src;
  for (size_t i = 0; i + 3 < want.pixels.size(); i += 4)
    for (size_t c = 0; c < 3; ++c)
      want.pixels[i + c] = static_cast<uint8_t>(255 - want.pixels[i + c]);
  film_fx::apply(film_fx::prepare(rp.film, src.width, src.height), want.pixels.data(),
                 static_cast<size_t>(src.width) * static_cast<size_t>(src.height), 0);

  const auto out = renderWithBytes(src, rp);
  REQUIRE(out != nullptr);
  CHECK(same(*out, want));

  // And it really is the film pass that made the difference.
  RenderParams noFilm = rp;
  noFilm.film = film_fx::Params{};
  const auto plain = renderWithBytes(src, noFilm);
  REQUIRE(plain != nullptr);
  CHECK(!same(*out, *plain));
}

TEST_CASE("render_bytes: maskBlend", "[render_bytes]")
{
  const CanvasImage src = makeImage(5, 200, 100); // colour 0..100, so "+100" never clips

  // A mask with no shapes that is NOT inverted selects nothing: skipped, the picture is untouched.
  {
    RenderParams rp = identityParams();
    MaskPass none = wholeMask(100);
    none.invert = false;
    rp.masks.push_back(none);
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(same(*out, src));
  }
  // Inverted: weight 1 everywhere. out = base + (adjusted - base) * weight.
  {
    RenderParams rp = identityParams();
    rp.masks.push_back(wholeMask(100, 1.0f));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(mapsTo(*out, src, [](int v, int) { return v + 100; })); // alpha untouched
  }
  // Amount scales the weight: half of +100 is +50 (0.5 is exact in float, so no rounding doubt).
  {
    RenderParams rp = identityParams();
    rp.masks.push_back(wholeMask(100, 0.5f));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(mapsTo(*out, src, [](int v, int) { return v + 50; }));
  }
  // Amount 0: nothing.
  {
    RenderParams rp = identityParams();
    rp.masks.push_back(wholeMask(100, 0.0f));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(same(*out, src));
  }
  // `tone` off: the mask's tone tables are not applied, whatever they hold.
  {
    RenderParams rp = identityParams();
    rp.masks.push_back(wholeMask(100, 1.0f, false));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(same(*out, src));
  }
  // Masks stack in list order: +20 then +20.
  {
    RenderParams rp = identityParams();
    rp.masks.push_back(wholeMask(20));
    rp.masks.push_back(wholeMask(20));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(mapsTo(*out, src, [](int v, int) { return v + 40; }));
  }
}

TEST_CASE("render_bytes: maskRunsAfterGlobalAndKeepsItsOwnColor", "[render_bytes]")
{
  const CanvasImage src = makeImage(4, 150, 100);

  // Global tables first (invert: 155..255), then the mask (+50, clipped at 255).
  {
    RenderParams rp = identityParams();
    fillLuts(rp.luts, [](int k, int) { return 255 - k; });
    rp.masks.push_back(wholeMask(50));
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(mapsTo(*out, src, [](int v, int) { return addSat(255 - v, 50); }));
  }
  // A mask's own colour pass (tone off) is applied to its copy and blended in at weight 1.
  {
    RenderParams rp = identityParams();
    MaskPass mp = wholeMask(0, 1.0f, false);
    mp.color = makeColorPass(0, 0, 0, 100, MixValues{}, ColorGrade{});
    REQUIRE(mp.color.active);
    rp.masks.push_back(mp);

    CanvasImage want = src;
    applyColorPass(mp.color, want.pixels.data(), want.pixels.size());
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK(same(*out, want));
    CHECK(!same(*out, src));
  }
}

TEST_CASE("render_bytes: emptyPictures", "[render_bytes]")
{
  // No pixels: a result with no pixels, the size kept, no stage touching memory. The params are
  // as busy as they can be without neighbor effects, to reach the mask and film set-up too.
  RenderParams rp = identityParams();
  rp.masks.push_back(wholeMask(10));
  rp.film = film_fx::paramsFromValues(-50, 0, 0, 40, 0, 0);

  const int sizes[][2] = {{0, 0}, {0, 5}, {5, 0}};
  for (const auto &s : sizes)
  {
    const CanvasImage src(s[0], s[1]);
    const auto out = renderWithBytes(src, rp);
    REQUIRE(out != nullptr);
    CHECK((out->width == s[0] && out->height == s[1] && out->pixels.empty()));
  }
}
} // namespace render_bytes_cases
