/**
 * glint_cursor_mac.mm
 * GlintCoreCursor ObjC implementation — compiled exactly once into the library.
 *
 * The _coreCursorType property override tells macOS CoreCursor which private
 * system cursor glyph to use, matching Chrome's CrCoreCursor technique:
 *   https://source.chromium.org/chromium/chromium/src/+/main:ui/base/cocoa/cursor_utils.mm
 */

#import "glint_cursor_mac.h"

@implementation GlintCoreCursor {
    GlintCoreCursorType _type;
}

@synthesize _coreCursorType = _type;

+ (instancetype)cursorWithType:(GlintCoreCursorType)type
{
    return [[[GlintCoreCursor alloc] initWithType:type] autorelease];
}

- (id)initWithType:(GlintCoreCursorType)type
{
    if ((self = [super init])) {
        _type = type;
    }
    return self;
}

@end
