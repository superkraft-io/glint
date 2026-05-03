#pragma once

#include "glint_resource_request.hpp"


#include <mutex>
#include <string>
#include <unordered_map>

struct GlintResourceCacheKey
{
  std::string                  url;
  glint_resource_request::Type type = glint_resource_request::Type::Unknown;

  bool operator==(const GlintResourceCacheKey& other) const
  {
    return type == other.type && url == other.url;
  }
};

struct GlintResourceCacheKeyHash
{
  size_t operator()(const GlintResourceCacheKey& key) const
  {
    const size_t h1 = std::hash<std::string>{}(key.url);
    const size_t h2 = std::hash<int>{}(static_cast<int>(key.type));
    return (h1 * 1315423911u) ^ h2;
  }
};

struct GlintCachedResource
{
  int           statusCode = 0;
  std::string   statusMessage;
  sk_sp<SkData> data;
  std::string   resolvedFilePath;

  bool ok() const
  {
    return statusCode == 200 && data != nullptr;
  }
};

inline std::unordered_map<GlintResourceCacheKey, GlintCachedResource, GlintResourceCacheKeyHash>&
glint_resource_cache()
{
  static std::unordered_map<GlintResourceCacheKey, GlintCachedResource, GlintResourceCacheKeyHash> cache;
  return cache;
}

inline std::mutex& glint_resource_cache_mutex()
{
  static std::mutex cacheMutex;
  return cacheMutex;
}

inline bool glint_resource_cache_lookup(
    const std::string&           url,
    glint_resource_request::Type type,
    GlintCachedResource*         out = nullptr)
{
  std::lock_guard<std::mutex> lock(glint_resource_cache_mutex());
  auto& cache = glint_resource_cache();
  auto it = cache.find({ url, type });
  if (it == cache.end()) return false;
  if (out) *out = it->second;
  return true;
}

inline void glint_resource_cache_store(
    const std::string&            url,
    glint_resource_request::Type  type,
    const glint_resource_request& req)
{
  std::lock_guard<std::mutex> lock(glint_resource_cache_mutex());
  glint_resource_cache()[{ url, type }] = {
    req.statusCode,
    req.statusMessage,
    req.responseData,
    req.resolvedFilePath
  };
}

inline void glint_resource_cache_store_disk(
    const std::string&           url,
    glint_resource_request::Type type,
    const sk_sp<SkData>&         data,
    const std::string&           resolvedFilePath = {})
{
  std::lock_guard<std::mutex> lock(glint_resource_cache_mutex());
  glint_resource_cache()[{ url, type }] = {
    data ? 200 : 404,
    data ? "OK" : "File not found",
    data,
    resolvedFilePath
  };
}
