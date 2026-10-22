// End-to-end SVG tests: whole documents through svg::parseString/parseFile,
// checking the resulting shapes, styles and transforms.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

// ------------------------------------------------------- document / viewBox

TEST_CASE("svg doc: not an SVG at all", "[svg][doc]") {
  CHECK_FALSE(svg::parseString("").has_value());
  CHECK_FALSE(svg::parseString("hello").has_value());
  CHECK_FALSE(svg::parseString("<html><body/></html>").has_value()); // wrong root
  CHECK_FALSE(svg::parseString("<rect width='1' height='1'/>").has_value());
}

TEST_CASE("svg doc: an empty <svg> is valid", "[svg][doc]") {
  auto doc = mustParse("<svg/>");
  CHECK(doc.shapes.empty());
  CHECK(doc.width == approx(100)); // default size when nothing is given
  CHECK(doc.height == approx(100));
  CHECK(doc.viewBoxX == approx(0));
  CHECK(doc.viewBoxY == approx(0));
}

TEST_CASE("svg doc: size comes from viewBox, else width/height", "[svg][doc]") {
  SECTION("width and height attributes only") {
    auto doc = mustParse(R"xml(<svg width="320" height="200"/>)xml");
    CHECK(doc.width == approx(320));
    CHECK(doc.height == approx(200));
  }
  SECTION("viewBox only") {
    auto doc = mustParse(R"xml(<svg viewBox="0 0 24 16"/>)xml");
    CHECK(doc.width == approx(24));
    CHECK(doc.height == approx(16));
  }
  SECTION("viewBox wins over width/height") {
    auto doc = mustParse(R"xml(<svg width="500" height="500" viewBox="0 0 24 16"/>)xml");
    CHECK(doc.width == approx(24));
    CHECK(doc.height == approx(16));
  }
  SECTION("viewBox accepts commas") {
    auto doc = mustParse(R"xml(<svg viewBox="0,0,48,32"/>)xml");
    CHECK(doc.width == approx(48));
    CHECK(doc.height == approx(32));
  }
  SECTION("a malformed viewBox is ignored") {
    auto doc = mustParse(R"xml(<svg width="30" height="40" viewBox="0 0 10"/>)xml");
    CHECK(doc.width == approx(30));
    CHECK(doc.height == approx(40));
  }
}

TEST_CASE("svg doc: viewBox origin shifts every shape", "[svg][doc]") {
  auto doc = mustParse(
      R"xml(<svg viewBox="10 20 100 50"><rect width="5" height="5"/></svg>)xml");
  CHECK(doc.viewBoxX == approx(10));
  CHECK(doc.viewBoxY == approx(20));
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].transform.e == approx(-10));
  CHECK(doc.shapes[0].transform.f == approx(-20));
}

TEST_CASE("svg doc: prolog, DOCTYPE and comments before <svg>", "[svg][doc]") {
  auto doc = mustParse(R"xml(<?xml version="1.0"?>
<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd">
<!-- exported by something -->
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8">
  <rect width="8" height="8"/>
</svg>)xml");
  CHECK(doc.shapes.size() == 1);
}

// --------------------------------------------------------- basic shapes

TEST_CASE("svg doc: each basic shape produces one Shape", "[svg][doc][shapes]") {
  auto doc = mustParse(R"xml(<svg>
    <rect width="10" height="10"/>
    <circle cx="5" cy="5" r="4"/>
    <ellipse cx="5" cy="5" rx="4" ry="2"/>
    <line x1="0" y1="0" x2="10" y2="10"/>
    <polyline points="0,0 5,5 10,0"/>
    <polygon points="0,0 5,5 10,0"/>
    <path d="M0 0 L10 10"/>
  </svg>)xml");
  CHECK(doc.shapes.size() == 7);
}

TEST_CASE("svg doc: degenerate shapes are dropped", "[svg][doc][shapes]") {
  auto doc = mustParse(R"xml(<svg>
    <rect width="0" height="10"/>
    <circle r="0"/>
    <ellipse rx="3"/>
    <polygon points=""/>
    <path d=""/>
    <path/>
  </svg>)xml");
  CHECK(doc.shapes.empty());
}

TEST_CASE("svg doc: unknown elements are skipped, known ones inside them kept",
          "[svg][doc][shapes]") {
  auto doc = mustParse(R"xml(<svg>
    <text>ignored</text>
    <foreignObject><rect width="3" height="3"/></foreignObject>
    <title>t</title>
  </svg>)xml");
  // Unknown containers recurse, so the rect inside foreignObject is found.
  CHECK(doc.shapes.size() == 1);
}

TEST_CASE("svg doc: rect rx without ry rounds both axes", "[svg][doc][shapes]") {
  auto doc = mustParse(R"xml(<svg><rect width="20" height="20" rx="4"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].ops.size() == 10); // rounded => 10 ops, sharp => 5
}

TEST_CASE("svg doc: line geometry", "[svg][doc][shapes]") {
  auto doc = mustParse(
      R"xml(<svg><line x1="1" y1="2" x2="3" y2="4" stroke="black"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  auto &ops = doc.shapes[0].ops;
  REQUIRE(ops.size() == 2);
  CHECK(ops[0].x == approx(1));
  CHECK(ops[0].y == approx(2));
  CHECK(ops[1].x == approx(3));
  CHECK(ops[1].y == approx(4));
}

// ----------------------------------------------------- fill and stroke

TEST_CASE("svg doc: default paint is black fill, no stroke", "[svg][doc][style]") {
  auto doc = mustParse(R"xml(<svg><rect width="1" height="1"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  auto &s = doc.shapes[0];
  CHECK(s.hasFill);
  CHECK(s.fill == Color{0, 0, 0, 255});
  CHECK_FALSE(s.hasStroke);
  CHECK(s.strokeWidth == approx(1));
  CHECK_FALSE(s.evenOdd);
  CHECK(s.lineCap == 0);
  CHECK(s.lineJoin == 0);
  CHECK(s.miterLimit == approx(4));
  CHECK(s.dashArray.empty());
}

TEST_CASE("svg doc: fill and stroke attributes", "[svg][doc][style]") {
  auto doc = mustParse(R"xml(<svg>
    <rect width="1" height="1" fill="#ff0000" stroke="blue" stroke-width="3"/>
    <rect width="1" height="1" fill="none"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 2);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
  CHECK(doc.shapes[0].hasStroke);
  CHECK(doc.shapes[0].stroke == Color{0, 0, 255, 255});
  CHECK(doc.shapes[0].strokeWidth == approx(3));
  CHECK_FALSE(doc.shapes[1].hasFill);
}

TEST_CASE("svg doc: an unparseable paint value keeps the inherited one",
          "[svg][doc][style]") {
  auto doc = mustParse(
      R"xml(<svg fill="red"><rect width="1" height="1" fill="url(#g)"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
}

TEST_CASE("svg doc: opacity multiplies into the alpha channel",
          "[svg][doc][style]") {
  SECTION("opacity alone") {
    auto doc = mustParse(
        R"xml(<svg><rect width="1" height="1" fill="#000" opacity="0.5"/></svg>)xml");
    CHECK(doc.shapes[0].fill.a == 127);
  }
  SECTION("fill-opacity and opacity compound") {
    auto doc = mustParse(R"xml(<svg><rect width="1" height="1" fill="#000"
                            fill-opacity="0.5" opacity="0.5"/></svg>)xml");
    CHECK(doc.shapes[0].fill.a == 63);
  }
  SECTION("stroke-opacity only affects the stroke") {
    auto doc = mustParse(R"xml(<svg><rect width="1" height="1" fill="#000"
                            stroke="#000" stroke-opacity="0.5"/></svg>)xml");
    CHECK(doc.shapes[0].fill.a == 255);
    CHECK(doc.shapes[0].stroke.a == 127);
  }
  SECTION("an rgba alpha is respected too") {
    auto doc = mustParse(
        R"xml(<svg><rect width="1" height="1" fill="rgba(0,0,0,0.5)"/></svg>)xml");
    CHECK(doc.shapes[0].fill.a == 127);
  }
}

TEST_CASE("svg doc: stroke geometry properties", "[svg][doc][style]") {
  auto doc = mustParse(R"xml(<svg><path d="M0 0 L10 10" stroke="#000"
      stroke-linecap="round" stroke-linejoin="bevel" stroke-miterlimit="8"
      stroke-dasharray="4 2" stroke-dashoffset="1" fill-rule="evenodd"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  auto &s = doc.shapes[0];
  CHECK(s.lineCap == 1);
  CHECK(s.lineJoin == 2);
  CHECK(s.miterLimit == approx(8));
  REQUIRE(s.dashArray.size() == 2);
  CHECK(s.dashArray[0] == approx(4));
  CHECK(s.dashOffset == approx(1));
  CHECK(s.evenOdd);
}

TEST_CASE("svg doc: linecap and linejoin keyword mapping", "[svg][doc][style]") {
  auto cap = [](const char *v) {
    auto doc = mustParse(std::string("<svg><path d='M0 0 L1 1' stroke='#000' "
                                     "stroke-linecap='") + v + "'/></svg>");
    return doc.shapes[0].lineCap;
  };
  auto join = [](const char *v) {
    auto doc = mustParse(std::string("<svg><path d='M0 0 L1 1' stroke='#000' "
                                     "stroke-linejoin='") + v + "'/></svg>");
    return doc.shapes[0].lineJoin;
  };
  CHECK(cap("butt") == 0);
  CHECK(cap("round") == 1);
  CHECK(cap("square") == 2);
  CHECK(cap("nonsense") == 0);
  CHECK(join("miter") == 0);
  CHECK(join("round") == 1);
  CHECK(join("bevel") == 2);
  CHECK(join("arcs") == 0); // unsupported values fall back to miter
}

TEST_CASE("svg doc: stroke properties are dropped when there is no stroke",
          "[svg][doc][style]") {
  auto doc = mustParse(R"xml(<svg><rect width="1" height="1" stroke="none"
                          stroke-width="9" stroke-linecap="round"/></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK_FALSE(doc.shapes[0].hasStroke);
  CHECK(doc.shapes[0].strokeWidth == approx(1)); // Shape default, not 9
  CHECK(doc.shapes[0].lineCap == 0);
}

// ------------------------------------------------ inheritance and groups

TEST_CASE("svg doc: groups pass style down; children can override",
          "[svg][doc][group]") {
  auto doc = mustParse(R"xml(<svg>
    <g fill="red" stroke="blue">
      <rect width="1" height="1"/>
      <rect width="1" height="1" fill="lime"/>
      <g stroke="none"><rect width="1" height="1"/></g>
    </g>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 3);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
  CHECK(doc.shapes[0].hasStroke);
  CHECK(doc.shapes[1].fill == Color{0, 255, 0, 255});
  CHECK(doc.shapes[1].hasStroke);
  CHECK(doc.shapes[2].fill == Color{255, 0, 0, 255});
  CHECK_FALSE(doc.shapes[2].hasStroke);
}

TEST_CASE("svg doc: style on the root <svg> is inherited", "[svg][doc][group]") {
  auto doc = mustParse(
      R"xml(<svg fill="green"><rect width="1" height="1"/></svg>)xml");
  CHECK(doc.shapes[0].fill == Color{0, 128, 0, 255});
}

TEST_CASE("svg doc: transforms accumulate down the tree", "[svg][doc][group]") {
  auto doc = mustParse(R"xml(<svg>
    <g transform="translate(10,0)">
      <g transform="scale(2)">
        <rect width="1" height="1" transform="translate(0,5)"/>
      </g>
    </g>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  // Local (0,0): translate(0,5) -> (0,5); scale(2) -> (0,10); translate -> (10,10)
  auto [x, y] = apply(doc.shapes[0].transform, 0, 0);
  CHECK(x == approx(10));
  CHECK(y == approx(10));
}

TEST_CASE("svg doc: nested <svg> recurses like a group", "[svg][doc][group]") {
  auto doc = mustParse(
      R"xml(<svg><svg fill="red"><rect width="1" height="1"/></svg></svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
}

// -------------------------------------------------------- style precedence

TEST_CASE("svg doc: inline style overrides the attribute", "[svg][doc][css]") {
  auto doc = mustParse(R"xml(<svg><rect width="1" height="1" fill="red"
                          style="fill:blue; stroke:#000"/></svg>)xml");
  CHECK(doc.shapes[0].fill == Color{0, 0, 255, 255});
  CHECK(doc.shapes[0].hasStroke);
}

TEST_CASE("svg doc: <style> class rules apply", "[svg][doc][css]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><style>.cls-1{fill:none;stroke:#ff0000;stroke-width:2}</style></defs>
    <rect class="cls-1" width="1" height="1"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK_FALSE(doc.shapes[0].hasFill);
  CHECK(doc.shapes[0].stroke == Color{255, 0, 0, 255});
  CHECK(doc.shapes[0].strokeWidth == approx(2));
}

TEST_CASE("svg doc: <style> inside CDATA works (Illustrator/Figma exports)",
          "[svg][doc][css]") {
  auto doc = mustParse(R"xml(<svg>
    <style><![CDATA[ .a { fill: #00ff00 } ]]></style>
    <rect class="a" width="1" height="1"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].fill == Color{0, 255, 0, 255});
}

TEST_CASE("svg doc: precedence is attribute < class rule < inline style",
          "[svg][doc][css]") {
  auto fillOf = [](const std::string &rectAttrs) {
    auto doc = mustParse("<svg><style>.c{fill:blue}</style><rect width='1' "
                         "height='1' " + rectAttrs + "/></svg>");
    return doc.shapes[0].fill;
  };
  CHECK(fillOf("fill='red'") == Color{255, 0, 0, 255});
  CHECK(fillOf("fill='red' class='c'") == Color{0, 0, 255, 255});
  CHECK(fillOf("fill='red' class='c' style='fill:lime'") == Color{0, 255, 0, 255});
}

TEST_CASE("svg doc: multiple classes on one element", "[svg][doc][css]") {
  auto doc = mustParse(R"xml(<svg>
    <style>.a{fill:red} .b{stroke:blue}</style>
    <rect class="  a   b " width="1" height="1"/>
  </svg>)xml");
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
  CHECK(doc.shapes[0].hasStroke);
}

TEST_CASE("svg doc: an unknown class is harmless", "[svg][doc][css]") {
  auto doc = mustParse(R"xml(<svg><style>.a{fill:red}</style>
                          <rect class="zzz" width="1" height="1"/></svg>)xml");
  CHECK(doc.shapes[0].fill == Color{0, 0, 0, 255});
}

// ------------------------------------------------ <defs>, <symbol>, <use>

TEST_CASE("svg doc: <defs> content is not rendered directly", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="r" width="1" height="1"/></defs>
  </svg>)xml");
  CHECK(doc.shapes.empty());
}

TEST_CASE("svg doc: <use> instantiates its target", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="r" width="1" height="1"/></defs>
    <use href="#r"/>
    <use href="#r"/>
  </svg>)xml");
  CHECK(doc.shapes.size() == 2);
}

TEST_CASE("svg doc: <use> also accepts the legacy xlink:href", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="r" width="1" height="1"/></defs>
    <use xlink:href="#r"/>
  </svg>)xml");
  CHECK(doc.shapes.size() == 1);
}

TEST_CASE("svg doc: <use> x/y and transform both move the instance",
          "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="r" width="1" height="1"/></defs>
    <use href="#r" x="10" y="20"/>
    <use href="#r" transform="translate(5,5)" x="1" y="1"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 2);
  auto [x0, y0] = apply(doc.shapes[0].transform, 0, 0);
  CHECK(x0 == approx(10));
  CHECK(y0 == approx(20));
  auto [x1, y1] = apply(doc.shapes[1].transform, 0, 0);
  CHECK(x1 == approx(6));
  CHECK(y1 == approx(6));
}

TEST_CASE("svg doc: <use> style flows into the instantiated shape",
          "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="r" width="1" height="1"/></defs>
    <use href="#r" fill="red"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
}

TEST_CASE("svg doc: <symbol> renders only through <use>", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <symbol id="s"><rect width="1" height="1"/><circle r="2"/></symbol>
    <use href="#s"/>
  </svg>)xml");
  CHECK(doc.shapes.size() == 2); // both children, once
}

TEST_CASE("svg doc: <use> of a group", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><g id="pair" fill="lime"><rect width="1" height="1"/>
                                   <rect width="2" height="2"/></g></defs>
    <use href="#pair"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 2);
  CHECK(doc.shapes[0].fill == Color{0, 255, 0, 255});
}

TEST_CASE("svg doc: <use> with a missing or odd target is ignored",
          "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <use href="#nope"/>
    <use href="nope"/>
    <use/>
    <use href=""/>
  </svg>)xml");
  CHECK(doc.shapes.empty());
}

TEST_CASE("svg doc: duplicate ids - the first one wins", "[svg][doc][use]") {
  auto doc = mustParse(R"xml(<svg>
    <defs><rect id="d" width="1" height="1" fill="red"/>
          <rect id="d" width="1" height="1" fill="blue"/></defs>
    <use href="#d"/>
  </svg>)xml");
  REQUIRE(doc.shapes.size() == 1);
  CHECK(doc.shapes[0].fill == Color{255, 0, 0, 255});
}

TEST_CASE("svg doc: cyclic <use> references terminate", "[svg][doc][use]") {
  SECTION("A uses B uses A") {
    auto doc = mustParse(R"xml(<svg>
      <defs>
        <g id="a"><rect width="1" height="1"/><use href="#b"/></g>
        <g id="b"><use href="#a"/></g>
      </defs>
      <use href="#a"/>
    </svg>)xml");
    // The rect is emitted once; the cycle back to "a" is cut.
    CHECK(doc.shapes.size() == 1);
  }
  SECTION("a <use> that points at its own ancestor") {
    auto doc = mustParse(R"xml(<svg>
      <g id="self"><rect width="1" height="1"/><use href="#self"/></g>
    </svg>)xml");
    // Walked once as ordinary content, and once more via <use> (which is not
    // an ancestor on the use-stack yet); the second visit's own <use> is then
    // cut. Either way it must terminate with a small, finite count.
    CHECK(doc.shapes.size() >= 1);
    CHECK(doc.shapes.size() <= 3);
  }
}

TEST_CASE("svg doc: very long <use> chains are depth-limited", "[svg][doc][use]") {
  // 100 groups, each using the next. The implementation caps nesting at 32,
  // so the final rect (at depth 100) is never reached — and nothing blows up.
  std::string s = "<svg><defs>";
  const int n = 100;
  for (int i = 0; i < n; ++i)
    s += "<g id='g" + std::to_string(i) + "'><use href='#g" +
         std::to_string(i + 1) + "'/></g>";
  s += "<rect id='g" + std::to_string(n) + "' width='1' height='1'/>";
  s += "</defs><use href='#g0'/></svg>";
  auto doc = mustParse(s);
  CHECK(doc.shapes.empty());
}

// ---------------------------------------------------------- parseFile

TEST_CASE("svg file: parseFile reads a valid file", "[svg][file]") {
  TempFile f(R"xml(<svg viewBox="0 0 4 4"><rect width="4" height="4"/></svg>)xml",
             "liteui_test_valid.svg");
  std::string err;
  auto doc = svg::parseFile(f.path.string(), &err);
  REQUIRE(doc.has_value());
  CHECK(err.empty());
  CHECK(doc->shapes.size() == 1);
  CHECK(doc->width == approx(4));
}

TEST_CASE("svg file: a missing file reports a useful error", "[svg][file]") {
  std::string err;
  auto doc = svg::parseFile("/definitely/not/here/nothing.svg", &err);
  CHECK_FALSE(doc.has_value());
  CHECK(err.find("failed to open") != std::string::npos);
  CHECK(err.find("nothing.svg") != std::string::npos);

  // The error out-parameter is optional.
  CHECK_FALSE(svg::parseFile("/definitely/not/here/nothing.svg").has_value());
}

TEST_CASE("svg file: a file that isn't SVG reports a parse error",
          "[svg][file]") {
  TempFile f("this is not xml", "liteui_test_garbage.svg");
  std::string err;
  auto doc = svg::parseFile(f.path.string(), &err);
  CHECK_FALSE(doc.has_value());
  CHECK(err.find("failed to parse") != std::string::npos);
}

TEST_CASE("svg file: an empty file is a parse error, not a crash",
          "[svg][file]") {
  TempFile f("", "liteui_test_empty.svg");
  std::string err;
  CHECK_FALSE(svg::parseFile(f.path.string(), &err).has_value());
  CHECK(err.find("failed to parse") != std::string::npos);
}
