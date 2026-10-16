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

## Making it interactive

There is no scripting: `onclick="..."` attributes are kept as plain attributes and do nothing. Behavior is attached in C++ to the elements the markup became, the same way as for a page built in C++.

1. **`DOMContentLoaded`:** `glint_html_load` / `glint_html_load_string` fire it on `target` once the tree is built and styled. Add the listener before loading; it runs again on every (re)load of the page.
2. **Find elements** with `querySelector` / `querySelectorAll`, on any element or the document. They take the same selectors as stylesheets, search descendants only, and return results in document order.
3. **Attach behavior:** use the component callbacks (`glint_button::onClick`, `glint_input::onChange`, `glint_select::onChange`, `glint_form::onSubmit`...) or `addEventListener` on any element. Events bubble, so a single listener on a container can handle many children.

```cpp
page->addEventListener("DOMContentLoaded", [this](glint_event& e) {
  glint_element* root = e.currentTarget;

  if (auto* save = dynamic_cast<glint_button*>(root->querySelector("#save")))
    save->onClick = [this] { saveSettings(); };

  // <button data-action="like">, <button data-action="reset">...
  for (glint_element* btn : root->querySelectorAll("[data-action]"))
  {
    bool found = false;
    const std::string action = btn->getAttribute("data-action", found);
    btn->addEventListener("click", [this, action](glint_event&) { runAction(action); });
  }

  if (auto* form = dynamic_cast<glint_form*>(root->querySelector("form#signup")))
    form->onSubmit = [](const std::vector<glint_form_value>& values, glint_element*) {
      return true;   // values: name/value pairs, like a browser submit
    };
});
glint_html_load(*doc, "html/settings.html", page);
```

To change content later, set an element's fields and mark it dirty, e.g. `el->innerText = "2 likes"; el->setDirty(false);`. You can also replace a subtree with `glint_html_set_inner_html`.

There is no page `load` event yet. Images load lazily, when first painted, and have no per-image `load`/`error` events.

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
