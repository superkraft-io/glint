#pragma once

/**
 * glint_d3d_shader_cache.hpp
 * Compiled-shader cache for Skia's Direct3D backend, without changing Skia.
 *
 * Skia's D3D backend generates HLSL per draw configuration and compiles it
 * with D3DCompile every time a page first needs it (its persistent cache only
 * stores the HLSL text). D3DCompile takes 3-10 ms per shader, which is most
 * of the pause the first time a page with new effects opens.
 *
 * install() redirects this module's import of D3DCompile (Skia is linked into
 * the same module) to a cache keyed by the exact HLSL + entry point + target +
 * flags. Lookup order:
 *   1. memory (shaders compiled or loaded in this process),
 *   2. the shader pack shipped inside the binary (an RCDATA resource named
 *      GLINT_D3D_SHADERS; CMake: glint_embed_d3d_shaders()) and tables passed
 *      to registerEmbedded(),
 *   3. the disk cache (%LOCALAPPDATA%\Glint\ShaderCache\D3D, or
 *      GLINT_D3D_SHADER_CACHE_DIR),
 *   4. the real D3DCompile; the result is written to the disk cache.
 * Different HLSL (e.g. after a Skia upgrade) simply misses and compiles, so a
 * stale pack or cache can only be slower, never wrong. The GPU driver's own
 * compile of the bytecode still runs once per machine; drivers cache that.
 *
 * Environment:
 *   GLINT_D3D_SHADER_CACHE=0          off: every shader compiles, as in Skia.
 *   GLINT_D3D_SHADER_CACHE_DIR=<dir>  disk cache location.
 *   GLINT_D3D_SHADER_CAPTURE=<file>   adds every shader this process uses to
 *                                     the shader pack <file> (merged with what
 *                                     it already holds, rewritten as new ones
 *                                     appear), to embed in the next build.
 *                                     Delete the file to start over.
 *   GLINT_D3D_SHADER_DEBUG_INFO=1     keep Debug Skia's D3DCOMPILE_DEBUG |
 *                                     SKIP_OPTIMIZATION (for stepping through
 *                                     shaders in PIX). By default they are
 *                                     dropped: Debug builds then use the same
 *                                     optimized shaders (and pack) as Release.
 *
 * Shader pack format (little-endian): "GLSC", u32 version (1), u32 count,
 * then per shader: u64 key, u32 size, `size` bytes of DXBC. Sorted by key.
 */

#include <windows.h>
#include <d3dcompiler.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/** One compiled shader shipped in the binary. */
struct glint_embedded_d3d_shader
{
  uint64_t             key;    // glint_d3d_shader_cache::keyFor() of the compile request
  const unsigned char* data;   // DXBC
  uint32_t             size;
};

class glint_d3d_shader_cache
{
public:
  using d3d_compile_fn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

  /** Counters for telemetry. */
  struct stats
  {
    std::atomic<uint32_t> compiles{ 0 };       // real D3DCompile calls
    std::atomic<uint64_t> compileMicros{ 0 };  // time spent in them
    std::atomic<uint32_t> memoryHits{ 0 };
    std::atomic<uint32_t> embeddedHits{ 0 };
    std::atomic<uint32_t> diskHits{ 0 };
  };

  static stats& counters()
  {
    static stats s;
    return s;
  }

  /** Redirects D3DCompile for the module containing this code and loads the
   *  module's embedded shader pack. Idempotent; call before the first
   *  GrDirectContext::MakeDirect3D. */
  static void install()
  {
    static std::once_flag once;
    std::call_once(once, [] {
      if (const char* v = std::getenv("GLINT_D3D_SHADER_CACHE"); v && std::strcmp(v, "0") == 0)
        return;
      HMODULE self = nullptr;
      ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&glint_d3d_shader_cache::install), &self);
      if (!self) return;
      void* original = nullptr;
      if (!patchImport(self, "d3dcompiler_47.dll", "D3DCompile", reinterpret_cast<void*>(&cachedCompile), &original))
        return;
      state().realCompile = reinterpret_cast<d3d_compile_fn>(original);

      if (HRSRC res = ::FindResourceW(self, L"GLINT_D3D_SHADERS", MAKEINTRESOURCEW(10) /* RT_RCDATA */))
        if (HGLOBAL mem = ::LoadResource(self, res))
          registerPack(::LockResource(mem), ::SizeofResource(self, res));

      if (const char* capture = std::getenv("GLINT_D3D_SHADER_CAPTURE"); capture && *capture)
      {
        state().capturePath = std::filesystem::path(capture);
        std::ifstream in(state().capturePath, std::ios::binary);
        const std::vector<unsigned char> existing((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<glint_embedded_d3d_shader> entries;
        if (parsePack(existing.data(), existing.size(), entries))
          for (const auto& e : entries)
            state().captured.emplace(e.key, std::vector<unsigned char>(e.data, e.data + e.size));
      }
    });
  }

  /** Makes compiled shaders shipped in the binary available. `entries[i].data`
   *  must stay valid while rendering (normally static data). */
  static void registerEmbedded(const glint_embedded_d3d_shader* entries, size_t count)
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    for (size_t i = 0; i < count; ++i)
      state().embedded[entries[i].key] = entries[i];
  }

  /** Registers the shaders of a shader pack (see the format above). The bytes
   *  must stay valid while rendering (e.g. a resource). False when malformed. */
  static bool registerPack(const void* data, size_t size)
  {
    std::vector<glint_embedded_d3d_shader> entries;
    if (!parsePack(data, size, entries)) return false;
    registerEmbedded(entries.data(), entries.size());
    return true;
  }

  /** Parses a shader pack; entries point into `data`. */
  static bool parsePack(const void* data, size_t size, std::vector<glint_embedded_d3d_shader>& entries)
  {
    const auto* p   = static_cast<const unsigned char*>(data);
    const auto* end = p + size;
    auto read = [&p, end](void* out, size_t n) {
      if (static_cast<size_t>(end - p) < n) return false;
      std::memcpy(out, p, n);
      p += n;
      return true;
    };
    char magic[4];
    uint32_t version = 0, count = 0;
    if (!data || !read(magic, 4) || std::memcmp(magic, "GLSC", 4) != 0 || !read(&version, 4) || version != 1
        || !read(&count, 4))
      return false;
    entries.clear();
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
      glint_embedded_d3d_shader e{};
      if (!read(&e.key, 8) || !read(&e.size, 4) || static_cast<size_t>(end - p) < e.size) return false;
      e.data = p;
      p += e.size;
      entries.push_back(e);
    }
    return true;
  }

  /** Cache key of a compile request: FNV-1a 64 over the HLSL, entry point,
   *  target and flags. Not the compiler build: d3dcompiler_47 ships with
   *  Windows and differs between Windows builds, and bytecode from any of them
   *  is valid on all (a shipped pack must hit on every PC). */
  static uint64_t keyFor(const void* src, size_t srcSize, const char* entry, const char* target,
                         UINT flags1, UINT flags2)
  {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
      const auto* b = static_cast<const unsigned char*>(p);
      for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const uint64_t len = srcSize;
    mix(&len, sizeof len);
    mix(src, srcSize);
    mix(entry ? entry : "", std::strlen(entry ? entry : "") + 1);
    mix(target ? target : "", std::strlen(target ? target : "") + 1);
    mix(&flags1, sizeof flags1);
    mix(&flags2, sizeof flags2);
    return h;
  }

  /** Folder of the on-disk cache (empty when it can't be determined). */
  static const std::filesystem::path& diskDir()
  {
    static const std::filesystem::path dir = [] {
      if (const char* custom = std::getenv("GLINT_D3D_SHADER_CACHE_DIR"); custom && *custom)
        return std::filesystem::path(custom);
      wchar_t base[MAX_PATH] = {};
      const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
      if (n == 0 || n >= MAX_PATH) return std::filesystem::path();
      return std::filesystem::path(base) / L"Glint" / L"ShaderCache" / L"D3D";
    }();
    return dir;
  }

private:
  struct state_t
  {
    std::mutex                                                  mutex;
    d3d_compile_fn                                              realCompile = nullptr;
    std::unordered_map<uint64_t, std::vector<unsigned char>>    memory;
    std::unordered_map<uint64_t, glint_embedded_d3d_shader>     embedded;
    std::filesystem::path                                       capturePath;
    std::map<uint64_t, std::vector<unsigned char>>              captured;   // sorted: stable pack files
  };

  static state_t& state()
  {
    static state_t s;
    return s;
  }

  static std::filesystem::path diskFile(uint64_t key)
  {
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.dxbc", static_cast<unsigned long long>(key));
    return diskDir() / name;
  }

  static bool isDxbc(const std::vector<unsigned char>& bytes)
  {
    return bytes.size() >= 32 && std::memcmp(bytes.data(), "DXBC", 4) == 0;
  }

  static bool readDisk(uint64_t key, std::vector<unsigned char>& out)
  {
    if (diskDir().empty()) return false;
    std::ifstream in(diskFile(key), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return isDxbc(out);
  }

  // Writes a temp file, then renames it: readers never see a partial file.
  static void writeFileAtomic(const std::filesystem::path& finalPath, const void* data, size_t size)
  {
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    std::filesystem::path tmpPath = finalPath;
    tmpPath += "." + std::to_string(::GetCurrentProcessId()) + "." + std::to_string(::GetCurrentThreadId()) + ".tmp";
    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out) return;
      out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
      if (!out) { out.close(); std::filesystem::remove(tmpPath, ec); return; }
    }
    std::filesystem::rename(tmpPath, finalPath, ec);
    if (ec) std::filesystem::remove(tmpPath, ec);
  }

  // Capture mode: adds the shader and rewrites the pack. Caller holds the mutex.
  static void captureLocked(uint64_t key, const unsigned char* data, size_t size)
  {
    state_t& st = state();
    if (st.capturePath.empty() || st.captured.count(key)) return;
    st.captured.emplace(key, std::vector<unsigned char>(data, data + size));

    std::vector<unsigned char> pack;
    auto put = [&pack](const void* p, size_t n) {
      pack.insert(pack.end(), static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + n);
    };
    const uint32_t version = 1, count = static_cast<uint32_t>(st.captured.size());
    put("GLSC", 4);
    put(&version, 4);
    put(&count, 4);
    for (const auto& [k, bytes] : st.captured)
    {
      const uint32_t n = static_cast<uint32_t>(bytes.size());
      put(&k, 8);
      put(&n, 4);
      put(bytes.data(), bytes.size());
    }
    writeFileAtomic(st.capturePath, pack.data(), pack.size());
  }

  static HRESULT makeBlob(const void* data, size_t size, ID3DBlob** out)
  {
    using create_blob_fn = HRESULT(WINAPI*)(SIZE_T, ID3DBlob**);
    static const create_blob_fn createBlob = reinterpret_cast<create_blob_fn>(
      ::GetProcAddress(::GetModuleHandleW(L"d3dcompiler_47.dll"), "D3DCreateBlob"));
    if (!createBlob) return E_FAIL;
    const HRESULT hr = createBlob(size, out);
    if (SUCCEEDED(hr)) std::memcpy((*out)->GetBufferPointer(), data, size);
    return hr;
  }

  static HRESULT WINAPI cachedCompile(LPCVOID src, SIZE_T srcSize, LPCSTR sourceName,
                                      const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                                      LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                      ID3DBlob** code, ID3DBlob** errors)
  {
    state_t& st = state();
    // Macros and includes can change the output without changing the text:
    // Skia uses neither, so such calls just go to the compiler.
    if (defines || include || !code)
      return st.realCompile(src, srcSize, sourceName, defines, include, entry, target, flags1, flags2, code, errors);

    // Debug Skia asks for unoptimized shaders with debug info: ~5x larger
    // bytecode, slower on the GPU, and a separate set of pack entries.
    static const bool keepDebugInfo = [] {
      const char* v = std::getenv("GLINT_D3D_SHADER_DEBUG_INFO");
      return v && std::strcmp(v, "1") == 0;
    }();
    if (!keepDebugInfo)
      flags1 &= ~static_cast<UINT>(D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);

    const uint64_t key = keyFor(src, srcSize, entry, target, flags1, flags2);
    stats& c = counters();
    {
      std::lock_guard<std::mutex> lock(st.mutex);
      if (auto it = st.memory.find(key); it != st.memory.end())
      {
        ++c.memoryHits;
        if (errors) *errors = nullptr;
        return makeBlob(it->second.data(), it->second.size(), code);
      }
      if (auto it = st.embedded.find(key); it != st.embedded.end())
      {
        ++c.embeddedHits;
        if (errors) *errors = nullptr;
        captureLocked(key, it->second.data, it->second.size);
        return makeBlob(it->second.data, it->second.size, code);
      }
    }

    std::vector<unsigned char> bytes;
    if (readDisk(key, bytes))
    {
      ++c.diskHits;
      if (errors) *errors = nullptr;
      const HRESULT hr = makeBlob(bytes.data(), bytes.size(), code);
      std::lock_guard<std::mutex> lock(st.mutex);
      captureLocked(key, bytes.data(), bytes.size());
      st.memory.emplace(key, std::move(bytes));
      return hr;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const HRESULT hr = st.realCompile(src, srcSize, sourceName, defines, include, entry, target, flags1, flags2, code, errors);
    ++c.compiles;
    c.compileMicros += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
    if (SUCCEEDED(hr) && *code)
    {
      const auto* data = static_cast<const unsigned char*>((*code)->GetBufferPointer());
      const size_t size = (*code)->GetBufferSize();
      if (!diskDir().empty()) writeFileAtomic(diskFile(key), data, size);
      std::lock_guard<std::mutex> lock(st.mutex);
      captureLocked(key, data, size);
      st.memory.emplace(key, std::vector<unsigned char>(data, data + size));
    }
    return hr;
  }

  // Replaces `funcName` imported from `dllName` in `module`'s import table.
  static bool patchImport(HMODULE module, const char* dllName, const char* funcName, void* replacement, void** original)
  {
    auto* base = reinterpret_cast<BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;

    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
      if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dllName) != 0) continue;
      auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
      auto* names = imp->OriginalFirstThunk ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk) : nullptr;
      for (; thunk->u1.Function; ++thunk)
      {
        bool match = false;
        if (names)
        {
          if (!IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
          {
            const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            match = std::strcmp(reinterpret_cast<const char*>(byName->Name), funcName) == 0;
          }
          ++names;
        }
        if (!match) continue;

        DWORD oldProtect = 0;
        if (!::VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
        *original = reinterpret_cast<void*>(thunk->u1.Function);
        thunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
        ::VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &oldProtect);
        return true;
      }
    }
    return false;
  }
};
