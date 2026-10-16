# glint::html — HTML pages in Glint

`glint::html` turns HTML markup into Glint elements. The tree it builds is an ordinary Glint tree: styled by Glint's CSS engine, laid out and drawn like any page built in C++, and editable from C++ afterwards.

Markup is parsed by [lexbor](https://github.com/lexbor/lexbor) (vendored at `third_party/lexbor`, v3.0.1), which follows the WHATWG HTML spec. Broken markup is repaired the way a browser repairs it: implied tags, misnested formatting, entities, raw text. The translator (`glint_html.cpp`) then maps the DOM onto Glint elements and components.

## Enabling it

The module is an opt-in CMake target; `glint_core` stays header-only.

```cmake
# GLINT_WITH_HTML is ON by default
target_link_libraries(my_app PRIVATE glint::html)
```

```cpp
#include "glint/glint_html_parser/glint_html.hpp"
```

## API

```cpp
// Fragments (innerHTML): parsed in the context of the parent's tag, like the DOM does.
glint_html_set_inner_html(card, "<h2>Title</h2><p>Hello <b>world</b></p>");   // replaces children
glint_html_append(list, "<li>One more</li>");                                 // appends

// Whole documents: <head> stylesheets go to the document, <body> into `target`.
auto res = glint_html_load(*doc, "html/page.html", contentRoot);   // fetched via doc.onRequest
if (!res.ok) showError(res.error);
glint_html_load_string(*doc, htmlText, contentRoot, "html/page.html");   // markup you already have
```

Every call returns a `glint_html_result` with `ok`, `error` and the document's `<title>`. `glint_html_options` has:

| Option | Default | Effect |
|---|---|---|
| `applyHtmlUaSheet` | `true` | Install the HTML element defaults (see below) the first time markup is parsed. |
| `listMarkers` | `true` | Give each `<li>` of a `<ul>`/`<ol>` a marker (bullet shape or number). |

### Documents

- **`<head>`:**
  - `<style>` and `<link rel="stylesheet">` are added to the document's stylesheets.
  - `<title>` is returned in the result.
  - `<base href>` sets the base URL.
- **`<body>`:** its `id`, `class` and `style` land on `target`, and its children are appended to `target`. So `target` plays the role of `<body>`: give it `margin: 8px` if it should look like a browser page.
- **Resources:** stylesheets, images and fonts are fetched through `doc.onRequest`. Relative URLs resolve against the page URL; `url()` inside a stylesheet resolves against that stylesheet's URL, as in CSS.
- **Dropped:** `<script>`, `<template>` and comments. There is no scripting.

## What the markup becomes

| Markup | Glint |
|---|---|
| `img`, `button`, `input`, `select`, `textarea`, `form`, `progress`, `meter` | The matching component. Attributes set its fields (`src`, `type`, `value`, `placeholder`, `checked`, `name`, `min`/`max`, `rows`, `selected` options...). |
| Tags registered with `glint_element::registerElement("my-tag", ...)` | That component. |
| Any other tag | A plain `glint_element` named after the tag, so `p`, `h1` and `.card` work as selectors. |
| `id`, `class`, `style` | `id`, `className`, inline style. |
| Other attributes | Kept: `getAttribute()` and `[attr]` / `[attr="v"]` selectors work. |
| Text that is an element's only content | That element's `innerText`. |
| Mixed text and elements | Anonymous inline `#text` runs; `<br>` is a forced line break. |
| `<li>` in `<ul>` / `<ol>` | An outside marker: a drawn disc, then circle, then square with nesting, or the number (`start`, `reversed`, `value`). |
| `<table>` / `<tr>` / `<td>` | Table layout; `thead`/`tbody`/`tfoot` rows are moved into the table. |

## Default styles

On first use, the translator appends an HTML user-agent sheet ([glint_html_ua.hpp](glint_html_ua.hpp)) to the document's UA sheet. Author CSS overrides it as in a browser.

- **Browser values:** the values follow Chrome's `html.css` and are em-based, so headings and margins scale with your font sizes. Form controls follow Chrome's dark color scheme: 21 px fields in Arial 13.33 px, 13 px checkboxes, thin outlined progress and meter bars.
- **Scoping:** the document's UA sheet is shared with native Glint pages, so these rules only reach elements the translator created. Those carry a `data-glint-html` attribute, and every UA selector is scoped to it. Pages built in C++ keep Glint's own defaults, even after an HTML page was opened in the same window.
- **Browser initial values:** the top-level elements of each parse carry `data-glint-html-root`. That gives them initial values a browser has where Glint's differ: `line-height: normal`, the font's own line spacing, instead of Glint's 1.2.

## Matching Chrome

Layout is compared against Chrome element by element, using the boxes `getBoundingClientRect()` reports for the same file. Headings, paragraphs, lists, tables and form controls match within a few pixels. The engine features this relies on:
- margin collapsing
- `line-height: normal` from font metrics
- a shared text baseline per line
- `border-spacing`
- shrink-to-fit tables
- `font-family` lists and generic families (`monospace` maps to Consolas on Windows, Menlo on macOS)

The demo's **HTML** page loads `glint_user_code/web/html/demo.html` and lays it out like a browser window. Open the same file in Chrome to compare side by side.

### Known differences

- **Text inset:** Glint draws text 2 px in from its box and counts that in its width, so inline runs start a few pixels later and table cells are about 4 px wider than in Chrome.
- **No `box-sizing`:** sizes are always border-box, so an author `width` on a form control includes padding and border. Controls end up 6–8 px narrower than Chrome's content-box sizing.
- **Inline element boxes:** an inline element's box is its line height, not the font's content area, so inline backgrounds can be a few pixels taller.
- **Images:** `<img>` has no intrinsic size yet, so give it `width` and `height`. It also gets no baseline gap below it.
- **Inline nesting:** an inline element that contains elements (`<a>x <b>y</b></a>`) lays out as one box that can't wrap across lines.
- **Not supported:** inline `<svg>`/`<math>` (skipped), CSS `list-style`, `::marker` styling, scripting.

## Testing

- **`bench/glint_html_test.cpp`** checks tree shape and layout. Running `glint_html_test.exe` exits 1 on any failure.
- **`glint_html_test.exe --dump <url> <webroot> <width>`** lays a page file out like a browser window and prints each element's box (`tag x= y= w= h=`), ready to diff against a browser dump of the same file.
- **`demo/tests/headless/29_html.txt`** screenshots the demo page headless; it's part of `demo/scripts/ab_test.ps1`.
