// tests/test_xml.cpp
//
// Unit tests for the liteui_xml namespace -- the small, tolerant XML
// parser that liteui_svg's own parsing is built on top of.
//
// NOTE: this assumes liteui.hpp lives at the project root next to
// CMakeLists.txt (matching its target_include_directories). If it's
// actually under an include/ subdirectory, change the #include below
// to "include/liteui.hpp" (or add that subdirectory to the include
// path in tests/CMakeLists.txt).

#include "liteui.hpp"

#include <catch2/catch_test_macros.hpp>

using liteui_xml::Node;
using liteui_xml::parse;

TEST_CASE("parses a bare self-closing element", "[xml]") {
  auto root = parse("<a/>");
  REQUIRE(root.has_value());
  CHECK(root->tag == "a");
  CHECK(root->children.empty());
  CHECK(root->text.empty());
}

TEST_CASE("parses a self-closing element with a space before the slash",
          "[xml]") {
  auto root = parse("<a />");
  REQUIRE(root.has_value());
  CHECK(root->tag == "a");
}

TEST_CASE("parses quoted attributes with both quote styles", "[xml]") {
  auto root = parse(R"(<rect x="1" y='2' fill="red"/>)");
  REQUIRE(root.has_value());
  CHECK(root->attrs.at("x") == "1");
  CHECK(root->attrs.at("y") == "2");
  CHECK(root->attrs.at("fill") == "red");
}

TEST_CASE("tolerates an unquoted attribute value", "[xml]") {
  auto root = parse("<a width=100/>");
  REQUIRE(root.has_value());
  CHECK(root->attrs.at("width") == "100");
}

TEST_CASE("parses nested children in document order", "[xml]") {
  auto root = parse("<g><rect/><circle/></g>");
  REQUIRE(root.has_value());
  CHECK(root->tag == "g");
  REQUIRE(root->children.size() == 2);
  CHECK(root->children[0].tag == "rect");
  CHECK(root->children[1].tag == "circle");
}

TEST_CASE("collects character data as text", "[xml]") {
  auto root = parse("<text>hello world</text>");
  REQUIRE(root.has_value());
  CHECK(root->text == "hello world");
}

TEST_CASE("CDATA sections are copied verbatim, without entity decoding",
          "[xml]") {
  auto root =
      parse("<style><![CDATA[.a { fill: none; } /* &amp; */]]></style>");
  REQUIRE(root.has_value());
  CHECK(root->text == ".a { fill: none; } /* &amp; */");
}

TEST_CASE("decodes the five predefined XML entities", "[xml]") {
  auto root = parse("<text>&amp;&lt;&gt;&quot;&apos;</text>");
  REQUIRE(root.has_value());
  CHECK(root->text == "&<>\"'");
}

TEST_CASE("decodes numeric character references, decimal and hex", "[xml]") {
  auto root = parse("<text>&#65;&#x42;</text>");
  REQUIRE(root.has_value());
  CHECK(root->text == "AB");
}

TEST_CASE("decodes entities inside attribute values too", "[xml]") {
  auto root = parse(R"(<a title="Tom &amp; Jerry"/>)");
  REQUIRE(root.has_value());
  CHECK(root->attrs.at("title") == "Tom & Jerry");
}

TEST_CASE("skips XML comments between and around elements", "[xml]") {
  auto root = parse("<!-- top --><a><!-- inner --><b/></a>");
  REQUIRE(root.has_value());
  CHECK(root->tag == "a");
  REQUIRE(root->children.size() == 1);
  CHECK(root->children[0].tag == "b");
}

TEST_CASE("skips processing instructions like the XML declaration", "[xml]") {
  auto root = parse(R"(<?xml version="1.0" encoding="UTF-8"?><svg/>)");
  REQUIRE(root.has_value());
  CHECK(root->tag == "svg");
}

TEST_CASE("skips a DOCTYPE with a bracketed internal subset", "[xml]") {
  auto root = parse("<!DOCTYPE svg [ <!ENTITY foo \"bar\"> ]><svg/>");
  REQUIRE(root.has_value());
  CHECK(root->tag == "svg");
}

TEST_CASE("mixed text and child elements are concatenated separately",
          "[xml]") {
  auto root = parse("<p>before<b/>after</p>");
  REQUIRE(root.has_value());
  CHECK(root->text == "beforeafter");
  REQUIRE(root->children.size() == 1);
  CHECK(root->children[0].tag == "b");
}

TEST_CASE("returns nullopt for input with no element at all", "[xml]") {
  auto root = parse("   just some text, no tags   ");
  CHECK_FALSE(root.has_value());
}

TEST_CASE("does not crash or hang on an unterminated element", "[xml]") {
  // No closing '>' at all -- the parser should bail out gracefully
  // rather than looping or reading out of bounds.
  auto root = parse("<a");
  REQUIRE(root.has_value());
  CHECK(root->tag == "a");
}