#include "shell_taskbar.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cstdint>

namespace {
constexpr wchar_t Property[] = L"Vetra.HiddenTaskbar.v1";
constexpr wchar_t OrigStateProperty[] = L"Vetra.OrigTaskbarState.v1";
constexpr wchar_t SecondaryLayerProperty[] = L"Vetra.LayeredTaskbar.v1";
constexpr wchar_t LegacyProperty[] = L"PrismDock.HiddenTaskbar.v1";
constexpr wchar_t RestoreMode[] = L"--restore-taskbar";

UINT taskbarState() {
    APPBARDATA data{sizeof(data)};
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &data));
}

void setTaskbarState(UINT state, bool force = false) {
    APPBARDATA data{sizeof(data)};
    data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!data.hWnd || (!force && state == taskbarState())) return;
    data.lParam = state;
    SHAppBarMessage(ABM_SETSTATE, &data);
}

bool isDesktopOrShellClass(const wchar_t* cls) {
    return wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0;
}

void refitMaximizedWindows(HMONITOR monitor, const RECT& newWork) {
    if (!monitor || IsRectEmpty(&newWork)) return;
    struct Context {
        HMONITOR monitor;
        RECT work;
        DWORD selfPid;
    } ctx{monitor, newWork, GetCurrentProcessId()};

    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        const auto& c = *reinterpret_cast<const Context*>(lp);
        if (!IsWindowVisible(w) || IsIconic(w) || !IsZoomed(w)) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == c.selfPid) return TRUE;
        if (MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST) != c.monitor) return TRUE;

        wchar_t cls[64]{};
        if (GetClassNameW(w, cls, ARRAYSIZE(cls)) && isDesktopOrShellClass(cls)) return TRUE;

        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;

        const auto style = GetWindowLongPtrW(w, GWL_STYLE);
        if ((style & WS_CAPTION) != WS_CAPTION) return TRUE;

        RECT wr{}, fr{};
        if (!GetWindowRect(w, &wr)) return TRUE;
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &fr, sizeof(fr)))) fr = wr;

        const int leftInset = std::clamp<int>(fr.left - wr.left, 0, 24);
        const int topInset = std::clamp<int>(fr.top - wr.top, 0, 24);
        const int rightInset = std::clamp<int>(wr.right - fr.right, 0, 24);
        const int bottomInset = std::clamp<int>(wr.bottom - fr.bottom, 0, 24);

        const int tx = c.work.left - leftInset;
        const int ty = c.work.top - topInset;
        const int tw = (c.work.right + rightInset) - tx;
        const int th = (c.work.bottom + bottomInset) - ty;
        if (tw <= 100 || th <= 100) return TRUE;

        if (std::abs(wr.left - tx) > 1 || std::abs(wr.top - ty) > 1 ||
            std::abs((wr.right - wr.left) - tw) > 1 || std::abs((wr.bottom - wr.top) - th) > 1) {
            // Notify only the maximized app window (never broadcast to Progman/Desktop).
            SendNotifyMessageW(w, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
            SetWindowPos(w, nullptr, tx, ty, tw, th,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
}

bool shellBar(HWND window) {
    wchar_t name[40]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    return wcscmp(name, L"Shell_TrayWnd") == 0 || wcscmp(name, L"Shell_SecondaryTrayWnd") == 0;
}

bool ownerExited(HANDLE marker) {
    const DWORD pid = static_cast<DWORD>(reinterpret_cast<uintptr_t>(marker));
    const HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!owner) return GetLastError() == ERROR_INVALID_PARAMETER;
    const bool exited = WaitForSingleObject(owner, 0) == WAIT_OBJECT_0;
    CloseHandle(owner);
    return exited;
}

void hideSecondarySurface(HWND window, HANDLE owner) {
    wchar_t name[40]{};
    if (!GetClassNameW(window, name, ARRAYSIZE(name)) || wcscmp(name, L"Shell_SecondaryTrayWnd") != 0) return;
    const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
    const bool ownLayer = GetPropW(window, SecondaryLayerProperty) != nullptr;
    if ((style & WS_EX_LAYERED) && !ownLayer) return;

    // Explorer can show the bar again while applying a new work area. Keep its
    // pixels transparent before any AppBar requests are sent.
    if (!SetPropW(window, SecondaryLayerProperty, owner)) return;
    if (!(style & WS_EX_LAYERED)) SetWindowLongPtrW(window, GWL_EXSTYLE, style | WS_EX_LAYERED);
    BYTE alpha{};
    DWORD flags{};
    if (GetLayeredWindowAttributes(window, nullptr, &alpha, &flags) && alpha == 0 && (flags & LWA_ALPHA)) return;
    if (!SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA)) {
        SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        RemovePropW(window, SecondaryLayerProperty);
    }
}

BOOL CALLBACK hideBar(HWND window, LPARAM owner) {
    if (!shellBar(window)) return TRUE;
    auto marker = reinterpret_cast<HANDLE>(owner);
    if (GetPropW(window, LegacyProperty)) RemovePropW(window, LegacyProperty);
    const HANDLE current = GetPropW(window, Property);
    const bool visible = IsWindowVisible(window) != FALSE;
    const bool unowned = !current || (current != marker && ownerExited(current));
    if (current != marker && (!unowned || !SetPropW(window, Property, marker))) return TRUE;
    hideSecondarySurface(window, marker);
    if (visible) ShowWindowAsync(window, SW_HIDE);
    return TRUE;
}

BOOL CALLBACK restoreBar(HWND window, LPARAM owner) {
    if (!shellBar(window)) return TRUE;
    const HANDLE current = GetPropW(window, Property);
    const HANDLE legacy = GetPropW(window, LegacyProperty);
    if (!owner || current == reinterpret_cast<HANDLE>(owner) || !current || ownerExited(current) || legacy) {
        if (current) RemovePropW(window, Property);
        if (legacy) RemovePropW(window, LegacyProperty);
        if (RemovePropW(window, SecondaryLayerProperty)) {
            const auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            SetWindowLongPtrW(window, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
        }
        ShowWindow(window, SW_SHOW);
        ShowWindowAsync(window, SW_SHOW);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    return TRUE;
}
}

ShellTaskbar::~ShellTaskbar() { restore(); }

bool ShellTaskbar::start(bool deferAutohide) {
    if (guarded) {
        if (!deferAutohide) applyAutohide();
        else EnumWindows(hideBar, GetCurrentProcessId());
        return true;
    }
    const HWND primaryBar = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HANDLE savedProp = primaryBar ? GetPropW(primaryBar, OrigStateProperty) : nullptr;
    if (savedProp) {
        originalState = static_cast<UINT>(reinterpret_cast<uintptr_t>(savedProp) - 1);
    } else {
        const bool wasHiddenByDock = primaryBar && (!IsWindowVisible(primaryBar) ||
            GetPropW(primaryBar, Property) != nullptr || GetPropW(primaryBar, LegacyProperty) != nullptr);
        originalState = taskbarState();
        if (wasHiddenByDock) originalState &= ~ABS_AUTOHIDE;
        if (primaryBar) SetPropW(primaryBar, OrigStateProperty,
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(originalState) + 1));
    }
    wchar_t image[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, image, ARRAYSIZE(image))) return false;
    HANDLE parent{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
        SYNCHRONIZE, TRUE, 0)) return false;

    SECURITY_ATTRIBUTES inheritSa{sizeof(inheritSa), nullptr, TRUE};
    if (cancelWatchdog) { CloseHandle(cancelWatchdog); cancelWatchdog = nullptr; }
    cancelWatchdog = CreateEventW(&inheritSa, TRUE, FALSE, nullptr);
    HANDLE inheritList[2]{parent, cancelWatchdog};
    const DWORD inheritCount = cancelWatchdog ? 2u : 1u;

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    auto* attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (attributes && InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
        if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList,
            inheritCount * sizeof(HANDLE), nullptr, nullptr)) {
            wchar_t command[MAX_PATH + 140]{};
            swprintf_s(command, L"\"%s\" %s %lu %llu %llu %u", image, RestoreMode, GetCurrentProcessId(),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(parent)),
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(cancelWatchdog)),
                originalState);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
            startup.StartupInfo.wShowWindow = SW_HIDE;
            startup.lpAttributeList = attributes;
            PROCESS_INFORMATION process{};
            guarded = CreateProcessW(image, command, nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process);
            if (guarded) {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }
        DeleteProcThreadAttributeList(attributes);
    }
    if (attributes) HeapFree(GetProcessHeap(), 0, attributes);
    CloseHandle(parent);
    if (!guarded && cancelWatchdog) { CloseHandle(cancelWatchdog); cancelWatchdog = nullptr; }
    if (guarded) {
        autohideApplied = false;
        EnumWindows(hideBar, GetCurrentProcessId());
        if (!deferAutohide) applyAutohide();
    }
    return guarded;
}

void ShellTaskbar::applyAutohide() {
    if (!guarded) return;
    autohideApplied = true;
    // SW_HIDE by itself leaves Explorer's maximized-window inset intact.
    setTaskbarState(originalState | ABS_AUTOHIDE);
    EnumWindows(hideBar, GetCurrentProcessId());
}

void ShellTaskbar::hide(bool force) const {
    if (!guarded) return;
    if (!force) {
        bool anyVisible = false;
        if (const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr); primary && IsWindowVisible(primary))
            anyVisible = true;
        if (!anyVisible) {
            HWND secondary = nullptr;
            while ((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
                if (IsWindowVisible(secondary)) { anyVisible = true; break; }
            }
        }
        if (!anyVisible) return;
    }
    if (autohideApplied) setTaskbarState(originalState | ABS_AUTOHIDE);
    EnumWindows(hideBar, GetCurrentProcessId());
}

void ShellTaskbar::revealPrimary() const {
    const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (guarded && taskbar && GetPropW(taskbar, Property) ==
        reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId()))) {
        ShowWindowAsync(taskbar, SW_SHOWNOACTIVATE);
    }
}

void ShellTaskbar::restore() {
    if (cancelWatchdog) {
        SetEvent(cancelWatchdog);
        CloseHandle(cancelWatchdog);
        cancelWatchdog = nullptr;
    }
    if (!guarded) return;
    guarded = false;
    autohideApplied = false;
    const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    const HANDLE marker = primary ? GetPropW(primary, Property) : nullptr;
    const HANDLE selfMarker = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId()));
    const bool stillOwner = !marker || marker == selfMarker || ownerExited(marker);
    if (stillOwner) {
        if (primary) RemovePropW(primary, OrigStateProperty);
        EnumWindows(restoreBar, 0);
        setTaskbarState(originalState, true);
        EnumWindows(restoreBar, 0);
        SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    }
}

void ShellTaskbar::restoreIfOrphaned() {
    if (guarded) return;
    const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr);
    const bool orphaned = primary && (!IsWindowVisible(primary) ||
        GetPropW(primary, Property) != nullptr || GetPropW(primary, LegacyProperty) != nullptr ||
        GetPropW(primary, OrigStateProperty) != nullptr);
    if (!orphaned) return;
    UINT targetState = taskbarState() & ~ABS_AUTOHIDE;
    if (const HANDLE savedProp = GetPropW(primary, OrigStateProperty)) {
        targetState = static_cast<UINT>(reinterpret_cast<uintptr_t>(savedProp) - 1);
        RemovePropW(primary, OrigStateProperty);
    }
    EnumWindows(restoreBar, 0);
    setTaskbarState(targetState, true);
    EnumWindows(restoreBar, 0);
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
}

bool ShellTaskbar::runRestoreMode(const wchar_t* command) {
    DWORD owner = 0;
    unsigned long long handle = 0;
    unsigned long long cancelRaw = 0;
    unsigned state = 0;
    const int parsed4 = swscanf_s(command, L"--restore-taskbar %lu %llu %llu %u", &owner, &handle, &cancelRaw, &state);
    if (parsed4 != 4) {
        cancelRaw = 0;
        if (swscanf_s(command, L"--restore-taskbar %lu %llu %u", &owner, &handle, &state) != 3) return false;
    }
    HANDLE parent = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle));
    HANDLE cancel = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(cancelRaw));
    HANDLE waitHandles[2]{parent, cancel};
    const DWORD waitCount = cancel ? 2u : 1u;
    const DWORD waitRes = WaitForMultipleObjects(waitCount, waitHandles, FALSE, INFINITE);
    if (waitRes == WAIT_OBJECT_0) {
        const HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
        const HANDLE marker = taskbar ? GetPropW(taskbar, Property) : nullptr;
        const bool stillOwner = !marker || marker == reinterpret_cast<HANDLE>(static_cast<uintptr_t>(owner)) || ownerExited(marker);
        if (stillOwner) {
            if (taskbar) {
                if (const HANDLE savedProp = GetPropW(taskbar, OrigStateProperty)) {
                    state = static_cast<UINT>(reinterpret_cast<uintptr_t>(savedProp) - 1);
                    RemovePropW(taskbar, OrigStateProperty);
                }
            }
            EnumWindows(restoreBar, owner);
            setTaskbarState(state, true);
            EnumWindows(restoreBar, owner);
            SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
        }
    }
    if (cancel) CloseHandle(cancel);
    CloseHandle(parent);
    return true;
}

void ShellTaskbar::expandWorkArea(HMONITOR monitor, RECT& work) const {
    if (!guarded) return;
    struct Area { HMONITOR monitor; RECT* work; } area{monitor, &work};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& area = *reinterpret_cast<Area*>(value);
        if (!shellBar(window) || GetPropW(window, Property) !=
            reinterpret_cast<HANDLE>(static_cast<uintptr_t>(GetCurrentProcessId())) ||
            MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) != area.monitor) return TRUE;
        RECT bar{};
        if (!GetWindowRect(window, &bar)) return TRUE;
        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(area.monitor, &info)) return TRUE;
        auto& r = *area.work;
        if (bar.right - bar.left >= bar.bottom - bar.top) {
            if (bar.top == r.bottom && bar.bottom == info.rcMonitor.bottom) r.bottom = bar.bottom;
            if (bar.bottom == r.top && bar.top == info.rcMonitor.top) r.top = bar.top;
        } else {
            if (bar.left == r.right && bar.right == info.rcMonitor.right) r.right = bar.right;
            if (bar.right == r.left && bar.left == info.rcMonitor.left) r.left = bar.left;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&area));
}

ScreenReservation::~ScreenReservation() { release(); }

void ScreenReservation::release() {
    if (!active) return;
    active = false;
    RECT restore = monitorBounds;
    const HMONITOR mon = monitorHandle;
    monitorHandle = nullptr;
    monitorBounds = appliedWork = stripRect = {};
    if (!IsRectEmpty(&restore)) {
        // Restore full monitor work area silently (fWinIni = 0) so Explorer never reflows desktop icons.
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &restore, 0);
        refitMaximizedWindows(mon, restore);
    }
}

bool ScreenReservation::place(HWND, const RECT& monitor, bool bottom, int thickness, RECT& strip) {
    if (IsRectEmpty(&monitor) || thickness <= 0 || thickness >= (monitor.bottom - monitor.top) / 2)
        return false;

    const HMONITOR mon = MonitorFromRect(&monitor, MONITOR_DEFAULTTONEAREST);
    if (!mon) return false;

    RECT newWork = monitor;
    RECT newStrip = monitor;
    if (bottom) {
        newWork.bottom = monitor.bottom - thickness;
        newStrip.top = monitor.bottom - thickness;
    } else {
        newWork.top = monitor.top + thickness;
        newStrip.bottom = monitor.top + thickness;
    }

    MONITORINFO info{sizeof(info)};
    const bool hasInfo = GetMonitorInfoW(mon, &info) != FALSE;
    const bool workMismatch = !hasInfo || !EqualRect(&info.rcWork, &newWork);
    if (!active || mon != monitorHandle || !EqualRect(&appliedWork, &newWork) || workMismatch) {
        // Use silent SPI_SETWORKAREA (fWinIni = 0, no SPIF_SENDCHANGE) instead of SHAppBarMessage.
        // Maximized windows read the updated rcWork natively, while Explorer's desktop IconLayouts
        // never receives a work-area shrink broadcast and desktop icons stay completely untouched.
        RECT apply = newWork;
        if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &apply, 0)) return false;
        monitorHandle = mon;
        monitorBounds = monitor;
        appliedWork = newWork;
        stripRect = newStrip;
        active = true;
        refitMaximizedWindows(mon, newWork);
    }

    strip = stripRect;
    return !IsRectEmpty(&strip);
}

void ScreenReservation::verify() const {
    if (!active || !monitorHandle || IsRectEmpty(&appliedWork)) return;
    MONITORINFO info{sizeof(info)};
    if (GetMonitorInfoW(monitorHandle, &info) && !EqualRect(&info.rcWork, &appliedWork)) {
        RECT apply = appliedWork;
        if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &apply, 0))
            refitMaximizedWindows(monitorHandle, appliedWork);
    }
}
