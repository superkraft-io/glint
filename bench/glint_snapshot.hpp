#pragma once

/**
 * glint_snapshot.hpp
 * Text dump of a laid-out tree, used as a regression gate for reflow changes.
 *
 * One line per element (depth-first), plus one line per rendered text line:
 *
 *   <indent>type#id.class rect=L,T,R,B paint=L,T,R,B scroll=W,H,top,left
 *            display=.. pos=.. w=.. color=.. bg=.. op=..
 *   <indent>  | line x,top,base w "text"
 *
 * Numbers are printed with %.2f so two runs of the same build are identical
 * byte-for-byte. Compare two dumps with glint_snapshot_compare().
 */

#include "glint/glint_standalone.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace glint_snapshot
{

inline void _appendf(std::string& out, const char* fmt, ...)
{
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) out.append(buf, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(buf) - 1))));
}

inline std::string _colorHex(const sk_color& c)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X",
                c.value.A & 0xFF, c.value.R & 0xFF, c.value.G & 0xFF, c.value.B & 0xFF);
  return buf;
}

inline void _dumpElement(const glint_element& e, int depth, std::string& out)
{
  const std::string indent(static_cast<size_t>(depth) * 2, ' ');
  const glint_style& cs = e.computedStyle;
  const glint_rect   pr = e.GetPaintRECT();

  _appendf(out, "%s%s", indent.c_str(), e.typeName() ? e.typeName() : "?");
  if (!e.id.empty()) _appendf(out, "#%s", e.id.c_str());
  if (!e.className.empty()) _appendf(out, ".%s", e.className.c_str());
  _appendf(out, " rect=%.2f,%.2f,%.2f,%.2f paint=%.2f,%.2f,%.2f,%.2f scroll=%.2f,%.2f,%.2f,%.2f",
           e.mRect.L, e.mRect.T, e.mRect.R, e.mRect.B,
           pr.L, pr.T, pr.R, pr.B,
           e.mScrollWidth, e.mScrollHeight, e.mScrollTop, e.mScrollLeft);
  _appendf(out, " display=%s pos=%s w=%s color=%s bg=%s op=%.3f\n",
           cs.display.c_str(), cs.position.c_str(), cs.width.raw.c_str(),
           _colorHex(cs.color).c_str(), _colorHex(cs.backgroundColor).c_str(),
           static_cast<double>(cs.opacity.value));

  if (!e.innerText.empty() && cs.display != "none")
  {
    const float sz = cs.fontSize.toFloat() > 0.f ? cs.fontSize.toFloat() : 12.f;
    const SkFont font = glint_element::skFont(sz, cs.fontFamily.c_str(),
                                              static_cast<int>(cs.fontWeight.value),
                                              cs.fontStyle.c_str());
    for (const auto& ln : e._buildRenderLines(font))
    {
      _appendf(out, "%s  | line %.2f,%.2f,%.2f w=%.2f \"%s\"\n", indent.c_str(),
               ln.x, ln.top, ln.baselineY, ln.width, ln.text.c_str());
    }
  }

  for (const auto& c : e.mChildren)
    _dumpElement(*c, depth + 1, out);
}

/** Dump the whole document tree. Call after at least one frame has run. */
inline std::string dump(glint_document& doc)
{
  std::string out;
  out.reserve(1 << 16);
  _dumpElement(doc.mCanvas, 0, out);
  return out;
}

inline bool writeFile(const std::string& path, const std::string& text)
{
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << text;
  return static_cast<bool>(f);
}

inline bool readFile(const std::string& path, std::string& text)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  text = ss.str();
  return true;
}

inline std::vector<std::string> _lines(const std::string& text)
{
  std::vector<std::string> lines;
  std::istringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) lines.push_back(line);
  return lines;
}

/**
 * Compare two dumps line by line. Prints up to maxReport differences and
 * returns the total number of differing lines (0 = identical).
 */
inline size_t compare(const std::string& expected, const std::string& actual,
                      const char* label, size_t maxReport = 20)
{
  const auto a = _lines(expected);
  const auto b = _lines(actual);
  size_t diffs = 0;
  const size_t n = std::max(a.size(), b.size());
  for (size_t i = 0; i < n; ++i)
  {
    const std::string* la = i < a.size() ? &a[i] : nullptr;
    const std::string* lb = i < b.size() ? &b[i] : nullptr;
    if (la && lb && *la == *lb) continue;
    if (diffs < maxReport)
    {
      std::printf("[%s] line %zu differs:\n  - %s\n  + %s\n", label, i + 1,
                  la ? la->c_str() : "<missing>", lb ? lb->c_str() : "<missing>");
    }
    ++diffs;
  }
  return diffs;
}

} // namespace glint_snapshot
