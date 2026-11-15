// cases/encode_helpers.hpp - test-only: writes a picture the way an export does, through liteui's encoders.
// Used by the cases that need a real PNG / JPEG file or stream to read back (image_codec, image_codec16,
// image_source, thumb_worker, export_worker). It is not part of the app: the app calls toImageBuffer and
// liteui_image::encodePng / encodeJpegMemory itself (main.cpp's exportImage, workers/export_worker.hpp).
//
// The arguments and results keep the order the old Literoom encoders had, so a case reads the same:
//   writePng8    CanvasImage -> 8-bit RGBA PNG file     (exportPngOptions(): level 3, always RGBA)
//   writePng16   Image16     -> 16-bit RGBA PNG file
//   jpegBytes    CanvasImage -> JPEG stream in memory   (alpha over white, 4:4:4 from quality 90)
// The two PNG writers also call color_tag::dropEncoderColor, as an export does, so a file comes out of
// them untagged on Windows too (WIC writes gAMA / cHRM of its own). Defining LITEUI_IMPLEMENTATION is
// the including translation unit's job (literoom_test.cpp does it once).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/image16.hpp"
#include "core/image16_convert.hpp"
#include "io/color_tag.hpp"
#include "liteui.hpp"

namespace encode_helpers
{
  inline bool writePng8(const std::string &path, const CanvasImage &img, std::string *errorOut = nullptr)
  {
    const auto buf = toImageBuffer(img);
    if (!buf)
    {
      if (errorOut)
        *errorOut = "nothing to write";
      return false;
    }
    if (!liteui_image::encodePng(path, *buf, errorOut, exportPngOptions()))
      return false;
    color_tag::dropEncoderColor(path);
    return true;
  }

  inline bool writePng16(const std::string &path, const Image16 &img, std::string *errorOut = nullptr)
  {
    const auto buf = toImageBuffer(img);
    if (!buf)
    {
      if (errorOut)
        *errorOut = "nothing to write";
      return false;
    }
    if (!liteui_image::encodePng(path, *buf, errorOut, exportPngOptions()))
      return false;
    color_tag::dropEncoderColor(path);
    return true;
  }

  inline bool jpegBytes(const CanvasImage &img, int quality, std::vector<uint8_t> *out,
                        std::string *errorOut = nullptr)
  {
    const auto buf = toImageBuffer(img);
    if (!buf)
    {
      if (errorOut)
        *errorOut = "nothing to write";
      return false;
    }
    return liteui_image::encodeJpegMemory(*out, *buf, quality, errorOut);
  }
} // namespace encode_helpers
