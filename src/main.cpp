// src/main.cpp

#include "liteui.hpp"
#include <cstdio>
#include <filesystem>
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

// Takes the already-decoded image (or nullptr if decoding failed/wasn't an
// image) so we don't decode the file twice.
static std::string describeFile(const std::string &path, const CanvasImage *img) {
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

// Shared state the button's onClick writes into and the preview Canvas
// reads from. Kept separate from the View tree entirely — no tree
// mutation happens after setRoot(), only this struct's contents change.
struct PreviewState {
  std::optional<CanvasImage> image;
  bool dirty = true; // starts true; View::computed.canvasNeedsRedraw also
                     // starts true, so the first frame paints regardless
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

  // ---- Open button ----
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

  // ---- Metadata text ----
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

  // ---- Image preview canvas ----
  auto preview = std::make_shared<PreviewState>();

  Canvas previewCanvas;
  previewCanvas.style.width = Size::full();
  previewCanvas.style.flexGrow = 1; // fill remaining vertical space
  previewCanvas.style.backgroundColor = Color{255, 255, 255};
  previewCanvas.style.borderWidth = 1.0f;
  previewCanvas.style.borderColor = Color{220, 220, 220};
  previewCanvas.style.borderRadius = 4.0f;

  previewCanvas.canvasDirtySource = [preview] {
    bool d = preview->dirty;
    preview->dirty = false;
    return d;
  };

  previewCanvas.onPaint = [preview](CanvasContext &ctx) {
    ctx.setFillColor(Color{255, 255, 255});
    ctx.fillRect(0, 0, ctx.width(), ctx.height());

    if (!preview->image) {
      ctx.setFillColor(Color{160, 160, 160});
      ctx.setFont("", 14.0f);
      ctx.setTextAlign(TextAlign::Center);
      ctx.setTextBaseline(TextBaseline::Middle);
      ctx.fillText("No image selected", ctx.width() / 2.0f, ctx.height() / 2.0f);
      return;
    }

    // Contain-fit into the canvas, same math View::toView(Image) uses.
    const CanvasImage &im = *preview->image;
    float cw = ctx.width(), ch = ctx.height();
    float iw = static_cast<float>(im.width), ih = static_cast<float>(im.height);
    float scale = std::min(cw / iw, ch / ih);
    float dw = iw * scale, dh = ih * scale;
    float dx = (cw - dw) / 2.0f, dy = (ch - dh) / 2.0f;
    ctx.drawImage(im, dx, dy, dw, dh);
  };

  button.onClick = [fileInfo, preview]() {
    auto path = openFilePicker("Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}});
    if (!path)
      return; // cancelled — leave existing state alone

    preview->image = liteui_image::decodeFile(*path);
    preview->dirty = true; // triggers the Canvas's next repaint
    *fileInfo = describeFile(*path, preview->image ? &*preview->image : nullptr);
    // No ui.setRoot() call here — we only mutate shared state; the click
    // dispatch that invoked this lambda already runs checkForUpdates()
    // and relayout() right after, which is what actually picks this up.
  };

  root.addChild(std::move(button));
  root.addChild(std::move(infoBox));
  root.addChild(std::move(previewCanvas));
  ui.setRoot(std::move(root));

  ui.run();
  return 0;
}