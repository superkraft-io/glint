#pragma once

/**
 * glint_css_cascade.hpp
 * CSS Cascade Level 5 — cascade + inheritance algorithms.
 *
 * Implements §6 Cascade:
 *   1. Filter:       collect all declarations that apply to the element
 *   2. Sort:         by cascade origin, importance, specificity, source order
 *   3. Defaulting:   inherit or use initial value for unset properties
 *
 * Cascade layers (§6.3):
 *   User-Agent < Author < User/Presentation < Animation/Transition
 *
 * Entry point:
 *   GlintCssCascade::computeDeclarations(element, stylesheets, inlineDecls)
 *     → vector<GlintCssDeclaration>  (one per property, winning value wins)
 *
 * Reference: https://www.w3.org/TR/css-cascade-5/
 */

#include "glint_css_rule.hpp"
#include "glint_css_selector.hpp"
#include "glint_css_parser.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Cascade origin ────────────────────────────────────────────────────────────
// Higher number = higher precedence (before importance flip).
enum class GlintCssOrigin : uint8_t
{
  USER_AGENT  = 0,
  USER        = 1,
  AUTHOR      = 2,
  ANIMATION   = 3,  // CSS Animations (overrides author)
  TRANSITION  = 4,  // CSS Transitions (highest)
};

// ── GlintMatchedDeclaration ─────────────────────────────────────────────────────
// One declaration that matched an element, plus its cascade metadata.
struct GlintMatchedDeclaration
{
  GlintCssDeclaration   decl;
  GlintCssSpecificity   specificity;
  GlintCssOrigin        origin      = GlintCssOrigin::AUTHOR;
  size_t               sourceOrder = 0;   // index of the rule in the stylesheet (0-based)
  bool                 isInline    = false;
  int                  layerOrder  = -1;  // @layer order (-1 = unlayered = highest in same origin)
  // Source provenance — used by the inspector to match disabled-decl ids.
  std::string          sourceUrl;
  uint32_t             sourceLine  = 0;
  // Selector text of the originating rule (stable across line-number shifts
  // caused by CSS edits; used as part of the disabled-decl identity key).
  std::string          selectorText;
};

// ── GlintCssCascade ────────────────────────────────────────────────────────────
class GlintCssCascade
{
public:
  // ── §6.2 — Collect matching declarations for one element ─────────────────
  //
  // selSheets:    stylesheet(s) at AUTHOR origin
  // agSheets:     user-agent stylesheets (optional)
  // inlineDecls:  declarations from the element's `style="..."` attribute
  //
  // Returns a map from property → winning declaration (after cascade sort).
  static std::unordered_map<std::string, GlintMatchedDeclaration>
  computeDeclarations(
    const GlintCssDomElement&                    element,
    const std::vector<const GlintCssStylesheet*>& selSheets,
    const std::vector<GlintCssDeclaration>&       inlineDecls,
    const std::vector<const GlintCssStylesheet*>& agSheets = {})
  {
    std::vector<GlintMatchedDeclaration> matched;
    size_t sourceOrder = 0;

    // ── 1. User-agent stylesheets ─────────────────────────────────────────
    for (const auto* sheet : agSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::USER_AGENT, sourceOrder, matched);

    // ── 2. Author stylesheets ─────────────────────────────────────────────
    for (const auto* sheet : selSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::AUTHOR, sourceOrder, matched);

    // ── 3. Inline style declarations ──────────────────────────────────────
    //    Inline = author origin, specificity (1,0,0,0) (above any selector),
    //    effectively treated as specificity infinity within author origin.
    for (const auto& d : inlineDecls)
    {
      GlintMatchedDeclaration md;
      md.decl        = d;
      md.specificity = { 0, 0, 0 }; // overridden by isInline flag below
      md.origin      = GlintCssOrigin::AUTHOR;
      md.sourceOrder = sourceOrder++;
      md.isInline    = true;
      matched.push_back(std::move(md));
    }

    // ── 4. Sort per cascade algorithm (§6.2.1) ────────────────────────────
    //    Precedence order (highest to lowest wins):
    //    a) Transition declarations
    //    b) Important user-agent
    //    c) Important user
    //    d) Important author (+ important inline)
    //    e) Animation declarations
    //    f) Normal author (inline = highest specificity within author)
    //    g) Normal user
    //    h) Normal user-agent
    //
    //    Within the same origin+importance tier: higher specificity wins.
    //    Ties in specificity: later source order wins.

    std::sort(matched.begin(), matched.end(),
      [](const GlintMatchedDeclaration& a, const GlintMatchedDeclaration& b)
      {
        const int wa = cascadeWeight(a);
        const int wb = cascadeWeight(b);
        if (wa != wb) return wa < wb;  // higher wins → sort ascending then take last

        // Same weight: higher specificity wins
        if (a.specificity.value() != b.specificity.value())
          return a.specificity.value() < b.specificity.value();

        // Tie: later source order wins (ascending → last element wins)
        return a.sourceOrder < b.sourceOrder;
      });

    // ── 5. Build property → winning declaration map ───────────────────────
    //    Because we sorted ascending-wins, iterate forward and overwrite;
    //    the last written value for each property wins.
    //    AST-disabled declarations (loaded from /* ... */ comments) are skipped
    //    here so they never affect the normal render path.  resolveSkipping()
    //    does NOT call computeDeclarations — it has its own loop that gates
    //    them via the mInspDisabledDecls ID set, which allows re-enabling.
    std::unordered_map<std::string, GlintMatchedDeclaration> result;
    for (auto& md : matched)
    {
      if (md.decl.disabled) continue; // AST-disabled (file-commented); excluded from cascade
      result[md.decl.property] = std::move(md);
    }

    return result;
  }

  // ── Convenience: return only the winning GlintCssDeclaration per property ─
  static std::vector<GlintCssDeclaration>
  resolve(
    const GlintCssDomElement&                     element,
    const std::vector<const GlintCssStylesheet*>&  sheets,
    const std::vector<GlintCssDeclaration>&        inlineDecls)
  {
    const auto winning = computeDeclarations(element, sheets, inlineDecls);
    std::vector<GlintCssDeclaration> out;
    out.reserve(winning.size());
    for (const auto& kv : winning)
      out.push_back(kv.second.decl);
    return out;
  }

  // ── Like resolve(), but skips declarations whose composite id
  //    "sourceUrl|sourceLine|property" is in `disabled`.
  //    The skip happens DURING reduction so the next-best non-disabled
  //    declaration wins (not after — that would leave the property empty).
  static std::vector<GlintCssDeclaration>
  resolveSkipping(
    const GlintCssDomElement&                     element,
    const std::vector<const GlintCssStylesheet*>&  sheets,
    const std::vector<GlintCssDeclaration>&        inlineDecls,
    const std::unordered_set<std::string>&        disabled,
    const std::vector<const GlintCssStylesheet*>&  agSheets = {})
  {
    // Collect + sort identically to computeDeclarations.
    std::vector<GlintMatchedDeclaration> matched;
    size_t sourceOrder = 0;
    for (const auto* sheet : agSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::USER_AGENT, sourceOrder, matched);
    for (const auto* sheet : sheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::AUTHOR, sourceOrder, matched);
    for (const auto& d : inlineDecls)
    {
      GlintMatchedDeclaration md;
      md.decl        = d;
      md.specificity = { 0, 0, 0 };
      md.origin      = GlintCssOrigin::AUTHOR;
      md.sourceOrder = sourceOrder++;
      md.isInline    = true;
      matched.push_back(std::move(md));
    }
    std::sort(matched.begin(), matched.end(),
      [](const GlintMatchedDeclaration& a, const GlintMatchedDeclaration& b)
      {
        const int wa = cascadeWeight(a);
        const int wb = cascadeWeight(b);
        if (wa != wb) return wa < wb;
        if (a.specificity.value() != b.specificity.value())
          return a.specificity.value() < b.specificity.value();
        return a.sourceOrder < b.sourceOrder;
      });
    // Reduce: iterate ascending (last non-disabled overwrite wins).
    // The disabled-decl id uses selectorText (not sourceLine) so it remains
    // stable when CSS edits shift line numbers in the same file.
    std::unordered_map<std::string, GlintCssDeclaration> result;
    for (auto& md : matched)
    {
      const std::string dId = md.sourceUrl + "|" +
                              md.selectorText + "|" +
                              md.decl.property;
      if (disabled.count(dId)) continue;   // skip — delegate to next best
      result[md.decl.property] = md.decl;
    }
    std::vector<GlintCssDeclaration> out;
    out.reserve(result.size());
    for (auto& kv : result)
      out.push_back(std::move(kv.second));
    return out;
  }

private:
  // ── Assign a numeric cascade weight for sorting ───────────────────────────
  // Higher = wins later (we sort ascending and take the last override).
  static int cascadeWeight(const GlintMatchedDeclaration& md)
  {
    // Transitions win everything
    if (md.origin == GlintCssOrigin::TRANSITION) return 100;

    // !important user-agent is very high
    if (md.decl.important && md.origin == GlintCssOrigin::USER_AGENT) return 80;

    // !important author
    if (md.decl.important && (md.origin == GlintCssOrigin::AUTHOR   ||
                               md.origin == GlintCssOrigin::USER))    return 70;

    // Animations
    if (md.origin == GlintCssOrigin::ANIMATION) return 60;

    // Normal inline author
    if (md.isInline && !md.decl.important) return 50;

    // Normal author / user
    if (md.origin == GlintCssOrigin::AUTHOR) return 40;
    if (md.origin == GlintCssOrigin::USER)   return 30;

    // Normal user-agent
    return 10;
  }

  // ── Collect matching declarations from one stylesheet ─────────────────────
  static void collectFromSheet(
    const GlintCssDomElement&     element,
    const GlintCssStylesheet&     sheet,
    GlintCssOrigin                origin,
    size_t&                      sourceOrder,
    std::vector<GlintMatchedDeclaration>& out)
  {
    std::vector<const GlintCssQualifiedRule*> rules;
    sheet.collectQualifiedRules(rules);

    for (const auto* rule : rules)
    {
      if (!rule->selectorList.matches(element)) continue;
      const GlintCssSpecificity spec = rule->selectorList.matchingSpecificity(element);

      for (const auto& decl : rule->declarations)
      {
        // NOTE: AST-disabled declarations (loaded from /* ... */ comments) are NOT
        // skipped here. Instead, show() seeds mInspDisabledDecls with their IDs so
        // resolveSkipping() handles them — this means re-enabling via the inspector
        // checkbox (which removes the ID) immediately participates in the cascade.
        GlintMatchedDeclaration md;
        md.decl        = decl;
        md.specificity = spec;
        md.origin      = origin;
        md.sourceOrder  = sourceOrder++;
        md.sourceUrl    = sheet.sourceUrl;
        md.sourceLine   = rule->sourceLine;
        md.selectorText = rule->prelude;
        out.push_back(std::move(md));
      }
    }
  }
};
