// Tests for the pure helpers of the GL canvas backend (Linux, OpenGL ES 3.0):
// blend factors, gradient colour ramp, glyph-atlas packer, matrix inverse,
// premultiplication and the colour-glyph heuristic. None of these touch GL.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

#if defined(LITEUI_CANVAS_GL) && !defined(_WIN32)

namespace cgl = liteui_canvas_gl; // short alias for the cases below

namespace canvas_gl_helpers_test {

// Small deterministic generator so failures reproduce.
struct Lcg {
  unsigned s;
  explicit Lcg(unsigned seed) : s(seed) {}
  float next() { // [0, 1)
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 8) / static_cast<float>(1 << 24);
  }
};

inline float factorValue(cgl::BlendFactor f, const float *s, const float *d,
                         int k) {
  using F = cgl::BlendFactor;
  switch (f) {
  case F::Zero:
    return 0;
  case F::One:
    return 1;
  case F::SrcAlpha:
    return s[3];
  case F::OneMinusSrcAlpha:
    return 1 - s[3];
  case F::DstAlpha:
    return d[3];
  case F::OneMinusDstAlpha:
    return 1 - d[3];
  case F::DstColor:
    return d[k];
  case F::OneMinusDstColor:
    return 1 - d[k];
  }
  return 1;
}

} // namespace canvas_gl_helpers_test

TEST_CASE("GL blend factors: every operator matches the W3C compositing formulas",
          "[canvas][gl][blend]") {
  using namespace canvas_gl_helpers_test;
  struct Op {
    CompositeOp op;
    const char *name;
  };
  const Op ops[] = {{CompositeOp::SourceOver, "source-over"},
                    {CompositeOp::SourceIn, "source-in"},
                    {CompositeOp::SourceOut, "source-out"},
                    {CompositeOp::SourceAtop, "source-atop"},
                    {CompositeOp::DestinationOver, "destination-over"},
                    {CompositeOp::DestinationIn, "destination-in"},
                    {CompositeOp::DestinationOut, "destination-out"},
                    {CompositeOp::DestinationAtop, "destination-atop"},
                    {CompositeOp::Lighter, "lighter"},
                    {CompositeOp::Copy, "copy"},
                    {CompositeOp::Xor, "xor"},
                    {CompositeOp::Screen, "screen"}};

  for (const Op &o : ops) {
    Lcg rng(7);
    float worst = 0;
    for (int i = 0; i < 20000; ++i) {
      // A random valid premultiplied source and destination colour.
      float s[4], d[4];
      s[3] = rng.next();
      d[3] = rng.next();
      for (int k = 0; k < 3; ++k) {
        s[k] = rng.next() * s[3];
        d[k] = rng.next() * d[3];
      }
      const cgl::BlendFactors f = cgl::blendFactorsFor(o.op);
      for (int k = 0; k < 4; ++k) {
        // What the GPU computes: src * srcFactor + dst * dstFactor.
        const float got =
            s[k] * factorValue(k < 3 ? f.srcRgb : f.srcA, s, d, k) +
            d[k] * factorValue(k < 3 ? f.dstRgb : f.dstA, s, d, k);
        const float S = s[k], D = d[k], as = s[3], ab = d[3];
        float ref = 0;
        switch (o.op) {
        case CompositeOp::SourceOver:
          ref = S + D * (1 - as);
          break;
        case CompositeOp::SourceIn:
          ref = S * ab;
          break;
        case CompositeOp::SourceOut:
          ref = S * (1 - ab);
          break;
        case CompositeOp::SourceAtop:
          ref = S * ab + D * (1 - as);
          break;
        case CompositeOp::DestinationOver:
          ref = D + S * (1 - ab);
          break;
        case CompositeOp::DestinationIn:
          ref = D * as;
          break;
        case CompositeOp::DestinationOut:
          ref = D * (1 - as);
          break;
        case CompositeOp::DestinationAtop:
          ref = D * as + S * (1 - ab);
          break;
        case CompositeOp::Lighter:
          ref = S + D;
          break;
        case CompositeOp::Copy:
          ref = S;
          break;
        case CompositeOp::Xor:
          ref = S * (1 - ab) + D * (1 - as);
          break;
        case CompositeOp::Screen: // colour and alpha both "screen"
          ref = S + D - S * D;
          if (k == 3)
            ref = as + ab - as * ab;
          break;
        default:
          break;
        }
        worst = std::max(worst, std::fabs(got - ref));
      }
    }
    INFO("operator: " << o.name << ", worst deviation " << worst);
    CHECK(worst < 1e-5f);
  }
}

TEST_CASE("GL blend factors: Multiply is computed by the shader, blending replaces",
          "[canvas][gl][blend]") {
  using namespace canvas_gl_helpers_test;
  using F = cgl::BlendFactor;

  SECTION("the factors are plain replace") {
    const auto f = cgl::blendFactorsFor(CompositeOp::Multiply);
    CHECK((f.srcRgb == F::One && f.dstRgb == F::Zero));
    CHECK((f.srcA == F::One && f.dstA == F::Zero));
  }

  SECTION("the shader formula equals the W3C separable blend with B = cb*cs") {
    Lcg rng(11);
    float worst = 0;
    for (int i = 0; i < 20000; ++i) {
      const float as = 0.05f + rng.next() * 0.95f; // source alpha
      const float ab = 0.05f + rng.next() * 0.95f; // backdrop alpha
      for (int k = 0; k < 3; ++k) {
        const float cs = rng.next(), cb = rng.next(); // un-premultiplied
        const float Cs = cs * as, Cb = cb * ab;       // premultiplied
        const float shader = Cs * (1 - ab) + Cb * (1 - as) + Cs * Cb;
        const float w3c = (1 - ab) * Cs + (1 - as) * Cb + as * ab * (cb * cs);
        worst = std::max(worst, std::fabs(shader - w3c));
      }
    }
    CHECK(worst < 1e-5f);
  }
}

TEST_CASE("mayHaveColorGlyphs: which strings take the colour-glyph path",
          "[canvas][gl][text]") {
  using cgl::mayHaveColorGlyphs;

  SECTION("ordinary text does not") {
    CHECK_FALSE(mayHaveColorGlyphs(""));
    CHECK_FALSE(mayHaveColorGlyphs("Hello, world 123"));
    CHECK_FALSE(mayHaveColorGlyphs("caf\xC3\xA9 na\xC3\xAFve")); // accents
    CHECK_FALSE(mayHaveColorGlyphs("\xE6\x97\xA5\xE6\x9C\xAC")); // CJK
  }

  SECTION("emoji, pictographs and emoji modifiers do") {
    CHECK(mayHaveColorGlyphs("\xF0\x9F\x98\x80"));         // U+1F600
    CHECK(mayHaveColorGlyphs("a\xF0\x9F\x8E\x89" "b"));    // inside a string
    CHECK(mayHaveColorGlyphs("\xE2\x98\x85"));             // U+2605
    CHECK(mayHaveColorGlyphs("x\xEF\xB8\x8F"));            // U+FE0F selector
    CHECK(mayHaveColorGlyphs("\xE2\x80\x8D"));             // zero-width joiner
  }

  SECTION("malformed UTF-8 is handled safely") {
    CHECK_FALSE(mayHaveColorGlyphs("\xF0\x9F"));  // truncated 4-byte sequence
    CHECK_FALSE(mayHaveColorGlyphs("\xE2\x98"));  // truncated 3-byte sequence
    CHECK_FALSE(mayHaveColorGlyphs("\xFF\xFE"));  // invalid lead bytes
  }
}

TEST_CASE("ShelfPacker: glyph-atlas rectangle packing", "[canvas][gl][atlas]") {
  SECTION("never overlaps or leaves the atlas, and eventually fills up") {
    cgl::ShelfPacker p;
    p.size = 64;
    struct R {
      int x, y, w, h;
    };
    std::vector<R> placed;
    canvas_gl_helpers_test::Lcg rng(12345);
    bool bad = false;
    int rejected = 0;
    for (int i = 0; i < 400; ++i) {
      const int w = 3 + static_cast<int>(rng.next() * 17);
      const int h = 3 + static_cast<int>(rng.next() * 13);
      int x = 0, y = 0;
      if (!p.allocate(w, h, x, y)) {
        ++rejected;
        continue;
      }
      if (x < 0 || y < 0 || x + w > 64 || y + h > 64)
        bad = true;
      for (const R &r : placed)
        if (x < r.x + r.w && r.x < x + w && y < r.y + r.h && r.y < y + h)
          bad = true;
      placed.push_back({x, y, w, h});
    }
    CHECK_FALSE(bad);
    CHECK(placed.size() > 20);
    CHECK(rejected > 0); // 400 rectangles cannot fit in 64 x 64
  }

  SECTION("rejects empty and oversize rectangles") {
    cgl::ShelfPacker p;
    p.size = 64;
    int x = 0, y = 0;
    CHECK_FALSE(p.allocate(100, 5, x, y));
    CHECK_FALSE(p.allocate(5, 100, x, y));
    CHECK_FALSE(p.allocate(0, 5, x, y));
    CHECK_FALSE(p.allocate(5, 0, x, y));
  }

  SECTION("reset() frees the whole area again") {
    cgl::ShelfPacker p;
    p.size = 64;
    int x = 0, y = 0;
    REQUIRE(p.allocate(64, 64, x, y));
    CHECK(x == 0);
    CHECK(y == 0);
    CHECK_FALSE(p.allocate(1, 1, x, y)); // completely full
    p.reset();
    CHECK(p.allocate(64, 64, x, y));
  }

  SECTION("rectangles of equal height share a shelf") {
    cgl::ShelfPacker p;
    p.size = 100;
    int x = 0, y = 0, x2 = 0, y2 = 0;
    REQUIRE(p.allocate(30, 10, x, y));
    REQUIRE(p.allocate(30, 10, x2, y2));
    CHECK(y2 == y);
    CHECK(x2 == x + 30);
  }
}

TEST_CASE("invert2x3: inverse of an affine transform", "[canvas][gl][matrix]") {
  // The result is a column-major 3x3 for glUniformMatrix3fv; applying it to a
  // transformed point must give the original back.
  auto applyInv = [](const float m[9], float x, float y) {
    return std::pair<float, float>{m[0] * x + m[3] * y + m[6],
                                   m[1] * x + m[4] * y + m[7]};
  };

  SECTION("identity") {
    float m[9];
    REQUIRE(cgl::invert2x3(1, 0, 0, 1, 0, 0, m));
    const auto p = applyInv(m, 3, 4);
    CHECK(p.first == approx(3));
    CHECK(p.second == approx(4));
  }

  SECTION("translation and scale") {
    float m[9];
    REQUIRE(cgl::invert2x3(2, 0, 0, 4, 10, 20, m)); // x' = 2x+10, y' = 4y+20
    const auto p = applyInv(m, 14, 36);             // from (2, 4)
    CHECK(p.first == approx(2));
    CHECK(p.second == approx(4));
  }

  SECTION("random transforms round-trip") {
    canvas_gl_helpers_test::Lcg rng(99);
    float worst = 0;
    for (int i = 0; i < 2000; ++i) {
      const float a = rng.next() * 4 - 2, b = rng.next() * 4 - 2;
      const float c = rng.next() * 4 - 2, d = rng.next() * 4 - 2;
      const float e = rng.next() * 100 - 50, f = rng.next() * 100 - 50;
      float m[9];
      if (!cgl::invert2x3(a, b, c, d, e, f, m))
        continue; // (near-)singular draws are skipped
      const float px = rng.next() * 100, py = rng.next() * 100;
      const float tx = a * px + c * py + e, ty = b * px + d * py + f;
      const auto q = applyInv(m, tx, ty);
      worst = std::max({worst, std::fabs(q.first - px), std::fabs(q.second - py)});
    }
    CHECK(worst < 0.05f); // single-precision round trip
  }

  SECTION("a singular transform has no inverse") {
    float m[9];
    CHECK_FALSE(cgl::invert2x3(0, 0, 0, 0, 5, 5, m));  // collapses to a point
    CHECK_FALSE(cgl::invert2x3(1, 2, 2, 4, 0, 0, m));  // collapses to a line
  }
}

TEST_CASE("buildRamp: gradient colour ramp", "[canvas][gl][gradient]") {
  std::vector<uint8_t> ramp(static_cast<size_t>(cgl::kRampSize) * 4);
  auto texel = [&](int i, int k) { return static_cast<int>(ramp[i * 4 + k]); };
  const Color red{255, 0, 0, 255}, blue{0, 0, 255, 255};
  const int last = cgl::kRampSize - 1;

  SECTION("two stops: end colours and the middle") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    g.addColorStop(0, red);
    g.addColorStop(1, blue);
    cgl::buildRamp(g, ramp.data());
    CHECK(texel(0, 0) == 255);
    CHECK(texel(0, 2) == 0);
    CHECK(texel(last, 0) == 0);
    CHECK(texel(last, 2) == 255);
    CHECK(std::abs(texel(last / 2, 0) - 128) <= 2);
    CHECK(std::abs(texel(last / 2, 2) - 127) <= 2);
    CHECK(texel(last / 2, 3) == 255);
  }

  SECTION("stops need not be given in order") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    g.addColorStop(1, blue);
    g.addColorStop(0, red);
    cgl::buildRamp(g, ramp.data());
    CHECK(texel(0, 0) == 255);
    CHECK(texel(last, 2) == 255);
  }

  SECTION("colour extends before the first and after the last stop") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    g.addColorStop(0.25f, red);
    g.addColorStop(0.75f, blue);
    cgl::buildRamp(g, ramp.data());
    CHECK(texel(0, 0) == 255);   // before 0.25: red
    CHECK(texel(last, 2) == 255); // after 0.75: blue
  }

  SECTION("two stops at the same offset make a hard edge") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    g.addColorStop(0, red);
    g.addColorStop(0.5f, red);
    g.addColorStop(0.5f, blue);
    g.addColorStop(1, blue);
    cgl::buildRamp(g, ramp.data());
    CHECK(texel(cgl::kRampSize / 2 - 2, 0) == 255);
    CHECK(texel(cgl::kRampSize / 2 + 2, 2) == 255);
    CHECK(texel(cgl::kRampSize / 2 + 2, 0) == 0);
  }

  SECTION("translucent stops are premultiplied") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    g.addColorStop(0, Color{255, 255, 255, 0});
    g.addColorStop(1, Color{255, 255, 255, 255});
    cgl::buildRamp(g, ramp.data());
    CHECK(texel(0, 3) == 0);
    CHECK(texel(0, 0) == 0); // transparent white is (0,0,0,0) premultiplied
    for (int i = 0; i < cgl::kRampSize; i += 37)
      CHECK(std::abs(texel(i, 0) - texel(i, 3)) <= 1); // white: colour == alpha
    CHECK(std::abs(texel(last / 2, 3) - 128) <= 2);
  }

  SECTION("no stops gives a fully transparent ramp") {
    CanvasGradient g = CanvasGradient::linear(0, 0, 1, 0);
    cgl::buildRamp(g, ramp.data());
    for (size_t i = 0; i < ramp.size(); ++i)
      REQUIRE(ramp[i] == 0);
  }
}

TEST_CASE("premultiplyImage: straight RGBA to premultiplied", "[canvas][gl][image]") {
  CanvasImage img(4, 1);
  const uint8_t px[16] = {255, 128, 0,   128,  // half-transparent orange
                          10,  20,  30,  0,    // fully transparent
                          200, 100, 50,  255,  // opaque: unchanged
                          255, 255, 255, 1};   // nearly invisible
  std::copy(px, px + 16, img.pixels.begin());
  std::vector<uint8_t> out;
  cgl::premultiplyImage(img, out);
  REQUIRE(out.size() == 16);
  CHECK(out[0] == 128);
  CHECK(out[1] == 64);
  CHECK(out[2] == 0);
  CHECK(out[3] == 128);
  CHECK((out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0));
  CHECK((out[8] == 200 && out[9] == 100 && out[10] == 50 && out[11] == 255));
  CHECK(out[12] == 1);
  CHECK(out[15] == 1);
}

#endif // LITEUI_CANVAS_GL && !_WIN32
