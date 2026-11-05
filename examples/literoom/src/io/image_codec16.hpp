// image_codec16.hpp - Reading and writing 16-bit pictures (Image16): libpng on Linux, WIC on Windows,
// the libraries liteui.hpp already pulls in (which is why this header needs liteui.hpp).
//   parsePngHeader / readPngHeader   the size, bit depth and color type of a PNG, from its first 33 bytes
//   decodeImage16                    any picture liteui can read -> Image16. A 16-bit PNG is read at full
//                                    precision; everything else (JPEG, 8-bit PNG, ...) goes through
//                                    liteui's own decoder and is widened (v * 257), so it is exactly the
//                                    8-bit picture the editor shows
//   encodePng16File                  RGBA, 16 bit per channel, straight alpha, written to a file
//   (widenImage / narrowImage, CanvasImage <-> Image16, live in core/image16_convert.hpp, which needs no
//   platform code; this header does, so a translation unit that includes it must define LITEUI_IMPLEMENTATION
//   before liteui.hpp, like image_codec.hpp's users)
//
// liteui's decoders reduce every PNG to 8 bits (libpng: png_set_strip_16; WIC: 32bppRGBA), so a
// 16-bit PNG needs its own read path; that is all this adds. TIFF and RAW would be more branches of
// decodeImage16 (they need libtiff / LibRaw, which Literoom does not link yet).
#pragma once
#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#include <array>

#include "core/image16.hpp"
#include "core/image16_convert.hpp" // widenImage / narrowImage (no platform code)
#include "io/color_tag.hpp"
#include "io/file_utils.hpp"
#include "liteui.hpp"

// ---------------- PNG header ----------------
struct PngHeader
{
  uint32_t width = 0, height = 0;
  int bitDepth = 0;  // 1, 2, 4, 8 or 16 (0 = not a PNG / unreadable)
  int colorType = 0; // 0 gray, 2 RGB, 3 palette, 4 gray + alpha, 6 RGBA
  bool ok() const { return bitDepth != 0; }
};

// The IHDR of a PNG held in memory (at least its first 33 bytes). A default PngHeader (ok() false)
// for anything that is not a well-formed start of a PNG.
inline PngHeader parsePngHeader(const uint8_t *d, size_t n)
{
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  PngHeader h;
  if (!d || n < 33 || std::memcmp(d, sig, 8) != 0)
    return h;
  auto be32 = [d](size_t o)
  { return (uint32_t{d[o]} << 24) | (uint32_t{d[o + 1]} << 16) | (uint32_t{d[o + 2]} << 8) | uint32_t{d[o + 3]}; };
  if (be32(8) != 13 || std::memcmp(d + 12, "IHDR", 4) != 0)
    return h;
  const uint32_t w = be32(16), ht = be32(20);
  const int depth = d[24], ct = d[25];
  const bool depthOk = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
  const bool typeOk = ct == 0 || ct == 2 || ct == 3 || ct == 4 || ct == 6;
  if (w == 0 || ht == 0 || !depthOk || !typeOk)
    return h;
  h.width = w;
  h.height = ht;
  h.bitDepth = depth;
  h.colorType = ct;
  return h;
}

// The IHDR of the file at `path` (UTF-8); ok() false when it can't be read or is no PNG.
inline PngHeader readPngHeader(const std::string &path)
{
  std::ifstream f(fsPath(path), std::ios::binary);
  uint8_t buf[33] = {0};
  if (!f)
    return PngHeader{};
  f.read(reinterpret_cast<char *>(buf), sizeof buf);
  return parsePngHeader(buf, static_cast<size_t>(f.gcount()));
}

// PNG samples are big-endian; both libraries hand 16-bit samples over in that order unless told
// otherwise, so on a little-endian machine (every machine Literoom runs on) they are swapped.
inline bool hostIsLittleEndian()
{
  const uint16_t one = 1;
  return *reinterpret_cast<const uint8_t *>(&one) == 1;
}

// CRC-32 as PNG uses it (polynomial 0xEDB88320), continued across calls.
inline uint32_t pngCrcUpdate(uint32_t crc, const uint8_t *d, size_t n)
{
  static const std::array<uint32_t, 256> table = []
  {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i)
    {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i)
    crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

// Walks the chunks of the PNG at `path` and checks that the file is whole: signature, IHDR first,
// every chunk's data and CRC present and correct, at least one IDAT, and a closing IEND. This is
// what libpng enforces by itself; WIC does not (a cut-off or corrupted PNG can decode "fine" with
// zeroed rows), so the Windows decoder calls this first.
inline bool validatePngChunks(const std::string &path, std::string *errorOut)
{
  auto fail = [errorOut](const char *msg)
  {
    if (errorOut)
      *errorOut = msg;
    return false;
  };
  std::ifstream f(fsPath(path), std::ios::binary);
  if (!f)
    return fail("failed to open file");
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  uint8_t head[8];
  if (!f.read(reinterpret_cast<char *>(head), 8) || std::memcmp(head, sig, 8) != 0)
    return fail("not a valid PNG file");

  auto be32 = [](const uint8_t *p)
  { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]}; };
  bool first = true, sawIdat = false;
  std::vector<uint8_t> buf(64 * 1024);
  for (;;)
  {
    uint8_t ch[8];
    if (!f.read(reinterpret_cast<char *>(ch), 8))
      return fail("PNG is truncated");
    uint32_t len = be32(ch);
    if (len > 0x7FFFFFFFu)
      return fail("PNG chunk length is invalid");
    const bool isIhdr = std::memcmp(ch + 4, "IHDR", 4) == 0;
    const bool isIdat = std::memcmp(ch + 4, "IDAT", 4) == 0;
    const bool isIend = std::memcmp(ch + 4, "IEND", 4) == 0;
    if (first && !isIhdr)
      return fail("PNG has no IHDR first");
    first = false;

    uint32_t crc = pngCrcUpdate(0xFFFFFFFFu, ch + 4, 4);
    for (uint32_t left = len; left > 0;)
    {
      const size_t take = std::min<size_t>(left, buf.size());
      if (!f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(take)))
        return fail("PNG is truncated");
      crc = pngCrcUpdate(crc, buf.data(), take);
      left -= static_cast<uint32_t>(take);
    }
    uint8_t tail[4];
    if (!f.read(reinterpret_cast<char *>(tail), 4))
      return fail("PNG is truncated");
    if ((crc ^ 0xFFFFFFFFu) != be32(tail))
      return fail("PNG is corrupt (chunk CRC mismatch)");

    if (isIdat)
      sawIdat = true;
    if (isIend)
      return sawIdat ? true : fail("PNG has no image data");
  }
}

#if defined(_WIN32)

// ---------------- Windows: WIC ----------------


inline std::optional<Image16> decodePng16File(const std::string &path, std::string *errorOut)
{
  if (!validatePngChunks(path, errorOut)) // WIC accepts damaged PNGs; libpng does not
    return std::nullopt;

  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool weInitialized = coHr == S_OK;

  IWICImagingFactory *factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)); 
  if (FAILED(hr))
  {
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: CoCreateInstance failed";
    return std::nullopt;
  }

  IWICBitmapDecoder *decoder = nullptr;
  hr = factory->CreateDecoderFromFilename(toWide(path).c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(hr))
  {
    factory->Release();
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: failed to open/decode file";
    return std::nullopt;
  }

  IWICBitmapFrameDecode *frame = nullptr;
  IWICFormatConverter *converter = nullptr;
  hr = decoder->GetFrame(0, &frame);
  if (SUCCEEDED(hr))
    hr = factory->CreateFormatConverter(&converter);
  if (SUCCEEDED(hr))
    hr = converter->Initialize(frame, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
  UINT w = 0, h = 0;
  if (SUCCEEDED(hr))
    hr = converter->GetSize(&w, &h);
  std::optional<Image16> result;
  if (SUCCEEDED(hr) && w > 0 && h > 0)
  {
    Image16 img(static_cast<int>(w), static_cast<int>(h));
    hr = converter->CopyPixels(nullptr, w * 8, static_cast<UINT>(img.pixels.size() * sizeof(uint16_t)),
                               reinterpret_cast<BYTE *>(img.pixels.data()));
    if (SUCCEEDED(hr))
      result = std::move(img);
  }
  if (converter)
    converter->Release();
  if (frame)
    frame->Release();
  decoder->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();
  if (!result && errorOut)
    *errorOut = "WIC: 16-bit decode failed";
  return result;
}

inline bool encodePng16File(const std::string &path, const Image16 &img, std::string *errorOut)
{
  if (img.empty() || img.pixels.size() < img.pixelCount() * 4)
  {
    if (errorOut)
      *errorOut = "nothing to write";
    return false;
  }
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  bool weInitialized = coHr == S_OK;

  IWICImagingFactory *factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr))
  {
    if (weInitialized)
      CoUninitialize();
    if (errorOut)
      *errorOut = "WIC: CoCreateInstance failed";
    return false;
  }

  IWICStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  bool ok = false;
  do
  {
    if (FAILED(factory->CreateStream(&stream)))
      break;
    if (FAILED(stream->InitializeFromFilename(toWide(path).c_str(), GENERIC_WRITE)))
      break;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
      break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)))
      break;
    if (FAILED(frame->Initialize(nullptr)))
      break;
    if (FAILED(frame->SetSize(static_cast<UINT>(img.width), static_cast<UINT>(img.height))))
      break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat64bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt)) || !IsEqualGUID(fmt, GUID_WICPixelFormat64bppRGBA))
      break; // the encoder wants some other format: not handled
    if (FAILED(frame->WritePixels(static_cast<UINT>(img.height), static_cast<UINT>(img.width) * 8,
                                  static_cast<UINT>(img.pixels.size() * sizeof(uint16_t)),
                                  reinterpret_cast<BYTE *>(const_cast<uint16_t *>(img.pixels.data())))))
      break;
    if (FAILED(frame->Commit()))
      break;
    ok = SUCCEEDED(encoder->Commit());
  } while (false);

  if (frame)
    frame->Release();
  if (encoder)
    encoder->Release();
  if (stream)
    stream->Release();
  factory->Release();
  if (weInitialized)
    CoUninitialize();

  if (!ok && errorOut)
    *errorOut = "WIC: 16-bit PNG encode failed";
  if (ok)
  {
    // WIC always writes gAMA + cHRM chunks of its own. An export must come out of the encoder
    // untagged, so that the source's tags (or none) are the only ones it carries
    // (color_tag.hpp: nothing is ever invented). Best effort: the PNG is valid either way.
    color_tag::insertColorIntoFile(path, color_tag::PngColor{});
  }
  return ok;
}

#else // Linux: libpng

// A 16-bit PNG (any color type but palette, which PNG limits to 8 bits) read at full precision as
// straight-alpha RGBA. The setup is liteui's decodePngFile (gray -> RGB, tRNS -> alpha, an opaque
// alpha channel added when there is none), minus png_set_strip_16, plus png_set_swap.
inline std::optional<Image16> decodePng16File(const std::string &path, std::string *errorOut)
{
  FILE *fp = fopen(path.c_str(), "rb");
  if (!fp)
  {
    if (errorOut)
      *errorOut = "failed to open file";
    return std::nullopt;
  }
  png_byte header[8];
  if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8))
  {
    fclose(fp);
    if (errorOut)
      *errorOut = "not a valid PNG file";
    return std::nullopt;
  }
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info)
  {
    if (png)
      png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng init failed";
    return std::nullopt;
  }
  // The picture and the row table are declared BEFORE setjmp: libpng's error longjmp skips the
  // destructors of anything created after it, which for a big picture would leak the whole buffer.
  Image16 img;
  std::vector<png_bytep> rows;
  if (setjmp(png_jmpbuf(png)))
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng decode error";
    return std::nullopt;
  }
  png_init_io(png, fp);
  png_set_sig_bytes(png, 8);
  png_read_info(png, info);

  const png_uint_32 w = png_get_image_width(png, info);
  const png_uint_32 h = png_get_image_height(png, info);
  const int bitDepth = png_get_bit_depth(png, info);
  const int colorType = png_get_color_type(png, info);
  if (bitDepth != 16 || w == 0 || h == 0 || w > 0x7FFFFFFFu / 8u || h > 0x7FFFFFFFu)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "not a 16-bit PNG";
    return std::nullopt;
  }

  if (colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png);
  if (colorType == PNG_COLOR_TYPE_RGB || colorType == PNG_COLOR_TYPE_GRAY ||
      colorType == PNG_COLOR_TYPE_PALETTE)
    png_set_filler(png, 0xFFFF, PNG_FILLER_AFTER);
  if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_interlace_handling(png);
  if (hostIsLittleEndian())
    png_set_swap(png); // PNG stores 16-bit samples big-endian
  png_read_update_info(png, info);

  img = Image16(static_cast<int>(w), static_cast<int>(h));
  const size_t rowBytes = png_get_rowbytes(png, info);
  if (rowBytes != static_cast<size_t>(w) * 8)
  {
    png_destroy_read_struct(&png, &info, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "unexpected PNG layout";
    return std::nullopt;
  }
  rows.assign(h, nullptr);
  for (png_uint_32 y = 0; y < h; ++y)
    rows[y] = reinterpret_cast<png_bytep>(img.pixels.data() + static_cast<size_t>(y) * w * 4);
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);

  png_destroy_read_struct(&png, &info, nullptr);
  fclose(fp);
  return std::optional<Image16>(std::move(img));
}

inline bool encodePng16File(const std::string &path, const Image16 &img, std::string *errorOut)
{
  if (img.empty() || img.pixels.size() < img.pixelCount() * 4)
  {
    if (errorOut)
      *errorOut = "nothing to write";
    return false;
  }
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp)
  {
    if (errorOut)
      *errorOut = "failed to open file for writing";
    return false;
  }
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png ? png_create_info_struct(png) : nullptr;
  if (!png || !info)
  {
    if (png)
      png_destroy_write_struct(&png, nullptr);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: failed to create write struct";
    return false;
  }
  // `rows` outlives the setjmp so an error path can still reach it.
  std::vector<png_bytep> rows(static_cast<size_t>(img.height));
  if (setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: error during write";
    return false;
  }
  png_init_io(png, fp);
  png_set_IHDR(png, info, static_cast<png_uint_32>(img.width), static_cast<png_uint_32>(img.height),
               16, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);
  if (hostIsLittleEndian())
    png_set_swap(png); // our samples are host-order; PNG wants big-endian
  for (int y = 0; y < img.height; ++y)
    rows[static_cast<size_t>(y)] = reinterpret_cast<png_bytep>(
        const_cast<uint16_t *>(img.pixels.data()) + static_cast<size_t>(y) * img.width * 4);
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  fclose(fp);
  return true;
}

#endif

// ---------------- Reading any picture as Image16 ----------------
// A 16-bit PNG at full precision; anything else through liteui's decoder, widened (so the 16-bit
// pipeline starts from exactly the pixels the editor shows for it). `errorOut` says what went wrong.
inline std::optional<Image16> decodeImage16(const std::string &path, std::string *errorOut = nullptr)
{
  const PngHeader h = readPngHeader(path);
  if (h.ok() && h.bitDepth == 16)
    return decodePng16File(path, errorOut);
  std::optional<CanvasImage> eight = liteui_image::decodeFile(path, errorOut);
  if (!eight)
    return std::nullopt;
  return widenImage(*eight);
}