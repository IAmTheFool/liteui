// Tests for the native Wayland clipboard (liteui_clipboard::detail::WlClipboard,
// the compositor's own wl_data_device) against the fake compositor in
// clipboard_wayland_mock.hpp: images and text between two clients, MIME choice,
// focus and input-serial gating, cancellation, stalled and misbehaving peers,
// persist-on-exit, and the public API routed through the native backend.
//
// Conventions: "B" is the main-thread client (the test pumps its events
// itself); "A" is a ThreadClient, i.e. another application. Where A blocks
// waiting for data from B, the test keeps pumping B, as a real event loop
// would.
// Empty on Windows or without libwayland-server. (Included from
// liteui_tests.cpp — not a standalone .cpp.)

#if !defined(_WIN32) && defined(LITEUI_TEST_WAYLAND_SERVER)

#include "clipboard_tools_mock.hpp"
#include "clipboard_wayland_mock.hpp"

namespace clipboard_test {

inline constexpr auto kReady = std::future_status::ready;

// One compositor plus the main-thread client B, with B already focused and
// holding an input serial.
struct Bench {
  Compositor comp;
  Client B;
  int seen = 0;

  Bench() : B(comp.name()) {
    B.clip->setFocused(true);
    B.clip->noteSerial(1);
  }

  // Waits until a set_selection newer than the last settle() has reached the
  // compositor, then until B and every given thread client have received the
  // resulting selection events. Deterministic: a client's sync round trip is
  // ordered after the events the compositor sent it before.
  void settle(std::initializer_list<ThreadClient *> others = {}) {
    for (int i = 0; i < 500 && comp.selectionSets() <= seen; ++i)
      usleep(10000);
    REQUIRE(comp.selectionSets() > seen);
    seen = comp.selectionSets();
    B.roundtrip();
    B.roundtrip();
    for (ThreadClient *t : others)
      t->sync();
  }
};

// Makes `c` the clipboard backend that the public liteui_clipboard functions use.
struct UseBackend {
  explicit UseBackend(WlClipboard &c) { cd::nativeClipboard() = &c; }
  ~UseBackend() { cd::nativeClipboard() = nullptr; }
};

inline const std::string kUnicodeText =
    std::string("h\xC3\xA9llo w\xC3\xB6rld \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
                "\nline2\r\nline3\t\"quoted\"\n") +
    std::string(1, '\0') + "after-nul";

// Used by the persist-on-exit scenario.
inline const char *const kRecordingCopy = R"(#!/bin/sh
cat > "$LITEUI_TEST_OUT/persist.bin"
echo "$*" > "$LITEUI_TEST_OUT/persist.args"
)";


// ============================================================================
// Images
// ============================================================================

TEST_CASE("clipboard wayland image: one client copies - another reads", "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(11);
  const Bytes png = fakePng(40 * 1024, 1);

  REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(png); }).get() == ClipStatus::Ok);
  b.settle({&A});

  CHECK(b.B.clip->hasPng() == ClipStatus::Ok); // from the cached offer: no process, no I/O
  Bytes got;
  REQUIRE(b.B.clip->readPng(got) == ClipStatus::Ok);
  CHECK(eq(got, png));

  // The owner reads its own selection directly. Asking the compositor would
  // make it wait on itself: it is blocked in that very call.
  CHECK(A.on([&](WlClipboard &c) {
           Bytes o;
           return c.readPng(o) == ClipStatus::Ok && eq(o, png);
         }).get());

  CHECK(b.comp.logHas("set_selection serial=11 src=1")); // the serial we noted is the one sent
  CHECK(b.comp.logHas("offer image/png"));
}

TEST_CASE("clipboard wayland image: a large image survives partial pipe writes", "[clipboard][wayland]") {
  // Far bigger than a pipe buffer, so the source must finish the transfer from
  // the poll loop (appendPollFds / servicePolled) rather than in one write.
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(12);
  const Bytes png = fakePng(24u << 20, 7);

  REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(png); }).get() == ClipStatus::Ok);
  b.settle({&A});
  Bytes got;
  REQUIRE(b.B.clip->readPng(got) == ClipStatus::Ok);
  CHECK(eq(got, png));
}

TEST_CASE("clipboard wayland image: a new owner cancels the previous one", "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(11);
  const Bytes first = fakePng(4096, 1), second = fakePng(8192, 2);

  REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(first); }).get() == ClipStatus::Ok);
  b.settle({&A});
  b.B.clip->noteSerial(22);
  REQUIRE(b.B.clip->writePng(second) == ClipStatus::Ok);
  b.settle({&A});

  // A's source was cancelled, so A now reads B's image through the compositor.
  // B must keep dispatching while A blocks in readPng.
  auto fut = A.on([&](WlClipboard &c) {
    Bytes o;
    return c.readPng(o) == ClipStatus::Ok && eq(o, second);
  });
  REQUIRE(b.B.pumpUntil([&] { return fut.wait_for(std::chrono::milliseconds(0)) == kReady; },
                        8000));
  CHECK(fut.get());
}

TEST_CASE("clipboard wayland: reading needs keyboard focus - writing needs focus and an input serial",
          "[clipboard][wayland]") {
  Bench b;
  Client f(b.comp.name());
  CHECK(f.clip->available());
  CHECK_FALSE(f.clip->canRead());
  CHECK_FALSE(f.clip->canWrite());
  // No serial yet: the caller is told to use another way (the tool fallback).
  CHECK(f.clip->writePng(fakePng(64, 1)) == ClipStatus::Unsupported);
  CHECK(f.clip->writeText("x") == ClipStatus::Unsupported);

  f.clip->noteSerial(5);
  CHECK_FALSE(f.clip->canWrite()); // a serial, but no focus
  f.clip->setFocused(true);
  CHECK(f.clip->canRead());
  CHECK(f.clip->canWrite());
  f.clip->setFocused(false);
  CHECK_FALSE(f.clip->canRead());
  CHECK_FALSE(f.clip->canWrite());
}

TEST_CASE("clipboard wayland: an input serial is only trusted for a few seconds",
          "[clipboard][wayland][slow]") {
  // set_selection is silently ignored when the serial is too old, so a stale
  // one must send the caller to the tool fallback instead.
  Bench b;
  Client f(b.comp.name());
  f.clip->setFocused(true);
  f.clip->noteSerial(5);
  REQUIRE(f.clip->canWrite());
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));
  CHECK_FALSE(f.clip->canWrite());
  CHECK(f.clip->canRead()); // reading never needs a serial
  f.clip->noteSerial(6);    // a new input event renews it
  CHECK(f.clip->canWrite());
}

TEST_CASE("clipboard wayland image: a reader that closes the pipe mid-transfer does not kill the writer",
          "[clipboard][wayland]") {
  std::signal(SIGPIPE, SIG_DFL); // the default: an unguarded write would end this process
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(3);
  const Bytes png = fakePng(8u << 20, 3);
  REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(png); }).get() == ClipStatus::Ok);
  b.settle({&A});

  {
    RawReader r(b.comp);
    REQUIRE(r.gotSelection);
    r.receiveAndAbort(16, /*abort=*/true);
  }
  CHECK(A.on([](WlClipboard &c) { return c.available(); }).get()); // still alive

  // and a later, complete transfer still works
  b.B.roundtrip();
  Bytes got;
  REQUIRE(b.B.clip->readPng(got) == ClipStatus::Ok);
  CHECK(eq(got, png));
}

TEST_CASE("clipboard wayland image: a source that stalls is given up on - not waited for forever",
          "[clipboard][wayland][slow]") {
  Bench b;
  RawPeer stalled(b.comp.name(), {"image/png"}, /*mode=*/1);
  b.settle();
  REQUIRE(b.B.clip->hasPng() == ClipStatus::Ok);

  const auto t0 = std::chrono::steady_clock::now();
  Bytes got;
  const ClipStatus st = b.B.clip->readPng(got);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  CHECK(st == ClipStatus::Error);
  CHECK(got.empty());
  CHECK(ms >= 2500); // it did wait for the idle timeout...
  CHECK(ms < 8000);  // ...but not much longer
}

TEST_CASE("clipboard wayland: a selection with no usable type reads as nothing", "[clipboard][wayland]") {
  Bench b;
  SECTION("html and uri-list are neither image nor text") {
    RawPeer p(b.comp.name(), {"text/html", "text/uri-list"}, 0, Bytes{'x'});
    b.settle();
    Bytes png;
    std::string text;
    CHECK(b.B.clip->hasPng() == ClipStatus::None);
    CHECK(b.B.clip->readPng(png) == ClipStatus::None);
    CHECK(b.B.clip->readText(text) == ClipStatus::None);
  }
  SECTION("text is not an image") {
    RawPeer p(b.comp.name(), {"text/plain"}, 0, Bytes{'h', 'i'});
    b.settle();
    Bytes png;
    CHECK(b.B.clip->hasPng() == ClipStatus::None);
    CHECK(b.B.clip->readPng(png) == ClipStatus::None);
  }
}

TEST_CASE("clipboard wayland: when the owner goes away the selection is cleared", "[clipboard][wayland]") {
  Bench b;
  auto A = std::make_unique<ThreadClient>(b.comp.name());
  A->focus(7);
  REQUIRE(A->on([&](WlClipboard &c) { return c.writePng(fakePng(512, 1)); }).get() ==
          ClipStatus::Ok);
  b.settle({A.get()});
  REQUIRE(b.B.clip->hasPng() == ClipStatus::Ok);

  A.reset(); // the other application exits
  CHECK(b.B.pumpUntil([&] { return b.B.clip->hasPng() != ClipStatus::Ok; }));
}

TEST_CASE("clipboard wayland: no file descriptors leak - even after aborted and half-written transfers",
          "[clipboard][wayland]") {
  std::signal(SIGPIPE, SIG_DFL);
  const int before = countFds();
  {
    Bench b;
    auto A = std::make_unique<ThreadClient>(b.comp.name());
    A->focus(7);
    const Bytes png = fakePng(12u << 20, 3);
    REQUIRE(A->on([&](WlClipboard &c) { return c.writePng(png); }).get() == ClipStatus::Ok);
    b.settle({A.get()});

    Bytes got; // a complete transfer
    REQUIRE(b.B.clip->readPng(got) == ClipStatus::Ok);
    {
      RawReader r(b.comp); // an aborted one
      r.receiveAndAbort(16, true);
    }
    {
      RawReader r(b.comp); // shutdown() while a write is still pending
      const int fd = r.receiveAndAbort(16, false);
      A->on([](WlClipboard &c) {
         c.shutdown();
         return 0;
       }).get();
      if (fd >= 0)
        close(fd);
    }
    A.reset();
  }
  CHECK(countFds() <= before);
}

// ============================================================================
// Text
// ============================================================================

TEST_CASE("clipboard wayland text: UTF-8 survives the round trip byte for byte", "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(31);
  REQUIRE(A.on([&](WlClipboard &c) { return c.writeText(kUnicodeText); }).get() ==
          ClipStatus::Ok);
  b.settle({&A});

  std::string got;
  REQUIRE(b.B.clip->readText(got) == ClipStatus::Ok);
  CHECK(eq(got, kUnicodeText)); // multi-byte characters, CRLF, tab, quotes, an embedded NUL

  // the owner reads its own text directly
  CHECK(A.on([&](WlClipboard &c) {
           std::string o;
           return c.readText(o) == ClipStatus::Ok && eq(o, kUnicodeText);
         }).get());
}

TEST_CASE("clipboard wayland text: the source offers the standard text types", "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(31);
  REQUIRE(A.on([&](WlClipboard &c) { return c.writeText("x"); }).get() == ClipStatus::Ok);
  b.settle({&A});
  for (const char *mime :
       {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT"}) {
    CAPTURE(mime);
    CHECK(b.comp.logHas(std::string("offer ") + mime + "\n"));
  }
}

TEST_CASE("clipboard wayland text: a large text transfers intact", "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(32);
  std::string big(20u << 20, 'x');
  for (size_t i = 0; i < big.size(); i += 97)
    big[i] = static_cast<char>('a' + i % 26);
  REQUIRE(A.on([&](WlClipboard &c) { return c.writeText(big); }).get() == ClipStatus::Ok);
  b.settle({&A});
  std::string got;
  REQUIRE(b.B.clip->readText(got) == ClipStatus::Ok);
  CHECK(eq(got, big));
}

TEST_CASE("clipboard wayland text: text and image selections never answer each other's requests",
          "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(33);
  Bytes png;
  std::string text;

  SECTION("an image on the clipboard") {
    REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(fakePng(2048, 4)); }).get() ==
            ClipStatus::Ok);
    b.settle({&A});
    CHECK(b.B.clip->readText(text) == ClipStatus::None);
    CHECK(text.empty());
    CHECK(A.on([](WlClipboard &c) { // the owner too: not PNG bytes handed out as text
             std::string o;
             return c.readText(o) == ClipStatus::None;
           }).get());
  }
  SECTION("text on the clipboard") {
    REQUIRE(A.on([&](WlClipboard &c) { return c.writeText("now text"); }).get() ==
            ClipStatus::Ok);
    b.settle({&A});
    CHECK(b.B.clip->hasPng() == ClipStatus::None);
    CHECK(b.B.clip->readPng(png) == ClipStatus::None);
    CHECK(A.on([](WlClipboard &c) { // the owner too: not text handed out as a PNG
             Bytes o;
             return c.readPng(o) == ClipStatus::None && c.hasPng() == ClipStatus::None;
           }).get());
  }
}

TEST_CASE("clipboard wayland text: empty text can be written and reads back as no text",
          "[clipboard][wayland]") {
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(35);
  REQUIRE(A.on([](WlClipboard &c) { return c.writeText(""); }).get() == ClipStatus::Ok);
  b.settle({&A});
  std::string got = "stale";
  CHECK(b.B.clip->readText(got) == ClipStatus::None);
  CHECK(got.empty());
}

TEST_CASE("clipboard wayland text: reading picks the best type the source offers", "[clipboard][wayland]") {
  // Mode 3 makes the source answer with the NAME of the type that was
  // requested, so what comes back shows which one the reader chose.
  struct Case {
    const char *what;
    std::vector<const char *> offered;
    const char *expected;
  };
  const Case cases[] = {
      {"UTF8_STRING beats text/plain and STRING",
       {"text/plain", "UTF8_STRING", "STRING"},
       "UTF8_STRING"},
      {"the charset type beats everything, matched case-insensitively and echoed in the "
       "source's own spelling",
       {"STRING", "text/plain", "text/plain;charset=UTF-8"},
       "text/plain;charset=UTF-8"},
      {"a space after the semicolon is tolerated",
       {"text/plain; charset=utf-8"},
       "text/plain; charset=utf-8"},
      {"bare text/plain is used when it is all there is", {"text/plain"}, "text/plain"},
  };
  for (const Case &c : cases) {
    SECTION(c.what) {
      Bench b;
      RawPeer p(b.comp.name(), c.offered, /*mode=*/3);
      b.settle();
      std::string got;
      REQUIRE(b.B.clip->readText(got) == ClipStatus::Ok);
      CHECK(got == c.expected);
    }
  }
}

TEST_CASE("clipboard wayland text: legacy STRING is Latin-1 and is converted to UTF-8",
          "[clipboard][wayland]") {
  Bench b;
  RawPeer p(b.comp.name(), {"STRING"}, 0, Bytes{'c', 'a', 'f', 0xE9});
  b.settle();
  std::string got;
  REQUIRE(b.B.clip->readText(got) == ClipStatus::Ok);
  CHECK(got == "caf\xC3\xA9");
}

// ============================================================================
// The public API routed through the native backend (real PNG codec)
// ============================================================================

TEST_CASE("clipboard wayland: setImage / getImage / setText / getText go through the compositor",
          "[clipboard][wayland]") {
  using namespace liteui_clipboard;
  Bench b;
  ThreadClient A(b.comp.name());
  A.focus(21);
  UseBackend backend(*b.B.clip);

  CHECK(imageSupported());

  // A copies a real PNG; B reads it with the public API, decoding for real.
  const CanvasImage img = makeImage(48, 32, 5);
  const Bytes png = realPng(img);
  REQUIRE(A.on([&](WlClipboard &c) { return c.writePng(png); }).get() == ClipStatus::Ok);
  b.settle({&A});
  CHECK(hasImage());
  CHECK(same(getImage(), img));

  // B copies an image with the public API; A receives the PNG it encoded.
  const CanvasImage img2 = makeImage(20, 30, 9);
  b.B.clip->noteSerial(77);
  REQUIRE(setImage(img2));
  b.settle({&A});
  auto fut = A.on([&](WlClipboard &c) {
    Bytes o;
    return c.readPng(o) == ClipStatus::Ok ? o : Bytes{};
  });
  REQUIRE(b.B.pumpUntil([&] { return fut.wait_for(std::chrono::milliseconds(0)) == kReady; },
                        8000));
  const Bytes received = fut.get();
  REQUIRE(cd::hasPngSignature(received));
  const auto decoded = liteui_image::decodeMemory(received.data(), received.size());
  CHECK(same(decoded, img2));

  // Text.
  b.B.clip->noteSerial(78);
  REQUIRE(setText("api text"));
  CHECK(getText() == std::optional<std::string>("api text"));
  setTextWithFallback("a\r\nb");
  CHECK(getTextWithFallback() == "a\nb"); // CRLF normalised to LF

  // An image on the clipboard reads as "" for text, not nullopt: the
  // compositor answered, so neither a tool nor the stale fallback is used.
  b.B.clip->noteSerial(79);
  REQUIRE(setImage(img));
  const auto t = getText();
  REQUIRE(t);
  CHECK(t->empty());
  CHECK(getTextWithFallback().empty());
}

// ============================================================================
// Persist on exit
// ============================================================================

TEST_CASE("clipboard wayland: a copy still owned at shutdown is handed to wl-copy so it outlives the app",
          "[clipboard][wayland]") {
  ToolBox box;
  box.install("wl-copy", kRecordingCopy);
  box.install("wl-paste", kFakeWlPaste);
  TempDir out;
  EnvMap env = box.env(true);
  env["LITEUI_TEST_OUT"] = out.path();
  CLIPBOARD_RUN_CHILD("clipboard child: wayland persist on exit", env);
}

// Needs the fake tools on PATH before the first use of the tool detection, so
// it runs in a child process.
TEST_CASE("clipboard child: wayland persist on exit", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  const std::string out = *getEnv("LITEUI_TEST_OUT");
  const std::string bin = out + "/persist.bin", args = out + "/persist.args";
  // Every section starts from a clean recorder: the sections share this
  // directory, and "nothing was handed over" must not see an earlier file.
  std::filesystem::remove(bin);
  std::filesystem::remove(args);
  Bench b;

  auto ownThenShutdown = [&](bool image, const std::string &text, auto &&beforeShutdown) {
    ThreadClient P(b.comp.name());
    P.focus(41);
    if (image)
      REQUIRE(P.on([&](WlClipboard &c) { return c.writePng(fakePng(1500, 5)); }).get() ==
              ClipStatus::Ok);
    else
      REQUIRE(P.on([&](WlClipboard &c) { return c.writeText(text); }).get() == ClipStatus::Ok);
    b.settle({&P});
    beforeShutdown(P);
    P.on([](WlClipboard &c) {
       c.shutdown();
       return 0;
     }).get();
  };
  auto nothing = [](ThreadClient &) {};

  SECTION("text") {
    ownThenShutdown(false, "persist me", nothing);
    CHECK(readFile(bin) == "persist me");
    CHECK(readFile(args).find("image/png") == std::string::npos);
  }
  SECTION("an image, byte for byte, with --type image/png") {
    ownThenShutdown(true, "", nothing);
    CHECK(eq(bytesOf(readFile(bin)), fakePng(1500, 5)));
    CHECK(readFile(args).find("--type image/png") != std::string::npos);
  }
  SECTION("not when switched off") {
    liteui_clipboard::setPersistOnExit(false);
    ownThenShutdown(false, "nope", nothing);
    liteui_clipboard::setPersistOnExit(true);
    CHECK(readFile(bin).empty());
  }
  SECTION("not when someone else has taken the clipboard since") {
    ownThenShutdown(false, "stale", [&](ThreadClient &P) {
      b.B.clip->noteSerial(51);
      REQUIRE(b.B.clip->writeText("newer") == ClipStatus::Ok);
      b.settle({&P}); // P has now been told its source was cancelled
    });
    CHECK(readFile(bin).empty());
  }
}

} // namespace clipboard_test

#endif // !_WIN32 && LITEUI_TEST_WAYLAND_SERVER
