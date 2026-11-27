#include "dock.h"
#include <dwmapi.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace {
using namespace std::chrono_literals;

constexpr float CardWidth = 216.f, CardHeight = 158.f, CardGap = 8.f;
constexpr float SidePadding = 16.f, CardTop = 14.f, BottomPadding = 16.f;
constexpr float NavigationHeight = 31.f;
constexpr int MaxVisibleCards = 4;
constexpr wchar_t ThumbHostClass[] = L"VetraDwmThumbHost";

std::mutex gPreviewCacheMutex;
std::unordered_map<HWND, std::shared_ptr<Gdiplus::Bitmap>> gPreviewCache;

struct PreviewOverlaySpec {
    HWND source{};
    Box frameDip{};
};

struct DwmThumbSlot {
    HWND host{};
    HWND owner{};
    HWND source{};
    HTHUMBNAIL thumb{};
    RECT lastRect{};
    BYTE lastAlpha = 0;
    bool visible = false;
    bool dwmOk = false;
};

std::vector<PreviewOverlaySpec> gOverlaySpecs;
std::vector<DwmThumbSlot> gThumbSlots;

LRESULT CALLBACK thumbHostProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(w, &ps);
        EndPaint(w, &ps);
        return 0;
    }
    default: return DefWindowProcW(w, msg, wp, lp);
    }
}

void ensureThumbHostClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = thumbHostProc;
    wc.lpszClassName = ThumbHostClass;
    if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) registered = true;
}

bool isPreviewOverlayWindow(HWND hwnd) {
    if (!hwnd) return false;
    for (const auto& slot : gThumbSlots) {
        if (slot.host == hwnd) return true;
    }
    return false;
}

bool hasActiveDwmThumb(HWND source) {
    if (!source) return false;
    for (const auto& slot : gThumbSlots) {
        if (slot.source == source && slot.dwmOk && slot.thumb) return true;
    }
    return false;
}

void destroyThumbSlot(DwmThumbSlot& slot) {
    if (slot.thumb) {
        DwmUnregisterThumbnail(slot.thumb);
        slot.thumb = nullptr;
    }
    if (slot.host) {
        DestroyWindow(slot.host);
        slot.host = nullptr;
    }
    slot.owner = nullptr;
    slot.source = nullptr;
    slot.lastRect = {};
    slot.lastAlpha = 0;
    slot.visible = false;
    slot.dwmOk = false;
}

std::shared_ptr<Gdiplus::Bitmap> lookupPreviewCache(HWND window) {
    std::lock_guard lock(gPreviewCacheMutex);
    const auto it = gPreviewCache.find(window);
    return it != gPreviewCache.end() ? it->second : std::shared_ptr<Gdiplus::Bitmap>{};
}

void storePreviewCache(HWND window, const std::shared_ptr<Gdiplus::Bitmap>& bmp) {
    if (!window || !bmp) return;
    std::lock_guard lock(gPreviewCacheMutex);
    std::erase_if(gPreviewCache, [](const auto& entry) { return !IsWindow(entry.first); });
    gPreviewCache[window] = bmp;
}

int visibleCards(float panelWidth) {
    return std::clamp(static_cast<int>((panelWidth - 2 * SidePadding + CardGap + .1f) /
        (CardWidth + CardGap)), 1, MaxVisibleCards);
}

bool hasRenderedContent(const DWORD* pixels, int w, const RECT& source) {
    const int sourceW = source.right - source.left, sourceH = source.bottom - source.top;
    if (sourceW < 8 || sourceH < 8) return false;
    const auto color = [&](int x, int y) { return pixels[static_cast<size_t>(y) * w + x] & 0x00FFFFFFu; };
    const int left = source.left + std::min(6, sourceW / 8);
    const int top = source.top + std::min(6, sourceH / 8);
    const int right = source.right - std::min(6, sourceW / 8);
    const int bottom = source.bottom - std::min(6, sourceH / 8);
    const DWORD first = color(left, top);
    const int stepY = std::max(1, (bottom - top) / 28);
    const int stepX = std::max(1, (right - left) / 28);
    for (int y = top; y < bottom; y += stepY)
        for (int x = left; x < right; x += stepX)
            if (color(x, y) != first) return true;
    return false;
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
    if (sourceW < 16 || sourceH < 16) return {};

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits{};
    const HBITMAP buffer = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!buffer) return {};
    const size_t pixelCount = static_cast<size_t>(w) * h;
    std::fill_n(static_cast<DWORD*>(bits), pixelCount, 0);
    const HDC dc = CreateCompatibleDC(nullptr);
    const auto old = dc ? SelectObject(dc, buffer) : nullptr;
    constexpr UINT renderFullContent = 2;
    bool captured = old && PrintWindow(window, dc, renderFullContent);
    GdiFlush();
    const auto* pixels = static_cast<const DWORD*>(bits);
    bool rendered = captured && hasRenderedContent(pixels, w, source);
    if (!rendered && old) {
        std::fill_n(static_cast<DWORD*>(bits), pixelCount, 0);
        captured = PrintWindow(window, dc, 0) != FALSE;
        GdiFlush();
        rendered = captured && hasRenderedContent(pixels, w, source);
    }
    if (old) SelectObject(dc, old);
    if (dc) DeleteDC(dc);

    std::shared_ptr<Gdiplus::Bitmap> preview;
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
    DeleteObject(buffer);
    if (preview) storePreviewCache(window, preview);
    return preview;
}
}

void Dock::clearWindowPreviewOverlays() {
    gOverlaySpecs.clear();
    for (auto& slot : gThumbSlots) destroyThumbSlot(slot);
    gThumbSlots.clear();
}

void Dock::syncWindowPreviewOverlays(const POINT* origin) {
    if (!panel || page != ChromeWindows || gOverlaySpecs.empty()) {
        for (auto& slot : gThumbSlots) {
            if (slot.host && slot.visible) {
                ShowWindow(slot.host, SW_HIDE);
                slot.visible = false;
            }
        }
        return;
    }

    const bool panelVisible = IsWindowVisible(panel) != FALSE;
    const BYTE alpha = sheet.d.layerOpacity;
    if (!panelVisible || alpha == 0) {
        for (auto& slot : gThumbSlots) {
            if (slot.host && slot.visible) {
                ShowWindow(slot.host, SW_HIDE);
                slot.visible = false;
            }
        }
        return;
    }

    POINT panelOrg{};
    if (origin) {
        panelOrg = *origin;
    } else {
        RECT pr{};
        if (!GetWindowRect(panel, &pr)) return;
        panelOrg = {pr.left, pr.top};
    }

    ensureThumbHostClass();
    while (gThumbSlots.size() > gOverlaySpecs.size()) {
        destroyThumbSlot(gThumbSlots.back());
        gThumbSlots.pop_back();
    }
    if (gThumbSlots.size() < gOverlaySpecs.size()) gThumbSlots.resize(gOverlaySpecs.size());

    for (size_t i = 0; i < gOverlaySpecs.size(); ++i) {
        const auto& spec = gOverlaySpecs[i];
        auto& slot = gThumbSlots[i];

        if (!spec.source || !IsWindow(spec.source)) {
            if (slot.host && slot.visible) { ShowWindow(slot.host, SW_HIDE); slot.visible = false; }
            slot.dwmOk = false;
            continue;
        }

        if (!slot.host || slot.owner != panel) {
            destroyThumbSlot(slot);
            const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED |
                (model.topmost ? WS_EX_TOPMOST : 0);
            slot.host = CreateWindowExW(exStyle, ThumbHostClass, L"", WS_POPUP,
                panelOrg.x, panelOrg.y, 1, 1, panel, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!slot.host) continue;
            slot.owner = panel;
            SetLayeredWindowAttributes(slot.host, 0, alpha, LWA_ALPHA);
            slot.lastAlpha = alpha;
            constexpr DWORD kCornerAttr = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
            const DWORD cornerPref = 3;       // DWMWCP_ROUNDSMALL
            DwmSetWindowAttribute(slot.host, kCornerAttr, &cornerPref, sizeof(cornerPref));
        }

        if (slot.source != spec.source || !slot.thumb) {
            if (slot.thumb) {
                DwmUnregisterThumbnail(slot.thumb);
                slot.thumb = nullptr;
            }
            slot.source = spec.source;
            slot.dwmOk = false;
            if (SUCCEEDED(DwmRegisterThumbnail(slot.host, spec.source, &slot.thumb)) && slot.thumb) {
                slot.dwmOk = true;
            }
        }

        SIZE srcSize{};
        if (!slot.dwmOk || !slot.thumb || FAILED(DwmQueryThumbnailSourceSize(slot.thumb, &srcSize)) ||
            srcSize.cx <= 0 || srcSize.cy <= 0) {
            slot.dwmOk = false;
            if (slot.visible) { ShowWindow(slot.host, SW_HIDE); slot.visible = false; }
            continue;
        }

        const float pad = 3.f;
        const float maxW = std::max(8.f, (width(spec.frameDip) - pad * 2.f) * sheet.scale);
        const float maxH = std::max(8.f, (height(spec.frameDip) - pad * 2.f) * sheet.scale);
        const float fit = std::min(maxW / static_cast<float>(srcSize.cx), maxH / static_cast<float>(srcSize.cy));
        const int tw = std::max(1, static_cast<int>(std::lround(srcSize.cx * fit)));
        const int th = std::max(1, static_cast<int>(std::lround(srcSize.cy * fit)));
        const float fx = panelOrg.x + (spec.frameDip.left + pad) * sheet.scale;
        const float fy = panelOrg.y + (spec.frameDip.top + pad) * sheet.scale;
        const int tx = static_cast<int>(std::lround(fx + (maxW - tw) * .5f));
        const int ty = static_cast<int>(std::lround(fy + (maxH - th) * .5f));
        const RECT targetRect{tx, ty, tx + tw, ty + th};

        if (slot.lastAlpha != alpha) {
            SetLayeredWindowAttributes(slot.host, 0, alpha, LWA_ALPHA);
            slot.lastAlpha = alpha;
        }

        const bool rectChanged = !EqualRect(&slot.lastRect, &targetRect);
        if (rectChanged || !slot.visible) {
            SetWindowPos(slot.host, model.topmost ? HWND_TOPMOST : HWND_TOP, tx, ty, tw, th,
                SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
            slot.lastRect = targetRect;
            slot.visible = true;
        }

        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination = {0, 0, tw, th};
        props.opacity = 255;
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(slot.thumb, &props);
    }
}

void Dock::rememberChromeWindow(HWND window) {
    if (std::find(chromeWindowOrder.begin(), chromeWindowOrder.end(), window) == chromeWindowOrder.end())
        chromeWindowOrder.push_back(window);
}

bool Dock::syncChromeWindows(bool renew) {
    std::vector<const OpenWindow*> matched;
    auto pushChromeGroup = [&]() {
        for (const HWND window : chromeWindowOrder) {
            const auto found = std::find_if(extra.windows.begin(), extra.windows.end(), [window](const OpenWindow& item) {
                return item.handle == window;
            });
            if (found != extra.windows.end() && isChromeApp(found->app) && IsWindow(window))
                matched.push_back(&*found);
        }
    };

    bool isPinnedChrome = false;
    if (previewTarget.kind == OrderKind::Shortcut) {
        if (previewTarget.index >= 0 && static_cast<size_t>(previewTarget.index) < model.shortcuts.size()) {
            const auto& app = model.shortcuts[static_cast<size_t>(previewTarget.index)];
            isPinnedChrome = isChromeApp(app);
            if (isPinnedChrome) {
                pushChromeGroup();
            } else {
                for (const auto& item : extra.windows) {
                    if (IsWindow(item.handle) && pinnedIndex(item.app) == previewTarget.index)
                        matched.push_back(&item);
                }
            }
        }
    } else if (previewTarget.kind == OrderKind::Window) {
        const auto found = std::find_if(extra.windows.begin(), extra.windows.end(),
            [&](const OpenWindow& item) { return item.handle == previewTarget.window; });
        if (found != extra.windows.end() && IsWindow(found->handle)) {
            if (isChromeApp(found->app)) {
                isPinnedChrome = true;
                pushChromeGroup();
            } else {
                matched.push_back(&*found);
            }
        }
    }

    std::vector<WindowPreview> current;
    current.reserve(matched.size());
    for (const OpenWindow* ptr : matched) {
        const auto& item = *ptr;
        const bool minimized = IsIconic(item.handle) != FALSE;
        const auto cached = std::find_if(chromeWindows.begin(), chromeWindows.end(), [&](const WindowPreview& entry) {
            return entry.window == item.handle && entry.title == item.title;
        });
        const bool hasCache = cached != chromeWindows.end();
        std::shared_ptr<Gdiplus::Bitmap> bmp = hasCache ? cached->preview : nullptr;
        if (!bmp) bmp = lookupPreviewCache(item.handle);

        // For pinned non-Chrome apps (like Netflix/Outlook/Explorer), prefer the crisp dock icon
        // so dark/empty UWP frame icons never render blank in the preview header.
        std::shared_ptr<Gdiplus::Bitmap> icon;
        if (!isPinnedChrome && previewTarget.kind == OrderKind::Shortcut &&
            previewTarget.index >= 0 && static_cast<size_t>(previewTarget.index) < appImages.size() &&
            appImages[static_cast<size_t>(previewTarget.index)]) {
            icon = appImages[static_cast<size_t>(previewTarget.index)];
        }
        if (!icon) icon = item.icon;
        if (!icon && previewTarget.kind == OrderKind::Shortcut &&
            previewTarget.index >= 0 && static_cast<size_t>(previewTarget.index) < appImages.size()) {
            icon = appImages[static_cast<size_t>(previewTarget.index)];
        }

        current.push_back({item.handle, item.title, item.active, minimized,
            !renew && hasCache && cached->minimized == minimized && cached->requested,
            std::move(icon), std::move(bmp)});
    }
    const bool changed = !std::equal(current.begin(), current.end(), chromeWindows.begin(), chromeWindows.end(),
        [](const WindowPreview& a, const WindowPreview& b) {
            return a.window == b.window && a.title == b.title && a.active == b.active &&
                a.minimized == b.minimized && a.icon == b.icon;
        });
    if (renew || changed) chromeWindows = std::move(current);
    return changed;
}

void Dock::activateAppDock(OrderItem item, HWND fallback, float x) {
    const OrderItem prevTarget = previewTarget;
    previewTarget = item;
    syncChromeWindows(false);
    const size_t count = chromeWindows.size();
    if (count == 1 && IsWindow(chromeWindows[0].window)) {
        const HWND only = chromeWindows[0].window;
        chromeIconHovered = true;
        hoverPreviewOrder = item;
        chromeHoverSince = 0;
        if (panel && page == ChromeWindows) closeSheet();
        else previewTarget = prevTarget;
        activateWindow(only, true);
        return;
    }
    if (count > 1) {
        previewTarget = prevTarget;
        showWindowPreviews(item, x, false);
        return;
    }
    previewTarget = prevTarget;
    if (fallback && IsWindow(fallback)) {
        chromeIconHovered = true;
        hoverPreviewOrder = item;
        chromeHoverSince = 0;
        if (panel && page == ChromeWindows) closeSheet();
        activateWindow(fallback, true);
    }
}

void Dock::showWindowPreviews(OrderItem target, float x, bool hover) {
    if (panel && page == ChromeWindows && !sheetClosing && previewTarget == target) {
        if (!hover && chromeHoverPanel) {
            chromeHoverPanel = false;
            SetForegroundWindow(panel);
        } else if (!hover) {
            chromeIconHovered = true;
            hoverPreviewOrder = target;
            chromeHoverSince = 0;
            close();
        }
        return;
    }
    const bool switchingHover = hover && panel && page == ChromeWindows && !sheetClosing && chromeHoverPanel;
    previewTarget = target;
    listOffset = 0;
    syncChromeWindows(true);
    if (chromeWindows.empty()) {
        previewTarget = {};
        if (switchingHover) close();
        return;
    }
    if (switchingHover) {
        chromeLeftAt = 0;
        panelDockX = x;
        sheet.hover = sheet.key = -1;
        positionSheet(false);
        paintSheet();
        updateChromePreviews();
        return;
    }
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
    OrderItem overOrder{};
    float anchor = 0;
    if (hovered == bar.hwnd && !hidden && !dockEditMode && !dockDragging && !dockOrderSource && !modal) {
        ScreenToClient(bar.hwnd, &cursor);
        const int index = bar.at(cursor.x, cursor.y);
        if (index >= 0) {
            const auto& hit = bar.hits[index];
            overIcon = hit.windowPreview && static_cast<bool>(hit.order);
            if (overIcon) {
                overOrder = hit.order;
                anchor = (hit.bounds.left + hit.bounds.right) / 2;
            }
        }
    }
    const bool showing = panel && page == ChromeWindows && chromeHoverPanel && !sheetClosing;
    if (showing && overIcon && overOrder != previewTarget) {
        chromeIconHovered = true;
        hoverPreviewOrder = overOrder;
        chromeHoverSince = 0;
        showWindowPreviews(overOrder, anchor, true);
        return;
    }
    if (overIcon != chromeIconHovered || (overIcon && overOrder != hoverPreviewOrder)) {
        chromeIconHovered = overIcon;
        hoverPreviewOrder = overIcon ? overOrder : OrderItem{};
        chromeHoverSince = overIcon ? stamp : 0;
    }
    const bool canOpen = !panel || (page == ChromeWindows && sheetClosing);
    if (canOpen && overIcon && chromeHoverSince && stamp - chromeHoverSince >= openDelay) {
        chromeHoverSince = 0;
        showWindowPreviews(overOrder, anchor, true);
        return;
    }
    if (showing) {
        if (overIcon || hovered == panel || isPreviewOverlayWindow(hovered)) chromeLeftAt = 0;
        else if (!chromeLeftAt) chromeLeftAt = stamp;
        else if (stamp - chromeLeftAt >= closeDelay) close();
    } else if (!overIcon || (panel && page != ChromeWindows)) {
        chromeIconHovered = false;
        hoverPreviewOrder = {};
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
                return entry.window == capture.window;
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
                // Skip CPU PrintWindow capture whenever DWM live thumbnail is already presenting this window!
                if (card.requested || card.minimized || hasActiveDwmThumb(card.window)) continue;
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
    gOverlaySpecs.clear();
    const int count = static_cast<int>(chromeWindows.size());
    if (!count) return;
    const int visible = visibleCards(sheet.w);
    listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
    const int end = std::min(count, listOffset + visible);
    for (int i = listOffset; i < end; ++i) {
        const auto& item = chromeWindows[i];
        const float x = SidePadding + (CardWidth + CardGap) * (i - listOffset);
        const auto card = box(x, CardTop, CardWidth, CardHeight);
        const int cardHitIdx = static_cast<int>(sheet.hits.size());
        hit(sheet, card, [this, target = item.window] {
            chromeIconHovered = true;
            chromeHoverSince = 0;
            closeSheet();
            activateWindow(target, false);
        });
        const float hot = sheet.hoverAmount();
        const float down = sheet.pressAmount();
        const bool overCard = sheet.hover == cardHitIdx || sheet.hover == cardHitIdx + 1;
        const float s = 1.f + 0.012f * hot - 0.025f * down;
        const float lift = -2.f * hot;
        const float cxCard = x + CardWidth / 2.f, cyCard = CardTop + CardHeight / 2.f;
        D2D1::Matrix3x2F cp{};
        const bool xform = hot > 0.01f || down > 0.01f;
        if (xform) {
            d.target->GetTransform(&cp);
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {cxCard, cyCard}) *
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
        if (item.icon) d.image(item.icon, icon, p.light);
        else d.icon(Layers, icon, tint(p.muted));
        d.text(item.title, box(x + 36, CardTop + 6, CardWidth - 66, 26), Caption, tint(p.ink));

        const auto frame = box(x + 10, CardTop + 37, CardWidth - 20, CardHeight - 47);
        d.fill(frame, 6, tint(p.base));

        // Record transformed frame bounds for live GPU DWM thumbnail overlay (works even when minimized!)
        Box transformedFrame = frame;
        if (xform) {
            transformedFrame.left   = cxCard + (frame.left - cxCard) * s;
            transformedFrame.right  = cxCard + (frame.right - cxCard) * s;
            transformedFrame.top    = cyCard + (frame.top - cyCard) * s + lift;
            transformedFrame.bottom = cyCard + (frame.bottom - cyCard) * s + lift;
        }
        gOverlaySpecs.push_back({item.window, transformedFrame});

        // Draw fallback only if DWM thumbnail isn't active for this window
        if (!hasActiveDwmThumb(item.window)) {
            if (item.preview) {
                const float w = static_cast<float>(item.preview->GetWidth());
                const float h = static_cast<float>(item.preview->GetHeight());
                const float scale = std::min((width(frame) - 6) / w, (height(frame) - 6) / h);
                d.image(item.preview, box(frame.left + (width(frame) - w * scale) / 2,
                    frame.top + (height(frame) - h * scale) / 2, w * scale, h * scale));
            } else if (item.icon) {
                const float sz = 34.f;
                d.image(item.icon, d.snap(box(frame.left + (width(frame) - sz) / 2.f,
                    frame.top + (height(frame) - sz) / 2.f, sz, sz)), p.light);
            } else {
                d.icon(ScreenIcon, box(frame.left + (width(frame) - 28) / 2,
                    frame.top + (height(frame) - 28) / 2, 28, 28), tint(p.muted, .70f));
            }
        }
        if (xform) d.target->SetTransform(cp);

        const Box closeBtn = box(x + CardWidth - 26.f, CardTop + 8.f, 20.f, 20.f);
        hit(sheet, closeBtn, [this, target = item.window] {
            closeAppWindows({target});
            std::erase_if(chromeWindows, [target](const WindowPreview& entry) { return entry.window == target; });
            if (chromeWindows.empty()) {
                closeSheet();
            } else {
                sheet.hover = sheet.key = -1;
                positionSheet(false);
                paintSheet();
            }
        });
        const float closeHot = sheet.hoverAmount();
        if (overCard || closeHot > 0.01f) {
            if (closeHot > 0.01f) d.fill(closeBtn, 6.f, tint(Design::Red, .85f * closeHot));
            else d.fill(closeBtn, 6.f, tint(p.ink, p.light ? .08f : .10f));
            const float cx = closeBtn.left + 10.f, cy = closeBtn.top + 10.f;
            const auto xCol = closeHot > 0.25f ? tint(0xFFFFFF, .96f) : tint(p.muted, .92f);
            d.line(cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, xCol, 1.35f);
            d.line(cx + 3.5f, cy - 3.5f, cx - 3.5f, cy + 3.5f, xCol, 1.35f);
        }
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
