// sam_segmenter.hpp - Segment-Anything-style segmentation via ONNX Runtime.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#if defined(LITEROOM_ONNX)
#include <memory>
#include <onnxruntime_cxx_api.h>
#endif

namespace sam {

struct Point {
  float x = 0, y = 0; // fraction of the picture's width / height
  bool fg = true;
};

// What encode() feeds the encoder, as a plain function so it can be tested
// without ONNX Runtime or a model. Returns an empty result (nw == nh == 0) for
// a null picture or a size <= 0.
inline constexpr int kInputSize = 1024; // SAM's longest-side input size

struct Prepared {
  std::vector<float> data;
  int nw = 0, nh = 0; // the picture as resized (long side kInputSize)
  bool ok() const { return nw > 0 && nh > 0; }
};

//   rank 4: normalized NCHW 1x3xkInputSize x kInputSize, zero-padded at the
//           bottom / right (data.size() == 3 * kInputSize^2);
//   anything else: raw 0-255 RGB, HWC at nh x nw (data.size() == nh * nw * 3).
// Alpha is ignored.
inline Prepared preprocess(const uint8_t *rgba, int w, int h, int rank) {
  Prepared out;
  if (!rgba || w <= 0 || h <= 0)
    return out;
  const double scale = static_cast<double>(kInputSize) / std::max(w, h);
  const int nw = std::max(1, static_cast<int>(std::lround(w * scale)));
  const int nh = std::max(1, static_cast<int>(std::lround(h * scale)));

  // Resize (box average when shrinking, nearest when enlarging). Rank 4:
  // normalize, CHW, zero-padded to kInputSize x kInputSize at the bottom / right.
  // Rank 3: raw 0-255 HWC at nh x nw (the model preprocesses itself).
  const size_t plane = static_cast<size_t>(kInputSize) * kInputSize;
  std::vector<float> in(rank == 4
                            ? 3 * plane
                            : static_cast<size_t>(nw) * nh * 3,
                        0.0f);
  static const float mean[3] = {123.675f, 116.28f, 103.53f};
  static const float inv[3] = {1.0f / 58.395f, 1.0f / 57.12f, 1.0f / 57.375f};
  for (int y = 0; y < nh; ++y) {
    const double sy0 = static_cast<double>(y) * h / nh;
    const double sy1 = static_cast<double>(y + 1) * h / nh;
    const int y0 = std::min(h - 1, static_cast<int>(std::floor(sy0)));
    const int y1 = std::min(h, std::max(y0 + 1, static_cast<int>(std::ceil(sy1))));
    for (int x = 0; x < nw; ++x) {
      const double sx0 = static_cast<double>(x) * w / nw;
      const double sx1 = static_cast<double>(x + 1) * w / nw;
      const int x0 = std::min(w - 1, static_cast<int>(std::floor(sx0)));
      const int x1 = std::min(w, std::max(x0 + 1, static_cast<int>(std::ceil(sx1))));
      float sum[3] = {0, 0, 0};
      for (int yy = y0; yy < y1; ++yy) {
        const uint8_t *row = rgba + (static_cast<size_t>(yy) * w + x0) * 4;
        for (int xx = x0; xx < x1; ++xx, row += 4) {
          sum[0] += row[0];
          sum[1] += row[1];
          sum[2] += row[2];
        }
      }
      const float cnt = static_cast<float>((y1 - y0) * (x1 - x0));
      const float v[3] = {sum[0] / cnt, sum[1] / cnt, sum[2] / cnt};
      if (rank == 4) {
        for (int c = 0; c < 3; ++c)
          in[c * plane + static_cast<size_t>(y) * kInputSize + x] =
              (v[c] - mean[c]) * inv[c];
      } else {
        float *o = &in[(static_cast<size_t>(y) * nw + x) * 3];
        o[0] = v[0];
        o[1] = v[1];
        o[2] = v[2];
      }
    }
  }
  out.data = std::move(in);
  out.nw = nw;
  out.nh = nh;
  return out;
}

#if defined(LITEROOM_ONNX)

// One image encoded at a time (the embedding is kept), any number of cheap
// decodes against it. NOT thread-safe: use from one thread (the worker).
//
// Two encoder layouts are supported (detected from the model's input rank):
//   rank 4: NCHW 1x3x1024x1024, already normalized and zero-padded here;
//   rank 3: HWC raw 0-255 RGB at the resized size (nh x nw x 3); the model
//           normalizes and pads itself (samexporter-style export).
class Segmenter {
public:
  static constexpr int kInput = kInputSize;

  Segmenter() : env_(ORT_LOGGING_LEVEL_WARNING, "literoom") {}

  bool loaded() const { return enc_ && dec_; }

  bool load(const std::string &encoderPath, const std::string &decoderPath,
            std::string *err) {
    try {
      Ort::SessionOptions so;
      so.SetIntraOpNumThreads(
          static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
      so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
      enc_ = std::make_unique<Ort::Session>(env_, toPath(encoderPath).c_str(), so);
      dec_ = std::make_unique<Ort::Session>(env_, toPath(decoderPath).c_str(), so);

      const auto encIn = names(*enc_, true), encOut = names(*enc_, false);
      const auto decIn = names(*dec_, true), decOut = names(*dec_, false);
      if (encIn.empty() || encOut.empty())
        return fail(err, "encoder has no inputs or outputs");
      for (const char *need :
           {"image_embeddings", "point_coords", "point_labels", "mask_input",
            "has_mask_input", "orig_im_size"})
        if (std::find(decIn.begin(), decIn.end(), need) == decIn.end())
          return fail(err, std::string("decoder lacks input '") + need +
                               "'; it has: " + join(decIn));
      if (std::find(decOut.begin(), decOut.end(), "masks") == decOut.end())
        return fail(err, "decoder lacks output 'masks'; it has: " + join(decOut));
      hasIou_ = std::find(decOut.begin(), decOut.end(), "iou_predictions") !=
                decOut.end();
      const auto shape =
          enc_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      if (shape.size() != 4 && shape.size() != 3)
        return fail(err, "encoder input rank " + std::to_string(shape.size()) +
                             " is neither NCHW (4) nor HWC (3)");
      encRank_ = static_cast<int>(shape.size());
      encIn_ = encIn[0];
      encOut_ = encOut[0];
      std::fprintf(stderr,
                   "[sam] encoder '%s' (rank %d) -> '%s'; decoder in: %s; out: %s\n",
                   encIn_.c_str(), encRank_, encOut_.c_str(),
                   join(decIn).c_str(), join(decOut).c_str());
      return true;
    } catch (const Ort::Exception &e) {
      enc_.reset();
      dec_.reset();
      return fail(err, e.what());
    }
  }

  // Encodes an RGBA8 picture (alpha ignored). Slow (~0.5-2 s on CPU), so the
  // caller should do it once per opened image.
  bool encode(const uint8_t *rgba, int w, int h, std::string *err) {
    embedding_.clear();
    if (!loaded() || !rgba || w <= 0 || h <= 0)
      return fail(err, "segmenter not ready");
    Prepared p = preprocess(rgba, w, h, encRank_);
    if (!p.ok())
      return fail(err, "segmenter not ready");
    nw_ = p.nw;
    nh_ = p.nh;
    std::vector<float> in = std::move(p.data);
    try {
      Ort::MemoryInfo mem =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const int64_t shape4[4] = {1, 3, kInput, kInput};
      const int64_t shape3[3] = {nh_, nw_, 3};
      Ort::Value t =
          encRank_ == 4
              ? Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape4, 4)
              : Ort::Value::CreateTensor<float>(mem, in.data(), in.size(), shape3, 3);
      const char *inN[] = {encIn_.c_str()}, *outN[] = {encOut_.c_str()};
      auto out = enc_->Run(Ort::RunOptions{nullptr}, inN, &t, 1, outN, 1);
      auto info = out[0].GetTensorTypeAndShapeInfo();
      const float *p = out[0].GetTensorData<float>();
      embedding_.assign(p, p + info.GetElementCount());
      embShape_ = info.GetShape();
      return true;
    } catch (const Ort::Exception &e) {
      embedding_.clear();
      return fail(err, e.what());
    }
  }

  bool encoded() const { return !embedding_.empty(); }

  // Logits (>0 = inside) at the encoded picture's aspect, long side kInput:
  // *outW x *outH. Cheap (tens of ms).
  bool decode(const std::vector<Point> &pts, std::vector<float> *logits,
              int *outW, int *outH, std::string *err) {
    if (!encoded() || pts.empty())
      return fail(err, "nothing encoded / no prompts");
    const int64_t n = static_cast<int64_t>(pts.size()) + 1;
    std::vector<float> coords(static_cast<size_t>(n) * 2), labels(static_cast<size_t>(n));
    for (size_t i = 0; i < pts.size(); ++i) {
      coords[i * 2] = pts[i].x * nw_; // coordinates in the resized picture
      coords[i * 2 + 1] = pts[i].y * nh_;
      labels[i] = pts[i].fg ? 1.0f : 0.0f;
    }
    coords[(n - 1) * 2] = coords[(n - 1) * 2 + 1] = 0.0f; // SAM's padding point
    labels[static_cast<size_t>(n - 1)] = -1.0f;
    std::vector<float> maskIn(256 * 256, 0.0f);
    float hasMask[1] = {0.0f};
    float orig[2] = {static_cast<float>(nh_), static_cast<float>(nw_)};
    try {
      Ort::MemoryInfo mem =
          Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      const int64_t cS[3] = {1, n, 2}, lS[2] = {1, n}, mS[4] = {1, 1, 256, 256},
                    hS[1] = {1}, oS[1] = {2};
      std::vector<Ort::Value> ins;
      ins.push_back(Ort::Value::CreateTensor<float>(
          mem, embedding_.data(), embedding_.size(), embShape_.data(),
          embShape_.size()));
      ins.push_back(Ort::Value::CreateTensor<float>(mem, coords.data(), coords.size(), cS, 3));
      ins.push_back(Ort::Value::CreateTensor<float>(mem, labels.data(), labels.size(), lS, 2));
      ins.push_back(Ort::Value::CreateTensor<float>(mem, maskIn.data(), maskIn.size(), mS, 4));
      ins.push_back(Ort::Value::CreateTensor<float>(mem, hasMask, 1, hS, 1));
      ins.push_back(Ort::Value::CreateTensor<float>(mem, orig, 2, oS, 1));
      const char *inN[] = {"image_embeddings", "point_coords", "point_labels",
                           "mask_input",       "has_mask_input", "orig_im_size"};
      const char *outN[] = {"masks", "iou_predictions"};
      auto out = dec_->Run(Ort::RunOptions{nullptr}, inN, ins.data(), ins.size(),
                           outN, hasIou_ ? 2 : 1);
      const auto ms = out[0].GetTensorTypeAndShapeInfo().GetShape(); // [1,K,H,W]
      if (ms.size() != 4)
        return fail(err, "unexpected mask output rank");
      const size_t K = static_cast<size_t>(ms[1]);
      const size_t H = static_cast<size_t>(ms[2]), W = static_cast<size_t>(ms[3]);
      size_t pick = 0; // multimask export: for ONE point choose the best of 1..K-1
      if (K > 1 && pts.size() == 1 && hasIou_) {
        const float *iou = out[1].GetTensorData<float>();
        pick = 1;
        for (size_t k = 2; k < K; ++k)
          if (iou[k] > iou[pick])
            pick = k;
      }
      const float *m = out[0].GetTensorData<float>() + pick * H * W;
      logits->assign(m, m + H * W);
      *outW = static_cast<int>(W);
      *outH = static_cast<int>(H);
      return true;
    } catch (const Ort::Exception &e) {
      return fail(err, e.what());
    }
  }

private:
  static bool fail(std::string *err, std::string m) {
    if (err)
      *err = std::move(m);
    return false;
  }
  static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &x : v)
      s += (s.empty() ? "" : ", ") + x;
    return s;
  }
  static std::vector<std::string> names(Ort::Session &s, bool input) {
    Ort::AllocatorWithDefaultOptions a;
    std::vector<std::string> out;
    const size_t n = input ? s.GetInputCount() : s.GetOutputCount();
    for (size_t i = 0; i < n; ++i) {
      auto p = input ? s.GetInputNameAllocated(i, a) : s.GetOutputNameAllocated(i, a);
      out.emplace_back(p.get());
    }
    return out;
  }
#if defined(_WIN32)
  static std::wstring toPath(const std::string &s) { return toWide(s); }
#else
  static std::string toPath(const std::string &s) { return s; }
#endif

  Ort::Env env_;
  std::unique_ptr<Ort::Session> enc_, dec_;
  std::string encIn_, encOut_;
  bool hasIou_ = false;
  int encRank_ = 4; // 4: normalized NCHW 1024x1024; 3: raw HWC (model preprocesses)
  std::vector<float> embedding_;
  std::vector<int64_t> embShape_;
  int nw_ = 0, nh_ = 0; // the picture as resized for the encoder
};

#else // built without ONNX: same API, never available

class Segmenter {
public:
  bool loaded() const { return false; }
  bool load(const std::string &, const std::string &, std::string *err) {
    if (err)
      *err = "built without LITEROOM_ONNX";
    return false;
  }
  bool encode(const uint8_t *, int, int, std::string *err) { return load("", "", err); }
  bool encoded() const { return false; }
  bool decode(const std::vector<Point> &, std::vector<float> *, int *, int *,
              std::string *err) { return load("", "", err); }
};

#endif
} // namespace sam