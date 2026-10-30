// lut_cube.hpp - Reads a 3D LUT in the Adobe / DaVinci Resolve ".cube" format.
// Pure text in, Lut3D out: no files (lut_library.hpp reads them), no UI.
//
// Strict, like every other reader in Literoom: anything it does not understand
// is an error with the line number, never a guess. Supported: LUT_3D_SIZE
// (2..65), DOMAIN_MIN / DOMAIN_MAX, LUT_3D_INPUT_RANGE, TITLE, comments (#),
// blank lines, CRLF or LF. Refused: 1D LUTs (LUT_1D_SIZE), unknown keywords,
// a wrong number of entries, non-finite or absurd (|v| > 16) values, a
// keyword after the data. Numbers are read with std::from_chars, so the
// system's locale (a decimal comma) can never change what a file means.
#pragma once
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "color/lut3d.hpp"

struct LutCubeInfo
{
  std::string title;         // TITLE "..." (printable ASCII, capped), may be empty
  bool domainDefault = true; // DOMAIN is 0..1 on every channel
  bool videoRange = false;   // LUT_IN_VIDEO_RANGE / LUT_OUT_VIDEO_RANGE was present
};

namespace lutcube_detail
{
  inline std::string_view trim(std::string_view s)
  {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
      s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
      s.remove_suffix(1);
    return s;
  }

  // Next whitespace-separated word of `s` (which is advanced past it).
  inline bool nextToken(std::string_view &s, std::string_view *tok)
  {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
      s.remove_prefix(1);
    if (s.empty())
      return false;
    size_t n = 0;
    while (n < s.size() && !std::isspace(static_cast<unsigned char>(s[n])))
      ++n;
    *tok = s.substr(0, n);
    s.remove_prefix(n);
    return true;
  }

  // A finite float, all of `tok`. A leading '+' is allowed (from_chars refuses it).
  inline bool parseFloat(std::string_view tok, float *out)
  {
    if (!tok.empty() && tok.front() == '+')
      tok.remove_prefix(1);
    if (tok.empty())
      return false;
    float v = 0.0f;
    const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
    if (ec != std::errc() || p != tok.data() + tok.size() || !std::isfinite(v))
      return false;
    *out = v;
    return true;
  }

  inline bool parseInt(std::string_view tok, int *out)
  {
    int v = 0;
    const auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
    if (ec != std::errc() || p != tok.data() + tok.size())
      return false;
    *out = v;
    return true;
  }

  inline std::string upper(std::string_view s)
  {
    std::string o(s);
    for (char &c : o)
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return o;
  }

  inline std::string lineErr(int line, const std::string &msg)
  {
    return "line " + std::to_string(line) + ": " + msg;
  }
} // namespace lutcube_detail

// Parses `text`. False (and *out, *info untouched) with a reason in *err
// otherwise. `info` and `err` may be null.
inline bool parseCube(std::string_view text, Lut3D *out, LutCubeInfo *info,
                      std::string *err)
{
  using namespace lutcube_detail;
  const auto fail = [&](const std::string &m)
  {
    if (err)
      *err = m;
    return false;
  };

  LutCubeInfo meta;
  std::array<float, 3> dmin{0.0f, 0.0f, 0.0f}, dmax{1.0f, 1.0f, 1.0f};
  int size = 0;
  bool haveSize = false, sawData = false;
  size_t want = 0, got = 0;
  std::vector<float> rgb;

  size_t pos = 0;
  int lineNo = 0;
  while (pos < text.size())
  {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos)
      eol = text.size();
    std::string_view line = text.substr(pos, eol - pos);
    pos = eol + 1;
    ++lineNo;

    line = trim(line);
    if (line.empty() || line.front() == '#')
      continue;

    const unsigned char c0 = static_cast<unsigned char>(line.front());
    if (std::isdigit(c0) || c0 == '-' || c0 == '+' || c0 == '.')
    {
      // ---- a data line: exactly three numbers ----
      if (const size_t h = line.find('#'); h != std::string_view::npos)
        line = trim(line.substr(0, h));
      if (!haveSize)
        return fail(lineErr(lineNo, "table data before LUT_3D_SIZE"));
      if (got >= want)
        return fail(lineErr(lineNo, "more entries than LUT_3D_SIZE allows"));
      sawData = true;
      std::string_view t;
      float v[3];
      for (int k = 0; k < 3; ++k)
      {
        if (!nextToken(line, &t) || !parseFloat(t, &v[k]))
          return fail(lineErr(lineNo, "expected three finite numbers"));
        if (std::fabs(v[k]) > 16.0f)
          return fail(lineErr(lineNo, "value out of range"));
      }
      if (nextToken(line, &t))
        return fail(lineErr(lineNo, "expected three numbers, found more"));
      rgb.push_back(v[0] * 255.0f);
      rgb.push_back(v[1] * 255.0f);
      rgb.push_back(v[2] * 255.0f);
      ++got;
      continue;
    }

    // ---- a keyword line ----
    std::string_view rest = line, kw;
    nextToken(rest, &kw);
    const std::string key = upper(kw);
    if (sawData)
      return fail(lineErr(lineNo, "'" + std::string(kw) + "' after the table data"));

    if (key == "TITLE")
    {
      std::string_view t = trim(rest);
      if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
        t = t.substr(1, t.size() - 2);
      meta.title.clear();
      for (char ch : t)
        if (ch >= 0x20 && ch <= 0x7E && meta.title.size() < 100)
          meta.title.push_back(ch);
      continue;
    }
    // Everything else may carry a trailing comment.
    if (const size_t h = rest.find('#'); h != std::string_view::npos)
      rest = rest.substr(0, h);

    if (key == "LUT_3D_SIZE")
    {
      std::string_view t;
      int n = 0;
      if (!nextToken(rest, &t) || !parseInt(t, &n) || nextToken(rest, &t))
        return fail(lineErr(lineNo, "LUT_3D_SIZE needs one whole number"));
      if (haveSize)
        return fail(lineErr(lineNo, "LUT_3D_SIZE given twice"));
      if (n < kLutMinSize || n > kLutMaxSize)
        return fail(lineErr(lineNo, "size " + std::to_string(n) +
                                        " isn't supported (" + std::to_string(kLutMinSize) +
                                        " to " + std::to_string(kLutMaxSize) + ")"));
      size = n;
      haveSize = true;
      want = static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n);
      rgb.reserve(want * 3);
    }
    else if (key == "LUT_1D_SIZE")
      return fail(lineErr(lineNo, "1D LUTs aren't supported yet"));
    else if (key == "DOMAIN_MIN" || key == "DOMAIN_MAX")
    {
      std::array<float, 3> v{};
      std::string_view t;
      for (size_t k = 0; k < 3; ++k)
        if (!nextToken(rest, &t) || !parseFloat(t, &v[k]))
          return fail(lineErr(lineNo, key + " needs three numbers"));
      if (nextToken(rest, &t))
        return fail(lineErr(lineNo, key + " needs three numbers, found more"));
      (key == "DOMAIN_MIN" ? dmin : dmax) = v;
    }
    else if (key == "LUT_3D_INPUT_RANGE")
    {
      float lo = 0.0f, hi = 0.0f;
      std::string_view t;
      if (!nextToken(rest, &t) || !parseFloat(t, &lo) || !nextToken(rest, &t) ||
          !parseFloat(t, &hi) || nextToken(rest, &t))
        return fail(lineErr(lineNo, "LUT_3D_INPUT_RANGE needs two numbers"));
      dmin = {lo, lo, lo};
      dmax = {hi, hi, hi};
    }
    else if (key == "LUT_IN_VIDEO_RANGE" || key == "LUT_OUT_VIDEO_RANGE")
      meta.videoRange = true;
    else
      return fail(lineErr(lineNo, "unknown keyword '" + std::string(kw) + "'"));
  }

  if (!haveSize)
    return fail("no LUT_3D_SIZE (only 3D LUTs are supported)");
  if (got != want)
    return fail("has " + std::to_string(got) + " entries, expected " +
                std::to_string(want));
  for (size_t c = 0; c < 3; ++c)
    if (!(dmax[c] > dmin[c]))
      return fail("DOMAIN_MAX must be larger than DOMAIN_MIN");
  meta.domainDefault = true;
  for (size_t c = 0; c < 3; ++c)
    if (dmin[c] != 0.0f || dmax[c] != 1.0f)
      meta.domainDefault = false;

  Lut3D L;
  L.size = size;
  L.domainMin = dmin;
  L.domainMax = dmax;
  L.rgb = std::move(rgb);
  L.finalize();
  if (!L.valid())
    return fail("the table is not usable");
  *out = std::move(L);
  if (info)
    *info = std::move(meta);
  return true;
}

// ---------------- "Made for another input?" ----------------
// Literoom applies a LUT to the picture's own sRGB / Rec.709 values. A LUT made
// for camera-log footage looks wrong there. Nothing in a .cube file says so for
// certain, so this only suggests: any one of three hints is enough, and the
// answer is a sentence for the status bar, or "" when nothing looks off.
//   * the name or title has a log-curve word (log, slog3, vlog, logc, ...);
//   * the input domain isn't 0..1;
//   * mid-gray comes out far from mid-gray.
// `label` is the LUT's name (the title is checked as well).
inline bool lutNameLooksLog(std::string_view s)
{
  static const char *const kBases[] = {"log",  "slog", "vlog",    "clog",    "logc",
                                       "flog", "nlog", "dlog",    "dlogm",   "redlog",
                                       "log3g", "acescct", "acescc"};
  std::string tok;
  const auto test = [&]() -> bool
  {
    while (!tok.empty() && std::isdigit(static_cast<unsigned char>(tok.back())))
      tok.pop_back();
    for (const char *b : kBases)
      if (tok == b)
        return true;
    return false;
  };
  for (char ch : s)
  {
    if (std::isalnum(static_cast<unsigned char>(ch)))
      tok.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    else
    {
      if (test())
        return true;
      tok.clear();
    }
  }
  return test();
}

inline std::string lutExpectationWarning(const std::string &label, const Lut3D &L,
                                         const LutCubeInfo &info)
{
  std::vector<std::string> why;
  if (lutNameLooksLog(label) || lutNameLooksLog(info.title))
    why.push_back("its name mentions a log curve");
  if (!info.domainDefault)
    why.push_back("its input range isn't 0 to 1");
  if (L.valid())
  {
    float o[3];
    lutEval(L, 127.5f, 127.5f, 127.5f, o);
    const float mean = (o[0] + o[1] + o[2]) / (3.0f * 255.0f);
    if (std::fabs(mean - 0.5f) > 0.25f)
      why.push_back("it moves mid-gray a long way");
  }
  if (why.empty())
    return {};
  std::string m = label + " may expect camera-log input (";
  for (size_t i = 0; i < why.size(); ++i)
    m += (i ? ", " : "") + why[i];
  m += "). Literoom applies LUTs to sRGB / Rec.709 pictures, so it may look wrong.";
  return m;
}
