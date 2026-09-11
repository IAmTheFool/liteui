// src/main.cpp

#include "liteui.hpp"
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>

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

static std::string describeFile(const std::string &path,
                                const CanvasImage *img) {
  std::error_code ec;
  std::filesystem::path p(path);

  std::ostringstream out;
  out << "Name: " << p.filename().string() << "\n";
  out << "Path: " << p.string() << "\n";

  std::string ext = p.extension().string();
  out << "Type: " << (ext.empty() ? "(no extension)" : ext) << "\n";

  auto size = std::filesystem::file_size(p, ec);
  out << "Size: " << (ec ? "unknown" : formatSize(size)) << "\n";

  if (img)
    out << "Dimensions: " << img->width << " x " << img->height << " px";
  else
    out << "Dimensions: (not a decodable image)";
  return out.str();
}

struct PreviewState {
  std::shared_ptr<CanvasImage> image; // null until a file is picked
};

int main() {
  LiteUI ui(800, 600, "Image viewer");

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

  auto fileInfo = std::make_shared<std::string>("No file selected");

  Text infoText;
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

  auto preview = std::make_shared<PreviewState>();

  Image previewImage;
  previewImage.style.width = Size::full();
  previewImage.style.flexGrow = 1;
  previewImage.style.backgroundColor = Color{255, 255, 255};
  previewImage.style.borderWidth = 1.0f;
  previewImage.style.borderColor = Color{220, 220, 220};
  previewImage.style.borderRadius = 4.0f;
  previewImage.fit = ObjectFit::Contain;
  previewImage.source = [preview] { return preview->image; };

  button.onClick = [fileInfo, preview]() {
    auto path =
        openFilePicker("Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}});
    if (!path)
      return;

    std::string err;
    auto decoded = liteui_image::decodeFile(*path, &err);
    preview->image =
        decoded ? std::make_shared<CanvasImage>(std::move(*decoded)) : nullptr;
    *fileInfo = describeFile(*path, preview->image.get());
  };

  root.addChild(std::move(button));
  root.addChild(std::move(infoBox));
  root.addChild(std::move(previewImage));
  ui.setRoot(std::move(root));

  ui.run();
  return 0;
}