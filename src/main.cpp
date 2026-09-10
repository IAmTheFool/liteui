// src/main.cpp

#include "liteui.hpp"
#include <cstdio>
#include <filesystem>
#include <sstream>

// Formats a byte count as "B"/"KB"/"MB" with one decimal place beyond the
// first tier, matching how most file managers display size.
static std::string formatSize(uintmax_t bytes) {
  constexpr double kKB = 1024.0, kMB = kKB * 1024.0;
  std::ostringstream out;
  if (bytes < static_cast<uintmax_t>(kKB)) {
    out << bytes << " B";
  } else if (bytes < static_cast<uintmax_t>(kMB)) {
    out.precision(1);
    out << std::fixed << (bytes / kKB) << " KB";
  } else {
    out.precision(1);
    out << std::fixed << (bytes / kMB) << " MB";
  }
  return out.str();
}

// Builds the multi-line info string shown under the button. Re-decodes the
// file via liteui_image::decodeFile purely to report pixel dimensions —
// fine as a one-off on file selection, not something to do per-frame.
static std::string describeFile(const std::string &path) {
  std::error_code ec;
  std::filesystem::path p(path);

  std::ostringstream out;
  out << "Name: " << p.filename().string() << "\n";
  out << "Path: " << p.string() << "\n";

  std::string ext = p.extension().string();
  out << "Type: " << (ext.empty() ? "(no extension)" : ext) << "\n";

  auto size = std::filesystem::file_size(p, ec);
  out << "Size: " << (ec ? "unknown" : formatSize(size)) << "\n";

  if (auto img = liteui_image::decodeFile(path)) {
    out << "Dimensions: " << img->width << " x " << img->height << " px";
  } else {
    out << "Dimensions: (not a decodable image)";
  }
  return out.str();
}

int main() {
  LiteUI ui(800, 600, "Scrolling demo");

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.padding = EdgeInsets::all(16);
  root.style.gap = 12;
  root.style.backgroundColor = Color{0xF2, 0xF2, 0xF2};

  Text buttonText;
  buttonText.label = "Open File";

  View button;
  button.style.width = Size::pixel(80);
  button.style.height = Size::pixel(34);
  button.style.backgroundColor = Color{240, 240, 240};
  button.style.hoverColor = Color{225, 225, 225};
  button.style.borderRadius = 4.0f;
  button.style.alignItems = Align::Center;
  button.style.justifyContent = Justify::Center;
  button.addChild(buttonText);

  // Shared, because both onClick's capture and the Text's dynamic label
  // need to read/write the same string across separate closures.
  auto fileInfo = std::make_shared<std::string>("No file selected");

  Text infoText;
  // A std::function label — not a plain string — so LiteUI's
  // checkForUpdates() re-polls it after every click and picks up whatever
  // onClick just wrote into *fileInfo.
  infoText.label = [fileInfo] { return *fileInfo; };
  infoText.fontSize = 14.0f;
  infoText.wrap = TextWrap::Wrap;
  infoText.color = Color{40, 40, 40};

  View infoBox;
  infoBox.style.width = Size::full();
  infoBox.style.padding = EdgeInsets::all(12);
  infoBox.style.backgroundColor = Color{255, 255, 255};
  infoBox.style.borderRadius = 4.0f;
  infoBox.style.borderWidth = 1.0f;
  infoBox.style.borderColor = Color{220, 220, 220};
  infoBox.addChild(infoText);

  button.onClick = [fileInfo]() {
    if (auto path = openFilePicker("Open Image",
                                   {{"Images", "*.png;*.jpg;*.jpeg"}})) {
      *fileInfo = describeFile(*path);
    }
    // Cancel leaves *fileInfo (and the displayed text) unchanged.
  };

  root.addChild(std::move(button));
  root.addChild(std::move(infoBox));
  ui.setRoot(std::move(root));

  ui.run();
  return 0;
}