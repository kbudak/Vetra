#include "dock.h"
#include <UIAutomation.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <oleacc.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "oleacc.lib")

namespace {
using Microsoft::WRL::ComPtr;

struct ScopedHighResTimer {
    bool active = false;
    ScopedHighResTimer() noexcept { active = (timeBeginPeriod(1) == TIMERR_NOERROR); }
    ~ScopedHighResTimer() { if (active) timeEndPeriod(1); }
    ScopedHighResTimer(const ScopedHighResTimer&) = delete;
    ScopedHighResTimer& operator=(const ScopedHighResTimer&) = delete;
};

struct ScopedBackgroundPriority {
    bool active = false;
    ScopedBackgroundPriority() noexcept {
        active = (SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN) != FALSE);
    }
    ~ScopedBackgroundPriority() {
        if (active) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }
    ScopedBackgroundPriority(const ScopedBackgroundPriority&) = delete;
    ScopedBackgroundPriority& operator=(const ScopedBackgroundPriority&) = delete;
};

struct CloakedMenuRecord {
    HWND hwnd{};
    LONG_PTR origEx = 0;
    RECT origRect{};
    bool changedEx = false;
    bool changedRgn = false;
    bool movedOffscreen = false;
    bool executed = false;
    ULONGLONG dismissStartedAt = 0;
    int dismissRetries = 0;
};

Dock* gTrayDock = nullptr;
HWND gCloakedOverflow = nullptr;
bool gRestoreLayered = false;
bool gRestoreOverflowRgn = false;
bool gInTraySync = false;
LONG_PTR gOrigExStyle = 0;
std::mutex gCloakMutex;
std::vector<CloakedMenuRecord> gCloakedMenus;
std::vector<CloakedMenuRecord> gDeferredUncloakMenus;
std::mutex gOverflowEventMutex;
HANDLE gOverflowReadyEvent = nullptr;
std::mutex gWarmupTrayMutex;
std::optional<std::vector<TrayItem>> gWarmupTrayItems;

// Dedicated high-priority WinEvent guard thread so overflow/popup cloaking NEVER stalls
// while the UI thread is inside COM/UIA calls or short wait loops.
constexpr UINT kGuardMsgStart = WM_APP + 41;
constexpr UINT kGuardMsgStop = WM_APP + 42;
std::mutex gGuardLifecycleMutex;
std::thread gGuardThread;
DWORD gGuardTid = 0;
HANDLE gGuardReadyEv = nullptr;
HANDLE gGuardSyncEv = nullptr;
std::atomic<bool> gGuardEnabled{false};
std::atomic<bool> gGuardCloakMenus{false};
std::atomic<bool> gGuardHideTaskbar{false};
std::atomic<bool> gGuardLockCursor{false};
std::atomic<LONG> gGuardCursorX{0};
std::atomic<LONG> gGuardCursorY{0};
std::atomic<HWND> gGuardNotifyHwnd{nullptr};
std::atomic<HWND> gGuardPanelHwnd{nullptr};
std::atomic<HWND> gGuardDetectedMenu{nullptr};
std::atomic<HWND> gGuardLastOverflow{nullptr};
std::atomic<ULONGLONG> gOverflowWatchUntil{0};
std::mutex gGuardFilterMutex;
std::unordered_set<HWND> gGuardPreVisible;
std::unordered_set<HWND> gGuardIgnored;
Text gGuardExpectedStem;
DWORD gGuardOwnerPid = 0;

void signalOverflowReadyEvent() {
    std::lock_guard lock(gOverflowEventMutex);
    if (gOverflowReadyEvent) SetEvent(gOverflowReadyEvent);
}

void syncGuardCursorState(const Dock& dock) {
    gGuardCursorX.store(dock.traySavedCursor.x, std::memory_order_relaxed);
    gGuardCursorY.store(dock.traySavedCursor.y, std::memory_order_relaxed);
    gGuardLockCursor.store(dock.trayCursorLocked, std::memory_order_release);
}

void unlockTrayCursor(Dock& dock) {
    gGuardLockCursor.store(false, std::memory_order_release);
    if (dock.trayCursorLocked) {
        ClipCursor(nullptr);
        dock.trayCursorLocked = false;
    }
    if (dock.traySavedCursor.x != 0 || dock.traySavedCursor.y != 0) {
        POINT cur{};
        if (GetCursorPos(&cur) && (cur.x != dock.traySavedCursor.x || cur.y != dock.traySavedCursor.y)) {
            SetCursorPos(dock.traySavedCursor.x, dock.traySavedCursor.y);
        }
    }
}

Text toLowerText(const wchar_t* text) {
    if (!text || !*text) return {};
    Text lower;
    for (const wchar_t* p = text; *p; ++p) lower.push_back(static_cast<wchar_t>(std::towlower(*p)));
    return lower;
}

bool containsCI(const wchar_t* text, const wchar_t* needle) {
    if (!text || !*text || !needle || !*needle) return false;
    return toLowerText(text).find(toLowerText(needle)) != Text::npos;
}

bool isChevronText(const wchar_t* text) {
    return containsCI(text, L"gizli simge") || containsCI(text, L"hidden icon") ||
        containsCI(text, L"overflow") || containsCI(text, L"chevron");
}

bool isOverflowWindow(HWND w) {
    if (!w) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(w, cls, ARRAYSIZE(cls))) return false;
    return wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
        wcscmp(cls, L"NotifyIconOverflowWindow") == 0;
}

HWND findAnyOverflow() {
    for (const wchar_t* cls : {L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {
        if (const HWND w = FindWindowW(cls, nullptr)) return w;
    }
    return nullptr;
}

HWND findVisibleOverflow() {
    for (const wchar_t* cls : {L"TopLevelWindowForOverflowXamlIsland", L"NotifyIconOverflowWindow"}) {
        if (const HWND w = FindWindowW(cls, nullptr); w && IsWindowVisible(w)) return w;
    }
    return nullptr;
}

bool isWin32MenuClass(HWND w) {
    if (!w || !IsWindow(w)) return false;
    wchar_t cls[32]{};
    return GetClassNameW(w, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"#32768") == 0;
}

HMENU nativePopupHandle(HWND window) {
    for (const LONG object : {OBJID_CLIENT, OBJID_MENU}) {
        MENUBARINFO info{sizeof(info)};
        if (GetMenuBarInfo(window, object, 0, &info) && IsMenu(info.hMenu)) return info.hMenu;
    }
    DWORD_PTR handle{};
    if (SendMessageTimeoutW(window, 0x01E1 /*MN_GETHMENU*/, 0, 0, SMTO_ABORTIFHUNG, 120, &handle) &&
        IsMenu(reinterpret_cast<HMENU>(handle))) return reinterpret_cast<HMENU>(handle);
    return nullptr;
}

HWND findWin32MenuForHandle(HMENU target, HWND exclude = nullptr) {
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"#32768", nullptr)) != nullptr) {
        if (w == exclude || !IsWindowVisible(w)) continue;
        if (!target || nativePopupHandle(w) == target) return w;
    }
    return nullptr;
}

bool isOnAnyMonitor(const RECT& rc);

void moveOverflowOffscreen(HWND overflow, bool async = false) {
    if (!overflow || !IsWindow(overflow)) return;
    const int offX = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int offY = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    RECT rc{};
    if (!GetWindowRect(overflow, &rc) || rc.left != offX || rc.top != offY) {
        const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING |
            (async ? SWP_ASYNCWINDOWPOS : 0u);
        SetWindowPos(overflow, HWND_BOTTOM, offX, offY, 0, 0, flags);
    }
    // For legacy non-DComp NotifyIconOverflowWindow, also suppress GDI rendering.
    const LONG_PTR ex = GetWindowLongPtrW(overflow, GWL_EXSTYLE);
    constexpr LONG_PTR kNoRedir = 0x00200000L;
    if ((ex & kNoRedir) == 0) {
        std::lock_guard lock(gCloakMutex);
        if (!gRestoreOverflowRgn) {
            if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
                if (SetWindowRgn(overflow, empty, FALSE)) {
                    gCloakedOverflow = overflow;
                    gRestoreOverflowRgn = true;
                } else {
                    DeleteObject(empty);
                }
            }
        }
    }
}

void forceForegroundWindow(HWND target) {
    if (!target || !IsWindow(target) || !IsWindowVisible(target)) return;
    if (GetForegroundWindow() == target) return;
    AllowSetForegroundWindow(ASFW_ANY);
    if (SetForegroundWindow(target) && GetForegroundWindow() == target) return;
    const HWND fg = GetForegroundWindow();
    const DWORD myTid = GetCurrentThreadId();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD tgtTid = GetWindowThreadProcessId(target, nullptr);
    bool attFg = false, attTgt = false;
    if (fgTid && fgTid != myTid) attFg = AttachThreadInput(myTid, fgTid, TRUE) != FALSE;
    if (tgtTid && tgtTid != myTid && tgtTid != fgTid) attTgt = AttachThreadInput(myTid, tgtTid, TRUE) != FALSE;
    SetForegroundWindow(target);
    BringWindowToTop(target);
    if (attTgt) AttachThreadInput(myTid, tgtTid, FALSE);
    if (attFg) AttachThreadInput(myTid, fgTid, FALSE);
}

void dismissOverflowWindow(HWND overflow, HWND foregroundRestore = nullptr) {
    if (!overflow || !IsWindow(overflow)) return;
    moveOverflowOffscreen(overflow, false);
    if (!IsWindowVisible(overflow)) return;

    wchar_t cls[64]{};
    GetClassNameW(overflow, cls, ARRAYSIZE(cls));
    const bool isXamlIsland = (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0);

    if (!isXamlIsland) {
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        ShowWindowAsync(overflow, SW_HIDE);
        return;
    }

    // Windows 11's XAML overflow island ignores WM_CLOSE; trigger clean light-dismiss
    // (Escape + WA_INACTIVE + foreground restore) so Explorer's internal IsOpen resets to false.
    PostMessageW(overflow, WM_KEYDOWN, VK_ESCAPE, 0x00010001);
    PostMessageW(overflow, WM_KEYUP, VK_ESCAPE, 0xC0010001);
    PostMessageW(overflow, WM_ACTIVATE, WA_INACTIVE, 0);
    if (foregroundRestore && IsWindow(foregroundRestore) && IsWindowVisible(foregroundRestore)) {
        forceForegroundWindow(foregroundRestore);
    }

    ScopedHighResTimer hiRes;
    for (int w = 0; w < 14 && IsWindow(overflow) && IsWindowVisible(overflow); ++w) {
        moveOverflowOffscreen(overflow, false);
        Sleep(2);
    }

    if (IsWindow(overflow) && IsWindowVisible(overflow) && GetForegroundWindow() == overflow) {
        INPUT esc[2]{};
        esc[0].type = esc[1].type = INPUT_KEYBOARD;
        esc[0].ki.wVk = esc[1].ki.wVk = VK_ESCAPE;
        esc[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(ARRAYSIZE(esc), esc, sizeof(INPUT));
        for (int w = 0; w < 10 && IsWindow(overflow) && IsWindowVisible(overflow); ++w) {
            moveOverflowOffscreen(overflow, false);
            Sleep(2);
        }
    }

    if (IsWindow(overflow) && IsWindowVisible(overflow)) {
        moveOverflowOffscreen(overflow, false);
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        ShowWindowAsync(overflow, SW_HIDE);
    }
}

bool isCloakedMenu(HWND w) {
    if (!w) return false;
    std::lock_guard lock(gCloakMutex);
    return std::any_of(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
}

bool cloakedOrigRect(HWND w, RECT& out) {
    std::lock_guard lock(gCloakMutex);
    const auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w && r.movedOffscreen; });
    if (it == gCloakedMenus.end()) return false;
    if ((it->origRect.right - it->origRect.left) < 16 || (it->origRect.bottom - it->origRect.top) < 12)
        return false;
    out = it->origRect;
    return true;
}

void hideSysShadowWindow(HWND w) {
    if (!w || !IsWindow(w)) return;
    if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
        if (!SetWindowRgn(w, empty, FALSE)) DeleteObject(empty);
    }
    ShowWindowAsync(w, SW_HIDE);
}

void cloakMenuWindow(HWND w, bool asyncPos = false) {
    if (!w || !IsWindow(w)) return;
    std::lock_guard lock(gCloakMutex);
    // Remove any stale deferred record for a reused popup HWND before re-cloaking.
    std::erase_if(gDeferredUncloakMenus, [w](const CloakedMenuRecord& r) { return r.hwnd == w; });

    auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
    CloakedMenuRecord rec{};
    const bool existed = (it != gCloakedMenus.end());
    if (existed) rec = *it;
    else rec.hwnd = w;

    // 1. Empty GDI region suppresses Win32 rendering immediately.
    if (!rec.changedRgn) {
        if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
            if (SetWindowRgn(w, empty, FALSE)) rec.changedRgn = true;
            else DeleteObject(empty);
        }
    }

    // 2. Full alpha=0 layered cloaking for non-WS_EX_NOREDIRECTIONBITMAP windows.
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    constexpr LONG_PTR kNoRedir = 0x00200000L; // WS_EX_NOREDIRECTIONBITMAP
    if (!rec.changedEx && (ex & kNoRedir) == 0) {
        SetLastError(0);
        SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
        if (GetLastError() == 0) {
            rec.origEx = ex;
            rec.changedEx = true;
            SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
        }
    } else if (rec.changedEx) {
        SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA);
    }

    // 3. Move offscreen with SWP_NOSENDCHANGING so even WS_EX_NOREDIRECTIONBITMAP
    // (WinUI 3 / Electron / WebView2 DComp) popups and their DWM shadows never appear on-screen,
    // while IsWindowVisible, HMENU, and UIA trees remain fully readable.
    const int offX = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int offY = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    RECT curRc{};
    if (GetWindowRect(w, &curRc)) {
        const bool validCurSize = (curRc.right - curRc.left) >= 16 && (curRc.bottom - curRc.top) >= 12;
        const bool hasValidOrig = (rec.origRect.right - rec.origRect.left) >= 16 &&
            (rec.origRect.bottom - rec.origRect.top) >= 12;
        if ((!rec.movedOffscreen || !hasValidOrig) && validCurSize &&
            (curRc.left != offX || curRc.top != offY)) {
            rec.origRect = curRc;
        }
        if (curRc.left != offX || curRc.top != offY) {
            const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING |
                (asyncPos ? SWP_ASYNCWINDOWPOS : 0u);
            if (SetWindowPos(w, HWND_BOTTOM, offX, offY, 0, 0, flags)) {
                rec.movedOffscreen = true;
            }
        }
    }

    if (rec.changedEx || rec.changedRgn || rec.movedOffscreen) {
        if (existed) *it = rec;
        else gCloakedMenus.push_back(rec);
    }
}

void restoreMenuRegionForClick(HWND w) {
    if (!w || !IsWindow(w)) return;
    std::lock_guard lock(gCloakMutex);
    const auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
    if (it != gCloakedMenus.end() && it->changedRgn) {
        SetWindowRgn(w, nullptr, FALSE);
        it->changedRgn = false;
    }
}

void applyRestoreCloakedRecord(const CloakedMenuRecord& rec) {
    if (!rec.hwnd || !IsWindow(rec.hwnd)) return;
    if (rec.movedOffscreen && (rec.origRect.right - rec.origRect.left) >= 16 &&
        (rec.origRect.bottom - rec.origRect.top) >= 12 && isOnAnyMonitor(rec.origRect)) {
        SetWindowPos(rec.hwnd, nullptr, rec.origRect.left, rec.origRect.top, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING);
    }
    if (rec.changedRgn) SetWindowRgn(rec.hwnd, nullptr, FALSE);
    if (rec.changedEx) {
        SetLayeredWindowAttributes(rec.hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(rec.hwnd, GWL_EXSTYLE, rec.origEx);
    }
}

void uncloakMenuWindow(HWND w, bool force = false, bool executed = false) {
    CloakedMenuRecord rec{};
    {
        std::lock_guard lock(gCloakMutex);
        const auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
            [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
        if (it == gCloakedMenus.end()) return;
        rec = *it;
        gCloakedMenus.erase(it);
    }
    if (!w || !IsWindow(w)) return;
    // Never uncloak a menu window while it is still visible on-screen (causes a flash right before Vetra's UI).
    if (!force && IsWindowVisible(w)) {
        rec.executed = executed;
        rec.dismissStartedAt = GetTickCount64();
        rec.dismissRetries = 0;
        std::lock_guard lock(gCloakMutex);
        std::erase_if(gDeferredUncloakMenus, [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
        gDeferredUncloakMenus.push_back(rec);
        return;
    }
    applyRestoreCloakedRecord(rec);
}

void clearDeferredUncloakFor(HWND w) {
    if (!w) return;
    std::lock_guard lock(gCloakMutex);
    std::erase_if(gDeferredUncloakMenus, [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
}

ComPtr<IUIAutomation> createUiaClient() {
    ComPtr<IUIAutomation> instance;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&instance))))
        CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&instance));
    ComPtr<IUIAutomation2> bounded;
    if (instance && SUCCEEDED(instance.As(&bounded))) {
        bounded->put_ConnectionTimeout(200);
        bounded->put_TransactionTimeout(300);
    }
    return instance;
}

IUIAutomation* sharedUia() {
    static ComPtr<IUIAutomation> instance;
    if (!instance) instance = createUiaClient();
    return instance.Get();
}

ComPtr<IUIAutomationCacheRequest> createOverflowButtonCacheReq(IUIAutomation* uia) {
    ComPtr<IUIAutomationCacheRequest> req;
    if (uia && SUCCEEDED(uia->CreateCacheRequest(&req)) && req) {
        req->AddProperty(UIA_NamePropertyId);
        req->AddProperty(UIA_LegacyIAccessibleNamePropertyId);
        req->AddProperty(UIA_HelpTextPropertyId);
        req->AddProperty(UIA_AutomationIdPropertyId);
        req->AddProperty(UIA_ClassNamePropertyId);
        req->AddProperty(UIA_BoundingRectanglePropertyId);
        req->AddProperty(UIA_RuntimeIdPropertyId);
        req->AddProperty(UIA_IsOffscreenPropertyId);
        req->put_TreeScope(TreeScope_Element);
    }
    return req;
}

ComPtr<IUIAutomationCacheRequest> createChevronButtonCacheReq(IUIAutomation* uia) {
    ComPtr<IUIAutomationCacheRequest> req;
    if (uia && SUCCEEDED(uia->CreateCacheRequest(&req)) && req) {
        req->AddProperty(UIA_NamePropertyId);
        req->AddProperty(UIA_AutomationIdPropertyId);
        req->AddProperty(UIA_ClassNamePropertyId);
        req->AddProperty(UIA_BoundingRectanglePropertyId);
        req->put_TreeScope(TreeScope_Element);
    }
    return req;
}

IUIAutomationCacheRequest* popupMenuCacheReq(IUIAutomation* uia) {
    static ComPtr<IUIAutomationCacheRequest> req;
    if (!req && uia && SUCCEEDED(uia->CreateCacheRequest(&req)) && req) {
        req->AddProperty(UIA_ControlTypePropertyId);
        req->AddProperty(UIA_NamePropertyId);
        req->AddProperty(UIA_BoundingRectanglePropertyId);
        req->AddProperty(UIA_IsEnabledPropertyId);
        req->AddProperty(UIA_IsOffscreenPropertyId);
        req->AddProperty(UIA_LegacyIAccessibleNamePropertyId);
        req->AddProperty(UIA_LegacyIAccessibleStatePropertyId);
        req->AddProperty(UIA_ToggleToggleStatePropertyId);
        req->AddProperty(UIA_SelectionItemIsSelectedPropertyId);
        req->AddProperty(UIA_ExpandCollapseExpandCollapseStatePropertyId);
        req->AddProperty(UIA_IsInvokePatternAvailablePropertyId);
        req->AddProperty(UIA_IsTogglePatternAvailablePropertyId);
        req->put_TreeScope(TreeScope_Element);
    }
    return req.Get();
}

bool isWindowCloakedByDwm(HWND w) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool isWindowTransparentAlpha(HWND w) {
    if ((GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_LAYERED) == 0) return false;
    BYTE alpha = 255;
    DWORD flags = 0;
    return GetLayeredWindowAttributes(w, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0 && alpha == 0;
}

bool isOnAnyMonitor(const RECT& rc) {
    return !IsRectEmpty(&rc) && MonitorFromRect(&rc, MONITOR_DEFAULTTONULL) != nullptr;
}

bool isNearOffscreenAnchor(const RECT& rc) {
    if (IsRectEmpty(&rc)) return false;
    const int offX = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int offY = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    return rc.left >= offX - 1200 && rc.left <= offX + 1600 &&
        rc.top >= offY - 1200 && rc.top <= offY + 1600;
}

void snapshotTrayPreWindows(Dock& dock) {
    dock.trayPreWindows.clear();
    dock.trayIgnoredPopups.clear();
    dock.trayPreForeground = GetForegroundWindow();
    std::unordered_set<HWND> preVis;
    std::pair<std::vector<Dock::TrayWindowSnap>*, std::unordered_set<HWND>*> snapCtx{&dock.trayPreWindows, &preVis};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(w)) return TRUE;
        auto* pair = reinterpret_cast<std::pair<std::vector<Dock::TrayWindowSnap>*, std::unordered_set<HWND>*>*>(lp);
        if (isWindowCloakedByDwm(w) || isWindowTransparentAlpha(w)) return TRUE;
        RECT rc{};
        GetWindowRect(w, &rc);
        pair->first->push_back({w, true, rc});
        pair->second->insert(w);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&snapCtx));
    {
        std::lock_guard lock(gGuardFilterMutex);
        gGuardPreVisible = std::move(preVis);
        gGuardIgnored.clear();
        gGuardExpectedStem = dock.trayExpectedStem;
        gGuardOwnerPid = dock.trayMenuOwnerPid;
    }
    gGuardDetectedMenu.store(nullptr, std::memory_order_release);
}

bool isExcludedSystemWindow(HWND w, const Dock& dock, wchar_t (&cls)[96], DWORD& outPid) {
    if (!w || !IsWindow(w)) return true;
    if (w == dock.hwnd || w == dock.panel || isOverflowWindow(w)) return true;
    if (!GetClassNameW(w, cls, ARRAYSIZE(cls))) return true;

    GetWindowThreadProcessId(w, &outPid);
    if (!outPid || outPid == GetCurrentProcessId()) return true;

    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"tooltips_class32") == 0 || wcscmp(cls, L"SysShadow") == 0 ||
        wcscmp(cls, L"IME") == 0 || wcscmp(cls, L"MSCTFIME UI") == 0 ||
        wcscmp(cls, L"TaskListThumbnailWnd") == 0 || wcscmp(cls, L"MultitaskingViewFrame") == 0 ||
        wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0 || wcscmp(cls, L"EdgeUiInputTopWndClass") == 0)
        return true;

    if (wcscmp(cls, L"#32768") == 0) return false;

    if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        DWORD explorerPid = 0;
        GetWindowThreadProcessId(tb, &explorerPid);
        if (explorerPid && outPid == explorerPid) {
            if (wcsstr(cls, L"Menu") == nullptr && wcsstr(cls, L"Popup") == nullptr &&
                wcsstr(cls, L"Flyout") == nullptr && wcsstr(cls, L"Xaml_WindowedPopup") == nullptr)
                return true;
        }
    }
    return false;
}

Text cachedProcessStemLower(DWORD pid) {
    if (!pid) return {};
    static std::mutex pidMutex;
    static std::unordered_map<DWORD, Text> pidCache;
    {
        std::lock_guard lock(pidMutex);
        if (const auto it = pidCache.find(pid); it != pidCache.end()) return it->second;
    }
    Text stem;
    if (const HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[1024]{};
        DWORD len = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(proc, 0, path, &len) && len > 0) {
            const Text full(path, len);
            const size_t slash = full.find_last_of(L"\\/");
            const size_t start = (slash == Text::npos) ? 0 : slash + 1;
            const size_t dot = full.find_last_of(L'.');
            const size_t end = (dot != Text::npos && dot > start) ? dot : full.size();
            stem = toLowerText(full.substr(start, end - start).c_str());
        }
        CloseHandle(proc);
    }
    if (!stem.empty()) {
        std::lock_guard lock(pidMutex);
        if (pidCache.size() > 192) pidCache.clear();
        pidCache[pid] = stem;
    }
    return stem;
}

bool stemMatchesExpected(const Text& winStem, const Text& expectedStem) {
    if (winStem.empty() || expectedStem.empty()) return false;
    if (winStem == expectedStem) return true;
    auto basePrefix = [](const Text& s) {
        size_t cut = s.find_first_of(L"._- ");
        return (cut != Text::npos && cut >= 4) ? s.substr(0, cut) : s;
    };
    const Text wb = basePrefix(winStem);
    const Text eb = basePrefix(expectedStem);
    if (wb.size() >= 4 && wb == eb) return true;
    if (expectedStem.size() >= 5 && winStem.find(expectedStem) != Text::npos) return true;
    if (winStem.size() >= 5 && expectedStem.find(winStem) != Text::npos) return true;
    return false;
}

int scoreTrayPopup(const Dock& dock, HWND w, bool fromShowEvent = false) {
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || isWindowCloakedByDwm(w)) return -1;
    if (std::find(dock.trayIgnoredPopups.begin(), dock.trayIgnoredPopups.end(), w) != dock.trayIgnoredPopups.end())
        return -1;
    wchar_t cls[96]{};
    DWORD winPid = 0;
    if (isExcludedSystemWindow(w, dock, cls, winPid)) return -1;
    if (dock.trayMenuOwnerPid && dock.trayMenuOwnerPid != winPid) return -1;

    RECT rc{};
    if (!GetWindowRect(w, &rc)) return -1;
    const bool cloaked = isCloakedMenu(w);
    RECT logicalRc = rc;
    const bool hasOrigRect = cloaked && cloakedOrigRect(w, logicalRc);
    // Apps triggered from an offscreen overflow window often open their popup near (-4000, -4000);
    // allow those offscreen rects while still rejecting parked/minimized (-32000, -32000) windows.
    const bool validGeo = cloaked || isOnAnyMonitor(rc) || isNearOffscreenAnchor(rc);
    if (!validGeo) return -1;
    const int rw = logicalRc.right - logicalRc.left, rh = logicalRc.bottom - logicalRc.top;
    if (rw < 32 || rh < 14 || rw > 1360 || rh > 1160) return -1;

    const bool win32Menu = wcscmp(cls, L"#32768") == 0;
    const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    const HWND fg = GetForegroundWindow();
    const bool isFg = (w == fg || (fg && GetAncestor(fg, GA_ROOT) == w));

    const Text winStem = (!dock.trayExpectedStem.empty() && winPid) ? cachedProcessStemLower(winPid) : Text{};
    const bool stemMatch = !dock.trayExpectedStem.empty() && stemMatchesExpected(winStem, dock.trayExpectedStem);

    const auto snap = std::find_if(dock.trayPreWindows.begin(), dock.trayPreWindows.end(),
        [w](const Dock::TrayWindowSnap& s) { return s.hwnd == w; });
    const bool newlyVisible = fromShowEvent || cloaked ||
        (dock.trayAction == Dock::TrayAction::ExecuteMenu && w == dock.trayLastPopupHwnd) ||
        (snap == dock.trayPreWindows.end() || !snap->visible);
    bool movedOnScreen = false;
    if (!newlyVisible && snap != dock.trayPreWindows.end()) {
        const int sw = snap->rect.right - snap->rect.left;
        const int sh = snap->rect.bottom - snap->rect.top;
        const bool wasHiddenGeo = (sw <= 4 || sh <= 4 || !isOnAnyMonitor(snap->rect));
        movedOnScreen = wasHiddenGeo || (!EqualRect(&snap->rect, &logicalRc) && (isFg || stemMatch));
    }
    const bool popupLikeClass = wcsncmp(cls, L"WindowsForms10.", 15) == 0 ||
        wcsncmp(cls, L"HwndWrapper[", 12) == 0 || wcsncmp(cls, L"Qt", 2) == 0 ||
        wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 || wcsncmp(cls, L"WinUIDesktop", 12) == 0 ||
        wcsncmp(cls, L"Xaml_WindowedPopup", 18) == 0 || wcsncmp(cls, L"vguiPopupWindow", 15) == 0 ||
        wcsncmp(cls, L"SDL_app", 7) == 0 || wcsstr(cls, L"Popup") != nullptr ||
        wcsstr(cls, L"Menu") != nullptr || wcsstr(cls, L"Flyout") != nullptr || wcsstr(cls, L"Tray") != nullptr;

    const bool newlyFg = isFg && w != dock.trayPreForeground &&
        (win32Menu || stemMatch || (style & WS_POPUP) != 0 || (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0 || popupLikeClass);

    if (!newlyVisible && !movedOnScreen && !newlyFg) return -1;

    // Prevent unrelated background apps from false-matching unless they look like an actual popup/menu.
    if (!win32Menu && !stemMatch && !dock.trayMenuOwnerPid &&
        (style & WS_POPUP) == 0 && (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) == 0 && !popupLikeClass)
        return -1;

    int score = 10;
    if (win32Menu) score += 150;
    if (stemMatch) score += 135;
    if (dock.trayMenuOwnerPid && dock.trayMenuOwnerPid == winPid) score += 100;
    if (isFg) score += 45;
    if ((style & WS_POPUP) != 0) score += 35;
    if ((ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0) score += 20;
    if (popupLikeClass) score += 25;

    if (cloaked && !hasOrigRect) {
        // Caught at EVENT_OBJECT_CREATE before ever painting on-screen.
        score += 28;
    } else {
        const auto distTo = [&](POINT pt) {
            const int dx = pt.x < logicalRc.left ? logicalRc.left - pt.x : (pt.x > logicalRc.right ? pt.x - logicalRc.right : 0);
            const int dy = pt.y < logicalRc.top ? logicalRc.top - pt.y : (pt.y > logicalRc.bottom ? pt.y - logicalRc.bottom : 0);
            return std::max(dx, dy);
        };
        const int dist = std::min(distTo(dock.trayNativeClick), distTo(dock.trayActionPoint));
        const int dpi = std::max<int>(96, static_cast<int>(GetDpiForWindow(dock.hwnd)));
        if (dist <= MulDiv(80, dpi, 96)) score += 30;
        else if (dist <= MulDiv(320, dpi, 96)) score += 15;
        else score += 5;
    }
    return score;
}

bool looksLikeImmediatePopup(const Dock& dock, HWND w) {
    if (!w || !IsWindow(w)) return false;
    if (std::find(dock.trayIgnoredPopups.begin(), dock.trayIgnoredPopups.end(), w) != dock.trayIgnoredPopups.end())
        return false;
    wchar_t cls[96]{};
    DWORD winPid = 0;
    if (isExcludedSystemWindow(w, dock, cls, winPid)) return false;
    if (dock.trayMenuOwnerPid && dock.trayMenuOwnerPid != winPid) return false;
    if (wcscmp(cls, L"#32768") == 0) return true;

    const auto snap = std::find_if(dock.trayPreWindows.begin(), dock.trayPreWindows.end(),
        [w](const Dock::TrayWindowSnap& s) { return s.hwnd == w; });
    if (snap != dock.trayPreWindows.end() && snap->visible) return false;

    const bool popupClass = wcsncmp(cls, L"WindowsForms10.", 15) == 0 || wcsncmp(cls, L"HwndWrapper[", 12) == 0 ||
        wcsncmp(cls, L"Qt", 2) == 0 || wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 ||
        wcsncmp(cls, L"WinUIDesktop", 12) == 0 || wcsncmp(cls, L"Xaml_WindowedPopup", 18) == 0 ||
        wcsncmp(cls, L"vguiPopupWindow", 15) == 0 || wcsncmp(cls, L"SDL_app", 7) == 0 ||
        wcsstr(cls, L"Popup") != nullptr || wcsstr(cls, L"Menu") != nullptr ||
        wcsstr(cls, L"Flyout") != nullptr || wcsstr(cls, L"Tray") != nullptr;

    if (!dock.trayExpectedStem.empty() && winPid) {
        const Text stem = cachedProcessStemLower(winPid);
        if (!stemMatchesExpected(stem, dock.trayExpectedStem) && !popupClass) return false;
    }

    const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    if ((style & WS_POPUP) != 0 || (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0) return true;
    return popupClass;
}

HWND findTriggeredTrayPopup(const Dock& dock) {
    struct Search {
        const Dock* dock{};
        HWND best{};
        int bestScore = -1;
    } state{&dock};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<Search*>(lp);
        const int score = scoreTrayPopup(*s->dock, w);
        if (score > s->bestScore) {
            s->bestScore = score;
            s->best = w;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));
    return state.best;
}

void scanAndCloakNewPopups(Dock& dock) {
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* d = reinterpret_cast<Dock*>(lp);
        if (!IsWindowVisible(w)) return TRUE;
        wchar_t cls[32]{};
        if (GetClassNameW(w, cls, ARRAYSIZE(cls)) > 0 && wcscmp(cls, L"SysShadow") == 0) {
            hideSysShadowWindow(w);
            return TRUE;
        }
        if (!isCloakedMenu(w) && looksLikeImmediatePopup(*d, w)) {
            RECT rc{};
            if (GetWindowRect(w, &rc) && (rc.right - rc.left) >= 32 && (rc.bottom - rc.top) >= 14) {
                cloakMenuWindow(w);
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dock));

    if (!dock.trayCapturedMenuHwnd || !IsWindowVisible(dock.trayCapturedMenuHwnd) ||
        !isWin32MenuClass(dock.trayCapturedMenuHwnd)) {
        if (HWND found = findTriggeredTrayPopup(dock)) {
            if (!dock.trayCapturedMenuHwnd || !IsWindowVisible(dock.trayCapturedMenuHwnd) ||
                (isWin32MenuClass(found) && !isWin32MenuClass(dock.trayCapturedMenuHwnd))) {
                dock.trayCapturedMenuHwnd = found;
                dock.trayPopupSeenTries = 0;
                cloakMenuWindow(found);
            }
        }
    }
}

void fastScanAndCloakPopups(Dock& dock, bool fullEnum) {
    // Ultra-fast kernel class lookup (~3 microseconds) catches Win32 #32768 menus & SysShadow before frame 0.
    HWND shadow = nullptr;
    while ((shadow = FindWindowExW(nullptr, shadow, L"SysShadow", nullptr)) != nullptr) {
        if (IsWindowVisible(shadow)) hideSysShadowWindow(shadow);
    }
    HWND menuWnd = nullptr;
    while ((menuWnd = FindWindowExW(nullptr, menuWnd, L"#32768", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(menuWnd, &pid);
        if (pid == GetCurrentProcessId()) continue;
        cloakMenuWindow(menuWnd);
        if (IsWindowVisible(menuWnd)) {
            const int sc = scoreTrayPopup(dock, menuWnd, true);
            const int curSc = (dock.trayCapturedMenuHwnd && IsWindowVisible(dock.trayCapturedMenuHwnd))
                ? scoreTrayPopup(dock, dock.trayCapturedMenuHwnd) : -1;
            if (sc >= 0 && sc > curSc) {
                dock.trayCapturedMenuHwnd = menuWnd;
                dock.trayPopupSeenTries = 0;
            }
        }
    }
    if (fullEnum) scanAndCloakNewPopups(dock);
}

bool isGuardExcludedPopup(HWND w, wchar_t (&cls)[96], DWORD& outPid) {
    if (!w || !IsWindow(w)) return true;
    if (w == gGuardNotifyHwnd.load(std::memory_order_relaxed) ||
        w == gGuardPanelHwnd.load(std::memory_order_relaxed) ||
        isOverflowWindow(w)) return true;
    if (!GetClassNameW(w, cls, ARRAYSIZE(cls))) return true;
    GetWindowThreadProcessId(w, &outPid);
    if (!outPid || outPid == GetCurrentProcessId()) return true;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"tooltips_class32") == 0 || wcscmp(cls, L"SysShadow") == 0 ||
        wcscmp(cls, L"IME") == 0 || wcscmp(cls, L"MSCTFIME UI") == 0 ||
        wcscmp(cls, L"TaskListThumbnailWnd") == 0 || wcscmp(cls, L"MultitaskingViewFrame") == 0 ||
        wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0 || wcscmp(cls, L"EdgeUiInputTopWndClass") == 0)
        return true;
    if (wcscmp(cls, L"#32768") == 0) return false;
    if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        DWORD explorerPid = 0;
        GetWindowThreadProcessId(tb, &explorerPid);
        if (explorerPid && outPid == explorerPid) {
            if (wcsstr(cls, L"Menu") == nullptr && wcsstr(cls, L"Popup") == nullptr &&
                wcsstr(cls, L"Flyout") == nullptr && wcsstr(cls, L"Xaml_WindowedPopup") == nullptr)
                return true;
        }
    }
    return false;
}

void CALLBACK guardWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!hwnd || !gGuardEnabled.load(std::memory_order_acquire)) return;

    if (idObject == OBJID_WINDOW && isOverflowWindow(hwnd)) {
        moveOverflowOffscreen(hwnd, true);
        gGuardLastOverflow.store(hwnd, std::memory_order_release);
        signalOverflowReadyEvent();
        if (gGuardHideTaskbar.load(std::memory_order_relaxed)) {
            if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb))
                ShowWindowAsync(tb, SW_HIDE);
        }
        return;
    }

    if (!gGuardCloakMenus.load(std::memory_order_acquire)) return;

    if (gGuardLockCursor.load(std::memory_order_relaxed)) {
        const LONG sx = gGuardCursorX.load(std::memory_order_relaxed);
        const LONG sy = gGuardCursorY.load(std::memory_order_relaxed);
        if (sx != 0 || sy != 0) {
            POINT cur{};
            if (GetCursorPos(&cur) && (cur.x != sx || cur.y != sy)) SetCursorPos(sx, sy);
        }
    }

    if (event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW &&
        event != EVENT_SYSTEM_MENUPOPUPSTART && event != EVENT_SYSTEM_FOREGROUND &&
        event != EVENT_OBJECT_LOCATIONCHANGE)
        return;

    if (idObject != OBJID_WINDOW && event != EVENT_SYSTEM_MENUPOPUPSTART && event != EVENT_SYSTEM_FOREGROUND)
        return;

    wchar_t cls[96]{};
    if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) <= 0) return;
    if (wcscmp(cls, L"SysShadow") == 0) {
        hideSysShadowWindow(hwnd);
        return;
    }

    const HWND root = GetAncestor(hwnd, GA_ROOT);
    const HWND target = (wcscmp(cls, L"#32768") == 0) ? hwnd : (root ? root : hwnd);
    DWORD winPid = 0;
    if (isGuardExcludedPopup(target, cls, winPid)) return;

    const bool win32Menu = (wcscmp(cls, L"#32768") == 0);
    if (!win32Menu) {
        bool wasPreVisible = false;
        bool ignored = false;
        DWORD expectedOwner = 0;
        Text expectedStem;
        {
            std::lock_guard lock(gGuardFilterMutex);
            wasPreVisible = gGuardPreVisible.contains(target);
            ignored = gGuardIgnored.contains(target);
            expectedOwner = gGuardOwnerPid;
            expectedStem = gGuardExpectedStem;
        }
        if (ignored || wasPreVisible) return;
        if (expectedOwner && expectedOwner != winPid) return;

        const bool popupClass = wcsncmp(cls, L"WindowsForms10.", 15) == 0 || wcsncmp(cls, L"HwndWrapper[", 12) == 0 ||
            wcsncmp(cls, L"Qt", 2) == 0 || wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 ||
            wcsncmp(cls, L"WinUIDesktop", 12) == 0 || wcsncmp(cls, L"Xaml_WindowedPopup", 18) == 0 ||
            wcsncmp(cls, L"vguiPopupWindow", 15) == 0 || wcsncmp(cls, L"SDL_app", 7) == 0 ||
            wcsstr(cls, L"Popup") != nullptr || wcsstr(cls, L"Menu") != nullptr ||
            wcsstr(cls, L"Flyout") != nullptr || wcsstr(cls, L"Tray") != nullptr;
        const LONG_PTR style = GetWindowLongPtrW(target, GWL_STYLE);
        const LONG_PTR ex = GetWindowLongPtrW(target, GWL_EXSTYLE);
        const bool popupStyle = (style & WS_POPUP) != 0 || (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0;
        if (!popupClass && !popupStyle) return;
        if (!expectedStem.empty()) {
            const Text stem = cachedProcessStemLower(winPid);
            if (!stemMatchesExpected(stem, expectedStem) && !popupClass) return;
        }
    }

    cloakMenuWindow(target, true);
    if (event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_MENUPOPUPSTART ||
        event == EVENT_SYSTEM_FOREGROUND || IsWindowVisible(target)) {
        const HWND prev = gGuardDetectedMenu.load(std::memory_order_acquire);
        if (!prev || !IsWindowVisible(prev) || (win32Menu && !isWin32MenuClass(prev))) {
            gGuardDetectedMenu.store(target, std::memory_order_release);
        }
        if (const HWND notify = gGuardNotifyHwnd.load(std::memory_order_relaxed)) {
            if (!gInTraySync) PostMessageW(notify, WM_TIMER, TraySyncTick, 0);
        }
    }
}

void ensureTrayGuardStarted() {
    std::lock_guard lock(gGuardLifecycleMutex);
    if (gGuardThread.joinable()) return;
    gGuardReadyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gGuardSyncEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gGuardThread = std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        MSG msg{};
        PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
        gGuardTid = GetCurrentThreadId();
        if (gGuardReadyEv) SetEvent(gGuardReadyEv);

        std::array<HWINEVENTHOOK, 3> hooks{};
        const auto unhookAll = [&]() {
            for (auto& h : hooks) {
                if (h) { UnhookWinEvent(h); h = nullptr; }
            }
        };

        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == kGuardMsgStart) {
                unhookAll();
                const DWORD pidFilter = static_cast<DWORD>(msg.wParam);
                constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
                hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MENUPOPUPEND, nullptr,
                    guardWinEventProc, pidFilter, 0, flags);
                hooks[1] = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
                    guardWinEventProc, pidFilter, 0, flags);
                hooks[2] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
                    guardWinEventProc, pidFilter, 0, flags);
                if (gGuardSyncEv) SetEvent(gGuardSyncEv);
            } else if (msg.message == kGuardMsgStop) {
                unhookAll();
                if (gGuardSyncEv) SetEvent(gGuardSyncEv);
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        unhookAll();
    });
    if (gGuardReadyEv) WaitForSingleObject(gGuardReadyEv, 500);
}

void shutdownTrayGuard() {
    std::lock_guard lock(gGuardLifecycleMutex);
    gGuardEnabled.store(false, std::memory_order_release);
    gGuardCloakMenus.store(false, std::memory_order_release);
    gGuardLockCursor.store(false, std::memory_order_release);
    if (gGuardThread.joinable()) {
        if (gGuardTid) PostThreadMessageW(gGuardTid, WM_QUIT, 0, 0);
        gGuardThread.join();
    }
    gGuardTid = 0;
    if (gGuardReadyEv) { CloseHandle(gGuardReadyEv); gGuardReadyEv = nullptr; }
    if (gGuardSyncEv) { CloseHandle(gGuardSyncEv); gGuardSyncEv = nullptr; }
}

void CALLBACK trayWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!hwnd) return;
    if (idObject == OBJID_WINDOW && isOverflowWindow(hwnd)) {
        moveOverflowOffscreen(hwnd);
        signalOverflowReadyEvent();
        if (gTrayDock && gTrayDock->trayAction != Dock::TrayAction::None) {
            gTrayDock->trayOverflow = hwnd;
            if (gTrayDock->trayNeedsReveal) {
                if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb))
                    ShowWindowAsync(tb, SW_HIDE);
            }
        }
        return;
    }
    if (!gTrayDock) return;
    if ((gTrayDock->trayAction == Dock::TrayAction::RightClickMenu ||
         gTrayDock->trayAction == Dock::TrayAction::ExecuteMenu) &&
        gTrayDock->trayRightClickPending) {
        if (gTrayDock->trayCursorLocked && (gTrayDock->traySavedCursor.x != 0 || gTrayDock->traySavedCursor.y != 0)) {
            POINT cur{};
            if (GetCursorPos(&cur) && (cur.x != gTrayDock->traySavedCursor.x || cur.y != gTrayDock->traySavedCursor.y))
                SetCursorPos(gTrayDock->traySavedCursor.x, gTrayDock->traySavedCursor.y);
        }
        if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_MENUPOPUPSTART) &&
            (idObject == OBJID_WINDOW || event == EVENT_SYSTEM_MENUPOPUPSTART)) {
            wchar_t cls[32]{};
            if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0) {
                if (wcscmp(cls, L"SysShadow") == 0) {
                    hideSysShadowWindow(hwnd);
                    return;
                }
                if (wcscmp(cls, L"#32768") == 0) {
                    cloakMenuWindow(hwnd);
                } else {
                    const HWND root = GetAncestor(hwnd, GA_ROOT);
                    const HWND target = root ? root : hwnd;
                    if (looksLikeImmediatePopup(*gTrayDock, target)) {
                        cloakMenuWindow(target);
                    }
                }
            }
        }
        if (event == EVENT_SYSTEM_MENUPOPUPSTART || event == EVENT_OBJECT_SHOW ||
            event == EVENT_SYSTEM_FOREGROUND || event == EVENT_OBJECT_LOCATIONCHANGE) {
            if (idObject != OBJID_WINDOW && event != EVENT_SYSTEM_MENUPOPUPSTART && event != EVENT_SYSTEM_FOREGROUND) return;
            const HWND root = GetAncestor(hwnd, GA_ROOT);
            const HWND candidate = root ? root : hwnd;
            const bool isShowEvent = (event == EVENT_OBJECT_SHOW || event == EVENT_SYSTEM_MENUPOPUPSTART);
            const int candScore = scoreTrayPopup(*gTrayDock, candidate, isShowEvent);
            if (candScore < 0) return;
            const HWND current = gTrayDock->trayCapturedMenuHwnd;
            const int curScore = current ? scoreTrayPopup(*gTrayDock, current) : -1;
            if (!current || !IsWindowVisible(current) || candScore > curScore + 20) {
                gTrayDock->trayCapturedMenuHwnd = candidate;
                gTrayDock->trayPopupSeenTries = 0;
                cloakMenuWindow(candidate);
                if (!gInTraySync) PostMessageW(gTrayDock->hwnd, WM_TIMER, TraySyncTick, 0);
            }
        }
    }
}

void installTrayHooks(Dock& dock, DWORD processFilter = 0) {
    for (auto& h : dock.trayHooks) {
        if (h) { UnhookWinEvent(h); h = nullptr; }
    }
    gTrayDock = &dock;
    gGuardNotifyHwnd.store(dock.hwnd, std::memory_order_relaxed);
    gGuardPanelHwnd.store(dock.panel, std::memory_order_relaxed);
    gGuardHideTaskbar.store(dock.trayNeedsReveal, std::memory_order_relaxed);
    gGuardCloakMenus.store(dock.trayRightClickPending, std::memory_order_release);
    gGuardEnabled.store(true, std::memory_order_release);

    ensureTrayGuardStarted();
    if (gGuardTid) {
        if (gGuardSyncEv) ResetEvent(gGuardSyncEv);
        PostThreadMessageW(gGuardTid, kGuardMsgStart, static_cast<WPARAM>(processFilter), 0);
        if (gGuardSyncEv) WaitForSingleObject(gGuardSyncEv, 60);
    }

    constexpr DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    dock.trayHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MENUPOPUPEND, nullptr,
        trayWinEventProc, processFilter, 0, flags);
    dock.trayHooks[1] = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
        trayWinEventProc, processFilter, 0, flags);
    dock.trayHooks[2] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        trayWinEventProc, processFilter, 0, flags);
}

void removeTrayHooks(Dock& dock) {
    gGuardCloakMenus.store(false, std::memory_order_release);
    gGuardLockCursor.store(false, std::memory_order_release);
    gGuardEnabled.store(false, std::memory_order_release);
    if (gGuardTid) {
        if (gGuardSyncEv) ResetEvent(gGuardSyncEv);
        PostThreadMessageW(gGuardTid, kGuardMsgStop, 0, 0);
        if (gGuardSyncEv) WaitForSingleObject(gGuardSyncEv, 40);
    }
    for (auto& h : dock.trayHooks) {
        if (h) { UnhookWinEvent(h); h = nullptr; }
    }
    if (gTrayDock == &dock) gTrayDock = nullptr;
}

bool tryInvoke(IUIAutomationElement* element) {
    if (!element) return false;
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) &&
        invoke && SUCCEEDED(invoke->Invoke()))
        return true;
    ComPtr<IUIAutomationTogglePattern> toggle;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) &&
        toggle && SUCCEEDED(toggle->Toggle()))
        return true;
    ComPtr<IUIAutomationSelectionItemPattern> select;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&select))) &&
        select && SUCCEEDED(select->Select()))
        return true;
    ComPtr<IUIAutomationLegacyIAccessiblePattern> legacy;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy))) &&
        legacy && SUCCEEDED(legacy->DoDefaultAction()))
        return true;
    return false;
}

enum class InvokeOutcome { Failed, Succeeded, TimedOut };

InvokeOutcome tryInvokeBounded(IUIAutomationElement* element, DWORD timeoutMs = 220) {
    if (!element) return InvokeOutcome::Failed;

    UIA_HWND native = nullptr;
    if (SUCCEEDED(element->get_CurrentNativeWindowHandle(&native)) && native) {
        HWND child = reinterpret_cast<HWND>(native);
        if (IsWindow(child)) {
            wchar_t cls[32]{};
            if (GetClassNameW(child, cls, ARRAYSIZE(cls)) > 0 && _wcsicmp(cls, L"Button") == 0) {
                PostMessageW(child, BM_CLICK, 0, 0);
                return InvokeOutcome::Succeeded;
            }
        }
    }

    bool supported = false;
    for (const PATTERNID pid : {UIA_InvokePatternId, UIA_TogglePatternId, UIA_SelectionItemPatternId, UIA_LegacyIAccessiblePatternId}) {
        ComPtr<IUnknown> pat;
        if (SUCCEEDED(element->GetCurrentPattern(pid, &pat)) && pat) {
            supported = true;
            break;
        }
    }
    if (!supported) return InvokeOutcome::Failed;

    IStream* stream = nullptr;
    if (FAILED(CoMarshalInterThreadInterfaceInStream(__uuidof(IUIAutomationElement), element, &stream)) || !stream)
        return tryInvoke(element) ? InvokeOutcome::Succeeded : InvokeOutcome::Failed;

    struct Shared {
        HANDLE doneEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atomic<bool> ok{false};
        ~Shared() { if (doneEv) CloseHandle(doneEv); }
    };
    auto shared = std::make_shared<Shared>();

    std::thread([stream, shared] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IUIAutomationElement> target;
        if (SUCCEEDED(CoGetInterfaceAndReleaseStream(stream, IID_PPV_ARGS(&target))) && target) {
            shared->ok.store(tryInvoke(target.Get()), std::memory_order_release);
        }
        if (shared->doneEv) SetEvent(shared->doneEv);
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();

    if (shared->doneEv && WaitForSingleObject(shared->doneEv, timeoutMs) == WAIT_OBJECT_0) {
        return shared->ok.load(std::memory_order_acquire) ? InvokeOutcome::Succeeded : InvokeOutcome::Failed;
    }
    // Still running past timeoutMs (e.g. app opened a modal dialog from Invoke): treat as invoked.
    return InvokeOutcome::TimedOut;
}

bool tryInvokeAsync(IUIAutomationElement* element) {
    return tryInvokeBounded(element, 180) != InvokeOutcome::Failed;
}

bool findAndInvokeInRoot(IUIAutomation* automation, IUIAutomationCacheRequest* cacheReq,
                         IUIAutomationElement* root, bool requireVisibleBounds,
                         ComPtr<IUIAutomationElement>* outChevron = nullptr) {
    if (!root || !automation || !cacheReq) return false;

    // Fast path: targeted FindFirst for Windows 11's SystemTray.ChevronButton avoids serializing 100+ taskbar buttons.
    VARIANT chevronCls{};
    chevronCls.vt = VT_BSTR;
    chevronCls.bstrVal = SysAllocString(L"SystemTray.ChevronButton");
    if (chevronCls.bstrVal) {
        ComPtr<IUIAutomationCondition> clsCond;
        if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ClassNamePropertyId, chevronCls, &clsCond)) && clsCond) {
            ComPtr<IUIAutomationElement> direct;
            if (SUCCEEDED(root->FindFirstBuildCache(TreeScope_Descendants, clsCond.Get(), cacheReq, &direct)) && direct) {
                RECT rc{};
                const bool visible = SUCCEEDED(direct->get_CachedBoundingRectangle(&rc)) &&
                    rc.right > rc.left && rc.bottom > rc.top;
                if ((visible || !requireVisibleBounds) && tryInvoke(direct.Get())) {
                    if (outChevron) *outChevron = direct;
                    VariantClear(&chevronCls);
                    return true;
                }
            }
        }
        VariantClear(&chevronCls);
    }

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, &cond)) || !cond)
        return false;

    ComPtr<IUIAutomationElementArray> buttons;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cacheReq, &buttons)) || !buttons)
        return false;

    int count = 0;
    buttons->get_Length(&count);

    ComPtr<IUIAutomationElement> leftmostNormal;
    LONG leftmostX = 0x7FFFFFFF;

    for (int i = 0; i < count; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(buttons->GetElement(i, &el)) || !el) continue;

        RECT rc{};
        const bool visible = SUCCEEDED(el->get_CachedBoundingRectangle(&rc)) &&
            rc.right > rc.left && rc.bottom > rc.top;

        BSTR nameBstr{}, idBstr{}, clsBstr{};
        el->get_CachedName(&nameBstr);
        el->get_CachedAutomationId(&idBstr);
        el->get_CachedClassName(&clsBstr);

        const wchar_t* name = nameBstr ? nameBstr : L"";
        const wchar_t* autoId = idBstr ? idBstr : L"";
        const wchar_t* cls = clsBstr ? clsBstr : L"";

        const bool explicitMatch = isChevronText(name) || isChevronText(autoId) || isChevronText(cls);
        const bool normalTrayBtn = wcscmp(autoId, L"SystemTrayIcon") == 0 &&
            (wcscmp(cls, L"SystemTray.NormalButton") == 0 || wcscmp(cls, L"SystemTray.ChevronButton") == 0);

        bool invoked = false;
        if (explicitMatch && (visible || !requireVisibleBounds)) {
            invoked = tryInvoke(el.Get());
            if (invoked && outChevron) *outChevron = el;
        } else if (requireVisibleBounds && visible && normalTrayBtn && rc.left < leftmostX) {
            leftmostX = rc.left;
            leftmostNormal = el;
        }

        if (nameBstr) SysFreeString(nameBstr);
        if (idBstr) SysFreeString(idBstr);
        if (clsBstr) SysFreeString(clsBstr);
        if (invoked) return true;
    }

    if (requireVisibleBounds && leftmostNormal && tryInvoke(leftmostNormal.Get())) {
        if (outChevron) *outChevron = leftmostNormal;
        return true;
    }
    return false;
}

bool invokeTrayOverflowWithUia(IUIAutomation* automation, HWND taskbar, bool requireVisibleBounds,
                               ComPtr<IUIAutomationElement>* outChevron = nullptr) {
    if (!automation || !taskbar) return false;
    auto cacheReq = createChevronButtonCacheReq(automation);
    if (!cacheReq) return false;

    // Query only the XAML composition bridge / tray notify child windows directly (avoids 10+ redundant child walks).
    std::vector<HWND> targets;
    EnumChildWindows(taskbar, [](HWND child, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<HWND>*>(lp);
        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, ARRAYSIZE(cls)) > 0 &&
            (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
             wcscmp(cls, L"TrayNotifyWnd") == 0 ||
             wcscmp(cls, L"ReBarWindow32") == 0)) {
            list->push_back(child);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targets));
    if (targets.empty()) targets.push_back(taskbar);

    for (HWND target : targets) {
        ComPtr<IUIAutomationElement> root;
        if (SUCCEEDED(automation->ElementFromHandle(target, &root)) && root &&
            findAndInvokeInRoot(automation, cacheReq.Get(), root.Get(), requireVisibleBounds, outChevron))
            return true;
    }
    if (targets.front() != taskbar) {
        ComPtr<IUIAutomationElement> root;
        if (SUCCEEDED(automation->ElementFromHandle(taskbar, &root)) && root &&
            findAndInvokeInRoot(automation, cacheReq.Get(), root.Get(), requireVisibleBounds, outChevron))
            return true;
    }
    return false;
}

bool invokeTrayOverflow(HWND taskbar, bool requireVisibleBounds) {
    static HWND cachedTaskbar = nullptr;
    static ComPtr<IUIAutomationElement> cachedChevron;
    if (cachedTaskbar == taskbar && cachedChevron) {
        RECT rc{};
        const bool hasRect = SUCCEEDED(cachedChevron->get_CurrentBoundingRectangle(&rc)) &&
            rc.right > rc.left && rc.bottom > rc.top;
        if ((hasRect || !requireVisibleBounds) && tryInvoke(cachedChevron.Get())) {
            return true;
        }
        cachedChevron.Reset();
    }
    ComPtr<IUIAutomationElement> found;
    if (invokeTrayOverflowWithUia(sharedUia(), taskbar, requireVisibleBounds, &found)) {
        if (found) {
            cachedTaskbar = taskbar;
            cachedChevron = std::move(found);
        }
        return true;
    }
    return false;
}

bool invokeViaWinBWithUia(IUIAutomation* automation, HWND taskbar) {
    DWORD taskbarPid = 0;
    GetWindowThreadProcessId(taskbar, &taskbarPid);
    if (!taskbarPid) return false;

    INPUT keys[4]{};
    for (auto& k : keys) k.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[3].ki.wVk = VK_LWIN;
    keys[1].ki.wVk = keys[2].ki.wVk = 'B';
    keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT)) != ARRAYSIZE(keys)) return false;

    Sleep(70);

    if (automation) {
        ComPtr<IUIAutomationElement> focused;
        if (SUCCEEDED(automation->GetFocusedElement(&focused)) && focused) {
            int focusedPid = 0;
            focused->get_CurrentProcessId(&focusedPid);
            if (static_cast<DWORD>(focusedPid) == taskbarPid && tryInvoke(focused.Get()))
                return true;
        }
    }

    const HWND fg = GetForegroundWindow();
    DWORD fgPid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fgPid);
    if (fgPid == taskbarPid) {
        INPUT enter[2]{};
        for (auto& k : enter) k.type = INPUT_KEYBOARD;
        enter[0].ki.wVk = enter[1].ki.wVk = VK_RETURN;
        enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
        return SendInput(ARRAYSIZE(enter), enter, sizeof(INPUT)) == ARRAYSIZE(enter);
    }
    return false;
}

bool invokeViaWinB(HWND taskbar) {
    return invokeViaWinBWithUia(sharedUia(), taskbar);
}

std::shared_ptr<Gdiplus::Bitmap> bitmapFromHIcon(HICON hIcon) {
    if (!hIcon) return nullptr;
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    std::shared_ptr<Gdiplus::Bitmap> result;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<DWORD> colorPx(static_cast<size_t>(w) * h);
        HDC hdc = GetDC(nullptr);
        const int gotColor = GetDIBits(hdc, ii.hbmColor, 0, h, colorPx.data(), &bi, DIB_RGB_COLORS);
        std::vector<DWORD> maskPx(static_cast<size_t>(w) * h);
        const int gotMask = ii.hbmMask ? GetDIBits(hdc, ii.hbmMask, 0, h, maskPx.data(), &bi, DIB_RGB_COLORS) : 0;
        ReleaseDC(nullptr, hdc);

        if (gotColor) {
            bool hasAlpha = false;
            for (DWORD px : colorPx) {
                if ((px >> 24) != 0) { hasAlpha = true; break; }
            }
            auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            Gdiplus::BitmapData data{};
            Gdiplus::Rect rect(0, 0, w, h);
            if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
                for (int y = 0; y < h; ++y) {
                    auto* dst = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.Scan0) + y * data.Stride);
                    const DWORD* src = colorPx.data() + static_cast<size_t>(y) * w;
                    const DWORD* msk = gotMask ? (maskPx.data() + static_cast<size_t>(y) * w) : nullptr;
                    for (int x = 0; x < w; ++x) {
                        const DWORD px = src[x];
                        if (hasAlpha) {
                            const unsigned a = (px >> 24) & 0xFFu;
                            const unsigned r = ((px >> 16) & 0xFFu) * a / 255u;
                            const unsigned g = ((px >> 8) & 0xFFu) * a / 255u;
                            const unsigned b = (px & 0xFFu) * a / 255u;
                            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
                        } else if (msk && (msk[x] & 0x00FFFFFFu) != 0) {
                            dst[x] = 0;
                        } else {
                            dst[x] = px | 0xFF000000u;
                        }
                    }
                }
                bmp->UnlockBits(&data);
                result = normalizeAppIcon(*bmp);
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

Text expandKnownFolderPrefix(const Text& raw) {
    if (raw.size() > 38 && raw[0] == L'{' && raw[37] == L'}') {
        GUID folderId{};
        PWSTR folder{};
        if (SUCCEEDED(CLSIDFromString(raw.substr(0, 38).c_str(), &folderId)) &&
            SUCCEEDED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &folder))) {
            Text expanded = Text(folder) + raw.substr(38);
            CoTaskMemFree(folder);
            return expanded;
        }
    }
    return raw;
}

Text fileStemLower(const Text& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t start = (slash == Text::npos) ? 0 : slash + 1;
    const size_t dot = path.find_last_of(L'.');
    const size_t end = (dot != Text::npos && dot > start) ? dot : path.size();
    return toLowerText(path.substr(start, end - start).c_str());
}

struct NotifyIconEntry {
    Text subKey;
    Text tip;
    Text tipLower;
    Text exePath;
    Text exeStemLower;
    bool exeExists = false;
    DWORD pngBytes = 0;
};

struct NotifyPngBlob {
    std::vector<BYTE> png;
    int w = 0;
    int h = 0;
};

NotifyPngBlob readNotifyIconPng(const Text& subKey) {
    NotifyPngBlob out{};
    if (subKey.empty()) return out;
    const Text fullKey = L"Control Panel\\NotifyIconSettings\\" + subKey;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, fullKey.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY,
            nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes <= 24)
        return out;
    out.png.resize(bytes);
    if (RegGetValueW(HKEY_CURRENT_USER, fullKey.c_str(), L"IconSnapshot", RRF_RT_REG_BINARY,
            nullptr, out.png.data(), &bytes) != ERROR_SUCCESS) {
        out.png.clear();
        return out;
    }
    static const BYTE kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (out.png.size() >= 24 && memcmp(out.png.data(), kPngSig, 8) == 0) {
        out.w = (out.png[16] << 24) | (out.png[17] << 16) | (out.png[18] << 8) | out.png[19];
        out.h = (out.png[20] << 24) | (out.png[21] << 16) | (out.png[22] << 8) | out.png[23];
    }
    return out;
}

std::vector<NotifyIconEntry> loadNotifyIconRegistry() {
    std::vector<NotifyIconEntry> entries;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return entries;

    std::unordered_map<Text, bool> existsCache;
    wchar_t subName[128]{};
    for (DWORD idx = 0;; ++idx) {
        DWORD nameLen = ARRAYSIZE(subName);
        if (RegEnumKeyExW(root, idx, subName, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, subName, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;

        NotifyIconEntry e{};
        e.subKey = subName;
        wchar_t regTip[512]{};
        DWORD tipBytes = sizeof(regTip);
        if (RegGetValueW(sub, nullptr, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, regTip, &tipBytes) == ERROR_SUCCESS && regTip[0]) {
            e.tip = regTip;
            e.tipLower = toLowerText(regTip);
        }
        wchar_t regExe[2048]{};
        DWORD exeBytes = sizeof(regExe);
        if (RegGetValueW(sub, nullptr, L"ExecutablePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, regExe, &exeBytes) == ERROR_SUCCESS && regExe[0]) {
            e.exePath = expandKnownFolderPrefix(regExe);
            e.exeStemLower = fileStemLower(e.exePath);
            const auto cachedIt = existsCache.find(e.exePath);
            if (cachedIt != existsCache.end()) {
                e.exeExists = cachedIt->second;
            } else {
                const DWORD attr = GetFileAttributesW(e.exePath.c_str());
                e.exeExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
                existsCache.emplace(e.exePath, e.exeExists);
            }
        }
        DWORD pngBytes = 0;
        if (RegQueryValueExW(sub, L"IconSnapshot", nullptr, nullptr, nullptr, &pngBytes) == ERROR_SUCCESS && pngBytes > 24) {
            e.pngBytes = pngBytes;
        }
        RegCloseKey(sub);
        if (!e.tip.empty() || !e.exePath.empty() || e.pngBytes > 0)
            entries.push_back(std::move(e));
    }
    RegCloseKey(root);
    return entries;
}

std::shared_ptr<Gdiplus::Bitmap> decodePngNormalized(const std::vector<BYTE>& png, bool stripOpaqueDarkBox = false) {
    if (png.size() < 24) return nullptr;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, png.size());
    if (!mem) return nullptr;
    if (void* ptr = GlobalLock(mem)) {
        memcpy(ptr, png.data(), png.size());
        GlobalUnlock(mem);
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream)) || !stream) {
        GlobalFree(mem);
        return nullptr;
    }
    std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream.Get()));
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok) return nullptr;

    if (stripOpaqueDarkBox) {
        const int w = static_cast<int>(decoded->GetWidth());
        const int h = static_cast<int>(decoded->GetHeight());
        if (w >= 16 && h >= 16) {
            auto copy = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            Gdiplus::Graphics g(copy.get());
            g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
            g.DrawImage(decoded.get(), 0, 0, w, h);
            Gdiplus::Rect rect(0, 0, w, h);
            Gdiplus::BitmapData data{};
            if (copy->LockBits(&rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
                auto pxAt = [&](int x, int y) -> DWORD& {
                    return reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride)[x];
                };
                const DWORD c0 = pxAt(0, 0), c1 = pxAt(w - 1, 0), c2 = pxAt(0, h - 1), c3 = pxAt(w - 1, h - 1);
                auto dist = [](DWORD a, DWORD b) {
                    const int dr = std::abs(static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF));
                    const int dg = std::abs(static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF));
                    const int db = std::abs(static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF));
                    return std::max({dr, dg, db});
                };
                const bool allOpaque = ((c0 >> 24) >= 245 && (c1 >> 24) >= 245 && (c2 >> 24) >= 245 && (c3 >> 24) >= 245);
                const bool uniform = dist(c0, c1) <= 12 && dist(c0, c2) <= 12 && dist(c0, c3) <= 12;
                const int maxCh = std::max({static_cast<int>((c0 >> 16) & 0xFF), static_cast<int>((c0 >> 8) & 0xFF), static_cast<int>(c0 & 0xFF)});
                if (allOpaque && uniform && maxCh <= 64) {
                    std::vector<uint8_t> vis(static_cast<size_t>(w) * h, 0);
                    std::queue<int> q;
                    auto push = [&](int x, int y) {
                        const size_t i = static_cast<size_t>(y) * w + x;
                        if (!vis[i] && dist(pxAt(x, y), c0) <= 16) {
                            vis[i] = 1;
                            q.push(static_cast<int>(i));
                        }
                    };
                    for (int x = 0; x < w; ++x) { push(x, 0); push(x, h - 1); }
                    for (int y = 1; y < h - 1; ++y) { push(0, y); push(w - 1, y); }
                    while (!q.empty()) {
                        const int cur = q.front();
                        q.pop();
                        const int cx = cur % w, cy = cur / w;
                        pxAt(cx, cy) = 0;
                        if (cx > 0) push(cx - 1, cy);
                        if (cx + 1 < w) push(cx + 1, cy);
                        if (cy > 0) push(cx, cy - 1);
                        if (cy + 1 < h) push(cx, cy + 1);
                    }
                }
                copy->UnlockBits(&data);
                return normalizeAppIcon(*copy);
            }
        }
    }
    return normalizeAppIcon(*decoded);
}

std::mutex gTrayCacheMutex;
std::mutex gAppsFolderBuildMutex;
std::vector<NotifyIconEntry> gNotifyRegistryCache;
bool gNotifyRegistryLoaded = false;
std::unordered_map<Text, Text> gAppsFolderPfnCache;
bool gAppsFolderPfnLoaded = false;
std::unordered_map<Text, std::shared_ptr<Gdiplus::Bitmap>> gTrayIconBitmapCache;

std::vector<NotifyIconEntry> cachedNotifyIconRegistry(bool forceReload = false) {
    {
        std::lock_guard lock(gTrayCacheMutex);
        if (!forceReload && gNotifyRegistryLoaded) return gNotifyRegistryCache;
    }
    auto loaded = loadNotifyIconRegistry();
    std::lock_guard lock(gTrayCacheMutex);
    gNotifyRegistryCache = loaded;
    gNotifyRegistryLoaded = true;
    return loaded;
}

Text lookupAppsFolderIdByPfn(const Text& pfnLower) {
    if (pfnLower.empty()) return {};
    {
        std::lock_guard lock(gTrayCacheMutex);
        if (gAppsFolderPfnLoaded) {
            const auto it = gAppsFolderPfnCache.find(pfnLower);
            return it != gAppsFolderPfnCache.end() ? it->second : Text{};
        }
    }
    std::lock_guard buildLock(gAppsFolderBuildMutex);
    {
        std::lock_guard lock(gTrayCacheMutex);
        if (gAppsFolderPfnLoaded) {
            const auto it = gAppsFolderPfnCache.find(pfnLower);
            return it != gAppsFolderPfnCache.end() ? it->second : Text{};
        }
    }
    std::unordered_map<Text, Text> built;
    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> entries;
    if (SUCCEEDED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder))) &&
        SUCCEEDED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&entries)))) {
        ComPtr<IShellItem> item;
        while (entries->Next(1, item.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
            PWSTR parsing{};
            if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &parsing)) && parsing) {
                const Text id = parsing;
                CoTaskMemFree(parsing);
                const size_t bang = id.find(L'!');
                if (bang != Text::npos && bang > 0) {
                    const Text key = toLowerText(id.substr(0, bang).c_str());
                    built.try_emplace(key, id);
                }
            }
        }
    }
    std::lock_guard lock(gTrayCacheMutex);
    gAppsFolderPfnCache = std::move(built);
    gAppsFolderPfnLoaded = true;
    const auto it = gAppsFolderPfnCache.find(pfnLower);
    return it != gAppsFolderPfnCache.end() ? it->second : Text{};
}

std::shared_ptr<Gdiplus::Bitmap> highResWindowsAppsIcon(const Text& exePath) {
    const Text low = toLowerText(exePath.c_str());
    const size_t pos = low.find(L"\\windowsapps\\");
    if (pos == Text::npos) return nullptr;
    const size_t pkgStart = pos + 13;
    const size_t pkgEnd = exePath.find(L'\\', pkgStart);
    if (pkgEnd == Text::npos) return nullptr;
    const Text pkgFull = exePath.substr(pkgStart, pkgEnd - pkgStart);
    const size_t firstUs = pkgFull.find(L'_');
    const size_t dblUs = pkgFull.rfind(L"__");
    if (firstUs == Text::npos || dblUs == Text::npos || dblUs <= firstUs) return nullptr;
    const Text pfn = pkgFull.substr(0, firstUs) + L"_" + pkgFull.substr(dblUs + 2);

    // Fast direct lookup via shell:AppsFolder\<PFN>!App, then O(1) cached PFN map lookup.
    const Text direct = L"shell:AppsFolder\\" + pfn + L"!App";
    if (auto bmp = highResIconForPath(direct.c_str()))
        return std::shared_ptr<Gdiplus::Bitmap>(std::move(bmp));

    const Text id = lookupAppsFolderIdByPfn(toLowerText(pfn.c_str()));
    if (!id.empty()) {
        if (auto bmp = highResIconForPath((L"shell:AppsFolder\\" + id).c_str()))
            return std::shared_ptr<Gdiplus::Bitmap>(std::move(bmp));
    }
    return nullptr;
}

Text firstWordLower(const Text& s) {
    Text out;
    for (wchar_t ch : s) {
        if (std::iswalnum(ch)) out.push_back(static_cast<wchar_t>(std::towlower(ch)));
        else if (!out.empty()) break;
    }
    return out;
}

bool isDynamicStatusTrayIcon(const Text& nameLower, const Text& exeStemLower) {
    return nameLower.find(L"güvenli") != Text::npos ||
        nameLower.find(L"security") != Text::npos ||
        nameLower.find(L"intel") != Text::npos ||
        nameLower.find(L"donanımı güvenle") != Text::npos ||
        nameLower.find(L"safely remove") != Text::npos ||
        exeStemLower == L"securityhealthsystray" ||
        exeStemLower == L"iastoricon" ||
        exeStemLower == L"explorer";
}

const NotifyIconEntry* matchNotifyRegistry(const std::vector<NotifyIconEntry>& entries, const Text& tip) {
    const Text tipLow = toLowerText(tip.c_str());
    const Text firstTok = firstWordLower(tip);
    const NotifyIconEntry* best = nullptr;
    int bestRank = 0;

    for (const auto& e : entries) {
        int score = 0;
        if (!e.tipLower.empty()) {
            if (tipLow == e.tipLower) score = 500;
            else if (tipLow.rfind(e.tipLower, 0) == 0 || e.tipLower.rfind(tipLow, 0) == 0) score = 380;
            else if (firstTok.size() >= 4 && e.tipLower.rfind(firstTok, 0) == 0) score = 260;
        }
        if (score == 0 && !e.exeStemLower.empty()) {
            // Strip common suffixes like ".root" (WhatsApp.Root -> whatsapp).
            Text stemBase = e.exeStemLower;
            if (const size_t dot = stemBase.find(L'.'); dot != Text::npos) stemBase.resize(dot);
            if (stemBase.size() >= 4 &&
                (tipLow == stemBase || tipLow.find(stemBase) != Text::npos ||
                 (firstTok.size() >= 4 && stemBase.find(firstTok) != Text::npos))) {
                score = 220;
            } else if (tipLow.find(L"g hub") != Text::npos && e.exeStemLower.find(L"lghub") != Text::npos) {
                score = 320;
            } else if ((tipLow.find(L"chatgpt") != Text::npos || tipLow.find(L"codex") != Text::npos) &&
                       (e.exeStemLower == L"chatgpt" || toLowerText(e.exePath.c_str()).find(L"openai.codex") != Text::npos)) {
                score = 320;
            } else if ((tipLow.find(L"windows güvenliği") != Text::npos || tipLow.find(L"windows security") != Text::npos) &&
                       e.exeStemLower == L"securityhealthsystray") {
                score = 320;
            } else if (tipLow.find(L"monster") != Text::npos && e.exeStemLower == L"gcuservice") {
                score = 320;
            }
        }
        if (score == 0) continue;

        int rank = score * 1000;
        if (e.exeExists) rank += 400;
        rank += std::min<int>(static_cast<int>(e.pngBytes / 32), 512);
        if (rank > bestRank) {
            bestRank = rank;
            best = &e;
        }
    }
    return best;
}

std::shared_ptr<Gdiplus::Bitmap> resolveCrispTrayIcon(const Text& name, HICON vetraIcon,
                                                      const std::vector<NotifyIconEntry>& regEntries,
                                                      const std::shared_ptr<Gdiplus::Bitmap>& printCaptured) {
    if (name == L"Vetra" && vetraIcon) {
        if (auto bmp = bitmapFromHIcon(vetraIcon)) return bmp;
    }

    const Text nameLow = toLowerText(name.c_str());
    const NotifyIconEntry* reg = matchNotifyRegistry(regEntries, name);
    const Text exeStem = reg ? reg->exeStemLower : Text{};
    const bool statusIcon = isDynamicStatusTrayIcon(nameLow, exeStem);

    std::optional<NotifyPngBlob> lazyPng;
    const auto getPng = [&]() -> const NotifyPngBlob& {
        if (!lazyPng) {
            lazyPng = (reg && reg->pngBytes > 24) ? readNotifyIconPng(reg->subKey) : NotifyPngBlob{};
        }
        return *lazyPng;
    };

    // 1. Dynamic Windows status icons (SecurityHealth, Intel RST, Safely Remove):
    // prefer LIVE PrintWindow capture first so the current green check / warning badge stays accurate.
    if (statusIcon) {
        if (printCaptured) return printCaptured;
        if (const auto& snap = getPng(); !snap.png.empty()) {
            if (auto bmp = decodePngNormalized(snap.png)) return bmp;
        }
    }

    // 2. High-res true-alpha IconSnapshot (>=48px: e.g. Antigravity 512px, NVIDIA 128px, Veloce 64px).
    if (reg && reg->pngBytes > 24) {
        const auto& snap = getPng();
        if (!snap.png.empty() && snap.w >= 48) {
            if (auto bmp = decodePngNormalized(snap.png, true)) return bmp;
        }
    }

    // 3. High-res 128x128 native executable or MSIX/Store app icon (never accept generic .exe placeholders).
    if (reg && reg->exeExists) {
        if (auto uwpBmp = highResWindowsAppsIcon(reg->exePath)) return uwpBmp;

        Text iconPath = reg->exePath;
        if (reg->exeStemLower == L"lghub_system_tray") {
            constexpr const wchar_t* ghubUi = L"C:\\Program Files\\LGHUB\\lghub.exe";
            if (GetFileAttributesW(ghubUi) != INVALID_FILE_ATTRIBUTES) iconPath = ghubUi;
        }
        if (ExtractIconExW(iconPath.c_str(), -1, nullptr, nullptr, 0) > 0) {
            if (auto exeBmp = highResIconForPath(iconPath.c_str()))
                return std::shared_ptr<Gdiplus::Bitmap>(std::move(exeBmp));
        }
    }

    // 4. Medium-res true-alpha IconSnapshot (>=24px: e.g. Monster 24px, PowerToys 32px, FocusSwitch 32px, Steam 32px).
    if (reg && reg->pngBytes > 24) {
        const auto& snap = getPng();
        if (!snap.png.empty() && snap.w >= 24) {
            if (auto bmp = decodePngNormalized(snap.png, true)) return bmp;
        }
    }

    // 5. Cleaned PrintWindow capture, then any remaining IconSnapshot.
    if (printCaptured) return printCaptured;
    if (reg && reg->pngBytes > 24) {
        const auto& snap = getPng();
        if (!snap.png.empty()) {
            if (auto bmp = decodePngNormalized(snap.png, true)) return bmp;
        }
    }
    return nullptr;
}

std::shared_ptr<Gdiplus::Bitmap> lookupNotifyIconSnapshot(const Text& tip) {
    const auto entries = cachedNotifyIconRegistry(false);
    return resolveCrispTrayIcon(tip, nullptr, entries, nullptr);
}
}

std::shared_ptr<Gdiplus::Bitmap> resolveTrayIconBitmap(const Text& name, HICON vetraIcon) {
    const auto entries = cachedNotifyIconRegistry(false);
    return resolveCrispTrayIcon(name, vetraIcon, entries, nullptr);
}

namespace {

std::shared_ptr<Gdiplus::Bitmap> lookupCachedTrayIcon(const Text& name) {
    if (name.empty()) return nullptr;
    const Text key = toLowerText(name.c_str());
    std::lock_guard lock(gTrayCacheMutex);
    const auto it = gTrayIconBitmapCache.find(key);
    return it != gTrayIconBitmapCache.end() ? it->second : nullptr;
}

void storeCachedTrayIcon(const Text& name, const std::shared_ptr<Gdiplus::Bitmap>& bmp) {
    if (!bmp || name.empty()) return;
    const Text key = toLowerText(name.c_str());
    std::lock_guard lock(gTrayCacheMutex);
    if (gTrayIconBitmapCache.size() > 160) gTrayIconBitmapCache.clear();
    gTrayIconBitmapCache[key] = bmp;
}

struct OverflowButton {
    Text name;
    RECT rect{};
    std::vector<int> runtimeId;
    ComPtr<IUIAutomationElement> element;
    bool onTaskbar = false;
};

Text readCachedStringProp(IUIAutomationElement* el, PROPERTYID prop) {
    VARIANT v{};
    VariantInit(&v);
    Text out;
    if (el && SUCCEEDED(el->GetCachedPropertyValue(prop, &v)) && v.vt == VT_BSTR && v.bstrVal) {
        out = v.bstrVal;
    }
    VariantClear(&v);
    return out;
}

std::vector<int> readCachedRuntimeId(IUIAutomationElement* el) {
    std::vector<int> rid;
    if (!el) return rid;
    VARIANT ridVar{};
    VariantInit(&ridVar);
    if (SUCCEEDED(el->GetCachedPropertyValue(UIA_RuntimeIdPropertyId, &ridVar)) &&
        ridVar.vt == (VT_I4 | VT_ARRAY) && ridVar.parray) {
        LONG lb = 0, ub = -1;
        if (SUCCEEDED(SafeArrayGetLBound(ridVar.parray, 1, &lb)) &&
            SUCCEEDED(SafeArrayGetUBound(ridVar.parray, 1, &ub)) && ub >= lb) {
            int* data = nullptr;
            if (SUCCEEDED(SafeArrayAccessData(ridVar.parray, reinterpret_cast<void**>(&data))) && data) {
                rid.assign(data, data + (ub - lb + 1));
                SafeArrayUnaccessData(ridVar.parray);
            }
        }
    }
    VariantClear(&ridVar);
    return rid;
}

bool isBuiltInTaskbarSystemButton(const Text& name, const Text& autoId, const Text& cls) {
    if (isChevronText(name.c_str()) || isChevronText(autoId.c_str()) || isChevronText(cls.c_str())) return true;
    if (containsCI(cls.c_str(), L"OmniButton") || containsCI(cls.c_str(), L"ChevronButton") ||
        containsCI(cls.c_str(), L"Language") || containsCI(cls.c_str(), L"StackList") ||
        containsCI(cls.c_str(), L"ShowDesktop"))
        return true;
    if (containsCI(autoId.c_str(), L"Clock") || containsCI(autoId.c_str(), L"ControlCenter") ||
        containsCI(autoId.c_str(), L"ShowDesktop") || containsCI(autoId.c_str(), L"InputIndicator") ||
        containsCI(autoId.c_str(), L"Notification") || containsCI(autoId.c_str(), L"StartButton") ||
        containsCI(autoId.c_str(), L"SearchButton") || containsCI(autoId.c_str(), L"TaskView") ||
        containsCI(autoId.c_str(), L"Widgets") || containsCI(autoId.c_str(), L"Copilot") ||
        containsCI(autoId.c_str(), L"PenMenu") || containsCI(autoId.c_str(), L"TouchKeyboard") ||
        containsCI(autoId.c_str(), L"VirtualTouchpad") || containsCI(autoId.c_str(), L"Privacy"))
        return true;
    // Exclude standard Windows status indicators (already hosted natively in Vetra's status area).
    if (containsCI(name.c_str(), L"hızlı ayarlar") || containsCI(name.c_str(), L"quick settings") ||
        containsCI(name.c_str(), L"sistem saati") || containsCI(name.c_str(), L"system clock") ||
        containsCI(name.c_str(), L"bildirim merkezi") || containsCI(name.c_str(), L"notification center") ||
        containsCI(name.c_str(), L"masaüstünü göster") || containsCI(name.c_str(), L"show desktop") ||
        containsCI(name.c_str(), L"giriş göstergesi") || containsCI(name.c_str(), L"input indicator") ||
        containsCI(name.c_str(), L"dokunmatik klavye") || containsCI(name.c_str(), L"touch keyboard") ||
        containsCI(name.c_str(), L"sanal dokunmatik") || containsCI(name.c_str(), L"virtual touchpad") ||
        containsCI(name.c_str(), L"kalem menüsü") || containsCI(name.c_str(), L"pen menu") ||
        containsCI(name.c_str(), L"mikrofonunuzu kullanıyor") || containsCI(name.c_str(), L"using your microphone") ||
        containsCI(name.c_str(), L"konumunuzu kullanıyor") || containsCI(name.c_str(), L"using your location"))
        return true;
    return false;
}

std::vector<OverflowButton> collectOverflowButtonsWithUia(IUIAutomation* automation, HWND overflow, bool allowHiddenBounds = false) {
    std::vector<OverflowButton> items;
    if (!overflow || !automation) return items;

    auto cacheReq = createOverflowButtonCacheReq(automation);
    if (!cacheReq) return items;

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, &cond)) || !cond)
        return items;

    const int dpi = std::max<int>(96, static_cast<int>(GetDpiForWindow(overflow)));
    const int maxDim = std::max(160, MulDiv(160, dpi, 96));

    // Query the XAML DesktopWindowContentBridge directly first; fall back to overflow/children only if empty.
    std::vector<HWND> primary;
    if (const HWND bridge = FindWindowExW(overflow, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr))
        primary.push_back(bridge);
    else
        primary.push_back(overflow);

    int unnamedCounter = 0;
    const auto scanTargets = [&](const std::vector<HWND>& targets) {
        for (HWND target : targets) {
            ComPtr<IUIAutomationElement> root;
            if (FAILED(automation->ElementFromHandle(target, &root)) || !root) continue;
            ComPtr<IUIAutomationElementArray> buttons;
            if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cacheReq.Get(), &buttons)) || !buttons) continue;

            int count = 0;
            buttons->get_Length(&count);
            for (int i = 0; i < count; ++i) {
                ComPtr<IUIAutomationElement> el;
                if (FAILED(buttons->GetElement(i, &el)) || !el) continue;
                RECT rc{};
                const bool hasRect = SUCCEEDED(el->get_CachedBoundingRectangle(&rc)) && rc.right > rc.left && rc.bottom > rc.top;
                if (!hasRect && !allowHiddenBounds) continue;
                const int rw = rc.right - rc.left, rh = rc.bottom - rc.top;
                if (hasRect && (rw < 12 || rh < 12 || rw > maxDim || rh > maxDim)) continue;

                BSTR nameBstr{};
                el->get_CachedName(&nameBstr);
                Text name = nameBstr ? nameBstr : L"";
                if (nameBstr) SysFreeString(nameBstr);
                if (name.empty()) name = readCachedStringProp(el.Get(), UIA_LegacyIAccessibleNamePropertyId);
                if (name.empty()) name = readCachedStringProp(el.Get(), UIA_HelpTextPropertyId);

                const Text autoId = readCachedStringProp(el.Get(), UIA_AutomationIdPropertyId);
                const Text cls = readCachedStringProp(el.Get(), UIA_ClassNamePropertyId);
                if (name.empty()) {
                    if (!hasRect) continue;
                    const bool looksTrayBtn = containsCI(cls.c_str(), L"SystemTray") ||
                        containsCI(cls.c_str(), L"NotifyIcon") ||
                        containsCI(autoId.c_str(), L"SystemTrayIcon") ||
                        containsCI(autoId.c_str(), L"NotifyIcon") ||
                        std::abs(rw - rh) <= 24;
                    if (!looksTrayBtn) continue;
                    name = L"Bildirim simgesi " + std::to_wstring(++unnamedCounter);
                } else if (!hasRect) {
                    const bool looksTrayBtn = containsCI(cls.c_str(), L"SystemTray") ||
                        containsCI(cls.c_str(), L"NotifyIcon") ||
                        containsCI(autoId.c_str(), L"SystemTrayIcon") ||
                        containsCI(autoId.c_str(), L"NotifyIcon");
                    if (!looksTrayBtn) continue;
                }

                std::vector<int> rid = readCachedRuntimeId(el.Get());
                const bool duplicate = std::any_of(items.begin(), items.end(), [&](const OverflowButton& existing) {
                    if (!rid.empty() && !existing.runtimeId.empty()) return existing.runtimeId == rid;
                    if (!hasRect) return existing.name == name;
                    return existing.name == name &&
                        std::abs(existing.rect.left - rc.left) <= 8 &&
                        std::abs(existing.rect.top - rc.top) <= 8;
                });
                if (!duplicate) items.push_back({std::move(name), rc, std::move(rid), el, false});
            }
        }
    };

    scanTargets(primary);
    if (items.empty()) {
        std::vector<HWND> fallback;
        if (primary.front() != overflow) fallback.push_back(overflow);
        EnumChildWindows(overflow, [](HWND child, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(child);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&fallback));
        scanTargets(fallback);
    }

    std::stable_sort(items.begin(), items.end(), [](const OverflowButton& a, const OverflowButton& b) {
        if (std::abs(a.rect.top - b.rect.top) > 12) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
    return items;
}

std::vector<OverflowButton> collectPromotedTaskbarButtonsWithUia(IUIAutomation* automation, HWND taskbar) {
    std::vector<OverflowButton> items;
    if (!taskbar || !automation) return items;

    auto cacheReq = createOverflowButtonCacheReq(automation);
    if (!cacheReq) return items;

    VARIANT buttonType{};
    buttonType.vt = VT_I4;
    buttonType.lVal = UIA_ButtonControlTypeId;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, buttonType, &cond)) || !cond)
        return items;

    std::vector<HWND> roots;
    EnumChildWindows(taskbar, [](HWND child, LPARAM lp) -> BOOL {
        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, ARRAYSIZE(cls)) > 0 &&
            (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
             wcscmp(cls, L"TrayNotifyWnd") == 0)) {
            reinterpret_cast<std::vector<HWND>*>(lp)->push_back(child);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&roots));
    if (roots.empty()) roots.push_back(taskbar);

    LONG chevronLeft = -1;
    LONG omniLeft = 0x7FFFFFFF;
    struct Candidate {
        Text name;
        Text autoId;
        Text cls;
        RECT rc{};
        std::vector<int> rid;
        ComPtr<IUIAutomationElement> el;
    };
    std::vector<Candidate> candidates;

    for (HWND target : roots) {
        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(target, &root)) || !root) continue;
        ComPtr<IUIAutomationElementArray> buttons;
        if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cacheReq.Get(), &buttons)) || !buttons) continue;

        int count = 0;
        buttons->get_Length(&count);
        for (int i = 0; i < count; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(buttons->GetElement(i, &el)) || !el) continue;

            RECT rc{};
            el->get_CachedBoundingRectangle(&rc);
            BSTR nameBstr{};
            el->get_CachedName(&nameBstr);
            Text name = nameBstr ? nameBstr : L"";
            if (nameBstr) SysFreeString(nameBstr);
            if (name.empty()) name = readCachedStringProp(el.Get(), UIA_LegacyIAccessibleNamePropertyId);
            const Text autoId = readCachedStringProp(el.Get(), UIA_AutomationIdPropertyId);
            const Text cls = readCachedStringProp(el.Get(), UIA_ClassNamePropertyId);

            const bool hasBounds = rc.right > rc.left && rc.bottom > rc.top;
            if (hasBounds) {
                if (isChevronText(name.c_str()) || isChevronText(autoId.c_str()) ||
                    wcscmp(cls.c_str(), L"SystemTray.ChevronButton") == 0) {
                    chevronLeft = std::max(chevronLeft, rc.left);
                } else if (containsCI(cls.c_str(), L"OmniButton") ||
                           containsCI(cls.c_str(), L"Language") ||
                           containsCI(autoId.c_str(), L"ControlCenter") ||
                           containsCI(autoId.c_str(), L"Clock")) {
                    omniLeft = std::min(omniLeft, rc.left);
                }
            }

            const bool isNormalTray = wcscmp(cls.c_str(), L"SystemTray.NormalButton") == 0 &&
                (wcscmp(autoId.c_str(), L"SystemTrayIcon") == 0 ||
                 containsCI(autoId.c_str(), L"NotifyIcon"));
            if (!isNormalTray) continue;
            if (isBuiltInTaskbarSystemButton(name, autoId, cls)) continue;
            if (name.empty()) continue;

            candidates.push_back({std::move(name), autoId, cls, rc, readCachedRuntimeId(el.Get()), el});
        }
    }

    for (auto& c : candidates) {
        const bool hasBounds = c.rc.right > c.rc.left && c.rc.bottom > c.rc.top;
        if (hasBounds) {
            if (chevronLeft >= 0 && c.rc.left < chevronLeft - 4) continue;
            if (omniLeft < 0x7FFFFFFF && c.rc.left >= omniLeft - 2) continue;
        }
        const bool duplicate = std::any_of(items.begin(), items.end(), [&](const OverflowButton& existing) {
            if (!c.rid.empty() && !existing.runtimeId.empty()) return existing.runtimeId == c.rid;
            return existing.name == c.name;
        });
        if (!duplicate) {
            items.push_back({std::move(c.name), c.rc, std::move(c.rid), std::move(c.el), true});
        }
    }

    std::sort(items.begin(), items.end(), [](const OverflowButton& a, const OverflowButton& b) {
        return a.rect.left < b.rect.left;
    });
    return items;
}

std::vector<OverflowButton> collectOverflowButtons(HWND overflow) {
    return collectOverflowButtonsWithUia(sharedUia(), overflow);
}

ComPtr<IUIAutomationScrollPattern> findOverflowVerticalScroller(IUIAutomation* automation, HWND overflow) {
    if (!automation || !overflow) return nullptr;
    VARIANT trueVal{};
    trueVal.vt = VT_BOOL;
    trueVal.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(automation->CreatePropertyCondition(UIA_IsScrollPatternAvailablePropertyId, trueVal, &cond)) || !cond)
        return nullptr;

    std::vector<HWND> roots;
    if (const HWND bridge = FindWindowExW(overflow, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr))
        roots.push_back(bridge);
    roots.push_back(overflow);

    for (HWND target : roots) {
        ComPtr<IUIAutomationElement> root;
        if (FAILED(automation->ElementFromHandle(target, &root)) || !root) continue;
        ComPtr<IUIAutomationElementArray> scrollers;
        if (FAILED(root->FindAll(TreeScope_Descendants, cond.Get(), &scrollers)) || !scrollers) continue;
        int n = 0;
        scrollers->get_Length(&n);
        for (int i = 0; i < n; ++i) {
            ComPtr<IUIAutomationElement> el;
            if (FAILED(scrollers->GetElement(i, &el)) || !el) continue;
            ComPtr<IUIAutomationScrollPattern> sp;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp))) && sp) {
                BOOL vScrollable = FALSE;
                if (SUCCEEDED(sp->get_CurrentVerticallyScrollable(&vScrollable)) && vScrollable)
                    return sp;
            }
        }
    }
    return nullptr;
}

int channelDist(DWORD a, DWORD b) {
    const int dr = std::abs(static_cast<int>((a >> 16) & 0xFF) - static_cast<int>((b >> 16) & 0xFF));
    const int dg = std::abs(static_cast<int>((a >> 8) & 0xFF) - static_cast<int>((b >> 8) & 0xFF));
    const int db = std::abs(static_cast<int>(a & 0xFF) - static_cast<int>(b & 0xFF));
    return std::max({dr, dg, db});
}

bool captureOverflowIcons(HWND overflow, const std::vector<OverflowButton>& buttons,
                          std::vector<std::shared_ptr<Gdiplus::Bitmap>>& outIcons) {
    outIcons.assign(buttons.size(), nullptr);
    RECT ovRect{};
    if (!GetWindowRect(overflow, &ovRect)) return false;
    const int ovW = ovRect.right - ovRect.left;
    const int ovH = ovRect.bottom - ovRect.top;
    if (ovW <= 0 || ovH <= 0) return false;

    HDC memDC = CreateCompatibleDC(nullptr);
    if (!memDC) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ovW;
    bi.bmiHeader.biHeight = -ovH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    if (!dib || !pixels) {
        if (dib) DeleteObject(dib);
        DeleteDC(memDC);
        return false;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, dib);
    std::fill_n(pixels, static_cast<size_t>(ovW) * ovH, 0u);
    constexpr UINT renderFullContent = 0x00000002;
    if (!PrintWindow(overflow, memDC, renderFullContent))
        PrintWindow(overflow, memDC, 0);
    GdiFlush();

    bool anyNonBlank = false;
    for (size_t idx = 0; idx < buttons.size(); ++idx) {
        const RECT& rc = buttons[idx].rect;
        const int rawBx = std::max<int>(0, rc.left - ovRect.left);
        const int rawBy = std::max<int>(0, rc.top - ovRect.top);
        const int rawBr = std::min<int>(ovW, rc.right - ovRect.left);
        const int rawBb = std::min<int>(ovH, rc.bottom - ovRect.top);
        const int rawBw = rawBr - rawBx;
        const int rawBh = rawBb - rawBy;
        if (rawBw < 12 || rawBh < 12) continue;

        // Inset more at the bottom to exclude Windows 11's keyboard focus underline inside the button cell.
        const int pad = std::max(3, std::min(rawBw, rawBh) / 10);
        const int padBot = std::max(pad + 2, rawBh / 7);
        const int bx = rawBx + pad;
        const int by = rawBy + pad;
        const int br = rawBr - pad;
        const int bb = rawBb - padBot;
        const int bw = br - bx;
        const int bh = bb - by;
        if (bw < 8 || bh < 8) continue;

        auto pxAt = [&](int x, int y) -> DWORD {
            return pixels[static_cast<size_t>(by + y) * ovW + (bx + x)];
        };

        const POINT corners[] = {{0, 0}, {bw - 1, 0}, {0, bh - 1}, {bw - 1, bh - 1}};
        DWORD bg = 0;
        for (POINT c : corners) {
            const DWORD p = pxAt(c.x, c.y);
            if ((p & 0x00FFFFFFu) != 0 || (p >> 24) != 0) { bg = p; break; }
        }

        bool cellHasAlpha = false;
        bool cellVaries = false;
        const DWORD firstPx = pxAt(0, 0);
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                const DWORD p = pxAt(x, y);
                if ((p >> 24) != 0) cellHasAlpha = true;
                if (channelDist(p, firstPx) > 4) cellVaries = true;
            }
        }
        if (!cellVaries) continue;

        auto isBg = [&](DWORD p) -> bool {
            if (cellHasAlpha && (p >> 24) <= 12) return true;
            return channelDist(p, bg) <= 14;
        };

        std::vector<uint8_t> bgMask(static_cast<size_t>(bw) * bh, 0);
        std::queue<int> q;
        auto pushIfBg = [&](int x, int y) {
            const size_t i = static_cast<size_t>(y) * bw + x;
            if (!bgMask[i] && isBg(pxAt(x, y))) {
                bgMask[i] = 1;
                q.push(static_cast<int>(i));
            }
        };
        for (int x = 0; x < bw; ++x) { pushIfBg(x, 0); pushIfBg(x, bh - 1); }
        for (int y = 1; y < bh - 1; ++y) { pushIfBg(0, y); pushIfBg(bw - 1, y); }
        while (!q.empty()) {
            const int cur = q.front();
            q.pop();
            const int cx = cur % bw, cy = cur / bw;
            if (cx > 0) pushIfBg(cx - 1, cy);
            if (cx + 1 < bw) pushIfBg(cx + 1, cy);
            if (cy > 0) pushIfBg(cx, cy - 1);
            if (cy + 1 < bh) pushIfBg(cx, cy + 1);
        }

        // Remove isolated 1-2px focus bars separated vertically from the main icon body.
        std::vector<int> rowCount(bh, 0);
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x)
                if (!bgMask[static_cast<size_t>(y) * bw + x]) ++rowCount[y];
        int bestBandStart = 0, bestBandEnd = bh - 1, bestBandArea = -1;
        for (int y = 0; y < bh;) {
            while (y < bh && rowCount[y] == 0) ++y;
            if (y >= bh) break;
            const int s = y;
            int area = 0;
            while (y < bh && rowCount[y] > 0) { area += rowCount[y]; ++y; }
            if (area > bestBandArea) { bestBandArea = area; bestBandStart = s; bestBandEnd = y - 1; }
        }
        if (bestBandArea > 0) {
            for (int y = 0; y < bh; ++y) {
                if (y < bestBandStart - 1 || y > bestBandEnd + 1) {
                    for (int x = 0; x < bw; ++x) bgMask[static_cast<size_t>(y) * bw + x] = 1;
                }
            }
        }

        int minX = bw, minY = bh, maxX = -1, maxY = -1;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                if (!bgMask[static_cast<size_t>(y) * bw + x]) {
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
            }
        }
        if (maxX < minX || maxY < minY) continue;

        anyNonBlank = true;
        const int fgW = maxX - minX + 1;
        const int fgH = maxY - minY + 1;
        const int side = std::min(std::max(bw, bh), std::max({fgW + 2, fgH + 2, bw / 2}));
        const int cropX = (minX + maxX + 1) / 2 - side / 2;
        const int cropY = (minY + maxY + 1) / 2 - side / 2;

        auto bmp = std::make_unique<Gdiplus::Bitmap>(side, side, PixelFormat32bppPARGB);
        Gdiplus::BitmapData data{};
        Gdiplus::Rect lockRect(0, 0, side, side);
        if (bmp->LockBits(&lockRect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
            for (int y = 0; y < side; ++y) {
                auto* dst = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.Scan0) + y * data.Stride);
                const int sy = cropY + y;
                for (int x = 0; x < side; ++x) {
                    const int sx = cropX + x;
                    if (sx < 0 || sy < 0 || sx >= bw || sy >= bh || bgMask[static_cast<size_t>(sy) * bw + sx]) {
                        dst[x] = 0;
                    } else {
                        const DWORD px = pxAt(sx, sy);
                        if (cellHasAlpha) {
                            const unsigned a = (px >> 24) & 0xFFu;
                            const unsigned r = std::min<unsigned>(a, (px >> 16) & 0xFFu);
                            const unsigned g = std::min<unsigned>(a, (px >> 8) & 0xFFu);
                            const unsigned b = std::min<unsigned>(a, px & 0xFFu);
                            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
                        } else {
                            dst[x] = px | 0xFF000000u;
                        }
                    }
                }
            }
            bmp->UnlockBits(&data);
            outIcons[idx] = normalizeAppIcon(*bmp);
        }
    }

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    return anyNonBlank;
}

void appendClassicToolbarIcons(HWND toolbar, bool onTaskbar, const std::vector<NotifyIconEntry>& regEntries,
                               std::vector<TrayItem>& items) {
    if (!toolbar) return;
    const int count = static_cast<int>(SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0));
    if (count <= 0) return;

    DWORD pid = 0;
    GetWindowThreadProcessId(toolbar, &pid);
    if (!pid) return;

    const HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process) return;

    constexpr SIZE_T bufBytes = 4096;
    void* remote = VirtualAllocEx(process, nullptr, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return;
    }

    struct TrayData64 {
        HWND hwnd;
        UINT uID;
        UINT uCallbackMessage;
        DWORD reserved[2];
        HICON hIcon;
    };

    for (int i = 0; i < count; ++i) {
        if (!SendMessageW(toolbar, TB_GETBUTTON, i, reinterpret_cast<LPARAM>(remote))) continue;
        TBBUTTON btn{};
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, remote, &btn, sizeof(btn), &read) || read != sizeof(btn)) continue;
        if ((btn.fsState & TBSTATE_HIDDEN) || (btn.fsStyle & BTNS_SEP) || !btn.dwData) continue;

        TrayData64 td{};
        if (!ReadProcessMemory(process, reinterpret_cast<const void*>(btn.dwData), &td, sizeof(td), &read) ||
            read != sizeof(td) || !IsWindow(td.hwnd))
            continue;

        const bool dup = std::any_of(items.begin(), items.end(), [&](const TrayItem& existing) {
            return existing.targetHwnd == td.hwnd && existing.uid == td.uID;
        });
        if (dup) continue;

        wchar_t tipBuf[512]{};
        const LRESULT textLen = SendMessageW(toolbar, TB_GETBUTTONTEXTW, btn.idCommand, reinterpret_cast<LPARAM>(remote));
        if (textLen > 0 && textLen < ARRAYSIZE(tipBuf)) {
            ReadProcessMemory(process, remote, tipBuf, static_cast<SIZE_T>(textLen) * sizeof(wchar_t), &read);
        }

        const Text name = tipBuf[0] ? Text(tipBuf) : Text(L"Uygulama");
        std::shared_ptr<Gdiplus::Bitmap> rawIcon;
        if (td.hIcon) {
            if (HICON copy = CopyIcon(td.hIcon)) {
                rawIcon = bitmapFromHIcon(copy);
                DestroyIcon(copy);
            }
        }
        auto icon = resolveCrispTrayIcon(name, nullptr, regEntries, rawIcon);
        items.push_back({name, std::move(icon), td.hwnd, td.uID, td.uCallbackMessage, onTaskbar});
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
}

bool tryCaptureClassicTray(std::vector<TrayItem>& out, bool forceReload = false) {
    const HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    const HWND overflowBar = overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr;
    HWND promotedBar = nullptr;
    if (const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        if (const HWND trayNotify = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr)) {
            if (const HWND sysPager = FindWindowExW(trayNotify, nullptr, L"SysPager", nullptr)) {
                promotedBar = FindWindowExW(sysPager, nullptr, L"ToolbarWindow32", nullptr);
            }
        }
    }
    if (!overflowBar && !promotedBar) return false;

    const auto regEntries = cachedNotifyIconRegistry(forceReload);
    std::vector<TrayItem> items;
    appendClassicToolbarIcons(overflowBar, false, regEntries, items);
    appendClassicToolbarIcons(promotedBar, true, regEntries, items);
    if (items.empty()) return false;
    out = std::move(items);
    return true;
}

ComPtr<IUIAutomationElement> matchOverflowButton(const std::vector<OverflowButton>& buttons,
                                                 const Text& targetName, int fallbackIndex, RECT& outRect) {
    for (const auto& b : buttons) {
        if (b.name == targetName) {
            outRect = b.rect;
            return b.element;
        }
    }
    const Text targetFirst = firstWordLower(targetName);
    if (targetFirst.size() >= 4) {
        for (const auto& b : buttons) {
            if (firstWordLower(b.name) == targetFirst) {
                outRect = b.rect;
                return b.element;
            }
        }
    }
    if (fallbackIndex >= 0 && fallbackIndex < static_cast<int>(buttons.size())) {
        outRect = buttons[fallbackIndex].rect;
        return buttons[fallbackIndex].element;
    }
    return nullptr;
}

Text cleanMenuLabel(const Text& raw) {
    Text out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t ch = raw[i];
        if (ch == L'\t') break;
        if (ch == L'&') {
            if (i + 1 < raw.size() && raw[i + 1] == L'&') {
                out.push_back(L'&');
                ++i;
            }
            continue;
        }
        if (ch == L'\r' || ch == L'\n') out.push_back(L' ');
        else out.push_back(ch);
    }
    size_t start = 0;
    while (start < out.size() && std::iswspace(out[start])) ++start;
    size_t end = out.size();
    while (end > start && std::iswspace(out[end - 1])) --end;
    return out.substr(start, end - start);
}

ComPtr<IAccessible> accessiblePopup(HWND menuHwnd) {
    ComPtr<IAccessible> acc;
    if (!menuHwnd || !IsWindow(menuHwnd)) return acc;
    for (const DWORD object : {static_cast<DWORD>(OBJID_CLIENT), static_cast<DWORD>(OBJID_MENU)}) {
        if (SUCCEEDED(AccessibleObjectFromWindow(menuHwnd, object, IID_IAccessible,
            reinterpret_cast<void**>(acc.ReleaseAndGetAddressOf()))) && acc)
            return acc;
    }
    return nullptr;
}

struct MsaaMenuEntry {
    Text name;
    LONG child = 0;
    LONG role = 0;
    bool separator = false;
    bool disabled = false;
    bool checked = false;
    bool isDefault = false;
    bool hasSubmenu = false;
    RECT rect{};
};

std::vector<MsaaMenuEntry> collectMsaaMenuEntries(HWND menuHwnd) {
    std::vector<MsaaMenuEntry> entries;
    const auto acc = accessiblePopup(menuHwnd);
    if (!acc) return entries;

    LONG count = 0;
    if (FAILED(acc->get_accChildCount(&count)) || count <= 0) return entries;
    count = std::min<LONG>(count, 80);

    for (LONG i = 1; i <= count; ++i) {
        VARIANT varChild{};
        varChild.vt = VT_I4;
        varChild.lVal = i;

        VARIANT varRole{};
        VariantInit(&varRole);
        LONG role = 0;
        if (SUCCEEDED(acc->get_accRole(varChild, &varRole)) && varRole.vt == VT_I4) role = varRole.lVal;
        VariantClear(&varRole);

        VARIANT varState{};
        VariantInit(&varState);
        LONG state = 0;
        if (SUCCEEDED(acc->get_accState(varChild, &varState)) && varState.vt == VT_I4) state = varState.lVal;
        VariantClear(&varState);
        if (state & STATE_SYSTEM_INVISIBLE) continue;

        BSTR nameBstr = nullptr;
        acc->get_accName(varChild, &nameBstr);
        Text name = nameBstr ? nameBstr : L"";
        if (nameBstr) SysFreeString(nameBstr);

        long l = 0, t = 0, w = 0, h = 0;
        acc->accLocation(&l, &t, &w, &h, varChild);

        MsaaMenuEntry entry{};
        entry.name = std::move(name);
        entry.child = i;
        entry.role = role;
        entry.separator = (role == ROLE_SYSTEM_SEPARATOR);
        entry.disabled = (state & STATE_SYSTEM_UNAVAILABLE) != 0;
        entry.checked = (state & STATE_SYSTEM_CHECKED) != 0;
        entry.isDefault = (state & STATE_SYSTEM_DEFAULT) != 0;
        entry.hasSubmenu = (state & STATE_SYSTEM_HASPOPUP) != 0;
        entry.rect = {l, t, l + w, t + h};
        entries.push_back(std::move(entry));
    }
    return entries;
}

struct UiaMenuEntry {
    Text name;
    CONTROLTYPEID ctrlType = 0;
    bool separator = false;
    bool enabled = true;
    bool checked = false;
    bool hasSubmenu = false;
    RECT rect{};
    ComPtr<IUIAutomationElement> element;
};

std::vector<UiaMenuEntry> collectUiaMenuEntries(HWND menuHwnd, bool menuItemsOnly = false,
                                                IUIAutomationElement* customRoot = nullptr) {
    std::vector<UiaMenuEntry> entries;
    IUIAutomation* automation = sharedUia();
    IUIAutomationCacheRequest* cacheReq = popupMenuCacheReq(automation);
    if (!automation || !cacheReq) return entries;

    ComPtr<IUIAutomationElement> root = customRoot;
    if (!root) {
        if (!menuHwnd || !IsWindow(menuHwnd)) return entries;
        if (FAILED(automation->ElementFromHandle(menuHwnd, &root)) || !root) return entries;
    }

    RECT winRect{};
    if (menuHwnd && IsWindow(menuHwnd)) GetWindowRect(menuHwnd, &winRect);

    ComPtr<IUIAutomationCondition> trueCond;
    if (FAILED(automation->CreateTrueCondition(&trueCond)) || !trueCond) return entries;

    ComPtr<IUIAutomationElementArray> all;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, trueCond.Get(), cacheReq, &all)) || !all)
        return entries;

    int count = 0;
    all->get_Length(&count);
    const int limit = std::min(count, 280);

    struct TextSpan {
        Text text;
        RECT rect{};
    };
    struct RawNode {
        CONTROLTYPEID type = 0;
        Text name;
        RECT rect{};
        bool enabled = true;
        bool checked = false;
        bool hasSub = false;
        ComPtr<IUIAutomationElement> el;
    };

    std::vector<TextSpan> textSpans;
    std::vector<RECT> excludedContainerRects;
    std::vector<RawNode> menuNodes;
    std::vector<RawNode> generalNodes;
    std::vector<RawNode> customInvokableNodes;
    std::vector<UiaMenuEntry> separators;

    const auto cachedStringProp = [](IUIAutomationElement* el, PROPERTYID prop) -> Text {
        VARIANT v{};
        VariantInit(&v);
        Text out;
        if (SUCCEEDED(el->GetCachedPropertyValue(prop, &v)) && v.vt == VT_BSTR && v.bstrVal) {
            out = v.bstrVal;
        }
        VariantClear(&v);
        return out;
    };

    const auto cachedBoolProp = [](IUIAutomationElement* el, PROPERTYID prop) -> bool {
        VARIANT v{};
        VariantInit(&v);
        bool out = false;
        if (SUCCEEDED(el->GetCachedPropertyValue(prop, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE) {
            out = true;
        }
        VariantClear(&v);
        return out;
    };

    for (int i = 0; i < limit; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(all->GetElement(i, &el)) || !el) continue;

        CONTROLTYPEID ctrlType = 0;
        if (FAILED(el->get_CachedControlType(&ctrlType))) continue;

        RECT rc{};
        el->get_CachedBoundingRectangle(&rc);
        if (IsRectEmpty(&rc)) continue;

        BOOL offscreen = FALSE;
        el->get_CachedIsOffscreen(&offscreen);
        if (offscreen && !IsRectEmpty(&winRect)) {
            RECT inter{};
            if (!IntersectRect(&inter, &rc, &winRect)) continue;
        }

        BSTR nameBstr{};
        el->get_CachedName(&nameBstr);
        Text name = nameBstr ? nameBstr : L"";
        if (nameBstr) SysFreeString(nameBstr);
        if (name.empty()) name = cachedStringProp(el.Get(), UIA_LegacyIAccessibleNamePropertyId);

        if (ctrlType == UIA_TextControlTypeId) {
            if (!name.empty()) textSpans.push_back({name, rc});
            // Some WebView2 / WinUI menus expose clickable Text nodes with InvokePattern.
            if (!menuItemsOnly && !name.empty() &&
                (cachedBoolProp(el.Get(), UIA_IsInvokePatternAvailablePropertyId) ||
                 cachedBoolProp(el.Get(), UIA_IsTogglePatternAvailablePropertyId))) {
                BOOL enabled = TRUE;
                el->get_CachedIsEnabled(&enabled);
                customInvokableNodes.push_back({ctrlType, std::move(name), rc, enabled != FALSE, false, false, el});
            }
            continue;
        }

        if (ctrlType == UIA_SliderControlTypeId || ctrlType == UIA_ScrollBarControlTypeId) {
            excludedContainerRects.push_back(rc);
            continue;
        }

        if (ctrlType == UIA_SeparatorControlTypeId) {
            UiaMenuEntry sep{};
            sep.ctrlType = ctrlType;
            sep.separator = true;
            sep.rect = rc;
            separators.push_back(std::move(sep));
            continue;
        }

        const bool isMenuItem = (ctrlType == UIA_MenuItemControlTypeId);
        const bool isGeneralInteractive = !menuItemsOnly &&
            (ctrlType == UIA_ButtonControlTypeId || ctrlType == UIA_ListItemControlTypeId ||
             ctrlType == UIA_CheckBoxControlTypeId || ctrlType == UIA_RadioButtonControlTypeId ||
             ctrlType == UIA_SplitButtonControlTypeId || ctrlType == UIA_HyperlinkControlTypeId ||
             ctrlType == UIA_TreeItemControlTypeId || ctrlType == UIA_DataItemControlTypeId);
        const bool isCustomInvokable = !menuItemsOnly && !isMenuItem && !isGeneralInteractive &&
            (ctrlType == UIA_CustomControlTypeId || ctrlType == UIA_GroupControlTypeId || ctrlType == UIA_PaneControlTypeId) &&
            (cachedBoolProp(el.Get(), UIA_IsInvokePatternAvailablePropertyId) ||
             cachedBoolProp(el.Get(), UIA_IsTogglePatternAvailablePropertyId));
        if (!isMenuItem && !isGeneralInteractive && !isCustomInvokable) continue;

        BOOL enabled = TRUE;
        el->get_CachedIsEnabled(&enabled);

        bool checked = false;
        VARIANT vToggle{};
        VariantInit(&vToggle);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_ToggleToggleStatePropertyId, &vToggle)) &&
            vToggle.vt == VT_I4 && vToggle.lVal == ToggleState_On)
            checked = true;
        VariantClear(&vToggle);
        if (!checked && cachedBoolProp(el.Get(), UIA_SelectionItemIsSelectedPropertyId)) checked = true;

        bool hasSub = false;
        VARIANT vExp{};
        VariantInit(&vExp);
        if (SUCCEEDED(el->GetCachedPropertyValue(UIA_ExpandCollapseExpandCollapseStatePropertyId, &vExp)) &&
            vExp.vt == VT_I4 && vExp.lVal != ExpandCollapseState_LeafNode)
            hasSub = true;
        VariantClear(&vExp);
        if (!hasSub) {
            VARIANT vState{};
            VariantInit(&vState);
            if (SUCCEEDED(el->GetCachedPropertyValue(UIA_LegacyIAccessibleStatePropertyId, &vState)) &&
                vState.vt == VT_I4 && (vState.lVal & STATE_SYSTEM_HASPOPUP) != 0)
                hasSub = true;
            VariantClear(&vState);
        }

        RawNode node{ctrlType, std::move(name), rc, enabled != FALSE, checked, hasSub, el};
        if (isMenuItem) menuNodes.push_back(std::move(node));
        else if (isGeneralInteractive) generalNodes.push_back(std::move(node));
        else customInvokableNodes.push_back(std::move(node));
    }

    auto& chosen = !menuNodes.empty() ? menuNodes : (!generalNodes.empty() ? generalNodes : customInvokableNodes);
    if (menuNodes.empty() && !excludedContainerRects.empty()) {
        chosen.erase(std::remove_if(chosen.begin(), chosen.end(), [&](const RawNode& node) {
            if (node.type != UIA_ButtonControlTypeId && node.type != UIA_ThumbControlTypeId) return false;
            const int cx = (node.rect.left + node.rect.right) / 2;
            const int cy = (node.rect.top + node.rect.bottom) / 2;
            for (const RECT& container : excludedContainerRects) {
                if (cx >= container.left && cx <= container.right && cy >= container.top && cy <= container.bottom)
                    return true;
            }
            return false;
        }), chosen.end());
    }

    for (auto& node : chosen) {
        if (!node.name.empty()) continue;
        std::vector<const TextSpan*> inside;
        for (const auto& ts : textSpans) {
            const int cx = (ts.rect.left + ts.rect.right) / 2;
            const int cy = (ts.rect.top + ts.rect.bottom) / 2;
            if (cx >= node.rect.left - 2 && cx <= node.rect.right + 2 &&
                cy >= node.rect.top - 2 && cy <= node.rect.bottom + 2) {
                inside.push_back(&ts);
            }
        }
        std::stable_sort(inside.begin(), inside.end(), [](const TextSpan* a, const TextSpan* b) {
            if (std::abs(a->rect.top - b->rect.top) > 4) return a->rect.top < b->rect.top;
            return a->rect.left < b->rect.left;
        });
        for (const TextSpan* ts : inside) {
            const Text cleaned = cleanMenuLabel(ts->text);
            if (cleaned.empty()) continue;
            if (!node.name.empty()) {
                if (node.name == cleaned) continue;
                node.name += L" · ";
            }
            node.name += cleaned;
        }
    }

    const auto typePriority = [](CONTROLTYPEID t) {
        if (t == UIA_MenuItemControlTypeId) return 4;
        if (t == UIA_CheckBoxControlTypeId || t == UIA_RadioButtonControlTypeId) return 3;
        if (t == UIA_ButtonControlTypeId || t == UIA_SplitButtonControlTypeId || t == UIA_HyperlinkControlTypeId) return 2;
        return 1;
    };

    for (auto& node : chosen) {
        const Text cleaned = cleanMenuLabel(node.name);
        if (cleaned.empty()) continue;
        bool merged = false;
        for (auto& existing : entries) {
            if (existing.separator) continue;
            if (cleanMenuLabel(existing.name) != cleaned) continue;
            const int interTop = std::max(existing.rect.top, node.rect.top);
            const int interBot = std::min(existing.rect.bottom, node.rect.bottom);
            const int minH = std::max<int>(1, std::min<int>(existing.rect.bottom - existing.rect.top,
                node.rect.bottom - node.rect.top));
            if (interBot - interTop >= (minH * 6) / 10) {
                if (typePriority(node.type) > typePriority(existing.ctrlType)) {
                    existing.ctrlType = node.type;
                    existing.element = node.el;
                    existing.enabled = node.enabled;
                    existing.checked = node.checked;
                    existing.hasSubmenu = existing.hasSubmenu || node.hasSub;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            entries.push_back({std::move(node.name), node.type, false, node.enabled, node.checked, node.hasSub, node.rect, node.el});
        }
    }

    if (!entries.empty() && !separators.empty()) {
        for (auto& sep : separators) entries.push_back(std::move(sep));
    }

    std::stable_sort(entries.begin(), entries.end(), [](const UiaMenuEntry& a, const UiaMenuEntry& b) {
        if (std::abs(a.rect.top - b.rect.top) > 3) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
    return entries;
}

std::vector<UiaMenuEntry> collectUiaMenuEntriesDeep(HWND menuHwnd, bool menuItemsOnly = false,
                                                    IUIAutomationElement* customRoot = nullptr) {
    auto entries = collectUiaMenuEntries(menuHwnd, menuItemsOnly, customRoot);
    if (!entries.empty() || customRoot || !menuHwnd || !IsWindow(menuHwnd)) return entries;

    // Some WinUI 3 / WebView2 / Electron popups expose UIA items only under a hosted child bridge HWND.
    std::vector<HWND> children;
    EnumChildWindows(menuHwnd, [](HWND child, LPARAM lp) -> BOOL {
        if (IsWindowVisible(child)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&children));
    for (HWND child : children) {
        entries = collectUiaMenuEntries(child, menuItemsOnly, nullptr);
        if (!entries.empty()) return entries;
    }
    return {};
}

HWND findNewPopupForPid(DWORD ownerPid, const std::vector<HWND>& existingPopups) {
    struct Search {
        DWORD pid;
        const std::vector<HWND>* existing;
        HWND found{};
    } ctx{ownerPid, &existingPopups};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<Search*>(lp);
        if (!IsWindowVisible(w) || isWindowCloakedByDwm(w)) return TRUE;
        DWORD p = 0;
        GetWindowThreadProcessId(w, &p);
        if (p != s->pid) return TRUE;
        if (std::find(s->existing->begin(), s->existing->end(), w) != s->existing->end()) return TRUE;
        RECT rc{};
        if (!GetWindowRect(w, &rc) || rc.right - rc.left < 36 || rc.bottom - rc.top < 16) return TRUE;
        s->found = w;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

std::vector<HWND> snapshotVisibleForPid(DWORD ownerPid) {
    struct Gather {
        DWORD pid;
        std::vector<HWND> list;
    } ctx{ownerPid};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* g = reinterpret_cast<Gather*>(lp);
        if (!IsWindowVisible(w)) return TRUE;
        DWORD p = 0;
        GetWindowThreadProcessId(w, &p);
        if (p == g->pid) g->list.push_back(w);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.list;
}

std::vector<TrayMenuItem> readHMenuItems(HMENU hMenu, HWND menuHwnd, const std::vector<int>& parentPath, int depth = 0) {
    std::vector<TrayMenuItem> result;
    if (!hMenu || !IsMenu(hMenu) || depth > 5) return result;

    std::optional<std::vector<MsaaMenuEntry>> msaaEntries;
    std::optional<std::vector<UiaMenuEntry>> uiaEntries;
    const int count = GetMenuItemCount(hMenu);
    size_t uiaIdx = 0;

    for (int i = 0; i < count; ++i) {
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(hMenu, i, TRUE, &mii)) continue;

        if (mii.fType & MFT_SEPARATOR) {
            if (!result.empty() && !result.back().separator) {
                TrayMenuItem sep{};
                sep.separator = true;
                sep.rawMenuIndex = i;
                sep.parentPath = parentPath;
                result.push_back(std::move(sep));
            }
            continue;
        }

        wchar_t buf[256]{};
        GetMenuStringW(hMenu, i, buf, ARRAYSIZE(buf), MF_BYPOSITION);
        if (!buf[0]) {
            MENUITEMINFOW strMii{sizeof(strMii)};
            strMii.fMask = MIIM_STRING;
            strMii.dwTypeData = buf;
            strMii.cch = ARRAYSIZE(buf) - 1;
            GetMenuItemInfoW(hMenu, i, TRUE, &strMii);
        }
        Text raw = buf;
        if (raw.empty() && menuHwnd) {
            if (!msaaEntries) msaaEntries = collectMsaaMenuEntries(menuHwnd);
            const auto entry = std::find_if(msaaEntries->begin(), msaaEntries->end(),
                [i](const MsaaMenuEntry& row) { return row.child == i + 1; });
            if (entry != msaaEntries->end() && !entry->separator) raw = entry->name;
        }
        if (raw.empty() && menuHwnd) {
            if (!uiaEntries) uiaEntries = collectUiaMenuEntriesDeep(menuHwnd, true);
            size_t nonSep = 0;
            for (const auto& u : *uiaEntries) {
                if (u.separator) continue;
                if (nonSep == uiaIdx) { raw = u.name; break; }
                ++nonSep;
            }
        }
        ++uiaIdx;

        Text label = cleanMenuLabel(raw);
        if (label.empty()) continue;

        TrayMenuItem item{};
        item.label = std::move(label);
        item.rawName = std::move(raw);
        item.cmdId = mii.wID;
        item.rawMenuIndex = i;
        item.parentPath = parentPath;
        item.disabled = (mii.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        item.checked = (mii.fState & MFS_CHECKED) != 0;
        item.isDefault = (mii.fState & MFS_DEFAULT) != 0;
        item.hasSubmenu = (mii.hSubMenu != nullptr);

        if (mii.hSubMenu && IsMenu(mii.hSubMenu) && depth < 5) {
            std::vector<int> childPath = parentPath;
            childPath.push_back(i);
            item.children = readHMenuItems(mii.hSubMenu, nullptr, childPath, depth + 1);
            if (item.children.empty() && menuHwnd) {
                DWORD_PTR r1 = 0, r2 = 0;
                SendMessageTimeoutW(menuHwnd, 0x01E5 /*MN_SELECTITEM*/, i, 0, SMTO_ABORTIFHUNG, 100, &r1);
                SendMessageTimeoutW(menuHwnd, 0x01E3 /*MN_OPENHIERARCHY*/, 0, 0, SMTO_ABORTIFHUNG, 150, &r2);
                HWND subWnd = reinterpret_cast<HWND>(r2);
                if (!subWnd || !IsWindow(subWnd) || nativePopupHandle(subWnd) != mii.hSubMenu)
                    subWnd = findWin32MenuForHandle(mii.hSubMenu, menuHwnd);
                if (subWnd) cloakMenuWindow(subWnd);
                item.children = readHMenuItems(mii.hSubMenu, subWnd, childPath, depth + 1);
                if (subWnd) {
                    SendMessageTimeoutW(menuHwnd, 0x01E4 /*MN_CLOSEHIERARCHY*/, 0, 0, SMTO_ABORTIFHUNG, 100, &r1);
                }
            }
        }
        result.push_back(std::move(item));
    }
    while (!result.empty() && result.back().separator) result.pop_back();
    return result;
}

std::vector<TrayMenuItem> buildMonsterControlMenu() {
    DWORD fanMode = 1, bytes = sizeof(fanMode);
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OEM\\GamingCenter2\\MyFan3",
        L"FanMode", RRF_RT_REG_DWORD, nullptr, &fanMode, &bytes);

    std::vector<TrayMenuItem> result;
    TrayMenuItem launchItem{};
    launchItem.label = L"Enable Monster Control Center";
    launchItem.rawName = launchItem.label;
    launchItem.cmdId = 9001;
    result.push_back(std::move(launchItem));

    TrayMenuItem modeMenu{};
    modeMenu.label = L"Operating Mode";
    modeMenu.rawName = modeMenu.label;
    modeMenu.cmdId = 9000;
    modeMenu.rawMenuIndex = 1;
    modeMenu.hasSubmenu = true;
    const wchar_t* modes[3] = {L"Office Mode", L"Game Mode", L"Turbo Mode"};
    for (int m = 0; m < 3; ++m) {
        TrayMenuItem sub{};
        sub.label = modes[m];
        sub.rawName = modes[m];
        sub.cmdId = 9010 + m;
        sub.rawMenuIndex = m;
        sub.parentPath = {1};
        sub.checked = (static_cast<int>(fanMode) == m);
        modeMenu.children.push_back(std::move(sub));
    }
    result.push_back(std::move(modeMenu));

    TrayMenuItem exitItem{};
    exitItem.label = L"Exit";
    exitItem.rawName = exitItem.label;
    exitItem.cmdId = 9003;
    exitItem.rawMenuIndex = 2;
    result.push_back(std::move(exitItem));
    return result;
}

std::vector<TrayMenuItem> buildKasperskyMenu(const Text& appName) {
    std::vector<TrayMenuItem> result;
    if (containsCI(appName.c_str(), L"vpn")) {
        TrayMenuItem openItem{};
        openItem.label = L"Kaspersky VPN’i aç";
        openItem.rawName = openItem.label;
        openItem.cmdId = 9002;
        openItem.rawMenuIndex = -1;
        openItem.isDefault = true;
        result.push_back(std::move(openItem));
        return result;
    }

    if (containsCI(appName.c_str(), L"password")) {
        TrayMenuItem openItem{};
        openItem.label = L"Kaspersky Password Manager’ı aç";
        openItem.rawName = openItem.label;
        openItem.cmdId = 9002;
        openItem.rawMenuIndex = -1;
        openItem.isDefault = true;
        result.push_back(std::move(openItem));
        return result;
    }

    TrayMenuItem openItem{};
    openItem.label = L"Kaspersky’yi aç";
    openItem.rawName = openItem.label;
    openItem.cmdId = 9002;
    openItem.rawMenuIndex = -1;
    openItem.isDefault = true;
    result.push_back(std::move(openItem));

    TrayMenuItem secItem{};
    secItem.label = L"Windows Güvenliği";
    secItem.rawName = secItem.label;
    secItem.launchTarget = L"windowsdefender:";
    result.push_back(std::move(secItem));
    return result;
}

std::vector<TrayMenuItem> buildNvidiaTrayMenu(const Text& name) {
    if (!containsCI(name.c_str(), L"nvidia")) return {};

    std::vector<TrayMenuItem> items;
    PWSTR folder{};
    Text programFiles;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &folder))) {
        programFiles = folder;
        CoTaskMemFree(folder);
    }
    const auto addApp = [&](const wchar_t* label, const wchar_t* shellTarget, const wchar_t* installedExe) {
        Text target = shellTarget;
        ComPtr<IShellItem> app;
        if (FAILED(SHCreateItemFromParsingName(shellTarget, nullptr, IID_PPV_ARGS(&app)))) {
            if (programFiles.empty()) return;
            target = programFiles + installedExe;
            const DWORD attributes = GetFileAttributesW(target.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        }
        TrayMenuItem item{};
        item.label = label;
        item.launchTarget = std::move(target);
        item.isDefault = items.empty();
        items.push_back(std::move(item));
    };
    addApp(L"NVIDIA App", L"shell:AppsFolder\\com.nvidia.nvapp",
        L"\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe");
    addApp(L"NVIDIA Denetim Masası", L"shell:AppsFolder\\NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIACorp.NVIDIAControlPanel",
        L"\\NVIDIA Corporation\\Control Panel Client\\nvcplui.exe");
    return items;
}

std::vector<TrayMenuItem> buildFallbackTrayMenu(const Text& appName) {
    if (auto nv = buildNvidiaTrayMenu(appName); !nv.empty()) return nv;
    if (containsCI(appName.c_str(), L"monster")) return buildMonsterControlMenu();
    if (containsCI(appName.c_str(), L"kaspersky")) return buildKasperskyMenu(appName);
    return {};
}

std::vector<TrayMenuItem> convertMsaaEntries(const std::vector<MsaaMenuEntry>& msaaEntries, bool requireMenuRole) {
    std::vector<TrayMenuItem> result;
    for (int i = 0; i < static_cast<int>(msaaEntries.size()); ++i) {
        const auto& e = msaaEntries[i];
        if (requireMenuRole && e.role != ROLE_SYSTEM_MENUITEM && e.role != ROLE_SYSTEM_SEPARATOR) continue;
        if (e.separator) {
            if (!result.empty() && !result.back().separator) {
                TrayMenuItem sep{};
                sep.separator = true;
                sep.rawMenuIndex = i;
                result.push_back(std::move(sep));
            }
            continue;
        }
        Text label = cleanMenuLabel(e.name);
        if (label.empty()) continue;
        TrayMenuItem item{};
        item.label = std::move(label);
        item.rawName = e.name;
        item.rawMenuIndex = i;
        item.disabled = e.disabled;
        item.checked = e.checked;
        item.isDefault = e.isDefault;
        item.hasSubmenu = e.hasSubmenu;
        result.push_back(std::move(item));
    }
    while (!result.empty() && result.back().separator) result.pop_back();
    return result;
}

void dismissPopupMenu(HWND menuHwnd, bool async = false) {
    if (!menuHwnd || !IsWindow(menuHwnd)) return;
    // Keep the menu cloaked & offscreen UNTIL the target app actually hides it,
    // so it NEVER flashes on-screen right before Vetra's UI opens.
    cloakMenuWindow(menuHwnd);
    if (isWin32MenuClass(menuHwnd)) {
        DWORD_PTR r = 0;
        if (async) PostMessageW(menuHwnd, 0x01E6 /*MN_CANCELMENUS*/, 0, 0);
        else SendMessageTimeoutW(menuHwnd, 0x01E6 /*MN_CANCELMENUS*/, 0, 0, SMTO_ABORTIFHUNG, 50, &r);
        PostMessageW(menuHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(menuHwnd, WM_KEYUP, VK_ESCAPE, 0);
    } else {
        // Never send WM_CLOSE to non-#32768 tray popups (e.g. Logitech G HUB MainWindow).
        PostMessageW(menuHwnd, WM_CANCELMODE, 0, 0);
        PostMessageW(menuHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(menuHwnd, WM_KEYUP, VK_ESCAPE, 0);
    }
    if (!async) {
        ScopedHighResTimer hiRes;
        for (int wait = 0; wait < 8 && IsWindow(menuHwnd) && IsWindowVisible(menuHwnd); ++wait) {
            Sleep(2);
        }
    }
    // Restores immediately if hidden, or defers until !IsWindowVisible(menuHwnd).
    uncloakMenuWindow(menuHwnd, false, false);
}

bool tryExpandSubmenuNonCursor(HWND menuHwnd, IUIAutomationElement* element, ComPtr<IUIAutomationExpandCollapsePattern>& outExpand) {
    if (!element) return false;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&outExpand))) && outExpand &&
        SUCCEEDED(outExpand->Expand())) {
        return true;
    }
    ComPtr<IUIAutomationSelectionItemPattern> select;
    if (SUCCEEDED(element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&select))) && select &&
        SUCCEEDED(select->Select())) {
        if (menuHwnd && IsWindow(menuHwnd)) PostMessageW(menuHwnd, WM_KEYDOWN, VK_RIGHT, 0);
        return true;
    }
    element->SetFocus();
    if (menuHwnd && IsWindow(menuHwnd)) {
        PostMessageW(menuHwnd, WM_KEYDOWN, VK_RIGHT, 0);
        return true;
    }
    return false;
}

std::vector<TrayMenuItem> convertUiaEntries(HWND menuHwnd, const std::vector<UiaMenuEntry>& uiaEntries,
                                            const std::vector<Text>& parentLabels = {}, int depth = 0) {
    std::vector<TrayMenuItem> result;
    DWORD ownerPid = 0;
    if (menuHwnd && IsWindow(menuHwnd)) GetWindowThreadProcessId(menuHwnd, &ownerPid);
    int expandedSubmenus = 0;

    for (int i = 0; i < static_cast<int>(uiaEntries.size()); ++i) {
        const auto& u = uiaEntries[i];
        if (u.separator) {
            if (!result.empty() && !result.back().separator) {
                TrayMenuItem sep{};
                sep.separator = true;
                sep.rawMenuIndex = i;
                sep.parentLabels = parentLabels;
                result.push_back(std::move(sep));
            }
            continue;
        }
        Text label = cleanMenuLabel(u.name);
        if (label.empty()) continue;
        TrayMenuItem item{};
        item.label = std::move(label);
        item.rawName = u.name;
        item.rawMenuIndex = i;
        item.parentLabels = parentLabels;
        item.disabled = !u.enabled;
        item.checked = u.checked;
        item.hasSubmenu = u.hasSubmenu;

        if (u.hasSubmenu && u.enabled && depth < 2 && expandedSubmenus < 5 && u.element && ownerPid) {
            ++expandedSubmenus;
            const auto before = snapshotVisibleForPid(ownerPid);
            ComPtr<IUIAutomationExpandCollapsePattern> expand;
            if (tryExpandSubmenuNonCursor(menuHwnd, u.element.Get(), expand)) {
                ScopedHighResTimer hiRes;
                HWND subPopup = nullptr;
                std::vector<UiaMenuEntry> subEntries;
                for (int wait = 0; wait < 3; ++wait) {
                    subPopup = findNewPopupForPid(ownerPid, before);
                    if (subPopup) {
                        cloakMenuWindow(subPopup);
                        subEntries = collectUiaMenuEntriesDeep(subPopup, false);
                        if (!subEntries.empty()) break;
                    }
                    subEntries = collectUiaMenuEntriesDeep(menuHwnd, true, u.element.Get());
                    if (!subEntries.empty()) break;
                    Sleep(4);
                }
                if (!subEntries.empty()) {
                    std::vector<Text> nextParents = parentLabels;
                    nextParents.push_back(item.label);
                    item.children = convertUiaEntries(subPopup ? subPopup : menuHwnd, subEntries, nextParents, depth + 1);
                }
                if (expand) expand->Collapse();
                if (subPopup && IsWindowVisible(subPopup)) dismissPopupMenu(subPopup, true);
            }
        }
        result.push_back(std::move(item));
    }
    while (!result.empty() && result.back().separator) result.pop_back();
    return result;
}

std::vector<TrayMenuItem> extractPopupMenuItems(HWND menuHwnd) {
    if (!menuHwnd || !IsWindow(menuHwnd)) return {};

    // 1. Native Win32 HMENU (fast direct read + recursive multi-level submenus + owner-drawn fallback).
    if (const HMENU menu = nativePopupHandle(menuHwnd)) {
        auto items = readHMenuItems(menu, menuHwnd, {}, 0);
        if (!items.empty()) return items;
    }

    // 2. Single-pass cached UIA extraction (including hosted WinUI 3 / WebView2 / Electron child HWNDs).
    if (auto uiaEntries = collectUiaMenuEntriesDeep(menuHwnd, false); !uiaEntries.empty()) {
        auto items = convertUiaEntries(menuHwnd, uiaEntries);
        if (!items.empty()) return items;
    }

    // 3. MSAA fallback.
    if (auto msaaItems = convertMsaaEntries(collectMsaaMenuEntries(menuHwnd), false); !msaaItems.empty()) {
        return msaaItems;
    }
    return {};
}

bool waitPopupClosed(HWND popup, int maxMs) {
    ScopedHighResTimer hiRes;
    for (int elapsed = 0; elapsed < maxMs; elapsed += 2) {
        if (!IsWindow(popup) || !IsWindowVisible(popup)) return true;
        Sleep(2);
    }
    return !IsWindow(popup) || !IsWindowVisible(popup);
}

bool postClientMenuClick(HWND menuWindow, RECT row, bool activateOffscreen = false) {
    if (!menuWindow || !IsWindow(menuWindow) || IsRectEmpty(&row)) return false;
    // Remove empty GDI region so framework hit-testing sees the client area while still offscreen & alpha=0.
    restoreMenuRegionForClick(menuWindow);
    if (activateOffscreen) {
        DWORD pid = 0;
        GetWindowThreadProcessId(menuWindow, &pid);
        if (pid) AllowSetForegroundWindow(pid);
        forceForegroundWindow(menuWindow);
    }
    POINT pt{(row.left + row.right) / 2, (row.top + row.bottom) / 2};
    RECT winRc{};
    if (GetWindowRect(menuWindow, &winRc) && !PtInRect(&winRc, pt)) {
        RECT origRc{};
        if (cloakedOrigRect(menuWindow, origRc) && PtInRect(&origRc, pt)) {
            pt.x = winRc.left + (pt.x - origRc.left);
            pt.y = winRc.top + (pt.y - origRc.top);
        }
    }
    HWND target = menuWindow;
    POINT clientPt = pt;
    if (!ScreenToClient(menuWindow, &clientPt)) return false;
    if (HWND child = ChildWindowFromPointEx(menuWindow, clientPt, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED)) {
        if (child != menuWindow) {
            POINT childPt = pt;
            if (ScreenToClient(child, &childPt)) {
                target = child;
                clientPt = childPt;
            }
        }
    }
    const LPARAM lp = MAKELPARAM(clientPt.x, clientPt.y);
    ScopedHighResTimer hiRes;
    PostMessageW(target, WM_MOUSEMOVE, 0, lp);
    Sleep(6);
    PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(10);
    PostMessageW(target, WM_LBUTTONUP, 0, lp);
    return true;
}

bool labelsMatchCI(const Text& a, const Text& b) {
    if (a.empty() || b.empty()) return false;
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

int findMenuCommand(HMENU menu, HWND menuWindow, const TrayMenuItem& item) {
    const int count = GetMenuItemCount(menu);
    const auto usable = [&](int idx) {
        const auto state = GetMenuState(menu, idx, MF_BYPOSITION);
        return state != static_cast<UINT>(-1) && !(state & (MF_DISABLED | MF_GRAYED | MF_SEPARATOR));
    };

    int match = -1;
    for (int i = 0; i < count; ++i) {
        wchar_t raw[256]{};
        GetMenuStringW(menu, i, raw, ARRAYSIZE(raw), MF_BYPOSITION);
        if (cleanMenuLabel(raw) != item.label || item.label.empty() || !usable(i)) continue;
        if (match >= 0) { match = -1; break; }
        match = i;
    }
    if (match >= 0) return match;

    for (int i = 0; i < count; ++i) {
        wchar_t raw[256]{};
        GetMenuStringW(menu, i, raw, ARRAYSIZE(raw), MF_BYPOSITION);
        if (!labelsMatchCI(cleanMenuLabel(raw), item.label) || !usable(i)) continue;
        if (match >= 0) { match = -1; break; }
        match = i;
    }
    if (match >= 0) return match;

    if (item.cmdId != 0) {
        for (int i = 0; i < count; ++i) {
            MENUITEMINFOW mii{sizeof(mii)};
            mii.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STATE;
            if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
            if ((mii.fType & MFT_SEPARATOR) || (mii.fState & (MFS_DISABLED | MFS_GRAYED))) continue;
            if (mii.wID == item.cmdId) {
                if (match >= 0) { match = -1; break; }
                match = i;
            }
        }
        if (match >= 0) return match;
    }

    if (menuWindow && !item.label.empty()) {
        for (const auto& entry : collectMsaaMenuEntries(menuWindow)) {
            if (entry.separator || entry.disabled || !labelsMatchCI(cleanMenuLabel(entry.name), item.label)) continue;
            const int idx = static_cast<int>(entry.child) - 1;
            if (idx >= 0 && idx < count && usable(idx)) return idx;
        }
        size_t uiaNonSep = 0;
        for (const auto& entry : collectUiaMenuEntriesDeep(menuWindow, true)) {
            if (entry.separator) continue;
            if (entry.enabled && labelsMatchCI(cleanMenuLabel(entry.name), item.label)) {
                size_t posNonSep = 0;
                for (int i = 0; i < count; ++i) {
                    const auto st = GetMenuState(menu, i, MF_BYPOSITION);
                    if (st == static_cast<UINT>(-1) || (st & MF_SEPARATOR)) continue;
                    if (posNonSep == uiaNonSep && usable(i)) return i;
                    ++posNonSep;
                }
            }
            ++uiaNonSep;
        }
    }

    if (item.rawMenuIndex >= 0 && item.rawMenuIndex < count && usable(item.rawMenuIndex)) {
        wchar_t raw[256]{};
        if (GetMenuStringW(menu, item.rawMenuIndex, raw, ARRAYSIZE(raw), MF_BYPOSITION) == 0)
            return item.rawMenuIndex;
        const Text curClean = cleanMenuLabel(raw);
        if (!curClean.empty() && !item.label.empty() &&
            (toLowerText(curClean.c_str()).rfind(toLowerText(item.label.c_str()).substr(0, std::min<size_t>(4, item.label.size())), 0) == 0)) {
            return item.rawMenuIndex;
        }
    }
    return -1;
}

bool invokePopupMenuItem(HWND popup, const TrayMenuItem& item, const std::vector<TrayMenuItem>& rootItems) {
    if (!IsWindow(popup)) return false;
    if (HMENU menu = nativePopupHandle(popup)) {
        HWND menuWindow = popup;
        const std::vector<TrayMenuItem>* level = &rootItems;
        for (const int parentRaw : item.parentPath) {
            const auto parent = std::find_if(level->begin(), level->end(),
                [&](const TrayMenuItem& row) { return row.rawMenuIndex == parentRaw; });
            if (parent == level->end()) return false;
            const int index = findMenuCommand(menu, menuWindow, *parent);
            HMENU nextMenu = index >= 0 ? GetSubMenu(menu, index) : nullptr;
            if (!nextMenu) return false;
            DWORD_PTR unused{}, opened{};
            SendMessageTimeoutW(menuWindow, 0x01E5 /*MN_SELECTITEM*/, index, 0, SMTO_ABORTIFHUNG, 100, &unused);
            SendMessageTimeoutW(menuWindow, 0x01E3 /*MN_OPENHIERARCHY*/, 0, 0, SMTO_ABORTIFHUNG, 150, &opened);
            HWND nextWindow = reinterpret_cast<HWND>(opened);
            if (!IsWindow(nextWindow) || nativePopupHandle(nextWindow) != nextMenu)
                nextWindow = findWin32MenuForHandle(nextMenu, menuWindow);
            if (!nextWindow || nativePopupHandle(nextWindow) != nextMenu) return false;
            cloakMenuWindow(nextWindow);
            menu = nextMenu;
            menuWindow = nextWindow;
            level = &parent->children;
        }
        const int index = findMenuCommand(menu, menuWindow, item);
        if (index >= 0 && !GetSubMenu(menu, index)) {
            GUITHREADINFO gti{sizeof(gti)};
            const DWORD tid = GetWindowThreadProcessId(menuWindow, nullptr);
            HWND menuOwner = (tid && GetGUIThreadInfo(tid, &gti) && IsWindow(gti.hwndMenuOwner)) ? gti.hwndMenuOwner : nullptr;
            DWORD_PTR unused{};

            // Strategy 1: Synchronized MN_SELECTITEM + MN_BUTTONDOWN + asynchronous MN_BUTTONUP
            // (works reliably for both TrackPopupMenuEx(TPM_RETURNCMD) and TrackPopupMenu(WM_COMMAND)).
            SendMessageTimeoutW(menuWindow, 0x01E5 /*MN_SELECTITEM*/, index, 0, SMTO_ABORTIFHUNG, 80, &unused);
            SendMessageTimeoutW(menuWindow, 0x01ED /*MN_BUTTONDOWN*/, index, 0, SMTO_ABORTIFHUNG, 80, &unused);
            PostMessageW(menuWindow, 0x01EF /*MN_BUTTONUP*/, index, 0);
            if (waitPopupClosed(popup, 44)) return true;

            // Strategy 2: MN_SELECTITEM + VK_RETURN to menu owner (and menuWindow).
            SendMessageTimeoutW(menuWindow, 0x01E5 /*MN_SELECTITEM*/, index, 0, SMTO_ABORTIFHUNG, 80, &unused);
            HWND keyTarget = menuOwner ? menuOwner : menuWindow;
            PostMessageW(keyTarget, WM_KEYDOWN, VK_RETURN, 0x001C0001);
            PostMessageW(keyTarget, WM_KEYUP, VK_RETURN, 0xC01C0001);
            if (waitPopupClosed(popup, 36)) return true;
            if (keyTarget != menuWindow) {
                PostMessageW(menuWindow, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                PostMessageW(menuWindow, WM_KEYUP, VK_RETURN, 0xC01C0001);
                if (waitPopupClosed(popup, 30)) return true;
            }

            // Strategy 3: Direct WM_COMMAND to menuOwner if popup stayed open and cmdId is known.
            if (menuOwner && IsWindow(menuOwner) && item.cmdId != 0) {
                SendMessageTimeoutW(popup, 0x01E6 /*MN_CANCELMENUS*/, 0, 0, SMTO_ABORTIFHUNG, 60, &unused);
                waitPopupClosed(popup, 24);
                PostMessageW(menuOwner, WM_COMMAND, MAKEWPARAM(item.cmdId, 0), 0);
                return true;
            }
            return false;
        }
        if (menuWindow != popup) return false;
    }

    if (!item.parentPath.empty()) return false;

    HWND activePopup = popup;
    ComPtr<IUIAutomationElement> scopeRoot;
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(popup, &ownerPid);
    if (ownerPid) AllowSetForegroundWindow(ownerPid);

    for (const Text& parentLabel : item.parentLabels) {
        const auto curEntries = collectUiaMenuEntriesDeep(activePopup, false, scopeRoot.Get());
        const auto it = std::find_if(curEntries.begin(), curEntries.end(), [&](const UiaMenuEntry& e) {
            return !e.separator && e.enabled && labelsMatchCI(cleanMenuLabel(e.name), parentLabel);
        });
        if (it == curEntries.end() || !it->element) return false;
        const auto before = snapshotVisibleForPid(ownerPid);
        ComPtr<IUIAutomationExpandCollapsePattern> expand;
        if (!tryExpandSubmenuNonCursor(activePopup, it->element.Get(), expand)) return false;
        ScopedHighResTimer hiRes;
        HWND nextPopup = nullptr;
        for (int wait = 0; wait < 7; ++wait) {
            nextPopup = findNewPopupForPid(ownerPid, before);
            if (nextPopup) { cloakMenuWindow(nextPopup); break; }
            if (!collectUiaMenuEntriesDeep(activePopup, true, it->element.Get()).empty()) break;
            Sleep(6);
        }
        if (nextPopup) {
            activePopup = nextPopup;
            scopeRoot.Reset();
        } else {
            scopeRoot = it->element;
        }
    }

    // Remove empty GDI region on non-#32768 popups (Electron / WPF / WinForms / WinUI / Qt) before invoking,
    // while keeping them offscreen & alpha=0 so framework hit-tests and UIA actions succeed cleanly.
    restoreMenuRegionForClick(activePopup);

    const auto uiaList = collectUiaMenuEntriesDeep(activePopup, false, scopeRoot.Get());
    const UiaMenuEntry* matched = nullptr;
    for (const auto& entry : uiaList) {
        if (!entry.separator && entry.enabled && cleanMenuLabel(entry.name) == item.label) {
            matched = &entry;
            break;
        }
    }
    if (!matched) {
        for (const auto& entry : uiaList) {
            if (!entry.separator && entry.enabled && labelsMatchCI(cleanMenuLabel(entry.name), item.label)) {
                matched = &entry;
                break;
            }
        }
    }
    if (!matched && item.rawMenuIndex >= 0 && item.rawMenuIndex < static_cast<int>(uiaList.size())) {
        const auto& cand = uiaList[item.rawMenuIndex];
        if (!cand.separator && cand.enabled) matched = &cand;
    }

    if (matched) {
        // Strategy 1: Bounded synchronous UIA Invoke (waits for Invoke to finish & popup to close,
        // preventing stopTraySync/dismissPopupMenu from cancelling Electron/WPF/WinUI/Qt commands!).
        if (matched->element) {
            const InvokeOutcome out = tryInvokeBounded(matched->element.Get(), 220);
            if (out == InvokeOutcome::TimedOut) return true;
            if (out == InvokeOutcome::Succeeded && waitPopupClosed(activePopup, 110)) return true;
        }

        // Strategy 2: MSAA accDoDefaultAction if popup is still open.
        if (item.parentLabels.empty() && IsWindow(activePopup) && IsWindowVisible(activePopup)) {
            if (const auto acc = accessiblePopup(activePopup)) {
                for (const auto& entry : collectMsaaMenuEntries(activePopup)) {
                    if (entry.separator || entry.disabled || entry.hasSubmenu) continue;
                    if (!labelsMatchCI(cleanMenuLabel(entry.name), item.label)) continue;
                    VARIANT child{};
                    child.vt = VT_I4;
                    child.lVal = entry.child;
                    if (acc->accDoDefaultAction(child) == S_OK && waitPopupClosed(activePopup, 60))
                        return true;
                }
            }
        }

        // Strategy 3: Direct client mouse click using fresh live element bounding rect.
        if (IsWindow(activePopup) && IsWindowVisible(activePopup)) {
            RECT liveRect = matched->rect;
            if (matched->element) {
                RECT fresh{};
                if (SUCCEEDED(matched->element->get_CurrentBoundingRectangle(&fresh)) && !IsRectEmpty(&fresh))
                    liveRect = fresh;
            }
            if (postClientMenuClick(activePopup, liveRect, true) && waitPopupClosed(activePopup, 70))
                return true;
        }

        // Strategy 4: Focus / Select element + VK_RETURN (offscreen), then arrow-key walk if still open.
        if (matched->element && IsWindow(activePopup) && IsWindowVisible(activePopup)) {
            ComPtr<IUIAutomationSelectionItemPattern> sel;
            if (SUCCEEDED(matched->element->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&sel))) && sel)
                sel->Select();
            matched->element->SetFocus();
            SetForegroundWindow(activePopup);
            Sleep(10);
            PostMessageW(activePopup, WM_KEYDOWN, VK_RETURN, 0x001C0001);
            PostMessageW(activePopup, WM_KEYUP, VK_RETURN, 0xC01C0001);
            if (waitPopupClosed(activePopup, 50)) return true;
            if (GetForegroundWindow() == activePopup) {
                INPUT enter[2]{};
                enter[0].type = enter[1].type = INPUT_KEYBOARD;
                enter[0].ki.wVk = enter[1].ki.wVk = VK_RETURN;
                enter[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(ARRAYSIZE(enter), enter, sizeof(INPUT));
                if (waitPopupClosed(activePopup, 50)) return true;
            }
            // Arrow-key walk fallback for custom popups (e.g. Qt QMenu) that ignore UIA SetFocus on items.
            if (IsWindow(activePopup) && IsWindowVisible(activePopup)) {
                int navSteps = 0;
                bool foundPos = false;
                for (const auto& e : uiaList) {
                    if (e.separator || !e.enabled) continue;
                    ++navSteps;
                    if (&e == matched) { foundPos = true; break; }
                }
                if (foundPos && navSteps > 0 && navSteps <= 24) {
                    SetForegroundWindow(activePopup);
                    for (int k = 0; k < navSteps; ++k) {
                        PostMessageW(activePopup, WM_KEYDOWN, VK_DOWN, 0x01500001);
                        PostMessageW(activePopup, WM_KEYUP, VK_DOWN, 0xC1500001);
                        Sleep(2);
                    }
                    PostMessageW(activePopup, WM_KEYDOWN, VK_RETURN, 0x001C0001);
                    PostMessageW(activePopup, WM_KEYUP, VK_RETURN, 0xC01C0001);
                    if (waitPopupClosed(activePopup, 60)) return true;
                }
            }
        }
        return !IsWindow(activePopup) || !IsWindowVisible(activePopup);
    }

    if (item.parentLabels.empty()) {
        if (const auto acc = accessiblePopup(popup)) {
            const auto entries = collectMsaaMenuEntries(popup);
            for (const auto& entry : entries) {
                if (entry.separator || entry.disabled || entry.hasSubmenu || !labelsMatchCI(cleanMenuLabel(entry.name), item.label)) continue;
                VARIANT child{};
                child.vt = VT_I4;
                child.lVal = entry.child;
                if (acc->accDoDefaultAction(child) == S_OK && waitPopupClosed(popup, 60)) return true;
                if (postClientMenuClick(popup, entry.rect, true) && waitPopupClosed(popup, 60)) return true;
            }
        }
    }
    return false;
}

void sendContextMenuKey() {
    INPUT keys[2]{};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = VK_APPS;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT));
}

void sendShiftF10Key() {
    INPUT keys[4]{};
    for (auto& k : keys) k.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[3].ki.wVk = VK_SHIFT;
    keys[1].ki.wVk = keys[2].ki.wVk = VK_F10;
    keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT));
}

void cloakTaskbarForTempReveal(HWND taskbar) {
    if (!taskbar || !IsWindow(taskbar)) return;
    const HANDLE selfMarker = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId()));
    if (!GetPropW(taskbar, L"Vetra.PrimaryRgn.v1")) {
        if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
            if (SetWindowRgn(taskbar, empty, FALSE)) SetPropW(taskbar, L"Vetra.PrimaryRgn.v1", selfMarker);
            else DeleteObject(empty);
        }
    }
    const LONG_PTR ex = GetWindowLongPtrW(taskbar, GWL_EXSTYLE);
    constexpr LONG_PTR kNoRedir = 0x00200000L;
    if ((ex & kNoRedir) == 0 && ((ex & WS_EX_LAYERED) == 0 || GetPropW(taskbar, L"Vetra.PrimaryLayer.v1") != nullptr)) {
        if (SetPropW(taskbar, L"Vetra.PrimaryLayer.v1", selfMarker)) {
            if ((ex & WS_EX_LAYERED) == 0) SetWindowLongPtrW(taskbar, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            SetLayeredWindowAttributes(taskbar, 0, 0, LWA_ALPHA);
        }
    }
}

void adoptGuardDetectedMenu(Dock& dock) {
    const HWND detected = gGuardDetectedMenu.exchange(nullptr, std::memory_order_acq_rel);
    if (!detected || !IsWindow(detected) || !IsWindowVisible(detected)) return;
    const int sc = scoreTrayPopup(dock, detected, true);
    if (sc < 0) return;
    const int curSc = (dock.trayCapturedMenuHwnd && IsWindowVisible(dock.trayCapturedMenuHwnd))
        ? scoreTrayPopup(dock, dock.trayCapturedMenuHwnd) : -1;
    if (!dock.trayCapturedMenuHwnd || !IsWindowVisible(dock.trayCapturedMenuHwnd) || sc > curSc + 15) {
        dock.trayCapturedMenuHwnd = detected;
        dock.trayPopupSeenTries = 0;
        cloakMenuWindow(detected);
    }
}

void cloakAndBurstWaitPopups(Dock& dock, int maxSteps = 26, HWND overflowHost = nullptr) {
    ScopedHighResTimer hiRes;
    const bool hasSaved = (dock.traySavedCursor.x != 0 || dock.traySavedCursor.y != 0);
    RECT lockRc{dock.traySavedCursor.x, dock.traySavedCursor.y,
                dock.traySavedCursor.x + 1, dock.traySavedCursor.y + 1};
    for (int i = 0; i < maxSteps; ++i) {
        if (overflowHost && IsWindow(overflowHost)) moveOverflowOffscreen(overflowHost, false);
        if (dock.trayCursorLocked && hasSaved) ClipCursor(&lockRc);
        if (hasSaved) {
            POINT cur{};
            if (GetCursorPos(&cur) && (cur.x != dock.traySavedCursor.x || cur.y != dock.traySavedCursor.y))
                SetCursorPos(dock.traySavedCursor.x, dock.traySavedCursor.y);
        }
        adoptGuardDetectedMenu(dock);
        fastScanAndCloakPopups(dock, (i % 4) == 0);
        if (dock.trayCapturedMenuHwnd && IsWindowVisible(dock.trayCapturedMenuHwnd)) {
            Sleep(2);
            fastScanAndCloakPopups(dock, false);
            break;
        }
        Sleep(1);
    }
    unlockTrayCursor(dock);
}

void triggerOffscreenContextMenu(Dock& dock, HWND hostWnd, IUIAutomationElement* target, bool useShiftF10 = false) {
    ScopedHighResTimer hiRes;
    const bool hostIsOverflow = hostWnd && isOverflowWindow(hostWnd);
    if (hostIsOverflow) moveOverflowOffscreen(hostWnd, false);

    const DWORD myTid = GetCurrentThreadId();
    DWORD hostPid = 0;
    const DWORD hostTid = hostWnd ? GetWindowThreadProcessId(hostWnd, &hostPid) : 0;
    if (hostPid) AllowSetForegroundWindow(hostPid);
    AllowSetForegroundWindow(ASFW_ANY);

    const HWND prevFg = GetForegroundWindow();
    const DWORD fgTid = prevFg ? GetWindowThreadProcessId(prevFg, nullptr) : 0;
    bool attachedFg = false, attachedHost = false;
    if (fgTid && fgTid != myTid) attachedFg = AttachThreadInput(myTid, fgTid, TRUE) != FALSE;
    if (hostTid && hostTid != myTid && hostTid != fgTid)
        attachedHost = AttachThreadInput(myTid, hostTid, TRUE) != FALSE;

    if (hostWnd) SetForegroundWindow(hostWnd);
    if (hostIsOverflow) moveOverflowOffscreen(hostWnd, false);

    // Lock the system cursor to the exact 1x1 pixel where the user right-clicked BEFORE sending VK_APPS,
    // so Explorer's XAML SystemTray compatibility shim can NEVER clamp SetCursorPos(-3950, -3950) to (0, 0),
    // and tray apps calling GetCursorPos() receive the exact user click location.
    if (dock.traySavedCursor.x != 0 || dock.traySavedCursor.y != 0) {
        RECT lockRc{dock.traySavedCursor.x, dock.traySavedCursor.y,
                    dock.traySavedCursor.x + 1, dock.traySavedCursor.y + 1};
        if (ClipCursor(&lockRc)) dock.trayCursorLocked = true;
    }
    syncGuardCursorState(dock);
    gGuardCloakMenus.store(true, std::memory_order_release);

    if (target) {
        target->SetFocus();
        // Verify keyboard focus actually landed on the requested icon (1-2ms fast path, avoids firing on icon #0).
        for (int wait = 0; wait < 14; ++wait) {
            if (hostIsOverflow) moveOverflowOffscreen(hostWnd, false);
            BOOL focused = FALSE;
            if (SUCCEEDED(target->get_CurrentHasKeyboardFocus(&focused)) && focused) break;
            if (wait == 5) {
                if (hostWnd) SetForegroundWindow(hostWnd);
                target->SetFocus();
            }
            Sleep(1);
        }
    } else {
        Sleep(3);
    }

    if (hostIsOverflow) moveOverflowOffscreen(hostWnd, false);
    if (dock.traySavedCursor.x != 0 || dock.traySavedCursor.y != 0) {
        RECT lockRc{dock.traySavedCursor.x, dock.traySavedCursor.y,
                    dock.traySavedCursor.x + 1, dock.traySavedCursor.y + 1};
        if (ClipCursor(&lockRc)) dock.trayCursorLocked = true;
    }

    if (useShiftF10) sendShiftF10Key();
    else sendContextMenuKey();

    if (attachedHost) AttachThreadInput(myTid, hostTid, FALSE);
    if (attachedFg) AttachThreadInput(myTid, fgTid, FALSE);

    cloakAndBurstWaitPopups(dock, 26, hostIsOverflow ? hostWnd : nullptr);
}
}

const std::vector<TrayMenuItem>& Dock::currentTrayMenu() const {
    const auto* list = &trayMenuItems;
    for (const auto index : trayMenuPath) {
        if (index >= list->size() || (*list)[index].children.empty()) break;
        list = &(*list)[index].children;
    }
    return *list;
}

Text Dock::trayMenuTitle() const {
    if (trayMenuIconIndex < 0 || trayMenuIconIndex >= static_cast<int>(trayIcons.size())) return {};
    Text title = trayIcons[trayMenuIconIndex].name;
    const auto* list = &trayMenuItems;
    for (const auto index : trayMenuPath) {
        if (index >= list->size() || (*list)[index].children.empty()) break;
        const auto& item = (*list)[index];
        title = item.label;
        list = &item.children;
    }
    return title;
}

float Dock::trayMenuSheetWidth() const {
    size_t maxChars = trayMenuTitle().size() + 4;
    for (const auto& item : currentTrayMenu()) {
        if (item.separator) continue;
        const size_t extraCols = (item.checked ? 3u : 0u) + (!item.children.empty() ? 3u : 0u);
        maxChars = std::max(maxChars, item.label.size() + extraCols);
    }
    const float estimated = 72.f + static_cast<float>(maxChars) * 7.2f;
    return std::clamp(estimated, 276.f, 360.f);
}

void Dock::pollDeferredTrayUncloak() {
    // Also ensure any lingering Explorer overflow stays offscreen & hidden if it survived a dismiss
    // or arrived late from a cancelled background chevron invoke.
    const ULONGLONG nowWatch = GetTickCount64();
    const bool watchingOverflow = nowWatch < gOverflowWatchUntil.load(std::memory_order_relaxed);
    if (!trayBusy()) {
        if (HWND lingeringOv = findVisibleOverflow()) {
            dismissOverflowWindow(lingeringOv, panel);
        }
    }

    std::vector<CloakedMenuRecord> toRestore;
    bool anyLeft = false;
    {
        std::lock_guard lock(gCloakMutex);
        if (gDeferredUncloakMenus.empty()) {
            if (!watchingOverflow && !findVisibleOverflow()) KillTimer(hwnd, TrayUncloakTick);
            return;
        }
        const ULONGLONG nowTick = GetTickCount64();
        for (auto it = gDeferredUncloakMenus.begin(); it != gDeferredUncloakMenus.end();) {
            HWND w = it->hwnd;
            if (!w || !IsWindow(w)) {
                it = gDeferredUncloakMenus.erase(it);
                continue;
            }
            if (!IsWindowVisible(w)) {
                toRestore.push_back(*it);
                it = gDeferredUncloakMenus.erase(it);
                continue;
            }
            if (it->executed) {
                // Executed menu: NEVER send WM_CANCELMODE/VK_ESCAPE (would abort Quit/Exit or dialogs!).
                if (nowTick - it->dismissStartedAt > 1800ULL) {
                    it = gDeferredUncloakMenus.erase(it);
                    continue;
                }
            } else {
                // Cancelled capture menu: keep cloaked & retry dismiss cleanly; never uncloak visibly on-screen.
                if (nowTick - it->dismissStartedAt > 100ULL && it->dismissRetries < 4) {
                    ++it->dismissRetries;
                    it->dismissStartedAt = nowTick;
                    if (isWin32MenuClass(w)) {
                        PostMessageW(w, 0x01E6 /*MN_CANCELMENUS*/, 0, 0);
                    } else {
                        PostMessageW(w, WM_CANCELMODE, 0, 0);
                    }
                    PostMessageW(w, WM_KEYDOWN, VK_ESCAPE, 0);
                    PostMessageW(w, WM_KEYUP, VK_ESCAPE, 0);
                } else if (it->dismissRetries >= 4 && nowTick - it->dismissStartedAt > 2000ULL) {
                    it = gDeferredUncloakMenus.erase(it);
                    continue;
                }
            }
            ++it;
        }
        anyLeft = !gDeferredUncloakMenus.empty();
    }
    for (const auto& rec : toRestore) applyRestoreCloakedRecord(rec);
    if (!anyLeft && !watchingOverflow && !findVisibleOverflow()) KillTimer(hwnd, TrayUncloakTick);
}

void Dock::stopTraySync(bool forceRestore, bool preserveExecutedPopup) {
    KillTimer(hwnd, TraySyncTick);
    unlockTrayCursor(*this);
    const bool cancelledCapture = (trayAction == TrayAction::Capture);
    if (trayCaptureCancel) {
        trayCaptureCancel->store(true);
        trayCaptureCancel.reset();
    }
    if (cancelledCapture && !forceRestore) {
        gOverflowWatchUntil.store(GetTickCount64() + 300ULL, std::memory_order_relaxed);
    }
    ++trayCaptureGen;

    // 1. Handle captured / cloaked popup menus BEFORE disabling guard hooks.
    if (preserveExecutedPopup) {
        // Move all cloaked menus to deferred uncloak marked executed=true so we NEVER cancel the command!
        std::vector<HWND> cloakedHwnds;
        {
            std::lock_guard lock(gCloakMutex);
            for (const auto& r : gCloakedMenus) cloakedHwnds.push_back(r.hwnd);
        }
        for (HWND w : cloakedHwnds) uncloakMenuWindow(w, false, true);
    } else {
        if (trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
            dismissPopupMenu(trayCapturedMenuHwnd, !forceRestore);
        }
        for (;;) {
            HWND w = nullptr;
            {
                std::lock_guard lock(gCloakMutex);
                if (gCloakedMenus.empty()) break;
                w = gCloakedMenus.back().hwnd;
            }
            if (!forceRestore && w && IsWindow(w) && IsWindowVisible(w)) {
                dismissPopupMenu(w, true);
            } else {
                uncloakMenuWindow(w, forceRestore, false);
            }
        }
    }

    if (forceRestore) {
        KillTimer(hwnd, TrayUncloakTick);
        std::vector<CloakedMenuRecord> deferred;
        {
            std::lock_guard lock(gCloakMutex);
            deferred.swap(gDeferredUncloakMenus);
        }
        for (const auto& rec : deferred) applyRestoreCloakedRecord(rec);
    }

    // 2. ALWAYS dismiss any visible Explorer overflow BEFORE turning off TrayGuard hooks!
    HWND ovToClose = (trayOverflow && IsWindow(trayOverflow) && IsWindowVisible(trayOverflow))
        ? trayOverflow : findVisibleOverflow();
    if (!ovToClose) {
        const HWND guardOv = gGuardLastOverflow.load(std::memory_order_acquire);
        if (guardOv && IsWindow(guardOv) && IsWindowVisible(guardOv)) ovToClose = guardOv;
    }
    HWND fgRestore = (!preserveExecutedPopup && panel && !sheetClosing && IsWindowVisible(panel)) ? panel : nullptr;
    if (ovToClose) {
        dismissOverflowWindow(ovToClose, fgRestore);
    }
    if (gRestoreLayered || gRestoreOverflowRgn) {
        HWND overflow = (gCloakedOverflow && IsWindow(gCloakedOverflow)) ? gCloakedOverflow : findAnyOverflow();
        if (overflow && IsWindow(overflow)) {
            dismissOverflowWindow(overflow, fgRestore);
            if (IsWindow(overflow)) {
                if (gRestoreOverflowRgn) SetWindowRgn(overflow, nullptr, FALSE);
                if (gRestoreLayered) {
                    SetLayeredWindowAttributes(overflow, 0, 255, LWA_ALPHA);
                    SetWindowLongPtrW(overflow, GWL_EXSTYLE, gOrigExStyle);
                }
            }
        }
        gCloakedOverflow = nullptr;
        gRestoreLayered = false;
        gRestoreOverflowRgn = false;
    }

    // 3. Remove hooks (and shut down guard thread cleanly on app exit).
    removeTrayHooks(*this);
    if (forceRestore) shutdownTrayGuard();

    bool needDeferredTimer = false;
    {
        std::lock_guard lock(gCloakMutex);
        needDeferredTimer = !gDeferredUncloakMenus.empty();
    }
    if (!forceRestore && (needDeferredTimer || cancelledCapture || findVisibleOverflow()) && hwnd) {
        SetTimer(hwnd, TrayUncloakTick, 20, nullptr);
    }

    trayAction = TrayAction::None;
    trayOverflow = nullptr;
    trayRightClickPending = false;
    trayActionOnTaskbar = false;
    trayUsedKeyboardMenu = false;
    trayFallbackMouseSent = false;
    trayRightClickStep = 0;
    trayPopupSeenTries = 0;
    trayOverflowSettled = false;
    trayLastOvRect = {};
    trayCapturedMenuHwnd = nullptr;
    trayPreForeground = nullptr;
    trayPreWindows.clear();
    trayIgnoredPopups.clear();
    trayMenuOwnerPid = 0;
    trayExpectedStem.clear();
    traySavedCursor = {};
    trayPendingMenuIndex = -1;
    trayRightClickStartedAt = 0;
    if (panel && !sheetClosing) {
        const LONG_PTR ex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
        if (ex & WS_EX_TRANSPARENT) SetWindowLongPtrW(panel, GWL_EXSTYLE, ex & ~WS_EX_TRANSPARENT);
        if (!IsWindowVisible(panel)) ShowWindow(panel, SW_SHOW);
    }
    if (trayNeedsReveal) {
        trayNeedsReveal = false;
        if (!nativeBarRevealed) shellTaskbar.hide();
    }
}

void Dock::showSystemTray(float x) {
    if (panel && page == SystemTray) {
        close();
        return;
    }
    if (trayIcons.empty()) {
        std::lock_guard lock(gWarmupTrayMutex);
        if (gWarmupTrayItems && !gWarmupTrayItems->empty()) {
            trayIcons = std::move(*gWarmupTrayItems);
            gWarmupTrayItems.reset();
        }
    }
    trayMenuIconIndex = -1;
    trayPendingMenuIndex = -1;
    trayMenuPath.clear();
    trayMenuLoading = trayMenuUnavailable = false;
    trayMenuItems.clear();
    trayLastPopupHwnd = nullptr;
    trayLoading = trayIcons.empty();
    open(SystemTray, x);
    refreshSystemTray(false);
}

void Dock::refreshSystemTray(bool forceReload) {
    stopTraySync();
    trayMenuIconIndex = -1;
    trayPendingMenuIndex = -1;
    trayMenuPath.clear();
    trayMenuLoading = trayMenuUnavailable = false;
    trayMenuItems.clear();
    trayLastPopupHwnd = nullptr;
    listOffset = 0;
    trayLoading = trayIcons.empty();
    trayCaptureTries = 0;
    trayNeedsReveal = false;
    trayOverflowSettled = false;
    trayLastOvRect = {};
    trayAction = TrayAction::Capture;
    if (forceReload && panel && page == SystemTray) paintSheet();

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar && !FindWindowW(L"NotifyIconOverflowWindow", nullptr)) {
        trayLoading = false;
        trayAction = TrayAction::None;
        if (panel && page == SystemTray) paintSheet();
        return;
    }

    if (taskbar) {
        DWORD explorerPid = 0;
        GetWindowThreadProcessId(taskbar, &explorerPid);
        installTrayHooks(*this, explorerPid);
        if (const HWND existing = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr))
            moveOverflowOffscreen(existing);
    }

    const uint64_t gen = ++trayCaptureGen;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    trayCaptureCancel = cancel;
    const HWND notify = hwnd;
    const HWND panelWnd = panel;
    const HICON vetraIcon = appIcon;
    const size_t existingCount = trayIcons.size();
    const auto stop = extra.proStop;

    try {
        trayCaptureRequest = std::async(std::launch::async,
            [gen, cancel, stop, notify, panelWnd, taskbar, vetraIcon, forceReload, existingCount]() -> TrayCaptureResult {
                TrayCaptureResult res{};
                res.generation = gen;
                const auto aborted = [&]() { return cancel->load() || (stop && stop->load()); };
                if (aborted()) return res;

                ScopedHighResTimer hiRes;
                const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (forceReload) {
                    std::lock_guard lock(gTrayCacheMutex);
                    gTrayIconBitmapCache.clear();
                    gNotifyRegistryLoaded = false;
                }

                if (tryCaptureClassicTray(res.items, forceReload)) {
                    res.ok = true;
                    if (SUCCEEDED(comHr)) CoUninitialize();
                    if (notify && !aborted()) PostMessageW(notify, AsyncWake, 0, 0);
                    return res;
                }

                if (taskbar && !aborted()) {
                    ComPtr<IUIAutomation> uia = createUiaClient();
                    bool revealed = false;

                    HANDLE readyEv = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    {
                        std::lock_guard lock(gOverflowEventMutex);
                        gOverflowReadyEvent = readyEv;
                    }
                    struct EventGuard {
                        HANDLE ev;
                        ~EventGuard() {
                            {
                                std::lock_guard lock(gOverflowEventMutex);
                                if (gOverflowReadyEvent == ev) gOverflowReadyEvent = nullptr;
                            }
                            if (ev) CloseHandle(ev);
                        }
                    } evGuard{readyEv};

                    const auto waitForOverflow = [&](int maxMs) -> HWND {
                        for (int waited = 0; waited < maxMs && !aborted(); waited += 3) {
                            if (HWND ov = findVisibleOverflow()) {
                                moveOverflowOffscreen(ov, false);
                                return ov;
                            }
                            if (readyEv) WaitForSingleObject(readyEv, 3);
                            else Sleep(3);
                        }
                        if (aborted()) return nullptr;
                        HWND ov = findVisibleOverflow();
                        if (ov) moveOverflowOffscreen(ov, false);
                        return ov;
                    };

                    HWND overflow = findVisibleOverflow();
                    bool invokedOverflow = false;
                    if (!overflow && !aborted()) {
                        if (!forceReload) {
                            if (const HWND cachedOv = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
                                moveOverflowOffscreen(cachedOv, false);
                            }
                        }

                        if (!IsWindowVisible(taskbar)) {
                            cloakTaskbarForTempReveal(taskbar);
                            gGuardHideTaskbar.store(true, std::memory_order_relaxed);
                            ShowWindowAsync(taskbar, SW_SHOWNOACTIVATE);
                            revealed = true;
                            Sleep(8);
                        }
                        if (readyEv) ResetEvent(readyEv);
                        if (uia && invokeTrayOverflowWithUia(uia.Get(), taskbar, true)) {
                            invokedOverflow = true;
                            overflow = waitForOverflow(100);
                        }
                        if (!overflow && uia && !aborted() && invokeTrayOverflowWithUia(uia.Get(), taskbar, false)) {
                            invokedOverflow = true;
                            overflow = waitForOverflow(80);
                        }
                        if (!overflow && !aborted() && invokeViaWinBWithUia(uia.Get(), taskbar)) {
                            invokedOverflow = true;
                            overflow = waitForOverflow(100);
                        }
                    }
                    // If capture was aborted mid-flight after invoking chevron and no new tray action took over,
                    // catch & dismiss the delayed overflow so it NEVER flashes on-screen on the right!
                    if (invokedOverflow && aborted() && !gGuardEnabled.load(std::memory_order_acquire)) {
                        for (int w = 0; w < 20 && !gGuardEnabled.load(std::memory_order_acquire); ++w) {
                            if (HWND lateOv = findVisibleOverflow()) {
                                dismissOverflowWindow(lateOv, nullptr);
                                break;
                            }
                            Sleep(5);
                        }
                    }

                    std::vector<OverflowButton> allButtons;
                    std::vector<std::shared_ptr<Gdiplus::Bitmap>> allBitmaps;
                    std::vector<NotifyIconEntry> regEntries = cachedNotifyIconRegistry(forceReload);
                    bool regRefreshed = forceReload;

                    const auto canResolveWithoutPrint = [&](const Text& btnName) -> bool {
                        if (forceReload || btnName.empty()) return false;
                        if (lookupCachedTrayIcon(btnName)) return true;
                        const NotifyIconEntry* reg = matchNotifyRegistry(regEntries, btnName);
                        if (!reg && !regRefreshed) {
                            regEntries = cachedNotifyIconRegistry(true);
                            regRefreshed = true;
                            reg = matchNotifyRegistry(regEntries, btnName);
                        }
                        if (!reg) return false;
                        if (isDynamicStatusTrayIcon(toLowerText(btnName.c_str()), reg->exeStemLower))
                            return false;
                        return reg->pngBytes > 24 || reg->exeExists;
                    };

                    if (overflow && !aborted()) {
                        moveOverflowOffscreen(overflow, false);
                        if (revealed) {
                            ShowWindowAsync(taskbar, SW_HIDE);
                            res.revealedTaskbar = true;
                        }

                        std::vector<OverflowButton> buttons;
                        for (int tries = 0; tries < 10 && !aborted(); ++tries) {
                            buttons = collectOverflowButtonsWithUia(uia.Get(), overflow);
                            if (!buttons.empty()) break;
                            Sleep(6);
                        }
                        // Only wait a second tick if we have no prior baseline or fewer buttons than expected.
                        if (!buttons.empty() && !aborted() && (existingCount == 0 || buttons.size() < existingCount)) {
                            Sleep(8);
                            auto next = collectOverflowButtonsWithUia(uia.Get(), overflow);
                            if (next.size() > buttons.size()) {
                                buttons = std::move(next);
                                Sleep(8);
                                auto next2 = collectOverflowButtonsWithUia(uia.Get(), overflow);
                                if (next2.size() > buttons.size()) buttons = std::move(next2);
                            }
                        }

                        if (!buttons.empty() && !aborted()) {
                            ComPtr<IUIAutomationScrollPattern> scroller;
                            if (buttons.size() >= 12) {
                                scroller = findOverflowVerticalScroller(uia.Get(), overflow);
                            }
                            if (scroller) {
                                scroller->SetScrollPercent(UIA_ScrollPatternNoScroll, 0.0);
                                Sleep(12);
                                double lastPct = -1.0;
                                for (int step = 0; step < 8 && !aborted(); ++step) {
                                    auto pageBtns = collectOverflowButtonsWithUia(uia.Get(), overflow);
                                    bool pageNeedsPrint = false;
                                    for (const auto& b : pageBtns) {
                                        if (!canResolveWithoutPrint(b.name)) { pageNeedsPrint = true; break; }
                                    }
                                    std::vector<std::shared_ptr<Gdiplus::Bitmap>> pageBmps;
                                    if (pageNeedsPrint && !pageBtns.empty()) {
                                        captureOverflowIcons(overflow, pageBtns, pageBmps);
                                    }
                                    for (size_t i = 0; i < pageBtns.size(); ++i) {
                                        const auto& b = pageBtns[i];
                                        const bool dup = std::any_of(allButtons.begin(), allButtons.end(), [&](const OverflowButton& e) {
                                            if (!b.runtimeId.empty() && !e.runtimeId.empty()) return e.runtimeId == b.runtimeId;
                                            return e.name == b.name;
                                        });
                                        if (!dup) {
                                            allButtons.push_back(b);
                                            allBitmaps.push_back(i < pageBmps.size() ? pageBmps[i] : nullptr);
                                        }
                                    }
                                    double curPct = 100.0;
                                    if (FAILED(scroller->get_CurrentVerticalScrollPercent(&curPct)) || curPct >= 99.0) break;
                                    if (lastPct >= 0.0 && curPct <= lastPct + 0.5) break;
                                    lastPct = curPct;
                                    if (FAILED(scroller->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement))) {
                                        scroller->SetScrollPercent(UIA_ScrollPatternNoScroll, std::min(100.0, curPct + 35.0));
                                    }
                                    Sleep(14);
                                }
                                scroller->SetScrollPercent(UIA_ScrollPatternNoScroll, 0.0);
                            } else {
                                bool needPrintWindow = false;
                                for (const auto& b : buttons) {
                                    if (!canResolveWithoutPrint(b.name)) { needPrintWindow = true; break; }
                                }
                                if (needPrintWindow) {
                                    for (int tries = 0; tries < 4 && !aborted(); ++tries) {
                                        if (captureOverflowIcons(overflow, buttons, allBitmaps)) break;
                                        Sleep(8);
                                    }
                                }
                                allButtons = std::move(buttons);
                                if (allBitmaps.size() < allButtons.size()) allBitmaps.resize(allButtons.size());
                            }
                        }
                        // Only dismiss overflow if this capture wasn't cancelled by an active user click/right-click!
                        if (!aborted()) {
                            dismissOverflowWindow(overflow, panelWnd);
                        }
                    }

                    // Also include any notification icons promoted directly onto the Windows taskbar.
                    if (uia && !aborted()) {
                        auto promoted = collectPromotedTaskbarButtonsWithUia(uia.Get(), taskbar);
                        for (auto& pBtn : promoted) {
                            const Text pLow = toLowerText(pBtn.name.c_str());
                            const bool dup = std::any_of(allButtons.begin(), allButtons.end(), [&](const OverflowButton& e) {
                                if (!pBtn.runtimeId.empty() && !e.runtimeId.empty()) return e.runtimeId == pBtn.runtimeId;
                                return toLowerText(e.name.c_str()) == pLow;
                            });
                            if (!dup) {
                                allButtons.push_back(std::move(pBtn));
                                allBitmaps.push_back(nullptr);
                            }
                        }
                    }

                    if (!allButtons.empty() && !aborted()) {
                        res.items.reserve(allButtons.size());
                        for (size_t i = 0; i < allButtons.size() && !aborted(); ++i) {
                            auto captured = (i < allBitmaps.size()) ? allBitmaps[i] : nullptr;
                            std::shared_ptr<Gdiplus::Bitmap> icon = forceReload ? nullptr : lookupCachedTrayIcon(allButtons[i].name);
                            if (!icon) {
                                if (!regRefreshed && !matchNotifyRegistry(regEntries, allButtons[i].name)) {
                                    regEntries = cachedNotifyIconRegistry(true);
                                    regRefreshed = true;
                                }
                                icon = resolveCrispTrayIcon(allButtons[i].name, vetraIcon, regEntries, captured);
                                storeCachedTrayIcon(allButtons[i].name, icon);
                            }
                            res.items.push_back({allButtons[i].name, std::move(icon), nullptr, 0, 0, allButtons[i].onTaskbar});
                        }
                        res.ok = !res.items.empty();
                    }

                    if (revealed) {
                        ShowWindowAsync(taskbar, SW_HIDE);
                        res.revealedTaskbar = true;
                    }
                    uia.Reset();
                }

                if (SUCCEEDED(comHr)) CoUninitialize();
                if (notify && !aborted()) PostMessageW(notify, AsyncWake, 0, 0);
                return res;
            });
    } catch (const std::exception&) {
        trayLoading = false;
        stopTraySync();
        if (panel && page == SystemTray) paintSheet();
    }
}

void Dock::pollTrayCapture() {
    if (trayIcons.empty() && !trayBusy()) {
        std::vector<TrayItem> warmed;
        {
            std::lock_guard lock(gWarmupTrayMutex);
            if (gWarmupTrayItems && !gWarmupTrayItems->empty()) {
                warmed = std::move(*gWarmupTrayItems);
                gWarmupTrayItems.reset();
            }
        }
        if (!warmed.empty()) {
            trayIcons = std::move(warmed);
            trayLoading = false;
            if (panel && page == SystemTray && !sheetClosing && trayMenuIconIndex < 0 && trayPendingMenuIndex < 0)
                positionSheet();
        }
    }

    if (!trayCaptureRequest.valid() ||
        trayCaptureRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    TrayCaptureResult res{};
    try { res = trayCaptureRequest.get(); }
    catch (const std::exception&) { res.ok = false; }

    if (res.generation != trayCaptureGen) return;
    if (trayAction == TrayAction::Capture) {
        if (HWND ov = findVisibleOverflow()) dismissOverflowWindow(ov, panel);
        if (!res.passiveWarmup && panel && page == SystemTray && !sheetClosing) {
            const HWND fg = GetForegroundWindow();
            if (!fg || isOverflowWindow(fg) || fg == FindWindowW(L"Shell_TrayWnd", nullptr))
                forceForegroundWindow(panel);
        }
        removeTrayHooks(*this);
        trayAction = TrayAction::None;
        trayCaptureCancel.reset();
    }
    if (res.revealedTaskbar && !nativeBarRevealed) shellTaskbar.hide();

    if (res.ok) {
        const auto oldGrid = trayGridLayout();
        const bool wasEmpty = trayIcons.empty();
        bool listChanged = res.items.size() != trayIcons.size();
        if (!listChanged) {
            for (size_t i = 0; i < res.items.size(); ++i) {
                if (res.items[i].name != trayIcons[i].name ||
                    res.items[i].onTaskbar != trayIcons[i].onTaskbar ||
                    (!trayIcons[i].icon && res.items[i].icon)) {
                    listChanged = true;
                    break;
                }
            }
        }
        trayIcons = std::move(res.items);
        const auto newGrid = trayGridLayout();
        const bool dimsChanged = wasEmpty || oldGrid.cols != newGrid.cols || oldGrid.visibleRows != newGrid.visibleRows;
        trayLoading = false;
        if (!res.passiveWarmup && panel && page == SystemTray && !sheetClosing && trayMenuIconIndex < 0 && trayPendingMenuIndex < 0) {
            if (dimsChanged) {
                triggerSheetContentAnim(0.f, 5.f);
                positionSheet();
            } else if (listChanged) {
                paintSheet();
            }
            const HWND fg = GetForegroundWindow();
            if (!fg || isOverflowWindow(fg) || fg == FindWindowW(L"Shell_TrayWnd", nullptr))
                SetForegroundWindow(panel);
        }
    } else if (!res.passiveWarmup) {
        trayLoading = false;
        if (panel && page == SystemTray && !sheetClosing && trayMenuIconIndex < 0 && trayPendingMenuIndex < 0) paintSheet();
    }
}

void Dock::prewarmTrayCaches() {
    if (trayWarmupDone) return;
    trayWarmupDone = true;
    const auto stop = extra.proStop;
    const HWND notify = hwnd;
    const HICON vetraIcon = appIcon;

    std::thread([stop, notify, vetraIcon]() {
        ScopedBackgroundPriority bgPriority;
        if (stop && stop->load()) return;

        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const auto regEntries = cachedNotifyIconRegistry(false);

        // Snapshot currently running process stems (~0.4ms) and pre-decode/cache their HD tray icons
        // so opening Gizli simgeler has zero icon decoding or shell:AppsFolder enumeration latency.
        std::unordered_set<Text> runningStems;
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{sizeof(pe)};
            if (Process32FirstW(snap, &pe)) {
                do {
                    runningStems.insert(fileStemLower(pe.szExeFile));
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }

        bool needsAppsFolder = false;
        for (const auto& e : regEntries) {
            if (stop && stop->load()) break;
            if (e.exeStemLower.empty() || !runningStems.contains(e.exeStemLower)) continue;
            if (toLowerText(e.exePath.c_str()).find(L"\\windowsapps\\") != Text::npos)
                needsAppsFolder = true;
            const Text cacheKey = !e.tip.empty() ? e.tip : e.exeStemLower;
            if (!lookupCachedTrayIcon(cacheKey)) {
                if (auto bmp = resolveCrispTrayIcon(cacheKey, vetraIcon, regEntries, nullptr))
                    storeCachedTrayIcon(cacheKey, bmp);
            }
        }
        if (needsAppsFolder && (!stop || !stop->load())) {
            lookupAppsFolderIdByPfn(L"_warmup_");
        }

        // Passively populate tray icon list without opening any window or changing foreground if Explorer
        // already has a classic toolbar or an already-realized XAML overflow island.
        std::vector<TrayItem> passiveItems;
        if (!stop || !stop->load()) {
            if (!tryCaptureClassicTray(passiveItems, false)) {
                if (const HWND cachedOv = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr)) {
                    moveOverflowOffscreen(cachedOv);
                    ComPtr<IUIAutomation> uia = createUiaClient();
                    if (uia) {
                        auto buttons = collectOverflowButtonsWithUia(uia.Get(), cachedOv, true);
                        if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) {
                            for (auto& pBtn : collectPromotedTaskbarButtonsWithUia(uia.Get(), tb)) {
                                const Text pLow = toLowerText(pBtn.name.c_str());
                                const bool dup = std::any_of(buttons.begin(), buttons.end(), [&](const OverflowButton& b) {
                                    return toLowerText(b.name.c_str()) == pLow;
                                });
                                if (!dup) buttons.push_back(std::move(pBtn));
                            }
                        }
                        for (const auto& b : buttons) {
                            if (stop && stop->load()) break;
                            auto icon = lookupCachedTrayIcon(b.name);
                            if (!icon) {
                                icon = resolveCrispTrayIcon(b.name, vetraIcon, regEntries, nullptr);
                                storeCachedTrayIcon(b.name, icon);
                            }
                            passiveItems.push_back({b.name, std::move(icon), nullptr, 0, 0, b.onTaskbar});
                        }
                    }
                }
            }
        }

        if (SUCCEEDED(comHr)) CoUninitialize();
        if (!passiveItems.empty() && (!stop || !stop->load())) {
            {
                std::lock_guard lock(gWarmupTrayMutex);
                gWarmupTrayItems = std::move(passiveItems);
            }
            if (notify) PostMessageW(notify, AsyncWake, 0, 0);
        }
    }).detach();
}

void Dock::activateTrayIcon(int index, bool rightClick, POINT screenPt) {
    if (index < 0 || index >= static_cast<int>(trayIcons.size())) return;
    const TrayItem item = trayIcons[index];

    if (item.name == L"Vetra") {
        open(rightClick ? ContextMenu : Studio);
        return;
    }

    if (rightClick) {
        // Elevated SYSTEM-owned / kernel self-protected tray icons (NVIDIA, Monster, Kaspersky)
        // cannot be cloaked cross-process due to UIPI / self-defense; render instant native menus.
        std::vector<TrayMenuItem> fastMenu = buildNvidiaTrayMenu(item.name);
        if (fastMenu.empty() && containsCI(item.name.c_str(), L"monster")) fastMenu = buildMonsterControlMenu();
        if (fastMenu.empty() && containsCI(item.name.c_str(), L"kaspersky")) fastMenu = buildKasperskyMenu(item.name);
        if (!fastMenu.empty()) {
            stopTraySync();
            trayMenuIconIndex = index;
            trayMenuPath.clear();
            trayMenuLoading = trayMenuUnavailable = false;
            trayMenuItems = std::move(fastMenu);
            listOffset = 0;
            if (panel && page == SystemTray) {
                sheet.resetMotion();
                menuHoverVisible = false;
                menuHoverY.initialized = false;
                triggerSheetContentAnim(16.f, 0.f);
                positionSheet();
            }
            return;
        }
    }

    if (!rightClick && item.targetHwnd && IsWindow(item.targetHwnd)) {
        close();
        PostMessageW(item.targetHwnd, item.callbackMsg, item.uid, WM_LBUTTONDOWN);
        PostMessageW(item.targetHwnd, item.callbackMsg, item.uid, WM_LBUTTONUP);
        PostMessageW(item.targetHwnd, item.callbackMsg, item.uid, NIN_SELECT);
        return;
    }

    startTrayIconAction(index, rightClick ? TrayAction::RightClickMenu : TrayAction::LeftClick, screenPt);
}

void Dock::startTrayIconAction(int index, TrayAction action, POINT screenPt) {
    const auto& item = trayIcons[index];
    // If background capture already has the overflow open offscreen, cancel capture cleanly
    // WITHOUT closing the overflow so this right-click/left-click reuses it with 0ms latency!
    const bool reuseOpenOverflow = (trayAction == TrayAction::Capture) && !item.onTaskbar && (findVisibleOverflow() != nullptr);
    if (reuseOpenOverflow) {
        KillTimer(hwnd, TraySyncTick);
        if (trayCaptureCancel) {
            trayCaptureCancel->store(true);
            trayCaptureCancel.reset();
        }
        ++trayCaptureGen;
        trayAction = TrayAction::None;
    } else {
        stopTraySync();
    }

    if (action == TrayAction::ExecuteMenu && trayLastPopupHwnd) {
        clearDeferredUncloakFor(trayLastPopupHwnd);
    }

    if (!GetCursorPos(&traySavedCursor)) traySavedCursor = screenPt;
    trayCursorLocked = false;
    trayAction = action;
    trayActionIndex = index;
    trayActionName = item.name;
    trayActionOnTaskbar = item.onTaskbar;
    trayActionPoint = (screenPt.x != 0 || screenPt.y != 0) ? screenPt : traySavedCursor;
    trayLocalOffset = {};
    trayCaptureTries = 0;
    trayPopupSeenTries = 0;
    trayNeedsReveal = false;
    trayRightClickPending = false;
    trayUsedKeyboardMenu = false;
    trayFallbackMouseSent = false;
    trayRightClickStep = 0;
    trayOverflowSettled = false;
    trayLastOvRect = {};
    trayCapturedMenuHwnd = nullptr;
    trayExpectedStem.clear();
    if (const auto* reg = matchNotifyRegistry(cachedNotifyIconRegistry(false), trayActionName)) {
        trayExpectedStem = reg->exeStemLower;
    }
    trayRightClickStartedAt = GetTickCount64();

    if (action == TrayAction::RightClickMenu) {
        trayPendingMenuIndex = index;
        trayMenuPath.clear();
        trayMenuLoading = false;
        trayMenuUnavailable = false;
        trayMenuItems.clear();
        trayLastPopupHwnd = nullptr;
        listOffset = 0;
        if (panel && page == SystemTray) paintSheet();
    } else if (action == TrayAction::LeftClick) {
        close();
    }

    installTrayHooks(*this, 0);

    const auto tryCompleteImmediateCapture = [&]() -> bool {
        adoptGuardDetectedMenu(*this);
        if (action == TrayAction::RightClickMenu && trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
            auto items = extractPopupMenuItems(trayCapturedMenuHwnd);
            if (!items.empty()) {
                trayLastPopupHwnd = trayCapturedMenuHwnd;
                trayMenuItems = std::move(items);
                finishTrayMenu();
                return true;
            }
        } else if (action == TrayAction::ExecuteMenu && trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
            if (invokePopupMenuItem(trayCapturedMenuHwnd, trayPendingExec, trayMenuItems)) {
                finishTrayMenu(true);
                return true;
            }
        }
        return false;
    };

    if (action != TrayAction::LeftClick && IsWindow(item.targetHwnd)) {
        if (panel && action == TrayAction::ExecuteMenu) {
            const LONG_PTR pex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
            SetWindowLongPtrW(panel, GWL_EXSTYLE, pex | WS_EX_TRANSPARENT);
        }
        GetWindowThreadProcessId(item.targetHwnd, &trayMenuOwnerPid);
        snapshotTrayPreWindows(*this);
        AllowSetForegroundWindow(trayMenuOwnerPid);
        trayNativeClick = trayActionPoint;
        trayRightClickStep = 2;
        trayRightClickPending = true;
        gGuardCloakMenus.store(true, std::memory_order_release);
        if (PostMessageW(item.targetHwnd, item.callbackMsg, item.uid, WM_RBUTTONUP)) {
            cloakAndBurstWaitPopups(*this, 26);
            if (tryCompleteImmediateCapture()) return;
            if (SetTimer(hwnd, TraySyncTick, 10, nullptr)) return;
        }
        finishTrayMenu();
        return;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) { finishTrayMenu(); return; }

    // Promoted taskbar tray icons live directly on Shell_TrayWnd (no need to open overflow).
    if (trayActionOnTaskbar) {
        if (!IsWindowVisible(taskbar)) {
            trayNeedsReveal = true;
            gGuardHideTaskbar.store(true, std::memory_order_relaxed);
            shellTaskbar.revealPrimary(true);
            ScopedHighResTimer hiRes;
            Sleep(6);
        }
        auto promoted = collectPromotedTaskbarButtonsWithUia(sharedUia(), taskbar);
        RECT btnRect{};
        auto target = matchOverflowButton(promoted, trayActionName, -1, btnRect);
        if (target) {
            if (action == TrayAction::LeftClick) {
                tryInvoke(target.Get());
                stopTraySync();
                return;
            }
            snapshotTrayPreWindows(*this);
            trayRightClickPending = true;
            trayUsedKeyboardMenu = true;
            trayRightClickStep = 2;
            trayCaptureTries = 0;
            trayNativeClick = trayActionPoint;
            if (panel && action == TrayAction::ExecuteMenu) {
                const LONG_PTR pex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
                SetWindowLongPtrW(panel, GWL_EXSTYLE, pex | WS_EX_TRANSPARENT);
            }
            triggerOffscreenContextMenu(*this, taskbar, target.Get(), false);
            if (trayNeedsReveal && IsWindowVisible(taskbar)) ShowWindowAsync(taskbar, SW_HIDE);
            if (tryCompleteImmediateCapture()) return;
            if (SetTimer(hwnd, TraySyncTick, 10, nullptr)) return;
            finishTrayMenu();
            return;
        }
    }

    if (const HWND existing = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr))
        moveOverflowOffscreen(existing, false);

    if (findVisibleOverflow()) {
        updateTraySync();
        return;
    }

    // Reveal cloaked taskbar immediately if hidden so overflow opens in ~10ms.
    if (!IsWindowVisible(taskbar)) {
        trayNeedsReveal = true;
        gGuardHideTaskbar.store(true, std::memory_order_relaxed);
        shellTaskbar.revealPrimary(true);
        ScopedHighResTimer hiRes;
        Sleep(6);
        invokeTrayOverflow(taskbar, true);
    } else if (!invokeTrayOverflow(taskbar, true)) {
        invokeTrayOverflow(taskbar, false);
    }

    // Fast inline wait (~24ms); TrayGuard thread stays live and pins overflow offscreen instantaneously!
    {
        ScopedHighResTimer hiRes;
        for (int w = 0; w < 8 && !findVisibleOverflow(); ++w) Sleep(3);
    }
    if (HWND ov = findVisibleOverflow()) {
        moveOverflowOffscreen(ov, false);
        updateTraySync();
        return;
    }
    if (!SetTimer(hwnd, TraySyncTick, 10, nullptr)) finishTrayMenu();
}

void Dock::executeTrayMenuItem(const TrayMenuItem& item) {
    if (item.disabled || trayBusy() || trayMenuIconIndex < 0 || trayMenuIconIndex >= static_cast<int>(trayIcons.size())) return;
    if (!item.launchTarget.empty()) {
        auto target = item.launchTarget;
        close();
        launch(target);
        return;
    }
    if (item.rawMenuIndex < 0 && item.cmdId == 9001) {
        const int idx = trayMenuIconIndex;
        trayMenuIconIndex = -1;
        trayMenuPath.clear();
        trayMenuItems.clear();
        constexpr const wchar_t* monsterApp = L"C:\\Program Files\\OEM\\Monster Kontrol Merkezi\\Core\\ControlCenterU.exe";
        if (GetFileAttributesW(monsterApp) != INVALID_FILE_ATTRIBUTES) {
            close();
            launch(monsterApp);
            return;
        }
        activateTrayIcon(idx, false);
        return;
    }
    if (item.rawMenuIndex < 0) {
        const int idx = trayMenuIconIndex;
        trayMenuIconIndex = -1;
        trayMenuPath.clear();
        trayMenuItems.clear();
        activateTrayIcon(idx, false);
        return;
    }

    const int idx = trayMenuIconIndex;
    POINT clickPt = trayActionPoint;
    if (!GetCursorPos(&clickPt) && panel) {
        RECT pr{};
        if (GetWindowRect(panel, &pr)) {
            clickPt = {(pr.left + pr.right) / 2, (pr.top + pr.bottom) / 2};
        }
    }

    trayPendingExec = item;
    startTrayIconAction(idx, TrayAction::ExecuteMenu, clickPt);
}

void Dock::finishTrayMenu(bool executed) {
    const bool capture = trayAction == TrayAction::RightClickMenu;
    const bool execute = trayAction == TrayAction::ExecuteMenu;
    const int resolvedIdx = (trayPendingMenuIndex >= 0) ? trayPendingMenuIndex : trayMenuIconIndex;
    if (capture) {
        if (resolvedIdx >= 0 && resolvedIdx < static_cast<int>(trayIcons.size()))
            trayMenuIconIndex = resolvedIdx;
        if (trayMenuItems.empty()) {
            trayMenuItems = buildFallbackTrayMenu(trayActionName);
            if (trayMenuItems.empty()) {
                trayMenuUnavailable = true;
                TrayMenuItem openItem{};
                openItem.label = L"Uygulamayı aç";
                openItem.rawMenuIndex = -1;
                trayMenuItems.push_back(std::move(openItem));
            }
        }
    }
    trayMenuLoading = false;
    // Guard against transient WA_INACTIVE closing Vetra's panel while foreground returns from overflow/menu.
    const bool keepSheet = (capture || (execute && !executed)) && panel && page == SystemTray && !sheetClosing;
    const bool prevOpening = openingSheet;
    if (keepSheet) openingSheet = true;
    stopTraySync(false, execute && executed);
    if (execute && executed) {
        trayMenuIconIndex = -1;
        trayMenuPath.clear();
        trayMenuItems.clear();
        trayLastPopupHwnd = nullptr;
        close();
    }
    if (keepSheet) {
        sheet.resetMotion();
        menuHoverVisible = false;
        menuHoverY.initialized = false;
        if (capture) triggerSheetContentAnim(16.f, 0.f);
        positionSheet();
        forceForegroundWindow(panel);
        if (execute && !executed) report(L"Menü komutu çalıştırılamadı.");
        openingSheet = prevOpening;
    }
}

void Dock::backTrayMenu() {
    if (trayBusy()) stopTraySync();
    if (!trayMenuPath.empty()) {
        trayMenuPath.pop_back();
    } else {
        trayMenuIconIndex = -1;
        trayPendingMenuIndex = -1;
        trayMenuLoading = trayMenuUnavailable = false;
        trayMenuItems.clear();
        trayLastPopupHwnd = nullptr;
    }
    listOffset = 0;
    sheet.resetMotion();
    menuHoverVisible = false;
    menuHoverY.initialized = false;
    triggerSheetContentAnim(-16.f, 0.f);
    positionSheet();
}

void Dock::updateTraySync() {
    if (gInTraySync) return;
    struct SyncGuard {
        bool& flag;
        explicit SyncGuard(bool& f) : flag(f) { flag = true; }
        ~SyncGuard() { flag = false; }
    } syncGuard(gInTraySync);
    KillTimer(hwnd, TraySyncTick);
    if (trayAction == TrayAction::None) { stopTraySync(); return; }

    // Only show "Menü yükleniyor…" if capture genuinely takes >260ms, avoiding any brief resize flash.
    if (trayAction == TrayAction::RightClickMenu && !trayMenuLoading && trayPendingMenuIndex >= 0 &&
        trayRightClickStartedAt > 0 && GetTickCount64() - trayRightClickStartedAt > 260) {
        trayMenuIconIndex = trayPendingMenuIndex;
        trayMenuLoading = true;
        if (panel && page == SystemTray) {
            positionSheet();
            paintSheet();
        }
    }

    const HWND overflow = findVisibleOverflow();
    if (overflow) moveOverflowOffscreen(overflow, false);
    if ((trayAction == TrayAction::RightClickMenu || trayAction == TrayAction::ExecuteMenu) && trayRightClickStep > 0) {
        if (trayRightClickStep == 4) {
            const bool done = !trayCapturedMenuHwnd || !IsWindowVisible(trayCapturedMenuHwnd) ||
                !isWin32MenuClass(trayCapturedMenuHwnd);
            finishTrayMenu(done);
            return;
        }

        const bool capture = trayAction == TrayAction::RightClickMenu;
        adoptGuardDetectedMenu(*this);
        fastScanAndCloakPopups(*this, true);
        if (!IsWindowVisible(trayCapturedMenuHwnd)) {
            trayCapturedMenuHwnd = nullptr;
            trayPopupSeenTries = 0;
        }

        if (trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
            ++trayPopupSeenTries;
            if (capture) {
                auto items = extractPopupMenuItems(trayCapturedMenuHwnd);
                if (!items.empty()) {
                    trayLastPopupHwnd = trayCapturedMenuHwnd;
                    trayMenuItems = std::move(items);
                    finishTrayMenu();
                    return;
                }
                // If a non-#32768 window stays empty after 4 ticks, check if another real popup opened.
                if (!isWin32MenuClass(trayCapturedMenuHwnd) && trayPopupSeenTries == 4) {
                    const HWND emptyCandidate = trayCapturedMenuHwnd;
                    trayIgnoredPopups.push_back(emptyCandidate);
                    {
                        std::lock_guard lock(gGuardFilterMutex);
                        gGuardIgnored.insert(emptyCandidate);
                    }
                    if (HWND alt = findTriggeredTrayPopup(*this)) {
                        uncloakMenuWindow(emptyCandidate, false);
                        trayCapturedMenuHwnd = alt;
                        trayPopupSeenTries = 0;
                        cloakMenuWindow(alt);
                    } else {
                        trayIgnoredPopups.pop_back();
                        std::lock_guard lock(gGuardFilterMutex);
                        gGuardIgnored.erase(emptyCandidate);
                    }
                }
                const int maxSeen = isWin32MenuClass(trayCapturedMenuHwnd) ? 10 : 20;
                if (trayPopupSeenTries >= maxSeen) {
                    finishTrayMenu();
                    return;
                }
            } else {
                if (invokePopupMenuItem(trayCapturedMenuHwnd, trayPendingExec, trayMenuItems)) {
                    finishTrayMenu(true);
                    return;
                } else if (trayPopupSeenTries >= (isWin32MenuClass(trayCapturedMenuHwnd) ? 8 : 16)) {
                    finishTrayMenu(false);
                    return;
                }
            }
        }

        // Offscreen Shift+F10 retry (never moves overflow on-screen and never moves the user's cursor!)
        if (!trayCapturedMenuHwnd && trayUsedKeyboardMenu && !trayFallbackMouseSent &&
            !trayActionOnTaskbar && trayCaptureTries >= 4 && overflow) {
            trayFallbackMouseSent = true;
            moveOverflowOffscreen(overflow, false);
            const auto retryBtns = collectOverflowButtons(overflow);
            RECT retryRect{};
            if (auto target = matchOverflowButton(retryBtns, trayActionName, trayActionIndex, retryRect)) {
                triggerOffscreenContextMenu(*this, overflow, target.Get(), true);
                adoptGuardDetectedMenu(*this);
                if (trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
                    if (capture) {
                        auto items = extractPopupMenuItems(trayCapturedMenuHwnd);
                        if (!items.empty()) {
                            trayLastPopupHwnd = trayCapturedMenuHwnd;
                            trayMenuItems = std::move(items);
                            finishTrayMenu();
                            return;
                        }
                    } else if (invokePopupMenuItem(trayCapturedMenuHwnd, trayPendingExec, trayMenuItems)) {
                        finishTrayMenu(true);
                        return;
                    }
                }
            }
        }

        if (++trayCaptureTries < 28) {
            if (!trayCapturedMenuHwnd && trayMenuOwnerPid && trayCaptureTries == 6 &&
                trayActionIndex >= 0 && trayActionIndex < static_cast<int>(trayIcons.size())) {
                const auto& item = trayIcons[trayActionIndex];
                if (item.targetHwnd && IsWindow(item.targetHwnd)) {
                    PostMessageW(item.targetHwnd, item.callbackMsg, MAKEWPARAM(trayNativeClick.x, trayNativeClick.y),
                        MAKELPARAM(WM_CONTEXTMENU, item.uid));
                }
            }
            if (SetTimer(hwnd, TraySyncTick, 10, nullptr)) return;
        }
        finishTrayMenu();
        return;
    }

    if (!overflow) {
        ++trayCaptureTries;
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar) {
            if (!trayNeedsReveal && !IsWindowVisible(taskbar)) {
                trayNeedsReveal = true;
                gGuardHideTaskbar.store(true, std::memory_order_relaxed);
                shellTaskbar.revealPrimary(true);
            }
            if (trayCaptureTries == 2 || trayCaptureTries == 5) invokeTrayOverflow(taskbar, true);
            else if (trayCaptureTries == 8) invokeViaWinB(taskbar);
        }
        if (trayCaptureTries < 14 && SetTimer(hwnd, TraySyncTick, 14, nullptr)) return;
        if (trayAction == TrayAction::Capture) {
            trayLoading = false;
            if (panel && page == SystemTray) paintSheet();
        }
        finishTrayMenu();
        return;
    }

    trayOverflow = overflow;
    moveOverflowOffscreen(overflow, false);
    if (trayNeedsReveal) {
        if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb))
            ShowWindowAsync(tb, SW_HIDE);
    }

    const auto buttons = collectOverflowButtons(overflow);
    if (buttons.empty()) {
        if (++trayCaptureTries < 14 && SetTimer(hwnd, TraySyncTick, 14, nullptr)) return;
        dismissOverflowWindow(overflow, panel);
        if (trayAction == TrayAction::Capture) {
            trayLoading = false;
            if (panel && page == SystemTray) paintSheet();
        }
        finishTrayMenu();
        return;
    }

    if (trayAction == TrayAction::Capture) {
        std::vector<std::shared_ptr<Gdiplus::Bitmap>> bitmaps;
        captureOverflowIcons(overflow, buttons, bitmaps);
        const auto regEntries = cachedNotifyIconRegistry(false);
        std::vector<TrayItem> next;
        next.reserve(buttons.size());
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto captured = (i < bitmaps.size()) ? bitmaps[i] : nullptr;
            auto icon = lookupCachedTrayIcon(buttons[i].name);
            if (!icon) {
                icon = resolveCrispTrayIcon(buttons[i].name, appIcon, regEntries, captured);
                storeCachedTrayIcon(buttons[i].name, icon);
            }
            next.push_back({buttons[i].name, std::move(icon), nullptr, 0, 0, false});
        }
        if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr)) {
            for (auto& pBtn : collectPromotedTaskbarButtonsWithUia(sharedUia(), tb)) {
                const Text pLow = toLowerText(pBtn.name.c_str());
                const bool dup = std::any_of(next.begin(), next.end(), [&](const TrayItem& e) {
                    return toLowerText(e.name.c_str()) == pLow;
                });
                if (!dup) {
                    auto icon = lookupCachedTrayIcon(pBtn.name);
                    if (!icon) {
                        icon = resolveCrispTrayIcon(pBtn.name, appIcon, regEntries, nullptr);
                        storeCachedTrayIcon(pBtn.name, icon);
                    }
                    next.push_back({pBtn.name, std::move(icon), nullptr, 0, 0, true});
                }
            }
        }
        trayIcons = std::move(next);
        trayLoading = false;
        dismissOverflowWindow(overflow, panel);
        stopTraySync();
        if (panel && page == SystemTray) {
            positionSheet();
            SetForegroundWindow(panel);
        }
        return;
    }

    RECT btnRect{};
    ComPtr<IUIAutomationElement> target = matchOverflowButton(buttons, trayActionName, trayActionIndex, btnRect);
    if (!target && buttons.size() >= 12) {
        if (auto scroller = findOverflowVerticalScroller(sharedUia(), overflow)) {
            ScopedHighResTimer hiRes;
            scroller->SetScrollPercent(UIA_ScrollPatternNoScroll, 0.0);
            Sleep(12);
            for (int step = 0; step < 8 && !target; ++step) {
                auto pageBtns = collectOverflowButtons(overflow);
                target = matchOverflowButton(pageBtns, trayActionName, -1, btnRect);
                if (target) break;
                double curPct = 100.0;
                if (FAILED(scroller->get_CurrentVerticalScrollPercent(&curPct)) || curPct >= 99.0) break;
                if (FAILED(scroller->Scroll(ScrollAmount_NoAmount, ScrollAmount_LargeIncrement))) {
                    scroller->SetScrollPercent(UIA_ScrollPatternNoScroll, std::min(100.0, curPct + 35.0));
                }
                Sleep(14);
            }
        }
    }
    if (!target) {
        dismissOverflowWindow(overflow, panel);
        finishTrayMenu();
        return;
    }

    if (trayAction == TrayAction::LeftClick) {
        tryInvoke(target.Get());
        dismissOverflowWindow(overflow, nullptr);
        stopTraySync();
        return;
    }

    // RightClickMenu / ExecuteMenu — Keep overflow 100% offscreen, lock cursor at traySavedCursor via ClipCursor,
    // verify keyboard focus on target icon, trigger context menu via VK_APPS, cloak in <1ms, and extract/execute.
    RECT ovRect{};
    if (GetWindowRect(overflow, &ovRect)) {
        trayLocalOffset = {
            (btnRect.left + btnRect.right) / 2 - ovRect.left,
            (btnRect.top + btnRect.bottom) / 2 - ovRect.top
        };
    }
    moveOverflowOffscreen(overflow, false);

    snapshotTrayPreWindows(*this);
    trayRightClickPending = true;
    trayUsedKeyboardMenu = true;
    trayFallbackMouseSent = false;
    trayRightClickStep = 2;
    trayCaptureTries = 0;
    trayCapturedMenuHwnd = nullptr;
    trayPopupSeenTries = 0;
    trayNativeClick = trayActionPoint;

    if (panel && trayAction == TrayAction::ExecuteMenu) {
        const LONG_PTR pex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
        SetWindowLongPtrW(panel, GWL_EXSTYLE, pex | WS_EX_TRANSPARENT);
    }

    triggerOffscreenContextMenu(*this, overflow, target.Get(), false);
    adoptGuardDetectedMenu(*this);

    if (trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) {
        if (trayAction == TrayAction::RightClickMenu) {
            auto items = extractPopupMenuItems(trayCapturedMenuHwnd);
            if (!items.empty()) {
                trayLastPopupHwnd = trayCapturedMenuHwnd;
                trayMenuItems = std::move(items);
                finishTrayMenu();
                return;
            }
        } else if (trayAction == TrayAction::ExecuteMenu) {
            if (invokePopupMenuItem(trayCapturedMenuHwnd, trayPendingExec, trayMenuItems)) {
                finishTrayMenu(true);
                return;
            }
        }
    }

    if (!SetTimer(hwnd, TraySyncTick, 10, nullptr)) {
        finishTrayMenu();
    }
}

bool Dock::invokeSystemTray() {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return false;
    return invokeTrayOverflow(taskbar, true);
}
