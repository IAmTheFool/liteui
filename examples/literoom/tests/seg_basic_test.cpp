// Build: g++ -std=c++20 -I.. -I<folder with liteui.hpp> seg_basic_test.cpp -o seg_basic_test
// seg_basic.hpp takes liteui's CanvasImage, so in CMake this test links liteui.
#include "mask/seg_basic.hpp"
#include "check.hpp"

#include <atomic>

// Every test uses its own image key: segmentObjectWand keeps ONE prepared image
// per process, keyed by the caller's number.
static uint64_t gKey = 1000;

static CanvasImage solid(int w, int h, int r, int g, int b)
{
  CanvasImage im(w, h);
  for (size_t i = 0; i < im.pixels.size(); i += 4)
  {
    im.pixels[i] = static_cast<uint8_t>(r);
    im.pixels[i + 1] = static_cast<uint8_t>(g);
    im.pixels[i + 2] = static_cast<uint8_t>(b);
    im.pixels[i + 3] = 255;
  }
  return im;
}
static void paint(CanvasImage &im, int x0, int y0, int x1, int y1, int r, int g, int b)
{
  for (int y = y0; y < y1; ++y)
    for (int x = x0; x < x1; ++x)
    {
      uint8_t *p = &im.pixels[(static_cast<size_t>(y) * im.width + x) * 4];
      p[0] = static_cast<uint8_t>(r);
      p[1] = static_cast<uint8_t>(g);
      p[2] = static_cast<uint8_t>(b);
    }
}
static SegPrepared prepared(const CanvasImage &im) { return *prepareSegImage(im); }
static size_t count(const std::vector<uint8_t> &v)
{
  size_t n = 0;
  for (uint8_t b : v)
    n += b != 0;
  return n;
}

static void testPrepare()
{
  // Small pictures are copied as they are (alpha dropped).
  CanvasImage a = solid(4, 3, 10, 20, 30);
  a.pixels[3] = 7;
  const SegPrepared pa = prepared(a);
  CHECK(pa.w == 4 && pa.h == 3 && pa.rgb.size() == 4 * 3 * 3);
  CHECK(pa.rgb[0] == 10 && pa.rgb[1] == 20 && pa.rgb[2] == 30);
  // Big ones are box-averaged by the smallest whole factor that gets the long side to 512.
  CanvasImage big = solid(1000, 500, 0, 0, 0); // factor 2 -> 500 x 250
  paint(big, 0, 0, 1, 1, 100, 0, 0);          // one pixel of a 2 x 2 block
  paint(big, 2, 0, 3, 1, 10, 20, 30);
  paint(big, 3, 1, 4, 2, 11, 21, 31);
  const SegPrepared pb = prepared(big);
  CHECK(pb.w == 500 && pb.h == 250);
  CHECK(pb.rgb[0] == 25); // (100 + 0 + 0 + 0 + 2) / 4
  CHECK(pb.rgb[(0 * 500 + 1) * 3] == 5 && pb.rgb[(0 * 500 + 1) * 3 + 1] == 10); // block {2..3, 0..1}: (10+11+2)/4, (20+21+2)/4
  // Odd sizes never produce an empty image.
  const SegPrepared thin = prepared(solid(1001, 3, 1, 2, 3));
  CHECK(thin.w == 500 && thin.h == 1 && !thin.rgb.empty());
  CHECK(prepared(solid(512, 512, 0, 0, 0)).w == 512); // exactly the limit: no reduction
  CHECK(prepared(solid(513, 100, 0, 0, 0)).w == 256); // one over: factor 2
}

static void testGrowTolerance()
{
  // Three columns of 10: the seed's color, +40 in red, +41 in red.
  CanvasImage im = solid(30, 8, 100, 100, 100);
  paint(im, 10, 0, 20, 8, 140, 100, 100); // distance exactly kSegTolerance
  paint(im, 20, 0, 30, 8, 141, 100, 100); // one more: out
  const SegPrepared p = prepared(im);
  std::vector<uint8_t> mark(static_cast<size_t>(p.w) * p.h, 0);
  CHECK(segGrow(p, 2, 4, mark, nullptr, 0));
  CHECK(count(mark) == 20u * 8u);
  for (int y = 0; y < 8; ++y)
  {
    CHECK(mark[static_cast<size_t>(y) * 30 + 19] == 1);
    CHECK(mark[static_cast<size_t>(y) * 30 + 20] == 0);
  }
  CHECK(kSegTolerance == 40);
}

static void testGrowShape()
{
  // The wall of a different color stops it; a region behind a wall stays untouched.
  CanvasImage im = solid(20, 10, 50, 50, 50);
  paint(im, 10, 0, 11, 10, 250, 250, 250);
  const SegPrepared p = prepared(im);
  std::vector<uint8_t> mark(static_cast<size_t>(p.w) * p.h, 0);
  segGrow(p, 3, 5, mark, nullptr, 0);
  CHECK(count(mark) == 10u * 10u);
  CHECK(mark[5 * 20 + 9] == 1 && mark[5 * 20 + 10] == 0 && mark[5 * 20 + 11] == 0);
  // Pixels join through an edge, not a corner (4-connectivity): two black squares
  // that only touch at a corner are separate objects.
  CanvasImage diag = solid(12, 12, 255, 255, 255);
  paint(diag, 0, 0, 6, 6, 0, 0, 0);
  paint(diag, 6, 6, 12, 12, 0, 0, 0);
  const SegPrepared dp = prepared(diag);
  std::vector<uint8_t> m2(144, 0);
  segGrow(dp, 2, 2, m2, nullptr, 0);
  CHECK(count(m2) == 36u && m2[2 * 12 + 2] == 1 && m2[8 * 12 + 8] == 0);
  // (segGrow expects a seed inside the picture; segmentObjectWand clamps its prompts first.)
}

static void testGrowCancel()
{
  const CanvasImage im = solid(200, 200, 90, 90, 90); // 40000 pixels: well past the first check
  const SegPrepared p = prepared(im);
  std::vector<uint8_t> mark(static_cast<size_t>(p.w) * p.h, 0);
  std::atomic<uint64_t> latest{7};
  CHECK(segGrow(p, 100, 100, mark, &latest, 7));      // still the current request
  CHECK(count(mark) == 40000u);
  std::vector<uint8_t> m2(mark.size(), 0);
  CHECK(!segGrow(p, 100, 100, m2, &latest, 6));       // superseded: gives up
  CHECK(count(m2) < 40000u);
  std::vector<uint8_t> m3(mark.size(), 0);
  CHECK(segGrow(p, 100, 100, m3, nullptr, 0));        // no cancel token: always completes
}

static void testWand()
{
  CanvasImage im = solid(60, 40, 200, 60, 60);        // left: red
  paint(im, 30, 0, 60, 40, 40, 60, 200);              // right: blue
  paint(im, 5, 5, 12, 12, 30, 30, 30);                // a dark spot inside the red half
  const uint64_t key = ++gKey;
  std::string err;
  auto m = segmentObjectWand(im, key, {ObjectPrompt{0.2f, 0.5f, true}}, &err, nullptr, 0);
  CHECK(m && m->w == 60 && m->h == 40 && m->a.size() == 60u * 40u);
  if (!m)
    return;
  CHECK(m->id != 0);
  auto cov = [&](const ObjectMap &mm, int x, int y) { return mm.a[static_cast<size_t>(y) * mm.w + x]; };
  CHECK(cov(*m, 20, 20) == 255 && cov(*m, 45, 20) == 0);
  CHECK(cov(*m, 8, 8) == 0);                        // the dark spot is a hole
  for (uint8_t v : m->a)
    CHECK(v == 0 || v == 255);                      // a hard map
  // Two results never share an id.
  auto m2 = segmentObjectWand(im, key, {ObjectPrompt{0.2f, 0.5f, true}}, &err, nullptr, 0);
  CHECK(m2 && m2->id != m->id && m2->a == m->a);
  // Foreground points add, background points subtract.
  auto both = segmentObjectWand(im, key, {ObjectPrompt{0.2f, 0.5f, true}, ObjectPrompt{0.8f, 0.5f, true}}, &err, nullptr, 0);
  CHECK(both && count(both->a) == 60u * 40u - 7u * 7u);
  auto minus = segmentObjectWand(im, key, {ObjectPrompt{0.2f, 0.5f, true}, ObjectPrompt{0.8f, 0.5f, false}}, &err, nullptr, 0);
  CHECK(minus && minus->a == m->a);                 // a background region elsewhere takes nothing away
  auto over = segmentObjectWand(im, key, {ObjectPrompt{0.2f, 0.5f, true}, ObjectPrompt{0.45f, 0.5f, false}}, &err, nullptr, 0);
  CHECK(over && count(over->a) == 0u);              // ...one on the same region takes all of it
  auto cut = segmentObjectWand(im, key, {ObjectPrompt{0.5f, 0.5f, true}, ObjectPrompt{0.75f, 0.5f, false}}, &err, nullptr, 0);
  CHECK(cut && count(cut->a) == 0u);                // foreground cancelled by the same region
  // No prompts: nothing selected. Prompts off the picture are clamped onto it.
  auto none = segmentObjectWand(im, key, {}, &err, nullptr, 0);
  CHECK(none && count(none->a) == 0u);
  auto edge = segmentObjectWand(im, key, {ObjectPrompt{-3.0f, 9.0f, true}}, &err, nullptr, 0);
  CHECK(edge && count(edge->a) > 0u);
  // Cancelled: null.
  std::atomic<uint64_t> latest{2};
  const CanvasImage bigImg = solid(300, 300, 9, 9, 9);
  CHECK(segmentObjectWand(bigImg, ++gKey, {ObjectPrompt{0.5f, 0.5f, true}}, &err, &latest, 1) == nullptr);
  CHECK(segmentObjectWand(bigImg, ++gKey, {ObjectPrompt{0.5f, 0.5f, true}}, &err, &latest, 2) != nullptr);
}

static void testWandReusesThePreparedImage()
{
  // The key stands for "this picture": the prepared copy is kept for it, so a
  // second request with the same key is answered from the first picture.
  CanvasImage a = solid(20, 20, 200, 0, 0);
  CanvasImage b = solid(20, 20, 0, 0, 200);
  paint(b, 10, 0, 20, 20, 0, 200, 0);
  const uint64_t key = ++gKey;
  std::string err;
  auto first = segmentObjectWand(a, key, {ObjectPrompt{0.5f, 0.5f, true}}, &err, nullptr, 0);
  auto same = segmentObjectWand(b, key, {ObjectPrompt{0.25f, 0.5f, true}}, &err, nullptr, 0);
  CHECK(first && same && count(same->a) == 400u); // `a` is all one color; `b` was not even looked at
  auto fresh = segmentObjectWand(b, ++gKey, {ObjectPrompt{0.25f, 0.5f, true}}, &err, nullptr, 0);
  CHECK(fresh && count(fresh->a) == 200u);        // a new key prepares the new picture
}


static void testRoundingAndSeedPatch()
{
  // Averaging rounds to nearest: a block whose channel sum is 3 becomes (3 + 2) / 4 = 1, not 0.
  CanvasImage im = solid(600, 100, 0, 0, 0);
  paint(im, 0, 0, 1, 1, 3, 3, 3);
  const SegPrepared p = prepared(im);
  CHECK(p.w == 300 && p.rgb[0] == 1 && p.rgb[1] == 1 && p.rgb[2] == 1);
  // The seed's reference color is the mean of the 5 x 5 patch around it (clamped to the
  // picture). A seed 2 pixels from a color edge has one column of the other color in
  // its patch: reference 100 + 45 / 5 = 109, so both sides (distances 9 and 36) qualify.
  CanvasImage two = solid(20, 5, 100, 100, 100);
  paint(two, 10, 0, 20, 5, 145, 100, 100);
  const SegPrepared tp = prepared(two);
  std::vector<uint8_t> mark(100, 0);
  segGrow(tp, 8, 2, mark, nullptr, 0);
  CHECK(count(mark) == 100u);
  // ...but from 3 pixels away the patch is pure and the other side (distance 45) stays out.
  std::vector<uint8_t> mark2(100, 0);
  segGrow(tp, 7, 2, mark2, nullptr, 0);
  CHECK(count(mark2) == 50u);
  // Same on the other side of the edge, from the left (patch reaches back across it).
  std::vector<uint8_t> mark3(100, 0);
  segGrow(tp, 11, 2, mark3, nullptr, 0);
  CHECK(count(mark3) == 100u);
}

int main()
{
  testRoundingAndSeedPatch();

  testPrepare();
  testGrowTolerance();
  testGrowShape();
  testGrowCancel();
  testWand();
  testWandReusesThePreparedImage();
  return finish();
}
