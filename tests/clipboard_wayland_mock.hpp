#pragma once
// A minimal fake Wayland compositor and test clients for the native clipboard
// (liteui_clipboard::detail::WlClipboard), used by cases/clipboard_wayland.inc.
// Included after liteui.hpp and clipboard_helpers.hpp. Linux only, and only
// when libwayland-server is available (tests/CMakeLists.txt defines
// LITEUI_TEST_WAYLAND_SERVER).
//
// The compositor implements just what the clipboard uses, following the
// protocol: wl_data_device_manager, wl_data_source, wl_data_device,
// wl_data_offer (receive() is relayed to the source's send() over a real pipe)
// and a wl_seat to hang the data device on. It runs in a forked child process,
// logs what it receives to a file, and on SIGUSR1 re-sends the current
// selection to every device (what a real compositor does when a client gains
// keyboard focus). It does NOT model focus or serial validation: clients
// decide that themselves, and those rules are tested through WlClipboard's own
// canRead() / canWrite().

#if !defined(_WIN32) && defined(LITEUI_TEST_WAYLAND_SERVER)

#include <wayland-client.h>
#include <wayland-server.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>

#include <atomic>
#include <csignal>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>

namespace clipboard_test {

using liteui_clipboard::detail::WlClipboard;

// ============================================================================
// The compositor (runs in the forked child)
// ============================================================================
namespace server {

struct SrcData {
  std::vector<std::string> mimes;
};

inline std::set<wl_resource *> live; // sources that still exist
inline wl_resource *current = nullptr;
inline std::vector<wl_resource *> devices;
inline FILE *logf = nullptr;

inline void sendSelection(wl_resource *dev) {
  if (!current) {
    wl_data_device_send_selection(dev, nullptr);
    return;
  }
  static const struct wl_data_offer_interface offerImpl = {
      /*accept*/ [](wl_client *, wl_resource *, uint32_t, const char *) {},
      /*receive*/
      [](wl_client *, wl_resource *r, const char *mime, int32_t fd) {
        auto *src = static_cast<wl_resource *>(wl_resource_get_user_data(r));
        if (live.count(src))
          wl_data_source_send_send(src, mime, fd);
        close(fd);
      },
      /*destroy*/ [](wl_client *, wl_resource *r) { wl_resource_destroy(r); },
      /*finish*/ [](wl_client *, wl_resource *) {},
      /*set_actions*/ [](wl_client *, wl_resource *, uint32_t, uint32_t) {}};
  auto *sd = static_cast<SrcData *>(wl_resource_get_user_data(current));
  wl_client *c = wl_resource_get_client(dev);
  wl_resource *offer =
      wl_resource_create(c, &wl_data_offer_interface, wl_resource_get_version(dev), 0);
  wl_resource_set_implementation(offer, &offerImpl, current, nullptr);
  wl_data_device_send_data_offer(dev, offer);
  for (const std::string &m : sd->mimes)
    wl_data_offer_send_offer(offer, m.c_str());
  wl_data_device_send_selection(dev, offer);
}

inline void broadcast() {
  for (wl_resource *d : devices)
    sendSelection(d);
}

inline void sourceDestroyed(wl_resource *r) {
  delete static_cast<SrcData *>(wl_resource_get_user_data(r));
  live.erase(r);
  if (current == r) { // the owner went away: the selection is cleared
    current = nullptr;
    broadcast();
  }
}

inline const struct wl_data_source_interface sourceImpl = {
    /*offer*/
    [](wl_client *, wl_resource *r, const char *m) {
      static_cast<SrcData *>(wl_resource_get_user_data(r))->mimes.push_back(m);
      fprintf(logf, "offer %s\n", m);
      fflush(logf);
    },
    /*destroy*/ [](wl_client *, wl_resource *r) { wl_resource_destroy(r); },
    /*set_actions*/ [](wl_client *, wl_resource *, uint32_t) {}};

inline void deviceDestroyed(wl_resource *r) {
  devices.erase(std::remove(devices.begin(), devices.end(), r), devices.end());
}

inline const struct wl_data_device_interface deviceImpl = {
    /*start_drag*/
    [](wl_client *, wl_resource *, wl_resource *, wl_resource *, wl_resource *, uint32_t) {},
    /*set_selection*/
    [](wl_client *, wl_resource *, wl_resource *src, uint32_t serial) {
      fprintf(logf, "set_selection serial=%u src=%d\n", serial, src ? 1 : 0);
      fflush(logf);
      if (current && current != src) // the previous owner loses the clipboard
        wl_data_source_send_cancelled(current);
      current = src;
      broadcast();
    },
    /*release*/ [](wl_client *, wl_resource *r) { wl_resource_destroy(r); }};

inline const struct wl_data_device_manager_interface managerImpl = {
    /*create_data_source*/
    [](wl_client *c, wl_resource *m, uint32_t id) {
      wl_resource *r = wl_resource_create(c, &wl_data_source_interface,
                                          wl_resource_get_version(m), id);
      wl_resource_set_implementation(r, &sourceImpl, new SrcData, sourceDestroyed);
      live.insert(r);
    },
    /*get_data_device*/
    [](wl_client *c, wl_resource *m, uint32_t id, wl_resource *) {
      wl_resource *r = wl_resource_create(c, &wl_data_device_interface,
                                          wl_resource_get_version(m), id);
      wl_resource_set_implementation(r, &deviceImpl, nullptr, deviceDestroyed);
      devices.push_back(r);
    }};

inline const struct wl_seat_interface seatImpl = {nullptr, nullptr, nullptr, nullptr};

// Returns when the compositor is terminated. `readyFd` gets one byte once the
// socket exists.
inline int run(const char *socketName, const char *logPath, int readyFd) {
  // This process is a fork of the Catch2 test binary, so it inherited Catch2's
  // fatal-signal handlers. Without this, the SIGTERM that ends the compositor
  // at the end of each test would run Catch2's handler in here and print a
  // bogus "fatal error" report into the test's output.
  for (int sig : {SIGTERM, SIGINT, SIGABRT, SIGSEGV, SIGFPE, SIGILL, SIGBUS})
    std::signal(sig, SIG_DFL);
  // If the test process dies (a crash, a timeout kill), take this compositor
  // down with it. Otherwise it lingers, holding the test's output pipe open,
  // and whatever is waiting on that pipe (ctest) hangs until its own timeout.
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() == 1) // the parent was already gone before the call above
    return 1;
  logf = fopen(logPath, "w");
  wl_display *d = wl_display_create();
  if (!logf || !d || wl_display_add_socket(d, socketName))
    return 1;
  wl_global_create(d, &wl_data_device_manager_interface, 3, nullptr,
                   [](wl_client *c, void *, uint32_t v, uint32_t id) {
                     wl_resource *r = wl_resource_create(
                         c, &wl_data_device_manager_interface, static_cast<int>(v), id);
                     wl_resource_set_implementation(r, &managerImpl, nullptr, nullptr);
                   });
  wl_global_create(d, &wl_seat_interface, 7, nullptr,
                   [](wl_client *c, void *, uint32_t v, uint32_t id) {
                     wl_resource *r =
                         wl_resource_create(c, &wl_seat_interface, static_cast<int>(v), id);
                     wl_resource_set_implementation(r, &seatImpl, nullptr, nullptr);
                     wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_KEYBOARD);
                   });
  wl_event_loop_add_signal(
      wl_display_get_event_loop(d), SIGUSR1,
      [](int, void *) {
        broadcast(); // "keyboard focus gained": resend the selection
        return 0;
      },
      nullptr);
  const char ok = 1;
  (void)!write(readyFd, &ok, 1);
  wl_display_run(d);
  return 0;
}

} // namespace server

// The compositor as seen from the test: a forked process plus its log.
class Compositor {
public:
  Compositor() : xdg_("XDG_RUNTIME_DIR", dir_.path()) {
    static int counter = 0;
    name_ = "lt-wl-" + std::to_string(getpid()) + "-" + std::to_string(counter++);
    logPath_ = dir_.file("server.log");
    int pf[2];
    REQUIRE(pipe(pf) == 0);
    pid_ = fork();
    if (pid_ == 0) {
      close(pf[0]);
      _exit(server::run(name_.c_str(), logPath_.c_str(), pf[1]));
    }
    close(pf[1]);
    char ok = 0;
    const ssize_t n = read(pf[0], &ok, 1);
    close(pf[0]);
    REQUIRE(n == 1); // the compositor came up and created its socket
  }
  ~Compositor() {
    kill(pid_, SIGTERM);
    waitpid(pid_, nullptr, 0);
  }
  Compositor(const Compositor &) = delete;
  Compositor &operator=(const Compositor &) = delete;

  const std::string &name() const { return name_; }
  // "A keyboard-focus gain": every device is sent the current selection again.
  void broadcast() const { kill(pid_, SIGUSR1); }
  std::string log() const { return readFile(logPath_); }
  bool logHas(const std::string &s) const { return log().find(s) != std::string::npos; }
  // How many set_selection requests with a real source have been received.
  int selectionSets() const {
    std::istringstream in(log());
    std::string line;
    int n = 0;
    while (std::getline(in, line))
      if (line.find("set_selection") != std::string::npos &&
          line.find("src=1") != std::string::npos)
        ++n;
    return n;
  }

private:
  TempDir dir_;
  EnvGuard xdg_; // libwayland puts the socket in $XDG_RUNTIME_DIR
  std::string name_, logPath_;
  pid_t pid_ = -1;
};

// ============================================================================
// Clients
// ============================================================================

// One connection with a real WlClipboard on it, driven the way the window's
// event loop drives it: poll the display fd plus the clipboard's pending-write
// fds, read events, dispatch, service writes.
class Client {
public:
  explicit Client(const std::string &socket) : clip(std::make_unique<WlClipboard>()) {
    display = wl_display_connect(socket.c_str());
    if (!display)
      throw std::runtime_error("cannot connect to the fake compositor");
    clip->attach(display);
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, this);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display); // the data device and its first events
  }
  ~Client() {
    clip->shutdown(); // objects must go before the connection does
    if (registry)
      wl_registry_destroy(registry);
    if (seat)
      wl_seat_destroy(seat);
    if (display)
      wl_display_disconnect(display);
  }
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  void roundtrip() { wl_display_roundtrip(display); }

  void pumpOnce(int timeoutMs, int extraFd = -1) {
    while (wl_display_prepare_read(display) != 0)
      wl_display_dispatch_pending(display);
    wl_display_flush(display);
    std::vector<pollfd> fds;
    fds.push_back({wl_display_get_fd(display), POLLIN, 0});
    const size_t clipStart = fds.size();
    const size_t clipCount = clip->appendPollFds(fds);
    const size_t extra = fds.size();
    if (extraFd >= 0)
      fds.push_back({extraFd, POLLIN, 0});
    poll(fds.data(), fds.size(), timeoutMs);
    if (fds[0].revents & POLLIN)
      wl_display_read_events(display);
    else
      wl_display_cancel_read(display);
    wl_display_dispatch_pending(display);
    clip->servicePolled(fds.data() + clipStart, clipCount);
    if (extraFd >= 0 && (fds[extra].revents & POLLIN)) {
      uint64_t v;
      (void)!read(extraFd, &v, sizeof(v));
    }
  }

  template <class Pred> bool pumpUntil(Pred done, int timeoutMs = 3000) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!done()) {
      if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(timeoutMs))
        return false;
      pumpOnce(20);
    }
    return true;
  }

  wl_display *display = nullptr;
  std::unique_ptr<WlClipboard> clip;

private:
  static void onGlobal(void *data, wl_registry *r, uint32_t name, const char *iface,
                       uint32_t version) {
    auto *self = static_cast<Client *>(data);
    if (!std::strcmp(iface, "wl_seat")) {
      self->seat = static_cast<wl_seat *>(
          wl_registry_bind(r, name, &wl_seat_interface, std::min<uint32_t>(version, 7)));
      self->clip->setSeat(self->seat);
    } else if (!std::strcmp(iface, "wl_data_device_manager")) {
      self->clip->bindManager(r, name, version);
    }
  }
  static constexpr wl_registry_listener registryListener = {
      onGlobal, [](void *, wl_registry *, uint32_t) {}};

  wl_registry *registry = nullptr;
  wl_seat *seat = nullptr;
};

// A Client whose WlClipboard lives on its own thread, like the UI thread of a
// separate application. WlClipboard is single-threaded by contract, so every
// call into it is shipped to that thread with on(); the calling thread (the
// test) waits on the returned future and does all its assertions itself,
// because Catch2 assertions are not thread-safe.
class ThreadClient {
public:
  explicit ThreadClient(const std::string &socket) {
    evfd_ = eventfd(0, 0);
    auto ready = ready_.get_future();
    thread_ = std::thread([this, socket] {
      try {
        client_ = std::make_unique<Client>(socket);
        ready_.set_value();
      } catch (...) {
        ready_.set_exception(std::current_exception());
        return;
      }
      while (!quit_) {
        client_->pumpOnce(50, evfd_);
        runTasks();
      }
      client_.reset();
    });
    ready.get(); // rethrows a connection failure into the test
  }
  ~ThreadClient() {
    quit_ = true;
    wake();
    if (thread_.joinable())
      thread_.join();
    close(evfd_);
  }
  ThreadClient(const ThreadClient &) = delete;
  ThreadClient &operator=(const ThreadClient &) = delete;

  template <class F> auto on(F f) {
    using R = std::invoke_result_t<F, WlClipboard &>;
    auto task = std::make_shared<std::packaged_task<R()>>(
        [this, f = std::move(f)]() mutable { return f(*client_->clip); });
    auto fut = task->get_future();
    enqueue([task] { (*task)(); });
    return fut;
  }
  // The client has processed everything the compositor sent it so far.
  void sync() {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    enqueue([this, done] {
      wl_display_roundtrip(client_->display);
      done->set_value();
    });
    fut.get();
  }
  // Like a window gaining keyboard focus with an input event.
  void focus(uint32_t serial) {
    on([serial](WlClipboard &c) {
      c.setFocused(true);
      c.noteSerial(serial);
      return 0;
    }).get();
  }

private:
  void enqueue(std::function<void()> f) {
    {
      std::lock_guard<std::mutex> l(m_);
      q_.push_back(std::move(f));
    }
    wake();
  }
  void wake() {
    const uint64_t one = 1;
    (void)!write(evfd_, &one, sizeof(one));
  }
  void runTasks() {
    std::deque<std::function<void()>> work;
    {
      std::lock_guard<std::mutex> l(m_);
      work.swap(q_);
    }
    for (auto &t : work)
      t();
  }

  int evfd_ = -1;
  std::mutex m_;
  std::deque<std::function<void()>> q_;
  std::atomic<bool> quit_{false};
  std::promise<void> ready_;
  std::unique_ptr<Client> client_;
  std::thread thread_;
};

// ---- bare-protocol peers, for behaviour WlClipboard itself never shows --------

// Owns a selection offering the given MIME types and serves it from a thread
// of its own. mode: 0 = write `payload` for any request, 1 = accept the
// request but never write (a stalled source), 3 = write the MIME name that was
// asked for (to see which type a reader chose).
class RawPeer {
public:
  RawPeer(const std::string &socket, std::vector<const char *> mimes, int mode,
          Bytes payload = {})
      : mode_(mode), payload_(std::move(payload)) {
    display_ = wl_display_connect(socket.c_str());
    REQUIRE(display_);
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);
    wl_display_roundtrip(display_);
    device_ = wl_data_device_manager_get_data_device(manager_, seat_);
    wl_data_device_add_listener(device_, &deviceListener, this);
    source_ = wl_data_device_manager_create_data_source(manager_);
    wl_data_source_add_listener(source_, &sourceListener, this);
    for (const char *m : mimes)
      wl_data_source_offer(source_, m);
    wl_data_device_set_selection(device_, source_, 1);
    wl_display_roundtrip(display_);
    thread_ = std::thread([this] {
      while (!quit_) {
        while (wl_display_prepare_read(display_) != 0)
          wl_display_dispatch_pending(display_);
        wl_display_flush(display_);
        pollfd p{wl_display_get_fd(display_), POLLIN, 0};
        if (poll(&p, 1, 50) > 0)
          wl_display_read_events(display_);
        else
          wl_display_cancel_read(display_);
        wl_display_dispatch_pending(display_);
      }
    });
  }
  ~RawPeer() {
    quit_ = true;
    if (thread_.joinable())
      thread_.join();
    for (int fd : held_)
      close(fd);
    for (wl_data_offer *o : offers_)
      wl_data_offer_destroy(o);
    wl_data_source_destroy(source_);
    wl_data_device_release(device_);
    wl_data_device_manager_destroy(manager_);
    wl_seat_destroy(seat_);
    wl_registry_destroy(registry_);
    wl_display_disconnect(display_);
  }
  RawPeer(const RawPeer &) = delete;
  RawPeer &operator=(const RawPeer &) = delete;

private:
  static void onGlobal(void *d, wl_registry *r, uint32_t n, const char *i, uint32_t v) {
    auto *s = static_cast<RawPeer *>(d);
    if (!std::strcmp(i, "wl_seat"))
      s->seat_ = static_cast<wl_seat *>(wl_registry_bind(r, n, &wl_seat_interface, 1));
    else if (!std::strcmp(i, "wl_data_device_manager"))
      s->manager_ = static_cast<wl_data_device_manager *>(wl_registry_bind(
          r, n, &wl_data_device_manager_interface, std::min<uint32_t>(v, 3)));
  }
  static void onSend(void *d, wl_data_source *, const char *mime, int32_t fd) {
    auto *s = static_cast<RawPeer *>(d);
    if (s->mode_ == 1) {
      s->held_.push_back(fd); // keep the pipe open, write nothing
      return;
    }
    if (s->mode_ == 3)
      (void)!write(fd, mime, std::strlen(mime));
    else if (!s->payload_.empty())
      (void)!write(fd, s->payload_.data(), s->payload_.size());
    close(fd);
  }
  static constexpr wl_registry_listener registryListener = {
      onGlobal, [](void *, wl_registry *, uint32_t) {}};
  // The compositor offers us the selection too (we are a client with a data
  // device); every offer is a protocol object that has to be destroyed.
  static void onDataOffer(void *d, wl_data_device *, wl_data_offer *o) {
    static_cast<RawPeer *>(d)->offers_.push_back(o);
  }
  static constexpr wl_data_device_listener deviceListener = {
      onDataOffer,
      [](void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t,
         wl_data_offer *) {},
      [](void *, wl_data_device *) {},
      [](void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {},
      [](void *, wl_data_device *) {},
      [](void *, wl_data_device *, wl_data_offer *) {}};
  static constexpr wl_data_source_listener sourceListener = {
      [](void *, wl_data_source *, const char *) {},
      onSend,
      [](void *, wl_data_source *) {},
      [](void *, wl_data_source *) {},
      [](void *, wl_data_source *) {},
      [](void *, wl_data_source *, uint32_t) {}};

  int mode_;
  Bytes payload_;
  wl_display *display_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_data_device_manager *manager_ = nullptr;
  wl_seat *seat_ = nullptr;
  wl_data_device *device_ = nullptr;
  wl_data_source *source_ = nullptr;
  std::vector<int> held_;
  std::vector<wl_data_offer *> offers_;
  std::thread thread_;
  std::atomic<bool> quit_{false};
};

// A reader that does the protocol by hand, so it can misbehave: take part of a
// transfer and slam the pipe shut, or keep it open and never read the rest.
class RawReader {
public:
  explicit RawReader(const Compositor &comp) {
    display_ = wl_display_connect(comp.name().c_str());
    REQUIRE(display_);
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);
    wl_display_roundtrip(display_);
    device_ = wl_data_device_manager_get_data_device(manager_, seat_);
    wl_data_device_add_listener(device_, &deviceListener, this);
    wl_display_roundtrip(display_);
    comp.broadcast(); // "gained focus": deliver the current selection
    for (int i = 0; i < 100 && !gotSelection; ++i) {
      wl_display_roundtrip(display_);
      usleep(10000);
    }
  }
  ~RawReader() {
    for (wl_data_offer *o : offers_)
      wl_data_offer_destroy(o);
    wl_data_device_release(device_);
    wl_data_device_manager_destroy(manager_);
    wl_seat_destroy(seat_);
    wl_registry_destroy(registry_);
    wl_display_disconnect(display_);
  }
  RawReader(const RawReader &) = delete;
  RawReader &operator=(const RawReader &) = delete;

  // Asks for image/png, reads `n` bytes, then closes the pipe (abort = true)
  // or returns its read end still open for the caller to close.
  int receiveAndAbort(size_t n, bool abort) {
    int f[2];
    REQUIRE(pipe(f) == 0);
    wl_data_offer_receive(selection_, "image/png", f[1]);
    close(f[1]);
    wl_display_flush(display_);
    Bytes b(n);
    size_t got = 0;
    while (got < n) {
      const ssize_t k = read(f[0], b.data() + got, n - got);
      if (k <= 0)
        break;
      got += static_cast<size_t>(k);
    }
    if (abort) {
      close(f[0]);
      return -1;
    }
    return f[0];
  }

  bool gotSelection = false;

private:
  static void onGlobal(void *d, wl_registry *r, uint32_t n, const char *i, uint32_t v) {
    auto *s = static_cast<RawReader *>(d);
    if (!std::strcmp(i, "wl_seat"))
      s->seat_ = static_cast<wl_seat *>(wl_registry_bind(r, n, &wl_seat_interface, 1));
    else if (!std::strcmp(i, "wl_data_device_manager"))
      s->manager_ = static_cast<wl_data_device_manager *>(wl_registry_bind(
          r, n, &wl_data_device_manager_interface, std::min<uint32_t>(v, 3)));
  }
  static constexpr wl_registry_listener registryListener = {
      onGlobal, [](void *, wl_registry *, uint32_t) {}};
  // A static function, not a lambda in the listener: its body may use members
  // declared further down the class, an initializer here may not.
  static void onSelection(void *d, wl_data_device *, wl_data_offer *o) {
    auto *s = static_cast<RawReader *>(d);
    s->selection_ = o;
    s->gotSelection = o != nullptr;
  }
  static void onDataOffer(void *d, wl_data_device *, wl_data_offer *o) {
    static_cast<RawReader *>(d)->offers_.push_back(o);
  }
  static constexpr wl_data_device_listener deviceListener = {
      onDataOffer,
      [](void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t,
         wl_data_offer *) {},
      [](void *, wl_data_device *) {},
      [](void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {},
      [](void *, wl_data_device *) {},
      onSelection};

  wl_display *display_ = nullptr;
  wl_registry *registry_ = nullptr;
  wl_data_device_manager *manager_ = nullptr;
  wl_seat *seat_ = nullptr;
  wl_data_device *device_ = nullptr;
  wl_data_offer *selection_ = nullptr;
  std::vector<wl_data_offer *> offers_;
};

} // namespace clipboard_test

#endif // !_WIN32 && LITEUI_TEST_WAYLAND_SERVER
