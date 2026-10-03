// glint_bench — headless reflow benchmark and layout-snapshot regression gate.
//
// Builds generated documents (no window, raster surface), drives frames and
// input through the real document APIs, and reports per-phase timings
// (cascade / tick / layout / paint) plus reflow counters.
//
//   glint_bench                               run every scenario, print a table
//   glint_bench --scenario list --frames 120  one scenario
//   glint_bench --json                        machine-readable output
//   glint_bench --no-paint                    style + layout only
//   glint_bench --gpu                         render on D3D12 (offscreen) instead of CPU raster;
//                                             each frame waits for the GPU ("gpu" column)
//   glint_bench --snapshot-out DIR            write DIR/<scenario>.txt layout dumps
//   glint_bench --snapshot-compare DIR        diff against DIR/<scenario>.txt (exit 1 on diff)
//   glint_bench --verify                      after the run, force a full relayout of the
//                                             same state and diff layout dump + pixels
//                                             (catches incremental/paint-only drift; exit 1)
//
// Animation timing uses a fixed 16 ms step clock, so snapshots are
// deterministic for a given --frames value. Always benchmark Release builds.

#include "glint/glint_standalone.hpp"
#include "glint/components/glint_button.hpp"
#include "glint/components/glint_checkbox.hpp"
#include "glint/components/glint_progress.hpp"
#include "glint/components/glint_select.hpp"
#include "glint/components/glint_slider.hpp"
#include "glint/components/input/glint_input.hpp"
#include "glint_snapshot.hpp"

#include "include/core/SkBitmap.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) \
    && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
  #define GLINT_BENCH_HAS_D3D12 1
  #include "glint/platform/win32/glint_win32_surface_shared.hpp"
  #include "include/gpu/ganesh/GrDirectContext.h"
  #include "include/gpu/ganesh/SkSurfaceGanesh.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace
{

// ── Heap accounting ────────────────────────────────────────────────────────────
// Every plain operator new/delete in the process goes through these, so the
// bench can report how much heap a page holds (Skia's own malloc-based
// allocations are not included). Each block carries its size in a header.
// The header is __STDCPP_DEFAULT_NEW_ALIGNMENT__ bytes (16 on x64) so blocks
// keep the alignment compilers assume for operator new: clang-compiled Skia
// uses aligned SSE stores on them (alignof(max_align_t) is only 8 on MSVC).
std::atomic<int64_t> gHeapLive{ 0 };
constexpr std::size_t kHeapHeader = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

} // namespace

void* operator new(std::size_t n)
{
  constexpr std::size_t kHeader = kHeapHeader;
  auto* p = static_cast<unsigned char*>(std::malloc(n + kHeader));
  if (!p) throw std::bad_alloc();
  *reinterpret_cast<std::size_t*>(p) = n;
  gHeapLive.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
  return p + kHeader;
}
void operator delete(void* ptr) noexcept
{
  if (!ptr) return;
  constexpr std::size_t kHeader = kHeapHeader;
  auto* p = static_cast<unsigned char*>(ptr) - kHeader;
  gHeapLive.fetch_sub(static_cast<int64_t>(*reinterpret_cast<std::size_t*>(p)), std::memory_order_relaxed);
  std::free(p);
}
void operator delete(void* ptr, std::size_t) noexcept { operator delete(ptr); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* ptr) noexcept { operator delete(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { operator delete(ptr); }

namespace
{

// ── GPU (D3D12, offscreen) ─────────────────────────────────────────────────────
#ifdef GLINT_BENCH_HAS_D3D12
struct GpuContext
{
  gr_cp<IDXGIAdapter1>       adapter;
  gr_cp<ID3D12Device>        device;
  gr_cp<ID3D12CommandQueue>  queue;
  sk_sp<GrDirectContext>     context;
  std::string                adapterName;

  bool init()
  {
    gr_cp<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (!glint_win32_surface::chooseHardwareAdapter(factory.get(), adapter, device)) return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)))) return false;
    GrD3DBackendContext backend{};
    backend.fAdapter = adapter;
    backend.fDevice  = device;
    backend.fQueue   = queue;
    context = GrDirectContext::MakeDirect3D(backend);
    DXGI_ADAPTER_DESC1 desc = {};
    if (SUCCEEDED(adapter->GetDesc1(&desc)))
    {
      char name[256] = {};
      ::WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
      adapterName = name;
    }
    return context != nullptr;
  }
};
GpuContext* gGpu = nullptr;
#endif

// ── Fixed-step animation clock ─────────────────────────────────────────────────
std::chrono::steady_clock::time_point gClockNow{};
std::chrono::steady_clock::time_point benchClock() { return gClockNow; }
void advanceClock(int ms) { gClockNow += std::chrono::milliseconds(ms); }

// ── Tree helpers ──────────────────────────────────────────────────────────────
glint_element* el(glint_element* parent, const char* cls, const std::string& text = {})
{
  auto* e = new glint_element();
  parent->addChild(e);
  if (cls && *cls) e->className = cls;
  if (!text.empty()) e->innerText = text;
  return e;
}

const char* kLorem =
  "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor "
  "incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud "
  "exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.";

std::string words(int i, int n)
{
  static const char* w[] = { "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
                             "golf", "hotel", "india", "juliet", "kilo", "lima" };
  std::string s;
  for (int k = 0; k < n; ++k)
  {
    if (k) s += ' ';
    s += w[(i * 7 + k * 3) % 12];
  }
  return s;
}

void collect(glint_element* root, const char* cls, std::vector<glint_element*>& out)
{
  if (root->className.size() && std::strstr(root->className.c_str(), cls)) out.push_back(root);
  for (auto& c : root->mChildren) collect(c.get(), cls, out);
}

// ── Scenario definition ───────────────────────────────────────────────────────
struct Ctx
{
  glint_document& doc;
  int frame = 0;            // index of the frame about to run (0-based)
  std::vector<glint_element*> targets;
};

struct Scenario
{
  const char* name;
  const char* description;
  const char* css;
  std::function<void(glint_element* root)> build;
  // Called once after the first frame (targets can be collected here).
  std::function<void(Ctx&)> setup;
  // Called before every measured frame.
  std::function<void(Ctx&)> step;
};

const glint_mouse_mod kNoMod{};

std::vector<Scenario> makeScenarios()
{
  std::vector<Scenario> s;

  // 1. Deep nested flex: alternating row/column containers with auto sizes.
  s.push_back({
    "deep_flex", "8 chains of 14 nested auto-sized flex levels with text leaves",
    ".root{display:flex;flex-direction:column;padding:4px}"
    ".row{display:flex;flex-direction:row;padding:2px;gap:4px}"
    ".col{display:flex;flex-direction:column;padding:2px;gap:2px}"
    ".t{font-size:12px}",
    [](glint_element* root) {
      auto* top = el(root, "root");
      for (int chain = 0; chain < 8; ++chain)
      {
        glint_element* cur = el(top, "row");
        for (int d = 0; d < 14; ++d)
        {
          el(cur, "t", words(chain + d, 2));
          cur = el(cur, (d % 2) ? "row" : "col");
        }
        el(cur, "t", "leaf");
      }
    },
    [](Ctx& c) { c.doc.setDirty(false); },
    [](Ctx& c) { c.doc.setDirty(false); } });   // force a reflow every frame

  // 2. Long list: 2000 rows of wrapping text inside a scroll container.
  s.push_back({
    "list", "2000 flex rows with wrapping text in overflow:auto (static frames)",
    ".scroller{height:600px;overflow-y:auto;display:flex;flex-direction:column}"
    ".item{display:flex;flex-direction:row;padding:4px 8px;gap:8px}"
    ".title{flex:1;font-size:13px}"
    ".tag{font-size:11px;padding:2px 4px}",
    [](glint_element* root) {
      auto* sc = el(root, "scroller");
      for (int i = 0; i < 2000; ++i)
      {
        auto* row = el(sc, "item");
        el(row, "title", words(i, 6 + (i % 9)));
        el(row, "tag", "#" + std::to_string(i));
      }
    },
    [](Ctx& c) { c.doc.setDirty(false); },
    [](Ctx& c) { c.doc.setDirty(false); } });   // force a reflow every frame

  // 3. Wheel-scroll the long list.
  s.push_back({
    "list_scroll", "2000-row list, one wheel tick per frame",
    ".scroller{height:600px;overflow-y:auto;display:flex;flex-direction:column}"
    ".item{display:flex;flex-direction:row;padding:4px 8px;gap:8px}"
    ".title{flex:1;font-size:13px}"
    ".tag{font-size:11px;padding:2px 4px}",
    [](glint_element* root) {
      auto* sc = el(root, "scroller");
      for (int i = 0; i < 2000; ++i)
      {
        auto* row = el(sc, "item");
        el(row, "title", words(i, 6 + (i % 9)));
        el(row, "tag", "#" + std::to_string(i));
      }
    },
    nullptr,
    [](Ctx& c) { c.doc.OnMouseWheel(200.f, 300.f, 0.f, 40.f, kNoMod); } });   // +Y scrolls down

  // 4. Table.
  s.push_back({
    "table", "150 x 8 table of text cells",
    ".tbl{display:table;width:100%}"
    ".tr{display:table-row}"
    ".td{display:table-cell;padding:2px 4px;font-size:12px}",
    [](glint_element* root) {
      auto* t = el(root, "tbl");
      for (int r = 0; r < 150; ++r)
      {
        auto* tr = el(t, "tr");
        for (int col = 0; col < 8; ++col)
          el(tr, "td", words(r + col, 1 + (r + col) % 3));
      }
    },
    [](Ctx& c) { c.doc.setDirty(false); },
    [](Ctx& c) { c.doc.setDirty(false); } });

  // 5. Nested scroll containers.
  s.push_back({
    "nested_scroll", "outer scroller holding 12 inner scrollers of 40 rows",
    ".outer{height:600px;overflow-y:auto;display:flex;flex-direction:column;gap:8px}"
    ".inner{height:180px;overflow-y:auto;display:flex;flex-direction:column;flex-shrink:0}"
    ".r{padding:3px 6px;font-size:12px}",
    [](glint_element* root) {
      auto* outer = el(root, "outer");
      for (int i = 0; i < 12; ++i)
      {
        auto* inner = el(outer, "inner");
        for (int r = 0; r < 40; ++r)
          el(inner, "r", words(i * 40 + r, 4));
      }
    },
    [](Ctx& c) { c.doc.setDirty(false); },
    [](Ctx& c) { c.doc.setDirty(false); } });

  // 6. Hover sweep, color-only :hover.
  const char* cardsCss =
    ".grid{display:flex;flex-direction:row;flex-wrap:wrap;gap:4px;padding:4px}"
    ".card{width:90px;height:40px;padding:4px;font-size:11px;background-color:#202830}"
    ".card:hover{background-color:#405060}";
  auto buildCards = [](glint_element* root) {
    auto* g = el(root, "grid");
    for (int i = 0; i < 400; ++i)
      el(g, "card", words(i, 2));
  };
  auto hoverSetup = [](Ctx& c) { collect(&c.doc.mCanvas, "card", c.targets); };
  auto hoverStep = [](Ctx& c) {
    if (c.targets.empty()) return;
    const glint_rect r = c.targets[static_cast<size_t>(c.frame) % c.targets.size()]->mRect;
    c.doc.OnMouseOver((r.L + r.R) * 0.5f, (r.T + r.B) * 0.5f, kNoMod);
  };
  s.push_back({ "hover_color", "400 cards, hover moves one card per frame (color-only rule)",
                cardsCss, buildCards, hoverSetup, hoverStep });

  // 7. Hover sweep, layout-changing :hover.
  s.push_back({ "hover_layout", "400 cards, hover moves one card per frame (padding rule)",
    ".grid{display:flex;flex-direction:row;flex-wrap:wrap;gap:4px;padding:4px}"
    ".card{width:90px;height:40px;padding:4px;font-size:11px;background-color:#202830}"
    ".card:hover{padding:8px}",
    buildCards, hoverSetup, hoverStep });

  // 8. Color transitions.
  s.push_back({ "transition_color", "200 cells transitioning background-color (300 ms)",
    ".wrap{display:flex;flex-direction:row;flex-wrap:wrap;gap:2px}"
    ".cell{width:40px;height:20px;background-color:#102030;transition:background-color 300ms linear}"
    ".cell.on{background-color:#F0A020}",
    [](glint_element* root) {
      auto* w = el(root, "wrap");
      for (int i = 0; i < 200; ++i) el(w, "cell");
    },
    [](Ctx& c) {
      collect(&c.doc.mCanvas, "cell", c.targets);
      for (auto* t : c.targets) t->classList.add("on");
    },
    nullptr });

  // 9. Width transitions (layout-affecting).
  s.push_back({ "transition_width", "50 bars transitioning width (300 ms) in a column",
    ".wrap{display:flex;flex-direction:column;gap:2px}"
    ".bar{width:100px;height:10px;background-color:#305070;transition:width 300ms linear}"
    ".bar.on{width:400px}"
    ".t{font-size:11px}",
    [](glint_element* root) {
      auto* w = el(root, "wrap");
      for (int i = 0; i < 50; ++i) { el(w, "bar"); el(w, "t", words(i, 3)); }
    },
    [](Ctx& c) {
      collect(&c.doc.mCanvas, "bar", c.targets);
      for (auto* t : c.targets) t->classList.add("on");
    },
    nullptr });

  // 10. Keyframe opacity loop.
  s.push_back({ "keyframes_opacity", "150 elements with an infinite opacity @keyframes loop",
    "@keyframes pulse{from{opacity:1}to{opacity:0.2}}"
    ".wrap{display:flex;flex-direction:row;flex-wrap:wrap;gap:2px}"
    ".p{width:40px;height:20px;background-color:#70A0D0;animation:pulse 1s linear infinite}",
    [](glint_element* root) {
      auto* w = el(root, "wrap");
      for (int i = 0; i < 150; ++i) el(w, "p");
    },
    nullptr, nullptr });

  // 11. Inline formatting context with long paragraphs.
  s.push_back({ "inline_text", "40 paragraphs mixing inline spans and long text",
    ".para{padding:4px;font-size:13px}"
    ".s{display:inline}"
    ".b{display:inline;font-weight:700}",
    [](glint_element* root) {
      for (int i = 0; i < 40; ++i)
      {
        auto* p = el(root, "para");
        el(p, "s", kLorem);
        el(p, "b", words(i, 5));
        el(p, "s", kLorem);
      }
    },
    [](Ctx& c) { c.doc.setDirty(false); },
    [](Ctx& c) { c.doc.setDirty(false); } });

  // 12. Hover that restyles descendants and siblings (".card:hover .title").
  s.push_back({ "hover_descendant", "200 cards; :hover restyles a child and the next sibling",
    ".grid{display:flex;flex-direction:column;gap:2px;padding:4px}"
    ".card{display:flex;flex-direction:row;gap:6px;padding:4px;background-color:#202830}"
    ".title{font-size:12px;color:#a0a0a0}"
    ".meta{font-size:11px;color:#606060}"
    ".card:hover .title{color:#ffcc00}"
    ".card:hover + .card .meta{color:#00ccff}",
    [](glint_element* root) {
      auto* g = el(root, "grid");
      for (int i = 0; i < 200; ++i)
      {
        auto* card = el(g, "card");
        el(card, "title", words(i, 3));
        el(card, "meta", "#" + std::to_string(i));
      }
    },
    [](Ctx& c) { collect(&c.doc.mCanvas, "card", c.targets); },
    [](Ctx& c) {
      if (c.targets.empty()) return;
      const glint_rect r = c.targets[static_cast<size_t>(c.frame) % c.targets.size()]->mRect;
      c.doc.OnMouseOver((r.L + r.R) * 0.5f, (r.T + r.B) * 0.5f, kNoMod);
    } });

  // 13. Components: rows of real controls (Layout() overrides, hover hooks).
  s.push_back({ "components", "40 rows of checkbox/button/slider/progress/input/select, hover sweep",
    ".rows{display:flex;flex-direction:column;gap:4px;padding:4px}"
    ".row{display:flex;flex-direction:row;gap:8px;padding:2px}"
    ".lbl{font-size:12px;width:80px}",
    [](glint_element* root) {
      auto* rows = el(root, "rows");
      for (int i = 0; i < 40; ++i)
      {
        auto* row = el(rows, "row");
        el(row, "lbl", "Row " + std::to_string(i));
        auto* cb = new glint_checkbox();
        row->addChild(cb);
        cb->text = words(i, 2);
        cb->checked = (i % 3) == 0;
        auto* bt = new glint_button();
        row->addChild(bt);
        bt->innerText = "Action " + std::to_string(i);
        auto* sl = new glint_slider();
        row->addChild(sl);
        sl->style.width = 120.f;
        sl->value = static_cast<float>(i % 10) / 10.f;
        auto* pr = new glint_progress();
        row->addChild(pr);
        pr->style.width = 100.f;
        pr->value = static_cast<float>(i % 7) / 7.f;
        auto* in = new glint_input();
        row->addChild(in);
        in->style.width = 140.f;
        auto* se = new glint_select();
        row->addChild(se);
        se->style.width = 120.f;
      }
    },
    [](Ctx& c) {
      std::function<void(glint_element*)> walk = [&](glint_element* e) {
        if (!e->_isPlainElement() && e->mParent && e->mParent->className.size()
            && std::strstr(e->mParent->className.c_str(), "row"))
          c.targets.push_back(e);
        for (auto& ch : e->mChildren) walk(ch.get());
      };
      walk(&c.doc.mCanvas);
    },
    [](Ctx& c) {
      if (c.targets.empty()) return;
      const glint_rect r = c.targets[static_cast<size_t>(c.frame * 7) % c.targets.size()]->mRect;
      c.doc.OnMouseOver((r.L + r.R) * 0.5f, (r.T + r.B) * 0.5f, kNoMod);
    } });

  // 14. Structural selectors under child-list changes: every frame inserts at
  //     the front / middle / end, removes or reorders list items, so
  //     :nth-child stripes, :last-child borders, `+` / `~` and :empty move.
  s.push_back({ "structural", "list of 24 items; insert / remove / reorder one child per frame",
    ".nthlist{display:flex;flex-direction:column;gap:2px;padding:4px}"
    "li{padding:2px 6px;font-size:12px;background-color:#202830;border-bottom:1px solid #303030}"
    "li:nth-child(odd){background-color:#2a3a4a}"
    "li:nth-child(3n+1){color:#ffcc00}"
    "li:nth-last-child(2){color:#00ccff}"
    "li:first-child{padding-top:8px}"
    "li:last-child{border-bottom:4px solid #ff0000}"
    "li:only-child{font-size:16px}"
    "li:nth-of-type(2){margin-left:10px}"
    "li:not(:first-of-type):last-of-type{padding-bottom:9px}"
    "li:nth-last-of-type(3){background-color:#504030}"
    "b{font-size:11px;color:#808080}"
    "b:first-of-type{color:#ff8080}"
    "b:only-of-type{font-weight:700}"
    "li + li{border-top:1px solid #405060}"
    "b ~ li{padding-left:14px}"
    "b + li{margin-top:6px}"
    "li:first-child div{color:#00ff00}"
    "li:nth-child(even) > div{font-size:13px}"
    ".emptyhost{display:flex;flex-direction:row;gap:4px;padding:4px}"
    "p{width:40px;height:16px;background-color:#305030}"
    "p:empty{height:8px;background-color:#ff00ff}"
    "p:empty + .after{width:80px}"
    ".after{width:20px;height:16px;background-color:#303050}"
    ".textp:empty{background-color:#00ffff}",
    [](glint_element* root) {
      auto* host = el(root, "emptyhost");
      auto* p = new glint_element();
      p->typeNameOverride = "p";
      host->addChild(p);
      el(host, "after");
      // :empty through innerText alone (no children).
      auto* tp = new glint_element();
      tp->typeNameOverride = "p";
      tp->className = "textp";
      host->addChild(tp);
      el(host, "after");
      auto* list = el(root, "nthlist");
      for (int i = 0; i < 24; ++i)
      {
        auto* li = new glint_element();
        li->typeNameOverride = (i % 7 == 3) ? "b" : "li";
        list->addChild(li);
        el(li, "", words(i, 2));
      }
      auto* solo = el(root, "nthlist sololist");
      auto* only = new glint_element();
      only->typeNameOverride = "li";
      solo->addChild(only);
      el(only, "", "only");
    },
    [](Ctx& c) {
      // targets: [0] the list, [1] the single-item list, [2] the :empty host <p>.
      std::vector<glint_element*> lists;
      collect(&c.doc.mCanvas, "nthlist", lists);
      std::vector<glint_element*> hosts;
      collect(&c.doc.mCanvas, "emptyhost", hosts);
      if (lists.size() < 2 || hosts.empty()) return;
      c.targets = { lists[0], lists[1], hosts[0]->mChildren.front().get() };
    },
    [](Ctx& c) {
      if (c.targets.size() < 3) return;
      glint_element* list = c.targets[0];
      auto& kids = list->mChildren;
      auto make = [&](const char* tag) {
        auto* e = new glint_element();
        e->typeNameOverride = tag;
        return e;
      };
      auto withText = [&](glint_element* e) { el(e, "", words(c.frame, 2)); };
      switch (c.frame % 6)
      {
        case 0: { auto* e = make("li"); list->insertBefore(e, kids.empty() ? nullptr : kids.front().get()); withText(e); break; }
        case 1: { auto* e = make("li"); list->insertBefore(e, kids.size() > 1 ? kids[kids.size() / 2].get() : nullptr); withText(e); break; }
        case 2: { auto* e = make("li"); list->addChild(e); withText(e); break; }
        case 3:
          if (kids.size() > 1) list->removeChild(kids[kids.size() / 3].get());
          if (kids.size() > 30) list->removeChild(kids.front().get());
          break;
        case 4:
          // Reorder: last to front, and an early item to the end.
          if (kids.size() > 2)
          {
            list->insertBefore(kids.back().get(), kids.front().get());
            list->insertBefore(kids[1].get(), nullptr);
          }
          break;
        case 5: { auto* e = make("b"); list->insertBefore(e, kids.size() > 2 ? kids[2].get() : nullptr); withText(e); break; }
      }
      // :only-child flips on the single-item list, :empty on the host <p>.
      glint_element* solo = c.targets[1];
      if (c.frame % 2 == 0) { auto* e = make("li"); solo->addChild(e); withText(e); }
      else if (solo->mChildren.size() > 1) solo->removeChild(solo->mChildren.back().get());
      glint_element* p = c.targets[2];
      if (p->mChildren.empty()) el(p, "", "x");
      else                      p->clearChildren();
      // The text-only <p> (third child of the host).
      if (p->mParent && p->mParent->mChildren.size() > 2)
      {
        glint_element* tp = p->mParent->mChildren[2].get();
        tp->innerText = tp->innerText.empty() ? "t" : "";
        tp->setDirty(false);
      }
    } });

  // 15. :has(): cards restyled by what they contain or what follows them; each
  //     frame adds / removes a descendant, toggles a class inside a card,
  //     adds / removes a direct child, hovers, or empties a card.
  s.push_back({ "has", "30 cards; :has() flips as descendants / siblings change",
    ".hgrid{display:flex;flex-direction:column;gap:2px;padding:4px}"
    ".hcard{display:flex;flex-direction:row;gap:4px;padding:4px;min-height:14px;background-color:#202830}"
    ".hcard:has(img){background-color:#304050}"
    ".hcard:has(> .badge){padding-left:12px}"
    ".hcard:has(.sel) .ttl{color:#ffcc00}"
    ".hcard:has(+ .hcard .sel){border-bottom:3px solid #ff0000}"
    ".hcard:has(~ .hcard img){margin-left:4px}"
    ".hcard:not(:has(.ttl)){min-height:30px}"
    ".hcard:has(.ttl:hover){background-color:#506070}"
    ".hgrid:has(.sel){padding-top:10px}"
    ".inner{display:flex;flex-direction:row;padding:2px}"
    ".ttl{font-size:12px;color:#a0a0a0}"
    ".badge{width:10px;height:10px;background-color:#00ff00}"
    "img{width:12px;height:12px;background-color:#ff8800}",
    [](glint_element* root) {
      auto* g = el(root, "hgrid");
      for (int i = 0; i < 30; ++i)
      {
        auto* card = el(g, "hcard");
        el(card, "ttl", words(i, 2));
        if (i % 3 == 0)
        {
          auto* inner = el(card, "inner");
          el(inner, "ttl", words(i + 1, 1));
        }
      }
    },
    [](Ctx& c) { collect(&c.doc.mCanvas, "hcard", c.targets); },
    [](Ctx& c) {
      if (c.targets.empty()) return;
      const size_t n = c.targets.size();
      glint_element* card = c.targets[static_cast<size_t>(c.frame * 7) % n];
      auto cls = [](glint_element* e) -> const std::string& { return e->className; };
      auto makeImg = []() {
        auto* e = new glint_element();
        e->typeNameOverride = "img";
        return e;
      };
      // Deepest container in the card: images go below .inner when there is one.
      glint_element* host = card;
      for (auto& ch : card->mChildren)
        if (cls(ch.get()) == "inner") host = ch.get();
      switch (c.frame % 6)
      {
        case 0: host->addChild(makeImg()); break;
        case 1:
          for (auto& ch : host->mChildren)
            if (std::string(ch->tagName()) == "img") { host->removeChild(ch.get()); break; }
          break;
        case 2:
          for (auto& ch : host->mChildren)
            if (cls(ch.get()) == "ttl" || cls(ch.get()) == "ttl sel")
            {
              ch->className = cls(ch.get()) == "ttl" ? "ttl sel" : "ttl";
              break;
            }
          break;
        case 3:
        {
          bool removed = false;
          for (auto& ch : card->mChildren)
            if (cls(ch.get()) == "badge") { card->removeChild(ch.get()); removed = true; break; }
          if (!removed) el(c.frame % 12 == 3 ? host : card, "badge");
          break;
        }
        case 4:
        {
          glint_element* ttl = nullptr;
          for (auto& ch : card->mChildren)
            if (cls(ch.get()) == "ttl" || cls(ch.get()) == "ttl sel") ttl = ch.get();
          if (ttl)
          {
            const glint_rect r = ttl->mRect;
            c.doc.OnMouseOver((r.L + r.R) * 0.5f, (r.T + r.B) * 0.5f, kNoMod);
          }
          break;
        }
        case 5:
          if (card->mChildren.empty()) el(card, "ttl", words(c.frame, 2));
          else                         card->clearChildren();
          break;
      }
    } });

  return s;
}

// ── Measurement ───────────────────────────────────────────────────────────────
struct PhaseSamples
{
  std::vector<double> cascade, tick, layout, paint, gpu, total;
};

double median(std::vector<double> v)
{
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

double p95(std::vector<double> v)
{
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * 0.95))];
}

size_t countNodes(const glint_element* e)
{
  size_t n = 1;
  for (const auto& c : e->mChildren) n += countNodes(c.get());
  return n;
}

struct Options
{
  std::string only;
  int frames = 60;
  bool noPaint = false;
  bool json = false;
  std::string snapshotOut;
  std::string snapshotCompare;
  bool verify = false;
  bool paintPhases = false;
  bool gpu = false;
};

struct Result
{
  std::string name;
  size_t nodes = 0;
  double buildMs = 0.0;        // tree build + CSS cascade
  double firstFrameMs = 0.0;   // first full frame (style + layout + paint)
  int64_t heapBytes = 0;       // live heap held by the page after its first frame
  PhaseSamples s;
  glint_perf_counters perFrame; // counters averaged per measured frame (integers rounded down)
  size_t layoutFrames = 0;
  size_t snapshotDiffs = 0;
  bool snapshotChecked = false;
  size_t verifyDiffs = 0;      // layout-dump lines + pixel mismatch (1) vs a forced full relayout
  bool verifyChecked = false;
  uint64_t styleVerifyFailures = 0;   // skipped style merges that differed from a real merge
};

uint64_t pixelHash(SkSurface& surface)
{
  SkPixmap pm;
  SkBitmap readback;
  if (!surface.peekPixels(&pm))
  {
    // GPU surface: read it back first.
    if (!readback.tryAllocPixels(surface.imageInfo()) || !surface.readPixels(readback, 0, 0))
      return 0;
    pm = readback.pixmap();
  }
  uint64_t h = 1469598103934665603ull;   // FNV-1a over all rows
  for (int y = 0; y < pm.height(); ++y)
  {
    const auto* row = static_cast<const uint8_t*>(pm.addr(0, y));
    for (size_t i = 0; i < pm.info().minRowBytes(); ++i) { h ^= row[i]; h *= 1099511628211ull; }
  }
  return h;
}

Result runScenario(const Scenario& sc, const Options& opt)
{
  Result res;
  res.name = sc.name;
  gClockNow = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

  constexpr int W = 1280, H = 800;
  glint_document doc{ glint_rect(0, 0, static_cast<float>(W), static_cast<float>(H)), nullptr, [] {} };
  sk_sp<SkSurface> surface;
#ifdef GLINT_BENCH_HAS_D3D12
  if (opt.gpu && gGpu)
    surface = SkSurfaces::RenderTarget(gGpu->context.get(), skgpu::Budgeted::kNo,
                                       SkImageInfo::Make(W, H, kRGBA_8888_SkColorType,
                                                         kPremul_SkAlphaType));
#endif
  if (!surface) surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(W, H));
  SkCanvas& canvas = *surface->getCanvas();

  // GPU: submit the frame and wait for it, so frame times include GPU work
  // (DrawToCanvas only records commands). Returns the wait in ms.
  auto finishGpu = [&]() -> double {
#ifdef GLINT_BENCH_HAS_D3D12
    if (opt.gpu && gGpu)
    {
      const auto s = std::chrono::steady_clock::now();
      gGpu->context->flushAndSubmit(GrSyncCpu::kYes);
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s).count();
    }
#endif
    return 0.0;
  };

  const std::string css = sc.css ? sc.css : "";
  doc.onRequest = [&css](glint_resource_request& req) {
    if (req.url == "bench://scenario.css") req.fromBuffer(css.data(), css.size(), true);
  };

  // Hosts clear the surface before every paint; anti-aliased edges would
  // otherwise accumulate across frames.
  auto paint = [&]() {
    canvas.clear(SK_ColorBLACK);
    doc.DrawToCanvas(canvas);
    finishGpu();
  };
  auto paintTimed = [&]() -> double {   // like paint(), returns the GPU wait
    canvas.clear(SK_ColorBLACK);
    doc.DrawToCanvas(canvas);
    return finishGpu();
  };
  auto runFrame = [&]() {
    if (opt.noPaint) doc.updateStyleAndLayout();
    else             paint();
  };

  const uint64_t cascadeFailsBefore = glint_perf().cascadeVerifyFailures;
  const int64_t heapBefore = gHeapLive.load();
  const auto t0 = std::chrono::steady_clock::now();
  doc.loadStylesheet("bench://scenario.css");
  sc.build(&doc.mCanvas);
  const auto t1 = std::chrono::steady_clock::now();
  paint();   // first frame always paints so rects/text caches exist
  const auto t2 = std::chrono::steady_clock::now();
  res.buildMs      = std::chrono::duration<double, std::milli>(t1 - t0).count();
  res.firstFrameMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
  res.nodes = countNodes(&doc.mCanvas);
  res.heapBytes = gHeapLive.load() - heapBefore;

  Ctx ctx{ doc };
  if (sc.setup) sc.setup(ctx);
  advanceClock(16);

  const glint_perf_counters before = glint_perf();
  for (int f = 0; f < opt.frames; ++f)
  {
    ctx.frame = f;
    if (sc.step) sc.step(ctx);
    const auto fs = std::chrono::steady_clock::now();
    double gpuMs = 0.0;
    if (opt.noPaint) runFrame();
    else             gpuMs = paintTimed();
    const auto fe = std::chrono::steady_clock::now();
    const glint_frame_stats& st = doc.lastFrameStats();
    res.s.cascade.push_back(st.cascadeMs);
    res.s.tick.push_back(st.tickMs);
    res.s.layout.push_back(st.layoutMs);
    res.s.paint.push_back(st.paintMs);
    res.s.gpu.push_back(gpuMs);
    res.s.total.push_back(std::chrono::duration<double, std::milli>(fe - fs).count() + st.cascadeMs);
    if (st.layoutRan) ++res.layoutFrames;
    advanceClock(16);
  }
  const glint_perf_counters after = glint_perf();

  if (opt.paintPhases)
  {
    // One extra profiled paint (glint's own render timing profile).
    glint_element::setRenderTimingEnabled(true);
    glint_element::resetRenderTimingProfile();
    paint();
    glint_element::setRenderTimingEnabled(false);
    const auto prof = glint_element::snapshotRenderTimingProfile();
    std::printf("[%s] paint phases (ms, inclusive): self %.3f content %.3f children %.3f "
                "transform %.3f/%.3f filter %.3f/%.3f backdrop %.3f mask %.3f\n",
                sc.name, prof.selfPaintMs, prof.contentMs, prof.childrenMs,
                prof.transformDirectMs, prof.transformOffscreenMs,
                prof.filterInPlaceMs, prof.filterOffscreenMs, prof.backdropMs, prof.maskMs);
  }
  const uint64_t n = static_cast<uint64_t>(std::max(1, opt.frames));
#define GLINT_BENCH_AVG(f) res.perFrame.f = (after.f - before.f) / n
  GLINT_BENCH_AVG(applyCss);
  GLINT_BENCH_AVG(rulesTested);
  GLINT_BENCH_AVG(mergedStyle);
  GLINT_BENCH_AVG(refreshLayoutStyle);
  GLINT_BENCH_AVG(layoutCalls);
  GLINT_BENCH_AVG(childPrefW);
  GLINT_BENCH_AVG(childPrefH);
  GLINT_BENCH_AVG(measureIntrinsicW);
  GLINT_BENCH_AVG(measureIntrinsicH);
  GLINT_BENCH_AVG(measureText);
  GLINT_BENCH_AVG(measureTextBytes);
  GLINT_BENCH_AVG(lengthParses);
  GLINT_BENCH_AVG(styleSkips);
  GLINT_BENCH_AVG(layoutSkips);
#undef GLINT_BENCH_AVG
  res.styleVerifyFailures = after.styleVerifyFailures - before.styleVerifyFailures;

  if (opt.verify)
  {
    // Repaint the current state as-is, then again after a from-scratch reflow
    // (incremental style + layout off). Animation clock is not advanced, so
    // both frames see the same time.
    paint();
    const std::string incDump = glint_snapshot::dump(doc);
    const uint64_t incPixels = pixelHash(*surface);
    glint_element::sIncrementalStyle  = false;
    glint_element::sIncrementalLayout = false;
    doc.restyleAll();   // a from-scratch cascade too: catches missed dependent restyles
    doc.setDirty(false);
    paint();
    glint_element::sIncrementalStyle  = true;
    glint_element::sIncrementalLayout = true;
    const std::string fullDump = glint_snapshot::dump(doc);
    const uint64_t fullPixels = pixelHash(*surface);
    res.verifyChecked = true;
    res.verifyDiffs = glint_snapshot::compare(fullDump, incDump, sc.name);
    if (incPixels != fullPixels)
    {
      std::printf("[%s] pixels differ from a full relayout\n", sc.name);
      ++res.verifyDiffs;
    }

    // The intrinsic-size memo must not change layout: relayout without it.
    glint_element::sLayoutMemoEnabled = false;
    doc.setDirty(false);
    paint();
    glint_element::sLayoutMemoEnabled = true;
    const std::string noMemoDump = glint_snapshot::dump(doc);
    const std::string label = std::string(sc.name) + " memo";
    res.verifyDiffs += glint_snapshot::compare(noMemoDump, fullDump, label.c_str());

    // Paint bounds (culling + bounded opacity layers) must not change pixels:
    // repaint the same state without them.
    paint();
    const uint64_t boundedPixels = pixelHash(*surface);
    glint_element::sUsePaintBounds = false;
    paint();
    glint_element::sUsePaintBounds = true;
    if (pixelHash(*surface) != boundedPixels)
    {
      std::printf("[%s] pixels differ with paint bounds off\n", sc.name);
      ++res.verifyDiffs;
    }

    const uint64_t cascadeFails = glint_perf().cascadeVerifyFailures - cascadeFailsBefore;
    if (cascadeFails)
    {
      std::printf("[%s] %llu indexed cascade result(s) differed from the reference cascade\n",
                  sc.name, static_cast<unsigned long long>(cascadeFails));
      res.verifyDiffs += static_cast<size_t>(cascadeFails);
    }

    // Incremental style: every skipped merge was re-checked during the run.
    const uint64_t styleFails = glint_perf().styleVerifyFailures - before.styleVerifyFailures;
    if (styleFails)
    {
      std::printf("[%s] %llu skipped style merge(s) differed from a real merge\n",
                  sc.name, static_cast<unsigned long long>(styleFails));
      res.verifyDiffs += static_cast<size_t>(styleFails);
    }
  }

  if (!opt.snapshotOut.empty() || !opt.snapshotCompare.empty())
  {
    // Snapshot reflects a painted frame even in --no-paint mode.
    if (opt.noPaint) paint();
    const std::string dump = glint_snapshot::dump(doc);
    if (!opt.snapshotOut.empty())
    {
      const std::string path = opt.snapshotOut + "/" + sc.name + ".txt";
      if (!glint_snapshot::writeFile(path, dump))
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
    }
    if (!opt.snapshotCompare.empty())
    {
      const std::string path = opt.snapshotCompare + "/" + sc.name + ".txt";
      std::string expected;
      res.snapshotChecked = true;
      if (!glint_snapshot::readFile(path, expected))
      {
        std::fprintf(stderr, "missing golden %s\n", path.c_str());
        res.snapshotDiffs = 1;
      }
      else
      {
        res.snapshotDiffs = glint_snapshot::compare(expected, dump, sc.name);
      }
    }
  }
  return res;
}

void printTable(const std::vector<Result>& results, const Options& opt)
{
  std::printf("glint_bench: %d frames per scenario%s, %s (times in ms, median / p95)\n\n",
              opt.frames, opt.noPaint ? ", no paint" : "", opt.gpu ? "GPU" : "CPU raster");
  std::printf("sizeof(glint_element) = %zu, sizeof(glint_style) = %zu bytes\n\n",
              sizeof(glint_element), sizeof(glint_style));
  std::printf("%-18s %6s %8s %8s %9s %7s | %-13s %-13s %-13s %-13s %-13s %-13s | %s\n",
              "scenario", "nodes", "build", "first", "heap KB", "B/node", "cascade", "tick",
              "layout", "paint", "gpu", "frame", "layouts");
  for (const auto& r : results)
  {
    auto cell = [](const std::vector<double>& v) {
      char b[32];
      std::snprintf(b, sizeof(b), "%6.3f/%-6.3f", median(v), p95(v));
      return std::string(b);
    };
    std::printf("%-18s %6zu %8.2f %8.2f %9.0f %7.0f | %s %s %s %s %s %s | %zu/%zu\n",
                r.name.c_str(), r.nodes, r.buildMs, r.firstFrameMs,
                static_cast<double>(r.heapBytes) / 1024.0,
                static_cast<double>(r.heapBytes) / static_cast<double>(std::max<size_t>(1, r.nodes - 1)),
                cell(r.s.cascade).c_str(), cell(r.s.tick).c_str(), cell(r.s.layout).c_str(),
                cell(r.s.paint).c_str(), cell(r.s.gpu).c_str(), cell(r.s.total).c_str(),
                r.layoutFrames, r.s.total.size());
  }
#ifdef GLINT_PERF_COUNTERS
  std::printf("\nper-frame counters\n");
  std::printf("%-18s %8s %8s %8s %8s %8s %8s %8s %8s %8s %10s %8s %8s %8s\n",
              "scenario", "applyCss", "rules", "merge", "refresh", "layout", "prefW", "prefH",
              "intrW/H", "measure", "measBytes", "lenParse", "skipped", "laySkip");
  for (const auto& r : results)
  {
    const auto& c = r.perFrame;
    std::printf("%-18s %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %8llu %10llu %8llu %8llu %8llu\n",
                r.name.c_str(),
                (unsigned long long)c.applyCss, (unsigned long long)c.rulesTested,
                (unsigned long long)c.mergedStyle, (unsigned long long)c.refreshLayoutStyle,
                (unsigned long long)c.layoutCalls, (unsigned long long)c.childPrefW,
                (unsigned long long)c.childPrefH,
                (unsigned long long)(c.measureIntrinsicW + c.measureIntrinsicH),
                (unsigned long long)c.measureText, (unsigned long long)c.measureTextBytes,
                (unsigned long long)c.lengthParses, (unsigned long long)c.styleSkips,
                (unsigned long long)c.layoutSkips);
  }
#endif
}

void printJson(const std::vector<Result>& results, const Options& opt)
{
  std::printf("{\"frames\":%d,\"noPaint\":%s,\"gpu\":%s,\"scenarios\":[", opt.frames,
              opt.noPaint ? "true" : "false", opt.gpu ? "true" : "false");
  for (size_t i = 0; i < results.size(); ++i)
  {
    const auto& r = results[i];
    auto ph = [](const char* k, const std::vector<double>& v) {
      std::printf("\"%s\":{\"median\":%.4f,\"p95\":%.4f}", k, median(v), p95(v));
    };
    std::printf("%s{\"name\":\"%s\",\"nodes\":%zu,\"buildMs\":%.4f,\"firstFrameMs\":%.4f,\"heapBytes\":%lld,",
                i ? "," : "", r.name.c_str(), r.nodes, r.buildMs, r.firstFrameMs,
                static_cast<long long>(r.heapBytes));
    ph("cascade", r.s.cascade); std::printf(",");
    ph("tick", r.s.tick);       std::printf(",");
    ph("layout", r.s.layout);   std::printf(",");
    ph("paint", r.s.paint);     std::printf(",");
    ph("gpu", r.s.gpu);         std::printf(",");
    ph("frame", r.s.total);
    const auto& c = r.perFrame;
    std::printf(",\"layoutFrames\":%zu,\"counters\":{\"applyCss\":%llu,\"rulesTested\":%llu,"
                "\"mergedStyle\":%llu,\"refreshLayoutStyle\":%llu,\"layoutCalls\":%llu,"
                "\"childPrefW\":%llu,\"childPrefH\":%llu,\"measureIntrinsicW\":%llu,"
                "\"measureIntrinsicH\":%llu,\"measureText\":%llu,\"measureTextBytes\":%llu,"
                "\"lengthParses\":%llu,\"styleSkips\":%llu}",
                r.layoutFrames,
                (unsigned long long)c.applyCss, (unsigned long long)c.rulesTested,
                (unsigned long long)c.mergedStyle, (unsigned long long)c.refreshLayoutStyle,
                (unsigned long long)c.layoutCalls, (unsigned long long)c.childPrefW,
                (unsigned long long)c.childPrefH, (unsigned long long)c.measureIntrinsicW,
                (unsigned long long)c.measureIntrinsicH, (unsigned long long)c.measureText,
                (unsigned long long)c.measureTextBytes, (unsigned long long)c.lengthParses,
                (unsigned long long)c.styleSkips);
    if (r.snapshotChecked) std::printf(",\"snapshotDiffs\":%zu", r.snapshotDiffs);
    std::printf("}");
  }
  std::printf("]}\n");
}

} // namespace

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  Options opt;
  for (int i = 1; i < argc; ++i)
  {
    const std::string a = argv[i];
    auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string{}; };
    if      (a == "--scenario")         opt.only = next();
    else if (a == "--frames")           opt.frames = std::max(1, std::atoi(next().c_str()));
    else if (a == "--no-paint")         opt.noPaint = true;
    else if (a == "--json")             opt.json = true;
    else if (a == "--snapshot-out")     opt.snapshotOut = next();
    else if (a == "--snapshot-compare") opt.snapshotCompare = next();
    else if (a == "--verify")           opt.verify = true;
    else if (a == "--paint-phases")     opt.paintPhases = true;
    else if (a == "--no-memo")          glint_element::sLayoutMemoEnabled = false;
    else if (a == "--gpu")              opt.gpu = true;
    else if (a == "--no-incremental")   glint_element::sIncrementalStyle = glint_element::sIncrementalLayout = false;
    else if (a == "--no-inc-layout")    glint_element::sIncrementalLayout = false;
    else if (a == "--no-cascade-index") glint_document::sCascadeIndexEnabled = false;
    else if (a == "--list")
    {
      for (const auto& sc : makeScenarios()) std::printf("%-18s %s\n", sc.name, sc.description);
      return 0;
    }
    else
    {
      std::fprintf(stderr,
        "usage: glint_bench [--scenario NAME] [--frames N] [--no-paint] [--json]\n"
        "                   [--snapshot-out DIR] [--snapshot-compare DIR] [--verify] [--list]\n");
      return 2;
    }
  }

  glint_element::sAnimationClock = &benchClock;

  if (opt.gpu)
  {
#ifdef GLINT_BENCH_HAS_D3D12
    static GpuContext gpu;
    if (!gpu.init())
    {
      std::fprintf(stderr, "--gpu: D3D12 initialization failed\n");
      return 2;
    }
    gGpu = &gpu;
    std::fprintf(opt.json ? stderr : stdout, "GPU: %s (D3D12)\n", gpu.adapterName.c_str());
#else
    std::fprintf(stderr, "--gpu: this build has no D3D12 backend\n");
    return 2;
#endif
  }
  if (opt.verify)
  {
    glint_element::sVerifyIncrementalStyle = true;
    glint_document::sVerifyCascadeIndex    = true;
  }

  std::vector<Result> results;
  for (const auto& sc : makeScenarios())
  {
    if (!opt.only.empty() && opt.only != "all" && opt.only != sc.name) continue;
    results.push_back(runScenario(sc, opt));
  }
  if (results.empty())
  {
    std::fprintf(stderr, "no scenario named '%s' (try --list)\n", opt.only.c_str());
    return 2;
  }

  if (opt.json) printJson(results, opt);
  else          printTable(results, opt);

  int rc = 0;
  size_t totalDiffs = 0, verifyDiffs = 0;
  bool checked = false, verified = false;
  for (const auto& r : results)
  {
    totalDiffs  += r.snapshotDiffs;  checked  |= r.snapshotChecked;
    verifyDiffs += r.verifyDiffs;    verified |= r.verifyChecked;
  }
  FILE* summary = opt.json ? stderr : stdout;
  if (checked)
  {
    std::fprintf(summary, "\nsnapshot: %zu differing line(s) across %zu scenario(s)\n",
                 totalDiffs, results.size());
    if (totalDiffs) rc = 1;
  }
  if (verified)
  {
    std::fprintf(summary, "verify: %zu mismatch(es) vs full relayout across %zu scenario(s)\n",
                 verifyDiffs, results.size());
    if (verifyDiffs) rc = 1;
  }
  return rc;
}
