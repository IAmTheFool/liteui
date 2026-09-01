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
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#else
#include "xdg-decoration-client-protocol.h" // Generated client bindings for the xdg-decoration protocol (server-side vs client-side decorations).
#include "xdg-shell-client-protocol.h" // Generated client bindings for the xdg-shell protocol (toplevel windows, configure events).
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h> // Core Wayland client protocol: displays, registries, surfaces, shm.
#endif

// Plain RGB color, one byte per channel.
struct Color {
  uint8_t r = 0, g = 0, b = 0;
};

// A static, axis-aligned filled rectangle the caller wants drawn on the
// window. Position is in window-local pixel coordinates, (0,0) at top-left.
struct Box {
  int width = 0;
  int height = 0;
  int pos_x = 0;
  int pos_y = 0;
  Color color;
};

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
enum class Justify { Start, End, Center, SpaceBetween, SpaceAround, SpaceEvenly };
// Note: flex-wrap and align-content are intentionally not implemented yet —
// this engine only supports a single flex line. Revisit once wrap is needed.
enum class Align { Start, End, Center, Stretch };

struct EdgeInsets {
  float top = 0, right = 0, bottom = 0, left = 0;
  static EdgeInsets all(float v) { return {v, v, v, v}; }
};

struct Style {
  Size width = Size::fit();
  Size height = Size::fit();

  EdgeInsets margin;
  EdgeInsets padding;

  FlexDirection direction = FlexDirection::Row;
  Justify justifyContent = Justify::Start;
  Align alignItems = Align::Stretch;
  float gap = 0;

  float flexGrow = 0;
  float flexShrink = 1;

  Color backgroundColor{255, 255, 255};
  float borderWidth = 0;
  Color borderColor{0, 0, 0};
  float borderRadius = 0;
};

// A node in the retained layout tree. Set `style` and `children`; the engine
// fills in `computed` (absolute window pixel coordinates) during layout.
// Renderers only ever read `computed`, never re-derive it from `style`.
class View {
public:
  Style style;
  std::vector<View> children;

  struct Computed {
    float x = 0, y = 0, w = 0, h = 0; // border-box, absolute window coords
  } computed;

  void addChild(View child) { children.push_back(std::move(child)); }
};

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
// Known limitations (fine for v1, revisit if needed): single flex line only
// (no wrap), no min/max size constraints, percentages resolve to 0 when an
// ancestor's size is itself Fit (indefinite).
namespace liteui_layout {

struct Natural { float w, h; };

inline float resolveAxis(const Size &s, float available, bool definite, float fitValue) {
  switch (s.kind) {
  case Size::Kind::Fixed: return s.value;
  case Size::Kind::Percentage: return definite ? available * s.value / 100.0f : fitValue;
  case Size::Kind::Full: return definite ? available : fitValue;
  case Size::Kind::Fit: return fitValue;
  }
  return fitValue;
}

inline Natural measureNatural(const View &node, float availW, float availH,
                              bool wDefinite, bool hDefinite) {
  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  bool needW = node.style.width.kind == Size::Kind::Fit;
  bool needH = node.style.height.kind == Size::Kind::Fit;

  float w = resolveAxis(node.style.width, availW, wDefinite, 0);
  float h = resolveAxis(node.style.height, availH, hDefinite, 0);
  if ((!needW && !needH) || node.children.empty()) return {w, h};

  float innerW = (wDefinite && !needW) ? max(0.0f, w - pad.left - pad.right) : availW;
  float innerH = (hDefinite && !needH) ? max(0.0f, h - pad.top - pad.bottom) : availH;

  float mainTotal = 0, crossMax = 0;
  for (size_t i = 0; i < node.children.size(); ++i) {
    const View &c = node.children[i];
    Natural cn = measureNatural(c, innerW, innerH, wDefinite || !needW, hDefinite || !needH);
    float mm = c.style.margin.left + c.style.margin.right;
    float mv = c.style.margin.top + c.style.margin.bottom;
    float childMain = horizontal ? cn.w + mm : cn.h + mv;
    float childCross = horizontal ? cn.h + mv : cn.w + mm;
    mainTotal += childMain;
    if (i + 1 < node.children.size()) mainTotal += node.style.gap;
    crossMax = max(crossMax, childCross);
  }
  if (needW) w = (horizontal ? mainTotal : crossMax) + pad.left + pad.right;
  if (needH) h = (horizontal ? crossMax : mainTotal) + pad.top + pad.bottom;
  return {w, h};
}

inline void placeNode(View &node, float x, float y, float w, float h) {
  node.computed = {x, y, w, h};
  if (node.children.empty()) return;

  bool horizontal = node.style.direction == FlexDirection::Row;
  const EdgeInsets &pad = node.style.padding;
  float contentX = x + pad.left, contentY = y + pad.top;
  float contentW = max(0.0f, w - pad.left - pad.right);
  float contentH = max(0.0f, h - pad.top - pad.bottom);
  float mainAvail = horizontal ? contentW : contentH;
  float crossAvail = horizontal ? contentH : contentW;

  size_t n = node.children.size();
  std::vector<float> basis(n), cross(n), mMainS(n), mMainE(n), mCrossS(n), mCrossE(n);
  float usedMain = 0, growSum = 0, shrinkSum = 0;

  for (size_t i = 0; i < n; ++i) {
    const View &c = node.children[i];
    Natural cn = measureNatural(c, contentW, contentH, true, true);
    basis[i] = horizontal ? cn.w : cn.h;
    cross[i] = horizontal ? cn.h : cn.w;
    mMainS[i] = horizontal ? c.style.margin.left : c.style.margin.top;
    mMainE[i] = horizontal ? c.style.margin.right : c.style.margin.bottom;
    mCrossS[i] = horizontal ? c.style.margin.top : c.style.margin.left;
    mCrossE[i] = horizontal ? c.style.margin.bottom : c.style.margin.right;
    usedMain += basis[i] + mMainS[i] + mMainE[i];
    growSum += c.style.flexGrow;
    shrinkSum += c.style.flexShrink;
    if (i + 1 < n) usedMain += node.style.gap;
  }

  float leftover = mainAvail - usedMain;
  std::vector<float> finalMain(n);
  for (size_t i = 0; i < n; ++i) {
    float extra = 0;
    if (leftover > 0 && growSum > 0)
      extra = leftover * (node.children[i].style.flexGrow / growSum);
    else if (leftover < 0 && shrinkSum > 0)
      extra = leftover * (node.children[i].style.flexShrink / shrinkSum);
    finalMain[i] = max(0.0f, basis[i] + extra);
  }

  float totalUsed = 0;
  for (size_t i = 0; i < n; ++i) {
    totalUsed += finalMain[i] + mMainS[i] + mMainE[i];
    if (i + 1 < n) totalUsed += node.style.gap;
  }
  float freeSpace = max(0.0f, mainAvail - totalUsed);
  float startOffset = 0, between = node.style.gap;
  switch (node.style.justifyContent) {
  case Justify::Start: break;
  case Justify::End: startOffset = freeSpace; break;
  case Justify::Center: startOffset = freeSpace / 2; break;
  case Justify::SpaceBetween: if (n > 1) between += freeSpace / (n - 1); break;
  case Justify::SpaceAround: { float each = n ? freeSpace / n : 0; startOffset = each / 2; between += each; break; }
  case Justify::SpaceEvenly: { float each = freeSpace / (n + 1); startOffset = each; between += each; break; }
  }

  float cursor = (horizontal ? contentX : contentY) + startOffset;
  for (size_t i = 0; i < n; ++i) {
    View &c = node.children[i];
    cursor += mMainS[i];

    bool explicitCross = horizontal ? c.style.height.kind != Size::Kind::Fit
                                    : c.style.width.kind != Size::Kind::Fit;
    float finalCross = cross[i];
    if (node.style.alignItems == Align::Stretch && !explicitCross)
      finalCross = max(0.0f, crossAvail - mCrossS[i] - mCrossE[i]);

    float crossOffset;
    switch (node.style.alignItems) {
    case Align::End: crossOffset = crossAvail - finalCross - mCrossE[i]; break;
    case Align::Center: crossOffset = (crossAvail - finalCross) / 2; break;
    default: crossOffset = mCrossS[i]; break; // Start & Stretch
    }

    float cx = horizontal ? cursor : contentX + crossOffset;
    float cy = horizontal ? contentY + crossOffset : cursor;
    float cw = horizontal ? finalMain[i] : finalCross;
    float ch = horizontal ? finalCross : finalMain[i];

    placeNode(c, cx, cy, cw, ch);
    cursor += finalMain[i] + mMainE[i] + between;
  }
}

inline void layoutRoot(View &root, float windowW, float windowH) {
  Natural n = measureNatural(root, windowW, windowH, true, true);
  placeNode(root, 0, 0, n.w, n.h);
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
    if (hasRoot_)
      liteui_layout::layoutRoot(root_, static_cast<float>(width_), static_cast<float>(height_));
  }

// Windows-only member/method block.
#if defined(_WIN32)
  // Native window handle; null until CreateWindowExW succeeds.
  HWND hwnd_ = nullptr;

  // Helper converting a UTF-8 std::string to the UTF-16 wide string Win32's *W
  // APIs require.
  static std::wstring toWide(const std::string &s) {
    // First pass: ask Windows how many wide characters the conversion will need
    // (including the null terminator).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    // Allocate a wide string of exactly that length, zero-initialized.
    std::wstring w(wlen, L'\0');
    // Second pass: actually perform the conversion into the allocated buffer.
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
    // Hand the converted string back to the caller.
    return w;
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
    // Repaint request: redraw all boxes using GDI.
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      if (self)
        self->paintBoxes(hdc);
        self->paintRoot(hdc);
      EndPaint(hwnd, &ps);
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

  // Draws every queued box into the given device context via GDI.
  void paintBoxes(HDC hdc) {
    for (const auto &b : boxes_) {
      RECT r{b.pos_x, b.pos_y, b.pos_x + b.width, b.pos_y + b.height};
      HBRUSH brush = CreateSolidBrush(RGB(b.color.r, b.color.g, b.color.b));
      FillRect(hdc, &r, brush);
      DeleteObject(brush);
    }
  }


  // Draws the layout tree (if any) using native GDI RoundRect, which handles
  // border-radius directly — no manual pixel math needed on this platform.
  void paintRoot(HDC hdc) {
    if (hasRoot_)
      paintView(hdc, root_);
  }

  void paintView(HDC hdc, const View &v) {
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    HBRUSH bg = CreateSolidBrush(RGB(s.backgroundColor.r, s.backgroundColor.g, s.backgroundColor.b));
    HPEN pen = s.borderWidth > 0
        ? CreatePen(PS_SOLID, static_cast<int>(s.borderWidth),
                    RGB(s.borderColor.r, s.borderColor.g, s.borderColor.b))
        : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(hdc, bg);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int d = static_cast<int>(s.borderRadius) * 2;
    RoundRect(hdc, x, y, x + w, y + h, d, d);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bg);
    if (s.borderWidth > 0) DeleteObject(pen);
    for (const auto &child : v.children)
      paintView(hdc, child);
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
  wl_buffer *buffer_ =
      nullptr; // The currently attached pixel buffer shown on screen.
  wl_seat *seat_ = nullptr; // The seat global, representing one user's set of
                            // input devices (keyboard/mouse/etc.).

  wl_pointer *pointer_ =
      nullptr; // The pointer (mouse) device obtained from
               // the seat, once it announces pointer capability.

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
  static constexpr uint32_t BTN_LEFT_CODE = 0x110; // linux/input-event-codes.h

  double pointer_x_ = 0,
         pointer_y_ = 0; // Last known pointer position within
                         // the surface, in surface-local coordinates.

  bool maximized_ =
      false; // Tracks whether the window currently believes
             // itself to be maximized (for toggling and icon state).

  // keep mapped for the buffer's lifetime instead of mmap/unmap per attach,
  // since we now need to repaint(maximize toggle,etc) after the initial attach.
  // File descriptor for the anonymous shared-memory-backed buffer; -1 until
  // created.
  int bufferFd_ = -1;

  uint8_t *bufferData_ = nullptr; // Pointer to the mmap'd pixel data backing
                                  // bufferFd_; null until attachBuffer() runs.

  int bufferSize_ =
      0; // Total size in bytes of the mapped buffer (stride * height).

  bool configured_ =
      false; // Set true once the compositor has sent its first
             // configure event, meaning we're allowed to attach a buffer.

  bool running_ = true; // Controls the event loop in run(); set false to
                        // request a clean exit.

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
    // On the very first configure, no buffer exists yet, so create and attach
    // one now.
    if (!self->buffer_)
      self->attachBuffer();
  }
  // Listener struct binding surfaceConfigure to xdg_surface's single event.
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  // Called when the compositor suggests a new size/state for the toplevel.
  static void toplevelConfigure(void *, xdg_toplevel *, int32_t, int32_t,
                                wl_array *) {
    // Compositor's suggested size — ignored until Chapter 3, where we
    // actually have a swap chain to resize.
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
  static void pointerEnter(void *data, wl_pointer *, uint32_t, wl_surface *,
                           wl_fixed_t sx, wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Convert Wayland's fixed-point x coordinate to a double and store it.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Same for the y coordinate.
    self->pointer_y_ = wl_fixed_to_double(sy);
  }
  // Called when the pointer leaves this surface; nothing to track here.
  static void pointerLeave(void *, wl_pointer *, uint32_t, wl_surface *) {}
  // Called on every pointer movement while over this surface.
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Update the stored x position.
    self->pointer_x_ = wl_fixed_to_double(sx);
    // Update the stored y position.
    self->pointer_y_ = wl_fixed_to_double(sy);
  }
  // Called on scroll/axis events; not used by this minimal window.
  static void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t,
                          wl_fixed_t) {}
  // Called on every pointer button press/release.
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state) {
    // Recover the owning LiteUI.
    auto *self = static_cast<LiteUI *>(data);
    // Ignore anything that isn't a left-button press (we don't handle
    // right-click or releases).
    if (button != BTN_LEFT_CODE || state != WL_POINTER_BUTTON_STATE_PRESSED)
      return;
    // Route the click to the custom titlebar hit-testing logic, passing the
    // event serial (needed for interactive move).
    self->handleTitlebarClick(serial);
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
  // Allocates the shared-memory pixel buffer and attaches it to the surface for
  // the first time.
  void attachBuffer() {
    // Each pixel is 4 bytes (XBGR8888), so a row's byte length is width * 4.
    const int stride = width_ * 4;
    // Total buffer size is one row's bytes times the number of rows.
    bufferSize_ = stride * height_;

    // Create an anonymous, memory-backed file descriptor to hold the pixel
    // data.
    bufferFd_ = memfd_create("gui-window-buf", 0);
    // Bail out if the kernel couldn't give us one.
    if (bufferFd_ < 0)
      throw std::runtime_error("memfd_create failed");
    // Resize that anonymous file to exactly the buffer size we need.
    if (ftruncate(bufferFd_, bufferSize_) < 0) {
      // Clean up the fd before propagating the error.
      close(bufferFd_);
      throw std::runtime_error("ftruncate failed");
    }
    // Kept mapped (not munmap'd) so redraw() can repaint after maximize
    // toggles etc, instead of only ever painting once at attach time.
    // Map the fd's memory into our address space, readable/writable and shared
    // with the compositor.
    bufferData_ = static_cast<uint8_t *>(mmap(nullptr, bufferSize_,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, bufferFd_, 0));
    // Bail out if the mapping failed.
    if (bufferData_ == MAP_FAILED) {
      // Clean up the fd before propagating the error.
      close(bufferFd_);
      throw std::runtime_error("mmap failed");
    }

    // Wrap the fd in a wl_shm_pool so the compositor can carve buffers out of
    // it.
    wl_shm_pool *pool = wl_shm_create_pool(shm_, bufferFd_, bufferSize_);
    // Create a single buffer covering the whole pool, describing
    // width/height/stride/pixel format.
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride,
                                        WL_SHM_FORMAT_XBGR8888);
    // The pool object itself isn't needed anymore once the buffer exists.
    wl_shm_pool_destroy(pool);

    // Paint the initial frame into the freshly mapped buffer and present it.
    redraw();
  }

  // ---- pixel helpers (XBGR8888: memory byte order R,G,B,X per pixel) ----
  // Writes a single opaque pixel at (x, y), silently ignoring out-of-bounds
  // coordinates.
  void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    // Skip drawing anything outside the buffer's bounds.
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return;
    // Compute the byte address of this pixel: row offset plus column offset,
    // four bytes per pixel.
    uint8_t *px = bufferData_ + (static_cast<size_t>(y) * width_ + x) * 4;
    // First byte in memory is red (per the XBGR8888 comment above).
    px[0] = r;
    // Second byte is green.
    px[1] = g;
    // Third byte is blue.
    px[2] = b;
    // Fourth byte is the unused/alpha channel; forced fully opaque.
    px[3] = 0xFF;
  }
  // Fills an axis-aligned rectangle with a solid color by calling setPixel for
  // every point inside it.
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    // Iterate every row of the rectangle.
    for (int y = y0; y < y0 + h; ++y)
      // Iterate every column of the current row.
      for (int x = x0; x < x0 + w; ++x)
        // Paint this pixel with the requested color.
        setPixel(x, y, r, g, b);
  }


  // Fills an axis-aligned rectangle with rounded corners, clamping the
  // radius so it can't exceed half the shorter side. Per-pixel distance
  // check against each corner's circle center — fine at this scale, not
  // meant for huge boxes.
  void fillRoundedRect(int x0, int y0, int w, int h, int radius, uint8_t r,
                       uint8_t g, uint8_t b) {
    if (w <= 0 || h <= 0) return;
    radius = std::max(0, std::min({radius, w / 2, h / 2}));
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        int cx = -1, cy = -1;
        if (x < radius && y < radius) { cx = radius; cy = radius; }
        else if (x >= w - radius && y < radius) { cx = w - radius - 1; cy = radius; }
        else if (x < radius && y >= h - radius) { cx = radius; cy = h - radius - 1; }
        else if (x >= w - radius && y >= h - radius) { cx = w - radius - 1; cy = h - radius - 1; }
        bool inside = true;
        if (cx >= 0) {
          int dx = x - cx, dy = y - cy;
          inside = (dx * dx + dy * dy) <= radius * radius;
        }
        if (inside) setPixel(x0 + x, y0 + y, r, g, b);
      }
    }
  }

  // Draws one View (background + border) using its already-computed layout,
  // then recurses into children. Border is drawn as an outer rounded rect in
  // borderColor with an inner rounded rect in backgroundColor inset by
  // borderWidth — simple and correct for a uniform border on all sides.
  void renderView(const View &v) {
    const Style &s = v.style;
    int x = static_cast<int>(v.computed.x), y = static_cast<int>(v.computed.y);
    int w = static_cast<int>(v.computed.w), h = static_cast<int>(v.computed.h);
    int radius = static_cast<int>(s.borderRadius);
    if (s.borderWidth > 0) {
      fillRoundedRect(x, y, w, h, radius, s.borderColor.r, s.borderColor.g, s.borderColor.b);
      int bw = static_cast<int>(s.borderWidth);
      fillRoundedRect(x + bw, y + bw, std::max(0, w - 2 * bw), std::max(0, h - 2 * bw),
                      std::max(0, radius - bw), s.backgroundColor.r, s.backgroundColor.g,
                      s.backgroundColor.b);
    } else {
      fillRoundedRect(x, y, w, h, radius, s.backgroundColor.r, s.backgroundColor.g, s.backgroundColor.b);
    }
    for (const auto &child : v.children)
      renderView(child);
  }

  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  // Draws a crude line between two points by linear interpolation, stepping
  // once per pixel along the longer axis.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g,
                uint8_t b) {
    // Horizontal distance to cover.
    int dx = x1 - x0, dy = y1 - y0;
    // Number of steps to take is the larger of the two axis distances, so we
    // don't skip pixels.
    int steps = std::max(std::abs(dx), std::abs(dy));
    // Degenerate case: start and end are the same point, so just plot it and
    // return.
    if (steps == 0) {
      setPixel(x0, y0, r, g, b);
      return;
    }
    // Walk from 0 to steps inclusive, interpolating along the way.
    for (int i = 0; i <= steps; ++i) {
      // Interpolated x position at this step (integer division truncates, which
      // is fine for icon-scale lines).
      int x = x0 + dx * i / steps;
      // Interpolated y position at this step.
      int y = y0 + dy * i / steps;
      // Plot the interpolated pixel.
      setPixel(x, y, r, g, b);
      // Also plot the pixel one to the right, giving the line a cheap 2px-wide
      // stroke.
      setPixel(x + 1, y, r, g, b); // cheap 2px-wide stroke
    }
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
  void redraw() {
    // Nothing to draw into yet if the buffer hasn't been mapped.
    if (!bufferData_)
      return;
    // Clear the whole buffer to white as the plain content-area background.
    std::memset(bufferData_, 0xFF, bufferSize_); // content area
    // Paint queued boxes on top of the plain background, before the titlebar
    // so it stays on top.
    for (const auto &b : boxes_)
      fillRect(b.pos_x, b.pos_y, b.width, b.height, b.color.r, b.color.g,
               b.color.b);
    if (hasRoot_)
      renderView(root_);
    // Paint the titlebar and its buttons on top of that background.
    drawTitlebar();
    // Tell the compositor this buffer is what the surface should display, at
    // offset (0,0).
    wl_surface_attach(surface_, buffer_, 0, 0);
    // Mark the whole buffer area as changed so the compositor knows to
    // re-composite it.
    wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
    // Submit the attach+damage as an atomic update to the compositor.
    wl_surface_commit(surface_);
  }

  // Hit-tests a left-click against the titlebar buttons and drag region.
  void handleTitlebarClick(uint32_t serial) {
    // Clicks below the titlebar strip aren't ours to handle (no widget tree yet
    // in this minimal header).
    if (pointer_y_ >= kTitlebarHeight)
      return; // no widget tree yet in this minimal header

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
    // empty titlebar space -> drag to move
    // Any other click in the titlebar (empty space) starts an interactive move,
    // if we have a seat to drive it.
    if (seat_)
      xdg_toplevel_move(toplevel_, seat_, serial);
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
// Windows-specific teardown path.
#if defined(_WIN32)
  // Destroy the native window if it was successfully created.
  if (hwnd_)
    DestroyWindow(hwnd_);
// Linux/Wayland-specific teardown path.
#else
  // Release the shared pixel buffer object if one was created.
  if (buffer_)
    wl_buffer_destroy(buffer_);
  // Unmap the shared memory region if it was mapped.
  if (bufferData_)
    munmap(bufferData_, bufferSize_);
  // Close the backing file descriptor if one was created.
  if (bufferFd_ >= 0)
    close(bufferFd_);
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
  root_ = std::move(view);
  hasRoot_ = true;
  relayout();
#if defined(_WIN32)
  if (hwnd_)
    InvalidateRect(hwnd_, nullptr, FALSE);
#else
  if (bufferData_)
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
  if (bufferData_)
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