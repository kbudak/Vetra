#include "dock.h"
#include <cmath>
#include <numeric>

namespace {
Text percentage(int n) { return n < 0 ? L"—" : num(n) + L"%"; }
Box inset(Box b, float padding) { return {b.left + padding, b.top + padding, b.right - padding, b.bottom - padding}; }

constexpr unsigned char SegMasks[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F  // 9
};

void drawSevenSegmentDigit(Draw& d, float dx, float dy, float dw, float dh,
                           int curDigit, int prevDigit, float animT, unsigned rgb, bool light) {
    const unsigned char curMask = SegMasks[std::clamp(curDigit, 0, 9)];
    const unsigned char prevMask = SegMasks[std::clamp(prevDigit < 0 ? curDigit : prevDigit, 0, 9)];
    const float t = 2.85f, g = 1.55f, r = 1.42f;
    const float halfH = (dh - t) / 2.f;
    const Box segs[7] = {
        box(dx + t + g, dy, dw - 2.f * (t + g), t),                              // a: top
        box(dx + dw - t, dy + t * .55f + g, t, halfH - t * .55f - 2.f * g),       // b: top-right
        box(dx + dw - t, dy + halfH + t * .45f + g, t, halfH - t * .55f - 2.f * g), // c: bot-right
        box(dx + t + g, dy + dh - t, dw - 2.f * (t + g), t),                     // d: bottom
        box(dx, dy + halfH + t * .45f + g, t, halfH - t * .55f - 2.f * g),        // e: bot-left
        box(dx, dy + t * .55f + g, t, halfH - t * .55f - 2.f * g),               // f: top-left
        box(dx + t + g, dy + halfH, dw - 2.f * (t + g), t)                       // g: middle
    };
    const float offAlpha = light ? .11f : .14f;
    const float onAlpha = .98f;
    for (int s = 0; s < 7; ++s) {
        const float from = ((prevMask >> s) & 1) ? onAlpha : offAlpha;
        const float to = ((curMask >> s) & 1) ? onAlpha : offAlpha;
        const float alpha = from + (to - from) * animT;
        d.fill(segs[s], r, tint(rgb, alpha));
    }
}

void drawSplitFlapCard(Draw& d, float cx, float cy, float cw, float ch,
                       int curDigit, int prevDigit, float animT, bool light) {
    const unsigned topBg = light ? 0xEAEAEF : 0x2F2F34;
    const unsigned botBg = light ? 0xDDDDE4 : 0x232327;
    const unsigned digitRgb = light ? 0x151518 : 0xFAFAFC;
    const unsigned hingeRgb = light ? 0xBEBECA : 0x111114;
    const Box full = box(cx, cy, cw, ch);
    const Box topHalf = box(cx, cy, cw, ch / 2.f);
    const Box botHalf = box(cx, cy + ch / 2.f, cw, ch / 2.f);
    const Box textBox = box(cx, cy - 1.f, cw, ch);

    d.fill(box(cx, cy + 1.f, cw, ch), 5.5f, tint(0x000000, light ? .10f : .32f));

    auto drawHalf = [&](bool top, int digit, float shade) {
        d.clip(top ? topHalf : botHalf);
        d.fill(full, 5.5f, tint(top ? topBg : botBg));
        d.text(num(std::clamp(digit, 0, 9)), textBox, ClockBig, tint(digitRgb), Center);
        if (shade > 0.f) d.fill(full, 5.5f, tint(0x000000, shade));
        else if (shade < 0.f) d.fill(full, 5.5f, tint(0xFFFFFF, -shade));
        d.unclip();
    };

    if (animT >= 1.f || curDigit == prevDigit || prevDigit < 0) {
        drawHalf(true, curDigit, 0.f);
        drawHalf(false, curDigit, 0.f);
    } else {
        // Static halves behind the moving flap:
        drawHalf(true, curDigit, (1.f - animT) * .18f);
        drawHalf(false, prevDigit, animT * .22f);

        const float angle = animT * 3.14159265f;
        const float cosA = std::cos(angle);
        const float scaleY = std::max(.05f, std::abs(cosA));
        D2D1::Matrix3x2F prevTransform{};
        d.target->GetTransform(&prevTransform);
        const D2D1_POINT_2F pivot{cx + cw / 2.f, cy + ch / 2.f};
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(1.f, scaleY, pivot) * prevTransform);
        if (cosA >= 0.f) {
            drawHalf(true, prevDigit, (1.f - cosA) * .28f);
        } else {
            drawHalf(false, curDigit, (1.f + cosA) * .18f);
        }
        d.target->SetTransform(prevTransform);
    }

    d.gradientOutline(inset(full, .45f), 5.f, tint(0xFFFFFF, light ? .65f : .15f), tint(0xFFFFFF, .03f), .8f);
    d.fill(box(cx, cy + ch / 2.f - .6f, cw, 1.2f), 0.f, tint(hingeRgb, .92f));
    d.fill(box(cx, cy + ch / 2.f - 2.f, 1.8f, 4.f), .6f, tint(hingeRgb));
    d.fill(box(cx + cw - 1.8f, cy + ch / 2.f - 2.f, 1.8f, 4.f), .6f, tint(hingeRgb));
}
}

float Dock::widgetWidth(int id, bool preview) const {
    if (id == Apps) {
        const size_t pinned = preview ? std::min<size_t>(5, pinnedOnBar()) : pinnedOnBar();
        const size_t running = preview ? 0 : runningOnBar().size();
        const size_t total = preview ? std::max<size_t>(1, pinned) : pinned + running;
        if (!total) return 0.f;
        const float divider = (pinned > 0 && running > 0) ? 8.f : 0.f;
        const float pitch = !preview && glassTheme(model.theme) ? Design::GlassAppPitch : Design::AppPitch;
        return 2 * Design::Pad + pitch * total + divider;
    }
    if (preview && id == Quick) return 128;
    return static_cast<float>(Widgets[id].width);
}

void Dock::widgetCard(View& view, int id, Box b, Palette p, bool preview) {
    auto& d = view.d;
    b = d.snap(b);
    const float w = width(b);
    auto at = [&](float x, float y, float width, float height) { return box(b.left + x, b.top + y, width, height); };
    const auto accent = Widgets[id].color;
    const auto time = preview ? catalogTime : now();
    const Date day = Date::from(time);
    const bool editing = !preview && dockEditMode;
    if (!preview) hit(view, b, [this, id, editing, middle = b.left + w / 2] {
        if (editing) return;
        else if (id == Apps) open(Apps, middle);
        else if (id == Volume) return;
        else if (id == QuickToggles) open(Quick, middle);
        else if (mediaWidget(id)) open(Media, middle);
        else if (weatherWidget(id)) open(Weather, middle);
        else if (id == ClockWeather) open(Weather, middle);
        else if (id == DesktopToggle) toggleDesktop();
        else if (id == StartButton) startMenu();
        else if (id == CpuMeter || id == RamMeter) open(System, middle);
        else if (id == AnalogClock) open(Clock, middle);
        else if (id == SevenDays || id == TodayWidget || id == DateCard || id == DayCard ||
                 id == DayProgress || id == MonthProgress || id == YearProgress) open(Calendar, middle);
        else if (id == SegmentClock || id == FlipClock) {
            if (id == FlipClock && motionEnabled()) {
                prevFlipDigits = {
                    (curFlipDigits[0] + 9) % 10, (curFlipDigits[1] + 9) % 10,
                    (curFlipDigits[2] + 9) % 10, (curFlipDigits[3] + 9) % 10
                };
                flipClockAtMs = animNowMs();
                startAnimTimer();
            }
            open(Clock, middle);
        }
        else open(id, middle);
    });
    if (!preview) view.hits.back().order = {OrderKind::Widget, id};
    const float hover = preview ? 0.f : view.hoverAmount();
    const float press = (!preview && !editing && id != Volume && id != Apps && id != Capture) ? view.pressAmount() : 0.f;
    D2D1::Matrix3x2F pressPrev{};
    if (press > 0.005f) {
        d.target->GetTransform(&pressPrev);
        const float s = 1.f - 0.018f * press;
        const D2D1_POINT_2F piv{b.left + w / 2.f, b.top + Design::CardHeight / 2.f};
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, piv) * pressPrev);
    }
    const bool opened = !preview && !sheetClosing && (page == id ||
        ((id == SevenDays || id == TodayWidget || id == DateCard || id == DayCard) && page == Calendar) ||
        ((id == SegmentClock || id == FlipClock) && page == Clock));
    const bool timer = id == Focus || id == Countdown || id == Stopwatch;
    const bool productivityCard = timer || id == Usage || id == Tasks || id == Notes ||
        id == Calendar || id == DayTimeline || id == WhatsApp || id == Currency;
    const bool audioCard = mediaWidget(id) || id == MicControl || id == Volume || id == QuickToggles;
    if ((productivityCard || audioCard) && !p.light) {
        d.fill(b, Design::Radius, tint(0x1C1C1E, .99f));
        d.outline(inset(b, .5f), Design::Radius - .5f, tint(0x363638), 1.f);
        if (hover > 0.f) d.fill(b, Design::Radius, tint(0xFFFFFF, .035f * hover));
    } else if (preview) d.fill(b, 12, tint(p.high));
    else if (id == Apps && p.theme == BareTheme) p.card(d, b, Design::Radius);
    else if (id != Apps) {
        const float visible = opened ? 1.f : hover;
        const bool liveTile = p.glass && !preview;
        if (liveTile) {
            if (p.theme == LiquidGlass) {
                const float tone = p.opacity / 100.f;
                d.fill(b, Design::Radius, tint(0x08090B, .22f + tone * .26f));
                if (visible > 0) d.fill(b, Design::Radius, tint(0xFFFFFF, .048f * visible));
                d.glassRim(b, Design::Radius, .28f + .16f * visible);
            } else {
                d.gradient(b, Design::Radius, tint(p.high, .50f), tint(p.base, .44f));
                if (visible > 0) d.fill(b, Design::Radius, tint(p.ink, .05f * visible));
                d.glassRim(b, Design::Radius, .34f + .14f * visible);
            }
        }
        else p.card(d, b, Design::Radius);
        if (!liveTile && visible > 0) d.fill(b, Design::Radius, tint(p.ink, .075f * visible));
    }
    auto text = [&](const Text& value, float x, float y, float available, float h, Font font, unsigned color, Alignment align = Left) {
        d.text(value, at(x, y, available, h), font, tint(color), align);
    };
    auto metric = [&](const Text& value, float x, float y, float available, Alignment align = Left) {
        text(value, x, y, available, 26, d.fit(value, available, Value, Strong), p.ink, align);
    };
    auto icon = [&](Glyph glyph, unsigned color, float x = Design::Pad) {
        auto square = at(x, 10, Design::Icon, Design::Icon);
        d.fill(square, 9, tint(color, p.light ? .15f : .18f));
        d.icon(glyph, inset(square, 8), tint(color));
    };
    auto mini = [&](float right, Glyph glyph, std::function<void()> action, float size = 30.f, unsigned color = 0,
                    bool solid = false) {
        auto button = at(w - right - size, (Design::CardHeight - size) / 2, size, size);
        float hot = 0.f, down = 0.f;
        if (!preview && !editing) {
            hit(view, button, std::move(action));
            hot = view.hoverAmount();
            down = view.pressAmount();
        }
        D2D1::Matrix3x2F mp{};
        if (down > 0.005f || hot > 0.005f) {
            d.target->GetTransform(&mp);
            const float s = 1.f + 0.05f * hot - 0.10f * down;
            const D2D1_POINT_2F piv{button.left + size / 2.f, button.top + size / 2.f};
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, piv) * mp);
        }
        if (color && solid) {
            d.circle(button.left + size / 2, button.top + size / 2, size / 2,
                tint(mixRgb(color, 0xFFFFFF, .15f * hot)));
            d.icon(glyph, inset(button, size * .30f), tint(0xFFFFFF));
        } else if (color) {
            d.fill(button, size / 2, tint(color, .20f + .14f * hot));
            d.icon(glyph, inset(button, size * .24f), tint(color));
        } else {
            d.circle(button.left + size / 2, button.top + size / 2, size / 2, tint(p.ink, .12f + .10f * hot));
            d.icon(glyph, inset(button, size * .24f), tint(p.ink));
        }
        if (down > 0.005f || hot > 0.005f) d.target->SetTransform(mp);
    };
    auto progress = [&](float x, float y, float available, float h, float portion, unsigned color) {
        d.fill(at(x, y, available, h), h / 2, tint(p.light ? 0xD8D8DC : 0x383838));
        if (portion > 0) d.fill(at(x, y, available * std::min(1.f, portion), h), h / 2, tint(color));
    };
    auto headline = [&](Glyph glyph, const Text& value, const Text& subtitle) {
        icon(glyph, accent);
        metric(value, 55, 5, w - 64);
        text(subtitle, 55, 32, w - 64, 17, Caption, p.muted);
    };

    if (timer) {
        const auto stamp = GetTickCount64();
        Text value, label = Widgets[id].title;
        const auto timerText = [](ULONGLONG seconds) {
            return seconds < 3600 ? num(seconds / 60) + L":" + two(static_cast<int>(seconds % 60))
                : durationText(seconds);
        };
        bool running = false;
        const bool onBreak = id == Focus && focusBreak && !preview;
        const unsigned timerColor = onBreak ? 0x37BD80u : accent;
        if (id == Stopwatch) {
            value = timerText(preview ? 0 : extra.elapsed(stamp) / 1000);
            running = !preview && extra.watchStart != 0;
        } else {
            const auto& session = timerFor(id);
            value = timerText(preview ? (id == Focus ? 25 * 60 : 60) : session.seconds);
            running = !preview && session.running();
            if (id == Focus) label = onBreak ? L"Mola" : L"Odak";
            if (!preview && session.status == SessionTimer::Status::Finished && id == Countdown) label = L"Süre doldu";
        }
        const float ring = preview ? (id == Stopwatch ? 0.f : 1.f) : timerRim(id, stamp);
        d.rimProgress(inset(b, 1.5f), Design::Radius - 1.5f, ring, tint(timerColor), 2.5f);
        text(label, 14, 6, w - 66, 17, TileLabel, timerColor);
        if (id == Focus) text(num(preview ? 1 : focusRound) + L"/4", 53, 6, 38, 17, Caption, p.muted);
        const bool stopwatchLap = id == Stopwatch && running;
        const float timeWidth = w - (id == Focus || stopwatchLap ? 91 : 57);
        text(value, 14, 21, timeWidth, 30, d.fit(value, timeWidth, ClockBig, Value), p.ink);
        if (id == Focus) mini(49, SkipSolid, [this] { skipFocus(); }, 26.f);
        if (stopwatchLap) mini(49, PinIcon, [this] { lapStopwatch(); }, 26.f);
        mini(9, running ? Pause : PlaySolid, [this, id] {
            if (id == Stopwatch) toggleStopwatch();
            else toggleTimer(id);
        }, 34.f, timerColor, true);
    } else switch (id) {
    case Apps: {
        const size_t pinned = preview ? std::min<size_t>(5, pinnedOnBar()) : pinnedOnBar();
        const auto running = preview ? std::vector<size_t>{} : runningOnBar();
        const float pitch = p.glass && !preview ? Design::GlassAppPitch : Design::AppPitch;
        float x = Design::Pad;
        const auto slotAt = [&](float left) {
            return at(left - (pitch - Design::AppSize) / 2, editing ? 8.f : 0.f,
                pitch, editing ? Design::CardHeight - 8.f : Design::CardHeight);
        };
        const auto appGlyph = [&](float left, float hover) {
            auto aligned = d.snap(at(left, 10, Design::AppSize, Design::AppSize));
            const float rise = std::round(2 * hover * d.scale) / d.scale;
            aligned.top -= rise;
            aligned.bottom -= rise;
            return aligned;
        };
        if (preview && !pinned) d.icon(Layers, at((w - 23) / 2, 16, 23, 23), tint(p.ink));
        if (editing) d.fill(at((w - 27) / 2, 2, 27, 3), 1.5f, tint(p.muted, .75f));
        for (size_t i = 0; i < pinned; ++i) {
            const auto& app = model.shortcuts[i];
            HWND target{};
            bool active = false;
            if (!preview) for (const auto& window : extra.windows) {
                if (pinnedIndex(window.app) != static_cast<int>(i)) continue;
                if (!target || window.active) target = window.handle;
                active |= window.active;
            }
            const auto slot = slotAt(x);
            const bool chrome = target && isChromeApp(app);
            if (!preview) hit(view, slot, [this, app, target, editing, chrome, mid = b.left + x + 16] {
                if (editing) return;
                if (chrome) showChromeWindows(mid);
                else if (target && IsWindow(target)) activateWindow(target, true);
                else launchShortcut(app);
            }, [this, app, target, image = appImages[i], mid = b.left + x + 16] { showAppMenu(app, mid, target, image); });
            if (!preview && !editing) view.hits.back().chromePreview = chrome;
            if (!preview) view.hits.back().order = {OrderKind::Shortcut, static_cast<int>(i)};
            if (!preview && page == AppMenu && panelDockX >= 0 && app.target == contextApp.target)
                panelDockX = b.left + x + 16;
            const float hot = !preview && !editing ? view.hoverAmount() : 0;
            const auto glyph = appGlyph(x, hot);
            if (appImages[i]) d.image(appImages[i], glyph, p.light);
            else {
                d.icon(app.target.find(L"://") != Text::npos ? Globe : Link, inset(glyph, 6), tint(p.ink));
            }
            if (target) d.fill(at(x + (Design::AppSize - (active ? 14 : 5)) / 2, 45, active ? 14 : 5, 3), 1.5f,
                tint(active ? p.accent : p.muted));
            x += pitch;
        }
        if (pinned > 0 && !running.empty()) {
            const float divX = b.left + x - (pitch - Design::AppSize) / 2.f + .5f;
            d.line(divX, b.top + 13, divX, b.bottom - 13, tint(p.ink, .22f), 1.f);
            x += 8.f;
        }
        for (size_t i : running) {
            const auto& win = extra.windows[i];
            const auto slot = slotAt(x);
            const bool chrome = isChromeApp(win.app);
            hit(view, slot, [this, editing, chrome, target = win.handle, mid = b.left + x + 16] {
                if (editing) return;
                if (chrome) showChromeWindows(mid);
                else activateWindow(target, true);
            },
                [this, target = win.handle, mid = b.left + x + 16] { showWindowMenu(target, mid); });
            if (!editing) view.hits.back().chromePreview = chrome;
            view.hits.back().order = {OrderKind::Window, -1, win.handle};
            if (&view == &bar && page == AppMenu && panelDockX >= 0 && contextWindow == win.handle) panelDockX = b.left + x + 16;
            const float hot = !editing ? view.hoverAmount() : 0;
            if (p.glass) d.glassRim(inset(slot, 2), 13, win.active ? .50f : .29f);
            const auto glyph = appGlyph(x, hot);
            if (win.icon) d.image(win.icon, glyph, p.light);
            else {
                d.icon(Layers, inset(glyph, 7), tint(p.ink));
            }
            const float indW = win.active ? 14.f : 5.f;
            const auto ind = at(x + (Design::AppSize - indW) / 2.f, 45.f, indW, 3.f);
            d.fill(ind, 1.5f, tint(win.active ? p.accent : p.muted, win.active ? 1.f : .80f));
            x += pitch;
        }
        break;
    }
    case Calendar: {
        const auto date = at(9, 10, 36, 36);
        d.fill(date, 10, tint(p.light ? 0xE7E7EA : 0x292929));
        text(preview ? L"SAL" : ShortDays[time.wDayOfWeek], 9, 11, 36, 13, Micro, 0xFF443D, Center);
        text(num(preview ? 20 : day.day), 9, 23, 36, 21, Value, p.ink, Center);
        const Event* next = nullptr;
        const Text minute = two(time.wHour) + L":" + two(time.wMinute);
        for (const auto& event : model.events) {
            if (event.day < day.key() || (event.day == day.key() && event.time < minute)) continue;
            if (!next || event.day < next->day || (event.day == next->day && event.time < next->time)) next = &event;
        }
        const unsigned colors[]{0x0F88F9, 0x8B6CF4, 0xA76FE8};
        const auto color = colors[next && !preview ? std::clamp(next->kind, 0, 2) : 0];
        d.fill(at(54, 13, 12, 12), 3, tint(color));
        d.icon(DateIcon, at(56, 15, 8, 8), tint(0xFFFFFF));
        text(preview ? L"Tasarım görüşmesi" : next ? next->title : L"Takvim", 72, 8, w - 82, 21, Strong, p.ink);
        Text subtitle = L"Yaklaşan etkinlik yok";
        if (preview) subtitle = L"16:05 · 23 dk sonra";
        else if (next && next->day == day.key() && next->time.size() >= 5) {
            const int minutes = (next->time[0] - L'0') * 600 + (next->time[1] - L'0') * 60 +
                (next->time[3] - L'0') * 10 + next->time[4] - L'0' - time.wHour * 60 - time.wMinute;
            const Text until = minutes <= 0 ? L"Şimdi" : minutes < 60 ? num(minutes) + L" dk sonra"
                : num(minutes / 60) + L" sa " + num(minutes % 60) + L" dk sonra";
            subtitle = next->time + L" · " + until;
        } else if (next) subtitle = Date::fromKey(next->day).label() + L" · " + next->time;
        text(subtitle, 54, 29, w - 64, 17, Caption, p.muted);
        break;
    }
    case Tasks: {
        int total = 0, remainingCount = 0;
        Text next;
        for (const auto& task : model.tasks) if (task.profile == model.profile) {
            ++total;
            if (!task.complete) { ++remainingCount; if (next.empty()) next = task.title; }
        }
        const auto circle = at(12, 13, 30, 30);
        const unsigned taskBlue = 0x0F88F9;
        d.arc(circle, -90, 360, tint(p.ink, .16f), 3);
        const float ratio = preview ? .72f : taskAnim.initialized ? std::clamp(taskAnim.value, 0.f, 1.f)
            : (total > 0 ? static_cast<float>(total - remainingCount) / total : 0.f);
        if (ratio > 0.005f) d.arc(circle, -90, 360.f * ratio, tint(taskBlue), 3);
        text(num(preview ? 1 : remainingCount), 12, 13, 30, 30, Caption, p.ink, Center);
        const auto check = at(51, 17, 22, 22);
        if (!preview && !editing && remainingCount) {
            hit(view, check, [this] {
                const auto task = std::find_if(model.tasks.begin(), model.tasks.end(),
                    [this](const Task& item) { return item.profile == model.profile && !item.complete; });
                if (task != model.tasks.end()) { task->complete = true; changed(false); }
            });
            view.hits.back().order = {OrderKind::Widget, id};
        }
        if (preview || remainingCount) d.arc(inset(check, 5), -90, 360, tint(p.muted), 1.5f);
        text(preview ? L"Sürüm notlarını hazırla" : next.empty() ? L"Görev ekle" : next,
            75, 14, w - 84, 28, Body, p.ink);
        break;
    }
    case Notes: {
        d.gradient(at(9, 9, 36, 38), 8, tint(0xFFFEF4), tint(0xE0DFD8));
        d.fill(at(10, 10, 34, 8), 4, tint(0xFFCB38));
        for (int i = 0; i < 3; ++i) d.line(b.left + 14, b.top + 25 + i * 5, b.left + 40, b.top + 25 + i * 5, tint(0xAAA698, .55f));
        const Text note = preview ? L"Toplantı notları\nTakvimi ve görevleri gözden geçir." : model.notes[model.profile];
        const auto split = note.find(L'\n');
        text(split == Text::npos ? L"Hızlı not" : note.substr(0, split), 54, 7, w - 93, 20, Strong, p.ink);
        text(note.empty() ? L"Not ekle" : split == Text::npos ? note : note.substr(split + 1),
            54, 26, w - 93, 21, Caption, p.muted);
        mini(8, Document, [this] { editNote(); }, 24.f);
        break;
    }
    case System:
        icon(Chip, accent);
        for (int i = 0; i < 2; ++i) {
            const int value = i ? ram : cpu;
            const float smoothVal = preview ? static_cast<float>(std::max(0, value))
                : i ? (ramAnim.initialized ? ramAnim.value : static_cast<float>(ram))
                    : (cpuAnim.initialized ? cpuAnim.value : static_cast<float>(std::max(0, cpu)));
            const unsigned barColor = i ? (p.light ? 0x6D4AFF : 0xA896FF) : (p.light ? 0x1A73E8 : 0x5FB2FF);
            text(i ? L"RAM" : L"CPU", 53, 7 + i * 19.f, 31, 19, Small, p.muted);
            progress(86, 15 + i * 19.f, w - 134, 3, std::clamp(smoothVal, 0.f, 100.f) / 100.f, barColor);
            text(percentage(value), w - 45, 7 + i * 19.f, 36, 19, Caption, p.ink, Right);
        }
        break;
    case Git: headline(Branch, branch.empty() ? L"Git" : branch, model.repo.empty() ? L"Depo seç" : leaf(model.repo)); break;
    case Sales: case Growth: {
        const auto total = model.total(id, day);
        text(id == Sales ? L"Net gelir" : L"Ziyaretçi", 11, 4, 70, 22, Caption, p.muted);
        text(id == Sales ? money(total[0]) : grouped(total[0]), 77, 3, w - 88, 24, Strong, p.ink, Right);
        const auto values = history(id);
        if (std::any_of(values.begin(), values.end(), [](auto n) { return n > 0; }))
            spark(d, at(12, 27, w - 24, 19), values, id == Sales ? Design::Green : Design::Amber, true);
        else text(L"Son 7 günde kayıt yok", 11, 29, w - 22, 18, Small, p.muted);
        break;
    }
    case Media: case MediaCompact: case MediaMini: case MediaBar: {
        const bool full = id == Media, miniPlayer = id == MediaMini, hasBar = id == MediaBar;
        const bool playing = preview ? full : extra.media.playing;
        const float position = preview ? (miniPlayer ? .31f : .30f) : extra.media.progress(GetTickCount64());
        const Box playBtn = miniPlayer ? at(8, 8, w - 16, 40) : at(w - (hasBar ? 63.f : 38.f), 14, 28, 28);
        const auto drawPlay = [&] {
            const float centerX = (playBtn.left + playBtn.right) / 2, centerY = (playBtn.top + playBtn.bottom) / 2;
            float hot = 0.f, down = 0.f;
            if (!preview && !editing) {
                hit(view, playBtn, [this] { media(VK_MEDIA_PLAY_PAUSE); });
                hot = view.hoverAmount();
                down = view.pressAmount();
            }
            if (miniPlayer) {
                const auto ring = box(centerX - 16, centerY - 16, 32, 32);
                d.arc(ring, -90, 360, tint(p.ink, .18f + .07f * hot), 3);
                if (position > 0.f) d.arc(ring, -90, position * 360.f, tint(p.ink, .95f), 3);
                if (hot > .01f) d.circle(centerX, centerY, 13, tint(p.ink, .06f * hot));
            } else d.circle(centerX, centerY, 14.f - down, tint(p.ink, .11f + .11f * hot));
            d.icon(playing ? Pause : PlaySolid, box(centerX - 7, centerY - 7, 14, 14), tint(p.ink, .95f));
        };
        if (miniPlayer) { drawPlay(); break; }
        const float artSide = full ? 38.f : 34.f;
        const Box art = at(9, (56.f - artSide) / 2, artSide, artSide);
        if (extra.media.cover && !preview) {
            d.clip(art);
            d.image(extra.media.cover, art, p.light);
            d.unclip();
            d.outline(art, 9.f, tint(p.ink, .12f), .8f);
        } else {
            if (full) {
                d.gradient(art, 8.f, tint(0xCC7049), tint(0x48285F), true);
                const float bars[] = {7.f, 13.f, 17.f, 11.f, 7.f};
                for (int i = 0; i < 5; ++i) {
                    const float bh = bars[i];
                    d.fill(box(art.left + 10.f + i * 4.f, art.top + (38.f - bh) / 2.f, 2.2f, bh), 1.1f, tint(0xFFFFFF, .92f));
                }
            } else {
                d.gradient(art, 8.f, tint(0x777783), tint(0x272730), true);
                d.circle(art.left + 17, art.top + 17, 9, tint(0xFFFFFF, .45f), false, .8f);
                d.circle(art.left + 17, art.top + 17, 1.2f, tint(0xFFFFFF, .75f));
            }
        }
        const Text titleTxt = preview ? (full ? L"Akşam Sürüşü" : L"Gece Işıkları") :
            extra.media.title.empty() ? L"Medya" : extra.media.title;
        const Text artistTxt = preview ? (full ? L"Kuzey Sahili" : L"Yankı") :
            extra.media.title.empty() ? L"Oynatılan parça yok" : extra.media.artist;
        const float left = full ? 55.f : 51.f, textW = playBtn.left - b.left - left - 7.f;
        text(titleTxt, left, hasBar ? 7.f : 10.f, textW, 18, full ? Strong : TileLabel, p.ink);
        text(artistTxt, left, hasBar ? 23.f : 29.f, textW, 14, Small, p.muted);
        if (hasBar) {
            const auto rail = at(left, 41, textW, 3);
            d.fill(rail, 1.5f, tint(p.ink, .18f));
            if (position > 0.f) d.fill(at(left, 41, textW * position, 3), 1.5f, tint(p.ink, .86f));
            const Box next = at(w - 31, 14, 26, 28);
            if (!preview && !editing) hit(view, next, [this] { media(VK_MEDIA_NEXT_TRACK); });
            d.icon(SkipSolid, at(w - 24, 22, 12, 12), tint(p.ink));
        }
        drawPlay();
        break;
    }
    case MicControl: {
        for (int i = 0; i < 2; ++i) {
            const bool muted = !preview && (i == 0 ? extra.microphoneMute == 1 : extra.muted);
            const bool ready = preview || (i == 0 ? extra.microphoneMute >= 0 : extra.volume >= 0);
            const auto button = at(9 + 43.f * i, 9, 38, 38);
            float hot = 0.f, down = 0.f;
            if (!preview && !editing && ready) {
                hit(view, button, [this, i] { if (i == 0) sampleMicrophone(true); else changeVolume(0, true); });
                hot = view.hoverAmount(); down = view.pressAmount();
            }
            const float x = button.left + 19.f, y = button.top + 19.f;
            const unsigned ink = muted ? Design::focusRed(p.light) : p.ink;
            d.circle(x, y, 19.f - down, tint(ink, (muted ? .18f : .10f) + .10f * hot));
            d.icon(i == 0 ? MicIcon : HeadphonesIcon, box(x - 10, y - 10, 20, 20), tint(ink, ready ? 1.f : .38f));
            if (muted) d.line(x - 8.f, y + 8.f, x + 8.f, y - 8.f, tint(ink), 1.55f);
        }
        break;
    }
    case Clock: {
        const Text hhmm = two(time.wHour) + L":" + two(time.wMinute);
        const Text secs = L":" + two(time.wSecond);
        text(hhmm, 12, 5, 74, 28, ClockBig, p.ink);
        text(secs, 81, 10, 48, 22, Strong, p.light ? 0x1A6ED8 : 0x6EB4FF);
        text(Text(ShortDays[time.wDayOfWeek]) + L" " + num(day.day) + L" " + ShortMonths[day.month - 1],
            13, 33, w - 22, 16, Caption, p.muted);
        break;
    }
    case World: {
        bool cityDaylight = true;
        const Text offset = worldOffsetText(&cityDaylight);
        const unsigned dotColor = cityDaylight ? 0xFFCC00 : 0x8C9EFF;
        d.circle(b.left + 17.5f, b.top + 18.5f, 5.2f + hover * 1.4f, tint(dotColor, .20f + .12f * hover));
        d.circle(b.left + 17.5f, b.top + 18.5f, 3.8f, tint(dotColor));
        text(Cities[model.worldCity].name, 26, 8, 76, 20, Strong, p.ink);
        text(offset, 12, 30, 92, 16, Small, p.muted);
        text(worldTime(), 92, 13, w - 104, 30, ClockBig, p.ink, Right);
        break;
    }
    case SevenDays: {
        text(Months[day.month - 1], 12, 6, 110, 15, Caption, p.ink);
        const Text temp = tempText(preview);
        d.icon(Sun, at(w - 44, 7.5f, 12, 12), tint(p.light ? 0xC97A00 : 0xFFCB47));
        text(temp, w - 30, 6, 20, 15, Caption, p.ink, Right);
        constexpr float colW = 26.5f, colGap = 1.8f, startX = 10.f;
        for (int i = 0; i < 7; ++i) {
            const Date dItem = day.add(i);
            const float cx = startX + i * (colW + colGap);
            if (i == 0) {
                const Box badge = at(cx, 22, 26, 28);
                d.fill(badge, 8.f, tint(p.light ? 0x0A7AFF : 0x2D8CFF));
            }
            text(ShortDays[dItem.weekday()], cx, 23, 26, 12, Micro, i == 0 ? 0xFFFFFF : p.muted, Center);
            text(num(dItem.day), cx, 35, 26, 14, Caption, i == 0 ? 0xFFFFFF : p.ink, Center);
        }
        break;
    }
    case TodayWidget: {
        text(LongDaysUpper[time.wDayOfWeek], 13, 8, 80, 15, Small, p.light ? 0xBE241D : 0xFF443D);
        text(Text(ShortMonths[day.month - 1]) + L" " + num(day.day), 13, 23, 80, 24, DateBig, p.ink);
        d.line(b.left + 95.5f, b.top + 11.f, b.left + 95.5f, b.bottom - 11.f, tint(p.ink, .13f), 1.f);
        text(two(time.wHour) + L":" + two(time.wMinute), 104, 8, w - 114, 21, Strong, p.ink);
        d.circle(b.left + 110.f, b.top + 38.f, 4.2f, tint(0xFFCC00));
        d.icon(Sun, at(103.5f, 31.5f, 13, 13), tint(0xFFCC00));
        text(tempText(preview), 120, 29, w - 130, 17, Caption, p.muted);
        break;
    }
    case DateCard: {
        text(LongDays[time.wDayOfWeek], 14, 8, w - 24, 17, Caption, p.muted);
        text(num(day.day) + L" " + Months[day.month - 1], 14, 24, w - 22, 24, DateBig, p.ink);
        break;
    }
    case DayCard: {
        text(Days[time.wDayOfWeek], 4, 8, w - 8, 13, Micro, p.light ? 0xD93829 : 0xFF5545, Center);
        text(num(day.day), 4, 20, w - 8, 30, ClockBig, p.ink, Center);
        break;
    }
    case SegmentClock: {
        const Text dayLabel = LongDaysUpper[time.wDayOfWeek];
        const float pillW = std::clamp(dayLabel.size() * 6.2f + 12.f, 42.f, 74.f);
        const Box pill = at(10, 6, pillW, 13.5f);
        d.fill(pill, 6.5f, tint(p.ink, .11f));
        text(dayLabel, 10, 6, pillW, 13.5f, Micro, p.ink, Center);
        text(num(day.day) + L" " + ShortMonthsUpper[day.month - 1], 16 + pillW, 5.5f, w - 24 - pillW, 14.5f, Caption, p.ink);

        const std::array<int, 4> digits = {
            time.wHour / 10, time.wHour % 10, time.wMinute / 10, time.wMinute % 10
        };
        if (!preview && curSegDigits[0] < 0) curSegDigits = prevSegDigits = digits;
        else if (!preview && digits != curSegDigits) {
            prevSegDigits = curSegDigits;
            curSegDigits = digits;
            if (motionEnabled()) { segmentClockAtMs = animNowMs(); startAnimTimer(); }
        }
        const float animT = !preview && segmentClockAtMs > 0.0
            ? std::clamp(static_cast<float>((animNowMs() - segmentClockAtMs) / 240.0), 0.f, 1.f) : 1.f;
        const float eased = easeInOutCubic(animT);
        constexpr float dw = 31.f, dh = 25.f, gap = 6.5f;
        for (int i = 0; i < 4; ++i) {
            const float dx = b.left + 10.f + i * (dw + gap) + (i >= 2 ? 3.f : 0.f);
            drawSevenSegmentDigit(d, dx, b.top + 23.5f, dw, dh, digits[i], prevSegDigits[i], eased, p.ink, p.light);
        }
        const float pulse = preview ? .85f : .35f + .60f * (0.5f + 0.5f * std::cos((GetTickCount64() % 1000) * 6.2831853f / 1000.f));
        const float colonX = b.left + 10.f + 2 * dw + gap + 3.f;
        d.circle(colonX, b.top + 31.5f, 1.35f, tint(p.ink, pulse));
        d.circle(colonX, b.top + 40.5f, 1.35f, tint(p.ink, pulse));
        break;
    }
    case ClockWeather: {
        text(two(time.wHour) + L":" + two(time.wMinute), 3, 3, w - 6, 22, Strong, p.ink, Center);
        d.line(b.left + 9, b.top + 27, b.right - 9, b.top + 27, tint(p.line, .25f));
        text(ShortDays[time.wDayOfWeek], 8, 30, 26, 11, Micro, p.muted);
        text(num(day.day), 8, 40, 22, 13, Caption, p.ink);
        d.circle(b.left + w - 18.f, b.top + 40.5f, 11.5f, tint(p.ink, .10f));
        text(tempText(preview), w - 30.f, 32.5f, 24.f, 16.f, Micro, p.ink, Center);
        break;
    }
    case FlipClock: {
        const std::array<int, 4> digits = {
            time.wHour / 10, time.wHour % 10, time.wMinute / 10, time.wMinute % 10
        };
        if (!preview && curFlipDigits[0] < 0) curFlipDigits = prevFlipDigits = digits;
        else if (!preview && digits != curFlipDigits) {
            prevFlipDigits = curFlipDigits;
            curFlipDigits = digits;
            if (motionEnabled()) { flipClockAtMs = animNowMs(); startAnimTimer(); }
        }
        const float animT = !preview && flipClockAtMs > 0.0
            ? std::clamp(static_cast<float>((animNowMs() - flipClockAtMs) / 380.0), 0.f, 1.f) : 1.f;
        const float eased = easeInOutCubic(animT);
        constexpr float cw = 26.f, ch = 38.f;
        const float xs[4] = {16.f, 44.5f, 83.5f, 112.f};
        for (int i = 0; i < 4; ++i) {
            drawSplitFlapCard(d, b.left + xs[i], b.top + 9.f, cw, ch, digits[i], prevFlipDigits[i], eased, p.light);
        }
        d.circle(b.left + w / 2, b.top + 23, 1.7f, tint(p.muted));
        d.circle(b.left + w / 2, b.top + 33, 1.7f, tint(p.muted));
        break;
    }
    case Storage:
        if (extra.disks.empty()) headline(DiskIcon, L"—", L"Depolama");
        else {
            const auto& disk = extra.disks.front();
            const auto value = spaceText(disk.free);
            text(value, 12, 6, w - 72, 28, d.fit(value, w - 72, ClockBig, Value), p.ink);
            text(L"boş", w - 61, 9, 48, 25, TileLabel, p.muted, Right);
            const float used = disk.total ? static_cast<float>(disk.total - std::min(disk.free, disk.total)) / disk.total : 0.f;
        const unsigned diskColor = used >= .88f ? Design::Red : used >= .75f ? Design::Amber : (p.light ? 0x1A73E8 : 0x4EA1FF);
        progress(12, 40, w - 24, 6, used * (preview ? 1.f : widgetProgress()), diskColor);
        }
        break;
    case NetworkWidget:
        text(extra.networkReady ? L"↓ " + rateText(extra.download.back()) : L"↓ —", 10, 6, w / 2 - 12, 19, Caption, p.light ? 0x0A74B8 : 0x6CD1FF);
        text(extra.networkReady ? L"↑ " + rateText(extra.upload.back()) : L"↑ —", w / 2, 6, w / 2 - 10, 19, Caption, p.light ? 0x6B48C4 : 0xB8A6FF, Right);
        spark(d, at(11, 26, w - 22, 19), extra.download, 0x53C5F7, true);
        break;
    case Power: {
        const auto ring = at((w - 37) / 2, 9, 37, 37);
        const float smoothBat = !preview && batteryAnim.initialized ? std::clamp(batteryAnim.value, 0.f, 100.f) : static_cast<float>(std::max(0, battery));
        d.arc(ring, -90, 360, tint(p.ink, .18f), 3);
        if (smoothBat > 0.5f) d.arc(ring, -90, 3.6f * smoothBat, tint(Design::Green), 3);
        text(percentage(battery), 3, 10, w - 6, 36, Caption, p.ink, Center);
        break;
    }
    case Volume: {
        const unsigned blueStart = 0x0F88F9;
        const unsigned blueEnd = p.light ? 0x1468D2 : 0x4DA3FF;
        const unsigned blueIcon = p.light ? 0x0A7AFF : 0x4DA3FF;
        const Box muteBtn = at(8, 5, 22, 22);
        const bool muteHot = !preview && !editing && hit(view, muteBtn, [this] { changeVolume(0, true); });
        if (muteHot) d.fill(muteBtn, 7, tint(blueIcon, .18f));
        d.circle(b.left + 19.f, b.top + 16.f, 10.f, tint(p.ink, .09f + (muteHot ? .10f : 0.f)));
        d.icon(Speaker, at(12, 9, 14, 14), tint(p.ink, !preview && extra.muted ? .50f : .90f));
        text(preview ? L"Hoparlör" : extra.outputName.empty() ? L"Ses çıkışı" : extra.outputName,
            34, 6, w - 85, 20, TileLabel, p.ink);
        const float rawLevel = extra.muted ? 0.f : (extra.volume < 0 ? 0.f : std::clamp(extra.volume / 100.f, 0.f, 1.f));
        const float shownLevel = preview ? .62f : (barVolumeLevel >= 0.f ? barVolumeLevel : rawLevel);
        const int shownPct = preview ? 62 : (extra.volume < 0 ? -1 : static_cast<int>(std::lround(shownLevel * 100.f)));
        text(!preview && extra.muted ? L"Sessiz" : percentage(shownPct), w - 53, 6, 42, 20, Caption,
            barVolumeDrag ? blueEnd : p.muted, Right);
        const Box trackBox = at(10, 31, w - 20, 16);
        if (!preview && &view == &bar) {
            barVolumeCard = b;
            barVolumeTrack = trackBox;
        }
        d.fill(trackBox, 8.f, tint(p.light ? 0xD8D8DC : 0xFFFFFF, p.light ? 1.f : .14f));
        const float fillW = width(trackBox) * std::clamp(shownLevel, 0.f, 1.f);
        if (fillW > 0.5f) {
            d.clip(box(trackBox.left, trackBox.top, fillW, height(trackBox)));
            d.fill(trackBox, 8.f, tint(blueStart));
            d.unclip();
        }
        if (!preview && (barVolumeDrag || hover > 0.05f)) {
            const float knobX = std::clamp(trackBox.left + fillW, trackBox.left + 5.5f, trackBox.right - 5.5f);
            d.circle(knobX, trackBox.top + 8.f, barVolumeDrag ? 4.2f : 3.5f, tint(0xFFFFFF, barVolumeDrag ? .96f : .82f * hover));
        }
        break;
    }
    case AnalogClock: {
        const float cx = b.left + 27, cy = b.top + 28;
        d.circle(cx, cy, 18, tint(p.ink, .34f), false);
        const auto hand = [&](float turns, float length, unsigned color, float stroke) {
            const float angle = turns * 6.2831853f;
            d.line(cx, cy, cx + std::sin(angle) * length, cy - std::cos(angle) * length, tint(color), stroke);
        };
        float secSmooth = static_cast<float>(time.wSecond);
        if (!preview && analogSecAtMs > 0.0 && analogPrevSec >= 0) {
            const float t = std::clamp(static_cast<float>((animNowMs() - analogSecAtMs) / 180.0), 0.f, 1.f);
            float delta = static_cast<float>(time.wSecond - analogPrevSec);
            if (delta < -30.f) delta += 60.f;
            else if (delta > 30.f) delta -= 60.f;
            secSmooth = static_cast<float>(analogPrevSec) + delta * easeOutBack(t);
        }
        const float minutes = time.wMinute + secSmooth / 60.f;
        hand((time.wHour + minutes / 60.f) / 12.f, 9, p.ink, 2);
        hand(minutes / 60.f, 13, p.ink, 1.5f);
        hand(secSmooth / 60.f, 15, p.light ? 0xD93829 : 0xFF6242, 1);
        d.circle(cx, cy, 2, tint(p.ink));
        text(two(time.wHour) + L":" + two(time.wMinute), 52, 8, w - 57, 22, Strong, p.ink);
        text(ShortDays[time.wDayOfWeek], 52, 30, w - 57, 16, Caption, p.muted);
        break;
    }
    case CpuMeter: case RamMeter: {
        const int value = id == CpuMeter ? cpu : ram;
        const float smoothVal = preview ? static_cast<float>(std::max(0, value))
            : id == CpuMeter ? (cpuAnim.initialized ? cpuAnim.value : static_cast<float>(std::max(0, cpu)))
                             : (ramAnim.initialized ? ramAnim.value : static_cast<float>(ram));
        const auto ring = at(12, 10, 36, 36);
        d.arc(ring, -90, 360, tint(p.ink, .14f), 3);
        if (smoothVal > 0.5f) d.arc(ring, -90, 3.6f * std::clamp(smoothVal, 0.f, 100.f), tint(accent), 3);
        text(percentage(value), 54, 6, w - 61, 25, Strong, p.ink);
        text(id == CpuMeter ? L"CPU" : L"RAM", 54, 29, w - 61, 17, Caption, p.muted);
        break;
    }
    case DayProgress: case MonthProgress: case YearProgress: {
        const float dayPart = (time.wHour * 60 + time.wMinute) / 1440.f;
        float elapsed = dayPart, duration = 1;
        if (id == MonthProgress) {
            elapsed += day.day - 1;
            duration = static_cast<float>(day.length());
        } else if (id == YearProgress) {
            elapsed += day.day - 1;
            for (int month = 1; month < day.month; ++month) elapsed += Date{day.year, month, 1}.length();
            duration = 365.f + (Date{day.year, 2, 1}.length() == 29);
        }
        const float fraction = elapsed / duration;
        text(Widgets[id].title, 10, 7, w - 42, 19, Caption, p.muted);
        text(percentage(static_cast<int>(fraction * 100)), w - 40, 7, 29, 19, Caption, p.ink, Right);
        progress(11, 35, w - 23, 5, fraction, accent);
        break;
    }
    case DesktopToggle:
        d.icon(ScreenIcon, at((w - 23) / 2, 16, 23, 23), tint(p.light ? 0x4458B8 : 0x8EA8FF));
        break;
    case StartButton: {
        const float cx = b.left + w / 2.f, cy = b.top + Design::CardHeight / 2.f;
        for (int i = 0; i < 4; ++i) {
            d.fill(box(cx - 9.f + (i % 2) * 10.f, cy - 9.f + (i / 2) * 10.f, 8.f, 8.f),
                1.2f, tint(hover > 0 ? (p.light ? p.action : 0xFFFFFF) : p.ink, .92f));
        }
        break;
    }
    case OpenWindows: headline(Layers, extra.windows.empty() && !shown(id) ? L"—" : num(extra.windows.size()), L"Açık pencere"); break;
    case Quick:
        d.icon(Sliders, at((w - 23) / 2, 16, 23, 23), tint(hover > 0 ? (p.light ? 0x0A7AFF : 0x62B0FF) : p.ink));
        break;
    case QuickToggles:
        drawQuickToggles(view, b, p, preview);
        break;
    case Weather: case WeatherDetails: case WeatherForecast: case WeatherStatus: case WeatherHourly: case SunTimes:
    case StockChart: case StockWatchlist: case Crypto: case Recent: case RecycleBin: case Search: case Commands: case Capture:
        drawNewWidget(view, id, b, p, preview, editing);
        break;
    case Usage: {
        const unsigned watchBlue = 0x0F88F9;
        const unsigned watchDark = 0x22476B;
        // Smartwatch silhouette icon matching Screen Time reference
        d.fill(at(18, 12, 11, 5), 2.f, tint(watchDark, .88f));
        d.fill(at(18, 39, 11, 5), 2.f, tint(watchDark, .88f));
        d.gradient(at(14, 16, 19, 24), 6.f, tint(watchBlue), tint(watchDark));
        d.fill(at(33.5f, 23, 2.f, 7), 1.f, tint(watchBlue, .85f));
        d.icon(Timer, at(17.f, 21.f, 13.f, 13.f), tint(watchBlue));
        text(L"Ekran süresi", 42, 9, 88, 15, Small, p.muted);
        const int shownSec = preview ? 5880 : extra.usageTotal;
        const Text val = usageText(shownSec);
        text(val, 42, 23, 92, 24, d.fit(val, 92, Value, Strong), p.ink);
        // 24-hour mini blue histogram on right
        const float chartX = w - 66.f, baseY = 42.f;
        d.line(b.left + chartX, b.top + baseY, b.left + chartX + 54.f, b.top + baseY, tint(p.ink, .14f), .8f);
        static constexpr int sampleHrs[24] = {0,0,0,0,0,0,0,0,120,240,3400,600,150,0,280,1400,2600,1100,1900,800,0,0,0,0};
        for (int i = 0; i < 24; ++i) {
            const int sec = preview ? sampleHrs[i]
                : (model.usageDay == day.key() ? model.usageSeconds[i] : 0);
            if (sec > 0) {
                const float hBar = std::max(2.f, std::min(1.f, sec / 3600.f) * 20.f) * (preview ? 1.f : widgetProgress());
                d.fill(at(chartX + i * 2.3f, baseY - hBar, 1.5f, hBar), .7f, tint(watchBlue, .95f));
            }
        }
        break;
    }
    case DayTimeline: case WhatsApp: case Currency:
        drawProductivityWidget(view, id, b, p, preview);
        break;
    default:
        if (proWidget(id)) drawProWidget(view, id, b, p, preview, editing);
        break;
    }
    if (editing) {
        const float cx = b.right - 5.f, cy = b.top + 5.f;
        const float pop = std::clamp(dockEditSpring.value, 0.f, 1.15f);
        hit(view, box(b.right - 18.f, b.top - 5.f, 23.f, 23.f), [this, id] { toggleTile(id); });
        const float hotRemove = view.hoverAmount();
        const float badgeScale = pop * (1.f + 0.12f * hotRemove);
        if (badgeScale > 0.05f) {
            D2D1::Matrix3x2F ep{};
            d.target->GetTransform(&ep);
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(badgeScale, badgeScale, {cx, cy}) * ep);
            d.circle(cx, cy, 9.5f, tint(p.base));
            d.circle(cx, cy, 8.2f, tint(mixRgb(Design::Red, 0xFF6B65, hotRemove)));
            d.icon(Minus, box(cx - 5.f, cy - 5.f, 10.f, 10.f), tint(0xFFFFFF));
            d.target->SetTransform(ep);
        }
    }
    if (press > 0.005f) d.target->SetTransform(pressPrev);
}

void Dock::drawAppSlot(View& view, OrderItem item, Box slot, Palette p, float dragLift) {
    auto& d = view.d;
    slot = d.snap(slot);
    const bool editing = dockEditMode;
    const float w = width(slot);
    const float mid = slot.left + w / 2.f;
    const float iconX = slot.left + (w - Design::AppSize) / 2.f;
    const Box bubble{slot.left + 2.f, slot.top, slot.right - 2.f, slot.bottom};
    const float bubbleRad = Design::Radius;
    const float wave = &view != &bar || dragLift > 0.01f ? 0.f : appWaveBoost(mid);

    const auto drawGlyphTransformed = [&](const std::shared_ptr<Gdiplus::Bitmap>& img, Glyph fallback,
                                          float hot, float down, float bouncePx) {
        const float boost = std::max(hot, wave * 0.85f);
        const float iconScale = 1.f + 0.12f * boost - 0.09f * down;
        const float rise = dragLift > 0.01f ? 0.f : (3.2f * boost + bouncePx);
        const Box baseGlyph = d.snap(box(iconX, slot.top + 11.f, Design::AppSize, Design::AppSize));
        const bool transformed = std::abs(iconScale - 1.f) > 0.003f || rise > 0.05f;
        D2D1::Matrix3x2F prev{};
        if (transformed) {
            d.target->GetTransform(&prev);
            const D2D1_POINT_2F piv{baseGlyph.left + Design::AppSize / 2.f, baseGlyph.bottom};
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(iconScale, iconScale, piv) *
                D2D1::Matrix3x2F::Translation(0.f, -rise) * prev);
        }
        if (img) d.image(img, baseGlyph, p.light);
        else d.icon(fallback, inset(baseGlyph, 6.5f), tint(p.ink));
        if (transformed) d.target->SetTransform(prev);
    };

    const auto drawGlassTile = [&](float hot, bool running, bool active) {
        if (p.glass || dragLift > 0.01f) {
            const float lift = std::clamp(dragLift, 0.f, 1.f);
            const float glow = std::max(hot, wave * 0.45f);
            const float tone = p.opacity / 100.f;
            if (p.glass) {
                if (p.theme == LiquidGlass) {
                    const float fillAlpha = (.14f + tone * .18f) + .08f * lift;
                    d.fill(bubble, bubbleRad, tint(0x08090B, fillAlpha));
                    if (glow > 0 && lift < 0.01f) d.fill(bubble, bubbleRad, tint(0xFFFFFF, .055f * glow));
                } else {
                    d.gradient(bubble, bubbleRad, tint(p.high, .28f + .08f * glow + .12f * lift),
                        tint(p.base, .24f + .06f * glow + .10f * lift));
                }
            } else {
                d.fill(bubble, bubbleRad, tint(p.high, .82f * lift));
            }
            if (lift > 0.01f) {
                const Box inner = box(bubble.left + 5.f, bubble.top + 6.f, width(bubble) - 10.f, height(bubble) - 12.f);
                d.fill(inner, 11.5f, tint(p.light ? 0x10141A : 0xFFFFFF, (p.light ? .10f : .13f) * lift));
            }
            const float rimStrength = (active ? .36f : running ? .30f : .26f) + .14f * glow + .18f * lift;
            d.glassRim(bubble, bubbleRad, rimStrength);
        }
    };

    if (item.kind == OrderKind::Shortcut) {
        if (item.index < 0 || static_cast<size_t>(item.index) >= model.shortcuts.size()) return;
        const size_t i = static_cast<size_t>(item.index);
        const auto& app = model.shortcuts[i];
        if (pinnedWindowRevision != dockRevision) {
            pinnedWindows.assign(model.shortcuts.size(), {});
            for (const auto& window : extra.windows) {
                const int index = pinnedIndex(window.app);
                if (index < 0) continue;
                auto& pin = pinnedWindows[index];
                if (!pin.target || window.active) pin.target = window.handle;
                pin.active |= window.active;
            }
            pinnedWindowRevision = dockRevision;
        }
        const auto [target, active] = pinnedWindows[i];
        const bool chrome = target && isChromeApp(app);
        const auto image = i < appImages.size() ? appImages[i] : std::shared_ptr<Gdiplus::Bitmap>{};
        hit(view, slot, [this, i, target, editing, chrome, mid] {
            if (editing) return;
            if (chrome) showChromeWindows(mid);
            else if (target && IsWindow(target)) activateWindow(target, true);
            else if (i < model.shortcuts.size()) launchShortcut(model.shortcuts[i]);
        }, [this, i, target, mid] {
            if (i < model.shortcuts.size()) {
                const auto img = i < appImages.size() ? appImages[i] : std::shared_ptr<Gdiplus::Bitmap>{};
                showAppMenu(model.shortcuts[i], mid, target, img);
            }
        });
        if (!editing) view.hits.back().chromePreview = chrome;
        view.hits.back().order = item;
        if (&view == &bar && page == AppMenu && panelDockX >= 0 && app.target == contextApp.target) panelDockX = mid;
        const float hot = !editing && !dockOrderDragging ? view.hoverAmount() : 0.f;
        const float down = !editing && !dockOrderDragging ? view.pressAmount() : 0.f;
        drawGlassTile(hot, target != nullptr, active);
        drawGlyphTransformed(image, app.target.find(L"://") != Text::npos ? Globe : Link, hot, down, (appBounceAtMs > 0.0 ? appBounceOffset(appKey(app)) : 0.f));
        if (target) {
            const float ratio = appActiveRatio(dockItemKey(item), active);
            const float indW = 3.5f + 6.5f * ratio;
            const float indH = 3.5f - 1.3f * ratio;
            const unsigned indColor = model.accent > 0 ? p.accent : (p.light ? 0x0A7AFF : 0x4DA3FF);
            d.fill(box(slot.left + (w - indW) / 2.f, slot.bottom - 5.5f, indW, indH), indH / 2.f,
                tint(mixRgb(p.muted, indColor, ratio), .72f + .23f * ratio));
        }
        return;
    }

    if (item.kind == OrderKind::Window) {
        const auto found = std::find_if(extra.windows.begin(), extra.windows.end(),
            [&](const OpenWindow& w) { return w.handle == item.window; });
        if (found == extra.windows.end()) return;
        const auto& win = *found;
        const bool chrome = isChromeApp(win.app);
        hit(view, slot, [this, editing, chrome, target = win.handle, mid] {
            if (editing) return;
            if (chrome) showChromeWindows(mid);
            else activateWindow(target, true);
        }, [this, target = win.handle, mid] { showWindowMenu(target, mid); });
        if (!editing) view.hits.back().chromePreview = chrome;
        view.hits.back().order = item;
        if (&view == &bar && page == AppMenu && panelDockX >= 0 && contextWindow == win.handle) panelDockX = mid;
        const float hot = !editing && !dockOrderDragging ? view.hoverAmount() : 0.f;
        const float down = !editing && !dockOrderDragging ? view.pressAmount() : 0.f;
        drawGlassTile(hot, true, win.active);
        drawGlyphTransformed(win.icon, Layers, hot, down, 0.f);
        const float ratio = appActiveRatio(dockItemKey(item), win.active);
        const float indW = 3.5f + 6.5f * ratio;
        const float indH = 3.5f - 1.3f * ratio;
        const unsigned indColor = model.accent > 0 ? p.accent : (p.light ? 0x0A7AFF : 0x4DA3FF);
        d.fill(box(slot.left + (w - indW) / 2.f, slot.bottom - 5.5f, indW, indH), indH / 2.f,
            tint(mixRgb(p.muted, indColor, ratio), .72f + .23f * ratio));
    }
}
