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

#include "cases/core_types.inc"
#include "cases/xml.inc"
#include "cases/svg_primitives.inc"
#include "cases/svg_paths.inc"
#include "cases/svg_documents.inc"
#include "cases/layout.inc"
#include "cases/platform.inc"
