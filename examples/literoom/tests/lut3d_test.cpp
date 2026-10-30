// Build: g++ -std=c++20 -I../src -I. lut3d_test.cpp -o lut3d_test && ./lut3d_test
#include "color/lut3d.hpp"
#include "check.hpp"

#include <cstdint>

// Deterministic pseudo-random floats in [0, 1).
struct Rng
{
  uint32_t s = 12345u;
  float next()
  {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>((s >> 8) & 0xFFFF) / 65536.0f;
  }
};

// A LUT holding f(r, g, b) (inputs / outputs in 0..255 units) at every grid point.
template <class F>
static Lut3D tableOf(int n, F f)
{
  Lut3D L = identityLut(n);
  const float k = 255.0f / static_cast<float>(n - 1);
  size_t i = 0;
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
      {
        float o[3];
        f(static_cast<float>(r) * k, static_cast<float>(g) * k, static_cast<float>(b) * k, o);
        L.rgb[i++] = o[0];
        L.rgb[i++] = o[1];
        L.rgb[i++] = o[2];
      }
  return L;
}

static void testNames()
{
  CHECK(sanitizeLutName("  Warm Film  ") == "Warm Film");
  CHECK(sanitizeLutName("caf\xC3\xA9 x") == "caf x");
  CHECK(sanitizeLutName("a/b\\c:d*e?f") == "a_b_c_d_e_f");
  CHECK(sanitizeLutName("name. .") == "name");
  CHECK(sanitizeLutName("   ").empty() && sanitizeLutName("").empty());
  CHECK(sanitizeLutName("a\tb\nc") == "abc");
  CHECK(sanitizeLutName(std::string(100, 'a')).size() == kLutNameMax);
  CHECK(sanitizeLutName(std::string(47, 'a') + "  b") == std::string(47, 'a')); // no trailing space after the cap
  // A name can never leave the library folder.
  CHECK(sanitizeLutName("../../etc/passwd").find('/') == std::string::npos);
  CHECK(lutFileStem("../x").find('/') == std::string::npos && lutFileStem("..\\x").find('\\') == std::string::npos);
  CHECK(lutFileStem("..") == "lut" && lutFileStem("") == "lut");
  CHECK(lutFileStem("con") == "_con" && lutFileStem("COM1") == "_COM1" && lutFileStem("COM10") == "COM10");
  CHECK(lutFileStem("Warm Film") == "Warm Film");
}

static void testHashHex()
{
  CHECK(lutHashToHex(0) == "0000000000000000");
  CHECK(lutHashToHex(0x0123456789abcdefull) == "0123456789abcdef");
  uint64_t h = 7;
  CHECK(lutHashFromHex("0123456789ABCDEF", &h) && h == 0x0123456789abcdefull);
  const uint64_t keep = h;
  CHECK(!lutHashFromHex("123", &h) && !lutHashFromHex("0123456789abcdeg", &h) && !lutHashFromHex("", &h) &&
        !lutHashFromHex("0123456789abcdef0", &h));
  CHECK(h == keep);
  CHECK(LutRef{} == LutRef{} && LutRef{}.empty() && !(LutRef{"a", 1} == LutRef{"a", 2}) && !LutRef{"a", 0}.empty());
}

static void testValid()
{
  CHECK(Lut3D{}.valid() == false);
  CHECK(identityLut(2).valid() && identityLut(33).valid() && identityLut(65).valid());
  CHECK(identityLut(1).size == kLutMinSize && identityLut(500).size == kLutMaxSize); // clamped
  Lut3D L = identityLut(5);
  L.rgb.pop_back();
  CHECK(!L.valid());
  L = identityLut(5);
  L.domainMax[1] = L.domainMin[1];
  L.finalize();
  CHECK(!L.valid());
  L = identityLut(5);
  L.domainMin[0] = std::nanf("");
  L.finalize();
  CHECK(!L.valid());
  L = identityLut(5);
  L.size = 66;
  CHECK(!L.valid());
}

static void testIdentityIsIdentity()
{
  Rng rng;
  for (int n : {2, 3, 5, 17, 33, 65})
  {
    const Lut3D L = identityLut(n);
    float o[3];
    for (int i = 0; i < 300; ++i)
    {
      const float r = rng.next() * 255.0f, g = rng.next() * 255.0f, b = rng.next() * 255.0f;
      lutEval(L, r, g, b, o);
      CHECK_NEAR(o[0], r, 0.01);
      CHECK_NEAR(o[1], g, 0.01);
      CHECK_NEAR(o[2], b, 0.01);
    }
    // the corners and the whole gray axis
    lutEval(L, 0, 0, 0, o);
    CHECK(o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f);
    lutEval(L, 255, 255, 255, o);
    CHECK_NEAR(o[0], 255.0, 1e-3);
    CHECK_NEAR(o[1], 255.0, 1e-3);
    CHECK_NEAR(o[2], 255.0, 1e-3);
    for (int v = 0; v <= 255; ++v)
    {
      lutEval(L, static_cast<float>(v), static_cast<float>(v), static_cast<float>(v), o);
      CHECK_NEAR(o[0], o[1], 1e-3); // gray stays gray
      CHECK_NEAR(o[1], o[2], 1e-3);
    }
  }
}

static void testAffineIsExact()
{
  // Tetrahedral interpolation reproduces any function that is affine in a cell, in
  // every one of the six tetrahedra: check with random points (all orderings of the
  // fractional parts occur) on a coarse grid.
  const auto f = [](float r, float g, float b, float *o)
  {
    o[0] = 0.5f * r + 0.25f * g + 10.0f;
    o[1] = 0.8f * g + 0.1f * b + 0.05f * r;
    o[2] = 255.0f - 0.9f * b + 0.3f * r;
  };
  const Lut3D L = tableOf(5, f);
  Rng rng;
  int seen[6] = {0, 0, 0, 0, 0, 0};
  for (int i = 0; i < 2000; ++i)
  {
    const float r = rng.next() * 255.0f, g = rng.next() * 255.0f, b = rng.next() * 255.0f;
    float got[3], want[3];
    lutEval(L, r, g, b, got);
    f(r, g, b, want);
    for (int k = 0; k < 3; ++k)
      CHECK_NEAR(got[k], want[k], 0.02);
    const float step = 255.0f / 4.0f;
    const float fr = std::fmod(r, step), fg = std::fmod(g, step), fb = std::fmod(b, step);
    const int t = fr >= fg ? (fg >= fb ? 0 : (fr >= fb ? 1 : 2)) : (fr >= fb ? 3 : (fg >= fb ? 4 : 5));
    ++seen[t];
  }
  for (int t = 0; t < 6; ++t)
    CHECK(seen[t] > 50); // every tetrahedron was exercised

  // A non-affine table: exact at the grid points, continuous between them.
  const Lut3D P = tableOf(9, [](float r, float g, float b, float *o)
                          {
                            o[0] = r * g / 255.0f;
                            o[1] = g * b / 255.0f;
                            o[2] = r * b / 255.0f; });
  float o[3];
  const float k = 255.0f / 8.0f;
  for (int ir = 0; ir < 9; ++ir)
    for (int ig = 0; ig < 9; ig += 2)
    {
      const float r = static_cast<float>(ir) * k, g = static_cast<float>(ig) * k, b = 4.0f * k;
      lutEval(P, r, g, b, o);
      CHECK_NEAR(o[0], r * g / 255.0, 0.05);
      CHECK_NEAR(o[1], g * b / 255.0, 0.05);
      CHECK_NEAR(o[2], r * b / 255.0, 0.05);
    }
  float a[3], c[3];
  lutEval(P, 100.0f, 80.0f, 60.0f, a);
  lutEval(P, 100.001f, 80.001f, 60.001f, c);
  for (int q = 0; q < 3; ++q)
    CHECK_NEAR(a[q], c[q], 0.01);
}

static void testClampingAndDomain()
{
  const Lut3D L = identityLut(17);
  float o[3];
  lutEval(L, -50.0f, 300.0f, std::nanf(""), o);
  CHECK_NEAR(o[0], 0.0, 1e-3);
  CHECK_NEAR(o[1], 255.0, 1e-3);
  CHECK_NEAR(o[2], 0.0, 1e-3);
  lutEval(L, INFINITY, -INFINITY, 0.0f, o);
  CHECK_NEAR(o[0], 255.0, 1e-3);
  CHECK_NEAR(o[1], 0.0, 1e-3);

  // A table whose domain is 0..0.5: pixel value 127.5 is its LAST grid point.
  Lut3D D = tableOf(5, [](float r, float g, float b, float *o)
                    {
                      o[0] = r;
                      o[1] = g;
                      o[2] = b; });
  D.domainMax = {0.5f, 0.5f, 0.5f};
  D.finalize();
  CHECK(D.valid());
  lutEval(D, 127.5f, 0.0f, 0.0f, o);
  CHECK_NEAR(o[0], 255.0, 1e-3); // the table's last entry is 255
  lutEval(D, 255.0f, 0.0f, 0.0f, o);
  CHECK_NEAR(o[0], 255.0, 1e-3); // beyond the domain clamps
  lutEval(D, 63.75f, 0.0f, 0.0f, o);
  CHECK_NEAR(o[0], 127.5, 1e-2); // halfway through the domain

  // A negative domain min shifts the mapping.
  Lut3D M = tableOf(3, [](float r, float g, float b, float *o)
                    {
                      o[0] = r;
                      o[1] = g;
                      o[2] = b; });
  M.domainMin = {-1.0f, -1.0f, -1.0f};
  M.domainMax = {1.0f, 1.0f, 1.0f};
  M.finalize();
  lutEval(M, 0.0f, 127.5f, 255.0f, o); // domain positions 0, 0.5, 1 -> grid 1, 1.5, 2
  CHECK_NEAR(o[0], 127.5, 1e-2);
  CHECK_NEAR(o[1], 191.25, 1e-2);
  CHECK_NEAR(o[2], 255.0, 1e-2);
}

static void testPass()
{
  auto id = std::make_shared<const Lut3D>(identityLut(9));
  CHECK(!makeLutPass(nullptr, 100.0f).any());
  CHECK(!makeLutPass(id, 0.0f).any() && !makeLutPass(id, -5.0f).any());
  CHECK(!makeLutPass(id, std::nanf("")).any() && !makeLutPass(id, INFINITY).any());
  CHECK(makeLutPass(id, 100.0f).amount == 1.0f && makeLutPass(id, 50.0f).amount == 0.5f);
  CHECK(makeLutPass(id, 900.0f).amount == 1.0f); // clamped
  CHECK(makeLutPass(id, 100.0f).any() && makeLutPass(id, 100.0f).lut == id);
  auto bad = std::make_shared<const Lut3D>(Lut3D{});
  CHECK(!makeLutPass(bad, 100.0f).any()); // invalid table: inactive

  // Amount blends between the pixel and the LUT's answer.
  auto inv = std::make_shared<const Lut3D>(tableOf(2, [](float r, float g, float b, float *o)
                                                    {
                                                      o[0] = 255.0f - r;
                                                      o[1] = 255.0f - g;
                                                      o[2] = 255.0f - b; }));
  float r = 40.0f, g = 100.0f, b = 200.0f;
  applyLutPixel(makeLutPass(inv, 100.0f), r, g, b);
  CHECK_NEAR(r, 215.0, 0.01);
  CHECK_NEAR(g, 155.0, 0.01);
  CHECK_NEAR(b, 55.0, 0.01);
  r = 40.0f;
  g = 100.0f;
  b = 200.0f;
  applyLutPixel(makeLutPass(inv, 50.0f), r, g, b);
  CHECK_NEAR(r, 127.5, 0.01); // halfway between 40 and 215
  CHECK_NEAR(g, 127.5, 0.01);
  CHECK_NEAR(b, 127.5, 0.01);
}

int main()
{
  testNames();
  testHashHex();
  testValid();
  testIdentityIsIdentity();
  testAffineIsExact();
  testClampingAndDomain();
  testPass();
  return finish();
}
