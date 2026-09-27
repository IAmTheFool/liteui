// Simple Photo Editor.
//
// Layout, top to bottom:
//   - Custom titlebar: app title, a small File/View menu bar, and our own
//     minimize/maximize/close buttons. Since we're drawing these ourselves,
//     the window is created with hideTitlebar=true (LiteUI's constructor)
//     so the OS doesn't also draw its own — this app owns the whole
//     client area, VS-Code-style.
//   - Workspace: an activity bar (icon strip) on the left, a closable/
//     resizable sidebar next to it, then the scrollable canvas viewport
//     (open/zoom/pan — same as before) filling the rest.
//   - Status bar: image info on the left, zoom controls on the right.
//
// The dropdown menus and the activity-bar-driven sidebar follow the same
// patterns as the VS-Code-style layout example: Position::Absolute + a
// display-source function for the dropdowns, and a plain width-source
// function (0 when "closed") for the sidebar.

#include "liteui.hpp"

#include <algorithm>
#include <memory>
#include <vector>

// ---------------- Layout constants ----------------
constexpr float kTitlebarH = 36.0f;
constexpr float kStatusBarH = 26.0f;
constexpr float kActivityBarW = 48.0f;
constexpr float kDividerW = 4.0f;
constexpr float kMinSidebarW = 180.0f;
constexpr float kMaxSidebarW = 420.0f;

// Zoom range/step.
constexpr float kZoomMin = 0.1f;
constexpr float kZoomMax = 8.0f;
constexpr float kZoomStep = 1.25f;

// Document size before any image has been opened, so there's a visible
// page to scroll/zoom around from the start.
constexpr float kPlaceholderW = 800.0f;
constexpr float kPlaceholderH = 600.0f;

// ---------------- Palette ----------------
constexpr Color kColorTransparent{0, 0, 0, 0};
constexpr Color kColorWhite{255, 255, 255, 255};

// Chrome backgrounds (titlebar, sidebar, activity bar, root, etc.).
constexpr Color kColorRootBg{235, 235, 235, 255};
constexpr Color kColorPanelBg{245, 245, 245, 255}; // titlebar / sidebar
constexpr Color kColorActivityBarBg{225, 225, 225,
                                    255};            // also win-btn hover,
                                                     // inactive activity btn
constexpr Color kColorMenuHover{224, 224, 224, 255}; // menu btn hover/active
constexpr Color kColorActivityHover{210, 210, 210, 255}; // also divider bg,
                                                         // scrollbar track
constexpr Color kColorScrollThumb{130, 130, 135, 255};

// Text.
constexpr Color kColorTextDark{40, 40, 40, 255};  // titles, menu labels
constexpr Color kColorTextMid{50, 50, 50, 255};   // win-btn glyphs, info pane
constexpr Color kColorTextIcon{60, 60, 60, 255};  // activity bar icons
constexpr Color kColorTextMuted{90, 90, 90, 255}; // sidebar section title
constexpr Color kColorTextFaint{140, 140, 140,
                                255}; // "coming soon" placeholders

// Dropdown menu.
constexpr Color kColorDropdownBorder{190, 190, 190, 255};
constexpr Color kColorDropdownRowHover{232, 240, 250, 255};

// Window controls / accents.
constexpr Color kColorCloseHover{232, 17, 35, 255}; // close button hover (red)
constexpr Color kColorDividerHover{100, 140, 220, 255}; // sidebar divider drag

// Canvas / viewport.
constexpr Color kColorCanvasPlaceholderBg{250, 250, 250, 255}; // no image open
constexpr Color kColorViewportBg{200, 200, 200, 255};
constexpr Color kColorCanvasBorder{150, 150, 150, 255};

// Status bar (blue).
constexpr Color kColorStatusBarBg{0, 122, 204, 255};
constexpr Color kColorStatusHover{30, 140, 220, 255};

// A sidebar tab in the activity bar.
struct Activity {
  int id;
  std::string title;
  std::string icon; // single glyph
};

struct EditorState {
  std::shared_ptr<CanvasImage> image; // null until something is opened
  std::string imagePath;              // full path of the opened file
  float zoom = 1.0f;
  bool dirty = true;

  int activeActivity = 0; // -1 = sidebar closed
  float sidebarWidth = 220.0f;
  int openMenu = -1;      // -1 = no dropdown open, else its menu id
  bool maximized = false; // mirrors our own toggle (no getter in LiteUI)

  float docWidth() const {
    return image ? static_cast<float>(image->width) : kPlaceholderW;
  }
  float docHeight() const {
    return image ? static_cast<float>(image->height) : kPlaceholderH;
  }
  void markDirty() { dirty = true; }
};

static EditorState state;

static std::string baseName(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

static void setZoom(float z) {
  state.zoom = std::clamp(z, kZoomMin, kZoomMax);
  state.markDirty();
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
  state.imagePath = *path;
  state.zoom = 1.0f; // start fresh at 100% on every newly opened image
  state.markDirty();
}

static void toggleSidebar() {
  state.activeActivity = (state.activeActivity == -1) ? 0 : -1;
}

// ---------------- Small reusable pieces ----------------

// A titlebar menu button ("File", "View", ...) that opens a dropdown of
// {label, action} items below it when clicked. Only one menu is open at
// a time (EditorState::openMenu); picking an item runs its action and
// closes the menu.
static View
menuButton(const std::string &label, int id,
           std::vector<std::pair<std::string, std::function<void()>>> items) {
  View btn;
  btn.style.height = Size::full();
  btn.style.padding = EdgeInsets{0, 12, 0, 12};
  btn.style.justifyContent = Justify::Center;
  btn.style.alignItems = Align::Center;
  btn.style.hoverColor = kColorMenuHover;
  btn.style.backgroundColor = std::function<Color()>([id] {
    return state.openMenu == id ? kColorMenuHover : kColorTransparent;
  });
  btn.onClick = [id] { state.openMenu = (state.openMenu == id) ? -1 : id; };

  Text label_;
  label_.label = label;
  label_.fontSize = 13.0f;
  label_.color = kColorTextDark;
  btn.addChild(std::move(label_));

  View dropdown;
  dropdown.style.position = Position::Absolute;
  dropdown.style.top = kTitlebarH;
  dropdown.style.left = 0.0f;
  dropdown.style.width = Size::pixel(210);
  dropdown.style.direction = FlexDirection::Column;
  dropdown.style.backgroundColor = kColorWhite;
  dropdown.style.borderWidth = 1.0f;
  dropdown.style.borderColor = kColorDropdownBorder;
  dropdown.style.borderRadius = 4.0f;
  dropdown.style.padding = EdgeInsets::all(4.0f);
  dropdown.style.zIndex = 100;
  dropdown.style.display = std::function<Display()>(
      [id] { return state.openMenu == id ? Display::Flex : Display::None; });

  for (auto &item : items) {
    View row;
    row.style.height = Size::pixel(30);
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding = EdgeInsets{0, 10, 0, 10};
    row.style.borderRadius = 3.0f;
    row.style.hoverColor = kColorDropdownRowHover;
    std::function<void()> action = item.second;
    row.onClick = [action] {
      action();
      state.openMenu = -1;
    };

    Text t;
    t.label = item.first;
    t.fontSize = 13.0f;
    t.color = kColorTextDark;
    row.addChild(std::move(t));
    dropdown.addChild(std::move(row));
  }

  btn.addChild(std::move(dropdown));
  return btn;
}

// One of the three window-control buttons (minimize/maximize/close).
static View winButton(const std::string &glyph, Color hoverBg,
                      std::function<void()> onClick) {
  View v;
  v.style.width = Size::pixel(46);
  v.style.height = Size::full();
  v.style.justifyContent = Justify::Center;
  v.style.alignItems = Align::Center;
  v.style.hoverColor = hoverBg;
  v.onClick = std::move(onClick);

  Text t;
  t.label = glyph;
  t.fontSize = 13.0f;
  t.color = kColorTextMid;
  v.addChild(std::move(t));
  return v;
}

static void paintCanvas(CanvasContext &ctx) {
  ctx.setFillColor(state.image ? kColorWhite : kColorCanvasPlaceholderBg);
  ctx.fillRect(0, 0, ctx.width(), ctx.height());

  if (!state.image || state.image->width <= 0 || state.image->height <= 0)
    return;

  // Drawn at the document's natural pixel size; the canvas itself is
  // already sized to docWidth*zoom / docHeight*zoom (see canvas.style
  // below), so a plain ctx.scale(zoom, zoom) here is all that's needed.
  ctx.save();
  ctx.scale(state.zoom, state.zoom);
  ctx.drawImage(*state.image, 0, 0);
  ctx.restore();
}

int main() {
  LiteUI ui("Photo Editor", 1100, 700, /*hideTitlebar=*/true);
  LiteUI *uiPtr = &ui; // captured by value in the callbacks below; ui
                       // outlives its own layout tree, so this is safe.

  // Scrollbars are app-global (there's no per-view override), so set
  // them once here instead of leaving the library's default gray —
  // a track/thumb pair that reads on both the light chrome and the
  // gray canvas viewport.
  ui.setScrollbarColors(/*track=*/kColorActivityHover,
                        /*thumb=*/kColorScrollThumb);

  const std::vector<Activity> activities = {
      {0, "Info", "I"},
      {1, "Adjust", "A"},
      {2, "Crop", "C"},
  };

  View root;
  root.style.direction = FlexDirection::Column;
  root.style.width = Size::full();
  root.style.height = Size::full();
  root.style.backgroundColor = kColorRootBg;

  // ---------------------------------------------------------------
  // Titlebar: title, File/View menus, window controls.
  // ---------------------------------------------------------------
  View titlebar;
  titlebar.style.direction = FlexDirection::Row;
  titlebar.style.width = Size::full();
  titlebar.style.height = Size::pixel(kTitlebarH);
  titlebar.style.flexShrink = 0;
  titlebar.style.alignItems = Align::Center;
  titlebar.style.backgroundColor = kColorPanelBg;
  titlebar.style.borderWidth = 0.0f;
  // Empty space in the titlebar (i.e. not over a button) drags the
  // window, and also closes any open dropdown menu.
  titlebar.onPressAt = [uiPtr](float, float) {
    state.openMenu = -1;
    uiPtr->requestMove();
  };

  Text appTitle;
  appTitle.label = std::string("Photo Editor");
  appTitle.fontSize = 13.0f;
  appTitle.fontWeight = FontWeight::SemiBold;
  appTitle.color = kColorTextDark;
  appTitle.style.margin = EdgeInsets{0, 16, 0, 14};
  titlebar.addChild(std::move(appTitle));

  titlebar.addChild(menuButton("File", 0,
                               {
                                   {"Open Image...", openImage},
                                   {"Exit", [uiPtr] { uiPtr->requestClose(); }},
                               }));
  titlebar.addChild(
      menuButton("View", 1,
                 {
                     {"Zoom In", [] { setZoom(state.zoom * kZoomStep); }},
                     {"Zoom Out", [] { setZoom(state.zoom / kZoomStep); }},
                     {"Actual Size (100%)", [] { setZoom(1.0f); }},
                     {"Toggle Sidebar", toggleSidebar},
                 }));

  View titlebarSpacer;
  titlebarSpacer.style.flexGrow = 1;
  titlebarSpacer.style.height = Size::full();
  titlebar.addChild(std::move(titlebarSpacer));

  titlebar.addChild(winButton("\u2500", kColorActivityBarBg,
                              [uiPtr] { uiPtr->requestMinimize(); }));
  titlebar.addChild(winButton("\u25A1", kColorActivityBarBg, [uiPtr] {
    uiPtr->requestMaximize();
    state.maximized = !state.maximized;
  }));
  titlebar.addChild(winButton("\u00D7", kColorCloseHover,
                              [uiPtr] { uiPtr->requestClose(); }));

  root.addChild(std::move(titlebar));

  // ---------------------------------------------------------------
  // Workspace: activity bar, sidebar, divider, canvas viewport.
  // ---------------------------------------------------------------
  View workspace;
  workspace.style.direction = FlexDirection::Row;
  workspace.style.width = Size::full();
  workspace.style.flexGrow = 1;
  // Clicking anywhere in the workspace that isn't itself a button
  // (e.g. the canvas, or blank sidebar space) closes an open menu too.
  workspace.onPressAt = [](float, float) { state.openMenu = -1; };

  // ---- Activity bar ----
  View activityBar;
  activityBar.style.width = Size::pixel(kActivityBarW);
  activityBar.style.height = Size::full();
  activityBar.style.flexShrink = 0;
  activityBar.style.direction = FlexDirection::Column;
  activityBar.style.alignItems = Align::Center;
  activityBar.style.backgroundColor = kColorActivityBarBg;
  activityBar.style.padding = EdgeInsets::all(6.0f);
  activityBar.style.gap = 10.0f;

  for (const auto &item : activities) {
    View button;
    button.style.width = Size::pixel(32);
    button.style.height = Size::pixel(32);
    button.style.borderRadius = 6.0f;
    button.style.justifyContent = Justify::Center;
    button.style.alignItems = Align::Center;
    button.style.hoverColor = kColorActivityHover;

    int id = item.id;
    button.style.backgroundColor = std::function<Color()>([id] {
      return state.activeActivity == id ? kColorWhite : kColorActivityBarBg;
    });
    button.onClick = [id] {
      state.activeActivity = (state.activeActivity == id) ? -1 : id;
    };

    Text icon;
    icon.label = item.icon;
    icon.fontSize = 15.0f;
    icon.fontWeight = FontWeight::SemiBold;
    icon.color = kColorTextIcon;
    button.addChild(std::move(icon));

    activityBar.addChild(std::move(button));
  }

  workspace.addChild(std::move(activityBar));

  // ---- Sidebar (width driven by activeActivity: 0 when closed) ----
  View sidebar;
  sidebar.style.height = Size::full();
  sidebar.style.flexShrink = 0;
  sidebar.style.direction = FlexDirection::Column;
  sidebar.style.backgroundColor = kColorPanelBg;
  sidebar.style.padding = EdgeInsets::all(12.0f);
  sidebar.style.overflowX = Overflow::Hidden;
  sidebar.style.overflowY = Overflow::Auto;
  sidebar.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? state.sidebarWidth : 0.0f);
  });

  Text sidebarTitle;
  sidebarTitle.label =
      std::function<std::string()>([activities]() -> std::string {
        for (const auto &a : activities)
          if (a.id == state.activeActivity)
            return a.title;
        return "";
      });
  sidebarTitle.fontSize = 12.0f;
  sidebarTitle.fontWeight = FontWeight::SemiBold;
  sidebarTitle.color = kColorTextMuted;
  sidebarTitle.style.margin = EdgeInsets{0, 0, 10, 0};
  sidebar.addChild(std::move(sidebarTitle));

  // -- "Info" pane: real data about the open image.
  View infoPane;
  infoPane.style.direction = FlexDirection::Column;
  infoPane.style.width = Size::full();
  infoPane.style.gap = 6.0f;
  infoPane.style.display = std::function<Display()>(
      [] { return state.activeActivity == 0 ? Display::Flex : Display::None; });

  Text infoName;
  infoName.label = std::function<std::string()>([] {
    return state.image ? ("File: " + baseName(state.imagePath))
                       : std::string("No image open");
  });
  infoName.fontSize = 13.0f;
  infoName.color = kColorTextMid;
  infoPane.addChild(std::move(infoName));

  Text infoDims;
  infoDims.label = std::function<std::string()>([] {
    if (!state.image)
      return std::string();
    return "Size: " + std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px";
  });
  infoDims.fontSize = 13.0f;
  infoDims.color = kColorTextMid;
  infoPane.addChild(std::move(infoDims));

  Text infoZoom;
  infoZoom.label = std::function<std::string()>([] {
    return "Zoom: " +
           std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  });
  infoZoom.fontSize = 13.0f;
  infoZoom.color = kColorTextMid;
  infoPane.addChild(std::move(infoZoom));

  sidebar.addChild(std::move(infoPane));

  // -- Placeholder panes for the not-yet-built tools.
  for (int id : {1, 2}) {
    std::string label =
        (id == 1) ? "Adjustments coming soon" : "Crop tool coming soon";
    View pane;
    pane.style.width = Size::full();
    pane.style.display = std::function<Display()>([id] {
      return state.activeActivity == id ? Display::Flex : Display::None;
    });

    Text placeholder;
    placeholder.label = label;
    placeholder.fontSize = 13.0f;
    placeholder.color = kColorTextFaint;
    pane.addChild(std::move(placeholder));

    sidebar.addChild(std::move(pane));
  }

  workspace.addChild(std::move(sidebar));

  // ---- Vertical divider — drag to resize the sidebar ----
  View vDivider;
  vDivider.style.height = Size::full();
  vDivider.style.flexShrink = 0;
  vDivider.style.backgroundColor = kColorActivityHover;
  vDivider.style.hoverColor = kColorDividerHover;
  vDivider.style.width = std::function<Size()>([] {
    return Size::pixel(state.activeActivity != -1 ? kDividerW : 0.0f);
  });

  auto updateSidebarWidth = [](float localX) {
    float next = state.sidebarWidth + localX - kDividerW / 2.0f;
    state.sidebarWidth = std::clamp(next, kMinSidebarW, kMaxSidebarW);
  };
  vDivider.onPressAt = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };
  vDivider.onDragTo = [updateSidebarWidth](float localX, float) {
    updateSidebarWidth(localX);
  };

  workspace.addChild(std::move(vDivider));

  // ---- Scrollable canvas viewport ----
  View viewport;
  viewport.style.height = Size::full();
  viewport.style.flexGrow = 1;
  viewport.style.backgroundColor = kColorViewportBg;
  viewport.style.overflowX = Overflow::Auto;
  viewport.style.overflowY = Overflow::Auto;
  viewport.style.justifyContent = Justify::Center;
  viewport.style.alignItems = Align::Center;
  viewport.style.padding = EdgeInsets::all(24);
  viewport.style.wheelScrollEnabled = false;
  viewport.style.scrollThumbColor = kColorScrollThumb;
  viewport.style.scrollTrackColor = kColorActivityHover;
  viewport.style.scrollbarAutoHide = true;

  View canvas;
  canvas.isCanvas = true;
  canvas.style.width = std::function<Size()>(
      [] { return Size::pixel(state.docWidth() * state.zoom); });
  canvas.style.height = std::function<Size()>(
      [] { return Size::pixel(state.docHeight() * state.zoom); });
  canvas.style.borderWidth = 1.0f;
  canvas.style.borderColor = kColorCanvasBorder;
  canvas.style.backgroundColor = kColorWhite;

  canvas.onPaint = [](CanvasContext &ctx) { paintCanvas(ctx); };
  canvas.canvasDirtySource = [] {
    bool d = state.dirty;
    state.dirty = false;
    return d;
  };
  canvas.onScrollUp = [] { setZoom(state.zoom * kZoomStep); };
  canvas.onScrollDown = [] { setZoom(state.zoom / kZoomStep); };

  viewport.addChild(std::move(canvas));
  workspace.addChild(std::move(viewport));

  root.addChild(std::move(workspace));

  // ---------------------------------------------------------------
  // Status bar: image info on the left, zoom controls on the right.
  // ---------------------------------------------------------------
  View statusBar;
  statusBar.style.width = Size::full();
  statusBar.style.height = Size::pixel(kStatusBarH);
  statusBar.style.flexShrink = 0;
  statusBar.style.direction = FlexDirection::Row;
  statusBar.style.alignItems = Align::Center;
  statusBar.style.justifyContent = Justify::SpaceBetween;
  statusBar.style.backgroundColor = kColorStatusBarBg;
  statusBar.style.padding = EdgeInsets{0, 10, 0, 12};

  Text statusLeft;
  statusLeft.label = std::function<std::string()>([] {
    if (!state.image)
      return std::string("No image open");
    return baseName(state.imagePath) + "  —  " +
           std::to_string(state.image->width) + " x " +
           std::to_string(state.image->height) + " px";
  });
  statusLeft.fontSize = 12.0f;
  statusLeft.color = kColorWhite;
  statusBar.addChild(std::move(statusLeft));

  View zoomControls;
  zoomControls.style.direction = FlexDirection::Row;
  zoomControls.style.alignItems = Align::Center;
  zoomControls.style.gap = 4.0f;

  auto statusGlyphBtn = [](const std::string &label,
                           std::function<void()> onClick) {
    View v;
    v.style.width = Size::pixel(20);
    v.style.height = Size::pixel(20);
    v.style.borderRadius = 3.0f;
    v.style.justifyContent = Justify::Center;
    v.style.alignItems = Align::Center;
    v.style.hoverColor = kColorStatusHover;
    v.onClick = std::move(onClick);
    Text t;
    t.label = label;
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    v.addChild(std::move(t));
    return v;
  };

  zoomControls.addChild(
      statusGlyphBtn("-", [] { setZoom(state.zoom / kZoomStep); }));

  Text zoomLabel;
  zoomLabel.label = [] {
    return std::to_string(static_cast<int>(state.zoom * 100.0f + 0.5f)) + "%";
  };
  zoomLabel.fontSize = 12.0f;
  zoomLabel.color = kColorWhite;
  zoomLabel.style.margin = EdgeInsets{0, 4, 0, 4};
  zoomControls.addChild(std::move(zoomLabel));

  zoomControls.addChild(
      statusGlyphBtn("+", [] { setZoom(state.zoom * kZoomStep); }));

  View zoomResetSpacer;
  zoomResetSpacer.style.width = Size::pixel(6);
  zoomControls.addChild(std::move(zoomResetSpacer));

  View zoomResetBtn;
  zoomResetBtn.style.justifyContent = Justify::Center;
  zoomResetBtn.style.alignItems = Align::Center;
  zoomResetBtn.style.hoverColor = kColorStatusHover;
  zoomResetBtn.style.borderRadius = 3.0f;
  zoomResetBtn.style.padding = EdgeInsets{2, 6, 2, 6};
  zoomResetBtn.onClick = [] { setZoom(1.0f); };
  {
    Text t;
    t.label = std::string("100%");
    t.fontSize = 12.0f;
    t.color = kColorWhite;
    zoomResetBtn.addChild(std::move(t));
  }
  zoomControls.addChild(std::move(zoomResetBtn));

  statusBar.addChild(std::move(zoomControls));

  root.addChild(std::move(statusBar));

  ui.setRoot(std::move(root));
  ui.run();
  return 0;
}