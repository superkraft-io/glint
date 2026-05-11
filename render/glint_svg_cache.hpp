#pragma once

#include "../glint_graphics.hpp"


#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <mutex>
#include <string>
#include <unordered_map>

/**
 * Cache entry pairing the shared SkSVGDOM with the SVG's true intrinsic size.
 *
 * The intrinsic size is captured once, immediately after the DOM is parsed —
 * BEFORE any call to DrawSVG which mutates dom->containerSize() every frame
 * (to match the destination rect).  Storing it here lets glint_load_svg_cached
 * always return a glint_svg whose W()/H() reflect the SVG's authored dimensions
 * regardless of how many draw calls have mutated the shared DOM since loading.
 */
struct GlintSvgDomCacheEntry
{
  sk_sp<SkSVGDOM> dom;
  SkSize          originalSize = SkSize::Make(0.f, 0.f);
};

inline std::unordered_map<std::string, GlintSvgDomCacheEntry>& glint_svg_dom_cache()
{
  static std::unordered_map<std::string, GlintSvgDomCacheEntry> cache;
  return cache;
}

inline std::mutex& glint_svg_cache_mutex()
{
  static std::mutex cacheMutex;
  return cacheMutex;
}

inline sk_sp<SkSVGDOM> glint_make_svg_dom_from_data(const sk_sp<SkData>& data)
{
  if (!data) return nullptr;

  SkMemoryStream stream(data);
  auto dom = SkSVGDOM::Builder().make(stream);
  if (!dom) return nullptr;

  if (dom->containerSize().width() == 0.f)
  {
    float svgW = 0.f;
    float svgH = 0.f;
    if (glint_graphics::ParseSVGIntrinsicSize(data->data(), data->size(), svgW, svgH))
      dom->setContainerSize(SkSize::Make(svgW, svgH));
  }

  return dom;
}

inline sk_sp<SkSVGDOM> glint_load_svg_dom_cached(const std::string& key, const sk_sp<SkData>& data)
{
  if (key.empty())
    return glint_make_svg_dom_from_data(data);

  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.dom;
  }

  sk_sp<SkSVGDOM> dom = glint_make_svg_dom_from_data(data);

  std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
  // Record the true intrinsic size NOW, before any DrawSVG call mutates
  // dom->containerSize() (DrawSVG sets it to the destination rect each frame).
  GlintSvgDomCacheEntry entry;
  entry.dom = dom;
  entry.originalSize = dom ? dom->containerSize() : SkSize::Make(0.f, 0.f);
  glint_svg_dom_cache()[key] = std::move(entry);
  return dom;
}

inline sk_sp<SkSVGDOM> glint_load_svg_dom_from_file_cached(const std::string& path)
{
  if (path.empty()) return nullptr;

  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second.dom;
  }

  return glint_load_svg_dom_cached(path, SkData::MakeFromFileName(path.c_str()));
}

inline glint_graphics::glint_svg glint_wrap_svg_dom(const sk_sp<SkSVGDOM>& dom)
{
  // For a freshly-created (uncached) DOM the containerSize has not been
  // mutated yet, so reading it here is safe.
  if (!dom) return {};
  glint_graphics::glint_svg svg(dom);
  svg.setSize(dom->containerSize());
  return svg;
}

inline glint_graphics::glint_svg glint_load_svg_cached(const std::string& key, const sk_sp<SkData>& data)
{
  sk_sp<SkSVGDOM> dom = glint_load_svg_dom_cached(key, data);
  if (!dom) return {};
  glint_graphics::glint_svg svg(dom);

  // Use the original intrinsic size stored at load time, not dom->containerSize()
  // which DrawSVG mutates on every frame (it calls setContainerSize(destW, destH)
  // so the SVG renders into the destination rect).  Without this, a second
  // glint_image that loads the same cached SVG URL would see the draw-destination
  // size (e.g. 14 px) as the SVG's "natural" width, causing DrawSVG to compute
  // srcW=14 and apply no scale — rendering the 800×800 SVG at full size.
  if (!key.empty())
  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(key);
    if (it != cache.end() && it->second.originalSize.width() > 0.f)
    {
      svg.setSize(it->second.originalSize);
      return svg;
    }
  }

  // Fallback: key was empty or not yet cached — DOM is fresh, containerSize is valid.
  svg.setSize(dom->containerSize());
  return svg;
}

inline glint_graphics::glint_svg glint_load_svg_from_file_cached(const std::string& path)
{
  if (path.empty()) return {};

  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(path);
    if (it != cache.end())
    {
      glint_graphics::glint_svg svg(it->second.dom);
      svg.setSize(it->second.originalSize.width() > 0.f
                  ? it->second.originalSize
                  : it->second.dom->containerSize());
      return svg;
    }
  }

  return glint_load_svg_cached(path, SkData::MakeFromFileName(path.c_str()));
}
