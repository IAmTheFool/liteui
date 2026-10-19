// file_utils_test.cpp - paths, extensions, natural order, prefix reads, atomic
// writes and folder listing. No UI, no liteui; the folder tests use a
// throw-away directory under the system temp folder.
#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

#include "check.hpp"
#include "file_utils.hpp"

namespace fs = std::filesystem;

// A fresh empty directory, removed again at the end of the scope.
struct TempDir
{
  fs::path path;
  explicit TempDir(const char *tag)
  {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    path = fs::temp_directory_path() /
           (std::string("literoom_fu_") + tag + "_" + std::to_string(stamp));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string str() const { return u8ToString(path); }
  std::string file(const std::string &name) const { return u8ToString(path / fsPath(name)); }
};

static void touch(const std::string &path, const std::string &content = "x")
{
  std::ofstream f(fsPath(path), std::ios::binary | std::ios::trunc);
  f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

static std::string slurp(const std::string &path)
{
  std::ifstream f(fsPath(path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static bool exists(const std::string &path)
{
  std::error_code ec;
  return fs::exists(fsPath(path), ec);
}

static std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// ---------------------------------------------------------------- paths
static void testBaseName()
{
  CHECK(baseName("a/b/c.jpg") == "c.jpg");
  CHECK(baseName("C:\\x\\y.png") == "y.png");
  CHECK(baseName("a\\b/c") == "c");
  CHECK(baseName("plain") == "plain");
  CHECK(baseName("dir/") == "");
  CHECK(baseName("") == "");
}

static void testUtf8RoundTrip()
{
  const std::string plain = "plain.jpg";
  const std::string utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC.jpg"; // "café 日本.jpg"
  CHECK(u8ToString(fsPath(plain)) == plain);
  CHECK(u8ToString(fsPath(utf8)) == utf8);
  CHECK(u8ToString(fsPath("a/dir/" + utf8).filename()) == utf8);
  CHECK(u8ToString(fsPath("")) == "");
}

// ----------------------------------------------------------- extensions
static void testExtensions()
{
  struct Row
  {
    const char *path, *ext;
    bool jpeg;
  };
  const Row rows[] = {
      {"a/b.c/IMG.JPG", "jpg", true},
      {"x.jpeg", "jpeg", true},
      {"a.JPEG", "jpeg", true},
      {"C:\\p\\q.PnG", "png", false},
      {"a.b.c.TIFF", "tiff", false},
      {".hidden", "hidden", false}, // a leading dot counts as the extension dot
      {".jpg", "jpg", true},
      {"noext", "", false},
      {"dir.d/file", "", false}, // the dot is in a folder name
      {"dir.d\\file", "", false},
      {"trailing.", "", false},
      {"", "", false},
  };
  for (const Row &r : rows)
  {
    CHECK(fileExtLower(r.path) == r.ext);
    CHECK(isJpegPath(r.path) == r.jpeg);
  }
  CHECK(filmLower("IMG_01.JpG") == "img_01.jpg");
  CHECK(filmLower("") == "");
}

// -------------------------------------------------------- natural order
static void testNaturalLess()
{
  CHECK(naturalLess("IMG_2", "IMG_10"));
  CHECK(!naturalLess("IMG_10", "IMG_2"));
  CHECK(naturalLess("a9b", "a10b"));
  CHECK(naturalLess("Photo2", "photo10"));
  CHECK(naturalLess("photo", "photo10")); // a prefix comes first
  CHECK(naturalLess("", "a"));
  CHECK(!naturalLess("a", ""));
  CHECK(naturalLess("a", "ab"));
  CHECK(naturalLess("1a", "a1")); // digits sort before letters
  CHECK(naturalLess("a.jpeg", "IMG_1.png")); // letters compare without case

  // Equal under natural order: case and leading zeros do not matter.
  const char *same[][2] = {{"img_2.jpg", "IMG_2.JPG"}, {"IMG_002", "IMG_2"}, {"007", "7"}, {"08", "8"}};
  for (auto &p : same)
  {
    CHECK(!naturalLess(p[0], p[1]));
    CHECK(!naturalLess(p[1], p[0]));
  }

  // Irreflexive and asymmetric over a mixed set.
  const std::vector<std::string> names = {"IMG_10.jpg", "img_2.jpg", "IMG_1.jpg", "IMG_002.jpg", "a10b", "a9b",
                                          "A9c", "photo", "Photo2", "photo10", "007", "7", "08", "8", ""};
  for (const auto &a : names)
  {
    CHECK(!naturalLess(a, a));
    for (const auto &b : names)
      CHECK(!(naturalLess(a, b) && naturalLess(b, a)));
  }
}

// The comparator listImageFiles uses must give ONE order whatever order the
// directory returned the names in (ties broken by plain comparison).
static void testTieBreakIsDeterministic()
{
  std::vector<std::string> v = {"IMG_2.jpg", "a.png", "b.jpg", "B.jpg", "img_1.jpg", "img_01.jpg"};
  const std::vector<std::string> want = {"a.png", "B.jpg", "b.jpg", "img_01.jpg", "img_1.jpg", "IMG_2.jpg"};
  std::sort(v.begin(), v.end());
  int perms = 0;
  do
  {
    std::vector<std::string> w = v;
    std::sort(w.begin(), w.end(), [](const std::string &a, const std::string &b)
              { return naturalLess(a, b) || (!naturalLess(b, a) && a < b); });
    CHECK(w == want);
    ++perms;
  } while (std::next_permutation(v.begin(), v.end()));
  CHECK(perms == 720);
}

// --------------------------------------------------------- file reading
static void testReadFilePrefix()
{
  TempDir d("prefix");
  std::string hundred;
  for (int i = 0; i < 100; ++i)
    hundred.push_back(static_cast<char>(i));
  touch(d.file("f.bin"), hundred);
  touch(d.file("empty.bin"), "");

  std::vector<uint8_t> out = bytes("junk that must not survive");
  CHECK(readFilePrefix(d.file("f.bin"), 10, &out));
  CHECK(out.size() == 10 && out[0] == 0 && out[9] == 9);

  CHECK(readFilePrefix(d.file("f.bin"), 1000, &out)); // smaller than the window: all of it
  CHECK(out.size() == 100 && out[99] == 99);
  CHECK(readFilePrefix(d.file("f.bin"), 100, &out) && out.size() == 100);

  CHECK(!readFilePrefix(d.file("empty.bin"), 10, &out)); // nothing read counts as failure
  CHECK(out.empty());
  CHECK(!readFilePrefix(d.file("f.bin"), 0, &out));
  CHECK(!readFilePrefix(d.file("missing.bin"), 10, &out));
  CHECK(!readFilePrefix(d.str(), 10, &out)); // a folder is not a file
}

// -------------------------------------------------------- atomic writes
static void testWriteFileAtomic()
{
  TempDir d("atomic");
  std::string err = "stale";

  // New file.
  const std::string a = d.file("a.jpg");
  CHECK(writeFileAtomic(a, bytes("hello"), &err));
  CHECK(slurp(a) == "hello");
  CHECK(!exists(a + ".tmp"));

  // Replaces an existing one completely (also with something shorter).
  CHECK(writeFileAtomic(a, bytes("hi"), &err));
  CHECK(slurp(a) == "hi");
  CHECK(!exists(a + ".tmp"));

  // Empty content is a valid file.
  CHECK(writeFileAtomic(a, {}, &err));
  CHECK(exists(a) && slurp(a).empty());

  // A leftover temp file from a crashed earlier export is simply overwritten.
  touch(a + ".tmp", "garbage from a crash");
  CHECK(writeFileAtomic(a, bytes("fresh"), &err));
  CHECK(slurp(a) == "fresh");
  CHECK(!exists(a + ".tmp"));

  // Binary content survives untouched (no newline translation).
  std::string bin;
  for (int i = 0; i < 256; ++i)
    bin.push_back(static_cast<char>(i));
  bin += "\r\n\r\n";
  CHECK(writeFileAtomic(a, bytes(bin), &err));
  CHECK(slurp(a) == bin);

  // Unwritable target (folder does not exist): false, the message names the
  // file, nothing is left behind, and a null errorOut is fine.
  const std::string bad = d.file("no_such_dir/b.jpg");
  err.clear();
  CHECK(!writeFileAtomic(bad, bytes("x"), &err));
  CHECK(err == "couldn't write b.jpg");
  CHECK(!exists(bad) && !exists(bad + ".tmp"));
  CHECK(!writeFileAtomic(bad, bytes("x"), nullptr));

  // Rename onto a directory fails: false, message set, temp file cleaned up,
  // the directory is still there.
  const std::string dirTarget = d.file("adir");
  fs::create_directory(fsPath(dirTarget));
  touch(u8ToString(fsPath(dirTarget) / "keep.txt")); // non-empty, so the rename cannot replace it
  err.clear();
  CHECK(!writeFileAtomic(dirTarget, bytes("x"), &err));
  CHECK(!err.empty());
  CHECK(!exists(dirTarget + ".tmp"));
  CHECK(fs::is_directory(fsPath(dirTarget)));
}

// ------------------------------------------------------- folder listing
static void testListImageFiles()
{
  TempDir d("list");
  const std::string utf8Name = "\xC3\xA9t\xC3\xA9.png"; // "été.png"
  for (const char *n : {"IMG_10.jpg", "img_2.JPG", "IMG_1.png", "a.jpeg", "notes.txt", "noext",
                        "photo.jpg.bak", "archive.png.zip"})
    touch(d.file(n));
  touch(d.file(utf8Name));
  fs::create_directory(fsPath(d.file("dir.jpg"))); // a folder named like a picture

  const std::vector<std::string> want = {"a.jpeg", "IMG_1.png", "img_2.JPG", "IMG_10.jpg", utf8Name};
  CHECK(listImageFiles(d.str(), 5000) == want);
  CHECK(listImageFiles(d.str(), 5) == want);

  // The cap is applied BEFORE sorting: only `cap` names come back, they are a
  // subset of the picture files, and they are sorted - but which subset is
  // up to the directory order (see the note on listImageFiles).
  for (size_t cap : {size_t{1}, size_t{3}, size_t{4}})
  {
    const auto got = listImageFiles(d.str(), cap);
    CHECK(got.size() == cap);
    for (const auto &n : got)
      CHECK(std::find(want.begin(), want.end(), n) != want.end());
    CHECK(std::is_sorted(got.begin(), got.end(), [](const std::string &a, const std::string &b)
                         { return naturalLess(a, b) || (!naturalLess(b, a) && a < b); }));
  }
  CHECK(listImageFiles(d.str(), 0).empty());

  // Nothing to list.
  CHECK(listImageFiles(d.file("no_such_folder"), 5000).empty());
  CHECK(listImageFiles(d.file("notes.txt"), 5000).empty()); // a file, not a folder
  TempDir empty("list_empty");
  CHECK(listImageFiles(empty.str(), 5000).empty());

  // Names that differ only in case sort by plain comparison ("B" before "b"),
  // so the strip never depends on the directory's own order. Skipped when the
  // file system folds case (Windows, default macOS): both files can't exist.
  TempDir t("list_tie");
  touch(t.file("b.jpg"));
  touch(t.file("B.jpg"));
  size_t entries = 0;
  for (auto it = fs::directory_iterator(t.path); it != fs::directory_iterator(); ++it)
    ++entries;
  if (entries == 2)
    CHECK((listImageFiles(t.str(), 5000) == std::vector<std::string>{"B.jpg", "b.jpg"}));
}

int main()
{
  testBaseName();
  testUtf8RoundTrip();
  testExtensions();
  testNaturalLess();
  testTieBreakIsDeterministic();
  testReadFilePrefix();
  testWriteFileAtomic();
  testListImageFiles();
  return finish();
}
