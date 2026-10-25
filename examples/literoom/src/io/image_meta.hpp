// image_meta.hpp - What an image FILE says about itself, read from disk:
//   * exifOrientationOf   the camera rotation of a JPEG, as the Orient that makes
//                         the picture upright;
//   * embedSourceMetadata carry the EXIF block and ICC profile of a source JPEG
//                         into a freshly encoded one;
//   * photoInfoOf         the camera data of a JPEG (make, model, lens, ISO,
//                         shutter, ...) for the Info panel.
// Glue between file_utils.hpp (reading a file's first bytes), exif.hpp (the
// byte-level work) and geometry.hpp (Orient). No liteui and no editor state, so
// all three functions can be tested with synthetic JPEG files in a temp folder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/exif.hpp"
#include "io/file_utils.hpp"
#include "io/virtual_copy.hpp"
#include "core/geometry.hpp"

// How much of the source file is searched for its EXIF / ICC header segments
// when exporting (exifOrientationOf looks at the first 256 KiB only).
constexpr size_t kMetaPrefixBytes = 8u << 20;

// How much of a file is searched for the camera data of the Info panel (the
// same window exifOrientationOf uses).
constexpr size_t kInfoPrefixBytes = 256u << 10;

// The rotation a camera recorded in a JPEG's EXIF data, as the Orient that
// makes the picture upright (identity for anything else). The decoders hand
// back the stored pixels, so a portrait shot would otherwise open on its
// side. It is applied once, as the file is opened: the "original" the editor
// (and its saved edits and masks) work on is the upright picture.
inline Orient exifOrientationOf(const std::string &path)
{
  Orient o;
  if (!isJpegPath(path))
    return o;
  std::vector<uint8_t> head;
  exif::JpegMeta meta;
  if (!readFilePrefix(path, 256u << 10, &head) ||
      !exif::readJpegMeta(head.data(), head.size(), &meta))
    return o;
  const exif::Transform t = exif::orientationToTransform(meta.orientation);
  o.rot = t.rot;
  o.flip = t.flip;
  return o;
}

// Carries the EXIF block and ICC profile of the JPEG at `sourcePath` into
// `*jpeg`, a freshly encoded JPEG of width x height pixels: the orientation is
// reset to "upright" (the pixels already are), the embedded thumbnail is erased
// and the picture size fields are updated (see exif::embed).
//
// With `removeGps`, the GPS location is cut out of the EXIF block first (see
// exif::stripGps); everything else in it is kept.
//
// Returns true when the source is a JPEG (by extension) whose header could be
// read, i.e. when exif::embed ran; `*jpeg` is then rewritten, and stays
// byte-identical if the source had no EXIF or ICC data. Returns false, with
// `*jpeg` untouched, for a source that is not a .jpg / .jpeg, is missing or
// empty, or does not start like a JPEG.
inline bool embedSourceMetadata(std::vector<uint8_t> *jpeg,
                                const std::string &sourcePath, int width,
                                int height, bool removeGps = false)
{
  std::vector<uint8_t> head;
  exif::JpegMeta meta;
  if (!isJpegPath(sourcePath) ||
      !readFilePrefix(sourcePath, kMetaPrefixBytes, &head) ||
      !exif::readJpegMeta(head.data(), head.size(), &meta))
    return false;
  if (removeGps)
    exif::stripGps(&meta);
  *jpeg = exif::embed(*jpeg, meta, width, height);
  return true;
}

// The camera data of the picture at `path` for the Info panel: make, model,
// lens, date taken, ISO, shutter, aperture and focal length (see
// exif::PhotoInfo). `path` may be a virtual copy's path ("photo.jpg|vc2"): the
// data is read from the real picture file, since a copy has no file of its own.
//
// Returns true when the file is a .jpg / .jpeg that could be read and starts
// like a JPEG; `*out` is then filled in, and may still be empty() when the file
// has no EXIF. Returns false, with `*out` reset to empty, for anything else
// (another extension, a missing or empty file, not a JPEG). Only the first
// kInfoPrefixBytes of the file are searched. GPS is never read.
inline bool photoInfoOf(const std::string &path, exif::PhotoInfo *out)
{
  *out = exif::PhotoInfo{};
  const std::string file = vcImageOf(path);
  std::vector<uint8_t> head;
  if (!isJpegPath(file) || !readFilePrefix(file, kInfoPrefixBytes, &head))
    return false;
  return exif::readPhotoInfo(head.data(), head.size(), out);
}