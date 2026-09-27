// Simple Photo Editor — starting point.
//
// Just a toolbar with an "Open" button and a canvas that displays the
// loaded image, scaled to fit the viewport (like an image preview). No
// editing yet — this is step 1: load a file, show it.
//
// Loading uses liteui_image::decodeFile directly (the same decoder the
// Image view uses internally) rather than the Image view itself, since
// we want the picture inside a plain Canvas we already control — that
// makes it straightforward to later add drawing/filters/etc. on top of
// the same CanvasContext.

#include "liteui.hpp"

#include <memory>

struct EditorState {
  std::shared_ptr<CanvasImage> image; // null until something is opened
  bool dirty = true;
};

static EditorState state;

static View textButton(const std::string &label,
                        std::function<void()> onClick) {
  View v;
  v.style.padding = EdgeInsets{8, 14, 8, 14};
  v.style.margin = EdgeInsets{3, 3, 3, 3};
  v.style.backgroundColor = Color{245, 245, 245, 255};
  v.style.hoverColor = Color{230, 230, 230, 255};
  v.style.borderWidth = 1.0f;
  v.style.borderColor = Color{190, 190, 190, 255};
  v.style.borderRadius = 6.0f;
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.onClick = std::move(onClick);

  Text t;
  t.label = label;
  t.fontSize = 14;
  t.color = Color{30, 30, 30, 255};
  v.addChild(std::move(t));
  return v;
}

static void openImage() {
  auto path = openFilePicker(
      "Open Image", {{"Images", "*.png;*.jpg;*.jpeg"}, {"All Files", "*.*"}});
  if (!path)
    return; // user cancelled

  std::string err;
  auto decoded = liteui_image::decodeFile(*path, &err);
  if (!decoded)
    return; // TODO: surface `err` to the user

  state.image = std::make_shared<CanvasImage>(std::move(*decoded));
  state.dirty = true;
}

static void paintCanvas(CanvasContext &ctx) {
  ctx.setFillColor({235, 235, 235, 255});
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

  if (!state.image || state.image->width <= 0 || state.image->height <= 0)
    return;

  // object-fit: contain — scale the image to fit inside the canvas
  // without cropping, centered.
  float cw = ctx.width(), ch = ctx.height();
  float iw = static_cast<float>(state.image->width);
  float ih = static_cast<float>(state.image->height);
  float scale = std::min(cw / iw, ch / ih);
  float dw = iw * scale, dh = ih * scale;
  float dx = (cw - dw) / 2.0f, dy = (ch - dh) / 2.0f;

  ctx.drawImage(*state.image, dx, dy, dw, dh);
}

int main() {
  LiteUI ui("Photo Editor", 900, 650);

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = Color{220, 220, 220, 255};

  // ---- Toolbar ----
  View toolbar;
  toolbar.style.direction = FlexDirection::Row;
  toolbar.style.alignItems = Align::Center;
  toolbar.style.padding = EdgeInsets::all(8);
  toolbar.style.backgroundColor = Color{245, 245, 245, 255};
  toolbar.style.height = Size::pixel(52);
  toolbar.style.flexShrink = 0;

  toolbar.addChild(textButton("Open", openImage));

  root.addChild(std::move(toolbar));

  // ---- Canvas (fills the rest of the window) ----
  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = Size::full();
  canvas.style.height = Size::full();

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx); };
  canvas.canvasDirtySource = [] {
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };

  root.addChild(std::move(canvas));

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}