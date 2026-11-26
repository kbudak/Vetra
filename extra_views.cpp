#include "dock.h"
#include <cmath>

namespace {
void track(Draw& d, Box b, float portion, Palette p, unsigned color) {
    d.fill(b, height(b) / 2, tint(p.ink, .12f));
    if (portion > 0) d.fill(box(b.left, b.top, width(b) * std::min(1.f, portion), height(b)), height(b) / 2, tint(color));
}
Glyph weatherIcon(int code, bool day = true) {
    if (code == 0) return day ? Sun : Moon;
    return code >= 50 ? Rain : Cloud;
}
Text degree(int value) { return num(value) + L"°"; }
Text percentage(int value) { return value < 0 ? L"—" : num(value) + L"%"; }
}

void Dock::drawQuickToggles(View& view, Box b, Palette p, bool preview, bool labels) {
    auto& d = view.d;
    constexpr float gap = 5.f;
    const float stripWidth = std::min(width(b), 283.f);
    const float usable = stripWidth - 14.f - 3.f * gap;
    const float circle = std::min(37.f, usable / 6.f);
    const float split = (usable - 2 * circle) / 2.f;
    const float y = b.top + 7.f;
    const auto details = [this, mid = (b.left + b.right) / 2](QuickSubPanel sub) {
        if (!panel || page != Quick || sheetClosing) open(Quick, mid);
        if (panel && page == Quick && !sheetClosing) openQuickSub(sub);
    };
    struct RadioButton { const wchar_t* label; Glyph icon; bool enabled; int command; QuickSubPanel details; };
    const RadioButton controls[]{
        {L"Wi-Fi", Wifi, preview || extra.radios.wifi == 1 || (extra.radios.wifi < 0 && extra.wifiConnected), 1, QuickSubPanel::Wifi},
        {L"Bluetooth", BluetoothIcon, !preview && extra.radios.bluetooth == 1, 2, QuickSubPanel::Bluetooth},
        {L"Paylaş", Link, !preview && extra.radios.hotspot == 1, 4, QuickSubPanel::Main},
        {L"Uçak", Plane, !preview && extra.radios.airplane, 3, QuickSubPanel::Main},
    };
    float x = b.left + (width(b) - stripWidth) / 2 + 7.f;
    for (const auto& control : controls) {
        const bool hasDetails = control.details != QuickSubPanel::Main;
        const float w = hasDetails ? split : circle;
        const float primaryW = hasDetails ? w * .54f : w;
        const auto pill = box(x, y, w, circle);
        float hot = 0.f, detailHot = 0.f, down = 0.f;
        if (!preview && !(dockEditMode && &view != &sheet)) {
            hit(view, box(x, y, primaryW, circle), [this, control, details] {
                if ((control.command == 1 && extra.radios.wifi < 0) ||
                    (control.command == 2 && extra.radios.bluetooth < 0)) details(control.details);
                else queryRadios(control.command);
            });
            hot = view.hoverAmount();
            down = view.pressAmount();
            if (hasDetails) {
                hit(view, box(x + primaryW, y, w - primaryW, circle), [details, control] { details(control.details); });
                detailHot = view.hoverAmount();
            }
        }
        const unsigned blue = p.light ? 0x1268C2 : 0x349BFF;
        const unsigned ink = control.enabled ? blue : p.ink;
        const float feedback = std::max(hot, detailHot);
        if (hasDetails) d.fill(pill, circle / 2, tint(control.enabled ? blue : p.ink,
            (control.enabled ? .15f : .09f) + .075f * feedback));
        else d.circle(x + w / 2, y + circle / 2, circle / 2 - down, tint(ink,
            (control.enabled ? .19f : .09f) + .075f * feedback));
        if (control.enabled) d.outline(box(x + .5f, y + .5f, w - 1, circle - 1), circle / 2 - .5f,
            tint(blue, .74f), .9f);
        const float iconSize = 16.f + hot;
        const auto glyph = box(x + (primaryW - iconSize) / 2, y + (circle - iconSize) / 2, iconSize, iconSize);
        if (control.icon == Plane || control.icon == Link) {
            D2D1::Matrix3x2F previous{};
            d.target->GetTransform(&previous);
            d.target->SetTransform(D2D1::Matrix3x2F::Rotation(control.icon == Plane ? 90.f : 45.f,
                {x + primaryW / 2, y + circle / 2}) * previous);
            d.icon(control.icon, glyph, tint(ink));
            d.target->SetTransform(previous);
        } else d.icon(control.icon, glyph, tint(ink));
        if (hasDetails) {
            d.line(x + primaryW, y + 9, x + primaryW, y + circle - 9, tint(ink, .16f));
            d.icon(ChevronRight, box(x + primaryW + (w - primaryW - 13.f) / 2, y + (circle - 13.f) / 2,
                13, 13), tint(ink, .84f + .16f * detailHot));
        }
        if (labels) d.text(control.label, box(x - 2, y + circle + 4, w + 4, 19), Small, tint(p.muted), Center);
        x += w + gap;
    }
}

float Dock::statusBarWidth() const {
    float result = 0.f;
    for (int id = StatusMic; id <= StatusNotifications; ++id)
        if (shown(id)) result += widgetWidth(id);
    return result;
}

static int dockControlPage(int id) {
    switch (id) {
    case HiddenTrayControl: return SystemTray;
    case StatusMic: return MicControl;
    case StatusKeyboard: return KeyboardLang;
    case StatusNetwork: return NetworkPanel;
    case StatusVolume: return Volume;
    case StatusBattery: return Power;
    case StatusClock:
    case StatusNotifications: return NotificationCenter;
    default: return -1;
    }
}

void Dock::activateDockControl(int id, float anchor) {
    if (id == HiddenTrayControl) showSystemTray(anchor);
    else if (id == DesktopStripControl) toggleDesktop();
    else if (const int destination = dockControlPage(id); destination >= 0) open(destination, anchor);
}

void Dock::drawDockControl(View& view, int id, Box b, Palette p, bool preview) {
    auto& d = view.d;
    const float centerX = (b.left + b.right) / 2.f;
    const float centerY = (b.top + b.bottom) / 2.f;
    const bool active = !preview && &view == &bar && panel && !sheetClosing && page == dockControlPage(id);
    const bool editing = !preview && dockEditMode;
    if (!preview) {
        std::function<void()> secondary;
        if (!editing) {
            switch (id) {
            case StatusMic: secondary = [this] { sampleMicrophone(true); }; break;
            case StatusKeyboard: secondary = [this] { cycleKeyboardLayout(1); }; break;
            case StatusNetwork: secondary = [this] { launch(L"ms-settings:network-status"); }; break;
            case StatusVolume: secondary = [this] { changeVolume(0, true); }; break;
            case StatusBattery: secondary = [this] { launch(L"ms-settings:powersleep"); }; break;
            case StatusClock: secondary = [this] { launch(L"ms-settings:dateandtime"); }; break;
            case StatusNotifications: secondary = [this] { launch(L"ms-settings:notifications"); }; break;
            }
        }
        hit(view, b, [this, id, centerX, editing] {
            if (!editing) activateDockControl(id, centerX);
        }, std::move(secondary));
        view.hits.back().order = {OrderKind::Widget, id};
        if (!editing && &view == &bar) {
            if (id == StatusKeyboard) statusKeyboardHit = b;
            if (id == StatusVolume) statusVolumeHit = b;
        }
    }
    const float hot = preview ? 0.f : view.hoverAmount();
    const float down = preview ? 0.f : view.pressAmount();
    // The dock surface is the background for these indicators. Only their
    // foreground tint and press offset change in response to input.
    const unsigned ink = mixRgb(p.ink, p.accent, active ? .75f : .36f * hot);

    const auto icon = [&](Glyph glyph, float size, Color color) {
        d.icon(glyph, d.snap(box(centerX - size / 2, centerY - size / 2 + down, size, size)), color);
    };
    switch (id) {
    case HiddenTrayControl:
        icon(active == (model.edge == 0) ? ChevronDown : ChevronUp, 16.f, tint(ink, .88f + .12f * hot));
        break;
    case StatusMic: {
        const bool muted = !preview && extra.microphoneMute == 1;
        const bool inUse = preview || (!extra.micActiveApps.empty() && !muted);
        const unsigned color = muted ? Design::focusRed(p.light) :
            inUse ? (p.light ? 0x078D82 : 0x45E5D8) : ink;
        icon(MicIcon, 18.f, tint(color, !preview && extra.microphoneMute < 0 ? .45f : .95f));
        if (muted) d.line(centerX - 6.f, centerY + 6.f, centerX + 6.f, centerY - 6.f, tint(color), 1.55f);
        if (inUse) d.circle(b.right - 7.f, b.top + 12.f, 2.3f, tint(color));
        break;
    }
    case StatusKeyboard: {
        const Text code = preview || extra.keyboardShort.empty() ? L"TR" : extra.keyboardShort;
        if (preview || extra.keyboardSub.empty()) d.text(code, b, Strong, tint(ink), Center);
        else {
            d.text(code, box(b.left, centerY - 15.f, width(b), 18.f), Caption, tint(ink), Center);
            d.text(extra.keyboardSub, box(b.left, centerY + 2.f, width(b), 15.f), Micro, tint(p.muted), Center);
        }
        break;
    }
    case StatusNetwork: {
        const bool ethernet = !preview && (extra.radios.ethernet || extra.netEthernetFast);
        const bool wifi = preview || extra.wifiConnected || !extra.radios.network.empty() || extra.netWifiFast;
        const bool airplane = !preview && extra.radios.airplane;
        icon(airplane ? Plane : ethernet ? EthernetIcon : wifi ? Wifi : GlobeOff, 20.f,
            tint(ethernet || wifi || airplane ? ink : p.muted, .96f));
        break;
    }
    case StatusVolume: {
        const bool muted = !preview && (extra.muted || extra.volume == 0);
        icon(muted ? SpeakerMuted : !preview && extra.volume >= 0 && extra.volume < 45 ? SpeakerLow : Speaker,
            20.f, tint(muted ? p.muted : ink, muted ? .8f : .96f));
        break;
    }
    case StatusBattery: {
        const int percent = preview ? 76 : battery;
        const bool charging = preview || extra.plugged;
        const auto body = box(centerX - 11.25f, centerY - 6.5f + down, 19.5f, 13.f);
        const Color outline = tint(percent < 0 ? p.muted : ink, .88f);
        d.outline(body, 3.2f, outline, 1.35f);
        d.fill(box(body.right + 1.f, body.top + 4.f, 2.f, 5.f), .9f, outline);
        const unsigned fill = charging ? (p.light ? 0x1FA65A : 0x7BE495) :
            percent <= 15 ? Design::Red : percent <= 25 ? Design::Amber : p.ink;
        if (percent > 0) d.fill(box(body.left + 2.5f, body.top + 2.5f, 14.5f * percent / 100.f, 8.f), 1.6f, tint(fill));
        if (charging && percent >= 0) {
            const float x = body.left + 9.8f, y = body.top + 6.5f;
            d.path({{x + 1.2f, y - 5.2f}, {x - 2.2f, y + .4f}, {x + .6f, y + .4f}, {x - 1.2f, y + 5.2f}},
                tint(p.light ? 0x121214 : 0x101412), 1.45f);
        }
        break;
    }
    case StatusClock: {
        const auto t = preview ? catalogTime : now();
        d.text(two(t.wHour) + L":" + two(t.wMinute), box(b.left + 2.f, centerY - 16.f, width(b) - 4.f, 18.f),
            Strong, tint(ink), Center);
        d.text(two(t.wDay) + L"." + two(t.wMonth) + L"." + num(t.wYear), box(b.left + 2.f, centerY + 2.f, width(b) - 4.f, 16.f),
            Small, tint(p.muted), Center);
        break;
    }
    case StatusNotifications: {
        const bool dnd = !preview && extra.dndEnabled;
        const unsigned color = dnd ? (p.light ? 0x078D82 : 0x45E5D8) : ink;
        icon(BellIcon, 17.f, tint(color, .90f));
        if (dnd || preview || !extra.notifications.empty())
            d.circle(b.right - 7.f, b.top + 12.f, 2.3f, tint(dnd ? color : p.accent));
        break;
    }
    case DesktopStripControl: {
        const float half = 11.f + 2.5f * hot - 1.5f * down;
        d.line(centerX, centerY - half, centerX, centerY + half,
            tint(ink, .45f + .35f * hot), 1.5f + .3f * hot);
        break;
    }
    }
}

void Dock::notificationPanel(Palette p) {
    auto& d = sheet.d;
    const auto t = now();
    const Date today = Date::from(t);
    const unsigned teal = model.accent > 0 ? p.accent : (p.light ? 0x078D82 : 0x3BD6C6);
    const unsigned tealInk = model.accent > 0 ? p.activeInk : 0x0B1514;
    const auto rowHoverColor = [&](float hot) {
        return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
    };

    // --- Section 1: Bildirimler ---
    d.text(L"Bildirimler", box(22, 16, 180, 28), Strong, tint(p.ink));

    // Do Not Disturb (Rahatsız Etmeyin) toggle button
    {
        const Box dndBtn = box(224, 16, 32, 28);
        hit(sheet, dndBtn, [this] { toggleDoNotDisturb(); });
        const float hot = sheet.hoverAmount();
        const float down = sheet.pressAmount();
        D2D1::Matrix3x2F prev{};
        if (down > 0.005f) {
            d.target->GetTransform(&prev);
            const float s = 1.f - 0.06f * down;
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {dndBtn.left + 16.f, dndBtn.top + 14.f}) * prev);
        }
        if (extra.dndEnabled) {
            d.fill(dndBtn, 8.f, tint(teal, .92f - .10f * hot));
            d.icon(BellIcon, box(dndBtn.left + 8.f, dndBtn.top + 6.f, 16.f, 16.f), tint(tealInk));
            d.line(dndBtn.left + 8.f, dndBtn.bottom - 7.f, dndBtn.right - 8.f, dndBtn.top + 7.f, tint(tealInk), 1.45f);
        } else {
            d.fill(dndBtn, 8.f, tint(p.ink, .08f + .08f * hot));
            d.outline(box(dndBtn.left + .5f, dndBtn.top + .5f, width(dndBtn) - 1.f, height(dndBtn) - 1.f), 7.5f, tint(p.line, .24f));
            d.icon(BellIcon, box(dndBtn.left + 8.f, dndBtn.top + 6.f, 16.f, 16.f), tint(p.ink, .86f));
        }
        if (down > 0.005f) d.target->SetTransform(prev);
    }

    // "Tümünü temizle" button
    button(sheet, p, box(262, 16, 102, 28), L"Tümünü temizle", [this] { clearAllNotifications(); }, false, Small);

    float listTop = 54.f;
    if (extra.dndEnabled) {
        const Box banner = box(20, 54, 344, 70);
        p.card(d, banner, 12);
        d.icon(Moon, box(32, 63, 14, 14), tint(teal));
        d.text(L"Rahatsız Etmeyin Modu Açık", box(51, 60, 298, 19), Strong, tint(p.ink));
        d.text(L"Yalnızca öncelikli bildirimler ve alarmlar görünür.", box(32, 81, 318, 16), Small, tint(p.muted));
        const Box settingsLink = box(30, 100, 118, 20);
        hit(sheet, settingsLink, [this] { launch(L"ms-settings:notifications"); });
        const float linkHot = sheet.hoverAmount();
        d.text(L"Bildirim ayarları", box(32, 100, 116, 18), Small, tint(mixRgb(teal, p.ink, .15f * linkHot)));
        d.line(32, 116.5f, 111, 116.5f, tint(teal, .55f + .35f * linkHot), .9f);
        listTop = 132.f;
    }

    const float listBottom = 326.f;
    const int totalNotifs = static_cast<int>(extra.notifications.size());
    const float cardH = 74.f, cardGap = 6.f;
    const int maxRows = std::max(1, static_cast<int>((listBottom - listTop + cardGap) / (cardH + cardGap)));
    listOffset = std::clamp(listOffset, 0, std::max(0, totalNotifs - maxRows));

    if (totalNotifs == 0) {
        const float midY = (listTop + listBottom) / 2.f;
        d.icon(BellIcon, box(180, midY - 26.f, 24, 24), tint(p.muted, .50f));
        d.text(L"Yeni bildirim yok", box(20, midY + 4.f, 344, 22), Body, tint(p.muted), Center);
        if (!extra.dndEnabled) {
            const Box settingsLink = box(132, midY + 30.f, 120, 20);
            hit(sheet, settingsLink, [this] { launch(L"ms-settings:notifications"); });
            d.text(L"Bildirim ayarları", settingsLink, Small, tint(teal), Center);
        }
    } else {
        for (int i = 0; i < maxRows && i + listOffset < totalNotifs; ++i) {
            const auto& item = extra.notifications[i + listOffset];
            const float cy = listTop + i * (cardH + cardGap);
            const Box card = box(20, cy, 344, cardH);
            p.card(d, card, 12);
            hit(sheet, box(card.left, card.top, width(card) - 30.f, height(card)), [this, item] { activateNotification(item); });
            const float hot = sheet.hoverAmount();
            if (hot > 0.f) d.fill(card, 12, rowHoverColor(hot));

            // Dismiss button (top-right inside card)
            const Box dismissBox = box(card.right - 26.f, cy + 6.f, 20.f, 20.f);
            hit(sheet, dismissBox, [this, id = item.id] { dismissNotification(id); });
            const float disHot = sheet.hoverAmount();
            if (disHot > 0.f) d.fill(dismissBox, 6.f, tint(p.ink, .14f * disHot));
            d.icon(Close, box(dismissBox.left + 5.f, dismissBox.top + 5.f, 10.f, 10.f), tint(p.muted, .55f + .45f * disHot));

            // App icon + name + timestamp
            if (item.icon) d.image(item.icon, box(31, cy + 9, 16, 16), p.light);
            else d.icon(BellIcon, box(32, cy + 10, 14, 14), tint(teal));
            d.text(item.appName, box(53, cy + 7, 210, 19), Caption, tint(p.ink));
            if (!item.timeLabel.empty())
                d.text(item.timeLabel, box(264, cy + 8, 68, 17), Small, tint(p.muted), Right);

            // Title & body
            if (!item.body.empty()) {
                d.text(item.title, box(31, cy + 29, 322, 19), Strong, tint(p.ink));
                d.text(item.body, box(31, cy + 49, 322, 17), Small, tint(p.muted));
            } else {
                d.text(item.title, box(31, cy + 35, 322, 24), Body, tint(p.ink));
            }
        }
        if (totalNotifs > maxRows) {
            const float trackH = listBottom - listTop - 6.f;
            const float thumbH = std::max(24.f, trackH * static_cast<float>(maxRows) / totalNotifs);
            const float frac = static_cast<float>(listOffset) / std::max(1, totalNotifs - maxRows);
            d.fill(box(369, listTop + 2.f, 2.5f, trackH), 1.25f, tint(p.ink, .08f));
            d.fill(box(369, listTop + 2.f + (trackH - thumbH) * frac, 2.5f, thumbH), 1.25f, tint(teal, .65f));
        }
    }

    // --- Section 2: Mini Calendar + Focus Session ---
    const float calTop = 338.f;
    const float calH = extra.notifCalExpanded ? 308.f : 44.f;
    const Box calCard = box(20, calTop, 344, calH);
    p.card(d, calCard, 14);

    // Date header ("25 Kasım Çarşamba") + collapse/expand chevron
    const Text headerDate = num(today.day) + L" " + Months[today.month - 1] + L" " + LongDays[t.wDayOfWeek];
    hit(sheet, box(26, calTop + 4, 290, 34), [this] {
        selected = month = Date::from(now());
        paintSheet();
    }, [this] { open(Calendar, panelDockX); });
    d.text(headerDate, box(34, calTop + 10, 276, 24), Strong, tint(p.ink));

    iconButton(sheet, p, box(330, calTop + 9, 26, 26), extra.notifCalExpanded ? ChevronDown : ChevronUp, [this] {
        extra.notifCalExpanded = !extra.notifCalExpanded;
        positionSheet();
    });

    if (!extra.notifCalExpanded) return;

    d.line(30, calTop + 44.f, 354, calTop + 44.f, tint(p.ink, .08f));

    // Month title + Up/Down arrows
    d.text(Text(Months[month.month - 1]) + L" " + num(month.year), box(34, calTop + 52.f, 240, 24), Strong, tint(p.ink));
    auto shiftMonth = [this](int dir) {
        Date first{month.year, month.month, 1};
        if ((first.year == 1601 && first.month == 1 && dir < 0) || (first.year == 9999 && first.month == 12 && dir > 0)) return;
        month = first.add(dir < 0 ? -1 : first.length());
        month.day = 1;
        paintSheet();
    };
    iconButton(sheet, p, box(296, calTop + 51.f, 26, 24), ChevronUp, [shiftMonth] { shiftMonth(-1); });
    iconButton(sheet, p, box(328, calTop + 51.f, 26, 24), ChevronDown, [shiftMonth] { shiftMonth(1); });

    // Weekday headers (Pt Sa Ça Pe Cu Ct Pa)
    constexpr const wchar_t* TwoDays[] = {L"Pt", L"Sa", L"Ça", L"Pe", L"Cu", L"Ct", L"Pa"};
    constexpr float gridX = 30.f, cellW = 46.2f, rowH = 26.5f;
    for (int col = 0; col < 7; ++col) {
        d.text(TwoDays[col], box(gridX + col * cellW, calTop + 82.f, cellW, 18.f), Small, tint(p.muted), Center);
    }

    // 6x7 day grid (including previous & next month overflow days)
    const Date firstOfMonth{month.year, month.month, 1};
    const int lead = (firstOfMonth.weekday() + 6) % 7;
    const Date gridStart = firstOfMonth.add(-lead);
    const float gridY = calTop + 102.f;
    for (int idx = 0; idx < 42; ++idx) {
        const Date cellDate = gridStart.add(idx);
        const int col = idx % 7, row = idx / 7;
        const float cx = gridX + col * cellW;
        const float cy = gridY + row * rowH;
        const float midX = cx + cellW / 2.f, midY = cy + 12.f;
        const bool inMonth = (cellDate.month == month.month && cellDate.year == month.year);
        const bool isToday = cellDate.key() == today.key();
        const bool isSel = cellDate.key() == selected.key();

        hit(sheet, box(cx + 4.f, cy, cellW - 8.f, 25.f), [this, cellDate] {
            selected = cellDate;
            month = {cellDate.year, cellDate.month, 1};
            paintSheet();
        }, [this, cellDate] {
            selected = cellDate;
            month = {cellDate.year, cellDate.month, 1};
            open(Calendar, panelDockX);
        });
        const float hot = sheet.hoverAmount();
        if (isToday) {
            d.circle(midX, midY, 12.f, tint(teal));
        } else if (isSel) {
            d.circle(midX, midY, 12.f, tint(teal, .16f));
            d.circle(midX, midY, 11.5f, tint(teal, .85f), false, 1.2f);
        } else if (hot > 0.f) {
            d.circle(midX, midY, 12.f, tint(p.ink, .11f * hot));
        }
        const Color dayCol = isToday ? tint(tealInk)
            : inMonth ? tint(p.ink, .94f)
            : tint(p.muted, .42f);
        d.text(num(cellDate.day), box(cx, cy + 1.5f, cellW, 21.f), isToday ? Strong : Caption, dayCol, Center);

        // Subtle dot if Vetra calendar has an event on this date
        for (const auto& ev : model.events) {
            if (ev.day == cellDate.key()) {
                d.circle(midX, cy + 23.2f, 1.5f, tint(isToday ? tealInk : EventColors[std::clamp(ev.kind, 0, 2)]));
                break;
            }
        }
    }

    // Focus session footer ("-  30 dk  +   [▶ Odaklanma]")
    const float footY = calTop + 266.f;
    d.line(30, footY - 4.f, 354, footY - 4.f, tint(p.ink, .08f));
    iconButton(sheet, p, box(32, footY + 3.f, 26, 26), Minus, [this] {
        setTimerMinutes(Focus, std::max(5, model.focusMinutes - 5));
    });
    const Text focusLabel = focusTimer.active() ? durationText(focusTimer.seconds) : (num(model.focusMinutes) + L" dk");
    d.text(focusLabel, box(62, footY + 3.f, 64, 26), Strong, tint(focusTimer.running() ? teal : p.ink), Center);
    iconButton(sheet, p, box(130, footY + 3.f, 26, 26), Plus, [this] {
        setTimerMinutes(Focus, std::min(FocusMaxMinutes, model.focusMinutes + 5));
    });
    const Text focusBtnLabel = focusTimer.running() ? L"Duraklat" : L"▶  Odaklanma";
    button(sheet, p, box(228, footY + 2.f, 126, 28), focusBtnLabel, [this] { toggleTimer(Focus); }, focusTimer.running(), Caption);
}

void Dock::networkPanel(Palette p) {
    auto& d = sheet.d;
    const bool eth = extra.radios.ethernet || extra.netEthernetFast;
    const bool wifiLinked = extra.wifiConnected || !extra.radios.network.empty();
    const Text sub = extra.radios.airplane ? L"Uçak modu açık"
        : eth ? (wifiLinked ? L"Ethernet · " + extra.radios.network : L"Ethernet bağlı")
        : wifiLinked ? extra.radios.network
        : extra.netWifiFast ? L"Wi-Fi bağlı"
        : L"Bağlı değil";
    heading(p, L"İnternet", sub);

    const auto rowHoverColor = [&](float hot) {
        return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
    };

    float y = 86.f;
    if (eth) {
        const Box ethCard = box(24, y, sheet.w - 48, 46);
        p.card(d, ethCard, 12);
        d.icon(EthernetIcon, box(38, y + 13, 20, 20), tint(Design::Green));
        d.text(L"Ethernet", box(68, y + 5, 220, 19), Strong, tint(p.ink));
        d.text(L"Kablolu bağlantı etkin", box(68, y + 24, 220, 16), Small, tint(Design::Green));
        d.circle(ethCard.right - 18.f, y + 23.f, 4.f, tint(Design::Green));
        y += 52.f;
    }

    // Wi-Fi toggle card
    const Box wifiHeader = box(24, y, sheet.w - 48, 42);
    p.card(d, wifiHeader, 12);
    d.icon(Wifi, box(38, y + 11, 20, 20), tint(wifiLinked || extra.radios.wifi == 1 ? p.accent : p.muted));
    d.text(L"Wi-Fi", box(68, y + 5, 190, 32), Strong, tint(p.ink));
    if (extra.radios.wifi >= 0)
        knob(p, box(wifiHeader.right - 56.f, y + 10.f, 44.f, 22.f), extra.radios.wifi == 1, [this] { queryRadios(1); });
    y += 48.f;

    const bool locBlocked = extra.wifiError == ERROR_ACCESS_DENIED;
    const float listBottom = sheet.h - 58.f;
    if (locBlocked && extra.wifi.empty() && !extra.wifiScanning) {
        const Box warnCard = box(24, y, sheet.w - 48, 76);
        p.card(d, warnCard, 12);
        d.text(L"Yakındaki ağlar taranamıyor", box(38, y + 7, width(warnCard) - 28, 19), Strong, tint(p.ink));
        d.text(L"Canlı Wi-Fi taraması için Windows konum izni gerekli.", box(38, y + 26, width(warnCard) - 28, 15), Small, tint(p.muted));
        button(sheet, p, box(38, y + 46, 154, 24), L"Konum iznini aç", [this] { launch(L"ms-settings:privacy-location"); }, true, Small);
        button(sheet, p, box(200, y + 46, 156, 24), L"Windows ağ paneli", [this] { launch(L"ms-availablenetworks:"); }, false, Small);
        y += 82.f;
        if (!extra.savedWifi.empty() && y + 44.f <= listBottom) {
            d.text(L"Kayıtlı profiller", box(26, y, 240, 18), Small, tint(p.muted));
            y += 20.f;
            const int maxSaved = std::max(1, static_cast<int>((listBottom - y) / 42.f));
            listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.savedWifi.size()) - maxSaved));
            for (int i = 0; i < maxSaved && i + listOffset < static_cast<int>(extra.savedWifi.size()); ++i) {
                const size_t idx = i + listOffset;
                const auto& net = extra.savedWifi[idx];
                const Box item = box(24, y + i * 42.f, sheet.w - 48, 38);
                p.card(d, item, 11);
                hit(sheet, item, [this, idx] { connectWifi(idx, true); });
                const float hot = sheet.hoverAmount();
                if (hot > 0.f) d.fill(item, 11, rowHoverColor(hot));
                d.icon(Wifi, box(36, item.top + 10, 18, 18), tint(net.connected ? p.accent : p.muted));
                d.text(net.name, box(62, item.top + 1, 210, 20), Strong, tint(p.ink));
                d.text(net.connected ? L"Bağlı" : L"Kayıtlı profil", box(62, item.top + 20, 210, 14), Small, tint(p.muted));
                d.text(net.connected ? L"Kes" : L"Bağlan", box(item.right - 82.f, item.top, 70.f, 38.f), Caption, tint(p.accent), Right);
            }
        }
    } else {
        d.text(extra.wifiScanning ? L"Yakındaki ağlar taranıyor…" : L"Kullanılabilir Wi-Fi ağları",
            box(26, y, 300, 20), Caption, tint(p.muted));
        y += 24.f;
        const int rows = std::max(1, static_cast<int>((listBottom - y) / 42.f));
        listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.wifi.size()) - rows));
        for (int i = 0; i < rows && i + listOffset < static_cast<int>(extra.wifi.size()); ++i) {
            const size_t idx = i + listOffset;
            const auto& net = extra.wifi[idx];
            const Box item = box(24, y + i * 42.f, sheet.w - 48, 38);
            p.card(d, item, 11);
            hit(sheet, item, [this, idx] { connectWifi(idx, false); });
            const float hot = sheet.hoverAmount();
            if (hot > 0.f) d.fill(item, 11, rowHoverColor(hot));
            d.icon(Wifi, box(36, item.top + 10, 18, 18), tint(net.connected ? p.accent : p.muted));
            d.text(net.name, box(62, item.top + 1, 210, 20), Strong, tint(p.ink));
            const Text st = net.connected
                ? (net.signal >= 0 ? L"Bağlı · %" + num(net.signal) : L"Bağlı")
                : (net.secure ? L"Parola korumalı" : L"Açık ağ");
            d.text(st, box(62, item.top + 20, 210, 14), Small, tint(p.muted));
            if (net.connected) d.text(L"Kes", box(item.right - 78.f, item.top, 66.f, 38.f), Caption, tint(p.accent), Right);
            else if (net.signal >= 0) d.text(num(net.signal) + L"%", box(item.right - 78.f, item.top, 66.f, 38.f), Caption, tint(p.muted), Right);
        }
        if (extra.wifi.empty()) {
            d.text(extra.radios.wifi == 0 ? L"Wi-Fi kapalı"
                : extra.wifiScanning ? L"Ağlar aranıyor…"
                : L"Kullanılabilir Wi-Fi ağı bulunamadı",
                box(24, y + 18.f, sheet.w - 48, 60), Body, tint(p.muted), Center);
        }
    }

    const float fy = sheet.h - 48.f;
    const float halfW = (sheet.w - 58.f) / 2.f;
    button(sheet, p, box(24, fy, halfW, 34), extra.wifiScanning ? L"Taranıyor…" : L"Yenile", [this] {
        sampleNetworkFast();
        refreshWifi(true, true);
        queryRadios();
        paintSheet();
    });
    button(sheet, p, box(24 + halfW + 10.f, fy, halfW, 34), L"Ağ ayarları", [this] { launch(L"ms-settings:network-status"); });
}

void Dock::keyboardPanel(Palette p) {
    auto& d = sheet.d;
    Text activeSubtitle = L"Yüklü klavye düzenleri";
    for (const auto& item : extra.keyboardLayouts) {
        if (item.active) {
            activeSubtitle = item.languageName;
            if (!item.layoutName.empty() && item.layoutName != item.languageName)
                activeSubtitle += L" · " + item.layoutName;
            break;
        }
    }
    heading(p, L"Klavye dili", activeSubtitle);
    const int count = static_cast<int>(extra.keyboardLayouts.size());
    constexpr int maxRows = 5;
    listOffset = std::clamp(listOffset, 0, std::max(0, count - maxRows));
    const int visible = std::min(maxRows, count);
    const auto rowHoverColor = [&](float hot) {
        return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
    };
    for (int i = 0; i < visible; ++i) {
        const auto& item = extra.keyboardLayouts[i + listOffset];
        const Box row = box(24, 92 + i * 52.f, sheet.w - 48, 46);
        p.card(d, row, 12);
        hit(sheet, row, [this, hkl = item.hkl] { switchKeyboardLayout(hkl); });
        const float hot = sheet.hoverAmount();
        if (item.active) {
            d.fill(row, 12, tint(p.accent, p.glass ? .22f : .14f));
            d.outline(box(row.left + .5f, row.top + .5f, width(row) - 1.f, height(row) - 1.f), 11.5f, tint(p.accent, .65f));
        } else if (hot > 0) {
            d.fill(row, 12, rowHoverColor(hot));
        }
        const Box codePill = box(row.left + 10.f, row.top + 9.f, 34.f, 28.f);
        d.fill(codePill, 8.f, tint(item.active ? p.accent : p.ink, item.active ? .24f : .10f));
        d.text(item.shortCode, codePill, Strong, tint(item.active ? p.accent : p.ink), Center);
        d.text(item.languageName, box(row.left + 54.f, row.top + 5.f, width(row) - 92.f, 20.f), Strong, tint(p.ink));
        d.text(item.layoutName, box(row.left + 54.f, row.top + 25.f, width(row) - 92.f, 16.f), Small, tint(p.muted));
        if (item.active) d.icon(Check, box(row.right - 30.f, row.top + 14.f, 18.f, 18.f), tint(p.accent));
    }
    if (count == 0) {
        d.text(L"Klavye düzeni okunamadı.", box(24, 100, sheet.w - 48, 42), Body, tint(p.muted), Center);
    }
    const float footerY = sheet.h - 52.f;
    const float halfW = (sheet.w - 58.f) / 2.f;
    button(sheet, p, box(24, footerY, halfW, 34), L"Sonraki düzen", [this] { cycleKeyboardLayout(1); }, count > 1);
    button(sheet, p, box(24 + halfW + 10.f, footerY, halfW, 34), L"Dil ayarları", [this] { launch(L"ms-settings:regionlanguage"); });
}

void Dock::microphonePanel(Palette p) {
    auto& d = sheet.d;
    const Text sub = extra.micDeviceName.empty() ? L"Varsayılan giriş ve ses çıkışı" : extra.micDeviceName;
    heading(p, L"Mikrofon", sub);

    const bool muted = extra.microphoneMute == 1;
    const bool inUse = !extra.micActiveApps.empty() && !muted;
    const Box statusCard = box(29, 88, 380, 48);
    p.card(d, statusCard, 12);
    const unsigned dotCol = muted ? Design::focusRed(p.light)
        : inUse ? (p.light ? 0x078D82 : 0x45E5D8)
        : extra.microphoneMute < 0 ? p.muted : Design::Green;
    d.circle(statusCard.left + 18.f, statusCard.top + 24.f, 5.f, tint(dotCol));
    Text statusTitle, statusDetail;
    if (extra.microphoneMute < 0) {
        statusTitle = L"Mikrofon algılanmadı";
        statusDetail = L"Windows ses girişi bulunamadı";
    } else if (muted) {
        statusTitle = L"Mikrofon susturuldu";
        statusDetail = extra.micActiveApps.empty() ? L"Hiçbir uygulama ses alamıyor" : L"Kayıt isteyen: " + extra.micActiveApps.front();
    } else if (inUse) {
        statusTitle = L"Mikrofon şu an kullanımda";
        for (size_t i = 0; i < extra.micActiveApps.size(); ++i) {
            if (i) statusDetail += L", ";
            statusDetail += extra.micActiveApps[i];
        }
    } else {
        statusTitle = L"Mikrofon hazır";
        statusDetail = L"Şu an aktif kayıt yapan uygulama yok";
    }
    d.text(statusTitle, box(statusCard.left + 32.f, statusCard.top + 6.f, width(statusCard) - 44.f, 19.f), Strong, tint(p.ink));
    d.text(statusDetail, box(statusCard.left + 32.f, statusCard.top + 25.f, width(statusCard) - 44.f, 16.f), Small, tint(inUse ? dotCol : p.muted));

    const Text micLabel = extra.microphoneMute < 0 ? L"Mikrofon yok"
        : muted ? L"Mikrofonu aç" : L"Mikrofonu sustur";
    const Text spkLabel = extra.volume < 0 ? L"Ses çıkışı yok"
        : extra.muted ? L"Sesi aç" : L"Sesi sustur";
    button(sheet, p, box(29, 146, 185, 38), micLabel, [this] { sampleMicrophone(true); }, muted);
    button(sheet, p, box(224, 146, 185, 38), spkLabel, [this] { changeVolume(0, true); }, extra.muted);

    d.text(L"Giriş aygıtları", box(31, 193, 240, 20), Caption, tint(p.muted));
    constexpr int rows = 3;
    const int devCount = static_cast<int>(extra.micDevices.size());
    listOffset = std::clamp(listOffset, 0, std::max(0, devCount - rows));
    const auto rowHoverColor = [&](float hot) {
        return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
    };
    for (int i = 0; i < rows && i + listOffset < devCount; ++i) {
        const auto& dev = extra.micDevices[i + listOffset];
        const Box item = box(29, 215 + i * 40.f, 380, 35);
        p.card(d, item, 10);
        hit(sheet, item, [this, id = dev.id] { setMicEndpoint(id); });
        const float hot = sheet.hoverAmount();
        if (dev.active) {
            d.fill(item, 10, tint(p.accent, p.glass ? .20f : .14f));
            d.outline(box(item.left + .5f, item.top + .5f, width(item) - 1.f, height(item) - 1.f), 9.5f, tint(p.accent, .62f));
        } else if (hot > 0) {
            d.fill(item, 10, rowHoverColor(hot));
        }
        d.icon(MicIcon, box(41, item.top + 9, 16, 16), tint(dev.active ? p.accent : p.muted));
        d.text(dev.name, box(66, item.top + 1, 298, 33), Body, tint(p.ink));
        if (dev.active) d.icon(Check, box(374, item.top + 9, 16, 16), tint(p.accent));
    }
    if (devCount == 0) {
        d.text(L"Etkin mikrofon aygıtı bulunamadı.", box(29, 224, 380, 80), Body, tint(p.muted), Center);
    }

    button(sheet, p, box(29, 344, 185, 34), L"Mikrofon gizliliği", [this] { launch(L"ms-settings:privacy-microphone"); });
    button(sheet, p, box(224, 344, 185, 34), L"Ses ayarları", [this] { launch(L"ms-settings:sound"); });
}

void Dock::extraPanel(Palette p) {
    auto& d = sheet.d;
    const unsigned color = p.accent;
    const auto stamp = GetTickCount64();
    const auto action = [&](float x, float y, float w, const Text& title, std::function<void()> click, bool primary = false) {
        button(sheet, p, box(x, y, w, 34), title, std::move(click), primary);
    };
    if (cityPicker) {
        const bool weather = page == Weather;
        heading(p, L"Şehir seç", weather ? L"Open-Meteo hava tahmini" : L"Dünya saati");
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(Cities)) + weather; ++i) {
            const int city = i < ARRAYSIZE(Cities) ? i : -1;
            const bool active = city == (weather ? model.weatherCity : model.worldCity);
            action(29 + 193.f * (i % 2), 103 + 41.f * (i / 2), 187,
                city < 0 ? L"Tahmini kapat" : Cities[city].name, [this, city] { selectCity(city); }, active);
        }
        action(29, weather ? 339 : 269, 108, L"‹  Geri", [this] { cityPicker = false; sheet.key = sheet.hover = -1; paintSheet(); });
        return;
    }
    if (page == Stopwatch) {
        heading(p, L"Kronometre", extra.laps.empty() ? L"Tur süresi kaydedebilirsin" : num(extra.laps.size()) + L" tur kaydedildi");
        p.card(d, box(29, 102, 380, 105));
        d.rimProgress(box(30.5f, 103.5f, 377.f, 102.f), Design::Radius - 1.5f,
            timerRim(Stopwatch, stamp), tint(Widgets[Stopwatch].color), 2.25f);
        d.text(durationText(extra.elapsed(stamp) / 1000), box(39, 121, 360, 62), Display, tint(p.ink), Center);
        int count = static_cast<int>(extra.laps.size());
        listOffset = std::clamp(listOffset, 0, std::max(0, count - 3));
        for (int row = 0; row < 3 && count - 1 - row - listOffset >= 0; ++row) {
            int index = count - 1 - row - listOffset;
            float y = 220 + row * 34.f;
            p.card(d, box(29, y, 380, 29), 8);
            d.text(L"Tur " + num(index + 1), box(40, y + 2, 114, 25), Body, tint(p.muted));
            const auto milliseconds = extra.laps[index] - (index ? extra.laps[index - 1] : 0);
            d.text(durationText(milliseconds / 1000) + L"," + two(static_cast<int>(milliseconds / 10 % 100)), box(196, y + 2, 199, 25), Strong, tint(p.ink), Right);
        }
        if (extra.laps.empty()) d.text(L"Henüz tur kaydı yok", box(28, 229, 381, 64), Caption, tint(p.muted), Center);
        action(29, 338, 144, extra.watchStart ? L"Duraklat" : L"Başlat", [this] { toggleStopwatch(); }, true);
        action(182, 338, 105, L"Tur kaydet", [this] { lapStopwatch(); });
        action(296, 338, 113, L"Sıfırla", [this] { resetStopwatch(); });
    } else if (page == World) {
        heading(p, L"Dünya saati", L"Windows saat diliminden yerel saat");
        p.card(d, box(29, 104, 380, 143));
        d.icon(Globe, box(56, 137, 41, 41), tint(color));
        d.text(worldTime(), box(115, 113, 263, 66), Display, tint(p.ink));
        d.text(Cities[model.worldCity].name, box(121, 185, 246, 35), Value, tint(p.muted));
        action(29, 268, 380, L"Şehri değiştir", [this] { cityPicker = true; sheet.key = sheet.hover = -1; paintSheet(); }, true);
    } else if (page == Storage) {
        heading(p, L"Depolama", L"Yerel disklerin kullanılabilir alanı");
        listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.disks.size()) - 3));
        for (int i = 0; i < 3 && i + listOffset < static_cast<int>(extra.disks.size()); ++i) {
            auto disk = extra.disks[i + listOffset];
            const float y = 104 + i * 73.f;
            p.card(d, box(29, y, 380, 65), 12);
            d.icon(DiskIcon, box(44, y + 15, 24, 24), tint(color));
            d.text(disk.path.substr(0, 2), box(79, y + 5, 106, 33), Strong, tint(p.ink));
            d.text(spaceText(disk.free) + L" boş / " + spaceText(disk.total), box(172, y + 6, 220, 31), Caption, tint(p.muted), Right);
            const float used = disk.total ? 1.f - static_cast<float>(disk.free) / static_cast<float>(disk.total) : 0.f;
            const unsigned diskColor = used >= .88f ? Design::Red : used >= .75f ? Design::Amber : (p.light ? 0x1A73E8 : 0x4EA1FF);
            track(d, box(80, y + 45, 309, 5), used, p, diskColor);
        }
        if (extra.disks.empty()) d.text(L"Yerel disk bilgisi alınamadı.", box(29, 110, 380, 150), Body, tint(p.muted), Center);
        action(29, 338, 380, L"Depolama ayarlarını aç", [this] { launch(L"ms-settings:storagesense"); });
    } else if (page == NetworkWidget) {
        heading(p, L"Ağ trafiği", L"Etkin fiziksel ağ bağdaştırıcıları");
        for (int i = 0; i < 2; ++i) {
            const auto& data = i ? extra.upload : extra.download;
            const auto accent = i ? (p.light ? 0x744AAC : 0xB4A0FF) : color;
            const float y = 100 + i * 112.f;
            p.card(d, box(29, y, 380, 103));
            d.text(i ? L"↑  Yükleme" : L"↓  İndirme", box(47, y + 10, 196, 22), Body, tint(p.muted));
            d.text(extra.networkReady ? rateText(data.back()) : L"Ölçülüyor…", box(47, y + 35, 156, 39), Value, tint(p.ink));
            d.line(224, y + 78, 386, y + 78, tint(p.line, .14f));
            spark(d, box(224, y + 31, 162, 45), data, accent);
        }
        action(29, 338, 380, L"Ağ ayarlarını aç", [this] { launch(L"ms-settings:network-status"); });
    } else if (page == Power) {
        const int percent = battery;
        const float smoothPct = batteryAnim.initialized ? std::clamp(batteryAnim.value, 0.f, 100.f) : static_cast<float>(std::max(0, percent));
        const unsigned ringColor = p.light ? 0x169C63 : 0x4BD898;
        heading(p, L"Pil ve güç", extra.plugged ? L"Prize bağlı" : L"Windows güç durumu");
        p.card(d, box(29, 105, 380, 139));
        d.arc(box(61, 132, 77, 77), -90, 360, tint(p.ink, .12f), 5);
        if (smoothPct > 0.3f) d.arc(box(61, 132, 77, 77), -90, smoothPct * 3.6f, tint(ringColor), 5);
        d.icon(Battery, box(86, 158, 27, 27), tint(ringColor));
        d.text(percentage(percent), box(164, 122, 224, 60), Number, tint(p.ink));
        Text detail = percent < 0 ? L"Pil algılanmadı" : extra.batteryMinutes >= 0 && !extra.plugged ?
            usageText(extra.batteryMinutes * 60) + L" tahmini" : extra.plugged ? L"Harici güç" : L"Pil kullanılıyor";
        d.text(detail, box(166, 187, 222, 26), Body, tint(p.muted));
        action(29, 267, 380, L"Güç ayarlarını aç", [this] { launch(L"ms-settings:powersleep"); }, true);
    } else if (page == Volume) {
        const Text sub = extra.outputName.empty() ? L"Varsayılan ses çıkışı" : extra.outputName;
        heading(p, L"Ses", sub);

        // Dedicated interactive volume slider card
        const Box volCard = box(24, 88, sheet.w - 48, 64);
        p.card(d, volCard, 14);
        const unsigned blueStart = p.light ? 0x0A7AFF : 0x2684FF;
        const unsigned blueEnd = p.light ? 0x3A9BFF : 0x5BB6FF;
        const bool isMuted = extra.muted || extra.volume == 0;
        const Glyph spkGlyph = isMuted ? SpeakerMuted : (extra.volume >= 0 && extra.volume < 45) ? SpeakerLow : Speaker;
        iconButton(sheet, p, box(34, 104, 34, 32), spkGlyph, [this] { changeVolume(0, true); }, isMuted);
        const float qVol = barVolumeLevel >= 0.f ? std::clamp(barVolumeLevel, 0.f, 1.f)
            : (extra.muted ? 0.f : std::max(0, extra.volume) / 100.f);
        const Box volTrack = box(78, 114.5f, sheet.w - 166.f, 11.f);
        quickVolumeTrack = volTrack;
        if (extra.volume >= 0) hit(sheet, box(volTrack.left - 4.f, 102.f, width(volTrack) + 8.f, 36.f), [] {});
        const float volHover = sheet.hoverAmount();
        d.fill(volTrack, 5.5f, tint(p.light ? 0xD8D8DC : 0xFFFFFF, p.light ? 1.f : .14f));
        const float fillW = width(volTrack) * qVol;
        if (fillW > 0.5f) {
            const float drawnW = std::max(height(volTrack), fillW);
            d.gradient(box(volTrack.left, volTrack.top, std::min(width(volTrack), drawnW), height(volTrack)),
                5.5f, tint(blueStart), tint(blueEnd), true);
        }
        if (qVol > 0.005f || volumeDrag || volHover > 0.05f) {
            const float kx = std::clamp(volTrack.left + std::max(5.5f, fillW - 5.5f), volTrack.left + 5.5f, volTrack.right - 5.5f);
            const float thumbVis = volumeDrag ? 1.f : std::max(0.75f, volHover);
            d.circle(kx, volTrack.top + 5.5f, volumeDrag ? 4.3f : 3.6f, tint(0xFFFFFF, .96f * thumbVis));
        }
        const int shownVol = extra.volume < 0 ? -1 : static_cast<int>(std::lround(qVol * 100.f));
        d.text(extra.muted ? L"0%" : percentage(shownVol), box(volTrack.right + 8.f, 106.f, 44.f, 28.f), Strong,
            tint(volumeDrag ? blueEnd : p.ink), Right);

        // Output devices list
        d.text(L"Çıkış aygıtları", box(26, 162, 240, 20), Caption, tint(p.muted));
        constexpr int rows = 3;
        const int devCount = static_cast<int>(extra.audioDevices.size());
        listOffset = std::clamp(listOffset, 0, std::max(0, devCount - rows));
        const auto rowHoverColor = [&](float hot) {
            return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
        };
        for (int i = 0; i < rows && i + listOffset < devCount; ++i) {
            const auto& dev = extra.audioDevices[i + listOffset];
            const Box item = box(24, 186 + i * 44.f, sheet.w - 48, 38);
            p.card(d, item, 11);
            hit(sheet, item, [this, id = dev.id] { setAudioEndpoint(id); });
            const float hot = sheet.hoverAmount();
            if (dev.active) {
                d.fill(item, 11, tint(p.accent, p.glass ? .20f : .14f));
                d.outline(box(item.left + .5f, item.top + .5f, width(item) - 1.f, height(item) - 1.f), 10.5f, tint(p.accent, .62f));
            } else if (hot > 0.f) {
                d.fill(item, 11, rowHoverColor(hot));
            }
            d.icon(Speaker, box(36, item.top + 10, 18, 18), tint(dev.active ? p.accent : p.muted));
            d.text(dev.name, box(62, item.top + 1, width(item) - 72.f, 36), Body, tint(p.ink));
            if (dev.active) d.icon(Check, box(item.right - 28.f, item.top + 10, 17, 17), tint(p.accent));
        }
        if (devCount == 0) {
            d.text(L"Etkin ses çıkış aygıtı bulunamadı.", box(24, 200, sheet.w - 48, 70), Body, tint(p.muted), Center);
        }

        const float fy = sheet.h - 48.f;
        const float halfW = (sheet.w - 58.f) / 2.f;
        button(sheet, p, box(24, fy, halfW, 34), L"Mikrofon", [this] { open(MicControl, panelDockX); });
        button(sheet, p, box(24 + halfW + 10.f, fy, halfW, 34), L"Ses ayarları", [this] { launch(L"ms-settings:sound"); });
    } else if (page == OpenWindows) {
        heading(p, L"Açık pencereler", num(extra.windows.size()) + L" pencere");
        listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.windows.size()) - 5));
        for (int i = 0; i < 5 && i + listOffset < static_cast<int>(extra.windows.size()); ++i) {
            auto item = extra.windows[i + listOffset];
            const auto b = box(29, 105 + i * 45.f, 380, 40);
            p.card(d, b, 12);
            hit(sheet, b, [this, target = item.handle] {
                if (!IsWindow(target)) { if (refreshOpenWindows() == 2) position(); else paintSheet(); return; }
                close();
                activateWindow(target, false);
            }, [this, target = item.handle] { showWindowMenu(target); });
            const float over = sheet.hoverAmount();
            if (over > 0) d.fill(b, 12, tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .07f : .10f) * over));
            if (item.icon) d.image(item.icon, box(38, b.top + 8, 24, 24), p.light);
            else d.icon(Layers, box(42, b.top + 12, 16, 16), tint(color));
            d.text(item.title, box(70, b.top + 4, 307, 32), Body, tint(p.ink));
            d.icon(ChevronRight, box(380, b.top + 12, 15, 15), tint(p.muted));
        }
        if (extra.windows.empty()) d.text(L"Açık pencere bulunamadı.", box(29, 105, 380, 180), Body, tint(p.muted), Center);
        action(29, 338, 380, L"Listeyi yenile", [this] { if (refreshOpenWindows() == 2) position(); else paintSheet(); });
    } else if (page == Quick) {
        const bool linked = extra.wifiConnected || !extra.radios.network.empty();
        const Text networkName = extra.radios.network.empty() ? (linked ? L"Bağlı" : L"Bağlı değil") : extra.radios.network;
        const auto rowHoverColor = [&](float hot) {
            return tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .075f : .12f) * hot);
        };
        if (extra.quickSub != QuickSubPanel::Main) {
            const float cT = sheetContentProgress();
            const auto subMotion = d.pushMotion(box(12, 12, sheet.w - 24, sheet.h - 24),
                cT < 1.f ? (0.15f + 0.85f * cT) : 1.f, sheetContentDx * (1.f - cT), sheetContentDy * (1.f - cT));

            if (extra.quickSub == QuickSubPanel::Wifi) {
                heading(p, L"Wi-Fi", networkName);
                p.card(d, box(29, 87, 380, 43), 12);
                d.icon(Wifi, box(43, 98, 20, 20), tint(linked ? p.accent : p.muted));
                d.text(L"Wi-Fi", box(74, 92, 143, 33), Strong, tint(p.ink));
                if (extra.radios.wifi >= 0)
                    knob(p, box(345, 97, 45, 22), extra.radios.wifi == 1, [this] { queryRadios(1); });

                const bool locBlocked = extra.wifiError == ERROR_ACCESS_DENIED;
                if (locBlocked && extra.wifi.empty() && !extra.wifiScanning) {
                    const auto warnCard = box(29, 138, 380, 82);
                    p.card(d, warnCard, 12);
                    d.text(L"Yakındaki ağlar taranamıyor", box(43, 145, 352, 20), Strong, tint(p.ink));
                    d.text(L"Windows 11 konum izni kapalı olduğu için canlı ağ listesi alınamıyor.",
                        box(43, 165, 352, 16), Small, tint(p.muted));
                    button(sheet, p, box(43, 186, 170, 26), L"Konum iznini aç", [this] { launch(L"ms-settings:privacy-location"); }, true);
                    button(sheet, p, box(223, 186, 172, 26), L"Windows Wi-Fi paneli", [this] { launch(L"ms-availablenetworks:"); });

                    if (!extra.savedWifi.empty()) {
                        d.text(L"Kayıtlı profiller (canlı tarama dışı)", box(32, 226, 360, 18), Small, tint(p.muted));
                        constexpr int savedRows = 2;
                        listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.savedWifi.size()) - savedRows));
                        for (int i = 0; i < savedRows && i + listOffset < static_cast<int>(extra.savedWifi.size()); ++i) {
                            const size_t index = i + listOffset;
                            const auto& network = extra.savedWifi[index];
                            auto item = box(29, 248 + i * 44.f, 380, 40);
                            p.card(d, item, 12);
                            hit(sheet, item, [this, index] { connectWifi(index, true); });
                            const float hot = sheet.hoverAmount();
                            if (hot > 0) d.fill(item, 12, tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .07f : .12f) * hot));
                            d.icon(Wifi, box(41, item.top + 10, 19, 19), tint(network.connected ? p.accent : p.muted));
                            d.text(network.name, box(69, item.top + 1, 220, 21), Strong, tint(p.ink));
                            d.text(network.connected ? L"Bağlı" : L"Kayıtlı profil", box(70, item.top + 22, 220, 14), Small, tint(p.muted));
                            d.text(network.connected ? L"Kes" : L"Bağlan", box(316, item.top, 78, 40), Caption, tint(p.accent), Right);
                        }
                    }
                } else {
                    if (locBlocked) {
                        d.text(L"Tarama için konum izni kapalı.", box(32, 135, 232, 28), Caption, tint(p.muted));
                        button(sheet, p, box(278, 135, 131, 27), L"Konum ayarları", [this] { launch(L"ms-settings:privacy-location"); });
                    } else {
                        d.text(extra.wifiScanning ? L"Yakındaki ağlar taranıyor…" : L"Kullanılabilir yakındaki ağlar",
                            box(32, 135, 360, 28), Caption, tint(p.muted));
                    }
                    constexpr int rows = 4;
                    listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.wifi.size()) - rows));
                    for (int i = 0; i < rows && i + listOffset < static_cast<int>(extra.wifi.size()); ++i) {
                        const size_t index = i + listOffset;
                        const auto& network = extra.wifi[index];
                        auto item = box(29, 165 + i * 42.f, 380, 39);
                        p.card(d, item, 12);
                        hit(sheet, item, [this, index] { connectWifi(index, false); });
                        const float hot = sheet.hoverAmount();
                        if (hot > 0) d.fill(item, 12, tint(p.glass ? (p.light ? 0x10141A : 0xFFFFFF) : p.accent, (p.glass ? .07f : .12f) * hot));
                        d.icon(Wifi, box(41, item.top + 10, 19, 19), tint(network.connected ? p.accent : p.muted));
                        d.text(network.name, box(69, item.top, 226, 22), Strong, tint(p.ink));
                        Text status = network.connected
                            ? (network.signal >= 0 ? L"Bağlı · %" + num(network.signal) : L"Bağlı")
                            : (!network.profile.empty() ? (network.secure ? L"Kayıtlı · Parola korumalı" : L"Kayıtlı · Açık ağ")
                                : (network.secure ? L"Parola korumalı" : L"Açık ağ"));
                        d.text(status, box(70, item.top + 21, 224, 15), Small, tint(p.muted));
                        if (network.connected) d.text(L"Kes", box(318, item.top, 74, 39), Caption, tint(p.accent), Right);
                        else if (network.signal >= 0) d.text(num(network.signal) + L"%", box(318, item.top, 74, 39), Caption, tint(p.muted), Right);
                    }
                    if (extra.wifi.empty()) {
                        d.text(extra.radios.wifi == 0 ? L"Wi-Fi kapalı"
                            : extra.wifiScanning ? L"Yakındaki ağlar aranıyor…"
                            : L"Yakında kullanılabilir ağ bulunamadı",
                            box(29, 192, 380, 95), Body, tint(p.muted), Center);
                    }
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 125, extra.wifiScanning ? L"Taranıyor…" : L"Yenile", [this] { refreshWifi(true, true); queryRadios(); paintSheet(); });
                action(277, 343, 132, L"Windows ağları", [this] { launch(L"ms-availablenetworks:"); });
            } else if (extra.quickSub == QuickSubPanel::Bluetooth) {
                int connectedCount = 0;
                for (const auto& dev : extra.btDevices) if (dev.connected) ++connectedCount;
                const Text btDetail = extra.radios.bluetooth == 0 ? L"Kapalı"
                    : connectedCount > 0 ? num(connectedCount) + L" aygıt bağlı" : L"Açık · Eşleştirilmiş aygıtlar";
                heading(p, L"Bluetooth", btDetail);
                p.card(d, box(29, 87, 380, 43), 12);
                d.icon(BluetoothIcon, box(43, 98, 20, 20), tint(extra.radios.bluetooth == 1 ? p.accent : p.muted));
                d.text(L"Bluetooth", box(74, 92, 143, 33), Strong, tint(p.ink));
                if (extra.radios.bluetooth >= 0)
                    knob(p, box(345, 97, 45, 22), extra.radios.bluetooth == 1, [this] { queryRadios(2); });

                d.text(extra.btLoading && extra.btDevices.empty() ? L"Aygıtlar taranıyor…" : L"Eşleştirilmiş aygıtlar",
                    box(32, 135, 360, 28), Caption, tint(p.muted));
                constexpr int rows = 4;
                listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.btDevices.size()) - rows));
                for (int i = 0; i < rows && i + listOffset < static_cast<int>(extra.btDevices.size()); ++i) {
                    const size_t index = i + listOffset;
                    const auto& dev = extra.btDevices[index];
                    auto item = box(29, 165 + i * 42.f, 380, 39);
                    p.card(d, item, 12);
                    hit(sheet, item, [this, index] { toggleBluetoothDevice(index); });
                    const float hot = sheet.hoverAmount();
                    if (hot > 0) d.fill(item, 12, rowHoverColor(hot));
                    d.icon(dev.isAudio ? Speaker : BluetoothIcon, box(41, item.top + 10, 19, 19),
                        tint(dev.connected ? p.accent : p.muted));
                    d.text(dev.name, box(69, item.top, 224, 22), Strong, tint(p.ink));
                    const bool busy = extra.btBusyAddress != 0 && extra.btBusyAddress == dev.address;
                    Text st = busy ? (extra.btBusyConnect ? L"Bağlanıyor…" : L"Kesiliyor…")
                        : dev.connected ? (dev.battery >= 0 ? L"Bağlı · %" + num(dev.battery)
                            : (dev.isAudio ? L"Bağlı · Ses" : L"Bağlı"))
                        : (dev.isAudio ? L"Eşleştirilmiş · Ses" : L"Eşleştirilmiş");
                    d.text(st, box(70, item.top + 21, 224, 15), Small, tint(dev.connected || busy ? p.accent : p.muted));
                    d.text(busy ? L"…" : (dev.connected ? L"Kes" : L"Bağlan"),
                        box(316, item.top, 76, 39), Caption, tint(dev.connected ? p.muted : p.accent), Right);
                }
                if (extra.btDevices.empty()) {
                    d.text(extra.radios.bluetooth == 0 ? L"Bluetooth kapalı"
                        : extra.btLoading ? L"Bluetooth aygıtları okunuyor…"
                        : L"Eşleştirilmiş Bluetooth aygıtı yok",
                        box(29, 192, 380, 95), Body, tint(p.muted), Center);
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 125, L"Yenile", [this] { queryBluetoothDevices(); queryRadios(); });
                action(277, 343, 132, L"Diğer ayarlar", [this] { launch(L"ms-settings:bluetooth"); });
            } else if (extra.quickSub == QuickSubPanel::Project) {
                heading(p, L"Yansıt", L"Çoklu ekran görünüm modu");
                struct ProjOpt { UINT32 topo, flag; const wchar_t* title; const wchar_t* desc; };
                const ProjOpt opts[] = {
                    {DISPLAYCONFIG_TOPOLOGY_INTERNAL, SDC_TOPOLOGY_INTERNAL, L"Yalnızca bilgisayar ekranı", L"Yalnızca ana ekranı kullan"},
                    {DISPLAYCONFIG_TOPOLOGY_CLONE,    SDC_TOPOLOGY_CLONE,    L"Yinele",                     L"Aynı görüntüyü tüm ekranlarda göster"},
                    {DISPLAYCONFIG_TOPOLOGY_EXTEND,   SDC_TOPOLOGY_EXTEND,   L"Uzat",                       L"Masaüstünü ekranlara genişlet"},
                    {DISPLAYCONFIG_TOPOLOGY_EXTERNAL, SDC_TOPOLOGY_EXTERNAL, L"Yalnızca ikinci ekran",      L"Yalnızca harici ekranı kullan"},
                };
                for (int i = 0; i < 4; ++i) {
                    const auto& o = opts[i];
                    const bool active = extra.displayTopology == o.topo;
                    auto item = box(29, 98 + i * 58.f, 380, 50);
                    p.card(d, item, 12);
                    hit(sheet, item, [this, flag = o.flag] { setDisplayTopology(flag); });
                    const float hot = sheet.hoverAmount();
                    if (active) {
                        d.fill(item, 12, tint(p.accent, p.glass ? .22f : .14f));
                        d.outline({item.left + .5f, item.top + .5f, item.right - .5f, item.bottom - .5f}, 11.5f, tint(p.accent, .65f));
                    } else if (hot > 0) {
                        d.fill(item, 12, rowHoverColor(hot));
                    }
                    d.icon(Layers, box(43, item.top + 15, 20, 20), tint(active ? p.accent : p.muted));
                    d.text(o.title, box(74, item.top + 5, 270, 22), Strong, tint(p.ink));
                    d.text(o.desc, box(74, item.top + 27, 270, 16), Small, tint(p.muted));
                    if (active) d.icon(Check, box(374, item.top + 17, 18, 18), tint(p.accent));
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 125, L"Kablosuz ekran", [this] { systemShortcut('K'); });
                action(277, 343, 132, L"Ekran ayarları", [this] { launch(L"ms-settings:display"); });
            } else if (extra.quickSub == QuickSubPanel::Accessibility) {
                heading(p, L"Erişilebilirlik", L"Hızlı erişilebilirlik anahtarları");
                struct AccOpt { int id; const wchar_t* title; const wchar_t* desc; bool on; };
                const AccOpt opts[] = {
                    {0, L"Büyüteç",          L"Ekranı yakınlaştır",                 extra.accessibility.magnifier},
                    {1, L"Renk filtreleri",  L"Renk körlüğü ve kontrast filtresi",  extra.accessibility.colorFilter},
                    {2, L"Ekran Okuyucusu",  L"Ekrandaki metinleri seslendir",      extra.accessibility.narrator},
                    {3, L"Canlı alt yazılar",L"Konuşulan sesi anlık yazıya dök",    extra.accessibility.liveCaptions},
                    {4, L"Yapışkan Tuşlar",  L"Kısayol tuşlarını tek tek bas",      extra.accessibility.stickyKeys},
                    {5, L"Yüksek karşıtlık", L"Yüksek kontrastlı renk teması",      extra.accessibility.highContrast},
                    {6, L"Mono ses",         L"Sol ve sağ ses kanallarını birleştir", extra.accessibility.monoAudio},
                };
                constexpr int rows = 5;
                listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(ARRAYSIZE(opts)) - rows));
                for (int i = 0; i < rows && i + listOffset < static_cast<int>(ARRAYSIZE(opts)); ++i) {
                    const auto& o = opts[i + listOffset];
                    auto item = box(29, 96 + i * 47.f, 380, 41);
                    p.card(d, item, 12);
                    hit(sheet, box(item.left, item.top, width(item) - 64, height(item)), [this, id = o.id] { toggleAccessibility(id); });
                    const float hot = sheet.hoverAmount();
                    if (hot > 0) d.fill(item, 12, rowHoverColor(hot));
                    d.text(o.title, box(43, item.top + 3, 276, 20), Strong, tint(p.ink));
                    d.text(o.desc, box(43, item.top + 22, 276, 15), Small, tint(p.muted));
                    knob(p, box(345, item.top + 10, 45, 22), o.on, [this, id = o.id] { toggleAccessibility(id); });
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 266, L"Diğer erişilebilirlik ayarları", [this] { launch(L"ms-settings:easeofaccess"); });
            } else if (extra.quickSub == QuickSubPanel::NearbyShare) {
                heading(p, L"Yakın paylaşım", L"Yakındaki cihazlarla paylaşım kapsamı");
                struct ShareOpt { int mode; const wchar_t* title; const wchar_t* desc; };
                const ShareOpt opts[] = {
                    {0, L"Kapalı",                   L"Yakın paylaşım devre dışı"},
                    {1, L"Yalnızca kendi cihazlarım",L"Aynı Microsoft hesabındaki cihazlar"},
                    {2, L"Yakındaki herkes",         L"Yakındaki tüm Windows cihazları"},
                };
                for (int i = 0; i < 3; ++i) {
                    const auto& o = opts[i];
                    const bool active = extra.nearbyShare == o.mode;
                    auto item = box(29, 106 + i * 66.f, 380, 56);
                    p.card(d, item, 12);
                    hit(sheet, item, [this, mode = o.mode] { setNearbyShare(mode); });
                    const float hot = sheet.hoverAmount();
                    if (active) {
                        d.fill(item, 12, tint(p.accent, p.glass ? .22f : .14f));
                        d.outline({item.left + .5f, item.top + .5f, item.right - .5f, item.bottom - .5f}, 11.5f, tint(p.accent, .65f));
                    } else if (hot > 0) {
                        d.fill(item, 12, rowHoverColor(hot));
                    }
                    d.icon(Link, box(43, item.top + 18, 20, 20), tint(active ? p.accent : p.muted));
                    d.text(o.title, box(74, item.top + 8, 270, 22), Strong, tint(p.ink));
                    d.text(o.desc, box(74, item.top + 30, 270, 16), Small, tint(p.muted));
                    if (active) d.icon(Check, box(374, item.top + 19, 18, 18), tint(p.accent));
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 266, L"Yakın paylaşım ayarları", [this] { launch(L"ms-settings:crossdevice"); });
            } else if (extra.quickSub == QuickSubPanel::AudioOutput) {
                heading(p, L"Ses çıkışı", L"Varsayılan oynatma aygıtı");
                constexpr int rows = 5;
                listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(extra.audioDevices.size()) - rows));
                for (int i = 0; i < rows && i + listOffset < static_cast<int>(extra.audioDevices.size()); ++i) {
                    const auto& dev = extra.audioDevices[i + listOffset];
                    auto item = box(29, 96 + i * 47.f, 380, 41);
                    p.card(d, item, 12);
                    hit(sheet, item, [this, id = dev.id] { setAudioEndpoint(id); });
                    const float hot = sheet.hoverAmount();
                    if (dev.active) {
                        d.fill(item, 12, tint(p.accent, p.glass ? .22f : .14f));
                        d.outline({item.left + .5f, item.top + .5f, item.right - .5f, item.bottom - .5f}, 11.5f, tint(p.accent, .65f));
                    } else if (hot > 0) {
                        d.fill(item, 12, rowHoverColor(hot));
                    }
                    d.icon(Speaker, box(42, item.top + 11, 19, 19), tint(dev.active ? p.accent : p.muted));
                    d.text(dev.name, box(70, item.top + 4, 290, 20), Strong, tint(p.ink));
                    d.text(dev.active ? L"Etkin çıkış aygıtı" : L"Seçmek için tıkla", box(70, item.top + 23, 270, 14), Small, tint(p.muted));
                    if (dev.active) d.icon(Check, box(374, item.top + 12, 18, 18), tint(p.accent));
                }
                if (extra.audioDevices.empty()) {
                    d.text(L"Etkin ses çıkış aygıtı bulunamadı.", box(29, 150, 380, 120), Body, tint(p.muted), Center);
                }
                action(29, 343, 105, L"‹  Geri", [this] { closeQuickSub(); });
                action(143, 343, 125, L"Yenile", [this] { refreshAudioEndpoints(); paintSheet(); });
                action(277, 343, 132, L"Ses ayarları", [this] { launch(L"ms-settings:sound"); });
            }

            d.popMotion(subMotion);
            return;
        }

        const bool hasMedia = !extra.media.title.empty();
        const float yOff = hasMedia ? 70.f : 0.f;
        heading(p, L"Denetim Merkezi", linked ? networkName : extra.radios.ethernet ? L"Ethernet bağlı" : L"Bağlantılar ve aygıtlar");
        if (hasMedia) {
            const auto mediaCard = box(29, 86, 380, 62);
            p.card(d, mediaCard, 14);
            const auto artBox = box(39, 95, 44, 44);
            if (extra.media.cover) {
                d.clip(artBox);
                d.image(extra.media.cover, artBox, p.light);
                d.unclip();
                d.outline(artBox, 8, tint(p.ink, .14f), 1.f);
            } else {
                badge(d, artBox, Music, p.accent);
            }
            d.text(extra.media.title, box(93, 95, 196, 21), Strong, tint(p.ink));
            const Text stateLabel = extra.media.playing ? L"Oynatılıyor" : L"Duraklatıldı";
            const Text sub = extra.media.artist.empty() ? stateLabel : (extra.media.artist + L" · " + stateLabel);
            d.text(sub, box(93, 118, 196, 17), Small, tint(extra.media.playing ? (p.light ? 0x0A7AFF : 0x5BB6FF) : p.muted));
            iconButton(sheet, p, box(297, 102, 30, 30), Back, [this] { media(VK_MEDIA_PREV_TRACK); });
            iconButton(sheet, p, box(331, 102, 34, 30), extra.media.playing ? Pause : Play,
                [this] { media(VK_MEDIA_PLAY_PAUSE); }, extra.media.playing);
            iconButton(sheet, p, box(369, 102, 30, 30), Next, [this] { media(VK_MEDIA_NEXT_TRACK); });
        } else {
            iconButton(sheet, p, box(269, 55, 29, 26), Back, [this] { media(VK_MEDIA_PREV_TRACK); });
            iconButton(sheet, p, box(303, 55, 29, 26), extra.media.playing ? Pause : Play, [this] { media(VK_MEDIA_PLAY_PAUSE); });
            iconButton(sheet, p, box(337, 55, 29, 26), Next, [this] { media(VK_MEDIA_NEXT_TRACK); });
        }
        const float qT = sheetContentProgress();
        const auto qMotion = d.pushMotion(box(20, 88 + yOff, sheet.w - 40, 166),
            qT < 1.f ? (0.15f + 0.85f * qT) : 1.f, sheetContentDx * (1.f - qT), sheetContentDy * (1.f - qT));
        const auto tile = [&](int index, const Text& label, const Text& status, Glyph glyph,
                              bool enabled, std::function<void()> primary, std::function<void()> details = {}, bool subOnly = false) {
            const float x = 29.f + 128.f * (index % 3), y = 94.f + yOff + 79.f * (index / 3);
            const auto base = box(x, y, 121, 44);
            hit(sheet, details ? box(x, y, 79, 44) : base, std::move(primary));
            const float over = sheet.hoverAmount();
            const float down = sheet.pressAmount();
            D2D1::Matrix3x2F tp{};
            if (down > 0.005f) {
                d.target->GetTransform(&tp);
                const float s = 1.f - 0.035f * down;
                d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {x + 60.5f, y + 22.f}) * tp);
            }
            const bool glassTile = p.glass && p.theme == LiquidGlass;
            const bool customAccent = model.accent > 0;
            if (glassTile) {
                if (enabled) {
                    const unsigned tileBlue = customAccent ? p.action : 0x2D8CFF;
                    d.fill(base, 14, tint(tileBlue, .30f + .08f * over));
                    d.glassRim(base, 14, .46f + .10f * over);
                } else {
                    d.fill(base, 14, tint(0x08090B, .28f));
                    if (over > 0) d.fill(base, 14, tint(0xFFFFFF, .06f * over));
                    d.glassRim(base, 14, .28f + .14f * over);
                }
            } else {
                const auto color = enabled ? p.action : p.high;
                d.fill(base, 15, tint(color, enabled ? (1.f - .12f * over) : (.78f + .10f * over)));
                d.outline({base.left + .5f, base.top + .5f, base.right - .5f, base.bottom - .5f}, 14.5f,
                    tint(p.ink, .16f + .10f * over));
            }
            const auto foreground = tint(enabled && !glassTile ? p.activeInk : p.ink);
            const float iconX = details ? (x + 29.5f) : (x + 50.5f);
            d.icon(glyph, box(iconX, y + 12, 20, 20), foreground);
            if (details) {
                const auto button = box(x + 79, y, 42, 44);
                hit(sheet, button, std::move(details));
                const float detHot = sheet.hoverAmount();
                if (detHot > 0) d.fill(button, 12, tint(enabled && !glassTile ? p.activeInk : p.ink, .12f * detHot));
                d.line(x + 79, y + 8, x + 79, y + 36, tint(enabled && !glassTile ? p.activeInk : p.ink, .14f));
                d.icon(ChevronRight, box(x + 94, y + 15, 13, 13), foreground);
            } else if (subOnly) {
                d.icon(ChevronRight, box(x + 96, y + 15, 13, 13), tint(enabled && !glassTile ? p.activeInk : p.muted));
            }
            if (down > 0.005f) d.target->SetTransform(tp);
            d.text(label, box(x + 2, y + 45, 117, 17), Caption, tint(p.ink), Center);
            if (!status.empty()) d.text(status, box(x + 2, y + 61, 117, 11), Small, tint(p.muted), Center);
        };
        if (quickPage == 0) {
            const bool wifiOn = extra.radios.wifi == 1 || (extra.radios.wifi < 0 && extra.wifiConnected);
            const Text wifiStatus = extra.radios.wifi == 0 ? L"Kapalı"
                : linked ? networkName : L"Bağlı değil";
            tile(0, L"Wi-Fi", wifiStatus, Wifi, wifiOn,
                [this] { if (extra.radios.wifi < 0) openQuickSub(QuickSubPanel::Wifi); else queryRadios(1); },
                [this] { openQuickSub(QuickSubPanel::Wifi); });
            const bool btOn = extra.radios.bluetooth == 1;
            Text btConnected;
            for (const auto& dev : extra.btDevices) {
                if (dev.connected) { btConnected = dev.name; break; }
            }
            const Text btStatus = extra.radios.bluetooth == 0 ? L"Kapalı"
                : btOn ? (btConnected.empty() ? L"Açık" : btConnected) : L"Yok";
            tile(1, L"Bluetooth", btStatus, BluetoothIcon, btOn,
                [this] { if (extra.radios.bluetooth < 0) openQuickSub(QuickSubPanel::Bluetooth); else queryRadios(2); },
                [this] { openQuickSub(QuickSubPanel::Bluetooth); });
            tile(2, L"Uçak modu", extra.radios.airplane ? L"Açık" : L"Kapalı", Plane, extra.radios.airplane,
                [this] { queryRadios(3); });
            tile(3, L"Enerji tasarrufu", extra.energySaver ? L"Açık" : L"Kapalı", Battery, extra.energySaver,
                [this] { toggleEnergySaver(); });
            const bool anyAcc = extra.accessibility.magnifier || extra.accessibility.colorFilter ||
                extra.accessibility.narrator || extra.accessibility.liveCaptions ||
                extra.accessibility.stickyKeys || extra.accessibility.highContrast || extra.accessibility.monoAudio;
            tile(4, L"Erişilebilirlik", anyAcc ? L"Etkin" : L"", Accessibility, anyAcc,
                [this] { openQuickSub(QuickSubPanel::Accessibility); }, {}, true);
            const Text projLabel = extra.displayTopology == DISPLAYCONFIG_TOPOLOGY_CLONE ? L"Yinele"
                : extra.displayTopology == DISPLAYCONFIG_TOPOLOGY_EXTEND ? L"Uzat"
                : extra.displayTopology == DISPLAYCONFIG_TOPOLOGY_EXTERNAL ? L"İkinci ekran"
                : L"Tek ekran";
            tile(5, L"Yansıt", projLabel, Layers, extra.displayTopology != DISPLAYCONFIG_TOPOLOGY_INTERNAL,
                [this] { openQuickSub(QuickSubPanel::Project); }, {}, true);
        } else {
            tile(0, L"Canlı alt yazı", extra.accessibility.liveCaptions ? L"Açık" : L"Kapalı", Document,
                extra.accessibility.liveCaptions, [this] { toggleAccessibility(3); });
            tile(1, L"Gece ışığı", extra.nightLight ? L"Açık" : L"Kapalı", Sun, extra.nightLight,
                [this] { toggleNightLight(); });
            const Text hsStatus = extra.radios.hotspot == 1
                ? (extra.radios.hotspotClients > 0 ? num(extra.radios.hotspotClients) + L" bağlı" : L"Açık")
                : extra.radios.hotspot == 2 ? L"…" : L"Kapalı";
            tile(2, L"Mobil etkin nokta", hsStatus, Link, extra.radios.hotspot == 1,
                [this] { queryRadios(4); });
            const Text nsStatus = extra.nearbyShare == 0 ? L"Kapalı" : extra.nearbyShare == 1 ? L"Cihazlarım" : L"Herkes";
            tile(3, L"Yakın paylaşım", nsStatus, Link, extra.nearbyShare > 0,
                [this] { openQuickSub(QuickSubPanel::NearbyShare); }, {}, true);
            tile(4, L"Kablosuz ekran", L"", Layers, false, [this] { systemShortcut('K'); });
            tile(5, L"Bildirimler", extra.dndEnabled ? L"Rahatsız etme" : extra.notifications.empty() ? L"" : num(extra.notifications.size()) + L" yeni",
                BellIcon, extra.dndEnabled, [this] { open(NotificationCenter, panelDockX); });
        }
        d.popMotion(qMotion);
        p.card(d, box(29, 257 + yOff, 380, 65), 14);
        const unsigned blueStart = p.light ? 0x0A7AFF : 0x2684FF;
        const unsigned blueEnd = p.light ? 0x3A9BFF : 0x5BB6FF;
        iconButton(sheet, p, box(37, 273 + yOff, 34, 31), Speaker, [this] { changeVolume(0, true); });
        const float qVol = barVolumeLevel >= 0.f ? std::clamp(barVolumeLevel, 0.f, 1.f)
            : (extra.muted ? 0.f : std::max(0, extra.volume) / 100.f);
        const Box volTrack = box(Design::QuickVolumeX, 283.5f + yOff, Design::QuickVolumeWidth, 11.f);
        quickVolumeTrack = volTrack;
        if (extra.volume >= 0) hit(sheet, box(volTrack.left - 4.f, 272.f + yOff, width(volTrack) + 8.f, 34.f), [] {});
        const float volHover = sheet.hoverAmount();
        d.fill(volTrack, 5.5f, tint(p.light ? 0xD8D8DC : 0xFFFFFF, p.light ? 1.f : .13f));
        const float fillW = width(volTrack) * qVol;
        if (fillW > 0.5f) {
            const float drawnW = std::max(height(volTrack), fillW);
            const Box fillBox = box(volTrack.left, volTrack.top, std::min(width(volTrack), drawnW), height(volTrack));
            d.gradient(fillBox, 5.5f, tint(blueStart), tint(blueEnd), true);
        }
        if (qVol > 0.005f || volumeDrag || volHover > 0.05f) {
            const float kx = std::clamp(volTrack.left + std::max(5.5f, fillW - 5.5f), volTrack.left + 5.5f, volTrack.right - 5.5f);
            const float thumbVis = volumeDrag ? 1.f : std::max(0.75f, volHover);
            d.circle(kx, volTrack.top + 5.5f, volumeDrag ? 4.3f : 3.6f, tint(0xFFFFFF, .96f * thumbVis));
        }
        const int shownVol = extra.volume < 0 ? -1 : static_cast<int>(std::lround(qVol * 100.f));
        d.text(extra.muted ? L"0%" : percentage(shownVol), box(332, 275 + yOff, 38, 28), Caption,
            tint(volumeDrag ? blueEnd : p.ink), Right);
        iconButton(sheet, p, box(374, 273 + yOff, 27, 30), ChevronRight, [this] { openQuickSub(QuickSubPanel::AudioOutput); });
        d.icon(Battery, box(33, 348 + yOff, 22, 18), tint(battery >= 0 ? 0x42CF81 : p.muted));
        d.text(percentage(battery), box(61, 344 + yOff, 52, 28), Caption, tint(p.muted));
        action(123, 342 + yOff, 112, quickPage == 0 ? L"Diğer denetimler ›" : L"‹ İlk sayfa",
            [this] { switchQuickPage(1 - quickPage); });
        action(247, 342 + yOff, 162, L"Windows denetimleri", [this] { showSystemQuick(); });
    } else if (page == Weather) {
        const auto& f = extra.forecast;
        heading(p, L"Hava durumu", model.weatherCity < 0 ? L"Tahmin için şehir seç" : Cities[model.weatherCity].name);
        p.card(d, box(29, 102, 380, 129));
        if (f) {
            d.text(degree(f->current), box(47, 117, 174, 69), Display, tint(p.ink));
            d.text(weatherText(f->code) + L"  ·  Hissedilen " + degree(f->feels), box(49, 187, 283, 26), Body, tint(p.muted));
            d.icon(weatherIcon(f->code, f->daylight), box(320, 132, 55, 55),
                tint(f->daylight && f->code == 0 ? (p.light ? 0x9A5B02 : 0xFFE2A1) : p.ink));
            for (size_t i = 0; i < f->days.size(); ++i) {
                const auto& day = f->days[i];
                auto b = box(29 + i * 96.f, 243, 91, 70);
                p.card(d, b, 12);
                d.text(i == 0 ? L"Bugün" : Days[Date::fromKey(day.day).weekday()], box(b.left + 3, b.top + 3, 85, 18), Caption, tint(p.muted), Center);
                d.icon(weatherIcon(day.code), box(b.left + 36, b.top + 24, 20, 20), tint(color));
                d.text(degree(day.high) + L"  /  " + degree(day.low), box(b.left + 2, b.top + 47, 87, 19), Body, tint(p.ink), Center);
            }
        } else {
            d.icon(Cloud, box(196, 119, 46, 46), tint(color));
            d.text(model.weatherCity < 0 ? L"Hava tahmini kapalı" : extra.weatherError ? L"Tahmin alınamadı" : L"Tahmin alınıyor…",
                box(38, 172, 362, 38), Strong, tint(p.ink), Center);
        }
        d.text(L"Open-Meteo" + (extra.weatherError && f ? L"  ·  Son alınan tahmin" : f ? L"  ·  " + f->time : Text()),
            box(29, 313, 378, 21), Caption, tint(p.muted));
        action(29, 339, 235, model.weatherCity < 0 ? L"Şehir seç" : L"Şehri değiştir",
            [this] { cityPicker = true; sheet.key = sheet.hover = -1; paintSheet(); }, true);
        if (model.weatherCity >= 0) action(274, 339, 135, L"Yenile", [&] { requestWeather(true); paintSheet(); });
    } else if (page == Usage) {
        const Date today = Date::from(now());
        const int oldest = today.add(1 - UsageHistoryDays).key();
        heading(p, L"Ekran süresi", L"Etkin kullanım");
        p.card(d, box(29, 93, 380, 175));
        button(sheet, p, box(233, 109, 74, 28), L"Bugün", [this] {
            if (usageWeek) {
                usageWeek = false; listOffset = 0; sheet.key = -1;
                if (motionEnabled()) { usageAnimMs = animNowMs(); startAnimTimer(); }
                paintSheet();
            }
        }, !usageWeek);
        button(sheet, p, box(311, 109, 80, 28), L"Son 7 gün", [this] {
            if (!usageWeek) {
                usageWeek = true; listOffset = 0; sheet.key = -1;
                if (motionEnabled()) { usageAnimMs = animNowMs(); startAnimTimer(); }
                paintSheet();
            }
        }, usageWeek);
        std::array<int, UsageHistoryDays> week{};
        int total = model.usageFor(today.key());
        if (usageWeek) {
            total = 0;
            for (int i = 0; i < UsageHistoryDays; ++i) {
                week[i] = model.usageFor(today.add(i + 1 - UsageHistoryDays).key());
                total += week[i];
            }
        }
        d.text(usageText(total), box(46, 103, 172, 43), Number, tint(p.ink));
        constexpr float baseline = 231.f, chartHeight = 73.f;
        const float chartGrow = usageAnimMs > 0.0
            ? easeOutQuint(static_cast<float>((animNowMs() - usageAnimMs) / 260.0)) : 1.f;
        const auto column = [&](float x, float span, float fraction) {
            d.fill(box(x, baseline - chartHeight, span, chartHeight), 3, tint(p.ink, .06f));
            const float grown = fraction * chartGrow;
            if (grown > 0.002f) {
                const float h = std::max(2.f, chartHeight * grown);
                d.fill(box(x, baseline - h, span, h), 3, tint(p.light ? 0x2B6FE8 : 0x62A8FF, .90f));
            }
        };
        if (usageWeek) {
            const int peak = std::max(3600, *std::max_element(week.begin(), week.end()));
            for (int i = 0; i < UsageHistoryDays; ++i) {
                const float x = 50 + i * 49.f;
                column(x, 39, week[i] / static_cast<float>(peak));
                d.text(ShortDays[today.add(i + 1 - UsageHistoryDays).weekday()],
                    box(x, 235, 39, 20), Small, tint(p.muted), Center);
            }
        } else {
            for (int i = 0; i < 24; ++i)
                column(49 + i * 14.f, 9, model.usageDay == today.key() ? model.usageSeconds[i] / 3600.f : 0.f);
            for (int hour : {0, 6, 12, 18})
                d.text(two(hour) + L":00", box(46 + hour * 14.f, 235, 47, 20), Small, tint(p.muted));
        }
        std::vector<UsageApp> summary;
        for (const auto& app : model.usageApps) {
            if (app.day < (usageWeek ? oldest : today.key()) || app.day > today.key()) continue;
            const auto found = std::find_if(summary.begin(), summary.end(), [&](const auto& item) {
                return CompareStringOrdinal(item.key.c_str(), -1, app.key.c_str(), -1, TRUE) == CSTR_EQUAL;
            });
            if (found == summary.end()) summary.push_back(app);
            else found->seconds += app.seconds;
        }
        std::stable_sort(summary.begin(), summary.end(), [](const auto& a, const auto& b) { return a.seconds > b.seconds; });
        d.text(L"Uygulamalar", box(30, 277, 200, 28), Strong, tint(p.ink));
        constexpr int rows = 5;
        const int count = static_cast<int>(summary.size());
        listOffset = std::clamp(listOffset, 0, std::max(0, count - rows));
        if (listOffset > 0) iconButton(sheet, p, box(348, 278, 27, 26), ChevronUp, [this] { --listOffset; paintSheet(); });
        if (listOffset + rows < count)
            iconButton(sheet, p, box(380, 278, 27, 26), ChevronDown, [this] { ++listOffset; paintSheet(); });
        for (int i = 0; i < rows && i + listOffset < count; ++i) {
            const auto& app = summary[i + listOffset];
            const float y = 312 + i * 39.f;
            p.card(d, box(29, y, 380, 34), 11);
            d.text(app.label, box(41, y, 246, 34), Body, tint(p.ink));
            d.text(app.seconds < 60 ? L"< 1 dk" : usageText(app.seconds),
                box(294, y, 102, 34), Caption, tint(p.muted), Right);
        }
        if (summary.empty()) d.text(L"Uygulama kullanımı henüz kaydedilmedi.", box(30, 320, 378, 75), Body, tint(p.muted), Center);
        d.text(L"Ölçümler bu bilgisayarda yedi gün saklanır.", box(29, 518, 380, 28), Caption, tint(p.muted), Center);
    }
}



