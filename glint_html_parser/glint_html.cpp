// glint_html.cpp — lexbor DOM → glint_element tree. See glint_html.hpp.

#include "glint_html.hpp"
#include "glint_html_ua.hpp"

#include "../glint_core.hpp"
#include "../components/glint_form.hpp"
#include "../components/glint_progress.hpp"
#include "../components/glint_radio.hpp"
#include "../components/glint_select.hpp"
#include "../components/glint_textarea.hpp"

#include <lexbor/html/html.h>
#include <lexbor/dom/dom.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

// ── lexbor helpers ───────────────────────────────────────────────────────────

std::string localName(lxb_dom_element_t* el)
{
  size_t len = 0;
  const lxb_char_t* s = lxb_dom_element_local_name(el, &len);
  return s ? std::string(reinterpret_cast<const char*>(s), len) : std::string();
}

bool getAttr(lxb_dom_element_t* el, const char* name, std::string& out)
{
  size_t len = 0;
  if (!lxb_dom_element_has_attribute(el, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name)))
    return false;
  const lxb_char_t* v = lxb_dom_element_get_attribute(
    el, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name), &len);
  out.assign(v ? reinterpret_cast<const char*>(v) : "", v ? len : 0);
  return true;
}

std::string attrOr(lxb_dom_element_t* el, const char* name, const std::string& fallback = {})
{
  std::string v;
  return getAttr(el, name, v) ? v : fallback;
}

std::string textContent(lxb_dom_node_t* node)
{
  size_t len = 0;
  lxb_char_t* s = lxb_dom_node_text_content(node, &len);
  return s ? std::string(reinterpret_cast<const char*>(s), len) : std::string();
}

std::string characterData(lxb_dom_node_t* node)
{
  const auto& d = lxb_dom_interface_character_data(node)->data;
  return std::string(reinterpret_cast<const char*>(d.data), d.length);
}

bool isWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

bool isAllWs(const std::string& s)
{
  for (char c : s) if (!isWs(c)) return false;
  return true;
}

std::string collapseWs(const std::string& s)
{
  std::string out;
  bool space = false;
  for (char c : s)
  {
    if (isWs(c)) { space = !out.empty(); continue; }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  return out;
}

float toFloat(const std::string& s, float fallback)
{
  if (s.empty()) return fallback;
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  return end == s.c_str() ? fallback : v;
}

int toInt(const std::string& s, int fallback)
{
  if (s.empty()) return fallback;
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  return end == s.c_str() ? fallback : static_cast<int>(v);
}

// ── URLs ─────────────────────────────────────────────────────────────────────

bool isAbsoluteUrl(const std::string& u)
{
  if (u.empty() || u[0] == '/' || u[0] == '\\' || u[0] == '#') return true;
  if (u.size() > 1 && u[1] == ':') return true;             // C:\...
  const auto colon = u.find(':');
  const auto slash = u.find('/');
  return colon != std::string::npos && (slash == std::string::npos || colon < slash);   // scheme:
}

// Resolve `ref` against the directory of `base`, folding "." and "..".
std::string resolveUrl(const std::string& base, const std::string& ref)
{
  if (base.empty() || isAbsoluteUrl(ref)) return ref;
  const auto cut = base.find_last_of("/\\");
  std::string joined = (cut == std::string::npos ? std::string() : base.substr(0, cut + 1)) + ref;

  // Keep any "scheme://host" prefix out of the segment folding.
  std::string prefix;
  const auto sep = joined.find("://");
  if (sep != std::string::npos)
  {
    const auto pathStart = joined.find('/', sep + 3);
    prefix = joined.substr(0, pathStart == std::string::npos ? joined.size() : pathStart);
    joined = pathStart == std::string::npos ? std::string() : joined.substr(pathStart);
  }
  std::vector<std::string> segs;
  std::string seg;
  const bool rooted = !joined.empty() && joined[0] == '/';
  for (size_t i = 0; i <= joined.size(); ++i)
  {
    if (i == joined.size() || joined[i] == '/' || joined[i] == '\\')
    {
      if (seg == "..") { if (!segs.empty() && segs.back() != "..") segs.pop_back(); else if (!rooted) segs.push_back(seg); }
      else if (!seg.empty() && seg != ".") segs.push_back(seg);
      seg.clear();
    }
    else seg += joined[i];
  }
  std::string out = prefix + (rooted ? "/" : "");
  for (size_t i = 0; i < segs.size(); ++i) out += (i ? "/" : "") + segs[i];
  return out;
}

// ── Tag classes ──────────────────────────────────────────────────────────────

bool isInlineLevelTag(const std::string& t)
{
  static const char* kTags[] = {
    "a", "abbr", "b", "bdi", "bdo", "big", "br", "button", "cite", "code", "data", "del",
    "dfn", "em", "font", "i", "img", "input", "ins", "kbd", "label", "mark", "meter",
    "output", "progress", "q", "s", "samp", "select", "small", "span", "strong", "sub",
    "sup", "textarea", "time", "tt", "u", "var" };
  for (const char* k : kTags) if (t == k) return true;
  return false;
}

// Never rendered. (Scripting is off, so <noscript> content does render.)
bool isNonRenderedTag(const std::string& t)
{
  return t == "script" || t == "template" || t == "head" || t == "meta";
}

bool isPreTag(const std::string& t)
{
  return t == "pre" || t == "textarea" || t == "listing" || t == "plaintext";
}

// The HTML UA sheet with [data-glint-html] appended to every selector, so its
// rules only reach elements the translator created (the document's UA sheet is
// shared with native glint pages). Commas inside (...) / [...] don't split.
std::string scopeUaSelectors(const std::string& css)
{
  static const std::string kScope = "[data-glint-html]";
  static const char* const kWs = " \t\r\n";
  std::string out;
  size_t i = 0;
  const size_t n = css.size();
  while (i < n)
  {
    if (css.compare(i, 2, "/*") == 0)   // comment
    {
      const size_t e = css.find("*/", i + 2);
      i = (e == std::string::npos) ? n : e + 2;
      continue;
    }
    const size_t brace = css.find('{', i);
    if (brace == std::string::npos) break;
    const size_t end = css.find('}', brace);
    if (end == std::string::npos) break;
    // Selector list -> each selector + scope.
    const std::string sel = css.substr(i, brace - i);
    std::string scoped;
    int depth = 0;
    size_t start = 0;
    auto flush = [&](size_t stop) {
      std::string s = sel.substr(start, stop - start);
      const size_t a = s.find_first_not_of(kWs);
      const size_t b = s.find_last_not_of(kWs);
      if (a == std::string::npos) return;
      if (!scoped.empty()) scoped += ", ";
      scoped += s.substr(a, b - a + 1) + kScope;
    };
    for (size_t k = 0; k < sel.size(); ++k)
    {
      const char c = sel[k];
      if (c == '(' || c == '[') ++depth;
      else if (c == ')' || c == ']') --depth;
      else if (c == ',' && depth == 0) { flush(k); start = k + 1; }
    }
    flush(sel.size());
    out += scoped + " " + css.substr(brace, end - brace + 1) + '\n';
    i = end + 1;
  }
  return out;
}

// ── Translator ───────────────────────────────────────────────────────────────

struct Translator
{
  glint_document*           doc = nullptr;   // null: detached fragment (no <style>/<link>)
  std::string               baseUrl;
  glint_html_options        opts;
  std::string               title;
  int                       preDepth  = 0;
  std::map<std::string, std::shared_ptr<glint_radio_group>> radioGroups;

  // One child slot of a glint element: a lexbor element or a run of text.
  struct Item
  {
    lxb_dom_element_t* el = nullptr;
    std::string        text;
    std::string        tag;   // el's local name
  };

  // ── head-ish elements (valid anywhere) ────────────────────────────────────
  // Returns true when `tag` was consumed here and must not be rendered.
  bool handleMetaElement(lxb_dom_element_t* el, const std::string& tag)
  {
    if (tag == "style")
    {
      if (doc) doc->addStylesheetText(textContent(lxb_dom_interface_node(el)),
                                      baseUrl.empty() ? "<style>" : baseUrl + " <style>");
      return true;
    }
    if (tag == "link")
    {
      std::string rel = attrOr(el, "rel"), href;
      for (auto& c : rel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (doc && rel.find("stylesheet") != std::string::npos && getAttr(el, "href", href) && !href.empty())
        doc->loadStylesheet(resolveUrl(baseUrl, href));
      return true;
    }
    if (tag == "title")
    {
      if (title.empty()) title = collapseWs(textContent(lxb_dom_interface_node(el)));
      return true;
    }
    if (tag == "base")
    {
      std::string href;
      if (getAttr(el, "href", href) && !href.empty()) baseUrl = resolveUrl(baseUrl, href);
      return true;
    }
    return isNonRenderedTag(tag);
  }

  // Gather `src`'s children as items: merges text, drops comments, flattens
  // table row groups into their table (glint tables want rows as children).
  void collectItems(lxb_dom_node_t* src, std::vector<Item>& items, bool flattenRowGroups)
  {
    for (lxb_dom_node_t* n = src->first_child; n; n = n->next)
    {
      if (n->type == LXB_DOM_NODE_TYPE_TEXT || n->type == LXB_DOM_NODE_TYPE_CDATA_SECTION)
      {
        if (!items.empty() && !items.back().el) items.back().text += characterData(n);
        else items.push_back({nullptr, characterData(n), {}});
        continue;
      }
      if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
      auto* el = lxb_dom_interface_element(n);
      if (n->ns != LXB_NS_HTML) continue;   // inline <svg> / <math>: not supported yet
      const std::string tag = localName(el);
      if (handleMetaElement(el, tag)) continue;
      if (flattenRowGroups && (tag == "thead" || tag == "tbody" || tag == "tfoot"))
      {
        collectItems(n, items, true);
        continue;
      }
      items.push_back({el, {}, tag});
    }
  }

  static bool itemIsInline(const Item& it)
  {
    return !it.el || isInlineLevelTag(it.tag);
  }

  glint_element* makeTextRun(std::string text, const char* name = "#text")
  {
    auto* run = new glint_element();
    run->typeNameOverride = name;
    run->style.display = "inline";
    run->innerText = std::move(text);
    return run;
  }

  std::string listMarker(lxb_dom_element_t* li, int& ordinal, int step, bool ordered)
  {
    std::string v;
    if (getAttr(li, "value", v)) ordinal = toInt(v, ordinal);
    if (ordered)
    {
      const int n = ordinal;
      ordinal += step;
      return std::to_string(n) + ".";
    }
    // Chrome's list-style-type by nesting: disc, then circle, then square.
    int depth = 0;
    for (lxb_dom_node_t* p = lxb_dom_interface_node(li)->parent; p; p = p->parent)
    {
      if (p->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
      const std::string t = localName(lxb_dom_interface_element(p));
      if (t == "ul" || t == "ol") ++depth;
    }
    return depth <= 1 ? "disc" : depth == 2 ? "circle" : "square";
  }

  // The <li>'s outside marker: a drawn shape (disc/circle/square: no glyphs
  // needed) or the number, positioned left of the content by the UA sheet.
  // Colored with the item's text color (glint has no currentColor).
  glint_element* makeMarker(const std::string& marker, const glint_element* li)
  {
    auto* m = new glint_element();
    m->typeNameOverride = "::marker";
    m->setAttribute("data-glint-html", "");
    const glint_color color = li->computedStyle.color.value;
    if (marker == "disc" || marker == "circle" || marker == "square")
    {
      m->className = "glint-marker-" + marker;
      if (marker == "circle") m->style.borderColor = color;
      else                    m->style.backgroundColor = color;
    }
    else
    {
      m->className = "glint-marker-num";
      m->innerText = marker;
    }
    return m;
  }

  // Translate src's children into dst. `marker` (for <li>) leads the content.
  void buildChildren(lxb_dom_node_t* src, glint_element* dst, const std::string& dstTag,
                     const std::string& marker = {})
  {
    std::vector<Item> items;
    collectItems(src, items, dstTag == "table");

    // Text-only content: the element carries it directly (no anonymous run).
    if (marker.empty() && items.size() == 1 && !items[0].el)
    {
      if (preDepth > 0 || !isAllWs(items[0].text))
        dst->innerText = std::move(items[0].text);
      return;
    }

    if (!marker.empty())
      dst->addChild(makeMarker(marker, dst));

    // <ol start reversed>: numbering of the <li> markers.
    const bool ordered  = dstTag == "ol";
    const bool reversed = ordered && boolAttr(lxb_dom_interface_element(src), "reversed");
    int liCount = 0;
    for (const auto& it : items) if (it.el && it.tag == "li") ++liCount;
    int ordinal = ordered ? toInt(attrOr(lxb_dom_interface_element(src), "start"), reversed ? liCount : 1) : 1;

    for (size_t i = 0; i < items.size(); ++i)
    {
      Item& it = items[i];
      if (!it.el)
      {
        // Whitespace-only runs matter only between two inline-level siblings
        // (or inside <pre>); elsewhere they would just open inline layout.
        if (preDepth == 0 && isAllWs(it.text))
        {
          const bool between = i > 0 && i + 1 < items.size() &&
                               itemIsInline(items[i - 1]) && itemIsInline(items[i + 1]);
          // In a flex/grid container whitespace-only text is not rendered
          // (each run would be an empty flex item, gap included).
          const auto& d = dst->computedStyle.display;
          const bool flexLike = d == "flex" || d == "inline-flex" || d == "grid" || d == "inline-grid";
          if (!between || flexLike) continue;
          it.text = " ";
        }
        dst->addChild(makeTextRun(std::move(it.text)));
        continue;
      }

      std::string m;
      if (opts.listMarkers && it.tag == "li" && (dstTag == "ul" || dstTag == "ol"))
        m = listMarker(it.el, ordinal, reversed ? -1 : 1, ordered);
      buildElement(it.el, it.tag, dst, m);
    }
  }

  // ── attributes ─────────────────────────────────────────────────────────────

  void applyCommonAttributes(lxb_dom_element_t* el, glint_element* node, const std::string& tag)
  {
    std::string inlineStyle;
    bool hasInlineStyle = false;
    for (lxb_dom_attr_t* a = lxb_dom_element_first_attribute(el); a; a = lxb_dom_element_next_attribute(a))
    {
      size_t nl = 0, vl = 0;
      const lxb_char_t* n = lxb_dom_attr_local_name(a, &nl);
      const lxb_char_t* v = lxb_dom_attr_value(a, &vl);
      std::string name(reinterpret_cast<const char*>(n), nl);
      std::string value(v ? reinterpret_cast<const char*>(v) : "", v ? vl : 0);
      if (name == "id")         node->id = value;
      else if (name == "class") node->className = value;
      else if (name == "style") { inlineStyle = std::move(value); hasInlineStyle = true; }
      else                      node->setAttribute(name, std::move(value));
    }

    // Presentational width/height (img, table cells...): below inline style.
    if (tag == "img" || tag == "td" || tag == "th" || tag == "table" || tag == "canvas" || tag == "video")
    {
      std::string w, h;
      auto dim = [](const std::string& s) { return s.find('%') != std::string::npos ? s : s + "px"; };
      if (getAttr(el, "width", w)  && !w.empty()) GlintCssApply::applyOne("width",  dim(w), node->style);
      if (getAttr(el, "height", h) && !h.empty()) GlintCssApply::applyOne("height", dim(h), node->style);
    }
    if (hasInlineStyle)
      // url()s in a style attribute are relative to the page.
      GlintCssApply::apply(GlintCssParser::parseInlineStyle(glint_document::resolveCssUrls(inlineStyle, baseUrl)),
                           node->style);
  }

  bool boolAttr(lxb_dom_element_t* el, const char* name)
  {
    return lxb_dom_element_has_attribute(el, reinterpret_cast<const lxb_char_t*>(name), std::strlen(name));
  }

  // ── components ─────────────────────────────────────────────────────────────
  // Returns true when the component owns its content (children not translated).

  bool setupComponent(lxb_dom_element_t* el, const std::string& tag, glint_element* node)
  {
    if (tag == "img")
    {
      auto* img = static_cast<glint_image*>(node);
      std::string src;
      if (getAttr(el, "src", src)) img->src = resolveUrl(baseUrl, src);
      return true;
    }
    if (tag == "input")
    {
      auto* in = static_cast<glint_input*>(node);
      std::string type = attrOr(el, "type", "text");
      for (auto& c : type) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      in->type         = type;
      node->setAttribute("type", type);   // <input> without type matches [type="text"]
      in->name         = attrOr(el, "name");
      in->placeholder  = attrOr(el, "placeholder");
      in->inputmode    = attrOr(el, "inputmode");
      in->enterkeyhint = attrOr(el, "enterkeyhint");
      in->autocomplete = attrOr(el, "autocomplete");
      in->autocapitalize = attrOr(el, "autocapitalize");
      in->spellcheck   = attrOr(el, "spellcheck");
      in->accept       = attrOr(el, "accept");
      in->pattern      = attrOr(el, "pattern");
      in->alt          = attrOr(el, "alt");
      std::string src;
      if (getAttr(el, "src", src)) in->src = resolveUrl(baseUrl, src);
      in->maxlength    = toInt(attrOr(el, "maxlength"), -1);
      in->minlength    = toInt(attrOr(el, "minlength"), -1);
      in->required     = boolAttr(el, "required");
      in->multiple     = boolAttr(el, "multiple");
      in->readonly     = boolAttr(el, "readonly");
      in->disabled     = boolAttr(el, "disabled");
      in->min          = toFloat(attrOr(el, "min"),  in->min);
      in->max          = toFloat(attrOr(el, "max"),  in->max);
      in->step         = toFloat(attrOr(el, "step"), in->step);

      std::string value;
      const bool hasValue = getAttr(el, "value", value);
      if (type == "checkbox" || type == "radio")
      {
        in->checked = boolAttr(el, "checked");
        in->checkSize = 13.f;   // Chrome's 13px box
        in->value   = hasValue ? value : "on";
        if (type == "radio" && !in->name.empty())
        {
          auto& g = radioGroups[in->name];
          if (!g) g = std::make_shared<glint_radio_group>();
          in->group = g;
        }
      }
      else if (hasValue && type != "file")
      {
        try { in->setValue(value); } catch (const std::exception&) {}
      }
      return true;
    }
    if (tag == "textarea")
    {
      auto* ta = static_cast<glint_textarea*>(node);
      ta->name         = attrOr(el, "name");
      ta->placeholder  = attrOr(el, "placeholder");
      ta->inputmode    = attrOr(el, "inputmode");
      ta->autocomplete = attrOr(el, "autocomplete");
      ta->spellcheck   = attrOr(el, "spellcheck");
      ta->maxlength    = toInt(attrOr(el, "maxlength"), -1);
      ta->minlength    = toInt(attrOr(el, "minlength"), -1);
      ta->required     = boolAttr(el, "required");
      ta->readonly     = boolAttr(el, "readonly");
      ta->disabled     = boolAttr(el, "disabled");
      ta->lineHeight   = 0.f;   // normal: the font's own line spacing, as in browsers
      // rows (default 2) -> height as Chrome sizes it: 15px lines of the UA's
      // 13.33px monospace, 2px padding, 1px border; unless the inline style sizes it.
      if (node->style.height.raw.empty())
      {
        const int n = std::max(1, toInt(attrOr(el, "rows", "2"), 2));
        GlintCssApply::applyOne("height", std::to_string(n * 15 + 6) + "px", node->style);
      }
      return true;   // value: set after attach (buildElement)
    }
    if (tag == "select")
    {
      auto* sel = static_cast<glint_select*>(node);
      sel->name     = attrOr(el, "name");
      sel->disabled = boolAttr(el, "disabled");
      // <option>s, including those inside <optgroup>, in document order.
      std::function<void(lxb_dom_node_t*)> walk = [&](lxb_dom_node_t* p) {
        for (lxb_dom_node_t* n = p->first_child; n; n = n->next)
        {
          if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
          auto* oe = lxb_dom_interface_element(n);
          const std::string t = localName(oe);
          if (t == "optgroup") { walk(n); continue; }
          if (t != "option") continue;
          std::string label;
          if (!getAttr(oe, "label", label) || label.empty()) label = collapseWs(textContent(n));
          if (boolAttr(oe, "selected")) sel->selectedIndex = static_cast<int>(sel->options.size());
          sel->options.push_back(std::move(label));
        }
      };
      walk(lxb_dom_interface_node(el));
      if (sel->selectedIndex < 0 && !sel->options.empty()) sel->selectedIndex = 0;
      return true;
    }
    if (tag == "button")
    {
      node->innerText = collapseWs(textContent(lxb_dom_interface_node(el)));
      return true;
    }
    if (tag == "progress")
    {
      auto* p = static_cast<glint_progress*>(node);
      std::string v;
      p->max   = std::max(0.f, toFloat(attrOr(el, "max"), 1.f));
      p->value = getAttr(el, "value", v) ? std::max(0.f, toFloat(v, 0.f)) : -1.f;
      // Chrome's native bar (glint_html_root_css): half the box height, outlined.
      p->trackClassName = "glint-html-progress-track";
      p->fillClassName  = "glint-html-progress-fill";
      return true;
    }
    if (tag == "meter")
    {
      auto* m = static_cast<glint_meter*>(node);
      const float lo = toFloat(attrOr(el, "min"), 0.f);
      const float hi = std::max(lo, toFloat(attrOr(el, "max"), 1.f));
      auto norm = [&](float x) { return hi > lo ? std::min(1.f, std::max(0.f, (x - lo) / (hi - lo))) : 0.f; };
      m->value   = norm(toFloat(attrOr(el, "value"), lo));
      m->low     = norm(toFloat(attrOr(el, "low"), lo));
      m->high    = norm(toFloat(attrOr(el, "high"), hi));
      m->optimum = norm(toFloat(attrOr(el, "optimum"), (lo + hi) * 0.5f));
      // Chrome's native bar, dark scheme: half the box height, outlined.
      m->barFraction     = 0.5f;
      m->outlineColor    = "#858585";
      m->trackColor      = "#3b3b3b";
      m->radius          = 2.f;
      m->colorOptimal    = "#74b374";
      m->colorSuboptimal = "#f2c812";
      m->colorBad        = "#e98f6d";
      return true;
    }
    return false;
  }

  glint_element* createNode(const std::string& tag)
  {
    if (tag == "textarea") return new glint_textarea();
    if (tag == "progress") return new glint_progress();
    if (tag == "meter")    return new glint_meter();
    if (tag == "input" || tag == "img" || tag == "button" || tag == "select" || tag == "form" ||
        tag.find('-') != std::string::npos)   // custom elements: registerElement("my-tag", ...)
    {
      glint_element* n = glint_element::createElement(tag);
      if (tag.find('-') != std::string::npos && n->typeNameOverride.empty() &&
          std::string(n->typeName()) == "div")
        n->typeNameOverride = tag;   // unregistered custom tag: plain element named after it
      return n;
    }
    auto* n = new glint_element();
    n->typeNameOverride = tag;
    return n;
  }

  void buildElement(lxb_dom_element_t* el, const std::string& tag, glint_element* parent,
                    const std::string& marker = {})
  {
    if (tag == "br")
    {
      // Forced line break: a pre-line newline run.
      auto* br = makeTextRun("\n", "br");
      br->style.whiteSpace = "pre-line";
      applyCommonAttributes(el, br, tag);
      br->setAttribute("data-glint-html", "");
      parent->addChild(br);
      return;
    }

    glint_element* node = createNode(tag);
    applyCommonAttributes(el, node, tag);
    node->setAttribute("data-glint-html", "");   // UA sheet scope
    const bool ownsContent = setupComponent(el, tag, node);

    // Attach before translating children: each node then cascades once, with
    // its ancestors (descendant selectors) already in place.
    node->syncBeforeLayout();
    node->computedStyle = node->mergedStyleForLayout();
    parent->addChild(node);
    // The text editor builds its document on attach; a value set earlier is lost.
    if (tag == "textarea")
      static_cast<glint_textarea*>(node)->setValue(textContent(lxb_dom_interface_node(el)));
    if (ownsContent) return;

    const bool pre = isPreTag(tag);
    preDepth += pre;
    buildChildren(lxb_dom_interface_node(el), node, tag, marker);
    preDepth -= pre;
  }

  void ensureUaSheet()
  {
    if (doc && opts.applyHtmlUaSheet && !doc->htmlUserAgentSheetApplied)
    {
      doc->htmlUserAgentSheetApplied = true;
      doc->appendUserAgentStylesheetText(scopeUaSelectors(glint_html_ua_css) + glint_html_root_css);
    }
  }
};

std::string parentContextTag(glint_element* parent)
{
  // lexbor needs an HTML context element; glint component names (list, tree...)
  // and anonymous nodes parse like a <div>.
  std::string t = parent->tagName() ? parent->tagName() : "";
  static const char* kContexts[] = {
    "body", "div", "span", "p", "section", "article", "header", "footer", "main", "nav",
    "aside", "ul", "ol", "li", "table", "tbody", "tr", "td", "th", "form", "label", "a",
    "pre", "blockquote", "select", "button", "dl", "dd", "dt", "figure" };
  for (const char* k : kContexts) if (t == k) return t;
  return "div";
}

glint_html_result parseFragmentInto(glint_element* parent, std::string_view html,
                                    const glint_html_options& opts)
{
  glint_html_result res;
  if (!parent) { res.error = "glint_html: null parent"; return res; }

  lxb_html_document_t* ldoc = lxb_html_document_create();
  if (!ldoc) { res.error = "glint_html: lexbor document allocation failed"; return res; }
  if (lxb_html_document_parse(ldoc, reinterpret_cast<const lxb_char_t*>(""), 0) != LXB_STATUS_OK)
  {
    lxb_html_document_destroy(ldoc);
    res.error = "glint_html: lexbor init failed";
    return res;
  }

  const std::string ctxTag = parentContextTag(parent);
  lxb_dom_element_t* ctxEl = lxb_dom_document_create_element(
    &ldoc->dom_document, reinterpret_cast<const lxb_char_t*>(ctxTag.data()), ctxTag.size(), nullptr);
  lxb_dom_node_t* root = ctxEl ? lxb_html_document_parse_fragment(
    ldoc, ctxEl, reinterpret_cast<const lxb_char_t*>(html.data()), html.size()) : nullptr;
  if (!root)
  {
    lxb_html_document_destroy(ldoc);
    res.error = "glint_html: fragment parse failed";
    return res;
  }

  Translator t;
  t.doc  = parent->mRoot;
  t.opts = opts;
  t.ensureUaSheet();
  t.preDepth  = isPreTag(ctxTag) ? 1 : 0;
  const size_t firstNew = parent->mChildren.size();
  t.buildChildren(root, parent, ctxTag);
  // Mark the new top-level elements: the UA sheet gives HTML content the
  // browser's initial values glint's own defaults differ from (line-height).
  for (size_t i = firstNew; i < parent->mChildren.size(); ++i)
    parent->mChildren[i]->setAttribute("data-glint-html-root", "");

  lxb_html_document_destroy(ldoc);
  res.ok = true;
  res.title = std::move(t.title);
  return res;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

glint_html_result glint_html_set_inner_html(glint_element* parent, std::string_view html,
                                            const glint_html_options& opts)
{
  if (parent)
  {
    parent->clearChildren();
    parent->innerText.clear();
  }
  return parseFragmentInto(parent, html, opts);
}

glint_html_result glint_html_append(glint_element* parent, std::string_view html,
                                    const glint_html_options& opts)
{
  return parseFragmentInto(parent, html, opts);
}

glint_html_result glint_html_load_string(glint_document& doc, std::string_view html,
                                         glint_element* target, const std::string& baseUrl,
                                         const glint_html_options& opts)
{
  glint_html_result res;
  if (!target) target = &doc.mCanvas;

  lxb_html_document_t* ldoc = lxb_html_document_create();
  if (!ldoc) { res.error = "glint_html: lexbor document allocation failed"; return res; }
  if (lxb_html_document_parse(ldoc, reinterpret_cast<const lxb_char_t*>(html.data()), html.size()) != LXB_STATUS_OK)
  {
    lxb_html_document_destroy(ldoc);
    res.error = "glint_html: parse failed";
    return res;
  }

  Translator t;
  t.doc     = &doc;
  t.baseUrl = baseUrl;
  t.opts    = opts;
  t.ensureUaSheet();

  // Head first: stylesheets are in place before the body cascades.
  if (auto* head = lxb_html_document_head_element(ldoc))
  {
    std::vector<Translator::Item> ignored;
    t.collectItems(lxb_dom_interface_node(head), ignored, false);
  }

  if (auto* body = lxb_html_document_body_element(ldoc))
  {
    auto* bodyEl = lxb_dom_interface_element(body);
    std::string v;
    if (getAttr(bodyEl, "id", v) && target->id.empty()) target->id = v;
    if (getAttr(bodyEl, "class", v) && !v.empty())
      target->className = target->className.empty() ? v : std::string(target->className) + " " + v;
    if (getAttr(bodyEl, "style", v))
    {
      GlintCssApply::apply(GlintCssParser::parseInlineStyle(glint_document::resolveCssUrls(v, baseUrl)), target->style);
      target->notifyStyleChanged();
    }
    target->setAttribute("data-glint-html-root", "");   // see parseFragmentInto
    t.buildChildren(lxb_dom_interface_node(body), target, "body");
  }

  lxb_html_document_destroy(ldoc);
  res.ok = true;
  res.title = std::move(t.title);

  // `target` is the page's <body>: keys pressed with nothing focused go to it.
  doc.setBodyElement(target == &doc.mCanvas ? nullptr : target);

  // The tree is built and styled: listeners on `target` set up the page
  // (querySelector + addEventListener), here and on every reload.
  glint_event loaded;
  loaded.type = "DOMContentLoaded";
  target->dispatchDOMEvent(loaded);
  return res;
}

glint_html_result glint_html_load(glint_document& doc, const std::string& url,
                                  glint_element* target, const glint_html_options& opts)
{
  glint_resource_request req;
  req.url    = url;
  req.type   = glint_resource_request::Type::Document;
  req.source = nullptr;
  req.parseUrl();
  if (doc.onRequest) doc.onRequest(req);

  std::string text;
  if (req.handled && req.statusCode == 200 && req.responseData)
  {
    text.assign(static_cast<const char*>(req.responseData->data()), req.responseData->size());
    glint_network_log_push(&doc.networkLog, url, req.type, req);
  }
  else if (!req.handled)
  {
    std::ifstream f(url, std::ios::binary);
    if (!f.is_open())
    {
      glint_network_log_push_disk(&doc.networkLog, url, req.type, false);
      glint_html_result res;
      res.error = "glint_html_load: cannot open \"" + url + "\"";
      return res;
    }
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    glint_network_log_push_disk(&doc.networkLog, url, req.type, true);
  }
  else
  {
    glint_network_log_push(&doc.networkLog, url, req.type, req);
    glint_html_result res;
    res.error = "glint_html_load: request error (" + std::to_string(req.statusCode) + ") for \"" +
                url + "\": " + req.statusMessage;
    return res;
  }
  return glint_html_load_string(doc, text, target, url, opts);
}
