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
#include "../utils/glint_perf.hpp"

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
  // Position after the cascade sort (higher = ranks later).  Winners must be
  // applied in ascending rank; see GlintCssCascade::inCascadeOrder().
  size_t               cascadeRank = 0;
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
    const std::vector<const GlintCssStylesheet*>& agSheets = {},
    const GlintCssMediaContext*                   media    = nullptr)   // nullptr: @media always applies
  {
    std::vector<GlintMatchedDeclaration> matched;
    size_t sourceOrder = 0;

    // ── 1. User-agent stylesheets ─────────────────────────────────────────
    for (const auto* sheet : agSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::USER_AGENT, sourceOrder, matched, media);

    // ── 2. Author stylesheets ─────────────────────────────────────────────
    for (const auto* sheet : selSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::AUTHOR, sourceOrder, matched, media);

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

    for (size_t i = 0; i < matched.size(); ++i)
      matched[i].cascadeRank = i;

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

  // ── Winners in the order they must be applied ───────────────────────────
  // glint applies a shorthand (margin, padding, border, background, flex...)
  // and its longhands (margin-top...) as separate properties, so whichever
  // ranks later in the cascade has to be applied last to win, exactly as if
  // the shorthand had been expanded into longhands.  The winners map has no
  // order, so sort by cascadeRank (lowest first).
  static std::vector<const GlintMatchedDeclaration*>
  inCascadeOrder(const std::unordered_map<std::string, GlintMatchedDeclaration>& winning)
  {
    std::vector<const GlintMatchedDeclaration*> ordered;
    ordered.reserve(winning.size());
    for (const auto& kv : winning)
      ordered.push_back(&kv.second);
    std::sort(ordered.begin(), ordered.end(),
      [](const GlintMatchedDeclaration* a, const GlintMatchedDeclaration* b)
      { return a->cascadeRank < b->cascadeRank; });
    return ordered;
  }

  // ── Convenience: the winning GlintCssDeclaration per property, in the
  //    order they must be applied (see inCascadeOrder) ──────────────────────
  static std::vector<GlintCssDeclaration>
  resolve(
    const GlintCssDomElement&                     element,
    const std::vector<const GlintCssStylesheet*>&  sheets,
    const std::vector<GlintCssDeclaration>&        inlineDecls)
  {
    const auto winning = computeDeclarations(element, sheets, inlineDecls);
    std::vector<GlintCssDeclaration> out;
    out.reserve(winning.size());
    for (const auto* md : inCascadeOrder(winning))
      out.push_back(md->decl);
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
    const std::vector<const GlintCssStylesheet*>&  agSheets = {},
    const GlintCssMediaContext*                    media    = nullptr)
  {
    // Collect + sort identically to computeDeclarations.
    std::vector<GlintMatchedDeclaration> matched;
    size_t sourceOrder = 0;
    for (const auto* sheet : agSheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::USER_AGENT, sourceOrder, matched, media);
    for (const auto* sheet : sheets)
      collectFromSheet(element, *sheet, GlintCssOrigin::AUTHOR, sourceOrder, matched, media);
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
    std::unordered_map<std::string, size_t> winnerIndex;   // property → index in matched
    for (size_t i = 0; i < matched.size(); ++i)
    {
      const auto& md = matched[i];
      const std::string dId = md.sourceUrl + "|" +
                              md.selectorText + "|" +
                              md.decl.property;
      if (disabled.count(dId)) continue;   // skip — delegate to next best
      winnerIndex[md.decl.property] = i;
    }
    // Output in cascade order (ascending index), see inCascadeOrder().
    std::vector<size_t> order;
    order.reserve(winnerIndex.size());
    for (const auto& kv : winnerIndex)
      order.push_back(kv.second);
    std::sort(order.begin(), order.end());
    std::vector<GlintCssDeclaration> out;
    out.reserve(order.size());
    for (const size_t i : order)
      out.push_back(std::move(matched[i].decl));
    return out;
  }

  // ── Indexed fast path ───────────────────────────────────────────────────
  // Rules bucketed by the key every match of their subject compound requires
  // (id, else first class, else tag, else universal). An element only needs
  // the rules in its own buckets: a rule that matches has a selector whose
  // subject compound matches, so the element carries that compound's key.
  struct RuleIndex
  {
    struct Entry
    {
      const GlintCssQualifiedRule* rule = nullptr;
      GlintCssOrigin               origin = GlintCssOrigin::AUTHOR;
    };
    std::vector<Entry> rules;   // flattened in cascade source order
    std::unordered_map<std::string, std::vector<uint32_t>> byId, byClass, byTag;
    std::vector<uint32_t> universal;

    // ── Invalidation dependencies ──────────────────────────────────────────
    // A pseudo-class or class in a NON-subject compound ("A:hover B",
    // ".open > .item", "a:focus ~ b") means a state change on one element
    // restyles others: its descendants (descendant/child combinator next to
    // the compound) or its following siblings (+ / ~). Each entry keeps the
    // compound so callers can skip elements that can't match it anyway.
    struct Dependency
    {
      const GlintCompoundSelector* compound = nullptr;
      bool                         siblings = false;   // else descendants
      bool                         adjacentOnly = false; // `+`: just the next sibling
    };
    std::unordered_map<std::string, std::vector<Dependency>> pseudoDeps;  // lower-case name
    std::unordered_map<std::string, std::vector<Dependency>> classDeps;

    // ── Child-list dependencies ────────────────────────────────────────────
    // Inserting, removing or reordering a parent's children moves the other
    // children (:nth-child, :last-of-type...), changes their previous siblings
    // (`+` / `~`) and can flip the parent's :empty. Each entry keeps the
    // compound matched against the affected element as a loose filter (null:
    // no filter, e.g. inside a complex :is()). Empty lists: child-list changes
    // never restyle anything.
    struct ChildListDependency
    {
      const GlintCompoundSelector* compound = nullptr;
      bool                         subtree  = false;
    };
    // Compound matched against a child of the changed parent; subtree: the
    // subject lies below that child (descendant / child combinator between).
    std::vector<ChildListDependency> childListDeps;
    // :empty in a compound matched against the changed parent itself;
    // subtree: the subject is a following sibling of it (or below one).
    std::vector<ChildListDependency> emptyDeps;

    bool hasChildListDeps() const { return !childListDeps.empty() || !emptyDeps.empty(); }

    // :has(): an element matching `anchor` (the compound holding the :has();
    // null = position unknown, any element) depends on its descendants /
    // following siblings. When one of those changes, the anchors above and
    // before it are re-cascaded. `subject` false: the rule's subject depends
    // on the anchor too (it is below it, or a following sibling: `siblings`).
    struct HasDependency
    {
      const GlintCompoundSelector* anchor   = nullptr;
      bool                         subject  = false;
      bool                         siblings = false;
    };
    std::vector<HasDependency>      hasDeps;
    std::unordered_set<std::string> hasClasses;   // classes used inside :has()
    std::unordered_set<std::string> hasPseudos;   // state pseudo-classes used inside :has()
    bool                            hasSiblingRelative = false;   // some :has(+ x) / :has(~ x)

    void clear()
    {
      rules.clear(); byId.clear(); byClass.clear(); byTag.clear(); universal.clear();
      pseudoDeps.clear(); classDeps.clear();
      childListDeps.clear(); emptyDeps.clear();
      hasDeps.clear(); hasClasses.clear(); hasPseudos.clear(); hasSiblingRelative = false;
    }

    void addHasDependencies(const GlintCompoundSelector& compound, const GlintCompoundSelector* owner,
                            bool subject, bool siblings)
    {
      for (const auto& ss : compound.simples)
      {
        if (ss.kind == GlintSimpleKind::PSEUDO_CLASS && lower(ss.name) == "has")
        {
          hasDeps.push_back({ owner, subject, siblings });
          for (const auto& rel : ss.nestedSelectors)
            if (rel) collectHasFeatures(*rel);
          continue;
        }
        // :has() inside :not() / :is() / :where(): their subject compound is
        // matched against the owner's element, anything further left is not.
        for (const auto& nested : ss.nestedSelectors)
          if (nested)
            for (size_t j = 0; j < nested->steps.size(); ++j)
              addHasDependencies(nested->steps[j].compound, j == 0 ? owner : nullptr,
                                 j == 0 && subject, siblings);
      }
    }

    void collectHasFeatures(const GlintComplexSelector& rel)
    {
      if (isSiblingCombinator(rel.anchorCombinator)) hasSiblingRelative = true;
      for (const auto& st : rel.steps)
        for (const auto& ss : st.compound.simples)
        {
          if (ss.kind == GlintSimpleKind::CLASS) hasClasses.insert(ss.name);
          else if (ss.kind == GlintSimpleKind::PSEUDO_CLASS) hasPseudos.insert(lower(ss.name));
          for (const auto& nested : ss.nestedSelectors)
            if (nested) collectHasFeatures(*nested);
        }
    }

    static bool isPositionalPseudo(const std::string& lowName)
    {
      static const char* kNames[] = { "first-child", "last-child", "only-child", "nth-child",
                                      "nth-last-child", "first-of-type", "last-of-type",
                                      "only-of-type", "nth-of-type", "nth-last-of-type" };
      for (const char* n : kNames)
        if (lowName == n) return true;
      return false;
    }

    static void pushUnique(std::vector<ChildListDependency>& v, const GlintCompoundSelector* c, bool subtree)
    {
      if (!v.empty() && v.back().compound == c && v.back().subtree == subtree) return;
      v.push_back({ c, subtree });
    }

    /**
     * Record the child-list features of `compound`, matched against the
     * element `owner` is (the k-th compound of its selector). `siblingChain`:
     * every combinator between that compound and the subject is `+` / `~`;
     * `siblingNext`: the combinator right after it (toward the subject) is.
     * `owner` null = position unknown (inside a complex :is()): no filter,
     * whole subtrees.
     */
    void addChildListDependencies(const GlintCompoundSelector& compound,
                                  const GlintCompoundSelector* owner, bool isSubject,
                                  bool siblingChain, bool siblingNext)
    {
      for (const auto& ss : compound.simples)
      {
        if (ss.kind == GlintSimpleKind::PSEUDO_CLASS)
        {
          const std::string low = lower(ss.name);
          if (isPositionalPseudo(low))
            pushUnique(childListDeps, owner, !owner || !siblingChain);
          else if (low == "empty")
          {
            // The parent itself, or the siblings following it. Below it
            // nothing can change: an empty parent has no descendants, and
            // children inserted into one are cascaded when they attach.
            if (owner && isSubject)  pushUnique(emptyDeps, owner, false);
            else if (!owner || siblingNext) pushUnique(emptyDeps, owner, true);
          }
        }
        const bool isHas = ss.kind == GlintSimpleKind::PSEUDO_CLASS && lower(ss.name) == "has";
        for (const auto& nested : ss.nestedSelectors)
        {
          if (!nested || isHas) continue;
          // The nested subject compound is matched against the owner's own
          // element; anything further left (or a sibling combinator) is not.
          for (size_t j = 0; j < nested->steps.size(); ++j)
          {
            const bool here = j == 0 && owner;
            addChildListDependencies(nested->steps[j].compound, here ? owner : nullptr,
                                     here && isSubject, here && siblingChain, here && siblingNext);
            if (j > 0 && isSiblingCombinator(nested->steps[j].combinator))
              pushUnique(childListDeps, nullptr, true);
          }
        }
      }
    }

    void addDependencies(const GlintCompoundSelector& compound, bool siblings,
                         bool adjacentOnly, const GlintCompoundSelector& owner)
    {
      for (const auto& ss : compound.simples)
      {
        if (ss.kind == GlintSimpleKind::PSEUDO_CLASS)
          pseudoDeps[lower(ss.name)].push_back({ &owner, siblings, adjacentOnly });
        else if (ss.kind == GlintSimpleKind::CLASS)
          classDeps[ss.name].push_back({ &owner, siblings, adjacentOnly });
        // Features inside :not() / :is() / :where() count for the owner too
        // (not :has(): its arguments match other elements; see hasDeps).
        if (ss.kind == GlintSimpleKind::PSEUDO_CLASS && lower(ss.name) == "has") continue;
        for (const auto& nested : ss.nestedSelectors)
          if (nested)
            for (const auto& st : nested->steps)
              addDependencies(st.compound, siblings, adjacentOnly, owner);
      }
    }

    static std::string lower(std::string s)
    {
      for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    }

    void add(const GlintCssQualifiedRule* rule, GlintCssOrigin origin)
    {
      const uint32_t idx = static_cast<uint32_t>(rules.size());
      rules.push_back({ rule, origin });
      auto push = [idx](std::vector<uint32_t>& bucket) {
        if (bucket.empty() || bucket.back() != idx) bucket.push_back(idx);
      };
      for (const auto& complexSel : rule->selectorList.selectors)
      {
        if (complexSel.steps.empty()) { push(universal); continue; }
        const auto& simples = complexSel.steps[0].compound.simples;
        const GlintSimpleSelector* idSel = nullptr;
        const GlintSimpleSelector* classSel = nullptr;
        const GlintSimpleSelector* typeSel = nullptr;
        for (const auto& ss : simples)
        {
          if      (ss.kind == GlintSimpleKind::ID    && !idSel)    idSel = &ss;
          else if (ss.kind == GlintSimpleKind::CLASS && !classSel) classSel = &ss;
          else if (ss.kind == GlintSimpleKind::TYPE  && !typeSel && ss.name != "*") typeSel = &ss;
        }
        if      (idSel)    push(byId[idSel->name]);
        else if (classSel) push(byClass[classSel->name]);
        else if (typeSel)  push(byTag[lower(typeSel->name)]);
        else               push(universal);

        // steps[k].combinator links compound k to compound k-1 (toward the
        // subject): a sibling combinator there puts the subject under a
        // following sibling of the element matching compound k.
        for (size_t k = 1; k < complexSel.steps.size(); ++k)
        {
          const auto comb = complexSel.steps[k].combinator;
          const bool adjacent = comb == GlintCombinator::ADJACENT_SIBLING;
          const bool siblings = adjacent || comb == GlintCombinator::GENERAL_SIBLING;
          addDependencies(complexSel.steps[k].compound, siblings, adjacent,
                          complexSel.steps[k].compound);
        }

        // Child-list features, compound by compound from the subject leftward.
        bool siblingChain = true;   // steps[1..k] are all `+` / `~`
        for (size_t k = 0; k < complexSel.steps.size(); ++k)
        {
          const bool siblingNext = k > 0 && isSiblingCombinator(complexSel.steps[k].combinator);
          if (siblingNext)
          {
            // `a + b` / `a ~ b`: compound k-1 is matched against an element
            // whose previous siblings it searches.
            pushUnique(childListDeps, &complexSel.steps[k - 1].compound, !siblingChain);
          }
          if (k > 0) siblingChain = siblingChain && siblingNext;
          addChildListDependencies(complexSel.steps[k].compound, &complexSel.steps[k].compound,
                                   k == 0, siblingChain, siblingNext);
        }

        for (size_t k = 0; k < complexSel.steps.size(); ++k)
          addHasDependencies(complexSel.steps[k].compound, &complexSel.steps[k].compound, k == 0,
                             k > 0 && isSiblingCombinator(complexSel.steps[k].combinator));
      }
    }

    static bool isSiblingCombinator(GlintCombinator c)
    {
      return c == GlintCombinator::ADJACENT_SIBLING || c == GlintCombinator::GENERAL_SIBLING;
    }
  };

  /** Lightweight match record: points into the stylesheet instead of copying. */
  struct FastMatch
  {
    const GlintCssDeclaration* decl;
    GlintCssSpecificity        specificity;
    int                        weight;       // weightOf(), computed once
    size_t                     sourceOrder;
  };

  /**
   * Winning declarations for `element`, in the order they must be applied —
   * identical to computeDeclarations() + inCascadeOrder() over the same rules
   * (no inline declarations), without copying declarations. Disabled
   * declarations are excluded, as in computeDeclarations().
   */
  static void computeWinnersIndexed(const GlintCssDomElement& element,
                                    const std::string& tagLower,
                                    const std::string& id,
                                    const std::vector<std::string>& classes,
                                    const RuleIndex& index,
                                    std::vector<uint32_t>& candidateScratch,
                                    std::vector<FastMatch>& matchScratch,
                                    std::vector<const GlintCssDeclaration*>& out)
  {
    out.clear();
    auto& cand = candidateScratch;
    cand.clear();
    auto addBucket = [&](const std::vector<uint32_t>* b) {
      if (b) cand.insert(cand.end(), b->begin(), b->end());
    };
    auto find = [](const std::unordered_map<std::string, std::vector<uint32_t>>& m,
                   const std::string& key) -> const std::vector<uint32_t>* {
      const auto it = m.find(key);
      return it == m.end() ? nullptr : &it->second;
    };
    if (!id.empty()) addBucket(find(index.byId, id));
    for (const auto& c : classes) addBucket(find(index.byClass, c));
    if (!tagLower.empty()) addBucket(find(index.byTag, tagLower));
    addBucket(&index.universal);
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

    auto& matched = matchScratch;
    matched.clear();
    size_t sourceOrder = 0;
    for (const uint32_t ri : cand)
    {
      const auto& entry = index.rules[ri];
      GLINT_PERF_INC(rulesTested);
      bool any = false;
      GlintCssSpecificity spec{};
      for (const auto& sel : entry.rule->selectorList.selectors)
      {
        if (!sel.matches(element)) continue;
        const GlintCssSpecificity s = sel.specificity();
        if (!any || spec < s) spec = s;
        any = true;
      }
      if (!any) continue;
      for (const auto& decl : entry.rule->declarations)
        matched.push_back({ &decl, spec, weightOf(entry.origin, decl.important, false), sourceOrder++ });
    }

    std::sort(matched.begin(), matched.end(),
      [](const FastMatch& a, const FastMatch& b)
      {
        if (a.weight != b.weight) return a.weight < b.weight;
        if (a.specificity.value() != b.specificity.value())
          return a.specificity.value() < b.specificity.value();
        return a.sourceOrder < b.sourceOrder;
      });

    // Last non-disabled declaration per property wins; apply winners in rank order.
    std::unordered_map<std::string_view, size_t> winner;
    winner.reserve(matched.size());
    for (size_t i = 0; i < matched.size(); ++i)
    {
      if (matched[i].decl->disabled) continue;
      winner[matched[i].decl->property] = i;
    }
    std::vector<size_t> ranks;
    ranks.reserve(winner.size());
    for (const auto& kv : winner) ranks.push_back(kv.second);
    std::sort(ranks.begin(), ranks.end());
    out.reserve(ranks.size());
    for (const size_t r : ranks) out.push_back(matched[r].decl);
  }

private:
  // ── Assign a numeric cascade weight for sorting ───────────────────────────
  // Higher = wins later (we sort ascending and take the last override).
  static int cascadeWeight(const GlintMatchedDeclaration& md)
  {
    return weightOf(md.origin, md.decl.important, md.isInline);
  }

  static int weightOf(GlintCssOrigin origin, bool important, bool isInline)
  {
    // Transitions win everything
    if (origin == GlintCssOrigin::TRANSITION) return 100;

    // !important user-agent is very high
    if (important && origin == GlintCssOrigin::USER_AGENT) return 80;

    // !important author
    if (important && (origin == GlintCssOrigin::AUTHOR   ||
                      origin == GlintCssOrigin::USER))    return 70;

    // Animations
    if (origin == GlintCssOrigin::ANIMATION) return 60;

    // Normal inline author
    if (isInline && !important) return 50;

    // Normal author / user
    if (origin == GlintCssOrigin::AUTHOR) return 40;
    if (origin == GlintCssOrigin::USER)   return 30;

    // Normal user-agent
    return 10;
  }

  // ── Collect matching declarations from one stylesheet ─────────────────────
  static void collectFromSheet(
    const GlintCssDomElement&     element,
    const GlintCssStylesheet&     sheet,
    GlintCssOrigin                origin,
    size_t&                      sourceOrder,
    std::vector<GlintMatchedDeclaration>& out,
    const GlintCssMediaContext*  media = nullptr)
  {
    std::vector<const GlintCssQualifiedRule*> rules;
    sheet.collectQualifiedRules(rules, media);

    for (const auto* rule : rules)
    {
      GLINT_PERF_INC(rulesTested);
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
