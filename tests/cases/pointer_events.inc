// Pointer events: dispatch, bubbling, capture, cancel, click/double-click,
// wheel, enter/leave, cursors, and pen/touch.
//
// These drive liteui_pointer::Router directly — LiteUIImpl's platform
// backends only translate native messages into the same calls, and the
// router needs no window. Included once, from liteui_tests.cpp.

#include <string>
#include <vector>

namespace ptr {

using liteui_pointer::Router;

// A tree laid out once and attached to a router. Build `root` (children
// included) BEFORE calling attach(): attach() lays it out and from then on
// the View addresses must not change.
struct Rig {
  View root;
  Router r;
  std::vector<std::string> log;
  int polls = 0;

  void attach(float w = 400, float h = 400) {
    liteui_layout::layoutRoot(root, w, h);
    r.root = &root;
    r.poll = [this] {
      ++polls;
      return false;
    };
  }
  void note(const std::string &s) { log.push_back(s); }
};

inline std::pair<float, float> centerOf(const View &v) {
  return {v.computed.x + v.computed.w / 2, v.computed.y + v.computed.h / 2};
}

inline PointerEvent ev(std::pair<float, float> p,
                       MouseButton b = MouseButton::None, uint64_t t = 1000) {
  PointerEvent e;
  e.windowX = p.first;
  e.windowY = p.second;
  e.button = b;
  e.timeMs = t;
  return e;
}

// root(Row) -> A(100x100), B(100x100): two siblings side by side.
inline void twoBoxes(Rig &g) {
  g.root = container(300, 100);
  g.root.children.push_back(box(100, 100));
  g.root.children.push_back(box(100, 100));
}

inline void press(Rig &g, std::pair<float, float> p,
                  MouseButton b = MouseButton::Left, uint64_t t = 1000) {
  g.r.down(ev(p, b, t));
}
inline bool release(Rig &g, std::pair<float, float> p,
                    MouseButton b = MouseButton::Left, uint64_t t = 1000) {
  return g.r.up(ev(p, b, t));
}

} // namespace ptr

TEST_CASE("pointer: down bubbles deepest-first until stopPropagation",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(300, 300);
  View parent = container(200, 200);
  parent.children.push_back(box(100, 100));
  g.root.children.push_back(std::move(parent));
  g.root.onPointerDown = [&](PointerEvent &) { g.note("root"); };
  g.root.children[0].onPointerDown = [&](PointerEvent &) { g.note("parent"); };
  g.root.children[0].children[0].onPointerDown = [&](PointerEvent &) {
    g.note("child");
  };
  g.attach();
  auto at = ptr::centerOf(g.root.children[0].children[0]);

  ptr::press(g, at);
  CHECK(g.log == std::vector<std::string>{"child", "parent", "root"});
  ptr::release(g, at);

  g.log.clear();
  g.root.children[0].onPointerDown = [&](PointerEvent &e) {
    g.note("parent");
    e.stopPropagation();
  };
  ptr::press(g, at);
  CHECK(g.log == std::vector<std::string>{"child", "parent"});
}

TEST_CASE("pointer: x/y are local to the view whose handler runs",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  float bx = -1, by = -1, wx = -1, rootX = -1;
  g.root.children[1].onPointerDown = [&](PointerEvent &e) {
    bx = e.x;
    by = e.y;
    wx = e.windowX;
  };
  g.root.onPointerDown = [&](PointerEvent &e) { rootX = e.x; };
  g.attach();
  const View &B = g.root.children[1];
  ptr::press(g, {B.computed.x + 30, B.computed.y + 40});
  CHECK(bx == approx(30));
  CHECK(by == approx(40));
  CHECK(wx == approx(B.computed.x + 30));
  CHECK(rootX == approx(B.computed.x + 30 - g.root.computed.x));
}

TEST_CASE("pointer: a press captures; moves and up follow it off the view",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  float moveX = -1;
  g.root.children[0].onPointerDown = [&](PointerEvent &) { g.note("A.down"); };
  g.root.children[0].onPointerMove = [&](PointerEvent &e) {
    g.note("A.move");
    moveX = e.x;
  };
  g.root.children[0].onPointerUp = [&](PointerEvent &) { g.note("A.up"); };
  g.root.children[1].onPointerMove = [&](PointerEvent &) { g.note("B.move"); };
  g.root.children[1].onPointerUp = [&](PointerEvent &) { g.note("B.up"); };
  g.attach();
  const View &A = g.root.children[0];
  const View &B = g.root.children[1];

  ptr::press(g, ptr::centerOf(A));
  CHECK(g.r.capturedView() == &g.root.children[0]);
  g.r.move(ptr::ev(ptr::centerOf(B)));
  // Local x of a point over B, in A's own space: past A's right edge.
  CHECK(moveX == approx(ptr::centerOf(B).first - A.computed.x));
  ptr::release(g, ptr::centerOf(B));
  CHECK(g.log == std::vector<std::string>{"A.down", "A.move", "A.up"});
  CHECK(g.r.capturedView() == nullptr);

  // Capture is over: a plain hover move now reaches B, not A.
  g.log.clear();
  g.r.move(ptr::ev(ptr::centerOf(B)));
  CHECK(g.log == std::vector<std::string>{"B.move"});
}

TEST_CASE("pointer: capture goes to the view that stopped the down, else the "
          "deepest handler",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(300, 300);
  View parent = container(200, 200);
  parent.children.push_back(box(100, 100));
  g.root.children.push_back(std::move(parent));
  View &P = g.root.children[0];
  View &C = P.children[0];
  P.onPointerDown = [&](PointerEvent &) {};
  C.onPointerDown = [&](PointerEvent &) {};
  g.attach();
  auto at = ptr::centerOf(C);

  ptr::press(g, at); // nobody stops: deepest receiver captures
  CHECK(g.r.capturedView() == &C);
  ptr::release(g, at);

  P.onPointerDown = [&](PointerEvent &e) { e.stopPropagation(); };
  ptr::press(g, at); // parent stops: parent captures
  CHECK(g.r.capturedView() == &P);
  ptr::release(g, at);
  CHECK(g.r.capturedView() == nullptr);
}

TEST_CASE("pointer: moves bubble from the capture target to its ancestors",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(300, 300);
  g.root.children.push_back(box(100, 100));
  g.root.onPointerMove = [&](PointerEvent &) { g.note("root.move"); };
  g.root.children[0].onPointerDown = [&](PointerEvent &) {};
  g.root.children[0].onPointerMove = [&](PointerEvent &) { g.note("A.move"); };
  g.attach();
  ptr::press(g, ptr::centerOf(g.root.children[0]));
  g.r.move(ptr::ev({290, 290}));
  CHECK(g.log == std::vector<std::string>{"A.move", "root.move"});
}

TEST_CASE("pointer: buttons bitmask, button field and mouse pressure",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  std::vector<PointerEvent> seen;
  g.root.onPointerDown = [&](PointerEvent &e) { seen.push_back(e); };
  g.root.onPointerMove = [&](PointerEvent &e) { seen.push_back(e); };
  g.root.onPointerUp = [&](PointerEvent &e) { seen.push_back(e); };
  g.attach();
  auto p = ptr::centerOf(g.root);

  ptr::press(g, p, MouseButton::Left);
  ptr::press(g, p, MouseButton::Right);
  g.r.move(ptr::ev(p));
  ptr::release(g, p, MouseButton::Left);
  g.r.move(ptr::ev(p));
  ptr::release(g, p, MouseButton::Right);
  g.r.move(ptr::ev(p));

  REQUIRE(seen.size() == 7);
  CHECK(seen[0].button == MouseButton::Left);
  CHECK(seen[0].buttons == (buttonBit(MouseButton::Left)));
  CHECK(seen[1].button == MouseButton::Right);
  CHECK(seen[1].buttons ==
        (buttonBit(MouseButton::Left) | buttonBit(MouseButton::Right)));
  CHECK(seen[1].held(MouseButton::Left));
  CHECK(seen[1].held(MouseButton::Right));
  CHECK_FALSE(seen[1].held(MouseButton::Middle));
  CHECK(seen[2].button == MouseButton::None); // a move changes no button
  CHECK(seen[2].buttons == 5);                // Left(1) | Right(4)
  CHECK(seen[2].pressure == approx(0.5));
  CHECK(seen[3].button == MouseButton::Left); // up: buttons is AFTER the change
  CHECK(seen[3].buttons == buttonBit(MouseButton::Right));
  CHECK(seen[5].buttons == 0);
  CHECK(seen[6].buttons == 0);
  CHECK(seen[6].pressure == approx(0.0));
  CHECK(seen[6].type == PointerType::Mouse);
}

TEST_CASE("pointer: cancel delivers onPointerCancel once and ends the press",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  A.onPointerDown = [&](PointerEvent &) {};
  A.onPointerUp = [&](PointerEvent &) { g.note("up"); };
  A.onPointerCancel = [&](PointerEvent &) { g.note("cancel"); };
  A.onClick = [&](PointerEvent &) { g.note("click"); };
  g.attach();
  auto at = ptr::centerOf(A);

  ptr::press(g, at);
  g.r.cancelAll();
  g.r.cancelAll(); // idempotent
  CHECK(g.log == std::vector<std::string>{"cancel"});
  CHECK(g.r.capturedView() == nullptr);
  CHECK(g.r.buttonsHeld() == 0);

  // The release that follows a lost capture completes nothing.
  ptr::release(g, at);
  CHECK(g.log == std::vector<std::string>{"cancel"});
}

TEST_CASE("pointer: invalidate() drops state silently (views may be gone)",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  A.onPointerDown = [&](PointerEvent &) {};
  A.onPointerCancel = [&](PointerEvent &) { g.note("cancel"); };
  A.onClick = [&](PointerEvent &) { g.note("click"); };
  g.attach();
  auto at = ptr::centerOf(A);
  ptr::press(g, at);
  g.r.invalidate();
  CHECK(g.r.capturedView() == nullptr);
  CHECK(g.r.pressedView(MouseButton::Left) == nullptr);
  ptr::release(g, at);
  CHECK(g.log.empty()); // no cancel, no click
}

TEST_CASE("pointer: a handler that invalidates mid-dispatch stops bubbling",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100));
  g.root.onPointerDown = [&](PointerEvent &) { g.note("root"); };
  g.root.children[0].onPointerDown = [&](PointerEvent &) {
    g.note("child");
    g.r.invalidate(); // e.g. the handler called setRoot()
  };
  g.attach();
  ptr::press(g, ptr::centerOf(g.root.children[0]));
  CHECK(g.log == std::vector<std::string>{"child"}); // root never touched
  CHECK(g.r.capturedView() == nullptr);
  CHECK(g.polls == 0); // nothing left to reconcile against
}

TEST_CASE("pointer: disabled and handler-less views are transparent",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100)); // has a handler but is disabled
  g.root.children.push_back(box(100, 100)); // no handlers at all
  g.root.onPointerDown = [&](PointerEvent &) { g.note("root"); };
  g.root.children[0].onPointerDown = [&](PointerEvent &) { g.note("child"); };
  g.root.children[0].disabled = true;
  g.attach();
  ptr::press(g, ptr::centerOf(g.root.children[0]));
  ptr::release(g, ptr::centerOf(g.root.children[0]));
  ptr::press(g, ptr::centerOf(g.root.children[1]));
  CHECK(g.log == std::vector<std::string>{"root", "root"});
}

TEST_CASE("pointer: an absolute view bubbles to its parent, not to what it "
          "covers",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100)); // S: covered by X
  View x = box(50, 50);
  x.style.position = Position::Absolute;
  x.style.left = 0.0f;
  x.style.top = 0.0f;
  g.root.children.push_back(std::move(x));
  g.root.onPointerDown = [&](PointerEvent &) { g.note("root"); };
  g.root.children[0].onPointerDown = [&](PointerEvent &) { g.note("S"); };
  View &X = g.root.children[1];
  X.onPointerDown = [&](PointerEvent &) { g.note("X"); };
  g.attach();
  REQUIRE(X.computed.w == approx(50));

  ptr::press(g, {X.computed.x + 10, X.computed.y + 10});
  CHECK(g.log == std::vector<std::string>{"X", "root"}); // not S
  ptr::release(g, {X.computed.x + 10, X.computed.y + 10});

  // Same point once X stops caring: the view underneath gets it.
  g.log.clear();
  X.onPointerDown = nullptr;
  ptr::press(g, {X.computed.x + 10, X.computed.y + 10});
  CHECK(g.log == std::vector<std::string>{"S", "root"});
}

TEST_CASE("click: left only, same view, bubbles unless stopped", "[pointer]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100));
  g.root.children.push_back(box(100, 100));
  g.root.onClick = [&](PointerEvent &) { g.note("root.click"); };
  g.root.children[0].onClick = [&](PointerEvent &e) {
    g.note("A.click");
    CHECK(e.clickCount == 1);
  };
  g.attach();
  auto a = ptr::centerOf(g.root.children[0]);
  auto b = ptr::centerOf(g.root.children[1]);

  ptr::press(g, a);
  ptr::release(g, a);
  CHECK(g.log == std::vector<std::string>{"A.click", "root.click"});

  // Pressed on A, released on B: no click anywhere.
  g.log.clear();
  ptr::press(g, a);
  ptr::release(g, b);
  CHECK(g.log.empty());

  // stopPropagation keeps the click off the ancestor.
  g.root.children[0].onClick = [&](PointerEvent &e) {
    g.note("A.click");
    e.stopPropagation();
  };
  ptr::press(g, a);
  ptr::release(g, a);
  CHECK(g.log == std::vector<std::string>{"A.click"});
}

TEST_CASE("click: a no-argument onClick ends bubbling",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.children.push_back(box(100, 100));
  g.root.onClick = [&](PointerEvent &) { g.note("root"); };
  g.root.children[0].onClick = [&] { g.note("short"); };
  CHECK(g.root.children[0].onClick.endsBubbling());
  CHECK_FALSE(g.root.onClick.endsBubbling());
  g.attach();
  auto p = ptr::centerOf(g.root.children[0]);
  ptr::press(g, p);
  ptr::release(g, p);
  CHECK(g.log == std::vector<std::string>{"short"});
}

TEST_CASE("click: PointerCallback accepts both forms, copies, and calls bare",
          "[pointer]") {
  int n = 0;
  PointerCallback none;
  CHECK_FALSE((bool)none);
  PointerCallback a = [&] { ++n; };
  PointerCallback b = [&](PointerEvent &) { n += 10; };
  PointerCallback c = a; // copyable, keeps its ends-bubbling flag
  a();
  b();
  c();
  CHECK(n == 12);
  CHECK(c.endsBubbling());
  none = nullptr;
  CHECK_FALSE((bool)none);
  std::function<void()> f = [&] { n += 100; };
  PointerCallback d = f;
  d();
  CHECK(n == 112);
}

TEST_CASE("click: middle is onAuxClick, right is onContextMenu, neither is "
          "onClick",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.onClick = [&](PointerEvent &) { g.note("click"); };
  g.root.onAuxClick = [&](PointerEvent &e) {
    g.note("aux");
    CHECK(e.button == MouseButton::Middle);
  };
  g.root.onContextMenu = [&](PointerEvent &e) {
    g.note("menu");
    CHECK(e.button == MouseButton::Right);
  };
  g.attach();
  auto p = ptr::centerOf(g.root);
  for (auto b : {MouseButton::Middle, MouseButton::Right, MouseButton::Left}) {
    ptr::press(g, p, b);
    ptr::release(g, p, b);
  }
  CHECK(g.log == std::vector<std::string>{"aux", "menu", "click"});
}

TEST_CASE("click: double click needs the same view, <=500 ms, <=4 px",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  uint8_t lastCount = 0;
  g.root.onClick = [&](PointerEvent &e) { lastCount = e.clickCount; };
  g.root.onDoubleClick = [&](PointerEvent &e) {
    g.note("dbl");
    CHECK(e.clickCount == 2);
  };
  g.attach();
  auto p = ptr::centerOf(g.root);
  auto click = [&](std::pair<float, float> at, uint64_t t) {
    ptr::press(g, at, MouseButton::Left, t);
    ptr::release(g, at, MouseButton::Left, t + 10);
  };

  click(p, 1000);
  CHECK(lastCount == 1);
  CHECK(g.log.empty());
  click({p.first + 3, p.second}, 1300); // close in time and space
  CHECK(lastCount == 2);
  CHECK(g.log == std::vector<std::string>{"dbl"});

  g.log.clear();
  click(p, 3000); // new sequence
  click(p, 3600); // too slow
  CHECK(lastCount == 1);
  click(p, 5000); // 1.4 s after the previous click: a new sequence
  click({p.first + 20, p.second}, 5100); // close in time, too far in space
  CHECK(lastCount == 1);
  CHECK(g.log.empty());
}

TEST_CASE("pan: dropClick cancels the click but not the capture",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.onClick = [&](PointerEvent &) { g.note("click"); };
  g.root.onPointerDown = [](PointerEvent &) {};
  g.root.onPointerMove = [&](PointerEvent &) { g.note("move"); };
  g.attach();
  auto p = ptr::centerOf(g.root);
  ptr::press(g, p);
  g.r.dropClick(MouseButton::Left); // a scroll pan claimed the gesture
  g.r.move(ptr::ev(p));
  ptr::release(g, p);
  CHECK(g.log == std::vector<std::string>{"move"}); // no click
}

TEST_CASE("wheel: onWheel bubbles; stopPropagation suppresses scrolling",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100));
  g.root.onWheel = [&](WheelEvent &e) {
    g.note("root");
    CHECK(e.dy == approx(40));
  };
  g.root.children[0].onWheel = [&](WheelEvent &) { g.note("child"); };
  g.attach();
  auto p = ptr::centerOf(g.root.children[0]);

  WheelEvent e;
  e.windowX = p.first;
  e.windowY = p.second;
  e.dy = 40;
  CHECK(g.r.wheel(e));
  CHECK(g.log == std::vector<std::string>{"child", "root"});
  CHECK_FALSE(e.propagationStopped()); // so the caller scrolls

  g.log.clear();
  g.root.children[0].onWheel = [&](WheelEvent &w) {
    g.note("child");
    w.stopPropagation();
  };
  WheelEvent e2 = e;
  CHECK(g.r.wheel(e2));
  CHECK(g.log == std::vector<std::string>{"child"});
  CHECK(e2.propagationStopped()); // caller must NOT scroll
}

TEST_CASE("hover: onPointerEnter/Leave follow the hover flag and don't bubble",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[0].onPointerEnter = [&](PointerEvent &) { g.note("A.in"); };
  g.root.children[0].onPointerLeave = [&](PointerEvent &) { g.note("A.out"); };
  g.root.children[1].onPointerEnter = [&](PointerEvent &) { g.note("B.in"); };
  g.root.children[1].onPointerLeave = [&](PointerEvent &) { g.note("B.out"); };
  g.attach();
  bool needPoll = false;

  CHECK(g.r.hover(ptr::ev(ptr::centerOf(g.root.children[0])), &needPoll));
  CHECK(needPoll); // a boundary handler may have changed app state
  needPoll = false;
  CHECK_FALSE(g.r.hover(ptr::ev(ptr::centerOf(g.root.children[0])), &needPoll));
  CHECK_FALSE(needPoll); // no flip, nothing fired

  CHECK(g.r.hover(ptr::ev(ptr::centerOf(g.root.children[1])), &needPoll));
  CHECK(g.log == std::vector<std::string>{"A.in", "A.out", "B.in"});
}

TEST_CASE("move: hover moves reach onPointerMove with buttons == 0",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  uint8_t buttons = 255;
  g.root.children[1].onPointerMove = [&](PointerEvent &e) {
    buttons = e.buttons;
    g.note("B.move");
  };
  g.attach();
  g.r.move(ptr::ev(ptr::centerOf(g.root.children[0])));
  CHECK(g.log.empty());
  g.r.move(ptr::ev(ptr::centerOf(g.root.children[1])));
  CHECK(g.log == std::vector<std::string>{"B.move"});
  CHECK(buttons == 0);
}

TEST_CASE("focus: a left press focuses the target, otherwise clears focus",
          "[pointer]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[0].focusable = true;
  g.root.children[0].onPointerDown = [](PointerEvent &) {};
  g.root.children[1].onPointerDown = [](PointerEvent &) {};
  g.attach();
  std::vector<View *> focused;
  g.r.focus = [&](View *v) { focused.push_back(v); };
  ptr::press(g, ptr::centerOf(g.root.children[0]));
  ptr::release(g, ptr::centerOf(g.root.children[0]));
  ptr::press(g, ptr::centerOf(g.root.children[1]));
  ptr::release(g, ptr::centerOf(g.root.children[1]));
  ptr::press(g, ptr::centerOf(g.root.children[0]), MouseButton::Right);
  REQUIRE(focused.size() == 2); // right button never moves focus
  CHECK(focused[0] == &g.root.children[0]);
  CHECK(focused[1] == nullptr);
}

TEST_CASE("canvas: every pointer handler survives Canvas -> View", "[pointer]") {
  Canvas c;
  int hits = 0;
  c.onPointerDown = [&](PointerEvent &) { ++hits; };
  c.onPointerMove = [&](PointerEvent &) { ++hits; };
  c.onPointerUp = [&](PointerEvent &) { ++hits; };
  c.onPointerCancel = [&](PointerEvent &) { ++hits; };
  c.onPointerEnter = [&](PointerEvent &) { ++hits; };
  c.onPointerLeave = [&](PointerEvent &) { ++hits; };
  c.onDoubleClick = [&](PointerEvent &) { ++hits; };
  c.onAuxClick = [&](PointerEvent &) { ++hits; };
  c.onContextMenu = [&](PointerEvent &) { ++hits; };
  c.onWheel = [&](WheelEvent &) { ++hits; };
  c.onClick = [&] { ++hits; }; // the short, no-argument form
  View parent;
  parent.addChild(std::move(c));
  View &v = parent.children.back();
  CHECK(v.isCanvas);
  CHECK((bool)v.onPointerDown);
  CHECK((bool)v.onPointerMove);
  CHECK((bool)v.onPointerUp);
  CHECK((bool)v.onPointerCancel);
  CHECK((bool)v.onPointerEnter);
  CHECK((bool)v.onPointerLeave);
  CHECK((bool)v.onDoubleClick);
  CHECK((bool)v.onAuxClick);
  CHECK((bool)v.onContextMenu);
  CHECK((bool)v.onWheel);
  REQUIRE((bool)v.onClick);
  CHECK(v.onClick.endsBubbling());
  v.onClick(); // a bare call still works
  CHECK(hits == 1);
}

TEST_CASE("click: onClick can be assigned, replaced and cleared in either form",
          "[pointer]") {
  View v;
  int n = 0;
  v.onClick = [&] { ++n; };
  std::function<void()> f = [&] { ++n; };
  v.onClick = f;
  CHECK((bool)v.onClick);
  v.onClick();
  CHECK(n == 1);
  v.onClick = [&](PointerEvent &) { n += 10; };
  v.onClick();
  CHECK(n == 11);
  v.onClick = nullptr;
  CHECK_FALSE((bool)v.onClick);
  // MouseButton's numeric values are unchanged; None sits after them.
  CHECK(static_cast<int>(MouseButton::Left) == 0);
  CHECK(static_cast<int>(MouseButton::Middle) == 1);
  CHECK(static_cast<int>(MouseButton::Right) == 2);
  CHECK(buttonBit(MouseButton::None) == 0);
}

// ---------------------------------------------------------------------------
// Style::cursor — Router::cursorAt()
// ---------------------------------------------------------------------------

TEST_CASE("cursor: Default when nothing sets one, and outside every view",
          "[pointer][cursor]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.attach();
  CHECK(g.r.cursorAt(ptr::centerOf(g.root.children[0]).first,
                     ptr::centerOf(g.root.children[0]).second) ==
        Cursor::Default);
  CHECK(g.r.cursorAt(-50, -50) == Cursor::Default);
  liteui_pointer::Router detached; // no tree at all
  CHECK(detached.cursorAt(0, 0) == Cursor::Default);
}

TEST_CASE("cursor: inherited from the nearest ancestor that sets one",
          "[pointer][cursor]") {
  ptr::Rig g;
  g.root = container(300, 300);
  View parent = container(200, 200);
  parent.children.push_back(box(100, 100)); // inherits
  parent.children.push_back(box(50, 50));    // overrides
  g.root.children.push_back(std::move(parent));
  g.root.style.cursor = Cursor::Crosshair;
  g.root.children[0].style.cursor = Cursor::Pointer;
  g.root.children[0].children[1].style.cursor = Cursor::Text;
  g.attach();
  const View &P = g.root.children[0];
  auto at = [&](const View &v) {
    auto c = ptr::centerOf(v);
    return g.r.cursorAt(c.first, c.second);
  };
  CHECK(at(P.children[0]) == Cursor::Pointer); // from the parent
  CHECK(at(P.children[1]) == Cursor::Text);    // its own, deepest wins
  CHECK(at(P) == at(P.children[0]));           // (centre of P is in child 0)
  // A point in root but outside the parent: the root's.
  CHECK(g.r.cursorAt(g.root.computed.x + 280, g.root.computed.y + 280) ==
        Cursor::Crosshair);
}

TEST_CASE("cursor: views with no handlers still count", "[pointer][cursor]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[1].style.cursor = Cursor::Help; // no handler on it at all
  g.attach();
  auto c = ptr::centerOf(g.root.children[1]);
  CHECK(g.r.cursorAt(c.first, c.second) == Cursor::Help);
}

TEST_CASE("cursor: an absolute view's cursor beats what it covers",
          "[pointer][cursor]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100));
  View x = box(50, 50);
  x.style.position = Position::Absolute;
  x.style.left = 0.0f;
  x.style.top = 0.0f;
  x.style.cursor = Cursor::Move;
  g.root.children.push_back(std::move(x));
  g.root.children[0].style.cursor = Cursor::Text;
  g.attach();
  const View &X = g.root.children[1];
  CHECK(g.r.cursorAt(X.computed.x + 10, X.computed.y + 10) == Cursor::Move);
  // Outside X but still over the covered view:
  CHECK(g.r.cursorAt(X.computed.x + 70, X.computed.y + 10) == Cursor::Text);
}

TEST_CASE("cursor: capture pins the target's cursor wherever the pointer goes",
          "[pointer][cursor]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[0].style.cursor = Cursor::Grabbing;
  g.root.children[1].style.cursor = Cursor::Pointer;
  g.root.children[0].onPointerDown = [](PointerEvent &) {};
  g.attach();
  const View &A = g.root.children[0];
  const View &B = g.root.children[1];
  auto b = ptr::centerOf(B);

  CHECK(g.r.cursorAt(b.first, b.second) == Cursor::Pointer);
  ptr::press(g, ptr::centerOf(A));
  CHECK(g.r.cursorAt(b.first, b.second) == Cursor::Grabbing); // over B, pinned
  CHECK(g.r.cursorAt(-500, -500) == Cursor::Grabbing);        // off the window
  ptr::release(g, b);
  CHECK(g.r.cursorAt(b.first, b.second) == Cursor::Pointer); // released
}

TEST_CASE("cursor: an inheriting capture target pins its ancestor's cursor",
          "[pointer][cursor]") {
  ptr::Rig g;
  g.root = container(300, 100);
  g.root.children.push_back(box(100, 100));
  g.root.children.push_back(box(100, 100));
  g.root.style.cursor = Cursor::Crosshair;
  g.root.children[1].style.cursor = Cursor::Text;
  g.root.children[0].onPointerDown = [](PointerEvent &) {};
  g.attach();
  auto b = ptr::centerOf(g.root.children[1]);
  ptr::press(g, ptr::centerOf(g.root.children[0]));
  CHECK(g.r.cursorAt(b.first, b.second) == Cursor::Crosshair);
}

TEST_CASE("cursor: callback values come from the polled cache, None hides",
          "[pointer][cursor]") {
  ptr::Rig g;
  g.root = container(100, 100);
  Cursor wanted = Cursor::Crosshair;
  g.root.style.cursor = [&] { return wanted; };
  g.attach();
  auto p = ptr::centerOf(g.root);
  // Before the first poll the cache still holds its initial Inherit, which
  // resolves to the default; the owner's checkForUpdates fills it in.
  CHECK(g.r.cursorAt(p.first, p.second) == Cursor::Default);
  g.root.computed.resolvedCursor = wanted;
  CHECK(g.r.cursorAt(p.first, p.second) == Cursor::Crosshair);
  g.root.style.cursor = Cursor::None;
  CHECK(g.r.cursorAt(p.first, p.second) == Cursor::None);
}

// ---------------------------------------------------------------------------
// Pen and touch — several pointers at once, each with its own state
// ---------------------------------------------------------------------------

namespace ptr {

inline constexpr uint32_t kFingerA = 0x1000, kFingerB = 0x1001, kPen = 0x2000;

inline PointerEvent pev(uint32_t id, PointerType type, std::pair<float, float> p,
                        MouseButton b = MouseButton::None, uint64_t t = 1000) {
  PointerEvent e = ev(p, b, t);
  e.pointerId = id;
  e.type = type;
  e.isPrimary = id != kFingerB;
  e.pressure = type == PointerType::Mouse ? 0.5f : 0.7f;
  return e;
}
inline PointerEvent finger(uint32_t id, std::pair<float, float> p,
                           MouseButton b = MouseButton::None,
                           uint64_t t = 1000) {
  return pev(id, PointerType::Touch, p, b, t);
}
inline PointerEvent pen(std::pair<float, float> p,
                        MouseButton b = MouseButton::None, uint64_t t = 1000) {
  return pev(kPen, PointerType::Pen, p, b, t);
}

} // namespace ptr

TEST_CASE("touch: two fingers are independent pointers with their own capture",
          "[pointer][touch]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  View &B = g.root.children[1];
  for (View *v : {&A, &B}) {
    const std::string n = v == &A ? "A" : "B";
    v->onPointerDown = [&g, n](PointerEvent &e) {
      g.note(n + ".down#" + std::to_string(e.pointerId));
    };
    v->onPointerMove = [&g, n](PointerEvent &e) {
      g.note(n + ".move#" + std::to_string(e.pointerId));
    };
    v->onPointerUp = [&g, n](PointerEvent &e) {
      g.note(n + ".up#" + std::to_string(e.pointerId));
    };
  }
  g.attach();
  const auto a = ptr::centerOf(A), b = ptr::centerOf(B);

  g.r.down(ptr::finger(ptr::kFingerA, a, MouseButton::Left));
  g.r.down(ptr::finger(ptr::kFingerB, b, MouseButton::Left));
  CHECK(g.r.pointerCount() == 3); // mouse + two fingers
  CHECK(g.r.capturedView(ptr::kFingerA) == &A);
  CHECK(g.r.capturedView(ptr::kFingerB) == &B);

  // Each finger's moves go to its own capture, wherever the finger is.
  g.log.clear();
  g.r.move(ptr::finger(ptr::kFingerA, b)); // A's finger wanders over B
  g.r.move(ptr::finger(ptr::kFingerB, a)); // and B's over A
  CHECK(g.log == std::vector<std::string>{"A.move#4096", "B.move#4097"});

  g.log.clear();
  g.r.up(ptr::finger(ptr::kFingerA, b, MouseButton::Left));
  CHECK(g.log == std::vector<std::string>{"A.up#4096"});
  CHECK(g.r.capturedView(ptr::kFingerB) == &B); // B's finger still holds it
  g.r.up(ptr::finger(ptr::kFingerB, a, MouseButton::Left));
  CHECK(g.r.pointerCount() == 1); // lifted fingers cease to exist
}

TEST_CASE("touch: a finger does not disturb the mouse", "[pointer][touch]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  A.onPointerDown = [](PointerEvent &) {};
  g.attach();
  const auto a = ptr::centerOf(A);
  ptr::press(g, a, MouseButton::Left);
  g.r.down(ptr::finger(ptr::kFingerA, a, MouseButton::Left));
  CHECK(g.r.buttonsHeld() == buttonBit(MouseButton::Left)); // the mouse's own
  g.r.cancelPointer(ptr::kFingerA);
  CHECK(g.r.capturedView() == &A); // mouse capture survives
  CHECK(g.r.buttonsHeld() == buttonBit(MouseButton::Left));
  ptr::release(g, a, MouseButton::Left);
  CHECK(g.r.buttonsHeld() == 0);
}

TEST_CASE("touch: cancelPointer cancels one finger only and forgets it",
          "[pointer][touch]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  View &B = g.root.children[1];
  A.onPointerDown = [](PointerEvent &) {};
  A.onPointerCancel = [&](PointerEvent &e) {
    g.note("A.cancel#" + std::to_string(e.pointerId));
    CHECK(e.type == PointerType::Touch);
  };
  B.onPointerDown = [](PointerEvent &) {};
  B.onPointerCancel = [&](PointerEvent &) { g.note("B.cancel"); };
  B.onClick = [&](PointerEvent &) { g.note("B.click"); };
  g.attach();
  g.r.down(ptr::finger(ptr::kFingerA, ptr::centerOf(A), MouseButton::Left));
  g.r.down(ptr::finger(ptr::kFingerB, ptr::centerOf(B), MouseButton::Left));
  g.r.cancelPointer(ptr::kFingerA); // e.g. the compositor took finger A
  CHECK(g.log == std::vector<std::string>{"A.cancel#4096"});
  CHECK_FALSE(g.r.knows(ptr::kFingerA));
  // The other finger carries on to a normal click.
  g.r.up(ptr::finger(ptr::kFingerB, ptr::centerOf(B), MouseButton::Left));
  CHECK(g.log == std::vector<std::string>{"A.cancel#4096", "B.click"});
  // A release for the cancelled finger completes nothing.
  g.r.up(ptr::finger(ptr::kFingerA, ptr::centerOf(A), MouseButton::Left));
  CHECK(g.log.size() == 2);
}

TEST_CASE("touch: double-tap works across fingers, and types keep separate histories",
          "[pointer][touch]") {
  ptr::Rig g;
  g.root = container(100, 100);
  std::vector<int> counts;
  g.root.onClick = [&](PointerEvent &e) { counts.push_back(e.clickCount); };
  g.root.onDoubleClick = [&](PointerEvent &e) {
    g.note("dbl#" + std::to_string(e.pointerId));
  };
  g.attach();
  const auto p = ptr::centerOf(g.root);
  auto tap = [&](uint64_t t) {
    g.r.down(ptr::finger(ptr::kFingerA, p, MouseButton::Left, t));
    g.r.up(ptr::finger(ptr::kFingerA, p, MouseButton::Left, t + 20));
  };
  tap(1000);
  // A mouse click in between must not reset or join the finger's sequence.
  ptr::press(g, p, MouseButton::Left, 1100);
  ptr::release(g, p, MouseButton::Left, 1120);
  tap(1300);
  CHECK(counts == std::vector<int>{1, 1, 2});
  CHECK(g.log == std::vector<std::string>{"dbl#4096"});
}

TEST_CASE("pen: tip contact is the Left button, so down/up/click work for a "
          "pen with no extra code",
          "[pointer][pen]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.onPointerDown = [&](PointerEvent &e) {
    g.note("down");
    CHECK(e.button == MouseButton::Left);
    CHECK(e.type == PointerType::Pen);
  };
  g.root.onPointerMove = [&](PointerEvent &) { g.note("move"); };
  g.root.onPointerUp = [&](PointerEvent &) { g.note("up"); };
  g.root.onClick = [&](PointerEvent &) { g.note("click"); };
  g.attach();
  const auto p = ptr::centerOf(g.root);
  g.r.down(ptr::pen(p, MouseButton::Left));
  g.r.move(ptr::pen({p.first + 5, p.second}));
  g.r.up(ptr::pen(p, MouseButton::Left));
  CHECK(g.log == std::vector<std::string>{"down", "move", "up", "click"});
}

TEST_CASE("pen: pressure, tilt, twist, eraser and type reach the handler",
          "[pointer][pen]") {
  ptr::Rig g;
  g.root = container(100, 100);
  PointerEvent seen;
  g.root.onPointerMove = [&](PointerEvent &e) { seen = e; };
  g.attach();
  PointerEvent e = ptr::pen(ptr::centerOf(g.root));
  e.pressure = 0.25f;
  e.tiltX = -30;
  e.tiltY = 45;
  e.twist = 270;
  e.eraser = true;
  g.r.move(e);
  CHECK(seen.type == PointerType::Pen);
  CHECK(seen.pointerId == ptr::kPen);
  CHECK(seen.pressure == approx(0.25)); // a pen's own value is not overridden
  CHECK(seen.tiltX == approx(-30));
  CHECK(seen.tiltY == approx(45));
  CHECK(seen.twist == approx(270));
  CHECK(seen.eraser);
}

TEST_CASE("pen: barrel button is Right; it persists until forget()",
          "[pointer][pen]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.onContextMenu = [&](PointerEvent &e) {
    g.note("menu");
    CHECK(e.type == PointerType::Pen);
  };
  g.attach();
  const auto p = ptr::centerOf(g.root);
  g.r.move(ptr::pen(p)); // hovering creates the pen's state
  CHECK(g.r.knows(ptr::kPen));
  g.r.down(ptr::pen(p, MouseButton::Right));
  g.r.up(ptr::pen(p, MouseButton::Right));
  CHECK(g.log == std::vector<std::string>{"menu"});
  CHECK(g.r.knows(ptr::kPen)); // a pen is not transient: it is still in range
  g.r.forget(ptr::kPen);
  CHECK_FALSE(g.r.knows(ptr::kPen));
  CHECK(g.r.pointerCount() == 1);
}

TEST_CASE("pen: forget() does not drop a pointer that still holds a button",
          "[pointer][pen]") {
  ptr::Rig g;
  g.root = container(100, 100);
  g.root.onPointerDown = [](PointerEvent &) {};
  g.attach();
  g.r.down(ptr::pen(ptr::centerOf(g.root), MouseButton::Left));
  g.r.forget(ptr::kPen); // a press in flight must be cancelPointer()'d instead
  CHECK(g.r.knows(ptr::kPen));
  g.r.cancelPointer(ptr::kPen);
  CHECK_FALSE(g.r.knows(ptr::kPen));
}

TEST_CASE("pen: hovering fires enter/leave like a mouse", "[pointer][pen]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[0].onPointerEnter = [&](PointerEvent &e) {
    g.note("A.in");
    CHECK(e.type == PointerType::Pen);
    CHECK(e.buttons == 0);
  };
  g.root.children[0].onPointerLeave = [&](PointerEvent &) { g.note("A.out"); };
  g.attach();
  bool needPoll = false;
  g.r.hover(ptr::pen(ptr::centerOf(g.root.children[0])), &needPoll);
  g.r.hover(ptr::pen({-100, -100}), &needPoll);
  CHECK(g.log == std::vector<std::string>{"A.in", "A.out"});
}

TEST_CASE("coalesced: a move carries every sample, ending with its own",
          "[pointer][pen]") {
  ptr::Rig g;
  g.root = container(200, 200);
  g.root.children.push_back(box(100, 100));
  View &C = g.root.children[0];
  std::vector<PointerSample> got;
  PointerEvent last;
  C.onPointerDown = [](PointerEvent &) {};
  C.onPointerMove = [&](PointerEvent &e) {
    last = e;
    got.assign(e.coalesced.begin(), e.coalesced.end());
  };
  g.attach();
  const auto p = ptr::centerOf(C);
  g.r.down(ptr::pen(p, MouseButton::Left));

  const PointerSample s[] = {{p.first + 1, p.second, 0.1f, 0, 0, 0, 1001},
                             {p.first + 2, p.second, 0.2f, 0, 0, 0, 1002},
                             {p.first + 3, p.second, 0.3f, 0, 0, 0, 1003}};
  PointerEvent m = ptr::pen({p.first + 3, p.second});
  m.coalesced = s;
  g.r.move(m);
  REQUIRE(got.size() == 3);
  CHECK(got[0].x == approx(p.first + 1));
  CHECK(got[2].pressure == approx(0.3));
  CHECK(got.back().x == approx(last.windowX));
  // In the view's own space:
  CHECK(last.toLocal(got[0]).x == approx(p.first + 1 - C.computed.x));
}

TEST_CASE("coalesced: a move without a batch still has one sample (itself)",
          "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  size_t n = 0;
  float x = -1;
  g.root.onPointerMove = [&](PointerEvent &e) {
    n = e.coalesced.size();
    x = n ? e.coalesced[0].x : -1;
  };
  g.attach();
  g.r.move(ptr::ev({30, 40}));
  CHECK(n == 1);
  CHECK(x == approx(30));
}

TEST_CASE("coalesced: down and up carry none", "[pointer]") {
  ptr::Rig g;
  g.root = container(100, 100);
  size_t n = 99;
  g.root.onPointerDown = [&](PointerEvent &e) { n = e.coalesced.size(); };
  g.root.onPointerUp = [&](PointerEvent &e) { n += e.coalesced.size(); };
  g.attach();
  const PointerSample s{1, 2, 0, 0, 0, 0, 1};
  PointerEvent d = ptr::ev(ptr::centerOf(g.root), MouseButton::Left);
  d.coalesced = std::span<const PointerSample>(&s, 1); // caller's mistake
  g.r.down(d);
  CHECK(n == 0);
  g.r.up(ptr::ev(ptr::centerOf(g.root), MouseButton::Left));
  CHECK(n == 0);
}

TEST_CASE("cursor: only the mouse's capture pins the cursor", "[pointer][cursor]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  g.root.children[0].style.cursor = Cursor::Grabbing;
  g.root.children[1].style.cursor = Cursor::Pointer;
  g.root.children[0].onPointerDown = [](PointerEvent &) {};
  g.attach();
  const auto b = ptr::centerOf(g.root.children[1]);
  g.r.down(ptr::finger(ptr::kFingerA, ptr::centerOf(g.root.children[0]),
                       MouseButton::Left)); // a finger captured A
  CHECK(g.r.cursorAt(b.first, b.second) == Cursor::Pointer);
}

TEST_CASE("invalidate: pointers stay known but forget their targets",
          "[pointer][touch]") {
  ptr::Rig g;
  ptr::twoBoxes(g);
  View &A = g.root.children[0];
  A.onPointerDown = [](PointerEvent &) {};
  A.onPointerUp = [&](PointerEvent &) { g.note("up"); };
  A.onPointerCancel = [&](PointerEvent &) { g.note("cancel"); };
  g.attach();
  g.r.down(ptr::finger(ptr::kFingerA, ptr::centerOf(A), MouseButton::Left));
  g.r.invalidate();
  CHECK(g.r.capturedView(ptr::kFingerA) == nullptr);
  CHECK(g.r.knows(ptr::kFingerA)); // its finger is still on the glass
  g.r.up(ptr::finger(ptr::kFingerA, ptr::centerOf(A), MouseButton::Left));
  CHECK(g.log.empty());           // nothing delivered to dead views
  CHECK_FALSE(g.r.knows(ptr::kFingerA)); // but the lifted finger is gone
}

TEST_CASE("pointer ids: defaults are the mouse, primary", "[pointer]") {
  PointerEvent e;
  CHECK(e.pointerId == liteui_pointer::kMousePointerId);
  CHECK(e.isPrimary);
  CHECK(e.coalesced.empty());
  CHECK(e.type == PointerType::Mouse);
}

TEST_CASE("text input: it handles onPointerDown and ignores non-left buttons",
          "[pointer]") {
  // (A left press measures text through the platform font stack, which a unit
  // test avoids; the caret placement itself is checked by hand.)
  View parent;
  parent.addChild(TextInput{});
  View &field = parent.children.back();
  REQUIRE((bool)field.onPointerDown);
  PointerEvent right;
  right.button = MouseButton::Right;
  field.onPointerDown(right);
  CHECK_FALSE(right.propagationStopped()); // e.g. a parent's context menu
}
