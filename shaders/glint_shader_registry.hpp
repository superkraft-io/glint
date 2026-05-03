#pragma once

/**
 * glint_shader_registry.hpp
 *
 * Factory registry for glint_shader_base subclasses.
 *
 * Shaders self-register by declaring a file-scope static:
 *
 *   static bool _reg = glint_shader_registry::add("my_shader",
 *       []{ return std::make_unique<MyShader>(); });
 *
 * glint_element calls glint_shader_registry::create(name) when it
 * encounters a "shader(id, name)" token in style.filter / style.backdropFilter
 * and no shader with that id exists yet in the component's shaders map.
 *
 * Registration only happens when the shader's header has been included in
 * the translation unit — include sk_ui_shaders.hpp (which includes all
 * built-in shader headers) to make the full set available.
 */


#include "glint_shader_base.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>

namespace glint_shader_registry
{
  inline std::map<std::string, std::function<std::unique_ptr<glint_shader_base>()>>&
  _table()
  {
    static std::map<std::string, std::function<std::unique_ptr<glint_shader_base>()>> t;
    return t;
  }

  /** Register a shader factory under a CSS name.
   *  Returns true so it can be used in a static initialiser. */
  inline bool add(const char* name,
                  std::function<std::unique_ptr<glint_shader_base>()> factory)
  {
    _table()[name] = std::move(factory);
    return true;
  }

  /** Create a new shader instance by registered name.
   *  Returns nullptr if the name is not registered. */
  inline std::unique_ptr<glint_shader_base> create(const std::string& name)
  {
    auto& t = _table();
    auto  it = t.find(name);
    if (it == t.end()) return nullptr;
    return it->second();
  }
}  // namespace glint_shader_registry

