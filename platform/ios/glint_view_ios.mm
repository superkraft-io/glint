/**
 * glint_view_ios.mm
 * iOS embedded view host — ObjC++ implementation.
 */

#import <UIKit/UIKit.h>
#import <PhotosUI/PhotosUI.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "glint_view_ios.hpp"
#include "../glint_platform.hpp"
#include "../glint_platform_colorpicker.hpp"
#include "../glint_platform_datepicker.hpp"
#include "../glint_platform_datetime_local_picker.hpp"
#include "../glint_platform_monthpicker.hpp"
#include "../glint_platform_timepicker.hpp"
#include "../glint_platform_weekpicker.hpp"
#include "../../i18n/glint_i18n.hpp"
#include "../../components/input/glint_input.hpp"
#include "../../components/input/week/glint_iso_week.hpp"
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

enum glint_temporal_input_kind
{
  glint_temporal_input_kind_none = 0,
  glint_temporal_input_kind_date,
  glint_temporal_input_kind_time,
};

glint_temporal_input_kind glint_temporal_kind_for_input(const glint_text_input& input)
{
  if (input.type == "date") return glint_temporal_input_kind_date;
  if (input.type == "time") return glint_temporal_input_kind_time;
  return glint_temporal_input_kind_none;
}

int glint_temporal_minute_interval_for_step(float step)
{
  if (!(step > 0.f))
    return 1;

  const double seconds = static_cast<double>(step);
  const double roundedSeconds = std::round(seconds);
  if (std::abs(seconds - roundedSeconds) > 1e-6)
    return 1;
  if (static_cast<int>(roundedSeconds) % 60 != 0)
    return 1;

  const int minutes = static_cast<int>(roundedSeconds) / 60;
  if (minutes < 1 || minutes > 30)
    return 1;
  if (60 % minutes != 0)
    return 1;
  return minutes;
}

NSDate* glint_temporal_date_from_value(glint_temporal_input_kind kind, std::string_view value)
{
  if (value.empty())
    return nil;

  NSCalendar* calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendar.timeZone = NSTimeZone.localTimeZone;

  const std::string utf8(value);
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = calendar;
  components.timeZone = calendar.timeZone;

  if (kind == glint_temporal_input_kind_date)
  {
    int year = 0;
    int month = 0;
    int day = 0;
    if (std::sscanf(utf8.c_str(), "%d-%d-%d", &year, &month, &day) != 3)
      return nil;
    components.year = year;
    components.month = month;
    components.day = day;
    components.hour = 12;
    return [calendar dateFromComponents:components];
  }

  if (kind == glint_temporal_input_kind_time)
  {
    NSDateComponents* today = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay
                                          fromDate:NSDate.date];
    int hour = 0;
    int minute = 0;
    if (std::sscanf(utf8.c_str(), "%d:%d", &hour, &minute) < 2)
      return nil;
    components.year = today.year;
    components.month = today.month;
    components.day = today.day;
    components.hour = hour;
    components.minute = minute;
    return [calendar dateFromComponents:components];
  }

  return nil;
}

std::string glint_temporal_value_from_date(glint_temporal_input_kind kind, NSDate* date)
{
  if (!date)
    return {};

  NSCalendar* calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendar.timeZone = NSTimeZone.localTimeZone;
  NSDateComponents* components = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay
                                             | NSCalendarUnitHour | NSCalendarUnitMinute
                                             fromDate:date];

  char buffer[32] = {};
  switch (kind)
  {
    case glint_temporal_input_kind_date:
      std::snprintf(buffer, sizeof(buffer), "%04ld-%02ld-%02ld",
                    static_cast<long>(components.year),
                    static_cast<long>(components.month),
                    static_cast<long>(components.day));
      return buffer;
    case glint_temporal_input_kind_time:
      std::snprintf(buffer, sizeof(buffer), "%02ld:%02ld",
                    static_cast<long>(components.hour),
                    static_cast<long>(components.minute));
      return buffer;
    case glint_temporal_input_kind_none:
      break;
  }

  return {};
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

CGRect glint_source_rect_for_point(UIView* sourceView, CGPoint point)
{
  if (!sourceView)
    return CGRectMake(point.x, point.y, 1.0, 1.0);

  const CGFloat maxX = std::max<CGFloat>(0.0, CGRectGetWidth(sourceView.bounds) - 1.0);
  const CGFloat maxY = std::max<CGFloat>(0.0, CGRectGetHeight(sourceView.bounds) - 1.0);
  const CGFloat clampedX = std::clamp(point.x, static_cast<CGFloat>(0.0), maxX);
  const CGFloat clampedY = std::clamp(point.y, static_cast<CGFloat>(0.0), maxY);
  return CGRectMake(clampedX, clampedY, 1.0, 1.0);
}

CGFloat glint_display_scale_for_view(UIView* view)
{
  if (!view)
    return 1.0f;

  if (view.window.windowScene.screen)
    return std::max<CGFloat>(1.0f, view.window.windowScene.screen.scale);

  UITraitCollection* traits = view.traitCollection;
  if (traits.displayScale > 0.0)
    return traits.displayScale;

  return 1.0f;
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

int glint_days_in_calendar_month(NSCalendar* calendar, int year, int month)
{
  NSCalendar* resolvedCalendar = calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = resolvedCalendar;
  components.timeZone = NSTimeZone.localTimeZone;
  components.year = std::max(1, year);
  components.month = std::max(1, std::min(12, month));
  components.day = 1;

  NSDate* date = [resolvedCalendar dateFromComponents:components];
  if (!date)
    return 31;

  NSRange dayRange = [resolvedCalendar rangeOfUnit:NSCalendarUnitDay inUnit:NSCalendarUnitMonth forDate:date];
  return dayRange.length > 0 ? static_cast<int>(dayRange.length) : 31;
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
@class GlintIOSDateTimeLocalPickerCoordinator;
@class GlintIOSDatePickerCoordinator;
@class GlintIOSMonthPickerCoordinator;
@class GlintIOSTimePickerCoordinator;
@class GlintIOSWeekPickerCoordinator;

@interface GlintIOSDateTimeLocalTimePopoverController : UIViewController <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  GlintIOSDateTimeLocalPickerCoordinator* coordinator;
  UIDatePicker* picker;
}
- (void)syncSelectionAnimated:(BOOL)animated;
@end

@interface GlintIOSDateTimeLocalPickerViewController : UIViewController <UICalendarViewDelegate, UICalendarSelectionSingleDateDelegate>
{
@public
  GlintIOSDateTimeLocalPickerCoordinator* coordinator;
  UICalendarView* calendarView;
  UICalendarSelectionSingleDate* selection;
  UIButton* timeButton;
}
- (void)handleResetButtonPressed:(id)sender;
- (void)handleConfirmButtonPressed:(id)sender;
- (void)handleTimeButtonPressed:(id)sender;
- (void)updateTimeButtonTitle;
@end

@interface GlintIOSDatePickerViewController : UIViewController <UICalendarViewDelegate, UICalendarSelectionSingleDateDelegate>
{
@public
  GlintIOSDatePickerCoordinator* coordinator;
  UICalendarView* calendarView;
  UICalendarSelectionSingleDate* selection;
}
- (void)handleResetButtonPressed:(id)sender;
- (void)handleConfirmButtonPressed:(id)sender;
@end

@interface GlintIOSMonthPickerViewController : UIViewController <UIPickerViewDataSource, UIPickerViewDelegate>
{
@public
  GlintIOSMonthPickerCoordinator* coordinator;
  UIPickerView* picker;
  NSArray<NSString*>* monthSymbols;
}
- (void)handleResetButtonPressed:(id)sender;
- (void)handleConfirmButtonPressed:(id)sender;
- (void)syncSelectionAnimated:(BOOL)animated;
@end

@interface GlintIOSTimePickerViewController : UIViewController
{
@public
  GlintIOSTimePickerCoordinator* coordinator;
  UIDatePicker* picker;
}
- (void)handleResetButtonPressed:(id)sender;
- (void)handleConfirmButtonPressed:(id)sender;
@end

@interface GlintIOSWeekPickerViewController : UIViewController <UICalendarViewDelegate, UICalendarSelectionWeekOfYearDelegate>
{
@public
  GlintIOSWeekPickerCoordinator* coordinator;
  UICalendarView* calendarView;
  UICalendarSelectionWeekOfYear* selection;
}
- (void)handleResetButtonPressed:(id)sender;
- (void)handleConfirmButtonPressed:(id)sender;
@end

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
  UIDatePicker* keyboardTemporalPicker;
  BOOL keyboardPrewarmScheduled;
  BOOL keyboardPrewarmActive;
  BOOL keyboardPrewarmDone;
  int lastTemporalInputKind;
}
- (instancetype)initWithView:(glint_view_ios*)view frame:(CGRect)frame;
- (void)displayLinkFired:(CADisplayLink*)displayLink;
- (void)handleEditMenuLongPress:(UILongPressGestureRecognizer*)recognizer;
- (void)handleKeyboardFieldEditingChanged:(UITextField*)sender;
- (void)handleKeyboardFieldTextDidChangeNotification:(NSNotification*)notification;
- (void)prewarmKeyboardHostIfNeeded;
- (void)syncKeyboardFocus;
- (UIView*)activeKeyboardInputView;
- (void)syncTemporalPickerState;
- (void)handleTemporalPickerValueChanged:(UIDatePicker*)sender;
@end

@interface GlintKeyboardProxyField : UITextField
{
@public
  glint_view_ios* cppView;
  GlintIOSView* ownerView;
}
@end

@interface GlintKeyboardSearchField : UISearchTextField
@end

@interface GlintIOSMenuTracker : NSObject <UIAdaptivePresentationControllerDelegate>
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
  NSString* systemImageName;
  BOOL enabled;
  BOOL checked;
  BOOL separator;
}
+ (instancetype)itemWithId:(int)itemId
                     title:(NSString*)title
                   enabled:(BOOL)enabled
                   checked:(BOOL)checked
                 separator:(BOOL)separator;
+ (instancetype)itemWithId:(int)itemId
                     title:(NSString*)title
           systemImageName:(NSString*)systemImageName
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

@interface GlintIOSDatePickerCoordinator : NSObject <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  UINavigationController* navigationController;
  GlintIOSDatePickerViewController* contentController;
  int initialYear;
  int initialMonth;
  int initialDay;
  int selectedYear;
  int selectedMonth;
  int selectedDay;
  std::function<void(int, int, int)> onChange;
  std::function<void(int, int, int)> onConfirm;
  std::function<void()> onReset;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithYear:(int)year month:(int)month day:(int)day anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
- (void)selectionDidChangeYear:(int)year month:(int)month day:(int)day;
- (NSDateComponents*)handleVisibleMonthChangeYear:(int)year month:(int)month;
- (void)reset:(id)sender;
- (void)confirm:(id)sender;
@end

@interface GlintIOSDateTimeLocalPickerCoordinator : NSObject <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  UINavigationController* navigationController;
  GlintIOSDateTimeLocalPickerViewController* contentController;
  GlintIOSDateTimeLocalTimePopoverController* timePopoverController;
  int initialYear;
  int initialMonth;
  int initialDay;
  int initialHour;
  int initialMinute;
  int selectedYear;
  int selectedMonth;
  int selectedDay;
  int selectedHour;
  int selectedMinute;
  std::function<void(int, int, int, int, int)> onChange;
  std::function<void(int, int, int, int, int)> onConfirm;
  std::function<void()> onReset;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithYear:(int)year month:(int)month day:(int)day hour:(int)hour minute:(int)minute anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
- (NSDateComponents*)dateComponentsForYear:(int)year month:(int)month day:(int)day;
- (NSDate*)timePickerDateForHour:(int)hour minute:(int)minute;
- (void)selectionDidChangeYear:(int)year month:(int)month day:(int)day;
- (NSDateComponents*)handleVisibleMonthChangeYear:(int)year month:(int)month;
- (void)selectionDidChangeHour:(int)hour minute:(int)minute;
- (void)presentTimePopoverFromSourceView:(UIView*)sourceView;
- (void)dismissTimePopoverAnimated:(BOOL)animated;
- (void)reset:(id)sender;
- (void)confirm:(id)sender;
@end

@interface GlintIOSTimePickerCoordinator : NSObject <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  UINavigationController* navigationController;
  GlintIOSTimePickerViewController* contentController;
  int initialHour;
  int initialMinute;
  std::function<void(int, int)> onConfirm;
  std::function<void()> onReset;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithHour:(int)hour minute:(int)minute anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
- (void)cancel:(id)sender;
- (void)reset:(id)sender;
- (void)confirm:(id)sender;
@end

@interface GlintIOSMonthPickerCoordinator : NSObject <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  UINavigationController* navigationController;
  GlintIOSMonthPickerViewController* contentController;
  int initialYear;
  int initialMonth;
  int selectedYear;
  int selectedMonth;
  std::function<void(int, int)> onChange;
  std::function<void(int, int)> onConfirm;
  std::function<void()> onReset;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithYear:(int)year month:(int)month anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
- (void)selectionDidChangeYear:(int)year month:(int)month;
- (void)reset:(id)sender;
- (void)confirm:(id)sender;
@end

@interface GlintIOSWeekPickerCoordinator : NSObject <UIPopoverPresentationControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  UIViewController* presenter;
  UIView* sourceView;
  CGRect sourceRect;
  UINavigationController* navigationController;
  GlintIOSWeekPickerViewController* contentController;
  int initialWeekYear;
  int initialWeek;
  int selectedWeekYear;
  int selectedWeek;
  std::function<void(int, int)> onChange;
  std::function<void(int, int)> onConfirm;
  std::function<void()> onReset;
  std::function<void()> onClosed;
  BOOL closedNotified;
}
- (void)presentWithWeekYear:(int)weekYear week:(int)week anchorScreenRect:(RECT)anchorScreenRect;
- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed;
- (void)destroy;
- (void)cancel:(id)sender;
- (void)reset:(id)sender;
- (void)confirm:(id)sender;
@end

@interface GlintIOSFilePickerTracker : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  NSMutableArray<NSString*>* selectedPaths;
  BOOL finished;
}
- (void)finishWithURLs:(NSArray<NSURL*>*)urls;
@end

@interface GlintIOSMediaPickerTracker : NSObject <UIImagePickerControllerDelegate, UINavigationControllerDelegate, PHPickerViewControllerDelegate, UIAdaptivePresentationControllerDelegate>
{
@public
  NSMutableArray<NSString*>* selectedPaths;
  BOOL finished;
}
- (NSString*)temporaryPathWithExtension:(NSString*)extension;
- (NSString*)copyURLToTemporaryLocation:(NSURL*)url suggestedExtension:(NSString*)extension;
- (NSString*)writeImageToTemporaryLocation:(UIImage*)image sourceURL:(NSURL*)sourceURL;
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
  if (ownerView)
    return [ownerView activeKeyboardInputView];

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

@implementation GlintIOSDateTimeLocalTimePopoverController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  picker = nil;
  self.preferredContentSize = CGSizeMake(220.0f, 216.0f);
  self.modalPresentationStyle = UIModalPresentationPopover;
  return self;
}

- (void)dealloc
{
  [picker release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 220.0f, 216.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  picker = [[UIDatePicker alloc] initWithFrame:CGRectZero];
  picker.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.4, *))
    picker.preferredDatePickerStyle = UIDatePickerStyleWheels;
  picker.datePickerMode = UIDatePickerModeTime;
  picker.locale = NSLocale.currentLocale;
  picker.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  picker.timeZone = NSTimeZone.localTimeZone;
  [picker addTarget:self action:@selector(handlePickerValueChanged:) forControlEvents:UIControlEventValueChanged];

  [root addSubview:picker];
  [NSLayoutConstraint activateConstraints:@[
    [picker.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
    [picker.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    [picker.topAnchor constraintEqualToAnchor:root.topAnchor],
    [picker.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
  ]];

  self.view = root;
  [root release];
}

- (void)handlePickerValueChanged:(UIDatePicker*)sender
{
  NSDateComponents* components = [sender.calendar components:NSCalendarUnitHour | NSCalendarUnitMinute fromDate:sender.date];
  [coordinator selectionDidChangeHour:(int)components.hour minute:(int)components.minute];
}

- (void)syncSelectionAnimated:(BOOL)animated
{
  if (!coordinator)
    return;
  [picker setDate:[coordinator timePickerDateForHour:coordinator->selectedHour minute:coordinator->selectedMinute] animated:animated];
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSDateTimeLocalPickerViewController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  calendarView = nil;
  selection = nil;
  timeButton = nil;
  self.preferredContentSize = CGSizeMake(340.0f, 476.0f);
  return self;
}

- (void)dealloc
{
  [selection release];
  [calendarView release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 340.0f, 476.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  UIView* timeRow = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  timeRow.translatesAutoresizingMaskIntoConstraints = NO;

  UILabel* timeLabel = [[[UILabel alloc] initWithFrame:CGRectZero] autorelease];
  timeLabel.translatesAutoresizingMaskIntoConstraints = NO;
  timeLabel.text = @"Time";
  timeLabel.textColor = UIColor.labelColor;
  timeLabel.font = [UIFont systemFontOfSize:15.0f weight:UIFontWeightMedium];

  timeButton = [UIButton buttonWithType:UIButtonTypeSystem];
  timeButton.translatesAutoresizingMaskIntoConstraints = NO;
  [timeButton setTitleColor:UIColor.labelColor forState:UIControlStateNormal];
  timeButton.backgroundColor = UIColor.secondarySystemBackgroundColor;
  timeButton.layer.cornerRadius = 18.0f;
  timeButton.layer.masksToBounds = YES;
  timeButton.contentEdgeInsets = UIEdgeInsetsMake(8.0f, 14.0f, 8.0f, 14.0f);
  timeButton.titleLabel.font = [UIFont monospacedDigitSystemFontOfSize:15.0f weight:UIFontWeightMedium];
  [timeButton addTarget:self action:@selector(handleTimeButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  calendarView = [[UICalendarView alloc] initWithFrame:CGRectZero];
  calendarView.translatesAutoresizingMaskIntoConstraints = NO;
  calendarView.locale = NSLocale.currentLocale;
  calendarView.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendarView.timeZone = NSTimeZone.localTimeZone;
  calendarView.tintColor = UIColor.systemBlueColor;
  calendarView.delegate = self;

  selection = [[UICalendarSelectionSingleDate alloc] initWithDelegate:self];
  calendarView.selectionBehavior = selection;

  UIButton* resetButton = [UIButton buttonWithType:UIButtonTypeSystem];
  resetButton.translatesAutoresizingMaskIntoConstraints = NO;
  [resetButton setTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_reset)) forState:UIControlStateNormal];
  [resetButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
  resetButton.backgroundColor = [UIColor colorWithWhite:0.93f alpha:1.0f];
  resetButton.layer.cornerRadius = 18.0f;
  resetButton.layer.masksToBounds = YES;
  [resetButton addTarget:self action:@selector(handleResetButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIButton* confirmButton = [UIButton buttonWithType:UIButtonTypeSystem];
  confirmButton.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.0, *))
  {
    [confirmButton setImage:[UIImage systemImageNamed:@"checkmark"] forState:UIControlStateNormal];
  }
  else
  {
    [confirmButton setTitle:@"✓" forState:UIControlStateNormal];
  }
  [confirmButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  confirmButton.tintColor = UIColor.whiteColor;
  confirmButton.backgroundColor = UIColor.systemBlueColor;
  confirmButton.layer.cornerRadius = 22.0f;
  confirmButton.layer.masksToBounds = YES;
  [confirmButton addTarget:self action:@selector(handleConfirmButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  [root addSubview:calendarView];
  [root addSubview:timeRow];
  [timeRow addSubview:timeLabel];
  [timeRow addSubview:timeButton];
  [root addSubview:footer];
  [footer addSubview:resetButton];
  [footer addSubview:confirmButton];

  UILayoutGuide* safeArea = root.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [calendarView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8.0f],
    [calendarView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8.0f],
    [calendarView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0f],
    [calendarView.bottomAnchor constraintEqualToAnchor:timeRow.topAnchor constant:-12.0f],

    [timeRow.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [timeRow.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [timeRow.heightAnchor constraintEqualToConstant:40.0f],
    [timeRow.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-12.0f],

    [timeLabel.leadingAnchor constraintEqualToAnchor:timeRow.leadingAnchor],
    [timeLabel.centerYAnchor constraintEqualToAnchor:timeRow.centerYAnchor],

    [timeButton.trailingAnchor constraintEqualToAnchor:timeRow.trailingAnchor],
    [timeButton.centerYAnchor constraintEqualToAnchor:timeRow.centerYAnchor],
    [timeButton.heightAnchor constraintEqualToConstant:36.0f],
    [timeButton.widthAnchor constraintGreaterThanOrEqualToConstant:108.0f],

    [footer.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [footer.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [footer.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-12.0f],
    [footer.heightAnchor constraintEqualToConstant:44.0f],

    [resetButton.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
    [resetButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [resetButton.heightAnchor constraintEqualToConstant:36.0f],
    [resetButton.widthAnchor constraintGreaterThanOrEqualToConstant:96.0f],

    [confirmButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
    [confirmButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [confirmButton.widthAnchor constraintEqualToConstant:44.0f],
    [confirmButton.heightAnchor constraintEqualToConstant:44.0f],
  ]];

  self.view = root;
  [root release];
}

- (void)handleResetButtonPressed:(id)sender
{
  [coordinator reset:sender];
}

- (void)handleConfirmButtonPressed:(id)sender
{
  [coordinator confirm:sender];
}

- (void)handleTimeButtonPressed:(id)sender
{
  [coordinator presentTimePopoverFromSourceView:(UIView*)sender];
}

- (void)updateTimeButtonTitle
{
  if (!coordinator)
    return;

  NSString* title = [NSString stringWithFormat:@"%02d:%02d", coordinator->selectedHour, coordinator->selectedMinute];
  if ([[timeButton titleForState:UIControlStateNormal] isEqualToString:title])
    return;

  [UIView performWithoutAnimation:^{
    [timeButton setTitle:title forState:UIControlStateNormal];
    [timeButton layoutIfNeeded];
  }];
}

- (void)dateSelection:(UICalendarSelectionSingleDate*)dateSelection didSelectDate:(NSDateComponents*)dateComponents
{
  (void)dateSelection;
  if (!coordinator || !dateComponents)
    return;

  [coordinator selectionDidChangeYear:(int)dateComponents.year
                                month:(int)dateComponents.month
                                  day:(int)dateComponents.day];
}

- (BOOL)dateSelection:(UICalendarSelectionSingleDate*)dateSelection canSelectDate:(NSDateComponents*)dateComponents
{
  (void)dateSelection;
  return dateComponents != nil;
}

- (void)calendarView:(UICalendarView*)calendarView didChangeVisibleDateComponentsFrom:(NSDateComponents*)previousDateComponents
{
  (void)previousDateComponents;
  if (!coordinator)
    return;

  NSDateComponents* visibleDate = calendarView.visibleDateComponents;
  if (!visibleDate)
    return;

  NSDateComponents* selectedDate = [coordinator handleVisibleMonthChangeYear:(int)visibleDate.year
                                                                       month:(int)visibleDate.month];
  if (!selectedDate)
    return;

  [selection setSelectedDate:selectedDate animated:NO];
}

@end

@implementation GlintIOSDatePickerViewController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  calendarView = nil;
  selection = nil;
  self.preferredContentSize = CGSizeMake(340.0f, 420.0f);
  return self;
}

- (void)dealloc
{
  [selection release];
  [calendarView release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 340.0f, 420.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  calendarView = [[UICalendarView alloc] initWithFrame:CGRectZero];
  calendarView.translatesAutoresizingMaskIntoConstraints = NO;
  calendarView.locale = NSLocale.currentLocale;
  calendarView.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendarView.timeZone = NSTimeZone.localTimeZone;
  calendarView.tintColor = UIColor.systemBlueColor;
  calendarView.delegate = self;

  selection = [[UICalendarSelectionSingleDate alloc] initWithDelegate:self];
  calendarView.selectionBehavior = selection;

  UIButton* resetButton = [UIButton buttonWithType:UIButtonTypeSystem];
  resetButton.translatesAutoresizingMaskIntoConstraints = NO;
  [resetButton setTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_reset)) forState:UIControlStateNormal];
  [resetButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
  resetButton.backgroundColor = [UIColor colorWithWhite:0.93f alpha:1.0f];
  resetButton.layer.cornerRadius = 18.0f;
  resetButton.layer.masksToBounds = YES;
  [resetButton addTarget:self action:@selector(handleResetButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIButton* confirmButton = [UIButton buttonWithType:UIButtonTypeSystem];
  confirmButton.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.0, *))
  {
    [confirmButton setImage:[UIImage systemImageNamed:@"checkmark"] forState:UIControlStateNormal];
  }
  else
  {
    [confirmButton setTitle:@"✓" forState:UIControlStateNormal];
  }
  [confirmButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  confirmButton.tintColor = UIColor.whiteColor;
  confirmButton.backgroundColor = UIColor.systemBlueColor;
  confirmButton.layer.cornerRadius = 22.0f;
  confirmButton.layer.masksToBounds = YES;
  [confirmButton addTarget:self action:@selector(handleConfirmButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  [root addSubview:calendarView];
  [root addSubview:footer];
  [footer addSubview:resetButton];
  [footer addSubview:confirmButton];

  UILayoutGuide* safeArea = root.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [calendarView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8.0f],
    [calendarView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8.0f],
    [calendarView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0f],
    [calendarView.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-16.0f],

    [footer.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [footer.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [footer.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-12.0f],
    [footer.heightAnchor constraintEqualToConstant:44.0f],

    [resetButton.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
    [resetButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [resetButton.heightAnchor constraintEqualToConstant:36.0f],
    [resetButton.widthAnchor constraintGreaterThanOrEqualToConstant:96.0f],

    [confirmButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
    [confirmButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [confirmButton.widthAnchor constraintEqualToConstant:44.0f],
    [confirmButton.heightAnchor constraintEqualToConstant:44.0f],
  ]];

  self.view = root;
  [root release];
}

- (void)handleResetButtonPressed:(id)sender
{
  [coordinator reset:sender];
}

- (void)handleConfirmButtonPressed:(id)sender
{
  [coordinator confirm:sender];
}

- (void)dateSelection:(UICalendarSelectionSingleDate*)dateSelection didSelectDate:(NSDateComponents*)dateComponents
{
  (void)dateSelection;
  if (!coordinator || !dateComponents)
    return;

  [coordinator selectionDidChangeYear:(int)dateComponents.year
                                month:(int)dateComponents.month
                                  day:(int)dateComponents.day];
}

- (BOOL)dateSelection:(UICalendarSelectionSingleDate*)dateSelection canSelectDate:(NSDateComponents*)dateComponents
{
  (void)dateSelection;
  return dateComponents != nil;
}

- (void)calendarView:(UICalendarView*)calendarView didChangeVisibleDateComponentsFrom:(NSDateComponents*)previousDateComponents
{
  (void)previousDateComponents;
  if (!coordinator)
    return;

  NSDateComponents* visibleDate = calendarView.visibleDateComponents;
  if (!visibleDate)
    return;

  NSDateComponents* selectedDate = [coordinator handleVisibleMonthChangeYear:(int)visibleDate.year
                                                                       month:(int)visibleDate.month];
  if (!selectedDate)
    return;

  [selection setSelectedDate:selectedDate animated:NO];
}

@end

@implementation GlintIOSMonthPickerViewController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  picker = nil;
  monthSymbols = nil;
  self.preferredContentSize = CGSizeMake(320.0f, 316.0f);
  return self;
}

- (void)dealloc
{
  [monthSymbols release];
  [picker release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 320.0f, 316.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  NSDateFormatter* formatter = [[[NSDateFormatter alloc] init] autorelease];
  formatter.locale = NSLocale.currentLocale;
  NSArray<NSString*>* localizedMonths = formatter.standaloneMonthSymbols;
  if (!localizedMonths || localizedMonths.count < 12)
    localizedMonths = formatter.monthSymbols;
  monthSymbols = [localizedMonths copy];

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  picker = [[UIPickerView alloc] initWithFrame:CGRectZero];
  picker.translatesAutoresizingMaskIntoConstraints = NO;
  picker.dataSource = self;
  picker.delegate = self;

  UIButton* resetButton = [UIButton buttonWithType:UIButtonTypeSystem];
  resetButton.translatesAutoresizingMaskIntoConstraints = NO;
  [resetButton setTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_reset)) forState:UIControlStateNormal];
  [resetButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
  resetButton.backgroundColor = [UIColor colorWithWhite:0.93f alpha:1.0f];
  resetButton.layer.cornerRadius = 18.0f;
  resetButton.layer.masksToBounds = YES;
  [resetButton addTarget:self action:@selector(handleResetButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIButton* confirmButton = [UIButton buttonWithType:UIButtonTypeSystem];
  confirmButton.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.0, *))
  {
    [confirmButton setImage:[UIImage systemImageNamed:@"checkmark"] forState:UIControlStateNormal];
  }
  else
  {
    [confirmButton setTitle:@"✓" forState:UIControlStateNormal];
  }
  [confirmButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  confirmButton.tintColor = UIColor.whiteColor;
  confirmButton.backgroundColor = UIColor.systemBlueColor;
  confirmButton.layer.cornerRadius = 22.0f;
  confirmButton.layer.masksToBounds = YES;
  [confirmButton addTarget:self action:@selector(handleConfirmButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  [root addSubview:picker];
  [root addSubview:footer];
  [footer addSubview:resetButton];
  [footer addSubview:confirmButton];

  UILayoutGuide* safeArea = root.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [picker.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
    [picker.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    [picker.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0f],
    [picker.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-16.0f],

    [footer.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [footer.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [footer.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-12.0f],
    [footer.heightAnchor constraintEqualToConstant:44.0f],

    [resetButton.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
    [resetButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [resetButton.heightAnchor constraintEqualToConstant:36.0f],
    [resetButton.widthAnchor constraintGreaterThanOrEqualToConstant:96.0f],

    [confirmButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
    [confirmButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [confirmButton.widthAnchor constraintEqualToConstant:44.0f],
    [confirmButton.heightAnchor constraintEqualToConstant:44.0f],
  ]];

  self.view = root;
  [root release];
}

- (void)handleResetButtonPressed:(id)sender
{
  [coordinator reset:sender];
}

- (void)handleConfirmButtonPressed:(id)sender
{
  [coordinator confirm:sender];
}

- (void)syncSelectionAnimated:(BOOL)animated
{
  const NSInteger monthRow = MAX(0, MIN(11, coordinator ? coordinator->selectedMonth - 1 : 0));
  const NSInteger yearRow = MAX(0, MIN(9998, coordinator ? coordinator->selectedYear - 1 : 0));
  [picker selectRow:monthRow inComponent:0 animated:animated];
  [picker selectRow:yearRow inComponent:1 animated:animated];
}

- (NSInteger)numberOfComponentsInPickerView:(UIPickerView*)pickerView
{
  (void)pickerView;
  return 2;
}

- (NSInteger)pickerView:(UIPickerView*)pickerView numberOfRowsInComponent:(NSInteger)component
{
  (void)pickerView;
  return component == 0 ? 12 : 9999;
}

- (CGFloat)pickerView:(UIPickerView*)pickerView widthForComponent:(NSInteger)component
{
  (void)pickerView;
  return component == 0 ? 160.0f : 100.0f;
}

- (NSString*)pickerView:(UIPickerView*)pickerView titleForRow:(NSInteger)row forComponent:(NSInteger)component
{
  (void)pickerView;
  if (component == 0)
  {
    if (row >= 0 && row < monthSymbols.count)
      return [monthSymbols objectAtIndex:(NSUInteger)row];
    return @"";
  }

  return [NSString stringWithFormat:@"%ld", (long)(row + 1)];
}

- (void)pickerView:(UIPickerView*)pickerView didSelectRow:(NSInteger)row inComponent:(NSInteger)component
{
  (void)row;
  (void)component;
  const int month = (int)[pickerView selectedRowInComponent:0] + 1;
  const int year = (int)[pickerView selectedRowInComponent:1] + 1;
  [coordinator selectionDidChangeYear:year month:month];
}

@end

@implementation GlintIOSTimePickerViewController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  picker = nil;
  self.preferredContentSize = CGSizeMake(320.0f, 316.0f);
  return self;
}

- (void)dealloc
{
  [picker release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 320.0f, 316.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  picker = [[UIDatePicker alloc] initWithFrame:CGRectZero];
  picker.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.4, *))
    picker.preferredDatePickerStyle = UIDatePickerStyleWheels;
  picker.datePickerMode = UIDatePickerModeTime;
  picker.locale = NSLocale.currentLocale;
  picker.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  picker.timeZone = NSTimeZone.localTimeZone;

  UIButton* resetButton = [UIButton buttonWithType:UIButtonTypeSystem];
  resetButton.translatesAutoresizingMaskIntoConstraints = NO;
  [resetButton setTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_reset)) forState:UIControlStateNormal];
  [resetButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
  resetButton.backgroundColor = [UIColor colorWithWhite:0.93f alpha:1.0f];
  resetButton.layer.cornerRadius = 18.0f;
  resetButton.layer.masksToBounds = YES;
  [resetButton addTarget:self action:@selector(handleResetButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIButton* confirmButton = [UIButton buttonWithType:UIButtonTypeSystem];
  confirmButton.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.0, *))
  {
    [confirmButton setImage:[UIImage systemImageNamed:@"checkmark"] forState:UIControlStateNormal];
  }
  else
  {
    [confirmButton setTitle:@"✓" forState:UIControlStateNormal];
  }
  [confirmButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  confirmButton.tintColor = UIColor.whiteColor;
  confirmButton.backgroundColor = UIColor.systemBlueColor;
  confirmButton.layer.cornerRadius = 22.0f;
  confirmButton.layer.masksToBounds = YES;
  [confirmButton addTarget:self action:@selector(handleConfirmButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  [root addSubview:picker];
  [root addSubview:footer];
  [footer addSubview:resetButton];
  [footer addSubview:confirmButton];

  UILayoutGuide* safeArea = root.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [picker.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
    [picker.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
    [picker.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0f],
    [picker.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-16.0f],

    [footer.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [footer.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [footer.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-12.0f],
    [footer.heightAnchor constraintEqualToConstant:44.0f],

    [resetButton.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
    [resetButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [resetButton.heightAnchor constraintEqualToConstant:36.0f],
    [resetButton.widthAnchor constraintGreaterThanOrEqualToConstant:96.0f],

    [confirmButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
    [confirmButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [confirmButton.widthAnchor constraintEqualToConstant:44.0f],
    [confirmButton.heightAnchor constraintEqualToConstant:44.0f],
  ]];

  self.view = root;
  [root release];
}

- (void)handleResetButtonPressed:(id)sender
{
  [coordinator reset:sender];
}

- (void)handleConfirmButtonPressed:(id)sender
{
  [coordinator confirm:sender];
}

@end

@implementation GlintIOSWeekPickerViewController

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  coordinator = nil;
  calendarView = nil;
  selection = nil;
  self.preferredContentSize = CGSizeMake(340.0f, 420.0f);
  return self;
}

- (void)dealloc
{
  [selection release];
  [calendarView release];
  [super dealloc];
}

- (void)loadView
{
  UIView* root = [[UIView alloc] initWithFrame:CGRectMake(0.0f, 0.0f, 340.0f, 420.0f)];
  root.backgroundColor = UIColor.systemBackgroundColor;

  UIView* footer = [[[UIView alloc] initWithFrame:CGRectZero] autorelease];
  footer.translatesAutoresizingMaskIntoConstraints = NO;

  calendarView = [[UICalendarView alloc] initWithFrame:CGRectZero];
  calendarView.translatesAutoresizingMaskIntoConstraints = NO;
  calendarView.locale = NSLocale.currentLocale;
  calendarView.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierISO8601] autorelease];
  calendarView.timeZone = NSTimeZone.localTimeZone;
  calendarView.tintColor = UIColor.systemBlueColor;
  calendarView.delegate = self;

  selection = [[UICalendarSelectionWeekOfYear alloc] initWithDelegate:self];
  calendarView.selectionBehavior = selection;

  UIButton* resetButton = [UIButton buttonWithType:UIButtonTypeSystem];
  resetButton.translatesAutoresizingMaskIntoConstraints = NO;
  [resetButton setTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_reset)) forState:UIControlStateNormal];
  [resetButton setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
  resetButton.backgroundColor = [UIColor colorWithWhite:0.93f alpha:1.0f];
  resetButton.layer.cornerRadius = 18.0f;
  resetButton.layer.masksToBounds = YES;
  [resetButton addTarget:self action:@selector(handleResetButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  UIButton* confirmButton = [UIButton buttonWithType:UIButtonTypeSystem];
  confirmButton.translatesAutoresizingMaskIntoConstraints = NO;
  if (@available(iOS 13.0, *))
  {
    [confirmButton setImage:[UIImage systemImageNamed:@"checkmark"] forState:UIControlStateNormal];
  }
  else
  {
    [confirmButton setTitle:@"✓" forState:UIControlStateNormal];
  }
  [confirmButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
  confirmButton.tintColor = UIColor.whiteColor;
  confirmButton.backgroundColor = UIColor.systemBlueColor;
  confirmButton.layer.cornerRadius = 22.0f;
  confirmButton.layer.masksToBounds = YES;
  [confirmButton addTarget:self action:@selector(handleConfirmButtonPressed:) forControlEvents:UIControlEventTouchUpInside];

  [root addSubview:calendarView];
  [root addSubview:footer];
  [footer addSubview:resetButton];
  [footer addSubview:confirmButton];

  UILayoutGuide* safeArea = root.safeAreaLayoutGuide;
  [NSLayoutConstraint activateConstraints:@[
    [calendarView.leadingAnchor constraintEqualToAnchor:root.leadingAnchor constant:8.0f],
    [calendarView.trailingAnchor constraintEqualToAnchor:root.trailingAnchor constant:-8.0f],
    [calendarView.topAnchor constraintEqualToAnchor:safeArea.topAnchor constant:8.0f],
    [calendarView.bottomAnchor constraintEqualToAnchor:footer.topAnchor constant:-16.0f],

    [footer.leadingAnchor constraintEqualToAnchor:safeArea.leadingAnchor constant:16.0f],
    [footer.trailingAnchor constraintEqualToAnchor:safeArea.trailingAnchor constant:-16.0f],
    [footer.bottomAnchor constraintEqualToAnchor:safeArea.bottomAnchor constant:-12.0f],
    [footer.heightAnchor constraintEqualToConstant:44.0f],

    [resetButton.leadingAnchor constraintEqualToAnchor:footer.leadingAnchor],
    [resetButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [resetButton.heightAnchor constraintEqualToConstant:36.0f],
    [resetButton.widthAnchor constraintGreaterThanOrEqualToConstant:96.0f],

    [confirmButton.trailingAnchor constraintEqualToAnchor:footer.trailingAnchor],
    [confirmButton.centerYAnchor constraintEqualToAnchor:footer.centerYAnchor],
    [confirmButton.widthAnchor constraintEqualToConstant:44.0f],
    [confirmButton.heightAnchor constraintEqualToConstant:44.0f],
  ]];

  self.view = root;
  [root release];
}

- (void)handleResetButtonPressed:(id)sender
{
  [coordinator reset:sender];
}

- (void)handleConfirmButtonPressed:(id)sender
{
  [coordinator confirm:sender];
}

- (void)weekOfYearSelection:(UICalendarSelectionWeekOfYear*)weekSelection didSelectWeekOfYear:(NSDateComponents*)weekOfYearComponents
{
  (void)weekSelection;
  if (!coordinator || !weekOfYearComponents)
    return;

  coordinator->selectedWeekYear = (int)weekOfYearComponents.yearForWeekOfYear;
  coordinator->selectedWeek = (int)weekOfYearComponents.weekOfYear;
  if (coordinator->onChange)
    coordinator->onChange(coordinator->selectedWeekYear, coordinator->selectedWeek);
}

- (BOOL)weekOfYearSelection:(UICalendarSelectionWeekOfYear*)weekSelection canSelectWeekOfYear:(NSDateComponents*)weekOfYearComponents
{
  (void)weekSelection;
  return weekOfYearComponents != nil;
}

@end

@implementation GlintIOSMenuTracker

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController
{
  (void)presentationController;
  finished = YES;
}

@end

@implementation GlintIOSMenuItem

+ (instancetype)itemWithId:(int)itemIdValue
                     title:(NSString*)titleValue
                   enabled:(BOOL)enabledValue
                   checked:(BOOL)checkedValue
                 separator:(BOOL)separatorValue
{
  return [self itemWithId:itemIdValue
                    title:titleValue
          systemImageName:nil
                  enabled:enabledValue
                  checked:checkedValue
                separator:separatorValue];
}

+ (instancetype)itemWithId:(int)itemIdValue
                     title:(NSString*)titleValue
           systemImageName:(NSString*)systemImageNameValue
                   enabled:(BOOL)enabledValue
                   checked:(BOOL)checkedValue
                 separator:(BOOL)separatorValue
{
  GlintIOSMenuItem* item = [[[self alloc] init] autorelease];
  item->itemId = itemIdValue;
  item->title = [titleValue copy];
  item->systemImageName = [systemImageNameValue copy];
  item->enabled = enabledValue;
  item->checked = checkedValue;
  item->separator = separatorValue;
  return item;
}

- (void)dealloc
{
  [title release];
  [systemImageName release];
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
  self.title = glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_options));
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
  cell.imageView.image = item->systemImageName.length > 0 ? [UIImage systemImageNamed:item->systemImageName] : nil;
  cell.imageView.tintColor = cell.textLabel.textColor;
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
  self.autoresizingMask = UIViewAutoresizingNone;
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
    CGRect sourceRect = glint_source_rect_for_point(hostView, point);
    sourcePoint = CGPointMake(CGRectGetMidX(sourceRect), CGRectGetMidY(sourceRect));
    menuTriggered = NO;

    menuControl = [[GlintIOSSelectMenuControl alloc] initWithFrame:sourceRect
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
    UIImage* image = item->systemImageName.length > 0 ? [UIImage systemImageNamed:item->systemImageName] : nil;
    UIAction* action = [UIAction actionWithTitle:item->title image:image identifier:nil handler:^(__kindof UIAction* selectedAction) {
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

    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad)
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

@implementation GlintIOSTimePickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  navigationController = nil;
  contentController = nil;
  initialHour = 0;
  initialMinute = 0;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [presenter release];
  [sourceView release];
  [navigationController release];
  [contentController release];
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

- (NSDate*)dateForHour:(int)hour minute:(int)minute
{
  NSCalendar* calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendar.timeZone = NSTimeZone.localTimeZone;
  NSDateComponents* today = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay
                                        fromDate:NSDate.date];
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = calendar;
  components.timeZone = calendar.timeZone;
  components.year = today.year;
  components.month = today.month;
  components.day = today.day;
  components.hour = hour;
  components.minute = minute;
  return [calendar dateFromComponents:components] ?: NSDate.date;
}

- (void)ensureControllers
{
  if (!contentController)
  {
    contentController = [[GlintIOSTimePickerViewController alloc] init];
    contentController->coordinator = self;
  }

  [contentController loadViewIfNeeded];

  if (!navigationController)
  {
    navigationController = [[UINavigationController alloc] initWithRootViewController:contentController];
    navigationController.modalPresentationStyle = UIModalPresentationPopover;
    navigationController.navigationBarHidden = YES;
  }

  navigationController.preferredContentSize = contentController.preferredContentSize;
}

- (void)presentWithHour:(int)hour minute:(int)minute anchorScreenRect:(RECT)anchorScreenRect
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

  [self ensureControllers];

  [presenter release];
  presenter = [top retain];

  UIView* resolvedSourceView = presenter.view ?: window;
  [sourceView release];
  sourceView = [resolvedSourceView retain];
  sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);
  initialHour = hour;
  initialMinute = minute;

  [contentController->picker setDate:[self dateForHour:hour minute:minute] animated:NO];
  closedNotified = NO;

  UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
  popover.delegate = self;
  popover.sourceView = sourceView;
  popover.sourceRect = sourceRect;
  popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

  if (navigationController.presentingViewController)
    return;

  [presenter presentViewController:navigationController animated:YES completion:nil];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  if (navigationController.presentingViewController)
  {
    [navigationController.presentingViewController dismissViewControllerAnimated:animated completion:^{
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
  onConfirm = nullptr;
  onReset = nullptr;
  onClosed = nullptr;
  if (navigationController.popoverPresentationController)
    navigationController.popoverPresentationController.delegate = nil;
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)cancel:(id)sender
{
  (void)sender;
  [self hideAnimated:YES notifyClosed:YES];
}

- (void)reset:(id)sender
{
  (void)sender;
  [contentController->picker setDate:[self dateForHour:initialHour minute:initialMinute] animated:YES];
}

- (void)confirm:(id)sender
{
  (void)sender;
  if (onConfirm)
  {
    NSCalendar* calendar = contentController->picker.calendar ?: [NSCalendar currentCalendar];
    NSDateComponents* components = [calendar components:NSCalendarUnitHour | NSCalendarUnitMinute
                                               fromDate:contentController->picker.date ?: NSDate.date];
    onConfirm((int)components.hour, (int)components.minute);
  }
  [self hideAnimated:YES notifyClosed:YES];
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

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSMonthPickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  navigationController = nil;
  contentController = nil;
  initialYear = 1;
  initialMonth = 1;
  selectedYear = 1;
  selectedMonth = 1;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [presenter release];
  [sourceView release];
  [navigationController release];
  [contentController release];
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

- (void)ensureControllers
{
  if (!contentController)
  {
    contentController = [[GlintIOSMonthPickerViewController alloc] init];
    contentController->coordinator = self;
  }

  [contentController loadViewIfNeeded];

  if (!navigationController)
  {
    navigationController = [[UINavigationController alloc] initWithRootViewController:contentController];
    navigationController.modalPresentationStyle = UIModalPresentationPopover;
    navigationController.navigationBarHidden = YES;
  }

  navigationController.preferredContentSize = contentController.preferredContentSize;
}

- (void)presentWithYear:(int)year month:(int)month anchorScreenRect:(RECT)anchorScreenRect
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

  [self ensureControllers];

  [presenter release];
  presenter = [top retain];

  UIView* resolvedSourceView = presenter.view ?: window;
  [sourceView release];
  sourceView = [resolvedSourceView retain];
  sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);

  initialYear = std::max(1, year);
  initialMonth = std::max(1, std::min(12, month));
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  [contentController syncSelectionAnimated:NO];
  closedNotified = NO;

  UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
  popover.delegate = self;
  popover.sourceView = sourceView;
  popover.sourceRect = sourceRect;
  popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

  if (navigationController.presentingViewController)
    return;

  [presenter presentViewController:navigationController animated:YES completion:nil];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  if (navigationController.presentingViewController)
  {
    [navigationController.presentingViewController dismissViewControllerAnimated:animated completion:^{
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
  onConfirm = nullptr;
  onReset = nullptr;
  onClosed = nullptr;
  if (navigationController.popoverPresentationController)
    navigationController.popoverPresentationController.delegate = nil;
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)selectionDidChangeYear:(int)year month:(int)month
{
  year = std::max(1, year);
  month = std::max(1, std::min(12, month));
  if (selectedYear == year && selectedMonth == month)
    return;

  selectedYear = year;
  selectedMonth = month;
  if (onChange)
    onChange(selectedYear, selectedMonth);
}

- (void)reset:(id)sender
{
  (void)sender;
  const bool changed = selectedYear != initialYear || selectedMonth != initialMonth;
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  [contentController syncSelectionAnimated:YES];
  if (onReset)
    onReset();
  if (changed && onChange)
    onChange(selectedYear, selectedMonth);
}

- (void)confirm:(id)sender
{
  (void)sender;
  if (onConfirm)
    onConfirm(selectedYear, selectedMonth);
  [self hideAnimated:YES notifyClosed:YES];
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

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSDatePickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  navigationController = nil;
  contentController = nil;
  initialYear = 1;
  initialMonth = 1;
  initialDay = 1;
  selectedYear = 1;
  selectedMonth = 1;
  selectedDay = 1;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [presenter release];
  [sourceView release];
  [navigationController release];
  [contentController release];
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

- (NSDateComponents*)dateComponentsForYear:(int)year month:(int)month day:(int)day
{
  NSCalendar* calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = calendar;
  components.timeZone = NSTimeZone.localTimeZone;
  components.year = year;
  components.month = month;
  components.day = day;
  return components;
}

- (void)ensureControllers
{
  if (!contentController)
  {
    contentController = [[GlintIOSDatePickerViewController alloc] init];
    contentController->coordinator = self;
  }

  [contentController loadViewIfNeeded];

  if (!navigationController)
  {
    navigationController = [[UINavigationController alloc] initWithRootViewController:contentController];
    navigationController.modalPresentationStyle = UIModalPresentationPopover;
    navigationController.navigationBarHidden = YES;
  }

  navigationController.preferredContentSize = contentController.preferredContentSize;
}

- (void)presentWithYear:(int)year month:(int)month day:(int)day anchorScreenRect:(RECT)anchorScreenRect
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

  [self ensureControllers];

  [presenter release];
  presenter = [top retain];

  UIView* resolvedSourceView = presenter.view ?: window;
  [sourceView release];
  sourceView = [resolvedSourceView retain];
  sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);
  initialYear = std::max(1, year);
  initialMonth = std::max(1, std::min(12, month));
  initialDay = std::max(1, day);
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  selectedDay = initialDay;

  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  [contentController->calendarView setVisibleDateComponents:selectedDate animated:NO];
  [contentController->selection setSelectedDate:selectedDate animated:NO];
  closedNotified = NO;

  UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
  popover.delegate = self;
  popover.sourceView = sourceView;
  popover.sourceRect = sourceRect;
  popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

  if (navigationController.presentingViewController)
    return;

  [presenter presentViewController:navigationController animated:YES completion:nil];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  if (navigationController.presentingViewController)
  {
    [navigationController.presentingViewController dismissViewControllerAnimated:animated completion:^{
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
  onConfirm = nullptr;
  onReset = nullptr;
  onClosed = nullptr;
  if (navigationController.popoverPresentationController)
    navigationController.popoverPresentationController.delegate = nil;
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)selectionDidChangeYear:(int)year month:(int)month day:(int)day
{
  if (selectedYear == year && selectedMonth == month && selectedDay == day)
    return;

  selectedYear = year;
  selectedMonth = month;
  selectedDay = day;
  if (onChange)
    onChange(selectedYear, selectedMonth, selectedDay);
}

- (NSDateComponents*)handleVisibleMonthChangeYear:(int)year month:(int)month
{
  year = std::max(1, year);
  month = std::max(1, std::min(12, month));

  NSCalendar* calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  const int clampedDay = std::min(selectedDay, glint_days_in_calendar_month(calendar, year, month));
  if (selectedYear == year && selectedMonth == month && selectedDay == clampedDay)
    return nil;

  selectedYear = year;
  selectedMonth = month;
  selectedDay = clampedDay;
  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  if (onChange)
    onChange(selectedYear, selectedMonth, selectedDay);
  return selectedDate;
}

- (void)reset:(id)sender
{
  (void)sender;
  const bool changed = selectedYear != initialYear || selectedMonth != initialMonth || selectedDay != initialDay;
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  selectedDay = initialDay;
  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  [contentController->selection setSelectedDate:selectedDate animated:YES];
  [contentController->calendarView setVisibleDateComponents:selectedDate animated:YES];
  if (onReset)
    onReset();
  if (changed && onChange)
    onChange(selectedYear, selectedMonth, selectedDay);
}

- (void)confirm:(id)sender
{
  (void)sender;
  if (onConfirm)
    onConfirm(selectedYear, selectedMonth, selectedDay);
  [self hideAnimated:YES notifyClosed:YES];
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

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSDateTimeLocalPickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  navigationController = nil;
  contentController = nil;
  timePopoverController = nil;
  initialYear = 1;
  initialMonth = 1;
  initialDay = 1;
  initialHour = 0;
  initialMinute = 0;
  selectedYear = 1;
  selectedMonth = 1;
  selectedDay = 1;
  selectedHour = 0;
  selectedMinute = 0;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [presenter release];
  [sourceView release];
  [navigationController release];
  [contentController release];
  [timePopoverController release];
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

- (NSDateComponents*)dateComponentsForYear:(int)year month:(int)month day:(int)day
{
  NSCalendar* calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = calendar;
  components.timeZone = NSTimeZone.localTimeZone;
  components.year = year;
  components.month = month;
  components.day = day;
  return components;
}

- (NSDate*)timePickerDateForHour:(int)hour minute:(int)minute
{
  NSCalendar* calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  calendar.timeZone = NSTimeZone.localTimeZone;
  NSDateComponents* today = [calendar components:NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay fromDate:NSDate.date];
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = calendar;
  components.timeZone = calendar.timeZone;
  components.year = today.year;
  components.month = today.month;
  components.day = today.day;
  components.hour = hour;
  components.minute = minute;
  return [calendar dateFromComponents:components] ?: NSDate.date;
}

- (void)ensureControllers
{
  if (!contentController)
  {
    contentController = [[GlintIOSDateTimeLocalPickerViewController alloc] init];
    contentController->coordinator = self;
  }

  [contentController loadViewIfNeeded];

  if (!navigationController)
  {
    navigationController = [[UINavigationController alloc] initWithRootViewController:contentController];
    navigationController.modalPresentationStyle = UIModalPresentationPopover;
    navigationController.navigationBarHidden = YES;
  }

  if (!timePopoverController)
  {
    timePopoverController = [[GlintIOSDateTimeLocalTimePopoverController alloc] init];
    timePopoverController->coordinator = self;
  }

  navigationController.preferredContentSize = contentController.preferredContentSize;
}

- (void)presentWithYear:(int)year month:(int)month day:(int)day hour:(int)hour minute:(int)minute anchorScreenRect:(RECT)anchorScreenRect
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

  [self ensureControllers];

  [presenter release];
  presenter = [top retain];

  UIView* resolvedSourceView = presenter.view ?: window;
  [sourceView release];
  sourceView = [resolvedSourceView retain];
  sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);
  initialYear = std::max(1, year);
  initialMonth = std::max(1, std::min(12, month));
  initialDay = std::max(1, day);
  initialHour = std::max(0, std::min(23, hour));
  initialMinute = std::max(0, std::min(59, minute));
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  selectedDay = initialDay;
  selectedHour = initialHour;
  selectedMinute = initialMinute;

  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  [contentController->calendarView setVisibleDateComponents:selectedDate animated:NO];
  [contentController->selection setSelectedDate:selectedDate animated:NO];
  [contentController updateTimeButtonTitle];
  [timePopoverController syncSelectionAnimated:NO];
  closedNotified = NO;

  UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
  popover.delegate = self;
  popover.sourceView = sourceView;
  popover.sourceRect = sourceRect;
  popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

  if (navigationController.presentingViewController)
    return;

  [presenter presentViewController:navigationController animated:YES completion:nil];
}

- (void)dismissTimePopoverAnimated:(BOOL)animated
{
  if (timePopoverController.presentingViewController)
    [timePopoverController.presentingViewController dismissViewControllerAnimated:animated completion:nil];
}

- (void)presentTimePopoverFromSourceView:(UIView*)timeSourceView
{
  if (!timeSourceView)
    return;

  [self ensureControllers];
  [timePopoverController syncSelectionAnimated:NO];

  UIPopoverPresentationController* popover = timePopoverController.popoverPresentationController;
  popover.delegate = timePopoverController;
  popover.sourceView = timeSourceView;
  popover.sourceRect = timeSourceView.bounds;
  popover.permittedArrowDirections = UIPopoverArrowDirectionUp | UIPopoverArrowDirectionDown;

  if (timePopoverController.presentingViewController)
    return;

  [contentController presentViewController:timePopoverController animated:YES completion:nil];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  [self dismissTimePopoverAnimated:NO];

  if (navigationController.presentingViewController)
  {
    [navigationController.presentingViewController dismissViewControllerAnimated:animated completion:^{
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
  onConfirm = nullptr;
  onReset = nullptr;
  onClosed = nullptr;
  if (navigationController.popoverPresentationController)
    navigationController.popoverPresentationController.delegate = nil;
  [self dismissTimePopoverAnimated:NO];
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)selectionDidChangeYear:(int)year month:(int)month day:(int)day
{
  if (selectedYear == year && selectedMonth == month && selectedDay == day)
    return;

  selectedYear = year;
  selectedMonth = month;
  selectedDay = day;
  if (onChange)
    onChange(selectedYear, selectedMonth, selectedDay, selectedHour, selectedMinute);
}

- (NSDateComponents*)handleVisibleMonthChangeYear:(int)year month:(int)month
{
  year = std::max(1, year);
  month = std::max(1, std::min(12, month));

  NSCalendar* calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
  const int clampedDay = std::min(selectedDay, glint_days_in_calendar_month(calendar, year, month));
  if (selectedYear == year && selectedMonth == month && selectedDay == clampedDay)
    return nil;

  selectedYear = year;
  selectedMonth = month;
  selectedDay = clampedDay;
  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  if (onChange)
    onChange(selectedYear, selectedMonth, selectedDay, selectedHour, selectedMinute);
  return selectedDate;
}

- (void)selectionDidChangeHour:(int)hour minute:(int)minute
{
  hour = std::max(0, std::min(23, hour));
  minute = std::max(0, std::min(59, minute));
  if (selectedHour == hour && selectedMinute == minute)
    return;

  selectedHour = hour;
  selectedMinute = minute;
  [contentController updateTimeButtonTitle];
  if (onChange)
    onChange(selectedYear, selectedMonth, selectedDay, selectedHour, selectedMinute);
}

- (void)reset:(id)sender
{
  (void)sender;
  const bool changed = selectedYear != initialYear || selectedMonth != initialMonth || selectedDay != initialDay || selectedHour != initialHour || selectedMinute != initialMinute;
  selectedYear = initialYear;
  selectedMonth = initialMonth;
  selectedDay = initialDay;
  selectedHour = initialHour;
  selectedMinute = initialMinute;
  NSDateComponents* selectedDate = [self dateComponentsForYear:selectedYear month:selectedMonth day:selectedDay];
  [contentController->selection setSelectedDate:selectedDate animated:YES];
  [contentController->calendarView setVisibleDateComponents:selectedDate animated:YES];
  [contentController updateTimeButtonTitle];
  [timePopoverController syncSelectionAnimated:YES];
  if (onReset)
    onReset();
  if (changed && onChange)
    onChange(selectedYear, selectedMonth, selectedDay, selectedHour, selectedMinute);
}

- (void)confirm:(id)sender
{
  (void)sender;
  if (onConfirm)
    onConfirm(selectedYear, selectedMonth, selectedDay, selectedHour, selectedMinute);
  [self hideAnimated:YES notifyClosed:YES];
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

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSWeekPickerCoordinator

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  presenter = nil;
  sourceView = nil;
  sourceRect = CGRectZero;
  navigationController = nil;
  contentController = nil;
  initialWeekYear = 1;
  initialWeek = 1;
  selectedWeekYear = 1;
  selectedWeek = 1;
  closedNotified = YES;
  return self;
}

- (void)dealloc
{
  [presenter release];
  [sourceView release];
  [navigationController release];
  [contentController release];
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

- (NSDateComponents*)weekOfYearComponentsForWeekYear:(int)weekYear week:(int)week
{
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierISO8601] autorelease];
  components.timeZone = NSTimeZone.localTimeZone;
  components.yearForWeekOfYear = weekYear;
  components.weekOfYear = week;
  return components;
}

- (NSDateComponents*)visibleDateComponentsForWeekYear:(int)weekYear week:(int)week
{
  const glint_ymd monday = glint_ymd_from_iso_week(weekYear, week, 1);
  NSDateComponents* components = [[[NSDateComponents alloc] init] autorelease];
  components.calendar = contentController->calendarView.calendar ?: [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierISO8601] autorelease];
  components.timeZone = NSTimeZone.localTimeZone;
  components.year = monday.year;
  components.month = monday.month;
  components.day = monday.day;
  return components;
}

- (void)ensureControllers
{
  if (!contentController)
  {
    contentController = [[GlintIOSWeekPickerViewController alloc] init];
    contentController->coordinator = self;
  }

  [contentController loadViewIfNeeded];

  if (!navigationController)
  {
    navigationController = [[UINavigationController alloc] initWithRootViewController:contentController];
    navigationController.modalPresentationStyle = UIModalPresentationPopover;
    navigationController.navigationBarHidden = YES;
  }

  navigationController.preferredContentSize = contentController.preferredContentSize;
}

- (void)presentWithWeekYear:(int)weekYear week:(int)week anchorScreenRect:(RECT)anchorScreenRect
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

  [self ensureControllers];

  [presenter release];
  presenter = [top retain];

  UIView* resolvedSourceView = presenter.view ?: window;
  [sourceView release];
  sourceView = [resolvedSourceView retain];
  sourceRect = glint_colorpicker_source_rect(sourceView, anchorScreenRect);
  initialWeekYear = weekYear;
  initialWeek = week;
  selectedWeekYear = weekYear;
  selectedWeek = week;

  [contentController->calendarView setVisibleDateComponents:[self visibleDateComponentsForWeekYear:weekYear week:week] animated:NO];
  [contentController->selection setSelectedWeekOfYear:[self weekOfYearComponentsForWeekYear:weekYear week:week] animated:NO];
  closedNotified = NO;

  UIPopoverPresentationController* popover = navigationController.popoverPresentationController;
  popover.delegate = self;
  popover.sourceView = sourceView;
  popover.sourceRect = sourceRect;
  popover.permittedArrowDirections = UIPopoverArrowDirectionAny;

  if (navigationController.presentingViewController)
    return;

  [presenter presentViewController:navigationController animated:YES completion:nil];
}

- (void)hideAnimated:(BOOL)animated notifyClosed:(BOOL)notifyClosed
{
  if (notifyClosed)
    closedNotified = NO;

  if (navigationController.presentingViewController)
  {
    [navigationController.presentingViewController dismissViewControllerAnimated:animated completion:^{
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
  onConfirm = nullptr;
  onReset = nullptr;
  onClosed = nullptr;
  if (navigationController.popoverPresentationController)
    navigationController.popoverPresentationController.delegate = nil;
  [self hideAnimated:NO notifyClosed:NO];
}

- (void)cancel:(id)sender
{
  (void)sender;
  [self hideAnimated:YES notifyClosed:YES];
}

- (void)reset:(id)sender
{
  (void)sender;
  const bool changed = selectedWeekYear != initialWeekYear || selectedWeek != initialWeek;
  selectedWeekYear = initialWeekYear;
  selectedWeek = initialWeek;
  [contentController->selection setSelectedWeekOfYear:[self weekOfYearComponentsForWeekYear:initialWeekYear week:initialWeek] animated:YES];
  [contentController->calendarView setVisibleDateComponents:[self visibleDateComponentsForWeekYear:initialWeekYear week:initialWeek] animated:YES];
  if (onReset)
    onReset();
  if (changed && onChange)
    onChange(selectedWeekYear, selectedWeek);
}

- (void)confirm:(id)sender
{
  (void)sender;
  if (onConfirm)
    onConfirm(selectedWeekYear, selectedWeek);
  [self hideAnimated:YES notifyClosed:YES];
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

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller
{
  (void)controller;
  return UIModalPresentationNone;
}

- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController*)controller traitCollection:(UITraitCollection*)traitCollection
{
  (void)controller;
  (void)traitCollection;
  return UIModalPresentationNone;
}

@end

@implementation GlintIOSFilePickerTracker

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  selectedPaths = [[NSMutableArray alloc] init];
  finished = NO;
  return self;
}

- (void)dealloc
{
  [selectedPaths release];
  [super dealloc];
}

- (void)finishWithURLs:(NSArray<NSURL*>*)urls
{
  [selectedPaths removeAllObjects];
  for (NSURL* url in urls)
  {
    if (!url)
      continue;

    NSString* path = url.path;
    if (path.length > 0)
      [selectedPaths addObject:path];
  }
  finished = YES;
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls
{
  (void)controller;
  [self finishWithURLs:urls ?: @[]];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentAtURL:(NSURL*)url
{
  (void)controller;
  [self finishWithURLs:url ? @[url] : @[]];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller
{
  (void)controller;
  finished = YES;
}

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController
{
  (void)presentationController;
  finished = YES;
}

@end

@implementation GlintIOSMediaPickerTracker

- (instancetype)init
{
  if (!(self = [super init]))
    return nil;

  selectedPaths = [[NSMutableArray alloc] init];
  finished = NO;
  return self;
}

- (void)dealloc
{
  [selectedPaths release];
  [super dealloc];
}

- (NSString*)temporaryPathWithExtension:(NSString*)extension
{
  NSString* normalized = extension ?: @"tmp";
  if ([normalized hasPrefix:@"."])
    normalized = [normalized substringFromIndex:1];
  if (normalized.length == 0)
    normalized = @"tmp";

  NSString* directory = [NSTemporaryDirectory() stringByAppendingPathComponent:@"glint-file-picker"];
  [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];

  NSString* fileName = [[NSUUID UUID].UUIDString stringByAppendingPathExtension:normalized];
  return [directory stringByAppendingPathComponent:fileName];
}

- (NSString*)copyURLToTemporaryLocation:(NSURL*)url suggestedExtension:(NSString*)extension
{
  if (!url)
    return nil;

  NSString* destinationPath = [self temporaryPathWithExtension:(extension.length > 0 ? extension : url.pathExtension)];
  NSURL* destinationURL = [NSURL fileURLWithPath:destinationPath];
  NSFileManager* fileManager = [NSFileManager defaultManager];

  if ([url isFileURL] && [fileManager copyItemAtURL:url toURL:destinationURL error:nil])
    return destinationPath;

  NSData* data = [NSData dataWithContentsOfURL:url];
  if (data && [data writeToURL:destinationURL atomically:YES])
    return destinationPath;

  return nil;
}

- (NSString*)writeImageToTemporaryLocation:(UIImage*)image sourceURL:(NSURL*)sourceURL
{
  if (!image)
    return nil;

  NSString* sourceExtension = sourceURL.pathExtension.lowercaseString;
  NSString* outputExtension = @"jpg";
  NSData* imageData = nil;

  if ([sourceExtension isEqualToString:@"png"])
  {
    imageData = UIImagePNGRepresentation(image);
    outputExtension = @"png";
  }

  if (!imageData)
  {
    imageData = UIImageJPEGRepresentation(image, 0.92);
    outputExtension = @"jpg";
  }

  if (!imageData)
    return nil;

  NSString* destinationPath = [self temporaryPathWithExtension:outputExtension];
  return [imageData writeToFile:destinationPath atomically:YES] ? destinationPath : nil;
}

- (void)imagePickerController:(UIImagePickerController*)picker didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id>*)info
{
  [selectedPaths removeAllObjects];

  NSString* mediaType = info[UIImagePickerControllerMediaType];
  NSString* selectedPath = nil;
  if ([mediaType isEqualToString:@"public.movie"])
  {
    selectedPath = [self copyURLToTemporaryLocation:info[UIImagePickerControllerMediaURL] suggestedExtension:nil];
  }
  else
  {
    NSURL* imageURL = nil;
    if (@available(iOS 11.0, *))
      imageURL = info[UIImagePickerControllerImageURL];

    if (imageURL && imageURL.isFileURL)
      selectedPath = [self copyURLToTemporaryLocation:imageURL suggestedExtension:nil];
    if (!selectedPath)
      selectedPath = [self writeImageToTemporaryLocation:info[UIImagePickerControllerOriginalImage] sourceURL:imageURL];
  }

  if (selectedPath.length > 0)
    [selectedPaths addObject:selectedPath];

  [picker.presentingViewController dismissViewControllerAnimated:YES completion:^{
    finished = YES;
  }];
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController*)picker
{
  [picker.presentingViewController dismissViewControllerAnimated:YES completion:^{
    finished = YES;
  }];
}

- (void)picker:(PHPickerViewController*)picker didFinishPicking:(NSArray<PHPickerResult*>*)results
{
  [selectedPaths removeAllObjects];

  if (!results.count)
  {
    [picker.presentingViewController dismissViewControllerAnimated:YES completion:^{
      finished = YES;
    }];
    return;
  }

  dispatch_group_t group = dispatch_group_create();
  for (PHPickerResult* result in results)
  {
    NSItemProvider* itemProvider = result.itemProvider;
    NSString* typeIdentifier = nil;
    if ([itemProvider hasItemConformingToTypeIdentifier:@"public.movie"])
      typeIdentifier = @"public.movie";
    else if ([itemProvider hasItemConformingToTypeIdentifier:@"public.image"])
      typeIdentifier = @"public.image";
    else if (itemProvider.registeredTypeIdentifiers.count > 0)
      typeIdentifier = itemProvider.registeredTypeIdentifiers.firstObject;

    if (!typeIdentifier)
      continue;

    dispatch_group_enter(group);
    [itemProvider loadFileRepresentationForTypeIdentifier:typeIdentifier completionHandler:^(NSURL* url, NSError* error) {
      (void)error;
      NSString* selectedPath = [self copyURLToTemporaryLocation:url suggestedExtension:nil];
      if (selectedPath.length > 0)
      {
        @synchronized (self)
        {
          [selectedPaths addObject:selectedPath];
        }
      }
      dispatch_group_leave(group);
    }];
  }

  dispatch_group_notify(group, dispatch_get_main_queue(), ^{
    [picker.presentingViewController dismissViewControllerAnimated:YES completion:^{
      finished = YES;
    }];
  });
}

- (void)presentationControllerDidDismiss:(UIPresentationController*)presentationController
{
  (void)presentationController;
  finished = YES;
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
    self.contentScaleFactor = glint_display_scale_for_view(self);
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
    keyboardProxyField->ownerView = self;
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
    keyboardTemporalPicker = nil;
    keyboardPrewarmScheduled = NO;
    keyboardPrewarmActive = NO;
    keyboardPrewarmDone = NO;
    lastTemporalInputKind = glint_temporal_input_kind_none;
  }
  return self;
}

- (void)dealloc
{
  [NSNotificationCenter.defaultCenter removeObserver:self name:UITextFieldTextDidChangeNotification object:keyboardProxyField];
  [NSNotificationCenter.defaultCenter removeObserver:self name:UITextFieldTextDidChangeNotification object:keyboardSearchBar.searchTextField];
  keyboardProxyField->cppView = nullptr;
  keyboardProxyField->ownerView = nil;
  glint_set_keyboard_cpp_view(keyboardSearchBar.searchTextField, nullptr);
  [lastTextContentType release];
  [keyboardTemporalPicker removeTarget:self action:@selector(handleTemporalPickerValueChanged:) forControlEvents:UIControlEventValueChanged];
  [keyboardTemporalPicker release];
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
  self.contentScaleFactor = glint_display_scale_for_view(self);
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

- (UIView*)activeKeyboardInputView
{
  if (!cppView)
    return nil;

  if (cppView->_focusedTemporalInputKind() != glint_temporal_input_kind_none)
  {
    [self syncTemporalPickerState];
    return keyboardTemporalPicker;
  }

  if (!cppView->_focusedSuppressesSoftwareKeyboard())
    return nil;

  static UIView* emptyInputView = nil;
  if (!emptyInputView)
    emptyInputView = [[UIView alloc] initWithFrame:CGRectZero];
  return emptyInputView;
}

- (void)syncTemporalPickerState
{
  if (!cppView)
    return;

  const glint_temporal_input_kind kind = (glint_temporal_input_kind) cppView->_focusedTemporalInputKind();
  if (kind == glint_temporal_input_kind_none)
    return;

  if (!keyboardTemporalPicker)
  {
    keyboardTemporalPicker = [[UIDatePicker alloc] initWithFrame:CGRectZero];
    if (@available(iOS 13.4, *))
      keyboardTemporalPicker.preferredDatePickerStyle = UIDatePickerStyleWheels;
    keyboardTemporalPicker.locale = NSLocale.currentLocale;
    keyboardTemporalPicker.calendar = [[[NSCalendar alloc] initWithCalendarIdentifier:NSCalendarIdentifierGregorian] autorelease];
    keyboardTemporalPicker.timeZone = NSTimeZone.localTimeZone;
    [keyboardTemporalPicker addTarget:self action:@selector(handleTemporalPickerValueChanged:) forControlEvents:UIControlEventValueChanged];
  }

  switch (kind)
  {
    case glint_temporal_input_kind_date:
      keyboardTemporalPicker.datePickerMode = UIDatePickerModeDate;
      keyboardTemporalPicker.minuteInterval = 1;
      break;
    case glint_temporal_input_kind_time:
      keyboardTemporalPicker.datePickerMode = UIDatePickerModeTime;
      keyboardTemporalPicker.minuteInterval = cppView->_focusedTemporalMinuteInterval();
      break;
    case glint_temporal_input_kind_none:
      return;
  }

  NSDate* date = glint_temporal_date_from_value(kind, cppView->_focusedTextValue());
  if (!date)
    date = NSDate.date;
  [keyboardTemporalPicker setDate:date animated:NO];
}

- (void)handleTemporalPickerValueChanged:(UIDatePicker*)sender
{
  if (!cppView || !sender)
    return;

  const glint_temporal_input_kind kind = (glint_temporal_input_kind) cppView->_focusedTemporalInputKind();
  if (kind == glint_temporal_input_kind_none)
    return;

  const std::string value = glint_temporal_value_from_date(kind, sender.date);
  if (value.empty())
    return;

  cppView->_replaceFocusedTextFromPlatform(value);

  suppressKeyboardFieldSync = YES;
  keyboardProxyField.text = glint_nsstring_from_utf8(value);
  suppressKeyboardFieldSync = NO;
}

- (void)syncKeyboardFocus
{
  if (!cppView)
    return;

  if (keyboardPrewarmActive && cppView->_focusedNodeWantsKeyboard())
    keyboardPrewarmActive = NO;

  const bool wantsKeyboard = cppView->_focusedNodeWantsKeyboard();
  const int temporalInputKind = cppView->_focusedTemporalInputKind();
  const bool wantsTemporalPicker = wantsKeyboard && temporalInputKind != glint_temporal_input_kind_none;
  const bool wantsNativeResponder = wantsKeyboard && (wantsTemporalPicker || cppView->_focusedNeedsNativeTextServices());
  const BOOL wantsSearchResponder = wantsKeyboard && !wantsTemporalPicker && cppView->_focusedReturnKeyType() == UIReturnKeySearch;
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
                          || lastUsesSearchResponder != wantsSearchResponder
                          || lastTemporalInputKind != temporalInputKind;

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
      [self syncTemporalPickerState];

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
  lastTemporalInputKind = temporalInputKind;
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

int glint_view_ios::_focusedTemporalInputKind() const
{
  if (!mDocument)
    return glint_temporal_input_kind_none;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return static_cast<int>(glint_temporal_kind_for_input(*input));

  return glint_temporal_input_kind_none;
}

int glint_view_ios::_focusedTemporalMinuteInterval() const
{
  if (!mDocument)
    return 1;

  const glint_element* focused = mDocument->getFocusedNode();
  if (const auto* input = dynamic_cast<const glint_text_input*>(focused))
    return glint_temporal_minute_interval_for_step(input->step);

  return 1;
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
    if (glint_temporal_kind_for_input(*input) != glint_temporal_input_kind_none)
      return true;
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

struct datetime_local_picker_handle
{
  GlintIOSDateTimeLocalPickerCoordinator* coordinator = nil;
};

struct datepicker_handle
{
  GlintIOSDatePickerCoordinator* coordinator = nil;
};

struct timepicker_handle
{
  GlintIOSTimePickerCoordinator* coordinator = nil;
};

struct monthpicker_handle
{
  GlintIOSMonthPickerCoordinator* coordinator = nil;
};

struct weekpicker_handle
{
  GlintIOSWeekPickerCoordinator* coordinator = nil;
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

datetime_local_picker_handle* showDateTimeLocalPicker(int initialYear,
                                                      int initialMonth,
                                                      int initialDay,
                                                      int initialHour,
                                                      int initialMinute,
                                                      const RECT& anchorScreenRect,
                                                      std::function<void(int, int, int, int, int)> onChange,
                                                      std::function<void(int, int, int, int, int)> onConfirm,
                                                      std::function<void()> onReset,
                                                      std::function<void()> onClosed)
{
  return reopenDateTimeLocalPicker(nullptr,
                                   initialYear,
                                   initialMonth,
                                   initialDay,
                                   initialHour,
                                   initialMinute,
                                   anchorScreenRect,
                                   std::move(onChange),
                                   std::move(onConfirm),
                                   std::move(onReset),
                                   std::move(onClosed));
}

datetime_local_picker_handle* reopenDateTimeLocalPicker(datetime_local_picker_handle* handle,
                                                        int initialYear,
                                                        int initialMonth,
                                                        int initialDay,
                                                        int initialHour,
                                                        int initialMinute,
                                                        const RECT& anchorScreenRect,
                                                        std::function<void(int, int, int, int, int)> onChange,
                                                        std::function<void(int, int, int, int, int)> onConfirm,
                                                        std::function<void()> onReset,
                                                        std::function<void()> onClosed)
{
  __block datetime_local_picker_handle* result = handle;
  __block std::function<void(int, int, int, int, int)> changeCb = std::move(onChange);
  __block std::function<void(int, int, int, int, int)> confirmCb = std::move(onConfirm);
  __block std::function<void()> resetCb = std::move(onReset);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new datetime_local_picker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSDateTimeLocalPickerCoordinator alloc] init];
    result->coordinator->onChange = std::move(changeCb);
    result->coordinator->onConfirm = std::move(confirmCb);
    result->coordinator->onReset = std::move(resetCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithYear:initialYear
                                   month:initialMonth
                                     day:initialDay
                                    hour:initialHour
                                  minute:initialMinute
                        anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideDateTimeLocalPicker(datetime_local_picker_handle* handle)
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

void destroyDateTimeLocalPicker(datetime_local_picker_handle* handle)
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

datepicker_handle* showDatePicker(int initialYear,
                                  int initialMonth,
                                  int initialDay,
                                  const RECT& anchorScreenRect,
                                  std::function<void(int, int, int)> onChange,
                                  std::function<void(int, int, int)> onConfirm,
                                  std::function<void()> onReset,
                                  std::function<void()> onClosed)
{
  return reopenDatePicker(nullptr,
                          initialYear,
                          initialMonth,
                          initialDay,
                          anchorScreenRect,
                          std::move(onChange),
                          std::move(onConfirm),
                          std::move(onReset),
                          std::move(onClosed));
}

datepicker_handle* reopenDatePicker(datepicker_handle* handle,
                                    int initialYear,
                                    int initialMonth,
                                    int initialDay,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int, int)> onChange,
                                    std::function<void(int, int, int)> onConfirm,
                                    std::function<void()> onReset,
                                    std::function<void()> onClosed)
{
  __block datepicker_handle* result = handle;
  __block std::function<void(int, int, int)> changeCb = std::move(onChange);
  __block std::function<void(int, int, int)> confirmCb = std::move(onConfirm);
  __block std::function<void()> resetCb = std::move(onReset);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new datepicker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSDatePickerCoordinator alloc] init];
    result->coordinator->onChange = std::move(changeCb);
    result->coordinator->onConfirm = std::move(confirmCb);
    result->coordinator->onReset = std::move(resetCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithYear:initialYear
                                   month:initialMonth
                                     day:initialDay
                        anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideDatePicker(datepicker_handle* handle)
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

void destroyDatePicker(datepicker_handle* handle)
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

timepicker_handle* showTimePicker(int initialHour,
                                  int initialMinute,
                                  const RECT& anchorScreenRect,
                                  std::function<void(int, int)> onConfirm,
                                  std::function<void()> onReset,
                                  std::function<void()> onClosed)
{
  return reopenTimePicker(nullptr,
                          initialHour,
                          initialMinute,
                          anchorScreenRect,
                          std::move(onConfirm),
                          std::move(onReset),
                          std::move(onClosed));
}

timepicker_handle* reopenTimePicker(timepicker_handle* handle,
                                    int initialHour,
                                    int initialMinute,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int)> onConfirm,
                                    std::function<void()> onReset,
                                    std::function<void()> onClosed)
{
  __block timepicker_handle* result = handle;
  __block std::function<void(int, int)> confirmCb = std::move(onConfirm);
  __block std::function<void()> resetCb = std::move(onReset);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new timepicker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSTimePickerCoordinator alloc] init];
    result->coordinator->onConfirm = std::move(confirmCb);
    result->coordinator->onReset = std::move(resetCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithHour:initialHour
                                  minute:initialMinute
                        anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideTimePicker(timepicker_handle* handle)
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

void destroyTimePicker(timepicker_handle* handle)
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

monthpicker_handle* showMonthPicker(int initialYear,
                                    int initialMonth,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int)> onChange,
                                    std::function<void(int, int)> onConfirm,
                                    std::function<void()> onReset,
                                    std::function<void()> onClosed)
{
  return reopenMonthPicker(nullptr,
                           initialYear,
                           initialMonth,
                           anchorScreenRect,
                           std::move(onChange),
                           std::move(onConfirm),
                           std::move(onReset),
                           std::move(onClosed));
}

monthpicker_handle* reopenMonthPicker(monthpicker_handle* handle,
                                      int initialYear,
                                      int initialMonth,
                                      const RECT& anchorScreenRect,
                                      std::function<void(int, int)> onChange,
                                      std::function<void(int, int)> onConfirm,
                                      std::function<void()> onReset,
                                      std::function<void()> onClosed)
{
  __block monthpicker_handle* result = handle;
  __block std::function<void(int, int)> changeCb = std::move(onChange);
  __block std::function<void(int, int)> confirmCb = std::move(onConfirm);
  __block std::function<void()> resetCb = std::move(onReset);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new monthpicker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSMonthPickerCoordinator alloc] init];
    result->coordinator->onChange = std::move(changeCb);
    result->coordinator->onConfirm = std::move(confirmCb);
    result->coordinator->onReset = std::move(resetCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithYear:initialYear
                                   month:initialMonth
                         anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideMonthPicker(monthpicker_handle* handle)
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

void destroyMonthPicker(monthpicker_handle* handle)
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

weekpicker_handle* showWeekPicker(int initialWeekYear,
                                  int initialWeek,
                                  const RECT& anchorScreenRect,
                                  std::function<void(int, int)> onChange,
                                  std::function<void(int, int)> onConfirm,
                                  std::function<void()> onReset,
                                  std::function<void()> onClosed)
{
  return reopenWeekPicker(nullptr,
                          initialWeekYear,
                          initialWeek,
                          anchorScreenRect,
                          std::move(onChange),
                          std::move(onConfirm),
                          std::move(onReset),
                          std::move(onClosed));
}

weekpicker_handle* reopenWeekPicker(weekpicker_handle* handle,
                                    int initialWeekYear,
                                    int initialWeek,
                                    const RECT& anchorScreenRect,
                                    std::function<void(int, int)> onChange,
                                    std::function<void(int, int)> onConfirm,
                                    std::function<void()> onReset,
                                    std::function<void()> onClosed)
{
  __block weekpicker_handle* result = handle;
  __block std::function<void(int, int)> changeCb = std::move(onChange);
  __block std::function<void(int, int)> confirmCb = std::move(onConfirm);
  __block std::function<void()> resetCb = std::move(onReset);
  __block std::function<void()> closedCb = std::move(onClosed);

  void (^run)(void) = ^{
    if (!result)
      result = new weekpicker_handle();
    if (!result->coordinator)
      result->coordinator = [[GlintIOSWeekPickerCoordinator alloc] init];
    result->coordinator->onChange = std::move(changeCb);
    result->coordinator->onConfirm = std::move(confirmCb);
    result->coordinator->onReset = std::move(resetCb);
    result->coordinator->onClosed = std::move(closedCb);
    [result->coordinator presentWithWeekYear:initialWeekYear
                                        week:initialWeek
                            anchorScreenRect:anchorScreenRect];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

void hideWeekPicker(weekpicker_handle* handle)
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

void destroyWeekPicker(weekpicker_handle* handle)
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

    if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad)
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

static NSArray<NSString*>* glint_document_picker_type_identifiers(const std::vector<std::string>& extensions,
                                                                  bool allowDirectories)
{
  static NSString* const kGlintUTIItem = @"public.item";
  static NSString* const kGlintUTIFolder = @"public.folder";
  static NSString* const kGlintUTIData = @"public.data";
  static NSString* const kGlintUTIImage = @"public.image";
  static NSString* const kGlintUTIAudio = @"public.audio";
  static NSString* const kGlintUTIMovie = @"public.movie";
  static NSString* const kGlintUTIPdf = @"com.adobe.pdf";
  static NSString* const kGlintUTIPlainText = @"public.plain-text";
  NSMutableArray<NSString*>* identifiers = [NSMutableArray array];
  NSMutableSet<NSString*>* seen = [NSMutableSet set];
  auto appendIdentifier = ^(NSString* identifier) {
    if (!identifier || identifier.length == 0 || [seen containsObject:identifier])
      return;
    [seen addObject:identifier];
    [identifiers addObject:identifier];
  };

  auto identifierForExtension = ^NSString* (std::string extension) {
    if (!extension.empty() && extension.front() == '.')
      extension.erase(extension.begin());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
        extension == "gif" || extension == "webp" || extension == "svg")
      return kGlintUTIImage;
    if (extension == "mp3" || extension == "wav" || extension == "m4a" || extension == "ogg")
      return kGlintUTIAudio;
    if (extension == "mp4" || extension == "mov" || extension == "m4v" || extension == "webm")
      return kGlintUTIMovie;
    if (extension == "pdf")
      return kGlintUTIPdf;
    if (extension == "txt")
      return kGlintUTIPlainText;
    return kGlintUTIData;
  };

  for (const auto& rawExtension : extensions)
  {
    appendIdentifier(identifierForExtension(rawExtension));
  }

  if (allowDirectories)
    appendIdentifier(kGlintUTIFolder);
  if (identifiers.count == 0)
    appendIdentifier(kGlintUTIItem);
  return identifiers;
}

enum class glint_ios_file_source_option {
  cancel = 0,
  photo_library = 1,
  camera = 2,
  files = 3,
};

static UIView* glint_file_picker_source_view(UIViewController* presenter, UIWindow* window)
{
  if (glint_last_interaction_view)
    return glint_last_interaction_view;
  return presenter.view ?: window;
}

static CGRect glint_file_picker_source_rect(UIView* sourceView)
{
  if (glint_last_interaction_view && sourceView == glint_last_interaction_view)
  {
    return CGRectMake(glint_last_interaction_point.x,
                      glint_last_interaction_point.y,
                      1.0f,
                      1.0f);
  }

  return glint_centered_source_rect(sourceView);
}

static NSArray<NSString*>* glint_media_picker_types(UIImagePickerControllerSourceType sourceType)
{
  NSArray<NSString*>* availableTypes = [UIImagePickerController availableMediaTypesForSourceType:sourceType] ?: @[];
  NSMutableArray<NSString*>* mediaTypes = [NSMutableArray array];
  if ([availableTypes containsObject:@"public.image"])
    [mediaTypes addObject:@"public.image"];
  if ([availableTypes containsObject:@"public.movie"])
    [mediaTypes addObject:@"public.movie"];
  if (!mediaTypes.count)
    [mediaTypes addObjectsFromArray:availableTypes];
  return mediaTypes;
}

static bool glint_extension_matches_any(const std::vector<std::string>& extensions,
                                        std::initializer_list<const char*> candidates)
{
  for (const auto& rawExtension : extensions)
  {
    std::string normalized = rawExtension;
    if (!normalized.empty() && normalized.front() == '.')
      normalized.erase(normalized.begin());
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    for (const char* candidate : candidates)
    {
      if (normalized == candidate)
        return true;
    }
  }
  return false;
}

static bool glint_allows_image_selection(const std::vector<std::string>& extensions)
{
  if (extensions.empty())
    return true;
  return glint_extension_matches_any(extensions, {"png", "jpg", "jpeg", "gif", "webp", "svg", "heic", "heif", "bmp", "tif", "tiff"});
}

static bool glint_allows_video_selection(const std::vector<std::string>& extensions)
{
  if (extensions.empty())
    return true;
  return glint_extension_matches_any(extensions, {"mp4", "mov", "m4v", "webm", "avi", "mpeg", "mpg"});
}

static NSString* glint_camera_action_label(bool allowsImage, bool allowsVideo)
{
  if (allowsImage && allowsVideo)
    return glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::file_input_take_photo_or_video));
  if (allowsVideo)
    return glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::file_input_take_video));
  return glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::file_input_take_photo));
}

static glint_ios_file_source_option glint_show_file_source_menu(const std::vector<std::string>& extensions)
{
  __block glint_ios_file_source_option result = glint_ios_file_source_option::cancel;

  void (^run)(void) = ^{
    UIWindow* window = glint_active_window();
    if (!window)
      return;

    UIViewController* presenter = glint_top_view_controller(window.rootViewController);
    if (!presenter)
      presenter = window.rootViewController;
    if (!presenter)
      return;

    UIView* sourceView = glint_file_picker_source_view(presenter, window);
    if (!sourceView)
      return;

    GlintIOSMenuTracker* tracker = [[GlintIOSMenuTracker alloc] init];
    tracker->selectedId = 0;
    tracker->finished = NO;

    const bool allowsImage = glint_allows_image_selection(extensions);
    const bool allowsVideo = glint_allows_video_selection(extensions);
    const bool allowsMedia = allowsImage || allowsVideo;

    NSMutableArray<GlintIOSMenuItem*>* nativeItems = [NSMutableArray arrayWithCapacity:3];
    if (allowsMedia)
    {
      [nativeItems addObject:[GlintIOSMenuItem itemWithId:(int)glint_ios_file_source_option::photo_library
                                                    title:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::file_input_photo_library))
                                          systemImageName:@"photo.on.rectangle.angled"
                                                  enabled:YES
                                                  checked:NO
                                                separator:NO]];
      [nativeItems addObject:[GlintIOSMenuItem itemWithId:(int)glint_ios_file_source_option::camera
                                                    title:glint_camera_action_label(allowsImage, allowsVideo)
                                          systemImageName:@"camera"
                                                  enabled:[UIImagePickerController isSourceTypeAvailable:UIImagePickerControllerSourceTypeCamera]
                                                  checked:NO
                                                separator:NO]];
    }
    [nativeItems addObject:[GlintIOSMenuItem itemWithId:(int)glint_ios_file_source_option::files
                                                  title:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::file_input_choose_file))
                                        systemImageName:@"folder"
                                                enabled:YES
                                                checked:NO
                                              separator:NO]];

    UIView* anchorView = glint_last_interaction_view ? glint_last_interaction_view : sourceView;
    CGPoint anchorPoint = glint_last_interaction_view
      ? glint_last_interaction_point
      : CGPointMake(CGRectGetMidX(anchorView.bounds), CGRectGetMidY(anchorView.bounds));

    if (@available(iOS 17.4, *))
    {
      GlintIOSSelectPickerCoordinator* coordinator = [[GlintIOSSelectPickerCoordinator alloc]
        initWithItems:nativeItems
           selectedId:0
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
      UIAlertController* alert = [UIAlertController alertControllerWithTitle:nil
                                                                     message:nil
                                                              preferredStyle:UIAlertControllerStyleActionSheet];

      for (GlintIOSMenuItem* item in nativeItems)
      {
        UIAlertAction* action = [UIAlertAction actionWithTitle:item->title
                                                         style:UIAlertActionStyleDefault
                                                       handler:^(__unused UIAlertAction* selectedAction) {
          tracker->selectedId = item->itemId;
          tracker->finished = YES;
        }];
        action.enabled = item->enabled;
        [alert addAction:action];
      }

      UIAlertAction* cancelAction = [UIAlertAction actionWithTitle:glint_nsstring_from_utf8(glint_i18n::localized(glint_i18n_key::common_cancel))
                                                             style:UIAlertActionStyleCancel
                                                           handler:^(__unused UIAlertAction* action) {
        tracker->finished = YES;
      }];
      [alert addAction:cancelAction];

      if (alert.presentationController)
        alert.presentationController.delegate = tracker;

      if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad)
      {
        UIPopoverPresentationController* popover = alert.popoverPresentationController;
        popover.sourceView = sourceView;
        popover.sourceRect = glint_file_picker_source_rect(sourceView);
        popover.permittedArrowDirections = UIPopoverArrowDirectionAny;
      }

      [presenter presentViewController:alert animated:YES completion:nil];

      while (!tracker->finished)
      {
        @autoreleasepool
        {
          NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
          [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
          [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
        }
      }
    }

    result = static_cast<glint_ios_file_source_option>(tracker->selectedId);
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

static std::vector<std::string> glint_run_media_picker(UIImagePickerControllerSourceType sourceType)
{
  __block std::vector<std::string> result;

  void (^run)(void) = ^{
    if (![UIImagePickerController isSourceTypeAvailable:sourceType])
      return;

    UIWindow* window = glint_active_window();
    if (!window)
      return;

    UIViewController* presenter = glint_top_view_controller(window.rootViewController);
    if (!presenter)
      presenter = window.rootViewController;
    if (!presenter)
      return;

    GlintIOSMediaPickerTracker* tracker = [[GlintIOSMediaPickerTracker alloc] init];
    UIImagePickerController* picker = [[UIImagePickerController alloc] init];
    picker.delegate = tracker;
    picker.sourceType = sourceType;
    picker.mediaTypes = glint_media_picker_types(sourceType);
    if (picker.presentationController)
      picker.presentationController.delegate = tracker;

    [presenter presentViewController:picker animated:YES completion:nil];

    while (!tracker->finished)
    {
      @autoreleasepool
      {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
        [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
      }
    }

    for (NSString* path in tracker->selectedPaths)
      result.push_back(glint_utf8_from_nsstring(path));

    [picker release];
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

static std::vector<std::string> glint_run_photo_picker(const std::vector<std::string>& extensions,
                                                       bool allowsMultipleSelection)
{
  __block std::vector<std::string> result;

  void (^run)(void) = ^{
    UIWindow* window = glint_active_window();
    if (!window)
      return;

    UIViewController* presenter = glint_top_view_controller(window.rootViewController);
    if (!presenter)
      presenter = window.rootViewController;
    if (!presenter)
      return;

    const bool allowsImage = glint_allows_image_selection(extensions);
    const bool allowsVideo = glint_allows_video_selection(extensions);

    GlintIOSMediaPickerTracker* tracker = [[GlintIOSMediaPickerTracker alloc] init];
    PHPickerConfiguration* configuration = [[PHPickerConfiguration alloc] init];
    configuration.selectionLimit = allowsMultipleSelection ? 0 : 1;
    configuration.preferredAssetRepresentationMode = PHPickerConfigurationAssetRepresentationModeCompatible;
    if (allowsImage && !allowsVideo)
      configuration.filter = PHPickerFilter.imagesFilter;
    else if (allowsVideo && !allowsImage)
      configuration.filter = PHPickerFilter.videosFilter;

    PHPickerViewController* picker = [[PHPickerViewController alloc] initWithConfiguration:configuration];
    picker.delegate = tracker;
    if (picker.presentationController)
      picker.presentationController.delegate = tracker;

    [presenter presentViewController:picker animated:YES completion:nil];

    while (!tracker->finished)
    {
      @autoreleasepool
      {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
        [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
      }
    }

    for (NSString* path in tracker->selectedPaths)
      result.push_back(glint_utf8_from_nsstring(path));

    [picker release];
    [configuration release];
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

static std::vector<std::string> glint_run_open_document_picker(const std::vector<std::string>& extensions,
                                                               const std::string& title,
                                                               bool allowDirectories,
                                                               bool allowsMultipleSelection)
{
  __block std::vector<std::string> result;

  void (^run)(void) = ^{
    UIWindow* window = glint_active_window();
    if (!window)
      return;

    UIViewController* presenter = glint_top_view_controller(window.rootViewController);
    if (!presenter)
      presenter = window.rootViewController;
    if (!presenter)
      return;

    GlintIOSFilePickerTracker* tracker = [[GlintIOSFilePickerTracker alloc] init];
    NSArray<NSString*>* typeIdentifiers = glint_document_picker_type_identifiers(extensions, allowDirectories);
    UIDocumentPickerViewController* picker = nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    picker = [[UIDocumentPickerViewController alloc] initWithDocumentTypes:typeIdentifiers
                                                                    inMode:UIDocumentPickerModeImport];
#pragma clang diagnostic pop

    picker.delegate = tracker;
    picker.title = title.empty() ? nil : glint_nsstring_from_utf8(title);
    if ([picker respondsToSelector:@selector(setAllowsMultipleSelection:)])
      picker.allowsMultipleSelection = allowsMultipleSelection;
    if (picker.presentationController)
      picker.presentationController.delegate = tracker;

    [presenter presentViewController:picker animated:YES completion:nil];

    while (!tracker->finished)
    {
      @autoreleasepool
      {
        NSDate* until = [NSDate dateWithTimeIntervalSinceNow:0.01];
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:until];
        [[NSRunLoop mainRunLoop] runMode:UITrackingRunLoopMode beforeDate:until];
      }
    }

    for (NSString* path in tracker->selectedPaths)
      result.push_back(glint_utf8_from_nsstring(path));

    [picker release];
    [tracker release];
  };

  if ([NSThread isMainThread])
    run();
  else
    dispatch_sync(dispatch_get_main_queue(), run);

  return result;
}

std::string showOpenFileDialog(const std::vector<std::string>& extensions,
                               const std::string& title,
                               bool allowDirectories)
{
  std::vector<std::string> results;
  if (allowDirectories)
  {
    results = glint_run_open_document_picker(extensions, title, true, false);
  }
  else
  {
    switch (glint_show_file_source_menu(extensions))
    {
      case glint_ios_file_source_option::photo_library:
        results = glint_run_photo_picker(extensions, false);
        break;
      case glint_ios_file_source_option::camera:
        results = glint_run_media_picker(UIImagePickerControllerSourceTypeCamera);
        break;
      case glint_ios_file_source_option::files:
        results = glint_run_open_document_picker(extensions, title, false, false);
        break;
      default:
        break;
    }
  }
  return results.empty() ? std::string{} : results.front();
}

std::vector<std::string> showOpenFilesDialog(const std::vector<std::string>& extensions,
                                             const std::string& title,
                                             bool allowDirectories)
{
  if (allowDirectories)
    return glint_run_open_document_picker(extensions, title, true, true);

  switch (glint_show_file_source_menu(extensions))
  {
    case glint_ios_file_source_option::photo_library:
      return glint_run_photo_picker(extensions, true);
    case glint_ios_file_source_option::camera:
      return glint_run_media_picker(UIImagePickerControllerSourceTypeCamera);
    case glint_ios_file_source_option::files:
      return glint_run_open_document_picker(extensions, title, false, true);
    default:
      return {};
  }
}

std::string showSaveFileDialog(const std::vector<std::string>&,
                               const std::string&,
                               const std::string&,
                               const std::string&)
{
  return {};
}

std::string showOpenFolderDialog(const std::string& title)
{
  const auto results = glint_run_open_document_picker({}, title, true, false);
  return results.empty() ? std::string{} : results.front();
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