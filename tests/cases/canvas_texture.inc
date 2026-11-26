// Tests for app-owned textures (CanvasTexture / drawTexture): the thread-safe CPU
// side (update, clipping, premultiplication, change tracking), the per-window
// GPU bookkeeping (TextureTable), and drawTexture through the GL mock and the
// Cairo backend. What the real GPU does with them is covered by
// canvas_gl_partial_check.cpp (hardware).
// (Included from liteui_tests.cpp, after canvas_gl_context.inc — not a
// standalone .cpp.)

#include <atomic>
#include <random>
#include <thread>

namespace texture_test {

inline std::vector<uint8_t> whole(const CanvasTexture &t) {
  std::vector<uint8_t> v;
  REQUIRE(t.readPixels(0, 0, t.width(), t.height(), v));
  return v;
}
// Applies what a GPU copy would apply.
inline void applyDelta(std::vector<uint8_t> &mirror, int W,
                       const CanvasTexture::Delta &d) {
  for (const auto &p : d.patches)
    for (int r = 0; r < p.h; ++r)
      std::copy_n(&p.px[static_cast<size_t>(r) * p.w * 4],
                  static_cast<size_t>(p.w) * 4,
                  &mirror[(static_cast<size_t>(p.y + r) * W + p.x) * 4]);
}
inline std::shared_ptr<CanvasTexture> premul(int w, int h) {
  return std::make_shared<CanvasTexture>(w, h, PixelFormat::RGBA8_UNORM,
                                   TextureAlpha::Premultiplied);
}
// Fills a whole w x h block with one byte value.
inline bool put(CanvasTexture &t, int x, int y, int w, int h, uint8_t v) {
  std::vector<uint8_t> d(static_cast<size_t>(w) * h * 4, v);
  return t.update(x, y, w, h, d.data(), static_cast<size_t>(w) * 4);
}

} // namespace texture_test

TEST_CASE("CanvasTexture: creation", "[texture]") {
  using namespace texture_test;
  SECTION("a valid texture starts transparent") {
    CanvasTexture t(4, 3);
    CHECK(t.valid());
    CHECK(t.width() == 4);
    CHECK(t.height() == 3);
    for (uint8_t b : whole(t))
      CHECK(b == 0);
  }
  SECTION("bad dimensions or formats make an invalid texture") {
    CHECK_FALSE(CanvasTexture(0, 5).valid());
    CHECK_FALSE(CanvasTexture(5, 0).valid());
    CHECK_FALSE(CanvasTexture(-1, 5).valid());
    CHECK_FALSE(CanvasTexture(4, 4, PixelFormat::RGBA16_FLOAT).valid());
    CHECK_FALSE(CanvasTexture(100000, 100000).valid()); // would need ~40 GB: refused
    CanvasTexture bad(0, 0);
    uint8_t px[4] = {1, 2, 3, 4};
    CHECK_FALSE(bad.update(0, 0, 1, 1, px, 4));
    std::vector<uint8_t> out;
    CHECK_FALSE(bad.readPixels(0, 0, 1, 1, out));
  }
  SECTION("every texture has its own id") {
    CanvasTexture a(1, 1), b(1, 1);
    CHECK(a.id() != b.id());
  }
}

TEST_CASE("CanvasTexture::update", "[texture]") {
  using namespace texture_test;

  SECTION("copies a rectangle into place") {
    auto t = premul(8, 8);
    const uint8_t src[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    REQUIRE(t->update(3, 4, 2, 2, src, 8));
    std::vector<uint8_t> out;
    REQUIRE(t->readPixels(3, 4, 2, 2, out));
    CHECK(std::equal(out.begin(), out.end(), src));
    REQUIRE(t->readPixels(2, 4, 1, 1, out));
    CHECK(out == std::vector<uint8_t>{0, 0, 0, 0}); // neighbours untouched
  }

  SECTION("straight alpha is premultiplied on the way in") {
    CanvasTexture t(2, 1); // Straight by default
    const uint8_t src[8] = {255, 0, 0, 128, 10, 20, 30, 255};
    REQUIRE(t.update(0, 0, 2, 1, src, 8));
    std::vector<uint8_t> out;
    REQUIRE(t.readPixels(0, 0, 2, 1, out));
    CHECK(out == std::vector<uint8_t>{128, 0, 0, 128, 10, 20, 30, 255});
  }

  SECTION("a row stride longer than the row is honoured") {
    auto t = premul(2, 2);
    uint8_t src[32] = {};
    for (int i = 0; i < 32; ++i)
      src[i] = static_cast<uint8_t>(i + 1);
    REQUIRE(t->update(0, 0, 2, 2, src, 16)); // 8 bytes of padding per row
    std::vector<uint8_t> out = whole(*t);
    CHECK(std::equal(out.begin(), out.begin() + 8, src));
    CHECK(std::equal(out.begin() + 8, out.end(), src + 16));
  }

  SECTION("a rectangle overhanging the edge is clipped, not shifted") {
    auto t = premul(4, 4);
    uint8_t src[64];
    for (int i = 0; i < 16; ++i) // pixel i has every byte == i + 1
      std::fill_n(src + i * 4, 4, static_cast<uint8_t>(i + 1));
    REQUIRE(t->update(2, 2, 4, 4, src, 16)); // only the top-left 2x2 fits
    std::vector<uint8_t> out;
    REQUIRE(t->readPixels(2, 2, 1, 1, out));
    CHECK(out[0] == 1); // src pixel (0,0)
    REQUIRE(t->readPixels(3, 3, 1, 1, out));
    CHECK(out[0] == 6); // src pixel (1,1)
    REQUIRE(t->readPixels(0, 0, 1, 1, out));
    CHECK(out[0] == 0);
    // Overhanging the top-left: src pixel (1,1) lands on texel (0,0).
    auto u = premul(4, 4);
    REQUIRE(u->update(-1, -1, 2, 2, src, 8));
    REQUIRE(u->readPixels(0, 0, 1, 1, out));
    CHECK(out[0] == 4); // src (row 1, col 1) of a 2-pixel-wide block = pixel 3
    REQUIRE(u->readPixels(1, 1, 1, 1, out));
    CHECK(out[0] == 0);
  }

  SECTION("a rectangle entirely outside succeeds and changes nothing") {
    auto t = premul(4, 4);
    const uint8_t src[16] = {};
    const uint64_t v = t->version();
    CHECK(t->update(10, 10, 2, 2, src, 8));
    CHECK(t->update(-5, 0, 2, 2, src, 8));
    CHECK(t->update(0, 0, 0, 3, src, 8)); // empty
    CHECK(t->version() == v);
  }

  SECTION("invalid arguments are rejected") {
    auto t = premul(4, 4);
    uint8_t src[16] = {};
    CHECK_FALSE(t->update(0, 0, 2, 2, nullptr, 8));
    CHECK_FALSE(t->update(0, 0, -1, 2, src, 8));
    CHECK_FALSE(t->update(0, 0, 2, -1, src, 8));
    CHECK_FALSE(t->update(0, 0, 2, 2, src, 4)); // stride shorter than a row
  }

  SECTION("every update bumps the version and calls notify without the lock") {
    auto t = premul(4, 4);
    int calls = 0;
    uint64_t seen = 0;
    t->setNotify([&] {
      ++calls;
      seen = t->version(); // would deadlock if update() still held its lock
    });
    const uint64_t v0 = t->version();
    REQUIRE(put(*t, 0, 0, 1, 1, 7));
    CHECK(calls == 1);
    CHECK(t->version() == v0 + 1);
    CHECK(seen == t->version());
    t->clear();
    CHECK(calls == 2);
    CHECK(t->version() == v0 + 2);
  }

  SECTION("a CanvasImage can be placed whole (straight-alpha textures only)") {
    CanvasTexture t(4, 4);
    CanvasImage img(2, 1);
    img.pixels = {255, 0, 0, 255, 0, 255, 0, 128};
    REQUIRE(t.update(1, 2, img));
    std::vector<uint8_t> out;
    REQUIRE(t.readPixels(1, 2, 2, 1, out));
    CHECK(out == std::vector<uint8_t>{255, 0, 0, 255, 0, 128, 0, 128});
    auto p = premul(4, 4);
    CHECK_FALSE(p->update(0, 0, img));
    CanvasImage shortImg(2, 2);
    shortImg.pixels.resize(4);
    CHECK_FALSE(t.update(0, 0, shortImg));
  }

  SECTION("clear resets every pixel") {
    auto t = premul(3, 3);
    REQUIRE(put(*t, 0, 0, 3, 3, 9));
    t->clear();
    for (uint8_t b : whole(*t))
      CHECK(b == 0);
  }
}

TEST_CASE("CanvasTexture::wrap", "[texture]") {
  using namespace texture_test;
  auto img = std::make_shared<CanvasImage>(4, 2);
  for (int i = 0; i < 8; ++i) { // opaque and half-transparent pixels alternate
    img->pixels[i * 4 + 0] = static_cast<uint8_t>(10 * i);
    img->pixels[i * 4 + 1] = 200;
    img->pixels[i * 4 + 2] = 100;
    img->pixels[i * 4 + 3] = i % 2 ? 128 : 255;
  }

  SECTION("shares the image's pixels: no copy is made") {
    auto t = CanvasTexture::wrap(img);
    CHECK(t->valid());
    CHECK(t->wrapped());
    CHECK((t->width() == 4 && t->height() == 2));
    CHECK(img.use_count() == 2); // the texture holds the SAME image
  }

  SECTION("it is immutable") {
    auto t = CanvasTexture::wrap(img);
    const uint8_t px[4] = {1, 2, 3, 4};
    const uint64_t v = t->version();
    CHECK_FALSE(t->update(0, 0, 1, 1, px, 4));
    CHECK_FALSE(t->update(0, 0, *img));
    t->clear();
    CHECK(t->version() == v);
    CHECK(img->pixels[0] == 0); // the image itself is untouched
  }

  SECTION("null, empty, short or oversized images give an invalid texture") {
    CHECK_FALSE(CanvasTexture::wrap(nullptr)->valid());
    CHECK_FALSE(CanvasTexture::wrap(std::make_shared<CanvasImage>(0, 0))->valid());
    auto shortImg = std::make_shared<CanvasImage>(4, 4);
    shortImg->pixels.resize(8);
    CHECK_FALSE(CanvasTexture::wrap(shortImg)->valid());
    CHECK_FALSE(CanvasTexture::wrap(nullptr)->wrapped());
  }

  SECTION("pixels read back premultiplied, like an updated texture") {
    auto t = CanvasTexture::wrap(img);
    std::vector<uint8_t> out;
    REQUIRE(t->readPixels(0, 0, 4, 2, out));
    CHECK((out[0] == 0 && out[1] == 200 && out[2] == 100 && out[3] == 255));
    // pixel 1: alpha 128 -> (10*128+127)/255 = 5, (200*128+127)/255 = 100, 50
    CHECK((out[4] == 5 && out[5] == 100 && out[6] == 50 && out[7] == 128));
    // identical to copying the same image into a straight-alpha texture
    CanvasTexture copy(4, 2);
    REQUIRE(copy.update(0, 0, *img));
    CHECK(whole(copy) == out);
    CHECK_FALSE(t->readPixels(3, 1, 2, 1, out)); // not fully inside
  }

  SECTION("a consumer gets one full premultiplied patch, then nothing") {
    auto t = CanvasTexture::wrap(img);
    CanvasTexture::Delta d;
    t->collect(0, d);
    CHECK(d.full);
    REQUIRE(d.patches.size() == 1);
    CHECK((d.patches[0].w == 4 && d.patches[0].h == 2));
    CHECK(d.patches[0].px[4] == 5); // premultiplied
    t->collect(d.version, d);
    CHECK(d.patches.empty());
  }

  SECTION("the texture keeps the image alive; dropping it releases the image") {
    std::weak_ptr<const CanvasImage> weak = img;
    auto t = CanvasTexture::wrap(img);
    img.reset();
    CHECK_FALSE(weak.expired()); // still shown by the texture
    std::vector<uint8_t> out;
    CHECK(t->readPixels(0, 0, 1, 1, out));
    t.reset();
    CHECK(weak.expired());
  }
}

TEST_CASE("CanvasTexture::collect", "[texture]") {
  using namespace texture_test;
  auto t = premul(100, 100);
  CanvasTexture::Delta d;

  SECTION("a new consumer gets everything, as one patch") {
    t->collect(0, d);
    CHECK(d.full);
    REQUIRE(d.patches.size() == 1);
    CHECK((d.patches[0].x == 0 && d.patches[0].y == 0));
    CHECK((d.patches[0].w == 100 && d.patches[0].h == 100));
    CHECK(d.version == t->version());
  }

  SECTION("then only what changed") {
    t->collect(0, d);
    const uint64_t v = d.version;
    REQUIRE(put(*t, 10, 20, 5, 6, 9));
    t->collect(v, d);
    CHECK_FALSE(d.full);
    REQUIRE(d.patches.size() == 1);
    const auto &p = d.patches[0];
    CHECK((p.x == 10 && p.y == 20 && p.w == 5 && p.h == 6));
    CHECK(std::all_of(p.px.begin(), p.px.end(), [](uint8_t b) { return b == 9; }));
    CHECK(d.version == t->version());
  }

  SECTION("nothing changed: no patches, same version") {
    t->collect(0, d);
    const uint64_t v = d.version;
    t->collect(v, d);
    CHECK(d.patches.empty());
    CHECK(d.version == v);
  }

  SECTION("the same tile updated repeatedly is sent once, with the last pixels") {
    t->collect(0, d);
    const uint64_t v = d.version;
    for (uint8_t i = 1; i <= 5; ++i)
      REQUIRE(put(*t, 10, 10, 8, 8, i));
    t->collect(v, d);
    REQUIRE(d.patches.size() == 1);
    CHECK(d.patches[0].px[0] == 5);
  }

  SECTION("a rectangle inside another is not sent separately") {
    t->collect(0, d);
    const uint64_t v = d.version;
    REQUIRE(put(*t, 10, 10, 20, 20, 1));
    REQUIRE(put(*t, 12, 12, 4, 4, 2));
    t->collect(v, d);
    REQUIRE(d.patches.size() == 1);
    const auto &p = d.patches[0];
    CHECK((p.w == 20 && p.h == 20));
    CHECK(p.px[(2 * 20 + 2) * 4] == 2); // the inner update is in the outer patch
    CHECK(p.px[0] == 1);
  }

  SECTION("disjoint rectangles are sent separately") {
    t->collect(0, d);
    const uint64_t v = d.version;
    REQUIRE(put(*t, 0, 0, 4, 4, 1));
    REQUIRE(put(*t, 50, 50, 4, 4, 2));
    t->collect(v, d);
    CHECK(d.patches.size() == 2);
  }

  SECTION("many scattered rectangles collapse into their bounding box") {
    t->collect(0, d);
    const uint64_t v = d.version;
    for (int i = 0; i < 20; ++i)
      REQUIRE(put(*t, i * 4, i * 4, 2, 2, 1));
    t->collect(v, d);
    CHECK_FALSE(d.full);
    REQUIRE(d.patches.size() == 1);
    CHECK((d.patches[0].x == 0 && d.patches[0].y == 0));
    CHECK((d.patches[0].w == 78 && d.patches[0].h == 78));
  }

  SECTION("when most of the texture changed, the whole texture is sent") {
    t->collect(0, d);
    const uint64_t v = d.version;
    REQUIRE(put(*t, 0, 0, 100, 70, 1));
    t->collect(v, d);
    CHECK(d.full);
    REQUIRE(d.patches.size() == 1);
    CHECK((d.patches[0].w == 100 && d.patches[0].h == 100));
  }

  SECTION("a long history collapses without losing a change") {
    std::vector<uint8_t> mirror = whole(*t);
    t->collect(0, d);
    const uint64_t v = d.version;
    for (int i = 0; i < 200; ++i) // far past the log's length limit
      REQUIRE(put(*t, i % 10, i / 10, 1, 1, static_cast<uint8_t>(i + 1)));
    t->collect(v, d);
    applyDelta(mirror, 100, d);
    CHECK(mirror == whole(*t));
  }

  SECTION("consumers at different versions all converge (randomised)") {
    constexpr int W = 37, H = 29;
    auto tex = premul(W, H);
    std::mt19937 rng(12345);
    struct Consumer {
      std::vector<uint8_t> mirror;
      uint64_t seen = 0;
    };
    std::vector<Consumer> cs(3);
    auto sync = [&](Consumer &c) {
      CanvasTexture::Delta dd;
      tex->collect(c.seen, dd);
      if (c.seen == 0)
        c.mirror.assign(static_cast<size_t>(W) * H * 4, 0);
      applyDelta(c.mirror, W, dd);
      c.seen = dd.version;
    };
    for (int step = 0; step < 600; ++step) {
      if (rng() % 3 == 0) {
        sync(cs[rng() % cs.size()]);
      } else {
        const int x = static_cast<int>(rng() % (W + 10)) - 5;
        const int y = static_cast<int>(rng() % (H + 10)) - 5;
        const int w = 1 + static_cast<int>(rng() % 12);
        const int h = 1 + static_cast<int>(rng() % 12);
        REQUIRE(put(*tex, x, y, w, h, static_cast<uint8_t>(rng())));
      }
    }
    for (auto &c : cs) {
      sync(c);
      CHECK(c.mirror == whole(*tex));
    }
  }
}

TEST_CASE("TextureTable", "[texture]") {
  using namespace texture_test;
  using Table = liteui_texture::TextureTable<int>;
  int creates = 0, patches = 0, destroyed = 0;
  auto create = [&](const CanvasTexture &, const CanvasTexture::Patch &full, int &h) {
    ++creates;
    h = 100 + creates;
    return full.w > 0;
  };
  auto apply = [&](int &, const CanvasTexture::Patch &) { ++patches; };
  auto destroy = [&](int &) { ++destroyed; };

  SECTION("creates once, then patches only what changed") {
    auto t = premul(16, 16);
    Table tab;
    bool changed = false;
    auto *e = tab.sync(t, create, apply, &changed);
    REQUIRE(e);
    CHECK(creates == 1);
    CHECK(changed);
    CHECK(e->handle == 101);
    e = tab.sync(t, create, apply, &changed);
    REQUIRE(e);
    CHECK_FALSE(changed);
    CHECK(patches == 0);
    REQUIRE(put(*t, 0, 0, 4, 4, 1));
    e = tab.sync(t, create, apply, &changed);
    REQUIRE(e);
    CHECK(changed);
    CHECK(patches == 1);
    CHECK(creates == 1);
    CHECK(e->handle == 101);
  }

  SECTION("a failed create stores nothing and is retried next time") {
    auto t = premul(4, 4);
    Table tab;
    bool fail = true;
    auto flaky = [&](const CanvasTexture &, const CanvasTexture::Patch &, int &h) {
      if (fail)
        return false;
      h = 7;
      return true;
    };
    CHECK(tab.sync(t, flaky, apply) == nullptr);
    CHECK(tab.size() == 0);
    fail = false;
    auto *e = tab.sync(t, flaky, apply);
    REQUIRE(e);
    CHECK(e->handle == 7);
  }

  SECTION("null and invalid textures are refused") {
    Table tab;
    CHECK(tab.sync(nullptr, create, apply) == nullptr);
    CHECK(tab.sync(std::make_shared<CanvasTexture>(0, 0), create, apply) == nullptr);
    CHECK(creates == 0);
  }

  SECTION("sweep releases the GPU copy of a destroyed texture, once") {
    auto keep = premul(4, 4), gone = premul(4, 4);
    Table tab;
    REQUIRE(tab.sync(keep, create, apply));
    REQUIRE(tab.sync(gone, create, apply));
    gone.reset();
    tab.sweep(destroy);
    CHECK(destroyed == 1);
    CHECK(tab.size() == 1);
    tab.sweep(destroy); // nothing new died
    CHECK(destroyed == 1);
  }

  SECTION("clear destroys everything") {
    auto a = premul(2, 2), b = premul(2, 2);
    Table tab;
    REQUIRE(tab.sync(a, create, apply));
    REQUIRE(tab.sync(b, create, apply));
    tab.clear(destroy);
    CHECK(destroyed == 2);
    CHECK(tab.size() == 0);
  }

  SECTION("two tables are independent consumers of one texture") {
    auto t = premul(8, 8);
    Table w1, w2;
    REQUIRE(w1.sync(t, create, apply));
    REQUIRE(w2.sync(t, create, apply));
    REQUIRE(put(*t, 0, 0, 2, 2, 1));
    REQUIRE(w1.sync(t, create, apply));
    CHECK(patches == 1);
    REQUIRE(w2.sync(t, create, apply)); // still owed the same update
    CHECK(patches == 2);
  }
}

TEST_CASE("CanvasTexture: concurrent updates and collection", "[texture][threads]") {
  using namespace texture_test;
  constexpr int W = 64, H = 64;
  auto t = premul(W, H);
  std::atomic<bool> stop{false};
  std::vector<uint8_t> mirror(static_cast<size_t>(W) * H * 4, 0);
  uint64_t seen = 0;
  std::thread consumer([&] {
    while (!stop.load()) {
      CanvasTexture::Delta d;
      t->collect(seen, d);
      if (seen == 0)
        std::fill(mirror.begin(), mirror.end(), uint8_t(0));
      applyDelta(mirror, W, d);
      seen = d.version;
    }
  });
  std::vector<std::thread> workers;
  for (int k = 0; k < 4; ++k)
    workers.emplace_back([&, k] {
      std::mt19937 rng(static_cast<unsigned>(k));
      for (int i = 0; i < 500; ++i)
        put(*t, static_cast<int>(rng() % 7) * 8, static_cast<int>(rng() % 7) * 8,
            8, 8, static_cast<uint8_t>(rng()));
    });
  for (auto &w : workers)
    w.join();
  stop = true;
  consumer.join();
  CanvasTexture::Delta d; // one last catch-up after all writers are done
  t->collect(seen, d);
  applyDelta(mirror, W, d);
  CHECK(mirror == whole(*t));
}

#if !defined(_WIN32)

TEST_CASE("GL canvas: drawTexture", "[canvas][gl][texture]") {
  using namespace canvas_gl_context_test;
  using namespace texture_test;
  auto quadTex = [] {
    auto t = std::make_shared<CanvasTexture>(2, 2); // Straight alpha
    REQUIRE(t->update(0, 0, quadImage()));
    return t;
  };

  SECTION("scaling picks the right texel for each destination pixel") {
    Fx f;
    f.c.setImageSampling(ImageSampling::Nearest);
    f.c.drawTexture(quadTex(), 10, 10, 4, 4);
    CHECK(f.ch(10, 10, 0) == within(255, 1)); // red
    CHECK(f.ch(12, 10, 1) == within(255, 1)); // green
    CHECK(f.ch(10, 12, 2) == within(255, 1)); // blue
    CHECK(f.ch(13, 13, 0) == within(255, 1)); // white
    CHECK(f.count() == 16);
  }

  SECTION("the source rectangle crops the texture") {
    Fx f;
    f.c.setImageSampling(ImageSampling::Nearest);
    f.c.drawTexture(quadTex(), 1, 0, 1, 2, 0, 0, 8, 16); // right column only
    CHECK(f.count() == 128);
    CHECK(f.ch(3, 3, 1) == within(255, 1));
    CHECK(f.ch(3, 12, 0) == within(255, 1));
  }

  SECTION("a source past the edge is clipped, shrinking the destination") {
    Fx f;
    f.c.setImageSampling(ImageSampling::Nearest);
    f.c.drawTexture(quadTex(), -1, -1, 2, 2, 0, 0, 20, 20);
    CHECK(f.count() == 100);
    CHECK(f.ch(15, 15, 0) == within(255, 1));
  }

  SECTION("degenerate input draws nothing") {
    Fx f;
    auto t = quadTex();
    f.c.drawTexture(nullptr, 0, 0);
    f.c.drawTexture(std::make_shared<CanvasTexture>(0, 0), 0, 0, 8, 8); // invalid
    f.c.drawTexture(t, 0, 0, 0, 2, 0, 0, 8, 8);                    // empty source
    f.c.drawTexture(t, 0, 0, 2, 2, 0, 0, 0, 8);                    // empty dest
    f.c.drawTexture(t, 5, 5, 2, 2, 0, 0, 8, 8);                    // outside
    CHECK(f.count() == 0);
    CHECK(f.gl.textureCreates == 0);
  }

  SECTION("the GPU copy is created once, then only patched") {
    Fx f;
    auto t = premul(8, 8);
    f.c.drawTexture(t, 0, 0);
    f.c.drawTexture(t, 10, 10);
    CHECK(f.gl.textureCreates == 1);
    CHECK(f.gl.texturePatches == 0);
    REQUIRE(put(*t, 4, 4, 2, 2, 255));
    f.c.drawTexture(t, 20, 20);
    CHECK(f.gl.textureCreates == 1);
    REQUIRE(f.gl.texturePatches == 1);
    CHECK((f.gl.patchRects[0] == std::array<int, 4>{4, 4, 2, 2}));
    CHECK(f.gl.uploads == 0); // never goes through the drawImage upload path
  }

  SECTION("an update shows up in the next draw") {
    Fx f;
    f.c.setImageSampling(ImageSampling::Nearest);
    auto t = premul(4, 4);
    for (int i = 0; i < 16; ++i) { // opaque green
      const uint8_t px[4] = {0, 255, 0, 255};
      REQUIRE(t->update(i % 4, i / 4, 1, 1, px, 4));
    }
    f.c.drawTexture(t, 0, 0);
    CHECK(f.ch(1, 1, 1) == within(255, 1));
    const uint8_t red[4] = {255, 0, 0, 255};
    REQUIRE(t->update(1, 1, 1, 1, red, 4));
    f.c.drawTexture(t, 8, 0);
    CHECK(f.ch(9, 1, 0) == within(255, 1)); // the edited texel is red
    CHECK(f.ch(9, 1, 1) == within(0, 1));
    CHECK(f.ch(10, 1, 1) == within(255, 1)); // its neighbour is still green
  }

  SECTION("straight alpha is premultiplied; globalAlpha, clip and transform apply") {
    Fx f(32, 32);
    f.c.setImageSampling(ImageSampling::Nearest);
    auto half = std::make_shared<CanvasTexture>(1, 1);
    const uint8_t px[4] = {255, 0, 0, 128};
    REQUIRE(half->update(0, 0, 1, 1, px, 4));
    f.c.drawTexture(half, 0, 0);
    CHECK(f.ch(0, 0, 3) == within(128, 1));
    CHECK(f.ch(0, 0, 0) == within(128, 1));
    f.c.setGlobalAlpha(0.5f);
    f.c.drawTexture(half, 4, 0);
    CHECK(f.ch(4, 0, 3) == within(64, 1));
    f.c.setGlobalAlpha(1);
    f.c.save();
    f.c.beginPath();
    f.c.rect(0, 8, 4, 4);
    f.c.clip();
    f.c.drawTexture(quadTex(), 0, 8, 8, 8);
    f.c.restore();
    CHECK(f.ch(2, 10, 3) == 255.0f);
    CHECK(f.ch(6, 10, 3) == 0.0f);
    f.c.translate(20, 20);
    f.c.scale(2, 2);
    f.c.drawTexture(quadTex(), 0, 0, 4, 4); // 8x8 on screen at (20,20)
    CHECK(f.ch(21, 21, 0) == within(255, 1));
    CHECK(f.ch(26, 26, 0) == within(255, 1));
  }

  SECTION("a destroyed texture's GPU copy is released") {
    Fx f;
    { f.c.drawTexture(quadTex(), 0, 0); } // drawn, then dropped
    CHECK(f.gl.deleted.empty());
    f.c.drawTexture(quadTex(), 8, 8); // next draw sweeps the dead entry
    CHECK(f.gl.deleted.size() == 1);
    CHECK(f.gl.textureCreates == 2);
  }

  SECTION("each canvas renderer keeps its own GPU copy") {
    Fx a, b;
    auto t = premul(8, 8);
    a.c.drawTexture(t, 0, 0);
    b.c.drawTexture(t, 0, 0);
    CHECK(a.gl.textureCreates == 1);
    CHECK(b.gl.textureCreates == 1);
    REQUIRE(put(*t, 0, 0, 2, 2, 1));
    a.c.drawTexture(t, 0, 0);
    b.c.drawTexture(t, 0, 0);
    CHECK(a.gl.texturePatches == 1);
    CHECK(b.gl.texturePatches == 1);
  }
}

TEST_CASE("GL canvas: a wrapped texture draws like an updated one",
          "[canvas][gl][texture]") {
  using namespace canvas_gl_context_test;
  auto img = std::make_shared<CanvasImage>(quadImage());
  Fx f;
  f.c.setImageSampling(ImageSampling::Nearest);
  auto t = CanvasTexture::wrap(img);
  f.c.drawTexture(t, 10, 10, 4, 4);
  CHECK(f.ch(10, 10, 0) == within(255, 1)); // red
  CHECK(f.ch(12, 10, 1) == within(255, 1)); // green
  CHECK(f.ch(10, 12, 2) == within(255, 1)); // blue
  CHECK(f.count() == 16);
  f.c.drawTexture(t, 20, 20); // drawn again: no new GPU copy, nothing patched
  CHECK(f.gl.textureCreates == 1);
  CHECK(f.gl.texturePatches == 0);
  CHECK(f.gl.uploads == 0);
  t.reset(); // the image is released once the texture is gone
  f.c.drawTexture(CanvasTexture::wrap(img), 0, 0, 2, 2);
  CHECK(f.gl.deleted.size() == 1);
}

TEST_CASE("GL canvas: textures use mipmaps, rebuilt only after a change",
          "[canvas][gl][texture][sampling]") {
  using namespace canvas_gl_context_test;
  using namespace texture_test;
  using liteui_canvas_sampling::ImageFilter;
  auto big = [] {
    auto t = std::make_shared<CanvasTexture>(4, 4, PixelFormat::RGBA8_UNORM,
                                       TextureAlpha::Premultiplied);
    REQUIRE(put(*t, 0, 0, 4, 4, 100));
    return t;
  };

  SECTION("a minified draw builds the chain once") {
    Fx f;
    auto t = big();
    f.c.drawTexture(t, 0, 0, 2, 2);
    CHECK(f.gl.lastFilter == ImageFilter::Mipmap);
    CHECK(f.gl.mipmapped.size() == 1);
    f.c.drawTexture(t, 4, 0, 2, 2);
    CHECK(f.gl.mipmapped.size() == 1);
  }
  SECTION("an update makes the chain stale: rebuilt on the next minified draw") {
    Fx f;
    auto t = big();
    f.c.drawTexture(t, 0, 0, 2, 2);
    REQUIRE(put(*t, 1, 1, 1, 1, 200));
    f.c.drawTexture(t, 8, 0, 8, 8); // magnified: no rebuild needed
    CHECK(f.gl.lastFilter == ImageFilter::Linear);
    CHECK(f.gl.mipmapped.size() == 1);
    f.c.drawTexture(t, 0, 8, 2, 2); // minified again
    CHECK(f.gl.mipmapped.size() == 2);
  }
  SECTION("magnified and Nearest draws build nothing") {
    Fx f;
    auto t = big();
    f.c.drawTexture(t, 0, 0, 8, 8);
    f.c.setImageSampling(ImageSampling::Nearest);
    f.c.drawTexture(t, 0, 0, 2, 2);
    CHECK(f.gl.mipmapped.empty());
  }
}

#endif // !_WIN32
