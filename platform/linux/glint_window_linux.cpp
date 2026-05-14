/**
 * glint_window_linux.cpp
 * Linux (X11/Xlib) implementation of glint_window_base.
 *
 * Rendering strategy (selected at compile time via GLINT_RENDER_GPU):
 *
 *   CPU (default / GLINT_RENDER_GPU=OFF):
 *     Skia SkBitmap (kN32 = BGRA_8888 on LE) → XPutImage → X11 window.
 *     On little-endian x86-64, BGRA_8888 maps directly to X11's typical
 *     visual (red_mask=0xFF0000, green_mask=0xFF00, blue_mask=0xFF) with
 *     byte_order=LSBFirst — no pixel-format conversion required.
 *
 *   GPU (GLINT_RENDER_GPU=ON):
 *     EGL (on X11 platform) + OpenGL ES 2 (or desktop GL) context.
 *     Skia Ganesh renders into a SkSurface wrapping FBO 0, then
 *     eglSwapBuffers presents the frame.
 */

#include "glint_window_linux.hpp"

// Xlib — must come after our own headers to avoid macro pollution
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/keysym.h>

// POSIX / Linux
#include <sys/timerfd.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>

// Standard C/C++
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

// Skia CPU surface
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
// EGL — present on every Mesa-based Linux / WSLg installation
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
// Skia Ganesh / GL headers
#  include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#  include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#  include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#  include "include/gpu/ganesh/SkSurfaceGanesh.h"
#  include "include/gpu/ganesh/GrBackendSurface.h"
#endif

// Inspector (if enabled)
#ifndef GLINT_INSPECTOR_DISABLED
#  include "../../components/glint_builder.hpp"
#  include "../../inspector/window.hpp"
#endif

// ─── Internal helpers ─────────────────────────────────────────────────────────

static int keysymToVK(KeySym sym)
{
  switch (sym)
  {
    case XK_Left:      return 0x25;
    case XK_Right:     return 0x27;
    case XK_Up:        return 0x26;
    case XK_Down:      return 0x28;
    case XK_Home:      return 0x24;
    case XK_End:       return 0x23;
    case XK_Prior:     return 0x21;   // Page Up
    case XK_Next:      return 0x22;   // Page Down
    case XK_BackSpace: return 0x08;
    case XK_Delete:    return 0x2E;
    case XK_Return:
    case XK_KP_Enter:  return 0x0D;
    case XK_Escape:    return 0x1B;
    case XK_Tab:       return 0x09;
    default:           break;
  }
  // ASCII printable — map lowercase to uppercase VK (matching Win32 convention)
  if (sym >= 'a' && sym <= 'z') return static_cast<int>(sym - 32);
  if (sym >= 'A' && sym <= 'Z') return static_cast<int>(sym);
  if (sym >= '0' && sym <= '9') return static_cast<int>(sym);
  if (sym >= 0x20 && sym <= 0x7E) return static_cast<int>(sym);
  return 0;
}

// ─── glint_window_linux member implementations ────────────────────────────────

glint_window_linux::~glint_window_linux()
{
  stopThread();
}

// ── Public API ─────────────────────────────────────────────────────────────────

void glint_window_linux::startThread()
{
  if (mRunning.load(std::memory_order_relaxed))
    return;

  mW  = defaultWidth();
  mH  = defaultHeight();
  mWpx = mW;
  mHpx = mH;

  // XInitThreads() must be called before any other Xlib call when using
  // Xlib from multiple threads.  Harmless if called multiple times.
  XInitThreads();

  mThread = std::thread([this]{ run(); });
  mThread.detach();

  // Wait up to 3 s for the window to be mapped
  for (int i = 0; i < 3000 && mXWindow == 0; ++i)
    ::usleep(1000);
}

void glint_window_linux::stopThread()
{
  mRunning.store(false, std::memory_order_relaxed);
  if (mWakeFd[1] != -1)
  {
    const char c = 'Q';
    (void)::write(mWakeFd[1], &c, 1);
  }
}

void glint_window_linux::requestRedraw()
{
  mRedrawRequested.store(true, std::memory_order_relaxed);
  if (mWakeFd[1] != -1)
  {
    const char c = 'R';
    (void)::write(mWakeFd[1], &c, 1);
  }
}

void glint_window_linux::openFileInDefaultApp(const std::string& path)
{
  // xdg-open is the standard way to open files on Linux desktops.
  std::string cmd = "xdg-open ";
  // Simple shell-quote the path (single-quotes, escape existing single-quotes)
  cmd += "'";
  for (char c : path)
  {
    if (c == '\'') cmd += "'\\''";
    else           cmd += c;
  }
  cmd += "' &";
  (void)::system(cmd.c_str());
}

// ── Screen coordinate helpers ─────────────────────────────────────────────────

RECT glint_window_linux::contentRectToScreen(float x, float y, float w, float h) const
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow) return {};

  int sx = 0, sy = 0;
  ::Window child = 0;
  XTranslateCoordinates(dpy,
                        static_cast<::Window>(mXWindow),
                        DefaultRootWindow(dpy),
                        static_cast<int>(x),
                        static_cast<int>(y),
                        &sx, &sy, &child);
  return RECT{ sx, sy, sx + static_cast<int>(w), sy + static_cast<int>(h) };
}

// ── Popup panel helpers ───────────────────────────────────────────────────────

void glint_window_linux::showPanel()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow) return;
  mSuppressAutoClose = false;
  XMapRaised(dpy, static_cast<Window>(mXWindow));
  XFlush(dpy);
  requestRedraw();
}

void glint_window_linux::hidePanel()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow) return;
  mSuppressAutoClose = true;
  XUnmapWindow(dpy, static_cast<Window>(mXWindow));
  XFlush(dpy);
}

void glint_window_linux::setPanelFrameOrigin(int x, int y)
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow) return;
  XMoveWindow(dpy, static_cast<Window>(mXWindow), x, y);
  XFlush(dpy);
}

/*static*/ RECT glint_window_linux::screenWorkArea()
{
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) return RECT{ 0, 0, 1920, 1080 };

  const int screen = DefaultScreen(dpy);
  const int sw = DisplayWidth(dpy, screen);
  const int sh = DisplayHeight(dpy, screen);
  RECT result{ 0, 0, sw, sh };

  // Try _NET_WORKAREA (excludes taskbar/panel)
  const Atom netWorkArea = XInternAtom(dpy, "_NET_WORKAREA", True);
  if (netWorkArea != None)
  {
    Atom           actualType;
    int            actualFormat;
    unsigned long  nItems, bytesAfter;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), netWorkArea,
                           0, 4, False, XA_CARDINAL,
                           &actualType, &actualFormat,
                           &nItems, &bytesAfter, &data) == Success
        && data && nItems >= 4)
    {
      const unsigned long* wa = reinterpret_cast<const unsigned long*>(data);
      result = RECT{ static_cast<int>(wa[0]),
                     static_cast<int>(wa[1]),
                     static_cast<int>(wa[0] + wa[2]),
                     static_cast<int>(wa[1] + wa[3]) };
    }
    if (data) XFree(data);
  }

  XCloseDisplay(dpy);
  return result;
}

// ── Internal helpers ───────────────────────────────────────────────────────────

/*static*/ std::string glint_window_linux::wcsToUtf8(const wchar_t* wcs)
{
  if (!wcs) return {};
  std::string result;
  result.reserve(std::wcslen(wcs));
  while (*wcs)
  {
    const unsigned long cp = static_cast<unsigned long>(*wcs++);
    if (cp < 0x80)
    {
      result += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
      result += static_cast<char>(0xC0 | (cp >> 6));
      result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
      result += static_cast<char>(0xE0 | (cp >> 12));
      result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      result += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
      result += static_cast<char>(0xF0 | (cp >> 18));
      result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      result += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  return result;
}

float glint_window_linux::detectDpr() const
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy) return 1.f;

  // 1) Try Xft.dpi from the X resource database
  const char* rms = XResourceManagerString(dpy);
  if (rms)
  {
    XrmDatabase db = XrmGetStringDatabase(rms);
    if (db)
    {
      XrmValue value;
      char* type = nullptr;
      if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr)
      {
        const float dpi = static_cast<float>(std::atof(value.addr));
        XrmDestroyDatabase(db);
        if (dpi >= 48.f)
          return dpi / 96.f;
      }
      else
      {
        XrmDestroyDatabase(db);
      }
    }
  }

  // 2) Compute from the physical screen dimensions reported by the server
  const int screen  = DefaultScreen(dpy);
  const int widthPx = DisplayWidth(dpy, screen);
  const int widthMm = DisplayWidthMM(dpy, screen);
  if (widthMm > 0)
  {
    const float dpi = static_cast<float>(widthPx) * 25.4f
                    / static_cast<float>(widthMm);
    if (dpi >= 48.f)
      return dpi / 96.f;
  }

  return 1.f;
}

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU

// ── EGL / Ganesh GPU helpers ──────────────────────────────────────────────────

bool glint_window_linux::initGpu()
{
  // mEglDisplay was set by createXWindow(); mEglSurface holds the EGLConfig
  // we chose there (temporarily).
  EGLDisplay eglDpy = static_cast<EGLDisplay>(mEglDisplay);
  if (eglDpy == EGL_NO_DISPLAY)
    return false;

  EGLConfig eglCfg = static_cast<EGLConfig>(mEglSurface);
  mEglSurface = nullptr;   // clear the temp value

  if (!eglCfg)
    return false;

  // Detect whether we're using desktop GL or GLES by querying the current API
  // (eglBindAPI was called in createXWindow).
  const EGLenum activeApi = eglQueryAPI();

  // Create the EGL context
  EGLint ctxAttrs[8];
  int    idx = 0;
  if (activeApi == EGL_OPENGL_ES_API)
  {
    ctxAttrs[idx++] = EGL_CONTEXT_CLIENT_VERSION;
    ctxAttrs[idx++] = 2;
  }
  ctxAttrs[idx++] = EGL_NONE;

  EGLContext eglCtx = eglCreateContext(eglDpy, eglCfg, EGL_NO_CONTEXT, ctxAttrs);
  if (eglCtx == EGL_NO_CONTEXT)
    return false;

  // Create the EGL window surface from the X11 window
  const EGLNativeWindowType xwin = static_cast<EGLNativeWindowType>(mXWindow);
  EGLSurface eglSurf = eglCreateWindowSurface(eglDpy, eglCfg, xwin, nullptr);
  if (eglSurf == EGL_NO_SURFACE)
  {
    eglDestroyContext(eglDpy, eglCtx);
    return false;
  }

  // Make context current
  if (!eglMakeCurrent(eglDpy, eglSurf, eglSurf, eglCtx))
  {
    eglDestroySurface(eglDpy, eglSurf);
    eglDestroyContext(eglDpy, eglCtx);
    return false;
  }

  // Leave swap interval at the EGL default (1).  On WSLg/XWayland the Mesa
  // LLVMpipe driver has no real hardware vsync so the swap returns promptly
  // regardless.  Setting interval=0 breaks frame presentation on XWayland
  // (the Wayland compositor drops frames before they ever appear on screen).

  mEglSurface = static_cast<void*>(eglSurf);
  mEglContext = static_cast<void*>(eglCtx);

  // Build Skia GL interface using eglGetProcAddress as the loader
  auto glInterface = GrGLMakeAssembledInterface(
      nullptr,
      [](void* /*ctx*/, const char name[]) -> GrGLFuncPtr {
        return reinterpret_cast<GrGLFuncPtr>(eglGetProcAddress(name));
      });

  if (!glInterface)
  {
    eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(eglDpy, eglSurf);
    eglDestroyContext(eglDpy, eglCtx);
    mEglSurface = mEglContext = nullptr;
    return false;
  }

  mGrContext = GrDirectContexts::MakeGL(std::move(glInterface));
  if (!mGrContext)
  {
    eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(eglDpy, eglSurf);
    eglDestroyContext(eglDpy, eglCtx);
    mEglSurface = mEglContext = nullptr;
    return false;
  }

  mGpuOk = true;
  recreateSurface();   // create the initial SkSurface wrapping FBO 0
  return true;
}

void glint_window_linux::destroyGpu()
{
  mGpuSurface.reset();
  mGrContext.reset();

  EGLDisplay eglDpy = static_cast<EGLDisplay>(mEglDisplay);
  if (eglDpy != EGL_NO_DISPLAY)
  {
    eglMakeCurrent(eglDpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mEglSurface)
      eglDestroySurface(eglDpy, static_cast<EGLSurface>(mEglSurface));
    if (mEglContext)
      eglDestroyContext(eglDpy, static_cast<EGLContext>(mEglContext));
    eglTerminate(eglDpy);
  }
  mEglSurface = mEglContext = mEglDisplay = nullptr;
  mGpuOk = false;
}

void glint_window_linux::recreateSurface()
{
  if (!mGpuOk || !mGrContext || mWpx <= 0 || mHpx <= 0)
  {
    recreateCpuSurface();
    return;
  }

  mGpuSurface.reset();

  // Query the current EGL surface dimensions to make sure they match
  EGLDisplay eglDpy  = static_cast<EGLDisplay>(mEglDisplay);
  EGLSurface eglSurf = static_cast<EGLSurface>(mEglSurface);

  EGLint surfW = mWpx, surfH = mHpx;
  eglQuerySurface(eglDpy, eglSurf, EGL_WIDTH,  &surfW);
  eglQuerySurface(eglDpy, eglSurf, EGL_HEIGHT, &surfH);

  // Wrap FBO 0 (the default framebuffer) as a Skia render target.
  // 0x8058 = GL_RGBA8 (same value in both desktop GL and GLES)
  GrGLFramebufferInfo fbInfo{};
  fbInfo.fFBOID  = 0;
  fbInfo.fFormat = 0x8058; // GL_RGBA8

  const GrBackendRenderTarget backendRT =
      GrBackendRenderTargets::MakeGL(surfW, surfH,
                                     /*sampleCnt=*/0,
                                     /*stencilBits=*/0,
                                     fbInfo);

  mGpuSurface = SkSurfaces::WrapBackendRenderTarget(
      mGrContext.get(),
      backendRT,
      kBottomLeft_GrSurfaceOrigin,
      kRGBA_8888_SkColorType,
      nullptr,
      nullptr);

  if (mGpuSurface)
  {
    mCanvas = mGpuSurface->getCanvas();
  }
  else
  {
    // GPU surface creation failed — fall back to CPU
    mGpuOk = false;
    recreateCpuSurface();
  }
}

#endif // GLINT_RENDER_GPU

bool glint_window_linux::createXWindow()
{
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) return false;
  mDisplay = dpy;

  const int screen = DefaultScreen(dpy);
  mDpr = detectDpr();
  if (mDpr < 0.5f) mDpr = 1.f;

  mWpx = static_cast<int>(std::lround(static_cast<float>(mW) * mDpr));
  mHpx = static_cast<int>(std::lround(static_cast<float>(mH) * mDpr));

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  // ── GPU path: ask EGL to pick a suitable visual for us ───────────────────
  // We use EGL_PLATFORM_X11_KHR so EGL works with our existing Xlib display.
  // The resulting visual / depth must match what EGL chose, otherwise
  // eglCreateWindowSurface will fail with EGL_BAD_MATCH.
  EGLDisplay eglDpy = EGL_NO_DISPLAY;

  // Use the platform-specific extension if available (preferred)
  auto eglGetPlatformDisplayEXT =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));

  if (eglGetPlatformDisplayEXT)
  {
    const EGLint platAttribs[] = {
      EGL_PLATFORM_X11_SCREEN_EXT, screen,
      EGL_NONE
    };
    eglDpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT,
                                      static_cast<void*>(dpy),
                                      platAttribs);
  }
  if (eglDpy == EGL_NO_DISPLAY)
    eglDpy = eglGetDisplay(static_cast<EGLNativeDisplayType>(dpy));
  if (eglDpy == EGL_NO_DISPLAY)
    eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);

  EGLint major = 0, minor = 0;
  bool eglReady = (eglDpy != EGL_NO_DISPLAY)
               && (eglInitialize(eglDpy, &major, &minor) == EGL_TRUE);

  // EGL config — prefer OpenGL; fall back to OpenGL ES 2
  EGLConfig eglCfg = nullptr;
  EGLint    eglCfgVisualId = 0;
  bool      eglCfgOk = false;

  if (eglReady)
  {
    // Try desktop GL first
    eglBindAPI(EGL_OPENGL_API);
    const EGLint cfgAttrsGL[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RED_SIZE,   8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE,  8,
      EGL_ALPHA_SIZE, 0,
      EGL_DEPTH_SIZE, 0,
      EGL_NONE
    };
    EGLint numCfg = 0;
    if (eglChooseConfig(eglDpy, cfgAttrsGL, &eglCfg, 1, &numCfg) && numCfg > 0)
    {
      eglGetConfigAttrib(eglDpy, eglCfg, EGL_NATIVE_VISUAL_ID, &eglCfgVisualId);
      eglCfgOk = true;
    }
    else
    {
      // Fall back to GLES 2
      eglBindAPI(EGL_OPENGL_ES_API);
      const EGLint cfgAttrsES[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,   8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE,  8,
        EGL_ALPHA_SIZE, 0,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
      };
      if (eglChooseConfig(eglDpy, cfgAttrsES, &eglCfg, 1, &numCfg) && numCfg > 0)
      {
        eglGetConfigAttrib(eglDpy, eglCfg, EGL_NATIVE_VISUAL_ID, &eglCfgVisualId);
        eglCfgOk = true;
      }
    }
  }

  if (eglReady && eglCfgOk && eglCfgVisualId != 0)
  {
    // Use the visual EGL chose
    XVisualInfo tmpl{};
    tmpl.visualid = static_cast<VisualID>(eglCfgVisualId);
    int n = 0;
    XVisualInfo* vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
    if (vi && n > 0)
    {
      mVisual = vi[0].visual;
      mDepth  = vi[0].depth;
      XFree(vi);
    }
    else
    {
      mVisual = DefaultVisual(dpy, screen);
      mDepth  = DefaultDepth(dpy, screen);
    }
  }
  else
  {
    mVisual = DefaultVisual(dpy, screen);
    mDepth  = DefaultDepth(dpy, screen);
  }

  // Store the EGL display for initGpu() which runs after the window is created
  mEglDisplay = static_cast<void*>(eglDpy);

  // If EGL init worked, store config in mEglSurface slot temporarily so
  // initGpu() can retrieve it without having to redo eglChooseConfig.
  // We encode: if eglCfgOk, cast EGLConfig to void* for now.
  // initGpu() will overwrite mEglSurface with the real EGLSurface.
  if (eglCfgOk)
    mEglSurface = static_cast<void*>(eglCfg);   // temporary — overwritten in initGpu

#else
  // ── CPU path: use the server's default visual ─────────────────────────────
  mVisual = DefaultVisual(dpy, screen);
  mDepth  = DefaultDepth(dpy, screen);
#endif

  // Window attributes: black background, subscribe to needed events
  XSetWindowAttributes attrs{};
  attrs.background_pixel = BlackPixel(dpy, screen);
  attrs.event_mask =
      ExposureMask        |
      StructureNotifyMask |
      ButtonPressMask     |
      ButtonReleaseMask   |
      PointerMotionMask   |
      Button1MotionMask   |
      LeaveWindowMask     |
      KeyPressMask        |
      KeyReleaseMask      |
      FocusChangeMask;

  unsigned long cwMask = CWBackPixel | CWEventMask;

  // When EGL has selected a non-default visual, we must create a matching
  // colormap; otherwise the window manager will refuse to map the window.
  Colormap cmap = 0;
  if (mVisual != DefaultVisual(dpy, screen))
  {
    cmap = XCreateColormap(dpy, RootWindow(dpy, screen),
                           static_cast<Visual*>(mVisual), AllocNone);
    attrs.colormap    = cmap;
    attrs.border_pixel = 0;
    cwMask |= CWColormap | CWBorderPixel;
  }

  const int posX = std::max(0, (DisplayWidth(dpy, screen)  - mWpx) / 2);
  const int posY = std::max(0, (DisplayHeight(dpy, screen) - mHpx) / 2);

  const Window xwin = XCreateWindow(
      dpy,
      RootWindow(dpy, screen),
      posX, posY,
      static_cast<unsigned>(mWpx),
      static_cast<unsigned>(mHpx),
      0,              // border width
      mDepth,
      InputOutput,
      static_cast<Visual*>(mVisual),
      cwMask,
      &attrs);

  if (!xwin) return false;
  mXWindow = static_cast<unsigned long>(xwin);

  // Window title
  const std::string title = wcsToUtf8(windowTitle());
  XStoreName(dpy, xwin, title.c_str());

  // WM_CLASS hint
  XClassHint* hint = XAllocClassHint();
  if (hint)
  {
    const std::string className = wcsToUtf8(windowClassName());
    hint->res_name  = const_cast<char*>(className.c_str());
    hint->res_class = const_cast<char*>(className.c_str());
    XSetClassHint(dpy, xwin, hint);
    XFree(hint);
  }

  // WM size hints (initial size)
  XSizeHints* sizeHints = XAllocSizeHints();
  if (sizeHints)
  {
    sizeHints->flags      = PMinSize;
    sizeHints->min_width  = 100;
    sizeHints->min_height = 50;
    XSetWMNormalHints(dpy, xwin, sizeHints);
    XFree(sizeHints);
  }

  // WM_DELETE_WINDOW protocol so closing via the window manager is intercepted
  const Atom wmDeleteAtom = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  mWmDeleteAtom = static_cast<unsigned long>(wmDeleteAtom);
  Atom protocol = wmDeleteAtom;
  XSetWMProtocols(dpy, xwin, &protocol, 1);

  // Graphics context for XPutImage
  mGC = XCreateGC(dpy, xwin, 0, nullptr);

  // Popup style: override redirect, no decorations, popup window type
  if (usePopupStyle())
  {
    Atom wmWindowType = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom wmPopupMenu  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
    XChangeProperty(dpy, xwin, wmWindowType, XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&wmPopupMenu), 1);
    // Remove window manager decorations (Motif hints)
    struct { unsigned long flags, functions, decorations, input_mode, status; } mwm{};
    mwm.flags       = 2;   // MWM_HINTS_DECORATIONS
    mwm.decorations = 0;
    const Atom motifHints = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
    XChangeProperty(dpy, xwin, motifHints, motifHints, 32, PropModeReplace,
                    reinterpret_cast<unsigned char*>(&mwm), 5);
  }

  if (showOnCreate())
  {
    XMapWindow(dpy, xwin);
    XRaiseWindow(dpy, xwin);
    XSync(dpy, False);  // wait for map to propagate through X server

    // Ask the WM to activate/focus this window (_NET_ACTIVE_WINDOW)
    const Atom netActiveWindow = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    if (netActiveWindow != None)
    {
      XEvent ev{};
      ev.type                 = ClientMessage;
      ev.xclient.window       = xwin;
      ev.xclient.message_type = netActiveWindow;
      ev.xclient.format       = 32;
      ev.xclient.data.l[0]    = 1; // source: application
      ev.xclient.data.l[1]    = CurrentTime;
      ev.xclient.data.l[2]    = 0;
      XSendEvent(dpy, RootWindow(dpy, screen), False,
                 SubstructureNotifyMask | SubstructureRedirectMask, &ev);
    }
    XFlush(dpy);
  }
  return true;
}

void glint_window_linux::initRoot()
{
  const glint_rect bounds(0.f, 0.f,
                          static_cast<float>(mW),
                          static_cast<float>(mH));
  mOwnRoot = std::make_unique<glint_document>(
    bounds, nullptr,
    [this] {
      mRedrawRequested.store(true, std::memory_order_relaxed);
      if (mWakeFd[1] != -1)
      {
        const char c = 'R';
        (void)::write(mWakeFd[1], &c, 1);
      }
    });

  mOwnRoot->devicePixelRatio = mDpr;
  mOwnRoot->linuxWindow = this;
  mOwnRoot->mCanvas.style.display       = "flex";
  mOwnRoot->mCanvas.style.flexDirection = "column";

  // Ctrl+Shift+I — toggle inspector.  Ctrl+Shift+C — open with picker.
  mOwnRoot->onGlobalKeyDown = [this](const glint_key_press& k) -> bool {
    if (k.ctrl && k.shift && k.vk == 'I') {
      glint_document* root = mOwnRoot.get();
      if (glint_insp_bridge::isOpen(root))
        glint_insp_bridge::close(root);
      else
        glint_insp_bridge::open(root);
      return true;
    }
    if (k.ctrl && k.shift && k.vk == 'C') {
      glint_insp_bridge::openAndEnableInspect(mOwnRoot.get());
      return true;
    }
    return false;
  };
}

void glint_window_linux::updateRootBounds()
{
  if (!mOwnRoot) return;
  const glint_rect bounds(0.f, 0.f,
                          static_cast<float>(mW),
                          static_cast<float>(mH));
  mOwnRoot->mCanvas.mRect      = bounds;
  mOwnRoot->mCanvas.mPaintRECT = bounds;
  mOwnRoot->mCanvas.mParentW   = static_cast<float>(mW);
  mOwnRoot->mCanvas.mParentH   = static_cast<float>(mH);
  mOwnRoot->mLayoutDirty = true;
}

void glint_window_linux::refreshWindowTitle()
{
  Display* dpy = static_cast<Display*>(mDisplay);
  if (!dpy || !mXWindow) return;
  const std::string title = wcsToUtf8(windowTitle());
  XStoreName(dpy, static_cast<Window>(mXWindow), title.c_str());
  XFlush(dpy);
}

bool glint_window_linux::shouldTimerRedraw() const
{
  if (mRedrawRequested.load(std::memory_order_relaxed))
    return true;

  if (!mOwnRoot)
    return false;

  if (mOwnRoot->mCanvas.hasActiveAnimationSubtree())
    return true;

  const glint_element* focused = mOwnRoot->getFocusedNode();
  if (focused && focused->wantsPeriodicRedraw())
    return std::chrono::steady_clock::now() >= focused->nextPeriodicRedrawTime();

  return false;
}

void glint_window_linux::paint()
{
  if (!mOwnRoot || !mCanvas || !mDisplay || !mXWindow)
    return;
  if (mWpx <= 0 || mHpx <= 0)
    return;

  mCanvas->clear(clearColor());
  if (mDpr != 1.f && mDpr > 0.f)
  {
    mCanvas->save();
    mCanvas->scale(mDpr, mDpr);
    mOwnRoot->DrawToCanvas(*mCanvas);
    mCanvas->restore();
  }
  else
  {
    mOwnRoot->DrawToCanvas(*mCanvas);
  }

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  if (mGpuOk && mGrContext && mGpuSurface)
  {
    // Flush Skia's deferred work and present via EGL.
    // kNo = don't stall the CPU waiting for the (software) GPU to finish;
    // eglSwapBuffers handles synchronisation with the display server.
    mGrContext->flushAndSubmit(GrSyncCpu::kNo);
    eglSwapBuffers(static_cast<EGLDisplay>(mEglDisplay),
                   static_cast<EGLSurface>(mEglSurface));
    mRedrawRequested.store(false, std::memory_order_relaxed);
    return;
  }
  // GPU failed or not ready — fall through to CPU path
#endif

  if (!mGC) return;

  Display*      dpy = static_cast<Display*>(mDisplay);
  const Window  win = static_cast<Window>(mXWindow);
  GC            gc  = static_cast<GC>(mGC);

  // Wrap the Skia pixel buffer in an XImage and blit it.
  // kN32 (BGRA_8888 on LE) maps directly to X11's RGB masks with byte_order=LSBFirst:
  //   R=0xFF0000, G=0xFF00, B=0xFF  (LE uint32: 0xAARRGGBB = [B,G,R,A] bytes)
  // The alpha byte is simply ignored by X11 for depth=24 windows.
  XImage* img = XCreateImage(
      dpy,
      static_cast<Visual*>(mVisual),
      static_cast<unsigned>(mDepth),
      ZPixmap,
      0,
      nullptr,                      // data set below
      static_cast<unsigned>(mWpx),
      static_cast<unsigned>(mHpx),
      32,                           // bitmap_pad
      mWpx * 4);                    // bytes_per_line

  if (!img) return;

  img->data       = static_cast<char*>(static_cast<void*>(mBitmap.getPixels()));
  img->byte_order = LSBFirst;
  img->bitmap_bit_order = LSBFirst;

  XPutImage(dpy, win, gc, img,
            0, 0, 0, 0,
            static_cast<unsigned>(mWpx),
            static_cast<unsigned>(mHpx));

  // Prevent XDestroyImage from freeing the Skia-owned pixel buffer
  img->data = nullptr;
  XDestroyImage(img);

  XFlush(dpy);

  mRedrawRequested.store(false, std::memory_order_relaxed);
}

void glint_window_linux::processXEvent(void* xev)
{
  XEvent& ev  = *static_cast<XEvent*>(xev);
  Display* dpy = static_cast<Display*>(mDisplay);

  switch (ev.type)
  {
  case Expose:
    if (ev.xexpose.count == 0)
      paint();
    break;

  case ConfigureNotify:
  {
    const int newWpx = ev.xconfigure.width;
    const int newHpx = ev.xconfigure.height;
    if (newWpx != mWpx || newHpx != mHpx)
    {
      mWpx = newWpx;
      mHpx = newHpx;
      mW   = static_cast<int>(std::lround(static_cast<float>(mWpx) / mDpr));
      mH   = static_cast<int>(std::lround(static_cast<float>(mHpx) / mDpr));
      if (mOwnRoot) mOwnRoot->devicePixelRatio = mDpr;
      updateRootBounds();
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
      recreateSurface();
#else
      recreateCpuSurface();
#endif
      paint();
    }
    break;
  }

  case ClientMessage:
    if (static_cast<Atom>(ev.xclient.data.l[0]) ==
        static_cast<Atom>(mWmDeleteAtom))
    {
      onDestroyed();
      mRunning.store(false, std::memory_order_relaxed);
    }
    break;

  case ButtonPress:
  {
    const float invDpr = mDpr > 0.f ? 1.f / mDpr : 1.f;
    const float x = static_cast<float>(ev.xbutton.x) * invDpr;
    const float y = static_cast<float>(ev.xbutton.y) * invDpr;

    glint_mouse_mod mod{};
    mod.S   = (ev.xbutton.state & ShiftMask)   != 0;
    mod.C   = (ev.xbutton.state & ControlMask) != 0;

    const unsigned int btn = ev.xbutton.button;
    if (btn == Button4) {
      if (mOwnRoot) mOwnRoot->OnMouseWheel(x, y,  0.f, -40.f, mod);
    } else if (btn == Button5) {
      if (mOwnRoot) mOwnRoot->OnMouseWheel(x, y,  0.f,  40.f, mod);
    } else if (btn == 6) {
      if (mOwnRoot) mOwnRoot->OnMouseWheel(x, y, -40.f, 0.f,  mod);
    } else if (btn == 7) {
      if (mOwnRoot) mOwnRoot->OnMouseWheel(x, y,  40.f, 0.f,  mod);
    } else {
      mod.L   = (btn == Button1);
      mod.R   = (btn == Button3);
      mod.Mid = (btn == Button2);
      mPrevX = x;
      mPrevY = y;
      if (mOwnRoot) mOwnRoot->OnMouseDown(x, y, mod);
    }
    paint();
    break;
  }

  case ButtonRelease:
  {
    const float invDpr = mDpr > 0.f ? 1.f / mDpr : 1.f;
    const float x = static_cast<float>(ev.xbutton.x) * invDpr;
    const float y = static_cast<float>(ev.xbutton.y) * invDpr;
    glint_mouse_mod mod{};
    mod.S = (ev.xbutton.state & ShiftMask)   != 0;
    mod.C = (ev.xbutton.state & ControlMask) != 0;
    if (mOwnRoot) mOwnRoot->OnMouseUp(x, y, mod);
    paint();
    break;
  }

  case MotionNotify:
  {
    // Coalesce: if more MotionNotify events are already queued, discard
    // intermediate positions and only process the most recent one.
    // This prevents the "replay" effect where the UI chases the mouse
    // through every queued position after a slow paint().
    {
      Display* cdpy = static_cast<Display*>(mDisplay);
      XEvent   next{};
      while (XCheckMaskEvent(cdpy,
                             PointerMotionMask | ButtonMotionMask |
                             Button1MotionMask | Button2MotionMask |
                             Button3MotionMask, &next))
        ev = *static_cast<XEvent*>(&next);
    }

    const float invDpr = mDpr > 0.f ? 1.f / mDpr : 1.f;
    const float x = static_cast<float>(ev.xmotion.x) * invDpr;
    const float y = static_cast<float>(ev.xmotion.y) * invDpr;
    const float dx = x - mPrevX;
    const float dy = y - mPrevY;
    mPrevX = x;
    mPrevY = y;

    glint_mouse_mod mod{};
    mod.L   = (ev.xmotion.state & Button1Mask) != 0;
    mod.R   = (ev.xmotion.state & Button3Mask) != 0;
    mod.Mid = (ev.xmotion.state & Button2Mask) != 0;
    mod.S   = (ev.xmotion.state & ShiftMask)   != 0;
    mod.C   = (ev.xmotion.state & ControlMask) != 0;

    if (mOwnRoot)
    {
      if (mod.L || mod.Mid)
        mOwnRoot->OnMouseDrag(x, y, dx, dy, mod);
      else
        mOwnRoot->OnMouseOver(x, y, mod, dx, dy);
    }
    mRedrawRequested.store(true, std::memory_order_relaxed);
    break;
  }

  case LeaveNotify:
    if (mOwnRoot) mOwnRoot->OnMouseOut();
    mRedrawRequested.store(true, std::memory_order_relaxed);
    break;

  case FocusOut:
    if (usePopupStyle())
      onOutsideClick();
    break;

  case KeyPress:
  {
    char buf[16] = {};
    KeySym sym = NoSymbol;
    const int len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &sym, nullptr);

    glint_key_press kp{};
    kp.vk    = keysymToVK(sym);
    kp.shift = (ev.xkey.state & ShiftMask)   != 0;
    kp.ctrl  = (ev.xkey.state & ControlMask) != 0;
    kp.alt   = (ev.xkey.state & Mod1Mask)    != 0;

    if (len > 0 && static_cast<unsigned char>(buf[0]) >= 0x20)
      std::memcpy(kp.utf8, buf, static_cast<std::size_t>(std::min(len, 4)));

    if (mOwnRoot)
    {
      mOwnRoot->OnKeyDown(kp);
      onKeyDown(kp);
    }
    mRedrawRequested.store(true, std::memory_order_relaxed);
    break;
  }

  case KeyRelease:
  {
    char buf[16] = {};
    KeySym sym = NoSymbol;
    XLookupString(&ev.xkey, buf, sizeof(buf) - 1, &sym, nullptr);

    glint_key_press kp{};
    kp.vk    = keysymToVK(sym);
    kp.shift = (ev.xkey.state & ShiftMask)   != 0;
    kp.ctrl  = (ev.xkey.state & ControlMask) != 0;
    kp.alt   = (ev.xkey.state & Mod1Mask)    != 0;

    if (mOwnRoot) mOwnRoot->OnKeyUp(kp);
    break;
  }

  default:
    break;
  }

  (void)dpy;
}

void glint_window_linux::run()
{
  mRunning.store(true, std::memory_order_relaxed);

  // Wake-up pipe: [0]=read end (event loop), [1]=write end (other threads)
  if (::pipe(mWakeFd) != 0)
  {
    mRunning.store(false, std::memory_order_relaxed);
    return;
  }
  // Make the read end non-blocking so we can drain it without stalling
  ::fcntl(mWakeFd[0], F_SETFL, O_NONBLOCK);

  if (!createXWindow())
  {
    ::close(mWakeFd[0]);
    ::close(mWakeFd[1]);
    mWakeFd[0] = mWakeFd[1] = -1;
    mRunning.store(false, std::memory_order_relaxed);
    return;
  }

  // timerfd for ~60 fps animation heartbeat
  const int timerFd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timerFd != -1)
  {
    struct itimerspec ts{};
    ts.it_value.tv_nsec    = 16L * 1000000L;
    ts.it_interval.tv_nsec = 16L * 1000000L;
    ::timerfd_settime(timerFd, 0, &ts, nullptr);
  }

  initRoot();
#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  if (!initGpu())
    recreateCpuSurface();   // GPU init failed — use CPU fallback
#else
  recreateCpuSurface();
#endif
  buildUI();
  onCreated();
  onThreadStarted();

  Display*       dpy  = static_cast<Display*>(mDisplay);
  const int      xFd  = XConnectionNumber(dpy);

  // Force an initial paint so the window has content when first shown.
  // This is needed because the Expose event may have already fired while
  // buildUI/onCreated were running (before the event loop started).
  mRedrawRequested.store(true, std::memory_order_relaxed);
  paint();

  while (mRunning.load(std::memory_order_relaxed))
  {
    // 1. Drain all pending X events (no paint inside the drain loop —
    //    processXEvent sets mRedrawRequested; we paint once after draining
    //    to avoid rendering every intermediate mouse position).
    while (XPending(dpy))
    {
      XEvent ev;
      XNextEvent(dpy, &ev);
      processXEvent(&ev);
      if (!mRunning.load(std::memory_order_relaxed))
        goto cleanup;
    }

    // Paint once after draining all queued events
    if (mRedrawRequested.load(std::memory_order_relaxed))
      paint();

    // 2. Poll: X connection fd + wake pipe + timer fd
    {
      struct pollfd fds[3];
      fds[0].fd      = xFd;
      fds[0].events  = POLLIN;
      fds[0].revents = 0;
      fds[1].fd      = mWakeFd[0];
      fds[1].events  = POLLIN;
      fds[1].revents = 0;
      fds[2].fd      = timerFd;
      fds[2].events  = (timerFd != -1) ? POLLIN : 0;
      fds[2].revents = 0;

      const int nfds = (timerFd != -1) ? 3 : 2;
      ::poll(fds, static_cast<nfds_t>(nfds), 100 /*ms safety timeout*/);

      // Wake pipe: drain, then check stop flag
      if (fds[1].revents & POLLIN)
      {
        char buf[64];
        (void)::read(mWakeFd[0], buf, sizeof(buf));
        if (!mRunning.load(std::memory_order_relaxed))
          break;
        // Explicit redraw request
        if (mRedrawRequested.load(std::memory_order_relaxed))
          paint();
      }

      // Timer: animation heartbeat
      if (timerFd != -1 && (fds[2].revents & POLLIN))
      {
        uint64_t exp = 0;
        (void)::read(timerFd, &exp, sizeof(exp));
        if (shouldTimerRedraw())
          paint();
      }
    }
  }

cleanup:
  onThreadEnded();

#if defined(GLINT_RENDER_GPU) && GLINT_RENDER_GPU
  destroyGpu();
#endif

  Display* closeDpy = static_cast<Display*>(mDisplay);
  if (closeDpy)
  {
    if (mGC && mXWindow)
      XFreeGC(closeDpy, static_cast<GC>(mGC));
    if (mXWindow)
      XDestroyWindow(closeDpy, static_cast<Window>(mXWindow));
    XCloseDisplay(closeDpy);
  }

  mDisplay = nullptr;
  mGC      = nullptr;
  mXWindow = 0;

  if (timerFd != -1) ::close(timerFd);
  ::close(mWakeFd[0]);
  ::close(mWakeFd[1]);
  mWakeFd[0] = mWakeFd[1] = -1;

  mRunning.store(false, std::memory_order_relaxed);
  afterRun();
}

// ── glint_platform Linux implementations ─────────────────────────────────────
// Declared in platform/glint_platform.hpp.

#include "../glint_platform.hpp"

namespace glint_platform {

void setClipboardText(const std::string& utf8)
{
  // Pipe the text to xclip (or xsel if available).
  FILE* xclip = ::popen("xclip -selection clipboard 2>/dev/null", "w");
  if (xclip)
  {
    ::fwrite(utf8.data(), 1, utf8.size(), xclip);
    ::pclose(xclip);
    return;
  }
  // Fallback: try xsel
  FILE* xsel = ::popen("xsel --clipboard --input 2>/dev/null", "w");
  if (xsel)
  {
    ::fwrite(utf8.data(), 1, utf8.size(), xsel);
    ::pclose(xsel);
  }
}

std::string getClipboardText()
{
  FILE* xclip = ::popen("xclip -selection clipboard -o 2>/dev/null", "r");
  if (xclip)
  {
    std::string result;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), xclip))
      result += buf;
    ::pclose(xclip);
    return result;
  }
  // Fallback: xsel
  FILE* xsel = ::popen("xsel --clipboard --output 2>/dev/null", "r");
  if (xsel)
  {
    std::string result;
    char buf[4096];
    while (::fgets(buf, sizeof(buf), xsel))
      result += buf;
    ::pclose(xsel);
    return result;
  }
  return {};
}

int showContextMenu(int /*screenX*/, int /*screenY*/,
                    const std::vector<std::pair<int, std::string>>& /*items*/,
                    const std::vector<int>& /*disabledIds*/,
                    const std::vector<int>& /*checkedIds*/)
{
  // Native X11 context menus require a dedicated pop-up window, which is a
  // sizeable undertaking.  Return 0 (no selection) for now; the calling code
  // falls back gracefully when no item is selected.
  return 0;
}

} // namespace glint_platform
