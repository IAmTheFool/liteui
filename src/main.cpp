// src/main.cpp

#include "liteui.hpp"
#include "liteui_editor_ext.hpp"
#include "liteui_editor_tabs.hpp"
#include "liteui_syntax.hpp"

#include <filesystem>

int main(int argc, char **argv) {
  TabbedEditor editor("CODE");
  for (int i = 1; i < argc; ++i)
    editor.openFile(argv[i]);

  editor.run();
  return 0;
}