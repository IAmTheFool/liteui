// Native Wayland drag-and-drop IN, against a small fake compositor built on
// libwayland-server (same approach as clipboard_wayland.inc). It checks the
// client side of the protocol: what WlClipboard accepts, which actions it
// asks for, that it reads the drop through a pipe polled from the loop, that
// it sends finish ONLY after a negotiated action, and that offers are freed.
// Compiled out without libwayland-server (see CMakeLists.txt).
#if !defined(_WIN32) && defined(LITEUI_TEST_WAYLAND_SERVER)

#include <catch2/catch_test_macros.hpp>
#include <wayland-server.h>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

namespace liteui_test_drop_wl {

struct FakeDndComp {
  wl_display *sd = nullptr;
  wl_event_loop *loop = nullptr;
  wl_client *client = nullptr;
  wl_resource *dev = nullptr, *offer = nullptr, *surface = nullptr;
  std::vector<std::string> reqs; // what the client asked of us, in order
  std::string payload;           // sent when the client receive()s
  bool offerAlive = false;
};
FakeDndComp *gFc = nullptr;

// The client protocol header is already included (it cannot be mixed with
// wayland-server-protocol.h), so the fake compositor uses generic dispatchers:
// requests arrive as (opcode, wl_argument[]), events go out with
// wl_resource_post_event(). Opcodes follow wayland.xml.
enum { // wl_data_offer requests
  kOffAccept = 0, kOffReceive, kOffDestroy, kOffFinish, kOffSetActions
};
enum { // wl_data_device events
  kDevDataOffer = 0, kDevEnter, kDevLeave, kDevMotion, kDevDrop
};
enum { kOffEvOffer = 0, kOffEvSourceActions, kOffEvAction };

int offerDispatch(const void *, void *target, uint32_t op, const wl_message *,
                  wl_argument *a) {
  auto *r = static_cast<wl_resource *>(target);
  switch (op) {
  case kOffAccept:
    gFc->reqs.push_back(std::string("accept:") + (a[1].s ? a[1].s : "null"));
    break;
  case kOffReceive: {
    gFc->reqs.push_back(std::string("receive:") + a[0].s);
    const int fd = a[1].h; // a request's fd belongs to the handler: close it
    const char *p = gFc->payload.data();
    size_t left = gFc->payload.size();
    while (left && fd >= 0) {
      const ssize_t k = ::write(fd, p, left);
      if (k <= 0)
        break;
      p += k;
      left -= static_cast<size_t>(k);
    }
    if (fd >= 0)
      ::close(fd);
    break;
  }
  case kOffDestroy:
    gFc->reqs.push_back("offer.destroy");
    gFc->offerAlive = false;
    wl_resource_destroy(r);
    break;
  case kOffFinish:
    gFc->reqs.push_back("finish");
    break;
  case kOffSetActions:
    gFc->reqs.push_back("actions:" + std::to_string(a[0].u) + "/" +
                        std::to_string(a[1].u));
    break;
  }
  return 0;
}
int devDispatch(const void *, void *target, uint32_t op, const wl_message *,
                wl_argument *) {
  if (op == 2) // release
    wl_resource_destroy(static_cast<wl_resource *>(target));
  return 0;
}
int mgrDispatch(const void *, void *target, uint32_t op, const wl_message *,
                wl_argument *a) {
  auto *m = static_cast<wl_resource *>(target);
  if (op == 1) { // get_data_device(new_id, seat)
    wl_client *c = wl_resource_get_client(m);
    gFc->client = c;
    gFc->dev = wl_resource_create(c, &wl_data_device_interface,
                                  wl_resource_get_version(m), a[0].n);
    wl_resource_set_dispatcher(gFc->dev, devDispatch, nullptr, nullptr, nullptr);
  }
  return 0;
}
void bindMgr(wl_client *c, void *, uint32_t v, uint32_t id) {
  wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, v, id);
  wl_resource_set_dispatcher(r, mgrDispatch, nullptr, nullptr, nullptr);
}
void bindSeat(wl_client *c, void *, uint32_t v, uint32_t id) {
  wl_resource *r = wl_resource_create(c, &wl_seat_interface, v, id);
  wl_resource_post_event(r, 0 /*capabilities*/, 0u);
}
int compDispatch(const void *, void *target, uint32_t op, const wl_message *,
                 wl_argument *a) {
  auto *comp = static_cast<wl_resource *>(target);
  if (op == 0) // create_surface
    gFc->surface = wl_resource_create(wl_resource_get_client(comp),
                                      &wl_surface_interface,
                                      wl_resource_get_version(comp), a[0].n);
  return 0;
}
void bindComp(wl_client *c, void *, uint32_t v, uint32_t id) {
  wl_resource *r = wl_resource_create(c, &wl_compositor_interface, v, id);
  wl_resource_set_dispatcher(r, compDispatch, nullptr, nullptr, nullptr);
}

// ---- client side registry ----
struct ClientGlobals {
  liteui_clipboard::detail::WlClipboard *clip = nullptr;
  wl_compositor *comp = nullptr;
};
void regGlobal(void *d, wl_registry *reg, uint32_t name, const char *iface,
               uint32_t ver) {
  auto *g = static_cast<ClientGlobals *>(d);
  if (!std::strcmp(iface, "wl_data_device_manager"))
    g->clip->bindManager(reg, name, ver);
  else if (!std::strcmp(iface, "wl_seat"))
    g->clip->setSeat(static_cast<wl_seat *>(
        wl_registry_bind(reg, name, &wl_seat_interface, 1)));
  else if (!std::strcmp(iface, "wl_compositor"))
    g->comp = static_cast<wl_compositor *>(
        wl_registry_bind(reg, name, &wl_compositor_interface, 1));
}
void regRemove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegListener = {regGlobal, regRemove};

struct DndRig {
  FakeDndComp fc;
  wl_display *cd = nullptr;
  liteui_clipboard::detail::WlClipboard clip;
  ClientGlobals g;
  // what the app side saw
  std::vector<std::string> events;
  std::vector<std::string> gotPaths;
  std::string gotText;
  bool acceptEnter = true;

  DndRig() {
    char tmpl[] = "/tmp/liteui-dnd-XXXXXX";
    REQUIRE(::mkdtemp(tmpl) != nullptr);
    ::setenv("XDG_RUNTIME_DIR", tmpl, 1);
    gFc = &fc;
    fc.sd = wl_display_create();
    fc.loop = wl_display_get_event_loop(fc.sd);
    const char *sock = wl_display_add_socket_auto(fc.sd);
    REQUIRE(sock != nullptr);
    wl_global_create(fc.sd, &wl_data_device_manager_interface, 3, nullptr, bindMgr);
    wl_global_create(fc.sd, &wl_seat_interface, 1, nullptr, bindSeat);
    wl_global_create(fc.sd, &wl_compositor_interface, 1, nullptr, bindComp);
    cd = wl_display_connect(sock);
    REQUIRE(cd != nullptr);
    clip.attach(cd);
    liteui_drop::Hooks h;
    h.enter = [this](float x, float y, bool f, bool t) {
      events.push_back("enter(" + std::to_string(int(x)) + "," + std::to_string(int(y)) +
                       (f ? ",files" : "") + (t ? ",text" : "") + ")");
      return acceptEnter;
    };
    h.over = [this](float x, float y) {
      events.push_back("over(" + std::to_string(int(x)) + "," + std::to_string(int(y)) + ")");
      return true;
    };
    h.leave = [this] { events.push_back("leave"); };
    h.drop = [this](float x, float y, std::vector<std::string> p, std::string t) {
      events.push_back("drop(" + std::to_string(int(x)) + "," + std::to_string(int(y)) + ")");
      gotPaths = std::move(p);
      gotText = std::move(t);
      return true;
    };
    clip.setDropHooks(std::move(h));
    g.clip = &clip;
    wl_registry *reg = wl_display_get_registry(cd);
    wl_registry_add_listener(reg, &kRegListener, &g);
    pump();
    pump();
    REQUIRE(fc.dev != nullptr);
    wl_surface *surf = wl_compositor_create_surface(g.comp);
    (void)surf;
    pump();
    REQUIRE(fc.surface != nullptr);
  }
  ~DndRig() {
    clip.shutdown();
    wl_display_disconnect(cd);
    wl_display_destroy(fc.sd);
    gFc = nullptr;
  }

  // Both ends live in this thread, so alternate them by hand.
  void pump() {
    for (int i = 0; i < 6; ++i) {
      wl_display_flush(cd);
      wl_event_loop_dispatch(fc.loop, 0);
      wl_display_flush_clients(fc.sd);
      wl_display_dispatch_pending(cd);
      if (wl_display_prepare_read(cd) == 0) {
        pollfd p{wl_display_get_fd(cd), POLLIN, 0};
        if (::poll(&p, 1, 0) > 0)
          wl_display_read_events(cd);
        else
          wl_display_cancel_read(cd);
      }
      wl_display_dispatch_pending(cd);
    }
  }
  // One turn of the window's poll loop for the drop reader.
  void loopOnce() {
    std::vector<pollfd> fds;
    const size_t n = clip.appendPollFds(fds);
    if (n)
      ::poll(fds.data(), static_cast<nfds_t>(fds.size()), 50);
    clip.servicePolled(fds.data(), n);
  }

  void sendOffer(const std::vector<const char *> &mimes) {
    fc.offer = wl_resource_create(fc.client, &wl_data_offer_interface, 3, 0);
    wl_resource_set_dispatcher(fc.offer, offerDispatch, nullptr, nullptr, nullptr);
    fc.offerAlive = true;
    wl_resource_post_event(fc.dev, kDevDataOffer, fc.offer);
    for (const char *m : mimes)
      wl_resource_post_event(fc.offer, kOffEvOffer, m);
    wl_resource_post_event(fc.offer, kOffEvSourceActions,
                           (uint32_t)WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  }
  void enter(double x, double y) {
    wl_resource_post_event(fc.dev, kDevEnter, 7u, fc.surface,
                           wl_fixed_from_double(x), wl_fixed_from_double(y),
                           fc.offer);
    wl_display_flush_clients(fc.sd);
  }
  void motion(double x, double y) {
    wl_resource_post_event(fc.dev, kDevMotion, 1u, wl_fixed_from_double(x),
                           wl_fixed_from_double(y));
  }
  void action(uint32_t a) { wl_resource_post_event(fc.offer, kOffEvAction, a); }
  void drop() {
    wl_resource_post_event(fc.dev, kDevDrop);
    wl_display_flush_clients(fc.sd);
  }
  void leave() {
    wl_resource_post_event(fc.dev, kDevLeave);
    wl_display_flush_clients(fc.sd);
  }
};

bool dndHas(const std::vector<std::string> &v, const std::string &s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace liteui_test_drop_wl

using liteui_test_drop_wl::DndRig;
using liteui_test_drop_wl::dndHas;

TEST_CASE("wayland dnd: a file drop is accepted, read from a pipe and delivered",
          "[drop][wayland]") {
  DndRig r;
  r.fc.payload = "# files\r\nfile:///home/u/a%20b.txt\r\nfile:///tmp/c.png\r\n";
  r.sendOffer({"text/uri-list", "text/plain"});
  r.enter(20.5, 30.25);
  r.pump();
  REQUIRE(r.events.size() >= 1);
  CHECK(r.events[0] == "enter(20,30,files)");
  // accepted with the uri-list type, copy only
  CHECK(dndHas(r.fc.reqs, "accept:text/uri-list"));
  CHECK(dndHas(r.fc.reqs, "actions:1/1"));

  r.motion(40, 50);
  r.action(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  wl_display_flush_clients(r.fc.sd);
  r.pump();
  CHECK(dndHas(r.events, "over(40,50)"));

  r.drop();
  r.pump();
  CHECK(dndHas(r.fc.reqs, "receive:text/uri-list"));
  // The data arrives through the poll loop, not by blocking in the handler.
  for (int i = 0; i < 20 && r.gotPaths.empty(); ++i)
    r.loopOnce();
  r.pump();
  REQUIRE(r.gotPaths.size() == 2);
  CHECK(r.gotPaths[0] == "/home/u/a b.txt");
  CHECK(r.gotPaths[1] == "/tmp/c.png");
  CHECK(dndHas(r.events, "drop(40,50)")); // dropped where the last motion was
  // finish comes before the offer is destroyed
  const auto fin = std::find(r.fc.reqs.begin(), r.fc.reqs.end(), "finish");
  const auto des = std::find(r.fc.reqs.begin(), r.fc.reqs.end(), "offer.destroy");
  REQUIRE(fin != r.fc.reqs.end());
  REQUIRE(des != r.fc.reqs.end());
  CHECK(fin < des);
  // a `leave` that follows the drop must not cancel or double-deliver it
  r.leave();
  r.pump();
  CHECK(std::count(r.events.begin(), r.events.end(), std::string("leave")) == 0);
}

TEST_CASE("wayland dnd: dropped text is read as text", "[drop][wayland]") {
  DndRig r;
  r.fc.payload = "hello wayland";
  r.sendOffer({"text/plain;charset=utf-8"});
  r.enter(5, 5);
  r.pump();
  CHECK(r.events[0] == "enter(5,5,text)");
  CHECK(dndHas(r.fc.reqs, "accept:text/plain;charset=utf-8"));
  r.action(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  r.drop();
  r.pump();
  for (int i = 0; i < 20 && r.gotText.empty(); ++i)
    r.loopOnce();
  CHECK(r.gotText == "hello wayland");
  CHECK(r.gotPaths.empty());
}

TEST_CASE("wayland dnd: declined drags are refused and the offer is freed",
          "[drop][wayland]") {
  DndRig r;
  r.acceptEnter = false;
  r.sendOffer({"text/uri-list"});
  r.enter(5, 5);
  r.pump();
  CHECK(dndHas(r.fc.reqs, "accept:null"));
  r.leave();
  r.pump();
  CHECK(dndHas(r.events, "leave"));
  CHECK(dndHas(r.fc.reqs, "offer.destroy"));
  CHECK_FALSE(dndHas(r.fc.reqs, "finish"));
}

TEST_CASE("wayland dnd: unusable offers are refused", "[drop][wayland]") {
  DndRig r;
  r.sendOffer({"image/png"}); // neither files nor text
  r.enter(5, 5);
  r.pump();
  CHECK(r.events[0] == "enter(5,5)");
  CHECK(dndHas(r.fc.reqs, "accept:null"));
}

TEST_CASE("wayland dnd: no finish and no read when no action was negotiated",
          "[drop][wayland]") {
  DndRig r;
  r.sendOffer({"text/uri-list"});
  r.enter(5, 5);
  r.pump();
  // The compositor never sends offer.action: finish would be a protocol error.
  r.drop();
  r.pump();
  CHECK_FALSE(dndHas(r.fc.reqs, "finish"));
  CHECK_FALSE(dndHas(r.fc.reqs, "receive:text/uri-list"));
  CHECK(dndHas(r.fc.reqs, "offer.destroy"));
  CHECK(dndHas(r.events, "leave")); // so the app clears its highlight
  CHECK(r.gotPaths.empty());
}

TEST_CASE("wayland dnd: a source that never answers cannot hang the window",
          "[drop][wayland]") {
  DndRig r;
  r.fc.payload.clear();
  r.sendOffer({"text/uri-list"});
  r.enter(5, 5);
  r.pump();
  r.action(WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  r.drop();
  r.pump();
  // Our fake closes the pipe at once (EOF, empty list): nothing is delivered,
  // the highlight is cleared and the offer is released.
  for (int i = 0; i < 5; ++i)
    r.loopOnce();
  r.pump();
  CHECK(r.gotPaths.empty());
  CHECK(dndHas(r.events, "leave"));
  CHECK(dndHas(r.fc.reqs, "offer.destroy"));
  CHECK(r.clip.pollTimeoutMs() == -1); // no reader left behind
}

#endif // !_WIN32 && LITEUI_TEST_WAYLAND_SERVER
