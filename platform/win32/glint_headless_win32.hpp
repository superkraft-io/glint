#pragma once

/**
 * glint_headless_win32.hpp
 * Headless mode for every Glint app on Windows: run the app's own document
 * without showing a window, drive it from a small script, and save
 * screenshots and memory figures. For tests, CI and before/after
 * comparisons that should not take over the screen.
 *
 * Start an app with
 *   app.exe --glint-headless <script> [--glint-headless-out <dir>]
 *           [--glint-headless-compare <dir>]
 * (or GLINT_HEADLESS / GLINT_HEADLESS_OUT / GLINT_HEADLESS_COMPARE). The
 * window is created hidden and builds its UI as usual; instead of its own
 * renderer it draws into an offscreen surface: Direct3D 12 by default (same
 * device setup, allocator and shader cache as a window, without a
 * swapchain), the CPU with GLINT_RENDERER=cpu. The app exits when the script
 * ends.
 *
 * Script: one command per line, `#` starts a comment. Coordinates and sizes
 * are logical (CSS) pixels.
 *   size W H [DPR]    document size and device pixel ratio
 *   wait [MS]         until settled: no tasks, layout, redraw, img decode or
 *                     animation left (default timeout 10000 ms). Animations
 *                     run on a virtual clock, one 60 Hz step per frame;
 *                     endless ones (and anything else redrawing
 *                     forever, e.g. a spinner) end the wait after 120 frames.
 *   shot NAME         render a frame and save <out>/NAME.png; with a compare
 *                     dir, also compare it with <compare>/NAME.png and save
 *                     <out>/NAME.diff.png when they differ
 *   mem LABEL         private bytes, working set, GPU cache, decoded imgs
 *   move X Y          mouse moves to X Y
 *   click X Y         left click at X Y
 *   scroll X Y DY     mouse wheel at X Y, DY px (positive = down)
 *   frames N          redraw everything N times (default 60): average CPU
 *                     draw time and whole frame time, in the report
 *   trim              what a window does when it goes idle: frees unused
 *                     GPU resources and imgs
 *   sleep MS
 *   exit
 * Anything else goes to the app (glint_window_win32::onHeadlessCommand()),
 * e.g. the demo's `page Masks`.
 *
 * Results go to <out>/report.txt: one line per command (errors, compare
 * results, memory) and a final "result ok" or "result failed".
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <shellapi.h>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/codec/SkCodec.h"
#include "include/encode/SkPngEncoder.h"

#include "glint_win32_surface_shared.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace glint_headless
{

struct config
{
  bool                  enabled = false;
  std::filesystem::path script;
  std::filesystem::path outDir;
  std::filesystem::path compareDir;   // empty: no comparison
};

/** From the command line (--glint-headless ...) or the environment; read once. */
inline const config& settings()
{
  static const config c = [] {
    config r;
    auto env = [](const wchar_t* name) -> std::wstring {
      wchar_t value[1024] = {};
      const DWORD n = ::GetEnvironmentVariableW(name, value, 1024);
      return n > 0 && n < 1024 ? std::wstring(value, n) : std::wstring();
    };
    std::wstring script = env(L"GLINT_HEADLESS"), out = env(L"GLINT_HEADLESS_OUT"), compare = env(L"GLINT_HEADLESS_COMPARE");
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc))
    {
      for (int i = 1; i + 1 < argc; ++i)
      {
        const std::wstring a = argv[i];
        if (a == L"--glint-headless")              script  = argv[++i];
        else if (a == L"--glint-headless-out")     out     = argv[++i];
        else if (a == L"--glint-headless-compare") compare = argv[++i];
      }
      ::LocalFree(argv);
    }
    if (script.empty()) return r;
    r.enabled = true;
    r.script  = std::filesystem::absolute(script);
    r.outDir  = out.empty() ? r.script.parent_path() / (r.script.stem().wstring() + L"_out")
                            : std::filesystem::absolute(out);
    if (!compare.empty()) r.compareDir = std::filesystem::absolute(compare);
    return r;
  }();
  return c;
}

inline bool enabled() { return settings().enabled; }

/** True once: for the first window created, which runs the script. Other
 *  windows of the process (popups, dropdowns) stay hidden and undrawn. */
inline bool claimScript()
{
  static std::atomic<bool> claimed{ false };
  return !claimed.exchange(true);
}

/** Number of imgs being decoded in the background. */
inline size_t pendingImageDecodes()
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  return glint_img_pending().size();
}

/** Bytes held by decoded imgs (glint_img_cache()). */
inline size_t decodedImageBytes()
{
  std::lock_guard<std::mutex> lock(gGlintImgCacheMutex);
  size_t total = 0;
  for (const auto& [key, img] : glint_img_cache())
    if (img) total += glint_img_bytes(*img);
  return total;
}

// ── Offscreen renderer ──────────────────────────────────────────────────────

class offscreen_renderer
{
public:

  /** Direct3D 12 unless `cpu` (or the GPU fails): then a raster surface. */
  void init(bool cpu)
  {
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
    if (!cpu)
    {
      mDevice = glint_win32_surface::createDirect3DDevice();
      if (mDevice.result == glint_win32_surface::direct3d_init_result::success && mDevice.grContext)
        mBudget.attach(mDevice.allocator);
      else
        mDevice = {};
    }
#else
    (void)cpu;
#endif
  }

  bool        isGpu() const { return context() != nullptr; }
  const char* name()  const { return isGpu() ? "Direct3D 12" : "CPU"; }

  /** Canvas for a frame of w x h device pixels; nullptr on failure. */
  SkCanvas* begin(int w, int h)
  {
    w = std::max(1, w); h = std::max(1, h);
    if (!mSurface || w != mW || h != mH)
    {
      mSurface.reset();
      // GPU: RGBA like a window's swapchain, so the same pipelines are used
      // (a captured shader pack then holds what windows need).
      if (GrDirectContext* ctx = context())
        mSurface = SkSurfaces::RenderTarget(ctx, skgpu::Budgeted::kNo,
                                            SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType));
      if (!mSurface) mSurface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
      mW = w; mH = h;
    }
    return mSurface ? mSurface->getCanvas() : nullptr;
  }

  /** Finishes the frame (waits for the GPU). */
  void end()
  {
    if (GrDirectContext* ctx = context())
    {
      ctx->flushAndSubmit(mSurface.get(), GrSyncCpu::kYes);
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
      mBudget.afterFrame(*ctx);
#endif
    }
  }

  bool readPixels(SkBitmap& out)
  {
    if (!mSurface || !out.tryAllocPixels(SkImageInfo::MakeN32Premul(mW, mH))) return false;
    return mSurface->readPixels(out, 0, 0);
  }

  /** As a window's renderer when it goes idle. */
  void trimIdle()
  {
    if (GrDirectContext* ctx = context())
    {
      ctx->performDeferredCleanup(std::chrono::seconds(3));
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
      mBudget.afterIdleCleanup(*ctx);
#endif
    }
  }

  size_t gpuCacheBytes() const
  {
    size_t bytes = 0;
    if (GrDirectContext* ctx = context()) ctx->getResourceCacheUsage(nullptr, &bytes);
    return bytes;
  }

private:
  GrDirectContext* context() const
  {
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
    return mDevice.grContext.get();
#else
    return nullptr;
#endif
  }

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
  glint_win32_surface::direct3d_device  mDevice;
  glint_win32_surface::gpu_cache_budget mBudget;
#endif
  sk_sp<SkSurface> mSurface;
  int              mW = 0, mH = 0;
};

// ── Animation clock ─────────────────────────────────────────────────────────
// Transitions and @keyframes run on a virtual clock that advances one 60 Hz
// frame per frame drawn (and by `sleep`): a run is as fast as the frames
// can be drawn, and screenshots of animated pages are the same every run.

inline std::chrono::steady_clock::time_point& virtualTime()
{
  static std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  return now;
}
inline std::chrono::steady_clock::time_point virtualNow() { return virtualTime(); }
inline void advanceVirtualTime(std::chrono::milliseconds ms) { virtualTime() += ms; }

// ── Script runner ───────────────────────────────────────────────────────────

/** What the runner needs from the window it runs in. */
struct host
{
  virtual ~host() = default;
  virtual glint_document* headlessDocument() = 0;
  /** Document size (logical px) and device pixel ratio. */
  virtual void headlessResize(int w, int h, float dpr) = 0;
  virtual void headlessSize(int& w, int& h, float& dpr) const = 0;
  /** Draws the document onto a cleared canvas of the device size. */
  virtual void headlessDraw(SkCanvas& canvas, const char* backendName, bool gpu) = 0;
  /** True when the document asked for a redraw since the last call. */
  virtual bool headlessTakeRedrawRequest() = 0;
  /** An app command; false (with `error`) when unknown or failed. */
  virtual bool headlessCommand(const std::vector<std::string>& args, std::string& error) = 0;
};

class runner
{
public:
  runner(host& h, const config& c) : mHost(h), mConfig(c) {}

  /** Runs the script; true when every command succeeded. */
  bool run()
  {
    std::error_code ec;
    std::filesystem::create_directories(mConfig.outDir, ec);
    mReport.open(mConfig.outDir / L"report.txt", std::ios::trunc);

    std::ifstream in(mConfig.script);
    if (!in) { line(0, "error: cannot read script " + mConfig.script.string()); return finish(); }

    const char* cpu = std::getenv("GLINT_RENDERER");
    mRenderer.init(cpu && std::string(cpu) == "cpu");
    line(0, std::string("renderer ") + mRenderer.name());
    gGlintAnimationClock = &virtualNow;

    std::string text;
    int number = 0;
    while (std::getline(in, text))
    {
      ++number;
      if (const size_t hash = text.find('#'); hash != std::string::npos) text.erase(hash);
      std::vector<std::string> args;
      std::istringstream words(text);
      for (std::string w; words >> w;) args.push_back(w);
      if (args.empty()) continue;
      if (args[0] == "exit") break;
      std::string result;
      const auto started = std::chrono::steady_clock::now();
      const bool ok = execute(args, result);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      if (!ok) ++mErrors;
      char took[32];
      std::snprintf(took, sizeof(took), "  [%.1f ms]", ms);
      line(number, text + (result.empty() ? "" : "  -> " + result) + (ok ? "" : "  [error]") + took);
    }
    return finish();
  }

private:
  bool execute(const std::vector<std::string>& a, std::string& result)
  {
    glint_document* doc = mHost.headlessDocument();
    if (!doc) { result = "no document"; return false; }
    const std::string& cmd = a[0];
    auto num = [&](size_t i, float fallback) {
      if (i >= a.size()) return fallback;
      try { return std::stof(a[i]); } catch (...) { return fallback; }
    };

    if (cmd == "size" && a.size() >= 3)
    {
      int w = 0, h = 0; float dpr = 1.f;
      mHost.headlessSize(w, h, dpr);
      mHost.headlessResize(static_cast<int>(num(1, float(w))), static_cast<int>(num(2, float(h))), num(3, dpr));
      return true;
    }
    if (cmd == "wait")
    {
      const auto started = std::chrono::steady_clock::now();
      frame_times times;
      const bool settled = settle(static_cast<int>(num(1, 10000.f)), result, &times);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
      char buf[200];
      std::snprintf(buf, sizeof(buf), "; %d frames, first %.1f ms (style %.1f, layout %.1f, paint %.1f, gpu %.1f), slowest %.1f ms",
                    times.count, times.firstMs, times.styleMs, times.layoutMs, times.paintMs, times.gpuMs, times.slowestMs);
      result = (settled ? "settled in " : "timed out after ") + std::to_string(ms) + " ms" + buf
             + (result.empty() ? "" : " (" + result + ")");
      return settled;
    }
    if (cmd == "shot" && a.size() >= 2) return shot(a[1], result);
    if (cmd == "mem")
    {
      PROCESS_MEMORY_COUNTERS_EX pm = {};
      pm.cb = sizeof(pm);
      ::K32GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
      char buf[256];
      std::snprintf(buf, sizeof(buf), "private_mb=%.1f working_set_mb=%.1f gpu_cache_mb=%.1f images_mb=%.1f",
                    pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0,
                    mRenderer.gpuCacheBytes() / 1048576.0, decodedImageBytes() / 1048576.0);
      result = buf;
      return true;
    }
    if ((cmd == "move" || cmd == "click") && a.size() >= 3)
    {
      const float x = num(1, 0.f), y = num(2, 0.f);
      doc->OnMouseOver(x, y, glint_mouse_mod{});
      if (cmd == "click")
      {
        glint_mouse_mod left{};
        left.L = true;
        doc->OnMouseDown(x, y, left);
        doc->OnMouseUp(x, y, left);
      }
      return true;
    }
    if (cmd == "scroll" && a.size() >= 4)
    {
      doc->OnMouseWheel(num(1, 0.f), num(2, 0.f), 0.f, num(3, 0.f), glint_mouse_mod{});
      return true;
    }
    if (cmd == "frames")
    {
      // Redraws the whole document N times: average CPU time drawing it and
      // whole frame time (including waiting for the GPU).
      const int n = std::max(1, static_cast<int>(num(1, 60.f)));
      double drawMs = 0.0;
      const auto started = std::chrono::steady_clock::now();
      for (int i = 0; i < n; ++i)
      {
        double ms = 0.0;
        frame(nullptr, &ms);
        drawMs += ms;
      }
      const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
      char buf[160];
      std::snprintf(buf, sizeof(buf), "draw_ms=%.3f frame_ms=%.3f (average of %d)", drawMs / n, totalMs / n, n);
      result = buf;
      return true;
    }
    if (cmd == "trim")
    {
      mRenderer.trimIdle();
      glint_trim_image_cache();
      return true;
    }
    if (cmd == "sleep")
    {
      const std::chrono::milliseconds ms(static_cast<int>(num(1, 0.f)));
      std::this_thread::sleep_for(ms);
      advanceVirtualTime(ms);
      return true;
    }
    if (!mHost.headlessCommand(a, result))
    {
      if (result.empty()) result = "unknown command";
      return false;
    }
    return true;
  }

  /** Draws a frame; reads it back into `pixels` when given. `drawMs`: CPU
   *  time spent drawing the document. */
  bool frame(SkBitmap* pixels, double* drawMs = nullptr, double* gpuMs = nullptr)
  {
    int w = 0, h = 0; float dpr = 1.f;
    mHost.headlessSize(w, h, dpr);
    SkCanvas* canvas = mRenderer.begin(static_cast<int>(std::lround(w * dpr)), static_cast<int>(std::lround(h * dpr)));
    if (!canvas) return false;
    const auto drawStart = std::chrono::steady_clock::now();
    mHost.headlessDraw(*canvas, mRenderer.name(), mRenderer.isGpu());
    if (drawMs) *drawMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawStart).count();
    const auto endStart = std::chrono::steady_clock::now();
    mRenderer.end();
    if (gpuMs) *gpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - endStart).count();
    advanceVirtualTime(std::chrono::milliseconds(16));
    return !pixels || mRenderer.readPixels(*pixels);
  }

  /** Draws frames until nothing is left to do, or `timeoutMs`. Something
   *  that redraws forever (a looping animation, a spinner) stops the wait
   *  after kMaxBusyFrames: that counts as settled (noted in `note`). */
  struct frame_times
  {
    int count = 0;
    double firstMs = 0.0, slowestMs = 0.0;
    // The first frame's phases: style (cascade + transitions), layout, paint
    // traversal, and the GPU (flush + wait: pipeline creation, uploads).
    double styleMs = 0.0, layoutMs = 0.0, paintMs = 0.0, gpuMs = 0.0;
  };

  bool settle(int timeoutMs, std::string& note, frame_times* times = nullptr)
  {
    static constexpr int kMaxBusyFrames = 120;
    glint_document* doc = mHost.headlessDocument();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool first = true;
    int quiet = 0, busyFrames = 0;
    for (;;)
    {
      if (const auto& queue = doc->taskQueue()) queue->drain();
      const bool animating = doc->mCanvas.hasActiveAnimationSubtree();
      const bool redraw = mHost.headlessTakeRedrawRequest() || doc->mLayoutDirty || first || animating;
      first = false;
      if (redraw)
      {
        const auto frameStart = std::chrono::steady_clock::now();
        double gpuMs = 0.0;
        frame(nullptr, nullptr, &gpuMs);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        if (times)
        {
          if (times->count++ == 0)
          {
            const glint_frame_stats& st = doc->lastFrameStats();
            times->firstMs  = ms;
            times->styleMs  = st.cascadeMs + st.tickMs;
            times->layoutMs = st.layoutMs;
            times->paintMs  = st.paintMs;
            times->gpuMs    = gpuMs;
          }
          times->slowestMs = std::max(times->slowestMs, ms);
        }
      }
      const size_t decoding = pendingImageDecodes();
      if (redraw && decoding == 0 && ++busyFrames >= kMaxBusyFrames)
      {
        note = std::string(animating ? "animations" : "redraws") + " still running after "
             + std::to_string(kMaxBusyFrames) + " frames";
        return true;
      }
      if (!redraw && decoding == 0)
      {
        if (++quiet >= 2) return true;
      }
      else
        quiet = 0;
      if (std::chrono::steady_clock::now() >= deadline)
      {
        note = decoding ? "imgs decoding" : "redraws requested";
        return false;
      }
      if (!redraw) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  bool shot(const std::string& name, std::string& result)
  {
    SkBitmap pixels;
    if (!frame(&pixels)) { result = "render failed"; return false; }
    const std::filesystem::path file = mConfig.outDir / (name + ".png");
    if (!writePng(file, pixels)) { result = "cannot write " + file.string(); return false; }
    result = file.filename().string();
    if (mConfig.compareDir.empty()) return true;

    const std::filesystem::path refFile = mConfig.compareDir / (name + ".png");
    SkBitmap ref;
    if (!readPng(refFile, ref)) { result += ", no reference " + refFile.string(); return false; }
    if (ref.width() != pixels.width() || ref.height() != pixels.height())
    {
      result += ", size differs from reference";
      return false;
    }
    size_t differ = 0, differMore = 0;
    int maxDelta = 0, l = pixels.width(), t = pixels.height(), r = -1, b = -1;
    SkBitmap diff;
    diff.allocN32Pixels(pixels.width(), pixels.height());
    for (int y = 0; y < pixels.height(); ++y)
      for (int x = 0; x < pixels.width(); ++x)
      {
        const SkColor p = pixels.getColor(x, y), q = ref.getColor(x, y);
        const int d = std::max({ std::abs(int(SkColorGetR(p)) - int(SkColorGetR(q))),
                                 std::abs(int(SkColorGetG(p)) - int(SkColorGetG(q))),
                                 std::abs(int(SkColorGetB(p)) - int(SkColorGetB(q))),
                                 std::abs(int(SkColorGetA(p)) - int(SkColorGetA(q))) });
        *diff.getAddr32(x, y) = d ? SkPreMultiplyColor(SK_ColorRED)
                                  : SkPreMultiplyColor(SkColorSetRGB(SkColorGetR(p) / 3, SkColorGetG(p) / 3, SkColorGetB(p) / 3));
        if (!d) continue;
        ++differ;
        if (d > 2) ++differMore;
        maxDelta = std::max(maxDelta, d);
        l = std::min(l, x); t = std::min(t, y); r = std::max(r, x); b = std::max(b, y);
      }
    if (differ == 0) { result += ", identical"; return true; }
    writePng(mConfig.outDir / (name + ".diff.png"), diff);
    char buf[200];
    std::snprintf(buf, sizeof(buf), ", differs: %zu px (%zu by >2), max delta %d, box %d,%d-%d,%d",
                  differ, differMore, maxDelta, l, t, r, b);
    result += buf;
    // Within 2/255 (anti-aliasing / rounding): reported, not an error.
    return differMore == 0;
  }

  static bool writePng(const std::filesystem::path& file, const SkBitmap& bitmap)
  {
    SkFILEWStream stream(file.string().c_str());
    SkPixmap pixmap;
    return stream.isValid() && bitmap.peekPixels(&pixmap) && SkPngEncoder::Encode(&stream, pixmap, {});
  }

  /** Decodes straight into `out` (an SkImage would keep the decoded pixels
   *  in Skia's global cache, which `mem` then counts). */
  static bool readPng(const std::filesystem::path& file, SkBitmap& out)
  {
    sk_sp<SkData> data = SkData::MakeFromFileName(file.string().c_str());
    std::unique_ptr<SkCodec> codec = data ? SkCodec::MakeFromData(std::move(data)) : nullptr;
    if (!codec) return false;
    const SkImageInfo info = SkImageInfo::MakeN32Premul(codec->dimensions());
    return out.tryAllocPixels(info) && codec->getPixels(out.pixmap()) == SkCodec::kSuccess;
  }

  void line(int number, const std::string& text)
  {
    if (number > 0) mReport << number << ": ";
    mReport << text << "\n";
    mReport.flush();
  }

  bool finish()
  {
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU && defined(GLINT_ENABLE_D3D12) && GLINT_ENABLE_D3D12 && defined(SK_DIRECT3D)
    if (mRenderer.isGpu())
    {
      const auto& sc = glint_d3d_shader_cache::counters();
      line(0, "shader cache: programs " + std::to_string(sc.programHits.load()) + " cached / "
              + std::to_string(sc.programMisses.load()) + " generated");
    }
#endif
    line(0, mErrors == 0 ? "result ok" : "result failed (" + std::to_string(mErrors) + " errors)");
    mReport.close();
    return mErrors == 0;
  }

  host&             mHost;
  const config&     mConfig;
  offscreen_renderer mRenderer;
  std::ofstream     mReport;
  int               mErrors = 0;
};

} // namespace glint_headless
