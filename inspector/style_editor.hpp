#pragma once

/**
 * inspector/style_editor.hpp
 * Style editing utilities and the editable style panel component used by the
 * glint inspector window.
 *
 * Deliberately separated from window.hpp so that the style-editor logic can
 * be read, tested and maintained in isolation from the Win32 window plumbing.
 *
 * Public API:
 *   glint_style_set_by_name(style, key, value)  � inverse of glint_style_serialize
 *   glint_style_is_valid_by_name(key, value)     � syntactic validation per field type
 *   InspStylePanel                               � editable CSS property list component
 */

#include "../platform/glint_window.hpp"             // glint_document, glint_element, all components
#include "../components/glint_checkbox.hpp"
#include "../components/glint_colorpicker_window.hpp"  // glint_colorpicker_window
#include "../components/glint_gradient_editor.hpp" // glint_gradient_editor, sk_gradient_stop
#include "../components/glint_select.hpp"
#include "glint_attributes_list.hpp"                   // glint_attributes_list_window

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// -- Color-picker window message codes (style_editor ? inspector window) ------
static constexpr UINT WM_INSP_CP_CHANGED  = WM_USER + 105;  // wp = uint32 ARGB
static constexpr UINT WM_INSP_CP_CLOSED   = WM_USER + 106;
static constexpr UINT WM_INSP_ATTR_PICKED = WM_USER + 107;  // lp = new'd std::string* key

static inline std::string _trimCssWhitespace(std::string value)
{
  while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
  while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
  return value;
}

static inline bool _isCssGradientString(const std::string& val)
{
  const std::string trimmed = _trimCssWhitespace(val);
  return trimmed.rfind("linear-gradient(", 0) == 0 ||
         trimmed.rfind("radial-gradient(", 0) == 0 ||
         trimmed.rfind("conic-gradient(", 0) == 0;
}

static inline std::string _formatCssNumber(float value)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  return buf;
}

// Non-throwing float parse — returns true on success, false if no conversion.
static inline bool _cssStof(const std::string& s, float& out)
{
  if (s.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str() || errno == ERANGE) return false;
  out = v;
  return true;
}

static inline bool _tryParseCssAngleDegrees(const std::string& token, float& outDegrees)
{
  const std::string trimmed = _trimCssWhitespace(token);
  float v = 0.f;
  if (trimmed.size() > 3 && trimmed.substr(trimmed.size() - 3) == "deg")
  {
    if (!_cssStof(trimmed.substr(0, trimmed.size() - 3), v)) return false;
    outDegrees = v; return true;
  }
  if (trimmed.size() > 4 && trimmed.substr(trimmed.size() - 4) == "grad")
  {
    if (!_cssStof(trimmed.substr(0, trimmed.size() - 4), v)) return false;
    outDegrees = v * 0.9f; return true;
  }
  if (trimmed.size() > 3 && trimmed.substr(trimmed.size() - 3) == "rad")
  {
    if (!_cssStof(trimmed.substr(0, trimmed.size() - 3), v)) return false;
    outDegrees = v * 57.29577951308232f; return true;
  }
  if (trimmed.size() > 4 && trimmed.substr(trimmed.size() - 4) == "turn")
  {
    if (!_cssStof(trimmed.substr(0, trimmed.size() - 4), v)) return false;
    outDegrees = v * 360.f; return true;
  }
  return false;
}

static inline void _parseCssGradientCenter(glint_style& s, const std::string& token)
{
  const size_t atPos = token.find("at ");
  if (atPos == std::string::npos) return;

  std::string center = token.substr(atPos + 3);
  while (!center.empty() && std::isspace((unsigned char)center.front())) center.erase(center.begin());
  while (!center.empty() && std::isspace((unsigned char)center.back())) center.pop_back();

  const size_t sp = center.find(' ');
  if (sp == std::string::npos) return;

  const std::string x = center.substr(0, sp);
  const std::string y = center.substr(sp + 1);
  float v = 0.f;
  if (!x.empty() && x.back() == '%')
    if (_cssStof(x.substr(0, x.size() - 1), v)) s.backgroundGradientCX = v / 100.f;
  if (!y.empty() && y.back() == '%')
    if (_cssStof(y.substr(0, y.size() - 1), v)) s.backgroundGradientCY = v / 100.f;
}

// -- glint_style_set_by_name ---------------------------------------------------
// Inverse of glint_style_serialize: maps each string key back to a write on
// Parses a CSS linear-gradient() or radial-gradient() string and populates
// the backgroundGradient stops, type, and angle fields of the given style.
// Returns true if at least one stop was parsed successfully.
static inline bool _parseCssGradient(glint_style& s, const std::string& val)
{
  const std::string trimmedVal = _trimCssWhitespace(val);
  const bool isLinear = trimmedVal.rfind("linear-gradient(", 0) == 0;
  const bool isRadial = trimmedVal.rfind("radial-gradient(",  0) == 0;
  const bool isConic  = trimmedVal.rfind("conic-gradient(",   0) == 0;
  if (!isLinear && !isRadial && !isConic) return false;

  s.backgroundGradientType = isLinear ? "linear" : (isRadial ? "radial" : "conic");

  // Strip outer function name and parentheses
  const size_t paren = trimmedVal.find('(');
  if (paren == std::string::npos) return false;
  const size_t closing = trimmedVal.rfind(')');
  if (closing == std::string::npos || closing <= paren) return false;
  const std::string inner = trimmedVal.substr(paren + 1, closing - paren - 1);

  // Split by comma, respecting nested parens (e.g. rgb(...))
  std::vector<std::string> parts;
  int depth = 0;
  std::string cur;
  const auto pushTrimmed = [&]() {
    while (!cur.empty() && std::isspace((unsigned char)cur.front())) cur.erase(cur.begin());
    while (!cur.empty() && std::isspace((unsigned char)cur.back()))  cur.pop_back();
    if (!cur.empty()) parts.push_back(cur);
    cur.clear();
  };
  for (char c : inner) {
    if      (c == '(')             { ++depth; cur += c; }
    else if (c == ')')             { --depth; cur += c; }
    else if (c == ',' && depth==0) { pushTrimmed(); }
    else                           { cur += c; }
  }
  pushTrimmed();
  if (parts.empty()) return false;

  // First token may be an angle or direction keyword
  size_t stopStart = 0;
  const std::string& first = parts[0];
  if (isConic && first.rfind("from ", 0) == 0) {
    std::string angleToken = first.substr(5);
    const size_t atPos = angleToken.find(" at ");
    if (atPos != std::string::npos) angleToken.resize(atPos);
    while (!angleToken.empty() && std::isspace((unsigned char)angleToken.back())) angleToken.pop_back();
    _tryParseCssAngleDegrees(angleToken, s.backgroundGradientAngle);
    _parseCssGradientCenter(s, first);
    stopStart = 1;
  } else if (isConic && first.rfind("at ", 0) == 0) {
    _parseCssGradientCenter(s, first);
    stopStart = 1;
  } else if (isRadial && (first == "circle" || first == "ellipse" ||
                          first.rfind("circle at ", 0) == 0 ||
                          first.rfind("ellipse at ", 0) == 0 ||
                          first.rfind("at ", 0) == 0)) {
    _parseCssGradientCenter(s, first);
    stopStart = 1;
  } else if (_tryParseCssAngleDegrees(first, s.backgroundGradientAngle)) {
    s.backgroundGradientDirection.clear();  // numeric angle — no keyword direction
    stopStart = 1;
  } else if (first.rfind("to ", 0) == 0) {
    const std::string dir = first.substr(3);
    if      (dir == "right")        s.backgroundGradientAngle =   90.f;
    else if (dir == "left")         s.backgroundGradientAngle =  270.f;
    else if (dir == "bottom")       s.backgroundGradientAngle =  180.f;
    else if (dir == "top")          s.backgroundGradientAngle =    0.f;
    else if (dir == "bottom right") s.backgroundGradientAngle =  135.f;
    else if (dir == "bottom left")  s.backgroundGradientAngle =  225.f;
    else if (dir == "top right")    s.backgroundGradientAngle =   45.f;
    else if (dir == "top left")     s.backgroundGradientAngle =  315.f;
    s.backgroundGradientDirection.clear();  // side/corner keyword stored as numeric angle only
    stopStart = 1;
  }

  // Parse color stops: "<color> <pos>%" or just "<color>"
  s.backgroundGradient.clear();
  const size_t numStops = parts.size() - stopStart;
  for (size_t i = stopStart; i < parts.size(); ++i) {
    const std::string& part = parts[i];
    sk_gradient_stop st;

    // Split on last space to separate color from optional position
    const size_t lastSp = part.rfind(' ');
    std::string colorStr, posStr;
    if (lastSp != std::string::npos) {
      posStr   = part.substr(lastSp + 1);
      colorStr = part.substr(0, lastSp);
      while (!colorStr.empty() && std::isspace((unsigned char)colorStr.back())) colorStr.pop_back();
    } else {
      colorStr = part;
    }

    // Parse position
    if (!posStr.empty() && posStr.back() == '%') {
      float v = 0.f;
      if (_cssStof(posStr.substr(0, posStr.size() - 1), v)) st.position = v / 100.f;
    } else if (isConic) {
      float degrees = 0.f;
      if (_tryParseCssAngleDegrees(posStr, degrees)) st.position = degrees / 360.f;
      else if (numStops <= 1)                         st.position = 0.f;
      else                                            st.position = (float)(i - stopStart) / (float)(numStops - 1);
    } else if (numStops <= 1) {
      st.position = 0.f;
    } else {
      st.position = (float)(i - stopStart) / (float)(numStops - 1);
    }

    while (!colorStr.empty() && std::isspace((unsigned char)colorStr.front())) colorStr.erase(colorStr.begin());
    sk_color c; c = colorStr.c_str(); st.color = c;
    s.backgroundGradient.push_back(st);
  }
  std::sort(s.backgroundGradient.begin(), s.backgroundGradient.end());
  return !s.backgroundGradient.empty();
}

// Converts internal gradient state to a CSS gradient string for display.
static inline std::string _gradientToCssString(
    const std::vector<sk_gradient_stop>& stops,
    const std::string& type,
    float angle,
    const std::string& direction = "",
    float centerX = 0.5f,
    float centerY = 0.5f)
{
  if (stops.empty()) return "";
  std::string result;
  bool hasLeadingArgs = false;
  if (type == "radial") {
    result = "radial-gradient(circle";
    result += " at ";
    result += _formatCssNumber(centerX * 100.f);
    result += "% ";
    result += _formatCssNumber(centerY * 100.f);
    result += "%";
    hasLeadingArgs = true;
  } else if (type == "conic") {
    result = "conic-gradient(";
    const bool hasAngle = std::fabs(angle) > 0.0001f;
    const bool hasCenter = std::fabs(centerX - 0.5f) > 0.0001f || std::fabs(centerY - 0.5f) > 0.0001f;
    if (hasAngle || hasCenter) {
      if (hasAngle) {
        result += "from ";
        result += _formatCssNumber(angle);
        result += "deg";
      }
      if (hasCenter) {
        if (hasAngle) result += " ";
        result += "at ";
        result += _formatCssNumber(centerX * 100.f);
        result += "% ";
        result += _formatCssNumber(centerY * 100.f);
        result += "%";
      }
      hasLeadingArgs = true;
    }
  } else {
    result = "linear-gradient(";
    if (!direction.empty()) {
      result += direction;
    } else {
      result += _formatCssNumber(angle);
      result += "deg";
    }
    hasLeadingArgs = true;
  }
  bool firstStop = true;
  for (const auto& st : stops) {
    if (!firstStop || hasLeadingArgs) result += ", ";
    if (st.color.A == 0) {
      result += "transparent";
    } else if (st.color.A < 255) {
      char hbuf[32];
      std::snprintf(hbuf, sizeof(hbuf), "rgba(%d,%d,%d,%.4f)",
        st.color.R, st.color.G, st.color.B, st.color.A / 255.f);
      result += hbuf;
    } else {
      char hbuf[12];
      std::snprintf(hbuf, sizeof(hbuf), "#%02x%02x%02x", st.color.R, st.color.G, st.color.B);
      result += hbuf;
    }
    result += " ";
    if (type == "conic") {
      result += _formatCssNumber(st.position * 360.f);
      result += "deg";
    } else {
      result += _formatCssNumber(st.position * 100.f);
      result += "%";
    }
    firstStop = false;
  }
  result += ")";
  return result;
}

// glint_style.  Called from the inspector style panel onChange handlers so that
// edits made in the inspector are reflected live on the target component.
static inline void glint_style_set_by_name(glint_style& s,
                                           const std::string& key,
                                           const std::string& val)
{
  const float f = [&]() -> float {
    if (val.empty()) return 0.f;
    char* end = nullptr;
    const float r = std::strtof(val.c_str(), &end);
    return (end != val.c_str()) ? r : 0.f;
  }();

  if      (key == "color")               s.color = val.c_str();
  else if (key == "background-color")    s.backgroundColor = val.c_str();
  else if (key == "background" || key == "background-gradient")
  {
    s.backgroundGradient.clear();
    if (!val.empty())
    {
      // CSS gradient syntax (e.g. "linear-gradient(135deg, #667eea 0%, #764ba2 100%)")
      if (_isCssGradientString(val))
      {
        _parseCssGradient(s, val);
      }
      else
      {
        // Internal pipe format: "<pos>:#rrggbbaa|<pos>:#rrggbbaa|..."
        size_t start = 0;
        while (start < val.size())
        {
          const size_t sep = val.find('|', start);
          const std::string tok = (sep == std::string::npos) ? val.substr(start)
                                                             : val.substr(start, sep - start);
          const size_t col = tok.find(':');
          if (col != std::string::npos)
          {
            sk_gradient_stop st;
            try { st.position = std::stof(tok.substr(0, col)); } catch (...) {}
            sk_color c; c = tok.substr(col + 1).c_str(); st.color = c;
            s.backgroundGradient.push_back(st);
          }
          if (sep == std::string::npos) break;
          start = sep + 1;
        }
        std::sort(s.backgroundGradient.begin(), s.backgroundGradient.end());
      }
    }
  }
  else if (key == "background-gradient-angle")
  {
    try { s.backgroundGradientAngle = std::stof(val); } catch (...) {}
  }
  else if (key == "background-gradient-type")
  {
    s.backgroundGradientType = val;
  }
  else if (key == "background-gradient-cx")
  {
    try { s.backgroundGradientCX = std::stof(val); } catch (...) {}
  }
  else if (key == "background-gradient-cy")
  {
    try { s.backgroundGradientCY = std::stof(val); } catch (...) {}
  }
  else if (key == "background-gradient-radius")
  {
    try { s.backgroundGradientRadius = std::stof(val); } catch (...) {}
  }
  else if (key == "background-img")    s.backgroundImageProp = val.c_str();
  else if (key == "background-size")     s.backgroundSize     = val;
  else if (key == "background-position") s.backgroundPosition = val;
  else if (key == "background-repeat")   s.backgroundRepeat   = val;
  else if (key == "opacity")              s.opacity = f;
  else if (key == "border-color")         s.borderColor = val.c_str();
  else if (key == "border-width")         s.borderWidth = f;
  else if (key == "border-radius")        s.borderRadius = val;  // glint_length: accepts "8px", "50%", 8.f
  else if (key == "border-style")         s.borderStyle = val;
  else if (key == "stroke-dashoffset")    { try { s.strokeDashoffset = std::stof(val); } catch (...) {} }
  else if (key == "stroke")              s.strokeColor = val.c_str();
  else if (key == "fill")               s.fill = val.c_str();
  else if (key == "stroke-dasharray")    s.strokeDasharray = val;
  else if (key == "stroke-linecap")      s.strokeLinecap = val;
  else if (key == "stroke-linejoin")     s.strokeLinejoin = val;
  else if (key == "stroke-miterlimit")   { try { s.strokeMiterlimit = std::stof(val); } catch (...) {} }
  else if (key == "stroke-opacity")      s.strokeOpacity = f;
  else if (key == "stroke-width")        { try { s.strokeWidth = std::stof(val); } catch (...) {} }
  else if (key == "border-top-left-radius")     s.borderTopLeftRadius = val;
  else if (key == "border-top-right-radius")    s.borderTopRightRadius = val;
  else if (key == "border-bottom-right-radius") s.borderBottomRightRadius = val;
  else if (key == "border-bottom-left-radius")  s.borderBottomLeftRadius = val;
  else if (key == "border-top-width")     s.borderTopWidth = val;
  else if (key == "border-right-width")   s.borderRightWidth = val;
  else if (key == "border-bottom-width")  s.borderBottomWidth = val;
  else if (key == "border-left-width")    s.borderLeftWidth = val;
  else if (key == "border-top-color")    s.borderTopColor    = val.c_str();
  else if (key == "border-right-color")  s.borderRightColor  = val.c_str();
  else if (key == "border-bottom-color") s.borderBottomColor = val.c_str();
  else if (key == "border-left-color")   s.borderLeftColor   = val.c_str();
  else if (key == "border-top-style")    s.borderTopStyle    = val;
  else if (key == "border-right-style")  s.borderRightStyle  = val;
  else if (key == "border-bottom-style") s.borderBottomStyle = val;
  else if (key == "border-left-style")   s.borderLeftStyle   = val;
  else if (key == "border-top")          s.borderTop    = val.c_str();
  else if (key == "border-right")        s.borderRight  = val.c_str();
  else if (key == "border-bottom")       s.borderBottom = val.c_str();
  else if (key == "border-left")         s.borderLeft   = val.c_str();
  else if (key == "box-shadow")          s.boxShadow = val;
  else if (key == "shadow-enabled")      s.shadowEnabled = (val == "true" || val == "1");
  else if (key == "shadow-color")        s.shadowColor = val.c_str();
  else if (key == "shadow-offset-x")     s.shadowOffsetX = f;
  else if (key == "shadow-offset-y")     s.shadowOffsetY = f;
  else if (key == "shadow-blur")         s.shadowBlur = f;
  else if (key == "font-size")           s.fontSize = val;
  else if (key == "font-family")         s.fontFamily = val;
  else if (key == "font-weight")         s.fontWeight = f;
  else if (key == "font-style")          s.fontStyle = val;
  else if (key == "line-height")         s.lineHeight = f;
  else if (key == "text-align")
  {
    if      (val == "left")  s.textAlign = EAlign::Near;
    else if (val == "right") s.textAlign = EAlign::Far;
    else                     s.textAlign = EAlign::Center;
  }
  else if (key == "vertical-align")      s.verticalAlign = val;
  else if (key == "text-decoration") s.textDecoration = val;
  else if (key == "padding")          s.padding       = val.c_str();
  else if (key == "padding-top")      s.paddingTop    = val.c_str();
  else if (key == "padding-right")    s.paddingRight  = val.c_str();
  else if (key == "padding-bottom")   s.paddingBottom = val.c_str();
  else if (key == "padding-left")     s.paddingLeft   = val.c_str();
  else if (key == "margin")           s.margin        = val.c_str();
  else if (key == "margin-top")       s.marginTop     = val.c_str();
  else if (key == "margin-right")     s.marginRight   = val.c_str();
  else if (key == "margin-bottom")    s.marginBottom  = val.c_str();
  else if (key == "margin-left")      s.marginLeft    = val.c_str();
  else if (key == "position")         s.position    = val;
  else if (key == "left")             s.left        = val;
  else if (key == "top")              s.top         = val;
  else if (key == "right")            s.right       = val;
  else if (key == "bottom")           s.bottom      = val;
  else if (key == "width")            s.width       = val;
  else if (key == "height")           s.height      = val;
  else if (key == "min-width")        s.minWidth    = val;
  else if (key == "max-width")        s.maxWidth    = val;
  else if (key == "min-height")       s.minHeight   = val;
  else if (key == "max-height")       s.maxHeight   = val;
  else if (key == "display")          s.display       = val;
  else if (key == "pointer-events")   s.pointerEvents  = val;
  else if (key == "user-select")      s.userSelect     = val;
  else if (key == "cursor")           s.cursor         = val;
  else if (key == "white-space")       s.whiteSpace     = val;
  else if (key == "selection-color")  s.selectionColor = val.c_str();
  else if (key == "flex-direction")   s.flexDirection  = val;
  else if (key == "justify-content")  s.justifyContent = val;
  else if (key == "align-items")      s.alignItems     = val;
  else if (key == "gap")              s.gap = val;
  else if (key == "flex-grow")        s.flexGrow = f;
  else if (key == "z-index")          s.zIndex = static_cast<int>(f);
  else if (key == "object-fit")       s.objectFit      = val;
  else if (key == "object-position")  s.objectPosition = val;
  else if (key == "transform")        s.transform = val;
  else if (key == "overflow-x")       s.overflowX = val;
  else if (key == "overflow-y")       s.overflowY = val;
  else if (key == "overflow")         s.overflow  = val.c_str();
  else if (key == "scrollbar-width")  s.scrollbarWidth       = f;
  else if (key == "scrollbar-thumb")  s.scrollbarThumbColor  = val.c_str();
  else if (key == "scrollbar-track")  s.scrollbarTrackColor  = val.c_str();
  else if (key == "scrollbar-button") s.scrollbarButtonColor = val.c_str();
  else if (key == "filter")           s.filter         = val;
  else if (key == "backdrop-filter")  s.backdropFilter = val;
  else if (key == "mix-blend-mode")        s.mixBlendMode        = val;
  else if (key == "background-blend-mode") s.backgroundBlendMode = val;
  else if (key == "isolation")             s.isolation           = val;
  else if (key == "mask")             s.mask           = val;
  else if (key == "mask-mode")        s.maskMode       = val;
  else if (key == "mask-position")    s.maskPosition   = val;
  else if (key == "mask-size")        s.maskSize       = val;
  else if (key == "mask-repeat")      s.maskRepeat     = val;
  else if (key == "mask-origin")      s.maskOrigin     = val;
  else if (key == "mask-clip")        s.maskClip       = val;
  else if (key == "mask-composite")   s.maskComposite  = val;
  else if (key == "transition")       s.transition = val;
}

// -- glint_style_is_valid_by_name ----------------------------------------------
// Returns true if `val` is a syntactically valid value for the given style key.
// Used by InspStylePanel to color inputs red on invalid input.
static inline bool glint_style_is_valid_by_name(const std::string& key,
                                                 const std::string& val)
{
  // Strip trailing "!important" so field-specific validators see only the bare value.
  // e.g. "#ae00ff !important" → "#ae00ff"
  auto _stripImportant = [](const std::string& s) -> std::string {
    std::string r = s;
    while (!r.empty() && std::isspace((unsigned char)r.back())) r.pop_back();
    if (r.size() >= 9) {
      std::string t = r.substr(r.size() - 9);
      for (char& c : t) c = (char)std::tolower((unsigned char)c);
      if (t == "important") {
        r.resize(r.size() - 9);
        while (!r.empty() && std::isspace((unsigned char)r.back())) r.pop_back();
        if (!r.empty() && r.back() == '!') {
          r.pop_back();
          while (!r.empty() && std::isspace((unsigned char)r.back())) r.pop_back();
        }
      }
    }
    return r;
  };
  const std::string v = _stripImportant(val);

  // Helper: is this string a valid (possibly negative, decimal) float?
  // Uses strtof (no-throw) to avoid LLDB exception breakpoints on bad input.
  auto isFloat = [](const std::string& s) -> bool {
    if (s.empty()) return false;
    char* end = nullptr;
    const float f = std::strtof(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(f);
  };

  // Helper: is this a valid CSS color?
  //   Accepts: empty string (transparent), #rgb, #rgba, #rrggbb, #rrggbbaa
  auto isColor = [](const std::string& s) -> bool {
    if (s.empty()) return true;   // transparent / unset
    if (s[0] != '#') return true; // named colors � let parser decide, don't block
    const size_t n = s.size() - 1;
    if (n != 3 && n != 4 && n != 6 && n != 8) return false;
    for (size_t i = 1; i < s.size(); ++i)
    {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
        return false;
    }
    return true;
  };

  // Helper: is this a CSS length (number, number%, numberpx, keyword, or empty)?
  // Also accepts CSS keywords valid for width/height and calc() expressions.
  auto isLength = [&isFloat](const std::string& s) -> bool {
    if (s.empty()) return true;
    // CSS keywords and calc() — accept without numeric parsing.
    if (s == "auto" || s == "min-content" || s == "max-content" || s == "fit-content")
      return true;
    if (s.size() >= 5 && s.substr(0, 5) == "calc(") return true;
    if (s.size() > 2 && s.substr(s.size() - 2) == "px") return isFloat(s.substr(0, s.size() - 2));
    if (s.back() == '%') return isFloat(s.substr(0, s.size() - 1));
    return isFloat(s);
  };

  auto isVerticalAlign = [&isLength](const std::string& s) -> bool {
    if (s.empty()) return true;
    const std::string low = [&]() {
      std::string r = s;
      for (char& c : r) c = (char) std::tolower((unsigned char) c);
      return r;
    }();
    return low == "baseline" || low == "middle" || low == "sub" || low == "super" ||
           low == "text-top" || low == "text-bottom" || low == "top" || low == "bottom" ||
           isLength(low);
  };

  // Helper: plain number or "npx" � NO percent.
  // Used for fields backed by a raw float (sk_side_proxy / float members).
  // Percent is structurally unsupported: stof("10%") silently gives 10px.
  auto isPxOrPlain = [&isFloat](const std::string& s) -> bool {
    if (s.empty()) return true;
    if (s.size() > 2 && s.substr(s.size() - 2) == "px") return isFloat(s.substr(0, s.size() - 2));
    return isFloat(s);
  };

  // Helper: CSS shorthand accepting 1-4 space-separated "npx" or plain-float tokens.
  // Used for padding, margin, border-width where multi-value syntax is valid.
  auto isShorthandPxOrPlain = [&isPxOrPlain](const std::string& s) -> bool {
    if (s.empty()) return true;
    size_t i = 0;
    int count = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
      if (i >= s.size()) break;
      size_t j = i;
      while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
      if (!isPxOrPlain(s.substr(i, j - i))) return false;
      ++count;
      i = j;
    }
    return count >= 1 && count <= 4;
  };

  // Helper: CSS shorthand accepting 1-4 space-separated length tokens (px, %, plain).
  // Used for border-radius (4 corners) and gap (row + column).
  auto isShorthandLength = [&isLength](const std::string& s) -> bool {
    if (s.empty()) return true;
    size_t i = 0;
    int count = 0;
    while (i < s.size()) {
      while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
      if (i >= s.size()) break;
      size_t j = i;
      while (j < s.size() && !std::isspace((unsigned char)s[j])) ++j;
      if (!isLength(s.substr(i, j - i))) return false;
      ++count;
      i = j;
    }
    return count >= 1 && count <= 4;
  };

  // -- Pure-unit numeric fields (no px suffix valid) -------------------------
  if (key == "opacity" || key == "flex-grow" || key == "line-height" ||
      key == "stroke-dashoffset" || key == "stroke-opacity" || key == "stroke-miterlimit" || key == "stroke-width" ||
      key == "background-gradient-angle" || key == "background-gradient-cx" ||
      key == "background-gradient-cy"    || key == "background-gradient-radius")
    return isFloat(v);

  // -- background gradient (CSS or internal pipe format) ---------------------
  if (key == "background" || key == "background-gradient")
  {
    if (v.empty()) return true;
    if (_isCssGradientString(v))
      return v.back() == ')';
    return v.find(':') != std::string::npos;  // pipe format
  }

  if (key == "box-shadow")
    return !v.empty();

  // -- Plain-float dimension fields (number or "npx" only, NOT %) -----------
  // These are backed by raw floats (sk_side_proxy / float) — % is not stored.
  // padding/margin/border-width support 1-4 value shorthand syntax.
  if (key == "padding" || key == "margin" || key == "border-width")
    return isShorthandPxOrPlain(v);

  if (key == "shadow-offset-x" || key == "shadow-offset-y"  || key == "shadow-blur"     ||
      key == "padding-top"     || key == "padding-right"    || key == "padding-bottom"  ||
      key == "padding-left"    || key == "margin-top"       || key == "margin-right"    ||
      key == "margin-bottom"   || key == "margin-left"      || key == "scrollbar-width")
    return isPxOrPlain(v);

  // Per-side widths: glint_length but % doesn't make sense for border widths
  if (key == "border-top-width" || key == "border-right-width" ||
      key == "border-bottom-width" || key == "border-left-width")
    return isPxOrPlain(v);

  // -- Color fields ---------------------------------------------------------
  if (key == "color"              || key == "background-color"  || key == "border-color"    ||
      key == "shadow-color"       || key == "scrollbar-thumb"   || key == "scrollbar-track" ||
      key == "scrollbar-button"   || key == "selection-color"   ||
      key == "border-top-color"   || key == "border-right-color" ||
      key == "border-bottom-color" || key == "border-left-color" || key == "stroke")
    return isColor(v);

  // -- Boolean ---------------------------------------------------------------
  if (key == "shadow-enabled")
    return v == "true" || v == "false" || v == "1" || v == "0";

  // -- Length fields (px or %) -----------------------------------------------
  if (key == "left" || key == "top" || key == "right" || key == "bottom" ||
      key == "width" || key == "height" ||
      key == "min-width" || key == "max-width" || key == "min-height" || key == "max-height" ||
      key == "font-size")
    return isLength(v);

  // border-radius and gap accept 1-4 space-separated length values.
  if (key == "border-radius" || key == "gap" ||
      key == "border-top-left-radius" || key == "border-top-right-radius" ||
      key == "border-bottom-right-radius" || key == "border-bottom-left-radius")
    return isShorthandLength(v);

  if (key == "vertical-align")
    return isVerticalAlign(v);

  // -- String / enum fields — no strict validation ---------------------------
  // (position, display, flex-direction, justify-content, align-items, overflow, filter, etc.)
  return true;
}

// =============================================================================
// Style property registry
// =============================================================================

// All known style keys in display order (determines both popup order and row order).
static const std::vector<const char*>& glint_all_style_keys()
{
  static const std::vector<const char*> keys = {
    // layout
    "display","pointer-events","user-select","cursor","white-space","position","flex-direction","flex-grow",
    "justify-content","align-items","gap","z-index",
    "transform",
    "overflow","overflow-x","overflow-y",
    // box model
    "width","height","min-width","max-width","min-height","max-height",
    "left","top","right","bottom",
    "padding","padding-top","padding-right","padding-bottom","padding-left",
    "margin","margin-top","margin-right","margin-bottom","margin-left",
    // typography
    "font-size","font-family","font-weight","font-style","line-height","color","text-align","vertical-align","text-decoration","selection-color",
    // appearance
    "background-color","background","background-img","background-size","background-position","background-repeat","opacity",
    "border-color","border-width","border-radius","border-style","stroke-dashoffset",
    "fill",
    "stroke","stroke-dasharray","stroke-linecap","stroke-linejoin",
    "stroke-miterlimit","stroke-opacity","stroke-width",
    "border-top-left-radius","border-top-right-radius","border-bottom-right-radius","border-bottom-left-radius",
    "border-top","border-right","border-bottom","border-left",
    "border-top-width","border-right-width","border-bottom-width","border-left-width",
    "border-top-color","border-right-color","border-bottom-color","border-left-color",
    "border-top-style","border-right-style","border-bottom-style","border-left-style",
    // shadow
    "box-shadow",
    // scrollbar
    "scrollbar-width","scrollbar-thumb","scrollbar-track","scrollbar-button",
    // misc
    "filter","backdrop-filter","mix-blend-mode","background-blend-mode","isolation",
    "mask","mask-mode","mask-position","mask-size",
    "mask-repeat","mask-origin","mask-clip","mask-composite",
    "object-fit","object-position",
    "transition",
  };
  return keys;
}

// Serialized default glint_style used to detect which properties are "set".
static const glint_style_info& glint_default_style_info()
{
  static const glint_style_info d = glint_style_serialize(glint_style{});
  return d;
}

// Returns true if `key` has a non-default value in `info`.
static bool glint_prop_is_set(const std::string& key, const glint_style_info& info)
{
  const auto& def = glint_default_style_info();
  auto it  = info.find(key);
  auto dit = def.find(key);
  if (it == info.end()) return false;
  if (dit == def.end()) return true;
  return it->second != dit->second;
}

// Returns a sensible non-default starter value for a property being added.
static std::string glint_add_default(const std::string& key)
{
  // Color fields
  if (key=="color" || key=="background-color" || key=="border-color" ||
      key=="shadow-color" || key=="scrollbar-thumb" || key=="scrollbar-track" ||
      key=="scrollbar-button" || key=="fill" || key=="stroke") return "#808080";
  if (key=="background")  return "linear-gradient(0deg, #333333 0%, #666666 100%)";
  if (key=="box-shadow")  return "0px 2px 4px 0px #00000066";
  // Length fields that default to "" — so any non-empty value is "set"
  if (key=="width" || key=="height") return "100%";
  if (key=="min-width" || key=="min-height") return "100px";
  if (key=="max-width" || key=="max-height") return "200px";
  if (key=="left"  || key=="top" || key=="right" || key=="bottom") return "0";
  if (key=="gap"                    ) return "4";
  if (key=="font-size"              ) return "12";
  if (key=="font-family"            ) return "Roboto";
  if (key=="font-weight"            ) return "400";
  if (key=="font-style"             ) return "italic";
  if (key=="line-height"            ) return "1.5";
  // Enums / keywords
  if (key=="z-index"             ) return "1";
  if (key=="white-space"         ) return "nowrap";
  if (key=="background-img"    ) return "url(\"path/to/img.png\")";
  if (key=="background-size"     ) return "cover";
  if (key=="background-position" ) return "center";
  if (key=="background-repeat"   ) return "no-repeat";
  if (key=="display")           return "flex";
  if (key=="pointer-events")    return "none";
  if (key=="user-select")       return "text";
  if (key=="cursor")            return "pointer";
  if (key=="selection-color")   return "#5db1ffb4";
  if (key=="position")          return "static";
  if (key=="flex-direction")    return "column";
  if (key=="justify-content")   return "center";
  if (key=="align-items")       return "center";
  if (key=="overflow" || key=="overflow-x" || key=="overflow-y") return "hidden";
  if (key=="border-style")      return "solid";
  if (key=="text-align")        return "left";
  if (key=="vertical-align")    return "middle";
  if (key=="text-decoration")   return "line-through";
  if (key=="shadow-enabled")    return "true";
  if (key=="object-fit")        return "fill";
  if (key=="object-position")   return "center";
  if (key=="transform")         return "translateX(-50%)";
  if (key=="filter")            return "blur(4px)";
  if (key=="backdrop-filter")   return "blur(8px)";
  if (key=="mix-blend-mode")        return "multiply";
  if (key=="background-blend-mode") return "multiply";
  if (key=="isolation")             return "isolate";
  if (key=="mask")              return "linear-gradient(to bottom, black, transparent)";
  if (key=="mask-mode")         return "alpha";
  if (key=="mask-position")     return "0% 0%";
  if (key=="mask-size")         return "cover";
  if (key=="mask-repeat")       return "no-repeat";
  if (key=="mask-origin")       return "border-box";
  if (key=="mask-clip")         return "border-box";
  if (key=="mask-composite")    return "add";
  if (key=="transition")        return "none";
  // Shorthand keys — set all sides at once; per-side rows appear after rebuild
  if (key=="padding")       return "8";
  if (key=="margin")        return "4";
  if (key=="border-top" || key=="border-right" ||
      key=="border-bottom" || key=="border-left") return "1px solid #808080";
  // Per-side border colors — must return a valid color string (not "0")
  if (key=="border-top-color" || key=="border-right-color" ||
      key=="border-bottom-color" || key=="border-left-color") return "#808080";
  // Per-side border styles — use a valid keyword
  if (key=="border-top-style" || key=="border-right-style" ||
      key=="border-bottom-style" || key=="border-left-style") return "solid";
  // Padding per-side: default serializes as "0" — use non-zero so the row appears
  if (key=="padding-top" || key=="padding-right" ||
      key=="padding-bottom" || key=="padding-left") return "8";
  // Margin per-side: same issue
  if (key=="margin-top" || key=="margin-right" ||
      key=="margin-bottom" || key=="margin-left") return "4";
  // SVG stroke properties
  if (key=="stroke")             return "#ffffff";
  if (key=="stroke-dasharray")   return "5 5";
  if (key=="stroke-linecap")     return "round";
  if (key=="stroke-linejoin")    return "round";
  if (key=="stroke-miterlimit")  return "4";
  if (key=="stroke-opacity")     return "0.8";
  if (key=="stroke-width")       return "1";
  // Numeric fields whose default is 0 — use 1 so they show as "set"
  if (key=="border-width"   || key=="border-radius" || key=="opacity"      ||
      key=="flex-grow"      || key=="shadow-blur"   || key=="shadow-offset-x"||
      key=="shadow-offset-y" || key=="scrollbar-width" || key=="stroke-dashoffset") return "1";
  return "0";
}
// =============================================================================
// Wheel-scroll helpers for numeric / length style properties
// =============================================================================

// Returns true for style keys that carry a numeric or length value and can be
// meaningfully incremented / decremented by mouse-wheel (up = increase, down = decrease).
static inline bool glint_style_is_scrubable(const std::string& key)
{
  // Pure-unit numerics
  if (key == "opacity" || key == "flex-grow" || key == "line-height" || key == "stroke-dashoffset" ||
      key == "stroke-opacity" || key == "stroke-miterlimit" || key == "stroke-width") return true;
  // Plain-float dimension fields
  if (key == "border-width"      || key == "padding"           || key == "margin"           ||
      key == "shadow-offset-x"   || key == "shadow-offset-y"  || key == "shadow-blur"       ||
      key == "padding-top"       || key == "padding-right"    || key == "padding-bottom"    ||
      key == "padding-left"      || key == "margin-top"       || key == "margin-right"      ||
      key == "margin-bottom"     || key == "margin-left"      ||
      key == "scrollbar-width"   || key == "border-top-width" || key == "border-right-width" ||
      key == "border-bottom-width" || key == "border-left-width") return true;
  // glint_length fields
  if (key == "left" || key == "top" || key == "right" || key == "bottom" ||
      key == "width" || key == "height" ||
      key == "min-width" || key == "max-width" || key == "min-height" || key == "max-height" ||
      key == "gap"  || key == "font-size" || key == "border-radius"              ||
      key == "border-top-left-radius"    || key == "border-top-right-radius"    ||
      key == "border-bottom-right-radius" || key == "border-bottom-left-radius") return true;
  // Transform / filter / mask strings — wheel adjusts the numeric value under the cursor
  if (key == "transform" || key == "filter" || key == "backdrop-filter" ||
      key == "mask") return true;
  return false;
}

// Returns true for properties whose computed value must be >= 0 per CSS spec.
// (negative values are either invalid or treated as 0 by browsers)
static inline bool glint_style_is_non_negative(const std::string& key)
{
  return key == "width"               || key == "height"              ||
         key == "min-width"           || key == "max-width"           ||
         key == "min-height"          || key == "max-height"          ||
         key == "padding"             || key == "padding-top"         ||
         key == "padding-right"       || key == "padding-bottom"      ||
         key == "padding-left"        ||
         key == "gap"                 || key == "font-size"           ||
         key == "border-radius"       ||
         key == "border-top-left-radius"     || key == "border-top-right-radius"    ||
         key == "border-bottom-right-radius" || key == "border-bottom-left-radius" ||
         key == "border-width"        || key == "border-top-width"    ||
         key == "border-right-width"  || key == "border-bottom-width" ||
         key == "border-left-width"   || key == "scrollbar-width"     ||
         key == "shadow-blur"         || key == "opacity"             ||
         key == "stroke-width"        || key == "flex-grow";
}

// Adjusts a CSS-length or plain-number string by `delta`:
//   "16"    + 1  ?  "17"
//   "16px"  - 1  ?  "15px"
//   "50%"   + 5  ?  "55%"
//   ""      + 1  ?  "1"
// Returns the original string when the value cannot be parsed.
static inline std::string glint_length_adjust(const std::string& val, float delta)
{
  std::string suffix;
  std::string numStr = val.empty() ? "0" : val;

  if (numStr.size() > 2 && numStr.substr(numStr.size() - 2) == "px")
  {
    suffix = "px";
    numStr = numStr.substr(0, numStr.size() - 2);
  }
  else if (!numStr.empty() && numStr.back() == '%')
  {
    suffix = "%";
    numStr = numStr.substr(0, numStr.size() - 1);
  }

  float f = 0.f;
  try { f = std::stof(numStr); } catch (...) { return val; }
  f += delta;

  char buf[64];
  // %.6g gives up to 6 significant digits � handles values up to 999999 without
  // scientific notation and preserves fine sub-pixel steps (e.g. 100.1, 0.05).
  if (std::floor(f) == f)
    snprintf(buf, sizeof(buf), "%.0f%s", f, suffix.c_str());
  else
    snprintf(buf, sizeof(buf), "%.6g%s", f, suffix.c_str());

  return buf;
}

// Adjusts one numeric value inside a CSS transform string.
// `cursorPos` is the text-cursor byte index in `val` (from inp->getCursorPos()).
// Algorithm:
//   1. Find the outermost func(�) block that contains the cursor.
//      If the cursor sits between functions, pick the block nearest to it.
//   2. Within that block, collect all numeric tokens (supporting units:
//      deg, rad, turn, px, %).
//   3. Pick the token whose byte range is nearest to the cursor.
//   4. Adjust that token via glint_length_adjust and rebuild the full string.
// Returns `val` unchanged when parsing fails.
static inline std::string glint_transform_adjust(const std::string& val, float delta, int cursorPos)
{
  if (val.empty() || val == "none") return val;

  // -- 1. Find the outermost func(�) block containing cursorPos -------------
  int funcOpen = -1, funcClose = -1;
  {
    // First pass: look for a block that actually contains cursorPos.
    int depth = 0, openIdx = -1;
    for (int i = 0; i < (int)val.size(); ++i)
    {
      if (val[i] == '(')
      {
        if (depth == 0) openIdx = i;
        ++depth;
      }
      else if (val[i] == ')')
      {
        --depth;
        if (depth == 0 && cursorPos >= openIdx && cursorPos <= i + 1)
        {
          funcOpen  = openIdx;
          funcClose = i;
          break;
        }
      }
    }
    // Fallback: cursor is between/outside all parens � pick the nearest block.
    if (funcOpen < 0)
    {
      depth = 0;
      int scanOpen = -1;
      int bestGap  = INT_MAX;
      for (int i = 0; i < (int)val.size(); ++i)
      {
        if (val[i] == '(')
        {
          if (depth == 0) scanOpen = i;
          ++depth;
        }
        else if (val[i] == ')')
        {
          --depth;
          if (depth == 0 && scanOpen >= 0)
          {
            int gap = (cursorPos < scanOpen) ? scanOpen  - cursorPos
                    : (cursorPos > i)        ? cursorPos - i
                    : 0;
            if (gap < bestGap) { bestGap = gap; funcOpen = scanOpen; funcClose = i; }
            scanOpen = -1;
          }
        }
      }
    }
  }
  if (funcOpen < 0 || funcClose < 0) return val;

  // -- 2. Parse numeric tokens inside the parentheses -----------------------
  const int insideStart = funcOpen + 1;
  const std::string inside = val.substr(insideStart, funcClose - insideStart);
  const int relCursor = std::max(0, std::min((int)inside.size(),
                                              cursorPos - insideStart));

  struct NumToken { int start, end; };
  std::vector<NumToken> tokens;

  for (int i = 0; i < (int)inside.size(); )
  {
    const char c = inside[i];
    // A token starts with a digit, '.', or a leading '-' immediately after
    // a comma/space (so we don't confuse a minus in "e-5" as a token start).
    bool isNumStart =
      std::isdigit((unsigned char)c) ||
      (c == '.' && i+1 < (int)inside.size() &&
       std::isdigit((unsigned char)inside[i+1])) ||
      (c == '-' && i+1 < (int)inside.size() &&
       (std::isdigit((unsigned char)inside[i+1]) || inside[i+1] == '.') &&
       (i == 0 || inside[i-1] == ',' || inside[i-1] == ' '));

    if (isNumStart)
    {
      int s = i;
      if (inside[i] == '-') ++i;
      while (i < (int)inside.size() && std::isdigit((unsigned char)inside[i])) ++i;
      if (i < (int)inside.size() && inside[i] == '.')
      {
        ++i;
        while (i < (int)inside.size() && std::isdigit((unsigned char)inside[i])) ++i;
      }
      // Consume optional unit suffix.
      if      (i   < (int)inside.size() && inside[i] == '%')             { ++i; }
      else if (i+1 < (int)inside.size() && inside.substr(i,2) == "px")   { i += 2; }
      else if (i+2 < (int)inside.size() && inside.substr(i,3) == "deg")  { i += 3; }
      else if (i+2 < (int)inside.size() && inside.substr(i,3) == "rad")  { i += 3; }
      else if (i+3 < (int)inside.size() && inside.substr(i,4) == "turn") { i += 4; }
      tokens.push_back({s, i});
    }
    else
    {
      ++i;
    }
  }

  if (tokens.empty()) return val;

  // -- 3. Pick the token nearest to relCursor -------------------------------
  int best = 0, bestDist = INT_MAX;
  for (int t = 0; t < (int)tokens.size(); ++t)
  {
    int dist = (relCursor < tokens[t].start) ? tokens[t].start - relCursor
             : (relCursor > tokens[t].end)   ? relCursor - tokens[t].end
             : 0;
    if (dist < bestDist) { bestDist = dist; best = t; }
  }

  // -- 4. Adjust and rebuild -------------------------------------------------
  const NumToken& tok = tokens[best];
  const std::string adjusted =
    glint_length_adjust(inside.substr(tok.start, tok.end - tok.start), delta);
  const std::string newInside =
    inside.substr(0, tok.start) + adjusted + inside.substr(tok.end);
  return val.substr(0, insideStart) + newInside + val.substr(funcClose);
}

// =============================================================================
// PropWriter — routes a (property, value) write to the correct style layer.
// Passed by buildCssRuleBlock to buildPropRow so CSS rule edits go to the
// stylesheet AST (via glint_document::updateCssDeclaration) rather than
// element.style.  Inline-style rows leave this defaulted to {}.
// PropDeleter — removes the property entirely from its layer.
// CSS rule rows call removeCssDeclaration; inline rows reset to the spec default.
// =============================================================================
using PropWriter  = std::function<void(const std::string&, const std::string&)>;
using PropDeleter = std::function<void()>;

// =============================================================================
// Forward declarations
// =============================================================================
class InspStylePanel;

// =============================================================================
// InspStaticText
// =============================================================================
// Passive single-line inspector label. Deliberately NOT a button: static labels
// must not inherit button defaults like centered text alignment or hover/press
// state machinery. The renderer is kept local here so fixed-height inspector
// rows still get vertically-centred text.
class InspStaticText : public glint_element
{
public:
  std::string& text;

  InspStaticText()
    : text(innerText)
  {
    style.display     = "block";
    style.textAlign   = EAlign::Near;
    style.userSelect  = "none";
    style.cursor      = "default";
  }

  const char* typeName() const override { return "insp_static_text"; }

protected:
  void drawContent(glint_canvas& g) override
  {
    if (text.empty()) return;

    const glint_style& s = computedStyle;
    const char* _fn_ = s.fontFamily.empty() ? nullptr : s.fontFamily.c_str();
    const float fontSize = s.fontSize.toFloat() > 0.f ? s.fontSize.toFloat() : 12.f;
    glint_text t(fontSize,
            ApplyOpacity(s.color.value, s.opacity),
            _fn_,
            s.textAlign,
            EVAlign::Middle);
    g.DrawText(t, text.c_str(), getContent());
  }

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    if (!canvas || text.empty()) return;

    const glint_style& s = computedStyle;
    const float fontSize = s.fontSize.toFloat() > 0.f ? s.fontSize.toFloat() : 12.f;
    SkFont font = skFont(fontSize,
                         s.fontFamily.c_str(),
                         s.fontWeight,
                         s.fontStyle.c_str());
    const float textW = font.measureText(text.c_str(), text.size(), SkTextEncoding::kUTF8);

    SkPaint tp;
    tp.setAntiAlias(true);
    tp.setColor(skColor(ApplyOpacity(s.color.value, s.opacity)));

    const glint_rect r = getContent();
    float textX = r.L;
    if (s.textAlign == EAlign::Far)
      textX = r.R - textW;
    else if (s.textAlign == EAlign::Center)
      textX = r.L + (r.W() - textW) * 0.5f;

    SkFontMetrics metrics;
    font.getMetrics(&metrics);
    const float ascent  = metrics.fAscent < 0.f ? -metrics.fAscent : 0.f;
    const float descent = metrics.fDescent > 0.f ? metrics.fDescent : 0.f;
    const float baselineY = r.T + std::max(0.f, (r.H() - (ascent + descent)) * 0.5f) + ascent;

    canvas->drawString(text.c_str(), textX, baselineY, font, tp);
  }
};

// =============================================================================
// InspNameButton
// =============================================================================
// Displays a property name in teal.  Any click opens a one-item popup
// "Reset to default" � which is handled in OnPopupMenuSelection (outside the
// DOM dispatch chain) so it is safe to call clearChildren() via show() there.
class InspNameButton : public glint_button
{
public:
  std::string      mKey;
  InspStylePanel*  mPanel    = nullptr;
  glint_element*    mLiveComp = nullptr;
  PropWriter       mWriter;   // set by buildPropRow; routes reset writes to correct layer

  const char* typeName() const override { return "insp_name_btn"; }

  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override;

  // Body defined after InspStylePanel.
};

// =============================================================================
// InspAddAttrButton
// =============================================================================
// Visible "+ add attribute" button with a grey border.  Clicking opens a
// glint_attributes_list_window popup anchored above the button.
class InspAddAttrButton : public glint_button
{
public:
  InspStylePanel* mPanel       = nullptr;
  // When non-empty, this button targets a matched CSS rule instead of element.style.
  std::string     mRuleUrl;
  uint32_t        mRuleLine    = 0;
  // Keys already declared in the target rule — shown as disabled in the picker.
  std::set<std::string> mExistingKeys;

  const char* typeName() const override { return "insp_add_attr_btn"; }

  InspAddAttrButton()
  {
    innerText               = "+ Add attribute";
    style.userSelect        = "none";
    style.cursor            = "default";
    style.width             = 150.f;
    style.height            = 26.f;
    style.margin            = 4.f;
    style.marginLeft        = 18.f;
    style.borderWidth       = 1.f;
    style.borderColor       = glint_color(255, 72, 72, 90);
    style.borderRadius      = 4.f;
    style.backgroundColor   = glint_color(20, 180, 180, 180);
    style.color             = glint_color(255, 180, 180, 180);
    style.fontSize          = 12.f;
    style.textAlign         = EAlign::Center;
    hover.backgroundColor   = glint_color(50, 220, 220, 220);
    hover.borderColor       = glint_color(255, 255, 255, 255);
    hover.fontSize          = 12.f;
    hover.color             = glint_color(255, 255, 255, 255);
    pressed.backgroundColor = glint_color(255, 32, 32, 48);
    pressed.color           = glint_color(255, 148, 148, 170);
  }

  // Body defined after InspStylePanel (needs mPanel->openAttrList / mOwnerHWND).
  void OnMouseDown(float x, float y, const glint_mouse_mod& mod) override;
};

// =============================================================================
// InspEnumButton  �  opens a popup menu of allowed enum values on click
// =============================================================================
class InspEnumButton : public glint_button
{
public:
  std::string                      mKey;
  const std::vector<std::string>*  mOpts       = nullptr;
  glint_element*                 mLiveComp   = nullptr;
  std::shared_ptr<bool>            mRowEnabled;
  PropWriter                       mWriter;   // set by buildPropRow; routes enum writes

  InspEnumButton()
  {
    style.width = 0.f;
    style.flexGrow = 1.f;
    style.height = "100%";
    mAcceptsFocus = true;
  }

  const char* typeName() const override { return "insp_enum_btn"; }

  void onFocusGained() override
  {
    style.borderWidth  = 1.f;
    style.borderRadius = "3px";
    style.borderColor  = glint_color(255, 80, 140, 255);
    setDirty(false);
  }

  void onFocusLost() override
  {
    style.borderWidth = 0.f;
    style.borderColor = glint_color(0, 0, 0, 0);
    setDirty(false);
  }

  void OnMouseDown(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/) override
  {
    if (!mOpts) return;
    const std::string cur = (innerText == "(none)") ? "" : innerText;
    using P = std::pair<int, std::string>;
    std::vector<P> items;
    std::vector<int> checked;
    items.reserve(mOpts->size());
    for (size_t idx = 0; idx < mOpts->size(); ++idx)
    {
      const auto& value = (*mOpts)[idx];
      const int id = static_cast<int>(idx) + 1;
      items.push_back({id, value.empty() ? "(none)" : value});
      if (value == cur) checked.push_back(id);
    }
    const int result = glint_platform::showContextMenu(0, 0, items, {}, checked);
    if (result < 1) return;
    const int idx = result - 1;
    if (idx >= static_cast<int>(mOpts->size())) return;
    if (mRowEnabled && !*mRowEnabled) return;
    const std::string next = (*mOpts)[static_cast<size_t>(idx)];
    innerText = next.empty() ? "(none)" : next;
    setDirty(false);
    if (mWriter)
      mWriter(mKey, next);
    else {
      glint_style_set_by_name(mLiveComp->style, mKey, next);
      mLiveComp->setDirty(false);
    }
  }

  bool OnKeyDown(const glint_key_press& key) override
  {
    if (!mOpts || mOpts->empty()) return false;
    if (key.vk != 0x26 && key.vk != 0x28) return false;  // VK_UP / VK_DOWN only
    if (mRowEnabled && !*mRowEnabled) return true;

    const std::string cur = (innerText == "(none)") ? "" : innerText;
    const int n = static_cast<int>(mOpts->size());
    int idx = 0;
    for (int i = 0; i < n; ++i)
      if ((*mOpts)[static_cast<size_t>(i)] == cur) { idx = i; break; }

    if (key.vk == 0x26) idx = (idx - 1 + n) % n;  // VK_UP   — previous
    else                idx = (idx + 1)     % n;  // VK_DOWN — next

    const std::string next = (*mOpts)[static_cast<size_t>(idx)];
    innerText = next.empty() ? "(none)" : next;
    setDirty(false);
    if (mWriter)
      mWriter(mKey, next);
    else if (mLiveComp) {
      glint_style_set_by_name(mLiveComp->style, mKey, next);
      mLiveComp->setDirty(false);
    }
    return true;
  }
};

// =============================================================================
// GradModeButton  �  small gradient-mode toggle inside the backgroundColor row
// =============================================================================
// Clicking switches the backgroundColor row between:
//   flat mode  � existing swatch + hex input + colorpicker popup window
//   grad mode  � inline glint_gradient_editor that edits backgroundGradient
// Constructor is declared here and defined after InspStylePanel so its body can
// call mPanel->toggleBgGradientMode() without a forward-declaration issue.
class GradModeButton : public glint_element
{
public:
  InspStylePanel* mPanel    = nullptr;
  bool            mGradMode = false;   // true = currently in gradient mode

  const char* typeName() const override { return "grad_mode_btn"; }

  GradModeButton();

  void DrawContentToCanvas(SkCanvas* canvas) override
  {
    const float L = mPaintRECT.L + 2.f, T = mPaintRECT.T + 2.f;
    const float R = mPaintRECT.R - 2.f, B = mPaintRECT.B - 2.f;
    if (R <= L || B <= T) return;
    const SkRect rect = SkRect::MakeLTRB(L, T, R, B);
    // Active (gradient mode): vivid blue?orange strip.
    // Inactive (flat mode):   dim grey strip � suggests "click to add gradient".
    SkPoint pts[2] = { {L, 0.f}, {R, 0.f} };
    const SkColor c0 = mGradMode ? SkColorSetARGB(255,  60,  60, 200)
                                 : SkColorSetARGB( 90,  80,  80,  80);
    const SkColor c1 = mGradMode ? SkColorSetARGB(255, 200, 100,  60)
                                 : SkColorSetARGB( 90, 160, 160, 160);
    SkColor colors[2] = { c0, c1 };
    auto shader = SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
    SkPaint p;
    p.setShader(shader);
    canvas->drawRect(rect, p);
  }
};

// -- CssSaveButton ------------------------------------------------------------
// Small yellow rounded-square button shown in a CSS rule header when that rule
// has unsaved inspector edits.  Invisible (no paint) when clean so it takes no
// visual space until needed.  onClick is wired by buildCssRuleBlock to call
// mDocument->saveRuleToFile() and reset the dirty flag.
class CssSaveButton : public glint_button
{
public:
  // Shared with the writer lambda so *dirtyFlag = true in the writer
  // immediately makes DrawToCanvas paint the button without a full rebuild.
  std::shared_ptr<bool> dirtyFlag;

protected:
  void DrawToCanvas(SkCanvas* canvas) override
  {
    if (!dirtyFlag || !*dirtyFlag)
    {
      for (auto& ch : mChildren) ch->DrawToCanvas(canvas);
      return;
    }

    const glint_color bg = mIsPressed ? glint_color(255, 195, 148, 18)
                    : mIsHovered ? glint_color(255, 235, 188, 48)
                                 : glint_color(255, 215, 168, 28);
    SkPaint fill;
    fill.setColor(skColor(bg));
    fill.setAntiAlias(true);
    canvas->drawRoundRect(skRect(mRect), 3.f, 3.f, fill);

    // Save icon: arrow-down + tray.
    const glint_rect  r  = getContent();
    const float  cx = r.L + r.W() * 0.5f;
    const float  cy = r.T + r.H() * 0.5f - 0.5f;
    SkPaint icon;
    icon.setColor(SkColorSetARGB(255, 255, 255, 255));
    icon.setAntiAlias(true);
    icon.setStyle(SkPaint::kStroke_Style);
    icon.setStrokeWidth(1.5f);
    icon.setStrokeCap(SkPaint::kRound_Cap);
    icon.setStrokeJoin(SkPaint::kRound_Join);

    canvas->drawLine(cx, cy - 3.5f, cx, cy + 1.5f, icon);
    SkPath wg;
    wg.moveTo(cx - 2.5f, cy);
    wg.lineTo(cx,        cy + 3.f);
    wg.lineTo(cx + 2.5f, cy);
    canvas->drawPath(wg, icon);
    canvas->drawLine(cx - 4.5f, cy + 4.5f, cx + 4.5f, cy + 4.5f, icon);

    for (auto& ch : mChildren) ch->DrawToCanvas(canvas);
  }
};

// =============================================================================
// InspStylePanel  �  Chrome DevTools-style "element.style { ... }" block
// =============================================================================
//
//   element.style {                   ? dim header
//     display         : flex ;        ? set prop row (teal name, orange value)
//     backgroundColor : #1a1a1a ;
//     borderRadius    : 6 ;
//     + add property...               ? dim, opens popup of unset props on click
//   }                                 ? dim footer
//
// Only SET properties are shown.  Clicking the property name opens a popup to
// reset it.  The add-row opens a popup of all unset properties; selecting one
// sets a starter value and rebuilds so the row appears and the value is focused.
class InspStylePanel : public glint_element
{
public:
  InspStylePanel()
  {
    style.overflowY     = "auto";
    style.flexDirection = "column";
  }

  // ── Image preview popup callbacks (same signature as InspComputedPanel) ─────────
  // Wired by glint_inspector_window::buildUI().
  using RowEnterCb = std::function<void(const std::string&, const std::string&,
                                        float, float, float)>;
  using RowLeaveCb = std::function<void()>;

  void setPreviewCallbacks(RowEnterCb onEnter, RowLeaveCb onLeave)
  {
    mOnRowEnter = std::move(onEnter);
    mOnRowLeave = std::move(onLeave);
  }

  static bool selectorHasPseudo(const std::string& selectorText)
  {
    return selectorText.find(':') != std::string::npos;
  }

  const char* typeName() const override { return "insp_style_panel"; }

  // Called from inspector's handleMessage(WM_INSP_ATTR_PICKED) on the inspector
  // thread � safe to modify component state and call setDirty.
  void commitAddProperty(const std::string& key)
  {
    mPendingAddKey    = key;
    mRebuildPending   = true;
    setDirty(false);
  }

  // Called by InspAddAttrButton::OnMouseDown before openAttrList() so that
  // commitAddProperty() knows which CSS rule to write to.
  // Pass empty url / line 0 to target element.style (default / reset).
  void setPendingAddRule(const std::string& url, uint32_t line)
  {
    mPendingAddRuleUrl  = url;
    mPendingAddRuleLine = line;
  }

  void cancelAdd()
  {
    mPendingAddKey.clear();
    mPendingAddRuleUrl.clear();
    mPendingAddRuleLine = 0;
    mRebuildPending = true;
    setDirty(false);
  }

  void requestRebuild()
  {
    mRebuildPending = true;
    setDirty(false);
  }

  // Save all CSS rules that have pending inspector edits to their source files.
  // Called from the inspector window on Ctrl+S.
  void saveAllDirtyRules()
  {
    if (!mDocument) return;
    for (const auto& key : mDirtyRules)
    {
      // ruleKey format: "sourceUrl|sourceLine"
      const size_t sep = key.rfind('|');
      if (sep == std::string::npos) continue;
      const std::string url = key.substr(0, sep);
      uint32_t line = 0;
      try { line = static_cast<uint32_t>(std::stoul(key.substr(sep + 1))); } catch (...) {}
      if (!url.empty() && line > 0)
        mDocument->saveRuleToFile(url, line);
    }
    mDirtyRules.clear();
    mRebuildPending = true;
    setDirty(false);
  }

  // Layout() is called by glint_document::Draw() outside any event-dispatch chain.
  // We exploit that timing to safely call clearChildren() / show() here.
  void Layout(glint_canvas* g) override
  {
    if (mRebuildPending) {
      mRebuildPending = false;
      if (!mPendingAddKey.empty() && mLiveComp) {
        mFocusAfterBuild      = mPendingAddKey;  // remember before clearing
        mFocusAfterBuildIsCss = !mPendingAddRuleUrl.empty();  // CSS vs inline
        if (!mPendingAddRuleUrl.empty() && mDocument)
        {
          // Route add to the CSS stylesheet AST, not element.style.
          mDocument->updateCssDeclaration(mPendingAddRuleUrl, mPendingAddRuleLine,
                                           mPendingAddKey, glint_add_default(mPendingAddKey),
                                           mLiveComp);
          mPendingAddRuleUrl.clear();
          mPendingAddRuleLine = 0;
        }
        else
        {
          glint_style_set_by_name(mLiveComp->style, mPendingAddKey, glint_add_default(mPendingAddKey));
          mLiveComp->setDirty(false);
        }
        mPendingAddKey.clear();
      }
      mInputToFocus = nullptr;  // reset; buildPropRow populates if key == mFocusAfterBuild
      show(mLiveComp);  // triggers setDirty(false) -> another Layout() pass
      // Auto-focus the value input of a freshly-added attribute.
      if (mInputToFocus && mRoot) mRoot->SetFocus(mInputToFocus);
      mInputToFocus = nullptr;
      mFocusAfterBuild.clear();
      mFocusAfterBuildIsCss = false;
      return;           // children just rebuilt; base Layout() runs next frame
    }

    if (mDeferredCssRulesStage == 1)
    {
      mDeferredCssRulesStage = 2;
      setDirty(false);
      glint_element::Layout(g);
      return;
    }

    if (mDeferredCssRulesStage == 2)
    {
      mDeferredCssRulesStage = 0;
      appendDeferredCssRuleBlocks();
    }

    glint_element::Layout(g);
  }

  // Rebuild for a new component (clears previous children, safe to call
  // outside the DOM/keyboard dispatch chain).
  void show(glint_element* comp)
  {
    if (!comp) { clear(); return; }
    clearDeferredCssRuleBuild();
    _dismissPickerWindow();                       // close any open picker first
    _dismissAttrList();                           // close any open attribute picker
    const bool isFirstShow = (comp != mLiveComp);
    if (isFirstShow) {
      mColorPickerKey.clear();
      mBgFlatMode = false;   // reset flat-colour mode when switching to a new component
      mDisabledDecls.clear();
      mDisabledSavedVals.clear();
      mLastValues.clear();    // reset flash baseline when switching to a new node
      mFlashProgress.clear(); // cancel any in-progress flashes
    }
    mLiveComp = comp;
    // Register disabled-decl set with the cascade engine (Option A).
    if (mDocument) mDocument->setInspectorDisabledDecls(&mDisabledDecls);

    // ── Realtime flash: detect value changes since the previous tick ─────────
    // info is computed here, before clearChildren(), so mLastValues can be
    // compared to the new snapshot while the old panel children still exist.
    const glint_style_info info = glint_style_serialize(comp->style);
    if (!mLastValues.empty() && !isFirstShow)
    {
      auto _chkFlash = [&](const std::string& fkey, const std::string& newVal)
      {
        auto it = mLastValues.find(fkey);
        if (it != mLastValues.end() && it->second != newVal)
          mFlashProgress[fkey] = 1.0f;
      };
      for (const auto& attr : _collectElementAttributeValues(comp))
        _chkFlash(attr.first, attr.second);
      for (const auto& kv : info) _chkFlash(kv.first, kv.second);
    }
    // Refresh the baseline snapshot for the next tick.
    mLastValues.clear();
    for (const auto& attr : _collectElementAttributeValues(comp))
      mLastValues[attr.first] = attr.second;
    for (const auto& kv : info) mLastValues[kv.first] = kv.second;

    clearChildren();

    // -- HTML Attributes section (innerText, etc.) --------------------------
    buildAttributesSection();

    // Fetch matched CSS rules early — needed to determine which inline
    // properties are beaten by a !important CSS rule.  Those rows must be
    // shown struck-through in the element.style block (inline loses to
    // !important author rules per the CSS cascade spec).
    std::vector<GlintMatchedCssRule> liveRules;
    std::unordered_set<std::string> importantCssProps;
    if (comp->mRoot && !comp->mRoot->stylesheets().empty())
    {
      liveRules = comp->mRoot->matchedCssRulesFor(comp, /*forcePseudoClasses=*/false);
      for (const auto& rule : liveRules)
        for (const auto& decl : rule.declarations)
          if (decl.important && !selectorHasPseudo(rule.selectorText))
            importantCssProps.insert(decl.property);
    }

    // -- Header: element.style { --------------------------------------------
    buildLine("element.style {", 26.f, 8.f);

    // -- One row per SET property --------------------------------------------
    for (const char* k : glint_all_style_keys())
    {
      const std::string key = k;
      const std::string dId = _declId("", "", key);
      bool isSet = glint_prop_is_set(key, info);
      // Chrome spec: glint_optional_float properties carry an explicit isSet flag
      // that is more authoritative than value comparison.  Without this, setting
      // e.g. style.opacity = 1.f is invisible because "1" == default "1", yet
      // Chrome DevTools DOES show it in element.style {} whenever it was
      // authored.  style.*= "" (removeProperty equivalent) clears isSet so the
      // row correctly disappears again.
      if      (key == "opacity")        isSet = comp->style.opacity.isSet;
      else if (key == "font-weight")    isSet = comp->style.fontWeight.isSet;
      else if (key == "stroke-opacity") isSet = comp->style.strokeOpacity.isSet;
      // When the property was just added via the picker, force-show it even if
      // its value equals the struct default (e.g. font-weight: 400).
      if (!isSet && !mFocusAfterBuild.empty() && key == mFocusAfterBuild && !mFocusAfterBuildIsCss) isSet = true;
      // When the user just toggled gradient OFF, force-show the "background" row
      // in flat-colour mode so the row doesn't disappear.
      if (!isSet && key == "background" && mBgFlatMode) isSet = true;
      // When an inline property was disabled, _write reset it to the CSS-spec
      // default so _mergedStyle() lets the CSS layer show through.  That makes
      // glint_prop_is_set() return false and the row disappears on rebuild.
      // Re-emit it as a disabled row using the value saved at disable-time.
      const bool isDisabledInline = !isSet && mDisabledSavedVals.count(dId);
      if (isDisabledInline) isSet = true;
      if (isSet)
      {
        // In flat mode the "background" row displays/edits backgroundColor.
        // For disabled inline rows, show the saved pre-disable value.
        const std::string val = isDisabledInline
                                ? mDisabledSavedVals.at(dId)
                                : (key == "background" && mBgFlatMode)
                                  ? (info.count("background-color") ? info.at("background-color") : "")
                                  : (info.count(key) ? info.at(key) : "");
        // Inline property is overridden (struck-through) when a !important
        // CSS rule wins the same property — the inline style loses in that case.
        buildPropRow(key, val, {}, importantCssProps.count(key) > 0, false, dId);
      }
    }

    // -- Add-attribute button --------------------------------------------------
    buildAddAttrButton();

    // -- Footer: } -----------------------------------------------------------
    buildLine("}", 24.f, 8.f);

    const bool shouldDeferCssRules = isFirstShow && comp->mRoot && !comp->mRoot->stylesheets().empty();
    if (shouldDeferCssRules)
    {
      mDeferredCssComp              = comp;
      mDeferredCssInfo              = info;
      mDeferredLiveRules            = std::move(liveRules);
      mDeferredImportantCssProps    = std::move(importantCssProps);
      mDeferredCssSeedDisabledDecls = isFirstShow;
      mDeferredCssRulesStage        = 1;
    }
    else if (comp->mRoot && !comp->mRoot->stylesheets().empty())
    {
      appendCssRuleBlocks(comp,
                          info,
                          liveRules,
                          comp->mRoot->matchedCssRulesFor(comp, /*forcePseudoClasses=*/true),
                          importantCssProps,
                          isFirstShow);
    }

    setDirty(false);
  }

  // Switch backgroundColor row between flat-colour and gradient mode.
  // Called from GradModeButton's click listener on the inspector thread.
  void toggleBgGradientMode()
  {
    if (!mLiveComp) return;
    const bool isGradNow = !mLiveComp->style.backgroundGradient.empty();
    if (isGradNow)
    {
      // Gradient → flat: just remove the gradient, leave backgroundColor untouched.
      mLiveComp->style.backgroundGradient.clear();
      mBgFlatMode = true;   // keep "background" row visible in flat-colour mode
    }
    else
    {
      // Flat ? gradient: seed two stops from the current flat colour.
      glint_color c = mLiveComp->style.backgroundColor;
      glint_color darker(c.A,
                    std::max(0, (int)(c.R * 0.55f)),
                    std::max(0, (int)(c.G * 0.55f)),
                    std::max(0, (int)(c.B * 0.55f)));
      mLiveComp->style.backgroundGradient = { {0.f, darker}, {1.f, c} };
    }
    _dismissPickerWindow();
    mLiveComp->setDirty(false);
    mRebuildPending = true;
    setDirty(false);
  }

  // Drop all children and release the live-component pointer.
  void clear()
  {
    clearDeferredCssRuleBuild();
    _dismissPickerWindow();
    _dismissAttrList();
    if (mDocument) mDocument->setInspectorDisabledDecls(nullptr);
    mLiveComp = nullptr;
    mDisabledDecls.clear();
    mDisabledSavedVals.clear();
    clearChildren();
    setDirty(false);
  }

  // Timer-driven live refresh for the Style tab. Rebuilds from the current
  // live node when safe, so runtime value changes show up without requiring
  // explicit inspector notifications.
  void liveRefresh(glint_element* comp)
  {
    if (!comp) { clear(); return; }
    if (!canLiveRefresh()) return;
    // Tick down all active flash animations (~1 s total at 150 ms cadence).
    constexpr float kFlashDecrement = 0.16f;
    for (auto it = mFlashProgress.begin(); it != mFlashProgress.end(); )
    {
      it->second -= kFlashDecrement;
      if (it->second <= 0.f) it = mFlashProgress.erase(it);
      else                   ++it;
    }
    show(comp);
  }

  bool canLiveRefresh() const
  {
    if (mRebuildPending || mAttrListWin || !mColorPickerKey.empty() ||
        mSwatchForPicker || mInputForPicker)
      return false;

    if (mRoot)
    {
      for (glint_element* n = mRoot->getFocusedNode(); n; n = n->mParent)
        if (n == this) return false;
    }

    return true;
  }

  void clearDeferredCssRuleBuild()
  {
    mDeferredCssComp = nullptr;
    mDeferredCssInfo.clear();
    mDeferredLiveRules.clear();
    mDeferredImportantCssProps.clear();
    mDeferredCssSeedDisabledDecls = false;
    mDeferredCssRulesStage = 0;
  }

  void appendDeferredCssRuleBlocks()
  {
    if (!mDeferredCssComp || mDeferredCssComp != mLiveComp || !mDeferredCssComp->mRoot)
    {
      clearDeferredCssRuleBuild();
      return;
    }

    auto rules = mDeferredCssComp->mRoot->matchedCssRulesFor(mDeferredCssComp, /*forcePseudoClasses=*/true);
    appendCssRuleBlocks(mDeferredCssComp,
                        mDeferredCssInfo,
                        mDeferredLiveRules,
                        rules,
                        mDeferredImportantCssProps,
                        mDeferredCssSeedDisabledDecls);
    clearDeferredCssRuleBuild();
    setDirty(false);
  }

  void appendCssRuleBlocks(glint_element* comp,
                           const glint_style_info& info,
                           const std::vector<GlintMatchedCssRule>& liveRules,
                           const std::vector<GlintMatchedCssRule>& rules,
                           const std::unordered_set<std::string>& importantCssProps,
                           bool seedDisabledDecls)
  {
    if (!comp || rules.empty()) return;

    if (seedDisabledDecls)
    {
      for (const auto& rule : rules)
        for (const auto& decl : rule.declarations)
          if (decl.disabled)
            mDisabledDecls.insert(_declId(rule.sourceUrl, rule.selectorText, decl.property));
      if (mDocument) mDocument->setInspectorDisabledDecls(&mDisabledDecls);
    }

    std::unordered_set<std::string> activeRuleKeys;
    for (const auto& rule : liveRules)
      activeRuleKeys.insert(rule.sourceUrl + "|" + std::to_string(rule.sourceLine) + "|" + std::to_string(rule.sourceOrder));

    std::unordered_set<std::string> claimed;
    for (const char* k : glint_all_style_keys())
    {
      const std::string ck = k;
      bool ckSet = glint_prop_is_set(ck, info);
      if      (ck == "opacity")        ckSet = comp->style.opacity.isSet;
      else if (ck == "font-weight")    ckSet = comp->style.fontWeight.isSet;
      else if (ck == "stroke-opacity") ckSet = comp->style.strokeOpacity.isSet;
      if (ckSet && !mDisabledDecls.count(_declId("", "", ck)) && !importantCssProps.count(ck))
        claimed.insert(ck);
    }

    auto* sep = new glint_element();
    sep->style.width  = "100%";
    sep->style.height = 6.f;
    addChild(sep);

    for (const auto& rule : rules)
    {
      const std::string ruleKey = rule.sourceUrl + "|" + std::to_string(rule.sourceLine) + "|" + std::to_string(rule.sourceOrder);
      buildCssRuleBlock(rule, claimed, activeRuleKeys.count(ruleKey) > 0);
    }
  }

  ~InspStylePanel() override
  {
    // destroy() tears down the picker thread; unlike hide() it does NOT fire
    // onClosed, so no stale WM_INSP_CP_CLOSED arrives on the now-dead inspector.
    if (mDocument) mDocument->setInspectorDisabledDecls(nullptr);
    if (mPickerWindow) { mPickerWindow->destroy(); mPickerWindow = nullptr; }
    _dismissAttrList();
  }

  // Pre-create the picker window (hidden) so the first real open only needs a
  // cheap reopen() instead of spinning up a new thread and Win32 window.
  // Call this once after mOwnerHWND is set.  Safe to call multiple times.
  void prewarmPicker()
  {
    if (mPickerWindow || !mOwnerHWND) return;
#if defined(_WIN32)
    // glint_colorpicker_window::showOnCreate() returns false, so the window
    // starts hidden and is never briefly visible at 0,0.  No hide() needed.
    mPickerWindow = glint_colorpicker_window::open(glint_color(255, 0, 0, 0), RECT{});
#endif
  }

  glint_element*  mLiveComp    = nullptr;
  void*          mOwnerHWND   = nullptr;  // inspector HWND (Win32) or null (macOS)
  glint_document* mDocument    = nullptr;  // set by window.hpp; used for CSS rule writes
  glint_element*  mDeferredCssComp = nullptr;
  glint_style_info mDeferredCssInfo;
  std::vector<GlintMatchedCssRule> mDeferredLiveRules;
  std::unordered_set<std::string> mDeferredImportantCssProps;
  bool mDeferredCssSeedDisabledDecls = false;
  int  mDeferredCssRulesStage = 0;

  // Called from inspector's handleMessage(WM_INSP_CP_CHANGED) on the inspector
  // thread � safe to update glint components directly.
  void updateActiveSwatch(glint_color c)
  {
    if (mSwatchForPicker)
    {
      mSwatchForPicker->style.backgroundColor = c;
      mSwatchForPicker->setDirty(false);
    }
    if (mInputForPicker)
      mInputForPicker->setValue(_colorToHex(c));
  }

  // Called from inspector's handleMessage(WM_INSP_CP_CLOSED) on the inspector
  // thread � the user closed the picker window via the OS title-bar X button.
  // Called from inspector's handleMessage(WM_INSP_CP_CLOSED) on the inspector
  // thread. gen must match mPickerGeneration — stale notifications from a
  // previous hide() are discarded so they don't disturb a reopen in progress.
  void onPickerClosed(int gen)
  {
    if (gen != mPickerGeneration) return;  // stale — a newer open is already in flight
    // mPickerWindow is still alive and reusable — do NOT null it.
    if (mSwatchForPicker)
    {
      mSwatchForPicker->style.borderColor = glint_color(255, 110, 110, 110);
      mSwatchForPicker->setDirty(false);
      mSwatchForPicker = nullptr;
    }
    mInputForPicker = nullptr;
    mColorPickerKey.clear();
    setDirty(false);
  }

  // Called from InspAddAttrButton::OnMouseDown on the inspector thread.
  // Opens the floating glint_attributes_list_window anchored above the add button.
  // ruleSetKeys: when non-null, the picker shows already-declared rule keys as
  // disabled instead of the inline element.style properties.
  void openAttrList(RECT anchorRect, const std::set<std::string>* ruleSetKeys = nullptr)
  {
#if defined(_WIN32)
    _dismissAttrList();
    if (!mLiveComp) return;

    std::set<std::string> setKeys;
    if (ruleSetKeys)
    {
      setKeys = *ruleSetKeys;
    }
    else
    {
      const glint_style_info info = glint_style_serialize(mLiveComp->style);
      for (const char* k : glint_all_style_keys())
        if (glint_prop_is_set(k, info)) setKeys.insert(k);
    }

    const void* ownerH = mOwnerHWND;
    mAttrListWin = glint_attributes_list_window::open(
      anchorRect,
      glint_all_style_keys(),
      std::move(setKeys),
      [ownerH](std::string key) {
        // Fires on the attr-list thread; marshal to the inspector thread.
        auto* heap = new std::string(std::move(key));
        if (ownerH) ::PostMessage((HWND)ownerH, WM_INSP_ATTR_PICKED, 0, (LPARAM)heap);
      },
      [this]() {
        // Fires on the attr-list thread after the window fully closes.
        mAttrListWin = nullptr;
      });
#elif defined(__APPLE__)
    _dismissAttrList();
    if (!mLiveComp) return;

    std::set<std::string> setKeys;
    if (ruleSetKeys)
    {
      setKeys = *ruleSetKeys;
    }
    else
    {
      const glint_style_info info = glint_style_serialize(mLiveComp->style);
      for (const char* k : glint_all_style_keys())
        if (glint_prop_is_set(k, info)) setKeys.insert(k);
    }

    InspStylePanel* panel = this;
    mAttrListWin = glint_attributes_list_window::open(
      anchorRect,
      glint_all_style_keys(),
      std::move(setKeys),
      [panel](std::string key) {
        // Fires on main thread (glint_attributes_list_window open() dispatches to main).
        panel->commitAddProperty(key);
      },
      [this]() {
        // Fires on main thread after the window fully closes.
        mAttrListWin = nullptr;
      });
#else
    (void)anchorRect; (void)ruleSetKeys;  // attribute picker not supported on this platform
#endif
  }

private:
  bool                       mRebuildPending  = false;
  bool                       mBgFlatMode      = false;  // true: show "background" row in flat-colour mode after gradient toggled off
  // Tracks explicitly-disabled property rows across panel rebuilds.
  // Key is a composite "sourceUrl|sourceLine|property" id so that disabling one
  // rule's declaration doesn't affect same-named declarations in other rules.
  // Inline element.style rows use the sentinel prefix "||" (empty url, line 0).
  std::unordered_set<std::string>  mDisabledDecls;       // declIds whose checkbox is unchecked
  std::map<std::string,std::string> mDisabledSavedVals;  // declId -> saved value (for restore)
  std::string                mPendingAddKey;
  std::string                mPendingAddRuleUrl;      // non-empty when the pending add targets a CSS rule
  uint32_t                   mPendingAddRuleLine = 0; // source line of the target rule
  std::string                mFocusAfterBuild;             // key whose input to auto-focus after rebuild
  bool                       mFocusAfterBuildIsCss = false; // true when the add targeted a CSS rule, not element.style
  glint_input*                mInputToFocus    = nullptr;   // set by buildPropRow when key==mFocusAfterBuild
  std::string                mColorPickerKey;               // key whose swatch is active
  int                        mPickerGeneration = 0;         // bumped on each open/dismiss; matched by onPickerClosed(gen)
  glint_colorpicker_window*  mPickerWindow    = nullptr;    // persistent OS window (hide/reopen, not close/recreate)
  glint_element*           mSwatchForPicker = nullptr;    // swatch in the active row
  glint_input*                  mInputForPicker  = nullptr;    // hex input in the active row
  glint_attributes_list_window* mAttrListWin     = nullptr;    // floating attribute-picker window
  // Set of ruleUrl+"|"+ruleLine strings for rules with pending (unsaved) edits.
  // Persists across panel rebuilds.  Cleared by saveAllDirtyRules() / per-rule save.
  std::unordered_set<std::string> mDirtyRules;

  // Image preview popup callbacks — set via setPreviewCallbacks().
  RowEnterCb mOnRowEnter;
  RowLeaveCb mOnRowLeave;

  // ── Realtime flash state ─────────────────────────────────────────────────
  // Snapshot of attribute/CSS property values from the previous liveRefresh
  // rebuild.  On each rebuild the new values are compared against this map;
  // any key whose value changed gets mFlashProgress[key] = 1.0f.
  std::unordered_map<std::string, std::string> mLastValues;
  // Per-key flash progress (0.0–1.0).  Set to 1.0 on change, decremented
  // by kFlashDecrement each liveRefresh tick (~0.16 at 150 ms ≈ 1 s total).
  std::unordered_map<std::string, float>       mFlashProgress;

  // Build a unique identity string for a CSS declaration so the disabled-state
  // maps can distinguish two declarations that share the same property name but
  // live in different rules.  Inline element.style rows pass url="", selector="".
  // Uses selectorText (not sourceLine) so the id survives CSS file edits that
  // shift line numbers without changing selector names.
  static std::string _declId(const std::string& url, const std::string& selectorText, const std::string& prop)
  {
    return url + "|" + selectorText + "|" + prop;
  }

  // Returns true for style keys that represent CSS color values.
  static bool _isColorKey(const std::string& k)
  {
    static const char* const keys[] = {
      "color","background-color","border-color","shadow-color",
      "scrollbar-thumb","scrollbar-track","scrollbar-button",
      "border-top-color","border-right-color","border-bottom-color","border-left-color",
      nullptr
    };
    for (const char* const* p = keys; *p; ++p) if (k == *p) return true;
    return false;
  }

  // Returns pointer to valid enum values for the key, or nullptr.
  static const std::vector<std::string>* _enumValues(const std::string& k)
  {
    static const std::map<std::string, std::vector<std::string>> m = {
      {"display",        {"flex","none","block","inline","table","table-row","table-cell"}},
      {"position",       {"static","relative","absolute","fixed","sticky"}},
      {"flex-direction",  {"row","column","row-reverse","column-reverse"}},
      {"justify-content", {"flex-start","center","flex-end","space-between","space-around"}},
      {"align-items",     {"flex-start","center","flex-end","stretch"}},
      {"overflow",       {"visible","hidden","scroll","auto"}},
      {"overflow-x",     {"visible","hidden","scroll","auto"}},
      {"overflow-y",     {"visible","hidden","scroll","auto"}},
      {"text-align",      {"left","center","right"}},
      {"vertical-align",  {"baseline","middle","sub","super","text-top","text-bottom","top","bottom"}},
      {"text-decoration", {"none","underline","line-through","underline line-through"}},
      {"border-style",    {"solid","dashed","dotted","none"}},
      {"object-fit",      {"contain","cover","fill","none","scale-down"}},
      {"object-position", {"center","top","bottom","left","right"}},
      {"background-gradient-type", {"linear","radial","conic"}},
    };
    auto it = m.find(k);
    return (it != m.end()) ? &it->second : nullptr;
  }

  // Keys whose text input should cycle values with ↑/↓ (not a popup button).
  static const std::vector<std::string>* _enumHints(const std::string& k)
  {
    static const std::map<std::string, std::vector<std::string>> m = {
      {"mix-blend-mode",      {"normal","multiply","screen","overlay","darken","lighten",
                               "color-dodge","color-burn","hard-light","soft-light",
                               "difference","exclusion","hue","saturation","color","luminosity"}},
      {"background-blend-mode",{"normal","multiply","screen","overlay","darken","lighten",
                               "color-dodge","color-burn","hard-light","soft-light",
                               "difference","exclusion","hue","saturation","color","luminosity"}},
    };
    auto it = m.find(k);
    return (it != m.end()) ? &it->second : nullptr;
  }

  static std::string _colorToHex(glint_color c)
  {
    char buf[12];
    if (c.A == 255) snprintf(buf, sizeof(buf), "#%02x%02x%02x",     c.R, c.G, c.B);
    else            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.R, c.G, c.B, c.A);
    return buf;
  }

  static std::string _trim(std::string s)
  {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
  }

  static std::string _lower(std::string s)
  {
    for (char& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  static std::string _floatToString(float v)
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
    return buf;
  }

  static std::string _intToString(int v)
  {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", v);
    return buf;
  }

  static std::string _boolToString(bool v)
  {
    return v ? "true" : "false";
  }

  static bool _tryParseInt(const std::string& text, int& out)
  {
    try
    {
      const std::string trimmed = _trim(text);
      size_t idx = 0;
      const int parsed = std::stoi(trimmed, &idx);
      if (idx != trimmed.size()) return false;
      out = parsed;
      return true;
    }
    catch (...) { return false; }
  }

  static bool _tryParseFloat(const std::string& text, float& out)
  {
    try
    {
      const std::string trimmed = _trim(text);
      size_t idx = 0;
      const float parsed = std::stof(trimmed, &idx);
      if (idx != trimmed.size()) return false;
      out = parsed;
      return true;
    }
    catch (...) { return false; }
  }

  static bool _tryParseBool(const std::string& text, bool& out)
  {
    const std::string v = _lower(_trim(text));
    if (v == "true" || v == "1" || v == "yes" || v == "on")  { out = true;  return true; }
    if (v == "false" || v == "0" || v == "no" || v == "off") { out = false; return true; }
    return false;
  }

  static std::string _joinStringList(const std::vector<std::string>& values)
  {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i)
    {
      if (i > 0) out += ", ";
      out += values[i];
    }
    return out;
  }

  static std::vector<std::string> _splitCommaList(const std::string& text)
  {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text)
    {
      if (c == ',')
      {
        cur = _trim(cur);
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
      }
      else
      {
        cur += c;
      }
    }
    cur = _trim(cur);
    if (!cur.empty()) out.push_back(cur);
    return out;
  }

  static std::string _colorModeToString(glint_colorpicker::ColorMode mode)
  {
    switch (mode)
    {
      case glint_colorpicker::ColorMode::HEX:  return "hex";
      case glint_colorpicker::ColorMode::RGBA: return "rgba";
      case glint_colorpicker::ColorMode::HSV:  return "hsv";
    }
    return "hex";
  }

  static bool _parseColorMode(const std::string& text, glint_colorpicker::ColorMode& out)
  {
    const std::string v = _lower(_trim(text));
    if (v == "hex")  { out = glint_colorpicker::ColorMode::HEX;  return true; }
    if (v == "rgba") { out = glint_colorpicker::ColorMode::RGBA; return true; }
    if (v == "hsv")  { out = glint_colorpicker::ColorMode::HSV;  return true; }
    return false;
  }

  static std::string _serializeGradientStops(const std::vector<sk_gradient_stop>& stops)
  {
    std::string out;
    for (size_t i = 0; i < stops.size(); ++i)
    {
      if (i > 0) out += ", ";
      out += _floatToString(stops[i].position);
      out += ":";
      out += _colorToHex(stops[i].color);
    }
    return out;
  }

  static bool _parseGradientStops(const std::string& text, std::vector<sk_gradient_stop>& out)
  {
    std::vector<sk_gradient_stop> parsed;
    for (const std::string& rawPart : _splitCommaList(text))
    {
      const size_t colon = rawPart.find(':');
      const size_t split = (colon != std::string::npos) ? colon : rawPart.find(' ');
      if (split == std::string::npos) return false;

      float pos = 0.f;
      if (!_tryParseFloat(rawPart.substr(0, split), pos)) return false;

      const std::string colorText = _trim(rawPart.substr(split + 1));
      if (!glint_style_is_valid_by_name("color", colorText)) return false;
      parsed.push_back({ std::clamp(pos, 0.f, 1.f), sk_color(colorText.c_str()).value });
    }

    if (parsed.empty()) return false;
    std::sort(parsed.begin(), parsed.end());
    out = std::move(parsed);
    return true;
  }

  std::vector<std::pair<std::string, std::string>> _collectElementAttributeValues(glint_element* comp) const
  {
    std::vector<std::pair<std::string, std::string>> attrs;
    if (!comp) return attrs;

    attrs.emplace_back("id", comp->id);
    attrs.emplace_back("className", comp->className);
    attrs.emplace_back("innerText", comp->innerText);

    if (auto* img = dynamic_cast<glint_image*>(comp))
    {
      attrs.emplace_back("src", img->src);
    }

    if (auto* input = dynamic_cast<glint_input*>(comp))
    {
      attrs.emplace_back("value", input->getValue());
      attrs.emplace_back("type", input->type);
      attrs.emplace_back("placeholder", input->placeholder);
      attrs.emplace_back("min", _floatToString(input->min));
      attrs.emplace_back("max", _floatToString(input->max));
      attrs.emplace_back("readonly", _boolToString(input->readonly));
      attrs.emplace_back("disabled", _boolToString(input->disabled));
    }

    if (auto* cb = dynamic_cast<glint_checkbox*>(comp))
    {
      attrs.emplace_back("checked", _boolToString(cb->checked));
      attrs.emplace_back("keepChecked", _boolToString(cb->keepChecked));
    }

    if (auto* sel = dynamic_cast<glint_select*>(comp))
    {
      attrs.emplace_back("selectedIndex", _intToString(sel->selectedIndex));
      attrs.emplace_back("placeholder", sel->placeholder);
      attrs.emplace_back("options", _joinStringList(sel->options));
    }

    if (auto* dial = dynamic_cast<glint_dial*>(comp))
      attrs.emplace_back("angle", _floatToString(dial->angle));

    if (auto* picker = dynamic_cast<glint_colorpicker*>(comp))
    {
      attrs.emplace_back("value", _colorToHex(picker->value));
      attrs.emplace_back("mode", _colorModeToString(picker->mode));
    }

    if (auto* grad = dynamic_cast<glint_gradient_editor*>(comp))
    {
      attrs.emplace_back("stops", _serializeGradientStops(grad->stops));
      attrs.emplace_back("direction", _floatToString(grad->direction));
      attrs.emplace_back("centerX", _floatToString(grad->centerX));
      attrs.emplace_back("centerY", _floatToString(grad->centerY));
      attrs.emplace_back("radius", _floatToString(grad->radius));
    }

    if (auto* list = dynamic_cast<glint_list*>(comp))
      attrs.emplace_back("highlightOnSelect", _boolToString(list->highlightOnSelect));

    return attrs;
  }

  // Hide the picker window and reset swatch state.  The window is kept alive
  // so the next _openPickerWindowBelow() can reopen it instantly via reopen().
  // mPickerGeneration is bumped to discard any in-flight WM_INSP_CP_CLOSED
  // posted by this hide() before the next open resets the generation.
  void _dismissPickerWindow()
  {
    if (mSwatchForPicker)
    {
      mSwatchForPicker->style.borderColor = glint_color(255, 110, 110, 110);
      mSwatchForPicker->setDirty(false);
      mSwatchForPicker = nullptr;
    }
    mInputForPicker = nullptr;
    mColorPickerKey.clear();
    ++mPickerGeneration;           // invalidate any in-flight WM_INSP_CP_CLOSED
    if (mPickerWindow) mPickerWindow->hide();  // hide but keep alive for reuse
  }

  // Close and forget the attribute-picker window.
  // Always null mAttrListWin BEFORE calling close() so the onClosed callback
  // arriving on the attr-list thread is a harmless no-op.
  void _dismissAttrList()
  {
    auto* w    = mAttrListWin;
    mAttrListWin = nullptr;
    if (w) w->close();
  }

  // Open the picker window positioned below `swatch` in screen-space.
  // mOwnerHWND must be set for ClientToScreen to work.
  void _openPickerWindowBelow(glint_element* swatch,
                              const std::string& key,
                              glint_color initialColor,
                              PropWriter writer)
  {
#if defined(_WIN32)
    if (!mRoot || !swatch) return;
    // Walk parent chain to accumulate scroll offsets (content-space → screen-space).
    float scrollX = 0.f, scrollY = 0.f;
    for (auto* p = swatch->mParent; p && p != &mRoot->mCanvas; p = p->mParent)
    { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }
    const glint_rect& sr = swatch->mPaintRECT;
    POINT tl = { static_cast<LONG>(sr.L - scrollX), static_cast<LONG>(sr.T - scrollY) };
    POINT br = { static_cast<LONG>(sr.R - scrollX), static_cast<LONG>(sr.B - scrollY) };
    if (mOwnerHWND) { ::ClientToScreen((HWND)mOwnerHWND, &tl); ::ClientToScreen((HWND)mOwnerHWND, &br); }
    RECT anchorRect = { tl.x, tl.y, br.x, br.y };

    const std::string k      = key;
    const HWND        ownerH = (HWND)mOwnerHWND;

    // Bump generation BEFORE building the onClosed lambda so the lambda
    // captures the correct generation value for this open.
    ++mPickerGeneration;
    const int gen = mPickerGeneration;

    auto onChange = [this, k, ownerH, writer](glint_color c)
    {
      if (mLiveComp) writer(k, _colorToHex(c));
      const uint32_t rgba = (static_cast<uint32_t>(c.A) << 24)
                          | (static_cast<uint32_t>(c.R) << 16)
                          | (static_cast<uint32_t>(c.G) <<  8)
                          |  static_cast<uint32_t>(c.B);
      if (ownerH) ::PostMessage(ownerH, WM_INSP_CP_CHANGED, (WPARAM)rgba, 0);
    };
    auto onClosed = [ownerH, gen]()
    {
      // Pass generation as LPARAM so the inspector can discard stale closes.
      if (ownerH) ::PostMessage(ownerH, WM_INSP_CP_CLOSED, 0, (LPARAM)(LONG_PTR)gen);
    };

    // Create a hidden-by-default window if not already pre-warmed, then always
    // use reopen() to position, seed the picker, apply callbacks, and show it.
    if (!mPickerWindow)
      mPickerWindow = glint_colorpicker_window::open(initialColor, anchorRect);
    mPickerWindow->reopen(initialColor, anchorRect,
                          std::move(onChange),
                          std::move(onClosed));
#elif defined(__APPLE__)
    if (!mRoot || !swatch || !mOwnerHWND) return;
    // Walk parent chain to accumulate scroll offsets (content-space → screen-space).
    float scrollX = 0.f, scrollY = 0.f;
    for (auto* p = swatch->mParent; p && p != &mRoot->mCanvas; p = p->mParent)
    { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }
    const glint_rect& sr = swatch->mPaintRECT;
    auto* macWin = static_cast<glint_window_mac*>(mOwnerHWND);
    const RECT anchorRect = macWin->contentRectToScreen(
      sr.L - scrollX, sr.T - scrollY, sr.W(), sr.H());

    const std::string k = key;
    ++mPickerGeneration;
    const int gen = mPickerGeneration;

    // On macOS everything runs on the main thread — call update functions directly.
    auto onChange = [this, k, writer](glint_color c)
    {
      if (mLiveComp) writer(k, _colorToHex(c));
      updateActiveSwatch(c);
      setDirty(false);
    };
    auto onClosed = [this, gen]()
    {
      onPickerClosed(gen);
      setDirty(false);
    };

    if (!mPickerWindow)
      mPickerWindow = glint_colorpicker_window::open(initialColor, anchorRect);
    mPickerWindow->reopen(initialColor, anchorRect,
                          std::move(onChange),
                          std::move(onClosed));
#else
    (void)swatch; (void)key; (void)initialColor; (void)writer;  // color picker not supported
#endif
  }
  // -- HTML Attributes section (innerText etc.) ----------------------------
  void buildAttributesSection()
  {
    if (!mLiveComp) return;

    buildLine("element {", 26.f, 8.f);

    // Helper to build a single attribute row
    auto buildAttrRow = [&](const char* attrName, const std::string& currentVal,
                             std::function<void(const std::string&)> onCommit)
    {
      auto* row              = new glint_element();
      row->style.display     = "flex";
      row->style.flexDirection = "row";
      row->style.alignItems  = "center";
      row->style.width       = "100%";
      row->style.height      = 24.f;
      row->style.marginLeft  = 14.f;
      row->style.paddingRight = 14.f;
      addChild(row);

      const bool canPreview = (std::strcmp(attrName, "src") == 0);

      // Leading spacer so element rows align with style rows, which reserve
      // this slot for the enable checkbox.
      auto* leadGap         = new glint_element();
      leadGap->style.width  = 11.f;
      leadGap->style.height = "100%";
      row->addChild(leadGap);

      // Property name
      auto* keyLbl           = new InspStaticText();
      keyLbl->text           = attrName;
      keyLbl->style.width    = 112.f;
      keyLbl->style.height   = "100%";
      keyLbl->style.fontSize = 13.f;
      keyLbl->style.color    = glint_color(255, 200, 200, 205);
      keyLbl->style.textAlign   = EAlign::Near;
      keyLbl->style.paddingLeft = 2.f;
      keyLbl->style.paddingRight = 4.f;
      row->addChild(keyLbl);

      // Colon
      auto* colon           = new InspStaticText();
      colon->text           = ":";
      colon->style.width    = 8.f;
      colon->style.height   = "100%";
      colon->style.color    = glint_color(255, 200, 200, 205);
      colon->style.fontSize = 13.f;
      row->addChild(colon);

      // Text input
      auto* inp              = new glint_input();
      inp->style.flexGrow    = 1.f;
      inp->style.height      = 22.f;
      inp->style.fontSize    = 13.f;
      inp->style.padding     = "0 2";
      inp->style.backgroundColor = glint_color(0, 0, 0, 0);
      inp->style.borderWidth = 0.f;
      inp->style.borderColor = glint_color(0, 0, 0, 0);
      inp->style.color       = glint_color(255, 255, 255, 255);
      inp->style.marginRight = 2.f;
      inp->onChange = [this, cb = std::move(onCommit)](const std::string& v) {
        if (!mLiveComp) return;
        cb(v);
        // Force an immediate layout pass on the main document so mRect is
        // updated right away (e.g. auto-sized elements resize with the new text).
        // tickTransitionsAll() must run first so computedStyle is synced from
        // style before childPrefH/W measures element sizes.
        if (mLiveComp->mRoot) {
          mLiveComp->mRoot->mCanvas.tickTransitionsAll();
          mLiveComp->mRoot->mCanvas.Layout(nullptr);
        }
        mLiveComp->setDirty(false);
      };
      inp->onFocus = [inp]() {
        inp->style.borderWidth  = 1.f;
        inp->style.borderRadius = "4px";
        inp->style.borderColor  = glint_color(255, 80, 140, 255);
        inp->setDirty(false);
      };
      inp->onBlur = [inp]() {
        inp->style.borderWidth = 0.f;
        inp->style.borderColor = glint_color(0, 0, 0, 0);
        inp->setDirty(false);
      };
      inp->setValue(currentVal);
      // Realtime flash: blue border that fades out when the value has changed.
      {
        const auto fit = mFlashProgress.find(std::string(attrName));
        if (fit != mFlashProgress.end() && fit->second > 0.f)
        {
          const int alpha         = static_cast<int>(fit->second * 255.f);
          inp->style.borderWidth  = 1.f;
          inp->style.borderRadius = "4px";
          inp->style.borderColor  = glint_color(alpha, 80, 140, 255);
        }
      }
      row->addChild(inp);

      if (canPreview && mOnRowEnter && mOnRowLeave)
      {
        row->element.addEventListener("mouseenter", [row, inp, this](glint_event&) {
          row->style.backgroundColor = glint_color(255, 60, 63, 82);
          row->setDirty(false);
          const std::string value = inp->getValue();
          if (value.empty()) return;
          float scrollX = 0.f, scrollY = 0.f;
          if (row->mRoot)
            for (auto* p = row->mParent; p && p != &row->mRoot->mCanvas; p = p->mParent)
            { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }
          mOnRowEnter("src", value,
                      row->mPaintRECT.L - scrollX,
                      row->mPaintRECT.T - scrollY,
                      row->mPaintRECT.B - scrollY);
        });
        row->element.addEventListener("mouseleave", [row, this](glint_event&) {
          row->style.backgroundColor = glint_color(0, 0, 0, 0);
          row->setDirty(false);
          mOnRowLeave();
        });
      }
      else
      {
        row->element.addEventListener("mouseenter", [row](glint_event&) {
          row->style.backgroundColor = glint_color(255, 60, 63, 82);
          row->setDirty(false);
        });
        row->element.addEventListener("mouseleave", [row](glint_event&) {
          row->style.backgroundColor = glint_color(0, 0, 0, 0);
          row->setDirty(false);
        });
      }

      // Match the style rows' extra space between ':' and value.
      auto* gap          = new glint_element();
      gap->style.width   = 4.f;
      gap->style.height  = "100%";
      row->addChild(gap);

      // Semicolon
      auto* semi             = new InspStaticText();
      semi->text             = ";";
      semi->style.width      = 10.f;
      semi->style.height     = "100%";
      semi->style.color      = glint_color(255, 200, 200, 205);
      semi->style.fontSize   = 13.f;
      row->addChild(semi);
    };

    auto* img    = dynamic_cast<glint_image*>(mLiveComp);
    auto* input  = dynamic_cast<glint_input*>(mLiveComp);
    auto* cb     = dynamic_cast<glint_checkbox*>(mLiveComp);
    auto* sel    = dynamic_cast<glint_select*>(mLiveComp);
    auto* dial   = dynamic_cast<glint_dial*>(mLiveComp);
    auto* picker = dynamic_cast<glint_colorpicker*>(mLiveComp);
    auto* grad   = dynamic_cast<glint_gradient_editor*>(mLiveComp);
    auto* list   = dynamic_cast<glint_list*>(mLiveComp);

    for (const auto& attr : _collectElementAttributeValues(mLiveComp))
    {
      const std::string& key = attr.first;
      const std::string& value = attr.second;

      if (key == "id")
      {
        buildAttrRow("id", value,
          [this](const std::string& v) { mLiveComp->id = v; });
      }
      else if (key == "className")
      {
        buildAttrRow("className", value,
          [this](const std::string& v) {
            mLiveComp->className = v;
            if (mLiveComp->mApplyCss) mLiveComp->mApplyCss(mLiveComp);
          });
      }
      else if (key == "innerText")
      {
        buildAttrRow("innerText", value,
          [this](const std::string& v) { mLiveComp->innerText = v; });
      }
      else if (img && key == "src")
      {
        buildAttrRow("src", value,
          [img](const std::string& v) { img->SetSrc(v.c_str(), img->numFrames); });
      }
      else if (input && key == "value")
      {
        buildAttrRow("value", value,
          [input](const std::string& v) { input->setValue(v); });
      }
      else if (input && key == "type")
      {
        buildAttrRow("type", value,
          [input](const std::string& v) { input->type = _trim(v); });
      }
      else if (input && key == "placeholder")
      {
        buildAttrRow("placeholder", value,
          [input](const std::string& v) { input->placeholder = v; });
      }
      else if (input && key == "min")
      {
        buildAttrRow("min", value,
          [input](const std::string& v) {
            float parsed = input->min;
            if (_tryParseFloat(v, parsed)) input->min = parsed;
          });
      }
      else if (input && key == "max")
      {
        buildAttrRow("max", value,
          [input](const std::string& v) {
            float parsed = input->max;
            if (_tryParseFloat(v, parsed)) input->max = parsed;
          });
      }
      else if (input && key == "readonly")
      {
        buildAttrRow("readonly", value,
          [input](const std::string& v) {
            bool parsed = input->readonly;
            if (_tryParseBool(v, parsed)) input->readonly = parsed;
          });
      }
      else if (input && key == "disabled")
      {
        buildAttrRow("disabled", value,
          [input](const std::string& v) {
            bool parsed = input->disabled;
            if (_tryParseBool(v, parsed)) input->disabled = parsed;
          });
      }
      else if (cb && key == "checked")
      {
        buildAttrRow("checked", value,
          [cb](const std::string& v) {
            bool parsed = cb->checked;
            if (_tryParseBool(v, parsed)) cb->SetChecked(parsed);
          });
      }
      else if (cb && key == "keepChecked")
      {
        buildAttrRow("keepChecked", value,
          [cb](const std::string& v) {
            bool parsed = cb->keepChecked;
            if (_tryParseBool(v, parsed)) cb->keepChecked = parsed;
          });
      }
      else if (sel && key == "selectedIndex")
      {
        buildAttrRow("selectedIndex", value,
          [sel](const std::string& v) {
            int parsed = sel->selectedIndex;
            if (!_tryParseInt(v, parsed)) return;
            sel->selectedIndex = parsed;
            if (sel->selectedIndex >= static_cast<int>(sel->options.size()))
              sel->selectedIndex = sel->options.empty() ? -1 : static_cast<int>(sel->options.size()) - 1;
          });
      }
      else if (sel && key == "placeholder")
      {
        buildAttrRow("placeholder", value,
          [sel](const std::string& v) { sel->placeholder = v; });
      }
      else if (sel && key == "options")
      {
        buildAttrRow("options", value,
          [sel](const std::string& v) {
            sel->options = _splitCommaList(v);
            if (sel->selectedIndex >= static_cast<int>(sel->options.size()))
              sel->selectedIndex = sel->options.empty() ? -1 : static_cast<int>(sel->options.size()) - 1;
          });
      }
      else if (dial && key == "angle")
      {
        buildAttrRow("angle", value,
          [dial](const std::string& v) {
            float parsed = dial->angle;
            if (_tryParseFloat(v, parsed)) dial->angle = parsed;
          });
      }
      else if (picker && key == "value")
      {
        buildAttrRow("value", value,
          [picker](const std::string& v) {
            const std::string colorText = _trim(v);
            if (glint_style_is_valid_by_name("color", colorText))
              picker->setValue(sk_color(colorText.c_str()).value);
          });
      }
      else if (picker && key == "mode")
      {
        buildAttrRow("mode", value,
          [picker](const std::string& v) {
            glint_colorpicker::ColorMode mode;
            if (_parseColorMode(v, mode)) picker->setMode(mode);
          });
      }
      else if (grad && key == "stops")
      {
        buildAttrRow("stops", value,
          [grad](const std::string& v) {
            std::vector<sk_gradient_stop> stops;
            if (_parseGradientStops(v, stops)) grad->setStops(std::move(stops));
          });
      }
      else if (grad && key == "direction")
      {
        buildAttrRow("direction", value,
          [grad](const std::string& v) {
            float parsed = grad->direction;
            if (_tryParseFloat(v, parsed))
            {
              grad->direction = parsed;
              if (grad->onDirectionChange) grad->onDirectionChange(grad->direction);
            }
          });
      }
      else if (grad && key == "centerX")
      {
        buildAttrRow("centerX", value,
          [grad](const std::string& v) {
            float parsed = grad->centerX;
            if (_tryParseFloat(v, parsed))
            {
              grad->centerX = std::clamp(parsed, 0.f, 1.f);
              if (grad->onCenterChange) grad->onCenterChange(grad->centerX, grad->centerY);
            }
          });
      }
      else if (grad && key == "centerY")
      {
        buildAttrRow("centerY", value,
          [grad](const std::string& v) {
            float parsed = grad->centerY;
            if (_tryParseFloat(v, parsed))
            {
              grad->centerY = std::clamp(parsed, 0.f, 1.f);
              if (grad->onCenterChange) grad->onCenterChange(grad->centerX, grad->centerY);
            }
          });
      }
      else if (grad && key == "radius")
      {
        buildAttrRow("radius", value,
          [grad](const std::string& v) {
            float parsed = grad->radius;
            if (_tryParseFloat(v, parsed))
            {
              grad->radius = std::max(0.01f, parsed);
              if (grad->onRadiusChange) grad->onRadiusChange(grad->radius);
            }
          });
      }
      else if (list && key == "highlightOnSelect")
      {
        buildAttrRow("highlightOnSelect", value,
          [list](const std::string& v) {
            bool parsed = list->highlightOnSelect;
            if (_tryParseBool(v, parsed)) list->highlightOnSelect = parsed;
          });
      }
    }

    buildLine("}", 22.f, 8.f);

    // Gap between attributes and style sections
    auto* gap        = new glint_element();
    gap->style.width = "100%";
    gap->style.height = 6.f;
    addChild(gap);
  }

  // -- dim text line (header / footer) ---------------------------------------
  void buildLine(const char* txt, float height, float leftPad)
  {
    auto* lbl           = new InspStaticText();
    lbl->text                = txt;
    lbl->style.width    = "100%";
    lbl->style.height   = height;
    lbl->style.color    = glint_color(255, 200, 200, 205);
    lbl->style.fontSize = 13.f;
    lbl->style.paddingLeft  = leftPad;
    addChild(lbl);
  }

  // -- CSS rule origin block helpers ----------------------------------------
  //
  // Renders one matched CSS rule in the style of Chrome DevTools' Styles panel:
  //
  //   #header {                     ← selector in link-blue, specificity badge
  //     background-color : #111 ;   ← prop rows; overridden ones are dimmed
  //   }

  // Header row: [selector {]                     [main.css:123]
  //              blue, left                       dim green, right
  CssSaveButton* buildCssRuleHeader(const std::string& selectorText,
                          const GlintCssSpecificity& spec,
                          const std::string& sourceUrl,
                          uint32_t sourceLine,
                          bool isActive,
                          std::shared_ptr<bool>   dirtyFlag = {},
                          std::function<void()>   onSave    = {})
  {
    (void)spec;  // specificity used for overridden-dimming but not displayed

    // Build origin label: "main.css:123"
    std::string originLabel;
    if (!sourceUrl.empty())
    {
      std::string filename = sourceUrl;
      const size_t slash = filename.find_last_of("/\\");
      if (slash != std::string::npos) filename = filename.substr(slash + 1);
      char buf[256];
      if (sourceLine > 0)
        snprintf(buf, sizeof(buf), "%s:%u", filename.c_str(), sourceLine);
      else
        snprintf(buf, sizeof(buf), "%s", filename.c_str());
      originLabel = buf;
    }

    auto* row = new glint_element();
    row->style.display       = "flex";
    row->style.flexDirection = "row";
    row->style.alignItems    = "center";
    row->style.width         = "100%";
    row->style.height        = 22.f;
    row->style.paddingLeft   = 8.f;
    row->style.paddingRight  = 8.f;
    if (!isActive)
      row->style.filter = "opacity(0.65)";
    addChild(row);

    // Selector text — link-blue, left-aligned.
    auto* sel = new InspStaticText();
    sel->text             = selectorText + " {";
    sel->style.flexGrow   = 1.f;
    sel->style.height     = "100%";
    sel->style.fontSize   = 13.f;
    sel->style.color      = glint_color(255, 100, 170, 255);
    sel->style.textAlign  = EAlign::Near;
    row->addChild(sel);

    // Source origin — dim green, right-aligned.
    if (!originLabel.empty())
    {
      auto* origin = new InspStaticText();
      origin->text             = originLabel;
      origin->style.height     = "100%";
      origin->style.fontSize   = 11.f;
      origin->style.color      = glint_color(255, 100, 160, 110);
      origin->style.textAlign  = EAlign::Far;
      row->addChild(origin);
    }

    // Save button — always in DOM, invisible when clean (CssSaveButton::DrawToCanvas
    // skips paint when *dirtyFlag == false so no yellow box flickers on load).
    CssSaveButton* saveBtn = nullptr;
    if (dirtyFlag)
    {
      saveBtn             = new CssSaveButton();
      saveBtn->dirtyFlag  = dirtyFlag;
      if (onSave) saveBtn->onClick = std::move(onSave);
      saveBtn->style.width      = 18.f;
      saveBtn->style.height     = 14.f;
      saveBtn->style.marginLeft = 4.f;
      row->addChild(saveBtn);
    }
    return saveBtn;
  }

  // Editable property row for CSS rule declarations.
  // `decl` is a live pointer into the stylesheet data — edits write back immediately.
  // `onApply` is called after a valid edit to apply the change to the live element.
  // Overridden properties are dimmed to 55% opacity.
  // Render one matched CSS rule block: header, property rows, footer.
  // `claimed` tracks which properties are already won by a higher-priority rule.
  void buildCssRuleBlock(const GlintMatchedCssRule& rule,
                         std::unordered_set<std::string>& claimed,
                         bool ruleIsActive)
  {
    const std::string ruleUrl  = rule.sourceUrl;
    const uint32_t    ruleLine = rule.sourceLine;
    const std::string ruleKey  = ruleUrl + "|" + std::to_string(ruleLine);
    const bool ruleClaimsProperties = ruleIsActive && !selectorHasPseudo(rule.selectorText);

    // Shared dirty flag: writer sets it true (no rebuild needed); save button
    // checks it in DrawToCanvas and resets it on click.
    auto dirtyFlag  = std::make_shared<bool>(mDirtyRules.count(ruleKey) > 0);
    auto saveBtnRef = std::make_shared<CssSaveButton*>(nullptr);

    auto onSave = [this, ruleUrl, ruleLine, ruleKey, dirtyFlag, saveBtnRef]() {
      if (mDocument) mDocument->saveRuleToFile(ruleUrl, ruleLine);
      *dirtyFlag = false;
      mDirtyRules.erase(ruleKey);
      if (*saveBtnRef) (*saveBtnRef)->setDirty(false);
    };

    *saveBtnRef = buildCssRuleHeader(rule.selectorText, rule.specificity,
                     ruleUrl, ruleLine, ruleIsActive, dirtyFlag, onSave);

    // Marks the rule dirty — shared by the writer lambda and the disable checkbox.
    auto markDirty = [this, ruleKey, dirtyFlag, saveBtnRef]() {
      if (!*dirtyFlag) {
        *dirtyFlag = true;
        mDirtyRules.insert(ruleKey);
        if (*saveBtnRef) (*saveBtnRef)->setDirty(false);
      }
    };

    // Writer routes edits into the stylesheet AST (not element.style).
    PropWriter writer = [this, ruleUrl, ruleLine, markDirty]
                        (const std::string& p, const std::string& v)
    {
      if (!mLiveComp || !mDocument) return;
      mDocument->updateCssDeclaration(ruleUrl, ruleLine, p, v, mLiveComp);
      markDirty();
    };

    for (const auto& decl : rule.declarations)
    {
      const std::string dId  = _declId(ruleUrl, rule.selectorText, decl.property);
      const bool overridden   = ruleClaimsProperties && claimed.count(decl.property) > 0;
      const std::string dProp = decl.property;
      PropDeleter deleter = [this, ruleUrl, ruleLine, dProp]() {
        if (mDocument && mLiveComp)
          mDocument->removeCssDeclaration(ruleUrl, ruleLine, dProp, mLiveComp);
        mRebuildPending = true;
        setDirty(false);
      };
      buildPropRow(decl.property, decl.value, writer, overridden, !ruleIsActive, dId, std::move(deleter), markDirty);
      // A disabled declaration does not "own" its property — a lower-priority
      // rule for the same key should not be shown as overridden.
      if (ruleClaimsProperties && !overridden && !mDisabledDecls.count(dId))
        claimed.insert(decl.property);
    }

    buildCssAddAttrButton(rule);
    buildLine("}", 22.f, 8.f);
  }

  // Add button at the bottom of a matched CSS rule block.
  // Reuses InspAddAttrButton but stashes (ruleUrl, ruleLine) and the rule's
  // existing keys so the picker filters them as already-set.
  void buildCssAddAttrButton(const GlintMatchedCssRule& rule)
  {
    auto* btn        = new InspAddAttrButton();
    btn->mPanel      = this;
    btn->mRuleUrl    = rule.sourceUrl;
    btn->mRuleLine   = rule.sourceLine;
    for (const auto& d : rule.declarations)
      btn->mExistingKeys.insert(d.property);
    addChild(btn);
  }

  // -- One property row ------------------------------------------------------
  //   [chk] [name 112px right] [:] [swatch?] [value-widget] [;] [x]
  //
  //   value-widget: color ? 14�14 swatch + hex input (click swatch ? picker window)
  //                 enum  ? click-to-cycle label (light blue)
  //                 bool  ? glint_checkbox + flex spacer
  //                 other ? plain glint_input (white text, wheel-adjustable)
  //
  void buildPropRow(const std::string& key, const std::string& val,
                    PropWriter writer = {}, bool overridden = false,
                    bool inactive = false,
                    const std::string& declId = {},
                    PropDeleter deleter = {},
                    std::function<void()> onDirty = {})
  {
    // Composite id used for the disabled-state maps.  Falls back to the
    // inline sentinel when no explicit id is supplied (should not happen).
    const std::string dId = declId.empty() ? _declId("", "", key) : declId;
    // When the gradient editor is shown in-place, suppress the plain text/swatch widgets.
    // Only applies to "background" (always gradient when stops are present).
    // isBgGrad is ONLY true for element.style rows (writer == null); CSS rule rows use
    // isCssRuleBgGrad so they read from the rule's declared value, not the live element.
    const bool isBgGrad = mLiveComp
                          && !mLiveComp->style.backgroundGradient.empty()
                          && key == "background"
                          && !writer;  // ← element.style only
    const bool isMaskGrad = (key == "mask" && _isCssGradientString(val));
    // CSS rule row whose declared value is a CSS gradient string.
    const bool isCssRuleBgGrad = (bool)writer && key == "background" && _isCssGradientString(val);
    const bool isGradRow = isBgGrad || isMaskGrad || isCssRuleBgGrad;

    // In flat mode the "background" row edits backgroundColor directly.
    // Use effectiveKey for all lambdas so reads/writes go to the right property.
    const std::string effectiveKey = (!isGradRow && key == "background" && mBgFlatMode)
                                     ? std::string("background-color") : key;

    const bool  isColor    = _isColorKey(key) || (!isGradRow && key == "background" && mBgFlatMode);
    const bool  isBool     = (key == "shadow-enabled");
    const auto* opts       = _enumValues(key);

    // Route writes to the correct style layer: CSS rule (via writer) or element.style (inline).
    PropWriter _write = writer ? writer : PropWriter{[this](const std::string& p, const std::string& v)
    {
      if (!mLiveComp) return;
      glint_style_set_by_name(mLiveComp->style, p, v);
      mLiveComp->setDirty(false);
    }};

    // -- Row container
    const bool rowIsDisabled = mDisabledDecls.count(dId) > 0;
    auto* row              = new glint_element();
    row->style.display     = "flex";
    row->style.flexDirection = "row";
    row->style.alignItems  = "center";
    row->style.width       = "100%";
    row->style.height      = 24.f;
    row->style.marginLeft  = 14.f;
    row->style.paddingRight = 14.f;
    addChild(row);

    // Dim the row based on its initial state (disabled takes priority over overridden).
    if (rowIsDisabled)
      row->style.filter = "opacity(0.35)";
    else if (overridden)
      row->style.filter = "opacity(0.45)";
    else if (inactive)
      row->style.filter = "opacity(0.65)";

    // Hover highlight — listeners are safe because they never rebuild
    // Detect img-bearing keys so the preview popup can be triggered.
    static const char* const kImgKeys[] = {
      "background-img", "background", "mask", nullptr
    };
    bool _hasUrl = false;
    for (const char* const* k = kImgKeys; *k; ++k)
    {
      if (key == *k && val.find("url(") != std::string::npos)
      { _hasUrl = true; break; }
    }
    if (_hasUrl && mOnRowEnter && mOnRowLeave)
    {
      // Capture key+val for preview; the existing highlight lambda is replaced
      // by these versions that also fire the popup callbacks.
      row->element.addEventListener("mouseenter",
        [row, k = key, v = val, this](glint_event&)
        {
          row->style.backgroundColor = glint_color(255, 60, 63, 82);
          row->setDirty(false);
          float scrollX = 0.f, scrollY = 0.f;
          if (row->mRoot)
            for (auto* p = row->mParent; p && p != &row->mRoot->mCanvas; p = p->mParent)
            { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }
          mOnRowEnter(k, v, row->mPaintRECT.L - scrollX,
                           row->mPaintRECT.T - scrollY,
                           row->mPaintRECT.B - scrollY);
        });
      row->element.addEventListener("mouseleave",
        [row, this](glint_event&)
        {
          row->style.backgroundColor = glint_color(0, 0, 0, 0);
          row->setDirty(false);
          mOnRowLeave();
        });
    }
    else
    {
      row->element.addEventListener("mouseenter", [row](glint_event&) {
        row->style.backgroundColor = glint_color(255, 60, 63, 82);
        row->setDirty(false);
      });
      row->element.addEventListener("mouseleave", [row](glint_event&) {
        row->style.backgroundColor = glint_color(0, 0, 0, 0);
        row->setDirty(false);
      });
    }

    // Checkbox — leftmost; unchecking dims the row and bypasses the style on
    // the live component (resets to default). Re-checking restores the value.
    auto* chk            = new glint_checkbox();
    chk->checked         = !rowIsDisabled;
    chk->text            = "";     // no label text
    chk->size            = 11.f;
    chk->style.marginLeft  = 4.f;
    chk->style.marginRight = 2.f;
    chk->style.height      = "100%";
    chk->style.alignItems  = "center";
    const std::string kForChk = key;
    // Shared enabled flag: value widgets check this before writing to mLiveComp.
    auto rowEnabled = std::make_shared<bool>(!rowIsDisabled);
    // Shared so that the uncheck branch can write the latest value for restore.
    // Initialise from the persistent saved-value map so a rebuild after disable
    // preserves the value for when the user re-enables.
    auto savedVal = std::make_shared<std::string>(
        mDisabledSavedVals.count(dId) ? mDisabledSavedVals.at(dId) : val);
    // Shared reference to the text input (filled in below, after inp is created).
    auto inpRef = std::make_shared<glint_input*>(nullptr);
    // True for inline element.style rows — disable resets the live computed
    // style to its default.  For CSS rule rows this is false: the stylesheet
    // value must be left untouched; the row is simply excluded from the
    // cascade "claimed" set so lower-priority declarations become active.
    const bool isInlineRow = (dId == _declId("", "", key));
    // Capture dId by value so the lambda identifies exactly this declaration.
    chk->onChange = [this, kForChk, dId, isInlineRow, row, savedVal, rowEnabled, inpRef, _write, overridden, inactive, onDirty](bool checked) {
      *rowEnabled = checked;
      if (!checked) {
        mDisabledDecls.insert(dId);
        if (mColorPickerKey == kForChk) _dismissPickerWindow();
        row->style.filter = "opacity(0.35)";
        // Always persist the original value BEFORE any write so the next
        // rebuild can restore it regardless of what _write puts in the sheet.
        // For inline rows, try to read the freshest live value first.
        mDisabledSavedVals[dId] = *savedVal;
        if (isInlineRow && mLiveComp) {
          const auto curInfo = glint_style_serialize(mLiveComp->style);
          auto it = curInfo.find(kForChk);
          if (it != curInfo.end()) {
            *savedVal = it->second;
            mDisabledSavedVals[dId] = it->second;
          }
          // Inline rows: write default to el->style so _mergedStyle() stops
          // applying this property and any CSS rule value can show through.
          const auto& def = glint_default_style_info();
          auto dit = def.find(kForChk);
          if (dit != def.end())
            _write(kForChk, dit->second);
        }
        // CSS rule rows: DO NOT call _write — the stylesheet is left untouched.
        // The cascade engine filters this decl via mInspDisabledDecls instead.
        // Mark the rule dirty so the save button appears and Ctrl+S works.
        if (!isInlineRow && onDirty) onDirty();
        // CSS rule rows affect every element matching the selector — walk the
        // whole tree so sibling/ancestor elements with the same class update.
        if (mDocument)
        {
          if (isInlineRow) mDocument->reapplyCss(mLiveComp);
          else             mDocument->reapplyAllCss();
        }
      } else {
        mDisabledDecls.erase(dId);
        mDisabledSavedVals.erase(dId);
        row->style.filter = overridden ? "opacity(0.45)" : (inactive ? "opacity(0.65)" : "");
        if (mLiveComp && !savedVal->empty()) {
          // Inline rows: restore saved value to el->style.
          // CSS rule rows: restore in case the user typed a different value
          // while the row was disabled; if unchanged, this is a no-op.
          std::string valueToApply = *savedVal;
          if (*inpRef) {
            const std::string currentInputVal = (*inpRef)->getValue();
            if (!currentInputVal.empty() && currentInputVal != *savedVal &&
                glint_style_is_valid_by_name(kForChk, currentInputVal))
              valueToApply = currentInputVal;
          }
          _write(kForChk, valueToApply);
        }
        // Re-cascade so the restored declaration is immediately effective.
        // CSS rule rows: walk the whole tree — all elements with the same class
        // must update.  Inline rows: only the inspected element needs it.
        if (mDocument)
        {
          if (isInlineRow) mDocument->reapplyCss(mLiveComp);
          else             mDocument->reapplyAllCss();
        }
      }
      // Rebuild the panel so the "claimed" set is recomputed and any previously
      // overridden row that this declaration was masking gets its strikethrough removed.
      mRebuildPending = true;
      row->setDirty(false);
      setDirty(false);
    };
    row->addChild(chk);

    // Property name � InspNameButton: click ? "Reset to default" popup
    auto* nameBtn         = new InspNameButton();
    nameBtn->mKey         = key;
    nameBtn->mPanel       = this;
    nameBtn->mLiveComp    = mLiveComp;
    nameBtn->mWriter      = _write;
    nameBtn->innerText    = key;
    nameBtn->style.width          = 112.f;
    nameBtn->style.height         = "100%";
    nameBtn->style.fontSize       = 13.f;
    nameBtn->style.paddingLeft    = 2.f;
    nameBtn->style.paddingRight   = 4.f;
    nameBtn->style.color          = overridden ? glint_color(255, 140, 140, 140)
                                               : glint_color(255, 204, 204, 204);   // 80% white
    nameBtn->style.textDecoration = overridden ? "line-through" : "none";
    nameBtn->style.textAlign      = EAlign::Near;                 // left-aligned
    nameBtn->style.backgroundColor = glint_color(0, 0, 0, 0);
    nameBtn->style.borderWidth    = 0.f;
    // Hover: pure white
    nameBtn->hover.color          = glint_color(255, 255, 255, 255);
    nameBtn->hover.backgroundColor = glint_color(0, 0, 0, 0);
    nameBtn->hover.borderWidth    = 0.f;
    nameBtn->hover.fontSize       = 13.f;
    row->addChild(nameBtn);

    // " : " separator
    auto* colon            = new InspStaticText();
    colon->text                 = ":";
    colon->style.width     = 8.f;
    colon->style.height    = "100%";
    colon->style.color     = glint_color(255, 200, 200, 205);
    colon->style.fontSize  = 13.f;
    row->addChild(colon);

    // 2px gap between : and value
    auto* gap          = new glint_element();
    gap->style.width   = 4.f;
    gap->style.height  = "100%";
    row->addChild(gap);

    const std::string k = effectiveKey;
    // True when this row was built as the flat-colour mode proxy for "background".
    const bool isBgFlatRow = (!isBgGrad && key == "background" && mBgFlatMode);
    auto maskGradState = std::make_shared<glint_style>();
    if (isMaskGrad || isCssRuleBgGrad) _parseCssGradient(*maskGradState, val);

    // -- Gradient-mode toggle button (backgroundColor and background keys) --
    // A small 16�16 button showing a gradient strip.  Dim when in flat mode,
    // vivid (blue?orange) when in gradient mode.  Clicking calls
    // toggleBgGradientMode() which seeds / clears backgroundGradient and
    // triggers a panel rebuild.
    // Only show for element.style rows — CSS rule rows don't toggle the inline style.
    if (key == "background" && !writer)
    {
      auto* gmb      = new GradModeButton();
      gmb->mPanel    = this;
      gmb->mGradMode = isBgGrad;
      row->addChild(gmb);
    }

    // -- Color swatch (color keys only, skipped in gradient mode) ------------
    // Click listener is wired later so it can also capture inp.
    glint_element* swatchLocal = nullptr;
    if (isColor && !isGradRow)
    {
      auto* sw               = new glint_element();
      sw->style.width        = 14.f;
      sw->style.height       = 14.f;
      sw->style.borderRadius = 2.f;
      sw->style.borderWidth  = 1.f;
      sw->style.borderColor  = (key == mColorPickerKey) ? glint_color(255, 100, 180, 255)
                                                        : glint_color(255, 110, 110, 110);
      sw->style.marginRight  = 4.f;
      if (!val.empty()) { sk_color c; c = val.c_str(); sw->style.backgroundColor = c; }
      else              { sw->style.backgroundColor = glint_color(255, 55, 55, 55); }
      row->addChild(sw);
      swatchLocal = sw;
      if (key == mColorPickerKey) mSwatchForPicker = sw;
    }

    // -- Value widget ---------------------------------------------------------
    glint_input* inp = nullptr;

    if (isGradRow)
    {
      // ── Gradient mode: top row has preview swatch + CSS string input ──────
      // ── Sub-row below has the visual gradient editor ──────────────────────

      const bool useLiveGradientState = isBgGrad;

      // Tiny preview swatch showing the first stop colour.
      auto* sw = new glint_element();
      sw->style.width        = 14.f;
      sw->style.height       = 14.f;
      sw->style.borderRadius = 2.f;
      sw->style.borderWidth  = 1.f;
      sw->style.borderColor  = glint_color(255, 110, 110, 110);
      sw->style.marginRight  = 4.f;
      const std::vector<sk_gradient_stop>* initialStops = nullptr;
      if (useLiveGradientState) initialStops = &mLiveComp->style.backgroundGradient;
      else                      initialStops = &maskGradState->backgroundGradient;
      if (initialStops && !initialStops->empty())
        sw->style.backgroundColor = initialStops->front().color;
      row->addChild(sw);

      // Shared pointers so onChange closures of inp2 and ge can cross-update.
      auto inpSh = std::make_shared<glint_input*>(nullptr);
      auto geSh  = std::make_shared<glint_gradient_editor*>(nullptr);

      // CSS gradient string input.
      auto* inp2                = new glint_input();
      *inpRef                   = inp2;
      inp2->style.flexGrow      = 1.f;
      inp2->style.height        = 22.f;
      inp2->style.fontSize      = 13.f;
      inp2->style.padding       = "0 2";
      inp2->style.backgroundColor = glint_color(0, 0, 0, 0);
      inp2->style.borderWidth   = 0.f;
      inp2->style.borderColor   = glint_color(0, 0, 0, 0);
      inp2->style.color         = glint_color(255, 120, 195, 255);
      inp2->style.marginRight   = 2.f;
      *inpSh = inp2;
      inp2->onChange = [this, k, inpSh, geSh, rowEnabled, sw, _write](const std::string& v) {
        if (!mLiveComp || !*rowEnabled) return;
        // Emptying the field turns off gradient mode.
        if (v.empty()) {
          mLiveComp->style.backgroundGradient.clear();
          mLiveComp->setDirty(false);
          mBgFlatMode = true;
          mRebuildPending = true;
          setDirty(false);
          return;
        }
        if (!glint_style_is_valid_by_name(k, v)) return;
        _write(k, v);
        if (*geSh && !mLiveComp->style.backgroundGradient.empty()) {
          (*geSh)->stops       = mLiveComp->style.backgroundGradient;
          (*geSh)->direction   = mLiveComp->style.backgroundGradientAngle;
          (*geSh)->gradientType= mLiveComp->style.backgroundGradientType;
          (*geSh)->setDirty(false);
        }
        if (!mLiveComp->style.backgroundGradient.empty())
          sw->style.backgroundColor = mLiveComp->style.backgroundGradient.front().color;
        sw->setDirty(false);
      };
      if (isMaskGrad || isCssRuleBgGrad)
      {
        inp2->onChange = [this, k, inpSh, geSh, rowEnabled, sw, _write, maskGradState](const std::string& v) {
          if (!mLiveComp || !*rowEnabled) return;
          if (v.empty()) {
            maskGradState->backgroundGradient.clear();
            _write(k, v);
            mRebuildPending = true;
            setDirty(false);
            return;
          }
          if (!glint_style_is_valid_by_name(k, v)) return;
          _write(k, v);
          glint_style parsed;
          if (_parseCssGradient(parsed, v)) {
            *maskGradState = parsed;
            if (*geSh) {
              (*geSh)->stops        = maskGradState->backgroundGradient;
              (*geSh)->direction    = maskGradState->backgroundGradientAngle;
              (*geSh)->gradientType = maskGradState->backgroundGradientType;
              (*geSh)->centerX      = maskGradState->backgroundGradientCX;
              (*geSh)->centerY      = maskGradState->backgroundGradientCY;
              (*geSh)->radius       = maskGradState->backgroundGradientRadius;
              (*geSh)->setDirty(false);
            }
            if (!maskGradState->backgroundGradient.empty())
              sw->style.backgroundColor = maskGradState->backgroundGradient.front().color;
            sw->setDirty(false);
          }
        };
      }
      inp2->onFocus = [inp2]() {
        inp2->style.borderWidth  = 1.f;
        inp2->style.borderRadius = "4px";
        inp2->style.borderColor  = glint_color(255, 80, 140, 255);
        inp2->setDirty(false);
      };
      inp2->onBlur = [inp2]() {
        inp2->style.borderWidth = 0.f;
        inp2->style.borderColor = glint_color(0, 0, 0, 0);
        inp2->setDirty(false);
      };
      row->addChild(inp2);
      if (overridden) inp2->style.textDecoration = "line-through";
      if (useLiveGradientState) {
        inp2->setValue(_gradientToCssString(
            mLiveComp->style.backgroundGradient,
            mLiveComp->style.backgroundGradientType,
            mLiveComp->style.backgroundGradientAngle,
            mLiveComp->style.backgroundGradientDirection,
            mLiveComp->style.backgroundGradientCX,
            mLiveComp->style.backgroundGradientCY));
      } else {
        inp2->setValue(_gradientToCssString(
            maskGradState->backgroundGradient,
            maskGradState->backgroundGradientType,
            maskGradState->backgroundGradientAngle,
            maskGradState->backgroundGradientDirection,
            maskGradState->backgroundGradientCX,
            maskGradState->backgroundGradientCY));
      }

      // Sub-row: full-width gradient editor below the main row.
      auto* geRow             = new glint_element();
      geRow->style.display    = "flex";
      geRow->style.width      = "100%";
      geRow->style.height     = kGE_BaseH;
      geRow->style.marginLeft = 14.f;
      geRow->style.paddingRight = 14.f;
      addChild(geRow);

      auto* ge = new glint_gradient_editor();
      *geSh = ge;
      ge->style.width    = 0.f;
      ge->style.flexGrow = 1.f;
      ge->style.height   = kGE_BaseH;
      if (useLiveGradientState) {
        ge->stops        = mLiveComp->style.backgroundGradient;
        ge->direction    = mLiveComp->style.backgroundGradientAngle;
        ge->gradientType = mLiveComp->style.backgroundGradientType;
        ge->centerX      = mLiveComp->style.backgroundGradientCX;
        ge->centerY      = mLiveComp->style.backgroundGradientCY;
        ge->radius       = mLiveComp->style.backgroundGradientRadius;

        auto _syncCss = [this, inpSh, sw]() {
          const std::string css = _gradientToCssString(
              mLiveComp->style.backgroundGradient,
              mLiveComp->style.backgroundGradientType,
              mLiveComp->style.backgroundGradientAngle,
              mLiveComp->style.backgroundGradientDirection,
              mLiveComp->style.backgroundGradientCX,
              mLiveComp->style.backgroundGradientCY);
          if (*inpSh) { (*inpSh)->setValue(css); (*inpSh)->setDirty(false); }
          if (!mLiveComp->style.backgroundGradient.empty())
            sw->style.backgroundColor = mLiveComp->style.backgroundGradient.front().color;
          sw->setDirty(false);
        };

        ge->onChange = [this, k, _syncCss, _write](const std::vector<sk_gradient_stop>& stops) {
          if (!mLiveComp) return;
          std::string gs;
          for (const auto& st : stops) {
            if (!gs.empty()) gs += '|';
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.3f:", st.position); gs += buf;
            char h[12]; std::snprintf(h, sizeof(h), "#%02x%02x%02x%02x",
                st.color.R, st.color.G, st.color.B, st.color.A); gs += h;
          }
          _write(k, gs);
          _syncCss();
        };
        ge->onDirectionChange = [this, _syncCss](float deg) {
          if (!mLiveComp) return;
          mLiveComp->style.backgroundGradientAngle = deg;
          mLiveComp->style.backgroundGradientDirection.clear();
          mLiveComp->setDirty(false); _syncCss();
        };
        ge->onTypeChange = [this, _syncCss](const std::string& t) {
          if (!mLiveComp) return;
          mLiveComp->style.backgroundGradientType = t;
          mLiveComp->setDirty(false); _syncCss();
        };
        ge->onCenterChange = [this, _syncCss](float cx, float cy) {
          if (!mLiveComp) return;
          mLiveComp->style.backgroundGradientCX = cx;
          mLiveComp->style.backgroundGradientCY = cy;
          mLiveComp->setDirty(false);
          _syncCss();
        };
        ge->onRadiusChange = [this](float r) {
          if (!mLiveComp) return;
          mLiveComp->style.backgroundGradientRadius = r;
          mLiveComp->setDirty(false);
        };
      } else {
        ge->stops        = maskGradState->backgroundGradient;
        ge->direction    = maskGradState->backgroundGradientAngle;
        ge->gradientType = maskGradState->backgroundGradientType;
        ge->centerX      = maskGradState->backgroundGradientCX;
        ge->centerY      = maskGradState->backgroundGradientCY;
        ge->radius       = maskGradState->backgroundGradientRadius;

        auto _syncMaskCss = [inpSh, sw, maskGradState, _write, k]() {
          const std::string css = _gradientToCssString(
              maskGradState->backgroundGradient,
              maskGradState->backgroundGradientType,
              maskGradState->backgroundGradientAngle,
              maskGradState->backgroundGradientDirection,
              maskGradState->backgroundGradientCX,
              maskGradState->backgroundGradientCY);
          _write(k, css);
          if (*inpSh) { (*inpSh)->setValue(css); (*inpSh)->setDirty(false); }
          if (!maskGradState->backgroundGradient.empty())
            sw->style.backgroundColor = maskGradState->backgroundGradient.front().color;
          sw->setDirty(false);
        };

        ge->onChange = [maskGradState, _syncMaskCss](const std::vector<sk_gradient_stop>& stops) {
          maskGradState->backgroundGradient = stops;
          _syncMaskCss();
        };
        ge->onDirectionChange = [maskGradState, _syncMaskCss](float deg) {
          maskGradState->backgroundGradientAngle = deg;
          maskGradState->backgroundGradientDirection.clear();
          _syncMaskCss();
        };
        ge->onTypeChange = [maskGradState, _syncMaskCss](const std::string& t) {
          maskGradState->backgroundGradientType = t;
          _syncMaskCss();
        };
        ge->onCenterChange = [maskGradState, _syncMaskCss](float cx, float cy) {
          maskGradState->backgroundGradientCX = cx;
          maskGradState->backgroundGradientCY = cy;
          _syncMaskCss();
        };
        ge->onRadiusChange = [maskGradState](float r) {
          maskGradState->backgroundGradientRadius = r;
        };
      }
      geRow->addChild(ge);
    }
    else if (isBool)
    {
      auto* chkBool  = new glint_checkbox();
      chkBool->checked         = (val == "true" || val == "1");
      chkBool->size            = 11.f;
      chkBool->style.marginRight = 4.f;
      chkBool->onChange = [this, k, rowEnabled, _write](bool v) {
        if (!mLiveComp) return;
        if (!*rowEnabled) return;
        _write(k, v ? "true" : "false");
      };
      row->addChild(chkBool);
      auto* sp = new glint_element();
      sp->style.flexGrow = 1.f;  sp->style.width = 0.f;  sp->style.height = 1.f;
      row->addChild(sp);
    }
    else if (opts)
    {
      auto* btn              = new InspEnumButton();
      btn->mKey              = k;
      btn->mOpts             = opts;
      btn->mLiveComp         = mLiveComp;
      btn->mRowEnabled       = rowEnabled;
      btn->mWriter           = _write;
      btn->innerText         = val.empty() ? "(none)" : val;
      btn->style.fontSize    = 13.f;
      btn->style.color       = glint_color(255, 120, 195, 255);
      btn->style.textAlign   = EAlign::Near;
      btn->style.padding     = "0 2";
      btn->style.backgroundColor  = glint_color(0, 0, 0, 0);
      btn->style.borderWidth      = 0.f;
      btn->hover.color            = glint_color(255, 160, 215, 255);
      btn->hover.backgroundColor  = glint_color(0, 0, 0, 0);
      btn->hover.borderWidth      = 0.f;
      btn->hover.fontSize         = 13.f;
      row->addChild(btn);
      if (overridden) btn->style.textDecoration = "line-through";
      // Realtime flash: blue border on the enum button if value changed.
      {
        const auto fit = mFlashProgress.find(key);
        if (fit != mFlashProgress.end() && fit->second > 0.f)
        {
          const int alpha         = static_cast<int>(fit->second * 255.f);
          btn->style.borderWidth  = 1.f;
          btn->style.borderRadius = "4px";
          btn->style.borderColor  = glint_color(alpha, 80, 140, 255);
        }
      }
    }
    else
    {
      inp                        = new glint_input();
      *inpRef                    = inp;   // allow chk->onChange to read the live input value
      if (!mFocusAfterBuild.empty() && key == mFocusAfterBuild)
        mInputToFocus = inp;              // will be focused after show() returns
      inp->style.flexGrow        = 1.f;
      inp->style.height          = 22.f;
      inp->style.fontSize        = 13.f;
      inp->style.padding         = "0 2";
      inp->style.backgroundColor = glint_color(0, 0, 0, 0);
      inp->style.borderWidth     = 0.f;
      inp->style.borderColor     = glint_color(0, 0, 0, 0);
      inp->style.color           = isColor ? glint_color(255, 145, 225, 145)
                                           : glint_color(255, 255, 255, 255);
      inp->style.marginRight     = 2.f;

      if (isColor && key == mColorPickerKey) mInputForPicker = inp;

      glint_element* swRef = swatchLocal;
      inp->onChange = [this, inp, k, swRef, rowEnabled, isBgFlatRow, _write](const std::string& v)
      {
        // Gradient string pasted into the flat-mode "background" row.
        // Auto-activate gradient mode instead of writing to backgroundColor.
        if (isBgFlatRow && _isCssGradientString(v))
        {
          if (mLiveComp && *rowEnabled)
            _write("background", v);
          mBgFlatMode = false;
          mRebuildPending = true;
          setDirty(false);
          return;
        }
        if (k == "mask" && _isCssGradientString(v))
        {
          if (mLiveComp && *rowEnabled)
            _write(k, v);
          mRebuildPending = true;
          setDirty(false);
          return;
        }
        const bool valid = v.empty() || glint_style_is_valid_by_name(k, v);
        inp->style.color = valid ? (_isColorKey(k) ? glint_color(255, 145, 225, 145)
                                                   : glint_color(255, 255, 255, 255))
                                 : glint_color(255, 220,  72,  72);
        inp->setDirty(false);
        if (!mLiveComp || !valid || v.empty()) return;
        if (!*rowEnabled) return;
        // Strip "!important" before writing: the CSS rule writer (updateCssDeclaration)
        // handles it internally, but the inline writer (glint_style_set_by_name) and
        // the color swatch both need a bare value string.
        std::string writeVal = v;
        {
          while (!writeVal.empty() && std::isspace((unsigned char)writeVal.back())) writeVal.pop_back();
          if (writeVal.size() >= 9) {
            std::string tail = writeVal.substr(writeVal.size() - 9);
            for (char& c : tail) c = (char)std::tolower((unsigned char)c);
            if (tail == "important") {
              writeVal.resize(writeVal.size() - 9);
              while (!writeVal.empty() && std::isspace((unsigned char)writeVal.back())) writeVal.pop_back();
              if (!writeVal.empty() && writeVal.back() == '!') {
                writeVal.pop_back();
                while (!writeVal.empty() && std::isspace((unsigned char)writeVal.back())) writeVal.pop_back();
              }
            }
          }
        }
        _write(k, v);  // pass raw v — updateCssDeclaration strips !important and sets decl.important
        if (swRef) { sk_color c; c = writeVal.c_str(); swRef->style.backgroundColor = c; swRef->setDirty(false); }
      };
      auto focused = std::make_shared<bool>(false);
      inp->onFocus = [inp, focused]() {
        *focused = true;
        inp->style.borderWidth = 1.f;
        inp->style.borderRadius = "4px";
        inp->style.borderColor = glint_color(255, 80, 140, 255);
        inp->setDirty(false);
      };
      inp->onBlur = [this, inp, k, focused]() {
        *focused = false;
        inp->style.borderWidth = 0.f;
        inp->style.borderColor = glint_color(0, 0, 0, 0);
        inp->setDirty(false);
        // After typing a gradient string the panel must rebuild so the visual
        // gradient editor in the backgroundColor row becomes visible.
        if ((mLiveComp && !mLiveComp->style.backgroundGradient.empty() && k == "background") ||
            (k == "mask" && _isCssGradientString(inp->getValue())))
        {
          mRebuildPending = true;
          setDirty(false);
        }
      };
      // Arrow up/down enum cycling for hinted keys (e.g. mix-blend-mode).
      if (const auto* hints = _enumHints(k))
      {
        inp->onKeyDown = [inp, hints, k, rowEnabled, _write](const glint_key_press& key) -> bool {
          if (key.vk != glint_vk::UP && key.vk != glint_vk::DOWN) return false;
          if (rowEnabled && !*rowEnabled) return true;
          const std::string cur = inp->getValue();
          const int n = static_cast<int>(hints->size());
          int idx = 0;
          for (int i = 0; i < n; ++i)
            if ((*hints)[static_cast<size_t>(i)] == cur) { idx = i; break; }
          if (key.vk == glint_vk::UP)   idx = (idx - 1 + n) % n;
          else                          idx = (idx + 1)     % n;
          const std::string next = (*hints)[static_cast<size_t>(idx)];
          inp->setValue(next);
          _write(k, next);
          return true;
        };
      }
      row->addChild(inp);
      if (overridden) inp->style.textDecoration = "line-through";
      inp->setValue(val);
      // Realtime flash: blue border that fades out when the value has changed.
      {
        const auto fit = mFlashProgress.find(key);
        if (fit != mFlashProgress.end() && fit->second > 0.f)
        {
          const int alpha         = static_cast<int>(fit->second * 255.f);
          inp->style.borderWidth  = 1.f;
          inp->style.borderRadius = "4px";
          inp->style.borderColor  = glint_color(alpha, 80, 140, 255);
        }
      }

      if (glint_style_is_scrubable(k))
      {
        row->element.addEventListener("wheel", [this, inp, k, focused, rowEnabled, _write](glint_event& e)
        {
          auto& we = static_cast<glint_wheel_event&>(e);
          we.preventDefault();
          if (!mLiveComp) return;
          if (!*rowEnabled) return;
          if (!*focused) return;
#ifdef __APPLE__
          const float sign = (we.deltaY > 0.f) ? 1.f : -1.f;
#else
          const float sign = (we.deltaY < 0.f) ? 1.f : -1.f;
#endif
          float step = 1.f;
          if      (k == "opacity")  step = 0.05f;
          else if (k == "flex-grow") step = 0.1f;
          if (we.shiftKey) step *= 0.1f;
          std::string newVal;
          if (k == "transform" || k == "filter" || k == "backdrop-filter")
          {
            // Determine which func(�) is under the mouse using its X position
            // over the input rect. filter/backdropFilter share the same func(value) syntax.
            const int mouseIdx = inp->charIndexAtMouseX(we.clientX);
            newVal = glint_transform_adjust(inp->getValue(), sign * step, mouseIdx);
          }
          else
            newVal = glint_length_adjust(inp->getValue(), sign * step);
          if (glint_style_is_non_negative(k))
          {
            // strip suffix, clamp to 0, reattach suffix
            const std::string& nv = newVal;
            if (!nv.empty() && nv[0] == '-')
            {
              std::string sfx;
              if (nv.size() > 3 && nv.substr(nv.size()-2) == "px") sfx = "px";
              else if (nv.back() == '%') sfx = "%";
              newVal = "0" + sfx;
            }
          }
          if (!glint_style_is_valid_by_name(k, newVal)) return;
          inp->setValue(newVal);
          inp->style.color = _isColorKey(k) ? glint_color(255, 145, 225, 145)
                                            : glint_color(255, 255, 255, 255);
          inp->setDirty(false);
          _write(k, newVal);
        });
      }
    }

    // -- Swatch click listener � wired after inp is declared -----------------
    if (isColor && swatchLocal && !isGradRow)
    {
      glint_element* swRef  = swatchLocal;
      glint_input*   inpRef = inp;
      swRef->element.addEventListener("click", [this, k, swRef, inpRef, _write](glint_event& e) {
        e.stopPropagation();
        if (mColorPickerKey == k)
        {
          _dismissPickerWindow();  // toggle off
          return;
        }
        // Toggle on
        mColorPickerKey = k;
        _dismissPickerWindow();   // close any existing picker
        mColorPickerKey = k;      // restore after _dismiss cleared it
        glint_color init;
        if (mLiveComp) {
          // Always read from computedStyle — it is the effective displayed value
          // (merge of cssStyle_ + inline style), so both CSS-rule rows and inline
          // rows open the picker at the correct current color.
          const glint_style_info info = glint_style_serialize(mLiveComp->computedStyle);
          if (info.count(k)) {
            const std::string& cur = info.at(k);
            if (!cur.empty()) { sk_color c; c = cur.c_str(); init = c; }
          }
        }
        swRef->style.borderColor = glint_color(255, 100, 180, 255);
        swRef->setDirty(false);
        mSwatchForPicker = swRef;
        mInputForPicker  = inpRef;
        _openPickerWindowBelow(swRef, k, init, _write);
      });
    }

    // -- ";" --------------------------------------------------------------------
    auto* semi            = new InspStaticText();
    semi->text                 = ";";
    semi->style.width     = 10.f;
    semi->style.height    = "100%";
    semi->style.color     = glint_color(255, 200, 200, 205);
    semi->style.fontSize  = 13.f;
    row->addChild(semi);

    // -- Trash button (×) — far right of the row --------------------------------
    // Wrapper: gives the button a fixed height slot and right-aligns the pill.
    auto* delWrap             = new glint_element();
    delWrap->style.width      = 20.f;
    delWrap->style.height     = "100%";
    delWrap->align = "left middle";

    auto* del                   = new glint_button();
    del->innerText              = "\u00d7";
    del->style.width            = 15.f;
    del->style.height           = 15.f;
    del->style.borderRadius     = "50%";
    del->style.backgroundColor  = glint_color(255, 75, 75, 80);   // grey circle
    del->style.color            = glint_color(255, 170, 170, 175); // grey X
    del->style.fontSize         = 12.f;
    del->style.fontWeight       = 700;
    del->style.textAlign        = EAlign::Center;
    const std::string delKey = key;
    PropDeleter ownedDeleter = deleter;  // capture by value
    del->element.addEventListener("mouseenter", [del](glint_event&) {
      del->style.backgroundColor = glint_color(255, 200, 48, 48);  // red circle
      del->style.color           = glint_color(255, 255, 255, 255); // white X
      del->setDirty(false);
    });
    del->element.addEventListener("mouseleave", [del](glint_event&) {
      del->style.backgroundColor = glint_color(255, 75, 75, 80);
      del->style.color           = glint_color(255, 170, 170, 175);
      del->setDirty(false);
    });
    del->element.addEventListener("click", [this, del, delKey, ownedDeleter](glint_event& e) {
      e.stopPropagation();
      if (ownedDeleter)
      {
        ownedDeleter();
      }
      else if (mLiveComp)
      {
        // Inline element.style row: reset the property to its spec default.
        const auto& def = glint_default_style_info();
        auto it = def.find(delKey);
        if (it != def.end())
        {
          glint_style_set_by_name(mLiveComp->style, delKey, it->second);
          mLiveComp->setDirty(false);
        }
      }
      mRebuildPending = true;
      setDirty(false);
    });
    delWrap->addChild(del);
    row->addChild(delWrap);
  }

  // -- Add-attribute button --------------------------------------------------
  void buildAddAttrButton()
  {
    auto* btn   = new InspAddAttrButton();
    btn->mPanel = this;
    addChild(btn);
  }
};

// =============================================================================
// Out-of-line method bodies (need InspStylePanel to be complete)
// =============================================================================

// InspAddAttrButton: click � open the floating attribute-picker window.
inline void InspAddAttrButton::OnMouseDown(float x, float y, const glint_mouse_mod& mod)
{
  glint_button::OnMouseDown(x, y, mod);   // lets the pressed state render

#if defined(_WIN32)
  if (!mPanel || !mPanel->mOwnerHWND) return;

  // Compute the button's screen-space rect.
  // mRect is in content-space; subtract accumulated parent scroll offsets.
  float scrollX = 0.f, scrollY = 0.f;
  for (glint_element* p = mParent; p; p = p->mParent)
  { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }

  POINT pt = { (LONG)(mRect.L - scrollX), (LONG)(mRect.T - scrollY) };
  ::ClientToScreen(static_cast<HWND>(mPanel->mOwnerHWND), &pt);

  const RECT anchorRect = {
    pt.x,
    pt.y,
    pt.x + (LONG)mRect.W(),
    pt.y + (LONG)mRect.H()
  };

  // Stash the target rule before opening the picker so commitAddProperty
  // can route the write to the stylesheet AST instead of element.style.
  mPanel->setPendingAddRule(mRuleUrl, mRuleLine);
  mPanel->openAttrList(anchorRect, mExistingKeys.empty() ? nullptr : &mExistingKeys);
#elif defined(__APPLE__)
  if (!mPanel || !mPanel->mOwnerHWND) return;

  // Compute the button's screen-space rect via the Mac inspector window.
  float scrollX = 0.f, scrollY = 0.f;
  for (glint_element* p = mParent; p; p = p->mParent)
  { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }

  const float cx = mRect.L - scrollX;
  const float cy = mRect.T  - scrollY;

  auto* macWin = static_cast<glint_window_mac*>(mPanel->mOwnerHWND);
  const RECT anchorRect = macWin->contentRectToScreen(cx, cy, mRect.W(), mRect.H());

  mPanel->setPendingAddRule(mRuleUrl, mRuleLine);
  mPanel->openAttrList(anchorRect, mExistingKeys.empty() ? nullptr : &mExistingKeys);
#else
  (void)x; (void)y; (void)mod;  // attribute list picker not supported on this platform
#endif
}

// InspNameButton::OnMouseDown — defined after InspStylePanel so the body can
// call mPanel->requestRebuild() on the fully-defined InspStylePanel type.
inline void InspNameButton::OnMouseDown(float /*x*/, float /*y*/, const glint_mouse_mod& /*mod*/)
{
  // Don't call base — the popup replaces the normal click/pressed dance.
  const int result = glint_platform::showContextMenu(0, 0, {{1, "Reset to default"}}, {}, {});
  if (result != 1 || !mPanel || !mLiveComp) return;

  InspStylePanel* panel    = mPanel;
  glint_element*  liveComp = mLiveComp;
  const auto& def = glint_default_style_info();
  auto it = def.find(mKey);
  if (it != def.end())
  {
    if (mWriter)
      mWriter(mKey, it->second);
    else {
      glint_style_set_by_name(liveComp->style, mKey, it->second);
      liveComp->setDirty(false);
    }
  }
  panel->requestRebuild();
}

// GradModeButton constructor � defined after InspStylePanel so the body can
// call mPanel->toggleBgGradientMode() on the fully-defined InspStylePanel type.
inline GradModeButton::GradModeButton()
{
  style.width        = 16.f;
  style.height       = 16.f;
  style.borderRadius = 2.f;
  style.borderWidth  = 1.f;
  style.borderColor  = mGradMode ? glint_color(255, 100, 180, 255)
                                 : glint_color(255, 110, 110, 110);
  style.marginRight  = 4.f;
  element.addEventListener("mouseenter", [this](glint_event&) {
    style.borderColor = glint_color(255, 160, 200, 255);
    setDirty(false);
  });
  element.addEventListener("mouseleave", [this](glint_event&) {
    style.borderColor = mGradMode ? glint_color(255, 100, 180, 255)
                                  : glint_color(255, 110, 110, 110);
    setDirty(false);
  });
  element.addEventListener("click", [this](glint_event& e) {
    e.stopPropagation();
    if (mPanel) mPanel->toggleBgGradientMode();
  });
}

// =============================================================================
// InspBoxModelDiagram — Chrome DevTools–style nested box model widget.
//
// Draws 4 concentric layers (margin / border / padding / content).
// Each ring has a tinted background and editable number inputs on each edge.
// Editing an input writes back to mLiveComp->style via glint_style_set_by_name.
// The innermost box shows computed content W × H (read-only).
//
// Layout strategy: every element gets an explicit pixel height derived from the
// constants below.  flexGrow is used only for horizontal width distribution
// within mid-rows.  This prevents the "everything collapses to zero" failure
// that occurs when all ancestors only specify flexGrow with no concrete height.
// =============================================================================
class InspBoxModelDiagram : public glint_element
{
public:
  int mFocusCount = 0;   // incremented by inputs on focus, decremented on blur

  const char* typeName() const override { return "insp_box_model"; }

  // skipIfFocused=true: return without rebuild if any input has focus (timer path).
  void update(glint_element* comp, bool skipIfFocused = false)
  {
    if (skipIfFocused && mFocusCount > 0) return;
    clearChildren();
    mFocusCount = 0;
    if (!comp) return;

    // ── Height constants ──────────────────────────────────────────────────────
    // Every layer height is computed explicitly so no element collapses.
    static constexpr float kBand     = 20.f;   // top/bottom edge-row height per ring
    static constexpr float kSide     = 36.f;   // left/right cell width per ring
    static constexpr float kContentH = 44.f;   // innermost content box height

    const float hPad = 2.f * kBand + kContentH;   // padding ring total height
    const float hBdr = 2.f * kBand + hPad;         // border ring total height
    const float hMar = 2.f * kBand + hBdr;         // margin ring total height = diagram height

    // Set our own size so the parent flex container allocates space for us.
    style.display       = "flex";
    style.flexDirection = "column";
    style.width         = "100%";
    style.height        = hMar;

    // ── Colors ────────────────────────────────────────────────────────────────
    const glint_color marginBg  (200,  73,  45,  12);
    const glint_color borderBg  (200,  95,  60,  12);
    const glint_color paddingBg (200,  28,  58,  20);
    const glint_color contentBg (200,  18,  48,  78);
    const glint_color marginTxt (255, 210, 140,  60);
    const glint_color borderTxt (255, 225, 155,  75);
    const glint_color paddingTxt(255, 110, 195,  80);
    const glint_color contentTxt(255,  90, 170, 230);
    const glint_color labelCol  (180, 200, 200, 200);

    // ── Resolved values ───────────────────────────────────────────────────────
    const glint_style& s = comp->computedStyle;
    const float mT = static_cast<float>(s.marginTop),    mR = static_cast<float>(s.marginRight);
    const float mB = static_cast<float>(s.marginBottom), mL = static_cast<float>(s.marginLeft);
    const float bT = s.resolvedBorderWidth(0), bR = s.resolvedBorderWidth(1);
    const float bB = s.resolvedBorderWidth(2), bL = s.resolvedBorderWidth(3);
    const float pT = static_cast<float>(s.paddingTop),    pR = static_cast<float>(s.paddingRight);
    const float pB = static_cast<float>(s.paddingBottom), pL = static_cast<float>(s.paddingLeft);
    const float cW = std::max(0.f, comp->mRect.W() - bL - bR - pL - pR);
    const float cH = std::max(0.f, comp->mRect.H() - bT - bB - pT - pB);

    // ── Helpers ───────────────────────────────────────────────────────────────
    auto fmt = [](float v) -> std::string {
      char buf[32];
      if (v == std::floor(v)) std::snprintf(buf, sizeof(buf), "%.0f", v);
      else                    std::snprintf(buf, sizeof(buf), "%.1f", v);
      return buf;
    };

    // Editable number input — writes back to comp->style on change.
    auto makeInp = [&](const char* key, float val, glint_color tc) -> glint_input*
    {
      auto* inp = new glint_input();
      inp->setValue(fmt(val));
      inp->style.width           = kSide - 4.f;
      inp->style.height          = kBand;
      inp->style.fontSize        = 11.f;
      inp->style.color           = tc;
      inp->style.backgroundColor = glint_color(0, 0, 0, 0);
      inp->style.borderColor     = glint_color(0, 0, 0, 0);
      inp->style.borderWidth     = 0.f;
      inp->style.textAlign       = EAlign::Center;
      inp->style.padding         = "0 2";
      glint_element* lc = comp;
      const std::string k = key;
      inp->onChange = [lc, k](const std::string& v) {
        if (v.empty()) return;
        glint_style_set_by_name(lc->style, k, v);
        lc->setDirty(false);
      };
      // Track focus and gate wheel scrub on it.
      auto focused = std::make_shared<bool>(false);
      inp->element.addEventListener("focus", [this, focused](glint_event&) {
        *focused = true;
        ++mFocusCount;
      });
      inp->element.addEventListener("blur", [this, focused](glint_event&) {
        *focused = false;
        if (mFocusCount > 0) --mFocusCount;
      });
      // Wheel-to-scrub: nudge by 1 (Shift → 0.1), only when focused.
      inp->element.addEventListener("wheel", [inp, lc, k, focused](glint_event& e) {
        if (!*focused) return;
        auto& we = static_cast<glint_wheel_event&>(e);
        we.preventDefault();
#ifdef __APPLE__
        const float sign = (we.deltaY > 0.f) ? 1.f : -1.f;
#else
        const float sign = (we.deltaY < 0.f) ? 1.f : -1.f;
#endif
        const float step = we.shiftKey ? 0.1f : 1.f;
        std::string newVal = glint_length_adjust(inp->getValue(), sign * step);
        if (glint_style_is_non_negative(k) && !newVal.empty() && newVal[0] == '-')
        {
          std::string sfx;
          if (newVal.size() > 3 && newVal.substr(newVal.size()-2) == "px") sfx = "px";
          else if (newVal.back() == '%') sfx = "%";
          newVal = "0" + sfx;
        }
        inp->setValue(newVal);
        inp->setDirty(false);
        glint_style_set_by_name(lc->style, k, newVal);
        lc->setDirty(false);
      });
      return inp;
    };

    // Top/bottom edge row for a ring.
    //   [ringLabel | kSide] [input | flexGrow:1] [spacer | kSide]
    // The spacer mirrors the label so the input is visually centred.
    auto makeEdgeRow = [&](const char* key, float val, glint_color tc,
                           const char* ringLabel, glint_color lc, float rowH) -> glint_element*
    {
      auto* row = new glint_element();
      row->style.display       = "flex";
      row->style.flexDirection = "row";
      row->style.alignItems    = "center";
      row->style.width         = "100%";
      row->style.height        = rowH;

      // Left: ring label  (italic, very small, colour-matched to the ring text)
      auto* lbl = new InspStaticText();
      lbl->text              = ringLabel;
      lbl->style.width       = kSide;
      lbl->style.height      = "100%";
      lbl->style.fontSize    = 9.f;
      lbl->style.color       = lc;
      lbl->style.paddingLeft = 4.f;
      row->addChild(lbl);

      // Center: fixed-width input centred inside a flex-grow wrapper
      auto* center = new glint_element();
      center->style.display        = "flex";
      center->style.flexGrow       = 1.f;
      center->style.height         = "100%";
      center->style.justifyContent = "center";
      center->style.alignItems     = "center";

      auto* inp = makeInp(key, val, tc);
      inp->style.width  = 48.f;
      inp->style.height = rowH;
      center->addChild(inp);
      row->addChild(center);

      // Right: spacer to balance the label
      auto* sp = new glint_element();
      sp->style.width  = kSide;
      sp->style.height = "100%";
      row->addChild(sp);

      return row;
    };

    // Left/right side cell — fixed width, input vertically centred.
    auto makeSideCell = [&](const char* key, float val, glint_color tc,
                            float cellH) -> glint_element*
    {
      auto* cell = new glint_element();
      cell->style.display        = "flex";
      cell->style.flexDirection  = "column";
      cell->style.alignItems     = "center";
      cell->style.justifyContent = "center";
      cell->style.width          = kSide;
      cell->style.height         = cellH;
      cell->addChild(makeInp(key, val, tc));
      return cell;
    };

    // Flex-column ring layer with a solid background.
    // height and flexGrow are set by the caller depending on context.
    auto makeRingEl = [](glint_color bg) -> glint_element* {
      auto* el = new glint_element();
      el->style.display         = "flex";
      el->style.flexDirection   = "column";
      el->style.backgroundColor = bg;
      return el;
    };

    // Flex-row middle row (left side-cell | inner ring | right side-cell).
    // width is always 100%; height is set by the caller.
    auto makeMidRow = [](float h) -> glint_element* {
      auto* row = new glint_element();
      row->style.display       = "flex";
      row->style.flexDirection = "row";
      row->style.width         = "100%";
      row->style.height        = h;
      return row;
    };

    // ── Margin ring ───────────────────────────────────────────────────────────
    auto* mar    = makeRingEl(marginBg);
    mar->style.width  = "100%";
    mar->style.height = hMar;

    auto* marMid = makeMidRow(hBdr);

    mar->addChild(makeEdgeRow("margin-top",    mT, marginTxt, "margin", labelCol, kBand));
    mar->addChild(marMid);
    mar->addChild(makeEdgeRow("margin-bottom", mB, marginTxt, "",       labelCol, kBand));
    addChild(mar);

    marMid->addChild(makeSideCell("margin-left",  mL, marginTxt, hBdr));

    // ── Border ring ───────────────────────────────────────────────────────────
    auto* bdr    = makeRingEl(borderBg);
    bdr->style.flexGrow = 1.f;   // fill horizontal space in marMid
    bdr->style.height   = "100%";

    auto* bdrMid = makeMidRow(hPad);

    bdr->addChild(makeEdgeRow("border-top-width",    bT, borderTxt, "border", labelCol, kBand));
    bdr->addChild(bdrMid);
    bdr->addChild(makeEdgeRow("border-bottom-width", bB, borderTxt, "",       labelCol, kBand));
    marMid->addChild(bdr);

    bdrMid->addChild(makeSideCell("border-left-width",  bL, borderTxt, hPad));

    // ── Padding ring ──────────────────────────────────────────────────────────
    auto* pad    = makeRingEl(paddingBg);
    pad->style.flexGrow = 1.f;   // fill horizontal space in bdrMid
    pad->style.height   = "100%";

    auto* padMid = makeMidRow(kContentH);

    pad->addChild(makeEdgeRow("padding-top",    pT, paddingTxt, "padding", labelCol, kBand));
    pad->addChild(padMid);
    pad->addChild(makeEdgeRow("padding-bottom", pB, paddingTxt, "",        labelCol, kBand));
    bdrMid->addChild(pad);

    padMid->addChild(makeSideCell("padding-left",  pL, paddingTxt, kContentH));

    // ── Content box — editable W × H on one row ──────────────────────────────
    auto* cnt = makeRingEl(contentBg);
    cnt->style.flexGrow       = 1.f;   // fill horizontal space in padMid
    cnt->style.height         = "100%";
    cnt->style.justifyContent = "center";
    cnt->style.alignItems     = "center";

    // Single flex-row:  W [inp]  ×  H [inp]
    auto* dimRow = new glint_element();
    dimRow->style.display       = "flex";
    dimRow->style.flexDirection = "row";
    dimRow->style.alignItems    = "center";
    dimRow->style.height        = kBand;

    // Helper: build one label+input and append to dimRow.
    // Reuses makeInp so wheel/focus wiring is shared.
    auto makeDimInp = [&](const char* dimLabel, const char* key, float val)
    {
      auto* lbl = new InspStaticText();
      lbl->text              = dimLabel;
      lbl->style.width       = 10.f;
      lbl->style.height      = "100%";
      lbl->style.fontSize    = 9.f;
      lbl->style.color       = glint_color(180, contentTxt.R, contentTxt.G, contentTxt.B);
      lbl->style.textAlign   = EAlign::Center;
      dimRow->addChild(lbl);

      auto* inp = makeInp(key, val, contentTxt);
      inp->style.width  = 38.f;   // override the default kSide-4 width
      inp->style.height = kBand;
      dimRow->addChild(inp);
    };

    makeDimInp("W", "width",  cW);

    // " × " separator
    auto* xLbl = new InspStaticText();
    xLbl->text             = "\xc3\x97";   // UTF-8 ×
    xLbl->style.width      = 14.f;
    xLbl->style.height     = "100%";
    xLbl->style.fontSize   = 10.f;
    xLbl->style.color      = glint_color(120, contentTxt.R, contentTxt.G, contentTxt.B);
    xLbl->style.textAlign  = EAlign::Center;
    dimRow->addChild(xLbl);

    makeDimInp("H", "height", cH);

    cnt->addChild(dimRow);
    padMid->addChild(cnt);

    // ── Close padding ─────────────────────────────────────────────────────────
    padMid->addChild(makeSideCell("padding-right", pR, paddingTxt, kContentH));

    // ── Close border ──────────────────────────────────────────────────────────
    bdrMid->addChild(makeSideCell("border-right-width", bR, borderTxt, hPad));

    // ── Close margin ──────────────────────────────────────────────────────────
    marMid->addChild(makeSideCell("margin-right",  mR, marginTxt, hBdr));

    setDirty(false);
  }
};

// =============================================================================
// InspComputedPanel — read-only view of every resolved (computed) style value.
//
// Shows two sections:
//   Box Model — the pixel position/size taken from mRect after layout.
//   All Properties — alphabetically sorted glint_style_serialize(computedStyle),
//                    non-empty values only.
//
// The panel is a flex-column with overflowY:auto.  Each row is a fixed-height
// flex-row containing a name label on the left and a value label on the right.
// =============================================================================
class InspComputedPanel : public glint_element
{
public:
  InspComputedPanel()
  {
    style.overflowY     = "auto";
    style.flexDirection = "column";
  }

  const char* typeName() const override { return "insp_computed_panel"; }

  // ── Image preview popup callbacks ──────────────────────────────────────────
  // Set by glint_inspector_window::buildUI() after the popup overlay is created.
  // onEnter(key, val, rowLeft, rowTop, rowBottom) — coords in root-canvas pixels.
  //   The receiver is responsible for URL extraction (image_preview_popup.hpp
  //   helpers are not visible here, keeping style_editor.hpp self-contained).
  // onLeave() — mouse left the row.
  using RowEnterCb = std::function<void(const std::string&, const std::string&,
                                        float, float, float)>;
  using RowLeaveCb = std::function<void()>;

  void setPreviewCallbacks(RowEnterCb onEnter, RowLeaveCb onLeave)
  {
    mOnRowEnter = std::move(onEnter);
    mOnRowLeave = std::move(onLeave);
  }

  // Full rebuild — called on selection change and explicit style edits.
  void show(glint_element* comp)
  {
    if (!comp) { clear(); return; }

    // ── Realtime flash: detect value changes since the previous tick ─────────
    const glint_style_info newInfo = glint_style_serialize(comp->computedStyle);
    const bool switchedNode = (comp != mLiveComp);
    if (!switchedNode && !mLastValues.empty())
    {
      for (const auto& kv : newInfo)
      {
        auto it = mLastValues.find(kv.first);
        if (it != mLastValues.end() && it->second != kv.second)
          mFlashProgress[kv.first] = 1.0f;
      }
    }
    // Refresh baseline snapshot.
    mLastValues.clear();
    for (const auto& kv : newInfo) mLastValues[kv.first] = kv.second;
    if (switchedNode) { mFlashProgress.clear(); }

    mLiveComp    = comp;
    mDiagram     = nullptr;
    clearChildren();

    // -- Box Model diagram ---------------------------------------------------

    auto* diag = new InspBoxModelDiagram();
    diag->update(comp, false);
    mDiagram = diag;
    addChild(diag);

    // -- Computed Style -------------------------------------------------------
    buildSectionHeader("All Properties");
    {
      // Sort alphabetically
      std::vector<std::pair<std::string, std::string>> pairs(newInfo.begin(), newInfo.end());
      std::sort(pairs.begin(), pairs.end());
      bool alt = false;
      for (const auto& kv : pairs)
      {
        if (kv.second.empty()) continue;
        buildRow(kv.first, kv.second, alt);
        alt = !alt;
      }
    }

    setDirty(false);
  }

  // Timer-driven live refresh. The default path updates only the box-model
  // diagram; realtime mode can request a full rebuild of the computed property
  // list too, but still skips while a diagram input has focus.
  void liveRefresh(glint_element* comp, bool fullRebuild = false)
  {
    if (!comp) { clear(); return; }
    if (fullRebuild)
    {
      if (mDiagram && mDiagram->mFocusCount > 0) return;
      // Tick down flash animations before rebuilding so the opacity is
      // already decremented when buildRow() reads mFlashProgress.
      constexpr float kFlashDecrement = 0.16f;
      for (auto it = mFlashProgress.begin(); it != mFlashProgress.end(); )
      {
        it->second -= kFlashDecrement;
        if (it->second <= 0.f) it = mFlashProgress.erase(it);
        else                   ++it;
      }
      show(comp);
      return;
    }

    if (!mDiagram) return;
    mDiagram->update(comp, true /*skipIfFocused*/);
    setDirty(false);
  }

  void clear()
  {
    mLiveComp = nullptr;
    mDiagram  = nullptr;
    clearChildren();
    mLastValues.clear();
    mFlashProgress.clear();
    setDirty(false);
  }

  glint_element*        mLiveComp = nullptr;
  InspBoxModelDiagram* mDiagram  = nullptr;

private:
  // ── Section header: uppercase 10 px grey label with hairline below ────────
  void buildSectionHeader(const char* title)
  {
    auto* hdr = new InspStaticText();
    hdr->text                   = title;
    hdr->style.width            = "100%";
    hdr->style.height           = 26.f;
    hdr->style.paddingLeft      = 10.f;
    hdr->style.fontSize         = 10.f;
    hdr->style.color            = glint_color(255, 110, 110, 110);
    hdr->style.backgroundColor  = glint_color(255, 30, 30, 30);
    hdr->style.borderBottomWidth = 1.f;
    hdr->style.borderBottomColor = "#383838";
    addChild(hdr);
  }

  // ── Single key : value row ─────────────────────────────────────────────────
  void buildRow(const std::string& key, const std::string& val, bool alt)
  {
    // Detect img-bearing properties whose url() token can be previewed.
    // URL extraction is intentionally left to the onEnter callback receiver;
    // this file stays free of image_preview_popup.hpp dependencies.
    static const char* const kImgKeys[] = {
      "background-img", "background", "mask", nullptr
    };
    bool isImageBearing = false;
    for (const char* const* k = kImgKeys; *k; ++k)
    {
      if (key == *k) { isImageBearing = true; break; }
    }
    // Only treat as preview-able when the value actually contains url(...).
    const bool hasUrl = isImageBearing && (val.find("url(") != std::string::npos);
    (void)hasUrl; // used below

    const glint_color rowBgNormal  = alt ? glint_color(255, 27, 27, 27) : glint_color(255, 30, 30, 30);
    const glint_color rowBgHovered = glint_color(255, 48, 52, 64);

    auto* row = new glint_element();
    row->style.display         = "flex";
    row->style.flexDirection   = "row";
    row->style.width           = "100%";
    row->style.minHeight       = "20";
    row->style.paddingLeft     = 10.f;
    row->style.paddingRight    = 6.f;
    row->style.paddingTop      = 2.f;
    row->style.paddingBottom   = 2.f;
    row->style.alignItems      = "center";
    row->style.backgroundColor = rowBgNormal;
    addChild(row);

    // Hover highlight + img preview trigger (only for img-bearing keys).
    if (hasUrl && mOnRowEnter && mOnRowLeave)
    {
      const glint_color bgN = rowBgNormal;
      const glint_color bgH = rowBgHovered;
      row->element.addEventListener("mouseenter",
        [row, k = key, v = val, bgH, this](glint_event&)
        {
          row->style.backgroundColor = bgH;
          row->setDirty(false);

          // Convert content-rect → root-canvas space by subtracting scroll offsets.
          float scrollX = 0.f, scrollY = 0.f;
          if (row->mRoot)
          {
            for (auto* p = row->mParent;
                 p && p != &row->mRoot->mCanvas;
                 p = p->mParent)
            { scrollX += p->mScrollLeft; scrollY += p->mScrollTop; }
          }
          const float rowTop  = row->mPaintRECT.T - scrollY;
          const float rowBot  = row->mPaintRECT.B - scrollY;
          const float rowLeft = row->mPaintRECT.L - scrollX;
          mOnRowEnter(k, v, rowLeft, rowTop, rowBot);
        });
      row->element.addEventListener("mouseleave",
        [row, bgN, this](glint_event&)
        {
          row->style.backgroundColor = bgN;
          row->setDirty(false);
          mOnRowLeave();
        });
    }

    // Property name
    auto* nameEl = new InspStaticText();
    nameEl->text           = key;
    nameEl->style.width    = 150.f;
    nameEl->style.height   = "100%";
    nameEl->style.fontSize = 11.f;
    nameEl->style.color    = glint_color(255, 204, 204, 204);
    nameEl->style.overflow = "hidden";
    row->addChild(nameEl);

    // " : " separator
    auto* sep = new InspStaticText();
    sep->text           = ":";
    sep->style.width    = 10.f;
    sep->style.height   = "100%";
    sep->style.fontSize = 11.f;
    sep->style.color    = glint_color(255, 120, 120, 120);
    row->addChild(sep);

    // Value — tinted amber when a preview is available.
    auto* valEl = new InspStaticText();
    valEl->text           = val;
    valEl->style.flexGrow = 1.f;
    valEl->style.height   = "100%";
    valEl->style.fontSize = 11.f;
    valEl->style.color    = hasUrl
                            ? glint_color(255, 200, 160, 80)   // amber tint = has preview
                            : glint_color(255, 255, 255, 255);
    valEl->style.overflow = "hidden";
    // Realtime flash: blue border on the value label when the value changed.
    {
      const auto fit = mFlashProgress.find(key);
      if (fit != mFlashProgress.end() && fit->second > 0.f)
      {
        const int alpha              = static_cast<int>(fit->second * 255.f);
        valEl->style.borderWidth     = 1.f;
        valEl->style.borderRadius    = "3px";
        valEl->style.borderColor     = glint_color(alpha, 80, 140, 255);
        valEl->style.paddingLeft     = 2.f;
        valEl->style.paddingRight    = 2.f;
      }
    }
    row->addChild(valEl);
  }

  // ── Image-preview callback storage ────────────────────────────────────────
  RowEnterCb mOnRowEnter;
  RowLeaveCb mOnRowLeave;

  // ── Realtime flash state ──────────────────────────────────────────────────
  std::unordered_map<std::string, std::string> mLastValues;    // computed value snapshot
  std::unordered_map<std::string, float>       mFlashProgress; // 1.0→0, per changed key
};


