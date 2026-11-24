#include "dock.h"
#include <winhttp.h>
#include <wincred.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include <iomanip>
#include <sstream>

using namespace winrt::Windows::Data::Json;

namespace {
struct Http {
    HINTERNET handle;
    ~Http() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};

Text getJson(const wchar_t* domain, const Text& path) {
    Http session{WinHttpOpen(L"Vetra/1.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0)};
    if (!session) return {};
    WinHttpSetTimeouts(session, 2500, 2500, 3500, 3500);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    Http host{WinHttpConnect(session, domain, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!host) return {};
    Http request{WinHttpOpenRequest(host, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)};
    if (!request || !WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr))
        return {};
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size, nullptr) ||
        status != 200) return {};
    std::string content;
    char buffer[4096];
    DWORD read = 0;
    do {
        if (!WinHttpReadData(request, buffer, sizeof(buffer), &read) || content.size() + read > 128 * 1024) return {};
        content.append(buffer, read);
    } while (read);
    return utf16(content);
}

struct Apartment {
    Apartment() { winrt::init_apartment(); }
    ~Apartment() { winrt::uninit_apartment(); }
};

Text clockPart(const winrt::hstring& time) {
    return time.size() >= 16 ? Text(time.c_str() + 11, 5) : Text();
}

int dayKey(const winrt::hstring& time) {
    if (time.size() < 10) return 0;
    const Text date(time.c_str(), 10);
    return std::stoi(date.substr(0, 4)) * 10000 + std::stoi(date.substr(5, 2)) * 100 + std::stoi(date.substr(8, 2));
}

int rounded(double number) { return static_cast<int>(std::lround(number)); }

double quoteNumber(const winrt::hstring& value) {
    size_t end = 0;
    const Text raw(value.c_str());
    const double number = std::stod(raw, &end);
    if (end != raw.size() || !std::isfinite(number)) throw std::invalid_argument("quote");
    return number;
}

bool symbolPart(const Text& symbol, bool share = false) {
    return !symbol.empty() && symbol.size() <= 14 && std::all_of(symbol.begin(), symbol.end(), [share](wchar_t ch) {
        return (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || (share && (ch == L'.' || ch == L'-'));
    });
}

bool cryptoPair(const Text& pair) {
    const size_t slash = pair.find(L'/');
    return slash != Text::npos && symbolPart(pair.substr(0, slash)) && symbolPart(pair.substr(slash + 1));
}

constexpr auto MarketCredential = L"Vetra/TwelveData";

bool validMarketKey(const Text& key) {
    return !key.empty() && key.size() <= 128 && std::all_of(key.begin(), key.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
    });
}

Text marketKey() {
    wchar_t key[160]{};
    const auto size = GetEnvironmentVariableW(L"VETRA_TWELVE_DATA_KEY", key, ARRAYSIZE(key));
    if (size && size < ARRAYSIZE(key) && validMarketKey(Text(key, size))) return Text(key, size);
    PCREDENTIALW credential{};
    Text value;
    if (CredReadW(MarketCredential, CRED_TYPE_GENERIC, 0, &credential)) {
        if (credential->CredentialBlobSize > 0 && credential->CredentialBlobSize % sizeof(wchar_t) == 0)
            value.assign(reinterpret_cast<const wchar_t*>(credential->CredentialBlob), credential->CredentialBlobSize / sizeof(wchar_t));
        CredFree(credential);
    }
    return validMarketKey(value) ? value : Text{};
}

std::optional<Quote> readStock(const Text& symbol, const Text& key, bool chart) {
    if (!symbolPart(symbol, true) || key.empty()) return {};
    try {
        constexpr auto host = L"api.twelvedata.com";
        const Text query = L"symbol=" + symbol + L"&apikey=" + key;
        auto json = JsonObject::Parse(getJson(host, L"/quote?" + query));
        Quote quote{quoteNumber(json.GetNamedString(L"close")), quoteNumber(json.GetNamedString(L"percent_change")),
            Text(json.GetNamedString(L"currency", L"").c_str()), {}};
        if (chart) {
            try {
                auto series = JsonObject::Parse(getJson(host, L"/time_series?interval=5min&outputsize=80&" + query));
                Text date;
                for (const auto& entry : series.GetNamedArray(L"values")) {
                    const auto bar = entry.GetObject();
                    const Text stamp = bar.GetNamedString(L"datetime").c_str();
                    if (stamp.size() < 10) continue;
                    if (date.empty()) date = stamp.substr(0, 10);
                    if (!stamp.starts_with(date)) break;
                    quote.trend.push_back(quoteNumber(bar.GetNamedString(L"close")));
                }
                std::reverse(quote.trend.begin(), quote.trend.end());
            } catch (const winrt::hresult_error&) { quote.trend.clear(); }
              catch (const std::exception&) { quote.trend.clear(); }
        }
        return quote;
    } catch (const winrt::hresult_error&) {}
      catch (const std::exception&) {}
    return {};
}

std::optional<Quote> readCrypto(const Text& pair) {
    if (!cryptoPair(pair)) return {};
    constexpr auto domain = L"data-api.binance.vision";
    auto symbol = pair;
    const auto slash = symbol.find(L'/');
    symbol.erase(slash, 1);
    try {
        const auto json = JsonObject::Parse(getJson(domain, L"/api/v3/ticker/24hr?symbol=" + symbol));
        Quote quote{quoteNumber(json.GetNamedString(L"lastPrice")), quoteNumber(json.GetNamedString(L"priceChangePercent")), pair.substr(slash + 1), {}};
        try {
            const auto candles = JsonArray::Parse(getJson(domain, L"/api/v3/klines?interval=30m&limit=48&symbol=" + symbol));
            for (const auto& candle : candles) quote.trend.push_back(quoteNumber(candle.GetArray().GetStringAt(4)));
        } catch (const winrt::hresult_error&) { quote.trend.clear(); }
          catch (const std::exception&) { quote.trend.clear(); }
        return quote;
    } catch (const winrt::hresult_error&) {}
      catch (const std::exception&) {}
    return {};
}

MarketResult readMarket(unsigned requested, std::array<Text, 4> stocks, Text crypto, Text key) {
    MarketResult result;
    result.requested = requested;
    Apartment apartment;
    if (requested & 1) result.quotes[0] = readStock(stocks[0], key, true);
    if (requested & 2) {
        for (int i = 1; i <= 3; ++i) {
            const auto earlier = std::find(stocks.begin(), stocks.begin() + i, stocks[i]);
            const int match = static_cast<int>(earlier - stocks.begin());
            if (match < i && (match != 0 || (requested & 1))) result.quotes[i] = result.quotes[match];
            else result.quotes[i] = readStock(stocks[i], key, false);
        }
    }
    if (requested & 4) result.quotes[4] = readCrypto(crypto);
    return result;
}

Text upperSymbol(Text symbol) {
    for (auto& ch : symbol) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    return symbol;
}
}

Text decimalText(double value, int decimals) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    auto text = out.str();
    std::replace(text.begin(), text.end(), L'.', L',');
    const auto decimal = text.find(L',');
    const auto whole = decimal == Text::npos ? text.size() : decimal;
    for (int i = static_cast<int>(whole) - 3; i > (value < 0 ? 1 : 0); i -= 3) text.insert(i, 1, L'.');
    return text;
}

std::optional<Forecast> readWeather(int city) {
    try {
        Apartment apartment;
        const Text path = Text(L"/v1/forecast?") + Cities[city].coordinates +
            L"&current=temperature_2m,apparent_temperature,weather_code,is_day,wind_speed_10m,precipitation"
            L"&hourly=temperature_2m,weather_code,is_day&daily=temperature_2m_max,temperature_2m_min,weather_code,sunrise,sunset"
            L"&timezone=auto&forecast_days=4";
        const auto root = JsonObject::Parse(getJson(L"api.open-meteo.com", path));
        const auto current = root.GetNamedObject(L"current");
        const auto stamp = current.GetNamedString(L"time");
        Forecast forecast;
        forecast.city = city;
        forecast.time = clockPart(stamp);
        forecast.current = rounded(current.GetNamedNumber(L"temperature_2m"));
        forecast.feels = rounded(current.GetNamedNumber(L"apparent_temperature"));
        forecast.code = rounded(current.GetNamedNumber(L"weather_code"));
        forecast.daylight = current.GetNamedNumber(L"is_day") == 1;
        forecast.wind = rounded(current.GetNamedNumber(L"wind_speed_10m"));
        forecast.precipitation = current.GetNamedNumber(L"precipitation");

        const auto daily = root.GetNamedObject(L"daily");
        const auto days = daily.GetNamedArray(L"time"), high = daily.GetNamedArray(L"temperature_2m_max"),
            low = daily.GetNamedArray(L"temperature_2m_min"), code = daily.GetNamedArray(L"weather_code"),
            rise = daily.GetNamedArray(L"sunrise"), set = daily.GetNamedArray(L"sunset");
        for (uint32_t i = 0; i < std::min(4u, days.Size()); ++i) {
            const int date = dayKey(days.GetStringAt(i));
            if (Date::fromKey(date).valid()) forecast.days.push_back({date,
                rounded(high.GetNumberAt(i)), rounded(low.GetNumberAt(i)), rounded(code.GetNumberAt(i)),
                clockPart(rise.GetStringAt(i)), clockPart(set.GetStringAt(i))});
        }
        const auto hourly = root.GetNamedObject(L"hourly");
        const auto hours = hourly.GetNamedArray(L"time"), temp = hourly.GetNamedArray(L"temperature_2m"),
            hourlyCode = hourly.GetNamedArray(L"weather_code"), light = hourly.GetNamedArray(L"is_day");
        for (uint32_t i = 0; i < hours.Size() && forecast.hours.size() < 5; ++i) {
            const auto hour = hours.GetStringAt(i);
            if (hour <= stamp) continue;
            forecast.hours.push_back({clockPart(hour), rounded(temp.GetNumberAt(i)),
                rounded(hourlyCode.GetNumberAt(i)), light.GetNumberAt(i) == 1});
        }
        if (!forecast.time.empty() && !forecast.days.empty()) return forecast;
    } catch (const winrt::hresult_error&) {}
      catch (const std::exception&) {}
    return {};
}

void Dock::requestCurrency(bool force) {
    const auto stamp = GetTickCount64();
    if (extra.exchangeRequest.valid() || (!force && stamp < extra.exchangeAt)) return;
    const int from = model.currencyFrom, to = model.currencyTo;
    if (from == to) {
        extra.exchange = {from, to, 1.0, {}};
        extra.exchangeAt = stamp + 900000;
        return;
    }
    try {
        extra.exchangeRequest = std::async(std::launch::async, [from, to] {
            ExtraWidgets::ExchangeRate rate{from, to, 0.0, {}};
            try {
                const Apartment apartment;
                const Text path = Text(L"/v2/rate/") + CurrencyCodes[from] + L"/" + CurrencyCodes[to];
                JsonObject json{nullptr};
                if (JsonObject::TryParse(getJson(L"api.frankfurter.dev", path), json)) {
                    const double value = json.GetNamedNumber(L"rate", 0.0);
                    if (std::isfinite(value) && value > 0.0) {
                        rate.rate = value;
                        rate.date = json.GetNamedString(L"date", L"").c_str();
                    }
                }
            } catch (const std::exception&) {}
            return rate;
        });
    } catch (const std::exception&) { extra.exchangeAt = stamp + 30000; }
}

void Dock::requestMarket(bool refresh) {
    if (extra.marketRequest.valid()) return;
    const auto stamp = GetTickCount64();
    const auto key = marketKey();
    unsigned requested = 0;
    for (int i = 0; i < 3; ++i) {
        if (!refresh && (stamp < extra.marketAt[i] || (page != StockChart + i && !shown(StockChart + i)))) continue;
        if (refresh && page != StockChart + i) continue;
        if (i < 2 && key.empty()) continue;
        requested |= 1u << i;
        extra.marketAt[i] = stamp + (i == 2 ? 60000 : 600000);
    }
    if (!requested) return;
    try {
        extra.marketRequest = std::async(std::launch::async, readMarket, requested, model.stockSymbols, model.cryptoSymbol, key);
    } catch (const std::system_error&) { extra.marketErrors |= requested; }
}

bool Dock::tickMarket() {
    if (!extra.marketRequest.valid() || extra.marketRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return false;
    try {
        auto market = extra.marketRequest.get();
        for (int i = 0; i < 5; ++i) {
            const int feed = i == 0 ? 0 : i == 4 ? 2 : 1;
            const unsigned bit = 1u << feed;
            if (!(market.requested & bit)) continue;
            if (i <= 1 || i == 4) extra.marketErrors &= ~bit;
            if (market.quotes[i]) extra.quotes[i] = std::move(market.quotes[i]);
            else extra.marketErrors |= bit;
        }
    } catch (const std::exception&) { extra.marketErrors = 7; }
    animateWidgets();
    return true;
}

void Dock::editMarket() {
    if (extra.marketRequest.valid()) { report(L"Veri yenileniyor. Biraz sonra tekrar dene."); return; }
    const int id = page;
    std::vector<Field> fields;
    if (id == Crypto) fields.push_back({L"Binance paritesi", model.cryptoSymbol});
    else if (id == StockChart) fields.push_back({L"Hisse sembolü", model.stockSymbols[0]});
    else for (int i = 1; i <= 3; ++i) fields.push_back({L"Hisse " + num(i), model.stockSymbols[i]});
    if (edit(Widgets[id].title, fields, false, [id](const auto& input) -> Text {
        for (const auto& field : input) {
            const Text symbol = upperSymbol(field.value);
            if (id == Crypto ? !cryptoPair(symbol) : !symbolPart(symbol, true))
                return id == Crypto ? L"Pariteyi BTC/USDT biçiminde gir." : L"Geçerli bir sembol gir. Örnek TSLA.";
        }
        return {};
    }) != 1) return;
    if (id == Crypto) { model.cryptoSymbol = upperSymbol(fields[0].value); extra.quotes[4].reset(); }
    else if (id == StockChart) { model.stockSymbols[0] = upperSymbol(fields[0].value); extra.quotes[0].reset(); }
    else for (int i = 1; i <= 3; ++i) { model.stockSymbols[i] = upperSymbol(fields[i - 1].value); extra.quotes[i].reset(); }
    extra.marketAt[id - StockChart] = 0;
    extra.marketErrors &= ~(1u << (id - StockChart));
    requestMarket(true);
    changed();
}

void Dock::editMarketKey() {
    if (extra.marketRequest.valid()) { report(L"Veri yenileniyor. Biraz sonra tekrar dene."); return; }
    PCREDENTIALW credential{};
    const bool saved = CredReadW(MarketCredential, CRED_TYPE_GENERIC, 0, &credential);
    if (saved) CredFree(credential);
    std::vector<Field> fields{{L"Twelve Data API anahtarı", {}, false, {}, true}};
    const int action = edit(L"Piyasa verisi anahtarı", fields, saved, [](const auto& input) -> Text {
        return validMarketKey(input[0].value) ? L"" : L"Geçerli bir Twelve Data API anahtarı gir.";
    });
    if (!action) return;
    bool ok;
    if (action == 2) {
        ok = CredDeleteW(MarketCredential, CRED_TYPE_GENERIC, 0) != FALSE;
    } else {
        Text& key = fields[0].value;
        CREDENTIALW entry{};
        entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = const_cast<wchar_t*>(MarketCredential);
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(key.data());
        entry.CredentialBlobSize = static_cast<DWORD>(key.size() * sizeof(wchar_t));
        entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        ok = CredWriteW(&entry, 0) != FALSE;
        SecureZeroMemory(key.data(), key.size() * sizeof(wchar_t));
    }
    if (!ok) { report(L"Windows Kimlik Bilgisi Yöneticisi güncellenemedi."); return; }
    extra.marketAt[0] = extra.marketAt[1] = 0;
    extra.marketErrors &= ~3u;
    for (size_t i = 0; i < 4; ++i) extra.quotes[i].reset();
    requestMarket();
    paintBar();
    paintSheet();
}

void Dock::marketPanel(Palette p) {
    const int index = page - StockChart;
    heading(p, Widgets[page].title, page == Crypto ? L"Binance · 24 saat" : L"Twelve Data · Hisse fiyatları");
    drawNewWidget(sheet, page, box(30, 113, sheet.w - 60, Design::CardHeight), p, false, true);
    const bool missingKey = page != Crypto && marketKey().empty();
    const bool error = (extra.marketErrors & (1u << index)) != 0;
    if (missingKey) {
        sheet.d.text(L"Fiyatlar için bir Twelve Data API anahtarı gerekir.", box(30, 190, sheet.w - 60, 30), Body, tint(p.ink));
        button(sheet, p, box(30, 230, 160, 31), L"Twelve Data’yı aç", [this] { launch(L"https://twelvedata.com/"); });
    } else {
        sheet.d.text(extra.marketRequest.valid() ? L"Veriler alınıyor…" : error ? L"Yenileme başarısız · Varsa son alınan fiyat gösteriliyor" :
            L"Fiyatların güncelliği veri sağlayıcısına bağlıdır.", box(30, 193, sheet.w - 60, 28), Caption, tint(p.muted));
    }
    button(sheet, p, box(30, 306, 151, 36), page == Crypto ? L"Pariteyi değiştir" : L"Sembolleri değiştir", [this] { editMarket(); }, !missingKey);
    if (page != Crypto) button(sheet, p, box(192, 306, 117, 36), missingKey ? L"API anahtarı ekle" : L"API anahtarı", [this] { editMarketKey(); }, missingKey);
    if (!missingKey) button(sheet, p, box(page == Crypto ? 192 : 321, 306, 84, 36), L"Yenile", [this] { requestMarket(true); paintSheet(); });
}
