#include "dock.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <cmath>
#include <thread>
#include <tuple>

static int px(float value) { return static_cast<int>(std::lround(value)); }
static constexpr UINT_PTR AnimationTick = 7;

double animNowMs() {
    static const double invFreqMs = [] {
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart > 0 ? 1000.0 / static_cast<double>(freq.QuadPart) : 0.0;
    }();
    if (invFreqMs > 0.0) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        return static_cast<double>(counter.QuadPart) * invFreqMs;
    }
    return static_cast<double>(GetTickCount64());
}

static LRESULT CALLBACK route(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    auto dock = reinterpret_cast<Dock*>(GetWindowLongPtrW(w, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        dock = static_cast<Dock*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dock));
    }
    return dock ? dock->message(w, msg, wp, lp) : DefWindowProcW(w, msg, wp, lp);
}
int View::at(int x, int y, bool secondary) const {
    for (int i = static_cast<int>(hits.size()) - 1; i >= 0; --i) {
        if (secondary && !hits[i].secondary) continue;
        auto b = hits[i].bounds;
        if (x >= b.left * scale && x < b.right * scale && y >= b.top * scale && y < b.bottom * scale) return i;
    }
    return -1;
}

float View::hoverAmount() const {
    const int index = static_cast<int>(hits.size()) - 1;
    if (index == key) return 1.f;
    if (index >= 0 && index < static_cast<int>(hoverVals.size())) return hoverVals[index];
    return index == hover ? 1.f : 0.f;
}

float View::pressAmount() const {
    const int index = static_cast<int>(hits.size()) - 1;
    if (index >= 0 && index < static_cast<int>(pressVals.size())) return pressVals[index];
    return index == pressed ? 1.f : 0.f;
}

void View::resetMotion() {
    hoverVals.clear();
    pressVals.clear();
    hover = key = pressed = -1;
}

void View::swap(View& other) noexcept {
    d.swap(other.d);
    const auto state = [](View& v) {
        return std::tie(v.hwnd, v.scale, v.w, v.h, v.hover, v.key, v.pressed, v.tracking,
            v.hoverVals, v.pressVals, v.hits, v.placementRevision, v.placement);
    };
    auto theirs = state(other);
    state(*this).swap(theirs);
}

void Dock::refreshDisplayTiming() {
    BOOL enabled = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0);
    cachedMotionEnabled = enabled != FALSE;

    animationMonitor = activeMonitor;
    double hz = 0.0;
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (animationMonitor && GetMonitorInfoW(animationMonitor, &monitor) &&
        EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        hz = mode.dmDisplayFrequency;
    if (hz < 30.0) {
        DWM_TIMING_INFO timing{};
        timing.cbSize = sizeof(timing);
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.rateRefresh.uiDenominator)
            hz = static_cast<double>(timing.rateRefresh.uiNumerator) / timing.rateRefresh.uiDenominator;
    }
    if (hz < 30.0) hz = 60.0;
    // Use an integer display divisor above 90 Hz so software-rasterized layered updates stay VSync-even with half the CPU.
    animationPeriodMs = 1000.0 * std::ceil(hz / 90.0) / hz;
    animIntervalMs = static_cast<UINT>(std::lround(animationPeriodMs));
    if (animTimerActive) {
        nextAnimTickMs = animNowMs() + animationPeriodMs;
        armAnimTimer();
    }
}

void Dock::startAnimTimer() {
    if (!hwnd || animTimerActive) return;
    lastAnimTickMs = animNowMs();
    nextAnimTickMs = lastAnimTickMs + animationPeriodMs;
    animTimerActive = true;
    armAnimTimer();
}

void Dock::armAnimTimer() {
    if (animationTimer) {
        const double nowMs = animNowMs();
        // Skip missed deadlines after a stall, then continue on the original timeline.
        if (nextAnimTickMs <= nowMs)
            nextAnimTickMs += (std::floor((nowMs - nextAnimTickMs) / animationPeriodMs) + 1.0) * animationPeriodMs;
        LARGE_INTEGER due{};
        due.QuadPart = -std::max(1LL, std::llround((nextAnimTickMs - nowMs) * 10000.0));
        if (SetWaitableTimer(animationTimer, &due, 0, nullptr, nullptr, FALSE)) return;
        CloseHandle(animationTimer);
        animationTimer = nullptr;
    }
    animTimerActive = SetTimer(hwnd, AnimationTick, animIntervalMs, nullptr) != 0;
}

void Dock::stopAnimTimer() {
    if (!animTimerActive) return;
    if (animationTimer) CancelWaitableTimer(animationTimer);
    else KillTimer(hwnd, AnimationTick);
    animTimerActive = false;
}

BOOL Dock::nextMessage(MSG& msg) {
    while (animationTimer) {
        const HANDLE timer = animTimerActive ? animationTimer : nullptr;
        const DWORD ready = MsgWaitForMultipleObjectsEx(timer ? 1 : 0, timer ? &timer : nullptr,
            INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ready == WAIT_FAILED) return -1;
        if (timer && ready == WAIT_OBJECT_0) {
            nextAnimTickMs += animationPeriodMs;
            animateFrame();
            if (animTimerActive) armAnimTimer();
        }
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) return msg.message != WM_QUIT;
    }
    return GetMessageW(&msg, nullptr, 0, 0);
}

void Dock::animateFrame() {
    const double nowMs = animNowMs();
    const float dt = static_cast<float>(std::clamp((nowMs - lastAnimTickMs) / 1000.0, 0.001, 0.25));
    lastAnimTickMs = nowMs;

    const auto barOrigin = advanceVisibility(dt);
    const bool hadPanel = panel != nullptr;
    const auto sheetOrigin = advanceSheet(nowMs);
    const bool dragFrame = dockAnimating;
    if (dragFrame) advanceDockDrag(nowMs);
    const bool volFrame = barVolumeAnimating;
    if (volFrame) advanceBarVolume(nowMs);

    const bool barHoverMoving = advanceViewMotion(bar, dt);
    const bool sheetHoverMoving = panel && !sheetClosing ? advanceViewMotion(sheet, dt) : false;

    const auto advance = [dt](Spring& spring, float positionEpsilon, float velocityEpsilon) {
        const float previous = spring.value;
        return spring.step(dt, positionEpsilon, velocityEpsilon) || previous != spring.value;
    };
    const bool waveMoving = advance(barWaveStrength, 0.004f, 0.01f) |
        (barCursorSmoothX.initialized && advance(barCursorSmoothX, 0.25f, 2.f));
    const bool editMoving = advance(dockEditSpring, 0.004f, 0.01f);

    const bool cpuMoving = advance(cpuAnim, 0.8f, 1.5f);
    const bool ramMoving = advance(ramAnim, 0.8f, 1.5f);
    const bool batMoving = advance(batteryAnim, 0.8f, 1.5f);
    const bool taskMoving = advance(taskAnim, 0.002f, 0.01f);
    const bool opMoving = advance(opacityAnim, 0.15f, 0.5f);
    const bool metersMoving = cpuMoving || ramMoving || batMoving || taskMoving;

    bool appIndMoving = false;
    const float indK = motionEnabled() ? (1.f - std::exp(-18.f * dt)) : 1.f;
    for (auto& entry : appActiveWidth) {
        if (entry.value == entry.target) continue;
        entry.value += (entry.target - entry.value) * indK;
        if (std::abs(entry.target - entry.value) <= 0.005f) entry.value = entry.target;
        appIndMoving = true;
    }

    const bool navMoving = panel && !sheetClosing &&
        (advance(studioNavY, 0.2f, 1.5f) | advance(catalogNavY, 0.2f, 1.5f) |
         advance(catalogScrollY, 0.2f, 1.5f) | advance(menuHoverY, 0.2f, 1.5f));

    const bool bounceMoving = appBounceAtMs > 0.0;
    if (bounceMoving && nowMs - appBounceAtMs >= 540.0) appBounceAtMs = 0.0;

    const bool analogMoving = analogSecAtMs > 0.0;
    if (analogMoving && nowMs - analogSecAtMs >= 180.0) analogSecAtMs = 0.0;

    const bool contentMoving = sheetContentMs > 0.0;
    if (contentMoving && nowMs - sheetContentMs >= Design::ContentAnimMs) sheetContentMs = 0.0;

    const bool usageMoving = usageAnimMs > 0.0;
    if (usageMoving && nowMs - usageAnimMs >= 260.0) usageAnimMs = 0.0;

    const ULONGLONG nowTick = GetTickCount64();
    const bool noticeFading = !notice.empty() && ((nowMs - noticeAtMs) < 200.0 ||
        (noticeUntil > nowTick && noticeUntil - nowTick <= 350));

    const bool flipMoving = flipClockAtMs > 0.0;
    if (flipMoving && nowMs - flipClockAtMs >= 380.0) flipClockAtMs = 0.0;

    const bool segMoving = segmentClockAtMs > 0.0;
    if (segMoving && nowMs - segmentClockAtMs >= 240.0) segmentClockAtMs = 0.0;

    const bool widgetMoving = widgetMotionMs > 0.0;
    if (widgetMoving && nowMs - widgetMotionMs >= 310.0) widgetMotionMs = 0.0;

    const bool switchMoving = switchAtMs > 0.0;
    if (switchMoving && nowMs - switchAtMs >= 180.0) switchAtMs = 0.0;
    const bool rimsMoving = timerRimMoving(nowMs);

    const bool sharedBarFrame = (hadPanel && !panel) || editMoving ||
        appIndMoving || bounceMoving || analogMoving || flipMoving || segMoving || widgetMoving ||
        rimsMoving || (!panel && noticeFading);
    const auto movingBarWidget = [&](const OrderItem& item) {
        if (item.kind != OrderKind::Widget) return false;
        switch (item.index) {
        case Volume: return volFrame;
        case System: return cpuMoving || ramMoving;
        case CpuMeter: return cpuMoving;
        case RamMeter: return ramMoving;
        case Power: return batMoving;
        case Tasks: return taskMoving;
        default: return false;
        }
    };
    const auto hasMovingWidgets = [&](const View& view) {
        return (volFrame || metersMoving) && std::any_of(view.placement.begin(), view.placement.end(),
            [&](const auto& cell) { return movingBarWidget(cell.item); });
    };
    const bool barFrame = sharedBarFrame || dragFrame || barHoverMoving || waveMoving || hasMovingWidgets(bar);
    const bool sheetFrame = panel && !sheetClosing && (sheetHoverMoving || switchMoving || navMoving ||
        contentMoving || usageMoving || opMoving || noticeFading ||
        (volFrame && (page == Quick || page == Volume)) ||
        ((cpuMoving || ramMoving || batMoving) && page == System) ||
        (batMoving && page == Power) || (taskMoving && page == Tasks) ||
        (widgetMoving && (marketWidget(page) || page == DayTimeline)) ||
        (rimsMoving && (page == Focus || page == Countdown || page == Stopwatch)));

    if (barFrame && (!hidden || barOrigin)) paintBarView(bar, folded, barOrigin ? &*barOrigin : nullptr);
    else if (barOrigin) bar.d.present(&*barOrigin);
    if (sharedBarFrame || volFrame || metersMoving) {
        for (auto& entry : otherDocks) {
            if (!entry->hidden && entry->view.w > 0 && (sharedBarFrame || hasMovingWidgets(entry->view)))
                paintBarView(entry->view, entry->folded);
        }
    }
    if (sheetFrame) paintSheet(sheetOrigin ? &*sheetOrigin : nullptr);
    else if (sheetOrigin) sheet.d.present(&*sheetOrigin);

    if (pendingPosition && !dockVisSpring.active(0.001f, 0.006f) && !dockDragging && !dockOrderDragging) {
        pendingPosition = false;
        position();
    }

    const bool anyActive = dockVisSpring.active(0.001f, 0.006f) || sheetMotionMs > 0.0 ||
        dockAnimating || barVolumeAnimating || barHoverMoving || sheetHoverMoving ||
        waveMoving || editMoving || metersMoving || opMoving || appIndMoving || navMoving ||
        bounceMoving || analogMoving || contentMoving || usageMoving || noticeFading || rimsMoving ||
        flipClockAtMs > 0.0 || segmentClockAtMs > 0.0 || widgetMotionMs > 0.0 || switchAtMs > 0.0;
    if (!anyActive) stopAnimTimer();
}

void Dock::updateVisibilityTimer() {
    if (!hwnd) return;
    const UINT period = (model.autoHides() || (panel && page == AppMenu)) ? 50 : 400;
    if (visibilityInterval != period && SetTimer(hwnd, 3, period, nullptr)) visibilityInterval = period;
}

bool Dock::advanceViewMotion(View& view, float dt) {
    const size_t n = view.hits.size();
    view.hoverVals.resize(n, 0.f);
    view.pressVals.resize(n, 0.f);
    if (n == 0) return false;
    const bool smooth = motionEnabled();
    const float kIn = smooth ? (1.f - std::exp(-38.f * dt)) : 1.f;
    const float kOut = smooth ? (1.f - std::exp(-28.f * dt)) : 1.f;
    const float kPress = smooth ? (1.f - std::exp(-42.f * dt)) : 1.f;
    bool moving = false;
    for (size_t i = 0; i < n; ++i) {
        const float hTarget = (static_cast<int>(i) == view.hover || static_cast<int>(i) == view.key) ? 1.f : 0.f;
        float& hv = view.hoverVals[i];
        if (hv != hTarget) {
            hv += (hTarget - hv) * (hTarget > hv ? kIn : kOut);
            if (std::abs(hTarget - hv) <= 0.008f) hv = hTarget;
            moving = true;
        }

        const float pTarget = (static_cast<int>(i) == view.pressed) ? 1.f : 0.f;
        float& pv = view.pressVals[i];
        if (pv != pTarget) {
            pv += (pTarget - pv) * kPress;
            if (std::abs(pTarget - pv) <= 0.005f) pv = pTarget;
            moving = true;
        }
    }
    return moving;
}

void Dock::changeHover(View& view, int hovered) {
    if (view.hover == hovered) return;
    if (view.hoverVals.size() < view.hits.size()) view.hoverVals.resize(view.hits.size(), 0.f);
    view.hover = hovered;
    if (!motionEnabled()) {
        for (size_t i = 0; i < view.hoverVals.size(); ++i)
            view.hoverVals[i] = static_cast<int>(i) == hovered ? 1.f : 0.f;
        return;
    }
    startAnimTimer();
}

void Dock::setPressed(View& view, int pressed) {
    if (view.pressed == pressed) return;
    if (view.pressVals.size() < view.hits.size()) view.pressVals.resize(view.hits.size(), 0.f);
    view.pressed = pressed;
    if (!motionEnabled()) {
        for (size_t i = 0; i < view.pressVals.size(); ++i)
            view.pressVals[i] = static_cast<int>(i) == pressed ? 1.f : 0.f;
        return;
    }
    startAnimTimer();
}

void Dock::beginSwitch(int index, bool enabled) {
    if (motionEnabled()) {
        switchHit = index;
        switchWasOn = enabled;
        switchAtMs = animNowMs();
        startAnimTimer();
    }
}

void Dock::triggerSheetContentAnim(float dx, float dy) {
    if (!motionEnabled()) { sheetContentMs = 0.0; return; }
    sheetContentDx = dx;
    sheetContentDy = dy;
    sheetContentMs = animNowMs();
    startAnimTimer();
}

float Dock::sheetContentProgress() const {
    if (sheetContentMs <= 0.0) return 1.f;
    return easeOutQuint(static_cast<float>((animNowMs() - sheetContentMs) / Design::ContentAnimMs));
}

void Dock::triggerAppBounce(const Text& key) {
    if (!motionEnabled() || key.empty()) return;
    appBounceKey = key;
    appBounceAtMs = animNowMs();
    startAnimTimer();
}

float Dock::appBounceOffset(const Text& key) const {
    if (appBounceAtMs <= 0.0 || key.empty() ||
        CompareStringOrdinal(key.c_str(), -1, appBounceKey.c_str(), -1, TRUE) != CSTR_EQUAL)
        return 0.f;
    const float t = static_cast<float>((animNowMs() - appBounceAtMs) / 540.0);
    if (t >= 1.f || t < 0.f) return 0.f;
    return std::abs(std::sin(t * 6.2831853f)) * std::exp(-3.4f * t) * 7.5f;
}

float Dock::appWaveBoost(float centerX) const {
    if (!motionEnabled() || dockOrderDragging || dockEditMode || !barCursorSmoothX.initialized) return 0.f;
    const float strength = std::clamp(barWaveStrength.value, 0.f, 1.f);
    if (strength <= 0.005f) return 0.f;
    const float radius = (glassTheme(model.theme) ? Design::GlassAppPitch : Design::AppPitch) * 1.55f;
    const float dist = std::abs(centerX - barCursorSmoothX.value);
    if (dist >= radius) return 0.f;
    const float c = std::cos((dist / radius) * 1.5707963f);
    return c * c * strength;
}

float Dock::appActiveRatio(const LiveDockItem& key, bool active) {
    const auto same = [](const LiveDockItem& a, const LiveDockItem& b) {
        if (a.kind != b.kind) return false;
        if (a.kind == OrderKind::Widget) return a.widget == b.widget;
        if (a.kind == OrderKind::Shortcut)
            return CompareStringOrdinal(a.appKey.c_str(), -1, b.appKey.c_str(), -1, TRUE) == CSTR_EQUAL;
        return a.chromeGroup == b.chromeGroup && (a.chromeGroup || a.window == b.window);
    };
    const float target = active ? 1.f : 0.f;
    for (auto& entry : appActiveWidth) {
        if (same(entry.key, key)) {
            entry.target = target;
            if (!motionEnabled()) entry.value = target;
            else if (entry.value != target) startAnimTimer();
            return entry.value;
        }
    }
    if (appActiveWidth.size() >= 32) appActiveWidth.erase(appActiveWidth.begin());
    appActiveWidth.push_back({key, target, target});
    return target;
}

void Dock::syncMeterTargets() {
    const bool smooth = motionEnabled();
    const auto updateMeter = [smooth](Spring& s, float val) {
        if (!smooth || !s.initialized || std::abs(val - s.value) <= 2.f) s.snap(val);
        else s.setTarget(val, true);
    };
    if (cpu >= 0) updateMeter(cpuAnim, static_cast<float>(cpu));
    updateMeter(ramAnim, static_cast<float>(ram));
    if (battery >= 0) updateMeter(batteryAnim, static_cast<float>(battery));
    int totalTasks = 0, doneTasks = 0;
    for (const auto& t : model.tasks) if (t.profile == model.profile) { ++totalTasks; if (t.complete) ++doneTasks; }
    taskAnim.setTarget(totalTasks > 0 ? static_cast<float>(doneTasks) / totalTasks : 0.f, smooth);
    opacityAnim.setTarget(static_cast<float>(model.opacity), smooth);
    if (smooth && (cpuAnim.active(0.8f, 1.5f) || ramAnim.active(0.8f, 1.5f) ||
        batteryAnim.active(0.8f, 1.5f) || taskAnim.active(0.002f, 0.01f) || opacityAnim.active(0.15f, 0.5f)))
        startAnimTimer();
}

void Dock::setOpacityFromX(float localX, bool save) {
    if (width(opacitySliderTrack) <= 0.f) return;
    const float ratio = std::clamp((localX - opacitySliderTrack.left) / width(opacitySliderTrack), 0.f, 1.f);
    const int next = std::clamp(static_cast<int>(std::lround(ratio * 100.f)), 0, 100);
    const bool dragging = opacitySliderDrag && !save;
    if (next == model.opacity && !save && !dragging) return;
    model.opacity = next;
    if (dragging) {
        opacityAnim.snap(ratio * 100.f);
        const double nowMs = animNowMs();
        if (nowMs - opacityPaintMs >= 8.0 || next == 0 || next == 100) {
            opacityPaintMs = nowMs;
            paintBar();
            if (panel) paintSheet();
        }
        return;
    }
    opacityAnim.setTarget(static_cast<float>(next), motionEnabled());
    opacityPaintMs = animNowMs();
    if (save) changed(false);
    else {
        paintBar();
        if (panel) paintSheet();
    }
    if (motionEnabled() && opacityAnim.active(0.15f, 0.5f)) startAnimTimer();
}

void Dock::animateWidgets() {
    if (motionEnabled()) {
        widgetMotionMs = animNowMs();
        startAnimTimer();
    } else widgetMotionMs = 0.0;
}

float Dock::widgetProgress() const {
    if (widgetMotionMs <= 0.0) return 1.f;
    return easeOutQuint(static_cast<float>((animNowMs() - widgetMotionMs) / 310.0));
}

Palette::Palette(AppearanceTheme style, int tone, int accentChoice) :
    theme(style), light(theme == WhiteTheme || theme == FrostGlass), glass(glassTheme(theme)),
    accent(light ? 0x353535 : 0xE2E2E2), action(light ? 0x242424 : 0xEAEAEA),
    activeInk(light ? 0xFFFFFF : 0x1A1A1A), opacity(tone) {
    if (light) {
        ink = 0x191919; muted = 0x595959; base = 0xFAFAFA; high = 0xFFFFFF; line = 0x868686; rail = 0xEBEBEB;
    } else {
        ink = 0xF7F7F7; muted = glass ? 0xC8CBCD : 0xB4B4B4; base = glass ? 0x090909 : 0x0E0E0E;
        high = glass ? 0x202020 : 0x232323; line = 0xA1A1A1; rail = glass ? 0x101010 : 0x171717;
    }
    if (theme == FrostGlass) {
        ink = 0x172735; muted = 0x435869; base = 0xCFDEEB; high = 0xF0F7FF; line = 0x59788A; rail = 0xCCDCE9;
        accent = action = 0x295670;
    } else if (theme == BareTheme) {
        ink = 0xF7F7F7; muted = 0xB4B4B4; base = 0x0A0A0B; high = 0x131315; line = 0x9A9A9E; rail = 0x111113;
        accent = 0xE2E2E2; action = 0xEAEAEA;
    }
    if (accentChoice > 0 && accentChoice < static_cast<int>(ARRAYSIZE(Accents)))
        accent = action = light ? Accents[accentChoice].light : Accents[accentChoice].dark;
}

void Palette::surface(Draw& d, Box b, float radius, bool panel) const {
    if (theme == BareTheme && !panel) return;
    if (glass) {
        const float tone = opacity / 100.f;
        if (theme == LiquidGlass) {
            const float baseAlpha = panel ? (.46f + tone * .30f) : (.20f + tone * .36f);
            d.fill(b, radius, tint(0x0A0B0D, baseAlpha));
            d.glassRim(b, radius, panel ? .46f : .52f);
            return;
        }
        const float strength = (panel ? .88f : .76f) + tone * .11f;
        d.gradient(b, radius, tint(high, strength), tint(base, strength));
        d.glassRim(b, radius, .42f);
        return;
    }
    d.fill(b, radius, tint(base));
    const float edge = .6f / d.scale;
    const Box rim{b.left + edge, b.top + edge, b.right - edge, b.bottom - edge};
    d.gradientOutline(rim, radius - edge, tint(line, .24f), tint(line, .15f), 1.f / d.scale);
}

void Palette::card(Draw& d, Box b, float radius) const {
    if (glass) {
        if (theme == LiquidGlass) {
            const float tone = opacity / 100.f;
            d.fill(b, radius, tint(0x08090B, .24f + tone * .18f));
            d.glassRim(b, radius, .28f);
            return;
        }
        d.gradient(b, radius, tint(high, .34f), tint(base, .22f));
        d.glassRim(b, radius, .28f);
        return;
    }
    d.fill(b, radius, tint(high));
    constexpr float edge = .5f;
    d.outline({b.left + edge, b.top + edge, b.right - edge, b.bottom - edge}, radius - edge,
        tint(line, light ? .23f : .20f), 1.f);
}
static BOOL CALLBACK monitorList(HMONITOR monitor, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<std::vector<HMONITOR>*>(lp)->push_back(monitor);
    return TRUE;
}
void Dock::refreshMonitors() {
    std::vector<HMONITOR> detected;
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    EnumDisplayMonitors(nullptr, nullptr, monitorList, reinterpret_cast<LPARAM>(&detected));
    auto found = std::find(detected.begin(), detected.end(), primary);
    if (found != detected.end()) std::iter_swap(detected.begin(), found);
    if (detected.empty() && primary) detected.push_back(primary);
    if (detected.empty()) return;
    const bool same = detected.size() == monitors.size() && detected.front() == monitors.front() &&
        std::is_permutation(detected.begin(), detected.end(), monitors.begin());
    if (same) return;

    reservations.clear();
    if (hwnd) {
        dockDragging = dockIconDragPending = dockOrderDragging = barVolumeDrag = false;
        dockOrderSource = {};
        if (GetCapture() == bar.hwnd) ReleaseCapture();
        finishDockDragAnim();
        selectDockWindow(hwnd, true);
        destroyMonitorDocks();
    }
    monitors = std::move(detected);
    activeMonitor = monitors.front();
    pointerLeftAt = lastFullscreenScan = 0;
    lastFullscreenFg = nullptr;
    fullscreenActive = false;
    fullscreenFrames.clear();
}

void Dock::syncMonitorDocks() {
    for (const auto monitor : monitors) {
        if (monitor == activeMonitor || std::any_of(otherDocks.begin(), otherDocks.end(),
            [monitor](const auto& entry) { return entry->monitor == monitor; })) continue;

        MONITORINFO info{sizeof(info)};
        if (!GetMonitorInfoW(monitor, &info)) continue;
        auto entry = std::make_unique<MonitorDock>();
        entry->monitor = monitor;
        entry->hidden = model.autoHides();
        entry->view.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            MonitorDockClass, L"Vetra", WS_POPUP, info.rcMonitor.left, info.rcMonitor.top, 1, 1,
            nullptr, nullptr, instance, this);
        if (entry->view.hwnd) {
            registerAppDrop(entry->view.hwnd);
            otherDocks.push_back(std::move(entry));
        }
    }
}

void Dock::destroyMonitorDocks() {
    for (auto& entry : otherDocks) {
        if (entry->view.hwnd == hwnd) continue;
        RevokeDragDrop(entry->view.hwnd);
        DestroyWindow(entry->view.hwnd);
    }
    otherDocks.clear();
    if (bar.hwnd && bar.hwnd != hwnd) {
        RevokeDragDrop(bar.hwnd);
        DestroyWindow(bar.hwnd);
        bar.hwnd = hwnd;
    }
}

bool Dock::selectDockWindow(HWND window, bool force) {
    if (window == bar.hwnd) return true;
    if (!force && (modal || dockDragging || dockOrderSource || barVolumeDrag || GetCapture())) return false;
    const auto found = std::find_if(otherDocks.begin(), otherDocks.end(), [window](const auto& entry) {
        return entry->view.hwnd == window;
    });
    if (found == otherDocks.end()) return false;

    if (panel && !modal) closeSheet(false);
    if (bar.tracking) {
        TRACKMOUSEEVENT cancel{sizeof(cancel), TME_CANCEL | TME_LEAVE, bar.hwnd, 0};
        TrackMouseEvent(&cancel);
        bar.tracking = false;
    }
    bar.resetMotion();
    bar.d.clipped = false;
    bar.d.layerOpacity = 255;
    if (hidden) ShowWindow(bar.hwnd, SW_HIDE);
    else {
        barWaveStrength.snap(0.f);
        paintBarView(bar, folded, &dockOrigin);
    }

    auto& entry = **found;
    bar.swap(entry.view);
    std::swap(activeMonitor, entry.monitor);
    std::swap(work, entry.work);
    std::swap(monitorRect, entry.bounds);
    std::swap(dockOrigin, entry.origin);
    folded.swap(entry.folded);
    std::swap(hidden, entry.hidden);
    std::swap(pointerLeftAt, entry.pointerLeftAt);
    std::swap(fullscreenActive, entry.fullscreen);
    dockExposure = hidden ? 0.f : 1.f;
    dockOffset = 0.f;
    dockVisSpring.snap(dockExposure);
    barWaveStrength.snap(0.f);
    barCursorSmoothX.initialized = false;
    if (!force) {
        refreshDisplayTiming();
        paintBarView(bar, folded);
    }
    return true;
}

bool Dock::anyDockVisible() const {
    return !hidden || std::any_of(otherDocks.begin(), otherDocks.end(),
        [](const auto& entry) { return !entry->hidden; });
}

namespace {
Dock* gWindowHookDock = nullptr;
HWINEVENTHOOK gWindowHooks[3]{};

void CALLBACK windowWinEventProc(HWINEVENTHOOK, DWORD event, HWND window, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!gWindowHookDock || !window || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (event == EVENT_OBJECT_CREATE) {
        wchar_t windowClass[64]{};
        if (GetAncestor(window, GA_ROOT) == window &&
            GetClassNameW(window, windowClass, ARRAYSIZE(windowClass)) &&
            CompareStringOrdinal(windowClass, -1, L"Chrome_WidgetWin_1", -1, TRUE) == CSTR_EQUAL)
            gWindowHookDock->rememberChromeWindow(window);
        return;
    }
    if (event != EVENT_SYSTEM_FOREGROUND &&
        event != EVENT_SYSTEM_MINIMIZESTART && event != EVENT_SYSTEM_MINIMIZEEND &&
        event != EVENT_OBJECT_SHOW && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_DESTROY &&
        event != EVENT_OBJECT_CLOAKED && event != EVENT_OBJECT_UNCLOAKED)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return;
    if (event == EVENT_OBJECT_DESTROY) std::erase(gWindowHookDock->chromeWindowOrder, window);
    if (event != EVENT_OBJECT_DESTROY && event != EVENT_OBJECT_HIDE && event != EVENT_OBJECT_CLOAKED) {
        if (GetAncestor(window, GA_ROOT) != window) return;
        const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
        if (ex & WS_EX_TOOLWINDOW) return;
    }
    gWindowHookDock->scheduleWindowSync();
}
}

void Dock::startWindowHook() {
    if (gWindowHookDock == this) return;
    gWindowHookDock = this;
    gWindowHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr,
        windowWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    gWindowHooks[1] = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
        windowWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    gWindowHooks[2] = SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr,
        windowWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    windowHook = gWindowHooks[0];
}

void Dock::stopWindowHook() {
    for (auto& hook : gWindowHooks) {
        if (hook) {
            UnhookWinEvent(hook);
            hook = nullptr;
        }
    }
    windowHook = nullptr;
    if (gWindowHookDock == this) gWindowHookDock = nullptr;
}

void Dock::scheduleWindowSync() {
    if (hwnd && !windowSyncScheduled) {
        windowSyncScheduled = true;
        SetTimer(hwnd, WindowSyncTick, 35, nullptr);
    }
}

void Dock::completeDeferredStartup() {
    if (startupCompleted || !hwnd) return;
    startupCompleted = true;
    if (model.hideTaskbar) shellTaskbar.applyAutohide();
    if (model.visibility == DockVisibility::Reserved) {
        for (auto& [mon, res] : reservations) res.verify();
    }
    tray(true);
    sampleExtras(true);
    SetTimer(hwnd, WarmupTick, 900, nullptr);
}

bool Dock::initialize() {
    if (!Draw::initialize()) return false;
    curArrow = LoadCursorW(nullptr, IDC_ARROW);
    curHand = LoadCursorW(nullptr, IDC_HAND);
    curSizeNS = LoadCursorW(nullptr, IDC_SIZENS);
    curSizeWE = LoadCursorW(nullptr, IDC_SIZEWE);
    curSizeAll = LoadCursorW(nullptr, IDC_SIZEALL);
    refreshDisplayTiming();
    appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (!appIcon) appIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    if (!appIcon) appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    HICON smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    for (auto name : {DockClass, MonitorDockClass, PanelClass}) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance; wc.lpfnWndProc = route; wc.lpszClassName = name; wc.hIcon = appIcon;
        wc.hIconSm = smallIcon ? smallIcon : appIcon;
        wc.hCursor = curArrow;
        if (!RegisterClassExW(&wc)) return false;
    }
    model.load();
    opacityAnim.snap(static_cast<float>(model.opacity));
    boot = model.startup();
    focusTimer.reset(model.focusMinutes);
    countdownTimer.reset(model.countdownMinutes);
    if (model.usageDay == Date::from(now()).key()) {
        for (int seconds : model.usageSeconds) extra.usageTotal += seconds;
    }
    appImages.assign(model.shortcuts.size(), {});
    refreshOpenWindows();
    refreshMonitors();
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | (model.topmost ? WS_EX_TOPMOST : 0), DockClass, L"Vetra",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!hwnd) return false;
    animationTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_MODIFY_STATE | SYNCHRONIZE);
    bar.hwnd = hwnd;
    startWindowHook();
    registerAppDrop(hwnd);
    trayRestore = RegisterWindowMessageW(L"TaskbarCreated");
    sample();
    if (model.statusBar.enabled) {
        if (statusShown(StatusKeyboard)) sampleKeyboardLayout();
        if (statusShown(StatusNetwork)) sampleNetworkFast();
        if (statusShown(StatusNotifications)) queryDoNotDisturb();
    }
    if (cpu >= 0) cpuAnim.snap(static_cast<float>(cpu));
    ramAnim.snap(static_cast<float>(ram));
    if (battery >= 0) batteryAnim.snap(static_cast<float>(battery));
    if (model.hideTaskbar) shellTaskbar.start(true);
    else shellTaskbar.restoreIfOrphaned();
    position();
    refreshAppImages();
    requestWindowIcons();
    SetTimer(hwnd, 1, 1000, nullptr);
    PostMessageW(hwnd, StartupDeferred, 0, 0);
    return true;
}
void Dock::tray(bool add) {
    NOTIFYICONDATAW icon{sizeof(icon)};
    icon.hWnd = hwnd; icon.uID = 1; icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = TrayMessage; icon.hIcon = appIcon;
    wcscpy_s(icon.szTip, L"Vetra");
    Shell_NotifyIconW(add ? NIM_ADD : NIM_DELETE, &icon);
}
bool Dock::shown(int id) const {
    return std::any_of(model.layouts[model.profile].begin(), model.layouts[model.profile].end(),
        [=](Tile tile) { return tile.id == id && tile.enabled; });
}
bool Dock::isFoldedWidget(int id) const {
    const auto contains = [id](const auto& items) {
        return std::any_of(items.begin(), items.end(),
            [id](const OrderItem& item) { return item.kind == OrderKind::Widget && item.index == id; });
    };
    if (!hidden && !contains(folded)) return false;
    return std::none_of(otherDocks.begin(), otherDocks.end(), [&](const auto& entry) {
        return !entry->hidden && !contains(entry->folded);
    });
}
static bool sameKey(const Text& a, const Text& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
static bool slotMatchesShortcut(const Text& key, const Shortcut& s) {
    return sameKey(key, appKey(s)) || sameKey(key, s.target);
}
static bool isAppKind(OrderKind kind) { return kind == OrderKind::Shortcut || kind == OrderKind::Window; }

void Dock::ensureDockOrder(int p) {
    auto& slots = model.dockOrder[p];
    const auto& layout = model.layouts[p];
    if (slots.empty()) {
        for (const auto& tile : layout) {
            if (tile.id == Apps) {
                for (const auto& s : model.shortcuts) slots.push_back({true, -1, appKey(s)});
            } else {
                slots.push_back({false, tile.id, {}});
            }
        }
        return;
    }
    std::vector<bool> seenShortcut(model.shortcuts.size(), false);
    std::array<bool, WidgetCount> seenWidget{};
    std::vector<DockSlot> cleaned;
    cleaned.reserve(slots.size() + model.shortcuts.size());
    for (auto& slot : slots) {
        if (slot.isApp) {
            int found = -1;
            for (size_t i = 0; i < model.shortcuts.size(); ++i) {
                if (!seenShortcut[i] && slotMatchesShortcut(slot.appKey, model.shortcuts[i])) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            if (found >= 0) {
                seenShortcut[found] = true;
                cleaned.push_back({true, -1, appKey(model.shortcuts[found])});
            }
        } else if (slot.widget >= 0 && slot.widget < WidgetCount && slot.widget != Apps && !seenWidget[slot.widget]) {
            seenWidget[slot.widget] = true;
            cleaned.push_back(slot);
        }
    }
    for (size_t i = 0; i < model.shortcuts.size(); ++i) {
        if (seenShortcut[i]) continue;
        size_t insertPos = 0;
        for (size_t j = 0; j < cleaned.size(); ++j)
            if (cleaned[j].isApp) insertPos = j + 1;
        cleaned.insert(cleaned.begin() + insertPos, {true, -1, appKey(model.shortcuts[i])});
    }
    for (const auto& tile : layout) {
        if (tile.id != Apps && !seenWidget[tile.id]) {
            seenWidget[tile.id] = true;
            cleaned.push_back({false, tile.id, {}});
        }
    }
    slots = std::move(cleaned);
}

const std::vector<OrderItem>& Dock::dockItems() {
    if (!dockItemsDirty) return cachedDockItems;
    dockItemsDirty = false;
    const int p = model.profile;
    ensureDockOrder(p);
    const bool showApps = shown(Apps);
    const size_t pinnedCount = showApps ? pinnedOnBar() : 0;
    const auto running = showApps ? runningOnBar() : std::vector<size_t>{};

    const auto findShortcut = [&](const Text& key, const std::vector<bool>& used) -> int {
        for (size_t i = 0; i < pinnedCount; ++i)
            if (!used[i] && slotMatchesShortcut(key, model.shortcuts[i])) return static_cast<int>(i);
        return -1;
    };
    const auto findWindow = [&](const LiveDockItem& item) -> HWND {
        for (size_t idx : running) {
            const auto& win = extra.windows[idx];
            if (item.chromeGroup && isChromeApp(win.app)) return win.handle;
            if (!item.chromeGroup && !isChromeApp(win.app) && win.handle == item.window) return win.handle;
        }
        return nullptr;
    };

    std::vector<LiveDockItem> activePersisted;
    {
        std::vector<bool> used(pinnedCount, false);
        for (const auto& slot : model.dockOrder[p]) {
            if (!slot.isApp) {
                if (shown(slot.widget) && widgetWidth(slot.widget) > 0)
                    activePersisted.push_back({OrderKind::Widget, slot.widget, {}, nullptr, false});
            } else if (showApps) {
                const int idx = findShortcut(slot.appKey, used);
                if (idx >= 0) {
                    used[idx] = true;
                    activePersisted.push_back({OrderKind::Shortcut, -1, appKey(model.shortcuts[idx]), nullptr, false});
                }
            }
        }
    }
    std::vector<LiveDockItem> activeWindows;
    for (size_t idx : running) {
        const auto& win = extra.windows[idx];
        const bool chrome = isChromeApp(win.app);
        activeWindows.push_back({OrderKind::Window, -1, {}, chrome ? nullptr : win.handle, chrome});
    }

    const auto sameLive = [](const LiveDockItem& a, const LiveDockItem& b) {
        if (a.kind != b.kind) return false;
        if (a.kind == OrderKind::Widget) return a.widget == b.widget;
        if (a.kind == OrderKind::Shortcut) return sameKey(a.appKey, b.appKey);
        return a.chromeGroup == b.chromeGroup && (a.chromeGroup || a.window == b.window);
    };

    auto& live = liveOrder[p];
    if (live.empty()) {
        live = activePersisted;
        size_t insertPos = 0;
        bool hasShortcut = false;
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i].kind == OrderKind::Shortcut) { insertPos = i + 1; hasShortcut = true; }
        }
        if (!hasShortcut) {
            const auto& layout = model.layouts[p];
            auto appsIt = std::find_if(layout.begin(), layout.end(), [](Tile t) { return t.id == Apps; });
            insertPos = live.size();
            if (appsIt != layout.end()) {
                for (auto it = appsIt + 1; it != layout.end(); ++it) {
                    if (!it->enabled) continue;
                    auto found = std::find_if(live.begin(), live.end(),
                        [id = it->id](const LiveDockItem& item) { return item.kind == OrderKind::Widget && item.widget == id; });
                    if (found != live.end()) { insertPos = found - live.begin(); break; }
                }
            }
        }
        live.insert(live.begin() + insertPos, activeWindows.begin(), activeWindows.end());
    } else {
        std::vector<LiveDockItem> kept;
        kept.reserve(live.size());
        std::vector<bool> usedShortcut(pinnedCount, false);
        for (const auto& item : live) {
            if (item.kind == OrderKind::Widget) {
                if (std::any_of(activePersisted.begin(), activePersisted.end(),
                    [&](const LiveDockItem& a) { return sameLive(a, item); }))
                    kept.push_back(item);
            } else if (item.kind == OrderKind::Shortcut) {
                const int idx = findShortcut(item.appKey, usedShortcut);
                if (idx >= 0) {
                    usedShortcut[idx] = true;
                    kept.push_back({OrderKind::Shortcut, -1, appKey(model.shortcuts[idx]), nullptr, false});
                }
            } else if (item.kind == OrderKind::Window) {
                if (std::any_of(activeWindows.begin(), activeWindows.end(),
                    [&](const LiveDockItem& a) { return sameLive(a, item); }))
                    kept.push_back(item);
            }
        }
        live = std::move(kept);
        for (size_t i = 0; i < activePersisted.size(); ++i) {
            const auto& item = activePersisted[i];
            if (std::any_of(live.begin(), live.end(), [&](const LiveDockItem& e) { return sameLive(e, item); })) continue;
            auto pos = live.end();
            for (size_t next = i + 1; next < activePersisted.size() && pos == live.end(); ++next) {
                pos = std::find_if(live.begin(), live.end(), [&](const LiveDockItem& e) { return sameLive(e, activePersisted[next]); });
            }
            if (pos == live.end()) {
                for (size_t prev = i; prev > 0; --prev) {
                    auto found = std::find_if(live.begin(), live.end(), [&](const LiveDockItem& e) { return sameLive(e, activePersisted[prev - 1]); });
                    if (found != live.end()) { pos = found + 1; break; }
                }
            }
            live.insert(pos, item);
        }
        for (const auto& win : activeWindows) {
            if (std::any_of(live.begin(), live.end(), [&](const LiveDockItem& e) { return sameLive(e, win); })) continue;
            size_t insertPos = 0;
            bool foundAnchor = false;
            for (size_t i = 0; i < live.size(); ++i)
                if (live[i].kind == OrderKind::Window) { insertPos = i + 1; foundAnchor = true; }
            if (!foundAnchor) {
                for (size_t i = 0; i < live.size(); ++i)
                    if (live[i].kind == OrderKind::Shortcut) { insertPos = i + 1; foundAnchor = true; }
            }
            live.insert(live.begin() + insertPos, win);
        }
    }

    cachedDockItems.clear();
    cachedDockItems.reserve(live.size());
    std::vector<bool> usedShortcut(pinnedCount, false);
    for (const auto& item : live) {
        if (item.kind == OrderKind::Widget) {
            cachedDockItems.push_back({OrderKind::Widget, item.widget, nullptr});
        } else if (item.kind == OrderKind::Shortcut) {
            const int idx = findShortcut(item.appKey, usedShortcut);
            if (idx >= 0) {
                usedShortcut[idx] = true;
                cachedDockItems.push_back({OrderKind::Shortcut, idx, nullptr});
            }
        } else if (item.kind == OrderKind::Window) {
            if (HWND h = findWindow(item)) cachedDockItems.push_back({OrderKind::Window, -1, h});
        }
    }
    return cachedDockItems;
}

float Dock::dockItemSpan(OrderItem item) const {
    if (item.kind == OrderKind::Widget) return widgetWidth(item.index);
    return glassTheme(model.theme) ? Design::GlassAppPitch : Design::AppPitch;
}

float Dock::dockGapBetween(OrderItem prev, OrderItem curr) const {
    if (prev.kind == OrderKind::Widget && curr.kind == OrderKind::Widget &&
        statusWidget(prev.index) && statusWidget(curr.index)) return 2.f;
    if (prev.kind == OrderKind::Shortcut && curr.kind == OrderKind::Window) return 8.f;
    if (isAppKind(prev.kind) && isAppKind(curr.kind)) return 0.f;
    return Design::Gap;
}

bool Dock::layoutMonitorDock(HMONITOR monitor, View& view, RECT& area, RECT& bounds, POINT& origin,
    std::vector<OrderItem>& overflow) {
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return false;
    area = info.rcWork;
    bounds = info.rcMonitor;
    shellTaskbar.expandWorkArea(monitor, area);
    // Place the window on its display before querying the per-window DPI.
    if (MonitorFromWindow(view.hwnd, MONITOR_DEFAULTTONEAREST) != monitor)
        SetWindowPos(view.hwnd, nullptr, area.left + 18, area.top + 18, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    const float sizes[] = {.84f, 1.0f, 1.15f};
    view.scale = GetDpiForWindow(view.hwnd) / 96.0f * sizes[model.size];
    view.h = Design::DockHeight;
    const int h = px(view.h * view.scale);
    if (model.visibility == DockVisibility::Reserved) {
        RECT reserveBase = bounds;
        if (!model.hideTaskbar) {
            if (const HWND tb = FindWindowW(L"Shell_TrayWnd", nullptr); tb && IsWindowVisible(tb) &&
                MonitorFromWindow(tb, MONITOR_DEFAULTTONEAREST) == monitor) {
                RECT tr{};
                if (GetWindowRect(tb, &tr)) {
                    if (tr.top >= bounds.bottom - 120) reserveBase.bottom = std::min(reserveBase.bottom, tr.top);
                    else if (tr.bottom <= bounds.top + 120) reserveBase.top = std::max(reserveBase.top, tr.bottom);
                }
            }
        }
        RECT strip{};
        if (reservations[monitor].place(hwnd, reserveBase, model.edge == 0, h + 8, strip)) {
            // Include the strip in our layout so the dock and its panels meet at its inner edge.
            area.left = std::max(area.left, strip.left);
            area.right = std::min(area.right, strip.right);
            if (model.edge == 0) area.bottom = strip.bottom;
            else area.top = strip.top;
        }
    }
    const float available = (area.right - area.left - 24) / view.scale;
    const float trailing = dockEditMode ? 82.f : 0.f;
    view.w = Design::DockLeading + Design::DockInset;
    const auto& items = dockItems();
    float required = view.w + trailing;
    for (size_t i = 0; i < items.size(); ++i) {
        const float span = dockItemSpan(items[i]);
        if (span <= 0) continue;
        if (i > 0) required += dockGapBetween(items[i - 1], items[i]);
        required += span;
    }
    if (!items.empty()) required += Design::Gap;
    const bool needsOverflow = required > available;
    const float overflowWidth = needsOverflow ? Design::CardHeight : 0.f;
    overflow.clear();
    bool hasVisible = false;
    OrderItem prevVisible{};
    for (const auto& item : items) {
        const float span = dockItemSpan(item);
        if (span <= 0) continue;
        const float gapBefore = hasVisible ? dockGapBetween(prevVisible, item) : 0.f;
        const float trailingGap = hasVisible ? 0.f : Design::Gap;
        if (overflow.empty() && view.w + gapBefore + span + trailingGap + overflowWidth + trailing <= available) {
            view.w += gapBefore + span + trailingGap;
            prevVisible = item;
            hasVisible = true;
        } else {
            overflow.push_back(item);
        }
    }
    if (!overflow.empty()) view.w += overflowWidth;
    view.w += trailing;
    const int w = px(view.w * view.scale);
    int space = area.right - area.left - w;
    int x = area.left + (model.alignment == 0 ? 12 : model.alignment == 1 ? space / 2 : space - 12);
    int y = model.edge == 0 ? area.bottom - h - 4 : area.top + 4;
    origin = {x, y};
    view.d.layerOpacity = 255;
    view.d.clipped = false;
    SetWindowPos(view.hwnd, model.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, w, h,
        SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return true;
}

void Dock::position() {
    if (dockDragging || !bar.hwnd || !activeMonitor) return;
    pendingPosition = false;
    invalidateDockItems();
    if (model.hideTaskbar && !nativeBarRevealed && !trayNeedsReveal) shellTaskbar.hide(true);
    else if (!model.hideTaskbar) shellTaskbar.restore();
    if (model.visibility != DockVisibility::Reserved) reservations.clear();
    if (!layoutMonitorDock(activeMonitor, bar, work, monitorRect, dockOrigin, folded)) return;
    if (activeMonitor != animationMonitor) refreshDisplayTiming();
    syncMonitorDocks();
    for (auto& entry : otherDocks) {
        layoutMonitorDock(entry->monitor, entry->view, entry->work, entry->bounds,
            entry->origin, entry->folded);
    }
    dockExposure = hidden ? 0.f : 1.f;
    dockOffset = 0.f;
    dockVisSpring.snap(dockExposure);
    paintBar();
    if (panel) positionSheet();
    updateVisibilityTimer();
    updateVisibility();
    ShowWindow(bar.hwnd, hidden ? SW_HIDE : SW_SHOWNOACTIVATE);
}

void Dock::scrollCatalog(int delta) {
    const int next = std::max(0, catalogPage + delta);
    if (next == catalogPage) return;
    catalogPage = next;
    sheet.key = sheet.hover = -1;
    if (motionEnabled()) startAnimTimer();
    paintSheet();
}

void Dock::switchQuickPage(int next) {
    if (quickPage == next) return;
    const float dir = next > quickPage ? 22.f : -22.f;
    quickPage = next;
    sheet.resetMotion();
    triggerSheetContentAnim(dir, 0.f);
    positionSheet();
}

Design::TrayGridLayout Dock::trayGridLayout() const {
    const float dpi = bar.hwnd ? std::max(1.f, GetDpiForWindow(bar.hwnd) / 96.0f) : 1.f;
    const float dockHeight = static_cast<float>(px(bar.h * bar.scale));
    const float maxW = std::max(260.f, (work.right - work.left - 32.f) / dpi * 0.72f);
    const float maxH = std::max(240.f, ((work.bottom - work.top) - (dockHeight + 36.f)) / dpi * 0.78f);
    return Design::computeTrayGrid(static_cast<int>(trayIcons.size()), maxW, maxH);
}

void Dock::positionSheet(bool reopening) {
    if (!panel || sheetClosing) return;
    const int dockHeight = px(bar.h * bar.scale);
    bool wide = page == Studio || page == Calendar || page == Clock;
    sheet.w = page == Studio ? Design::StudioWidth : wide ? 810 : 438;
    sheet.h = page == Studio ? Design::StudioHeight : wide ? 493 : 398;
    if (page == Quick && extra.quickSub == QuickSubPanel::Main && !extra.media.title.empty()) sheet.h = 468;
    if (page == Usage) sheet.h = 563;
    if (page == AiLimits) sheet.h = 540;
    const float dpi = GetDpiForWindow(bar.hwnd) / 96.0f;
    if (page == Apps || page == Recent || page == Search || page == Commands) { sheet.w = 485; sheet.h = 428; }
    if (page == RecycleBin) sheet.h = 252;
    if (page == AppMenu) sizeAppMenu();
    if (page == ChromeWindows) sizeChromeWindows((work.right - work.left - 20.f) / dpi);
    if (page == Overflow) { sheet.w = 345; sheet.h = 116 + 40.f * std::min<size_t>(6, folded.size()); }
    if (page == ContextMenu) { sheet.w = 242; sheet.h = 240; }
    if (page == KeyboardLang) {
        const int rows = std::clamp(static_cast<int>(extra.keyboardLayouts.size()), 1, 5);
        sheet.w = 348.f;
        sheet.h = 154.f + rows * 52.f;
    }
    if (page == NotificationCenter) {
        sheet.w = 384.f;
        sheet.h = extra.notifCalExpanded ? 662.f : 400.f;
    }
    if (page == NetworkPanel) {
        sheet.w = 408.f;
        sheet.h = 404.f;
    }
    if (page == Volume) {
        sheet.w = 408.f;
        sheet.h = 376.f;
    }
    if (page == SystemTray) {
        if (trayMenuIconIndex >= 0) {
            sheet.w = 276.f;
            if (trayMenuLoading) {
                sheet.h = 104.f;
            } else {
                float contentH = 62.f + (trayMenuUnavailable ? 24.f : 0.f);
                for (const auto& item : currentTrayMenu()) contentH += item.separator ? 9.f : 33.f;
                sheet.h = std::clamp(contentH, 96.f, 520.f);
            }
        } else {
            const auto grid = trayGridLayout();
            sheet.w = grid.width;
            sheet.h = grid.height;
        }
    }
    if (page == Focus || page == Countdown) sheet.h = 424;
    if (page == Media || page == Git || page == System ||
        page == World || page == Power) sheet.h = 330;
    const bool attached = page == Studio || page == ContextMenu || page == Quick || page == SystemTray ||
        page == AppMenu || page == Overflow || page == ChromeWindows || page == KeyboardLang || page == NotificationCenter || page == NetworkPanel || page == Volume;
    const float availableHeight = (work.bottom - work.top) - (dockHeight + 26.f);
    sheet.scale = std::min({dpi, (work.right - work.left - 20.0f) / sheet.w, availableHeight / sheet.h});
    const int w = px(sheet.w * sheet.scale), h = px(sheet.h * sheet.scale);
    const float originX = page == Studio || panelDockX < 0 ? bar.w / 2.f : std::clamp(panelDockX, 0.f, bar.w);
    const int centre = dockOrigin.x + px(originX * bar.scale);
    const int x = std::clamp(centre - w / 2, static_cast<int>(work.left + 10), std::max<int>(work.left + 10, work.right - w - 10));
    const int gap = attached ? 0 : 9;
    const int y = model.edge == 0 ? dockOrigin.y - h - gap : dockOrigin.y + dockHeight + gap;
    sheetTail = std::clamp((centre - x) / sheet.scale, 26.f, sheet.w - 26.f);
    const bool opening = positionedPage < 0;
    const bool tabChanged = !opening && page == Studio && positionedTab != tab;
    const bool transition = positionedPage != page || tabChanged ||
        (page == Quick && positionedQuickPage != quickPage);
    if (transition) {
        sheet.resetMotion();
        switchAtMs = 0.0;
        menuHoverVisible = false;
        menuHoverY.initialized = false;
        if (page == Studio && tab == 1) {
            catalogTime = now();
            catalogScrollY.snap(0.f);
            catalogPage = 0;
        }
        if (tabChanged) triggerSheetContentAnim(0.f, 9.f);
        if (marketWidget(page)) animateWidgets();
    }
    const bool animate = (opening || reopening) && motionEnabled();
    const RECT destination{x, y, x + w, y + h};
    const bool moved = !EqualRect(&sheetDestination, &destination);
    sheetDestination = destination;
    positionedPage = page;
    positionedTab = tab;
    positionedQuickPage = quickPage;
    POINT origin{x, y};
    if (animate) {
        const bool compactAnim = page == ContextMenu || page == AppMenu || page == SystemTray || page == ChromeWindows || page == KeyboardLang || page == NotificationCenter || page == NetworkPanel || page == Volume;
        const int slidePx = px((page == Studio ? 14.f : Design::SheetSlidePx) * sheet.scale);
        sheetFrom = {x, model.edge == 0 ? y + slidePx : y - slidePx};
        sheetFromAlpha = .08f;
        if (reopening) {
            RECT current{};
            if (GetWindowRect(panel, &current)) sheetFrom = {current.left, current.top};
            sheetFromAlpha = sheet.d.layerOpacity / 255.f;
        }
        origin = sheetFrom;
        sheetAnimDurationMs = compactAnim ? 145.f : Design::SheetOpenMs;
        sheet.d.layerOpacity = static_cast<BYTE>(px(255.f * sheetFromAlpha));
    } else if (!transition && moved && sheetMotionMs > 0.0 && motionEnabled()) {
        const float progress = std::clamp(static_cast<float>((animNowMs() - sheetMotionMs) / std::max(1.f, sheetAnimDurationMs)), 0.f, 1.f);
        const float posT = easeOutQuint(progress);
        const int slidePx = px((page == Studio ? 14.f : Design::SheetSlidePx) * sheet.scale);
        sheetFrom = {x, model.edge == 0 ? y + slidePx : y - slidePx};
        origin = {
            px(sheetFrom.x + (x - sheetFrom.x) * posT),
            px(sheetFrom.y + (y - sheetFrom.y) * posT)
        };
    } else if (transition || moved || sheetMotionMs <= 0.0) {
        sheetMotionMs = 0.0;
        sheet.d.layerOpacity = 255;
    } else {
        RECT current{};
        if (GetWindowRect(panel, &current)) origin = {current.left, current.top};
    }
    const bool topmost = (GetWindowLongPtrW(panel, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    if (topmost != model.topmost)
        SetWindowPos(panel, model.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    paintSheet(&origin);
    if (animate) {
        sheetMotionMs = animNowMs();
        startAnimTimer();
    }
}

std::optional<POINT> Dock::advanceSheet(double nowMs) {
    if (!panel || sheetMotionMs <= 0.0) return {};
    const float progress = std::clamp(static_cast<float>((nowMs - sheetMotionMs) / std::max(1.f, sheetAnimDurationMs)), 0.f, 1.f);
    const float posT = sheetClosing ? easeInCubic(progress) : easeOutQuint(progress);
    const float alphaT = sheetClosing ? easeInCubic(progress) : easeOutCubic(std::min(1.f, progress * 1.45f));
    const float alpha = sheetClosing
        ? sheetFromAlpha * (1.f - alphaT)
        : (sheetFromAlpha + (1.f - sheetFromAlpha) * alphaT);
    const POINT curOrigin{
        px(sheetFrom.x + (sheetDestination.left - sheetFrom.x) * posT),
        px(sheetFrom.y + (sheetDestination.top - sheetFrom.y) * posT)
    };
    sheet.d.layerOpacity = static_cast<BYTE>(px(255.f * std::clamp(alpha, 0.f, 1.f)));
    if (progress >= 1.f) {
        sheetMotionMs = 0.0;
        if (sheetClosing) {
            closeSheet(false);
            return {};
        }
    }
    return curOrigin;
}

void Dock::open(int id, float x, bool focus) {
    if (const HWND fg = GetForegroundWindow(); fg && IsWindow(fg)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid && pid != GetCurrentProcessId()) lastExternalFg = fg;
    }
    if (weatherWidget(id)) id = Weather;
    const bool reopening = sheetClosing && id == page && motionEnabled();
    if (reopening) {
        sheetClosing = false;
        SetWindowLongPtrW(panel, GWL_EXSTYLE, GetWindowLongPtrW(panel, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    } else if (sheetClosing) {
        closeSheet();
    } else if (panel && id == page && x >= 0 && id != AppMenu) {
        close();
        return;
    }
    if (id != SystemTray && page == SystemTray && trayAction != TrayAction::LeftClick && trayAction != TrayAction::ExecuteMenu && trayBusy()) stopTraySync();
    page = id; listOffset = 0;
    if (id == Usage) {
        usageWeek = false;
        if (motionEnabled()) { usageAnimMs = animNowMs(); startAnimTimer(); }
    }
    timerInput.reset();
    invalidTimerInput = false;
    searchingCatalog = false;
    typingTool = id == Apps || id == Search || id == Commands;
    toolInput.clear();
    extra.quickSub = QuickSubPanel::Main;
    if (id == Quick) {
        quickPage = 0;
        queryQuickStates();
        queryBluetoothDevices();
        queryMedia();
    }
    cityPicker = false;
    if (id == Calendar || id == Clock || id == NotificationCenter) selected = month = Date::from(now());
    if (id == NotificationCenter) refreshNotifications(true);
    if (id == NetworkPanel) { sampleNetworkFast(); refreshWifi(true, true); queryRadios(); }
    if (id == Volume) refreshAudioEndpoints();
    if (id == Git) branch = currentBranch(model.repo);
    if (typingTool) loadApps();
    if (id == Recent) loadRecentFiles();
    if (id == RecycleBin) refreshRecycleBin();
    if (id == Media) queryMedia();
    if (id == Currency) requestCurrency();
    if (id == DayTimeline) { requestWeather(); animateWidgets(); }
    if (id == WhatsApp) { loadApps(); tickProductivity(GetTickCount64()); }
    if (id == Capture || id == OpenWindows) refreshOpenWindows();
    if (id == KeyboardLang) sampleKeyboardLayout();
    if (id == MicControl) refreshMicEndpoints();
    if (id == Quick || id == Storage || id == NetworkWidget || id == Volume ||
        id == Power || id == MicControl || weatherWidget(id) || marketWidget(id) || proWidget(id)) sampleExtras(true);
    if (id == Temperature) sample();
    RECT bounds{};
    GetWindowRect(bar.hwnd, &bounds);
    panelDockX = x;
    if (!panel) {
        panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, PanelClass, L"Vetra · Panel",
            WS_POPUP, bounds.left, bounds.top, 1, 1, hwnd, nullptr, instance, this);
        if (!panel) return;
        sheet.hwnd = panel;
    }
    sheet.resetMotion();
    studioNavY.initialized = false;
    catalogNavY.initialized = false;
    menuHoverY.initialized = false;
    menuHoverVisible = false;
    openingSheet = true;
    positionSheet(reopening);
    ShowWindow(panel, SW_SHOWNOACTIVATE);
    if (focus) SetForegroundWindow(panel);
    openingSheet = false;
    updateVisibilityTimer();
    updateVisibility();
    paintBar();
}
void Dock::close() {
    if (!panel) return;
    if (volumeDrag) { volumeDrag = false; ReleaseCapture(); }
    if (opacitySliderDrag) { opacitySliderDrag = false; ReleaseCapture(); }
    if (dragSource >= 0) { dragSource = dragTarget = -1; ReleaseCapture(); }
    if (sheetClosing) return;
    if (page == SystemTray && trayAction != TrayAction::LeftClick && trayAction != TrayAction::ExecuteMenu && trayBusy()) stopTraySync();
    RECT current{};
    if (IsWindowVisible(panel) && motionEnabled() && GetWindowRect(panel, &current)) {
        sheetClosing = true;
        sheetFrom = {current.left, current.top};
        const int slidePx = px(8.f * sheet.scale);
        sheetDestination = {current.left, model.edge == 0 ? current.top + slidePx : current.top - slidePx, current.right, current.bottom};
        sheetFromAlpha = sheet.d.layerOpacity / 255.f;
        sheetAnimDurationMs = Design::SheetCloseMs;
        sheetMotionMs = animNowMs();
        startAnimTimer();
        SetWindowLongPtrW(panel, GWL_EXSTYLE, GetWindowLongPtrW(panel, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
        paintBar();
        return;
    }
    closeSheet();
}

void Dock::closeSheet(bool redrawBar) {
    if (!panel) return;
    if (page == SystemTray && trayAction != TrayAction::LeftClick && trayAction != TrayAction::ExecuteMenu && trayBusy()) stopTraySync();
    sheetClosing = false;
    sheetMotionMs = 0.0;
    sheetContentMs = 0.0;
    positionedPage = -1;
    switchAtMs = 0.0;
    HWND old = panel;
    panel = nullptr;
    DestroyWindow(old);
    if (page == ChromeWindows) {
        chromeHoverPanel = false;
        chromeLeftAt = 0;
        previewTarget = {};
    }
    page = -1;
    timerInput.reset();
    sheet.hits.clear(); sheet.hwnd = nullptr;
    sheet.resetMotion(); sheet.tracking = false;
    contextWindow = nullptr;
    contextWindows.clear();
    contextIcon.reset();
    contextJumpList = {};
    contextJumpLoading = false;
    ++jumpListGen;
    updateVisibilityTimer();
    if (redrawBar) paintBar();
}
void Dock::report(Text message) {
    notice = std::move(message);
    noticeUntil = GetTickCount64() + 5000;
    noticeAtMs = animNowMs();
    if (motionEnabled()) startAnimTimer();
    if (panel) paintSheet(); else paintBar();
}
void Dock::changed(bool layout) {
    invalidateDockItems();
    syncMeterTargets();
    if (!model.save()) report(L"Ayarlar diske yazılamadı.");
    if (layout) position();
    else { paintBar(); if (panel) paintSheet(); }
}
static ULONGLONG fileValue(FILETIME ft) { return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }
void Dock::sample() {
    const int pdhCpu = readCpuUtilityPct();
    if (pdhCpu >= 0) {
        cpu = pdhCpu;
    } else {
        static ULONGLONG lastSysSampleAt = 0;
        const ULONGLONG nowTick = GetTickCount64();
        FILETIME idle{}, kernel{}, user{};
        if ((!lastSysSampleAt || nowTick - lastSysSampleAt >= 500ULL) && GetSystemTimes(&idle, &kernel, &user)) {
            const auto total = fileValue(kernel) + fileValue(user);
            const auto free = fileValue(idle);
            if (lastTotal && total > lastTotal && free >= lastIdle)
                cpu = static_cast<int>(100 - std::min<ULONGLONG>(100, (free - lastIdle) * 100 / (total - lastTotal)));
            lastIdle = free; lastTotal = total;
            lastSysSampleAt = nowTick;
        }
    }
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) ram = memory.dwMemoryLoad;
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        battery = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
        extra.plugged = power.ACLineStatus == 1;
        extra.batteryMinutes = power.BatteryLifeTime == DWORD(-1) ? -1 : static_cast<int>(power.BatteryLifeTime / 60);
    }
    if (shown(Git) || page == Git) branch = currentBranch(model.repo);
    syncMeterTargets();
}
static bool coversMonitor(const RECT& frame, const RECT& monitor) {
    return !IsRectEmpty(&frame) && !IsRectEmpty(&monitor) &&
        frame.left <= monitor.left + 2 && frame.top <= monitor.top + 2 &&
        frame.right >= monitor.right - 2 && frame.bottom >= monitor.bottom - 2;
}

static bool fullscreenCandidate(HWND window, RECT& bounds) {
    if (!window || !IsWindowVisible(window) || window == GetShellWindow()) return false;
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const auto extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    return !(style & WS_MINIMIZE) && !(extended & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)) &&
        !((style & WS_MAXIMIZE) && (style & WS_CAPTION) == WS_CAPTION) && GetWindowRect(window, &bounds);
}

static RECT fullscreenFrame(HWND window, RECT bounds) {
    wchar_t name[48]{};
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"Progman") == 0 || wcscmp(name, L"WorkerW") == 0) return {};
    DWORD cloaked{};
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return {};
    RECT frame{};
    return SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) ? frame : bounds;
}

static bool fullscreenOn(const RECT& monitor, const RECT& foreground, const POINT* normalForegroundCenter,
    const std::vector<RECT>& frames) {
    if (coversMonitor(foreground, monitor)) return true;
    if (normalForegroundCenter && PtInRect(&monitor, *normalForegroundCenter)) return false;
    return std::any_of(frames.begin(), frames.end(), [&](const RECT& frame) { return coversMonitor(frame, monitor); });
}
static bool dockEdgeZone(const RECT& monitor, const POINT& origin, const View& view,
    int edge, bool hidden, POINT cursor) {
    if (!PtInRect(&monitor, cursor)) return false;
    const int trigger = std::max(4, px(5.f * view.scale));
    const bool atEdge = edge == 0 ? cursor.y >= monitor.bottom - trigger : cursor.y < monitor.top + trigger;
    const bool across = cursor.x >= origin.x && cursor.x < origin.x + px(view.w * view.scale);
    const bool nearDock = edge == 0 ? cursor.y >= origin.y - 8 :
        cursor.y < origin.y + px(view.h * view.scale) + 8;
    return atEdge || (!hidden && across && nearDock);
}

void Dock::updateOtherDockVisibility(ULONGLONG stamp, const POINT& cursor,
    const RECT& foregroundFrame, const POINT* normalForegroundCenter) {
    for (auto& entry : otherDocks) {
        auto& view = entry->view;
        if (view.w <= 0.f) continue;
        entry->fullscreen = fullscreenOn(entry->bounds, foregroundFrame, normalForegroundCenter, fullscreenFrames);
        bool hide = entry->fullscreen;
        if (model.autoHides() && !hide && !dockEditMode) {
            const bool inZone = dockEdgeZone(entry->bounds, entry->origin, view, model.edge, entry->hidden, cursor);
            if (inZone) entry->pointerLeftAt = 0;
            else if (!entry->pointerLeftAt) entry->pointerLeftAt = stamp;
            hide = !inZone && (entry->hidden || stamp - entry->pointerLeftAt >= 550);
        } else {
            entry->pointerLeftAt = 0;
        }
        if (hide == entry->hidden && hide == !IsWindowVisible(view.hwnd)) continue;
        entry->hidden = hide;
        if (hide) ShowWindow(view.hwnd, SW_HIDE);
        else {
            paintBarView(view, entry->folded, &entry->origin);
            ShowWindow(view.hwnd, SW_SHOWNOACTIVATE);
        }
    }
}

void Dock::updateVisibility() {
    if (dockDragging) return;
    const auto stamp = GetTickCount64();
    const HWND foreground = GetForegroundWindow();
    POINT cursor{};
    if (model.autoHides()) GetCursorPos(&cursor);

    const auto spansDisplay = [this](const RECT& bounds) {
        return coversMonitor(bounds, monitorRect) || std::any_of(otherDocks.begin(), otherDocks.end(),
            [&](const auto& entry) { return coversMonitor(bounds, entry->bounds); });
    };
    RECT foregroundFrame{}, foregroundBounds{};
    if (fullscreenCandidate(foreground, foregroundBounds) && spansDisplay(foregroundBounds))
        foregroundFrame = fullscreenFrame(foreground, foregroundBounds);

    POINT foregroundCenter{};
    const POINT* normalForegroundCenter = nullptr;
    DWORD foregroundPid{};
    if (foreground && foreground != GetShellWindow() && IsWindowVisible(foreground) && !IsIconic(foreground) &&
        GetWindowThreadProcessId(foreground, &foregroundPid) && foregroundPid != GetCurrentProcessId() &&
        !(GetWindowLongPtrW(foreground, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) {
        wchar_t name[48]{};
        if (GetClassNameW(foreground, name, ARRAYSIZE(name)) &&
            wcscmp(name, L"Progman") != 0 && wcscmp(name, L"WorkerW") != 0 &&
            wcscmp(name, L"Shell_TrayWnd") != 0 && wcscmp(name, L"Shell_SecondaryTrayWnd") != 0 &&
            (!IsRectEmpty(&foregroundBounds) || GetWindowRect(foreground, &foregroundBounds))) {
            foregroundCenter = {(foregroundBounds.left + foregroundBounds.right) / 2,
                (foregroundBounds.top + foregroundBounds.bottom) / 2};
            normalForegroundCenter = &foregroundCenter;
        }
    }

    const auto needsScan = [&](const RECT& bounds) {
        return !IsRectEmpty(&bounds) && !coversMonitor(foregroundFrame, bounds) &&
            (!normalForegroundCenter || !PtInRect(&bounds, *normalForegroundCenter));
    };
    const bool scanNeeded = needsScan(monitorRect) || std::any_of(otherDocks.begin(), otherDocks.end(),
        [&](const auto& entry) { return needsScan(entry->bounds); });
    if (scanNeeded && (!lastFullscreenScan || foreground != lastFullscreenFg || stamp - lastFullscreenScan >= 1500)) {
        lastFullscreenScan = stamp;
        lastFullscreenFg = foreground;
        fullscreenFrames.clear();
        struct Scan {
            const decltype(spansDisplay)& spansDisplay;
            std::vector<RECT>& frames;
            HWND foreground;
        } scan{spansDisplay, fullscreenFrames, foreground};
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& scan = *reinterpret_cast<Scan*>(data);
            if (window == scan.foreground) return TRUE;
            RECT bounds{};
            if (fullscreenCandidate(window, bounds) && scan.spansDisplay(bounds)) {
                const auto frame = fullscreenFrame(window, bounds);
                if (!IsRectEmpty(&frame)) scan.frames.push_back(frame);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&scan));
    }

    updateOtherDockVisibility(stamp, cursor, foregroundFrame, normalForegroundCenter);
    const bool fullscreen = fullscreenOn(monitorRect, foregroundFrame, normalForegroundCenter, fullscreenFrames);
    if (fullscreen) {
        if (!fullscreenActive) {
            fullscreenActive = true;
            hidden = true;
            pointerLeftAt = 0;
            dockExposure = 0.f;
            dockOffset = 0.f;
            dockVisSpring.snap(0.f);
            bar.d.clipped = false;
            ShowWindow(bar.hwnd, SW_HIDE);
            if (!modal) closeSheet();
        }
        return;
    }
    fullscreenActive = false;
    bool hide = false;
    if (model.autoHides() && !panel && !modal && !dockEditMode && !dockOrderSource && !barVolumeDrag) {
        const bool pointerInZone = dockEdgeZone(monitorRect, dockOrigin, bar, model.edge, hidden, cursor);
        if (pointerInZone) pointerLeftAt = 0;
        else if (!pointerLeftAt) pointerLeftAt = stamp;
        hide = !pointerInZone && (hidden || stamp - pointerLeftAt >= 550);
    } else pointerLeftAt = 0;
    if (hide == hidden) return;
    hidden = hide;
    if (hidden) close();

    if (!motionEnabled()) {
        dockExposure = hidden ? 0.f : 1.f;
        dockOffset = 0.f;
        dockVisSpring.snap(dockExposure);
        bar.d.layerOpacity = 255;
        bar.d.clipped = false;
        if (hidden) {
            ShowWindow(bar.hwnd, SW_HIDE);
        } else {
            bar.d.present(&dockOrigin);
            ShowWindow(bar.hwnd, SW_SHOWNOACTIVATE);
            paintBar();
        }
        return;
    }
    dockVisSpring.response = hidden ? 0.145f : 0.195f;
    dockVisSpring.setTarget(hidden ? 0.f : 1.f, true);
    if (!hidden && !IsWindowVisible(bar.hwnd)) {
        const float travel = Design::DockEdgeTravel * bar.scale;
        dockVisSpring.snap(0.f);
        dockVisSpring.response = 0.195f;
        dockVisSpring.setTarget(1.f, true);
        dockExposure = 0.f;
        dockOffset = model.edge == 0 ? travel : -travel;
        bar.d.layerOpacity = 0;
        const auto origin = advanceVisibility(0.008f);
        paintBar(origin ? &*origin : nullptr);
        ShowWindow(bar.hwnd, SW_SHOWNOACTIVATE);
    }
    startAnimTimer();
}

std::optional<POINT> Dock::advanceVisibility(float dt) {
    if (!dockVisSpring.active(0.001f, 0.006f)) return {};
    const bool stillMoving = dockVisSpring.step(dt, 0.001f, 0.006f);
    dockExposure = std::clamp(dockVisSpring.value, 0.f, 1.f);
    const int w = px(bar.w * bar.scale), h = px(bar.h * bar.scale);
    const float travel = Design::DockEdgeTravel * bar.scale;
    const float slide = 1.f - dockExposure;
    dockOffset = (model.edge == 0 ? travel : -travel) * slide;
    if (!stillMoving && hidden) {
        bar.d.clipped = false;
        ShowWindow(bar.hwnd, SW_HIDE);
        return {};
    }
    const POINT origin{dockOrigin.x, dockOrigin.y + px(dockOffset)};
    bar.d.clipped = px(dockOffset) != 0;
    bar.d.localClip = {0, std::clamp(work.top - origin.y, 0L, static_cast<LONG>(h)), w,
        std::clamp(work.bottom - origin.y, 0L, static_cast<LONG>(h))};
    const float alpha = hidden ? dockExposure : easeOutCubic(std::min(1.f, dockExposure * 1.25f));
    bar.d.layerOpacity = static_cast<BYTE>(px(255.f * std::clamp(alpha, 0.f, 1.f)));
    return origin;
}

void Dock::startMenu() {
    INPUT keys[4]{};
    for (auto& key : keys) key.type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[3].ki.wVk = VK_CONTROL;
    keys[1].ki.wVk = keys[2].ki.wVk = VK_ESCAPE;
    keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT));
}

void Dock::beginDockDragFromCursor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;
    dockDragging = true;
    dockIconDragPending = false;
    dockDragOffset = {cursor.x - dockOrigin.x, cursor.y - dockOrigin.y};
    setPressed(bar, -1);
    if (panel) closeSheet();
    if (GetCapture() != bar.hwnd) SetCapture(bar.hwnd);
}

void Dock::endDockDrag(bool commit) {
    dockDragging = false;
    dockIconDragPending = false;
    if (GetCapture() == bar.hwnd) ReleaseCapture();
    POINT cursor{};
    if (commit && GetCursorPos(&cursor)) {
        const auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        const auto found = std::find(monitors.begin(), monitors.end(), monitor);
        MONITORINFO info{sizeof(info)};
        if (found != monitors.end() && GetMonitorInfoW(monitor, &info)) {
            const int edge = cursor.y < info.rcMonitor.top + (info.rcMonitor.bottom - info.rcMonitor.top) / 2 ? 1 : 0;
            const int w = px(bar.w * bar.scale);
            const int monW = info.rcWork.right - info.rcWork.left;
            int align = model.alignment;
            if (monW - w > 120) {
                const float centerX = static_cast<float>((cursor.x - dockDragOffset.x) + w / 2 - info.rcWork.left);
                const float rel = centerX / std::max(1, monW);
                align = rel < 0.33f ? 0 : rel > 0.67f ? 2 : 1;
            }
            if (edge != model.edge || align != model.alignment) {
                model.edge = edge;
                model.alignment = align;
                changed(true);
                return;
            }
        }
    }
    position();
}

template<class Item>
static void moveEntry(std::vector<Item>& entries, size_t from, size_t to) {
    if (to < from) std::rotate(entries.begin() + to, entries.begin() + from, entries.begin() + from + 1);
    else std::rotate(entries.begin() + from, entries.begin() + from + 1, entries.begin() + to + 1);
}

static bool sameLiveKey(const LiveDockItem& a, const LiveDockItem& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == OrderKind::Widget) return a.widget == b.widget;
    if (a.kind == OrderKind::Shortcut) return sameKey(a.appKey, b.appKey);
    return a.chromeGroup == b.chromeGroup && (a.chromeGroup || a.window == b.window);
}

LiveDockItem Dock::dockItemKey(OrderItem item) const {
    if (item.kind == OrderKind::Widget) return {OrderKind::Widget, item.index, {}, nullptr, false};
    if (item.kind == OrderKind::Shortcut && item.index >= 0 && static_cast<size_t>(item.index) < model.shortcuts.size())
        return {OrderKind::Shortcut, -1, appKey(model.shortcuts[item.index]), nullptr, false};
    if (item.kind == OrderKind::Window) {
        const auto found = std::find_if(extra.windows.begin(), extra.windows.end(),
            [&](const OpenWindow& w) { return w.handle == item.window; });
        const bool chrome = found != extra.windows.end() && isChromeApp(found->app);
        return {OrderKind::Window, -1, {}, chrome ? nullptr : item.window, chrome};
    }
    return {};
}

std::vector<PlacedDockItem> Dock::layoutDockItems(const std::vector<OrderItem>& items) const {
    std::vector<PlacedDockItem> placed;
    placed.reserve(items.size());
    float x = Design::DockLeading;
    for (const auto& item : items) {
        const float span = dockItemSpan(item);
        if (span <= 0) continue;
        if (!placed.empty()) x += dockGapBetween(placed.back().item, item);
        placed.push_back({item, x, span});
        x += span;
    }
    return placed;
}

bool Dock::findVisualX(const LiveDockItem& key, float& outX) const {
    for (const auto& entry : dockVisualX) {
        if (sameLiveKey(entry.key, key)) { outX = entry.x; return true; }
    }
    return false;
}

int Dock::dockDropAt(int x, int y) const {
    const float localX = x / bar.scale, localY = y / bar.scale;
    if (localY < -90.f || localY > bar.h + 90.f) return -1;
    std::vector<OrderItem> visible;
    const auto& all = const_cast<Dock*>(this)->dockItems();
    for (const auto& item : all) {
        if (std::find(folded.begin(), folded.end(), item) == folded.end() && dockItemSpan(item) > 0)
            visible.push_back(item);
    }
    int srcIdx = -1;
    for (size_t i = 0; i < visible.size(); ++i)
        if (visible[i] == dockOrderSource) { srcIdx = static_cast<int>(i); break; }
    if (srcIdx < 0 || visible.empty()) return -1;
    const auto base = layoutDockItems(visible);
    if (base.size() != visible.size()) return -1;
    const float srcSpan = base[srcIdx].span;
    const float bandLeft = base.front().x;
    const float bandRight = base.back().x + base.back().span;
    const float dragLeft = std::clamp(localX - dockDragGrabX, bandLeft - 6.f, bandRight - srcSpan + 6.f);
    const float dragCenter = dragLeft + srcSpan / 2.f;
    int bestDest = 0, dest = 0;
    float bestDist = std::numeric_limits<float>::infinity();
    float prefixEnd = bandLeft;
    OrderItem previous{};
    const auto consider = [&] {
        const float gap = previous ? dockGapBetween(previous, dockOrderSource) : 0.f;
        const float center = (prefixEnd + gap) + srcSpan / 2.f;
        const float dist = std::abs(center - dragCenter);
        if (dist < bestDist) { bestDist = dist; bestDest = dest; }
    };
    // The insertion position depends only on the items before the dragged card.
    for (const auto& cell : base) {
        if (cell.item == dockOrderSource) continue;
        consider();
        if (previous) prefixEnd += dockGapBetween(previous, cell.item);
        prefixEnd += cell.span;
        previous = cell.item;
        ++dest;
    }
    consider();
    return bestDest;
}

void Dock::startDockDragAnim(int cursorX, int cursorY) {
    std::vector<OrderItem> visible;
    for (const auto& item : dockItems()) {
        if (std::find(folded.begin(), folded.end(), item) == folded.end() && dockItemSpan(item) > 0)
            visible.push_back(item);
    }
    const auto base = layoutDockItems(visible);
    int srcIdx = -1;
    for (size_t i = 0; i < base.size(); ++i)
        if (base[i].item == dockOrderSource) { srcIdx = static_cast<int>(i); break; }
    if (srcIdx < 0) return;
    dockOrderDragging = true;
    dockSettlingKey = {};
    dockVisualX.clear();
    dockVisualX.reserve(base.size());
    for (const auto& entry : base) dockVisualX.push_back({dockItemKey(entry.item), entry.x, 0.f});
    const float bandLeft = base.front().x;
    const float bandRight = base.back().x + base.back().span;
    dockDragGrabX = std::clamp(dockOrderOrigin.x / bar.scale - base[srcIdx].x, 0.f, base[srcIdx].span);
    dockDragX = base[srcIdx].x;
    dockDragVel = 0.f;
    dockDragTargetX = std::clamp(cursorX / bar.scale - dockDragGrabX, bandLeft - 6.f, bandRight - base[srcIdx].span + 6.f);
    dockDragLiftSpring.response = 0.14f;
    if (motionEnabled()) {
        dockDragLiftSpring.snap(0.f);
        dockDragLiftSpring.setTarget(1.f, true);
        dockDragLift = 0.f;
    } else {
        dockDragLiftSpring.snap(1.f);
        dockDragLift = 1.f;
    }
    dockDropIndex = dockDropAt(cursorX, cursorY);
    dockAnimTickMs = animNowMs();
    if (motionEnabled()) {
        dockAnimating = true;
        startAnimTimer();
    } else {
        dockAnimating = false;
        advanceDockDrag(dockAnimTickMs);
    }
}

void Dock::finishDockDragAnim() {
    dockVisualX.clear();
    dockSettlingKey = {};
    dockDragVel = 0.f;
    dockDragLift = 0.f;
    dockDragLiftSpring.snap(0.f);
    dockAnimating = false;
}

void Dock::advanceDockDrag(double nowMs) {
    std::vector<OrderItem> visible;
    for (const auto& item : dockItems()) {
        if (std::find(folded.begin(), folded.end(), item) == folded.end() && dockItemSpan(item) > 0)
            visible.push_back(item);
    }
    const auto base = layoutDockItems(visible);
    if (base.empty()) { finishDockDragAnim(); return; }
    const float bandLeft = base.front().x;
    const float bandRight = base.back().x + base.back().span;

    std::vector<PlacedDockItem> target = base;
    if (dockOrderDragging) {
        int srcIdx = -1;
        for (size_t i = 0; i < visible.size(); ++i)
            if (visible[i] == dockOrderSource) { srcIdx = static_cast<int>(i); break; }
        if (srcIdx >= 0 && dockDropIndex >= 0 && dockDropIndex < static_cast<int>(visible.size()) && dockDropIndex != srcIdx) {
            auto preview = visible;
            moveEntry(preview, static_cast<size_t>(srcIdx), static_cast<size_t>(dockDropIndex));
            target = layoutDockItems(preview);
            for (auto& entry : target) {
                if (entry.item != dockOrderSource)
                    entry.x = std::clamp(entry.x, bandLeft, std::max(bandLeft, bandRight - entry.span));
            }
        }
    } else if (dockSettlingKey.kind != OrderKind::None) {
        for (const auto& entry : target) {
            if (sameLiveKey(dockItemKey(entry.item), dockSettlingKey)) {
                dockDragTargetX = entry.x;
                break;
            }
        }
    }

    const float dt = dockAnimTickMs > 0.0 ? std::clamp(static_cast<float>((nowMs - dockAnimTickMs) / 1000.0), 0.001f, 0.05f) : 0.016f;
    dockAnimTickMs = nowMs;
    const bool smooth = motionEnabled();

    auto stepCritSpring = [&](float& pos, float& vel, float dst, float response) {
        if (!smooth) { pos = dst; vel = 0.f; return; }
        const float dx = pos - dst;
        const float omega = 6.f / response;
        const float e = std::exp(-omega * dt);
        const float c2 = vel + omega * dx;
        pos = dst + (dx + c2 * dt) * e;
        vel = (vel - omega * c2 * dt) * e;
        if (std::abs(pos - dst) < 0.12f && std::abs(vel) < 1.5f) { pos = dst; vel = 0.f; }
    };

    float maxDiff = 0.f;
    std::vector<DockAnimPos> nextVisual;
    nextVisual.reserve(target.size());
    for (const auto& entry : target) {
        const auto key = dockItemKey(entry.item);
        float cur = entry.x, vel = 0.f;
        for (const auto& prev : dockVisualX) {
            if (sameLiveKey(prev.key, key)) { cur = prev.x; vel = prev.vel; break; }
        }
        stepCritSpring(cur, vel, entry.x, 0.23f);
        maxDiff = std::max(maxDiff, std::abs(entry.x - cur) + std::abs(vel) * 0.01f);
        nextVisual.push_back({key, cur, vel});
    }
    dockVisualX = std::move(nextVisual);

    if (dockOrderDragging) {
        const float prevX = dockDragX;
        const float alphaDrag = smooth ? (1.f - std::exp(-34.f * dt)) : 1.f;
        dockDragX += (dockDragTargetX - dockDragX) * alphaDrag;
        dockDragVel = std::clamp((dockDragX - prevX) / dt, -1400.f, 1400.f);
        if (std::abs(dockDragTargetX - dockDragX) < 0.15f) dockDragX = dockDragTargetX;
    } else {
        stepCritSpring(dockDragX, dockDragVel, dockDragTargetX, 0.21f);
    }
    const float dragDiff = std::abs(dockDragTargetX - dockDragX) + std::abs(dockDragVel) * 0.01f;

    dockDragLiftSpring.setTarget(dockOrderDragging ? 1.f : 0.f, smooth);
    dockDragLiftSpring.step(dt, 0.002f, 0.01f);
    dockDragLift = std::clamp(dockDragLiftSpring.value, 0.f, 1.f);
    const float liftDiff = std::abs((dockOrderDragging ? 1.f : 0.f) - dockDragLift);

    if (!dockOrderDragging) {
        if (maxDiff < 0.25f && dragDiff < 0.25f && liftDiff < 0.008f) finishDockDragAnim();
    } else {
        if (maxDiff < 0.2f && dragDiff < 0.2f && liftDiff < 0.008f) dockAnimating = false;
    }
}

void Dock::persistLiveOrder() {
    const int p = model.profile;
    ensureDockOrder(p);
    std::vector<DockSlot> active;
    for (const auto& item : liveOrder[p]) {
        if (item.kind == OrderKind::Widget) active.push_back({false, item.widget, {}});
        else if (item.kind == OrderKind::Shortcut) active.push_back({true, -1, item.appKey});
    }
    const auto matchesActive = [&](const DockSlot& slot) {
        return std::any_of(active.begin(), active.end(), [&](const DockSlot& a) {
            if (slot.isApp != a.isApp) return false;
            return slot.isApp ? sameKey(slot.appKey, a.appKey) : slot.widget == a.widget;
        });
    };
    size_t write = 0;
    for (auto& slot : model.dockOrder[p]) {
        if (matchesActive(slot) && write < active.size()) slot = active[write++];
    }
    // Sync enabled non-Apps widget order in model.layouts[p].
    auto& layout = model.layouts[p];
    std::vector<int> orderedWidgets;
    for (const auto& slot : model.dockOrder[p])
        if (!slot.isApp && shown(slot.widget)) orderedWidgets.push_back(slot.widget);
    size_t wIdx = 0;
    for (auto& tile : layout) {
        if (tile.enabled && tile.id != Apps && wIdx < orderedWidgets.size())
            tile.id = orderedWidgets[wIdx++];
    }
    // Sync relative order of visible pinned shortcuts in model.shortcuts & appImages.
    const size_t pinnedCount = pinnedOnBar();
    std::vector<size_t> newPinOrder;
    std::vector<bool> taken(pinnedCount, false);
    for (const auto& slot : model.dockOrder[p]) {
        if (!slot.isApp) continue;
        for (size_t i = 0; i < pinnedCount; ++i) {
            if (!taken[i] && slotMatchesShortcut(slot.appKey, model.shortcuts[i])) {
                taken[i] = true;
                newPinOrder.push_back(i);
                break;
            }
        }
    }
    for (size_t i = 0; i < pinnedCount; ++i) if (!taken[i]) newPinOrder.push_back(i);
    if (newPinOrder.size() == pinnedCount) {
        std::vector<Shortcut> reorderedShortcuts;
        std::vector<std::shared_ptr<Gdiplus::Bitmap>> reorderedImages;
        reorderedShortcuts.reserve(pinnedCount);
        reorderedImages.reserve(pinnedCount);
        for (size_t idx : newPinOrder) {
            reorderedShortcuts.push_back(model.shortcuts[idx]);
            reorderedImages.push_back(idx < appImages.size() ? appImages[idx] : std::shared_ptr<Gdiplus::Bitmap>{});
        }
        for (size_t i = 0; i < pinnedCount; ++i) {
            model.shortcuts[i] = std::move(reorderedShortcuts[i]);
            if (i < appImages.size()) appImages[i] = std::move(reorderedImages[i]);
        }
    }
}

bool Dock::reorderDockSlot(OrderItem source, int destIndex) {
    if (!source || destIndex < 0) return false;
    std::vector<OrderItem> visible;
    for (const auto& item : dockItems()) {
        if (std::find(folded.begin(), folded.end(), item) == folded.end() && dockItemSpan(item) > 0)
            visible.push_back(item);
    }
    int srcVis = -1;
    for (size_t i = 0; i < visible.size(); ++i)
        if (visible[i] == source) { srcVis = static_cast<int>(i); break; }
    if (srcVis < 0 || destIndex >= static_cast<int>(visible.size()) || destIndex == srcVis) return false;
    const OrderItem targetItem = visible[destIndex];

    const auto items = dockItems();
    auto& live = liveOrder[model.profile];
    if (items.size() != live.size()) return false;
    const auto findInItems = [&](OrderItem needle) -> int {
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i] == needle) return static_cast<int>(i);
        return -1;
    };
    const int liveFrom = findInItems(source), liveTo = findInItems(targetItem);
    if (liveFrom < 0 || liveTo < 0 || liveFrom == liveTo) return false;

    const auto oldDockOrder = model.dockOrder[model.profile];
    const auto oldLayouts = model.layouts[model.profile];
    const auto oldShortcuts = model.shortcuts;
    const auto oldImages = appImages;
    const auto oldLive = live;

    moveEntry(live, static_cast<size_t>(liveFrom), static_cast<size_t>(liveTo));
    persistLiveOrder();

    const bool persistedChanged = [&] {
        if (model.dockOrder[model.profile].size() != oldDockOrder.size()) return true;
        for (size_t i = 0; i < oldDockOrder.size(); ++i) {
            const auto& a = model.dockOrder[model.profile][i];
            const auto& b = oldDockOrder[i];
            if (a.isApp != b.isApp || a.widget != b.widget || !sameKey(a.appKey, b.appKey)) return true;
        }
        return false;
    }();

    if (persistedChanged && !model.save()) {
        model.dockOrder[model.profile] = oldDockOrder;
        model.layouts[model.profile] = oldLayouts;
        model.shortcuts = oldShortcuts;
        appImages = oldImages;
        live = oldLive;
        report(L"Sıralama kaydedilemedi.");
        return false;
    }
    position();
    return true;
}

bool Dock::reorderDockItems(OrderItem source, OrderItem target) {
    if (!source || source == target || source.kind != target.kind) return false;
    const int p = model.profile;
    ensureDockOrder(p);
    const auto oldDockOrder = model.dockOrder;
    const auto oldLayouts = model.layouts;
    const auto oldShortcuts = model.shortcuts;
    const auto oldImages = appImages;
    const auto oldLive = liveOrder;

    if (source.kind == OrderKind::Widget) {
        auto& layout = model.layouts[p];
        const auto find = [&](int id) {
            return std::find_if(layout.begin(), layout.end(), [id](Tile tile) { return tile.id == id && tile.enabled; });
        };
        const auto from = find(source.index), to = find(target.index);
        if (from == layout.end() || to == layout.end()) return false;
        moveEntry(layout, from - layout.begin(), to - layout.begin());
        auto& slots = model.dockOrder[p];
        if (source.index != Apps && target.index != Apps) {
            const auto findSlot = [&](int id) {
                return std::find_if(slots.begin(), slots.end(), [id](const DockSlot& s) { return !s.isApp && s.widget == id; });
            };
            const auto sFrom = findSlot(source.index), sTo = findSlot(target.index);
            if (sFrom != slots.end() && sTo != slots.end())
                moveEntry(slots, sFrom - slots.begin(), sTo - slots.begin());
        } else {
            // Reordering the Apps group itself in Settings -> Düzen rebuilds slot order from layout.
            slots.clear();
            ensureDockOrder(p);
        }
        liveOrder[p].clear();
    } else if (source.kind == OrderKind::Shortcut) {
        const auto count = static_cast<int>(model.shortcuts.size());
        if (source.index < 0 || target.index < 0 || source.index >= count || target.index >= count) return false;
        const Text fromKey = appKey(model.shortcuts[source.index]);
        const Text toKey = appKey(model.shortcuts[target.index]);
        moveEntry(model.shortcuts, static_cast<size_t>(source.index), static_cast<size_t>(target.index));
        moveEntry(appImages, static_cast<size_t>(source.index), static_cast<size_t>(target.index));
        for (int prof = 0; prof < ProfileCount; ++prof) {
            ensureDockOrder(prof);
            auto& slots = model.dockOrder[prof];
            std::vector<size_t> appPositions;
            for (size_t i = 0; i < slots.size(); ++i)
                if (slots[i].isApp) appPositions.push_back(i);
            const auto findPos = [&](const Text& key) -> int {
                for (size_t k = 0; k < appPositions.size(); ++k)
                    if (sameKey(slots[appPositions[k]].appKey, key)) return static_cast<int>(k);
                return -1;
            };
            const int fIdx = findPos(fromKey), tIdx = findPos(toKey);
            if (fIdx >= 0 && tIdx >= 0 && fIdx != tIdx) {
                std::vector<DockSlot> apps;
                apps.reserve(appPositions.size());
                for (size_t pos : appPositions) apps.push_back(slots[pos]);
                moveEntry(apps, static_cast<size_t>(fIdx), static_cast<size_t>(tIdx));
                for (size_t k = 0; k < appPositions.size(); ++k) slots[appPositions[k]] = std::move(apps[k]);
            }
            liveOrder[prof].clear();
        }
    } else {
        return false;
    }

    if (!model.save()) {
        model.dockOrder = oldDockOrder;
        model.layouts = oldLayouts;
        model.shortcuts = oldShortcuts;
        appImages = oldImages;
        liveOrder = oldLive;
        report(L"Sıralama kaydedilemedi.");
        return false;
    }
    position();
    return true;
}

void Dock::tick(bool pollWindows) {
    const auto t = now();
    const auto stamp = GetTickCount64();
    if (pollWindows) {
        if (model.hideTaskbar && !nativeBarRevealed && !trayNeedsReveal) shellTaskbar.hide();
        if (model.visibility == DockVisibility::Reserved) {
            for (auto& [mon, res] : reservations) res.verify();
        }
    }
    // SetWinEventHook already tracks window changes in real time; run fallback EnumWindows every 4s.
    static ULONGLONG lastFallbackWinPoll = 0;
    const bool doWinFallback = pollWindows && (!lastFallbackWinPoll || stamp - lastFallbackWinPoll >= 4000);
    if (doWinFallback) lastFallbackWinPoll = stamp;
    const int winState = doWinFallback ? refreshOpenWindows() : 0;
    if (winState) invalidateDockItems();
    if (winState == 2) pendingPosition = true;
    if (installedRequest.valid()) updateInstalledApps();
    bool chromeChanged = false;
    if (pollWindows && panel && page == ChromeWindows && !sheetClosing && syncChromeWindows()) {
        if (chromeWindows.empty()) close();
        else {
            chromeChanged = true;
            sheet.hover = sheet.key = -1;
        }
    }
    const bool reposition = pendingPosition && !dockVisSpring.active(0.001f, 0.006f) && !dockDragging && !dockOrderDragging;
    const bool secondChanged = t.wSecond != lastSecond;
    const bool minuteChanged = t.wMinute != lastMinute || t.wDay != lastDay;
    const bool secActive = anyDockVisible() && (shown(Clock) || shown(SegmentClock) || shown(AnalogClock));
    if (secondChanged && anyDockVisible() && shown(AnalogClock) && motionEnabled()) {
        analogPrevSec = lastSecond >= 0 ? lastSecond : t.wSecond;
        analogSecAtMs = animNowMs();
        startAnimTimer();
    }
    const float prevStatusW = statusBarWidth();
    bool sampled = pollMicrophone();
    bool dataChanged = tickExtras(stamp, t);
    dataChanged |= tickMarket();
    const bool timersChanged = updateTimers(stamp);
    if (statusShown(StatusKeyboard)) {
        const HKL prevHkl = extra.activeKeyboardHkl;
        const Text prevShort = extra.keyboardShort, prevSub = extra.keyboardSub;
        sampleKeyboardLayout();
        if (extra.activeKeyboardHkl != prevHkl || extra.keyboardShort != prevShort || extra.keyboardSub != prevSub)
            sampled = true;
    }
    if (model.statusBar.enabled && (!extra.lastStatusFastAt || stamp - extra.lastStatusFastAt >= 2000)) {
        extra.lastStatusFastAt = stamp;
        const int prevBat = battery;
        const bool prevPlug = extra.plugged, prevEth = extra.netEthernetFast, prevWifi = extra.netWifiFast;
        if (statusShown(StatusMic)) sampleMicrophone();
        if (statusShown(StatusNetwork)) sampleNetworkFast();
        if (statusShown(StatusBattery)) {
            SYSTEM_POWER_STATUS power{};
            if (GetSystemPowerStatus(&power)) {
                battery = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
                extra.plugged = power.ACLineStatus == 1;
                extra.batteryMinutes = power.BatteryLifeTime == DWORD(-1) ? -1 : static_cast<int>(power.BatteryLifeTime / 60);
                syncMeterTargets();
            }
        }
        if (battery != prevBat || extra.plugged != prevPlug ||
            extra.netEthernetFast != prevEth || extra.netWifiFast != prevWifi)
            sampled = true;
    }
    static ULONGLONG lastCpuTick = 0;
    const bool cpuWidgetVisible = shown(System) || page == System || shown(CpuMeter) || shown(RamMeter) ||
        shown(Temperature) || page == Temperature;
    if (cpuWidgetVisible && (!lastCpuTick || stamp - lastCpuTick >= 2000)) {
        lastCpuTick = stamp;
        const int prevCpu = cpu, prevRam = ram, prevBat = battery;
        sample();
        if (cpu != prevCpu || ram != prevRam || battery != prevBat) sampled = true;
    }
    if (stamp >= slowTick) {
        slowTick = stamp + 4500;
        const int prevCpu = cpu, prevRam = ram, prevBat = battery;
        const int prevVol = extra.volume;
        const bool prevMuted = extra.muted, prevPlugged = extra.plugged;
        const long long prevDown = extra.download.back(), prevUp = extra.upload.back();
        const Text prevBranch = branch;
        if (shown(Git) || page == Git) sample();
        sampleExtras();
        if (mediaInUse()) queryMedia();
        if (cpu != prevCpu || ram != prevRam || battery != prevBat ||
            extra.volume != prevVol || extra.muted != prevMuted ||
            extra.plugged != prevPlugged || extra.download.back() != prevDown || extra.upload.back() != prevUp ||
            branch != prevBranch || page == Storage || page == OpenWindows) {
            sampled = true;
        }
    }
    if (std::abs(statusBarWidth() - prevStatusW) > 0.5f) pendingPosition = true;
    const bool repositionNow = reposition || (pendingPosition && !dockVisSpring.active(0.001f, 0.006f) && !dockDragging && !dockOrderDragging);
    const bool noticeExpired = !notice.empty() && stamp >= noticeUntil;
    if (noticeExpired) { notice.clear(); noticeAtMs = 0.0; }
    else if (!notice.empty() && noticeUntil > stamp && noticeUntil - stamp <= 350 && motionEnabled()) startAnimTimer();
    const bool barDirty = dataChanged || timersChanged || sampled || winState || minuteChanged ||
        (secActive && secondChanged) || noticeExpired;
    const bool livePanel = page == Clock || page == Calendar || page == System || page == Git ||
        page == Media || page == OpenWindows || page == Capture || page == Volume || page == MicControl || page == Power ||
        page == NetworkWidget || page == Storage || page == Quick || page == Stopwatch || page == World ||
        page == Usage || page == DayTimeline || page == Currency || page == WhatsApp || page == KeyboardLang || page == NotificationCenter || page == NetworkPanel ||
        page == RecycleBin || weatherWidget(page) || marketWidget(page) || proWidget(page) || page == Overflow;
    const bool sheetDirty = noticeExpired || (page == Clock && secondChanged) ||
        ((page == Countdown || page == Focus || page == NotificationCenter) && timersChanged) ||
        (livePanel && (dataChanged || sampled || winState || minuteChanged));
    if (repositionNow) position();
    else {
        if (barDirty && anyDockVisible()) paintBar();
        if (chromeChanged) positionSheet();
        else if (sheetDirty && panel && !sheetClosing) paintSheet();
    }
    if (pollWindows && panel && page == ChromeWindows && !sheetClosing) updateChromePreviews();
    lastMinute = t.wMinute; lastDay = t.wDay;
    lastSecond = t.wSecond;
}
void Dock::menu() {
    open(ContextMenu);
}
void Dock::openOverflow(float anchor) {
    if (folded.empty()) return;
    open(Overflow, anchor);
}
void Dock::showSystemQuick() { requestNativeFlyout(NativeFlyout::Quick); }

void Dock::requestNativeFlyout(NativeFlyout flyout) {
    if (panel) closeSheet();
    nativeFlyout = flyout;
    nativeFlyoutTries = 0;
    nativeQuickOpen = false;
    nativeBarRevealed = true;
    shellTaskbar.revealPrimary();
    if (model.topmost)
        SetWindowPos(bar.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!SetTimer(hwnd, 8, 85, nullptr)) updateNativeFlyout();
}

void Dock::updateNativeFlyout() {
    KillTimer(hwnd, 8);
    const auto flyout = nativeFlyout;
    nativeFlyout = NativeFlyout::None;
    if (flyout == NativeFlyout::Quick) {
        nativeQuickOpen = true;
        systemShortcut('A');
        if (SetTimer(hwnd, 8, 350, nullptr)) return;
    }
    if (nativeQuickOpen) {
        const HWND quick = FindWindowW(L"ControlCenterWindow", nullptr);
        // Explorer can show its own bar for as long as Quick Settings has
        // focus. Leave it alone until the native window is dismissed.
        if (quick && IsWindowVisible(quick) && SetTimer(hwnd, 8, 150, nullptr)) return;
        nativeQuickOpen = false;
    }
    nativeBarRevealed = false;
    shellTaskbar.hide();
}

void Dock::systemShortcut(WORD key, bool control) {
    close();
    INPUT keys[6]{};
    const WORD sequence[] = {VK_LWIN, VK_CONTROL, key};
    int count = 0;
    for (WORD code : sequence) {
        if (code == VK_CONTROL && !control) continue;
        keys[count].type = INPUT_KEYBOARD;
        keys[count++].ki.wVk = code;
    }
    for (int i = 0; i < count; ++i) {
        keys[count + i] = keys[count - i - 1];
        keys[count + i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    if (SendInput(2 * count, keys, sizeof(INPUT)) != static_cast<UINT>(2 * count))
        report(L"Windows denetimi açılamadı.");
}

void Dock::launch(const Text& target, const Text& dir) {
    if (target.empty()) return;
    auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(panel ? panel : hwnd, L"open", target.c_str(), nullptr,
        dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL));
    if (result <= 32) report(L"Bağlantı veya uygulama açılamadı.");
}
void Dock::changeVolumeAt(float x, bool dragging, bool commit) {
    if (extra.volume < 0) return;
    const float left = width(quickVolumeTrack) > 0.f ? quickVolumeTrack.left : Design::QuickVolumeX;
    const float span = width(quickVolumeTrack) > 0.f ? width(quickVolumeTrack) : Design::QuickVolumeWidth;
    const float level = std::clamp((x - left) / span, 0.f, 1.f);
    if (dragging) setVolumeLevel(level, true, true);
    else setVolumeLevel(level, commit, false);
}
void Dock::changeBarVolumeAt(float localX, bool dragging, bool commit) {
    if (width(barVolumeTrack) <= 0.f) return;
    const float level = std::clamp((localX - barVolumeTrack.left) / width(barVolumeTrack), 0.f, 1.f);
    if (dragging) setVolumeLevel(level, true, true);
    else setVolumeLevel(level, commit, false);
}
void Dock::advanceBarVolume(double nowMs) {
    if (barVolumeTarget < 0.f) { barVolumeAnimating = false; return; }
    if (!motionEnabled() || barVolumeLevel < 0.f) {
        barVolumeLevel = barVolumeTarget;
        barVolumeAnimating = false;
        return;
    }
    const float dt = barVolumeTickMs > 0.0 ? std::clamp(static_cast<float>((nowMs - barVolumeTickMs) / 1000.0), 0.001f, 0.05f) : 0.016f;
    barVolumeTickMs = nowMs;
    const float speed = (barVolumeDrag || volumeDrag) ? 40.f : 22.f;
    barVolumeLevel += (barVolumeTarget - barVolumeLevel) * (1.f - std::exp(-speed * dt));
    if (std::abs(barVolumeTarget - barVolumeLevel) < 0.002f) {
        barVolumeLevel = barVolumeTarget;
        barVolumeAnimating = false;
    }
}

LRESULT Dock::message(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    const bool isOwner = w == hwnd;
    if (isOwner && trayRestore && msg == trayRestore) {
        reservations.clear();
        tray(true);
        if (model.hideTaskbar) shellTaskbar.start();
        position();
        return 0;
    }
    if (msg == WM_DPICHANGED && w != panel && hwnd) {
        PostMessageW(hwnd, WM_DISPLAYCHANGE, 0, 0);
        return 0;
    }
    if (w != bar.hwnd && w != panel) {
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        const bool pointerAction = (msg >= WM_LBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) ||
            msg == WM_MOUSEWHEEL || (msg == WM_MOUSEMOVE && !panel);
        if (!(pointerAction && selectDockWindow(w)) &&
            (!isOwner || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) ||
                msg == WM_MOUSELEAVE || msg == WM_ACTIVATE || msg == WM_CAPTURECHANGED)) {
            if (msg == WM_CLOSE || msg == WM_MOUSELEAVE) return 0;
            return DefWindowProcW(w, msg, wp, lp);
        }
    }
    const bool isBar = w == bar.hwnd;
    auto& v = isBar ? bar : sheet;
    auto repaint = [&]() { if (isBar) paintBar(nullptr, false); else if (w == panel) paintSheet(); };
    auto orderRow = [&](bool handle) {
        const float x = GET_X_LPARAM(lp) / v.scale, y = GET_Y_LPARAM(lp) / v.scale;
        for (const auto& [bounds, index] : orderRows)
            if (x >= bounds.left && x < (handle ? bounds.left + 30 : bounds.right) &&
                y >= bounds.top && y < bounds.bottom) return index;
        return -1;
    };
    const bool orderSheet = !isBar && page == Studio && (tab == 0 || tab == 3);
    const auto movedForOrder = [&]() {
        return std::abs(GET_X_LPARAM(lp) - dockOrderOrigin.x) >= std::max(GetSystemMetrics(SM_CXDRAG), 7) ||
            std::abs(GET_Y_LPARAM(lp) - dockOrderOrigin.y) >= std::max(GetSystemMetrics(SM_CYDRAG), 7);
    };
    if (w == panel && sheetClosing && ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
        (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST))) return 0;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        if (isBar || (page == ChromeWindows && chromeHoverPanel)) return MA_NOACTIVATE;
        break;
    case WM_MOUSEMOVE: {
        if (isBar && dockDragging) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                const POINT origin{cursor.x - dockDragOffset.x, cursor.y - dockDragOffset.y};
                bar.d.present(&origin);
            }
            SetCursor(curSizeAll);
            return 0;
        }
        if (isBar && dockIconDragPending) {
            const int dx = GET_X_LPARAM(lp) - dockIconDragOrigin.x;
            const int dy = GET_Y_LPARAM(lp) - dockIconDragOrigin.y;
            if (std::abs(dx) >= GetSystemMetrics(SM_CXDRAG) || std::abs(dy) >= GetSystemMetrics(SM_CYDRAG)) {
                beginDockDragFromCursor();
                POINT cursor{};
                if (GetCursorPos(&cursor)) {
                    const POINT origin{cursor.x - dockDragOffset.x, cursor.y - dockDragOffset.y};
                    bar.d.present(&origin);
                }
                SetCursor(curSizeAll);
            }
            return 0;
        }
        if (isBar && barVolumeDrag) {
            changeBarVolumeAt(GET_X_LPARAM(lp) / bar.scale, true, false);
            SetCursor(curHand);
            return 0;
        }
        if (isBar && dockOrderSource) {
            const int dx = GET_X_LPARAM(lp) - dockOrderOrigin.x;
            const int dy = GET_Y_LPARAM(lp) - dockOrderOrigin.y;
            const float localY = GET_Y_LPARAM(lp) / bar.scale;
            const bool verticalPull = !dockEditMode && !dockVisSpring.active(0.001f, 0.006f) && (
                (!dockOrderDragging && std::abs(dy) >= std::max(GetSystemMetrics(SM_CYDRAG) * 2, 12) && std::abs(dy) > std::abs(dx) * 1.4f) ||
                (dockOrderDragging && (localY < -36.f || localY > bar.h + 36.f)));
            if (verticalPull) {
                dockOrderSource = {};
                dockDropIndex = -1;
                dockOrderDragging = false;
                finishDockDragAnim();
                paintBar();
                beginDockDragFromCursor();
                SetCursor(curSizeAll);
                return 0;
            }
            const bool started = !dockOrderDragging && movedForOrder();
            if (started) {
                setPressed(bar, -1);
                if (panel && (page == ChromeWindows || page == AppMenu)) close();
                startDockDragAnim(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            if (dockOrderDragging) {
                std::vector<OrderItem> visible;
                for (const auto& item : dockItems()) {
                    if (std::find(folded.begin(), folded.end(), item) == folded.end() && dockItemSpan(item) > 0)
                        visible.push_back(item);
                }
                const auto base = layoutDockItems(visible);
                float srcSpan = dockItemSpan(dockOrderSource);
                if (!base.empty()) {
                    const float bandLeft = base.front().x;
                    const float bandRight = base.back().x + base.back().span;
                    dockDragTargetX = std::clamp(GET_X_LPARAM(lp) / bar.scale - dockDragGrabX,
                        bandLeft - 6.f, bandRight - srcSpan + 6.f);
                }
                dockDropIndex = dockDropAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
                if (motionEnabled()) {
                    if (!dockAnimating) {
                        dockAnimating = true;
                        dockAnimTickMs = animNowMs();
                        startAnimTimer();
                    }
                } else {
                    advanceDockDrag(animNowMs());
                    repaint();
                }
                SetCursor(curSizeWE);
            }
            return 0;
        }
        if (!isBar && volumeDrag) {
            changeVolumeAt(GET_X_LPARAM(lp) / v.scale, true, false);
            SetCursor(curHand);
            return 0;
        }
        if (!isBar && opacitySliderDrag) {
            setOpacityFromX(GET_X_LPARAM(lp) / v.scale, false);
            SetCursor(curHand);
            return 0;
        }
        if (!isBar && dragSource >= 0) {
            const int target = orderRow(false);
            if (dragTarget != target) { dragTarget = target; repaint(); }
            SetCursor(curSizeAll);
            return 0;
        }
        if (!v.tracking) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, w, 0}; TrackMouseEvent(&track); v.tracking = true; }
        const int index = v.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (isBar && motionEnabled() && !dockEditMode) {
            const float lx = GET_X_LPARAM(lp) / bar.scale;
            const bool overApp = index >= 0 && isAppKind(bar.hits[index].order.kind);
            if (overApp) {
                if (!barCursorSmoothX.initialized) barCursorSmoothX.snap(lx);
                else barCursorSmoothX.setTarget(lx, true);
                barWaveStrength.setTarget(1.f, true);
                if (barWaveStrength.active(0.004f, 0.01f) || barCursorSmoothX.active(0.25f, 2.f)) startAnimTimer();
            } else if (barWaveStrength.target > 0.f) {
                barWaveStrength.setTarget(0.f, true);
                startAnimTimer();
            }
        }
        const bool grip = orderSheet && orderRow(true) >= 0;
        const bool reorder = isBar && dockEditMode && index >= 0 && v.hits[index].order;
        if (isBar && !dockEditMode && index >= 0 && v.hits[index].windowPreview) trackChromeHover();
        SetCursor(grip ? curSizeAll : reorder ? curSizeWE :
            index < 0 ? curArrow : curHand);
        if (v.hover != index) {
            changeHover(v, index);
            v.key = -1;
            if (!motionEnabled()) repaint();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v.tracking = false;
        if (isBar && barWaveStrength.target > 0.f) {
            barWaveStrength.setTarget(0.f, motionEnabled());
            if (motionEnabled()) startAnimTimer();
        }
        if (v.pressed != -1) setPressed(v, -1);
        if (v.hover != -1) {
            changeHover(v, -1);
            if (!motionEnabled()) repaint();
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (isBar && !dockDragging && !modal) {
            if (dockSettlingKey.kind != OrderKind::None || dockAnimating) {
                finishDockDragAnim();
                paintBar();
            }
            if (!dockEditMode && width(barVolumeTrack) > 0.f) {
                const float lx = GET_X_LPARAM(lp) / bar.scale, ly = GET_Y_LPARAM(lp) / bar.scale;
                if (lx >= barVolumeTrack.left - 8.f && lx <= barVolumeTrack.right + 8.f &&
                    ly >= barVolumeTrack.top - 8.f && ly <= barVolumeTrack.bottom + 10.f) {
                    barVolumeDrag = true;
                    SetCapture(w);
                    changeBarVolumeAt(lx, false, true);
                    return 0;
                }
            }
            const int index = bar.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index >= 0) { setPressed(bar, index); paintBar(nullptr, false); }
            if (index >= 0 && bar.hits[index].order) {
                SetCapture(w);
                if (GetCapture() == w) {
                    dockOrderSource = bar.hits[index].order;
                    dockDropIndex = -1;
                    dockOrderOrigin = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                    dockOrderDragging = false;
                }
                return 0;
            }
            if (!dockEditMode && !dockVisSpring.active(0.001f, 0.006f) && (index <= 0)) {
                dockIconDragPending = true;
                dockIconDragOrigin = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                SetCapture(w);
                return 0;
            }
        }
        if (!isBar && page == Studio && tab == 2 && glassTheme(model.theme) && width(opacitySliderTrack) > 0.f) {
            const float x = GET_X_LPARAM(lp) / v.scale, y = GET_Y_LPARAM(lp) / v.scale;
            if (x >= opacitySliderTrack.left - 6.f && x <= opacitySliderTrack.right + 6.f &&
                y >= opacitySliderTrack.top - 10.f && y <= opacitySliderTrack.bottom + 10.f) {
                opacitySliderDrag = true;
                SetCapture(w);
                setOpacityFromX(x, false);
                return 0;
            }
        }
        if (!isBar && ((page == Quick && extra.quickSub == QuickSubPanel::Main) || page == Volume) && extra.volume >= 0 && width(quickVolumeTrack) > 0.f) {
            const float x = GET_X_LPARAM(lp) / v.scale, y = GET_Y_LPARAM(lp) / v.scale;
            if (x >= quickVolumeTrack.left - 8.f && x <= quickVolumeTrack.right + 8.f &&
                y >= quickVolumeTrack.top - 10.f && y <= quickVolumeTrack.bottom + 10.f) {
                volumeDrag = true;
                SetCapture(w);
                changeVolumeAt(x, false, true);
                return 0;
            }
        }
        if (orderSheet) {
            dragSource = dragTarget = orderRow(true);
            if (dragSource >= 0) { SetCapture(w); repaint(); return 0; }
        }
        if (!isBar) {
            const int index = sheet.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index >= 0) { setPressed(sheet, index); paintSheet(); }
        }
        return 0;
    case WM_LBUTTONUP: {
        if (v.pressed != -1) setPressed(v, -1);
        if (isBar && dockDragging) { endDockDrag(true); return 0; }
        if (isBar && dockIconDragPending) {
            dockIconDragPending = false;
            if (GetCapture() == w) ReleaseCapture();
        }
        if (isBar && barVolumeDrag) {
            barVolumeDrag = false;
            if (GetCapture() == w) ReleaseCapture();
            changeBarVolumeAt(GET_X_LPARAM(lp) / bar.scale, false, true);
            return 0;
        }
        if (isBar && dockOrderSource) {
            const auto source = dockOrderSource;
            const bool dragged = dockOrderDragging || movedForOrder();
            if (dragged && !dockOrderDragging) startDockDragAnim(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            const int drop = dragged ? dockDropAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) : -1;
            const auto sourceKey = dockItemKey(source);
            dockOrderSource = {};
            dockDropIndex = -1;
            dockOrderDragging = false;
            if (GetCapture() == w) ReleaseCapture();
            if (dragged) {
                const int oldOriginX = dockOrigin.x;
                if (motionEnabled()) dockSettlingKey = sourceKey;
                else finishDockDragAnim();
                reorderDockSlot(source, drop);
                if (motionEnabled() && dockSettlingKey.kind != OrderKind::None) {
                    const float shift = (oldOriginX - dockOrigin.x) / bar.scale;
                    if (shift != 0.f) {
                        dockDragX += shift;
                        for (auto& pos : dockVisualX) pos.x += shift;
                    }
                    dockAnimating = true;
                    dockAnimTickMs = animNowMs();
                    advanceDockDrag(dockAnimTickMs);
                    startAnimTimer();
                    repaint();
                } else {
                    finishDockDragAnim();
                    repaint();
                }
                return 0;
            }
            const int index = bar.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (index < 0 || bar.hits[index].order != source) { repaint(); return 0; }
        }
        if (!isBar && opacitySliderDrag) {
            opacitySliderDrag = false;
            ReleaseCapture();
            setOpacityFromX(GET_X_LPARAM(lp) / v.scale, true);
            return 0;
        }
        if (!isBar && volumeDrag) {
            volumeDrag = false;
            ReleaseCapture();
            changeVolumeAt(GET_X_LPARAM(lp) / v.scale, false, true);
            return 0;
        }
        if (!isBar && dragSource >= 0) {
            const int from = dragSource, to = orderRow(false);
            dragSource = dragTarget = -1;
            ReleaseCapture();
            bool moved = false;
            if (orderSheet && to >= 0 && to != from && tab == 3) {
                moved = reorderDockItems({OrderKind::Shortcut, from}, {OrderKind::Shortcut, to});
            } else if (orderSheet && to >= 0 && to != from) {
                auto& layout = model.layouts[model.profile];
                moved = reorderDockItems({OrderKind::Widget, layout[from].id}, {OrderKind::Widget, layout[to].id});
            }
            if (!moved) repaint();
            return 0;
        }
        const int index = v.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (index >= 0) { auto action = v.hits[index].action; action(); }
        else repaint();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (v.pressed != -1) setPressed(v, -1);
        if (isBar && dockDragging) { endDockDrag(false); return 0; }
        if (isBar && dockIconDragPending) { dockIconDragPending = false; return 0; }
        if (isBar && barVolumeDrag) { barVolumeDrag = false; paintBar(); return 0; }
        if (isBar && dockOrderSource) {
            const bool redraw = dockOrderDragging;
            const auto sourceKey = dockItemKey(dockOrderSource);
            dockOrderSource = {};
            dockDropIndex = -1;
            dockOrderDragging = false;
            if (redraw && motionEnabled()) {
                dockSettlingKey = sourceKey;
                dockAnimating = true;
                dockAnimTickMs = animNowMs();
                startAnimTimer();
            } else {
                finishDockDragAnim();
                if (redraw) repaint();
            }
            return 0;
        }
        if (!isBar && opacitySliderDrag) { opacitySliderDrag = false; model.save(); }
        if (!isBar) volumeDrag = false;
        if (!isBar && dragSource >= 0) { dragSource = dragTarget = -1; repaint(); }
        return 0;
    case WM_RBUTTONUP: {
        if (isBar && (dockOrderSource || barVolumeDrag || dockDragging || dockIconDragPending)) return 0;
        const int index = v.at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
        if (index >= 0) {
            auto action = v.hits[index].secondary;
            action();
        } else if (isBar) open(ContextMenu, GET_X_LPARAM(lp) / bar.scale);
        else if (w == panel && page == SystemTray) {
            if (trayMenuIconIndex >= 0) {
                backTrayMenu();
            } else {
                const float x = GET_X_LPARAM(lp) / sheet.scale, y = GET_Y_LPARAM(lp) / sheet.scale;
                POINT screenPt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ClientToScreen(panel, &screenPt);
                for (const auto& [bounds, idx] : trayRows) {
                    if (x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom) {
                        activateTrayIcon(idx, true, screenPt);
                        break;
                    }
                }
            }
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (isBar && !dockEditMode && (width(barVolumeCard) > 0.f || width(statusVolumeHit) > 0.f || width(statusKeyboardHit) > 0.f)) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float lx = pt.x / bar.scale, ly = pt.y / bar.scale;
            const auto inBox = [lx, ly](Box b) {
                return width(b) > 0.f && lx >= b.left && lx < b.right && ly >= b.top && ly < b.bottom;
            };
            if (inBox(barVolumeCard) || inBox(statusVolumeHit)) {
                const float step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.04f : -0.04f;
                const float base = barVolumeTarget >= 0.f ? barVolumeTarget :
                    (extra.volume >= 0 ? extra.volume / 100.f : 0.f);
                setVolumeLevel(std::clamp(base + step, 0.f, 1.f), false);
                return 0;
            }
            if (inBox(statusKeyboardHit)) {
                cycleKeyboardLayout(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1);
                return 0;
            }
        }
        if (!isBar && page == Studio && tab == 2 && glassTheme(model.theme) && width(opacitySliderTrack) > 0.f) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float lx = pt.x / sheet.scale, ly = pt.y / sheet.scale;
            if (lx >= opacitySliderTrack.left - 12.f && lx <= opacitySliderTrack.right + 56.f &&
                ly >= opacitySliderTrack.top - 14.f && ly <= opacitySliderTrack.bottom + 14.f) {
                const int step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4 : -4;
                const int next = std::clamp(model.opacity + step, 0, 100);
                if (next != model.opacity) {
                    model.opacity = next;
                    opacityAnim.setTarget(static_cast<float>(next), motionEnabled());
                    changed(false);
                    if (motionEnabled() && opacityAnim.active(0.15f, 0.5f)) startAnimTimer();
                }
                return 0;
            }
        }
        if (!isBar && ((page == Quick && extra.quickSub == QuickSubPanel::Main) || page == Volume) && width(quickVolumeTrack) > 0.f) {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(w, &pt);
            const float lx = pt.x / sheet.scale, ly = pt.y / sheet.scale;
            if (lx >= quickVolumeTrack.left - 48.f && lx <= quickVolumeTrack.right + 48.f &&
                ly >= quickVolumeTrack.top - 18.f && ly <= quickVolumeTrack.bottom + 18.f) {
                const float step = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.04f : -0.04f;
                const float base = barVolumeTarget >= 0.f ? barVolumeTarget :
                    (extra.volume >= 0 ? extra.volume / 100.f : 0.f);
                setVolumeLevel(std::clamp(base + step, 0.f, 1.f), false);
                return 0;
            }
        }
        if (!isBar && (page == Tasks || page == Calendar || page == Clock || page == Sales || page == Growth || page == Quick ||
            page == OpenWindows || page == Storage || page == Stopwatch || page == Studio || page == SystemTray ||
            page == Apps || page == AppMenu || page == Recent || page == Search || page == Commands || page == Capture ||
            page == Overflow || page == ChromeWindows || page == Usage || page == KeyboardLang || page == MicControl || page == NotificationCenter || page == NetworkPanel || page == Volume)) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
            if (page == Studio) {
                if (tab == 0) orderPage = std::max(0, orderPage + delta);
                if (tab == 1) { scrollCatalog(delta); return 0; }
                if (tab == 3) shortcutOffset = std::max(0, shortcutOffset + 8 * delta);
            } else if (page == Quick && extra.quickSub == QuickSubPanel::Main) { switchQuickPage(delta > 0 ? 1 : 0); return 0; }
            else if (page == ChromeWindows) { scrollChromeWindows(delta); return 0; }
            else listOffset = std::max(0, listOffset + delta);
            repaint();
        }
        return 0;
    case WM_KEYDOWN:
        if (!isBar && timerInput) {
            if (wp == VK_ESCAPE) {
                timerInput.reset();
                invalidTimerInput = false;
                paintSheet();
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_TAB) {
                const bool applied = applyTimerInput();
                if (wp == VK_RETURN || !applied) return 0;
            } else if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) {
                selectTimerInput = true;
                paintSheet();
                return 0;
            }
        }
        if (isBar && wp == VK_ESCAPE && dockDragging) {
            endDockDrag(false);
        } else if (isBar && wp == VK_ESCAPE && dockOrderSource) {
            const bool redraw = dockOrderDragging;
            const auto sourceKey = dockItemKey(dockOrderSource);
            dockOrderSource = {};
            dockDropIndex = -1;
            dockOrderDragging = false;
            if (GetCapture() == w) ReleaseCapture();
            if (redraw && motionEnabled()) {
                dockSettlingKey = sourceKey;
                dockAnimating = true;
                dockAnimTickMs = animNowMs();
                startAnimTimer();
            } else {
                finishDockDragAnim();
                if (redraw) repaint();
            }
        } else if (isBar && wp == VK_ESCAPE && dockEditMode) {
            dockEditMode = false;
            dockEditSpring.snap(0.f);
            KillTimer(hwnd, 4);
            position();
        } else if (!isBar && wp == VK_ESCAPE) {
            if (page == Quick && extra.quickSub != QuickSubPanel::Main) {
                closeQuickSub();
            }
            else if (cityPicker) { cityPicker = false; sheet.resetMotion(); triggerSheetContentAnim(-16.f, 0.f); paintSheet(); }
            else if (page == SystemTray && trayMenuIconIndex >= 0) {
                backTrayMenu();
            }
            else close();
        }
        else if (!isBar && wp == VK_F5 && page == SystemTray) {
            refreshSystemTray(true);
        } else if (!isBar && wp == VK_F5 && page == ChromeWindows) {
            refreshChromeWindows();
        } else if (!isBar && wp == VK_TAB && !v.hits.empty()) {
            typingTool = false;
            const int count = static_cast<int>(v.hits.size());
            v.key = (v.key + ((GetKeyState(VK_SHIFT) < 0) ? (v.key < 0 ? 0 : count - 1) : 1) + count) % count;
            v.hover = -1; repaint();
        } else if (!isBar && wp == VK_RETURN && typingTool && v.key < 0 && (page == Search || page == Commands)) {
            runTool(toolInput);
        } else if (!isBar && (wp == VK_RETURN || (wp == VK_SPACE && !searchingCatalog && !typingTool)) && v.key >= 0 && v.key < static_cast<int>(v.hits.size())) {
            auto action = v.hits[v.key].action; action();
        }
        return 0;
    case WM_CHAR:
        if (!isBar && timerInput && (wp == VK_BACK || (wp >= L'0' && wp <= L'9'))) {
            if (selectTimerInput) timerInput->clear();
            if (wp == VK_BACK) {
                if (!selectTimerInput && !timerInput->empty()) timerInput->pop_back();
            } else if (timerInput->size() < 4) *timerInput += static_cast<wchar_t>(wp);
            selectTimerInput = false;
            invalidTimerInput = false;
            paintSheet();
        } else if (!isBar && page == Studio && tab == 1 && searchingCatalog) {
            if (wp == VK_BACK && !catalogSearch.empty()) catalogSearch.pop_back();
            else if (wp >= 32 && wp != 127 && catalogSearch.size() < 60) catalogSearch += static_cast<wchar_t>(wp);
            catalogPage = 0;
            catalogScrollY.snap(0.f);
            paintSheet();
        } else if (!isBar && typingTool && (page == Apps || page == Search || page == Commands)) {
            if (wp == VK_BACK && !toolInput.empty()) toolInput.pop_back();
            else if (wp >= 32 && wp != 127 && toolInput.size() < 512) toolInput += static_cast<wchar_t>(wp);
            listOffset = 0;
            paintSheet();
        }
        return 0;
    case WM_ACTIVATE:
        if (!isBar && w == panel && !openingSheet && !sheetClosing && !modal && !trayBusy() && LOWORD(wp) == WA_INACTIVE) {
            const HWND target = reinterpret_cast<HWND>(lp);
            if (target != bar.hwnd && target != panel) {
                if (dockEditMode) { dockEditMode = false; dockEditSpring.snap(0.f); KillTimer(hwnd, 4); position(); }
                close();
            }
        }
        if (isBar && LOWORD(wp) == WA_INACTIVE) {
            if (bar.hover != -1 || bar.key != -1 || bar.pressed != -1) {
                bar.hover = bar.key = bar.pressed = -1;
                barWaveStrength.setTarget(0.f, motionEnabled());
                paintBar();
            }
        }
        return 0;
    case WM_TIMER:
        if (isOwner && wp == ChromePreviewTick) updateChromePreviews();
        if (isOwner && wp == ChromeHoverTick) updateChromeHover();
        if (isOwner && wp == InstalledAppsTick) updateInstalledApps();
        if (isOwner && wp == WindowSyncTick) {
            KillTimer(hwnd, WindowSyncTick);
            windowSyncScheduled = false;
            const int winState = refreshOpenWindows();
            bool kbChanged = false;
            if (statusShown(StatusKeyboard)) {
                const HKL prevHkl = extra.activeKeyboardHkl;
                const Text prevShort = extra.keyboardShort, prevSub = extra.keyboardSub;
                sampleKeyboardLayout();
                kbChanged = extra.activeKeyboardHkl != prevHkl || extra.keyboardShort != prevShort || extra.keyboardSub != prevSub;
            }
            if (winState) invalidateDockItems();
            if (winState == 2) pendingPosition = true;
            bool chromeChanged = false;
            if (panel && page == ChromeWindows && !sheetClosing && syncChromeWindows()) {
                if (chromeWindows.empty()) close();
                else {
                    chromeChanged = true;
                    sheet.hover = sheet.key = -1;
                }
            }
            if (pendingPosition && !dockVisSpring.active(0.001f, 0.006f) && !dockDragging && !dockOrderDragging) {
                position();
            } else {
                if ((winState || kbChanged) && anyDockVisible()) paintBar();
                if (chromeChanged) positionSheet();
                else if (winState && panel && !sheetClosing && (page == OpenWindows || page == Capture || page == Overflow))
                    paintSheet();
                else if (kbChanged && panel && !sheetClosing && page == KeyboardLang)
                    paintSheet();
            }
            if (panel && page == ChromeWindows && !sheetClosing) updateChromePreviews();
        }
        if (isOwner && wp == WarmupTick) {
            KillTimer(hwnd, WarmupTick);
            prewarmTrayCaches();
            if (const auto stop = extra.proStop) {
                std::thread([stop] {
                    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
                    if (stop->load()) return;
                    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE) {
                        prewarmJumpListIndex();
                        if (SUCCEEDED(comHr)) CoUninitialize();
                    }
                }).detach();
            }
        }
        if (isOwner && wp == SessionTimerTick) {
            const bool changed = updateTimers(GetTickCount64());
            const auto visible = [&](int id) { return shown(id) && !isFoldedWidget(id); };
            const bool active = focusTimer.running() || countdownTimer.running() || extra.watchStart;
            if (anyDockVisible() && (changed || (motionEnabled() &&
                ((focusTimer.running() && visible(Focus)) || (countdownTimer.running() && visible(Countdown)) ||
                    (extra.watchStart && visible(Stopwatch)))))) paintBar();
            if ((page == Focus || page == Countdown || page == Stopwatch || page == NotificationCenter) &&
                (changed || (motionEnabled() && active))) paintSheet();
            if (!active) KillTimer(hwnd, SessionTimerTick);
        }
        if (isOwner && wp == 1) tick();
        if (isOwner && wp == 3) {
            updateVisibility();
            const auto stamp = GetTickCount64();
            if (anyDockVisible() && extra.media.playing && extra.media.durationTicks > 0 &&
                stamp >= extra.mediaProgressPaintAt &&
                ((shown(MediaMini) && !isFoldedWidget(MediaMini)) || (shown(MediaBar) && !isFoldedWidget(MediaBar)))) {
                extra.mediaProgressPaintAt = stamp + 250;
                paintBar();
            }
            if (page == AppMenu && panel && !sheetClosing && !modal && GetForegroundWindow() != panel &&
                ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000)) {
                POINT p{};
                RECT popup{}, dock{};
                if (GetCursorPos(&p) && GetWindowRect(panel, &popup) && GetWindowRect(bar.hwnd, &dock) &&
                    !PtInRect(&popup, p) && !PtInRect(&dock, p)) close();
            }
        }
        if (isOwner && wp == 8) updateNativeFlyout();
        if (isOwner && wp == 10) updateTraySync();
        if (isOwner && wp == 11) finishCapture();
        if (isOwner && wp == 12) pollDeferredTrayUncloak();
        if (isOwner && wp == AnimationTick && animTimerActive && !animationTimer) animateFrame();
        if (isOwner && wp == 4) {
            if (!dockEditMode) { KillTimer(hwnd, 4); return 0; }
            if (!panel && !modal && !dockOrderSource &&
                (((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0)) {
                POINT pt{}; RECT rc{};
                if (GetCursorPos(&pt) && GetWindowRect(bar.hwnd, &rc) && !PtInRect(&rc, pt)) {
                    dockEditMode = false; dockEditSpring.snap(0.f); bar.hover = -1; bar.key = -1;
                    KillTimer(hwnd, 4); position();
                }
            }
        }
        return 0;
    case DockReservationChanged:
        if (isOwner && model.visibility == DockVisibility::Reserved) position();
        return 0;
    case WM_DWMCOMPOSITIONCHANGED:
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE: if (isOwner) { lastFullscreenScan = 0; refreshMonitors(); refreshDisplayTiming(); position(); } return 0;
    case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(w, &ps); EndPaint(w, &ps); return 0; }
    case TrayMessage:
        if (lp == WM_RBUTTONUP) menu();
        else if (lp == WM_LBUTTONUP) open(Studio);
        return 0;
    case ShowStudio: open(Studio); return 0;
    case AsyncWake: if (isOwner) tick(false); return 0;
    case StartupDeferred: if (isOwner) completeDeferredStartup(); return 0;
    case LaunchFailedMsg: if (isOwner) report(L"Uygulama veya dosya açılamadı."); return 0;
    case WM_CLOSE: if (isOwner) DestroyWindow(w); else if (!isBar) close(); return 0;
    case WM_DESTROY:
        if (isOwner) {
            reservations.clear();
            RevokeDragDrop(hwnd);
            destroyMonitorDocks();
            if (extra.proStop) extra.proStop->store(true);
            if (extra.usageDirty) model.save();
            stopAnimTimer();
            if (animationTimer) { CloseHandle(animationTimer); animationTimer = nullptr; }
            stopWindowHook();
            KillTimer(hwnd, WindowSyncTick);
            KillTimer(hwnd, WarmupTick);
            stopTraySync(true); KillTimer(hwnd, 10); KillTimer(hwnd, 12); closeSheet(); shellTaskbar.restore(); KillTimer(hwnd, 8); tray(false); KillTimer(hwnd, 1); KillTimer(hwnd, 3); KillTimer(hwnd, 4); releaseAudioEndpoint(); PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command, int) {
    if (ShellTaskbar::runRestoreMode(command)) return 0;
    wchar_t deskName[128]{L"Default"};
    if (HDESK desk = GetThreadDesktop(GetCurrentThreadId()))
        GetUserObjectInformationW(desk, UOI_NAME, deskName, sizeof(deskName), nullptr);
    const Text mutexName = Text(L"Local\\Vetra.v1.") + deskName;
    for (const wchar_t* cls : {DockClass, L"PrismDock.Window"}) {
        if (HWND existing = FindWindowW(cls, nullptr)) {
            DWORD oldPid = 0;
            GetWindowThreadProcessId(existing, &oldPid);
            HANDLE oldProc = oldPid ? OpenProcess(SYNCHRONIZE, FALSE, oldPid) : nullptr;
            DWORD_PTR dummy = 0;
            SendMessageTimeoutW(existing, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG, 1000, &dummy);
            if (oldProc) {
                WaitForSingleObject(oldProc, 600);
                CloseHandle(oldProc);
            } else {
                for (int i = 0; i < 20 && FindWindowW(cls, nullptr); ++i) Sleep(15);
            }
        }
    }
    HANDLE single = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!single) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (auto window = FindWindowW(DockClass, nullptr)) PostMessageW(window, ShowStudio, 0, 0);
        CloseHandle(single); return 0;
    }
    int result = 0;
    const auto apartment = OleInitialize(nullptr);
    {
        Dock dock(instance);
        if (dock.initialize()) {
            if (wcsstr(command, L"--studio")) dock.open(Studio);
            MSG msg{};
            while (dock.nextMessage(msg) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        } else { MessageBoxW(nullptr, L"Vetra başlatılamadı.", L"Vetra", MB_ICONERROR); result = 1; }
    }
    Draw::shutdown();
    if (SUCCEEDED(apartment)) OleUninitialize();
    CloseHandle(single);
    return result;
}

