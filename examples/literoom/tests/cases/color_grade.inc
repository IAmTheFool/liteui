// cases/color_grade.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace color_grade_cases
// so they cannot collide with other cases.

#include "color/color_grade.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace color_grade_cases
{
static constexpr size_t S = static_cast<size_t>(GradeRange::Shadows);
static constexpr size_t M = static_cast<size_t>(GradeRange::Midtones);
static constexpr size_t H = static_cast<size_t>(GradeRange::Highlights);
static constexpr size_t G = static_cast<size_t>(GradeRange::Global);

TEST_CASE("color_grade: hueToRgb", "[color_grade]")
{
  struct Case
  {
    float hue, r, g, b;
  };
  const Case cases[] = {{0, 1, 0, 0},   {30, 1, 0.5f, 0}, {60, 1, 1, 0},   {120, 0, 1, 0},
                        {180, 0, 1, 1}, {240, 0, 0, 1},   {300, 1, 0, 1},  {360, 1, 0, 0},
                        {-60, 1, 0, 1}, {720, 1, 0, 0},   {-300, 1, 1, 0}};
  for (const Case &c : cases)
  {
    float rgb[3];
    gradeHueToRgb(c.hue, rgb);
    CHECK(rgb[0] == within(c.r, 1e-5));
    CHECK(rgb[1] == within(c.g, 1e-5));
    CHECK(rgb[2] == within(c.b, 1e-5));
  }
  // Always fully saturated: the largest channel is 1, the smallest 0.
  for (int h = -400; h <= 800; h += 7)
  {
    float rgb[3];
    gradeHueToRgb(static_cast<float>(h), rgb);
    const float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
    CHECK(mx == within(1.0f, 1e-5));
    CHECK(mn == within(0.0f, 1e-5));
  }
}

TEST_CASE("color_grade: state", "[color_grade]")
{
  ColorGrade g;
  CHECK(!g.anyActive());
  CHECK(g == ColorGrade{});
  CHECK((g.wheel[0].neutral() && g.wheel[0].visible));
  g.wheel[M].sat = 10.0f;
  CHECK((!g.wheel[M].neutral() && g.anyActive()));
  g.wheel[M].visible = false; // switched off: not active any more
  CHECK(!g.anyActive());
  g.wheel[M].visible = true;
  g.wheel[M].sat = 0.0f;
  g.wheel[M].lum = -5.0f; // luminance alone counts
  CHECK(g.anyActive());
  // Hue alone (sat = 0, lum = 0) does nothing, so it is "neutral".
  ColorGrade h;
  h.wheel[S].hue = 200.0f;
  CHECK(!h.anyActive());
  CHECK(!(g == h));
}

TEST_CASE("color_grade: neutralIsZero", "[color_grade]")
{
  const GradeLut lut = buildGradeLut(ColorGrade{});
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < 256; ++i)
      CHECK(lut[static_cast<size_t>(c)][static_cast<size_t>(i)] == 0.0f);
}

TEST_CASE("color_grade: luminanceOffsets", "[color_grade]")
{
  const float full = kGradeLumAmount * 255.0f;
  // Global applies everywhere equally.
  {
    ColorGrade g;
    g.wheel[G].lum = 100.0f;
    const GradeLut lut = buildGradeLut(g);
    for (int c = 0; c < 3; ++c)
      for (int i = 0; i < 256; ++i)
        CHECK(lut[static_cast<size_t>(c)][static_cast<size_t>(i)] == within(full, 1e-3));
  }
  // Shadows act on the dark end, Highlights on the bright end, Midtones in between.
  ColorGrade s, h, m;
  s.wheel[S].lum = 100.0f;
  h.wheel[H].lum = 100.0f;
  m.wheel[M].lum = 100.0f;
  const GradeLut ls = buildGradeLut(s), lh = buildGradeLut(h), lm = buildGradeLut(m);
  CHECK(ls[0][0] == within(full, 1e-3));
  CHECK(ls[0][255] == within(0.0f, 1e-3));
  CHECK(lh[0][0] == within(0.0f, 1e-3));
  CHECK(lh[0][255] == within(full, 1e-3));
  CHECK(lm[0][0] == within(0.0f, 1e-3));
  CHECK(lm[0][255] == within(0.0f, 1e-3));
  CHECK(lm[0][128] > 0.5f * full); // mostly midtone at the middle
  // Shadows' influence only ever falls with brightness; Highlights' only rises.
  for (int i = 1; i < 256; ++i)
  {
    CHECK(ls[0][static_cast<size_t>(i)] <= ls[0][static_cast<size_t>(i - 1)] + 1e-4f);
    CHECK(lh[0][static_cast<size_t>(i)] >= lh[0][static_cast<size_t>(i - 1)] - 1e-4f);
  }
  // The three ranges always add up to 1: all three at +100 equals one Global at +100.
  ColorGrade all;
  all.wheel[S].lum = all.wheel[M].lum = all.wheel[H].lum = 100.0f;
  const GradeLut la = buildGradeLut(all);
  for (int i = 0; i < 256; ++i)
    CHECK(la[1][static_cast<size_t>(i)] == within(full, 1e-3));
  // Sign flips the offsets.
  ColorGrade neg;
  neg.wheel[S].lum = -100.0f;
  CHECK(buildGradeLut(neg)[0][0] == within(-full, 1e-3));
}

TEST_CASE("color_grade: tintIsLumaNeutral", "[color_grade]")
{
  // A tint shifts color, not brightness: weighted by Rec.601 luma, the three
  // channel offsets cancel at every level, whatever the hue.
  for (float hue : {0.0f, 45.0f, 90.0f, 200.0f, 275.0f, 330.0f})
    for (size_t r : {S, M, H, G})
    {
      ColorGrade g;
      g.wheel[r].hue = hue;
      g.wheel[r].sat = 100.0f;
      const GradeLut lut = buildGradeLut(g);
      float peak = 0.0f;
      for (size_t i = 0; i < 256; ++i)
      {
        const float net = 0.299f * lut[0][i] + 0.587f * lut[1][i] + 0.114f * lut[2][i];
        CHECK(net == within(0.0f, 1e-3));
        peak = std::max({peak, std::fabs(lut[0][i]), std::fabs(lut[1][i]), std::fabs(lut[2][i])});
      }
      CHECK(peak > 1.0f); // and it does tint something
    }
  // Red tint: R up, G and B down. Strength follows saturation.
  ColorGrade g;
  g.wheel[G].hue = 0.0f;
  g.wheel[G].sat = 100.0f;
  GradeLut lut = buildGradeLut(g);
  CHECK((lut[0][100] > 0.0f && lut[1][100] < 0.0f && lut[2][100] < 0.0f));
  const float full = lut[0][100];
  g.wheel[G].sat = 50.0f;
  lut = buildGradeLut(g);
  CHECK(lut[0][100] == within(full * 0.5f, 1e-3));
}

TEST_CASE("color_grade: hiddenWheelAndTuning", "[color_grade]")
{
  ColorGrade g;
  g.wheel[G].lum = 100.0f;
  g.wheel[G].visible = false;
  const GradeLut off = buildGradeLut(g);
  for (int i = 0; i < 256; ++i)
    CHECK(off[0][static_cast<size_t>(i)] == 0.0f);

  // Balance slides the crossovers: toward - the shadows wheel reaches further
  // up into the picture, toward + the highlights wheel reaches further down.
  ColorGrade a, b;
  a.wheel[S].lum = b.wheel[S].lum = 100.0f;
  a.balance = -100.0f;
  b.balance = 100.0f;
  CHECK(buildGradeLut(a)[0][128] > buildGradeLut(b)[0][128]);
  ColorGrade c, d;
  c.wheel[H].lum = d.wheel[H].lum = 100.0f;
  c.balance = -100.0f;
  d.balance = 100.0f;
  CHECK(buildGradeLut(c)[0][128] < buildGradeLut(d)[0][128]);
  // Blending widens the overlap: with more blending the shadows wheel reaches
  // further into the mids. (lum 30, not 100: at +100 with Blending 0 the
  // monotonic clamp holds the curve on a plateau and masks the difference.)
  ColorGrade lo, hi;
  lo.wheel[S].lum = hi.wheel[S].lum = 30.0f;
  lo.blending = 0.0f;
  hi.blending = 100.0f;
  CHECK(buildGradeLut(hi)[0][110] > buildGradeLut(lo)[0][110]);
}
} // namespace color_grade_cases
