#pragma once

#include "glint_css_parser/glint_css_dom_adapter.hpp"
#include "glint_css_parser/glint_css.hpp"

#include <vector>

inline constexpr const char* glint_default_style_css = R"css(
button {
  text-align: center;
  display: inline-block;
}

input {
  height: 36px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 8px;
  padding: 0 10px;
  color: #dcdcdc;
  font-size: 13px;
}

input[type="range"] {
  border: none;
  background-color: transparent;
  border-radius: 0;
  padding: 0;
}

dial {
  border-radius: 9999px;
  background-color: #1a1a1a;
  border: 1px solid #505050;
}

tree {
  overflow-y: auto;
}

colorpicker {
  width: 220px;
  display: flex;
  flex-direction: column;
  background-color: #1c1c1c;
  border: 1px solid #404040;
  border-radius: 6px;
}

gradient-editor {
  width: 300px;
  height: 48px;
  background-color: #1c1c1c;
  position: relative;
}

)css";

inline const GlintCssStylesheet& glint_default_user_agent_stylesheet()
{
  static const GlintCssStylesheet sheet = [] {
    auto parsed = GlintCssParser::parseStylesheet(glint_default_style_css);
    parsed.sourceUrl = "source/glint/default_style.hpp";
    return parsed;
  }();
  return sheet;
}

inline std::vector<GlintCssDeclaration> glint_default_user_agent_declarations_for(glint_element& el)
{
  GlintCssDomAdapter adapter(&el);
  const auto* uaSheet = &glint_default_user_agent_stylesheet();
  const auto winning = GlintCssCascade::computeDeclarations(adapter, {}, {}, { uaSheet });
  std::vector<GlintCssDeclaration> decls;
  decls.reserve(winning.size());
  for (const auto& kv : winning)
    decls.push_back(kv.second.decl);
  return decls;
}

inline glint_style glint_default_user_agent_style_for(glint_element& el)
{
  glint_style style;
  GlintCssApply::apply(glint_default_user_agent_declarations_for(el), style);
  return style;
}