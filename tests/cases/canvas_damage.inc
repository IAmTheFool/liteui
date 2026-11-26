// Tests for canvas partial redraw: the rounding/planning helpers
// (liteui_canvas_damage) and the thread-safe CanvasDamage accumulator. No GPU
// or window. The GL clear/scissor/resolve itself is covered by
// canvas_gl_partial_check.cpp (hardware).
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <atomic>
#include <thread>

TEST_CASE("canvas damage: toDevice rounds outward and clamps",
          "[canvas][damage]") {
  using namespace liteui_canvas_damage;
  SECTION("fractional edges grow to whole pixels") {
    auto d = toDevice({10.2f, 5.7f, 3.1f, 2.0f}, 1.0f, 100, 100);
    REQUIRE((d.x0 == 10 && d.y0 == 5 && d.x1 == 14 && d.y1 == 8));
  }
  SECTION("an exact pixel boundary does not grow") {
    auto d = toDevice({10, 5, 4, 3}, 1.0f, 100, 100);
    REQUIRE((d.x0 == 10 && d.y0 == 5 && d.x1 == 14 && d.y1 == 8));
  }
  SECTION("fractional device scale") {
    auto d = toDevice({8, 8, 8, 8}, 1.25f, 100, 100);
    REQUIRE((d.x0 == 10 && d.y0 == 10 && d.x1 == 20 && d.y1 == 20));
    auto e = toDevice({1, 1, 1, 1}, 1.5f, 100, 100); // 1.5..3.0 -> 1..3
    REQUIRE((e.x0 == 1 && e.x1 == 3 && e.y0 == 1 && e.y1 == 3));
  }
  SECTION("clamping and off-canvas") {
    auto d = toDevice({-10, -10, 30, 30}, 1.0f, 100, 100);
    REQUIRE((d.x0 == 0 && d.y0 == 0 && d.x1 == 20 && d.y1 == 20));
    REQUIRE(toDevice({150, 150, 10, 10}, 1.0f, 100, 100).empty());
    REQUIRE(toDevice({0, 0, 0, 5}, 1.0f, 100, 100).empty());
    auto e = toDevice({90, 90, 50, 50}, 1.0f, 100, 100);
    REQUIRE((e.x0 == 90 && e.x1 == 100 && e.y1 == 100));
  }
}

TEST_CASE("canvas damage: plan chooses full vs partial", "[canvas][damage]") {
  using namespace liteui_canvas_damage;
  REQUIRE(plan(true, {1, 1, 2, 2}, 1, 100, 100).full); // full asked
  REQUIRE(plan(false, {}, 1, 100, 100).full); // no rect: safe
  REQUIRE(plan(false, {0, 0, 100, 100}, 1, 100, 100).full);
  REQUIRE(plan(false, DirtyRect::everything(), 1, 100, 100).full);
  REQUIRE(plan(false, {-5, -5, 200, 200}, 1, 100, 100).full);
  auto off = plan(false, {150, 150, 5, 5}, 1, 100, 100);
  REQUIRE((off.nothing && !off.full));
  auto p = plan(false, {10.5f, 10.5f, 4, 4}, 2.0f, 200, 200);
  REQUIRE(!p.full);
  REQUIRE((p.dev.x0 == 21 && p.dev.x1 == 29 && p.dev.y0 == 21 && p.dev.y1 == 29));
  REQUIRE((p.logical.x == 10.5f && p.logical.w == 4.0f));
}

TEST_CASE("canvas damage: DirtyRect::united", "[canvas][damage]") {
  DirtyRect a{0, 0, 10, 10}, b{20, 5, 5, 30};
  auto u = a.united(b);
  REQUIRE((u.x == 0 && u.y == 0 && u.w == 25 && u.h == 35));
  REQUIRE(DirtyRect{}.united(b).x == 20);
  REQUIRE(a.united(DirtyRect{}).w == 10);
}

TEST_CASE("canvas damage: CanvasDamage accumulates and drains",
          "[canvas][damage]") {
  CanvasDamage c;
  REQUIRE(!c.take().any);
  c.invalidate(0, 0, 0, 5); // no area: ignored
  REQUIRE(!c.take().any);
  c.invalidate(10, 10, -5, -5); // negative size normalised
  auto p = c.take();
  REQUIRE((p.any && p.rect.x == 5 && p.rect.w == 5));
  c.invalidate(0, 0, 2, 2);
  c.invalidate(8, 8, 2, 2);
  p = c.take();
  REQUIRE((p.rect.w == 10 && p.rect.h == 10)); // bounding box
  REQUIRE(!c.take().any);                       // take() clears
  c.invalidate(NAN, 0, 1, 1);                   // non-finite => whole canvas
  p = c.take();
  REQUIRE((p.any && p.rect.x == 0 && p.rect.y == 0));
  REQUIRE((p.rect.w >= 1e9f && p.rect.h >= 1e9f));
  c.invalidate(DirtyRect::everything());
  c.invalidate(1, 1, 1, 1); // absorbed by the whole-canvas box
  p = c.take();
  REQUIRE((p.rect.x == 0 && p.rect.w >= 1e9f));
}

TEST_CASE("canvas damage: notify and threads", "[canvas][damage][threads]") {
  CanvasDamage c;
  std::atomic<int> notes{0};
  c.setNotify([&] { ++notes; });
  c.invalidate(1, 1, 1, 1);
  c.invalidate(DirtyRect::everything());
  c.invalidate(0, 0, 0, 0); // ignored: no notify
  REQUIRE(notes == 2);
  c.take();
  notes = 0;
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < 1000; ++i)
        c.invalidate(float(t * 10), float(i % 50), 5, 5);
    });
  for (auto &t : ts)
    t.join();
  auto p = c.take();
  REQUIRE(p.any);
  REQUIRE((p.rect.x == 0 && p.rect.w == 75 && p.rect.h == 54));
  REQUIRE(notes == 8000);
}
