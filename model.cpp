#include "model.h"
#include <filesystem>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <sstream>

namespace fs = std::filesystem;

int Date::length() const {
    constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}
int Date::weekday() const {
    SYSTEMTIME time{static_cast<WORD>(year), static_cast<WORD>(month), 0, static_cast<WORD>(day)};
    FILETIME file{};
    SystemTimeToFileTime(&time, &file);
    FileTimeToSystemTime(&file, &time);
    return time.wDayOfWeek;
}
Date Date::add(int delta) const {
    Date result = *this;
    result.day += delta;
    while (result.day < 1) {
        if (--result.month == 0) { result.month = 12; --result.year; }
        result.day += result.length();
    }
    while (result.day > result.length()) {
        result.day -= result.length();
        if (++result.month == 13) { result.month = 1; ++result.year; }
    }
    return result;
}
Text utf16(const std::string& value) {
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    Text result(length, L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}
static std::string utf8(const Text& value) {
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}
static Text encodeNote(const Text& value) {
    Text result;
    for (auto c : value) {
        if (c == L'\\') result += L"\\\\";
        else if (c == L'\r') result += L"\\r";
        else if (c == L'\n') result += L"\\n";
        else result += c;
    }
    return result;
}
static Text decodeNote(const Text& value) {
    Text result;
    for (size_t i = 0; i < value.size(); ++i) {
        auto c = value[i];
        if (c == L'\\' && i + 1 < value.size()) {
            c = value[++i];
            if (c == L'r') c = L'\r';
            else if (c == L'n') c = L'\n';
        }
        result += c;
    }
    return result;
}
Text leaf(const Text& path) {
    auto end = path.find_last_not_of(L"\\/");
    if (end == Text::npos) return path;
    auto slash = path.find_last_of(L"\\/", end);
    return path.substr(slash == Text::npos ? 0 : slash + 1, end - (slash == Text::npos ? 0 : slash + 1) + 1);
}
Text grouped(long long value) {
    Text result = num(value);
    for (int i = static_cast<int>(result.size()) - 3; i > 0; i -= 3) result.insert(i, 1, L'.');
    return result;
}
Text money(long long cents) {
    return L"₺" + grouped(cents / 100) + (cents % 100 ? L"," + two(static_cast<int>(cents % 100)) : L"");
}
Text currentBranch(const Text& repo) {
    if (repo.empty()) return {};
    auto dir = fs::path(repo) / L".git";
    std::error_code error;
    if (!fs::is_directory(dir, error)) {
        std::ifstream link(dir);
        std::string line;
        if (!std::getline(link, line) || line.compare(0, 8, "gitdir: ") != 0) return {};
        if (!line.empty() && line.back() == '\r') line.pop_back();
        dir = fs::path(repo) / fs::path(utf16(line.substr(8)));
    }
    std::ifstream file(dir / L"HEAD");
    std::string branch;
    if (!std::getline(file, branch)) return {};
    if (!branch.empty() && branch.back() == '\r') branch.pop_back();
    if (branch.compare(0, 16, "ref: refs/heads/") == 0) return utf16(branch.substr(16));
    return utf16(branch.substr(0, 8));
}
Model::Model() {
    const int defaults[][10] = {
        {StartButton, Apps, Git, NetworkWidget, System, Calendar, OpenWindows, Quick, Clock, -1},
        {StartButton, Apps, Sales, Tasks, Calendar, Storage, Quick, Clock, -1, -1},
        {StartButton, Apps, Growth, Sales, Calendar, World, Quick, Clock, -1, -1},
        {StartButton, Media, Apps, Focus, Tasks, Calendar, Countdown, Weather, Quick, Clock}
    };
    for (int p = 0; p < ProfileCount; ++p) {
        for (int id : defaults[p]) if (id >= 0) layouts[p].push_back({id, true});
        for (int id = 0; id < WidgetCount; ++id) {
            if (std::none_of(layouts[p].begin(), layouts[p].end(), [=](Tile t) { return t.id == id; }))
                layouts[p].push_back({id, dockControlWidget(id)});
        }
    }
    shortcuts = {{L"Dosyalar", L"explorer.exe"}, {L"Web", L"https://www.google.com/"}, {L"Terminal", L"cmd.exe"}, {L"Not Defteri", L"notepad.exe"}};
    wchar_t data[32768]{};
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", data, ARRAYSIZE(data));
    if (size && size < ARRAYSIZE(data)) folder = Text(data) + L"\\Vetra";
}
void Model::load() {
    if (folder.empty()) return;
    auto datPath = fs::path(folder) / L"dock.dat";
    std::error_code ec;
    if (!fs::exists(datPath, ec)) {
        const auto legacy = fs::path(folder).parent_path() / L"PrismDock" / L"dock.dat";
        if (fs::exists(legacy, ec)) datPath = legacy;
    }
    std::ifstream input(datPath, std::ios::binary);
    std::string line;
    if (!std::getline(input, line)) return;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != "Vetra 1" && line != "PrismDock 1") return;
    shortcuts.clear();
    const auto today = Date::from(now());
    const int oldestUsage = today.add(1 - UsageHistoryDays).key();
    const auto validUsageDate = [&](int day) {
        return day >= oldestUsage && day <= today.key() && Date::fromKey(day).valid();
    };
    std::array<std::vector<Tile>, ProfileCount> stored;
    std::array<std::vector<DockSlot>, ProfileCount> storedSlots;
    std::array<bool, StatusNotifications - StatusMic + 1> legacyStatus{};
    legacyStatus.fill(true);
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::string type, a, b;
        row >> type;
        if (type == "appearance") {
            int savedTheme = LiquidGlass, legacyScreen = 0, legacyAccent = 0;
            bool legacyFullscreen = false, savedTransparency = true;
            row >> profile >> savedTheme >> opacity >> size >> edge >> alignment >> legacyScreen >> legacyAccent >> topmost >> legacyFullscreen >> focusMinutes;
            bool enabled;
            if (row >> enabled) savedTransparency = enabled;
            if (row >> enabled) visibility = enabled ? DockVisibility::Edge : DockVisibility::Overlay;
            if (savedTheme == WhiteTheme) theme = WhiteTheme;
            else if (savedTheme == BlackTheme || (savedTheme == 2 && !savedTransparency)) theme = BlackTheme;
            else if (savedTheme == FrostGlass) theme = FrostGlass;
            else if (savedTheme == BareTheme) theme = BareTheme;
            else theme = LiquidGlass;
            const int legacy = std::clamp(opacity, 48, 100);
            if (!(row >> opacity)) opacity = ((legacy - 48) * 100 + 26) / 52;
            if (row >> enabled && enabled) visibility = DockVisibility::Reserved;
        } else if (type == "accent") {
            row >> accent;
        } else if (type == "statusBar") {
            row >> statusBar.enabled >> legacyStatus[0] >> statusBar.micOnlyInUse;
            for (size_t i = 1; i < legacyStatus.size(); ++i) row >> legacyStatus[i];
        } else if (type == "taskbar") {
            row >> hideTaskbar;
        } else if (type == "widgets") {
            row >> countdownMinutes >> worldCity >> weatherCity;
        } else if (type == "market") {
            for (auto& symbol : stockSymbols) if (row >> std::quoted(a)) symbol = utf16(a);
            if (row >> std::quoted(a)) cryptoSymbol = utf16(a);
        } else if (type == "aiPair") {
            int first, second, triple = -1;
            if (row >> first >> second && first >= 0 && first < AiPlanCount &&
                second >= 0 && second < AiPlanCount) {
                aiLimitFirst = first;
                aiLimitSecond = first != second ? second : first == 0 ? 1 : 0;
                if (row >> triple) aiLimitTriple = (triple != 0);
            }
        } else if (type == "aiPlan") {
            int idx = -1;
            AiOverride ov{};
            std::string r1, r2, r3, el, ev;
            if (row >> idx >> ov.p1 >> ov.p2 >> ov.p3 >> std::quoted(r1) >> std::quoted(r2) >> std::quoted(r3) >> std::quoted(el) >> std::quoted(ev) &&
                idx >= 0 && idx < AiPlanCount) {
                ov.r1 = utf16(r1); ov.r2 = utf16(r2); ov.r3 = utf16(r3);
                ov.extraLabel = utf16(el); ov.extraVal = utf16(ev);
                aiOverrides[idx] = std::move(ov);
            }
        } else if (type == "ghUser" && row >> std::quoted(a)) {
            githubUser = utf16(a);
        } else if (type == "xProf" && row >> std::quoted(a) >> xFollowers) {
            xHandle = utf16(a);
        } else if (type == "stripeCfg") {
            row >> stripeRevenue >> stripeChange;
        } else if (type == "dataFastCfg") {
            row >> dataFastVisitors >> dataFastChange;
        } else if (type == "dsBal") {
            row >> deepSeekBalance >> deepSeekToday >> deepSeekWeek;
        } else if (type == "usage") {
            row >> usageDay;
            for (auto& seconds : usageSeconds) {
                row >> seconds;
                seconds = std::clamp(seconds, 0, 3600);
            }
        } else if (type == "usageDay") {
            UsageDaily entry{};
            if (row >> entry.day >> entry.seconds && validUsageDate(entry.day) && entry.seconds >= 0 && entry.seconds <= 86400 &&
                std::none_of(usageHistory.begin(), usageHistory.end(), [&](const auto& item) { return item.day == entry.day; }))
                usageHistory.push_back(entry);
        } else if (type == "usageTimeline") {
            int day{}, kind{};
            if (row >> day >> kind && day == usageDay && kind >= 0 && kind < static_cast<int>(timelineUsage.size()))
                for (auto& seconds : timelineUsage[kind]) { row >> seconds; seconds = std::clamp(seconds, 0, 3600); }
        } else if (type == "focusTimeline") {
            int day{};
            if (row >> day && day == usageDay)
                for (auto& rounds : timelineFocus) { row >> rounds; rounds = std::clamp(rounds, 0, 60); }
        } else if (type == "currency") {
            double amount{};
            int from{}, to{};
            if (row >> amount >> from >> to && std::isfinite(amount) && amount >= 0 && amount <= 1000000000 &&
                from >= 0 && from < ARRAYSIZE(CurrencyCodes) && to >= 0 && to < ARRAYSIZE(CurrencyCodes)) {
                currencyAmount = amount; currencyFrom = from; currencyTo = to;
            }
        } else if (type == "usageApp") {
            UsageApp entry{};
            if (row >> entry.day >> entry.seconds >> std::quoted(a) >> std::quoted(b) && validUsageDate(entry.day) &&
                entry.seconds > 0 && entry.seconds <= 86400 && !a.empty() && !b.empty() &&
                usageApps.size() < UsageHistoryDays * MaxUsageAppsPerDay) {
                entry.key = utf16(a);
                entry.label = utf16(b);
                usageApps.push_back(std::move(entry));
            }
        } else if (type == "tile") {
            int p, id; bool enabled;
            if (row >> p >> id >> enabled && p >= 0 && p < ProfileCount && id >= 0 && id < WidgetCount &&
                std::none_of(stored[p].begin(), stored[p].end(), [=](Tile tile) { return tile.id == id; }))
                stored[p].push_back({id, enabled});
        } else if (type == "slot") {
            int p; std::string kind;
            if (row >> p >> kind && p >= 0 && p < ProfileCount) {
                if (kind == "w") {
                    int id;
                    if (row >> id && id >= 0 && id < WidgetCount && id != Apps &&
                        std::none_of(storedSlots[p].begin(), storedSlots[p].end(), [=](const DockSlot& s) { return !s.isApp && s.widget == id; }))
                        storedSlots[p].push_back({false, id, {}});
                } else if (kind == "a" && row >> std::quoted(a) && !a.empty()) {
                    Text key = utf16(a);
                    if (std::none_of(storedSlots[p].begin(), storedSlots[p].end(), [&](const DockSlot& s) {
                        return s.isApp && CompareStringOrdinal(s.appKey.c_str(), -1, key.c_str(), -1, TRUE) == CSTR_EQUAL;
                    }))
                        storedSlots[p].push_back({true, -1, std::move(key)});
                }
            }
        } else if ((type == "app" || type == "recent") && row >> std::quoted(a) >> std::quoted(b)) {
            std::string identity;
            row >> std::quoted(identity);
            auto& items = type == "app" ? shortcuts : recentApps;
            if (items.size() < (type == "app" ? MaxShortcuts : 12) && !b.empty())
                items.push_back({utf16(a), utf16(b), utf16(identity)});
        } else if (type == "repo" && row >> std::quoted(a)) {
            repo = utf16(a);
        } else if (type == "note" || type == "note2") {
            int p;
            if (row >> p >> std::quoted(a) && p >= 0 && p < ProfileCount)
                notes[p] = type == "note2" ? decodeNote(utf16(a)) : utf16(a);
        } else if (type == "event") {
            Event e{};
            if (row >> e.day >> e.kind >> std::quoted(a) >> std::quoted(b) && Date::fromKey(e.day).valid() &&
                e.kind >= 0 && e.kind < 3 && events.size() < 2000) {
                e.time = utf16(a); e.title = utf16(b); events.push_back(e);
            }
        } else if (type == "task") {
            Task t{};
            if (row >> t.profile >> t.complete >> std::quoted(a) && t.profile >= 0 && t.profile < ProfileCount && tasks.size() < 2000) {
                t.title = utf16(a); tasks.push_back(t);
            }
        } else if (type == "metric") {
            Metric m{};
            if (row >> m.day >> m.widget >> m.first >> m.second && Date::fromKey(m.day).valid() && (m.widget == Sales || m.widget == Growth) &&
                m.first >= 0 && m.first <= 99999999999LL && m.second >= 0 && m.second <= 999999999 && metrics.size() < 2000)
                metrics.push_back(m);
        }
    }
    for (int p = 0; p < ProfileCount; ++p) {
        if (!stored[p].empty()) {
            const bool hadStart = std::any_of(stored[p].begin(), stored[p].end(),
                [](Tile current) { return current.id == StartButton; });
            for (auto tile : layouts[p]) {
                if (statusWidget(tile.id)) tile.enabled = legacyStatus[tile.id - StatusMic];
                if (std::none_of(stored[p].begin(), stored[p].end(), [=](Tile current) { return current.id == tile.id; })) {
                    if (tile.id == StartButton) stored[p].insert(stored[p].begin(), tile);
                    else stored[p].push_back(tile);
                }
            }
            layouts[p] = std::move(stored[p]);
            if (!hadStart && !storedSlots[p].empty() &&
                std::none_of(storedSlots[p].begin(), storedSlots[p].end(), [](const DockSlot& s) { return !s.isApp && s.widget == StartButton; })) {
                storedSlots[p].insert(storedSlots[p].begin(), {false, StartButton, {}});
            }
        } else {
            for (auto& tile : layouts[p])
                if (statusWidget(tile.id)) tile.enabled = legacyStatus[tile.id - StatusMic];
        }
        if (!storedSlots[p].empty()) dockOrder[p] = std::move(storedSlots[p]);
    }
    profile = std::clamp(profile, 0, ProfileCount - 1);
    opacity = std::clamp(opacity, 0, 100); size = std::clamp(size, 0, 2);
    edge = std::clamp(edge, 0, 1); alignment = std::clamp(alignment, 0, 2);
    focusMinutes = std::clamp(focusMinutes, 1, FocusMaxMinutes);
    accent = std::clamp(accent, 0, static_cast<int>(ARRAYSIZE(Accents)) - 1);
    countdownMinutes = std::clamp(countdownMinutes, 1, CountdownMaxMinutes);
    worldCity = std::clamp(worldCity, 0, static_cast<int>(ARRAYSIZE(Cities)) - 1);
    weatherCity = std::clamp(weatherCity, -1, static_cast<int>(ARRAYSIZE(Cities)) - 1);
    rollUsageDay(today);
}

void Model::rollUsageDay(Date day) {
    const int key = day.key(), oldest = day.add(1 - UsageHistoryDays).key();
    if (usageDay == key) return;
    std::erase_if(usageHistory, [&](const auto& item) { return item.day < oldest || item.day >= key || item.day == usageDay; });
    std::erase_if(usageApps, [&](const auto& item) { return item.day < oldest || item.day > key; });
    if (usageDay >= oldest && usageDay < key && Date::fromKey(usageDay).valid())
        usageHistory.push_back({usageDay, std::accumulate(usageSeconds.begin(), usageSeconds.end(), 0)});
    usageDay = key;
    usageSeconds.fill(0);
    timelineUsage = {};
    timelineFocus = {};
}

int Model::usageFor(int day) const {
    if (day == usageDay) return std::accumulate(usageSeconds.begin(), usageSeconds.end(), 0);
    const auto item = std::find_if(usageHistory.begin(), usageHistory.end(), [&](const auto& entry) { return entry.day == day; });
    return item == usageHistory.end() ? 0 : item->seconds;
}
bool Model::save() const {
    if (folder.empty()) return false;
    std::error_code error;
    fs::create_directories(folder, error);
    auto path = fs::path(folder) / L"dock.dat";
    auto temp = fs::path(folder) / L"dock.tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << "Vetra 1\nappearance " << profile << ' ' << theme << ' ' << (48 + (opacity * 52 + 50) / 100) << ' ' << size << ' ' << edge << ' '
        << alignment << " 0 0 " << topmost << " 1 " << focusMinutes << ' ' << glassTheme(theme) << ' ' << autoHides() << ' ' << opacity << ' ' << (visibility == DockVisibility::Reserved) << '\n';
    out << "accent " << accent << '\n';
    const auto enabledStatus = [&](int id) {
        const auto& layout = layouts[profile];
        return std::any_of(layout.begin(), layout.end(), [id](Tile tile) { return tile.id == id && tile.enabled; });
    };
    out << "statusBar " << statusBar.enabled << ' ' << enabledStatus(StatusMic) << ' ' << statusBar.micOnlyInUse;
    for (int id = StatusKeyboard; id <= StatusNotifications; ++id) out << ' ' << enabledStatus(id);
    out << '\n';
    out << "taskbar " << hideTaskbar << '\n';
    out << "widgets " << countdownMinutes << ' ' << worldCity << ' ' << weatherCity << '\n';
    out << "market";
    for (const auto& symbol : stockSymbols) out << ' ' << std::quoted(utf8(symbol));
    out << ' ' << std::quoted(utf8(cryptoSymbol)) << '\n';
    out << "aiPair " << aiLimitFirst << ' ' << aiLimitSecond << ' ' << (aiLimitTriple ? 1 : 0) << '\n';
    for (int i = 0; i < AiPlanCount; ++i) {
        const auto& ov = aiOverrides[i];
        if (ov.p1 >= 0 || ov.p2 >= 0 || ov.p3 >= 0 || !ov.r1.empty() || !ov.r2.empty() || !ov.r3.empty() ||
            !ov.extraLabel.empty() || !ov.extraVal.empty()) {
            out << "aiPlan " << i << ' ' << ov.p1 << ' ' << ov.p2 << ' ' << ov.p3 << ' '
                << std::quoted(utf8(ov.r1)) << ' ' << std::quoted(utf8(ov.r2)) << ' ' << std::quoted(utf8(ov.r3)) << ' '
                << std::quoted(utf8(ov.extraLabel)) << ' ' << std::quoted(utf8(ov.extraVal)) << '\n';
        }
    }
    if (!githubUser.empty()) out << "ghUser " << std::quoted(utf8(githubUser)) << '\n';
    if (!xHandle.empty() || xFollowers >= 0) out << "xProf " << std::quoted(utf8(xHandle)) << ' ' << xFollowers << '\n';
    if (stripeRevenue >= 0) out << "stripeCfg " << stripeRevenue << ' ' << stripeChange << '\n';
    if (dataFastVisitors >= 0) out << "dataFastCfg " << dataFastVisitors << ' ' << dataFastChange << '\n';
    if (deepSeekBalance >= 0) out << "dsBal " << deepSeekBalance << ' ' << deepSeekToday << ' ' << deepSeekWeek << '\n';
    out << "usage " << usageDay;
    for (int seconds : usageSeconds) out << ' ' << seconds;
    out << '\n';
    for (size_t kind = 0; kind < timelineUsage.size(); ++kind) {
        out << "usageTimeline " << usageDay << ' ' << kind;
        for (int seconds : timelineUsage[kind]) out << ' ' << seconds;
        out << '\n';
    }
    out << "focusTimeline " << usageDay;
    for (int rounds : timelineFocus) out << ' ' << rounds;
    out << "\ncurrency " << std::setprecision(15) << currencyAmount << ' ' << currencyFrom << ' ' << currencyTo << '\n';
    for (const auto& item : usageHistory) out << "usageDay " << item.day << ' ' << item.seconds << '\n';
    for (const auto& item : usageApps) out << "usageApp " << item.day << ' ' << item.seconds << ' '
        << std::quoted(utf8(item.key)) << ' ' << std::quoted(utf8(item.label)) << '\n';
    for (int p = 0; p < ProfileCount; ++p) {
        for (auto tile : layouts[p]) out << "tile " << p << ' ' << tile.id << ' ' << tile.enabled << '\n';
        for (const auto& slot : dockOrder[p]) {
            if (!slot.isApp && slot.widget >= 0 && slot.widget < WidgetCount && slot.widget != Apps)
                out << "slot " << p << " w " << slot.widget << '\n';
            else if (slot.isApp && !slot.appKey.empty())
                out << "slot " << p << " a " << std::quoted(utf8(slot.appKey)) << '\n';
        }
        out << "note2 " << p << ' ' << std::quoted(utf8(encodeNote(notes[p]))) << '\n';
    }
    for (const auto& app : shortcuts)
        out << "app " << std::quoted(utf8(app.label)) << ' ' << std::quoted(utf8(app.target)) << ' ' << std::quoted(utf8(app.identity)) << '\n';
    for (const auto& app : recentApps)
        out << "recent " << std::quoted(utf8(app.label)) << ' ' << std::quoted(utf8(app.target)) << ' ' << std::quoted(utf8(app.identity)) << '\n';
    for (const auto& e : events) out << "event " << e.day << ' ' << e.kind << ' ' << std::quoted(utf8(e.time)) << ' ' << std::quoted(utf8(e.title)) << '\n';
    for (const auto& t : tasks) out << "task " << t.profile << ' ' << t.complete << ' ' << std::quoted(utf8(t.title)) << '\n';
    for (const auto& m : metrics) out << "metric " << m.day << ' ' << m.widget << ' ' << m.first << ' ' << m.second << '\n';
    out.close();
    return out.good() && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
static constexpr auto RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool Model::startup() const {
    wchar_t path[32768]{}, command[32768]{};
    DWORD bytes = sizeof(command);
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return RegGetValueW(HKEY_CURRENT_USER, RunKey, L"Vetra", RRF_RT_REG_SZ, nullptr, command, &bytes) == ERROR_SUCCESS &&
        Text(command) == Text(L"\"") + path + L"\"";
}
bool Model::setStartup(bool enable) const {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    RegDeleteValueW(key, L"PrismDock");
    LONG result;
    if (enable) {
        wchar_t path[32768]{};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
        Text command = Text(L"\"") + path + L"\"";
        result = RegSetValueExW(key, L"Vetra", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, L"Vetra");
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS || (!enable && result == ERROR_FILE_NOT_FOUND);
}
std::array<long long, 2> Model::total(int widget, Date day) const {
    std::array<long long, 2> value{};
    for (auto m : metrics) if (m.widget == widget && m.day == day.key()) { value[0] += m.first; value[1] += m.second; }
    return value;
}

