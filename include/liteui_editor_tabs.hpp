// liteui_editor_tabs.hpp

#pragma once

#include "liteui.hpp"
#include "liteui_editor_ext.hpp"
#include "liteui_syntax.hpp"
#include "liteui_terminal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
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

  // True for the VS-Code-style landing tab (see TabbedEditor::
  // newWelcomeTab()). Such a document has no real buffer or backing
  // file — buildEditor() renders it via buildWelcomePane() instead of a
  // CodeEditor, and saveActive()/the status bar skip it accordingly.
  bool isWelcome = false;
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
    newWelcomeTab();
    spawnNewTerminal(); // start with one terminal, like VS Code's default
    ui_.addInterval(33, [this] {
      for (auto &t : terminals_)
        t.state->pollOutput();
    });
    buildRoot(); // built once; tab strip + editor stack reconcile themselves
    setupShortcuts();
  }

  ~TabbedEditor() {
    for (auto &t : terminals_)
      t.state->shutdown();
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

  // Opens `path` (or focuses its existing tab) and moves the cursor to
  // `line` (0-based). Used by workspace search results — a plain
  // openFile() either resets the cursor to (0,0) for a fresh tab or
  // leaves it untouched for an already-open one, neither of which is
  // what a "jump to this match" click wants.
  void openFileAtLine(const std::string &path, size_t line) {
    openFile(path);
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath || doc.path != path)
        continue;
      if (line < doc.state->lines.size()) {
        doc.state->cursor = {line, 0};
        doc.state->selectionAnchor.reset();
        doc.state->noteCursorMoved();
        doc.state->dirty = true;
      }
      break;
    }
  }

  void newDocument() {
    EditorDocument doc;
    doc.title = "untitled-" + std::to_string(++untitledCounter_);
    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // A VS-Code-style landing tab shown on first launch (and again if every
  // other tab gets closed — see closeTab()'s all-closed fallback below):
  // no backing file, just Open File / Open Folder / New File shortcuts.
  // Rendered by buildWelcomePane() rather than a CodeEditor — see
  // buildEditor()'s isWelcome branch.
  void newWelcomeTab() {
    EditorDocument doc;
    doc.title = "Welcome";
    doc.isWelcome = true;
    docs_.push_back(std::move(doc));
    *activeIndex_ = static_cast<int>(docs_.size()) - 1;
  }

  // forcePickPath=true implements Save As.
  void saveActive(bool forcePickPath = false) {
    EditorDocument *doc = activeDoc();
    if (!doc || doc->isWelcome)
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
      newWelcomeTab();
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

  // One terminal instance: its own PTY-backed state (see
  // liteui_terminal.hpp) plus the label shown in the list on the right.
  // Index-based keys are fine for now since terminals_ is append-only —
  // switch to a stable per-tab id (like docs_ does) once close support
  // lands and entries can actually disappear from the middle.
  struct TerminalTab {
    std::shared_ptr<liteui_terminal::TerminalState> state =
        std::make_shared<liteui_terminal::TerminalState>();
    std::string label = "powershell"; // default name for now, per spec
  };
  std::vector<TerminalTab> terminals_;
  std::shared_ptr<int> activeTerminalIndex_ = std::make_shared<int>(0);

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
  // Currently selected explorer item (a full path — either a file or a
  // folder). Defaults to the workspace root itself once one is opened,
  // matching VS Code's "root is selected until you click something
  // else" behavior. New File/New Folder target this (see
  // selectedDirectory()) and buildExplorerRow()/buildExplorerHeaderRow()
  // use it to paint the selection highlight.
  std::string selectedPath_;

  // ---- workspace text search (Ctrl+Shift+F pane) ----
  struct WorkspaceSearchMatch {
    std::string path;
    size_t line = 0;      // 0-based
    std::string lineText; // untrimmed, for the snippet preview
  };
  std::string workspaceSearchQuery_;
  std::vector<WorkspaceSearchMatch> workspaceSearchResults_;
  std::shared_ptr<TextInputState> searchInputState_ =
      std::make_shared<TextInputState>();

  std::string workspaceReplaceText_;
  std::shared_ptr<TextInputState> replaceInputState_ =
      std::make_shared<TextInputState>();
  static constexpr size_t kMaxSearchResults = 500;
  static constexpr uintmax_t kMaxSearchFileBytes = 2 * 1024 * 1024; // 2 MB

  // Directory names never worth descending into for a text search —
  // typically huge, generated, or binary-heavy.
  static bool isSearchExcludedDir(const std::string &name) {
    static const std::unordered_set<std::string> kExcluded = {
        ".git", "node_modules", "build", "out", ".cache", "dist", ".vs"};
    return kExcluded.count(name) != 0;
  }

  // Recursively walks workspaceRoot_, doing a case-insensitive substring
  // search of `query` across every line of every file it can read as
  // text. Deliberately simple (phase 1): no regex, no whole-word/case
  // toggle, no incremental indexing — a linear rescan on every keystroke,
  // capped by kMaxSearchResults/kMaxSearchFileBytes so a huge or
  // pathological workspace can't hang the UI.
  void runWorkspaceSearch(const std::string &query) {
    workspaceSearchQuery_ = query;
    workspaceSearchResults_.clear();
    if (query.empty() || workspaceRoot_.empty())
      return;
    std::string needle = asciiLower(query);

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        workspaceRoot_,
        std::filesystem::directory_options::skip_permission_denied, ec);
    std::filesystem::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
      if (workspaceSearchResults_.size() >= kMaxSearchResults)
        break;
      const std::filesystem::directory_entry &entry = *it;

      std::error_code typeEc;
      if (entry.is_directory(typeEc)) {
        if (isSearchExcludedDir(entry.path().filename().string()))
          it.disable_recursion_pending();
        continue;
      }
      if (typeEc || !entry.is_regular_file(typeEc))
        continue;

      std::error_code sizeEc;
      uintmax_t sz = entry.file_size(sizeEc);
      if (sizeEc || sz > kMaxSearchFileBytes)
        continue;

      std::ifstream in(entry.path(), std::ios::binary);
      if (!in)
        continue;
      std::ostringstream ss;
      ss << in.rdbuf();
      std::string content = ss.str();
      // Crude binary-file guard: real text files essentially never
      // contain a NUL byte.
      if (content.find('\0') != std::string::npos)
        continue;

      std::vector<std::string> lines;
      splitLinesInto(content, lines);
      std::string path = entry.path().string();
      for (size_t li = 0; li < lines.size(); ++li) {
        std::string hay = asciiLower(lines[li]);
        if (hay.find(needle) == std::string::npos)
          continue;
        workspaceSearchResults_.push_back({path, li, lines[li]});
        if (workspaceSearchResults_.size() >= kMaxSearchResults)
          break;
      }
    }
  }

  // Replaces every case-insensitive occurrence of `needle` in `line`
  // with `replacement`, preserving everything else in the line
  // byte-for-byte. Mirrors the matching logic runWorkspaceSearch already
  // uses (asciiLower + find), so "what counts as a match" stays
  // consistent between the search pass and the replace pass.
  static std::string caseInsensitiveReplaceAll(const std::string &line,
                                               const std::string &needle,
                                               const std::string &replacement) {
    if (needle.empty())
      return line;
    std::string hay = asciiLower(line);
    std::string lneedle = asciiLower(needle);
    std::string result;
    size_t pos = 0;
    while (true) {
      size_t found = hay.find(lneedle, pos);
      if (found == std::string::npos) {
        result += line.substr(pos);
        break;
      }
      result += line.substr(pos, found - pos);
      result += replacement;
      pos = found + needle.size();
    }
    return result;
  }

  // Replaces the query text on one specific search-result line, either
  // through the file's already-open CodeEditorState (so a user with
  // unsaved edits doesn't get overwritten from under them, and undo
  // still works via beginEdit) or, if the file isn't open, directly on
  // disk. Either way the line count can't change (neither the query nor
  // the replacement text can contain '\n' — TextInput is single-line),
  // so every other result's stored line number stays valid; only this
  // line's own content changes. Finishes with a full rescan rather than
  // patching workspaceSearchResults_ in place — replacing may make this
  // line stop matching, or start matching a second time, or (if the
  // replacement text itself contains the query) matter for later
  // results too, and a rescan is the simplest way to stay correct.
  void replaceMatch(size_t idx) {
    if (idx >= workspaceSearchResults_.size() || workspaceSearchQuery_.empty())
      return;
    WorkspaceSearchMatch m = workspaceSearchResults_[idx]; // copy: the
                                                           // vector gets
                                                           // cleared by
                                                           // the rescan
                                                           // below
    bool handledInOpenDoc = false;
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath || doc.path != m.path)
        continue;
      if (m.line < doc.state->lines.size()) {
        std::string replaced = caseInsensitiveReplaceAll(
            doc.state->lines[m.line], workspaceSearchQuery_,
            workspaceReplaceText_);
        if (replaced != doc.state->lines[m.line]) {
          doc.state->beginEdit(false); // its own undo step, like paste/cut
          doc.state->lines[m.line] = replaced;
          doc.state->clampCursor();
          doc.state->dirty = true;
        }
      }
      std::ofstream out(doc.path, std::ios::binary);
      out << joinLines(doc.state->lines);
      *doc.modified = false; // matches saveActive()'s own bookkeeping
      handledInOpenDoc = true;
      break;
    }

    if (!handledInOpenDoc) {
      std::ifstream in(m.path, std::ios::binary);
      if (!in)
        return; // TODO surface a real error dialog/status message
      std::ostringstream ss;
      ss << in.rdbuf();
      std::vector<std::string> lines;
      splitLinesInto(ss.str(), lines);
      if (m.line >= lines.size())
        return;
      lines[m.line] = caseInsensitiveReplaceAll(
          lines[m.line], workspaceSearchQuery_, workspaceReplaceText_);
      std::ofstream out(m.path, std::ios::binary);
      out << joinLines(lines);
    }

    runWorkspaceSearch(workspaceSearchQuery_);
  }

  // Applies the current query -> replacement across every visible search
  // result in one pass, grouped by file so each file is read/written (or
  // its open CodeEditorState edited) exactly once rather than once per
  // match. Unlike replaceMatch(), this deliberately does NOT call
  // runWorkspaceSearch() after every single replacement — for a result
  // set spanning many files that would mean re-walking the entire
  // workspace once per match. Instead it walks workspaceSearchResults_
  // once up front to build a path -> line-numbers map, applies each
  // file's replacements in one shot, and rescans exactly once at the end.
  void replaceAllMatches() {
    if (workspaceSearchResults_.empty() || workspaceSearchQuery_.empty())
      return;

    // path -> the distinct lines within it that matched, in first-seen
    // order. Each WorkspaceSearchMatch is already one entry per matching
    // line (runWorkspaceSearch stops scanning a line the moment it finds
    // one hit — see its inner loop), so no further de-duplication of
    // line numbers is needed here, only grouping by path.
    std::vector<std::string> orderedPaths;
    std::unordered_map<std::string, std::vector<size_t>> linesByPath;
    for (const auto &m : workspaceSearchResults_) {
      auto [it, inserted] = linesByPath.try_emplace(m.path);
      if (inserted)
        orderedPaths.push_back(m.path);
      it->second.push_back(m.line);
    }

    for (const std::string &path : orderedPaths) {
      const std::vector<size_t> &targetLines = linesByPath[path];

      bool handledInOpenDoc = false;
      for (auto &doc : docs_) {
        if (doc.closed || !doc.hasPath || doc.path != path)
          continue;
        bool changedAny = false;
        for (size_t line : targetLines) {
          if (line >= doc.state->lines.size())
            continue;
          std::string replaced = caseInsensitiveReplaceAll(
              doc.state->lines[line], workspaceSearchQuery_,
              workspaceReplaceText_);
          if (replaced != doc.state->lines[line]) {
            if (!changedAny)
              doc.state->beginEdit(false); // one undo step per file
            doc.state->lines[line] = replaced;
            changedAny = true;
          }
        }
        if (changedAny) {
          doc.state->clampCursor();
          doc.state->dirty = true;
        }
        std::ofstream out(doc.path, std::ios::binary);
        out << joinLines(doc.state->lines);
        *doc.modified = false; // matches saveActive()'s own bookkeeping
        handledInOpenDoc = true;
        break;
      }
      if (handledInOpenDoc)
        continue;

      std::ifstream in(path, std::ios::binary);
      if (!in)
        continue; // TODO surface a real error dialog/status message
      std::ostringstream ss;
      ss << in.rdbuf();
      std::vector<std::string> lines;
      splitLinesInto(ss.str(), lines);
      for (size_t line : targetLines) {
        if (line >= lines.size())
          continue;
        lines[line] = caseInsensitiveReplaceAll(
            lines[line], workspaceSearchQuery_, workspaceReplaceText_);
      }
      std::ofstream out(path, std::ios::binary);
      out << joinLines(lines);
    }

    runWorkspaceSearch(workspaceSearchQuery_);
  }

  // One result *slot* (not a keyed row): built once per index, up to
  // kMaxSearchResults of them, and kept mounted for the life of the pane.
  // Everything it shows is a Dynamic<> callback reading
  // workspaceSearchResults_[idx] live, and it hides itself via
  // Display::None once idx falls outside the current result count.
  //
  // Deliberately NOT a keyed list: the previous version rebuilt the
  // results as a keysSource/itemBuilder list, whose key count changes on
  // every keystroke. Any keyed-list reconcile anywhere in the tree makes
  // LiteUI::pollAndRelayout() call invalidateViewPointers() as a safety
  // measure (see its comment in liteui.hpp) — which unconditionally
  // nulls focusedView_, since it can't tell which cached View* pointers
  // are still valid. That silently kicked focus off the search
  // TextInput after every single character, even though the TextInput's
  // own View never moved. A fixed, always-mounted pool of slots sidesteps
  // the problem entirely: nothing structural ever changes while typing.
  View buildSearchResultSlot(size_t idx) {
    View row;
    row.style.direction = FlexDirection::Row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding = EdgeInsets{5, 12, 5, 12};
    row.style.gap = 8;
    row.style.backgroundColor = Color{243, 243, 243};
    row.style.hoverColor = Color{226, 226, 226};
    row.style.display = [this, idx]() -> Display {
      return idx < workspaceSearchResults_.size() ? Display::Flex
                                                  : Display::None;
    };
    row.onClick = [this, idx] {
      if (idx >= workspaceSearchResults_.size())
        return;
      const WorkspaceSearchMatch &m = workspaceSearchResults_[idx];
      openFileAtLine(m.path, m.line);
    };

    View textCol;
    textCol.style.direction = FlexDirection::Column;
    textCol.style.flexGrow = 1;
    textCol.style.gap = 2;

    Text top;
    top.label = std::function<std::string()>([this, idx]() -> std::string {
      if (idx >= workspaceSearchResults_.size())
        return std::string();
      const WorkspaceSearchMatch &m = workspaceSearchResults_[idx];
      return editorTitleFromPath(m.path) + ":" + std::to_string(m.line + 1);
    });
    top.fontSize = 12;
    top.fontWeight = FontWeight::SemiBold;
    top.color = Color{40, 40, 40};
    textCol.addChild(top);

    Text snippetText;
    snippetText.label =
        std::function<std::string()>([this, idx]() -> std::string {
          if (idx >= workspaceSearchResults_.size())
            return std::string(" ");
          std::string snippet =
              trimWhitespace(workspaceSearchResults_[idx].lineText);
          if (snippet.size() > 140)
            snippet = snippet.substr(0, 140) + "...";
          return snippet.empty() ? std::string(" ") : snippet;
        });
    snippetText.fontSize = 12;
    snippetText.color = Color{120, 120, 120};
    snippetText.overflow = TextOverflow::Ellipsis;
    snippetText.wrap = TextWrap::NoWrap;
    textCol.addChild(snippetText);

    row.addChild(std::move(textCol));

    // "R" = replace just this match. Own onClick, so it consumes the
    // click before it ever reaches row.onClick's "open the file" — same
    // pattern as buildTab's closeBtn sitting inside a clickable tab.
    View replaceBtn;
    replaceBtn.style.width = Size::pixel(22);
    replaceBtn.style.height = Size::pixel(22);
    replaceBtn.style.flexShrink = 0;
    replaceBtn.style.justifyContent = Justify::Center;
    replaceBtn.style.alignItems = Align::Center;
    replaceBtn.style.borderRadius = 3.0f;
    replaceBtn.style.backgroundColor = Color{0, 0, 0, 0};
    replaceBtn.style.hoverColor = Color{210, 210, 210};
    replaceBtn.tooltip = "Replace this match";
    replaceBtn.onClick = [this, idx] { replaceMatch(idx); };
    Text replaceLabel;
    replaceLabel.label = std::string("R");
    replaceLabel.fontSize = 11;
    replaceLabel.fontWeight = FontWeight::SemiBold;
    replaceLabel.color = Color{90, 90, 90};
    replaceBtn.addChild(replaceLabel);
    row.addChild(std::move(replaceBtn));

    return row;
  }

  // Inline create-new-item state: while active, the explorer shows a
  // TextInput row (see buildPendingCreateRow) inside parentDir instead of
  // immediately creating a file/folder with a placeholder name. Enter
  // submits (see submitPendingCreate); clicking anywhere else discards it
  // (see checkPendingCreateBlur/cancelPendingCreate).
  struct PendingCreate {
    bool active = false;
    bool isDir = false;
    std::string parentDir;
  };
  PendingCreate pendingCreate_;
  // Whether the input has ever actually received focus since it was
  // created. Clicking N/F doesn't itself focus the new TextInput (liteui
  // has no public API to force keyboard focus — see beginPendingCreate),
  // so the very first "not focused" poll after creation must NOT be
  // read as a blur, or the row would vanish before anyone could click it.
  bool pendingCreateEverFocused_ = false;
  // Rebuilt fresh every time a creation starts or ends (see
  // resetPendingCreate), so a stale blinking caret or leftover typed text
  // from a previously discarded input can never bleed into the next one.
  std::shared_ptr<TextInputState> pendingInputState_ =
      std::make_shared<TextInputState>();
  // Sentinel key spliced into explorerKeys() while a creation is active;
  // chosen so it can never collide with a real filesystem path.
  static constexpr const char *kPendingCreateKey =
      "\x01__liteui_pending_create__";

  // Inline rename state — same shape as PendingCreate, but replaces an
  // *existing* row's rendering instead of adding a new one. explorerKeys()
  // swaps the target node's own key for a rename-sentinel key (see
  // collectExplorerKeys) so reconcileChildren actually notices the change
  // and calls itemBuilder again for that row, rather than reusing the
  // already-built plain row untouched.
  struct PendingRename {
    bool active = false;
    bool isDir = false;
    std::string originalPath;
  };
  PendingRename pendingRename_;
  // Same "hasn't been clicked into yet" guard as pendingCreateEverFocused_.
  bool pendingRenameEverFocused_ = false;
  std::shared_ptr<TextInputState> pendingRenameInputState_ =
      std::make_shared<TextInputState>();
  // Prefix rather than a single fixed sentinel (unlike kPendingCreateKey)
  // since a rename target is a specific existing path, not a generic
  // "one pending slot" — the prefix plus path makes each rename key unique
  // and keeps it out of the way of any real filesystem path.
  static constexpr const char *kPendingRenameKeyPrefix =
      "\x02__liteui_pending_rename__:";

  // Right-click context menu for explorer rows (Open/Rename/Delete for a
  // file, New File/New Folder/Rename/Delete for a folder).
  std::shared_ptr<bool> explorerMenuOpen_ = std::make_shared<bool>(false);
  std::string explorerMenuTargetPath_;
  bool explorerMenuTargetIsDir_ = false;
  std::shared_ptr<float> explorerMenuX_ = std::make_shared<float>(0.0f);
  std::shared_ptr<float> explorerMenuY_ = std::make_shared<float>(0.0f);

  // Delete-confirmation dialog state. deleteDialogOpen_ is a shared_ptr
  // for the same reason activeIndex_/activityIndex_ are — the dialog's
  // own Dynamic<Display>/onClick closures need to read it live across
  // rebuilds. deleteTargetPath_/deleteTargetIsDir_ are captured once,
  // when the dialog opens, so the confirm/cancel buttons always act on
  // whatever was selected at that moment rather than whatever
  // selectedPath_ has drifted to since (which can't actually change
  // while the modal backdrop is up, but this keeps the two concerns
  // separate regardless).
  std::shared_ptr<bool> deleteDialogOpen_ = std::make_shared<bool>(false);
  std::string deleteTargetPath_;
  bool deleteTargetIsDir_ = false;

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
    selectedPath_ = p; // root selected by default
  }

  void openWorkspaceFolder() {
    auto picked = openWorkspaceFolderDialog("Open Folder");
    if (picked)
      setWorkspaceRoot(*picked);
  }

  // Directory New File/New Folder should target: the selected folder
  // itself, or the parent directory of the selected file — falling back
  // to the workspace root when nothing is selected. This is what makes
  // "select a folder, hit N" create inside that folder rather than
  // always at the root, matching VS Code.
  std::string selectedDirectory() {
    if (selectedPath_.empty())
      return workspaceRoot_;
    int depth = 0;
    FileTreeNode *node = findExplorerNode(selectedPath_, depth);
    if (!node)
      return workspaceRoot_;
    if (node->isDir)
      return node->fullPath;
    return std::filesystem::path(node->fullPath).parent_path().string();
  }

  static std::string trimWhitespace(const std::string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
      return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
  }

  // Row's displayed indent depth for a given full path — same "shift by
  // one to hide the root row" adjustment buildExplorerRow already does,
  // factored out so the pending-create row can indent one level deeper
  // than whatever folder it's being created in.
  int explorerDepthForPath(const std::string &path) {
    if (path == workspaceRoot_)
      return -1; // so a direct child of the root lands at depth 0
    int depth = 0;
    FileTreeNode *node = findExplorerNode(path, depth);
    if (!node)
      return -1;
    return std::max(0, depth - 1);
  }

  // Stops the input's blink timer (if it ever started — mirrors the same
  // leak-guard closeTab() already applies to a closed CodeEditorState)
  // and clears all pending-create state, discarding whatever was typed.
  void resetPendingCreate() {
    if (pendingInputState_->blinkTimerHandle >= 0) {
      ui_.removeInterval(pendingInputState_->blinkTimerHandle);
      pendingInputState_->blinkTimerHandle = -1;
    }
    pendingInputState_->focused = false;
    pendingCreate_.active = false;
    pendingCreate_.isDir = false;
    pendingCreate_.parentDir.clear();
    pendingCreateEverFocused_ = false;
    pendingInputState_ = std::make_shared<TextInputState>();
  }

  void cancelPendingCreate() { resetPendingCreate(); }

  // Enter handler: trims the typed name and, if anything's left, creates
  // the file/folder; a trimmed-empty name is treated as a no-op rather
  // than an error. Either way the input closes.
  void submitPendingCreate(const std::string &rawText) {
    if (!pendingCreate_.active)
      return;
    std::string name = trimWhitespace(rawText);
    bool isDir = pendingCreate_.isDir;
    std::string dir = pendingCreate_.parentDir;
    resetPendingCreate(); // close the input either way
    if (name.empty())
      return;

    if (isDir) {
      std::error_code ec;
      std::filesystem::path newDir = std::filesystem::path(dir) / name;
      std::filesystem::create_directory(newDir, ec);
      explorerRefresh();
      selectedPath_ = newDir.string();
    } else {
      std::string path = (std::filesystem::path(dir) / name).string();
      std::ofstream out(path, std::ios::binary);
      out.close();
      explorerRefresh();
      selectedPath_ = path;
      openFile(path);
    }
  }

  // Polled every dispatched event via buildPendingCreateRow's
  // style.disabled hook — checkForUpdates() already re-evaluates every
  // view's Dynamic<bool> disabled field after each click/key, so this
  // rides that instead of needing its own timer. Always returns false;
  // the row is never actually disabled, this is purely a polling hook.
  void checkPendingCreateBlur() {
    if (!pendingCreate_.active)
      return;
    if (pendingInputState_->focused) {
      pendingCreateEverFocused_ = true;
      return;
    }
    // Hasn't been clicked into yet — nothing to blur from.
    if (pendingCreateEverFocused_)
      cancelPendingCreate();
  }

  void beginPendingCreate(bool isDir) {
    if (workspaceRoot_.empty())
      return;
    std::string dir = selectedDirectory();
    // Auto-expand the target folder so the new input row is actually
    // visible where it's being created — matches the old immediate-create
    // behavior of the new item appearing in an already-open directory.
    if (dir != workspaceRoot_) {
      int depth = 0;
      FileTreeNode *node = findExplorerNode(dir, depth);
      if (node && node->isDir) {
        if (!node->childrenLoaded)
          loadFileTreeChildren(*node);
        node->expanded = true;
      }
    }
    resetPendingCreate(); // discard any previous unfinished creation first
    resetPendingRename(); // ...and cancel any in-progress rename too
    pendingCreate_.active = true;
    pendingCreate_.isDir = isDir;
    pendingCreate_.parentDir = dir;
  }

  void explorerNewFile() { beginPendingCreate(false); }
  void explorerNewFolder() { beginPendingCreate(true); }

  // ---- inline rename ----

  void resetPendingRename() {
    if (pendingRenameInputState_->blinkTimerHandle >= 0) {
      ui_.removeInterval(pendingRenameInputState_->blinkTimerHandle);
      pendingRenameInputState_->blinkTimerHandle = -1;
    }
    pendingRenameInputState_->focused = false;
    pendingRename_.active = false;
    pendingRename_.isDir = false;
    pendingRename_.originalPath.clear();
    pendingRenameEverFocused_ = false;
    pendingRenameInputState_ = std::make_shared<TextInputState>();
  }

  void cancelPendingRename() { resetPendingRename(); }

  // Same polling trick as checkPendingCreateBlur — piggybacks on the
  // generic Dynamic<bool> disabled poll so losing focus (clicking
  // elsewhere) cancels the rename.
  void checkPendingRenameBlur() {
    if (!pendingRename_.active)
      return;
    if (pendingRenameInputState_->focused) {
      pendingRenameEverFocused_ = true;
      return;
    }
    if (pendingRenameEverFocused_)
      cancelPendingRename();
  }

  void beginPendingRename(const std::string &path, bool isDir) {
    resetPendingCreate(); // discard any pending N/F creation first
    resetPendingRename();
    pendingRename_.active = true;
    pendingRename_.isDir = isDir;
    pendingRename_.originalPath = path;
  }

  // Updates any open tab(s) whose backing path sits at or under oldPath so
  // a rename doesn't leave a tab silently pointing at a now-missing file —
  // same reasoning/shape as closeTabsUnderPath's use of pathIsUnderOrEqual.
  void renamePathInTabs(const std::string &oldPath, const std::string &newPath,
                        bool isDir) {
    for (auto &doc : docs_) {
      if (doc.closed || !doc.hasPath)
        continue;
      if (!isDir) {
        if (doc.path != oldPath)
          continue;
        doc.path = newPath;
      } else {
        if (!pathIsUnderOrEqual(doc.path, oldPath))
          continue;
        doc.path = newPath + doc.path.substr(oldPath.size());
      }
      doc.title = editorTitleFromPath(doc.path);
      doc.highlighter = makeHighlighter(languageForExtension(doc.path));
    }
  }

  // Enter handler for the rename TextInput: trims the typed name and, if
  // anything's left and it actually differs from the current name,
  // renames on disk, fixes up any open tabs, and refreshes the tree.
  void submitPendingRename(const std::string &rawText) {
    if (!pendingRename_.active)
      return;
    std::string name = trimWhitespace(rawText);
    std::string oldPath = pendingRename_.originalPath;
    bool isDir = pendingRename_.isDir;
    resetPendingRename(); // close the input either way
    if (name.empty())
      return;
    std::string newPath =
        (std::filesystem::path(oldPath).parent_path() / name).string();
    if (newPath == oldPath)
      return;
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    if (ec)
      return; // TODO surface a real error dialog/status message
    renamePathInTabs(oldPath, newPath, isDir);
    explorerRefresh();
    selectedPath_ = newPath;
  }

  // ---- right-click context menu ----

  void openExplorerContextMenu(const std::string &path, bool isDir, float x,
                               float y) {
    explorerMenuTargetPath_ = path;
    explorerMenuTargetIsDir_ = isDir;
    *explorerMenuX_ = x;
    *explorerMenuY_ = y;
    *explorerMenuOpen_ = true;
  }
  void closeExplorerContextMenu() { *explorerMenuOpen_ = false; }

  // Each action reuses the same logic the toolbar buttons/keyboard
  // shortcut already use, just pointed at whatever was right-clicked
  // (selectedPath_ drives selectedDirectory()/beginDeleteConfirm() already,
  // so setting it first is enough — no new targeting logic needed).
  void explorerContextOpen() {
    if (!explorerMenuTargetIsDir_)
      openFile(explorerMenuTargetPath_);
  }
  void explorerContextRename() {
    beginPendingRename(explorerMenuTargetPath_, explorerMenuTargetIsDir_);
  }
  void explorerContextDelete() {
    selectedPath_ = explorerMenuTargetPath_;
    beginDeleteConfirm();
  }
  void explorerContextNewFile() {
    selectedPath_ = explorerMenuTargetPath_;
    explorerNewFile();
  }
  void explorerContextNewFolder() {
    selectedPath_ = explorerMenuTargetPath_;
    explorerNewFolder();
  }

  // Whether `path` is `ancestor` itself or lives somewhere underneath it —
  // used to find tabs open on files inside a folder that's about to be
  // deleted, and to tell whether the current selection just got deleted.
  static bool pathIsUnderOrEqual(const std::string &path,
                                 const std::string &ancestor) {
    if (path == ancestor)
      return true;
    std::string prefix = ancestor;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
      prefix += static_cast<char>(std::filesystem::path::preferred_separator);
    return path.size() > prefix.size() &&
           path.compare(0, prefix.size(), prefix) == 0;
  }

  // Closes every open tab whose backing file sits at or under `path`,
  // before the actual filesystem delete runs — otherwise a deleted
  // document would keep its tab open pointing at a now-missing file.
  void closeTabsUnderPath(const std::string &path) {
    for (size_t i = 0; i < docs_.size(); ++i)
      if (!docs_[i].closed && docs_[i].hasPath &&
          pathIsUnderOrEqual(docs_[i].path, path))
        closeTab(i);
  }

  // Opens the confirmation dialog for whatever's currently selected.
  // No-op if nothing is selected, the workspace root itself is selected
  // (deleting the open folder from under itself isn't supported here),
  // or the dialog is already open.
  void beginDeleteConfirm() {
    if (*deleteDialogOpen_ || selectedPath_.empty() ||
        selectedPath_ == workspaceRoot_)
      return;
    int depth = 0;
    FileTreeNode *node = findExplorerNode(selectedPath_, depth);
    deleteTargetPath_ = selectedPath_;
    deleteTargetIsDir_ = node && node->isDir;
    *deleteDialogOpen_ = true;
  }

  void cancelDeleteConfirm() {
    *deleteDialogOpen_ = false;
    deleteTargetPath_.clear();
    deleteTargetIsDir_ = false;
  }

  // Actually deletes deleteTargetPath_ (recursively, if it's a folder),
  // closing any tabs open under it first and refreshing the tree
  // afterward. Falls back to selecting the workspace root if the
  // deleted path was (or contained) the current selection.
  void confirmDelete() {
    if (deleteTargetPath_.empty()) {
      cancelDeleteConfirm();
      return;
    }
    std::string path = deleteTargetPath_;
    bool isDir = deleteTargetIsDir_;
    cancelDeleteConfirm();

    closeTabsUnderPath(path);

    std::error_code ec;
    if (isDir)
      std::filesystem::remove_all(path, ec);
    else
      std::filesystem::remove(path, ec);

    explorerRefresh();
    if (pathIsUnderOrEqual(selectedPath_, path))
      selectedPath_ = workspaceRoot_;
  }

  // Re-scans a directory node from disk, but only if it was already
  // expanded/loaded — untouched (collapsed, lazy) subtrees are left alone.
  // Expand state is carried over for children that still exist after the
  // rescan, so refreshing doesn't visually collapse everything.
  static void refreshNode(FileTreeNode &node) {
    if (!node.isDir)
      return;
    bool wasLoaded = node.childrenLoaded;
    bool wasExpanded = node.expanded;
    std::vector<FileTreeNode> old = std::move(node.children);
    node.children.clear();
    node.childrenLoaded = false;
    if (wasLoaded) {
      loadFileTreeChildren(node);
      for (auto &c : node.children)
        for (auto &o : old)
          if (o.isDir && c.isDir && o.fullPath == c.fullPath) {
            c.expanded = o.expanded;
            c.childrenLoaded = o.childrenLoaded;
            c.children = std::move(o.children);
            break;
          }
      for (auto &c : node.children)
        if (c.isDir && c.expanded)
          refreshNode(c);
    }
    node.expanded = wasExpanded;
  }

  void explorerRefresh() {
    if (!explorerRoot_)
      return;
    refreshNode(*explorerRoot_);
  }

  static void collapseAllNode(FileTreeNode &node) {
    if (!node.isDir)
      return;
    node.expanded = false;
    for (auto &c : node.children)
      collapseAllNode(c);
  }

  void explorerCollapseAll() {
    if (!explorerRoot_)
      return;
    // Collapse every descendant folder; leave the workspace root itself
    // expanded, matching VS Code's "Collapse All".
    for (auto &c : explorerRoot_->children)
      collapseAllNode(c);
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

  // When a creation is pending inside a given folder, kPendingCreateKey is
  // spliced in as that folder's first "child" key, so the input row always
  // renders as the top item of the folder it's being created in.
  // Root-level creations are handled the same way in explorerKeys() below,
  // since the root itself never gets a collectExplorerKeys() call of its
  // own.
  void collectExplorerKeys(const FileTreeNode &node,
                           std::vector<std::string> &out) const {
    bool renamingThis =
        pendingRename_.active && pendingRename_.originalPath == node.fullPath;
    out.push_back(renamingThis
                      ? std::string(kPendingRenameKeyPrefix) + node.fullPath
                      : node.fullPath);
    if (node.isDir && node.expanded) {
      if (pendingCreate_.active && pendingCreate_.parentDir == node.fullPath)
        out.push_back(kPendingCreateKey);
      for (const auto &c : node.children)
        collectExplorerKeys(c, out);
    }
  }
  std::vector<std::string> explorerKeys() const {
    std::vector<std::string> keys;
    if (!explorerRoot_)
      return keys;
    if (pendingCreate_.active && pendingCreate_.parentDir == workspaceRoot_)
      keys.push_back(kPendingCreateKey);
    // The root folder itself is never shown as a row — only its
    // contents — so start recursion at its children instead of at the
    // root node.
    for (const auto &c : explorerRoot_->children)
      collectExplorerKeys(c, keys);
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
    auto pushLine = [&] {
      if (!cur.empty() && cur.back() == '\r')
        cur.pop_back();
      out.push_back(cur);
      cur.clear();
    };
    for (char c : text) {
      if (c == '\n')
        pushLine();
      else
        cur += c;
    }
    pushLine();
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
    // Root itself is never shown as a row (see explorerKeys), so shift
    // every visible depth up by one to compensate — a top-level file/
    // folder should render at indent 0, not 1.
    depth = std::max(0, depth - 1);
    bool isDir = node && node->isDir;
    bool expanded = node && node->expanded;
    std::string name = node ? node->name : key;
    const EditorDocument *active = activeDocument();
    bool isActive = !isDir && active && active->hasPath && active->path == key;
    bool isSelected = (key == selectedPath_);

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};

    // was: computed once from isSelected/isActive captured at build time.
    // now: recomputed on every checkForUpdates() poll, so a click that only
    // changes selectedPath_ (with no key-list change) still repaints this row.
    row.style.backgroundColor = [this, key, isDir]() -> Color {
      const EditorDocument *active = activeDocument();
      bool isActive =
          !isDir && active && active->hasPath && active->path == key;
      bool isSelected = (key == selectedPath_);
      return isSelected ? Color{197, 220, 250}
             : isActive ? Color{213, 228, 249}
                        : Color{243, 243, 243};
    };
    row.style.hoverColor = Color{226, 226, 226};
    row.onClick = [this, key] {
      selectedPath_ = key;
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

    // Captured via onLayout (same pattern buildMenuBar's own triggers use)
    // so the right-click handler below can turn a local press point into
    // a window-absolute position for the context menu.
    auto rowX = std::make_shared<float>(0.0f);
    auto rowY = std::make_shared<float>(0.0f);
    row.onLayout = [rowX, rowY](float x, float y, float, float) {
      *rowX = x;
      *rowY = y;
    };
    row.onRightPressAt = [this, key, isDir, rowX, rowY](float lx, float ly) {
      selectedPath_ = key;
      openExplorerContextMenu(key, isDir, *rowX + lx, *rowY + ly);
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

  // The inline TextInput row shown while a creation is pending — sits at
  // whatever depth is one deeper than its parent folder's own rows.
  View buildPendingCreateRow() {
    bool isDir = pendingCreate_.isDir;
    int depth = explorerDepthForPath(pendingCreate_.parentDir) + 1;

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};
    row.style.backgroundColor = Color{243, 243, 243};
    // Piggybacks on the generic Dynamic<bool> polling checkForUpdates()
    // already does for every view's `disabled` field — re-evaluated
    // after every dispatched event — purely to detect focus loss on the
    // TextInput below. The row is never actually disabled; this always
    // returns false.
    row.disabled = [this]() -> bool {
      checkPendingCreateBlur();
      return false;
    };

    Text chevron;
    chevron.label = std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = Color{110, 110, 110};
    row.addChild(chevron);

    TextInput input;
    input.style.flexGrow = 1;
    input.style.height = Size::pixel(20);
    input.fontSize = 13;
    input.placeholder = isDir ? "Folder name" : "File name";
    input.textColor = Color{40, 40, 40};
    input.borderColor = Color{170, 170, 170};
    input.focusedBorderColor = Color{80, 140, 230};
    input.leftPadding = 4.0f;
    input.onSubmit = [this](const std::string &text) {
      submitPendingCreate(text);
    };
    input.state = pendingInputState_;
    row.addChild(input);
    return row;
  }

  // Inline rename row: replaces an existing row's content (via the
  // rename-sentinel key) with a TextInput pre-filled with the item's
  // current name, at the same indent depth the row already had.
  View buildPendingRenameRow(const std::string &originalPath) {
    bool isDir = pendingRename_.isDir;
    int depth = explorerDepthForPath(originalPath);
    int d = 0;
    FileTreeNode *node = findExplorerNode(originalPath, d);
    bool expanded = node && node->expanded;

    View row;
    row.style.width = Size::full();
    row.style.alignItems = Align::Center;
    row.style.padding =
        EdgeInsets{3, 12, 3, static_cast<float>(8 + depth * 14)};
    row.style.backgroundColor = Color{243, 243, 243};
    row.disabled = [this]() -> bool {
      checkPendingRenameBlur();
      return false;
    };

    Text chevron;
    chevron.label = isDir ? (expanded ? std::string("\xE2\x96\xBE ")  // ▾
                                      : std::string("\xE2\x96\xB8 ")) // ▸
                          : std::string("   ");
    chevron.fontSize = 11;
    chevron.style.width = Size::pixel(14);
    chevron.color = Color{110, 110, 110};
    row.addChild(chevron);

    TextInput input;
    input.style.flexGrow = 1;
    input.style.height = Size::pixel(20);
    input.fontSize = 13;
    input.text =
        editorTitleFromPath(originalPath); // prefilled with current name
    input.textColor = Color{40, 40, 40};
    input.borderColor = Color{170, 170, 170};
    input.focusedBorderColor = Color{80, 140, 230};
    input.leftPadding = 4.0f;
    input.onSubmit = [this](const std::string &text) {
      submitPendingRename(text);
    };
    input.state = pendingRenameInputState_;
    row.addChild(input);
    return row;
  }

  // Row shown just above the file tree, next to the folder name: New File
  // (N), New Folder (F), Refresh (R), Collapse All (C). Text labels for
  // now instead of icons. Hidden entirely until a workspace folder is open
  // (matches openBtn's own visibility condition, just inverted).
  View buildExplorerHeaderRow() {
    View row;
    row.style.direction = FlexDirection::Row;
    row.style.alignItems = Align::Center;
    row.style.width = Size::full();
    row.style.padding = EdgeInsets{4, 8, 4, 12};
    row.style.gap = 2;
    // Highlighted the same way a selected child row is, whenever the
    // root itself is the current selection (its default state).
    row.style.backgroundColor = [this]() -> Color {
      return selectedPath_ == workspaceRoot_ ? Color{197, 220, 250}
                                             : Color{243, 243, 243};
    };
    row.style.hoverColor = Color{233, 233, 233};
    row.onClick = [this] { selectedPath_ = workspaceRoot_; };
    row.style.display = [this]() -> Display {
      return workspaceRoot_.empty() ? Display::None : Display::Flex;
    };

    Text nameLabel;
    nameLabel.label = std::function<std::string()>([this]() -> std::string {
      std::string name = editorTitleFromPath(workspaceRoot_);
      for (auto &c : name)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      return name;
    });
    nameLabel.fontSize = 11;
    nameLabel.fontWeight = FontWeight::SemiBold;
    nameLabel.color = Color{110, 110, 110};
    nameLabel.style.flexGrow = 1;
    row.addChild(nameLabel);

    auto makeActionBtn = [](const std::string &label,
                            const std::string &tooltip,
                            std::function<void()> onClick) {
      View btn;
      btn.style.width = Size::pixel(20);
      btn.style.height = Size::pixel(20);
      btn.style.justifyContent = Justify::Center;
      btn.style.alignItems = Align::Center;
      btn.style.borderRadius = 3.0f;
      btn.style.hoverColor = Color{222, 222, 222};
      btn.style.backgroundColor = Color{0, 0, 0, 0};
      btn.tooltip = tooltip;
      btn.onClick = std::move(onClick);
      Text t;
      t.label = label;
      t.fontSize = 11;
      t.fontWeight = FontWeight::SemiBold;
      t.color = Color{90, 90, 90};
      btn.addChild(t);
      return btn;
    };

    row.addChild(makeActionBtn("N", "New File", [this] { explorerNewFile(); }));
    row.addChild(
        makeActionBtn("F", "New Folder", [this] { explorerNewFolder(); }));
    row.addChild(makeActionBtn("R", "Refresh", [this] { explorerRefresh(); }));
    row.addChild(
        makeActionBtn("C", "Collapse All", [this] { explorerCollapseAll(); }));
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
    pane.addChild(buildExplorerHeaderRow());

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
      if (key == kPendingCreateKey)
        return buildPendingCreateRow();
      if (pendingRename_.active && key == std::string(kPendingRenameKeyPrefix) +
                                              pendingRename_.originalPath)
        return buildPendingRenameRow(pendingRename_.originalPath);
      return buildExplorerRow(key);
    };
    pane.addChild(std::move(list));
    return pane;
  }

  View buildSearchPane() {
    auto act = activityIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.width = Size::full();
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = Color{243, 243, 243};
    pane.style.display = [act]() -> Display {
      return *act == kSearchActivityId ? Display::Flex : Display::None;
    };

    View inputRow;
    inputRow.style.width = Size::full();
    inputRow.style.padding = EdgeInsets{8, 12, 4, 12};
    inputRow.style.flexShrink = 0;

    TextInput input;
    input.style.width = Size::full();
    input.style.height = Size::pixel(28);
    input.style.flexShrink = 0;
    input.fontSize = 13;
    input.placeholder = "Search";
    input.textColor = Color{40, 40, 40};
    input.borderColor = Color{190, 190, 190};
    input.focusedBorderColor = Color{80, 140, 230};
    input.leftPadding = 6.0f;
    input.state = searchInputState_;
    input.onChange = [this](const std::string &text) {
      runWorkspaceSearch(text);
    };
    inputRow.addChild(input);
    pane.addChild(std::move(inputRow));

    View replaceRow;
    replaceRow.style.direction = FlexDirection::Row;
    replaceRow.style.width = Size::full();
    replaceRow.style.padding = EdgeInsets{0, 12, 4, 12};
    replaceRow.style.flexShrink = 0; // same reasoning as inputRow: never
                                     // let the panel's shrink math eat this
    replaceRow.style.gap = 6;
    replaceRow.style.alignItems = Align::Center;

    TextInput replaceInput;
    replaceInput.style.flexGrow = 1;
    replaceInput.style.height = Size::pixel(28);
    replaceInput.style.flexShrink = 0;
    replaceInput.fontSize = 13;
    replaceInput.placeholder = "Replace";
    replaceInput.textColor = Color{40, 40, 40};
    replaceInput.borderColor = Color{190, 190, 190};
    replaceInput.focusedBorderColor = Color{80, 140, 230};
    replaceInput.leftPadding = 6.0f;
    replaceInput.state = replaceInputState_;
    replaceInput.onChange = [this](const std::string &text) {
      workspaceReplaceText_ = text;
    };
    replaceRow.addChild(std::move(replaceInput));

    View replaceAllBtn;
    replaceAllBtn.style.height = Size::pixel(28);
    replaceAllBtn.style.flexShrink = 0;
    replaceAllBtn.style.padding = EdgeInsets{0, 10, 0, 10};
    replaceAllBtn.style.justifyContent = Justify::Center;
    replaceAllBtn.style.alignItems = Align::Center;
    replaceAllBtn.style.borderRadius = 3.0f;
    replaceAllBtn.style.backgroundColor = Color{225, 225, 225};
    replaceAllBtn.style.hoverColor = Color{205, 205, 205};
    replaceAllBtn.tooltip = "Replace all current matches";
    replaceAllBtn.onClick = [this] { replaceAllMatches(); };
    Text replaceAllLabel;
    replaceAllLabel.label = std::string("Replace All");
    replaceAllLabel.fontSize = 12;
    replaceAllLabel.color = Color{50, 50, 50};
    replaceAllBtn.addChild(replaceAllLabel);
    replaceRow.addChild(std::move(replaceAllBtn));

    pane.addChild(std::move(replaceRow));

    Text hintOrCount;
    hintOrCount.label = std::function<std::string()>([this]() -> std::string {
      if (workspaceRoot_.empty())
        return "Open a folder to search its files.";
      if (workspaceSearchQuery_.empty())
        return "";
      return std::to_string(workspaceSearchResults_.size()) +
             (workspaceSearchResults_.size() >= kMaxSearchResults
                  ? "+ results"
                  : (workspaceSearchResults_.size() == 1 ? " result"
                                                         : " results"));
    });
    hintOrCount.style.padding = EdgeInsets{0, 12, 6, 12};
    hintOrCount.style.flexShrink = 0;
    hintOrCount.fontSize = 11;
    hintOrCount.color = Color{130, 130, 130};
    pane.addChild(hintOrCount);

    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.overflowY = Overflow::Auto;
    list.style.backgroundColor = Color{243, 243, 243};
    // Fixed pool, not a keyed list — see buildSearchResultSlot's comment
    // for why. Hidden slots (idx >= current result count) cost nothing:
    // measureNatural() skips Display::None children entirely, so an
    // empty or short result set doesn't pay for the unused rows.
    for (size_t i = 0; i < kMaxSearchResults; ++i)
      list.addChild(buildSearchResultSlot(i));
    pane.addChild(std::move(list));
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
    header.style.flexShrink = 0;
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

  // Spawns a new terminal tab: home directory if no workspace folder is
  // open, the workspace root otherwise (spawn()'s startDir already
  // treats an empty string as "use the home directory" — see
  // liteui_terminal.hpp — so passing workspaceRoot_ straight through
  // covers both cases without a branch here).
  void spawnNewTerminal() {
    TerminalTab tab;
    tab.state->spawn(80, 24, workspaceRoot_);
    terminals_.push_back(std::move(tab));
    *activeTerminalIndex_ = static_cast<int>(terminals_.size()) - 1;
  }

  std::vector<std::string> terminalKeys() const {
    std::vector<std::string> keys;
    keys.reserve(terminals_.size());
    for (size_t i = 0; i < terminals_.size(); ++i)
      keys.push_back(std::to_string(i));
    return keys;
  }
  static size_t terminalKeyToIndex(const std::string &key) {
    return static_cast<size_t>(std::stoul(key));
  }

  // One terminal's canvas, kept mounted and hidden via Display::None
  // when it isn't the active one — same pattern as the editor's own
  // per-document CodeEditor stack, so switching terminals never tears
  // down or rebuilds a live PTY.
  View buildTerminalOutput(size_t idx) {
    auto activeIdx = activeTerminalIndex_;
    liteui_terminal::Terminal shell;
    shell.state = terminals_[idx].state;
    shell.fontSize = 13.0f;
    shell.style.width = Size::full();
    shell.style.height = Size::full();
    shell.style.padding = EdgeInsets{4, 10, 6, 10};
    shell.style.display = [activeIdx, idx]() -> Display {
      return static_cast<size_t>(*activeIdx) == idx ? Display::Flex
                                                    : Display::None;
    };
    return liteui_terminal::toTerminalView(std::move(shell));
  }

  View buildTerminalOutputStack() {
    View stack;
    stack.style.width = Size::full();
    stack.style.flexGrow = 1;
    stack.keysSource = [this] { return terminalKeys(); };
    stack.itemBuilder = [this](const std::string &key) {
      return buildTerminalOutput(terminalKeyToIndex(key));
    };
    return stack;
  }

  // One row in the terminal list: label + a stub close "x". Clicking
  // the row (anywhere but the x) switches the active terminal.
  View buildTerminalRow(size_t idx) {
    auto activeIdx = activeTerminalIndex_;

    View row;
    row.style.direction = FlexDirection::Row;
    row.style.alignItems = Align::Center;
    row.style.width = Size::full();
    row.style.padding = EdgeInsets{6, 8, 6, 12};
    row.style.gap = 6;
    row.style.backgroundColor = [activeIdx, idx]() -> Color {
      return static_cast<size_t>(*activeIdx) == idx ? Color{55, 55, 55}
                                                    : Color{30, 30, 30};
    };
    row.style.hoverColor = Color{45, 45, 45};
    row.onClick = [activeIdx, idx] { *activeIdx = static_cast<int>(idx); };

    Text label;
    label.label = std::function<std::string()>([this, idx]() -> std::string {
      return idx < terminals_.size() ? terminals_[idx].label : std::string();
    });
    label.fontSize = 13;
    label.color = Color{210, 210, 210};
    label.style.flexGrow = 1;
    row.addChild(label);

    View closeBtn;
    closeBtn.style.width = Size::pixel(18);
    closeBtn.style.height = Size::pixel(18);
    closeBtn.style.justifyContent = Justify::Center;
    closeBtn.style.alignItems = Align::Center;
    closeBtn.style.borderRadius = 3.0f;
    closeBtn.style.hoverColor = Color{70, 70, 70};
    closeBtn.style.backgroundColor = Color{0, 0, 0, 0};
    // Stub only, as asked — visually present, not wired up. Closing a
    // running PTY safely (reassigning the active index, tearing down
    // the shell, deciding what "no terminals left" looks like) is real
    // behavior for a later pass, not a one-liner here.
    closeBtn.onClick = [] {};
    Text closeLabel;
    closeLabel.label = std::string("x");
    closeLabel.fontSize = 12;
    closeLabel.color = Color{150, 150, 150};
    closeBtn.addChild(closeLabel);
    row.addChild(closeBtn);
    return row;
  }

  // Right-hand column: a "+" header (New Terminal) above the keyed list
  // of terminal rows — the split VS Code itself uses.
  View buildTerminalListPanel() {
    View panel;
    panel.style.direction = FlexDirection::Column;
    panel.style.width = Size::pixel(180.0f);
    panel.style.flexShrink = 0;
    panel.style.height = Size::full();
    panel.style.backgroundColor = liteui_terminal::kDefaultBg;
    panel.style.borderWidth = 1.0f;
    panel.style.borderColor = Color{50, 50, 50};

    View header;
    header.style.direction = FlexDirection::Row;
    header.style.width = Size::full();
    header.style.height = Size::pixel(30);
    header.style.alignItems = Align::Center;
    header.style.justifyContent = Justify::End;
    header.style.padding = EdgeInsets{0, 8, 0, 8};
    header.style.flexShrink = 0;

    View addBtn;
    addBtn.style.width = Size::pixel(22);
    addBtn.style.height = Size::pixel(22);
    addBtn.style.justifyContent = Justify::Center;
    addBtn.style.alignItems = Align::Center;
    addBtn.style.borderRadius = 3.0f;
    addBtn.style.hoverColor = Color{60, 60, 60};
    addBtn.style.backgroundColor = Color{0, 0, 0, 0};
    addBtn.tooltip = "New Terminal";
    addBtn.onClick = [this] { spawnNewTerminal(); };
    Text plus;
    plus.label = std::string("+");
    plus.fontSize = 16;
    plus.color = Color{200, 200, 200};
    addBtn.addChild(plus);
    header.addChild(addBtn);
    panel.addChild(std::move(header));

    View list;
    list.style.direction = FlexDirection::Column;
    list.style.width = Size::full();
    list.style.flexGrow = 1;
    list.style.overflowY = Overflow::Auto;
    list.keysSource = [this] { return terminalKeys(); };
    list.itemBuilder = [this](const std::string &key) {
      return buildTerminalRow(terminalKeyToIndex(key));
    };
    panel.addChild(std::move(list));
    return panel;
  }

  View buildTerminalPanel() {
    auto heightPtr = terminalHeight_;

    View terminal;
    terminal.style.width = Size::full();
    terminal.style.flexGrow = 0;
    terminal.style.flexShrink = 0;
    terminal.style.direction = FlexDirection::Column;
    terminal.style.backgroundColor = liteui_terminal::kDefaultBg;
    terminal.style.overflowY = Overflow::Hidden;
    terminal.style.height = [heightPtr]() -> Size {
      return Size::pixel(*heightPtr);
    };

    Text title;
    title.label = std::string("TERMINAL");
    title.fontSize = 12;
    title.fontWeight = FontWeight::SemiBold;
    title.color = Color{180, 180, 180};
    title.style.padding = EdgeInsets{6, 10, 4, 10};
    title.style.flexShrink = 0;
    terminal.addChild(title);

    // Active terminal's output on the left, the terminal list (with its
    // own "+") on the right — the same row-split VS Code itself uses.
    View body;
    body.style.direction = FlexDirection::Row;
    body.style.width = Size::full();
    body.style.flexGrow = 1;
    body.addChild(buildTerminalOutputStack());
    body.addChild(buildTerminalListPanel());
    terminal.addChild(std::move(body));
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
      if (!doc || doc->isWelcome)
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
      if (!doc || doc->isWelcome)
        return "";
      if (!doc->highlighter || !doc->highlighter->lang)
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
    if (doc.isWelcome)
      return buildWelcomePane(idx);

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

  // VS-Code-style landing tab: shown in place of a blank untitled buffer
  // (see newWelcomeTab()). Lives in the same keyed `stack` container as
  // every CodeEditor, so it switches tabs the same way — a plain
  // Dynamic<Display> keyed off activeIndex_, not a rebuild.
  View buildWelcomePane(size_t idx) {
    auto activeIndexPtr = activeIndex_;

    View pane;
    pane.style.direction = FlexDirection::Column;
    pane.style.flexGrow = 1;
    pane.style.backgroundColor = Color{255, 255, 255};
    pane.style.padding = EdgeInsets::all(48.0f);
    pane.style.gap = 4;
    pane.style.display = [activeIndexPtr, idx]() -> Display {
      return *activeIndexPtr == static_cast<int>(idx) ? Display::Flex
                                                      : Display::None;
    };

    Text title;
    title.label = std::string("liteui code editor");
    title.fontSize = 28;
    title.fontWeight = FontWeight::SemiBold;
    title.color = Color{40, 40, 40};
    pane.addChild(title);

    Text subtitle;
    subtitle.label = std::string("A lightweight editor.");
    subtitle.fontSize = 13;
    subtitle.color = Color{120, 120, 120};
    subtitle.style.margin = EdgeInsets{4, 0, 24, 0};
    pane.addChild(subtitle);

    Text startHeader;
    startHeader.label = std::string("Start");
    startHeader.fontSize = 12;
    startHeader.fontWeight = FontWeight::SemiBold;
    startHeader.color = Color{110, 110, 110};
    startHeader.style.margin = EdgeInsets{0, 0, 4, 0};
    pane.addChild(startHeader);

    auto makeLink = [](const std::string &label,
                       std::function<void()> onClick) {
      View row;
      row.style.padding = EdgeInsets{4, 0, 4, 0};
      row.style.hoverColor = Color{240, 240, 240};
      row.onClick = std::move(onClick);
      Text t;
      t.label = label;
      t.fontSize = 13;
      t.color = Color{20, 90, 200};
      row.addChild(t);
      return row;
    };

    pane.addChild(makeLink("New File", [this] { newDocument(); }));
    pane.addChild(makeLink("Open File...", [this] {
      auto p = openFilePicker("Open File", {{"All Files", "*"}});
      if (p)
        openFile(*p);
    }));
    pane.addChild(makeLink("Open Folder...", [this] {
      openWorkspaceFolder();
      *activityIndex_ = kExplorerActivityId; // reveal the Explorer
    }));

    return pane;
  }

  // Delete-confirmation dialog, built the same way as a standalone
  // confirm dialog: a dimmed, absolute, full-window backdrop that
  // centers dialogBox via alignItems/justifyContent, closes on an
  // outside click, and swallows clicks that land on the dialog itself
  // so they don't bubble up to that same outside-click handler.
  View buildDeleteDialog() {
    auto openPtr = deleteDialogOpen_;

    View dialogBox;
    dialogBox.style.direction = FlexDirection::Column;
    dialogBox.style.width = Size::pixel(340);
    dialogBox.style.padding = EdgeInsets::all(20);
    dialogBox.style.gap = 16;
    dialogBox.style.backgroundColor = Color{255, 255, 255};
    dialogBox.style.borderRadius = 8.0f;
    dialogBox.onClick = [] {};
    Text title;
    title.label = std::function<std::string()>([this]() -> std::string {
      return deleteTargetIsDir_ ? "Delete Folder?" : "Delete File?";
    });
    title.fontSize = 18;
    title.fontWeight = FontWeight::SemiBold;
    dialogBox.addChild(title);

    Text message;
    message.label = std::function<std::string()>([this]() -> std::string {
      std::string name = deleteTargetPath_.empty()
                             ? std::string()
                             : editorTitleFromPath(deleteTargetPath_);
      if (deleteTargetIsDir_)
        return "\"" + name +
               "\" and everything inside it will be permanently deleted. "
               "This action cannot be undone.";
      return "\"" + name +
             "\" will be permanently deleted. This action cannot be undone.";
    });
    message.wrap = TextWrap::Wrap;
    message.style.width = Size::full();
    message.color = Color{90, 90, 90};
    dialogBox.addChild(message);

    View buttonRow;
    buttonRow.style.direction = FlexDirection::Row;
    buttonRow.style.justifyContent = Justify::End;
    buttonRow.style.gap = 10;

    Text cancelLabel;
    cancelLabel.label = std::string("Cancel");
    View cancelButton;
    cancelButton.style.width = Size::pixel(80);
    cancelButton.style.height = Size::pixel(34);
    cancelButton.style.backgroundColor = Color{240, 240, 240};
    cancelButton.style.hoverColor = Color{225, 225, 225};
    cancelButton.style.borderRadius = 4.0f;
    cancelButton.style.alignItems = Align::Center;
    cancelButton.style.justifyContent = Justify::Center;
    cancelButton.onClick = [this] { cancelDeleteConfirm(); };
    cancelButton.addChild(cancelLabel);

    Text confirmLabel;
    confirmLabel.label = std::string("Delete");
    confirmLabel.color = Color{255, 255, 255};
    View confirmButton;
    confirmButton.style.width = Size::pixel(80);
    confirmButton.style.height = Size::pixel(34);
    confirmButton.style.backgroundColor = Color{0xC0, 0x39, 0x2B};
    confirmButton.style.hoverColor = Color{0xA8, 0x2F, 0x23};
    confirmButton.style.borderRadius = 4.0f;
    confirmButton.style.alignItems = Align::Center;
    confirmButton.style.justifyContent = Justify::Center;
    confirmButton.onClick = [this] { confirmDelete(); };
    confirmButton.addChild(confirmLabel);

    buttonRow.addChild(cancelButton);
    buttonRow.addChild(confirmButton);
    dialogBox.addChild(buttonRow);

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 300; // above the menu dropdowns (200)
    backdrop.style.backgroundColor = Color{0, 0, 0, 90};
    backdrop.style.alignItems = Align::Center;
    backdrop.style.justifyContent = Justify::Center;
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [this] { cancelDeleteConfirm(); };
    backdrop.addChild(dialogBox);
    return backdrop;
  }

  // Right-click context menu, same backdrop+absolute-menu shape as the
  // top menu bar's dropdowns. Item visibility toggles per target (file vs
  // folder) via Dynamic<Display> reading explorerMenuTargetIsDir_, since
  // the menu itself is built once and never rebuilt.
  View buildExplorerContextMenu() {
    auto openPtr = explorerMenuOpen_;
    auto xPtr = explorerMenuX_;
    auto yPtr = explorerMenuY_;

    View backdrop;
    backdrop.style.position = Position::Absolute;
    backdrop.style.left = 0.0f;
    backdrop.style.top = 0.0f;
    backdrop.style.right = 0.0f;
    backdrop.style.bottom = 0.0f;
    backdrop.style.zIndex = 250; // above menu-bar dropdowns (200), below
                                 // the delete-confirmation dialog (300)
    backdrop.style.backgroundColor = Color{255, 255, 255, 2};
    backdrop.style.display = [openPtr]() -> Display {
      return *openPtr ? Display::Flex : Display::None;
    };
    backdrop.onClick = [openPtr] { *openPtr = false; };

    View menu;
    menu.style.position = Position::Absolute;
    menu.style.direction = FlexDirection::Column;
    menu.style.width = Size::pixel(170);
    menu.style.backgroundColor = Color{255, 255, 255};
    menu.style.borderWidth = 1.0f;
    menu.style.borderColor = Color{200, 200, 200};
    menu.style.borderRadius = 4.0f;
    menu.style.zIndex = 260;
    menu.style.left = [xPtr]() { return *xPtr; };
    menu.style.top = [yPtr]() { return *yPtr; };

    auto makeItem = [this](const std::string &label,
                           std::function<void()> action,
                           std::function<Display()> display = nullptr) {
      Text t;
      t.label = label;
      t.style.padding = EdgeInsets::all(8);
      t.fontSize = 13;
      t.color = Color{40, 40, 40};

      View row;
      row.style.hoverColor = Color{240, 240, 240};
      if (display)
        row.style.display = std::move(display);
      auto openPtr = explorerMenuOpen_;
      row.onClick = [openPtr, action] {
        *openPtr = false;
        if (action)
          action();
      };
      row.addChild(t);
      return row;
    };

    auto fileOnly = [this]() -> Display {
      return explorerMenuTargetIsDir_ ? Display::None : Display::Flex;
    };
    auto dirOnly = [this]() -> Display {
      return explorerMenuTargetIsDir_ ? Display::Flex : Display::None;
    };

    // Order matches the request for both cases: a file sees
    // Open/Rename/Delete; a folder sees New Folder/New File/Rename/Delete
    // (Open simply hides itself for a folder, and New File/New Folder
    // hide themselves for a file).
    menu.addChild(
        makeItem("Open", [this] { explorerContextOpen(); }, fileOnly));
    menu.addChild(makeItem(
        "New Folder", [this] { explorerContextNewFolder(); }, dirOnly));
    menu.addChild(
        makeItem("New File", [this] { explorerContextNewFile(); }, dirOnly));
    menu.addChild(makeItem("Rename", [this] { explorerContextRename(); }));
    menu.addChild(makeItem("Delete", [this] { explorerContextDelete(); }));

    backdrop.addChild(menu);
    return backdrop;
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
    // Defensive: keep the tab strip un-shrinkable, the same way
    // buildHDivider()/buildTerminalPanel() already protect themselves.
    // Without this, any sibling whose Fit-sizing balloons (as
    // buildWelcomePane's did before the fix above) can eat into the
    // tab bar's height via this file's flex-shrink pool.
    tabBar.style.flexShrink = 0;

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
    root.addChild(buildDeleteDialog());
    root.addChild(buildExplorerContextMenu());
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

    // Global shortcuts fire before a focused view's own onKeyDown (see
    // LiteUI::dispatchKeyDown), so this guards against stealing Delete
    // away from an in-progress rename (the pending-create TextInput) or
    // from a CodeEditor that's actually focused and mid-edit — in both
    // those cases Delete should do its normal "delete the next
    // character" thing instead of popping this dialog.
    ui_.addShortcut(KeyModifiers{}, Key::Delete, [this] {
      if (pendingCreate_.active || pendingRename_.active)
        return;
      const EditorDocument *active = activeDocument();
      if (active && !active->isWelcome && active->state &&
          active->state->focused)
        return;

      beginDeleteConfirm();
    });
  }

  bool commandsInstalled_ = false;
};