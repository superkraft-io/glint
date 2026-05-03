#pragma once

/**
 * glint_resource_request.hpp
 * Resource interception object — mirrors the browser Fetch / service-worker API.
 *
 * Fired by glint_document::onRequest before every url-based asset load:
 *   <img src="...">, CSS background-image: url("..."), CSS mask: url("...")
 *
 * The handler is the SOLE authority when registered — if it does not call
 * one of the respond helpers the load silently fails (HTTP 404 equivalent).
 * Only when NO handler is registered does the engine fall back to disk.
 *
 * Respond with one of the three helpers:
 *
 *   req.fromFile("/real/path/on/disk.png");          // reads file, 200 or 404
 *   req.fromBuffer(ptr, size);                        // wrap raw bytes, 200
 *   req.fromData(sk_sp<SkData>);                      // wrap SkData, 200
 *   req.error(404, "Not found");                      // explicit failure, no data
 *
 * Leaving the request untouched (no helper called) → silent 404.
 *
 * Example:
 *
 *   doc.onRequest = [&](glint_resource_request& req) {
 *     if (req.pathname == "/img/logo.svg")
 *       req.fromBuffer(LOGO_SVG, LOGO_SVG_SIZE);
 *     else
 *       req.error(404, "Unknown resource");
 *   };
 *
 * req.source is the requesting element — valid only during the callback,
 * do not store the pointer.
 */

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

// Forward declaration — glint_element is defined in glint_element.hpp.
// We only need a pointer here so no full definition is required.
class glint_element;

#include "include/core/SkData.h"

struct glint_resource_request
{
  enum class Type { Image, SVG, Stylesheet, Unknown };

  // ── Raw URL ──────────────────────────────────────────────────────────────
  std::string         url;           // full path / URL that was requested

  // ── Parsed URL components (mirrors the WHATWG URL API) ───────────────────
  // All fields are populated by parseUrl(), which is called automatically
  // by the engine after setting url. Bare paths (no scheme) leave scheme and
  // host empty and put the full path in pathname.
  //
  //   "res://myhost/img/icon.svg?scale=2&v=3#frag"
  //    scheme   = "res"
  //    host     = "myhost"
  //    pathname = "/img/icon.svg"
  //    search   = "?scale=2&v=3"   (includes leading '?')
  //    hash     = "#frag"           (includes leading '#')
  //
  //   "img/svg/time_fast.svg"
  //    scheme   = ""
  //    host     = ""
  //    pathname = "img/svg/time_fast.svg"
  //    search   = ""
  //    hash     = ""
  std::string         scheme;        // e.g. "res", "file", "https", "" for bare paths
  std::string         host;          // e.g. "example.com", "" for local paths
  std::string         pathname;      // path component, e.g. "/img/icon.svg"
  std::string         search;        // query string including '?', e.g. "?key=val"
  std::string         hash;          // fragment including '#',  e.g. "#section"
  std::unordered_map<std::string, std::string>
                      queryParams;   // parsed key→value pairs from search

  // ── Meta ─────────────────────────────────────────────────────────────────
  Type                type;          // Image | SVG | Unknown
  const glint_element* source;        // requesting element — valid during callback only, do not store

  // ── Response ─────────────────────────────────────────────────────────────
  sk_sp<SkData> responseData;        // raw encoded bytes supplied by the handler

  // ── Status (set by the respond helpers, readable after the callback) ─────
  // Mirrors HTTP status semantics:
  //   0   = disk load (no handler registered — never seen in a handled request)
  //   200 = ok        (fromFile / fromBuffer / fromData succeeded)
  //   4xx = client error (fromFile not found → 404, explicit error() call)
  //   5xx = server error (500 when handler returned without calling any helper)
  int         statusCode    = 0;
  std::string statusMessage;         // human-readable, e.g. "Not Found"
  // handled is set automatically by the respond helpers — never set it manually.
  // Readable after the callback returns; used by glint_network_log.
  bool        handled       = false;
  // Populated by fromFile() with the normalised on-disk path that was actually
  // read.  Empty when the response came from fromBuffer() / fromData().
  // Used by glint_document's hot-reload watcher to monitor the real file.
  std::string resolvedFilePath;

  // ── Respond helpers ───────────────────────────────────────────────────────

  /** Serve raw bytes already in an SkData. Sets status 200. */
  void fromData(sk_sp<SkData> data)
  {
    responseData  = std::move(data);
    statusCode    = 200;
    statusMessage = "OK";
    handled       = true;
  }

  /** Wrap a raw byte buffer. Pass copy=false (default) for static/embedded
   *  arrays — the caller must ensure the buffer outlives the request. */
  void fromBuffer(const void* ptr, size_t size, bool copy = false)
  {
    fromData(copy ? SkData::MakeWithCopy(ptr, size)
                  : SkData::MakeWithoutCopy(ptr, size));
  }

  /** Read a file from disk. On success sets status 200; if the file is not
   *  found automatically calls error(404, ...).
   *  The path is normalised first (mixed slashes → OS-native separator). */
  void fromFile(const std::string& path)
  {
    // Normalise: converts any mix of '/' and '\' to the OS-preferred separator,
    // then canonicalises redundant separators / . segments without touching the
    // filesystem (lexically_normal). This handles paths assembled from
    // glint_assets_dir() (Windows backslash) + req.pathname (forward slash).
    std::string normalised =
        std::filesystem::path(path).make_preferred().lexically_normal().string();

    auto data = SkData::MakeFromFileName(normalised.c_str());
    if (data)
    {
      resolvedFilePath = normalised;
      fromData(std::move(data));
    }
    else
      error(404, "File not found: " + normalised);
  }

  /** Signal an explicit failure without providing data.
   *  The engine treats this as a silent load failure (blank image). */
  void error(int code, const std::string& message)
  {
    statusCode    = code;
    statusMessage = message;
    handled       = true;
    responseData  = nullptr;
  }

  // ── Helpers ───────────────────────────────────────────────────────────────
  /** Parse this->url into scheme, host, pathname, search, queryParams, hash.
   *  Called automatically by the engine after setting url. */
  void parseUrl()
  {
    scheme = host = pathname = search = hash = "";
    queryParams.clear();

    std::string u = url;

    // 1. Strip fragment
    auto hashPos = u.find('#');
    if (hashPos != std::string::npos)
    {
      hash = u.substr(hashPos);   // includes '#'
      u    = u.substr(0, hashPos);
    }

    // 2. Strip query string
    auto qPos = u.find('?');
    if (qPos != std::string::npos)
    {
      search = u.substr(qPos);    // includes '?'
      u      = u.substr(0, qPos);

      // parse key=value pairs
      std::string qs = search.substr(1);
      size_t      start = 0;
      while (start < qs.size())
      {
        auto amp  = qs.find('&', start);
        std::string pair = (amp == std::string::npos)
                          ? qs.substr(start)
                          : qs.substr(start, amp - start);
        auto eq = pair.find('=');
        if (eq != std::string::npos)
          queryParams[pair.substr(0, eq)] = pair.substr(eq + 1);
        else if (!pair.empty())
          queryParams[pair] = "";
        start = (amp == std::string::npos) ? qs.size() : amp + 1;
      }
    }

    // 3. Extract scheme
    auto schemeEnd = u.find("://");
    if (schemeEnd != std::string::npos)
    {
      scheme = u.substr(0, schemeEnd);
      u      = u.substr(schemeEnd + 3);   // past "://"

      // 4. Extract host (up to the first '/')
      auto slashPos = u.find('/');
      if (slashPos != std::string::npos)
      {
        host     = u.substr(0, slashPos);
        pathname = u.substr(slashPos);    // includes leading '/'
      }
      else
      {
        host     = u;
        pathname = "";
      }
    }
    else
    {
      // bare path — no scheme or host
      pathname = u;
    }
  }
};

