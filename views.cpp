#include "dock.h"
#include <cmath>
#include <cwctype>

static constexpr Glyph Glyphs[] = {
    FolderIcon, DateIcon, Timer, Check, Document, Chip, Branch, Bag, Trend, Music, Timer,
    Timer, Timer, Globe, DiskIcon, Wifi, Battery, Speaker, Layers, Sliders, Cloud, Activity,
    DateIcon, Sun, DateIcon, DateIcon, Timer, Cloud, Timer,
    WindIcon, Sun, Cloud, Timer, Sun, Trend, Trend, Trend, HistoryIcon, Trash, SearchIcon, Terminal, ScreenIcon,
    Timer, Chip, Layers, Sun, DateIcon, Activity, ScreenIcon,
    Activity, Trend, Activity, Branch, Trend, Chip, Globe,
    Activity, Activity, Activity, Activity, Activity, Activity, Terminal, Terminal, Activity, Layers, Activity,
    Activity, Link, Refresh, Music, PlaySolid, Activity, MicIcon, Sliders,
    ChevronUp, MicIcon, KeyboardIcon, EthernetIcon, Speaker, Battery, Timer, BellIcon, ScreenIcon
};
static_assert(ARRAYSIZE(Glyphs) == WidgetCount);
static constexpr Glyph ProfileIcons[] = {Terminal, Bag, Trend, Document};
static constexpr const wchar_t* ProfileDetails[] = {L"Repo ve sistem", L"Sipariş ve gelir", L"Trafik ve dönüşüm", L"Ders ve odak"};
static Box inset(Box b, float v) { return {b.left + v, b.top + v, b.right - v, b.bottom - v}; }
static Text percent(int p) { return p < 0 ? L"—" : num(p) + L"%"; }
bool Dock::hit(View& view, Box b, std::function<void()> action, std::function<void()> secondary) {
    int index = static_cast<int>(view.hits.size());
    view.hits.push_back({b, std::move(action), std::move(secondary), {}});
    return view.hover == index || view.key == index;
}
void Dock::badge(Draw& d, Box b, Glyph glyph, unsigned color) {
    d.fill(b, width(b) * .295f, tint(color, .18f));
    d.icon(glyph, inset(b, width(b) * .22f), tint(color));
}
void Dock::button(View& v, Palette p, Box b, const Text& label, std::function<void()> action, bool active, Font font) {
    auto& d = v.d;
    hit(v, b, std::move(action));
    const float over = v.hoverAmount();
    const float down = v.pressAmount();
    D2D1::Matrix3x2F prev{};
    if (down > 0.005f) {
        d.target->GetTransform(&prev);
        const float s = 1.f - 0.035f * down;
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {b.left + width(b) / 2.f, b.top + height(b) / 2.f}) * prev);
    }
    const float r = p.glass ? std::min(12.f, height(b) / 2.f) : height(b) / 2.f;
    if (p.glass) {
        if (p.theme == LiquidGlass) {
            const bool customAccent = model.accent > 0;
            if (active) {
                d.fill(b, r, tint(customAccent ? p.action : 0xFFFFFF, (customAccent ? .24f : .13f) + .05f * over));
                d.glassRim(b, r, .40f + .10f * over);
            } else {
                d.fill(b, r, tint(0x08090B, .28f));
                if (over > 0) d.fill(b, r, tint(0xFFFFFF, .06f * over));
                d.glassRim(b, r, .28f + .14f * over);
            }
            d.text(label, inset(b, 4), font, tint(p.ink), Center);
            if (down > 0.005f) d.target->SetTransform(prev);
            return;
        }
        d.fill(b, r, tint(active ? p.action : p.high, (active ? .26f : .24f) + .08f * over));
        d.glassRim(b, r, (active ? .42f : .28f) + .12f * over);
        d.text(label, inset(b, 4), font, tint(p.ink), Center);
        if (down > 0.005f) d.target->SetTransform(prev);
        return;
    }
    if (active) {
        d.fill(b, r, tint(mixRgb(p.action, p.base, over * .12f)));
        d.text(label, inset(b, 3), font, tint(p.activeInk), Center);
    } else {
        d.fill(b, r, tint(p.ink, .065f + .075f * over));
        d.text(label, inset(b, 4), font, tint(p.ink), Center);
    }
    if (down > 0.005f) d.target->SetTransform(prev);
}
void Dock::iconButton(View& v, Palette p, Box b, Glyph icon, std::function<void()> action, bool active) {
    auto& d = v.d;
    hit(v, b, std::move(action));
    const float over = v.hoverAmount();
    const float down = v.pressAmount();
    D2D1::Matrix3x2F prev{};
    if (down > 0.005f) {
        d.target->GetTransform(&prev);
        const float s = 1.f - 0.07f * down;
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {b.left + width(b) / 2.f, b.top + height(b) / 2.f}) * prev);
    }
    if (p.glass) {
        const float r = std::min({11.f, width(b) / 2.f, height(b) / 2.f});
        const bool customAccent = model.accent > 0;
        if (active) {
            d.fill(b, r, tint(customAccent ? p.action : 0xFFFFFF, (customAccent ? .26f : .15f) + .05f * over));
            d.glassRim(b, r, .42f + .10f * over);
        } else {
            d.fill(b, r, tint(p.theme == LiquidGlass ? 0x08090B : p.base, .26f));
            if (over > 0) d.fill(b, r, tint(0xFFFFFF, .07f * over));
            d.glassRim(b, r, .26f + .16f * over);
        }
        const float size = std::min({width(b), height(b), 22.0f}) - 5.f;
        d.icon(icon, box(b.left + (width(b) - size) / 2, b.top + (height(b) - size) / 2, size, size), tint(p.ink));
        if (down > 0.005f) d.target->SetTransform(prev);
        return;
    }
    const float radius = std::min(width(b), height(b)) / 2.f;
    if (active) d.fill(b, radius, tint(p.action, 1.f - .13f * over));
    else if (over > 0 || icon == Close)
        d.fill(b, radius, tint(p.ink, .12f * over + (icon == Close ? .08f : 0.f)));
    float size = std::min({width(b), height(b), 22.0f}) - 4;
    d.icon(icon, box(b.left + (width(b) - size) / 2, b.top + (height(b) - size) / 2, size, size), tint(active ? p.activeInk : p.ink));
    if (down > 0.005f) d.target->SetTransform(prev);
}
void Dock::knob(Palette p, Box b, bool enabled, std::function<void()> action) {
    auto& d = sheet.d;
    const int index = static_cast<int>(sheet.hits.size());
    hit(sheet, b, [this, index, enabled, action = std::move(action)] {
        beginSwitch(index, enabled);
        action();
    });
    const float over = sheet.hoverAmount();
    float position = enabled ? 1.f : 0.f;
    float stretch = 0.f;
    if (switchAtMs > 0.0 && index == switchHit && switchWasOn != enabled) {
        const float t = std::clamp(static_cast<float>((animNowMs() - switchAtMs) / 180.0), 0.f, 1.f);
        const float eased = easeOutQuint(t);
        position = enabled ? eased : 1.f - eased;
        stretch = std::sin(t * 3.14159265f) * 3.2f;
    }
    const float half = height(b) / 2;
    d.fill(b, half, tint(p.ink, .12f + .12f * over));
    if (position > 0) d.fill(b, half, tint(p.action, position));
    d.outline(inset(b, .5f), half - .5f, tint(over > 0 ? p.accent : p.line, .20f + .52f * over), .8f);
    const float r = height(b) * .365f;
    const float cx = b.left + half + (width(b) - 2 * half) * position;
    const Color thumbCol = tint(mixRgb(p.light ? 0xFFFFFF : 0xBEBEBE, p.activeInk, std::clamp((position - .45f) * 2.2f, 0.f, 1.f)));
    if (stretch > 0.25f) {
        d.fill(box(cx - r - stretch * .5f, b.top + half - r, r * 2.f + stretch, r * 2.f), r, thumbCol);
    } else {
        d.circle(cx, b.top + half, r, thumbCol);
    }
}
std::array<long long, 7> Dock::history(int widget) const {
    std::array<long long, 7> values{};
    auto today = Date::from(now());
    for (int i = 0; i < 7; ++i) values[i] = model.total(widget, today.add(i - 6))[0];
    return values;
}
void Dock::spark(Draw& draw, Box b, const std::array<long long, 7>& values, unsigned color, bool area) {
    draw.graph(b, values, color, area);
}

void Dock::paintBar(const POINT* origin, bool repaintOtherDocks) {
    if (!hidden || origin) paintBarView(bar, folded, origin);
    if (!repaintOtherDocks) return;
    for (auto& entry : otherDocks) {
        if (!entry->hidden && entry->view.w > 0)
            paintBarView(entry->view, entry->folded);
    }
}
void Dock::paintBarView(View& view, const std::vector<OrderItem>& overflow, const POINT* origin) {
    const bool active = &view == &bar;
    auto& d = view.d;
    if (!view.hwnd || !d.begin(view.hwnd, static_cast<int>(std::lround(view.w * view.scale)),
        static_cast<int>(std::lround(view.h * view.scale)), view.scale)) return;
    view.hits.clear();
    if (active) barVolumeCard = barVolumeTrack = statusVolumeHit = statusKeyboardHit = {};
    Palette p(model);
    const auto body = d.snap(box(1, 1, view.w - 2, view.h - 2));
    p.surface(d, body, 22.f, false);
    auto studio = box(10, (view.h - 36.f) / 2.f, 36, 36);
    hit(view, studio, [this] { open(Studio, 28); });
    const float studioHot = view.hoverAmount();
    const float studioDown = view.pressAmount();
    if (p.theme == BareTheme) p.card(d, studio, 10);
    {
        D2D1::Matrix3x2F sp{};
        const float ls = 1.f + 0.04f * studioHot - 0.07f * studioDown;
        if (std::abs(ls - 1.f) > 0.003f) {
            d.target->GetTransform(&sp);
            d.target->SetTransform(D2D1::Matrix3x2F::Scale(ls, ls, {studio.left + 18.f, studio.top + 18.f}) * sp);
        }
        d.icon(Logo, d.snap(studio), tint(mixRgb(0xF3F6FC, 0xDCE8FA, studioHot)));
        if (std::abs(ls - 1.f) > 0.003f) d.target->SetTransform(sp);
    }
    if (view.placementRevision != dockRevision) {
        const auto& allItems = dockItems();
        auto& placement = view.placement;
        placement.clear();
        placement.reserve(allItems.size());
        float itemX = Design::DockLeading;
        for (const auto& item : allItems) {
            if (std::find(overflow.begin(), overflow.end(), item) != overflow.end()) continue;
            const float span = dockItemSpan(item);
            if (span <= 0.f) continue;
            if (!placement.empty()) itemX += dockGapBetween(placement.back().item, item);
            placement.push_back({item, itemX, span});
            itemX += span;
        }
        view.placementRevision = dockRevision;
    }
    const auto& placed = view.placement;
    float x = placed.empty() ? Design::DockLeading : (placed.back().x + placed.back().span + Design::Gap);
    const bool animActive = active && (dockOrderDragging || dockSettlingKey.kind != OrderKind::None || !dockVisualX.empty());
    int floatingIdx = -1;
    if (active && dockOrderDragging) {
        for (size_t i = 0; i < placed.size(); ++i)
            if (placed[i].item == dockOrderSource) { floatingIdx = static_cast<int>(i); break; }
    } else if (active && dockSettlingKey.kind != OrderKind::None) {
        for (size_t i = 0; i < placed.size(); ++i) {
            const auto key = dockItemKey(placed[i].item);
            if (key.kind == dockSettlingKey.kind &&
                ((key.kind == OrderKind::Widget && key.widget == dockSettlingKey.widget) ||
                 (key.kind == OrderKind::Shortcut && CompareStringOrdinal(key.appKey.c_str(), -1, dockSettlingKey.appKey.c_str(), -1, TRUE) == CSTR_EQUAL) ||
                 (key.kind == OrderKind::Window && key.chromeGroup == dockSettlingKey.chromeGroup && (key.chromeGroup || key.window == dockSettlingKey.window)))) {
                floatingIdx = static_cast<int>(i);
                break;
            }
        }
    }
    if (p.theme == BareTheme && !animActive) {
        const auto isApp = [](OrderKind k) { return k == OrderKind::Shortcut || k == OrderKind::Window; };
        for (size_t i = 0; i < placed.size(); ) {
            if (!isApp(placed[i].item.kind)) { ++i; continue; }
            size_t j = i;
            while (j + 1 < placed.size() && isApp(placed[j + 1].item.kind)) ++j;
            p.card(d, box(placed[i].x, Design::DockInset, (placed[j].x + placed[j].span) - placed[i].x, Design::CardHeight), Design::Radius);
            i = j + 1;
        }
    }
    for (size_t i = 0; i < placed.size(); ++i) {
        if (static_cast<int>(i) == floatingIdx) continue;
        const auto& cur = placed[i];
        float drawX = cur.x;
        if (animActive) findVisualX(dockItemKey(cur.item), drawX);
        if (!animActive && i > 0 && placed[i - 1].item.kind == OrderKind::Shortcut && cur.item.kind == OrderKind::Window) {
            const float divX = ((placed[i - 1].x + placed[i - 1].span) + cur.x) / 2.f;
            d.line(divX, Design::DockInset + 13.f, divX, Design::DockInset + Design::CardHeight - 13.f, tint(p.ink, .22f), 1.f);
        }
        const Box itemBox = box(drawX, Design::DockInset, cur.span, Design::CardHeight);
        if (p.theme == BareTheme && animActive && cur.item.kind != OrderKind::Widget)
            p.card(d, itemBox, 13.f);
        if (cur.item.kind == OrderKind::Widget) widgetCard(view, cur.item.index, itemBox, p);
        else drawAppSlot(view, cur.item, itemBox, p);
    }
    if (!overflow.empty()) {
        const auto b = box(x, Design::DockInset, Design::CardHeight - 6, Design::CardHeight);
        hit(view, b, [this, anchor = x + Design::CardHeight / 2.f] { openOverflow(anchor); });
        const float hot = view.hoverAmount();
        if (p.glass) {
            d.fill(b, Design::Radius, tint(p.theme == LiquidGlass ? 0x08090B : p.base, .22f));
            if (hot > 0) d.fill(b, Design::Radius, tint(0xFFFFFF, .055f * hot));
            d.glassRim(b, Design::Radius, .28f + .14f * hot);
        } else {
            if (p.theme == BareTheme) p.card(d, b, Design::Radius);
            else d.fill(b, Design::Radius, tint(p.high, .87f));
            if (hot > 0) d.fill(b, Design::Radius, tint(p.ink, .08f * hot));
        }
        for (int i = 0; i < 3; ++i) d.circle(x + 17 + i * 8.f, view.h / 2, 1.45f, tint(p.ink));
        x += Design::CardHeight;
    }
    if (dockEditMode) {
        const auto addBox = box(x, (view.h - 38.f) / 2.f, 36, 38);
        hit(view, addBox, [this, x] { tab = 1; open(Studio, x + 18); });
        const float addHot = view.hoverAmount();
        d.fill(addBox, 10, tint(p.accent, .18f + .12f * addHot));
        d.outline(inset(addBox, .5f), 9.5f, tint(p.accent, .50f), 1.f / d.scale);
        d.icon(Plus, inset(addBox, 9), tint(p.light ? p.action : p.ink));
        const auto doneBox = box(x + 42, (view.h - 38.f) / 2.f, 36, 38);
        hit(view, doneBox, [this] {
            dockEditMode = false;
            dockEditSpring.snap(0.f);
            KillTimer(hwnd, 4);
            position();
        });
        const float doneHot = view.hoverAmount();
        d.fill(doneBox, 10, tint(p.action, 1.f - .18f * doneHot));
        d.icon(Check, inset(doneBox, 9), tint(p.activeInk));
    }
    if (!notice.empty() && !panel && view.w > 250) {
        float nAlpha = 1.f;
        if (motionEnabled()) {
            const float inT = easeOutCubic(static_cast<float>((animNowMs() - noticeAtMs) / 180.0));
            const ULONGLONG left = noticeUntil > GetTickCount64() ? noticeUntil - GetTickCount64() : 0;
            const float outT = left < 300 ? static_cast<float>(left) / 300.f : 1.f;
            nAlpha = std::min(inT, outT);
        }
        auto b = box(view.w - 246, 12 + (1.f - nAlpha) * 4.f, 229, 40);
        d.fill(b, 12, tint(p.base, nAlpha));
        d.outline(b, 12, tint(p.accent, .45f * nAlpha));
        d.text(notice, inset(b, 8), Caption, tint(p.ink, nAlpha), Center);
    }
    if (floatingIdx >= 0) {
        const auto& fl = placed[floatingIdx];
        const float lift = std::clamp(dockDragLift, 0.f, 1.f);
        const Box dragBox = box(dockDragX, Design::DockInset, fl.span, Design::CardHeight);
        const bool isWidget = fl.item.kind == OrderKind::Widget;
        const float rad = Design::Radius;
        D2D1::Matrix3x2F prevTransform{};
        d.target->GetTransform(&prevTransform);
        const float scaleUp = 1.f + (isWidget ? 0.025f : 0.04f) * lift;
        const float liftY = -1.f * lift;
        const D2D1_POINT_2F pivot{dragBox.left + fl.span / 2.f, dragBox.top + Design::CardHeight / 2.f};
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(scaleUp, scaleUp, pivot) *
            D2D1::Matrix3x2F::Translation(0.f, liftY) * prevTransform);
        if (lift > 0.01f) {
            const Box shadowBox = isWidget ? dragBox : Box{dragBox.left + 2.f, dragBox.top, dragBox.right - 2.f, dragBox.bottom};
            d.fill(box(shadowBox.left, shadowBox.top + 3.f * lift, width(shadowBox), height(shadowBox)),
                rad, tint(0x000000, (p.light ? .12f : .20f) * lift));
        }
        if (isWidget) {
            if (lift > 0.01f && p.glass) {
                d.fill(dragBox, rad, tint(p.theme == LiquidGlass ? 0x08090B : p.base, .38f * lift));
                const Box inner = inset(dragBox, 4.f);
                d.fill(inner, 13.f, tint(0xFFFFFF, .06f * lift));
            }
            widgetCard(view, fl.item.index, dragBox, p);
            if (lift > 0.01f) {
                if (p.glass) d.glassRim(dragBox, rad, .44f * lift);
                else d.outline(inset(dragBox, .8f), rad, tint(p.accent, .55f * lift), 1.f / d.scale);
            }
        } else {
            if (p.theme == BareTheme && lift > 0.01f) p.card(d, dragBox, rad);
            drawAppSlot(view, fl.item, dragBox, p, lift);
        }
        d.target->SetTransform(prevTransform);
    }
    if (active && appDropHover) d.outline(inset(body, 1.5f), 21.f, tint(p.accent, .86f), 1.5f);
    d.end(origin);
}
void Dock::heading(Palette p, const Text& title, const Text& detail) {
    auto& d = sheet.d;
    d.text(title, box(32, 24, sheet.w - 120, 36), Title, tint(p.ink));
    d.text(detail, box(33, 61, sheet.w - 115, 18), Body, tint(p.muted));
    const float x = sheet.w - 62;
    iconButton(sheet, p, box(x, 29, 34, 34), Close, [&] { close(); });
}
void Dock::paintSheet(const POINT* origin) {
    auto& d = sheet.d;
    if (!panel || sheetClosing || !d.begin(panel, static_cast<int>(std::lround(sheet.w * sheet.scale)), static_cast<int>(std::lround(sheet.h * sheet.scale)), sheet.scale)) return;
    sheet.hits.clear();
    quickVolumeTrack = {};
    Palette p(model);
    const bool compact = page == ContextMenu || page == AppMenu || page == SystemTray || page == ChromeWindows || page == KeyboardLang || page == NotificationCenter || page == NetworkPanel || page == Volume;
    const auto surface = box(6, 4, sheet.w - 12, sheet.h - 12);
    if (page == Studio) {
        p.glass = false;
        p.rail = mixRgb(p.base, p.ink, p.light ? .035f : .045f);
        p.high = mixRgb(p.base, p.ink, p.light ? .065f : .105f);
        d.fill(surface, 20, tint(p.base));
        d.outline(inset(surface, .5f), 19.5f, tint(p.line, .26f));
    } else if (page == ChromeWindows && !p.glass) {
        d.fill(surface, 18, tint(p.base));
        d.outline(inset(surface, .5f), 17.5f, tint(p.line, .30f));
    } else p.surface(d, surface, compact ? 20.f : p.glass ? 22.f : 24.f);
    if (page == AppMenu || page == ContextMenu || page == SystemTray || page == Overflow || page == ChromeWindows || page == KeyboardLang || page == NotificationCenter || page == NetworkPanel || page == Volume) {
        const float bottom = model.edge == 0 ? sheet.h - 8 : 5;
        d.fill(box(sheetTail - 11, bottom - 1.f, 22, 2), 1.f, tint(p.accent, p.glass ? .32f : .68f));
    }
    switch (page) {
    case ContextMenu: contextMenu(p); break;
    case Overflow: overflowPanel(p); break;
    case ChromeWindows: chromeWindowsPanel(p); break;
    case KeyboardLang: keyboardPanel(p); break;
    case NotificationCenter: notificationPanel(p); break;
    case NetworkPanel: networkPanel(p); break;
    case Quick:
        extraPanel(p);
        break;
    case Studio:
        if (tab == 1) gallery(p);
        else studio(p);
        break;
    case SystemTray:
        systemTray(p);
        break;
    case Calendar: case Clock: calendar(p); break;
    case Focus: case Countdown: timerPanel(p); break;
    case Tasks: tasks(p); break;
    case Notes: notes(p); break;
    case DayTimeline: case WhatsApp: case Currency: productivityPanel(p); break;
    case System: system(p); break;
    case Git: git(p); break;
    case Sales: case Growth: metrics(p); break;
    case Media: playback(p); break;
    case MicControl: microphonePanel(p); break;
    case Apps: case AppMenu: case Recent: case RecycleBin: case Search: case Commands: case Capture:
        appPanel(p); break;
    case StockChart: case StockWatchlist: case Crypto: marketPanel(p); break;
    default:
        if (proWidget(page)) proPanel(p);
        else if (page >= Countdown && page < WidgetCount) extraPanel(p);
        break;
    }
    if (!notice.empty() && page != AppMenu && page != Apps && page != Focus && page != Countdown && page != ChromeWindows) {
        float nAlpha = 1.f;
        if (motionEnabled()) {
            const float inT = easeOutCubic(static_cast<float>((animNowMs() - noticeAtMs) / 180.0));
            const ULONGLONG left = noticeUntil > GetTickCount64() ? noticeUntil - GetTickCount64() : 0;
            const float outT = left < 300 ? static_cast<float>(left) / 300.f : 1.f;
            nAlpha = std::min(inT, outT);
        }
        auto b = box(28, sheet.h - 68 + (1.f - nAlpha) * 6.f, sheet.w - 56, 38);
        d.fill(b, 10, tint(p.base, nAlpha)); d.outline(b, 10, tint(p.accent, .6f * nAlpha));
        d.text(notice, inset(b, 6), Body, tint(p.ink, nAlpha), Center);
    }
    d.end(origin);
}
void Dock::contextMenu(Palette p) {
    auto& d = sheet.d;
    struct MenuRow { float y; const wchar_t* label; std::function<void()> run; bool destructive; };
    MenuRow rows[] = {
        {10, dockEditMode ? L"Düzenlemeyi bitir" : L"Dock’u düzenle", [this] {
            dockEditMode = !dockEditMode;
            if (dockEditMode) {
                dockEditSpring.snap(0.f);
                dockEditSpring.setTarget(1.f, motionEnabled());
                if (motionEnabled()) startAnimTimer();
            } else dockEditSpring.snap(0.f);
            close();
            if (dockEditMode) SetTimer(hwnd, 4, 50, nullptr);
            else KillTimer(hwnd, 4);
            position();
        }, false},
        {45, L"Widget’lar…", [this] { tab = 1; open(Studio); }, false},
        {80, L"Ayarlar", [this] { tab = 2; open(Studio); }, false},
        {123, L"Açık pencereler", [this] { open(OpenWindows); }, false},
        {158, L"Görev Yöneticisi", [this] { close(); launch(L"taskmgr.exe"); }, false},
        {194, L"Vetra’yı kapat", [this] { DestroyWindow(hwnd); }, true},
    };
    const int activeIdx = sheet.hover >= 0 ? sheet.hover : sheet.key;
    if (activeIdx >= 0 && activeIdx < static_cast<int>(ARRAYSIZE(rows))) {
        const float targetY = rows[activeIdx].y;
        if (!menuHoverY.initialized) menuHoverY.snap(targetY);
        else { menuHoverY.setTarget(targetY, motionEnabled()); if (motionEnabled()) startAnimTimer(); }
        menuHoverVisible = true;
    } else menuHoverVisible = false;
    if (menuHoverVisible && menuHoverY.initialized) {
        const Box pill = box(11, menuHoverY.value, 220, 33);
        if (p.glass) {
            d.fill(pill, 10, tint(p.light ? 0x10141A : 0xFFFFFF, .10f));
            d.glassRim(pill, 10, .30f);
        } else d.fill(pill, 10, tint(p.action));
    }
    d.line(22, 119, 215, 119, tint(0x646870, .22f));
    for (int i = 0; i < static_cast<int>(ARRAYSIZE(rows)); ++i) {
        const auto& item = rows[i];
        const auto row = box(11, item.y, 220, 33);
        hit(sheet, row, item.run);
        const float hot = sheet.hoverAmount();
        const unsigned inkColor = (!p.glass && i == activeIdx) ? p.activeInk :
            item.destructive ? (p.light ? 0xB52D39 : 0xFF7879) : p.ink;
        d.text(item.label, box(22 + 1.5f * hot, item.y, 195, 33), Body, tint(inkColor));
    }
}

void Dock::overflowPanel(Palette p) {
    heading(p, L"Diğer öğeler", num(folded.size()) + L" öğe");
    auto& d = sheet.d;
    constexpr int visible = 6;
    const int count = static_cast<int>(folded.size());
    listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
    const auto drawRowBg = [&](Box r, float hot) {
        if (p.glass) {
            p.card(d, r, 11);
            if (hot > 0) d.fill(r, 11, tint(p.light ? 0x10141A : 0xFFFFFF, .06f * hot));
        } else {
            d.fill(r, 12, tint(p.accent, .075f + .15f * hot));
        }
    };
    for (int i = 0; i < visible && i + listOffset < count; ++i) {
        const auto item = folded[i + listOffset];
        const float y = 88 + 40.f * i;
        const auto row = box(24, y, sheet.w - 48, 36);
        if (item.kind == OrderKind::Widget) {
            const int id = item.index;
            hit(sheet, row, [this, id] {
                if (dockControlWidget(id)) activateDockControl(id);
                else open(id);
            });
            drawRowBg(row, sheet.hoverAmount());
            d.icon(Glyphs[id], box(36, y + 9, 18, 18), tint(p.accent));
            d.text(Widgets[id].title, box(65, y, sheet.w - 129, 36), Body, tint(p.ink));
            d.icon(ChevronRight, box(sheet.w - 50, y + 10, 16, 16), tint(p.muted));
        } else if (item.kind == OrderKind::Shortcut && item.index >= 0 && static_cast<size_t>(item.index) < model.shortcuts.size()) {
            const auto app = model.shortcuts[item.index];
            const auto image = static_cast<size_t>(item.index) < appImages.size() ? appImages[item.index] : std::shared_ptr<Gdiplus::Bitmap>{};
            hit(sheet, row, [this, app] { close(); launchShortcut(app); },
                [this, app, image] { showAppMenu(app, -1, nullptr, image); });
            drawRowBg(row, sheet.hoverAmount());
            if (image) d.image(image, box(34, y + 7, 22, 22), p.light);
            else d.icon(Layers, box(36, y + 9, 18, 18), tint(p.accent));
            d.text(app.label, box(65, y, sheet.w - 104, 36), Body, tint(p.ink));
        } else if (item.kind == OrderKind::Window) {
            const auto found = std::find_if(extra.windows.begin(), extra.windows.end(),
                [&](const OpenWindow& w) { return w.handle == item.window; });
            if (found != extra.windows.end()) {
                const auto target = found->handle;
                hit(sheet, row, [this, target] { close(); activateWindow(target); },
                    [this, target] { showWindowMenu(target); });
                drawRowBg(row, sheet.hoverAmount());
                if (found->icon) d.image(found->icon, box(34, y + 7, 22, 22), p.light);
                else d.icon(Layers, box(36, y + 9, 18, 18), tint(p.accent));
                d.text(found->title, box(65, y, sheet.w - 104, 36), Body, tint(p.ink));
            }
        }
    }
    if (count > visible) {
        iconButton(sheet, p, box(sheet.w - 104, sheet.h - 35, 32, 24), ChevronUp,
            [this] { --listOffset; paintSheet(); });
        iconButton(sheet, p, box(sheet.w - 66, sheet.h - 35, 32, 24), ChevronDown,
            [this] { ++listOffset; paintSheet(); });
    }
}

void Dock::systemTray(Palette p) {
    auto& d = sheet.d;
    const bool glass = p.glass;
    trayRows.clear();
    const int count = static_cast<int>(trayIcons.size());

    const float contentT = sheetContentProgress();
    const auto motion = d.pushMotion(box(8, 6, sheet.w - 16, sheet.h - 14),
        contentT < 1.f ? (0.15f + 0.85f * contentT) : 1.f,
        sheetContentDx * (1.f - contentT), sheetContentDy * (1.f - contentT));

    if (trayMenuIconIndex >= 0 && trayMenuIconIndex < count) {
        const auto& app = trayIcons[trayMenuIconIndex];
        iconButton(sheet, p, box(12, 11, 26, 26), ChevronLeft, [this] { backTrayMenu(); });
        if (app.icon) d.image(app.icon, box(42, 14, 20, 20), p.light);
        else d.icon(Layers, box(43, 15, 18, 18), tint(p.muted));

        d.text(trayMenuTitle(), box(68, 12, sheet.w - 82, 24), Strong, tint(p.ink));
        d.line(18, 44, sheet.w - 18, 44, tint(p.line, glass ? (p.light ? .22f : .35f) : .65f));

        auto isExitLabel = [](const Text& s) {
            Text low;
            for (wchar_t ch : s) low.push_back(static_cast<wchar_t>(std::towlower(ch)));
            return low == L"exit" || low == L"quit" || low == L"kapat" ||
                low.find(L"çık") != Text::npos || low.find(L"exit ") == 0 || low.find(L"quit ") == 0;
        };

        float y = 50.f;
        if (trayMenuLoading) {
            d.text(L"Menü yükleniyor…", box(16, y + 6.f, sheet.w - 32.f, 26.f), Caption, tint(p.muted), Center);
            d.popMotion(motion);
            return;
        }
        if (trayMenuUnavailable) {
            d.text(L"Menü seçenekleri okunamadı.", box(20, y, sheet.w - 40.f, 20.f), Caption, tint(p.muted));
            y += 24.f;
        }
        const auto& menu = currentTrayMenu();
        const int itemCount = static_cast<int>(menu.size());
        const float maxY = sheet.h - 12.f;
        int maxOffset = 0;
        {
            float tailH = 0.f;
            int fit = 0;
            for (int k = itemCount - 1; k >= 0; --k) {
                const float h = menu[k].separator ? 9.f : 33.f;
                if (fit > 0 && tailH + h > maxY - y) break;
                tailH += h;
                ++fit;
            }
            maxOffset = std::max(0, itemCount - fit);
        }
        listOffset = std::clamp(listOffset, 0, maxOffset);
        for (int i = listOffset; i < itemCount; ++i) {
            const auto& item = menu[i];
            if (item.separator) {
                if (y + 9.f > maxY) break;
                d.line(20.f, y + 4.f, sheet.w - 20.f, y + 4.f, tint(p.line, glass ? (p.light ? .20f : .30f) : .55f));
                y += 9.f;
                continue;
            }
            if (y + 31.f > maxY + 2.f) break;
            const Box row = box(12, y, sheet.w - 24, 31);
            float hot = 0.f;
            const bool hasSub = !item.children.empty();
            if (!item.disabled) {
                if (!hasSub) {
                    hit(sheet, row, [this, item] { executeTrayMenuItem(item); });
                } else {
                    hit(sheet, row, [this, i] {
                        trayMenuPath.push_back(static_cast<size_t>(i));
                        listOffset = 0;
                        sheet.resetMotion();
                        menuHoverVisible = false;
                        menuHoverY.initialized = false;
                        triggerSheetContentAnim(16.f, 0.f);
                        positionSheet();
                    });
                }
                hot = sheet.hoverAmount();
            }
            if (hot > 0.f) {
                if (glass) {
                    d.fill(row, 10, tint(p.light ? 0x10141A : 0xFFFFFF, .10f * hot));
                    d.glassRim(row, 10, .30f * hot);
                } else d.fill(row, 9, tint(p.action, hot));
            }
            const Color ink = item.disabled ? tint(p.muted, .45f)
                : (!glass && hot > .6f) ? tint(p.activeInk)
                : isExitLabel(item.label) ? tint(p.light ? 0xB52D39 : 0xFF7879) : tint(p.ink);
            if (item.checked) d.icon(Check, box(19.f, y + 8.f, 14.f, 14.f), ink);
            if (hasSub) d.icon(ChevronRight, box(sheet.w - 31.f, y + 8.f, 14.f, 14.f), ink);
            const float textX = (item.checked ? 39.f : 22.f) + 1.2f * hot;
            d.text(item.label, box(textX, y, sheet.w - textX - (hasSub ? 35.f : 18.f), 31),
                item.isDefault ? Strong : Body, ink);
            y += 33.f;
        }
        d.popMotion(motion);
        return;
    }

    if (trayLoading && count == 0) {
        d.text(L"Yükleniyor…", box(16, 12, sheet.w - 32, sheet.h - 24), Caption, tint(p.muted), Center);
        d.popMotion(motion);
        return;
    }
    if (count == 0) {
        d.text(L"Açık gizli simge yok", box(16, 14, sheet.w - 32, 24), Caption, tint(p.muted), Center);
        button(sheet, p, box((sheet.w - 94.f) / 2.f, 44, 94, 28), L"Yenile", [this] { refreshSystemTray(true); });
        d.popMotion(motion);
        return;
    }

    const auto grid = trayGridLayout();
    constexpr float tile = Design::TrayTileSize, gap = Design::TrayTileGap, gridY = Design::TrayGridPadY;
    const int cols = grid.cols;
    const float gridW = cols * tile + (cols - 1) * gap;
    const float gridX = (sheet.w - gridW) / 2.f;
    const int maxOffset = grid.maxOffset;
    listOffset = std::clamp(listOffset, 0, maxOffset);
    const int visibleRows = grid.visibleRows;

    int hoveredItem = -1;
    for (int row = 0; row < visibleRows; ++row) {
        for (int col = 0; col < cols; ++col) {
            const int idx = (row + listOffset) * cols + col;
            if (idx >= count) break;
            const Box b = box(gridX + col * (tile + gap), gridY + row * (tile + gap), tile, tile);
            trayRows.push_back({b, idx});
            const bool over = hit(sheet, b, [this, idx] { activateTrayIcon(idx, false); });
            const float hoverVal = sheet.hoverAmount();
            const float pressVal = sheet.pressAmount();
            if (over) hoveredItem = idx;

            if (glass) {
                const float strength = (over || idx == trayMenuIconIndex) ? std::max(.5f, hoverVal) : hoverVal;
                d.fill(b, 10, tint(p.theme == LiquidGlass ? 0x08090B : p.base, .22f));
                if (strength > 0.f) d.fill(b, 10, tint(p.light ? 0x10141A : 0xFFFFFF, .08f * strength));
                d.glassRim(b, 10, .24f + .18f * strength);
            } else if (over || hoverVal > 0.f || idx == trayMenuIconIndex) {
                const float strength = (over || idx == trayMenuIconIndex) ? std::max(.35f, hoverVal) : hoverVal;
                d.fill(b, 9, tint(p.ink, .16f * strength));
                d.outline(inset(b, .45f), 8.5f, tint(p.ink, .18f * strength), .85f);
            }

            const float iconScale = 1.f + 0.09f * hoverVal - 0.08f * pressVal;
            const float iconLift = -1.f * hoverVal;
            const bool xform = std::abs(iconScale - 1.f) > 0.004f || std::abs(iconLift) > 0.05f;
            D2D1::Matrix3x2F prev{};
            if (xform) {
                d.target->GetTransform(&prev);
                d.target->SetTransform(D2D1::Matrix3x2F::Scale(iconScale, iconScale, {b.left + tile / 2.f, b.top + tile / 2.f}) *
                    D2D1::Matrix3x2F::Translation(0.f, iconLift) * prev);
            }
            if (trayIcons[idx].icon) {
                d.image(trayIcons[idx].icon, inset(b, 5.f), p.light);
            } else {
                d.icon(Layers, inset(b, 9.f), tint(p.muted));
            }
            if (xform) d.target->SetTransform(prev);
        }
    }

    const float tipY = gridY + visibleRows * tile + (visibleRows - 1) * gap + 5.f;
    if (hoveredItem >= 0 && hoveredItem < count) {
        d.text(trayIcons[hoveredItem].name, box(14, tipY, sheet.w - 28, 19), Caption, tint(p.ink), Center);
    } else {
        d.text(maxOffset > 0 ? L"Gizli simgeleri göster · Kaydır" : L"Gizli simgeleri göster",
            box(14, tipY, sheet.w - 28, 19), Small, tint(p.muted, .75f), Center);
    }
    d.popMotion(motion);
}
void Dock::gallery(Palette p) {
    auto& d = sheet.d;
    enum Category { All, Ai, Clocks, Work, DeviceTools, Applications, Audio, Desktop, Insights, Markets, Forecasts };
    struct CategoryInfo { const wchar_t* title; Glyph glyph; };
    constexpr CategoryInfo categories[] = {
        {L"Tüm widget’lar", Layers}, {L"Yapay zekâ", Activity}, {L"Saat ve tarih", Timer}, {L"Üretkenlik", Check},
        {L"Sistem", Chip}, {L"Uygulama ve dosya", FolderIcon}, {L"Medya ve ses", Music},
        {L"Masaüstü", Sliders}, {L"Analiz", Trend}, {L"Piyasalar", Activity}, {L"Hava durumu", Sun}
    };
    const auto categoryOf = [](int id) {
        if (Widgets[id].group == AiGroup) return Ai;
        if (id == Clock || id == World || id == AnalogClock || (id >= SevenDays && id <= FlipClock)) return Clocks;
        if (id == Apps || id == OpenWindows || Widgets[id].group == AppGroup) return Applications;
        if (mediaWidget(id) || id == Volume || id == MicControl) return Audio;
        switch (Widgets[id].group) {
        case Productivity: return Work;
        case Device: return DeviceTools;
        case Analytics: return Insights;
        case WeatherGroup: return Forecasts;
        case MarketGroup: return Markets;
        default: return Desktop;
        }
    };

    constexpr float start = 228, contentTop = 106, rowHeight = 91, sectionHeight = 24, gap = 16;
    const float end = sheet.w - 38, contentBottom = sheet.h - 57;
    d.fill(box(20, 19, 183, sheet.h - 46), 15, tint(p.rail));
    d.line(214, 29, 214, sheet.h - 35, tint(p.ink, .08f));

    const auto search = box(30, 31, 163, 35);
    hit(sheet, search, [this] { searchingCatalog = true; sheet.key = -1; paintSheet(); });
    d.fill(search, 10, tint(p.ink, .068f));
    if (searchingCatalog) d.outline(inset(search, .5f), 9.5f, tint(p.accent, .75f));
    d.icon(SearchIcon, box(39, 40, 16, 16), tint(p.muted));
    d.text(catalogSearch.empty() ? L"Widget ara…" : catalogSearch + (searchingCatalog ? L"|" : L""),
        box(59, 31, 108, 35), Body, tint(catalogSearch.empty() ? p.muted : p.ink));
    if (!catalogSearch.empty()) iconButton(sheet, p, box(169, 37, 21, 23), Close,
        [this] { catalogSearch.clear(); catalogPage = 0; paintSheet(); });

    const float navTargetY = 81.f + 37.f * catalogGroup;
    if (!catalogNavY.initialized) catalogNavY.snap(navTargetY);
    else if (std::abs(catalogNavY.target - navTargetY) > 0.5f) {
        catalogNavY.setTarget(navTargetY, motionEnabled());
        if (motionEnabled()) startAnimTimer();
    }
    const auto activeSlot = box(28, catalogNavY.value, 167, 33);
    d.fill(activeSlot, 8, tint(p.accent, p.light ? .13f : .16f));
    d.fill(box(activeSlot.left + 3, activeSlot.top + 8.5f, 2, 16), 1, tint(p.accent));

    for (int i = 0; i < ARRAYSIZE(categories); ++i) {
        const auto slot = box(28, 81 + 37.f * i, 167, 33);
        hit(sheet, slot, [this, i] {
            if (catalogGroup != i) {
                catalogGroup = i; catalogPage = 0; catalogScrollY.snap(0.f); searchingCatalog = false;
                sheet.resetMotion();
                triggerSheetContentAnim(0.f, 8.f);
                paintSheet();
            }
        });
        const bool selected = i == catalogGroup;
        const float hover = sheet.hoverAmount();
        if (!selected && hover > 0) {
            d.fill(slot, 8, tint(p.ink, .07f * hover));
        }
        const auto& category = categories[i];
        d.icon(category.glyph, box(40, slot.top + 9, 16, 16), tint(selected ? p.accent : p.muted));
        d.text(category.title, box(65, slot.top, 124, 34), selected ? Strong : Body, tint(selected ? p.ink : p.muted));
    }

    const auto back = box(28, sheet.h - 72, 167, 34);
    hit(sheet, back, [this] {
        tab = 0; searchingCatalog = false; sheet.hover = sheet.key = -1; positionSheet();
    });
    d.fill(back, 8, tint(p.ink, .055f + sheet.hoverAmount() * .075f));
    d.icon(Gear, box(40, back.top + 9, 16, 16), tint(p.muted));
    d.text(L"Ayarlar", box(65, back.top, 114, 34), Body, tint(p.ink));

    const auto matches = [&](int id) {
        if (catalogSearch.empty()) return true;
        const Text terms = Text(Widgets[id].title) + L" " + Widgets[id].description;
        constexpr DWORD flags = FIND_FROMSTART | NORM_IGNORECASE | NORM_IGNORENONSPACE;
        return FindNLSStringEx(L"tr-TR", flags, terms.c_str(), -1, catalogSearch.c_str(), -1,
            nullptr, nullptr, nullptr, 0) >= 0 ||
            FindNLSStringEx(LOCALE_NAME_INVARIANT, flags, terms.c_str(), -1, catalogSearch.c_str(), -1,
                nullptr, nullptr, nullptr, 0) >= 0;
    };

    struct Item { int id; float span, cell; };
    struct Row { int category; std::vector<Item> items; };
    std::vector<Row> rows;
    int matchCount = 0;
    for (int group = Ai; group <= Forecasts; ++group) {
        if (catalogGroup != All && group != catalogGroup) continue;
        Row row{group, {}};
        float occupied = 0;
        for (int index = 0; index < WidgetCount; ++index) {
            // Keep both AI comparison cards adjacent while retaining stable saved widget IDs.
            const int id = index == AiLimits + 1 ? AiLimitsTriple
                : index > AiLimits + 1 && index <= AiLimitsTriple ? index - 1 : index;
            if (categoryOf(id) != group || !matches(id)) continue;
            const float span = widgetWidth(id, true);
            const float cell = std::max(span, 106.f);
            if (occupied + cell > end - start && !row.items.empty()) {
                rows.push_back(std::move(row));
                row = {group, {}};
                occupied = 0;
            }
            row.items.push_back({id, span, cell});
            occupied += cell + gap;
            ++matchCount;
        }
        if (!row.items.empty()) rows.push_back(std::move(row));
    }

    const auto& layout = model.layouts[model.profile];
    const auto activeCount = std::count_if(layout.begin(), layout.end(), [](Tile item) { return item.enabled; });
    const Text title = catalogGroup == All ? L"Widget kataloğu" : categories[catalogGroup].title;
    d.text(title, box(start, 24, sheet.w - start - 102, 38), Title, tint(p.ink));
    d.text(num(matchCount) + L" widget  ·  " + num(activeCount) + L" dock’ta", box(start + 1, 65, 340, 17), Caption, tint(p.muted));
    iconButton(sheet, p, box(sheet.w - 58, 27, 31, 31), Close, [this] { close(); });
    d.line(start, 92, end, 92, tint(p.ink, .09f));

    const int count = static_cast<int>(rows.size());
    const auto startsSection = [&](int i, int first) {
        return catalogGroup == All && (i == first || rows[i].category != rows[i - 1].category);
    };
    std::vector<float> rowTop(count, 0.f);
    std::vector<bool> rowHeader(count, false);
    float totalH = 0.f;
    for (int i = 0; i < count; ++i) {
        rowHeader[i] = startsSection(i, 0);
        rowTop[i] = totalH;
        if (rowHeader[i]) totalH += sectionHeight;
        totalH += rowHeight;
    }
    const float viewH = contentBottom - contentTop;
    const float maxScrollPx = std::max(0.f, totalH - viewH);
    int maxOffset = 0;
    while (maxOffset + 1 < count && rowTop[maxOffset] < maxScrollPx - 1.f) ++maxOffset;
    catalogPage = std::clamp(catalogPage, 0, maxOffset);
    const float targetScroll = count > 0 ? std::min(maxScrollPx, rowTop[catalogPage]) : 0.f;
    catalogScrollY.setTarget(targetScroll, motionEnabled(), 0.25f);
    if (motionEnabled() && catalogScrollY.active(0.2f, 1.5f)) startAnimTimer();
    const float scrollPx = std::clamp(catalogScrollY.value, 0.f, maxScrollPx);

    const float cT = sheetContentProgress();
    const Box clipRect = box(start - 4, contentTop - 4, end - start + 8, viewH + 8);
    const auto catMotion = d.pushMotion(clipRect, cT < 1.f ? (0.2f + 0.8f * cT) : 1.f,
        sheetContentDx * (1.f - cT), sheetContentDy * (1.f - cT));
    if (!catMotion.pushedLayer && !catMotion.pushedClip) d.clip(clipRect);

    for (int i = 0; i < count; ++i) {
        float y = contentTop + rowTop[i] - scrollPx;
        const float itemBottom = y + (rowHeader[i] ? sectionHeight : 0.f) + rowHeight;
        if (itemBottom < contentTop - 4.f || y > contentBottom + 4.f) continue;
        const auto& row = rows[i];
        if (rowHeader[i]) {
            d.text(categories[row.category].title, box(start, y - 2, 330, 19), Strong, tint(p.muted));
            y += sectionHeight;
        }
        float x = start;
        for (const auto& item : row.items) {
            const int id = item.id;
            const Box hitBox = box(x, std::max(contentTop, y), item.cell,
                std::max(0.f, std::min(contentBottom, y + rowHeight - 8) - std::max(contentTop, y)));
            float hotVal = 0.f;
            if (height(hitBox) > 4.f) {
                hit(sheet, hitBox, [this, id] { toggleTile(id); });
                hotVal = sheet.hoverAmount();
            }
            const bool hot = hotVal > .05f;
            const bool enabled = shown(id);
            const auto card = box(x, y, item.span, Design::CardHeight);
            widgetCard(sheet, id, card, p, true);
            if (hotVal > 0.01f) d.outline(inset(card, .65f), 12, tint(p.accent, .85f * hotVal), 1.3f);

            const bool sample = ((weatherWidget(id) || id == SevenDays || id == TodayWidget || id == ClockWeather) && !extra.forecast) ||
                (marketWidget(id) && (id == StockWatchlist ? !extra.quotes[1] || !extra.quotes[2] || !extra.quotes[3] :
                    !extra.quotes[id == Crypto ? 4 : 0]));
            const bool showControl = hot || enabled;
            const float controlSpace = showControl ? 22.f : 0.f;
            const bool showExample = sample && item.cell >= 130;
            d.text(Widgets[id].title, box(x + 2, y + 59, item.cell - controlSpace - (showExample ? 43 : 4), 21),
                Caption, tint(mixRgb(p.muted, p.ink, hotVal)));
            if (showExample) d.text(L"Örnek", box(x + item.cell - controlSpace - 42, y + 59, 38, 21),
                Small, tint(p.muted), Right);
            if (showControl) {
                const auto control = box(x + item.cell - 19, y + 61, 17, 17);
                d.fill(control, 5, tint(p.accent, .13f + .11f * hotVal));
                d.icon(enabled ? (hot ? Minus : Check) : Plus, inset(control, 3), tint(p.accent));
            }
            x += item.cell + gap;
        }
    }
    if (!catMotion.pushedLayer && !catMotion.pushedClip) d.unclip();
    d.popMotion(catMotion);

    if (rows.empty()) {
        d.icon(SearchIcon, box((start + end) / 2 - 14, 218, 28, 28), tint(p.muted));
        d.text(L"Aramana uyan widget bulunamadı.", box(start, 257, end - start, 32), Body, tint(p.muted), Center);
    }
    if (maxScrollPx > 1.f) {
        const float footer = sheet.h - 53;
        d.line(start, footer + 1, end, footer + 1, tint(p.ink, .09f));
        if (catalogPage > 0) iconButton(sheet, p, box(end - 72, footer + 6, 30, 30), ChevronUp, [this] { scrollCatalog(-1); });
        if (catalogPage < maxOffset) iconButton(sheet, p, box(end - 34, footer + 6, 30, 30), ChevronDown, [this] { scrollCatalog(1); });
        const float track = viewH;
        const float thumb = std::max(28.f, track * (viewH / std::max(viewH, totalH)));
        const float frac = scrollPx / maxScrollPx;
        d.fill(box(end + 9, contentTop, 3, track), 1.5f, tint(p.ink, .07f));
        d.fill(box(end + 9, contentTop + (track - thumb) * frac, 3, thumb), 1.5f, tint(p.accent, .56f));
    }
}
void Dock::studio(Palette p) {
    auto& d = sheet.d;
    orderRows.clear();
    opacitySliderTrack = {};
    constexpr float content = 228, navWidth = 183;
    const float right = sheet.w - 38;
    const auto sidebar = box(20, 19, navWidth, sheet.h - 46);
    d.fill(sidebar, 15, tint(p.rail));
    d.line(214, 29, 214, sheet.h - 35, tint(p.ink, .08f));
    d.icon(Logo, d.snap(box(32, 32, 41, 41)), tint(0xF3F6FC));
    d.text(L"Vetra", box(85, 32, 95, 41), Value, tint(p.ink));
    constexpr const wchar_t* tabs[] = {L"Düzen", L"Widget kataloğu", L"Görünüm", L"Kısayollar", L"Durum alanı"};
    constexpr Glyph icons[] = {Layers, Plus, Sliders, PinIcon, Activity};
    const float targetNavY = 104.f + tab * 41.f;
    if (!studioNavY.initialized) studioNavY.snap(targetNavY);
    else if (std::abs(studioNavY.target - targetNavY) > 0.5f) {
        studioNavY.setTarget(targetNavY, motionEnabled());
        if (motionEnabled()) startAnimTimer();
    }
    const auto activeNavRow = box(28, studioNavY.value, navWidth - 16, 34);
    d.fill(activeNavRow, 8, tint(p.accent, p.light ? .13f : .16f));
    d.fill(box(activeNavRow.left + 3, activeNavRow.top + 9, 2, 16), 1, tint(p.accent));
    for (int i = 0; i < ARRAYSIZE(tabs); ++i) {
        const auto row = box(28, 104 + i * 41.f, navWidth - 16, 34);
        hit(sheet, row, [this, i] { tab = i; sheet.resetMotion(); positionSheet(); });
        const float hover = sheet.hoverAmount();
        if (tab != i && hover > 0) d.fill(row, 8, tint(p.ink, .07f * hover));
        d.icon(icons[i], box(row.left + 12, row.top + 9, 16, 16), tint(tab == i ? p.accent : p.muted));
        d.text(tabs[i], box(row.left + 37, row.top, width(row) - 45, 34),
            tab == i ? TileLabel : Body, tint(tab == i ? p.ink : p.muted));
    }

    const wchar_t* title = tab == 0 ? L"Dock düzeni" : tabs[tab];
    d.text(title, box(content, 28, 468, 46), Title, tint(p.ink));
    iconButton(sheet, p, box(right - 34, 34, 34, 34), Close, [this] { close(); });
    d.line(content, 77, right, 77, tint(p.ink, .09f));

    const float cT = sheetContentProgress();
    const auto tabMotion = d.pushMotion(box(content - 6, 80, right - content + 12, sheet.h - 90),
        cT < 1.f ? (0.15f + 0.85f * cT) : 1.f,
        sheetContentDx * (1.f - cT), sheetContentDy * (1.f - cT));

    if (tab == 0) {
        d.text(L"Profil", box(content, 87, 230, 23), Strong, tint(p.ink));
        const auto profiles = box(content, 116, right - content, 68);
        p.card(d, profiles, 18);
        const float pitch = width(profiles) / static_cast<float>(ProfileCount);
        for (int i = 0; i < ProfileCount; ++i) {
            const Box item = box(content + 5 + i * pitch, 121, pitch - 10, 58);
            hit(sheet, item, [this, i] {
                model.profile = i;
                orderPage = 0;
                sheet.key = sheet.hover = -1;
                if (shown(Git)) branch = currentBranch(model.repo);
                sampleExtras(true);
                changed(true);
            });
            const bool selected = model.profile == i;
            const float hover = sheet.hoverAmount();
            if (selected || hover > 0) d.fill(item, 14, tint(p.ink, selected ? .13f : .08f * hover));
            d.icon(ProfileIcons[i], box(item.left + 11, item.top + 11, 17, 17), tint(selected ? p.ink : p.muted));
            d.text(ProfileNames[i], box(item.left + 34, item.top + 8, width(item) - 45, 21), TileLabel, tint(p.ink));
            d.text(ProfileDetails[i], box(item.left + 11, item.top + 32, width(item) - 22, 17), Caption, tint(p.muted));
        }

        const auto& layout = model.layouts[model.profile];
        std::vector<int> enabled;
        for (int i = 0; i < static_cast<int>(layout.size()); ++i) if (layout[i].enabled) enabled.push_back(i);
        constexpr int perPage = 6;
        const int pages = std::max(1, (static_cast<int>(enabled.size()) + perPage - 1) / perPage);
        orderPage = std::clamp(orderPage, 0, pages - 1);
        d.text(num(enabled.size()) + L" etkin widget", box(content, 201, 250, 29), Strong, tint(p.ink));
        button(sheet, p, box(right - 122, 199, 122, 33), L"+ Widget ekle", [this] {
            tab = 1; catalogPage = 0; positionSheet();
        }, true);

        p.card(d, box(content, 243, right - content, 302), 20);
        if (enabled.empty())
            d.text(L"Katalogdan bir widget ekle.", box(content + 12, 315, right - content - 24, 63),
                Body, tint(p.muted), Center);

        auto move = [this](int from, int to) {
            const auto& items = model.layouts[model.profile];
            reorderDockItems({OrderKind::Widget, items[from].id}, {OrderKind::Widget, items[to].id});
        };
        for (int row = 0; row < perPage; ++row) {
            const int indexInPage = row + orderPage * perPage;
            if (indexInPage >= static_cast<int>(enabled.size())) break;
            const int index = enabled[indexInPage], id = layout[index].id;
            const float y = 247 + 49.f * row;
            const Box rowBounds = box(content + 7, y, right - content - 14, 48);
            orderRows.push_back({rowBounds, index});
            if (dragSource == index || dragTarget == index) d.fill(rowBounds, 13, tint(p.ink, .12f));
            else if (row) d.line(content + 20, y, right - 20, y, tint(p.line, .18f));
            d.icon(Grip, box(content + 13, y + 15, 18, 18), tint(p.muted));
            const Box name = box(content + 38, y + 3, 385, 42);
            hit(sheet, name, [this, id] {
                if (id == Apps) { tab = 3; sheet.key = sheet.hover = -1; positionSheet(); }
                else open(id);
            });
            const float hover = sheet.hoverAmount();
            if (hover > 0) d.fill(name, 12, tint(p.ink, .065f * hover));
            badge(d, box(content + 43, y + 9, 30, 30), Glyphs[id], p.ink);
            d.text(Widgets[id].title, box(content + 84, y + 5, 316, 20), TileLabel, tint(p.ink));
            d.text(Widgets[id].description, box(content + 84, y + 25, 316, 18), Caption, tint(p.muted));
            if (indexInPage > 0)
                iconButton(sheet, p, box(right - 139, y + 8, 29, 32), ChevronUp,
                    [=, other = enabled[indexInPage - 1]] { move(index, other); });
            if (indexInPage + 1 < static_cast<int>(enabled.size()))
                iconButton(sheet, p, box(right - 105, y + 8, 29, 32), ChevronDown,
                    [=, other = enabled[indexInPage + 1]] { move(index, other); });
            knob(p, box(right - 60, y + 12, 43, 24), true, [this, id] { toggleTile(id); });
        }
        if (!folded.empty())
            d.text(num(folded.size()) + L" öğe taşma menüsünde", box(content, 553, 365, 26), Caption, tint(p.muted));
        else if (enabled.size() > 1)
            d.text(L"Sıralamak için tutamaçtan sürükle", box(content, 553, 365, 26), Caption, tint(p.muted));
        if (pages > 1) {
            iconButton(sheet, p, box(right - 112, 553, 30, 28), ChevronLeft,
                [this] { orderPage = std::max(0, orderPage - 1); paintSheet(); });
            d.text(num(orderPage + 1) + L" / " + num(pages), box(right - 79, 553, 43, 28), Caption, tint(p.muted), Center);
            iconButton(sheet, p, box(right - 32, 553, 30, 28), ChevronRight,
                [this, pages] { orderPage = std::min(pages - 1, orderPage + 1); paintSheet(); });
        }
    } else if (tab == 2) {
        struct ThemeOption { AppearanceTheme value; const wchar_t* label; };
        constexpr ThemeOption themes[] = {{LiquidGlass, L"Liquid Glass"}, {WhiteTheme, L"Beyaz"}, {BlackTheme, L"Siyah"},
            {FrostGlass, L"Buzlu cam"}, {BareTheme, L"Çerçevesiz"}};
        const float gap = 11, span = (right - content - 2 * gap) / 3;
        for (int i = 0; i < ARRAYSIZE(themes); ++i) {
            const auto option = themes[i];
            const auto tile = box(content + (i % 3) * (span + gap), 88 + (i / 3) * 80.f, span, 71);
            hit(sheet, tile, [this, option] { model.theme = option.value; changed(true); });
            const float hover = sheet.hoverAmount();
            const float down = sheet.pressAmount();
            const bool selected = model.theme == option.value;
            D2D1::Matrix3x2F tp{};
            if (down > 0.005f) {
                d.target->GetTransform(&tp);
                const float s = 1.f - 0.025f * down;
                d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s, {tile.left + span / 2.f, tile.top + 35.5f}) * tp);
            }
            p.card(d, tile, 17);
            if (hover > 0) d.fill(tile, 17, tint(p.ink, .06f * hover));
            if (selected) d.outline(inset(tile, .75f), 16.25f, tint(p.accent, .80f), 1.3f);

            const Palette colors(option.value, model.opacity, model.accent);
            const auto preview = box(tile.left + 10, tile.top + 8, width(tile) - 20, 34);
            d.gradient(preview, 8, tint(0x1D2835), tint(0x566D82), true);
            const auto sampleDock = box(preview.left + 19, preview.top + 6, width(preview) - 38, 22);
            colors.surface(d, sampleDock, 8, false);
            for (int j = 0; j < 3; ++j) {
                const auto cell = box(sampleDock.left + 6 + j * (width(sampleDock) - 12) / 3, sampleDock.top + 4,
                    (width(sampleDock) - 18) / 3, 14);
                colors.card(d, cell, 4);
                d.fill(box(cell.left + 5, cell.top + 4, width(cell) * .45f, 5), 2.5f, tint(colors.accent, .72f));
            }
            d.text(option.label, box(tile.left + 13, tile.top + 43, width(tile) - 45, 23), TileLabel, tint(p.ink));
            if (selected) d.icon(Check, box(tile.right - 24, tile.top + 47, 14, 14), tint(p.accent));
            if (down > 0.005f) d.target->SetTransform(tp);
        }

        const auto row = [&](float y, const Text& label) {
            p.card(d, box(content, y, right - content, 40), 13);
            d.text(label, box(content + 15, y, 250, 40), TileLabel, tint(p.ink));
        };
        const auto choices = [&](float y, const Text& label, std::initializer_list<const wchar_t*> values, int& selected) {
            row(y, label);
            const float pitch = 310.f / values.size();
            int index = 0;
            for (const auto item : values) {
                button(sheet, p, box(right - 324 + index * pitch, y + 5, pitch - 5, 30), item,
                    [this, destination = &selected, index] { *destination = index; changed(true); }, selected == index);
                ++index;
            }
        };
        float y = 248;
        row(y, L"Vurgu rengi");
        for (int i = 0; i < ARRAYSIZE(Accents); ++i) {
            const auto chip = box(right - 332 + i * 62.f, y + 5, 57, 30);
            const auto color = i == 0 ? p.ink : p.light ? Accents[i].light : Accents[i].dark;
            const bool selected = model.accent == i;
            hit(sheet, chip, [this, i] { model.accent = i; changed(false); });
            const float hover = sheet.hoverAmount();
            if (selected || hover > 0) d.fill(chip, 8, tint(color, selected ? .19f : .11f * hover));
            if (selected) d.outline(inset(chip, .5f), 8, tint(color, .72f));
            d.text(Accents[i].label, chip, Small, tint(p.ink), Center);
        }
        y += 45;
        if (glassTheme(model.theme)) {
            row(y, L"Cam tonu");
            const unsigned blueStart = p.light ? 0x0A7AFF : 0x2684FF;
            const unsigned blueEnd = p.light ? 0x3A9BFF : 0x5BB6FF;
            const float x = right - 324, track = 258, center = y + 20.f;
            const float smoothOp = opacityAnim.initialized ? std::clamp(opacityAnim.value, 0.f, 100.f) : static_cast<float>(model.opacity);
            const float portion = std::clamp(smoothOp / 100.f, 0.f, 1.f);
            const Box trackBox = box(x, center - 5.5f, track, 11.f);
            opacitySliderTrack = trackBox;
            hit(sheet, box(x - 4.f, y + 4.f, track + 8.f, 32.f), [] {});
            const float sliderHover = sheet.hoverAmount();
            d.fill(trackBox, 5.5f, tint(p.light ? 0xD8D8DC : 0xFFFFFF, p.light ? 1.f : .13f));
            const float fillW = track * portion;
            if (fillW > 0.5f) {
                const float drawnW = std::max(height(trackBox), fillW);
                const Box fillBox = box(trackBox.left, trackBox.top, std::min(track, drawnW), height(trackBox));
                d.gradient(fillBox, 5.5f, tint(blueStart), tint(blueEnd), true);
            }
            const float thumbVis = opacitySliderDrag ? 1.f : std::max(0.75f, sliderHover);
            const float knobX = std::clamp(trackBox.left + std::max(5.5f, fillW - 5.5f), trackBox.left + 5.5f, trackBox.right - 5.5f);
            d.circle(knobX, center, opacitySliderDrag ? 4.3f : 3.6f, tint(0xFFFFFF, .96f * thumbVis));
            d.text(percent(model.opacity), box(x + track + 8.f, y + 5.f, 44.f, 30.f), Caption,
                tint(opacitySliderDrag ? blueEnd : p.muted), Right);
            y += 45;
        }
        choices(y, L"Dock boyutu", {L"Kompakt", L"Standart", L"Geniş"}, model.size);
        choices(y + 45, L"Ekran kenarı", {L"Alt", L"Üst"}, model.edge);
        choices(y + 90, L"Hizalama", {L"Sol", L"Orta", L"Sağ"}, model.alignment);
        row(y + 135, L"Dock görünürlüğü");
        constexpr std::pair<DockVisibility, const wchar_t*> visibilityModes[] = {
            {DockVisibility::Overlay, L"Sabit göster"},
            {DockVisibility::Edge, L"Kenar ile aç"},
            {DockVisibility::Reserved, L"Yer ayır"}
        };
        for (size_t i = 0; i < std::size(visibilityModes); ++i) {
            const auto [mode, label] = visibilityModes[i];
            button(sheet, p, box(right - 324 + 103.f * i, y + 140, 98, 30), label, [this, mode] {
                model.visibility = mode;
                model.topmost = true;
                changed(true);
            }, model.visibility == mode);
        }
        row(y + 180, L"Windows görev çubuğunu gizle");
        knob(p, box(right - 61, y + 188, 43, 24), model.hideTaskbar, [this] {
            model.hideTaskbar = !model.hideTaskbar;
            if (model.hideTaskbar) shellTaskbar.start(false);
            else shellTaskbar.restore();
            changed(true);
        });
        row(y + 225, L"Monitör");
        d.text(L"Tüm monitörler", box(right - 225, y + 230, 205, 30), Caption, tint(p.ink), Right);
        row(y + 270, L"Windows ile başlat");
        knob(p, box(right - 61, y + 278, 43, 24), boot, [this] {
            if (model.setStartup(!boot)) { boot = !boot; paintSheet(); }
            else report(L"Başlangıç ayarı değiştirilmedi.");
        });
    } else if (tab == 3) {
        button(sheet, p, box(content, 89, 142, 34), L"Uygulamalar", [this] { open(Apps); }, true);
        button(sheet, p, box(content + 152, 89, 139, 34), L"+ Özel kısayol", [this] { editShortcut(-1); });
        button(sheet, p, box(content + 301, 89, 177, 34), L"Windows sabitlerini al", [this] { importTaskbarPins(); });
        constexpr size_t pageSize = 8;
        const int lastPage = model.shortcuts.empty() ? 0 : static_cast<int>((model.shortcuts.size() - 1) / pageSize) * pageSize;
        shortcutOffset = std::clamp(shortcutOffset, 0, lastPage);
        p.card(d, box(content, 140, right - content, 398), 19);
        for (size_t i = shortcutOffset; i < model.shortcuts.size() && i < shortcutOffset + pageSize; ++i) {
            const float y = 144 + 49.f * (i - shortcutOffset);
            const auto& app = model.shortcuts[i];
            const auto rowBounds = box(content + 7, y, right - content - 14, 48);
            const int index = static_cast<int>(i);
            orderRows.push_back({rowBounds, index});
            if (i > static_cast<size_t>(shortcutOffset)) d.line(content + 18, y, right - 18, y, tint(p.line, .19f));
            if (dragSource == index || dragTarget == index) d.fill(rowBounds, 13, tint(p.ink, .12f));
            d.icon(Grip, box(content + 13, y + 15, 18, 18), tint(p.muted));
            const auto icon = box(content + 41, y + 9, 31, 31);
            if (appImages[i]) d.image(appImages[i], icon, p.light);
            else badge(d, icon, Link, p.ink);
            d.text(app.label, box(content + 82, y + 5, 300, 20), TileLabel, tint(p.ink));
            d.text(app.target, box(content + 82, y + 25, 300, 18), Caption, tint(p.muted));
            if (i > 0) iconButton(sheet, p, box(right - 165, y + 9, 30, 30), ChevronUp, [this, i] {
                reorderDockItems({OrderKind::Shortcut, static_cast<int>(i)}, {OrderKind::Shortcut, static_cast<int>(i - 1)});
            });
            if (i + 1 < model.shortcuts.size()) iconButton(sheet, p, box(right - 130, y + 9, 30, 30), ChevronDown, [this, i] {
                reorderDockItems({OrderKind::Shortcut, static_cast<int>(i)}, {OrderKind::Shortcut, static_cast<int>(i + 1)});
            });
            button(sheet, p, box(right - 89, y + 9, 69, 30), L"Düzenle", [this, i] { editShortcut(static_cast<int>(i)); });
        }
        if (model.shortcuts.empty())
            d.text(L"Henüz sabitlenmiş uygulama yok.", box(content, 194, right - content, 38), Body, tint(p.muted), Center);
        d.text(L"İlk sekiz sabit uygulama dock’ta görünür.", box(content, 551, 382, 27), Caption, tint(p.muted));
        if (lastPage > 0) {
            iconButton(sheet, p, box(right - 72, 549, 32, 30), ChevronLeft, [this] { shortcutOffset -= 8; paintSheet(); });
            iconButton(sheet, p, box(right - 33, 549, 32, 30), ChevronRight, [this] { shortcutOffset += 8; paintSheet(); });
        }
    } else if (tab == 4) {
        auto& sb = model.statusBar;
        const auto masterCard = box(content, 88, right - content, 54);
        p.card(d, masterCard, 16);
        d.text(L"Windows durum alanını göster", box(content + 16, 94, right - content - 100, 22), Strong, tint(p.ink));
        d.text(L"Etkin profilde ekli olan durum göstergelerini göster",
            box(content + 16, 116, right - content - 100, 18), Caption, tint(p.muted));
        knob(p, box(right - 60, 103, 43, 24), sb.enabled, [this] {
            model.statusBar.enabled = !model.statusBar.enabled;
            if (model.statusBar.enabled) sampleExtras(true);
            changed(true);
        });

        struct StatusRow { Glyph icon; const wchar_t* title; const wchar_t* desc; int widget; };
        StatusRow rows[] = {
            {MicIcon, L"Mikrofon göstergesi", L"Aktif kayıt rengi, hızlı susturma (sağ tık) ve giriş seçici", StatusMic},
            {MicIcon, L"Mikrofonu yalnızca kullanımda göster", L"Bir uygulama mikrofonu kullandığında veya susturulduğunda görünür", -1},
            {KeyboardIcon, L"Klavye dili", L"Aktif klavye düzeni (TR/EN), tek tık seçici ve sağ tık/tekerlek geçişi", StatusKeyboard},
            {EthernetIcon, L"İnternet bağlantısı", L"Ethernet/Wi-Fi simgesi ve özel İnternet paneli", StatusNetwork},
            {Speaker, L"Ses seviyesi", L"Özel Ses & çıkış aygıtı paneli, sağ tık susturma ve tekerlek ayarı", StatusVolume},
            {Battery, L"Pil ve şarj durumu", L"Doluluk ve şarj simgesi (pil algılanmazsa otomatik gizlenir)", StatusBattery},
            {Timer, L"Saat ve tarih", L"İki satırlı saat/tarih (tıklayınca Vetra Bildirim & Takvim panelini açar)", StatusClock},
            {BellIcon, L"Bildirimler", L"Vetra yerel Bildirim Merkezi, Rahatsız Etmeyin, Takvim ve Odaklanma paneli", StatusNotifications},
        };
        const float rowAlpha = sb.enabled ? 1.f : .45f;
        for (int i = 0; i < static_cast<int>(ARRAYSIZE(rows)); ++i) {
            const auto& r = rows[i];
            const float y = 152.f + i * 50.f;
            const bool sub = r.widget < 0;
            const float leftPad = sub ? 22.f : 0.f;
            const Box card = box(content + leftPad, y, right - content - leftPad, 44);
            p.card(d, card, 13);
            const float alpha = (sb.enabled && (!sub || shown(StatusMic))) ? 1.f : .42f;
            if (!sub) {
                d.fill(box(card.left + 11, y + 9, 26, 26), 8, tint(p.accent, .14f * alpha));
                d.icon(r.icon, box(card.left + 16, y + 14, 16, 16), tint(p.ink, .88f * alpha));
            }
            const float textX = card.left + (sub ? 15.f : 46.f);
            d.text(r.title, box(textX, y + 5, card.right - textX - 68.f, 18), TileLabel, tint(p.ink, alpha));
            d.text(r.desc, box(textX, y + 23, card.right - textX - 68.f, 16), Small, tint(p.muted, alpha));
            knob(p, box(right - 60, y + 10, 43, 24),
                sub ? sb.micOnlyInUse : shown(r.widget), [this, widget = r.widget] {
                    if (widget >= 0) toggleTile(widget);
                    else {
                        model.statusBar.micOnlyInUse = !model.statusBar.micOnlyInUse;
                        changed(true);
                    }
                });
        }
        d.text(L"İpucu: Durum alanındaki simgelere sağ tıklayarak susturma, düzen değiştirme veya Windows ayarlarını açabilirsin.",
            box(content, 560, right - content, 24), Small, tint(p.muted, rowAlpha));
    }
    d.popMotion(tabMotion);
}
void Dock::calendar(Palette p) {
    auto& d = sheet.d;
    const auto current = now();
    const Date today = Date::from(current);
    const Text detail = page == Clock ? two(current.wHour) + L":" + two(current.wMinute) + L":" + two(current.wSecond) : L"Aylık takvim ve günlük ajanda";
    heading(p, Text(Months[month.month - 1]) + L" " + num(month.year), detail);
    button(sheet, p, box(613, 32, 62, 31), L"Bugün", [this] {
        selected = month = Date::from(now()); listOffset = 0; sheet.resetMotion();
        triggerSheetContentAnim(0.f, 8.f); paintSheet();
    }, selected.key() == today.key());
    auto move = [this](int direction) {
        Date first{month.year, month.month, 1};
        if ((first.year == 1601 && first.month == 1 && direction < 0) || (first.year == 9999 && first.month == 12 && direction > 0)) return;
        selected = month = first.add(direction < 0 ? -1 : first.length());
        selected.day = month.day = 1;
        listOffset = 0; sheet.resetMotion();
        triggerSheetContentAnim(direction > 0 ? 22.f : -22.f, 0.f);
        paintSheet();
    };
    iconButton(sheet, p, box(680, 32, 30, 31), ChevronLeft, [move] { move(-1); });
    iconButton(sheet, p, box(715, 32, 30, 31), ChevronRight, [move] { move(1); });
    
    constexpr float start = 38, cellW = 62, cellH = 47;
    for (int i = 0; i < 7; ++i)
        d.text(Days[(i + 1) % 7], box(start + i * cellW, 114, 61, 23), Small, tint(p.muted), Center);
    d.line(45, 145, 466, 145, tint(p.ink, .075f));
    const float cT = sheetContentProgress();
    const auto calMotion = d.pushMotion(box(32, 147, 444, 304), cT < 1.f ? (0.15f + 0.85f * cT) : 1.f,
        sheetContentDx * (1.f - cT), sheetContentDy * (1.f - cT));
    const int gap = (Date{month.year, month.month, 1}.weekday() + 6) % 7;
    for (int n = 1; n <= month.length(); ++n) {
        const int index = n + gap - 1;
        Date date{month.year, month.month, n};
        const Box b = box(start + (index % 7) * cellW, 151 + (index / 7) * cellH, 61, 43);
        const Box dayBubble = box(b.left + 11.5f, b.top + 1.5f, 38, 38);
        const bool active = date.key() == selected.key();
        hit(sheet, b, [this, date] { selected = date; listOffset = 0; paintSheet(); });
        const float over = sheet.hoverAmount();
        if (p.theme == LiquidGlass && p.glass) {
            d.fill(b, 11, tint(0x08090B, active ? .38f : .24f));
            if (active || over > 0) d.fill(b, 11, tint(0xFFFFFF, active ? .09f : .055f * over));
            d.glassRim(b, 11, active ? .52f : (.20f + .18f * over));
        } else {
            d.fill(b, 12, tint(p.high, p.glass ? .58f : 1.f));
            if (active) d.outline(inset(b, .8f), 11, tint(p.accent, 1.f), 2);
            else if (over > 0) d.outline(inset(b, .8f), 11, tint(p.line, .6f * over), 1);
        }
        if (!active && date.key() == today.key()) d.outline(dayBubble, 19, tint(p.accent, .77f));
        d.text(num(n), box(b.left, b.top + 5, 61, 30), Strong, tint(active ? p.accent : index % 7 >= 5 ? p.muted : p.ink), Center);
        int marker = 0;
        for (const auto& e : model.events) if (e.day == date.key() && marker < 3) {
            d.fill(box(b.left + 22 + 6 * marker++, b.bottom - 7, 4, 2), 1, tint(EventColors[e.kind]));
        }
    }
    d.popMotion(calMotion);
    for (int i = 0; i < 3; ++i) {
        const float x = 42 + i * 84.0f;
        d.circle(x + 4, 465, 3, tint(EventColors[i]));
        d.text(EventKinds[i], box(x + 13, 455, 71, 20), Caption, tint(p.muted));
    }
    Box agenda = box(501, 103, 278, 346);
    p.card(d, agenda, 19);
    d.text(today.key() == selected.key() ? L"Bugün" : selected.label(), box(519, 119, 159, 29), Value, tint(p.ink));
    std::vector<int> events;
    for (int i = 0; i < static_cast<int>(model.events.size()); ++i) if (model.events[i].day == selected.key()) events.push_back(i);
    std::stable_sort(events.begin(), events.end(), [&](int a, int b) { return model.events[a].time < model.events[b].time; });
    d.text(num(events.size()) + L" kayıt", box(687, 123, 80, 22), Caption, tint(p.muted), Right);
    d.line(520, 153, 760, 153, tint(p.ink, .085f));
    listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(events.size()) - 4));
    for (int i = 0; i < 4 && i + listOffset < static_cast<int>(events.size()); ++i) {
        const int index = events[i + listOffset];
        const auto& e = model.events[index];
        const float y = 162 + i * 54.0f;
        auto b = box(516, y, 249, 47);
        if (hit(sheet, b, [this, index] { editEvent(index); })) d.fill(b, 9, tint(p.ink, .09f));
        d.text(e.time, box(524, y + 6, 41, 34), Caption, tint(p.muted));
        d.fill(box(572, y + 10, 3, 28), 1.5f, tint(EventColors[e.kind]));
        d.text(e.title, box(584, y + 3, 178, 23), Strong, tint(p.ink));
        d.text(EventKinds[e.kind], box(584, y + 26, 168, 15), Caption, tint(p.muted));
    }
    if (events.empty()) {
        d.icon(DateIcon, box(625, 223, 30, 30), tint(p.muted, .78f));
        d.text(L"Ajanda boş", box(517, 267, 245, 26), Strong, tint(p.ink), Center);
        d.text(L"Etkinlik veya teslim tarihi ekle.", box(517, 295, 245, 21), Caption, tint(p.muted), Center);
    }
    button(sheet, p, box(520, 398, 240, 34), L"+  Yeni etkinlik", [&] { editEvent(-1); }, true);
}
void Dock::tasks(Palette p) {
    auto& d = sheet.d;
    std::vector<int> entries;
    int completed = 0;
    for (int i = 0; i < static_cast<int>(model.tasks.size()); ++i) {
        const auto& task = model.tasks[i];
        if (task.profile == model.profile) { entries.push_back(i); completed += task.complete; }
    }
    heading(p, L"Görevler", Text(ProfileNames[model.profile]) + L"  ·  " + num(completed) + L" / " + num(entries.size()) + L" tamamlandı");
    if (!entries.empty()) {
        d.fill(box(27, 105, 384, 4), 2, tint(p.line, .20f));
        const float ratio = taskAnim.initialized ? std::clamp(taskAnim.value, 0.f, 1.f) : (static_cast<float>(completed) / entries.size());
        if (ratio > 0.002f) d.fill(box(27, 105, 384.0f * ratio, 4), 2, tint(Widgets[Tasks].color));
    }
    listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(entries.size()) - 5));
    for (int i = 0; i < 5 && i + listOffset < static_cast<int>(entries.size()); ++i) {
        const int index = entries[i + listOffset];
        const auto& task = model.tasks[index];
        const float y = 122 + i * 41.0f;
        p.card(d, box(27, y, 384, 35), 11);
        auto b = box(31, y + 2, 30, 31);
        const bool over = hit(sheet, b, [this, index] { model.tasks[index].complete = !model.tasks[index].complete; changed(); });
        d.circle(b.left + 15, b.top + 15, 8, tint(task.complete || over ? Widgets[Tasks].color : p.line, task.complete ? .9f : .55f), task.complete, 1.2f);
        if (task.complete) d.icon(Check, box(b.left + 9, b.top + 9, 12, 12), tint(0x173D35));
        const bool overTitle = hit(sheet, box(67, y, 333, 35), [this, index] { editTask(index); });
        if (overTitle) d.fill(box(67, y + 3, 333, 29), 8, tint(p.ink, .06f));
        d.text(task.title, box(74, y, 318, 35), Body, tint(task.complete ? p.muted : p.ink));
    }
    if (entries.empty()) {
        badge(d, box(198, 176, 42, 42), Check, Widgets[Tasks].color);
        d.text(L"İlk görevini ekle", box(27, 231, 384, 26), Strong, tint(p.ink), Center);
    }
    button(sheet, p, box(27, 338, 384, 34), L"+ Yeni görev", [&] { editTask(-1); }, true);
}
void Dock::notes(Palette p) {
    auto& d = sheet.d;
    heading(p, L"Hızlı not", ProfileNames[model.profile]);
    p.card(d, box(27, 106, 384, 212));
    const auto& note = model.notes[model.profile];
    if (note.empty()) {
        badge(d, box(198, 152, 42, 42), Document, Widgets[Notes].color);
        d.text(L"Henüz hızlı not yok", box(47, 206, 344, 26), Strong, tint(p.ink), Center);
        d.text(L"Bu profil için bir not ekle.", box(47, 233, 344, 22), Caption, tint(p.muted), Center);
    }
    for (size_t start = 0, row = 0; start < note.size() && row < 8; ++row) {
        auto end = note.find(L'\n', start);
        Text line = note.substr(start, end == Text::npos ? end : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        d.text(line, box(47, 124 + row * 22.0f, 344, 22), Body, tint(p.ink));
        if (end == Text::npos) break;
        start = end + 1;
    }
    button(sheet, p, box(27, 337, 384, 36), note.empty() ? L"+ Not ekle" : L"Notu düzenle", [&] { editNote(); }, true);
}
void Dock::system(Palette p) {
    auto& d = sheet.d;
    heading(p, L"Sistem", L"Windows sistem ölçümleri");
    const int samples[] = {cpu, ram, battery};
    const float smoothSamples[] = {
        cpuAnim.initialized ? cpuAnim.value : static_cast<float>(std::max(0, cpu)),
        ramAnim.initialized ? ramAnim.value : static_cast<float>(ram),
        batteryAnim.initialized ? batteryAnim.value : static_cast<float>(std::max(0, battery))
    };
    const wchar_t* labels[] = {L"İşlemci", L"Bellek", L"Pil"};
    const unsigned colors[] = {0x7CBBFF, 0xA393FF, 0x70DFC4};
    for (int i = 0; i < 3; ++i) {
        auto y = 110 + i * 52.0f;
        p.card(d, box(27, y, 384, 44), 13);
        d.text(labels[i], box(43, y, 80, 44), Body, tint(p.ink));
        d.fill(box(132, y + 20, 185, 4), 2, tint(p.ink, .12f));
        if (samples[i] >= 0 && smoothSamples[i] > 0.3f)
            d.fill(box(132, y + 20, 185.f * std::clamp(smoothSamples[i], 0.f, 100.f) / 100.0f, 4), 2, tint(colors[i]));
        d.text(percent(samples[i]), box(329, y, 64, 44), Strong, tint(p.ink), Right);
    }
    button(sheet, p, box(27, 271, 384, 36), L"Görev Yöneticisi’ni aç", [&] { launch(L"taskmgr.exe"); });
}
void Dock::git(Palette p) {
    auto& d = sheet.d;
    heading(p, L"Git çalışma alanı", L"Yerel depo");
    p.card(d, box(27, 107, 384, 132));
    d.text(model.repo.empty() ? L"Git deposu seç" : leaf(model.repo), box(44, 123, 346, 31), Value, tint(p.ink));
    d.text(model.repo.empty() ? L"Bir Git çalışma klasörünün yolunu ekle." : model.repo, box(44, 155, 346, 23), Caption, tint(p.muted));
    badge(d, box(44, 191, 28, 28), Branch, Widgets[Git].color);
    d.text(branch.empty() ? L"Dal bilgisi bekleniyor" : branch, box(80, 191, 307, 28), Body, tint(p.ink));
    button(sheet, p, box(27, 267, 117, 34), L"Depo seç", [&] { editRepo(); }, true);
    if (!model.repo.empty()) {
        button(sheet, p, box(153, 267, 126, 34), L"Klasörü aç", [&] { launch(model.repo); });
        button(sheet, p, box(288, 267, 123, 34), L"Terminali aç", [&] { launch(L"cmd.exe", model.repo); });
    }
}
void Dock::metrics(Palette p) {
    auto& d = sheet.d;
    const int widget = page;
    const bool sale = widget == Sales;
    const auto today = Date::from(now());
    auto daily = model.total(widget, today);
    auto values = history(widget);
    auto color = Widgets[widget].color;
    heading(p, Widgets[widget].title, L"Yerel kayıtlar · son yedi gün");
    p.card(d, box(27, 104, 384, 125));
    d.text(sale ? money(daily[0]) : grouped(daily[0]), box(42, 116, 222, 34), Title, tint(p.ink));
    d.text(sale ? L"Bugünkü gelir" : L"Bugünkü ziyaretçi", box(43, 153, 212, 19), Caption, tint(p.muted));
    Text detail = grouped(daily[1]) + (sale ? L" sipariş" : L" kayıt");
    if (!sale && daily[0]) detail += L" · %" + num(daily[1] * 100 / daily[0]);
    d.text(detail, box(43, 182, 204, 24), Body, tint(p.ink));
    d.line(265, 193, 391, 193, tint(p.line, .19f));
    spark(d, box(265, 137, 126, 54), values, color);
    d.text(L"7 GÜNLÜK TREND", box(263, 198, 129, 19), Small, tint(p.muted), Right);
    std::vector<int> indices;
    for (int i = static_cast<int>(model.metrics.size()) - 1; i >= 0; --i) if (model.metrics[i].widget == widget) indices.push_back(i);
    std::stable_sort(indices.begin(), indices.end(), [&](int a, int b) { return model.metrics[a].day > model.metrics[b].day; });
    listOffset = std::clamp(listOffset, 0, std::max(0, static_cast<int>(indices.size()) - 3));
    for (int row = 0; row < 3 && row + listOffset < static_cast<int>(indices.size()); ++row) {
        const int index = indices[row + listOffset];
        const auto& item = model.metrics[index];
        const float y = 238 + row * 30.0f;
        auto b = box(27, y, 384, 27);
        p.card(d, b, 7);
        if (hit(sheet, b, [this, widget, index] { editMetric(widget, index); })) d.fill(b, 7, tint(p.line, .16f));
        d.text(Date::fromKey(item.day).label(), box(39, y, 137, 27), Caption, tint(p.muted));
        d.text((sale ? money(item.first) : grouped(item.first)) + L"  ·  " + grouped(item.second) + (sale ? L" sipariş" : L" kayıt"),
            box(169, y, 229, 27), Body, tint(p.ink), Right);
    }
    if (indices.empty()) d.text(L"Grafiği oluşturmak için ilk kaydını ekle.", box(39, 254, 361, 57), Caption, tint(p.muted), Center);
    button(sheet, p, box(27, 337, 384, 34), L"+ Kayıt ekle", [this, widget] { editMetric(widget, -1); }, true);
}
void Dock::playback(Palette p) {
    auto& d = sheet.d;
    heading(p, L"Medya", L"Windows oynatma kontrolleri");
    p.card(d, box(27, 108, 384, 144));
    const auto artwork = box(48, 119, 53, 53);
    if (extra.media.cover) d.image(extra.media.cover, artwork);
    else badge(d, artwork, Music, Widgets[Media].color);
    d.text(extra.media.title.empty() ? L"Etkin parça yok" : extra.media.title, box(115, 120, 275, 28), Strong, tint(p.ink));
    if (!extra.media.artist.empty()) d.text(extra.media.artist, box(115, 149, 275, 20), Caption, tint(p.muted));
    iconButton(sheet, p, box(128, 188, 48, 43), Back, [&] { media(VK_MEDIA_PREV_TRACK); });
    iconButton(sheet, p, box(188, 183, 62, 53), extra.media.playing ? Pause : Play, [&] { media(VK_MEDIA_PLAY_PAUSE); }, true);
    iconButton(sheet, p, box(262, 188, 48, 43), Next, [&] { media(VK_MEDIA_NEXT_TRACK); });
    button(sheet, p, box(27, 268, 117, 33), L"Sesi azalt", [&] { media(VK_VOLUME_DOWN); });
    button(sheet, p, box(156, 268, 126, 33), L"Sessize al / aç", [&] { media(VK_VOLUME_MUTE); });
    button(sheet, p, box(294, 268, 117, 33), L"Sesi artır", [&] { media(VK_VOLUME_UP); });
}




