// Build: g++ -std=c++20 -I.. photo_meta_test.cpp -o photo_meta_test && ./photo_meta_test
#include "io/photo_meta.hpp"
#include "check.hpp"

using mj::Json;

static PhotoMeta load(const std::string &text)
{
  Json j;
  std::string err;
  if (!Json::parse(text, &j, &err))
    return PhotoMeta{};
  return photoMetaFromJson(j);
}

static void testDefaultsAndRoundTrip()
{
  CHECK(PhotoMeta{}.isDefault());
  CHECK(photoMetaToJson(PhotoMeta{}).isObject());
  CHECK(photoMetaToJson(PhotoMeta{}).size() == 0); // a default field is left out

  for (int r = 0; r <= kMaxRating; ++r)
    for (PhotoFlag f : {PhotoFlag::None, PhotoFlag::Pick, PhotoFlag::Reject})
      for (int l = 0; l <= kLabelCount; ++l)
      {
        const PhotoMeta m{r, f, l};
        const PhotoMeta back = load(photoMetaToJson(m).dump());
        CHECK(back == m);
      }

  const Json one = photoMetaToJson(PhotoMeta{3, PhotoFlag::Pick, 1});
  CHECK(one.get("rating").asNum(-1) == 3);
  CHECK(one.get("flag").asStr() == "pick");
  CHECK(one.get("label").asStr() == "red");
  CHECK(photoMetaToJson(PhotoMeta{0, PhotoFlag::Reject, 5}).get("label").asStr() == "purple");
  CHECK(photoMetaToJson(PhotoMeta{0, PhotoFlag::Reject, 0}).find("rating") == nullptr);
  CHECK(photoMetaToJson(PhotoMeta{0, PhotoFlag::Reject, 0}).find("label") == nullptr);
}

static void testHostileInput()
{
  CHECK(load(R"({"rating": 99})").rating == kMaxRating);
  CHECK(load(R"({"rating": -4})").rating == 0);
  CHECK(load(R"({"rating": 1e300})").rating == kMaxRating);
  CHECK(load(R"({"rating": "5"})").rating == 0);   // not a number
  CHECK(load(R"({"rating": null})").rating == 0);
  CHECK(load(R"({"flag": "PICK"})").flag == PhotoFlag::None); // names are exact
  CHECK(load(R"({"flag": 1})").flag == PhotoFlag::None);
  CHECK(load(R"({"flag": "maybe"})").flag == PhotoFlag::None);
  CHECK(load(R"({"label": "orange"})").label == 0);
  CHECK(load(R"({"label": 2})").label == 0);
  CHECK(load(R"({"label": "yellow"})").label == 2);
  CHECK(load(R"([1,2,3])").isDefault());
  CHECK(load(R"("pick")").isDefault());
  CHECK(load(R"({})").isDefault());
  // Unknown members are ignored; the known ones still load.
  CHECK(load(R"({"rating": 2, "stars": 5, "x": {}})") == PhotoMeta{2, PhotoFlag::None, 0});

  // A hand-built PhotoMeta is forced into range on the way out.
  const PhotoMeta bad{-77, static_cast<PhotoFlag>(9), -3};
  CHECK(photoMetaSanitized(bad) == PhotoMeta{});
  CHECK(photoMetaToJson(bad).size() == 0);
  CHECK(photoMetaSanitized(PhotoMeta{77, PhotoFlag::None, 99}) == PhotoMeta{kMaxRating, PhotoFlag::None, kLabelCount});
  CHECK(photoMetaToJson(PhotoMeta{77, PhotoFlag::None, 0}).get("rating").asNum(0) == kMaxRating);
  CHECK(photoMetaSanitized(PhotoMeta{-1, PhotoFlag::Reject, 99}) == PhotoMeta{0, PhotoFlag::Reject, kLabelCount});
}

static void testFilter()
{
  const MetaFilter off;
  CHECK(!off.active());
  CHECK(off.matches(PhotoMeta{}));
  CHECK(off.matches(PhotoMeta{5, PhotoFlag::Reject, 3}));

  MetaFilter f;
  f.toggleRating(3);
  CHECK(f.active() && f.minRating == 3);
  CHECK(!f.matches(PhotoMeta{2, PhotoFlag::None, 0}));
  CHECK(f.matches(PhotoMeta{3, PhotoFlag::None, 0}));
  CHECK(f.matches(PhotoMeta{5, PhotoFlag::Reject, 0})); // "at least"
  f.toggleRating(3); // the value already set switches it off
  CHECK(!f.active());
  f.toggleRating(9); // clamped
  CHECK(f.minRating == kMaxRating);
  f.toggleRating(-2); // clamped to 0 = off
  CHECK(f.minRating == 0);

  f = MetaFilter{};
  f.toggleFlag(PhotoFlag::Pick);
  CHECK(f.matches(PhotoMeta{0, PhotoFlag::Pick, 0}));
  CHECK(!f.matches(PhotoMeta{0, PhotoFlag::None, 0}));
  CHECK(!f.matches(PhotoMeta{0, PhotoFlag::Reject, 0}));
  f.toggleFlag(PhotoFlag::Reject); // OR within a criterion
  CHECK(f.matches(PhotoMeta{0, PhotoFlag::Reject, 0}));
  CHECK(!f.matches(PhotoMeta{0, PhotoFlag::None, 0}));
  f.toggleFlag(PhotoFlag::Pick);
  f.toggleFlag(PhotoFlag::Reject);
  CHECK(!f.active());

  f = MetaFilter{};
  f.toggleLabel(1);
  f.toggleLabel(4);
  CHECK(f.matches(PhotoMeta{0, PhotoFlag::None, 1}));
  CHECK(f.matches(PhotoMeta{0, PhotoFlag::None, 4}));
  CHECK(!f.matches(PhotoMeta{0, PhotoFlag::None, 2}));
  CHECK(!f.matches(PhotoMeta{0, PhotoFlag::None, 0})); // no label doesn't match a color
  f.toggleLabel(0);  // out of range: ignored
  f.toggleLabel(6);
  f.toggleLabel(-1);
  CHECK(f.labels == ((1u << 1) | (1u << 4)));

  // AND across criteria.
  f = MetaFilter{};
  f.toggleRating(4);
  f.toggleFlag(PhotoFlag::Pick);
  f.toggleLabel(3);
  CHECK(f.matches(PhotoMeta{4, PhotoFlag::Pick, 3}));
  CHECK(!f.matches(PhotoMeta{3, PhotoFlag::Pick, 3}));
  CHECK(!f.matches(PhotoMeta{4, PhotoFlag::None, 3}));
  CHECK(!f.matches(PhotoMeta{4, PhotoFlag::Pick, 2}));
  CHECK(f == f && !(f == MetaFilter{}));
}

static void testPatch()
{
  CHECK(MetaPatch{}.empty());
  CHECK(MetaPatch{}.applyTo(PhotoMeta{3, PhotoFlag::Pick, 2}) == PhotoMeta({3, PhotoFlag::Pick, 2}));

  const PhotoMeta base{3, PhotoFlag::Pick, 2};
  MetaPatch p;
  p.rating = 5;
  CHECK(!p.empty());
  CHECK(p.applyTo(base) == PhotoMeta({5, PhotoFlag::Pick, 2})); // the other fields are kept
  p = MetaPatch{};
  p.flag = PhotoFlag::Reject;
  CHECK(p.applyTo(base) == PhotoMeta({3, PhotoFlag::Reject, 2}));
  p = MetaPatch{};
  p.label = 0; // "set to none" is a change, not an empty patch
  CHECK(!p.empty());
  CHECK(p.applyTo(base) == PhotoMeta({3, PhotoFlag::Pick, 0}));
  p = MetaPatch{};
  p.rating = 0;
  p.flag = PhotoFlag::None;
  p.label = 0;
  CHECK(p.applyTo(base).isDefault());
  CHECK(p.applyTo(PhotoMeta{}).isDefault());

  p = MetaPatch{}; // out-of-range values are forced into range
  p.rating = 99;
  p.label = -4;
  CHECK(p.applyTo(PhotoMeta{}) == PhotoMeta({kMaxRating, PhotoFlag::None, 0}));
  p.rating = -1;
  p.label = 77;
  CHECK(p.applyTo(PhotoMeta{}) == PhotoMeta({0, PhotoFlag::None, kLabelCount}));
}

int main()
{
  testPatch();
  testDefaultsAndRoundTrip();
  testHostileInput();
  testFilter();
  return finish();
}