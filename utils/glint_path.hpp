#pragma once

/**
 * glint_path.hpp
 * Path helpers — cross-platform equivalent of Node's __dirname.
 *
 * glint_exe_dir()   → directory that contains the current executable / DLL.
 * glint_res_dir()   → glint_exe_dir() / "resources"
 *
 * Usage:
 *   #include "glint/utils/glint_path.hpp"
 *
 *   // Capture once; lambdas capture by value cheaply (path is ~small string).
 *   auto dir = glint_exe_dir();
 *
 *   body->mRoot.onRequest = [dir](glint_resource_request& req) {
 *     // req.pathname = "/resources/img/logo.png"
 *     req.fromFile(...);
 *   };
 */

#include <filesystem>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <dlfcn.h>
#  include <limits.h>
#elif defined(__linux__)
#  include <unistd.h>
#  include <dlfcn.h>
#  include <limits.h>
#endif

// ── glint_exe_dir ──────────────────────────────────────────────────────────────
/** Returns the directory containing the current executable (or DLL on Win32).
 *  @param module  Win32 only: HMODULE of the DLL.  Pass nullptr for the EXE. */
#if defined(_WIN32)
inline std::filesystem::path glint_exe_dir(HMODULE module = nullptr)
{
  char buf[MAX_PATH] = {};
  GetModuleFileNameA(module, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}
#elif defined(__APPLE__)
inline std::filesystem::path glint_exe_dir()
{
  char buf[PATH_MAX] = {};
  uint32_t size = PATH_MAX;
  if (_NSGetExecutablePath(buf, &size) == 0)
    return std::filesystem::path(buf).parent_path();
  return std::filesystem::current_path();
}
#else  // Linux / generic POSIX
inline std::filesystem::path glint_exe_dir()
{
  char buf[PATH_MAX] = {};
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) return std::filesystem::path(buf).parent_path();
  return std::filesystem::current_path();
}
#endif

// ── glint_res_dir ──────────────────────────────────────────────────────────────
/** Returns <exeDir> / "resources". */
#if defined(_WIN32)
inline std::filesystem::path glint_res_dir(HMODULE module = nullptr)
{
  return glint_exe_dir(module) / "resources";
}
#else
inline std::filesystem::path glint_res_dir()
{
  return glint_exe_dir() / "resources";
}
#endif

// ── glint_self_dir ───────────────────────────────────────────────────────────────
/** Returns the directory containing the binary (exe, dylib, or DLL) that
 *  contains this code — resolved at runtime via dladdr / GetModuleHandleEx.
 *  Safe to use from plugins where glint_exe_dir() would return the host DAW. */
#if defined(_WIN32)
inline std::filesystem::path glint_self_dir()
{
  HMODULE hm = nullptr;
  GetModuleHandleExA(
    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
    reinterpret_cast<LPCSTR>(&glint_self_dir),
    &hm);
  char buf[MAX_PATH] = {};
  GetModuleFileNameA(hm, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}
#else
inline std::filesystem::path glint_self_dir()
{
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(&glint_self_dir), &info) && info.dli_fname)
    return std::filesystem::path(info.dli_fname).parent_path();
  return std::filesystem::current_path();
}
#endif

// ── glint_assets_dir / glint_assets_path ──────────────────────────────────────
// INTERNAL — used by the glint inspector for img previews.
// These functions walk up from the compile-time __FILE__ path to find a
// sibling "glint_assets" directory and only work when the source tree is
// present on disk.  They are NOT part of the public glint API.
//
// In developer onRequest() handlers, write your own path or memory logic:
//   req.fromFile(myAssetsDir / relative_path);
//   req.fromMemory(embeddedData, size);   // for bundled/installed apps
inline std::filesystem::path glint_assets_dir()
{
    static const std::filesystem::path cached = []() -> std::filesystem::path {
        std::filesystem::path cur =
            std::filesystem::path(__FILE__).parent_path();
        const std::filesystem::path root = cur.root_path();
        while (cur != root) {
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(cur, ec))
            {
                if (entry.is_directory(ec) &&
                    entry.path().filename().string() == "glint_assets")
                {
                    return entry.path();
                }
            }
            cur = cur.parent_path();
        }
        return {}; // not found
    }();
    return cached;
}

inline std::filesystem::path glint_assets_path(const std::string& pathname)
{
    std::string p = pathname;
    while (!p.empty() && (p.front() == '/' || p.front() == '\\'))
        p.erase(p.begin());
    return glint_assets_dir() / p;
}
