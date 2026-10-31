# Canvas

`Canvas` is LiteUI's escape hatch for custom drawing: charts, gauges, paint programs, image viewers, custom controls. You get a `CanvasContext` modeled on the HTML5 2D context (paths, transforms, gradients, text, images, pixel access) and draw whatever you like into a rectangle that takes part in normal layout.

This guide covers the mental model, how and when your drawing code runs, handling input, the full drawing API, and the places where the Windows (Direct2D) and Linux (Cairo) backends behave differently.

- [Mental model](#mental-model)
- [Quick start](#quick-start)
- [The drawing surface](#the-drawing-surface)
- [When `onPaint` runs](#when-onpaint-runs)
- [Input and focus](#input-and-focus)
- [Context state and defaults](#context-state-and-defaults)
- [Paths and drawing](#paths-and-drawing)
- [Gradients, alpha, shadows and compositing](#gradients-alpha-shadows-and-compositing)
- [Text](#text)
- [Images and pixels](#images-and-pixels)
- [Platform differences](#platform-differences)
- [Canvas-backed built-ins](#canvas-backed-built-ins)
- [Recipes](#recipes)
- [Performance notes](#performance-notes)
- [Not available](#not-available)

## Mental model

A canvas is a **retained bitmap**. LiteUI keeps an offscreen surface for each canvas node and draws it into the window like any other box. Your `onPaint` callback is **not** a per-frame draw loop. It is called when the picture needs rebuilding, and the result is cached until the next time it does.

Treat `onPaint` as: *"rebuild my picture from the app state I close over."* Keep your data (strokes, values, an image) in your own variables, mutate those from event handlers, and tell LiteUI the picture is stale with a dirty flag. Do not draw from event handlers, because there is no context to draw into outside `onPaint`.

## Quick start

```cpp
#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

int main()
{
    Canvas c;
    c.style.width  = Size::pixel(240);
    c.style.height = Size::pixel(240);
    c.onPaint = [](CanvasContext &g) {
        g.setFillColor(Color{220, 80, 60});
        g.beginPath();
        g.arc(g.width() / 2, g.height() / 2, 80, 0, 6.2831853f);
        g.fill();
    };

    View root;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.style.alignItems = Align::Center;
    root.style.justifyContent = Justify::Center;
    root.addChild(c);

    LiteUI ui("Canvas", 400, 320);
    ui.setRoot(root);
    ui.run();
}
```

A `Canvas` has a `Style` like any other node, so it lays out with the normal [layout rules](LAYOUT.md). Its background and border are painted *underneath* the bitmap, so `style.backgroundColor` gives the canvas a solid backdrop for free. The default is transparent.

## The drawing surface

- **The surface is the canvas's content box**: the node's size minus its padding, in whole pixels (at least 1 × 1). `g.width()` and `g.height()` give you that size, so lay your drawing out from them and it follows resizes automatically.
- **A `Canvas` has no intrinsic size.** It has no children to fit around, so a `Fit` width or height resolves to **0** and nothing is drawn. Give it a `Fixed`, `Percentage` or `Full` size, or let `flexGrow` size it. (`Image` and `Svg` are different: they default to their natural size.)
- **Every `onPaint` starts from a fully transparent surface and a fresh context.** Nothing carries over from the previous call, neither pixels nor state such as the fill color or transform. Redraw everything each time.
- **A resize rebuilds the surface and triggers a redraw.** You do not need to detect it.
- **Coordinates are 1:1 with window pixels.** The header performs no DPI scaling.
- **Do not keep the `CanvasContext` after `onPaint` returns.** It is a stack object owned by the library. Unbalanced `save()`/`clip()` calls are cleaned up for you when the callback returns.
- **A huge canvas costs memory**, because the surface is a CPU bitmap the size of the box. For something like a zoomable image, keep the canvas the size of the viewport and draw with a source rectangle (see [the viewer recipe](#recipes)).

## When `onPaint` runs

LiteUI calls `onPaint` when:

1. the canvas is first laid out,
2. the surface is resized, or
3. `canvasDirtySource` returns `true`.

It does **not** re-run for unrelated repaints, such as a sibling's hover change, a scroll, or a timer tick elsewhere. This is what keeps idle UIs cheap.

### The dirty-flag pattern

`canvasDirtySource` is polled after every dispatched event, interval tick and wake. Return `true` **once** per change and then go back to `false`:

```cpp
bool dirty = false;
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };

// anywhere your state changes:
state.value = 0.7f;
dirty = true;
```

Event handlers on the canvas (and on other views, timers, and worker threads that call `LiteUI::wake()`) have no handle back into the tree, so a flag in your own state is the intended channel. Do not return `true` permanently: you would redraw on every poll.

### Animation

Drive the flag from an interval:

```cpp
float t = 0;
bool dirty = false;

LiteUI ui("Spinner", 320, 240);
ui.addInterval(16, [&] { t += 0.05f; dirty = true; });   // roughly 60 redraws per second at most
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };
c.onPaint = [&](CanvasContext &g) { /* draw using t */ };
```

Every tick repaints the window, not just the canvas, so animate only while something is actually moving. `removeInterval` stops it.

## Input and focus

`Canvas` exposes the same pointer, hover, wheel and focus members as `View`:

| Category | Members |
|---|---|
| Clicks | `onClick`, `onMiddleClick`, `onRightClick` |
| Press / drag | `onPressAt(x, y)`, `onDragTo(x, y)` (plus `onMiddle*` and `onRight*` variants), `onDragEnd()` |
| Hover | `onHoverAt(x, y)`, `onHoverEnd()`: return `true` if app state changed and a repaint is needed |
| Wheel notches | `onScrollUp`, `onScrollDown` |
| Keyboard | `focusable`, `onFocus`, `onBlur`, `onKeyDown`, `onTextInput(codepoint)` |

`Canvas` does not expose `onKeyUp`, `onLayout` or `tooltip`. Use `g.width()`/`g.height()` inside `onPaint` to learn the size.

Things that matter when drawing from pointer events:

- **Pointer coordinates are local to the node's border box**, with (0, 0) at its top-left, **not** to the drawing surface. If the canvas has padding, subtract `padding.left` and `padding.top` before using the coordinates for drawing. The simplest fix is to give canvases no padding.
- **Drags keep reporting while the button is held**, even when the pointer leaves the canvas. Coordinates can be negative or larger than `width()`/`height()`.
- **`onDragEnd` fires once** when the button is released or the platform aborts the drag (for example alt-tab).
- **A drag needs a press handler too.** A press lands on the topmost view that has an `onClick` or `onPressAt` for that button (or the middle/right equivalents). A canvas that sets only `onDragTo` is never the press target, so it never receives drags. Set `onPressAt` as well, even if it only records the start point.
- **Clicks bubble.** If the deepest view under the pointer has no handler, the press goes to the nearest ancestor that does.
- **Keyboard focus is explicit.** Set `focusable = true` and a click on the canvas focuses it. Only the focused view receives `onKeyDown` and `onTextInput`.
- **Wheel:** `onScrollUp`/`onScrollDown` fire per notch. A canvas is not a scroll container, so there is no pixel scrolling to turn off.

## Context state and defaults

A new `CanvasContext` starts with:

| State | Default |
|---|---|
| Fill and stroke | opaque black |
| Line width | 1 px on Windows, **2 px on Linux** (see below) |
| Line cap / join / miter limit | `Butt` / `Miter` / 10 |
| Line dash | solid |
| Global alpha | 1 |
| Composite operation | `SourceOver` |
| Shadow | none |
| Font | 16 px, platform default UI family, regular weight and style |
| Text align / baseline | `Start` / `Alphabetic` |
| Transform | identity |

> **Always call `setLineWidth` before stroking.** The Windows backend initializes the line width to 1 px, while the Linux backend leaves Cairo's own default in place, which is 2 px. A stroke that relies on the default will differ between platforms.

`save()` and `restore()` stack the transform, clip, fill/stroke paints, line width/cap/join/miter/dash, global alpha, composite operation, shadow, font, and text alignment and baseline. The **current path is not saved**, which matches HTML canvas.

## Paths and drawing

### Path model

- The current path persists until **`beginPath()`**. `fill()`, `stroke()` and `clip()` do **not** clear it, so you can fill and then stroke the same shape.
- `fillRect`, `strokeRect` and `clearRect` draw their own rectangle and leave the current path untouched.
- `arc` and `ellipse` connect to the current point with a straight line if a path is in progress, and start a new subpath if not. `arcTo` rounds a corner using two tangent points and a radius, as in HTML canvas.
- `closePath()` closes the current subpath. `rect` and `roundRect` close themselves. `roundRect` clamps the radius to half of the shorter side, and the same radius is used for all four corners.
- `fill` and `clip` take a `FillRule` (`NonZero` or `EvenOdd`). `stroke` has no rule.

```cpp
g.beginPath();
g.roundRect(10, 10, 120, 60, 12);
g.setFillColor(Color{40, 90, 200});
g.fill();
g.setStrokeColor(Color{255, 255, 255});
g.setLineWidth(2);
g.stroke();                    // same path, still current
```

### Transforms

`translate`, `rotate` (radians, clockwise on screen), `scale`, `transformBy(a,b,c,d,e,f)` (multiplies onto the current matrix), `setTransform(a,b,c,d,e,f)` (replaces it) and `resetTransform()`. The transform in effect when you call `fill`, `stroke`, `fillText` or `drawImage` is the one applied to that call. Shape coordinates are not "baked in" earlier, except for `arc`/`ellipse`/`arcTo` on Linux, which follow Cairo's rules.

### Clipping

`clip()` intersects the clip with the current path. A clip lasts **until the enclosing `restore()`**, so bracket it:

```cpp
g.save();
g.beginPath();
g.arc(cx, cy, r, 0, 6.2831853f);
g.clip();
// ...draw things that must stay inside the circle...
g.restore();
```

### Lines

`setLineWidth`, `setLineCap` (`Butt`, `Round`, `Square`), `setLineJoin` (`Miter`, `Round`, `Bevel`), `setMiterLimit`, `setLineDash({on, off, ...})` and `setLineDashOffset`. Dash lengths are in pixels on both platforms.

## Gradients, alpha, shadows and compositing

### Gradients

```cpp
CanvasGradient grad = CanvasGradient::linear(0, 0, 0, g.height());
grad.addColorStop(0.0f, Color{30, 40, 80});
grad.addColorStop(1.0f, Color{120, 60, 160});
g.setFillGradient(grad);       // setStrokeGradient for strokes
g.fillRect(0, 0, g.width(), g.height());
```

`CanvasGradient::radial(x0, y0, r0, x1, y1, r1)` takes an inner and an outer circle. Gradient coordinates are interpreted in the user space in effect when you fill or stroke. Offsets are clamped to 0..1.

On Windows, radial gradients with a non-zero inner radius are **approximated** (stop offsets are rescaled), while Linux draws the exact two-circle model.

### Global alpha

`setGlobalAlpha(a)` (0..1, clamped) multiplies the alpha of fills, strokes, gradient stops, text, and `drawImage`.

### Shadows

`setShadow(color, blurPx, offsetX, offsetY)` draws a **flat, unblurred, offset copy** of the next fills, strokes and text in `color` first. `blurPx` is accepted but ignored on both platforms. The offset is applied in the current user space, so a rotated or scaled transform rotates or scales it, unlike HTML canvas, where it is a device-space offset. Shadows apply to fills, strokes and text. `drawImage` and `putImageData` draw no shadow. Set the color's alpha to 0 or the offsets to 0 to turn shadows off.

### Compositing

`setGlobalCompositeOperation` takes `SourceOver`, `SourceIn`, `SourceOut`, `SourceAtop`, `DestinationOver`, `DestinationIn`, `DestinationOut`, `DestinationAtop`, `Lighter`, `Copy`, `Xor`, `Multiply` or `Screen`.

- **Linux:** honored, mapped onto the matching Cairo operator.
- **Windows:** **ignored.** The value is stored but everything draws with normal source-over blending. Do not rely on it if you ship on both platforms (erasing with `DestinationOut` is the usual casualty; use `clearRect` instead).

## Text

```cpp
g.setFont("", 24, FontWeight::Bold);          // empty family = platform default UI font
g.setFillColor(Color{255, 255, 255});
g.setTextAlign(TextAlign::Center);
g.setTextBaseline(TextBaseline::Middle);
g.fillText("Hello", g.width() / 2, g.height() / 2);

float w = g.measureText("Hello").width;       // width only, single line
```

- Text is **single-line**: `fillText` never wraps, and a newline character is not a line break. For paragraphs, use a `Text` node, or break lines yourself and draw each one.
- `TextAlign` accepts `Start`, `Center` and `End`. (`Justify` behaves like `Start`.) `TextBaseline` accepts `Top`, `Middle`, `Alphabetic` and `Bottom`.
- `measureText` returns only a width. It does not report ascent, descent or bounding boxes.
- The `maxWidth` argument of `fillText`/`strokeText` is accepted but **unused**. There is no squeeze-to-fit.
- **Gradient fills on text use only the first stop's color.** Text is always drawn in a single flat color.
- `strokeText` is an approximation: it draws the text repeatedly in the stroke color, nudged in a ring of eight directions by half the line width. It is not a true glyph outline, and it looks rough at large widths.
- Text is drawn with the current transform, shadow, alpha and (on Linux) composite operation.

Small cross-platform differences: `measureText` returns a rounded whole-pixel width on Linux and a fractional one on Windows, and on Windows the `Alphabetic` baseline is approximated as 80% of the line height, whereas Linux uses the font's real baseline. Expect text placed on the alphabetic baseline to sit a pixel or two differently between platforms.

## Images and pixels

### `CanvasImage`

A `CanvasImage` is a straight-alpha RGBA8 bitmap: `width`, `height` and `pixels` of `width * height * 4` bytes, row-major. `CanvasImage(w, h)` allocates a transparent one. To load files or encoded data:

```cpp
std::string err;
std::optional<CanvasImage> img = liteui_image::decodeFile("photo.jpg", &err);
// or: liteui_image::decodeMemory(bytes.data(), bytes.size(), &err);
```

On Linux, decoding supports **PNG and JPEG only**. On Windows it uses WIC, so anything WIC supports works. Decoding is synchronous.

### Drawing images

```cpp
g.drawImage(img, dx, dy);                              // natural size
g.drawImage(img, dx, dy, dw, dh);                      // scaled
g.drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh);      // source rect -> destination rect
```

Images are drawn with bilinear filtering, the current transform and global alpha. The nine-argument form is how you crop, zoom and pan.

> **In-place edits are not detected.** To avoid re-converting and re-uploading the same bitmap every redraw, each canvas caches the last image it drew, keyed on the pixel buffer's **address, size and dimensions**. A new image (a fresh allocation) or a resized one is picked up. If you change pixels **in place**, with the same buffer at the same address, the cache still holds the old picture and the canvas keeps drawing it. After in-place edits, give the image a fresh `pixels` vector (assign a new one, or copy the image) before the next draw.

### Reading and writing pixels

```cpp
std::optional<CanvasImage> data = g.getImageData(x, y, w, h);
g.putImageData(*data, dx, dy);
```

Both work in **raw surface pixels**, ignoring the current transform.

- `getImageData` returns straight RGBA. Areas outside the canvas come back transparent black.
- `putImageData` ignores the transform. Beyond that, the platforms differ for **translucent** pixels (see below).
- `clearRect(x, y, w, h)` really clears to transparent, honoring clip and transform. On Windows the clear tests pixel centers, so a rotated or skewed rectangle has hard (non-antialiased) edges.

## Platform differences

| Area | Windows (Direct2D) | Linux (Cairo) |
|---|---|---|
| Backing surface | Software render target on a WIC bitmap | Cairo ARGB32 image surface |
| Surface upload | Rebuilt as a GPU bitmap **every window paint** | Uploaded to a texture only when the canvas redraws |
| Default line width | 1 px | 2 px (Cairo default), so set it explicitly |
| `setGlobalCompositeOperation` | Ignored (always source-over) | Fully honored |
| Radial gradient inner radius | Approximated | Exact |
| `putImageData` | Alpha-**blends** over existing pixels and respects clipping | **Replaces** pixels directly, ignoring clip, alpha and compositing |
| `clearRect` | Hard pixel-center test on rotated edges | Antialiased |
| `measureText` width | Fractional | Rounded to whole pixels |
| `Alphabetic` baseline | Approximated (80% of line height) | Font's real baseline |
| `width()` / `height()` | Content box size, possibly fractional | Whole pixels |
| Shadow blur | Not supported | Not supported |

The same on both: gradient text uses the first stop, `strokeText` is approximate, `maxWidth` is unused, shadows are unblurred, and there is no DPI scaling.

For `putImageData`, the difference only shows for pixels that are not fully opaque. For opaque pixels, which is the usual case for photos and for round-tripping `getImageData` on an opaque drawing, both platforms give identical results.

## Canvas-backed built-ins

Three built-ins are implemented on top of the canvas machinery, so everything above about redraw behavior, surfaces and caching applies to them too:

- `Image` blits a decoded `CanvasImage` honoring `ObjectFit` (`Fill`, `Contain`, `Cover`, `None`, `ScaleDown`). Its `source` can be a callback returning a `std::shared_ptr<CanvasImage>`. Return a **different** pointer to make it repaint.
- `Svg` replays a parsed vector document at the node's size.
- `TextInput` draws its text, placeholder and caret on a canvas, and uses the public helper `liteui_text::caretIndexForX` for click-to-position. You can use the same helper to build your own text widgets.

## Recipes

**A simple paint program**

```cpp
struct Stroke { std::vector<std::pair<float, float>> pts; Color color; float width; };
std::vector<Stroke> strokes;
bool dirty = false;

Canvas c;
c.style.width = Size::full();
c.style.height = Size::full();
c.style.backgroundColor = Color{255, 255, 255};

c.onPressAt = [&](float x, float y) {
    Stroke s;
    s.color = Color{20, 20, 20};
    s.width = 3.0f;
    s.pts.push_back({x, y});
    strokes.push_back(std::move(s));
    dirty = true;
};
c.onDragTo = [&](float x, float y) {
    if (!strokes.empty()) strokes.back().pts.push_back({x, y});
    dirty = true;
};
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };

c.onPaint = [&](CanvasContext &g) {
    g.setLineCap(LineCap::Round);
    g.setLineJoin(LineJoin::Round);
    for (const Stroke &s : strokes) {
        if (s.pts.empty()) continue;
        g.setStrokeColor(s.color);
        g.setLineWidth(s.width);
        g.beginPath();
        g.moveTo(s.pts[0].first, s.pts[0].second);
        for (size_t i = 1; i < s.pts.size(); ++i)
            g.lineTo(s.pts[i].first, s.pts[i].second);
        g.stroke();
    }
};
```

Because the whole picture is rebuilt from `strokes` on every redraw, it also survives window resizes. For very long drawings, keep a committed `CanvasImage` of finished strokes and replay only the current one.

**A progress ring**

```cpp
float value = 0.65f;   // 0..1, updated elsewhere (set a dirty flag when it changes)

c.onPaint = [&](CanvasContext &g) {
    float cx = g.width() / 2, cy = g.height() / 2;
    float r = std::min(cx, cy) - 8;
    const float kTau = 6.2831853f, kTop = -1.5707963f;

    g.setLineWidth(10);
    g.setLineCap(LineCap::Round);

    g.setStrokeColor(Color{60, 60, 60});
    g.beginPath();  g.arc(cx, cy, r, 0, kTau);  g.stroke();

    g.setStrokeColor(Color{80, 170, 255});
    g.beginPath();  g.arc(cx, cy, r, kTop, kTop + kTau * value);  g.stroke();

    g.setFont("", 28, FontWeight::Bold);
    g.setFillColor(Color{255, 255, 255});
    g.setTextAlign(TextAlign::Center);
    g.setTextBaseline(TextBaseline::Middle);
    g.fillText(std::to_string(int(value * 100)) + "%", cx, cy);
};
```

**Hit-testing your own shapes**

The context has no `isPointInPath`, so keep shapes as data and test them yourself:

```cpp
struct Dot { float x, y, r; };
std::vector<Dot> dots;
Dot *selected = nullptr;

c.onPressAt = [&](float px, float py) {   // carries the position, unlike onClick
    for (auto it = dots.rbegin(); it != dots.rend(); ++it) {     // topmost first
        float dx = px - it->x, dy = py - it->y;
        if (dx * dx + dy * dy <= it->r * it->r) { selected = &*it; dirty = true; break; }
    }
};
```

**Zoomable image viewer (wheel zoom, drag to pan)**

```cpp
std::optional<CanvasImage> img = liteui_image::decodeFile("photo.png");
float zoom = 1.0f, panX = 0, panY = 0;           // panX/panY: image-space offset of the view's top-left
float lastX = 0, lastY = 0;
bool dirty = false;

c.onScrollUp   = [&] { zoom = std::min(16.0f, zoom * 1.1f); dirty = true; };
c.onScrollDown = [&] { zoom = std::max(0.05f, zoom / 1.1f); dirty = true; };
c.onPressAt    = [&](float x, float y) { lastX = x; lastY = y; };
c.onDragTo     = [&](float x, float y) {
    panX -= (x - lastX) / zoom;  panY -= (y - lastY) / zoom;
    lastX = x;  lastY = y;  dirty = true;
};
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };

c.onPaint = [&](CanvasContext &g) {
    if (!img) return;
    // The part of the image that is visible, in image pixels:
    float sw = g.width() / zoom, sh = g.height() / zoom;
    g.drawImage(*img, panX, panY, sw, sh, 0, 0, g.width(), g.height());
};
```

Only the visible window of the image is ever scaled and uploaded, so the canvas stays viewport-sized no matter how large the picture is. (Clamp `panX`/`panY` to the image bounds in real code.)

**Per-pixel processing**

```cpp
c.onPaint = [&](CanvasContext &g) {
    // ...draw an opaque scene first...
    if (auto data = g.getImageData(0, 0, g.width(), g.height())) {
        auto &px = data->pixels;
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            uint8_t gray = static_cast<uint8_t>((px[i] * 30 + px[i + 1] * 59 + px[i + 2] * 11) / 100);
            px[i] = px[i + 1] = px[i + 2] = gray;
        }
        g.putImageData(*data, 0, 0);
    }
};
```

## Performance notes

- **Cache your inputs, not your output.** A redraw rasterizes everything you issue, so avoid rebuilding large data structures inside `onPaint`. Precompute them and let `onPaint` only draw.
- **Redraw only when something changed.** Prefer event-driven dirty flags over a permanent timer. An always-on interval repaints the whole window every tick.
- **On Windows the finished bitmap is re-uploaded to the GPU on every window paint**, even when the canvas itself did not redraw. A large canvas on a window that repaints often (hovering, animating siblings) pays that cost repeatedly. Keep canvases no bigger than they need to be.
- **Large images:** draw with a source rectangle (see the viewer recipe) rather than scaling a huge image into a tiny box, and decode once, outside `onPaint`.
- **Save/restore around clips and transforms** rather than undoing them by hand.

## Not available

These HTML canvas features do not exist on `CanvasContext` today:

- patterns (`createPattern`), `Path2D`, `isPointInPath` / `isPointInStroke`, `getTransform`
- shadow blur (the parameter is ignored), filters, `imageSmoothingEnabled` (images are always bilinear; `putImageData` on Windows is nearest-neighbor)
- text metrics beyond width, wrapped or multi-line text, `letterSpacing`, `direction`, true stroked glyph outlines
- `createImageData` (use `CanvasImage(w, h)` instead)
- DPI-aware surfaces
