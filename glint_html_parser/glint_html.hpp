#pragma once

/**
 * glint_html.hpp — HTML → glint_element trees (link glint::html).
 *
 * lexbor (third_party/lexbor) parses the markup exactly as a browser would
 * (WHATWG tree construction: implied tags, misnesting repair, entities, raw
 * text); glint_html.cpp then translates that DOM into glint_elements:
 *
 *   - Tags with a registered component become it (img, button, input, select,
 *     form, textarea, progress, meter, any registerElement() custom tag); every
 *     other tag is a plain glint_element named after the tag.
 *   - id / class / style go to their glint members; all other attributes are
 *     kept (getAttribute / [attr] selectors). Component attributes (src, type,
 *     value, placeholder, checked, ...) also set the typed fields.
 *   - Text that is an element's only content becomes its innerText. Mixed
 *     content becomes anonymous inline "#text" children; <br> is a forced break.
 *   - <style>, <link rel=stylesheet>, <title> and <base> are applied to the
 *     document; <script>, <template> and comments are dropped.
 *   - On first use, an HTML user-agent sheet (headings, paragraphs, lists,
 *     inline formatting, tables...) is appended to the document's UA sheet.
 *
 * Known gaps: inline elements that contain elements (e.g. <a>x <b>y</b></a>)
 * lay out as one unbreakable box; inline <svg>/<math> is skipped; table row
 * groups (thead/tbody/tfoot) are flattened into the table; <img> has no
 * intrinsic size yet, so give it width and height (attributes or CSS);
 * vertical margins of adjacent blocks add up (glint has no margin collapsing).
 *
 * Usage:
 *   glint_html_set_inner_html(card, "<h2>Title</h2><p>Hello <b>world</b></p>");
 *   glint_html_load(*doc, "html/page.html", contentRoot);   // via doc.onRequest
 */

#include <string>
#include <string_view>

class glint_element;
class glint_document;

struct glint_html_options
{
  /** Append the HTML element defaults to the document's UA sheet (once). */
  bool applyHtmlUaSheet = true;
  /** Prefix <li> content with a "•" / "1." marker run (no CSS list-style yet). */
  bool listMarkers = true;
};

struct glint_html_result
{
  bool        ok = false;
  std::string error;   // set when !ok
  std::string title;   // <title> text (documents)
};

/** Replace parent's children with the parsed fragment (innerHTML setter).
 *  Parsed in the context of parent's tag, like the DOM does. */
glint_html_result glint_html_set_inner_html(glint_element* parent, std::string_view html,
                                            const glint_html_options& opts = {});

/** Append the parsed fragment after parent's existing children. */
glint_html_result glint_html_append(glint_element* parent, std::string_view html,
                                    const glint_html_options& opts = {});

/** Parse a whole document. Head stylesheets go to `doc`; <body> attributes and
 *  children go to `target` (doc.mCanvas when null), appended after its
 *  existing children. Relative URLs resolve against `baseUrl`. */
glint_html_result glint_html_load_string(glint_document& doc, std::string_view html,
                                         glint_element* target = nullptr,
                                         const std::string& baseUrl = {},
                                         const glint_html_options& opts = {});

/** Fetch `url` through doc.onRequest (disk fallback, like loadStylesheet)
 *  and load it with glint_html_load_string, using `url` as the base URL. */
glint_html_result glint_html_load(glint_document& doc, const std::string& url,
                                  glint_element* target = nullptr,
                                  const glint_html_options& opts = {});
