// sam_segmenter_test.cpp - the parts of sam_segmenter.hpp that need neither
// ONNX Runtime nor a model: the stand-in Segmenter of a build without
// LITEROOM_ONNX, and sam::preprocess (the resize / normalize / pad step that
// Segmenter::encode feeds the encoder). The real model path is not covered.
// cl /std:c++20 /EHsc /O2 /W3 /I. tests\sam_segmenter_test.cpp /Fe:tests\sam_segmenter_test.exe
// tests\sam_segmenter_test.exe
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "mask/sam_segmenter.hpp"

using Rgba = std::vector<uint8_t>;

static Rgba makeRgba(int w, int h, uint8_t (*f)(int x, int y, int c))
{
  Rgba img(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      for (int c = 0; c < 4; ++c)
        img[(static_cast<size_t>(y) * w + x) * 4 + c] = c == 3 ? 255 : f(x, y, c);
  return img;
}

static Rgba solid(int w, int h, int r, int g, int b, int a = 255)
{
  Rgba img(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < img.size(); i += 4)
  {
    img[i] = static_cast<uint8_t>(r);
    img[i + 1] = static_cast<uint8_t>(g);
    img[i + 2] = static_cast<uint8_t>(b);
    img[i + 3] = static_cast<uint8_t>(a);
  }
  return img;
}

// The same constants as Segmenter::encode (SAM's ImageNet mean / std, 0-255).
static float norm(int v, int c)
{
  static const float mean[3] = {123.675f, 116.28f, 103.53f};
  static const float stdv[3] = {58.395f, 57.12f, 57.375f};
  return (static_cast<float>(v) - mean[c]) / stdv[c];
}

// ------------------------------------------------ stand-in without ONNX
static void testStandIn()
{
#if !defined(LITEROOM_ONNX)
  sam::Segmenter s;
  CHECK(!s.loaded() && !s.encoded());

  std::string err;
  CHECK(!s.load("enc.onnx", "dec.onnx", &err));
  CHECK(err == "built without LITEROOM_ONNX");

  const Rgba img = solid(4, 4, 1, 2, 3);
  err.clear();
  CHECK(!s.encode(img.data(), 4, 4, &err) && !err.empty());
  CHECK(!s.encoded());

  std::vector<float> logits;
  int w = 7, h = 7;
  err.clear();
  CHECK(!s.decode({sam::Point{0.5f, 0.5f, true}}, &logits, &w, &h, &err));
  CHECK(!err.empty());

  // A null errorOut is fine everywhere.
  CHECK(!s.load("", "", nullptr));
  CHECK(!s.encode(img.data(), 4, 4, nullptr));
  CHECK(!s.decode({}, &logits, &w, &h, nullptr));
#endif
}

// ------------------------------------------------------------- sizes
static void testResizedSize()
{
  using sam::preprocess;
  const Rgba big = solid(2000, 1000, 0, 0, 0);
  auto p = preprocess(big.data(), 2000, 1000, 4);
  CHECK(p.ok() && p.nw == 1024 && p.nh == 512); // long side becomes 1024

  const Rgba tall = solid(1000, 2000, 0, 0, 0);
  p = preprocess(tall.data(), 1000, 2000, 4);
  CHECK(p.nw == 512 && p.nh == 1024);

  const Rgba sq = solid(1024, 1024, 0, 0, 0);
  p = preprocess(sq.data(), 1024, 1024, 4);
  CHECK(p.nw == 1024 && p.nh == 1024);

  const Rgba tiny = solid(3, 1, 0, 0, 0); // enlarged; 1 x 3 -> 1024 x 341
  p = preprocess(tiny.data(), 3, 1, 4);
  CHECK(p.nw == 1024 && p.nh == 341);

  const Rgba strip = solid(4000, 1, 0, 0, 0); // short side never rounds to 0
  p = preprocess(strip.data(), 4000, 1, 4);
  CHECK(p.nw == 1024 && p.nh == 1);

  const size_t plane = size_t{1024} * 1024;
  p = preprocess(big.data(), 2000, 1000, 4);
  CHECK(p.data.size() == 3 * plane); // rank 4: always the full padded square
  p = preprocess(big.data(), 2000, 1000, 3);
  CHECK(p.nw == 1024 && p.nh == 512);
  CHECK(p.data.size() == size_t{1024} * 512 * 3); // rank 3: just the resized picture
}

static void testInvalidInput()
{
  const Rgba img = solid(4, 4, 1, 2, 3);
  for (int rank : {3, 4})
  {
    auto p = sam::preprocess(nullptr, 4, 4, rank);
    CHECK(!p.ok() && p.data.empty() && p.nw == 0 && p.nh == 0);
    p = sam::preprocess(img.data(), 0, 4, rank);
    CHECK(!p.ok() && p.data.empty());
    p = sam::preprocess(img.data(), 4, 0, rank);
    CHECK(!p.ok() && p.data.empty());
    p = sam::preprocess(img.data(), -4, 4, rank);
    CHECK(!p.ok() && p.data.empty());
  }
}

// ----------------------------------------------- rank 4: NCHW, padded
static void testRank4NormalizeAndPad()
{
  const size_t plane = size_t{1024} * 1024;

  // Wide picture: rows past nh are padding, and padding is zero *after*
  // normalization (not the normalized value of black).
  const Rgba wide = solid(2000, 1000, 255, 128, 0);
  const auto p = sam::preprocess(wide.data(), 2000, 1000, 4);
  CHECK(p.nw == 1024 && p.nh == 512);
  const int want[3] = {255, 128, 0};
  for (int c = 0; c < 3; ++c)
  {
    CHECK_NEAR(p.data[c * plane + 0], norm(want[c], c), 1e-4);                    // top-left
    CHECK_NEAR(p.data[c * plane + 511 * 1024 + 1023], norm(want[c], c), 1e-4);     // last real pixel
    CHECK(p.data[c * plane + 512 * 1024] == 0.0f);                                 // first padded row
    CHECK(p.data[c * plane + 1023 * 1024 + 1023] == 0.0f);                         // bottom-right
  }

  // Tall picture: columns past nw are padding.
  const Rgba tall = solid(1000, 2000, 255, 128, 0);
  const auto q = sam::preprocess(tall.data(), 1000, 2000, 4);
  CHECK(q.nw == 512 && q.nh == 1024);
  for (int c = 0; c < 3; ++c)
  {
    CHECK_NEAR(q.data[c * plane + 511], norm(want[c], c), 1e-4);
    CHECK(q.data[c * plane + 512] == 0.0f);
    CHECK(q.data[c * plane + 1023 * 1024 + 1023] == 0.0f);
  }
}

// Channel planes in R, G, B order, and the formula (v - mean) / std per channel.
static void testRank4ChannelOrder()
{
  const Rgba img = solid(1024, 512, 10, 100, 200);
  const auto p = sam::preprocess(img.data(), 1024, 512, 4);
  const size_t plane = size_t{1024} * 1024;
  CHECK_NEAR(p.data[0 * plane + 5], norm(10, 0), 1e-4);
  CHECK_NEAR(p.data[1 * plane + 5], norm(100, 1), 1e-4);
  CHECK_NEAR(p.data[2 * plane + 5], norm(200, 2), 1e-4);
}

// --------------------------------------------------- rank 3: raw HWC
static void testRank3RawHwc()
{
  // Identity scale (1024 x 512 in, 1024 x 512 out): every output pixel is
  // exactly the source pixel, stored as R, G, B in row-major order.
  const Rgba img = makeRgba(1024, 512, [](int x, int y, int c) -> uint8_t
                            { return static_cast<uint8_t>(c == 1 ? x : c == 2 ? y : x + y); });
  const auto p = sam::preprocess(img.data(), 1024, 512, 3);
  CHECK(p.nw == 1024 && p.nh == 512);
  for (const auto &xy : {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}, std::pair{700, 300}, std::pair{1023, 511}})
  {
    const int x = xy.first, y = xy.second;
    const float *o = &p.data[(static_cast<size_t>(y) * 1024 + x) * 3];
    CHECK(o[0] == static_cast<float>(static_cast<uint8_t>(x + y))); // raw 0-255 R
    CHECK(o[1] == static_cast<float>(static_cast<uint8_t>(x)));     // G
    CHECK(o[2] == static_cast<float>(static_cast<uint8_t>(y)));     // B
  }
}

// ------------------------------------------------------ resampling
static void testBoxAverageWhenShrinking()
{
  // 2048 x 1024 -> 1024 x 512: each output pixel is the mean of a 2 x 2 block.
  // Columns alternate 0 / 100, so the mean is 50 everywhere.
  const Rgba img = makeRgba(2048, 1024, [](int x, int, int) -> uint8_t
                            { return x % 2 ? 100 : 0; });
  const auto p = sam::preprocess(img.data(), 2048, 1024, 3);
  CHECK(p.nw == 1024 && p.nh == 512);
  for (size_t i : {size_t{0}, size_t{1}, size_t{12345}, p.data.size() - 1})
    CHECK_NEAR(p.data[i], 50.0f, 1e-3);

  // A 4 x 4 block of 16 values averages exactly.
  const Rgba blocks = makeRgba(4096, 2048, [](int x, int y, int) -> uint8_t
                               { return static_cast<uint8_t>((x % 4) * 4 + (y % 4)); });
  const auto q = sam::preprocess(blocks.data(), 4096, 2048, 3);
  CHECK(q.nw == 1024 && q.nh == 512);
  CHECK_NEAR(q.data[0], 7.5f, 1e-3); // mean of 0..15
}

// A shrink factor that is not a whole number (1.5): boxes overlap the source
// pixels unevenly, and each box still reaches its last partly covered pixel.
static void testFractionalShrink()
{
  const Rgba img = makeRgba(1536, 768, [](int x, int, int) -> uint8_t
                            { return x % 2 ? 90 : 0; });
  const auto p = sam::preprocess(img.data(), 1536, 768, 3);
  CHECK(p.nw == 1024 && p.nh == 512);
  CHECK_NEAR(p.data[0], 45.0f, 1e-3);     // source columns 0, 1
  CHECK_NEAR(p.data[3], 45.0f, 1e-3);     // output column 1: source columns 1, 2
}

static void testNearestWhenEnlarging()
{
  // 2 x 1 (left black, right white) -> 1024 x 512: each half is one flat colour.
  Rgba img = solid(2, 1, 0, 0, 0);
  img[4] = img[5] = img[6] = 255;
  const auto p = sam::preprocess(img.data(), 2, 1, 3);
  CHECK(p.nw == 1024 && p.nh == 512);
  const float *left = &p.data[(static_cast<size_t>(10) * 1024 + 100) * 3];
  const float *right = &p.data[(static_cast<size_t>(10) * 1024 + 900) * 3];
  CHECK(left[0] == 0.0f && left[1] == 0.0f && left[2] == 0.0f);
  CHECK(right[0] == 255.0f && right[1] == 255.0f && right[2] == 255.0f);
}

static void testAlphaIgnored()
{
  const Rgba opaque = solid(64, 32, 30, 60, 90, 255);
  const Rgba clear = solid(64, 32, 30, 60, 90, 0);
  CHECK(sam::preprocess(opaque.data(), 64, 32, 4).data ==
        sam::preprocess(clear.data(), 64, 32, 4).data);
  CHECK(sam::preprocess(opaque.data(), 64, 32, 3).data ==
        sam::preprocess(clear.data(), 64, 32, 3).data);
}

int main()
{
  testStandIn();
  testResizedSize();
  testInvalidInput();
  testRank4NormalizeAndPad();
  testRank4ChannelOrder();
  testRank3RawHwc();
  testBoxAverageWhenShrinking();
  testFractionalShrink();
  testNearestWhenEnlarging();
  testAlphaIgnored();
  return finish();
}
