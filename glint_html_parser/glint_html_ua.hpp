#pragma once

// HTML element defaults, appended to a document's UA sheet by glint::html the
// first time it parses markup. Values follow Chrome's html.css (em-based, so
// they scale with the author's font sizes); form controls follow Chrome's
// dark color scheme, which is glint's default look.
//
// Scoping: the document's UA sheet is shared with native glint pages, so every
// selector here is scoped to elements the translator created (it marks them
// [data-glint-html] and appends that to each selector's subject when the
// sheet is installed, see glint_html_scope_selectors). Native pages keep
// glint's own defaults even after an HTML page was opened.

inline constexpr const char* glint_html_ua_css = R"css(
span, a, b, strong, i, em, u, s, small, big, code, kbd, samp, tt, var, mark,
abbr, cite, q, sub, sup, time, label, del, ins, dfn, bdi, bdo, data, output, font {
  display: inline;
}

[hidden] { display: none; }

p, dl { margin: 1em 0; }
blockquote, figure { margin: 1em 40px; }
dd { margin-left: 40px; }

h1 { font-size: 2em;    font-weight: 700; margin: 0.67em 0; }
h2 { font-size: 1.5em;  font-weight: 700; margin: 0.83em 0; }
h3 { font-size: 1.17em; font-weight: 700; margin: 1em 0; }
h4 { font-size: 1em;    font-weight: 700; margin: 1.33em 0; }
h5 { font-size: 0.83em; font-weight: 700; margin: 1.67em 0; }
h6 { font-size: 0.67em; font-weight: 700; margin: 2.33em 0; }

b, strong, th { font-weight: 700; }
i, em, cite, var, dfn, address { font-style: italic; }
u, ins { text-decoration: underline; }
s, del { text-decoration: line-through; }
small, sub, sup { font-size: 83.333%; }
big { font-size: 120%; }
sub { vertical-align: sub; }
sup { vertical-align: super; }
mark { background-color: #ffff00; color: #000000; }
a { color: #4c9aff; text-decoration: underline; cursor: pointer; }

code, kbd, samp, tt, pre { font-family: monospace; }
pre { white-space: pre; margin: 1em 0; }

ul, ol { margin: 1em 0; padding-left: 40px; }
li ul, li ol, ul ul, ul ol, ol ul, ol ol { margin: 0; }
li { position: relative; }

/* list-style-position: outside. The translator gives each <li> one marker
   element and colors it with the item's text color. */
.glint-marker-disc, .glint-marker-circle, .glint-marker-square {
  position: absolute; left: -1.21em; top: 0.5em; width: 0.36em; height: 0.36em;
}
.glint-marker-disc, .glint-marker-circle { border-radius: 50%; }
.glint-marker-circle { border-width: 1px; border-style: solid; }
.glint-marker-num { position: absolute; right: 100%; top: 0; padding-right: 0.3em; white-space: nowrap; }

hr { height: 0; margin: 0.5em 0; border-top: 1px solid #808080; border-bottom: 1px solid #3a3a3a; }

/* Form controls: Chrome's sizes (13.33px Arial, 21px tall fields) and its
   dark-scheme colors. glint sizes are border-box. */
input, select, textarea, button { display: inline-block; font-family: Arial; font-size: 13.333px; color: #ffffff; }
input[type="text"], input[type="email"], input[type="password"], input[type="number"],
input[type="search"], input[type="tel"], input[type="url"] {
  width: 153px; height: 21px; padding: 1px 3px;   /* Chrome: 2px inset border + 2px padding */
  border: 1px solid #858585; border-radius: 2px; background-color: #3b3b3b;
}
input[type="checkbox"] { width: 13px; height: 13px; margin: 3px 3px 3px 4px; }
input[type="radio"]    { width: 13px; height: 13px; margin: 3px 3px 0 5px; }
select {
  width: 160px; height: 19px; padding: 0 4px;
  border: 1px solid #858585; border-radius: 2px; background-color: #3b3b3b;
}
button {
  height: 21px; padding: 1px 6px;
  border: 0; border-radius: 3px; background-color: #6b6b6b;
}
textarea {
  font-family: monospace; width: 182px; padding: 2px;
  border: 1px solid #858585; border-radius: 2px; background-color: #3b3b3b;
}
progress { display: inline-block; width: 10em; height: 1em; }
meter    { display: inline-block; width: 5em;  height: 1em; }

table { display: table; border-spacing: 2px; }
tr { display: table-row; }
td, th { display: table-cell; padding: 1px; }
th { text-align: center; }
)css";

// Initial values HTML content gets from its root(s) and inherits, where
// glint's own initial value differs from the browser's. Not scoped: matched
// on the roots only ([data-glint-html-root]), so author CSS still overrides.
inline constexpr const char* glint_html_root_css = R"css(
[data-glint-html-root] { line-height: normal; }

/* <progress> parts (classes set by the translator): Chrome's native bar,
   half the box height, outlined; the fill leaves the outline open at its end. */
.glint-html-progress-track {
  position: absolute; left: 0; top: 25%; width: 100%; height: 50%;
  background-color: #3b3b3b; border: 1px solid #858585; border-radius: 2px;
}
.glint-html-progress-fill {
  position: absolute; top: 25%; height: 50%;
  background-color: #99c8ff; border: 1px solid #858585; border-right-width: 0; border-radius: 2px;
}
)css";
