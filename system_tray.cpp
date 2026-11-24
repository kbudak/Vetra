#include "dock.h"
#include <UIAutomation.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "oleacc.lib")

namespace {
using Microsoft::WRL::ComPtr;

struct CloakedMenuRecord {
    HWND hwnd{};
    LONG_PTR origEx = 0;
    bool changedEx = false;
    bool changedRgn = false;
};

Dock* gTrayDock = nullptr;
HWND gCloakedOverflow = nullptr;
bool gRestoreLayered = false;
bool gRestoreOverflowRgn = false;
bool gInTraySync = false;
LONG_PTR gOrigExStyle = 0;
std::vector<CloakedMenuRecord> gCloakedMenus;

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

void moveOverflowOffscreen(HWND overflow) {
    if (!overflow) return;
    const int offX = GetSystemMetrics(SM_XVIRTUALSCREEN) - 4000;
    const int offY = GetSystemMetrics(SM_YVIRTUALSCREEN) - 4000;
    RECT rc{};
    if (GetWindowRect(overflow, &rc) && rc.left == offX && rc.top == offY) return;
    SetWindowPos(overflow, HWND_BOTTOM, offX, offY, 0, 0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void cloakMenuWindow(HWND w) {
    if (!w || !IsWindow(w)) return;
    auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
    CloakedMenuRecord rec{};
    const bool existed = (it != gCloakedMenus.end());
    if (existed) rec = *it;
    else rec.hwnd = w;

    // Empty window region hides even WS_EX_NOREDIRECTIONBITMAP (WinUI 3 / Electron DComp)
    // windows completely in DWM while keeping IsWindowVisible, HMENU, and UIA fully intact.
    if (!rec.changedRgn) {
        if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
            if (SetWindowRgn(w, empty, FALSE)) rec.changedRgn = true;
            else DeleteObject(empty);
        }
    }

    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    constexpr LONG_PTR kNoRedir = 0x00200000L; // WS_EX_NOREDIRECTIONBITMAP
    if (!rec.changedEx && (ex & kNoRedir) == 0) {
        SetLastError(0);
        SetWindowLongPtrW(w, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TOOLWINDOW);
        if (GetLastError() == 0) {
            rec.origEx = ex;
            rec.changedEx = true;
            SetLayeredWindowAttributes(w, 0, 1, LWA_ALPHA);
        }
    } else if (rec.changedEx) {
        SetLayeredWindowAttributes(w, 0, 1, LWA_ALPHA);
    }

    if (rec.changedEx || rec.changedRgn) {
        if (existed) *it = rec;
        else gCloakedMenus.push_back(rec);
    }
}

void uncloakMenuWindow(HWND w) {
    const auto it = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [w](const CloakedMenuRecord& r) { return r.hwnd == w; });
    if (it == gCloakedMenus.end()) return;
    const CloakedMenuRecord rec = *it;
    gCloakedMenus.erase(it);
    if (!w || !IsWindow(w)) return;
    if (rec.changedRgn) SetWindowRgn(w, nullptr, FALSE);
    if (rec.changedEx) {
        SetLayeredWindowAttributes(w, 0, 255, LWA_ALPHA);
        SetWindowLongPtrW(w, GWL_EXSTYLE, rec.origEx);
    }
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
        req->AddProperty(UIA_BoundingRectanglePropertyId);
        req->AddProperty(UIA_RuntimeIdPropertyId);
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

void snapshotTrayPreWindows(Dock& dock) {
    dock.trayPreWindows.clear();
    dock.trayIgnoredPopups.clear();
    dock.trayPreForeground = GetForegroundWindow();
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        auto* list = reinterpret_cast<std::vector<Dock::TrayWindowSnap>*>(lp);
        RECT rc{};
        GetWindowRect(w, &rc);
        const bool vis = IsWindowVisible(w) != FALSE && !isWindowCloakedByDwm(w) && !isWindowTransparentAlpha(w);
        list->push_back({w, vis, rc});
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dock.trayPreWindows));
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

int scoreTrayPopup(const Dock& dock, HWND w, bool fromShowEvent = false) {
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || isWindowCloakedByDwm(w)) return -1;
    if (std::find(dock.trayIgnoredPopups.begin(), dock.trayIgnoredPopups.end(), w) != dock.trayIgnoredPopups.end())
        return -1;
    wchar_t cls[96]{};
    DWORD winPid = 0;
    if (isExcludedSystemWindow(w, dock, cls, winPid)) return -1;
    if (dock.trayMenuOwnerPid && dock.trayMenuOwnerPid != winPid) return -1;

    RECT rc{};
    if (!GetWindowRect(w, &rc) || !isOnAnyMonitor(rc)) return -1;
    const int rw = rc.right - rc.left, rh = rc.bottom - rc.top;
    if (rw < 36 || rh < 16 || rw > 1280 || rh > 1100) return -1;

    const bool win32Menu = wcscmp(cls, L"#32768") == 0;
    const LONG_PTR style = GetWindowLongPtrW(w, GWL_STYLE);
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    const HWND fg = GetForegroundWindow();
    const bool isFg = (w == fg || (fg && GetAncestor(fg, GA_ROOT) == w));

    const auto snap = std::find_if(dock.trayPreWindows.begin(), dock.trayPreWindows.end(),
        [w](const Dock::TrayWindowSnap& s) { return s.hwnd == w; });
    const bool newlyVisible = fromShowEvent ||
        (dock.trayAction == Dock::TrayAction::ExecuteMenu && w == dock.trayLastPopupHwnd) ||
        (snap == dock.trayPreWindows.end() || !snap->visible);
    bool movedOnScreen = false;
    if (!newlyVisible && snap != dock.trayPreWindows.end()) {
        const int sw = snap->rect.right - snap->rect.left;
        const int sh = snap->rect.bottom - snap->rect.top;
        const bool wasHiddenGeo = (sw <= 4 || sh <= 4 || !isOnAnyMonitor(snap->rect));
        movedOnScreen = wasHiddenGeo || (!EqualRect(&snap->rect, &rc) && isFg);
    }
    const bool newlyFg = isFg && w != dock.trayPreForeground &&
        (win32Menu || (style & WS_POPUP) != 0 || (ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0 ||
         wcsstr(cls, L"WinUI") != nullptr || wcsstr(cls, L"Popup") != nullptr ||
         wcsstr(cls, L"Menu") != nullptr || wcsstr(cls, L"Tray") != nullptr ||
         wcsstr(cls, L"Xaml") != nullptr);

    if (!newlyVisible && !movedOnScreen && !newlyFg) return -1;

    int score = 10;
    if (win32Menu) score += 140;
    if (dock.trayMenuOwnerPid && dock.trayMenuOwnerPid == winPid) score += 90;
    if (isFg) score += 45;
    if ((style & WS_POPUP) != 0) score += 35;
    if ((ex & (WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) != 0) score += 20;
    if (wcsncmp(cls, L"WindowsForms10.", 15) == 0 || wcsncmp(cls, L"HwndWrapper[", 12) == 0 ||
        wcsncmp(cls, L"Qt", 2) == 0 || wcsncmp(cls, L"Chrome_WidgetWin_", 17) == 0 ||
        wcsncmp(cls, L"WinUIDesktop", 12) == 0 || wcsncmp(cls, L"Xaml_WindowedPopup", 18) == 0 ||
        wcsstr(cls, L"Popup") != nullptr || wcsstr(cls, L"Menu") != nullptr ||
        wcsstr(cls, L"Flyout") != nullptr || wcsstr(cls, L"Tray") != nullptr)
        score += 25;

    const auto distTo = [&](POINT pt) {
        const int dx = pt.x < rc.left ? rc.left - pt.x : (pt.x > rc.right ? pt.x - rc.right : 0);
        const int dy = pt.y < rc.top ? rc.top - pt.y : (pt.y > rc.bottom ? pt.y - rc.bottom : 0);
        return std::max(dx, dy);
    };
    const int dist = std::min(distTo(dock.trayNativeClick), distTo(dock.trayActionPoint));
    const int dpi = std::max<int>(96, static_cast<int>(GetDpiForWindow(dock.hwnd)));
    if (dist <= MulDiv(80, dpi, 96)) score += 30;
    else if (dist <= MulDiv(320, dpi, 96)) score += 15;
    else score += 5;
    return score;
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

void CALLBACK trayWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!gTrayDock || !hwnd || gInTraySync) return;
    if (idObject == OBJID_WINDOW && isOverflowWindow(hwnd)) {
        if (gTrayDock->trayAction == Dock::TrayAction::Capture ||
            gTrayDock->trayAction == Dock::TrayAction::LeftClick ||
            ((gTrayDock->trayAction == Dock::TrayAction::RightClickMenu ||
              gTrayDock->trayAction == Dock::TrayAction::ExecuteMenu) &&
             !gTrayDock->trayRightClickPending && gTrayDock->trayRightClickStep == 0)) {
            gTrayDock->trayOverflow = hwnd;
            moveOverflowOffscreen(hwnd);
            if (gTrayDock->trayNeedsReveal) {
                if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb))
                    ShowWindowAsync(tb, SW_HIDE);
            }
        }
        return;
    }
    if ((gTrayDock->trayAction == Dock::TrayAction::RightClickMenu ||
         gTrayDock->trayAction == Dock::TrayAction::ExecuteMenu) &&
        gTrayDock->trayRightClickPending) {
        // Cloak #32768 menus and suppress SysShadow drop-shadows BEFORE Windows paints frame 0.
        if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW) && idObject == OBJID_WINDOW) {
            wchar_t cls[32]{};
            if (GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) > 0) {
                if (wcscmp(cls, L"SysShadow") == 0) {
                    ShowWindowAsync(hwnd, SW_HIDE);
                    return;
                }
                if (wcscmp(cls, L"#32768") == 0) {
                    cloakMenuWindow(hwnd);
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
                PostMessageW(gTrayDock->hwnd, WM_TIMER, 10, 0);
            }
        }
    }
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

bool tryInvokeAsync(IUIAutomationElement* element) {
    if (!element) return false;

    UIA_HWND native = nullptr;
    if (SUCCEEDED(element->get_CurrentNativeWindowHandle(&native)) && native) {
        HWND child = reinterpret_cast<HWND>(native);
        if (IsWindow(child)) {
            wchar_t cls[32]{};
            if (GetClassNameW(child, cls, ARRAYSIZE(cls)) > 0 && _wcsicmp(cls, L"Button") == 0) {
                PostMessageW(child, BM_CLICK, 0, 0);
                return true;
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
    if (!supported) return false;

    IStream* stream = nullptr;
    if (FAILED(CoMarshalInterThreadInterfaceInStream(__uuidof(IUIAutomationElement), element, &stream)) || !stream)
        return tryInvoke(element);

    std::thread([stream] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IUIAutomationElement> target;
        if (SUCCEEDED(CoGetInterfaceAndReleaseStream(stream, IID_PPV_ARGS(&target))) && target) {
            tryInvoke(target.Get());
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }).detach();
    return true;
}

bool findAndInvokeInRoot(IUIAutomation* automation, IUIAutomationCacheRequest* cacheReq,
                         IUIAutomationElement* root, bool requireVisibleBounds) {
    if (!root || !automation || !cacheReq) return false;
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
        } else if (requireVisibleBounds && visible && normalTrayBtn && rc.left < leftmostX) {
            leftmostX = rc.left;
            leftmostNormal = el;
        }

        if (nameBstr) SysFreeString(nameBstr);
        if (idBstr) SysFreeString(idBstr);
        if (clsBstr) SysFreeString(clsBstr);
        if (invoked) return true;
    }

    return requireVisibleBounds && leftmostNormal && tryInvoke(leftmostNormal.Get());
}

bool invokeTrayOverflowWithUia(IUIAutomation* automation, HWND taskbar, bool requireVisibleBounds) {
    if (!automation || !taskbar) return false;
    auto cacheReq = createChevronButtonCacheReq(automation);
    if (!cacheReq) return false;

    // Prioritize taskbar root and its XAML bridge / tray notify child windows to avoid redundant walks.
    std::vector<HWND> preferred;
    std::vector<HWND> fallback;
    preferred.push_back(taskbar);
    struct Gather { std::vector<HWND>* pref; std::vector<HWND>* fall; } g{&preferred, &fallback};
    EnumChildWindows(taskbar, [](HWND child, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<Gather*>(lp);
        wchar_t cls[96]{};
        if (GetClassNameW(child, cls, ARRAYSIZE(cls)) > 0 &&
            (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0 ||
             wcscmp(cls, L"TrayNotifyWnd") == 0 ||
             wcscmp(cls, L"ReBarWindow32") == 0)) {
            ctx->pref->push_back(child);
        } else {
            ctx->fall->push_back(child);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&g));

    for (HWND target : preferred) {
        ComPtr<IUIAutomationElement> root;
        if (SUCCEEDED(automation->ElementFromHandle(target, &root)) && root &&
            findAndInvokeInRoot(automation, cacheReq.Get(), root.Get(), requireVisibleBounds))
            return true;
    }
    for (HWND target : fallback) {
        ComPtr<IUIAutomationElement> root;
        if (SUCCEEDED(automation->ElementFromHandle(target, &root)) && root &&
            findAndInvokeInRoot(automation, cacheReq.Get(), root.Get(), requireVisibleBounds))
            return true;
    }
    return false;
}

bool invokeTrayOverflow(HWND taskbar, bool requireVisibleBounds) {
    return invokeTrayOverflowWithUia(sharedUia(), taskbar, requireVisibleBounds);
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
    Text tip;
    Text tipLower;
    Text exePath;
    Text exeStemLower;
    bool exeExists = false;
    std::vector<BYTE> png;
    int pngW = 0;
    int pngH = 0;
};

std::vector<NotifyIconEntry> loadNotifyIconRegistry() {
    std::vector<NotifyIconEntry> entries;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return entries;

    wchar_t subName[128]{};
    for (DWORD idx = 0;; ++idx) {
        DWORD nameLen = ARRAYSIZE(subName);
        if (RegEnumKeyExW(root, idx, subName, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, subName, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;

        NotifyIconEntry e{};
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
            const DWORD attr = GetFileAttributesW(e.exePath.c_str());
            e.exeExists = (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
        }
        DWORD pngBytes = 0;
        if (RegGetValueW(sub, nullptr, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &pngBytes) == ERROR_SUCCESS && pngBytes > 24) {
            e.png.resize(pngBytes);
            if (RegGetValueW(sub, nullptr, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, e.png.data(), &pngBytes) == ERROR_SUCCESS) {
                static const BYTE kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
                if (e.png.size() >= 24 && memcmp(e.png.data(), kPngSig, 8) == 0) {
                    e.pngW = (e.png[16] << 24) | (e.png[17] << 16) | (e.png[18] << 8) | e.png[19];
                    e.pngH = (e.png[20] << 24) | (e.png[21] << 16) | (e.png[22] << 8) | e.png[23];
                }
            } else {
                e.png.clear();
            }
        }
        RegCloseKey(sub);
        if (!e.tip.empty() || !e.exePath.empty() || !e.png.empty())
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
        rank += std::min(e.pngW, 512);
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

    // 1. Dynamic Windows status icons (SecurityHealth, Intel RST, Safely Remove):
    // prefer LIVE PrintWindow capture first so the current green check / warning badge stays accurate.
    if (statusIcon) {
        if (printCaptured) return printCaptured;
        if (reg && !reg->png.empty()) {
            if (auto bmp = decodePngNormalized(reg->png)) return bmp;
        }
    }

    // 2. High-res true-alpha IconSnapshot (>=48px: e.g. Antigravity 512px, NVIDIA 128px, Veloce 64px).
    // Strip opaque dark outer square (e.g. NVIDIA's baked dark grey box) so the logo floats cleanly.
    if (reg && !reg->png.empty() && reg->pngW >= 48) {
        if (auto bmp = decodePngNormalized(reg->png, true)) return bmp;
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
    if (reg && !reg->png.empty() && reg->pngW >= 24) {
        if (auto bmp = decodePngNormalized(reg->png, true)) return bmp;
    }

    // 5. Cleaned PrintWindow capture, then any remaining IconSnapshot.
    if (printCaptured) return printCaptured;
    if (reg && !reg->png.empty()) {
        if (auto bmp = decodePngNormalized(reg->png, true)) return bmp;
    }
    return nullptr;
}

std::shared_ptr<Gdiplus::Bitmap> lookupNotifyIconSnapshot(const Text& tip) {
    const auto entries = loadNotifyIconRegistry();
    return resolveCrispTrayIcon(tip, nullptr, entries, nullptr);
}
}

std::shared_ptr<Gdiplus::Bitmap> resolveTrayIconBitmap(const Text& name, HICON vetraIcon) {
    const auto entries = cachedNotifyIconRegistry(false);
    return resolveCrispTrayIcon(name, vetraIcon, entries, nullptr);
}

namespace {

std::shared_ptr<Gdiplus::Bitmap> lookupCachedTrayIcon(const Text& name) {
    const Text key = toLowerText(name.c_str());
    if (isDynamicStatusTrayIcon(key, {})) return nullptr;
    std::lock_guard lock(gTrayCacheMutex);
    const auto it = gTrayIconBitmapCache.find(key);
    return it != gTrayIconBitmapCache.end() ? it->second : nullptr;
}

void storeCachedTrayIcon(const Text& name, const std::shared_ptr<Gdiplus::Bitmap>& bmp) {
    if (!bmp) return;
    const Text key = toLowerText(name.c_str());
    if (isDynamicStatusTrayIcon(key, {})) return;
    std::lock_guard lock(gTrayCacheMutex);
    if (gTrayIconBitmapCache.size() > 96) gTrayIconBitmapCache.clear();
    gTrayIconBitmapCache[key] = bmp;
}

struct OverflowButton {
    Text name;
    RECT rect{};
    std::vector<int> runtimeId;
    ComPtr<IUIAutomationElement> element;
};

std::vector<OverflowButton> collectOverflowButtonsWithUia(IUIAutomation* automation, HWND overflow) {
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

    std::vector<HWND> roots;
    roots.push_back(overflow);
    EnumChildWindows(overflow, [](HWND child, LPARAM lp) -> BOOL {
        reinterpret_cast<std::vector<HWND>*>(lp)->push_back(child);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&roots));

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
            if (FAILED(el->get_CachedBoundingRectangle(&rc)) || rc.right <= rc.left || rc.bottom <= rc.top) continue;
            const int rw = rc.right - rc.left, rh = rc.bottom - rc.top;
            if (rw < 16 || rh < 16 || rw > 128 || rh > 128) continue;

            BSTR nameBstr{};
            el->get_CachedName(&nameBstr);
            Text name = nameBstr ? nameBstr : L"";
            if (nameBstr) SysFreeString(nameBstr);
            if (name.empty()) continue;

            std::vector<int> rid;
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

            const bool duplicate = std::any_of(items.begin(), items.end(), [&](const OverflowButton& existing) {
                if (!rid.empty() && existing.runtimeId == rid) return true;
                return existing.name == name &&
                    std::abs(existing.rect.left - rc.left) <= 8 &&
                    std::abs(existing.rect.top - rc.top) <= 8;
            });
            if (!duplicate) items.push_back({std::move(name), rc, std::move(rid), el});
        }
        if (!items.empty()) break;
    }

    std::sort(items.begin(), items.end(), [](const OverflowButton& a, const OverflowButton& b) {
        if (std::abs(a.rect.top - b.rect.top) > 12) return a.rect.top < b.rect.top;
        return a.rect.left < b.rect.left;
    });
    return items;
}

std::vector<OverflowButton> collectOverflowButtons(HWND overflow) {
    return collectOverflowButtonsWithUia(sharedUia(), overflow);
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

bool tryCaptureClassicTray(std::vector<TrayItem>& out, bool forceReload = false) {
    const HWND overflow = FindWindowW(L"NotifyIconOverflowWindow", nullptr);
    const HWND toolbar = overflow ? FindWindowExW(overflow, nullptr, L"ToolbarWindow32", nullptr) : nullptr;
    if (!toolbar) return false;

    const int count = static_cast<int>(SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0));
    if (count <= 0) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(toolbar, &pid);
    if (!pid) return false;

    const HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process) return false;

    constexpr SIZE_T bufBytes = 4096;
    void* remote = VirtualAllocEx(process, nullptr, bufBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(process);
        return false;
    }

    struct TrayData64 {
        HWND hwnd;
        UINT uID;
        UINT uCallbackMessage;
        DWORD reserved[2];
        HICON hIcon;
    };

    const auto regEntries = cachedNotifyIconRegistry(forceReload);
    std::vector<TrayItem> items;
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
        items.push_back({name, std::move(icon), td.hwnd, td.uID, td.uCallbackMessage});
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
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
        openItem.label = L"Kaspersky VPN";
        openItem.rawName = openItem.label;
        openItem.cmdId = 9002;
        openItem.isDefault = true;
        result.push_back(std::move(openItem));

        TrayMenuItem exitItem{};
        exitItem.label = L"Çık";
        exitItem.rawName = exitItem.label;
        exitItem.cmdId = 9003;
        exitItem.rawMenuIndex = 1;
        result.push_back(std::move(exitItem));
        return result;
    }

    const wchar_t* labels[] = {
        L"Kaspersky Premium",
        L"Korumayı duraklat",
        L"Ayarlar",
        L"Rahatsız Etme Modunu Aç",
        L"Oyun Modunu aç",
        L"Müşteri hizmetleri",
        L"Hakkında",
        L"Çık"
    };
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(labels)); ++i) {
        TrayMenuItem item{};
        item.label = labels[i];
        item.rawName = labels[i];
        item.cmdId = (i == 0) ? 9002 : (9100 + i);
        item.rawMenuIndex = i ? i : -1;
        item.isDefault = (i == 0);
        result.push_back(std::move(item));
    }
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
        items.push_back(std::move(item));
    };
    addApp(L"NVIDIA App", L"shell:AppsFolder\\com.nvidia.nvapp",
        L"\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe");
    addApp(L"NVIDIA Denetim Masası", L"shell:AppsFolder\\NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIACorp.NVIDIAControlPanel",
        L"\\NVIDIA Corporation\\Control Panel Client\\nvcplui.exe");
    if (!items.empty()) {
        TrayMenuItem exit{};
        exit.label = L"Çık · Windows erişimi kısıtlı";
        exit.disabled = true;
        items.push_back(std::move(exit));
    }
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

void dismissPopupMenu(HWND menuHwnd) {
    if (!menuHwnd || !IsWindow(menuHwnd)) return;
    uncloakMenuWindow(menuHwnd);
    if (isWin32MenuClass(menuHwnd)) {
        DWORD_PTR r = 0;
        SendMessageTimeoutW(menuHwnd, 0x01E6 /*MN_CANCELMENUS*/, 0, 0, SMTO_ABORTIFHUNG, 60, &r);
        PostMessageW(menuHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
        PostMessageW(menuHwnd, WM_KEYUP, VK_ESCAPE, 0);
        return;
    }
    // Never send WM_CLOSE to non-#32768 tray popups (e.g. Logitech G HUB MainWindow).
    PostMessageW(menuHwnd, WM_CANCELMODE, 0, 0);
    PostMessageW(menuHwnd, WM_KEYDOWN, VK_ESCAPE, 0);
    PostMessageW(menuHwnd, WM_KEYUP, VK_ESCAPE, 0);
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

        if (u.hasSubmenu && u.enabled && depth < 2 && expandedSubmenus < 6 && u.element && ownerPid) {
            ++expandedSubmenus;
            const auto before = snapshotVisibleForPid(ownerPid);
            ComPtr<IUIAutomationExpandCollapsePattern> expand;
            bool opened = false;
            bool usedHover = false;
            POINT savedCursor{};
            if (SUCCEEDED(u.element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expand))) && expand &&
                SUCCEEDED(expand->Expand())) {
                opened = true;
            } else if (!IsRectEmpty(&u.rect)) {
                GetCursorPos(&savedCursor);
                SetCursorPos((u.rect.left + u.rect.right) / 2, (u.rect.top + u.rect.bottom) / 2);
                opened = usedHover = true;
            }
            if (opened) {
                HWND subPopup = nullptr;
                std::vector<UiaMenuEntry> subEntries;
                for (int wait = 0; wait < 4; ++wait) {
                    subPopup = findNewPopupForPid(ownerPid, before);
                    if (subPopup) {
                        cloakMenuWindow(subPopup);
                        subEntries = collectUiaMenuEntriesDeep(subPopup, false);
                        if (!subEntries.empty()) break;
                    }
                    subEntries = collectUiaMenuEntriesDeep(menuHwnd, true, u.element.Get());
                    if (!subEntries.empty()) break;
                    Sleep(14);
                }
                if (!subEntries.empty()) {
                    std::vector<Text> nextParents = parentLabels;
                    nextParents.push_back(item.label);
                    item.children = convertUiaEntries(subPopup ? subPopup : menuHwnd, subEntries, nextParents, depth + 1);
                }
                if (expand) expand->Collapse();
                if (usedHover) SetCursorPos(savedCursor.x, savedCursor.y);
                if (subPopup && IsWindowVisible(subPopup)) dismissPopupMenu(subPopup);
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

bool clickMenuRow(HWND menuWindow, RECT row) {
    if (IsRectEmpty(&row)) return false;
    RECT winRect{};
    if (!GetWindowRect(menuWindow, &winRect)) return false;
    const POINT center{(row.left + row.right) / 2, (row.top + row.bottom) / 2};

    // Temporarily open a 3x3 pixel hit-test aperture strictly under the cursor if the popup was region-cloaked.
    const auto cloakedIt = std::find_if(gCloakedMenus.begin(), gCloakedMenus.end(),
        [menuWindow](const CloakedMenuRecord& r) { return r.hwnd == menuWindow && r.changedRgn; });
    const bool hadEmptyRgn = (cloakedIt != gCloakedMenus.end());
    if (hadEmptyRgn) {
        const int lx = center.x - winRect.left;
        const int ly = center.y - winRect.top;
        if (HRGN spot = CreateRectRgn(lx - 1, ly - 1, lx + 2, ly + 2)) {
            if (!SetWindowRgn(menuWindow, spot, FALSE)) DeleteObject(spot);
        }
    }

    SetWindowPos(menuWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const HWND hit = WindowFromPoint(center);
    bool sent = false;
    if (!hit || GetAncestor(hit, GA_ROOT) == menuWindow || hit == menuWindow) {
        POINT saved{};
        GetCursorPos(&saved);
        SetCursorPos(center.x, center.y);
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        sent = SendInput(ARRAYSIZE(click), click, sizeof(INPUT)) == ARRAYSIZE(click);
        SetCursorPos(saved.x, saved.y);
    }

    if (hadEmptyRgn && IsWindow(menuWindow)) {
        if (HRGN empty = CreateRectRgn(0, 0, 0, 0)) {
            if (!SetWindowRgn(menuWindow, empty, FALSE)) DeleteObject(empty);
        }
    }
    return sent;
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
            if (entry.separator || entry.disabled || cleanMenuLabel(entry.name) != item.label) continue;
            const int idx = static_cast<int>(entry.child) - 1;
            if (idx >= 0 && idx < count && usable(idx)) return idx;
        }
    }

    if (item.rawMenuIndex >= 0 && item.rawMenuIndex < count && usable(item.rawMenuIndex)) {
        wchar_t raw[16]{};
        if (GetMenuStringW(menu, item.rawMenuIndex, raw, ARRAYSIZE(raw), MF_BYPOSITION) == 0)
            return item.rawMenuIndex;
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
            DWORD_PTR unused{};
            SendMessageTimeoutW(menuWindow, 0x01E5 /*MN_SELECTITEM*/, index, 0, SMTO_ABORTIFHUNG, 100, &unused);
            PostMessageW(menuWindow, 0x01ED /*MN_BUTTONDOWN*/, index, 0);
            PostMessageW(menuWindow, 0x01EF /*MN_BUTTONUP*/, index, 0);
            Sleep(18);
            if (!IsWindowVisible(popup)) return true;
            RECT row{};
            if (GetMenuItemRect(nullptr, menu, index, &row) && clickMenuRow(menuWindow, row)) return true;
            return true;
        }
        if (menuWindow != popup) return false;
    }

    if (!item.parentPath.empty()) return false;

    HWND activePopup = popup;
    ComPtr<IUIAutomationElement> scopeRoot;
    DWORD ownerPid = 0;
    GetWindowThreadProcessId(popup, &ownerPid);
    for (const Text& parentLabel : item.parentLabels) {
        const auto curEntries = collectUiaMenuEntriesDeep(activePopup, false, scopeRoot.Get());
        const auto it = std::find_if(curEntries.begin(), curEntries.end(), [&](const UiaMenuEntry& e) {
            return !e.separator && e.enabled && cleanMenuLabel(e.name) == parentLabel;
        });
        if (it == curEntries.end() || !it->element) return false;
        const auto before = snapshotVisibleForPid(ownerPid);
        ComPtr<IUIAutomationExpandCollapsePattern> expand;
        POINT savedCursor{};
        bool movedCursor = false;
        if (SUCCEEDED(it->element->GetCurrentPatternAs(UIA_ExpandCollapsePatternId, IID_PPV_ARGS(&expand))) && expand &&
            SUCCEEDED(expand->Expand())) {
        } else if (!IsRectEmpty(&it->rect)) {
            GetCursorPos(&savedCursor);
            SetCursorPos((it->rect.left + it->rect.right) / 2, (it->rect.top + it->rect.bottom) / 2);
            movedCursor = true;
        }
        HWND nextPopup = nullptr;
        for (int wait = 0; wait < 5; ++wait) {
            nextPopup = findNewPopupForPid(ownerPid, before);
            if (nextPopup) { cloakMenuWindow(nextPopup); break; }
            if (!collectUiaMenuEntriesDeep(activePopup, true, it->element.Get()).empty()) break;
            Sleep(14);
        }
        if (movedCursor) SetCursorPos(savedCursor.x, savedCursor.y);
        if (nextPopup) {
            activePopup = nextPopup;
            scopeRoot.Reset();
        } else {
            scopeRoot = it->element;
        }
    }

    for (const auto& entry : collectUiaMenuEntriesDeep(activePopup, false, scopeRoot.Get())) {
        if (entry.separator || !entry.enabled || cleanMenuLabel(entry.name) != item.label) continue;
        if (tryInvokeAsync(entry.element.Get()) || clickMenuRow(activePopup, entry.rect)) return true;
    }

    if (item.parentLabels.empty()) {
        if (const auto acc = accessiblePopup(popup)) {
            const auto entries = collectMsaaMenuEntries(popup);
            for (const auto& entry : entries) {
                if (entry.separator || entry.disabled || entry.hasSubmenu || cleanMenuLabel(entry.name) != item.label) continue;
                VARIANT child{};
                child.vt = VT_I4;
                child.lVal = entry.child;
                if (acc->accDoDefaultAction(child) == S_OK || clickMenuRow(popup, entry.rect)) return true;
            }
        }
    }
    return false;
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

void Dock::stopTraySync() {
    KillTimer(hwnd, 10);
    if (trayCaptureCancel) {
        trayCaptureCancel->store(true);
        trayCaptureCancel.reset();
    }
    ++trayCaptureGen;
    if (trayHook) {
        UnhookWinEvent(trayHook);
        trayHook = nullptr;
    }
    if (gTrayDock == this) gTrayDock = nullptr;
    if (trayCapturedMenuHwnd && IsWindowVisible(trayCapturedMenuHwnd)) dismissPopupMenu(trayCapturedMenuHwnd);
    while (!gCloakedMenus.empty()) uncloakMenuWindow(gCloakedMenus.back().hwnd);
    if (gRestoreLayered || gRestoreOverflowRgn) {
        HWND overflow = (gCloakedOverflow && IsWindow(gCloakedOverflow)) ? gCloakedOverflow : findAnyOverflow();
        if (overflow && IsWindow(overflow)) {
            if (gRestoreOverflowRgn) SetWindowRgn(overflow, nullptr, FALSE);
            if (gRestoreLayered) {
                SetLayeredWindowAttributes(overflow, 0, 255, LWA_ALPHA);
                SetWindowLongPtrW(overflow, GWL_EXSTYLE, gOrigExStyle);
            }
            moveOverflowOffscreen(overflow);
            if (IsWindowVisible(overflow)) PostMessageW(overflow, WM_CLOSE, 0, 0);
        }
        gCloakedOverflow = nullptr;
        gRestoreLayered = false;
        gRestoreOverflowRgn = false;
    } else if (trayOverflow && IsWindowVisible(trayOverflow)) {
        moveOverflowOffscreen(trayOverflow);
        PostMessageW(trayOverflow, WM_CLOSE, 0, 0);
    }
    trayAction = TrayAction::None;
    trayOverflow = nullptr;
    trayRightClickPending = false;
    trayRightClickStep = 0;
    trayPopupSeenTries = 0;
    trayOverflowSettled = false;
    trayLastOvRect = {};
    trayCapturedMenuHwnd = nullptr;
    trayPreForeground = nullptr;
    trayPreWindows.clear();
    trayIgnoredPopups.clear();
    trayMenuOwnerPid = 0;
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
        gTrayDock = this;
        trayHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            trayWinEventProc, explorerPid, 0, WINEVENT_OUTOFCONTEXT);
        if (const HWND existing = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr))
            moveOverflowOffscreen(existing);
    }

    const uint64_t gen = ++trayCaptureGen;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    trayCaptureCancel = cancel;
    const HWND notify = hwnd;
    const HICON vetraIcon = appIcon;
    const auto stop = extra.proStop;

    try {
        trayCaptureRequest = std::async(std::launch::async,
            [gen, cancel, stop, notify, taskbar, vetraIcon, forceReload]() -> TrayCaptureResult {
                TrayCaptureResult res{};
                res.generation = gen;
                const auto aborted = [&]() { return cancel->load() || (stop && stop->load()); };
                if (aborted()) return res;

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
                    const auto waitForOverflow = [&](int maxMs) -> HWND {
                        for (int waited = 0; waited < maxMs && !aborted(); waited += 15) {
                            if (HWND ov = findVisibleOverflow()) return ov;
                            Sleep(15);
                        }
                        return aborted() ? nullptr : findVisibleOverflow();
                    };

                    HWND overflow = findVisibleOverflow();
                    if (!overflow && uia && !aborted()) {
                        if (invokeTrayOverflowWithUia(uia.Get(), taskbar, false))
                            overflow = waitForOverflow(120);
                    }
                    if (!overflow && !aborted()) {
                        if (!IsWindowVisible(taskbar)) {
                            ShowWindowAsync(taskbar, SW_SHOWNOACTIVATE);
                            revealed = true;
                            Sleep(25);
                        }
                        if (uia && invokeTrayOverflowWithUia(uia.Get(), taskbar, true))
                            overflow = waitForOverflow(180);
                        if (!overflow && uia && !aborted() && invokeTrayOverflowWithUia(uia.Get(), taskbar, true))
                            overflow = waitForOverflow(120);
                        if (!overflow && !aborted() && invokeViaWinBWithUia(uia.Get(), taskbar))
                            overflow = waitForOverflow(150);
                    }

                    if (overflow && !aborted()) {
                        moveOverflowOffscreen(overflow);
                        if (revealed) {
                            ShowWindowAsync(taskbar, SW_HIDE);
                            res.revealedTaskbar = true;
                        }
                        RECT prevRc{};
                        GetWindowRect(overflow, &prevRc);
                        Sleep(35);
                        for (int settle = 0; settle < 4 && !aborted(); ++settle) {
                            RECT curRc{};
                            if (GetWindowRect(overflow, &curRc) && EqualRect(&curRc, &prevRc) && !IsRectEmpty(&curRc))
                                break;
                            prevRc = curRc;
                            Sleep(18);
                        }

                        std::vector<OverflowButton> buttons;
                        for (int tries = 0; tries < 8 && !aborted(); ++tries) {
                            buttons = collectOverflowButtonsWithUia(uia.Get(), overflow);
                            if (!buttons.empty()) break;
                            Sleep(20);
                        }

                        if (!buttons.empty() && !aborted()) {
                            bool needPrintWindow = forceReload;
                            if (!needPrintWindow) {
                                for (const auto& b : buttons) {
                                    const Text low = toLowerText(b.name.c_str());
                                    if (isDynamicStatusTrayIcon(low, {}) || !lookupCachedTrayIcon(b.name)) {
                                        needPrintWindow = true;
                                        break;
                                    }
                                }
                            }
                            std::vector<std::shared_ptr<Gdiplus::Bitmap>> bitmaps;
                            if (needPrintWindow) {
                                for (int tries = 0; tries < 4 && !aborted(); ++tries) {
                                    if (captureOverflowIcons(overflow, buttons, bitmaps)) break;
                                    Sleep(20);
                                }
                            }
                            // Dismiss overflow immediately once pixels/buttons are read.
                            PostMessageW(overflow, WM_CLOSE, 0, 0);

                            std::vector<NotifyIconEntry> regEntries;
                            bool regLoaded = false;
                            bool regRefreshed = forceReload;
                            res.items.reserve(buttons.size());
                            for (size_t i = 0; i < buttons.size() && !aborted(); ++i) {
                                auto captured = (i < bitmaps.size()) ? bitmaps[i] : nullptr;
                                std::shared_ptr<Gdiplus::Bitmap> icon = forceReload ? nullptr : lookupCachedTrayIcon(buttons[i].name);
                                if (!icon) {
                                    if (!regLoaded) {
                                        regEntries = cachedNotifyIconRegistry(forceReload);
                                        regLoaded = true;
                                    }
                                    if (!regRefreshed && !matchNotifyRegistry(regEntries, buttons[i].name)) {
                                        regEntries = cachedNotifyIconRegistry(true);
                                        regRefreshed = true;
                                    }
                                    icon = resolveCrispTrayIcon(buttons[i].name, vetraIcon, regEntries, captured);
                                    storeCachedTrayIcon(buttons[i].name, icon);
                                }
                                res.items.push_back({buttons[i].name, std::move(icon), nullptr, 0, 0});
                            }
                            res.ok = !res.items.empty();
                        } else {
                            PostMessageW(overflow, WM_CLOSE, 0, 0);
                        }
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
    if (!trayCaptureRequest.valid() ||
        trayCaptureRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;

    TrayCaptureResult res{};
    try { res = trayCaptureRequest.get(); }
    catch (const std::exception&) { res.ok = false; }

    if (res.generation != trayCaptureGen) return;
    if (trayAction == TrayAction::Capture) {
        if (trayHook) {
            UnhookWinEvent(trayHook);
            trayHook = nullptr;
        }
        if (gTrayDock == this) gTrayDock = nullptr;
        trayAction = TrayAction::None;
        trayCaptureCancel.reset();
    }
    if (res.revealedTaskbar && !nativeBarRevealed) shellTaskbar.hide();

    if (res.ok) {
        const auto gridDims = [](size_t c) -> std::pair<int, int> {
            const int cnt = static_cast<int>(c);
            if (cnt == 0) return {0, 0};
            const int cols = cnt < 5 ? cnt : 5;
            const int rows = std::min(4, (cnt + cols - 1) / cols);
            return {cols, rows};
        };
        const bool dimsChanged = trayIcons.empty() || gridDims(trayIcons.size()) != gridDims(res.items.size());
        trayIcons = std::move(res.items);
        trayLoading = false;
        if (panel && page == SystemTray && !sheetClosing && trayMenuIconIndex < 0) {
            if (dimsChanged) {
                triggerSheetContentAnim(0.f, 5.f);
                positionSheet();
            } else {
                paintSheet();
            }
            const HWND fg = GetForegroundWindow();
            if (!fg || isOverflowWindow(fg) || fg == FindWindowW(L"Shell_TrayWnd", nullptr))
                SetForegroundWindow(panel);
        }
    } else {
        trayLoading = false;
        if (panel && page == SystemTray && !sheetClosing && trayMenuIconIndex < 0) paintSheet();
    }
}

void Dock::prewarmTrayCaches() {
    if (trayWarmupDone) return;
    trayWarmupDone = true;
    const auto stop = extra.proStop;
    std::thread([stop] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        if (!stop || !stop->load()) cachedNotifyIconRegistry(false);
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
        // Elevated SYSTEM-owned tray icons (NVIDIA, Monster) cannot be cloaked cross-process due to UIPI.
        std::vector<TrayMenuItem> fastMenu = buildNvidiaTrayMenu(item.name);
        if (fastMenu.empty() && containsCI(item.name.c_str(), L"monster")) fastMenu = buildMonsterControlMenu();
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
    stopTraySync();
    trayAction = action;
    trayActionIndex = index;
    trayActionName = item.name;
    trayActionPoint = screenPt;
    trayLocalOffset = {};
    trayCaptureTries = 0;
    trayPopupSeenTries = 0;
    trayNeedsReveal = false;
    trayRightClickPending = false;
    trayRightClickStep = 0;
    trayOverflowSettled = false;
    trayLastOvRect = {};
    trayCapturedMenuHwnd = nullptr;
    trayRightClickStartedAt = GetTickCount64();

    if (action == TrayAction::RightClickMenu) {
        // Keep the icon grid steady while capturing so Vetra's panel never flashes/resizes twice.
        trayPendingMenuIndex = index;
        trayMenuPath.clear();
        trayMenuLoading = false;
        trayMenuUnavailable = false;
        trayMenuItems.clear();
        trayLastPopupHwnd = nullptr;
        listOffset = 0;
    } else if (action == TrayAction::LeftClick) {
        close();
    }

    gTrayDock = this;
    trayHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        trayWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);

    if (action != TrayAction::LeftClick && IsWindow(item.targetHwnd)) {
        if (panel && action == TrayAction::ExecuteMenu) {
            const LONG_PTR pex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
            SetWindowLongPtrW(panel, GWL_EXSTYLE, pex | WS_EX_TRANSPARENT);
        }
        snapshotTrayPreWindows(*this);
        GetWindowThreadProcessId(item.targetHwnd, &trayMenuOwnerPid);
        AllowSetForegroundWindow(trayMenuOwnerPid);
        trayNativeClick = screenPt;
        trayRightClickStep = 2;
        trayRightClickPending = true;
        if (PostMessageW(item.targetHwnd, item.callbackMsg, item.uid, WM_RBUTTONUP) &&
            SetTimer(hwnd, 10, 16, nullptr)) return;
        finishTrayMenu();
        return;
    }

    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) { finishTrayMenu(); return; }
    if (const HWND existing = FindWindowW(L"TopLevelWindowForOverflowXamlIsland", nullptr))
        moveOverflowOffscreen(existing);

    if (findVisibleOverflow()) {
        updateTraySync();
        return;
    }

    if (!invokeTrayOverflow(taskbar, false)) {
        trayNeedsReveal = true;
        shellTaskbar.revealPrimary();
    }
    if (!SetTimer(hwnd, 10, 20, nullptr)) finishTrayMenu();
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
    if (panel) {
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
                trayMenuItems.push_back(std::move(openItem));
            }
        }
    }
    trayMenuLoading = false;
    stopTraySync();
    if (execute && executed) {
        trayMenuIconIndex = -1;
        trayMenuPath.clear();
        trayMenuItems.clear();
        trayLastPopupHwnd = nullptr;
        close();
    }
    if ((capture || execute) && panel && page == SystemTray && !sheetClosing) {
        sheet.resetMotion();
        menuHoverVisible = false;
        menuHoverY.initialized = false;
        if (capture) triggerSheetContentAnim(16.f, 0.f);
        positionSheet();
        SetForegroundWindow(panel);
        if (execute && !executed) report(L"Menü komutu yeniden açılamadı.");
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
    KillTimer(hwnd, 10);
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
    if ((trayAction == TrayAction::RightClickMenu || trayAction == TrayAction::ExecuteMenu) && trayRightClickStep > 0) {
        if (trayRightClickStep == 4) {
            const bool done = !trayCapturedMenuHwnd || !IsWindowVisible(trayCapturedMenuHwnd) ||
                !isWin32MenuClass(trayCapturedMenuHwnd);
            finishTrayMenu(done);
            return;
        }
        if (trayRightClickStep == 1) {
            if (!overflow) { finishTrayMenu(); return; }
            int clickX = trayActionPoint.x;
            int clickY = trayActionPoint.y;
            RECT ovRect{};
            if (GetWindowRect(overflow, &ovRect)) {
                clickX = ovRect.left + trayLocalOffset.x;
                clickY = ovRect.top + trayLocalOffset.y;
            }
            const auto curButtons = collectOverflowButtons(overflow);
            RECT curBtnRect{};
            if (matchOverflowButton(curButtons, trayActionName, trayActionIndex, curBtnRect)) {
                clickX = (curBtnRect.left + curBtnRect.right) / 2;
                clickY = (curBtnRect.top + curBtnRect.bottom) / 2;
                if (GetWindowRect(overflow, &ovRect)) {
                    const int curLocalX = clickX - ovRect.left;
                    const int curLocalY = clickY - ovRect.top;
                    if (std::abs(curLocalX - trayLocalOffset.x) > 1 || std::abs(curLocalY - trayLocalOffset.y) > 1) {
                        trayLocalOffset = {curLocalX, curLocalY};
                        if (HRGN clip = CreateRectRgn(curLocalX - 1, curLocalY - 1, curLocalX + 2, curLocalY + 2)) {
                            if (SetWindowRgn(overflow, clip, FALSE)) gRestoreOverflowRgn = true;
                            else DeleteObject(clip);
                        }
                    }
                }
            }
            SetForegroundWindow(overflow);

            snapshotTrayPreWindows(*this);
            trayRightClickPending = true;
            trayCapturedMenuHwnd = nullptr;
            trayPopupSeenTries = 0;
            trayNativeClick = {clickX, clickY};

            POINT prevCursor{};
            GetCursorPos(&prevCursor);
            SetCursorPos(clickX, clickY);
            INPUT move{};
            move.type = INPUT_MOUSE;
            move.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &move, sizeof(INPUT));
            Sleep(8);
            INPUT down{};
            down.type = INPUT_MOUSE;
            down.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            SendInput(1, &down, sizeof(INPUT));
            Sleep(12);
            INPUT up{};
            up.type = INPUT_MOUSE;
            up.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(1, &up, sizeof(INPUT));
            if (trayAction == TrayAction::ExecuteMenu) SetCursorPos(prevCursor.x, prevCursor.y);
            else SetCursorPos(trayActionPoint.x, trayActionPoint.y);

            trayRightClickStep = 2;
            trayCaptureTries = 0;
            if (SetTimer(hwnd, 10, 16, nullptr)) return;
        }

        const bool capture = trayAction == TrayAction::RightClickMenu;
        if (!IsWindowVisible(trayCapturedMenuHwnd)) {
            trayCapturedMenuHwnd = nullptr;
            trayPopupSeenTries = 0;
        }
        if (!trayCapturedMenuHwnd || !isWin32MenuClass(trayCapturedMenuHwnd)) {
            if (HWND found = findTriggeredTrayPopup(*this)) {
                if (!trayCapturedMenuHwnd || (isWin32MenuClass(found) && !isWin32MenuClass(trayCapturedMenuHwnd))) {
                    trayCapturedMenuHwnd = found;
                    trayPopupSeenTries = 0;
                    cloakMenuWindow(found);
                }
            }
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
                    if (HWND alt = findTriggeredTrayPopup(*this)) {
                        uncloakMenuWindow(emptyCandidate);
                        trayCapturedMenuHwnd = alt;
                        trayPopupSeenTries = 0;
                        cloakMenuWindow(alt);
                    } else {
                        trayIgnoredPopups.pop_back();
                    }
                }
                const int maxSeen = isWin32MenuClass(trayCapturedMenuHwnd) ? 12 : 28;
                if (trayPopupSeenTries >= maxSeen) {
                    finishTrayMenu();
                    return;
                }
            } else {
                if (invokePopupMenuItem(trayCapturedMenuHwnd, trayPendingExec, trayMenuItems)) {
                    trayRightClickStep = 4;
                    if (SetTimer(hwnd, 10, 40, nullptr)) return;
                } else if (trayPopupSeenTries >= (isWin32MenuClass(trayCapturedMenuHwnd) ? 12 : 24)) {
                    finishTrayMenu(false);
                    return;
                }
            }
        }

        if (++trayCaptureTries < 50) {
            if (!trayCapturedMenuHwnd && trayMenuOwnerPid && trayCaptureTries == 10) {
                const auto& item = trayIcons[trayActionIndex];
                PostMessageW(item.targetHwnd, item.callbackMsg, MAKEWPARAM(trayNativeClick.x, trayNativeClick.y),
                    MAKELPARAM(WM_CONTEXTMENU, item.uid));
            }
            if (SetTimer(hwnd, 10, 16, nullptr)) return;
        }
        finishTrayMenu();
        return;
    }

    if (!overflow) {
        ++trayCaptureTries;
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (taskbar) {
            if (!trayNeedsReveal && trayCaptureTries >= 2) {
                trayNeedsReveal = true;
                shellTaskbar.revealPrimary();
                if (SetTimer(hwnd, 10, 25, nullptr)) return;
            }
            if (trayCaptureTries == 3 || trayCaptureTries == 6) invokeTrayOverflow(taskbar, true);
            else if (trayCaptureTries == 9) invokeViaWinB(taskbar);
        }
        if (trayCaptureTries < 16 && SetTimer(hwnd, 10, 20, nullptr)) return;
        if (trayAction == TrayAction::Capture) {
            trayLoading = false;
            if (panel && page == SystemTray) paintSheet();
        }
        finishTrayMenu();
        return;
    }

    trayOverflow = overflow;
    moveOverflowOffscreen(overflow);
    if (trayNeedsReveal) {
        if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb))
            ShowWindowAsync(tb, SW_HIDE);
    }

    if (trayAction == TrayAction::Capture) {
        RECT curOvRect{};
        if (GetWindowRect(overflow, &curOvRect)) {
            if (!trayOverflowSettled) {
                trayOverflowSettled = true;
                trayLastOvRect = curOvRect;
                if (SetTimer(hwnd, 10, 65, nullptr)) return;
            }
            if (!EqualRect(&curOvRect, &trayLastOvRect) && trayCaptureTries < 6) {
                trayLastOvRect = curOvRect;
                ++trayCaptureTries;
                if (SetTimer(hwnd, 10, 25, nullptr)) return;
            }
        }
    }

    const auto buttons = collectOverflowButtons(overflow);
    if (buttons.empty()) {
        if (++trayCaptureTries < 16 && SetTimer(hwnd, 10, 20, nullptr)) return;
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        if (trayAction == TrayAction::Capture) {
            trayLoading = false;
            if (panel && page == SystemTray) paintSheet();
        }
        finishTrayMenu();
        return;
    }

    if (trayAction == TrayAction::Capture) {
        std::vector<std::shared_ptr<Gdiplus::Bitmap>> bitmaps;
        const bool gotPixels = captureOverflowIcons(overflow, buttons, bitmaps);
        if (!gotPixels && trayCaptureTries < 8) {
            ++trayCaptureTries;
            if (SetTimer(hwnd, 10, 25, nullptr)) return;
        }
        const auto regEntries = loadNotifyIconRegistry();
        std::vector<TrayItem> next;
        next.reserve(buttons.size());
        for (size_t i = 0; i < buttons.size(); ++i) {
            auto captured = (i < bitmaps.size()) ? bitmaps[i] : nullptr;
            auto icon = resolveCrispTrayIcon(buttons[i].name, appIcon, regEntries, captured);
            next.push_back({buttons[i].name, std::move(icon), nullptr, 0, 0});
        }
        trayIcons = std::move(next);
        trayLoading = false;
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        stopTraySync();
        if (panel && page == SystemTray) {
            positionSheet();
            SetForegroundWindow(panel);
        }
        return;
    }

    RECT btnRect{};
    const auto target = matchOverflowButton(buttons, trayActionName, trayActionIndex, btnRect);
    if (!target) {
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        finishTrayMenu();
        return;
    }

    if (trayAction == TrayAction::LeftClick) {
        tryInvoke(target.Get());
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        stopTraySync();
        return;
    }

    // RightClickMenu / ExecuteMenu: clip overflow to a 3x3 pixel aperture strictly under the cursor tip
    // BEFORE moving it on-screen so Windows 11's DirectComposition overflow window never flickers visibly.
    RECT ovRect{};
    if (!GetWindowRect(overflow, &ovRect)) {
        PostMessageW(overflow, WM_CLOSE, 0, 0);
        finishTrayMenu();
        return;
    }
    const int ovW = ovRect.right - ovRect.left;
    const int ovH = ovRect.bottom - ovRect.top;
    const int localX = (btnRect.left + btnRect.right) / 2 - ovRect.left;
    const int localY = (btnRect.top + btnRect.bottom) / 2 - ovRect.top;
    trayLocalOffset = {localX, localY};
    trayRightClickPending = false;
    trayRightClickStep = 1;
    trayCapturedMenuHwnd = nullptr;
    trayPopupSeenTries = 0;

    int targetX = trayActionPoint.x - localX;
    int targetY = trayActionPoint.y - localY;
    if (const HMONITOR mon = MonitorFromPoint(trayActionPoint, MONITOR_DEFAULTTONEAREST)) {
        MONITORINFO mi{sizeof(mi)};
        if (GetMonitorInfoW(mon, &mi)) {
            targetX = std::clamp<int>(targetX, mi.rcMonitor.left, std::max<int>(mi.rcMonitor.left, mi.rcMonitor.right - ovW));
            targetY = std::clamp<int>(targetY, mi.rcMonitor.top, std::max<int>(mi.rcMonitor.top, mi.rcMonitor.bottom - ovH));
        }
    }

    gCloakedOverflow = overflow;
    if (HRGN clip = CreateRectRgn(localX - 1, localY - 1, localX + 2, localY + 2)) {
        if (SetWindowRgn(overflow, clip, FALSE)) gRestoreOverflowRgn = true;
        else DeleteObject(clip);
    }
    const LONG_PTR ex = GetWindowLongPtrW(overflow, GWL_EXSTYLE);
    constexpr LONG_PTR kNoRedir = 0x00200000L;
    if ((ex & kNoRedir) == 0) {
        gOrigExStyle = ex;
        gRestoreLayered = true;
        SetWindowLongPtrW(overflow, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(overflow, 0, 1, LWA_ALPHA);
    }

    if (panel) {
        const LONG_PTR pex = GetWindowLongPtrW(panel, GWL_EXSTYLE);
        SetWindowLongPtrW(panel, GWL_EXSTYLE, pex | WS_EX_TRANSPARENT);
    }
    SetWindowPos(overflow, HWND_TOPMOST, targetX, targetY, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(overflow);

    if (!SetTimer(hwnd, 10, 20, nullptr)) {
        finishTrayMenu();
    }
}

bool Dock::invokeSystemTray() {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar) return false;
    return invokeTrayOverflow(taskbar, true);
}
