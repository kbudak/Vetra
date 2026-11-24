#include "dock.h"
#include <cmath>
#include <cwctype>

namespace {
Text degrees(int value) { return num(value) + L"°"; }

unsigned swingColor(double value, Palette p) {
    return value < 0 ? (p.light ? 0xB92C39 : 0xFF7472) : (p.light ? 0x08794B : 0x36DB86);
}

Text swing(double value) {
    return (value > 0 ? L"+" : Text()) + decimalText(value) + L"%";
}

Forecast weatherExample() {
    const auto today = Date::from(now());
    return {0, 36, 38, 1, 17, 0, true, L"16:15",
        {{today.key(), 41, 29, 1, L"06:10", L"18:05"},
         {today.add(1).key(), 39, 29, 0, {}, {}}, {today.add(2).key(), 38, 28, 0, {}, {}},
         {today.add(3).key(), 36, 29, 2, {}, {}}},
        {{L"17:00", 35, 0, true}, {L"18:00", 34, 0, true}, {L"19:00", 35, 0, false},
         {L"20:00", 34, 0, false}, {L"21:00", 33, 0, false}}};
}

void weatherMark(Draw& d, Box b, int code, bool day, bool light) {
    const float size = width(b), cx = b.left + size / 2, cy = b.top + size / 2;
    const unsigned sunny = light ? 0xA66000 : 0xFFCB38;
    if (code <= 2 && day) {
        d.circle(cx, cy, size * .247f, tint(sunny));
        d.gradient(box(cx - size * .216f, cy - size * .216f, size * .432f, size * .432f),
            size / 2, tint(light ? 0xFFC24D : 0xFFE98A), tint(light ? sunny : 0xFFAF1A));
        for (int i = 0; i < 8; ++i) {
            const float radians = i * .78539816f;
            const float x = std::cos(radians) * size, y = std::sin(radians) * size;
            d.line(cx + .35f * x, cy + .35f * y, cx + .475f * x, cy + .475f * y, tint(sunny), std::max(1.f, .08f * size));
        }
        if (code == 0 || size < 24) return;
        b = box(b.left + size * .35f, b.top + size * .5f, size * .67f, size * .63f);
    } else if (code == 0) {
        d.icon(Moon, b, tint(light ? 0x735315 : 0xFFE6A1));
        return;
    }
    const auto cloud = light ? 0x405974 : 0xDFEDFF;
    d.icon(code >= 50 && (code < 71 || code >= 80) ? Rain : Cloud, b, tint(cloud));
}

int minutes(const Text& time) {
    if (time.size() != 5 || time[2] != L':' || !std::iswdigit(time[0]) || !std::iswdigit(time[1]) ||
        !std::iswdigit(time[3]) || !std::iswdigit(time[4])) return -1;
    return (time[0] - L'0') * 600 + (time[1] - L'0') * 60 + (time[3] - L'0') * 10 + (time[4] - L'0');
}
}

void Dock::drawNewWidget(View& view, int id, Box b, Palette p, bool preview, bool editing) {
    auto& d = view.d;
    const float w = width(b);
    const float motion = preview ? 1.f : widgetProgress();
    const auto at = [&](float x, float y, float width, float height) { return box(b.left + x, b.top + y, width, height); };
    const auto text = [&](const Text& label, float x, float y, float width, float height, Font font, unsigned rgb, Alignment align = Left) {
        d.text(label, at(x, y, width, height), font, tint(rgb), align);
    };
    if (weatherWidget(id)) {
        const auto example = preview && !extra.forecast ? weatherExample() : Forecast{};
        const Forecast* f = extra.forecast ? &*extra.forecast : preview ? &example : nullptr;
        if (!f || f->days.empty()) {
            d.icon(Cloud, at(12, 15, 26, 26), tint(p.accent));
            text(Widgets[id].title, 49, 7, w - 58, 20, TileLabel, p.ink);
            text(model.weatherCity < 0 ? L"Şehir seç" : extra.weatherError ? L"Tahmin alınamadı" : L"Yükleniyor…",
                49, 29, w - 57, 19, Caption, p.muted);
            return;
        }
        const auto& daily = f->days.front();
        const float drift = (1 - motion) * 3;
        const auto weather = [&](float x, float y, float size, int code, bool day, bool light = false) {
            weatherMark(d, at(x, y + drift, size, size), code, day, light);
        };
        const auto range = L"↑ " + degrees(daily.high) + L"  ↓ " + degrees(daily.low);
        switch (id) {
        case Weather:
            weather(13, 10, 36, f->code, f->daylight, p.light);
            text(degrees(f->current), 61, 3, w - 70, 29, ClockBig, p.ink);
            text(range, 61, 31, w - 67, 17, Caption, p.muted);
            break;
        case WeatherDetails: {
            weather(12, 13, 29, f->code, f->daylight, p.light);
            text(degrees(f->current), 49, 5, 86, 24, Value, p.ink);
            text(weatherText(f->code), 49, 30, 85, 18, Caption, p.muted);
            d.line(b.left + 136, b.top + 9, b.left + 136, b.bottom - 9, tint(p.ink, .15f));
            d.icon(WindIcon, at(146, 7, 13, 13), tint(p.light ? 0x12698E : 0x72CFFF));
            d.icon(DropIcon, at(146, 21, 13, 13), tint(p.light ? 0x12698E : 0x72CFFF));
            text(num(f->wind) + L" km/sa", 167, 4, w - 175, 17, Caption, p.ink);
            text(decimalText(f->precipitation, f->precipitation == 0 ? 0 : 1) + L" mm", 167, 20, w - 175, 16, Caption, p.ink);
            text(range, 146, 36, w - 152, 16, Caption, p.ink);
            break;
        }
        case WeatherForecast: {
            weather(11, 15, 24, f->code, f->daylight, p.light);
            text(degrees(f->current), 43, 6, 45, 25, Value, p.ink);
            text(L"Bugün", 43, 33, 45, 17, Caption, p.muted);
            d.line(b.left + 89, b.top + 9, b.left + 89, b.bottom - 9, tint(p.ink, .15f));
            const float span = (w - 94) / 3;
            for (int i = 1; i < static_cast<int>(f->days.size()); ++i) {
                const auto& day = f->days[i];
                const float x = 92 + (i - 1) * span;
                text(ShortDays[Date::fromKey(day.day).weekday()], x, 3, span, 14, Small, p.muted, Center);
                weather(x + (span - 16) / 2, 19, 16, day.code, true, p.light);
                text(degrees(day.high) + L" " + degrees(day.low), x, 37, span, 15, Small, p.ink, Center);
            }
            break;
        }
        case WeatherStatus: {
            text(weatherText(f->code), 12, 6, w - 105, 23, Strong, p.ink);
            text(Cities[f->city].name, 12, 30, w - 105, 20, Caption, p.muted);
            text(degrees(f->current), w - 107, 8, 55, 40, Number, p.ink, Right);
            weather(w - 42, 10, 33, f->code, f->daylight, p.light);
            break;
        }
        case WeatherHourly: {
            const float col = (w - 12) / 6;
            d.fill(at(5, 5, col + 1, 46), 9, tint(p.ink, .10f));
            auto drawHour = [&](int i, const Text& hour, int temp, int code, bool daylight) {
                const float x = 6 + i * col;
                text(hour, x, 4, col, 14, Small, i ? p.muted : p.ink, Center);
                weather(x + (col - 15) / 2.f, 20, 15, code, daylight, p.light);
                text(degrees(temp), x, 36, col, 15, Caption, p.ink, Center);
            };
            drawHour(0, L"Şimdi", f->current, f->code, f->daylight);
            for (int i = 0; i < static_cast<int>(f->hours.size()); ++i) {
                const auto& hour = f->hours[i];
                drawHour(i + 1, hour.time, hour.temp, hour.code, hour.daylight);
            }
            break;
        }
        case SunTimes: {
            const auto left = b.left + 13, length = w - 26, y = b.top + 33;
            const unsigned gold = p.light ? 0xAA590D : 0xFFC473;
            auto altitude = [&](float fraction) { return y - 18 * std::sin(fraction * 3.14159265f); };
            const int rise = minutes(daily.sunrise), set = minutes(daily.sunset), current = minutes(f->time);
            const float position = rise >= 0 && set > rise && current >= 0 ?
                std::clamp(static_cast<float>(current - rise) / (set - rise), 0.f, 1.f) : 0;
            const float marker = position * motion;
            for (int i = 0; i < 36; ++i) {
                const float from = i / 36.f, to = (i + 1) / 36.f;
                d.line(left + length * from, altitude(from), left + length * to, altitude(to),
                    tint(gold, from <= marker ? 1.f : .3f), 1.5f);
            }
            d.circle(left + length * marker, altitude(marker), 5.5f, tint(gold, .18f));
            d.circle(left + length * marker, altitude(marker), 3.5f, tint(gold));
            text(L"↑ " + (daily.sunrise.empty() ? L"—" : daily.sunrise), 12, 36, w / 2 - 13, 17, TileLabel, p.ink);
            text(L"↓ " + (daily.sunset.empty() ? L"—" : daily.sunset), w / 2, 36, w / 2 - 12, 17, TileLabel, p.ink, Right);
            break;
        }
        }
        return;
    }
    if (marketWidget(id)) {
        if (&view == &sheet && page == id) {
            d.fill(b, Design::Radius, tint(p.high, .93f));
            d.outline(b, Design::Radius, tint(p.ink, .12f));
        }
        static const std::array<Quote, 5> examples{{
            {354.81, .56, L"USD", {348.2, 348.6, 348.4, 351.6, 351.0, 349.4, 350.3, 351.8, 352.1, 351.7, 352.2, 353.9, 354.0, 353.2, 352.7, 353.5, 353.6, 353.0, 354.81}},
            {229.57, 1.89, L"USD", {}}, {137.30, -.36, L"USD", {}}, {333.02, 1.10, L"USD", {}},
            {80852.40, 2.41, L"USDT", {78800, 79030, 79340, 79600, 79580, 79445, 79000, 78610, 78505, 78770, 79300, 79700, 80220, 80440, 80480, 80160, 79740, 79675, 79730, 79900}}
        }};
        auto quote = [&](int index) -> const Quote* {
            return extra.quotes[index] ? &*extra.quotes[index] : preview ? &examples[index] : nullptr;
        };
        auto ticker = [&](int index, bool sample) -> Text {
            if (sample) return index == 4 ? L"BTC/USDT" : std::array<Text, 4>{L"TSLA", L"CRM", L"ORCL", L"AAPL"}[index];
            return index == 4 ? model.cryptoSymbol : model.stockSymbols[index];
        };
        auto price = [&](const Quote& q) {
            return decimalText(q.price, q.price != 0 && q.price < .01 ? 8 : 2);
        };
        if (id == StockWatchlist) {
            for (int i = 1; i <= 3; ++i) {
                const auto* q = quote(i);
                const float y = 3.f + (i - 1) * 17;
                text(ticker(i, preview && !extra.quotes[i]), 12, y, w * .30f - 13, 16, TileLabel, p.ink);
                text(q ? price(*q) : L"—", w * .30f, y, w * .37f, 16, Caption, p.ink, Right);
                text(q ? swing(q->change) : L"—", w * .69f, y, w * .31f - 11, 16, Caption,
                    q ? swingColor(q->change, p) : p.muted, Right);
            }
        } else {
            const int index = id == StockChart ? 0 : 4;
            const auto* q = quote(index);
            if (id == Crypto) {
                d.circle(b.left + 20, b.top + 15, 7.5f, tint(0xEF9C26));
                text(L"₿", 13, 7, 14, 15, Small, 0x171717, Center);
            }
            const float x = id == Crypto ? 33 : 12;
            text(ticker(index, preview && !extra.quotes[index]), x, 4, w * .46f - x, 20, Strong, p.ink);
            const Text amount = q ? price(*q) : L"—";
            const float amountWidth = w * .54f - 11.f;
            text(amount, w * .46f, 1, amountWidth, 23, d.fit(amount, amountWidth, Value, Strong), p.ink, Right);
            if (q) {
                const auto color = swingColor(q->change, p);
                text(swing(q->change), w * .53f, 22, w * .47f - 12, 15, Caption, color, Right);
                if (q->trend.size() > 1) d.sparkline(at(12, 35, w - 24, 17), q->trend, color, motion);
                else text(L"Grafik bekleniyor", 12, 33, w - 24, 16, Small, p.muted);
            } else {
                const bool error = (extra.marketErrors & (1u << (id - StockChart))) != 0;
                text(error ? L"Veri alınamadı" : id == Crypto ? L"Fiyat bekleniyor" : L"Fiyat kaynağı · Twelve Data",
                    12, 32, w - 24, 16, Caption, p.muted);
            }
        }
        return;
    }
    switch (id) {
    case Recent: {
        d.icon(HistoryIcon, at(13, 16, 25, 25), tint(p.muted));
        bool any = !model.recentApps.empty() || !recentFiles.empty();
        if (any || preview) {
            const int pinned = model.recentApps.empty() ? -1 : pinnedIndex(model.recentApps.front());
            if (pinned >= 0 && appImages[pinned]) d.image(appImages[pinned], at(50, 12, 32, 32), p.light);
            else badge(d, at(50, 12, 32, 32), Layers, p.accent);
            if (!recentFiles.empty() || preview) badge(d, at(91, 12, 32, 32), Document, p.light ? 0x40546E : 0xCBD8E9);
        } else text(L"Son açılanlar", 44, 14, w - 50, 29, Caption, p.ink);
        break;
    }
    case RecycleBin:
        d.circle(b.left + w / 2, b.top + 28, 20, tint(p.ink, .10f));
        d.icon(Trash, at((w - 25) / 2, 15, 25, 25), tint(p.muted));
        if (recycleItems > 0 || preview) d.circle(b.right - 13, b.top + 13, 3, tint(p.light ? 0xC97A00 : 0xFFAE42));
        break;
    case Search:
    case Commands:
        d.icon(id == Search ? SearchIcon : ChevronRight, at(11, 16, 23, 23),
            tint(id == Search ? p.muted : swingColor(1, p)));
        text(id == Search ? L"Ara veya aç" : L"Komut yaz", 42, 10, w - 53, 36, TileLabel, p.ink);
        break;
    case Capture: {
        const Glyph glyph[] = {ScreenIcon, CropIcon, Layers};
        const unsigned color[] = {p.light ? 0x215AA4u : 0x6CB2FFu, swingColor(1, p), p.light ? 0x64449Au : 0xBEA3FFu};
        for (int i = 0; i < 3; ++i) {
            const float x = 17 + i * 49.f;
            const auto slot = at(x, 9, 38, 38);
            float hover = 0;
            if (!editing && !preview) {
                hit(view, slot, [this, i, center = b.left + w / 2] { if (i == 2) open(Capture, center); else startCapture(i); });
                hover = view.hoverAmount();
            }
            d.circle(slot.left + 19, slot.top + 19, 17 + 2 * hover, tint(color[i], .17f + .16f * hover));
            d.icon(glyph[i], at(x + 8, 17 - hover * 2, 22, 22), tint(color[i]));
        }
        break;
    }
    }
}
