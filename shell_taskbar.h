#pragma once
#include <windows.h>

class ShellTaskbar {
    bool guarded = false;
    bool autohideApplied = false;
    UINT originalState = 0;
public:
    ~ShellTaskbar();
    bool start(bool deferAutohide = false);
    void applyAutohide();
    void hide(bool force = false) const;
    void revealPrimary() const;
    void restore();
    void expandWorkArea(HMONITOR monitor, RECT& work) const;
    static bool runRestoreMode(const wchar_t* command);
};

inline constexpr UINT DockReservationChanged = WM_APP + 15;

class ScreenReservation {
    HMONITOR monitorHandle{};
    RECT monitorBounds{}, appliedWork{}, stripRect{};
    bool active = false;
public:
    ScreenReservation() = default;
    ScreenReservation(const ScreenReservation&) = delete;
    ScreenReservation& operator=(const ScreenReservation&) = delete;
    ~ScreenReservation();
    void release();
    bool place(HWND dock, const RECT& monitor, bool bottom, int thickness, RECT& strip);
    void verify() const;
};
