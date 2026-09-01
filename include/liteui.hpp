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

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
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
  }
  static void registryRemove(void *, wl_registry *, uint32_t) {}
  static constexpr wl_registry_listener registryListener = {registryGlobal,
                                                            registryRemove};
  // static constexpr data members are implicitly inline since C++17,
  // so (unlike the old .cpp) no out-of-class definitions are needed here.
  void attachBuffer()
  {
    const int stride = width_ * 4;
    const int size = stride * height_;

    int fd = memfd_create("gui-window-buf", 0);
    if (fd < 0)
    {
      throw std::runtime_error("memfd_create failed");
    }
    if (ftruncate(fd, size) < 0)
    {
      close(fd);
      throw std::runtime_error("ftruncate failed");
    }
    void *data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED)
    {
      close(fd);
      throw std::runtime_error("mmap failed");
    }
    std::memset(data, 0xFF, size);
    munmap(data, size);

    wl_shm_pool *pool = wl_shm_create_pool(shm_, fd, size);
    buffer_ = wl_shm_pool_create_buffer(pool, 0, width_, height_, stride, WL_SHM_FORMAT_XBGR8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    wl_surface_attach(surface_, buffer_, 0, 0);
    wl_surface_damage_buffer(surface_, 0, 0, width_, height_);
    wl_surface_commit(surface_);
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