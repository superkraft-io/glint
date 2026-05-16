#pragma once

#include <cstdint>
#include <string>

namespace glint_mac_cursor {

bool registerCustomCursorRGBA(const std::string& token,
                              int widthPx,
                              int heightPx,
                              const std::uint8_t* rgbaBytes,
                              int hotspotXPx,
                              int hotspotYPx,
                              float backingScale = 1.f);

void unregisterCustomCursor(const std::string& token);

void* findCustomCursor(const std::string& token);

}  // namespace glint_mac_cursor