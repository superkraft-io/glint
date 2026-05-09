#pragma once

/**
 * glint_network_log.hpp
 * Per-document network request log — mirrors the Chrome DevTools Network tab.
 *
 * Every onRequest-intercepted or disk-fallback asset load (background-img,
 * mask url(), <img> src, SVG) pushes one glint_network_log_entry here.
 * The inspector reads a thread-safe snapshot to display the Network tab.
 *
 * Usage:
 *   // Read from inspector thread:
 *   auto entries = body->mRoot.networkLog.snapshot();
 *
 *   // Clear from UI thread:
 *   body->mRoot.networkLog.clear();
 */

#include "../render/glint_resource_request.hpp"
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// ── glint_network_log_entry ───────────────────────────────────────────────────

struct glint_network_log_entry
{
  // ── Request ───────────────────────────────────────────────────────────────
  std::string                  url;      // full string set on src / url(...)
  std::string                  pathname; // parsed path component
  glint_resource_request::Type  type   = glint_resource_request::Type::Unknown;

  // ── Response ──────────────────────────────────────────────────────────────
  // statusCode mirrors HTTP semantics:
  //   0   = unhandled (handler was never called — disk path, no onRequest)
  //   200 = OK  (data provided or disk load succeeded)
  //   404 = Not Found (handler errored, or disk file missing)
  //   other 4xx/5xx = explicit req.error(code, msg) from the handler
  int         statusCode    = 0;
  std::string statusMessage;
  bool        handled       = false;  // true when onRequest handler responded
  size_t      byteSize      = 0;      // bytes in responseData (0 on error)

  // ── Timing ────────────────────────────────────────────────────────────────
  std::chrono::steady_clock::time_point timestamp;

  // ── Source element identity ───────────────────────────────────────────────
  // Copied at push time — the pointer is never stored.
  // Zero/empty when the source cannot be resolved at the call site
  // (e.g. free-function loaders in glint_mask.hpp where glint_element is
  // only forward-declared; those fields are filled in from glint_image::_loadSrc).
  uint64_t    sourceId    = 0;   // glint_element::mId
  int         sourceTag   = -1;  // glint_element::mTag  (-1 = kNoTag / unknown)
  std::string sourceType;        // glint_element::typeName()  e.g. "img", "div"
  std::string sourceElemId;      // glint_element::id  (the DOM string id)
};

// ── glint_network_log ─────────────────────────────────────────────────────────

class glint_network_log
{
public:
  /** Maximum number of entries retained before the oldest is dropped. */
  static constexpr size_t kMaxEntries = 500;

  /** Thread-safe: add an entry. Oldest is dropped if capacity is exceeded.
   *  Stamps timestamp automatically when entry.timestamp is at the epoch. */
  void push(glint_network_log_entry entry)
  {
    if (entry.timestamp == std::chrono::steady_clock::time_point{})
      entry.timestamp = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mMutex);
    if (mEntries.size() >= kMaxEntries)
      mEntries.pop_front();
    mEntries.push_back(std::move(entry));
  }

  /** Thread-safe: discard all entries. */
  void clear()
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mEntries.clear();
  }

  /** Thread-safe snapshot — returns a copy suitable for reading on any thread
   *  (e.g. the inspector window thread). */
  std::vector<glint_network_log_entry> snapshot() const
  {
    std::lock_guard<std::mutex> lock(mMutex);
    return { mEntries.begin(), mEntries.end() };
  }

  /** Number of entries currently stored (approximate — may race). */
  size_t size() const
  {
    std::lock_guard<std::mutex> lock(mMutex);
    return mEntries.size();
  }

private:
  mutable std::mutex                  mMutex;
  std::deque<glint_network_log_entry>  mEntries;
};

// ── Push helpers (free functions) ─────────────────────────────────────────────
// Called from glint_mask.hpp loaders (source identity not available there) and
// from glint_image::_loadSrc (full source identity available via `this`).

/** Push a log entry from a completed glint_resource_request.
 *  Source identity fields default to zero/empty; supply them from call sites
 *  where the glint_element type is fully defined. */
inline void glint_network_log_push(
    glint_network_log*                    log,
    const std::string&                   url,
    glint_resource_request::Type          type,
    const glint_resource_request&         req,
    uint64_t    sourceId     = 0,
    int         sourceTag    = -1,
    const char* sourceType   = nullptr,
    const char* sourceElemId = nullptr)
{
  if (!log) return;
  glint_network_log_entry e;
  e.url           = url;
  e.pathname      = req.pathname;
  e.type          = type;
  e.statusCode    = req.statusCode;
  e.statusMessage = req.statusMessage;
  e.handled       = req.handled;
  e.byteSize      = req.responseData ? req.responseData->size() : 0;
  e.sourceId      = sourceId;
  e.sourceTag     = sourceTag;
  if (sourceType)   e.sourceType   = sourceType;
  if (sourceElemId) e.sourceElemId = sourceElemId;
  log->push(std::move(e));
}

/** Push a log entry for a disk-fallback load (onRequest not involved).
 *  handled = false signals to the Network tab that this bypassed onRequest. */
inline void glint_network_log_push_disk(
    glint_network_log*                log,
    const std::string&               url,
    glint_resource_request::Type      type,
    bool                             found,
    uint64_t    sourceId     = 0,
    int         sourceTag    = -1,
    const char* sourceType   = nullptr,
    const char* sourceElemId = nullptr)
{
  if (!log) return;
  glint_network_log_entry e;
  e.url           = url;
  e.pathname      = url;   // bare path — no scheme component to strip
  e.type          = type;
  e.statusCode    = found ? 200 : 404;
  e.statusMessage = found ? "OK" : "File Not Found";
  e.handled       = false;
  e.byteSize      = 0;
  e.sourceId      = sourceId;
  e.sourceTag     = sourceTag;
  if (sourceType)   e.sourceType   = sourceType;
  if (sourceElemId) e.sourceElemId = sourceElemId;
  log->push(std::move(e));
}
