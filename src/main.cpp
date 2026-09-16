// src/main.cpp

#include "liteui.hpp"
#include "liteui_editor_ext.hpp"
#include "liteui_editor_tabs.hpp"
#include "liteui_syntax.hpp"

#include <filesystem>

int main(int argc, char **argv) {
  TabbedEditor editor(1200, 750, "liteui code editor");
  for (int i = 1; i < argc; ++i)
    editor.openFile(argv[i]);

  editor.run();
  return 0;
}