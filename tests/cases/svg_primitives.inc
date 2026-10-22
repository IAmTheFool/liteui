// Tests for the low-level SVG helpers: numbers, transforms, colours, opacity,
// dash arrays, CSS declarations and stylesheets.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

// ------------------------------------------------------------ scanNumber

TEST_CASE("svg: scanNumber reads the usual number shapes", "[svg][number]") {
  auto scan = [](const std::string &s, float &out) {
    size_t i = 0;
    return svg::scanNumber(s, i, out);
  };
  float v = 0;
  REQUIRE(scan("42", v));      CHECK(v == approx(42));
  REQUIRE(scan("-3.5", v));    CHECK(v == approx(-3.5));
  REQUIRE(scan("+7", v));      CHECK(v == approx(7));
  REQUIRE(scan(".5", v));      CHECK(v == approx(0.5));
  REQUIRE(scan("5.", v));      CHECK(v == approx(5));
  REQUIRE(scan("1.5e2", v));   CHECK(v == approx(150));
  REQUIRE(scan("2E-2", v));    CHECK(v == approx(0.02));
  REQUIRE(scan("  ,  9", v));  CHECK(v == approx(9)); // leading separators
}

TEST_CASE("svg: scanNumber rejects non-numbers and leaves the cursor alone",
          "[svg][number]") {
  float v = -1;
  for (const char *s : {"", "abc", "-", "+", ".", "-.", "  "}) {
    DYNAMIC_SECTION("input: '" << s << "'") {
      std::string str = s;
      size_t i = 0;
      CHECK_FALSE(svg::scanNumber(str, i, v));
    }
  }
}

TEST_CASE("svg: scanNumber stops before a dangling exponent", "[svg][number]") {
  std::string s = "1e";
  size_t i = 0;
  float v = 0;
  REQUIRE(svg::scanNumber(s, i, v));
  CHECK(v == approx(1));
  CHECK(i == 1); // the 'e' was NOT consumed
}

TEST_CASE("svg: compact number lists split on signs and dots", "[svg][number]") {
  // SVG writers love omitting separators: "10-20" is 10 then -20,
  // and ".5.5" is 0.5 then 0.5.
  auto all = [](const std::string &s) {
    std::vector<float> out;
    size_t i = 0;
    float v;
    while (svg::scanNumber(s, i, v))
      out.push_back(v);
    return out;
  };
  auto a = all("10-20");
  REQUIRE(a.size() == 2);
  CHECK(a[0] == approx(10));
  CHECK(a[1] == approx(-20));

  auto b = all(".5.5");
  REQUIRE(b.size() == 2);
  CHECK(b[0] == approx(0.5));
  CHECK(b[1] == approx(0.5));

  CHECK(all("1,2 , 3\n4").size() == 4);
}

// ------------------------------------------------------------------ Mat2x3

TEST_CASE("svg: Mat2x3 identity leaves points alone", "[svg][matrix]") {
  auto [x, y] = apply(svg::Mat2x3::identity(), 3, 4);
  CHECK(x == approx(3));
  CHECK(y == approx(4));
}

TEST_CASE("svg: Mat2x3 basic transforms", "[svg][matrix]") {
  SECTION("translate") {
    auto [x, y] = apply(svg::Mat2x3::translate(10, -5), 1, 1);
    CHECK(x == approx(11));
    CHECK(y == approx(-4));
  }
  SECTION("scale") {
    auto [x, y] = apply(svg::Mat2x3::scaleM(2, 3), 4, 5);
    CHECK(x == approx(8));
    CHECK(y == approx(15));
  }
  SECTION("rotate 90 degrees takes +x to +y") {
    auto [x, y] = apply(svg::Mat2x3::rotate(3.14159265f / 2), 1, 0);
    CHECK(x == approx(0));
    CHECK(y == approx(1));
  }
  SECTION("skewX 45 degrees shears x by y") {
    auto [x, y] = apply(svg::Mat2x3::skewX(3.14159265f / 4), 0, 2);
    CHECK(x == approx(2));
    CHECK(y == approx(2));
  }
  SECTION("skewY 45 degrees shears y by x") {
    auto [x, y] = apply(svg::Mat2x3::skewY(3.14159265f / 4), 2, 0);
    CHECK(x == approx(2));
    CHECK(y == approx(2));
  }
}

TEST_CASE("svg: Mat2x3::multiply applies the right-hand matrix first",
          "[svg][matrix]") {
  // (translate * scale)(p) == translate(scale(p))
  auto m = svg::Mat2x3::translate(10, 0).multiply(svg::Mat2x3::scaleM(2, 2));
  auto [x, y] = apply(m, 1, 1);
  CHECK(x == approx(12));
  CHECK(y == approx(2));

  // ...and the other order gives a different answer: scale(translate(p)).
  auto m2 = svg::Mat2x3::scaleM(2, 2).multiply(svg::Mat2x3::translate(10, 0));
  auto [x2, y2] = apply(m2, 1, 1);
  CHECK(x2 == approx(22));
  CHECK(y2 == approx(2));
}

// ---------------------------------------------------------- parseTransform

TEST_CASE("svg: parseTransform - each function", "[svg][transform]") {
  SECTION("translate with one and two arguments") {
    auto a = svg::parseTransform("translate(10 20)");
    CHECK(a.e == approx(10));
    CHECK(a.f == approx(20));
    auto b = svg::parseTransform("translate(5)");
    CHECK(b.e == approx(5));
    CHECK(b.f == approx(0)); // second argument defaults to 0
  }
  SECTION("scale: one argument is uniform") {
    auto a = svg::parseTransform("scale(2)");
    CHECK(a.a == approx(2));
    CHECK(a.d == approx(2));
    auto b = svg::parseTransform("scale(2, 3)");
    CHECK(b.a == approx(2));
    CHECK(b.d == approx(3));
  }
  SECTION("rotate about the origin") {
    auto m = svg::parseTransform("rotate(90)");
    auto [x, y] = apply(m, 1, 0);
    CHECK(x == approx(0));
    CHECK(y == approx(1));
  }
  SECTION("rotate about a point leaves that point fixed") {
    auto m = svg::parseTransform("rotate(90 10 10)");
    auto [fx, fy] = apply(m, 10, 10);
    CHECK(fx == approx(10));
    CHECK(fy == approx(10));
    auto [x, y] = apply(m, 11, 10);
    CHECK(x == approx(10));
    CHECK(y == approx(11));
  }
  SECTION("skewX / skewY take degrees") {
    CHECK(svg::parseTransform("skewX(45)").c == approx(1));
    CHECK(svg::parseTransform("skewY(45)").b == approx(1));
  }
  SECTION("matrix(a b c d e f)") {
    auto m = svg::parseTransform("matrix(1 2 3 4 5 6)");
    CHECK(m.a == approx(1));
    CHECK(m.b == approx(2));
    CHECK(m.c == approx(3));
    CHECK(m.d == approx(4));
    CHECK(m.e == approx(5));
    CHECK(m.f == approx(6));
  }
}

TEST_CASE("svg: parseTransform - lists compose left to right", "[svg][transform]") {
  // "translate(10,0) scale(2)" means: scale first, then translate.
  auto m = svg::parseTransform("translate(10,0) scale(2)");
  auto [x, y] = apply(m, 1, 1);
  CHECK(x == approx(12));
  CHECK(y == approx(2));

  // Commas between functions are tolerated too.
  auto m2 = svg::parseTransform("translate(10,0),scale(2)");
  auto [x2, y2] = apply(m2, 1, 1);
  CHECK(x2 == approx(12));
  CHECK(y2 == approx(2));
}

TEST_CASE("svg: parseTransform - bad input degrades to identity",
          "[svg][transform]") {
  auto isIdentity = [](const svg::Mat2x3 &m) {
    return m.a == approx(1) && m.b == approx(0) && m.c == approx(0) &&
           m.d == approx(1) && m.e == approx(0) && m.f == approx(0);
  };
  CHECK(isIdentity(svg::parseTransform("")));
  CHECK(isIdentity(svg::parseTransform("garbage")));
  CHECK(isIdentity(svg::parseTransform("((")));
  CHECK(isIdentity(svg::parseTransform("frobnicate(1 2)")));
  CHECK(isIdentity(svg::parseTransform("translate()")));       // no args
  CHECK(isIdentity(svg::parseTransform("matrix(1 2 3)")));     // too few
}

// ------------------------------------------------------------ colours

TEST_CASE("svg: parseHexColor", "[svg][color]") {
  Color c;
  SECTION("#rgb expands each digit") {
    REQUIRE(svg::parseHexColor("#fff", c));
    CHECK(c == Color{255, 255, 255, 255});
    REQUIRE(svg::parseHexColor("#f80", c));
    CHECK(c == Color{255, 136, 0, 255});
  }
  SECTION("#rrggbb") {
    REQUIRE(svg::parseHexColor("#ff8000", c));
    CHECK(c == Color{255, 128, 0, 255});
    REQUIRE(svg::parseHexColor("#0A0b0C", c)); // mixed case
    CHECK(c == Color{10, 11, 12, 255});
  }
  SECTION("#rrggbbaa") {
    REQUIRE(svg::parseHexColor("#ff800080", c));
    CHECK(c == Color{255, 128, 0, 128});
  }
  SECTION("invalid forms are rejected") {
    CHECK_FALSE(svg::parseHexColor("#ggg", c));
    CHECK_FALSE(svg::parseHexColor("#12", c));
    CHECK_FALSE(svg::parseHexColor("#1234", c));   // #rgba is not supported
    CHECK_FALSE(svg::parseHexColor("#12345", c));
    CHECK_FALSE(svg::parseHexColor("#12345g", c));
    CHECK_FALSE(svg::parseHexColor("#", c));
  }
}

TEST_CASE("svg: namedColor", "[svg][color]") {
  Color c;
  REQUIRE(svg::namedColor("red", c));
  CHECK(c == Color{255, 0, 0, 255});
  REQUIRE(svg::namedColor("green", c));
  CHECK(c == Color{0, 128, 0, 255}); // CSS green, not lime
  REQUIRE(svg::namedColor("lime", c));
  CHECK(c == Color{0, 255, 0, 255});
  REQUIRE(svg::namedColor("transparent", c));
  CHECK(c.a == 0);
  REQUIRE(svg::namedColor("grey", c));
  Color gray;
  REQUIRE(svg::namedColor("gray", gray));
  CHECK(c == gray); // both spellings work
  CHECK_FALSE(svg::namedColor("RED", c)); // lookup is case-sensitive
  CHECK_FALSE(svg::namedColor("notacolor", c));
}

TEST_CASE("svg: parsePaint", "[svg][color]") {
  Color c;
  SECTION("none and empty mean 'no paint'") {
    CHECK_FALSE(svg::parsePaint("none", c));
    CHECK_FALSE(svg::parsePaint("", c));
    CHECK_FALSE(svg::parsePaint("   ", c));
  }
  SECTION("hex, named, and surrounding whitespace") {
    REQUIRE(svg::parsePaint("#00f", c));
    CHECK(c == Color{0, 0, 255, 255});
    REQUIRE(svg::parsePaint("  blue \n", c));
    CHECK(c == Color{0, 0, 255, 255});
  }
  SECTION("rgb() with integers") {
    REQUIRE(svg::parsePaint("rgb(255, 128, 0)", c));
    CHECK(c == Color{255, 128, 0, 255});
    REQUIRE(svg::parsePaint("rgb(1 2 3)", c)); // space-separated
    CHECK(c == Color{1, 2, 3, 255});
  }
  SECTION("rgb() with percentages") {
    REQUIRE(svg::parsePaint("rgb(100%, 0%, 50%)", c));
    CHECK(c.r == 255);
    CHECK(c.g == 0);
    CHECK(c.b == 127);
  }
  SECTION("rgba() alpha is a 0..1 fraction") {
    REQUIRE(svg::parsePaint("rgba(0, 0, 0, 0.5)", c));
    CHECK(c.a == 127);
    REQUIRE(svg::parsePaint("rgba(0, 0, 0, 1)", c));
    CHECK(c.a == 255);
    REQUIRE(svg::parsePaint("rgba(0, 0, 0, 0)", c));
    CHECK(c.a == 0);
  }
  SECTION("out-of-range channels are clamped") {
    REQUIRE(svg::parsePaint("rgb(300, -5, 10)", c));
    CHECK(c == Color{255, 0, 10, 255});
  }
  SECTION("too few components, unknown names") {
    CHECK_FALSE(svg::parsePaint("rgb(1, 2)", c));
    CHECK_FALSE(svg::parsePaint("bogus", c));
    CHECK_FALSE(svg::parsePaint("url(#gradient)", c)); // gradients unsupported
  }
}

TEST_CASE("svg: parseOpacity", "[svg][color]") {
  CHECK(svg::parseOpacity("") == approx(1));      // absent => opaque
  CHECK(svg::parseOpacity("0.25") == approx(0.25));
  CHECK(svg::parseOpacity("50%") == approx(0.5));
  CHECK(svg::parseOpacity("2") == approx(1));     // clamped high
  CHECK(svg::parseOpacity("-1") == approx(0));    // clamped low
  CHECK(svg::parseOpacity("0") == approx(0));
}

// --------------------------------------------------- dash arrays, CSS text

TEST_CASE("svg: parseDashArray", "[svg][style]") {
  CHECK(svg::parseDashArray("").empty());
  CHECK(svg::parseDashArray("none").empty());

  auto a = svg::parseDashArray("5,3");
  REQUIRE(a.size() == 2);
  CHECK(a[0] == approx(5));
  CHECK(a[1] == approx(3));

  auto b = svg::parseDashArray("4 2 1");
  CHECK(b.size() == 3);
}

TEST_CASE("svg: parseDeclarations splits and trims", "[svg][style]") {
  auto d = svg::parseDeclarations("fill:red; stroke : blue ;;junk; stroke-width:2");
  REQUIRE(d.size() == 3);
  CHECK(d[0] == std::pair<std::string, std::string>{"fill", "red"});
  CHECK(d[1] == std::pair<std::string, std::string>{"stroke", "blue"});
  CHECK(d[2] == std::pair<std::string, std::string>{"stroke-width", "2"});

  CHECK(svg::parseDeclarations("").empty());
  CHECK(svg::parseDeclarations(":novalue-key").empty()); // empty key dropped
}

TEST_CASE("svg: parseStylesheet - class rules only", "[svg][style]") {
  SECTION("simple rule") {
    auto s = svg::parseStylesheet(".a{fill:red}");
    REQUIRE(s.count(".a") == 1);
    CHECK(s[".a"] == "fill:red");
  }
  SECTION("selector lists get one entry per class") {
    auto s = svg::parseStylesheet(".a, .b ,.c { stroke:blue }");
    CHECK(s.size() == 3);
    CHECK(s[".a"] == " stroke:blue ");
    CHECK(s.count(".b") == 1);
    CHECK(s.count(".c") == 1);
  }
  SECTION("comments are stripped") {
    auto s = svg::parseStylesheet("/* header */ .a{fill:red} /* tail */");
    CHECK(s.size() == 1);
  }
  SECTION("tag, id and other selectors are ignored") {
    auto s = svg::parseStylesheet("rect{fill:red} #id{fill:blue} .keep{fill:lime}");
    CHECK(s.size() == 1);
    CHECK(s.count(".keep") == 1);
  }
  SECTION("a redefined class appends, so the later rule wins on iteration") {
    auto s = svg::parseStylesheet(".a{fill:red} .a{fill:blue}");
    CHECK(s[".a"] == "fill:red;fill:blue");
    auto decls = svg::parseDeclarations(s[".a"]);
    REQUIRE(decls.size() == 2);
    CHECK(decls.back().second == "blue");
  }
  SECTION("unterminated rule is ignored without crashing") {
    CHECK(svg::parseStylesheet(".a{fill:red").empty());
    CHECK(svg::parseStylesheet("").empty());
  }
}
