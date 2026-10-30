// Build: g++ -std=c++20 -I../src -I. lut_cube_test.cpp -o lut_cube_test && ./lut_cube_test
#include "io/lut_cube.hpp"
#include "check.hpp"

#include <charconv>
#include <chrono>
#include <clocale>
#include <cstdio>

// `v` with 6 decimals and a '.', whatever the C locale is (snprintf would write a
// decimal comma under a German locale, which the parser rightly refuses).
static void appendNum(std::string &s, double v)
{
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 6);
  s.append(buf, r.ptr);
}

// A .cube holding the identity LUT of `n` points, with the header lines given.
static std::string cubeText(int n, const std::string &header = "", const char *eol = "\n")
{
  std::string s = header;
  s += "LUT_3D_SIZE " + std::to_string(n) + eol;
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
      {
        appendNum(s, r / double(n - 1));
        s += ' ';
        appendNum(s, g / double(n - 1));
        s += ' ';
        appendNum(s, b / double(n - 1));
        s += eol;
      }
  return s;
}

static bool parses(const std::string &t, Lut3D *L = nullptr, LutCubeInfo *i = nullptr, std::string *e = nullptr)
{
  Lut3D tmp;
  LutCubeInfo it;
  std::string et;
  return parseCube(t, L ? L : &tmp, i ? i : &it, e ? e : &et);
}

static std::string errOf(const std::string &t)
{
  Lut3D L;
  std::string e;
  CHECK(!parseCube(t, &L, nullptr, &e));
  CHECK(!e.empty());
  return e;
}

static void testBasics()
{
  Lut3D L;
  LutCubeInfo info;
  std::string err;
  CHECK(parseCube(cubeText(2), &L, &info, &err));
  CHECK(L.size == 2 && L.valid() && L.rgb.size() == 24 && info.domainDefault && !info.videoRange && info.title.empty());
  // The order of a cube file: red changes fastest, then green, then blue.
  const std::string t =
      "LUT_3D_SIZE 2\n"
      "0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n";
  if (parses(t, &L) && L.rgb.size() == 24)
  {
    CHECK(L.rgb[3] == 255.0f && L.rgb[4] == 0.0f);   // entry 1: r = 1
    CHECK(L.rgb[7] == 255.0f && L.rgb[6] == 0.0f);   // entry 2: g = 1
    CHECK(L.rgb[14] == 255.0f && L.rgb[12] == 0.0f); // entry 4: b = 1
  }
  else
    CHECK(false);
  // An identity cube really is the identity.
  if (parses(cubeText(17), &L) && L.valid())
  {
    float o[3];
    lutEval(L, 33.0f, 99.0f, 201.0f, o);
    CHECK_NEAR(o[0], 33.0, 0.05);
    CHECK_NEAR(o[1], 99.0, 0.05);
    CHECK_NEAR(o[2], 201.0, 0.05);
  }
  else
    CHECK(false);
  CHECK(parses(cubeText(65), &L) && L.size == 65 && L.rgb.size() == size_t{65} * 65 * 65 * 3);
}

static void testSyntaxAllowed()
{
  Lut3D L;
  LutCubeInfo info;
  // CRLF, comments, blank lines, title, trailing comment on data, +, exponent, tabs
  std::string t = "# made by hand\r\n\r\nTITLE \"My Look # 1\"\r\nlut_3d_size 2 # lower case ok\r\n"
                  "DOMAIN_MIN 0 0 0\r\nDOMAIN_MAX 1.0 1.0 1.0\r\n"
                  "0 0 0\r\n+1 0 0 # red\r\n0\t1\t0\r\n1 1 0\r\n0 0 1e0\r\n1.0E+0 0 1\r\n0 1 1\r\n.5e1 1 1\r\n";
  std::string err;
  if (parseCube(t, &L, &info, &err) && L.rgb.size() == 24)
  {
    CHECK(err.empty());
    CHECK(info.title == "My Look # 1" && info.domainDefault);
    CHECK(L.rgb[21] == 5.0f * 255.0f); // ".5e1" = 5 (allowed: finite and within range)
  }
  else
    CHECK(false);
  // A non-default domain is remembered.
  CHECK(parses(cubeText(3, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n"), &L, &info));
  CHECK(!info.domainDefault && L.domainMax[0] == 2.0f && L.valid());
  CHECK(parses(cubeText(3, "LUT_3D_INPUT_RANGE -0.1 1.1\n"), &L, &info));
  CHECK(!info.domainDefault && L.domainMin[2] == -0.1f && L.domainMax[1] == 1.1f);
  CHECK(parses(cubeText(2, "LUT_IN_VIDEO_RANGE\nLUT_OUT_VIDEO_RANGE\n"), &L, &info) && info.videoRange);
}

static void testRejects()
{
  CHECK(errOf("").find("LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("# only a comment\n").find("LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("LUT_1D_SIZE 4\n0 0 0\n").find("1D") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 1\n0 0 0\n").find("isn't supported") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 66\n").find("isn't supported") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 0\n").find("isn't supported") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE -3\n").find("isn't supported") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2.5\n").find("LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE\n").find("LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2 3\n").find("LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\nLUT_3D_SIZE 2\n").find("twice") != std::string::npos);
  CHECK(errOf("0 0 0\nLUT_3D_SIZE 2\n").find("before LUT_3D_SIZE") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 0\n").find("expected 8") != std::string::npos); // too few
  CHECK(errOf(cubeText(2) + "0 0 0\n").find("more entries") != std::string::npos); // too many
  CHECK(errOf("LUT_3D_SIZE 2\n0 0\n").find("three") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 0 0\n").find("more") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 abc\n").find("three") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 0.5x\n").find("three") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0,5 0 0\n").find("three") != std::string::npos); // a decimal comma is not a number
  CHECK(errOf("LUT_3D_SIZE 2\nnan 0 0\n").find("unknown keyword") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 99\n").find("out of range") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n-17 0 0\n").find("out of range") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\n1e999 0 0\n").find("three") != std::string::npos);
  CHECK(errOf("LUT_3D_SIZE 2\nFROBNICATE 1\n").find("unknown keyword") != std::string::npos);
  CHECK(errOf("DOMAIN_MIN 0 0\nLUT_3D_SIZE 2\n").find("DOMAIN_MIN") != std::string::npos);
  CHECK(errOf("DOMAIN_MIN 0 0 0 0\nLUT_3D_SIZE 2\n").find("DOMAIN_MIN") != std::string::npos);
  CHECK(errOf("DOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\n" + cubeText(2)).find("DOMAIN_MAX must be larger") != std::string::npos);
  CHECK(errOf(cubeText(2, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 0 1 1\n")).find("DOMAIN_MAX must be larger") != std::string::npos);
  CHECK(errOf("LUT_3D_INPUT_RANGE 0\nLUT_3D_SIZE 2\n").find("INPUT_RANGE") != std::string::npos);
  // a keyword after the table
  CHECK(errOf(cubeText(2) + "TITLE \"late\"\n").find("after the table") != std::string::npos);
  // Errors say where.
  CHECK(errOf("LUT_3D_SIZE 2\n0 0 0\n0 0 q\n").find("line 3") != std::string::npos);
  // A failed parse leaves the outputs alone.
  Lut3D keep = identityLut(3);
  LutCubeInfo ki;
  ki.title = "keep";
  std::string e;
  CHECK(!parseCube("LUT_3D_SIZE 2\n", &keep, &ki, &e));
  CHECK(keep.size == 3 && ki.title == "keep");
}

static void testLocaleDoesNotMatter()
{
  // from_chars ignores the C locale; make sure a comma locale (if installed) changes nothing.
  const char *old = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = old ? old : "C";
  for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"})
    if (std::setlocale(LC_NUMERIC, name))
      break;
  Lut3D L;
  if (parses(cubeText(2), &L) && L.rgb.size() == 24)
    CHECK(L.rgb[3] == 255.0f);
  else
    CHECK(false);
  if (parses("LUT_3D_SIZE 2\n0.25 0.5 0.75\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n", &L) && L.rgb.size() == 24)
  {
    CHECK_NEAR(L.rgb[0], 63.75, 1e-3);
    CHECK_NEAR(L.rgb[1], 127.5, 1e-3);
  }
  else
    CHECK(false);
  // ...and a decimal comma is never read, whatever the locale
  CHECK(!parses("LUT_3D_SIZE 2\n0,25 0,5 0,75\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n0 0 0\n"));
  std::setlocale(LC_NUMERIC, saved.c_str());
}

static void testSpeed()
{
  const std::string t = cubeText(65);
  const auto t0 = std::chrono::steady_clock::now();
  Lut3D L;
  CHECK(parses(t, &L));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("parse of a 65^3 cube (%zu bytes): %.1f ms\n", t.size(), ms);
  CHECK(ms < 3000.0); // generous: even an unoptimized build is far below this
}

static void testWarnings()
{
  CHECK(lutNameLooksLog("SLog3 to Rec709") && lutNameLooksLog("sony_slog2") && lutNameLooksLog("V-Log_to_709") &&
        lutNameLooksLog("ARRI LogC3 K1S1") && lutNameLooksLog("Log") && lutNameLooksLog("canon.clog2") &&
        lutNameLooksLog("RED Log3G10") && lutNameLooksLog("F-Log2"));
  CHECK(!lutNameLooksLog("Analog Film") && !lutNameLooksLog("Catalog") && !lutNameLooksLog("Warm Teal") && !lutNameLooksLog("Dialog") &&
        !lutNameLooksLog("") && !lutNameLooksLog("Kodak 2383"));

  Lut3D L;
  LutCubeInfo info;
  CHECK(parses(cubeText(9), &L, &info));
  CHECK(lutExpectationWarning("Neutral", L, info).empty()); // identity: nothing odd
  CHECK(!lutExpectationWarning("SLog3 to 709", L, info).empty());
  info.title = "Log to display";
  CHECK(!lutExpectationWarning("Neutral", L, info).empty()); // the title counts too
  info.title.clear();
  info.domainDefault = false;
  const std::string w = lutExpectationWarning("Odd", L, info);
  CHECK(w.find("Odd may expect camera-log input") == 0 && w.find("input range") != std::string::npos);
  info.domainDefault = true;
  // A LUT that lifts mid-gray far: flagged by its response alone.
  Lut3D bright = identityLut(9);
  for (float &v : bright.rgb)
    v = std::min(255.0f, v * 1.9f);
  CHECK(lutExpectationWarning("Bright", bright, info).find("mid-gray") != std::string::npos);
  // A mild creative LUT is left alone.
  Lut3D mild = identityLut(9);
  for (float &v : mild.rgb)
    v = std::min(255.0f, v * 1.1f);
  CHECK(lutExpectationWarning("Mild", mild, info).empty());
}

int main()
{
  testBasics();
  testSyntaxAllowed();
  testRejects();
  testLocaleDoesNotMatter();
  testSpeed();
  testWarnings();
  return finish();
}