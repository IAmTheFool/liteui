// liteui_editor_tabs.hpp

#pragma once

#include "liteui.hpp"
#include "liteui_editor_ext.hpp"
#include "liteui_syntax.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

inline std::string editorTitleFromPath(const std::string &path) {
  size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

// liteui.hpp ships openFilePicker/saveFilePicker but had no folder-picker
// when this file was first written — this adds one the same way (shell
// out to zenity/kdialog on Linux, the COM file dialog with
// FOS_PICKFOLDERS on Windows), reusing liteui_filepicker's existing
// commandExists/shellQuote helpers rather than duplicating them.
//
// Named openWorkspaceFolderDialog rather than the more obvious
// "openFolderPicker" specifically to avoid colliding with a same-named
// function some projects' own liteui.hpp may already have added
// independently — a plain function name this generic is exactly the kind
// of thing two people extending the same library end up picking
// separately, and a duplicate-definition error from that is a lot more
// confusing to debug than this comment is to read.
inline std::optional<std::string>
openWorkspaceFolderDialog(const std::string &title = "Open Folder") {
#if defined(_WIN32)
  HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                             COINIT_DISABLE_OLE1DDE);
  bool weInitialized = coHr == S_OK;
  IFileOpenDialog *dlg = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
  if (FAILED(hr)) {
    if (weInitialized)
      CoUninitialize();
    return std::nullopt;
  }
  dlg->SetTitle(toWide(title).c_str());
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts |
                  FOS_PICKFOLDERS); // the one difference from openFilePicker
  std::optional<std::string> result;
  hr = dlg->Show(nullptr);
  if (SUCCEEDED(hr)) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr,
                                      nullptr);
        std::string s(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
        if (len > 0)
          WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), len, nullptr,
                              nullptr);
        result = std::move(s);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  if (weInitialized)
    CoUninitialize();
  return result;
#else
  using namespace liteui_filepicker;
  std::string cmd;
  if (commandExists("zenity"))
    cmd = "zenity --file-selection --directory --title=" + shellQuote(title);
  else if (commandExists("kdialog"))
    cmd = "kdialog --getexistingdirectory . --title " + shellQuote(title);
  else
    return std::nullopt;
  cmd += " 2>/dev/null";
  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe)
    return std::nullopt;
  std::string result;
  char buf[1024];
  while (fgets(buf, sizeof(buf), pipe))
    result += buf;
  int rc = pclose(pipe);
  if (rc != 0 || result.empty())
    return std::nullopt;
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();
  return result;
#endif
}

// One entry in the workspace folder tree. Owned by TabbedEditor and
// persisted across rebuild()s (unlike the View tree itself, which is torn
// down and rebuilt every time) — see the note on rebuild() for why that
// makes raw FileTreeNode* safe to capture in click handlers.
struct FileTreeNode {
  std::string name;
  std::string fullPath;
  bool isDir = false;
  bool expanded = false;
  bool childrenLoaded = false; // directories are scanned lazily, on first
                               // expand, so opening a large workspace
                               // folder doesn't recursively walk the
                               // entire tree (or loop forever on a
                               // symlink cycle) up front
  std::vector<FileTreeNode> children;
};

// One open buffer. Kept alive independently of the View tree (which gets
// torn down and rebuilt on every structural change — see the file header)
// so a document's content and editing state are never tied to any
// particular tree instance.
struct EditorDocument {
  std::string title = "untitled";
  std::string path;
  bool hasPath = false;
  // Soft-delete rather than actually erasing from the vector: several
  // Dynamic<> closures captured in the *previous* tree's Views (which may
  // still be mid-callback when a close happens — e.g. a close button's
  // own onClick) close over indices into docs_. Erasing would invalidate
  // those indices out from under a callback that's still running: rebuild()
  // simply skips closed documents rather than shrinking the vector.
  bool closed = false;
  std::shared_ptr<CodeEditorState> state = std::make_shared<CodeEditorState>();
  std::shared_ptr<bool> modified = std::make_shared<bool>(false);
  // Always non-null (even for a plain-text/unknown extension, where it
  // just has lang == nullptr and applyHighlighting() no-ops on it) so
  // callers never need to null-check it. Kept per-document rather than
  // rebuilt from scratch each time so its incremental token cache
  // (SyntaxHighlighter::cache) survives tab open/close/switch rebuilds —
  // the same reasoning as keeping `state` per-document instead of
  // recreating it.
  std::shared_ptr<SyntaxHighlighter> highlighter = makeHighlighter(nullptr);
};

// Scans one directory level into node.children, directories first then
// alphabetical (case-insensitive). No-op if already loaded. Free function
// (not a TabbedEditor method) so it's independently testable and reusable.
inline void loadFileTreeChildren(FileTreeNode &node) {
  if (node.childrenLoaded)
    return;
  node.childrenLoaded = true;
  node.children.clear();
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(node.fullPath, ec)) {
    if (ec)
      break;
    FileTreeNode child;
    child.name = entry.path().filename().string();
    child.fullPath = entry.path().string();
    std::error_code ec2;
    child.isDir = entry.is_directory(ec2);
    node.children.push_back(std::move(child));
  }
  std::sort(node.children.begin(), node.children.end(),
            [](const FileTreeNode &a, const FileTreeNode &b) {
              if (a.isDir != b.isDir)
                return a.isDir > b.isDir; // directories before files
              std::string al = a.name, bl = b.name;
              std::transform(al.begin(), al.end(), al.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              std::transform(bl.begin(), bl.end(), bl.begin(),
                             [](unsigned char c) { return std::tolower(c); });
              return al < bl;
            });
}

class TabbedEditor {
public:
  TabbedEditor(int width = 1100, int height = 700,
               const std::string &windowTitle = "liteui code editor")
      : ui_(width, height, windowTitle),
        activeIndex_(std::make_shared<int>(-1)) {
    newDocument();
    buildRoot(); // built once; tab strip + editor stack reconcile themselves
    setupShortcuts();
  }

  // One poll before the loop starts, so any openFile()/newDocument() the
  // caller made between construction and run() is reconciled into the tree
  // before the first paint. Once the loop is running, LiteUI's own
  // pollAndRelayout() (after every dispatched event) handles it. Safe here
  // precisely because it is NOT inside a handler — see closeTab().
  void run() {
    ui_.requestRepaint();
    ui_.run();
  }

  // Opens a file into a new tab, or focuses it if it's already open.
  void openFile(const std::string &path) {
    for (size_t i = 0; i < docs_.size(); ++i) {
      if (!docs_[i].closed && docs_[i].hasPath && docs_[i].path == path) {
        *activeIndex_ = static_cast<int>(i);
        return;
      }
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
      return; // TODO surface a real error dialog/status message
    std::ostringstream ss;
    ss << in.rdbuf();

    EditorDocument doc;
    doc.path = path;
    doc.hasPath = true;
    doc.title = editorTitleFromPath(path);
    doc.highlighter = makeHighlighter(languageForExtension(path));
    splitLinesInto(ss.str(), doc.state->lines);
    doc.state->cursor = {0, 0};

    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  void newDocument() {
    EditorDocument doc;
    doc.title = "untitled-" + std::to_string(++untitledCounter_);
    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // forcePickPath=true implements Save As.
  void saveActive(bool forcePickPath = false) {
    EditorDocument *doc = activeDoc();
    if (!doc)
      return;
    if (!doc->hasPath || forcePickPath) {
      auto picked =
          saveFilePicker("Save File", doc->title,
                         {{"Text", "*.txt"}, {"All Files", "*"}}, "txt");
      if (!picked)
        return;
      doc->path = *picked;
      doc->hasPath = true;
      doc->title = editorTitleFromPath(doc->path);
      // Save As can turn an untitled buffer into e.g. "foo.py" — pick up
      // a language for it now rather than leaving it permanently plain.
      doc->highlighter = makeHighlighter(languageForExtension(doc->path));
    }
    std::ofstream out(doc->path, std::ios::binary);
    out << joinLines(doc->state->lines);
    *doc->modified = false;
    // The tab label is a Dynamic<std::string> reading docs_[idx] live (see
    // buildTab), so the modified-dot and any Save-As title change appear on
    // the next poll with no tree rebuild.
  }

  void closeTab(size_t i) {
    if (i >= docs_.size() || docs_[i].closed)
      return;
    docs_[i].closed = true;
    // TODO prompt to save if *docs_[i].modified before closing.
    // The editor View for this doc is about to be dropped by
    // reconcileChildren(), which frees it without going through onBlur —
    // so its caret-blink interval would otherwise tick forever against a
    // state nothing renders.
    if (docs_[i].state->blinkTimerHandle >= 0) {
      ui_.removeInterval(docs_[i].state->blinkTimerHandle);
      docs_[i].state->blinkTimerHandle = -1;
    }
    docs_[i].state->focused = false;

    if (static_cast<int>(i) == *activeIndex_) {
      int next = -1;
      for (int k = static_cast<int>(i) - 1; k >= 0; --k)
        if (!docs_[static_cast<size_t>(k)].closed) {
          next = k;
          break;
        }
      if (next < 0)
        for (size_t k = i + 1; k < docs_.size(); ++k)
          if (!docs_[k].closed) {
            next = static_cast<int>(k);
            break;
          }
      *activeIndex_ = next;
    }
    if (std::all_of(docs_.begin(), docs_.end(),
                    [](const EditorDocument &d) { return d.closed; }))
      newDocument();
  }

  // ---- introspection (status bar, tests, etc.) ----
  int activeIndex() const { return *activeIndex_; }
  size_t openDocumentCount() const {
    return static_cast<size_t>(
        std::count_if(docs_.begin(), docs_.end(),
                      [](const EditorDocument &d) { return !d.closed; }));
  }
  const EditorDocument *documentAt(size_t i) const {
    return i < docs_.size() ? &docs_[i] : nullptr;
  }
  const EditorDocument *activeDocument() const {
    return (*activeIndex_ >= 0 &&
            static_cast<size_t>(*activeIndex_) < docs_.size())
               ? &docs_[static_cast<size_t>(*activeIndex_)]
               : nullptr;
  }

  void nextTab(int direction) { // +1 or -1, wraps, skips closed tabs
    std::vector<size_t> open;
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed)
        open.push_back(i);
    if (open.size() < 2)
      return;
    auto it =
        std::find(open.begin(), open.end(), static_cast<size_t>(*activeIndex_));
    size_t pos =
        (it == open.end()) ? 0 : static_cast<size_t>(it - open.begin());
    size_t n = open.size();
    pos = (pos + static_cast<size_t>(direction) + n) % n;
    *activeIndex_ = static_cast<int>(open[pos]);
  }

private:
  LiteUI ui_;
  std::vector<EditorDocument> docs_;
  std::shared_ptr<int> activeIndex_; // shared so Dynamic<> closures in the
                                     // *current* tree can read it without
                                     // caring that the tree gets replaced
                                     // out from under them on every rebuild
  int untitledCounter_ = 0;

  std::shared_ptr<int> openMenuIndex_ = std::make_shared<int>(-1);

  // Which activity-bar item is selected: 0 = Explorer, 1 = Search, -1 =
  // panel collapsed. shared_ptr for the same reason activeIndex_ is —
  // every Dynamic<> in the tree reads it live.
  std::shared_ptr<int> activityIndex_ = std::make_shared<int>(0);

  // Terminal panel height, resizable via its own horizontal divider — same
  // shared_ptr/clamp pattern as sidePanelWidth_, just on the vertical axis.
  std::shared_ptr<float> terminalHeight_ = std::make_shared<float>(180.0f);
  static constexpr float kMinTerminalHeight = 80.0f;
  static constexpr float kMaxTerminalHeight = 400.0f;
  static constexpr float kHDividerHeight = 6.0f;

  // Resizable side panel width, shared with the divider's drag handler and
  // the panel's own Dynamic<Size> width — same reasoning as activityIndex_:
  // every closure that reads it needs to survive this tree being rebuilt.
  std::shared_ptr<float> sidePanelWidth_ = std::make_shared<float>(240.0f);
  static constexpr float kMinSidePanelWidth = 160.0f;
  static constexpr float kMaxSidePanelWidth = 480.0f;
  static constexpr float kSideDividerWidth = 6.0f;

  // Named so the pane-visibility checks in buildExplorerPane()/
  // buildSearchPane() below can't silently drift out of sync with
  // activityItems()'s ids if the list is ever reordered or extended.
  static constexpr int kExplorerActivityId = 0;
  static constexpr int kSearchActivityId = 1;

  // One entry per sidebar view. Shared by buildActivityBar() (which
  // renders the icon column) and buildSidePanel() (whose header needs the
  // matching full name) so the id -> name mapping lives in exactly one
  // place — previously the panel header had its own hardcoded ternary
  // that would silently show the wrong name for any activity beyond the
  // first two.
  struct ActivityItem {
    int id;
    std::string label;   // short glyph shown in the bar
    std::string tooltip; // full name shown in the bar's tooltip and as
                         // the side panel's header
  };
  static const std::vector<ActivityItem> &activityItems() {
    static const std::vector<ActivityItem> items = {
        {kExplorerActivityId, "E", "Explorer"},
        {kSearchActivityId, "S", "Search"},
    };
    return items;
  }

  // Explorer state: a real expand/collapse tree rooted at the opened
  // workspace folder, matching VS Code's explorer. explorerRoot_ is
  // nullopt until a folder is opened; each FileTreeNode lazily loads its
  // own children the first time it's expanded (see loadFileTreeChildren,
  // already used this way by loadFileTreeChildren's own callers).
  std::string workspaceRoot_;
  std::optional<FileTreeNode> explorerRoot_;

  void setWorkspaceRoot(const std::string &p) {
    if (p.empty())
      return;
    workspaceRoot_ = p;
    FileTreeNode root;
    root.fullPath = p;
    root.name = editorTitleFromPath(p);
    root.isDir = true;
    root.expanded = true; // VS Code opens a freshly-added folder expanded
    loadFileTreeChildren(root);
    explorerRoot_ = std::move(root);
  }

  void openWorkspaceFolder() {
    auto picked = openWorkspaceFolderDialog("Open Folder");
    if (picked)
      setWorkspaceRoot(*picked);
  }

  // DFS by full path from explorerRoot_, reporting the match's depth
  // (root = 0) via outDepth so buildExplorerRow can indent without
  // tracking depth itself. Returns a non-owning pointer straight into
  // explorerRoot_'s tree, so the caller can flip ->expanded on it.
  static FileTreeNode *findExplorerNodeIn(FileTreeNode &node,
                                          const std::string &path, int depth,
                                          int &outDepth) {
    if (node.fullPath == path) {
      outDepth = depth;
      return &node;
    }
    for (auto &c : node.children)
      if (FileTreeNode *hit = findExplorerNodeIn(c, path, depth + 1, outDepth))
        return hit;
    return nullptr;
  }
  FileTreeNode *findExplorerNode(const std::string &path, int &outDepth) {
    if (!explorerRoot_)
      return nullptr;
    return findExplorerNodeIn(*explorerRoot_, path, 0, outDepth);
  }

  // Flattens the currently-expanded part of the tree into a list of full
  // paths, depth-first — this is the explorer's keysSource. Collapsing a
  // folder simply removes its subtree's paths from this list on the next
  // poll, so reconcileChildren() frees exactly those rows and nothing
  // else; expanding one adds them back, freshly built.
  static void collectExplorerKeys(const FileTreeNode &node,
                                  std::vector<std::string> &out) {
    out.push_back(node.fullPath);
    if (node.isDir && node.expanded)
      for (const auto &c : node.children)
        collectExplorerKeys(c, out);
  }
  std::vector<std::string> explorerKeys() const {
    std::vector<std::string> keys;
    if (explorerRoot_)
      collectExplorerKeys(*explorerRoot_, keys);
    return keys;
  }

  EditorDocument *activeDoc() {
    if (*activeIndex_ < 0 || static_cast<size_t>(*activeIndex_) >= docs_.size())
      return nullptr;
    return &docs_[static_cast<size_t>(*activeIndex_)];
  }

  static void splitLinesInto(const std::string &text,
                             std::vector<std::string> &out) {
    out.clear();
    std::string cur;
    for (char c : text) {
      if (c == '\n') {
        out.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    out.push_back(cur);
    if (out.empty())
      out.push_back(std::string());
  }

  // One row in the explorer tree. `key` is a node's full path, looked up
  // fresh on every build (rather than captured by value) so a row that
  // reconcileChildren reuses across an unrelated expand/collapse
  // elsewhere in the tree still reflects its own current depth/expanded
  // state and the active tab's highlight correctly.
  View buildExplorerRow(const std::string &key) {
    int depth = 0;
    FileTreeNode *node = findExplorerNode(key, depth);
    bool isDir = node && node->isDir;
    bool expanded = node && node->expanded;
    std::string name = node ? node->name : key;
    const EditorDocument *active = activeDocument();
    bool isActive = !isDir && active && active->hasPath && active->path == key;

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};
    row.style.backgroundColor =
        isActive ? Color{213, 228, 249} : Color{243, 243, 243};
    row.style.hoverColor = Color{226, 226, 226};
    row.onClick = [this, key] {
      int d = 0;
      FileTreeNode *n = findExplorerNode(key, d);
      if (!n)
        return;
      if (n->isDir) {
        if (!n->childrenLoaded)
          loadFileTreeChildren(*n);
        n->expanded = !n->expanded;
      } else {
        openFile(key);
      }
    };

    Text chevron;
    chevron.label = isDir ? (expanded ? std::string("\xE2\x96\xBE ")  // ▾
                                      : std::string("\xE2\x96\xB8 ")) // ▸
                          : std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = Color{110, 110, 110};
    row.addChild(chevron);

    Text label;
    label.label = name;
    label.fontSize = 13;
    label.color = isDir ? Color{40, 40, 40} : Color{70, 70, 70};
    row.addChild(label);
    return row;
  }

  View buildExplorerPane() {
    auto act = activityIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.width = Size::full();
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = Color{243, 243, 243};
    pane.style.display = [act]() -> Display {
      return *act == kExplorerActivityId ? Display::Flex : Display::None;
    };

    View openBtn;
    openBtn.style.width = Size::full();
    openBtn.style.padding = EdgeInsets{6, 12, 6, 12};
    openBtn.style.alignItems = Align::Center;
    openBtn.style.backgroundColor = Color{243, 243, 243};
    openBtn.style.hoverColor = Color{226, 226, 226};
    openBtn.style.display = [this]() -> Display {
      return workspaceRoot_.empty() ? Display::Flex : Display::None;
    };
    openBtn.onClick = [this] { openWorkspaceFolder(); };
    Text openLabel;
    openLabel.label = std::string("Open Folder...");
    openLabel.fontSize = 13;
    openLabel.color = Color{40, 40, 40};
    openBtn.addChild(openLabel);
    pane.addChild(std::move(openBtn));

    // Keyed the same way the tab strip is: navigating into a folder
    // changes the key list, and only the rows that actually differ get
    // built. Scrolls on its own so a big directory doesn't stretch the
    // panel past the window.
    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.backgroundColor = Color{243, 243, 243};
    list.style.overflowY = Overflow::Auto;
    list.keysSource = [this] { return explorerKeys(); };
    list.itemBuilder = [this](const std::string &key) {
      return buildExplorerRow(key);
    };
    pane.addChild(std::move(list));
    return pane;
  }

  // Placeholder for now — the per-document Ctrl+F overlay in
  // liteui_editor_ext.hpp is still where searching actually happens.
  View buildSearchPane() {
    auto act = activityIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.width = Size::full();
    pane.style.flexGrow = 1;
    pane.style.padding = EdgeInsets{4, 12, 4, 12};
    pane.style.backgroundColor = Color{243, 243, 243};
    pane.style.display = [act]() -> Display {
      return *act == kSearchActivityId ? Display::Flex : Display::None;
    };

    Text hint;
    hint.label = std::string("Press Ctrl+F in the editor to search the "
                             "current file.");
    hint.style.width = Size::full();
    hint.fontSize = 12;
    hint.color = Color{110, 110, 110};
    pane.addChild(hint);
    return pane;
  }

  // The expanding half of the sidebar. Hidden entirely (Display::None, so
  // it reserves no space) while no activity item is selected.
  View buildSidePanel() {
    auto act = activityIndex_;

    View panel;
    panel.style.direction = FlexDirection::Column;
    {
      auto widthPtr = sidePanelWidth_;
      panel.style.width = [widthPtr]() -> Size {
        return Size::pixel(*widthPtr);
      };
    }
    panel.style.height = Size::full();
    panel.style.backgroundColor = Color{243, 243, 243};
    panel.style.padding = EdgeInsets{8, 0, 8, 0};
    panel.style.gap = 2;

    panel.style.display = [act]() -> Display {
      return *act >= 0 ? Display::Flex : Display::None;
    };

    Text header;
    header.label = std::function<std::string()>([act]() -> std::string {
      for (const auto &activity : activityItems())
        if (activity.id == *act) {
          std::string name = activity.tooltip;
          for (auto &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
          return name;
        }
      return "";
    });
    header.style.padding = EdgeInsets{10, 12, 6, 12};
    header.style.width = Size::full();
    header.fontSize = 11;
    header.color = Color{110, 110, 110};
    panel.addChild(header);

    panel.addChild(buildExplorerPane());
    panel.addChild(buildSearchPane());
    return panel;
  }

  // Drag handle between the side panel and the editor area. Collapses to
  // 0 width alongside the panel itself (same activityIndex_ check) so it's
  // neither visible nor hit-testable while the panel is closed — matching
  // vDivider's behavior in the plain liteui example.
  View buildSideDivider() {
    auto act = activityIndex_;
    auto widthPtr = sidePanelWidth_;

    View divider;
    divider.style.height = Size::full();
    divider.style.flexShrink = 0;
    divider.style.backgroundColor = Color{215, 215, 215};
    divider.style.hoverColor = Color{80, 80, 220};

    divider.style.width = [act]() -> Size {
      return Size::pixel(*act >= 0 ? kSideDividerWidth : 0.0f);
    };

    // Same self-centering pattern as main.cpp's updateSidebarWidth: each
    // onPressAt/onDragTo call reports the pointer's position local to the
    // divider, and a relayout runs after every one, so nudging the width
    // by (localX - half the divider's own width) converges to a normal
    // drag-to-resize even though only local coordinates are available.
    auto updateWidth = [widthPtr](float localX) {
      float next = *widthPtr + localX - kSideDividerWidth / 2.0f;
      *widthPtr = std::clamp(next, kMinSidePanelWidth, kMaxSidePanelWidth);
    };
    divider.onPressAt = [updateWidth](float localX, float) {
      updateWidth(localX);
    };
    divider.onDragTo = [updateWidth](float localX, float) {
      updateWidth(localX);
    };
    return divider;
  }
  // Drag handle between the editor stack and the terminal panel. Always
  // visible/draggable (the terminal itself has no open/close toggle here,
  // matching the plain liteui example's rightColumn layout) — see
  // buildTerminalPanel() below for the panel it resizes.
  View buildHDivider() {
    auto heightPtr = terminalHeight_;

    View divider;
    divider.style.width = Size::full();
    divider.style.flexGrow = 0;
    divider.style.flexShrink = 0;
    divider.style.height = Size::pixel(kHDividerHeight);
    divider.style.backgroundColor = Color{210, 210, 210};
    divider.style.hoverColor = Color{80, 80, 220};

    // Same self-centering drag pattern as buildSideDivider()/main.cpp's
    // updateTerminalHeight, just on the vertical axis: the terminal grows
    // upward as the pointer moves down, hence the subtraction.
    auto updateHeight = [heightPtr](float localY) {
      float next = *heightPtr - (localY - kHDividerHeight / 2.0f);
      *heightPtr = std::clamp(next, kMinTerminalHeight, kMaxTerminalHeight);
    };
    divider.onPressAt = [updateHeight](float, float localY) {
      updateHeight(localY);
    };
    divider.onDragTo = [updateHeight](float, float localY) {
      updateHeight(localY);
    };
    return divider;
  }

  // Terminal panel: fixed-looking placeholder content (a title + a bare
  // prompt), sized by terminalHeight_ via buildHDivider() above. Same
  // dark palette as the plain liteui example's `terminal` View.
  View buildTerminalPanel() {
    auto heightPtr = terminalHeight_;

    View terminal;
    terminal.style.width = Size::full();
    terminal.style.flexGrow = 0;
    terminal.style.flexShrink = 0;
    terminal.style.direction = FlexDirection::Column;
    terminal.style.backgroundColor = Color{24, 24, 24};
    terminal.style.padding = EdgeInsets::all(10.0f);
    terminal.style.overflowY = Overflow::Hidden;
    terminal.style.height = [heightPtr]() -> Size {
      return Size::pixel(*heightPtr);
    };

    Text title;
    title.label = std::string("TERMINAL");
    title.fontSize = 12;
    title.fontWeight = FontWeight::SemiBold;
    title.color = Color{180, 180, 180};
    terminal.addChild(title);

    Text prompt;
    prompt.label = std::string("$ ");
    prompt.fontSize = 13;
    prompt.fontFamily = "monospace";
    prompt.color = Color{100, 220, 130};
    prompt.style.margin = EdgeInsets{8, 0, 0, 0};
    terminal.addChild(prompt);
    return terminal;
  }

  // ---- activity bar: always visible, one row per sidebar view. Text
  // labels rather than icons for now, so the column is wide enough to
  // read them. Clicking the selected item again collapses the panel,
  // matching VS Code.
  View buildActivityBar() {
    auto act = activityIndex_;

    const auto &activities = activityItems();

    View bar;
    bar.style.direction = FlexDirection::Column;
    bar.style.width = Size::pixel(44);
    bar.style.flexShrink = 0;
    bar.style.height = Size::full();
    bar.style.backgroundColor = Color{55, 55, 60};
    bar.style.padding = EdgeInsets{8, 0, 8, 0};
    bar.style.gap = 2;

    for (const auto &activity : activities) {
      int idx = activity.id;
      View row;
      row.style.width = Size::full();
      row.style.height = Size::pixel(36);
      row.style.justifyContent = Justify::Center;
      row.style.alignItems = Align::Center;
      row.style.hoverColor = Color{75, 75, 80};
      row.style.backgroundColor = [act, idx]() -> Color {
        return *act == idx ? Color{40, 40, 45} : Color{55, 55, 60};
      };
      row.tooltip = activity.tooltip;
      row.onClick = [act, idx] { *act = (*act == idx) ? -1 : idx; };

      Text label;
      label.label = activity.label;
      label.fontSize = 12;
      label.color = [act, idx]() -> Color {
        return *act == idx ? Color{255, 255, 255} : Color{185, 185, 190};
      };
      row.addChild(label);
      bar.addChild(std::move(row));
    }
    return bar;
  }

  // ---- status bar ----
  // Built entirely from this TabbedEditor's own state — line:col of the
  // active document's cursor, its detected language (falling back to
  // "Plain Text"), a fixed "UTF-8" (the only encoding this editor ever
  // reads/writes — see EditorDocument/openFile/saveActive), and how many
  // tabs are open. All four fields are Dynamic<std::string>, so they
  // update live via LiteUI's normal checkForUpdates() polling with no
  // rebuild needed on every keystroke — only opening/closing a tab (a
  // real structural change) goes through rebuild().
  View buildStatusBar() {
    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(24);
    bar.style.backgroundColor = Color{235, 235, 235};
    bar.style.padding = EdgeInsets{0, 12, 0, 12};
    bar.style.gap = 16;
    bar.style.flexShrink = 0;

    Text posLabel;
    posLabel.label = std::function<std::string()>([this]() -> std::string {
      const EditorDocument *doc = activeDocument();
      if (!doc)
        return "";
      return "Ln " + std::to_string(doc->state->cursor.line + 1) + ", Col " +
             std::to_string(doc->state->cursor.col + 1);
    });
    posLabel.fontSize = 12;
    posLabel.color = Color{80, 80, 80};
    bar.addChild(posLabel);

    Text langLabel;
    langLabel.label = std::function<std::string()>([this]() -> std::string {
      const EditorDocument *doc = activeDocument();
      if (!doc || !doc->highlighter || !doc->highlighter->lang)
        return "Plain Text";
      return doc->highlighter->lang->name;
    });
    langLabel.fontSize = 12;
    langLabel.color = Color{80, 80, 80};
    bar.addChild(langLabel);

    Text encLabel;
    encLabel.label = std::string("UTF-8");
    encLabel.fontSize = 12;
    encLabel.color = Color{80, 80, 80};
    bar.addChild(encLabel);

    Text countLabel;
    countLabel.label = std::function<std::string()>([this]() -> std::string {
      return std::to_string(openDocumentCount()) + " open";
    });
    countLabel.fontSize = 12;
    countLabel.color = Color{140, 140, 140};
    bar.addChild(countLabel);

    return bar;
  }

  // ---- top menu bar: File/Edit/Selection/View/Go/Run/Terminal/Help,
  // each a real dropdown. One shared backdrop (absolute, full-window)
  // catches "click outside to close" for whichever menu is open, exactly
  // the pattern from the standalone context-menu example: trigger
  // captures its own on-screen position via onLayout, the menu's
  // left/top are computed relative to the backdrop's own captured
  // position, and a single Dynamic<Display>/zIndex pair handles both
  // showing the right menu and keeping it on top.
  View buildMenuBar() {
    struct MenuItemSpec {
      std::string label;
      std::function<void()>
          action; // nullptr = no-op placeholder, still closes the menu
    };
    struct MenuDef {
      std::string title;
      std::vector<MenuItemSpec> items;
    };

    View bar;
    bar.style.direction = FlexDirection::Row;
    bar.style.alignItems = Align::Center;
    bar.style.width = Size::full();
    bar.style.height = Size::pixel(38);
    bar.style.backgroundColor = Color{238, 238, 238};
    bar.style.padding = EdgeInsets{0, 8, 0, 8};
    bar.style.gap = 2;
    bar.style.flexShrink = 0;

    auto openIdx = openMenuIndex_;
    auto backdropX = std::make_shared<float>(0.0f);
    auto backdropY = std::make_shared<float>(0.0f);

    // Full-window backdrop. Transparent (alpha 2, matching the reference)
    // so it's still hit-testable for the outside-click-to-close without
    // visibly dimming anything. Only shown while some menu is open.
    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 100;
    backdrop.style.backgroundColor = Color{255, 255, 255, 2};
    backdrop.style.display = [openIdx]() -> Display {
      return *openIdx >= 0 ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openIdx] { *openIdx = -1; };
    backdrop.onLayout = [backdropX, backdropY](float x, float y, float, float) {
      *backdropX = x;
      *backdropY = y;
    };

    std::vector<MenuDef> menus;
    menus.push_back({"File",
                     {{"New Tab", [this] { newDocument(); }},
                      {"Open File...",
                       [this] {
                         auto p =
                             openFilePicker("Open File", {{"All Files", "*"}});
                         if (p)
                           openFile(*p);
                       }},
                      {"Open Folder...",
                       [this] {
                         openWorkspaceFolder();
                         *activityIndex_ = 0; // reveal the Explorer
                       }},
                      {"Save", [this] { saveActive(false); }},
                      {"Save As...", [this] { saveActive(true); }},
                      {"Close Tab", [this] {
                         if (*activeIndex_ >= 0)
                           closeTab(static_cast<size_t>(*activeIndex_));
                       }}}});
    menus.push_back({"Edit",
                     {{"Undo", nullptr},
                      {"Redo", nullptr},
                      {"Cut", nullptr},
                      {"Copy", nullptr},
                      {"Paste", nullptr}}});
    menus.push_back({"Selection",
                     {{"Select All", nullptr},
                      {"Expand Selection", nullptr},
                      {"Shrink Selection", nullptr}}});
    menus.push_back({"View", {}});
    menus.push_back({"Go",
                     {{"Next Tab", [this] { nextTab(1); }},
                      {"Previous Tab", [this] { nextTab(-1); }}}});
    menus.push_back(
        {"Run",
         {{"Start Debugging", nullptr}, {"Run Without Debugging", nullptr}}});
    menus.push_back({"Terminal", {{"New Terminal", nullptr}}});
    menus.push_back({"Help", {{"About", nullptr}}});

    for (size_t mi = 0; mi < menus.size(); ++mi) {
      const MenuDef &def = menus[mi];
      size_t idx = mi;

      // Captured live from this trigger's own onLayout, same idea as the
      // reference's triggerX/triggerY/triggerH.
      auto triggerX = std::make_shared<float>(0.0f);
      auto triggerY = std::make_shared<float>(0.0f);
      auto triggerH = std::make_shared<float>(0.0f);

      View trigger;
      trigger.style.height = Size::full();
      trigger.style.padding = EdgeInsets{0, 10, 0, 10};
      trigger.style.alignItems = Align::Center;
      trigger.style.justifyContent = Justify::Center;
      trigger.style.hoverColor = Color{220, 220, 220};
      trigger.style.backgroundColor = [openIdx, idx]() -> Color {
        return static_cast<size_t>(*openIdx) == idx ? Color{210, 210, 210}
                                                    : Color{238, 238, 238};
      };
      trigger.onClick = [openIdx, idx] {
        *openIdx =
            (static_cast<size_t>(*openIdx) == idx) ? -1 : static_cast<int>(idx);
      };
      trigger.onLayout = [triggerX, triggerY, triggerH](float x, float y, float,
                                                        float h) {
        *triggerX = x;
        *triggerY = y;
        *triggerH = h;
      };
      Text label;
      label.label = def.title;
      label.fontSize = 13;
      label.color = Color{50, 50, 50};
      trigger.addChild(label);
      bar.addChild(trigger);

      // Dropdown itself: absolute, positioned relative to the backdrop's
      // own captured origin exactly as in the reference (left/top =
      // trigger position minus backdrop position, plus trigger height
      // for a small drop below the bar).
      View menu;
      menu.style.position = Position::Absolute;
      menu.style.direction = FlexDirection::Column;
      menu.style.width = Size::pixel(200);
      menu.style.backgroundColor = Color{255, 255, 255};
      menu.style.borderWidth = 1.0f;
      menu.style.borderColor = Color{200, 200, 200};
      menu.style.borderRadius = 4.0f;
      menu.style.zIndex = 200;
      menu.style.display = [openIdx, idx]() -> Display {
        return static_cast<size_t>(*openIdx) == idx ? Display::Flex
                                                    : Display::None;
      };
      menu.style.left = [triggerX, backdropX]() {
        return *triggerX - *backdropX;
      };
      menu.style.top = [triggerY, triggerH, backdropY]() {
        return (*triggerY - *backdropY) + *triggerH;
      };

      for (const auto &item : def.items) {
        Text itemLabel;
        itemLabel.label = item.label;
        itemLabel.style.padding = EdgeInsets::all(8);
        itemLabel.fontSize = 13;
        itemLabel.color = Color{40, 40, 40};

        View row;
        row.style.hoverColor = Color{240, 240, 240};
        auto action = item.action;
        row.onClick = [openIdx, action] {
          *openIdx = -1;
          if (action)
            action();
        };
        row.addChild(itemLabel);
        menu.addChild(row);
      }

      backdrop.addChild(menu);
    }

    bar.addChild(backdrop);
    return bar;
  }

  // A document's identity for keyed reconciliation. docs_ is append-only
  // (closing soft-deletes — see EditorDocument::closed), so a document's
  // index never changes for its whole lifetime, which is exactly the
  // stability a key needs. Closed documents are simply absent from the
  // list, and reconcileChildren() frees their rows.
  std::vector<std::string> documentKeys() const {
    std::vector<std::string> keys;
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed)
        keys.push_back(std::to_string(i));
    return keys;
  }
  static size_t keyToIndex(const std::string &key) {
    return static_cast<size_t>(std::stoul(key));
  }

  // One tab in the strip. Built once per document, when its key first
  // appears; everything that can change afterwards (title, modified dot,
  // active background) is a Dynamic<> reading docs_/activeIndex_ live.
  View buildTab(size_t idx) {
    auto activeIndexPtr = activeIndex_;

    View tab;
    tab.style.direction = FlexDirection::Row;
    tab.style.alignItems = Align::Center;
    tab.style.padding = EdgeInsets{6, 10, 6, 10};
    tab.style.gap = 8;
    tab.style.height = Size::full();
    tab.style.backgroundColor = [activeIndexPtr, idx]() -> Color {
      return *activeIndexPtr == static_cast<int>(idx) ? Color{255, 255, 255}
                                                      : Color{225, 225, 225};
    };
    tab.style.hoverColor = Color{240, 240, 240};
    tab.onClick = [activeIndexPtr, idx] {
      *activeIndexPtr = static_cast<int>(idx);
    };

    Text label;
    // Reads docs_[idx] on every poll rather than capturing the title by
    // value: Save As renames a document in place, and without a rebuild
    // there is nothing else left to refresh the label.
    label.label = std::function<std::string()>([this, idx]() -> std::string {
      if (idx >= docs_.size())
        return std::string();
      const EditorDocument &d = docs_[idx];
      return d.title + (*d.modified ? " *" : "");
    });
    label.fontSize = 13;
    label.color = Color{60, 60, 60};
    tab.addChild(label);

    View closeBtn;
    closeBtn.style.width = Size::pixel(16);
    closeBtn.style.height = Size::pixel(16);
    closeBtn.style.justifyContent = Justify::Center;
    closeBtn.style.alignItems = Align::Center;
    closeBtn.style.borderRadius = 3.0f;
    closeBtn.style.hoverColor = Color{210, 210, 210};
    closeBtn.style.backgroundColor = Color{0, 0, 0, 0};
    closeBtn.onClick = [this, idx] { closeTab(idx); };
    Text closeLabel;
    closeLabel.label = std::string("x");
    closeLabel.fontSize = 12;
    closeLabel.color = Color{110, 110, 110};
    closeBtn.addChild(closeLabel);
    tab.addChild(closeBtn);
    return tab;
  }

  // One document's CodeEditor. Hidden rather than destroyed when inactive,
  // same as before — switching tabs only flips this Dynamic<Display>, so
  // the keyed list never reconciles on a tab switch at all.
  View buildEditor(size_t idx) {
    auto activeIndexPtr = activeIndex_;
    EditorDocument &doc = docs_[idx];

    CodeEditor ed;
    ed.style.flexGrow = 1;
    ed.style.display = [activeIndexPtr, idx]() -> Display {
      return *activeIndexPtr == static_cast<int>(idx) ? Display::Flex
                                                      : Display::None;
    };
    ed.style.backgroundColor = Color{255, 255, 255};
    ed.showLineNumbers = true;
    ed.fontFamily = "Monospace";
    ed.resetStateFromText = false; // reuse doc.state as-is
    ed.state = doc.state;
    std::shared_ptr<bool> modifiedFlag = doc.modified;
    ed.onChange = [modifiedFlag](const std::string &) { *modifiedFlag = true; };
    applyHighlighting(ed, doc.highlighter);
    return toCodeEditorView(std::move(ed));
  }

  // Builds the tree once. The tab strip and the editor stack are keyed
  // containers: their children come from keysSource/itemBuilder, and
  // LiteUI's checkForUpdates() reconciles them after every dispatched
  // event. Opening or closing a document costs one build, not a
  // re-rasterization of every tab and every editor in the window.
  void buildRoot() {
    View root;
    root.style.direction = FlexDirection::Column;
    root.style.width = Size::full();
    root.style.height = Size::full();
    root.style.backgroundColor = Color{250, 250, 250};
    root.addChild(buildMenuBar());
    View mainArea;
    mainArea.style.direction = FlexDirection::Row;
    mainArea.style.width = Size::full();
    mainArea.style.flexGrow = 1;

    mainArea.addChild(buildActivityBar());

    mainArea.addChild(buildSidePanel());
    mainArea.addChild(buildSideDivider());

    View editorArea;
    editorArea.style.direction = FlexDirection::Column;
    editorArea.style.height = Size::full();
    editorArea.style.flexGrow = 1;

    // ---- tab strip ----
    View tabBar;
    tabBar.style.direction = FlexDirection::Row;
    tabBar.style.alignItems = Align::Center;
    tabBar.style.width = Size::full();
    tabBar.style.height = Size::pixel(34);
    tabBar.style.backgroundColor = Color{225, 225, 225};
    tabBar.style.overflowX = Overflow::Auto;
    tabBar.style.gap = 2;

    // The keyed tabs live in their own container rather than directly in
    // tabBar: reconcileChildren() replaces a node's children wholesale, so
    // a keyed container can't also hold fixed siblings like the "+" button.
    View tabList;
    tabList.style.direction = FlexDirection::Row;
    tabList.style.alignItems = Align::Center;
    tabList.style.height = Size::full();
    tabList.style.gap = 2;
    tabList.style.backgroundColor = Color{225, 225, 225};
    tabList.keysSource = [this] { return documentKeys(); };
    tabList.itemBuilder = [this](const std::string &key) {
      return buildTab(keyToIndex(key));
    };
    tabBar.addChild(std::move(tabList));
    View newTabBtn;
    newTabBtn.style.width = Size::pixel(28);
    newTabBtn.style.height = Size::full();
    newTabBtn.style.justifyContent = Justify::Center;
    newTabBtn.style.alignItems = Align::Center;
    newTabBtn.style.hoverColor = Color{210, 210, 210};
    newTabBtn.style.backgroundColor = Color{225, 225, 225};
    newTabBtn.onClick = [this] { newDocument(); };
    Text plus;
    plus.label = std::string("+");
    plus.fontSize = 16;
    plus.color = Color{90, 90, 90};
    newTabBtn.addChild(plus);
    tabBar.addChild(newTabBtn);

    editorArea.addChild(tabBar);

    // ... stack (added below, unchanged) sits between tabBar and the
    // terminal — see the two new addChild calls right after it.

    // ---- editor stack: one CodeEditor per open document, keyed the same
    // way as the tabs, all but the active one hidden via Display::None ----
    View stack;
    stack.style.width = Size::full();
    stack.style.flexGrow = 1;

    stack.keysSource = [this] { return documentKeys(); };
    stack.itemBuilder = [this](const std::string &key) {
      return buildEditor(keyToIndex(key));
    };
    editorArea.addChild(stack);

    editorArea.addChild(buildHDivider());
    editorArea.addChild(buildTerminalPanel());

    mainArea.addChild(editorArea);

    root.addChild(mainArea);
    root.addChild(buildStatusBar());
    ui_.setRoot(std::move(root));
  }

  bool shortcutsInstalled_ = false;
  void setupShortcuts() {
    if (shortcutsInstalled_)
      return;
    shortcutsInstalled_ = true;

    KeyModifiers ctrl;
    ctrl.ctrl = true;
    KeyModifiers ctrlShift;
    ctrlShift.ctrl = true;
    ctrlShift.shift = true;

    ui_.addShortcut(ctrl, Key::T, [this] { newDocument(); });
    ui_.addShortcut(ctrl, Key::W, [this] {
      if (*activeIndex_ >= 0)
        closeTab(static_cast<size_t>(*activeIndex_));
    });
    ui_.addShortcut(ctrl, Key::O, [this] {
      auto picked = openFilePicker("Open File", {{"All Files", "*"}});
      if (picked)
        openFile(*picked);
    });
    ui_.addShortcut(ctrl, Key::S, [this] { saveActive(false); });
    ui_.addShortcut(ctrlShift, Key::S, [this] { saveActive(true); });
    ui_.addShortcut(ctrl, Key::Tab, [this] { nextTab(1); });
    ui_.addShortcut(ctrlShift, Key::Tab, [this] { nextTab(-1); });
    ui_.addShortcut(ctrl, Key::PageDown, [this] { nextTab(1); });
    ui_.addShortcut(ctrl, Key::PageUp, [this] { nextTab(-1); });
  }

  bool commandsInstalled_ = false;
};