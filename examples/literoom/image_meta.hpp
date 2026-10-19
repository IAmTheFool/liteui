// image_meta.hpp - What an image FILE says about itself, read from disk:
//   * exifOrientationOf   the camera rotation of a JPEG, as the Orient that makes
//                         the picture upright;
//   * embedSourceMetadata carry the EXIF block and ICC profile of a source JPEG
//                         into a freshly encoded one.
// Glue between file_utils.hpp (reading a file's first bytes), exif.hpp (the
// byte-level work) and geometry.hpp (Orient). No liteui and no editor state, so
// both functions can be tested with synthetic JPEG files in a temp folder.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "exif.hpp"
#include "file_utils.hpp"
#include "geometry.hpp"

// How much of the source file is searched for its EXIF / ICC header segments
// when exporting (exifOrientationOf looks at the first 256 KiB only).
constexpr size_t kMetaPrefixBytes = 8u << 20;

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
// Returns true when the source is a JPEG (by extension) whose header could be
// read, i.e. when exif::embed ran; `*jpeg` is then rewritten, and stays
// byte-identical if the source had no EXIF or ICC data. Returns false, with
// `*jpeg` untouched, for a source that is not a .jpg / .jpeg, is missing or
// empty, or does not start like a JPEG.
inline bool embedSourceMetadata(std::vector<uint8_t> *jpeg,
                                const std::string &sourcePath, int width,
                                int height)
{
  std::vector<uint8_t> head;
  exif::JpegMeta meta;
  if (!isJpegPath(sourcePath) ||
      !readFilePrefix(sourcePath, kMetaPrefixBytes, &head) ||
      !exif::readJpegMeta(head.data(), head.size(), &meta))
    return false;
  *jpeg = exif::embed(*jpeg, meta, width, height);
  return true;
}
