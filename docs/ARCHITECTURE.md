# Architecture

This document explains how LiteUI is put together: what lives where in `liteui.hpp`, how a UI description becomes pixels, how input and updates flow, and how the Windows and Linux backends fit into the same design. It is for people who want to read, debug or extend the library. For how to *use* it, start with the [README](../README.md) and [API.md](API.md).

- [Design principles](#design-principles)
- [Map of the header](#map-of-the-header)
- [Public surface versus implementation](#public-surface-versus-implementation)
- [The data model](#the-data-model)
- [The update cycle](#the-update-cycle)
- [Layout](#layout)
- [Rendering](#rendering)
- [Event loop and input](#event-loop-and-input)
- [Threading and timers](#threading-and-timers)
- [Supporting subsystems](#supporting-subsystems)
- [Platform strategy](#platform-strategy)
- [Build architecture](#build-architecture)
- [Costs and trade-offs](#costs-and-trade-offs)
- [Sharp edges](#sharp-edges)
- [Extending LiteUI](#extending-liteui)

## Design principles

- **One header, one implementation translation unit.** Everything is in `liteui.hpp`. The implementation sits behind `LITEUI_IMPLEMENTATION` and is compiled into exactly one `.cpp` per program.
- **A retained tree you describe once.** You build a tree of `View`s with styles and callbacks, hand it to the window, and the library owns it from then on.
- **Poll, don't push.** Instead of an observable or a diffing layer, fields that can change are `Dynamic<T>`: a value or a callback. After every event the library polls the callbacks, compares against a cached copy, and relays out and repaints only if something changed.
- **No runtime polymorphism for platforms.** Windows and Linux are selected with `#if defined(_WIN32)`, and the canvas backend with `LITEUI_CANVAS_GL`, not virtual interfaces. The public `LiteUI` and `CanvasContext` classes are thin handles that forward into backend classes.
- **Native, GPU-accelerated rendering per platform.** Direct3D 11 with Direct2D 1.1 and DirectWrite on Windows; EGL with OpenGL ES 3.0 on Linux (Wayland), using Pango and Cairo to rasterize text. No bundled rasterizer or font engine.
- **Pure logic is separable.** Layout, pointer routing, XML, SVG parsing and key mapping are plain code. They have no window dependency, which is why they can be unit-tested directly. Even the GL canvas context is a template over its renderer, so tests can run it against a software renderer without a GPU.

## Map of the header

`liteui.hpp` is organized public-first, then implementation.

```
PUBLIC (always compiled)
  basic types          Color, Size, EdgeInsets, Dynamic<T>, enums, CanvasImage
  input types          Key, KeyModifiers, KeyEvent
  pointer types        PointerType, PointerEvent, WheelEvent, PointerCallback, Cursor
  pixel types          PixelFormat, ImageBuffer, DisplaySettings
  Style                layout + box + scroll + cursor properties
  leaf nodes           Text, TextInput, Image, Svg, Canvas, CanvasContext
  View                 the retained node, with View::Computed
  LiteUI               the window handle
  helper declarations  file pickers, liteui_clipboard, liteui_image, liteui_text

#ifdef LITEUI_IMPLEMENTATION
  profiler             liteui_prof
  file pickers         liteui_filepicker
  clipboard            liteui_clipboard
  image codecs         liteui_image (decode, probe, encode, convert)
  XML parser           liteui_xml
  SVG                  liteui_svg
  text backend         liteui_text
  canvas backends      Windows: CanvasContextImpl (Direct2D)
                       Linux:   liteui_canvas_gl::Renderer + GlCanvasContext<>  (LITEUI_CANVAS_GL, default)
                                or CanvasContextImpl (Cairo)                     (fallback)
  layout               liteui_layout
  pointer routing      liteui_pointer::Router (hit-testing, capture, clicks)
  window core          LiteUIImpl: state, update cycle, input dispatch
  window, Windows      Win32 window + WndProc + D3D11/DXGI swap chain + Direct2D painting
  window, Linux        Wayland + EGL/GLES 3.0 + xkbcommon + wl_touch + tablet_v2
  glue                 View::toView overloads, LiteUI forwarding methods, run()
#endif
```

This order is deliberate: each layer only uses things defined above it (layout uses the text backend for measurement; the window core uses layout, the pointer router and the canvas backends; the platform windows use the core).

## Public surface versus implementation

The public half of the header must compile everywhere without platform SDK headers, so it uses **opaque handles**:

- **`LiteUI`** holds a pointer to `LiteUIImpl`, which is only *defined* in the implementation section. Every `LiteUI` method is a one-line forwarder.
- **`CanvasContext`** is a public class with no data. Its methods are defined in the implementation as `static_cast<CanvasContextImpl *>(this)->...`. The context you receive in `onPaint` is really the platform's concrete canvas class (`CanvasContextImpl` on Windows and for the Cairo fallback; on Linux/GL, `CanvasContextImpl` is the production instantiation of `GlCanvasContext<Renderer>`). There are no virtual functions, so there is no vtable and no indirect call, only a cast the compiler can see through.
- **`View::Computed`** holds the backend's cached resources (text layouts, GPU textures, canvas bitmaps) as forward-declared or type-erased pointers. Releasing them is centralized in `View::freeTextResources()`, because `View`s are copied while a tree is built and a resource must be released exactly once.

The header also guards against being compiled twice with `LITEUI_IMPLEMENTATION_DONE`.

## The data model

### A tree of `View`s

The retained tree is made only of `View`s. `Text`, `TextInput`, `Image`, `Svg` and `Canvas` are **author-facing descriptions**. `View::addChild` *flattens* each into a `View` (a text node sets `isText`; canvas-backed nodes set `isCanvas` and an `onPaint`). The rest of the library never needs to know what kind of leaf it is looking at, only whether it is text or canvas.

`TextInput`, `Image` and `Svg` are built on the canvas path. Their `toView` functions install an `onPaint` that draws the text, bitmap or vector document into the node's surface.

### Ownership and copies

`addChild` and `setRoot` take their argument **by value**, so the window owns a private copy of the whole tree (`root_`). Callbacks you wrote capture your own variables by reference; the *tree* is never shared with your code. This keeps lifetime simple (no dangling parent pointers) at the cost of copy work during construction.

### Style and Computed

`View::style` is what you declared. `View::computed` is everything the library derives:

| Part of `computed` | Purpose |
|---|---|
| `x`, `y`, `w`, `h` | Final border box in window coordinates (written by layout). |
| `contentW/H`, `scrollX/Y` | Scroll extent and offset. |
| `resolved*` caches | The last value polled from each `Dynamic<>` field (width, height, margin, padding, colors, border, position edges, z-index, display, visibility, text, ...). |
| `dirty` and size stamps | Tell the renderer when a cached text layout or texture must be rebuilt. |
| Backend resources | Text layouts and textures, canvas surfaces and bitmaps, image caches. |
| `isHovered` | Maintained by the input code for hover colors and scrollbar auto-hide. |

The central trick is `resolveDynamic(field, cache)`: if the field holds a value, return it; if it holds a callback, return the **cached result of the last poll**. Layout and painting only ever call `resolveDynamic`. **User callbacks are never invoked during layout or painting**, only during polling, which makes frame work predictable and keeps a slow callback from stalling a paint.

## The update cycle

Everything that can change the screen goes through the same loop:

```
event (click, key, wheel, timer tick, wake, resize)
   |
   v
your handler runs                      (on the UI thread)
   |
   v
pollAndRelayout()
   |-- checkForUpdates(root)           walk the tree:
   |       keysSource  -> reconcile keyed children
   |       each Dynamic<T> callback -> compare with resolved* cache
   |       mark changed nodes dirty
   |-- if nothing changed: stop        (no layout, no paint)
   |-- if the tree's structure changed: drop pointers into the tree
   |-- honor focus requests (wantsFocus / TextInput requestFocus)
   |-- if only a dynamic Style::cursor changed: refresh the cursor image
   `-- relayout()                      measure + place, onLayout callbacks
   |
   v
repaint
```

Details that matter:

- **`requestRepaint()` is "poll, then repaint".** Timer ticks and wakes call it, which is why an interval callback that only changes your own variables still updates the screen. On Windows the repaint is `InvalidateRect` (painted on the next `WM_PAINT`); on Linux it redraws **synchronously**, and because the swap interval is 1 a redraw can block until the next vertical blank.
- **`setRoot` lays out twice.** It runs layout, polls once (so values captured through `onLayout` take effect), and lays out again, so the first paint is already correct.
- **Polling walks the whole tree.** There is no subscription or dependency tracking. Each event pays for one traversal plus every callback it contains. Keep callbacks cheap.
- **Keyed lists** reconcile inside `checkForUpdates`: `keysSource` is polled, the key vector is compared with the cached one, surviving children are moved (not rebuilt), new keys call `itemBuilder`, and removed children are freed.
- **A canvas redraws only when asked.** `canvasDirtySource` is polled in the same pass. See [CANVAS.md](CANVAS.md#when-onpaint-runs).

## Layout

Layout lives in `liteui_layout` and is pure: it reads `style` plus the polled caches and writes `computed`. It is two passes, **measure** (bottom-up natural sizes against the space offered) then **place** (top-down: basis sizes, grow/shrink, alignment, wrapping, absolute children, scroll extents). Text sizing calls into the text backend so layout and paint agree. The full rules are in [LAYOUT.md](LAYOUT.md).

Because layout is pure code with exported helpers (`resolveAxis`, `clampSize`, `axisScrollbarVisible`, `layoutRoot`), the unit tests drive it directly with fixed-size boxes. See [TESTING.md](TESTING.md).

## Rendering

A repaint **paints the whole window**. There is no damage tracking or partial invalidation: every paint clears the background and draws the full tree. The order is the same on both platforms:

1. Window background.
2. The in-flow tree, depth first. Each node paints background, border, its own content (text or canvas bitmap), then its children, then its scrollbars. Scroll containers clip their children to their box.
3. All absolutely positioned nodes, collected from the whole tree and sorted by `zIndex` then document order.
4. (Linux) the title bar, unless the window is frameless.
5. The tooltip.

### Windows

Everything the window and every canvas draws with hangs off **one Direct3D 11 device**, so a canvas texture is drawn into the window without ever leaving the GPU:

```
d3dDevice_ -> d2dDevice_ -> renderTarget_   window context (draws the back buffer)
                         \-> canvasDc_      canvas-node context (draws canvas textures)
d3dDevice_ + hwnd_ -> swapChain_ -> backBuffer_ (renderTarget_'s target)
```

Painting happens in `WM_PAINT`: `BeginDraw`, clear, paint the tree with a push/pop clip stack, draw absolutes, draw the tooltip, `EndDraw`, then `swapChain_->Present(1, 0)` (vsynced).

- **Two device contexts** exist because a canvas is rendered while the window's context is already inside `BeginDraw()`/`EndDraw()`, and a context cannot switch targets in the middle of a draw.
- **Hardware first, WARP as the fallback.** Device creation tries the GPU and falls back to Microsoft's software rasterizer, so the library still runs on a machine without a usable driver.
- **Device loss is recovered.** If drawing or `Present` reports `D2DERR_RECREATE_TARGET` or a removed/reset device, the window drops every device object (including each canvas texture, via `freeTextResources`) and rebuilds on the next paint. Canvases redraw themselves because their texture pointer is null.
- **Text** is laid out with DirectWrite. The `IDWriteTextLayout` is cached in `computed` and rebuilt only when the text, font or available width changes.
- **Canvases** render into a GPU bitmap (`ID2D1Bitmap1`) that **stays in video memory** and is redrawn only when `onPaint` reruns; a window repaint just draws that texture. Pixel readback (`getImageData`) is the expensive path.
- **Images** are decoded with WIC.

### Linux (Wayland)

Painting is done with **EGL and OpenGL ES 3.0** into a Wayland surface. A driver that cannot provide ES 3.0 makes startup fail with a clear message, and this holds even when `LITEUI_CANVAS_GL` is off.

- **Boxes** are quads drawn as triangle strips with small shaders, clipped with the scissor test.
- **Text** is laid out with **Pango**, rasterized by **Cairo** into an image, and uploaded to a GL texture that is cached per node and rebuilt when text, font or size changes.
- **Canvases** (default, `LITEUI_CANVAS_GL`) are drawn by one `liteui_canvas_gl::Renderer` shared by every canvas node: drawing is batched into triangles and rendered into per-canvas GPU textures, with glyph atlases for text. The canvas texture is then composited like any other texture, and a canvas redraws only when asked. With `LITEUI_CANVAS_GL` off, canvases are Cairo image surfaces uploaded to a texture only when they redraw.
- **Context loss is recovered.** If `eglSwapBuffers` reports `EGL_CONTEXT_LOST` (GPU reset, driver restart, suspend/resume), the window builds a fresh context, recreates its GL objects, marks every canvas for redraw and repaints. After **three consecutive losses** it gives up, prints a message, and the window stops updating.
- **Window chrome:** LiteUI draws its own 32 px title bar (minimize, maximize, close), because it does not negotiate server-side decorations.
- **Presentation** is `eglSwapBuffers` with a swap interval of 1 (vsync), so the compositor owns frame timing. `redraw()` is synchronous.
- **Images** are decoded with libpng and libjpeg.

## Event loop and input

### Windows

A standard Win32 message pump (`GetMessage` / `TranslateMessage` / `DispatchMessage`) drives a `WndProc` that handles mouse, wheel, keyboard, character, timer, paint, resize and close messages, plus a private wake message (`WM_APP + 1`). The mouse and touchpad use the classic `WM_MOUSE*` messages; **pen and touch arrive as `WM_POINTER*`** (Windows 8+), where the pen's batched history is read back so fast strokes keep every sample. Losing mouse capture (`WM_CAPTURECHANGED`) cancels every in-flight press.

### Linux

A single-threaded `poll()` loop sleeps on a set of file descriptors:

```
poll() over:
  - the Wayland display fd
  - the key-repeat timerfd
  - the wake eventfd
  - one timerfd per addInterval()
```

The Wayland display also carries touch (`wl_touch`) and pen (`zwp_tablet_v2`) events, which is why the build vendors the tablet protocol bindings.

Notable choices:

- **Pointer motion is coalesced.** Mouse motion events only record the latest position; `flushPendingMotion()` applies at most one hover/drag update and one redraw before the loop sleeps again, so a flood of motion events cannot queue up redraws. It also runs first thing in any press, release or axis handler, so a button event never sees a drag that is a few motion events behind.
- **Touch and pen report in frames.** Each finger and each pen tool folds a burst of events into one state per `frame` event. A frame that only moves is parked and applied once per loop iteration **with every sample it collected**, so a 400 Hz pen costs one handler call per display frame yet loses nothing (`PointerEvent::coalesced`). A frame that presses, lifts or changes a button first applies everything older, so ordering is preserved.
- **Key repeat is implemented in the library** with a timerfd, because Wayland delivers key press and release but leaves repeat to the client.
- **Keyboard input** is translated through xkbcommon: keysyms map to `Key` (a pure function, unit-tested), and composed text arrives as UTF-32 for `onTextInput`.
- The loop obeys the `wl_display_prepare_read` protocol, and anything that can redraw (and therefore read the display inside `eglSwapBuffers`) happens *outside* that window.

### Dispatch (shared logic)

The platform code translates native events into calls on shared `LiteUIImpl` methods:

- **Shortcuts** are checked first (exact key and modifier match; first match consumes the event), then the focused view's handlers.
- **Pointer routing lives in `liteui_pointer::Router`**, which has no window, platform or renderer dependency. `LiteUIImpl` owns one, and every backend feeds it `PointerEvent` and `WheelEvent` values for mouse, pen and touch alike. The router keeps **one state record per pointer** (the mouse, each finger down, each pen in range): which buttons are held, which view each button's press targeted, the capture target, and the click history for double-clicks.
- **Hit-testing** mirrors painting. For each event, `chainAt` builds the list of views that want it: the topmost absolutely positioned layer (highest `zIndex`, latest in document order) that has a matching view under the point wins, else the in-flow tree. The list runs from the deepest matching view up through its matching ancestors, and **that list is the bubbling order**. A layer with no match falls through to the layers below, so a view without a handler (or a disabled one) never blocks something beneath it. The walk applies the same scroll-ancestor clips as painting.
- **Capture.** A press makes the view that stopped propagation (else the deepest view with `onPointerDown`) the capture target; moves and the release go to it even outside its bounds, until the last button comes up or the press is cancelled.
- **Press, click and drag state is per pointer and per button**, so left, middle and right can be in different states at once, and several fingers can be in flight at once. `onClick` needs the release on the same view as the press; double-clicks use a 500 ms / 4 px window.
- **Focus** is moved by the router's `focus` callback on a left press, *before* any handler runs: the target is focused if it is focusable, otherwise focus is cleared. It is a single pointer to the focused view, and only that view receives keyboard handlers.
- **Scrolling** is resolved by the window core, not the router: it finds the deepest scrollable view under the pointer, handling the scrollbar track and thumb, wheel, and content panning (with a 4 px drag threshold so clicks still work). A press over scrollable content is offered to the scroll code first. A thumb or track grab consumes it; plain content arms a possible pan but the press still goes to the router. When the pan starts, the router is told to drop the pending click (`dropClick`), and the capture stays armed.
- **Wheel** events bubble through `onWheel` handlers; if none calls `stopPropagation()`, the built-in wheel scrolling applies.
- **Hover, cursor and tooltips:** hover state updates `isHovered` and drives enter/leave; a timer shows the tooltip after the configured delay. After motion and after a cancel, the window re-derives the **cursor image** (a window-edge resize handle, else the `Style::cursor` of what is under the mouse, or the **mouse's** capture target while a button is held; pens and fingers do not change it).

### Pointers into the tree

Input state is stored as **raw pointers into the tree**: the router's press and capture targets, `focusedView_`, the scroll-drag target and the tooltip target. Rebuilding children invalidates them, so whenever polling changes the tree's *structure* (a keyed list gaining or losing a key), `invalidateViewPointers()` clears all of them: it **silently** invalidates the router (a generation counter also makes a dispatch that is in progress stop bubbling), drops the scroll drag, nulls the focus and hides the tooltip. The consequences are described in [Sharp edges](#sharp-edges).

## Threading and timers

LiteUI is a single-threaded UI toolkit with one cross-thread door.

- **Everything runs on the UI thread**: handlers, `Dynamic<>` callbacks, `onPaint`, timers and the wake handler.
- **`wake()`** is the only thread-safe call. On Windows it posts the private message, guarded by an atomic flag so repeated wakes coalesce into one message in the queue. On Linux it writes to an `eventfd`; N pending wakes drain as one callback. Either way the loop then runs the wake handler and calls `requestRepaint()`.
- **Intervals** use `SetTimer` on Windows and one `timerfd` per interval on Linux. A tick runs your callback and then `requestRepaint()`, which polls and repaints.
- **Library timers use the same mechanism.** Tooltip delay and the text-input caret blink are ordinary intervals, created and removed through `addInterval` / `removeInterval`.

## Supporting subsystems

| Subsystem | Notes |
|---|---|
| **Text backend** (`liteui_text`) | Measures and lays out text for layout and paint, and answers `caretIndexForX`. DirectWrite on Windows, Pango on Linux. |
| **Canvas backends** | Direct2D on a GPU bitmap (Windows); the OpenGL ES 3.0 renderer (Linux default); Cairo (Linux fallback). Same method surface; behavior differences are catalogued in [CANVAS.md](CANVAS.md#platform-differences). |
| **Pointer router** (`liteui_pointer`) | Hit-testing, bubbling, capture, click and double-click pairing and cancel, per pointer. Unit-tested directly. |
| **XML parser** (`liteui_xml`) | A small, forgiving subset: elements, attributes, text, entities, CDATA, comments. It returns whatever it could read rather than failing hard, and deep nesting is bounded. |
| **SVG** (`liteui_svg`) | Parses into a `Document` of shapes (path operations plus paint and stroke state). The `Svg` node replays that document through a `CanvasContext` at the node's pixel size, which is why it stays crisp. Gradients, clip paths, masks and text are not modeled. |
| **Image codecs** (`liteui_image`) | WIC on Windows; libpng and libjpeg on Linux. Synchronous. Decodes to 8-bit `CanvasImage` or to a precision-preserving `ImageBuffer`, probes headers, encodes PNG and JPEG, converts between pixel formats and builds the 8-bit display copy that `Image::hdrSource` shows. See [API.md](API.md#liteui_image-decoding-encoding-and-display-conversion). |
| **Pickers and clipboard** | Native APIs on Windows. On Linux they shell out to `zenity` / `kdialog` and `wl-clipboard` / `xclip` / `xsel`, with a process-local clipboard fallback. |
| **Profiler** (`liteui_prof`) | Compile-time optional frame timing (the probes are in the Windows paint and mouse paths only today). See [PROFILING.md](PROFILING.md). |

## Platform strategy

The two backends are interleaved in one `LiteUIImpl` class using `#if defined(_WIN32)`. The *core* (tree, polling, layout call, hit-testing, scroll and focus logic) is shared, and each backend supplies a matching surface:

| Concern | Windows | Linux |
|---|---|---|
| Window and loop | Win32, message pump | Wayland xdg-shell, `poll()` loop |
| Painting | Direct2D on a D3D11 swap chain, in `WM_PAINT`, `Present(1, 0)` | EGL/OpenGL ES 3.0, synchronous `redraw()`, swap interval 1 |
| Text | DirectWrite | Pango + Cairo textures |
| Canvas | Direct2D on a GPU bitmap | GL renderer (default) or Cairo image surface |
| Pointer input | `WM_MOUSE*`; `WM_POINTER*` for pen and touch | `wl_pointer`; `wl_touch`; `zwp_tablet_v2` |
| Timers | `SetTimer` | `timerfd` |
| Wake | `PostMessage` | `eventfd` |
| Title bar | OS-provided (or custom if frameless) | Drawn by LiteUI (or custom if frameless) |
| Key mapping | virtual keys | xkbcommon keysyms |
| Images | WIC | libpng, libjpeg |
| GPU loss | Rebuild on next paint | Recreate context (gives up after 3 in a row) |

When you change behavior in the shared core, check both backends; when you change a backend, check that the other still presents the same observable semantics. Most of the documented differences in the guides are places where that parity is not yet complete.

## Build architecture

- **`liteui::liteui` is an `INTERFACE` library.** It contributes the include path, `cxx_std_23`, optional `LITEUI_PROFILE`, `LITEUI_CANVAS_GL` (Linux, on by default), and the platform libraries. The implementation is compiled inside the consuming executable.
- **Wayland protocol bindings are vendored** (`platform/wayland-protocols/`: `xdg-shell`, `xdg-decoration` and `tablet-unstable-v2`), so consumers need only the runtime libraries, not `wayland-scanner`. A `regen-protocols` target regenerates them. See [API.md](API.md#compile-time-switches-and-cmake) for the options and presets.
- **Tests compile the implementation themselves.** The core test suite is a single translation unit that defines `LITEUI_IMPLEMENTATION` so it can reach internals such as `liteui_layout`, `liteui_pointer` and `liteui_svg`; the GL canvas context is tested through a software renderer, with no GPU involved. See [TESTING.md](TESTING.md) and [BUILDING.md](BUILDING.md).

## Costs and trade-offs

| Choice | Benefit | Cost |
|---|---|---|
| Whole-window repaints | Simple, no damage bookkeeping, no stale pixels | Every repaint redraws everything; cost scales with node count. |
| Polling the entire tree after each event | No dependency tracking; callbacks "just work" | O(nodes) per event plus every callback. Heavy callbacks hurt. |
| Copy-on-add trees | No shared ownership or dangling parents | Construction copies; rebuilding a large tree is not free (keyed lists exist to avoid it). |
| No virtual dispatch for backends | No vtables, trivial inlining, small ABI | Two backends in one class body; changes touch `#if` blocks. |
| Single header | Trivial to vendor | Large header; implementation compile time lands in one TU. |
| GPU-resident canvases | A canvas is drawn once and composited cheaply; no per-paint upload | GPU memory scales with canvas pixel size; `getImageData` is a readback that flushes pending work. |
| One window per process | Simple global state | No multi-window support (`LiteUI::current()` is the latest window). |

## Sharp edges

These are places where the architecture's assumptions leak into behavior. They come from reading the code, and each is worth knowing when debugging.

- **Structural tree changes drop input state.** When a keyed list gains or loses a key, the window clears focus, any in-progress press or drag, and the tooltip. In-flight presses and drags simply stop: the views they targeted get **no** `onPointerUp` or `onPointerCancel`, because they may already be destroyed. Clearing focus does **not** call `onBlur`, so a `TextInput` that was focused can keep its focused border and blinking caret while no view actually has keyboard focus. To keep typing uninterrupted across list updates, ask for focus again from the same update (`TextInput`'s `state->requestFocus`, or a `wantsFocus` callback that returns `true`); the poll re-focuses after the rebuild.
- **Windows interval handles can collide.** `addInterval` on Windows picks the `SetTimer` id as the current number of timers plus one. After a removal, a new interval can receive an id that is still in use, which replaces the older timer's callback and period. Because tooltips and caret blinking are intervals too, add/remove sequences involving them can disturb user intervals. Linux uses a monotonically increasing handle and is not affected.
- **`setOnCloseRequest` is Windows-only** at present. Only the Win32 close path calls the handler.
- **Repaints on Linux are synchronous.** `requestRepaint()` redraws immediately and every timer tick triggers a full redraw, so a very short interval is a very short frame budget.
- **Both GPU paths can lose their device.** Windows rebuilds everything on the next paint. Linux recreates the GL context, but after three consecutive losses the window stops updating. Canvas contents are redrawn by their `onPaint` either way, so keep that code able to rerun from your own state.
- **Linux needs OpenGL ES 3.0 for the window itself**, not only for GL canvases. `-DLITEUI_CANVAS_GL=OFF` swaps the canvas backend but does not lower this requirement.
- **Some options are accepted but inert**, such as `setShadow`'s blur radius, and all `globalCompositeOperation` values except `SourceOver`, `Copy` and `Lighter` on Windows.

## Extending LiteUI

These are the touch points, taken from how the existing pieces are wired.

### Adding a `Style` property

1. Add the field to `Style`. Make it `Dynamic<T>` only if it must change at runtime.
2. If it is `Dynamic<>`, add a `resolved*` cache to `View::Computed` and a polling branch in `checkForUpdates`, so a change marks the node dirty.
3. Read it through `resolveDynamic(field, cache)` in layout and in **both** painters. Never call the callback during layout or paint.
4. If it affects layout, handle it in `measureNatural` / `placeNode`, and add a layout test with fixed-size boxes.

### Adding a leaf node type

1. Define the author-facing struct (with a `Style`).
2. Add a `View::addChild` overload and a `toView` that flattens it into a `View`. Prefer building on the canvas path (`isCanvas` plus an `onPaint`) so no new renderer code is needed.
3. Forward any handlers it should expose onto the `View`.

### Adding a `CanvasContext` method

1. Declare it in `CanvasContext`.
2. Implement it in **every backend**: the Direct2D `CanvasContextImpl`, `GlCanvasContext` (and whatever it needs from `liteui_canvas_gl::Renderer`, plus the software renderer the tests use), and the Cairo fallback `CanvasContextImpl`.
3. Add the forwarder `CanvasContext::name(...)` that casts to `CanvasContextImpl`.
4. Document any behavior difference in [CANVAS.md](CANVAS.md#platform-differences).

### Adding a pointer or input event

1. Declare the handler on `View` and on `Canvas` (both `PointerHandler` or `WheelHandler` members), and forward it in the `Canvas` to `View` flattening.
2. Dispatch it from `liteui_pointer::Router`, using `chainAt` with a predicate for "views that want this event" so transparency, disabling and bubbling stay consistent.
3. Feed it from **every** platform path that can produce it (Win32 mouse and `WM_POINTER*`, Wayland pointer, touch and tablet), and add a router test; the router needs no window.
4. Document it in [API.md](API.md#pointer-events).

### Adding or changing a platform backend

The shared core expects each backend to provide window creation and the event loop, native-to-shared event translation, painting in the order described above, the text backend and canvas context, timers, wake, and clipboard, pickers and image decoding. A new platform means a new branch of each of those under a new preprocessor condition, plus an entry in the CMake file for its libraries.
