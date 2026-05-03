# Getting Started Guide

This guide shows the supported way to start a new native Win32 Glint project.

If you stay on the stock host APIs:

- `glint_window` for a standalone top-level app window
- `glint_view` / `glint::createView(...)` for an embedded child view

then getting started is straightforward, including GPU-capable rendering.

The main caveat is that `GLINT_RENDER_GPU=ON` compiles the GPU path in, but runtime can still fall back to CPU if GPU initialization or surface creation fails.

macOS host code lives under `platform/mac/` and integrates with the Metal backend, but should be treated as in progress; this walkthrough covers the actively used Win32 path.

## 1. Install prerequisites

Use this setup on Windows:

```text
Visual Studio 2022 with MSVC
CMake 3.25+
Node.js
Git
```

## 2. Create a new project layout

Start with a simple layout like this:

```text
my_glint_app/
  CMakeLists.txt
  src/
    main.cpp
    app_window.hpp
    app_window.cpp
  third_party/
    glint/
```

## 3. Vendor Glint into the project

Copy or add the Glint tree under:

```text
my_glint_app/third_party/glint
```

## 4. Prepare the Skia bundle

By default, Glint expects:

```text
my_glint_app/third_party/skia
```

From your new project root, run with your chosen render backend:

```powershell
# CPU (default — software rasterizer)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both

# OpenGL (GPU, Ganesh backend)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both --backend opengl

# Direct3D 12 (GPU, Graphite backend, Windows only)
node .\third_party\glint\scripts\init_skia.mjs --source --config Both --backend d3d12
```

The script will print the selected backend and generate `third_party/glint/glint_render_backend.h`, which is included automatically by `glint.hpp` and activates the correct compile-time paths. No CMake flags are needed.

That should produce:

```text
third_party/skia/
  src/skia/
  win/x64/Release/
  win/x64/Debug/
  win/bin/
```

If you already have a compatible Skia bundle somewhere else, you can point CMake at it with `GLINT_DEPS_DIR`.

## 5. Write the root CMakeLists.txt

Use this minimal standalone setup:

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_glint_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/third_party/glint" "${CMAKE_CURRENT_BINARY_DIR}/glint")

add_executable(my_glint_app WIN32
  src/main.cpp
  src/app_window.cpp
  src/app_window.hpp
)

target_link_libraries(my_glint_app PRIVATE glint::host_win32)
```

## 6. Add a minimal top-level Glint window

Create `src/app_window.hpp`:

```cpp
#pragma once

#include "glint/glint_window.hpp"

class AppWindow final : public glint_window
{
public:
  static void open();
  static bool isOpen();

protected:
  const wchar_t* windowClassName() const override { return L"my_glint_app"; }
  const wchar_t* windowTitle() const override { return L"My Glint App"; }
  void buildUI() override;
  void onThreadEnded() override;

private:
  AppWindow() = default;
  static AppWindow* sInstance;
};
```

Create `src/app_window.cpp`:

```cpp
#include "app_window.hpp"

#include "glint/glint_standalone.hpp"

AppWindow* AppWindow::sInstance = nullptr;

void AppWindow::open()
{
  if (sInstance && sInstance->isRunning())
    return;

  if (!sInstance)
    sInstance = new AppWindow();

  sInstance->startThread();
}

bool AppWindow::isOpen()
{
  return sInstance && sInstance->isRunning();
}

void AppWindow::buildUI()
{
  mOwnRoot->mCanvas.style.backgroundColor = "#101010";

  mOwnRoot->add.div([](glint_component_style& root) {
    root.style.width = "100%";
    root.style.height = "100%";
    root.style.display = "flex";
    root.style.alignItems = "center";
    root.style.justifyContent = "center";
    root.style.color = "white";
    root.innerText = "Hello from Glint";
  });
}

void AppWindow::onThreadEnded()
{
  sInstance = nullptr;
}
```

Create `src/main.cpp`:

```cpp
#include "app_window.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
  AppWindow::open();

  while (AppWindow::isOpen())
    ::Sleep(16);

  return 0;
}
```

## 7. Configure the build

From the project root:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

If your Skia bundle is not in `third_party/skia`, use:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -D GLINT_DEPS_DIR=C:\path\to\third_party\skia
```

No GPU flags are needed — the render backend was baked in by `init_skia.mjs` via `glint_render_backend.h`.

## 8. Build the project

Use Release for real runtime behavior:

```powershell
cmake --build build --config Release
```

## 9. Run it with runtime verification enabled

Before launching the app:

```powershell
$env:GLINT_ENABLE_RUNTIME_LOG='1'
$env:GLINT_ENABLE_TELEMETRY='1'
.\build\Release\my_glint_app.exe
```

## 10. Check that GPU actually activated

Look at:

```text
%TEMP%/glint_runtime.log
```

For a top-level app window, good signs look like:

```text
GLINT WINDOW: requested backend = OpenGL
GLINT WINDOW: GrDirectContext created
GLINT WINDOW: GPU surface created (OpenGL)
GLINT WINDOW: active backend = OpenGL (GPU)
```

For the Direct3D backend, the equivalent success path is:

```text
GLINT WINDOW: requested backend = D3D12
GLINT WINDOW: GrDirectContext created
GLINT WINDOW: GPU surface created (D3D12)
GLINT WINDOW: active backend = D3D12 (GPU)
```

If telemetry is enabled, you should also see timing lines.

## 11. Embed Glint into an existing Win32 app when needed

If you want to embed Glint instead of creating a standalone window, switch to `glint_view`.

Minimal embedded wrapper:

```cpp
#include "glint/glint_view.hpp"

class EmbeddedGlintView
{
public:
  bool open(HWND parent, int width, int height)
  {
    glint::glint_view_options options{};
    options.parent = parent;
    options.width = width;
    options.height = height;
    options.onDocumentCreated = [](glint_document& document) {
      document.loadStylesheet("/styles/main.css");
    };

    mView = glint::createView(options);
    return static_cast<bool>(mView);
  }

  void resize(int width, int height)
  {
    if (mView)
      mView->resize(width, height);
  }

private:
  std::unique_ptr<glint::glint_view> mView;
};
```

On the supported embedded Win32 path, Glint creates and owns the child `HWND`, sets up the device context, and manages redraw plus CPU/GPU fallback internally.

## 12. Keep the main caveat in mind

This is easy if you stay on:

```text
glint_window
glint_view / createView(...)
```

It is not equally easy if you bypass those and build directly on lower-level host plumbing such as `glint_window_base`. In that case, you are responsible for WGL, device-context ownership, surface lifecycle, and fallback behavior yourself.