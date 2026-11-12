#pragma once
// A software stand-in for liteui_canvas_gl::Renderer, used by
// cases/canvas_gl_context.inc to test the GL canvas context (paths, strokes,
// clips, gradients, images, text, blending) without a GPU or a window.
//
// It implements the same interface the context calls, and emulates what the
// real renderer asks the GPU to do:
//   * the stencil modes of Renderer::setMode (clip level in bits 7..4,
//     scratch/winding count in bits 3..0) on a per-pixel stencil array;
//   * the fragment shader's paint maths (solid, image, linear/radial
//     gradient, glyph masks) evaluated per pixel;
//   * blending through the real blendFactorsFor(), plus the Multiply shader
//     formula, on premultiplied float colour;
//   * triangle rasterization by pixel-centre sampling (no antialiasing).
// Text is faked: rasterizeText returns a solid bar per character (6 px per
// character, ink rows 2..9 of a 12 px line, baseline at 10) and
// outlineText returns one 4x8 rectangle per character, so the *placement
// logic* can be checked exactly without depending on installed fonts.
//
// Included from liteui_tests.cpp after liteui.hpp. Only meaningful when the
// GL canvas backend is compiled in (Linux, LITEUI_CANVAS_GL).

#if defined(LITEUI_CANVAS_GL) && !defined(_WIN32)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cgl = liteui_canvas_gl;

namespace canvas_gl_test {

struct MockGl {
  enum class Mode {
    Off,
    ClipTest,
    CountWinding,
    CountEvenOdd,
    CountUnion,
    CoverFill,
    CoverClip,
    ReduceClip
  };
  enum class PaintKind { Solid, Texture, Gradient };

  // ---- observable state ------------------------------------------------
  int W = 0, H = 0;
  std::vector<uint8_t> stencil;
  std::vector<float> col;   // premultiplied RGBA, 0..1, 4 floats per pixel
  std::vector<float> alpha; // alpha channel of `col`, for quick checks
  int uploads = 0;          // textures uploaded (cache tests)
  std::vector<cgl::Vertex> lastDraw; // vertices of the most recent draw call
  std::vector<GLuint> deleted;

  // Counters for the faked glyph rasterizer (shared by all instances).
  static inline int rasterCalls = 0;
  static inline int lastBake = -1; // 1 if the last raster was colour-baked
  static inline float lastScale = 0;
  static void resetCounters() {
    rasterCalls = 0;
    lastBake = -1;
    lastScale = 0;
  }

  MockGl(int w, int h) : W(w), H(h) {
    stencil.assign(static_cast<size_t>(w) * h, 0);
    col.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    alpha.assign(static_cast<size_t>(w) * h, 0.0f);
  }

  // ---- renderer interface used by GlCanvasContext ------------------------
  void setBlend(CompositeOp o) { op_ = o; }
  void setMask(int m) { mask_ = m; }
  void setPaintSolid() { paint_ = PaintKind::Solid; }
  void setPaintTexture(GLuint t) {
    paint_ = PaintKind::Texture;
    tex_ = t;
  }
  void setPaintGradient(const CanvasGradient &g, const float inv[9]) {
    paint_ = PaintKind::Gradient;
    grad_ = g;
    std::copy(inv, inv + 9, inv_);
    ramp_.resize(static_cast<size_t>(cgl::kRampSize) * 4);
    cgl::buildRamp(g, ramp_.data());
  }
  void setMode(Mode m, int level) {
    if (m == Mode::ClipTest && level == 0)
      m = Mode::Off; // same state, as in the real renderer
    mode_ = m;
    level_ = level;
  }
  void reduceClip(int level) {
    setMode(Mode::ReduceClip, level);
    const float w = static_cast<float>(W), h = static_cast<float>(H);
    const cgl::Vertex q[6] = {{0, 0, 0, 0, 0, 0}, {w, 0, 0, 0, 0, 0},
                              {w, h, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0},
                              {w, h, 0, 0, 0, 0}, {0, h, 0, 0, 0, 0}};
    draw(q, 6);
  }

  GLuint uploadImage(const CanvasImage &img) {
    ++uploads;
    std::vector<uint8_t> p;
    cgl::premultiplyImage(img, p);
    texPx_.push_back(std::move(p));
    texSize_.push_back({img.width, img.height});
    return static_cast<GLuint>(texPx_.size());
  }
  void deferDelete(GLuint t) { deleted.push_back(t); }
  void readback(const View::Computed &, int x, int y, int w, int h,
                uint8_t *dst) {
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        for (int k = 0; k < 4; ++k)
          dst[(r * w + c) * 4 + k] = static_cast<uint8_t>(std::lround(
              std::clamp(col[(static_cast<size_t>(y + r) * W + x + c) * 4 + k],
                         0.0f, 1.0f) *
              255));
  }

  bool findText(const std::string &key, cgl::TextEntry &e) {
    auto it = rasters_.find(key);
    if (it == rasters_.end())
      return false;
    curKey_ = key;
    fillEntry(it->second, e);
    return true;
  }
  bool addText(const std::string &key, const cgl::RasterText &rt,
               cgl::TextEntry &e) {
    rasters_[key] = rt;
    curKey_ = key;
    fillEntry(rt, e);
    return true;
  }

  // Fake glyph bitmaps (see the header comment).
  static bool rasterizeText(const std::string &text, const TextStyle &,
                            float scale, const Color *bake,
                            cgl::RasterText &out) {
    ++rasterCalls;
    lastScale = scale;
    lastBake = bake ? 1 : 0;
    out = cgl::RasterText{};
    const size_t n = text.size();
    out.advance = 6.0f * static_cast<float>(n);
    out.height = 12;
    out.baseline = 10;
    if (text.find_first_not_of(' ') == std::string::npos)
      return true; // only spaces: no ink
    const int pad = 2;
    const int iw = static_cast<int>(std::ceil((6 * n - 2) * scale));
    const int ih = static_cast<int>(std::ceil(8 * scale));
    const int y0 = static_cast<int>(std::floor(2 * scale)); // ink rows 2..10
    out.w = iw + 2 * pad;
    out.h = ih + 2 * pad;
    if (bake) {
      // "Colour" raster: left half in the baked fill colour, right half a
      // pretend emoji colour (opaque green).
      out.color = true;
      out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
      const float a = bake->a / 255.0f;
      for (int y = pad; y < pad + ih; ++y)
        for (int x = pad; x < pad + iw; ++x) {
          uint8_t *p = &out.rgba[(static_cast<size_t>(y) * out.w + x) * 4];
          if (x < pad + iw / 2) {
            p[0] = static_cast<uint8_t>(std::lround(bake->r * a));
            p[1] = static_cast<uint8_t>(std::lround(bake->g * a));
            p[2] = static_cast<uint8_t>(std::lround(bake->b * a));
            p[3] = bake->a;
          } else {
            p[0] = 0;
            p[1] = 255;
            p[2] = 0;
            p[3] = 255;
          }
        }
    } else {
      out.a8.assign(static_cast<size_t>(out.w) * out.h, 0);
      for (int y = pad; y < pad + ih; ++y)
        for (int x = pad; x < pad + iw; ++x)
          out.a8[static_cast<size_t>(y) * out.w + x] = 255;
    }
    out.offX = static_cast<float>(0 - pad) / scale;
    out.offY = static_cast<float>(y0 - pad) / scale;
    return true;
  }
  static bool outlineText(const std::string &text, const TextStyle &, float,
                          std::vector<cgl::OutlineContour> &out,
                          cgl::TextBox &box) {
    box = {6.0f * static_cast<float>(text.size()), 12, 10};
    out.clear();
    for (size_t i = 0; i < text.size(); ++i) {
      cgl::OutlineContour c;
      c.closed = true;
      const float x = 6.0f * static_cast<float>(i);
      c.xy = {x, 2, x + 4, 2, x + 4, 10, x, 10};
      out.push_back(c);
    }
    return true;
  }

  // Triangle rasterization by pixel-centre sampling.
  void draw(const cgl::Vertex *v, size_t n) {
    lastDraw.assign(v, v + n);
    for (size_t t = 0; t + 2 < n; t += 3) {
      const cgl::Vertex &a = v[t], &b = v[t + 1], &c = v[t + 2];
      const float area =
          (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
      if (area == 0)
        continue;
      const bool front = area > 0;
      for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
          // A tiny bias keeps pixel centres off exact triangle edges (real
          // GPUs use a top-left rule to the same end).
          const float X = static_cast<float>(x) + 0.5f + 1e-3f;
          const float Y = static_cast<float>(y) + 0.5f + 2e-3f;
          const float w0 = (b.x - a.x) * (Y - a.y) - (b.y - a.y) * (X - a.x);
          const float w1 = (c.x - b.x) * (Y - b.y) - (c.y - b.y) * (X - b.x);
          const float w2 = (a.x - c.x) * (Y - c.y) - (a.y - c.y) * (X - c.x);
          const bool in = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                   : (w0 <= 0 && w1 <= 0 && w2 <= 0);
          if (!in)
            continue;
          const float sum = w0 + w1 + w2;
          const float la = w1 / sum, lb = w2 / sum, lc = w0 / sum;
          float s[4];
          shade(X, Y, la * a.u + lb * b.u + lc * c.u,
                la * a.v + lb * b.v + lc * c.v, a, s);
          pixel(y * W + x, front, s);
        }
    }
  }

private:
  static void fillEntry(const cgl::RasterText &rt, cgl::TextEntry &e) {
    e = cgl::TextEntry{};
    e.u1 = e.v1 = 1; // the mock keeps each bitmap whole: uv spans 0..1
    e.w = rt.w;
    e.h = rt.h;
    e.color = rt.color;
    e.offX = rt.offX;
    e.offY = rt.offY;
    e.advance = rt.advance;
    e.height = rt.height;
    e.baseline = rt.baseline;
  }

  // The fragment shader, line for line (premultiplied output in s[0..3]).
  void shade(float X, float Y, float u, float v, const cgl::Vertex &vc,
             float s[4]) {
    s[0] = s[1] = s[2] = s[3] = 1.0f;
    if (paint_ == PaintKind::Texture) {
      const auto [tw, th] = texSize_[tex_ - 1];
      const auto &p = texPx_[tex_ - 1];
      const int ix = std::clamp(static_cast<int>(std::floor(u * tw)), 0, tw - 1);
      const int iy = std::clamp(static_cast<int>(std::floor(v * th)), 0, th - 1);
      for (int k = 0; k < 4; ++k)
        s[k] = p[(static_cast<size_t>(iy) * tw + ix) * 4 + k] / 255.0f;
    } else if (paint_ == PaintKind::Gradient) {
      const float px = inv_[0] * X + inv_[3] * Y + inv_[6];
      const float py = inv_[1] * X + inv_[4] * Y + inv_[7];
      float t = 0;
      bool ok = false;
      if (grad_.kind == CanvasGradient::Kind::Linear) {
        const float dx = grad_.x1 - grad_.x0, dy = grad_.y1 - grad_.y0;
        const float dd = dx * dx + dy * dy;
        if (dd > 0) {
          t = ((px - grad_.x0) * dx + (py - grad_.y0) * dy) / dd;
          ok = true;
        }
      } else {
        const float cdx = grad_.x1 - grad_.x0, cdy = grad_.y1 - grad_.y0;
        const float dr = grad_.r1 - grad_.r0;
        const float pdx = px - grad_.x0, pdy = py - grad_.y0;
        const float a = cdx * cdx + cdy * cdy - dr * dr;
        const float b = pdx * cdx + pdy * cdy + grad_.r0 * dr;
        const float c = pdx * pdx + pdy * pdy - grad_.r0 * grad_.r0;
        if (std::fabs(a) < 1e-6f) {
          if (std::fabs(b) > 1e-6f) {
            t = c / (2 * b);
            ok = grad_.r0 + t * dr >= 0;
          }
        } else {
          const float disc = b * b - a * c;
          if (disc >= 0) {
            const float sq = std::sqrt(disc);
            const float s1 = (b + sq) / a, s2 = (b - sq) / a;
            const float hi = std::max(s1, s2), lo = std::min(s1, s2);
            if (grad_.r0 + hi * dr >= 0) {
              t = hi;
              ok = true;
            } else if (grad_.r0 + lo * dr >= 0) {
              t = lo;
              ok = true;
            }
          }
        }
      }
      if (ok) {
        const float n = static_cast<float>(cgl::kRampSize);
        const float tx = (std::clamp(t, 0.0f, 1.0f) * (n - 1) + 0.5f) / n;
        const int i = std::clamp(static_cast<int>(std::floor(tx * n)), 0,
                                 cgl::kRampSize - 1);
        for (int k = 0; k < 4; ++k)
          s[k] = ramp_[static_cast<size_t>(i) * 4 + k] / 255.0f;
      } else {
        s[0] = s[1] = s[2] = s[3] = 0;
      }
    }
    if (mask_ != 0) {
      const cgl::RasterText &rt = rasters_[curKey_];
      const int ix =
          std::clamp(static_cast<int>(std::floor(u * rt.w)), 0, rt.w - 1);
      const int iy =
          std::clamp(static_cast<int>(std::floor(v * rt.h)), 0, rt.h - 1);
      if (mask_ == 1) {
        const float cov = rt.a8[static_cast<size_t>(iy) * rt.w + ix] / 255.0f;
        for (int k = 0; k < 4; ++k)
          s[k] *= cov;
      } else {
        const uint8_t *p = &rt.rgba[(static_cast<size_t>(iy) * rt.w + ix) * 4];
        if (mask_ == 2) {
          for (int k = 0; k < 4; ++k)
            s[k] = p[k] / 255.0f;
        } else { // 3: the colour glyph's silhouette
          const float a = p[3] / 255.0f;
          for (int k = 0; k < 4; ++k)
            s[k] *= a;
        }
      }
    }
    s[0] *= vc.r;
    s[1] *= vc.g;
    s[2] *= vc.b;
    s[3] *= vc.a;
  }

  static float factor(cgl::BlendFactor f, const float *s, const float *d,
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

  void blendInto(int i, const float s[4]) {
    float *d = &col[static_cast<size_t>(i) * 4];
    float o[4];
    if (op_ == CompositeOp::Multiply) {
      // The shader's Multiply; blending is "replace".
      for (int k = 0; k < 3; ++k)
        o[k] = s[k] * (1 - d[3]) + d[k] * (1 - s[3]) + s[k] * d[k];
      o[3] = s[3] + d[3] - s[3] * d[3];
    } else {
      const cgl::BlendFactors f = cgl::blendFactorsFor(op_);
      for (int k = 0; k < 4; ++k) {
        const cgl::BlendFactor sf = k < 3 ? f.srcRgb : f.srcA;
        const cgl::BlendFactor df = k < 3 ? f.dstRgb : f.dstA;
        o[k] = s[k] * factor(sf, s, d, k) + d[k] * factor(df, s, d, k);
      }
    }
    for (int k = 0; k < 4; ++k)
      d[k] = std::clamp(o[k], 0.0f, 1.0f); // fixed-point targets clamp
    alpha[static_cast<size_t>(i)] = d[3];
  }

  // What the GPU does with one fragment in each stencil mode.
  void pixel(int i, bool front, const float s[4]) {
    uint8_t &sv = stencil[static_cast<size_t>(i)];
    switch (mode_) {
    case Mode::Off:
      blendInto(i, s);
      break;
    case Mode::ClipTest:
      if ((sv & 0xF0) == (level_ << 4))
        blendInto(i, s);
      break;
    case Mode::CountWinding:
    case Mode::CountEvenOdd:
    case Mode::CountUnion:
      if ((sv & 0xF0) == (level_ << 4)) {
        uint8_t lo = sv & 0x0F;
        if (mode_ == Mode::CountEvenOdd)
          lo ^= 0x0F;
        else if (mode_ == Mode::CountUnion)
          lo = 1; // REPLACE with the flag: idempotent, never wraps
        else
          lo = front ? ((lo + 1) & 0x0F) : ((lo - 1) & 0x0F);
        sv = static_cast<uint8_t>((sv & 0xF0) | lo);
      }
      break;
    case Mode::CoverFill:
      if (sv & 0x0F) {
        blendInto(i, s);
        sv &= 0xF0;
      }
      break;
    case Mode::CoverClip:
      if (sv & 0x0F)
        sv = static_cast<uint8_t>((level_ + 1) << 4);
      break;
    case Mode::ReduceClip:
      if ((sv & 0xF0) > (level_ << 4))
        sv = static_cast<uint8_t>(level_ << 4);
      break;
    }
  }

  CompositeOp op_ = CompositeOp::SourceOver;
  Mode mode_ = Mode::Off;
  int level_ = 0;
  int mask_ = 0;
  PaintKind paint_ = PaintKind::Solid;
  GLuint tex_ = 0;
  CanvasGradient grad_;
  float inv_[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  std::vector<uint8_t> ramp_;
  std::vector<std::vector<uint8_t>> texPx_;
  std::vector<std::pair<int, int>> texSize_;
  std::unordered_map<std::string, cgl::RasterText> rasters_;
  std::string curKey_;
};

} // namespace canvas_gl_test

#endif // LITEUI_CANVAS_GL && !_WIN32
