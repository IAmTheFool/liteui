// export_render.hpp - The pixel half of exporting one picture: from the decoded ORIGINAL to the rendered
// picture the encoders write. A pure function of its inputs (no files, no editor state, no sidecar), so it
// is tested without any of those (tests/cases/export_render.inc); exportPicture (export_worker.hpp) does the
// file work around it.
//
// The chain is the geometry worker's: orient -> straighten -> crop, from the untouched original, then the
// render. The original is either 8-bit (a JPEG, an 8-bit PNG: CanvasImage) or 16-bit (a 16-bit PNG:
// Image16) and the picture stays in that type until it is rendered; the result is 8-bit (JPEG / 8-bit PNG)
// or, when `want16` is set, 16-bit (16-bit PNG). An 8-bit original can be exported as 16-bit as well: the
// float pipeline then keeps its extra precision, so a heavy edit does not show steps in smooth gradients.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "core/geometry.hpp"
#include "core/image16.hpp"
#include "core/raster_ops.hpp"
#include "edit/render_params.hpp"
#include "liteui.hpp"
#include "render/render_core.hpp"

// What the encoders get: exactly one of the two is set.
struct ExportPixels
{
  std::shared_ptr<CanvasImage> img8;
  std::shared_ptr<Image16> img16;
  bool is16() const { return img16 != nullptr; }
  int width() const { return img16 ? img16->width : (img8 ? img8->width : 0); }
  int height() const { return img16 ? img16->height : (img8 ? img8->height : 0); }
  // Scaled so the long side is `longEdge` (see fitLongEdge: never enlarges; < 1 changes nothing).
  void resizeToLongEdge(int longEdge)
  {
    if (img16)
      img16 = ::resizeToLongEdge(img16, longEdge);
    else if (img8)
      img8 = ::resizeToLongEdge(img8, longEdge);
  }
};

// The geometry edits of one picture (AdjustmentSet::orient / angle / expand / crop).
struct ExportGeometry
{
  Orient orient;
  float angle = 0.0f;
  bool expand = false;
  CropRect crop;
};

namespace export_render_detail
{
  // orient -> straighten -> crop. Null = superseded (rotateArbitrary saw `latest` != `gen`).
  template <class Img>
  inline std::shared_ptr<Img> buildSource(std::shared_ptr<Img> original, const ExportGeometry &g,
                                          const std::atomic<uint64_t> *latest, uint64_t gen)
  {
    std::shared_ptr<Img> oriented = orientImage(original, g.orient);
    original.reset(); // keep the peak low: only what the next step needs stays
    std::shared_ptr<Img> rotated = oriented;
    if (g.angle != 0.0f)
    {
      rotated = rotateArbitrary(*oriented, g.angle, g.expand, latest, gen);
      if (!rotated)
        return nullptr;
    }
    oriented.reset();
    return g.crop.empty() ? rotated : cropImage(*rotated, g.crop);
  }
} // namespace export_render_detail

// Renders an 8-bit original. False = superseded (nothing in `out`).
inline bool renderForExport(std::shared_ptr<CanvasImage> original, const ExportGeometry &g,
                            const RenderParams &rp, bool want16,
                            const std::atomic<uint64_t> *latest, uint64_t gen, ExportPixels *out)
{
  std::shared_ptr<CanvasImage> source =
      export_render_detail::buildSource(std::move(original), g, latest, gen);
  if (!source)
    return false;
  *out = ExportPixels{};
  if (want16)
    out->img16 = renderToImage16(*source, rp, latest, gen);
  else
    out->img8 = renderWith(*source, rp, latest, gen);
  return out->img16 || out->img8;
}

// Renders a 16-bit original. False = superseded.
inline bool renderForExport(std::shared_ptr<Image16> original, const ExportGeometry &g,
                            const RenderParams &rp, bool want16,
                            const std::atomic<uint64_t> *latest, uint64_t gen, ExportPixels *out)
{
  std::shared_ptr<Image16> source =
      export_render_detail::buildSource(std::move(original), g, latest, gen);
  if (!source)
    return false;
  *out = ExportPixels{};
  if (want16)
    out->img16 = renderToImage16(*source, nullptr, rp, latest, gen);
  else
    out->img8 = renderToDisplay(*source, nullptr, rp, latest, gen);
  return out->img16 || out->img8;
}
