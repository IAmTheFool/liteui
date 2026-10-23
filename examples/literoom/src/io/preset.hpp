// preset.hpp - Presets: a saved "look" (sliders, curves, Color Mixer, Color
// Grading) that can be applied to any picture. Geometry (crop, rotation,
// straighten) and masks are NOT part of a preset: they belong to one picture.
// Pure functions on plain values plus small file helpers; no UI, no editor
// state. Loading reuses the sidecar readers, so every value is validated and
// clamped exactly as a saved-edits file is.
//
// File: <presetDir()>/<name>.literoom-preset.json
//   { "app": "literoom", "kind": "preset", "version": 1, "name": "...",
//     "adjust": {...}, "curves": {...}, "colorMix": {...}, "colorGrading": {...} }
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "color/color_grade.hpp"
#include "color/color_mix.hpp"
#include "color/curves.hpp"
#include "core/adjustments.hpp"
#include "io/file_utils.hpp"
#include "io/minijson.hpp"
#include "io/sidecar.hpp"

constexpr int kPresetVersion = 1;
constexpr const char *kPresetExt = ".literoom-preset.json";
constexpr size_t kPresetNameMax = 32;
constexpr size_t kPresetMaxFileBytes = size_t{1} << 20; // 1 MiB: a preset is tiny
constexpr size_t kPresetMaxFiles = 500;

struct Preset
{
  std::string name;
  AdjValues values{};
  CurveSet curves = identityCurves();
  MixValues mix{};
  ColorGrade grade;
  bool operator==(const Preset &) const = default;
};

// ---------------- Names ----------------
// Printable ASCII only (the TextInput widget is ASCII), trimmed, capped.
inline std::string sanitizePresetName(const std::string &s)
{
  std::string o;
  for (char c : s)
    if (c >= 0x20 && c <= 0x7E)
      o.push_back(c);
  const size_t b = o.find_first_not_of(' ');
  if (b == std::string::npos)
    return {};
  const size_t e = o.find_last_not_of(' ');
  o = o.substr(b, e - b + 1);
  if (o.size() > kPresetNameMax)
  {
    o.resize(kPresetNameMax);
    const size_t e2 = o.find_last_not_of(' ');
    o.resize(e2 + 1);
  }
  return o;
}
inline bool presetNameBlank(const std::string &s)
{
  return sanitizePresetName(s).empty();
}

// A file-name stem for a preset name: characters Windows forbids become '_',
// no trailing dot/space, and the reserved device names are prefixed.
inline std::string presetFileStem(const std::string &name)
{
  std::string s = sanitizePresetName(name);
  for (char &c : s)
    if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
      c = '_';
  while (!s.empty() && (s.back() == '.' || s.back() == ' '))
    s.pop_back();
  if (s.empty())
    return "preset";
  std::string up = s;
  for (char &c : up)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  const bool reserved =
      up == "CON" || up == "PRN" || up == "AUX" || up == "NUL" ||
      ((up.rfind("COM", 0) == 0 || up.rfind("LPT", 0) == 0) && up.size() == 4 &&
       up[3] >= '1' && up[3] <= '9');
  return reserved ? "_" + s : s;
}

// ---------------- JSON ----------------
inline mj::Json presetToJson(const Preset &p)
{
  using mj::Json;
  Json o = Json::object();
  o.set("app", Json::str("literoom"));
  o.set("kind", Json::str("preset"));
  o.set("version", Json::num(kPresetVersion));
  o.set("name", Json::str(sanitizePresetName(p.name)));
  o.set("adjust", sidecarAdjToJson(p.values));
  o.set("curves", sidecarCurvesToJson(p.curves));
  o.set("colorMix", sidecarMixToJson(p.mix));
  o.set("colorGrading", sidecarGradeToJson(p.grade));
  return o;
}

// False (and *out untouched) if `o` is not a preset object of a version this
// build understands. Everything inside is clamped, never trusted. A missing
// name stays empty; loadPresetFile fills it from the file name.
inline bool presetFromJson(const mj::Json &o, Preset *out)
{
  if (!o.isObject())
    return false;
  const mj::Json *kind = o.find("kind");
  if (!kind || !kind->isString() || kind->asStr() != "preset")
    return false;
  const int ver = o.get("version").asInt(0, 0, 1000000);
  if (ver < 1 || ver > kPresetVersion)
    return false;
  Preset p;
  p.name = sanitizePresetName(o.get("name").asStr());
  p.values = sidecarAdjFromJson(o.find("adjust"));
  if (const mj::Json *cs = o.find("curves"); cs && cs->isObject())
    for (size_t c = 0; c < kCurveChannelCount; ++c)
      if (const mj::Json *a = cs->find(kSidecarCurveKeys[c]))
        p.curves[c] = sidecarCurveFromJson(*a);
  p.mix = sidecarMixFromJson(o.find("colorMix"));
  p.grade = sidecarGradeFromJson(o.find("colorGrading"));
  *out = std::move(p);
  return true;
}

// ---------------- Files ----------------
// Where presets live: %APPDATA%\Literoom\presets on Windows,
// $XDG_CONFIG_HOME/literoom/presets or ~/.config/literoom/presets elsewhere.
// Falls back to a relative "presets" folder if no home can be found.
inline std::string presetDir()
{
#ifdef _WIN32
  if (const char *a = std::getenv("APPDATA"); a && *a)
    return std::string(a) + "\\Literoom\\presets";
#else
  if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return std::string(x) + "/literoom/presets";
  if (const char *h = std::getenv("HOME"); h && *h)
    return std::string(h) + "/.config/literoom/presets";
#endif
  return "presets";
}

inline std::string presetPath(const std::string &dir, const std::string &name)
{
  return dir + "/" + presetFileStem(name) + kPresetExt;
}

// Saves `p` (same name = overwrite). Creates the folder if needed. On success
// *pathOut (optional) is the file written.
inline bool savePresetFile(const std::string &dir, const Preset &p,
                           std::string *err, std::string *pathOut = nullptr)
{
  if (presetNameBlank(p.name))
  {
    if (err)
      *err = "give the preset a name";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(fsPath(dir), ec);
  if (ec)
  {
    if (err)
      *err = ec.message();
    return false;
  }
  const std::string path = presetPath(dir, p.name);
  const std::string text = presetToJson(p).dump();
  const std::vector<uint8_t> bytes(text.begin(), text.end());
  if (!writeFileAtomic(path, bytes, err))
    return false;
  if (pathOut)
    *pathOut = path;
  return true;
}

inline bool loadPresetFile(const std::string &path, Preset *out,
                           std::string *err)
{
  std::vector<uint8_t> bytes;
  if (!readFilePrefix(path, kPresetMaxFileBytes + 1, &bytes))
  {
    if (err)
      *err = "couldn't read " + baseName(path);
    return false;
  }
  if (bytes.size() > kPresetMaxFileBytes)
  {
    if (err)
      *err = baseName(path) + " is too large for a preset";
    return false;
  }
  mj::Json j;
  std::string perr;
  if (!mj::Json::parse(
          std::string_view(reinterpret_cast<const char *>(bytes.data()),
                           bytes.size()),
          &j, &perr))
  {
    if (err)
      *err = baseName(path) + ": " + perr;
    return false;
  }
  Preset p;
  if (!presetFromJson(j, &p))
  {
    if (err)
      *err = baseName(path) + " is not a Literoom preset";
    return false;
  }
  if (p.name.empty()) // fall back to the file name without the extension
  {
    std::string stem = baseName(path);
    if (filmLower(stem).size() >= std::string(kPresetExt).size())
      stem.resize(stem.size() - std::string(kPresetExt).size());
    p.name = sanitizePresetName(stem);
  }
  *out = std::move(p);
  return true;
}

// File names (no directory) of the presets in `dir`, natural order. Missing
// folder gives {}.
inline std::vector<std::string> listPresetFiles(const std::string &dir)
{
  std::vector<std::string> names;
  std::error_code ec;
  const std::string ext = kPresetExt;
  for (std::filesystem::directory_iterator it(fsPath(dir), ec), end;
       !ec && it != end && names.size() < kPresetMaxFiles; it.increment(ec))
  {
    std::error_code e2;
    if (!it->is_regular_file(e2) || e2)
      continue;
    std::string n = u8ToString(it->path().filename());
    const std::string low = filmLower(n);
    if (low.size() > ext.size() &&
        low.compare(low.size() - ext.size(), ext.size(), ext) == 0)
      names.push_back(std::move(n));
  }
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b)
            { return naturalLess(a, b) || (!naturalLess(b, a) && a < b); });
  return names;
}

inline bool deletePresetFile(const std::string &path)
{
  std::error_code ec;
  return std::filesystem::remove(fsPath(path), ec) && !ec;
}
