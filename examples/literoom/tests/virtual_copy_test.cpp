// Build: pure header test, no liteui, no threads (see CMakeLists.txt).
#include "io/virtual_copy.hpp"
#include "check.hpp"

#include <chrono>
#include <fstream>

// A scratch folder that removes itself.
struct TempDir
{
  std::filesystem::path dir;
  TempDir()
  {
    static int n = 0;
    dir = std::filesystem::temp_directory_path() /
          ("literoom_vc_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "_" + std::to_string(n++));
    std::filesystem::create_directories(dir);
  }
  ~TempDir()
  {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  void touch(const std::string &name) const
  {
    std::ofstream f(dir / fsPath(name), std::ios::binary);
    f << "x";
  }
  std::string path() const { return u8ToString(dir); }
};

static void testPaths()
{
  CHECK(vcPath("a/photo.jpg", 0) == "a/photo.jpg");
  CHECK(vcPath("a/photo.jpg", -3) == "a/photo.jpg");
  CHECK(vcPath("a/photo.jpg", 2) == "a/photo.jpg|vc2");

  for (int n = 1; n <= kMaxVirtualCopies; ++n)
  { // every copy number survives a round trip
    const std::string p = vcPath("C:\\pics\\IMG 1.png", n);
    std::string img;
    int got = 0;
    CHECK(vcSplit(p, &img, &got) && img == "C:\\pics\\IMG 1.png" && got == n);
    CHECK(vcImageOf(p) == "C:\\pics\\IMG 1.png" && vcCopyOf(p) == n);
  }
  CHECK(vcSplit("a|vc3", nullptr, nullptr)); // both outputs are optional

  // Things that are not a copy's path: the path itself comes back, copy 0.
  for (const char *s : {"a/photo.jpg", "", "|vc1", "a/|vc1", "a\\|vc1", "x|vc0",
                        "x|vc10", "x|vcA", "x|vc", "x|vc1y", "x|VC1", "x|v1"})
  {
    CHECK(!vcSplit(s, nullptr, nullptr));
    CHECK(vcImageOf(s) == s && vcCopyOf(s) == 0);
  }
  // The key the filmstrip uses (the last part of the path) differs per copy.
  CHECK(baseName(vcPath("d/photo.jpg", 2)) == "photo.jpg|vc2");
  CHECK(baseName(vcPath("d/photo.jpg", 2)) != baseName("d/photo.jpg"));
}

static void testSidecarNames()
{
  CHECK(vcSidecarFileOf("a/photo.jpg") == "a/photo.jpg.literoom.json");
  CHECK(vcSidecarFileOf("a/photo.jpg|vc2") == "a/photo.jpg.vc2.literoom.json");
  CHECK(vcSidecarFileOf(vcPath("a/photo.jpg", 9)) == "a/photo.jpg.vc9.literoom.json");

  std::string img;
  int n = 0;
  CHECK(vcParseSidecarName("photo.jpg.vc2.literoom.json", &img, &n) && img == "photo.jpg" && n == 2);
  CHECK(vcParseSidecarName("PHOTO.JPG.vc9.LITEROOM.JSON", &img, &n) && img == "PHOTO.JPG" && n == 9);
  CHECK(vcParseSidecarName("x.vc1.literoom.json", &img, &n) && img == "x" && n == 1);
  CHECK(vcParseSidecarName("a.vc1.b.vc3.literoom.json", &img, &n) && img == "a.vc1.b" && n == 3);
  // The file name built for a copy parses back to its picture and number.
  CHECK(vcParseSidecarName(vcSidecarFileOf("photo.jpg|vc4"), &img, &n) && img == "photo.jpg" && n == 4);
  for (const char *s : {"photo.jpg.literoom.json", "photo.jpg.vc0.literoom.json",
                        ".vc1.literoom.json", "photo.jpg.vc2.json",
                        "photo.jpg.vc10.literoom.json", "photo.jpg.vcX.literoom.json",
                        "photo.jpg.VC2.literoom.json", "photo.jpg.vc2", "", ".literoom.json"})
    CHECK(!vcParseSidecarName(s, nullptr, nullptr));
}

static void testFreeId()
{
  CHECK(vcFreeId({}) == 1);
  CHECK(vcFreeId({1, 2}) == 3);
  CHECK(vcFreeId({2}) == 1);
  CHECK(vcFreeId({3, 1}) == 2);
  std::vector<int> all;
  for (int n = 1; n <= kMaxVirtualCopies; ++n)
    all.push_back(n);
  CHECK(vcFreeId(all) == 0);
  all.erase(all.begin() + 4); // copy 5 was deleted: it is the next one
  CHECK(vcFreeId(all) == 5);
}

static void testListFolder()
{
  TempDir td;
  for (const char *f : {"photo.jpg", "photo.jpg.literoom.json",
                        "photo.jpg.vc2.literoom.json", "photo.jpg.vc1.literoom.json",
                        "Other.PNG.vc3.literoom.json", "junk.vc12.literoom.json",
                        "notes.txt"})
    td.touch(f);
  std::filesystem::create_directories(td.dir / "dir.vc4.literoom.json"); // not a file
  const auto m = listVirtualCopies(td.path(), 100);
  CHECK(m.size() == 2);
  const auto a = m.find("photo.jpg"), b = m.find("other.png"); // keyed lower-case
  CHECK(a != m.end() && a->second == (std::vector<int>{1, 2})); // ascending
  CHECK(b != m.end() && b->second == (std::vector<int>{3}));
  CHECK(listVirtualCopies(td.path() + "/missing", 100).empty());
  CHECK(listVirtualCopies(td.path(), 0).empty()); // the cap counts folder entries
}

int main()
{
  testPaths();
  testSidecarNames();
  testFreeId();
  testListFolder();
  return finish();
}
