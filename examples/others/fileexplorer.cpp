// src/main.cpp
#include "liteui.hpp"
#include <filesystem>
#include <algorithm>
#include <memory>
#include <vector>
#include <string>

namespace fs = std::filesystem;

// ---- App-side tree model ----
struct FileNode {
    std::string name;
    std::string path;
    bool isDir = false;
    bool expanded = false;
    bool loaded = false;
    std::vector<std::shared_ptr<FileNode>> children;
};

struct AppState {
    LiteUI *ui = nullptr;
    std::shared_ptr<FileNode> root;
    std::string selectedPath;
};

static AppState app;

void loadChildren(const std::shared_ptr<FileNode> &node) {
    node->children.clear();
    std::error_code ec;
    if (!fs::exists(node->path, ec) || !fs::is_directory(node->path, ec)) {
        node->loaded = true;
        return;
    }
    for (auto &entry : fs::directory_iterator(node->path, ec)) {
        auto child = std::make_shared<FileNode>();
        child->name = entry.path().filename().string();
        child->path = entry.path().string();
        child->isDir = entry.is_directory();
        node->children.push_back(child);
    }
    std::sort(node->children.begin(), node->children.end(),
              [](const auto &a, const auto &b) {
                  if (a->isDir != b->isDir) return a->isDir > b->isDir; // dirs first
                  return a->name < b->name;
              });
    node->loaded = true;
}

void rebuild(); // fwd decl

// One row (icon + name) for a single node, with click handling.
View buildRow(std::shared_ptr<FileNode> node, int depth) {
    View row;
    row.style.direction = FlexDirection::Row;
    row.style.width = Size::full();
    row.style.height = Size::pixel(24);
    row.style.alignItems = Align::Center;
    row.style.padding = EdgeInsets{2, 4, 2, static_cast<float>(8 + depth * 16)};
    row.style.hoverColor = Color{230, 230, 230, 255};
    bool selected = !node->isDir && node->path == app.selectedPath;
    row.style.backgroundColor = selected ? Color{200, 220, 250, 255}
                                          : Color{255, 255, 255, 255};

    Text icon;
    icon.label = node->isDir ? (node->expanded ? std::string("\xE2\x96\xBE ")   // ▾
                                                : std::string("\xE2\x96\xB8 ")) // ▸
                              : std::string("   ");
    icon.fontSize = 12.0f;
    icon.style.width = Size::pixel(16);
    row.addChild(icon);

    Text label;
    label.label = node->name;
    label.fontSize = 13.0f;
    row.addChild(label);

    // Click bubbles up to `row` from the icon/label children automatically,
    // since neither of them has its own onClick set.
    row.onClick = [node]() {
        if (node->isDir) {
            if (!node->loaded) loadChildren(node);
            node->expanded = !node->expanded;
        } else {
            app.selectedPath = node->path;
        }
        rebuild();
    };
    return row;
}

// Flattens the visible (expanded) part of the tree into a list of rows.
void collectRows(const std::shared_ptr<FileNode> &node, int depth,
                  std::vector<View> &rows) {
    rows.push_back(buildRow(node, depth));
    if (node->isDir && node->expanded)
        for (auto &c : node->children) collectRows(c, depth + 1, rows);
}

View buildRootView() {
    View root;
    root.style.direction = FlexDirection::Row;
    root.style.width = Size::full();
    root.style.height = Size::full();

    // ---- Sidebar ----
    View sidebar;
    sidebar.style.direction = FlexDirection::Column;
    sidebar.style.width = Size::pixel(280);
    sidebar.style.height = Size::full();
    sidebar.style.backgroundColor = Color{245, 245, 245, 255};
    sidebar.style.borderWidth = 1.0f;
    sidebar.style.borderColor = Color{220, 220, 220, 255};

    // "Open Folder" button
    View button;
    button.style.margin = EdgeInsets::all(8);
    button.style.padding = EdgeInsets{6, 12, 6, 12};
    button.style.backgroundColor = Color{60, 120, 220, 255};
    button.style.hoverColor = Color{50, 100, 200, 255};
    button.style.borderRadius = 4.0f;
    button.style.justifyContent = Justify::Center;
    button.style.alignItems = Align::Center;
    Text btnLabel;
    btnLabel.label = "Open Folder";
    btnLabel.color = Color{255, 255, 255, 255};
    btnLabel.fontSize = 13.0f;
    button.addChild(btnLabel);
    button.onClick = [] {
        auto picked = openFolderPicker("Open Folder");
        if (picked) {
            auto node = std::make_shared<FileNode>();
            node->path = *picked;
            node->name = fs::path(*picked).filename().string();
            if (node->name.empty()) node->name = *picked;
            node->isDir = true;
            node->expanded = true;
            loadChildren(node);
            app.root = node;
            app.selectedPath.clear();
            rebuild();
        }
    };
    sidebar.addChild(button);

    // Scrollable tree area
    View treeContainer;
    treeContainer.style.direction = FlexDirection::Column;
    treeContainer.style.width = Size::full();
    treeContainer.style.flexGrow = 1;
    treeContainer.style.overflowY = Overflow::Auto;

    if (app.root) {
        std::vector<View> rows;
        collectRows(app.root, 0, rows);
        for (auto &r : rows) treeContainer.addChild(r);
    } else {
        Text hint;
        hint.label = "No folder opened";
        hint.color = Color{150, 150, 150, 255};
        hint.fontSize = 13.0f;
        hint.style.margin = EdgeInsets::all(8);
        treeContainer.addChild(hint);
    }
    sidebar.addChild(treeContainer);
    root.addChild(sidebar);

    // ---- Content area ----
    View content;
    content.style.flexGrow = 1;
    content.style.height = Size::full();
    content.style.padding = EdgeInsets::all(16);
    content.style.backgroundColor = Color{255, 255, 255, 255};
    Text contentText;
    contentText.label = app.selectedPath.empty() ? std::string("Select a file")
                                                  : app.selectedPath;
    contentText.fontSize = 14.0f;
    content.addChild(contentText);
    root.addChild(content);

    return root;
}

void rebuild() {
    if (app.ui) app.ui->setRoot(buildRootView());
}

int main() {
    LiteUI ui("File Explorer Example",1000, 650);
    app.ui = &ui;
    ui.setRoot(buildRootView());
    ui.run();
    return 0;
}