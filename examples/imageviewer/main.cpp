// imageviewer — open an image and view it on a Canvas with wheel zoom,
// drag-to-pan and scrollbars. The image is centered, and wheel zoom is
// anchored at the pointer (the point under the cursor stays put).
// It also shows the high-precision image pipeline (ImageBuffer, RGBA16F /
// RGBA32F working formats, exposure and clamp display settings).
//
//   imageviewer [path/to/image.png|jpg]
//
// Second toolbar row (the third row is an info line):
//   Format  cycles the in-memory working format of the image: 8-bit,
//           FP16 (half the memory of FP32) or FP32. Files are always
//           decoded to 8-bit sRGB first; FP16/FP32 hold them as linear light.
//   EV -/+  exposure in stops (float working formats only: an 8-bit image is
//           already display-referred, so exposure is ignored there).
//   Clamp   how too-bright colours are clamped for the display: per channel
//           (simple clip) or "Keep hue" (scale r,g,b together).
//   HDR test  generates a scene-linear FP32 gradient reaching 8x white, so
//           exposure and clamp are easy to see (try EV -3 .. -1).

#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
  constexpr float kMinZoom = 0.05f;
  constexpr float kMaxZoom = 32.0f;
  constexpr float kZoomStep = 1.1f; // per wheel notch
  constexpr float kBar = kScrollbarThickness;
  constexpr float kMinThumb = 24.0f;
  constexpr float kEvStep = 0.5f;
  constexpr float kEvLimit = 8.0f;

  // Everything the UI callbacks share. Views are *copied* by addChild(), so
  // the callbacks capture this by reference from main() instead of owning it.
  struct ViewerState
  {
    // ---- Image pipeline ------------------------------------------------
    // master:  the best-quality pixels we have (RGBA8 for a file, RGBA32F for
    //          the HDR test). Format changes always start from here, so
    //          switching 8-bit -> FP16 -> 8-bit never degrades the image.
    // working: master converted to `format` (shares the pointer when the
    //          formats already match, so there is no second copy).
    // image:   the RGBA8 display copy that the Canvas actually draws.
    std::shared_ptr<const ImageBuffer> master;
    std::shared_ptr<const ImageBuffer> working;
    PixelFormat format = PixelFormat::RGBA16_FLOAT;
    DisplaySettings display; // exposure + clamp mode
    std::shared_ptr<CanvasImage> image; // null until an image is loaded
    // The backends cache the uploaded bitmap keyed on the pixel buffer's
    // identity, so a new display copy must never land on a freed one's
    // address before the next paint. Replaced copies wait here until the
    // paint that draws the new one has run.
    std::vector<std::shared_ptr<CanvasImage>> retired;
    bool needRebuild = false; // display copy is out of date (coalesced per frame)
    double lastBuildMs = 0;

    std::string title;
    std::string status = "No image loaded";
    float zoom = 1.0f; // 1.0 = one image pixel per screen pixel
    // Top-left of the visible part, in zoomed-image pixels. Only meaningful
    // on an axis where the zoomed image is larger than the view.
    float scrollX = 0, scrollY = 0;
    float viewW = 0, viewH = 0; // canvas size, updated from onLayout
    // Last pointer position over the canvas (canvas-local), used as the
    // wheel-zoom anchor. hasHover is false while the pointer is elsewhere.
    float hoverX = 0, hoverY = 0;
    bool hasHover = false;
    bool needFit = false;       // fit the new image once the view size is known
    bool dirty = false;         // the Canvas must repaint

    enum class Drag
    {
      None,
      Pan,
      VThumb,
      HThumb
    } drag = Drag::None;
    float dragX = 0, dragY = 0;             // pointer at press
    float dragScrollX = 0, dragScrollY = 0; // scroll at press
  };

  // Sizes derived from the state: where the image sits and how far it scrolls.
  struct Geometry
  {
    float cw = 0, ch = 0; // zoomed image size
    bool vbar = false, hbar = false;
    float availW = 0, availH = 0; // view minus the scrollbars showing
    float offX = 0, offY = 0;     // centering offset on an axis that fits
    float maxX = 0, maxY = 0;     // largest scroll offset
  };

  Geometry geometry(const ViewerState &s)
  {
    Geometry g;
    g.availW = s.viewW;
    g.availH = s.viewH;
    if (!s.image)
      return g;
    g.cw = static_cast<float>(s.image->width) * s.zoom;
    g.ch = static_cast<float>(s.image->height) * s.zoom;
    // A bar eats space on the other axis, which can make that bar necessary
    // too, so settle it in two passes. 0.5 px of slack avoids a bar
    // appearing from float rounding when the image exactly fits.
    for (int pass = 0; pass < 2; ++pass)
    {
      g.availW = std::max(0.0f, s.viewW - (g.vbar ? kBar : 0.0f));
      g.availH = std::max(0.0f, s.viewH - (g.hbar ? kBar : 0.0f));
      g.hbar = g.cw > g.availW + 0.5f;
      g.vbar = g.ch > g.availH + 0.5f;
    }
    g.availW = std::max(0.0f, s.viewW - (g.vbar ? kBar : 0.0f));
    g.availH = std::max(0.0f, s.viewH - (g.hbar ? kBar : 0.0f));
    g.maxX = std::max(0.0f, g.cw - g.availW);
    g.maxY = std::max(0.0f, g.ch - g.availH);
    g.offX = std::max(0.0f, (g.availW - g.cw) / 2.0f);
    g.offY = std::max(0.0f, (g.availH - g.ch) / 2.0f);
    return g;
  }

  void clampScroll(ViewerState &s)
  {
    Geometry g = geometry(s);
    s.scrollX = std::clamp(s.scrollX, 0.0f, g.maxX);
    s.scrollY = std::clamp(s.scrollY, 0.0f, g.maxY);
  }

  // Changes the zoom so that the image point under the canvas-local position
  // (ax, ay) stays under it. An axis on which the image fits stays centered
  // by itself (its scroll is clamped to 0).
  void zoomAbout(ViewerState &s, float newZoom, float ax, float ay)
  {
    if (!s.image)
      return;
    Geometry a = geometry(s);
    // Keep the anchor inside the image area, so a pointer over a scrollbar
    // or the toolbar-side edge still behaves sensibly.
    ax = std::clamp(ax, 0.0f, a.availW);
    ay = std::clamp(ay, 0.0f, a.availH);
    // Image-space point under the anchor. offX is 0 on an overflowing axis
    // and scrollX is 0 on a fitting one, so one formula covers both. It may
    // lie outside the image when the pointer is over the margin.
    float ix = (s.scrollX + ax - a.offX) / s.zoom;
    float iy = (s.scrollY + ay - a.offY) / s.zoom;

    s.zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);

    Geometry b = geometry(s);
    s.scrollX = ix * s.zoom - (ax - b.offX);
    s.scrollY = iy * s.zoom - (ay - b.offY);
    clampScroll(s);
    s.dirty = true;
  }

  // Zooms about the middle of the visible area (the 100% button).
  void zoomAboutCenter(ViewerState &s, float newZoom)
  {
    Geometry g = geometry(s);
    zoomAbout(s, newZoom, g.availW / 2.0f, g.availH / 2.0f);
  }

  // Wheel zoom: about the pointer when it is over the canvas, else the centre.
  void zoomAtPointer(ViewerState &s, float newZoom)
  {
    if (s.hasHover)
      zoomAbout(s, newZoom, s.hoverX, s.hoverY);
    else
      zoomAboutCenter(s, newZoom);
  }

  // Shrinks a too-big image to fit the view (never enlarges past 100%).
  void fitToView(ViewerState &s)
  {
    if (!s.image || s.viewW <= 0 || s.viewH <= 0)
      return;
    float fit = std::min({1.0f, s.viewW / static_cast<float>(s.image->width),
                          s.viewH / static_cast<float>(s.image->height)});
    s.zoom = std::max(kMinZoom, fit);
    s.scrollX = s.scrollY = 0;
    s.needFit = false;
    s.dirty = true;
  }

  std::string baseName(const std::string &path)
  {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
  }

  // ---- High-precision pipeline ------------------------------------------

  const char *formatName(PixelFormat f)
  {
    switch (f)
    {
    case PixelFormat::RGBA8_UNORM:
      return "8-bit";
    case PixelFormat::RGBA16_FLOAT:
      return "FP16";
    case PixelFormat::RGBA32_FLOAT:
      return "FP32";
    default:
      return "?";
    }
  }

  PixelFormat nextFormat(PixelFormat f)
  {
    switch (f)
    {
    case PixelFormat::RGBA8_UNORM:
      return PixelFormat::RGBA16_FLOAT;
    case PixelFormat::RGBA16_FLOAT:
      return PixelFormat::RGBA32_FLOAT;
    default:
      return PixelFormat::RGBA8_UNORM;
    }
  }

  // Exposure and clamp only apply to the float working formats; an 8-bit
  // image is already display-referred and toCanvasImage copies it unchanged.
  bool settingsAffectDisplay(PixelFormat f)
  {
    return f == PixelFormat::RGBA16_FLOAT || f == PixelFormat::RGBA32_FLOAT;
  }

  // Called when exposure or clamp changed. The setting is always stored (so it
  // is in effect once a float format is chosen again), but the full-image
  // display rebuild is only requested when it would change the pixels.
  void settingsChanged(ViewerState &s)
  {
    if (s.working && settingsAffectDisplay(s.working->format))
      s.needRebuild = true;
  }

  // master -> working (in the chosen format).
  void rebuildWorking(ViewerState &s)
  {
    if (!s.master)
      return;
    if (s.master->format == s.format)
    {
      s.working = s.master; // same format: share, don't copy
    }
    else
    {
      ImageBuffer converted;
      converted.format = s.format;
      if (!liteui_image::convertImage(*s.master, converted))
      {
        s.status = "Could not convert the image to " + std::string(formatName(s.format));
        return;
      }
      s.working = std::make_shared<const ImageBuffer>(std::move(converted));
    }
    s.needRebuild = true;
  }

  // working + display settings -> the RGBA8 copy the Canvas draws.
  void rebuildDisplay(ViewerState &s)
  {
    s.needRebuild = false;
    if (!s.working)
      return;
    auto t0 = std::chrono::steady_clock::now();
    auto disp = liteui_image::toCanvasImage(*s.working, s.display);
    if (!disp)
    {
      s.status = "Could not prepare the image for display";
      return;
    }
    s.lastBuildMs = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    // The new copy was allocated while the old one was alive; park the old
    // one until the next paint (see ViewerState::retired).
    if (s.image)
      s.retired.push_back(std::move(s.image));
    s.image = std::make_shared<CanvasImage>(std::move(*disp));
    s.dirty = true;
  }

  // Installs `img` as the new master and shows it, fitted to the view.
  void setMaster(ViewerState &s, std::shared_ptr<const ImageBuffer> img,
                 const std::string &title)
  {
    s.master = std::move(img);
    s.title = title;
    s.zoom = 1.0f;
    s.scrollX = s.scrollY = 0;
    rebuildWorking(s);
    rebuildDisplay(s); // now, so the size is known for fitting
    s.needFit = true;  // on the first layout if the view size isn't known yet
    fitToView(s);
    s.dirty = true;
  }

  // Decodes `path` into the state. On failure the previous image (if any)
  // stays on screen and only the status line changes.
  void loadImage(ViewerState &s, const std::string &path)
  {
    std::string err;
    auto decoded = liteui_image::decodeFile(path, &err);
    if (!decoded)
    {
      s.status = "Could not open " + baseName(path) + ": " + err;
      return;
    }
    auto buf = ImageBuffer::create(static_cast<uint32_t>(decoded->width),
                                   static_cast<uint32_t>(decoded->height),
                                   PixelFormat::RGBA8_UNORM);
    if (!buf || buf->pixels.size() != decoded->pixels.size())
    {
      s.status = "Could not open " + baseName(path) + ": out of memory";
      return;
    }
    std::memcpy(buf->pixels.data(), decoded->pixels.data(), decoded->pixels.size());
    setMaster(s, std::make_shared<const ImageBuffer>(std::move(*buf)), baseName(path));
  }

  // A scene-linear FP32 test image: intensity ramps 0 -> 8x white left to
  // right (so it needs negative exposure to see), while the colour shifts
  // from orange to magenta top to bottom, which makes the difference
  // between clipping per channel and keeping the hue obvious.
  void loadHdrTest(ViewerState &s)
  {
    constexpr uint32_t W = 1200, H = 800;
    auto buf = ImageBuffer::create(W, H, PixelFormat::RGBA32_FLOAT);
    if (!buf)
      return;
    for (uint32_t y = 0; y < H; ++y)
    {
      const float t = static_cast<float>(y) / (H - 1);
      const float c[3] = {1.0f, 0.55f * (1.0f - t) + 0.05f, 0.1f + 0.9f * t};
      for (uint32_t x = 0; x < W; ++x)
      {
        const float u = static_cast<float>(x) / (W - 1);
        const float intensity = 8.0f * u * u;
        const float px[4] = {c[0] * intensity, c[1] * intensity, c[2] * intensity, 1.0f};
        buf->setPixel(x, y, px);
      }
    }
    s.display = DisplaySettings{}; // start from neutral so the clipping is visible
    setMaster(s, std::make_shared<const ImageBuffer>(std::move(*buf)), "HDR test gradient");
  }

  // One line describing the image and the pipeline, for the second toolbar.
  std::string infoLine(const ViewerState &s)
  {
    if (!s.image || !s.working)
      return s.status;
    char ev[32];
    std::snprintf(ev, sizeof ev, "%+.1f", s.display.exposureEv);
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.1f", s.lastBuildMs);
    char mb[32];
    std::snprintf(mb, sizeof mb, "%.1f", static_cast<double>(s.working->byteSize()) / 1e6);
    std::string line = s.title + "  -  " + std::to_string(s.image->width) + " x " +
                       std::to_string(s.image->height) + " px  -  " +
                       formatName(s.working->format) + " " + mb + " MB";
    if (s.working->format == PixelFormat::RGBA8_UNORM)
      line += "  (8-bit: exposure ignored)";
    else
      line += std::string("  -  EV ") + ev;
    line += std::string("  -  display copy ") + ms + " ms";
    return line;
  }

  // Scrollbar thumb along a track of length `track` for content of length
  // `content` scrolled by `scroll` (0..maxScroll).
  struct Thumb
  {
    float pos, len;
  };
  Thumb thumbFor(float track, float content, float scroll, float maxScroll)
  {
    float len = std::clamp(track * track / std::max(1.0f, content), kMinThumb, track);
    float pos = maxScroll > 0 ? scroll / maxScroll * (track - len) : 0.0f;
    return {pos, len};
  }

  // Pointer handling: press decides what is being dragged; drag moves it.
  void pressAt(ViewerState &s, float x, float y)
  {
    s.drag = ViewerState::Drag::None;
    if (!s.image)
      return;
    Geometry g = geometry(s);
    if (g.vbar && x >= s.viewW - kBar && y < g.availH)
    {
      Thumb t = thumbFor(g.availH, g.ch, s.scrollY, g.maxY);
      if (y >= t.pos && y <= t.pos + t.len)
      {
        s.drag = ViewerState::Drag::VThumb;
        s.dragY = y;
        s.dragScrollY = s.scrollY;
      }
      else // click on the track: one page up/down
      {
        s.scrollY += (y < t.pos ? -g.availH : g.availH);
        clampScroll(s);
        s.dirty = true;
      }
      return;
    }
    if (g.hbar && y >= s.viewH - kBar && x < g.availW)
    {
      Thumb t = thumbFor(g.availW, g.cw, s.scrollX, g.maxX);
      if (x >= t.pos && x <= t.pos + t.len)
      {
        s.drag = ViewerState::Drag::HThumb;
        s.dragX = x;
        s.dragScrollX = s.scrollX;
      }
      else
      {
        s.scrollX += (x < t.pos ? -g.availW : g.availW);
        clampScroll(s);
        s.dirty = true;
      }
      return;
    }
    s.drag = ViewerState::Drag::Pan;
    s.dragX = x;
    s.dragY = y;
    s.dragScrollX = s.scrollX;
    s.dragScrollY = s.scrollY;
  }

  void dragTo(ViewerState &s, float x, float y)
  {
    if (!s.image || s.drag == ViewerState::Drag::None)
      return;
    Geometry g = geometry(s);
    switch (s.drag)
    {
    case ViewerState::Drag::Pan:
      s.scrollX = s.dragScrollX - (x - s.dragX);
      s.scrollY = s.dragScrollY - (y - s.dragY);
      break;
    case ViewerState::Drag::VThumb:
    {
      Thumb t = thumbFor(g.availH, g.ch, s.scrollY, g.maxY);
      float range = g.availH - t.len;
      if (range > 0)
        s.scrollY = s.dragScrollY + (y - s.dragY) / range * g.maxY;
      break;
    }
    case ViewerState::Drag::HThumb:
    {
      Thumb t = thumbFor(g.availW, g.cw, s.scrollX, g.maxX);
      float range = g.availW - t.len;
      if (range > 0)
        s.scrollX = s.dragScrollX + (x - s.dragX) / range * g.maxX;
      break;
    }
    default:
      break;
    }
    clampScroll(s);
    s.dirty = true;
  }

  void paintViewer(ViewerState &s, CanvasContext &ctx)
  {
    float w = ctx.width(), h = ctx.height();
    ctx.setFillColor(Color{30, 30, 30});
    ctx.fillRect(0, 0, w, h);

    if (!s.image)
    {
      ctx.setFillColor(Color{150, 150, 150});
      ctx.setFont("", 15);
      ctx.setTextAlign(TextAlign::Center);
      ctx.setTextBaseline(TextBaseline::Middle);
      ctx.fillText("Click \"Open Image...\" to choose a PNG or JPEG, or \"HDR test\".", w / 2, h / 2);
      return;
    }

    Geometry g = geometry(s);

    // Draw the whole image, scaled and shifted, clipped to the visible
    // area (the drawImage source-rect form doesn't clip to its destination).
    // Offsets are rounded so a 100% image stays pixel-exact.
    ctx.save();
    ctx.beginPath();
    ctx.rect(0, 0, g.availW, g.availH);
    ctx.clip();
    ctx.drawImage(*s.image, std::floor(g.offX) - std::round(s.scrollX),
                  std::floor(g.offY) - std::round(s.scrollY), g.cw, g.ch);
    ctx.restore();
    s.retired.clear(); // the backend's bitmap cache now refers to *s.image

    const Color track{45, 45, 48}, thumb{110, 110, 115};
    if (g.vbar)
    {
      ctx.setFillColor(track);
      ctx.fillRect(w - kBar, 0, kBar, g.availH);
      Thumb t = thumbFor(g.availH, g.ch, s.scrollY, g.maxY);
      ctx.setFillColor(thumb);
      ctx.beginPath();
      ctx.roundRect(w - kBar + 2, t.pos + 1, kBar - 4, t.len - 2, 3);
      ctx.fill();
    }
    if (g.hbar)
    {
      ctx.setFillColor(track);
      ctx.fillRect(0, h - kBar, g.availW, kBar);
      Thumb t = thumbFor(g.availW, g.cw, s.scrollX, g.maxX);
      ctx.setFillColor(thumb);
      ctx.beginPath();
      ctx.roundRect(t.pos + 1, h - kBar + 2, t.len - 2, kBar - 4, 3);
      ctx.fill();
    }
    if (g.vbar && g.hbar) // the corner where the two bars meet
    {
      ctx.setFillColor(track);
      ctx.fillRect(w - kBar, h - kBar, kBar, kBar);
    }
  }

  // A toolbar button whose label is recomputed every frame.
  View makeButton(std::function<std::string()> label, Color bg, Color hover,
                  std::function<void()> onClick)
  {
    Text t;
    t.label = std::move(label);
    t.fontSize = 14;
    t.color = Color{255, 255, 255};

    View b;
    b.style.padding = EdgeInsets{6, 14, 6, 14}; // top, right, bottom, left
    b.style.backgroundColor = bg;
    b.style.hoverColor = hover;
    b.style.borderRadius = 4.0f;
    b.style.flexShrink = 0; // never squeeze a button below its label
    b.onClick = std::move(onClick);
    b.addChild(t);
    return b;
  }

  View makeToolbar(float height)
  {
    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.gap = 10;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(height);
    bar.style.flexShrink = 0;
    bar.style.padding = EdgeInsets{0, 12, 0, 12};
    bar.style.backgroundColor = Color{45, 45, 48};
    return bar;
  }
} // namespace

int main(int argc, char **argv)
{
  ViewerState state;
  if (argc > 1)
    loadImage(state, argv[1]);

  const Color blue{0, 110, 200}, blueHover{20, 130, 225};
  const Color grey{70, 70, 75}, greyHover{90, 90, 96};

  // ---- Toolbar 1: [Open Image...] [HDR test] [100%]  status text -----------
  View toolbar = makeToolbar(48);
  toolbar.addChild(makeButton([]
                              { return std::string("Open Image..."); },
                              blue, blueHover, [&state]
                              {
    // Modal and synchronous, so it is safe to call from a click handler.
    auto path = openFilePicker("Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}});
    if (path)
      loadImage(state, *path); }));
  toolbar.addChild(makeButton([]
                              { return std::string("HDR test"); },
                              grey, greyHover, [&state]
                              { loadHdrTest(state); }));
  toolbar.addChild(makeButton([]
                              { return std::string("100%"); },
                              grey, greyHover, [&state]
                              { zoomAboutCenter(state, 1.0f); }));

  Text statusText;
  statusText.label = std::function<std::string()>(
      [&state]
      {
        if (!state.image)
          return state.status;
        return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
      });
  statusText.fontSize = 14;
  statusText.color = Color{200, 200, 200};
  toolbar.addChild(statusText);

  // ---- Toolbar 2: [Format] [EV -] [EV +] [EV 0] [Clamp] --------------------
  View toolbar2 = makeToolbar(42);
  toolbar2.addChild(makeButton(
      [&state]
      { return std::string("Format: ") + formatName(state.format); },
      grey, greyHover, [&state]
      {
        state.format = nextFormat(state.format);
        rebuildWorking(state); }));
  toolbar2.addChild(makeButton([]
                               { return std::string("EV -"); },
                               grey, greyHover, [&state]
                               {
        state.display.exposureEv = std::max(-kEvLimit, state.display.exposureEv - kEvStep);
        settingsChanged(state); }));
  // Current exposure, between the - and + buttons. A fixed-width box keeps the
  // neighbouring buttons from shifting as the number changes ("+0.5" -> "-10.0").
  {
    Text evText;
    evText.label = std::function<std::string()>(
        [&state]
        {
          char buf[32];
          std::snprintf(buf, sizeof buf, "%+.1f EV", state.display.exposureEv);
          return std::string(buf);
        });
    evText.fontSize = 14;
    evText.color = Color{255, 255, 255};

    View evBox;
    evBox.style.direction = FlexDirection::Row;
    evBox.style.justifyContent = Justify::Center;
    evBox.style.alignItems = Align::Center;
    evBox.style.width = Size::pixel(72);
    evBox.style.flexShrink = 0;
    evBox.addChild(evText);
    toolbar2.addChild(std::move(evBox));
  }
  toolbar2.addChild(makeButton([]
                               { return std::string("EV +"); },
                               grey, greyHover, [&state]
                               {
        state.display.exposureEv = std::min(kEvLimit, state.display.exposureEv + kEvStep);
        settingsChanged(state); }));
  toolbar2.addChild(makeButton([]
                               { return std::string("EV 0"); },
                               grey, greyHover, [&state]
                               {
        state.display.exposureEv = 0.0f;
        settingsChanged(state); }));
  toolbar2.addChild(makeButton(
      [&state]
      {
        return std::string("Clamp: ") +
               (state.display.clamp == ClampMode::PerChannel ? "clip" : "keep hue");
      },
      grey, greyHover, [&state]
      {
        state.display.clamp = state.display.clamp == ClampMode::PerChannel
                                  ? ClampMode::PreserveHue
                                  : ClampMode::PerChannel;
        settingsChanged(state); }));

  // ---- Info row: the long pipeline description gets a row of its own, so
  // it can't squeeze the controls above it. ------------------------------
  View infoBar = makeToolbar(28);
  Text infoText;
  infoText.label = std::function<std::string()>([&state]
                                                { return infoLine(state); });
  infoText.fontSize = 13;
  infoText.color = Color{200, 200, 200};
  infoBar.addChild(infoText);

  // ---- The Canvas: fills everything under the toolbars --------------------
  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = Size::full();
  canvas.style.flexGrow = 1;
  canvas.onPaint = [&state](CanvasContext &ctx)
  { paintViewer(state, ctx); };
  // Polled once per frame; returning true re-runs onPaint. Several clicks on
  // the exposure buttons within one frame cost a single display rebuild.
  canvas.canvasDirtySource = [&state]
  {
    if (state.needRebuild)
      rebuildDisplay(state);
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };
  // Fires on every relayout (window resize included) with the final size.
  canvas.onLayout = [&state](float, float, float w, float h)
  {
    bool resized = w != state.viewW || h != state.viewH;
    state.viewW = w;
    state.viewH = h;
    if (state.needFit)
      fitToView(state);
    else if (resized)
      clampScroll(state);
  };
  canvas.onScrollUp = [&state]
  { zoomAtPointer(state, state.zoom * kZoomStep); };
  canvas.onScrollDown = [&state]
  { zoomAtPointer(state, state.zoom / kZoomStep); };
  // Track the pointer for cursor-anchored zoom. Return false: moving the
  // mouse changes nothing visible, so it must not trigger a repaint.
  canvas.onHoverAt = [&state](float x, float y)
  {
    state.hoverX = x;
    state.hoverY = y;
    state.hasHover = true;
    return false;
  };
  canvas.onHoverEnd = [&state]
  {
    state.hasHover = false;
    return false;
  };
  canvas.onPressAt = [&state](float x, float y)
  { pressAt(state, x, y); };
  canvas.onDragTo = [&state](float x, float y)
  { dragTo(state, x, y); };
  canvas.onDragEnd = [&state]
  { state.drag = ViewerState::Drag::None; };

  // ---- Window -------------------------------------------------------------
  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.addChild(std::move(toolbar));
  root.addChild(std::move(toolbar2));
  root.addChild(std::move(infoBar));
  root.addChild(std::move(canvas));

  LiteUI ui("Image Viewer", 1000, 700);
  ui.setRoot(root);
  ui.run(); // blocks until the window closes
  return 0;
}