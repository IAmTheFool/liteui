// Tests for the tool-based clipboard backends on Linux (wl-clipboard, xclip,
// xsel), driven by the fake tools in clipboard_tools_mock.hpp, and for the
// in-process image fallback when no tool can carry images.
//
// Each scenario has a parent test case (builds the fake tools, then re-runs
// this binary as a child) and a hidden child test case ("clipboard child: ...")
// that makes the assertions against the public liteui_clipboard API. See
// clipboard_helpers.hpp for why.
// Empty on Windows. (Included from liteui_tests.cpp — not a standalone .cpp.)

#if !defined(_WIN32)

#include <csignal>

#include "clipboard_tools_mock.hpp"

namespace clipboard_test {

// ---- scenarios (run inside a child; they use the PUBLIC API) ---------------------

// wl-clipboard and xclip behave the same through the API.
void toolImageAndTextRoundTrip() {
  using namespace liteui_clipboard;
  const std::string store = *getEnv("LITEUI_FAKE_STORE");
  REQUIRE(imageSupported());
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());

  const CanvasImage img = makeImage(24, 16, 3);
  REQUIRE(setImage(img));
  CHECK(cd::hasPngSignature(bytesOf(readFile(store + ".img")))); // a real PNG reached the tool
  CHECK(hasImage());
  CHECK(same(getImage(), img)); // real encode -> tool -> real decode

  // Someone else puts bytes that are not a PNG on the clipboard under image/png.
  writeFile(store + ".img", std::string("definitely not a png"));
  CHECK(hasImage());       // it is offered...
  CHECK_FALSE(getImage()); // ...but refused: the signature check

  // Text goes through the tool as well, and replaces the image.
  REQUIRE(setText("tool text \xC3\xA9"));
  CHECK(getText() == std::optional<std::string>("tool text \xC3\xA9"));
  CHECK_FALSE(hasImage());

  // An image on the clipboard reads as "" for text (the tool refused), which
  // is different from nullopt ("no usable tool").
  REQUIRE(setImage(img));
  const auto t = getText();
  REQUIRE(t);
  CHECK(t->empty());
}

// No image support at all: no tool, or only text-only xsel.
void fallbackOnlyScenario(bool osKeepsText) {
  using namespace liteui_clipboard;
  const CanvasImage a = makeImage(5, 4, 1), b = makeImage(3, 3, 2);
  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());

  // The OS cannot take it, so false, but our own copy is kept.
  CHECK_FALSE(setImageWithFallback(a));
  CHECK(hasImageWithFallback());
  CHECK(same(getImageWithFallback(), a));
  // The plain OS-only functions are unaffected: the fallback is opt-in.
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());

  // A malformed image is rejected and does not clobber the stored copy.
  CHECK_FALSE(setImageWithFallback(CanvasImage()));
  CHECK(same(getImageWithFallback(), a));

  // Most recent copy wins: text drops the image fallback...
  setTextWithFallback("hi");
  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());
  // ...and an image drops the text fallback.
  CHECK_FALSE(setImageWithFallback(b));
  CHECK(same(getImageWithFallback(), b));
  if (!osKeepsText) // with xsel the OS still holds "hi" and we cannot clear it
    CHECK(getTextWithFallback().empty());

  clearImageFallback();
  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());
}


// ============================================================================
// Parent test cases
// ============================================================================

TEST_CASE("clipboard tools: wl-clipboard backend carries images and text", "[clipboard][tools]") {
  ToolBox box;
  box.install("wl-copy", kFakeWlCopy);
  box.install("wl-paste", kFakeWlPaste);
  CLIPBOARD_RUN_CHILD("clipboard child: tools wl", box.env(true));
}

TEST_CASE("clipboard tools: xclip backend carries images and text", "[clipboard][tools]") {
  ToolBox box;
  box.install("xclip", kFakeXclip);
  CLIPBOARD_RUN_CHILD("clipboard child: tools xclip", box.env(false));
}

TEST_CASE("clipboard tools: xsel is text-only - so images report unsupported", "[clipboard][tools]") {
  ToolBox box;
  box.install("xsel", kFakeXsel);
  CLIPBOARD_RUN_CHILD("clipboard child: tools xsel", box.env(false));
}

TEST_CASE("clipboard tools: with no clipboard tool nothing works - and the fallbacks take over",
          "[clipboard][tools]") {
  ToolBox box; // empty
  CLIPBOARD_RUN_CHILD("clipboard child: tools none", box.env(true));
}

TEST_CASE("clipboard tools: a copy tool that fails makes setImage fail", "[clipboard][tools]") {
  ToolBox box;
  box.install("wl-copy", kFailingCopy);
  box.install("wl-paste", kFakeWlPaste);
  CLIPBOARD_RUN_CHILD("clipboard child: tools failing copy", box.env(true));
}

TEST_CASE("clipboard tools: a copy tool that dies without reading must not kill the app (SIGPIPE)",
          "[clipboard][tools]") {
  ToolBox box;
  box.install("wl-copy", kDeadCopy);
  box.install("wl-paste", kFakeWlPaste);
  CLIPBOARD_RUN_CHILD("clipboard child: tools dead tool", box.env(true));
}

TEST_CASE("clipboard fallback: with no image support the in-process copy is used",
          "[clipboard][fallback][tools]") {
  SECTION("no tool at all") {
    ToolBox box;
    CLIPBOARD_RUN_CHILD("clipboard child: fallback none", box.env(true));
  }
  SECTION("only xsel (text-only)") {
    ToolBox box;
    box.install("xsel", kFakeXsel);
    CLIPBOARD_RUN_CHILD("clipboard child: fallback xsel", box.env(false));
  }
}

TEST_CASE("clipboard fallback: when the OS clipboard can be asked - its answer is final",
          "[clipboard][fallback][tools]") {
  ToolBox box;
  box.install("wl-copy", kFakeWlCopy);
  box.install("wl-paste", kFakeWlPaste);
  CLIPBOARD_RUN_CHILD("clipboard child: fallback authoritative", box.env(true));
}

// ============================================================================
// Child test cases (hidden; run only by the parents above)
// ============================================================================

TEST_CASE("clipboard child: tools wl", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  toolImageAndTextRoundTrip();
}

TEST_CASE("clipboard child: tools xclip", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  toolImageAndTextRoundTrip();
}

TEST_CASE("clipboard child: tools xsel", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  using namespace liteui_clipboard;
  const CanvasImage img = makeImage(8, 8, 1);
  CHECK_FALSE(imageSupported());
  CHECK_FALSE(setImage(img));
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());
  REQUIRE(setText("xsel text"));
  CHECK(getText() == std::optional<std::string>("xsel text"));
}

TEST_CASE("clipboard child: tools none", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  using namespace liteui_clipboard;
  CHECK_FALSE(imageSupported());
  CHECK_FALSE(setImage(makeImage(4, 4)));
  CHECK_FALSE(hasImage());
  CHECK_FALSE(getImage());
  CHECK_FALSE(setText("x"));
  CHECK_FALSE(getText()); // nullopt = "no usable tool"
  setTextWithFallback("kept in process");
  CHECK(getTextWithFallback() == "kept in process");
}

TEST_CASE("clipboard child: tools failing copy", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  CHECK_FALSE(liteui_clipboard::setImage(makeImage(16, 16, 2)));
}

TEST_CASE("clipboard child: tools dead tool", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  using namespace liteui_clipboard;
  std::signal(SIGPIPE, SIG_DFL); // the default: an unguarded write would kill this process
  // Incompressible, so the PNG is far bigger than a pipe buffer.
  CHECK_FALSE(setImage(makeNoiseImage(1000, 1000, 7)));
  // The guard must put the previous disposition back.
  const auto prev = std::signal(SIGPIPE, SIG_DFL);
  CHECK(prev == SIG_DFL);
}

TEST_CASE("clipboard child: fallback none", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  fallbackOnlyScenario(/*osKeepsText=*/false);
}

TEST_CASE("clipboard child: fallback xsel", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  fallbackOnlyScenario(/*osKeepsText=*/true);
}

TEST_CASE("clipboard child: fallback authoritative", "[.][child]") {
  CLIPBOARD_CHILD_ONLY();
  using namespace liteui_clipboard;
  const std::string store = *getEnv("LITEUI_FAKE_STORE");
  const CanvasImage a = makeImage(5, 4, 1), b = makeImage(3, 3, 2);

  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());

  REQUIRE(setImageWithFallback(a)); // the OS accepted it
  CHECK(same(getImageWithFallback(), a));

  // Another app copies a different image: the OS wins over our stored copy.
  writeFile(store + ".img", realPng(b));
  CHECK(same(getImageWithFallback(), b));

  // The clipboard is emptied: "no image" is final. Our stale copy is NOT
  // resurrected.
  std::filesystem::remove(store + ".img");
  CHECK_FALSE(hasImageWithFallback());
  CHECK_FALSE(getImageWithFallback());

  // Undecodable data under image/png: nullopt, again not the fallback.
  writeFile(store + ".img", std::string("not a png"));
  CHECK_FALSE(getImageWithFallback());
}

} // namespace clipboard_test

#endif // !_WIN32
