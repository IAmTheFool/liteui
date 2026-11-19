#pragma once
// Shared helpers for the clipboard cases (cases/clipboard_*.inc): test images,
// temp files and environment variables, DIB builders, and the machinery for
// running a scenario in a child process.
//
// Included once, from liteui_tests.cpp, AFTER liteui.hpp (so the
// liteui_clipboard namespace and its detail:: internals already exist) and
// after test_helpers.hpp. Everything lives in namespace clipboard_test so none
// of these short names (same, add, eq, ...) can collide with another case file
// in the single translation unit.
//
// Child processes: liteui_clipboard::detail::tools() is detected once per
// process from PATH / WAYLAND_DISPLAY / DISPLAY and cached, so each tool setup
// has to run in a fresh process. A scenario is therefore a hidden "[.]" test
// case (named "clipboard child: ...") that an ordinary test runs by
// re-executing this very binary with the environment the scenario needs
// (see runChild).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_version_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Catch2 >= 3.3 has SKIP(); older 3.x releases (which the root CMakeLists
// still accepts via find_package(Catch2 3)) do not.
#if CATCH_VERSION_MAJOR > 3 || (CATCH_VERSION_MAJOR == 3 && CATCH_VERSION_MINOR >= 3)
#define CLIPBOARD_SKIP(msg) SKIP(msg)
#else
#define CLIPBOARD_SKIP(msg)                                                           \
  do {                                                                         \
    WARN("skipped: " msg);                                                     \
    return;                                                                    \
  } while (0)
#endif

namespace clipboard_test {

namespace cd = liteui_clipboard::detail;
using cd::ClipStatus;
using Bytes = std::vector<uint8_t>;

// ---- images ----------------------------------------------------------------

// A patterned RGBA8 image. Alpha varies and is never 0, so a premultiplied or
// "all-zero alpha means opaque" mix-up shows up as a pixel difference.
inline CanvasImage makeImage(int w, int h, uint8_t seed = 1, bool opaque = false) {
  CanvasImage im(w, h);
  for (size_t i = 0; i < im.pixels.size(); i += 4) {
    const size_t p = i / 4;
    im.pixels[i + 0] = static_cast<uint8_t>(p * 7 + seed);
    im.pixels[i + 1] = static_cast<uint8_t>(p * 13 + seed * 3);
    im.pixels[i + 2] = static_cast<uint8_t>(p * 29 + seed * 5);
    im.pixels[i + 3] = opaque ? 255 : static_cast<uint8_t>(64 + (p * 11 + seed) % 192);
  }
  return im;
}

// Incompressible pixels: the encoded PNG is about as big as the raw data.
inline CanvasImage makeNoiseImage(int w, int h, uint32_t seed = 1) {
  CanvasImage im(w, h);
  uint32_t x = seed * 2654435761u + 1;
  for (uint8_t &b : im.pixels) {
    x = x * 1664525u + 1013904223u;
    b = static_cast<uint8_t>(x >> 24);
  }
  return im;
}

inline bool same(const CanvasImage &a, const CanvasImage &b) {
  return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}
inline bool same(const std::optional<CanvasImage> &a, const CanvasImage &b) {
  return a && same(*a, b);
}

// Big buffers: compare through a bool so a failure does not make Catch2 try to
// print tens of megabytes.
inline bool eq(const Bytes &a, const Bytes &b) { return a == b; }
inline bool eq(const std::string &a, const std::string &b) { return a == b; }

// The PNG signature followed by pseudo-random bytes. Enough for the clipboard
// transport, which only checks the signature; the real codec is exercised
// separately.
inline Bytes fakePng(size_t size, uint32_t seed) {
  Bytes v(std::max<size_t>(size, 8));
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::memcpy(v.data(), sig, 8);
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 8; i < v.size(); ++i) {
    x = x * 1664525u + 1013904223u;
    v[i] = static_cast<uint8_t>(x >> 24);
  }
  return v;
}

inline Bytes realPng(const CanvasImage &img) {
  Bytes png;
  REQUIRE(cd::encodeClipboardPng(img, png));
  return png;
}

// ---- files, environment ----------------------------------------------------

class TempDir {
public:
  TempDir() {
#if defined(_WIN32)
    static int counter = 0;
    path_ = (std::filesystem::temp_directory_path() /
             ("liteui-test-" + std::to_string(GetCurrentProcessId()) + "-" +
              std::to_string(counter++)))
                .string();
    std::filesystem::create_directories(path_);
#else
    // mkdtemp gives 0700, which libwayland wants for XDG_RUNTIME_DIR.
    std::string t = (std::filesystem::temp_directory_path() / "liteui-test-XXXXXX").string();
    std::vector<char> b(t.begin(), t.end());
    b.push_back('\0');
    if (!mkdtemp(b.data()))
      throw std::runtime_error("mkdtemp failed");
    path_ = b.data();
#endif
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;
  const std::string &path() const { return path_; }
  std::string file(const std::string &name) const { return path_ + "/" + name; }

private:
  std::string path_;
};

inline std::string readFile(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
inline void writeFile(const std::string &path, const std::string &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}
inline void writeFile(const std::string &path, const Bytes &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char *>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

inline std::optional<std::string> getEnv(const std::string &k) {
  const char *v = std::getenv(k.c_str());
  return v ? std::optional<std::string>(v) : std::nullopt;
}
inline void setEnv(const std::string &k, const std::optional<std::string> &v) {
#if defined(_WIN32)
  _putenv_s(k.c_str(), v ? v->c_str() : ""); // an empty value removes it
#else
  if (v)
    ::setenv(k.c_str(), v->c_str(), 1);
  else
    ::unsetenv(k.c_str());
#endif
}
// Sets an environment variable for one scope and puts the old value back.
class EnvGuard {
public:
  EnvGuard(std::string k, const std::optional<std::string> &v)
      : key_(std::move(k)), old_(getEnv(key_)) {
    setEnv(key_, v);
  }
  ~EnvGuard() { setEnv(key_, old_); }
  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

private:
  std::string key_;
  std::optional<std::string> old_;
};

#if !defined(_WIN32)
inline int countFds() {
  int n = 0;
  for (const auto &e : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)e;
    ++n;
  }
  return n;
}
#endif

// ---- running a scenario in a child process ------------------------------------
//
// liteui_clipboard::detail::tools() is detected once per process from the
// environment (PATH, WAYLAND_DISPLAY, DISPLAY) and cached. Each tool setup is
// therefore a "child" test case: hidden with [.], and run by re-executing this
// very binary with the environment the scenario needs. The parent test case
// checks the child's exit code and shows its output on failure.

using EnvMap = std::map<std::string, std::optional<std::string>>;

inline bool isChild() { return std::getenv("LITEUI_TEST_CHILD") != nullptr; }

struct ChildResult {
  int status = -1;
  std::string output;
};

inline std::string selfExe() {
#if defined(_WIN32)
  char buf[MAX_PATH];
  const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return std::string(buf, n);
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
#endif
}

inline ChildResult runChild(const std::string &testName, const EnvMap &env,
                            int timeoutSec = 180) {
  TempDir td;
  const std::string outPath = td.file("child.out");
  const std::string exe = selfExe();
  ChildResult r;
#if defined(_WIN32)
  std::vector<std::unique_ptr<EnvGuard>> guards;
  guards.push_back(std::make_unique<EnvGuard>("LITEUI_TEST_CHILD", "1"));
  for (const auto &[k, v] : env)
    guards.push_back(std::make_unique<EnvGuard>(k, v));
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out = CreateFileA(outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = out;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::string cmd = "\"" + exe + "\" \"" + testName + "\"";
  if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                     nullptr, &si, &pi)) {
    if (WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeoutSec) * 1000) ==
        WAIT_TIMEOUT)
      TerminateProcess(pi.hProcess, 124);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.status = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  CloseHandle(out);
#else
  const pid_t pid = fork();
  if (pid == 0) {
    for (const auto &[k, v] : env)
      setEnv(k, v);
    setEnv("LITEUI_TEST_CHILD", "1");
    const int fd = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(fd, 1);
    dup2(fd, 2);
    alarm(static_cast<unsigned>(timeoutSec));
    execl(exe.c_str(), exe.c_str(), testName.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
#endif
  r.output = readFile(outPath);
  return r;
}

// In the parent: run a child scenario and require it to pass.
#define CLIPBOARD_RUN_CHILD(name, env)                                                \
  do {                                                                         \
    const auto lt_r = ::clipboard_test::runChild(name, env);                               \
    INFO("child '" << name << "' output:\n" << lt_r.output);                   \
    REQUIRE(lt_r.status == 0);                                                 \
  } while (0)

// In a child scenario: do nothing if someone runs the hidden test directly.
#define CLIPBOARD_CHILD_ONLY()                                                        \
  if (!::clipboard_test::isChild()) {                                                      \
    SUCCEED("only meaningful when spawned by its parent test");                \
    return;                                                                    \
  }


// ---- hand-built DIBs (clipboard_dib.inc and clipboard_windows.inc) -----------

// A BITMAPINFOHEADER-style header of `size` bytes (40, 52, 56, 108 or 124).
inline Bytes dibHeader(uint32_t size, int32_t w, int32_t h, uint16_t bpp, uint32_t comp,
                uint32_t clrUsed = 0) {
  Bytes v(size, 0);
  cd::dibPut32(v.data() + 0, size);
  cd::dibPut32(v.data() + 4, static_cast<uint32_t>(w));
  cd::dibPut32(v.data() + 8, static_cast<uint32_t>(h));
  cd::dibPut16(v.data() + 12, 1);
  cd::dibPut16(v.data() + 14, bpp);
  cd::dibPut32(v.data() + 16, comp);
  cd::dibPut32(v.data() + 32, clrUsed);
  return v;
}
inline void add(Bytes &v, std::initializer_list<uint8_t> b) { v.insert(v.end(), b); }
inline void add32(Bytes &v, uint32_t x) {
  uint8_t t[4];
  cd::dibPut32(t, x);
  v.insert(v.end(), t, t + 4);
}
// Standard 8-bit BGRA masks plus an alpha mask, inside a V4/V5 header.
inline void setMasks(Bytes &h, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  cd::dibPut32(h.data() + 40, r);
  cd::dibPut32(h.data() + 44, g);
  cd::dibPut32(h.data() + 48, b);
  cd::dibPut32(h.data() + 52, a);
}

inline Bytes bytesOf(const std::string &s) { return Bytes(s.begin(), s.end()); }

} // namespace clipboard_test
