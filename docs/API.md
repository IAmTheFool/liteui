# API reference

Everything LiteUI exposes lives in `liteui.hpp`, in the **global namespace**, plus three public helper namespaces: `liteui_clipboard`, `liteui_image` and `liteui_text`. Other namespaces you may see in the header (`liteui_layout`, `liteui_svg`, `liteui_xml`, `liteui_pointer`, `liteui_filepicker`, `liteui_canvas_gl`, `liteui_prof`) are implementation details and not part of the API. This page documents version **0.5** (`LITEUI_VERSION_MAJOR` / `LITEUI_VERSION_MINOR`) and is the reference for the public types and functions. For concepts and behavior, see the guides: [LAYOUT.md](LAYOUT.md), [CANVAS.md](CANVAS.md) and [BUILDING.md](BUILDING.md).

- [Conventions](#conventions)
- [Value types](#value-types)
- [Enumerations](#enumerations)
- [`Dynamic<T>`](#dynamict)
- [Input types](#input-types)
- [Pointer events](#pointer-events): `PointerEvent`, `WheelEvent`, `PointerCallback`, `Cursor`
- [`Style`](#style)
- [Leaf nodes](#leaf-nodes): `Text`, `TextInput`, `Image`, `Svg`, `Canvas`
- [`View`](#view)
- [`LiteUI` (the window)](#liteui-the-window)
- [Dialogs, clipboard and text helpers](#dialogs-clipboard-and-text-helpers)
- [Pixel formats and `ImageBuffer`](#pixel-formats-and-imagebuffer)
- [`liteui_image`: decoding, encoding and display conversion](#liteui_image-decoding-encoding-and-display-conversion)
- [Compile-time switches and CMake](#compile-time-switches-and-cmake)
- [Errors, threading and lifetime](#errors-threading-and-lifetime)

## Conventions

- **One implementation per program.** Include `liteui.hpp` anywhere; define `LITEUI_IMPLEMENTATION` before including it in **exactly one** `.cpp` of each executable (see [BUILDING.md](BUILDING.md#rule-1-one-implementation-per-program)).
- **C++23.** The CMake target sets `cxx_std_23`.
- **Version macros.** `LITEUI_VERSION_MAJOR` (0) and `LITEUI_VERSION_MINOR` (5) are defined by the header, so you can `#if` on them.
- **Trees are copied.** `View::addChild` and `LiteUI::setRoot` take their argument **by value**. Build children completely first; later changes to your local `View` do not affect the copy that was added. Keep any state your callbacks capture alive while the window runs.
- **UI thread only**, except `LiteUI::wake()`, which is safe from any thread.
- **Units** are pixels, angles are radians, and colors are 8-bit per channel.

---

## Value types

### `Color`

```cpp
struct Color { uint8_t r = 0, g = 0, b = 0, a = 255; };   // has operator==
```

Straight (non-premultiplied) RGBA. `Color{255, 128, 0}` is opaque orange; `Color{0, 0, 0, 0}` is fully transparent.

### `EdgeInsets`

```cpp
struct EdgeInsets {
    float top = 0, right = 0, bottom = 0, left = 0;       // CSS order
    static EdgeInsets all(float v);
};                                                         // has operator==
```

Used for `margin` and `padding`. Aggregate initialization follows the field order: `EdgeInsets{8, 12, 8, 12}` is top 8, right 12, bottom 8, left 12.

### `Size`

```cpp
struct Size {
    enum class Kind { Fixed, Percentage, Fit, Full };
    Kind kind = Kind::Fit;
    float value = 0;

    static Size pixel(float v);        // Fixed, in pixels (border box)
    static Size percentage(float v);   // 0-100, of the space offered by the parent
    static Size fit();                 // size to content (the default)
    static Size full();                // all the space the parent offers on that axis
};
```

How each kind resolves is described in [LAYOUT.md](LAYOUT.md#sizing).

### `CanvasImage`

```cpp
struct CanvasImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;      // width * height * 4 bytes, row-major, RGBA8
    CanvasImage() = default;
    CanvasImage(int w, int h);        // allocates zero-filled (transparent) pixels
};
```

The 8-bit bitmap type used by `Canvas::drawImage`, `Image::source` and the 8-bit decoders. Straight (non-premultiplied) alpha, tightly packed. For higher-precision pixels see [`ImageBuffer`](#pixel-formats-and-imagebuffer).

### `kScrollbarThickness`

`constexpr float kScrollbarThickness = 12.0f;` The width of a vertical scrollbar and the height of a horizontal one. Layout and painting share this constant.

---

## Enumerations

| Enum | Values |
|---|---|
| `FlexDirection` | `Row`, `Column` |
| `Justify` | `Start`, `End`, `Center`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly` |
| `Align` | `Start`, `End`, `Center`, `Stretch` |
| `FlexWrap` | `NoWrap`, `Wrap` |
| `AlignContent` | `Start`, `End`, `Center`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly`, `Stretch` |
| `Position` | `Static`, `Absolute` |
| `Display` | `Flex`, `None` |
| `Visibility` | `Visible`, `Hidden` |
| `Overflow` | `Visible`, `Hidden`, `Scroll`, `Auto` |
| `FontWeight` (`int`) | `Thin` 100, `ExtraLight` 200, `Light` 300, `Regular` 400, `Medium` 500, `SemiBold` 600, `Bold` 700, `ExtraBold` 800, `Black` 900 |
| `FontStyle` | `Normal`, `Italic` |
| `TextAlign` | `Start`, `Center`, `End`, `Justify` |
| `TextOverflow` | `Clip`, `Ellipsis` |
| `TextWrap` | `Wrap`, `NoWrap` |
| `ObjectFit` | `Fill`, `Contain`, `Cover`, `None`, `ScaleDown` |
| `MouseButton` | `Left`, `Middle`, `Right`, `None` (what `PointerEvent::button` holds when no button changed) |
| `PointerType` (`uint8_t`) | `Mouse`, `Pen`, `Touch` |
| `Cursor` (`uint8_t`) | See [`Cursor`](#cursor) |
| `PixelFormat` (`uint8_t`) | `RGBA8_UNORM`, `RGBA16_UNORM`, `RGBA16_FLOAT`, `RGBA32_FLOAT` |
| `ClampMode` (`uint8_t`) | `PerChannel`, `PreserveHue` |
| `ImageFileFormat` (`uint8_t`) | `Unknown`, `Png`, `Jpeg` |
| `JpegAlpha` (`uint8_t`) | `FlattenOverWhite`, `Discard` |
| `JpegChroma` (`uint8_t`) | `Auto`, `S420`, `S444` |

Canvas-specific enums (`LineCap`, `LineJoin`, `TextBaseline`, `FillRule`, `CompositeOp`) are listed in [CANVAS.md](CANVAS.md).

---

## `Dynamic<T>`

```cpp
template <class T> using Dynamic = std::variant<T, std::function<T()>>;
```

A field that holds either a value or a callback that returns one. Assign either form:

```cpp
label.label = std::string("fixed");
label.label = [&] { return "Count: " + std::to_string(count); };
```

LiteUI polls callbacks after every dispatched event and once in `setRoot`; when a polled value changes it relays out and repaints. Callbacks should be cheap and free of side effects.

| Helper | Purpose |
|---|---|
| `resolveDynamic(field, cache)` | Library-internal read helper (value if static, else the last polled value). |
| `resolveStatic(field)` | Returns the stored value, and throws `std::bad_variant_access` if the field holds a callback. |

> A `Dynamic<float>` assigned from an `int` does not compile (narrowing in `std::variant` conversion). Use `8.0f`, not `8`.

---

## Input types

### `Key`

`Unknown`, `A`–`Z`, `N0`–`N9`, `F1`–`F12`, `Enter`, `Escape`, `Backspace`, `Tab`, `Space`, `Delete`, `Left`, `Right`, `Up`, `Down`, `Home`, `End`, `PageUp`, `PageDown`, `Shift`, `Control`, `Alt`, `Super`, `Minus`, `Equal`, `LeftBracket`, `RightBracket`, `Backslash`, `Semicolon`, `Quote`, `Comma`, `Period`, `Slash`, `Grave`.

Keys are logical and independent of platform scan codes. There are no keypad, Insert, or lock keys.

### `KeyModifiers`

```cpp
struct KeyModifiers { bool shift = false, ctrl = false, alt = false, super = false; };   // operator==
```

### `KeyEvent`

```cpp
struct KeyEvent { Key key = Key::Unknown; KeyModifiers mods; };
```

Delivered to `onKeyDown` / `onKeyUp`. Key repeat produces repeated `onKeyDown` events. For typed characters use `onTextInput`, which delivers an already-composed UTF-32 code point and is the right input for text fields.

---

## Pointer events

Mouse, pen and touch all produce the same events, modelled on the DOM pointer-events API. Handlers live on `View` and `Canvas` (see [Event handlers](#event-handlers)); this section describes the event objects and the dispatch rules.

### Dispatch model

- **Targeting and bubbling.** Down, move, up, click and wheel events go to the **deepest view under the pointer that has a handler for that event**, then bubble to ancestors (again, only those with a handler) until a handler calls `e.stopPropagation()`. A `Position::Absolute` view bubbles to its parent in the tree, not to whatever is painted beneath it.
- **Transparent views.** A view without a handler for an event is transparent to it. So is a **disabled** view (the exception is `onWheel`, which fires whether or not the view is disabled).
- **Capture.** A press captures the pointer. The capture target is the view that called `stopPropagation()` in its `onPointerDown`; if none did, it is the deepest view whose `onPointerDown` ran. While any button is held, moves and the final up go to the capture target (and bubble from it), **even when the pointer is outside it**. Capture ends when the last button is released or when `onPointerCancel` fires.
- **Cancel.** `onPointerCancel` fires when the press can no longer complete: the OS took the pointer (focus loss, a compositor gesture, a system dialog, a window-move grab), or a scroll pan swallowed the release (see below). It is **not** fired for a view that is being destroyed (a keyed-list rebuild or `setRoot()`).
- **Enter / leave** do not bubble. They follow the same hover flag that drives `Style::hoverColor`, so they are purely geometric and are not suppressed while another view holds capture.
- **Clicks.** `onClick` fires for the **left** button only, and only if the release lands on the same view the press did. Middle fires `onAuxClick` and right fires `onContextMenu` (on release). `onDoubleClick` fires after a second left click within **500 ms and 4 px**; `PointerEvent::clickCount` holds the running count.
- **Pen and touch.** A pen tip touching down, or a finger, counts as `MouseButton::Left`, so clicks and double-clicks work with no extra code. The pen's barrel button is `Right`, its second barrel button is `Middle`, and the eraser end sets `PointerEvent::eraser`. Every pen tool and every finger gets its own `pointerId`, capture and click pairing, so several touches can be in flight at once. A pen reports hover (moves with `buttons == 0`) before and after contact and drives enter/leave and hover colors like a mouse; a finger has no hover and only produces events while down. Scroll-container panning (`Style::contentPanEnabled`) and scrollbar grabs work for fingers and pens as for the mouse, one pointer at a time.
- **Scroll panning.** Pressing on content inside a scroll container with `contentPanEnabled` arms a pan, and the ordinary press still proceeds, so your `onPointerDown` runs first. Once the pointer has moved more than 4 px the content starts to pan and the pending **click is dropped**. The press target keeps its capture, so it **keeps receiving `onPointerMove` while the content pans**; when the button is released, the release is consumed by the pan, so the target gets `onPointerCancel` instead of `onPointerUp`. If a view should own the drag, set `contentPanEnabled = false` on the scroll container.
- **Coordinates.** `x`/`y` are rewritten for each view the event reaches (local to the view whose handler is running, relative to its border box); `windowX`/`windowY` are not.
- **Platform support.** Windows: mouse and touchpad use the classic mouse messages; pen and touch use `WM_POINTER*` (Windows 8+). Linux/Wayland: `wl_pointer` for the mouse, `wl_touch` for fingers and `zwp_tablet_v2` for pens (which is why the build vendors the tablet protocol, see [Compile-time switches and CMake](#compile-time-switches-and-cmake)).

### `PointerType`

`enum class PointerType : uint8_t { Mouse, Pen, Touch };`

### `PointerSample`

```cpp
struct PointerSample {
    float x = 0, y = 0;                 // WINDOW coordinates
    float pressure = 0;
    float tiltX = 0, tiltY = 0, twist = 0;
    uint64_t timeMs = 0;
};
```

### `PointerEvent`

Passed by non-const reference to every pointer handler.

| Field / method | Meaning |
|---|---|
| `float x, y` | Position **local** to the view whose handler is running. |
| `float windowX, windowY` | Position in window coordinates. |
| `uint32_t pointerId` | The mouse is always `1`; each pen tool and finger contact gets its own id while it exists. |
| `bool isPrimary` | The primary pointer of its type: the mouse; the first finger down (until it lifts); a pen. |
| `MouseButton button` | The button that changed on a down/up event; `MouseButton::None` on moves, wheel and enter/leave. |
| `uint8_t buttons` | Bitmask of buttons held **after** this event's change. Bits follow the enum order: `Left` = 1, `Middle` = 2, `Right` = 4 (note: not the DOM's order). |
| `KeyModifiers mods` | Modifier keys at the time of the event. |
| `PointerType type` | `Mouse`, `Pen` or `Touch`. |
| `float pressure` | 0 to 1. For a mouse, 0.5 while a button is held and 0 otherwise. |
| `float tiltX, tiltY, twist` | Pen orientation (0 when unavailable). |
| `bool eraser` | `true` when the pen's eraser end is in use. |
| `uint8_t clickCount` | Running click count; set on `onClick` / `onDoubleClick` only, `0` elsewhere. |
| `uint64_t timeMs` | Monotonic milliseconds (arbitrary epoch). |
| `std::span<const PointerSample> coalesced` | **`onPointerMove` only.** Every position the pointer reported since the previous move event, oldest first, ending with this event's own position. Handlers run once per frame, so a fast pen stroke would otherwise lose samples; iterate this to draw smooth strokes. Never empty on a move, empty on other events. Valid only while the handler runs, so copy what you need. Window coordinates. |
| `PointerSample toLocal(PointerSample s) const` | Converts a coalesced sample to the local coordinates of the view whose handler is running. |
| `void stopPropagation()` / `bool propagationStopped() const` | Stop the event from reaching ancestors. In `onPointerDown` this also makes this view the capture target. |
| `bool held(MouseButton b) const` | `true` if `b` is currently held. |
| `PointerEvent fresh() const` | A copy that has not been stopped, for firing a second event from the same data. |

`constexpr uint8_t buttonBit(MouseButton b)` returns the `buttons` bit for a button (`0` for `None`).

```cpp
Canvas c;
c.onPointerDown = [&](PointerEvent &e) {
    if (e.button != MouseButton::Left) return;
    stroke.begin(e.x, e.y, e.pressure);
};
c.onPointerMove = [&](PointerEvent &e) {
    if (!e.held(MouseButton::Left)) return;          // hover moves have buttons == 0
    for (const PointerSample &s : e.coalesced) {
        PointerSample p = e.toLocal(s);              // window -> this canvas
        stroke.add(p.x, p.y, p.pressure);
    }
    dirty = true;                                    // picked up by canvasDirtySource
};
c.onPointerUp = [&](PointerEvent &) { stroke.end(); };
c.onPointerCancel = [&](PointerEvent &) { stroke.discard(); };
```

### `WheelEvent`

```cpp
struct WheelEvent {
    float x, y;                // local to the view whose handler is running
    float windowX, windowY;
    float dx, dy;              // pixels; +dy = content scrolls down, +dx = right
    bool precise;              // pixel deltas (trackpad-style) vs a notched wheel
    KeyModifiers mods;
    uint64_t timeMs;
    void stopPropagation();    // stops bubbling AND suppresses the built-in scrolling
    bool propagationStopped() const;
};
```

If no `onWheel` handler calls `stopPropagation()`, the wheel falls through to the normal overflow scrolling. Call it when the view should consume the wheel itself (for example, zooming a canvas). On Windows a wheel notch is 40 px and `precise` is `true` when the notch count is fractional; the Wayland backend cannot tell a trackpad from a wheel, so `precise` stays `false` there.

### Handler types

```cpp
using PointerHandler = std::function<void(PointerEvent &)>;
using WheelHandler   = std::function<void(WheelEvent &)>;
```

### `PointerCallback`

The type of `onClick` (on both `View` and `Canvas`). It accepts either form:

```cpp
view.onClick = [&](PointerEvent &e) { /* bubbles to ancestors unless e.stopPropagation() */ };
view.onClick = [&] { /* short form: runs, then ENDS bubbling */ };
```

A no-argument handler cannot call `stopPropagation()`, so it is taken to have handled the click and bubbling ends. Use the one-argument form if an ancestor should also see the click. Other members: `explicit operator bool()`, `operator()(PointerEvent &)`, `operator()()` (calls with a default event) and `endsBubbling()`.

### `Cursor`

The mouse cursor shown over a view; see `Style::cursor`. Names follow CSS's `cursor` keywords.

```cpp
enum class Cursor : uint8_t {
    Inherit,      // use the nearest ancestor's cursor (the default); the root falls back to Default
    Default,      // the ordinary arrow
    None,         // hide the cursor
    Pointer,      // hand, for clickable things
    Text,         // I-beam
    Crosshair,
    Move,         // four-way arrows
    Grab, Grabbing,
    NotAllowed,
    Wait,         // busy, blocks the UI
    Progress,     // busy, still interactive
    Help,
    ResizeEW, ResizeNS, ResizeNESW, ResizeNWSE,
    ZoomIn, ZoomOut,
};
```

Because the cursor is inherited, setting it once on a container covers everything inside. While a view holds the **mouse's** pointer capture, the **capture target's** cursor is shown wherever the pointer goes, so a drag keeps its "grabbing" or resize cursor outside the view. Only the mouse drives the cursor image: a finger's or a pen's capture does not pin it. A window-edge resize handle still wins over `Style::cursor`.

---

## `Style`

Layout, box painting and scrolling behavior. Every `View` and every leaf node has one. Fields marked **D** are `Dynamic<>` (value or callback); the rest are plain fields that are read when layout runs.

| Field | Type | Default | |
|---|---|---|---|
| `width`, `height` | `Dynamic<Size>` | `Size::fit()` | **D** |
| `minWidth`, `minHeight` | `float` | `0` | |
| `maxWidth`, `maxHeight` | `float` | `+infinity` | |
| `margin`, `padding` | `Dynamic<EdgeInsets>` | all `0` | **D** |
| `direction` | `FlexDirection` | `Row` | |
| `justifyContent` | `Justify` | `Start` | |
| `alignItems` | `Align` | `Stretch` | |
| `gap` | `float` | `0` | |
| `flexWrap` | `FlexWrap` | `NoWrap` | |
| `alignContent` | `AlignContent` | `Stretch` | |
| `flexGrow` | `float` | `0` | |
| `flexShrink` | `float` | `1` | |
| `backgroundColor` | `Dynamic<Color>` | transparent | **D** |
| `borderWidth` | `Dynamic<float>` | `0` | **D** |
| `borderColor` | `Dynamic<Color>` | black | **D** |
| `borderRadius` | `Dynamic<float>` | `0` | **D** |
| `hoverColor` | `std::optional<Color>` | unset | Replaces `backgroundColor` while the pointer is over the view. |
| `cursor` | `Dynamic<Cursor>` | `Cursor::Inherit` | **D**. Cursor shown over the view; inherited from the parent when `Inherit`. See [`Cursor`](#cursor). |
| `position` | `Position` | `Static` | |
| `left`, `top`, `right`, `bottom` | `Dynamic<float>` | `NaN` (unset) | **D** |
| `zIndex` | `Dynamic<int>` | `0` | **D**. Absolute views only. |
| `overflowX`, `overflowY` | `Overflow` | `Visible` | |
| `contentPanEnabled` | `bool` | `true` | Drag inside a scroll container to pan (mouse, pen or finger). Starts after about 4 px of movement. |
| `wheelScrollEnabled` | `bool` | `true` | Wheel moves a scroll container. `onWheel` handlers fire regardless. |
| `scrollTrackColor`, `scrollThumbColor` | `Dynamic<std::optional<Color>>` | unset | **D**. Overrides the window default. |
| `scrollbarAutoHide` | `bool` | `false` | Show bars only while hovered or dragging. |
| `display` | `Dynamic<Display>` | `Flex` | **D** |
| `visibility` | `Dynamic<Visibility>` | `Visible` | **D** |
| `escapeClip` | `bool` | `false` | Absolute views: ignore ancestors' scroll clips. |

Because `gap`, `direction`, `flexGrow`, `flexShrink`, the min/max sizes and the other plain fields are not polled, changing them after the tree is built has no effect on screen. See [LAYOUT.md](LAYOUT.md) for the full semantics of each.

---

## Leaf nodes

`Text`, `TextInput`, `Image`, `Svg` and `Canvas` are **author-facing descriptions**. `View::addChild` flattens each into a plain `View`, so the retained tree only ever contains `View`s.

### `Text`

```cpp
struct Text {
    Dynamic<std::string> label;
    Style style;                        // layout only
    float fontSize = 16;
    FontWeight fontWeight = FontWeight::Regular;
    FontStyle fontStyle = FontStyle::Normal;
    std::string fontFamily;             // empty = platform default UI font
    Dynamic<Color> color = Color{0, 0, 0};
    TextAlign align = TextAlign::Start;
    TextOverflow overflow = TextOverflow::Clip;
    TextWrap wrap = TextWrap::Wrap;
    float lineHeight = 0;               // 0 = from font metrics
    float letterSpacing = 0;
    int maxLines = 0;                   // 0 = unlimited
    bool underline = false, strikethrough = false;
};
```

- Only `label` and `color` can be callbacks. `fontSize`, `fontWeight` and `fontStyle` are plain values.
- `style` affects layout only. **A text node never paints its own background or border.** Wrap it in a styled `View` for that.
- `Text` has no event handlers. To make text clickable, put a handler on a parent `View`; clicks on the text bubble up to it.
- Give paragraphs a definite width so they wrap with a correct height (see [LAYOUT.md](LAYOUT.md#text)).

### `TextInput`

A single-line text field with caret, placeholder and basic editing wired up.

```cpp
struct TextInput {
    Style style;
    std::string text;                   // initial text, read once at addChild
    std::string placeholder;
    float fontSize = 16;
    FontWeight fontWeight = FontWeight::Regular;
    std::string fontFamily;
    Color textColor{0,0,0};
    Color placeholderColor{160,160,160};
    Color caretColor{20,20,20};
    Color borderColor{180,180,180};
    Color focusedBorderColor{50,120,220};
    float leftPadding = 6;
    std::function<void(const std::string &)> onChange;   // after each edit or paste
    std::function<void(const std::string &)> onSubmit;   // Enter
    std::shared_ptr<TextInputState> state;               // live state, shared with the node
};
```

- **Give it a size.** A `TextInput` is canvas-backed and has no intrinsic size; set `style.width` and `style.height`.
- **Border:** the border color switches between `borderColor` and `focusedBorderColor` automatically (it overrides `style.borderColor`). Set `style.borderWidth` to make it visible.
- **Reading the text:** keep a copy of the `state` pointer before `addChild`, and read `state->text` any time. `text` is only the initial value.
- **Focusing from code:** set `state->requestFocus = true`; it takes focus at the next poll.

```cpp
TextInput name;
name.style.width = Size::pixel(220);
name.style.height = Size::pixel(32);
name.style.borderWidth = 1.0f;
name.placeholder = "Your name";
auto nameState = name.state;                  // shared with the node after addChild
name.onSubmit = [&](const std::string &s) { /* ... */ };
root.addChild(name);
// later: nameState->text
```

`TextInputState` fields: `text`, `cursor` (byte offset), `focused`, `blinkOn`, `dirty`, `scrollOffset`, `requestFocus` (plus an internal `blinkTimerHandle`).

**Supported editing:** typing, Left/Right, Home/End, Backspace/Delete, Enter (`onSubmit`), click to place the caret (left button; the field consumes the press, so it does not also click a parent), `Ctrl+V` paste (line breaks and non-ASCII bytes are stripped). **Not supported:** selection, copy/cut, select-all, undo, multi-line, and non-ASCII input (typed characters outside printable ASCII are ignored, and the caret moves by bytes). For richer fields, build your own on a `Canvas` using `liteui_text::caretIndexForX`.

### `Image`

```cpp
struct Image {
    Style style;
    std::string path;                              // decoded when added
    std::vector<uint8_t> memoryData;               // encoded bytes, decoded when added
    Dynamic<std::shared_ptr<CanvasImage>> source;  // pixels you supply (fixed or callback)
    ObjectFit fit = ObjectFit::Fill;
    Color backgroundColor{0,0,0,0};                // shown in letterboxed margins
    std::function<void(int w, int h)> onLoad;
    std::function<void(const std::string &err)> onError;
    Dynamic<std::shared_ptr<ImageBuffer>> hdrSource;   // high-precision pixels (any PixelFormat)
    Dynamic<DisplaySettings> displaySettings;          // exposure / clamp for hdrSource
};
```

- **Source priority:** `hdrSource` (fixed or callback), then `source` (fixed or callback), then `path`, then `memoryData`.
- **High-precision images.** `hdrSource` takes an [`ImageBuffer`](#pixel-formats-and-imagebuffer), typically `RGBA32_FLOAT` linear-light data from a RAW or HDR pipeline. It is converted once per new pointer into an 8-bit display copy by `liteui_image::toCanvasImage`: float formats are treated as linear light, exposed, clamped and sRGB-encoded (no tone mapping); `RGBA8_UNORM` buffers are shown unchanged. Like `source`, change detection is by **pointer**: mutating a buffer in place is not noticed, so hand over a new `shared_ptr`.
- **Exposure without a new buffer.** `displaySettings` may be a fixed value or a callback polled every dispatch cycle. When the returned settings change, the display copy is rebuilt and the node repaints, so an exposure slider needs only `img.displaySettings = [&] { return DisplaySettings{.exposureEv = ev}; };`. It is ignored for `RGBA8_UNORM` buffers (they are already display-referred) and for the other sources.
- `ImageState` (the shared cache behind an `Image`) is declared in the header but is an implementation detail.
- **Decoding is synchronous**, inside `addChild`. `onLoad` / `onError` fire there, once.
- **Natural size:** if `width`/`height` is `Fit`, the node takes the image's pixel size. With a callback `source` or `hdrSource` it follows whichever image is current.
- **Dynamic source:** return a **different** `shared_ptr` when the picture changes. Comparison is by pointer, not by pixel content.
- Linux decodes PNG and JPEG only; Windows uses WIC (more formats).

### `Svg`

```cpp
struct Svg {
    Style style;
    std::string path;                     // file path, or...
    std::string source;                   // ...inline SVG text (used if path is empty)
    ObjectFit fit = ObjectFit::Contain;
    Color backgroundColor{0,0,0,0};
    std::function<void()> onLoad;
    std::function<void(const std::string &err)> onError;
};
```

Parsed when added (`onLoad` / `onError` fire there). A `Fit` size becomes the document's width/height. It re-renders at the node's pixel size, including after resizes, so it stays crisp. Supported: `path`, `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, groups, `<defs>`/`<use>`, transforms, presentation attributes, inline `style=""`, and `<style>` blocks with simple `.class` rules. Not supported: gradients, patterns, `clipPath`/`mask`, and text. Group opacity is approximated per shape.

Behavior details confirmed by the SVG test suite:

- **Size:** the document size comes from `viewBox` if present and valid, otherwise from `width`/`height`, otherwise **100 × 100**. A malformed `viewBox` is ignored. The `viewBox` origin shifts every shape.
- **Precedence:** a presentation attribute loses to a `<style>` class rule, which loses to an inline `style=""`. `<style>` blocks (including inside CDATA, as Illustrator and Figma export them) support simple `.class` selectors only; tag and id selectors are ignored.
- **Paints:** `none`, `#rgb`/`#rrggbb`, named colors, `rgb()` and `rgba()` are understood. Anything else, notably `url(#gradient)`, is **ignored and the inherited paint is kept**, so a gradient-filled shape falls back to its parent's fill (black by default), not to `none`.
- **Shapes:** a shape with zero size (`width="0"`, `r="0"`, empty `points`, empty `d`) is dropped. Unknown elements such as `<text>` are skipped, but known shapes nested inside an unknown container are still drawn.
- **`<use>`:** resolves `href` and `xlink:href`, supports `x`/`y` and `transform`, ignores missing targets, picks the first element when ids are duplicated, renders a `<symbol>` only through `<use>`, cuts reference cycles, and stops following chains beyond 32 levels.
- **Errors:** `onError` receives a message such as `failed to open <path>` or `failed to parse <path>`. Input that is not an `<svg>` document (including an empty file) is a parse failure.

### `Canvas`

Custom drawing surface. Fields: `style`, `onPaint(CanvasContext &)`, the [pointer](#pointer-events) and wheel handlers (same types and semantics as on `View`), `canvasDirtySource`, and the focus and key handlers. The full model, the drawing API, and platform differences are in **[CANVAS.md](CANVAS.md)**.

```cpp
struct Canvas {
    Style style;
    std::function<void(CanvasContext &)> onPaint;
    PointerCallback onClick;
    PointerHandler  onDoubleClick, onAuxClick, onContextMenu;
    PointerHandler  onPointerDown, onPointerMove, onPointerUp, onPointerCancel;
    PointerHandler  onPointerEnter, onPointerLeave;
    WheelHandler    onWheel;
    std::function<bool()> canvasDirtySource;
    bool focusable = false;
    std::function<void()> onFocus, onBlur;
    std::function<void(KeyEvent)> onKeyDown;
    std::function<void(uint32_t)> onTextInput;
};
```

`CanvasImage`, `CanvasGradient`, and the `CanvasContext` methods are documented in [CANVAS.md](CANVAS.md).

---

## `View`

A node in the retained tree. You normally create `View`s for containers and let `addChild` create the rest.

### Building the tree

```cpp
void addChild(View child);
void addChild(Text t);
void addChild(Canvas c);
void addChild(TextInput ti);
void addChild(Image img);
void addChild(Svg s);
```

Each appends a child (flattening leaf descriptions into `View`s). The argument is taken **by value**.

### Members

| Member | Type | Notes |
|---|---|---|
| `style` | `Style` | |
| `children` | `std::vector<View>` | Filled by `addChild`. Fill either `children` or use a keyed list. |
| `key` | `std::string` | Set automatically for keyed-list children. Empty means "not keyed". |
| `keysSource` | `std::function<std::vector<std::string>()>` | Keyed list: the keys this view should hold. Polled after every event. |
| `itemBuilder` | `std::function<View(const std::string &key)>` | Builds the child for a **new** key only. Surviving children are reused as they are. |
| `disabled` | `Dynamic<bool>` | A disabled view is transparent to pointer events (its press/click handlers do not run; `onWheel` still does) and shows no tooltip. |
| `focusable` | `bool` | Click gives it keyboard focus. Default `false`. |
| `wantsFocus` | `std::function<bool()>` | Polled; when it returns `true` this view takes focus. |
| `tooltip` | `std::string` | Shown after hovering for `TooltipStyle::delayMs`. Plain field, read when the tooltip appears. |
| `onLayout` | `std::function<void(float x, float y, float w, float h)>` | After each layout, with the final window-space box. |
| `computed` | `View::Computed` | Layout results and caches. **Read-only for apps.** Contains `x`, `y`, `w`, `h` (border box in window coordinates), `contentW`, `contentH`, `scrollX`, `scrollY`, and `isHovered`. |

`isText`, `text`, `textStyle`, `isCanvas` and `onPaint` are set by `addChild` when it flattens a leaf; do not set them by hand.

### Event handlers

All are `std::function`-style members and may be left empty. Pointer handlers follow the dispatch rules in [Pointer events](#pointer-events): deepest view with a handler first, then bubbling, with capture on press.

| Member | Type | When |
|---|---|---|
| `onPointerDown` | `PointerHandler` | A button goes down (any button; check `e.button`), or a pen/finger touches. Calling `e.stopPropagation()` makes this view the capture target. |
| `onPointerMove` | `PointerHandler` | Pointer movement: hover (`e.buttons == 0`) and drags. While a button is held, delivered to the capture target even outside it. See `e.coalesced`. |
| `onPointerUp` | `PointerHandler` | A button goes up. |
| `onPointerCancel` | `PointerHandler` | The press can no longer complete (OS took the pointer, or a scroll pan swallowed the release). Not fired for views being destroyed. |
| `onPointerEnter`, `onPointerLeave` | `PointerHandler` | Pointer enters or leaves the view. **Do not bubble.** Follow the hover flag used by `hoverColor`. |
| `onClick` | `PointerCallback` | Left press and release both land on this view. Bubbles unless the handler stops it; the no-argument form ends bubbling. |
| `onDoubleClick` | `PointerHandler` | After a second left click within 500 ms / 4 px (`e.clickCount == 2`). |
| `onAuxClick` | `PointerHandler` | Middle-button click. |
| `onContextMenu` | `PointerHandler` | Right-button click, fired on release. |
| `onWheel` | `WheelHandler` | Wheel or trackpad scroll over the view. Bubbles; falls through to overflow scrolling unless a handler calls `e.stopPropagation()`. Fires even when the view is disabled. |
| `onFocus`, `onBlur` | `void()` | Focus gained or lost. |
| `onKeyDown`, `onKeyUp` | `void(KeyEvent)` | Only the focused view; no bubbling. `onKeyDown` repeats. |
| `onTextInput` | `void(uint32_t codepoint)` | Only the focused view; composed UTF-32 text. |
| `canvasDirtySource` | `bool()` | Canvas views: return `true` once to request a redraw. See [CANVAS.md](CANVAS.md#when-onpaint-runs). |

After a dispatched event, LiteUI re-polls `Dynamic<>` values and keyed lists, and relays out and repaints if something changed. Handlers run on the UI thread.

> **Migrating from the older handler set.** The per-button, position-argument handlers were replaced by the pointer-event model. The old names no longer exist in the header, so code using them will not compile.
>
> | Old | New |
> |---|---|
> | `onMiddleClick` | `onAuxClick` |
> | `onRightClick` | `onContextMenu` (fires on release) |
> | `onPressAt(x, y)`, `onMiddlePressAt`, `onRightPressAt` | `onPointerDown`, checking `e.button`; coordinates are `e.x`, `e.y` |
> | `onDragTo(x, y)` and the middle/right variants | `onPointerMove` while `e.held(button)`; capture keeps the events coming outside the view |
> | `onDragEnd` | `onPointerUp` (and `onPointerCancel` if the drag is aborted) |
> | `onHoverAt(x, y)` | `onPointerMove` with `e.buttons == 0`, or `onPointerEnter` |
> | `onHoverEnd` | `onPointerLeave` |
> | `onScrollUp`, `onScrollDown` | `onWheel`, reading the sign of `e.dy` |
>
> Handlers no longer need to return `bool` to ask for a repaint, and a drag no longer needs a separate press handler on the view to become the press target: any view with `onPointerDown` captures the pointer.

### Scrolling helpers

| Member | Meaning |
|---|---|
| `bool scrollsX() const` / `scrollsY() const` | `true` if `overflowX` / `overflowY` is anything but `Visible`. |
| `float maxScrollX() const` / `maxScrollY() const` | How far the content can currently scroll (`max(0, content - viewport)`). |
| `void requestCanvasRedraw()` | Marks a canvas view's surface stale. Apps normally use `canvasDirtySource` instead, since handlers have no handle to the tree. |

---

## `LiteUI` (the window)

```cpp
class LiteUI {
public:
    explicit LiteUI(const std::string &title = "Window",
                    int width = -1, int height = -1, bool hideTitlebar = false);
    ~LiteUI();                                  // not copyable
    ...
};
```

- `width` / `height` are the initial size in pixels. If either is `<= 0` the window starts **maximized**.
- `hideTitlebar = true` creates a window without the title bar (Linux: LiteUI's own bar is not drawn), so you can build your own and drive it with `requestMove` / `requestMinimize` / `requestMaximize` / `requestClose`.
- The constructor **throws `std::runtime_error`** if the platform cannot be initialized (for example, no Wayland connection).
- Create one window per process. `LiteUI::current()` returns the most recently created one.

### Methods

| Method | Description |
|---|---|
| `void run()` | Blocks and pumps the platform event loop until the window closes. |
| `void setRoot(View view)` | Sets or replaces the tree. Lays it out immediately (twice, polling `Dynamic<>` values in between) and repaints. The argument is copied or moved in. |
| `void requestClose()` | Asks to close. On Windows this posts a close message, so it goes through `setOnCloseRequest` if one is set. **On Linux it currently closes the window immediately** (see the note below). |
| `void forceClose()` | Closes immediately, bypassing the close-request handler. |
| `void setOnCloseRequest(std::function<void()> fn)` | Intended to be called when the user asks to close (the window's close button, Alt+F4). **The handler must call `forceClose()` itself** to actually close. With no handler, close requests close immediately. **Currently honored on Windows only** (see the note below). |

> **Close-request handler on Linux.** In the current implementation, the Wayland backend never calls the handler: the title-bar close button, the compositor's close event, and `requestClose()` all end the event loop directly. A "save changes?" prompt installed with `setOnCloseRequest` therefore works on Windows but is skipped on Linux. If you need one on both, also confirm before calling `requestClose()` yourself from your own UI, and treat the handler as a Windows-only hook until the Linux path is wired up.
| `void requestMinimize()` | Minimizes. |
| `void requestMaximize()` | **Toggles** maximized and normal. |
| `void requestMove()` | Starts an interactive window move (call it from a press handler on your own title bar). |
| `KeyModifiers modifiers() const` | The modifier keys currently held. |
| `int addInterval(int ms, std::function<void()> fn)` | Runs `fn` every `ms` milliseconds on the UI thread until removed. Each tick polls `Dynamic<>` sources and repaints. Returns a handle. |
| `void removeInterval(int handle)` | Stops that interval. |
| `void addShortcut(KeyModifiers mods, Key key, std::function<void()> fn)` | Global shortcut, checked **before** the focused view's `onKeyDown`. A shortcut matches when the key **and the exact modifier set** are equal; the first match wins and consumes the event. |
| `void setTooltipStyle(TooltipStyle s)` | Sets tooltip delay and appearance. |
| `void setWindowBackground(Color c)` | Window background behind the root. |
| `void setScrollbarColors(Color track, Color thumb)` | Window-wide default scrollbar colors (a view's own style overrides them). |
| `void requestRepaint()` | Requests a repaint. |
| `void wake()` | **Thread-safe.** Wakes the event loop from any thread; wakes coalesce. The woken loop runs the wake handler, then polls `Dynamic<>` sources and repaints. Join any thread that can call `wake()` before destroying the `LiteUI`. |
| `void setWakeHandler(std::function<void()> fn)` | Runs on the UI thread once per coalesced wake, before the repaint. |
| `static LiteUI *current()` | The active window, or `nullptr`. |

```cpp
LiteUI ui("Notes", 900, 600);
ui.setWindowBackground(Color{24, 24, 28});
ui.addShortcut(KeyModifiers{.ctrl = true}, Key::S, [&] { save(); });
ui.setOnCloseRequest([&] { if (!dirty || confirmDiscard()) ui.forceClose(); });
ui.setRoot(root);
ui.run();
```

### `TooltipStyle`

```cpp
struct TooltipStyle {
    int delayMs = 500;
    Color background{50, 50, 50, 230};
    Color textColor{255, 255, 255, 255};
    float padding = 6;
    float fontSize = 13;
    std::string fontFamily;         // empty = platform default
};
```

---

## Dialogs, clipboard and text helpers

### File and folder pickers

```cpp
struct FileFilter { std::string name; std::string pattern; };   // pattern: ";"-separated globs, "*.png;*.jpg"

std::optional<std::string>  openFilePicker  (const std::string &title = "Open File",
                                             const std::vector<FileFilter> &filters = {});
std::vector<std::string>    openFilesPicker (const std::string &title = "Open Files",
                                             const std::vector<FileFilter> &filters = {});
std::optional<std::string>  saveFilePicker  (const std::string &title = "Save File",
                                             const std::string &defaultName = "",
                                             const std::vector<FileFilter> &filters = {},
                                             const std::string &defaultExt = "");
std::optional<std::string>  openFolderPicker(const std::string &title = "Open Folder");
std::optional<std::string>  saveFolderPicker(const std::string &title = "Select Folder");
```

All are **modal and synchronous**, so they are safe to call from an `onClick`. They return `std::nullopt` (or an empty vector) on cancel or failure. `defaultExt` has no leading dot and is appended if the typed name does not already end in an extension from the filters. `saveFolderPicker` differs from `openFolderPicker` only in default title and button wording. On Linux the dialogs are provided by `zenity` or `kdialog`; if neither is installed, the pickers return nothing.

### `liteui_clipboard`

```cpp
bool                        liteui_clipboard::setText(const std::string &utf8);
std::optional<std::string>  liteui_clipboard::getText();
void                        liteui_clipboard::setTextWithFallback(const std::string &s);
std::string                 liteui_clipboard::getTextWithFallback();
std::string                 liteui_clipboard::toLF  (const std::string &in);   // CRLF / CR -> LF
std::string                 liteui_clipboard::toCRLF(const std::string &in);   // LF -> CRLF
```

- `setText` / `getText` talk to the operating system clipboard. `getText` returns the raw text (possibly with `\r\n`), or `nullopt` if it cannot be read.
- `setTextWithFallback` always keeps a copy in the process **and** tries the system clipboard. `getTextWithFallback` returns the system text normalized to `\n`, or the process copy if the system clipboard is unavailable. This makes copy and paste inside your app work even when no clipboard tool is installed on Linux.
- On Linux, the system clipboard uses `wl-copy`/`wl-paste` under Wayland, or `xclip` / `xsel` when `DISPLAY` is set.

### `liteui_text`

```cpp
size_t liteui_text::caretIndexForX(const std::string &text, const TextStyle &style,
                                   float fontSize, FontWeight fontWeight,
                                   FontStyle fontStyle, float clickX);
```

Returns the byte index in `text` whose caret position is closest to `clickX`, measured from the text's left edge. It is the helper `TextInput` uses, exposed for hand-built text widgets.

---

## Pixel formats and `ImageBuffer`

`CanvasImage` is always 8-bit RGBA. For anything with more precision (16-bit PNGs, float working images in a photo editor) use `ImageBuffer`, which is independent of the UI.

### `PixelFormat`

| Value | Storage | Color meaning |
|---|---|---|
| `RGBA8_UNORM` | 4 x `uint8` (4 bytes/pixel) | sRGB-encoded, display-referred |
| `RGBA16_UNORM` | 4 x `uint16` (8 bytes/pixel) | sRGB-encoded, display-referred |
| `RGBA16_FLOAT` | 4 x IEEE half (8 bytes/pixel) | **linear light**; max 65504, about 3 decimal digits |
| `RGBA32_FLOAT` | 4 x `float` (16 bytes/pixel) | **linear light**, not clamped to 0..1 |

Alpha is always **straight** (not premultiplied) and is never color-transformed, only scaled or quantized. Storage format, color space and what a display backend wants are separate properties; this enum describes storage only.

Helpers (all `constexpr`/`inline`, `noexcept`):

```cpp
size_t                bytesPerPixel(PixelFormat f);                 // 0 for an invalid value
std::optional<size_t> minimumRowBytes(uint32_t width, PixelFormat f);   // nullopt on overflow
std::optional<size_t> imageByteSize(uint32_t w, uint32_t h, PixelFormat f, size_t rowStride = 0);
bool                  isStorageSupported(PixelFormat f);            // true for every PixelFormat
float                 halfToFloat(uint16_t h);                      // exact
uint16_t              floatToHalf(float f);                         // round-to-nearest-even; > 65504 becomes inf; NaN kept
```

`rowStride == 0` means tightly packed. A stride may exceed one row (padding) but never be smaller.

### `ImageBuffer`

```cpp
struct ImageBuffer {
    uint32_t width = 0, height = 0;
    size_t rowStride = 0;                  // bytes between row starts
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    std::vector<std::byte> pixels;

    static std::optional<ImageBuffer> create(uint32_t w, uint32_t h, PixelFormat f, size_t stride = 0);
    bool empty() const;                    // width == 0 || height == 0
    size_t byteSize() const;
    bool valid() const;                    // fields and buffer agree
    std::byte *row(uint32_t y);            // nullptr if invalid or out of range (also const)
    bool getPixel(uint32_t x, uint32_t y, float out[4]) const;
    bool setPixel(uint32_t x, uint32_t y, const float in[4]);
};
```

- `create` returns a **zero-filled** buffer, or `nullopt` for an unsupported format, a stride below one row, a size overflow or allocation failure. Zero width or height gives a valid empty image.
- Copyable and movable; a moved-from buffer is left **empty and valid**.
- `getPixel` / `setPixel` work in four floats `r, g, b, a` **as stored**: UNORM formats are divided by 255 / 65535 (a plain normalization, **not** an sRGB decode); float formats are widened unchanged. `setPixel` on `RGBA32_FLOAT` keeps values untouched (HDR, negatives, NaN and inf survive); `RGBA16_FLOAT` rounds to half precision; UNORM formats clamp to 0..1, round to nearest and map NaN to 0. Both return `false` for an out-of-range coordinate or an invalid buffer. They go through `memcpy`, so no alignment is assumed, but they are per-pixel: for bulk work use `row(y)`.
- No color-space conversion or clamping of float data ever happens implicitly.

### `DisplaySettings` and `ClampMode`

```cpp
enum class ClampMode : uint8_t { PerChannel, PreserveHue };

struct DisplaySettings {
    float exposureEv = 0.0f;               // stops: linear rgb *= 2^exposureEv (non-finite -> 0, limited to +-32)
    ClampMode clamp = ClampMode::PerChannel;
    bool operator==(const DisplaySettings &) const = default;
};
```

The output transform used by `liteui_image::makeDisplayImage` and `Image::displaySettings`. `PerChannel` clips each of r, g, b to 0..1, so very bright colors drift toward a primary or white. `PreserveHue` scales r, g, b together when the brightest channel exceeds 1, keeping the r:g:b ratio (negatives still clip). Output encoding is fixed sRGB. It applies to float sources only; RGBA8 images pass through unchanged.

---

## `liteui_image`: decoding, encoding and display conversion

All functions here are synchronous and **report failure through the return value** (`std::nullopt` or `false`); the optional `std::string *errorOut` receives a message. None of them throw for bad input.

### 8-bit decoding to `CanvasImage`

```cpp
std::optional<CanvasImage> liteui_image::decodeFile  (const std::string &path, std::string *errorOut = nullptr);
std::optional<CanvasImage> liteui_image::decodeMemory(const uint8_t *data, size_t size,
                                                      std::string *errorOut = nullptr);
```

Decode into straight-alpha RGBA8; a 16-bit PNG is **reduced to 8 bits**. Linux: PNG and JPEG (`decodeFile` picks the codec by extension, `decodeMemory` by magic bytes). Windows: any format WIC supports.

### Header probing

```cpp
enum class ImageFileFormat : uint8_t { Unknown, Png, Jpeg };

struct ImageInfo {
    ImageFileFormat format = ImageFileFormat::Unknown;
    uint32_t width = 0, height = 0;
    int  bitDepth = 8;            // per channel: PNG 1/2/4/8/16, JPEG 8 (rarely 12)
    bool hasAlpha = false;        // an alpha channel, or a PNG tRNS chunk
    bool truncated = false;       // PNG only: the file ends before IEND (decoding it is refused)
    PixelFormat bufferFormat() const;   // what decodeFileBuffer returns: RGBA16_UNORM if bitDepth == 16, else RGBA8_UNORM
};

std::optional<ImageInfo> liteui_image::probeFile  (const std::string &path, std::string *errorOut = nullptr);
std::optional<ImageInfo> liteui_image::probeMemory(const uint8_t *data, size_t size, std::string *errorOut = nullptr);
```

Reads only the header (no pixel data), so it is cheap even for huge files. `nullopt` if the data is neither PNG nor JPEG or the header is damaged.

### Decoding to `ImageBuffer` (keeps the file's precision)

```cpp
std::optional<ImageBuffer> liteui_image::decodeFileBuffer  (const std::string &path, std::string *errorOut = nullptr);
std::optional<ImageBuffer> liteui_image::decodeMemoryBuffer(const uint8_t *data, size_t size,
                                                            std::string *errorOut = nullptr);
```

| File | Result |
|---|---|
| JPEG, or PNG with 8 bits per channel or fewer | `RGBA8_UNORM` |
| PNG with 16 bits per channel | `RGBA16_UNORM` |

Both are sRGB-encoded, display-referred, straight alpha and tightly packed. Gray becomes RGB, missing alpha becomes opaque, a PNG `tRNS` chunk becomes alpha, and palettes and 1/2/4-bit samples are expanded. The format is recognized from the **content, never the extension**. **No gamma, ICC or EXIF-orientation handling** is applied. The RGB of a pixel whose alpha is 0 is unspecified (libpng keeps what the file stored; WIC may return black for some gray PNGs), so do not rely on it. To work in float, follow with `convertImage`; a 16-bit PNG then survives losslessly as `RGBA32_FLOAT`.

PNGs are validated up front, identically on every platform, because WIC would otherwise decode damaged files "successfully" with blank rows. These fail with a message: `PNG is truncated` (cut off before IEND), `PNG is corrupt (chunk CRC mismatch)` (any chunk, ancillary ones included) and `PNG has no image data`. The CRC pass reads the whole file once. Unusual JPEGs depend on the platform codec: libjpeg (Linux) refuses CMYK/YCCK and 12-bit files; WIC (Windows) may decode them.

### Encoding

```cpp
struct PngEncodeOptions {
    int  compression = 3;          // zlib 0..9 (libpng only; WIC ignores it)
    bool dropOpaqueAlpha = true;   // fully opaque images are written as RGB
};

bool liteui_image::encodePngMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                                   std::string *errorOut = nullptr, const PngEncodeOptions &options = {});
bool liteui_image::encodePng      (const std::string &path, const ImageBuffer &image,
                                   std::string *errorOut = nullptr, const PngEncodeOptions &options = {});

struct JpegEncodeOptions {
    int quality = 90;                            // 1..100; values outside are clamped
    JpegAlpha alpha = JpegAlpha::FlattenOverWhite;
    JpegChroma chroma = JpegChroma::Auto;
};

bool liteui_image::encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                                    const JpegEncodeOptions &options, std::string *errorOut = nullptr);
bool liteui_image::encodeJpegMemory(std::vector<uint8_t> &out, const ImageBuffer &image,
                                    int quality = 90, std::string *errorOut = nullptr);
bool liteui_image::encodeJpeg(const std::string &path, const ImageBuffer &image,
                              const JpegEncodeOptions &options, std::string *errorOut = nullptr);
bool liteui_image::encodeJpeg(const std::string &path, const ImageBuffer &image,
                              int quality = 90, std::string *errorOut = nullptr);

inline constexpr uint32_t kJpegMaxSide = 65500;
inline bool jpegSizeOk(uint32_t width, uint32_t height);   // 1..kJpegMaxSide on both sides
```

- **PNG:** `RGBA8_UNORM` gives an 8-bit PNG and `RGBA16_UNORM` a 16-bit one. **The float formats are refused**: convert them to a UNORM format first with `convertImage`. The default compression of 3 is deliberate: on a 24-megapixel 16-bit photo, level 6 took about six times longer than level 3 for a file only about 1.4% smaller. Raise it for archival output.
- **JPEG:** baseline, **8-bit only**: `RGBA8_UNORM` is accepted and anything else is refused (convert first), as is a side over `kJpegMaxSide`. JPEG has no alpha: `FlattenOverWhite` (default) composites over white, which gives the same bytes on every platform; `Discard` writes the stored RGB and ignores alpha. `JpegChroma::Auto` uses 4:4:4 at quality 90 or above and 4:2:0 below that; `S420` gives the smallest files; `S444` keeps colored edges crisp. On Windows the chroma setting is passed to WIC as an encoder property and may not be honored for every combination. The `int` overloads use default options at that quality.
- **Memory vs file:** the `Memory` forms **replace** `out`. They are what you want when splicing EXIF or ICC data into the bytes. The file forms write to `<path>.tmp` and rename it over `path`, so a failed write never leaves a half-written file behind.

### Conversion and display

```cpp
float liteui_image::srgbToLinear(float encoded);
float liteui_image::linearToSrgb(float linear);

bool liteui_image::convertImage(const ImageBuffer &source, ImageBuffer &destination);

std::optional<CanvasImage>  liteui_image::toCanvasImage(const ImageBuffer &source);
std::optional<CanvasImage>  liteui_image::toCanvasImage(const ImageBuffer &source, const DisplaySettings &settings);

std::optional<ImageBuffer>  liteui_image::makeDisplayImage(const ImageBuffer &source,
                                                           const DisplaySettings &settings = {});
std::optional<ImageBuffer>  liteui_image::makeDisplayImage(const ImageBuffer &source,
                                                           const DisplaySettings &settings,
                                                           const ImageRect &region);

struct ImageRect { uint32_t x = 0, y = 0, width = 0, height = 0; };

std::optional<ImageBuffer>  liteui_image::downscaleImage(const ImageBuffer &source, uint32_t factor);
```

- **`srgbToLinear` / `linearToSrgb`** are the IEC 61966-2-1 transfer functions. Not clamped; negative inputs are mirrored about 0; NaN in gives NaN out.
- **`convertImage`** converts `source` into the format named by **`destination.format`** and replaces `destination` with a tightly packed image of the source's size. Returns `false` and leaves `destination` untouched if the source is invalid, either format is unsupported or allocation fails. Same format is a plain copy (padding dropped). UNORM to UNORM rescales the encoded values (8 to 16 bit is x257; 16 to 8 rounds) with no transfer function involved. Anything else goes through linear float: UNORM sources are sRGB-decoded, and UNORM destinations get NaN to 0, a clamp to 0..1, sRGB encoding and rounding. Float destinations keep the values (`RGBA16_FLOAT` rounds to half and overflows to inf past 65504).
- **`makeDisplayImage`** is the display adapter, producing an `RGBA8_UNORM` image ready to paint. Float sources get `rgb *= 2^exposureEv`, then the clamp mode, sRGB encoding and rounding to 8 bits (NaN and negatives become 0; +inf is a very bright value; alpha is only clamped and quantized). `RGBA8_UNORM` is copied unchanged whatever the settings (convert to `RGBA32_FLOAT` first if you want exposure on an 8-bit image). `RGBA16_UNORM` is display-referred like RGBA8 and is rescaled to 8 bits. Rows are processed in small chunks, so an `RGBA16_FLOAT` photo is displayed without doubling its memory.
- **The `region` overload** does the same for one rectangle, producing an image of the region's size that is bit-for-bit the matching crop of the full display image. It is the building block for tiled or viewport-only rendering of large photos. `nullopt` if the region does not lie inside the source.
- **`toCanvasImage`** is `makeDisplayImage` followed by a copy into a `CanvasImage`. `nullopt` if the buffer is invalid or too large for `CanvasImage`'s `int` sizes.
- **`downscaleImage`** makes a reduced-resolution copy for fast previews (for example while a slider moves). Each output pixel is the box average of up to `factor x factor` source pixels, computed in linear light and weighted by alpha so transparent pixels do not darken edges (a fully transparent block gets rgb 0). The output is `ceil(w/factor) x ceil(h/factor)` in the **same format** as the source. `factor == 1` copies; `factor == 0` or an invalid source gives `nullopt`.

```cpp
// Open a 16-bit PNG, brighten it a stop, show it, and later export 8-bit JPEG.
std::string err;
auto buf = liteui_image::decodeFileBuffer("photo.png", &err);        // RGBA16_UNORM
ImageBuffer work;  work.format = PixelFormat::RGBA32_FLOAT;
liteui_image::convertImage(*buf, work);                              // linear float working copy
auto shared = std::make_shared<ImageBuffer>(std::move(work));

Image view;
view.hdrSource = shared;
view.displaySettings = [&] { return DisplaySettings{.exposureEv = ev}; };

ImageBuffer out8;  out8.format = PixelFormat::RGBA8_UNORM;
liteui_image::convertImage(*shared, out8);                           // linear -> sRGB, clamped
liteui_image::encodeJpeg("out.jpg", out8, 92, &err);
```

---

## Compile-time switches and CMake

### Macros

| Macro | Where | Effect |
|---|---|---|
| `LITEUI_IMPLEMENTATION` | Exactly one `.cpp` per executable, before `#include "liteui.hpp"` | Compiles the platform backend into that translation unit. |
| `LITEUI_PROFILE` | Define for the implementation translation unit (CMake: `-DLITEUI_PROFILE=ON`) | Prints per-second frame timing to `stderr`. Compiles to nothing when undefined. |
| `LITEUI_CANVAS_GL` | Linux, implementation translation unit (CMake: on by default, `-DLITEUI_CANVAS_GL=OFF` to disable) | Renders `Canvas` nodes with the OpenGL ES 3.0 backend (GPU). Without it the previous Cairo backend is used. Ignored on Windows, where canvases always render on the GPU through Direct2D/Direct3D 11. |
| `LITEUI_VERSION_MAJOR`, `LITEUI_VERSION_MINOR` | Defined by the header | Library version (currently 0.5). |

The backend is chosen by `#if defined(_WIN32)` (Direct2D) versus everything else (Wayland). See [BUILDING.md](BUILDING.md) for libraries and flags.

### CMake targets and options

`liteui` is a **header-only `INTERFACE` library**, aliased as `liteui::liteui`. Because the implementation is compiled inside your executable, everything it needs (include path, `cxx_std_23`, the platform libraries, `LITEUI_PROFILE` and `LITEUI_CANVAS_GL`) propagates to whatever links it:

```cmake
add_subdirectory(path/to/liteui)               # or FetchContent
target_link_libraries(app PRIVATE liteui::liteui)
```

| Option | Default | Effect |
|---|---|---|
| `LITEUI_PROFILE` | `OFF` | Defines `LITEUI_PROFILE` for consumers (frame timing on `stderr`). |
| `LITEUI_CANVAS_GL` | `ON` | Linux only: OpenGL ES 3.0 canvas backend (needs an EGL driver with GLES 3.0 and the GLESv2 headers). |
| `LITEUI_BUILD_TESTS` | `ON` | Top-level builds only: fetches or finds Catch2 v3 and builds `tests/`. |
| `LITEUI_SANITIZE` | `OFF` | Builds the tests with AddressSanitizer (plus UBSan on GCC/Clang). |
| `LITEUI_BUILD_EXAMPLES` | `ON` | Top-level builds only: builds `examples/`. |

Tests and examples are skipped entirely when LiteUI is pulled in via `add_subdirectory` or `FetchContent`, so consumers never download Catch2.

**Platform libraries.** Windows links `user32 gdi32 d3d11 dxgi d2d1 dwrite windowscodecs ole32` (no C compiler needed). Linux requires, through `pkg-config`: `wayland-client`, `wayland-cursor`, `wayland-egl`, `cairo`, `pangocairo`, `egl`, `glesv2`, `xkbcommon`, `libpng` and `libjpeg`; it also enables the C language for the vendored protocol sources.

**Vendored Wayland protocols (Linux).** The generated bindings for `xdg-shell`, `xdg-decoration` and `tablet-unstable-v2` (pen input) are checked in under `platform/wayland-protocols/`, so consumers need only the runtime libraries above, **not** `wayland-scanner` or the `wayland-protocols` package. All six generated files (`*-protocol.c` and `*-client-protocol.h` for each) must exist; they are built into a static library `liteui_wayland_protocols` that `liteui::liteui` links. On a fresh checkout they are missing: configure still succeeds with a warning if `wayland-scanner` and `wayland-protocols` are installed, and you then run

```bash
cmake --build <builddir> --target regen-protocols
git add platform/wayland-protocols && git commit
```

and re-run configure. If the files are missing and the generator tools are not installed, configure fails with a `FATAL_ERROR` (on Debian/Ubuntu: `sudo apt install libwayland-bin wayland-protocols`). The trade-off of vendoring is that the files are frozen at the `wayland-protocols` version that generated them; re-run `regen-protocols` and commit to pick up upstream changes.

### Presets (`CMakePresets.json`)

Configure presets use Ninja and are gated by host OS (Windows presets only appear on Windows, Linux presets on Linux). Build and test presets share the configure preset's name.

| Windows (MSVC `cl.exe`, x64) | Linux (GCC) | Build directory | Extra cache setting |
|---|---|---|---|
| `msvc-debug` | `gcc-debug` | `build/debug` | `CMAKE_BUILD_TYPE=Debug` |
| `msvc-release` | `gcc-release` | `build/release` | `CMAKE_BUILD_TYPE=Release` |
| `msvc-debug-profile` | `gcc-debug-profile` | `build/debug-profile` | `LITEUI_PROFILE=ON` |
| `msvc-release-profile` | `gcc-release-profile` | `build/release-profile` | `LITEUI_PROFILE=ON` |
| `msvc-debug-asan` | `gcc-debug-asan` | `build/debug-asan` | `LITEUI_SANITIZE=ON` |

```bash
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --preset gcc-debug
```

There are test presets for the debug, release and ASan variants of each toolchain (none for the profile variants). Test presets print output on failure, time out at 120 s and treat "no tests found" as an error.

---

## Errors, threading and lifetime

- **Exceptions.** The `LiteUI` constructor throws `std::runtime_error` on platform initialization failure. `resolveStatic` throws `std::bad_variant_access` if a field holds a callback. Everything else reports failure through return values (`std::optional`, empty vectors, `bool`) or `onError` callbacks. That includes every `liteui_image` decoder, encoder and converter, which fill an optional `errorOut` message instead of throwing.
- **Threads.** All API calls belong to the UI thread, except `LiteUI::wake()`. Handlers, interval callbacks, `Dynamic<>` callbacks, `onPaint` and the wake handler all run on the UI thread. The pure image functions in `liteui_image` (decode, encode, convert, downscale) touch no UI state, but LiteUI makes no thread-safety promise for them, so run them on a worker thread only on data that thread owns, and hand results back with `wake()`.
- **Pointer events and destruction.** `onPointerCancel` is not sent to a view that is being destroyed (keyed-list rebuild, `setRoot()`), so release any capture-related app state from the code that triggers the rebuild.
- **Copies.** `addChild` and `setRoot` copy their argument. State captured by callbacks must outlive the window (locals in `main` captured by reference are fine while `run()` is on the stack).
- **Keyed lists.** `itemBuilder` runs only for new keys; reused rows keep what they were built with, so make anything that can change a `Dynamic<>` callback. See the [README](../README.md#keyed-dynamic-lists). A change in the key list also **clears keyboard focus, in-progress presses and drags, and the tooltip** (without calling `onBlur`); set `TextInput::state->requestFocus` or a `wantsFocus` callback in the same update to restore focus. Details in [ARCHITECTURE.md](ARCHITECTURE.md#sharp-edges).
- **Destruction.** Join every thread that may call `wake()` before the `LiteUI` is destroyed.
