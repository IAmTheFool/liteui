# Architecture

How `liteui.hpp` is put together, for people reading or changing the
library itself. If you just want to build a window, start with
[`GETTING_STARTED.md`](GETTING_STARTED.md); for field-by-field reference
see [`API.md`](API.md); for per-backend quirks see
[`PLATFORM_NOTES.md`](PLATFORM_NOTES.md).

## Design principles

- **One file to ship.** Users copy `liteui.hpp` and write one line
  (`#define LITEUI_IMPLEMENTATION`) in one `.cpp`.
- **Retained mode, no diffing layer.** The app builds a `View` tree once.
  State lives in ordinary variables the app owns; `Dynamic<T>` fields read
  them back by polling (see [The frame pipeline](#the-frame-pipeline)).
- **Native rendering, no abstraction layer.** Windows uses Direct2D and
  DirectWrite; Linux uses Wayland, EGL/GLES2, Cairo and Pango. Platform
  differences are selected at compile time with `#if defined(_WIN32)`.
  There are no virtual backend interfaces and no vtables.
- **Cheap to include.** The part of the header every file sees contains no
  platform headers and compiles no function bodies of any size.

## The two halves of the header

`liteui.hpp` is split in the style of `stb_*.h`.

```
liteui.hpp
├── Public half            (always visible; guarded by LITEUI_HPP_INCLUDED)
│     standard headers only, no platform headers
│     Color / Key / KeyEvent / KeyModifiers
│     Dynamic<T>, Size, EdgeInsets, Style, Text, TextStyle
│     CanvasImage, FileFilter, CanvasGradient, enums
│     CanvasContext   (declarations only)
│     Canvas, Image, TextInput, Svg   (plain data structs)
│     View            (the tree node)
│     TooltipStyle, LiteUI            (declarations only)
│     declarations of the free functions: file pickers, liteui_clipboard,
│       liteui_image::decode*, liteui_text::caretIndexForX
│
└── Implementation half    (only where LITEUI_IMPLEMENTATION is defined)
      platform headers and the remaining standard headers
      Win32 helpers · optional frame profiler (LITEUI_PROFILE)
      file pickers · clipboard · image decoding (WIC / libpng+libjpeg)
      XML parser · SVG parser/renderer
      text backend (DirectWrite / Pango)
      CanvasContextImpl   (one definition per platform branch)
      layout engine (liteui_layout) · xkb key mapping (Linux)
      LiteUIImpl          (the window: Win32 or Wayland+EGL)
      out-of-line definitions, including the public facades
```

The implementation half is compiled in exactly one translation unit: the
file that defines `LITEUI_IMPLEMENTATION`; the library has no separate
implementation `.cpp`, each executable provides that file (for example its
`main.cpp`). A second definition of the macro in the same program is a
duplicate-symbol link error by design.

## The public/implementation boundary

Anything an application can name lives in the public half; everything that
needs a platform header lives behind the macro. Four patterns bridge the
gap.

### 1. `LiteUI` is a handle to `LiteUIImpl`

`LiteUI` is the class applications construct. It holds a single
`LiteUIImpl *p_` and every public method is a one-line forwarder defined in
the implementation half. `LiteUIImpl` is the real window class with all the
Win32 or Wayland state in it. `LiteUI::current()` returns the public handle;
internal code uses `LiteUIImpl::current()`.

### 2. `CanvasContext` is a base class of `CanvasContextImpl`

Applications receive a `CanvasContext&` in `onPaint`. The public
`CanvasContext` declares the drawing methods but has no data and a protected
constructor. Each platform branch defines
`class CanvasContextImpl : public CanvasContext`, holding the
`ID2D1RenderTarget*` or `cairo_t*` and the real method bodies. The library
creates a `CanvasContextImpl` and passes it as the base reference. Every
public method is a small non-inline forwarder:

```cpp
void CanvasContext::moveTo(float x, float y) {
  static_cast<CanvasContextImpl *>(this)->moveTo(x, y);
}
```

The `static_cast` is well-defined because every `CanvasContext` the
application ever sees really is the base of a `CanvasContextImpl`. Inside
the implementation, code that holds a `CanvasContextImpl` calls its own
methods directly.

### 3. `View` stays a plain struct with opaque platform pointers

`View` is public and has no user-declared constructors, but its `computed` block caches
platform objects (a DirectWrite text layout, a Direct2D render target, a
GL texture, a Cairo surface). Those members are pointers to forward-declared
types (`struct IDWriteTextLayout;`, `typedef struct _cairo_surface
cairo_surface_t;`, and `unsigned int` in place of `GLuint`), so the public
half never needs the platform headers. `View::freeTextResources()` and the
`toView` conversions call into the platform, so they are declared in the
class and defined in the implementation half.

### 4. Free functions are declared publicly, defined once

Public free functions such as `openFilePicker` or
`liteui_image::decodeFile` are declared (with their default arguments) in
the public half and defined, **without `inline`**, in the implementation
half. Internal helpers keep `inline` or live in `detail` namespaces.

### Rules that keep the boundary intact

- No platform header, platform type or `LITEUI_PROF_*` macro in the public
  half. The only platform names there are the opaque forward declarations.
- No non-template, non-inline function definitions in the public half, or
  two files including the header would define the same symbol.
- A function that is declared in the public half must not be defined
  `inline` in the implementation half; the compiler may then never emit it.
- Default arguments are written once, on the public declaration, never on
  the definition.

## Data model

- **`Style`** holds layout and appearance. Most fields are `Dynamic<T>`.
- **`Dynamic<T>`** is `std::variant<T, std::function<T()>>`. Plain values are
  used as-is; callbacks are polled. The last polled result is cached in the
  node's `computed.resolved*` fields so painting never calls user code.
- **`View`** is the tree node: `style`, `children`, event handlers, and a
  `computed` block that the layout and paint passes fill in (absolute
  window coordinates, scroll extents, hover/dirty flags, cached platform
  resources). Renderers only read `computed`.
- **Author-facing leaves** (`Text`, `Canvas`, `TextInput`, `Image`, `Svg`)
  are plain structs. `View::addChild` converts them with `toView`: `Text`
  becomes a text node, and the other four become `isCanvas` nodes whose
  `onPaint` draws them through `CanvasContext`.
- **Keyed lists** (`keysSource` / `itemBuilder`) are reconciled by
  `reconcileChildren`: surviving keys are moved over untouched, new keys are
  built, dropped keys are freed.

## The frame pipeline

There is no render loop to write and no per-frame callback. Work happens in
response to events:

```
platform event ──► dispatch (hit test, focus, shortcuts)
  (Win32 message          │
   or Wayland listener)   ▼
                    app handler runs (mutates app variables)
                          │
timer tick / wake() ──────┤
                          ▼
                  pollAndRelayout()
                    checkForUpdates(root)   poll every Dynamic<T>,
                          │                 write changes back, mark dirty
                          ▼ (if anything changed)
                    reconcileChildren       keyed lists only
                    relayout()              liteui_layout::layoutRoot
                          │
                          ▼
                       redraw()
                    Windows: paintView  (Direct2D)
                    Linux:   renderView (GLES2 + cached textures)
```

`pollAndRelayout` is the one place that polls dynamic sources and applies
the result; `requestRepaint()` calls it too. If a structural change
(a keyed list) happened, pointers to the old tree held by press, drag,
focus and tooltip state are cleared first so nothing dangles.

### Layout

`liteui_layout::layoutRoot` runs a flexbox subset in two passes:
`measureNatural` computes each node's natural size bottom-up (resolving
`Size` kinds with `resolveAxis`), then `placeNode` assigns absolute window
coordinates top-down, writing them to `computed`. Scroll containers measure
their children at natural size and track `contentW/H` and
`scrollX/Y`. `Position::Absolute` nodes are gathered globally
(`collectAbsolutes`, `sortAbsolutes` by `zIndex`) and are painted and
hit-tested in a separate pass on top of the flow tree, using the same
ordering so what is visually on top is what receives the click.

### Hit testing

`hitTestFlow` walks the tree back-to-front with clip rectangles so scrolled
out content is not hit; `hitTestTooltipFlow` and `resolveScrollTarget`
follow the same clip-aware descent. `updateHover` mirrors it so hover state
matches what the pointer would actually click.

## Platform backends

### Windows

- **Window and loop:** a Win32 window; `run()` is the standard
  `GetMessage` / `DispatchMessage` loop and `WndProc` is the event entry
  point. Interval timers use `SetTimer`.
- **Painting:** Direct2D on the window's render target. Text uses cached
  DirectWrite layouts (`computed.textLayout`), rebuilt when the width, text
  or font values they were built for change.
- **Canvas nodes:** each gets an offscreen Direct2D render target backed by a
  WIC bitmap, so `CanvasContextImpl` can lock real pixels for
  `getImageData` and `clearRect`. Each paint blits the result into the
  window target.
- **Decorations:** native.

### Linux (Wayland)

- **Window and loop:** `xdg-shell` toplevel with the protocol bindings
  vendored under `platform/wayland-protocols/`. `run()` is a `poll()` loop
  over the Wayland display fd, one `timerfd` per interval, and an `eventfd`
  used by `wake()`. Pointer motion is coalesced (`flushPendingMotion`) and
  key repeat is driven client-side.
- **Input:** `wl_pointer`, `wl_keyboard` and `xkbcommon`; `xkbKeysymToKey`
  maps xkb keysyms to the platform-independent `Key` enum.
- **Painting:** an EGL/GLES2 surface. Rectangles are drawn with a small GL
  shader. Text is laid out by Pango, rasterized by Cairo into an image
  surface and uploaded as a cached GL texture. Canvas nodes draw through
  Cairo into a CPU image surface that is uploaded as a texture; keeping the
  surface is what makes `getImageData` a plain read.
- **Decorations:** a client-side titlebar drawn in-process (`drawTitlebar`),
  since Wayland has no server-side decoration guarantee.

## Threading

Everything runs on the UI thread. The single exception is
`LiteUI::wake()`, which any thread may call to make the loop wake, run the
optional wake handler, then poll and repaint. On Windows it posts a message
with a coalescing flag; on Linux it writes to the `eventfd`. Multiple wakes
before the UI thread runs produce one callback, and none is lost.

## Resource lifetime

`View` has no destructor that frees platform resources, because views are
copied freely while a tree is built (`root.addChild(heading)` copies
`heading`) and an auto-freeing destructor would double-release across those
copies. `freeTextResources()` frees cached textures, layouts and surfaces
explicitly, and is called from `setRoot()` and `~LiteUI()`, the only places
a live tree is retired.

## Subsystems

| Subsystem | Namespace | Notes |
|---|---|---|
| File and folder pickers | `liteui_filepicker` (the free functions are the public API) | `IFileOpenDialog`/`IFileSaveDialog` on Windows; shells out to `zenity`/`kdialog` on Linux. All five pickers build a `Request` and go through one platform-specific `pick()`; to add a mode, extend `Mode` and both `pick()` implementations |
| Clipboard | `liteui_clipboard` | Win32 clipboard; `wl-copy`/`wl-paste`/`xclip`/`xsel` on Linux; silent in-process fallback |
| Image decoding | `liteui_image` | WIC on Windows; libpng + libjpeg on Linux |
| XML | `liteui_xml` | Intentionally partial parser, used by SVG |
| SVG | `liteui_svg` | Parsed once at `addChild`, replayed through `CanvasContext` on every paint |
| Text measurement | `liteui_text` | DirectWrite or Pango; `caretIndexForX` is public |
| Frame profiler | `liteui_prof` | Compile-time `LITEUI_PROFILE`; compiles to nothing when off |

## Build integration

- **CMake:** `liteui::liteui` is a header-only `INTERFACE` library with no
  sources of its own. It carries the include directory, `cxx_std_23`, the
  optional `LITEUI_PROFILE` define, the platform libraries, and (on Linux)
  the vendored `liteui_wayland_protocols` static library, all as
  `INTERFACE` properties, so they propagate to every target that links it.
  The consumer defines `LITEUI_IMPLEMENTATION` in one `.cpp`, so the
  implementation is compiled inside the consumer. Examples are
  `examples/<name>/{CMakeLists.txt,main.cpp}`, globbed at configure time.
- **Protocol bindings:** `platform/wayland-protocols/` holds the generated
  xdg-shell and xdg-decoration sources; the `regen-protocols` target
  rebuilds them and the result must be committed.
- **Without CMake:** copy the header and define the macro in one `.cpp`; see
  the [README](../README.md#how-the-single-header-works).

## Making changes

**Add a method to `LiteUI`.** Declare it in the public `LiteUI` class, add the
real method to `LiteUIImpl`, and add a one-line forwarder next to the others
at the end of the implementation half.

**Add a method to `CanvasContext`.** Declare it (with any default arguments)
in the public `CanvasContext`, implement it in **both** `CanvasContextImpl`
definitions with the same signature, and add the `static_cast` forwarder.
Then document it in `API.md` and note any backend difference in
`PLATFORM_NOTES.md`.

**Add a public free function.** Declare it in the public half; define it in
the implementation half without `inline` and without repeating default
arguments.

**Add a platform-specific feature.** Keep it in the implementation half,
inside the existing `#if defined(_WIN32)` / `#else` structure. If a `View`
needs to cache a platform object, add an opaque pointer to
`View::computed` using a forward-declared type, and release it in
`freeTextResources()`.

**Check your change.** A file that only includes `liteui.hpp` must still
compile with no platform headers on the include path. Compiling two such
files into one program must not produce duplicate symbols.