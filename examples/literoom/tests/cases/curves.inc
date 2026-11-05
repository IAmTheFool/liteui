// cases/curves.inc - included from literoom_test.cpp: no main(), no check.hpp, no
// #define LITEUI_IMPLEMENTATION. Helpers and TEST_CASEs live in namespace curves_cases
// so they cannot collide with other cases.

#include "color/curves.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace curves_cases
{
TEST_CASE("curves: identity", "[curves]")
{
  const CurvePoints id = identityCurve();
  CHECK((id.size() == 2 && id[0].x == 0 && id[0].y == 0 && id[1].x == 255 && id[1].y == 255));
  const CurveSet set = identityCurves();
  CHECK((set.size() == kCurveChannelCount && kCurveChannelCount == 4));
  for (const CurvePoints &c : set)
    CHECK(c == id);
  const auto lut = curveToLut(id);
  for (int i = 0; i < 256; ++i)
    CHECK(lut[static_cast<size_t>(i)] == within(i, 1e-3));
  CHECK(curveIsIdentity(lut));
  // Fewer than two points: nothing to interpolate, so identity.
  CHECK(curveIsIdentity(curveToLut({})));
  CHECK(curveIsIdentity(curveToLut({{10.0f, 200.0f}})));
}

TEST_CASE("curves: shape", "[curves]")
{
  // It passes through its points (whole-number x), end points hold beyond themselves.
  const CurvePoints p{{0, 20}, {64, 40}, {128, 180}, {200, 210}, {255, 250}};
  const auto lut = curveToLut(p);
  for (const CurvePoint &c : p)
    CHECK(lut[static_cast<size_t>(c.x)] == within(c.y, 1e-3));
  CHECK(lut[0] == within(20, 1e-3));
  CHECK(lut[255] == within(250, 1e-3));
  CHECK(!curveIsIdentity(lut));
  // End points that don't sit at 0 / 255 are held flat outside them.
  const auto inner = curveToLut({{50, 100}, {200, 150}});
  CHECK((inner[0] == 100.0f && inner[49] == 100.0f && inner[255] == 150.0f && inner[201] == 150.0f));
  // Two points: a straight line.
  const auto line = curveToLut({{0, 10}, {255, 200}});
  for (int i = 0; i < 256; ++i)
    CHECK(line[static_cast<size_t>(i)] == within(10.0 + 190.0 * i / 255.0, 1e-2));
  // Flat is flat; outputs are always clamped to 0..255.
  const auto flat = curveToLut({{0, 100}, {80, 100}, {255, 100}});
  for (float v : flat)
    CHECK(v == within(100, 1e-4));
  const auto hot = curveToLut({{0, -50}, {255, 400}});
  CHECK((hot[0] == 0.0f && hot[255] == 255.0f));
  for (float v : hot)
    CHECK((v >= 0.0f && v <= 255.0f));
}

TEST_CASE("curves: identityDetection", "[curves]")
{
  auto lut = curveToLut(identityCurve());
  lut[100] += 0.4f; // rounds back to 100: still identity
  CHECK(curveIsIdentity(lut));
  lut[100] += 0.2f; // 100.6 rounds to 101
  CHECK(!curveIsIdentity(lut));
}

TEST_CASE("curves: monotoneAndNoOvershoot", "[curves]")
{
  // The reason for the Fritsch-Carlson cubic: sorted-y points can never make
  // the curve run backwards or leave the range of their neighbors.
  std::mt19937 rng(1234);
  for (int trial = 0; trial < 3000; ++trial)
  {
    const size_t n = 2 + rng() % 8;
    std::vector<float> xs{0.0f, 255.0f};
    while (xs.size() < n)
    {
      const float x = static_cast<float>(1 + rng() % 254);
      bool ok = true;
      for (float e : xs)
        ok = ok && std::fabs(e - x) >= kCurveMinGap;
      if (ok)
        xs.push_back(x);
      else
        break;
    }
    std::sort(xs.begin(), xs.end());
    std::vector<float> ys;
    for (size_t i = 0; i < xs.size(); ++i)
      ys.push_back(static_cast<float>(rng() % 256));
    CurvePoints mono, any;
    std::vector<float> sorted = ys;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < xs.size(); ++i)
    {
      mono.push_back({xs[i], sorted[i]});
      any.push_back({xs[i], ys[i]});
    }
    const auto lm = curveToLut(mono);
    for (int i = 1; i < 256; ++i)
      CHECK(lm[static_cast<size_t>(i)] >= lm[static_cast<size_t>(i - 1)] - 1e-3f);
    // Never outside the range of the points, monotone or not.
    const float lo = *std::min_element(ys.begin(), ys.end()), hi = *std::max_element(ys.begin(), ys.end());
    const auto la = curveToLut(any);
    for (float v : la)
      CHECK((v >= lo - 1e-3f && v <= hi + 1e-3f));
    // Between two neighbors it stays between their heights.
    for (size_t k = 0; k + 1 < any.size(); ++k)
    {
      const float a = std::min(any[k].y, any[k + 1].y), b = std::max(any[k].y, any[k + 1].y);
      for (int x = static_cast<int>(std::ceil(any[k].x)); x <= static_cast<int>(any[k + 1].x); ++x)
        CHECK((la[static_cast<size_t>(x)] >= a - 1e-3f && la[static_cast<size_t>(x)] <= b + 1e-3f));
    }
  }
}

TEST_CASE("curves: insert", "[curves]")
{
  CurvePoints p = identityCurve();
  CHECK((curveInsert(p, 100, 120) == 1 && p.size() == 3 && p[1].x == 100 && p[1].y == 120));
  CHECK((curveInsert(p, 50, 60) == 1 && p[1].x == 50 && p[2].x == 100)); // kept sorted
  CHECK((curveInsert(p, 200, 10) == 3 && p[3].x == 200));
  // Refused: on / next to an existing point or an end, or outside the ends.
  const size_t n = p.size();
  CHECK(curveInsert(p, 100, 5) == -1);
  CHECK(curveInsert(p, 100 + kCurveMinGap - 0.5f, 5) == -1);
  CHECK(curveInsert(p, 100 - kCurveMinGap + 0.5f, 5) == -1);
  CHECK((curveInsert(p, 0, 5) == -1 && curveInsert(p, 1, 5) == -1));
  CHECK((curveInsert(p, 255, 5) == -1 && curveInsert(p, 254, 5) == -1));
  CHECK((curveInsert(p, -10, 5) == -1 && curveInsert(p, 300, 5) == -1));
  CHECK(p.size() == n);
  // Exactly the minimum gap is allowed; y is clamped into range.
  CHECK(curveInsert(p, 100 + kCurveMinGap, 999) >= 0);
  int k = -1;
  for (size_t i = 0; i < p.size(); ++i)
    if (p[i].x == 100 + kCurveMinGap)
      k = static_cast<int>(i);
  CHECK((k > 0 && p[static_cast<size_t>(k)].y == 255.0f));
  CHECK(kCurveMaxPoints >= 2);
}

TEST_CASE("curves: moveAndRemove", "[curves]")
{
  CurvePoints p{{0, 0}, {80, 80}, {160, 160}, {255, 255}};
  curveMovePoint(p, 1, 500, 300); // x pinned between the neighbors, y clamped
  CHECK((p[1].x == 160 - kCurveMinGap && p[1].y == 255.0f));
  curveMovePoint(p, 1, -500, -9);
  CHECK((p[1].x == 0 + kCurveMinGap && p[1].y == 0.0f));
  curveMovePoint(p, 0, 100, 50); // end points: y only
  CHECK((p[0].x == 0 && p[0].y == 50));
  curveMovePoint(p, 3, 10, 70);
  CHECK((p[3].x == 255 && p[3].y == 70));
  const CurvePoints before = p;
  curveMovePoint(p, -1, 5, 5);
  curveMovePoint(p, 99, 5, 5);
  CHECK(p == before); // out-of-range index: nothing

  CHECK((!curveRemove(p, 0) && !curveRemove(p, 3) && !curveRemove(p, -1) && !curveRemove(p, 4)));
  CHECK(p.size() == 4);
  CHECK((curveRemove(p, 1) && p.size() == 3));
  CHECK((curveRemove(p, 1) && p.size() == 2));
  CHECK(!curveRemove(p, 1)); // only the two ends are left
}

TEST_CASE("curves: randomEditsKeepTheListValid", "[curves]")
{
  std::mt19937 rng(99);
  CurvePoints p = identityCurve();
  for (int step = 0; step < 20000; ++step)
  {
    const float x = static_cast<float>(static_cast<int>(rng() % 3000) - 1000) / 10.0f;
    const float y = static_cast<float>(static_cast<int>(rng() % 3000) - 1000) / 10.0f;
    switch (rng() % 3)
    {
    case 0:
      if (p.size() < kCurveMaxPoints)
        curveInsert(p, x, y);
      break;
    case 1:
      curveMovePoint(p, static_cast<int>(rng() % (p.size() + 2)) - 1, x, y);
      break;
    default:
      curveRemove(p, static_cast<int>(rng() % (p.size() + 2)) - 1);
      break;
    }
    CHECK((p.size() >= 2 && p.size() <= kCurveMaxPoints));
    CHECK((p.front().x == 0.0f && p.back().x == 255.0f));
    for (size_t i = 0; i < p.size(); ++i)
    {
      CHECK((p[i].x >= 0.0f && p[i].x <= 255.0f && p[i].y >= 0.0f && p[i].y <= 255.0f));
      if (i)
        CHECK(p[i].x - p[i - 1].x >= kCurveMinGap - 1e-4f);
    }
  }
  // Whatever the edits did, the result still makes a sane table.
  const auto lut = curveToLut(p);
  for (float v : lut)
    CHECK((v >= 0.0f && v <= 255.0f));
}
} // namespace curves_cases
