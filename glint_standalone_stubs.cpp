#include "render/glint_tree_node.hpp"
#include "glint_document.hpp"
#ifdef _WIN32
#include "platform/win32/glint_window_win32.hpp"
#else
// Forward-declare glint_insp_bridge so the stubs compile on non-Windows
// without pulling in the full platform header.
struct glint_insp_bridge {
  static void open(glint_document*);
  static void close(glint_document*);
  static bool isOpen(glint_document*);
  static void openAndEnableInspect(glint_document*);
};
#endif

#ifdef GLINT_INSPECTOR_DISABLED

void glint_document::showInspector(bool)
{
}

bool glint_document::isInspectorOpen() const
{
  return false;
}

void glint_document::openInspectorWithPicker()
{
}

void glint_insp_bridge::open(glint_document*)
{
}

void glint_insp_bridge::close(glint_document*)
{
}

bool glint_insp_bridge::isOpen(glint_document*)
{
  return false;
}

void glint_insp_bridge::openAndEnableInspect(glint_document*)
{
}

#endif