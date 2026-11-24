#include "dock.h"
#include <appmodel.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <numeric>
#include <sstream>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Notifications;
using namespace winrt::Windows::UI::Notifications::Management;

struct Dock::WhatsAppFeed {
    UserNotificationListener listener{nullptr};
    IAsyncOperation<UserNotificationListenerAccessStatus> access{nullptr};
    IAsyncOperation<Collections::IVectorView<UserNotification>> request{nullptr};
    ULONGLONG nextPoll = 0;
    int count = -1;
    Text latest;
    bool unavailable = false;
};

namespace {
constexpr const wchar_t* Categories[] = {L"Tarayıcı", L"Çalışma", L"Medya", L"Diğer"};

Text decimalText(double value, bool fixed) {
    std::wostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(2) << value;
    Text text = out.str();
    if (!fixed) {
        while (text.ends_with(L"0")) text.pop_back();
        if (text.ends_with(L".")) text.pop_back();
    }
    const auto decimal = text.find(L'.');
    if (decimal != Text::npos) text[decimal] = L',';
    return text;
}

double currencyInput(Text value) {
    if (value.empty() || value.find_first_not_of(L"0123456789.,") != Text::npos) return -1;
    std::replace(value.begin(), value.end(), L',', L'.');
    std::wistringstream input(value);
    input.imbue(std::locale::classic());
    double amount = 0;
    return input >> amount && input.eof() && amount >= 0.0 && amount <= 1000000000.0 ? amount : -1.0;
}

bool isWhatsApp(Text id) {
    std::transform(id.begin(), id.end(), id.begin(), [](wchar_t c) { return std::towlower(c); });
    return id.find(L"whatsapp") != Text::npos;
}

int clockMinute(const Text& clock) {
    if (clock.size() != 5 || clock[2] != L':' ||
        clock.find_first_not_of(L"0123456789:") != Text::npos) return -1;
    const int h = (clock[0] - L'0') * 10 + clock[1] - L'0';
    const int m = (clock[3] - L'0') * 10 + clock[4] - L'0';
    return h < 24 && m < 60 ? h * 60 + m : -1;
}
}

void Dock::requestWhatsAppAccess() {
    if (!whatsAppFeed) whatsAppFeed = std::make_shared<WhatsAppFeed>();
    auto& feed = *whatsAppFeed;
    if (feed.access || feed.unavailable) return;
    try {
        if (!feed.listener) feed.listener = UserNotificationListener::Current();
        feed.access = feed.listener.RequestAccessAsync();
        feed.nextPoll = 0;
    } catch (const winrt::hresult_error&) { feed.unavailable = true; }
    paintSheet();
}

bool Dock::tickProductivity(ULONGLONG stamp) {
    const auto visible = [&](int id) { return page == id || (shown(id) && !isFoldedWidget(id)); };
    bool dirty = false;
    if (extra.exchangeRequest.valid() &&
        extra.exchangeRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const auto result = extra.exchangeRequest.get();
        if (result.from == model.currencyFrom && result.to == model.currencyTo) {
            if (result.rate > 0 || extra.exchange.from != result.from || extra.exchange.to != result.to)
                extra.exchange = result;
            extra.exchangeAt = stamp + (result.rate > 0 ? 900000 : 60000);
            dirty = true;
        } else extra.exchangeAt = 0;
    }
    if (visible(Currency)) requestCurrency();

    if (!visible(WhatsApp)) return dirty;
    if (!whatsAppFeed) {
        whatsAppFeed = std::make_shared<WhatsAppFeed>();
        UINT32 length = 0;
        whatsAppFeed->unavailable = GetCurrentPackageFullName(&length, nullptr) == APPMODEL_ERROR_NO_PACKAGE;
        dirty = true;
    }
    auto& feed = *whatsAppFeed;
    if (feed.unavailable) return dirty;
    try {
        if (!feed.listener) feed.listener = UserNotificationListener::Current();
        if (feed.access && feed.access.Status() != AsyncStatus::Started) {
            feed.access = nullptr;
            dirty = true;
        }
        if (feed.listener.GetAccessStatus() != UserNotificationListenerAccessStatus::Allowed) {
            if (feed.count >= 0) { feed.count = -1; feed.latest.clear(); dirty = true; }
            return dirty;
        }
        if (feed.request && feed.request.Status() != AsyncStatus::Started) {
            const auto request = std::exchange(feed.request, nullptr);
            int count = 0;
            Text latest;
            auto timestamp = winrt::clock::time_point::min();
            for (const auto& notification : request.GetResults()) {
                if (!isWhatsApp(notification.AppInfo().AppUserModelId().c_str())) continue;
                ++count;
                if (notification.CreationTime() < timestamp) continue;
                timestamp = notification.CreationTime();
                latest.clear();
                const auto binding = notification.Notification().Visual().GetBinding(KnownNotificationBindings::ToastGeneric());
                if (binding) {
                    const auto elements = binding.GetTextElements();
                    if (elements.Size()) latest = elements.GetAt(0).Text().c_str();
                }
            }
            dirty |= count != feed.count || latest != feed.latest;
            feed.count = count;
            feed.latest = std::move(latest);
        }
        if (!feed.request && !feed.access && stamp >= feed.nextPoll) {
            feed.request = feed.listener.GetNotificationsAsync(NotificationKinds::Toast);
            feed.nextPoll = stamp + 5000;
        }
    } catch (const winrt::hresult_error&) {
        feed.request = nullptr;
        feed.count = -1;
        feed.latest.clear();
        feed.nextPoll = stamp + 60000;
        dirty = true;
        if (!feed.listener) feed.unavailable = true;
    }
    return dirty;
}

void Dock::launchWhatsApp() {
    const auto window = std::find_if(extra.windows.begin(), extra.windows.end(), [](const auto& win) {
        return isWhatsApp(appKey(win.app));
    });
    if (window != extra.windows.end() && IsWindow(window->handle)) {
        activateWindow(window->handle, false);
        return;
    }
    const auto installed = std::find_if(installedApps.begin(), installedApps.end(), [](const auto& app) {
        return isWhatsApp(appKey(app));
    });
    if (installed != installedApps.end()) launchShortcut(*installed);
    else launch(L"https://web.whatsapp.com/");
}

void Dock::drawProductivityWidget(View& view, int id, Box b, Palette p, bool preview) {
    auto& d = view.d;
    const float w = width(b), h = height(b);
    const auto at = [&](float x, float y, float bw, float bh) { return box(b.left + x, b.top + y, bw, bh); };
    if (id == WhatsApp) {
        const auto badge = at(9, 7, 42, 42);
        d.fill(badge, 10, tint(0x25D366));
        d.arc(at(17, 14, 26, 26), 145, 325, tint(0xFFFFFF), 2.65f);
        d.line(b.left + 19, b.top + 34, b.left + 17, b.top + 40, tint(0xFFFFFF), 2.6f);
        d.line(b.left + 17, b.top + 40, b.left + 24, b.top + 38, tint(0xFFFFFF), 2.6f);
        d.arc(at(23, 19, 13, 13), 72, 105, tint(0xFFFFFF), 2.7f);
        const int count = preview ? 3 : whatsAppFeed ? whatsAppFeed->count : -1;
        const Text name = preview ? L"Anna" : count == 0 ? L"Bildirim yok"
            : count > 0 && !whatsAppFeed->latest.empty() ? whatsAppFeed->latest : L"WhatsApp";
        d.text(count < 0 ? L"—" : num(count), at(62, 4, w - 71, 28), ClockBig, tint(p.ink));
        d.text(name, at(62, 32, w - 71, 18), Body, tint(p.muted));
        return;
    }
    if (id == Currency) {
        const double amount = preview ? 500.0 : model.currencyAmount;
        const auto& rate = extra.exchange;
        const bool matches = rate.from == model.currencyFrom && rate.to == model.currencyTo && rate.rate > 0;
        const Text converted = preview ? L"74,44" : matches ? decimalText(amount * rate.rate, true) : L"—";
        d.text(decimalText(amount, false), at(12, 5, w - 69, 19), Strong, tint(p.muted));
        d.text(converted, at(12, 24, w - 69, 27), d.fit(converted, w - 69, Value, Body), tint(p.ink));
        for (int row = 0; row < 2; ++row) {
            const float y = row ? 34.f : 7.f;
            const auto pill = at(w - 54, y, 45, 14);
            d.fill(pill, 7.f, tint(p.ink, .10f));
            const int code = preview ? row : row ? model.currencyTo : model.currencyFrom;
            d.text(CurrencyCodes[code], pill, Caption, tint(p.ink), Center);
        }
        d.text(L"↕", at(w - 54, 20, 45, 15), Body, tint(p.muted), Center);
        return;
    }

    const float base = h - 8, slot = (w - 22) / 24.f;
    const float chartH = h - 28;
    const float motion = preview ? 1.f : widgetProgress();
    const auto local = now();
    const auto day = Date::from(local);
    const bool haveToday = model.usageDay == day.key();
    static constexpr int example[24]{0,0,0,0,0,0,1200,2000,2800,3200,1600,2800,2400,2900,1100,0,0,0,0,0,0,0,0,0};
    const auto rounds = std::accumulate(model.timelineFocus.begin(), model.timelineFocus.end(), 0);
    d.text(preview ? L"Sal 20 Eki" : Text(Days[local.wDayOfWeek]) + L" " + num(day.day) + L" " + ShortMonths[day.month - 1],
        at(10, 3, w * .44f, 14), Micro, tint(p.ink));
    const Text total = (preview ? Text(L"6 sa 5 dk") : usageText(haveToday ? extra.usageTotal : 0)) +
        L"  ·  " + num(preview ? 4 : haveToday ? rounds : 0) + L" odak";
    d.text(total, at(w * .43f, 3, w * .57f - 9, 14), Micro, tint(p.muted), Right);
    for (int hour = 0; hour < 24; ++hour) {
        const float x = 11 + hour * slot;
        d.fill(at(x, base, slot - 1.f, 1.4f), .7f, tint(p.ink, .13f));
        int unclassified = haveToday ? model.usageSeconds[hour] : 0;
        if (haveToday) for (const auto& category : model.timelineUsage) unclassified -= category[hour];
        float barY = base;
        for (size_t kind = 0; kind < ARRAYSIZE(TimelineColors); ++kind) {
            const int seconds = preview ? (kind == 0 ? example[hour] / 2 : kind == (hour % 3) + 1 ? example[hour] / 2 : 0)
                : haveToday ? model.timelineUsage[kind][hour] + (kind == 3 ? std::max(0, unclassified) : 0) : 0;
            const float bar = std::clamp(seconds / 3600.f, 0.f, 1.f) * chartH * motion;
            if (bar <= 0.f) continue;
            const float next = std::max(17.f, barY - bar);
            if (next < barY) d.fill(at(x, next, slot - 1.f, barY - next), .7f, tint(TimelineColors[kind]));
            barY = next;
        }
        if ((preview && hour > 6 && hour % 3 == 0 && hour < 17) ||
            (!preview && haveToday && model.timelineFocus[hour] > 0))
            d.fill(at(x + 1, base + 3, std::max(2.f, slot - 3.f), 1.8f), .8f, tint(0xFF4038));
    }
    const auto marker = [&](int minute, unsigned color, bool hollow) {
        if (minute >= 0) d.circle(b.left + 11 + (minute / 1440.f) * slot * 24, b.top + 20, 1.8f, tint(color), !hollow, 1.f);
    };
    if (preview) {
        marker(440, 0xF6AD2E, true); marker(610, 0x0F88F9, false);
        marker(770, 0xFFFFFF, false); marker(1060, 0xF6AD2E, true);
    } else {
        for (const auto& event : model.events)
            if (event.day == day.key()) marker(clockMinute(event.time), 0x0F88F9, false);
        if (extra.forecast) for (const auto& weather : extra.forecast->days) if (weather.day == day.key()) {
            marker(clockMinute(weather.sunrise), 0xF6AD2E, true);
            marker(clockMinute(weather.sunset), 0xF6AD2E, true);
            break;
        }
    }
}

void Dock::productivityPanel(Palette p) {
    auto& d = sheet.d;
    if (page == Currency) {
        heading(p, L"Döviz", L"Frankfurter · günlük referans kurları");
        const auto card = box(29, 99, 380, 56);
        p.card(d, card);
        drawProductivityWidget(sheet, Currency, card, p, false);
        const auto& rate = extra.exchange;
        const bool matches = rate.from == model.currencyFrom && rate.to == model.currencyTo && rate.rate > 0;
        const Text status = matches ? (rate.date.empty() ? L"Aynı para birimi" : L"Kur tarihi · " + rate.date)
            : extra.exchangeRequest.valid() ? L"Kur alınıyor…" : L"Kur alınamadı. Yenileyebilirsin.";
        d.text(status, box(29, 174, 380, 27), Body, tint(p.muted));
        button(sheet, p, box(29, 253, 380, 40), L"Tutar ve para birimleri", [this] {
            const std::vector<Text> codes(std::begin(CurrencyCodes), std::end(CurrencyCodes));
            std::vector<Field> fields{{L"Tutar", decimalText(model.currencyAmount, false)},
                {L"Kaynak", CurrencyCodes[model.currencyFrom], false, codes},
                {L"Hedef", CurrencyCodes[model.currencyTo], false, codes}};
            if (edit(L"Döviz", fields, false, [](const auto& input) -> Text {
                    return currencyInput(input[0].value) < 0.0 ? L"Geçerli bir tutar gir." : L"";
                }) != 1) return;
            model.currencyAmount = currencyInput(fields[0].value);
            const auto code = [&](const Text& value) {
                return static_cast<int>(std::find(codes.begin(), codes.end(), value) - codes.begin());
            };
            model.currencyFrom = code(fields[1].value);
            model.currencyTo = code(fields[2].value);
            extra.exchangeAt = 0;
            requestCurrency(true);
            changed(false);
        }, true);
        button(sheet, p, box(29, 314, 185, 37), L"Yönü değiştir", [this] {
            std::swap(model.currencyFrom, model.currencyTo);
            extra.exchangeAt = 0;
            requestCurrency(true);
            changed(false);
        });
        button(sheet, p, box(224, 314, 185, 37), L"Yenile", [this] { requestCurrency(true); paintSheet(); });
    } else if (page == WhatsApp) {
        heading(p, L"WhatsApp", L"Windows Bildirim Merkezi");
        const auto card = box(29, 99, 380, 56);
        p.card(d, card);
        drawProductivityWidget(sheet, WhatsApp, card, p, false);
        const bool unavailable = whatsAppFeed && whatsAppFeed->unavailable;
        const bool ready = whatsAppFeed && whatsAppFeed->count >= 0;
        d.text(unavailable ? L"Bu kurulumda Windows bildirim erişimi yok."
            : ready ? L"Sayı, Bildirim Merkezi’ndeki WhatsApp bildirimlerini gösterir."
            : L"Bildirimleri görmek için Windows’ta erişim ver.",
            box(29, 178, 380, 30), Body, tint(p.muted));
        if (!unavailable && !ready)
            button(sheet, p, box(29, 260, 380, 37), L"Bildirim erişimi ver", [this] { requestWhatsAppAccess(); });
        button(sheet, p, box(29, 314, 380, 37), L"WhatsApp’ı aç", [this] { launchWhatsApp(); }, true);
    } else {
        heading(p, L"Gün çizelgesi", L"Bugünkü kullanım, odak ve takvim");
        const auto card = box(29, 102, 380, 87);
        p.card(d, card);
        drawProductivityWidget(sheet, DayTimeline, card, p, false);
        for (size_t i = 0; i < ARRAYSIZE(Categories); ++i) {
            const float x = 33 + (i % 2) * 187.f, y = 206 + (i / 2) * 25.f;
            d.circle(x + 6, y + 9, 3.f, tint(TimelineColors[i]));
            d.text(Categories[i], box(x + 17, y, 150, 18), Caption, tint(p.muted));
        }
        d.text(L"Kırmızı · Odak     Mavi nokta · Etkinlik     Sarı · Güneş", box(29, 266, 380, 24), Caption, tint(p.muted));
        button(sheet, p, box(29, 314, 380, 37), L"Ekran süresi ayrıntıları", [this] { open(Usage); }, true);
    }
}
