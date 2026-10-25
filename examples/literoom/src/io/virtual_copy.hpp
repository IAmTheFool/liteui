// virtual_copy.hpp - Virtual copies: more than one set of edits for one picture
// file. Pure string and folder helpers; no UI, no editor state, no pixels.
//
// A virtual copy is a second (third, ...) sidecar next to the picture:
//   photo.jpg                      the picture
//   photo.jpg.literoom.json        the original's edits   (copy 0)
//   photo.jpg.vc2.literoom.json    virtual copy 2's edits
// Nothing is duplicated but the edits, so a copy costs a few kilobytes.
//
// The filmstrip (and every worker) identifies a picture by one string, its
// "path". A virtual copy gets its own path by appending a marker that cannot be
// part of a file name on any platform:
//   /photos/photo.jpg|vc2
// so everything that keys on the path (the filmstrip's name index, thumbnail and
// sync results, the open picture) tells copies apart with no other change. The
// few places that need the real picture file (decoding, EXIF, export) call
// vcImageOf(); sidecarFileOf() (sidecar.hpp) understands the marker itself.
#pragma once
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "io/file_utils.hpp"

constexpr const char *kSidecarExt = ".literoom.json";
constexpr int kMaxVirtualCopies = 9; // copies 1..9; a single digit in the marker
constexpr const char *kVcMark = "|vc";
static_assert(kMaxVirtualCopies >= 1 && kMaxVirtualCopies <= 9);

// The path of copy `n` of `imagePath`; the picture itself for n < 1.
inline std::string vcPath(const std::string &imagePath, int n)
{
  return n < 1 ? imagePath : imagePath + kVcMark + std::to_string(n);
}

// True if `p` is a virtual copy's path; then *image is the real picture's path
// and *n the copy number (both optional). Exactly "<image>|vc<1..9>" counts.
inline bool vcSplit(const std::string &p, std::string *image, int *n)
{
  const size_t mark = p.rfind(kVcMark);
  if (mark == std::string::npos || mark == 0 || mark + 4 != p.size())
    return false;
  if (p[mark - 1] == '/' || p[mark - 1] == '\\') // no file name before the marker
    return false;
  const char d = p[mark + 3];
  if (d < '1' || d > static_cast<char>('0' + kMaxVirtualCopies))
    return false;
  if (image)
    *image = p.substr(0, mark);
  if (n)
    *n = d - '0';
  return true;
}

// The real picture file behind a path (the path itself for a picture).
inline std::string vcImageOf(const std::string &p)
{
  std::string img;
  return vcSplit(p, &img, nullptr) ? img : p;
}

// 0 for the picture itself, else the copy number.
inline int vcCopyOf(const std::string &p)
{
  int n = 0;
  return vcSplit(p, nullptr, &n) ? n : 0;
}

// The sidecar file name of a path: "photo.jpg" -> "photo.jpg.literoom.json",
// "photo.jpg|vc2" -> "photo.jpg.vc2.literoom.json".
inline std::string vcSidecarFileOf(const std::string &p)
{
  std::string img;
  int n = 0;
  if (vcSplit(p, &img, &n))
    return img + ".vc" + std::to_string(n) + kSidecarExt;
  return p + kSidecarExt;
}

// If `fileName` (no directory) is a virtual copy's sidecar, "<image>.vc<n>" +
// the sidecar extension, gives the picture's file name and the copy number.
inline bool vcParseSidecarName(const std::string &fileName, std::string *imageName,
                               int *n)
{
  const std::string ext = kSidecarExt;
  if (fileName.size() <= ext.size() ||
      filmLower(fileName.substr(fileName.size() - ext.size())) != ext)
    return false;
  const std::string stem = fileName.substr(0, fileName.size() - ext.size());
  if (stem.size() < 5) // at least one character of name, then ".vcN"
    return false;
  const size_t k = stem.size() - 4;
  if (stem[k] != '.' || stem[k + 1] != 'v' || stem[k + 2] != 'c')
    return false;
  const char d = stem[k + 3];
  if (d < '1' || d > static_cast<char>('0' + kMaxVirtualCopies))
    return false;
  if (imageName)
    *imageName = stem.substr(0, k);
  if (n)
    *n = d - '0';
  return true;
}

// The virtual copies found in `folder`: lower-case picture file name -> copy
// numbers, ascending. The picture itself need not exist (the caller matches the
// names against the pictures it lists). At most `maxFiles` folder entries are
// looked at; a missing folder gives {}.
inline std::map<std::string, std::vector<int>> listVirtualCopies(
    const std::string &folder, size_t maxFiles)
{
  std::map<std::string, std::vector<int>> out;
  std::error_code ec;
  size_t seen = 0;
  for (std::filesystem::directory_iterator it(fsPath(folder), ec), end;
       !ec && it != end && seen < maxFiles; it.increment(ec), ++seen)
  {
    std::error_code e2;
    if (!it->is_regular_file(e2) || e2)
      continue;
    std::string image;
    int n = 0;
    if (vcParseSidecarName(u8ToString(it->path().filename()), &image, &n))
      out[filmLower(image)].push_back(n);
  }
  for (auto &kv : out)
  {
    std::sort(kv.second.begin(), kv.second.end());
    kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
  }
  return out;
}

// The lowest copy number in 1..kMaxVirtualCopies that is not in `used`; 0 if
// every number is taken.
inline int vcFreeId(const std::vector<int> &used)
{
  for (int n = 1; n <= kMaxVirtualCopies; ++n)
    if (std::find(used.begin(), used.end(), n) == used.end())
      return n;
  return 0;
}
