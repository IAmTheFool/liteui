// wide_chain.hpp - The 16-bit twin of the editor's picture chain.
//
// The editor holds a chain of pictures: original -> oriented -> rotated (straightened) -> source
// (cropped), built by orientImage / rotateArbitrary / cropImage, all RGBA8. A picture that really has
// more than 8 bits (a 16-bit PNG) also gets a twin of every stage as Image16, built from the 16-bit
// original by the SAME operations with the same arguments, so each twin stage has exactly the size of its
// 8-bit namesake. The 8-bit chain stays what every tool of the editor measures against (the histogram,
// scopes, white-balance picker, Auto Tone, the mask tools). The twin only feeds the render: the
// full-resolution render and the drag preview (previewBase16) start from it, and the neighbor effects
// read it directly (see render_core.hpp). A twin stage is null when the file is 8-bit.
//
// These functions mirror, line for line, what EditorState::syncSource and runGeometryJob do for the 8-bit
// chain, so the two cannot drift apart: tests/cases/wide_chain.inc checks sizes and content against it.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>

#include "core/geometry.hpp"
#include "core/image16.hpp"
#include "core/raster_ops.hpp"

namespace wide_chain
{
  using Wide = std::shared_ptr<Image16>;

  // original -> oriented. Identity orientation returns `original` itself; null in, null out.
  inline Wide oriented(const Wide &original, const Orient &o)
  {
    return original ? orientImage(original, o) : nullptr;
  }

  // oriented -> rotated. An angle of 0 returns `oriented` itself (no copy); null in, null out. With
  // `latest` / `gen` it can be superseded, which gives null from a NON-null input: callers that need to
  // tell the two apart check their input first (runGeometryJob does).
  inline Wide rotated(const Wide &oriented, float angle, bool expand,
                      const std::atomic<uint64_t> *latest = nullptr, uint64_t gen = 0)
  {
    if (!oriented)
      return nullptr;
    return angle == 0.0f ? oriented : rotateArbitrary(*oriented, angle, expand, latest, gen);
  }

  // rotated -> source. An empty crop returns `rotated` itself; null in, null out.
  inline Wide cropped(const Wide &rotated, const CropRect &crop)
  {
    if (!rotated)
      return nullptr;
    return crop.empty() ? rotated : cropImage(*rotated, crop);
  }
} // namespace wide_chain
