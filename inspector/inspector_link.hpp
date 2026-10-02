#pragma once

/**
 * inspector/inspector_link.hpp
 * The inspector's only way to reach the document it inspects.
 *
 * The inspector window runs on its own thread, and the inspected document is
 * laid out, drawn and changed on another one.  The inspector therefore never
 * dereferences the document or its elements:
 *
 *   - Reads are snapshots: request() runs a function on the document's thread
 *     (posted to its task queue) and hands the value it returns back to the
 *     inspector's thread (through the inspector document's task queue).
 *   - Edits are commands: command() runs a function on the document's thread.
 *
 * Elements are identified by their stable mId.  Work for a document that has
 * been destroyed is dropped (its queue is never drained again), and results
 * for an inspector that has closed are dropped the same way.
 */

#include "../glint_document.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Everything the inspector panels show about one element, captured on the
// document's thread (glint_inspector_capture_node).
struct glint_inspector_node
{
  uint64_t    id     = 0;       // 0: the element no longer exists
  bool        isRoot = false;   // the document canvas (cannot be removed)
  std::string typeName;
  std::string elementId;        // element.id
  std::string className;
  std::string innerText;
  glint_rect  rect;             // layout box (mRect)
  glint_style style;            // authored (inline) layer
  glint_style computedStyle;
  // id / className / innerText plus component attributes (value, src...).
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<GlintMatchedCssRule> rules;        // matching CSS rules
  std::vector<GlintMatchedCssRule> forcedRules;  // with :hover etc. forced on
};

// Captures a glint_inspector_node.  Runs on the inspected document's thread;
// defined in style_editor.hpp.
inline glint_inspector_node glint_inspector_capture_node(glint_document& doc, uint64_t id);

class glint_inspector_link
{
public:
  /** On the inspected document's thread (the inspector is opened from it). */
  void bindApp(glint_document* app)
  {
    mApp      = app;
    mAppQueue = app ? app->taskQueue() : nullptr;
    mAppLife  = app ? app->documentLifeToken() : std::weak_ptr<void>{};
  }

  /** On the inspector's thread, once its own document exists.  `life` must
   *  expire when the inspector goes away (results are dropped after that). */
  void bindInspector(glint_document* inspector, std::weak_ptr<void> life)
  {
    mInspQueue = inspector ? inspector->taskQueue() : nullptr;
    mInspLife  = std::move(life);
  }

  /** False once the inspected document has been destroyed. */
  bool appAlive() const { return mAppQueue && !mAppLife.expired(); }

  /** Runs fn(document) on the document's thread. */
  void command(std::function<void(glint_document&)> fn) const
  {
    if (!mAppQueue || !fn) return;
    glint_document* app = mApp;
    std::weak_ptr<void> life = mAppLife;
    mAppQueue->post([app, life, fn = std::move(fn)] {
      if (!life.expired()) fn(*app);
    });
  }

  /** Runs fn(document) on the document's thread, then done(result) on the
   *  inspector's thread.  Both must capture by value: they run on other
   *  threads than the caller. */
  template <class R>
  void request(std::function<R(glint_document&)> fn, std::function<void(R&)> done) const
  {
    if (!mAppQueue || !mInspQueue || !fn || !done) return;
    glint_document* app = mApp;
    std::weak_ptr<void> appLife = mAppLife;
    std::shared_ptr<glint_task_queue> inspQueue = mInspQueue;
    std::weak_ptr<void> inspLife = mInspLife;
    mAppQueue->post([app, appLife, inspQueue, inspLife, fn = std::move(fn), done = std::move(done)] {
      if (appLife.expired() || inspLife.expired()) return;
      auto result = std::make_shared<R>(fn(*app));
      inspQueue->post([inspLife, result, done] {
        if (!inspLife.expired()) done(*result);
      });
    });
  }

  /** Runs fn on the inspector's thread (e.g. from another inspector-owned
   *  thread such as a picker window). */
  void toInspector(std::function<void()> fn) const
  {
    if (!mInspQueue || !fn) return;
    std::weak_ptr<void> life = mInspLife;
    mInspQueue->post([life, fn = std::move(fn)] {
      if (!life.expired()) fn();
    });
  }

private:
  glint_document*                   mApp = nullptr;   // dereferenced on its own thread only
  std::shared_ptr<glint_task_queue> mAppQueue;
  std::weak_ptr<void>               mAppLife;
  std::shared_ptr<glint_task_queue> mInspQueue;
  std::weak_ptr<void>               mInspLife;
};
