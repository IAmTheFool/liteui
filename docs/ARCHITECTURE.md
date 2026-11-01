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
- **No runtime polymorphism for platforms.** Windows and Linux are selected with `#if defined(_WIN32)`, not virtual interfaces. The public `LiteUI` and `CanvasContext` classes are thin handles that forward into backend classes.
- **Native rendering per platform.** Direct2D/DirectWrite on Windows; EGL/GLES2 with Cairo and Pango on Linux (Wayland). No bundled rasterizer or font engine.
- **Pure logic is separable.** Layout, XML, SVG parsing and key mapping are plain code. They have no window dependency, which is why they can be unit-tested directly.

## Map of the header

`liteui.hpp` is organized public-first, then implementation.

```
PUBLIC (always compiled)
  basic types          Color, Size, EdgeInsets, Dynamic<T>, enums
  input types          Key, KeyModifiers, KeyEvent
  Style                layout + box + scroll properties
  leaf nodes           Text, TextInput, Image, Svg, Canvas, CanvasContext
  View                 the retained node, with View::Computed
  LiteUI               the window handle
  helper declarations  file pickers, liteui_clipboard, liteui_image, liteui_text

#ifdef LITEUI_IMPLEMENTATION
  profiler             liteui_prof
  file pickers         liteui_filepicker
  clipboard            liteui_clipboard
  image decoding       liteui_image
  XML parser           liteui_xml
  SVG                  liteui_svg
  text backend         liteui_text
  canvas backends      CanvasContextImpl (Direct2D | Cairo)
  layout               liteui_layout
  window core          LiteUIImpl: state, update cycle, input dispatch
  window, Windows      Win32 window + WndProc + Direct2D painting
  window, Linux        Wayland + EGL/GLES2 + xkbcommon
  glue                 View::toView overloads, LiteUI forwarding methods, run()
#endif
```

This order is deliberate: each layer only uses things defined above it (layout uses the text backend for measurement; the window core uses layout and the canvas backends; the platform windows use the core).

## Public surface versus implementation

The public half of the header must compile everywhere without platform SDK headers, so it uses **opaque handles**:

- **`LiteUI`** holds a pointer to `LiteUIImpl`, which is only *defined* in the implementation section. Every `LiteUI` method is a one-line forwarder.
- **`CanvasContext`** is a public class with no data. Its methods are defined in the implementation as `static_cast<CanvasContextImpl *>(this)->...`. The context you receive in `onPaint` is really a `CanvasContextImpl`. There are no virtual functions, so there is no vtable and no indirect call, only a cast the compiler can see through.
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
   `-- relayout()                      measure + place, onLayout callbacks
   |
   v
repaint
```

Details that matter:

- **`requestRepaint()` is "poll, then repaint".** Timer ticks and wakes call it, which is why an interval callback that only changes your own variables still updates the screen. On Windows the repaint is `InvalidateRect` (painted on the next `WM_PAINT`); on Linux it redraws **synchronously**.
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

Painting happens in `WM_PAINT` on a Direct2D `HwndRenderTarget`: `BeginDraw`, clear, paint the tree with a push/pop clip stack, draw absolutes, draw the tooltip, `EndDraw`.

- **Text** is laid out with DirectWrite. The `IDWriteTextLayout` is cached in `computed` and rebuilt only when the text, font or available width changes.
- **Canvases** draw into a **CPU bitmap**: a WIC bitmap with a software Direct2D render target. When the window paints, that bitmap is converted to a GPU bitmap and drawn. This happens on **every window paint**, not only when the canvas redrew, which is the main cost of large canvases on Windows.
- **Images** are decoded with WIC.

### Linux (Wayland)

Painting is done with **EGL and GLES2** into a Wayland surface.

- **Boxes** are quads drawn as triangle strips with small shaders, clipped with the scissor test.
- **Text** is laid out with **Pango**, rasterized by **Cairo** into an image, and uploaded to a GL texture that is cached per node and rebuilt when text, font or size changes.
- **Canvases** are Cairo image surfaces, uploaded to a texture **only when the canvas redraws**.
- **Window chrome:** LiteUI draws its own 32 px title bar (minimize, maximize, close), because it does not negotiate server-side decorations.
- **Presentation** is `eglSwapBuffers`, so the compositor owns frame timing. `redraw()` is synchronous.
- **Images** are decoded with libpng and libjpeg.

## Event loop and input

### Windows

A standard Win32 message pump (`GetMessage` / `TranslateMessage` / `DispatchMessage`) drives a `WndProc` that handles pointer, wheel, keyboard, character, timer, paint, resize and close messages, plus a private wake message (`WM_APP + 1`).

### Linux

A single-threaded `poll()` loop sleeps on a set of file descriptors:

```
poll() over:
  - the Wayland display fd
  - the key-repeat timerfd
  - the wake eventfd
  - one timerfd per addInterval()
```

Notable choices:

- **Pointer motion is coalesced.** Motion events only record the latest position; `flushPendingMotion()` applies at most one hover/drag update and one redraw before the loop sleeps again, so a flood of motion events cannot queue up redraws.
- **Key repeat is implemented in the library** with a timerfd, because Wayland delivers key press and release but leaves repeat to the client.
- **Keyboard input** is translated through xkbcommon: keysyms map to `Key` (a pure function, unit-tested), and composed text arrives as UTF-32 for `onTextInput`.
- The loop obeys the `wl_display_prepare_read` protocol, and anything that can redraw (and therefore read the display inside `eglSwapBuffers`) happens *outside* that window.

### Dispatch (shared logic)

The platform code translates native events into calls on shared `LiteUIImpl` methods:

- **Shortcuts** are checked first (exact key and modifier match; first match consumes the event), then the focused view's handlers.
- **Hit-testing** tests absolutes first (highest `zIndex` down), then the flow tree, and returns the topmost view with a handler for the button. Clicks **bubble** to the nearest ancestor with a handler.
- **Press and drag state** is tracked **per mouse button** (`pressedView_[3]`, `dragView_[3]`), so left, middle and right can be in different states at once. A drag only starts on a view that was hit by the press.
- **Scrolling** resolves the deepest scrollable view under the pointer, handling the scrollbar track and thumb, wheel, and content panning (with a small drag threshold so clicks still work).
- **Hover and tooltips:** hover state updates `isHovered`; a timer shows the tooltip after the configured delay.
- **Focus** is a single pointer to the focused view. Only that view receives keyboard handlers.

### Pointers into the tree

The input state above is stored as **raw pointers into the tree** (`pressedView_`, `dragView_`, `focusedView_`, the scroll-drag target, the tooltip target). Rebuilding children invalidates them, so whenever polling changes the tree's *structure* (a keyed list gaining or losing a key), the window **clears all of them** and hides the tooltip. The consequences are described in [Sharp edges](#sharp-edges).

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
| **Canvas backends** (`CanvasContextImpl`) | Direct2D with a WIC-backed target, or Cairo. Same method surface; behavior differences are catalogued in [CANVAS.md](CANVAS.md#platform-differences). |
| **XML parser** (`liteui_xml`) | A small, forgiving subset: elements, attributes, text, entities, CDATA, comments. It returns whatever it could read rather than failing hard, and deep nesting is bounded. |
| **SVG** (`liteui_svg`) | Parses into a `Document` of shapes (path operations plus paint and stroke state). The `Svg` node replays that document through a `CanvasContext` at the node's pixel size, which is why it stays crisp. Gradients, clip paths, masks and text are not modeled. |
| **Image decoding** (`liteui_image`) | WIC on Windows; libpng and libjpeg on Linux. Synchronous, returns straight-alpha RGBA. |
| **Pickers and clipboard** | Native APIs on Windows. On Linux they shell out to `zenity` / `kdialog` and `wl-clipboard` / `xclip` / `xsel`, with a process-local clipboard fallback. |
| **Profiler** (`liteui_prof`) | Compile-time optional frame timing (Windows probes only today). See [PROFILING.md](PROFILING.md). |

## Platform strategy

The two backends are interleaved in one `LiteUIImpl` class using `#if defined(_WIN32)`. The *core* (tree, polling, layout call, hit-testing, scroll and focus logic) is shared, and each backend supplies a matching surface:

| Concern | Windows | Linux |
|---|---|---|
| Window and loop | Win32, message pump | Wayland xdg-shell, `poll()` loop |
| Painting | Direct2D in `WM_PAINT` | EGL/GLES2, synchronous `redraw()` |
| Text | DirectWrite | Pango + Cairo textures |
| Canvas | Direct2D on WIC bitmap | Cairo image surface |
| Timers | `SetTimer` | `timerfd` |
| Wake | `PostMessage` | `eventfd` |
| Title bar | OS-provided (or custom if frameless) | Drawn by LiteUI (or custom if frameless) |
| Key mapping | virtual keys | xkbcommon keysyms |
| Images | WIC | libpng, libjpeg |

When you change behavior in the shared core, check both backends; when you change a backend, check that the other still presents the same observable semantics. Most of the documented differences in the guides are places where that parity is not yet complete.

## Build architecture

- **`liteui::liteui` is an `INTERFACE` library.** It contributes the include path, `cxx_std_23`, optional `LITEUI_PROFILE`, and the platform libraries. The implementation is compiled inside the consuming executable.
- **Wayland protocol bindings are vendored** (`platform/wayland-protocols/`), so consumers need only the runtime libraries, not `wayland-scanner`.
- **Tests compile the implementation themselves.** The core test suite is a single translation unit that defines `LITEUI_IMPLEMENTATION` so it can reach internals such as `liteui_layout` and `liteui_svg`. See [TESTING.md](TESTING.md) and [BUILDING.md](BUILDING.md).

## Costs and trade-offs

| Choice | Benefit | Cost |
|---|---|---|
| Whole-window repaints | Simple, no damage bookkeeping, no stale pixels | Every repaint redraws everything; cost scales with node count. |
| Polling the entire tree after each event | No dependency tracking; callbacks "just work" | O(nodes) per event plus every callback. Heavy callbacks hurt. |
| Copy-on-add trees | No shared ownership or dangling parents | Construction copies; rebuilding a large tree is not free (keyed lists exist to avoid it). |
| No virtual dispatch for backends | No vtables, trivial inlining, small ABI | Two backends in one class body; changes touch `#if` blocks. |
| Single header | Trivial to vendor | Large header; implementation compile time lands in one TU. |
| One window per process | Simple global state | No multi-window support (`LiteUI::current()` is the latest window). |

## Sharp edges

These are places where the architecture's assumptions leak into behavior. They come from reading the code, and each is worth knowing when debugging.

- **Structural tree changes drop input state.** When a keyed list gains or loses a key, the window clears focus, any in-progress press or drag, and the tooltip. In-flight drags end without `onDragEnd`. Clearing focus does **not** call `onBlur`, so a `TextInput` that was focused can keep its focused border and blinking caret while no view actually has keyboard focus. To keep typing uninterrupted across list updates, ask for focus again from the same update (`TextInput`'s `state->requestFocus`, or a `wantsFocus` callback that returns `true`); the poll re-focuses after the rebuild.
- **Windows interval handles can collide.** `addInterval` on Windows picks the `SetTimer` id as the current number of timers plus one. After a removal, a new interval can receive an id that is still in use, which replaces the older timer's callback and period. Because tooltips and caret blinking are intervals too, add/remove sequences involving them can disturb user intervals. Linux uses a monotonically increasing handle and is not affected.
- **`setOnCloseRequest` is Windows-only** at present. Only the Win32 close path calls the handler.
- **Repaints on Linux are synchronous.** `requestRepaint()` redraws immediately and every timer tick triggers a full redraw, so a very short interval is a very short frame budget.
- **Canvas upload cost differs.** Windows re-uploads each canvas bitmap on every window paint; Linux uploads only on redraw.
- **Some options are accepted but inert**, such as `setShadow`'s blur radius, and `globalCompositeOperation` on Windows.

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
2. Implement it in **both** `CanvasContextImpl` classes (Direct2D and Cairo).
3. Add the forwarder `CanvasContext::name(...)` that casts to `CanvasContextImpl`.
4. Document any behavior difference in [CANVAS.md](CANVAS.md#platform-differences).

### Adding or changing a platform backend

The shared core expects each backend to provide window creation and the event loop, native-to-shared event translation, painting in the order described above, the text backend and canvas context, timers, wake, and clipboard, pickers and image decoding. A new platform means a new branch of each of those under a new preprocessor condition, plus an entry in the CMake file for its libraries.
