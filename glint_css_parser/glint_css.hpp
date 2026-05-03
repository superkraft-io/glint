#pragma once

/**
 * glint_css.hpp
 * Umbrella header — include this to get the full CSS parser for glint.
 *
 * Pulls in (in dependency order):
 *   glint_css_token.hpp      — GlintCssToken, GlintCssTokenType
 *   glint_css_tokenizer.hpp  — GlintCssTokenizer
 *   glint_css_selector.hpp   — GlintSelectorList, GlintCssSpecificity, GlintCssDomElement
 *   glint_css_rule.hpp       — GlintCssStylesheet, GlintCssQualifiedRule, GlintCssAtRule, GlintCssDeclaration
 *   glint_css_parser.hpp     — GlintCssParser
 *   glint_css_cascade.hpp    — GlintCssCascade
 *   glint_css_apply.hpp      — GlintCssApply
 *
 * Quick-start examples
 * ──────────────────────
 *
 * 1. Apply an inline style string to a glint_style:
 *
 *   #include "glint_css_parser/glint_css.hpp"
 *
 *   auto decls = GlintCssParser::parseInlineStyle(
 *       "color: #fff; background-color: #1a1a1a; font-size: 14px; padding: 8px 12px");
 *   GlintCssApply::apply(decls, el.style);
 *
 *
 * 2. Parse a full stylesheet and apply it via the cascade:
 *
 *   // Parse once (e.g. at startup or on hot-reload)
 *   GlintCssStylesheet sheet = GlintCssParser::parseStylesheet(cssText);
 *
 *   // For each element, resolve + apply:
 *   //   MyDomAdapter wraps a glint_element to implement GlintCssDomElement
 *   MyDomAdapter domEl(*el);
 *   auto decls = GlintCssCascade::resolve(domEl, {&sheet}, {});
 *   GlintCssApply::apply(decls, el->style);
 *
 *
 * 3. Parse and match a selector manually:
 *
 *   GlintSelectorList sel = GlintCssParser::parseSelector(".panel > .header:hover");
 *   if (sel.matches(domEl))
 *     GlintCssApply::applyOne("background-color", "#2a2a2a", el->style);
 *
 *
 * DOM adapter
 * ───────────
 * To use the cascade or selector matching with live glint_elements, implement
 * GlintCssDomElement for your element type.  A minimal adapter:
 *
 *   struct MyDomAdapter : GlintCssDomElement
 *   {
 *     explicit MyDomAdapter(const glint_element& el) : mEl(el) {}
 *
 *     std::string tagName() const override { return mEl.element.tagName; }
 *     std::string id()      const override { return mEl.id; }
 *     std::vector<std::string> classNames() const override
 *     {
 *       std::vector<std::string> cls;
 *       std::istringstream ss(mEl.element.className);
 *       std::string tok;
 *       while (ss >> tok) cls.push_back(tok);
 *       return cls;
 *     }
 *     std::string attribute(const std::string& name, bool& found) const override
 *     {
 *       found = false; return {};
 *     }
 *     bool pseudoState(const std::string& p) const override
 *     {
 *       if (p == "hover")  return mEl.mHovered;
 *       if (p == "active") return mEl.mActive;
 *       if (p == "focus")  return mEl.mFocused;
 *       return false;
 *     }
 *     const GlintCssDomElement* parent()          const override { return nullptr; }
 *     size_t childIndex()                         const override { return 0; }
 *     size_t siblingCount()                       const override { return 1; }
 *     size_t typeChildIndex()                     const override { return 0; }
 *     size_t typeSiblingCount()                   const override { return 1; }
 *     bool   isRoot()                             const override { return mEl.mParent == nullptr; }
 *     bool   isEmpty()                            const override { return mEl.mChildren.empty(); }
 *   private:
 *     const glint_element& mEl;
 *   };
 */

#include "glint_css_token.hpp"
#include "glint_css_tokenizer.hpp"
#include "glint_css_selector.hpp"
#include "glint_css_rule.hpp"
#include "glint_css_parser.hpp"
#include "glint_css_cascade.hpp"
#include "glint_css_apply.hpp"
