// Build: g++ -std=c++20 -I.. minijson_test.cpp -o minijson_test && ./minijson_test
#include "io/minijson.hpp"
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      std::printf("FAIL line %d: %s\n", __LINE__, #c);                         \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

int main() {
  using mj::Json;
  // Round trip of a nested document, floats with awkward values.
  Json o = Json::object();
  o.set("name", Json::str("caf\xC3\xA9 \"q\"\n\\"));
  o.set("f", Json::num(0.3f));
  o.set("g", Json::num(-1234.5678f));
  o.set("big", Json::num(16777217.0));
  o.set("flag", Json::flag(true));
  o.set("none", Json());
  Json a = Json::array();
  a.push(Json::num(1)).push(Json::num(2.5f)).push(Json::str("x"));
  o.set("a", a);
  Json nested = Json::array();
  nested.push(a).push(a);
  o.set("n", nested);
  const std::string text = o.dump();
  Json back;
  std::string err;
  CHECK(Json::parse(text, &back, &err));
  CHECK(back.find("name") && back.find("name")->asStr() == "caf\xC3\xA9 \"q\"\n\\");
  CHECK(static_cast<float>(back.find("f")->asNum(0)) == 0.3f);
  CHECK(static_cast<float>(back.find("g")->asNum(0)) == -1234.5678f);
  CHECK(back.find("big")->asNum(0) == 16777217.0);
  CHECK(back.find("flag")->asFlag(false));
  CHECK(back.find("none")->isNull());
  CHECK(back.find("n")->items().size() == 2);
  CHECK(back.dump() == text); // stable

  // Every float in a sweep survives text.
  for (int i = 0; i < 20000; ++i) {
    const float v = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * 1000.0f;
    Json t = Json::array();
    t.push(Json::num(v));
    Json r;
    CHECK(Json::parse(t.dump(), &r, &err));
    CHECK(static_cast<float>(r.items()[0].asNum(0)) == v);
  }

  // Escapes and surrogate pairs.
  Json e;
  CHECK(Json::parse("\"\\u00e9\\ud83d\\ude00\\n\"", &e, &err));
  CHECK(e.asStr() == "\xC3\xA9\xF0\x9F\x98\x80\n");

  // Malformed input is rejected, never crashes.
  const char *bad[] = {"", "{", "[1,]", "{\"a\":}", "{\"a\" 1}", "[1 2]",
                       "\"abc", "tru", "01x", "[1] x", "\"\\ud800\"",
                       "\"\\q\"", "nan", "[--1]"};
  for (const char *b : bad) {
    Json x;
    CHECK(!Json::parse(b, &x, &err));
  }
  std::string deep(200, '[');
  Json x;
  CHECK(!Json::parse(deep, &x, &err));

  // base64 / RLE.
  std::vector<uint8_t> raw;
  for (int i = 0; i < 1000; ++i)
    raw.push_back(static_cast<uint8_t>(i < 400 ? 0 : (i < 900 ? 255 : i)));
  for (size_t cut = 0; cut < 8; ++cut) {
    std::vector<uint8_t> part(raw.begin(), raw.begin() + 990 + cut), d;
    CHECK(mj::base64Decode(mj::base64Encode(part), &d) && d == part);
  }
  std::vector<uint8_t> rle = mj::rleEncode(raw), dec;
  CHECK(rle.size() < raw.size());
  CHECK(mj::rleDecode(rle, raw.size(), &dec) && dec == raw);
  CHECK(!mj::rleDecode(rle, raw.size() + 1, &dec));
  CHECK(!mj::rleDecode(rle, raw.size() - 1, &dec));
  CHECK(!mj::rleDecode({0, 7}, 0, &dec));
  std::vector<uint8_t> d2;
  CHECK(!mj::base64Decode("abc", &d2));
  CHECK(!mj::base64Decode("ab=c", &d2));
  CHECK(!mj::base64Decode("a!cd", &d2));

  std::printf(failures ? "%d FAILED\n" : "all ok\n", failures);
  return failures ? 1 : 0;
}