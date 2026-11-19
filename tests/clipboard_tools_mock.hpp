#pragma once
// Fake clipboard tools for the Linux tool backends, used by
// cases/clipboard_tools.inc and cases/clipboard_wayland.inc (persist-on-exit).
//
// kFakeWlCopy / kFakeWlPaste / kFakeXclip / kFakeXsel are small sh scripts
// that keep the "clipboard" in two files, <store>.img (a PNG) and <store>.txt
// (text); setting one removes the other, like a real single-owner clipboard.
// ToolBox is a private directory used as the WHOLE PATH of a child process:
// the fake tools asked for, plus cat and rm for them to use. A real xclip or
// wl-copy on the machine running the tests can therefore never be picked up
// by accident.
//
// Included from the case files after liteui.hpp and clipboard_helpers.hpp.
// Linux only.

#if !defined(_WIN32)

#include <filesystem>

namespace clipboard_test {


inline const char *const kFakeWlCopy = R"(#!/bin/sh
S="$LITEUI_FAKE_STORE"
if [ "$1" = "--type" ]; then cat > "$S.img"; rm -f "$S.txt"
else cat > "$S.txt"; rm -f "$S.img"; fi
)";

inline const char *const kFakeWlPaste = R"(#!/bin/sh
S="$LITEUI_FAKE_STORE"
case "$*" in
  *--list-types*)
    if [ -s "$S.img" ]; then echo image/png
    elif [ -f "$S.txt" ]; then echo text/plain
    else exit 1; fi ;;
  *"--type image/png"*) if [ -s "$S.img" ]; then cat "$S.img"; else exit 1; fi ;;
  *"--type text"*)      if [ -f "$S.txt" ]; then cat "$S.txt"; else exit 1; fi ;;
  *) exit 1 ;;
esac
)";

inline const char *const kFakeXclip = R"(#!/bin/sh
S="$LITEUI_FAKE_STORE"
case "$*" in
  *"TARGETS -o"*)
    if [ -s "$S.img" ]; then echo TARGETS; echo image/png
    elif [ -f "$S.txt" ]; then echo TARGETS; echo UTF8_STRING
    else exit 1; fi ;;
  *"-t image/png -o"*)
    if [ -s "$S.img" ]; then cat "$S.img"; else echo "Error: target image/png not available" >&2; exit 1; fi ;;
  *"-t image/png"*) cat > "$S.img"; rm -f "$S.txt" ;;
  *"-selection clipboard -o"*) if [ -f "$S.txt" ]; then cat "$S.txt"; else exit 1; fi ;;
  *"-selection clipboard"*) cat > "$S.txt"; rm -f "$S.img" ;;
  *) exit 1 ;;
esac
)";

// xsel is text-only.
inline const char *const kFakeXsel = R"(#!/bin/sh
S="$LITEUI_FAKE_STORE"
case "$*" in
  *--input*)  cat > "$S.txt" ;;
  *--output*) if [ -f "$S.txt" ]; then cat "$S.txt"; else exit 1; fi ;;
  *) exit 1 ;;
esac
)";

inline const char *const kFailingCopy = "#!/bin/sh\ncat > /dev/null\nexit 1\n";
inline const char *const kDeadCopy = "#!/bin/sh\nexit 0\n"; // never reads its stdin

// A private directory used as the whole PATH: the fake tools asked for, plus
// cat and rm for them to use. A real xclip or wl-copy on the machine running
// the tests therefore can never be picked up by accident.
class ToolBox {
public:
  ToolBox() {
    for (const char *name : {"cat", "rm"})
      for (const char *prefix : {"/bin/", "/usr/bin/"}) {
        const std::string real = std::string(prefix) + name;
        if (std::filesystem::exists(real)) {
          std::filesystem::create_symlink(real, dir_.file(name));
          break;
        }
      }
  }
  void install(const std::string &name, const char *script) {
    const std::string path = dir_.file(name);
    writeFile(path, std::string(script));
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec,
                                 std::filesystem::perm_options::replace);
  }
  std::string store() const { return dir_.file("clip"); }
  // wayland = true: WAYLAND_DISPLAY set (wl-clipboard is considered);
  // false: DISPLAY set (xclip / xsel).
  EnvMap env(bool wayland) const {
    EnvMap e;
    e["PATH"] = dir_.path();
    e["LITEUI_FAKE_STORE"] = store();
    e["WAYLAND_DISPLAY"] = wayland ? std::optional<std::string>("wl-test") : std::nullopt;
    e["DISPLAY"] = wayland ? std::nullopt : std::optional<std::string>(":99");
    return e;
  }

private:
  TempDir dir_;
};

} // namespace clipboard_test

#endif // !_WIN32
