// Tests for liteui_xml: the small, forgiving XML parser behind the SVG loader.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

// ---------------------------------------------------------------- structure

TEST_CASE("xml: a self-closing element", "[xml]") {
  auto n = xml::parse("<a/>");
  REQUIRE(n);
  CHECK(n->tag == "a");
  CHECK(n->attrs.empty());
  CHECK(n->children.empty());
  CHECK(n->text.empty());
}

TEST_CASE("xml: an element with an explicit close tag", "[xml]") {
  auto n = xml::parse("<a></a>");
  REQUIRE(n);
  CHECK(n->tag == "a");
  CHECK(n->children.empty());
}

TEST_CASE("xml: nested children keep document order", "[xml]") {
  auto n = xml::parse("<root><a/><b><c/></b><d/></root>");
  REQUIRE(n);
  REQUIRE(n->children.size() == 3);
  CHECK(n->children[0].tag == "a");
  CHECK(n->children[1].tag == "b");
  CHECK(n->children[2].tag == "d");
  REQUIRE(n->children[1].children.size() == 1);
  CHECK(n->children[1].children[0].tag == "c");
}

TEST_CASE("xml: leading whitespace is skipped", "[xml]") {
  auto n = xml::parse("  \n\t <a />");
  REQUIRE(n);
  CHECK(n->tag == "a");
}

// --------------------------------------------------------------- attributes

TEST_CASE("xml: attributes with double and single quotes", "[xml][attrs]") {
  auto n = xml::parse(R"xml(<a x="1" y='two' z = "3"/>)xml");
  REQUIRE(n);
  CHECK(n->attrs.size() == 3);
  CHECK(n->attrs.at("x") == "1");
  CHECK(n->attrs.at("y") == "two");
  CHECK(n->attrs.at("z") == "3"); // whitespace around '=' is fine
}

TEST_CASE("xml: an attribute value may contain the other quote kind",
          "[xml][attrs]") {
  auto n = xml::parse(R"xml(<a d="it's" e='say "hi"'/>)xml");
  REQUIRE(n);
  CHECK(n->attrs.at("d") == "it's");
  CHECK(n->attrs.at("e") == "say \"hi\"");
}

TEST_CASE("xml: attribute values are entity-decoded", "[xml][attrs]") {
  auto n = xml::parse(R"xml(<a v="a&amp;b &lt;c&gt;"/>)xml");
  REQUIRE(n);
  CHECK(n->attrs.at("v") == "a&b <c>");
}

TEST_CASE("xml: namespace prefixes are kept as plain text", "[xml][attrs]") {
  auto n = xml::parse(R"xml(<svg:use xlink:href="#icon"/>)xml");
  REQUIRE(n);
  CHECK(n->tag == "svg:use");
  CHECK(n->attrs.at("xlink:href") == "#icon");
}

TEST_CASE("xml: lenient forms - unquoted values and bare attributes",
          "[xml][attrs]") {
  SECTION("unquoted value") {
    auto n = xml::parse("<a x=5 y=hello/>");
    REQUIRE(n);
    CHECK(n->attrs.at("x") == "5");
    CHECK(n->attrs.at("y") == "hello");
  }
  SECTION("attribute with no value at all") {
    auto n = xml::parse("<a disabled/>");
    REQUIRE(n);
    REQUIRE(n->attrs.count("disabled") == 1);
    CHECK(n->attrs.at("disabled").empty());
  }
}

TEST_CASE("xml: a duplicate attribute keeps the last value", "[xml][attrs]") {
  auto n = xml::parse(R"xml(<a x="1" x="2"/>)xml");
  REQUIRE(n);
  CHECK(n->attrs.at("x") == "2");
}

// --------------------------------------------------------------------- text

TEST_CASE("xml: text content", "[xml][text]") {
  auto n = xml::parse("<p>hello world</p>");
  REQUIRE(n);
  CHECK(n->text == "hello world");
}

TEST_CASE("xml: text whitespace is preserved verbatim", "[xml][text]") {
  auto n = xml::parse("<p>  a\n b  </p>");
  REQUIRE(n);
  CHECK(n->text == "  a\n b  ");
}

TEST_CASE("xml: mixed content is concatenated, order vs children is lost",
          "[xml][text]") {
  // Documented limitation: all text at one level joins into Node::text.
  auto n = xml::parse("<p>one<b/>two<i/>three</p>");
  REQUIRE(n);
  CHECK(n->text == "onetwothree");
  CHECK(n->children.size() == 2);
}

// ----------------------------------------------------------------- entities

TEST_CASE("xml: the five named entities", "[xml][entities]") {
  auto n = xml::parse("<p>&lt;&amp;&gt;&quot;&apos;</p>");
  REQUIRE(n);
  CHECK(n->text == "<&>\"'");
}

TEST_CASE("xml: numeric character references become UTF-8", "[xml][entities]") {
  SECTION("decimal and hex, ASCII") {
    auto n = xml::parse("<p>&#65;&#x42;&#X43;</p>");
    REQUIRE(n);
    CHECK(n->text == "ABC");
  }
  SECTION("2-byte sequence (e-acute)") {
    auto n = xml::parse("<p>&#233;</p>");
    REQUIRE(n);
    CHECK(n->text == "\xC3\xA9");
  }
  SECTION("3-byte sequence (euro sign)") {
    auto n = xml::parse("<p>&#x20AC;</p>");
    REQUIRE(n);
    CHECK(n->text == "\xE2\x82\xAC");
  }
  SECTION("4-byte sequence (emoji)") {
    auto n = xml::parse("<p>&#x1F600;</p>");
    REQUIRE(n);
    CHECK(n->text == "\xF0\x9F\x98\x80");
  }
}

TEST_CASE("xml: appendUtf8 encodes each length boundary", "[xml][entities]") {
  auto enc = [](unsigned long cp) {
    std::string s;
    xml::appendUtf8(s, cp);
    return s;
  };
  CHECK(enc(0x41) == "A");
  CHECK(enc(0x7F) == "\x7F");
  CHECK(enc(0x80) == "\xC2\x80");
  CHECK(enc(0x7FF) == "\xDF\xBF");
  CHECK(enc(0x800) == "\xE0\xA0\x80");
  CHECK(enc(0xFFFF) == "\xEF\xBF\xBF");
  CHECK(enc(0x10000) == "\xF0\x90\x80\x80");
}

TEST_CASE("xml: malformed entities are left untouched", "[xml][entities]") {
  CHECK(xml::decodeEntities("&nbsp;") == "&nbsp;");        // unknown name
  CHECK(xml::decodeEntities("a & b") == "a & b");          // bare ampersand
  CHECK(xml::decodeEntities("&amp") == "&amp");            // no semicolon
  CHECK(xml::decodeEntities("&#;") == "&#;");              // no digits
  CHECK(xml::decodeEntities("&#xZZ;") == "&#xZZ;");        // bad hex
  CHECK(xml::decodeEntities("&#12ab;") == "&#12ab;");      // trailing junk
  CHECK(xml::decodeEntities("&averyveryverylongname;") ==  // > 12 chars
        "&averyveryverylongname;");
}

TEST_CASE("xml: decoding does not recurse", "[xml][entities]") {
  // "&amp;lt;" must become the literal text "&lt;", not "<".
  CHECK(xml::decodeEntities("&amp;lt;") == "&lt;");
}

// -------------------------------------------------------------------- CDATA

TEST_CASE("xml: CDATA is taken raw, with no entity decoding", "[xml][cdata]") {
  auto n = xml::parse("<s><![CDATA[a < b && c &amp; d]]></s>");
  REQUIRE(n);
  CHECK(n->text == "a < b && c &amp; d");
}

TEST_CASE("xml: CDATA and plain text interleave in order", "[xml][cdata]") {
  auto n = xml::parse("<s>x<![CDATA[<y>]]>z&amp;</s>");
  REQUIRE(n);
  CHECK(n->text == "x<y>z&");
}

TEST_CASE("xml: an unterminated CDATA runs to the end without crashing",
          "[xml][cdata]") {
  std::optional<xml::Node> n;
  REQUIRE_NOTHROW(n = xml::parse("<s><![CDATA[never closed"));
  REQUIRE(n);
  CHECK(n->text == "never closed");
}

// ------------------------------------------------- comments, PIs, DOCTYPE

TEST_CASE("xml: comments are skipped", "[xml][misc]") {
  SECTION("before the root") {
    auto n = xml::parse("<!-- hi --><a/>");
    REQUIRE(n);
    CHECK(n->tag == "a");
  }
  SECTION("between children") {
    auto n = xml::parse("<a><!-- x --><b/><!-- y --></a>");
    REQUIRE(n);
    CHECK(n->children.size() == 1);
  }
  SECTION("markup inside a comment is not parsed") {
    auto n = xml::parse("<a><!-- <b/> --></a>");
    REQUIRE(n);
    CHECK(n->children.empty());
    CHECK(n->text.empty());
  }
}

TEST_CASE("xml: the XML declaration and other PIs are skipped", "[xml][misc]") {
  auto n = xml::parse(R"xml(<?xml version="1.0" encoding="UTF-8"?><a/>)xml");
  REQUIRE(n);
  CHECK(n->tag == "a");
}

TEST_CASE("xml: DOCTYPE is skipped, including an internal subset",
          "[xml][misc]") {
  SECTION("simple DOCTYPE") {
    auto n = xml::parse(
        R"xml(<!DOCTYPE svg PUBLIC "-//W3C//DTD SVG 1.1//EN" "x.dtd"><svg/>)xml");
    REQUIRE(n);
    CHECK(n->tag == "svg");
  }
  SECTION("internal subset containing '>' characters") {
    auto n = xml::parse(R"xml(<!DOCTYPE a [ <!ENTITY e "v"> <!ELEMENT a ANY> ]><a/>)xml");
    REQUIRE(n);
    CHECK(n->tag == "a");
  }
}

// ----------------------------------------------------------- bad input

TEST_CASE("xml: input with no element yields nullopt", "[xml][errors]") {
  CHECK_FALSE(xml::parse("").has_value());
  CHECK_FALSE(xml::parse("   \n ").has_value());
  CHECK_FALSE(xml::parse("just some text").has_value());
  CHECK_FALSE(xml::parse("<!-- only a comment -->").has_value());
  CHECK_FALSE(xml::parse("<?xml version=\"1.0\"?>").has_value());
}

TEST_CASE("xml: truncated input never crashes", "[xml][errors]") {
  // The parser is deliberately lenient: it returns whatever it managed to
  // read. What matters is that it terminates without reading out of bounds.
  const char *inputs[] = {
      "<",         "<a",           "<a ",         "<a x",       "<a x=",
      "<a x=\"",   "<a x=\"1",     "<a x=\"1\"",  "<a>",        "<a><b",
      "<a><b>",    "<a></",        "<a></a",      "<!--",       "<!-- x",
      "<?xml",     "<!DOCTYPE",    "<!DOCTYPE a [", "<a>&#",    "<a>&amp",
  };
  for (const char *in : inputs) {
    DYNAMIC_SECTION("input: " << in) {
      REQUIRE_NOTHROW(xml::parse(in));
    }
  }
}

TEST_CASE("xml: truncated element still reports what it read", "[xml][errors]") {
  auto n = xml::parse("<a><b>");
  REQUIRE(n);
  CHECK(n->tag == "a");
  REQUIRE(n->children.size() == 1);
  CHECK(n->children[0].tag == "b");
}

TEST_CASE("xml: moderately deep nesting parses", "[xml][limits]") {
  constexpr int depth = 200;
  std::string s;
  for (int i = 0; i < depth; ++i)
    s += "<g>";
  for (int i = 0; i < depth; ++i)
    s += "</g>";
  auto n = xml::parse(s);
  REQUIRE(n);
  int seen = 1;
  for (const xml::Node *p = &*n; !p->children.empty(); p = &p->children[0])
    ++seen;
  CHECK(seen == depth);
}
