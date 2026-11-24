#include "dock.h"

namespace {
int maxMinutes(int id) {
    return id == Focus ? FocusMaxMinutes : CountdownMaxMinutes;
}

int displaySeconds(ULONGLONG milliseconds) {
    return static_cast<int>((milliseconds + 999) / 1000);
}

constexpr double RimTransitionMs = 285.0;
size_t rimIndex(int id) { return id == Focus ? 0 : id == Countdown ? 1 : 2; }
}

bool SessionTimer::update(ULONGLONG stamp) {
    if (!running()) return false;
    leftMs = stamp < deadline ? deadline - stamp : 0;
    const int previous = seconds;
    seconds = displaySeconds(leftMs);
    if (leftMs == 0) {
        status = Status::Finished;
        deadline = 0;
        return true;
    }
    return seconds != previous;
}

void SessionTimer::reset(int minutes) {
    status = Status::Ready;
    durationMs = leftMs = static_cast<ULONGLONG>(minutes) * 60000;
    seconds = minutes * 60;
    deadline = 0;
}

bool Dock::updateTimers(ULONGLONG stamp) {
    const bool focusChanged = focusTimer.update(stamp);
    const bool countdownChanged = countdownTimer.update(stamp);
    const bool focusFinished = focusChanged && focusTimer.status == SessionTimer::Status::Finished;
    const bool countdownFinished = countdownChanged && countdownTimer.status == SessionTimer::Status::Finished;
    if (focusFinished && !focusBreak) {
        ++sessions;
        const auto local = now();
        if (model.usageDay != Date::from(local).key()) {
            model.rollUsageDay(Date::from(local));
            extra.usageTotal = 0;
            extra.lastUsage = 0;
        }
        ++model.timelineFocus[local.wHour];
        extra.usageDirty = true;
    }
    if (focusFinished || countdownFinished) MessageBeep(MB_ICONASTERISK);
    return focusChanged || countdownChanged;
}

void Dock::toggleTimer(int id) {
    auto& timer = timerFor(id);
    const bool wasRunning = timer.running();
    const auto stamp = GetTickCount64();
    updateTimers(stamp);
    if (!wasRunning && id == Focus && timer.status == SessionTimer::Status::Finished) skipFocus(false);
    const float rim = timerRim(id, stamp);
    if (timer.running()) {
        timer.status = SessionTimer::Status::Paused;
        timer.deadline = 0;
    } else if (!wasRunning) {
        if (timer.status != SessionTimer::Status::Paused) timer.reset(sessionMinutes(id));
        timer.deadline = stamp + timer.leftMs;
        timer.status = SessionTimer::Status::Running;
        SetTimer(hwnd, SessionTimerTick, extra.watchStart ? 40 : 80, nullptr);
        if (rim < .999f && timer.leftMs == timer.durationMs) animateTimerRim(id, rim);
    }
    paintBar();
    paintSheet();
}

void Dock::resetTimer(int id) {
    const auto stamp = GetTickCount64();
    const float rim = timerRim(id, stamp);
    updateTimers(stamp);
    if (page == id) {
        timerInput.reset();
        invalidTimerInput = false;
    }
    timerFor(id).reset(sessionMinutes(id));
    animateTimerRim(id, rim);
    paintBar();
    paintSheet();
}

void Dock::setTimerMinutes(int id, int minutes) {
    auto& configured = minutesFor(id);
    const bool save = minutes != configured;
    configured = minutes;
    updateTimers(GetTickCount64());
    auto& timer = timerFor(id);
    if (!timer.active() && !(id == Focus && focusBreak)) {
        const auto rim = timerRim(id, GetTickCount64());
        timer.reset(minutes);
        animateTimerRim(id, rim);
    }
    timerInput.reset();
    invalidTimerInput = false;
    sheet.key = -1;
    if (save) changed();
    else {
        paintBar();
        paintSheet();
    }
}

int Dock::sessionMinutes(int id) const {
    if (id == Countdown) return model.countdownMinutes;
    return focusBreak ? (focusRound == 4 ? 15 : 5) : model.focusMinutes;
}

void Dock::skipFocus(bool keepRunning) {
    const auto stamp = GetTickCount64();
    const float rim = timerRim(Focus, stamp);
    const bool wasRunning = keepRunning && focusTimer.running();
    updateTimers(stamp);
    if (focusBreak) focusRound = focusRound % 4 + 1;
    focusBreak = !focusBreak;
    focusTimer.reset(sessionMinutes(Focus));
    if (wasRunning) {
        focusTimer.deadline = stamp + focusTimer.leftMs;
        focusTimer.status = SessionTimer::Status::Running;
        SetTimer(hwnd, SessionTimerTick, extra.watchStart ? 40 : 80, nullptr);
    }
    animateTimerRim(Focus, rim);
    paintBar();
    paintSheet();
}

void Dock::toggleStopwatch() {
    const auto stamp = GetTickCount64();
    if (extra.watchStart) {
        extra.watchAccum = extra.elapsed(stamp);
        extra.watchStart = 0;
        if (focusTimer.running() || countdownTimer.running()) SetTimer(hwnd, SessionTimerTick, 80, nullptr);
    } else {
        extra.watchStart = stamp;
        SetTimer(hwnd, SessionTimerTick, 40, nullptr);
    }
    paintBar();
    paintSheet();
}

void Dock::lapStopwatch() {
    if (!extra.watchStart) return;
    if (extra.laps.size() >= 120) { report(L"En fazla 120 tur kaydedilebilir."); return; }
    extra.laps.push_back(extra.elapsed(GetTickCount64()));
    listOffset = 0;
    paintBar();
    paintSheet();
}

void Dock::resetStopwatch() {
    const float rim = timerRim(Stopwatch, GetTickCount64());
    extra.watchStart = extra.watchAccum = 0;
    extra.laps.clear();
    animateTimerRim(Stopwatch, rim);
    paintBar();
    paintSheet();
}

float Dock::timerRim(int id, ULONGLONG stamp) const {
    const auto& motion = timerRims[rimIndex(id)];
    const float target = id == Stopwatch ? static_cast<float>(extra.elapsed(stamp) % 60000) / 60000.f
        : (id == Focus ? focusTimer : countdownTimer).remaining(stamp);
    if (!motionEnabled() || motion.start <= 0.0) return target;
    const float progress = easeOutCubic(static_cast<float>((animNowMs() - motion.start) / RimTransitionMs));
    return std::lerp(motion.from, target, progress);
}

void Dock::animateTimerRim(int id, float previous) {
    auto& motion = timerRims[rimIndex(id)];
    motion = {};
    if (!motionEnabled() || std::abs(previous - timerRim(id, GetTickCount64())) < .004f) return;
    motion = {previous, animNowMs()};
    startAnimTimer();
}

bool Dock::timerRimMoving(double nowMs) {
    bool redraw = false;
    for (auto& rim : timerRims) if (rim.start > 0.0) {
        redraw = true;
        if (!motionEnabled() || nowMs - rim.start >= RimTransitionMs) rim = {};
    }
    return redraw;
}

bool Dock::applyTimerInput() {
    if (!timerInput || (page != Focus && page != Countdown)) return true;
    int minutes = 0;
    for (const auto ch : *timerInput) minutes = minutes * 10 + (ch - L'0');
    if (minutes < 1 || minutes > maxMinutes(page)) {
        invalidTimerInput = true;
        paintSheet();
        return false;
    }
    setTimerMinutes(page, minutes);
    return true;
}

void Dock::timerPanel(Palette p) {
    auto& d = sheet.d;
    const int id = page;
    const bool focus = id == Focus;
    const auto& timer = timerFor(id);
    const bool running = timer.running();
    const bool paused = timer.status == SessionTimer::Status::Paused;
    const bool finished = timer.status == SessionTimer::Status::Finished;
    const auto color = focus && focusBreak ? 0x37BD80u : Widgets[id].color;
    const auto duration = minutesFor(id);
    const int maximum = maxMinutes(id);
    heading(p, focus ? (focusBreak ? L"Mola" : L"Odak") : L"Sayaç",
        focus ? L"Pomodoro · " + num(focusRound) + L"/4 · " + num(sessions) + L" tamamlanan odak" : L"Geri sayım");

    const auto clock = box(27, 94, 384, 118);
    p.card(d, clock, 16);
    if (running) d.fill(clock, 16, tint(color, .05f));
    d.rimProgress(box(28.5f, 95.5f, 381.f, 115.f), 14.5f, timerRim(id, GetTickCount64()), tint(color), 2.25f);
    const auto status = finished ? L"Süre doldu" : paused ? L"Duraklatıldı" :
        running ? (focus ? (focusBreak ? L"Mola sürüyor" : L"Odaklanıyor") : L"Geri sayıyor") : L"Hazır";
    d.text(status, box(49, 106, 340, 21), TileLabel, tint(focus && (running || finished) ? color : p.muted), Center);
    d.text(durationText(timer.seconds), box(42, 127, 354, 60), Display, tint(p.ink), Center);
    d.text(timer.active() ? (focus ? L"Sonraki odak süresi" : L"Sonraki sayaç süresi") : L"Süre",
        box(27, 224, 280, 22), Strong, tint(p.ink));
    const std::array<int, 4> presets = focus ? std::array{15, 25, 45, 60} : std::array{1, 5, 10, 30};
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const auto minutes = presets[i];
        button(sheet, p, box(27 + 98.f * i, 252, 90, 30), num(minutes) + L" dk",
            [this, id, minutes] { setTimerMinutes(id, minutes); }, !timerInput && duration == minutes, TileLabel);
    }
    button(sheet, p, box(27, 291, 39, 34), L"−", [this, id, duration] {
        setTimerMinutes(id, std::max(1, duration - 1));
    }, false, Strong);
    const auto input = box(73, 291, 138, 34);
    hit(sheet, input, [this, id] {
        if (!timerInput) timerInput = num(minutesFor(id));
        sheet.key = -1;
        selectTimerInput = true;
        invalidTimerInput = false;
        paintSheet();
    });
    const bool editing = timerInput.has_value();
    if (p.glass) {
        p.card(d, input, 11);
        if (editing || sheet.hoverAmount() > 0)
            d.fill(input, 11, tint(p.light ? 0x10141A : 0xFFFFFF, editing ? .07f : .04f * sheet.hoverAmount()));
        if (invalidTimerInput) d.outline(input, 11, tint(Design::focusRed(p.light), .85f), 1.2f / d.scale);
        else if (editing) d.glassRim(input, 11, .50f);
    } else {
        d.fill(input, 11, tint(p.ink, editing ? .13f : .07f + .045f * sheet.hoverAmount()));
        d.outline(input, 11, tint(invalidTimerInput ? Design::focusRed(p.light) : p.line, editing ? .82f : .28f),
            editing ? 1.25f : 1.f);
    }
    const Text value = editing ? *timerInput : num(duration);
    if (editing && selectTimerInput && !value.empty())
        d.fill(box(84, 297, static_cast<float>(value.size()) * 8 + 8, 22), 5, tint(p.ink, .16f));
    d.text(value.empty() ? L"—" : value, box(88, 291, 65, 34), Strong, tint(p.ink));
    d.text(L"dk", box(164, 291, 32, 34), Caption, tint(p.muted), Right);
    button(sheet, p, box(218, 291, 39, 34), L"+", [this, id, duration, maximum] {
        setTimerMinutes(id, std::min(maximum, duration + 1));
    }, false, Strong);
    if (editing) {
        button(sheet, p, box(267, 291, 144, 34), L"Uygula", [this] { applyTimerInput(); }, true, TileLabel);
        const auto hint = invalidTimerInput ? L"1–" + num(maximum) + L" dakika arasında bir süre gir." :
            L"Enter uygular · Esc vazgeçer";
        d.text(hint, box(27, 329, 384, 18), Caption,
            tint(invalidTimerInput ? Design::focusRed(p.light) : p.muted));
    }

    const auto action = running ? L"Duraklat" : paused ? L"Sürdür" : finished ? L"Yeniden başlat" : L"Başlat";
    button(sheet, p, box(27, 355, 226, 39), action, [this, id] {
        if (timerFor(id).active() || applyTimerInput()) toggleTimer(id);
    }, true, Strong);
    button(sheet, p, box(263, 355, 148, 39), L"Sıfırla", [this, id] { resetTimer(id); }, false, TileLabel);
    if (!notice.empty())
        d.text(notice, box(27, 400, 384, 16), Caption, tint(p.muted), Center);
}
