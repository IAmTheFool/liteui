// lut3d.hpp - 3D color LUTs: the table (Lut3D), the name that points at one in
// the library (LutRef), and the per-pixel lookup (tetrahedral interpolation).
// Pure values and arithmetic: no files, no UI, no editor state. Reading a
// .cube file is io/lut_cube.hpp; finding one by name is io/lut_library.hpp.
//
// A LUT here maps the picture's own 8-bit RGB (display-referred sRGB / Rec.709,
// which is all Literoom has today) to new RGB. The table keeps the input domain
// of the file (DOMAIN_MIN / DOMAIN_MAX) and maps pixel values onto the grid
// through it, so a LUT built for another input range or a camera-log workflow
// can be supported later without touching the table format.
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr int kLutMinSize = 2;
constexpr int kLutMaxSize = 65;
constexpr size_t kLutNameMax = 48;

// ---------------- Names and references ----------------
// Printable ASCII only (the TextInput widget is ASCII), characters Windows
// forbids in file names become '_', trimmed, capped, no trailing dot or space.
// The result is safe to use as a file-name stem, so a name read from a
// hand-edited sidecar can never point outside the library folder.
inline std::string sanitizeLutName(const std::string &s)
{
  std::string o;
  for (char c : s)
  {
    if (c < 0x20 || c > 0x7E)
      continue;
    if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
      c = '_';
    o.push_back(c);
  }
  const size_t b = o.find_first_not_of(' ');
  if (b == std::string::npos)
    return {};
  o.erase(0, b);
  if (o.size() > kLutNameMax)
    o.resize(kLutNameMax);
  while (!o.empty() && (o.back() == ' ' || o.back() == '.'))
    o.pop_back();
  return o;
}

// The file-name stem a LUT name is stored under: the name itself, "lut" if it is
// blank, and the reserved Windows device names get a "_" in front.
inline std::string lutFileStem(const std::string &name)
{
  std::string s = sanitizeLutName(name);
  if (s.empty())
    return "lut";
  std::string up = s;
  for (char &c : up)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const bool reserved =
      up == "CON" || up == "PRN" || up == "AUX" || up == "NUL" ||
      ((up.rfind("COM", 0) == 0 || up.rfind("LPT", 0) == 0) && up.size() == 4 &&
       up[3] >= '1' && up[3] <= '9');
  return reserved ? "_" + s : s;
}

// 16 lowercase hex digits (the form the sidecar and presets store a hash in).
inline std::string lutHashToHex(uint64_t h)
{
  static const char *d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, h >>= 4)
    s[static_cast<size_t>(i)] = d[h & 0xF];
  return s;
}
// False (and *out untouched) unless `s` is exactly 16 hex digits.
inline bool lutHashFromHex(std::string_view s, uint64_t *out)
{
  if (s.size() != 16)
    return false;
  uint64_t h = 0;
  for (char c : s)
  {
    int v;
    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else
      return false;
    h = (h << 4) | static_cast<uint64_t>(v);
  }
  *out = h;
  return true;
}

// What an edit stores to say "this picture uses that LUT": the library name and
// a hash of the file's bytes when it was chosen. Hash 0 = unknown (accept any).
struct LutRef
{
  std::string name; // empty = no LUT
  uint64_t hash = 0;
  bool empty() const { return name.empty(); }
  bool operator==(const LutRef &) const = default;
};

// ---------------- The table ----------------
struct Lut3D
{
  int size = 0; // grid points per axis, kLutMinSize..kLutMaxSize
  std::array<float, 3> domainMin{0.0f, 0.0f, 0.0f}; // input range of the file
  std::array<float, 3> domainMax{1.0f, 1.0f, 1.0f};
  // size^3 RGB triples in 0..255 units (the picture's own scale), the red
  // index varying fastest, then green, then blue: the order of a .cube file.
  std::vector<float> rgb;
  // Derived by finalize(): pixel value (0..255) -> grid position
  // (pos = value * scale + offset).
  std::array<float, 3> scale{0.0f, 0.0f, 0.0f};
  std::array<float, 3> offset{0.0f, 0.0f, 0.0f};

  // Call after size and domain are set (the parser and identityLut do).
  void finalize()
  {
    for (size_t c = 0; c < 3; ++c)
    {
      const float span = domainMax[c] - domainMin[c];
      scale[c] = static_cast<float>(size - 1) / (255.0f * span);
      offset[c] = -domainMin[c] * static_cast<float>(size - 1) / span;
    }
  }

  // Safe to look up: size in range, the right number of entries, a sane domain.
  bool valid() const
  {
    if (size < kLutMinSize || size > kLutMaxSize)
      return false;
    const size_t n = static_cast<size_t>(size);
    if (rgb.size() != n * n * n * 3)
      return false;
    for (size_t c = 0; c < 3; ++c)
      if (!std::isfinite(domainMin[c]) || !std::isfinite(domainMax[c]) ||
          !(domainMax[c] > domainMin[c]) || !std::isfinite(scale[c]) ||
          !(scale[c] > 0.0f) || !std::isfinite(offset[c]))
        return false;
    return true;
  }
};

// A LUT that changes nothing (up to float rounding), `n` points per axis.
inline Lut3D identityLut(int n)
{
  Lut3D L;
  L.size = std::clamp(n, kLutMinSize, kLutMaxSize);
  const size_t s = static_cast<size_t>(L.size);
  L.rgb.resize(s * s * s * 3);
  const float k = 255.0f / static_cast<float>(L.size - 1);
  size_t i = 0;
  for (size_t b = 0; b < s; ++b)
    for (size_t g = 0; g < s; ++g)
      for (size_t r = 0; r < s; ++r)
      {
        L.rgb[i++] = static_cast<float>(r) * k;
        L.rgb[i++] = static_cast<float>(g) * k;
        L.rgb[i++] = static_cast<float>(b) * k;
      }
  L.finalize();
  return L;
}

// ---------------- Lookup ----------------
// The LUT's output for one pixel (values in 0..255 units; inputs outside
// 0..255 and NaN are clamped first) with TETRAHEDRAL interpolation: the cell is
// cut into six tetrahedra along its main diagonal and the output is linear in
// each, so the gray axis stays exactly gray (trilinear leaves color casts
// there) and a LUT that is affine in the cell is reproduced exactly.
// Requires L.valid(). Raw pointers and no std:: helpers in the body, same
// Debug-build reason as applyColorPass.
inline void lutEval(const Lut3D &L, float r, float g, float b, float *out)
{
  const int n = L.size;
  const float top = static_cast<float>(n - 1);
  const float in[3] = {r, g, b};
  int i0[3];
  float d[3];
  for (int c = 0; c < 3; ++c)
  {
    float x = in[c];
    if (!(x > 0.0f))
      x = 0.0f;
    if (x > 255.0f)
      x = 255.0f;
    float p = x * L.scale[static_cast<size_t>(c)] + L.offset[static_cast<size_t>(c)];
    if (!(p > 0.0f))
      p = 0.0f;
    if (p > top)
      p = top;
    int i = static_cast<int>(p);
    if (i > n - 2)
      i = n - 2;
    i0[c] = i;
    d[c] = p - static_cast<float>(i);
  }
  const size_t sr = 3, sg = 3 * static_cast<size_t>(n),
               sb = 3 * static_cast<size_t>(n) * static_cast<size_t>(n);
  const float *base = L.rgb.data() + static_cast<size_t>(i0[0]) * sr +
                      static_cast<size_t>(i0[1]) * sg + static_cast<size_t>(i0[2]) * sb;
  const float dr = d[0], dg = d[1], db = d[2];
  // The path from the cell's origin to its far corner visits the axes in order
  // of decreasing fractional part: w1 on the first step, w2 on the second, w3
  // on the last.
  size_t v1, v2;
  float w1, w2, w3;
  if (dr >= dg)
  {
    if (dg >= db)
    { // r >= g >= b
      v1 = sr;
      v2 = sr + sg;
      w1 = dr;
      w2 = dg;
      w3 = db;
    }
    else if (dr >= db)
    { // r >= b > g
      v1 = sr;
      v2 = sr + sb;
      w1 = dr;
      w2 = db;
      w3 = dg;
    }
    else
    { // b > r >= g
      v1 = sb;
      v2 = sb + sr;
      w1 = db;
      w2 = dr;
      w3 = dg;
    }
  }
  else
  {
    if (dr >= db)
    { // g > r >= b
      v1 = sg;
      v2 = sg + sr;
      w1 = dg;
      w2 = dr;
      w3 = db;
    }
    else if (dg >= db)
    { // g >= b > r
      v1 = sg;
      v2 = sg + sb;
      w1 = dg;
      w2 = db;
      w3 = dr;
    }
    else
    { // b > g > r
      v1 = sb;
      v2 = sb + sg;
      w1 = db;
      w2 = dg;
      w3 = dr;
    }
  }
  const size_t v3 = sr + sg + sb;
  for (int k = 0; k < 3; ++k)
  {
    const float p0 = base[k], p1 = base[v1 + static_cast<size_t>(k)],
                p2 = base[v2 + static_cast<size_t>(k)], p3 = base[v3 + static_cast<size_t>(k)];
    out[k] = p0 + w1 * (p1 - p0) + w2 * (p2 - p1) + w3 * (p3 - p2);
  }
}

// ---------------- What the render loop holds ----------------
// A LUT frozen for the render thread. Shared and immutable, so copying a
// RenderParams / ColorPass stays cheap however big the table is.
struct LutPass
{
  std::shared_ptr<const Lut3D> lut; // null = no LUT
  float amount = 0.0f;              // 0..1
  bool any() const { return lut != nullptr && amount > 0.0f; }
};

// `slider` is the LUT Amount slider (0..100). No LUT, an invalid one, NaN or
// an amount of 0 all give the inactive pass.
inline LutPass makeLutPass(std::shared_ptr<const Lut3D> lut, float slider)
{
  LutPass p;
  if (!lut || !lut->valid() || !std::isfinite(slider))
    return p;
  const float a = std::clamp(slider, 0.0f, 100.0f) / 100.0f;
  if (a <= 0.0f)
    return p;
  p.lut = std::move(lut);
  p.amount = a;
  return p;
}

// One pixel (0..255 floats) in place: lerp(pixel, lut(pixel), amount).
inline void applyLutPixel(const LutPass &p, float &r, float &g, float &b)
{
  float o[3];
  lutEval(*p.lut, r, g, b, o);
  const float a = p.amount;
  r += (o[0] - r) * a;
  g += (o[1] - g) * a;
  b += (o[2] - b) * a;
}
