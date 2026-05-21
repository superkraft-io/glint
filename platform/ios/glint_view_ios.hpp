#pragma once

/**
 * glint_view_ios.hpp
 * iOS embedded view host for glint_document.
 *
 * This is the iOS counterpart to glint_view_mac. It hosts a glint document
 * inside a UIView backed by CAMetalLayer and routes touch-first interaction
 * into the existing document mouse/gesture APIs.
 */

#include "../glint_view_base.hpp"

#include <atomic>
#include <memory>

class glint_view_ios final : public glint_view_base
{
public:
  static std::unique_ptr<glint_view_ios> create(const glint_view_options& options = {})
  {
    auto view = std::unique_ptr<glint_view_ios>(new glint_view_ios(options));
    if (!view->open())
      return nullptr;

    return view;
  }

  ~glint_view_ios() override
  {
    close();
  }

  void* nativeHandle() const override
  {
    return mViewHandle;
  }

  bool isOpen() const
  {
    return mViewHandle != nullptr;
  }

  void resize(int width, int height) override;
  void requestRedraw() override;

  void _handleTouchDown(float x, float y);
  void _handleTouchMove(float x, float y);
  void _handleTouchUp(float x, float y);
  void _handleTouchCancel();
  void _handlePinch(float x, float y, glint_input_phase phase, float magnification);
  void _handleRotation(float x, float y, glint_input_phase phase, float rotation);
  void _handleTwoFingerPan(float x, float y, glint_input_phase phase, float dx, float dy);
  void _handleDisplayLink();
  void _syncKeyboardFocus();
  bool _focusedNodeWantsKeyboard() const;
  bool _focusedSuppressesSoftwareKeyboard() const;
  bool _focusedNodeHasText() const;
  bool _focusedNeedsNativeTextServices() const;
  std::string _focusedTextValue() const;
  glint_rect _focusedPaintRect() const;
  int _focusedKeyboardType() const;
  int _focusedReturnKeyType() const;
  int _focusedAutocapitalizationType() const;
  int _focusedAutocorrectionType() const;
  int _focusedSpellCheckingType() const;
  std::string _focusedAutocomplete() const;
  bool _replaceFocusedTextFromPlatform(const std::string& utf8);
  bool _focusedSecureEntry() const;
  bool _focusedCanCut() const;
  bool _focusedCanCopy() const;
  bool _focusedCanPaste() const;
  bool _focusedCanSelectAll() const;
  bool _focusedCut();
  bool _focusedCopy();
  bool _focusedPaste();
  bool _focusedSelectAll();
  bool _handleTextInsert(const std::string& utf8);
  bool _handleReturnKey();
  bool _handleBackspace();

  bool _metalEnabled() const { return mActiveBackend == glint_backend::Metal; }

private:
  explicit glint_view_ios(const glint_view_options& options)
    : glint_view_base(options)
  {}

  bool open();
  void close();
  void initDocument();
  void setupMetal();
  void teardownMetal();
  void updateDrawableSize();
  void paintMetal();

  void* mParentHandle = nullptr;   // UIView* or UIWindow* (unretained)
  void* mViewHandle = nullptr;     // GlintIOSView* (CFRetained)
  void* mDisplayLink = nullptr;    // CADisplayLink* (retained by Objective-C)
  void* mMetalDevice = nullptr;    // id<MTLDevice> (+1 retain)
  void* mMetalQueue = nullptr;     // id<MTLCommandQueue> (+1 retain)
  void* mMetalLayer = nullptr;     // CAMetalLayer* (unretained, owned by UIView)
  void* mActiveTouch = nullptr;    // UITouch* (weak identity only)
  std::atomic<bool> mFramePending{false};
  bool mRedrawRequested = false;
  bool mLastTouchTargetWantsKeyboard = false;
};