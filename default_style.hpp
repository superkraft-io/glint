#pragma once

#include "glint_css_parser/glint_css_dom_adapter.hpp"
#include "glint_css_parser/glint_css.hpp"

#include <vector>

inline constexpr const char* glint_default_style_css = R"css(
button {
  text-align: center;
  display: inline-block;
  user-select: none;
  cursor: default;
  height: 36px;
  padding: 0 12px;
  background-color: #2d2d2d;
  border: 1px solid #555555;
  border-radius: 8px;
  color: #e6e6e6;
  font-size: 13px;
}

button:hover {
  background-color: #353535;
}

button:active {
  background-color: #222222;
}

select {
  user-select: none;
  cursor: default;
}

input {
  position: relative;
}

input[type="text"],
input[type="email"],
input[type="password"],
input[type="number"],
input[type="search"],
input[type="tel"],
input[type="url"] {
  height: 36px;
  border: 1px solid #414141;
  border-radius: 8px;
  padding: 0 10px;
  color: #dcdcdc;
  font-size: 13px;
}

input[type="button"],
input[type="submit"],
input[type="reset"],
input[type="file"],
input[type="image"],
input[type="checkbox"],
input[type="radio"],
input[type="color"],
input[type="range"] {
  cursor: default;
}

input[type="file"] {
  height: 36px;
}

input[type="color"] {
  height: 36px;
  width: 44px;
  border: 0;
  border-radius: 0;
}

input[type="button"],
input[type="submit"],
input[type="reset"] {
  height: 36px;
}

input > button,
input > checkbox,
input > radio,
input > slider,
input > image-input,
input > text-input {
  position: absolute;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
}

input > text-input {
  cursor: text;
}

input > button {
  user-select: none;
  text-align: center;
}

input[type="color"] > button {
  border: 1px solid #414141;
  border-radius: 8px;
}

text-input {
  cursor: text;
}

dial {
  border-radius: 9999px;
  background-color: #1a1a1a;
  border: 1px solid #505050;
  user-select: none;
  cursor: default;
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
  user-select: none;
}

gradient-editor {
  width: 300px;
  height: 48px;
  background-color: #1c1c1c;
  position: relative;
  user-select: none;
}

textarea-resize-handle {
  position: absolute;
  right: 0;
  bottom: 0;
  cursor: se-resize;
}

.glint_radio_dot {
  border-radius: 9999px;
  background-color: #ffffff;
}

progress {
  position: relative;
  overflow: hidden;
  display: block;
  width: 160px;
  height: 16px;
  user-select: none;
}

.glint_progress_track {
  position: absolute;
  left: 0;
  top: 0;
  width: 100%;
  height: 100%;
  border-radius: 9999px;
  background-color: #2d2d2d;
}

.glint_progress_fill {
  position: absolute;
  top: 0;
  height: 100%;
  border-radius: 9999px;
  background-color: #4c9eff;
}

.glint_progress_fill--indeterminate {
  position: absolute;
  top: 0;
  height: 100%;
  border-radius: 9999px;
  background-color: #888888;
}

.glint_tooltip {
  position: relative;
}

.glint_tooltip_popup {
  position: absolute;
  display: none;
  background-color: #1a1a1a;
  border-color: #3c3c3c;
  border-width: 1px;
  border-radius: 4px;
  padding: 6px;
  font-size: 12px;
  color: #e6e6e6;
  white-space: nowrap;
  max-width: 200px;
  z-index: 1000;
}

.glint_tooltip_popup--visible {
  display: block;
}

datepicker {
  display: flex;
  flex-direction: column;
  width: 224px;
  height: 276px;
  background-color: #1e1e1e;
  border: 1px solid #3c3c3c;
  border-radius: 8px;
  padding: 8px;
  overflow: hidden;
  user-select: none;
}

.dp-header {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 100%;
  height: 32px;
}

.dp-nav-btn {
  display: flex;
  flex-direction: column;
  width: 32px;
  height: 32px;
  align-items: center;
  justify-content: center;
  border-radius: 4px;
  cursor: pointer;
}

.dp-nav-btn-label {
  font-size: 13px;
  color: #b4b4b4;
}

.dp-header-label {
  display: flex;
  flex-direction: column;
  height: 32px;
  flex-grow: 1;
  align-items: center;
  justify-content: center;
}

.dp-header-text {
  font-size: 13px;
  color: #e6e6e6;
}

.dp-dow-row {
  display: flex;
  flex-direction: row;
  width: 100%;
  height: 20px;
}

.dp-dow-cell {
  display: flex;
  flex-direction: column;
  width: 30px;
  height: 20px;
  align-items: center;
  justify-content: center;
}

.dp-dow-label {
  font-size: 10px;
  color: #787878;
}

.dp-grid {
  display: flex;
  flex-direction: column;
  width: 100%;
}

.dp-row {
  display: flex;
  flex-direction: row;
  width: 100%;
  height: 28px;
}

.dp-cell {
  display: flex;
  flex-direction: column;
  width: 30px;
  height: 28px;
  align-items: center;
  justify-content: center;
  border-radius: 13px;
  cursor: pointer;
}

.dp-cell-label <{
  font-size: 12px;
}

.dp-spacer {
  width: 100%;
  height: 8px;
}

.dp-today-btn {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 24px;
  align-items: center;
  justify-content: center;
  border: 1px solid #d9d9d9;
  border-radius: 4px;
  cursor: pointer;
}

.dp-today-btn:hover {
  background-color: #505050;
}

.dp-today-label {
  font-size: 12px;
  color: #d9d9d9;
}

monthpicker {
  display: flex;
  flex-direction: column;
  width: 224px;
  height: 208px;
  background-color: #1e1e1e;
  border: 1px solid #3c3c3c;
  border-radius: 8px;
  padding: 8px;
  overflow: hidden;
  user-select: none;
}

.mp-header {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 100%;
  height: 32px;
}

.mp-nav-btn {
  display: flex;
  flex-direction: column;
  width: 32px;
  height: 32px;
  align-items: center;
  justify-content: center;
  border-radius: 4px;
  cursor: pointer;
}

.mp-nav-btn-label {
  font-size: 13px;
  color: #b4b4b4;
}

.mp-header-label {
  display: flex;
  flex-direction: column;
  height: 32px;
  flex-grow: 1;
  align-items: center;
  justify-content: center;
}

.mp-header-text {
  font-size: 13px;
  color: #e6e6e6;
}

.mp-grid {
  display: flex;
  flex-direction: column;
  width: 100%;
}

.mp-row {
  display: flex;
  flex-direction: row;
  width: 100%;
  height: 34px;
  gap: 4px;
}

.mp-cell {
  display: flex;
  flex-direction: column;
  flex-grow: 1;
  height: 30px;
  align-items: center;
  justify-content: center;
  border-radius: 8px;
  cursor: pointer;
}

.mp-cell-label {
  font-size: 12px;
}

.mp-spacer {
  width: 100%;
  height: 8px;
}

.mp-today-btn {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 24px;
  align-items: center;
  justify-content: center;
  border: 1px solid #d9d9d9;
  border-radius: 4px;
  cursor: pointer;
}

.mp-today-btn:hover {
  background-color: #505050;
}

.mp-today-label {
  font-size: 12px;
  color: #d9d9d9;
}

weekpicker {
  display: flex;
  flex-direction: column;
  width: 272px;
  height: 278px;
  background-color: #1e1e1e;
  border: 1px solid #3c3c3c;
  border-radius: 8px;
  padding: 8px;
  overflow: hidden;
  user-select: none;
}

.wp-header {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 100%;
  height: 32px;
}

.wp-nav-btn {
  display: flex;
  flex-direction: column;
  width: 32px;
  height: 32px;
  align-items: center;
  justify-content: center;
  border-radius: 4px;
  cursor: pointer;
}

.wp-nav-btn-label {
  font-size: 13px;
  color: #b4b4b4;
}

.wp-header-label {
  display: flex;
  flex-direction: column;
  height: 32px;
  flex-grow: 1;
  align-items: center;
  justify-content: center;
}

.wp-header-text {
  font-size: 13px;
  color: #e6e6e6;
}

.wp-dow-row {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 100%;
  height: 20px;
  gap: 2px;
}

.wp-weeknum-cell {
  display: flex;
  flex-direction: column;
  width: 36px;
  height: 20px;
  align-items: center;
  justify-content: center;
  border-right: 1px solid #3a3a3a;
  margin-right: 4px;
}

.wp-weeknum-label {
  font-size: 10px;
  color: #787878;
}

.wp-dow-cell {
  display: flex;
  flex-direction: column;
  width: 30px;
  height: 20px;
  align-items: center;
  justify-content: center;
}

.wp-dow-label {
  font-size: 10px;
  color: #787878;
}

.wp-grid {
  display: flex;
  flex-direction: column;
  width: 100%;
  gap: 2px;
}

.wp-row {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 100%;
  height: 28px;
  border-radius: 8px;
  cursor: pointer;
}

.wp-day-cell {
  display: flex;
  flex-direction: column;
  width: 30px;
  height: 28px;
  align-items: center;
  justify-content: center;
}

.wp-day-label {
  font-size: 12px;
}

.wp-spacer {
  width: 100%;
  height: 8px;
}

.wp-today-btn {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 24px;
  align-items: center;
  justify-content: center;
  border: 1px solid #d9d9d9;
  border-radius: 4px;
  cursor: pointer;
}

.wp-today-btn:hover {
  background-color: #505050;
}

.wp-today-label {
  font-size: 12px;
  color: #d9d9d9;
}

timepicker {
  display: flex;
  flex-direction: column;
  width: 168px;
  height: 252px;
  background-color: #1e1e1e;
  border: 1px solid #3c3c3c;
  border-radius: 16px;
  padding: 8px;
  overflow: hidden;
  user-select: none;
}

.glint-tp-columns {
  display: flex;
  flex-direction: row;
  width: 100%;
  height: 100%;
  flex-grow: 1;
  gap: 8px;
}

.glint-tp-col {
  display: flex;
  flex-direction: column;
  flex-grow: 1;
}

.glint-tp-col-header {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 18px;
  align-items: center;
  justify-content: center;
}

.glint-tp-col-header-text {
  font-size: 11px;
  color: #8c8c8c;
}

.glint-tp-list {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 100%;
  flex-grow: 1;
  border: 1px solid #414141;
  border-radius: 8px;
  overflow-x: hidden;
  overflow-y: scroll;
  scrollbar-width: none;
  background-color: #222222;
}

.glint-tp-row {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 26px;
  align-items: center;
  justify-content: center;
  cursor: pointer;
}

.glint-tp-row-selected {
  background-color: #1a73e8;
}

.glint-tp-row-label {
  font-size: 13px;
  color: #dcdcdc;
}

.glint-tp-row-label-selected {
  color: #ffffff;
}

.glint-tp-now-btn {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 28px;
  margin-top: 8px;
  align-items: center;
  justify-content: center;
  background-color: #2a2a2a;
  border: 1px solid #414141;
  border-radius: 6px;
  color: #dcdcdc;
  font-size: 12px;
  cursor: pointer;
}

.glint-tp-now-btn:hover {
  background-color: #353535;
}

.glint-tp-now-btn:active {
  background-color: #3f3f3f;
}

date-input {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 180px;
  height: 28px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 6px;
  padding-left: 8px;
  padding-right: 4px;
  user-select: none;
}

.di-field {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

.di-sep {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.di-sep-label {
  font-size: 13px;
  color: #646464;
}

.di-field-text {
  font-size: 13px;
  color: #dcdcdc;
}

.di-spacer {
  flex-grow: 1;
}

.di-icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

month-input {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 140px;
  height: 28px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 6px;
  padding-left: 8px;
  padding-right: 4px;
  user-select: none;
}

.mi-field {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

.mi-sep {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.mi-sep-label {
  font-size: 13px;
  color: #646464;
}

.mi-field-text {
  font-size: 13px;
  color: #dcdcdc;
}

.mi-spacer {
  flex-grow: 1;
  align-self: stretch;
  cursor: pointer;
}

.mi-icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

week-input {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 156px;
  height: 28px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 6px;
  padding-left: 6px;
  padding-right: 4px;
  user-select: none;
}

.wi-prefix {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.wi-prefix-label {
  font-size: 13px;
  color: #646464;
}

.wi-field {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

.wi-sep {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.wi-sep-label {
  font-size: 13px;
  color: #646464;
}

.wi-field-text {
  font-size: 13px;
  color: #dcdcdc;
}

.wi-spacer {
  flex-grow: 1;
  align-self: stretch;
  cursor: pointer;
}

.wi-icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

time-input {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 118px;
  height: 28px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 6px;
  padding-left: 8px;
  padding-right: 4px;
  user-select: none;
}

.ti-field {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

.ti-sep {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.ti-sep-label {
  font-size: 13px;
  color: #646464;
}

.ti-field-text {
  font-size: 13px;
  color: #dcdcdc;
}

.ti-spacer {
  flex-grow: 1;
  align-self: stretch;
  cursor: pointer;
}

.ti-icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

datetime-local-input {
  display: flex;
  flex-direction: row;
  align-items: center;
  width: 244px;
  height: 28px;
  background-color: #202020;
  border: 1px solid #414141;
  border-radius: 6px;
  padding-left: 8px;
  padding-right: 4px;
  user-select: none;
}

.dli-field {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
}

.dli-sep {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 10px;
}

.dli-sep-label {
  font-size: 13px;
  color: #646464;
}

.dli-field-text {
  font-size: 13px;
  color: #dcdcdc;
}

.dli-gap {
  width: 8px;
}

.dli-spacer {
  flex-grow: 1;
}

.dli-icon {
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  width: 26px;
  height: 22px;
  border-radius: 3px;
  cursor: pointer;
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