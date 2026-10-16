// glint_html_test — checks for glint::html (glint_html_parser/).
//
//   glint_html_test.exe            run every check, exit 1 on any failure
//
// Tree checks compare a compact dump of the translated tree with the expected
// one for inputs a browser has to repair (implied tags, misnesting, tables,
// raw text). Layout checks compare a parsed tree with the same tree built in
// C++ (glint_snapshot dumps), and pin the inline white-space behaviour.

#include "glint/glint_standalone.hpp"
#include "glint/components/glint_select.hpp"
#include "glint/glint_html_parser/glint_html.hpp"
#include "glint_snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace {

int gFailures = 0;
int gChecks   = 0;

void check(bool ok, const std::string& what, const std::string& detail = {})
{
  ++gChecks;
  if (ok) return;
  ++gFailures;
  std::printf("FAIL  %s\n", what.c_str());
  if (!detail.empty()) std::printf("%s\n", detail.c_str());
}

// tag#id.class{"text"}(children) — enough to pin the tree shape.
std::string tree(const glint_element& e)
{
  std::string s = e.tagName() ? e.tagName() : "?";
  if (!e.id.empty()) s += "#" + e.id;
  if (!e.className.empty()) s += "." + std::string(e.className);
  if (!e.innerText.empty()) s += "{\"" + e.innerText + "\"}";
  // Components build their own internals (checkbox box, popup...): not markup.
  static const char* kComponents[] = { "input", "select", "textarea", "button", "progress", "meter", "img" };
  for (const char* c : kComponents) if (s.rfind(c, 0) == 0 && std::string(e.tagName()) == c) return s;
  if (!e.mChildren.empty())
  {
    s += "(";
    for (size_t i = 0; i < e.mChildren.size(); ++i)
      s += (i ? " " : "") + tree(*e.mChildren[i]);
    s += ")";
  }
  return s;
}

std::string children(const glint_element& e)
{
  std::string s;
  for (size_t i = 0; i < e.mChildren.size(); ++i)
    s += (i ? " " : "") + tree(*e.mChildren[i]);
  return s;
}

struct Doc
{
  glint_document doc{ glint_rect(0, 0, 800.f, 600.f), nullptr, [] {} };
};

void expectTree(const char* name, const char* html, const std::string& expected,
                glint_html_options opts = {})
{
  Doc d;
  opts.applyHtmlUaSheet = false;
  const auto res = glint_html_set_inner_html(&d.doc.mCanvas, html, opts);
  check(res.ok, std::string(name) + ": parse ok", res.error);
  const std::string got = children(d.doc.mCanvas);
  check(got == expected, std::string(name) + ": tree",
        "  input:    " + std::string(html) + "\n  expected: " + expected + "\n  got:      " + got);
}

void treeChecks()
{
  expectTree("implied </p>", "<p>one<p>two",
             "p{\"one\"} p{\"two\"}");
  expectTree("misnested formatting", "<b><i>x</b>y</i>",
             "b(i{\"x\"}) i{\"y\"}");
  expectTree("entities", "<span>&amp;&lt;&copy;&nbsp;&#8364;</span>",
             "span{\"&<\xC2\xA9\xC2\xA0\xE2\x82\xAC\"}");
  expectTree("raw text", "<pre>&lt;a&gt;\n  b</pre><script>var s='<b>';</script>",
             "pre{\"<a>\n  b\"}");
  expectTree("comments dropped", "<div><!-- hi -->x</div>",
             "div{\"x\"}");
  expectTree("block whitespace dropped", "<div>\n  <p>x</p>\n  <p>y</p>\n</div>",
             "div(p{\"x\"} p{\"y\"})");
  expectTree("inline whitespace kept", "<p>a <b>b</b> <i>c</i> d</p>",
             "p(#text{\"a \"} b{\"b\"} #text{\" \"} i{\"c\"} #text{\" d\"})");
  expectTree("table row groups flattened", "<table><thead><tr><th>h</th></tr></thead><tr><td>1</td></tr></table>",
             "table(tr(th{\"h\"}) tr(td{\"1\"}))");
  expectTree("void elements", "<p>a<br>b<img src=x.png><input type=checkbox></p>",
             "p(#text{\"a\"} br{\"\n\"} #text{\"b\"} img input)");
  {
    glint_html_options o; o.listMarkers = false;
    expectTree("implied </li>", "<ul><li>a<li>b</ul>", "ul(li{\"a\"} li{\"b\"})", o);
  }
  expectTree("list markers", "<ol start=3><li>a<li>b</ol><ul><li>c</ul>",
             "ol(li(::marker.glint-marker-num{\"3.\"} #text{\"a\"}) li(::marker.glint-marker-num{\"4.\"} #text{\"b\"})) "
             "ul(li(::marker.glint-marker-disc #text{\"c\"}))");
  expectTree("nested list markers", "<ul><li>a<ul><li>b<ul><li>c</ul></ul></ul>",
             "ul(li(::marker.glint-marker-disc #text{\"a\"} ul(li(::marker.glint-marker-circle #text{\"b\"} "
             "ul(li(::marker.glint-marker-square #text{\"c\"}))))))");
  expectTree("unknown + custom tags", "<section><my-widget>w</my-widget></section>",
             "section(my-widget{\"w\"})");
  expectTree("inline svg skipped", "<div>a<svg><rect/></svg></div>",
             "div{\"a\"}");

  // Attributes, typed component fields.
  {
    Doc d;
    glint_html_options o; o.applyHtmlUaSheet = false;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div id=x class='a b' data-k=v style='width:10px'></div>"
      "<img src='img/p.png'>"
      "<input type=Checkbox checked name=c>"
      "<select><option>A<optgroup><option selected>B</optgroup></select>"
      "<progress value=3 max=4></progress>", o);
    auto& kids = d.doc.mCanvas.mChildren;
    check(kids.size() == 5, "attributes: 5 nodes", children(d.doc.mCanvas));
    if (kids.size() == 5)
    {
      auto* div = kids[0].get();
      bool found = false;
      check(div->id == "x" && std::string(div->className) == "a b", "attributes: id/class");
      check(div->getAttribute("data-k", found) == "v" && found, "attributes: generic data-k");
      check(div->style.width.raw == "10px", "attributes: inline style", "  width=" + div->style.width.raw);
      check(static_cast<glint_image*>(kids[1].get())->src == "img/p.png", "img: src");
      auto* in = static_cast<glint_input*>(kids[2].get());
      check(in->type == "checkbox" && in->checked && in->name == "c", "input: type/checked/name");
      auto* sel = static_cast<glint_select*>(kids[3].get());
      check(sel->options.size() == 2 && sel->selectedIndex == 1, "select: options + optgroup + selected");
      auto* pr = static_cast<glint_progress*>(kids[4].get());
      check(pr->value == 3.f && pr->max == 4.f, "progress: value/max");
    }
  }

  // Documents: head styles, title, <base>, body attributes onto the target.
  {
    Doc d;
    const auto res = glint_html_load_string(d.doc,
      "<!doctype html><html><head><title> My  page </title>"
      "<style>.hot{color:#ff0000}</style></head>"
      "<body class=b><p class=hot>x</p></body></html>");
    check(res.ok && res.title == "My page", "document: title", "  title=\"" + res.title + "\"");
    check(std::string(d.doc.mCanvas.className) == "b", "document: body class on target");
    d.doc.updateStyleAndLayout();
    auto* p = d.doc.mCanvas.mChildren.empty() ? nullptr : d.doc.mCanvas.mChildren[0].get();
    check(p && p->computedStyle.color.value.R == 255 && p->computedStyle.color.value.G == 0,
          "document: <style> applied");
  }
}

// ── Layout ───────────────────────────────────────────────────────────────────

void layoutChecks()
{
  const char* css =
    ".card{display:flex;flex-direction:column;padding:8px;gap:4px;width:300px}"
    ".title{font-size:18px}.body{font-size:13px}";

  // Parsed tree lays out exactly like the same tree built in C++.
  {
    Doc a, b;
    a.doc.addStylesheetText(css);
    b.doc.addStylesheetText(css);
    glint_html_options o; o.applyHtmlUaSheet = false;
    glint_html_set_inner_html(&a.doc.mCanvas,
      "<div class=card><div class=title>Title</div><div class=body>Some body text that wraps "
      "across several lines inside the fixed-width card.</div></div>", o);

    auto mk = [](glint_element* parent, const char* cls, const char* text) {
      auto* e = new glint_element();
      e->typeNameOverride = "div";
      e->className = cls;
      if (text) e->innerText = text;
      parent->addChild(e);
      return e;
    };
    auto* card = mk(&b.doc.mCanvas, "card", nullptr);
    mk(card, "title", "Title");
    mk(card, "body", "Some body text that wraps across several lines inside the fixed-width card.");

    a.doc.updateStyleAndLayout();
    b.doc.updateStyleAndLayout();
    const std::string da = glint_snapshot::dump(a.doc), db = glint_snapshot::dump(b.doc);
    const size_t diffs = glint_snapshot::compare(db, da, "parsed vs built");
    check(diffs == 0, "layout: parsed == built in C++");
  }

  // Spaces between inline elements survive: "a <b>b</b>" keeps a gap.
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas, "<p>aaa <b>bbb</b></p>");
    d.doc.updateStyleAndLayout();
    auto* p = d.doc.mCanvas.mChildren[0].get();
    check(p->mChildren.size() == 2, "inline gap: two runs", children(d.doc.mCanvas));
    if (p->mChildren.size() == 2)
    {
      const auto& run = *p->mChildren[0];
      const auto& b   = *p->mChildren[1];
      const float textR = run.mInlineTextRenderLines.empty() ? 0.f
                        : run.mInlineTextRenderLines[0].x + run.mInlineTextRenderLines[0].width;
      const float gap = b.mRect.L - textR;
      char buf[96]; std::snprintf(buf, sizeof(buf), "  gap=%.2f", gap);
      check(gap > 2.f && gap < 8.f, "inline gap: one space between runs", buf);
    }
  }

  // A paragraph of wrapping inline runs is as tall as all its lines.
  {
    Doc d;
    d.doc.addStylesheetText(".narrow{width:120px}");
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div class=narrow><p>one <b>two</b> three <i>four</i> five <b>six</b> seven</p><p>after</p></div>");
    d.doc.updateStyleAndLayout();
    auto* div = d.doc.mCanvas.mChildren[0].get();
    auto* p1 = div->mChildren[0].get();
    auto* p2 = div->mChildren[1].get();
    float lastLineBottom = 0.f;
    for (auto& c : p1->mChildren)
      for (const auto& ln : c->mInlineTextRenderLines) lastLineBottom = std::max(lastLineBottom, ln.lineBoxBottom);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  p1=%.1f..%.1f lastLine=%.1f p2.top=%.1f",
                  p1->mRect.T, p1->mRect.B, lastLineBottom, p2->mRect.T);
    check(p1->mRect.H() > 30.f, "wrapped runs: paragraph spans several lines", buf);
    check(lastLineBottom <= p1->mRect.B + 0.5f, "wrapped runs: lines inside the paragraph", buf);
    check(p2->mRect.T >= p1->mRect.B, "wrapped runs: next block below", buf);
  }

  // One line of runs in a block whose width estimate is narrow (flex-grow
  // content pane, like the demo shell) stays one line tall.
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div style='display:flex;flex-direction:row;width:1400px;height:900px'>"
      "<div style='width:180px'>side</div>"
      "<div style='flex-grow:1;height:100%;overflow-y:auto;padding:16px'><div style='width:100%'>"
      "<p><span>data</span> and <span>warn</span> are colored by attribute selectors; "
      "<span>this</span> uses an inline style.</p><p>after</p></div></div></div>");
    d.doc.updateStyleAndLayout();
    const auto* page = d.doc.mCanvas.mChildren[0]->mChildren[1]->mChildren[0].get();
    const auto* p1 = page->mChildren[0].get();
    char buf[64]; std::snprintf(buf, sizeof(buf), "  p height=%.1f", p1->mRect.H());
    check(p1->mRect.H() < 30.f, "flex pane: one line of runs stays one line", buf);
  }

  // Text followed by a block (<li>Nested:<ul>...</ul>): the text runs share
  // one line above the block, they don't each take a line.
  {
    Doc d;
    glint_html_options o; o.listMarkers = true;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div style='width:600px'><ul><li>Nested: <b>x</b><ul><li>a</li><li>b</li></ul></li></ul><p>after</p></div>", o);
    d.doc.updateStyleAndLayout();
    const auto* ul = d.doc.mCanvas.mChildren[0]->mChildren[0].get();
    const auto* li = ul->mChildren[0].get();
    const auto* inner = li->mChildren.back().get();
    char buf[96]; std::snprintf(buf, sizeof(buf), "  li=%.1f..%.1f inner ul=%.1f..%.1f",
                                li->mRect.T, li->mRect.B, inner->mRect.T, inner->mRect.B);
    check(std::fabs(li->mRect.B - inner->mRect.B) < 1.f, "mixed content: li ends with its nested list", buf);
  }

  // <label><input type=checkbox> Text</label> in a shrink-to-fit flex item:
  // the label is as wide as box + space + text, so they share one line.
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<form style='display:flex;flex-direction:column;align-items:flex-start'>"
      "<label><input type=checkbox checked> Checkbox</label></form>");
    d.doc.updateStyleAndLayout();
    const auto* label = d.doc.mCanvas.mChildren[0]->mChildren[0].get();
    const auto* box  = label->mChildren[0].get();
    const auto* text = label->mChildren[1].get();
    char buf[128]; std::snprintf(buf, sizeof(buf), "  label w=%.1f box=%.1f..%.1f text=%.1f,%.1f",
                                 label->mRect.W(), box->mRect.L, box->mRect.R, text->mRect.L, text->mRect.T);
    check(text->mRect.T < box->mRect.B && text->mRect.L > box->mRect.R, "label: control and text on one line",
          std::string(buf) + "\n" + glint_snapshot::dump(d.doc));
  }

  // Nested list inside an auto-width pane (width only known at layout).
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div style='display:flex;flex-direction:row;width:1400px;height:900px'>"
      "<div style='width:180px'>side</div>"
      "<div style='flex-grow:1;height:100%;overflow-y:auto'><div style='width:100%'>"
      "<ul><li>Nested:<ul><li>Inner one</li><li>Inner two</li></ul></li></ul><ol><li>Three</li></ol>"
      "</div></div></div>");
    d.doc.updateStyleAndLayout();
    const auto* page = d.doc.mCanvas.mChildren[0]->mChildren[1]->mChildren[0].get();
    const auto* ul = page->mChildren[0].get();
    const auto* li = ul->mChildren[0].get();
    char buf[96]; std::snprintf(buf, sizeof(buf), "  ul=%.1f..%.1f li=%.1f..%.1f", ul->mRect.T, ul->mRect.B, li->mRect.T, li->mRect.B);
    check(ul->mRect.B - li->mRect.B < 1.f, "nested list: ul ends with its last item", buf);
  }

  // <select> gets a clickable box from the HTML defaults (glint_select has none).
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<form style='display:flex;flex-direction:column;align-items:flex-start'>"
      "<select><option>Apple</option><option selected>Banana</option></select></form>");
    d.doc.updateStyleAndLayout();
    auto* sel = d.doc.mCanvas.mChildren[0]->mChildren[0].get();
    const float x = (sel->mRect.L + sel->mRect.R) * 0.5f, y = (sel->mRect.T + sel->mRect.B) * 0.5f;
    char buf[96]; std::snprintf(buf, sizeof(buf), "  select %.1fx%.1f", sel->mRect.W(), sel->mRect.H());
    check(sel->mRect.H() >= 18.f && d.doc.mCanvas.HitTest(x, y) == sel, "select: has a box and receives the click", buf);
  }

  // A table with width:auto shrinks to its columns (Chrome); width:100% fills.
  {
    Doc d;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div style='width:800px'><table><tr><td>a</td><td>b</td></tr></table>"
      "<table style='width:100%'><tr><td>a</td></tr></table></div>");
    d.doc.updateStyleAndLayout();
    const auto* box = d.doc.mCanvas.mChildren[0].get();
    const float autoW = box->mChildren[0]->mRect.W(), fullW = box->mChildren[1]->mRect.W();
    char buf[96]; std::snprintf(buf, sizeof(buf), "  auto table w=%.1f, 100%% table w=%.1f", autoW, fullW);
    check(autoW > 0.f && autoW < 200.f && std::fabs(fullW - 800.f) < 1.f, "table: auto width shrinks to fit", buf);
  }

  // querySelector / querySelectorAll: stylesheet selectors, descendants only,
  // document order.
  {
    Doc d;
    glint_element* root = &d.doc.mCanvas;
    glint_html_set_inner_html(root,
      "<div class=box><p id=first>1</p><span class=b data-k=v>2</span>"
      "<section><p class=b>3</p></section></div>");
    d.doc.updateStyleAndLayout();
    auto* box = root->querySelector(".box");
    auto* first = root->querySelector("#first");
    char buf[96];
    std::snprintf(buf, sizeof(buf), "  box=%p first=%s", (void*)box, first ? first->innerText.c_str() : "null");
    check(box && first && first->innerText == "1", "querySelector: class and id", buf);
    check(root->querySelectorAll("p").size() == 2, "querySelectorAll: type, document order");
    check(root->querySelectorAll(".b").size() == 2 && root->querySelectorAll(".box > .b").size() == 1,
          "querySelectorAll: class and child combinator");
    auto* attr = root->querySelector("[data-k=\"v\"]");
    check(attr && attr->innerText == "2", "querySelector: attribute selector");
    check(box && box->querySelector(".box") == nullptr, "querySelector: descendants only");
    check(root->querySelector("p, span")->innerText == "1", "querySelector: list, first in document order");
    check(root->querySelector("%%") == nullptr && root->querySelectorAll("%%").empty(), "querySelector: invalid matches nothing");
    check(d.doc.querySelector("section p") != nullptr, "document querySelector");
  }

  // DOMContentLoaded fires once on the load target, with the tree in place.
  {
    Doc d;
    auto* page = new glint_element();
    d.doc.mCanvas.addChild(page);
    int fired = 0;
    bool sawButton = false;
    page->addEventListener("DOMContentLoaded", [&](glint_event& e) {
      ++fired;
      sawButton = e.currentTarget->querySelector("button#go") != nullptr;
    });
    const auto res = glint_html_load_string(d.doc, "<!doctype html><title>T</title><p>x<button id=go>Go</button></p>", page);
    char buf[64]; std::snprintf(buf, sizeof(buf), "  fired=%d sawButton=%d", fired, sawButton ? 1 : 0);
    check(res.ok && fired == 1 && sawButton, "DOMContentLoaded: fired once, tree queryable", buf);
  }

  // Shrink-to-fit cell with one line of inline runs stays one line tall (its
  // intrinsic-width estimate is narrower than the line; Text demo page).
  {
    Doc d;
    glint_html_options o; o.applyHtmlUaSheet = false;
    glint_html_set_inner_html(&d.doc.mCanvas,
      "<div style='display:table;width:1200px'><div style='display:table-row'>"
      "<div style='display:table-cell;width:120px;padding:8px'>top</div>"
      "<div style='display:table-cell;padding:8px'><span style='display:inline;font-size:24px'>Big</span>"
      "<div style='display:inline-block;width:28px;height:18px'></div>"
      "<span style='display:inline;font-size:13px'>small text</span></div></div></div>", o);
    d.doc.updateStyleAndLayout();
    const auto* row = d.doc.mCanvas.mChildren[0]->mChildren[0].get();
    char buf[64]; std::snprintf(buf, sizeof(buf), "  row height=%.1f", row->mRect.H());
    check(row->mRect.H() < 50.f, "table cell: one line of inline runs", buf);
  }
}

} // namespace

// --dump <url> <webroot> <viewportW>: lay out a page file the way a browser
// window would (body margin 8px) and print every element's border box, one
// per line, indented by depth: "tag x= y= w= h=". Compare with a browser's
// getBoundingClientRect() of the same file.
int dumpPage(const std::string& url, const std::string& webRoot, float viewportW)
{
  glint_document doc{ glint_rect(0, 0, viewportW, 4000.f), nullptr, [] {} };
  doc.onRequest = [webRoot](glint_resource_request& req) {
    std::string p = req.pathname.empty() ? req.url : req.pathname;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\')) p.erase(p.begin());
    req.fromFile(webRoot + "/" + p);
  };
  auto* page = new glint_element();
  page->style.margin = "8px";   // the browser's body margin
  doc.mCanvas.addChild(page);
  const auto res = glint_html_load(doc, url, page);
  if (!res.ok) { std::printf("load failed: %s\n", res.error.c_str()); return 1; }
  for (int i = 0; i < 3; ++i) doc.updateStyleAndLayout();

  std::function<void(const glint_element&, int)> walk = [&](const glint_element& e, int depth) {
    const std::string tag = e.tagName() ? e.tagName() : "?";
    if (tag == "::marker") return;
    const glint_rect& r = e.mRect;
    std::printf("%*s%s%s%s x=%.1f y=%.1f w=%.1f h=%.1f\n", depth * 2, "", tag.c_str(),
                e.className.empty() ? "" : ".", std::string(e.className).c_str(), r.L, r.T, r.W(), r.H());
    static const char* kComponents[] = { "input", "select", "textarea", "button", "progress", "meter", "img" };
    for (const char* comp : kComponents) if (tag == comp) return;
    for (const auto& c : e.mChildren) walk(*c, depth + 1);
  };
  walk(*page, 0);
  if (std::getenv("GLINT_DUMP_SNAPSHOT")) std::printf("%s\n", glint_snapshot::dump(doc).c_str());
  if (std::getenv("GLINT_DUMP_FONT"))
  {
    SkFont f = glint_element::skFont(14.f, "Kanit", 400.f, "normal");
    SkFontMetrics m; f.getMetrics(&m);
    SkString fam; f.getTypeface()->getFamilyName(&fam);
    std::printf("Kanit 14: family=%s ascent=%.2f descent=%.2f leading=%.2f -> %.2f\n",
                fam.c_str(), -m.fAscent, m.fDescent, m.fLeading, -m.fAscent + m.fDescent + m.fLeading);
  }
  return 0;
}

int main(int argc, char** argv)
{
  if (argc >= 5 && std::string(argv[1]) == "--dump")
    return dumpPage(argv[2], argv[3], static_cast<float>(std::atof(argv[4])));

  treeChecks();
  layoutChecks();
  std::printf("%d/%d checks passed\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
