
// window.h
//
// Single-header, C, cross-platform window.
// GuiWindow is a plain struct with platform members selected at compile
// time via #ifdef _WIN32 — fully visible here, no opaque pointer, no
// PImpl equivalent. Functions are `static inline` so this header can be
// #included in multiple translation units without linker errors.

#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#endif

typedef struct GuiWindow {
  int width;
  int height;

#if defined(_WIN32)
  HWND hwnd;
#else
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct xdg_wm_base *wm_base;
  struct wl_surface *surface;
  struct xdg_surface *xdg_surf;
  struct xdg_toplevel *toplevel;
  bool configured;
  bool running;
#endif
} GuiWindow;

/* Public API */

/* Creates and shows a window. Returns NULL on failure (no exceptions in C). */
static inline GuiWindow *gui_window_create(int width, int height, const char *title);
/* Destroys the window and releases platform resources. Safe to call with NULL. */
static inline void gui_window_destroy(GuiWindow *w);
/* Blocks, running the platform event loop until the window is closed. */
static inline void gui_window_run(GuiWindow *w);

/* ---- implementation ---- */

#if defined(_WIN32)

static inline wchar_t *gui_window_to_wide(const char *s) {
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
  if (!w) return NULL;
  MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wlen);
  return w;
}

static inline LRESULT CALLBACK gui_window_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                                   LPARAM lp) {
  GuiWindow *self = NULL;

  if (msg == WM_NCCREATE) {
    /* WM_NCCREATE is the very first message a window receives, sent
       during CreateWindowExW itself, before the window is usable. */
    CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
    self = (GuiWindow *)cs->lpCreateParams;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (GuiWindow *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  }

  switch (msg) {
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

static inline GuiWindow *gui_window_create(int width, int height, const char *title) {
  GuiWindow *w = (GuiWindow *)calloc(1, sizeof(GuiWindow));
  if (!w) return NULL;
  w->width = width;
  w->height = height;

  HINSTANCE hInst = GetModuleHandleW(NULL);
  const wchar_t *className = L"GuiWindowClass";

  WNDCLASSW wc;
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = gui_window_wndproc;
  wc.hInstance = hInst;
  wc.lpszClassName = className;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassW(&wc);

  wchar_t *wtitle = gui_window_to_wide(title);
  if (!wtitle) {
    free(w);
    return NULL;
  }

  w->hwnd = CreateWindowExW(0, className, wtitle, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL,
                            NULL, hInst, w);
  free(wtitle);

  if (!w->hwnd) {
    fprintf(stderr, "CreateWindowExW failed\n");
    free(w);
    return NULL;
  }
  ShowWindow(w->hwnd, SW_SHOWDEFAULT);
  UpdateWindow(w->hwnd);
  return w;
}

static inline void gui_window_destroy(GuiWindow *w) {
  if (!w) return;
  if (w->hwnd) DestroyWindow(w->hwnd);
  free(w);
}

static inline void gui_window_run(GuiWindow *w) {
  (void)w;
  MSG msg;
  while (GetMessage(&msg, NULL, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

#else /* Linux / Wayland */

static inline void gui_window_wm_base_ping(void *data, struct xdg_wm_base *base,
                                           uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener gui_window_wm_base_listener = {
    gui_window_wm_base_ping};

static inline void gui_window_surface_configure(void *data, struct xdg_surface *xs,
                                                 uint32_t serial) {
  GuiWindow *self = (GuiWindow *)data;
  xdg_surface_ack_configure(xs, serial);
  self->configured = true;
}
static const struct xdg_surface_listener gui_window_surf_listener = {
    gui_window_surface_configure};

static inline void gui_window_toplevel_configure(void *data, struct xdg_toplevel *tl,
                                                 int32_t width, int32_t height,
                                                 struct wl_array *states) {
  (void)data;
  (void)tl;
  (void)width;
  (void)height;
  (void)states;
  /* Compositor's suggested size — ignored until Chapter 3, where we
     actually have a swap chain to resize. */
}
static inline void gui_window_toplevel_close(void *data, struct xdg_toplevel *tl) {
  (void)tl;
  ((GuiWindow *)data)->running = false;
}
static const struct xdg_toplevel_listener gui_window_toplevel_listener = {
    gui_window_toplevel_configure, gui_window_toplevel_close};

static inline void gui_window_registry_global(void *data, struct wl_registry *registry,
                                              uint32_t name, const char *interface,
                                              uint32_t version) {
  (void)version;
  GuiWindow *self = (GuiWindow *)data;
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    self->compositor = (struct wl_compositor *)wl_registry_bind(
        registry, name, &wl_compositor_interface, 4);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    self->wm_base = (struct xdg_wm_base *)wl_registry_bind(
        registry, name, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(self->wm_base, &gui_window_wm_base_listener, self);
  }
}
static inline void gui_window_registry_remove(void *data, struct wl_registry *registry,
                                              uint32_t name) {
  (void)data;
  (void)registry;
  (void)name;
}
static const struct wl_registry_listener gui_window_registry_listener = {
    gui_window_registry_global, gui_window_registry_remove};

static inline GuiWindow *gui_window_create(int width, int height, const char *title) {
  GuiWindow *w = (GuiWindow *)calloc(1, sizeof(GuiWindow));
  if (!w) return NULL;
  w->width = width;
  w->height = height;
  w->running = true;

  w->display = wl_display_connect(NULL);
  if (!w->display) {
    fprintf(stderr, "Failed to connect to Wayland display\n");
    free(w);
    return NULL;
  }

  struct wl_registry *registry = wl_display_get_registry(w->display);
  wl_registry_add_listener(registry, &gui_window_registry_listener, w);
  /* Round-trip blocks until the server has answered every request sent
     so far — the only way to get bind() results synchronously, since
     globals normally arrive as async events. */
  wl_display_roundtrip(w->display);

  if (!w->compositor || !w->wm_base) {
    fprintf(stderr, "Missing required Wayland globals\n");
    wl_display_disconnect(w->display);
    free(w);
    return NULL;
  }

  w->surface = wl_compositor_create_surface(w->compositor);
  w->xdg_surf = xdg_wm_base_get_xdg_surface(w->wm_base, w->surface);
  xdg_surface_add_listener(w->xdg_surf, &gui_window_surf_listener, w);

  w->toplevel = xdg_surface_get_toplevel(w->xdg_surf);
  xdg_toplevel_add_listener(w->toplevel, &gui_window_toplevel_listener, w);
  xdg_toplevel_set_title(w->toplevel, title);

  wl_surface_commit(w->surface); /* triggers the first configure event */
  return w;
}

static inline void gui_window_destroy(GuiWindow *w) {
  if (!w) return;
  if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
  if (w->xdg_surf) xdg_surface_destroy(w->xdg_surf);
  if (w->surface) wl_surface_destroy(w->surface);
  if (w->display) wl_display_disconnect(w->display);
  free(w);
}

static inline void gui_window_run(GuiWindow *w) {
  while (w->running && wl_display_dispatch(w->display) != -1) {
    /* event loop */
  }
}

#endif

#endif /* GUI_WINDOW_H */


===================================

// main.c
#include "window.h"

int main(void) {
  GuiWindow *w = gui_window_create(800, 600, "My Window");
  if (!w) {
    return 1; // creation failed, message already printed by window.h
  }

  gui_window_run(w); // blocks until the window is closed

  gui_window_destroy(w);
  return 0;
}