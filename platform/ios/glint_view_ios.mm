/**
 * glint_view_ios.mm
 * iOS embedded view host — ObjC++ implementation.
 */

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include "glint_view_ios.hpp"
#include "../glint_platform.hpp"
#include "../../components/glint_input.hpp"
#include "../../components/glint_textarea.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "include/gpu/ganesh/GrTypes.h"

namespace {

UIView* glint_resolve_parent_view(void* parent)
{
  id candidate = (id) parent;
  if (candidate == nil)
    return nil;

  if ([candidate isKindOfClass:[UIView class]])
    return (UIView*) candidate;

  if ([candidate isKindOfClass:[UIWindow class]])
    return (UIWindow*) candidate;

  if ([candidate isKindOfClass:[UIViewController class]])
    return [(UIViewController*) candidate view];

  return nil;
}

glint_input_phase glint_phase_from_gesture_state(UIGestureRecognizerState state)
{
  switch (state)
  {
    case UIGestureRecognizerStateBegan: return glint_input_phase::began;
    case UIGestureRecognizerStateChanged: return glint_input_phase::changed;
    case UIGestureRecognizerStateEnded: return glint_input_phase::ended;
    case UIGestureRecognizerStateCancelled: return glint_input_phase::cancelled;
    default: return glint_input_phase::none;
  }
}

glint_mouse_mod glint_touch_mod(bool down)
{
  glint_mouse_mod mod = {};
  mod.L = down;
  return mod;
}

bool glint_should_schedule_redraw(glint_document* doc, bool redrawRequested)
{
  if (redrawRequested)
    return true;

  glint_element* focused = doc ? doc->getFocusedNode() : nullptr;
  if (!focused || !focused->wantsPeriodicRedraw())
    return false;

  return std::chrono::steady_clock::now() >= focused->nextPeriodicRedrawTime();
}

} // namespace

@class GlintKeyboardProxyField;

@interface GlintIOSView : UIView <UIGestureRecognizerDelegate, UIKeyInput, UITextInputTraits>
{
@public
  glint_view_ios* cppView;
  UIPinchGestureRecognizer* pinchRecognizer;
  UIRotationGestureRecognizer* rotationRecognizer;
  UIPanGestureRecognizer* twoFingerPanRecognizer;
  GlintKeyboardProxyField* keyboardProxyField;
  int lastKeyboardType;
  int lastReturnKeyType;
  BOOL lastSecureEntry;
  BOOL lastWantedKeyboard;
}
- (instancetype)initWithView:(glint_view_ios*)view frame:(CGRect)frame;
- (void)displayLinkFired:(CADisplayLink*)displayLink;
- (void)syncKeyboardFocus;
@end

@interface GlintKeyboardProxyField : UITextField
{
@public
  glint_view_ios* cppView;
}
@end

@implementation GlintKeyboardProxyField

- (BOOL)canBecomeFirstResponder
{
  return YES;
}

- (BOOL)hasText
{
  return cppView ? cppView->_focusedNodeHasText() : [super hasText];
}

- (void)insertText:(NSString*)text
{
  if (!cppView || text == nil || text.length == 0)
    return;

  if ([text isEqualToString:@"\n"] || [text isEqualToString:@"\r"])
  {
    cppView->_handleReturnKey();
    return;
  }

  NSData* utf8data = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8data || utf8data.length == 0)
    return;

  const NSUInteger length = std::min<NSUInteger>(utf8data.length, 32u);
  std::string utf8((const char*) utf8data.bytes, length);
  cppView->_handleTextInsert(utf8);
}

- (void)deleteBackward
{
  if (cppView)
    cppView->_handleBackspace();
}

- (UIKeyboardType)keyboardType
{
  return cppView ? (UIKeyboardType) cppView->_focusedKeyboardType() : UIKeyboardTypeDefault;
}

- (UIReturnKeyType)returnKeyType
{
  return cppView ? (UIReturnKeyType) cppView->_focusedReturnKeyType() : UIReturnKeyDefault;
}

- (BOOL)isSecureTextEntry
{
  return cppView ? cppView->_focusedSecureEntry() : NO;
}

- (UITextAutocapitalizationType)autocapitalizationType
{
  if (!cppView)
    return UITextAutocapitalizationTypeSentences;

  const int keyboardType = cppView->_focusedKeyboardType();
  if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
    return UITextAutocapitalizationTypeNone;
  return UITextAutocapitalizationTypeSentences;
}

- (UITextAutocorrectionType)autocorrectionType
{
  if (cppView)
  {
    const int keyboardType = cppView->_focusedKeyboardType();
    if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
      return UITextAutocorrectionTypeNo;
  }
  return UITextAutocorrectionTypeDefault;
}

- (UITextSpellCheckingType)spellCheckingType
{
  if (cppView)
  {
    const int keyboardType = cppView->_focusedKeyboardType();
    if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
      return UITextSpellCheckingTypeNo;
  }
  return UITextSpellCheckingTypeDefault;
}

- (UIKeyboardAppearance)keyboardAppearance
{
  return UIKeyboardAppearanceDark;
}

- (BOOL)enablesReturnKeyAutomatically
{
  return NO;
}

@end

@implementation GlintIOSView

+ (Class)layerClass
{
  return [CAMetalLayer class];
}

- (instancetype)initWithView:(glint_view_ios*)view frame:(CGRect)frame
{
  self = [super initWithFrame:frame];
  if (self)
  {
    cppView = view;
    self.multipleTouchEnabled = YES;
    self.contentScaleFactor = UIScreen.mainScreen.scale;
    self.opaque = YES;
    self.backgroundColor = UIColor.clearColor;

    pinchRecognizer = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(handlePinch:)];
    pinchRecognizer.delegate = self;
    [self addGestureRecognizer:pinchRecognizer];

    rotationRecognizer = [[UIRotationGestureRecognizer alloc] initWithTarget:self action:@selector(handleRotation:)];
    rotationRecognizer.delegate = self;
    [self addGestureRecognizer:rotationRecognizer];

    twoFingerPanRecognizer = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(handleTwoFingerPan:)];
    twoFingerPanRecognizer.minimumNumberOfTouches = 2;
    twoFingerPanRecognizer.maximumNumberOfTouches = 2;
    twoFingerPanRecognizer.delegate = self;
    [self addGestureRecognizer:twoFingerPanRecognizer];

    keyboardProxyField = [[GlintKeyboardProxyField alloc] initWithFrame:CGRectMake(0.0, 0.0, 1.0, 1.0)];
    keyboardProxyField->cppView = view;
    keyboardProxyField.alpha = 1.0;
    keyboardProxyField.backgroundColor = UIColor.clearColor;
    keyboardProxyField.textColor = UIColor.clearColor;
    keyboardProxyField.tintColor = UIColor.clearColor;
    keyboardProxyField.borderStyle = UITextBorderStyleNone;
    keyboardProxyField.autocorrectionType = UITextAutocorrectionTypeNo;
    keyboardProxyField.spellCheckingType = UITextSpellCheckingTypeNo;
    [self addSubview:keyboardProxyField];

    lastKeyboardType = UIKeyboardTypeDefault;
    lastReturnKeyType = UIReturnKeyDefault;
    lastSecureEntry = NO;
    lastWantedKeyboard = NO;
  }
  return self;
}

- (void)dealloc
{
  keyboardProxyField->cppView = nullptr;
  [keyboardProxyField release];
  [pinchRecognizer release];
  [rotationRecognizer release];
  [twoFingerPanRecognizer release];
  [super dealloc];
}

- (BOOL)canBecomeFirstResponder
{
  return YES;
}

- (BOOL)hasText
{
  return cppView ? cppView->_focusedNodeHasText() : NO;
}

- (void)insertText:(NSString*)text
{
  if (!cppView || text == nil || text.length == 0)
    return;

  if ([text isEqualToString:@"\n"] || [text isEqualToString:@"\r"])
  {
    cppView->_handleReturnKey();
    return;
  }

  NSData* utf8data = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8data || utf8data.length == 0)
    return;

  const NSUInteger length = std::min<NSUInteger>(utf8data.length, 32u);
  std::string utf8((const char*) utf8data.bytes, length);
  cppView->_handleTextInsert(utf8);
}

- (void)deleteBackward
{
  if (cppView)
    cppView->_handleBackspace();
}

- (UIKeyboardType)keyboardType
{
  return cppView ? (UIKeyboardType) cppView->_focusedKeyboardType() : UIKeyboardTypeDefault;
}

- (UIReturnKeyType)returnKeyType
{
  return cppView ? (UIReturnKeyType) cppView->_focusedReturnKeyType() : UIReturnKeyDefault;
}

- (BOOL)isSecureTextEntry
{
  return cppView ? cppView->_focusedSecureEntry() : NO;
}

- (UITextAutocapitalizationType)autocapitalizationType
{
  if (!cppView)
    return UITextAutocapitalizationTypeSentences;

  const int keyboardType = cppView->_focusedKeyboardType();
  if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
    return UITextAutocapitalizationTypeNone;
  return UITextAutocapitalizationTypeSentences;
}

- (UITextAutocorrectionType)autocorrectionType
{
  if (cppView)
  {
    const int keyboardType = cppView->_focusedKeyboardType();
    if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
      return UITextAutocorrectionTypeNo;
  }
  return UITextAutocorrectionTypeDefault;
}

- (UITextSpellCheckingType)spellCheckingType
{
  if (cppView)
  {
    const int keyboardType = cppView->_focusedKeyboardType();
    if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || cppView->_focusedSecureEntry())
      return UITextSpellCheckingTypeNo;
  }
  return UITextSpellCheckingTypeDefault;
}

- (UIKeyboardAppearance)keyboardAppearance
{
  return UIKeyboardAppearanceDark;
}

- (BOOL)enablesReturnKeyAutomatically
{
  return NO;
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer*)gestureRecognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer*)otherGestureRecognizer
{
  (void)gestureRecognizer;
  (void)otherGestureRecognizer;
  return YES;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!cppView || touches.count == 0)
    return;

  UITouch* touch = touches.anyObject;
  CGPoint pt = [touch locationInView:self];
  cppView->_handleTouchDown((float) pt.x, (float) pt.y);
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!cppView || touches.count == 0)
    return;

  UITouch* touch = touches.anyObject;
  CGPoint pt = [touch locationInView:self];
  cppView->_handleTouchMove((float) pt.x, (float) pt.y);
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)event;
  if (!cppView || touches.count == 0)
    return;

  UITouch* touch = touches.anyObject;
  CGPoint pt = [touch locationInView:self];
  cppView->_handleTouchUp((float) pt.x, (float) pt.y);
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event
{
  (void)touches;
  (void)event;
  if (cppView)
    cppView->_handleTouchCancel();
}

- (void)handlePinch:(UIPinchGestureRecognizer*)recognizer
{
  if (!cppView)
    return;
  CGPoint pt = [recognizer locationInView:self];
  cppView->_handlePinch((float) pt.x,
                        (float) pt.y,
                        glint_phase_from_gesture_state(recognizer.state),
                        (float) (recognizer.scale - 1.0));
  if (recognizer.state == UIGestureRecognizerStateChanged)
    recognizer.scale = 1.0;
}

- (void)handleRotation:(UIRotationGestureRecognizer*)recognizer
{
  if (!cppView)
    return;
  CGPoint pt = [recognizer locationInView:self];
  cppView->_handleRotation((float) pt.x,
                           (float) pt.y,
                           glint_phase_from_gesture_state(recognizer.state),
                           (float) recognizer.rotation);
  if (recognizer.state == UIGestureRecognizerStateChanged)
    recognizer.rotation = 0.0;
}

- (void)handleTwoFingerPan:(UIPanGestureRecognizer*)recognizer
{
  if (!cppView)
    return;
  CGPoint pt = [recognizer locationInView:self];
  CGPoint delta = [recognizer translationInView:self];
  cppView->_handleTwoFingerPan((float) pt.x,
                               (float) pt.y,
                               glint_phase_from_gesture_state(recognizer.state),
                               (float) delta.x,
                               (float) delta.y);
  if (recognizer.state == UIGestureRecognizerStateChanged)
    [recognizer setTranslation:CGPointZero inView:self];
}

- (void)displayLinkFired:(CADisplayLink*)displayLink
{
  (void)displayLink;
  if (cppView)
    cppView->_handleDisplayLink();
}

- (void)syncKeyboardFocus
{
  if (!cppView)
    return;

  const bool wantsKeyboard = cppView->_focusedNodeWantsKeyboard();
  const bool proxyResponder = [keyboardProxyField isFirstResponder];
  const int keyboardType = cppView->_focusedKeyboardType();
  const int returnKeyType = cppView->_focusedReturnKeyType();
  const BOOL secureEntry = cppView->_focusedSecureEntry() ? YES : NO;
  const BOOL traitsChanged = lastWantedKeyboard != wantsKeyboard
                          || lastKeyboardType != keyboardType
                          || lastReturnKeyType != returnKeyType
                          || lastSecureEntry != secureEntry;

  if (wantsKeyboard)
  {
    if (!self.window)
      return;

    if (!proxyResponder)
      [keyboardProxyField becomeFirstResponder];
    else if (traitsChanged)
      [keyboardProxyField reloadInputViews];

  }
  else if (proxyResponder)
  {
    [keyboardProxyField resignFirstResponder];
  }

  lastWantedKeyboard = wantsKeyboard ? YES : NO;
  lastKeyboardType = keyboardType;
  lastReturnKeyType = returnKeyType;
  lastSecureEntry = secureEntry;
}

@end

bool glint_view_ios::open()
{
  UIView* parent = glint_resolve_parent_view(mOptions.parent);
  if (parent)
    mParentHandle = (void*) parent;

  if (!shouldUseGpu())
    return false;

  setupMetal();
  if (mActiveBackend != glint_backend::Metal)
    return false;

  GlintIOSView* view = [[GlintIOSView alloc] initWithView:this
                                                    frame:CGRectMake(0.0, 0.0, mW, mH)];
  if (parent)
    [parent addSubview:view];
  mViewHandle = (void*) CFBridgingRetain(view);

  CAMetalLayer* layer = (CAMetalLayer*) view.layer;
  layer.device = (id<MTLDevice>) mMetalDevice;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = NO;
  layer.opaque = YES;
  layer.contentsScale = view.contentScaleFactor;
  mMetalLayer = (__bridge void*) layer;
  [view release];

  CADisplayLink* displayLink = [CADisplayLink displayLinkWithTarget:(__bridge GlintIOSView*) mViewHandle
                                                           selector:@selector(displayLinkFired:)];
  [displayLink addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
  mDisplayLink = (void*) [displayLink retain];

  initDocument();
  updateDrawableSize();
  requestRedraw();
  return true;
}

void glint_view_ios::close()
{
  if (mDisplayLink)
  {
    CADisplayLink* displayLink = (CADisplayLink*) mDisplayLink;
    [displayLink invalidate];
    [displayLink release];
    mDisplayLink = nullptr;
  }

  if (mViewHandle)
  {
    GlintIOSView* view = (__bridge GlintIOSView*) mViewHandle;
    view->cppView = nullptr;
    [view removeFromSuperview];
    CFRelease(mViewHandle);
    mViewHandle = nullptr;
  }

  mParentHandle = nullptr;
  mMetalLayer = nullptr;
  mActiveTouch = nullptr;
  mDocument.reset();
  mRedrawRequested = false;
  teardownMetal();
}

void glint_view_ios::initDocument()
{
  createDocument([this]() { requestRedraw(); });

  mDocument->mCanvas.style.display = "flex";
  mDocument->mCanvas.style.flexDirection = "column";
  updateDocumentBounds();

  if (mOptions.onDocumentCreated)
    mOptions.onDocumentCreated(*mDocument);
}

void glint_view_ios::setupMetal()
{
  @autoreleasepool
  {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device)
      return;

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue)
    {
      [device release];
      return;
    }

    GrMtlBackendContext backendContext;
    backendContext.fDevice.retain((__bridge GrMTLHandle) device);
    backendContext.fQueue.retain((__bridge GrMTLHandle) queue);
    mGrContext = GrDirectContexts::MakeMetal(backendContext);
    if (!mGrContext)
    {
      [queue release];
      [device release];
      return;
    }

    mMetalDevice = (void*) device;
    mMetalQueue = (void*) queue;
    mActiveBackend = glint_backend::Metal;
  }
}

void glint_view_ios::teardownMetal()
{
  mGrContext.reset();

  if (mMetalQueue)
  {
    [(id<MTLCommandQueue>) mMetalQueue release];
    mMetalQueue = nullptr;
  }
  if (mMetalDevice)
  {
    [(id<MTLDevice>) mMetalDevice release];
    mMetalDevice = nullptr;
  }

  if (mActiveBackend == glint_backend::Metal)
    mActiveBackend = glint_backend::CPU;
}

void glint_view_ios::updateDrawableSize()
{
  if (!mViewHandle || !mMetalLayer)
    return;

  GlintIOSView* view = (__bridge GlintIOSView*) mViewHandle;
  CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;
  const CGFloat scale = std::max<CGFloat>(view.contentScaleFactor, 1.0);
  mDpr = (float) scale;
  mWpx = std::max(1, (int) std::lround((double) mW * scale));
  mHpx = std::max(1, (int) std::lround((double) mH * scale));
  layer.contentsScale = scale;
  layer.drawableSize = CGSizeMake(mWpx, mHpx);
}

void glint_view_ios::resize(int width, int height)
{
  mW = std::max(width, 1);
  mH = std::max(height, 1);
  updateDocumentBounds();

  if (mViewHandle)
  {
    GlintIOSView* view = (__bridge GlintIOSView*) mViewHandle;
    view.frame = CGRectMake(0.0, 0.0, mW, mH);
  }

  updateDrawableSize();
  requestRedraw();
}

void glint_view_ios::requestRedraw()
{
  mRedrawRequested = true;
}

void glint_view_ios::_syncKeyboardFocus()
{
  if (!mViewHandle)
    return;

  GlintIOSView* view = (__bridge GlintIOSView*) mViewHandle;
  dispatch_async(dispatch_get_main_queue(), ^{
    [view syncKeyboardFocus];
  });
}

bool glint_view_ios::_focusedNodeWantsKeyboard() const
{
  if (!mDocument)
    return false;

  const glint_element* focused = mDocument->getFocusedNode();
  if (!focused)
    return false;

  const char* typeName = focused->typeName();
  return typeName && (std::strcmp(typeName, "text-input") == 0 || std::strcmp(typeName, "textarea") == 0);
}

bool glint_view_ios::_focusedNodeHasText() const
{
  if (!mDocument)
    return false;

  const glint_element* focused = mDocument->getFocusedNode();
  if (!focused)
    return false;

  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return !input->getValue().empty();
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return !textarea->getValue().empty();
  return false;
}

int glint_view_ios::_focusedKeyboardType() const
{
  if (!mDocument)
    return UIKeyboardTypeDefault;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
  {
    if (input->type == "number") return UIKeyboardTypeDecimalPad;
    if (input->type == "email")  return UIKeyboardTypeEmailAddress;
  }

  return UIKeyboardTypeDefault;
}

int glint_view_ios::_focusedReturnKeyType() const
{
  if (!mDocument)
    return UIReturnKeyDefault;

  const glint_element* focused = mDocument->getFocusedNode();
  if (focused && std::strcmp(focused->typeName(), "text-input") == 0)
    return UIReturnKeyDone;
  return UIReturnKeyDefault;
}

bool glint_view_ios::_focusedSecureEntry() const
{
  if (!mDocument)
    return false;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return input->type == "password";
  return false;
}

bool glint_view_ios::_handleTextInsert(const std::string& utf8)
{
  if (!mDocument || utf8.empty())
    return false;

  glint_key_press key = {};
  const size_t length = std::min<size_t>(utf8.size(), sizeof(key.utf8) - 1);
  std::memcpy(key.utf8, utf8.data(), length);
  key.utf8[length] = '\0';
  const bool consumed = mDocument->OnKeyDown(key);
  requestRedraw();
  return consumed;
}

bool glint_view_ios::_handleReturnKey()
{
  if (!mDocument)
    return false;

  glint_key_press key = {};
  key.vk = glint_vk::RETURN;
  const bool consumed = mDocument->OnKeyDown(key);
  requestRedraw();
  return consumed;
}

bool glint_view_ios::_handleBackspace()
{
  if (!mDocument)
    return false;

  glint_key_press key = {};
  key.vk = glint_vk::BACK;
  const bool consumed = mDocument->OnKeyDown(key);
  requestRedraw();
  return consumed;
}

void glint_view_ios::_handleTouchDown(float x, float y)
{
  if (!mDocument)
    return;

  mPrevX = x;
  mPrevY = y;
  mDocument->OnMouseDown(x, y, glint_touch_mod(true));
  _syncKeyboardFocus();
  requestRedraw();
}

void glint_view_ios::_handleTouchMove(float x, float y)
{
  if (!mDocument)
    return;

  const float dx = x - mPrevX;
  const float dy = y - mPrevY;
  mPrevX = x;
  mPrevY = y;
  mDocument->OnMouseDrag(x, y, dx, dy, glint_touch_mod(true));
  requestRedraw();
}

void glint_view_ios::_handleTouchUp(float x, float y)
{
  if (!mDocument)
    return;

  mDocument->OnMouseUp(x, y, glint_touch_mod(false));
  _syncKeyboardFocus();
  requestRedraw();
}

void glint_view_ios::_handleTouchCancel()
{
  if (!mDocument)
    return;

  mDocument->OnMouseOut();
  _syncKeyboardFocus();
  requestRedraw();
}

void glint_view_ios::_handlePinch(float x, float y, glint_input_phase phase, float magnification)
{
  if (!mDocument)
    return;

  mDocument->OnGesture(x, y, glint_gesture_kind::pinch, phase, {}, 0.f, 0.f, magnification, 0.f, false, true);
  requestRedraw();
}

void glint_view_ios::_handleRotation(float x, float y, glint_input_phase phase, float rotation)
{
  if (!mDocument)
    return;

  const float rotationDegrees = -rotation * (180.f / static_cast<float>(M_PI));
  mDocument->OnGesture(x, y, glint_gesture_kind::rotate, phase, {}, 0.f, 0.f, 0.f, rotationDegrees, false, true);
  requestRedraw();
}

void glint_view_ios::_handleTwoFingerPan(float x, float y, glint_input_phase phase, float dx, float dy)
{
  if (!mDocument)
    return;

  mDocument->OnMouseWheel(x, y, dx, -dy, {}, true, phase, glint_input_phase::none);
  requestRedraw();
}

void glint_view_ios::_handleDisplayLink()
{
  if (glint_should_schedule_redraw(mDocument.get(), mRedrawRequested))
    paintMetal();
}

void glint_view_ios::paintMetal()
{
  if (!mGrContext || !mDocument || !mMetalLayer || mW <= 0 || mH <= 0)
    return;

  @autoreleasepool
  {
    CAMetalLayer* layer = (__bridge CAMetalLayer*) mMetalLayer;
    updateDrawableSize();

    GrMTLHandle drawableHandle = nullptr;
    auto surface = SkSurfaces::WrapCAMetalLayer(
      mGrContext.get(),
      (__bridge GrMTLHandle) layer,
      kTopLeft_GrSurfaceOrigin,
      1,
      kBGRA_8888_SkColorType,
      nullptr,
      nullptr,
      &drawableHandle);
    if (!surface)
      return;

    mRedrawRequested = false;

    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(mOptions.clearColor);
    canvas->save();
    canvas->scale((SkScalar) mDpr, (SkScalar) mDpr);
    mDocument->devicePixelRatio = mDpr;
    mDocument->DrawToCanvas(*canvas);
    canvas->restore();

    GrFlushInfo flushInfo{};
    mGrContext->flush(surface.get(), SkSurfaces::BackendSurfaceAccess::kPresent, flushInfo);
    mGrContext->submit();

    id<CAMetalDrawable> drawable = (__bridge id<CAMetalDrawable>) drawableHandle;
    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>) mMetalQueue commandBuffer];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
  }
}

namespace glint_platform {

void setClipboardText(const std::string& utf8)
{
  UIPasteboard.generalPasteboard.string = [NSString stringWithUTF8String:utf8.c_str()];
}

std::string getClipboardText()
{
  NSString* text = UIPasteboard.generalPasteboard.string;
  if (!text)
    return {};
  return std::string(text.UTF8String ? text.UTF8String : "");
}

int showContextMenu(int,
                    int,
                    const std::vector<std::pair<int, std::string>>&,
                    const std::vector<int>&,
                    const std::vector<int>&)
{
  return 0;
}

std::string showOpenFileDialog(const std::vector<std::string>&,
                               const std::string&,
                               bool)
{
  return {};
}

std::string showSaveFileDialog(const std::vector<std::string>&,
                               const std::string&,
                               const std::string&,
                               const std::string&)
{
  return {};
}

std::string showOpenFolderDialog(const std::string&)
{
  return {};
}

void showAlertDialog(const std::string&, const std::string&)
{
}

confirm_dialog_result showConfirmDialog(const std::string&,
                                        const std::string&,
                                        const std::string&,
                                        const std::string&,
                                        const std::string&)
{
  return confirm_dialog_result::cancel;
}

} // namespace glint_platform