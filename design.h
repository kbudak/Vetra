#pragma once

namespace Design {
inline constexpr unsigned Green = 0x2BCB71, Blue = 0x087FF1, Red = 0xFF4740, Amber = 0xFF9F0A;
inline constexpr unsigned focusRed(bool light) { return light ? 0xC62C36 : 0xFF545A; }
inline constexpr float CardHeight = 56, Radius = 17, Gap = 6, Pad = 10, Icon = 36;
inline constexpr float DockInset = 5, DockLeading = 54, DockHeight = CardHeight + 2 * DockInset, TrayWidth = 42;
inline constexpr float StatusBarGap = 5, DesktopStripGap = 5, DesktopStripWidth = 18;
inline constexpr float DockTrailing = TrayWidth + DesktopStripGap + DesktopStripWidth;
inline constexpr float DockEdgeTravel = 26.f, DockRevealMs = 200.f, DockHideMs = 150.f;
inline constexpr float SheetOpenMs = 175.f, SheetCloseMs = 115.f, SheetSlidePx = 12.f, ContentAnimMs = 185.f;
inline constexpr float TrayTileSize = 34.f, TrayTileGap = 4.f, TrayGridPadX = 16.f, TrayGridPadY = 14.f;
inline constexpr float AppPitch = 39, GlassAppPitch = 56, AppSize = 32;
inline constexpr float StudioWidth = 1000, StudioHeight = 678;
inline constexpr float QuickVolumeX = 77, QuickVolumeWidth = 251, QuickVolumeTop = 273, QuickVolumeHeight = 32;
inline constexpr float Small = 11, Caption = 12, Body = 13, Strong = 14, Value = 20;
}
