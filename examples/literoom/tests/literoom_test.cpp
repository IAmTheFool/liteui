
// The single translation unit of the literoom test executable (literoom_test).
// To add a test file: create cases/<name>.inc and add an #include below.
//
// Cases that need LiteUI (CanvasImage, decoders): put these two lines FIRST in
// this file (the implementation may be compiled into exactly one TU) and link
// liteui::liteui in CMakeLists.txt:
//     #define LITEUI_IMPLEMENTATION
//     #include "liteui.hpp"

#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// |x - v| <= tol (absolute). Use: CHECK(x == within(v, tol));
inline Catch::Approx within(double v, double tol) { return Catch::Approx(v).margin(tol).epsilon(0.0); }

#include "cases/adjustment_set.inc"
#include "cases/adjustments.inc"
#include "cases/auto_tone.inc"
#include "cases/color_grade.inc"
#include "cases/color_mix.inc"
#include "cases/color_pass.inc"
#include "cases/color_tag.inc"
#include "cases/curves.inc"
#include "cases/detail_layer.inc"
#include "cases/exif.inc"
#include "cases/export_render.inc"
#include "cases/export_worker.inc"
#include "cases/file_utils.inc"
#include "cases/film_fx.inc"
#include "cases/geometry.inc"
#include "cases/histogram.inc"
#include "cases/image16.inc"
#include "cases/image_codec.inc"
#include "cases/image_codec16.inc"
#include "cases/image_meta.inc"
#include "cases/image_source.inc"
#include "cases/latest_worker.inc"
#include "cases/lut3d.inc"
#include "cases/lut_cube.inc"
#include "cases/lut_library.inc"
#include "cases/lut_preview_worker.inc"
#include "cases/lut_render.inc"
#include "cases/mask.inc"
#include "cases/mask_brush.inc"
#include "cases/mask_geom.inc"
#include "cases/mask_object.inc"
#include "cases/mask_polygon.inc"
#include "cases/mask_range.inc"
#include "cases/mask_raster.inc"
#include "cases/mask_rect.inc"
#include "cases/minijson.inc"
#include "cases/neighbor_fx.inc"
#include "cases/photo_meta.inc"
#include "cases/pixel_math.inc"
#include "cases/preset.inc"
#include "cases/raster_ops.inc"
#include "cases/render.inc"
#include "cases/render_core.inc"
#include "cases/render_params.inc"
#include "cases/sam_segmenter.inc"
#include "cases/scopes.inc"
#include "cases/seg_basic.inc"
#include "cases/sidecar.inc"
#include "cases/thumb_worker.inc"
#include "cases/tone_table.inc"
#include "cases/virtual_copy.inc"
#include "cases/wide_chain.inc"
#include "cases/image16_convert.inc"
#include "cases/render_bytes.inc"