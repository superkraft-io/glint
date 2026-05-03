#pragma once

/**
 * glint_css_token.hpp
 * CSS Syntax Module Level 3 §4 — token types and token struct.
 *
 * Token types match the spec exactly.  The GlintCssToken carries enough
 * information for a one-pass parser: the type enum, the raw string value,
 * and type-specific numeric/flag fields.
 *
 * Reference: https://www.w3.org/TR/css-syntax-3/#tokenization
 */

#include <string>
#include <cmath>
#include <cstdint>

// ── Token type ────────────────────────────────────────────────────────────────
// Numbers after each name match the §4 production so they can be used in
// switch statements from spec pseudocode.
enum class GlintCssTokenType : uint8_t
{
  // §4.3.1
  IDENT,           // identifier
  FUNCTION,        // identifier followed by '('  — value = name (without '(')
  AT_KEYWORD,      // @identifier                 — value = name (without '@')
  HASH,            // #name                       — value = name (without '#')
  STRING,          // "..." or '...'              — value = unescaped content
  BAD_STRING,      // parse error inside string
  URL,             // url(...)                    — value = url (whitespace stripped)
  BAD_URL,         // parse error inside url()
  DELIM,           // any single character not covered above — value is the character
  NUMBER,          // integer or real number
  PERCENTAGE,      // number '%'
  DIMENSION,       // number <ident>              — value = ident, numericValue = number
  WHITESPACE,      // one or more whitespace characters
  CDO,             // <!--
  CDC,             // -->
  COLON,           // :
  SEMICOLON,       // ;
  COMMA,           // ,
  OPEN_SQUARE,     // [
  CLOSE_SQUARE,    // ]
  OPEN_PAREN,      // (
  CLOSE_PAREN,     // )
  OPEN_CURLY,      // {
  CLOSE_CURLY,     // }
  COMMENT,         // /* ... */ — preserved so the parser can load disabled declarations
  EOF_TOKEN,       // end of stream
};

// HASH flag: whether the value is an identifier or an unrestricted name
enum class GlintCssHashFlag : uint8_t { UNRESTRICTED, ID };

// NUMBER flag: integer vs number
enum class GlintCssNumericType : uint8_t { INTEGER, NUMBER };

// ── Token struct ──────────────────────────────────────────────────────────────
struct GlintCssToken
{
  GlintCssTokenType type = GlintCssTokenType::EOF_TOKEN;

  // String payload — for IDENT, FUNCTION, AT_KEYWORD, HASH, STRING, URL, DELIM,
  // DIMENSION (unit), and the raw representation of NUMBER/PERCENTAGE.
  std::string value;

  // Numeric payload — set for NUMBER, PERCENTAGE, DIMENSION.
  double numericValue = 0.0;

  // Type flags
  GlintCssHashFlag    hashFlag    = GlintCssHashFlag::UNRESTRICTED;
  GlintCssNumericType numericType = GlintCssNumericType::NUMBER;

  // Source position (line / column), 1-based — useful for error messages.
  uint32_t line   = 1;
  uint32_t column = 1;

  // ── Convenience constructors ─────────────────────────────────────────────
  static GlintCssToken make(GlintCssTokenType t,
                           std::string v    = {},
                           double num       = 0.0,
                           uint32_t ln      = 0,
                           uint32_t col     = 0)
  {
    GlintCssToken tok;
    tok.type         = t;
    tok.value        = std::move(v);
    tok.numericValue = num;
    tok.line         = ln;
    tok.column       = col;
    return tok;
  }

  // ── Helpers ──────────────────────────────────────────────────────────────
  bool is(GlintCssTokenType t) const { return type == t; }

  bool isWhitespace()  const { return type == GlintCssTokenType::WHITESPACE; }
  bool isEOF()         const { return type == GlintCssTokenType::EOF_TOKEN; }
  bool isIdent()       const { return type == GlintCssTokenType::IDENT; }
  bool isDelim(char c) const
  {
    return type == GlintCssTokenType::DELIM
        && !value.empty()
        && value[0] == c;
  }

  // True if this token is a <{-token>
  bool isOpenCurly()  const { return type == GlintCssTokenType::OPEN_CURLY; }
  bool isCloseCurly() const { return type == GlintCssTokenType::CLOSE_CURLY; }

  // True for Number/Percentage/Dimension
  bool isNumeric() const
  {
    return type == GlintCssTokenType::NUMBER
        || type == GlintCssTokenType::PERCENTAGE
        || type == GlintCssTokenType::DIMENSION;
  }

  // Return the integer value if this is an INTEGER-typed number token.
  int intValue() const { return static_cast<int>(std::round(numericValue)); }
};
