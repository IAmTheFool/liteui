// Tests for the DIB converters behind the Windows clipboard image path:
// buildDibV5 (what setImage writes as CF_DIBV5) and parseDib (what getImage
// reads from CF_DIBV5 / CF_DIB). Pure byte-level code, so it runs on every
// platform without a clipboard.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

namespace clipboard_test {


TEST_CASE("clipboard dib: buildDibV5 writes a bottom-up 32-bit BGRA bitmap with a V5 header",
          "[clipboard][dib]") {
  const CanvasImage img = makeImage(7, 5, 2);
  Bytes dib;
  REQUIRE(cd::buildDibV5(img, dib));
  REQUIRE(dib.size() == 124 + 7 * 5 * 4);

  CHECK(cd::dibGet32(dib.data() + 0) == 124);         // bV5Size
  CHECK(cd::dibGet32(dib.data() + 4) == 7);           // width
  CHECK(cd::dibGet32(dib.data() + 8) == 5);           // positive height: bottom-up
  CHECK(cd::dibGet16(dib.data() + 12) == 1);          // planes
  CHECK(cd::dibGet16(dib.data() + 14) == 32);         // bit count
  CHECK(cd::dibGet32(dib.data() + 16) == 3);          // BI_BITFIELDS
  CHECK(cd::dibGet32(dib.data() + 40) == 0x00FF0000u); // R
  CHECK(cd::dibGet32(dib.data() + 44) == 0x0000FF00u); // G
  CHECK(cd::dibGet32(dib.data() + 48) == 0x000000FFu); // B
  CHECK(cd::dibGet32(dib.data() + 52) == 0xFF000000u); // A
  CHECK(cd::dibGet32(dib.data() + 56) == 0x73524742u); // LCS_sRGB

  // The first pixel stored is the LAST image row's first pixel, as B,G,R,A.
  const uint8_t *last = img.pixels.data() + (5 - 1) * 7 * 4;
  CHECK(dib[124 + 0] == last[2]);
  CHECK(dib[124 + 1] == last[1]);
  CHECK(dib[124 + 2] == last[0]);
  CHECK(dib[124 + 3] == last[3]);
}

TEST_CASE("clipboard dib: build then parse round-trips every pixel including alpha",
          "[clipboard][dib]") {
  const std::vector<std::pair<int, int>> sizes = {{1, 1}, {7, 5}, {64, 3}, {3, 64}};
  for (const auto &[w, h] : sizes) {
    CAPTURE(w, h);
    const CanvasImage img = makeImage(w, h, 5);
    Bytes dib;
    REQUIRE(cd::buildDibV5(img, dib));
    CanvasImage back;
    REQUIRE(cd::parseDib(dib.data(), dib.size(), back));
    CHECK(same(back, img));
  }
}

TEST_CASE("clipboard dib: 24-bit bottom-up with row padding", "[clipboard][dib]") {
  // Width 3 => 9 pixel bytes padded to a 12-byte stride. File order: bottom row first.
  Bytes v = dibHeader(40, 3, 2, 24, 0);
  add(v, {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0});          // bottom row (BGR x3)
  add(v, {10, 20, 30, 40, 50, 60, 70, 80, 90, 0, 0, 0}); // top row
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  REQUIRE(o.width == 3);
  REQUIRE(o.height == 2);
  // top-left pixel comes from the FILE'S LAST row, BGR -> RGB, opaque
  CHECK(o.pixels[0] == 30);
  CHECK(o.pixels[1] == 20);
  CHECK(o.pixels[2] == 10);
  CHECK(o.pixels[3] == 255);
  // bottom-left comes from the file's first row
  CHECK(o.pixels[12] == 3);
  CHECK(o.pixels[13] == 2);
  CHECK(o.pixels[14] == 1);
}

TEST_CASE("clipboard dib: 32-bit BI_RGB (what CF_DIB carries) has no usable alpha",
          "[clipboard][dib]") {
  // The 4th byte of BI_RGB pixels is unused and often garbage or 0: opaque.
  Bytes v = dibHeader(40, 2, 1, 32, 0);
  add(v, {10, 20, 30, 0, 40, 50, 60, 7});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 30);
  CHECK(o.pixels[3] == 255);
  CHECK(o.pixels[7] == 255);
}

TEST_CASE("clipboard dib: negative height means top-down", "[clipboard][dib]") {
  Bytes v = dibHeader(40, 1, -2, 32, 0);
  add(v, {1, 2, 3, 0, 4, 5, 6, 0});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 3); // first file row is the top row
  CHECK(o.pixels[4] == 6);
}

TEST_CASE("clipboard dib: BI_BITFIELDS with three masks after a 40-byte header",
          "[clipboard][dib]") {
  // What Windows itself writes when it synthesizes CF_DIB from a 32-bit bitmap.
  Bytes v = dibHeader(40, 1, 1, 32, 3);
  add32(v, 0x00FF0000);
  add32(v, 0x0000FF00);
  add32(v, 0x000000FF);
  add(v, {30, 20, 10, 99});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 10);
  CHECK(o.pixels[1] == 20);
  CHECK(o.pixels[2] == 30);
  CHECK(o.pixels[3] == 255); // no alpha mask: opaque
}

TEST_CASE("clipboard dib: BI_ALPHABITFIELDS reads alpha through the fourth mask",
          "[clipboard][dib]") {
  Bytes v = dibHeader(40, 1, 1, 32, 6);
  add32(v, 0x00FF0000);
  add32(v, 0x0000FF00);
  add32(v, 0x000000FF);
  add32(v, 0xFF000000);
  add(v, {30, 20, 10, 128});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 10);
  CHECK(o.pixels[3] == 128);
}

TEST_CASE("clipboard dib: channels that are not 8 bits wide are rescaled", "[clipboard][dib]") {
  // 10-10-10-2: 1023 -> 255, 512 -> 128, alpha 3 (of 3) -> 255
  Bytes v = dibHeader(124, 1, 1, 32, 3);
  setMasks(v, 0x3FF00000, 0x000FFC00, 0x000003FF, 0xC0000000);
  add32(v, (3u << 30) | (1023u << 20) | (0u << 10) | 512u);
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 255);
  CHECK(o.pixels[1] == 0);
  CHECK(o.pixels[2] == 128);
  CHECK(o.pixels[3] == 255);
}

TEST_CASE("clipboard dib: alpha that is zero everywhere means 'not filled in' - mixed alpha is kept",
          "[clipboard][dib]") {
  auto make = [](std::initializer_list<uint8_t> px) {
    Bytes v = dibHeader(124, 2, 1, 32, 3);
    setMasks(v, 0x00FF0000, 0x0000FF00, 0xFF, 0xFF000000);
    add(v, px);
    return v;
  };
  SECTION("all zero => opaque") {
    Bytes v = make({1, 2, 3, 0, 4, 5, 6, 0});
    CanvasImage o;
    REQUIRE(cd::parseDib(v.data(), v.size(), o));
    CHECK(o.pixels[3] == 255);
    CHECK(o.pixels[7] == 255);
  }
  SECTION("a genuinely transparent pixel next to an opaque one stays transparent") {
    Bytes v = make({1, 2, 3, 0, 4, 5, 6, 255});
    CanvasImage o;
    REQUIRE(cd::parseDib(v.data(), v.size(), o));
    CHECK(o.pixels[3] == 0);
    CHECK(o.pixels[7] == 255);
  }
}

TEST_CASE("clipboard dib: a V5 header with an alpha mask uses it even under BI_RGB",
          "[clipboard][dib]") {
  Bytes v = dibHeader(124, 1, 1, 32, 0);
  cd::dibPut32(v.data() + 52, 0xFF000000);
  add(v, {1, 2, 3, 77});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[3] == 77);
}

TEST_CASE("clipboard dib: an optional colour table before the pixels is skipped",
          "[clipboard][dib]") {
  Bytes v = dibHeader(40, 1, 1, 24, 0, /*clrUsed=*/2);
  v.insert(v.end(), 8, 0xEE); // two RGBQUADs
  add(v, {9, 8, 7, 0});
  CanvasImage o;
  REQUIRE(cd::parseDib(v.data(), v.size(), o));
  CHECK(o.pixels[0] == 7);
  CHECK(o.pixels[2] == 9);
}

TEST_CASE("clipboard dib: malformed or unsupported bitmaps are refused - never read out of bounds",
          "[clipboard][dib]") {
  CanvasImage o;
  SECTION("truncated pixel data") {
    Bytes v = dibHeader(40, 2, 2, 24, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("palettised 8-bit") {
    Bytes v = dibHeader(40, 1, 1, 8, 0);
    v.insert(v.end(), 8, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("16-bit") {
    Bytes v = dibHeader(40, 1, 1, 16, 0);
    v.insert(v.end(), 8, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("RLE-compressed") {
    Bytes v = dibHeader(40, 1, 1, 24, 1);
    v.insert(v.end(), 8, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("dimensions whose pixel count would overflow or exhaust memory") {
    Bytes v = dibHeader(40, 2000000000, 2000000000, 32, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("zero width") {
    Bytes v = dibHeader(40, 0, 1, 32, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("an absurd colour-table size") {
    Bytes v = dibHeader(40, 1, 1, 24, 0, 4000000000u);
    v.insert(v.end(), 8, 0);
    CHECK_FALSE(cd::parseDib(v.data(), v.size(), o));
  }
  SECTION("buffers too small to hold a header") {
    const uint8_t tiny[10] = {0};
    CHECK_FALSE(cd::parseDib(tiny, sizeof(tiny), o));
    CHECK_FALSE(cd::parseDib(nullptr, 0, o));
  }
  SECTION("buildDibV5 refuses an empty or inconsistent image") {
    Bytes out;
    CHECK_FALSE(cd::buildDibV5(CanvasImage(), out));
    CanvasImage bad(10, 10);
    bad.pixels.pop_back();
    CHECK_FALSE(cd::buildDibV5(bad, out));
  }
}

} // namespace clipboard_test
