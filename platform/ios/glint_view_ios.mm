/**
 * glint_view_ios.mm
 * iOS embedded view host — ObjC++ implementation.
 */

#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "glint_view_ios.hpp"
#include "../glint_platform.hpp"
#include "../glint_platform_colorpicker.hpp"
#include "../../components/glint_input.hpp"
#include "../../components/glint_textarea.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string_view>
#include <string>
#include <vector>

#include "include/gpu/ganesh/GrTypes.h"

namespace {

UIView* glint_last_interaction_view = nil;
CGPoint glint_last_interaction_point = CGPointZero;

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

bool glint_node_wants_keyboard(const glint_element* node)
{
  const char* typeName = node ? node->typeName() : nullptr;
  return typeName && (std::strcmp(typeName, "text-input") == 0 || std::strcmp(typeName, "textarea") == 0);
}

bool glint_hit_targets_keyboard(const glint_element* hit)
{
  for (const glint_element* node = hit; node; node = node->mParent)
  {
    if (glint_node_wants_keyboard(node))
      return true;
  }
  return false;
}

glint_element* glint_keyboard_target_from_hit(const glint_element* hit)
{
  for (const glint_element* node = hit; node; node = node->mParent)
  {
    if (glint_node_wants_keyboard(node))
      return const_cast<glint_element*>(node);
  }
  return nullptr;
}

bool glint_hit_keeps_keyboard_focus(const glint_element* hit, const glint_element* focused)
{
  for (const glint_element* node = hit; node; node = node->mParent)
  {
    if (node == focused || glint_node_wants_keyboard(node))
      return true;
  }
  return false;
}

bool glint_inputmode_is_none(std::string_view inputmode)
{
  return inputmode == "none";
}

UITextAutocapitalizationType glint_autocapitalization_for_traits(int keyboardType, bool secureEntry)
{
  if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || secureEntry)
    return UITextAutocapitalizationTypeNone;
  return UITextAutocapitalizationTypeSentences;
}

std::string glint_lower_ascii(std::string_view value)
{
  std::string lower;
  lower.reserve(value.size());
  for (char ch : value)
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lower;
}

std::string glint_last_token_lower(std::string_view value)
{
  size_t end = value.size();
  while (end > 0 && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  size_t start = end;
  while (start > 0 && !std::isspace(static_cast<unsigned char>(value[start - 1]))) --start;
  return glint_lower_ascii(value.substr(start, end - start));
}

UITextAutocapitalizationType glint_autocapitalization_for_attribute(std::string_view autocapitalize,
                                                                    int keyboardType,
                                                                    bool secureEntry)
{
  if (secureEntry)
    return UITextAutocapitalizationTypeNone;

  const std::string value = glint_lower_ascii(autocapitalize);
  if (value.empty())
    return glint_autocapitalization_for_traits(keyboardType, secureEntry);
  if (value == "none" || value == "off")
    return UITextAutocapitalizationTypeNone;
  if (value == "words")
    return UITextAutocapitalizationTypeWords;
  if (value == "characters")
    return UITextAutocapitalizationTypeAllCharacters;
  if (value == "sentences" || value == "on")
    return UITextAutocapitalizationTypeSentences;
  return glint_autocapitalization_for_traits(keyboardType, secureEntry);
}

UITextAutocorrectionType glint_autocorrection_for_traits(int keyboardType, bool secureEntry)
{
  if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || secureEntry)
    return UITextAutocorrectionTypeNo;
  return UITextAutocorrectionTypeDefault;
}

UITextAutocorrectionType glint_autocorrection_for_spellcheck(std::string_view spellcheck,
                                                             int keyboardType,
                                                             bool secureEntry)
{
  if (secureEntry)
    return UITextAutocorrectionTypeNo;

  const std::string value = glint_lower_ascii(spellcheck);
  if (value == "false" || value == "off" || value == "no")
    return UITextAutocorrectionTypeNo;
  if (value == "true" || value == "on" || value == "yes")
    return UITextAutocorrectionTypeDefault;
  return glint_autocorrection_for_traits(keyboardType, secureEntry);
}

UITextSpellCheckingType glint_spellchecking_for_traits(int keyboardType, bool secureEntry)
{
  if (keyboardType == UIKeyboardTypeEmailAddress || keyboardType == UIKeyboardTypeDecimalPad || secureEntry)
    return UITextSpellCheckingTypeNo;
  return UITextSpellCheckingTypeDefault;
}

UITextSpellCheckingType glint_spellchecking_for_attribute(std::string_view spellcheck,
                                                          int keyboardType,
                                                          bool secureEntry)
{
  if (secureEntry)
    return UITextSpellCheckingTypeNo;

  const std::string value = glint_lower_ascii(spellcheck);
  if (value == "false" || value == "off" || value == "no")
    return UITextSpellCheckingTypeNo;
  if (value == "true" || value == "on" || value == "yes")
    return UITextSpellCheckingTypeDefault;
  return glint_spellchecking_for_traits(keyboardType, secureEntry);
}

NSString* glint_text_content_type_for_autocomplete(std::string_view autocomplete)
{
  const std::string token = glint_last_token_lower(autocomplete);
  if (token.empty() || token == "on") return nil;
  if (token == "off") return @"";
  if (token == "name") return @"name";
  if (token == "honorific-prefix") return @"namePrefix";
  if (token == "given-name") return @"givenName";
  if (token == "additional-name") return @"middleName";
  if (token == "family-name") return @"familyName";
  if (token == "nickname") return @"nickname";
  if (token == "organization") return @"organizationName";
  if (token == "street-address") return @"fullStreetAddress";
  if (token == "postal-code") return @"postalCode";
  if (token == "country-name") return @"countryName";
  if (token == "tel") return @"telephoneNumber";
  if (token == "email") return @"emailAddress";
  if (token == "username") return @"username";
  if (token == "current-password") return @"password";
  if (token == "new-password") return @"newPassword";
  if (token == "one-time-code") return @"oneTimeCode";
  if (token == "url") return @"URL";
  return nil;
}

int glint_keyboard_type_from_inputmode(std::string_view inputmode)
{
  if (inputmode == "decimal") return UIKeyboardTypeDecimalPad;
  if (inputmode == "numeric") return UIKeyboardTypeNumberPad;
  if (inputmode == "tel")     return UIKeyboardTypePhonePad;
  if (inputmode == "search")  return UIKeyboardTypeWebSearch;
  if (inputmode == "email")   return UIKeyboardTypeEmailAddress;
  if (inputmode == "url")     return UIKeyboardTypeURL;
  return UIKeyboardTypeDefault;
}

int glint_keyboard_type_for_input(const glint_text_input& input)
{
  if (!input.inputmode.empty())
    return glint_keyboard_type_from_inputmode(input.inputmode);

  if (input.type == "email")  return UIKeyboardTypeEmailAddress;
  if (input.type == "number") return UIKeyboardTypeDecimalPad;
  if (input.type == "search") return UIKeyboardTypeWebSearch;
  if (input.type == "tel")    return UIKeyboardTypePhonePad;
  if (input.type == "url")    return UIKeyboardTypeURL;
  return UIKeyboardTypeDefault;
}

int glint_keyboard_type_for_textarea(const glint_textarea& textarea)
{
  if (!textarea.inputmode.empty())
    return glint_keyboard_type_from_inputmode(textarea.inputmode);
  return UIKeyboardTypeDefault;
}

int glint_return_key_type_from_hint(std::string_view enterkeyhint)
{
  if (enterkeyhint == "enter")    return UIReturnKeyDefault;
  if (enterkeyhint == "done")     return UIReturnKeyDone;
  if (enterkeyhint == "go")       return UIReturnKeyGo;
  if (enterkeyhint == "next")     return UIReturnKeyNext;
  if (enterkeyhint == "previous") return UIReturnKeyDefault;
  if (enterkeyhint == "search")   return UIReturnKeySearch;
  if (enterkeyhint == "send")     return UIReturnKeySend;
  return UIReturnKeyDefault;
}

glint_text_editor_base* glint_focused_text_editor(glint_document* doc)
{
  if (!doc)
    return nullptr;

  return dynamic_cast<glint_text_editor_base*>(doc->getFocusedNode());
}

const glint_text_editor_base* glint_focused_text_editor(const glint_document* doc)
{
  return glint_focused_text_editor(const_cast<glint_document*>(doc));
}

const void* glint_keyboard_cpp_view_key()
{
  static int key = 0;
  return &key;
}

glint_view_ios* glint_keyboard_cpp_view(id object)
{
  NSValue* value = (NSValue*) objc_getAssociatedObject(object, glint_keyboard_cpp_view_key());
  return value ? (glint_view_ios*) value.pointerValue : nullptr;
}

void glint_set_keyboard_cpp_view(id object, glint_view_ios* cppView)
{
  if (!object)
    return;

  if (!cppView)
  {
    objc_setAssociatedObject(object, glint_keyboard_cpp_view_key(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return;
  }

  objc_setAssociatedObject(object,
                           glint_keyboard_cpp_view_key(),
                           [NSValue valueWithPointer:cppView],
                           OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

UIWindow* glint_active_window()
{
  UIApplication* app = UIApplication.sharedApplication;
  if (@available(iOS 13.0, *))
  {
    for (UIScene* scene in app.connectedScenes)
    {
      if (![scene isKindOfClass:[UIWindowScene class]])
        continue;
      if (scene.activationState != UISceneActivationStateForegroundActive)
        continue;

      UIWindowScene* windowScene = (UIWindowScene*) scene;
      for (UIWindow* window in windowScene.windows)
      {
        if (window.isKeyWindow)
          return window;
      }
      for (UIWindow* window in windowScene.windows)
      {
        if (!window.hidden)
          return window;
      }
    }
  }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (app.keyWindow)
    return app.keyWindow;
  for (UIWindow* window in app.windows)
  {
    if (!window.hidden)
      return window;
  }
#pragma clang diagnostic pop
  return nil;
}

UIViewController* glint_top_view_controller(UIViewController* controller)
{
  UIViewController* current = controller;
  while (current)
  {
    if ([current isKindOfClass:[UINavigationController class]])
    {
      UIViewController* visible = ((UINavigationController*) current).visibleViewController;
      if (visible && visible != current)
      {
        current = visible;
        continue;
      }
    }

    if ([current isKindOfClass:[UITabBarController class]])
    {
      UIViewController* selected = ((UITabBarController*) current).selectedViewController;
      if (selected && selected != current)
      {
        current = selected;
        continue;
      }
    }

    UIViewController* presented = current.presentedViewController;
    if (!presented || presented == current)
      break;
    current = presented;
  }

  return current;
}

NSString* glint_nsstring_from_utf8(const std::string& utf8)
{
  NSString* str = [NSString stringWithUTF8String:utf8.c_str()];
  return str ? str : @"";
}

std::string glint_utf8_from_nsstring(NSString* text)
{
  if (!text)
    return {};

  NSData* utf8data = [text dataUsingEncoding:NSUTF8StringEncoding allowLossyConversion:NO];
  if (!utf8data || utf8data.length == 0)
    return {};

  return std::string(static_cast<const char*>(utf8data.bytes), utf8data.length);
}

void glint_set_last_interaction(UIView* view, CGPoint point)
{
  glint_last_interaction_view = view;
  glint_last_interaction_point = point;
}

CGRect glint_centered_source_rect(UIView* sourceView)
{
  return CGRectMake(CGRectGetMidX(sourceView.bounds),
                    CGRectGetMidY(sourceView.bounds),
                    1.0,
                    1.0);
}

UIColor* glint_uicolor_from_glint_color(glint_color color)
{
  return [UIColor colorWithRed:std::clamp(color.R / 255.0f, 0.0f, 1.0f)
                         green:std::clamp(color.G / 255.0f, 0.0f, 1.0f)
                          blue:std::clamp(color.B / 255.0f, 0.0f, 1.0f)
                         alpha:std::clamp(color.A / 255.0f, 0.0f, 1.0f)];
}

glint_color glint_color_from_uicolor(UIColor* color)
{
  CGFloat red = 0.0f;
  CGFloat green = 0.0f;
  CGFloat blue = 0.0f;
  CGFloat alpha = 1.0f;
  if (![color getRed:&red green:&green blue:&blue alpha:&alpha])
  {
    CGFloat white = 0.0f;
    if ([color getWhite:&white alpha:&alpha])
      red = green = blue = white;
  }

  return glint_color(static_cast<int>(std::round(std::clamp(alpha, 0.0, 1.0) * 255.0)),
                     static_cast<int>(std::round(std::clamp(red,   0.0, 1.0) * 255.0)),
                     static_cast<int>(std::round(std::clamp(green, 0.0, 1.0) * 255.0)),
                     static_cast<int>(std::round(std::clamp(blue,  0.0, 1.0) * 255.0)));
}

CGRect glint_colorpicker_source_rect(UIView* sourceView, RECT anchorScreenRect)
{
  if (glint_last_interaction_view)
  {
    CGPoint point = [sourceView convertPoint:glint_last_interaction_point fromView:glint_last_interaction_view];
    return CGRectMake(point.x, point.y, 1.0f, 1.0f);
  }

  const CGFloat width = std::max<CGFloat>(1.0f, static_cast<CGFloat>(anchorScreenRect.right - anchorScreenRect.left));
  const CGFloat height = std::max<CGFloat>(1.0f, static_cast<CGFloat>(anchorScreenRect.bottom - anchorScreenRect.top));
  if (sourceView.window && width > 0.0f && height > 0.0f)
  {
    CGRect screenRect = CGRectMake(static_cast<CGFloat>(anchorScreenRect.left),
                                   static_cast<CGFloat>(anchorScreenRect.top),
                                   width,
                                   height);
    CGRect localRect = [sourceView convertRect:screenRect fromCoordinateSpace:sourceView.window.screen.coordinateSpace];
    if (!CGRectIsNull(localRect) && !CGRectIsEmpty(localRect))
      return localRect;
  }

  return glint_centered_source_rect(sourceView);
}

} // namespace

@class GlintKeyboardProxyField;
@class GlintKeyboardSearchField;
@class GlintIOSMenuTracker;
@class GlintIOSMenuItem;
@class GlintIOSMenuListController;
@class GlintIOSSelectPickerCoordinator;
@class GlintIOSSelectMenuControl;
@class GlintIOSColorPickerCoordinator;

@interface GlintIOSView : UIView <UIGestureRecognizerDelegate, UIKeyInput, UITextInputTraits, UITextFieldDelegate>
{
@public
  glint_view_ios* cppView;
  UIPinchGestureRecognizer* pinchRecognizer;
  UIRotationGestureRecognizer* rotationRecognizer;
  UIPanGestureRecognizer* twoFingerPanRecognizer;
  UILongPressGestureRecognizer* editMenuRecognizer;
  GlintKeyboardProxyField* keyboardProxyField;
  UISearchBar* keyboardSearchBar;
  int lastKeyboardType;
  int lastReturnKeyType;
  int lastAutocapitalizationType;
  int lastAutocorrectionType;
  int lastSpellCheckingType;
  NSString* lastTextContentType;
  BOOL lastSecureEntry;
  BOOL lastWantedKeyboard;
  BOOL lastSuppressesSoftwareKeyboard;
  BOOL lastUsesSearchResponder;
  BOOL suppressKeyboardFieldSync;
  BOOL keyboardPrewarmScheduled;
  BOOL keyboardPrewarmActive;
  BOOL keyboardPrewarmDone;
}
- (instancetype)initWithView:(glint_view_ios*)view frame:(CGRect)frame;
- (void)displayLinkFired:(CADisplayLink*)displayLink;
- (void)handleEditMenuLongPress:(UILongPressGestureRecognizer*)recognizer;
- (void)handleKeyboardFieldEditingChanged:(UITextField*)sender;
- (void)handleKeyboardFieldTextDidChangeNotification:(NSNotification*)notification;
- (void)prewarmKeyboardHostIfNeeded;
- (void)syncKeyboardFocus;
@end

@interface GlintKeyboardProxyField : UITextField
{
@public
  glint_view_ios* cppView;
}
@end

@interface GlintKeyboardSearchField : UISearchTextField
@end

@interface GlintIOSMenuTracker : NSObject
{
@public
  int selectedId;
  BOOL finished;
}
@end

@interface GlintIOSMenuItem : NSObject
{
@public
  int itemId;
  NSString* title;
  BOOL enabled;
  BOOL checked;
  BOOL separator;
}
+ (instancetype)itemWithId:(int)itemId
                     title:(NSString*)title
                   enabled:(BOOL)enabled
                   checked:(BOOL)checked
                 separator:(BOOL)separator;
@end

@interface GlintIOSMenuListController : UITableViewController <UIPopoverPresentationControllerDelegate>
{
@public
  GlintIOSMenuTracker* tracker;
  NSArray<GlintIOSMenuItem*>* menuItems;
}
- (instancetype)initWithItems:(NSArray<GlintIOSMenuItem*>*)items
                      tracker:(GlintIOSMenuTracker*)menuTracker;
- (void)close:(id)sender;
@end

@interface GlintIOSSelectPickerCoordinator : NSObject
{
@public
  GlintIOSMenuTracker* tracker;
  NSArray<GlintIOSMenuItem*>* menuItems;
  UIView* hostView;
  GlintIOSSelectMenuControl* menuControl;
  CGPoint sourcePoint;
  BOOL menuTriggered;
  BOOL awaitingDismissCleanup;
}
- (instancetype)initWithItems:(NSArray<GlintIOSMenuItem*>*)items
                   selectedId:(int)selectedId
                      tracker:(GlintIOSMenuTracker*)menuTracker;
- (void)presentInView:(UIView*)view sourcePoint:(CGPoint)point;
- (BOOL)isAwaitingDismissal;
- (UIMenu*)buildMenu;
- (void)menuDidDisplay;
- (void)menuDidDismiss;
- (void)failIfMenuDidNotAppear;
@end

@interface GlintIOSSelectMenuControl : UIControl
{
@public
  GlintIOSSelectPickerCoordinator* owner;
  CGPoint attachmentPoint;
}
- (instancetype)initWithFrame:(CGRect)frame
                        owner:(GlintIOSSelectPickerCoordinator*)menuOwner
              attachmentPoint:(CGPoint)point;
@end

@interface GlintIOSColorPickerCoordinator : NSObject <UIColorPickerViewControllerDelegate, UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIColorPickerViewController* picker;
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  std::function<void(glint_color)> onChange;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithColor:(UIColor*)color anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
@end

@implementation GlintKeyboardProxyField

- (BOOL)canBecomeFirstResponder
{
  return YES;
}

- (CGRect)textRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)editingRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)placeholderRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)caretRectForPosition:(UITextPosition*)position
{
  (void)position;
  return CGRectZero;
}

- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range
{
  (void)range;
  return @[];
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
}

- (BOOL)hasText
{
  return cppView ? cppView->_focusedNodeHasText() : NO;
}

- (UIView*)inputView
{
  if (!cppView || !cppView->_focusedSuppressesSoftwareKeyboard())
    return nil;

  static UIView* emptyInputView = nil;
  if (!emptyInputView)
    emptyInputView = [[UIView alloc] initWithFrame:CGRectZero];
  return emptyInputView;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
  (void)sender;
  if (!cppView)
    return NO;

  if (action == @selector(cut:))       return cppView->_focusedCanCut() ? YES : NO;
  if (action == @selector(copy:))      return cppView->_focusedCanCopy() ? YES : NO;
  if (action == @selector(paste:))     return cppView->_focusedCanPaste() ? YES : NO;
  if (action == @selector(selectAll:)) return cppView->_focusedCanSelectAll() ? YES : NO;
  return NO;
}

- (void)cut:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedCut();
}

- (void)copy:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedCopy();
}

- (void)paste:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedPaste();
}

- (void)selectAll:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedSelectAll();
}

- (void)insertText:(NSString*)text
{
  [super insertText:text];
}

- (void)deleteBackward
{
  [super deleteBackward];
}

@end

@implementation GlintIOSMenuTracker

@end

@implementation GlintIOSMenuItem

+ (instancetype)itemWithId:(int)itemIdValue
                     title:(NSString*)titleValue
                   enabled:(BOOL)enabledValue
                   checked:(BOOL)checkedValue
                 separator:(BOOL)separatorValue
{
  GlintIOSMenuItem* item = [[[self alloc] init] autorelease];
  item->itemId = itemIdValue;
  item->title = [titleValue copy];
  item->enabled = enabledValue;
  item->checked = checkedValue;
  item->separator = separatorValue;
  return item;
}

- (void)dealloc
{
  [title release];
  [super dealloc];
}

@end

@implementation GlintIOSMenuListController

- (instancetype)initWithItems:(NSArray<GlintIOSMenuItem*>*)items
                      tracker:(GlintIOSMenuTracker*)menuTracker
{
  if (!(self = [super initWithStyle:UITableViewStylePlain]))
    return nil;

  menuItems = [items copy];
  tracker = [menuTracker retain];
  self.title = @"Options";
  return self;
}

- (void)dealloc
{
  [menuItems release];
  [tracker release];
  [super dealloc];
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  self.navigationItem.rightBarButtonItem = [[[UIBarButtonItem alloc]
    initWithBarButtonSystemItem:UIBarButtonSystemItemClose
                         target:self
                         action:@selector(close:)] autorelease];
  self.tableView.tableFooterView = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
}

- (void)viewDidDisappear:(BOOL)animated
{
  [super viewDidDisappear:animated];
  if (!tracker->finished)
    tracker->finished = YES;
}

- (void)viewDidLayoutSubviews
{
  [super viewDidLayoutSubviews];
  CGFloat preferredHeight = 1.0f;
  for (GlintIOSMenuItem* item in menuItems)
    preferredHeight += item->separator ? 12.0f : 50.0f;
  preferredHeight = std::min<CGFloat>(std::max<CGFloat>(preferredHeight, 80.0f), 420.0f);
  self.preferredContentSize = CGSizeMake(320.0f, preferredHeight);
}

- (void)close:(id)sender
{
  (void)sender;
  tracker->finished = YES;
  [self.navigationController dismissViewControllerAnimated:YES completion:nil];
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
                                                             traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

- (NSInteger)tableView:(UITableView*)tableView numberOfRowsInSection:(NSInteger)section
{
  (void)tableView;
  (void)section;
  return (NSInteger)menuItems.count;
}

- (CGFloat)tableView:(UITableView*)tableView heightForRowAtIndexPath:(NSIndexPath*)indexPath
{
  (void)tableView;
  GlintIOSMenuItem* item = [menuItems objectAtIndex:(NSUInteger)indexPath.row];
  return item->separator ? 12.0f : 50.0f;
}

- (NSIndexPath*)tableView:(UITableView*)tableView willSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  (void)tableView;
  GlintIOSMenuItem* item = [menuItems objectAtIndex:(NSUInteger)indexPath.row];
  return (item->separator || !item->enabled) ? nil : indexPath;
}

- (UITableViewCell*)tableView:(UITableView*)tableView cellForRowAtIndexPath:(NSIndexPath*)indexPath
{
  GlintIOSMenuItem* item = [menuItems objectAtIndex:(NSUInteger)indexPath.row];
  if (item->separator)
  {
    static NSString* separatorReuseId = @"GlintIOSMenuSeparatorCell";
    UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:separatorReuseId];
    if (!cell)
    {
      cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:separatorReuseId] autorelease];
      UIView* line = [[[UIView alloc] initWithFrame:CGRectMake(16.0f, 5.5f, 288.0f, 1.0f)] autorelease];
      line.autoresizingMask = UIViewAutoresizingFlexibleWidth;
      line.backgroundColor = [UIColor colorWithWhite:0.85f alpha:1.0f];
      [cell.contentView addSubview:line];
    }
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.userInteractionEnabled = NO;
    cell.backgroundColor = [UIColor clearColor];
    cell.contentView.backgroundColor = [UIColor clearColor];
    return cell;
  }

  static NSString* itemReuseId = @"GlintIOSMenuItemCell";
  UITableViewCell* cell = [tableView dequeueReusableCellWithIdentifier:itemReuseId];
  if (!cell)
    cell = [[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:itemReuseId] autorelease];

  cell.textLabel.text = item->title;
  cell.textLabel.enabled = item->enabled;
  cell.textLabel.textColor = item->enabled
    ? [UIColor colorWithWhite:0.05f alpha:1.0f]
    : [UIColor colorWithWhite:0.05f alpha:0.35f];
  cell.accessoryType = item->checked ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
  cell.selectionStyle = item->enabled ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
  cell.userInteractionEnabled = item->enabled;
  return cell;
}

- (void)tableView:(UITableView*)tableView didSelectRowAtIndexPath:(NSIndexPath*)indexPath
{
  [tableView deselectRowAtIndexPath:indexPath animated:YES];
  GlintIOSMenuItem* item = [menuItems objectAtIndex:(NSUInteger)indexPath.row];
  if (item->separator || !item->enabled)
    return;

  tracker->selectedId = item->itemId;
  tracker->finished = YES;
  [self.navigationController dismissViewControllerAnimated:YES completion:nil];
}

@end

@implementation GlintIOSSelectMenuControl

- (instancetype)initWithFrame:(CGRect)frame
                        owner:(GlintIOSSelectPickerCoordinator*)menuOwner
              attachmentPoint:(CGPoint)point
{
  if (!(self = [super initWithFrame:frame]))
    return nil;

  owner = menuOwner;
  attachmentPoint = point;
  self.alpha = 0.01f;
  self.opaque = NO;
  self.backgroundColor = [UIColor clearColor];
  self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.contextMenuInteractionEnabled = YES;
  self.showsMenuAsPrimaryAction = YES;
  return self;
}

- (CGPoint)menuAttachmentPointForConfiguration:(UIContextMenuConfiguration*)configuration
{
  (void)configuration;
  return attachmentPoint;
}

- (UIContextMenuConfiguration*)contextMenuInteraction:(UIContextMenuInteraction*)interaction
                      configurationForMenuAtLocation:(CGPoint)location
{
  (void)interaction;
  (void)location;

  UIContextMenuConfiguration* configuration = [UIContextMenuConfiguration configurationWithIdentifier:nil
                                                                                       previewProvider:nil
                                                                                        actionProvider:^UIMenu*(NSArray<UIMenuElement*>* suggestedActions) {
    (void)suggestedActions;
    return [owner buildMenu];
  }];

  if (@available(iOS 16.0, *))
    configuration.preferredMenuElementOrder = UIContextMenuConfigurationElementOrderFixed;

  return configuration;
}

- (UIView*)hitTest:(CGPoint)point withEvent:(UIEvent*)event
{
  if (owner && owner->menuTriggered)
    return nil;
  return [super hitTest:point withEvent:event];
}

- (void)contextMenuInteraction:(UIContextMenuInteraction*)interaction
willDisplayMenuForConfiguration:(UIContextMenuConfiguration*)configuration
                       animator:(id<UIContextMenuInteractionAnimating>)animator
{
  [super contextMenuInteraction:interaction willDisplayMenuForConfiguration:configuration animator:animator];
  [owner menuDidDisplay];
}

- (void)contextMenuInteraction:(UIContextMenuInteraction*)interaction
       willEndForConfiguration:(UIContextMenuConfiguration*)configuration
                       animator:(id<UIContextMenuInteractionAnimating>)animator
{
  [super contextMenuInteraction:interaction willEndForConfiguration:configuration animator:animator];
  [animator addCompletion:^{
    [owner menuDidDismiss];
  }];
}

@end

@implementation GlintIOSSelectPickerCoordinator

- (instancetype)initWithItems:(NSArray<GlintIOSMenuItem*>*)items
                   selectedId:(int)selectedId
                      tracker:(GlintIOSMenuTracker*)menuTracker
{
  if (!(self = [super init]))
    return nil;

  menuItems = [items copy];
  tracker = [menuTracker retain];
  hostView = nil;
  menuControl = nil;
  sourcePoint = CGPointZero;
  menuTriggered = NO;
  awaitingDismissCleanup = NO;

  for (NSUInteger index = 0; index < menuItems.count; ++index)
    [menuItems objectAtIndex:index]->checked = ([menuItems objectAtIndex:index]->itemId == selectedId);

  return self;
}

- (void)dealloc
{
  [menuControl removeFromSuperview];
  [menuControl release];
  [hostView release];
  [menuItems release];
  [tracker release];
  [super dealloc];
}

- (void)presentInView:(UIView*)view sourcePoint:(CGPoint)point
{
  if (!view)
  {
    tracker->finished = YES;
    return;
  }

  if (@available(iOS 17.4, *))
  {
    hostView = [view retain];
    sourcePoint = point;
    menuTriggered = NO;

    menuControl = [[GlintIOSSelectMenuControl alloc] initWithFrame:hostView.bounds
                                                             owner:self
                                                   attachmentPoint:sourcePoint];
    [hostView addSubview:menuControl];
    [menuControl performPrimaryAction];
    dispatch_async(dispatch_get_main_queue(), ^{
      [self failIfMenuDidNotAppear];
    });
    return;
  }

  tracker->finished = YES;
}

- (UIMenu*)buildMenu
{
  NSMutableArray<UIMenuElement*>* actions = [NSMutableArray arrayWithCapacity:menuItems.count];
  for (GlintIOSMenuItem* item in menuItems)
  {
    UIMenuElementAttributes attributes = item->enabled ? 0 : UIMenuElementAttributesDisabled;
    UIAction* action = [UIAction actionWithTitle:item->title image:nil identifier:nil handler:^(__kindof UIAction* selectedAction) {
      (void)selectedAction;
      if (!awaitingDismissCleanup)
      {
        awaitingDismissCleanup = YES;
        [self retain];
      }
      tracker->selectedId = item->itemId;
      tracker->finished = YES;
    }];
    action.attributes = attributes;
    action.state = item->checked ? UIMenuElementStateOn : UIMenuElementStateOff;
    [actions addObject:action];
  }
  UIMenu* menu = [UIMenu menuWithTitle:@""
                                 image:nil
                            identifier:nil
                               options:0
                              children:actions];
  if (@available(iOS 16.0, *))
    menu.preferredElementSize = UIMenuElementSizeLarge;
  return menu;
}

- (void)menuDidDisplay
{
  menuTriggered = YES;
}

- (void)menuDidDismiss
{
  [menuControl removeFromSuperview];
  [menuControl release];
  menuControl = nil;

  [hostView release];
  hostView = nil;

  tracker->finished = YES;

  if (awaitingDismissCleanup)
  {
    awaitingDismissCleanup = NO;
    [self release];
  }
}

- (void)failIfMenuDidNotAppear
{
  if (menuTriggered || !menuControl)
    return;

  [self menuDidDismiss];
}

- (BOOL)isAwaitingDismissal
{
  return menuControl != nil;
}

@end

@implementation GlintIOSColorPickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  picker = nil;
  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [picker release];
  [presenter release];
  [sourceView release];
  [super dealloc];
}

- (void)notifyClosedIfNeeded
{
  if (closedNotified)
    return;
  closedNotified = YES;
  if (onClosed)
    onClosed();
}

- (void)presentWithColor:(UIColor*)color anchorScreenRect:(RECT)anchorScreenRect
{
  if (@available(iOS 14.0, *))
  {
    UIWindow* window = glint_active_window();
    UIViewController* top = glint_top_view_controller(window.rootViewController);
    if (!top)
      top = window.rootViewController;
    if (!top)
    {
      [self notifyClosedIfNeeded];
      return;
    }

    if (!picker)
    {
      picker = [[UIColorPickerViewController alloc] init];
      picker.delegate = self;
      picker.supportsAlpha = YES;
    }

    [presenter release];
    presenter = [top retain];

    UIView* resolvedSourceView = presenter.view ?: window;
    [sourceView release];
    sourceView = [resolvedSourceView retain];
    sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);

    picker.selectedColor = color ?: UIColor.blackColor;
    closedNotified = NO;

    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad)
    {
      picker.modalPresentationStyle = UIModalPresentationPopover;
      UIPopoverPresentationController* popover = picker.popoverPresentationController;
      popover.delegate = self;
      popover.sourceView = sourceView;
      popover.sourceRect = sourceRect;
      popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }

    if (picker.presentingViewController)
      return;

    [presenter presentViewController:picker animated:YES completion:nil];
    return;
  }

  [self notifyClosedIfNeeded];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  if (picker.presentingViewController)
  {
    [picker.presentingViewController dismissViewControllerAnimated:animated completion:^{
      if (notifyClosed)
        [self notifyClosedIfNeeded];
    }];
    return;
  }

  if (notifyClosed)
    [self notifyClosedIfNeeded];
}

- (void)destroy
{
  onChange = nullptr;
  onClosed = nullptr;
  if (picker)
    picker.delegate = nil;
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)colorPickerViewControllerDidSelectColor:(UIColorPickerViewController*)viewController API_AVAILABLE(ios(14.0))
{
  if (onChange)
    onChange(glint_color_from_uicolor(viewController.selectedColor));
}

- (void)colorPickerViewControllerDidFinish:(UIColorPickerViewController*)viewController API_AVAILABLE(ios(14.0))
{
  (void)viewController;
  [self notifyClosedIfNeeded];
}

- (void)popoverPresentationControllerDidDismissPopover:(UIPopoverPresentationController*)popoverPresentationController
{
  (void)popoverPresentationController;
  [self notifyClosedIfNeeded];
}

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController
{
  (void)presentationController;
  [self notifyClosedIfNeeded];
}

@end

@implementation GlintKeyboardSearchField

- (CGRect)textRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)editingRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)placeholderRectForBounds:(CGRect)bounds
{
  (void)bounds;
  return CGRectZero;
}

- (CGRect)caretRectForPosition:(UITextPosition*)position
{
  (void)position;
  return CGRectZero;
}

- (NSArray<UITextSelectionRect*>*)selectionRectsForRange:(UITextRange*)range
{
  (void)range;
  return @[];
}

- (void)drawRect:(CGRect)rect
{
  (void)rect;
}

- (BOOL)hasText
{
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  return cppView ? cppView->_focusedNodeHasText() : NO;
}

- (UIView*)inputView
{
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (!cppView || !cppView->_focusedSuppressesSoftwareKeyboard())
    return nil;

  static UIView* emptyInputView = nil;
  if (!emptyInputView)
    emptyInputView = [[UIView alloc] initWithFrame:CGRectZero];
  return emptyInputView;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
  (void)sender;
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (!cppView)
    return NO;

  if (action == @selector(cut:))       return cppView->_focusedCanCut() ? YES : NO;
  if (action == @selector(copy:))      return cppView->_focusedCanCopy() ? YES : NO;
  if (action == @selector(paste:))     return cppView->_focusedCanPaste() ? YES : NO;
  if (action == @selector(selectAll:)) return cppView->_focusedCanSelectAll() ? YES : NO;
  return NO;
}

- (void)cut:(id)sender
{
  (void)sender;
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (cppView)
    cppView->_focusedCut();
}

- (void)copy:(id)sender
{
  (void)sender;
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (cppView)
    cppView->_focusedCopy();
}

- (void)paste:(id)sender
{
  (void)sender;
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (cppView)
    cppView->_focusedPaste();
}

- (void)selectAll:(id)sender
{
  (void)sender;
  glint_view_ios* cppView = glint_keyboard_cpp_view(self);
  if (cppView)
    cppView->_focusedSelectAll();
}

- (void)insertText:(NSString*)text
{
  [super insertText:text];
}

- (void)deleteBackward
{
  [super deleteBackward];
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

    editMenuRecognizer = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(handleEditMenuLongPress:)];
    editMenuRecognizer.minimumPressDuration = 0.4;
    editMenuRecognizer.delegate = self;
    [self addGestureRecognizer:editMenuRecognizer];

    keyboardProxyField = [[GlintKeyboardProxyField alloc] initWithFrame:CGRectMake(0.0, 0.0, 1.0, 1.0)];
    keyboardProxyField->cppView = view;
    keyboardProxyField.alpha = 1.0;
    keyboardProxyField.backgroundColor = UIColor.clearColor;
    keyboardProxyField.borderStyle = UITextBorderStyleNone;
    keyboardProxyField.delegate = self;
    keyboardProxyField.tintColor = UIColor.clearColor;
    keyboardProxyField.textColor = UIColor.clearColor;
    keyboardProxyField.clipsToBounds = YES;
    [keyboardProxyField addTarget:self action:@selector(handleKeyboardFieldEditingChanged:) forControlEvents:UIControlEventEditingChanged];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(handleKeyboardFieldTextDidChangeNotification:)
                                               name:UITextFieldTextDidChangeNotification
                                             object:keyboardProxyField];
    [self addSubview:keyboardProxyField];

    // Keep the search responder in the hierarchy for keyboard traits, but park
    // it well outside the visible viewport so UIKit does not paint its chrome.
    keyboardSearchBar = [[UISearchBar alloc] initWithFrame:CGRectMake(-1000.0, -1000.0, 1.0, 1.0)];
    keyboardSearchBar.alpha = 1.0;
    keyboardSearchBar.backgroundColor = UIColor.clearColor;
    keyboardSearchBar.barTintColor = UIColor.clearColor;
    keyboardSearchBar.tintColor = UIColor.clearColor;
    keyboardSearchBar.searchBarStyle = UISearchBarStyleMinimal;
    UISearchTextField* nativeSearchField = keyboardSearchBar.searchTextField;
    object_setClass(nativeSearchField, [GlintKeyboardSearchField class]);
    glint_set_keyboard_cpp_view(nativeSearchField, view);
    nativeSearchField.delegate = self;
    nativeSearchField.backgroundColor = UIColor.clearColor;
    nativeSearchField.textColor = UIColor.clearColor;
    nativeSearchField.tintColor = UIColor.clearColor;
    nativeSearchField.clipsToBounds = YES;
    [nativeSearchField addTarget:self action:@selector(handleKeyboardFieldEditingChanged:) forControlEvents:UIControlEventEditingChanged];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(handleKeyboardFieldTextDidChangeNotification:)
                                               name:UITextFieldTextDidChangeNotification
                                             object:nativeSearchField];
    [self addSubview:keyboardSearchBar];

    lastKeyboardType = UIKeyboardTypeDefault;
    lastReturnKeyType = UIReturnKeyDefault;
    lastAutocapitalizationType = static_cast<int>(UITextAutocapitalizationTypeSentences);
    lastAutocorrectionType = static_cast<int>(UITextAutocorrectionTypeDefault);
    lastSpellCheckingType = static_cast<int>(UITextSpellCheckingTypeDefault);
    lastTextContentType = nil;
    lastSecureEntry = NO;
    lastWantedKeyboard = NO;
    lastSuppressesSoftwareKeyboard = NO;
    lastUsesSearchResponder = NO;
    suppressKeyboardFieldSync = NO;
    keyboardPrewarmScheduled = NO;
    keyboardPrewarmActive = NO;
    keyboardPrewarmDone = NO;
  }
  return self;
}

- (void)dealloc
{
  [NSNotificationCenter.defaultCenter removeObserver:self name:UITextFieldTextDidChangeNotification object:keyboardProxyField];
  [NSNotificationCenter.defaultCenter removeObserver:self name:UITextFieldTextDidChangeNotification object:keyboardSearchBar.searchTextField];
  keyboardProxyField->cppView = nullptr;
  glint_set_keyboard_cpp_view(keyboardSearchBar.searchTextField, nullptr);
  [lastTextContentType release];
  [keyboardProxyField release];
  [keyboardSearchBar release];
  [pinchRecognizer release];
  [rotationRecognizer release];
  [twoFingerPanRecognizer release];
  [editMenuRecognizer release];
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

- (UIView*)inputView
{
  if (keyboardPrewarmActive)
  {
    static UIView* emptyInputView = nil;
    if (!emptyInputView)
      emptyInputView = [[UIView alloc] initWithFrame:CGRectZero];
    return emptyInputView;
  }

  return [keyboardProxyField inputView];
}

- (void)didMoveToWindow
{
  [super didMoveToWindow];
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender
{
  (void)sender;
  if (!cppView)
    return NO;

  if (action == @selector(cut:))       return cppView->_focusedCanCut() ? YES : NO;
  if (action == @selector(copy:))      return cppView->_focusedCanCopy() ? YES : NO;
  if (action == @selector(paste:))     return cppView->_focusedCanPaste() ? YES : NO;
  if (action == @selector(selectAll:)) return cppView->_focusedCanSelectAll() ? YES : NO;
  return NO;
}

- (void)cut:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedCut();
}

- (void)copy:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedCopy();
}

- (void)paste:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedPaste();
}

- (void)selectAll:(id)sender
{
  (void)sender;
  if (cppView)
    cppView->_focusedSelectAll();
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
  return (UITextAutocapitalizationType) cppView->_focusedAutocapitalizationType();
}

- (UITextAutocorrectionType)autocorrectionType
{
  return cppView ? (UITextAutocorrectionType) cppView->_focusedAutocorrectionType()
                 : UITextAutocorrectionTypeDefault;
}

- (UITextSpellCheckingType)spellCheckingType
{
  return cppView ? (UITextSpellCheckingType) cppView->_focusedSpellCheckingType()
                 : UITextSpellCheckingTypeDefault;
}

- (UITextContentType)textContentType
{
  if (!cppView)
    return nil;
  return glint_text_content_type_for_autocomplete(cppView->_focusedAutocomplete());
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

- (void)prewarmKeyboardHostIfNeeded
{
  keyboardPrewarmScheduled = NO;
  keyboardPrewarmActive = NO;
  keyboardPrewarmDone = YES;
}

- (void)handleEditMenuLongPress:(UILongPressGestureRecognizer*)recognizer
{
  if (!cppView || recognizer.state != UIGestureRecognizerStateBegan)
    return;

  if (!cppView->_focusedNodeWantsKeyboard())
    return;

  const BOOL wantsSearchResponder = cppView->_focusedReturnKeyType() == UIReturnKeySearch;
  const BOOL wantsProxyResponder = cppView->_focusedNeedsNativeTextServices() && !wantsSearchResponder;
  if (wantsSearchResponder)
  {
    if (![keyboardSearchBar.searchTextField isFirstResponder])
      [keyboardSearchBar.searchTextField becomeFirstResponder];
  }
  else if (wantsProxyResponder)
  {
    if (![keyboardProxyField isFirstResponder])
      [keyboardProxyField becomeFirstResponder];
  }
  else if (![self isFirstResponder])
  {
    [self becomeFirstResponder];
  }

  const bool hasActions = cppView->_focusedCanCut() || cppView->_focusedCanCopy()
                       || cppView->_focusedCanPaste() || cppView->_focusedCanSelectAll();
  if (!hasActions)
    return;

  const CGPoint pt = [recognizer locationInView:self];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  UIMenuController* menu = [UIMenuController sharedMenuController];
  [menu setTargetRect:CGRectMake(pt.x, pt.y, 1.0, 1.0) inView:self];
  [menu setMenuVisible:YES animated:YES];
#pragma clang diagnostic pop
}

- (void)handleKeyboardFieldEditingChanged:(UITextField*)sender
{
  if (!cppView || suppressKeyboardFieldSync)
    return;

  cppView->_replaceFocusedTextFromPlatform(glint_utf8_from_nsstring(sender.text));

  const std::string syncedValue = cppView->_focusedTextValue();
  NSString* syncedText = glint_nsstring_from_utf8(syncedValue);
  NSString* currentText = sender.text ?: @"";
  if (![currentText isEqualToString:syncedText])
  {
    suppressKeyboardFieldSync = YES;
    sender.text = syncedText;
    suppressKeyboardFieldSync = NO;
  }
}

- (void)handleKeyboardFieldTextDidChangeNotification:(NSNotification*)notification
{
  id object = notification.object;
  if (![object isKindOfClass:[UITextField class]])
    return;
  [self handleKeyboardFieldEditingChanged:(UITextField*) object];
}

- (BOOL)textFieldShouldReturn:(UITextField*)textField
{
  (void)textField;
  if (cppView)
    cppView->_handleReturnKey();
  return NO;
}

- (void)textFieldDidChangeSelection:(UITextField*)textField
{
  [self handleKeyboardFieldEditingChanged:textField];
}

- (void)syncKeyboardFocus
{
  if (!cppView)
    return;

  if (keyboardPrewarmActive && cppView->_focusedNodeWantsKeyboard())
    keyboardPrewarmActive = NO;

  const bool wantsKeyboard = cppView->_focusedNodeWantsKeyboard();
  const bool wantsNativeResponder = wantsKeyboard && cppView->_focusedNeedsNativeTextServices();
  const BOOL wantsSearchResponder = wantsKeyboard && cppView->_focusedReturnKeyType() == UIReturnKeySearch;
  const bool viewResponder = [self isFirstResponder];
  const bool proxyFieldResponder = [keyboardProxyField isFirstResponder];
  UISearchTextField* searchField = keyboardSearchBar.searchTextField;
  const bool searchResponder = [searchField isFirstResponder];
  const int keyboardType = cppView->_focusedKeyboardType();
  const int returnKeyType = cppView->_focusedReturnKeyType();
  const int autocapitalizationType = cppView->_focusedAutocapitalizationType();
  const int autocorrectionType = cppView->_focusedAutocorrectionType();
  const int spellCheckingType = cppView->_focusedSpellCheckingType();
  NSString* textContentType = glint_text_content_type_for_autocomplete(cppView->_focusedAutocomplete());
  const std::string focusedTextValue = cppView->_focusedTextValue();
  NSString* focusedText = glint_nsstring_from_utf8(focusedTextValue);
  const glint_rect focusedPaintRect = cppView->_focusedPaintRect();
  const BOOL secureEntry = cppView->_focusedSecureEntry() ? YES : NO;
  const BOOL suppressesSoftwareKeyboard = cppView->_focusedSuppressesSoftwareKeyboard() ? YES : NO;
  const CGFloat hostW = CGRectGetWidth(self.bounds);
  const CGFloat hostH = CGRectGetHeight(self.bounds);
  const CGFloat anchorW = std::clamp<CGFloat>(focusedPaintRect.R - focusedPaintRect.L, 24.0, std::max<CGFloat>(24.0, hostW));
  const CGFloat anchorH = std::clamp<CGFloat>(focusedPaintRect.B - focusedPaintRect.T, 24.0, std::max<CGFloat>(24.0, hostH));
  const CGFloat anchorX = std::clamp<CGFloat>(focusedPaintRect.L, 0.0, std::max<CGFloat>(0.0, hostW - anchorW));
  const CGFloat anchorY = std::clamp<CGFloat>(focusedPaintRect.T, 0.0, std::max<CGFloat>(0.0, hostH - anchorH));
  const CGRect activeAnchorRect = CGRectMake(anchorX, anchorY, anchorW, anchorH);
  const CGRect parkedAnchorRect = CGRectMake(-1000.0, -1000.0, 1.0, 1.0);
  keyboardProxyField.frame = wantsNativeResponder ? activeAnchorRect : parkedAnchorRect;
  keyboardSearchBar.frame = wantsSearchResponder ? activeAnchorRect : parkedAnchorRect;
  keyboardProxyField.keyboardType = (UIKeyboardType) keyboardType;
  keyboardProxyField.returnKeyType = (UIReturnKeyType) returnKeyType;
  keyboardProxyField.secureTextEntry = secureEntry;
  keyboardProxyField.autocapitalizationType = (UITextAutocapitalizationType) autocapitalizationType;
  keyboardProxyField.autocorrectionType = (UITextAutocorrectionType) autocorrectionType;
  keyboardProxyField.spellCheckingType = (UITextSpellCheckingType) spellCheckingType;
  keyboardProxyField.textContentType = textContentType;
  keyboardProxyField.keyboardAppearance = UIKeyboardAppearanceDark;
  keyboardProxyField.enablesReturnKeyAutomatically = NO;
  searchField.keyboardType = (UIKeyboardType) keyboardType;
  searchField.returnKeyType = (UIReturnKeyType) returnKeyType;
  searchField.secureTextEntry = secureEntry;
  searchField.autocapitalizationType = (UITextAutocapitalizationType) autocapitalizationType;
  searchField.autocorrectionType = (UITextAutocorrectionType) autocorrectionType;
  searchField.spellCheckingType = (UITextSpellCheckingType) spellCheckingType;
  searchField.textContentType = textContentType;
  searchField.keyboardAppearance = UIKeyboardAppearanceDark;
  searchField.enablesReturnKeyAutomatically = NO;
  const BOOL textContentTypeChanged = (lastTextContentType == nil) != (textContentType == nil)
                                   || (lastTextContentType && textContentType
                                       && ![lastTextContentType isEqualToString:textContentType]);
  const BOOL traitsChanged = lastWantedKeyboard != wantsKeyboard
                          || lastKeyboardType != keyboardType
                          || lastReturnKeyType != returnKeyType
                          || lastAutocapitalizationType != autocapitalizationType
                          || lastAutocorrectionType != autocorrectionType
                          || lastSpellCheckingType != spellCheckingType
                          || textContentTypeChanged
                          || lastSecureEntry != secureEntry
                          || lastSuppressesSoftwareKeyboard != suppressesSoftwareKeyboard
                          || lastUsesSearchResponder != wantsSearchResponder;

  auto syncHiddenField = ^(UITextField* field) {
    suppressKeyboardFieldSync = YES;
    NSString* currentText = field.text ?: @"";
    if (![currentText isEqualToString:focusedText])
      field.text = focusedText;
    suppressKeyboardFieldSync = NO;
  };

  if (wantsKeyboard)
  {
    if (!self.window)
      return;

    if (wantsSearchResponder)
    {
      if (viewResponder)
        [self resignFirstResponder];
      if (proxyFieldResponder)
        [keyboardProxyField resignFirstResponder];

      syncHiddenField(searchField);

      if (!searchResponder)
        [searchField becomeFirstResponder];
      else if (traitsChanged)
        [searchField reloadInputViews];
    }
    else if (wantsNativeResponder)
    {
      if (searchResponder)
        [keyboardSearchBar.searchTextField resignFirstResponder];
      if (viewResponder)
        [self resignFirstResponder];

      syncHiddenField(keyboardProxyField);

      if (!proxyFieldResponder)
        [keyboardProxyField becomeFirstResponder];
      else if (traitsChanged)
        [keyboardProxyField reloadInputViews];
    }
    else
    {
      if (searchResponder)
        [keyboardSearchBar.searchTextField resignFirstResponder];
      if (proxyFieldResponder)
        [keyboardProxyField resignFirstResponder];

      if (!viewResponder)
        [self becomeFirstResponder];
      else if (traitsChanged)
        [self reloadInputViews];
    }

  }
  else if (viewResponder || proxyFieldResponder || searchResponder)
  {
    [keyboardSearchBar.searchTextField resignFirstResponder];
    [keyboardProxyField resignFirstResponder];
    [self resignFirstResponder];
    [self endEditing:YES];
    [self.window endEditing:YES];
  }

  lastWantedKeyboard = wantsKeyboard ? YES : NO;
  lastKeyboardType = keyboardType;
  lastReturnKeyType = returnKeyType;
  lastAutocapitalizationType = autocapitalizationType;
  lastAutocorrectionType = autocorrectionType;
  lastSpellCheckingType = spellCheckingType;
  [lastTextContentType release];
  lastTextContentType = [textContentType copy];
  lastSecureEntry = secureEntry;
  lastSuppressesSoftwareKeyboard = suppressesSoftwareKeyboard;
  lastUsesSearchResponder = wantsSearchResponder;
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
  if ([NSThread isMainThread])
  {
    [view syncKeyboardFocus];
    return;
  }

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

  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return !input->readonly && !input->disabled;

  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return !textarea->readonly && !textarea->disabled;

  const char* typeName = focused->typeName();
  return typeName && (std::strcmp(typeName, "text-input") == 0 || std::strcmp(typeName, "textarea") == 0);
}

bool glint_view_ios::_focusedSuppressesSoftwareKeyboard() const
{
  if (!mDocument)
    return false;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return glint_inputmode_is_none(input->inputmode);
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return glint_inputmode_is_none(textarea->inputmode);
  return false;
}

bool glint_view_ios::_focusedNodeHasText() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return !editor->getValue().empty();
  return false;
}

bool glint_view_ios::_focusedNeedsNativeTextServices() const
{
  if (!mDocument)
    return false;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
  {
    if (input->type == "number")
      return false;
    return !input->autocomplete.empty() || !input->autocapitalize.empty() || !input->spellcheck.empty();
  }
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return !textarea->autocomplete.empty() || !textarea->autocapitalize.empty() || !textarea->spellcheck.empty();

  return false;
}

std::string glint_view_ios::_focusedTextValue() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return editor->getValue();
  return {};
}

glint_rect glint_view_ios::_focusedPaintRect() const
{
  if (!mDocument)
    return {};

  const glint_element* focused = mDocument->getFocusedNode();
  return focused ? focused->GetPaintRECT() : glint_rect{};
}

int glint_view_ios::_focusedKeyboardType() const
{
  if (!mDocument)
    return UIKeyboardTypeDefault;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return glint_keyboard_type_for_input(*input);
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return glint_keyboard_type_for_textarea(*textarea);

  return UIKeyboardTypeDefault;
}

int glint_view_ios::_focusedReturnKeyType() const
{
  if (!mDocument)
    return UIReturnKeyDefault;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
  {
    if (!input->enterkeyhint.empty())
      return glint_return_key_type_from_hint(input->enterkeyhint);
  }
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
  {
    if (!textarea->enterkeyhint.empty())
      return glint_return_key_type_from_hint(textarea->enterkeyhint);
  }

  return UIReturnKeyDefault;
}

int glint_view_ios::_focusedAutocapitalizationType() const
{
  const int keyboardType = _focusedKeyboardType();
  const bool secureEntry = _focusedSecureEntry();

  if (!mDocument)
    return static_cast<int>(glint_autocapitalization_for_traits(keyboardType, secureEntry));

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return static_cast<int>(glint_autocapitalization_for_attribute(input->autocapitalize, keyboardType, secureEntry));
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return static_cast<int>(glint_autocapitalization_for_attribute(textarea->autocapitalize, keyboardType, secureEntry));

  return static_cast<int>(glint_autocapitalization_for_traits(keyboardType, secureEntry));
}

int glint_view_ios::_focusedAutocorrectionType() const
{
  const int keyboardType = _focusedKeyboardType();
  const bool secureEntry = _focusedSecureEntry();

  if (!mDocument)
    return static_cast<int>(glint_autocorrection_for_traits(keyboardType, secureEntry));

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return static_cast<int>(glint_autocorrection_for_spellcheck(input->spellcheck, keyboardType, secureEntry));
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return static_cast<int>(glint_autocorrection_for_spellcheck(textarea->spellcheck, keyboardType, secureEntry));

  return static_cast<int>(glint_autocorrection_for_traits(keyboardType, secureEntry));
}

int glint_view_ios::_focusedSpellCheckingType() const
{
  const int keyboardType = _focusedKeyboardType();
  const bool secureEntry = _focusedSecureEntry();

  if (!mDocument)
    return static_cast<int>(glint_spellchecking_for_traits(keyboardType, secureEntry));

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return static_cast<int>(glint_spellchecking_for_attribute(input->spellcheck, keyboardType, secureEntry));
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return static_cast<int>(glint_spellchecking_for_attribute(textarea->spellcheck, keyboardType, secureEntry));

  return static_cast<int>(glint_spellchecking_for_traits(keyboardType, secureEntry));
}

std::string glint_view_ios::_focusedAutocomplete() const
{
  if (!mDocument)
    return {};

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return input->autocomplete;
  if (const auto* textarea = dynamic_cast<const glint_textarea*>(focused))
    return textarea->autocomplete;

  return {};
}

bool glint_view_ios::_replaceFocusedTextFromPlatform(const std::string& utf8)
{
  if (auto* editor = glint_focused_text_editor(mDocument.get()))
  {
    editor->replaceTextFromPlatform(utf8);
    requestRedraw();
    return true;
  }
  return false;
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

bool glint_view_ios::_focusedCanCut() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return editor->canCutSelection();
  return false;
}

bool glint_view_ios::_focusedCanCopy() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return editor->canCopySelection();
  return false;
}

bool glint_view_ios::_focusedCanPaste() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return editor->canPasteFromClipboard();
  return false;
}

bool glint_view_ios::_focusedCanSelectAll() const
{
  if (const auto* editor = glint_focused_text_editor(mDocument.get()))
    return editor->canSelectAllText();
  return false;
}

bool glint_view_ios::_focusedCut()
{
  if (auto* editor = glint_focused_text_editor(mDocument.get()))
  {
    editor->cutSelection();
    requestRedraw();
    return true;
  }
  return false;
}

bool glint_view_ios::_focusedCopy()
{
  if (auto* editor = glint_focused_text_editor(mDocument.get()))
  {
    editor->copySelection();
    requestRedraw();
    return true;
  }
  return false;
}

bool glint_view_ios::_focusedPaste()
{
  if (auto* editor = glint_focused_text_editor(mDocument.get()))
  {
    editor->pasteFromClipboard();
    requestRedraw();
    return true;
  }
  return false;
}

bool glint_view_ios::_focusedSelectAll()
{
  if (auto* editor = glint_focused_text_editor(mDocument.get()))
  {
    editor->selectAllText();
    requestRedraw();
    return true;
  }
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

  if (mViewHandle)
    glint_set_last_interaction((__bridge UIView*) mViewHandle, CGPointMake(x, y));

  const glint_element* hit = mDocument->mCanvas.HitTest(x, y);
  mLastTouchTargetWantsKeyboard = glint_hit_targets_keyboard(hit);
  glint_element* keyboardTarget = glint_keyboard_target_from_hit(hit);

  if (const glint_element* focused = mDocument->getFocusedNode(); glint_node_wants_keyboard(focused))
  {
    if (!glint_hit_keeps_keyboard_focus(hit, focused))
      mDocument->SetFocus(nullptr);
  }

  if (keyboardTarget && mDocument->getFocusedNode() != keyboardTarget)
  {
    mDocument->SetFocus(keyboardTarget);
    _syncKeyboardFocus();
  }

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

  if (mViewHandle)
    glint_set_last_interaction((__bridge UIView*) mViewHandle, CGPointMake(x, y));

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

  if (mViewHandle)
    glint_set_last_interaction((__bridge UIView*) mViewHandle, CGPointMake(x, y));

  mDocument->OnMouseUp(x, y, glint_touch_mod(false));
  if (!mLastTouchTargetWantsKeyboard && _focusedNodeWantsKeyboard())
    mDocument->SetFocus(nullptr);
  _syncKeyboardFocus();
  requestRedraw();
}

void glint_view_ios::_handleTouchCancel()
{
  if (!mDocument)
    return;

  mDocument->OnMouseOut();
  if (!mLastTouchTargetWantsKeyboard && _focusedNodeWantsKeyboard())
    mDocument->SetFocus(nullptr);
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

struct colorpicker_handle
{
  GlintIOSColorPickerCoordinator* coordinator = nil;
};

colorpicker_handle* showColorPicker(const glint_color& initialColor,
                                    const RECT& anchorScreenRect,
                                    std::function<void(glint_color)> onChange,
                                    std::function<void()> onClosed)
{
  return reopenColorPicker(nullptr, initialColor, anchorScreenRect, std::move(onChange), std::move(onClosed));
}

colorpicker_handle* reopenColorPicker(colorpicker_handle* handle,
                                      const glint_color& initialColor,
                                      const RECT& anchorScreenRect,
                                      std::function<void(glint_color)> onChange,
                                      std::function<void()> onClosed)
{
  __block colorpicker_handle* result = handle;
  __block std::function<void(glint_color)> changeCb = std::move(onChange);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new colorpicker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSColorPickerCoordinator alloc] init];
    result->coordinator->onChange = std::move(changeCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithColor:glint_uicolor_from_glint_color(initialColor)
                         anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideColorPicker(colorpicker_handle* handle)
{
  if (!handle || !handle->coordinator)
    return;

  void (^run)(void) = ^{
    [handle->coordinator hideAnimated:YES notifyClosed:YES];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);
}

void destroyColorPicker(colorpicker_handle* handle)
{
  if (!handle)
    return;

  void (^run)(void) = ^{
    if (handle->coordinator)
    {
      [handle->coordinator destroy];
      [handle->coordinator release];
      handle->coordinator = nil;
    }
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  delete handle;
}

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
                    const std::vector<std::pair<int, std::string>>& items,
                    const std::vector<int>& disabledIds,
                    const std::vector<int>& checkedIds)
{
  __block int result = 0;

  void (^run)(void) = ^{
    UIWindow* window = glint_active_window();
    UIViewController* presenter = glint_top_view_controller(window.rootViewController);
    UIView* sourceView = presenter.view ?: window;
    if (!presenter || !sourceView)
      return;

    NSMutableArray<GlintIOSMenuItem*>* nativeItems = [NSMutableArray arrayWithCapacity:items.size()];
    for (const auto& item : items)
    {
      const bool separator = item.first == 0 && item.second == "-";
      const bool disabled = !separator && std::find(disabledIds.begin(), disabledIds.end(), item.first) != disabledIds.end();
      const bool checked = !separator && std::find(checkedIds.begin(), checkedIds.end(), item.first) != checkedIds.end();
      [nativeItems addObject:[GlintIOSMenuItem itemWithId:item.first
                                                    title:(separator ? @"" : glint_nsstring_from_utf8(item.second))
                                                  enabled:separator ? NO : !disabled
                                                  checked:checked
                                                separator:separator]];
    }

    GlintIOSMenuTracker* tracker = [[GlintIOSMenuTracker alloc] init];
    tracker->selectedId = 0;
    tracker->finished = NO;

    GlintIOSMenuListController* listController = [[GlintIOSMenuListController alloc] initWithItems:nativeItems tracker:tracker];
    UINavigationController* navigationController = [[UINavigationController alloc] initWithRootViewController:listController];
    navigationController.preferredContentSize = listController.preferredContentSize;

    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad)
    {
      navigationController.modalPresentationStyle = UIModalPresentationPopover;
      UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
      popover.sourceView = sourceView;
      popover.sourceRect = glint_centered_source_rect(sourceView);
      popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
    }
    else
    {
      navigationController.modalPresentationStyle = UIModalPresentationPageSheet;
      if (@available(iOS 15.0, *))
      {
        UISheetPresentationController* sheet = navigationController.sheetPresentationController;
        if (sheet)
        {
          sheet.detents = @[UISheetPresentationControllerDetent.mediumDetent,
                            UISheetPresentationControllerDetent.largeDetent];
          sheet.prefersGrabberVisible = YES;
        }
      }
    }

    [presenter presentViewController:navigationController animated:YES completion:nil];

    while (!tracker->finished)
    {
      @autoreleasepool
      {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
        [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
      }
    }

    result = tracker->selectedId;
    [navigationController release];
    [listController release];
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

int showSelectMenu(int,
                   int,
                   const std::vector<std::pair<int, std::string>>& items,
                   int selectedId,
                   const std::vector<int>& disabledIds)
{
  __block int result = 0;

  void (^run)(void) = ^{
    UIWindow* window = glint_active_window();
    if (!window)
      return;

    UIView* anchorView = glint_last_interaction_view ? glint_last_interaction_view : window;
    CGPoint anchorPoint = glint_last_interaction_view
      ? glint_last_interaction_point
      : CGPointMake(CGRectGetMidX(anchorView.bounds), CGRectGetMidY(anchorView.bounds));

    NSMutableArray<GlintIOSMenuItem*>* nativeItems = [NSMutableArray arrayWithCapacity:items.size()];
    for (const auto& item : items)
    {
      if (item.first == 0 && item.second == "-")
        continue;

      const bool disabled = std::find(disabledIds.begin(), disabledIds.end(), item.first) != disabledIds.end();
      [nativeItems addObject:[GlintIOSMenuItem itemWithId:item.first
                                                    title:glint_nsstring_from_utf8(item.second)
                                                  enabled:!disabled
                                                  checked:item.first == selectedId
                                                separator:NO]];
    }
    if (!nativeItems.count)
      return;

    GlintIOSMenuTracker* tracker = [[GlintIOSMenuTracker alloc] init];
    tracker->selectedId = 0;
    tracker->finished = NO;

    if (@available(iOS 17.4, *))
    {
      GlintIOSSelectPickerCoordinator* coordinator = [[GlintIOSSelectPickerCoordinator alloc]
        initWithItems:nativeItems
           selectedId:selectedId
              tracker:tracker];
      [coordinator presentInView:anchorView sourcePoint:anchorPoint];

      while (!tracker->finished && [coordinator isAwaitingDismissal])
      {
        @autoreleasepool
        {
          NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
          [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
          [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
        }
      }

      [coordinator release];
    }
    else
    {
      GlintIOSMenuListController* listController = [[GlintIOSMenuListController alloc] initWithItems:nativeItems tracker:tracker];
      UINavigationController* navigationController = [[UINavigationController alloc] initWithRootViewController:listController];
      navigationController.preferredContentSize = listController.preferredContentSize;

      UIViewController* presenter = glint_top_view_controller(window.rootViewController);
      if (!presenter)
      {
        presenter = window.rootViewController;
        if (!presenter)
        {
          [navigationController release];
          [listController release];
          [tracker release];
          return;
        }
      }

      navigationController.modalPresentationStyle = UIModalPresentationPopover;
      UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
      popover.delegate = listController;
      popover.sourceView = anchorView;
      popover.sourceRect = CGRectMake(anchorPoint.x, anchorPoint.y, 1.0f, 1.0f);
      popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

      [presenter presentViewController:navigationController animated:YES completion:nil];

      while (!tracker->finished)
      {
        @autoreleasepool
        {
          NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
          [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
          [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
        }
      }

      [navigationController release];
      [listController release];
    }

    result = tracker->selectedId;
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
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