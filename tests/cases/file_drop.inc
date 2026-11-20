// File / text drop: the parsers (text/uri-list, CF_HDROP), the router's
// drop-target hit test, and the Dispatcher that turns enter/over/leave/drop
// into View handlers. Platform-neutral: the Wayland and OLE plumbing that
// feeds them needs a compositor / a real drag and is not covered here.

#include <catch2/catch_test_macros.hpp>

namespace liteui_test_drop {

std::string fdJoin(const std::vector<std::string> &v) {
  std::string r;
  for (const auto &s : v)
    r += (r.empty() ? "" : "|") + s;
  return r;
}

// A tiny hand-laid-out tree (window coordinates):
//   root 0,0 400x300
//     A 10,10 100x100  (onDrop)         -> contains B
//       B 20,20 40x40  (onDrop)
//     C 200,10 100x100 (onDrop, disabled)
//     D 200,150 100x100 (no handlers)
struct DropTree {
  View root;
  liteui_pointer::Router router;
  std::vector<std::string> log;

  static void geom(View &v, float x, float y, float w, float h) {
    v.computed.x = x;
    v.computed.y = y;
    v.computed.w = w;
    v.computed.h = h;
  }
  void hook(View &v, const std::string &name) {
    v.onDrop = [this, name](DropEvent &e) {
      log.push_back(name + ".drop(" + fdJoin(e.paths) + (e.text.empty() ? "" : ";" + e.text) +
                    ")@" + std::to_string(int(e.x)) + "," + std::to_string(int(e.y)));
    };
    v.onDragEnter = [this, name](DropEvent &e) {
      log.push_back(name + ".enter" + (e.hasFiles ? "[files]" : "") + (e.hasText ? "[text]" : ""));
    };
    v.onDragOver = [this, name](DropEvent &) { log.push_back(name + ".over"); };
    v.onDragLeave = [this, name](DropEvent &) { log.push_back(name + ".leave"); };
  }

  DropTree() {
    root.children.resize(3 + 1);
    geom(root, 0, 0, 400, 300);
    View &A = root.children[0];
    View &C = root.children[1];
    View &D = root.children[2];
    geom(A, 10, 10, 100, 100);
    A.children.resize(1);
    geom(A.children[0], 20, 20, 40, 40);
    geom(C, 200, 10, 100, 100);
    geom(D, 200, 150, 100, 100);
    root.children.resize(3);
    hook(A, "A");
    hook(A.children[0], "B");
    hook(C, "C");
    C.disabled = true;
    router.root = &root;
  }
  View &A() { return root.children[0]; }
  View &B() { return root.children[0].children[0]; }
  View &C() { return root.children[1]; }
};

} // namespace liteui_test_drop

using liteui_test_drop::DropTree;
using liteui_test_drop::fdJoin;

TEST_CASE("drop: text/uri-list parsing", "[drop]") {
  using liteui_drop::parseUriList;
  SECTION("plain, CRLF, comments, blanks") {
    auto p = parseUriList("# a comment\r\nfile:///home/u/a.txt\r\n\r\nfile:///tmp/b%20c.png\r\n");
    REQUIRE(p.size() == 2);
    CHECK(p[0] == "/home/u/a.txt");
    CHECK(p[1] == "/tmp/b c.png");
  }
  SECTION("no trailing newline; localhost and single-slash forms") {
    auto p = parseUriList("file://localhost/x/y\nfile:/z");
    REQUIRE(p.size() == 2);
    CHECK(p[0] == "/x/y");
    CHECK(p[1] == "/z");
  }
  SECTION("scheme is case-insensitive") {
    auto p = parseUriList("FILE:///Up/Case");
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "/Up/Case");
  }
  SECTION("UTF-8 percent escapes decode to bytes") {
    auto p = parseUriList("file:///t/caf%C3%A9.txt");
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "/t/caf\xC3\xA9.txt");
  }
  SECTION("an escaped # or ? is part of the name") {
    auto p = parseUriList("file:///t/a%23b%3Fc");
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "/t/a#b?c");
  }
  SECTION("rejects other hosts, relative paths, NULs, empty paths") {
    CHECK(parseUriList("file://otherhost/x").empty());
    CHECK(parseUriList("file:relative").empty());
    CHECK(parseUriList("file:///a%00b").empty());
    CHECK(parseUriList("file://").empty());
    CHECK(parseUriList("").empty());
  }
  SECTION("bad escapes are kept literally") {
    auto p = parseUriList("file:///a%zzb%2");
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "/a%zzb%2");
  }
  SECTION("non-file URIs go to `other`") {
    std::vector<std::string> other;
    auto p = parseUriList("https://example.com/x\nfile:///f\nmailto:a@b", &other);
    CHECK(p.size() == 1);
    REQUIRE(other.size() == 2);
    CHECK(other[0] == "https://example.com/x");
    CHECK(other[1] == "mailto:a@b");
  }
}

TEST_CASE("drop: text helpers", "[drop]") {
  using namespace liteui_drop;
  CHECK(latin1ToUtf8("caf\xE9") == "caf\xC3\xA9");
  const char16_t smile[] = {0xD83D, 0xDE00, u'x'};
  CHECK(utf16ToUtf8(smile, 3) == "\xF0\x9F\x98\x80x");
  const char16_t lone[] = {0xD800, u'a'};
  CHECK(utf16ToUtf8(lone, 2) == "\xEF\xBF\xBD" "a");
  const char16_t e[] = {u'\u00e9', u'\u20ac'};
  CHECK(utf16ToUtf8(e, 2) == "\xC3\xA9\xE2\x82\xAC");
}

TEST_CASE("drop: CF_HDROP block parsing", "[drop]") {
  auto make = [](const std::u16string &list, bool wide) {
    std::vector<uint8_t> b(20, 0);
    const uint32_t off = 20, w = wide ? 1 : 0;
    std::memcpy(b.data(), &off, 4);
    std::memcpy(b.data() + 16, &w, 4);
    if (wide) {
      for (char16_t c : list) {
        b.push_back(uint8_t(c & 0xFF));
        b.push_back(uint8_t(c >> 8));
      }
    } else {
      for (char16_t c : list)
        b.push_back(uint8_t(c));
    }
    return b;
  };
  SECTION("wide list, two names, non-ASCII") {
    std::u16string l = u"C:\\a\\b.txt";
    l.push_back(0);
    l += u"D:\\caf\u00e9.png";
    l.push_back(0);
    l.push_back(0);
    auto b = make(l, true);
    auto p = liteui_drop::parseDropFiles(b.data(), b.size());
    REQUIRE(p.size() == 2);
    CHECK(p[0] == "C:\\a\\b.txt");
    CHECK(p[1] == "D:\\caf\xC3\xA9.png");
  }
  SECTION("ANSI list") {
    std::u16string l = u"x.txt";
    l.push_back(0);
    l.push_back(0);
    auto b = make(l, false);
    auto p = liteui_drop::parseDropFiles(b.data(), b.size());
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "x.txt");
  }
  SECTION("missing final terminator is still read") {
    auto b = make(u"abc", true);
    auto p = liteui_drop::parseDropFiles(b.data(), b.size());
    REQUIRE(p.size() == 1);
    CHECK(p[0] == "abc");
  }
  SECTION("garbage is rejected, not read out of bounds") {
    CHECK(liteui_drop::parseDropFiles(nullptr, 0).empty());
    std::vector<uint8_t> tiny(10, 0);
    CHECK(liteui_drop::parseDropFiles(tiny.data(), tiny.size()).empty());
    auto b = make(u"abc", true);
    const uint32_t bad = 9999;
    std::memcpy(b.data(), &bad, 4); // offset past the end
    CHECK(liteui_drop::parseDropFiles(b.data(), b.size()).empty());
  }
}

TEST_CASE("drop: router picks the deepest enabled onDrop view", "[drop]") {
  DropTree t;
  CHECK(t.router.dropTargetAt(30, 30) == &t.B());   // inside B (inside A)
  CHECK(t.router.dropTargetAt(15, 15) == &t.A());   // A, outside B
  CHECK(t.router.dropTargetAt(210, 20) == nullptr); // C is disabled
  CHECK(t.router.dropTargetAt(210, 160) == nullptr); // D has no handler
  CHECK(t.router.dropTargetAt(399, 299) == nullptr); // bare root
  t.B().style.visibility = Visibility::Hidden;       // hidden views are skipped
  CHECK(t.router.dropTargetAt(30, 30) == &t.A());
  liteui_pointer::Router empty;
  CHECK(empty.dropTargetAt(1, 1) == nullptr);
}

TEST_CASE("drop: dispatcher fires enter/over/leave as the target changes", "[drop]") {
  DropTree t;
  liteui_drop::Dispatcher d(t.router);
  int after = 0;
  d.afterHandlers = [&] { ++after; };

  CHECK(d.enter(15, 15, true, false)); // onto A: enter, then a first over
  CHECK(d.over(16, 16));
  CHECK(d.over(30, 30)); // into B: A leaves, B enters
  CHECK_FALSE(d.over(300, 250)); // bare area, no window handler: refused
  CHECK(fdJoin(t.log) == "A.enter[files]|A.over|A.over|A.leave|B.enter[files]|B.over|B.leave");
  CHECK(after >= 1);

  t.log.clear();
  d.leave(); // nothing is current any more
  CHECK(t.log.empty());
}

TEST_CASE("drop: a window handler accepts everywhere and gets stray drops", "[drop]") {
  DropTree t;
  liteui_drop::Dispatcher d(t.router);
  std::vector<std::string> got;
  float wx = -1, wy = -1;
  d.windowHandler = [&](DropEvent &e) {
    got = e.paths;
    wx = e.x;
    wy = e.y;
  };
  CHECK(d.enter(300, 250, true, false)); // accepted although no view is there
  CHECK(d.drop(300, 250, {"/a", "/b"}, {}));
  CHECK(fdJoin(got) == "/a|/b");
  CHECK(wx == 300);
  CHECK(wy == 250);

  DropTree t2;
  liteui_drop::Dispatcher d2(t2.router);
  CHECK_FALSE(d2.drop(300, 250, {"/a"}, {})); // nobody home
  CHECK(t2.log.empty());
}

TEST_CASE("drop: onDragLeave runs right before onDrop; coordinates are local", "[drop]") {
  DropTree t;
  liteui_drop::Dispatcher d(t.router);
  d.enter(30, 30, true, false);
  t.log.clear();
  CHECK(d.drop(30, 35, {"/x/y"}, {}));
  // B is at 20,20 so the local position is (10, 15).
  CHECK(fdJoin(t.log) == "B.leave|B.drop(/x/y)@10,15");
  CHECK(d.target() == nullptr);

  t.log.clear();
  CHECK(d.drop(15, 15, {}, "hello"));
  CHECK(fdJoin(t.log) == "A.drop(;hello)@5,5");
}

TEST_CASE("drop: a rebuilt tree is never called through a stale pointer", "[drop]") {
  DropTree t;
  liteui_drop::Dispatcher d(t.router);
  d.enter(15, 15, true, false);
  t.log.clear();
  t.router.invalidate(); // what the tree rebuild does: old views may be gone
  d.leave();
  CHECK(t.log.empty()); // A.leave must NOT fire on a possibly-dead view

  d.enter(15, 15, true, false);
  t.router.invalidate();
  t.log.clear();
  d.over(30, 30); // new target is resolved fresh, old one is not called
  CHECK(fdJoin(t.log) == "B.enter[files]|B.over");
}

TEST_CASE("drop: handlers that rebuild the tree do not leave a dangling target", "[drop]") {
  DropTree t;
  liteui_drop::Dispatcher d(t.router);
  int enters = 0;
  t.A().onDragEnter = [&](DropEvent &) {
    ++enters;
    t.router.invalidate();
  };
  d.enter(15, 15, true, false);
  CHECK(enters == 1);
  CHECK(d.target() == &t.A()); // re-resolved after the rebuild
  d.over(16, 16);              // same target: no second enter
  CHECK(enters == 1);
}
