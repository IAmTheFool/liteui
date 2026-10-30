// lut_library.hpp - The LUT library: .cube files kept in one folder, found by
// name, with a small cache of the ones in use. Standard library plus
// file_utils / lut_cube / lut3d: no liteui, no editor state, so everything here
// can be tested against a temp directory.
//
// Folder: %APPDATA%\Literoom\luts on Windows, $XDG_CONFIG_HOME/literoom/luts or
// ~/.config/literoom/luts elsewhere (next to the presets). A LUT's name is its
// file name without ".cube"; an edit refers to it by that name plus a hash of
// the file's bytes (LutRef, lut3d.hpp).
//
// THE CACHE. The pixel pipeline never reads files: AdjustmentSet turns a LutRef
// into a shared Lut3D through acquireLut(), which keeps the last few tables in
// memory (and remembers failures, so a missing file costs one file check, not
// one per slider frame). It is a global guarded by a mutex because the UI
// thread and the export worker both resolve LUTs. A table is parsed on the
// calling thread the first time it is needed. Call clearLutCache() whenever the
// folder's content may have changed (import, refresh, opening a picture).
// Previews of every LUT in the list use loadLutFile() instead, which never
// touches the cache, so they cannot push out the LUT that is in use.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "color/lut3d.hpp"
#include "io/file_utils.hpp"
#include "io/lut_cube.hpp"

constexpr const char *kLutExt = ".cube";
constexpr size_t kLutMaxFileBytes = size_t{16} << 20; // 16 MiB: a 65^3 cube is ~6 MB
constexpr size_t kLutMaxFiles = 100;
constexpr size_t kLutCacheEntries = 4;

// ---------------- Hash ----------------
// FNV-1a, 64 bit, of a file's bytes. Never 0: 0 means "unknown" in a LutRef.
inline uint64_t lutHashBytes(const uint8_t *p, size_t n)
{
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i)
  {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h == 0 ? 1 : h;
}

// ---------------- Folder ----------------
// Tests point the library at a temp folder with this (set it before any other
// thread uses the library).
inline std::string &lutDirOverride()
{
  static std::string s;
  return s;
}

inline std::string lutDir()
{
  if (!lutDirOverride().empty())
    return lutDirOverride();
#ifdef _WIN32
  if (const char *a = std::getenv("APPDATA"); a && *a)
    return std::string(a) + "\\Literoom\\luts";
#else
  if (const char *x = std::getenv("XDG_CONFIG_HOME"); x && *x)
    return std::string(x) + "/literoom/luts";
  if (const char *h = std::getenv("HOME"); h && *h)
    return std::string(h) + "/.config/literoom/luts";
#endif
  return "luts";
}

inline std::string lutPath(const std::string &dir, const std::string &name)
{
  return dir + "/" + lutFileStem(name) + kLutExt;
}

// The LUT name a file name stands for ("Warm.cube" -> "Warm").
inline std::string lutNameOfFile(const std::string &fileName)
{
  std::string s = baseName(fileName);
  const std::string ext = kLutExt;
  if (filmLower(s).size() >= ext.size() &&
      filmLower(s).compare(s.size() - ext.size(), ext.size(), ext) == 0)
    s.resize(s.size() - ext.size());
  return sanitizeLutName(s);
}

// ---------------- Reading ----------------
// All bytes of a LUT file, or false with a reason. Refuses empty and
// oversized files before reading them.
inline bool readLutBytes(const std::string &path, std::vector<uint8_t> *out,
                         std::string *err)
{
  std::ifstream f(fsPath(path), std::ios::binary | std::ios::ate);
  if (!f)
  {
    if (err)
      *err = "couldn't read " + baseName(path);
    return false;
  }
  const std::streamoff n = f.tellg();
  if (n <= 0)
  {
    if (err)
      *err = baseName(path) + " is empty";
    return false;
  }
  if (static_cast<uint64_t>(n) > kLutMaxFileBytes)
  {
    if (err)
      *err = baseName(path) + " is too large for a LUT (16 MiB at most)";
    return false;
  }
  out->resize(static_cast<size_t>(n));
  f.seekg(0);
  f.read(reinterpret_cast<char *>(out->data()), n);
  if (!f)
  {
    if (err)
      *err = "couldn't read " + baseName(path);
    return false;
  }
  return true;
}

inline bool parseLutBytes(const std::vector<uint8_t> &bytes, const std::string &path,
                          Lut3D *out, LutCubeInfo *info, std::string *err)
{
  std::string perr;
  if (!parseCube(std::string_view(reinterpret_cast<const char *>(bytes.data()),
                                  bytes.size()),
                 out, info, &perr))
  {
    if (err)
      *err = baseName(path) + ": " + perr;
    return false;
  }
  return true;
}

// Reads and parses one file, bypassing the cache. *hash (optional) is the hash
// of the file's bytes.
inline bool loadLutFile(const std::string &path, Lut3D *out, LutCubeInfo *info,
                        uint64_t *hash, std::string *err)
{
  std::vector<uint8_t> bytes;
  if (!readLutBytes(path, &bytes, err))
    return false;
  if (!parseLutBytes(bytes, path, out, info, err))
    return false;
  if (hash)
    *hash = lutHashBytes(bytes.data(), bytes.size());
  return true;
}

// ---------------- Listing and importing ----------------
struct LutEntry
{
  std::string name; // the library name (file name without ".cube")
  std::string path; // the file in the library
  uint64_t hash = 0;
  int size = 0; // grid points per axis (set by import)
};

// File names (no directory) of the LUTs in `dir`, natural order. A missing
// folder gives {}. At most kLutMaxFiles are collected (before sorting).
inline std::vector<std::string> listLutFiles(const std::string &dir)
{
  std::vector<std::string> names;
  std::error_code ec;
  const std::string ext = kLutExt;
  for (std::filesystem::directory_iterator it(fsPath(dir), ec), end;
       !ec && it != end && names.size() < kLutMaxFiles; it.increment(ec))
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

// Copies the .cube at `src` into the library `dir` after checking that it
// parses. The name is the file's name, cleaned (see sanitizeLutName); a taken
// name gets "-2", "-3", ... unless the taken file holds the very same bytes, in
// which case that one is reused and nothing is written. *warning (optional) is
// the "may expect camera-log input" sentence, or "". On failure nothing is
// written and *err says why.
inline bool importLutFile(const std::string &src, const std::string &dir,
                          LutEntry *out, std::string *warning, std::string *err)
{
  if (warning)
    warning->clear();
  const auto fail = [&](const std::string &m)
  {
    if (err)
      *err = m;
    return false;
  };
  if (fileExtLower(src) != "cube")
    return fail(baseName(src) + " is not a .cube file");

  std::vector<uint8_t> bytes;
  std::string rerr;
  if (!readLutBytes(src, &bytes, &rerr))
    return fail(rerr);
  Lut3D lut;
  LutCubeInfo info;
  if (!parseLutBytes(bytes, src, &lut, &info, &rerr))
    return fail(rerr);
  const uint64_t hash = lutHashBytes(bytes.data(), bytes.size());

  std::string base = lutNameOfFile(src);
  if (base.empty())
    base = "LUT";

  std::error_code ec;
  std::filesystem::create_directories(fsPath(dir), ec);
  if (ec)
    return fail(ec.message());

  for (int k = 1; k <= 99; ++k)
  {
    std::string name = base;
    if (k > 1)
    {
      const std::string suffix = "-" + std::to_string(k);
      name = base.substr(0, kLutNameMax - suffix.size());
      while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
      name += suffix;
    }
    const std::string path = lutPath(dir, name);
    std::error_code e2;
    if (std::filesystem::exists(fsPath(path), e2) && !e2)
    {
      std::vector<uint8_t> have;
      if (readLutBytes(path, &have, nullptr) &&
          lutHashBytes(have.data(), have.size()) == hash)
      { // the same file is already there
        if (out)
          *out = LutEntry{name, path, hash, lut.size};
        if (warning)
          *warning = lutExpectationWarning(name, lut, info);
        return true;
      }
      continue; // a different file owns this name
    }
    if (listLutFiles(dir).size() >= kLutMaxFiles)
      return fail("the LUT library is full (" + std::to_string(kLutMaxFiles) +
                  " LUTs)");
    std::string werr;
    if (!writeFileAtomic(path, bytes, &werr))
      return fail(werr);
    if (out)
      *out = LutEntry{name, path, hash, lut.size};
    if (warning)
      *warning = lutExpectationWarning(name, lut, info);
    return true;
  }
  return fail("too many LUTs named " + base);
}

// The LUTs of the library as entries (name and path; hash and size are not known until a file
// is read). Only files an edit can point at by name are listed: a file whose name has
// characters a LUT name can't hold (see sanitizeLutName) would not be found again, so it is
// left out. Natural order, at most kLutMaxFiles.
inline std::vector<LutEntry> listLutEntries(const std::string &dir)
{
  std::vector<LutEntry> out;
  for (const std::string &file : listLutFiles(dir))
  {
    std::string stem = file.substr(0, file.size() - std::string(kLutExt).size());
    const std::string name = lutNameOfFile(file);
    if (name.empty() || filmLower(lutFileStem(name)) != filmLower(stem))
      continue;
    out.push_back(LutEntry{name, dir + "/" + file, 0, 0});
  }
  return out;
}

inline bool deleteLutFile(const std::string &path)
{
  std::error_code ec;
  return std::filesystem::remove(fsPath(path), ec) && !ec;
}

// ---------------- Resolving a reference ----------------
enum class LutStatus
{
  None,        // the edit has no LUT
  Ok,          // found, same bytes as when it was chosen
  HashChanged, // found and used, but the file is not the one that was chosen
  Missing,     // no such file in the library
  Unreadable   // the file is there but can't be read or parsed
};

struct LutResolved
{
  std::shared_ptr<const Lut3D> lut; // null unless Ok / HashChanged
  LutStatus status = LutStatus::None;
  uint64_t hash = 0; // hash of the file's bytes as it is now (what a new LutRef should store); 0 if none
};

namespace lutlib_detail
{
  struct CacheEntry
  {
    std::string key;
    std::shared_ptr<const Lut3D> lut; // null: a remembered failure
    uint64_t hash = 0;
    LutStatus fail = LutStatus::None; // Missing / Unreadable when lut is null
  };
  struct Cache
  {
    std::mutex m;
    std::vector<CacheEntry> v; // oldest first
  };
  inline Cache &cache()
  {
    static Cache c;
    return c;
  }
  inline std::string keyOf(const std::string &dir, const std::string &name)
  {
    return filmLower(dir) + "|" + filmLower(lutFileStem(name));
  }
  inline void insert(Cache &c, CacheEntry e)
  {
    for (auto &x : c.v)
      if (x.key == e.key)
      {
        x = std::move(e);
        return;
      }
    if (c.v.size() >= kLutCacheEntries)
      c.v.erase(c.v.begin());
    c.v.push_back(std::move(e));
  }
  inline LutResolved resultOf(const CacheEntry &e, const LutRef &ref)
  {
    LutResolved r;
    if (!e.lut)
    {
      r.status = e.fail;
      return r;
    }
    r.lut = e.lut;
    r.hash = e.hash;
    r.status = (ref.hash == 0 || ref.hash == e.hash) ? LutStatus::Ok
                                                     : LutStatus::HashChanged;
    return r;
  }
} // namespace lutlib_detail

// The table `ref` points at, from the cache or (the first time) from the
// library folder. Safe to call from any thread.
inline LutResolved acquireLut(const LutRef &ref)
{
  using namespace lutlib_detail;
  if (ref.empty())
    return {};
  const std::string dir = lutDir();
  const std::string key = keyOf(dir, ref.name);
  Cache &c = cache();
  {
    std::lock_guard lk(c.m);
    for (const CacheEntry &e : c.v)
      if (e.key == key)
        return resultOf(e, ref);
  }
  // Not cached: load it outside the lock (parsing a big cube takes a moment).
  CacheEntry e;
  e.key = key;
  const std::string path = lutPath(dir, ref.name);
  std::error_code ec;
  if (!std::filesystem::exists(fsPath(path), ec) || ec)
    e.fail = LutStatus::Missing;
  else
  {
    auto lut = std::make_shared<Lut3D>();
    uint64_t h = 0;
    if (loadLutFile(path, lut.get(), nullptr, &h, nullptr))
    {
      e.lut = std::move(lut);
      e.hash = h;
    }
    else
      e.fail = LutStatus::Unreadable;
  }
  std::lock_guard lk(c.m);
  for (const CacheEntry &x : c.v) // another thread got there first: use its entry
    if (x.key == key)
      return resultOf(x, ref);
  const LutResolved r = resultOf(e, ref);
  insert(c, std::move(e));
  return r;
}

inline void clearLutCache()
{
  lutlib_detail::Cache &c = lutlib_detail::cache();
  std::lock_guard lk(c.m);
  c.v.clear();
}

// Tests: makes `name` resolve to `lut` (with file hash `hash`) without a file.
inline void lutCachePut(const std::string &name, std::shared_ptr<const Lut3D> lut,
                        uint64_t hash)
{
  lutlib_detail::Cache &c = lutlib_detail::cache();
  lutlib_detail::CacheEntry e;
  e.key = lutlib_detail::keyOf(lutDir(), name);
  e.lut = std::move(lut);
  e.hash = hash;
  std::lock_guard lk(c.m);
  lutlib_detail::insert(c, std::move(e));
}