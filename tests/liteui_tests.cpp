// The single translation unit of the test executable. See CMakeLists.txt in
// this folder for why every test lives in one TU: the internals under test
// (liteui_xml, liteui_svg, liteui_layout, ...) only exist in the
// LITEUI_IMPLEMENTATION section of the header, and that section may be
// compiled into exactly one translation unit.
//
// To add a test file: create cases/<name>.inc and add an #include below.

#define LITEUI_IMPLEMENTATION
#include "liteui.hpp"

#include "test_helpers.hpp"
#include "clipboard_helpers.hpp"

#include "cases/core_types.inc"
#include "cases/pixel_format.inc"
#include "cases/image_buffer.inc"
#include "cases/image_convert.inc"
#include "cases/image_display.inc"
#include "cases/image_formats16.inc"
#include "cases/image_codec.inc"
#include "cases/image_widget.inc"
#include "cases/xml.inc"
#include "cases/svg_primitives.inc"
#include "cases/svg_paths.inc"
#include "cases/svg_documents.inc"
#include "cases/layout.inc"
#include "cases/pointer_events.inc"
#include "cases/wheel_input.inc"
#include "cases/file_drop.inc"
#include "cases/platform.inc"

// Clipboard (text and images): the DIB converters and the shared PNG layer run
// everywhere; the tool-backed and native Wayland cases are Linux-only (the
// Wayland ones also need libwayland-server, see CMakeLists.txt), and the
// real-clipboard cases are Windows-only. Each file is empty where it does not
// apply. file_drop_wayland.inc (drag-and-drop in, native Wayland) is gated the
// same way as clipboard_wayland.inc.
#include "cases/clipboard_dib.inc"
#include "cases/clipboard_image.inc"
#include "cases/clipboard_tools.inc"
#include "cases/clipboard_wayland.inc"
#include "cases/file_drop_wayland.inc"
#include "cases/clipboard_windows.inc"

// Canvas: the GL backend cases compile only on Linux with LITEUI_CANVAS_GL (the
// two canvas_gl_*.inc files are empty elsewhere). canvas_hardware.inc builds on
// every platform but is hidden unless "[hardware]" is requested explicitly.
#include "cases/canvas_sampling.inc"
#include "cases/canvas_gl_helpers.inc"
#include "cases/canvas_gl_context.inc"
#include "cases/canvas_texture.inc"
#include "cases/canvas_damage.inc"
#include "cases/canvas_hardware.inc"