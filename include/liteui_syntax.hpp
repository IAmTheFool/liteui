// liteui_syntax.hpp

// This is a single generic, config-driven tokenizer (keywords/strings/
// comments/numbers/preprocessor lines) rather than a bespoke lexer per
// language — LanguageDef below just parameterizes it (comment syntax,
// string quote char, keyword set). It's not a real grammar engine (no
// TextMate-style nested scopes, no per-language edge cases like Python's
// triple-quoted strings or C++'s raw string literals) but it's a genuine
// tokenizer producing genuine colored spans, and it retokenizes
// incrementally rather than re-scanning the whole file on every keystroke
// — see SyntaxHighlighter::sync() for how.
//
// Requires liteui_editor_ext.hpp to be included first (for HighlightSpan
// and Color).

#pragma once

#include "liteui_editor_ext.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

enum class SyntaxTokenType {
  Plain,
  Keyword,
  Comment,
  String,
  Number,
  Preprocessor
};

struct SyntaxToken {
  size_t start = 0, length = 0;
  SyntaxTokenType kind = SyntaxTokenType::Plain;
};

// Parameterizes the generic tokenizer below for one language. Not a real
// grammar — just enough knobs (keyword set, comment syntax, string quote)
// to make the same scanning loop work across a handful of C-like and
// scripting languages.
struct LanguageDef {
  std::string name;
  std::vector<std::string> extensions; // e.g. {".cpp", ".hpp"}, dot included
  std::unordered_set<std::string> keywords;
  std::string lineComment;       // e.g. "//" or "#"; empty = none
  std::string blockCommentStart; // e.g. "/*"; empty = no block comments
  std::string blockCommentEnd;   // e.g. "*/"
  bool hasStrings = true;
  char stringQuote = '"';
  bool hasChars = false; // 'x' style char literals, distinct from strings
  char charQuote = '\'';
  bool hasHashPreprocessor = false; // a line whose first non-space char is
                                    // '#' is colored as one Preprocessor
                                    // span for its whole remainder
};

inline bool syntaxIsIdentStart(unsigned char c) {
  return std::isalpha(c) || c == '_' || c >= 0x80; // >=0x80: UTF-8
                                                   // lead/continuation
                                                   // bytes count as
                                                   // identifier bytes so
                                                   // multi-byte identifiers
                                                   // don't get fragmented
}
inline bool syntaxIsIdentCont(unsigned char c) {
  return std::isalnum(c) || c == '_' || c >= 0x80;
}

// Tokenizes one line. `startInBlockComment` is the state carried in from
// the previous line (see SyntaxHighlighter for how that's threaded
// through a whole file); `endInBlockComment` is set to whatever state
// should carry into the *next* line.
//
// Known simplifications single-char string/char delimiters only
// (no Python triple-quotes, no C++ raw strings); a whole "starts with #"
// line is one Preprocessor span rather than parsing includes/macros
// specially; no template/generic-aware bracket matching.
inline std::vector<SyntaxToken> tokenizeLine(const std::string &line,
                                             const LanguageDef &lang,
                                             bool startInBlockComment,
                                             bool &endInBlockComment) {
  std::vector<SyntaxToken> tokens;
  size_t i = 0, n = line.size();
  bool inBlockComment = startInBlockComment;
  auto push = [&](size_t start, size_t len, SyntaxTokenType t) {
    if (len > 0)
      tokens.push_back(SyntaxToken{start, len, t});
  };

  if (inBlockComment) {
    size_t end = lang.blockCommentEnd.empty()
                     ? std::string::npos
                     : line.find(lang.blockCommentEnd, 0);
    if (end == std::string::npos) {
      push(0, n, SyntaxTokenType::Comment);
      endInBlockComment = true;
      return tokens;
    }
    push(0, end + lang.blockCommentEnd.size(), SyntaxTokenType::Comment);
    i = end + lang.blockCommentEnd.size();
    inBlockComment = false;
  }

  if (lang.hasHashPreprocessor) {
    size_t j = i;
    while (j < n && (line[j] == ' ' || line[j] == '\t'))
      ++j;
    if (j < n && line[j] == '#') {
      push(i, n - i, SyntaxTokenType::Preprocessor);
      endInBlockComment = false;
      return tokens;
    }
  }

  while (i < n) {
    unsigned char c = static_cast<unsigned char>(line[i]);

    if (!lang.lineComment.empty() &&
        line.compare(i, lang.lineComment.size(), lang.lineComment) == 0) {
      push(i, n - i, SyntaxTokenType::Comment);
      i = n;
      break;
    }
    if (!lang.blockCommentStart.empty() &&
        line.compare(i, lang.blockCommentStart.size(),
                     lang.blockCommentStart) == 0) {
      size_t end =
          line.find(lang.blockCommentEnd, i + lang.blockCommentStart.size());
      if (end == std::string::npos) {
        push(i, n - i, SyntaxTokenType::Comment);
        inBlockComment = true;
        i = n;
        break;
      }
      push(i, end + lang.blockCommentEnd.size() - i, SyntaxTokenType::Comment);
      i = end + lang.blockCommentEnd.size();
      continue;
    }
    if (lang.hasStrings && c == static_cast<unsigned char>(lang.stringQuote)) {
      size_t start = i++;
      while (i < n) {
        if (line[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        bool closing = static_cast<unsigned char>(line[i]) ==
                       static_cast<unsigned char>(lang.stringQuote);
        ++i;
        if (closing)
          break;
      }
      push(start, i - start, SyntaxTokenType::String);
      continue;
    }
    if (lang.hasChars && c == static_cast<unsigned char>(lang.charQuote)) {
      size_t start = i++;
      while (i < n) {
        if (line[i] == '\\' && i + 1 < n) {
          i += 2;
          continue;
        }
        bool closing = static_cast<unsigned char>(line[i]) ==
                       static_cast<unsigned char>(lang.charQuote);
        ++i;
        if (closing)
          break;
      }
      push(start, i - start, SyntaxTokenType::String);
      continue;
    }
    if (std::isdigit(c)) {
      size_t start = i++;
      while (i < n && (std::isalnum(static_cast<unsigned char>(line[i])) ||
                       line[i] == '.' || line[i] == '_'))
        ++i; // crude but covers hex/float/suffix cases well enough to color
             // a number as a number
      push(start, i - start, SyntaxTokenType::Number);
      continue;
    }
    if (syntaxIsIdentStart(c)) {
      size_t start = i++;
      while (i < n && syntaxIsIdentCont(static_cast<unsigned char>(line[i])))
        ++i;
      std::string word = line.substr(start, i - start);
      push(start, i - start,
           lang.keywords.count(word) ? SyntaxTokenType::Keyword
                                     : SyntaxTokenType::Plain);
      continue;
    }
    // Punctuation/operators/whitespace: coalesce a run of "everything
    // else" bytes into one Plain span so we're not pushing one token per
    // character.
    size_t start = i++;
    while (i < n) {
      unsigned char cc = static_cast<unsigned char>(line[i]);
      if (syntaxIsIdentStart(cc) || std::isdigit(cc))
        break;
      if (lang.hasStrings && cc == static_cast<unsigned char>(lang.stringQuote))
        break;
      if (lang.hasChars && cc == static_cast<unsigned char>(lang.charQuote))
        break;
      if (!lang.lineComment.empty() &&
          line.compare(i, lang.lineComment.size(), lang.lineComment) == 0)
        break;
      if (!lang.blockCommentStart.empty() &&
          line.compare(i, lang.blockCommentStart.size(),
                       lang.blockCommentStart) == 0)
        break;
      ++i;
    }
    push(start, i - start, SyntaxTokenType::Plain);
  }

  endInBlockComment = inBlockComment;
  return tokens;
}

struct SyntaxTheme {
  Color plainColor = Color{212, 212, 212};
  Color keywordColor = Color{86, 156, 214};
  Color commentColor = Color{106, 153, 85};
  Color stringColor = Color{206, 145, 120};
  Color numberColor = Color{181, 206, 168};
  Color preprocessorColor = Color{197, 134, 192};
};

inline Color colorForToken(SyntaxTokenType t, const SyntaxTheme &theme) {
  switch (t) {
  case SyntaxTokenType::Keyword:
    return theme.keywordColor;
  case SyntaxTokenType::Comment:
    return theme.commentColor;
  case SyntaxTokenType::String:
    return theme.stringColor;
  case SyntaxTokenType::Number:
    return theme.numberColor;
  case SyntaxTokenType::Preprocessor:
    return theme.preprocessorColor;
  case SyntaxTokenType::Plain:
  default:
    return theme.plainColor;
  }
}

struct LineHighlight {
  bool valid = false;      // false means "never tokenized" or "explicitly
                           // invalidated" — distinct from an empty-but-valid
                           // token list for a blank line
  std::string sourceText;  // the line text this was computed from
  bool startState = false; // block-comment state entering this line
  bool endState = false;   // block-comment state leaving this line
  std::vector<SyntaxToken> tokens;
};

// Holds one document's per-line token cache and keeps it in sync with the
// buffer incrementally: sync() only retokenizes lines whose own text
// changed, or whose *incoming* block-comment state changed as a
// consequence of an earlier line's edit (e.g. typing "/*" partway through
// a file needs to recolor everything after it as a comment, until a
// matching "*/" is found) — every other line is a cheap string-equality
// cache hit.
//
// On a structural edit (a line inserted/removed/split/merged — Enter,
// Backspace/Delete across a line boundary, or a multi-line paste), the
// cache is realigned to the new line count by trimming the longest
// matching prefix and suffix against the old cache first, so only the
// (usually small) region actually touched needs recomputation rather than
// the whole file.
struct SyntaxHighlighter {
  std::shared_ptr<LanguageDef> lang;
  SyntaxTheme theme;
  std::vector<LineHighlight> cache;

  void sync(const std::vector<std::string> &lines) {
    if (!lang) {
      cache.clear();
      return;
    }
    if (cache.size() != lines.size())
      realign(lines);

    bool state = false; // block-comment state entering line 0
    for (size_t i = 0; i < lines.size(); ++i) {
      LineHighlight &lc = cache[i];
      if (lc.valid && lc.sourceText == lines[i] && lc.startState == state) {
        state = lc.endState;
        continue;
      }
      bool endState = false;
      lc.tokens = tokenizeLine(lines[i], *lang, state, endState);
      lc.sourceText = lines[i];
      lc.startState = state;
      lc.endState = endState;
      lc.valid = true;
      state = endState;
    }
  }

  // Converts cache[lineIndex]'s tokens into colored spans. Assumes sync()
  // has already been called for the current buffer this paint — see
  // CodeEditor::beforePaintSync/highlightLine in liteui_editor_ext.hpp,
  // which is exactly what calls these two in the right order.
  void spansFor(size_t lineIndex, std::vector<HighlightSpan> &out) const {
    out.clear();
    if (!lang || lineIndex >= cache.size())
      return;
    for (const SyntaxToken &tok : cache[lineIndex].tokens)
      out.push_back({tok.start, tok.length, colorForToken(tok.kind, theme)});
  }

private:
  void realign(const std::vector<std::string> &lines) {
    size_t oldN = cache.size(), newN = lines.size();
    size_t prefix = 0;
    while (prefix < oldN && prefix < newN && cache[prefix].valid &&
           cache[prefix].sourceText == lines[prefix])
      ++prefix;
    size_t maxSuffix = std::min(oldN - prefix, newN - prefix);
    size_t suffix = 0;
    while (suffix < maxSuffix && cache[oldN - 1 - suffix].valid &&
           cache[oldN - 1 - suffix].sourceText == lines[newN - 1 - suffix])
      ++suffix;

    std::vector<LineHighlight> rebuilt(newN);
    for (size_t i = 0; i < prefix; ++i)
      rebuilt[i] = cache[i];
    for (size_t i = 0; i < suffix; ++i)
      rebuilt[newN - 1 - i] = cache[oldN - 1 - i];
    // Everything strictly between prefix and (newN - suffix) is left
    // default-constructed (valid = false) and will be picked up by the
    // main sync() loop above, along with anything after it whose
    // effective incoming state turns out to have changed.
    cache = std::move(rebuilt);
  }
};

inline std::shared_ptr<SyntaxHighlighter>
makeHighlighter(std::shared_ptr<LanguageDef> lang, SyntaxTheme theme = {}) {
  auto h = std::make_shared<SyntaxHighlighter>();
  h->lang = std::move(lang);
  h->theme = theme;
  return h;
}

// Wires an existing SyntaxHighlighter into a CodeEditor's paint hooks.
// No-ops (leaves highlighting off) if the highlighter has no language set.
inline void applyHighlighting(CodeEditor &ed,
                              std::shared_ptr<SyntaxHighlighter> highlighter) {
  if (!highlighter || !highlighter->lang)
    return;
  ed.beforePaintSync = [highlighter](const std::vector<std::string> &lines) {
    highlighter->sync(lines);
  };
  ed.highlightLine = [highlighter](size_t i, const std::string &,
                                   std::vector<HighlightSpan> &out) {
    highlighter->spansFor(i, out);
  };
}

// ==================== built-in language definitions ====================

inline std::shared_ptr<LanguageDef> languageCpp() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "C++";
  l->extensions = {".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".hxx"};
  l->lineComment = "//";
  l->blockCommentStart = "/*";
  l->blockCommentEnd = "*/";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = true;
  l->charQuote = '\'';
  l->hasHashPreprocessor = true;
  l->keywords = {
      "alignas",       "alignof",     "and",
      "and_eq",        "asm",         "auto",
      "bitand",        "bitor",       "bool",
      "break",         "case",        "catch",
      "char",          "char8_t",     "char16_t",
      "char32_t",      "class",       "compl",
      "concept",       "const",       "consteval",
      "constexpr",     "constinit",   "const_cast",
      "continue",      "co_await",    "co_return",
      "co_yield",      "decltype",    "default",
      "delete",        "do",          "double",
      "dynamic_cast",  "else",        "enum",
      "explicit",      "export",      "extern",
      "false",         "float",       "for",
      "friend",        "goto",        "if",
      "inline",        "int",         "long",
      "mutable",       "namespace",   "new",
      "noexcept",      "not",         "not_eq",
      "nullptr",       "operator",    "or",
      "or_eq",         "private",     "protected",
      "public",        "register",    "reinterpret_cast",
      "requires",      "return",      "short",
      "signed",        "sizeof",      "static",
      "static_assert", "static_cast", "struct",
      "switch",        "template",    "this",
      "thread_local",  "throw",       "true",
      "try",           "typedef",     "typeid",
      "typename",      "union",       "unsigned",
      "using",         "virtual",     "void",
      "volatile",      "wchar_t",     "while",
      "xor",           "xor_eq",      "override",
      "final",
  };
  return l;
}

inline std::shared_ptr<LanguageDef> languagePython() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "Python";
  l->extensions = {".py", ".pyw"};
  l->lineComment = "#";
  l->blockCommentStart = ""; // no block comments; triple-quoted strings
                             // spanning lines aren't modeled (see file
                             // header's known-simplifications note)
  l->blockCommentEnd = "";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = false;
  l->hasHashPreprocessor = false;
  l->keywords = {
      "False",  "None",   "True",    "and",      "as",       "assert", "async",
      "await",  "break",  "class",   "continue", "def",      "del",    "elif",
      "else",   "except", "finally", "for",      "from",     "global", "if",
      "import", "in",     "is",      "lambda",   "nonlocal", "not",    "or",
      "pass",   "raise",  "return",  "try",      "while",    "with",   "yield",
  };
  return l;
}

inline std::shared_ptr<LanguageDef> languageJavaScript() {
  auto l = std::make_shared<LanguageDef>();
  l->name = "JavaScript";
  l->extensions = {".js", ".jsx", ".mjs", ".ts", ".tsx"};
  l->lineComment = "//";
  l->blockCommentStart = "/*";
  l->blockCommentEnd = "*/";
  l->hasStrings = true;
  l->stringQuote = '"';
  l->hasChars = false;
  l->hasHashPreprocessor = false;
  l->keywords = {
      "break",     "case",     "catch",   "class",      "const",
      "continue",  "debugger", "default", "delete",     "do",
      "else",      "export",   "extends", "finally",    "for",
      "function",  "if",       "import",  "in",         "instanceof",
      "new",       "return",   "super",   "switch",     "this",
      "throw",     "try",      "typeof",  "var",        "let",
      "void",      "while",    "with",    "yield",      "async",
      "await",     "static",   "get",     "set",        "of",
      "interface", "type",     "enum",    "implements", "namespace",
      "as",        "from",
  };
  return l;
}

inline std::shared_ptr<LanguageDef>
languageForExtension(const std::string &path) {
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos)
    return nullptr;
  std::string ext = path.substr(dot);
  for (auto &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  static const std::vector<std::shared_ptr<LanguageDef>> kLanguages = {
      languageCpp(),
      languagePython(),
      languageJavaScript(),
  };
  for (const auto &lang : kLanguages)
    for (const auto &e : lang->extensions)
      if (e == ext)
        return lang;
  return nullptr;
}