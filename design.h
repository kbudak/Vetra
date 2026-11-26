#pragma once
#include <algorithm>
#include <cmath>

namespace Design {
inline constexpr unsigned Green = 0x2BCB71, Blue = 0x087FF1, Red = 0xFF4740, Amber = 0xFF9F0A;
inline constexpr unsigned focusRed(bool light) { return light ? 0xC62C36 : 0xFF545A; }
inline constexpr float CardHeight = 56, Radius = 17, Gap = 6, Pad = 10, Icon = 36;
inline constexpr float DockInset = 5, DockLeading = 54, DockHeight = CardHeight + 2 * DockInset;
inline constexpr float DockEdgeTravel = 26.f, DockRevealMs = 200.f, DockHideMs = 150.f;
inline constexpr float SheetOpenMs = 175.f, SheetCloseMs = 115.f, SheetSlidePx = 12.f, ContentAnimMs = 185.f;
inline constexpr float TrayTileSize = 34.f, TrayTileGap = 4.f, TrayGridPadX = 16.f, TrayGridPadY = 14.f;
inline constexpr float AppPitch = 39, GlassAppPitch = 56, AppSize = 32;
inline constexpr float StudioWidth = 1000, StudioHeight = 678;
inline constexpr float QuickVolumeX = 77, QuickVolumeWidth = 251, QuickVolumeTop = 273, QuickVolumeHeight = 32;
inline constexpr float Small = 11, Caption = 12, Body = 13, Strong = 14, Value = 20;

struct TrayGridLayout {
    int cols = 5;
    int totalRows = 1;
    int visibleRows = 1;
    int maxOffset = 0;
    float width = 210.f;
    float height = 92.f;
};

inline TrayGridLayout computeTrayGrid(int count, float maxSheetW, float maxSheetH) {
    TrayGridLayout g{};
    if (count <= 0) return g;
    constexpr float tile = TrayTileSize, gap = TrayTileGap, padX = TrayGridPadX, footerH = 52.f;
    const float usableW = std::max(210.f, maxSheetW);
    const float usableH = std::max(220.f, maxSheetH);
    const int maxColsFit = std::clamp(
        static_cast<int>(std::floor((usableW - 2.f * padX + gap) / (tile + gap))), 5, 12);
    const int maxRowsFit = std::max(4,
        static_cast<int>(std::floor((usableH - footerH + gap) / (tile + gap))));

    int cols = 5;
    if (count <= 5) cols = count;
    else if (count <= 10) cols = 5;
    else if (count <= 18) cols = std::min(6, maxColsFit);
    else if (count <= 28) cols = std::min(7, maxColsFit);
    else if (count <= 40) cols = std::min(8, maxColsFit);
    else {
        const int target = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count) * 1.45)));
        cols = std::clamp(target, 8, maxColsFit);
    }
    const int totalRows = (count + cols - 1) / cols;
    const int visibleRows = std::min(totalRows, maxRowsFit);
    const float gridW = cols * tile + (cols - 1) * gap;
    const float gridH = visibleRows * tile + (visibleRows - 1) * gap;
    g.cols = cols;
    g.totalRows = totalRows;
    g.visibleRows = visibleRows;
    g.maxOffset = std::max(0, totalRows - visibleRows);
    g.width = std::max(168.f, gridW + 2.f * padX);
    g.height = gridH + footerH;
    return g;
}
}
