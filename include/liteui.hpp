// include/liteui.hpp
//
// Single-header, PImpl-free cross-platform window.
// Platform members/methods are selected at compile time via #ifdef,
// so there's exactly one LiteUI definition per build — no vtable,
// no heap-allocated Impl, no indirection through a pointer.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dxgiformat.h> // DXGI_FORMAT_R8G8B8A8_UNORM, used by Canvas::drawImage/putImageData
#include <windows.h>
#include <windowsx.h> // GET_X_LPARAM/GET_Y_LPARAM/GET_WHEEL_DELTA_WPARAM, used by the mouse-wheel handlers
#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")
#else
#include "xdg-decoration-client-protocol.h" // Generated client bindings for the xdg-decoration protocol (server-side vs client-side decorations).
#include "xdg-shell-client-protocol.h" // Generated client bindings for the xdg-shell protocol (toplevel windows, configure events).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <cairo/cairo.h>
#include <fcntl.h>
#include <pango/pangocairo.h> // Text shaping/layout + measurement; rendering still goes through GL (see ensureTextTexture)
#include <unistd.h>
#include <wayland-client.h> // Core Wayland client protocol: displays, registries, surfaces, shm.
#include <wayland-cursor.h> // wl_cursor_theme_load / wl_cursor_theme_get_cursor, for showing resize/arrow cursors.
#include <wayland-egl.h>
#endif

// Plain RGB color, one byte per channel.
struct Color {
  uint8_t r = 0, g = 0, b = 0, a = 255;
};

// Which physical mouse button an event refers to. Used to key the
// per-button press/drag tracking in LiteUI (pressedView_/dragView_) and
// to select which of a View's onClick/onMiddleClick/onRightClick (etc.)
// triple gets checked/fired.
enum class MouseButton { Left, Middle, Right };

#if defined(_WIN32)
// Converts a UTF-8 std::string to the UTF-16 wide string Win32/DirectWrite
// APIs require. File-scope (rather than a LiteUI member) because both
// LiteUI's WndProc path and liteui_text's DirectWrite layout builder need
// it, and DirectWrite objects aren't tied to any particular window.
inline std::wstring toWide(const std::string &s) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
  return w;
}
#endif

// A static, axis-aligned filled rectangle the caller wants drawn on the
// window. Position is in window-local pixel coordinates, (0,0) at top-left.
struct Box {
  int width = 0;
  int height = 0;
  int pos_x = 0;
  int pos_y = 0;
  Color color;
};

// Width (vertical bar) / height (horizontal bar) of a scrollbar's track, in
// pixels. Shared between the layout engine (which reserves this much space
// out of a scroll container's content box on whichever edges show a bar)
// and both renderers (which draw the track/thumb at exactly this size) —
// keeping it in one place means layout and painting can never disagree
// about how much room a bar takes up.
constexpr float kScrollbarThickness = 12.0f;

// A field settable as either a fixed value or a callback polled after each
// dispatched event (see checkForUpdates) — lets author code write
// `x = "hello";` or `x = []{ return someVar; };` through plain assignment.
template <class T> using Dynamic = std::variant<T, std::function<T()>>;

// What every read site outside checkForUpdates should call: the stored
// value directly if it's static (always fresh, no indirection), or `cache`
// — the last value checkForUpdates polled from the callback — if dynamic.
template <class T>
inline const T &resolveDynamic(const Dynamic<T> &field, const T &cache) {
  if (auto *v = std::get_if<T>(&field))
    return *v;
  return cache;
}

// ---------------- Layout engine: Size / Style / View ----------------

// A size along one axis. Fixed/Percentage are self-explanatory; Fit sizes
// to the sum/max of children (like CSS's "auto" on a flex item); Full fills
// whatever space the parent gives along that axis (like width: 100%, but
// resolved from available space rather than the parent's own size).
struct Size {
  enum class Kind { Fixed, Percentage, Fit, Full };
  Kind kind = Kind::Fit;
  float value = 0; // pixels for Fixed, 0-100 for Percentage; unused otherwise

  static Size pixel(float v) { return {Kind::Fixed, v}; }
  static Size percentage(float v) { return {Kind::Percentage, v}; }
  static Size fit() { return {Kind::Fit, 0}; }
  static Size full() { return {Kind::Full, 0}; }
};

enum class FlexDirection { Row, Column };
enum class Justify {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly
};

enum class Align { Start, End, Center, Stretch };

// Whether children that overflow the main axis wrap onto additional lines.
enum class FlexWrap { NoWrap, Wrap };

// How multiple flex lines are distributed along the cross axis. Only
// meaningful when FlexWrap::Wrap actually produces more than one line;
// with a single line this reduces to Stretch filling crossAvail (matching
// the old un-wrapped behavior) or the others packing that one line at the
// start.
enum class AlignContent {
  Start,
  End,
  Center,
  SpaceBetween,
  SpaceAround,
  SpaceEvenly,
  Stretch
};

struct EdgeInsets {
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v) { return {v, v, v, v}; }
};

enum class Position { Static, Absolute };

// display:none analog — a false-y node is skipped entirely by layout,
// paint, and hit-testing, everywhere Position::Absolute is already
// skipped for being "out of flow". Space is NOT reserved.
enum class Display { Flex, None };

// visibility:hidden analog — layout still reserves the node's space and
// siblings flow around it normally; only paint/hit-test/hover skip it.
enum class Visibility { Visible, Hidden };

// CSS-style overflow behavior for one axis of a container. Visible (the
// default) behavior: children are never clipped and never
// scroll, regardless of how big they get. Hidden clips children to the
// container's box but offers no scrollbar/interaction. Scroll always
// clips *and* always shows that axis's scrollbar, even if content
// currently fits. Auto clips and shows the scrollbar only when content
// actually exceeds the viewport on that axis.
enum class Overflow { Visible, Hidden, Scroll, Auto };

struct Style {
  Dynamic<Size> width = Size::fit();
  Dynamic<Size> height = Size::fit();

  // Clamp bounds applied (in pixels) after width/height above are resolved.
  // Defaults impose no constraint. maxWidth < minWidth (or the height
  // equivalent) is treated as maxWidth == minWidth rather than producing a
  // negative range.
  float minWidth = 0;
  float maxWidth = std::numeric_limits<float>::infinity();
  float minHeight = 0;
  float maxHeight = std::numeric_limits<float>::infinity();

  EdgeInsets margin;
  EdgeInsets padding;

  FlexDirection direction = FlexDirection::Row;
  Justify justifyContent = Justify::Start;
  Align alignItems = Align::Stretch;
  float gap = 0;
  FlexWrap flexWrap = FlexWrap::NoWrap;
  AlignContent alignContent = AlignContent::Stretch;

  float flexGrow = 0;
  float flexShrink = 1;

  Dynamic<Color> backgroundColor = Color{255, 255, 255};
  Dynamic<float> borderWidth = 0.0f;
  Dynamic<Color> borderColor = Color{0, 0, 0};
  Dynamic<float> borderRadius = 0.0f;

  // Applied instead of backgroundColor while the pointer is over this
  // view. Set directly by the app; isHovered itself is derived
  // automatically by LiteUI::updateHover from pointer position, never
  // app-sourced.
  std::optional<Color> hoverColor;

  // Absolute children are pulled out of flex distribution entirely and
  // placed against the parent's content box using left/top/right/bottom.
  // NaN means "unset" for each edge. If width/height is Fit and both
  // opposing edges are set, size is derived from them (contentW - left -
  // right); otherwise size resolves normally (Fixed/Percentage/Fit/Full)
  // against the containing block, same as an in-flow child would.
  Position position = Position::Static;
  float left = std::numeric_limits<float>::quiet_NaN();
  float top = std::numeric_limits<float>::quiet_NaN();
  float right = std::numeric_limits<float>::quiet_NaN();
  float bottom = std::numeric_limits<float>::quiet_NaN();
  // Stacking order among all Absolute nodes tree-wide (not just siblings).
  // Ties break by document order — see collectAbsolutes().
  int zIndex = 0;

  // When either axis is non-Visible, this view becomes a scroll container:
  // its children are measured at their natural size (not squeezed to fit)
  // and clipped+offset by the view's live scroll position. See
  // View::Computed::scrollX/scrollY and the "Scrolling" section of the
  // layout engine below for how the two axes are handled independently.
  Overflow overflowX = Overflow::Visible;
  Overflow overflowY = Overflow::Visible;
  bool contentPanEnabled = true;
  // Same idea as contentPanEnabled, but gates pixel scrolling driven by
  // the mouse wheel/trackpad axis (see applyWheelScroll) instead of a
  // click-and-drag pan. Defaults to true. Set to false to stop the wheel
  // from directly moving this view's scroll position — the view can
  // still be scrolled via its scrollbar (thumb drag or track click).
  // onScrollUp/onScrollDown below still fire on every wheel notch
  // regardless of this flag (they're dispatched independently by
  // dispatchScroll, never gated), so the app can hook them to implement
  // its own effect instead.
  bool wheelScrollEnabled = true;
  Display display = Display::Flex;
  Visibility visibility = Visibility::Visible;
};

// ---------------- Text: author-facing, leaf-only ----------------
enum class FontWeight : int {
  Thin = 100,
  ExtraLight = 200,
  Light = 300,
  Regular = 400,
  Medium = 500,
  SemiBold = 600,
  Bold = 700,
  ExtraBold = 800,
  Black = 900
};
enum class FontStyle { Normal, Italic };
enum class TextAlign { Start, Center, End, Justify };
enum class TextOverflow { Clip, Ellipsis };
enum class TextWrap { Wrap, NoWrap };

// Internal mirror of Text's styling fields, stored on the View a Text
// flattens into (see View::toView below). Kept as its own struct so View
// doesn't have to duplicate every Text field under a different name.
struct TextStyle {
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  FontStyle fontStyle = FontStyle::Normal;
  std::string fontFamily; // empty = platform default UI font
  Color color{0, 0, 0};
  TextAlign align = TextAlign::Start;
  TextOverflow overflow = TextOverflow::Clip;
  TextWrap wrap = TextWrap::Wrap;
  float lineHeight = 0; // 0 = auto from font metrics
  float letterSpacing = 0;
  int maxLines = 0; // 0 = unlimited
  bool underline = false;
  bool strikethrough = false;
};

// Author-facing leaf node. addChild(Text) flattens this into a View (see
// View::toView) — Text itself never appears in the retained tree.
struct Text {
  Dynamic<std::string> label = std::string{};
  Style
      style; // layout only — width/height/margin/position/etc, reused from View
  float fontSize = 16.0f;
  FontWeight fontWeight = FontWeight::Regular;
  FontStyle fontStyle = FontStyle::Normal;
  std::string fontFamily;
  Color color{0, 0, 0};
  TextAlign align = TextAlign::Start;
  TextOverflow overflow = TextOverflow::Clip;
  TextWrap wrap = TextWrap::Wrap;
  float lineHeight = 0;
  float letterSpacing = 0;
  int maxLines = 0;
  bool underline = false;
  bool strikethrough = false;
};

// ---------------- Canvas: retained, immediate-mode-style 2D drawing
// ----------------
//
// Canvas is deliberately modeled after the HTML5 <canvas> 2D context: the
// app gets a CanvasContext and calls familiar methods (moveTo/lineTo/arc/
// fill/stroke/fillText/drawImage/...) on it. Unlike HTML canvas, the app
// never touches raw pixels directly except via getImageData/putImageData —
// everything else is vector drawing, matching what Direct2D (Windows) and
// Cairo (Linux) both do natively.
//
// CanvasContext itself is platform-specific (see the two full definitions
// guarded by #if defined(_WIN32) below, after liteui_text) since its
// internals wrap either an ID2D1RenderTarget or a cairo_t directly rather
// than going through a generic vtable — consistent with this file's "one
// compiled definition per build, no vtable" design. Both definitions
// expose the exact same public method surface, so app code that only
// calls CanvasContext's public API compiles unchanged on either platform.
//
// Known limitations (fine for v1, revisit if needed):
//  - No image decoding (PNG/JPEG/etc.) is included; CanvasImage just wraps
//    raw, already-decoded RGBA8 pixels the app supplies (e.g. via stb_image
//    used externally).
//  - getImageData is fully supported on Linux (backed by a Cairo image
//    surface) but unsupported on Windows in this build (a plain
//    ID2D1RenderTarget isn't CPU-readable without WIC/DXGI plumbing this
//    header doesn't pull in) — it always returns std::nullopt there.
//    putImageData and drawImage work on both platforms.
//  - globalCompositeOperation maps onto the full range of Cairo operators
//    on Linux; a plain ID2D1RenderTarget (v1) has no Porter-Duff blend
//    control at all, so on Windows it's stored but has no visible effect
//    — everything always draws with normal source-over alpha blending
//    there.
//  - clearRect gives a true sub-rectangle clear on Linux (via Cairo's
//    CAIRO_OPERATOR_CLEAR) but clears the ENTIRE canvas on Windows
//    (Direct2D v1's Clear() ignores clip/transform and always wipes the
//    whole render target).
//  - Radial gradients use Cairo's native two-circle model exactly on
//    Linux; Direct2D only models a single circle plus a focal point, so
//    the inner-radius stop is approximated there by rescaling stop
//    offsets rather than drawn exactly.
//  - Shadows are a simple flat-color, unblurred offset copy (no Gaussian
//    blur), since neither backend is asked to do a blur pass here.
//  - strokeText approximates a stroked glyph outline by drawing the fill
//    text in the stroke color, nudged in a ring of directions — it is not
//    a true outline of the glyph contours.
//  - fillText/strokeText's maxWidth parameter is accepted for API
//    familiarity but currently unused (no horizontal squeeze-to-fit).
class CanvasContext;

// A simple RGBA8, straight- (non-premultiplied) alpha bitmap. The app is
// responsible for populating `pixels` (e.g. from a decoded image file or
// generated procedurally) — this header does no image-format decoding.
struct CanvasImage {
  int width = 0, height = 0;
  std::vector<uint8_t> pixels; // width*height*4 bytes, row-major, RGBA8

  CanvasImage() = default;
  CanvasImage(int w, int h)
      : width(w), height(h),
        pixels(static_cast<size_t>(std::max(0, w)) * std::max(0, h) * 4, 0) {}
};

struct CanvasGradientStop {
  float offset; // 0..1
  Color color;
};

// A linear or radial gradient paint, built via the static factories and
// addColorStop(). Passed to CanvasContext::setFillGradient/
// setStrokeGradient (copied by value, so it's safe to build one on the
// stack and hand it straight to the setter).
struct CanvasGradient {
  enum class Kind { Linear, Radial } kind = Kind::Linear;
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // Linear: the two endpoints.
                                        // Radial: (x0,y0) inner circle
                                        // center, (x1,y1) outer circle
                                        // center.
  float r0 = 0, r1 = 0; // Radial only: inner/outer circle radii.
  std::vector<CanvasGradientStop> stops;

  static CanvasGradient linear(float x0, float y0, float x1, float y1) {
    CanvasGradient g;
    g.kind = Kind::Linear;
    g.x0 = x0;
    g.y0 = y0;
    g.x1 = x1;
    g.y1 = y1;
    return g;
  }
  static CanvasGradient radial(float x0, float y0, float r0, float x1, float y1,
                               float r1) {
    CanvasGradient g;
    g.kind = Kind::Radial;
    g.x0 = x0;
    g.y0 = y0;
    g.r0 = r0;
    g.x1 = x1;
    g.y1 = y1;
    g.r1 = r1;
    return g;
  }
  void addColorStop(float offset, Color color) {
    stops.push_back({offset, color});
  }
};

enum class LineCap { Butt, Round, Square };
enum class LineJoin { Miter, Round, Bevel };
enum class TextBaseline { Top, Middle, Alphabetic, Bottom };
enum class FillRule { NonZero, EvenOdd };

// Subset of HTML canvas's globalCompositeOperation. Fully honored on
// Linux (maps 1:1 onto cairo_operator_t); on Windows only SourceOver and
// Copy are distinguished (see the file-level Canvas limitations note).
enum class CompositeOp {
  SourceOver,
  SourceIn,
  SourceOut,
  SourceAtop,
  DestinationOver,
  DestinationIn,
  DestinationOut,
  DestinationAtop,
  Lighter,
  Copy,
  Xor,
  Multiply,
  Screen
};

// Author-facing leaf node, addChild(Canvas)'d the same way Text is —
// flattened into a View with isCanvas=true (see View::toView(Canvas)).
// `onPaint` is invoked (with a CanvasContext bound to this node's own
// backing surface) whenever the canvas needs to be redrawn: once after
// the first layout, and again any time `canvasDirtySource` reports true
// (see its own comment on View — that's the mechanism an interactive
// canvas, like a paint program responding to mouse drags via `onDragTo`,
// uses to request the next redraw, since those handlers have no direct
// reference back into the tree) — plain drawing calls made once are
// *not* re-issued on every frame the way a real-time game loop's draw
// callback would be; treat onPaint as "rebuild my picture from whatever
// app state it closes over" rather than "run every frame".
struct Canvas {
  Style style;
  std::function<void(CanvasContext &)> onPaint;
  std::function<void()> onClick;
  std::function<void()> onMiddleClick;
  std::function<void()> onRightClick;
  std::function<void(float localX, float localY)> onPressAt;
  std::function<void(float localX, float localY)> onMiddlePressAt;
  std::function<void(float localX, float localY)> onRightPressAt;
  std::function<void(float localX, float localY)> onDragTo;
  std::function<void(float localX, float localY)> onMiddleDragTo;
  std::function<void(float localX, float localY)> onRightDragTo;
  std::function<void()> onScrollUp;
  std::function<void()> onScrollDown;
  std::function<bool()> canvasDirtySource;
};

// A node in the retained layout tree. Set `style` and `children`; the engine
// fills in `computed` (absolute window pixel coordinates) during layout.
// Renderers only ever read `computed`, never re-derive it from `style`.
class View {
public:
  Style style;
  std::vector<View> children;

  // True for a View created via addChild(Text) — a text leaf. isText
  // implies children.empty() always; text/textStyle are meaningless
  // otherwise.
  bool isText = false;
  Dynamic<std::string> text;
  TextStyle textStyle;

  // True for a View created via addChild(Canvas) — a canvas leaf. isCanvas
  // implies children.empty() always (a canvas has no flow children of its
  // own); onPaint is meaningless otherwise. See the Canvas struct's own
  // comment for what "invoked" means here.
  bool isCanvas = false;
  std::function<void(CanvasContext &)> onPaint;

  // Fired on a left-click whose point lands on this view (see LiteUI::hitTest).
  // Bubbles: if a deeper view under the point has no handler, the nearest
  // containing ancestor's onClick fires instead.
  std::function<void()> onClick;

  // Same bubbling semantics as onClick, for the middle and right buttons.
  std::function<void()> onMiddleClick;
  std::function<void()> onRightClick;

  // Fired the instant a left-button press lands on this view (before
  // any matching release/onClick pairing) — unlike onClick, this gives
  // the press point in coordinates *local* to this view (0,0 at its
  // own top-left), which onClick has no way to expose. Lets a view like
  // a slider track compute "where along my width was I clicked" without
  // the library needing to know anything about sliders specifically.
  std::function<void(float localX, float localY)> onPressAt;
  std::function<void(float localX, float localY)> onMiddlePressAt;
  std::function<void(float localX, float localY)> onRightPressAt;

  // Fired continuously while a left-button press that started on this
  // view is still held and the pointer moves — unlike onPressAt (which
  // fires once, at the instant of the initial press), this fires on
  // every subsequent move until release, giving the *current* press
  // point in this view's own local coordinates. Lets a view like a
  // slider track drive a live drag instead of only sampling the
  // initial click position.
  std::function<void(float localX, float localY)> onDragTo;
  // Same idea as onDragTo, tracked independently per button — a
  // middle-button drag and a left-button drag can't be started by the
  // same physical press, but nothing stops an app from wiring both up
  // on the same view for different purposes.
  std::function<void(float localX, float localY)> onMiddleDragTo;
  std::function<void(float localX, float localY)> onRightDragTo;

  // Fired once per relayout(), right after placeNode() finalizes this
  // node's absolute on-screen box. Lets the app read a view's real
  // x/y/w/h to drive some other view's position — e.g. anchoring a
  // dropdown/menu to the button that opened it.
  std::function<void(float x, float y, float w, float h)> onLayout;

  // Fired on a discrete wheel notch (or trackpad step) whose point lands
  // on this view, independent of pixel-based content scrolling — lets a
  // plain, non-overflow view (e.g. a stepper/spinner arrow) react to the
  // wheel the same way onClick reacts to a press. Bubbles exactly like
  // onClick: the nearest ancestor with a handler fires if the deepest
  // hit view has none.
  std::function<void()> onScrollUp;
  std::function<void()> onScrollDown;

  std::function<float()> positionSource;  // pixels — drives style.left
                                          // directly (Position::Absolute
                                          // slider thumbs)
  std::function<float()> topSource;       // pixels — drives style.top,
                                          // same idea as positionSource
  std::function<bool()> displaySource;    // true -> Display::Flex,
                                          // false -> Display::None
  std::function<bool()> visibilitySource; // true -> Visibility::Visible,
                                          // false -> Visibility::Hidden
  // isCanvas nodes only. Polled the same way as the sources above; return
  // true once to make the next paint/render pass re-invoke onPaint (see
  // Canvas's own comment), then go back to returning false until there's
  // something new to draw. This is the mechanism to use from inside a
  // canvas's own onPressAt/onDragTo/onClick handlers — those are plain
  // callbacks with no reference back into the tree, so they can't call
  // View::requestCanvasRedraw() on themselves directly; stashing a dirty
  // flag in app state and reporting+clearing it here is the workaround.
  // A typical pattern: `bool dirty = false;` in the app's own state,
  // set true wherever a stroke/shape is added, and
  // `canvasDirtySource = [&]{ bool d = dirty; dirty = false; return d; };`
  std::function<bool()> canvasDirtySource;

  Dynamic<bool> disabled = false;

  struct Computed {
    float x = 0, y = 0, w = 0, h = 0; // border-box, absolute window coords

    // Set by LiteUI::updateHover from pointer position on every move
    // event; drives style.hoverColor at paint time. Not app-sourced.
    mutable bool isHovered = false;

    // True whenever this node's own paint output may be stale — flipped
    // by checkForUpdates()/updateHover() on any actual change, cleared by
    // whichever paint pass consumes it. Not yet wired into per-node skip
    // logic (both paintView/renderView still walk unconditionally today);
    // this is scaffolding for that later.
    mutable bool dirty = true;

    // Only meaningful when style.overflowX/Y != Visible. contentW/contentH
    // is how big this view's children naturally want to be (the scrollable
    // extent); scrollX/scrollY is how far the content is currently
    // scrolled, always clamped to [0, max(0, content - viewport)]. A
    // non-scrollable view leaves these at 0 and they're simply unused.
    float contentW = 0, contentH = 0;
    float scrollX = 0, scrollY = 0;

    // Only meaningful for isCanvas nodes. Starts true so the very first
    // paint/render pass always invokes onPaint at least once; cleared by
    // whichever backend (re)builds the cached backing surface below, and
    // set again only by View::requestCanvasRedraw() or a resize (see
    // ensureCanvasSurface/ensureCanvasTarget) — plain repaints (e.g. from
    // an unrelated sibling's hover change) do NOT re-invoke onPaint.
    mutable bool canvasNeedsRedraw = true;

    // Last polled result when text/disabled/value hold a callback; unused
    // (and untouched) when they hold a plain value — see resolveDynamic.
    mutable std::string resolvedText;
    mutable bool resolvedDisabled = false;
    mutable float resolvedValue = 0.0f;
    mutable Color resolvedBackgroundColor{255, 255, 255};
    mutable Size resolvedWidth = Size::fit();
    mutable Size resolvedHeight = Size::fit();
    mutable float resolvedBorderWidth = 0.0f;
    mutable Color resolvedBorderColor{0, 0, 0};
    mutable float resolvedBorderRadius = 0.0f;

    // Cached platform text backing for isText nodes, rebuilt by the
    // renderer whenever the width it was built for goes stale (a resize
    // or relayout can change how the text wraps). `mutable` so paint
    // code — which only ever sees a `const View&` — can lazily build/
    // rebuild this. Nothing here is ever auto-freed by a destructor (see
    // View::freeTextResources) since Views get copied while a tree is
    // being built (e.g. `root.addChild(heading)` copies `heading`), and
    // an auto-freeing destructor would risk a double-Release/double-
    // glDelete across those copies.
#if defined(_WIN32)
    mutable IDWriteTextLayout *textLayout = nullptr;
    mutable float textLayoutBuiltForWidth = -1.0f;
    mutable std::string textLayoutBuiltForText;
    // A GPU-backed offscreen render target the size of this canvas node's
    // inner (padding-excluded) content box, drawn into by onPaint and
    // then blitted into the window's own render target every paint pass
    // (see LiteUI::ensureCanvasTarget/paintCanvas). Rebuilt whenever the
    // content box resizes.
    mutable ID2D1BitmapRenderTarget *canvasTarget = nullptr;
    mutable float canvasBuiltForWidth = -1.0f, canvasBuiltForHeight = -1.0f;
#else
    mutable GLuint textTexture = 0;
    mutable int textTexW = 0, textTexH = 0;
    mutable float textTextureBuiltForWidth = -1.0f;
    mutable std::string textTextureBuiltForText;
    // A CPU-side Cairo image surface the size of this canvas node's inner
    // content box, drawn into by onPaint, then converted+uploaded as an
    // RGBA GL texture for actual display (see
    // LiteUI::ensureCanvasSurface/drawCanvasTexture). Keeping the Cairo
    // surface around (rather than freeing it right after upload) is what
    // makes getImageData possible on this platform: it's just a read of
    // the very same pixels already sitting in `canvasSurface`.
    mutable cairo_surface_t *canvasSurface = nullptr;
    mutable GLuint canvasTexture = 0;
    mutable float canvasBuiltForWidth = -1.0f, canvasBuiltForHeight = -1.0f;
#endif
  } computed;

  void addChild(View child) { children.push_back(std::move(child)); }
  void addChild(Text t) { children.push_back(toView(std::move(t))); }
  void addChild(Canvas c) { children.push_back(toView(std::move(c))); }

  // Marks this canvas node's cached backing surface stale, so the next
  // paint/render pass re-invokes onPaint instead of reusing whatever it
  // last drew. A no-op (harmlessly) on a non-canvas node. Since the
  // renderer walks a `const View&`, this is the app's own handle to a
  // View it's still holding a mutable reference to (e.g. right after
  // addChild, or via onLayout capturing a pointer/index) — there's no way
  // to reach into an already-submitted tree from outside.
  void requestCanvasRedraw() { computed.canvasNeedsRedraw = true; }

  // Whether this view clips/scrolls on the given axis.
  bool scrollsX() const { return style.overflowX != Overflow::Visible; }
  bool scrollsY() const { return style.overflowY != Overflow::Visible; }

  // Maximum scrollX/scrollY this view can currently have, given its last
  // computed content size vs its viewport size. 0 when content fits (or
  // the axis doesn't scroll).
  float maxScrollX() const {
    return std::max(0.0f, computed.contentW - computed.w);
  }
  float maxScrollY() const {
    return std::max(0.0f, computed.contentH - computed.h);
  }

  // Frees this node's cached text- and canvas-rendering resources (if
  // any) and recurses into children. Must be called explicitly before a
  // tree is discarded/replaced — see setRoot() and ~LiteUI(), the only
  // two places a live tree is actually retired. (The name predates
  // Canvas support; kept as-is rather than renamed to minimize disruption
  // to any code already calling it.)
  void freeTextResources() {
#if defined(_WIN32)
    if (computed.textLayout) {
      computed.textLayout->Release();
      computed.textLayout = nullptr;
    }
    if (computed.canvasTarget) {
      computed.canvasTarget->Release();
      computed.canvasTarget = nullptr;
    }
#else
    if (computed.textTexture) {
      glDeleteTextures(1, &computed.textTexture);
      computed.textTexture = 0;
    }
    if (computed.canvasTexture) {
      glDeleteTextures(1, &computed.canvasTexture);
      computed.canvasTexture = 0;
    }
    if (computed.canvasSurface) {
      cairo_surface_destroy(computed.canvasSurface);
      computed.canvasSurface = nullptr;
    }
#endif
    for (auto &c : children)
      c.freeTextResources();
  }

private:
  static View toView(Text t);
  static View toView(Canvas c);
};

inline View View::toView(Text t) {
  View v;
  v.style = std::move(t.style);
  v.isText = true;
  v.text = std::move(t.label);
  v.textStyle.fontSize = t.fontSize;
  v.textStyle.fontWeight = t.fontWeight;
  v.textStyle.fontStyle = t.fontStyle;
  v.textStyle.fontFamily = std::move(t.fontFamily);
  v.textStyle.color = t.color;
  v.textStyle.align = t.align;
  v.textStyle.overflow = t.overflow;
  v.textStyle.wrap = t.wrap;
  v.textStyle.lineHeight = t.lineHeight;
  v.textStyle.letterSpacing = t.letterSpacing;
  v.textStyle.maxLines = t.maxLines;
  v.textStyle.underline = t.underline;
  v.textStyle.strikethrough = t.strikethrough;
  return v;
}

inline View View::toView(Canvas c) {
  View v;
  v.style = std::move(c.style);
  v.isCanvas = true;
  v.onPaint = std::move(c.onPaint);
  v.onClick = std::move(c.onClick);
  v.onMiddleClick = std::move(c.onMiddleClick);
  v.onRightClick = std::move(c.onRightClick);
  v.onPressAt = std::move(c.onPressAt);
  v.onMiddlePressAt = std::move(c.onMiddlePressAt);
  v.onRightPressAt = std::move(c.onRightPressAt);
  v.onDragTo = std::move(c.onDragTo);
  v.onMiddleDragTo = std::move(c.onMiddleDragTo);
  v.onRightDragTo = std::move(c.onRightDragTo);
  v.onScrollUp = std::move(c.onScrollUp);
  v.onScrollDown = std::move(c.onScrollDown);
  v.canvasDirtySource = std::move(c.canvasDirtySource);
  return v;
}

// ---------------- Text measurement/layout backend ----------------
// Platform-specific: DirectWrite on Windows, Pango/Cairo on Linux. Only
// liteui_layout::measureNatural (for sizing) and LiteUI's renderers (for
// painting) call into this.
namespace liteui_text {

struct Measurement {
  float width, height;
};

#if defined(_WIN32)

// Lazily-created, process-wide DirectWrite factory. DirectWrite objects
// aren't tied to any particular HWND/render target, so one factory serves
// every LiteUI window in the process.
inline IDWriteFactory *factory() {
  static IDWriteFactory *f = [] {
    IDWriteFactory *p = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&p))))
      throw std::runtime_error("DWriteCreateFactory failed");
    return p;
  }();
  return f;
}

inline DWRITE_FONT_WEIGHT toDWriteWeight(FontWeight w) {
  return static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(w));
}
inline DWRITE_FONT_STYLE toDWriteStyle(FontStyle s) {
  return s == FontStyle::Italic ? DWRITE_FONT_STYLE_ITALIC
                                : DWRITE_FONT_STYLE_NORMAL;
}

// Builds a ready-to-measure-or-draw layout. `availWidth`/`availHeight`
// bound wrapping/trimming — pass a huge value for "unbounded" (used when
// measuring a Fit-width node's intrinsic single-line size). Caller owns
// the returned pointer.
inline IDWriteTextLayout *makeLayout(const std::string &text,
                                     const TextStyle &style, float availWidth,
                                     float availHeight) {
  std::wstring wfam =
      style.fontFamily.empty() ? L"Segoe UI" : toWide(style.fontFamily);
  IDWriteTextFormat *format = nullptr;
  factory()->CreateTextFormat(
      wfam.c_str(), nullptr, toDWriteWeight(style.fontWeight),
      toDWriteStyle(style.fontStyle), DWRITE_FONT_STRETCH_NORMAL,
      style.fontSize, L"", &format);
  if (!format)
    throw std::runtime_error("CreateTextFormat failed");
  format->SetWordWrapping(style.wrap == TextWrap::Wrap
                              ? DWRITE_WORD_WRAPPING_WRAP
                              : DWRITE_WORD_WRAPPING_NO_WRAP);
  switch (style.align) {
  case TextAlign::Center:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    break;
  case TextAlign::End:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    break;
  case TextAlign::Justify:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_JUSTIFIED);
    break;
  default:
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    break;
  }

  std::wstring wtext = toWide(text);
  IDWriteTextLayout *layout = nullptr;
  factory()->CreateTextLayout(wtext.c_str(), static_cast<UINT32>(wtext.size()),
                              format, availWidth, availHeight, &layout);
  format->Release(); // layout holds what it needs internally
  if (!layout)
    throw std::runtime_error("CreateTextLayout failed");

  DWRITE_TEXT_RANGE full{0, static_cast<UINT32>(wtext.size())};
  if (style.letterSpacing != 0) {
    // Character spacing is IDWriteTextLayout1 (Windows 8.1+); skip
    // silently if the interface isn't available rather than failing the
    // whole layout over a cosmetic feature.
    IDWriteTextLayout1 *layout1 = nullptr;
    if (SUCCEEDED(layout->QueryInterface(&layout1)) && layout1) {
      layout1->SetCharacterSpacing(0, style.letterSpacing, 0, full);
      layout1->Release();
    }
  }
  if (style.underline)
    layout->SetUnderline(TRUE, full);
  if (style.strikethrough)
    layout->SetStrikethrough(TRUE, full);
  if (style.overflow == TextOverflow::Ellipsis) {
    IDWriteInlineObject *ellipsis = nullptr;
    factory()->CreateEllipsisTrimmingSign(layout, &ellipsis);
    if (ellipsis) {
      DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      layout->SetTrimming(&trimming, ellipsis);
      ellipsis->Release();
    }
  }
  if (style.maxLines > 0)
    layout->SetMaxHeight(availHeight);
  return layout;
}

// Used only by measureNatural — builds a throwaway layout purely to read
// metrics back. The renderer's own cached layout (built per-paint, see
// LiteUI::ensureTextLayout) is what's actually drawn.
inline Measurement measure(const std::string &text, const TextStyle &style,
                           float availWidth) {
  float w =
      availWidth >= 0 ? availWidth : std::numeric_limits<float>::max() / 4;
  float h = std::numeric_limits<float>::max() / 4;
  IDWriteTextLayout *layout = makeLayout(text, style, w, h);
  DWRITE_TEXT_METRICS m;
  layout->GetMetrics(&m);
  float outH = m.height;
  if (style.lineHeight > 0) {
    UINT32 lineCount = 0;
    layout->GetLineMetrics(nullptr, 0, &lineCount);
    outH = lineCount * style.lineHeight;
  }
  if (style.maxLines > 0 && style.lineHeight > 0)
    outH = std::min(outH, style.maxLines * style.lineHeight);
  Measurement result{m.widthIncludingTrailingWhitespace, outH};
  layout->Release();
  return result;
}

#else // Linux — Pango/Cairo

inline PangoFontDescription *makeFontDescription(const TextStyle &style) {
  PangoFontDescription *desc = pango_font_description_new();
  pango_font_description_set_family(
      desc, style.fontFamily.empty() ? "Sans" : style.fontFamily.c_str());
  pango_font_description_set_weight(
      desc, static_cast<PangoWeight>(static_cast<int>(style.fontWeight)));
  pango_font_description_set_style(desc, style.fontStyle == FontStyle::Italic
                                             ? PANGO_STYLE_ITALIC
                                             : PANGO_STYLE_NORMAL);
  // Absolute pixel size sidesteps a DPI round-trip — everything else in
  // this file (layout, GL) already works in plain screen pixels.
  pango_font_description_set_absolute_size(desc, style.fontSize * PANGO_SCALE);
  return desc;
}

// One shared throwaway cairo context used only to create PangoLayouts for
// measurement — Pango requires *a* context to build a layout on, even one
// that's never painted to.
inline cairo_t *measureCr() {
  static cairo_surface_t *surf =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  static cairo_t *cr = cairo_create(surf);
  return cr;
}

inline PangoLayout *makeLayout(const std::string &text, const TextStyle &style,
                               float availWidth, cairo_t *cr) {
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_text(layout, text.c_str(), -1);
  PangoFontDescription *desc = makeFontDescription(style);
  pango_layout_set_font_description(layout, desc);
  pango_font_description_free(desc);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_width(layout,
                         (style.wrap == TextWrap::Wrap && availWidth >= 0)
                             ? static_cast<int>(availWidth * PANGO_SCALE)
                             : -1);
  if (style.maxLines > 0) {
    pango_layout_set_height(
        layout, -style.maxLines); // Pango idiom: negative = max line count
    pango_layout_set_ellipsize(layout, style.overflow == TextOverflow::Ellipsis
                                           ? PANGO_ELLIPSIZE_END
                                           : PANGO_ELLIPSIZE_NONE);
  } else if (style.overflow == TextOverflow::Ellipsis) {
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  }
  switch (style.align) {
  case TextAlign::Center:
    pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
    break;
  case TextAlign::End:
    pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);
    break;
  case TextAlign::Justify:
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    pango_layout_set_justify(layout, TRUE);
    break;
  default:
    pango_layout_set_alignment(layout, PANGO_ALIGN_LEFT);
    break;
  }
  // Underline/strikethrough/letter-spacing are all plain PangoAttrList
  // entries — collected into one list and applied once.
  if (style.letterSpacing != 0 || style.underline || style.strikethrough) {
    PangoAttrList *attrs = pango_attr_list_new();
    if (style.letterSpacing != 0)
      pango_attr_list_insert(
          attrs, pango_attr_letter_spacing_new(
                     static_cast<int>(style.letterSpacing * PANGO_SCALE)));
    if (style.underline)
      pango_attr_list_insert(attrs,
                             pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
    if (style.strikethrough)
      pango_attr_list_insert(attrs, pango_attr_strikethrough_new(TRUE));
    pango_layout_set_attributes(layout, attrs);
    pango_attr_list_unref(attrs);
  }
  if (style.lineHeight > 0) {
    PangoContext *ctx = pango_layout_get_context(layout);
    PangoFontMetrics *metrics = pango_context_get_metrics(
        ctx, pango_layout_get_font_description(layout), nullptr);
    double natural =
        pango_font_metrics_get_height(metrics) / (double)PANGO_SCALE;
    pango_font_metrics_unref(metrics);
    if (natural > 0)
      pango_layout_set_line_spacing(
          layout, static_cast<float>(style.lineHeight / natural));
  }
  return layout;
}

inline Measurement measure(const std::string &text, const TextStyle &style,
                           float availWidth) {
  PangoLayout *layout = makeLayout(text, style, availWidth, measureCr());
  int w, h;
  pango_layout_get_pixel_size(layout, &w, &h);
  Measurement result{static_cast<float>(w), static_cast<float>(h)};
  g_object_unref(layout);
  return result;
}

#endif
} // namespace liteui_text

// ---------------- Canvas 2D context: Direct2D (Windows) / Cairo (Linux) ----
//
// Two complete, independent definitions of CanvasContext follow (one per
// platform), each exposing the identical public method surface described
// in the Canvas struct's own comment above. App code that only calls the
// public API compiles unchanged on either platform.
#if defined(_WIN32)

class CanvasContext {
public:
  // `rt` is a bitmap render target sized exactly to the canvas node's
  // inner (padding-excluded) content box, already BeginDraw()'d and
  // cleared to transparent by the caller (see LiteUI::ensureCanvasTarget).
  // `width`/`height` mirror that box's size so the app can query it back
  // via width()/height() without hand-computing it.
  CanvasContext(ID2D1RenderTarget *rt, float width, float height)
      : rt_(rt), width_(width), height_(height) {
    rt_->GetFactory(&factory_);
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
  }
  ~CanvasContext() {
    // Leave the render target's layer stack balanced even if onPaint
    // forgot to restore() every save()/clip() — an imbalanced PushLayer
    // would corrupt every subsequent frame drawn into this same target.
    while (!clipLayers_.empty()) {
      rt_->PopLayer();
      clipLayers_.back()->Release();
      clipLayers_.pop_back();
    }
    if (factory_)
      factory_->Release();
  }
  CanvasContext(const CanvasContext &) = delete;
  CanvasContext &operator=(const CanvasContext &) = delete;

  float width() const { return width_; }
  float height() const { return height_; }

  // ---- state stack ----
  void save() {
    State s;
    s.transform = transform_;
    s.fill = fillPaint_;
    s.stroke = strokePaint_;
    s.lineWidth = lineWidth_;
    s.lineCap = lineCap_;
    s.lineJoin = lineJoin_;
    s.miterLimit = miterLimit_;
    s.dashes = dashes_;
    s.dashOffset = dashOffset_;
    s.globalAlpha = globalAlpha_;
    s.composite = composite_;
    s.shadowColor = shadowColor_;
    s.shadowOffsetX = shadowOffsetX_;
    s.shadowOffsetY = shadowOffsetY_;
    s.font = font_;
    s.textAlign = textAlign_;
    s.textBaseline = textBaseline_;
    s.clipDepth = clipLayers_.size();
    stack_.push_back(std::move(s));
  }
  void restore() {
    if (stack_.empty())
      return;
    State s = std::move(stack_.back());
    stack_.pop_back();
    while (clipLayers_.size() > s.clipDepth) {
      rt_->PopLayer();
      clipLayers_.back()->Release();
      clipLayers_.pop_back();
    }
    transform_ = s.transform;
    fillPaint_ = s.fill;
    strokePaint_ = s.stroke;
    lineWidth_ = s.lineWidth;
    lineCap_ = s.lineCap;
    lineJoin_ = s.lineJoin;
    miterLimit_ = s.miterLimit;
    dashes_ = s.dashes;
    dashOffset_ = s.dashOffset;
    globalAlpha_ = s.globalAlpha;
    composite_ = s.composite;
    shadowColor_ = s.shadowColor;
    shadowOffsetX_ = s.shadowOffsetX;
    shadowOffsetY_ = s.shadowOffsetY;
    font_ = s.font;
    textAlign_ = s.textAlign;
    textBaseline_ = s.textBaseline;
  }

  // ---- transform ----
  void translate(float x, float y) {
    transform_ = D2D1::Matrix3x2F::Translation(x, y) * transform_;
  }
  void rotate(float radians) {
    transform_ =
        D2D1::Matrix3x2F::Rotation(radians * 180.0f / 3.14159265358979f) *
        transform_;
  }
  void scale(float sx, float sy) {
    transform_ = D2D1::Matrix3x2F::Scale(sx, sy) * transform_;
  }
  void transformBy(float a, float b, float c, float d, float e, float f) {
    transform_ = D2D1::Matrix3x2F(a, b, c, d, e, f) * transform_;
  }
  void setTransform(float a, float b, float c, float d, float e, float f) {
    transform_ = D2D1::Matrix3x2F(a, b, c, d, e, f);
  }
  void resetTransform() { transform_ = D2D1::Matrix3x2F::Identity(); }

  // ---- paint/style ----
  void setFillColor(Color c) { fillPaint_ = Paint::solid(c); }
  void setFillGradient(const CanvasGradient &g) { fillPaint_ = Paint::grad(g); }
  void setStrokeColor(Color c) { strokePaint_ = Paint::solid(c); }
  void setStrokeGradient(const CanvasGradient &g) {
    strokePaint_ = Paint::grad(g);
  }
  void setLineWidth(float w) { lineWidth_ = std::max(0.0f, w); }
  void setLineCap(LineCap c) { lineCap_ = c; }
  void setLineJoin(LineJoin j) { lineJoin_ = j; }
  void setMiterLimit(float m) { miterLimit_ = m; }
  void setLineDash(const std::vector<float> &dashes) { dashes_ = dashes; }
  void setLineDashOffset(float offset) { dashOffset_ = offset; }
  void setGlobalAlpha(float a) { globalAlpha_ = std::clamp(a, 0.0f, 1.0f); }
  // See the Canvas limitations note: Windows always draws with normal
  // (source-over) alpha blending regardless of what's set here — a plain
  // ID2D1RenderTarget (v1) has no Porter-Duff blend control, unlike
  // Cairo on Linux, which honors the full operator range.
  void setGlobalCompositeOperation(CompositeOp op) { composite_ = op; }
  void setShadow(Color color, float /*blurPx, unsupported: unblurred*/,
                 float offsetX, float offsetY) {
    shadowColor_ = color;
    shadowOffsetX_ = offsetX;
    shadowOffsetY_ = offsetY;
  }
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal) {
    font_.fontFamily = family;
    font_.fontSize = sizePx;
    font_.fontWeight = weight;
    font_.fontStyle = style;
  }
  void setTextAlign(TextAlign a) { textAlign_ = a; }
  void setTextBaseline(TextBaseline b) { textBaseline_ = b; }

  // ---- path building ----
  // Mirrors HTML canvas path semantics: fill()/stroke()/clip() below do
  // NOT clear the current path (only beginPath() does), so the same
  // shape can be filled then stroked, or extended with more segments,
  // across multiple calls.
  void beginPath() {
    segs_.clear();
    figureClosed_.clear();
    curX_ = curY_ = startX_ = startY_ = 0;
  }
  void closePath() {
    if (!figureClosed_.empty())
      figureClosed_.back() = true;
    curX_ = startX_;
    curY_ = startY_;
  }
  void moveTo(float x, float y) {
    segs_.push_back(Seg::move(x, y));
    figureClosed_.push_back(false);
    curX_ = startX_ = x;
    curY_ = startY_ = y;
  }
  void lineTo(float x, float y) {
    ensureFigure();
    segs_.push_back(Seg::line(x, y));
    curX_ = x;
    curY_ = y;
  }
  void quadraticCurveTo(float cpx, float cpy, float x, float y) {
    ensureFigure();
    float c1x = curX_ + 2.0f / 3.0f * (cpx - curX_);
    float c1y = curY_ + 2.0f / 3.0f * (cpy - curY_);
    float c2x = x + 2.0f / 3.0f * (cpx - x);
    float c2y = y + 2.0f / 3.0f * (cpy - y);
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
    ensureFigure();
    segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, x, y));
    curX_ = x;
    curY_ = y;
  }
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false) {
    appendArc(cx, cy, r, r, 0, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false) {
    appendArc(cx, cy, rx, ry, rotation, startAngle, endAngle, ccw,
              figureClosed_.empty());
  }
  void arcTo(float x1, float y1, float x2, float y2, float radius) {
    float x0 = curX_, y0 = curY_;
    float dx1 = x0 - x1, dy1 = y0 - y1;
    float dx2 = x2 - x1, dy2 = y2 - y1;
    float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
    if (len1 < 1e-6f || len2 < 1e-6f || radius <= 0) {
      lineTo(x1, y1);
      return;
    }
    dx1 /= len1;
    dy1 /= len1;
    dx2 /= len2;
    dy2 /= len2;
    float cosA = std::clamp(dx1 * dx2 + dy1 * dy2, -1.0f, 1.0f);
    float angle = std::acos(cosA);
    if (angle < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    float dist = radius / std::tan(angle / 2.0f);
    float t1x = x1 + dx1 * dist, t1y = y1 + dy1 * dist;
    float t2x = x1 + dx2 * dist, t2y = y1 + dy2 * dist;
    lineTo(t1x, t1y);
    float bisx = dx1 + dx2, bisy = dy1 + dy2;
    float bisLen = std::sqrt(bisx * bisx + bisy * bisy);
    if (bisLen < 1e-6f) {
      lineTo(x1, y1);
      return;
    }
    bisx /= bisLen;
    bisy /= bisLen;
    float centerDist = std::sqrt(radius * radius + dist * dist);
    float ccx = x1 + bisx * centerDist, ccy = y1 + bisy * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx);
    float a1 = std::atan2(t2y - ccy, t2x - ccx);
    float cross = dx1 * dy2 - dy1 * dx2; // winding sign, picks short way
    appendArc(ccx, ccy, radius, radius, 0, a0, a1, cross > 0, false);
  }
  void rect(float x, float y, float w, float h) {
    moveTo(x, y);
    lineTo(x + w, y);
    lineTo(x + w, y + h);
    lineTo(x, y + h);
    closePath();
  }
  void roundRect(float x, float y, float w, float h, float radius) {
    constexpr float kPi = 3.14159265358979f;
    radius = std::max(0.0f, std::min(radius, std::min(w, h) / 2.0f));
    moveTo(x + radius, y);
    lineTo(x + w - radius, y);
    appendArc(x + w - radius, y + radius, radius, radius, 0, -kPi / 2, 0, false,
              false);
    lineTo(x + w, y + h - radius);
    appendArc(x + w - radius, y + h - radius, radius, radius, 0, 0, kPi / 2,
              false, false);
    lineTo(x + radius, y + h);
    appendArc(x + radius, y + h - radius, radius, radius, 0, kPi / 2, kPi,
              false, false);
    lineTo(x, y + radius);
    appendArc(x + radius, y + radius, radius, radius, 0, kPi, kPi * 1.5f, false,
              false);
    closePath();
  }

  // ---- drawing ----
  void fill(FillRule rule = FillRule::NonZero) {
    ID2D1PathGeometry *geom = buildGeometry(toFillMode(rule));
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    drawShadowThenReal([&](ID2D1Brush *b) { rt_->FillGeometry(geom, b); },
                       fillPaint_);
    geom->Release();
  }
  void stroke() {
    ID2D1PathGeometry *geom = buildGeometry(D2D1_FILL_MODE_WINDING);
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    ID2D1StrokeStyle *strokeStyle = makeStrokeStyle();
    drawShadowThenReal(
        [&](ID2D1Brush *b) {
          rt_->DrawGeometry(geom, b, lineWidth_, strokeStyle);
        },
        strokePaint_);
    if (strokeStyle)
      strokeStyle->Release();
    geom->Release();
  }
  void clip(FillRule rule = FillRule::NonZero) {
    ID2D1PathGeometry *geom = buildGeometry(toFillMode(rule));
    if (!geom)
      return;
    rt_->SetTransform(transform_);
    ID2D1Layer *layer = nullptr;
    rt_->CreateLayer(nullptr, &layer);
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geom), layer);
    clipLayers_.push_back(layer);
    geom->Release();
  }
  void fillRect(float x, float y, float w, float h) {
    std::vector<Seg> savedSegs = segs_;
    std::vector<bool> savedClosed = figureClosed_;
    float savedX = curX_, savedY = curY_, savedSX = startX_, savedSY = startY_;
    beginPath();
    rect(x, y, w, h);
    fill();
    segs_ = std::move(savedSegs);
    figureClosed_ = std::move(savedClosed);
    curX_ = savedX;
    curY_ = savedY;
    startX_ = savedSX;
    startY_ = savedSY;
  }
  void strokeRect(float x, float y, float w, float h) {
    std::vector<Seg> savedSegs = segs_;
    std::vector<bool> savedClosed = figureClosed_;
    float savedX = curX_, savedY = curY_, savedSX = startX_, savedSY = startY_;
    beginPath();
    rect(x, y, w, h);
    stroke();
    segs_ = std::move(savedSegs);
    figureClosed_ = std::move(savedClosed);
    curX_ = savedX;
    curY_ = savedY;
    startX_ = savedSX;
    startY_ = savedSY;
  }
  // Direct2D (v1) render targets have no partial-area clear — Clear()
  // always wipes the ENTIRE target regardless of clip/transform, so this
  // clears the whole canvas rather than just (x,y,w,h) (Linux's Cairo
  // backend gives a true sub-rectangle clear — see the Canvas limitations
  // note at the top of this section).
  void clearRect(float, float, float, float) {
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
  }

  // ---- text ----
  struct TextMetricsResult {
    float width;
  };
  TextMetricsResult measureText(const std::string &text) {
    TextStyle ts = fontToTextStyle();
    liteui_text::Measurement m = liteui_text::measure(text, ts, -1);
    return {m.width};
  }
  void fillText(const std::string &text, float x, float y,
                float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, false);
  }
  void strokeText(const std::string &text, float x, float y,
                  float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, true);
  }

  // ---- images ----
  void drawImage(const CanvasImage &img, float dx, float dy) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy,
              static_cast<float>(img.width), static_cast<float>(img.height));
  }
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy, dw, dh);
  }
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh) {
    if (img.width <= 0 || img.height <= 0)
      return;
    ID2D1Bitmap *bmp = makeBitmap(img);
    if (!bmp)
      return;
    rt_->SetTransform(transform_);
    D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + dw, dy + dh);
    D2D1_RECT_F src = D2D1::RectF(sx, sy, sx + sw, sy + sh);
    rt_->DrawBitmap(bmp, dst, globalAlpha_,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
    bmp->Release();
  }

  // ---- pixel data ----
  // Not supported on Windows in this build — see the Canvas limitations
  // note. Always returns std::nullopt here; fully supported on Linux.
  std::optional<CanvasImage> getImageData(float, float, float, float) {
    return std::nullopt;
  }
  // putImageData ignores the current transform and paints the pixels
  // directly at (dx,dy), matching HTML canvas semantics.
  void putImageData(const CanvasImage &img, float dx, float dy) {
    if (img.width <= 0 || img.height <= 0)
      return;
    ID2D1Bitmap *bmp = makeBitmap(img);
    if (!bmp)
      return;
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    D2D1_RECT_F dst = D2D1::RectF(dx, dy, dx + static_cast<float>(img.width),
                                  dy + static_cast<float>(img.height));
    rt_->DrawBitmap(bmp, dst, 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    bmp->Release();
  }

private:
  // ---- internal path representation ----
  struct Seg {
    enum class Kind { Move, Line, Cubic } kind;
    float x, y;               // endpoint (all kinds)
    float c1x, c1y, c2x, c2y; // control points (Cubic only)
    static Seg move(float x, float y) { return {Kind::Move, x, y, 0, 0, 0, 0}; }
    static Seg line(float x, float y) { return {Kind::Line, x, y, 0, 0, 0, 0}; }
    static Seg cubic(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
      return {Kind::Cubic, x, y, c1x, c1y, c2x, c2y};
    }
  };

  struct Paint {
    bool isGradient = false;
    Color color{0, 0, 0, 255};
    CanvasGradient gradient;
    static Paint solid(Color c) {
      Paint p;
      p.isGradient = false;
      p.color = c;
      return p;
    }
    static Paint grad(const CanvasGradient &g) {
      Paint p;
      p.isGradient = true;
      p.gradient = g;
      return p;
    }
  };

  struct State {
    D2D1_MATRIX_3X2_F transform;
    Paint fill, stroke;
    float lineWidth;
    LineCap lineCap;
    LineJoin lineJoin;
    float miterLimit;
    std::vector<float> dashes;
    float dashOffset;
    float globalAlpha;
    CompositeOp composite;
    Color shadowColor;
    float shadowOffsetX, shadowOffsetY;
    TextStyle font;
    TextAlign textAlign;
    TextBaseline textBaseline;
    size_t clipDepth;
  };

  static Color colorOf(const Paint &p) {
    if (!p.isGradient)
      return p.color;
    return p.gradient.stops.empty() ? Color{0, 0, 0, 255}
                                    : p.gradient.stops.front().color;
  }

  void ensureFigure() {
    if (figureClosed_.empty())
      moveTo(curX_, curY_);
  }

  // Splits [startAngle, endAngle) into <=90-degree pieces and appends
  // each as a cubic Bezier via the standard circular-arc kappa
  // approximation, applied in a unit circle then mapped through
  // rotation+non-uniform-scale+translate — this is what lets the same
  // helper serve arc(), ellipse(), arcTo(), and roundRect()'s corners.
  void appendArc(float cx, float cy, float rx, float ry, float rotation,
                 float a0, float a1, bool ccw, bool asMove) {
    constexpr float kTwoPi = 6.28318530717959f;
    float delta = a1 - a0;
    if (!ccw) {
      while (delta < 0)
        delta += kTwoPi;
    } else {
      while (delta > 0)
        delta -= kTwoPi;
    }
    float cosR = std::cos(rotation), sinR = std::sin(rotation);
    auto mapPt = [&](float ux, float uy, float &px, float &py) {
      float ex = rx * ux, ey = ry * uy;
      px = cx + ex * cosR - ey * sinR;
      py = cy + ex * sinR + ey * cosR;
    };
    float p0x, p0y;
    mapPt(std::cos(a0), std::sin(a0), p0x, p0y);
    if (asMove)
      moveTo(p0x, p0y);
    else
      lineTo(p0x, p0y);
    if (std::abs(delta) < 1e-6f)
      return;
    int segCount = std::max(
        1, static_cast<int>(std::ceil(std::abs(delta) / (3.14159265f / 2))));
    float segAngle = delta / segCount;
    float ang = a0;
    for (int i = 0; i < segCount; ++i) {
      float ang1 = ang + segAngle;
      float t = std::tan((ang1 - ang) / 4.0f);
      float alpha =
          std::sin(ang1 - ang) * (std::sqrt(4.0f + 3.0f * t * t) - 1.0f) / 3.0f;
      float e0x = std::cos(ang), e0y = std::sin(ang);
      float e1x = std::cos(ang1), e1y = std::sin(ang1);
      float d0x = -e0y, d0y = e0x;
      float d1x = -e1y, d1y = e1x;
      float c1ex = e0x + alpha * d0x, c1ey = e0y + alpha * d0y;
      float c2ex = e1x - alpha * d1x, c2ey = e1y - alpha * d1y;
      float c1x, c1y, c2x, c2y, ex1, ey1;
      mapPt(c1ex, c1ey, c1x, c1y);
      mapPt(c2ex, c2ey, c2x, c2y);
      mapPt(e1x, e1y, ex1, ey1);
      segs_.push_back(Seg::cubic(c1x, c1y, c2x, c2y, ex1, ey1));
      curX_ = ex1;
      curY_ = ey1;
      ang = ang1;
    }
  }

  static D2D1_FILL_MODE toFillMode(FillRule rule) {
    return rule == FillRule::EvenOdd ? D2D1_FILL_MODE_ALTERNATE
                                     : D2D1_FILL_MODE_WINDING;
  }

  ID2D1PathGeometry *buildGeometry(D2D1_FILL_MODE fillMode) {
    if (segs_.empty())
      return nullptr;
    ID2D1PathGeometry *geom = nullptr;
    factory_->CreatePathGeometry(&geom);
    if (!geom)
      return nullptr;
    ID2D1GeometrySink *sink = nullptr;
    geom->Open(&sink);
    sink->SetFillMode(fillMode);
    bool figureOpen = false;
    size_t figIdx = 0;
    bool first = true;
    for (auto &s : segs_) {
      if (s.kind == Seg::Kind::Move) {
        if (figureOpen)
          sink->EndFigure(figureClosed_[figIdx] ? D2D1_FIGURE_END_CLOSED
                                                : D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(D2D1::Point2F(s.x, s.y), D2D1_FIGURE_BEGIN_FILLED);
        figureOpen = true;
        figIdx = first ? 0 : figIdx + 1;
        first = false;
      } else if (s.kind == Seg::Kind::Line) {
        sink->AddLine(D2D1::Point2F(s.x, s.y));
      } else {
        sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(s.c1x, s.c1y),
                                            D2D1::Point2F(s.c2x, s.c2y),
                                            D2D1::Point2F(s.x, s.y)));
      }
    }
    if (figureOpen)
      sink->EndFigure(figureClosed_[figIdx] ? D2D1_FIGURE_END_CLOSED
                                            : D2D1_FIGURE_END_OPEN);
    sink->Close();
    sink->Release();
    return geom;
  }

  ID2D1Brush *makeBrush(const Paint &p) {
    if (!p.isGradient) {
      ID2D1SolidColorBrush *b = nullptr;
      Color c = p.color;
      rt_->CreateSolidColorBrush(D2D1::ColorF(c.r / 255.0f, c.g / 255.0f,
                                              c.b / 255.0f,
                                              (c.a / 255.0f) * globalAlpha_),
                                 &b);
      return b;
    }
    if (p.gradient.stops.empty())
      return nullptr;
    std::vector<D2D1_GRADIENT_STOP> stops;
    for (auto &s : p.gradient.stops) {
      D2D1_GRADIENT_STOP gs;
      gs.position = std::clamp(s.offset, 0.0f, 1.0f);
      gs.color =
          D2D1::ColorF(s.color.r / 255.0f, s.color.g / 255.0f,
                       s.color.b / 255.0f, (s.color.a / 255.0f) * globalAlpha_);
      stops.push_back(gs);
    }
    if (p.gradient.kind == CanvasGradient::Kind::Radial) {
      // D2D1's radial brush models a single circle plus a focal offset
      // point, unlike canvas's two-circle (r0..r1) model — approximate
      // by rescaling stop offsets so t=0 lands at r0/r1 of the way into
      // the D2D circle (see Canvas limitations note).
      float r1 = std::max(1.0f, p.gradient.r1);
      float innerFrac = std::clamp(p.gradient.r0 / r1, 0.0f, 0.99f);
      for (auto &gs : stops)
        gs.position = innerFrac + gs.position * (1.0f - innerFrac);
    }
    ID2D1GradientStopCollection *coll = nullptr;
    rt_->CreateGradientStopCollection(stops.data(),
                                      static_cast<UINT32>(stops.size()), &coll);
    if (!coll)
      return nullptr;
    ID2D1Brush *brush = nullptr;
    if (p.gradient.kind == CanvasGradient::Kind::Linear) {
      ID2D1LinearGradientBrush *lb = nullptr;
      rt_->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(
              D2D1::Point2F(p.gradient.x0, p.gradient.y0),
              D2D1::Point2F(p.gradient.x1, p.gradient.y1)),
          coll, &lb);
      brush = lb;
    } else {
      float r1 = std::max(1.0f, p.gradient.r1);
      float ox =
          std::clamp(p.gradient.x0 - p.gradient.x1, -r1 * 0.99f, r1 * 0.99f);
      float oy =
          std::clamp(p.gradient.y0 - p.gradient.y1, -r1 * 0.99f, r1 * 0.99f);
      ID2D1RadialGradientBrush *rb = nullptr;
      rt_->CreateRadialGradientBrush(
          D2D1::RadialGradientBrushProperties(
              D2D1::Point2F(p.gradient.x1, p.gradient.y1),
              D2D1::Point2F(ox, oy), r1, r1),
          coll, &rb);
      brush = rb;
    }
    coll->Release();
    return brush;
  }

  template <class Fn> void drawShadowThenReal(Fn drawFn, const Paint &paint) {
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      D2D1_MATRIX_3X2_F saved = transform_;
      rt_->SetTransform(
          D2D1::Matrix3x2F::Translation(shadowOffsetX_, shadowOffsetY_) *
          transform_);
      ID2D1Brush *shadowBrush = makeBrush(Paint::solid(shadowColor_));
      if (shadowBrush) {
        drawFn(shadowBrush);
        shadowBrush->Release();
      }
      rt_->SetTransform(saved);
    }
    ID2D1Brush *brush = makeBrush(paint);
    if (brush) {
      drawFn(brush);
      brush->Release();
    }
  }

  ID2D1StrokeStyle *makeStrokeStyle() {
    D2D1_CAP_STYLE cap = lineCap_ == LineCap::Round    ? D2D1_CAP_STYLE_ROUND
                         : lineCap_ == LineCap::Square ? D2D1_CAP_STYLE_SQUARE
                                                       : D2D1_CAP_STYLE_FLAT;
    D2D1_LINE_JOIN join = lineJoin_ == LineJoin::Round   ? D2D1_LINE_JOIN_ROUND
                          : lineJoin_ == LineJoin::Bevel ? D2D1_LINE_JOIN_BEVEL
                                                         : D2D1_LINE_JOIN_MITER;
    // D2D interprets dash array/offset values as multiples of stroke
    // width, whereas canvas's setLineDash/lineDashOffset are absolute
    // pixels — rescale to match.
    float lw = std::max(0.0001f, lineWidth_);
    std::vector<float> scaled;
    scaled.reserve(dashes_.size());
    for (float d : dashes_)
      scaled.push_back(d / lw);
    D2D1_DASH_STYLE dashStyle =
        scaled.empty() ? D2D1_DASH_STYLE_SOLID : D2D1_DASH_STYLE_CUSTOM;
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        cap, cap, cap, join, miterLimit_, dashStyle, dashOffset_ / lw);
    ID2D1StrokeStyle *style = nullptr;
    factory_->CreateStrokeStyle(props, scaled.empty() ? nullptr : scaled.data(),
                                static_cast<UINT32>(scaled.size()), &style);
    return style;
  }

  ID2D1Bitmap *makeBitmap(const CanvasImage &img) {
    ID2D1Bitmap *bmp = nullptr;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(
        DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_STRAIGHT));
    rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(img.width),
                                  static_cast<UINT32>(img.height)),
                      img.pixels.data(), static_cast<UINT32>(img.width) * 4,
                      props, &bmp);
    return bmp;
  }

  TextStyle fontToTextStyle() const {
    TextStyle ts = font_;
    ts.wrap = TextWrap::NoWrap;
    return ts;
  }

  // maxWidth is accepted for API familiarity with HTML canvas but not
  // currently applied (no horizontal squeeze-to-fit is performed).
  void drawTextImpl(const std::string &text, float x, float y, float,
                    bool stroked) {
    TextStyle ts = fontToTextStyle();
    IDWriteTextLayout *layout =
        liteui_text::makeLayout(text, ts, 1.0e6f, 1.0e6f);
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    float drawX = x, drawY = y;
    switch (textAlign_) {
    case TextAlign::Center:
      drawX -= m.widthIncludingTrailingWhitespace / 2.0f;
      break;
    case TextAlign::End:
      drawX -= m.widthIncludingTrailingWhitespace;
      break;
    default:
      break;
    }
    switch (textBaseline_) {
    case TextBaseline::Top:
      break;
    case TextBaseline::Middle:
      drawY -= m.height / 2.0f;
      break;
    case TextBaseline::Bottom:
      drawY -= m.height;
      break;
    case TextBaseline::Alphabetic:
    default:
      // Approximate ascent as 80% of line height (DirectWrite line
      // metrics would give an exact figure; kept simple here).
      drawY -= m.height * 0.8f;
      break;
    }
    rt_->SetTransform(transform_);
    auto paintOnce = [&](Color c) {
      ID2D1SolidColorBrush *brush = nullptr;
      rt_->CreateSolidColorBrush(D2D1::ColorF(c.r / 255.0f, c.g / 255.0f,
                                              c.b / 255.0f,
                                              (c.a / 255.0f) * globalAlpha_),
                                 &brush);
      if (brush) {
        rt_->DrawTextLayout(D2D1::Point2F(drawX, drawY), layout, brush);
        brush->Release();
      }
    };
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      D2D1_MATRIX_3X2_F saved = transform_;
      rt_->SetTransform(
          D2D1::Matrix3x2F::Translation(shadowOffsetX_, shadowOffsetY_) *
          transform_);
      paintOnce(shadowColor_);
      rt_->SetTransform(saved);
    }
    if (!stroked) {
      paintOnce(colorOf(fillPaint_));
    } else {
      // Approximates a stroked glyph outline by drawing the fill text in
      // the stroke color, nudged across a ring of directions — not a
      // true outline of the glyph contours (see Canvas limitations note).
      Color base = colorOf(strokePaint_);
      float r = std::max(1.0f, lineWidth_ / 2.0f);
      static const float offs[8][2] = {
          {1, 0},       {-1, 0},       {0, 1},        {0, -1},
          {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
      for (auto &o : offs) {
        D2D1_MATRIX_3X2_F saved = transform_;
        rt_->SetTransform(D2D1::Matrix3x2F::Translation(o[0] * r, o[1] * r) *
                          transform_);
        paintOnce(base);
        rt_->SetTransform(saved);
      }
    }
    layout->Release();
  }

  ID2D1RenderTarget *rt_;
  ID2D1Factory *factory_ = nullptr;
  float width_, height_;

  D2D1_MATRIX_3X2_F transform_ = D2D1::Matrix3x2F::Identity();
  Paint fillPaint_ = Paint::solid(Color{0, 0, 0, 255});
  Paint strokePaint_ = Paint::solid(Color{0, 0, 0, 255});
  float lineWidth_ = 1.0f;
  LineCap lineCap_ = LineCap::Butt;
  LineJoin lineJoin_ = LineJoin::Miter;
  float miterLimit_ = 10.0f;
  std::vector<float> dashes_;
  float dashOffset_ = 0.0f;
  float globalAlpha_ = 1.0f;
  CompositeOp composite_ = CompositeOp::SourceOver;
  Color shadowColor_{0, 0, 0, 0};
  float shadowOffsetX_ = 0.0f, shadowOffsetY_ = 0.0f;
  TextStyle font_;
  TextAlign textAlign_ = TextAlign::Start;
  TextBaseline textBaseline_ = TextBaseline::Alphabetic;

  std::vector<Seg> segs_;
  std::vector<bool> figureClosed_; // one entry per Move, parallel order
  float curX_ = 0, curY_ = 0, startX_ = 0, startY_ = 0;

  std::vector<ID2D1Layer *> clipLayers_;
  std::vector<State> stack_;
};

#else // Linux — Cairo

class CanvasContext {
public:
  // `cr` is a Cairo context targeting an image surface sized exactly to
  // the canvas node's inner (padding-excluded) content box, already
  // cleared to transparent by the caller (see LiteUI::ensureCanvasSurface).
  CanvasContext(cairo_t *cr, float width, float height)
      : cr_(cr), width_(width), height_(height) {}
  ~CanvasContext() = default;
  CanvasContext(const CanvasContext &) = delete;
  CanvasContext &operator=(const CanvasContext &) = delete;

  float width() const { return width_; }
  float height() const { return height_; }

  // ---- state stack ----
  // Cairo's own save/restore already covers the CTM, clip, line width/
  // cap/join/miter/dash — only the fields HTML canvas tracks that Cairo
  // has no native slot for (fill vs. stroke paint, globalAlpha, font,
  // text align/baseline, shadow) need to be mirrored by hand here.
  void save() {
    cairo_save(cr_);
    ExtraState s;
    s.fill = fillPaint_;
    s.stroke = strokePaint_;
    s.globalAlpha = globalAlpha_;
    s.font = font_;
    s.textAlign = textAlign_;
    s.textBaseline = textBaseline_;
    s.shadowColor = shadowColor_;
    s.shadowOffsetX = shadowOffsetX_;
    s.shadowOffsetY = shadowOffsetY_;
    extraStack_.push_back(std::move(s));
  }
  void restore() {
    cairo_restore(cr_);
    if (extraStack_.empty())
      return;
    ExtraState s = std::move(extraStack_.back());
    extraStack_.pop_back();
    fillPaint_ = s.fill;
    strokePaint_ = s.stroke;
    globalAlpha_ = s.globalAlpha;
    font_ = s.font;
    textAlign_ = s.textAlign;
    textBaseline_ = s.textBaseline;
    shadowColor_ = s.shadowColor;
    shadowOffsetX_ = s.shadowOffsetX;
    shadowOffsetY_ = s.shadowOffsetY;
  }

  // ---- transform ----
  void translate(float x, float y) { cairo_translate(cr_, x, y); }
  void rotate(float radians) { cairo_rotate(cr_, radians); }
  void scale(float sx, float sy) { cairo_scale(cr_, sx, sy); }
  void transformBy(float a, float b, float c, float d, float e, float f) {
    cairo_matrix_t m;
    m.xx = a;
    m.yx = b;
    m.xy = c;
    m.yy = d;
    m.x0 = e;
    m.y0 = f;
    cairo_transform(cr_, &m);
  }
  void setTransform(float a, float b, float c, float d, float e, float f) {
    cairo_matrix_t m;
    m.xx = a;
    m.yx = b;
    m.xy = c;
    m.yy = d;
    m.x0 = e;
    m.y0 = f;
    cairo_set_matrix(cr_, &m);
  }
  void resetTransform() { cairo_identity_matrix(cr_); }

  // ---- paint/style ----
  void setFillColor(Color c) { fillPaint_ = Paint::solid(c); }
  void setFillGradient(const CanvasGradient &g) { fillPaint_ = Paint::grad(g); }
  void setStrokeColor(Color c) { strokePaint_ = Paint::solid(c); }
  void setStrokeGradient(const CanvasGradient &g) {
    strokePaint_ = Paint::grad(g);
  }
  void setLineWidth(float w) { cairo_set_line_width(cr_, std::max(0.0f, w)); }
  void setLineCap(LineCap c) {
    cairo_set_line_cap(cr_, c == LineCap::Round    ? CAIRO_LINE_CAP_ROUND
                            : c == LineCap::Square ? CAIRO_LINE_CAP_SQUARE
                                                   : CAIRO_LINE_CAP_BUTT);
  }
  void setLineJoin(LineJoin j) {
    cairo_set_line_join(cr_, j == LineJoin::Round   ? CAIRO_LINE_JOIN_ROUND
                             : j == LineJoin::Bevel ? CAIRO_LINE_JOIN_BEVEL
                                                    : CAIRO_LINE_JOIN_MITER);
  }
  void setMiterLimit(float m) { cairo_set_miter_limit(cr_, m); }
  void setLineDash(const std::vector<float> &dashes) {
    std::vector<double> d(dashes.begin(), dashes.end());
    cairo_set_dash(cr_, d.empty() ? nullptr : d.data(),
                   static_cast<int>(d.size()), dashOffset_);
  }
  void setLineDashOffset(float offset) {
    dashOffset_ = offset;
    int n = cairo_get_dash_count(cr_);
    std::vector<double> d(static_cast<size_t>(std::max(0, n)));
    if (n > 0)
      cairo_get_dash(cr_, d.data(), nullptr);
    cairo_set_dash(cr_, n > 0 ? d.data() : nullptr, n, offset);
  }
  void setGlobalAlpha(float a) { globalAlpha_ = std::clamp(a, 0.0f, 1.0f); }
  void setGlobalCompositeOperation(CompositeOp op) {
    cairo_set_operator(cr_, toCairoOp(op));
  }
  void setShadow(Color color, float /*blurPx, unsupported: unblurred*/,
                 float offsetX, float offsetY) {
    shadowColor_ = color;
    shadowOffsetX_ = offsetX;
    shadowOffsetY_ = offsetY;
  }
  void setFont(const std::string &family, float sizePx,
               FontWeight weight = FontWeight::Regular,
               FontStyle style = FontStyle::Normal) {
    font_.fontFamily = family;
    font_.fontSize = sizePx;
    font_.fontWeight = weight;
    font_.fontStyle = style;
  }
  void setTextAlign(TextAlign a) { textAlign_ = a; }
  void setTextBaseline(TextBaseline b) { textBaseline_ = b; }

  // ---- path building ----
  void beginPath() { cairo_new_path(cr_); }
  void closePath() { cairo_close_path(cr_); }
  void moveTo(float x, float y) { cairo_move_to(cr_, x, y); }
  void lineTo(float x, float y) { cairo_line_to(cr_, x, y); }
  void quadraticCurveTo(float cpx, float cpy, float x, float y) {
    double cx0 = 0, cy0 = 0;
    if (cairo_has_current_point(cr_))
      cairo_get_current_point(cr_, &cx0, &cy0);
    float c1x =
        static_cast<float>(cx0) + 2.0f / 3.0f * (cpx - static_cast<float>(cx0));
    float c1y =
        static_cast<float>(cy0) + 2.0f / 3.0f * (cpy - static_cast<float>(cy0));
    float c2x = x + 2.0f / 3.0f * (cpx - x);
    float c2y = y + 2.0f / 3.0f * (cpy - y);
    cairo_curve_to(cr_, c1x, c1y, c2x, c2y, x, y);
  }
  void bezierCurveTo(float c1x, float c1y, float c2x, float c2y, float x,
                     float y) {
    cairo_curve_to(cr_, c1x, c1y, c2x, c2y, x, y);
  }
  void arc(float cx, float cy, float r, float startAngle, float endAngle,
           bool ccw = false) {
    if (ccw)
      cairo_arc_negative(cr_, cx, cy, r, startAngle, endAngle);
    else
      cairo_arc(cr_, cx, cy, r, startAngle, endAngle);
  }
  void arcTo(float x1, float y1, float x2, float y2, float radius) {
    double x0 = 0, y0 = 0;
    if (cairo_has_current_point(cr_))
      cairo_get_current_point(cr_, &x0, &y0);
    float dx1 = static_cast<float>(x0) - x1, dy1 = static_cast<float>(y0) - y1;
    float dx2 = x2 - x1, dy2 = y2 - y1;
    float len1 = std::sqrt(dx1 * dx1 + dy1 * dy1);
    float len2 = std::sqrt(dx2 * dx2 + dy2 * dy2);
    if (len1 < 1e-6f || len2 < 1e-6f || radius <= 0) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    dx1 /= len1;
    dy1 /= len1;
    dx2 /= len2;
    dy2 /= len2;
    float cosA = std::clamp(dx1 * dx2 + dy1 * dy2, -1.0f, 1.0f);
    float angle = std::acos(cosA);
    if (angle < 1e-6f) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    float dist = radius / std::tan(angle / 2.0f);
    float t1x = x1 + dx1 * dist, t1y = y1 + dy1 * dist;
    float t2x = x1 + dx2 * dist, t2y = y1 + dy2 * dist;
    cairo_line_to(cr_, t1x, t1y);
    float bisx = dx1 + dx2, bisy = dy1 + dy2;
    float bisLen = std::sqrt(bisx * bisx + bisy * bisy);
    if (bisLen < 1e-6f) {
      cairo_line_to(cr_, x1, y1);
      return;
    }
    bisx /= bisLen;
    bisy /= bisLen;
    float centerDist = std::sqrt(radius * radius + dist * dist);
    float ccx = x1 + bisx * centerDist, ccy = y1 + bisy * centerDist;
    float a0 = std::atan2(t1y - ccy, t1x - ccx);
    float a1 = std::atan2(t2y - ccy, t2x - ccx);
    float cross = dx1 * dy2 - dy1 * dx2;
    if (cross > 0)
      cairo_arc_negative(cr_, ccx, ccy, radius, a0, a1);
    else
      cairo_arc(cr_, ccx, ccy, radius, a0, a1);
  }
  void ellipse(float cx, float cy, float rx, float ry, float rotation,
               float startAngle, float endAngle, bool ccw = false) {
    // Standard Cairo idiom: build the arc against a unit circle under a
    // temporarily scaled/rotated/translated CTM, then restore — the path
    // points get baked into the OUTER coordinate system as this shape's
    // true (rx,ry)-scaled ellipse, and any path built before/after this
    // call in the outer frame stays correctly connected.
    cairo_save(cr_);
    cairo_translate(cr_, cx, cy);
    cairo_rotate(cr_, rotation);
    cairo_scale(cr_, std::max(1e-6f, rx), std::max(1e-6f, ry));
    if (ccw)
      cairo_arc_negative(cr_, 0, 0, 1, startAngle, endAngle);
    else
      cairo_arc(cr_, 0, 0, 1, startAngle, endAngle);
    cairo_restore(cr_);
  }
  void rect(float x, float y, float w, float h) {
    cairo_rectangle(cr_, x, y, w, h);
  }
  void roundRect(float x, float y, float w, float h, float radius) {
    constexpr float kPi = 3.14159265358979f;
    radius = std::max(0.0f, std::min(radius, std::min(w, h) / 2.0f));
    cairo_move_to(cr_, x + radius, y);
    cairo_line_to(cr_, x + w - radius, y);
    cairo_arc(cr_, x + w - radius, y + radius, radius, -kPi / 2, 0);
    cairo_line_to(cr_, x + w, y + h - radius);
    cairo_arc(cr_, x + w - radius, y + h - radius, radius, 0, kPi / 2);
    cairo_line_to(cr_, x + radius, y + h);
    cairo_arc(cr_, x + radius, y + h - radius, radius, kPi / 2, kPi);
    cairo_line_to(cr_, x, y + radius);
    cairo_arc(cr_, x + radius, y + radius, radius, kPi, kPi * 1.5);
    cairo_close_path(cr_);
  }

  // ---- drawing ----
  // fill()/stroke()/clip() preserve the current path (via the *_preserve
  // variants), matching HTML canvas semantics where only beginPath()
  // clears it.
  void fill(FillRule rule = FillRule::NonZero) {
    cairo_set_fill_rule(cr_, rule == FillRule::EvenOdd
                                 ? CAIRO_FILL_RULE_EVEN_ODD
                                 : CAIRO_FILL_RULE_WINDING);
    drawShadowThenReal([&] { cairo_fill_preserve(cr_); }, fillPaint_);
  }
  void stroke() {
    drawShadowThenReal([&] { cairo_stroke_preserve(cr_); }, strokePaint_);
  }
  void clip(FillRule rule = FillRule::NonZero) {
    cairo_set_fill_rule(cr_, rule == FillRule::EvenOdd
                                 ? CAIRO_FILL_RULE_EVEN_ODD
                                 : CAIRO_FILL_RULE_WINDING);
    cairo_clip_preserve(cr_);
  }
  void fillRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    drawShadowThenReal([&] { cairo_fill(cr_); }, fillPaint_);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }
  void strokeRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    drawShadowThenReal([&] { cairo_stroke(cr_); }, strokePaint_);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }
  // A true partial-area clear (unlike the Windows backend — see the
  // Canvas limitations note), implemented via CAIRO_OPERATOR_CLEAR.
  void clearRect(float x, float y, float w, float h) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    cairo_new_path(cr_);
    cairo_rectangle(cr_, x, y, w, h);
    cairo_operator_t prevOp = cairo_get_operator(cr_);
    cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR);
    cairo_fill(cr_);
    cairo_set_operator(cr_, prevOp);
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    cairo_path_destroy(orig);
  }

  // ---- text ----
  struct TextMetricsResult {
    float width;
  };
  TextMetricsResult measureText(const std::string &text) {
    TextStyle ts = fontToTextStyle();
    PangoLayout *layout = liteui_text::makeLayout(text, ts, -1, cr_);
    int w = 0, h = 0;
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);
    return {static_cast<float>(w)};
  }
  void fillText(const std::string &text, float x, float y,
                float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, false);
  }
  void strokeText(const std::string &text, float x, float y,
                  float maxWidth = -1) {
    drawTextImpl(text, x, y, maxWidth, true);
  }

  // ---- images ----
  void drawImage(const CanvasImage &img, float dx, float dy) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy,
              static_cast<float>(img.width), static_cast<float>(img.height));
  }
  void drawImage(const CanvasImage &img, float dx, float dy, float dw,
                 float dh) {
    drawImage(img, 0, 0, static_cast<float>(img.width),
              static_cast<float>(img.height), dx, dy, dw, dh);
  }
  void drawImage(const CanvasImage &img, float sx, float sy, float sw, float sh,
                 float dx, float dy, float dw, float dh) {
    if (img.width <= 0 || img.height <= 0)
      return;
    cairo_surface_t *surf = imageToCairoSurface(img);
    cairo_save(cr_);
    cairo_translate(cr_, dx, dy);
    cairo_scale(cr_, dw / std::max(1.0f, sw), dh / std::max(1.0f, sh));
    cairo_translate(cr_, -sx, -sy);
    cairo_set_source_surface(cr_, surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr_), CAIRO_FILTER_BILINEAR);
    cairo_paint_with_alpha(cr_, globalAlpha_);
    cairo_restore(cr_);
    cairo_surface_destroy(surf);
  }

  // ---- pixel data ----
  // Fully supported: reads straight out of this canvas's own backing
  // Cairo image surface (the render target `cr_` draws into).
  std::optional<CanvasImage> getImageData(float x, float y, float w, float h) {
    cairo_surface_t *target = cairo_get_target(cr_);
    cairo_surface_flush(target);
    int sw = cairo_image_surface_get_width(target);
    int sh = cairo_image_surface_get_height(target);
    int ix = static_cast<int>(std::floor(x)),
        iy = static_cast<int>(std::floor(y));
    int iw = std::max(0, static_cast<int>(std::round(w)));
    int ih = std::max(0, static_cast<int>(std::round(h)));
    CanvasImage out(iw, ih);
    unsigned char *src = cairo_image_surface_get_data(target);
    int stride = cairo_image_surface_get_stride(target);
    for (int row = 0; row < ih; ++row) {
      int sy = iy + row;
      if (sy < 0 || sy >= sh)
        continue;
      for (int col = 0; col < iw; ++col) {
        int sx = ix + col;
        if (sx < 0 || sx >= sw)
          continue;
        unsigned char *p = src + sy * stride + sx * 4;
        unsigned char b = p[0], g = p[1], r = p[2], a = p[3];
        unsigned char *dst =
            out.pixels.data() + (static_cast<size_t>(row) * iw + col) * 4;
        dst[0] = a ? static_cast<unsigned char>(std::min(255, r * 255 / a)) : 0;
        dst[1] = a ? static_cast<unsigned char>(std::min(255, g * 255 / a)) : 0;
        dst[2] = a ? static_cast<unsigned char>(std::min(255, b * 255 / a)) : 0;
        dst[3] = a;
      }
    }
    return out;
  }
  // putImageData ignores the current transform/compositing and pokes
  // pixels directly, matching HTML canvas semantics.
  void putImageData(const CanvasImage &img, float dx, float dy) {
    if (img.width <= 0 || img.height <= 0)
      return;
    cairo_surface_t *target = cairo_get_target(cr_);
    cairo_surface_flush(target);
    int sw = cairo_image_surface_get_width(target);
    int sh = cairo_image_surface_get_height(target);
    unsigned char *dst = cairo_image_surface_get_data(target);
    int stride = cairo_image_surface_get_stride(target);
    int ix = static_cast<int>(std::round(dx)),
        iy = static_cast<int>(std::round(dy));
    for (int row = 0; row < img.height; ++row) {
      int ty = iy + row;
      if (ty < 0 || ty >= sh)
        continue;
      for (int col = 0; col < img.width; ++col) {
        int tx = ix + col;
        if (tx < 0 || tx >= sw)
          continue;
        const unsigned char *s =
            img.pixels.data() +
            (static_cast<size_t>(row) * img.width + col) * 4;
        unsigned char r = s[0], g = s[1], b = s[2], a = s[3];
        unsigned char *p = dst + ty * stride + tx * 4;
        p[0] = static_cast<unsigned char>(b * a / 255);
        p[1] = static_cast<unsigned char>(g * a / 255);
        p[2] = static_cast<unsigned char>(r * a / 255);
        p[3] = a;
      }
    }
    cairo_surface_mark_dirty(target);
  }

private:
  struct Paint {
    bool isGradient = false;
    Color color{0, 0, 0, 255};
    CanvasGradient gradient;
    static Paint solid(Color c) {
      Paint p;
      p.isGradient = false;
      p.color = c;
      return p;
    }
    static Paint grad(const CanvasGradient &g) {
      Paint p;
      p.isGradient = true;
      p.gradient = g;
      return p;
    }
  };

  struct ExtraState {
    Paint fill, stroke;
    float globalAlpha;
    TextStyle font;
    TextAlign textAlign;
    TextBaseline textBaseline;
    Color shadowColor;
    float shadowOffsetX, shadowOffsetY;
  };

  static Color colorOf(const Paint &p) {
    if (!p.isGradient)
      return p.color;
    return p.gradient.stops.empty() ? Color{0, 0, 0, 255}
                                    : p.gradient.stops.front().color;
  }

  static cairo_operator_t toCairoOp(CompositeOp op) {
    switch (op) {
    case CompositeOp::SourceOver:
      return CAIRO_OPERATOR_OVER;
    case CompositeOp::SourceIn:
      return CAIRO_OPERATOR_IN;
    case CompositeOp::SourceOut:
      return CAIRO_OPERATOR_OUT;
    case CompositeOp::SourceAtop:
      return CAIRO_OPERATOR_ATOP;
    case CompositeOp::DestinationOver:
      return CAIRO_OPERATOR_DEST_OVER;
    case CompositeOp::DestinationIn:
      return CAIRO_OPERATOR_DEST_IN;
    case CompositeOp::DestinationOut:
      return CAIRO_OPERATOR_DEST_OUT;
    case CompositeOp::DestinationAtop:
      return CAIRO_OPERATOR_DEST_ATOP;
    case CompositeOp::Lighter:
      return CAIRO_OPERATOR_ADD;
    case CompositeOp::Copy:
      return CAIRO_OPERATOR_SOURCE;
    case CompositeOp::Xor:
      return CAIRO_OPERATOR_XOR;
    case CompositeOp::Multiply:
      return CAIRO_OPERATOR_MULTIPLY;
    case CompositeOp::Screen:
      return CAIRO_OPERATOR_SCREEN;
    }
    return CAIRO_OPERATOR_OVER;
  }

  void applyPaint(const Paint &p) {
    if (!p.isGradient) {
      Color c = p.color;
      cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                            (c.a / 255.0) * globalAlpha_);
      return;
    }
    cairo_pattern_t *pat =
        p.gradient.kind == CanvasGradient::Kind::Linear
            ? cairo_pattern_create_linear(p.gradient.x0, p.gradient.y0,
                                          p.gradient.x1, p.gradient.y1)
            : cairo_pattern_create_radial(p.gradient.x0, p.gradient.y0,
                                          p.gradient.r0, p.gradient.x1,
                                          p.gradient.y1, p.gradient.r1);
    for (auto &s : p.gradient.stops)
      cairo_pattern_add_color_stop_rgba(pat, std::clamp(s.offset, 0.0f, 1.0f),
                                        s.color.r / 255.0, s.color.g / 255.0,
                                        s.color.b / 255.0,
                                        (s.color.a / 255.0) * globalAlpha_);
    cairo_set_source(cr_, pat);
    cairo_pattern_destroy(pat);
  }

  // Cairo paths are stored as literal coordinate data, not re-evaluated
  // against a later CTM — cairo_copy_path()/cairo_append_path() is the
  // standard way to "replay" the current path under a temporarily
  // different transform (used here to offset a shadow copy) and to put
  // the app's original path back afterward, since fill()/stroke() must
  // leave the current path exactly as HTML canvas would (untouched,
  // aside from the intentional *_preserve semantics).
  template <class Fn> void drawShadowThenReal(Fn drawFn, const Paint &paint) {
    cairo_path_t *orig = cairo_copy_path(cr_);
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0)) {
      cairo_save(cr_);
      cairo_translate(cr_, shadowOffsetX_, shadowOffsetY_);
      cairo_new_path(cr_);
      cairo_append_path(cr_, orig);
      applyPaint(Paint::solid(shadowColor_));
      drawFn();
      cairo_restore(cr_);
    }
    cairo_new_path(cr_);
    cairo_append_path(cr_, orig);
    applyPaint(paint);
    drawFn();
    cairo_path_destroy(orig);
  }

  static cairo_surface_t *imageToCairoSurface(const CanvasImage &img) {
    int w = img.width, h = img.height;
    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_surface_flush(surf);
    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int row = 0; row < h; ++row) {
      unsigned char *dst = data + row * stride;
      const unsigned char *src =
          img.pixels.data() + static_cast<size_t>(row) * w * 4;
      for (int col = 0; col < w; ++col) {
        unsigned char r = src[col * 4 + 0], g = src[col * 4 + 1],
                      b = src[col * 4 + 2], a = src[col * 4 + 3];
        // Cairo's ARGB32 wants premultiplied, native-endian 32-bit
        // pixels — byte order B,G,R,A on the little-endian platforms
        // this file targets (same convention as ensureTextTexture above).
        unsigned char *px = dst + col * 4;
        px[0] = static_cast<unsigned char>(b * a / 255);
        px[1] = static_cast<unsigned char>(g * a / 255);
        px[2] = static_cast<unsigned char>(r * a / 255);
        px[3] = a;
      }
    }
    cairo_surface_mark_dirty(surf);
    return surf;
  }

  TextStyle fontToTextStyle() const {
    TextStyle ts = font_;
    ts.wrap = TextWrap::NoWrap;
    return ts;
  }

  // maxWidth is accepted for API familiarity with HTML canvas but not
  // currently applied (no horizontal squeeze-to-fit is performed).
  void drawTextImpl(const std::string &text, float x, float y, float,
                    bool stroked) {
    TextStyle ts = fontToTextStyle();
    PangoLayout *layout = liteui_text::makeLayout(text, ts, -1, cr_);
    int pw = 0, ph = 0;
    pango_layout_get_pixel_size(layout, &pw, &ph);
    PangoLayoutIter *iter = pango_layout_get_iter(layout);
    int baseline = pango_layout_iter_get_baseline(iter) / PANGO_SCALE;
    pango_layout_iter_free(iter);
    float baseX = x, baseY = y;
    switch (textAlign_) {
    case TextAlign::Center:
      baseX -= pw / 2.0f;
      break;
    case TextAlign::End:
      baseX -= static_cast<float>(pw);
      break;
    default:
      break;
    }
    switch (textBaseline_) {
    case TextBaseline::Top:
      break;
    case TextBaseline::Middle:
      baseY -= ph / 2.0f;
      break;
    case TextBaseline::Bottom:
      baseY -= static_cast<float>(ph);
      break;
    case TextBaseline::Alphabetic:
    default:
      baseY -= static_cast<float>(baseline);
      break;
    }
    auto paintOnce = [&](Color c, float ox, float oy) {
      cairo_set_source_rgba(cr_, c.r / 255.0, c.g / 255.0, c.b / 255.0,
                            (c.a / 255.0) * globalAlpha_);
      cairo_move_to(cr_, baseX + ox, baseY + oy);
      pango_cairo_show_layout(cr_, layout);
    };
    if (shadowColor_.a > 0 && (shadowOffsetX_ != 0 || shadowOffsetY_ != 0))
      paintOnce(shadowColor_, shadowOffsetX_, shadowOffsetY_);
    if (!stroked) {
      paintOnce(colorOf(fillPaint_), 0, 0);
    } else {
      // Approximates a stroked glyph outline the same way the Windows
      // backend does (see its drawTextImpl comment) — not a true glyph
      // contour outline.
      Color base = colorOf(strokePaint_);
      double lw = cairo_get_line_width(cr_);
      float r = static_cast<float>(std::max(1.0, lw / 2.0));
      static const float offs[8][2] = {
          {1, 0},       {-1, 0},       {0, 1},        {0, -1},
          {0.7f, 0.7f}, {-0.7f, 0.7f}, {0.7f, -0.7f}, {-0.7f, -0.7f}};
      for (auto &o : offs)
        paintOnce(base, o[0] * r, o[1] * r);
    }
    g_object_unref(layout);
  }

  cairo_t *cr_;
  float width_, height_;

  Paint fillPaint_ = Paint::solid(Color{0, 0, 0, 255});
  Paint strokePaint_ = Paint::solid(Color{0, 0, 0, 255});
  float dashOffset_ = 0.0f;
  float globalAlpha_ = 1.0f;
  Color shadowColor_{0, 0, 0, 0};
  float shadowOffsetX_ = 0.0f, shadowOffsetY_ = 0.0f;
  TextStyle font_;
  TextAlign textAlign_ = TextAlign::Start;
  TextBaseline textBaseline_ = TextBaseline::Alphabetic;

  std::vector<ExtraState> extraStack_;
};

#endif

// ---------------- Layout algorithm (flexbox subset) ----------------
//
// Two passes per subtree:
//  1. measureNatural() — bottom-up. Resolves a node's own border-box size:
//     Fixed/Percentage/Full resolve directly against the parent's available
//     space; Fit sums/maxes its children's own natural sizes.
//  2. placeNode() — top-down. Given a node's already-decided final box (from
//     step 1 at the root, or from the parent's flex distribution below it),
//     computes each child's basis size, distributes leftover space via
//     flexGrow/flexShrink, positions via justifyContent/alignItems, and
//     recurses.
//

// Known limitations (fine for v1, revisit if needed): percentages resolve to
// 0 when an ancestor's size is itself Fit (indefinite). flex-wrap only
// affects placeNode's placement pass — measureNatural's Fit sizing still
// assumes a single line, since a Fit main axis has no definite width to
// wrap against in the first place; give the container a definite/Full main
// size if you want Fit-height wrapping content to actually wrap.
namespace liteui_layout {

struct Natural {
  float w, h;
};

inline float resolveAxis(const Size &s, float available, bool definite,
                         float fitValue) {
  switch (s.kind) {
  case Size::Kind::Fixed:
    return s.value;
  case Size::Kind::Percentage:
    return definite ? available * s.value / 100.0f : fitValue;
  case Size::Kind::Full:
    return definite ? available : fitValue;
  case Size::Kind::Fit:
    return fitValue;
  }
  return fitValue;
}

// Clamps a resolved axis size into [minV, maxV], guarding against a
// misconfigured maxV < minV by falling back to minV.
inline float clampSize(float v, float minV, float maxV) {
  return std::clamp(v, minV, std::max(minV, maxV));
}

// Whether a given axis's scrollbar should actually be drawn/interactive:
// Scroll always shows it, Auto shows it only once content overflows the
// viewport (the "+ 0.5" is just slack against float rounding so a
// perfectly-fitting container doesn't flicker a bar on and off), and
// Hidden/Visible never do. Used by both placeNode (to decide whether to
// reserve gutter space for it) and every renderer/hit-tester (to decide
// whether to draw/click it) — kept as one function so those two places
// can never disagree about whether a bar is showing.
inline bool axisScrollbarVisible(Overflow ov, float content, float viewport) {
  if (ov == Overflow::Scroll)
    return true;
  if (ov == Overflow::Auto)
    return content > viewport + 0.5f;
  return false; // Visible, Hidden
}

// Takes node by non-const reference (unlike the rest of this "measure"
// pass, which is conceptually read-only) for exactly one reason: a scroll
// container's natural content size — the scrollable extent — needs to be
// recorded somewhere for later use by placeNode() (to offset/clip
// children), by the scrollbar-thumb sizing code, and by input handling
// (to know how far a drag/wheel event is allowed to move the scroll
// offset). node.computed.contentW/contentH is that somewhere. Everything
// else this function does is still the same bottom-up size query it always
// was.
inline Natural measureNatural(View &node, float availW, float availH,
                              bool wDefinite, bool hDefinite) {
  const Size &styleWidth =
      resolveDynamic(node.style.width, node.computed.resolvedWidth);
  const Size &styleHeight =
      resolveDynamic(node.style.height, node.computed.resolvedHeight);
  if (node.isText) {
    const EdgeInsets &pad = node.style.padding;
    bool widthIsFit = styleWidth.kind == Size::Kind::Fit;
    bool widthIndefinitePercentage =
        styleWidth.kind == Size::Kind::Percentage && !wDefinite;
    float outerW = clampSize(resolveAxis(styleWidth, availW, wDefinite, 0),
                             node.style.minWidth, node.style.maxWidth);
    float measureWidth = (widthIsFit || widthIndefinitePercentage)
                             ? -1.0f
                             : std::max(0.0f, outerW - pad.left - pad.right);
    liteui_text::Measurement m = liteui_text::measure(
        resolveDynamic(node.text, node.computed.resolvedText), node.textStyle,
        measureWidth);
    float w = (widthIsFit || widthIndefinitePercentage)
                  ? clampSize(m.width + pad.left + pad.right,
                              node.style.minWidth, node.style.maxWidth)
                  : outerW;
    float naturalH = m.height + pad.top + pad.bottom;
    float h =
        clampSize(styleHeight.kind == Size::Kind::Fit
                      ? naturalH
                      : resolveAxis(styleHeight, availH, hDefinite, naturalH),
                  node.style.minHeight, node.style.maxHeight);
    return {w, h};
  }
  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  bool needW = styleWidth.kind == Size::Kind::Fit;
  bool needH = styleHeight.kind == Size::Kind::Fit;
  bool scrollX = node.scrollsX();
  bool scrollY = node.scrollsY();

  float w = clampSize(resolveAxis(styleWidth, availW, wDefinite, 0),
                      node.style.minWidth, node.style.maxWidth);
  float h = clampSize(resolveAxis(styleHeight, availH, hDefinite, 0),
                      node.style.minHeight, node.style.maxHeight);
  // A scroll container must still visit its children even when neither
  // axis is Fit (e.g. a fixed-size scrollable box) — that's the whole
  // point: we need to know how big the content *wants* to be so we know
  // how far it can scroll, even though the container's own box size
  // doesn't depend on that at all.
  if ((!needW && !needH && !scrollX && !scrollY) || node.children.empty())
    return {w, h};

  // Along an axis this node scrolls, children are measured against
  // effectively unbounded space so they report their true desired size
  // instead of being squeezed into the viewport — that natural total is
  // exactly the "scrollable extent". Along a non-scrolling axis, sizing is
  // unchanged from before (children measured against the resolved inner
  // box, or against availW/availH while this node's own size is still
  // being figured out).
  constexpr float kUnbounded = std::numeric_limits<float>::max() / 4;
  float innerW = scrollX ? kUnbounded
                 : (wDefinite && !needW)
                     ? std::max(0.0f, w - pad.left - pad.right)
                     : availW;
  float innerH = scrollY ? kUnbounded
                 : (hDefinite && !needH)
                     ? std::max(0.0f, h - pad.top - pad.bottom)
                     : availH;

  float mainTotal = 0, crossMax = 0;
  bool firstFlow = true;
  for (size_t i = 0; i < node.children.size(); ++i) {
    View &c = node.children[i];
    if (c.style.position == Position::Absolute ||
        c.style.display == Display::None)
      continue; // out of flow / not rendered: doesn't affect Fit size
    Natural cn = measureNatural(c, innerW, innerH,
                                scrollX ? false : (wDefinite || !needW),
                                scrollY ? false : (hDefinite || !needH));
    float mm = c.style.margin.left + c.style.margin.right;
    float mv = c.style.margin.top + c.style.margin.bottom;
    float childMain = horizontal ? cn.w + mm : cn.h + mv;
    float childCross = horizontal ? cn.h + mv : cn.w + mm;
    if (!firstFlow)
      mainTotal += node.style.gap;
    mainTotal += childMain;
    crossMax = std::max(crossMax, childCross);
    firstFlow = false;
  }
  if (needW)
    w = clampSize((horizontal ? mainTotal : crossMax) + pad.left + pad.right,
                  node.style.minWidth, node.style.maxWidth);

  if (needH)
    h = clampSize((horizontal ? crossMax : mainTotal) + pad.top + pad.bottom,
                  node.style.minHeight, node.style.maxHeight);

  // Content extent is deliberately NOT run through clampSize/min-max: a
  // container's max-width doesn't shrink its *content*, only its own box
  // (that's the entire reason overflow is a thing).
  if (scrollX)
    node.computed.contentW =
        (horizontal ? mainTotal : crossMax) + pad.left + pad.right;
  if (scrollY)
    node.computed.contentH =
        (horizontal ? crossMax : mainTotal) + pad.top + pad.bottom;
  return {w, h};
}

inline void placeNode(View &node, float x, float y, float w, float h) {
  // contentW/contentH here already existed as "content's natural size" on
  // node.computed before this call, set by the measureNatural() pass that
  // ran over this same node earlier (either from the parent's per-child
  // loop, or from layoutRoot() for the root). We're about to overwrite
  // x/y/w/h with the final viewport box; contentW/contentH and
  // scrollX/scrollY are left untouched by this assignment.
  node.computed.x = x;
  node.computed.y = y;
  node.computed.w = w;
  node.computed.h = h;
  if (node.onLayout)
    node.onLayout(x, y, w, h);

  if (node.children.empty()) {
    // No children means nothing to scroll regardless of overflow setting;
    // pin the offset at 0 so a container that briefly had children (and
    // therefore a scroll offset) doesn't leave a stale one behind if it's
    // ever emptied out.
    node.computed.scrollX = node.computed.scrollY = 0;
    return;
  }

  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  float contentW = std::max(0.0f, w - pad.left - pad.right);
  float contentH = std::max(0.0f, h - pad.top - pad.bottom);

  // ---- Scrolling: gutter reservation, offset clamping, content origin ----
  // Whether each axis's scrollbar is actually showing determines whether it
  // eats into the space available for children — same rule CSS uses (a
  // visible scrollbar shrinks the content box on the OTHER axis; the
  // scrolling axis itself is unbounded so its own bar doesn't need to
  // "make room" against itself).
  bool showVBar =
      node.scrollsY() &&
      axisScrollbarVisible(node.style.overflowY, node.computed.contentH, h);
  bool showHBar =
      node.scrollsX() &&
      axisScrollbarVisible(node.style.overflowX, node.computed.contentW, w);
  if (showVBar)
    contentW = std::max(0.0f, contentW - kScrollbarThickness);
  if (showHBar)
    contentH = std::max(0.0f, contentH - kScrollbarThickness);

  // Scroll offsets are user/input-driven state that can go stale the
  // instant content size or viewport size changes (a window resize, or
  // content shrinking), so every relayout re-clamps them into range rather
  // than trusting whatever a previous frame left behind.
  if (node.scrollsX())
    node.computed.scrollX =
        std::clamp(node.computed.scrollX, 0.0f, node.maxScrollX());
  else
    node.computed.scrollX = 0;
  if (node.scrollsY())
    node.computed.scrollY =
        std::clamp(node.computed.scrollY, 0.0f, node.maxScrollY());
  else
    node.computed.scrollY = 0;

  // The content origin simply shifts by the (clamped) scroll offset —
  // children are positioned exactly as they would be at scroll (0,0), then
  // this single subtraction slides the whole subtree. Clipping (handled by
  // the renderers/hit-testers, not here) is what actually hides the part
  // that scrolls out of view.
  float contentX = x + pad.left - node.computed.scrollX;
  float contentY = y + pad.top - node.computed.scrollY;

  float mainAvail = horizontal ? contentW : contentH;
  float crossAvail = horizontal ? contentH : contentW;
  // A scrolling main axis must never let flexShrink squeeze children below
  // their natural size just because the viewport is smaller than the
  // content — that's the entire point of scrolling instead of shrinking.
  // Widening mainAvail to at least the natural content total makes
  // "leftover" in the flex-resolution pass below >= 0, which keeps every
  // shrink factor's candidate at-or-above its unclamped basis.
  if (horizontal && node.scrollsX())
    mainAvail =
        std::max(mainAvail, node.computed.contentW - pad.left - pad.right);
  if (!horizontal && node.scrollsY())
    mainAvail =
        std::max(mainAvail, node.computed.contentH - pad.top - pad.bottom);

  size_t n = node.children.size();
  bool wrap = node.style.flexWrap == FlexWrap::Wrap;
  std::vector<size_t> flowIdx;
  flowIdx.reserve(node.children.size());
  for (size_t i = 0; i < node.children.size(); ++i) {
    const Style &cs = node.children[i].style;
    if (cs.position != Position::Absolute && cs.display != Display::None)
      flowIdx.push_back(i);
  }
  n = flowIdx.size();
  std::vector<float> basis(n), cross(n), mMainS(n), mMainE(n), mCrossS(n),
      mCrossE(n), minMain(n), maxMain(n), marginMain(n);

  for (size_t k = 0; k < n; ++k) {
    View &c = node.children[flowIdx[k]];
    Natural cn = measureNatural(c, contentW, contentH, true, true);
    basis[k] = horizontal ? cn.w : cn.h;
    cross[k] = horizontal ? cn.h : cn.w;
    mMainS[k] = horizontal ? c.style.margin.left : c.style.margin.top;
    mMainE[k] = horizontal ? c.style.margin.right : c.style.margin.bottom;
    mCrossS[k] = horizontal ? c.style.margin.top : c.style.margin.left;
    mCrossE[k] = horizontal ? c.style.margin.bottom : c.style.margin.right;
    minMain[k] = horizontal ? c.style.minWidth : c.style.minHeight;
    maxMain[k] = horizontal ? c.style.maxWidth : c.style.maxHeight;
    marginMain[k] = mMainS[k] + mMainE[k];
  }

  // ---- Line breaking ----
  // With wrap disabled this is always one line spanning every child (the
  // original single-line behavior, byte-for-byte). With wrap enabled,
  // children are greedily packed onto a line until the next child's basis
  // would overflow mainAvail, at which point a new line starts. A line
  // always takes at least one child, even an oversized one, so a single
  // giant child can't stall the packer.
  struct Line {
    size_t begin, end; // half-open [begin, end) into node.children
  };
  std::vector<Line> lines;
  if (!wrap) {
    lines.push_back({0, n});
  } else {
    size_t start = 0;
    float used = 0;
    for (size_t i = 0; i < n; ++i) {
      float itemMain = basis[i] + marginMain[i];
      float withGap = (i > start) ? node.style.gap : 0.0f;
      if (i > start && used + withGap + itemMain > mainAvail) {
        lines.push_back({start, i});
        start = i;
        used = itemMain;
      } else {
        used += withGap + itemMain;
      }
    }
    lines.push_back({start, n});
  }

  // ---- Per-line main-axis flex resolution ----
  // Resolve flexGrow/flexShrink into final main-axis sizes, honoring each
  // child's own min/max — this is CSS flexbox's "resolve flexible lengths"
  // algorithm, scoped to one line's children at a time. A single pass
  // (basis + share of leftover, then clamp) would silently drop whatever a
  // clamped child couldn't absorb; instead, any item whose share would
  // violate its own bound gets frozen at that bound and removed from the
  // pool, and the remaining free space is recalculated and redistributed
  // among the still-flexible siblings. Repeats until nothing new freezes
  // (at most one extra item freezes per pass, so ln+1 passes always
  // suffices). Also tracks each line's cross size (max child cross extent)
  // for the cross-axis distribution pass below.
  std::vector<float> finalMain(n);
  std::vector<float> lineCross(lines.size());
  for (size_t li = 0; li < lines.size(); ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    std::vector<bool> frozen(ln, false);
    std::vector<float> lineFinal(ln);
    float gapTotal = ln > 1 ? node.style.gap * (ln - 1) : 0.0f;

    for (size_t pass = 0; pass <= ln; ++pass) {
      float used = gapTotal, gsum = 0, ssum = 0;
      for (size_t k = 0; k < ln; ++k) {
        size_t i = lb + k;
        used += (frozen[k] ? lineFinal[k] : basis[i]) + marginMain[i];
        if (!frozen[k]) {
          gsum += node.children[flowIdx[i]].style.flexGrow;
          ssum += node.children[flowIdx[i]].style.flexShrink;
        }
      }
      float leftover = mainAvail - used;
      if (leftover == 0 || (leftover > 0 && gsum <= 0) ||
          (leftover < 0 && ssum <= 0)) {
        for (size_t k = 0; k < ln; ++k)
          if (!frozen[k])
            lineFinal[k] =
                clampSize(basis[lb + k], minMain[lb + k], maxMain[lb + k]);
        break;
      }
      bool frozeAny = false;
      for (size_t k = 0; k < ln; ++k) {
        if (frozen[k])
          continue;
        size_t i = lb + k;
        const Style &cs = node.children[flowIdx[i]].style;
        float extra = leftover > 0 ? leftover * (cs.flexGrow / gsum)
                                   : leftover * (cs.flexShrink / ssum);
        float candidate = std::max(0.0f, basis[i] + extra);
        float clamped = clampSize(candidate, minMain[i], maxMain[i]);
        lineFinal[k] = clamped;
        if (clamped != candidate) {
          frozen[k] = true;
          frozeAny = true;
        }
      }
      if (!frozeAny)
        break; // this pass's candidates all satisfied their bounds — done
    }

    float maxCross = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      finalMain[i] = lineFinal[k];
      maxCross = std::max(maxCross, cross[i] + mCrossS[i] + mCrossE[i]);
    }
    lineCross[li] = maxCross;
  }

  // ---- Distribute lines along the cross axis (align-content) ----
  // With exactly one line this collapses to the old behavior: Stretch
  // grows that line to fill crossAvail (matching the previous unconditional
  // stretch-to-container-cross-size), everything else just packs the one
  // line at the start.
  size_t numLines = lines.size();
  float lineGapTotal = numLines > 1 ? node.style.gap * (numLines - 1) : 0.0f;
  float linesTotal = lineGapTotal;
  for (float lc : lineCross)
    linesTotal += lc;
  float crossFree = std::max(0.0f, crossAvail - linesTotal);

  std::vector<float> lineOffset(numLines), lineSize(numLines);
  float crossStart = 0, crossBetween = node.style.gap;
  switch (node.style.alignContent) {
  case AlignContent::Start:
    lineSize = lineCross;
    break;
  case AlignContent::End:
    crossStart = crossFree;
    lineSize = lineCross;
    break;
  case AlignContent::Center:
    crossStart = crossFree / 2;
    lineSize = lineCross;
    break;
  case AlignContent::SpaceBetween:
    if (numLines > 1)
      crossBetween += crossFree / (numLines - 1);
    lineSize = lineCross;
    break;
  case AlignContent::SpaceAround: {
    float each = numLines ? crossFree / numLines : 0;
    crossStart = each / 2;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::SpaceEvenly: {
    float each = crossFree / (numLines + 1);
    crossStart = each;
    crossBetween += each;
    lineSize = lineCross;
    break;
  }
  case AlignContent::Stretch: {
    float extra = numLines ? crossFree / numLines : 0;
    for (size_t li = 0; li < numLines; ++li)
      lineSize[li] = lineCross[li] + extra;
    break;
  }
  }
  {
    float pos = crossStart;
    for (size_t li = 0; li < numLines; ++li) {
      lineOffset[li] = pos;
      pos += lineSize[li] + crossBetween;
    }
  }

  // ---- Per-line: justify main axis, align children within the line's
  // cross extent, then recurse ----
  for (size_t li = 0; li < numLines; ++li) {
    size_t lb = lines[li].begin, le = lines[li].end;
    size_t ln = le - lb;
    float lineCrossAvail = lineSize[li];
    float lineCrossPos = (horizontal ? contentY : contentX) + lineOffset[li];

    float totalUsed = 0;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      totalUsed += finalMain[i] + mMainS[i] + mMainE[i];
      if (k + 1 < ln)
        totalUsed += node.style.gap;
    }
    float freeSpace = std::max(0.0f, mainAvail - totalUsed);
    float startOffset = 0, between = node.style.gap;
    switch (node.style.justifyContent) {
    case Justify::Start:
      break;
    case Justify::End:
      startOffset = freeSpace;
      break;
    case Justify::Center:
      startOffset = freeSpace / 2;
      break;
    case Justify::SpaceBetween:
      if (ln > 1)
        between += freeSpace / (ln - 1);
      break;
    case Justify::SpaceAround: {
      float each = ln ? freeSpace / ln : 0;
      startOffset = each / 2;
      between += each;
      break;
    }
    case Justify::SpaceEvenly: {
      float each = freeSpace / (ln + 1);
      startOffset = each;
      between += each;
      break;
    }
    }

    float cursor = (horizontal ? contentX : contentY) + startOffset;
    for (size_t k = 0; k < ln; ++k) {
      size_t i = lb + k;
      View &ch = node.children[flowIdx[i]];
      cursor += mMainS[i];

      bool explicitCross =
          horizontal
              ? resolveDynamic(ch.style.height, ch.computed.resolvedHeight)
                        .kind != Size::Kind::Fit
              : resolveDynamic(ch.style.width, ch.computed.resolvedWidth)
                        .kind != Size::Kind::Fit;
      float finalCross = cross[i];
      if (node.style.alignItems == Align::Stretch && !explicitCross)
        finalCross = std::max(0.0f, lineCrossAvail - mCrossS[i] - mCrossE[i]);

      float minCross = horizontal ? ch.style.minHeight : ch.style.minWidth;
      float maxCross = horizontal ? ch.style.maxHeight : ch.style.maxWidth;
      finalCross = clampSize(finalCross, minCross, maxCross);

      float crossOffset;
      switch (node.style.alignItems) {
      case Align::End:
        crossOffset = lineCrossAvail - finalCross - mCrossE[i];
        break;
      case Align::Center:
        crossOffset = (lineCrossAvail - finalCross) / 2;
        break;
      default:
        crossOffset = mCrossS[i];
        break; // Start & Stretch
      }

      float cx = horizontal ? cursor : lineCrossPos + crossOffset;
      float cy = horizontal ? lineCrossPos + crossOffset : cursor;
      float cw = horizontal ? finalMain[i] : finalCross;
      float chh = horizontal ? finalCross : finalMain[i];

      placeNode(ch, cx, cy, cw, chh);
      cursor += finalMain[i] + mMainE[i] + between;
    }
  }

  // ---- Position::Absolute children ----
  // Placed against this node's content box, entirely independent of the
  // flex distribution above. Sizing reuses measureNatural: Fixed/
  // Percentage/Full resolve normally against contentW/contentH (always
  // definite here, since this node's own box is already finalized);
  // Fit falls back to natural content size unless both opposing edges
  // are set, in which case size is derived from them (CSS's "left+right
  // implies width" rule).
  for (auto &ch : node.children) {
    if (ch.style.position != Position::Absolute ||
        ch.style.display == Display::None)
      continue;
    const Style &cs = ch.style;
    const Size &csWidth = resolveDynamic(cs.width, ch.computed.resolvedWidth);
    const Size &csHeight =
        resolveDynamic(cs.height, ch.computed.resolvedHeight);
    bool hasL = !std::isnan(cs.left), hasR = !std::isnan(cs.right);
    bool hasT = !std::isnan(cs.top), hasB = !std::isnan(cs.bottom);

    Natural probe = measureNatural(ch, contentW, contentH, true, true);
    float aw = (csWidth.kind == Size::Kind::Fit && hasL && hasR)
                   ? contentW - cs.left - cs.right
                   : probe.w;
    float ah = (csHeight.kind == Size::Kind::Fit && hasT && hasB)
                   ? contentH - cs.top - cs.bottom
                   : probe.h;
    aw = clampSize(aw, cs.minWidth, cs.maxWidth);
    ah = clampSize(ah, cs.minHeight, cs.maxHeight);

    float ax = hasL   ? contentX + cs.left + cs.margin.left
               : hasR ? contentX + contentW - cs.right - cs.margin.right - aw
                      : contentX + cs.margin.left;
    float ay = hasT   ? contentY + cs.top + cs.margin.top
               : hasB ? contentY + contentH - cs.bottom - cs.margin.bottom - ah
                      : contentY + cs.margin.top;

    placeNode(ch, ax, ay, aw, ah);
  }
}

// originX/originY let a caller reserve space above/left of the root — used
// on Linux to keep content out of the custom titlebar strip; Windows (no
// custom titlebar) always passes the defaults of (0, 0).
inline void layoutRoot(View &root, float windowW, float windowH,
                       float originX = 0, float originY = 0) {
  Natural n = measureNatural(root, windowW, windowH, true, true);
  placeNode(root, originX, originY, n.w, n.h);
}

} // namespace liteui_layout

class LiteUI {

public:
  // Constructor: explicit prevents accidental implicit conversions from a bare
  // int; defaults give an 800x600 "Window".
  explicit LiteUI(int width = 800, int height = 600,
                  const std::string &title = "Window");
  // Destructor: tears down whatever platform resources were created.
  ~LiteUI();
  // Copying is disabled — a LiteUI owns unique OS handles that can't be
  // safely duplicated.
  LiteUI(const LiteUI &) = delete;
  // Copy-assignment is disabled too.
  LiteUI &operator=(const LiteUI &) = delete;

  // Public entry point that blocks the calling thread and pumps the platform's
  // event loop.
  void run(); // blocks, runs the event loop

  // Adds a static box to be drawn on the window and triggers a repaint.
  void addBox(const Box &box);

  // Sets (or replaces) the root of the layout tree, lays it out immediately,
  // and triggers a repaint. Ownership of the tree is copied/moved in.
  void setRoot(View view);

  // Everything below is internal implementation detail.
private:
  // Requested window width in pixels, stored so pixel-drawing helpers can
  // bounds-check.
  int width_;
  // Requested window height in pixels, same purpose as width_.
  int height_;

  // Boxes queued for drawing, in the order addBox() was called (paint order).
  std::vector<Box> boxes_;
  View root_;
  bool hasRoot_ = false;

  // Re-runs the layout algorithm over root_ against the current window size.
  void relayout() {
    if (!hasRoot_)
      return;
#if defined(_WIN32)
    // Native window decorations — the root view owns the whole client area.
    liteui_layout::layoutRoot(root_, static_cast<float>(width_),
                              static_cast<float>(height_));
#else
    // Custom (client-side) titlebar occupies the top kTitlebarHeight
    // pixels, drawn as an opaque overlay after content (see redraw()).
    // The root view must be laid out *below* that strip, not underneath
    // it — otherwise a child near the top of root (e.g. a heading) can
    // extend far enough to peek out past the titlebar's bottom edge,
    // which is exactly what happened here: the heading's box reached
    // deeper than kTitlebarHeight, so its lower slice (including the
    // text) rendered outside the titlebar's opaque cover instead of
    // safely underneath it.
    float top = static_cast<float>(kTitlebarHeight);
    float availH = std::max(0.0f, static_cast<float>(height_) - top);
    liteui_layout::layoutRoot(root_, static_cast<float>(width_), availH, 0.0f,
                              top);
#endif
  }

  // A rectangle used to accumulate the intersection of every scrollable
  // ancestor's viewport as we walk down the tree, so painting and
  // hit-testing can both tell "is this point/pixel actually visible, or
  // has it scrolled behind a clipping ancestor". Defaults to "the whole
  // plane" so the root of any walk starts unclipped.
  struct ClipRect {
    float x0 = -std::numeric_limits<float>::infinity();
    float y0 = -std::numeric_limits<float>::infinity();
    float x1 = std::numeric_limits<float>::infinity();
    float y1 = std::numeric_limits<float>::infinity();
    bool contains(float px, float py) const {
      return px >= x0 && px < x1 && py >= y0 && py < y1;
    }
    // Narrows this clip to also be inside the given box — used every time
    // we descend into a scroll container's children.
    ClipRect intersect(float bx, float by, float bw, float bh) const {
      return {std::max(x0, bx), std::max(y0, by), std::min(x1, bx + bw),
              std::min(y1, by + bh)};
    }
  };

  // Plain float rectangle for scrollbar geometry (track/thumb), kept
  // separate from Wayland's integer-pixel `Rect` below since scrollbar math
  // wants to stay in the same float space as View::Computed.
  struct PixRect {
    float x, y, w, h;
  };

  // Whether view v's vertical/horizontal scrollbar is currently showing —
  // thin wrappers around axisScrollbarVisible() using v's own already-
  // computed sizes, so callers don't have to repeat the h vs. contentH /
  // w vs. contentW pairing correctly every time.
  static bool wantVBar(const View &v) {
    return v.scrollsY() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowY, v.computed.contentH, v.computed.h);
  }
  static bool wantHBar(const View &v) {
    return v.scrollsX() &&
           liteui_layout::axisScrollbarVisible(
               v.style.overflowX, v.computed.contentW, v.computed.w);
  }

  // Track rectangles run the full length of their edge, minus the corner
  // square where both bars would otherwise overlap (only relevant when
  // both axes scroll at once).
  static PixRect vTrackRect(const View &v) {
    float h = v.computed.h - (wantHBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x + v.computed.w - kScrollbarThickness, v.computed.y,
            kScrollbarThickness, std::max(0.0f, h)};
  }
  static PixRect hTrackRect(const View &v) {
    float w = v.computed.w - (wantVBar(v) ? kScrollbarThickness : 0.0f);
    return {v.computed.x, v.computed.y + v.computed.h - kScrollbarThickness,
            std::max(0.0f, w), kScrollbarThickness};
  }

  // Minimum thumb length so a very long scrollable area doesn't shrink the
  // thumb down to an unclickable sliver.
  static constexpr float kMinThumb = 20.0f;

  // Thumb length is proportional to viewport/content (how much of the
  // content is visible at once); thumb position is proportional to how far
  // through the scrollable range the current offset is.
  static PixRect vThumbRect(const View &v) {
    PixRect track = vTrackRect(v);
    float thumbH =
        v.computed.contentH > 0
            ? std::clamp(track.h * (v.computed.h / v.computed.contentH),
                         kMinThumb, track.h)
            : track.h;
    float maxScroll = v.maxScrollY();
    float pos = maxScroll > 0
                    ? (v.computed.scrollY / maxScroll) * (track.h - thumbH)
                    : 0.0f;
    return {track.x, track.y + pos, track.w, thumbH};
  }
  static PixRect hThumbRect(const View &v) {
    PixRect track = hTrackRect(v);
    float thumbW =
        v.computed.contentW > 0
            ? std::clamp(track.w * (v.computed.w / v.computed.contentW),
                         kMinThumb, track.w)
            : track.w;
    float maxScroll = v.maxScrollX();
    float pos = maxScroll > 0
                    ? (v.computed.scrollX / maxScroll) * (track.w - thumbW)
                    : 0.0f;
    return {track.x + pos, track.y, thumbW, track.h};
  }
  static bool pixRectContains(const PixRect &r, float px, float py) {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }

  // ---- Press/drag/wheel resolution against scrollbars & scrollable content
  // ---- What a press (or a wheel event, which reuses this to find its target)
  // landed on. VThumb/HThumb mean "start dragging that thumb"; VTrack/
  // HTrack mean "clicked empty track — jump the thumb to click position;
  // Content means "this is inside some scrollable view's content area
  // (not its scrollbar)" — used both to start a possible pan-to-scroll
  // drag and, for wheel events, as the view to scroll.
  enum class ScrollHit { None, VThumb, HThumb, VTrack, HTrack, Content };
  struct ScrollPress {
    ScrollHit kind = ScrollHit::None;
    View *view = nullptr;
    float trackFrac = 0; // 0..1 position along the track, for VTrack/HTrack
  };

  // Walks down from v looking for the deepest relevant scroll interaction
  // under (x, y). Each node's own scrollbars are checked before recursing
  // into its children, which is always correct because layout already
  // reserves the scrollbar's gutter out of the children's placement area —
  // a child box can never overlap its parent's own scrollbar. Known
  // limitation shared with hitTestFlow below: Position::Absolute
  // descendants aren't threaded through this clip-aware walk at all (they
  // bypass the normal recursion entirely, same as elsewhere in this file),
  // so an absolute view inside a scrolled-out region can still be found —
  // acceptable for v1, matching this file's existing absolute-positioning
  // trade-offs.
  static ScrollPress resolveScrollTarget(View &v, float x, float y,
                                         ClipRect clip) {
    if (v.style.visibility == Visibility::Hidden)
      return {};
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return {};
    if (wantVBar(v)) {
      PixRect track = vTrackRect(v);
      if (pixRectContains(track, x, y)) {
        PixRect thumb = vThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::VThumb, &v, 0};
        float frac = track.h > 0 ? (y - track.y) / track.h : 0;
        return {ScrollHit::VTrack, &v, frac};
      }
    }
    if (wantHBar(v)) {
      PixRect track = hTrackRect(v);
      if (pixRectContains(track, x, y)) {
        PixRect thumb = hThumbRect(v);
        if (pixRectContains(thumb, x, y))
          return {ScrollHit::HThumb, &v, 0};
        float frac = track.w > 0 ? (x - track.x) / track.w : 0;
        return {ScrollHit::HTrack, &v, frac};
      }
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          it->style.display == Display::None)
        continue;
      ScrollPress r = resolveScrollTarget(*it, x, y, childClip);
      if (r.kind != ScrollHit::None)
        return r;
    }
    if (v.scrollsX() || v.scrollsY())
      return {ScrollHit::Content, &v, 0};
    return {};
  }

  // How a click-and-drag inside scrollable content is currently being
  // interpreted. Mirrors pressedView_'s press/release pairing, but for
  // scroll interactions instead of onClick.
  enum class DragMode { None, VThumb, HThumb, ContentPan };
  struct ScrollDrag {
    DragMode mode = DragMode::None;
    View *target = nullptr;
    float startPointerX = 0, startPointerY = 0;
    float startScrollX = 0, startScrollY = 0;
    // ContentPan only: whether the pointer has moved past the click/drag
    // threshold yet. Until it does, this might still turn out to be an
    // ordinary click (see beginScrollPress/updateScrollDrag/endScrollPress).
    bool moved = false;
  } scrollDrag_;

  // Pixels of pointer movement before a press-in-content is treated as a
  // pan-to-scroll drag rather than a click.
  static constexpr float kDragThreshold = 4.0f;

  // Call on every left-button press, before the existing beginPress(). If
  // this returns true, it fully owns the press (a scrollbar grab, or a
  // track-click jump) and the caller must NOT also call beginPress() —
  // there's nothing left to click. If it returns false, the caller should
  // fall through to its normal beginPress(x, y) — resolveScrollTarget()
  // found either nothing scrollable, or plain scrollable *content*, in
  // which case a ContentPan drag is armed here but the ordinary click path
  // still runs too, since a small movement should behave as a click, not a
  // pan (see updateScrollDrag/endScrollPress).
  bool beginScrollPress(float x, float y) {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    switch (r.kind) {
    case ScrollHit::VThumb:
      scrollDrag_ = {
          DragMode::VThumb,         r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      return true;
    case ScrollHit::HThumb:
      scrollDrag_ = {
          DragMode::HThumb,         r.view, x, y, r.view->computed.scrollX,
          r.view->computed.scrollY, true};
      return true;
    case ScrollHit::VTrack: {
      PixRect track = vTrackRect(*r.view), thumb = vThumbRect(*r.view);
      float target = r.trackFrac * track.h - thumb.h / 2;
      float maxScroll = r.view->maxScrollY();
      float range = track.h - thumb.h;
      r.view->computed.scrollY = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::HTrack: {
      PixRect track = hTrackRect(*r.view), thumb = hThumbRect(*r.view);
      float target = r.trackFrac * track.w - thumb.w / 2;
      float maxScroll = r.view->maxScrollX();
      float range = track.w - thumb.w;
      r.view->computed.scrollX = std::clamp(
          range > 0 ? (target / range) * maxScroll : 0.0f, 0.0f, maxScroll);
      relayout();
      return true;
    }
    case ScrollHit::Content:
      if (r.view->style.contentPanEnabled) {
        scrollDrag_ = {
            DragMode::ContentPan,     r.view, x, y, r.view->computed.scrollX,
            r.view->computed.scrollY, false};
      }
      return false; // ordinary click press still proceeds too
    case ScrollHit::None:
      return false;
    }
    return false;
  }

  // Call on every pointer-motion event while a button is held. Advances an
  // in-progress scrollbar drag or content pan; a no-op if scrollDrag_ isn't
  // active. Returns true if it changed anything (caller should repaint).
  bool updateScrollDrag(float x, float y) {
    if (scrollDrag_.mode == DragMode::None)
      return false;
    View &v = *scrollDrag_.target;
    float dx = x - scrollDrag_.startPointerX;
    float dy = y - scrollDrag_.startPointerY;
    if (scrollDrag_.mode == DragMode::ContentPan) {
      if (!scrollDrag_.moved && std::abs(dx) < kDragThreshold &&
          std::abs(dy) < kDragThreshold)
        return false; // still within click tolerance — not a pan yet
      if (!scrollDrag_.moved) {
        scrollDrag_.moved = true;
        // It just became a drag, not a click — cancel any pending onClick
        // so the eventual release doesn't also fire it.
        pressedView_[btnIdx(MouseButton::Left)] = nullptr;
      }
      if (v.scrollsX())
        v.computed.scrollX =
            std::clamp(scrollDrag_.startScrollX - dx, 0.0f, v.maxScrollX());
      if (v.scrollsY())
        v.computed.scrollY =
            std::clamp(scrollDrag_.startScrollY - dy, 0.0f, v.maxScrollY());
    } else if (scrollDrag_.mode == DragMode::VThumb) {
      PixRect track = vTrackRect(v), thumb = vThumbRect(v);
      float range = track.h - thumb.h;
      float delta = range > 0 ? (dy / range) * v.maxScrollY() : 0.0f;
      v.computed.scrollY =
          std::clamp(scrollDrag_.startScrollY + delta, 0.0f, v.maxScrollY());
    } else if (scrollDrag_.mode == DragMode::HThumb) {
      PixRect track = hTrackRect(v), thumb = hThumbRect(v);
      float range = track.w - thumb.w;
      float delta = range > 0 ? (dx / range) * v.maxScrollX() : 0.0f;
      v.computed.scrollX =
          std::clamp(scrollDrag_.startScrollX + delta, 0.0f, v.maxScrollX());
    }
    relayout();
    return true;
  }

  // Call on every left-button release. Resolves a ContentPan that never
  // crossed the drag threshold back into an ordinary click via the
  // existing endPress(); anything that already became a real drag (a
  // scrollbar grab, or a pan that moved) just ends quietly. Always clears
  // scrollDrag_ so a stale target can't leak into some unrelated later
  // press.
  bool endScrollPress(float x, float y) {
    bool changed = false;
    if (scrollDrag_.mode == DragMode::None ||
        (scrollDrag_.mode == DragMode::ContentPan && !scrollDrag_.moved))
      changed = endPress(x, y);
    scrollDrag_ = {};
    return changed;
  }

  // Call on every wheel/scroll event; deltaX/deltaY are in pixels (already
  // sign-adjusted so positive means "scroll right"/"scroll down" — each
  // platform's wheel callback is responsible for that conversion). Finds
  // the deepest scrollable view under the cursor via the same walk used
  // for press resolution — scrollbar vs. content doesn't matter for wheel
  // input, both count as "the pointer is over this scrollable view".
  // Returns true if it changed anything (caller should repaint).
  //
  // If the resolved view has style.wheelScrollEnabled == false, this is a
  // no-op — the wheel no longer moves that view's scroll position at all,
  // even though it's still scrollable via its scrollbar. This function is
  // entirely separate from dispatchScroll()/onScrollUp/onScrollDown: a
  // caller that wants "wheel does something custom instead of scrolling"
  // (e.g. zoom) should turn this off and rely on those handlers, which
  // fire regardless of wheelScrollEnabled.
  bool applyWheelScroll(float x, float y, float deltaX, float deltaY) {
    if (!hasRoot_)
      return false;
    ScrollPress r = resolveScrollTarget(root_, x, y, ClipRect{});
    if (r.kind == ScrollHit::None || !r.view)
      return false;
    View &v = *r.view;
    if (!v.style.wheelScrollEnabled)
      return false; // pixel scroll disabled for this view; scrollbar
                    // drag/track-click still work normally, and
                    // onScrollUp/onScrollDown still fire via the
                    // caller's separate dispatchScroll() call
    bool changed = false;
    if (v.scrollsY() && deltaY != 0.0f) {
      float ns = std::clamp(v.computed.scrollY + deltaY, 0.0f, v.maxScrollY());
      changed |= ns != v.computed.scrollY;
      v.computed.scrollY = ns;
    }
    if (v.scrollsX() && deltaX != 0.0f) {
      float ns = std::clamp(v.computed.scrollX + deltaX, 0.0f, v.maxScrollX());
      changed |= ns != v.computed.scrollX;
      v.computed.scrollX = ns;
    }
    if (changed)
      relayout();
    return changed;
  }

  // Global z-index stacking, shared by both backends.
  struct AbsoluteEntry {
    const View *view;
    int order; // document/discovery order, for stable z-index ties
  };

  // Walks the whole tree (not just direct children) collecting every
  // Position::Absolute node, tagged with its pre-order discovery index.
  // Recurses into every node regardless of its own position, so nested
  // absolutes (an absolute inside another absolute's subtree) still get
  // their own top-level slot in the global list.
  void collectAbsolutes(const View &v, std::vector<AbsoluteEntry> &out) {
    for (const auto &child : v.children) {
      if (child.style.display == Display::None)
        continue; // a display:none subtree contributes no absolutes either
      if (child.style.position == Position::Absolute)
        out.push_back({&child, static_cast<int>(out.size())});
      collectAbsolutes(child, out);
    }
  }

  // Sorts absolute entries by zIndex ascending, document order breaking
  // ties — shared by paintRoot() (Windows) and redraw() (Linux).
  static void sortAbsolutes(std::vector<AbsoluteEntry> &absolutes) {
    std::stable_sort(absolutes.begin(), absolutes.end(),
                     [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                       if (a.view->style.zIndex != b.view->style.zIndex)
                         return a.view->style.zIndex < b.view->style.zIndex;
                       return a.order < b.order;
                     });
  }

  // Returns whether (px, py) lies within v's already-computed border-box.
  static bool containsPoint(const View &v, float px, float py) {
    return px >= v.computed.x && px < v.computed.x + v.computed.w &&
           py >= v.computed.y && py < v.computed.y + v.computed.h;
  }

  // Whether v itself would respond to a press/click of the given button —
  // shared by hitTestFlow so each button tracks its own independent hit
  // test instead of only ever matching onClick/onPressAt.
  static bool hasButtonHandler(const View &v, MouseButton btn) {
    if (resolveDynamic(v.disabled, v.computed.resolvedDisabled))
      return false;
    switch (btn) {
    case MouseButton::Left:
      return (bool)v.onClick || (bool)v.onPressAt;
    case MouseButton::Middle:
      return (bool)v.onMiddleClick || (bool)v.onMiddlePressAt;
    case MouseButton::Right:
      return (bool)v.onRightClick || (bool)v.onRightPressAt;
    }
    return false;
  }

  // Recursively finds the topmost in-flow view under (x, y) with a
  // non-null onClick. Children are checked last-to-first (later siblings
  // paint on top), and Position::Absolute children are skipped here —
  // they're handled globally by hitTest(), same split as
  // collectAbsolutes()/renderView(). If the deepest matching view (or any
  // of its flow descendants) has no handler, the search falls through to
  // checking `v` itself, so a click bubbles up to the nearest ancestor
  // that does have one.
  //
  // `clip` is the accumulated intersection of every scrollable ancestor's
  // viewport seen so far — a point outside it means whatever's physically
  // there has scrolled out of view, so it can't be hit no matter what its
  // own box says. Only scrollable nodes narrow the clip further as we
  // descend, exactly mirroring how renderView() decides what to clip.
  static View *hitTestFlow(View &v, float x, float y, ClipRect clip,
                           MouseButton btn) {
    if (v.style.visibility == Visibility::Hidden)
      return nullptr;
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return nullptr;
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          it->style.display == Display::None)
        continue;
      if (View *hit = hitTestFlow(*it, x, y, childClip, btn))
        return hit;
    }
    return hasButtonHandler(v, btn) ? &v : nullptr;
  }

  // Top-level hit test against the whole tree: absolutes take priority
  // over the flow tree, highest zIndex/latest doc-order first, mirroring
  // paint order (collectAbsolutes + sortAbsolutes are the same lists used
  // to paint on Windows/Linux). Absolutes are tested unclipped — see the
  // "known limitation" note on resolveScrollTarget() above.
  View *hitTest(float x, float y, MouseButton btn = MouseButton::Left) {
    if (!hasRoot_)
      return nullptr;
    std::vector<AbsoluteEntry> absolutes;
    collectAbsolutes(root_, absolutes);
    sortAbsolutes(absolutes);
    for (auto it = absolutes.rbegin(); it != absolutes.rend(); ++it)
      if (View *hit =
              hitTestFlow(const_cast<View &>(*it->view), x, y, ClipRect{}, btn))
        return hit;
    return hitTestFlow(root_, x, y, ClipRect{}, btn);
  }

  // Same bubbling shape as hitTestFlow, but matches on onScrollUp/
  // onScrollDown instead of click/press handlers — kept as a separate
  // walk (rather than folding into hasButtonHandler) since scroll
  // notches aren't a MouseButton and can coexist with a view that also
  // has click handlers.
  static View *hitTestScroll(View &v, float x, float y, ClipRect clip,
                             bool up) {
    if (v.style.visibility == Visibility::Hidden)
      return nullptr;
    if (!clip.contains(x, y) || !containsPoint(v, x, y))
      return nullptr;
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto it = v.children.rbegin(); it != v.children.rend(); ++it) {
      if (it->style.position == Position::Absolute ||
          it->style.display == Display::None)
        continue;
      if (View *hit = hitTestScroll(*it, x, y, childClip, up))
        return hit;
    }
    bool has = up ? (bool)v.onScrollUp : (bool)v.onScrollDown;
    return has ? &v : nullptr;
  }

  // Polls every dynamic source in the subtree rooted at v, writing
  // changes back in place and marking the affected node dirty. Called
  // after a click is dispatched, since the handler may have mutated the
  // plain variables these sources read from. Returns whether anything
  // changed, so the caller knows whether to relayout/repaint.
  static bool checkForUpdates(View &v) {
    bool changed = false;

    if (auto *fn = std::get_if<std::function<std::string()>>(&v.text)) {
      std::string next = (*fn)();
      if (next != v.computed.resolvedText) {
        v.computed.resolvedText = std::move(next);
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<bool()>>(&v.disabled)) {
      bool next = (*fn)();
      if (next != v.computed.resolvedDisabled) {
        v.computed.resolvedDisabled = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Size()>>(&v.style.width)) {
      Size next = (*fn)();
      Size &cur = v.computed.resolvedWidth;
      if (next.kind != cur.kind || next.value != cur.value) {
        cur = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (auto *fn = std::get_if<std::function<Size()>>(&v.style.height)) {
      Size next = (*fn)();
      Size &cur = v.computed.resolvedHeight;
      if (next.kind != cur.kind || next.value != cur.value) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.borderWidth)) {
      float next = (*fn)();
      if (next != v.computed.resolvedBorderWidth) {
        v.computed.resolvedBorderWidth = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<Color()>>(&v.style.borderColor)) {
      Color next = (*fn)();
      Color &cur = v.computed.resolvedBorderColor;
      if (next.r != cur.r || next.g != cur.g || next.b != cur.b ||
          next.a != cur.a) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn = std::get_if<std::function<float()>>(&v.style.borderRadius)) {
      float next = (*fn)();
      if (next != v.computed.resolvedBorderRadius) {
        v.computed.resolvedBorderRadius = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (v.positionSource) {
      float next = v.positionSource();
      if (next != v.style.left) {
        v.style.left = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (v.topSource) {
      float next = v.topSource();
      if (next != v.style.top) {
        v.style.top = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (auto *fn =
            std::get_if<std::function<Color()>>(&v.style.backgroundColor)) {
      Color next = (*fn)();
      Color &cur = v.computed.resolvedBackgroundColor;
      if (next.r != cur.r || next.g != cur.g || next.b != cur.b ||
          next.a != cur.a) {
        cur = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (v.displaySource) {
      Display next = v.displaySource() ? Display::Flex : Display::None;
      if (next != v.style.display) {
        v.style.display = next;
        v.computed.dirty = true;
        changed = true; // affects layout — relayout() picked up by caller
      }
    }
    if (v.visibilitySource) {
      Visibility next =
          v.visibilitySource() ? Visibility::Visible : Visibility::Hidden;
      if (next != v.style.visibility) {
        v.style.visibility = next;
        v.computed.dirty = true;
        changed = true;
      }
    }
    if (v.canvasDirtySource && v.canvasDirtySource()) {
      v.computed.canvasNeedsRedraw = true;
      v.computed.dirty = true;
      changed = true;
    }
    for (auto &c : v.children)
      changed |= checkForUpdates(c);
    return changed;
  }

  // Updates v's (and its descendants') isHovered flag based on (x, y),
  // mirroring hitTestFlow's clip-aware descent so a node scrolled out of
  // view is never marked hovered. Returns whether any flag actually
  // flipped, so callers only repaint when hover state visibly changes.
  static bool updateHover(View &v, float x, float y, ClipRect clip) {
    if (v.style.visibility == Visibility::Hidden) {
      bool changed = v.computed.isHovered;
      v.computed.isHovered = false; // scrolled-out/None already relied on
                                    // this same "force false" idiom
      return changed;
    }
    bool inside = clip.contains(x, y) && containsPoint(v, x, y);
    bool changed = false;
    if (inside != v.computed.isHovered) {
      v.computed.isHovered = inside;
      if (v.style.hoverColor)
        v.computed.dirty = true;
      changed = true;
    }
    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (auto &c : v.children)
      changed |= updateHover(c, x, y, childClip);
    return changed;
  }

  // Invokes v's click handler for `btn` if it has one; no-op for
  // nullptr, a disabled view, or an unset handler.
  static void dispatchClick(View *v, MouseButton btn) {
    if (!v || resolveDynamic(v->disabled, v->computed.resolvedDisabled))
      return;
    switch (btn) {
    case MouseButton::Left:
      if (v->onClick)
        v->onClick();
      break;
    case MouseButton::Middle:
      if (v->onMiddleClick)
        v->onMiddleClick();
      break;
    case MouseButton::Right:
      if (v->onRightClick)
        v->onRightClick();
      break;
    }
  }

  // Fires onScrollUp/onScrollDown on the topmost view under (x, y) that
  // declares one, via hitTestScroll. Independent of applyWheelScroll's
  // pixel-based content scrolling, so a plain (non-overflow) view can
  // still react to the wheel. `notches` > 0 means wheel-up. Returns
  // whether a handler fired (caller should repaint) — poll/relayout for
  // any state the handler mutated, same idiom as endPress/updateDrag.
  bool dispatchScroll(float x, float y, float notches) {
    if (notches == 0.0f || !hasRoot_)
      return false;
    bool up = notches > 0;
    View *v = hitTestScroll(root_, x, y, ClipRect{}, up);
    if (!v)
      return false;
    if (up) {
      if (v->onScrollUp)
        v->onScrollUp();
    } else {
      if (v->onScrollDown)
        v->onScrollDown();
    }
    if (checkForUpdates(root_))
      relayout();
    return true;
  }

  // Tracks which view (if any) most recently received a press of each
  // button, so a click only fires if the matching release lands back on
  // the same view — ordinary UI click semantics: press, drag off,
  // release elsewhere cancels; press and release both on the button
  // fires it. Indexed by MouseButton so an independent left/middle/right
  // press-release pairing can be in flight at once without clobbering
  // each other.
  View *pressedView_[3] = {nullptr, nullptr, nullptr};

  // The view beginPress() found to have the relevant onXDragTo handler
  // for that button, if any — kept separate from pressedView_ (which
  // exists purely for click press/release pairing) so a click and a
  // drag can be tracked independently. Cleared on release/capture-loss
  // the same way pressedView_ is. Also indexed by MouseButton.
  View *dragView_[3] = {nullptr, nullptr, nullptr};

  static int btnIdx(MouseButton b) { return static_cast<int>(b); }

  // Records the view under (x, y) as the pending click's press target,
  // for whichever button was pressed.
  void beginPress(float x, float y, MouseButton btn = MouseButton::Left) {
    int i = btnIdx(btn);
    View *hit = hitTest(x, y, btn);
    pressedView_[i] = hit;
    if (hit) {
      float lx = x - hit->computed.x, ly = y - hit->computed.y;
      switch (btn) {
      case MouseButton::Left:
        if (hit->onPressAt)
          hit->onPressAt(lx, ly);
        break;
      case MouseButton::Middle:
        if (hit->onMiddlePressAt)
          hit->onMiddlePressAt(lx, ly);
        break;
      case MouseButton::Right:
        if (hit->onRightPressAt)
          hit->onRightPressAt(lx, ly);
        break;
      }
    }
    bool hasDrag =
        hit && (btn == MouseButton::Left     ? (bool)hit->onDragTo
                : btn == MouseButton::Middle ? (bool)hit->onMiddleDragTo
                                             : (bool)hit->onRightDragTo);
    dragView_[i] = hasDrag ? hit : nullptr;
  }

  // Call on every pointer-motion event while a button is held. Advances
  // an in-progress onDragTo drag (armed by beginPress above) by
  // re-invoking the handler with the current point translated into
  // dragView_'s own local coordinates, then polling/applying whatever
  // state the handler changed — same checkForUpdates+relayout idiom used
  // everywhere else dynamic sources are re-read. A no-op, and cheap,
  // when no drag is active.
  bool updateDrag(float x, float y, MouseButton btn = MouseButton::Left) {
    View *v = dragView_[btnIdx(btn)];
    if (!v)
      return false;
    float lx = x - v->computed.x, ly = y - v->computed.y;
    switch (btn) {
    case MouseButton::Left:
      v->onDragTo(lx, ly);
      break;
    case MouseButton::Middle:
      v->onMiddleDragTo(lx, ly);
      break;
    case MouseButton::Right:
      v->onRightDragTo(lx, ly);
      break;
    }
    if (!hasRoot_)
      return false;
    if (checkForUpdates(root_)) {
      relayout();
      return true;
    }
    return false;
  }

  // Advances any of the three buttons' drags at once — motion events
  // don't carry "which button", so this just tries all three; each is a
  // no-op unless that button's drag is actually in progress.
  bool updateAllDrags(float x, float y) {
    bool changed = false;
    changed |= updateDrag(x, y, MouseButton::Left);
    changed |= updateDrag(x, y, MouseButton::Middle);
    changed |= updateDrag(x, y, MouseButton::Right);
    return changed;
  }

  // Completes a pending press: fires pressedView_'s onClick only if the
  // release also landed on that same view, then clears the pending state
  // unconditionally (a press that never resolves shouldn't linger and
  // affect some later, unrelated release).
  bool endPress(float x, float y, MouseButton btn = MouseButton::Left) {
    int i = btnIdx(btn);
    View *released = hitTest(x, y, btn);
    if (released && released == pressedView_[i])
      dispatchClick(released, btn);
    pressedView_[i] = nullptr;
    dragView_[i] = nullptr;
    if (!hasRoot_)
      return false;
    if (checkForUpdates(root_)) {
      relayout();
      return true;
    }
    return false;
  }

// Windows-only member/method block.
#if defined(_WIN32)
  // Native window handle; null until CreateWindowExW succeeds.
  HWND hwnd_ = nullptr;

  // Device-independent: created once in the constructor, lives for the
  // process (well, the LiteUI instance's) lifetime.
  ID2D1Factory *d2dFactory_ = nullptr;

  // Device-dependent: bound to hwnd_'s current size. Torn down and
  // recreated if EndDraw() ever reports D2DERR_RECREATE_TARGET (e.g. after
  // a display driver reset) — everything that draws goes through this
  // pointer, never a raw HDC.
  ID2D1HwndRenderTarget *renderTarget_ = nullptr;

  // Lazily (re)creates renderTarget_ against hwnd_'s current client size.
  // A no-op once a valid target already exists; WM_SIZE calls Resize()
  // directly instead of tearing this down, so this only actually runs
  // once per (factory, hwnd) pair unless EndDraw() invalidates the target.
  void ensureRenderTarget() {
    if (renderTarget_)
      return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_SIZE_U size =
        D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(1, rc.right - rc.left)),
                    static_cast<UINT32>(std::max<LONG>(1, rc.bottom - rc.top)));
    HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_, size), &renderTarget_);
    if (FAILED(hr))
      throw std::runtime_error("CreateHwndRenderTarget failed");
  }

  // Converts our own Color into the D2D1::ColorF Direct2D brushes want.
  static D2D1::ColorF toD2DColor(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
  }

  // Draws a filled rectangle in one shot: create brush, fill, release.
  // Mirrors the old gdiFillRect's create-use-delete pattern 1:1.
  static void d2dFillRect(ID2D1RenderTarget *rt, float x, float y, float w,
                          float h, Color c) {
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(c), &brush);
    if (brush) {
      rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush);
      brush->Release();
    }
  }

  // The Win32 window procedure: Windows calls this for every message sent to
  // hwnd.
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // Will hold the LiteUI instance associated with this hwnd, if any.
    LiteUI *self = nullptr;

    // WM_NCCREATE arrives before any other message and carries the creation
    // parameters.
    if (msg == WM_NCCREATE) {
      // WM_NCCREATE is the very first message a window receives, sent
      // during CreateWindowExW itself, before the window is usable.
      // Reinterpret the message's lParam as the CREATESTRUCTW Windows built for
      // us.
      auto cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      // Recover the `this` pointer we passed as the lpParam argument to
      // CreateWindowExW.
      self = static_cast<LiteUI *>(cs->lpCreateParams);
      // Stash that pointer in the window's user-data slot so future messages
      // can retrieve it.
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    // For every other message, the pointer was already stored by the
    // WM_NCCREATE branch above.
    else {
      // Fetch the previously stored `this` pointer back out of the window's
      // user-data slot.
      self = reinterpret_cast<LiteUI *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // Dispatch on the specific message type.
    switch (msg) {
    // Repaint request: paint everything into the off-screen back buffer,
    // then blit it to the screen in a single BitBlt — see memDC_'s comment
    // for why (this is the actual flicker fix).
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      if (self) {
        self->ensureRenderTarget();
        self->renderTarget_->BeginDraw();
        // Plain white background first — WM_ERASEBKGND below tells
        // Windows not to do this for us anymore, so we own it, matching
        // what the Linux/Wayland renderer already does for its content
        // area.
        self->renderTarget_->Clear(D2D1::ColorF(D2D1::ColorF::White));
        self->paintBoxes(self->renderTarget_);
        self->paintRoot(self->renderTarget_);
        HRESULT hr = self->renderTarget_->EndDraw();
        // D2DERR_RECREATE_TARGET means the underlying device is gone
        // (driver reset, GPU removal, etc.) — drop the target so the next
        // WM_PAINT's ensureRenderTarget() rebuilds it from scratch.
        if (hr == D2DERR_RECREATE_TARGET) {
          self->renderTarget_->Release();
          self->renderTarget_ = nullptr;
        }
      }
      EndPaint(hwnd, &ps);
      return 0;
    }

    // Tells Windows we're handling the background ourselves (see
    // WM_PAINT), so it should skip its own default erase-to-brush pass —
    // that default erase is what caused a visible white/gray flash right
    // before every repaint.
    case WM_ERASEBKGND:
      return 1;

    // Left mouse button pressed: first give scrollbars/scrollable content a
    // chance to claim the press (beginScrollPress) — a thumb grab or track
    // click consumes it entirely; a press over plain scrollable content
    // arms a possible pan but still falls through to the ordinary
    // beginPress() below, since a small movement should still resolve as a
    // click (see updateScrollDrag/endScrollPress). Capture the mouse so we
    // still get the matching WM_MOUSEMOVE/WM_LBUTTONUP even if the cursor
    // leaves the window before the button is released.
    case WM_LBUTTONDOWN: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (!self->beginScrollPress(x, y))
          self->beginPress(x, y);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Pointer moved with a button held: advances an in-progress scrollbar
    // drag or content pan. No-op (returns false) if neither is active, so
    // this costs nothing on ordinary hover.
    case WM_MOUSEMOVE: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        bool changed = self->updateScrollDrag(x, y);
        if (self->updateAllDrags(x, y))
          changed = true;
        if (self->hasRoot_ &&
            LiteUI::updateHover(self->root_, x, y, ClipRect{}))
          changed = true;
        if (changed && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Left mouse button released: resolve the pending press against
    // whatever's under the cursor now, firing onClick only if it matches
    // the original press target (endScrollPress handles the scroll-drag
    // side of this and defers to endPress() when a pan never actually
    // moved, i.e. it was really just a click).
    case WM_LBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endScrollPress(x, y) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    // Middle/right buttons don't interact with scrollbars — that's a
    // left-drag convention — so these go straight through the ordinary
    // press/click path.
    case WM_MBUTTONDOWN: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->beginPress(x, y, MouseButton::Middle);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    case WM_MBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endPress(x, y, MouseButton::Middle) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    case WM_RBUTTONDOWN: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        self->beginPress(x, y, MouseButton::Right);
        SetCapture(hwnd);
        if (self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    case WM_RBUTTONUP: {
      if (self) {
        float x = static_cast<float>(static_cast<short>(LOWORD(lp)));
        float y = static_cast<float>(static_cast<short>(HIWORD(lp)));
        if (self->endPress(x, y, MouseButton::Right) && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      ReleaseCapture();
      return 0;
    }

    // Vertical mouse-wheel rotation. WM_MOUSEWHEEL's cursor coordinates are
    // in *screen* space (unlike every other mouse message here, which are
    // client-space) — ScreenToClient converts before hit-testing. One
    // notch (WHEEL_DELTA = 120) scrolls a fixed 40px step; larger/precision
    // wheels report multiples/fractions of that.
    case WM_MOUSEWHEEL: {
      if (self) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        // Wheel-up (positive notches) should scroll content up, i.e.
        // decrease scrollY — hence the negation.
        bool changed = self->applyWheelScroll(static_cast<float>(pt.x),
                                              static_cast<float>(pt.y), 0.0f,
                                              -notches * 40.0f);
        if (self->dispatchScroll(static_cast<float>(pt.x),
                                 static_cast<float>(pt.y), notches))
          changed = true;
        if (changed && self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Horizontal mouse-wheel rotation (tilt-wheel or shift+wheel on most
    // drivers). Same coordinate/notch handling as WM_MOUSEWHEEL, but
    // positive notches scroll right, so no negation here.
    case WM_MOUSEHWHEEL: {
      if (self) {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float notches =
            static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        if (self->applyWheelScroll(static_cast<float>(pt.x),
                                   static_cast<float>(pt.y), notches * 40.0f,
                                   0.0f) &&
            self->hwnd_)
          InvalidateRect(self->hwnd_, nullptr, FALSE);
      }
      return 0;
    }

    // Capture was taken away from us mid-press (e.g. alt-tab, a system
    // dialog popping up) — the click/drag can't complete normally, so drop
    // both the pending click and any in-progress scroll drag rather than
    // let a later, unrelated event resolve them.
    case WM_CAPTURECHANGED:
      if (self) {
        for (int i = 0; i < 3; ++i) {
          self->pressedView_[i] = nullptr;
          self->dragView_[i] = nullptr;
        }
        self->scrollDrag_ = {};
      }
      return 0;

    // Window was resized (including maximize/restore/snap): update our
    // stored dimensions and re-run layout against the new size. GDI needs
    // no buffer reallocation (it paints straight into the window's DC), so
    // this is just relayout + repaint.
    case WM_SIZE: {
      if (self) {
        self->width_ = LOWORD(lp);
        self->height_ = HIWORD(lp);
        if (self->renderTarget_)
          self->renderTarget_->Resize(
              D2D1::SizeU(static_cast<UINT32>(self->width_),
                          static_cast<UINT32>(self->height_)));
        self->relayout();
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    // When the window is being destroyed...
    case WM_DESTROY:
      // ...tell Windows to post a WM_QUIT message, which ends the GetMessage
      // loop in run().
      PostQuitMessage(0);
      // Indicate this message was fully handled.
      return 0;
    }
    // Any message not explicitly handled above falls through to Windows'
    // default behavior.
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // Draws every queued box into the render target.
  void paintBoxes(ID2D1RenderTarget *rt) {
    for (const auto &b : boxes_)
      d2dFillRect(rt, static_cast<float>(b.pos_x), static_cast<float>(b.pos_y),
                  static_cast<float>(b.width), static_cast<float>(b.height),
                  b.color);
  }

  // Draws the layout tree (if any). Border-radius is handled natively by
  // D2D1_ROUNDED_RECT — no manual pixel math needed on this platform,
  // same as the old RoundRect approach, just anti-aliased for free.
  void paintRoot(ID2D1RenderTarget *rt) {
    if (hasRoot_) {
      paintView(rt, root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      sortAbsolutes(absolutes);
      for (const auto &e : absolutes)
        paintView(rt, *e.view, ClipRect{});
    }
  }

  // Draws v's vertical/horizontal scrollbar (whichever are currently
  // showing) using flat fills — same classic fixed track+thumb styling as
  // before, unclipped by the content clip beyond whatever `clip` already
  // restricts (a scrollbar always sits fully within its own view's box,
  // which is itself already visible or this function wouldn't have been
  // reached).
  void paintScrollbars(ID2D1RenderTarget *rt, const View &v) {
    if (wantVBar(v)) {
      PixRect t = vTrackRect(v), th = vThumbRect(v);
      d2dFillRect(rt, t.x, t.y, t.w, t.h, {0xE0, 0xE0, 0xE0});
      d2dFillRect(rt, th.x, th.y, th.w, th.h, {0x90, 0x90, 0x90});
    }
    if (wantHBar(v)) {
      PixRect t = hTrackRect(v), th = hThumbRect(v);
      d2dFillRect(rt, t.x, t.y, t.w, t.h, {0xE0, 0xE0, 0xE0});
      d2dFillRect(rt, th.x, th.y, th.w, th.h, {0x90, 0x90, 0x90});
    }
    // Corner filler where both bars would otherwise leave a gap/overlap.
    if (wantVBar(v) && wantHBar(v))
      d2dFillRect(rt, v.computed.x + v.computed.w - kScrollbarThickness,
                  v.computed.y + v.computed.h - kScrollbarThickness,
                  kScrollbarThickness, kScrollbarThickness, {0xE0, 0xE0, 0xE0});
  }

  // Rebuilds v.computed.textLayout if it's missing or was built for a
  // different final width — a resize/relayout can change how the text
  // wraps even with no string/style change. `v` is const here (called
  // from paintView), which is exactly why the cache fields are mutable.
  static void ensureTextLayout(const View &v) {
    const std::string &text = resolveDynamic(v.text, v.computed.resolvedText);
    if (v.computed.textLayout &&
        v.computed.textLayoutBuiltForWidth == v.computed.w &&
        v.computed.textLayoutBuiltForText == text)
      return;
    if (v.computed.textLayout) {
      v.computed.textLayout->Release();
      v.computed.textLayout = nullptr;
    }
    float innerW = std::max(0.0f, v.computed.w - v.style.padding.left -
                                      v.style.padding.right);
    float innerH = std::max(0.0f, v.computed.h - v.style.padding.top -
                                      v.style.padding.bottom);
    v.computed.textLayout =
        liteui_text::makeLayout(text, v.textStyle, innerW, innerH);
    v.computed.textLayoutBuiltForWidth = v.computed.w;
    v.computed.textLayoutBuiltForText = text;
  }

  void paintText(ID2D1RenderTarget *rt, const View &v) {
    ensureTextLayout(v);
    ID2D1SolidColorBrush *brush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(v.textStyle.color), &brush);
    if (brush) {
      float x = v.computed.x + v.style.padding.left;
      float y = v.computed.y + v.style.padding.top;
      rt->DrawTextLayout(D2D1::Point2F(x, y), v.computed.textLayout, brush,
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
      brush->Release();
    }
  }

  // (Re)builds v.computed.canvasTarget — an offscreen bitmap render
  // target sized to v's inner (padding-excluded) content box — whenever
  // that size has changed since it was last built, then (re)invokes
  // v.onPaint into it whenever the size changed OR the app requested a
  // redraw via View::requestCanvasRedraw() (canvasNeedsRedraw starts
  // true, so this always happens at least once). A plain repaint with
  // neither condition true is a no-op: the previous frame's bitmap is
  // simply reused.
  void ensureCanvasTarget(const View &v) {
    float innerW = std::max(1.0f, v.computed.w - v.style.padding.left -
                                      v.style.padding.right);
    float innerH = std::max(1.0f, v.computed.h - v.style.padding.top -
                                      v.style.padding.bottom);
    bool resized = !v.computed.canvasTarget ||
                   v.computed.canvasBuiltForWidth != innerW ||
                   v.computed.canvasBuiltForHeight != innerH;
    if (resized) {
      if (v.computed.canvasTarget) {
        v.computed.canvasTarget->Release();
        v.computed.canvasTarget = nullptr;
      }
      renderTarget_->CreateCompatibleRenderTarget(D2D1::SizeF(innerW, innerH),
                                                  &v.computed.canvasTarget);
      v.computed.canvasBuiltForWidth = innerW;
      v.computed.canvasBuiltForHeight = innerH;
      v.computed.canvasNeedsRedraw = true;
    }
    if (!v.computed.canvasTarget)
      return;
    if (v.computed.canvasNeedsRedraw && v.onPaint) {
      v.computed.canvasTarget->BeginDraw();
      v.computed.canvasTarget->Clear(D2D1::ColorF(0, 0, 0, 0));
      CanvasContext ctx(v.computed.canvasTarget, innerW, innerH);
      v.onPaint(ctx);
      v.computed.canvasTarget->EndDraw();
      v.computed.canvasNeedsRedraw = false;
    }
  }

  // Blits v's cached canvas bitmap into the window's own render target at
  // v's final on-screen position (inset by padding, same convention as
  // paintText's x/y).
  void paintCanvas(ID2D1RenderTarget *rt, const View &v) {
    ensureCanvasTarget(v);
    if (!v.computed.canvasTarget)
      return;
    ID2D1Bitmap *bmp = nullptr;
    v.computed.canvasTarget->GetBitmap(&bmp);
    if (!bmp)
      return;
    float x = v.computed.x + v.style.padding.left;
    float y = v.computed.y + v.style.padding.top;
    D2D1_SIZE_F sz = bmp->GetSize();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    rt->DrawBitmap(bmp, D2D1::RectF(x, y, x + sz.width, y + sz.height));
    bmp->Release();
  }

  // `clip` is the accumulated visible region from scrollable ancestors.
  // Direct2D's clip stack is push/pop rather than GDI's set-and-restore,
  // but since this function is itself called recursively (one call frame
  // per View), pushing on entry and popping on exit naturally nests
  // correctly with the call tree — no need to save/restore a previous
  // clip handle the way SelectClipRgn did.
  void paintView(ID2D1RenderTarget *rt, const View &v, ClipRect clip) {
    if (v.style.visibility == Visibility::Hidden)
      return; // space already reserved by layout; just don't draw it
    const Style &s = v.style;
    float x = v.computed.x, y = v.computed.y, w = v.computed.w,
          h = v.computed.h;

    bool clipped = clip.x0 != -std::numeric_limits<float>::infinity() ||
                   clip.y0 != -std::numeric_limits<float>::infinity() ||
                   clip.x1 != std::numeric_limits<float>::infinity() ||
                   clip.y1 != std::numeric_limits<float>::infinity();
    if (clipped)
      rt->PushAxisAlignedClip(D2D1::RectF(clip.x0, clip.y0, clip.x1, clip.y1),
                              D2D1_ANTIALIAS_MODE_ALIASED);

    // Text leaves never paint their own background/border — Text::style is
    // layout-only (see the struct's comment); a solid backgroundColor here
    // would otherwise paint an opaque box under the glyphs on every text
    // node, since Style::backgroundColor defaults to opaque white rather
    // than "none" (Color has no alpha channel to express transparent).
    if (v.isText) {
      paintText(rt, v);
      if (clipped)
        rt->PopAxisAlignedClip();
      return;
    }

    float borderRadius = resolveDynamic(s.borderRadius, v.computed.resolvedBorderRadius);
    D2D1_ROUNDED_RECT rr = {D2D1::RectF(x, y, x + w, y + h), borderRadius,
                            borderRadius};
    Color bg = (v.computed.isHovered && s.hoverColor)
                   ? *s.hoverColor
                   : resolveDynamic(s.backgroundColor,
                                    v.computed.resolvedBackgroundColor);
    ID2D1SolidColorBrush *bgBrush = nullptr;
    rt->CreateSolidColorBrush(toD2DColor(bg), &bgBrush);
    if (bgBrush) {
      rt->FillRoundedRectangle(rr, bgBrush);
      bgBrush->Release();
    }
    float borderWidth = resolveDynamic(s.borderWidth, v.computed.resolvedBorderWidth);
    if (borderWidth > 0) {
      ID2D1SolidColorBrush *borderBrush = nullptr;
      rt->CreateSolidColorBrush(
          toD2DColor(resolveDynamic(s.borderColor, v.computed.resolvedBorderColor)),
          &borderBrush);
      if (borderBrush) {
        rt->DrawRoundedRectangle(rr, borderBrush, borderWidth);
        borderBrush->Release();
      }
    }

    // A canvas node paints its own Style background/border like any
    // other box (above), then has its cached onPaint bitmap blitted on
    // top — unlike a text leaf, which skips background/border entirely.
    // Canvas nodes aren't expected to have children, so control falls
    // through to the (harmless, normally no-op) children loop below.
    if (v.isCanvas)
      paintCanvas(rt, v);

    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute &&
          child.style.display != Display::None)
        paintView(rt, child, childClip);

    // Scrollbars are drawn after children, still under v's own (ancestor,
    // not child-narrowed) clip — we haven't popped it yet, so this is
    // still exactly `clip`, matching the old GDI re-selection semantics.
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them on top the way an overlay
    // scrollbar should be.
    if (v.scrollsX() || v.scrollsY())
      paintScrollbars(rt, v);

    if (clipped)
      rt->PopAxisAlignedClip();
  }

#else // Linux / Wayland

  wl_display *display_ =
      nullptr; // Connection handle to the Wayland compositor; null until
               // wl_display_connect succeeds.
  wl_compositor *compositor_ =
      nullptr; // The compositor global, used to create surfaces.
  xdg_wm_base *wm_base_ = nullptr;  // The xdg-shell global, used to turn a raw
                                    // surface into a desktop window.
  wl_surface *surface_ = nullptr;   // The raw drawable surface for this window.
  xdg_surface *xdg_surf_ = nullptr; // The xdg_surface wrapping surface_, needed
                                    // for configure/ack handshakes.
  xdg_toplevel *toplevel_ =
      nullptr; // The xdg_toplevel representing this as a top-level
               // (movable/resizable/closable) window.
  wl_shm *shm_ =
      nullptr; // The shared-memory global used to allocate pixel buffers.

  wl_seat *seat_ = nullptr; // The seat global, representing one user's set of
                            // input devices (keyboard/mouse/etc.).

  wl_pointer *pointer_ =
      nullptr; // The pointer (mouse) device obtained from
               // the seat, once it announces pointer capability.
  wl_cursor_theme *cursorTheme_ =
      nullptr; // Loaded cursor theme (arrow, resize handles, etc.), used to
               // look up the pixel images for wl_pointer_set_cursor.
  wl_surface *cursorSurface_ =
      nullptr; // Dedicated surface that holds whichever cursor image is
               // currently active; the compositor renders this at the
               // pointer position once we call wl_pointer_set_cursor.
  uint32_t pointerEnterSerial_ =
      0; // Serial from the most recent pointer-enter event;
         // wl_pointer_set_cursor requires one and it's not resent on motion, so
         // we cache it.
  std::string currentCursorName_; // Name of the cursor image currently shown,
                                  // so we don't reissue set_cursor every
                                  // single motion event for no reason.

  zxdg_decoration_manager_v1 *decoration_manager_ =
      nullptr; // Global for negotiating who draws window decorations (server vs
               // client).

  zxdg_toplevel_decoration_v1 *toplevel_decoration_ =
      nullptr; // Per-window decoration-mode object obtained from
               // decoration_manager_.

  // software rendered CSD titlebar. No cairo here, so buttons are drawn with
  // raw pixel writes straight into the shm buffer.

  static constexpr int kTitlebarHeight =
      32; // Height in pixels reserved at the top of the window for the custom
          // titlebar.

  static constexpr int kButtonSize =
      18; // Width/height in pixels of each titlebar button's square hitbox.

  static constexpr int kButtonMargin =
      8; // Horizontal gap in pixels between buttons and between the last button
         // and the window edge.
  // Raw Linux input-event code for the left mouse button (from
  // linux/input-event-codes.h).
  static constexpr uint32_t BTN_LEFT_CODE = 0x110;  // linux/input-event-codes.h
  static constexpr uint32_t BTN_RIGHT_CODE = 0x111; // linux/input-event-codes.h
  static constexpr uint32_t BTN_MIDDLE_CODE =
      0x112; // linux/input-event-codes.h

  double pointer_x_ = 0,
         pointer_y_ = 0; // Last known pointer position within
                         // the surface, in surface-local coordinates.

  bool maximized_ =
      false; // Tracks whether the window currently believes
             // itself to be maximized (for toggling and icon state).

  // ---- EGL / GLES2 ----
  // GPU-accelerated replacement for the old wl_shm double buffer. The
  // compositor owns presentation timing via eglSwapBuffers (backed by
  // wayland-egl's own internal buffer queue), so there's no manual
  // busy-tracking/retry logic to maintain anymore — swap is synchronous
  // from our point of view.
  wl_egl_window *eglWindow_ = nullptr;
  EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
  EGLContext eglContext_ = EGL_NO_CONTEXT;
  EGLSurface eglSurface_ = EGL_NO_SURFACE;
  bool eglReady_ = false; // true once initEgl() has completed

  GLuint rectProgram_ = 0, flatProgram_ = 0;
  GLuint texProgram_ = 0;
  GLuint canvasProgram_ = 0; // samples RGBA directly, no alpha-tint (unlike
                             // texProgram_'s glyph-coverage tinting)
  GLuint quadVbo_ = 0; // static unit quad, reused by every rectProgram_ draw
  GLuint flatVbo_ = 0; // rewritten per-call for flatProgram_ (line quads)
  GLint rectAPos_ = -1, rectUPos_ = -1, rectUSize_ = -1, rectUScreen_ = -1,
        rectURadius_ = -1, rectUColor_ = -1;
  GLint flatAPos_ = -1, flatUScreen_ = -1, flatUColor_ = -1;

  GLint texAPos_ = -1, texUPos_ = -1, texUSize_ = -1, texUScreen_ = -1,
        texUTex_ = -1, texUColor_ = -1;
  GLint canvasAPos_ = -1, canvasUPos_ = -1, canvasUSize_ = -1,
        canvasUScreen_ = -1, canvasUTex_ = -1;

  bool configured_ =
      false; // Set true once the compositor has sent its first
             // configure event, meaning we're allowed to attach a buffer.
  int pendingWidth_ = 0;  // Size most recently suggested by
  int pendingHeight_ = 0; // toplevelConfigure; 0 means "no suggestion yet"
                          // (the compositor may send 0x0 to mean "you decide").
  bool running_ = true;   // Controls the event loop in run(); set false to
                          // request a clean exit.

  // ---- GLES2 rendering ----
  // Two tiny shader programs cover everything this UI draws:
  //  - rectProgram_: an axis-aligned box with an optional rounded-corner
  //    radius, via a signed-distance test in the fragment shader. Used for
  //    all backgrounds/borders/scrollbars (radius 0 == a plain rect).
  //  - flatProgram_: an arbitrary pixel-space quad with a flat color. Used
  //    only for drawLine's thick-line quads (titlebar icons).
  static constexpr const char *kRectVS = R"(
    precision mediump float;
    attribute vec2 a_pos;      // unit quad, 0..1
    uniform vec2 u_rectPos;    // top-left, window pixels
    uniform vec2 u_rectSize;   // pixels
    uniform vec2 u_screen;     // window size, pixels
    varying vec2 v_local;      // pixel offset within the rect
    void main() {
      v_local = a_pos * u_rectSize;
      vec2 px = u_rectPos + a_pos * u_rectSize;
      gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,
                         1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kRectFS = R"(
    precision mediump float;
    varying vec2 v_local;
    uniform vec2 u_rectSize;
    uniform float u_radius;
    uniform vec4 u_color;
    void main() {
      vec2 half_ = u_rectSize * 0.5;
      vec2 d = abs(v_local - half_) - (half_ - u_radius);
      float dist = length(max(d, 0.0)) - u_radius;
      float alpha = 1.0 - smoothstep(-1.0, 1.0, dist);
      gl_FragColor = vec4(u_color.rgb, u_color.a * alpha);
    }
  )";
  static constexpr const char *kFlatVS = R"(
    precision mediump float;
    attribute vec2 a_pos;   // window pixels, explicit per-vertex
    uniform vec2 u_screen;
    void main() {
      gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0,
                         1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kFlatFS = R"(
    precision mediump float;
    uniform vec4 u_color;
    void main() { gl_FragColor = u_color; }
  )";

  // Draws a cached glyph-coverage texture (see ensureTextTexture) tinted
  // by u_color. The texture stores coverage in its alpha channel only —
  // rasterized once in plain white so the same texture can be recolored
  // without re-rendering glyphs.
  static constexpr const char *kTexVS = R"(
    precision mediump float;
    attribute vec2 a_pos;   // unit quad, 0..1 — doubles as UV
    uniform vec2 u_pos;
    uniform vec2 u_size;
    uniform vec2 u_screen;
    varying vec2 v_uv;
    void main() {
      v_uv = a_pos;
      vec2 px = u_pos + a_pos * u_size;
      gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,
                         1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);
    }
  )";
  static constexpr const char *kTexFS = R"(
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    uniform vec4 u_color;
    void main() {
      float a = texture2D(u_tex, v_uv).a;
      gl_FragColor = vec4(u_color.rgb, u_color.a * a);
    }
  )";

  // Draws a Canvas node's cached backing texture (see ensureCanvasSurface).
  // Unlike kTexFS (which stores only glyph coverage in alpha and tints it
  // with a separate u_color), a canvas texture already carries real RGBA
  // color data straight out of Cairo, so this simply samples it as-is —
  // reuses kTexVS unchanged since the vertex stage (position a unit quad,
  // pass through UV) is identical.
  static constexpr const char *kCanvasFS = R"(
    precision mediump float;
    varying vec2 v_uv;
    uniform sampler2D u_tex;
    void main() {
      gl_FragColor = texture2D(u_tex, v_uv);
    }
  )";

  static GLuint compileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof(log), nullptr, log);
      fprintf(stderr, "shader compile error: %s\n", log);
      throw std::runtime_error("GLSL shader compile failed");
    }
    return s;
  }
  static GLuint linkProgram(const char *vs, const char *fs) {
    GLuint v = compileShader(GL_VERTEX_SHADER, vs);
    GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[512];
      glGetProgramInfoLog(p, sizeof(log), nullptr, log);
      fprintf(stderr, "program link error: %s\n", log);
      throw std::runtime_error("GLSL program link failed");
    }
    return p;
  }

  // xdg_wm_base ping handler: the compositor periodically checks we're alive.
  static void wmBasePing(void *, xdg_wm_base *base, uint32_t serial) {
    // Echo the serial straight back so the compositor knows we're responsive.
    xdg_wm_base_pong(base, serial);
  }
  // Listener struct binding the ping callback above to xdg_wm_base's single
  // event.
  static constexpr xdg_wm_base_listener wmBaseListener = {wmBasePing};

  // Called when the compositor wants us to (re)configure our xdg_surface.
  static void surfaceConfigure(void *data, xdg_surface *xs, uint32_t serial) {
    // Recover the LiteUI instance this callback belongs to.
    auto *self = static_cast<LiteUI *>(data);
    // Acknowledge the configure event with its serial, as the protocol
    // requires.
    xdg_surface_ack_configure(xs, serial);
    // Remember that we're now allowed to attach a buffer.
    self->configured_ = true;
    // If the compositor suggested a size different from what we have, resize
    // (reallocate the buffer + relayout) to match. Otherwise, on the very
    // first configure, no buffer exists yet, so create and attach one now.
    if (self->pendingWidth_ > 0 && self->pendingHeight_ > 0 &&
        (self->pendingWidth_ != self->width_ ||
         self->pendingHeight_ != self->height_)) {
      self->resize(self->pendingWidth_, self->pendingHeight_);
    } else if (!self->eglReady_)
      self->initEgl();
  }
  // Listener struct binding surfaceConfigure to xdg_surface's single event.
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  // Called when the compositor suggests a new size/state for the toplevel.
  static void toplevelConfigure(void *data, xdg_toplevel *, int32_t width,
                                int32_t height, wl_array *) {
    // 0x0 means "you decide the size" — keep whatever we currently have.
    // The actual resize happens later, in surfaceConfigure, once this
    // configure is ack'd (that's the point at which the protocol allows us
    // to attach a differently-sized buffer).
    auto *self = static_cast<LiteUI *>(data);
    if (width > 0 && height > 0) {
      self->pendingWidth_ = width;
      self->pendingHeight_ = height;
    }
  }
  // Called when the compositor/user requests the window be closed (e.g. via a
  // taskbar close action).
  static void toplevelClose(void *data, xdg_toplevel *) {
    // Flip the running flag so run()'s dispatch loop exits on its next check.
    static_cast<LiteUI *>(data)->running_ = false;
  }
  // Listener struct binding the two callbacks above to xdg_toplevel's
  // configure/close events.
  static constexpr xdg_toplevel_listener toplevelListener = {toplevelConfigure,
                                                             toplevelClose};

  // wl_seat/wl_pointer

  // Called when the seat announces which input capabilities
  // (pointer/keyboard/touch) it has.
  static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Only act if the seat has a pointer and we haven't already grabbed one.
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
      // Request the pointer object from the seat.
      self->pointer_ = wl_seat_get_pointer(seat);
      // Register our pointer event handlers on it.
      wl_pointer_add_listener(self->pointer_, &pointerListener, self);
    }
  }

  // Called when the seat reports its human-readable name; unused here.
  static void seatName(void *, wl_seat *, const char *) {}
  // Listener struct binding the two seat callbacks above to wl_seat's events.
  static constexpr wl_seat_listener seatListener = {seatCapabilities, seatName};

  // Called when the pointer enters this surface.
  static void pointerEnter(void *data, wl_pointer *, uint32_t serial,
                           wl_surface *, wl_fixed_t sx, wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Convert Wayland's fixed-point x coordinate to a double and store it.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Same for the y coordinate.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // wl_pointer_set_cursor requires the serial of an enter event, and it
    // isn't resent on plain motion, so remember it for later calls.
    self->pointerEnterSerial_ = serial;
    // Force the next setCursor() call to actually apply, since re-entering
    // (e.g. after a resize/move) may land us back on the same cursor name
    // the compositor's default already reset to something else.
    self->currentCursorName_.clear();
    self->setCursor(cursorNameForEdge(
        self->resizeEdgeAt(self->pointer_x_, self->pointer_y_)));
  }
  // Called when the pointer leaves this surface. Reset the cached cursor
  // name so re-entering always re-applies one, rather than skipping the
  // very next setCursor() as a no-op change.
  static void pointerLeave(void *data, wl_pointer *, uint32_t, wl_surface *) {
    static_cast<LiteUI *>(data)->currentCursorName_.clear();
  }
  // Called on every pointer movement while over this surface.
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Update the stored x position.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Update the stored y position.
    self->pointer_y_ = wl_fixed_to_double(sy);

    // Advance any in-progress scrollbar drag / content pan (see
    // beginScrollPress/handlePress). A no-op, and cheap, when nothing's
    // being dragged.
    bool changed = self->updateScrollDrag(static_cast<float>(self->pointer_x_),
                                          static_cast<float>(self->pointer_y_));
    if (self->updateAllDrags(static_cast<float>(self->pointer_x_),
                             static_cast<float>(self->pointer_y_)))
      changed = true;
    if (self->hasRoot_ &&
        LiteUI::updateHover(self->root_, static_cast<float>(self->pointer_x_),
                            static_cast<float>(self->pointer_y_), ClipRect{}))
      changed = true;
    if (changed)
      self->redraw();

    // Re-derive which edge (if any) the pointer is over and update the
    // cursor image to match, so the user sees a resize cursor before they
    // even click.
    self->setCursor(cursorNameForEdge(
        self->resizeEdgeAt(self->pointer_x_, self->pointer_y_)));
  }
  // Called on scroll/axis events — mouse wheel rotation or a touchpad's
  // continuous scroll gesture. `value` is already a relative-movement
  // amount in the same coordinate space as pointer motion (per the
  // wl_pointer protocol), so it's usable directly as a pixel delta with no
  // extra scaling, unlike Windows' notch-based WM_MOUSEWHEEL.
  static void pointerAxis(void *data, wl_pointer *, uint32_t, uint32_t axis,
                          wl_fixed_t value) {
    auto *self = static_cast<LiteUI *>(data);
    float delta = static_cast<float>(wl_fixed_to_double(value));
    float dx = 0, dy = 0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
      dy = delta;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
      dx = delta;
    bool changed =
        self->applyWheelScroll(static_cast<float>(self->pointer_x_),
                               static_cast<float>(self->pointer_y_), dx, dy);
    // Wayland's vertical-scroll axis value is positive for
    // scroll-down/content-down, matching applyWheelScroll's sign
    // convention above — negate it here so dispatchScroll's "notches >
    // 0 means wheel-up" contract stays consistent across platforms.
    if (dy != 0.0f &&
        self->dispatchScroll(static_cast<float>(self->pointer_x_),
                             static_cast<float>(self->pointer_y_), -dy))
      changed = true;
    if (changed)
      self->redraw();
  }
  // Called on every pointer button press/release.
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    MouseButton btn;
    if (button == BTN_LEFT_CODE)
      btn = MouseButton::Left;
    else if (button == BTN_MIDDLE_CODE)
      btn = MouseButton::Middle;
    else if (button == BTN_RIGHT_CODE)
      btn = MouseButton::Right;
    else
      return; // some other device button we don't handle
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
      // Route the press to the custom titlebar hit-testing logic, passing
      // the event serial (needed for interactive resize/move grabs).
      self->handlePress(serial, btn);
    else if (state == WL_POINTER_BUTTON_STATE_RELEASED)
      self->handleRelease(btn);
  }
  // Listener struct binding all five pointer callbacks above to wl_pointer's
  // events, in the order the interface expects.
  static constexpr wl_pointer_listener pointerListener = {
      pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis};

  // Called once per global object the compositor advertises via the registry.
  static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // If this global is the compositor interface...
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
      // ...bind to it at version 4 and store the resulting proxy.
      self->compositor_ = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    // If instead this global is the xdg_wm_base (window-shell) interface...
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      // ...bind to it at version 1...
      self->wm_base_ = static_cast<xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      // ...and register the ping/pong listener on it immediately.
      xdg_wm_base_add_listener(self->wm_base_, &wmBaseListener, self);
    }
    // If instead this global is the shared-memory interface...
    else if (strcmp(interface, wl_shm_interface.name) == 0) {
      // ...bind to it so we can later allocate pixel buffers.
      self->shm_ = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    // If instead this global is the seat (input devices) interface...
    else if (strcmp(interface, wl_seat_interface.name) == 0) {
      // ...bind to it...
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      // ...and register the capabilities/name listener so we learn about the
      // pointer.
      wl_seat_add_listener(self->seat_, &seatListener, self);
    }
    // If instead this global is the decoration-manager interface...
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
             0) {
      // ...bind to it so we can later request client-side (or server-side)
      // decorations.
      self->decoration_manager_ =
          static_cast<zxdg_decoration_manager_v1 *>(wl_registry_bind(
              registry, name, &zxdg_decoration_manager_v1_interface, 1));
    }
  }
  // Called when a global is removed; this minimal window doesn't need to react.
  static void registryRemove(void *, wl_registry *, uint32_t) {}
  // Listener struct binding the two registry callbacks above to wl_registry's
  // events.
  static constexpr wl_registry_listener registryListener = {registryGlobal,
                                                            registryRemove};

  // How close (in pixels) the pointer must be to the window's outer edge
  // before we treat it as a resize grab instead of ordinary titlebar
  // content. Kept well inside the button icons (which start ~7px into a
  // 32px titlebar) so it never steals clicks from minimize/maximize/close.
  static constexpr int kResizeMargin = 6;

  // Maps a pointer position to which edge (if any) an interactive resize
  // should grab, mirroring how most CSD toolkits treat a thin strip along
  // each window edge as a resize handle rather than ordinary content.
  uint32_t resizeEdgeAt(double px, double py) const {
    bool left = px < kResizeMargin;
    bool right = px >= width_ - kResizeMargin;
    bool top = py < kResizeMargin;
    bool bottom = py >= height_ - kResizeMargin;
    if (top && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    if (top && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    if (bottom && left)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    if (bottom && right)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    if (left)
      return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    if (right)
      return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    if (top)
      return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    if (bottom)
      return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
  }

  // Standard XCursor names for each edge/corner; "left_ptr" is the ordinary
  // arrow shown everywhere else.
  static const char *cursorNameForEdge(uint32_t edge) {
    switch (edge) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
      return "top_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
      return "bottom_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
      return "left_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
      return "right_side";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
      return "top_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
      return "top_right_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
      return "bottom_left_corner";
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
      return "bottom_right_corner";
    default:
      return "left_ptr";
    }
  }

  // Swaps the pointer's visible cursor to the named XCursor image, skipping
  // the work entirely if it's already showing (motion events fire far more
  // often than the cursor actually needs to change).
  void setCursor(const char *name) {
    if (!cursorTheme_ || !pointer_ || currentCursorName_ == name)
      return;
    wl_cursor *cursor = wl_cursor_theme_get_cursor(cursorTheme_, name);
    if (!cursor)
      cursor = wl_cursor_theme_get_cursor(cursorTheme_, "default");
    if (!cursor || cursor->image_count == 0)
      return;
    wl_cursor_image *image = cursor->images[0];
    wl_buffer *cbuf = wl_cursor_image_get_buffer(image);
    if (!cbuf)
      return;
    currentCursorName_ = name;
    wl_surface_attach(cursorSurface_, cbuf, 0, 0);
    wl_surface_damage_buffer(cursorSurface_, 0, 0, image->width, image->height);
    wl_surface_commit(cursorSurface_);
    wl_pointer_set_cursor(pointer_, pointerEnterSerial_, cursorSurface_,
                          image->hotspot_x, image->hotspot_y);
  }

  // Handles a compositor-driven resize: tears down the old shm buffers,
  // updates width_/height_, re-runs layout against the new size, and
  // allocates+attaches fresh buffers at the new dimensions. Buffers can't
  // be resized in place — wl_shm buffers are fixed-size — so this is a full
  // destroy/recreate rather than a realloc.
  void resize(int newWidth, int newHeight) {

    width_ = newWidth;
    height_ = newHeight;
    if (eglWindow_)
      wl_egl_window_resize(eglWindow_, width_, height_, 0, 0);
    relayout();
    if (eglReady_)
      redraw();
  }

  // Creates the EGL context/surface for this window's wl_surface and
  // compiles the two shader programs. Called once, from surfaceConfigure(),
  // the first time the compositor hands us a configure event — mirroring
  // when attachBuffer() used to run.
  void initEgl() {
    // eglGetDisplay() can't reliably tell "this pointer is a wl_display*"
    // apart from other native display types on multi-platform Mesa
    // builds — on some setups it silently falls back to the generic
    // "device" platform, which tries to open a DRM fd (fails with -1)
    // and then falls further back to a broken software/zink path. Asking
    // for EGL_PLATFORM_WAYLAND_KHR explicitly removes the guesswork.
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (eglGetPlatformDisplayEXT) {
      eglDisplay_ =
          eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT, display_, nullptr);
    } else {
      eglDisplay_ =
          eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    }
    if (eglDisplay_ == EGL_NO_DISPLAY)
      throw std::runtime_error("eglGetDisplay failed");
    EGLint major, minor;
    if (!eglInitialize(eglDisplay_, &major, &minor))
      throw std::runtime_error("eglInitialize failed");
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfgAttribs[] = {EGL_SURFACE_TYPE,
                                 EGL_WINDOW_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES2_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_ALPHA_SIZE,
                                 0,
                                 EGL_NONE};
    EGLConfig config;
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, cfgAttribs, &config, 1, &numConfigs) ||
        numConfigs < 1)
      throw std::runtime_error("eglChooseConfig failed");

    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    eglContext_ =
        eglCreateContext(eglDisplay_, config, EGL_NO_CONTEXT, ctxAttribs);
    if (eglContext_ == EGL_NO_CONTEXT)
      throw std::runtime_error("eglCreateContext failed");

    eglWindow_ = wl_egl_window_create(surface_, width_, height_);
    if (!eglWindow_)
      throw std::runtime_error("wl_egl_window_create failed");

    eglSurface_ = eglCreateWindowSurface(
        eglDisplay_, config, reinterpret_cast<EGLNativeWindowType>(eglWindow_),
        nullptr);
    if (eglSurface_ == EGL_NO_SURFACE)
      throw std::runtime_error("eglCreateWindowSurface failed");

    if (!eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_))
      throw std::runtime_error("eglMakeCurrent failed");
    eglSwapInterval(eglDisplay_,
                    1); // vsync — avoids tearing during scroll drags

    // Tell the compositor this whole surface is opaque, so it can skip
    // blending against the desktop entirely rather than relying solely on
    // the alpha-less EGL config above. INT32_MAX as the region size is the
    // standard "cover the whole surface regardless of actual dimensions"
    // idiom — Wayland clips it to the surface's real bounds internally.
    wl_region *opaque = wl_compositor_create_region(compositor_);
    wl_region_add(opaque, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_set_opaque_region(surface_, opaque);
    wl_region_destroy(opaque);

    rectProgram_ = linkProgram(kRectVS, kRectFS);
    rectAPos_ = glGetAttribLocation(rectProgram_, "a_pos");
    rectUPos_ = glGetUniformLocation(rectProgram_, "u_rectPos");
    rectUSize_ = glGetUniformLocation(rectProgram_, "u_rectSize");
    rectUScreen_ = glGetUniformLocation(rectProgram_, "u_screen");
    rectURadius_ = glGetUniformLocation(rectProgram_, "u_radius");
    rectUColor_ = glGetUniformLocation(rectProgram_, "u_color");

    flatProgram_ = linkProgram(kFlatVS, kFlatFS);
    flatAPos_ = glGetAttribLocation(flatProgram_, "a_pos");
    flatUScreen_ = glGetUniformLocation(flatProgram_, "u_screen");
    flatUColor_ = glGetUniformLocation(flatProgram_, "u_color");

    texProgram_ = linkProgram(kTexVS, kTexFS);
    texAPos_ = glGetAttribLocation(texProgram_, "a_pos");
    texUPos_ = glGetUniformLocation(texProgram_, "u_pos");
    texUSize_ = glGetUniformLocation(texProgram_, "u_size");
    texUScreen_ = glGetUniformLocation(texProgram_, "u_screen");
    texUTex_ = glGetUniformLocation(texProgram_, "u_tex");
    texUColor_ = glGetUniformLocation(texProgram_, "u_color");

    canvasProgram_ = linkProgram(kTexVS, kCanvasFS);
    canvasAPos_ = glGetAttribLocation(canvasProgram_, "a_pos");
    canvasUPos_ = glGetUniformLocation(canvasProgram_, "u_pos");
    canvasUSize_ = glGetUniformLocation(canvasProgram_, "u_size");
    canvasUScreen_ = glGetUniformLocation(canvasProgram_, "u_screen");
    canvasUTex_ = glGetUniformLocation(canvasProgram_, "u_tex");

    static const float kUnitQuad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kUnitQuad), kUnitQuad, GL_STATIC_DRAW);
    glGenBuffers(1, &flatVbo_); // contents supplied per-draw in drawLine()

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_SCISSOR_TEST); // ClipRect maps straight onto glScissor

    eglReady_ = true;
    redraw();
  }

  // Restricts subsequent draws to `clip`, intersected with the window
  // bounds. GL's scissor origin is bottom-left, so the y range is flipped
  // relative to ClipRect's top-left/y-down convention.
  void applyScissor(const ClipRect &clip) {
    float x0 = std::max(clip.x0, 0.0f), y0 = std::max(clip.y0, 0.0f);
    float x1 = std::min(clip.x1, static_cast<float>(width_));
    float y1 = std::min(clip.y1, static_cast<float>(height_));
    if (x1 < x0)
      x1 = x0;
    if (y1 < y0)
      y1 = y0;
    glScissor(static_cast<int>(x0), static_cast<int>(height_ - y1),
              static_cast<int>(x1 - x0), static_cast<int>(y1 - y0));
  }

  // Draws one filled, optionally rounded-corner rectangle. radius <= 0
  // renders a plain rect (same shader — the SDF collapses correctly).
  void drawRectGL(float x, float y, float w, float h, float radius, Color c,
                  const ClipRect &clip) {
    if (w <= 0 || h <= 0)
      return;
    applyScissor(clip);
    glUseProgram(rectProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(rectAPos_);
    glVertexAttribPointer(rectAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(rectUPos_, x, y);
    glUniform2f(rectUSize_, w, h);
    glUniform2f(rectUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    glUniform1f(rectURadius_, std::max(0.0f, radius));
    glUniform4f(rectUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                c.a / 255.0f);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Thick line as an oriented quad — needed (rather than a bbox rect) since
  // drawTitlebar uses this for the X icon's two diagonal strokes.
  void drawLineGL(float x0, float y0, float x1, float y1, Color c,
                  float thickness = 2.0f) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx * dx + dy * dy);
    float nx = len > 0.0001f ? -dy / len : 1.0f;
    float ny = len > 0.0001f ? dx / len : 0.0f;
    float hw = thickness / 2.0f;
    const float verts[8] = {x0 + nx * hw, y0 + ny * hw, x0 - nx * hw,
                            y0 - ny * hw, x1 + nx * hw, y1 + ny * hw,
                            x1 - nx * hw, y1 - ny * hw};
    applyScissor(ClipRect{}); // titlebar icons are never clipped
    glUseProgram(flatProgram_);
    glBindBuffer(GL_ARRAY_BUFFER, flatVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(flatAPos_);
    glVertexAttribPointer(flatAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glUniform2f(flatUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    glUniform4f(flatUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // Fills an axis-aligned rectangle with a solid color by calling setPixel for
  // every point inside it. Same unclipped/clipped overload split as setPixel.
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    fillRect(x0, y0, w, h, r, g, b, ClipRect{});
  }
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b,
                const ClipRect &clip) {
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h), 0.0f, {r, g, b},
               clip);
  }

  // Fills an axis-aligned rectangle with rounded corners, clamping the
  // radius so it can't exceed half the shorter side. Per-pixel distance
  // check against each corner's circle center — fine at this scale, not
  // meant for huge boxes. Same unclipped/clipped overload split as setPixel.
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b) {
    fillRoundedRect(x0, y0, w, h, radius, r, g, b, ClipRect{});
  }
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b, const ClipRect &clip) {
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    drawRectGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(w), static_cast<float>(h),
               static_cast<float>(radius), {r, g, b}, clip);
  }

  // Rasterizes v.text under v.textStyle into an alpha-only glyph-coverage
  // surface sized to v's final content box (via Pango/Cairo), uploads it
  // as a GL texture, and caches the result until v.computed.w changes —
  // the only thing that can change how the text wraps without the
  // string/style themselves changing.
  //
  // Uploads as a single-channel GL_ALPHA texture (see below) rather than
  // cairo's native ARGB32 buffer — GL_ALPHA is core GLES2, unlike the
  // BGRA extension and non-tightly-packed row uploads that ARGB32 would
  // otherwise require.
  static void ensureTextTexture(const View &v) {
    const std::string &text = resolveDynamic(v.text, v.computed.resolvedText);
    if (v.computed.textTexture &&
        v.computed.textTextureBuiltForWidth == v.computed.w &&
        v.computed.textTextureBuiltForText == text)
      return;
    if (v.computed.textTexture) {
      glDeleteTextures(1, &v.computed.textTexture);
      v.computed.textTexture = 0;
    }
    const Style &s = v.style;
    int innerW = std::max(
        1, static_cast<int>(v.computed.w - s.padding.left - s.padding.right));
    int innerH = std::max(
        1, static_cast<int>(v.computed.h - s.padding.top - s.padding.bottom));

    cairo_surface_t *surf =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, innerW, innerH);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(
        cr, 1, 1, 1,
        1); // color is applied later via u_color; only alpha matters
    PangoLayout *layout = liteui_text::makeLayout(
        text, v.textStyle, static_cast<float>(innerW), cr);
    pango_cairo_update_layout(cr, layout);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_surface_flush(surf);

    unsigned char *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);

    // GLES2 core has neither GL_UNPACK_ROW_LENGTH (GLES3+/desktop only)
    // nor a guaranteed GL_BGRA_EXT, so uploading cairo's native ARGB32
    // buffer directly isn't portable here. The fragment shader (kTexFS)
    // only ever reads this texture's alpha channel anyway — color comes
    // from u_color — so there's no need to upload color data at all:
    // extract just the alpha byte of each pixel into a tightly packed
    // buffer and upload that as a single-channel GL_ALPHA texture.
    // Cairo's ARGB32 stores alpha in the high byte of each native-endian
    // 32-bit pixel, which on the little-endian platforms this file
    // targets is byte offset 3.
    std::vector<unsigned char> alpha(static_cast<size_t>(innerW) * innerH);
    for (int row = 0; row < innerH; ++row) {
      const unsigned char *src = data + row * stride;
      unsigned char *dst = alpha.data() + row * innerW;
      for (int col = 0; col < innerW; ++col)
        dst[col] = src[col * 4 + 3];
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Our `alpha` buffer is tightly packed at 1 byte/pixel with no row
    // padding, but GL's default GL_UNPACK_ALIGNMENT is 4 — it would
    // otherwise assume each row starts on a 4-byte boundary and skip
    // "padding" bytes that don't actually exist, shearing the image
    // into diagonal noise whenever innerW isn't a multiple of 4 (which
    // is almost always, for arbitrary text widths).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, innerW, innerH, 0, GL_ALPHA,
                 GL_UNSIGNED_BYTE, alpha.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT,
                  4); // restore GL's default for other uploads

    cairo_destroy(cr);
    cairo_surface_destroy(surf);

    v.computed.textTexture = tex;
    v.computed.textTexW = innerW;
    v.computed.textTexH = innerH;
    v.computed.textTextureBuiltForWidth = v.computed.w;
    v.computed.textTextureBuiltForText = text;
  }

  void drawTextTexture(const View &v, const ClipRect &clip) {
    ensureTextTexture(v);
    if (!v.computed.textTexture)
      return;
    applyScissor(clip);
    glUseProgram(texProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v.computed.textTexture);
    glUniform1i(texUTex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(texAPos_);
    glVertexAttribPointer(texAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    float x = v.computed.x + v.style.padding.left;
    float y = v.computed.y + v.style.padding.top;
    glUniform2f(texUPos_, x, y);
    glUniform2f(texUSize_, static_cast<float>(v.computed.textTexW),
                static_cast<float>(v.computed.textTexH));
    glUniform2f(texUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    const Color &c = v.textStyle.color;
    glUniform4f(texUColor_, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f,
                c.a / 255.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // (Re)builds v.computed.canvasSurface — a Cairo ARGB32 image surface
  // sized to v's inner (padding-excluded) content box — whenever that
  // size has changed since it was last built, then (re)invokes v.onPaint
  // into it whenever the size changed OR the app requested a redraw via
  // View::requestCanvasRedraw() (canvasNeedsRedraw starts true, so this
  // always happens at least once). Finally (re)uploads the surface's
  // pixels as a GL texture whenever either of those happened — a plain
  // repaint with neither condition true reuses the existing texture
  // untouched.
  void ensureCanvasSurface(const View &v) {
    float innerW = std::max(1.0f, v.computed.w - v.style.padding.left -
                                      v.style.padding.right);
    float innerH = std::max(1.0f, v.computed.h - v.style.padding.top -
                                      v.style.padding.bottom);
    int iw = std::max(1, static_cast<int>(innerW));
    int ih = std::max(1, static_cast<int>(innerH));
    bool resized = !v.computed.canvasSurface ||
                   v.computed.canvasBuiltForWidth != innerW ||
                   v.computed.canvasBuiltForHeight != innerH;
    if (resized) {
      if (v.computed.canvasSurface) {
        cairo_surface_destroy(v.computed.canvasSurface);
        v.computed.canvasSurface = nullptr;
      }
      v.computed.canvasSurface =
          cairo_image_surface_create(CAIRO_FORMAT_ARGB32, iw, ih);
      v.computed.canvasBuiltForWidth = innerW;
      v.computed.canvasBuiltForHeight = innerH;
      v.computed.canvasNeedsRedraw = true;
    }
    if (!v.computed.canvasSurface)
      return;
    bool needUpload = resized;
    if (v.computed.canvasNeedsRedraw && v.onPaint) {
      cairo_t *cr = cairo_create(v.computed.canvasSurface);
      cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
      cairo_set_source_rgba(cr, 0, 0, 0, 0);
      cairo_paint(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      CanvasContext ctx(cr, static_cast<float>(iw), static_cast<float>(ih));
      v.onPaint(ctx);
      cairo_destroy(cr);
      cairo_surface_flush(v.computed.canvasSurface);
      v.computed.canvasNeedsRedraw = false;
      needUpload = true;
    }
    if (!needUpload && v.computed.canvasTexture)
      return;

    // Convert Cairo's premultiplied, native-endian ARGB32 (byte order
    // B,G,R,A on the little-endian platforms this file targets — same
    // convention noted in ensureTextTexture above) into straight-alpha
    // RGBA for GL upload, since this file's blend function (GL_SRC_ALPHA,
    // GL_ONE_MINUS_SRC_ALPHA) expects straight alpha like every other
    // texture drawn here.
    unsigned char *data =
        cairo_image_surface_get_data(v.computed.canvasSurface);
    int stride = cairo_image_surface_get_stride(v.computed.canvasSurface);
    std::vector<unsigned char> rgba(static_cast<size_t>(iw) * ih * 4);
    for (int row = 0; row < ih; ++row) {
      const unsigned char *src = data + row * stride;
      unsigned char *dst = rgba.data() + static_cast<size_t>(row) * iw * 4;
      for (int col = 0; col < iw; ++col) {
        unsigned char b = src[col * 4 + 0], g = src[col * 4 + 1],
                      r = src[col * 4 + 2], a = src[col * 4 + 3];
        unsigned char *px = dst + col * 4;
        px[0] = a ? static_cast<unsigned char>(std::min(255, r * 255 / a)) : 0;
        px[1] = a ? static_cast<unsigned char>(std::min(255, g * 255 / a)) : 0;
        px[2] = a ? static_cast<unsigned char>(std::min(255, b * 255 / a)) : 0;
        px[3] = a;
      }
    }
    if (v.computed.canvasTexture) {
      glDeleteTextures(1, &v.computed.canvasTexture);
      v.computed.canvasTexture = 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT,
                  1); // rgba is tightly packed (see
                      // ensureTextTexture's identical note)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    v.computed.canvasTexture = tex;
  }

  void drawCanvasTexture(const View &v, const ClipRect &clip) {
    ensureCanvasSurface(v);
    if (!v.computed.canvasTexture)
      return;
    applyScissor(clip);
    glUseProgram(canvasProgram_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, v.computed.canvasTexture);
    glUniform1i(canvasUTex_, 0);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(canvasAPos_);
    glVertexAttribPointer(canvasAPos_, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    float x = v.computed.x + v.style.padding.left;
    float y = v.computed.y + v.style.padding.top;
    glUniform2f(canvasUPos_, x, y);
    glUniform2f(canvasUSize_, v.computed.canvasBuiltForWidth,
                v.computed.canvasBuiltForHeight);
    glUniform2f(canvasUScreen_, static_cast<float>(width_),
                static_cast<float>(height_));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }

  // showing) as flat filled rectangles — classic fixed track+thumb
  // styling, matching the Windows/GDI renderer's look. `clip` is v's own
  // ancestor-level clip (not narrowed by v's own children), so a
  // scrollbar correctly disappears if v itself has scrolled out of some
  // outer ancestor's viewport, same reasoning as the GDI version.
  void renderScrollbars(const View &v, const ClipRect &clip) {
    if (wantVBar(v)) {
      PixRect t = vTrackRect(v), th = vThumbRect(v);
      fillRect(static_cast<int>(t.x), static_cast<int>(t.y),
               static_cast<int>(t.w), static_cast<int>(t.h), 0xE0, 0xE0, 0xE0,
               clip);
      fillRect(static_cast<int>(th.x), static_cast<int>(th.y),
               static_cast<int>(th.w), static_cast<int>(th.h), 0x90, 0x90, 0x90,
               clip);
    }
    if (wantHBar(v)) {
      PixRect t = hTrackRect(v), th = hThumbRect(v);
      fillRect(static_cast<int>(t.x), static_cast<int>(t.y),
               static_cast<int>(t.w), static_cast<int>(t.h), 0xE0, 0xE0, 0xE0,
               clip);
      fillRect(static_cast<int>(th.x), static_cast<int>(th.y),
               static_cast<int>(th.w), static_cast<int>(th.h), 0x90, 0x90, 0x90,
               clip);
    }
    if (wantVBar(v) && wantHBar(v))
      fillRect(
          static_cast<int>(v.computed.x + v.computed.w - kScrollbarThickness),
          static_cast<int>(v.computed.y + v.computed.h - kScrollbarThickness),
          static_cast<int>(kScrollbarThickness),
          static_cast<int>(kScrollbarThickness), 0xE0, 0xE0, 0xE0, clip);
  }

  // Draws one View (background + border) using its already-computed layout,
  // then recurses into children. Border is drawn as an outer rounded rect in
  // borderColor with an inner rounded rect in backgroundColor inset by
  // borderWidth — simple and correct for a uniform border on all sides.
  // Paints v's own background/border, then recurses into in-flow
  // children only. Any Position::Absolute descendant, at any depth, is
  // skipped here — it's collected separately and painted in a single
  // global-stacking pass afterward, so it can interleave correctly with
  // absolute nodes from entirely different subtrees.
  //
  // `clip` is the accumulated intersection of every scrollable ancestor's
  // viewport, exactly mirroring hitTestFlow()'s clip parameter — v's own
  // background/border is drawn under `clip` (the ancestor-level one, not
  // narrowed by v itself), while children are drawn under a further-
  // narrowed clip if v itself scrolls, which is what actually makes
  // scrolled-out content invisible instead of just mispositioned.
  void renderView(const View &v, ClipRect clip) {
    if (v.style.visibility == Visibility::Hidden)
      return; // space already reserved by layout; just don't draw it
    // Same reasoning as paintView (Windows): a text leaf's Style is
    // layout-only, so it must never paint its own opaque background —
    // otherwise white text (or any text) can end up invisible against
    // its own node's default-white box, as happened here.
    if (v.isText) {
      drawTextTexture(v, clip);
      return;
    }
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    int radius = static_cast<int>(
        resolveDynamic(s.borderRadius, v.computed.resolvedBorderRadius));
    Color bg = (v.computed.isHovered && s.hoverColor)
                   ? *s.hoverColor
                   : resolveDynamic(s.backgroundColor,
                                    v.computed.resolvedBackgroundColor);
    float borderWidthVal =
        resolveDynamic(s.borderWidth, v.computed.resolvedBorderWidth);
    if (borderWidthVal > 0) {
      Color borderColorVal =
          resolveDynamic(s.borderColor, v.computed.resolvedBorderColor);
      fillRoundedRect(x, y, w, h, radius, borderColorVal.r, borderColorVal.g,
                      borderColorVal.b, clip);
      int bw = static_cast<int>(borderWidthVal);
      fillRoundedRect(x + bw, y + bw, std::max(0, w - 2 * bw),
                      std::max(0, h - 2 * bw), std::max(0, radius - bw), bg.r,
                      bg.g, bg.b, clip);
    } else {
      fillRoundedRect(x, y, w, h, radius, bg.r, bg.g, bg.b, clip);
    }
    // A canvas node paints its own Style background/border like any
    // other box (above), then has its cached onPaint texture drawn on
    // top — unlike a text leaf, which skips background/border entirely.
    // Canvas nodes aren't expected to have children, so control falls
    // through to the (harmless, normally no-op) children loop below.
    if (v.isCanvas)
      drawCanvasTexture(v, clip);

    ClipRect childClip = (v.scrollsX() || v.scrollsY())
                             ? clip.intersect(v.computed.x, v.computed.y,
                                              v.computed.w, v.computed.h)
                             : clip;
    for (const auto &child : v.children)
      if (child.style.position != Position::Absolute &&
          child.style.display != Display::None)
        renderView(child, childClip);
    // Scrollbars sit in the gutter layout already reserved outside the
    // children's placement area, so drawing them after children never
    // overlaps content, and keeps them visually on top.
    if (v.scrollsX() || v.scrollsY())
      renderScrollbars(v, clip);
  }

  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  // Draws a crude line between two points by linear interpolation, stepping
  // once per pixel along the longer axis.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                uint8_t b) {
    drawLineGL(static_cast<float>(x0), static_cast<float>(y0),
               static_cast<float>(x1), static_cast<float>(y1), {r, g, b});
  }

  // ---- titlebar button layout ----
  // Plain axis-aligned rectangle used for button hit-testing.
  struct Rect {
    int x, y, w, h;
  };
  // Returns whether point (px, py) falls within rectangle r (using half-open
  // bounds).
  static bool inside(const Rect &r, double px, double py) {
    // Standard axis-aligned bounding box containment test.
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }
  // Computes the close button's rectangle: flush against the right edge of the
  // titlebar.
  Rect closeRect() const {
    // x is inset from the right edge by the margin and the button's own width;
    // y centers it vertically in the titlebar.
    return {width_ - kButtonMargin - kButtonSize,
            (kTitlebarHeight - kButtonSize) / 2, kButtonSize, kButtonSize};
  }
  // Computes the maximize button's rectangle, positioned one button-width left
  // of the close button.
  Rect maximizeRect() const {
    // Start from the close button's rectangle as a reference point.
    Rect c = closeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {c.x - kButtonMargin - kButtonSize, c.y, kButtonSize, kButtonSize};
  }
  // Computes the minimize button's rectangle, positioned one button-width left
  // of the maximize button.
  Rect minimizeRect() const {
    // Start from the maximize button's rectangle as a reference point.
    Rect m = maximizeRect();
    // Shift left by one margin plus one button width, keeping the same y/size.
    return {m.x - kButtonMargin - kButtonSize, m.y, kButtonSize, kButtonSize};
  }

  // Paints the custom titlebar background and its three buttons/icons.
  void drawTitlebar() {
    // Fill the whole titlebar strip with a dark gray background.
    fillRect(0, 0, width_, kTitlebarHeight, 0x2D, 0x2D, 0x2D);

    // Compute all three button rectangles up front for reuse below.
    Rect minr = minimizeRect(), maxr = maximizeRect(), clsr = closeRect();
    // Draw the minimize button's background in a lighter gray.
    fillRect(minr.x, minr.y, minr.w, minr.h, 0x50, 0x50, 0x50);
    // Draw the maximize button's background in the same lighter gray.
    fillRect(maxr.x, maxr.y, maxr.w, maxr.h, 0x50, 0x50, 0x50);
    // Draw the close button's background in red to visually distinguish it.
    fillRect(clsr.x, clsr.y, clsr.w, clsr.h, 0xC0, 0x39, 0x2B);

    // minimize: underscore
    // Draw a single horizontal line near the bottom of the minimize button to
    // form an underscore glyph.
    drawLine(minr.x + 4, minr.y + minr.h - 5, minr.x + minr.w - 4,
             minr.y + minr.h - 5, 0xFF, 0xFF, 0xFF);
    // maximize: square outline
    // Top edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + maxr.w - 4, maxr.y + 4, 0xFF,
             0xFF, 0xFF);
    // Bottom edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + maxr.h - 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // Left edge of the square icon.
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + 4, maxr.y + maxr.h - 4, 0xFF,
             0xFF, 0xFF);
    // Right edge of the square icon.
    drawLine(maxr.x + maxr.w - 4, maxr.y + 4, maxr.x + maxr.w - 4,
             maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // close: X
    // First diagonal stroke of the X, from top-left to bottom-right.
    drawLine(clsr.x + 4, clsr.y + 4, clsr.x + clsr.w - 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
    // Second diagonal stroke of the X, from top-right to bottom-left.
    drawLine(clsr.x + clsr.w - 4, clsr.y + 4, clsr.x + 4, clsr.y + clsr.h - 4,
             0xFF, 0xFF, 0xFF);
  }

  // Repaints the entire window content and commits it to the compositor.
  // Draws into whichever of the two buffers (drawBuf_) isn't currently
  // still owned by the compositor — see the "Double buffering" comment
  // block for why that matters. If it's still busy, the draw is skipped
  // entirely (never blocks) and redrawPending_ is set so bufferRelease()
  // retries automatically the moment that buffer frees up — a call to
  // redraw() should therefore be thought of as "request a repaint soon",
  // not "repaint synchronously right now".
  void redraw() {
    if (!eglReady_)
      return;
    glViewport(0, 0, width_, height_);
    applyScissor(ClipRect{});
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f); // plain white content-area background
    glClear(GL_COLOR_BUFFER_BIT);
    // Paint queued boxes on top of the plain background, before the titlebar
    // so it stays on top.
    for (const auto &b : boxes_)
      fillRect(b.pos_x, b.pos_y, b.width, b.height, b.color.r, b.color.g,
               b.color.b);
    if (hasRoot_) {
      renderView(root_, ClipRect{});
      std::vector<AbsoluteEntry> absolutes;
      collectAbsolutes(root_, absolutes);
      std::stable_sort(absolutes.begin(), absolutes.end(),
                       [](const AbsoluteEntry &a, const AbsoluteEntry &b) {
                         if (a.view->style.zIndex != b.view->style.zIndex)
                           return a.view->style.zIndex < b.view->style.zIndex;
                         return a.order < b.order;
                       });
      for (const auto &e : absolutes)
        renderView(*e.view, ClipRect{});
    }
    // Paint the titlebar and its buttons on top of that background.
    drawTitlebar();
    eglSwapBuffers(eglDisplay_, eglSurface_);
  }

  // Hit-tests a left-button press against the titlebar buttons, resize
  // edges, and drag region. Resize/move grabs and the chrome buttons
  // (close/maximize/minimize) still act immediately on press, same as
  // before — only content-area widget clicks wait for a matching release
  // (see beginPress/endPress).
  void handlePress(uint32_t serial, MouseButton btn = MouseButton::Left) {
    // Resize/move grabs and the chrome buttons are a left-button-only
    // convention (matching every desktop's own titlebar) — a middle/
    // right click on the resize strip just falls through to whatever's
    // below it instead of starting a grab.
    if (btn == MouseButton::Left) {
      uint32_t edge = resizeEdgeAt(pointer_x_, pointer_y_);
      if (edge != XDG_TOPLEVEL_RESIZE_EDGE_NONE) {
        if (seat_)
          xdg_toplevel_resize(toplevel_, seat_, serial, edge);
        return;
      }
    }

    // Presses below the titlebar strip first give scrollbars/scrollable
    // content a chance to claim the press (beginScrollPress) — a thumb
    // grab or track click consumes it entirely; anything else falls
    // through to the ordinary pending-click press, resolved later in
    // handleRelease().
    if (pointer_y_ >= kTitlebarHeight) {
      float x = static_cast<float>(pointer_x_),
            y = static_cast<float>(pointer_y_);
      // Only the left button interacts with scrollbars — a thumb grab
      // or track-click jump is a left-drag convention.
      if (btn != MouseButton::Left || !beginScrollPress(x, y))
        beginPress(x, y, btn);
      redraw();
      return;
    }

    if (btn != MouseButton::Left)
      return; // titlebar chrome (close/max/min/move) is left-button only

    // If the click landed on the close button...
    if (inside(closeRect(), pointer_x_, pointer_y_)) {
      // ...request the event loop to stop, ending run().
      running_ = false;
      return;
    }
    // If instead the click landed on the maximize button...
    if (inside(maximizeRect(), pointer_x_, pointer_y_)) {
      // If we're currently maximized, ask the compositor to restore the normal
      // size...
      if (maximized_)
        xdg_toplevel_unset_maximized(toplevel_);
      // ...otherwise ask it to maximize the window.
      else
        xdg_toplevel_set_maximized(toplevel_);
      // Flip our local tracking flag to match the new state.
      maximized_ = !maximized_;
      // Repaint immediately (icon/behavior may depend on this state later).
      redraw();
      return;
    }
    // If instead the click landed on the minimize button...
    if (inside(minimizeRect(), pointer_x_, pointer_y_)) {
      // ...ask the compositor to minimize the toplevel.
      xdg_toplevel_set_minimized(toplevel_);
      return;
    }

    // Any other click in the titlebar (empty space) starts an interactive move,
    // if we have a seat to drive it.
    if (seat_)
      xdg_toplevel_move(toplevel_, seat_, serial);
  }

  // Resolves a pending widget click (or in-progress scroll drag) on button
  // release. Chrome buttons and resize/move grabs don't need this — they
  // already acted on press — so this only matters for content-area
  // interactions below the titlebar.
  void handleRelease(MouseButton btn = MouseButton::Left) {
    if (pointer_y_ >= kTitlebarHeight) {
      bool changed = (btn == MouseButton::Left)
                         ? endScrollPress(static_cast<float>(pointer_x_),
                                          static_cast<float>(pointer_y_))
                         : endPress(static_cast<float>(pointer_x_),
                                    static_cast<float>(pointer_y_), btn);
      if (changed)
        redraw();
    } else {
      // release moved back into the titlebar; cancel the pending press
      pressedView_[btnIdx(btn)] = nullptr;
    }
    dragView_[btnIdx(btn)] = nullptr;
    if (btn == MouseButton::Left)
      scrollDrag_ = {}; // scroll-drag tracking only ever exists for Left
  }

// Ends the Windows/Linux member block.
#endif
};

// Out-of-line constructor definition; inline because this is a single-header
// library.
inline LiteUI::LiteUI(int w, int h, const std::string &title)
    : width_(w), height_(h) {
// Windows-specific construction path.
#if defined(_WIN32)
  // Handle to the current executable module, needed to register a window class.
  HINSTANCE hInst = GetModuleHandleW(nullptr);
  // Name used to identify our window class to Windows.
  const wchar_t *className = L"LiteUIClass";

  // Descriptor for the window class we're about to register.
  WNDCLASSW wc = {};
  // Point the class at our static message-handling function.
  wc.lpfnWndProc = WndProc;
  // Associate the class with this module.
  wc.hInstance = hInst;
  // Give the class the name declared above.
  wc.lpszClassName = className;
  // Use the standard arrow cursor while hovering the window.
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // Use the default window background brush/color.
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  // Register the class with the OS so CreateWindowExW can use it.
  RegisterClassW(&wc);

  // Convert the UTF-8 title into the wide string Win32 expects.
  std::wstring wtitle = toWide(title);

  // Create the actual window, passing `this` as the creation parameter so
  // WndProc can recover it.
  hwnd_ = CreateWindowExW(0, className, wtitle.c_str(), WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, width_, height_,
                          nullptr, nullptr, hInst, this);
  // If creation failed, surface it as an exception rather than continuing with
  // a null handle.
  if (!hwnd_) {
    throw std::runtime_error("CreateWindowExW failed");
  }

  // One factory per LiteUI instance is simplest here; a real app with many
  // windows would normally share a single process-wide factory instead.
  if (FAILED(
          D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_)))
    throw std::runtime_error("D2D1CreateFactory failed");
  ensureRenderTarget();

  // Make the window visible using the platform's default show behavior.
  ShowWindow(hwnd_, SW_SHOWDEFAULT);
  // Force an initial paint of the window.
  UpdateWindow(hwnd_);

// Linux/Wayland-specific construction path.
#else
  // Connect to the compositor via the default Wayland socket.
  display_ = wl_display_connect(nullptr);
  // Fail loudly if no compositor is reachable.
  if (!display_)
    throw std::runtime_error("Failed to connect to Wayland display");

  // Ask the display for its registry, the object through which globals are
  // advertised.
  wl_registry *registry = wl_display_get_registry(display_);
  // Register our global-announcement callbacks on it.
  wl_registry_add_listener(registry, &registryListener, this);
  // Round-trip blocks until the server has answered every request sent
  // so far — the only way to get bind() results synchronously, since
  // globals normally arrive as async events.
  // Block until the compositor has processed everything so far, guaranteeing
  // all globals are bound by now.
  wl_display_roundtrip(display_);

  // Make sure the globals this window absolutely needs were actually
  // advertised.
  if (!compositor_ || !wm_base_ || !shm_)
    throw std::runtime_error("Missing required Wayland globals");

  // Load a cursor theme so resize/arrow cursors can be shown over CSD
  // window edges — nothing does this automatically the way server-side
  // decorations would. nullptr picks the user's configured/default theme;
  // 24 is a conventional base cursor size in pixels. Non-fatal if it fails:
  // we just silently keep whatever cursor the compositor already set.
  cursorTheme_ = wl_cursor_theme_load(nullptr, 24, shm_);
  if (cursorTheme_)
    cursorSurface_ = wl_compositor_create_surface(compositor_);

  // Create the raw drawable surface from the compositor.
  surface_ = wl_compositor_create_surface(compositor_);
  // Wrap that surface as an xdg_surface so it can become a desktop window.
  xdg_surf_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  // Register the configure/ack handler on the xdg_surface.
  xdg_surface_add_listener(xdg_surf_, &surfListener, this);

  // Promote the xdg_surface to a toplevel (a normal, movable/resizable
  // top-level window).
  toplevel_ = xdg_surface_get_toplevel(xdg_surf_);
  // Register the configure/close handlers on the toplevel.
  xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
  // Set the window's title as shown in taskbars/window switchers.
  xdg_toplevel_set_title(toplevel_, title.c_str());

  // Commit the surface state now, which triggers the compositor's first
  // configure event.
  wl_surface_commit(surface_); // triggers the first configure event
// Ends the Windows/Linux construction branch.
#endif
}

// Out-of-line destructor definition.
inline LiteUI::~LiteUI() {
  if (hasRoot_)
    root_.freeTextResources(); // must run before releasing d2dFactory_ / before
                               // eglMakeCurrent(NO_CONTEXT) below

// Windows-specific teardown path.
#if defined(_WIN32)
  // Release Direct2D resources — order matters: the render target must
  // go before the factory that created it.
  if (renderTarget_)
    renderTarget_->Release();
  if (d2dFactory_)
    d2dFactory_->Release();
  // Destroy the native window if it was successfully created.
  if (hwnd_)
    DestroyWindow(hwnd_);
// Linux/Wayland-specific teardown path.
#else
  if (eglDisplay_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (rectProgram_)
      glDeleteProgram(rectProgram_);
    if (flatProgram_)
      glDeleteProgram(flatProgram_);
    if (texProgram_)
      glDeleteProgram(texProgram_);
    if (canvasProgram_)
      glDeleteProgram(canvasProgram_);
    if (quadVbo_)
      glDeleteBuffers(1, &quadVbo_);
    if (flatVbo_)
      glDeleteBuffers(1, &flatVbo_);
    if (eglSurface_ != EGL_NO_SURFACE)
      eglDestroySurface(eglDisplay_, eglSurface_);
    if (eglContext_ != EGL_NO_CONTEXT)
      eglDestroyContext(eglDisplay_, eglContext_);
    eglTerminate(eglDisplay_);
  }
  if (eglWindow_)
    wl_egl_window_destroy(eglWindow_);
  // Destroy the cursor surface and theme if they were created.
  if (cursorSurface_)
    wl_surface_destroy(cursorSurface_);
  if (cursorTheme_)
    wl_cursor_theme_destroy(cursorTheme_);
  // Destroy the decoration-mode object if one was requested.
  if (toplevel_decoration_)
    zxdg_toplevel_decoration_v1_destroy(toplevel_decoration_);
  // Release the pointer device if one was obtained.
  if (pointer_)
    wl_pointer_destroy(pointer_);
  // Release the seat object if one was bound.
  if (seat_)
    wl_seat_destroy(seat_);
  // Destroy the toplevel object if one was created.
  if (toplevel_)
    xdg_toplevel_destroy(toplevel_);
  // Destroy the xdg_surface wrapper if one was created.
  if (xdg_surf_)
    xdg_surface_destroy(xdg_surf_);
  // Destroy the raw surface if one was created.
  if (surface_)
    wl_surface_destroy(surface_);
  // Disconnect from the compositor if a connection was established.
  if (display_)
    wl_display_disconnect(display_);
// Ends the Windows/Linux teardown branch.
#endif
}
inline void LiteUI::setRoot(View view) {
  if (hasRoot_)
    root_.freeTextResources();
  root_ = std::move(view);
  hasRoot_ = true;
  // First pass: runs every onLayout callback (e.g. a track capturing its
  // own width into a plain variable) so any positionSource/valueSource
  // that reads such app-side state sees a real value, not whatever it
  // was default-initialized to.
  relayout();
  // Now poll dynamic sources — textSource/valueSource/positionSource/etc.
  // — so a Text with only `source` set isn't blank, and anything whose
  // source depends on state the pass above just populated computes
  // correctly instead of against stale zeros.
  checkForUpdates(root_);
  // Second pass: applies whatever checkForUpdates just wrote (e.g. the
  // now-correct style.left from positionSource) to the actual layout
  // before the first paint.
  relayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (eglReady_)
    redraw();
#endif
}

inline void LiteUI::addBox(const Box &box) {
  boxes_.push_back(box);
#if defined(_WIN32)
  // Ask Windows to repaint; the actual drawing happens in WM_PAINT.
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  // Repaint immediately if the buffer already exists; if it doesn't yet,
  // attachBuffer()'s own redraw() will pick up boxes_ once configured.
  if (eglReady_)
    redraw();
#endif
}

// Out-of-line definition of the blocking event loop.
inline void LiteUI::run() {
// Windows-specific message loop.
#if defined(_WIN32)
  // Storage for each retrieved message.
  MSG msg;
  // GetMessage blocks until a message arrives and returns 0 on WM_QUIT, ending
  // the loop.
  while (GetMessage(&msg, nullptr, 0, 0)) {
    // Translate virtual-key messages into character messages (needed for text
    // input).
    TranslateMessage(&msg);
    // Send the message on to WndProc for handling.
    DispatchMessage(&msg);
  }
// Linux/Wayland-specific event loop.
#else
  // Keep dispatching Wayland events as long as we haven't been asked to stop
  // and the connection is healthy.
  while (running_ && wl_display_dispatch(display_) != -1) {
    // event loop
  }
// Ends the Windows/Linux run() branch.
#endif
}