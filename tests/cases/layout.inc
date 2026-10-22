// Tests for the flexbox layout engine (liteui_layout). Everything here uses
// fixed-size boxes and no text, so the results are exact and don't depend on
// fonts or the platform's text renderer.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

namespace {
// Run layout on `root` in an 800x600 window and return it for inspection.
void doLayout(View &root, float ox = 0, float oy = 0) {
  layout::layoutRoot(root, 800, 600, ox, oy);
}
} // namespace

// -------------------------------------------------------- helper functions

TEST_CASE("layout: resolveAxis", "[layout][helpers]") {
  CHECK(layout::resolveAxis(Size::pixel(30), 500, true, 7) == approx(30));
  CHECK(layout::resolveAxis(Size::percentage(25), 200, true, 7) == approx(50));
  CHECK(layout::resolveAxis(Size::full(), 200, true, 7) == approx(200));
  CHECK(layout::resolveAxis(Size::fit(), 200, true, 7) == approx(7));
  // With an indefinite available size, percentage/full fall back to "fit".
  CHECK(layout::resolveAxis(Size::percentage(25), 200, false, 7) == approx(7));
  CHECK(layout::resolveAxis(Size::full(), 200, false, 7) == approx(7));
  // Fixed ignores definiteness.
  CHECK(layout::resolveAxis(Size::pixel(30), 0, false, 7) == approx(30));
}

TEST_CASE("layout: clampSize", "[layout][helpers]") {
  CHECK(layout::clampSize(50, 10, 100) == approx(50));
  CHECK(layout::clampSize(5, 10, 100) == approx(10));
  CHECK(layout::clampSize(500, 10, 100) == approx(100));
  // max < min is treated as max == min instead of an invalid range.
  CHECK(layout::clampSize(50, 80, 20) == approx(80));
  CHECK(layout::clampSize(500, 80, 20) == approx(80));
}

TEST_CASE("layout: axisScrollbarVisible", "[layout][helpers]") {
  using layout::axisScrollbarVisible;
  CHECK(axisScrollbarVisible(Overflow::Scroll, 10, 100));  // always
  CHECK_FALSE(axisScrollbarVisible(Overflow::Visible, 1000, 100));
  CHECK_FALSE(axisScrollbarVisible(Overflow::Hidden, 1000, 100));
  CHECK(axisScrollbarVisible(Overflow::Auto, 200, 100));   // overflows
  CHECK_FALSE(axisScrollbarVisible(Overflow::Auto, 100, 100)); // exact fit
  // Half a pixel of slack stops a perfectly-fitting box from flickering a bar.
  CHECK_FALSE(axisScrollbarVisible(Overflow::Auto, 100.4f, 100));
  CHECK(axisScrollbarVisible(Overflow::Auto, 100.6f, 100));
}

// ----------------------------------------------------------- basic flow

TEST_CASE("layout: the root is placed at the origin with its own size",
          "[layout]") {
  View root = container(300, 200);
  root.addChild(box(10, 10));
  doLayout(root);
  CHECK(root.computed.x == approx(0));
  CHECK(root.computed.y == approx(0));
  CHECK(root.computed.w == approx(300));
  CHECK(root.computed.h == approx(200));
}

TEST_CASE("layout: originX/originY offset the whole tree", "[layout]") {
  View root = container(300, 200);
  root.addChild(box(10, 10));
  doLayout(root, 5, 40);
  CHECK(root.computed.x == approx(5));
  CHECK(root.computed.y == approx(40));
  CHECK(root.children[0].computed.x == approx(5));
  CHECK(root.children[0].computed.y == approx(40));
}

TEST_CASE("layout: Row places children left to right with gap", "[layout][flex]") {
  View root = container(400, 100);
  root.style.alignItems = Align::Start;
  root.style.gap = 10;
  root.addChild(box(50, 20));
  root.addChild(box(80, 30));
  root.addChild(box(20, 10));
  doLayout(root);

  auto &c = root.children;
  CHECK(c[0].computed.x == approx(0));
  CHECK(c[1].computed.x == approx(60));   // 50 + gap 10
  CHECK(c[2].computed.x == approx(150));  // 60 + 80 + gap 10
  for (auto &ch : c)
    CHECK(ch.computed.y == approx(0));
  CHECK(c[1].computed.w == approx(80));
  CHECK(c[1].computed.h == approx(30));
}

TEST_CASE("layout: Column places children top to bottom with gap",
          "[layout][flex]") {
  View root = container(100, 400);
  root.style.direction = FlexDirection::Column;
  root.style.alignItems = Align::Start;
  root.style.gap = 5;
  root.addChild(box(20, 50));
  root.addChild(box(30, 60));
  doLayout(root);

  CHECK(root.children[0].computed.y == approx(0));
  CHECK(root.children[1].computed.y == approx(55));
  CHECK(root.children[1].computed.x == approx(0));
}

TEST_CASE("layout: a Fit container wraps its children", "[layout][flex]") {
  View root; // width/height default to Fit
  root.style.gap = 4;
  root.addChild(box(30, 10));
  root.addChild(box(20, 25));
  doLayout(root);
  CHECK(root.computed.w == approx(54));  // 30 + 4 + 20
  CHECK(root.computed.h == approx(25));  // tallest child
}

// ---------------------------------------------------- padding and margin

TEST_CASE("layout: padding insets the content box", "[layout][box]") {
  View root = container(200, 100);
  root.style.padding = EdgeInsets{10, 20, 30, 40}; // top right bottom left
  root.style.alignItems = Align::Start;
  root.addChild(box(50, 20));
  doLayout(root);
  CHECK(root.children[0].computed.x == approx(40));
  CHECK(root.children[0].computed.y == approx(10));
}

TEST_CASE("layout: padding is included when a Fit container sizes itself",
          "[layout][box]") {
  View root;
  root.style.padding = EdgeInsets::all(10);
  root.addChild(box(50, 20));
  doLayout(root);
  CHECK(root.computed.w == approx(70));
  CHECK(root.computed.h == approx(40));
}

TEST_CASE("layout: margins offset a child and space its siblings", "[layout][box]") {
  View root = container(400, 100);
  root.style.alignItems = Align::Start;
  View a = box(50, 20);
  a.style.margin = EdgeInsets{5, 10, 5, 15}; // top right bottom left
  root.addChild(a);
  root.addChild(box(30, 20));
  doLayout(root);

  CHECK(root.children[0].computed.x == approx(15)); // left margin
  CHECK(root.children[0].computed.y == approx(5));  // top margin
  // Second child starts after: left margin 15 + width 50 + right margin 10.
  CHECK(root.children[1].computed.x == approx(75));
}

// ------------------------------------------------------ justify / align

TEST_CASE("layout: justifyContent along the main axis", "[layout][flex]") {
  auto positions = [](Justify j) {
    View root = container(300, 50);
    root.style.alignItems = Align::Start;
    root.style.justifyContent = j;
    root.addChild(box(40, 10));
    root.addChild(box(40, 10));
    root.addChild(box(40, 10));
    doLayout(root);
    return std::vector<float>{root.children[0].computed.x,
                              root.children[1].computed.x,
                              root.children[2].computed.x};
  };
  // 300 wide, 120 of children => 180 free.
  SECTION("Start") {
    auto p = positions(Justify::Start);
    CHECK(p[0] == approx(0));
    CHECK(p[1] == approx(40));
    CHECK(p[2] == approx(80));
  }
  SECTION("End") {
    auto p = positions(Justify::End);
    CHECK(p[0] == approx(180));
    CHECK(p[2] == approx(260));
  }
  SECTION("Center") {
    auto p = positions(Justify::Center);
    CHECK(p[0] == approx(90));
    CHECK(p[2] == approx(170));
  }
  SECTION("SpaceBetween: first at start, last at end, equal gaps") {
    auto p = positions(Justify::SpaceBetween);
    CHECK(p[0] == approx(0));
    CHECK(p[1] == approx(130));
    CHECK(p[2] == approx(260));
  }
  SECTION("SpaceAround: half-gaps at the ends") {
    auto p = positions(Justify::SpaceAround);
    // free 180 / 3 items = 60 per item, 30 each side.
    CHECK(p[0] == approx(30));
    CHECK(p[1] == approx(130));
    CHECK(p[2] == approx(230));
  }
  SECTION("SpaceEvenly: equal gaps everywhere") {
    auto p = positions(Justify::SpaceEvenly);
    // free 180 / 4 gaps = 45.
    CHECK(p[0] == approx(45));
    CHECK(p[1] == approx(130));
    CHECK(p[2] == approx(215));
  }
}

TEST_CASE("layout: alignItems along the cross axis", "[layout][flex]") {
  auto yOf = [](Align a, float childH) {
    View root = container(200, 100);
    root.style.alignItems = a;
    root.addChild(box(40, childH));
    doLayout(root);
    return std::pair{root.children[0].computed.y, root.children[0].computed.h};
  };
  CHECK(yOf(Align::Start, 20).first == approx(0));
  CHECK(yOf(Align::End, 20).first == approx(80));
  CHECK(yOf(Align::Center, 20).first == approx(40));
}

TEST_CASE("layout: Align::Stretch fills the cross axis for auto-sized children",
          "[layout][flex]") {
  View root = container(200, 100); // alignItems defaults to Stretch
  View child;
  child.style.width = Size::pixel(40); // height stays Fit
  root.addChild(child);
  doLayout(root);
  CHECK(root.children[0].computed.h == approx(100));
  CHECK(root.children[0].computed.w == approx(40));
}

// --------------------------------------------------------- grow / shrink

TEST_CASE("layout: flexGrow splits free space in proportion", "[layout][grow]") {
  View root = container(300, 50);
  root.style.alignItems = Align::Start;
  View a = box(50, 10);
  a.style.flexGrow = 1;
  View b = box(50, 10);
  b.style.flexGrow = 2;
  root.addChild(a);
  root.addChild(b);
  doLayout(root);

  // 300 - 100 = 200 free, split 1:2.
  CHECK(root.children[0].computed.w == approx(50 + 200.0 / 3));
  CHECK(root.children[1].computed.w == approx(50 + 400.0 / 3));
  CHECK(root.children[1].computed.x == approx(50 + 200.0 / 3));
}

TEST_CASE("layout: items without flexGrow don't take free space", "[layout][grow]") {
  View root = container(300, 50);
  root.style.alignItems = Align::Start;
  View a = box(50, 10);
  a.style.flexGrow = 1;
  root.addChild(a);
  root.addChild(box(50, 10)); // grow 0
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(250));
  CHECK(root.children[1].computed.w == approx(50));
}

TEST_CASE("layout: flexShrink squeezes overflowing items", "[layout][shrink]") {
  View root = container(100, 50);
  root.style.alignItems = Align::Start;
  root.addChild(box(100, 10));
  root.addChild(box(100, 10));
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(50));
  CHECK(root.children[1].computed.w == approx(50));
}

TEST_CASE("layout: flexShrink = 0 refuses to shrink", "[layout][shrink]") {
  View root = container(100, 50);
  root.style.alignItems = Align::Start;
  View a = box(100, 10);
  a.style.flexShrink = 0;
  root.addChild(a);
  root.addChild(box(100, 10));
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(100));
  CHECK(root.children[1].computed.w < 100);
}

// ----------------------------------------------- percentage / full / clamps

TEST_CASE("layout: percentage and full sizes resolve against the parent",
          "[layout][size]") {
  View root = container(200, 100);
  root.style.alignItems = Align::Start;
  View p;
  p.style.width = Size::percentage(50);
  p.style.height = Size::percentage(25);
  root.addChild(p);
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(100));
  CHECK(root.children[0].computed.h == approx(25));
}

TEST_CASE("layout: Size::full fills the available main axis", "[layout][size]") {
  View root = container(200, 100);
  root.style.alignItems = Align::Start;
  View f;
  f.style.width = Size::full();
  f.style.height = Size::pixel(10);
  root.addChild(f);
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(200));
}

TEST_CASE("layout: minWidth / maxWidth clamp the resolved size", "[layout][size]") {
  View root = container(400, 100);
  root.style.alignItems = Align::Start;
  View narrow = box(10, 10);
  narrow.style.minWidth = 60;
  View wide = box(300, 10);
  wide.style.maxWidth = 80;
  root.addChild(narrow);
  root.addChild(wide);
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(60));
  CHECK(root.children[1].computed.w == approx(80));
}

TEST_CASE("layout: minHeight / maxHeight clamp a Fit container", "[layout][size]") {
  View tall;
  tall.style.minHeight = 50;
  tall.addChild(box(10, 5));
  doLayout(tall);
  CHECK(tall.computed.h == approx(50));

  View capped;
  capped.style.maxHeight = 20;
  capped.addChild(box(10, 200));
  doLayout(capped);
  CHECK(capped.computed.h == approx(20));
}

// ---------------------------------------------- display:none / absolute

TEST_CASE("layout: Display::None removes a child from flow", "[layout][display]") {
  View root = container(400, 100);
  root.style.alignItems = Align::Start;
  root.style.gap = 10;
  root.addChild(box(50, 10));
  View hidden = box(999, 10);
  hidden.style.display = Display::None;
  root.addChild(hidden);
  root.addChild(box(50, 10));
  doLayout(root);
  // The hidden child reserves no space and no gap.
  CHECK(root.children[2].computed.x == approx(60));
}

TEST_CASE("layout: Visibility::Hidden still reserves space", "[layout][display]") {
  View root = container(400, 100);
  root.style.alignItems = Align::Start;
  root.addChild(box(50, 10));
  View ghost = box(70, 10);
  ghost.style.visibility = Visibility::Hidden;
  root.addChild(ghost);
  root.addChild(box(50, 10));
  doLayout(root);
  CHECK(root.children[2].computed.x == approx(120));
}

TEST_CASE("layout: Position::Absolute is placed by left/top, out of flow",
          "[layout][absolute]") {
  View root = container(300, 200);
  root.style.alignItems = Align::Start;
  root.addChild(box(40, 10));
  View abs = box(30, 30);
  abs.style.position = Position::Absolute;
  abs.style.left = 100.0f;
  abs.style.top = 50.0f;
  root.addChild(abs);
  root.addChild(box(40, 10));
  doLayout(root);

  CHECK(root.children[1].computed.x == approx(100));
  CHECK(root.children[1].computed.y == approx(50));
  // The absolute child takes no part in flow: sibling 2 follows sibling 0.
  CHECK(root.children[2].computed.x == approx(40));
}

TEST_CASE("layout: absolute right/bottom anchor to the far edges",
          "[layout][absolute]") {
  View root = container(300, 200);
  View abs = box(30, 30);
  abs.style.position = Position::Absolute;
  abs.style.right = 10.0f;
  abs.style.bottom = 20.0f;
  root.addChild(abs);
  doLayout(root);
  CHECK(root.children[0].computed.x == approx(300 - 10 - 30));
  CHECK(root.children[0].computed.y == approx(200 - 20 - 30));
}

// --------------------------------------------------------------- wrapping

TEST_CASE("layout: FlexWrap::Wrap starts a new line when items don't fit",
          "[layout][wrap]") {
  View root = container(150, 200);
  root.style.flexWrap = FlexWrap::Wrap;
  root.style.alignItems = Align::Start;
  root.style.alignContent = AlignContent::Start;
  root.addChild(box(60, 20));
  root.addChild(box(60, 20));
  root.addChild(box(60, 20)); // 180 > 150 => this one wraps
  doLayout(root);

  CHECK(root.children[0].computed.y == approx(0));
  CHECK(root.children[1].computed.y == approx(0));
  CHECK(root.children[1].computed.x == approx(60));
  CHECK(root.children[2].computed.x == approx(0));
  CHECK(root.children[2].computed.y == approx(20));
}

TEST_CASE("layout: NoWrap keeps everything on one line (and shrinks)",
          "[layout][wrap]") {
  View root = container(150, 200);
  root.style.alignItems = Align::Start;
  for (int i = 0; i < 3; ++i)
    root.addChild(box(60, 20));
  doLayout(root);
  for (auto &c : root.children)
    CHECK(c.computed.y == approx(0));
  CHECK(root.children[2].computed.x + root.children[2].computed.w ==
        approx(150));
}

// --------------------------------------------------------------- scrolling

TEST_CASE("layout: a scroll container records its content size",
          "[layout][scroll]") {
  View root = container(100, 100);
  root.style.direction = FlexDirection::Column;
  root.style.overflowY = Overflow::Scroll;
  root.style.alignItems = Align::Start;
  for (int i = 0; i < 5; ++i)
    root.addChild(box(20, 50)); // 250 tall in total
  doLayout(root);

  CHECK(root.computed.contentH == approx(250));
  CHECK(root.maxScrollY() == approx(150));  // 250 - 100 viewport
  CHECK(root.maxScrollX() == approx(0));
  CHECK(root.scrollsY());
  CHECK_FALSE(root.scrollsX());
  // Children keep their natural size instead of being squeezed to fit.
  CHECK(root.children[4].computed.h == approx(50));
  CHECK(root.children[4].computed.y == approx(200));
}

TEST_CASE("layout: scroll offset moves children; max scroll is derived",
          "[layout][scroll]") {
  View root = container(100, 100);
  root.style.direction = FlexDirection::Column;
  root.style.overflowY = Overflow::Auto;
  root.style.alignItems = Align::Start;
  for (int i = 0; i < 4; ++i)
    root.addChild(box(20, 50));
  doLayout(root);
  REQUIRE(root.maxScrollY() == approx(100));

  root.computed.scrollY = 30;
  doLayout(root);
  CHECK(root.children[0].computed.y == approx(-30));
  CHECK(root.children[1].computed.y == approx(20));
}

TEST_CASE("layout: scrollY is clamped when content shrinks", "[layout][scroll]") {
  View root = container(100, 100);
  root.style.direction = FlexDirection::Column;
  root.style.overflowY = Overflow::Auto;
  for (int i = 0; i < 4; ++i)
    root.addChild(box(20, 50));
  doLayout(root);
  root.computed.scrollY = 100; // scrolled to the bottom

  root.children.resize(2); // now everything fits
  doLayout(root);
  CHECK(root.computed.scrollY == approx(0));
}

TEST_CASE("layout: Overflow::Auto shows no scroll range when content fits",
          "[layout][scroll]") {
  View root = container(100, 100);
  root.style.overflowY = Overflow::Auto;
  root.addChild(box(20, 20));
  doLayout(root);
  CHECK(root.maxScrollY() == approx(0));
}

TEST_CASE("layout: a childless view is never scrolled", "[layout][scroll]") {
  View root = container(100, 100);
  root.style.overflowY = Overflow::Scroll;
  root.computed.scrollY = 50;
  doLayout(root);
  CHECK(root.computed.scrollY == approx(0));
}

// ------------------------------------------------------------- callbacks

TEST_CASE("layout: onLayout receives the final box", "[layout][callbacks]") {
  View root = container(200, 100);
  root.style.alignItems = Align::Start;
  root.style.padding = EdgeInsets::all(10);
  View child = box(40, 30);
  float gx = -1, gy = -1, gw = -1, gh = -1;
  child.onLayout = [&](float x, float y, float w, float h) {
    gx = x; gy = y; gw = w; gh = h;
  };
  root.addChild(child);
  doLayout(root);
  CHECK(gx == approx(10));
  CHECK(gy == approx(10));
  CHECK(gw == approx(40));
  CHECK(gh == approx(30));
}

// ------------------------------------------------- dynamic (polled) styles

TEST_CASE("layout: polled width values are read from the cache",
          "[layout][dynamic]") {
  // A callback-valued field resolves through View::computed.resolvedWidth,
  // which the app's update loop (checkForUpdates) normally refreshes.
  View root = container(300, 50);
  root.style.alignItems = Align::Start;
  View child;
  child.style.width = std::function<Size()>([] { return Size::pixel(10); });
  child.style.height = Size::pixel(10);
  child.computed.resolvedWidth = Size::pixel(123); // what the poll "returned"
  root.addChild(child);
  doLayout(root);
  CHECK(root.children[0].computed.w == approx(123));
}
