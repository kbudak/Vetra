#include "dock.h"
#include <dwmapi.h>
#include <chrono>

namespace {
using namespace std::chrono_literals;

constexpr float CardWidth = 216.f, CardHeight = 158.f, CardGap = 8.f;
constexpr float SidePadding = 16.f, CardTop = 14.f, BottomPadding = 16.f;
constexpr float NavigationHeight = 31.f;
constexpr int MaxVisibleCards = 4;

int visibleCards(float panelWidth) {
    return std::clamp(static_cast<int>((panelWidth - 2 * SidePadding + CardGap + .1f) /
        (CardWidth + CardGap)), 1, MaxVisibleCards);
}

std::shared_ptr<Gdiplus::Bitmap> captureWindowPreview(HWND window) {
    RECT bounds{};
    if (!IsWindow(window) || IsIconic(window) || IsHungAppWindow(window) || !GetWindowRect(window, &bounds)) return {};
    const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
    if (w < 32 || h < 32 || w > 8192 || h > 8192) return {};

    RECT frame{}, source = bounds;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) {
        RECT intersection{};
        if (IntersectRect(&intersection, &bounds, &frame)) source = intersection;
    }
    OffsetRect(&source, -bounds.left, -bounds.top);
    const int sourceW = source.right - source.left, sourceH = source.bottom - source.top;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits{};
    const HBITMAP buffer = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!buffer) return {};
    std::fill_n(static_cast<DWORD*>(bits), static_cast<size_t>(w) * h, 0);
    const HDC dc = CreateCompatibleDC(nullptr);
    const auto old = dc ? SelectObject(dc, buffer) : nullptr;
    constexpr UINT renderFullContent = 2;
    const bool captured = old && PrintWindow(window, dc, renderFullContent);
    GdiFlush();
    if (old) SelectObject(dc, old);
    if (dc) DeleteDC(dc);

    std::shared_ptr<Gdiplus::Bitmap> preview;
    if (captured) {
        // An occluded browser can return a flat placeholder below its toolbar.
        const auto* pixels = static_cast<const DWORD*>(bits);
        const auto color = [&](int x, int y) { return pixels[static_cast<size_t>(y) * w + x] & 0x00FFFFFF; };
        const int top = source.top + sourceH / 4, left = source.left + sourceW / 16;
        const auto first = color(left, top);
        bool rendered = false;
        for (int y = top; y < source.bottom - sourceH / 16 && !rendered; y += std::max(1, sourceH / 24))
            for (int x = left; x < source.right - sourceW / 16 && !rendered; x += std::max(1, sourceW / 24))
                rendered = color(x, y) != first;
        if (rendered) {
            using namespace Gdiplus;
            Bitmap capture(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
            const float scale = std::min({1.f, 384.f / sourceW, 216.f / sourceH});
            const int thumbW = std::max(1, static_cast<int>(sourceW * scale));
            const int thumbH = std::max(1, static_cast<int>(sourceH * scale));
            auto image = std::make_shared<Bitmap>(thumbW, thumbH, PixelFormat32bppPARGB);
            Graphics graphics(image.get());
            graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
            if (graphics.DrawImage(&capture, Rect(0, 0, thumbW, thumbH), source.left, source.top,
                sourceW, sourceH, UnitPixel) == Ok) preview = std::move(image);
        }
    }
    DeleteObject(buffer);
    return preview;
}
}

bool Dock::syncChromeWindows(bool renew) {
    std::vector<WindowPreview> current;
    for (const auto& item : extra.windows) {
        if (!isChromeApp(item.app) || !IsWindow(item.handle)) continue;
        const bool minimized = IsIconic(item.handle) != FALSE;
        const auto cached = std::find_if(chromeWindows.begin(), chromeWindows.end(), [&](const WindowPreview& entry) {
            return entry.window == item.handle && entry.title == item.title;
        });
        const bool hasCache = cached != chromeWindows.end();
        current.push_back({item.handle, item.title, item.active, minimized,
            !renew && hasCache && cached->minimized == minimized && cached->requested,
            item.icon, hasCache ? cached->preview : nullptr});
    }
    const bool changed = !std::equal(current.begin(), current.end(), chromeWindows.begin(), chromeWindows.end(),
        [](const WindowPreview& a, const WindowPreview& b) {
            return a.window == b.window && a.title == b.title && a.active == b.active &&
                a.minimized == b.minimized && a.icon == b.icon;
        });
    if (renew || changed) chromeWindows = std::move(current);
    return changed;
}

void Dock::showChromeWindows(float x, bool hover) {
    if (panel && page == ChromeWindows && !sheetClosing) {
        if (!hover && chromeHoverPanel) {
            chromeHoverPanel = false;
            SetForegroundWindow(panel);
        } else if (!hover) {
            chromeIconHovered = true;
            chromeHoverSince = 0;
            close();
        }
        return;
    }
    syncChromeWindows(true);
    if (chromeWindows.empty()) return;
    if (sheetClosing) closeSheet();
    chromeHoverPanel = hover;
    chromeLeftAt = 0;
    open(ChromeWindows, x, !hover);
    updateChromePreviews();
}

void Dock::trackChromeHover() {
    if (!chromeHoverTracking && SetTimer(hwnd, ChromeHoverTick, 25, nullptr))
        chromeHoverTracking = true;
}

void Dock::updateChromeHover() {
    constexpr ULONGLONG openDelay = 175, closeDelay = 240;
    const auto stamp = GetTickCount64();
    POINT cursor{};
    GetCursorPos(&cursor);
    const HWND hovered = WindowFromPoint(cursor);
    bool overIcon = false;
    float anchor = 0;
    if (hovered == bar.hwnd && !hidden && !dockEditMode && !dockDragging && !dockOrderSource && !modal) {
        ScreenToClient(bar.hwnd, &cursor);
        const int index = bar.at(cursor.x, cursor.y);
        if (index >= 0) {
            const auto& hit = bar.hits[index];
            overIcon = hit.chromePreview;
            anchor = (hit.bounds.left + hit.bounds.right) / 2;
        }
    }
    if (overIcon != chromeIconHovered) {
        chromeIconHovered = overIcon;
        chromeHoverSince = overIcon ? stamp : 0;
    }
    const bool showing = panel && page == ChromeWindows && chromeHoverPanel && !sheetClosing;
    if (!panel && overIcon && chromeHoverSince && stamp - chromeHoverSince >= openDelay) {
        chromeHoverSince = 0;
        showChromeWindows(anchor, true);
        return;
    }
    if (showing) {
        if (overIcon || hovered == panel) chromeLeftAt = 0;
        else if (!chromeLeftAt) chromeLeftAt = stamp;
        else if (stamp - chromeLeftAt >= closeDelay) close();
    } else if (!overIcon || (panel && page != ChromeWindows)) {
        chromeIconHovered = false;
        chromeHoverSince = 0;
        KillTimer(hwnd, ChromeHoverTick);
        chromeHoverTracking = false;
    }
}

void Dock::refreshChromeWindows() {
    const int state = refreshOpenWindows();
    if (syncChromeWindows(true)) sheet.hover = sheet.key = -1;
    if (chromeWindows.empty()) { close(); return; }
    if (state == 2) position();
    else positionSheet();
    updateChromePreviews();
}

void Dock::updateChromePreviews() {
    bool painted = false;
    if (chromePreviewRead.valid() && chromePreviewRead.wait_for(0ms) == std::future_status::ready) {
        for (auto& capture : chromePreviewRead.get()) {
            if (!capture.preview) continue;
            const auto current = std::find_if(chromeWindows.begin(), chromeWindows.end(), [&](const WindowPreview& entry) {
                return entry.window == capture.window && entry.title == capture.title;
            });
            if (current != chromeWindows.end()) {
                current->preview = std::move(capture.preview);
                painted = true;
            }
        }
    }
    if (panel && page == ChromeWindows && !sheetClosing) {
        if (painted) paintSheet();
        if (!chromePreviewRead.valid()) {
            std::vector<WindowPreview> targets;
            const int end = std::min(static_cast<int>(chromeWindows.size()), listOffset + visibleCards(sheet.w));
            for (int i = listOffset; i < end; ++i) {
                auto& card = chromeWindows[i];
                if (card.requested || card.minimized) continue;
                card.requested = true;
                targets.push_back({card.window, card.title});
            }
            if (!targets.empty()) {
                chromePreviewRead = std::async(std::launch::async, [targets = std::move(targets)]() mutable {
                    for (auto& card : targets) card.preview = captureWindowPreview(card.window);
                    return targets;
                });
            }
        }
    }
    if (chromePreviewRead.valid()) SetTimer(hwnd, ChromePreviewTick, 35, nullptr);
    else KillTimer(hwnd, ChromePreviewTick);
}

void Dock::sizeChromeWindows(float availableWidth) {
    const int count = static_cast<int>(chromeWindows.size());
    const int visible = std::min(count, visibleCards(availableWidth));
    sheet.w = visible ? (2 * SidePadding + visible * CardWidth + (visible - 1) * CardGap) : 292.f;
    sheet.h = visible ? (CardTop + CardHeight + BottomPadding + (count > visible ? NavigationHeight : 0.f)) : 62.f;
}

void Dock::scrollChromeWindows(int delta) {
    const int last = std::max(0, static_cast<int>(chromeWindows.size()) - visibleCards(sheet.w));
    const int next = std::clamp(listOffset + delta, 0, last);
    if (next == listOffset) return;
    listOffset = next;
    sheet.hover = sheet.key = -1;
    paintSheet();
    updateChromePreviews();
}

void Dock::chromeWindowsPanel(Palette p) {
    auto& d = sheet.d;
    const int count = static_cast<int>(chromeWindows.size());
    if (!count) return;
    const int visible = visibleCards(sheet.w);
    listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
    const int end = std::min(count, listOffset + visible);
    for (int i = listOffset; i < end; ++i) {
        const auto& item = chromeWindows[i];
        const float x = SidePadding + (CardWidth + CardGap) * (i - listOffset);
        const auto card = box(x, CardTop, CardWidth, CardHeight);
        hit(sheet, card, [this, target = item.window] {
            close();
            activateWindow(target);
        });
        const float hot = sheet.hoverAmount();
        const float down = sheet.pressAmount();
        D2D1::Matrix3x2F cp{};
        const bool xform = hot > 0.01f || down > 0.01f;
        if (xform) {
            d.target->GetTransform(&cp);
            const float s = 1.f + 0.012f * hot - 0.025f * down;
            const float lift = -2.f * hot;
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {x + CardWidth / 2.f, CardTop + CardHeight / 2.f}) *
                D2D1::Matrix3x2F::Translation(0.f, lift) * cp);
        }
        if (p.glass) {
            p.card(d, card, 13);
            if (hot > 0) d.fill(card, 13, tint(p.light ? 0x10141A : 0xFFFFFF, .05f * hot));
            if (item.active || hot > 0) d.glassRim(card, 13, item.active ? .48f : (.28f + .16f * hot));
        } else {
            d.fill(card, 12, tint(p.high));
            if (item.active || hot > 0) d.outline(box(x + .5f, CardTop + .5f, CardWidth - 1, CardHeight - 1),
                11.5f, tint(p.accent, item.active ? std::max(.40f, .75f * hot) : .75f * hot));
        }

        const auto icon = d.snap(box(x + 11, CardTop + 10, 17, 17));
        if (item.icon) d.image(item.icon, icon);
        else d.icon(Globe, icon, tint(p.muted));
        d.text(item.title, box(x + 36, CardTop + 6, CardWidth - 46, 26), Caption, tint(p.ink));

        const auto frame = box(x + 10, CardTop + 37, CardWidth - 20, CardHeight - 47);
        d.fill(frame, 6, tint(p.base));
        if (item.preview) {
            const float w = static_cast<float>(item.preview->GetWidth());
            const float h = static_cast<float>(item.preview->GetHeight());
            const float scale = std::min((width(frame) - 6) / w, (height(frame) - 6) / h);
            d.image(item.preview, box(frame.left + (width(frame) - w * scale) / 2,
                frame.top + (height(frame) - h * scale) / 2, w * scale, h * scale));
        } else {
            d.icon(ScreenIcon, box(frame.left + (width(frame) - 28) / 2,
                frame.top + (height(frame) - 28) / 2, 28, 28), tint(p.muted, .70f));
        }
        if (xform) d.target->SetTransform(cp);
    }
    if (count > visible) {
        const float y = CardTop + CardHeight + 3.f, mid = sheet.w / 2;
        d.text(num(listOffset + 1) + L"–" + num(end) + L" / " + num(count),
            box(mid - 51, y, 102, 26), Small, tint(p.muted), Center);
        if (listOffset > 0) iconButton(sheet, p, box(mid - 77, y, 26, 26), ChevronLeft,
            [this, visible] { scrollChromeWindows(-visible); });
        if (end < count) iconButton(sheet, p, box(mid + 51, y, 26, 26), ChevronRight,
            [this, visible] { scrollChromeWindows(visible); });
    }
}
