#pragma once

/**
 * glint_document.hpp
 * Scene-graph root for the glint component library.
 *
 * glint_document is a plain C++ class — NOT a host control.  It owns and drives
 * the entire glint scene graph.  A host bridge injects the callback it
 * needs:
 *
 *   requestRedraw   — called when any component asks for a repaint
 *
 * The adapter converts host-specific mouse events to glint_mouse_mod and calls
 * the corresponding On* methods here.
 *
 * Responsibilities:
 *   - Two-phase render: Layout() (top-down), then Draw() traversal.
 *   - Mouse routing: hit-tests the tree, dispatches DOM events + virtuals.
 *   - Hover tracking (mouseout / mouseover) and press tracking.
 *   - dblclick detection (browser-compatible sequence: click → click → dblclick).
 *   - Tag registry: RegisterTag / GetNodeWithTag.
 */

#include "glint_element.hpp"
#include "render/glint_tree_node.hpp"
#include "events/glint_keyboard_event.hpp"
#include "glint_bus.hpp"
#include "default_style.hpp"
#if defined(__APPLE__)
#include "platform/glint_platform.hpp"
class glint_window_mac;  // forward declaration for macWindow field
#endif
#include "glint_css_parser/glint_css.hpp"
#include "glint_css_parser/glint_css_dom_adapter.hpp"

#include <array>
#include <fstream>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// GetDoubleClickTime() is Win32-only.
#if defined(OS_WIN) || defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

// ── GlintMatchedCssRule ───────────────────────────────────────────────────────
// One CSS qualified rule that matched a given element during selector matching.
// Produced by glint_document::matchedCssRulesFor() and consumed by the
// inspector's style panel to render DevTools-style origin rule blocks.
struct GlintMatchedCssRule
{
  std::string                     selectorText;  // serialised selector (e.g. "#header", ".btn")
  GlintCssSpecificity              specificity;   // CSS specificity (a, b, c)
  std::vector<GlintCssDeclaration> declarations;  // property declarations in this rule (copies)
  size_t                          sourceOrder = 0; // rule index across all stylesheets
  std::string                     sourceUrl;     // URL/path of the stylesheet (e.g. "/styles/main.css")
  uint32_t                        sourceLine = 0; // 1-based line number of the rule in the source
};

struct GlintFlatQualifiedRuleRef
{
  const GlintCssQualifiedRule* rule = nullptr;
  std::string                 selectorText;
  std::string                 sourceUrl;
  size_t                      sourceOrder = 0;
};

struct GlintMatchedCssCacheKey
{
  uint64_t nodeId = 0;
  uint64_t stylesheetRevision = 0;
  uint64_t treeRevision = 0;
  uint64_t selectorSignature = 0;
  bool     forcePseudoClasses = false;

  bool operator==(const GlintMatchedCssCacheKey& other) const
  {
    return nodeId == other.nodeId
        && stylesheetRevision == other.stylesheetRevision
        && treeRevision == other.treeRevision
        && selectorSignature == other.selectorSignature
        && forcePseudoClasses == other.forcePseudoClasses;
  }
};

struct GlintMatchedCssCacheKeyHash
{
  size_t operator()(const GlintMatchedCssCacheKey& key) const
  {
    const size_t h1 = std::hash<uint64_t>{}(key.nodeId);
    const size_t h2 = std::hash<uint64_t>{}(key.stylesheetRevision);
    const size_t h3 = std::hash<uint64_t>{}(key.treeRevision);
    const size_t h4 = std::hash<uint64_t>{}(key.selectorSignature);
    const size_t h5 = std::hash<bool>{}(key.forcePseudoClasses);
    return ((((h1 * 1315423911u) ^ h2) * 1315423911u) ^ h3) * 1315423911u ^ (h4 + h5);
  }
};

// ── glint_document ────────────────────────────────────────────────────────────────

class glint_document final
{
public:
  // ── Canvas ─────────────────────────────────────────────────────────────────
  // All top-level scene-graph nodes are children of the canvas.  The canvas
  // has no visual (transparent background) and covers the full window.
  glint_element mCanvas;

  /** Convenience alias — equivalent to mCanvas.add.component(...) etc.
   *  Requires glint.hpp (not glint_document.hpp alone) to get the full method set. */
  glint_element::ComponentAdd& add{ mCanvas.add };

  // ── Construction ────────────────────────────────────────────────────────────

  /**
   * @param bounds        Full-window rect (typically 0,0,W,H).
   * @param pG            glint_canvas* stamped on every node (may be nullptr for
   *                      adapters that manage their own draw surface).
   * @param requestRedraw Called whenever any subtree node calls setDirty().
   */
  explicit glint_document(const glint_rect&          bounds,
                      glint_canvas*            pG,
                      std::function<void()> requestRedraw)
    : mRequestRedraw(std::move(requestRedraw))
  {
    mCanvas.mRect          = mCanvas.mPaintRECT = bounds;
    mCanvas.mpG            = pG;
    mCanvas.mRoot          = this;
    // Give the canvas a stable ID (it's not added via addChild, so we stamp it here).
    mCanvas.mId            = glint_id_counter.fetch_add(1, std::memory_order_relaxed);
    // Propagate the same callback so addChild() stamps it on every child.
    mCanvas.mRequestRedraw = mRequestRedraw;
    mCanvas.mRequestRedrawDetailed = mRequestRedrawDetailed;
    mCanvas.mApplyCss      = [this](glint_element* el){ _applyCssToElement(el); };
    mCanvas.mKeyframeRegistryPtr_ = &mKeyframeRegistry_;
    // Share the tree mutex so every subsequent addChild propagates it.
    mCanvas.mTreeMutex     = &mTreeMutex;
    mCanvas.mParentW       = bounds.W();
    mCanvas.mParentH       = bounds.H();
    mCanvas.typeNameOverride = "body";
    mCanvas.attachSubtree();
    // Take a mutable in-memory copy of the UA stylesheet so the inspector can
    // patch its declarations at runtime without touching the static singleton.
    mUaSheet = glint_default_user_agent_stylesheet();
    _rebuildQualifiedRuleCache();
  }

  ~glint_document()
  {
    auto clearDebugNode = [this](std::atomic<glint_element*>& slot) {
      if (auto* node = slot.load(); node && node->mRoot == this)
        slot.store(nullptr);
    };

    clearDebugNode(glint_debug::hoveredNode);
    clearDebugNode(glint_debug::inspectedNode);
    clearDebugNode(glint_debug::pinnedNode);

    if (glint_debug::inspectorDoc == this)
      glint_debug::inspectorDoc = nullptr;
  }

  // ── Inspector notifications — published via glint_bus ───────────────────────
  // Use glint_bus::subscribe<glint_tree_changed_event>(...) etc. to listen.
  // Filter by `e.root == yourRoot` if multiple roots are alive simultaneously.

  /** Global key interceptor — called first in OnKeyDown, before any focused-node
   *  dispatch, so it fires regardless of focus state.
   *  Return true to consume the event (stop further propagation). */
  std::function<bool(const glint_key_press&)> onGlobalKeyDown;

  /**
   * Resource request callback — mirroring WebView2 / WKWebView interception.
   *
   * Called before every file-based resource load (CSS background-image, mask url(),
   * SVG files, etc.).  Populate req.responseData with raw encoded bytes to intercept
   * the load; leave it null to fall through to the normal disk-load + cache path.
   *
   * The req.source pointer is the requesting element — valid only during the
   * callback, do not store it.
   *
   * Example:
   *   doc.onRequest = [](glint_resource_request& req) {
   *     if (req.url.rfind("res://", 0) == 0)
   *       req.responseData = myResourceMap[req.url];
   *     // else: fall through to disk
   *   };
   */
  std::function<void(glint_resource_request&)> onRequest;

  /**
   * Load and parse a CSS file, routing through onRequest if set.
   *
   * Mirrors browser stylesheet loading:
   *   1. A glint_resource_request is constructed with the given path as its URL,
   *      parsed via parseUrl() (scheme://host/pathname?query#hash).
   *   2. If onRequest is registered it is called — the handler may intercept the
   *      load (req.fromFile / req.fromBuffer / req.fromData) or ignore it.
   *   3. When the handler responds with status 200, the response bytes are used
   *      directly as the CSS source text.
   *      When no handler is registered, or the handler does not call any respond
   *      helper, the file is read directly from disk as a fallback.
   *   4. The parsed GlintCssStylesheet is appended to the document's stylesheet
   *      list, accessible via stylesheets().
   *
   * Returns true on success, false if the resource could not be fetched or
   * the handler returned an error.  Inspect lastCSSError() for details.
   *
   * Example:
   *   doc.onRequest = [](glint_resource_request& req) {
   *     if (req.pathname == "/main.css")
   *       req.fromBuffer(MAIN_CSS_DATA, MAIN_CSS_SIZE);
   *   };
   *   doc.loadStylesheet("res://app/main.css");  // triggers onRequest
   *   doc.loadStylesheet("styles/theme.css");    // falls back to disk
   */
  bool loadStylesheet(const std::string& path)
  {
    glint_resource_request req;
    req.url    = path;
    req.type   = glint_resource_request::Type::Stylesheet;
    req.source = nullptr;
    req.parseUrl();

    if (onRequest) onRequest(req);

    std::string cssText;
    bool fromDisk = false;

    if (req.handled && req.statusCode == 200 && req.responseData)
    {
      // Handler intercepted with data.
      cssText.assign(static_cast<const char*>(req.responseData->data()),
                     req.responseData->size());
    }
    else if (!req.handled)
    {
      // No handler or handler ignored the request — fall back to disk.
      fromDisk = true;
      std::ifstream f(path);
      if (!f.is_open())
      {
        mLastCSSError = "loadStylesheet: cannot open \"" + path + "\"";
        glint_network_log_push_disk(&networkLog, path, req.type, false);
        return false;
      }
      cssText.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    }
    else
    {
      // Handler responded with an explicit error.
      mLastCSSError = "loadStylesheet: request error (" + std::to_string(req.statusCode)
                    + ") for \"" + path + "\": " + req.statusMessage;
      glint_network_log_push(&networkLog, path, req.type, req);
      return false;
    }

    auto sheet = GlintCssParser::parseStylesheet(cssText);
    // Use the original request pathname as the source URL (strips query/hash
    // and is more readable than the full URL in the inspector).
    sheet.sourceUrl = req.pathname.empty() ? path : req.pathname;
    mStylesheets.push_back(std::move(sheet));
    _processFontFaces(mStylesheets.back());
    _rebuildKeyframeRegistry();
    _rebuildQualifiedRuleCache();
    _invalidateMatchedCssRuleCache(/*bumpStylesheetRevision=*/true);
    _applyCssToTree(&mCanvas);
    mLastCSSError.clear();

    // Log to the network tab — after successful parse so byteSize is meaningful.
    if (fromDisk)
      glint_network_log_push_disk(&networkLog, path, req.type, true);
    else
      glint_network_log_push(&networkLog, path, req.type, req);

    // Track sheets that were served from a real file for hot-reload polling.
    // This covers both:
    //   - direct disk fallback (fromDisk == true, watchPath == path)
    //   - onRequest handlers that called req.fromFile() (req.resolvedFilePath)
    const std::string watchPath = fromDisk ? path : req.resolvedFilePath;
    if (!watchPath.empty())
    {
      std::error_code ec;
      auto mtime = std::filesystem::last_write_time(watchPath, ec);
      if (!ec)
      {
        const std::string& surl = mStylesheets.back().sourceUrl;
        // Remove any stale entry before re-inserting (handles _tickHotReload reloads).
        mHotWatchedPaths.erase(
          std::remove_if(mHotWatchedPaths.begin(), mHotWatchedPaths.end(),
            [&watchPath](const HotWatchEntry& e){ return e.path == watchPath; }),
          mHotWatchedPaths.end());
        mHotWatchedPaths.push_back({ path, watchPath, surl, mtime });
      }
    }

    return true;
  }

  /** Remove all stylesheets previously loaded via loadStylesheet(). */
  void clearStylesheets()
  {
    mStylesheets.clear();
    mKeyframeRegistry_.clear();
    mHotWatchedPaths.clear();
    _rebuildQualifiedRuleCache();
    _invalidateMatchedCssRuleCache(/*bumpStylesheetRevision=*/true);
  }

  /** Enable or disable polling-based stylesheet hot reload.
   *  Disabled by default so shipped standalone windows do not self-sustain an
   *  idle render loop purely to watch CSS files on disk. */
  void setStylesheetHotReloadEnabled(bool enabled) { mStylesheetHotReloadEnabled = enabled; }

  /** True when polling-based stylesheet hot reload is active. */
  bool isStylesheetHotReloadEnabled() const { return mStylesheetHotReloadEnabled; }

  /** Read-only access to all loaded stylesheets for use with GlintCssCascade::resolve(). */
  const std::vector<GlintCssStylesheet>& stylesheets() const { return mStylesheets; }

  /** Resolve a stylesheet sourceUrl to the on-disk file path that backed it, when any. */
  std::string stylesheetResolvedPath(const std::string& sourceUrl) const
  {
    for (auto it = mHotWatchedPaths.rbegin(); it != mHotWatchedPaths.rend(); ++it)
    {
      if (it->sourceUrl != sourceUrl) continue;

      std::error_code ec;
      std::filesystem::path p(it->path);
      p = p.make_preferred().lexically_normal();
      if (!p.is_absolute())
        p = std::filesystem::absolute(p, ec).make_preferred().lexically_normal();
      return p.string();
    }

    if (sourceUrl.empty()) return {};

    std::error_code ec;
    std::filesystem::path p(sourceUrl);
    if (!p.is_absolute())
      p = std::filesystem::absolute(p, ec);
    if (!ec && std::filesystem::exists(p, ec))
      return p.make_preferred().lexically_normal().string();

    return {};
  }

  /**
   * Mutate a single declaration in a loaded stylesheet and re-cascade the element.
   * Called by the inspector when the user edits a value inside a CSS rule block.
   * Writes go to the stylesheet AST (and through cssStyle_ layer), never to el->style.
   */
  void updateCssDeclaration(const std::string& sourceUrl,
                             uint32_t           sourceLine,
                             const std::string& prop,
                             const std::string& val,
                             glint_element*      target)
  {
    // Strip trailing "!important" from val and record the flag separately so
    // decl.important is set correctly and decl.value stores only the bare value.
    std::string cleanVal = val;
    bool        important = false;
    {
      while (!cleanVal.empty() && std::isspace((unsigned char)cleanVal.back()))
        cleanVal.pop_back();
      if (cleanVal.size() >= 9)
      {
        std::string tail = cleanVal.substr(cleanVal.size() - 9);
        for (char& c : tail) c = (char)std::tolower((unsigned char)c);
        if (tail == "important")
        {
          cleanVal.resize(cleanVal.size() - 9);
          while (!cleanVal.empty() && std::isspace((unsigned char)cleanVal.back()))
            cleanVal.pop_back();
          if (!cleanVal.empty() && cleanVal.back() == '!')
          {
            cleanVal.pop_back();
            while (!cleanVal.empty() && std::isspace((unsigned char)cleanVal.back()))
              cleanVal.pop_back();
            important = true;
          }
        }
      }
    }

    // Helper: mutate the declaration in a matched qualified rule.
    auto mutateQRule = [&](GlintCssQualifiedRule& qr) {
      if (qr.sourceLine != sourceLine) return;
      bool found = false;
      for (auto& decl : qr.declarations)
      {
        if (decl.property == prop)
        {
          decl.value       = cleanVal;
          decl.important   = important;
          decl.disabled    = false;  // re-enabling via inspector un-comments the declaration
          decl.valueTokens.clear();  // stale after mutation; applyOne uses value string
          found = true;
          break;
        }
      }
      if (!found)
      {
        GlintCssDeclaration d;
        d.property = prop;
        d.value    = cleanVal;
        d.important = important;
        qr.declarations.push_back(std::move(d));
      }
    };

    for (auto& sheet : mStylesheets)
    {
      if (sheet.sourceUrl != sourceUrl) continue;
      for (auto& topRule : sheet.rules)
      {
        if (topRule.kind == GlintCssStylesheet::Rule::Kind::QUALIFIED)
        {
          if (topRule.qualified) mutateQRule(*topRule.qualified);
        }
        else if (topRule.kind == GlintCssStylesheet::Rule::Kind::AT && topRule.atRule)
        {
          const std::string& n = topRule.atRule->name;
          if (n == "media" || n == "supports" || n == "layer" || n == "document")
          {
            for (auto& child : topRule.atRule->children)
            {
              if (child.kind == GlintCssAtRule::ChildRule::Kind::QUALIFIED && child.qualified)
                mutateQRule(*child.qualified);
            }
          }
        }
      }
    }

    // Also search the mutable UA sheet copy — allows the inspector to edit
    // UA-layer declarations at runtime without adding an inline override.
    if (mUaSheet.sourceUrl == sourceUrl)
    {
      for (auto& topRule : mUaSheet.rules)
      {
        if (topRule.kind == GlintCssStylesheet::Rule::Kind::QUALIFIED)
        {
          if (topRule.qualified) mutateQRule(*topRule.qualified);
        }
        else if (topRule.kind == GlintCssStylesheet::Rule::Kind::AT && topRule.atRule)
        {
          const std::string& n = topRule.atRule->name;
          if (n == "media" || n == "supports" || n == "layer" || n == "document")
          {
            for (auto& child : topRule.atRule->children)
            {
              if (child.kind == GlintCssAtRule::ChildRule::Kind::QUALIFIED && child.qualified)
                mutateQRule(*child.qualified);
            }
          }
        }
      }
    }

    // Re-cascade the entire tree: the edited rule may match elements other than
    // the currently-inspected one (e.g. a class selector applied to siblings).
    _invalidateMatchedCssRuleCache(/*bumpStylesheetRevision=*/true);
    _applyCssToTree(&mCanvas);
    if (target)
      target->setDirty(false);   // repaint
    // Note: disk write is deferred — the inspector's save button (or Ctrl+S)
    // calls saveRuleToFile() explicitly when the user is ready to commit.
  }

  /**
   * Remove a single declaration from a loaded stylesheet and re-cascade.
   * Called by the inspector's trash button on a CSS rule row.
   */
  void removeCssDeclaration(const std::string& sourceUrl,
                             uint32_t           sourceLine,
                             const std::string& prop,
                             glint_element*      target)
  {
    for (auto& sheet : mStylesheets)
    {
      if (sheet.sourceUrl != sourceUrl) continue;
      for (auto& topRule : sheet.rules)
      {
        if (topRule.kind != GlintCssStylesheet::Rule::Kind::QUALIFIED) continue;
        if (!topRule.qualified) continue;
        if (topRule.qualified->sourceLine != sourceLine) continue;
        auto& decls = topRule.qualified->declarations;
        decls.erase(
          std::remove_if(decls.begin(), decls.end(),
            [&prop](const GlintCssDeclaration& d){ return d.property == prop; }),
          decls.end());
      }
    }
    if (mUaSheet.sourceUrl == sourceUrl)
    {
      for (auto& topRule : mUaSheet.rules)
      {
        if (topRule.kind != GlintCssStylesheet::Rule::Kind::QUALIFIED) continue;
        if (!topRule.qualified) continue;
        if (topRule.qualified->sourceLine != sourceLine) continue;
        auto& decls = topRule.qualified->declarations;
        decls.erase(
          std::remove_if(decls.begin(), decls.end(),
            [&prop](const GlintCssDeclaration& d){ return d.property == prop; }),
          decls.end());
      }
    }
    _invalidateMatchedCssRuleCache(/*bumpStylesheetRevision=*/true);
    _applyCssToTree(&mCanvas);
    if (target)
      target->setDirty(false);

    // Persist the deletion to the source file on disk immediately — a deletion
    // is a discrete, unambiguous action so no deferred-save step is needed.
    _patchCssFile(sourceUrl, sourceLine, prop, {}, /*remove=*/true);
  }

  /**
   * Serialize the entire AST state of one CSS qualified rule back to the
   * source file on disk.  Called by the inspector's per-rule save button and
   * by saveAllDirtyRules() (Ctrl+S).  All pending inspector edits (value
   * changes since the last save) are flushed in a single file write.
   *
   * Algorithm:
   *   1. Find the real disk path via mHotWatchedPaths.
   *   2. Find the rule in the in-memory AST.
   *   3. Read the file.  Locate the rule block (from sourceLine to `}`).
   *   4. Extract the original selector text from the file text.
   *   5. Re-serialise all AST declarations as "  prop: val;\n" lines.
   *   6. Replace the original rule block with the serialised version.
   *   7. Write the file.  Bump stored mtime to suppress the hot-reload tick.
   */
  void saveRuleToFile(const std::string& sourceUrl, uint32_t sourceLine)
  {
    // ── 1. Disk path ───────────────────────────────────────────────────
    std::string diskPath;
    size_t      watchIdx = SIZE_MAX;
    for (size_t i = 0; i < mHotWatchedPaths.size(); ++i)
    {
      if (mHotWatchedPaths[i].sourceUrl == sourceUrl)
        { diskPath = mHotWatchedPaths[i].path; watchIdx = i; break; }
    }
    if (diskPath.empty()) return;

    // ── 2. Find rule in AST ──────────────────────────────────────────
    const GlintCssQualifiedRule* rule = nullptr;
    for (const auto& sheet : mStylesheets)
    {
      if (sheet.sourceUrl != sourceUrl) continue;
      for (const auto& topRule : sheet.rules)
      {
        if (topRule.kind == GlintCssStylesheet::Rule::Kind::QUALIFIED)
        {
          if (topRule.qualified && topRule.qualified->sourceLine == sourceLine)
            { rule = topRule.qualified.get(); break; }
        }
        else if (topRule.kind == GlintCssStylesheet::Rule::Kind::AT && topRule.atRule)
        {
          const std::string& n = topRule.atRule->name;
          if (n == "media" || n == "supports" || n == "layer" || n == "document")
          {
            for (const auto& child : topRule.atRule->children)
            {
              if (child.kind == GlintCssAtRule::ChildRule::Kind::QUALIFIED
                  && child.qualified && child.qualified->sourceLine == sourceLine)
                { rule = child.qualified.get(); break; }
            }
          }
        }
        if (rule) break;
      }
      if (rule) break;
    }
    if (!rule) return;

    // ── 3. Read file ─────────────────────────────────────────────────────
    std::ifstream fin(diskPath, std::ios::binary);
    if (!fin.is_open()) return;
    std::string src((std::istreambuf_iterator<char>(fin)),
                     std::istreambuf_iterator<char>());
    fin.close();

    const bool        crlf = (src.find("\r\n") != std::string::npos);
    const std::string nl   = crlf ? "\r\n" : "\n";

    // Line offset table.
    std::vector<size_t> lineStarts;
    lineStarts.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
      if (src[i] == '\n' && (i + 1) < src.size())
        lineStarts.push_back(i + 1);

    if (sourceLine == 0 || static_cast<size_t>(sourceLine) > lineStarts.size()) return;
    const size_t ruleStartOff = lineStarts[sourceLine - 1];

    // ── 4. Locate opening brace, extract selector text ───────────────────────
    size_t openBrace = std::string::npos;
    {
      bool inStr = false; char strQ = 0;
      for (size_t i = ruleStartOff; i < src.size(); ++i)
      {
        if (inStr) { if (src[i] == '\\') ++i; else if (src[i] == strQ) inStr = false; continue; }
        if (src[i] == '"' || src[i] == '\'') { inStr = true; strQ = src[i]; continue; }
        if (src[i] == '{') { openBrace = i; break; }
      }
    }
    if (openBrace == std::string::npos) return;

    std::string selectorTxt = src.substr(ruleStartOff, openBrace - ruleStartOff);
    while (!selectorTxt.empty() && std::isspace((unsigned char)selectorTxt.back()))
      selectorTxt.pop_back();
    if (selectorTxt.empty())
      selectorTxt = _prelToksToText(rule->prelToks);

    // ── 5. Find closing brace ───────────────────────────────────────────────
    int    depth      = 0;
    size_t ruleEndOff = std::string::npos;
    for (size_t i = openBrace; i < src.size(); ++i)
    {
      const char c = src[i];
      if (c == '/' && i + 1 < src.size() && src[i + 1] == '*')
        { i += 2; while (i + 1 < src.size() && !(src[i] == '*' && src[i+1] == '/')) ++i; i += 1; continue; }
      if (c == '"' || c == '\'')
        { const char q = c; ++i; while (i < src.size() && src[i] != q) { if (src[i] == '\\') ++i; ++i; } continue; }
      if      (c == '{') ++depth;
      else if (c == '}') { --depth; if (depth == 0) { ruleEndOff = i; break; } }
    }
    if (ruleEndOff == std::string::npos) return;

    // Advance past the closing-brace line (including its trailing newline).
    size_t ruleEndLineEnd = ruleEndOff + 1;
    if (ruleEndLineEnd < src.size() && src[ruleEndLineEnd] == '\r') ++ruleEndLineEnd;
    if (ruleEndLineEnd < src.size() && src[ruleEndLineEnd] == '\n') ++ruleEndLineEnd;

    // ── 6. Serialise the rule from AST ────────────────────────────────────────
    std::string newRule;
    newRule += selectorTxt;
    newRule += " {";
    newRule += nl;
    for (const auto& decl : rule->declarations)
    {
      // Declarations currently disabled in the inspector are written as CSS
      // comments so they survive as editable text but have no cascade effect.
      // Use rule->prelude (selector text) instead of sourceLine so the id
      // stays stable when CSS edits shift line numbers in the same file.
      const std::string disabledId = sourceUrl + "|" + rule->prelude
                                     + "|" + decl.property;
      const bool isDisabled = mInspDisabledDecls
                              && mInspDisabledDecls->count(disabledId) > 0;
      if (isDisabled)
      {
        newRule += "  /* ";
        newRule += decl.property;
        newRule += ": ";
        newRule += decl.value;
        if (decl.important) newRule += " !important";
        newRule += "; */";
        newRule += nl;
      }
      else
      {
        newRule += "  ";
        newRule += decl.property;
        newRule += ": ";
        newRule += decl.value;
        if (decl.important) newRule += " !important";
        newRule += ";";
        newRule += nl;
      }
    }
    newRule += "}";
    newRule += nl;

    // ── 7. Write file + bump mtime ──────────────────────────────────────────
    {
      std::string patched;
      patched.reserve(src.size());
      patched += src.substr(0, ruleStartOff);
      patched += newRule;
      patched += src.substr(ruleEndLineEnd);

      std::ofstream fout(diskPath, std::ios::binary | std::ios::trunc);
      if (!fout.is_open()) return;
      fout.write(patched.data(), static_cast<std::streamsize>(patched.size()));
    }
    if (watchIdx != SIZE_MAX)
    {
      std::error_code ec;
      const auto mt = std::filesystem::last_write_time(diskPath, ec);
      if (!ec) mHotWatchedPaths[watchIdx].mtime = mt;
    }
  }

  /**
   * Inspector-only: register the set of disabled declaration ids so
   * _applyCssToElement skips them during cascade computation.
   * Pass nullptr to remove the filter (e.g. when the inspector closes).
   * The set is owned by the caller (InspStylePanel) and must outlive this call.
   */
  void setInspectorDisabledDecls(const std::unordered_set<std::string>* decls)
  {
    mInspDisabledDecls = decls;
  }

  /**
   * Re-run the CSS cascade for a single element and mark it dirty.
   * Used by the inspector after toggling a declaration's enabled state.
   */
  void reapplyCss(glint_element* el)
  {
    if (!el) return;
    _applyCssToElement(el);
    el->setDirty(false);
  }

  /**
   * Walk the entire element tree and reapply CSS to every element.
   * Called after toggling a CSS rule declaration in the inspector so that
   * ALL elements matching the affected selector update, not just the
   * currently inspected one.
   */
  void reapplyAllCss()
  {
    _applyCssToTree(&mCanvas);
  }

  /**
   * Collect every CSS qualified rule (from all loaded stylesheets) whose
   * selector list matches `el`, returning them sorted by descending specificity
   * (then by descending source order to put later rules first among ties).
   *
   * This is the core query used by the inspector's Styles panel to show
   * DevTools-style "selector { ... }" origin blocks.
   *
   * @param el  The element to match against.  Must not be null.
   * @returns   Vector of GlintMatchedCssRule, highest-specificity first.
   */
  std::vector<GlintMatchedCssRule> matchedCssRulesFor(glint_element* el, bool forcePseudoClasses = false)
  {
    if (!el) return {};

    const GlintMatchedCssCacheKey cacheKey{
      el->mId,
      mStylesheetRevision,
      mTreeRevision,
      _selectorMatchSignature(el, forcePseudoClasses),
      forcePseudoClasses
    };

    if (auto it = mMatchedCssRulesCache.find(cacheKey); it != mMatchedCssRulesCache.end())
      return it->second;

    GlintCssDomAdapter adapter(el);
    adapter.forcePseudoClasses = forcePseudoClasses;

    std::vector<GlintMatchedCssRule> result;
    result.reserve(mQualifiedRuleCache.size());
    for (const auto& flatRule : mQualifiedRuleCache)
    {
      if (!flatRule.rule) continue;
      for (const auto& complexSel : flatRule.rule->selectorList.selectors)
      {
        if (!complexSel.matches(adapter)) continue;

        GlintMatchedCssRule matched;
        matched.selectorText = flatRule.selectorText;
        matched.specificity  = complexSel.specificity();
        matched.declarations = flatRule.rule->declarations;
        matched.sourceOrder  = flatRule.sourceOrder;
        matched.sourceUrl    = flatRule.sourceUrl;
        matched.sourceLine   = flatRule.rule->sourceLine;
        result.push_back(std::move(matched));
        break;
      }
    }

    std::sort(result.begin(), result.end(),
      [](const GlintMatchedCssRule& a, const GlintMatchedCssRule& b) {
        if (a.specificity.value() != b.specificity.value())
          return a.specificity.value() > b.specificity.value();
        return a.sourceOrder > b.sourceOrder;
      });

    if (mMatchedCssRulesCache.size() > 512)
      mMatchedCssRulesCache.clear();
    mMatchedCssRulesCache.emplace(cacheKey, result);
    return result;
  }

  /** Human-readable failure message from the most recent loadStylesheet() that returned false. */
  const std::string& lastCSSError() const { return mLastCSSError; }

  /** Per-document network request log — one entry per asset load.
   *  Read a thread-safe snapshot for the inspector Network tab:
   *    auto entries = body->mRoot.networkLog.snapshot(); */
  glint_network_log networkLog;

  /** Human-readable name for this root (e.g. "glint_project", "Component Demos").
   *  Used by the inspector window to build its title bar: "Inspecting <name>".  Optional. */
  std::string name;

  /** When true, OnMouseOver and OnMouseDown will NOT write to glint_debug::hoveredNode /
   *  inspectedNode even when glint_debug::inspectMode is active.
   *  Set this on the inspector's own glint_document so the inspector UI itself is
   *  never treated as an inspection target. */
  bool skipInspectMode = false;

  /** When true, Ctrl+Shift+I / Ctrl+Shift+C fire the inspector shortcuts.
   *  Defaults to true in Debug builds (NDEBUG not defined), false otherwise. */
#ifdef NDEBUG
  bool acceptInspectorShortcut = false;
#else
  bool acceptInspectorShortcut = true;
#endif

  /** Open (open=true) or close (open=false) the inspector window for this root.
   *  Defined in inspector/window.hpp after glint_inspector_window is complete. */
  void showInspector(bool open);

  /** Returns true if the inspector window is currently open for this root.
   *  Defined in inspector/window.hpp after glint_inspector_window is complete. */
  bool isInspectorOpen() const;

  /** Open the inspector and immediately activate element-picker (crosshair) mode.
   *  Defined in inspector/window.hpp after glint_inspector_window is complete. */
  void openInspectorWithPicker();

#if defined(_WIN32) || defined(OS_WIN)
  /** Native HWND for this root's window.  Stamped by glint_window_win32::initRoot().
  *  nullptr when the host bridge owns the native window handle.
   *  Components use this to show Win32 context menus when mpG is unavailable. */
  HWND hwnd = nullptr;
#elif defined(__APPLE__)
  /** macOS window pointer. Stamped by glint_window_mac::_createPanelAndView().
   *  Used by gradient_editor and other components that need screen-coord conversion. */
  glint_window_mac* macWindow = nullptr;
#endif

  /** Device pixel ratio (physical pixels / logical CSS pixels).
   *  Stamped by the platform host (glint_window_win32, glint_view_win32, etc.)
   *  whenever mDpr changes.  Used by backdrop shaders to convert logical element
   *  rects to the physical pixel space that SkImageFilters::RuntimeShader operates in. */
  float devicePixelRatio = 1.f;

  // ── Performance counters (read-only public accessors) ─────────────────────

  /** Smoothed frames-per-second.  Returns 0 if fewer than 2 frames recorded. */
  float getFPS() const
  {
    if (mFrameCount < 2) return 0.f;
    const int n   = mFrameCount;
    // Oldest sample index in the ring buffer.
    const int old = (mFrameHead - n + kFrameBufSize) % kFrameBufSize;
    const int newest = (mFrameHead - 1 + kFrameBufSize) % kFrameBufSize;
    using Dur = std::chrono::duration<float>;
    const float secs = std::chrono::duration_cast<Dur>(
        mFrameTimes[newest] - mFrameTimes[old]).count();
    if (secs <= 0.f) return 0.f;
    return static_cast<float>(n - 1) / secs;
  }

  /** Average frame time in milliseconds over the last N frames.  0 if no data. */
  float getFrameTimeMs() const
  {
    const float fps = getFPS();
    return fps > 0.f ? 1000.f / fps : 0.f;
  }

  /** Total number of Draw() / DrawToCanvas() calls since this root was created. */
  uint64_t getDrawCount() const { return mDrawCount; }

  /** Per-frame FPS values derived from the ring buffer, oldest → newest.
   *  Each element is 1000 / (dt_ms between consecutive recorded frames).
   *  Returns an empty vector if fewer than 2 frames have been recorded.
   *  Safe to call from any thread — reads only atomic-stamped timestamps. */
  std::vector<float> getFrameSamples() const
  {
    if (mFrameCount < 2) return {};
    std::vector<float> result;
    result.reserve(static_cast<size_t>(mFrameCount - 1));
    using Dur = std::chrono::duration<float, std::milli>;
    for (int i = 1; i < mFrameCount; ++i)
    {
      const int prev = (mFrameHead - mFrameCount + i - 1 + kFrameBufSize) % kFrameBufSize;
      const int curr = (mFrameHead - mFrameCount + i     + kFrameBufSize) % kFrameBufSize;
      const float ms = std::chrono::duration_cast<Dur>(
                           mFrameTimes[curr] - mFrameTimes[prev]).count();
      if (ms > 0.f) result.push_back(1000.f / ms);
    }
    return result;
  }

  // ── Focus management ──────────────────────────────────────────────────────────────

  /** Read-only access to the currently focused node (nullptr if none). */
  glint_element* getFocusedNode() const { return mFocusedNode; }

  /** True when the current focus arrived via Tab/Shift+Tab keyboard navigation
   *  (:focus-visible semantics).  False when focus came from a mouse click. */
  bool isFocusViaKeyboard() const { return mFocusViaKeyboard; }

  /**
   * Transfer keyboard focus to `node`.
   * Fires onFocusLost() + DOM "blur" (non-bubbling) on the old node,
   * then onFocusGained() + DOM "focus" (non-bubbling) on the new node.
   * Passing nullptr clears focus entirely.
   */
  void SetFocus(glint_element* node)
  {
    if (mFocusedNode == node) return;

    // Blur old node.
    if (mFocusedNode)
    {
      mFocusedNode->mIsFocused = false;
      // Clear :focus-within on entire old ancestor chain.
      for (glint_element* n = mFocusedNode; n; n = n->mParent)
      {
        n->mIsFocusWithin = false;
        _applyCssToElement(n);
      }
      mFocusedNode->onFocusLost();
      glint_keyboard_event blur;
      blur.type    = "blur";
      blur.bubbles = false;
      mFocusedNode->element._dispatchToListeners(blur);
    }

    mFocusedNode = node;

    // Focus new node.
    if (mFocusedNode)
    {
      mFocusedNode->mIsFocused = true;
      // Set :focus-within on entire new ancestor chain.
      for (glint_element* n = mFocusedNode; n; n = n->mParent)
      {
        n->mIsFocusWithin = true;
        _applyCssToElement(n);
      }
      mFocusedNode->onFocusGained();
      glint_keyboard_event focus;
      focus.type    = "focus";
      focus.bubbles = false;
      mFocusedNode->element._dispatchToListeners(focus);
    }

    // Redraw so the focus ring appears/disappears immediately.
    setDirty(false);
  }

  // ── Keyboard events ─────────────────────────────────────────────────────────────
  // Called by the adapter after converting IKeyPress → glint_key_press.
  // Dispatches a DOM "keydown"/"keyup" event (bubbling) to the focused node,
  // then calls the virtual OnKeyDown/Up on it.
  // Returns true if the event was consumed and should not bubble further.

  bool OnKeyDown(const glint_key_press& key)
  {
    if (onGlobalKeyDown && onGlobalKeyDown(key)) return true;

    // ── Inspector keyboard shortcuts ────────────────────────────────────────
    // Ctrl+Shift+I: toggle inspector.  Ctrl+Shift+C: open + activate picker.
    // Gated on acceptInspectorShortcut (true in debug builds, false in release).
    if (acceptInspectorShortcut && key.ctrl && key.shift)
    {
      if (key.vk == 'I') { showInspector(!isInspectorOpen()); return true; }
      if (key.vk == 'C') { openInspectorWithPicker();               return true; }
    }

    // ── Cross-element selection keyboard shortcuts ─────────────────────────
    // Escape: dismiss an active cross-element selection.
    if (key.vk == 0x1B /* ESC */ && mGlobalSelActive)
    {
      _clearGlobalSelection();
      setDirty(false);
      return true;
    }
    // Ctrl+A: select all globally-selectable text.
    // Yield only to a focused component that explicitly consumes Ctrl+A
    // (i.e. glint_text_editor_base / glint_input via consumesCtrlA()).
    // Any other focused node — labels, tab-focused buttons, etc. — does NOT
    // intercept it, matching Chrome's rule: Ctrl+A always selects page text
    // unless a text-input field has keyboard focus.
    if (key.ctrl && key.vk == 'A' &&
        (!mFocusedNode || mFocusedNode->isGlobalSelectable() || !mFocusedNode->consumesCtrlA()))
    {
      _clearGlobalSelection();
      std::vector<glint_element*> nodes;
      _collectSelectable(mCanvas, nodes);
      if (!nodes.empty())
      {
        mGlobalAnchor    = { nodes.front(), 0 };
        mGlobalFocus     = { nodes.back(), nodes.back()->globalTextLen() };
        mGlobalSelActive = true;
        _applyGlobalSelection();
        setDirty(false);
      }
      return true;
    }
    // Ctrl+C: copy the cross-element selection when it spans multiple nodes.
    if (key.ctrl && key.vk == 'C' && mGlobalSelActive)
    {
      const std::string txt = _getGlobalSelectedText();
      if (!txt.empty()) _setClipboard(txt);
      return true;
    }

    // Tab / Shift+Tab: cycle keyboard focus through focusable nodes.
    // Handled at root level so it works even when no node is focused.
    if (key.vk == 0x09 /* TAB */)
    {
      _focusTraversal(key.shift);
      return true;
    }
    if (!mFocusedNode) return false;
    glint_keyboard_event e;
    e.type      = "keydown";
    e.bubbles   = true;
    e.cancelable = true;
    e.key       = key;
    mFocusedNode->dispatchDOMEvent(e);
    if (e.defaultPrevented) return true;
    return mFocusedNode->OnKeyDown(key);
  }

  bool OnKeyUp(const glint_key_press& key)
  {
    if (!mFocusedNode) return false;
    glint_keyboard_event e;
    e.type      = "keyup";
    e.bubbles   = true;
    e.cancelable = true;
    e.key       = key;
    mFocusedNode->dispatchDOMEvent(e);
    if (e.defaultPrevented) return true;
    return mFocusedNode->OnKeyUp(key);
  }
  // ── Redraw ─────────────────────────────────────────────────────────────────

  /** Trigger a repaint.  Called by components via mRequestRedraw; also called
   *  directly by the inspector overlay (glint_debug::inspectedNode). */
  void setDirty(bool = false, int = glint_no_val_idx)
  {
    // Conservative default: any setDirty caller might have changed something
    // that affects layout, so mark layout dirty too. Callers that know they
    // only need a repaint (e.g. scroll offset changes) should call
    // setPaintOnlyDirty() on their element instead, which goes through
    // mRequestRedraw without flipping mLayoutDirty.
    mLayoutDirty = true;
    if (mRequestRedraw) mRequestRedraw();
  }

  /** Trigger a repaint without marking layout dirty. Use for purely-visual
   *  changes (scroll offsets, etc.). If unsure, call setDirty() instead. */
  void setPaintOnlyDirty()
  {
    if (mRequestRedraw) mRequestRedraw();
  }

  /** Layout-dirty flag. Read by DrawToCanvas/Draw to decide whether to run
   *  the (expensive) Layout() pass; set by setDirty() and by tree-mutation
   *  hooks on glint_element. Cleared after a layout pass completes. Starts
   *  true so the very first frame always lays out. */
  bool mLayoutDirty = true;

  void setDetailedRedrawReporter(std::function<void(glint_element*)> reporter)
  {
    mRequestRedrawDetailed = std::move(reporter);
    mCanvas.mRequestRedrawDetailed = mRequestRedrawDetailed;
  }

  // ── Node registration ──────────────────────────────────────────────────────

  void RegisterTag(int tag, glint_element* node)
  {
    if (tag != glint_no_tag) mTagMap[tag] = node;
  }

  glint_element* GetNodeWithTag(int tag) const
  {
    auto it = mTagMap.find(tag);
    return (it != mTagMap.end()) ? it->second : nullptr;
  }

  // ── Node ID lookup ────────────────────────────────────────────────────

  /** Find the node with the given mId, or nullptr if not found. O(n) tree walk. */
  glint_element* getNodeById(uint64_t id)
  {
    return findById(mCanvas, id);
  }

  /**
   * Debug-only inspector removal. Hides the node from layout/render/hit testing
   * and excludes it from future inspector tree snapshots without destroying it.
   * Returns false for invalid ids or the canvas root.
   */
  bool debugRemoveNodeById(uint64_t id, uint64_t* fallbackSelectionId = nullptr)
  {
    if (fallbackSelectionId) *fallbackSelectionId = 0;
    if (id == 0) return false;

    glint_element* node = getNodeById(id);
    if (!node || node == &mCanvas || !node->mParent) return false;

    if (fallbackSelectionId)
      *fallbackSelectionId = node->mParent->mId;

    auto hasAncestor = [](glint_element* elem, glint_element* ancestor) noexcept {
      for (auto* p = elem; p; p = p->mParent)
        if (p == ancestor) return true;
      return false;
    };

    node->mInspectorRemoved = true;

    if (mHoveredNode   && hasAncestor(mHoveredNode,   node)) mHoveredNode   = nullptr;
    if (mMouseDownNode && hasAncestor(mMouseDownNode, node)) mMouseDownNode = nullptr;
    if (mLastClickNode && hasAncestor(mLastClickNode, node)) mLastClickNode = nullptr;
    if (mFocusedNode   && hasAncestor(mFocusedNode,   node)) SetFocus(nullptr);
    if (mGlobalAnchor.comp && hasAncestor(mGlobalAnchor.comp, node)) { mGlobalAnchor = {}; mGlobalSelActive = false; }
    if (mGlobalFocus.comp  && hasAncestor(mGlobalFocus.comp,  node)) { mGlobalFocus  = {}; mGlobalSelActive = false; }

    if (auto* hovered = glint_debug::hoveredNode.load(); hovered && hovered->mRoot == this && hasAncestor(hovered, node))
      glint_debug::hoveredNode.store(nullptr);
    if (auto* inspected = glint_debug::inspectedNode.load(); inspected && inspected->mRoot == this && hasAncestor(inspected, node))
      glint_debug::inspectedNode.store(nullptr);
    if (auto* pinned = glint_debug::pinnedNode.load(); pinned && pinned->mRoot == this && hasAncestor(pinned, node))
      glint_debug::pinnedNode.store(nullptr);

    mCanvas.setDirty(false);
    glint_bus::publish(glint_tree_changed_event{this});
    return true;
  }

  /**
   * Undo a prior debug-only inspector removal.
   * Returns false when the node does not exist, is the canvas root, or is not removed.
   */
  bool debugRestoreNodeById(uint64_t id)
  {
    if (id == 0) return false;

    glint_element* node = findByIdIncludingRemoved(mCanvas, id);
    if (!node || node == &mCanvas || !node->mInspectorRemoved) return false;

    node->mInspectorRemoved = false;
    mCanvas.setDirty(false);
    glint_bus::publish(glint_tree_changed_event{this});
    return true;
  }

  /**
   * Find the node with the given element.id string, or nullptr if not found.
   * Used internally by glint_element::findMaskSourceElement() for url("#id") mask
   * references. O(n) DFS tree walk.
   */
  glint_element* getElementByStringId(const std::string& strId)
  {
    if (strId.empty()) return nullptr;
    return findByStringId(mCanvas, strId);
  }

  /** Called by glint_element::notifyDestroyed() (via destructor).
   *  Nulls every tracking pointer that equals `node` OR has `node` anywhere in
   *  its ancestor (mParent) chain.  Without the ancestor check, destroying a
   *  container frees its children but leaves mHoveredNode pointing to a valid
   *  leaf whose mParent chain now contains freed nodes — causing a crash the
   *  next time buildChain() walks that chain.
   *
   *  Safety: this is called from ~glint_element() BEFORE the member vectors
   *  (mChildren) are destroyed, so walking mParent from any tracked pointer
   *  back to `node` is guaranteed to touch only live memory. */
  void _onComponentDestroyed(glint_element* node)
  {
    // Returns true if `ancestor` is `elem` itself or any node in elem's mParent chain.
    // All pointers in the chain are alive when called (see safety note above).
    auto hasAncestor = [](glint_element* elem, glint_element* ancestor) noexcept {
      for (auto* p = elem; p; p = p->mParent)
        if (p == ancestor) return true;
      return false;
    };

    if (mHoveredNode   && hasAncestor(mHoveredNode,   node)) mHoveredNode   = nullptr;
    if (mMouseDownNode && hasAncestor(mMouseDownNode,  node)) mMouseDownNode = nullptr;
    if (mFocusedNode   && hasAncestor(mFocusedNode,    node)) mFocusedNode   = nullptr;
    if (mLastClickNode && hasAncestor(mLastClickNode,  node)) mLastClickNode = nullptr;
    if (mGlobalAnchor.comp == node) { mGlobalAnchor = {}; mGlobalSelActive = false; }
    if (mGlobalFocus.comp  == node) { mGlobalFocus  = {}; mGlobalSelActive = false; }
  }

  // ── Tree snapshot export ─────────────────────────────────────────────

  /**
   * Walk the full component tree and return a recursive glint_tree_node
   * snapshot.  The snapshot is a value (not a live reference): it is safe to
   * read on a background thread after being captured on the UI thread.
   *
   * The root of the returned tree is the canvas node.  Each node carries:
   *   id        — the component's stable mId
   *   typeName  — result of typeName() (e.g. "button", "label")
   *   rect      — GetPaintRECT() at the time of the call
   *   styleInfo — full glint_style serialized as a string map
   *   children  — recursive child nodes
   */
  glint_tree_node getUITree() const
  {
    std::lock_guard<std::mutex> lk(mTreeMutex);
    return buildTreeNode(mCanvas);
  }

  struct glint_overlay_point
  {
    float x = 0.f;
    float y = 0.f;
  };

  struct glint_overlay_quad
  {
    std::array<glint_overlay_point, 4> pts{};
  };

  static SkM44 _overlayMatrixFor(glint_element* node)
  {
    SkM44 ctm;
    if (!node) return ctm;

    std::vector<glint_element*> chain;
    for (auto* cur = node; cur; cur = cur->mParent)
      chain.push_back(cur);

    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
    {
      glint_element* cur = *it;
      const glint_rect pr = cur->GetPaintRECT();
      const SkM44 local = cur->computedStyle.ResolveTransformMatrix(pr.W(), pr.H(), pr.MW(), pr.MH());
      if (!(local == SkM44{}))
        ctm = ctm * local;

      if (cur != node && (cur->mScrollLeft != 0.f || cur->mScrollTop != 0.f))
        ctm = ctm * SkM44::Translate(-cur->mScrollLeft, -cur->mScrollTop, 0.f);
    }

    return ctm;
  }

  static glint_overlay_quad _overlayMapRect(const glint_rect& rect, const SkM44& ctm)
  {
    auto mapPoint = [&](float x, float y) -> glint_overlay_point {
      if (ctm == SkM44{})
        return { x, y };

      const SkV4 p = ctm.map(x, y, 0.f, 1.f);
      const float invW = (p.w != 0.f) ? (1.f / p.w) : 1.f;
      return { p.x * invW, p.y * invW };
    };

    glint_overlay_quad q;
    q.pts[0] = mapPoint(rect.L, rect.T);
    q.pts[1] = mapPoint(rect.R, rect.T);
    q.pts[2] = mapPoint(rect.R, rect.B);
    q.pts[3] = mapPoint(rect.L, rect.B);
    return q;
  }

  static glint_rect _overlayBounds(const glint_overlay_quad& q)
  {
    float minX = q.pts[0].x, maxX = q.pts[0].x;
    float minY = q.pts[0].y, maxY = q.pts[0].y;
    for (int i = 1; i < 4; ++i)
    {
      minX = std::min(minX, q.pts[i].x);
      maxX = std::max(maxX, q.pts[i].x);
      minY = std::min(minY, q.pts[i].y);
      maxY = std::max(maxY, q.pts[i].y);
    }
    return glint_rect(minX, minY, maxX, maxY);
  }

  /**
   * Render the full scene graph into a raw Skia canvas (no glint_canvas needed).
   * Used by the inspector window which owns its own CPU SkSurface.
   * Runs Layout(nullptr) before the draw traversal so mRect values are current.
   */
  void DrawToCanvas(SkCanvas& canvas)
  {
    _recordFrame();
    _tickHotReload();

    mCanvas.tickTransitionsAll();
    if (mLayoutDirty)
    {
      mCanvas.Layout(nullptr);
      mLayoutDirty = false;
    }
    mCanvas.DrawToCanvas(&canvas);

    // Phase 3 — inspector highlights (mirrors Draw(glint_canvas&) but using Skia).
    auto* hovered   = glint_debug::hoveredNode.load();
    auto* inspected = glint_debug::inspectedNode.load();

    // Only draw overlays for nodes that belong to this root.
    auto belongsHere = [&](glint_element* c) {
      return c && c->mRoot == this;
    };

    SkPaint paint;
    paint.setAntiAlias(true);

    auto quadPath = [](const glint_overlay_quad& q) {
      SkPath path;
      path.moveTo(q.pts[0].x, q.pts[0].y);
      for (int i = 1; i < 4; ++i)
        path.lineTo(q.pts[i].x, q.pts[i].y);
      path.close();
      return path;
    };

    auto ringPath = [](const glint_overlay_quad& outer, const glint_overlay_quad& inner) {
      SkPath path;
      path.setFillType(SkPathFillType::kWinding);

      path.moveTo(outer.pts[0].x, outer.pts[0].y);
      for (int i = 1; i < 4; ++i)
        path.lineTo(outer.pts[i].x, outer.pts[i].y);
      path.close();

      path.moveTo(inner.pts[3].x, inner.pts[3].y);
      for (int i = 2; i >= 0; --i)
        path.lineTo(inner.pts[i].x, inner.pts[i].y);
      path.close();
      return path;
    };

    auto fillQuad = [&](const glint_overlay_quad& q, SkColor col)
    {
      paint.setStyle(SkPaint::kFill_Style);
      paint.setColor(col);
      canvas.drawPath(quadPath(q), paint);
    };

    auto fillRing = [&](const glint_overlay_quad& outer, const glint_overlay_quad& inner, SkColor col)
    {
      paint.setStyle(SkPaint::kFill_Style);
      paint.setColor(col);
      canvas.drawPath(ringPath(outer, inner), paint);
    };

    auto strokeQuad = [&](const glint_overlay_quad& q, SkColor col, float width)
    {
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(width);
      paint.setColor(col);
      canvas.drawPath(quadPath(q), paint);
    };

    // Draw box model zones for a node. alpha: 60 = hover (faint), 140 = selected (bright).
    auto drawBoxModel = [&](glint_element* node, uint8_t ringAlpha, uint8_t contentAlpha, uint8_t outlineAlpha)
    {
      const glint_rect elem = node->GetPaintRECT();
      const SkM44 ctm = _overlayMatrixFor(node);

      // Derive parent content width for resolving % margins/padding — mirrors the layout pass.
      float parentContentW = 0.f;
      if (node->mParent)
      {
        const glint_rect par = node->mParent->GetPaintRECT();
        const float ppL = (float)node->mParent->computedStyle.paddingLeft;
        const float ppR = (float)node->mParent->computedStyle.paddingRight;
        parentContentW = std::max(0.f, par.W() - ppL - ppR);
      }
      else
      {
        parentContentW = elem.W();
      }

      const float pL = node->computedStyle.paddingLeft.resolve(parentContentW),
                  pR = node->computedStyle.paddingRight.resolve(parentContentW),
                  pT = node->computedStyle.paddingTop.resolve(parentContentW),
                  pB = node->computedStyle.paddingBottom.resolve(parentContentW);
      const float mL = node->computedStyle.marginLeft.resolve(parentContentW),
                  mR = node->computedStyle.marginRight.resolve(parentContentW),
                  mT = node->computedStyle.marginTop.resolve(parentContentW),
                  mB = node->computedStyle.marginBottom.resolve(parentContentW);

      const glint_overlay_quad marginQ  = _overlayMapRect(glint_rect(elem.L - mL, elem.T - mT, elem.R + mR, elem.B + mB), ctm);
      const glint_overlay_quad elemQ    = _overlayMapRect(elem, ctm);
      const glint_overlay_quad contentQ = _overlayMapRect(glint_rect(elem.L + pL, elem.T + pT, elem.R - pR, elem.B - pB), ctm);

      fillRing(marginQ, elemQ,    SkColorSetARGB(ringAlpha,    255, 160,  30));
      fillRing(elemQ,   contentQ, SkColorSetARGB(ringAlpha,     60, 200, 120));
      fillQuad(contentQ, SkColorSetARGB(contentAlpha, 100, 160, 255));
      if (outlineAlpha > 0)
        strokeQuad(elemQ, SkColorSetARGB(outlineAlpha, 0, 220, 220), outlineAlpha == 255 ? 2.f : 1.5f);
    };

    // Hover: zones, no border outline.
    if (belongsHere(hovered))
      drawBoxModel(hovered, /*ring*/120, /*content*/90, /*outline*/0);

    // Eye-pinned highlight: teal outline only (no fill zones — fillRing with
    // alpha=0 zeros pixels in the premultiplied buffer, appearing black).
    {
      auto* pinned = glint_debug::pinnedNode.load();
      if (belongsHere(pinned))
      {
        const glint_overlay_quad pq = _overlayMapRect(pinned->GetPaintRECT(), _overlayMatrixFor(pinned));
        strokeQuad(pq, SkColorSetARGB(220, 0, 210, 170), 2.f);
      }
    }

    // (inspectedNode persistent overlay intentionally omitted.)
    if (false && belongsHere(inspected))
    {
      drawBoxModel(inspected, /*ring*/140, /*content*/100, /*outline*/255);

      const glint_rect elem = _overlayBounds(_overlayMapRect(inspected->GetPaintRECT(), _overlayMatrixFor(inspected)));
      const char* tname = inspected->typeName();
      if (tname && *tname)
      {
        SkFont font = glint_element::skFont(11.f);
        const float textW = static_cast<float>(std::strlen(tname)) * 7.f + 4.f;
        const SkRect bg = SkRect::MakeLTRB(elem.L + 2.f, elem.T + 1.f,
                                            elem.L + 2.f + textW, elem.T + 14.f);
        paint.setStyle(SkPaint::kFill_Style);
        paint.setColor(SkColorSetARGB(200, 0, 0, 0));
        canvas.drawRect(bg, paint);
        paint.setColor(SkColorSetARGB(255, 255, 255, 255));
        canvas.drawString(tname, bg.fLeft + 2.f, bg.fBottom - 3.f, font, paint);
      }
    }

  }

  // ── Drawing ─────────────────────────────────────────────────────────────────

  void Draw(glint_canvas& g)
  {
    _recordFrame();

    _tickHotReload();

    // Pre-pass: tick all transitions BEFORE layout so animated width/height
    // values are visible to childPrefH/W during the layout pass.
    mCanvas.tickTransitionsAll();

    // Phase 1 — layout: re-compute mRect for every node from current style.
    // Skipped on frames where nothing has called setDirty() / markLayoutDirty()
    // since the previous Layout(); paint-only redraws (e.g. scroll) bypass this.
    if (mLayoutDirty)
    {
      mCanvas.Layout(&g);
      mLayoutDirty = false;
    }

    // Phase 2 — draw traversal.
    mCanvas.Draw(g);

    // Phase 3 — inspector highlights (drawn on top of everything).
    // hoveredNode = transient faint blue (tree row hover / inspect-mode cursor).
    // inspectedNode = persistent bright blue (tree row click / inspect-mode click).
    auto* hovered   = glint_debug::hoveredNode.load();
    auto* inspected = glint_debug::inspectedNode.load();

    auto belongsHere = [&](glint_element* node) {
      return node && node->mRoot == this;
    };

    auto fillQuad = [&](const glint_overlay_quad& q, glint_color col)
    {
      float xs[4] = { q.pts[0].x, q.pts[1].x, q.pts[2].x, q.pts[3].x };
      float ys[4] = { q.pts[0].y, q.pts[1].y, q.pts[2].y, q.pts[3].y };
      g.FillConvexPolygon(col, xs, ys, 4);
    };

    auto fillRing = [&](const glint_overlay_quad& outer, const glint_overlay_quad& inner, glint_color col)
    {
      auto addQuadPath = [&](const glint_overlay_quad& q, bool reverse)
      {
        const int start = reverse ? 3 : 0;
        g.PathMoveTo(q.pts[start].x, q.pts[start].y);
        if (reverse)
        {
          for (int i = 2; i >= 0; --i)
            g.PathLineTo(q.pts[i].x, q.pts[i].y);
        }
        else
        {
          for (int i = 1; i < 4; ++i)
            g.PathLineTo(q.pts[i].x, q.pts[i].y);
        }
        g.PathClose();
      };

      g.PathClear();
      g.PathSetWinding(false);
      addQuadPath(outer, false);
      g.PathSetWinding(true);
      addQuadPath(inner, true);
      g.PathFill(col, glint_fill_options(false, EFillRule::Preserve));
    };

    auto strokeQuad = [&](const glint_overlay_quad& q, glint_color col, float width)
    {
      float xs[4] = { q.pts[0].x, q.pts[1].x, q.pts[2].x, q.pts[3].x };
      float ys[4] = { q.pts[0].y, q.pts[1].y, q.pts[2].y, q.pts[3].y };
      g.DrawConvexPolygon(col, xs, ys, 4, nullptr, width);
    };

    // Draw box model zones for a node. Use lower alpha for hover, full for selected.
    auto drawBoxModel = [&](glint_element* node, int ringA, int contentA, int outlineA)
    {
      const glint_rect elem = node->GetPaintRECT();
      const SkM44 ctm = _overlayMatrixFor(node);

      float parentContentW2 = 0.f;
      if (node->mParent)
      {
        const glint_rect par2 = node->mParent->GetPaintRECT();
        const float ppL2 = (float)node->mParent->computedStyle.paddingLeft;
        const float ppR2 = (float)node->mParent->computedStyle.paddingRight;
        parentContentW2 = std::max(0.f, par2.W() - ppL2 - ppR2);
      }
      else
      {
        parentContentW2 = elem.W();
      }

      const float pL = node->computedStyle.paddingLeft.resolve(parentContentW2),
                  pR = node->computedStyle.paddingRight.resolve(parentContentW2),
                  pT = node->computedStyle.paddingTop.resolve(parentContentW2),
                  pB = node->computedStyle.paddingBottom.resolve(parentContentW2);
      const float mL = node->computedStyle.marginLeft.resolve(parentContentW2),
                  mR = node->computedStyle.marginRight.resolve(parentContentW2),
                  mT = node->computedStyle.marginTop.resolve(parentContentW2),
                  mB = node->computedStyle.marginBottom.resolve(parentContentW2);

      const glint_overlay_quad marginQ  = _overlayMapRect(glint_rect(elem.L - mL, elem.T - mT, elem.R + mR, elem.B + mB), ctm);
      const glint_overlay_quad elemQ    = _overlayMapRect(elem, ctm);
      const glint_overlay_quad contentQ = _overlayMapRect(glint_rect(elem.L + pL, elem.T + pT, elem.R - pR, elem.B - pB), ctm);
      fillRing(marginQ, elemQ,    glint_color(ringA,    255, 160,  30));
      fillRing(elemQ,   contentQ, glint_color(ringA,     60, 200, 120));
      fillQuad(contentQ, glint_color(contentA, 100, 160, 255));
      if (outlineA > 0)
        strokeQuad(elemQ, glint_color(outlineA, 0, 220, 220), outlineA == 255 ? 2.f : 1.5f);
      return _overlayBounds(elemQ);
    };

    // Hover: zones, no border outline.
    if (belongsHere(hovered))
      drawBoxModel(hovered, /*ring*/120, /*content*/90, /*outline*/0);

    // (inspectedNode persistent overlay intentionally omitted.)
    if (false && belongsHere(inspected))
    {
      const glint_rect elem = drawBoxModel(inspected, /*ring*/140, /*content*/100, /*outline*/255);
      const char* tname = inspected->typeName();
      if (tname && *tname)
      {
        const glint_rect lr(elem.L + 2.f, elem.T + 1.f,
                       elem.L + 2.f + static_cast<float>(std::strlen(tname)) * 7.f + 4.f,
                       elem.T + 14.f);
        g.FillRect(glint_color(200, 0, 0, 0), lr);
        g.DrawText(glint_text(11.f, glint_color(255,255,255,255), "Roboto-Regular",
                         EAlign::Near, EVAlign::Top), tname, lr);
      }
    }

    // Eye-pinned highlight: teal outline only (no fill zones — fillRing with
    // alpha=0 zeros pixels in the premultiplied buffer, appearing black).
    if (auto* pinned = glint_debug::pinnedNode.load(); pinned && pinned->mRoot == this)
    {
      const glint_overlay_quad pq = _overlayMapRect(pinned->GetPaintRECT(), _overlayMatrixFor(pinned));
      strokeQuad(pq, glint_color(220, 0, 210, 170), 2.f);
    }

  }

  // ── Mouse events ─────────────────────────────────────────────────────────
  // Called by the adapter after converting host-specific types to glint_mouse_mod.

  void OnMouseDown(float x, float y, const glint_mouse_mod& mod)
  {
    auto* hit = hitTest(x, y);
    // Inspect mode: clicking in the main UI selects that component persistently,
    // then immediately exits inspect mode (like Chrome DevTools).
    if (!skipInspectMode && glint_debug::inspectMode.load() && hit)
    {
      glint_debug::inspectedNode.store(hit);
      glint_debug::inspectMode.store(false);   // exit inspect mode on click
      glint_debug::hoveredNode.store(nullptr);  // clear transient hover highlight
      glint_bus::publish(glint_selected_node_changed_event{this, hit->mId});
      setDirty(false);
    }
    // Focus management: clicking a focusable node gives it focus;
    // clicking anything else (including nothing) blurs the current focused node.
    // Mouse clicks suppress the focus ring (:focus-visible semantics — ring only shows for keyboard focus).
    mFocusViaKeyboard = false;
    if (hit && hit->mAcceptsFocus)
      SetFocus(hit);
    else if (mFocusedNode)
      SetFocus(nullptr);

    // ── Cross-element selection ──────────────────────────────────────────────
    // Shift+Click on a *different* selectable component: extend cross-el sel.
    if (mod.S && mGlobalAnchor.comp && mGlobalAnchor.comp->mRoot == this &&
        hit && hit != mGlobalAnchor.comp && hit->isGlobalSelectable())
    {
      float gx = x, gy = y;
      scrollAdjusted(hit, gx, gy);
      mGlobalFocus     = { hit, hit->globalHitTestPos(gx, gy) };
      mGlobalSelActive = true;
      _applyGlobalSelection();
      mMouseDownNode = hit;
      setDirty(false);
      return;
    }
    // Normal click: clear prior cross-el selection; record new anchor.
    _clearGlobalSelection();
    if (hit && hit->isGlobalSelectable())
    {
      float gx = x, gy = y;
      scrollAdjusted(hit, gx, gy);
      mGlobalAnchor = { hit, hit->globalHitTestPos(gx, gy) };
    }

    mMouseDownNode = hit;
    if (hit)
    {
      // Set :active on the hit element AND every ancestor — CSS spec: :active
      // propagates up the ancestor chain so parent rules like
      // `div:active > button` and `section:active` match correctly when a
      // child is pressed.
      for (glint_element* n = hit; n; n = n->mParent)
        n->mIsActive = true;
      _reapplyCssChain(hit);
      float ex = x, ey = y;
      scrollAdjusted(hit, ex, ey);
      auto e = makeME("mousedown", ex, ey, mod, 0.f, 0.f, /*bubbles=*/true);
      hit->dispatchDOMEvent(e);
      hit->OnMouseDown(ex, ey, mod);
      for (auto& [_sid, _s] : hit->shaders)
        _s->onMouseDown(ex - hit->mPaintRECT.L, ey - hit->mPaintRECT.T);
    }
    setDirty(false);
  }

  void OnMouseUp(float x, float y, const glint_mouse_mod& mod)
  {
    if (mMouseDownNode)
    {
      // Clear :active before any events fire so cssStyle_ is current when
      // event handlers (e.g. glint_button::OnMouseUp → _startStateTransition) run.
      // Clear on the element AND all ancestors (mirrors the set in OnMouseDown).
      for (glint_element* n = mMouseDownNode; n; n = n->mParent)
        n->mIsActive = false;
      _reapplyCssChain(mMouseDownNode);

      float ex = x, ey = y;
      scrollAdjusted(mMouseDownNode, ex, ey);
      auto eu = makeME("mouseup", ex, ey, mod, 0.f, 0.f, /*bubbles=*/true);
      mMouseDownNode->dispatchDOMEvent(eu);
      mMouseDownNode->OnMouseUp(ex, ey, mod);

      glint_element* upNode = hitTest(x, y);
      if (upNode == mMouseDownNode)
      {
        auto ec = makeME("click", ex, ey, mod, 0.f, 0.f, /*bubbles=*/true);
        mMouseDownNode->dispatchDOMEvent(ec);

        // dblclick — browser sequence: mousedown→mouseup→click→…→click→dblclick
        // DOM spec §17.1: dblclick fires on the nearest common ancestor of the
        // two click targets, so composite elements (e.g. a knob whose child
        // input and knob body are separate hit-test nodes) work correctly.
        using Clock = std::chrono::steady_clock;
        auto now = Clock::now();
#if defined(OS_WIN) || defined(_WIN32)
        const int dblMs = static_cast<int>(::GetDoubleClickTime());
#else
        const int dblMs = 500;   // system default
#endif
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - mLastClickTime).count();

        if (mLastClickNode && elapsedMs <= dblMs)
        {
          glint_element* dblTarget = _nearestCommonAncestor(mLastClickNode, mMouseDownNode);
          if (dblTarget)
          {
            auto ed = makeME("dblclick", ex, ey, mod, 0.f, 0.f, /*bubbles=*/true);
            dblTarget->dispatchDOMEvent(ed);
          }
          mLastClickNode = nullptr;
          mLastClickTime = Clock::time_point{};
        }
        else
        {
          mLastClickNode = mMouseDownNode;
          mLastClickTime = now;
        }
      }

      mMouseDownNode = nullptr;
    }
    setDirty(false);
  }

  void OnMouseDrag(float x, float y, float dX, float dY, const glint_mouse_mod& mod)
  {
    if (mMouseDownNode)
    {
      // ── Cross-element selection ────────────────────────────────────────────
      // When the drag originated on a globally-selectable node, update the
      // focus point.  Activate global mode when we leave the anchor component.
      if (mGlobalAnchor.comp)
      {
        auto* curHit = hitTest(x, y);

        glint_element* focusComp = nullptr;
        int focusByte = 0;

        if (curHit && curHit->isGlobalSelectable())
        {
          float gx = x, gy = y;
          scrollAdjusted(curHit, gx, gy);
          focusComp = curHit;
          focusByte = curHit->globalHitTestPos(gx, gy);
        }
        else
        {
          // Over a non-selectable gap — snap to the spatially nearest label.
          focusComp = _nearestSelectable(x, y);
          if (focusComp)
          {
            const glint_rect& r = focusComp->GetPaintRECT();
            // Convert content-space rect centre to screen-space for comparison.
            float screenCY = (r.T + r.B) * 0.5f;
            for (const glint_element* p = focusComp->mParent; p; p = p->mParent)
              screenCY -= p->mScrollTop;
            focusByte = (y < screenCY) ? 0 : focusComp->globalTextLen();
          }
        }

        if (focusComp)
        {
          mGlobalFocus = { focusComp, focusByte };
          if (focusComp != mGlobalAnchor.comp)
            mGlobalSelActive = true;

          if (mGlobalSelActive)
          {
            _applyGlobalSelection();
            setDirty(false);
            return;   // Bypass per-component drag in global mode.
          }
        }
      }

      // Normal (single-component) drag.
      float ex = x, ey = y;
      scrollAdjusted(mMouseDownNode, ex, ey);
      auto e = makeME("mousemove", ex, ey, mod, dX, dY, /*bubbles=*/true);
      mMouseDownNode->dispatchDOMEvent(e);
      mMouseDownNode->OnMouseDrag(ex, ey, dX, dY, mod);
      // Use paint-only redraw by default: elements that actually need a full
      // layout pass (text selection, sliders, etc.) call setDirty(false) on
      // themselves inside their OnMouseDrag override. Avoiding an unconditional
      // layout-dirty here prevents a full Layout() pass on every drag event,
      // which is the primary cause of drag lag when the element tree is large.
      setPaintOnlyDirty();
    }
  }

  void OnMouseOver(float x, float y, const glint_mouse_mod& mod, float dX = 0.f, float dY = 0.f)
  {
    auto* hit = hitTest(x, y);

    // Inspect mode: update the transient hover highlight and notify the inspector
    // so it can scroll the tree.  Uses hoveredNode (faint), not inspectedNode.
    if (!skipInspectMode && glint_debug::inspectMode.load())
    {
      if (hit != glint_debug::hoveredNode.load())
      {
        glint_debug::hoveredNode.store(hit);
        glint_bus::publish(glint_hovered_node_changed_event{this, hit ? hit->mId : 0u});
        setDirty(false);
      }
    }

    if (hit != mHoveredNode)
    {
      // Browser-spec mouseenter/mouseleave:
      // Build ancestor chains (index 0 = deepest target, increasing toward root).
      // Fire mouseleave only on nodes that are leaving (old chain \ new chain).
      // Fire mouseenter only on nodes that are entering (new chain \ old chain).
      // mouseout/mouseover still fire only on the direct targets and bubble normally.

      auto buildChain = [](glint_element* node) -> std::vector<glint_element*> {
        std::vector<glint_element*> chain;
        for (; node; node = node->mParent) chain.push_back(node);
        return chain;
      };
      auto inChain = [](const std::vector<glint_element*>& chain, glint_element* n) {
        return std::find(chain.begin(), chain.end(), n) != chain.end();
      };

      const auto oldChain = buildChain(mHoveredNode);
      const auto newChain = buildChain(hit);

      // ── Pass 1: update pseudo-class flags + re-cascade CSS ────────────────
      // Must happen before any events fire so that OnMouseOver() / OnMouseOut()
      // overrides (e.g. glint_button::_startStateTransition) read current cssStyle_.

      // Clear :hover on nodes leaving the chain.
      for (auto* node : oldChain)
        if (!inChain(newChain, node))
        {
          node->mIsHovered = false;
          _applyCssToElement(node);
        }
      // Set :hover on nodes entering the chain (shallowest-first so ancestors cascade before children).
      {
        std::vector<glint_element*> toEnter;
        for (auto* node : newChain)
          if (!inChain(oldChain, node)) toEnter.push_back(node);
        for (int i = static_cast<int>(toEnter.size()) - 1; i >= 0; --i)
        {
          toEnter[i]->mIsHovered = true;
          _applyCssToElement(toEnter[i]);
        }
      }

      // ── Pass 2: dispatch DOM events ───────────────────────────────────────

      // 1. mouseout on old target (bubbles up the tree).
      if (mHoveredNode)
      {
        float ox = x, oy = y;
        scrollAdjusted(mHoveredNode, ox, oy);
        auto eo = makeME("mouseout", ox, oy, mod, 0.f, 0.f, /*bubbles=*/true);
        mHoveredNode->dispatchDOMEvent(eo);
        mHoveredNode->OnMouseOut();
      }

      // 2. mouseleave on every node that is no longer hovered (deepest first).
      for (auto* node : oldChain)
      {
        if (!inChain(newChain, node))
        {
          float ox = x, oy = y;
          scrollAdjusted(node, ox, oy);
          auto el = makeME("mouseleave", ox, oy, mod, 0.f, 0.f, /*bubbles=*/false);
          node->dispatchDOMEvent(el);
        }
      }

      mHoveredNode = hit;

      // 3. mouseover on new target (bubbles up the tree).
      if (hit)
      {
        float ex = x, ey = y;
        scrollAdjusted(hit, ex, ey);
        auto eo2 = makeME("mouseover", ex, ey, mod, 0.f, 0.f, /*bubbles=*/true);
        hit->dispatchDOMEvent(eo2);
        hit->OnMouseOver(ex, ey, mod);
      }

      // 4. mouseenter on every node that is newly hovered (shallowest first).
      {
        std::vector<glint_element*> toEnter;
        for (auto* node : newChain)
          if (!inChain(oldChain, node)) toEnter.push_back(node);
        // newChain is deepest-first; fire shallowest-first.
        for (int i = static_cast<int>(toEnter.size()) - 1; i >= 0; --i)
        {
          float ex = x, ey = y;
          scrollAdjusted(toEnter[i], ex, ey);
          auto oe = makeME("mouseenter", ex, ey, mod, 0.f, 0.f, /*bubbles=*/false);
          toEnter[i]->dispatchDOMEvent(oe);
        }
      }

      setDirty(false);
    }

    // Browser-spec mousemove: fire on every pointer move over the current hit
    // target (not just during drags), bubbling through ancestors.
    if (hit)
    {
      float ex = x, ey = y;
      scrollAdjusted(hit, ex, ey);
      auto em = makeME("mousemove", ex, ey, mod, dX, dY, /*bubbles=*/true);
      hit->dispatchDOMEvent(em);
    }
  }

  void OnMouseOut()
  {
    if (mHoveredNode)
    {
      glint_mouse_mod mod{};

      // Pass 1: clear flags + re-cascade CSS before any events fire.
      for (glint_element* node = mHoveredNode; node; node = node->mParent)
      {
        node->mIsHovered = false;
        _applyCssToElement(node);
      }

      // Pass 2: dispatch events.
      // Snapshot the ancestor chain BEFORE dispatching any events — a mouseleave
      // listener may trigger a rebuild that frees nodes in the chain, making the
      // live `node = node->mParent` walk UB on the next iteration.
      std::vector<glint_element*> leaveChain;
      for (glint_element* n = mHoveredNode; n; n = n->mParent) leaveChain.push_back(n);

      // mouseout on the deepest target (bubbles).
      auto eo = makeME("mouseout", 0.f, 0.f, mod, 0.f, 0.f, /*bubbles=*/true);
      mHoveredNode->dispatchDOMEvent(eo);
      mHoveredNode->OnMouseOut();
      // mouseleave up the entire ancestor chain (deepest first, no bubble).
      // Iterate the snapshot; after each dispatch this element may be freed.
      for (auto* node : leaveChain)
      {
        auto el = makeME("mouseleave", 0.f, 0.f, mod, 0.f, 0.f, /*bubbles=*/false);
        node->dispatchDOMEvent(el);
      }
      mHoveredNode = nullptr;
      if (glint_debug::inspectMode.load())
        glint_debug::hoveredNode.store(nullptr);
      glint_bus::publish(glint_hovered_node_changed_event{this, 0u});
      setDirty(false);
    }
  }

  /**
   * Mouse-wheel event.  Called by the adapter after converting the host wheel
   * delta.  Dispatches a DOM "wheel" event (bubbling) starting at the hit node;
   * if not preventDefault()'d, scrolls the nearest scrollable ancestor.
   *
   * @param deltaX  horizontal scroll in pixels (positive = scroll right)
   * @param deltaY  vertical   scroll in pixels (positive = scroll down)
   */
  void OnMouseWheel(float x, float y, float deltaX, float deltaY, const glint_mouse_mod& mod)
  {
    auto* hit = hitTest(x, y);

    // Dispatch "wheel" DOM event first — listener can call preventDefault() to
    // prevent the default scroll action.
    glint_wheel_event we;
    we.type      = "wheel";
    we.bubbles   = true;
    we.cancelable = true;
    we.clientX   = x;
    we.clientY   = y;
    we.deltaX    = deltaX;
    we.deltaY    = deltaY;
    we.deltaMode = 0;   // DOM_DELTA_PIXEL
    we.shiftKey  = mod.S;
    we.ctrlKey   = mod.C;
    we.altKey    = mod.A;
    if (hit) hit->dispatchDOMEvent(we);

    if (we.defaultPrevented)
    {
      setDirty(false);
      return;
    }

    // Walk up the tree to find the nearest scrollable ancestor and scroll it.
    glint_element* node = hit;
    while (node)
    {
      bool consumed = false;

      if (deltaY != 0.f &&
          (node->computedStyle.overflowY == "scroll" || node->computedStyle.overflowY == "auto"))
      {
        const float sbW   = node->style.scrollbarWidth;
        const bool  hasSH = node->mScrollbarH && node->mScrollbarH->style.display != "none";
        const float viewH = node->GetPaintRECT().H() - (hasSH ? sbW : 0.f);
        const float maxY  = std::max(0.f, node->mScrollHeight - viewH);
        if (maxY > 0.f)
        {
          node->mScrollTop = std::max(0.f, std::min(node->mScrollTop + deltaY, maxY));
          consumed = true;
        }
      }

      if (deltaX != 0.f &&
          (node->computedStyle.overflowX == "scroll" || node->computedStyle.overflowX == "auto"))
      {
        const float sbW   = node->style.scrollbarWidth;
        const bool  hasSV = node->mScrollbarV && node->mScrollbarV->style.display != "none";
        const float viewW = node->GetPaintRECT().W() - (hasSV ? sbW : 0.f);
        const float maxX  = std::max(0.f, node->mScrollWidth - viewW);
        if (maxX > 0.f)
        {
          node->mScrollLeft = std::max(0.f, std::min(node->mScrollLeft + deltaX, maxX));
          consumed = true;
        }
      }

      if (consumed)
      {
        // Scroll changes mScrollTop/mScrollLeft; the scrollbar thumb position
        // is resolved during Layout(), so we must mark the document
        // layout-dirty (not just paint-dirty) for the thumb to track. This
        // runs at input rate, not frame rate, so it's cheap.
        node->setDirty(false);
        break;
      }

      node = node->mParent;
    }

    setDirty(false);
  }

  void NotifyTreeStructureChanged()
  {
    _onTreeStructureChanged();
  }

  /**
   * Returns the effective CSS cursor string for the element under (x, y).
   * Walks up the ancestor chain from the hit element until a non-empty cursor
   * is found, matching Chrome's inheritance behaviour.  Returns "default" if
   * no element in the chain specifies a cursor.
   */
  std::string getCursorAtPoint(float x, float y) const
  {
    const glint_element* hit = hitTest(x, y);
    for (const glint_element* e = hit; e; e = e->mParent)
    {
      const std::string& c = e->computedStyle.cursor;
      if (!c.empty() && c != "auto")
        return c;
    }
    return "default";
  }

private:
  // Apply the CSS cascade to the dedicated cssStyle_ layer on the element.
  // el->style (inline) is never touched — the merge happens per-frame in
  // glint_element::_mergedStyle(), which factors both layers into computedStyle.
  // When mInspDisabledDecls is set (inspector active), winning declarations
  // whose composite id "url|line|property" appears in the set are skipped so
  // the inspector checkbox can silence a rule without mutating the stylesheet.
  void _applyCssToElement(glint_element* el)
  {
    if (!el) return;
    GlintCssDomAdapter adapter(el);
    std::vector<const GlintCssStylesheet*> sheets;
    sheets.reserve(mStylesheets.size());
    for (const auto& s : mStylesheets) sheets.push_back(&s);
    const std::vector<const GlintCssStylesheet*> uaSheets{ &mUaSheet };

    // When the inspector is active (mInspDisabledDecls != nullptr), always use
    // resolveSkipping — even if the set is currently empty.  An empty set means
    // "nothing is user-disabled", but we must still NOT apply the decl.disabled
    // AST filter (computeDeclarations step 5) so that file-commented declarations
    // re-enabled via the inspector checkbox immediately participate in the cascade.
    if (mInspDisabledDecls)
    {
      // Use resolveSkipping: disabled entries are skipped DURING reduction so
      // the next-best non-disabled declaration for the same property wins.
      const auto decls = GlintCssCascade::resolveSkipping(adapter, sheets, {}, *mInspDisabledDecls, uaSheets);
      glint_style cssStyle;
      GlintCssApply::apply(decls, cssStyle);
      el->setCssStyleLayer(cssStyle);
      // Collect !important winners so _mergedStyle() can suppress inline overrides.
      el->mCssImportantProps_.clear();
      for (const auto& d : decls)
        if (d.important) el->mCssImportantProps_.insert(d.property);
    }
    else
    {
      // Use computeDeclarations directly so we can read the important flag on each winner.
      const auto winning = GlintCssCascade::computeDeclarations(adapter, sheets, {}, uaSheets);
      std::vector<GlintCssDeclaration> decls;
      decls.reserve(winning.size());
      el->mCssImportantProps_.clear();
      for (const auto& kv : winning)
      {
        decls.push_back(kv.second.decl);
        if (kv.second.decl.important) el->mCssImportantProps_.insert(kv.first);
      }
      glint_style cssStyle;
      GlintCssApply::apply(decls, cssStyle);
      el->setCssStyleLayer(cssStyle);
    }
  }

  /**
   * Patch a single declaration in the on-disk CSS file that backs `sourceUrl`.
   *
   * Called by updateCssDeclaration / removeCssDeclaration after the in-memory
   * AST has been updated so that the change is persisted across the process
   * lifetime and visible in the text editor immediately.
   *
   * After writing the file, the stored mtime for the HotWatchEntry is bumped
   * to the new on-disk value so the hot-reload poller does not immediately
   * re-read the file and re-cascade (which would be a no-op but wastes CPU).
   *
   * Algorithm:
   *   1. Resolve the real disk path from mHotWatchedPaths via sourceUrl.
   *   2. Read the file as text.
   *   3. Scan from the line at sourceLine-1 (0-based) tracking brace depth
   *      to locate the rule's closing `}`.
   *   4. Within the rule block, find the declaration line for `prop`.
   *   5a. remove==true  → delete the entire line.
   *   5b. line found    → replace the value in-place, preserving indentation.
   *   5c. line missing  → insert `  prop: newVal;\n` before the closing `}`.
   *   6. Write the patched string back to disk.
   *   7. Refresh the stored mtime.
   *
   * @param sourceUrl   Sheet sourceUrl (used to look up HotWatchEntry::path).
   * @param sourceLine  1-based line number of the rule opener in the file.
   * @param prop        Lower-case CSS property name.
   * @param newVal      New value string (ignored when remove == true).
   * @param remove      When true the declaration line is deleted entirely.
   */
  void _patchCssFile(const std::string& sourceUrl,
                     uint32_t           sourceLine,
                     const std::string& prop,
                     const std::string& newVal,
                     bool               remove = false)
  {
    // ── 1. Resolve disk path ────────────────────────────────────────────────
    std::string diskPath;
    size_t      watchIdx = SIZE_MAX;
    for (size_t i = 0; i < mHotWatchedPaths.size(); ++i)
    {
      if (mHotWatchedPaths[i].sourceUrl == sourceUrl)
      {
        diskPath = mHotWatchedPaths[i].path;
        watchIdx = i;
        break;
      }
    }
    if (diskPath.empty()) return;   // sheet has no file backing — nothing to write

    // ── 2. Read file ────────────────────────────────────────────────────────
    std::ifstream fin(diskPath, std::ios::binary);
    if (!fin.is_open()) return;
    std::string src((std::istreambuf_iterator<char>(fin)),
                     std::istreambuf_iterator<char>());
    fin.close();

    // ── 3. Build a line-offset table ─────────────────────────────────────
    // lineStarts[i] = byte offset of line i (0-based index).
    std::vector<size_t> lineStarts;
    lineStarts.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
      if (src[i] == '\n' && (i + 1) < src.size())
        lineStarts.push_back(i + 1);

    if (sourceLine == 0 || static_cast<size_t>(sourceLine) > lineStarts.size()) return;
    const size_t ruleStartOff = lineStarts[sourceLine - 1];

    // ── 4. Find the rule's closing `}` ────────────────────────────────────
    // Start scanning from ruleStartOff; the first `{` opens the rule block.
    // Depth returns to 0 at the matching `}`.  Quoted strings and /* */
    // comments are skipped so braces inside them are not counted.
    int    depth      = 0;
    bool   inRule     = false;
    size_t ruleEndOff = std::string::npos;
    for (size_t i = ruleStartOff; i < src.size(); ++i)
    {
      const char c = src[i];
      // Skip block comments /* ... */
      if (c == '/' && i + 1 < src.size() && src[i + 1] == '*')
      {
        i += 2;
        while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) ++i;
        i += 1;  // step over '/' on next loop increment
        continue;
      }
      // Skip quoted strings
      if (c == '"' || c == '\'')
      {
        const char q = c; ++i;
        while (i < src.size() && src[i] != q) { if (src[i] == '\\') ++i; ++i; }
        continue;
      }
      if      (c == '{') { ++depth; inRule = true; }
      else if (c == '}') { --depth; if (inRule && depth == 0) { ruleEndOff = i; break; } }
    }
    if (ruleEndOff == std::string::npos) return;   // malformed CSS

    // ── 5. Locate the declaration line within the rule block ─────────────
    size_t declLineStart = std::string::npos;
    size_t declLineEnd   = std::string::npos;
    for (size_t ls : lineStarts)
    {
      if (ls <= ruleStartOff || ls >= ruleEndOff) continue;
      // Line end: byte after the newline (or end-of-file).
      size_t le = src.find('\n', ls);
      le = (le == std::string::npos) ? src.size() : le + 1;

      // Skip leading whitespace to reach the property name.
      size_t p = ls;
      while (p < le && std::isspace((unsigned char)src[p])) ++p;

      // Check whether src[p..] starts with `prop` (case-insensitive).
      if (p + prop.size() > src.size()) continue;
      bool match = true;
      for (size_t j = 0; j < prop.size(); ++j)
        if (std::tolower((unsigned char)src[p + j]) != (unsigned char)prop[j])
          { match = false; break; }
      if (!match) continue;

      // After the name, require optional whitespace then `:`.
      size_t q2 = p + prop.size();
      while (q2 < le && std::isspace((unsigned char)src[q2])) ++q2;
      if (q2 >= le || src[q2] != ':') continue;

      declLineStart = ls;
      declLineEnd   = le;
      break;
    }

    // ── 6. Build patched string ───────────────────────────────────────────
    std::string patched;
    if (remove)
    {
      if (declLineStart == std::string::npos) return;   // nothing to remove
      patched.reserve(src.size());
      patched += src.substr(0, declLineStart);
      patched += src.substr(declLineEnd);               // drop the entire line
    }
    else if (declLineStart != std::string::npos)
    {
      // Replace in-place, preserving the original indentation.
      size_t indentEnd = declLineStart;
      while (indentEnd < declLineEnd &&
             std::isspace((unsigned char)src[indentEnd]) &&
             src[indentEnd] != '\n')
        ++indentEnd;
      const std::string indent = src.substr(declLineStart, indentEnd - declLineStart);
      const bool crlf = (declLineEnd >= 2 && src[declLineEnd - 2] == '\r');
      patched.reserve(src.size());
      patched += src.substr(0, declLineStart);
      patched += indent;
      patched += prop;
      patched += ": ";
      patched += newVal;
      patched += ";";
      patched += (crlf ? "\r\n" : "\n");
      patched += src.substr(declLineEnd);
    }
    else
    {
      // Property not in rule yet — insert before the closing `}`.
      // Walk back from ruleEndOff to find the start of the closing-brace line.
      size_t closingLineStart = ruleEndOff;
      while (closingLineStart > 0 && src[closingLineStart - 1] != '\n')
        --closingLineStart;
      patched.reserve(src.size() + prop.size() + newVal.size() + 8);
      patched += src.substr(0, closingLineStart);
      patched += "  ";
      patched += prop;
      patched += ": ";
      patched += newVal;
      patched += ";\n";
      patched += src.substr(closingLineStart);
    }

    // ── 7. Write to disk ─────────────────────────────────────────────────
    {
      std::ofstream fout(diskPath, std::ios::binary | std::ios::trunc);
      if (!fout.is_open()) return;
      fout.write(patched.data(), static_cast<std::streamsize>(patched.size()));
    }

    // ── 8. Refresh stored mtime so hot-reload poller ignores this write ──
    if (watchIdx != SIZE_MAX)
    {
      std::error_code ec;
      const auto newMtime = std::filesystem::last_write_time(diskPath, ec);
      if (!ec) mHotWatchedPaths[watchIdx].mtime = newMtime;
    }
  }

  // Poll watched stylesheet files for changes and reload any that have been
  // modified on disk since the last check. Called from Draw() every frame when
  // explicitly enabled; actual file-stat checks are throttled to
  // kHotReloadIntervalMs.
  void _tickHotReload()
  {
    if (!mStylesheetHotReloadEnabled || mHotWatchedPaths.empty()) return;

    // Keep Draw() being called continuously only when stylesheet polling was
    // explicitly requested. This avoids pinning normal windows to a permanent
    // full-frame layout/render loop just because they loaded CSS from disk.
    mRequestRedraw();

    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(
          now - mLastHotReloadCheck).count() < kHotReloadIntervalMs)
      return;
    mLastHotReloadCheck = now;

    for (size_t i = 0; i < mHotWatchedPaths.size(); ++i)
    {
      std::error_code ec;
      const auto mtime = std::filesystem::last_write_time(mHotWatchedPaths[i].path, ec);
      if (ec || mtime == mHotWatchedPaths[i].mtime) continue;

      // Capture strings before any vector mutation below.
      const std::string originalUrl = mHotWatchedPaths[i].originalUrl;
      const std::string sourceUrl   = mHotWatchedPaths[i].sourceUrl;

      // Remove the old parsed sheet so the re-load appends a fresh one.
      mStylesheets.erase(
        std::remove_if(mStylesheets.begin(), mStylesheets.end(),
          [&sourceUrl](const GlintCssStylesheet& s){ return s.sourceUrl == sourceUrl; }),
        mStylesheets.end());

      // Re-load via the original URL so onRequest is invoked correctly.
      // This also re-cascades the tree and updates mHotWatchedPaths with the
      // new mtime, potentially invalidating iterators.
      loadStylesheet(originalUrl);

      // Restart scanning next tick; mHotWatchedPaths may have been reallocated.
      break;
    }
  }

  // Recursively apply the cascade to every element in the subtree.
  void _applyCssToTree(glint_element* el)
  {
    if (!el) return;
    _applyCssToElement(el);
    for (auto& child : el->mChildren)
      _applyCssToTree(child.get());
  }

  // Re-cascade el and every ancestor so that ancestor selectors like
  // "div:hover > child" are re-evaluated when pseudo-class state changes.
  void _reapplyCssChain(glint_element* el)
  {
    for (glint_element* n = el; n; n = n->mParent)
      _applyCssToElement(n);
  }

  // Inspector disabled-declaration filter — null when inspector is inactive.
  const std::unordered_set<std::string>* mInspDisabledDecls = nullptr;

  // ── CSS stylesheets loaded via loadStylesheet() ──────────────────────────────
  GlintCssStylesheet              mUaSheet;      // mutable in-memory copy of the UA sheet
  std::vector<GlintCssStylesheet> mStylesheets;
  std::string                    mLastCSSError;

  // ── CSS @keyframes registry ───────────────────────────────────────────────
  // Rebuilt by _rebuildKeyframeRegistry() after every stylesheet change.
  // Elements read it at draw time via mKeyframeRegistryPtr_ to play animations.
  glint_keyframe_registry mKeyframeRegistry_;

  // ── CSS hot-reload ────────────────────────────────────────────────────────
  // One entry per stylesheet that was served from a real file on disk.
  // Polled every kHotReloadIntervalMs milliseconds inside Draw();
  // changed files are reloaded automatically.
  struct HotWatchEntry
  {
    std::string                             originalUrl; // argument passed to loadStylesheet()
    std::string                             path;        // real on-disk file path (from fromFile / disk fallback)
    std::string                             sourceUrl;   // sheet.sourceUrl (used to remove old entry)
    std::filesystem::file_time_type         mtime;       // last-known modification time
  };
  std::vector<HotWatchEntry>            mHotWatchedPaths;
  bool                                  mStylesheetHotReloadEnabled = false;
  std::chrono::steady_clock::time_point mLastHotReloadCheck{};
  static constexpr int kHotReloadIntervalMs = 500;
  uint64_t                              mStylesheetRevision = 1;
  uint64_t                              mTreeRevision = 1;
  std::vector<GlintFlatQualifiedRuleRef> mQualifiedRuleCache;
  std::vector<std::string>              mSelectorAttributeNames; // attribute names used in [attr] selectors
  std::unordered_map<GlintMatchedCssCacheKey,
                     std::vector<GlintMatchedCssRule>,
                     GlintMatchedCssCacheKeyHash> mMatchedCssRulesCache;

  // Reconstruct the selector text from the raw prelude token stream stored in
  // GlintCssQualifiedRule::prelToks.  Used by matchedCssRulesFor() to label
  // inspector rule blocks with the original selector string.
  static std::string _prelToksToText(const std::vector<GlintCssToken>& toks)
  {
    std::string out;
    for (const auto& t : toks)
    {
      switch (t.type)
      {
        case GlintCssTokenType::IDENT:        out += t.value; break;
        case GlintCssTokenType::HASH:         out += '#'; out += t.value; break;
        case GlintCssTokenType::DELIM:        out += t.value; break;
        case GlintCssTokenType::COMMA:        out += ','; break;
        case GlintCssTokenType::WHITESPACE:   out += ' '; break;
        case GlintCssTokenType::COLON:        out += ':'; break;
        case GlintCssTokenType::OPEN_SQUARE:  out += '['; break;
        case GlintCssTokenType::CLOSE_SQUARE: out += ']'; break;
        case GlintCssTokenType::OPEN_PAREN:   out += '('; break;
        case GlintCssTokenType::CLOSE_PAREN:  out += ')'; break;
        case GlintCssTokenType::STRING:       out += '"'; out += t.value; out += '"'; break;
        default: break;
      }
    }
    // Strip trailing whitespace.
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
  }

  static uint64_t _hashSelectorToken(uint64_t seed, const std::string& value)
  {
    uint64_t h = seed;
    for (unsigned char c : value)
      h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ull;
    return h;
  }

  uint64_t _selectorMatchSignature(const glint_element* el, bool forcePseudoClasses) const
  {
    uint64_t hash = 1469598103934665603ull;
    hash = _hashSelectorToken(hash, forcePseudoClasses ? "1" : "0");
    for (const glint_element* node = el; node; node = node->mParent)
    {
      hash = _hashSelectorToken(hash, node->tagName() ? node->tagName() : "");
      hash = _hashSelectorToken(hash, node->element.id);
      hash = _hashSelectorToken(hash, node->className);
      hash = _hashSelectorToken(hash, node->innerText.empty() ? "0" : "1");
      hash = _hashSelectorToken(hash, std::to_string(node->mChildren.size()));
      // Include attribute values for any attributes referenced by loaded stylesheets
      // so that rules like input[type="range"] cache-invalidate correctly.
      for (const auto& attrName : mSelectorAttributeNames)
      {
        bool found = false;
        const std::string val = node->getAttribute(attrName, found);
        hash = _hashSelectorToken(hash, found ? val : "\x01");
      }
      if (forcePseudoClasses)
      {
        hash = _hashSelectorToken(hash, "1111");
      }
      else
      {
        hash = _hashSelectorToken(hash, node->mIsHovered ? "1" : "0");
        hash = _hashSelectorToken(hash, node->mIsActive ? "1" : "0");
        hash = _hashSelectorToken(hash, node->mIsFocused ? "1" : "0");
        hash = _hashSelectorToken(hash, node->mIsFocusWithin ? "1" : "0");
      }
      if (node->mParent)
      {
        GlintCssDomAdapter adapter(const_cast<glint_element*>(node));
        adapter.forcePseudoClasses = forcePseudoClasses;
        hash = _hashSelectorToken(hash, std::to_string(adapter.childIndex()));
        hash = _hashSelectorToken(hash, std::to_string(adapter.siblingCount()));
        hash = _hashSelectorToken(hash, std::to_string(adapter.typeChildIndex()));
        hash = _hashSelectorToken(hash, std::to_string(adapter.typeSiblingCount()));
        for (const auto& sibling : node->mParent->mChildren)
        {
          if (!sibling) continue;
          hash = _hashSelectorToken(hash, sibling->tagName() ? sibling->tagName() : "");
          hash = _hashSelectorToken(hash, sibling->element.id);
          hash = _hashSelectorToken(hash, sibling->className);
        }
      }
      hash ^= 0x9e3779b97f4a7c15ull;
    }
    return hash;
  }

  void _rebuildQualifiedRuleCache()
  {
    mQualifiedRuleCache.clear();

    size_t sourceOrder = 0;
    auto appendRules = [this, &sourceOrder](const GlintCssStylesheet& sheet)
    {
      std::vector<const GlintCssQualifiedRule*> sheetRules;
      sheet.collectQualifiedRules(sheetRules);
      for (auto* rule : sheetRules)
      {
        if (!rule)
        {
          ++sourceOrder;
          continue;
        }

        GlintFlatQualifiedRuleRef ref;
        ref.rule = rule;
        ref.selectorText = _prelToksToText(rule->prelToks);
        ref.sourceUrl = sheet.sourceUrl;
        ref.sourceOrder = sourceOrder++;
        mQualifiedRuleCache.push_back(std::move(ref));
      }
    };

    appendRules(mUaSheet);
    for (const auto& sheet : mStylesheets)
      appendRules(sheet);

    // Collect the set of attribute names referenced in [attr] selectors across
    // all loaded rules so _selectorMatchSignature can include them in the hash.
    std::unordered_set<std::string> attrNames;
    for (const auto& flat : mQualifiedRuleCache)
    {
      if (!flat.rule) continue;
      for (const auto& complexSel : flat.rule->selectorList.selectors)
        for (const auto& step : complexSel.steps)
          for (const auto& ss : step.compound.simples)
            if (ss.kind == GlintSimpleKind::ATTRIBUTE && !ss.attrName.empty())
              attrNames.insert(ss.attrName);
    }
    mSelectorAttributeNames.assign(attrNames.begin(), attrNames.end());
    std::sort(mSelectorAttributeNames.begin(), mSelectorAttributeNames.end());
  }

  void _invalidateMatchedCssRuleCache(bool bumpStylesheetRevision)
  {
    if (bumpStylesheetRevision)
      ++mStylesheetRevision;
    mMatchedCssRulesCache.clear();
  }

  void _onTreeStructureChanged()
  {
    ++mTreeRevision;
    mMatchedCssRulesCache.clear();
  }

  // ── @keyframes registry helpers ────────────────────────────────────────────

  /** Rebuild the @keyframes registry from scratch by scanning all loaded
   *  stylesheets in order.  Later sheets override same-named keyframes from
   *  earlier sheets (last-writer-wins, mirroring the CSS cascade source order).
   *  Called after every loadStylesheet() and clearStylesheets(). */
  void _rebuildKeyframeRegistry()
  {
    mKeyframeRegistry_.clear();
    for (const auto& sheet : mStylesheets)
      _processKeyframes(sheet);
  }

  /** Scan one stylesheet for @keyframes at-rules and add them to `mKeyframeRegistry_`.
   *  Each stop's declared properties are serialised via glint_style_get_by_name()
   *  so that glint_keyframe_apply() can interpolate them using the existing lerp
   *  infrastructure without any additional parsing at runtime. */
  void _processKeyframes(const GlintCssStylesheet& sheet)
  {
    // Helper: trim whitespace from both ends.
    auto trimKf = [](const std::string& s) -> std::string {
      const size_t a = s.find_first_not_of(" \t\n\r\f");
      if (a == std::string::npos) return {};
      const size_t b = s.find_last_not_of(" \t\n\r\f");
      return s.substr(a, b - a + 1);
    };
    // Helper: ASCII lower-case.
    auto lowKf = [](std::string s) -> std::string {
      for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return s;
    };

    for (const auto& rule : sheet.rules)
    {
      if (rule.kind != GlintCssStylesheet::Rule::Kind::AT) continue;
      if (!rule.atRule) continue;
      const std::string& n = rule.atRule->name;
      if (n != "keyframes" && n != "-webkit-keyframes") continue;

      const std::string animName = trimKf(rule.atRule->prelude);
      if (animName.empty()) continue;

      glint_keyframe_rule kf;
      kf.name = animName;

      for (const auto& child : rule.atRule->children)
      {
        if (child.kind != GlintCssAtRule::ChildRule::Kind::QUALIFIED || !child.qualified)
          continue;

        // Resolve the declared properties into a temporary style so we can
        // serialise them back via glint_style_get_by_name().
        glint_style tmp;
        GlintCssApply::apply(child.qualified->declarations, tmp);

        // Build a set of the CSS property names that are EXPLICITLY declared in
        // this keyframe stop rule.  Only these properties will be stored in the
        // stop; un-declared properties must not appear in stop.properties because
        // glint_keyframe_apply iterates hi->properties and writes every entry to
        // computedStyle — if default-valued undeclared properties (e.g. the
        // default backgroundColor "#00000000") were stored, the animation would
        // overwrite CSS-cascaded values (e.g. the element's actual background-color)
        // with their C++ defaults every frame, making the element visually wrong.
        // This matches browser behaviour: only declared properties participate in
        // keyframe interpolation; undeclared ones fall back to the underlying
        // cascade or the adjacent stop's value at interpolation time.
        std::unordered_set<std::string> declaredCssProps;
        for (const auto& decl : child.qualified->declarations)
          declaredCssProps.insert(decl.property);

        // The stop selector sits in child.qualified->prelude ("from", "to", "0%", ...).
        // A single child rule may list multiple stops: "0%, 100%".
        const std::string& stopSel = child.qualified->prelude;

        std::vector<std::string> selParts;
        {
          std::string buf;
          for (const char c : stopSel)
          {
            if (c == ',') { selParts.push_back(trimKf(buf)); buf.clear(); }
            else          buf += c;
          }
          selParts.push_back(trimKf(buf));
        }

        for (const auto& sel : selParts)
        {
          const std::string low = lowKf(sel);
          float offset = 0.f;
          if      (low == "to")   offset = 1.f;
          else if (low == "from") offset = 0.f;
          else                    try { offset = std::stof(sel) / 100.f; } catch (...) {}

          glint_keyframe_stop stop;
          stop.offset = std::max(0.f, std::min(1.f, offset));
          for (const auto& key : glint_animatable_keys())
          {
            // Only store explicitly declared properties — never store default
            // values for properties that were not listed in the CSS stop rule.
            if (declaredCssProps.count(key) == 0) continue;
            const std::string val = glint_style_get_by_name(tmp, key);
            if (!val.empty())
              stop.properties[key] = val;
          }
          kf.stops.push_back(std::move(stop));
        }
      }

      // Sort stops ascending by offset.
      std::sort(kf.stops.begin(), kf.stops.end(),
        [](const glint_keyframe_stop& a, const glint_keyframe_stop& b) {
          return a.offset < b.offset; });

      mKeyframeRegistry_[animName] = std::move(kf);
    }
  }

  /**
   * Walk every @font-face rule in a parsed stylesheet and fire onRequest for
   * each url() found in its src declaration.
   *
   * This mirrors browser behaviour: after a stylesheet is parsed, the browser
   * fetches all font resources referenced by @font-face src entries so they
   * are available before any text is rendered.  Here the responseData is not
   * used yet (font registration via glint_load_font still happens in the
   * layout function), but the callback gives the host an opportunity to
   * intercept and log every font load, or to pre-cache the bytes.
   *
   * URL extraction follows CSS Fonts Level 4 §4.3:
   *   src: url("a.woff2") format("woff2"), url("a.ttf") format("truetype");
   * Multiple url() candidates are iterated left-to-right; onRequest is fired
   * for every one (the host can choose to respond only for formats it supports).
   */
  /**
   * Walk every @font-face rule in a parsed stylesheet, fire onRequest for
   * each url() in the src, and — when the handler responds with bytes —
  * register the font with both the graphics font cache and glint_font_registry
   * (including the raw SkTypeface for the Skia render path).
   *
   * This replaces explicit glint_load_font() call-sites: all font loading
   * now flows through CSS @font-face → onRequest → register.
   *
   * Only the first successfully fetched src candidate per @font-face is used
   * (mirrors browser fallback behaviour — the list is left-to-right).
   */
  void _processFontFaces(const GlintCssStylesheet& sheet)
  {
    if (!onRequest) return;
    glint_canvas* pG = mCanvas.mpG;

    for (const auto& rule : sheet.rules)
    {
      if (rule.kind != GlintCssStylesheet::Rule::Kind::AT) continue;
      if (!rule.atRule || rule.atRule->name != "font-face") continue;

      // Collect font-family, font-weight, font-style descriptor, and src.
      std::string fontFamily;
      int fontWeightDescriptor = 400;   // default per CSS spec
      std::string fontStyleDescriptor = "normal"; // default per CSS spec
      const GlintCssDeclaration* srcDecl = nullptr;
      for (const auto& decl : rule.atRule->declarations)
      {
        if (decl.property == "font-family")  fontFamily = decl.value;
        else if (decl.property == "src")     srcDecl    = &decl;
        else if (decl.property == "font-weight")
        {
          const std::string& wv = decl.value;
          if      (wv == "normal")  fontWeightDescriptor = 400;
          else if (wv == "bold")    fontWeightDescriptor = 700;
          else { try { fontWeightDescriptor = std::stoi(wv); } catch (...) {} }
        }
        else if (decl.property == "font-style")
        {
          const std::string low = decl.value;
          if (low == "italic" || low == "oblique") fontStyleDescriptor = low;
          else                                      fontStyleDescriptor = "normal";
        }
      }
      if (fontFamily.empty() || !srcDecl || srcDecl->valueTokens.empty()) continue;

      // font-family STRING tokens are re-serialised with surrounding quotes:
      //   font-family: "Kanit-Regular"  →  fontFamily = "\"Kanit-Regular\""
      // Strip them so the registered fontID matches _c.style.fontFace values.
      if (fontFamily.size() >= 2 &&
          fontFamily.front() == '"' && fontFamily.back() == '"')
        fontFamily = fontFamily.substr(1, fontFamily.size() - 2);
      if (fontFamily.empty()) continue;

      // For a unified family (e.g. font-family: "Kanit") with multiple @font-face
      // blocks at different weights/styles, the per-block registration keys are:
      //   Legacy weight key: "Kanit@100"        (for getTypefaceWeighted)
      //   Axis key:          "Kanit@100@italic"  (for getTypefaceByAxes)
      const std::string weightedKey = fontFamily + "@" + std::to_string(fontWeightDescriptor);
      const std::string axisKey     = weightedKey + "@" + fontStyleDescriptor;

      // Skip if this exact variant is already registered.
      if (glint_font_registry::isLoaded(axisKey)) continue;

      // Iterate src token stream — the CSS tokenizer produces two forms:
      //   Unquoted  url(/a/b.ttf)   → URL token,  .value = "/a/b.ttf"
      //   Quoted    url("/a/b.ttf") → FUNCTION("url") + STRING + CLOSE_PAREN
      const auto& toks = srcDecl->valueTokens;
      for (std::size_t i = 0; i < toks.size(); ++i)
      {
        std::string fontUrl;

        if (toks[i].type == GlintCssTokenType::URL)
        {
          fontUrl = toks[i].value;
        }
        else if (toks[i].type == GlintCssTokenType::FUNCTION &&
                 toks[i].value == "url")
        {
          std::size_t j = i + 1;
          while (j < toks.size() && toks[j].type == GlintCssTokenType::WHITESPACE) ++j;
          if (j < toks.size() && toks[j].type == GlintCssTokenType::STRING)
            fontUrl = toks[j].value;
        }

        if (fontUrl.empty()) continue;

        glint_resource_request fontReq;
        fontReq.url    = fontUrl;
        fontReq.type   = glint_resource_request::Type::Unknown;
        fontReq.source = nullptr;
        fontReq.parseUrl();
        onRequest(fontReq);

        if (!fontReq.handled || fontReq.statusCode != 200 || !fontReq.responseData)
          continue; // try next src candidate

        // ── Register with the graphics font cache (glint_text / DrawText path) ──
        if (pG)
          pG->LoadFont(fontFamily.c_str(),
                       const_cast<void*>(fontReq.responseData->data()),
                       static_cast<int>(fontReq.responseData->size()));

        // ── Register raw SkTypeface (glint_element raw Skia render path) ───
        auto mgr = glint_font_registry::systemFontMgr();
        if (mgr)
        {
          auto tf = mgr->makeFromData(fontReq.responseData);
          if (tf)
          {
            // Register under the exact font-family name used by CSS text nodes.
            glint_font_registry::registerTypeface(fontFamily, tf);
            glint_font_registry::loadedFonts().insert(fontFamily);

            // Legacy weight key: "Kanit@100" → for getTypefaceWeighted().
            glint_font_registry::registerTypeface(weightedKey, tf);
            glint_font_registry::loadedFonts().insert(weightedKey);

            // Three-axis key: "Kanit@100@italic" → for getTypefaceByAxes().
            // fontId = fontFamily (the name passed to pG->LoadFont above).
            glint_font_registry::registerTypefaceAxes(
              fontFamily, fontWeightDescriptor, fontStyleDescriptor, tf, fontFamily);
          }
        }

        break; // first successful src candidate is sufficient
      }
    }
  }

  // ── Injected callbacks ───────────────────────────────────────────────────────
  std::function<void()>                                              mRequestRedraw;
  std::function<void(glint_element*)>                                 mRequestRedrawDetailed;

  // ── Tree mutex ────────────────────────────────────────────────────────────
  // Guards concurrent access to mChildren vectors: held (shared) during
  // getUITree() on the inspector thread, and exclusively during addChild /
  // clearChildren / removeChild on the UI thread.
  mutable std::mutex mTreeMutex;

  // ── Frame-time ring buffer ────────────────────────────────────────────────
  static constexpr int kFrameBufSize = 64;
  std::array<std::chrono::steady_clock::time_point, kFrameBufSize> mFrameTimes{};
  int      mFrameHead  = 0;
  int      mFrameCount = 0;
  uint64_t mDrawCount  = 0;

  void _recordFrame()
  {
    mFrameTimes[mFrameHead] = std::chrono::steady_clock::now();
    mFrameHead = (mFrameHead + 1) % kFrameBufSize;
    if (mFrameCount < kFrameBufSize) ++mFrameCount;
    ++mDrawCount;
  }

  // ── Mouse + focus state ───────────────────────────────────────────────────────────
  glint_element* mHoveredNode    = nullptr;
  glint_element* mMouseDownNode  = nullptr;
  glint_element* mFocusedNode    = nullptr;  // currently focused (keyboard) node
  bool             mFocusViaKeyboard = false;  // true only for Tab/Shift+Tab focus (:focus-visible)

  glint_element* mLastClickNode = nullptr;
  std::chrono::steady_clock::time_point mLastClickTime{};

  // ── Tag registry ─────────────────────────────────────────────────────────────
  std::unordered_map<int, glint_element*> mTagMap;

  // ── Global cross-element selection ────────────────────────────────────────
  /** A point within a particular text component's content. */
  struct SelPoint { glint_element* comp = nullptr; int byte = 0; };
  SelPoint mGlobalAnchor;          ///< Where the current drag started.
  SelPoint mGlobalFocus;           ///< Current drag endpoint.
  bool     mGlobalSelActive = false; ///< True when ≥2 components involved.

  // ── Tree walk helpers ──────────────────────────────────────────────────

  static glint_element* findById(glint_element& node, uint64_t id)
  {
    if (node.isInspectorRemoved()) return nullptr;
    if (node.mId == id) return &node;
    for (auto& child : node.mChildren)
    {
      if (auto* found = findById(*child, id)) return found;
    }
    return nullptr;
  }

  static glint_element* findByIdIncludingRemoved(glint_element& node, uint64_t id)
  {
    if (node.mId == id) return &node;
    for (auto& child : node.mChildren)
    {
      if (auto* found = findByIdIncludingRemoved(*child, id)) return found;
    }
    return nullptr;
  }

  /**
   * Returns the nearest common ancestor of two nodes in the same tree.
   * DOM spec §17.1: dblclick is dispatched on the NCA of the two click targets.
   * If either pointer is null the other is returned; if they share no ancestor
   * (different trees) nullptr is returned.
   */
  static glint_element* _nearestCommonAncestor(glint_element* a, glint_element* b) noexcept
  {
    if (!a) return b;
    if (!b) return a;
    if (a == b) return a;
    // Build the ancestor chain for 'a' (including itself).
    glint_element* cur = a;
    while (cur)
    {
      // Walk up 'b' to see if any ancestor matches 'cur'.
      for (glint_element* p = b; p; p = p->mParent)
        if (p == cur) return cur;
      cur = cur->mParent;
    }
    return nullptr;
  }

  /** DFS walk matching element.id string. */
  static glint_element* findByStringId(glint_element& node, const std::string& strId)
  {
    if (node.isInspectorRemoved()) return nullptr;
    if (node.id == strId) return &node;
    for (auto& child : node.mChildren)
    {
      if (auto* found = findByStringId(*child, strId)) return found;
    }
    return nullptr;
  }

  /** Collect all focusable (mAcceptsFocus==true, display!=none) nodes in DFS order. */
  static void _collectFocusable(glint_element& node,
                                  std::vector<glint_element*>& out)
  {
    if (node.isInspectorRemoved()) return;
    if (node.style.display == "none") return;
    if (&node != &node.mRoot->mCanvas && node.mAcceptsFocus && node.mTabStop) out.push_back(&node);
    for (auto& child : node.mChildren)
      _collectFocusable(*child, out);
  }

  /** Move keyboard focus to the next (or previous) focusable node in DFS order. */
  void _focusTraversal(bool reverse)
  {
    mFocusViaKeyboard = true;   // Tab/Shift+Tab — show the focus ring (:focus-visible)
    std::vector<glint_element*> focusable;
    _collectFocusable(mCanvas, focusable);
    if (focusable.empty()) return;
    if (!mFocusedNode)
    {
      SetFocus(reverse ? focusable.back() : focusable.front());
      return;
    }
    auto it = std::find(focusable.begin(), focusable.end(), mFocusedNode);
    if (it == focusable.end())
    {
      SetFocus(reverse ? focusable.back() : focusable.front());
      return;
    }
    if (reverse)
    {
      if (it == focusable.begin()) SetFocus(focusable.back());
      else                         SetFocus(*std::prev(it));
    }
    else
    {
      ++it;
      if (it == focusable.end()) SetFocus(focusable.front());
      else                       SetFocus(*it);
    }
  }

  static glint_tree_node buildTreeNode(const glint_element& node)
  {
    glint_tree_node n;
    n.id        = node.mId;
    n.typeName  = node.tagName() ? node.tagName() : "";
    n.elementId = node.element.id;
    n.className = node.className;
    n.innerText = node.innerText;
    n.rect      = node.GetPaintRECT();
    n.styleInfo = glint_style_serialize(node.style);
    for (const auto& child : node.mChildren)
      if (child && !child->isInspectorRemoved()) n.children.push_back(buildTreeNode(*child));
    return n;
  }
  // ── Helpers ──────────────────────────────────────────────────────────────────

  // ── Global cross-element selection helpers ────────────────────────────────

  /** DFS walk: collect all isGlobalSelectable() nodes in document order.
   *  Stops recursing into a node once it is itself selectable. */
  static void _collectSelectable(glint_element& node,
                                  std::vector<glint_element*>& out)
  {
    if (node.isInspectorRemoved()) return;
    if (node.style.display == "none") return;
    if (node.isGlobalSelectable()) { out.push_back(&node); return; }
    for (auto& ch : node.mChildren)
      _collectSelectable(*ch, out);
  }

  /** Clear all globally-assigned selection ranges and reset state. */
  void _clearGlobalSelection()
  {
    if (mGlobalSelActive || mGlobalAnchor.comp)
    {
      std::vector<glint_element*> nodes;
      _collectSelectable(mCanvas, nodes);
      for (auto* n : nodes) n->setGlobalSelRange(-1, -1);
    }
    mGlobalAnchor    = {};
    mGlobalFocus     = {};
    mGlobalSelActive = false;
  }

  /** Apply selection ranges to all selectable nodes based on mGlobalAnchor /
   *  mGlobalFocus in document (DFS) order. */
  void _applyGlobalSelection()
  {
    if (!mGlobalAnchor.comp || !mGlobalFocus.comp) return;

    std::vector<glint_element*> nodes;
    _collectSelectable(mCanvas, nodes);

    int anchorIdx = -1, focusIdx = -1;
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
      if (nodes[i] == mGlobalAnchor.comp) anchorIdx = i;
      if (nodes[i] == mGlobalFocus.comp)  focusIdx  = i;
    }
    if (anchorIdx < 0 || focusIdx < 0) return;

    const bool forward = (anchorIdx < focusIdx) ||
                         (anchorIdx == focusIdx &&
                          mGlobalAnchor.byte <= mGlobalFocus.byte);
    const int loIdx  = forward ? anchorIdx          : focusIdx;
    const int hiIdx  = forward ? focusIdx           : anchorIdx;
    int loByte = forward ? mGlobalAnchor.byte : mGlobalFocus.byte;
    int hiByte = forward ? mGlobalFocus.byte  : mGlobalAnchor.byte;

    // Double-click (word-mode) drag extending across label boundaries:
    // snap both selection edges to word boundaries, matching Chrome's
    // behaviour of selecting whole words when double-click-dragging.
    // Single-node WORD drag is handled internally by glint_element::OnMouseDrag.
    if (loIdx != hiIdx && mGlobalAnchor.comp->globalDragIsWordMode())
    {
      loByte = nodes[loIdx]->globalWordBoundaryAt(loByte).first;   // expand to word start
      hiByte = nodes[hiIdx]->globalWordBoundaryAt(hiByte).second;  // expand to word end
    }

    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
    {
      auto* n = nodes[i];
      if      (i < loIdx)                n->setGlobalSelRange(-1, -1);
      else if (i > hiIdx)                n->setGlobalSelRange(-1, -1);
      else if (i == loIdx && i == hiIdx) n->setGlobalSelRange(loByte, hiByte);
      else if (i == loIdx)               n->setGlobalSelRange(loByte, n->globalTextLen());
      else if (i == hiIdx)               n->setGlobalSelRange(0, hiByte);
      else                               n->setGlobalSelRange(0, n->globalTextLen());
    }
  }

  /** Return the selectable node whose paint rect is screen-closest to (x, y). */
  glint_element* _nearestSelectable(float x, float y) const
  {
    std::vector<glint_element*> nodes;
    _collectSelectable(const_cast<glint_element&>(mCanvas), nodes);
    glint_element* best     = nullptr;
    float            bestDist = 1e30f;
    for (auto* n : nodes)
    {
      // mPaintRECT is in content-space (layout coords).  Convert to screen-space
      // by subtracting accumulated ancestor scroll offsets — the inverse of what
      // scrollAdjusted() does (it adds offsets to go screen → content).
      glint_rect r = n->GetPaintRECT();
      for (const glint_element* p = n->mParent; p; p = p->mParent)
      {
        r.L -= p->mScrollLeft; r.R -= p->mScrollLeft;
        r.T -= p->mScrollTop;  r.B -= p->mScrollTop;
      }
      const float dx = (x < r.L) ? r.L - x : (x > r.R) ? x - r.R : 0.f;
      const float dy = (y < r.T) ? r.T - y : (y > r.B) ? y - r.B : 0.f;
      const float d  = dx * dx + dy * dy;
      if (d < bestDist) { bestDist = d; best = n; }
    }
    return best;
  }

  /** Collect selected text from each selectable node in DFS order,

   *  joining non-empty chunks with '\n'. */
  std::string _getGlobalSelectedText() const
  {
    std::vector<glint_element*> nodes;
    _collectSelectable(const_cast<glint_element&>(mCanvas), nodes);
    std::string result;
    for (auto* n : nodes)
    {
      const std::string chunk = n->getGlobalSelText();
      if (!chunk.empty())
      {
        if (!result.empty()) result += '\n';
        result += chunk;
      }
    }
    return result;
  }

  /** Write text to the system clipboard (Win32 CF_UNICODETEXT). */
  static void _setClipboard(const std::string& text)
  {
#ifdef _WIN32
    if (!::OpenClipboard(nullptr)) return;
    ::EmptyClipboard();
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen > 0)
    {
      HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE, static_cast<SIZE_T>(wlen) * sizeof(WCHAR));
      if (h)
      {
        WCHAR* p = static_cast<WCHAR*>(::GlobalLock(h));
        if (p) { ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wlen); ::GlobalUnlock(h); }
        ::SetClipboardData(CF_UNICODETEXT, h);
      }
    }
    ::CloseClipboard();
#elif defined(__APPLE__)
    glint_platform::setClipboardText(text);
#endif
  }

  /**
   * Translate screen-space mouse coordinates into content-space coordinates
   * by accumulating mScrollLeft / mScrollTop from all ancestors of `node`.
   * HitTest already applies these offsets when recursing into scroll containers,
   * so a child's mRect is in content space.  We need clientX/Y to match.
   *
   * EXCEPTION: scrollbars (mScrollbarV/H/mScrollCorner) are hit-tested with
   * screen-space coords (before the scroll offset is applied), so their mRect
   * is in screen space.  When the child being walked is a scrollbar of its
   * parent, we must NOT add the parent's scroll offset at that level.
   */
  static void scrollAdjusted(const glint_element* node, float& x, float& y)
  {
    const glint_element* child = node;
    const glint_element* p    = node ? node->mParent : nullptr;
    while (p)
    {
      // Scrollbars are excluded from content-space offset: they were hit-tested
      // with raw screen coords, so don't add the parent's mScrollLeft/Top here.
      const bool isScrollbarOfParent = (child == p->mScrollbarV  ||
                                        child == p->mScrollbarH  ||
                                        child == p->mScrollCorner);
      if (!isScrollbarOfParent)
      {
        x += p->mScrollLeft;
        y += p->mScrollTop;
      }
      child = p;
      p     = p->mParent;
    }
  }

  static glint_mouse_event makeME(const char* type, float x, float y,
                                   const glint_mouse_mod& mod,
                                   float dX = 0.f, float dY = 0.f,
                                   bool bubbles = true)
  {
    glint_mouse_event e;
    e.type       = type;
    e.bubbles    = bubbles;
    e.cancelable = true;
    e.clientX    = x;
    e.clientY    = y;
    e.movementX  = dX;
    e.movementY  = dY;
    e.shiftKey   = mod.S;
    e.ctrlKey    = mod.C;
    e.altKey     = mod.A;
    e.button     = mod.R ? 2 : 0;
    e.buttons    = (mod.L ? 1 : 0) | (mod.R ? 2 : 0);
    return e;
  }

  glint_element* hitTest(float x, float y) const
  {
    return const_cast<glint_element&>(mCanvas).HitTest(x, y);
  }
};

// ── glint_element::Blur ────────────────────────────────────────────────────
// Defined here because it calls glint_document::SetFocus (mRoot must be complete).

inline void glint_element::Blur()
{
  if (mRoot) mRoot->SetFocus(nullptr);
}

// ── glint_element attach/tree helpers ──────────────────────────────────────
// Implemented here (after glint_document is fully defined) because they
// dereference glint_document members.

inline void glint_element::attachSubtree()
{
  if (mIsAttachedToTree) return;

  mIsAttachedToTree = true;

  if (mRoot && mTag != glint_no_tag)
    mRoot->RegisterTag(mTag, this);

  finalizeTreeState();

  std::vector<glint_element*> children;
  {
    std::unique_lock<std::mutex> lk;
    if (mTreeMutex) lk = std::unique_lock<std::mutex>(*mTreeMutex);
    children.reserve(mChildren.size());
    for (auto& child : mChildren)
      children.push_back(child.get());
  }

  for (auto* child : children)
  {
    if (!child) continue;
    child->mpG                   = mpG;
    child->mRoot                 = mRoot;
    child->mRequestRedraw        = mRequestRedraw;
    child->mRequestRedrawDetailed = mRequestRedrawDetailed;
    child->mApplyCss             = mApplyCss;
    child->mKeyframeRegistryPtr_ = mKeyframeRegistryPtr_;
    child->mTreeMutex            = mTreeMutex;
    child->mParent               = this;
    if (child->mParentW == 0.f) child->mParentW = std::max(0.f, getContent().W());
    if (child->mParentH == 0.f) child->mParentH = std::max(0.f, getContent().H());
    child->attachSubtree();
  }
}

inline void glint_element::callRootTreeChanged()
{
  if (mRoot)
  {
    mRoot->NotifyTreeStructureChanged();
    glint_bus::publish(glint_tree_changed_event{mRoot});
  }
}

inline void glint_element::callRootStyleChanged(uint64_t id)
{
  if (mRoot) glint_bus::publish(glint_node_style_changed_event{mRoot, id});
}

// Resolve a mask url(#id) reference: walk tree from the document root.
inline glint_element* glint_element::findMaskSourceElement(const std::string& strId) const
{
  if (!mRoot || strId.empty()) return nullptr;
  return mRoot->getElementByStringId(strId);
}

// ── glint_element::notifyDestroyed ─────────────────────────────────────────
// Defined here because it calls glint_document::_onComponentDestroyed.

inline void glint_element::notifyDestroyed()
{
  if (mRoot) mRoot->_onComponentDestroyed(this);
}

// ── glint_element::_getOnRequest ───────────────────────────────────────────
// Returns a pointer to the owning document's onRequest callback, or nullptr.
// Defined here (after glint_document is complete) so inline method bodies in
// glint_element.hpp and glint_element_render.hpp can pass the callback to
// glint_load_image / glint_load_svg_dom without needing the full document type.

inline const std::function<void(glint_resource_request&)>*
glint_element::_getOnRequest() const
{
  return mRoot ? &mRoot->onRequest : nullptr;
}

inline glint_network_log*
glint_element::_getNetworkLog() const
{
  return mRoot ? &mRoot->networkLog : nullptr;
}

inline bool glint_element::_isFocusViaKeyboard() const
{
  return mRoot && mRoot->isFocusViaKeyboard();
}

inline void glint_element::_markRootLayoutDirty()
{
  if (mRoot) mRoot->mLayoutDirty = true;
}

inline float glint_element::_getRootDpr() const
{
  return mRoot ? mRoot->devicePixelRatio : 1.f;
}

// ── glint_document ─────────────────────────────────────────────────────────────────
// New API name for glint_document.  registerElement / createElement are on
// glint_element (the base class) so they are accessible from both names.
// Both refer to the same class; use glint_document in new code.
