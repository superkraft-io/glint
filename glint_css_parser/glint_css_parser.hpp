#pragma once

/**
 * glint_css_parser.hpp
 * CSS Syntax Module Level 3 §5 — parser algorithms.
 *
 * Implements:
 *   §5.3.3  Parse a stylesheet
 *   §5.3.4  Parse a list of rules
 *   §5.3.5  Parse a rule
 *   §5.3.6  Parse a declaration
 *   §5.3.7  Parse a list of declarations
 *   §5.3.8  Parse a component value
 *   §5.4    Parsing algorithms (consume qualified rule, at-rule, declarations)
 *
 *   Selector parsing per Selectors Level 4 §4–§10.
 *
 * Entry points:
 *   GlintCssParser::parseStylesheet(source)           → GlintCssStylesheet
 *   GlintCssParser::parseDeclarationList(source)      → vector<GlintCssDeclaration>
 *   GlintCssParser::parseInlineStyle(source)          → vector<GlintCssDeclaration>
 *   GlintCssParser::parseSelector(source)             → GlintSelectorList
 *
 * Reference: https://www.w3.org/TR/css-syntax-3/#parsing
 *            https://www.w3.org/TR/selectors-4/
 */

#include "glint_css_tokenizer.hpp"
#include "glint_css_rule.hpp"
#include "glint_css_selector.hpp"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// ── GlintCssParser ─────────────────────────────────────────────────────────────
class GlintCssParser
{
public:
  // ── Public parse entry points ─────────────────────────────────────────────

  // Parse a full CSS stylesheet string.
  static GlintCssStylesheet parseStylesheet(const std::string& src)
  {
    GlintCssParser p(GlintCssTokenizer::tokenize(src));
    return p.consumeStylesheet();
  }

  // Parse a declaration-block body (everything between the braces).
  static std::vector<GlintCssDeclaration> parseDeclarationList(const std::string& src)
  {
    GlintCssParser p(GlintCssTokenizer::tokenize(src));
    return p.consumeDeclarationList();
  }

  // Parse an inline `style="..."` attribute string.
  // Identical to parseDeclarationList but named to match browser terminology.
  static std::vector<GlintCssDeclaration> parseInlineStyle(const std::string& src)
  {
    return parseDeclarationList(src);
  }

  // Parse a CSS selector string.
  static GlintSelectorList parseSelector(const std::string& src)
  {
    GlintCssParser p(GlintCssTokenizer::tokenize(src));
    return p.consumeSelectorList();
  }

private:
  // ── Token stream ──────────────────────────────────────────────────────────
  std::vector<GlintCssToken> mToks;
  size_t                    mPos = 0;

  explicit GlintCssParser(std::vector<GlintCssToken> toks)
    : mToks(std::move(toks)), mPos(0) {}

  // ── Token stream helpers ──────────────────────────────────────────────────

  const GlintCssToken& current() const
  {
    static GlintCssToken eof = GlintCssToken::make(GlintCssTokenType::EOF_TOKEN);
    return mPos < mToks.size() ? mToks[mPos] : eof;
  }

  const GlintCssToken& peek(size_t offset = 1) const
  {
    static GlintCssToken eof = GlintCssToken::make(GlintCssTokenType::EOF_TOKEN);
    const size_t i = mPos + offset;
    return i < mToks.size() ? mToks[i] : eof;
  }

  GlintCssToken consume()
  {
    if (mPos < mToks.size()) return mToks[mPos++];
    return GlintCssToken::make(GlintCssTokenType::EOF_TOKEN);
  }

  void reconsume() { if (mPos > 0) --mPos; }

  bool eof() const { return current().isEOF(); }

  // Consume and discard all leading whitespace tokens.
  void skipWhitespace()
  {
    while (current().isWhitespace()) consume();
  }

  // Returns a lower-case copy of s.
  static std::string toLower(std::string s)
  {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  static std::string trim(const std::string& s)
  {
    const size_t start = s.find_first_not_of(" \t\n\r\f");
    if (start == std::string::npos) return {};
    const size_t end = s.find_last_not_of(" \t\n\r\f");
    return s.substr(start, end - start + 1);
  }

  // Join a span of tokens back to a raw string (preserves whitespace).
  static std::string tokensToString(const std::vector<GlintCssToken>& toks)
  {
    std::string out;
    for (const auto& t : toks)
    {
      switch (t.type)
      {
        case GlintCssTokenType::WHITESPACE:    out += ' ';            break;
        case GlintCssTokenType::STRING:        out += '"' + t.value + '"'; break;
        case GlintCssTokenType::IDENT:
        case GlintCssTokenType::DELIM:
        case GlintCssTokenType::AT_KEYWORD:    out += t.value;        break;
        case GlintCssTokenType::FUNCTION:      out += t.value + '('; break;
        case GlintCssTokenType::HASH:          out += '#' + t.value;  break;
        case GlintCssTokenType::COLON:         out += ':';            break;
        case GlintCssTokenType::SEMICOLON:     out += ';';            break;
        case GlintCssTokenType::COMMA:         out += ',';            break;
        case GlintCssTokenType::OPEN_PAREN:    out += '(';            break;
        case GlintCssTokenType::CLOSE_PAREN:   out += ')';            break;
        case GlintCssTokenType::OPEN_SQUARE:   out += '[';            break;
        case GlintCssTokenType::CLOSE_SQUARE:  out += ']';            break;
        case GlintCssTokenType::OPEN_CURLY:    out += '{';            break;
        case GlintCssTokenType::CLOSE_CURLY:  out += '}';            break;
        case GlintCssTokenType::COMMENT:      out += "/*" + t.value + "*/"; break;
        case GlintCssTokenType::NUMBER:        out += t.value;        break;
        case GlintCssTokenType::PERCENTAGE:
        {
          char buf[32]; std::snprintf(buf, sizeof(buf), "%g", t.numericValue);
          out += buf; out += '%';
          break;
        }
        case GlintCssTokenType::DIMENSION:
        {
          char buf[32]; std::snprintf(buf, sizeof(buf), "%g", t.numericValue);
          out += buf; out += t.value;
          break;
        }
        case GlintCssTokenType::URL:           out += "url(" + t.value + ')'; break;
        default: break;
      }
    }
    return out;
  }

  // ── §5.4.1 — Consume a list of rules ─────────────────────────────────────
  GlintCssStylesheet consumeStylesheet()
  {
    GlintCssStylesheet sheet;
    for (;;)
    {
      skipWhitespace();
      if (eof()) break;

      // CSS comments at the top level are not rules — skip them.
      // Without this, a comment gets absorbed into the prelToks of the next
      // qualified rule, corrupting its selector text (and the source-line used
      // by the inspector).  This matches Chrome's behaviour: comments between
      // top-level rules are simply discarded.
      if (current().type == GlintCssTokenType::COMMENT)
      {
        consume(); continue;
      }

      if (current().type == GlintCssTokenType::CDO ||
          current().type == GlintCssTokenType::CDC)
      {
        consume(); continue;
      }

      if (current().type == GlintCssTokenType::AT_KEYWORD)
      {
        auto atRule = consumeAtRule();
        if (atRule)
        {
          GlintCssStylesheet::Rule r;
          r.kind   = GlintCssStylesheet::Rule::Kind::AT;
          r.atRule = std::move(atRule);
          sheet.rules.push_back(std::move(r));
        }
        continue;
      }

      auto qualRule = consumeQualifiedRule();
      if (qualRule)
      {
        GlintCssStylesheet::Rule r;
        r.kind      = GlintCssStylesheet::Rule::Kind::QUALIFIED;
        r.qualified = std::move(qualRule);
        sheet.rules.push_back(std::move(r));
      }
    }
    return sheet;
  }

  // ── §5.4.2 — Consume an at-rule ──────────────────────────────────────────
  std::shared_ptr<GlintCssAtRule> consumeAtRule()
  {
    auto rule = std::make_shared<GlintCssAtRule>();
    rule->name       = toLower(consume().value); // consume @keyword
    rule->sourceLine = current().line;

    // Collect prelude tokens until '{' or ';' or EOF
    for (;;)
    {
      if (eof()) break;
      if (current().type == GlintCssTokenType::SEMICOLON) { consume(); break; }
      if (current().type == GlintCssTokenType::OPEN_CURLY)
      {
        // Consume the block
        consume(); // consume '{'
        rule->blockToks = consumeSimpleBlock(GlintCssTokenType::CLOSE_CURLY);

        // For at-rules that contain style rules (media, supports, layer, document),
        // re-parse the block as a rule list.
        const std::string& n = rule->name;
        if (n == "media" || n == "supports" || n == "layer" || n == "document")
        {
          // Parse child rules from blockToks
          GlintCssParser inner(rule->blockToks);
          while (!inner.eof())
          {
            inner.skipWhitespace();
            if (inner.eof()) break;
            if (inner.current().type == GlintCssTokenType::AT_KEYWORD)
            {
              auto childAt = inner.consumeAtRule();
              if (childAt)
              {
                GlintCssAtRule::ChildRule cr;
                cr.kind   = GlintCssAtRule::ChildRule::Kind::AT;
                cr.atRule = std::move(childAt);
                rule->children.push_back(std::move(cr));
              }
            }
            else
            {
              auto childQual = inner.consumeQualifiedRule();
              if (childQual)
              {
                GlintCssAtRule::ChildRule cr;
                cr.kind      = GlintCssAtRule::ChildRule::Kind::QUALIFIED;
                cr.qualified = std::move(childQual);
                rule->children.push_back(std::move(cr));
              }
            }
          }
        }
        else if (n == "keyframes" || n == "-webkit-keyframes")
        {
          // @keyframes inner block: each stop is a qualified rule
          //   from { ... }  /  to { ... }  /  0% { ... }
          // Parse as child qualified rules (NOT a declaration list).
          GlintCssParser inner(rule->blockToks);
          while (!inner.eof())
          {
            inner.skipWhitespace();
            if (inner.eof()) break;
            auto childQual = inner.consumeQualifiedRule();
            if (childQual)
            {
              GlintCssAtRule::ChildRule cr;
              cr.kind      = GlintCssAtRule::ChildRule::Kind::QUALIFIED;
              cr.qualified = std::move(childQual);
              rule->children.push_back(std::move(cr));
            }
          }
        }
        else if (n == "font-face")
        {
          // @font-face inner block IS a declaration list (font descriptors).
          GlintCssParser inner(rule->blockToks);
          rule->declarations = inner.consumeDeclarationList();
        }
        break;
      }

      const GlintCssToken tok = consume();
      rule->preludeToks.push_back(tok);
    }
    rule->prelude = trim(tokensToString(rule->preludeToks));
    return rule;
  }

  // ── §5.4.3 — Consume a qualified rule ────────────────────────────────────
  std::shared_ptr<GlintCssQualifiedRule> consumeQualifiedRule()
  {
    auto rule = std::make_shared<GlintCssQualifiedRule>();
    rule->sourceLine = current().line;

    // Collect prelude tokens (selector) until '{'
    for (;;)
    {
      if (eof())
      {
        // Parse error — discard
        return nullptr;
      }
      if (current().type == GlintCssTokenType::OPEN_CURLY)
      {
        consume(); // consume '{'
        const std::vector<GlintCssToken> blockToks = consumeSimpleBlock(GlintCssTokenType::CLOSE_CURLY);

        // Parse declarations from block
        GlintCssParser inner(blockToks);
        rule->declarations = inner.consumeDeclarationList();

        // Parse selectors from prelude
        {
          GlintCssParser selP(rule->prelToks);
          rule->selectorList = selP.consumeSelectorList();
        }
        // Store the trimmed prelude string (selector text or keyframe stop selector).
        rule->prelude = trim(tokensToString(rule->prelToks));
        return rule;
      }
      rule->prelToks.push_back(consume());
    }
  }

  // ── §5.4.6 — Consume a simple block ──────────────────────────────────────
  // Consumes tokens up to and including the mirror of the opening bracket.
  // The opening bracket has already been consumed when this is called.
  // endType is CLOSE_CURLY, CLOSE_PAREN, or CLOSE_SQUARE.
  std::vector<GlintCssToken> consumeSimpleBlock(GlintCssTokenType endType)
  {
    std::vector<GlintCssToken> toks;
    for (;;)
    {
      if (eof() || current().type == endType) { consume(); break; }

      // Nested blocks
      if (current().type == GlintCssTokenType::OPEN_CURLY)
      {
        toks.push_back(consume());
        for (const auto& t : consumeSimpleBlock(GlintCssTokenType::CLOSE_CURLY))
          toks.push_back(t);
        toks.push_back(GlintCssToken::make(GlintCssTokenType::CLOSE_CURLY));
        continue;
      }
      if (current().type == GlintCssTokenType::OPEN_PAREN)
      {
        toks.push_back(consume());
        for (const auto& t : consumeSimpleBlock(GlintCssTokenType::CLOSE_PAREN))
          toks.push_back(t);
        toks.push_back(GlintCssToken::make(GlintCssTokenType::CLOSE_PAREN));
        continue;
      }
      if (current().type == GlintCssTokenType::OPEN_SQUARE)
      {
        toks.push_back(consume());
        for (const auto& t : consumeSimpleBlock(GlintCssTokenType::CLOSE_SQUARE))
          toks.push_back(t);
        toks.push_back(GlintCssToken::make(GlintCssTokenType::CLOSE_SQUARE));
        continue;
      }

      toks.push_back(consume());
    }
    return toks;
  }

  // ── §5.4.4 — Consume a list of declarations ──────────────────────────────
  std::vector<GlintCssDeclaration> consumeDeclarationList()
  {
    std::vector<GlintCssDeclaration> decls;
    for (;;)
    {
      skipWhitespace();
      if (eof()) break;

      if (current().type == GlintCssTokenType::SEMICOLON) { consume(); continue; }
      if (current().type == GlintCssTokenType::AT_KEYWORD) { consumeAtRule(); continue; }

      // COMMENT token — try to parse the body as a disabled declaration.
      // We write disabled declarations as "/* prop: val; */" on save, so the
      // body we receive here is " prop: val; " (with optional whitespace/semicolon).
      if (current().type == GlintCssTokenType::COMMENT)
      {
        const std::string body = current().value;
        consume();
        // Strip leading/trailing whitespace and a trailing ';'
        std::string stripped = trim(body);
        if (!stripped.empty() && stripped.back() == ';')
          stripped.pop_back();
        stripped = trim(stripped);
        // Re-tokenize the comment body and attempt to parse it as a declaration.
        if (!stripped.empty())
        {
          auto bodyToks = GlintCssTokenizer::tokenize(stripped);
          if (auto d = parseDeclarationFrom(bodyToks))
          {
            d->disabled = true;
            decls.push_back(std::move(*d));
          }
        }
        continue;
      }

      if (current().type == GlintCssTokenType::IDENT)
      {
        // Collect tokens for this declaration up to ';' or EOF
        std::vector<GlintCssToken> tmp;
        while (!eof()
               && current().type != GlintCssTokenType::SEMICOLON
               && current().type != GlintCssTokenType::CLOSE_CURLY)
        {
          tmp.push_back(consume());
        }
        if (auto d = parseDeclarationFrom(tmp))
          decls.push_back(std::move(*d));
        continue;
      }

      // Error recovery: consume until next ';' or EOF
      while (!eof()
             && current().type != GlintCssTokenType::SEMICOLON
             && current().type != GlintCssTokenType::CLOSE_CURLY)
      {
        consume();
      }
    }
    return decls;
  }

  // ── §5.4.5 — Parse a single declaration from a token sequence ────────────
  // Returns nullopt on parse error.
  static std::optional<GlintCssDeclaration> parseDeclarationFrom(
    const std::vector<GlintCssToken>& toks)
  {
    GlintCssParser p(toks);
    p.skipWhitespace();
    if (p.eof() || !p.current().isIdent()) return std::nullopt;

    GlintCssDeclaration decl;
    decl.property    = toLower(p.consume().value);
    decl.sourceLine  = 0;
    p.skipWhitespace();
    if (p.eof() || p.current().type != GlintCssTokenType::COLON) return std::nullopt;
    p.consume(); // consume ':'
    p.skipWhitespace();

    // Collect remaining value tokens
    while (!p.eof())
      decl.valueTokens.push_back(p.consume());

    // Strip trailing whitespace from valueTokens
    while (!decl.valueTokens.empty() && decl.valueTokens.back().isWhitespace())
      decl.valueTokens.pop_back();

    // Check for !important
    if (decl.valueTokens.size() >= 2)
    {
      const auto& last  = decl.valueTokens.back();
      const auto& penul = decl.valueTokens[decl.valueTokens.size() - 2];
      const bool lastIsImportant  = last.isIdent() && toLower(last.value) == "important";
      const bool penulIsBang      = penul.isDelim('!');
      if (lastIsImportant && penulIsBang)
      {
        decl.important = true;
        decl.valueTokens.resize(decl.valueTokens.size() - 2);
        while (!decl.valueTokens.empty() && decl.valueTokens.back().isWhitespace())
          decl.valueTokens.pop_back();
      }
    }

    decl.value = trim(tokensToString(decl.valueTokens));
    return decl;
  }

  // ─────────────────────────────────────────────────────────────────────────
  // SELECTOR PARSER
  // Selectors Level 4 §4–§10
  // ─────────────────────────────────────────────────────────────────────────

  // ── Consume a selector list (comma-separated complex selectors) ──────────
  GlintSelectorList consumeSelectorList()
  {
    GlintSelectorList list;
    skipWhitespace();
    while (!eof())
    {
      const auto complex = consumeComplexSelector();
      if (!complex.steps.empty())
        list.selectors.push_back(complex);

      skipWhitespace();
      if (eof()) break;
      if (current().type == GlintCssTokenType::COMMA) { consume(); skipWhitespace(); }
    }
    return list;
  }

  // ── Consume a complex selector (compound selectors joined by combinators) ─
  GlintComplexSelector consumeComplexSelector()
  {
    GlintComplexSelector complex;

    // First compound — no leading combinator
    const auto first = consumeCompoundSelector();
    if (first.simples.empty()) return complex; // empty → no match

    GlintComplexSelector::Step firstStep;
    firstStep.combinator = GlintCombinator::DESCENDANT; // unused for index 0
    firstStep.compound   = first;
    complex.steps.push_back(std::move(firstStep));

    while (!eof())
    {
      // Peek for combinator or end
      const bool ws = current().isWhitespace();
      skipWhitespace();

      if (eof() || current().type == GlintCssTokenType::COMMA
                 || current().type == GlintCssTokenType::OPEN_CURLY)
        break;

      GlintCombinator comb = GlintCombinator::DESCENDANT;
      if (current().isDelim('>'))      { consume(); comb = GlintCombinator::CHILD;            skipWhitespace(); }
      else if (current().isDelim('+')) { consume(); comb = GlintCombinator::ADJACENT_SIBLING; skipWhitespace(); }
      else if (current().isDelim('~')) { consume(); comb = GlintCombinator::GENERAL_SIBLING;  skipWhitespace(); }
      else if (!ws)
      {
        // No whitespace and no explicit combinator — end of complex selector
        break;
      }
      // else: whitespace combinator (DESCENDANT), already set

      const auto compound = consumeCompoundSelector();
      if (compound.simples.empty()) break;

      GlintComplexSelector::Step step;
      step.combinator = comb;
      step.compound   = compound;
      complex.steps.push_back(std::move(step));
    }
    return complex;
  }

  // ── Consume a compound selector (sequence of simple selectors, no combinator) ─
  GlintCompoundSelector consumeCompoundSelector()
  {
    GlintCompoundSelector compound;
    while (!eof())
    {
      const GlintCssTokenType t = current().type;
      // End of compound: whitespace, combinator delimiter, comma, '{', EOF
      if (t == GlintCssTokenType::WHITESPACE
       || t == GlintCssTokenType::COMMA
       || t == GlintCssTokenType::OPEN_CURLY
       || t == GlintCssTokenType::CLOSE_PAREN
       || t == GlintCssTokenType::EOF_TOKEN)
        break;
      if (t == GlintCssTokenType::DELIM)
      {
        const char c = current().value.empty() ? 0 : current().value[0];
        if (c == '>' || c == '+' || c == '~') break; // combinator
      }

      auto ss = consumeSimpleSelector();
      if (ss.kind == GlintSimpleKind::UNIVERSAL && ss.name.empty())
        break; // parse error guard
      compound.simples.push_back(std::move(ss));
    }
    return compound;
  }

  // ── Consume a simple selector ─────────────────────────────────────────────
  GlintSimpleSelector consumeSimpleSelector()
  {
    GlintSimpleSelector ss;

    // '.' → class selector
    if (current().isDelim('.'))
    {
      consume();
      ss.kind = GlintSimpleKind::CLASS;
      ss.name = current().isIdent() ? consume().value : "";
      return ss;
    }

    // '#' → id selector (HASH token)
    if (current().type == GlintCssTokenType::HASH)
    {
      ss.kind = GlintSimpleKind::ID;
      ss.name = consume().value;
      return ss;
    }

    // '*' → universal selector
    if (current().isDelim('*'))
    {
      consume();
      ss.kind = GlintSimpleKind::UNIVERSAL;
      ss.name = "*";
      return ss;
    }

    // IDENT → type selector
    if (current().isIdent())
    {
      ss.kind = GlintSimpleKind::TYPE;
      ss.name = toLower(consume().value);
      return ss;
    }

    // ':' → pseudo-class or pseudo-element
    if (current().type == GlintCssTokenType::COLON)
    {
      consume(); // consume first ':'
      bool isPseudoElement = false;
      if (current().type == GlintCssTokenType::COLON)
      {
        consume(); // consume second ':'
        isPseudoElement = true;
      }
      ss.kind = isPseudoElement ? GlintSimpleKind::PSEUDO_ELEMENT : GlintSimpleKind::PSEUDO_CLASS;

      if (current().isIdent())
      {
        ss.name = toLower(consume().value);
      }
      else if (current().type == GlintCssTokenType::FUNCTION)
      {
        ss.name = toLower(consume().value); // name without '('
        // Consume argument tokens up to matching ')'
        std::vector<GlintCssToken> argToks;
        int depth = 1;
        while (!eof() && depth > 0)
        {
          if (current().type == GlintCssTokenType::OPEN_PAREN)  ++depth;
          if (current().type == GlintCssTokenType::CLOSE_PAREN) { --depth; if (depth == 0) { consume(); break; } }
          argToks.push_back(consume());
        }
        ss.argument = trim(tokensToString(argToks));

        // For :not(), :is(), :has(), :where() parse nested selector list
        if (ss.name == "not" || ss.name == "is" || ss.name == "has" || ss.name == "where")
        {
          GlintCssParser nested(argToks);
          for (auto& sel : nested.consumeSelectorList().selectors)
            ss.nestedSelectors.push_back(std::make_shared<GlintComplexSelector>(std::move(sel)));
        }
      }
      return ss;
    }

    // '[' → attribute selector
    if (current().type == GlintCssTokenType::OPEN_SQUARE)
    {
      consume(); // consume '['
      ss.kind = GlintSimpleKind::ATTRIBUTE;
      skipWhitespace();
      if (current().isIdent()) ss.attrName = toLower(consume().value);
      skipWhitespace();

      if (current().type == GlintCssTokenType::CLOSE_SQUARE)
      {
        consume();
        ss.attrOp = GlintAttrOp::EXISTS;
        return ss;
      }

      // Parse operator
      ss.attrOp = parseAttrOp();
      skipWhitespace();

      // Value
      if (current().type == GlintCssTokenType::STRING)
        ss.attrValue = consume().value;
      else if (current().isIdent())
        ss.attrValue = consume().value;

      skipWhitespace();

      // Optional case flag
      if (current().isIdent() && toLower(current().value) == "i")
      {
        ss.attrCaseInsensitive = true;
        consume();
        skipWhitespace();
      }

      if (current().type == GlintCssTokenType::CLOSE_SQUARE) consume();
      return ss;
    }

    // Fall-through: unknown — consume and return UNIVERSAL to avoid infinite loop
    consume();
    ss.kind = GlintSimpleKind::UNIVERSAL;
    ss.name = "";
    return ss;
  }

  // ── Parse attribute operator ──────────────────────────────────────────────
  GlintAttrOp parseAttrOp()
  {
    if (current().isDelim('='))   { consume(); return GlintAttrOp::EQUALS; }
    if (current().isDelim('~') && peek().isDelim('=')) { consume(); consume(); return GlintAttrOp::INCLUDES; }
    if (current().isDelim('|') && peek().isDelim('=')) { consume(); consume(); return GlintAttrOp::DASH_MATCH; }
    if (current().isDelim('^') && peek().isDelim('=')) { consume(); consume(); return GlintAttrOp::PREFIX; }
    if (current().isDelim('$') && peek().isDelim('=')) { consume(); consume(); return GlintAttrOp::SUFFIX; }
    if (current().isDelim('*') && peek().isDelim('=')) { consume(); consume(); return GlintAttrOp::SUBSTRING; }
    return GlintAttrOp::EXISTS;
  }
};
