// liteui_theme.hpp
#pragma once
#include "liteui.hpp"

namespace liteui_theme {

// surfaces
inline constexpr Color kEditorBg{30, 30, 30, 255};
inline constexpr Color kSideBarBg{37, 37, 38, 255};
inline constexpr Color kTabBarBg{37, 37, 38, 255};
inline constexpr Color kTabInactiveBg{45, 45, 45, 255};
inline constexpr Color kMenuBarBg{60, 60, 60, 255};
inline constexpr Color kMenuBarOpenBg{95, 95, 95, 255};
inline constexpr Color kStatusBarBg{0, 122, 204, 255};
inline constexpr Color kMenuBg{37, 37, 38, 255};
inline constexpr Color kMenuBorder{69, 69, 69, 255};
inline constexpr Color kInputBg{60, 60, 60, 255};
inline constexpr Color kButtonBg{60, 60, 60, 255};
inline constexpr Color kButtonHoverBg{78, 78, 78, 255};
inline constexpr Color kDivider{60, 60, 60, 255};

// interaction states
inline constexpr Color kHoverBg{42, 45, 46, 255};
inline constexpr Color kMenuHoverBg{9, 71, 113, 255};
inline constexpr Color kSelectedBg{4, 57, 94, 255};
inline constexpr Color kActiveItemBg{55, 55, 61, 255};
inline constexpr Color kTabHoverBg{55, 55, 55, 255};
inline constexpr Color kAccent{0, 122, 204, 255};
inline constexpr Color kDanger{0xC0, 0x39, 0x2B, 255};
inline constexpr Color kDangerHover{0xA8, 0x2F, 0x23, 255};

// text
inline constexpr Color kText{204, 204, 204, 255};
inline constexpr Color kTextMuted{157, 157, 157, 255};
inline constexpr Color kTextDim{128, 128, 128, 255};
inline constexpr Color kTextBright{255, 255, 255, 255};
inline constexpr Color kTextInactive{150, 150, 150, 255};
inline constexpr Color kLink{55, 148, 255, 255};
inline constexpr Color kOnAccent{255, 255, 255, 255};
inline constexpr Color kOnAccentDim{190, 220, 245, 255};

// editor text
inline constexpr Color kEditorText{212, 212, 212, 255};
inline constexpr Color kPlaceholder{130, 130, 130, 255};
inline constexpr Color kCaret{174, 175, 173, 255};

// misc
inline constexpr Color kTransparent{0, 0, 0, 0};
inline constexpr Color kOverlay{0, 0, 0, 120};    // modal dimmer
inline constexpr Color kOverlayClear{0, 0, 0, 2}; // click-away catcher
inline constexpr Color kScrollTrack{37, 37, 38, 255};
inline constexpr Color kScrollThumb{90, 90, 90, 255};

} // namespace liteui_theme