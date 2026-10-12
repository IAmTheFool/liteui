// minijson.hpp - a tiny JSON value, writer and parser (UTF-8, no dependencies),
// plus base64 and byte run-length helpers. Used for literoom's edit sidecar.
//
// Numbers go through printf/strtod, i.e. the C locale; literoom never calls
// setlocale, so "0.5" is always written and read with a dot.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mj {

class Json {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Json() = default;
  static Json flag(bool b) {
    Json j;
    j.type_ = Type::Bool;
    j.b_ = b;
    return j;
  }
  static Json num(double v) {
    Json j;
    j.type_ = Type::Number;
    j.n_ = std::isfinite(v) ? v : 0.0; // JSON has no NaN / Inf
    return j;
  }
  // A float written with the fewest digits that read back as the same float
  // (0.3f -> 0.3, not 0.300000012).
  static Json num(float v) {
    if (!std::isfinite(v))
      return num(0.0);
    char buf[40];
    for (int prec = 1; prec <= 9; ++prec) {
      std::snprintf(buf, sizeof buf, "%.*g", prec, static_cast<double>(v));
      if (std::strtof(buf, nullptr) == v)
        break;
    }
    return num(std::strtod(buf, nullptr));
  }
  static Json num(int v) { return num(static_cast<double>(v)); }
  static Json str(std::string s) {
    Json j;
    j.type_ = Type::String;
    j.s_ = std::move(s);
    return j;
  }
  static Json array() {
    Json j;
    j.type_ = Type::Array;
    return j;
  }
  static Json object() {
    Json j;
    j.type_ = Type::Object;
    return j;
  }

  Type type() const { return type_; }
  bool isNull() const { return type_ == Type::Null; }
  bool isBool() const { return type_ == Type::Bool; }
  bool isNumber() const { return type_ == Type::Number; }
  bool isString() const { return type_ == Type::String; }
  bool isArray() const { return type_ == Type::Array; }
  bool isObject() const { return type_ == Type::Object; }

  double asNum(double def) const { return isNumber() ? n_ : def; }
  bool asFlag(bool def) const { return isBool() ? b_ : def; }
  const std::string &asStr() const { return s_; }

  // Array access.
  Json &push(Json v) {
    items_.push_back(std::move(v));
    return *this;
  }
  const std::vector<Json> &items() const { return items_; }
  size_t size() const { return isObject() ? vals_.size() : items_.size(); }

  // Object access (insertion order is kept, so files diff cleanly).
  Json &set(const std::string &key, Json v) {
    for (size_t i = 0; i < keys_.size(); ++i)
      if (keys_[i] == key) {
        vals_[i] = std::move(v);
        return *this;
      }
    keys_.push_back(key);
    vals_.push_back(std::move(v));
    return *this;
  }
  const Json *find(std::string_view key) const {
    for (size_t i = 0; i < keys_.size(); ++i)
      if (keys_[i] == key)
        return &vals_[i];
    return nullptr;
  }

  std::string dump() const {
    std::string out;
    write(out, 0);
    out += '\n';
    return out;
  }

  // Parses `text`; on failure returns false and (optionally) says where.
  static bool parse(std::string_view text, Json *out, std::string *err) {
    Parser p{text, 0, err};
    Json v;
    p.ws();
    if (!p.value(v, 0))
      return false;
    p.ws();
    if (p.i != text.size())
      return p.fail("trailing characters");
    *out = std::move(v);
    return true;
  }

private:
  Type type_ = Type::Null;
  bool b_ = false;
  double n_ = 0;
  std::string s_;
  std::vector<Json> items_;
  std::vector<std::string> keys_; // parallel to vals_ (Object)
  std::vector<Json> vals_;

  // ---- writer ----
  static void quote(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
      switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else
          o += static_cast<char>(c); // UTF-8 passes through
      }
    }
    o += '"';
  }
  static void number(std::string &o, double v) {
    char buf[40];
    if (v == std::floor(v) && std::fabs(v) < 1e15) {
      std::snprintf(buf, sizeof buf, "%.0f", v);
    } else {
      for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v)
          break;
      }
    }
    o += buf;
  }
  bool allScalars() const {
    for (const Json &j : items_)
      if (j.isArray() || j.isObject())
        return false;
    return true;
  }
  void write(std::string &o, int depth) const {
    switch (type_) {
    case Type::Null: o += "null"; break;
    case Type::Bool: o += b_ ? "true" : "false"; break;
    case Type::Number: number(o, n_); break;
    case Type::String: quote(o, s_); break;
    case Type::Array:
      if (items_.empty()) {
        o += "[]";
      } else if (allScalars()) { // numbers etc.: rows of 16 per line
        o += '[';
        for (size_t i = 0; i < items_.size(); ++i) {
          if (i) {
            o += ',';
            if (i % 16 == 0) {
              o += '\n';
              o.append(static_cast<size_t>(depth + 1) * 2, ' ');
            } else
              o += ' ';
          }
          items_[i].write(o, depth + 1);
        }
        o += ']';
      } else {
        o += "[\n";
        for (size_t i = 0; i < items_.size(); ++i) {
          o.append(static_cast<size_t>(depth + 1) * 2, ' ');
          items_[i].write(o, depth + 1);
          o += i + 1 < items_.size() ? ",\n" : "\n";
        }
        o.append(static_cast<size_t>(depth) * 2, ' ');
        o += ']';
      }
      break;
    case Type::Object:
      if (keys_.empty()) {
        o += "{}";
      } else {
        o += "{\n";
        for (size_t i = 0; i < keys_.size(); ++i) {
          o.append(static_cast<size_t>(depth + 1) * 2, ' ');
          quote(o, keys_[i]);
          o += ": ";
          vals_[i].write(o, depth + 1);
          o += i + 1 < keys_.size() ? ",\n" : "\n";
        }
        o.append(static_cast<size_t>(depth) * 2, ' ');
        o += '}';
      }
      break;
    }
  }

  // ---- parser ----
  struct Parser {
    std::string_view s;
    size_t i;
    std::string *err;
    static constexpr int kMaxDepth = 64;

    bool fail(const char *m) {
      if (err)
        *err = std::string(m) + " at byte " + std::to_string(i);
      return false;
    }
    void ws() {
      while (i < s.size() &&
             (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        ++i;
    }
    bool lit(std::string_view w) {
      if (s.substr(i, w.size()) != w)
        return false;
      i += w.size();
      return true;
    }
    bool value(Json &out, int depth) {
      if (depth > kMaxDepth)
        return fail("nesting too deep");
      if (i >= s.size())
        return fail("unexpected end");
      const char c = s[i];
      if (c == '{')
        return object(out, depth);
      if (c == '[')
        return arr(out, depth);
      if (c == '"') {
        std::string t;
        if (!string(t))
          return false;
        out = Json::str(std::move(t));
        return true;
      }
      if (c == 't' && lit("true")) {
        out = Json::flag(true);
        return true;
      }
      if (c == 'f' && lit("false")) {
        out = Json::flag(false);
        return true;
      }
      if (c == 'n' && lit("null")) {
        out = Json();
        return true;
      }
      if (c == '-' || (c >= '0' && c <= '9'))
        return numeric(out);
      return fail("unexpected character");
    }
    bool numeric(Json &out) {
      const size_t b = i;
      if (s[i] == '-')
        ++i;
      while (i < s.size() &&
             ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
              s[i] == 'E' || s[i] == '+' || s[i] == '-'))
        ++i;
      const std::string t(s.substr(b, i - b));
      char *end = nullptr;
      const double v = std::strtod(t.c_str(), &end);
      if (t.empty() || end != t.c_str() + t.size() || !std::isfinite(v))
        return fail("bad number");
      out = Json::num(v);
      return true;
    }
    static void utf8(std::string &o, unsigned cp) {
      if (cp < 0x80)
        o += static_cast<char>(cp);
      else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
      } else if (cp < 0x10000) {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        o += static_cast<char>(0xF0 | (cp >> 18));
        o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    bool hex4(unsigned &v) {
      if (i + 4 > s.size())
        return fail("bad \\u escape");
      v = 0;
      for (int k = 0; k < 4; ++k) {
        const char c = s[i++];
        v <<= 4;
        if (c >= '0' && c <= '9')
          v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f')
          v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
          v |= static_cast<unsigned>(c - 'A' + 10);
        else
          return fail("bad \\u escape");
      }
      return true;
    }
    bool string(std::string &out) {
      ++i; // opening quote
      while (i < s.size()) {
        const char c = s[i++];
        if (c == '"')
          return true;
        if (static_cast<unsigned char>(c) < 0x20)
          return fail("control character in string");
        if (c != '\\') {
          out += c;
          continue;
        }
        if (i >= s.size())
          break;
        const char e = s[i++];
        switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp))
            return false;
          if (cp >= 0xD800 && cp < 0xDC00) { // high surrogate: need its pair
            unsigned lo;
            if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
              return fail("bad surrogate pair");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF)
            return fail("lone surrogate");
          utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
        }
      }
      return fail("unterminated string");
    }
    bool arr(Json &out, int depth) {
      ++i; // [
      out = Json::array();
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
        return true;
      }
      for (;;) {
        Json v;
        ws();
        if (!value(v, depth + 1))
          return false;
        out.push(std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == ']') {
          ++i;
          return true;
        }
        return fail("expected , or ]");
      }
    }
    bool object(Json &out, int depth) {
      ++i; // {
      out = Json::object();
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
      }
      for (;;) {
        ws();
        if (i >= s.size() || s[i] != '"')
          return fail("expected key");
        std::string key;
        if (!string(key))
          return false;
        ws();
        if (i >= s.size() || s[i] != ':')
          return fail("expected :");
        ++i;
        ws();
        Json v;
        if (!value(v, depth + 1))
          return false;
        out.set(key, std::move(v));
        ws();
        if (i < s.size() && s[i] == ',') {
          ++i;
          continue;
        }
        if (i < s.size() && s[i] == '}') {
          ++i;
          return true;
        }
        return fail("expected , or }");
      }
    }
  };
};

// ---- base64 (RFC 4648, padded) ----
inline std::string base64Encode(const std::vector<uint8_t> &d) {
  static const char *T =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve((d.size() + 2) / 3 * 4);
  for (size_t i = 0; i < d.size(); i += 3) {
    const uint32_t b0 = d[i], b1 = i + 1 < d.size() ? d[i + 1] : 0,
                   b2 = i + 2 < d.size() ? d[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    o += T[(v >> 18) & 63];
    o += T[(v >> 12) & 63];
    o += i + 1 < d.size() ? T[(v >> 6) & 63] : '=';
    o += i + 2 < d.size() ? T[v & 63] : '=';
  }
  return o;
}
inline bool base64Decode(std::string_view s, std::vector<uint8_t> *out) {
  out->clear();
  if (s.size() % 4 != 0)
    return false;
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out->reserve(s.size() / 4 * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    const bool p2 = s[i + 2] == '=', p3 = s[i + 3] == '=';
    if ((p2 && !p3) || ((p2 || p3) && i + 4 != s.size()))
      return false;
    const int a = val(s[i]), b = val(s[i + 1]);
    const int c = p2 ? 0 : val(s[i + 2]), d = p3 ? 0 : val(s[i + 3]);
    if (a < 0 || b < 0 || c < 0 || d < 0)
      return false;
    const uint32_t v = (static_cast<uint32_t>(a) << 18) |
                       (static_cast<uint32_t>(b) << 12) |
                       (static_cast<uint32_t>(c) << 6) | static_cast<uint32_t>(d);
    out->push_back(static_cast<uint8_t>(v >> 16));
    if (!p2)
      out->push_back(static_cast<uint8_t>(v >> 8));
    if (!p3)
      out->push_back(static_cast<uint8_t>(v));
  }
  return true;
}

// ---- byte run-length coding: (count 1..255, value) pairs ----
inline std::vector<uint8_t> rleEncode(const std::vector<uint8_t> &in) {
  std::vector<uint8_t> o;
  for (size_t i = 0; i < in.size();) {
    size_t n = 1;
    while (i + n < in.size() && in[i + n] == in[i] && n < 255)
      ++n;
    o.push_back(static_cast<uint8_t>(n));
    o.push_back(in[i]);
    i += n;
  }
  return o;
}
// False unless the runs add up to exactly `expected` bytes.
inline bool rleDecode(const std::vector<uint8_t> &in, size_t expected,
                      std::vector<uint8_t> *out) {
  out->clear();
  if (in.size() % 2 != 0)
    return false;
  out->reserve(expected);
  for (size_t i = 0; i < in.size(); i += 2) {
    if (in[i] == 0 || out->size() + in[i] > expected)
      return false;
    out->insert(out->end(), in[i], in[i + 1]);
  }
  return out->size() == expected;
}

} // namespace mj