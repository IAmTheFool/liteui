# API reference

Everything LiteUI exposes lives in `liteui.hpp`, in the **global namespace** (plus four helper namespaces: `liteui_clipboard`, `liteui_image`, `liteui_text` and, internally, `liteui_layout`/`liteui_svg`). This page is the reference for the public types and functions. For concepts and behavior, see the guides: [LAYOUT.md](LAYOUT.md), [CANVAS.md](CANVAS.md) and [BUILDING.md](BUILDING.md).

- [Conventions](#conventions)
- [Value types](#value-types)
- [Enumerations](#enumerations)
- [`Dynamic<T>`](#dynamict)
- [Input types](#input-types)
- [`Style`](#style)
- [Leaf nodes](#leaf-nodes): `Text`, `TextInput`, `Image`, `Svg`, `Canvas`
- [`View`](#view)
- [`LiteUI` (the window)](#liteui-the-window)
- [Dialogs, clipboard and image helpers](#dialogs-clipboard-and-image-helpers)
- [Compile-time switches](#compile-time-switches)
- [Errors, threading and lifetime](#errors-threading-and-lifetime)

## Conventions

- **One implementation per program.** Include `liteui.hpp` anywhere; define `LITEUI_IMPLEMENTATION` before including it in **exactly one** `.cpp` of each executable (see [BUILDING.md](BUILDING.md#rule-1-one-implementation-per-program)).
- **C++23.** The CMake target sets `cxx_std_23`.
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
| `MouseButton` | `Left`, `Middle`, `Right` |

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
| `position` | `Position` | `Static` | |
| `left`, `top`, `right`, `bottom` | `Dynamic<float>` | `NaN` (unset) | **D** |
| `zIndex` | `Dynamic<int>` | `0` | **D**. Absolute views only. |
| `overflowX`, `overflowY` | `Overflow` | `Visible` | |
| `contentPanEnabled` | `bool` | `true` | Drag inside a scroll container to pan. |
| `wheelScrollEnabled` | `bool` | `true` | Wheel moves a scroll container. |
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

**Supported editing:** typing, Left/Right, Home/End, Backspace/Delete, Enter (`onSubmit`), click to place the caret, `Ctrl+V` paste (line breaks and non-ASCII bytes are stripped). **Not supported:** selection, copy/cut, select-all, undo, multi-line, and non-ASCII input (typed characters outside printable ASCII are ignored, and the caret moves by bytes). For richer fields, build your own on a `Canvas` using `liteui_text::caretIndexForX`.

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
};
```

- **Source priority:** `source` (fixed or callback), then `path`, then `memoryData`.
- **Decoding is synchronous**, inside `addChild`. `onLoad` / `onError` fire there, once.
- **Natural size:** if `width`/`height` is `Fit`, the node takes the image's pixel size. With a callback `source` it follows whichever image is current.
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

Custom drawing surface. Fields: `style`, `onPaint(CanvasContext &)`, the pointer, hover and wheel handlers, `canvasDirtySource`, and the focus and key handlers. The full model, the drawing API, and platform differences are in **[CANVAS.md](CANVAS.md)**.

```cpp
struct Canvas {
    Style style;
    std::function<void(CanvasContext &)> onPaint;
    std::function<void()> onClick, onMiddleClick, onRightClick;
    std::function<void(float x, float y)> onPressAt, onMiddlePressAt, onRightPressAt;
    std::function<void(float x, float y)> onDragTo, onMiddleDragTo, onRightDragTo;
    std::function<void()> onDragEnd;
    std::function<bool(float x, float y)> onHoverAt;
    std::function<bool()> onHoverEnd;
    std::function<void()> onScrollUp, onScrollDown;
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
| `disabled` | `Dynamic<bool>` | A disabled view ignores press/click handlers and shows no tooltip. |
| `focusable` | `bool` | Click gives it keyboard focus. Default `false`. |
| `wantsFocus` | `std::function<bool()>` | Polled; when it returns `true` this view takes focus. |
| `tooltip` | `std::string` | Shown after hovering for `TooltipStyle::delayMs`. Plain field, read when the tooltip appears. |
| `onLayout` | `std::function<void(float x, float y, float w, float h)>` | After each layout, with the final window-space box. |
| `computed` | `View::Computed` | Layout results and caches. **Read-only for apps.** Contains `x`, `y`, `w`, `h` (border box in window coordinates), `contentW`, `contentH`, `scrollX`, `scrollY`, and `isHovered`. |

`isText`, `text`, `textStyle`, `isCanvas` and `onPaint` are set by `addChild` when it flattens a leaf; do not set them by hand.

### Event handlers

All are `std::function` members and may be left empty.

| Member | Signature | When |
|---|---|---|
| `onClick`, `onMiddleClick`, `onRightClick` | `void()` | Press and release both land on this view. **Bubbles** to the nearest ancestor with a handler if the deepest view under the pointer has none. |
| `onPressAt`, `onMiddlePressAt`, `onRightPressAt` | `void(float x, float y)` | Immediately on press, with coordinates local to the view's border box. |
| `onDragTo`, `onMiddleDragTo`, `onRightDragTo` | `void(float x, float y)` | On every move while that button is held, after a press that started on this view. The view must also have a press handler (`onClick` or `onPressAt`) to be the press target. |
| `onDragEnd` | `void()` | Once when a drag ends (release, or the platform aborts it). Not fired if the view is destroyed mid-drag. |
| `onHoverAt` | `bool(float x, float y)` | On pointer movement over the view. Return `true` if app state changed so LiteUI re-polls and repaints. |
| `onHoverEnd` | `bool()` | Once when the pointer leaves. Same return convention. |
| `onScrollUp`, `onScrollDown` | `void()` | A wheel notch over the view. Bubbles. Independent of pixel scrolling. |
| `onFocus`, `onBlur` | `void()` | Focus gained or lost. |
| `onKeyDown`, `onKeyUp` | `void(KeyEvent)` | Only the focused view; no bubbling. `onKeyDown` repeats. |
| `onTextInput` | `void(uint32_t codepoint)` | Only the focused view; composed UTF-32 text. |
| `canvasDirtySource` | `bool()` | Canvas views: return `true` once to request a redraw. See [CANVAS.md](CANVAS.md#when-onpaint-runs). |

After any handler returns, LiteUI re-polls `Dynamic<>` values and keyed lists, and relays out and repaints if something changed. Handlers run on the UI thread.

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

## Dialogs, clipboard and image helpers

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

### `liteui_image`

```cpp
std::optional<CanvasImage> liteui_image::decodeFile  (const std::string &path,
                                                      std::string *errorOut = nullptr);
std::optional<CanvasImage> liteui_image::decodeMemory(const uint8_t *data, size_t size,
                                                      std::string *errorOut = nullptr);
```

Decode synchronously into straight-alpha RGBA8. `errorOut`, if given, receives a message on failure. Linux: PNG and JPEG (`decodeFile` picks the codec by extension, `decodeMemory` by magic bytes). Windows: any format WIC supports.

### `liteui_text`

```cpp
size_t liteui_text::caretIndexForX(const std::string &text, const TextStyle &style,
                                   float fontSize, FontWeight fontWeight,
                                   FontStyle fontStyle, float clickX);
```

Returns the byte index in `text` whose caret position is closest to `clickX`, measured from the text's left edge. It is the helper `TextInput` uses, exposed for hand-built text widgets.

---

## Compile-time switches

| Macro | Where | Effect |
|---|---|---|
| `LITEUI_IMPLEMENTATION` | Exactly one `.cpp` per executable, before `#include "liteui.hpp"` | Compiles the platform backend into that translation unit. |
| `LITEUI_PROFILE` | Define for the implementation translation unit (CMake: `-DLITEUI_PROFILE=ON`) | Prints per-second frame timing to `stderr`. Compiles to nothing when undefined. |

The backend is chosen by `#if defined(_WIN32)` (Direct2D) versus everything else (Wayland). See [BUILDING.md](BUILDING.md) for libraries and flags.

---

## Errors, threading and lifetime

- **Exceptions.** The `LiteUI` constructor throws `std::runtime_error` on platform initialization failure. `resolveStatic` throws `std::bad_variant_access` if a field holds a callback. Everything else reports failure through return values (`std::optional`, empty vectors, `bool`) or `onError` callbacks.
- **Threads.** All API calls belong to the UI thread, except `LiteUI::wake()`. Handlers, interval callbacks, `Dynamic<>` callbacks, `onPaint` and the wake handler all run on the UI thread.
- **Copies.** `addChild` and `setRoot` copy their argument. State captured by callbacks must outlive the window (locals in `main` captured by reference are fine while `run()` is on the stack).
- **Keyed lists.** `itemBuilder` runs only for new keys; reused rows keep what they were built with, so make anything that can change a `Dynamic<>` callback. See the [README](../README.md#keyed-dynamic-lists).
- **Destruction.** Join every thread that may call `wake()` before the `LiteUI` is destroyed.
