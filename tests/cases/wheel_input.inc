// Scroll normalization (liteui_wheel): how raw wheel / touchpad input becomes
// the WheelEvent deltas apps see. The Wayland backend accumulates one
// wl_pointer.frame worth of axis events into liteui_wheel::Axis records and
// calls normalize(); these tests feed it the same shapes of input real
// compositors send. liteui_wheel has no window or platform dependency, so
// they run on Windows and Linux alike.
// (Included from liteui_tests.cpp — not a standalone .cpp.)

namespace wheel_test {

// Short alias; every liteui_wheel name below is qualified with it so this
// file can't collide with other cases in the shared test translation unit
// (canvas_hardware.inc, for one, already defines its own `Result`).
namespace lw = liteui_wheel;

// wl_pointer.axis: continuous value in surface-local pixels.
inline lw::Axis withValue(lw::Axis a, double v) {
  a.value += v;
  a.hasValue = true;
  return a;
}
// wl_pointer.axis_value120 (Wayland seat v8): 120 = one notch.
inline lw::Axis withV120(lw::Axis a, int32_t v) {
  a.v120 += v;
  a.hasV120 = true;
  return a;
}
// wl_pointer.axis_discrete (Wayland seat v5-v7): whole notches.
inline lw::Axis withDiscrete(lw::Axis a, int32_t n) {
  a.discrete += n;
  a.hasDiscrete = true;
  return a;
}

} // namespace wheel_test

TEST_CASE("wheel: one notch is 40px, the same step Win32 uses",
          "[wheel][normalize]") {
  using namespace wheel_test;
  CHECK(lw::kNotchPx == 40.0f);

  // A v8 compositor: value120 plus the (compositor-specific, here 10px/notch)
  // continuous value that is sent alongside it and must NOT be double counted.
  lw::Axis v = withValue(withV120({}, 120), 10.0);
  lw::Result r = lw::normalize(lw::Source::Wheel, v, {});
  CHECK(r.dy == approx(40));
  CHECK(r.dx == approx(0));
  CHECK_FALSE(r.precise); // a whole notch, so apps may treat it as one click
}

TEST_CASE("wheel: value120 scales linearly and keeps its sign",
          "[wheel][normalize]") {
  using namespace wheel_test;
  CHECK(lw::normalize(lw::Source::Wheel, withV120({}, 240), {}).dy == approx(80));
  CHECK(lw::normalize(lw::Source::Wheel, withV120({}, -120), {}).dy == approx(-40));
  // Several value120 events in one frame accumulate.
  lw::Axis v = withV120(withV120({}, 120), 120);
  lw::Result r = lw::normalize(lw::Source::Wheel, v, {});
  CHECK(r.dy == approx(80));
  CHECK_FALSE(r.precise); // still whole notches
}

TEST_CASE("wheel: a fractional notch (hi-res wheel) is precise",
          "[wheel][normalize]") {
  using namespace wheel_test;
  lw::Result r = lw::normalize(lw::Source::Wheel, withV120({}, 30), {});
  CHECK(r.dy == approx(10)); // 30/120 of a notch
  CHECK(r.precise);          // same rule as Win32's fractional-notch check

  // Fractions that add up to a whole notch within one frame are whole again.
  lw::Axis v = withV120(withV120({}, 60), 60);
  lw::Result whole = lw::normalize(lw::Source::Wheel, v, {});
  CHECK(whole.dy == approx(40));
  CHECK_FALSE(whole.precise);
}

TEST_CASE("wheel: axis_discrete (seat v5-v7) counts whole notches",
          "[wheel][normalize]") {
  using namespace wheel_test;
  // The continuous value (here 20px) is ignored in favour of the notch count.
  lw::Axis v = withValue(withDiscrete({}, -2), -20.0);
  lw::Result r = lw::normalize(lw::Source::Wheel, v, {});
  CHECK(r.dy == approx(-80));
  CHECK_FALSE(r.precise);
}

TEST_CASE("wheel: value120 wins over discrete when both are present",
          "[wheel][normalize]") {
  using namespace wheel_test;
  lw::Axis v = withDiscrete(withV120({}, 120), 1);
  CHECK(lw::normalize(lw::Source::Wheel, v, {}).dy == approx(40)); // not 80
}

TEST_CASE("wheel: wheel with only a continuous value falls back to it",
          "[wheel][normalize]") {
  using namespace wheel_test;
  // A seat-v5 compositor that reports the wheel source but no discrete count.
  lw::Result r = lw::normalize(lw::Source::Wheel, withValue({}, 15.0), {});
  CHECK(r.dy == approx(15));
  CHECK_FALSE(r.precise);
}

TEST_CASE("wheel: touchpad (finger) pixels pass through and are precise",
          "[wheel][normalize]") {
  using namespace wheel_test;
  lw::Result r = lw::normalize(lw::Source::Finger, withValue({}, 3.5), {});
  CHECK(r.dy == approx(3.5));
  CHECK(r.precise);

  // The failure this guards against: many tiny events must stay tiny. Ten
  // touchpad events of 1px are 10px in total, not ten "notches" of 40px.
  float total = 0;
  for (int i = 0; i < 10; ++i) {
    lw::Result step = lw::normalize(lw::Source::Finger, withValue({}, 1.0), {});
    CHECK(step.precise);
    total += step.dy;
  }
  CHECK(total == approx(10));
}

TEST_CASE("wheel: both axes of a diagonal gesture land in one result",
          "[wheel][normalize]") {
  using namespace wheel_test;
  // Vertical is the first argument, horizontal the second; the result is one
  // WheelEvent carrying both deltas.
  lw::Result r = lw::normalize(lw::Source::Finger, withValue({}, 5.0), withValue({}, -3.0));
  CHECK(r.dy == approx(5));
  CHECK(r.dx == approx(-3));
  CHECK(r.precise);

  // Horizontal-only wheel tilt: 1 notch right.
  lw::Result h = lw::normalize(lw::Source::Wheel, {}, withV120({}, 120));
  CHECK(h.dx == approx(40));
  CHECK(h.dy == approx(0));
}

TEST_CASE("wheel: an unknown source (compositor older than seat v5)",
          "[wheel][normalize]") {
  using namespace wheel_test;
  // No axis_source and no frame: the raw value is all there is.
  lw::Result raw = lw::normalize(lw::Source::Unknown, withValue({}, 10.0), {});
  CHECK(raw.dy == approx(10));
  CHECK_FALSE(raw.precise);

  // A notch or value120 count can only come from a wheel, even if the source
  // event was never sent.
  lw::Result notch = lw::normalize(lw::Source::Unknown, withV120({}, 120), {});
  CHECK(notch.dy == approx(40));
  CHECK_FALSE(notch.precise);
  CHECK(lw::normalize(lw::Source::Unknown, withDiscrete({}, 1), {}).dy == approx(40));
}

TEST_CASE("wheel: a frame with no movement is empty and must not dispatch",
          "[wheel][normalize]") {
  using namespace wheel_test;
  // Nothing at all, and the axis_stop shape: a finger frame whose value is 0.
  CHECK(lw::normalize(lw::Source::Finger, {}, {}).empty());
  CHECK(lw::normalize(lw::Source::Finger, withValue({}, 0.0), {}).empty());
  CHECK_FALSE(lw::normalize(lw::Source::Finger, withValue({}, 0.1), {}).empty());
  // Opposite wheel steps in one frame cancel out.
  CHECK(lw::normalize(lw::Source::Wheel, withV120(withV120({}, 120), -120), {}).empty());
}

TEST_CASE("wheel: precise reaches onWheel handlers", "[wheel][pointer]") {
  using namespace wheel_test;
  // End to end through the router: what a zoom-on-wheel app sees. A mouse
  // notch arrives with precise == false (one zoom step), touchpad scrolling
  // with precise == true (scale smoothly by the pixel delta).
  ptr::Rig g;
  g.root = container(200, 200);
  bool sawPrecise = false;
  float sawDy = 0;
  g.root.onWheel = [&](WheelEvent &e) {
    sawPrecise = e.precise;
    sawDy = e.dy;
  };
  g.attach();
  auto p = ptr::centerOf(g.root);

  auto send = [&](const lw::Result &r) {
    WheelEvent e;
    e.windowX = p.first;
    e.windowY = p.second;
    e.dx = r.dx;
    e.dy = r.dy;
    e.precise = r.precise;
    g.r.wheel(e);
  };

  send(lw::normalize(lw::Source::Wheel, withV120({}, 120), {}));
  CHECK_FALSE(sawPrecise);
  CHECK(sawDy == approx(40));

  send(lw::normalize(lw::Source::Finger, withValue({}, 2.0), {}));
  CHECK(sawPrecise);
  CHECK(sawDy == approx(2));
}
