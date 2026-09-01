// include/liteui.hpp
//
// Single-header, PImpl-free cross-platform window.
// Platform members/methods are selected at compile time via #ifdef,
// so there's exactly one GuiWindow definition per build — no vtable,
// no heap-allocated Impl, no indirection through a pointer.

#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <cmath>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#endif

class GuiWindow
{
public:
  explicit GuiWindow(int width = 800, int height = 600,
                     const std::string &title = "Window");
  ~GuiWindow();
  GuiWindow(const GuiWindow &) = delete;
  GuiWindow &operator=(const GuiWindow &) = delete;

  void run(); // blocks, runs the event loop

private:
  int width_;
  int height_;

#if defined(_WIN32)
  HWND hwnd_ = nullptr;

  static std::wstring toWide(const std::string &s)
  {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), wlen);
    return w;
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
  {
    GuiWindow *self = nullptr;

    if (msg == WM_NCCREATE)
    {
      // WM_NCCREATE is the very first message a window receives, sent
      // during CreateWindowExW itself, before the window is usable.
      auto cs = reinterpret_cast<CREATESTRUCTW *>(lp);
      self = static_cast<GuiWindow *>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    else
    {
      self = reinterpret_cast<GuiWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg)
    {
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

#else // Linux / Wayland
  wl_display *display_ = nullptr;
  wl_compositor *compositor_ = nullptr;
  xdg_wm_base *wm_base_ = nullptr;
  wl_surface *surface_ = nullptr;
  xdg_surface *xdg_surf_ = nullptr;
  xdg_toplevel *toplevel_ = nullptr;
  wl_shm *shm_ = nullptr;
  wl_buffer *buffer_ = nullptr;

  wl_seat *seat_ = nullptr;
  wl_pointer *pointer_ = nullptr;
  zxdg_decoration_manager_v1 *decoration_manager_ = nullptr;
  zxdg_toplevel_decoration_v1 *toplevel_decoration_ = nullptr;

  // software rendered CSD titlebar. No cairo here, so buttons are drawn with raw pixel writes straight into the shm buffer.

  static constexpr int kTitlebarHeight = 32;
  static constexpr int kButtonSize = 18;
  static constexpr int kButtonMargin = 8;
  static constexpr uint32_t BTN_LEFT_CODE = 0x110; // linux/input-event-codes.h

  double pointer_x_ = 0, pointer_y_ = 0;
  bool maximized_ = false;

  // keep mapped for the buffer's lifetime instead of mmap/unmap per attach, since we now need to repaint(maximize toggle,etc) after the initial attach.
  int bufferFd_ = -1;
  uint8_t *bufferData_ = nullptr;
  int bufferSize_ = 0;

  bool configured_ = false;
  bool running_ = true;

  static void wmBasePing(void *, xdg_wm_base *base, uint32_t serial)
  {
    xdg_wm_base_pong(base, serial);
  }
  static constexpr xdg_wm_base_listener wmBaseListener = {wmBasePing};

  static void surfaceConfigure(void *data, xdg_surface *xs, uint32_t serial)
  {
    auto *self = static_cast<GuiWindow *>(data);
    xdg_surface_ack_configure(xs, serial);
    self->configured_ = true;
    if (!self->buffer_)
      self->attachBuffer();
  }
  static constexpr xdg_surface_listener surfListener = {surfaceConfigure};

  static void toplevelConfigure(void *, xdg_toplevel *, int32_t, int32_t,
                                wl_array *)
  {
    // Compositor's suggested size — ignored until Chapter 3, where we
    // actually have a swap chain to resize.
  }
  static void toplevelClose(void *data, xdg_toplevel *)
  {
    static_cast<GuiWindow *>(data)->running_ = false;
  }
  static constexpr xdg_toplevel_listener toplevelListener = {toplevelConfigure,
                                                             toplevelClose};

  // wl_seat/wl_pointer

  static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps)
  {
    auto *self = static_cast<GuiWindow *>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_)
    {
      self->pointer_ = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(self->pointer_, &pointerListener, self);
    }
  }

  static void seatName(void *, wl_seat *, const char *) {}
  static constexpr wl_seat_listener seatListener = {seatCapabilities, seatName};

  static void pointerEnter(void *data, wl_pointer *, uint32_t, wl_surface *,
                           wl_fixed_t sx, wl_fixed_t sy)
  {
    auto *self = static_cast<GuiWindow *>(data);
    self->pointer_x_ = wl_fixed_to_double(sx);
    self->pointer_y_ = wl_fixed_to_double(sy);
  }
  static void pointerLeave(void *, wl_pointer *, uint32_t, wl_surface *) {}
  static void pointerMotion(void *data, wl_pointer *, uint32_t, wl_fixed_t sx,
                            wl_fixed_t sy)
  {
    auto *self = static_cast<GuiWindow *>(data);
    self->pointer_x_ = wl_fixed_to_double(sx);
    self->pointer_y_ = wl_fixed_to_double(sy);
  }
  static void pointerAxis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
  static void pointerButton(void *data, wl_pointer *, uint32_t serial, uint32_t,
                            uint32_t button, uint32_t state)
  {
    auto *self = static_cast<GuiWindow *>(data);
    if (button != BTN_LEFT_CODE || state != WL_POINTER_BUTTON_STATE_PRESSED)
      return;
    self->handleTitlebarClick(serial);
  }
  static constexpr wl_pointer_listener pointerListener = {
      pointerEnter, pointerLeave, pointerMotion, pointerButton, pointerAxis};

  static void registryGlobal(void *data, wl_registry *registry, uint32_t name,
                             const char *interface, uint32_t)
  {
    auto *self = static_cast<GuiWindow *>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
      self->compositor_ = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    }
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
      self->wm_base_ = static_cast<xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      xdg_wm_base_add_listener(self->wm_base_, &wmBaseListener, self);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
      self->shm_ = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
      self->seat_ = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      wl_seat_add_listener(self->seat_, &seatListener, self);
    }
    else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
    {
      self->decoration_manager_ = static_cast<zxdg_decoration_manager_v1 *>(
          wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
    }
  }
  static void registryRemove(void *, wl_registry *, uint32_t) {}
  static constexpr wl_registry_listener registryListener = {registryGlobal,
                                                            registryRemove};
  void attachBuffer()
  {
    const int stride = width_ * 4;
    bufferSize_ = stride * height_;

    bufferFd_ = memfd_create("gui-window-buf", 0);
    if (bufferFd_ < 0)
      throw std::runtime_error("memfd_create failed");
    if (ftruncate(bufferFd_, bufferSize_) < 0)
    {
      close(bufferFd_);
      throw std::runtime_error("ftruncate failed");
    }
    // Kept mapped (not munmap'd) so redraw() can repaint after maximize
    // toggles etc, instead of only ever painting once at attach time.
    bufferData_ = static_cast<uint8_t *>(
        mmap(nullptr, bufferSize_, PROT_READ | PROT_WRITE, MAP_SHARED, bufferFd_, 0));
    if (bufferData_ == MAP_FAILED)
    {
      close(bufferFd_);
      throw std::runtime_error("mmap failed");
    }

    wl_shm_pool *pool = wl_shm_create_pool(shm_, bufferFd_, bufferSize_);
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride, WL_SHM_FORMAT_XBGR8888);
    wl_shm_pool_destroy(pool);

    redraw();
  }

  // ---- pixel helpers (XBGR8888: memory byte order R,G,B,X per pixel) ----
  void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b)
  {
    if (x < 0 || y < 0 || x >= width_ || y >= height_)
      return;
    uint8_t *px = bufferData_ + (static_cast<size_t>(y) * width_ + x) * 4;
    px[0] = r;
    px[1] = g;
    px[2] = b;
    px[3] = 0xFF;
  }
  void fillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
  {
    for (int y = y0; y < y0 + h; ++y)
      for (int x = x0; x < x0 + w; ++x)
        setPixel(x, y, r, g, b);
  }
  // Simple stepped line, good enough for axis-aligned/diagonal 18px icons.
  void drawLine(int x0, int y0, int x1, int y1, uint8_t r, uint8_t g, uint8_t b)
  {
    int dx = x1 - x0, dy = y1 - y0;
    int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) { setPixel(x0, y0, r, g, b); return; }
    for (int i = 0; i <= steps; ++i)
    {
      int x = x0 + dx * i / steps;
      int y = y0 + dy * i / steps;
      setPixel(x, y, r, g, b);
      setPixel(x + 1, y, r, g, b); // cheap 2px-wide stroke
    }
  }

  // ---- titlebar button layout ----
  struct Rect { int x, y, w, h; };
  static bool inside(const Rect &r, double px, double py)
  {
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
  }
  Rect closeRect() const
  {
    return {width_ - kButtonMargin - kButtonSize,
            (kTitlebarHeight - kButtonSize) / 2, kButtonSize, kButtonSize};
  }
  Rect maximizeRect() const
  {
    Rect c = closeRect();
    return {c.x - kButtonMargin - kButtonSize, c.y, kButtonSize, kButtonSize};
  }
  Rect minimizeRect() const
  {
    Rect m = maximizeRect();
    return {m.x - kButtonMargin - kButtonSize, m.y, kButtonSize, kButtonSize};
  }

  void drawTitlebar()
  {
    fillRect(0, 0, width_, kTitlebarHeight, 0x2D, 0x2D, 0x2D);

    Rect minr = minimizeRect(), maxr = maximizeRect(), clsr = closeRect();
    fillRect(minr.x, minr.y, minr.w, minr.h, 0x50, 0x50, 0x50);
    fillRect(maxr.x, maxr.y, maxr.w, maxr.h, 0x50, 0x50, 0x50);
    fillRect(clsr.x, clsr.y, clsr.w, clsr.h, 0xC0, 0x39, 0x2B);

    // minimize: underscore
    drawLine(minr.x + 4, minr.y + minr.h - 5, minr.x + minr.w - 4,
             minr.y + minr.h - 5, 0xFF, 0xFF, 0xFF);
    // maximize: square outline
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + maxr.w - 4, maxr.y + 4, 0xFF, 0xFF, 0xFF);
    drawLine(maxr.x + 4, maxr.y + maxr.h - 4, maxr.x + maxr.w - 4, maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    drawLine(maxr.x + 4, maxr.y + 4, maxr.x + 4, maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    drawLine(maxr.x + maxr.w - 4, maxr.y + 4, maxr.x + maxr.w - 4, maxr.y + maxr.h - 4, 0xFF, 0xFF, 0xFF);
    // close: X
    drawLine(clsr.x + 4, clsr.y + 4, clsr.x + clsr.w - 4, clsr.y + clsr.h - 4, 0xFF, 0xFF, 0xFF);
    drawLine(clsr.x + clsr.w - 4, clsr.y + 4, clsr.x + 4, clsr.y + clsr.h - 4, 0xFF, 0xFF, 0xFF);
  }

  void redraw()
  {
    if (!bufferData_)
      return;
    std::memset(bufferData_, 0xFF, bufferSize_); // content area
    drawTitlebar();
    wl_surface_attach(surface_, buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
    wl_surface_commit(surface_);
  }

  void handleTitlebarClick(uint32_t serial)
  {
    if (pointer_y_ >= kTitlebarHeight)
      return; // no widget tree yet in this minimal header

    if (inside(closeRect(), pointer_x_, pointer_y_))
    {
      running_ = false;
      return;
    }
    if (inside(maximizeRect(), pointer_x_, pointer_y_))
    {
      if (maximized_)
        xdg_toplevel_unset_maximized(toplevel_);
      else
        xdg_toplevel_set_maximized(toplevel_);
      maximized_ = !maximized_;
      redraw();
      return;
    }
    if (inside(minimizeRect(), pointer_x_, pointer_y_))
    {
      xdg_toplevel_set_minimized(toplevel_);
      return;
    }
    // empty titlebar space -> drag to move
    if (seat_)
      xdg_toplevel_move(toplevel_, seat_, serial);
  }

#endif
};

inline GuiWindow::GuiWindow(int w, int h, const std::string &title)
    : width_(w), height_(h)
{
#if defined(_WIN32)
  HINSTANCE hInst = GetModuleHandleW(nullptr);
  const wchar_t *className = L"GuiWindowClass";

  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = className;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  std::wstring wtitle = toWide(title);

  hwnd_ = CreateWindowExW(0, className, wtitle.c_str(), WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, width_, height_,
                          nullptr, nullptr, hInst, this);
  if (!hwnd_)
  {
    throw std::runtime_error("CreateWindowExW failed");
  }
  ShowWindow(hwnd_, SW_SHOWDEFAULT);
  UpdateWindow(hwnd_);

#else
  display_ = wl_display_connect(nullptr);
  if (!display_)
    throw std::runtime_error("Failed to connect to Wayland display");

  wl_registry *registry = wl_display_get_registry(display_);
  wl_registry_add_listener(registry, &registryListener, this);
  // Round-trip blocks until the server has answered every request sent
  // so far — the only way to get bind() results synchronously, since
  // globals normally arrive as async events.
  wl_display_roundtrip(display_);

  if (!compositor_ || !wm_base_ || !shm_)
    throw std::runtime_error("Missing required Wayland globals");

  surface_ = wl_compositor_create_surface(compositor_);
  xdg_surf_ = xdg_wm_base_get_xdg_surface(wm_base_, surface_);
  xdg_surface_add_listener(xdg_surf_, &surfListener, this);

  toplevel_ = xdg_surface_get_toplevel(xdg_surf_);
  xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
  xdg_toplevel_set_title(toplevel_, title.c_str());

  wl_surface_commit(surface_); // triggers the first configure event
#endif
}

inline GuiWindow::~GuiWindow()
{
#if defined(_WIN32)
  if (hwnd_)
    DestroyWindow(hwnd_);
#else
  if (buffer_)
    wl_buffer_destroy(buffer_);
  if (bufferData_)
    munmap(bufferData_, bufferSize_);
  if (bufferFd_ >= 0)
    close(bufferFd_);
  if (toplevel_decoration_)
    zxdg_toplevel_decoration_v1_destroy(toplevel_decoration_);
  if (pointer_)
    wl_pointer_destroy(pointer_);
  if (seat_)
    wl_seat_destroy(seat_);
  if (toplevel_)
    xdg_toplevel_destroy(toplevel_);
  if (xdg_surf_)
    xdg_surface_destroy(xdg_surf_);
  if (surface_)
    wl_surface_destroy(surface_);
  if (display_)
    wl_display_disconnect(display_);
#endif
}

inline void GuiWindow::run()
{
#if defined(_WIN32)
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0))
  {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
#else
  while (running_ && wl_display_dispatch(display_) != -1)
  {
    // event loop
  }
#endif
}