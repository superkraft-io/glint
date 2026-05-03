#pragma once

#include "../glint_graphics.hpp"


#include "include/core/SkData.h"
#include "include/core/SkStream.h"
#include "modules/svg/include/SkSVGDOM.h"

#include <mutex>
#include <string>
#include <unordered_map>

inline std::unordered_map<std::string, sk_sp<SkSVGDOM>>& glint_svg_dom_cache()
{
  static std::unordered_map<std::string, sk_sp<SkSVGDOM>> cache;
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
    if (it != cache.end()) return it->second;
  }

  sk_sp<SkSVGDOM> dom = glint_make_svg_dom_from_data(data);

  std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
  glint_svg_dom_cache()[key] = dom;
  return dom;
}

inline sk_sp<SkSVGDOM> glint_load_svg_dom_from_file_cached(const std::string& path)
{
  if (path.empty()) return nullptr;

  {
    std::lock_guard<std::mutex> lock(glint_svg_cache_mutex());
    auto& cache = glint_svg_dom_cache();
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
  }

  return glint_load_svg_dom_cached(path, SkData::MakeFromFileName(path.c_str()));
}

inline glint_graphics::glint_svg glint_wrap_svg_dom(const sk_sp<SkSVGDOM>& dom)
{
  if (!dom) return {};
  glint_graphics::glint_svg svg(dom);
  svg.setSize(dom->containerSize());
  return svg;
}

inline glint_graphics::glint_svg glint_load_svg_cached(const std::string& key, const sk_sp<SkData>& data)
{
  return glint_wrap_svg_dom(glint_load_svg_dom_cached(key, data));
}

inline glint_graphics::glint_svg glint_load_svg_from_file_cached(const std::string& path)
{
  return glint_wrap_svg_dom(glint_load_svg_dom_from_file_cached(path));
}
