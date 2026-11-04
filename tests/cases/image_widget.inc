// Tests for displaying an ImageBuffer through the Image widget
// (Image::hdrSource) and liteui_image::toCanvasImage. Painting itself needs a
// real backend, so these check what the widget resolves and reports.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#include <limits>

namespace {
std::shared_ptr<ImageBuffer> sharedBuf(uint32_t w, uint32_t h, PixelFormat f) {
  auto b = ImageBuffer::create(w, h, f);
  REQUIRE(b.has_value());
  return std::make_shared<ImageBuffer>(std::move(*b));
}
// addChild copies the flattened View into the parent; return that child.
View &addImage(View &parent, Image img) {
  parent.addChild(std::move(img));
  return parent.children.back();
}
} // namespace

TEST_CASE("toCanvasImage: RGBA32F is sRGB-encoded and clamped", "[image][hdr]") {
  auto b = sharedBuf(3, 1, PixelFormat::RGBA32_FLOAT);
  const float px[3][4] = {{0.5f, 0, 1, 0.5f}, {4.0f, -1.0f, 0, 1}, {0, 0, 0, 0}};
  for (uint32_t x = 0; x < 3; ++x) b->setPixel(x, 0, px[x]);

  auto img = liteui_image::toCanvasImage(*b);
  REQUIRE(img.has_value());
  CHECK(img->width == 3);
  CHECK(img->height == 1);
  REQUIRE(img->pixels.size() == 12);
  CHECK(img->pixels[0] == 188); // linear 0.5 -> sRGB 188
  CHECK(img->pixels[2] == 255);
  CHECK(img->pixels[3] == 128); // alpha only quantised
  CHECK(img->pixels[4] == 255); // HDR 4.0 clamps
  CHECK(img->pixels[5] == 0);   // negative clamps
}

TEST_CASE("toCanvasImage: RGBA8 passes through, padding dropped", "[image][hdr]") {
  auto b = ImageBuffer::create(2, 2, PixelFormat::RGBA8_UNORM, 12); // padded
  REQUIRE(b.has_value());
  const float px[4] = {1.0f, 0.5f, 0.0f, 1.0f};
  b->setPixel(1, 1, px);
  auto img = liteui_image::toCanvasImage(*b);
  REQUIRE(img.has_value());
  CHECK(img->pixels.size() == 16);
  CHECK(img->pixels[(1 * 2 + 1) * 4 + 0] == 255);
  CHECK(img->pixels[(1 * 2 + 1) * 4 + 1] == 128); // no sRGB change for 8-bit
}

TEST_CASE("toCanvasImage: invalid and empty buffers", "[image][hdr]") {
  auto b = sharedBuf(2, 2, PixelFormat::RGBA8_UNORM);
  b->pixels.pop_back();
  CHECK_FALSE(liteui_image::toCanvasImage(*b).has_value());
  auto e = liteui_image::toCanvasImage(ImageBuffer{});
  REQUIRE(e.has_value());
  CHECK(e->width == 0);
  CHECK(e->pixels.empty());
}

TEST_CASE("Image: fixed hdrSource sizes the node and fires onLoad", "[image][hdr]") {
  Image im;
  im.hdrSource = sharedBuf(6, 4, PixelFormat::RGBA32_FLOAT);
  int lw = 0, lh = 0, loads = 0, errors = 0;
  im.onLoad = [&](int w, int h) { lw = w; lh = h; ++loads; };
  im.onError = [&](const std::string &) { ++errors; };

  View parent;
  View &child = addImage(parent, std::move(im));
  CHECK(loads == 1);
  CHECK(errors == 0);
  CHECK(lw == 6);
  CHECK(lh == 4);
  REQUIRE(std::get_if<Size>(&child.style.width));
  CHECK(std::get<Size>(child.style.width).value == 6);
  CHECK(std::get<Size>(child.style.height).value == 4);
  REQUIRE(child.canvasDirtySource);
  CHECK(child.canvasDirtySource());       // first paint is always due
  CHECK_FALSE(child.canvasDirtySource()); // and then nothing changes
}

TEST_CASE("Image: dynamic hdrSource swaps by pointer", "[image][hdr]") {
  auto a = sharedBuf(2, 2, PixelFormat::RGBA32_FLOAT);
  auto b = sharedBuf(5, 3, PixelFormat::RGBA32_FLOAT);
  auto current = a;
  int loads = 0, lastW = 0;

  Image im;
  im.hdrSource = std::function<std::shared_ptr<ImageBuffer>()>([&] { return current; });
  im.onLoad = [&](int w, int) { ++loads; lastW = w; };

  View parent;
  View &child = addImage(parent, std::move(im));
  CHECK(loads == 1);
  CHECK(lastW == 2);
  CHECK(child.canvasDirtySource());       // initial paint
  CHECK_FALSE(child.canvasDirtySource()); // same pointer: no repaint

  current = b;
  CHECK(child.canvasDirtySource());       // new pointer: repaint
  CHECK(loads == 2);
  CHECK(lastW == 5);
  CHECK_FALSE(child.canvasDirtySource());

  // Fit-sized nodes re-poll their size from the new image.
  auto *w = std::get_if<std::function<Size()>>(&child.style.width);
  REQUIRE(w != nullptr);
  CHECK((*w)().value == 5);
}

TEST_CASE("Image: bad hdrSource reports an error and paints nothing", "[image][hdr]") {
  auto bad = sharedBuf(2, 2, PixelFormat::RGBA8_UNORM);
  bad->pixels.pop_back(); // inconsistent buffer
  Image im;
  im.hdrSource = bad;
  int loads = 0, errors = 0;
  im.onLoad = [&](int, int) { ++loads; };
  im.onError = [&](const std::string &) { ++errors; };
  View parent;
  addImage(parent, std::move(im));
  CHECK(loads == 0);
  CHECK(errors == 1);

  // A dynamic source that returns null later reports it too.
  auto good = sharedBuf(2, 2, PixelFormat::RGBA32_FLOAT);
  auto current = good;
  int errs2 = 0;
  Image dyn;
  dyn.hdrSource = std::function<std::shared_ptr<ImageBuffer>()>([&] { return current; });
  dyn.onError = [&](const std::string &) { ++errs2; };
  View parent2;
  View &c2 = addImage(parent2, std::move(dyn));
  c2.canvasDirtySource();
  current = nullptr;
  CHECK(c2.canvasDirtySource());
  CHECK(errs2 == 1);
}

TEST_CASE("Image: displaySettings callback rebuilds without a new buffer", "[image][hdr]") {
  auto buf = sharedBuf(2, 2, PixelFormat::RGBA32_FLOAT);
  DisplaySettings cur;
  int loads = 0, errors = 0;

  Image im;
  im.hdrSource = buf;
  im.displaySettings = std::function<DisplaySettings()>([&] { return cur; });
  im.onLoad = [&](int, int) { ++loads; };
  im.onError = [&](const std::string &) { ++errors; };

  View parent;
  View &child = addImage(parent, std::move(im));
  CHECK(loads == 1);
  CHECK(child.canvasDirtySource());       // first paint
  CHECK_FALSE(child.canvasDirtySource()); // settings unchanged

  cur.exposureEv = 1.5f;                  // e.g. the exposure slider moved
  CHECK(child.canvasDirtySource());
  CHECK_FALSE(child.canvasDirtySource());
  CHECK(loads == 1); // same image: onLoad is not repeated
  CHECK(errors == 0);

  cur.clamp = ClampMode::PreserveHue;
  CHECK(child.canvasDirtySource());
}

TEST_CASE("Image: displaySettings is ignored for non-hdr sources", "[image][hdr]") {
  auto px = std::make_shared<CanvasImage>(2, 2);
  Image im;
  im.source = px;
  im.displaySettings = std::function<DisplaySettings()>([] { return DisplaySettings{}; });
  View parent;
  View &child = addImage(parent, std::move(im));
  CHECK(child.canvasDirtySource());       // first paint
  CHECK_FALSE(child.canvasDirtySource()); // the callback must not retrigger it
}
