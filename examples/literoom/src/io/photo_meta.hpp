// photo_meta.hpp - A picture's rating, flag and color label, and the filmstrip
// filter that selects by them. Pure values and JSON: no UI, no editor state, no
// files.
//
// These are not edits: they never go through AdjustmentSet, have no undo step and
// no History label. They live in the sidecar's top-level "meta" object (next to
// "image" and "edits", see sidecar.hpp) and are written only when something
// differs from the default, so a picture that was never rated has no "meta" key.
// Loading is as strict as for the edits: nothing in the file is trusted.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "io/minijson.hpp"

constexpr int kMaxRating = 5; // 0 = unrated

enum class PhotoFlag : uint8_t
{
  None = 0,
  Pick = 1,
  Reject = 2
};

// Color labels are numbered 1..kLabelCount; 0 means no label.
constexpr int kLabelCount = 5;
constexpr std::array<const char *, kLabelCount> kLabelNames = {
    "red", "yellow", "green", "blue", "purple"};

struct PhotoMeta
{
  int rating = 0; // 0..kMaxRating
  PhotoFlag flag = PhotoFlag::None;
  int label = 0; // 0..kLabelCount
  bool operator==(const PhotoMeta &) const = default;
  bool isDefault() const { return *this == PhotoMeta{}; }
};

// The same values forced into range (a hostile or hand-built PhotoMeta).
inline PhotoMeta photoMetaSanitized(PhotoMeta m)
{
  m.rating = std::clamp(m.rating, 0, kMaxRating);
  if (m.flag != PhotoFlag::Pick && m.flag != PhotoFlag::Reject)
    m.flag = PhotoFlag::None;
  m.label = std::clamp(m.label, 0, kLabelCount);
  return m;
}

// ---------------- JSON ----------------
// {"rating": 3, "flag": "pick", "label": "red"}; a default field is left out,
// so a default PhotoMeta is an empty object.
inline mj::Json photoMetaToJson(const PhotoMeta &in)
{
  using mj::Json;
  const PhotoMeta m = photoMetaSanitized(in);
  Json o = Json::object();
  if (m.rating > 0)
    o.set("rating", Json::num(m.rating));
  if (m.flag == PhotoFlag::Pick)
    o.set("flag", Json::str("pick"));
  else if (m.flag == PhotoFlag::Reject)
    o.set("flag", Json::str("reject"));
  if (m.label > 0)
    o.set("label", Json::str(kLabelNames[static_cast<size_t>(m.label - 1)]));
  return o;
}

// Anything that isn't what the writer produces becomes the default for that
// field: a non-number rating is 0, a rating out of range is clamped, an unknown
// flag or label name is "none". A non-object gives the default PhotoMeta.
inline PhotoMeta photoMetaFromJson(const mj::Json &o)
{
  PhotoMeta m;
  if (!o.isObject())
    return m;
  if (const mj::Json *r = o.find("rating"); r && r->isNumber())
    m.rating = r->asInt(0, 0, kMaxRating);
  if (const mj::Json *f = o.find("flag"); f && f->isString())
  {
    if (f->asStr() == "pick")
      m.flag = PhotoFlag::Pick;
    else if (f->asStr() == "reject")
      m.flag = PhotoFlag::Reject;
  }
  if (const mj::Json *l = o.find("label"); l && l->isString())
    for (int i = 0; i < kLabelCount; ++i)
      if (l->asStr() == kLabelNames[static_cast<size_t>(i)])
        m.label = i + 1;
  return m;
}

// ---------------- Patch ----------------
// "Set these fields": what one rating / flag / label command changes. The same
// patch is applied to the open picture and, in the background, to the other
// selected pictures, each keeping the fields the patch doesn't mention.
struct MetaPatch
{
  std::optional<int> rating;
  std::optional<PhotoFlag> flag;
  std::optional<int> label;

  bool empty() const { return !rating && !flag && !label; }
  PhotoMeta applyTo(PhotoMeta m) const
  {
    if (rating)
      m.rating = *rating;
    if (flag)
      m.flag = *flag;
    if (label)
      m.label = *label;
    return photoMetaSanitized(m);
  }
};

// ---------------- Filter ----------------
// What the filmstrip shows. Each criterion that is switched on must match
// (AND); within a criterion, any chosen value matches (OR). Everything off
// shows every picture.
struct MetaFilter
{
  int minRating = 0;  // at least this many stars; 0 = off
  unsigned flags = 0; // bit (1 << PhotoFlag value) per flag to accept; 0 = off
  unsigned labels = 0; // bit (1 << label) per color to accept (1..5); 0 = off

  bool operator==(const MetaFilter &) const = default;
  bool active() const { return minRating > 0 || flags != 0 || labels != 0; }

  bool matches(const PhotoMeta &m) const
  {
    if (minRating > 0 && m.rating < minRating)
      return false;
    if (flags != 0 && !(flags & (1u << static_cast<unsigned>(m.flag))))
      return false;
    if (labels != 0 && !(labels & (1u << static_cast<unsigned>(m.label))))
      return false;
    return true;
  }

  // "At least n stars"; choosing the value already set switches it off.
  void toggleRating(int n)
  {
    n = std::clamp(n, 0, kMaxRating);
    minRating = (n == minRating) ? 0 : n;
  }
  void toggleFlag(PhotoFlag f) { flags ^= 1u << static_cast<unsigned>(f); }
  void toggleLabel(int c)
  {
    if (c >= 1 && c <= kLabelCount)
      labels ^= 1u << static_cast<unsigned>(c);
  }
};