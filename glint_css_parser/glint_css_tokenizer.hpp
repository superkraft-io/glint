#pragma once

/**
 * glint_css_tokenizer.hpp
 * CSS Syntax Module Level 3 §4.3 — streaming tokenizer.
 *
 * GlintCssTokenizer takes a UTF-8 CSS source string (as std::string_view)
 * and provides a linear stream of GlintCssTokens via next() / peek().
 *
 * Deviation from spec: the input is treated as a pre-decoded code-point
 * sequence (UTF-8 bytes).  Surrogate pairs and BOM handling is skipped
 * because the standalone pipeline uses UTF-8 / UTF-32 throughout.
 *
 * Reference: https://www.w3.org/TR/css-syntax-3/#tokenization
 */

#include "glint_css_token.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// ── GlintCssTokenizer ──────────────────────────────────────────────────────────
class GlintCssTokenizer
{
public:
  explicit GlintCssTokenizer(std::string_view src)
    : mSrc(src), mPos(0), mLine(1), mCol(1)
  {}

  // Tokenize the entire source into a vector (convenience wrapper).
  static std::vector<GlintCssToken> tokenize(std::string_view src)
  {
    GlintCssTokenizer tz(src);
    std::vector<GlintCssToken> out;
    for (;;)
    {
      GlintCssToken t = tz.next();
      out.push_back(t);
      if (t.type == GlintCssTokenType::EOF_TOKEN) break;
    }
    return out;
  }

  // ── Streaming API ─────────────────────────────────────────────────────────

  // Consume and return the next token.
  GlintCssToken next()
  {
    // ── §4.3.2 — CSS comments ─────────────────────────────────────────────────
    // Emit COMMENT tokens (value = body text between /* and */) so the
    // declaration-list parser can recognise commented-out declarations and
    // surface them as disabled rows in the DevTools inspector.
    if (!eof() && current() == '/' && peek(1) == '*')
    {
      const uint32_t startLine = mLine;
      const uint32_t startCol  = mCol;
      advance(); advance(); // consume '/*'
      std::string body;
      while (!eof())
      {
        if (current() == '*' && peek(1) == '/')
          { advance(); advance(); break; } // consume '*/' and stop
        body += current();
        advance();
      }
      return makeToken(GlintCssTokenType::COMMENT, std::move(body), 0.0, startLine, startCol);
    }
    consumeComments(); // safety: skip any remaining /* */ that weren't caught above
    if (eof()) return makeToken(GlintCssTokenType::EOF_TOKEN);

    const uint32_t startLine = mLine;
    const uint32_t startCol  = mCol;
    const char     c         = current();

    // Whitespace
    if (isWhitespace(c))
    {
      while (!eof() && isWhitespace(current())) advance();
      return makeToken(GlintCssTokenType::WHITESPACE, " ", 0.0, startLine, startCol);
    }

    // String " or '
    if (c == '"' || c == '\'')
    {
      advance(); // consume opening quote
      return consumeString(c, startLine, startCol);
    }

    // # — HASH
    if (c == '#')
    {
      advance();
      if (!eof() && (isNameChar(current()) || isEscape()))
      {
        GlintCssHashFlag flag = GlintCssHashFlag::UNRESTRICTED;
        if (wouldStartIdent()) flag = GlintCssHashFlag::ID;
        std::string name = consumeName();
        GlintCssToken tok = makeToken(GlintCssTokenType::HASH, name, 0.0, startLine, startCol);
        tok.hashFlag = flag;
        return tok;
      }
      return makeToken(GlintCssTokenType::DELIM, "#", 0.0, startLine, startCol);
    }

    // + — NUMBER or DELIM
    if (c == '+')
    {
      if (wouldStartNumber())
        return consumeNumericToken(startLine, startCol);
      advance();
      return makeToken(GlintCssTokenType::DELIM, "+", 0.0, startLine, startCol);
    }

    // , COMMA
    if (c == ',')
    {
      advance();
      return makeToken(GlintCssTokenType::COMMA, ",", 0.0, startLine, startCol);
    }

    // - — NUMBER, CDC, or DELIM
    if (c == '-')
    {
      if (wouldStartNumber())
        return consumeNumericToken(startLine, startCol);
      if (peek(1) == '-' && peek(2) == '>')
      {
        advance(); advance(); advance();
        return makeToken(GlintCssTokenType::CDC, "-->", 0.0, startLine, startCol);
      }
      if (wouldStartIdent())
        return consumeIdentLikeToken(startLine, startCol);
      advance();
      return makeToken(GlintCssTokenType::DELIM, "-", 0.0, startLine, startCol);
    }

    // . — NUMBER or DELIM
    if (c == '.')
    {
      if (wouldStartNumber())
        return consumeNumericToken(startLine, startCol);
      advance();
      return makeToken(GlintCssTokenType::DELIM, ".", 0.0, startLine, startCol);
    }

    // : ; ( ) [ ] { }
    if (c == ':')  { advance(); return makeToken(GlintCssTokenType::COLON,       ":", 0.0, startLine, startCol); }
    if (c == ';')  { advance(); return makeToken(GlintCssTokenType::SEMICOLON,   ";", 0.0, startLine, startCol); }
    if (c == '(')  { advance(); return makeToken(GlintCssTokenType::OPEN_PAREN,  "(", 0.0, startLine, startCol); }
    if (c == ')')  { advance(); return makeToken(GlintCssTokenType::CLOSE_PAREN, ")", 0.0, startLine, startCol); }
    if (c == '[')  { advance(); return makeToken(GlintCssTokenType::OPEN_SQUARE,  "[", 0.0, startLine, startCol); }
    if (c == ']')  { advance(); return makeToken(GlintCssTokenType::CLOSE_SQUARE, "]", 0.0, startLine, startCol); }
    if (c == '{')  { advance(); return makeToken(GlintCssTokenType::OPEN_CURLY,  "{", 0.0, startLine, startCol); }
    if (c == '}')  { advance(); return makeToken(GlintCssTokenType::CLOSE_CURLY, "}", 0.0, startLine, startCol); }

    // < — CDO or DELIM
    if (c == '<')
    {
      if (peek(1) == '!' && peek(2) == '-' && peek(3) == '-')
      {
        advance(); advance(); advance(); advance();
        return makeToken(GlintCssTokenType::CDO, "<!--", 0.0, startLine, startCol);
      }
      advance();
      return makeToken(GlintCssTokenType::DELIM, "<", 0.0, startLine, startCol);
    }

    // @ — AT_KEYWORD or DELIM
    if (c == '@')
    {
      advance();
      if (!eof() && wouldStartIdentAtCurrent())
      {
        std::string name = consumeName();
        return makeToken(GlintCssTokenType::AT_KEYWORD, name, 0.0, startLine, startCol);
      }
      return makeToken(GlintCssTokenType::DELIM, "@", 0.0, startLine, startCol);
    }

    // \ — escape or DELIM
    if (c == '\\')
    {
      if (isEscape())
      {
        // Treat like start of ident
        return consumeIdentLikeToken(startLine, startCol);
      }
      advance();
      return makeToken(GlintCssTokenType::DELIM, "\\", 0.0, startLine, startCol);
    }

    // Digit → numeric token
    if (isDigit(c))
      return consumeNumericToken(startLine, startCol);

    // Name-start → ident-like
    if (isNameStart(c))
      return consumeIdentLikeToken(startLine, startCol);

    // Anything else → DELIM
    std::string d(1, c);
    advance();
    return makeToken(GlintCssTokenType::DELIM, d, 0.0, startLine, startCol);
  }

  // Peek at the next token without consuming.
  GlintCssToken peek()
  {
    const size_t savedPos  = mPos;
    const uint32_t savedLn  = mLine;
    const uint32_t savedCol = mCol;
    GlintCssToken tok = next();
    mPos  = savedPos;
    mLine = savedLn;
    mCol  = savedCol;
    return tok;
  }

  bool atEnd() const { return mPos >= mSrc.size(); }

private:
  // ── Source navigation ─────────────────────────────────────────────────────

  std::string_view mSrc;
  size_t           mPos;
  uint32_t         mLine;
  uint32_t         mCol;

  bool eof() const { return mPos >= mSrc.size(); }

  char current() const
  {
    return mPos < mSrc.size() ? mSrc[mPos] : '\0';
  }

  char peek(size_t offset) const
  {
    const size_t idx = mPos + offset;
    return idx < mSrc.size() ? mSrc[idx] : '\0';
  }

  void advance()
  {
    if (eof()) return;
    if (mSrc[mPos] == '\n') { ++mLine; mCol = 1; }
    else                    { ++mCol; }
    ++mPos;
  }

  // ── Character class predicates (§4.2) ────────────────────────────────────

  static bool isWhitespace(char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
  }

  static bool isDigit(char c)  { return c >= '0' && c <= '9'; }
  static bool isHexDigit(char c)
  {
    return isDigit(c)
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
  }

  // §4.2 — name-start code point
  static bool isNameStart(char c)
  {
    // The spec defines name-start as letter | non-ASCII | '_'
    // We accept any byte ≥ 0x80 as a potential multi-byte UTF-8 continuation.
    return std::isalpha(static_cast<unsigned char>(c))
        || c == '_'
        || static_cast<unsigned char>(c) >= 0x80;
  }

  // §4.2 — name code point (name-start | digit | '-')
  static bool isNameChar(char c)
  {
    return isNameStart(c) || isDigit(c) || c == '-';
  }

  // §4.3.9 — valid escape (current='\', next ≠ newline)
  bool isEscape() const
  {
    return current() == '\\' && peek(1) != '\n' && peek(1) != '\0';
  }

  // §4.3.10 — would the current pos start an identifier?
  bool wouldStartIdent() const
  {
    const char c0 = current();
    const char c1 = peek(1);
    const char c2 = peek(2);
    if (c0 == '-') return c1 == '-' || isNameStart(c1) || (c1 == '\\' && c2 != '\n');
    if (c0 == '\\') return current() != '\n';
    return isNameStart(c0);
  }

  // Same check but from the *current* position (after a leading '@' was already consumed)
  bool wouldStartIdentAtCurrent() const { return wouldStartIdent(); }

  // §4.3.10 — would the current pos start a number?
  bool wouldStartNumber() const
  {
    const char c0 = current();
    const char c1 = peek(1);
    const char c2 = peek(2);
    if (c0 == '+' || c0 == '-')
      return isDigit(c1) || (c1 == '.' && isDigit(c2));
    if (c0 == '.')
      return isDigit(c1);
    return isDigit(c0);
  }

  // ── Token factories ──────────────────────────────────────────────────────

  GlintCssToken makeToken(GlintCssTokenType type,
                         std::string value  = {},
                         double num         = 0.0,
                         uint32_t ln        = 0,
                         uint32_t col       = 0) const
  {
    GlintCssToken tok;
    tok.type         = type;
    tok.value        = std::move(value);
    tok.numericValue = num;
    tok.line         = ln  ? ln  : mLine;
    tok.column       = col ? col : mCol;
    return tok;
  }

  // ── §4.3.2 — consume comments ────────────────────────────────────────────
  void consumeComments()
  {
    while (!eof() && current() == '/' && peek(1) == '*')
    {
      advance(); advance(); // consume '/*'
      while (!eof())
      {
        if (current() == '*' && peek(1) == '/')
        {
          advance(); advance(); // consume '*/'
          break;
        }
        advance();
      }
    }
  }

  // ── §4.3.7 — consume an escaped code point ───────────────────────────────
  // Returns the resulting character(s) appended into `out`.
  void consumeEscapedCodePoint(std::string& out)
  {
    // current() is '\' — consume it
    advance();
    if (eof()) { out += "\xEF\xBF\xBD"; return; }

    if (isHexDigit(current()))
    {
      // Consume up to 6 hex digits
      std::string hex;
      for (int i = 0; i < 6 && !eof() && isHexDigit(current()); ++i)
      {
        hex += current(); advance();
      }
      // Consume optional following whitespace
      if (!eof() && isWhitespace(current())) advance();

      unsigned long cp = std::stoul(hex, nullptr, 16);
      if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;

      // Encode cp as UTF-8
      if      (cp <= 0x7F)   { out += static_cast<char>(cp); }
      else if (cp <= 0x7FF)  { out += static_cast<char>(0xC0 | (cp >> 6));
                               out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else if (cp <= 0xFFFF) { out += static_cast<char>(0xE0 | (cp >> 12));
                               out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                               out += static_cast<char>(0x80 | (cp & 0x3F)); }
      else                   { out += static_cast<char>(0xF0 | (cp >> 18));
                               out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                               out += static_cast<char>(0x80 | ((cp >>  6) & 0x3F));
                               out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    else
    {
      out += current();
      advance();
    }
  }

  // ── §4.3.11 — consume a name ──────────────────────────────────────────────
  std::string consumeName()
  {
    std::string result;
    while (!eof())
    {
      const char c = current();
      if (isNameChar(c))          { result += c; advance(); }
      else if (c == '\\' && isEscape())  { consumeEscapedCodePoint(result); }
      else break;
    }
    return result;
  }

  // ── §4.3.12 — consume a number ───────────────────────────────────────────
  // Returns the numeric value; fills out repr
  double consumeNumber(std::string& repr, GlintCssNumericType& numType)
  {
    numType = GlintCssNumericType::INTEGER;

    // Optional sign
    if (!eof() && (current() == '+' || current() == '-'))
    {
      repr += current(); advance();
    }

    // Integer part
    while (!eof() && isDigit(current()))
    {
      repr += current(); advance();
    }

    // Decimal part
    if (!eof() && current() == '.' && isDigit(peek(1)))
    {
      numType = GlintCssNumericType::NUMBER;
      repr += '.'; advance();
      while (!eof() && isDigit(current()))
      {
        repr += current(); advance();
      }
    }

    // Exponent part
    if (!eof() && (current() == 'e' || current() == 'E'))
    {
      const char next1 = peek(1);
      const char next2 = peek(2);
      if (isDigit(next1) || ((next1 == '+' || next1 == '-') && isDigit(next2)))
      {
        numType = GlintCssNumericType::NUMBER;
        repr += current(); advance();
        if (current() == '+' || current() == '-') { repr += current(); advance(); }
        while (!eof() && isDigit(current())) { repr += current(); advance(); }
      }
    }

    try { return std::stod(repr); } catch (...) { return 0.0; }
  }

  // ── §4.3.3 — consume a numeric token ─────────────────────────────────────
  GlintCssToken consumeNumericToken(uint32_t startLine, uint32_t startCol)
  {
    std::string repr;
    GlintCssNumericType numType;
    const double num = consumeNumber(repr, numType);

    if (wouldStartIdent())
    {
      // DIMENSION
      std::string unit = consumeName();
      GlintCssToken tok = makeToken(GlintCssTokenType::DIMENSION, unit, num, startLine, startCol);
      tok.numericType = numType;
      return tok;
    }
    if (!eof() && current() == '%')
    {
      advance();
      GlintCssToken tok = makeToken(GlintCssTokenType::PERCENTAGE, "%", num, startLine, startCol);
      tok.numericType = numType;
      return tok;
    }
    GlintCssToken tok = makeToken(GlintCssTokenType::NUMBER, repr, num, startLine, startCol);
    tok.numericType = numType;
    return tok;
  }

  // ── §4.3.5 — consume a string token ──────────────────────────────────────
  // endChar is the opening quote character ('" or '\'').
  GlintCssToken consumeString(char endChar, uint32_t startLine, uint32_t startCol)
  {
    std::string result;
    while (!eof())
    {
      const char c = current();
      if (c == endChar) { advance(); break; }
      if (c == '\n')
      {
        // Bad string — return bad-string; do not consume newline
        return makeToken(GlintCssTokenType::BAD_STRING, result, 0.0, startLine, startCol);
      }
      if (c == '\\')
      {
        const char next = peek(1);
        if (next == '\0') break; // EOF after '\'
        if (next == '\n') { advance(); advance(); continue; } // escaped newline → ignored
        consumeEscapedCodePoint(result);
      }
      else { result += c; advance(); }
    }
    return makeToken(GlintCssTokenType::STRING, result, 0.0, startLine, startCol);
  }

  // ── §4.3.6 — consume a url token (after "url(" has been consumed) ─────────
  GlintCssToken consumeUrlToken(uint32_t startLine, uint32_t startCol)
  {
    // Skip leading whitespace
    while (!eof() && isWhitespace(current())) advance();

    std::string url;
    while (!eof())
    {
      const char c = current();
      if (c == ')')              { advance(); break; }
      if (isWhitespace(c))
      {
        while (!eof() && isWhitespace(current())) advance();
        if (eof() || current() == ')') { advance(); break; }
        // Non-) after whitespace → bad url
        consumeBadUrl();
        return makeToken(GlintCssTokenType::BAD_URL, url, 0.0, startLine, startCol);
      }
      if (c == '"' || c == '\'' || c == '(')
      {
        consumeBadUrl();
        return makeToken(GlintCssTokenType::BAD_URL, url, 0.0, startLine, startCol);
      }
      if (c == '\\')
      {
        if (isEscape()) { consumeEscapedCodePoint(url); }
        else
        {
          consumeBadUrl();
          return makeToken(GlintCssTokenType::BAD_URL, url, 0.0, startLine, startCol);
        }
      }
      else { url += c; advance(); }
    }
    return makeToken(GlintCssTokenType::URL, url, 0.0, startLine, startCol);
  }

  void consumeBadUrl()
  {
    while (!eof())
    {
      const char c = current();
      if (c == ')') { advance(); return; }
      if (c == '\\' && isEscape()) { std::string dummy; consumeEscapedCodePoint(dummy); }
      else advance();
    }
  }

  // ── §4.3.4 — consume an ident-like token ─────────────────────────────────
  GlintCssToken consumeIdentLikeToken(uint32_t startLine, uint32_t startCol)
  {
    std::string name = consumeName();

    if (!eof() && current() == '(')
    {
      advance(); // consume '('

      // url() is special — produces URL not FUNCTION
      std::string nameLow = name;
      for (char& ch : nameLow) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (nameLow == "url")
      {
        // Peek past whitespace to check for a quoted string
        size_t tmp = mPos;
        while (tmp < mSrc.size() && isWhitespace(mSrc[tmp])) ++tmp;
        if (tmp < mSrc.size() && (mSrc[tmp] == '"' || mSrc[tmp] == '\''))
        {
          // url("...") → FUNCTION token, not URL token
          return makeToken(GlintCssTokenType::FUNCTION, name, 0.0, startLine, startCol);
        }
        return consumeUrlToken(startLine, startCol);
      }
      return makeToken(GlintCssTokenType::FUNCTION, name, 0.0, startLine, startCol);
    }
    return makeToken(GlintCssTokenType::IDENT, name, 0.0, startLine, startCol);
  }
};
