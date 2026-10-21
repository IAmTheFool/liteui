// minijson.hpp - a tiny JSON value, writer and parser (UTF-8, no dependencies),
// plus base64 and byte run-length helpers. Used for literoom's edit sidecar.
//
// Numbers are written and read locale-independently: the C library's decimal
// separator is swapped for '.' on write and back on read, so a comma-decimal
// locale can neither corrupt a file nor make a valid one unreadable.
//
// Save files in binary mode (dump() uses '\n') and ideally write to a temp
// file and rename over the original so a crash can't leave a partial sidecar.
#pragma once
#include <algorithm>
#include <cassert>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mj
{

  class Json
  {
  public:
    enum class Type
    {
      Null,
      Bool,
      Number,
      String,
      Array,
      Object
    };

    // Inputs larger than this are rejected before parsing (each Json value is
    // ~140 bytes in memory, so an unbounded file is an easy memory bomb).
    static constexpr std::size_t kMaxInputBytes = 64u * 1024u * 1024u;

    Json() = default;
    static Json flag(bool b)
    {
      Json j;
      j.type_ = Type::Bool;
      j.b_ = b;
      return j;
    }
    static Json num(double v)
    {
      Json j;
      j.type_ = Type::Number;
      if (!std::isfinite(v))
        v = 0.0; // JSON has no NaN / Inf
      if (v == 0.0)
        v = 0.0; // normalizes -0.0 to +0.0 so files don't flip between 0 and -0
      j.n_ = v;
      return j;
    }
    // A float written with the fewest digits that read back as the same float
    // (0.3f -> 0.3, not 0.300000012).
    static Json num(float v)
    {
      if (!std::isfinite(v))
        return num(0.0);
      char buf[40];
      for (int prec = 1; prec <= 9; ++prec)
      {
        std::snprintf(buf, sizeof buf, "%.*g", prec, static_cast<double>(v));
        if (std::strtof(buf, nullptr) == v) // buf is in the C locale's own form
          break;
      }
      return num(std::strtod(buf, nullptr));
    }
    // Any integer type (int, long, size_t, int64_t, ...). Values beyond 2^53
    // lose precision; store such values (hashes, mtimes) as strings instead.
    template <class T,
              std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>,
                               int> = 0>
    static Json num(T v)
    {
      return num(static_cast<double>(v));
    }
    static Json str(std::string s)
    {
      Json j;
      j.type_ = Type::String;
      j.s_ = std::move(s);
      return j;
    }
    static Json array()
    {
      Json j;
      j.type_ = Type::Array;
      return j;
    }
    static Json object()
    {
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

    // Clamped accessors: always safe to cast, even for hostile values like 1e300.
    int asInt(int def, int lo = INT_MIN, int hi = INT_MAX) const
    {
      double v = isNumber() ? n_ : static_cast<double>(def);
      v = std::clamp(v, static_cast<double>(lo), static_cast<double>(hi));
      return static_cast<int>(std::lround(v));
    }
    float asFloat(float def, float lo, float hi) const
    {
      double v = isNumber() ? n_ : static_cast<double>(def);
      v = std::clamp(v, static_cast<double>(lo), static_cast<double>(hi));
      return static_cast<float>(v);
    }

    // Array access. A Null value becomes an array on first push.
    Json &push(Json v)
    {
      if (type_ == Type::Null)
        type_ = Type::Array;
      assert(isArray());
      items_.push_back(std::move(v));
      return *this;
    }
    const std::vector<Json> &items() const { return items_; }
    const Json &at(std::size_t i) const { return items_.at(i); }
    std::size_t size() const { return isObject() ? vals_.size() : items_.size(); }

    // Object access (insertion order is kept, so files diff cleanly).
    // A Null value becomes an object on first set.
    Json &set(const std::string &key, Json v)
    {
      if (type_ == Type::Null)
        type_ = Type::Object;
      assert(isObject());
      for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key)
        {
          vals_[i] = std::move(v);
          return *this;
        }
      keys_.push_back(key);
      vals_.push_back(std::move(v));
      return *this;
    }
    const Json *find(std::string_view key) const
    {
      for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key)
          return &vals_[i];
      return nullptr;
    }
    Json *find(std::string_view key)
    {
      for (std::size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key)
          return &vals_[i];
      return nullptr;
    }
    // Like find(), but returns a shared Null value when the key is missing, so
    // lookups can be chained without null checks: j.get("a").get("b").asNum(0).
    const Json &get(std::string_view key) const
    {
      static const Json kNull;
      const Json *p = find(key);
      return p ? *p : kNull;
    }
    // Enumerate object members (parallel vectors, insertion order). Lets callers
    // round-trip keys they don't know about.
    const std::vector<std::string> &keys() const { return keys_; }
    const std::vector<Json> &values() const { return vals_; }

    std::string dump() const
    {
      std::string out;
      write(out, 0);
      out += '\n';
      return out;
    }

    // Parses `text`; on failure returns false and (optionally) says where.
    // A leading UTF-8 BOM is skipped.
    static bool parse(std::string_view text, Json *out, std::string *err)
    {
      if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
          static_cast<unsigned char>(text[1]) == 0xBB &&
          static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
      if (text.size() > kMaxInputBytes)
      {
        if (err)
          *err = "input too large";
        return false;
      }
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

    // ---- locale-independent number text ----
    static std::string decimalPoint()
    {
      const std::lconv *lc = std::localeconv();
      return (lc && lc->decimal_point && *lc->decimal_point) ? lc->decimal_point
                                                             : ".";
    }
    // printf output (locale form) -> JSON form.
    static std::string toJsonNumber(const char *buf)
    {
      std::string s(buf);
      const std::string dp = decimalPoint();
      if (dp != ".")
      {
        const std::size_t p = s.find(dp);
        if (p != std::string::npos)
          s.replace(p, dp.size(), ".");
      }
      return s;
    }
    // JSON form -> text strtod understands under the current locale.
    static std::string fromJsonNumber(std::string s)
    {
      const std::string dp = decimalPoint();
      if (dp != ".")
      {
        const std::size_t p = s.find('.');
        if (p != std::string::npos)
          s.replace(p, 1, dp);
      }
      return s;
    }

    // Strict UTF-8 check: no overlongs, surrogates, or code points > U+10FFFF.
    static bool validUtf8(const std::string &s)
    {
      std::size_t i = 0;
      const std::size_t n = s.size();
      while (i < n)
      {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80)
        {
          ++i;
          continue;
        }
        std::size_t len;
        unsigned cp;
        if (c >= 0xC2 && c <= 0xDF)
        {
          len = 2;
          cp = c & 0x1Fu;
        }
        else if (c >= 0xE0 && c <= 0xEF)
        {
          len = 3;
          cp = c & 0x0Fu;
        }
        else if (c >= 0xF0 && c <= 0xF4)
        {
          len = 4;
          cp = c & 0x07u;
        }
        else
          return false;
        if (i + len > n)
          return false;
        for (std::size_t k = 1; k < len; ++k)
        {
          const unsigned char cc = static_cast<unsigned char>(s[i + k]);
          if ((cc & 0xC0) != 0x80)
            return false;
          cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
          return false;
        i += len;
      }
      return true;
    }

    // ---- writer ----
    static void quote(std::string &o, const std::string &s)
    {
      o += '"';
      for (unsigned char c : s)
      {
        switch (c)
        {
        case '"':
          o += "\\\"";
          break;
        case '\\':
          o += "\\\\";
          break;
        case '\n':
          o += "\\n";
          break;
        case '\r':
          o += "\\r";
          break;
        case '\t':
          o += "\\t";
          break;
        default:
          if (c < 0x20)
          {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
          }
          else
            o += static_cast<char>(c); // UTF-8 passes through
        }
      }
      o += '"';
    }
    static void number(std::string &o, double v)
    {
      if (v == 0.0)
        v = 0.0; // never write "-0"
      char buf[40];
      if (v == std::floor(v) && std::fabs(v) < 1e15)
      {
        std::snprintf(buf, sizeof buf, "%.0f", v);
      }
      else
      {
        for (int prec = 1; prec <= 17; ++prec)
        {
          std::snprintf(buf, sizeof buf, "%.*g", prec, v);
          if (std::strtod(buf, nullptr) == v)
            break;
        }
      }
      o += toJsonNumber(buf);
    }
    // Only numbers/bools are packed 16 per line; strings etc. get one per line.
    bool allPackable() const
    {
      for (const Json &j : items_)
        if (!j.isNumber() && !j.isBool())
          return false;
      return true;
    }
    void write(std::string &o, int depth) const
    {
      switch (type_)
      {
      case Type::Null:
        o += "null";
        break;
      case Type::Bool:
        o += b_ ? "true" : "false";
        break;
      case Type::Number:
        number(o, n_);
        break;
      case Type::String:
        quote(o, s_);
        break;
      case Type::Array:
        if (items_.empty())
        {
          o += "[]";
        }
        else if (allPackable())
        { // numbers/bools: rows of 16 per line
          o += '[';
          for (std::size_t i = 0; i < items_.size(); ++i)
          {
            if (i)
            {
              o += ',';
              if (i % 16 == 0)
              {
                o += '\n';
                o.append(static_cast<std::size_t>(depth + 1) * 2, ' ');
              }
              else
                o += ' ';
            }
            items_[i].write(o, depth + 1);
          }
          o += ']';
        }
        else
        {
          o += "[\n";
          for (std::size_t i = 0; i < items_.size(); ++i)
          {
            o.append(static_cast<std::size_t>(depth + 1) * 2, ' ');
            items_[i].write(o, depth + 1);
            o += i + 1 < items_.size() ? ",\n" : "\n";
          }
          o.append(static_cast<std::size_t>(depth) * 2, ' ');
          o += ']';
        }
        break;
      case Type::Object:
        if (keys_.empty())
        {
          o += "{}";
        }
        else
        {
          o += "{\n";
          for (std::size_t i = 0; i < keys_.size(); ++i)
          {
            o.append(static_cast<std::size_t>(depth + 1) * 2, ' ');
            quote(o, keys_[i]);
            o += ": ";
            vals_[i].write(o, depth + 1);
            o += i + 1 < keys_.size() ? ",\n" : "\n";
          }
          o.append(static_cast<std::size_t>(depth) * 2, ' ');
          o += '}';
        }
        break;
      }
    }

    // ---- parser ----
    struct Parser
    {
      std::string_view s;
      std::size_t i;
      std::string *err;
      static constexpr int kMaxDepth = 64;

      bool fail(const char *m)
      {
        if (err)
        {
          std::size_t line = 1, col = 1;
          const std::size_t end = std::min(i, s.size());
          for (std::size_t k = 0; k < end; ++k)
          {
            if (s[k] == '\n')
            {
              ++line;
              col = 1;
            }
            else
              ++col;
          }
          *err = std::string(m) + " at line " + std::to_string(line) +
                 ", column " + std::to_string(col) + " (byte " +
                 std::to_string(i) + ")";
        }
        return false;
      }
      void ws()
      {
        while (i < s.size() &&
               (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
          ++i;
      }
      bool lit(std::string_view w)
      {
        if (s.substr(i, w.size()) != w)
          return false;
        i += w.size();
        return true;
      }
      bool value(Json &out, int depth)
      {
        if (depth > kMaxDepth)
          return fail("nesting too deep");
        if (i >= s.size())
          return fail("unexpected end");
        const char c = s[i];
        if (c == '{')
          return object(out, depth);
        if (c == '[')
          return arr(out, depth);
        if (c == '"')
        {
          std::string t;
          if (!string(t))
            return false;
          out = Json::str(std::move(t));
          return true;
        }
        if (c == 't' && lit("true"))
        {
          out = Json::flag(true);
          return true;
        }
        if (c == 'f' && lit("false"))
        {
          out = Json::flag(false);
          return true;
        }
        if (c == 'n' && lit("null"))
        {
          out = Json();
          return true;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
          return numeric(out);
        return fail("unexpected character");
      }
      bool numeric(Json &out)
      {
        const std::size_t b = i;
        if (s[i] == '-')
          ++i;
        while (i < s.size() &&
               ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                s[i] == 'E' || s[i] == '+' || s[i] == '-'))
          ++i;
        const std::string t = fromJsonNumber(std::string(s.substr(b, i - b)));
        char *end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (t.empty() || end != t.c_str() + t.size() || !std::isfinite(v))
        {
          i = b; // report the error at the start of the number
          return fail("bad number");
        }
        out = Json::num(v);
        return true;
      }
      static void utf8(std::string &o, unsigned cp)
      {
        if (cp < 0x80)
          o += static_cast<char>(cp);
        else if (cp < 0x800)
        {
          o += static_cast<char>(0xC0 | (cp >> 6));
          o += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
          o += static_cast<char>(0xE0 | (cp >> 12));
          o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          o += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
          o += static_cast<char>(0xF0 | (cp >> 18));
          o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
          o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
          o += static_cast<char>(0x80 | (cp & 0x3F));
        }
      }
      bool hex4(unsigned &v)
      {
        if (i + 4 > s.size())
          return fail("bad \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k)
        {
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
      bool string(std::string &out)
      {
        ++i; // opening quote
        while (i < s.size())
        {
          const char c = s[i++];
          if (c == '"')
          {
            if (!validUtf8(out))
              return fail("invalid UTF-8 in string");
            return true;
          }
          if (static_cast<unsigned char>(c) < 0x20)
            return fail("control character in string");
          if (c != '\\')
          {
            out += c;
            continue;
          }
          if (i >= s.size())
            break;
          const char e = s[i++];
          switch (e)
          {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case '/':
            out += '/';
            break;
          case 'b':
            out += '\b';
            break;
          case 'f':
            out += '\f';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
          {
            unsigned cp;
            if (!hex4(cp))
              return false;
            if (cp >= 0xD800 && cp < 0xDC00)
            { // high surrogate: need its pair
              unsigned lo;
              if (!lit("\\u") || !hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                return fail("bad surrogate pair");
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
              return fail("lone surrogate");
            else if (cp == 0)
              return fail("NUL character in string"); // would truncate c_str()
            utf8(out, cp);
            break;
          }
          default:
            return fail("bad escape");
          }
        }
        return fail("unterminated string");
      }
      bool arr(Json &out, int depth)
      {
        ++i; // [
        out = Json::array();
        ws();
        if (i < s.size() && s[i] == ']')
        {
          ++i;
          return true;
        }
        for (;;)
        {
          Json v;
          ws();
          if (!value(v, depth + 1))
            return false;
          out.push(std::move(v));
          ws();
          if (i < s.size() && s[i] == ',')
          {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == ']')
          {
            ++i;
            return true;
          }
          return fail("expected , or ]");
        }
      }
      bool object(Json &out, int depth)
      {
        ++i; // {
        out = Json::object();
        ws();
        if (i < s.size() && s[i] == '}')
        {
          ++i;
          return true;
        }
        for (;;)
        {
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
          if (i < s.size() && s[i] == ',')
          {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == '}')
          {
            ++i;
            return true;
          }
          return fail("expected , or }");
        }
      }
    };
  };

  // ---- base64 (RFC 4648, padded) ----
  inline std::string base64Encode(const std::vector<std::uint8_t> &d)
  {
    static const char *T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < d.size(); i += 3)
    {
      const std::uint32_t b0 = d[i], b1 = i + 1 < d.size() ? d[i + 1] : 0,
                          b2 = i + 2 < d.size() ? d[i + 2] : 0;
      const std::uint32_t v = (b0 << 16) | (b1 << 8) | b2;
      o += T[(v >> 18) & 63];
      o += T[(v >> 12) & 63];
      o += i + 1 < d.size() ? T[(v >> 6) & 63] : '=';
      o += i + 2 < d.size() ? T[v & 63] : '=';
    }
    return o;
  }
  inline bool base64Decode(std::string_view s, std::vector<std::uint8_t> *out)
  {
    out->clear();
    if (s.size() % 4 != 0)
      return false;
    auto val = [](char c) -> int
    {
      if (c >= 'A' && c <= 'Z')
        return c - 'A';
      if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
      if (c >= '0' && c <= '9')
        return c - '0' + 52;
      if (c == '+')
        return 62;
      if (c == '/')
        return 63;
      return -1;
    };
    out->reserve(s.size() / 4 * 3);
    for (std::size_t i = 0; i < s.size(); i += 4)
    {
      const bool p2 = s[i + 2] == '=', p3 = s[i + 3] == '=';
      if ((p2 && !p3) || ((p2 || p3) && i + 4 != s.size()))
      {
        out->clear();
        return false;
      }
      const int a = val(s[i]), b = val(s[i + 1]);
      const int c = p2 ? 0 : val(s[i + 2]), d = p3 ? 0 : val(s[i + 3]);
      if (a < 0 || b < 0 || c < 0 || d < 0)
      {
        out->clear();
        return false;
      }
      const std::uint32_t v = (static_cast<std::uint32_t>(a) << 18) |
                              (static_cast<std::uint32_t>(b) << 12) |
                              (static_cast<std::uint32_t>(c) << 6) |
                              static_cast<std::uint32_t>(d);
      out->push_back(static_cast<std::uint8_t>(v >> 16));
      if (!p2)
        out->push_back(static_cast<std::uint8_t>(v >> 8));
      if (!p3)
        out->push_back(static_cast<std::uint8_t>(v));
    }
    return true;
  }

  // ---- byte run-length coding: (count 1..255, value) pairs ----
  // Worst case (no repeated bytes) doubles the size; callers storing large
  // buffers may want to compare sizes and keep the raw bytes when RLE loses.
  inline std::vector<std::uint8_t> rleEncode(const std::vector<std::uint8_t> &in)
  {
    std::vector<std::uint8_t> o;
    for (std::size_t i = 0; i < in.size();)
    {
      std::size_t n = 1;
      while (i + n < in.size() && in[i + n] == in[i] && n < 255)
        ++n;
      o.push_back(static_cast<std::uint8_t>(n));
      o.push_back(in[i]);
      i += n;
    }
    return o;
  }
  // False unless the runs add up to exactly `expected` bytes. `out` is empty on
  // failure. Safe on untrusted input: never allocates more than the stream can
  // actually produce, whatever `expected` claims.
  inline bool rleDecode(const std::vector<std::uint8_t> &in, std::size_t expected,
                        std::vector<std::uint8_t> *out)
  {
    out->clear();
    if (in.size() % 2 != 0)
      return false;

    // Each (count, value) pair yields at most 255 bytes.
    const std::size_t maxProducible = in.size() / 2 * 255;
    if (expected > maxProducible)
      return false;

    out->reserve(expected);
    for (std::size_t i = 0; i < in.size(); i += 2)
    {
      const std::size_t n = in[i];
      if (n == 0 || n > expected - out->size())
      {
        out->clear();
        return false;
      }
      out->resize(out->size() + n, in[i + 1]);
    }
    if (out->size() != expected)
    {
      out->clear();
      return false;
    }
    return true;
  }

} // namespace mj