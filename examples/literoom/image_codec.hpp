// image_codec.hpp - Encoding a CanvasImage as PNG or JPEG: libpng / libjpeg on
// Linux, WIC on Windows (the libraries liteui.hpp already pulls in for its
// decoders, which is why this header needs liteui.hpp).
//   encodePngFile     RGBA, straight alpha, written to a file
//   encodeJpegMemory  RGB, alpha flattened onto white, encoded to memory so the
//                     source photo's metadata can be spliced in (image_meta.hpp)
//   flattenRow        the one pure piece: a row of RGBA as packed RGB / BGR
// Everything that touches the editor (file pickers, state, status line) stays
// in main.cpp's exportImage / saveJpegFile.
#pragma once
#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "liteui.hpp"

// ---------------- PNG export ----------------
// liteui.hpp only ships image *decoding* (liteui_image::decodeFile) and no
// encoder, so exporting is a small amount of platform code, using the same
// libraries liteui.hpp already links in for us (libpng on Linux via
// CMakeLists' PkgConfig::PNG, WIC on Windows).

#if defined(_WIN32)

inline bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut)
{
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
    if (FAILED(stream->InitializeFromFilename(toWide(path).c_str(),
                                              GENERIC_WRITE)))
      break;
    if (FAILED(
            factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
      break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)))
      break;
    if (FAILED(frame->Initialize(nullptr)))
      break;
    if (FAILED(frame->SetSize(static_cast<UINT>(img.width),
                              static_cast<UINT>(img.height))))
      break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&fmt)))
      break;

    const BYTE *src = img.pixels.data();
    std::vector<BYTE> swizzled;

    if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
    {
      // Encoder substituted BGRA — swap R and B.
      swizzled.resize(img.pixels.size());
      for (size_t i = 0; i < img.pixels.size(); i += 4)
      {
        swizzled[i + 0] = img.pixels[i + 2]; // B
        swizzled[i + 1] = img.pixels[i + 1]; // G
        swizzled[i + 2] = img.pixels[i + 0]; // R
        swizzled[i + 3] = img.pixels[i + 3]; // A
      }
      src = swizzled.data();
    }
    else if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppRGBA))
    {
      break; // some other format we don't handle
    }

    if (FAILED(frame->WritePixels(
            static_cast<UINT>(img.height), static_cast<UINT>(img.width) * 4,
            static_cast<UINT>(img.pixels.size()), const_cast<BYTE *>(src))))
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
    *errorOut = "WIC: PNG encode failed";
  return ok;
}

#else // Linux

inline bool encodePngFile(const std::string &path, const CanvasImage &img,
                          std::string *errorOut)
{
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp)
  {
    if (errorOut)
      *errorOut = "failed to open file for writing";
    return false;
  }

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
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

  if (setjmp(png_jmpbuf(png)))
  {
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    if (errorOut)
      *errorOut = "libpng: error during write";
    return false;
  }

  png_init_io(png, fp);
  png_set_IHDR(png, info, static_cast<png_uint_32>(img.width),
               static_cast<png_uint_32>(img.height), 8, PNG_COLOR_TYPE_RGBA,
               PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT,
               PNG_FILTER_TYPE_DEFAULT);
  png_write_info(png, info);

  std::vector<png_bytep> rows(img.height);
  for (int y = 0; y < img.height; ++y)
    rows[y] = const_cast<png_bytep>(img.pixels.data() +
                                    static_cast<size_t>(y) * img.width * 4);
  png_write_image(png, rows.data());
  png_write_end(png, nullptr);

  png_destroy_write_struct(&png, &info);
  fclose(fp);
  return true;
}

#endif

// ---------------- JPEG export ----------------
// Same split as PNG above: WIC on Windows, libjpeg on Linux (the library
// liteui.hpp already links for decoding). Both encode to memory so the
// source photo's EXIF block and ICC profile can be spliced in before the file
// is written (see exif.hpp). JPEG has no alpha, so transparent areas (the
// corners left by Straighten) are flattened onto white.

constexpr int kJpegQualityDefault = 92;
constexpr int kJpegMaxSide = 65500; // the format's limit is 65535

// One row of the picture as packed 8-bit RGB (or BGR), alpha over white.
inline void flattenRow(const CanvasImage &img, int y, uint8_t *dst, bool bgr)
{
  const uint8_t *s = &img.pixels[static_cast<size_t>(y) * img.width * 4];
  for (int x = 0; x < img.width; ++x, s += 4, dst += 3)
  {
    const int a = s[3];
    int c[3];
    for (int k = 0; k < 3; ++k)
      c[k] = a == 255 ? s[k] : (s[k] * a + 255 * (255 - a) + 127) / 255;
    dst[0] = static_cast<uint8_t>(bgr ? c[2] : c[0]);
    dst[1] = static_cast<uint8_t>(c[1]);
    dst[2] = static_cast<uint8_t>(bgr ? c[0] : c[2]);
  }
}

// 4:4:4 (no chroma subsampling) from quality 90 up: keeps colored edges crisp.
inline bool jpegFullChroma(int quality) { return quality >= 90; }

#if defined(_WIN32)

inline bool encodeJpegMemory(const CanvasImage &img, int quality,
                             std::vector<uint8_t> *out, std::string *errorOut)
{
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

  IStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  IPropertyBag2 *props = nullptr;
  bool ok = false;
  do
  {
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
      break;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr,
                                      &encoder)))
      break;
    if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache)))
      break;
    if (FAILED(encoder->CreateNewFrame(&frame, &props)))
      break;

    {
      PROPBAG2 opt;
      ZeroMemory(&opt, sizeof opt);
      opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
      VARIANT v;
      ZeroMemory(&v, sizeof v);
      v.vt = VT_R4;
      v.fltVal = static_cast<float>(quality) / 100.0f;
      if (FAILED(props->Write(1, &opt, &v)))
        break;
    }
    if (jpegFullChroma(quality))
    { // best effort: an encoder without this option just keeps its default
      PROPBAG2 opt;
      ZeroMemory(&opt, sizeof opt);
      opt.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
      VARIANT v;
      ZeroMemory(&v, sizeof v);
      v.vt = VT_UI1;
      v.bVal = 3; // WICJpegYCrCbSubsampling444
      props->Write(1, &opt, &v);
    }

    if (FAILED(frame->Initialize(props)))
      break;
    if (FAILED(frame->SetSize(static_cast<UINT>(img.width),
                              static_cast<UINT>(img.height))))
      break;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt)) ||
        !IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR))
      break;

    constexpr int kChunkRows = 64;
    const UINT stride = static_cast<UINT>(img.width) * 3;
    std::vector<uint8_t> rows(static_cast<size_t>(stride) * kChunkRows);
    bool wrote = true;
    for (int y = 0; y < img.height && wrote; y += kChunkRows)
    {
      const int n = std::min(kChunkRows, img.height - y);
      for (int r = 0; r < n; ++r)
        flattenRow(img, y + r, &rows[static_cast<size_t>(r) * stride], true);
      wrote = SUCCEEDED(frame->WritePixels(static_cast<UINT>(n), stride,
                                           static_cast<UINT>(stride * n),
                                           rows.data()));
    }
    if (!wrote)
      break;
    if (FAILED(frame->Commit()) || FAILED(encoder->Commit()))
      break;

    STATSTG st;
    HGLOBAL hg = nullptr;
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME)) ||
        FAILED(GetHGlobalFromStream(stream, &hg)) || !hg)
      break;
    const void *p = GlobalLock(hg);
    if (!p)
      break;
    out->assign(static_cast<const uint8_t *>(p),
                static_cast<const uint8_t *>(p) + st.cbSize.QuadPart);
    GlobalUnlock(hg);
    ok = true;
  } while (false);

  if (props)
    props->Release();
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
    *errorOut = "WIC: JPEG encode failed";
  return ok;
}

#else // Linux

struct JpegEncErr
{
  jpeg_error_mgr pub; // must be first: libjpeg hands back a jpeg_error_mgr*
  jmp_buf jump;
  char msg[JMSG_LENGTH_MAX] = {0};
};
inline void jpegEncErrExit(j_common_ptr c)
{
  auto *e = reinterpret_cast<JpegEncErr *>(c->err);
  (*c->err->format_message)(c, e->msg);
  longjmp(e->jump, 1); // libjpeg's default would exit() the whole app
}

inline bool encodeJpegMemory(const CanvasImage &img, int quality,
                             std::vector<uint8_t> *out, std::string *errorOut)
{
  jpeg_compress_struct cinfo;
  JpegEncErr jerr;
  struct
  {
    unsigned char *buf = nullptr;
    unsigned long size = 0;
  } dest;
  std::vector<uint8_t> row(static_cast<size_t>(img.width) * 3);

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = jpegEncErrExit;
  if (setjmp(jerr.jump))
  {
    jpeg_destroy_compress(&cinfo);
    std::free(dest.buf);
    if (errorOut)
      *errorOut = std::string("libjpeg: ") + jerr.msg;
    return false;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &dest.buf, &dest.size);
  cinfo.image_width = static_cast<JDIMENSION>(img.width);
  cinfo.image_height = static_cast<JDIMENSION>(img.height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  if (jpegFullChroma(quality))
  {
    cinfo.comp_info[0].h_samp_factor = 1;
    cinfo.comp_info[0].v_samp_factor = 1;
  }
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height)
  {
    flattenRow(img, static_cast<int>(cinfo.next_scanline), row.data(), false);
    JSAMPROW rp = row.data();
    jpeg_write_scanlines(&cinfo, &rp, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  out->assign(dest.buf, dest.buf + dest.size);
  std::free(dest.buf);
  return true;
}

#endif
