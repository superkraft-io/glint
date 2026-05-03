#pragma once

/**
 * glint_css_rule.hpp
 * CSS rule AST — QualifiedRule, AtRule, Declaration, Stylesheet.
 *
 * A stylesheet is a list of top-level rules.  Each rule is either:
 *   - A QualifiedRule:  selector-list { declaration-list }
 *   - An AtRule:        @keyword prelude { block } | @keyword prelude ;
 *
 * Declarations hold a property name and a list of component values.
 * The component values are stored both as a raw string (for glint_css_apply
 * which does its own value parsing) and as a typed token vector.
 *
 * Reference: https://www.w3.org/TR/css-syntax-3/#parsing
 */

#include "glint_css_token.hpp"
#include "glint_css_selector.hpp"

#include <memory>
#include <string>
#include <vector>

// ── GlintCssDeclaration ────────────────────────────────────────────────────────
struct GlintCssDeclaration
{
  std::string              property;   // lower-case property name
  std::string              value;      // raw value string (trimmed)
  std::vector<GlintCssToken> valueTokens; // tokenized value component values
  bool                     important = false; // true if !important was present
  bool                     disabled  = false; // true if loaded from a /* ... */ comment block
  uint32_t                 sourceLine = 0;    // for error messages
};

// ── GlintCssQualifiedRule ──────────────────────────────────────────────────────
// Represents one style rule:  selector-list { decl ; decl ; … }
struct GlintCssQualifiedRule
{
  GlintSelectorList             selectorList;
  std::vector<GlintCssDeclaration> declarations;

  // Raw prelude tokens (before parsing into GlintSelectorList) — preserved for
  // debugging / re-serialisation.
  std::vector<GlintCssToken> prelToks;
  // Trimmed prelude string — set by the parser after block consumption.
  // For regular style rules this is the selector text; for @keyframes child
  // stops it is the stop selector ("from", "to", "0%", "50%", etc.).
  std::string prelude;
  uint32_t sourceLine = 0;
};

// ── GlintCssAtRule ─────────────────────────────────────────────────────────────
// Represents @media, @keyframes, @import, @charset, @supports, @layer, etc.
struct GlintCssAtRule
{
  std::string              name;        // keyword without '@' (lower-case)
  std::string              prelude;     // raw prelude string (after keyword, before '{' or ';')
  std::vector<GlintCssToken> preludeToks;

  // Child rules (for at-rules with a block that contains style rules, e.g. @media)
  // Stored as variant: the owned vector alternates between QualifiedRule or AtRule.
  // We model it with tagged-union structs for simplicity.
  struct ChildRule
  {
    enum class Kind { QUALIFIED, AT } kind;
    std::shared_ptr<GlintCssQualifiedRule> qualified;
    std::shared_ptr<GlintCssAtRule>        atRule;
  };
  std::vector<ChildRule> children;

  // For at-rules with a declaration block (e.g. @font-face, @keyframes stops)
  std::vector<GlintCssDeclaration> declarations;

  // Raw block token list (inner tokens between the braces)
  std::vector<GlintCssToken> blockToks;

  uint32_t sourceLine = 0;
};

// ── GlintCssStylesheet ─────────────────────────────────────────────────────────
// Top-level result of parsing a CSS source string.
struct GlintCssStylesheet
{
  struct Rule
  {
    enum class Kind { QUALIFIED, AT } kind;
    std::shared_ptr<GlintCssQualifiedRule> qualified;
    std::shared_ptr<GlintCssAtRule>        atRule;
  };

  std::vector<Rule> rules;

  // URL or path of the file this stylesheet was loaded from.  Set by
  // glint_document::loadStylesheet() immediately after parsing.  Empty string
  // when the stylesheet was parsed from an inline string.
  std::string sourceUrl;

  // ── Helpers ───────────────────────────────────────────────────────────────

  // Collect all qualified (style) rules, including nested in @media / @supports / @layer blocks.
  void collectQualifiedRules(std::vector<const GlintCssQualifiedRule*>& out,
                              const std::string& mediaFeature = "") const
  {
    for (const auto& r : rules)
    {
      if (r.kind == Rule::Kind::QUALIFIED)
      {
        out.push_back(r.qualified.get());
      }
      else if (r.kind == Rule::Kind::AT && r.atRule)
      {
        const std::string& n = r.atRule->name;
        if (n == "media" || n == "supports" || n == "layer" || n == "document")
        {
          for (const auto& child : r.atRule->children)
          {
            if (child.kind == GlintCssAtRule::ChildRule::Kind::QUALIFIED && child.qualified)
              out.push_back(child.qualified.get());
          }
        }
      }
    }
    (void)mediaFeature;
  }

};
