#pragma once

#include "../glint_element.hpp"

#include <functional>
#include <vector>

class glint_form : public glint_element
{
public:
  std::function<bool(const std::vector<glint_form_value>&, glint_element*)> onSubmit;
  std::function<void()> onReset;
  bool noValidate = false;

  glint_form()
  {
    style.width = "100%";
  }

  const char* typeName() const override { return "form"; }
  const char* tagName() const override { return "form"; }

  static glint_form* nearestFor(glint_element* start)
  {
    glint_element* node = start;
    while (node)
    {
      if (auto* form = dynamic_cast<glint_form*>(node))
        return form;
      node = node->mParent;
    }
    return nullptr;
  }

  std::vector<glint_form_value> collectFormValues(const glint_element* submitter = nullptr)
  {
    _captureDefaultsForTree(this);
    std::vector<glint_form_value> values;
    _collectValues(this, values, submitter);
    return values;
  }

  bool isValid()
  {
    _captureDefaultsForTree(this);
    return _isTreeValid(this);
  }

  bool submit(glint_element* submitter = nullptr)
  {
    _captureDefaultsForTree(this);
    if (!noValidate && !_isTreeValid(this))
      return false;

    glint_event event;
    event.type = "submit";
    event.bubbles = true;
    event.cancelable = true;
    dispatchDOMEvent(event);
    if (event.defaultPrevented)
      return false;

    const auto values = collectFormValues(submitter);
    if (onSubmit)
      return onSubmit(values, submitter);
    return true;
  }

  bool reset()
  {
    _captureDefaultsForTree(this);

    glint_event event;
    event.type = "reset";
    event.bubbles = true;
    event.cancelable = true;
    dispatchDOMEvent(event);
    if (event.defaultPrevented)
      return false;

    _resetTree(this);
    if (onReset)
      onReset();
    return true;
  }

private:
  static void _captureDefaultsForTree(glint_element* node)
  {
    if (!node) return;
    if (node->isFormAssociatedControl())
      node->captureFormDefaultsIfNeeded();
    for (auto* child : node->children())
      _captureDefaultsForTree(child);
  }

  static bool _isTreeValid(glint_element* node)
  {
    if (!node) return true;
    if (node->isFormAssociatedControl() && !node->formControlIsValid())
      return false;
    for (auto* child : node->children())
    {
      if (!_isTreeValid(child))
        return false;
    }
    return true;
  }

  static void _collectValues(glint_element* node,
                             std::vector<glint_form_value>& values,
                             const glint_element* submitter)
  {
    if (!node) return;
    if (node->isFormAssociatedControl())
      node->appendFormValues(values, submitter);
    for (auto* child : node->children())
      _collectValues(child, values, submitter);
  }

  static void _resetTree(glint_element* node)
  {
    if (!node) return;
    if (node->isFormAssociatedControl())
      node->resetFormControl();
    for (auto* child : node->children())
      _resetTree(child);
  }
};

namespace { struct _glint_form_reg { _glint_form_reg() { glint_element::registerElement("form", []{ return new glint_form(); }); } } _glint_form_reg_; }