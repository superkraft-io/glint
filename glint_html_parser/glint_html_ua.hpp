#pragma once

// HTML element defaults, appended to a document's UA sheet by glint::html the
// first time it parses markup (so pure C++ pages keep today's defaults).
// Values follow Chrome's html.css for a 16px base font, in px.

inline constexpr const char* glint_html_ua_css = R"css(
span, a, b, strong, i, em, u, s, small, big, code, kbd, samp, tt, var, mark,
abbr, cite, q, sub, sup, time, label, del, ins, dfn, bdi, bdo, data, output, font {
  display: inline;
}

[hidden] { display: none; }

p, dl, figure { margin-top: 16px; margin-bottom: 16px; }
blockquote { margin: 16px 40px; }
dd { margin-left: 40px; }

h1 { font-size: 32px;    font-weight: 700; margin-top: 21.44px; margin-bottom: 21.44px; }
h2 { font-size: 24px;    font-weight: 700; margin-top: 19.92px; margin-bottom: 19.92px; }
h3 { font-size: 18.72px; font-weight: 700; margin-top: 18.72px; margin-bottom: 18.72px; }
h4 { font-size: 16px;    font-weight: 700; margin-top: 21.28px; margin-bottom: 21.28px; }
h5 { font-size: 13.28px; font-weight: 700; margin-top: 22.18px; margin-bottom: 22.18px; }
h6 { font-size: 10.72px; font-weight: 700; margin-top: 24.97px; margin-bottom: 24.97px; }

b, strong, th { font-weight: 700; }
i, em, cite, var, dfn, address { font-style: italic; }
u, ins { text-decoration: underline; }
s, del { text-decoration: line-through; }
small { font-size: 13.33px; }
big { font-size: 19.2px; }
sub { vertical-align: sub; font-size: 13.33px; }
sup { vertical-align: super; font-size: 13.33px; }
mark { background-color: #ffff00; color: #000000; }
a { color: #4c9aff; text-decoration: underline; cursor: pointer; }

code, kbd, samp, tt, pre { font-family: monospace; }
pre { white-space: pre; margin-top: 16px; margin-bottom: 16px; }

ul:not([role="listbox"]), ol { margin-top: 16px; margin-bottom: 16px; padding-left: 40px; }
li ul:not([role="listbox"]), li ol { margin-top: 0; margin-bottom: 0; }

hr { height: 0; margin: 8px 0; border-top: 1px solid #808080; }

/* Controls inside text: Chrome's inputs are inline-block; glint's are block,
   so only lift the ones that sit in a <label> next to their text. */
label input[type="checkbox"], label input[type="radio"] { display: inline-block; width: 16px; height: 16px; }
meter { display: inline-block; width: 80px; height: 16px; }
/* Same look as glint's text inputs; height comes from rows (default 2). */
textarea {
  display: inline-block; width: 180px; height: 57px; padding: 8px;
  border: 1px solid #414141; border-radius: 8px; color: #dcdcdc; font-size: 13px;
}

table { display: table; }
tr { display: table-row; }
td, th { display: table-cell; padding: 1px; }
th { text-align: center; }
)css";
