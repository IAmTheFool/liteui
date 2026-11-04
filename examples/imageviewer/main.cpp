// imageviewer — open an image and view it on a Canvas with wheel zoom,
// drag-to-pan and scrollbars. The image is centered, and zoom is centered.
//
//   imageviewer [path/to/image.png|jpg]



#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace
{
  constexpr float kMinZoom = 0.05f;
  constexpr float kMaxZoom = 32.0f;
  constexpr float kZoomStep = 1.1f; // per wheel notch
  constexpr float kBar = kScrollbarThickness;
  constexpr float kMinThumb = 24.0f;

  // Everything the UI callbacks share. Views are *copied* by addChild(), so
  // the callbacks capture this by reference from main() instead of owning it.
  struct ViewerState
  {
    std::shared_ptr<CanvasImage> image; // null until an image is loaded
    std::string status = "No image loaded";
    float zoom = 1.0f; // 1.0 = one image pixel per screen pixel
    // Top-left of the visible part, in zoomed-image pixels. Only meaningful
    // on an axis where the zoomed image is larger than the view.
    float scrollX = 0, scrollY = 0;
    float viewW = 0, viewH = 0; // canvas size, updated from onLayout
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

  // Changes the zoom so that the point at the middle of the visible area
  // stays at the middle (an image that fits stays centered by itself).
  void zoomAboutCenter(ViewerState &s, float newZoom)
  {
    if (!s.image)
      return;
    Geometry a = geometry(s);
    // The image-space point currently at the middle of the visible area.
    float cx = a.cw <= a.availW
                   ? static_cast<float>(s.image->width) / 2.0f
                   : (s.scrollX + a.availW / 2.0f) / s.zoom;
    float cy = a.ch <= a.availH
                   ? static_cast<float>(s.image->height) / 2.0f
                   : (s.scrollY + a.availH / 2.0f) / s.zoom;

    s.zoom = std::clamp(newZoom, kMinZoom, kMaxZoom);

    Geometry b = geometry(s);
    s.scrollX = b.cw <= b.availW ? 0.0f : cx * s.zoom - b.availW / 2.0f;
    s.scrollY = b.ch <= b.availH ? 0.0f : cy * s.zoom - b.availH / 2.0f;
    clampScroll(s);
    s.dirty = true;
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
    s.image = std::make_shared<CanvasImage>(std::move(*decoded));
    s.status = baseName(path) + "  -  " + std::to_string(s.image->width) +
               " x " + std::to_string(s.image->height) + " px";
    s.zoom = 1.0f;
    s.scrollX = s.scrollY = 0;
    s.needFit = true; // on the first layout if the view size isn't known yet
    fitToView(s);
    s.dirty = true;
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
      ctx.fillText("Click \"Open Image...\" to choose a PNG or JPEG.", w / 2, h / 2);
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
} // namespace

int main(int argc, char **argv)
{
  ViewerState state;
  if (argc > 1)
    loadImage(state, argv[1]);

  // ---- Toolbar: [Open Image...] [100%]  status text ---------------------
  Text buttonLabel;
  buttonLabel.label = std::string("Open Image...");
  buttonLabel.fontSize = 14;
  buttonLabel.color = Color{255, 255, 255};

  View openButton;
  openButton.style.padding = EdgeInsets{6, 14, 6, 14}; // top, right, bottom, left
  openButton.style.backgroundColor = Color{0, 110, 200};
  openButton.style.hoverColor = Color{20, 130, 225};
  openButton.style.borderRadius = 4.0f;
  openButton.onClick = [&state]
  {
    // Modal and synchronous, so it is safe to call from a click handler.
    auto path = openFilePicker("Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}});
    if (path)
      loadImage(state, *path);
  };
  openButton.addChild(buttonLabel);

  Text resetLabel;
  resetLabel.label = std::string("100%");
  resetLabel.fontSize = 14;
  resetLabel.color = Color{255, 255, 255};

  View resetButton;
  resetButton.style.padding = EdgeInsets{6, 14, 6, 14};
  resetButton.style.backgroundColor = Color{70, 70, 75};
  resetButton.style.hoverColor = Color{90, 90, 96};
  resetButton.style.borderRadius = 4.0f;
  resetButton.onClick = [&state]
  { zoomAboutCenter(state, 1.0f); };
  resetButton.addChild(resetLabel);

  Text statusText;
  statusText.label = std::function<std::string()>(
      [&state]
      {
        if (!state.image)
          return state.status;
        return state.status + "  -  " +
               std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
      });
  statusText.fontSize = 14;
  statusText.color = Color{200, 200, 200};

  View toolbar;
  toolbar.style.direction = FlexDirection::Row;
  toolbar.style.alignItems = Align::Center;
  toolbar.style.gap = 12;
  toolbar.style.width = Size::full();
  toolbar.style.height = Size::pixel(48);
  toolbar.style.flexShrink = 0;
  toolbar.style.padding = EdgeInsets{0, 12, 0, 12};
  toolbar.style.backgroundColor = Color{45, 45, 48};
  toolbar.addChild(std::move(openButton));
  toolbar.addChild(std::move(resetButton));
  toolbar.addChild(statusText);

  // ---- The Canvas: fills everything under the toolbar ---------------------
  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = Size::full();
  canvas.style.flexGrow = 1;
  canvas.onPaint = [&state](CanvasContext &ctx)
  { paintViewer(state, ctx); };
  // Polled once per frame; returning true re-runs onPaint.
  canvas.canvasDirtySource = [&state]
  {
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
  { zoomAboutCenter(state, state.zoom * kZoomStep); };
  canvas.onScrollDown = [&state]
  { zoomAboutCenter(state, state.zoom / kZoomStep); };
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
  root.addChild(std::move(canvas));

  LiteUI ui("Image Viewer", 900, 650);
  ui.setRoot(root);
  ui.run(); // blocks until the window closes
  return 0;
}