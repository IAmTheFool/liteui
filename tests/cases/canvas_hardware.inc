// Hardware canvas checks: opens a real window, draws known patterns into a
// Canvas, reads the actual pixels back with getImageData() and verifies them.
// Unlike the other canvas cases this exercises the real backend and driver
// (OpenGL ES 3.0 on Linux, Direct3D 11 on Windows, or Cairo with
// -DLITEUI_CANVAS_GL=OFF), so it is how you find out whether a particular
// GPU/driver does what liteui expects.
//
// HIDDEN BY DEFAULT: it needs a display, so the "[.]" tag keeps it out of a
// normal run (and out of CI). Run it explicitly:
//
//     liteui_core_test "[hardware]"        (add -s to also list passing checks)
//
// Run it under each backend (Linux GL / Linux Cairo / Windows) to compare them.
// Backends without a feature are reported as a warning ("SKIP ...") rather
// than a failure; the emoji line is informational.
//
// The window is opened once; the results are then reported through Catch2 as
// one section per check. (Included from liteui_tests.cpp.)

namespace canvas_hw {
constexpr int W = 160, H = 160;
constexpr float kPi = 3.14159265358979f;

#if defined(_WIN32)
constexpr const char *kBackend = "Windows (Direct3D 11 + Direct2D)";
#elif defined(LITEUI_CANVAS_GL)
constexpr const char *kBackend = "Linux (OpenGL ES 3.0)";
#else
constexpr const char *kBackend = "Linux (Cairo)";
#endif

// Results are recorded while the window paints and reported through Catch2
// afterwards (Catch2 assertions are not used inside the paint callback).
struct Result {
  enum class Kind { Pass, Fail, Skip, Info } kind;
  std::string name, detail;
};
inline std::vector<Result> &results() {
  static std::vector<Result> r;
  return r;
}
inline void report(const char *name, bool ok, const std::string &detail = "") {
  results().push_back({ok ? Result::Kind::Pass : Result::Kind::Fail, name, detail});
}
inline void skip(const char *name, const char *why) {
  results().push_back({Result::Kind::Skip, name, why});
}
inline void info(const std::string &s) {
  // Catch2 treats sections with the same name as one section, so each info
  // line needs a name of its own.
  results().push_back({Result::Kind::Info, "info: " + s, s});
}

struct Px {
  int r, g, b, a;
};
inline Px at(const CanvasImage &im, int x, int y) {
  const uint8_t *p = &im.pixels[(static_cast<size_t>(y) * im.width + x) * 4];
  return {p[0], p[1], p[2], p[3]};
}
inline bool pxNear(const Px &p, int r, int g, int b, int a, int tol = 3) {
  return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol &&
         std::abs(p.b - b) <= tol && std::abs(p.a - a) <= tol;
}
inline std::string str(const Px &p) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "got (%d,%d,%d,%d)", p.r, p.g, p.b, p.a);
  return buf;
}
inline int countAlpha(const CanvasImage &im, int minAlpha) {
  int n = 0;
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x)
      n += at(im, x, y).a >= minAlpha;
  return n;
}
struct Box {
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1, n = 0;
};
inline Box inkBox(const CanvasImage &im, int minAlpha = 128) {
  Box b;
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x)
      if (at(im, x, y).a >= minAlpha) {
        b.x0 = std::min(b.x0, x);
        b.y0 = std::min(b.y0, y);
        b.x1 = std::max(b.x1, x);
        b.y1 = std::max(b.y1, y);
        ++b.n;
      }
  return b;
}

// Runs one check: clears the canvas, runs `draw` inside save()/restore(), and
// hands the finished pixels to `verify`.
template <class Draw, class Verify>
inline void test(CanvasContext &g, const char *name, Draw draw, Verify verify) {
  g.clearRect(0, 0, W, H);
  g.save();
  draw(g);
  g.restore();
  auto im = g.getImageData(0, 0, W, H);
  if (!im || im->width != W || im->height != H) {
    report(name, false, "getImageData failed");
    return;
  }
  verify(*im);
}

const Color kRed{255, 0, 0, 255}, kGreen{0, 255, 0, 255},
    kBlue{0, 0, 255, 255}, kWhite{255, 255, 255, 255},
    kBlack{0, 0, 0, 255};

inline void runAll(CanvasContext &g) {
  info(std::string("backend: ") + kBackend);
  {
    char d[80];
    std::snprintf(d, sizeof(d), "canvas %dx%d (reported %gx%g)", W, H, g.width(), g.height());
    info(d);
  }

  test(g, "fillRect: solid colour and edges",
       [](CanvasContext &c) {
         c.setFillColor(kRed);
         c.fillRect(10, 10, 40, 40);
       },
       [](const CanvasImage &im) {
         bool ok = pxNear(at(im, 30, 30), 255, 0, 0, 255) &&
                   at(im, 5, 5).a == 0 && at(im, 52, 30).a == 0 &&
                   at(im, 30, 52).a == 0;
         report("fillRect: solid colour and edges", ok, str(at(im, 30, 30)));
       });

  test(g, "alpha blend over opaque white",
       [](CanvasContext &c) {
         c.setFillColor(kWhite);
         c.fillRect(0, 0, W, H);
         c.setFillColor(Color{0, 0, 255, 128});
         c.fillRect(20, 20, 40, 40);
       },
       [](const CanvasImage &im) {
         report("alpha blend over opaque white",
                pxNear(at(im, 40, 40), 127, 127, 255, 255, 4),
                str(at(im, 40, 40)));
       });

  test(g, "transform: translate + rotate(90deg) + fillRect",
       [](CanvasContext &c) {
         c.setFillColor(kRed);
         c.translate(100, 20);
         c.rotate(kPi / 2);
         c.fillRect(0, 0, 30, 10); // lands on x 90..100, y 20..50
       },
       [](const CanvasImage &im) {
         report("transform: translate + rotate(90deg) + fillRect",
                pxNear(at(im, 95, 35), 255, 0, 0, 255) &&
                    at(im, 110, 25).a == 0 && at(im, 95, 55).a == 0,
                str(at(im, 95, 35)));
       });

  test(g, "fill: evenodd leaves a hole, nonzero (same direction) does not",
       [](CanvasContext &c) {
         c.setFillColor(kBlue);
         c.beginPath();
         c.rect(10, 10, 60, 60);
         c.rect(30, 30, 20, 20);
         c.fill(FillRule::EvenOdd);
         c.beginPath();
         c.rect(90, 10, 60, 60);
         c.rect(110, 30, 20, 20);
         c.fill(FillRule::NonZero);
       },
       [](const CanvasImage &im) {
         report("fill: evenodd leaves a hole, nonzero (same direction) does not",
                at(im, 40, 40).a == 0 && at(im, 15, 15).a == 255 &&
                    at(im, 120, 40).a == 255,
                str(at(im, 40, 40)));
       });

  test(g, "arc: filled circle has the right area",
       [](CanvasContext &c) {
         c.setFillColor(kBlack);
         c.beginPath();
         c.arc(80, 80, 40, 0, 2 * kPi);
         c.fill();
       },
       [](const CanvasImage &im) {
         // Fully covered pixels only, so antialiasing can't inflate the count.
         int n = countAlpha(im, 250);
         double ideal = kPi * 40 * 40;
         char d[96];
         std::snprintf(d, sizeof(d), "%d opaque pixels, ideal %.0f", n, ideal);
         report("arc: filled circle has the right area",
                n > ideal * 0.94 && n < ideal * 1.02, d);
       });

  test(g, "stroke: width, butt caps",
       [](CanvasContext &c) {
         c.setStrokeColor(kBlack);
         c.setLineWidth(10);
         c.beginPath();
         c.moveTo(20, 80);
         c.lineTo(140, 80);
         c.stroke();
       },
       [](const CanvasImage &im) {
         report("stroke: width, butt caps",
                at(im, 80, 80).a == 255 && at(im, 80, 76).a == 255 &&
                    at(im, 80, 70).a == 0 && at(im, 80, 90).a == 0 &&
                    at(im, 15, 80).a == 0 && at(im, 25, 80).a == 255,
                str(at(im, 15, 80)));
       });

  test(g, "stroke: square caps extend the line",
       [](CanvasContext &c) {
         c.setStrokeColor(kBlack);
         c.setLineWidth(10);
         c.setLineCap(LineCap::Square);
         c.beginPath();
         c.moveTo(20, 80);
         c.lineTo(140, 80);
         c.stroke();
       },
       [](const CanvasImage &im) {
         report("stroke: square caps extend the line",
                at(im, 17, 80).a == 255 && at(im, 13, 80).a == 0,
                str(at(im, 17, 80)));
       });

  test(g, "stroke: translucent joins do not double-darken",
       [](CanvasContext &c) {
         c.setStrokeColor(Color{0, 0, 0, 255});
         c.setGlobalAlpha(0.5f);
         c.setLineWidth(20);
         c.beginPath();
         c.moveTo(20, 40);
         c.lineTo(100, 40);
         c.lineTo(100, 120);
         c.stroke();
       },
       [](const CanvasImage &im) {
         int a = at(im, 96, 44).a; // inside both segments near the corner
         char d[64];
         std::snprintf(d, sizeof(d), "alpha %d (want ~128)", a);
         report("stroke: translucent joins do not double-darken",
                std::abs(a - 128) <= 4, d);
       });

  test(g, "clip: circle clip, then restore",
       [](CanvasContext &c) {
         c.save();
         c.beginPath();
         c.arc(80, 80, 30, 0, 2 * kPi);
         c.clip();
         c.setFillColor(kRed);
         c.fillRect(0, 0, W, H);
         c.restore();
         c.setFillColor(kBlue);
         c.fillRect(0, 0, 10, 10); // clip must be gone again
       },
       [](const CanvasImage &im) {
         report("clip: circle clip, then restore",
                pxNear(at(im, 80, 80), 255, 0, 0, 255) && at(im, 40, 40).a == 0 &&
                    pxNear(at(im, 5, 5), 0, 0, 255, 255),
                str(at(im, 40, 40)));
       });

  test(g, "linear gradient",
       [](CanvasContext &c) {
         CanvasGradient gr = CanvasGradient::linear(0, 0, W, 0);
         gr.addColorStop(0, kRed);
         gr.addColorStop(1, kBlue);
         c.setFillGradient(gr);
         c.fillRect(0, 0, W, H);
       },
       [](const CanvasImage &im) {
         bool ok = pxNear(at(im, 1, 80), 253, 0, 2, 255, 6) &&
                   pxNear(at(im, 80, 80), 127, 0, 127, 255, 6) &&
                   pxNear(at(im, 158, 80), 2, 0, 253, 255, 6);
         report("linear gradient", ok, str(at(im, 80, 80)));
       });

  test(g, "radial gradient (inner colour, middle, padded outside)",
       [](CanvasContext &c) {
         CanvasGradient gr = CanvasGradient::radial(80, 80, 0, 80, 80, 60);
         gr.addColorStop(0, kRed);
         gr.addColorStop(1, kBlue);
         c.setFillGradient(gr);
         c.fillRect(0, 0, W, H);
       },
       [](const CanvasImage &im) {
         bool ok = pxNear(at(im, 80, 80), 255, 0, 0, 255, 8) &&
                   pxNear(at(im, 80, 110), 127, 0, 127, 255, 10) &&
                   pxNear(at(im, 80, 155), 0, 0, 255, 255, 4);
         report("radial gradient (inner colour, middle, padded outside)", ok,
                str(at(im, 80, 110)));
       });

  test(g, "drawImage: scaling picks the right texels",
       [](CanvasContext &c) {
         CanvasImage img(2, 2);
         const uint8_t px[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                                 0, 0, 255, 255, 255, 255, 255, 255};
         std::copy(px, px + 16, img.pixels.begin());
         c.drawImage(img, 10, 10, 64, 64);
       },
       [](const CanvasImage &im) {
         // Quadrant centres are far from the bilinear transitions, but not
         // exactly on a texel centre: the sample is ~1.6% of a texel off on
         // each axis, so a pure colour reads as e.g. (247,4,4). Allow 10.
         bool ok = pxNear(at(im, 26, 26), 255, 0, 0, 255, 10) &&
                   pxNear(at(im, 58, 26), 0, 255, 0, 255, 10) &&
                   pxNear(at(im, 26, 58), 0, 0, 255, 255, 10) &&
                   pxNear(at(im, 58, 58), 255, 255, 255, 255, 10) &&
                   at(im, 5, 5).a == 0;
         report("drawImage: scaling picks the right texels", ok,
                str(at(im, 26, 26)));
       });

  test(g, "drawImage: source rectangle crops",
       [](CanvasContext &c) {
         CanvasImage img(2, 2);
         const uint8_t px[16] = {255, 0, 0, 255, 0, 255, 0, 255,
                                 0, 0, 255, 255, 255, 255, 255, 255};
         std::copy(px, px + 16, img.pixels.begin());
         c.drawImage(img, 1, 0, 1, 2, 10, 10, 40, 80); // right column only
       },
       [](const CanvasImage &im) {
         report("drawImage: source rectangle crops",
                pxNear(at(im, 30, 20), 0, 255, 0, 255, 6) &&
                    pxNear(at(im, 30, 80), 255, 255, 255, 255, 6) &&
                    at(im, 55, 20).a == 0,
                str(at(im, 30, 20)));
       });

  test(g, "putImageData then getImageData round trip",
       [](CanvasContext &c) {
         CanvasImage img(8, 8);
         for (int y = 0; y < 8; ++y)
           for (int x = 0; x < 8; ++x) {
             uint8_t *p = &img.pixels[(y * 8 + x) * 4];
             p[0] = static_cast<uint8_t>(x * 30);
             p[1] = static_cast<uint8_t>(y * 30);
             p[2] = 90;
             p[3] = (x + y) % 2 ? 255 : 128;
           }
         c.putImageData(img, 40, 40);
       },
       [&g](const CanvasImage &) {
         auto back = g.getImageData(40, 40, 8, 8);
         bool ok = back.has_value();
         int worst = 0;
         if (ok)
           for (int y = 0; y < 8; ++y)
             for (int x = 0; x < 8; ++x) {
               Px p = at(*back, x, y);
               int a = (x + y) % 2 ? 255 : 128;
               worst = std::max({worst, std::abs(p.r - x * 30),
                                 std::abs(p.g - y * 30), std::abs(p.b - 90),
                                 std::abs(p.a - a)});
             }
         char d[48];
         std::snprintf(d, sizeof(d), "worst channel error %d", worst);
         report("putImageData then getImageData round trip", ok && worst <= 4,
                d);
       });

  {
    g.clearRect(0, 0, W, H);
    auto off = g.getImageData(-4, -4, 8, 8);
    bool ok = off.has_value() && at(*off, 0, 0).a == 0 && at(*off, 7, 7).a == 0;
    report("getImageData: off-canvas area is transparent black", ok);
  }

  test(g, "shadow: offset copy beneath the shape",
       [](CanvasContext &c) {
         c.setShadow(kBlue, 0, 12, 12);
         c.setFillColor(kRed);
         c.fillRect(20, 20, 30, 30);
       },
       [](const CanvasImage &im) {
         report("shadow: offset copy beneath the shape",
                pxNear(at(im, 30, 30), 255, 0, 0, 255) &&
                    pxNear(at(im, 58, 58), 0, 0, 255, 255),
                str(at(im, 58, 58)));
       });

  // ---- composite operations --------------------------------------------
  auto composite = [&](const char *name, CompositeOp op, Color dst, Color src,
                       int r, int gr, int b, int a, bool supported) {
    if (!supported) {
      skip(name, "not supported by this backend");
      return;
    }
    test(g, name,
         [&](CanvasContext &c) {
           c.setFillColor(dst);
           c.fillRect(0, 0, W, H);
           c.setGlobalCompositeOperation(op);
           c.setFillColor(src);
           c.fillRect(20, 20, 60, 60);
         },
         [&](const CanvasImage &im) {
           report(name, pxNear(at(im, 50, 50), r, gr, b, a, 3),
                  str(at(im, 50, 50)));
         });
  };
#if defined(_WIN32)
  constexpr bool kAllOps = false; // Direct2D: SourceOver, Copy, Lighter only
#else
  constexpr bool kAllOps = true;
#endif
  composite("composite: lighter adds", CompositeOp::Lighter,
            Color{100, 0, 0, 255}, Color{100, 50, 0, 255}, 200, 50, 0, 255,
            true);
  composite("composite: copy replaces (incl. alpha)", CompositeOp::Copy, kRed,
            Color{0, 0, 255, 0}, 0, 0, 0, 0, true);
  composite("composite: destination-out erases", CompositeOp::DestinationOut,
            kRed, kBlack, 0, 0, 0, 0, kAllOps);
  composite("composite: source-atop keeps backdrop alpha",
            CompositeOp::SourceAtop, kBlue, kRed, 255, 0, 0, 255, kAllOps);
  composite("composite: xor of two opaque shapes is empty", CompositeOp::Xor,
            kBlack, kWhite, 0, 0, 0, 0, kAllOps);
  composite("composite: multiply grey x red", CompositeOp::Multiply,
            Color{128, 128, 128, 255}, kRed, 128, 0, 0, 255, kAllOps);
  composite("composite: screen grey + green", CompositeOp::Screen,
            Color{128, 128, 128, 255}, Color{0, 128, 0, 255}, 128, 192, 128,
            255, kAllOps);
  if (kAllOps) {
    // Each shape must see the result of the previous one, even within one
    // draw pass: 255 * 0.5 * 0.5 = 64. (A renderer that batched both shapes
    // into a single draw call would read the same backdrop for both and give
    // 128.)
    test(g, "composite: multiply applies once per overlapping shape",
         [](CanvasContext &c) {
           c.setFillColor(kBlue);
           c.fillRect(0, 0, W, H);
           c.setGlobalCompositeOperation(CompositeOp::Multiply);
           c.setFillColor(Color{128, 128, 128, 255});
           c.fillRect(20, 20, 60, 60);
           c.fillRect(20, 20, 60, 60);
         },
         [](const CanvasImage &im) {
           report("composite: multiply applies once per overlapping shape",
                  pxNear(at(im, 50, 50), 0, 0, 64, 255, 3), str(at(im, 50, 50)));
         });
  } else {
    skip("composite: multiply applies once per overlapping shape",
         "not supported by this backend");
  }

  // ---- text --------------------------------------------------------------
  {
    CanvasContext &c = g;
    c.setFont("sans-serif", 60);
    auto m = c.measureText("H");
    report("measureText returns a sensible width", m.width > 15 && m.width < 80,
           std::to_string(m.width));
  }
  test(g, "fillText: colour, position and baseline",
       [](CanvasContext &c) {
         c.setFont("sans-serif", 60);
         c.setFillColor(kRed);
         c.fillText("H", 20, 100);
       },
       [](const CanvasImage &im) {
         Box b = inkBox(im);
         // 'H' sits on the baseline (no descender) and starts near x = 20.
         bool pos = b.n > 200 && b.y1 >= 96 && b.y1 <= 104 && b.x0 >= 18 &&
                    b.x0 <= 32;
         // Solid ink must be the fill colour.
         bool colour = true;
         for (int y = b.y0; y <= b.y1 && colour; ++y)
           for (int x = b.x0; x <= b.x1; ++x) {
             Px p = at(im, x, y);
             if (p.a == 255 && !pxNear(p, 255, 0, 0, 255, 6)) {
               colour = false;
               break;
             }
           }
         char d[96];
         std::snprintf(d, sizeof(d), "ink box x%d..%d y%d..%d, %d px", b.x0,
                       b.x1, b.y0, b.y1, b.n);
         report("fillText: colour, position and baseline", pos && colour, d);
       });
  test(g, "fillText: textAlign center",
       [](CanvasContext &c) {
         c.setFont("sans-serif", 40);
         c.setFillColor(kBlack);
         c.setTextAlign(TextAlign::Center);
         c.fillText("HH", 80, 100);
       },
       [](const CanvasImage &im) {
         Box b = inkBox(im);
         int centre = (b.x0 + b.x1) / 2;
         char d[48];
         std::snprintf(d, sizeof(d), "ink centre x = %d (want ~80)", centre);
         report("fillText: textAlign center", b.n > 100 && std::abs(centre - 80) <= 8, d);
       });
  test(g, "fillText: honours globalAlpha and clip",
       [](CanvasContext &c) {
         c.setFont("sans-serif", 60);
         c.setFillColor(kBlack);
         c.setGlobalAlpha(0.5f);
         c.beginPath();
         c.rect(0, 0, 40, H);
         c.clip();
         c.fillText("HH", 20, 100);
       },
       [](const CanvasImage &im) {
         Box b = inkBox(im, 1);
         int maxA = 0;
         for (int y = 0; y < H; ++y)
           for (int x = 0; x < W; ++x)
             maxA = std::max(maxA, at(im, x, y).a);
         report("fillText: honours globalAlpha and clip",
                b.n > 50 && b.x1 < 40 && maxA >= 124 && maxA <= 132,
                "max alpha " + std::to_string(maxA));
       });

#if defined(LITEUI_CANVAS_GL)
  {
    // Only the GL backend strokes real glyph outlines; the others draw an
    // approximation (fill text nudged in a ring), which is much heavier.
    int fillN = 0;
    g.clearRect(0, 0, W, H);
    g.save();
    g.setFont("sans-serif", 90);
    g.setFillColor(kBlack);
    g.fillText("H", 40, 120);
    g.restore();
    if (auto im = g.getImageData(0, 0, W, H))
      fillN = countAlpha(*im, 128);
    g.clearRect(0, 0, W, H);
    g.save();
    g.setFont("sans-serif", 90);
    g.setStrokeColor(kBlack);
    g.setLineWidth(1);
    g.strokeText("H", 40, 120);
    g.restore();
    int strokeN = 0;
    if (auto im = g.getImageData(0, 0, W, H))
      strokeN = countAlpha(*im, 128);
    char d[80];
    std::snprintf(d, sizeof(d), "outline %d px vs solid %d px", strokeN, fillN);
    report("strokeText: traces the glyph outline (hollow)",
           fillN > 500 && strokeN > 50 && strokeN * 100 < fillN * 60, d);
  }
#else
  skip("strokeText: traces the glyph outline (hollow)",
       "this backend only approximates stroked text");
#endif

  {
    // Informational: does an emoji render in colour on this system?
    g.clearRect(0, 0, W, H);
    g.save();
    g.setFont("sans-serif", 60);
    g.setFillColor(kRed);
    g.fillText("\xF0\x9F\x98\x80", 30, 100); // U+1F600
    g.restore();
    if (auto im = g.getImageData(0, 0, W, H)) {
      int ink = countAlpha(*im, 200), nonRed = 0;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          Px p = at(*im, x, y);
          if (p.a >= 200 && (p.g > 100 || p.b > 100))
            ++nonRed;
        }
      if (ink == 0)
        info("emoji: nothing drawn (no font has U+1F600?)");
      else if (nonRed > 30)
        info("emoji: drawn in colour (" + std::to_string(nonRed) +
             " non-red pixels)");
      else
        info("emoji: drawn as a single-colour glyph (install an emoji font, "
             "e.g. fonts-noto-color-emoji, for colour)");
    }
  }
}

// A second canvas, checked independently of the first.
inline void runSecond(CanvasContext &g) {
  g.setFillColor(kGreen);
  g.fillRect(0, 0, g.width(), g.height());
  g.setFillColor(kBlue);
  g.beginPath();
  g.arc(32, 32, 16, 0, 2 * kPi);
  g.fill();
  auto im = g.getImageData(0, 0, 64, 64);
  bool ok = im && pxNear(at(*im, 32, 32), 0, 0, 255, 255) &&
            pxNear(at(*im, 2, 2), 0, 255, 0, 255);
  report("second canvas is independent of the first", ok,
         im ? str(at(*im, 32, 32)) : "getImageData failed");
}


} // namespace canvas_hw

namespace canvas_hw {

// Opens a window holding two canvases, lets them paint once (recording the
// results above), then closes it.
inline bool runWindow() {
  bool ranFirst = false, ranSecond = false;

  Canvas first;
  first.style.width = Size::pixel(W);
  first.style.height = Size::pixel(H);
  first.onPaint = [&](CanvasContext &g) {
    if (ranFirst)
      return; // repaints (e.g. after a resize) don't re-run the checks
    ranFirst = true;
    runAll(g);
  };

  Canvas second;
  second.style.width = Size::pixel(64);
  second.style.height = Size::pixel(64);
  second.onPaint = [&](CanvasContext &g) {
    if (ranSecond)
      return;
    ranSecond = true;
    runSecond(g);
  };

  View root;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(std::move(first));
  root.addChild(std::move(second));

  LiteUI ui("liteui canvas hardware test", 360, 220);
  ui.setRoot(std::move(root));

  int ticks = 0;
  ui.addInterval(100, [&]() {
    ++ticks;
    if (ranFirst && ranSecond)
      ui.forceClose();
    else if (ticks > 100) // ~10 s without a paint: no display?
      ui.forceClose();
  });
  ui.run();
  return ranFirst && ranSecond;
}

} // namespace canvas_hw

TEST_CASE("canvas hardware: drawing on the real backend", "[.][hardware][canvas]") {
  using namespace canvas_hw;
  static bool done = false; // Catch2 re-enters the test once per section
  static bool painted = false;
  if (!done) {
    done = true;
    painted = runWindow();
  }
  if (!painted) {
    FAIL("the canvases never painted (is a display available?)");
  }

  for (const Result &r : results()) {
    DYNAMIC_SECTION(r.name) {
      switch (r.kind) {
      case Result::Kind::Pass:
        SUCCEED();
        break;
      case Result::Kind::Fail:
        FAIL_CHECK(r.detail);
        break;
      case Result::Kind::Skip:
        WARN("SKIP: " << r.name << " (" << r.detail << ")");
        break;
      case Result::Kind::Info:
        WARN("INFO: " << r.detail);
        break;
      }
    }
  }
}
