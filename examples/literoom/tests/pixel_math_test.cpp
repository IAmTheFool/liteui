// Build: g++ -std=c++20 -I.. pixel_math_test.cpp -o pixel_math_test && ./pixel_math_test
#include "pixel_math.hpp"
#include "check.hpp"

static void testSmoothStep()
{
  CHECK(smoothStep01(0.2f, 0.8f, -1.0f) == 0.0f); // below: 0
  CHECK(smoothStep01(0.2f, 0.8f, 0.2f) == 0.0f);
  CHECK(smoothStep01(0.2f, 0.8f, 0.8f) == 1.0f);
  CHECK(smoothStep01(0.2f, 0.8f, 5.0f) == 1.0f); // above: 1
  CHECK_NEAR(smoothStep01(0.0f, 1.0f, 0.5f), 0.5f, 1e-6);
  // Monotone non-decreasing, and symmetric: s(x) + s(1 - x) == 1.
  float prev = 0.0f;
  for (int i = 0; i <= 1000; ++i)
  {
    const float x = static_cast<float>(i) / 1000.0f;
    const float s = smoothStep01(0.0f, 1.0f, x);
    CHECK(s >= prev);
    CHECK_NEAR(s + smoothStep01(0.0f, 1.0f, 1.0f - x), 1.0f, 1e-5);
    prev = s;
  }
  // Flat at both ends (that is what "smooth" means here): much slower than linear.
  CHECK(smoothStep01(0.0f, 1.0f, 0.05f) < 0.01f);
  CHECK(smoothStep01(0.0f, 1.0f, 0.95f) > 0.99f);
  // Works on any interval, not just [0, 1].
  CHECK_NEAR(smoothStep01(10.0f, 30.0f, 20.0f), 0.5f, 1e-6);
}

static void testToU8()
{
  for (int i = 0; i <= 255; ++i)
    CHECK(toU8Clamped(static_cast<float>(i)) == i); // exact on whole levels
  CHECK(toU8Clamped(-5.0f) == 0);
  CHECK(toU8Clamped(-0.4f) == 0);
  CHECK(toU8Clamped(255.4f) == 255);
  CHECK(toU8Clamped(1e9f) == 255);
  CHECK(toU8Clamped(127.4f) == 127); // rounds to nearest...
  CHECK(toU8Clamped(127.5f) == 128); // ...halves go up
  CHECK(toU8Clamped(254.5f) == 255);
}

static void testDegToRad()
{
  CHECK_NEAR(180.0 * kDegToRad, 3.14159265358979323846, 1e-12);
  CHECK_NEAR(90.0 * kDegToRad, 1.57079632679489661923, 1e-12);
}

int main()
{
  testSmoothStep();
  testToU8();
  testDegToRad();
  return finish();
}
