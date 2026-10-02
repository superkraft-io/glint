#pragma once

/**
 * glint_css_media.hpp
 * Media query evaluation for @media rules (Media Queries Level 4 subset).
 *
 *   GlintCssMediaContext ctx{ 800.f, 600.f };          // viewport in CSS px
 *   glint_css_media_matches("screen and (min-width: 600px)", ctx);   // true
 *   glint_css_media_matches("(400px <= width < 700px)", ctx);        // false
 *
 * Supported:
 *   - query lists (comma = or), `not` / `only`, media types all / screen
 *     (print / speech never match), `and` / `or` / `not` conditions, nesting
 *   - width, height (+ min- / max-), aspect-ratio, orientation, resolution,
 *     prefers-color-scheme, prefers-reduced-motion, hover / any-hover,
 *     pointer / any-pointer, color, and the range syntax
 *     (width >= 600px), (400px <= width < 800px)
 *   - lengths in px, em / rem (16px), pt, pc, in, cm, mm, Q, vw / vh
 * Unknown features and media types evaluate to false, as the spec requires.
 */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

struct GlintCssMediaContext
{
  float width  = 0.f;               // viewport width in CSS px
  float height = 0.f;               // viewport height in CSS px
  float dpr    = 1.f;               // device pixels per CSS px (resolution, dppx)
  bool  darkColorScheme = false;    // prefers-color-scheme: dark

  bool operator==(const GlintCssMediaContext& o) const
  {
    return width == o.width && height == o.height && dpr == o.dpr
        && darkColorScheme == o.darkColorScheme;
  }
  bool operator!=(const GlintCssMediaContext& o) const { return !(*this == o); }
};

namespace glint_css_media_detail
{
  inline std::string trim(const std::string& s)
  {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
  }

  // Lower-case and drop /* comments */.
  inline std::string normalize(const std::string& in)
  {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
      if (in[i] == '/' && i + 1 < in.size() && in[i + 1] == '*')
      {
        const size_t end = in.find("*/", i + 2);
        if (end == std::string::npos) break;
        i = end + 1;
        out += ' ';
        continue;
      }
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(in[i])));
    }
    return out;
  }

  // Split on `sep` outside parentheses.
  inline std::vector<std::string> splitTopLevel(const std::string& s, char sep)
  {
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    for (char c : s)
    {
      if (c == '(') ++depth;
      else if (c == ')' && depth > 0) --depth;
      if (c == sep && depth == 0) { out.push_back(trim(cur)); cur.clear(); continue; }
      cur += c;
    }
    out.push_back(trim(cur));
    return out;
  }

  // Tokens of a query/condition: words, and parenthesized groups kept whole
  // (with their parentheses).
  inline std::vector<std::string> tokenize(const std::string& s)
  {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size())
    {
      if (std::isspace(static_cast<unsigned char>(s[i]))) { ++i; continue; }
      if (s[i] == '(')
      {
        int depth = 0;
        const size_t start = i;
        for (; i < s.size(); ++i)
        {
          if (s[i] == '(') ++depth;
          else if (s[i] == ')' && --depth == 0) { ++i; break; }
        }
        out.push_back(s.substr(start, i - start));
        continue;
      }
      const size_t start = i;
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i])) && s[i] != '(') ++i;
      out.push_back(s.substr(start, i - start));
    }
    return out;
  }

  // A length, resolution or ratio value; returns false if it can't be read.
  inline bool parseLength(const std::string& v, const GlintCssMediaContext& ctx, float& out)
  {
    const std::string t = trim(v);
    char* end = nullptr;
    const float n = std::strtof(t.c_str(), &end);
    if (end == t.c_str()) return false;
    const std::string unit = trim(std::string(end));
    if (unit.empty() || unit == "px") { out = n; return true; }
    if (unit == "em" || unit == "rem") { out = n * 16.f; return true; }   // initial font-size
    if (unit == "pt") { out = n * 96.f / 72.f; return true; }
    if (unit == "pc") { out = n * 16.f; return true; }
    if (unit == "in") { out = n * 96.f; return true; }
    if (unit == "cm") { out = n * 96.f / 2.54f; return true; }
    if (unit == "mm") { out = n * 96.f / 25.4f; return true; }
    if (unit == "q")  { out = n * 96.f / 101.6f; return true; }
    if (unit == "vw") { out = n * ctx.width / 100.f; return true; }
    if (unit == "vh") { out = n * ctx.height / 100.f; return true; }
    return false;
  }

  inline bool parseResolution(const std::string& v, float& out)   // → dppx
  {
    const std::string t = trim(v);
    char* end = nullptr;
    const float n = std::strtof(t.c_str(), &end);
    if (end == t.c_str()) return false;
    const std::string unit = trim(std::string(end));
    if (unit == "dppx" || unit == "x") { out = n; return true; }
    if (unit == "dpi")  { out = n / 96.f; return true; }
    if (unit == "dpcm") { out = n * 2.54f / 96.f; return true; }
    return false;
  }

  inline bool parseRatio(const std::string& v, float& out)          // "16/9" or "1.5"
  {
    const size_t slash = v.find('/');
    const float a = std::strtof(trim(v.substr(0, slash)).c_str(), nullptr);
    float b = 1.f;
    if (slash != std::string::npos) b = std::strtof(trim(v.substr(slash + 1)).c_str(), nullptr);
    if (b <= 0.f) return false;
    out = a / b;
    return true;
  }

  // Numeric value of a range-capable feature, and how to read a value for it.
  enum class Kind { Length, Ratio, Resolution, None };
  inline Kind featureValue(const std::string& name, const GlintCssMediaContext& ctx, float& out)
  {
    if (name == "width")        { out = ctx.width;  return Kind::Length; }
    if (name == "height")       { out = ctx.height; return Kind::Length; }
    if (name == "aspect-ratio") { out = ctx.height > 0.f ? ctx.width / ctx.height : 0.f; return Kind::Ratio; }
    if (name == "resolution")   { out = ctx.dpr;    return Kind::Resolution; }
    return Kind::None;
  }

  inline bool readValue(Kind kind, const std::string& v, const GlintCssMediaContext& ctx, float& out)
  {
    switch (kind)
    {
      case Kind::Length:     return parseLength(v, ctx, out);
      case Kind::Ratio:      return parseRatio(v, out);
      case Kind::Resolution: return parseResolution(v, out);
      default:               return false;
    }
  }

  inline bool compare(float a, const std::string& op, float b)
  {
    const float eps = 1e-4f;
    if (op == "<")  return a <  b - eps;
    if (op == "<=") return a <= b + eps;
    if (op == ">")  return a >  b + eps;
    if (op == ">=") return a >= b - eps;
    if (op == "=")  return std::abs(a - b) <= eps;
    return false;
  }

  // Discrete features (`name: value`, or `name` alone in boolean context).
  inline bool discreteFeature(const std::string& name, const std::string& value,
                              bool hasValue, const GlintCssMediaContext& ctx)
  {
    if (name == "orientation")
    {
      const bool portrait = ctx.height >= ctx.width;
      return !hasValue || value == (portrait ? "portrait" : "landscape");
    }
    if (name == "prefers-color-scheme")
      return !hasValue || value == (ctx.darkColorScheme ? "dark" : "light");
    if (name == "prefers-reduced-motion")
      return hasValue ? value == "no-preference" : false;
    if (name == "hover" || name == "any-hover")
      return !hasValue || value == "hover";          // desktop: a mouse can hover
    if (name == "pointer" || name == "any-pointer")
      return !hasValue || value == "fine";
    if (name == "color")
      return true;                                   // a color display
    if (name == "monochrome" || name == "grid")
      return hasValue ? std::strtof(value.c_str(), nullptr) == 0.f : false;
    return false;                                    // unknown feature
  }

  // One media feature, the contents of `( ... )` without the parentheses.
  inline bool feature(const std::string& raw, const GlintCssMediaContext& ctx)
  {
    const std::string f = trim(raw);

    // Range syntax: find comparison operators outside a value.
    std::vector<std::string> parts, ops;
    {
      std::string cur;
      for (size_t i = 0; i < f.size(); ++i)
      {
        const char c = f[i];
        if (c == '<' || c == '>' || c == '=')
        {
          std::string op(1, c);
          if ((c == '<' || c == '>') && i + 1 < f.size() && f[i + 1] == '=') { op += '='; ++i; }
          parts.push_back(trim(cur));
          ops.push_back(op);
          cur.clear();
          continue;
        }
        cur += c;
      }
      parts.push_back(trim(cur));
    }
    if (!ops.empty())
    {
      // name op value | value op name | value op name op value
      auto isName = [&](const std::string& p) {
        float tmp;
        return featureValue(p, ctx, tmp) != Kind::None;
      };
      if (ops.size() == 1)
      {
        float fv, v;
        if (isName(parts[0]))
        {
          const Kind k = featureValue(parts[0], ctx, fv);
          return readValue(k, parts[1], ctx, v) && compare(fv, ops[0], v);
        }
        if (isName(parts[1]))
        {
          const Kind k = featureValue(parts[1], ctx, fv);
          return readValue(k, parts[0], ctx, v) && compare(v, ops[0], fv);
        }
        return false;
      }
      if (ops.size() == 2 && isName(parts[1]))
      {
        float fv, lo, hi;
        const Kind k = featureValue(parts[1], ctx, fv);
        return readValue(k, parts[0], ctx, lo) && readValue(k, parts[2], ctx, hi)
            && compare(lo, ops[0], fv) && compare(fv, ops[1], hi);
      }
      return false;
    }

    // `name: value` or boolean `name`.
    const size_t colon = f.find(':');
    const std::string name  = trim(f.substr(0, colon));
    const std::string value = colon == std::string::npos ? std::string() : trim(f.substr(colon + 1));
    const bool hasValue = colon != std::string::npos;

    std::string base = name;
    int bound = 0;   // -1 = min-, +1 = max-
    if (base.rfind("min-", 0) == 0) { base = base.substr(4); bound = -1; }
    else if (base.rfind("max-", 0) == 0) { base = base.substr(4); bound = +1; }

    float fv;
    const Kind k = featureValue(base, ctx, fv);
    if (k != Kind::None)
    {
      if (!hasValue) return bound == 0 && fv != 0.f;   // boolean context
      float v;
      if (!readValue(k, value, ctx, v)) return false;
      if (bound < 0) return compare(fv, ">=", v);
      if (bound > 0) return compare(fv, "<=", v);
      return compare(fv, "=", v);
    }
    if (bound != 0) return true;                        // min-color etc.: color display
    return discreteFeature(base, value, hasValue, ctx);
  }

  inline bool condition(const std::vector<std::string>& toks, size_t& i, const GlintCssMediaContext& ctx);

  // `(feature)`, `(condition)` or `not (...)`.
  inline bool conditionTerm(const std::vector<std::string>& toks, size_t& i, const GlintCssMediaContext& ctx)
  {
    if (i >= toks.size()) return false;
    if (toks[i] == "not") { ++i; return !conditionTerm(toks, i, ctx); }
    const std::string& t = toks[i++];
    if (t.size() < 2 || t.front() != '(' || t.back() != ')') return false;
    const std::string inner = t.substr(1, t.size() - 2);
    const auto innerToks = tokenize(inner);
    if (!innerToks.empty() && (innerToks[0] == "not" || innerToks[0].front() == '('))
    {
      size_t j = 0;
      return condition(innerToks, j, ctx);              // nested condition
    }
    return feature(inner, ctx);
  }

  // term (and term)* | term (or term)*
  inline bool condition(const std::vector<std::string>& toks, size_t& i, const GlintCssMediaContext& ctx)
  {
    bool result = conditionTerm(toks, i, ctx);
    while (i < toks.size() && (toks[i] == "and" || toks[i] == "or"))
    {
      const bool isAnd = toks[i] == "and";
      ++i;
      const bool rhs = conditionTerm(toks, i, ctx);
      result = isAnd ? (result && rhs) : (result || rhs);
    }
    return result;
  }

  // One media query: [not|only] [type] [and condition] | condition
  inline bool query(const std::string& q, const GlintCssMediaContext& ctx)
  {
    const auto toks = tokenize(q);
    if (toks.empty()) return true;                      // `@media {}` / `@media all`
    size_t i = 0;
    bool negate = false;
    if (toks[i] == "not" && i + 1 < toks.size() && toks[i + 1].front() != '(') { negate = true; ++i; }
    else if (toks[i] == "only") ++i;

    bool result = true;
    if (i < toks.size() && toks[i].front() != '(' && toks[i] != "not")
    {
      const std::string& type = toks[i++];
      result = (type == "all" || type == "screen");
      if (i < toks.size())
      {
        if (toks[i] != "and") return false;             // malformed
        ++i;
        result = condition(toks, i, ctx) && result;
      }
    }
    else
    {
      result = condition(toks, i, ctx);
    }
    if (i != toks.size()) return false;                 // trailing garbage: invalid query
    return negate ? !result : result;
  }
}

/** True when the media query list (an @media prelude) matches `ctx`. */
inline bool glint_css_media_matches(const std::string& queryList, const GlintCssMediaContext& ctx)
{
  using namespace glint_css_media_detail;
  const std::string norm = trim(normalize(queryList));
  if (norm.empty()) return true;
  for (const auto& q : splitTopLevel(norm, ','))
    if (query(q, ctx)) return true;
  return false;
}
