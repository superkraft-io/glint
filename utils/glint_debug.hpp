#pragma once

/**
 * glint_debug.hpp
 * Runtime debug utilities for the glint component library.
 *
 * Enable "Colorize Borders" via the plugin's Debug menu (hamburger icon)
 * to draw a unique coloured outline around every glint_element � useful
 * for visualising component bounds, overlap, and layout issues.
 *
 * Flags are plain inline bools so they survive across the whole session
 * without any singleton plumbing. Flip them from anywhere in the UI code:
 *
 *   glint_debug::colorizedBorders = true;
 *   pGraphics->SetAllControlsDirty();
 */

#include "../glint_graphics.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#endif

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"
#ifdef SK_BUILD_FOR_WIN
#include "include/ports/SkTypeface_win.h"
#elif defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)
#include "include/ports/SkFontMgr_mac_ct.h"
#endif

using namespace glint_graphics;

#ifndef DBGMSG
#define DBGMSG(...) do { std::fprintf(stderr, __VA_ARGS__); } while (0)
#endif

#if defined(_WIN32)
inline const void* LoadWinResource(const char* name, const char* type, int& outSize, HMODULE module)
{
  outSize = 0;
  if (!name || !type || !module) return nullptr;

  HRSRC resource = FindResourceA(module, name, type);
  if (!resource) return nullptr;

  const DWORD resourceSize = SizeofResource(module, resource);
  if (resourceSize == 0) return nullptr;

  HGLOBAL loaded = LoadResource(module, resource);
  if (!loaded) return nullptr;

  const void* data = LockResource(loaded);
  if (!data) return nullptr;

  outSize = static_cast<int>(resourceSize);
  return data;
}
#endif

// Forward declaration � glint_element is defined in glint_element.hpp.
// Only the pointer type is needed here (for inspectedNode).
class glint_element;

// --- glint_font_registry ------------------------------------------------------
// Tracks every fontID successfully loaded via glint_load_font().
// glint_element::drawContent() checks this before calling DrawText so an unloaded
// font never reaches the Skia backend in an inconsistent state.
//
// Usage � replace every pGraphics->LoadFont() call with:
//   glint_load_font(pGraphics, "Kanit-Regular", KANIT_REGULAR_FN);
namespace glint_font_registry
{
  inline std::unordered_set<std::string>& loadedFonts()
  {
    // Heap-allocated and intentionally never destroyed: protects against the
    // static-destruction-order fiasco that occurs when the demo/inspector window
    // thread is still alive after WinMain returns and the CRT starts tearing
    // down function-local statics.  The OS reclaims the memory on process exit.
    static auto* s = new std::unordered_set<std::string>();
    return *s;
  }

  inline bool isLoaded(const char* fontID)
  {
    if (!fontID) return false;
    return loadedFonts().count(fontID) > 0;
  }

  inline bool isLoaded(const std::string& fontID)
  {
    return loadedFonts().count(fontID) > 0;
  }

  // Maps fontID → sk_sp<SkTypeface> for raw Skia rendering.
  // Populated by glint_load_font(); queried by glint_element::skFont().
  inline std::unordered_map<std::string, sk_sp<SkTypeface>>& loadedTypefaces()
  {
    // Heap-allocated and intentionally never destroyed: prevents the
    // static-destruction-order fiasco — the demo window thread may still call
    // getTypefaceWeighted() after WinMain returns and the CRT destroys statics.
    static auto* s = new std::unordered_map<std::string, sk_sp<SkTypeface>>();
    return *s;
  }

  inline void registerTypeface(const std::string& fontID, sk_sp<SkTypeface> tf)
  {
    if (tf) loadedTypefaces()[fontID] = std::move(tf);
  }

  inline sk_sp<SkTypeface> getTypeface(const char* fontID)
  {
    if (!fontID) return nullptr;
    auto& m = loadedTypefaces();
    auto  it = m.find(fontID);
    return it != m.end() ? it->second : nullptr;
  }

  /**
   * Returns a typeface for the given CSS font-weight, resolved in this order:
   *  1. Exact "fontID@weight" hit in the registry (populated by @font-face
   *     processing — e.g. "Kanit@100", "Kanit@500").
   *  2. Closest "fontID@*" weight registered in the same family.
   *  3. Variable-font wght axis clone (if the base typeface supports it).
   *  4. Base typeface unchanged (graceful fallback).
   */
  inline sk_sp<SkTypeface> getTypefaceWeighted(const char* fontID, int weight)
  {
    if (!fontID) return nullptr;

    const std::string key = std::string(fontID) + "@" + std::to_string(weight);
    auto& m = loadedTypefaces();

    // 1. Exact cache / registry hit (covers @font-face "Kanit@100" entries).
    {
      auto it = m.find(key);
      if (it != m.end()) return it->second;
    }

    // 2. Find the closest registered weight variant for this family.
    //    All @font-face weight variants are stored as "fontID@NNN" keys.
    {
      const std::string prefix = std::string(fontID) + "@";
      sk_sp<SkTypeface> best;
      int bestDist = INT_MAX;
      for (const auto& [k, tf] : m)
      {
        if (k.size() <= prefix.size()) continue;
        if (k.compare(0, prefix.size(), prefix) != 0) continue;
        const char* wstr = k.c_str() + prefix.size();
        if (!std::isdigit(static_cast<unsigned char>(*wstr))) continue;
        const int w    = std::atoi(wstr);
        const int dist = std::abs(w - weight);
        if (dist < bestDist) { bestDist = dist; best = tf; }
      }
      if (best) { m[key] = best; return best; }
    }

    // 3. Variable font: try the wght axis on the base typeface.
    sk_sp<SkTypeface> base = getTypeface(fontID);
    if (!base) { m[key] = nullptr; return nullptr; }
    {
      const uint32_t kWght = SkSetFourByteTag('w', 'g', 'h', 't');
      int axisCount = base->getVariationDesignParameters(nullptr, 0);
      if (axisCount > 0)
      {
        std::vector<SkFontParameters::Variation::Axis> axes(static_cast<size_t>(axisCount));
        axisCount = base->getVariationDesignParameters(axes.data(), axisCount);
        bool hasWghtAxis = false;
        for (int i = 0; i < axisCount; ++i)
          if (axes[i].tag == kWght) { hasWghtAxis = true; break; }
        if (hasWghtAxis)
        {
          SkFontArguments::VariationPosition::Coordinate coord;
          coord.axis  = kWght;
          coord.value = SkScalar(weight);
          SkFontArguments::VariationPosition pos;
          pos.coordinates     = &coord;
          pos.coordinateCount = 1;
          SkFontArguments args;
          args.setVariationDesignPosition(pos);
          auto wTf = base->makeClone(args);
          if (wTf) { m[key] = wTf; return wTf; }
        }
      }
    }

    // 4. No weight variant available — use base typeface unchanged.
    m[key] = base;
    return base;
  }

  // ── Three-axis registry: (family@weight@style) ─────────────────────────────
  // Key format: "Kanit@100@italic", "Kanit@400@normal", etc.

  /** Maps (family@weight@style) → sk_sp<SkTypeface>.
   *  Populated by registerTypefaceAxes() during @font-face processing. */
  inline std::unordered_map<std::string, sk_sp<SkTypeface>>& axisTypefaces()
  {
    static auto* s = new std::unordered_map<std::string, sk_sp<SkTypeface>>();
    return *s;
  }

  /** Maps (family@weight@style) → font ID string (as passed to pG->LoadFont). */
  inline std::unordered_map<std::string, std::string>& axisFontIds()
  {
    static auto* s = new std::unordered_map<std::string, std::string>();
    return *s;
  }

  inline void registerTypefaceAxes(const std::string& family,
                                    int weight,
                                    const std::string& style,
                                    sk_sp<SkTypeface> tf,
                                    const std::string& fontId)
  {
    if (!tf) return;
    const std::string key = family + "@" + std::to_string(weight) + "@" + style;
    axisTypefaces()[key]  = tf;
    axisFontIds()[key]    = fontId;
    loadedFonts().insert(key);
  }

  // Returns the ordered style fallbacks per the CSS font-matching spec:
  //  italic/oblique requested → italic, oblique, normal
  //  normal (or empty)        → normal, oblique, italic
  inline std::vector<std::string> _styleFallbacks(const std::string& style)
  {
    if (style == "italic" || style == "oblique")
      return {"italic", "oblique", "normal", ""};
    return {"normal", "", "oblique", "italic"};
  }

  /**
   * CSS font-matching (§9) for the three axes family / weight / style.
   * Steps:
   *  1. Filter axisTypefaces() by family name.
   *  2. Apply style matching (italic → oblique → normal fallback).
   *  3. Within the style group, apply CSS weight algorithm:
   *       exact → nearest below (descending) → nearest above (ascending).
   * Returns nullptr when no variant is registered for the family.
   */
  inline sk_sp<SkTypeface> getTypefaceByAxes(const char* family,
                                              int         weight,
                                              const char* style)
  {
    if (!family || !family[0]) return nullptr;

    const std::string fam   = family;
    const std::string sty   = style ? style : "normal";
    const std::string prefix = fam + "@";

    // Build the set of (weight, style) pairs available for this family.
    // Entry format: "fam@weight@style" → key suffix after prefix is "weight@style".
    struct Variant { int w; std::string s; sk_sp<SkTypeface> tf; };
    std::vector<Variant> variants;
    for (const auto& [k, tf] : axisTypefaces())
    {
      if (k.size() <= prefix.size()) continue;
      if (k.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string rest = k.substr(prefix.size()); // "weight@style"
      const size_t at = rest.find('@');
      if (at == std::string::npos) continue;
      int w = 0;
      try { w = std::stoi(rest.substr(0, at)); } catch (...) { continue; }
      const std::string s = rest.substr(at + 1);
      variants.push_back({w, s, tf});
    }
    if (variants.empty()) return nullptr;

    // CSS style order: try each style candidate in fallback order.
    for (const auto& candidateStyle : _styleFallbacks(sty))
    {
      // Collect weights available for this style.
      std::vector<std::pair<int, sk_sp<SkTypeface>>> byWeight;
      for (const auto& v : variants)
        if (v.s == candidateStyle)
          byWeight.emplace_back(v.w, v.tf);
      if (byWeight.empty()) continue;

      // Sort ascending by weight.
      std::sort(byWeight.begin(), byWeight.end(),
                [](const auto& a, const auto& b){ return a.first < b.first; });

      // CSS weight algorithm:
      //   400 → try {400,500}, below descending, above ascending
      //   500 → try {500,400}, below descending, above ascending
      //   <400 → below descending, then above ascending
      //   >500 → above ascending, then below descending
      auto pickNearest = [&]() -> sk_sp<SkTypeface>
      {
        // Exact match first.
        for (const auto& [w, tf] : byWeight)
          if (w == weight) return tf;

        if (weight >= 400 && weight <= 500)
        {
          // Try 400–500 range first (ascending toward 500).
          for (const auto& [w, tf] : byWeight)
            if (w >= weight && w <= 500) return tf;
          // Then below weight descending.
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
          // Then above 500 ascending.
          for (const auto& [w, tf] : byWeight)
            if (w > 500) return tf;
        }
        else if (weight < 400)
        {
          // Below weight descending.
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
          // Then above weight ascending.
          for (const auto& [w, tf] : byWeight)
            if (w > weight) return tf;
        }
        else // weight > 500
        {
          // Above weight ascending.
          for (const auto& [w, tf] : byWeight)
            if (w > weight) return tf;
          // Then below weight descending.
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
        }
        // Last resort: first registered variant.
        return byWeight.front().second;
      };

      auto tf = pickNearest();
      if (tf) return tf;
    }
    return nullptr;
  }

  /** Cached platform font manager for resolving installed system families
   *  such as "Times New Roman" when no @font-face / registry entry exists. */
  inline sk_sp<SkFontMgr> systemFontMgr()
  {
    static const sk_sp<SkFontMgr>* s = []() -> const sk_sp<SkFontMgr>* {
      auto* mgr = new sk_sp<SkFontMgr>();
#ifdef SK_BUILD_FOR_WIN
      *mgr = SkFontMgr_New_DirectWrite();
#elif defined(SK_BUILD_FOR_MAC) || defined(SK_BUILD_FOR_IOS)
      *mgr = SkFontMgr_New_CoreText(nullptr);
#elif defined(__linux__)
      *mgr = SkFontMgr_New_FontConfig(nullptr);
#else
      *mgr = SkFontMgr::RefEmpty();
#endif
      return mgr;
    }();
    return *s;
  }

  inline std::unordered_map<std::string, sk_sp<SkTypeface>>& systemTypefaces()
  {
    static auto* s = new std::unordered_map<std::string, sk_sp<SkTypeface>>();
    return *s;
  }

  inline sk_sp<SkTypeface> getSystemTypefaceByAxes(const char* family,
                                                   int         weight,
                                                   const char* style)
  {
    if (!family || !family[0]) return nullptr;

    const std::string sty = style ? style : "normal";
    const std::string key = std::string(family) + "@" + std::to_string(weight) + "@" + sty;
    auto& cache = systemTypefaces();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    auto mgr = systemFontMgr();
    if (!mgr)
    {
      cache[key] = nullptr;
      return nullptr;
    }

    const auto match = [&](SkFontStyle::Slant slant) -> sk_sp<SkTypeface> {
      return mgr->matchFamilyStyle(family,
                                   SkFontStyle(weight, SkFontStyle::kNormal_Width, slant));
    };

    sk_sp<SkTypeface> tf;
    if (sty == "italic")
    {
      tf = match(SkFontStyle::kItalic_Slant);
      if (!tf) tf = match(SkFontStyle::kUpright_Slant);
    }
    else if (sty == "oblique")
    {
      tf = match(SkFontStyle::kItalic_Slant);
      if (!tf) tf = match(SkFontStyle::kUpright_Slant);
    }
    else
    {
      tf = match(SkFontStyle::kUpright_Slant);
      if (!tf) tf = match(SkFontStyle::kItalic_Slant);
    }

    cache[key] = tf;
    return tf;
  }

  /**
  * Resolves (family, weight, style) → the font ID string to use with glint_text.
   * Returns "" if no match (caller should fall back to the raw family string).
   */
  inline std::string resolveFontFaceId(const char* family,
                           int         weight,
                           const char* style)
  {
    if (!family || !family[0]) return "";
    const std::string fam    = family;
    const std::string sty    = style ? style : "normal";
    const std::string prefix = fam + "@";

    struct Variant { int w; std::string s; std::string id; };
    std::vector<Variant> variants;
    for (const auto& [k, id] : axisFontIds())
    {
      if (k.size() <= prefix.size()) continue;
      if (k.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string rest = k.substr(prefix.size());
      const size_t at = rest.find('@');
      if (at == std::string::npos) continue;
      int w = 0;
      try { w = std::stoi(rest.substr(0, at)); } catch (...) { continue; }
      const std::string s = rest.substr(at + 1);
      variants.push_back({w, s, id});
    }
    if (variants.empty()) return "";

    for (const auto& candidateStyle : _styleFallbacks(sty))
    {
      std::vector<std::pair<int, std::string>> byWeight;
      for (const auto& v : variants)
        if (v.s == candidateStyle)
          byWeight.emplace_back(v.w, v.id);
      if (byWeight.empty()) continue;

      std::sort(byWeight.begin(), byWeight.end(),
                [](const auto& a, const auto& b){ return a.first < b.first; });

      auto pickNearest = [&]() -> std::string
      {
        for (const auto& [w, id] : byWeight)
          if (w == weight) return id;
        if (weight >= 400 && weight <= 500)
        {
          for (const auto& [w, id] : byWeight)
            if (w >= weight && w <= 500) return id;
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
          for (const auto& [w, id] : byWeight)
            if (w > 500) return id;
        }
        else if (weight < 400)
        {
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
          for (const auto& [w, id] : byWeight)
            if (w > weight) return id;
        }
        else
        {
          for (const auto& [w, id] : byWeight)
            if (w > weight) return id;
          for (int i = (int)byWeight.size() - 1; i >= 0; --i)
            if (byWeight[i].first < weight) return byWeight[i].second;
        }
        return byWeight.front().second;
      };

      const std::string id = pickNearest();
      if (!id.empty()) return id;
    }
    return "";
  }

}

/** Drop-in replacement for pGraphics->LoadFont().
 *  On success the fontID is added to glint_font_registry so components can
 *  validate it before drawing. On failure a warning is printed to the console.
 *  Returns true on success (same as glint_canvas::LoadFont). */
inline bool glint_load_font(glint_canvas* pGraphics,
                             const char* fontID,
                             const char* fileNameOrResID)
{
  if (!pGraphics) return false;
  const bool ok = pGraphics->LoadFont(fontID, fileNameOrResID);
  if (ok)
  {
    glint_font_registry::loadedFonts().insert(fontID);

    [&]() {
      sk_sp<SkData> fontData;

#if defined(SK_BUILD_FOR_WIN)
      {
        const std::string quotedResID = std::string("\"") + fileNameOrResID + "\"";
        int resSize = 0;
        const void* pFontMem = LoadWinResource(quotedResID.c_str(), "TTF", resSize,
                                               pGraphics->GetWinModuleHandle());
        if (!pFontMem || resSize <= 0)
          pFontMem = LoadWinResource(quotedResID.c_str(), "ttf", resSize,
                                     pGraphics->GetWinModuleHandle());
        if (pFontMem && resSize > 0)
          fontData = SkData::MakeWithoutCopy(pFontMem, static_cast<size_t>(resSize));
      }
#endif

      if (!fontData && fileNameOrResID)
      {
        if (FILE* f = std::fopen(fileNameOrResID, "rb"))
        {
          std::fseek(f, 0, SEEK_END);
          const long len = std::ftell(f);
          std::fseek(f, 0, SEEK_SET);
          if (len > 0)
          {
            sk_sp<SkData> d = SkData::MakeUninitialized(static_cast<size_t>(len));
            std::fread(const_cast<void*>(d->data()), 1, static_cast<size_t>(len), f);
            fontData = std::move(d);
          }
          std::fclose(f);
        }
      }

      if (fontData)
      {
        auto mgr = glint_font_registry::systemFontMgr();
        if (mgr)
        {
          auto tf = mgr->makeFromData(fontData);
          glint_font_registry::registerTypeface(fontID, std::move(tf));
        }
      }
    }();
  }
  else
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[glint] WARNING: LoadFont(\"%s\", \"%s\") failed -> "
                  "font will fall back to Roboto-Regular\n",
                  fontID ? fontID : "(null)",
                  fileNameOrResID ? fileNameOrResID : "(null)");
    DBGMSG("%s", buf);
  }
  return ok;
}

namespace glint_debug
{
  // When true, every glint_element draws a bright coloured outline
  // on top of its normal rendering so you can see exact component bounds.
  inline bool colorizedBorders = false;

  // Pointer to the inspector's own glint_document (stored as void* to avoid
  // a circular include with glint_document.hpp).  Elements belonging to this
  // document skip colorized-border rendering so the inspector UI is never
  // affected by the debug overlay.
  inline void* inspectorDoc = nullptr;

  // Transient hover highlight (faint).  Set when mouse enters a tree row or
  // when Inspect mode is active and the cursor moves over a main-UI component.
  // Cleared on mouse-out / row leave.  Read each frame by glint_document::Draw().
  inline std::atomic<glint_element*> hoveredNode { nullptr };

  // Persistent selected highlight (bright).  Set when the user clicks a tree
  // row or clicks a component in the main UI while Inspect mode is active.
  // Cleared on inspector close or tree rebuild.  Read by glint_document::Draw().
  inline std::atomic<glint_element*> inspectedNode { nullptr };

  // Eye-pinned highlight.  Set when the user toggles the eye button on a tree
  // row.  Independent of selection — persists until toggled off or inspector
  // closes.  Drawn as a teal stroke + faint fill in glint_document::Draw().
  inline std::atomic<glint_element*> pinnedNode { nullptr };

  // When true, hovering the main plugin UI highlights the component under the
  // cursor in real time (DevTools element-picker mode). Toggled by the inspector's
  // Inspect button; read by glint_document::OnMouseOver via atomic poll.
  inline std::atomic<bool> inspectMode { false };

  // Returns a deterministic, visually distinct colour for a given component
  // instance, derived from its pointer address.
  inline glint_color borderColorFor(const void* ptr)
  {
    static const glint_color palette[] = {
      glint_color(255, 255,  70,  70),   // red
      glint_color(255,  70, 210,  70),   // green
      glint_color(255,  70, 140, 255),   // blue
      glint_color(255, 255, 210,  40),   // yellow
      glint_color(255, 255,  80, 255),   // magenta
      glint_color(255,  40, 220, 220),   // cyan
      glint_color(255, 255, 150,  40),   // orange
      glint_color(255, 170,  90, 255),   // purple
    };
    constexpr int N = static_cast<int>(sizeof(palette) / sizeof(palette[0]));
    // Mix a few bits of the address so adjacent allocations get different colours.
    const uintptr_t addr  = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t mixed = (addr ^ (addr >> 4) ^ (addr >> 8)) & 0xFF;
    return palette[mixed % N];
  }
}
