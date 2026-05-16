/**
 * glint_cursor_mac.mm
 * GlintCoreCursor ObjC implementation — compiled exactly once into the library.
 *
 * The _coreCursorType property override tells macOS CoreCursor which private
 * system cursor glyph to use, matching Chrome's CrCoreCursor technique:
 *   https://source.chromium.org/chromium/chromium/src/+/main:ui/base/cocoa/cursor_utils.mm
 */

#import "glint_cursor_mac.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace {

using GlintCustomCursorRegistry = std::unordered_map<std::string, NSCursor*>;

GlintCustomCursorRegistry& glint_custom_cursor_registry()
{
    static GlintCustomCursorRegistry registry;
    return registry;
}

void glint_release_registered_cursor(const std::string& token)
{
    auto& registry = glint_custom_cursor_registry();
    const auto it = registry.find(token);
    if (it == registry.end())
        return;

    [it->second release];
    registry.erase(it);
}

}  // namespace

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

namespace glint_mac_cursor {

bool registerCustomCursorRGBA(const std::string& token,
                              int widthPx,
                              int heightPx,
                              const std::uint8_t* rgbaBytes,
                              int hotspotXPx,
                              int hotspotYPx,
                              float backingScale)
{
    if (token.empty())
        return false;

    glint_release_registered_cursor(token);

    if (!rgbaBytes || widthPx <= 0 || heightPx <= 0)
        return false;

    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:nil
                      pixelsWide:widthPx
                      pixelsHigh:heightPx
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:widthPx * 4
                    bitsPerPixel:32];
    if (!rep)
        return false;

    unsigned char* bitmapData = [rep bitmapData];
    if (!bitmapData) {
        [rep release];
        return false;
    }

    std::memcpy(bitmapData, rgbaBytes,
                static_cast<std::size_t>(widthPx) * static_cast<std::size_t>(heightPx) * 4u);

    const CGFloat scale = backingScale > 0.f ? static_cast<CGFloat>(backingScale) : 1.f;
    const CGFloat pixelStep = 1.0 / scale;
    const CGFloat widthPt = static_cast<CGFloat>(widthPx) / scale;
    const CGFloat heightPt = static_cast<CGFloat>(heightPx) / scale;
    const CGFloat maxHotX = widthPt > pixelStep ? widthPt - pixelStep : 0.0;
    const CGFloat maxHotY = heightPt > pixelStep ? heightPt - pixelStep : 0.0;
    const CGFloat hotX = std::clamp(static_cast<CGFloat>(hotspotXPx) / scale, 0.0, maxHotX);
    const CGFloat hotY = std::clamp(static_cast<CGFloat>(hotspotYPx) / scale, 0.0, maxHotY);

    NSImage* image = [[NSImage alloc] initWithSize:NSMakeSize(widthPt, heightPt)];
    [image addRepresentation:rep];

    NSCursor* cursor = [[NSCursor alloc] initWithImage:image
                                               hotSpot:NSMakePoint(hotX, hotY)];

    [image release];
    [rep release];

    if (!cursor)
        return false;

    glint_custom_cursor_registry().emplace(token, cursor);
    return true;
}

void unregisterCustomCursor(const std::string& token)
{
    if (token.empty())
        return;

    glint_release_registered_cursor(token);
}

void* findCustomCursor(const std::string& token)
{
    auto& registry = glint_custom_cursor_registry();
    const auto it = registry.find(token);
    return it == registry.end() ? nullptr : it->second;
}

}  // namespace glint_mac_cursor
