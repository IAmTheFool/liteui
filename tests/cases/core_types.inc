// Tests for the small public value types: Color, Box, Size, Dynamic<T>.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

TEST_CASE("Color: defaults and equality", "[core][color]") {
  Color c;
  CHECK(c.r == 0);
  CHECK(c.g == 0);
  CHECK(c.b == 0);
  CHECK(c.a == 255); // opaque unless you say otherwise

  CHECK(Color{1, 2, 3, 4} == Color{1, 2, 3, 4});
  CHECK_FALSE(Color{1, 2, 3, 4} == Color{1, 2, 3, 5});
  CHECK(Color{1, 2, 3} == Color{1, 2, 3, 255});
}

TEST_CASE("Box: defaults are all zero", "[core]") {
  Box b;
  CHECK(b.width == 0);
  CHECK(b.height == 0);
  CHECK(b.pos_x == 0);
  CHECK(b.pos_y == 0);
  CHECK(b.color == Color{});
}

TEST_CASE("Size: factory functions", "[core][size]") {
  auto px = Size::pixel(12.5f);
  CHECK(px.kind == Size::Kind::Fixed);
  CHECK(px.value == 12.5f);

  auto pct = Size::percentage(40);
  CHECK(pct.kind == Size::Kind::Percentage);
  CHECK(pct.value == 40);

  CHECK(Size::fit().kind == Size::Kind::Fit);
  CHECK(Size::full().kind == Size::Kind::Full);

  // A default-constructed Size sizes to its content.
  CHECK(Size{}.kind == Size::Kind::Fit);
}

TEST_CASE("EdgeInsets: all() and equality", "[core]") {
  auto e = EdgeInsets::all(4);
  CHECK(e.top == 4);
  CHECK(e.right == 4);
  CHECK(e.bottom == 4);
  CHECK(e.left == 4);
  CHECK(e == EdgeInsets{4, 4, 4, 4});
  CHECK_FALSE(e == EdgeInsets{});
}

TEST_CASE("Dynamic<T>: static value vs polled callback", "[core][dynamic]") {
  SECTION("a plain value is returned directly; the cache is ignored") {
    Dynamic<int> field = 7;
    int cache = 99;
    CHECK(resolveDynamic(field, cache) == 7);
    CHECK(resolveStatic(field) == 7);
  }

  SECTION("a callback resolves to the cache, never calls the function") {
    int calls = 0;
    Dynamic<int> field = std::function<int()>([&] { ++calls; return 5; });
    int cache = 42;
    CHECK(resolveDynamic(field, cache) == 42);
    CHECK(calls == 0); // resolving never invokes the callback itself
  }

  SECTION("resolveStatic throws when handed a callback") {
    Dynamic<int> field = std::function<int()>([] { return 1; });
    CHECK_THROWS(resolveStatic(field));
  }
}
