// render.hpp - The pixel pass: tone tables -> color pass -> neighbor effects ->
// masks
// -> vignette and grain, in blocks of rows that a background render can
// abandon.
//
// The pass itself is render_core.hpp (float, 8- or 16-bit in, 8- or 16-bit
// out): renderWith for the screen's RGBA8 pictures, renderToDisplay /
// renderToImage16 for a 16-bit source. This header adds the one piece that
// needs the editor's AdjustmentSet. Takes liteui's CanvasImage.
#pragma once
#include "edit/adjustment_set.hpp"
#include "render/render_core.hpp"

// Renders a copy of `original` with the adjustments in `set`.
// Non-destructive: always starts from `original`'s untouched pixels, so
// re-running after any value change never compounds onto a prior result.
// Synchronous; the full-resolution path for big images goes through
// RenderWorker instead (see updateImageFromAdjustments).
inline std::shared_ptr<CanvasImage>
renderAdjusted(const AdjustmentSet &set, const CanvasImage &original) {
  return renderWith(original, set.buildRenderParams());
}