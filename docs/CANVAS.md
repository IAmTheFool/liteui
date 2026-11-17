# Canvas

`Canvas` is LiteUI's escape hatch for custom drawing: charts, gauges, paint programs, image viewers, custom controls. You get a `CanvasContext` modeled on the HTML5 2D context (paths, transforms, gradients, text, images, pixel access) and draw whatever you like into a rectangle that takes part in normal layout.

This guide covers the mental model, how and when your drawing code runs, handling input, the full drawing API, and the places where the backends behave differently. There are three: **Windows** (Direct2D on a Direct3D 11 device), **Linux with OpenGL ES 3.0** (the default), and **Linux with Cairo** (a fallback you get by configuring with `-DLITEUI_CANVAS_GL=OFF`). Both Windows and Linux/GL draw on the GPU.

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

Treat `onPaint` as: *"rebuild my picture from the app state I close over."* (By default every rebuild repaints the whole canvas. If only a small region changes at a time, a [retained canvas](#retained-canvases-and-partial-redraw) repaints just that region.) Keep your data (strokes, values, an image) in your own variables, mutate those from event handlers, and tell LiteUI the picture is stale with a dirty flag. Do not draw from event handlers, because there is no context to draw into outside `onPaint`.

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
- **Every `onPaint` starts from a fully transparent surface and a fresh context.** Nothing carries over from the previous call, neither pixels nor state such as the fill color or transform. Redraw everything each time. (The one exception is a [retained canvas](#retained-canvases-and-partial-redraw): its pixels outside the damaged region are kept, but the context state is still fresh.)
- **A resize rebuilds the surface and triggers a redraw.** You do not need to detect it. A lost GPU device or EGL context is also recovered automatically: the surfaces are rebuilt for you.
- **Some failures print one line to `stderr`** instead of throwing, for example an image larger than the GPU supports, text too large for the glyph atlas, or a canvas target that could not be created on Linux/GL (messages start with `liteui [LITEUI_CANVAS_GL]:`). Nothing is drawn for that call, and each distinct message prints once per process.
- **Coordinates are logical units; the backing store is in device pixels.** `width()`, `height()` and every drawing coordinate are logical, so the same code looks the same on a 100% and a 200% display. The surface itself is `pixelWidth()` x `pixelHeight()` device pixels, and `devicePixelRatio()` is the ratio between them (1 on a 96-dpi display, 2 at 200%, fractional values such as 1.25 are common; `LiteUI::contentScale()` is the same number for the window). `getImageData` and `putImageData` address those raw device pixels and ignore the transform.
- **Do not keep the `CanvasContext` after `onPaint` returns.** It is a stack object owned by the library. Unbalanced `save()`/`clip()` calls are cleaned up for you when the callback returns.
- **A huge canvas costs memory**, because the surface is a texture the size of the box (GPU memory on Windows and Linux/GL; a CPU bitmap on the Cairo fallback). For something like a zoomable image, keep the canvas the size of the viewport and draw with a source rectangle (see [the viewer recipe](#recipes)).

## When `onPaint` runs

LiteUI calls `onPaint` when:

1. the canvas is first laid out,
2. the surface is resized,
3. `canvasDirtySource` returns `true`, or
4. (retained canvases only) something was reported to the canvas's [`CanvasDamage`](#retained-canvases-and-partial-redraw).

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

Event handlers on the canvas (and on other views, timers, and worker threads that call `LiteUI::wake()`) have no handle back into the tree, so a flag in your own state is the intended channel. Do not return `true` permanently: you would redraw on every poll. A bool says "everything is stale"; to say *which region* changed, and to do it safely from a worker thread, use a [`CanvasDamage`](#retained-canvases-and-partial-redraw) handle instead.

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

### Retained canvases and partial redraw

By default a redraw clears the whole canvas and `onPaint` repaints all of it. That is the right model for most canvases. It is wasteful when a small region changes at a time (a tile-based renderer, a paint surface): every change repaints, clears and re-resolves the full-size surface. A **retained** canvas keeps its pixels between paints and repaints only the region you report.

```cpp
auto damage = std::make_shared<CanvasDamage>();
damage->setNotify([&ui] { ui.wake(); });          // optional: wake the UI loop after each report

Canvas c;
c.retained = true;
c.damage = damage;
c.onPaint = [&](CanvasContext &g) {
    if (g.isFullRepaint()) { /* draw everything */ }
    else                   { /* draw what intersects g.damage() */ }
};

damage->invalidate(x, y, w, h);                   // any thread; canvas logical units
```

**What happens on a redraw**

- Only the damaged box is **cleared to transparent and repainted**. Drawing outside it is clipped away, so pixels outside it keep exactly what they had. Repaint everything that intersects the box; you do not need to repaint anything else.
- `g.damage()` returns the box in logical units. It is the reported rectangle rounded **outward to whole device pixels**, so a fractionally covered pixel is repainted, never left stale. `g.isFullRepaint()` is true when the whole surface was cleared, and then `g.damage()` is `(0, 0, width(), height())`.
- Damage reported between two repaints is **merged into one bounding box**, so scattered updates repaint the box around them. A rectangle with no area is ignored; one with a non-finite coordinate counts as `invalidateAll()`. Damage that lies entirely off the canvas skips `onPaint`.
- On Windows the box is applied with an axis-aligned clip and a copy-blend clear, on Linux/GL with a scissor over the box (only that box is resolved from the multisample target), and on the Cairo fallback with a clip (only that box is re-uploaded).

**What forces a full repaint** (`isFullRepaint()` is `true`, the whole surface is cleared first): the first paint, a resize, a lost GPU device or EGL context, a `canvasDirtySource` that returns `true`, `damage->invalidateAll()`, and any canvas that is not retained. An `onPaint` that honors `damage()` must therefore handle the full case too.

**Threads.** `CanvasDamage::invalidate()` and `invalidateAll()` are safe to call from any thread. The notify callback runs on the invalidating thread after the lock is released; `LiteUI::wake()` is the intended callback, because it is the one call that is safe off the UI thread. Without a callback the damage is picked up at the next repaint for any reason, so wake the loop yourself. Join your worker threads before the `LiteUI` they notify goes away.

**Notes**

- `damage` without `retained` is just another dirty source: it triggers a full redraw.
- Context state is never retained, only pixels. Every `onPaint` still starts with a fresh context.
- `putImageData` is clipped to the damaged box on Windows and Linux/GL. The Cairo fallback writes pixels directly and does **not** clip it, so keep it inside the box.
- Retained mode helps most together with [textures](#textures), which let the tiles themselves be updated in place.

## Input and focus

`Canvas` exposes the same pointer, wheel and focus members as `View`, with the same types and semantics (the full rules for dispatch, capture and bubbling are in [API.md](API.md#pointer-events)):

| Category | Members |
|---|---|
| Pointer | `onPointerDown`, `onPointerMove`, `onPointerUp`, `onPointerCancel`, `onPointerEnter`, `onPointerLeave` (all `void(PointerEvent &)`) |
| Clicks | `onClick` (accepts `[&]{...}` or `[&](PointerEvent &e){...}`), `onDoubleClick`, `onAuxClick` (middle), `onContextMenu` (right, on release) |
| Wheel | `onWheel` (`void(WheelEvent &)`) |
| Keyboard | `focusable`, `onFocus`, `onBlur`, `onKeyDown`, `onTextInput(codepoint)` |

`Canvas` does not expose `onKeyUp`, `onLayout` or `tooltip`. Use `g.width()`/`g.height()` inside `onPaint` to learn the size. To change the mouse cursor over a canvas, set `style.cursor` (for example `Cursor::Crosshair`).

One event type covers mouse, pen and touch. The button is a **field**, not a separate handler: `if (e.button == MouseButton::Middle) ...`. A pen tip or a finger counts as the left button, so a paint program written for the mouse works with a pen, and additionally gets `e.pressure`, `e.tiltX` / `e.tiltY` and `e.eraser` for free.

Things that matter when drawing from pointer events:

- **Pointer coordinates are local to the node's border box**, with (0, 0) at its top-left, **not** to the drawing surface. If the canvas has padding, subtract `padding.left` and `padding.top` before using the coordinates for drawing. The simplest fix is to give canvases no padding.
- **A press captures the pointer.** Once `onPointerDown` has run on the canvas, the following moves and the final `onPointerUp` are delivered to it **even when the pointer leaves the canvas**, so coordinates can be negative or larger than `width()`/`height()`. This is what makes dragging work, and it needs no extra handler: `onPointerDown` alone is enough to become the capture target.
- **Tell hover from dragging with `buttons`.** `onPointerMove` fires for both. During a hover `e.buttons == 0`; during a drag the button bit is set, so `e.held(MouseButton::Left)` is the usual test.
- **Use `e.coalesced` for strokes.** Handlers run once per frame, but a fast pen or mouse can report several positions in that time. `e.coalesced` lists every position since the previous move event, oldest first, ending with the current one, so a stroke built from it has no gaps. The samples are in window coordinates; convert each with `e.toLocal(sample)`. Copy what you need, because the span is only valid while the handler runs.
- **End a drag in both `onPointerUp` and `onPointerCancel`.** The cancel arrives when the OS takes the pointer away (alt-tab, a system dialog, a touch gesture the compositor claims) or when the press turned into a scroll container's drag-to-pan: the canvas keeps getting `onPointerMove` during the pan, and the release arrives as `onPointerCancel` instead of `onPointerUp`. It is **not** sent if the canvas is destroyed mid-drag (a keyed-list rebuild or `setRoot()`).
- **Bubbling.** An event goes to the deepest view under the pointer that has a handler for it, then up through ancestors that have one. A one-argument handler can call `e.stopPropagation()` to stop that; the short `[&]{...}` form of `onClick` ends bubbling automatically. A press on a canvas inside a scroll container may start a pan once the pointer moves a few pixels; if you want the canvas to keep the drag for itself, set `contentPanEnabled = false` on the scroll container.
- **Keyboard focus is explicit.** Set `focusable = true` and a click on the canvas focuses it. Only the focused view receives `onKeyDown` and `onTextInput`.
- **The wheel falls through to scrolling.** A canvas is not a scroll container, but if an ancestor scrolls, a wheel event that nothing stops will scroll that ancestor. If the canvas uses the wheel itself (zooming, say), call `e.stopPropagation()` in `onWheel`; that also suppresses the built-in scrolling. `e.dy` is in pixels, positive for "scroll down". On Windows one notch is 40 px.

## Context state and defaults

A new `CanvasContext` starts with:

| State | Default |
|---|---|
| Fill and stroke | opaque black |
| Line width | 1 px (Windows and Linux/GL), **2 px on the Linux/Cairo fallback** (see below) |
| Line cap / join / miter limit | `Butt` / `Miter` / 10 |
| Line dash | solid |
| Global alpha | 1 |
| Composite operation | `SourceOver` |
| Shadow | none |
| Font | 16 px, platform default UI family, regular weight and style |
| Text align / baseline | `Start` / `Alphabetic` |
| Transform | identity |

> **Always call `setLineWidth` before stroking.** The Windows and Linux/GL backends initialize the line width to 1 px, while the Cairo fallback leaves Cairo's own default in place, which is 2 px. A stroke that relies on the default will differ if you ever build with `-DLITEUI_CANVAS_GL=OFF`.

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

`translate`, `rotate` (radians, clockwise on screen), `scale`, `transformBy(a,b,c,d,e,f)` (multiplies onto the current matrix), `setTransform(a,b,c,d,e,f)` (replaces it) and `resetTransform()`. The transform in effect when you call `fill`, `stroke`, `fillText` or `drawImage` is the one applied to that call. Shape coordinates are not "baked in" earlier, except for `arc`/`ellipse`/`arcTo` on the Cairo fallback, which follow Cairo's rules.

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

On Windows, radial gradients with a non-zero inner radius are **approximated** (Direct2D only models one circle plus a focal point, so stop offsets are rescaled), while both Linux backends draw the exact two-circle model.

### Global alpha

`setGlobalAlpha(a)` (0..1, clamped) multiplies the alpha of fills, strokes, gradient stops, text, and `drawImage`.

### Shadows

`setShadow(color, blurPx, offsetX, offsetY)` draws a **flat, unblurred, offset copy** of the next fills, strokes and text in `color` first. `blurPx` is accepted but ignored on both platforms. The offset is applied in the current user space, so a rotated or scaled transform rotates or scales it, unlike HTML canvas, where it is a device-space offset. Shadows apply to fills, strokes and text. `drawImage` and `putImageData` draw no shadow. Set the color's alpha to 0 or the offsets to 0 to turn shadows off.

### Compositing

`setGlobalCompositeOperation` takes `SourceOver`, `SourceIn`, `SourceOut`, `SourceAtop`, `DestinationOver`, `DestinationIn`, `DestinationOut`, `DestinationAtop`, `Lighter`, `Copy`, `Xor`, `Multiply` or `Screen`.

- **Linux/GL (default):** every operator is honored with GPU blending. `Multiply` is the expensive one: it reads back the canvas drawn so far, so it costs one copy of the canvas **per shape**. Avoid it for many small shapes.
- **Linux/Cairo fallback:** every operator is honored, mapped onto the matching Cairo operator.
- **Windows:** only `SourceOver`, `Copy` and `Lighter` (additive) are honored. Any other value is stored but draws as plain source-over, because Direct2D's primitive blend modes cover only those three. Do not rely on the others if you ship on Windows (erasing with `DestinationOut` is the usual casualty; use `clearRect` instead).

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
- **Gradient fills on text:** on Linux/GL a gradient fill really paints the text. On Windows and the Cairo fallback only the **first stop's color** is used, so text is a single flat color there.
- **`strokeText`** differs by backend. On Linux/GL it traces the **real glyph outlines** with the normal stroker, so line width, joins, dashes, gradients, shadow and clip all apply. On Windows and the Cairo fallback it is an approximation: it draws the text repeatedly in the stroke color, nudged in a ring of directions by half the line width, which looks rough at large widths.
- **Color emoji** keep their own colors with `fillText` on Linux/GL when the fill is a solid color. With a gradient fill they draw as silhouettes tinted by the gradient.
- Text is drawn with the current transform, shadow, alpha and (on Linux) composite operation.

Small cross-platform differences: `measureText` returns a rounded whole-pixel width on both Linux backends and a fractional one on Windows, and on Windows the `Alphabetic` baseline is approximated as 80% of the line height, whereas Linux uses the font's real baseline. Expect text placed on the alphabetic baseline to sit a pixel or two differently between platforms.

## Images and pixels

### `CanvasImage`

A `CanvasImage` is a straight-alpha RGBA8 bitmap: `width`, `height` and `pixels` of `width * height * 4` bytes, row-major. `CanvasImage(w, h)` allocates a transparent one. To load files or encoded data:

```cpp
std::string err;
std::optional<CanvasImage> img = liteui_image::decodeFile("photo.jpg", &err);
// or: liteui_image::decodeMemory(bytes.data(), bytes.size(), &err);
```

On Linux, decoding supports **PNG and JPEG only**. On Windows it uses WIC, so anything WIC supports works. Decoding is synchronous. A 16-bit PNG is reduced to 8 bits here; for photo work that needs the file's full precision, decode to an `ImageBuffer` with `liteui_image::decodeFileBuffer` and turn it into a displayable `CanvasImage` with `liteui_image::toCanvasImage` (see [API.md](API.md#liteui_image-decoding-encoding-and-display-conversion)). The `Image` node does that conversion for you when you give it an `hdrSource`.

### Drawing images

```cpp
g.drawImage(img, dx, dy);                              // natural size
g.drawImage(img, dx, dy, dw, dh);                      // scaled
g.drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh);      // source rect -> destination rect
```

Images honor the current transform, clip and global alpha (and, on Linux/GL, the composite operation). The nine-argument form is how you crop, zoom and pan.

**Filtering.** `setImageSampling(mode)` chooses how `drawImage` (and `drawTexture`) filter the source. It is saved and restored by `save()` / `restore()`, and `putImageData` is unaffected (it always blits 1:1).

| Mode | Behavior |
|---|---|
| `Smooth` (default) | Bilinear. Drawn smaller than the source (zoomed out), a downscale filter is used so detail does not shimmer: mipmaps on Linux/GL, high-quality filtering on Windows. |
| `Nearest` | Nearest neighbor: crisp pixels. |
| `Auto` | `Nearest` while the image is drawn at one device pixel per source pixel or larger (zoomed in), `Smooth` below that. Decided per call from the transform, the rectangle ratio and `devicePixelRatio()`, using the smaller axis scale. |

If you downscale an image yourself (for a slider preview, say), set `Smooth` explicitly: `Auto` would see the already-reduced image drawn magnified and pick `Nearest`. On Windows and Linux/GL the source rectangle really **crops**: the part outside the image is dropped and the destination shrinks in proportion. The Cairo fallback treats an out-of-bounds source rectangle differently, so keep it inside the image if you may run there.

> **In-place edits are not detected, and the cache has one slot.** To avoid re-converting and re-uploading the same bitmap every redraw, each canvas caches the **last** image it drew, keyed on the pixel buffer's **address, size and dimensions**. A new image (a fresh allocation) or a resized one is picked up. If you change pixels **in place**, with the same buffer at the same address, the cache still holds the old picture and the canvas keeps drawing it. After in-place edits, give the image a fresh `pixels` vector (assign a new one, or copy the image) before the next draw.
>
> Because there is only one slot, a canvas that draws **several different images in one `onPaint`** (a row of thumbnails, a before/after pair) evicts and re-uploads them on every redraw. Use a [`CanvasTexture`](#textures) for each picture instead: every texture keeps its own GPU copy, and one you update in place is always current.

### Textures

A `CanvasTexture` is a picture **you own** that lives on the GPU between paints and is drawn with `drawTexture`. It fixes the two weak spots of `drawImage` described above: pictures never evict each other, and a texture can be updated **one rectangle at a time**, so a tile that finished does not re-send the rest. There are two kinds.

**An updatable texture** for pixels that arrive in pieces:

```cpp
auto tex = std::make_shared<CanvasTexture>(4096, 4096);   // RGBA8, starts transparent
tex->update(tx, ty, 256, 256, pixels, 256 * 4);           // any thread: x, y, w, h, data, row bytes
g.drawTexture(tex, sx, sy, sw, sh, dx, dy, dw, dh);       // inside onPaint
```

**A wrapped texture** for a picture that is built once and never edited (a decoded photo, a finished render):

```cpp
auto tex = CanvasTexture::wrap(image);                    // std::shared_ptr<const CanvasImage>
g.drawTexture(tex, 0, 0);
```

`wrap` **shares** the image's pixels instead of copying them, so it costs no memory beyond the image itself. It is immutable (`update` returns `false`), and you promise not to modify the image while the texture exists: to change the picture, wrap the new image. The texture keeps the image alive, and dropping the texture releases both.

`drawTexture` takes the same arguments as `drawImage` (natural size, scaled, or source rectangle to destination rectangle; the source is in **texture pixels**, the destination in logical units) and honors the transform, clip, global alpha, the composite operation and `setImageSampling`. A source rectangle outside the texture is cropped. A null, invalid or too-large texture draws nothing.

**Updating**

- `update(x, y, w, h, data, strideBytes)` copies a rectangle in and returns `false` only for invalid arguments (null data, a negative size, a stride shorter than a row, or an invalid texture). The part outside the texture is clipped, so an edge tile may overhang it; a rectangle wholly outside succeeds and changes nothing. `update(x, y, image)` places a whole `CanvasImage`.
- A texture made with the default `TextureAlpha::Straight` takes straight-alpha input (what `CanvasImage` holds) and premultiplies it **on the calling thread**. `TextureAlpha::Premultiplied` copies as is.
- `clear()` resets every pixel to transparent. `version()` increases on every change. `setNotify(fn)` runs after every `update` / `clear` on the calling thread with no lock held; `LiteUI::wake()` is the intended callback.
- `update()` does **not** redraw anything: a canvas repaints only when told to. Also report the affected region to the canvas's [`CanvasDamage`](#retained-canvases-and-partial-redraw), or set a dirty flag.

**Threads and memory**

- A texture is a plain CPU object, safe to create, update and destroy on **any thread**, even before a window exists, and one texture can be drawn by several canvases or windows. It keeps a premultiplied CPU copy of its pixels (4 bytes per pixel; a wrapped texture keeps none) and creates the GPU copy the first time it is drawn.
- Each window uploads **only the rectangles changed since it last drew** the texture. Overlapping or nested updates are sent once, many scattered ones collapse to their bounding box, and a mostly-changed texture is sent whole.
- The CPU copy is what makes a lost GPU device or EGL context recover with no help from you: the next draw rebuilds the GPU copy from it.
- When a texture is destroyed, its GPU copy is released at the next canvas pass.

**Limits.** Only `PixelFormat::RGBA8_UNORM` is supported for now (any other format makes `valid()` false). `valid()` is also false for non-positive or oversized dimensions (the CPU copy is capped at 2 GB), and a texture larger than the GPU's maximum texture size draws nothing and prints one line to `stderr`. The Cairo fallback has no GPU copy: each `drawTexture` converts just the part it needs into a temporary surface, which is simple but slower.

### Reading and writing pixels

```cpp
std::optional<CanvasImage> data = g.getImageData(x, y, w, h);
g.putImageData(*data, dx, dy);
```

Both work in **raw surface pixels**, ignoring the current transform.

- `getImageData` returns straight RGBA. Areas outside the canvas come back transparent black. On both GPU backends it **flushes pending drawing and reads the pixels back from the GPU**, which is slow compared with drawing. Call it once per redraw (as in the per-pixel recipe), not once per shape or pixel.
- `putImageData` ignores the transform. Beyond that, the platforms differ for **translucent** pixels and for clipping (see below).
- `clearRect(x, y, w, h)` really clears to transparent, honoring clip and transform. The edges are antialiased on every backend, including for rotated or skewed rectangles.

## Platform differences

| Area | Windows (Direct2D) | Linux, OpenGL ES 3.0 (default) | Linux, Cairo (`LITEUI_CANVAS_GL=OFF`) |
|---|---|---|---|
| Backing surface | GPU render target (`ID2D1Bitmap1`) on the window's Direct3D 11 device | GPU texture (MSAA when available) | CPU Cairo ARGB32 image surface, uploaded as a texture |
| Surface lifetime | Stays in video memory; redrawn only when `onPaint` reruns | Same | Uploaded to a texture only when the canvas redraws |
| Default line width | 1 px | 1 px | 2 px (Cairo default), so set it explicitly |
| `setGlobalCompositeOperation` | `SourceOver`, `Copy`, `Lighter` only; others draw as source-over | All operators (`Multiply` costs a canvas copy per shape) | All operators |
| Radial gradient inner radius | Approximated | Exact | Exact |
| Gradient fill on text | First stop's color only | Honored | First stop's color only |
| `strokeText` | Approximation (offset copies) | True glyph outlines | Approximation (offset copies) |
| Color emoji in `fillText` | Not specified | Keep their colors (solid fill) | Not specified |
| `putImageData` | Alpha-**blends** over existing pixels, honors clip, nearest-neighbor | **Replaces** pixels, ignoring clip, alpha and compositing; origin rounded to whole pixels | **Replaces** pixels directly, ignoring clip, alpha and compositing |
| `getImageData` | GPU to CPU readback (flushes pending work) | GPU to CPU readback | Direct read of the surface |
| `clearRect` | Antialiased | Antialiased, honors rotation and skew | Antialiased |
| `measureText` width | Fractional | Rounded to whole pixels | Rounded to whole pixels |
| `Alphabetic` baseline | Approximated (80% of line height) | Font's real baseline | Font's real baseline |
| `width()` / `height()` | Content box size, possibly fractional | Whole pixels | Whole pixels |
| `drawImage` source rectangle | Crops | Crops | Differs for out-of-bounds rectangles |
| `drawTexture` source rectangle | Crops | Crops | Crops |
| `CanvasTexture` GPU copy | One `ID2D1Bitmap1` each, changed rectangles written with `CopyFromMemory` | One GL texture each, changed rectangles written with `glTexSubImage2D`; mipmaps rebuilt lazily after a change | None: each draw converts the part it needs into a temporary surface |
| Retained canvas: partial redraw | Axis-aligned clip + copy-blend clear of the damage box | Scissor over the damage box; only that box is resolved from the multisample target | Clip over the damage box; only that box is re-uploaded |
| Retained canvas: `putImageData` | Clipped to the damage box | Clipped to the damage box | **Not** clipped (writes pixels directly) |
| Shadow blur | Not supported | Not supported | Not supported |

The same on all three: `maxWidth` is unused, shadows are flat and unblurred, and `drawImage`, `drawTexture` and `putImageData` draw no shadow.

For `putImageData`, the differences only show for pixels that are not fully opaque, or when a clip is active. For opaque pixels with no clip, which is the usual case for photos and for round-tripping `getImageData` on an opaque drawing, all backends give identical results.

## Canvas-backed built-ins

Three built-ins are implemented on top of the canvas machinery, so everything above about redraw behavior, surfaces and caching applies to them too:

- `Image` blits a decoded `CanvasImage` honoring `ObjectFit` (`Fill`, `Contain`, `Cover`, `None`, `ScaleDown`). Its `source` can be a callback returning a `std::shared_ptr<CanvasImage>`. Return a **different** pointer to make it repaint. For high-precision data, `hdrSource` takes a `std::shared_ptr<ImageBuffer>` and `displaySettings` (exposure, clamp mode) can be a callback, so an exposure slider re-renders without a new buffer.
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
c.style.cursor = Cursor::Crosshair;

c.onPointerDown = [&](PointerEvent &e) {
    if (e.button != MouseButton::Left) return;
    Stroke s;
    s.color = e.eraser ? Color{255, 255, 255} : Color{20, 20, 20};
    s.width = 3.0f;
    s.pts.push_back({e.x, e.y});
    strokes.push_back(std::move(s));
    dirty = true;
};
c.onPointerMove = [&](PointerEvent &e) {
    if (!e.held(MouseButton::Left) || strokes.empty()) return;   // ignore hover
    for (const PointerSample &smp : e.coalesced) {               // no gaps in fast strokes
        PointerSample p = e.toLocal(smp);                        // window -> this canvas
        strokes.back().pts.push_back({p.x, p.y});
    }
    dirty = true;
};
c.onPointerCancel = [&](PointerEvent &) {                        // OS took the pointer
    if (!strokes.empty()) strokes.pop_back();
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

Because the whole picture is rebuilt from `strokes` on every redraw, it also survives window resizes. Pen input works with no extra code: a tip touching down is the left button, and `e.pressure` is available if you want to vary `width` with it. For very long drawings, keep a committed `CanvasImage` of finished strokes and replay only the current one.

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

c.onPointerDown = [&](PointerEvent &e) {   // e.x, e.y are local to the canvas
    for (auto it = dots.rbegin(); it != dots.rend(); ++it) {     // topmost first
        float dx = e.x - it->x, dy = e.y - it->y;
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

c.onWheel = [&](WheelEvent &e) {
    zoom = e.dy < 0 ? std::min(16.0f, zoom * 1.1f)    // wheel up: zoom in
                    : std::max(0.05f, zoom / 1.1f);   // wheel down: zoom out
    dirty = true;
    e.stopPropagation();                              // don't also scroll an ancestor
};
c.onPointerDown = [&](PointerEvent &e) { lastX = e.x; lastY = e.y; };
c.onPointerMove = [&](PointerEvent &e) {
    if (!e.held(MouseButton::Left)) return;
    panX -= (e.x - lastX) / zoom;  panY -= (e.y - lastY) / zoom;
    lastX = e.x;  lastY = e.y;  dirty = true;
};
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };

c.onPaint = [&](CanvasContext &g) {
    if (!img) return;
    // The part of the image that is visible, in image pixels:
    float sw = g.width() / zoom, sh = g.height() / zoom;
    g.drawImage(*img, panX, panY, sw, sh, 0, 0, g.width(), g.height());
};
```

Only the visible window of the image is ever scaled and drawn, so the canvas stays viewport-sized no matter how large the picture is. (Clamp `panX`/`panY` to the image bounds in real code; the source rectangle crops on Windows and Linux/GL, but is best kept inside the image on the Cairo fallback.)

**Per-pixel processing**

```cpp
c.onPaint = [&](CanvasContext &g) {
    // ...draw an opaque scene first...
    // getImageData reads back from the GPU, so call it once per redraw like this.
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

**A progressive tile renderer (worker threads)**

Tiles are rendered off the UI thread. Each finished tile is written into a texture and only its region is repainted. The canvas is retained, so nothing else is cleared or redrawn.

```cpp
constexpr int   kTile = 256;
constexpr float kZoom = 0.25f;                       // canvas units per texture pixel
auto tex    = std::make_shared<CanvasTexture>(4096, 4096);
auto damage = std::make_shared<CanvasDamage>();

LiteUI ui("Tiles", 1024, 768);
damage->setNotify([&ui] { ui.wake(); });            // every invalidate() wakes the UI loop

Canvas c;
c.style.width = Size::full();
c.style.height = Size::full();
c.retained = true;                                   // keep pixels between paints
c.damage = damage;
c.onPaint = [tex](CanvasContext &g) {
    const DirtyRect d = g.damage();                  // the whole canvas on a full repaint
    g.setImageSampling(ImageSampling::Smooth);
    g.drawTexture(tex, d.x / kZoom, d.y / kZoom, d.w / kZoom, d.h / kZoom,   // texture pixels
                  d.x, d.y, d.w, d.h);               // canvas units
};

std::thread worker([&] {
    std::vector<uint8_t> px(kTile * kTile * 4);
    for (int ty = 0; ty < 16; ++ty)
        for (int tx = 0; tx < 16; ++tx) {
            // ... render the tile into px (straight-alpha RGBA8) ...
            tex->update(tx * kTile, ty * kTile, kTile, kTile, px.data(), kTile * 4);  // any thread
            damage->invalidate(tx * kTile * kZoom, ty * kTile * kZoom,
                               kTile * kZoom, kTile * kZoom);                         // any thread
        }
});
// ... ui.run(...); then worker.join() before `ui` goes away.
```

If the view can zoom or pan, call `damage->invalidateAll()` when it changes: the tiles already in the texture are still right, but the region they map to is not.

**A before / after split with two textures**

Both pictures stay on the GPU, so dragging the split does not re-upload either one. (With `drawImage`, drawing the two alternately would re-upload both on every redraw.)

```cpp
std::shared_ptr<CanvasImage> before, after;          // finished pictures, never edited in place
auto beforeTex = CanvasTexture::wrap(before);        // shares the pixels: no copy
auto afterTex  = CanvasTexture::wrap(after);
float split = 0.5f;
bool dirty = false;

Canvas c;
c.style.width = Size::full();
c.style.height = Size::full();
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };
c.onPaint = [&](CanvasContext &g) {
    const float W = g.width(), H = g.height(), x = W * split;
    g.save();
    g.beginPath();  g.rect(0, 0, x, H);  g.clip();
    g.drawTexture(beforeTex, 0, 0, W, H);
    g.restore();
    g.save();
    g.beginPath();  g.rect(x, 0, W - x, H);  g.clip();
    g.drawTexture(afterTex, 0, 0, W, H);
    g.restore();
};

// the split moved:        split = 0.7f;  dirty = true;
// a new edit finished:    afterTex = CanvasTexture::wrap(newAfter);  dirty = true;
```

## Performance notes

- **Cache your inputs, not your output.** A redraw rasterizes everything you issue, so avoid rebuilding large data structures inside `onPaint`. Precompute them and let `onPaint` only draw.
- **Redraw only when something changed.** Prefer event-driven dirty flags over a permanent timer. An always-on interval repaints the whole window every tick.
- **The finished canvas stays on the GPU** (Windows and Linux/GL), so a window repaint that does not redraw the canvas only draws its texture. The costs to watch instead are `getImageData` (a GPU readback that flushes pending work), `globalCompositeOperation = Multiply` on Linux/GL (one canvas copy per shape), and very large canvases, which take GPU memory proportional to their pixel size. Keep canvases no bigger than they need to be.
- **Large images:** draw with a source rectangle (see the viewer recipe) rather than scaling a huge image into a tiny box, and decode once, outside `onPaint`.
- **Several images in one canvas, or pixels that change in pieces:** use [`CanvasTexture`](#textures). `drawImage` has one cache slot per canvas, so alternating images re-upload every redraw. Use `CanvasTexture::wrap` for finished pictures (no extra memory) and an updatable texture for tiles.
- **Small changes to a big canvas:** make it [retained](#retained-canvases-and-partial-redraw) and report the damaged rectangle. The cost of a redraw then follows the size of the change, not the size of the canvas. Nothing here reduces the window-level cost: the window still repaints as before.
- **Save/restore around clips and transforms** rather than undoing them by hand.

## Not available

These HTML canvas features do not exist on `CanvasContext` today:

- patterns (`createPattern`), `Path2D`, `isPointInPath` / `isPointInStroke`, `getTransform`
- shadow blur (the parameter is ignored), filters, a per-call `imageSmoothingEnabled` flag (use `setImageSampling`; `putImageData` on Windows is nearest-neighbor)
- text metrics beyond width, wrapped or multi-line text, `letterSpacing`, `direction`, and (on Windows and the Cairo fallback) true stroked glyph outlines; Linux/GL does trace real outlines
- `createImageData` (use `CanvasImage(w, h)` instead)
- texture formats other than RGBA8 (`CanvasTexture` rejects them), and rectangle updates to a wrapped texture (wrap a new image instead)