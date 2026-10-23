// Build: g++ -std=c++20 -I../src -I. preset_test.cpp -o preset_test && ./preset_test
#include "io/preset.hpp"
#include "check.hpp"

#include <chrono>

static Preset sample()
{
  Preset p;
  p.name = "Warm Film";
  p.values[adjIndex(AdjId::Exposure)] = 12.5f;
  p.values[adjIndex(AdjId::Temperature)] = 30.0f;
  p.values[adjIndex(AdjId::Sepia)] = 20.0f;
  p.values[adjIndex(AdjId::Clarity)] = -15.0f;
  p.curves[0] = CurvePoints{{0, 10}, {128, 140}, {255, 255}};
  p.mix[mixIndex(MixChan::Hue, 1)] = 8.0f;
  p.grade.wheel[0].hue = 220.0f;
  p.grade.wheel[0].sat = 30.0f;
  p.grade.blending = 60.0f;
  return p;
}

static void testNames()
{
  CHECK(sanitizePresetName("  Hello  ") == "Hello");
  CHECK(sanitizePresetName("caf\xC3\xA9 x") == "caf x"); // non-ASCII dropped
  CHECK(sanitizePresetName("a\tb\nc") == "abc");
  CHECK(presetNameBlank("") && presetNameBlank("   ") && !presetNameBlank("x"));
  const std::string longName(100, 'a');
  CHECK(sanitizePresetName(longName).size() == kPresetNameMax);
  CHECK(sanitizePresetName(std::string(31, 'a') + "  b") == std::string(31, 'a')); // no trailing space after the cap

  CHECK(presetFileStem("Warm Film") == "Warm Film");
  CHECK(presetFileStem("a/b\\c:d*e?f") == "a_b_c_d_e_f");
  CHECK(presetFileStem("") == "preset" && presetFileStem("  ") == "preset");
  CHECK(presetFileStem("name.") == "name");
  CHECK(presetFileStem("con") == "_con" && presetFileStem("COM1") == "_COM1" &&
        presetFileStem("COM10") == "COM10" && presetFileStem("lpt9") == "_lpt9");
  CHECK(presetPath("d", "x") == "d/x.literoom-preset.json");
}

static void testJsonRoundTrip()
{
  const Preset p = sample();
  Preset q;
  CHECK(presetFromJson(presetToJson(p), &q));
  CHECK(q == p);

  // through text, too
  mj::Json parsed;
  std::string err;
  CHECK(mj::Json::parse(presetToJson(p).dump(), &parsed, &err));
  Preset r;
  CHECK(presetFromJson(parsed, &r) && r == p);

  // the default look round-trips and serializes as empty groups
  Preset d;
  d.name = "Neutral";
  Preset d2;
  CHECK(presetFromJson(presetToJson(d), &d2) && d2 == d);
  CHECK(presetToJson(d).get("adjust").size() == 0);
}

static void testRejectsAndClamps()
{
  Preset out;
  out.name = "untouched";
  CHECK(!presetFromJson(mj::Json::array(), &out));
  CHECK(!presetFromJson(mj::Json::object(), &out)); // no kind
  CHECK(out.name == "untouched");

  mj::Json j = presetToJson(sample());
  j.set("kind", mj::Json::str("edits"));
  CHECK(!presetFromJson(j, &out));
  j = presetToJson(sample());
  j.set("version", mj::Json::num(kPresetVersion + 1));
  CHECK(!presetFromJson(j, &out)); // newer than we know
  j.set("version", mj::Json::num(0));
  CHECK(!presetFromJson(j, &out));
  j.set("version", mj::Json::str("1"));
  CHECK(!presetFromJson(j, &out));
  CHECK(out.name == "untouched");

  // out-of-range and hostile values are clamped / dropped
  j = presetToJson(sample());
  mj::Json adj = mj::Json::object();
  adj.set("Exposure", mj::Json::num(9999));
  adj.set("Contrast", mj::Json::str("big"));
  adj.set("Sepia", mj::Json::num(-50));
  j.set("adjust", adj);
  j.set("curves", mj::Json::object().set("rgb", mj::Json::str("junk")));
  j.set("name", mj::Json::str("  \x01" "Evil\x7F" " name  "));
  CHECK(presetFromJson(j, &out));
  CHECK(out.values[adjIndex(AdjId::Exposure)] == 100.0f);
  CHECK(out.values[adjIndex(AdjId::Contrast)] == 0.0f);
  CHECK(out.values[adjIndex(AdjId::Sepia)] == 0.0f);
  CHECK(out.curves[0] == identityCurve());
  CHECK(out.name == "Evil name");
}

static void testFiles()
{
  namespace fs = std::filesystem;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root = fs::temp_directory_path() / ("literoom_preset_" + std::to_string(stamp));
  const std::string dir = u8ToString(root / "nested" / "presets"); // does not exist yet

  CHECK(listPresetFiles(dir).empty());

  std::string err, path;
  CHECK(!savePresetFile(dir, Preset{}, &err) && !err.empty()); // blank name refused

  Preset p = sample();
  CHECK(savePresetFile(dir, p, &err, &path));
  CHECK(path == presetPath(dir, p.name) && fs::exists(fsPath(path)));
  CHECK(!fs::exists(fsPath(path + ".tmp")));

  Preset back;
  CHECK(loadPresetFile(path, &back, &err) && back == p);

  Preset p2 = p;
  p2.name = "Cool 2";
  p2.values[adjIndex(AdjId::Temperature)] = -30.0f;
  Preset p3 = p;
  p3.name = "Cool 10";
  CHECK(savePresetFile(dir, p2, &err) && savePresetFile(dir, p3, &err));
  CHECK(savePresetFile(dir, p, &err)); // same name overwrites
  const auto names = listPresetFiles(dir);
  CHECK(names.size() == 3 && names[0] == "Cool 2.literoom-preset.json" &&
        names[1] == "Cool 10.literoom-preset.json" &&
        names[2] == "Warm Film.literoom-preset.json"); // natural order

  // a stray file and a subfolder are ignored
  { std::ofstream(fsPath(dir + "/notes.txt")) << "hi"; }
  fs::create_directories(fsPath(dir + "/sub.literoom-preset.json"));
  CHECK(listPresetFiles(dir).size() == 3);

  // bad files
  const std::string bad = dir + "/bad.literoom-preset.json";
  { std::ofstream(fsPath(bad), std::ios::binary) << "{ not json"; }
  Preset keep = p;
  CHECK(!loadPresetFile(bad, &keep, &err) && !err.empty() && keep == p);
  { std::ofstream(fsPath(bad), std::ios::binary) << "{\"kind\":\"edits\",\"version\":1}"; }
  CHECK(!loadPresetFile(bad, &keep, &err));
  CHECK(!loadPresetFile(dir + "/missing.literoom-preset.json", &keep, &err));
  { std::ofstream f(fsPath(bad), std::ios::binary); f << std::string(kPresetMaxFileBytes + 10, ' '); }
  CHECK(!loadPresetFile(bad, &keep, &err));

  // a nameless preset takes its file name
  {
    mj::Json j = presetToJson(p);
    j.set("name", mj::Json::str(""));
    std::ofstream(fsPath(dir + "/From File.literoom-preset.json"), std::ios::binary) << j.dump();
  }
  Preset nameless;
  CHECK(loadPresetFile(dir + "/From File.literoom-preset.json", &nameless, &err) &&
        nameless.name == "From File");

  CHECK(deletePresetFile(path) && !fs::exists(fsPath(path)));
  CHECK(!deletePresetFile(path)); // already gone

  std::error_code ec;
  fs::remove_all(root, ec);
}

int main()
{
  testNames();
  testJsonRoundTrip();
  testRejectsAndClamps();
  testFiles();
  CHECK(!presetDir().empty());
  return finish();
}
