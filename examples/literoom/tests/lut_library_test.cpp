// Build: g++ -std=c++20 -pthread -I../src -I. lut_library_test.cpp -o lut_library_test && ./lut_library_test
#include "io/lut_library.hpp"
#include "check.hpp"

#include <charconv>
#include <chrono>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

struct TempDir
{
  fs::path dir;
  TempDir()
  {
    static int n = 0;
    dir = fs::temp_directory_path() /
          ("literoom_lut_test_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
           std::to_string(n++));
    fs::create_directories(dir);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
  std::string path(const std::string &name) const { return u8ToString(dir / fsPath(name)); }
  std::string root() const { return u8ToString(dir); }
};

static std::string cubeText(int n, float gain = 1.0f)
{
  std::string s = "LUT_3D_SIZE " + std::to_string(n) + "\n";
  char buf[40];
  const auto num = [&](double v)
  { // locale-independent, always a '.'
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 6);
    s.append(buf, r.ptr);
  };
  for (int b = 0; b < n; ++b)
    for (int g = 0; g < n; ++g)
      for (int r = 0; r < n; ++r)
      {
        num(gain * r / double(n - 1));
        s += ' ';
        num(gain * g / double(n - 1));
        s += ' ';
        num(gain * b / double(n - 1));
        s += '\n';
      }
  return s;
}

static void write(const std::string &path, const std::string &text)
{
  fs::create_directories(fsPath(path).parent_path());
  std::ofstream f(fsPath(path), std::ios::binary);
  f << text;
}

static void testHash()
{
  const uint8_t a[] = {1, 2, 3}, b[] = {1, 2, 4};
  CHECK(lutHashBytes(a, 3) == lutHashBytes(a, 3));
  CHECK(lutHashBytes(a, 3) != lutHashBytes(b, 3));
  CHECK(lutHashBytes(a, 3) != 0 && lutHashBytes(nullptr, 0) != 0);
  CHECK(lutHashBytes(a, 2) != lutHashBytes(a, 3));
}

static void testNamesAndPaths()
{
  CHECK(lutNameOfFile("/x/y/Warm Film.cube") == "Warm Film");
  CHECK(lutNameOfFile("C:\\x\\Teal.CUBE") == "Teal");
  CHECK(lutNameOfFile("noext") == "noext");
  CHECK(lutPath("d", "Warm") == "d/Warm.cube");
  // a hostile name cannot leave the folder
  const std::string p = lutPath("d", "../../etc/x");
  CHECK(p.rfind("d/", 0) == 0 && p.find('/', 2) == std::string::npos && p.find('\\') == std::string::npos);
  CHECK(!lutDir().empty());
}

static void testImport()
{
  TempDir in, lib;
  const std::string dir = lib.path("nested/luts"); // does not exist yet
  CHECK(listLutFiles(dir).empty());

  const std::string text = cubeText(5);
  write(in.path("My Look.cube"), text);
  LutEntry e;
  std::string warn, err;
  CHECK(importLutFile(in.path("My Look.cube"), dir, &e, &warn, &err));
  CHECK(e.name == "My Look" && e.size == 5 && e.hash != 0 && e.path == lutPath(dir, "My Look") && fs::exists(fsPath(e.path)));
  CHECK(warn.empty());
  CHECK(!fs::exists(fsPath(e.path + ".tmp")));
  // the bytes are copied as they are
  std::vector<uint8_t> back;
  CHECK(readLutBytes(e.path, &back, &err) && std::string(back.begin(), back.end()) == text);
  CHECK(e.hash == lutHashBytes(back.data(), back.size()));

  // the same file again: reused, no copy
  LutEntry again;
  CHECK(importLutFile(in.path("My Look.cube"), dir, &again, nullptr, &err));
  CHECK(again.name == "My Look" && again.hash == e.hash && listLutFiles(dir).size() == 1);

  // another file with the same name: -2, then -3
  write(in.path("other/My Look.cube"), cubeText(5, 0.9f));
  LutEntry second;
  CHECK(importLutFile(in.path("other/My Look.cube"), dir, &second, nullptr, &err));
  CHECK(second.name == "My Look-2" && second.hash != e.hash && listLutFiles(dir).size() == 2);
  write(in.path("third/My Look.cube"), cubeText(5, 0.8f));
  LutEntry third;
  CHECK(importLutFile(in.path("third/My Look.cube"), dir, &third, nullptr, &err) && third.name == "My Look-3");
  // importing the -2 file again finds it by content, not by name
  LutEntry second2;
  CHECK(importLutFile(in.path("other/My Look.cube"), dir, &second2, nullptr, &err) && second2.name == "My Look-2" &&
        listLutFiles(dir).size() == 3);

  // names are cleaned; a log-ish name warns but is still imported
  write(in.path("caf\xC3\xA9: SLog3?.cube"), cubeText(3));
  LutEntry odd;
  CHECK(importLutFile(in.path("caf\xC3\xA9: SLog3?.cube"), dir, &odd, &warn, &err));
  CHECK(odd.name == "caf_ SLog3_" && fs::exists(fsPath(odd.path)) && warn.find("camera-log") != std::string::npos);
  // a long name is capped, and its -2 stays within the cap
  const std::string longName(80, 'q');
  write(in.path(longName + ".cube"), cubeText(2));
  write(in.path("x/" + longName + ".cube"), cubeText(3));
  LutEntry l1, l2;
  CHECK(importLutFile(in.path(longName + ".cube"), dir, &l1, nullptr, &err) && l1.name.size() == kLutNameMax);
  CHECK(importLutFile(in.path("x/" + longName + ".cube"), dir, &l2, nullptr, &err) && l2.name.size() <= kLutNameMax &&
        l2.name.size() >= 2 && l2.name.substr(l2.name.size() - 2) == "-2" && l1.name != l2.name);

  // natural order, other files ignored
  write(dir + "/readme.txt", "hi");
  fs::create_directories(fsPath(dir + "/sub.cube"));
  write(in.path("A 2.cube"), cubeText(2));
  write(in.path("A 10.cube"), cubeText(3));
  LutEntry t;
  CHECK(importLutFile(in.path("A 2.cube"), dir, &t, nullptr, &err) && importLutFile(in.path("A 10.cube"), dir, &t, nullptr, &err));
  const auto names = listLutFiles(dir);
  size_t i2 = 99, i10 = 99;
  for (size_t i = 0; i < names.size(); ++i)
  {
    if (names[i] == "A 2.cube")
      i2 = i;
    if (names[i] == "A 10.cube")
      i10 = i;
  }
  CHECK(i2 < i10 && i10 < names.size());
  for (const auto &n : names)
    CHECK(n != "readme.txt" && n != "sub.cube");

  CHECK(deleteLutFile(e.path) && !fs::exists(fsPath(e.path)) && !deleteLutFile(e.path));
}

static void testListEntries()
{
  TempDir lib;
  const std::string dir = lib.root();
  CHECK(listLutEntries(dir).empty() && listLutEntries(lib.path("nope")).empty());
  write(dir + "/B look.cube", cubeText(2));
  write(dir + "/A look 10.cube", cubeText(2));
  write(dir + "/A look 2.cube", cubeText(2));
  write(dir + "/notes.txt", "x");
  fs::create_directories(fsPath(dir + "/folder.cube"));
  // Names an edit could not point at again (a character a LUT name can't hold) are left out.
  write(dir + "/caf\xC3\xA9.cube", cubeText(2));
  write(dir + "/trailing dot..cube", cubeText(2));
  const auto es = listLutEntries(dir);
  CHECK(es.size() == 3);
  if (es.size() == 3)
  {
    CHECK(es[0].name == "A look 2" && es[1].name == "A look 10" && es[2].name == "B look"); // natural order
    CHECK(es[0].path == dir + "/A look 2.cube" && es[0].hash == 0 && es[0].size == 0);
    for (const auto &e : es) // every listed LUT is found again through its name
      CHECK(lutPath(dir, e.name) == e.path);
  }
}

static void testImportRefusals()
{
  TempDir in, lib;
  const std::string dir = lib.path("luts");
  LutEntry e;
  std::string err;
  write(in.path("not.txt"), cubeText(2));
  CHECK(!importLutFile(in.path("not.txt"), dir, &e, nullptr, &err) && err.find(".cube") != std::string::npos);
  CHECK(!importLutFile(in.path("missing.cube"), dir, &e, nullptr, &err) && !err.empty());
  write(in.path("empty.cube"), "");
  CHECK(!importLutFile(in.path("empty.cube"), dir, &e, nullptr, &err) && err.find("empty") != std::string::npos);
  write(in.path("bad.cube"), "LUT_3D_SIZE 2\n0 0 0\n");
  CHECK(!importLutFile(in.path("bad.cube"), dir, &e, nullptr, &err) && err.find("bad.cube") == 0);
  write(in.path("1d.cube"), "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
  CHECK(!importLutFile(in.path("1d.cube"), dir, &e, nullptr, &err) && err.find("1D") != std::string::npos);
  {
    std::ofstream f(fsPath(in.path("huge.cube")), std::ios::binary);
    f << std::string(kLutMaxFileBytes + 1, ' ');
  }
  CHECK(!importLutFile(in.path("huge.cube"), dir, &e, nullptr, &err) && err.find("too large") != std::string::npos);
  CHECK(listLutFiles(dir).empty()); // nothing was copied

  // A full library takes no more.
  for (size_t i = 0; i < kLutMaxFiles; ++i)
    write(dir + "/f" + std::to_string(i) + ".cube", cubeText(2));
  write(in.path("one more.cube"), cubeText(3));
  CHECK(!importLutFile(in.path("one more.cube"), dir, &e, nullptr, &err) && err.find("full") != std::string::npos);
}

static void testResolve()
{
  TempDir lib;
  lutDirOverride() = lib.root();
  clearLutCache();
  CHECK(lutDir() == lib.root());

  CHECK(acquireLut(LutRef{}).status == LutStatus::None && !acquireLut(LutRef{}).lut);
  CHECK(acquireLut(LutRef{"nope", 0}).status == LutStatus::Missing && !acquireLut(LutRef{"nope", 0}).lut);

  const std::string text = cubeText(9);
  write(lutPath(lib.root(), "Good"), text);
  const uint64_t h = lutHashBytes(reinterpret_cast<const uint8_t *>(text.data()), text.size());
  LutResolved r = acquireLut(LutRef{"Good", h});
  CHECK(r.status == LutStatus::Ok && r.lut && r.lut->size == 9 && r.lut->valid());
  CHECK(r.hash == h); // the file's own hash, what a new LutRef should store
  CHECK(acquireLut(LutRef{"Good", 0}).hash == h && acquireLut(LutRef{"Good", h + 5}).hash == h); // whatever the reference said
  CHECK(acquireLut(LutRef{"nope", 0}).hash == 0 && acquireLut(LutRef{}).hash == 0);
  // cached: the same table comes back
  CHECK(acquireLut(LutRef{"Good", h}).lut == r.lut);
  // unknown hash (0) accepts the file; a different hash still resolves, flagged
  CHECK(acquireLut(LutRef{"Good", 0}).status == LutStatus::Ok);
  const LutResolved changed = acquireLut(LutRef{"Good", h + 1});
  CHECK(changed.status == LutStatus::HashChanged && changed.lut == r.lut);

  // a damaged file
  write(lutPath(lib.root(), "Broken"), "LUT_3D_SIZE 2\n1 2\n");
  CHECK(acquireLut(LutRef{"Broken", 0}).status == LutStatus::Unreadable && !acquireLut(LutRef{"Broken", 0}).lut);

  // a failure is remembered until the cache is cleared (a missing file costs one check, not one per frame)
  write(lutPath(lib.root(), "Late"), text);
  clearLutCache();
  CHECK(acquireLut(LutRef{"Late", 0}).status == LutStatus::Ok);
  CHECK(deleteLutFile(lutPath(lib.root(), "Late")));
  CHECK(acquireLut(LutRef{"Late", 0}).status == LutStatus::Ok); // still cached
  clearLutCache();
  CHECK(acquireLut(LutRef{"Late", 0}).status == LutStatus::Missing);
  write(lutPath(lib.root(), "Late"), text);
  CHECK(acquireLut(LutRef{"Late", 0}).status == LutStatus::Missing); // remembered
  clearLutCache();
  CHECK(acquireLut(LutRef{"Late", 0}).status == LutStatus::Ok);

  // a name that tries to climb out of the library finds nothing, and reads nothing outside
  write(lib.path("../escape.cube"), text);
  CHECK(acquireLut(LutRef{"../escape", 0}).status == LutStatus::Missing);
  std::error_code ec;
  fs::remove(fsPath(lib.path("../escape.cube")), ec);

  // the cache holds a few tables and drops the oldest
  clearLutCache();
  for (size_t i = 0; i < kLutCacheEntries + 2; ++i)
    write(lutPath(lib.root(), "c" + std::to_string(i)), cubeText(2 + static_cast<int>(i)));
  std::vector<LutResolved> got;
  for (size_t i = 0; i < kLutCacheEntries + 2; ++i)
    got.push_back(acquireLut(LutRef{"c" + std::to_string(i), 0}));
  for (const auto &g : got)
    CHECK(g.status == LutStatus::Ok);
  CHECK(acquireLut(LutRef{"c5", 0}).lut == got[5].lut);  // newest: still cached
  CHECK(acquireLut(LutRef{"c0", 0}).lut != got[0].lut);  // oldest: was dropped, loaded again
  CHECK(got[0].lut && got[0].lut->size == 2);            // ...and the old copy stays valid for its holder

  // injection for tests that need no file
  clearLutCache();
  auto inj = std::make_shared<const Lut3D>(identityLut(3));
  lutCachePut("Injected", inj, 42);
  CHECK(acquireLut(LutRef{"Injected", 42}).lut == inj && acquireLut(LutRef{"Injected", 42}).status == LutStatus::Ok);
  CHECK(acquireLut(LutRef{"Injected", 7}).status == LutStatus::HashChanged);
  CHECK(acquireLut(LutRef{"injected", 42}).lut == inj); // names compare without case, like the file system

  // the same table can be asked for by several threads at once
  clearLutCache();
  std::vector<std::thread> ts;
  std::vector<LutResolved> rs(6);
  for (size_t i = 0; i < rs.size(); ++i)
    ts.emplace_back([&, i]
                    { rs[i] = acquireLut(LutRef{"Good", 0}); });
  for (auto &t : ts)
    t.join();
  for (const auto &x : rs)
    CHECK(x.status == LutStatus::Ok && x.lut && x.lut->size == 9);

  lutDirOverride().clear();
  clearLutCache();
}

static void testLoadFile()
{
  TempDir lib;
  const std::string p = lib.path("x.cube");
  write(p, cubeText(4));
  Lut3D L;
  LutCubeInfo info;
  uint64_t h = 0;
  std::string err;
  CHECK(loadLutFile(p, &L, &info, &h, &err) && L.size == 4 && h != 0);
  CHECK(!loadLutFile(lib.path("none.cube"), &L, &info, &h, &err) && !err.empty());
  // uncached: it never touches the cache
  lutDirOverride() = lib.root();
  clearLutCache();
  CHECK(loadLutFile(p, &L, &info, &h, &err));
  write(lutPath(lib.root(), "x"), cubeText(7));
  const LutResolved fresh = acquireLut(LutRef{"x", 0});
  CHECK(fresh.lut && fresh.lut->size == 7); // the cache had nothing for it
  lutDirOverride().clear();
  clearLutCache();
}

int main()
{
  testHash();
  testNamesAndPaths();
  testImport();
  testListEntries();
  testImportRefusals();
  testResolve();
  testLoadFile();
  return finish();
}