// tests/test_svg.cpp
//
// Unit tests for liteui_svg's non-matrix pieces: path data ("d")
// parsing, primitive-to-path conversion, color/paint parsing, style
// inheritance, and full-document parsing via parseString().

#include "liteui.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using namespace liteui_svg;

// ---------------------------------------------------------------------
// Path data ("d" attribute)
// ---------------------------------------------------------------------

TEST_CASE("parsePathData: absolute moveto + lineto", "[svg][path]") {
  auto ops = parsePathData("M10 20 L30 40");
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].kind == PathOp::Kind::Move);
  CHECK(ops[0].x == Approx(10));
  CHECK(ops[0].y == Approx(20));
  CHECK(ops[1].kind == PathOp::Kind::Line);
  CHECK(ops[1].x == Approx(30));
  CHECK(ops[1].y == Approx(40));
}

TEST_CASE("parsePathData: relative moveto/lineto accumulate off the current "
          "point",
          "[svg][path]") {
  auto ops = parsePathData("m10 10 l5 5 l5 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[1].x == Approx(15));
  CHECK(ops[1].y == Approx(15));
  CHECK(ops[2].x == Approx(20));
  CHECK(ops[2].y == Approx(15));
}

TEST_CASE("parsePathData: a second coordinate pair after moveto is an "
          "implicit lineto",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 10 10 20 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[0].kind == PathOp::Kind::Move);
  CHECK(ops[1].kind == PathOp::Kind::Line);
  CHECK(ops[1].x == Approx(10));
  CHECK(ops[2].kind == PathOp::Kind::Line);
  CHECK(ops[2].y == Approx(0));
}

TEST_CASE("parsePathData: H and V move along a single axis", "[svg][path]") {
  auto ops = parsePathData("M5 5 H20 V30");
  REQUIRE(ops.size() == 3);
  CHECK(ops[1].x == Approx(20));
  CHECK(ops[1].y == Approx(5));
  CHECK(ops[2].x == Approx(20));
  CHECK(ops[2].y == Approx(30));
}

TEST_CASE("parsePathData: cubic bezier records both control points",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 C1 1 2 2 3 3");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].kind == PathOp::Kind::Cubic);
  CHECK(ops[1].c1x == Approx(1));
  CHECK(ops[1].c1y == Approx(1));
  CHECK(ops[1].c2x == Approx(2));
  CHECK(ops[1].c2y == Approx(2));
  CHECK(ops[1].x == Approx(3));
}

TEST_CASE("parsePathData: S reflects the previous cubic's control point",
          "[svg][path]") {
  // After C ending at (10,0) with second control point (10,-10), S's
  // implicit first control point is the reflection: (10,10).
  auto ops = parsePathData("M0 0 C0 0 10 -10 10 0 S20 20 20 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[2].kind == PathOp::Kind::Cubic);
  CHECK(ops[2].c1x == Approx(10));
  CHECK(ops[2].c1y == Approx(10));
}

TEST_CASE("parsePathData: S without a preceding cubic uses the current point",
          "[svg][path]") {
  auto ops = parsePathData("M5 5 S10 10 15 5");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].c1x == Approx(5));
  CHECK(ops[1].c1y == Approx(5));
}

TEST_CASE("parsePathData: quadratic bezier and its T reflection",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 Q5 10 10 0 T20 0");
  REQUIRE(ops.size() == 3);
  CHECK(ops[1].kind == PathOp::Kind::Quad);
  CHECK(ops[1].qx == Approx(5));
  CHECK(ops[1].qy == Approx(10));
  CHECK(ops[2].kind == PathOp::Kind::Quad);
  // reflection of (5,10) about the current point (10,0) is (15,-10)
  CHECK(ops[2].qx == Approx(15));
  CHECK(ops[2].qy == Approx(-10));
}

TEST_CASE("parsePathData: Z closes the path and resets to the subpath start",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 L10 0 L10 10 Z l5 5");
  REQUIRE(ops.size() == 5);
  CHECK(ops[3].kind == PathOp::Kind::Close);
  // after Z, current point resets to (0,0), so a relative lineto lands at (5,5)
  CHECK(ops[4].x == Approx(5));
  CHECK(ops[4].y == Approx(5));
}

TEST_CASE("parsePathData: a degenerate arc (rx or ry == 0) falls back to a "
          "line",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 A0 10 0 0 1 10 10");
  REQUIRE(ops.size() == 2);
  CHECK(ops[1].kind == PathOp::Kind::Line);
  CHECK(ops[1].x == Approx(10));
  CHECK(ops[1].y == Approx(10));
}

TEST_CASE("parsePathData: a real arc expands into cubics ending at the "
          "target point",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 A5 5 0 0 1 10 0");
  REQUIRE(ops.size() >= 2);
  const auto &last = ops.back();
  CHECK(last.kind == PathOp::Kind::Cubic);
  CHECK(last.x == Approx(10).margin(1e-3));
  CHECK(last.y == Approx(0).margin(1e-3));
}

TEST_CASE("parsePathData: bare coordinate pairs reuse the last command letter",
          "[svg][path]") {
  auto ops = parsePathData("M0 0 L10 0 20 0 30 0");
  REQUIRE(ops.size() == 4);
  for (size_t i = 1; i < ops.size(); ++i)
    CHECK(ops[i].kind == PathOp::Kind::Line);
  CHECK(ops[3].x == Approx(30));
}

// ---------------------------------------------------------------------
// Primitive shapes -> path ops
// ---------------------------------------------------------------------

TEST_CASE("rectToOps: a plain rectangle is four lines and a close",
          "[svg][shapes]") {
  auto ops = rectToOps(0, 0, 10, 20, 0, 0);
  REQUIRE(ops.size() == 5);
  CHECK(ops.front().kind == PathOp::Kind::Move);
  CHECK(ops.back().kind == PathOp::Kind::Close);
  CHECK(ops[2].x == Approx(10));
  CHECK(ops[2].y == Approx(20));
}

TEST_CASE("rectToOps: a zero-size rectangle produces no ops", "[svg][shapes]") {
  CHECK(rectToOps(0, 0, 0, 10, 0, 0).empty());
  CHECK(rectToOps(0, 0, 10, 0, 0, 0).empty());
}

TEST_CASE("rectToOps: corner radii are clamped to half the box",
          "[svg][shapes]") {
  // rx/ry of 100 on a 10x20 box must clamp to 5/10, not overflow it.
  auto ops = rectToOps(0, 0, 10, 20, 100, 100);
  REQUIRE_FALSE(ops.empty());
  CHECK(ops.front().kind == PathOp::Kind::Move);
  CHECK(ops.front().x == Approx(5)); // x + clamped rx (5)
  CHECK(ops.front().y == Approx(0));
}

TEST_CASE("ellipseToOps: degenerate radius yields no ops", "[svg][shapes]") {
  CHECK(ellipseToOps(0, 0, 0, 5).empty());
}

TEST_CASE("ellipseToOps: starts at the rightmost point and closes",
          "[svg][shapes]") {
  auto ops = ellipseToOps(10, 10, 5, 3);
  REQUIRE_FALSE(ops.empty());
  CHECK(ops.front().kind == PathOp::Kind::Move);
  CHECK(ops.front().x == Approx(15));
  CHECK(ops.front().y == Approx(10));
  CHECK(ops.back().kind == PathOp::Kind::Close);
}

TEST_CASE("polyToOps: polyline does not close, polygon does",
          "[svg][shapes]") {
  auto line = polyToOps("0,0 10,0 10,10", false);
  auto poly = polyToOps("0,0 10,0 10,10", true);
  REQUIRE(line.size() == 3);
  CHECK(line.back().kind != PathOp::Kind::Close);
  REQUIRE(poly.size() == 4);
  CHECK(poly.front().kind == PathOp::Kind::Move);
  CHECK(poly[1].kind == PathOp::Kind::Line);
  CHECK(poly.back().kind == PathOp::Kind::Close);
}

TEST_CASE("polyToOps: a trailing unpaired number is dropped", "[svg][shapes]") {
  auto ops = polyToOps("0,0 10,0 5", false);
  CHECK(ops.size() == 2);
}

// ---------------------------------------------------------------------
// Color / paint parsing
// ---------------------------------------------------------------------

TEST_CASE("parseHexColor: 3, 6 and 8 digit forms", "[svg][color]") {
  Color c;
  REQUIRE(parseHexColor("#f00", c));
  CHECK(c.r == 255);
  CHECK(c.g == 0);
  CHECK(c.b == 0);
  CHECK(c.a == 255);

  REQUIRE(parseHexColor("#336699", c));
  CHECK(c.r == 0x33);
  CHECK(c.g == 0x66);
  CHECK(c.b == 0x99);

  REQUIRE(parseHexColor("#11223344", c));
  CHECK(c.r == 0x11);
  CHECK(c.g == 0x22);
  CHECK(c.b == 0x33);
  CHECK(c.a == 0x44);
}

TEST_CASE("parseHexColor: rejects malformed input", "[svg][color]") {
  Color c;
  CHECK_FALSE(parseHexColor("#zzz", c));
  CHECK_FALSE(parseHexColor("#12345", c)); // wrong length
}

TEST_CASE("namedColor: recognizes a known name and rejects an unknown one",
          "[svg][color]") {
  Color c;
  REQUIRE(namedColor("blue", c));
  CHECK(c.b == 255);
  CHECK_FALSE(namedColor("cornflowerblue", c));
}

TEST_CASE("parsePaint: none and empty mean no paint", "[svg][color]") {
  Color c;
  CHECK_FALSE(parsePaint("none", c));
  CHECK_FALSE(parsePaint("", c));
}

TEST_CASE("parsePaint: rgb() and rgba()", "[svg][color]") {
  Color c;
  REQUIRE(parsePaint("rgb(255, 0, 128)", c));
  CHECK(c.r == 255);
  CHECK(c.g == 0);
  CHECK(c.b == 128);
  CHECK(c.a == 255);

  REQUIRE(parsePaint("rgba(0,0,0,0.5)", c));
  CHECK(c.a == Approx(127).margin(1));
}

TEST_CASE("parseOpacity: clamps to [0,1] and understands percentages",
          "[svg][color]") {
  CHECK(parseOpacity("") == Approx(1.0f));
  CHECK(parseOpacity("0.5") == Approx(0.5f));
  CHECK(parseOpacity("50%") == Approx(0.5f));
  CHECK(parseOpacity("2") == Approx(1.0f));
  CHECK(parseOpacity("-1") == Approx(0.0f));
}

TEST_CASE("parseDashArray: none/empty is solid, otherwise a list of numbers",
          "[svg][style]") {
  CHECK(parseDashArray("").empty());
  CHECK(parseDashArray("none").empty());
  auto d = parseDashArray("4, 2, 1");
  REQUIRE(d.size() == 3);
  CHECK(d[0] == Approx(4));
  CHECK(d[2] == Approx(1));
}

// ---------------------------------------------------------------------
// Declarations / stylesheet / style inheritance
// ---------------------------------------------------------------------

TEST_CASE("parseDeclarations: splits and trims a semicolon-separated block",
          "[svg][style]") {
  auto decls = parseDeclarations(" fill : none ; stroke:#000 ");
  REQUIRE(decls.size() == 2);
  CHECK(decls[0].first == "fill");
  CHECK(decls[0].second == "none");
  CHECK(decls[1].first == "stroke");
  CHECK(decls[1].second == "#000");
}

TEST_CASE("parseStylesheet: extracts class rules and ignores comments",
          "[svg][style]") {
  auto sheet = parseStylesheet("/* c */ .a, .b { fill: red; } .c{stroke:blue}");
  REQUIRE(sheet.count(".a") == 1);
  REQUIRE(sheet.count(".b") == 1);
  REQUIRE(sheet.count(".c") == 1);
  CHECK(sheet.at(".a") == " fill: red; ");
  CHECK(sheet.at(".b") == " fill: red; ");
  CHECK(sheet.at(".c") == "stroke:blue");
}

TEST_CASE("applyStyle: presentation attributes override inherited defaults",
          "[svg][style]") {
  InheritedStyle base;
  base.hasFill = true;
  base.fill = {1, 2, 3, 255};

  std::unordered_map<std::string, std::string> attrs = {{"fill", "#ff0000"},
                                                          {"stroke", "none"}};
  auto s = applyStyle(attrs, base);
  CHECK(s.hasFill);
  CHECK(s.fill.r == 255);
  CHECK_FALSE(s.hasStroke);
}

TEST_CASE("applyStyle: inline style=\"\" wins over a stylesheet class rule",
          "[svg][style]") {
  Stylesheet sheet = {{".red", "fill:#ff0000"}};
  std::unordered_map<std::string, std::string> attrs = {
      {"class", "red"}, {"style", "fill:#00ff00"}};
  auto s = applyStyle(attrs, InheritedStyle(), &sheet);
  CHECK(s.fill.g == 255);
  CHECK(s.fill.r == 0);
}

TEST_CASE("applyStyle: a stylesheet class applies when there's no inline "
          "style",
          "[svg][style]") {
  Stylesheet sheet = {{".blue", "fill:#0000ff"}};
  std::unordered_map<std::string, std::string> attrs = {{"class", "blue"}};
  auto s = applyStyle(attrs, InheritedStyle(), &sheet);
  CHECK(s.fill.b == 255);
}

// ---------------------------------------------------------------------
// Full-document parsing
// ---------------------------------------------------------------------

TEST_CASE("parseString: rejects a document whose root is not <svg>",
          "[svg][document]") {
  CHECK_FALSE(parseString("<notsvg/>").has_value());
}

TEST_CASE("parseString: rejects unparsable input", "[svg][document]") {
  CHECK_FALSE(parseString("").has_value());
}

TEST_CASE("parseString: viewBox sets document size and origin",
          "[svg][document]") {
  auto doc = parseString(R"(<svg viewBox="10 20 100 50"/>)");
  REQUIRE(doc.has_value());
  CHECK(doc->width == Approx(100));
  CHECK(doc->height == Approx(50));
  CHECK(doc->viewBoxX == Approx(10));
  CHECK(doc->viewBoxY == Approx(20));
}

TEST_CASE("parseString: falls back to width/height attrs without a viewBox",
          "[svg][document]") {
  auto doc = parseString(R"(<svg width="200" height="80"/>)");
  REQUIRE(doc.has_value());
  CHECK(doc->width == Approx(200));
  CHECK(doc->height == Approx(80));
}

TEST_CASE("parseString: falls back to a 100x100 default with no size info",
          "[svg][document]") {
  auto doc = parseString("<svg/>");
  REQUIRE(doc.has_value());
  CHECK(doc->width == Approx(100));
  CHECK(doc->height == Approx(100));
}

TEST_CASE("parseString: a plain rect produces one filled shape",
          "[svg][document]") {
  auto doc = parseString(
      R"(<svg><rect x="0" y="0" width="10" height="10" fill="#ff0000"/></svg>)");
  REQUIRE(doc.has_value());
  REQUIRE(doc->shapes.size() == 1);
  CHECK(doc->shapes[0].hasFill);
  CHECK(doc->shapes[0].fill.r == 255);
}

TEST_CASE("parseString: <defs> content is not rendered on its own",
          "[svg][document]") {
  auto doc =
      parseString(R"(<svg><defs><rect width="10" height="10"/></defs></svg>)");
  REQUIRE(doc.has_value());
  CHECK(doc->shapes.empty());
}

TEST_CASE("parseString: <use> instantiates a <defs> shape with its own "
          "translate",
          "[svg][document]") {
  auto doc = parseString(
      R"(<svg><defs><rect id="r" width="10" height="10"/></defs>
           <use href="#r" x="5" y="7"/></svg>)");
  REQUIRE(doc.has_value());
  REQUIRE(doc->shapes.size() == 1);
  // <use>'s x/y becomes an extra translate baked into the shape's transform.
  CHECK(doc->shapes[0].transform.e == Approx(5));
  CHECK(doc->shapes[0].transform.f == Approx(7));
}

TEST_CASE("parseString: a self-referencing <use> cycle terminates instead of "
          "looping forever",
          "[svg][document]") {
  auto doc = parseString(R"(<svg><g id="g"><use href="#g"/></g></svg>)");
  REQUIRE(doc.has_value()); // the point of this test: it must terminate
  CHECK(doc->shapes.empty());
}

TEST_CASE("parseString: nested <g> transforms compose onto child shapes",
          "[svg][document]") {
  auto doc = parseString(
      R"xml(<svg><g transform="translate(10,0)">
           <rect width="5" height="5" transform="translate(0,10)"/>
         </g></svg>)xml");
  REQUIRE(doc.has_value());
  REQUIRE(doc->shapes.size() == 1);
  CHECK(doc->shapes[0].transform.e == Approx(10));
  CHECK(doc->shapes[0].transform.f == Approx(10));
}

TEST_CASE("parseString: a <style> block's class rule reaches child shapes",
          "[svg][document]") {
  auto doc = parseString(
      R"(<svg><style>.a{fill:#0000ff}</style><rect class="a" width="1" height="1"/></svg>)");
  REQUIRE(doc.has_value());
  REQUIRE(doc->shapes.size() == 1);
  CHECK(doc->shapes[0].fill.b == 255);
}