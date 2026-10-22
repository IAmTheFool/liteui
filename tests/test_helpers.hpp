#pragma once
// Shared helpers for the liteui tests. Included once, from liteui_tests.cpp,
// AFTER liteui.hpp (so the liteui_* namespaces already exist).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace xml = liteui_xml;
namespace svg = liteui_svg;
namespace layout = liteui_layout;

// Float comparison with a tolerance that suits single-precision geometry.
inline Catch::Approx approx(double v) { return Catch::Approx(v).margin(1e-3); }

// Apply an SVG matrix to a point:  [x']   [a c e] [x]
//                                  [y'] = [b d f] [y]
inline std::pair<float, float> apply(const svg::Mat2x3 &m, float x, float y) {
  return {m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
}

// A fixed-size flex item, the building block for most layout tests.
inline View box(float w, float h) {
  View v;
  v.style.width = Size::pixel(w);
  v.style.height = Size::pixel(h);
  return v;
}

// A container with a fixed size.
inline View container(float w, float h) { return box(w, h); }

// Parse an SVG string and fail the test immediately if it doesn't parse.
inline svg::Document mustParse(const std::string &s) {
  auto doc = svg::parseString(s);
  REQUIRE(doc.has_value());
  return *doc;
}

// Writes `contents` to a unique file in the system temp directory and
// deletes it again when the object goes out of scope.
struct TempFile {
  std::filesystem::path path;
  explicit TempFile(const std::string &contents, const char *name) {
    path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path, std::ios::binary) << contents;
  }
  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};
