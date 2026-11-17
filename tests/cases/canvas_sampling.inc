// Tests for the image-sampling decision shared by every canvas backend
// (liteui_canvas_sampling::useNearest). Pure arithmetic, so it runs on every
// platform and needs no renderer. The per-backend plumbing (state save/restore,
// which filter reaches the renderer) is covered by canvas_gl_context.inc for
// the GL backend and by canvas_hardware.inc for the real ones.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

namespace {
using liteui_canvas_sampling::useNearest;
constexpr float kRight90[4] = {0, 1, -1, 0}; // a,b,c,d of a 90-degree rotation
} // namespace

TEST_CASE("ImageSampling: Smooth is the default and Smooth/Nearest ignore scale",
          "[canvas][sampling]") {
  CHECK(ImageSampling{} == ImageSampling::Smooth);
  for (float s : {0.01f, 0.5f, 1.0f, 8.0f}) {
    CHECK_FALSE(useNearest(ImageSampling::Smooth, s, 0, 0, s, 1, 1));
    CHECK(useNearest(ImageSampling::Nearest, s, 0, 0, s, 1, 1));
  }
  // Even a degenerate matrix: an explicit mode is honoured as asked.
  CHECK(useNearest(ImageSampling::Nearest, 0, 0, 0, 0, 0, 0));
  CHECK_FALSE(useNearest(ImageSampling::Smooth, 0, 0, 0, 0, 0, 0));
}

TEST_CASE("ImageSampling: Auto switches at one device pixel per source pixel",
          "[canvas][sampling]") {
  const auto autoAt = [](float scale) {
    return useNearest(ImageSampling::Auto, scale, 0, 0, scale, 1, 1);
  };
  CHECK(autoAt(8.0f));
  CHECK(autoAt(2.0f));
  CHECK(autoAt(1.0f));
  CHECK_FALSE(autoAt(0.99f));
  CHECK_FALSE(autoAt(0.5f));
  CHECK_FALSE(autoAt(0.01f));
  // Float noise right at the threshold must not flip the answer.
  CHECK(autoAt(0.9995f));
  CHECK(useNearest(ImageSampling::Auto, 0.8f, 0, 0, 0.8f, 1, 1, 1.25f));
}

TEST_CASE("ImageSampling: Auto combines transform, rect ratio and dpr",
          "[canvas][sampling]") {
  const auto a = ImageSampling::Auto;
  // A 2x2 source into 8x8 under a 0.25 scale is 1 device px per source px.
  CHECK(useNearest(a, 0.25f, 0, 0, 0.25f, 4, 4));
  CHECK_FALSE(useNearest(a, 0.2f, 0, 0, 0.2f, 4, 4));
  // dpr multiplies in when the matrix does not already contain it ...
  CHECK(useNearest(a, 1, 0, 0, 1, 0.5f, 0.5f, 2.0f));
  CHECK_FALSE(useNearest(a, 1, 0, 0, 1, 0.5f, 0.5f, 1.0f));
  // ... and a matrix that already has it (the GL backend) passes dpr = 1.
  CHECK_FALSE(useNearest(a, 2, 0, 0, 2, 0.25f, 0.25f)); // 0.5 device px
  // A flipped destination (negative size) scales the same as an unflipped one.
  CHECK(useNearest(a, 1, 0, 0, 1, -4, 4));
}

TEST_CASE("ImageSampling: Auto uses the smaller axis and ignores rotation",
          "[canvas][sampling]") {
  const auto a = ImageSampling::Auto;
  CHECK(useNearest(a, 4, 0, 0, 4, 1, 1));
  CHECK_FALSE(useNearest(a, 4, 0, 0, 0.5f, 1, 1)); // squeezed in y
  CHECK_FALSE(useNearest(a, 0.5f, 0, 0, 4, 1, 1)); // squeezed in x
  CHECK(useNearest(a, kRight90[0] * 4, kRight90[1] * 4, kRight90[2] * 4,
                   kRight90[3] * 4, 1, 1));
  CHECK_FALSE(useNearest(a, kRight90[0] * 0.5f, kRight90[1] * 0.5f,
                         kRight90[2] * 0.5f, kRight90[3] * 0.5f, 1, 1));
  // A skew lengthens the transformed axes: |(1,1)| = sqrt(2) >= 1.
  CHECK(useNearest(a, 1, 1, 0, 1, 1, 1));
}

TEST_CASE("ImageSampling: Auto falls back to Smooth on degenerate input",
          "[canvas][sampling]") {
  const auto a = ImageSampling::Auto;
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(useNearest(a, 0, 0, 0, 0, 1, 1)); // collapsed matrix
  CHECK_FALSE(useNearest(a, 1, 0, 0, 1, 0, 1)); // empty destination
  CHECK_FALSE(useNearest(a, nan, 0, 0, 1, 1, 1));
  CHECK_FALSE(useNearest(a, 1, 0, 0, 1, inf, 1));
  CHECK_FALSE(useNearest(a, 1, 0, 0, 1, 1, 1, nan));
}

TEST_CASE("ImageSampling: pickFilter adds the minified case", "[canvas][sampling]") {
  using liteui_canvas_sampling::ImageFilter;
  using liteui_canvas_sampling::pickFilter;
  const auto at = [](ImageSampling m, float scale, float dpr = 1.0f) {
    return pickFilter(m, scale, 0, 0, scale, 1, 1, dpr);
  };
  // Nearest is always Nearest.
  CHECK(at(ImageSampling::Nearest, 0.1f) == ImageFilter::Nearest);
  CHECK(at(ImageSampling::Nearest, 8.0f) == ImageFilter::Nearest);
  // Smooth: Linear from 1:1 up, Mipmap below.
  CHECK(at(ImageSampling::Smooth, 8.0f) == ImageFilter::Linear);
  CHECK(at(ImageSampling::Smooth, 1.0f) == ImageFilter::Linear);
  CHECK(at(ImageSampling::Smooth, 0.9995f) == ImageFilter::Linear); // noise
  CHECK(at(ImageSampling::Smooth, 0.99f) == ImageFilter::Mipmap);
  CHECK(at(ImageSampling::Smooth, 0.01f) == ImageFilter::Mipmap);
  // Auto: Nearest from 1:1 up, Mipmap below (never Linear).
  CHECK(at(ImageSampling::Auto, 2.0f) == ImageFilter::Nearest);
  CHECK(at(ImageSampling::Auto, 1.0f) == ImageFilter::Nearest);
  CHECK(at(ImageSampling::Auto, 0.5f) == ImageFilter::Mipmap);
  // The smaller axis decides, and dpr counts.
  CHECK(pickFilter(ImageSampling::Smooth, 4, 0, 0, 0.5f, 1, 1) == ImageFilter::Mipmap);
  CHECK(pickFilter(ImageSampling::Smooth, 1, 0, 0, 1, 0.5f, 0.5f, 2.0f) == ImageFilter::Linear);
  // Degenerate input is plain Linear for the smooth modes.
  CHECK(pickFilter(ImageSampling::Smooth, 0, 0, 0, 0, 1, 1) == ImageFilter::Linear);
  CHECK(pickFilter(ImageSampling::Auto, 1, 0, 0, 1, 0, 1) == ImageFilter::Linear);
}
