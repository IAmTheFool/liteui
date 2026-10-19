// file_utils.hpp - Paths, extensions, folder listing, natural sort and atomic file
// writes. Standard library only (no liteui, no editor state), so every function
// here can be tested against a temp directory.
//
// All paths are UTF-8 std::string at the edges of the program; fsPath() turns one
// into a std::filesystem::path without going through the system code page, and
// u8ToString() is the way back.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

// ---------------- Paths ----------------
inline std::filesystem::path fsPath(const std::string &utf8)
{
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

inline std::string u8ToString(const std::filesystem::path &p)
{
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

// The part of `path` after the last slash or backslash (all of it if there is none).
inline std::string baseName(const std::string &path)
{
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ---------------- Extensions ----------------
inline std::string fileExtLower(const std::string &path)
{
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return "";
  std::string e = path.substr(dot + 1);
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c)
                 { return static_cast<char>(std::tolower(c)); });
  return e;
}
inline bool isJpegPath(const std::string &path)
{
  const std::string e = fileExtLower(path);
  return e == "jpg" || e == "jpeg";
}

// ASCII lower-casing; the filmstrip's case-insensitive name key.
inline std::string filmLower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                 { return static_cast<char>(std::tolower(c)); });
  return s;
}

// ---------------- Natural order ----------------
// "IMG_2" before "IMG_10"; letters compare without case.
inline bool naturalLess(const std::string &a, const std::string &b)
{
  auto digit = [](char c)
  { return c >= '0' && c <= '9'; };
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size())
  {
    if (digit(a[i]) && digit(b[j]))
    {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && digit(a[i2]))
        ++i2;
      while (j2 < b.size() && digit(b[j2]))
        ++j2;
      size_t ia = i, jb = j; // skip leading zeros (keep one digit)
      while (ia + 1 < i2 && a[ia] == '0')
        ++ia;
      while (jb + 1 < j2 && b[jb] == '0')
        ++jb;
      const size_t la = i2 - ia, lb = j2 - jb;
      if (la != lb)
        return la < lb;
      if (const int c = a.compare(ia, la, b, jb, lb))
        return c < 0;
      i = i2;
      j = j2;
    }
    else
    {
      const int ca = std::tolower(static_cast<unsigned char>(a[i]));
      const int cb = std::tolower(static_cast<unsigned char>(b[j]));
      if (ca != cb)
        return ca < cb;
      ++i;
      ++j;
    }
  }
  return (a.size() - i) < (b.size() - j);
}

// ---------------- Reading and writing files ----------------
// The first `maxBytes` of a file (all of it if smaller). False if unreadable.
inline bool readFilePrefix(const std::string &path, size_t maxBytes,
                           std::vector<uint8_t> *out)
{
  std::ifstream f(fsPath(path), std::ios::binary);
  if (!f)
    return false;
  out->resize(maxBytes);
  f.read(reinterpret_cast<char *>(out->data()),
         static_cast<std::streamsize>(maxBytes));
  out->resize(static_cast<size_t>(f.gcount()));
  return !out->empty();
}

// Writes `bytes` to `path` via a temp file + rename, so an interrupted export
// never leaves a half-written picture over a good one.
inline bool writeFileAtomic(const std::string &path,
                            const std::vector<uint8_t> &bytes,
                            std::string *errorOut)
{
  const std::filesystem::path target = fsPath(path);
  std::filesystem::path tmp = target;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char *>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.flush();
    if (!f)
    {
      f.close();
      std::filesystem::remove(tmp, ec);
      if (errorOut)
        *errorOut = "couldn't write " + baseName(path);
      return false;
    }
  }
  std::filesystem::rename(tmp, target, ec); // replaces an existing file
  if (ec)
  {
    std::error_code ignore;
    std::filesystem::remove(tmp, ignore);
    if (errorOut)
      *errorOut = ec.message();
    return false;
  }
  return true;
}

// ---------------- Folder listing ----------------
// The JPEG / PNG files (by extension, ignoring case) that lie directly in
// `folder`, as file names (UTF-8, no directory part) in natural order. Names
// that compare equal in natural order (a.jpg / A.jpg) are ordered by plain
// comparison, so the result never depends on the directory's own order.
// Unreadable folders and entries are skipped; a missing folder gives {}.
//
// `maxFiles` caps how many names are collected, and the cap is applied BEFORE
// sorting: a folder with more matching files than that yields an arbitrary
// (directory-order) subset, sorted. This is exactly what the filmstrip always
// did; it is kept as is (file_utils_test pins it).
inline std::vector<std::string> listImageFiles(const std::string &folder,
                                               size_t maxFiles)
{
  const std::filesystem::path base = fsPath(folder);
  std::vector<std::string> names;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(base, ec), end;
       !ec && it != end && names.size() < maxFiles; it.increment(ec))
  {
    std::error_code e2;
    if (!it->is_regular_file(e2) || e2)
      continue;
    std::string name = u8ToString(it->path().filename());
    const std::string ext = fileExtLower(name);
    if (ext == "jpg" || ext == "jpeg" || ext == "png")
      names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end(),
            [](const std::string &a, const std::string &b)
            {
              return naturalLess(a, b) || (!naturalLess(b, a) && a < b);
            });
  return names;
}
