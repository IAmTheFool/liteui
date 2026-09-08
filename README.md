# LiteUI

A single-header, cross-platform C++ UI library. No dependencies to manage beyond your platform's native stack — just `#include <liteui.hpp>` and go.

- **Windows**: renders with Direct2D + DirectWrite
- **Linux**: renders with Wayland + EGL/GLES2 (text/vector fallback via Cairo + Pango)

One `LiteUI` class definition exists per build — the platform is picked with `#ifdef`, so there's no vtable, no `Impl` pointer, no indirection. What you write is what runs.

## Requirements

- C++23
- **Windows**: `d2d1`, `dwrite` (linked automatically via `#pragma comment`)
- **Linux**: `wayland-client`, `wayland-cursor`, `wayland-egl`, `egl`, `glesv2`, `cairo`, `pangocairo`, `xkbcommon`, plus generated `xdg-shell` / `xdg-decoration` protocol bindings (see the provided `CMakeLists.txt`)

## Quick start

```cpp
#include <liteui.hpp>

int main() {
    LiteUI ui(800, 600, "My App");

    View root;
    root.style.direction = FlexDirection::Column;
    root.style.padding = EdgeInsets::all(16);
    root.style.gap = 12;

    Text heading;
    heading.label = "Hello, LiteUI!";
    heading.fontSize = 24;
    heading.fontWeight = FontWeight::Bold;
    root.addChild(heading);

    View button;
    button.style.width = Size::pixel(120);
    button.style.height = Size::pixel(40);
    button.style.backgroundColor = Color{50, 120, 220};
    button.style.borderRadius = 8;
    button.onClick = [] { printf("clicked!\n"); };
    root.addChild(button);

    ui.setRoot(std::move(root));
    ui.run(); // blocks, pumps the platform event loop
}
```

## Core concepts

### `View` — the tree

Everything on screen is a `View`. Build a tree, hand the root to `ui.setRoot()`, and the library lays it out, paints it, and hit-tests it for you.

```cpp
View container;
container.addChild(childView);   // a plain View
container.addChild(Text{...});   // flattened into a text-leaf View
container.addChild(Canvas{...}); // flattened into a canvas-leaf View
```

`Text` and `Canvas` are author-facing convenience structs — they get flattened into a `View` the moment you call `addChild`, and never appear in the retained tree themselves.

### Layout — a flexbox subset

`Style` controls sizing and layout, and mirrors CSS flexbox on purpose:

```cpp
style.direction    = FlexDirection::Row;   // or Column
style.justifyContent = Justify::SpaceBetween;
style.alignItems    = Align::Center;
style.flexWrap      = FlexWrap::Wrap;
style.gap           = 8;
style.flexGrow      = 1;
style.flexShrink    = 1;
```

**Sizing** (`Size`) has four kinds, set per-axis via `style.width` / `style.height`:

| Kind | Meaning |
|---|---|
| `Size::pixel(v)` | fixed size, in pixels |
| `Size::percentage(v)` | percentage of parent's resolved size |
| `Size::fit()` | shrink-to-fit children (default) |
| `Size::full()` | fill available space (like `width: 100%` on a flex item) |

`minWidth`/`maxWidth`/`minHeight`/`maxHeight` clamp the result. `margin`/`padding` use `EdgeInsets`.

**Positioning**: `Position::Static` (default, flows in the flexbox) or `Position::Absolute` (pulled out of flow, placed via `left`/`top`/`right`/`bottom` against the parent's content box, stacked by `zIndex` across the *whole tree*, not just siblings).

**Scrolling**: set `style.overflowX` / `style.overflowY` to `Hidden`, `Scroll`, or `Auto` to turn a view into a scroll container. Scrollbars, drag-to-scroll, and wheel scrolling are handled for you.

**Visibility**: `Display::None` removes a node from layout entirely (no space reserved); `Visibility::Hidden` reserves its space but skips painting/hit-testing.

### Events

Handlers are plain `std::function` members on `View`:

```cpp
view.onClick       = [] { ... };          // left click
view.onMiddleClick, onRightClick          // other buttons
view.onPressAt     = [](float x, float y) { ... }; // local coords, fires on press
view.onDragTo      = [](float x, float y) { ... }; // fires while dragging
view.onScrollUp / onScrollDown            // discrete wheel notches
view.onLayout      = [](float x, float y, float w, float h) { ... };
```

Clicks **bubble**: if the deepest view under the pointer has no handler, the nearest ancestor that does have one fires instead.

**Keyboard** requires opting in:

```cpp
view.focusable = true;
view.onFocus = [] { ... };
view.onKeyDown = [](KeyEvent e) { ... };
view.onTextInput = [](uint32_t codepoint) { ... }; // composed text, not raw keys
```

Global shortcuts (work regardless of focus):

```cpp
ui.addShortcut({.ctrl = true}, Key::S, [] { save(); });
```

### Canvas — immediate-mode 2D drawing

For custom drawing, add a `Canvas` node and draw HTML5-canvas-style:

```cpp
Canvas c;
c.style.width = Size::full();
c.style.height = Size::pixel(300);
c.onPaint = [](CanvasContext &ctx) {
    ctx.setFillColor(Color{255, 0, 0});
    ctx.fillRect(10, 10, 100, 100);
    ctx.beginPath();
    ctx.arc(50, 50, 40, 0, 3.14159f);
    ctx.stroke();
};
root.addChild(c);
```

`CanvasContext` supports paths, gradients, transforms, text, images, and pixel data (`getImageData`/`putImageData`) — see the header's own comment block above `CanvasContext` for the small list of platform differences (e.g. `clearRect` clips per-rectangle on Linux but clears the whole surface on Windows).

`onPaint` is **not** a per-frame render loop — it only re-runs when something asks it to. From inside an interactive canvas handler, set a dirty flag and report it via `canvasDirtySource`:

```cpp
bool dirty = false;
c.onDragTo = [&](float x, float y) { addPoint(x, y); dirty = true; };
c.canvasDirtySource = [&] { bool d = dirty; dirty = false; return d; };
```

## State management

LiteUI has **no** internal reactive/observable system — state lives wherever your app keeps it (plain variables, a struct, whatever). Two mechanisms connect that state to the UI:

### 1. `Dynamic<T>` — poll-based bindings

Most `Style`/`Text` fields (`backgroundColor`, `width`, `text`, `disabled`, `left`, `display`, etc.) are `Dynamic<T> = std::variant<T, std::function<T()>>`. Assign a plain value for something static, or a lambda for something that should track app state:

```cpp
bool isOn = false;

Text label;
label.label = [&]() -> std::string { return isOn ? "ON" : "OFF"; };

View toggle;
toggle.style.backgroundColor = [&]() -> Color {
    return isOn ? Color{0, 200, 0} : Color{200, 0, 0};
};
toggle.onClick = [&] { isOn = !isOn; };
```

After **any** event handler runs (click, key, drag, scroll...), LiteUI automatically walks the tree, re-invokes every `Dynamic` callback, and diffs the result against the cached value. If anything changed, it marks the node dirty and re-lays-out/repaints — you never call this yourself.

### 2. Manual mutation + `requestCanvasRedraw()`

For canvas content, or if you're holding a live pointer into the tree (e.g. from an `onLayout` capture), you can mutate a `View` directly and ask for a redraw:

```cpp
someView.style.backgroundColor = newColor;
someView.requestCanvasRedraw(); // only meaningful for canvas nodes
```

### Summary

| You want... | Use |
|---|---|
| A value that changes in response to some event | `Dynamic<T>` lambda |
| A one-off, unchanging value | plain value assignment |
| Custom drawing that redraws on demand | `Canvas::onPaint` + `canvasDirtySource` |
| To read a view's final on-screen box | `onLayout` callback |

## How the header works internally

- **Two-pass layout** (`liteui_layout::measureNatural` then `placeNode`): a bottom-up sizing pass followed by a top-down placement pass that distributes flex space, wraps lines, and resolves scrolling — the same shape as the CSS flexbox algorithm.
- **`View::Computed`** holds everything layout/paint derive: final pixel box, scroll offsets, resolved `Dynamic` values, and platform-specific cached resources (a `IDWriteTextLayout*`/GL texture per text node, etc.).
- **One `LiteUI` definition per build.** Platform members (window handles, GL/D2D state, Wayland listeners) are compiled in via `#ifdef _WIN32` — there's exactly one code path per platform, not a runtime abstraction layer.
- **Hit-testing, hover, and scroll resolution** all walk the same tree shape (clip-aware, bubbling), so a click, a hover highlight, and a scrollbar drag all agree about what's actually visible.
- **Repaint is request-driven**, not a fixed frame loop: `run()` blocks on the platform's native event queue and only repaints when something changed.

## Building

The included `CMakeLists.txt` handles both platforms — on Linux it also generates the `xdg-shell`/`xdg-decoration` protocol bindings via `wayland-scanner` before compiling. Just:

```bash
cmake -B build
cmake --build build
```

## Known limitations (v1)

- No image decoding (PNG/JPEG) — `CanvasImage` expects already-decoded RGBA8 pixels.
- `getImageData` is unsupported on Windows (returns `std::nullopt`).
- `Canvas::clearRect` clears the whole surface on Windows, only the given rect on Linux.
- `strokeText` approximates a glyph outline; it isn't a true contour stroke.
- Percentage sizes resolve to `0` against an indefinite (`Fit`) ancestor.