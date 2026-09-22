# API reference

Field-by-field, method-by-method reference for `liteui.hpp`. Read
[`getting-started.md`](getting-started.md) first if you haven't built a
window yet — this doc assumes you know the `View` tree / `Dynamic<T>` /
`addChild` model already. See [`PLATFORM_NOTES.md`](PLATFORM_NOTES.md)
for anything that behaves differently between Windows and Linux, or is a
deliberate v1 limitation.

## Contents

- [`Dynamic<T>`](#dynamict)
- [`Size`](#size)
- [`EdgeInsets`](#edgeinsets)
- [Layout enums](#layout-enums)
- [`Style`](#style)
- [`Text` / `TextStyle`](#text--textstyle)
- [`View`](#view)
- [`Canvas`](#canvas)
- [`CanvasContext`](#canvascontext)
- [`CanvasGradient`](#canvasgradient)
- [`CanvasImage`](#canvasimage)
- [`Image`](#image)
- [`Svg`](#svg)
- [`TextInput`](#textinput)
- [`Color`](#color)
- [Input types](#input-types) (`Key`, `KeyEvent`, `KeyModifiers`, `MouseButton`)
- [File & folder pickers](#file--folder-pickers)
- [Clipboard](#clipboard-liteui_clipboard)
- [`LiteUI`](#liteui)
- [`TooltipStyle`](#tooltipstyle)
- [`Box`](#box)

---

## `Dynamic<T>`

```cpp
template <class T> using Dynamic = std::variant<T, std::function<T()>>;
```

Almost every meaningful `Style`/`Text`/`TextStyle` field is a
`Dynamic<T>`. Assign either a plain `T` or a `std::function<T()>`
directly (`x = 5;` or `x = [&]{ return count; };`) — no wrapper syntax
needed. A callback is re-polled once per dispatched event (click, key,
scroll, timer tick), and the field only shows up as changed (triggering
a repaint/relayout) if the returned value actually differs from last
time.

---

## `Size`

One axis's sizing rule.

```cpp
enum class Kind { Fixed, Percentage, Fit, Full };
```

| Factory | Meaning |
|---|---|
| `Size::pixel(v)` | Fixed `v` px |
| `Size::percentage(v)` | `v`% (0–100) of the parent's resolved size |
| `Size::fit()` | Shrink to content — **the default** |
| `Size::full()` | Fill available space along that axis |

Percentages resolve to `0` when the ancestor's own size is itself `Fit`
(indefinite) — see PLATFORM_NOTES.

## `EdgeInsets`

```cpp
struct EdgeInsets {
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v);   // all four sides equal
};
```

Used by `Style::margin` and `Style::padding` (both `Dynamic<EdgeInsets>`).

## Layout enums

| Enum | Values | Used by |
|---|---|---|
| `FlexDirection` | `Row`, `Column` | `Style::direction` |
| `Justify` | `Start`, `End`, `Center`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly` | `Style::justifyContent` (main axis) |
| `Align` | `Start`, `End`, `Center`, `Stretch` | `Style::alignItems` (cross axis, single line) |
| `FlexWrap` | `NoWrap`, `Wrap` | `Style::flexWrap` |
| `AlignContent` | `Start`, `End`, `Center`, `SpaceBetween`, `SpaceAround`, `SpaceEvenly`, `Stretch` | `Style::alignContent` (distributes multiple wrapped lines) |
| `Position` | `Static`, `Absolute` | `Style::position` |
| `Display` | `Flex`, `None` | `Style::display` — `None` skips layout/paint/hit-test entirely, space **not** reserved |
| `Visibility` | `Visible`, `Hidden` | `Style::visibility` — `Hidden` still reserves space, only paint/hit-test/hover are skipped |
| `Overflow` | `Visible`, `Hidden`, `Scroll`, `Auto` | `Style::overflowX` / `overflowY` |

`Overflow` semantics: `Visible` (default) never clips or scrolls.
`Hidden` clips but has no scrollbar/interaction. `Scroll` always clips
and always shows that axis's scrollbar. `Auto` clips and shows the
scrollbar only once content actually exceeds the viewport.

## `Style`

Every `View` and `Text`/`Canvas` (via their own `.style`) carries one of
these.

| Field | Type | Default | Notes |
|---|---|---|---|
| `width`, `height` | `Dynamic<Size>` | `Size::fit()` | |
| `minWidth`, `maxWidth`, `minHeight`, `maxHeight` | `float` | `0` / `∞` | Clamp applied **after** `width`/`height` resolve. `maxWidth < minWidth` is treated as `maxWidth == minWidth`. |
| `margin`, `padding` | `Dynamic<EdgeInsets>` | `{0,0,0,0}` | |
| `direction` | `FlexDirection` | `Row` | Not `Dynamic` — plain field |
| `justifyContent` | `Justify` | `Start` | |
| `alignItems` | `Align` | `Stretch` | |
| `gap` | `float` | `0` | Space between flex children (and between wrapped lines) |
| `flexWrap` | `FlexWrap` | `NoWrap` | |
| `alignContent` | `AlignContent` | `Stretch` | Only matters with >1 wrapped line |
| `flexGrow` | `float` | `0` | |
| `flexShrink` | `float` | `1` | |
| `backgroundColor` | `Dynamic<Color>` | `{0,0,0,0}` | |
| `borderWidth` | `Dynamic<float>` | `0` | Uniform on all sides |
| `borderColor` | `Dynamic<Color>` | `{0,0,0}` | |
| `borderRadius` | `Dynamic<float>` | `0` | |
| `hoverColor` | `std::optional<Color>` | unset | Replaces `backgroundColor` while the pointer is over this view. `isHovered` itself is computed by the library from pointer position — never app-sourced. |
| `position` | `Position` | `Static` | |
| `left`, `top`, `right`, `bottom` | `Dynamic<float>` | `NaN` (unset) | Only meaningful when `position == Absolute`. See [Absolute positioning](#absolute-positioning) below. |
| `zIndex` | `Dynamic<int>` | `0` | Stacking order among **all** absolute nodes tree-wide, ties broken by document order |
| `overflowX`, `overflowY` | `Overflow` | `Visible` | Setting either to non-`Visible` makes this view a scroll container |
| `contentPanEnabled` | `bool` | `true` | Whether click-and-drag inside the content area pans/scrolls it |
| `wheelScrollEnabled` | `bool` | `true` | Whether the mouse wheel moves this view's scroll position. `onScrollUp`/`onScrollDown` still fire regardless of this flag. |
| `display` | `Dynamic<Display>` | `Flex` | |
| `visibility` | `Dynamic<Visibility>` | `Visible` | |

### Absolute positioning

A `Position::Absolute` child is pulled out of flex distribution and
placed against its parent's content box using `left`/`top`/`right`/
`bottom` (each `NaN` = unset). If `width`/`height` is `Fit` and **both**
opposing edges are set (`left` **and** `right`, or `top` **and**
`bottom`), the size is derived from them (`contentW - left - right`);
otherwise size resolves normally (`Fixed`/`Percentage`/`Fit`/`Full`)
against the containing block, same as an in-flow child.

Absolute nodes are collected and painted/hit-tested in one **global**
pass on top of the whole flow tree, stacked by `zIndex` (ties by
document/discovery order) — this ordering is identical between painting
and hit-testing. They are **not** threaded through scroll-aware
clipping/hit-testing (a v1 trade-off, not a bug — see PLATFORM_NOTES).

## `Text` / `TextStyle`

`Text` is author-facing and leaf-only: `addChild(Text)` flattens it into
a `View` (`isText = true`) — `Text` itself never appears in the retained
tree afterward.

```cpp
struct Text {
  Dynamic<std::string> label = std::string{};
  Style style;                 // layout only: width/height/margin/position/etc
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  FontStyle fontStyle = FontStyle::Normal;
  std::string fontFamily;      // empty = platform default UI font
  Dynamic<Color> color = Color{0,0,0};
  TextAlign align = TextAlign::Start;
  TextOverflow overflow = TextOverflow::Clip;
  TextWrap wrap = TextWrap::Wrap;
  float lineHeight = 0;        // 0 = auto from font metrics
  float letterSpacing = 0;
  int maxLines = 0;            // 0 = unlimited
  bool underline = false;
  bool strikethrough = false;
};
```

| Enum | Values |
|---|---|
| `FontWeight` | `Thin`(100) … `Black`(900), including `Regular`(400), `Bold`(700), etc. — matches CSS numeric weights |
| `FontStyle` | `Normal`, `Italic` |
| `TextAlign` | `Start`, `Center`, `End`, `Justify` |
| `TextOverflow` | `Clip`, `Ellipsis` |
| `TextWrap` | `Wrap`, `NoWrap` |

`TextStyle` is the internal mirror of these fields stored on the `View`
a `Text` flattens into (`View::textStyle`) — same fields, no `style`
member (layout stays on `View::style`).

## `View`

The retained tree node. You mostly build these implicitly via
`addChild(Text{...})` / `addChild(Canvas{...})` / etc., but nothing
stops you from filling a `View` directly for a plain container/box.

### Adding children

```cpp
void addChild(View child);
void addChild(Text t);
void addChild(Canvas c);
void addChild(TextInput ti);
void addChild(Image img);
void addChild(Svg s);
```

Each non-`View` overload flattens the argument into a `View` and
appends it. `Image`/`Svg` decode/parse **synchronously**, at `addChild`
time.

### Core fields

| Field | Type | Notes |
|---|---|---|
| `style` | `Style` | |
| `children` | `std::vector<View>` | Populated via `addChild`, or left empty in favor of `keysSource`/`itemBuilder` below |
| `key` | `std::string` | Identity within a keyed sibling list; set automatically by the reconciler, empty = "not keyed" |
| `isText` / `text` / `textStyle` | `bool` / `Dynamic<std::string>` / `TextStyle` | Set by `toView(Text)`; `isText` implies `children.empty()` always |
| `isCanvas` / `onPaint` | `bool` / `std::function<void(CanvasContext&)>` | Set by `toView(Canvas/Image/Svg/TextInput)` |
| `focusable` | `bool` | Default `false`. Click-to-focus is automatic for any focusable view. |
| `tooltip` | `std::string` | Plain field (not `Dynamic`), looked up once when the tooltip shows |
| `disabled` | `Dynamic<bool>` | A disabled view never receives click/press dispatch |

### Keyed dynamic lists

```cpp
std::function<std::vector<std::string>()> keysSource;
std::function<View(const std::string &key)> itemBuilder;
```

Set these instead of populating `children` directly for a list whose
items change over time. `keysSource` is polled after every dispatched
event; when the returned list differs from what's built, children are
reconciled by `key` — survivors (matched key) are moved over untouched
(cached text layout, canvas surface, scroll position, hover state all
carry over); only new keys are built, only dropped keys are freed.

### Event handlers

All are plain `std::function` members, empty by default.

| Handler | Signature | Fires when | Bubbles? |
|---|---|---|---|
| `onClick` / `onMiddleClick` / `onRightClick` | `void()` | Full press-and-release over the same view | Yes — to nearest ancestor with a handler |
| `onPressAt` / `onMiddlePressAt` / `onRightPressAt` | `void(float localX, float localY)` | Instant a press lands; coordinates local to this view (0,0 = view's own top-left) | No |
| `onDragTo` / `onMiddleDragTo` / `onRightDragTo` | `void(float localX, float localY)` | Continuously while a press that started on this view is held and moving | No |
| `onScrollUp` / `onScrollDown` | `void()` | Per wheel notch, independent of pixel-based content scrolling | Yes |
| `onLayout` | `void(float x, float y, float w, float h)` | Once per `relayout()`, right after this node's absolute on-screen box is finalized | — |
| `onFocus` / `onBlur` | `void()` | Gains/loses keyboard focus | — |
| `onKeyDown` / `onKeyUp` | `void(KeyEvent)` | Only while this view is the focused view; no bubbling. `onKeyDown` also fires on OS/library key-repeat. | No |
| `onTextInput` | `void(uint32_t codepoint)` | Only while focused; an already-composed UTF-32 codepoint (this is what a text field should append, not `onKeyDown`) | No |
| `canvasDirtySource` | `bool()` | `isCanvas` nodes only — polled like the sources above; return `true` once to force the next paint to re-invoke `onPaint`, then go back to `false` | — |

### Canvas-node methods

```cpp
void requestCanvasRedraw();   // marks this canvas node's cached backing
                               // surface stale; no-op on a non-canvas node
```

### Scroll-related read accessors

```cpp
bool scrollsX() const;        // style.overflowX != Overflow::Visible
bool scrollsY() const;
float maxScrollX() const;     // max(0, contentW - w)
float maxScrollY() const;     // max(0, contentH - h)
```

### `Computed` (read-only, filled by layout)

`View::computed` holds everything the layout/paint passes derive.
Renderers only ever read this — never re-derive it from `style`. The
fields you'll actually want to read from app code:

| Field | Meaning |
|---|---|
| `x`, `y`, `w`, `h` | Border-box, absolute window pixel coordinates |
| `isHovered` | Set by the library from pointer position every move event |
| `contentW`, `contentH` | Only meaningful when scrolling on that axis — the content's natural (scrollable) size |
| `scrollX`, `scrollY` | Current scroll offset, always clamped to `[0, maxScroll*]` |

Everything else under `Computed` (cached text layouts, GL textures,
resolved-dynamic caches) is renderer-internal.

### Lifetime

```cpp
void freeTextResources();
```

Frees this node's cached text-/canvas-rendering resources and recurses
into children. `View` frees nothing in a destructor (views get copied
freely while building a tree), so this must be called explicitly before
a tree is discarded — `LiteUI::setRoot()` and `~LiteUI()` are the only
two places a live tree is normally retired; call it yourself only if
you're managing a `View` tree outside those paths.

---

## `Canvas`

Author-facing leaf node; `addChild(Canvas)` flattens it into an
`isCanvas` `View`.

```cpp
struct Canvas {
  Style style;
  std::function<void(CanvasContext &)> onPaint;
  std::function<void()> onClick, onMiddleClick, onRightClick;
  std::function<void(float, float)> onPressAt, onMiddlePressAt, onRightPressAt;
  std::function<void(float, float)> onDragTo, onMiddleDragTo, onRightDragTo;
  std::function<void()> onScrollUp, onScrollDown;
  std::function<bool()> canvasDirtySource;
  bool focusable = false;
  std::function<void()> onFocus, onBlur;
  std::function<void(KeyEvent)> onKeyDown;
  std::function<void(uint32_t)> onTextInput;
};
```

`onPaint` is **not** a per-frame callback. It re-runs only when the
canvas is actually dirty: first paint, a resize, or when
`requestCanvasRedraw()` / `canvasDirtySource` reports true. Treat it as
"rebuild my picture from whatever state I closed over," not "draw one
frame of a game loop."

## `CanvasContext`

Passed by reference into `onPaint`. HTML5-canvas-style 2D API, backed
by Direct2D on Windows and Cairo on Linux — see PLATFORM_NOTES for the
handful of places the two backends diverge (`getImageData`,
`clearRect`, `globalCompositeOperation`, radial gradients, shadows,
`strokeText`).

```cpp
float width() const;
float height() const;
```

### State stack

```cpp
void save();
void restore();
```

### Transform

```cpp
void translate(float x, float y);
void rotate(float radians);
void scale(float sx, float sy);
void transformBy(float a, float b, float c, float d, float e, float f); // pre-multiplies
void setTransform(float a, float b, float c, float d, float e, float f); // replaces
void resetTransform();
```

### Paint / style state

```cpp
void setFillColor(Color c);
void setFillGradient(const CanvasGradient &g);
void setStrokeColor(Color c);
void setStrokeGradient(const CanvasGradient &g);
void setLineWidth(float w);
void setLineCap(LineCap c);          // Butt | Round | Square
void setLineJoin(LineJoin j);        // Miter | Round | Bevel
void setMiterLimit(float m);
void setLineDash(const std::vector<float> &dashes);
void setLineDashOffset(float offset);
void setGlobalAlpha(float a);        // 0..1
void setGlobalCompositeOperation(CompositeOp op);
void setShadow(Color color, float blurPx /* accepted, ignored — unblurred */,
                float offsetX, float offsetY);
void setFont(const std::string &family, float sizePx,
             FontWeight weight = FontWeight::Regular,
             FontStyle style = FontStyle::Normal);
void setTextAlign(TextAlign a);
void setTextBaseline(TextBaseline b); // Top | Middle | Alphabetic | Bottom
```

`CompositeOp`: `SourceOver`, `SourceIn`, `SourceOut`, `SourceAtop`,
`DestinationOver`, `DestinationIn`, `DestinationOut`, `DestinationAtop`,
`Lighter`, `Copy`, `Xor`, `Multiply`, `Screen`. Full range honored on
Linux; only `SourceOver`-equivalent behavior on Windows (see
PLATFORM_NOTES).

### Path building

Same semantics as HTML canvas: `fill()`/`stroke()`/`clip()` do **not**
clear the current path — only `beginPath()` does.

```cpp
void beginPath();
void closePath();
void moveTo(float x, float y);
void lineTo(float x, float y);
void quadraticCurveTo(float cpx, float cpy, float x, float y);
void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x, float y);
void arc(float cx, float cy, float r, float startAngle, float endAngle, bool ccw = false);
void ellipse(float cx, float cy, float rx, float ry, float rotation,
             float startAngle, float endAngle, bool ccw = false);
void arcTo(float x1, float y1, float x2, float y2, float radius);
void rect(float x, float y, float w, float h);
void roundRect(float x, float y, float w, float h, float radius);
```

### Drawing

```cpp
void fill(FillRule rule = FillRule::NonZero);     // NonZero | EvenOdd
void stroke();
void clip(FillRule rule = FillRule::NonZero);
void fillRect(float x, float y, float w, float h);
void strokeRect(float x, float y, float w, float h);
void clearRect(float x, float y, float w, float h);
```

### Text

```cpp
struct TextMetricsResult { float width; };
TextMetricsResult measureText(const std::string &text);
void fillText(const std::string &text, float x, float y, float maxWidth = -1);
void strokeText(const std::string &text, float x, float y, float maxWidth = -1);
```

`maxWidth` is accepted for HTML-canvas API familiarity but currently
does nothing (no squeeze-to-fit). `strokeText` approximates a stroked
outline by nudging fill-text in a ring of directions — not a true
glyph-contour stroke.

### Images

```cpp
void drawImage(const CanvasImage &img, float dx, float dy);
void drawImage(const CanvasImage &img, float dx, float dy, float dw, float dh);
void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
               float dx, float dy, float dw, float dh);
```

No image *decoding* happens here — `CanvasImage` just wraps raw,
already-decoded RGBA8 pixels. Use the `Image`/`Svg` nodes, or
`liteui_image::decodeFile`/`decodeMemory`, to get pixels from a
PNG/JPEG file.

### Pixel data

```cpp
std::optional<CanvasImage> getImageData(float x, float y, float w, float h);
void putImageData(const CanvasImage &img, float dx, float dy);
```

`getImageData` always returns `std::nullopt` on Windows (unsupported in
this build — see PLATFORM_NOTES); fully supported on Linux, reading
straight from the canvas's own backing surface. `putImageData` ignores
the current transform and paints directly at `(dx, dy)`, matching HTML
canvas semantics.

## `CanvasGradient`

```cpp
CanvasGradient g = CanvasGradient::linear(x0, y0, x1, y1);
CanvasGradient g = CanvasGradient::radial(x0, y0, r0, x1, y1, r1);
g.addColorStop(offset /* 0..1 */, color);
```

Copied by value into `setFillGradient`/`setStrokeGradient` — safe to
build on the stack. Radial gradients are exact on Linux (Cairo's native
two-circle model); Direct2D only models one circle plus a focal point,
so the inner-radius stop is approximated there (see PLATFORM_NOTES).

## `CanvasImage`

```cpp
struct CanvasImage {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels; // width*height*4, row-major, RGBA8, straight alpha
  CanvasImage() = default;
  CanvasImage(int w, int h); // allocates + zero-fills pixels
};
```

---

## `Image`

Author-facing leaf node built on `Canvas`. `addChild(Image)` decodes
synchronously (PNG/JPEG via libpng/libjpeg on Linux, WIC on Windows).

```cpp
struct Image {
  Style style;
  std::string path;                        // decoded at addChild time
  std::vector<uint8_t> memoryData;          // decoded at addChild time
  Dynamic<std::shared_ptr<CanvasImage>> source; // see below
  ObjectFit fit = ObjectFit::Fill;
  Color backgroundColor = Color{0,0,0,0};   // painted behind letterboxed margins
  std::function<void(int width, int height)> onLoad;  // fires once, synchronously
  std::function<void(const std::string &error)> onError;
};
```

Set exactly one of `path` / `memoryData` / `source`. `source` can be a
plain `std::shared_ptr<CanvasImage>` (fixed image) or a
`std::function<std::shared_ptr<CanvasImage>()>` re-polled every
dispatch cycle the same way `Text::label` is — return a *different*
`shared_ptr` (comparison is by pointer, not pixel content) to swap the
displayed image; the node repaints and reflows (if sized `Fit`)
automatically.

`ObjectFit`: `Fill`, `Contain`, `Cover`, `None`, `ScaleDown` — same
semantics as CSS `object-fit`.

## `Svg`

Author-facing leaf node built on `Canvas`, parsed once at `addChild`
time and replayed through `CanvasContext` on every paint — stays fully
vector, re-rendering cleanly at any size including across a window
resize (unlike `Image`, which is a fixed-resolution bitmap).

```cpp
struct Svg {
  Style style;
  std::string path;    // exactly one of path / source
  std::string source;  // inline SVG XML text, used if path is empty
  ObjectFit fit = ObjectFit::Contain; // SVG's "meet" == Contain
  Color backgroundColor = Color{0,0,0,0};
  std::function<void()> onLoad;
  std::function<void(const std::string &error)> onError;
};
```

Supported subset: `path`, `rect`, `circle`, `ellipse`, `line`,
`polyline`, `polygon`, `<g>`/`<symbol>`/nested `<svg>`, `<use>` (both
`href` and `xlink:href`, cycle/depth-guarded), presentation attributes,
inline `style=""`, and a single-class subset of `<style>` block CSS
(`.foo { fill: ...; }`, matched in file order — no specificity, no
compound selectors). **Not supported:** gradients, patterns,
`clipPath`/`mask`, `<text>` elements. See PLATFORM_NOTES for `<use>`/
`<symbol>` sizing caveats and how group `opacity` is approximated.

## `TextInput`

Author-facing single-line text field built on `Canvas`, with caret,
click-to-position, arrow-key navigation, and editing already wired up.

```cpp
struct TextInput {
  Style style;
  std::string text;
  std::string placeholder;
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  std::string fontFamily;
  Color textColor = Color{0,0,0};
  Color placeholderColor = Color{160,160,160};
  Color caretColor = Color{20,20,20};
  Color borderColor = Color{180,180,180};
  Color focusedBorderColor = Color{50,120,220};
  float leftPadding = 6.0f;
  std::function<void(const std::string &)> onChange;
  std::function<void(const std::string &)> onSubmit; // fires on Enter

  std::shared_ptr<TextInputState> state = std::make_shared<TextInputState>();
};
```

`state` is shared across copies of the flattened `View` — you normally
never touch it directly, but it's there if you need to read/set
`text`/`cursor`/`focused` from outside the widget's own handlers.

`Ctrl+V` paste is wired up. **ASCII-only** (codepoints `0x20`–`0x7F`)
for both typed and pasted text — see PLATFORM_NOTES; this does not
affect plain `Text` rendering.

For anything this widget doesn't cover, build a `Canvas` by hand using
`liteui_text::caretIndexForX` the same way this does internally.

---

## `Color`

```cpp
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 255;
  bool operator==(const Color &) const = default;
};
```

Plain aggregate — `Color{255, 100, 100}` or `Color{0, 0, 0, 128}`.

---

## Input types

```cpp
enum class MouseButton { Left, Middle, Right };

struct KeyModifiers {
  bool shift = false, ctrl = false, alt = false, super = false;
  bool operator==(const KeyModifiers &) const = default;
};

struct KeyEvent {
  Key key = Key::Unknown;
  KeyModifiers mods;
};
```

`Key` is a platform-independent enum covering `A`–`Z`, `N0`–`N9`,
`F1`–`F12`, `Enter`, `Escape`, `Backspace`, `Tab`, `Space`, `Delete`,
arrow keys, `Home`/`End`/`PageUp`/`PageDown`, `Shift`/`Control`/`Alt`/
`Super`, and the common punctuation keys (`Minus`, `Equal`,
`LeftBracket`, `RightBracket`, `Backslash`, `Semicolon`, `Quote`,
`Comma`, `Period`, `Slash`, `Grave`).

---

## File & folder pickers

Blocking, synchronous, modal — safe to call from an `onClick` handler.
On Linux these shell out to `zenity` or `kdialog` (in that order); if
neither is installed, every picker call returns `std::nullopt` as if
cancelled (no built-in fallback dialog).

```cpp
struct FileFilter { std::string name; std::string pattern; }; // ';'-separated globs, e.g. "*.png;*.jpg"

std::optional<std::string>
openFilePicker(const std::string &title = "Open File",
               const std::vector<FileFilter> &filters = {});

std::optional<std::string>
saveFilePicker(const std::string &title = "Save File",
               const std::string &defaultName = "",
               const std::vector<FileFilter> &filters = {},
               const std::string &defaultExt = "");

std::optional<std::string> openFolderPicker(const std::string &title = "Open Folder");
std::optional<std::string> saveFolderPicker(const std::string &title = "Select Folder");
```

`saveFilePicker`'s `defaultExt` (no leading dot) is appended if the
user's typed name doesn't already end in an extension covered by
`filters`. All four return the chosen absolute path, or `std::nullopt`
on cancel/failure.

## Clipboard (`liteui_clipboard`)

```cpp
namespace liteui_clipboard {
  bool setText(const std::string &utf8);
  std::optional<std::string> getText(); // nullopt = no usable OS clipboard;
                                        // "" = clipboard has no text

  // What TextInput actually uses — always mirrors into an in-process
  // fallback so copy/paste still round-trips within THIS app even with
  // no OS clipboard tool available:
  void setTextWithFallback(const std::string &s);
  std::string getTextWithFallback();
}
```

On Linux, `setText`/`getText` shell out to `wl-copy`/`wl-paste`
(Wayland), `xclip`, or `xsel`, checked in that order and only counted
as available if the matching display-server env var is also set. See
PLATFORM_NOTES for the silent in-process fallback's caveats.

---

## `LiteUI`

The window class.

```cpp
explicit LiteUI(const std::string &title = "Window",
                int width = -1, int height = -1,
                bool hideTitlebar = false);
```

`width`/`height` of `-1` (the default) starts the window maximized.
`hideTitlebar` (Linux only) suppresses the custom client-side titlebar
— on Windows this has no effect (native decorations are always used).

### Running

```cpp
void run(); // blocks, pumps the platform event loop until the window closes
```

### Tree management

```cpp
void setRoot(View view);
```

Lays the tree out immediately and repaints. Safe to call more than
once — replaces the previous tree (freeing its cached text/canvas
resources first) and invalidates any stale click/focus/scroll-drag/
tooltip pointers into the old tree.

```cpp
void addBox(const Box &box);
```

Queues a static filled rectangle drawn underneath the `View` tree.
Mostly a low-level escape hatch predating `View`/`Canvas` — prefer a
plain `View` with `backgroundColor` for anything dynamic.

### Window control

```cpp
void requestClose();
void requestMinimize();
void requestMaximize(); // toggles: maximizes if normal, restores if maximized
void requestMove();     // starts an interactive window-drag (call from onClick)
```

### Timers

```cpp
int  addInterval(int ms, std::function<void()> fn);
void removeInterval(int handle);
```

`fn` runs every `ms` milliseconds until removed. Each tick automatically
triggers the same poll-dynamic-sources + relayout + repaint cycle as a
click/key dispatch, so `fn` can freely mutate plain captured
variables and have it show up on screen with no extra plumbing.

### Global shortcuts

```cpp
void addShortcut(KeyModifiers mods, Key key, std::function<void()> fn);
```

Fires regardless of what (if anything) has keyboard focus — checked
*before* the focused view's own `onKeyDown`. Use for things like
Ctrl+S that should work even while, say, a plain button has focus.

### Appearance

```cpp
void setTooltipStyle(TooltipStyle s);
void setWindowBackground(Color c);
void setScrollbarColors(Color track, Color thumb);
```

### Misc

```cpp
void requestRepaint();       // polls dynamic sources, relayouts, repaints
static LiteUI *current();    // the active instance (used internally by
                              // TextInput/Image for their own timers/polling)
```

---

## `TooltipStyle`

```cpp
struct TooltipStyle {
  int delayMs = 500;
  Color background{50, 50, 50, 230};
  Color textColor{255, 255, 255, 255};
  float padding = 6.0f;
  float fontSize = 13.0f;
  std::string fontFamily; // empty = platform default
};
```

Applies to every `View::tooltip` in the window; set once via
`LiteUI::setTooltipStyle`. `delayMs` is the hover time before a tooltip
appears.

---

## `Box`

```cpp
struct Box {
  int width = 0, height = 0;
  int pos_x = 0, pos_y = 0;
  Color color;
};
```

A static, axis-aligned filled rectangle for `LiteUI::addBox` — window-
local pixel coordinates, `(0,0)` at top-left. See `addBox` above.
