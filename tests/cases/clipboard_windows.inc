// Tests for the image clipboard on Windows, against the REAL clipboard
// (registered "PNG", CF_DIBV5, CF_DIB) and the real WIC codec: what setImage
// writes and in which order, what getImage reads and in which order, text/image
// replacement, the *WithFallback functions, and a clipboard another process
// holds open.
//
// These cases overwrite the clipboard of the desktop session they run in, so
// do not run them while you have something precious copied. A session without
// a usable clipboard (a service, some CI setups) makes them skip instead of
// fail.
// Empty elsewhere. (Included from liteui_tests.cpp — not a standalone .cpp.)

#if defined(_WIN32)

namespace clipboard_test {

UINT pngFormat() {
  static const UINT f = RegisterClipboardFormatW(L"PNG");
  return f;
}

bool openRetry() {
  for (int i = 0; i < 50; ++i) {
    if (OpenClipboard(nullptr))
      return true;
    Sleep(20);
  }
  return false;
}

bool clipboardUsable() {
  if (!openRetry())
    return false;
  CloseClipboard();
  return true;
}

#define CLIPBOARD_NEED_SESSION()                                                    \
  if (!clipboardUsable())                                                      \
  CLIPBOARD_SKIP("no usable clipboard in this session")

void clearClipboard() {
  if (openRetry()) {
    EmptyClipboard();
    CloseClipboard();
  }
}

// Replaces the clipboard with exactly these formats, in this order.
bool putFormats(std::initializer_list<std::pair<UINT, Bytes>> formats) {
  if (!openRetry())
    return false;
  EmptyClipboard();
  bool ok = true;
  for (const auto &[fmt, bytes] : formats) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!h) {
      ok = false;
      continue;
    }
    if (void *p = GlobalLock(h)) {
      std::memcpy(p, bytes.data(), bytes.size());
      GlobalUnlock(h);
    }
    if (!SetClipboardData(fmt, h)) {
      GlobalFree(h);
      ok = false;
    }
  }
  CloseClipboard();
  return ok;
}

std::vector<UINT> formatsOnClipboard() {
  std::vector<UINT> v;
  if (!openRetry())
    return v;
  for (UINT f = EnumClipboardFormats(0); f != 0; f = EnumClipboardFormats(f))
    v.push_back(f);
  CloseClipboard();
  return v;
}

bool has(const std::vector<UINT> &v, UINT f) {
  return std::find(v.begin(), v.end(), f) != v.end();
}

Bytes utf16(const wchar_t *s) {
  const size_t n = (std::wcslen(s) + 1) * sizeof(wchar_t);
  Bytes b(n);
  std::memcpy(b.data(), s, n);
  return b;
}

// Starts this binary again running a hidden child test, WITHOUT waiting for it.
HANDLE startChild(const std::string &testName, const EnvMap &env) {
  std::vector<std::unique_ptr<EnvGuard>> guards;
  guards.push_back(std::make_unique<EnvGuard>("LITEUI_TEST_CHILD", "1"));
  for (const auto &[k, v] : env)
    guards.push_back(std::make_unique<EnvGuard>(k, v));
  // The child's own console output would interleave with ours: send it nowhere.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = si.hStdError = nul;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION pi{};
  std::string cmd = "\"" + selfExe() + "\" \"" + testName + "\"";
  const BOOL started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0,
                                      nullptr, nullptr, &si, &pi);
  CloseHandle(nul);
  if (!started)
    return nullptr;
  CloseHandle(pi.hThread);
  return pi.hProcess;
}


// ============================================================================
// What setImage puts on the clipboard
// ============================================================================

TEST_CASE("clipboard windows: setImage writes PNG first - then CF_DIBV5; Windows adds CF_DIB",
          "[clipboard][windows]") {
  CLIPBOARD_NEED_SESSION();
  clearClipboard();
  REQUIRE(liteui_clipboard::setImage(makeImage(6, 4, 3)));
  const auto formats = formatsOnClipboard();
  REQUIRE_FALSE(formats.empty());
  // Order matters: an app that takes the first format it understands should
  // get the lossless, alpha-preserving one.
  CHECK(formats.front() == pngFormat());
  CHECK(has(formats, CF_DIBV5));
  CHECK(has(formats, CF_DIB)); // synthesized by Windows, so old apps can paste too
}

TEST_CASE("clipboard windows: an image round-trips through the real clipboard and the real codec",
          "[clipboard][windows]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  clearClipboard();
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());
  CHECK(imageSupported());

  const std::vector<CanvasImage> images = {makeImage(33, 17, 2), makeImage(16, 16, 4, /*opaque=*/true),
                                           makeNoiseImage(40, 30, 3)};
  for (const CanvasImage &img : images) {
    CAPTURE(img.width, img.height);
    REQUIRE(setImage(img));
    CHECK(hasImage());
    CHECK(same(getImage(), img)); // pixels and straight alpha survive PNG + WIC
  }
}

TEST_CASE("clipboard windows: setImage replaces text - and setText replaces an image",
          "[clipboard][windows]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  REQUIRE(setText("some text"));
  REQUIRE(setImage(makeImage(4, 4)));
  CHECK(getText() == std::optional<std::string>(""));
  CHECK(hasImage());
  REQUIRE(setText("text again"));
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());
}

TEST_CASE("clipboard windows: a clipboard holding only text has no image", "[clipboard][windows]") {
  CLIPBOARD_NEED_SESSION();
  REQUIRE(putFormats({{CF_UNICODETEXT, utf16(L"hi")}}));
  CHECK_FALSE(liteui_clipboard::hasImage());
  CHECK_FALSE(liteui_clipboard::getImage());
}

// ============================================================================
// What getImage reads, and in which order
// ============================================================================

TEST_CASE("clipboard windows: reading prefers PNG - then CF_DIBV5 - then CF_DIB", "[clipboard][windows]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  const CanvasImage a = makeImage(6, 4, 1), b = makeImage(6, 4, 2);
  Bytes dibB;
  REQUIRE(cd::buildDibV5(b, dibB));

  SECTION("PNG wins over CF_DIBV5") {
    REQUIRE(putFormats({{pngFormat(), realPng(a)}, {CF_DIBV5, dibB}}));
    CHECK(same(getImage(), a));
  }
  SECTION("CF_DIBV5 alone, with its alpha") {
    REQUIRE(putFormats({{CF_DIBV5, dibB}}));
    CHECK(hasImage());
    CHECK(same(getImage(), b));
  }
  SECTION("a corrupt PNG falls through to CF_DIBV5") {
    REQUIRE(putFormats({{pngFormat(), Bytes(64, 0x42)}, {CF_DIBV5, dibB}}));
    CHECK(same(getImage(), b));
  }
  SECTION("CF_DIB alone: 24-bit, bottom-up, row padding, opaque") {
    Bytes v = dibHeader(40, 3, 2, 24, 0);
    add(v, {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 0, 0});
    add(v, {10, 20, 30, 40, 50, 60, 70, 80, 90, 0, 0, 0});
    REQUIRE(putFormats({{CF_DIB, v}}));
    const auto img = getImage();
    REQUIRE(img);
    CHECK(img->width == 3);
    CHECK(img->height == 2);
    CHECK(img->pixels[0] == 30); // top-left is the file's LAST row, BGR -> RGB
    CHECK(img->pixels[1] == 20);
    CHECK(img->pixels[2] == 10);
    CHECK(img->pixels[3] == 255);
  }
  SECTION("a bitmap it cannot read (8-bit palette) is not an image") {
    Bytes v = dibHeader(40, 1, 1, 8, 0);
    v.insert(v.end(), 8, 0);
    REQUIRE(putFormats({{CF_DIB, v}}));
    CHECK_FALSE(getImage());
  }
}

// ============================================================================
// The fallback API against the real clipboard
// ============================================================================

TEST_CASE("clipboard windows fallback: the OS clipboard's answer is final", "[clipboard][windows][fallback]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  const CanvasImage a = makeImage(5, 4, 1), b = makeImage(3, 3, 2);
  clearClipboard();
  clearImageFallback();

  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());

  REQUIRE(setImageWithFallback(a)); // the OS accepted it
  CHECK(same(getImageWithFallback(), a));

  // Another app copies a different image: the OS wins over our stored copy.
  REQUIRE(putFormats({{pngFormat(), realPng(b)}}));
  CHECK(same(getImageWithFallback(), b));

  // The clipboard is emptied: "no image" is final, our copy is NOT resurrected.
  clearClipboard();
  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());

  // Only text on the clipboard: still no image.
  REQUIRE(putFormats({{CF_UNICODETEXT, utf16(L"hi")}}));
  CHECK_FALSE(getImageWithFallback());

  CHECK_FALSE(setImageWithFallback(CanvasImage())); // malformed: rejected
  clearImageFallback();
}

TEST_CASE("clipboard windows fallback: a text copy replaces the image fallback",
          "[clipboard][windows][fallback]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  REQUIRE(setImageWithFallback(makeImage(4, 4)));
  setTextWithFallback("hello");
  clearClipboard();
  CHECK_FALSE(getImageWithFallback());
  clearImageFallback();
}

TEST_CASE("clipboard windows fallback: a clipboard another process holds open is unreachable - "
          "so the in-process copy is used",
          "[clipboard][windows][fallback]") {
  CLIPBOARD_NEED_SESSION();
  using namespace liteui_clipboard;
  TempDir dir;
  const std::string ready = dir.file("ready");

  // Another process takes the clipboard, puts an image on it, and keeps it open.
  HANDLE holder = startChild("clipboard child: windows hold clipboard", {{"LITEUI_TEST_READY", ready}});
  REQUIRE(holder);
  bool up = false;
  for (int i = 0; i < 300 && !up; ++i) {
    up = std::filesystem::exists(ready);
    if (!up)
      Sleep(50);
  }
  REQUIRE(up);

  const CanvasImage mine = makeImage(5, 4, 8), theirs = makeImage(2, 2, 3);
  clearImageFallback();
  CHECK_FALSE(setImageWithFallback(mine)); // the OS refused (locked), but the copy is kept
  CHECK(hasImage());                       // formats can still be queried while it is locked
  CHECK_FALSE(getImage());                 // ...but not read
  const auto got = getImageWithFallback();
  REQUIRE(got);
  CHECK(same(*got, mine));      // so the fallback is used...
  CHECK_FALSE(same(*got, theirs)); // ...and it is OUR image, not the holder's

  TerminateProcess(holder, 0);
  WaitForSingleObject(holder, 5000);
  CloseHandle(holder);
  clearImageFallback();
}

// Hidden child of the test above: owns the clipboard (through a real window,
// as another application would) and holds it open until it is killed.
TEST_CASE("clipboard child: windows hold clipboard", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  REQUIRE(liteui_clipboard::setImage(makeImage(2, 2, 3)));
  HWND w = CreateWindowExA(0, "STATIC", "holder", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                           nullptr, nullptr);
  REQUIRE(w);
  REQUIRE(OpenClipboard(w));
  writeFile(*getEnv("LITEUI_TEST_READY"), std::string("up"));
  Sleep(30000); // the parent terminates this process when it is done
  CloseClipboard();
}

} // namespace clipboard_test

#endif // _WIN32
