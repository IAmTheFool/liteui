// Tests for SVG path data and the shape-to-path converters.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

using Op = svg::PathOp::Kind;

// ------------------------------------------------------------- parsePathData

TEST_CASE("svg path: absolute M, L, Z", "[svg][path]") {
  auto ops = svg::parsePathData("M10 20 L30 40 Z");
  REQUIRE(ops.size() == 3);
  CHECK(ops[0].kind == Op::Move);
  CHECK(ops[0].x == approx(10));
  CHECK(ops[0].y == approx(20));
  CHECK(ops[1].kind == Op::Line);
  CHECK(ops[1].x == approx(30));
  CHECK(ops[1].y == approx(40));
  CHECK(ops[2].kind == Op::Close);
}

TEST_CASE("svg path: relative commands add to the current point",
          "[svg][path]") {
  auto ops = svg::parsePathData("m10 20 l5 5 l-2 3");
  REQUIRE(ops.size() == 3);
  CHECK(ops[1].x == approx(15));
  CHECK(ops[1].y == approx(25));
  CHECK(ops[2].x == approx(13));
  CHECK(ops[2].y == approx(28));
}

TEST_CASE("svg path: extra coordinate pairs after M become implicit lineto",
          "[svg][path]") {
  SECTION("absolute M -> L") {
    auto ops = svg::parsePathData("M0 0 10 10 20 0");
    REQUIRE(ops.size() == 3);
    CHECK(ops[0].kind == Op::Move);
    CHECK(ops[1].kind == Op::Line);
    CHECK(ops[2].kind == Op::Line);
  }
  SECTION("relative m -> l") {
    auto ops = svg::parsePathData("m1 1 2 2");
    REQUIRE(ops.size() == 2);
    CHECK(ops[1].kind == Op::Line);
    CHECK(ops[1].x == approx(3)); // relative to (1,1)
    CHECK(ops[1].y == approx(3));
  }
}

TEST_CASE("svg path: a repeated command letter may be omitted",
          "[svg][path]") {
  auto ops = svg::parsePathData("M0 0 L10 0 10 10 0 10");
  REQUIRE(ops.size() == 4);
  CHECK(ops[3].x == approx(0));
  CHECK(ops[3].y == approx(10));
}

TEST_CASE("svg path: H and V keep the other coordinate", "[svg][path]") {
  auto ops = svg::parsePathData("M5 5 H20 V30 h-10 v-5");
  REQUIRE(ops.size() == 5);
  CHECK(ops[1].x == approx(20));
  CHECK(ops[1].y == approx(5));
  CHECK(ops[2].x == approx(20));
  CHECK(ops[2].y == approx(30));
  CHECK(ops[3].x == approx(10));
  CHECK(ops[3].y == approx(30));
  CHECK(ops[4].x == approx(10));
  CHECK(ops[4].y == approx(25));
}

TEST_CASE("svg path: cubic C stores both controls and the endpoint",
          "[svg][path]") {
  auto ops = svg::parsePathData("M0 0 C1 2 3 4 5 6");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].kind == Op::Cubic);
  CHECK(ops[1].c1x == approx(1));
  CHECK(ops[1].c1y == approx(2));
  CHECK(ops[1].c2x == approx(3));
  CHECK(ops[1].c2y == approx(4));
  CHECK(ops[1].x == approx(5));
  CHECK(ops[1].y == approx(6));
}

TEST_CASE("svg path: relative cubic offsets every point", "[svg][path]") {
  auto ops = svg::parsePathData("M10 10 c1 2 3 4 5 6");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].c1x == approx(11));
  CHECK(ops[1].c2y == approx(14));
  CHECK(ops[1].x == approx(15));
  CHECK(ops[1].y == approx(16));
}

TEST_CASE("svg path: smooth cubic S reflects the previous control point",
          "[svg][path]") {
  // Previous C ends at (10,0) with second control (10,10).
  // Reflection through the endpoint gives (2*10-10, 2*0-10) = (10,-10).
  auto ops = svg::parsePathData("M0 0 C0 10 10 10 10 0 S 20 -10 20 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[2].kind == Op::Cubic);
  CHECK(ops[2].c1x == approx(10));
  CHECK(ops[2].c1y == approx(-10));
  CHECK(ops[2].c2x == approx(20));
  CHECK(ops[2].c2y == approx(-10));
}

TEST_CASE("svg path: S with no preceding curve uses the current point",
          "[svg][path]") {
  auto ops = svg::parsePathData("M5 5 S 8 8 10 10");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].c1x == approx(5));
  CHECK(ops[1].c1y == approx(5));
}

TEST_CASE("svg path: quadratic Q and smooth T", "[svg][path]") {
  auto ops = svg::parsePathData("M0 0 Q5 10 10 0 T 20 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[1].kind == Op::Quad);
  CHECK(ops[1].qx == approx(5));
  CHECK(ops[1].qy == approx(10));
  CHECK(ops[1].x == approx(10));
  // T reflects (5,10) through (10,0): (15,-10).
  CHECK(ops[2].kind == Op::Quad);
  CHECK(ops[2].qx == approx(15));
  CHECK(ops[2].qy == approx(-10));
  CHECK(ops[2].x == approx(20));
}

TEST_CASE("svg path: Z returns the pen to the subpath start", "[svg][path]") {
  // After Z, the current point is back at (5,5), so "l1 1" lands on (6,6).
  auto ops = svg::parsePathData("M5 5 L10 10 Z l1 1");
  REQUIRE(ops.size() == 4);
  CHECK(ops[2].kind == Op::Close);
  CHECK(ops[3].kind == Op::Line);
  CHECK(ops[3].x == approx(6));
  CHECK(ops[3].y == approx(6));
}

TEST_CASE("svg path: compact notation without separators", "[svg][path]") {
  auto ops = svg::parsePathData("M10-20L-5.5.5");
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].x == approx(10));
  CHECK(ops[0].y == approx(-20));
  CHECK(ops[1].x == approx(-5.5));
  CHECK(ops[1].y == approx(0.5));
}

TEST_CASE("svg path: commas and newlines are separators", "[svg][path]") {
  auto ops = svg::parsePathData("M 1,2\n L\t3 , 4");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].x == approx(3));
  CHECK(ops[1].y == approx(4));
}

TEST_CASE("svg path: elliptical arcs become cubic segments", "[svg][path]") {
  SECTION("a half circle ends exactly at the target") {
    auto ops = svg::parsePathData("M0 0 A10 10 0 0 1 20 0");
    REQUIRE(ops.size() >= 3);
    CHECK(ops[0].kind == Op::Move);
    for (size_t k = 1; k < ops.size(); ++k)
      CHECK(ops[k].kind == Op::Cubic);
    CHECK(ops.back().x == approx(20));
    CHECK(ops.back().y == approx(0).margin(0.01));
  }
  SECTION("sweep flag picks which side the arc bulges to") {
    // sweep=1 means "clockwise on screen". SVG's y axis points DOWN, so going
    // clockwise from the left point to the right point passes over the top
    // (negative y); sweep=0 goes the other way, under (positive y).
    auto cw = svg::parsePathData("M0 0 A10 10 0 0 1 20 0");
    auto ccw = svg::parsePathData("M0 0 A10 10 0 0 0 20 0");
    REQUIRE(cw.size() >= 2);
    REQUIRE(ccw.size() >= 2);
    CHECK(cw[1].c1y < 0);
    CHECK(ccw[1].c1y > 0);
  }
  SECTION("a zero radius degrades to a straight line") {
    auto ops = svg::parsePathData("M0 0 A0 5 0 0 1 10 10");
    REQUIRE(ops.size() == 2);
    CHECK(ops[1].kind == Op::Line);
    CHECK(ops[1].x == approx(10));
  }
  SECTION("radii too small for the chord are scaled up, still reaching the end") {
    auto ops = svg::parsePathData("M0 0 A1 1 0 0 1 100 0");
    REQUIRE(ops.size() >= 2);
    CHECK(ops.back().x == approx(100).margin(0.01));
    CHECK(ops.back().y == approx(0).margin(0.01));
  }
}

TEST_CASE("svg path: bad or truncated data stops cleanly", "[svg][path]") {
  SECTION("empty") { CHECK(svg::parsePathData("").empty()); }
  SECTION("whitespace only") { CHECK(svg::parsePathData("  \n ").empty()); }
  SECTION("command with too few numbers emits nothing for it") {
    CHECK(svg::parsePathData("M10").empty());
    auto ops = svg::parsePathData("M0 0 L5");
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].kind == Op::Move);
  }
  SECTION("numbers before any command are ignored") {
    CHECK(svg::parsePathData("10 20 30").empty());
  }
  SECTION("an unknown command letter stops parsing but keeps earlier ops") {
    auto ops = svg::parsePathData("M0 0 L1 1 X 5 5 L9 9");
    REQUIRE(ops.size() == 2);
  }
  SECTION("random junk never crashes") {
    for (const char *s : {"MMMM", "ZZZ", "A", "AAAA 1 2", "C1", "..", "-", "M-"})
      REQUIRE_NOTHROW(svg::parsePathData(s));
  }
}

// ------------------------------------------------- shape -> path converters

TEST_CASE("svg shapes: rectToOps", "[svg][shapes]") {
  SECTION("zero or negative size produces nothing") {
    CHECK(svg::rectToOps(0, 0, 0, 10, 0, 0).empty());
    CHECK(svg::rectToOps(0, 0, 10, 0, 0, 0).empty());
    CHECK(svg::rectToOps(0, 0, -5, 10, 0, 0).empty());
  }
  SECTION("a plain rectangle is a closed 4-point polygon") {
    auto ops = svg::rectToOps(10, 20, 30, 40, 0, 0);
    REQUIRE(ops.size() == 5);
    CHECK(ops[0].kind == Op::Move);
    CHECK(ops[0].x == approx(10));
    CHECK(ops[0].y == approx(20));
    CHECK(ops[2].x == approx(40)); // bottom-right corner
    CHECK(ops[2].y == approx(60));
    CHECK(ops[4].kind == Op::Close);
  }
  SECTION("rounded corners use four cubic curves") {
    auto ops = svg::rectToOps(0, 0, 100, 50, 10, 10);
    REQUIRE(ops.size() == 10);
    int cubics = 0;
    for (auto &o : ops)
      cubics += (o.kind == Op::Cubic);
    CHECK(cubics == 4);
    CHECK(ops.front().x == approx(10)); // starts after the top-left corner
    CHECK(ops.back().kind == Op::Close);
  }
  SECTION("oversized radii are clamped to half the side") {
    auto ops = svg::rectToOps(0, 0, 10, 10, 1000, 1000);
    REQUIRE(ops.size() == 10);
    CHECK(ops[0].x == approx(5)); // rx clamped to w/2
    CHECK(ops[1].x == approx(5)); // so the straight edge has zero length
  }
  SECTION("a radius of zero on one axis means sharp corners") {
    CHECK(svg::rectToOps(0, 0, 10, 10, 5, 0).size() == 5);
  }
}

TEST_CASE("svg shapes: ellipseToOps", "[svg][shapes]") {
  CHECK(svg::ellipseToOps(0, 0, 0, 5).empty());
  CHECK(svg::ellipseToOps(0, 0, 5, 0).empty());
  CHECK(svg::ellipseToOps(0, 0, -1, -1).empty());

  auto ops = svg::ellipseToOps(10, 20, 4, 2);
  REQUIRE(ops.size() == 6); // move + 4 cubics + close
  CHECK(ops[0].kind == Op::Move);
  CHECK(ops[0].x == approx(14)); // starts at the rightmost point
  CHECK(ops[0].y == approx(20));
  // The four cubics end on the bottom, left, top, right extremes.
  CHECK(ops[1].x == approx(10));
  CHECK(ops[1].y == approx(22));
  CHECK(ops[2].x == approx(6));
  CHECK(ops[2].y == approx(20));
  CHECK(ops[3].x == approx(10));
  CHECK(ops[3].y == approx(18));
  CHECK(ops[4].x == approx(14));
  CHECK(ops[4].y == approx(20));
  CHECK(ops[5].kind == Op::Close);
}

TEST_CASE("svg shapes: polyToOps", "[svg][shapes]") {
  SECTION("polyline stays open") {
    auto ops = svg::polyToOps("0,0 10,0 10,10", false);
    REQUIRE(ops.size() == 3);
    CHECK(ops[0].kind == Op::Move);
    CHECK(ops[1].kind == Op::Line);
    CHECK(ops[2].kind == Op::Line);
  }
  SECTION("polygon closes") {
    auto ops = svg::polyToOps("0,0 10,0 10,10", true);
    REQUIRE(ops.size() == 4);
    CHECK(ops.back().kind == Op::Close);
  }
  SECTION("a dangling coordinate is dropped") {
    auto ops = svg::polyToOps("1 2 3", false);
    REQUIRE(ops.size() == 1);
  }
  SECTION("no points, no ops (not even a Close)") {
    CHECK(svg::polyToOps("", true).empty());
    CHECK(svg::polyToOps("garbage", true).empty());
  }
}
