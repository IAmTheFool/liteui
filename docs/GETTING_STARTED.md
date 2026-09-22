# Getting started

This walks through the concepts you need to build a real window with
liteui, in the order you'll actually use them. For linking/build setup,
see the main [README](../README.md). For a field-by-field reference
once you know your way around, see [`API.md`](API.md).

## The shape of a liteui program

Every program follows the same three steps:

```cpp
#include "liteui.hpp"

int main() {
  View root;
  // ... build the tree ...

  LiteUI ui("Window Title", 800, 600);
  ui.setRoot(root);
  ui.run();   // blocks, pumps the platform event loop
}
```

1. Build a `View` tree describing what's on screen.
2. Hand it to a `LiteUI` window with `setRoot`.
3. Call `run()`, which blocks until the window closes.

There's no separate "render loop" to write. Layout, painting, input
dispatch, and re-rendering after state changes are all handled
internally — you just describe the tree and react to events.

## Building a tree with `View`

A `View` is a rectangle with layout rules (`style`) and, optionally,
children:

```cpp
View box;
box.style.width = Size::pixel(200);
box.style.height = Size::pixel(100);
box.style.backgroundColor = Color{240, 240, 240};
box.style.borderRadius = 8.0f;
```

`addChild` appends a child:

```cpp
View row;
row.style.direction = FlexDirection::Row;
row.style.gap = 12;

View left, right;
row.addChild(left);
row.addChild(right);
```

Layout is a flexbox subset — if you know CSS flexbox, `Style`'s fields
map directly: `direction` (row/column), `justifyContent`, `alignItems`,
`flexWrap`, `alignContent`, `gap`, `flexGrow`/`flexShrink`, `padding`,
`margin`. `Size` has four kinds:

```cpp
Size::pixel(140)      // fixed
Size::percentage(50)  // % of parent
Size::fit()           // shrink to content — the default
Size::full()          // fill available space
```

## Text

`Text` is a leaf node — `addChild(Text)` flattens it into the tree, it
never appears as a separate `View` in your code afterward:

```cpp
Text label;
label.label = "Hello";
label.fontSize = 18;
label.color = Color{20, 20, 20};
root.addChild(label);
```

## Making things dynamic: `Dynamic<T>`

Almost every meaningful field — `label`, `backgroundColor`, `width`,
`disabled`, and more — accepts either a plain value or a
`std::function<T()>`. Assign a lambda and it's re-polled automatically
after every dispatched event (click, key, timer tick):

```cpp
int count = 0;

Text label;
label.label = [&]() { return "Count: " + std::to_string(count); };

View button;
button.onClick = [&]() { count++; };
```

You never call anything to "trigger a re-render" — mutating `count` in
the click handler is enough, because `label.label`'s callback gets
polled right after the click is dispatched. This is the entire state
model: plain variables captured by reference, read back through
`Dynamic<T>` callbacks.

## Handling input

- `onClick` / `onMiddleClick` / `onRightClick` — fires on a full
  press-and-release over the same view. Bubbles to the nearest
  ancestor with a handler if the exact view clicked has none.
- `onPressAt(float localX, float localY)` — fires the instant a press
  lands, giving coordinates local to that view. Use this for "where
  inside me was clicked" logic (sliders, custom controls).
- `onDragTo(float localX, float localY)` — fires continuously while a
  press that started on this view is held and moving.
- `onScrollUp` / `onScrollDown` — fires per wheel notch, independent of
  any pixel-based content scrolling.
- `focusable = true` plus `onKeyDown` / `onTextInput` — for keyboard
  input. Only one view has focus at a time; click-to-focus is
  automatic for any `focusable` view.

```cpp
View slider;
slider.style.width = Size::pixel(200);
slider.style.height = Size::pixel(20);
slider.onPressAt = [&](float x, float) {
  value = std::clamp(x / 200.0f, 0.0f, 1.0f);
};
slider.onDragTo = [&](float x, float) {
  value = std::clamp(x / 200.0f, 0.0f, 1.0f);
};
```

## Scrolling containers

Set `overflowX`/`overflowY` on a `Style` to make that view a scroll
container — its children are then measured at their natural size and
clipped/scrolled instead of being squeezed to fit:

```cpp
View list;
list.style.height = Size::pixel(300);
list.style.overflowY = Overflow::Auto;   // scrollbar appears only if content overflows
```

`Overflow::Scroll` always shows the scrollbar; `Overflow::Auto` shows
it only when content actually exceeds the viewport; `Overflow::Hidden`
clips without a scrollbar; `Overflow::Visible` (the default) never
clips at all.

## Keyed dynamic lists

For a list whose items change over time (add/remove rows) where you
don't want to rebuild the whole subtree by hand, use `keysSource` +
`itemBuilder` instead of populating `children` directly:

```cpp
std::vector<std::string> todos = {"a", "b", "c"};

View list;
list.style.direction = FlexDirection::Column;
list.keysSource = [&]() { return todos; };   // called after every dispatched event
list.itemBuilder = [&](const std::string& key) {
  Text row;
  row.label = key;
  View wrapper;
  wrapper.addChild(row);
  return wrapper;
};
```

When `todos` changes, only the rows whose keys actually changed are
rebuilt — surviving rows (matched by key) are moved over untouched,
including their own scroll position, cached text layout, and canvas
surface if they have one. This is the mechanism to reach for before
hand-rolling your own diffing.

## Canvas — custom drawing

`Canvas` gives you an HTML5-canvas-style 2D context for anything the
built-in nodes don't cover directly:

```cpp
Canvas c;
c.style.width = Size::pixel(300);
c.style.height = Size::pixel(200);
c.onPaint = [](CanvasContext& ctx) {
  ctx.setFillColor(Color{255, 100, 100});
  ctx.fillRect(10, 10, 100, 80);

  ctx.beginPath();
  ctx.arc(200, 100, 40, 0, 6.283f);
  ctx.setFillColor(Color{100, 100, 255});
  ctx.fill();
};
root.addChild(c);
```

`onPaint` is not a per-frame callback — it only re-runs when the
canvas is actually dirty (first paint, a resize, or when you call
`requestCanvasRedraw()` / set `canvasDirtySource`). Treat it as "redraw
from whatever state I closed over," not "draw one frame of an
animation loop." See [`API.md`](API.md) and
[`PLATFORM_NOTES.md`](PLATFORM_NOTES.md) for the full `CanvasContext`
method list and its per-backend limitations.

## Images and SVG

```cpp
Image img;
img.path = "logo.png";
img.fit = ObjectFit::Contain;
root.addChild(img);

Svg icon;
icon.path = "icon.svg";
root.addChild(icon);
```

Both decode/parse once, synchronously, at `addChild` time (not lazily,
not on a background thread). `Image` supports PNG/JPEG. `Svg` stays
vector and re-renders cleanly at any size, including across a window
resize — see `PLATFORM_NOTES.md` for what subset of SVG is supported.

## Text input

```cpp
TextInput field;
field.placeholder = "Type here...";
field.onChange = [](const std::string& text) { /* ... */ };
field.onSubmit = [](const std::string& text) { /* fires on Enter */ };
root.addChild(field);
```

Caret, click-to-position, arrow-key navigation, and basic editing are
all wired up for you. Note the ASCII-only limitation documented in
`PLATFORM_NOTES.md` before using it for anything beyond plain Latin
text.

## Where to go from here

- [`API.md`](API.md) — full field/method reference once you're past
  the basics.
- [`PLATFORM_NOTES.md`](PLATFORM_NOTES.md) — what's different or
  unsupported between Windows and Linux.
- [`examples/`](../examples/) — one small, focused program per
  feature (menus, dialogs, split panes, a paint canvas, a file
  explorer, and more).
