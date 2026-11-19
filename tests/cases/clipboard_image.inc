// Tests for the platform-independent half of the image clipboard: PNG
// encoding for the clipboard with the REAL codec (WIC on Windows, libpng on
// Linux), the PNG signature check, and rejection of malformed images.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

namespace clipboard_test {


TEST_CASE("clipboard image: PNG encoding for the clipboard decodes back to the same pixels",
          "[clipboard][image]") {
  const std::vector<CanvasImage> images = {
      makeImage(1, 1, 1),         makeImage(37, 23, 2),
      makeImage(64, 64, 3, true), makeImage(5, 200, 4),
      makeNoiseImage(50, 40, 9),
  };
  for (const CanvasImage &img : images) {
    CAPTURE(img.width, img.height);
    Bytes png;
    REQUIRE(cd::encodeClipboardPng(img, png));
    CHECK(cd::hasPngSignature(png));
    const auto back = liteui_image::decodeMemory(png.data(), png.size());
    REQUIRE(back);
    CHECK(same(*back, img));
  }
}

TEST_CASE("clipboard image: PNG encoding refuses images that are empty or inconsistent",
          "[clipboard][image]") {
  Bytes png{1, 2, 3};
  CHECK_FALSE(cd::encodeClipboardPng(CanvasImage(), png));
  CHECK(png.empty()); // cleared, not left holding stale bytes
  CanvasImage bad(8, 8);
  bad.pixels.resize(bad.pixels.size() - 1);
  CHECK_FALSE(cd::encodeClipboardPng(bad, png));
  CanvasImage neg;
  neg.width = -4;
  neg.height = 4;
  CHECK_FALSE(cd::encodeClipboardPng(neg, png));
  CHECK_FALSE(cd::validCanvasImage(neg));
  CHECK(cd::validCanvasImage(makeImage(2, 2)));
}

TEST_CASE("clipboard image: PNG signature check", "[clipboard][image]") {
  CHECK(cd::hasPngSignature(fakePng(8, 1)));
  CHECK_FALSE(cd::hasPngSignature({}));
  CHECK_FALSE(cd::hasPngSignature({0x89, 'P', 'N', 'G'}));                 // too short
  CHECK_FALSE(cd::hasPngSignature({'G', 'I', 'F', '8', '9', 'a', 0, 0}));  // other format
  Bytes almost = fakePng(16, 1);
  almost[7] ^= 1;
  CHECK_FALSE(cd::hasPngSignature(almost));
}

} // namespace clipboard_test
