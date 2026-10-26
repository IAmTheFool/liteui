// render_params.hpp - Everything the per-pixel pipeline needs, frozen into one small value
// (RenderParams), and the same for each mask (MaskPass). Built from the
// edit state in O(256); no pixels, no UI.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "color/color_pass.hpp"
#include "color/curves.hpp"
#include "core/adjustments.hpp"
#include "core/geometry.hpp"
#include "detail/film_fx.hpp"
#include "detail/neighbor_fx.hpp"
#include "mask/mask.hpp"

// The Filters sliders of one AdjValues, in the form makeColorPass takes. Use it
// at BOTH makeColorPass call sites: here (masks) and AdjustmentSet::buildColorPass().
inline FilterValues filterValuesFromAdj(const AdjValues &a)
{
  FilterValues f;
  f.greyscale = a[adjIndex(AdjId::Greyscale)];
  f.sepia = a[adjIndex(AdjId::Sepia)];
  f.vintage = a[adjIndex(AdjId::Vintage)];
  f.fade = a[adjIndex(AdjId::Fade)];
  f.negative = a[adjIndex(AdjId::Negative)];
  f.posterize = a[adjIndex(AdjId::Posterize)];
  f.solarize = a[adjIndex(AdjId::Solarize)];
  f.bleachBypass = a[adjIndex(AdjId::BleachBypass)];
  f.threshold = a[adjIndex(AdjId::Threshold)];
  f.crossProcess = a[adjIndex(AdjId::CrossProcess)];
  return f;
}

// The Vignette and Grain sliders of one AdjValues, in the form renderWith takes.
// Global only: buildMaskPasses never reads them, so a mask cannot carry them.
inline film_fx::Params filmFromAdj(const AdjValues &a)
{
  return film_fx::paramsFromValues(
      a[adjIndex(AdjId::Vignette)], a[adjIndex(AdjId::VignetteMidpoint)],
      a[adjIndex(AdjId::VignetteFeather)], a[adjIndex(AdjId::Grain)],
      a[adjIndex(AdjId::GrainSize)], a[adjIndex(AdjId::GrainRoughness)]);
}

// A mask frozen for the render thread: its shape and everything its sliders
// boil down to. Built on the UI thread (O(256)), copied into RenderParams.
struct MaskPass
{
  std::vector<MaskPassPart> parts; // combined left to right (MaskComposite)
  bool invert = false;
  float amount = 1.0f;                            // 0..1, multiplies the weight
  bool tone = false;                              // any Tone slider or curve non-zero (luts is valid)
  std::array<std::array<uint8_t, 256>, 3> luts{}; // R, G, B tables
  ColorPass color;
  neighbor_fx::Gains fx; // this mask's Texture / Clarity / Sharpness (+ Dehaze, Noise, Blur)
};

// Only masks that are on AND would change something are included. Shapes are
// mapped into the current picture here, once, on the UI thread.
inline std::vector<MaskPass> buildMaskPasses(const MaskSet &masks,
                                             const GeoFrame &frame)
{
  std::vector<MaskPass> out;
  for (const Mask &m : masks)
  {
    if (!m.enabled)
      continue;
    MaskPass mp;
    if (m.parts.empty())
      continue;
    for (const MaskPart &p : m.parts)
      mp.parts.push_back(MaskPassPart{maskShapeToSource(p.shape, frame), p.op});
    mp.invert = m.invert;
    mp.amount = std::clamp(m.amount, 0.0f, 100.0f) / 100.0f;
    if (mp.amount <= 0.0f)
      continue; // Amount 0: the mask does nothing
    for (const auto &spec : kAdjustmentSpecs)
      if (spec.stage == AdjustmentSpec::Stage::Tone &&
          m.adj[static_cast<size_t>(spec.id)] != 0.0f)
        mp.tone = true;
    // Curves: the master curve runs after the tone sliders, then each
    // channel's own curve on top (same order as AdjustmentSet::buildChannelLuts).
    const auto masterCurve = curveToLut(m.curves[0]);
    const bool useMaster = !curveIsIdentity(masterCurve);
    std::array<std::array<float, 256>, 3> chanCurve{};
    bool useChan[3] = {false, false, false};
    for (size_t c = 0; c < 3; ++c)
    {
      chanCurve[c] = curveToLut(m.curves[1 + c]);
      useChan[c] = !curveIsIdentity(chanCurve[c]);
      if (useChan[c])
        mp.tone = true;
    }
    if (useMaster)
      mp.tone = true;
    mp.color = makeColorPass(m.adj[static_cast<size_t>(AdjId::Temperature)],
                             m.adj[static_cast<size_t>(AdjId::Tint)],
                             m.adj[static_cast<size_t>(AdjId::Vibrance)],
                             m.adj[static_cast<size_t>(AdjId::Saturation)],
                             m.mix, m.grade,
                             m.adj[static_cast<size_t>(AdjId::Hue)],
                             m.adj[static_cast<size_t>(AdjId::ColorizeHue)],
                             m.adj[static_cast<size_t>(AdjId::ColorizeAmount)],
                             filterValuesFromAdj(m.adj));
    mp.fx = neighbor_fx::gainsFromValues(
        m.adj[static_cast<size_t>(AdjId::Texture)],
        m.adj[static_cast<size_t>(AdjId::Clarity)],
        m.adj[static_cast<size_t>(AdjId::Sharpness)],
        m.adj[static_cast<size_t>(AdjId::Dehaze)],
        m.adj[static_cast<size_t>(AdjId::NoiseLuma)],
        m.adj[static_cast<size_t>(AdjId::NoiseColor)],
        m.adj[static_cast<size_t>(AdjId::Blur)]);
    if (!mp.tone && !mp.color.active && !mp.fx.any())
      continue; // a mask with no adjustments does nothing
    if (mp.tone)
    {
      const auto base = toneLutFromValues(m.adj, useMaster ? &masterCurve : nullptr);
      for (size_t c = 0; c < 3; ++c)
      {
        if (!useChan[c])
        {
          mp.luts[c] = base;
          continue;
        }
        for (int k = 0; k < 256; ++k)
          mp.luts[c][static_cast<size_t>(k)] = static_cast<uint8_t>(std::clamp(
              static_cast<int>(std::lround(chanCurve[c][base[static_cast<size_t>(k)]])),
              0, 255));
      }
    }
    out.push_back(std::move(mp));
  }
  return out;
}

// ---------------- Render parameters ----------------
// Everything the per-pixel pipeline needs, frozen into one small value:
// three 256-entry tables (tone sliders + tone curves, per channel) and the
// float color pass. Building it is O(256) (AdjustmentSet::buildRenderParams),
// so the UI thread builds one and hands a *copy* to the render worker — the
// worker never touches the AdjustmentSet, which the UI thread keeps mutating.
struct RenderParams
{
  std::array<std::array<uint8_t, 256>, 3> luts{}; // R, G, B tables
  ColorPass color;                                // float pass, after the LUTs
  neighbor_fx::Gains fx;                          // Texture / Clarity / Sharpness / Dehaze / Noise / Blur (neighbor_fx.hpp), after the color pass
  std::vector<MaskPass> masks;                    // local adjustments, applied after all of it
  film_fx::Params film;                           // Vignette / Grain (film_fx.hpp): the very last step, after the masks
};