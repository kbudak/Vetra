#include "dock.h"
#include <pdh.h>
#include <pdhmsg.h>
#include <winhttp.h>
#include <wincred.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <regex>
#include <sstream>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

namespace {
constexpr auto CredDeepSeek = L"Vetra/DeepSeek";
constexpr auto CredStripe   = L"Vetra/Stripe";
constexpr auto CredDataFast = L"Vetra/DataFast";

Text formatCompact(long long n) {
    if (n < 0) return L"—";
    wchar_t buf[32]{};
    if (n >= 1000000000LL) { swprintf_s(buf, L"%.1fB", n / 1e9); return buf; }
    if (n >= 1000000LL) { swprintf_s(buf, L"%.1fM", n / 1e6); return buf; }
    if (n >= 1000LL) { swprintf_s(buf, L"%.1fK", n / 1e3); return buf; }
    return num(n);
}

Text formatUsd(double v, bool decimals = true) {
    wchar_t buf[48]{};
    swprintf_s(buf, decimals ? L"$%.2f" : L"$%.0f", v);
    Text s(buf);
    const auto dot = s.find(L'.');
    const size_t wholeEnd = dot == Text::npos ? s.size() : dot;
    for (int i = static_cast<int>(wholeEnd) - 3; i > 1; i -= 3) s.insert(i, 1, L',');
    return s;
}

Text formatCountComma(long long n) {
    if (n < 0) return L"—";
    Text s = num(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, 1, L',');
    return s;
}

time_t parseIsoUtc(std::string_view s) {
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || (s[10] != 'T' && s[10] != ' ') || s[13] != ':' || s[16] != ':')
        return 0;
    auto dig = [&](size_t pos, int len) -> int {
        int v = 0;
        for (int i = 0; i < len; ++i) {
            const char c = s[pos + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    const int yr = dig(0, 4), mo = dig(5, 2), dy = dig(8, 2), hr = dig(11, 2), mi = dig(14, 2), sc = dig(17, 2);
    if (yr < 2020 || mo < 1 || mo > 12 || dy < 1 || dy > 31 || hr < 0 || hr > 23 || mi < 0 || mi > 59 || sc < 0 || sc > 59)
        return 0;
    tm t{};
    t.tm_year = yr - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = dy;
    t.tm_hour = hr;
    t.tm_min = mi;
    t.tm_sec = sc;
    time_t epoch = _mkgmtime(&t);
    if (epoch <= 0) return 0;
    // Handle optional timezone offset (+HH:MM / -HH:MM) after seconds/fractional seconds
    size_t tz = 19;
    if (tz < s.size() && s[tz] == '.') {
        ++tz;
        while (tz < s.size() && s[tz] >= '0' && s[tz] <= '9') ++tz;
    }
    if (tz + 5 < s.size() && (s[tz] == '+' || s[tz] == '-') && s[tz + 3] == ':') {
        const int offH = dig(tz + 1, 2), offM = dig(tz + 4, 2);
        if (offH >= 0 && offM >= 0) {
            const long long offSec = offH * 3600LL + offM * 60LL;
            epoch += (s[tz] == '+') ? -offSec : offSec;
        }
    }
    return epoch;
}

time_t fileTimeToEpoch(fs::file_time_type ft) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(fs::file_time_type::clock::now() - ft).count();
    return std::time(nullptr) - static_cast<time_t>(std::max<long long>(0, age));
}

Text formatResetCountdown(time_t resetAt, int pctLeft, int windowMins) {
    if (resetAt <= 0) return L"—";
    const time_t nowT = std::time(nullptr);
    const long long diff = static_cast<long long>(resetAt) - static_cast<long long>(nowT);
    if (diff <= 0) return L"—";
    if (pctLeft >= 100 && windowMins > 0 && diff >= static_cast<long long>(windowMins) * 60 - 180)
        return L"—";
    const long long mins = (diff + 30) / 60;
    if (mins <= 0) return L"<1m";
    if (mins < 60) return num(mins) + L"m";
    const long long hrs = mins / 60;
    if (hrs < 24) return num(hrs) + L"h " + num(mins % 60) + L"m";
    const long long days = hrs / 24;
    return num(days) + L"d " + num(hrs % 24) + L"h";
}

Text formatResetClock(time_t resetAt) {
    if (resetAt <= 0) return {};
    const time_t nowT = std::time(nullptr);
    if (resetAt <= nowT) return {};
    tm loc{}, nowLoc{};
    localtime_s(&loc, &resetAt);
    localtime_s(&nowLoc, &nowT);
    const Text hhmm = two(loc.tm_hour) + L":" + two(loc.tm_min);
    if (loc.tm_year == nowLoc.tm_year && loc.tm_yday == nowLoc.tm_yday) return hhmm;
    return Text(ShortDays[std::clamp(loc.tm_wday, 0, 6)]) + L" " + hhmm;
}

Text formatUpdatedTime(time_t updatedAt) {
    if (updatedAt <= 0) return {};
    tm loc{};
    localtime_s(&loc, &updatedAt);
    return two(loc.tm_hour) + L":" + two(loc.tm_min) + L":" + two(loc.tm_sec);
}

bool getJsonObject(const JsonObject& parent, const wchar_t* key, JsonObject& out) {
    if (!parent.HasKey(key)) return false;
    const auto v = parent.Lookup(key);
    if (!v || v.ValueType() != JsonValueType::Object) return false;
    out = v.GetObject();
    return true;
}

double getJsonNum(const JsonObject& obj, const wchar_t* key, double def = -1.0) {
    if (!obj.HasKey(key)) return def;
    const auto v = obj.Lookup(key);
    if (!v || v.ValueType() != JsonValueType::Number) return def;
    return v.GetNumber();
}

Text getJsonStr(const JsonObject& obj, const wchar_t* key) {
    if (!obj.HasKey(key)) return {};
    const auto v = obj.Lookup(key);
    if (!v || v.ValueType() != JsonValueType::String) return {};
    return Text(v.GetString().c_str());
}

Text shortModelName(const Text& raw) {
    if (raw.empty()) return L"—";
    Text s = raw;
    if (s.rfind(L"claude-", 0) == 0) s = s.substr(7);
    for (size_t i = 0; i < s.size(); ++i) if (s[i] == L'-') s[i] = L' ';
    if (!s.empty() && s[0] >= L'a' && s[0] <= L'z') s[0] = static_cast<wchar_t>(s[0] - L'a' + L'A');
    if (s.size() > 14) s.resize(14);
    return s;
}

Text envDir(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n && n < ARRAYSIZE(buf) ? Text(buf, n) : Text{};
}

Text readCred(const wchar_t* target) {
    PCREDENTIALW cred{};
    Text val;
    if (CredReadW(target, CRED_TYPE_GENERIC, 0, &cred)) {
        if (cred->CredentialBlobSize > 0 && cred->CredentialBlobSize % sizeof(wchar_t) == 0)
            val.assign(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
        CredFree(cred);
    }
    return val;
}

bool writeCred(const wchar_t* target, const Text& val) {
    CREDENTIALW entry{};
    entry.Type = CRED_TYPE_GENERIC;
    entry.TargetName = const_cast<wchar_t*>(target);
    entry.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(val.data()));
    entry.CredentialBlobSize = static_cast<DWORD>(val.size() * sizeof(wchar_t));
    entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&entry, 0) != FALSE;
}

Text httpGet(const wchar_t* domain, const Text& path, const Text& authHeader = {}, size_t maxBytes = 512 * 1024) {
    struct H { HINTERNET h{}; ~H() { if (h) WinHttpCloseHandle(h); } operator HINTERNET() const { return h; } };
    H session{WinHttpOpen(L"Vetra/1.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0)};
    if (!session) return {};
    WinHttpSetTimeouts(session, 3000, 3000, 4000, 4000);
    H host{WinHttpConnect(session, domain, INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!host) return {};
    H req{WinHttpOpenRequest(host, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)};
    if (!req) return {};
    Text hdr = L"Accept: application/json, text/html\r\n";
    if (!authHeader.empty()) hdr += authHeader + L"\r\n";
    if (!WinHttpSendRequest(req, hdr.c_str(), static_cast<DWORD>(hdr.size()), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr))
        return {};
    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr) ||
        status != 200)
        return {};
    std::string out;
    char buf[4096];
    DWORD rd = 0;
    do {
        if (!WinHttpReadData(req, buf, sizeof(buf), &rd) || out.size() + rd > maxBytes) return {};
        out.append(buf, rd);
    } while (rd);
    return utf16(out);
}

// ---------- 1. Live CPU Temperature via persistent PDH query (no elevation needed!) ----------
struct PersistentPdhTemp {
    std::mutex mutex;
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    std::vector<BYTE> buffer;
    bool attempted = false;

    ~PersistentPdhTemp() {
        if (query) { PdhCloseQuery(query); query = nullptr; counter = nullptr; }
    }

    int readC() {
        std::lock_guard lock(mutex);
        if (!query) {
            if (attempted) return -1;
            attempted = true;
            if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return -1; }
            if (PdhAddEnglishCounterW(query, L"\\Thermal Zone Information(*)\\Temperature", 0, &counter) != ERROR_SUCCESS) {
                PdhCloseQuery(query);
                query = nullptr;
                counter = nullptr;
                return -1;
            }
        }
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return -1;
        DWORD bufSize = static_cast<DWORD>(buffer.size());
        DWORD itemCount = 0;
        LONG st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &itemCount,
            bufSize ? reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()) : nullptr);
        if (st == static_cast<LONG>(PDH_MORE_DATA) && bufSize > 0) {
            buffer.resize(bufSize);
            st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bufSize, &itemCount,
                reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data()));
        }
        if (st != ERROR_SUCCESS || itemCount == 0) return -1;
        const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        int bestC = -1;
        for (DWORD i = 0; i < itemCount; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
            const double val = items[i].FmtValue.doubleValue;
            const int c = val > 200.0 ? static_cast<int>(std::lround(val - 273.15)) : static_cast<int>(std::lround(val));
            if (c >= 15 && c <= 115 && c > bestC) bestC = c;
        }
        return bestC;
    }
};

int readCpuTempC() {
    static PersistentPdhTemp pdh;
    return pdh.readC();
}

// Persistent Windows Task Manager-matched CPU utility counter (\Processor Information(_Total)\% Processor Utility)
struct PersistentPdhCpu {
    std::mutex mutex;
    PDH_HQUERY query{};
    PDH_HCOUNTER counter{};
    std::atomic<bool> ready{false};
    bool attempted = false;
    bool primed = false;
    ULONGLONG lastCollectAt = 0;
    int lastPct = -1;

    ~PersistentPdhCpu() {
        if (query) { PdhCloseQuery(query); query = nullptr; counter = nullptr; }
    }

    void primeAsync() {
        if (ready.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex);
        if (query || attempted) return;
        attempted = true;
        if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS) { query = nullptr; return; }
        if (PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Utility", 0, &counter) != ERROR_SUCCESS &&
            PdhAddEnglishCounterW(query, L"\\Processor(_Total)\\% Processor Time", 0, &counter) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = nullptr;
            counter = nullptr;
            return;
        }
        if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
            primed = true;
            lastCollectAt = GetTickCount64();
        }
        ready.store(true, std::memory_order_release);
    }

    int readPct() {
        // Never delay-load/initialize PDH synchronously on the UI thread during startup.
        if (!ready.load(std::memory_order_acquire)) return -1;
        std::lock_guard lock(mutex);
        if (!query) return -1;
        const ULONGLONG nowTick = GetTickCount64();
        // Avoid tiny-delta jitter spikes when sample() is triggered back-to-back (e.g., opening a panel).
        if (primed && lastCollectAt && nowTick - lastCollectAt < 500ULL) return lastPct;
        if (PdhCollectQueryData(query) != ERROR_SUCCESS) return lastPct;
        lastCollectAt = nowTick;
        if (!primed) { primed = true; return lastPct; }
        PDH_FMT_COUNTERVALUE value{};
        if (PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value) == ERROR_SUCCESS &&
            (value.CStatus == ERROR_SUCCESS || value.CStatus == PDH_CSTATUS_NEW_DATA)) {
            if (std::isfinite(value.doubleValue)) {
                lastPct = std::clamp(static_cast<int>(std::lround(value.doubleValue)), 0, 100);
            }
        }
        return lastPct;
    }
};

PersistentPdhCpu& sharedPdhCpu() {
    static PersistentPdhCpu pdhCpu;
    return pdhCpu;
}

int readCpuUtilityPctInternal() {
    return sharedPdhCpu().readPct();
}

Text userProfileDir() { return envDir(L"USERPROFILE"); }

// ---------- 2A. Official Codex app-server CLI JSON-RPC live rate limits ----------
Text findCodexAppServerCmd() {
    static std::mutex cacheMu;
    static Text cachedCmd;
    static ULONGLONG cachedAt = 0;
    const ULONGLONG nowTick = GetTickCount64();
    {
        std::lock_guard lock(cacheMu);
        if (cachedAt && nowTick - cachedAt < 300000ULL) return cachedCmd;
    }
    Text result;
    std::error_code ec;
    const Text appData = envDir(L"APPDATA");
    if (!appData.empty()) {
        const fs::path npmWin64 = fs::path(appData) / L"npm" / L"node_modules" / L"@openai" / L"codex" /
            L"node_modules" / L"@openai" / L"codex-win32-x64" / L"vendor" / L"x86_64-pc-windows-msvc" / L"bin" / L"codex.exe";
        if (fs::exists(npmWin64, ec)) result = L"\"" + npmWin64.wstring() + L"\" app-server";
        if (result.empty()) {
            const fs::path npmArm64 = fs::path(appData) / L"npm" / L"node_modules" / L"@openai" / L"codex" /
                L"node_modules" / L"@openai" / L"codex-win32-arm64" / L"vendor" / L"aarch64-pc-windows-msvc" / L"bin" / L"codex.exe";
            if (fs::exists(npmArm64, ec)) result = L"\"" + npmArm64.wstring() + L"\" app-server";
        }
    }
    wchar_t found[MAX_PATH]{};
    if (result.empty() && SearchPathW(nullptr, L"codex.exe", nullptr, ARRAYSIZE(found), found, nullptr))
        result = L"\"" + Text(found) + L"\" app-server";
    if (result.empty()) {
        const Text home = userProfileDir();
        if (!home.empty()) {
            const fs::path cargo = fs::path(home) / L".cargo" / L"bin" / L"codex.exe";
            if (fs::exists(cargo, ec)) result = L"\"" + cargo.wstring() + L"\" app-server";
        }
    }
    if (result.empty() && SearchPathW(nullptr, L"codex.cmd", nullptr, ARRAYSIZE(found), found, nullptr))
        result = L"cmd.exe /c \"\"" + Text(found) + L"\" app-server\"";
    {
        std::lock_guard lock(cacheMu);
        cachedCmd = result;
        cachedAt = nowTick;
    }
    return result;
}

bool parseCodexLimitWindow(const JsonObject& rl, const wchar_t* key, int& pctLeft, time_t& resetsAt, int& winMins) {
    JsonObject w{nullptr};
    if (!getJsonObject(rl, key, w)) return false;
    double used = getJsonNum(w, L"usedPercent", -1.0);
    if (used < 0.0) used = getJsonNum(w, L"used_percent", -1.0);
    if (used < 0.0) return false;
    double reset = getJsonNum(w, L"resetsAt", 0.0);
    if (reset <= 0.0) reset = getJsonNum(w, L"resets_at", 0.0);
    double mins = getJsonNum(w, L"windowDurationMins", 0.0);
    if (mins <= 0.0) mins = getJsonNum(w, L"window_minutes", 0.0);
    pctLeft = std::clamp(100 - static_cast<int>(std::lround(used)), 0, 100);
    resetsAt = static_cast<time_t>(std::llround(reset));
    winMins = static_cast<int>(std::lround(mins));
    const time_t nowT = std::time(nullptr);
    if (resetsAt > 0 && resetsAt <= nowT) {
        pctLeft = 100;
        resetsAt = 0;
    }
    return true;
}

AiPlanLive fetchCodexAppServer(std::shared_ptr<std::atomic<bool>> stop) {
    AiPlanLive out;
    Text cmd = findCodexAppServerCmd();
    if (cmd.empty()) return out;

    struct Guard {
        HANDLE inRd{}, inWr{}, outRd{}, outWr{}, nul{};
        PROCESS_INFORMATION pi{};
        ~Guard() {
            if (inWr) CloseHandle(inWr);
            if (inRd) CloseHandle(inRd);
            if (outRd) CloseHandle(outRd);
            if (outWr) CloseHandle(outWr);
            if (nul) CloseHandle(nul);
            if (pi.hProcess) {
                if (WaitForSingleObject(pi.hProcess, 450) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 0);
                CloseHandle(pi.hProcess);
            }
            if (pi.hThread) CloseHandle(pi.hThread);
        }
    } g;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&g.inRd, &g.inWr, &sa, 0) || !CreatePipe(&g.outRd, &g.outWr, &sa, 0)) return out;
    SetHandleInformation(g.inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g.outRd, HANDLE_FLAG_INHERIT, 0);
    g.nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = g.inRd;
    si.hStdOutput = g.outWr;
    si.hStdError = g.nul ? g.nul : g.outWr;

    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &g.pi))
        return out;
    CloseHandle(g.inRd); g.inRd = nullptr;
    CloseHandle(g.outWr); g.outWr = nullptr;
    if (g.nul) { CloseHandle(g.nul); g.nul = nullptr; }

    auto writeAll = [&](std::string_view s) {
        DWORD written = 0;
        return g.inWr && WriteFile(g.inWr, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) != FALSE;
    };

    const char* initReq = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"vetra\",\"version\":\"1.2.0\"}}}\n";
    if (!writeAll(initReq)) return out;

    std::string buf;
    bool sentRead = false;
    const ULONGLONG deadline = GetTickCount64() + 5500;
    while (GetTickCount64() < deadline && (!stop || !stop->load())) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g.outRd, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            if (WaitForSingleObject(g.pi.hProcess, 25) == WAIT_OBJECT_0) break;
            continue;
        }
        char tmp[4096];
        DWORD rd = 0;
        if (!ReadFile(g.outRd, tmp, std::min<DWORD>(avail, sizeof(tmp)), &rd, nullptr) || rd == 0) break;
        buf.append(tmp, rd);

        size_t nl = 0;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (line.empty()) continue;
            JsonObject msg{nullptr};
            if (!JsonObject::TryParse(utf16(line), msg)) continue;
            const double id = getJsonNum(msg, L"id", -1.0);
            if (id == 1.0 && !sentRead) {
                sentRead = true;
                writeAll("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\"}\n"
                         "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":{}}\n");
            } else if (id == 2.0) {
                JsonObject res{nullptr}, rl{nullptr}, byId{nullptr}, codexRl{nullptr};
                if (getJsonObject(msg, L"result", res)) {
                    bool gotObj = false;
                    if (getJsonObject(res, L"rateLimitsByLimitId", byId) && getJsonObject(byId, L"codex", codexRl)) {
                        rl = codexRl;
                        gotObj = true;
                    } else if (getJsonObject(res, L"rateLimits", rl)) {
                        gotObj = true;
                    }
                    if (gotObj) {
                        bool got1 = parseCodexLimitWindow(rl, L"primary", out.p1, out.r1At, out.r1WinMins);
                        bool got2 = parseCodexLimitWindow(rl, L"secondary", out.p2, out.r2At, out.r2WinMins);
                        if (got1 || got2) {
                            out.planType = getJsonStr(rl, L"planType");
                            out.ready = true;
                            out.hasLimits = true;
                            out.sourceLabel = L"Codex canlı (app-server)";
                            out.updatedAt = std::time(nullptr);
                        }
                    }
                }
                if (g.inWr) { CloseHandle(g.inWr); g.inWr = nullptr; }
                return out;
            }
        }
    }
    return out;
}

time_t parseJsonTime(const JsonObject& obj, const wchar_t* key) {
    if (!obj.HasKey(key)) return 0;
    const auto v = obj.Lookup(key);
    if (!v) return 0;
    if (v.ValueType() == JsonValueType::Number) {
        long long n = static_cast<long long>(std::llround(v.GetNumber()));
        if (n > 1000000000000LL) n /= 1000LL; // accept Unix ms transparently
        return n > 0 ? static_cast<time_t>(n) : 0;
    }
    if (v.ValueType() == JsonValueType::String) {
        const Text w(v.GetString().c_str());
        std::string a(w.begin(), w.end());
        return parseIsoUtc(a);
    }
    return 0;
}

AiPlanLive parseAntigravityUsage(const Text& response) {
    AiPlanLive out;
    JsonObject root{nullptr}, command{nullptr}, data{nullptr};
    if (!JsonObject::TryParse(response, root) ||
        !getJsonObject(root, L"command", command) || !getJsonObject(command, L"data", data)) return out;

    const auto groups = data.GetNamedArray(L"groups", nullptr);
    if (!groups) return out;
    for (const auto& groupValue : groups) {
        if (groupValue.ValueType() != JsonValueType::Object) continue;
        const auto buckets = groupValue.GetObject().GetNamedArray(L"buckets", nullptr);
        if (!buckets) continue;
        for (const auto& bucketValue : buckets) {
            if (bucketValue.ValueType() != JsonValueType::Object) continue;
            const auto bucket = bucketValue.GetObject();
            const Text id = getJsonStr(bucket, L"id");
            if (id != L"gemini-5h" && id != L"gemini-weekly") continue;
            const double fraction = getJsonNum(bucket, L"remaining_fraction");
            if (!std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0) continue;
            const int percent = static_cast<int>(std::lround(fraction * 100));
            const time_t resetAt = parseJsonTime(bucket, L"reset_time");
            if (id == L"gemini-5h") {
                out.p1 = percent;
                out.r1At = resetAt;
            } else {
                out.p2 = percent;
                out.r2At = resetAt;
            }
        }
    }
    out.ready = out.hasLimits = out.p1 >= 0 || out.p2 >= 0;
    if (out.ready) {
        out.updatedAt = std::time(nullptr);
        out.sourceLabel = L"Antigravity CLI · Gemini Models";
    }
    return out;
}

AiPlanLive fetchAntigravityUsage(const std::atomic<bool>& stop) {
    const Text local = envDir(L"LOCALAPPDATA");
    const fs::path install = fs::path(local) / L"agy" / L"bin";
    fs::path exe = install / L"agy.exe";
    std::error_code ec;
    if (local.empty() || !fs::is_regular_file(exe, ec)) {
        // Search the configured PATH explicitly, without searching the current directory.
        const DWORD pathSize = GetEnvironmentVariableW(L"PATH", nullptr, 0);
        Text path(pathSize, L'\0');
        const DWORD copied = GetEnvironmentVariableW(L"PATH", path.data(), pathSize);
        if (!copied || copied >= pathSize) return {};
        path.resize(copied);
        std::wstringstream paths(path);
        Text dir;
        while (std::getline(paths, dir, L';')) {
            fs::path candidate = fs::path(dir) / L"agy.exe";
            if (candidate.is_absolute() && fs::is_regular_file(candidate, ec)) { exe = std::move(candidate); break; }
        }
    }
    if (stop.load() || !exe.is_absolute() || !fs::is_regular_file(exe, ec)) return {};

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE reader{}, writer{};
    if (!CreatePipe(&reader, &writer, &sa, 0)) return {};
    winrt::handle readPipe{reader}, writePipe{writer};
    if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) return {};
    const HANDLE nullHandle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullHandle == INVALID_HANDLE_VALUE) return {};
    winrt::handle nullDevice{nullHandle};

    SIZE_T attrBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrBytes);
    std::vector<BYTE> storage(attrBytes);
    const auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrBytes)) return {};
    const std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)>
        attributes(attrs, DeleteProcThreadAttributeList);
    HANDLE inherited[]{writer, nullHandle};
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
        return {};

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = si.StartupInfo.hStdError = nullHandle;
    si.StartupInfo.hStdOutput = writer;
    si.lpAttributeList = attrs;
    Text cmd = L"\"" + exe.wstring() + L"\" --print /usage --output-format json --disable-slash-commands=false --print-timeout 10s --log-file NUL";

    // Keep the CLI's normal login environment while disabling auto-update for this poll.
    const auto env = GetEnvironmentStringsW();
    if (!env) return {};
    constexpr std::wstring_view updateKey = L"AGY_CLI_DISABLE_AUTO_UPDATE=";
    Text childEnv;
    bool updateSet = false;
    for (auto entry = env; *entry; entry += wcslen(entry) + 1) {
        const int order = _wcsnicmp(entry, updateKey.data(), updateKey.size());
        if (!updateSet && order >= 0) {
            childEnv.append(updateKey).append(L"true").push_back(L'\0');
            updateSet = true;
        }
        if (order != 0) childEnv.append(entry).push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
    if (!updateSet) childEnv.append(updateKey).append(L"true").push_back(L'\0');

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        childEnv.data(), exe.parent_path().c_str(), &si.StartupInfo, &pi)) return {};
    winrt::handle process{pi.hProcess}, thread{pi.hThread};
    writePipe.close();

    std::string response;
    const ULONGLONG deadline = GetTickCount64() + 12000;
    while (!stop.load() && GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) || !available) {
            if (WaitForSingleObject(process.get(), 25) == WAIT_OBJECT_0) {
                // The process can exit between PeekNamedPipe and the wait.
                if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) continue;
                DWORD code = 1;
                if (GetExitCodeProcess(process.get(), &code) && code == 0)
                    return parseAntigravityUsage(utf16(response));
                return {};
            }
            continue;
        }
        char buffer[4096];
        DWORD count = 0;
        if (!ReadFile(reader, buffer, std::min<DWORD>(available, sizeof(buffer)), &count, nullptr) ||
            response.size() + count > 256 * 1024) break;
        response.append(buffer, count);
    }
    TerminateProcess(process.get(), 1);
    return {};
}

// ---------- 2B. Local Codex / ChatGPT JSONL fallback reader ----------
void readLocalCodex(AiPlanLive& out) {
    const Text home = userProfileDir();
    if (home.empty()) return;
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const wchar_t* sub : {L"sessions", L"archived_sessions"}) {
        const fs::path root = fs::path(home) / L".codex" / sub;
        if (!fs::exists(root, ec)) continue;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            if (it->path().extension() == L".jsonl")
                files.push_back({it->last_write_time(ec), it->path()});
        }
    }
    if (files.empty()) return;
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    const size_t limit = std::min<size_t>(files.size(), 12);
    for (size_t i = 0; i < limit; ++i) {
        std::ifstream f(files[i].second, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        const auto sz = f.tellg();
        if (sz <= 0) continue;
        const std::streamoff readSz = std::min<std::streamoff>(sz, 512 * 1024);
        f.seekg(sz - readSz);
        std::string chunk(static_cast<size_t>(readSz), '\0');
        f.read(chunk.data(), readSz);

        // Scan backwards through lines looking for the newest valid rate_limits entry
        size_t end = chunk.size();
        while (end > 0) {
            const size_t start = chunk.rfind('\n', end - 1);
            const size_t linePos = (start == std::string::npos) ? 0 : start + 1;
            const std::string_view sv(chunk.data() + linePos, end - linePos);
            end = (start == std::string::npos) ? 0 : start;
            if (sv.find("\"rate_limits\"") == std::string_view::npos || sv.find("\"used_percent\"") == std::string_view::npos)
                continue;
            JsonObject root{nullptr}, payload{nullptr}, rl{nullptr};
            if (!JsonObject::TryParse(utf16(std::string(sv)), root)) continue;
            if (!getJsonObject(root, L"payload", payload) || !getJsonObject(payload, L"rate_limits", rl)) continue;
            const Text limId = getJsonStr(rl, L"limit_id");
            if (!limId.empty() && limId != L"codex") continue;
            int p1 = -1, p2 = -1, w1 = 0, w2 = 0;
            time_t r1At = 0, r2At = 0;
            const bool got1 = parseCodexLimitWindow(rl, L"primary", p1, r1At, w1);
            const bool got2 = parseCodexLimitWindow(rl, L"secondary", p2, r2At, w2);
            if (!got1 && !got2) continue;
            out.p1 = p1; out.r1At = r1At; out.r1WinMins = w1;
            out.p2 = p2; out.r2At = r2At; out.r2WinMins = w2;
            out.planType = getJsonStr(rl, L"plan_type");
            const Text tsW = getJsonStr(root, L"timestamp");
            std::string tsA(tsW.begin(), tsW.end());
            const time_t evT = parseIsoUtc(tsA);
            out.updatedAt = evT > 0 ? evT : fileTimeToEpoch(files[i].first);
            out.ready = true;
            out.hasLimits = true;
            out.sourceLabel = L"~/.codex/sessions";
            return;
        }
    }
}

// ---------- 3. Local Claude Code Usage reader (exact per-message UTC timestamp + dedup, NO fake %) ----------
void readLocalClaude(AiPlanLive& out) {
    const Text home = userProfileDir();
    if (home.empty()) return;
    const fs::path root = fs::path(home) / L".claude" / L"projects";
    std::error_code ec;
    if (!fs::exists(root, ec)) return;

    const time_t nowT = std::time(nullptr);
    const time_t cut5h = nowT - 5 * 3600;
    const time_t cut7d = nowT - 7 * 86400;
    const auto nowFt = fs::file_time_type::clock::now();

    std::vector<std::pair<fs::file_time_type, fs::path>> recentFiles;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".jsonl") continue;
        const auto mtime = it->last_write_time(ec);
        const auto ageHours = std::chrono::duration_cast<std::chrono::hours>(nowFt - mtime).count();
        if (ageHours > 192) continue; // 8-day file mtime prefilter
        recentFiles.push_back({mtime, it->path()});
    }
    std::sort(recentFiles.begin(), recentFiles.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (recentFiles.size() > 120) recentFiles.resize(120);

    struct TurnUsage { time_t ts = 0; long long tok = 0; std::string model; };
    std::unordered_map<std::string, TurnUsage> byTurn;
    long long anonIdx = 0;

    for (const auto& [mtime, path] : recentFiles) {
        std::ifstream f(path, std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("\"input_tokens\"") == std::string::npos || line.find("\"assistant\"") == std::string::npos)
                continue;
            // Fast pre-check timestamp string before full JSON parse
            const size_t tsPos = line.find("\"timestamp\"");
            time_t ts = 0;
            if (tsPos != std::string::npos) {
                const size_t q1 = line.find('"', tsPos + 11);
                const size_t q2 = q1 != std::string::npos ? line.find('"', q1 + 1) : std::string::npos;
                if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 18)
                    ts = parseIsoUtc(std::string_view(line.data() + q1 + 1, q2 - q1 - 1));
            }
            if (ts <= 0 || ts < cut7d) continue;

            JsonObject rootObj{nullptr}, msgObj{nullptr}, usageObj{nullptr};
            if (!JsonObject::TryParse(utf16(line), rootObj)) continue;
            if (!getJsonObject(rootObj, L"message", msgObj) || !getJsonObject(msgObj, L"usage", usageObj)) continue;

            const long long tok =
                static_cast<long long>(std::max(0.0, getJsonNum(usageObj, L"input_tokens", 0.0))) +
                static_cast<long long>(std::max(0.0, getJsonNum(usageObj, L"output_tokens", 0.0))) +
                static_cast<long long>(std::max(0.0, getJsonNum(usageObj, L"cache_read_input_tokens", 0.0))) +
                static_cast<long long>(std::max(0.0, getJsonNum(usageObj, L"cache_creation_input_tokens", 0.0)));
            if (tok <= 0) continue;

            Text msgIdW = getJsonStr(msgObj, L"id");
            if (msgIdW.empty()) msgIdW = getJsonStr(rootObj, L"requestId");
            std::string key = msgIdW.empty() ? ("anon_" + std::to_string(++anonIdx))
                                             : std::string(msgIdW.begin(), msgIdW.end());
            const Text modelW = getJsonStr(msgObj, L"model");
            std::string modelA(modelW.begin(), modelW.end());

            auto& slot = byTurn[key];
            if (tok >= slot.tok) {
                slot.tok = tok;
                slot.ts = std::max(slot.ts, ts);
                if (!modelA.empty()) slot.model = std::move(modelA);
            }
        }
    }

    long long tokens5h = 0, tokens7d = 0;
    std::unordered_map<std::string, long long> byModel;
    for (const auto& [k, u] : byTurn) {
        tokens7d += u.tok;
        if (u.ts >= cut5h) tokens5h += u.tok;
        if (!u.model.empty()) byModel[u.model] += u.tok;
    }

    out.ready = true;
    out.hasLimits = false;
    out.hasTokens = true;
    out.p1 = -1;
    out.p2 = -1;
    out.sessionTokens = tokens5h;
    out.weeklyTokens = tokens7d;
    out.r1 = L"last 5h";
    out.r2 = L"last 7d";
    out.sourceLabel = L"~/.claude/projects";
    out.updatedAt = nowT;
    std::string topM;
    long long topV = 0;
    for (const auto& [k, v] : byModel) if (v > topV) { topV = v; topM = k; }
    if (!topM.empty()) {
        out.modelName = shortModelName(utf16(topM));
        out.modelTokens = formatCompact(topV);
    }
}

// ---------- 4. Local Gemini CLI session reader (last 24h Pro vs Flash tokens, NO fake %) ----------
void readLocalGemini(AiPlanLive& out) {
    const Text home = userProfileDir();
    if (home.empty()) return;
    const fs::path root = fs::path(home) / L".gemini" / L"tmp";
    std::error_code ec;
    if (!fs::exists(root, ec)) return;

    const time_t nowT = std::time(nullptr);
    const time_t cut24h = nowT - 86400;
    const auto nowFt = fs::file_time_type::clock::now();
    long long proTok = 0, flashTok = 0;

    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != L".json") continue;
        if (it->path().parent_path().filename() != L"chats") continue;
        const auto ageHours = std::chrono::duration_cast<std::chrono::hours>(nowFt - it->last_write_time(ec)).count();
        if (ageHours > 36) continue;
        std::ifstream f(it->path(), std::ios::binary);
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        JsonObject doc{nullptr};
        if (!JsonObject::TryParse(utf16(content), doc) || !doc.HasKey(L"messages")) continue;
        const auto msgsVal = doc.Lookup(L"messages");
        if (!msgsVal || msgsVal.ValueType() != JsonValueType::Array) continue;
        const auto msgs = msgsVal.GetArray();
        for (uint32_t i = 0; i < msgs.Size(); ++i) {
            const auto itemVal = msgs.GetAt(i);
            if (!itemVal || itemVal.ValueType() != JsonValueType::Object) continue;
            const auto m = itemVal.GetObject();
            JsonObject tokObj{nullptr};
            if (!getJsonObject(m, L"tokens", tokObj)) continue;
            const Text tsW = getJsonStr(m, L"timestamp");
            std::string tsA(tsW.begin(), tsW.end());
            const time_t ts = parseIsoUtc(tsA);
            if (ts <= 0 || ts < cut24h) continue;
            double total = getJsonNum(tokObj, L"total", -1.0);
            if (total < 0.0) {
                total = std::max(0.0, getJsonNum(tokObj, L"input", 0.0)) +
                        std::max(0.0, getJsonNum(tokObj, L"output", 0.0)) +
                        std::max(0.0, getJsonNum(tokObj, L"thoughts", 0.0));
            }
            const long long t = static_cast<long long>(std::llround(std::max(0.0, total)));
            if (t <= 0) continue;
            Text model = getJsonStr(m, L"model");
            for (auto& ch : model) ch = static_cast<wchar_t>(towlower(ch));
            if (model.find(L"flash") != Text::npos) flashTok += t;
            else proTok += t;
        }
    }

    out.ready = true;
    out.hasLimits = false;
    out.hasTokens = (proTok > 0 || flashTok > 0);
    out.p1 = -1;
    out.p2 = -1;
    out.sessionTokens = proTok;
    out.secondaryTokens = flashTok;
    out.r1 = L"24h";
    out.r2 = L"24h";
    out.sourceLabel = L"~/.gemini/tmp (CLI)";
    out.updatedAt = nowT;
}

// ---------- 5. Local OpenCode SQLite reader via Windows built-in winsqlite3.dll ----------
void readLocalOpenCode(AiPlanLive& out) {
    static std::mutex dbCacheMu;
    static bool hasCached = false;
    static fs::file_time_type cachedDbMtime{}, cachedWalMtime{};
    static AiPlanLive cachedPlan{};

    const Text home = userProfileDir();
    if (home.empty()) return;
    const fs::path dbPath = fs::path(home) / L".local" / L"share" / L"opencode" / L"opencode.db";
    std::error_code ec;
    if (!fs::exists(dbPath, ec)) return;
    const auto dbMtime = fs::last_write_time(dbPath, ec);
    const fs::path walPath = dbPath.wstring() + L"-wal";
    const auto walMtime = fs::exists(walPath, ec) ? fs::last_write_time(walPath, ec) : fs::file_time_type{};
    {
        std::lock_guard lock(dbCacheMu);
        if (hasCached && cachedDbMtime == dbMtime && cachedWalMtime == walMtime) {
            out = cachedPlan;
            return;
        }
    }

    static HMODULE hSq = LoadLibraryW(L"winsqlite3.dll");
    if (!hSq) return;
    using OpenFn = int(__cdecl*)(const char*, void**, int, const char*);
    using CloseFn = int(__cdecl*)(void*);
    using PrepFn = int(__cdecl*)(void*, const char*, int, void**, const char**);
    using StepFn = int(__cdecl*)(void*);
    using ColInt64Fn = long long(__cdecl*)(void*, int);
    using ColTextFn = const unsigned char*(__cdecl*)(void*, int);
    using FinFn = int(__cdecl*)(void*);
    auto sqOpen  = reinterpret_cast<OpenFn>(GetProcAddress(hSq, "sqlite3_open_v2"));
    auto sqClose = reinterpret_cast<CloseFn>(GetProcAddress(hSq, "sqlite3_close"));
    auto sqPrep  = reinterpret_cast<PrepFn>(GetProcAddress(hSq, "sqlite3_prepare_v2"));
    auto sqStep  = reinterpret_cast<StepFn>(GetProcAddress(hSq, "sqlite3_step"));
    auto sqInt64 = reinterpret_cast<ColInt64Fn>(GetProcAddress(hSq, "sqlite3_column_int64"));
    auto sqText  = reinterpret_cast<ColTextFn>(GetProcAddress(hSq, "sqlite3_column_text"));
    auto sqFin   = reinterpret_cast<FinFn>(GetProcAddress(hSq, "sqlite3_finalize"));
    if (!sqOpen || !sqClose || !sqPrep || !sqStep || !sqInt64 || !sqText || !sqFin) return;

    const std::string pathUtf8 = [&]() {
        const Text w = dbPath.wstring();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(std::max(0, n - 1), '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
        return s;
    }();
    void* db{};
    if (sqOpen(pathUtf8.c_str(), &db, 1 /*SQLITE_OPEN_READONLY*/, nullptr) != 0 || !db) return;

    const time_t nowT = std::time(nullptr);
    const long long nowMs = static_cast<long long>(nowT) * 1000LL;
    const long long cut5h = nowMs - 5LL * 3600 * 1000;
    const long long cut7d = nowMs - 7LL * 86400 * 1000;
    const auto sumQuery = [&](long long sinceMs) -> long long {
        const std::string sql =
            "SELECT COALESCE(SUM("
            "COALESCE(json_extract(data,'$.tokens.input'),0)+"
            "COALESCE(json_extract(data,'$.tokens.output'),0)+"
            "COALESCE(json_extract(data,'$.tokens.reasoning'),0)+"
            "COALESCE(json_extract(data,'$.tokens.cache.read'),0)+"
            "COALESCE(json_extract(data,'$.tokens.cache.write'),0)),0) "
            "FROM session_message WHERE time_created >= " + std::to_string(sinceMs) +
            " AND json_extract(data,'$.tokens') IS NOT NULL;";
        void* stmt{};
        long long total = 0;
        if (sqPrep(db, sql.c_str(), -1, &stmt, nullptr) == 0 && stmt) {
            if (sqStep(stmt) == 100) total = sqInt64(stmt, 0);
            sqFin(stmt);
        }
        return total;
    };
    out.sessionTokens = sumQuery(cut5h);
    out.weeklyTokens = sumQuery(cut7d);
    const std::string topSql =
        "SELECT COALESCE(json_extract(data,'$.model.id'),''), "
        "SUM(COALESCE(json_extract(data,'$.tokens.input'),0)+COALESCE(json_extract(data,'$.tokens.output'),0)+"
        "COALESCE(json_extract(data,'$.tokens.reasoning'),0)+COALESCE(json_extract(data,'$.tokens.cache.read'),0)+"
        "COALESCE(json_extract(data,'$.tokens.cache.write'),0)) AS t "
        "FROM session_message WHERE time_created >= " + std::to_string(cut7d) +
        " AND json_extract(data,'$.tokens') IS NOT NULL GROUP BY 1 ORDER BY t DESC LIMIT 1;";
    void* stmt{};
    if (sqPrep(db, topSql.c_str(), -1, &stmt, nullptr) == 0 && stmt) {
        if (sqStep(stmt) == 100) {
            const long long topTok = sqInt64(stmt, 1);
            if (topTok > 0) {
                if (const auto* txt = sqText(stmt, 0)) {
                    Text m = utf16(reinterpret_cast<const char*>(txt));
                    if (m.size() > 13) m.resize(13);
                    out.modelName = m;
                }
                out.modelTokens = formatCompact(topTok);
            }
        }
        sqFin(stmt);
    }
    sqClose(db);
    out.ready = true;
    out.hasLimits = false;
    out.hasTokens = true;
    out.r1 = L"last 5h";
    out.r2 = L"last 7d";
    out.sourceLabel = L"opencode.db";
    out.updatedAt = nowT;
    {
        std::lock_guard lock(dbCacheMu);
        hasCached = true;
        cachedDbMtime = dbMtime;
        cachedWalMtime = walMtime;
        cachedPlan = out;
    }
}

// ---------- 5B. Local user-supplied JSON feed (%LOCALAPPDATA%\Vetra\ai_usage.json) ----------
fs::path aiFeedPath() {
    const Text local = envDir(L"LOCALAPPDATA");
    return local.empty() ? fs::path{} : (fs::path(local) / L"Vetra" / L"ai_usage.json");
}

int parseJsonPct(const JsonObject& obj, const wchar_t* pKey, const wchar_t* fracKey, const wchar_t* remKey) {
    const double direct = getJsonNum(obj, pKey, -1.0);
    if (direct >= 0.0) return std::clamp(static_cast<int>(std::lround(direct)), 0, 100);
    double frac = getJsonNum(obj, fracKey, -1.0);
    if (frac < 0.0) frac = getJsonNum(obj, remKey, -1.0);
    if (frac >= 0.0) {
        const double pct = frac <= 1.0 ? frac * 100.0 : frac;
        return std::clamp(static_cast<int>(std::lround(pct)), 0, 100);
    }
    return -1;
}

void readAiFeedFile(std::array<AiPlanLive, AiPlanCount>& ai) {
    static std::mutex feedMutex;
    static bool hasCached = false;
    static bool cachedExists = false;
    static fs::file_time_type cachedMtime{};
    static uintmax_t cachedSize = 0;
    static std::array<AiPlanLive, AiPlanCount> cachedFeed{};

    const fs::path path = aiFeedPath();
    if (path.empty()) return;
    std::error_code ec;
    const bool exists = fs::exists(path, ec) && fs::is_regular_file(path, ec);
    if (!exists) {
        std::lock_guard lock(feedMutex);
        hasCached = true;
        cachedExists = false;
        cachedFeed = {};
        return;
    }
    const auto fsz = fs::file_size(path, ec);
    const auto mtime = fs::last_write_time(path, ec);
    if (ec || fsz == 0 || fsz > 512 * 1024) return;
    {
        std::lock_guard lock(feedMutex);
        if (hasCached && cachedExists && cachedMtime == mtime && cachedSize == fsz) {
            for (int i = 0; i < AiPlanCount; ++i)
                if (cachedFeed[i].ready) ai[i] = cachedFeed[i];
            return;
        }
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string raw(static_cast<size_t>(fsz), '\0');
    f.read(raw.data(), static_cast<std::streamsize>(fsz));
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    JsonObject root{nullptr};
    if (!JsonObject::TryParse(utf16(raw), root)) return;

    const time_t fileT = fileTimeToEpoch(mtime);
    std::array<AiPlanLive, AiPlanCount> parsed{};
    struct PlanKeys { int idx; const wchar_t* k1; const wchar_t* k2; };
    static constexpr PlanKeys Map[] = {
        {0, L"claude",   L"Claude"},
        {1, L"chatgpt",  L"codex"},
        {2, L"gemini",   L"antigravity"},
        {3, L"grok",     L"Grok"},
        {4, L"cursor",   L"Cursor"},
        {5, L"glm",      L"GLM"},
        {6, L"deepseek", L"DeepSeek"},
        {7, L"opencode", L"OpenCode"},
    };
    for (const auto& entry : Map) {
        JsonObject obj{nullptr};
        if (!getJsonObject(root, entry.k1, obj) && !getJsonObject(root, entry.k2, obj)) continue;
        AiPlanLive plan;
        plan.p1 = parseJsonPct(obj, L"p1", L"remainingFraction1", L"remaining1");
        plan.p2 = parseJsonPct(obj, L"p2", L"remainingFraction2", L"remaining2");
        plan.p3 = parseJsonPct(obj, L"p3", L"remainingFraction3", L"remaining3");
        plan.r1At = parseJsonTime(obj, L"r1At");
        plan.r2At = parseJsonTime(obj, L"r2At");
        plan.r3At = parseJsonTime(obj, L"r3At");
        plan.r1 = getJsonStr(obj, L"r1");
        plan.r2 = getJsonStr(obj, L"r2");
        plan.r3 = getJsonStr(obj, L"r3");
        plan.l1 = getJsonStr(obj, L"label1"); if (plan.l1.empty()) plan.l1 = getJsonStr(obj, L"l1");
        plan.l2 = getJsonStr(obj, L"label2"); if (plan.l2.empty()) plan.l2 = getJsonStr(obj, L"l2");
        plan.l3 = getJsonStr(obj, L"label3"); if (plan.l3.empty()) plan.l3 = getJsonStr(obj, L"l3");
        plan.modelName = getJsonStr(obj, L"modelName");
        plan.modelTokens = getJsonStr(obj, L"modelTokens");
        const double st = getJsonNum(obj, L"sessionTokens", -1.0);
        const double wt = getJsonNum(obj, L"weeklyTokens", -1.0);
        const double sc = getJsonNum(obj, L"secondaryTokens", -1.0);
        if (st >= 0.0) plan.sessionTokens = static_cast<long long>(std::llround(st));
        if (wt >= 0.0) plan.weeklyTokens = static_cast<long long>(std::llround(wt));
        if (sc >= 0.0) plan.secondaryTokens = static_cast<long long>(std::llround(sc));
        const double bal = getJsonNum(obj, L"balance", -1.0);
        if (bal >= 0.0) {
            plan.balance = bal;
            plan.todaySpend = std::max(0.0, getJsonNum(obj, L"todaySpend", 0.0));
            plan.weekSpend = std::max(0.0, getJsonNum(obj, L"weekSpend", 0.0));
        }
        plan.hasLimits = (plan.p1 >= 0 || plan.p2 >= 0 || plan.p3 >= 0);
        plan.hasTokens = (plan.sessionTokens >= 0 || plan.weeklyTokens >= 0);
        if (!plan.hasLimits && !plan.hasTokens && plan.balance < 0.0) continue;
        const time_t uAt = parseJsonTime(obj, L"updatedAt");
        plan.updatedAt = uAt > 0 ? uAt : fileT;
        plan.ready = true;
        plan.fromFeedFile = true;
        plan.sourceLabel = L"ai_usage.json";
        parsed[entry.idx] = plan;
        ai[entry.idx] = std::move(plan);
    }
    {
        std::lock_guard lock(feedMutex);
        hasCached = true;
        cachedExists = true;
        cachedMtime = mtime;
        cachedSize = fsz;
        cachedFeed = std::move(parsed);
    }
}

void openAiFeedInExplorer(HWND owner) {
    const fs::path path = aiFeedPath();
    if (path.empty()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (!fs::exists(path, ec)) {
        std::ofstream f(path, std::ios::binary);
        if (f) f << "{\n}\n";
    }
    const Text args = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(owner, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

// ---------- 5. Web Feeds: GitHub Contributions, DeepSeek, Stripe, DataFast ----------
GitHubLive fetchGitHub(const Text& user) {
    GitHubLive out;
    if (user.empty()) return out;
    const Text html = httpGet(L"github.com", L"/users/" + user + L"/contributions", {}, 640 * 1024);
    if (html.empty()) { out.error = true; return out; }

    struct DayCell { std::string date; int level = 0; int count = 0; };
    std::vector<DayCell> cells;
    // Parse narrow UTF-8 copy for fast regex
    const int n = WideCharToMultiByte(CP_UTF8, 0, html.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(std::max(0, n - 1), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, html.c_str(), -1, narrow.data(), n, nullptr, nullptr);

    static const std::regex rxCell("data-date=\"([0-9]{4}-[0-9]{2}-[0-9]{2})\"[^>]*id=\"([^\"]+)\"[^>]*data-level=\"([0-4])\"");
    static const std::regex rxCellAlt("id=\"([^\"]+)\"[^>]*data-date=\"([0-9]{4}-[0-9]{2}-[0-9]{2})\"[^>]*data-level=\"([0-4])\"");
    static const std::regex rxTip("for=\"([^\"]+)\"[^>]*>\\s*([0-9]+|No)\\s+contribution");

    std::unordered_map<std::string, int> countsById;
    for (auto it = std::sregex_iterator(narrow.begin(), narrow.end(), rxTip); it != std::sregex_iterator(); ++it) {
        const std::string val = (*it)[2].str();
        countsById[(*it)[1].str()] = (val == "No") ? 0 : std::stoi(val);
    }
    for (auto it = std::sregex_iterator(narrow.begin(), narrow.end(), rxCell); it != std::sregex_iterator(); ++it) {
        const std::string id = (*it)[2].str();
        const int lvl = std::stoi((*it)[3].str());
        int cnt = countsById.count(id) ? countsById[id] : (lvl > 0 ? lvl * 2 : 0);
        cells.push_back({(*it)[1].str(), lvl, cnt});
    }
    if (cells.empty()) {
        for (auto it = std::sregex_iterator(narrow.begin(), narrow.end(), rxCellAlt); it != std::sregex_iterator(); ++it) {
            const std::string id = (*it)[1].str();
            const int lvl = std::stoi((*it)[3].str());
            int cnt = countsById.count(id) ? countsById[id] : (lvl > 0 ? lvl * 2 : 0);
            cells.push_back({(*it)[2].str(), lvl, cnt});
        }
    }
    if (cells.empty()) { out.error = true; return out; }
    std::sort(cells.begin(), cells.end(), [](const auto& a, const auto& b) { return a.date < b.date; });

    out.today = cells.back().count;
    const size_t total = cells.size();
    for (size_t i = total > 7 ? total - 7 : 0; i < total; ++i) out.week += cells[i].count;
    int streak = 0;
    for (int i = static_cast<int>(total) - 1; i >= 0; --i) {
        if (cells[i].level > 0 || cells[i].count > 0) ++streak;
        else if (i == static_cast<int>(total) - 1) continue; // today not yet committed
        else break;
    }
    out.streak = streak;
    const size_t start = total > 42 ? total - 42 : 0;
    for (size_t i = 0; i < 42 && start + i < total; ++i) {
        out.grid[i] = static_cast<uint8_t>(std::clamp(cells[start + i].level, 0, 4));
    }
    out.ready = true;
    return out;
}

AiPlanLive fetchDeepSeek(const Text& key) {
    AiPlanLive out;
    if (key.empty()) return out;
    const Text body = httpGet(L"api.deepseek.com", L"/user/balance", L"Authorization: Bearer " + key);
    if (body.empty()) return out;
    try {
        const auto root = JsonObject::Parse(body);
        const auto infos = root.GetNamedArray(L"balance_infos");
        if (infos.Size() > 0) {
            const auto first = infos.GetObjectAt(0);
            out.balance = std::stod(Text(first.GetNamedString(L"total_balance", L"0").c_str()));
            out.ready = true;
        }
    } catch (...) {}
    return out;
}

AnalyticsLive fetchStripe(const Text& key) {
    AnalyticsLive out;
    if (key.empty()) return out;
    const long long since = static_cast<long long>(std::time(nullptr)) - 14LL * 86400;
    const Text body = httpGet(L"api.stripe.com", L"/v1/balance_transactions?limit=100&created[gte]=" + num(since),
        L"Authorization: Bearer " + key);
    if (body.empty()) { out.error = true; return out; }
    try {
        const auto root = JsonObject::Parse(body);
        const auto arr = root.GetNamedArray(L"data");
        std::array<double, 14> daily{};
        const long long nowT = static_cast<long long>(std::time(nullptr));
        for (uint32_t i = 0; i < arr.Size(); ++i) {
            const auto item = arr.GetObjectAt(i);
            const Text type(item.GetNamedString(L"type", L"").c_str());
            if (type != L"charge" && type != L"payment") continue;
            const double net = item.GetNamedNumber(L"net", 0.0) / 100.0;
            const long long created = static_cast<long long>(item.GetNamedNumber(L"created", 0.0));
            const int daysAgo = static_cast<int>((nowT - created) / 86400);
            if (daysAgo >= 0 && daysAgo < 14) daily[13 - daysAgo] += net;
        }
        double prev7 = 0, cur7 = 0;
        for (int i = 0; i < 7; ++i) prev7 += daily[i];
        for (int i = 7; i < 14; ++i) cur7 += daily[i];
        out.value = cur7;
        out.changePct = prev7 > 0.01 ? ((cur7 - prev7) / prev7) * 100.0 : (cur7 > 0 ? 100.0 : 0.0);
        out.trend.assign(daily.begin(), daily.end());
        out.ready = true;
    } catch (...) { out.error = true; }
    return out;
}

AnalyticsLive fetchDataFast(const Text& key) {
    AnalyticsLive out;
    if (key.empty()) return out;
    const Text body = httpGet(L"datafa.st", L"/api/v1/analytics/timeseries?fields=visitors&interval=day",
        L"Authorization: Bearer " + key);
    if (body.empty()) { out.error = true; return out; }
    try {
        const auto root = JsonObject::Parse(body);
        const auto arr = root.GetNamedArray(L"data");
        std::vector<double> vals;
        for (uint32_t i = 0; i < arr.Size(); ++i)
            vals.push_back(arr.GetObjectAt(i).GetNamedNumber(L"visitors", 0.0));
        if (vals.size() > 14) vals.erase(vals.begin(), vals.end() - 14);
        double cur7 = 0, prev7 = 0;
        const size_t n = vals.size();
        for (size_t i = 0; i < n; ++i) {
            if (i + 7 >= n) cur7 += vals[i];
            else prev7 += vals[i];
        }
        out.value = cur7;
        out.changePct = prev7 > 0.01 ? ((cur7 - prev7) / prev7) * 100.0 : (cur7 > 0 ? 100.0 : 0.0);
        out.trend = std::move(vals);
        out.ready = true;
    } catch (...) { out.error = true; }
    return out;
}

// ---------- 6. Custom Pixel-Accurate Brand Logos ----------
void drawBrandLogo(Draw& d, int plan, Box b) {
    const float w = width(b), h = height(b);
    const float cx = b.left + w / 2.f, cy = b.top + h / 2.f;
    const float r = std::max(4.f, w * .24f);
    switch (plan) {
    case 0: { // Claude coral starburst
        d.fill(b, r, tint(0xD97757));
        const float outer = w * .34f, inner = w * .08f;
        const float stroke = std::max(1.2f, w * .068f);
        for (int i = 0; i < 10; ++i) {
            const float a = i * 0.62831853f - 0.15f;
            d.line(cx + std::cos(a) * inner, cy + std::sin(a) * inner,
                   cx + std::cos(a) * outer, cy + std::sin(a) * outer, tint(0x161210), stroke);
        }
        d.circle(cx, cy, w * .09f, tint(0x161210));
        break;
    }
    case 1: { // ChatGPT / OpenAI interlocking 6-loop knot
        d.fill(b, r, tint(0xF6F6F7));
        const float ring = w * .145f, loopR = w * .15f;
        const float stroke = std::max(1.0f, w * .048f);
        for (int i = 0; i < 6; ++i) {
            const float a = i * 1.04719755f - 0.52359878f;
            const float px = cx + std::cos(a) * ring;
            const float py = cy + std::sin(a) * ring;
            d.circle(px, py, loopR, tint(0x141619), false, stroke);
        }
        d.circle(cx, cy, w * .095f, tint(0xF6F6F7));
        d.circle(cx, cy, w * .095f, tint(0x141619), false, stroke * .75f);
        break;
    }
    case 2: { // Gemini concave 4-point 4-color sparkle star (cached unit geometries)
        d.fill(b, r, tint(0xF7F8FA));
        const float s = w * .37f;
        struct GeminiSparkleCache {
            ID2D1Factory* owner = nullptr;
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> petals[4];
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> bridge;
            void ensure(ID2D1Factory* factory) {
                if (!factory || (owner == factory && petals[0])) return;
                owner = factory;
                constexpr float spec[4][6] = {
                    { 0.f, -1.f, -0.8f, -0.4f,  0.8f, -0.4f},
                    { 1.f,  0.f,  0.4f, -0.8f,  0.4f,  0.8f},
                    { 0.f,  1.f,  0.8f,  0.4f, -0.8f,  0.4f},
                    {-1.f,  0.f, -0.4f,  0.8f, -0.4f, -0.8f},
                };
                for (int i = 0; i < 4; ++i) {
                    petals[i].Reset();
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(factory->CreatePathGeometry(&petals[i])) && SUCCEEDED(petals[i]->Open(&sink))) {
                        sink->BeginFigure({0.f, 0.f}, D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddQuadraticBezier({{spec[i][2] * .22f, spec[i][3] * .22f}, {spec[i][0], spec[i][1]}});
                        sink->AddQuadraticBezier({{spec[i][4] * .22f, spec[i][5] * .22f}, {0.f, 0.f}});
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        if (FAILED(sink->Close())) petals[i].Reset();
                    }
                }
                bridge.Reset();
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(factory->CreatePathGeometry(&bridge)) && SUCCEEDED(bridge->Open(&sink))) {
                    constexpr float c = .30f;
                    sink->BeginFigure({0.f, -.72f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddQuadraticBezier({{ c * .35f, -c * .35f}, { .72f, 0.f}});
                    sink->AddQuadraticBezier({{ c * .35f,  c * .35f}, { 0.f,  .72f}});
                    sink->AddQuadraticBezier({{-c * .35f,  c * .35f}, {-.72f, 0.f}});
                    sink->AddQuadraticBezier({{-c * .35f, -c * .35f}, { 0.f, -.72f}});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    if (FAILED(sink->Close())) bridge.Reset();
                }
            }
        };
        static GeminiSparkleCache cache;
        cache.ensure(Draw::factory);
        constexpr unsigned colors[4] = {0xEA4335, 0x4285F4, 0x34A853, 0xFBBC05};
        D2D1::Matrix3x2F prev{};
        d.target->GetTransform(&prev);
        d.target->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(cx, cy) * prev);
        for (int i = 0; i < 4; ++i) {
            if (cache.petals[i]) {
                d.brush->SetColor(D2D1::ColorF(colors[i], 1.f));
                d.target->FillGeometry(cache.petals[i].Get(), d.brush);
            }
        }
        if (cache.bridge) {
            d.brush->SetColor(D2D1::ColorF(0x4285F4, .22f));
            d.target->FillGeometry(cache.bridge.Get(), d.brush);
        }
        d.target->SetTransform(prev);
        break;
    }
    case 3: { // Grok ghost-visor mark
        d.fill(b, r, tint(0x181A1E));
        d.outline(b, r, tint(0xFFFFFF, .14f), .8f);
        const Box head = box(b.left + w * .16f, b.top + h * .20f, w * .68f, h * .60f);
        d.gradient(head, h * .28f, tint(0xFFFFFF), tint(0xCBD2DC));
        d.fill(box(cx - w * .13f, cy - h * .09f, w * .08f, h * .18f), 2.f, tint(0x15171A));
        d.fill(box(cx + w * .05f, cy - h * .09f, w * .08f, h * .18f), 2.f, tint(0x15171A));
        break;
    }
    case 4: { // Cursor black 3D isometric cube
        d.fill(b, r, tint(0x0B0C0E));
        d.outline(b, r, tint(0xFFFFFF, .16f), .8f);
        const float rr = w * .31f;
        const float dx = rr * .866f, dy = rr * .5f;
        const Vertex top{cx, cy - rr}, rt{cx + dx, cy - dy}, rb{cx + dx, cy + dy},
                     bot{cx, cy + rr}, lb{cx - dx, cy + dy}, lt{cx - dx, cy - dy};
        d.path({top, rt, rb, bot, lb, lt, top}, tint(0xFFFFFF, .95f), std::max(1.2f, w * .055f));
        d.line(cx, cy, top.x, top.y, tint(0xFFFFFF, .75f), std::max(1.f, w * .048f));
        d.line(cx, cy, rb.x, rb.y, tint(0xFFFFFF, .75f), std::max(1.f, w * .048f));
        d.line(cx, cy, lb.x, lb.y, tint(0xFFFFFF, .75f), std::max(1.f, w * .048f));
        break;
    }
    case 5: { // GLM Z.ai bold Z
        d.fill(b, r, tint(0x1C1E23));
        d.outline(b, r, tint(0xFFFFFF, .14f), .8f);
        const float pad = w * .26f;
        const float stroke = std::max(1.8f, w * .10f);
        d.path({{b.left + pad, b.top + pad}, {b.right - pad, b.top + pad},
                {b.left + pad, b.bottom - pad}, {b.right - pad, b.bottom - pad}},
               tint(0xFFFFFF, .96f), stroke);
        break;
    }
    case 6: { // DeepSeek blue whale
        d.fill(b, r, tint(0x4D6BFE));
        d.fill(box(b.left + w * .24f, b.top + h * .34f, w * .52f, h * .36f), h * .18f, tint(0xFFFFFF, .96f));
        d.path({{b.left + w * .26f, b.top + h * .46f}, {b.left + w * .16f, b.top + h * .30f}, {b.left + w * .34f, b.top + h * .36f}},
               tint(0xFFFFFF, .96f), std::max(1.5f, w * .08f));
        d.circle(b.left + w * .62f, b.top + h * .49f, std::max(1.2f, w * .045f), tint(0x4D6BFE));
        break;
    }
    case 7: { // OpenCode matte square emblem
        d.fill(b, r, tint(0x1B1C20));
        d.outline(b, r, tint(0xFFFFFF, .12f), .8f);
        const float pad = w * .22f;
        d.outline(box(b.left + pad, b.top + pad, w - 2 * pad, h - 2 * pad), 2.f, tint(0xFFFFFF, .28f), 1.2f);
        const float in = w * .30f;
        d.fill(box(b.left + in, b.top + h * .36f, w - 2 * in, h * .42f), 1.5f, tint(0xE8EAEE));
        break;
    }
    }
}

unsigned quotaBarColor(int pctLeft, Palette p) {
    if (pctLeft < 0) return p.ink;
    if (pctLeft < 10) return p.light ? 0xD93025 : 0xFF4740;
    if (pctLeft < 30) return p.light ? 0xD97706 : 0xFF9F0A;
    return p.light ? 0x169C53 : 0x2FD36B;
}

unsigned quotaTextColor(int pctLeft, Palette p) {
    if (pctLeft < 0) return p.muted;
    if (pctLeft == 0) return p.light ? 0xC62828 : 0xFF6B6B;
    return p.ink;
}

void drawGreenBar(Draw& d, Box b, float ratio, Palette p, unsigned color = 0x2FD36B) {
    const float rad = height(b) / 2.f;
    d.fill(b, rad, tint(p.ink, p.light ? .12f : .16f));
    const float fw = width(b) * std::clamp(ratio, 0.f, 1.f);
    if (fw > 0.8f) {
        d.fill(box(b.left, b.top, std::max(height(b), fw), height(b)), rad, tint(color));
    }
}

void drawGlowChart(Draw& d, Box b, std::span<const double> vals, unsigned color, float progress) {
    if (vals.size() < 2) return;
    d.sparkline(b, vals, color, progress, true);
    double lo = vals[0], hi = vals[0];
    for (double v : vals) { lo = std::min(lo, v); hi = std::max(hi, v); }
    const double span = std::max(1e-6, hi - lo);
    const size_t lastIdx = std::max<size_t>(1, static_cast<size_t>(std::lround((vals.size() - 1) * std::clamp(progress, 0.f, 1.f))));
    const float px = b.left + width(b) * (static_cast<float>(lastIdx) / static_cast<float>(vals.size() - 1));
    const float py = b.bottom - static_cast<float>((vals[lastIdx] - lo) / span) * (height(b) - 3.f) - 1.5f;
    d.circle(px, py, 4.2f, tint(color, .28f));
    d.circle(px, py, 2.2f, tint(color));
}

struct PlanDisplay {
    int p1 = -1, p2 = -1, p3 = -1;
    time_t r1At = 0, r2At = 0, r3At = 0;
    Text l1, l2, l3;
    Text r1, r2, r3, modelName, modelTokens;
    long long sessionTok = -1, weeklyTok = -1, secondaryTok = -1;
    double balance = -1, todaySpend = 0, weekSpend = 0;
    bool hasLimits = false;
    bool hasTokens = false;
    bool hasOverride = false;
    bool fromFeedFile = false;
    bool isSample = false;
};

const AiPlanLive& activeAiPlan(const ExtraWidgets& extra, int plan) {
    const auto& local = extra.aiLive[plan];
    if (plan == 2 && !local.fromFeedFile && extra.antigravityLive.ready) return extra.antigravityLive;
    return local;
}

PlanDisplay resolvePlan(int plan, const Model& model, const ExtraWidgets& extra, bool preview) {
    const int idx = std::clamp(plan, 0, AiPlanCount - 1);
    if (preview) {
        PlanDisplay s;
        s.isSample = true;
        switch (idx) {
        case 0: s.p1 = 41; s.p2 = 37; s.r1 = L"2h 14m"; s.r2 = L"6 Tue"; s.modelName = L"Opus 5.5"; s.modelTokens = L"31.0M"; s.sessionTok = 184000; s.weeklyTok = 31000000; s.hasLimits = true; break;
        case 1: s.p1 = 34; s.p2 = 21; s.r1 = L"2h 12m"; s.r2 = L"Thu 15"; s.hasLimits = true; break;
        case 2: s.p1 = 59; s.p2 = 52; s.r1 = L"3h 28m"; s.r2 = L"4d 22h"; s.hasLimits = true; break;
        case 3: s.p1 = 28; s.r1 = L"Fri 30"; s.hasLimits = true; break;
        case 4: s.p1 = 39; s.p2 = 12; s.r1 = L"Wed 28"; s.r2 = L"Wed 28"; s.hasLimits = true; break;
        case 5: s.p1 = 41; s.p2 = 12; s.p3 = 9; s.r1 = L"2h 54m"; s.r2 = L"Fri 16"; s.r3 = L"Sat 31"; s.hasLimits = true; break;
        case 6: s.balance = 18.42; s.todaySpend = 0.36; s.weekSpend = 2.91; break;
        case 7: s.r1 = L"last 5h"; s.r2 = L"last 7d"; s.modelName = L"Muse-spark"; s.modelTokens = L"248.4K"; s.sessionTok = 182400; s.weeklyTok = 248400; s.hasTokens = true; break;
        }
        return s;
    }

    // Live mode: NEVER populate hardcoded sample numbers!
    PlanDisplay out;
    const auto& live = activeAiPlan(extra, idx);
    const auto& ov = model.aiOverrides[idx];
    const time_t nowT = std::time(nullptr);

    if (live.ready) {
        out.hasLimits = live.hasLimits;
        out.hasTokens = live.hasTokens;
        out.fromFeedFile = live.fromFeedFile;
        out.p1 = live.p1;
        out.p2 = live.p2;
        out.p3 = live.p3;
        out.r1At = live.r1At;
        out.r2At = live.r2At;
        out.r3At = live.r3At;
        out.l1 = live.l1;
        out.l2 = live.l2;
        out.l3 = live.l3;
        out.r1 = live.r1;
        out.r2 = live.r2;
        out.r3 = live.r3;
        out.modelName = live.modelName;
        out.modelTokens = live.modelTokens;
        out.sessionTok = live.sessionTokens;
        out.weeklyTok = live.weeklyTokens;
        out.secondaryTok = live.secondaryTokens;
        out.balance = live.balance;
        out.todaySpend = live.todaySpend;
        out.weekSpend = live.weekSpend;

        // Live dynamic countdown & automatic expiry rollover when reset timestamp passes
        if (out.r1At > 0) {
            if (out.r1At <= nowT) {
                if (out.p1 >= 0) out.p1 = 100;
                out.r1 = L"—";
                out.r1At = 0;
            } else {
                out.r1 = formatResetCountdown(out.r1At, out.p1, live.r1WinMins);
            }
        }
        if (out.r2At > 0) {
            if (out.r2At <= nowT) {
                if (out.p2 >= 0) out.p2 = 100;
                out.r2 = L"—";
                out.r2At = 0;
            } else {
                out.r2 = formatResetCountdown(out.r2At, out.p2, live.r2WinMins);
            }
        }
        if (out.r3At > 0) {
            if (out.r3At <= nowT) {
                if (out.p3 >= 0) out.p3 = 100;
                out.r3 = L"—";
                out.r3At = 0;
            } else {
                out.r3 = formatResetCountdown(out.r3At, out.p3, 0);
            }
        }
    }

    if (ov.p1 >= 0) { out.p1 = ov.p1; out.hasLimits = true; out.hasOverride = true; }
    if (ov.p2 >= 0) { out.p2 = ov.p2; out.hasLimits = true; out.hasOverride = true; }
    if (ov.p3 >= 0) { out.p3 = ov.p3; out.hasLimits = true; out.hasOverride = true; }
    if (!ov.r1.empty()) { out.r1 = ov.r1; out.hasOverride = true; }
    if (!ov.r2.empty()) { out.r2 = ov.r2; out.hasOverride = true; }
    if (!ov.r3.empty()) { out.r3 = ov.r3; out.hasOverride = true; }
    if (!ov.extraLabel.empty()) { out.modelName = ov.extraLabel; out.hasOverride = true; }
    if (!ov.extraVal.empty()) { out.modelTokens = ov.extraVal; out.hasOverride = true; }
    if (idx == 6 && model.deepSeekBalance >= 0) {
        if (!live.ready) out.balance = model.deepSeekBalance;
        out.todaySpend = model.deepSeekToday;
        out.weekSpend = model.deepSeekWeek;
        out.hasOverride = true;
    }
    return out;
}

bool planLiveChanged(const AiPlanLive& a, const AiPlanLive& b) {
    return a.ready != b.ready || a.hasLimits != b.hasLimits || a.hasTokens != b.hasTokens ||
           a.fromFeedFile != b.fromFeedFile ||
           a.p1 != b.p1 || a.p2 != b.p2 || a.p3 != b.p3 ||
           a.r1At != b.r1At || a.r2At != b.r2At || a.r3At != b.r3At ||
           a.l1 != b.l1 || a.l2 != b.l2 || a.l3 != b.l3 ||
           a.r1 != b.r1 || a.r2 != b.r2 || a.r3 != b.r3 ||
           a.sessionTokens != b.sessionTokens || a.weeklyTokens != b.weeklyTokens ||
           a.secondaryTokens != b.secondaryTokens ||
           a.modelName != b.modelName || a.modelTokens != b.modelTokens ||
           a.sourceLabel != b.sourceLabel || a.balance != b.balance;
}

int planForWidget(int id) {
    switch (id) {
    case ClaudeUsage: case ClaudePlanUsage: return 0;
    case ChatGptUsage: return 1;
    case GeminiUsage: return 2;
    case GrokUsage: return 3;
    case CursorUsage: return 4;
    case GlmUsage: return 5;
    case DeepSeekBalance: return 6;
    case OpenCodeUsage: case OpenCodePlanUsage: return 7;
    default: return 0;
    }
}

constexpr const wchar_t* OverviewNames[] = {L"Claude", L"Codex", L"Antigravity"};
constexpr std::array<int, 3> OverviewPlans{0, 1, 2};
constexpr std::array<std::array<int, 2>, 3> PlanPairs{{{0, 1}, {0, 2}, {1, 2}}};
}

int readCpuUtilityPct() { return readCpuUtilityPctInternal(); }

void Dock::drawProWidget(View& view, int id, Box b, Palette p, bool preview, bool) {
    auto& d = view.d;
    const float w = width(b);
    const float motion = preview ? 1.f : widgetProgress();
    const auto at = [&](float x, float y, float bw, float bh) { return box(b.left + x, b.top + y, bw, bh); };
    const auto text = [&](const Text& label, float x, float y, float bw, float bh, Font font, unsigned rgb, Alignment align = Left) {
        d.text(label, at(x, y, bw, bh), font, tint(rgb), align);
    };

    if (&view == &sheet && page == id) {
        d.fill(b, Design::Radius, tint(p.high, .93f));
        d.outline(b, Design::Radius, tint(p.ink, .12f));
    }

    const auto drawTwoRowAi = [&](int plan, const wchar_t* l1, const wchar_t* l2) {
        const auto pd = resolvePlan(plan, model, extra, preview);
        drawBrandLogo(d, plan, at(10, 10, 36, 36));
        const float xLabel = 54.f, wLabel = 48.f;
        const float xPct = w - 45.f, wPct = 35.f;
        const float wReset = 44.f, xReset = xPct - wReset - 6.f;
        const float xBar = 104.f, wBar = std::max(26.f, xReset - xBar - 8.f);
        const Text labels[2] = {pd.l1.empty() ? Text(l1) : pd.l1, pd.l2.empty() ? Text(l2) : pd.l2};
        const int pcts[2] = {pd.p1, pd.p2};
        const long long toks[2] = {pd.sessionTok, pd.secondaryTok};
        const Text resets[2] = {pd.r1, pd.r2};
        for (int i = 0; i < 2; ++i) {
            const float ry = 10.f + i * 20.f;
            text(labels[i], xLabel, ry, wLabel, 16, Small, p.ink);
            if (pcts[i] >= 0) {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), (pcts[i] / 100.f) * motion, p, quotaBarColor(pcts[i], p));
                text(resets[i].empty() ? L"—" : resets[i], xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(num(pcts[i]) + L"%", xPct, ry, wPct, 16, Caption, quotaTextColor(pcts[i], p), Right);
            } else if (!preview && pd.hasTokens && toks[i] >= 0) {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), 0.f, p);
                text(resets[i].empty() ? L"24h" : resets[i], xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(formatCompact(toks[i]), xPct - 6.f, ry, wPct + 6.f, 16, Caption, p.ink, Right);
            } else {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), 0.f, p);
                text(L"—", xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(L"—", xPct, ry, wPct, 16, Caption, p.muted, Right);
            }
        }
    };

    if (id == AiLimitsTriple || (id == AiLimits &&
        model.aiLimitFirst < std::ssize(OverviewNames) && model.aiLimitSecond < std::ssize(OverviewNames))) {
        const std::array pair{model.aiLimitFirst, model.aiLimitSecond};
        const bool triple = id == AiLimitsTriple;
        const std::span<const int> plans = triple ? std::span<const int>(OverviewPlans) : std::span<const int>(pair);
        const float colW = 60.f;
        const float colGap = 10.f;
        const float columns[]{w - 11.f - colW * 2.f - colGap, w - 11.f - colW};
        const float headerY = triple ? 1.5f : 4.f;
        const float headerH = 12.f;
        d.text(L"5h", at(columns[0], headerY, colW, headerH), Micro, tint(p.muted, .78f), Left);
        d.text(L"Weekly", at(columns[1], headerY, colW, headerH), Micro, tint(p.muted, .78f), Left);
        const float logoSize = triple ? 12.f : 14.f;
        const float nameX = 10.f + logoSize + 5.f;
        const float rowH = triple ? 13.f : 15.f;
        const float textH = 15.f;
        const float textYOffset = (rowH - textH) * .5f;
        const float barW = 28.f;
        const float barH = triple ? 3.f : 3.5f;
        for (size_t row = 0; row < plans.size(); ++row) {
            const int plan = plans[row];
            const float y = triple ? 13.f + row * 13.5f : 17.f + row * 18.f;
            const auto pd = resolvePlan(plan, model, extra, preview);
            drawBrandLogo(d, plan, at(10.f, y + (rowH - logoSize) * .5f, logoSize, logoSize));
            text(OverviewNames[plan], nameX, y + textYOffset, std::max(36.f, columns[0] - nameX - 6.f), textH, Caption, p.ink);
            const int percentages[]{pd.p1, pd.p2};
            const long long tokens[]{pd.sessionTok, pd.weeklyTok};
            for (int column = 0; column < 2; ++column) {
                const int remaining = percentages[column];
                const float x = columns[column];
                if (remaining < 0 && plan == 0 && pd.hasTokens && tokens[column] >= 0) {
                    text(formatCompact(tokens[column]) + L" t", x, y + textYOffset, colW, textH, Caption, p.muted, Right);
                    continue;
                }
                drawGreenBar(d, at(x, y + (rowH - barH) * .5f + .5f, barW, barH), std::max(0, remaining) / 100.f * motion,
                    p, quotaBarColor(remaining, p));
                text(remaining >= 0 ? num(remaining) + L"%" : L"—", x + barW + 1.f, y + textYOffset, colW - barW - 1.f, textH,
                    Caption, quotaTextColor(remaining, p), Right);
            }
        }
        return;
    }

    switch (id) {
    case AiLimits: {
        const int plans[2] = {std::clamp(model.aiLimitFirst, 0, AiPlanCount - 1),
                              std::clamp(model.aiLimitSecond, 0, AiPlanCount - 1)};
        for (int i = 0; i < 2; ++i) {
            const float ry = 9.f + i * 22.f;
            const auto pd = resolvePlan(plans[i], model, extra, preview);
            drawBrandLogo(d, plans[i], at(10, ry, 16, 16));
            text(AiPlanNames[plans[i]], 32, ry, 58, 16, Caption, p.ink);
            const float xPct = w - 45.f, wPct = 35.f;
            const float wReset = 44.f, xReset = xPct - wReset - 6.f;
            const float xBar = 92.f, wBar = std::max(26.f, xReset - xBar - 8.f);
            if (plans[i] == 6) {
                const float frac = pd.balance >= 0 ? std::clamp(static_cast<float>(pd.balance / 25.0), 0.f, 1.f) : 0.f;
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), frac * motion, p, quotaBarColor(pd.balance >= 0 ? 100 : -1, p));
                text(pd.balance >= 0 ? L"USD" : L"—", xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(pd.balance >= 0 ? formatUsd(pd.balance, false) : L"—", xPct - 2.f, ry, wPct + 2.f, 16, Caption, pd.balance >= 0 ? p.ink : p.muted, Right);
            } else if (pd.p1 >= 0) {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), (pd.p1 / 100.f) * motion, p, quotaBarColor(pd.p1, p));
                text(pd.r1.empty() ? L"—" : pd.r1, xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(num(pd.p1) + L"%", xPct, ry, wPct, 16, Caption, quotaTextColor(pd.p1, p), Right);
            } else if (pd.hasTokens && pd.sessionTok >= 0) {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), 0.f, p);
                text(pd.r1.empty() ? L"last 5h" : pd.r1, xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(formatCompact(pd.sessionTok), xPct - 2.f, ry, wPct + 2.f, 16, Caption, p.ink, Right);
            } else {
                drawGreenBar(d, at(xBar, ry + 6.f, wBar, 4.f), 0.f, p);
                text(L"—", xReset, ry + 1.f, wReset, 14, Micro, p.muted, Right);
                text(L"—", xPct, ry, wPct, 16, Caption, p.muted, Right);
            }
        }
        break;
    }
    case ClaudeUsage: {
        auto pd = resolvePlan(0, model, extra, preview);
        if (preview) { pd.p1 = 100; pd.r1 = L"1h 25m"; }
        drawBrandLogo(d, 0, at(9, 9, 38, 38));
        text(L"Session", 55, 6, 72, 16, Caption, p.ink);
        if (pd.p1 >= 0) {
            text(pd.r1.empty() ? L"—" : pd.r1, w - 76, 6, 65, 16, Small, p.muted, Right);
            text(num(pd.p1) + L"%", 55, 20, w - 66, 22, Value, quotaTextColor(pd.p1, p));
            drawGreenBar(d, at(55, 44, w - 66, 4.5f), (pd.p1 / 100.f) * motion, p, quotaBarColor(pd.p1, p));
        } else if (pd.hasTokens && pd.sessionTok >= 0) {
            text(L"last 5h", w - 76, 6, 65, 16, Small, p.muted, Right);
            text(formatCompact(pd.sessionTok), 55, 22, w - 66, 24, Value, p.ink);
        } else {
            text(L"—", w - 76, 6, 65, 16, Small, p.muted, Right);
            text(L"—", 55, 20, w - 66, 22, Value, p.muted);
            drawGreenBar(d, at(55, 44, w - 66, 4.5f), 0.f, p);
        }
        break;
    }
    case ClaudePlanUsage: {
        auto pd = resolvePlan(0, model, extra, preview);
        if (preview) { pd.p1 = 100; pd.r1 = L"1h 26m"; pd.p2 = 37; pd.r2 = L"6 Tue"; pd.modelName = L"Opus 5.5"; pd.modelTokens = L"31.0M"; }
        drawBrandLogo(d, 0, at(10, 10, 36, 36));
        const float xLabel = 53.f, xBar = 103.f, wBar = std::max(28.f, w - 201.f);
        const float xReset = xBar + wBar + 5.f;
        if (pd.p1 >= 0 || pd.p2 >= 0) {
            text(L"Session", xLabel, 5, 48, 14, Micro, p.ink);
            drawGreenBar(d, at(xBar, 10.5f, wBar, 3.5f), (std::max(0, pd.p1) / 100.f) * motion, p, quotaBarColor(pd.p1, p));
            text(pd.r1.empty() ? L"—" : pd.r1, xReset, 5, 46, 14, Micro, p.muted, Right);
            text(pd.p1 >= 0 ? (num(pd.p1) + L"%") : L"—", w - 45, 5, 35, 14, Small, quotaTextColor(pd.p1, p), Right);

            text(L"Weekly", xLabel, 21, 48, 14, Micro, p.ink);
            drawGreenBar(d, at(xBar, 26.5f, wBar, 3.5f), (std::max(0, pd.p2) / 100.f) * motion, p, quotaBarColor(pd.p2, p));
            text(pd.r2.empty() ? L"—" : pd.r2, xReset, 21, 46, 14, Micro, p.muted, Right);
            text(pd.p2 >= 0 ? (num(pd.p2) + L"%") : L"—", w - 45, 21, 35, 14, Small, quotaTextColor(pd.p2, p), Right);
        } else if (pd.hasTokens) {
            text(L"Session", xLabel, 5, 76, 14, Micro, p.ink);
            text(L"last 5h", w - 108, 5, 50, 14, Micro, p.muted, Right);
            text(formatCompact(pd.sessionTok), w - 55, 5, 45, 14, Small, p.ink, Right);

            text(L"Weekly", xLabel, 21, 76, 14, Micro, p.ink);
            text(L"last 7d", w - 108, 21, 50, 14, Micro, p.muted, Right);
            text(formatCompact(pd.weeklyTok), w - 55, 21, 45, 14, Small, p.ink, Right);
        } else {
            text(L"Session", xLabel, 5, 48, 14, Micro, p.ink);
            drawGreenBar(d, at(xBar, 10.5f, wBar, 3.5f), 0.f, p);
            text(L"—", xReset, 5, 46, 14, Micro, p.muted, Right);
            text(L"—", w - 45, 5, 35, 14, Small, p.muted, Right);

            text(L"Weekly", xLabel, 21, 48, 14, Micro, p.ink);
            drawGreenBar(d, at(xBar, 26.5f, wBar, 3.5f), 0.f, p);
            text(L"—", xReset, 21, 46, 14, Micro, p.muted, Right);
            text(L"—", w - 45, 21, 35, 14, Small, p.muted, Right);
        }

        text(pd.modelName.empty() ? L"Model" : pd.modelName, xLabel, 37, 94, 14, Micro, p.ink);
        text(L"most used", w - 103, 37, 54, 14, Micro, p.muted, Right);
        text(pd.modelTokens.empty() ? L"—" : pd.modelTokens, w - 46, 37, 36, 14, Small, pd.modelTokens.empty() ? p.muted : p.ink, Right);
        break;
    }
    case ChatGptUsage:
        drawTwoRowAi(1, L"Session", L"Weekly");
        break;
    case GeminiUsage:
        drawTwoRowAi(2, L"5h", L"Weekly");
        break;
    case GrokUsage: {
        const auto pd = resolvePlan(3, model, extra, preview);
        drawBrandLogo(d, 3, at(10, 10, 36, 36));
        text(L"Credits", 54, 20, 46, 16, Small, p.ink);
        const float xBar = 102.f, wBar = std::max(28.f, w - 196.f);
        drawGreenBar(d, at(xBar, 26.f, wBar, 4.f), pd.p1 >= 0 ? (pd.p1 / 100.f) * motion : 0.f, p, quotaBarColor(pd.p1, p));
        text(pd.r1.empty() ? L"—" : pd.r1, xBar + wBar + 5.f, 21, 44, 14, Micro, p.muted, Right);
        text(pd.p1 >= 0 ? (num(pd.p1) + L"%") : L"—", w - 42, 20, 32, 16, Caption, quotaTextColor(pd.p1, p), Right);
        break;
    }
    case DeepSeekBalance: {
        const auto pd = resolvePlan(6, model, extra, preview);
        drawBrandLogo(d, 6, at(10, 10, 36, 36));
        text(L"Balance", 54, 5, 76, 14, Micro, p.ink);
        text(pd.balance >= 0 ? formatUsd(pd.balance) : L"—", w - 92, 5, 81, 14, Small, pd.balance >= 0 ? p.ink : p.muted, Right);
        text(L"Today", 54, 21, 76, 14, Micro, p.muted);
        text(pd.balance >= 0 ? formatUsd(pd.todaySpend) : L"—", w - 92, 21, 81, 14, Small, pd.balance >= 0 ? p.ink : p.muted, Right);
        text(L"7 days", 54, 37, 76, 14, Micro, p.muted);
        text(pd.balance >= 0 ? formatUsd(pd.weekSpend) : L"—", w - 92, 37, 81, 14, Small, pd.balance >= 0 ? p.ink : p.muted, Right);
        break;
    }
    case GlmUsage: {
        const auto pd = resolvePlan(5, model, extra, preview);
        drawBrandLogo(d, 5, at(10, 10, 36, 36));
        const float xLabel = 53.f, xBar = 101.f, wBar = std::max(28.f, w - 198.f);
        const float xReset = xBar + wBar + 5.f;
        const wchar_t* labels[3] = {L"Session", L"Weekly", L"MCP"};
        const int pcts[3] = {pd.p1, pd.p2, pd.p3};
        const Text resets[3] = {pd.r1, pd.r2, pd.r3};
        for (int i = 0; i < 3; ++i) {
            const float ry = 5.f + i * 16.f;
            text(labels[i], xLabel, ry, 46, 14, Micro, p.ink);
            drawGreenBar(d, at(xBar, ry + 5.5f, wBar, 3.5f), pcts[i] >= 0 ? (pcts[i] / 100.f) * motion : 0.f, p, quotaBarColor(pcts[i], p));
            text(resets[i].empty() ? L"—" : resets[i], xReset, ry, 45, 14, Micro, p.muted, Right);
            text(pcts[i] >= 0 ? (num(pcts[i]) + L"%") : L"—", w - 44, ry, 34, 14, Small, quotaTextColor(pcts[i], p), Right);
        }
        break;
    }
    case OpenCodeUsage: {
        const auto pd = resolvePlan(7, model, extra, preview);
        drawBrandLogo(d, 7, at(10, 10, 36, 36));
        text(L"Session", 55, 8, 72, 16, Caption, p.ink);
        text(pd.r1.empty() ? L"last 5h" : pd.r1, w - 74, 8, 63, 16, Small, p.muted, Right);
        text(formatCompact(pd.sessionTok), 55, 25, w - 65, 24, Value, pd.sessionTok >= 0 ? p.ink : p.muted);
        break;
    }
    case OpenCodePlanUsage: {
        auto pd = resolvePlan(7, model, extra, preview);
        if (preview) { pd.sessionTok = 0; pd.weeklyTok = 248400; pd.modelName = L"Muse-spark"; pd.modelTokens = L"248.4K"; }
        drawBrandLogo(d, 7, at(10, 10, 36, 36));
        text(L"Session", 54, 5, 76, 14, Micro, p.ink);
        text(L"last 5h", w - 108, 5, 50, 14, Micro, p.muted, Right);
        text(formatCompact(pd.sessionTok), w - 55, 5, 45, 14, Small, pd.sessionTok >= 0 ? p.ink : p.muted, Right);

        text(L"Weekly", 54, 21, 76, 14, Micro, p.ink);
        text(L"last 7d", w - 108, 21, 50, 14, Micro, p.muted, Right);
        text(formatCompact(pd.weeklyTok), w - 55, 21, 45, 14, Small, pd.weeklyTok >= 0 ? p.ink : p.muted, Right);

        text(pd.modelName.empty() ? L"Model" : pd.modelName, 54, 37, 86, 14, Micro, p.ink);
        text(L"most used", w - 108, 37, 50, 14, Micro, p.muted, Right);
        text(pd.modelTokens.empty() ? L"—" : pd.modelTokens, w - 55, 37, 45, 14, Small, pd.modelTokens.empty() ? p.muted : p.ink, Right);
        break;
    }
    case CursorUsage:
        drawTwoRowAi(4, L"Auto", L"API");
        break;
    case StripeChart: {
        static const std::vector<double> sampleTrend = {420, 610, 560, 520, 580, 690, 670, 740, 810, 780, 860, 840, 910, 790};
        const bool live = extra.stripe.ready && !preview;
        const bool manual = !live && model.stripeRevenue >= 0 && !preview;
        const double rev = live ? extra.stripe.value : manual ? model.stripeRevenue : 2980.0;
        const double chg = live ? extra.stripe.changePct : manual ? model.stripeChange : 31.0;
        const auto& tr = (live && extra.stripe.trend.size() > 1) ? extra.stripe.trend : sampleTrend;
        d.circle(b.left + 19.5f, b.top + 15.f, 8.f, tint(0x635BFF));
        text(L"S", 12, 7.5f, 15, 15, Small, 0xFFFFFF, Center);
        text(L"Net revenue", 32, 6, 102, 18, Strong, p.ink);
        text(formatUsd(rev, false), w - 98, 4, 86, 19, Strong, p.ink, Right);
        const unsigned chgCol = chg < 0 ? (p.light ? 0xC62828 : 0xFF6B6B) : (p.light ? 0x169C53 : 0x2FD36B);
        text((chg < 0 ? L"▼ " : L"▲ ") + num(static_cast<long long>(std::lround(std::abs(chg)))) + L"%",
            w - 92, 20, 80, 13, Micro, chgCol, Right);
        drawGlowChart(d, at(12, 33, w - 24, 17), tr, 0x2FD36B, motion);
        break;
    }
    case DataFastChart: {
        static const std::vector<double> sampleTrend = {190, 210, 205, 225, 218, 195, 185, 240, 265, 280, 295, 285, 310, 298};
        const bool live = extra.dataFast.ready && !preview;
        const bool manual = !live && model.dataFastVisitors >= 0 && !preview;
        const long long vis = live ? static_cast<long long>(std::lround(extra.dataFast.value)) : manual ? model.dataFastVisitors : 4049;
        const double chg = live ? extra.dataFast.changePct : manual ? model.dataFastChange : 32.0;
        const auto& tr = (live && extra.dataFast.trend.size() > 1) ? extra.dataFast.trend : sampleTrend;
        d.circle(b.left + 19.5f, b.top + 15.f, 8.f, tint(p.ink, .10f));
        d.circle(b.left + 19.5f, b.top + 15.f, 8.f, tint(p.ink, .16f), false, .7f);
        d.fill(at(15.5f, 15.5f, 2.f, 4.f), 1.f, tint(0xFF7A45));
        d.fill(at(18.5f, 11.5f, 2.f, 8.f), 1.f, tint(0xFF7A45));
        d.fill(at(21.5f, 13.5f, 2.f, 6.f), 1.f, tint(0xFF7A45));
        text(L"Visitors", 32, 6, 98, 18, Strong, p.ink);
        text(formatCountComma(vis), w - 98, 4, 86, 19, Strong, p.ink, Right);
        const unsigned chgCol = chg < 0 ? (p.light ? 0xC62828 : 0xFF6B6B) : (p.light ? 0x169C53 : 0x2FD36B);
        text((chg < 0 ? L"▼ " : L"▲ ") + num(static_cast<long long>(std::lround(std::abs(chg)))) + L"%",
            w - 92, 20, 80, 13, Micro, chgCol, Right);
        drawGlowChart(d, at(12, 33, w - 24, 17), tr, 0xFF7A45, motion);
        break;
    }
    case GitHubActivity: {
        static constexpr uint8_t sampleGrid[42] = {
            1,2,3,2,1,2,1,3,2,4,2,1,2,2,
            2,3,4,3,3,2,3,4,3,3,4,3,3,4,
            0,1,1,2,2,1,0,1,2,1,2,3,2,3
        };
        const bool live = extra.github.ready && !preview;
        const int td = live ? extra.github.today : 6;
        const int wk = live ? extra.github.week : 37;
        const int st = live ? extra.github.streak : 23;
        text(num(td), 11, 5, 22, 14, Caption, p.ink);
        text(L"today", 24, 6, 36, 13, Micro, p.muted);
        text(num(wk), 66, 5, 26, 14, Caption, p.ink);
        text(L"week", 86, 6, 34, 13, Micro, p.muted);
        // Orange flame icon
        const float fx = b.left + 131.f, fy = b.top + 11.f;
        d.circle(fx, fy + 1.5f, 3.6f, tint(0xFF6B24));
        d.circle(fx, fy - 1.f, 2.2f, tint(0xFF9F2E));
        d.circle(fx, fy + 2.f, 1.5f, tint(0xFFD166));
        text(num(st), 140, 5, 24, 14, Caption, p.ink);
        text(L"streak", 160, 6, 44, 13, Micro, p.muted);

        const unsigned greensDark[5]  = {0x2A2D31, 0x0E4429, 0x006D32, 0x26A641, 0x39D353};
        const unsigned greensLight[5] = {0xDFE3E8, 0x9BE9A8, 0x40C463, 0x30A14E, 0x216E39};
        const float startX = 11.f, startY = 23.f;
        const float cellW = (w - 22.f - 13.f * 2.2f) / 14.f;
        constexpr float cellH = 7.4f, gapX = 2.2f, gapY = 2.4f;
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 14; ++col) {
                const uint8_t lvl = live ? extra.github.grid[row * 14 + col] : sampleGrid[row * 14 + col];
                const unsigned colRgb = (p.light ? greensLight : greensDark)[std::min<uint8_t>(4, lvl)];
                d.fill(at(startX + col * (cellW + gapX), startY + row * (cellH + gapY), cellW, cellH), 2.f,
                    tint(colRgb, lvl == 0 && !p.light ? .75f : 1.f));
            }
        }
        break;
    }
    case Temperature: {
        const bool hasTemp = preview || extra.cpuTempC >= 0;
        const int temp = preview ? 58 : extra.cpuTempC;
        const bool hasCpu = preview || cpu >= 0;
        const int cVal = preview ? 23 : std::max(0, cpu);
        const int rVal = preview ? 65 : std::max(0, ram);
        const float smoothCpu = preview ? 23.f : (hasCpu ? (cpuAnim.initialized ? cpuAnim.value : static_cast<float>(cVal)) : 0.f);
        const float smoothRam = preview ? 65.f : (ramAnim.initialized ? ramAnim.value : static_cast<float>(rVal));
        const Box gauge = at(12, 8, 40, 40);
        d.arc(gauge, 135.f, 270.f, tint(p.ink, .16f), 4.f);
        if (hasTemp) {
            const float frac = std::clamp((temp - 20.f) / 80.f, 0.08f, 1.f) * motion;
            const unsigned tempCol = temp >= 82 ? Design::Red : temp >= 68 ? Design::Amber : 0x2FD36B;
            d.arc(gauge, 135.f, 270.f * frac, tint(tempCol), 4.f);
        }
        text(hasTemp ? (num(temp) + L"°") : L"—°", 12, 16, 40, 18, Strong, hasTemp ? p.ink : p.muted, Center);
        text(L"CPU", 12, 38, 40, 12, Micro, p.muted, Center);

        const unsigned barBlue = p.light ? 0x1A73E8 : 0x2B8BFF;
        text(L"CPU", 64, 10, 32, 16, Caption, p.muted);
        drawGreenBar(d, at(100, 16, w - 150, 4.5f), std::clamp(smoothCpu, 0.f, 100.f) / 100.f, p, barBlue);
        text(hasCpu ? (num(cVal) + L"%") : L"—", w - 45, 9, 34, 18, Strong, hasCpu ? p.ink : p.muted, Right);

        text(L"RAM", 64, 30, 32, 16, Caption, p.muted);
        drawGreenBar(d, at(100, 36, w - 150, 4.5f), std::clamp(smoothRam, 0.f, 100.f) / 100.f, p, barBlue);
        text(num(rVal) + L"%", w - 45, 29, 34, 18, Strong, p.ink, Right);
        break;
    }
    case XProfile: {
        const Box logo = at(10, 9, 38, 38);
        d.fill(logo, 9.f, tint(0x060708));
        d.outline(logo, 9.f, tint(0xFFFFFF, .14f), .8f);
        const float lx = logo.left + 10.f, rx = logo.right - 10.f, ty = logo.top + 10.f, by = logo.bottom - 10.f;
        d.line(lx, ty, rx - 4.f, by, tint(0xFFFFFF), 2.f);
        d.line(lx + 4.f, ty, rx, by, tint(0xFFFFFF), 2.f);
        d.line(lx, ty, lx + 4.f, ty, tint(0xFFFFFF), 1.6f);
        d.line(rx - 4.f, by, rx, by, tint(0xFFFFFF), 1.6f);
        d.line(rx - 1.f, ty, lx + 1.f, by, tint(0xFFFFFF), 1.8f);

        const long long followers = (!preview && extra.xFollowersLive >= 0) ? extra.xFollowersLive
            : (!preview && model.xFollowers >= 0) ? model.xFollowers : 2400;
        text(formatCompact(followers), 58, 7, w - 66, 24, Value, p.ink);
        text(L"followers", 58, 31, w - 66, 16, Caption, p.muted);
        break;
    }
    }
}

void Dock::sampleProWidgets(bool forceWeb) {
    const auto stamp = GetTickCount64();
    const auto active = [&](int id) { return page == id || (shown(id) && !isFoldedWidget(id)); };

    const bool triple = active(AiLimitsTriple);
    const bool aiLimits = active(AiLimits);
    const auto aiPlanShown = [&](int idx) {
        if (aiLimits && (model.aiLimitFirst == idx || model.aiLimitSecond == idx)) return true;
        if (triple && (idx == 0 || idx == 1 || idx == 2)) return true;
        return false;
    };
    const bool needClaude   = forceWeb || aiPlanShown(0) || active(ClaudeUsage) || active(ClaudePlanUsage);
    const bool needCodex    = forceWeb || aiPlanShown(1) || active(ChatGptUsage);
    const bool needGemini   = forceWeb || aiPlanShown(2) || active(GeminiUsage);
    const bool needOpenCode = forceWeb || aiPlanShown(7) || active(OpenCodeUsage) || active(OpenCodePlanUsage);
    const bool aiAny = needClaude || needCodex || needGemini || needOpenCode || aiLimits ||
        active(CursorUsage) || active(GrokUsage) || active(GlmUsage) || active(DeepSeekBalance);
    const bool needTemp = active(Temperature) || forceWeb;
    const bool needCpuPdh = needTemp || active(System) || active(CpuMeter);
    const bool needLocal = forceWeb || needTemp || needCpuPdh || aiAny;

    // Fast continuous local telemetry (persistent PDH + mtime-gated ai_usage.json every 5s),
    // while expensive recursive JSONL/SQLite log scans run ONLY for active providers every 30s.
    static ULONGLONG lastDeepScanAt = 0;
    const bool doDeepScan = forceWeb || !lastDeepScanAt || (stamp - lastDeepScanAt >= 30000);
    if (needLocal && !extra.proLocalRequest.valid() && (forceWeb || !extra.proLocalAt || stamp >= extra.proLocalAt)) {
        extra.proLocalAt = stamp + 5000;
        if (doDeepScan && aiAny) lastDeepScanAt = stamp;
        const bool scanClaude   = doDeepScan && needClaude;
        const bool scanCodex    = doDeepScan && needCodex && !(extra.aiLive[1].ready && !extra.aiLive[1].fromFeedFile && !forceWeb);
        const bool scanGemini   = doDeepScan && needGemini && !extra.antigravityLive.ready;
        const bool scanOpenCode = doDeepScan && needOpenCode;
        const bool scanFeed     = forceWeb || aiAny;
        const HWND notify = hwnd;
        const auto stop = extra.proStop;
        try {
            extra.proLocalRequest = std::async(std::launch::async,
                [needTemp, needCpuPdh, scanClaude, scanCodex, scanGemini, scanOpenCode, scanFeed, notify, stop]() {
                winrt::init_apartment();
                ProLocalSnapshot snap;
                if (needCpuPdh) sharedPdhCpu().primeAsync();
                if (needTemp) snap.cpuTempC = readCpuTempC();
                if (scanClaude)   readLocalClaude(snap.ai[0]);
                if (scanCodex)    readLocalCodex(snap.ai[1]);
                if (scanGemini)   readLocalGemini(snap.ai[2]);
                if (scanOpenCode) readLocalOpenCode(snap.ai[7]);
                if (scanFeed)     readAiFeedFile(snap.ai);
                winrt::uninit_apartment();
                if (notify && (!stop || !stop->load())) PostMessageW(notify, AsyncWake, 0, 0);
                return snap;
            });
        } catch (const std::exception&) {}
    }

    // Official Codex app-server live rate-limit refresh (every 60s when visible, or immediately on Yenile)
    const bool needCodexLive = forceWeb || triple || active(ChatGptUsage) ||
        (aiLimits && (model.aiLimitFirst == 1 || model.aiLimitSecond == 1));
    if (needCodexLive && !extra.codexLiveRequest.valid() && (forceWeb || !extra.codexLiveAt || stamp >= extra.codexLiveAt)) {
        extra.codexLiveAt = stamp + 60000;
        auto stop = extra.proStop;
        try {
            extra.codexLiveRequest = std::async(std::launch::async, [stop]() {
                winrt::init_apartment();
                auto res = fetchCodexAppServer(stop);
                winrt::uninit_apartment();
                return res;
            });
        } catch (const std::exception&) {}
    }

    const bool needAntigravity = triple || active(GeminiUsage) ||
        (aiLimits && (model.aiLimitFirst == 2 || model.aiLimitSecond == 2));
    if (needAntigravity && !extra.aiLive[2].fromFeedFile && !extra.antigravityRequest.valid() &&
        (forceWeb || stamp >= extra.antigravityAt)) {
        extra.antigravityAt = stamp + 60000;
        auto stop = extra.proStop;
        try {
            extra.antigravityRequest = std::async(std::launch::async, [stop]() {
                winrt::init_apartment();
                auto live = fetchAntigravityUsage(*stop);
                winrt::uninit_apartment();
                return live;
            });
        } catch (const std::exception&) {}
    }

    // Only query Credential Manager when a web refresh is actually due (never every 4.5s on the UI thread!)
    if (!extra.proWebRequest.valid() && (forceWeb || !extra.proWebAt || stamp >= extra.proWebAt)) {
        unsigned webMask = 0;
        const bool wantGh = (active(GitHubActivity) || (forceWeb && page == GitHubActivity)) && !model.githubUser.empty();
        const bool wantDs = active(DeepSeekBalance) || (aiLimits && (model.aiLimitFirst == 6 || model.aiLimitSecond == 6)) || (forceWeb && page == DeepSeekBalance);
        const bool wantSt = active(StripeChart) || (forceWeb && page == StripeChart);
        const bool wantDf = active(DataFastChart) || (forceWeb && page == DataFastChart);

        const Text dsKey = wantDs ? readCred(CredDeepSeek) : Text{};
        const Text stKey = wantSt ? readCred(CredStripe) : Text{};
        const Text dfKey = wantDf ? readCred(CredDataFast) : Text{};
        if (wantGh) webMask |= 1u;
        if (!dsKey.empty()) webMask |= 2u;
        if (!stKey.empty()) webMask |= 4u;
        if (!dfKey.empty()) webMask |= 8u;

        extra.proWebAt = stamp + 180000; // 3 min refresh
        if (webMask) {
            const Text ghUser = model.githubUser;
            try {
                extra.proWebRequest = std::async(std::launch::async, [webMask, ghUser, dsKey, stKey, dfKey]() {
                    winrt::init_apartment();
                    ProWebSnapshot snap;
                    snap.mask = webMask;
                    if (webMask & 1u) snap.github = fetchGitHub(ghUser);
                    if (webMask & 2u) snap.deepSeek = fetchDeepSeek(dsKey);
                    if (webMask & 4u) snap.stripe = fetchStripe(stKey);
                    if (webMask & 8u) snap.dataFast = fetchDataFast(dfKey);
                    winrt::uninit_apartment();
                    return snap;
                });
            } catch (const std::exception&) {}
        }
    }
}

bool Dock::tickProWidgets(ULONGLONG) {
    bool dirty = false;
    if (extra.antigravityRequest.valid() &&
        extra.antigravityRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        AiPlanLive live;
        try { live = extra.antigravityRequest.get(); } catch (const std::exception&) {}
        if (!extra.aiLive[2].fromFeedFile && planLiveChanged(extra.antigravityLive, live)) dirty = true;
        if (!extra.aiLive[2].fromFeedFile) extra.antigravityLive = std::move(live);
    }
    if (extra.codexLiveRequest.valid() && extra.codexLiveRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto live = extra.codexLiveRequest.get();
            if (live.ready && !extra.aiLive[1].fromFeedFile) {
                if (planLiveChanged(extra.aiLive[1], live)) dirty = true;
                extra.aiLive[1] = std::move(live);
            }
        } catch (const std::exception&) {}
    }
    if (extra.proLocalRequest.valid() && extra.proLocalRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto snap = extra.proLocalRequest.get();
            if (snap.cpuTempC >= 0 && snap.cpuTempC != extra.cpuTempC) { extra.cpuTempC = snap.cpuTempC; dirty = true; }
            for (int i = 0; i < AiPlanCount; ++i) {
                if (i == 2 && snap.ai[i].fromFeedFile) {
                    extra.antigravityLive = {};
                    extra.antigravityAt = 0;
                }
                if (!snap.ai[i].ready) {
                    if (extra.aiLive[i].fromFeedFile) { extra.aiLive[i] = {}; dirty = true; }
                    continue;
                }
                // Don't let an older local .codex/sessions JSONL overwrite a fresher codex app-server live snapshot
                if (i == 1 && !snap.ai[1].fromFeedFile && extra.aiLive[1].ready && !extra.aiLive[1].fromFeedFile &&
                    extra.aiLive[1].updatedAt >= snap.ai[1].updatedAt) continue;
                if (planLiveChanged(extra.aiLive[i], snap.ai[i])) dirty = true;
                extra.aiLive[i] = std::move(snap.ai[i]);
            }
        } catch (const std::exception&) {}
    }
    if (extra.proWebRequest.valid() && extra.proWebRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto snap = extra.proWebRequest.get();
            if (snap.mask & 1u) { extra.github = std::move(snap.github); dirty = true; }
            if ((snap.mask & 2u) && snap.deepSeek.ready) {
                if (planLiveChanged(extra.aiLive[6], snap.deepSeek)) dirty = true;
                extra.aiLive[6] = std::move(snap.deepSeek);
            }
            if (snap.mask & 4u) { extra.stripe = std::move(snap.stripe); dirty = true; }
            if (snap.mask & 8u) { extra.dataFast = std::move(snap.dataFast); dirty = true; }
        } catch (const std::exception&) {}
    }
    return dirty;
}

void Dock::editProKey(int id) {
    const wchar_t* credName = id == DeepSeekBalance ? CredDeepSeek : id == StripeChart ? CredStripe : CredDataFast;
    const wchar_t* label = id == DeepSeekBalance ? L"DeepSeek API anahtarı"
        : id == StripeChart ? L"Stripe Restricted API anahtarı"
        : L"DataFast API anahtarı";
    const bool hasKey = !readCred(credName).empty();
    std::vector<Field> fields{{label, L"", false, {}, true}};
    const int action = edit(Widgets[id].title, fields, hasKey, [](const auto& f) -> Text {
        return f[0].value.size() < 8 ? L"Geçerli bir API anahtarı gir." : L"";
    });
    if (!action) return;
    bool ok = false;
    if (action == 2) ok = CredDeleteW(credName, CRED_TYPE_GENERIC, 0) != FALSE;
    else {
        ok = writeCred(credName, fields[0].value);
        SecureZeroMemory(fields[0].value.data(), fields[0].value.size() * sizeof(wchar_t));
    }
    if (!ok) { report(L"Kimlik Bilgisi Yöneticisi güncellenemedi."); return; }
    extra.proWebAt = 0;
    sampleProWidgets(true);
    paintBar();
    paintSheet();
}

void Dock::editProWidget(int id) {
    if (id == AiLimits) {
        std::vector<Text> planChoices(AiPlanNames, AiPlanNames + AiPlanCount);
        std::vector<Field> fields{
            {L"1. Plan", AiPlanNames[std::clamp(model.aiLimitFirst, 0, AiPlanCount - 1)], false, planChoices},
            {L"2. Plan", AiPlanNames[std::clamp(model.aiLimitSecond, 0, AiPlanCount - 1)], false, planChoices}
        };
        if (edit(L"AI Limitleri", fields, false, [](const auto& values) -> Text {
            return values[0].value == values[1].value ? L"İki farklı plan seç." : L"";
        }) != 1) return;
        for (int i = 0; i < AiPlanCount; ++i) {
            if (fields[0].value == AiPlanNames[i]) model.aiLimitFirst = i;
            if (fields[1].value == AiPlanNames[i]) model.aiLimitSecond = i;
        }
        extra.proLocalAt = 0;
        extra.codexLiveAt = 0;
        sampleProWidgets(true);
        changed(false);
        return;
    }
    if (id == GitHubActivity) {
        std::vector<Field> fields{{L"GitHub kullanıcı adı", model.githubUser}};
        if (edit(L"GitHub Aktivitesi", fields) != 1) return;
        model.githubUser = fields[0].value;
        while (!model.githubUser.empty() && model.githubUser[0] == L'@') model.githubUser.erase(model.githubUser.begin());
        extra.github = {};
        extra.proWebAt = 0;
        changed(false);
        sampleProWidgets(true);
        return;
    }
    if (id == XProfile) {
        std::vector<Field> fields{
            {L"X kullanıcı adı (@)", model.xHandle},
            {L"Takipçi sayısı", model.xFollowers >= 0 ? num(model.xFollowers) : L"2400"}
        };
        if (edit(L"X Profili", fields) != 1) return;
        model.xHandle = fields[0].value;
        try { model.xFollowers = std::max(0LL, std::stoll(fields[1].value)); } catch (...) {}
        changed(false);
        return;
    }
    if (id == StripeChart) {
        std::vector<Field> fields{
            {L"Net gelir (USD)", model.stripeRevenue >= 0 ? num(static_cast<long long>(model.stripeRevenue)) : L"2980"},
            {L"Değişim (%)", num(static_cast<long long>(std::lround(model.stripeChange)))}
        };
        if (edit(L"Stripe Grafiği", fields) != 1) return;
        try { model.stripeRevenue = std::max(0.0, std::stod(fields[0].value)); } catch (...) {}
        try { model.stripeChange = std::stod(fields[1].value); } catch (...) {}
        changed(false);
        return;
    }
    if (id == DataFastChart) {
        std::vector<Field> fields{
            {L"Ziyaretçi sayısı", model.dataFastVisitors >= 0 ? num(model.dataFastVisitors) : L"4049"},
            {L"Değişim (%)", num(static_cast<long long>(std::lround(model.dataFastChange)))}
        };
        if (edit(L"DataFast Grafiği", fields) != 1) return;
        try { model.dataFastVisitors = std::max(0LL, std::stoll(fields[0].value)); } catch (...) {}
        try { model.dataFastChange = std::stod(fields[1].value); } catch (...) {}
        changed(false);
        return;
    }
    if (id == DeepSeekBalance) {
        auto pd = resolvePlan(6, model, extra, false);
        std::vector<Field> fields{
            {L"Bakiye (USD)", pd.balance >= 0 ? decimalText(pd.balance, 2) : L""},
            {L"Bugün (USD)", pd.balance >= 0 ? decimalText(pd.todaySpend, 2) : L"0.00"},
            {L"7 gün (USD)", pd.balance >= 0 ? decimalText(pd.weekSpend, 2) : L"0.00"}
        };
        const int action = edit(L"DeepSeek Bakiye", fields, model.deepSeekBalance >= 0);
        if (!action) return;
        if (action == 2) {
            model.deepSeekBalance = -1; model.deepSeekToday = 0; model.deepSeekWeek = 0;
            changed(false);
            return;
        }
        auto parseNum = [](Text t) {
            std::replace(t.begin(), t.end(), L',', L'.');
            return std::stod(t);
        };
        try { model.deepSeekBalance = std::max(0.0, parseNum(fields[0].value)); } catch (...) {}
        try { model.deepSeekToday = std::max(0.0, parseNum(fields[1].value)); } catch (...) {}
        try { model.deepSeekWeek = std::max(0.0, parseNum(fields[2].value)); } catch (...) {}
        changed(false);
        return;
    }
    // Generic AI usage override dialog (with Reset / Clear override button when manual values are set)
    const int plan = planForWidget(id);
    auto& ov = model.aiOverrides[plan];
    const auto cur = resolvePlan(plan, model, extra, false);
    const bool hasOv = ov.p1 >= 0 || ov.p2 >= 0 || ov.p3 >= 0 || !ov.r1.empty() || !ov.r2.empty() || !ov.r3.empty() || !ov.extraLabel.empty();
    std::vector<Field> fields;
    if (id == OpenCodeUsage || id == OpenCodePlanUsage) {
        fields = {
            {L"Son 5s token", cur.sessionTok >= 0 ? num(cur.sessionTok) : L"0"},
            {L"Son 7g token", cur.weeklyTok >= 0 ? num(cur.weeklyTok) : L"0"},
            {L"En çok kullanılan model", cur.modelName}
        };
        const int action = edit(Widgets[id].title, fields, hasOv);
        if (!action) return;
        if (action == 2) {
            ov = {};
            extra.proLocalAt = 0;
            sampleProWidgets(true);
            changed(false);
            return;
        }
        try { extra.aiLive[7].sessionTokens = std::max(0LL, std::stoll(fields[0].value)); extra.aiLive[7].ready = true; extra.aiLive[7].hasTokens = true; } catch (...) {}
        try { extra.aiLive[7].weeklyTokens = std::max(0LL, std::stoll(fields[1].value)); extra.aiLive[7].ready = true; extra.aiLive[7].hasTokens = true; } catch (...) {}
        ov.extraLabel = fields[2].value;
        ov.extraVal = formatCompact(extra.aiLive[7].weeklyTokens >= 0 ? extra.aiLive[7].weeklyTokens : std::max(0LL, cur.weeklyTok));
        changed(false);
        return;
    }
    fields = {
        {id == GeminiUsage ? L"5h kalan (%)" : id == GrokUsage ? L"Kredi kalan (%)" : id == CursorUsage ? L"Auto kalan (%)" : L"Oturum kalan (%)",
         cur.p1 >= 0 ? num(cur.p1) : L""},
        {L"Sıfırlanma / süre", cur.r1 == L"—" ? L"" : cur.r1}
    };
    if (id != GrokUsage && id != ClaudeUsage) {
        fields.push_back({id == GeminiUsage ? L"Weekly kalan (%)" : id == CursorUsage ? L"API kalan (%)" : L"Haftalık kalan (%)",
                          cur.p2 >= 0 ? num(cur.p2) : L""});
        fields.push_back({L"2. Sıfırlanma", cur.r2 == L"—" ? L"" : cur.r2});
    }
    if (id == GlmUsage) {
        fields.push_back({L"MCP kalan (%)", cur.p3 >= 0 ? num(cur.p3) : L""});
    }
    const int action = edit(Widgets[id].title, fields, hasOv);
    if (!action) return;
    if (action == 2) {
        ov = {};
        extra.proLocalAt = 0;
        extra.codexLiveAt = 0;
        sampleProWidgets(true);
        changed(false);
        return;
    }
    if (fields[0].value.empty()) ov.p1 = -1;
    else try { ov.p1 = std::clamp(std::stoi(fields[0].value), 0, 100); } catch (...) {}
    ov.r1 = fields[1].value;
    if (fields.size() >= 4) {
        if (fields[2].value.empty()) ov.p2 = -1;
        else try { ov.p2 = std::clamp(std::stoi(fields[2].value), 0, 100); } catch (...) {}
        ov.r2 = fields[3].value;
    }
    if (fields.size() >= 5) {
        if (fields[4].value.empty()) ov.p3 = -1;
        else try { ov.p3 = std::clamp(std::stoi(fields[4].value), 0, 100); } catch (...) {}
    }
    changed(false);
}

void Dock::proPanel(Palette p) {
    auto& d = sheet.d;
    const int id = page;
    Text subtitle = Widgets[id].description;
    if (id == Temperature) subtitle = L"Canlı termal sensör, CPU ve RAM";
    else if (id == GitHubActivity) subtitle = model.githubUser.empty() ? L"GitHub kullanıcı adı ekle" : (L"github.com/" + model.githubUser);
    else if (id == XProfile) subtitle = model.xHandle.empty() ? L"X hesabı ekle" : (model.xHandle[0] == L'@' ? model.xHandle : L"@" + model.xHandle);
    heading(p, Widgets[id].title, subtitle);

    // Centered live card preview
    const float cardW = std::min(sheet.w - 60.f, std::max(220.f, static_cast<float>(Widgets[id].width)));
    drawProWidget(sheet, id, box((sheet.w - cardW) / 2.f, 104, cardW, Design::CardHeight), p, false, false);

    if (id == AiLimits || id == AiLimitsTriple) {
        const bool triple = id == AiLimitsTriple;
        const std::array currentPair{model.aiLimitFirst, model.aiLimitSecond};
        const std::span<const int> plans = triple ? std::span<const int>(OverviewPlans) : std::span<const int>(currentPair);
        if (!triple) {
            float y = 178.f;
            for (const auto& pair : PlanPairs) {
                const auto [first, second] = pair;
                const bool selected = (model.aiLimitFirst == first && model.aiLimitSecond == second) ||
                    (model.aiLimitFirst == second && model.aiLimitSecond == first);
                const Text label = Text(OverviewNames[first]) + L" + " + OverviewNames[second];
                button(sheet, p, box(29, y, 380, 38), selected ? L"✓  " + label : label, [this, first, second, selected] {
                    if (selected) return;
                    model.aiLimitFirst = first;
                    model.aiLimitSecond = second;
                    sampleProWidgets(true);
                    changed(false);
                }, selected);
                y += 45.f;
            }
            button(sheet, p, box(29, 438, 210, 34), L"Diğer planlar", [this] { editProWidget(AiLimits); });
        }
        const bool overview = triple || (currentPair[0] < std::ssize(OverviewNames) && currentPair[1] < std::ssize(OverviewNames));
        if (overview) {
            const float top = triple ? 178.f : 315.f;
            p.card(d, box(29, top, 380, triple ? 134.f : 105.f), 14);
            d.text(L"Sıfırlanmaya kalan", box(45, top + 7, 178, 24), Caption, tint(p.muted));
            d.text(L"5h", box(226, top + 7, 72, 24), Caption, tint(p.muted), Right);
            d.text(L"Weekly", box(309, top + 7, 84, 24), Caption, tint(p.muted), Right);
            float y = top + 35;
            for (const int plan : plans) {
                const auto pd = resolvePlan(plan, model, extra, false);
                drawBrandLogo(d, plan, box(45, y + 4, 16, 16));
                d.text(OverviewNames[plan], box(70, y, 146, 24), Body, tint(p.ink));
                d.text(pd.p1 < 0 || pd.r1.empty() ? L"—" : pd.r1, box(226, y, 72, 24), Body, tint(p.ink), Right);
                d.text(pd.p2 < 0 || pd.r2.empty() ? L"—" : pd.r2, box(309, y, 84, 24), Body, tint(p.ink), Right);
                y += 28;
            }
        }
        button(sheet, p, box(249, triple ? 338.f : 438.f, 160, 34), L"Şimdi yenile", [this] {
            sampleProWidgets(true);
            paintSheet();
        });
        return;
    }

    // Source / live status block
    p.card(d, box(29, 178, 380, 134), 14);
    if (id == Temperature) {
        const int t = extra.cpuTempC;
        d.text(L"CPU Sıcaklığı", box(45, 194, 160, 22), Body, tint(p.muted));
        d.text(t >= 0 ? (num(t) + L" °C  (" + num(static_cast<int>(std::lround(t * 9.0 / 5.0 + 32.0))) + L" °F)") : L"Sensör bekleniyor",
            box(195, 194, 198, 22), Strong, tint(p.ink), Right);
        d.text(L"İşlemci yükü", box(45, 232, 160, 22), Body, tint(p.muted));
        d.text(cpu >= 0 ? (num(cpu) + L"%") : L"—", box(195, 232, 198, 22), Strong, tint(p.ink), Right);
        d.text(L"Bellek kullanımı", box(45, 270, 160, 22), Body, tint(p.muted));
        d.text(num(ram) + L"%", box(195, 270, 198, 22), Strong, tint(p.ink), Right);
        button(sheet, p, box(29, 338, 184, 34), L"Yenile", [this] { extra.proLocalAt = 0; sample(); sampleProWidgets(true); paintSheet(); }, true);
        button(sheet, p, box(225, 338, 184, 34), L"Görev Yöneticisi", [this] { close(); launch(L"taskmgr.exe"); });
        return;
    }

    Text infoLine, detailLine, detailLine2, metaLine;
    if (id == GitHubActivity) {
        infoLine = model.githubUser.empty() ? L"GitHub kullanıcı adı tanımlı değil"
            : extra.github.ready ? (L"Bugün " + num(extra.github.today) + L" · Hafta " + num(extra.github.week) + L" · Seri " + num(extra.github.streak))
            : extra.github.error ? L"GitHub katkı verisi alınamadı" : L"GitHub katkıları alınıyor…";
        detailLine = L"API anahtarı gerekmez; herkese açık GitHub katkı takvimini okur.";
    } else if (id == StripeChart || id == DataFastChart || id == DeepSeekBalance) {
        const wchar_t* cred = id == DeepSeekBalance ? CredDeepSeek : id == StripeChart ? CredStripe : CredDataFast;
        const bool hasKey = !readCred(cred).empty();
        infoLine = hasKey ? L"API anahtarı Windows Kimlik Bilgisi Yöneticisi’nde kayıtlı"
                          : L"Canlı veri için API anahtarı ekle veya değerleri elle gir";
        detailLine = id == DeepSeekBalance ? L"GET api.deepseek.com/user/balance · 3 dk’da bir yenilenir"
                   : id == StripeChart ? L"GET api.stripe.com/v1/balance_transactions · 3 dk’da bir"
                   : L"GET datafa.st/api/v1/analytics/timeseries · 3 dk’da bir";
    } else if (id == XProfile) {
        infoLine = model.xHandle.empty() ? L"X kullanıcı adını ve takipçi sayını düzenle"
            : L"Hesap: " + (model.xHandle[0] == L'@' ? model.xHandle : L"@" + model.xHandle);
        detailLine = L"Profiline tek tıkla gidebilir veya takipçi sayısını güncelleyebilirsin.";
    } else {
        const int plan = planForWidget(id);
        const auto& live = activeAiPlan(extra, plan);
        const auto pd = resolvePlan(plan, model, extra, false);
        const Text updated = formatUpdatedTime(live.updatedAt);

        if (id == ChatGptUsage) {
            if (pd.p1 >= 0 || pd.p2 >= 0) {
                const Text wReset = formatResetClock(pd.r2At);
                infoLine = L"5 saatlik: " + (pd.p1 >= 0 ? (num(pd.p1) + L"% kalan") : L"—") +
                           L"  ·  Haftalık: " + (pd.p2 >= 0 ? (num(pd.p2) + L"% kalan") : L"—");
                detailLine = !wReset.empty()
                    ? (L"Haftalık limit sıfırlanma: " + wReset + L" (" + pd.r2 + L")")
                    : L"Canlı Codex limitleri otomatik yenilenir (60 sn).";
                detailLine2 = L"Geri sayım ve dolan pencereler her dakika güncellenir.";
                metaLine = (live.sourceLabel.empty() ? L"Kaynak: Codex" : L"Kaynak: " + live.sourceLabel) +
                           (updated.empty() ? L"" : L" · " + updated);
            } else {
                infoLine = L"Codex verisi bekleniyor…";
                detailLine = L"Resmi codex app-server CLI veya ~/.codex/sessions";
                detailLine2 = L"üzerinden otomatik okunur.";
            }
        } else if (id == ClaudeUsage || id == ClaudePlanUsage) {
            if (pd.fromFeedFile && (pd.p1 >= 0 || pd.p2 >= 0)) {
                const Text l1 = pd.l1.empty() ? L"Session" : pd.l1;
                const Text l2 = pd.l2.empty() ? L"Weekly" : pd.l2;
                infoLine = l1 + L": " + (pd.p1 >= 0 ? (num(pd.p1) + L"% kalan") : L"—") +
                           L"  ·  " + l2 + L": " + (pd.p2 >= 0 ? (num(pd.p2) + L"% kalan") : L"—");
                detailLine = L"%LOCALAPPDATA%\\Vetra\\ai_usage.json izleniyor (5 sn).";
                detailLine2 = L"Geri sayım ve dolan pencereler her dakika güncellenir.";
            } else {
                infoLine = pd.hasTokens
                    ? (L"Claude Code · Son 5s: " + formatCompact(pd.sessionTok) + L" · Son 7g: " + formatCompact(pd.weeklyTok))
                    : L"Yerel Claude Code oturumu bulunamadı";
                detailLine = L"Canlı % için %LOCALAPPDATA%\\Vetra\\ai_usage.json yaz.";
                detailLine2 = L"Yerel token kayıtları 5 sn’de bir otomatik yenilenir.";
            }
            metaLine = (live.sourceLabel.empty() ? L"Kaynak: ~/.claude/projects" : L"Kaynak: " + live.sourceLabel) +
                       (updated.empty() ? L"" : L" · " + updated);
        } else if (id == GeminiUsage) {
            if (live.hasLimits && (pd.p1 >= 0 || pd.p2 >= 0)) {
                const Text l1 = pd.l1.empty() ? L"5h" : pd.l1;
                const Text l2 = pd.l2.empty() ? L"Weekly" : pd.l2;
                const Text r1Clk = formatResetClock(pd.r1At);
                const Text r2Clk = formatResetClock(pd.r2At);
                infoLine = l1 + L": " + (pd.p1 >= 0 ? (num(pd.p1) + L"% kalan") : L"—") +
                           L"  ·  " + l2 + L": " + (pd.p2 >= 0 ? (num(pd.p2) + L"% kalan") : L"—");
                detailLine = !r1Clk.empty() ? (l1 + L" sıfırlanma: " + r1Clk + L" (" + pd.r1 + L")")
                           : !r2Clk.empty() ? (l2 + L" sıfırlanma: " + r2Clk + L" (" + pd.r2 + L")")
                           : (pd.fromFeedFile ? L"ai_usage.json 5 sn’de bir okunur." : L"Antigravity CLI limitleri 60 sn’de bir yenilenir.");
                detailLine2 = (!r1Clk.empty() && !r2Clk.empty())
                    ? (l2 + L" sıfırlanma: " + r2Clk + L" (" + pd.r2 + L")")
                    : L"Dock otomatik yenilenir.";
            } else {
                infoLine = pd.hasTokens
                    ? (L"Gemini CLI (24s) · Pro: " + formatCompact(pd.sessionTok) + L" · Flash: " + formatCompact(pd.secondaryTok))
                    : pd.hasOverride ? (L"Elle ayarlandı · 5h: " + (pd.p1 >= 0 ? num(pd.p1) + L"%" : L"—") +
                                        L" · Weekly: " + (pd.p2 >= 0 ? num(pd.p2) + L"%" : L"—"))
                    : L"Antigravity kota verisi bekleniyor";
                detailLine = L"Canlı kota için agy CLI’da Google hesabına giriş yap.";
                detailLine2 = L"JSON beslemesi ve elle girilen değerler de desteklenir.";
            }
            metaLine = (live.sourceLabel.empty() ? L"Kaynak: Antigravity CLI" : L"Kaynak: " + live.sourceLabel) +
                       (updated.empty() ? L"" : L" · " + updated);
        } else if (id == OpenCodeUsage || id == OpenCodePlanUsage) {
            infoLine = pd.hasTokens
                ? (L"Son 5s: " + formatCompact(pd.sessionTok) + L" · Son 7g: " + formatCompact(pd.weeklyTok))
                : L"Yerel OpenCode veritabanı bulunamadı";
            detailLine = L"opencode.db ve ai_usage.json 5 sn’de bir otomatik yenilenir.";
            metaLine = (live.sourceLabel.empty() ? L"Kaynak: opencode.db" : L"Kaynak: " + live.sourceLabel) +
                       (updated.empty() ? L"" : L" · " + updated);
        } else if (id == CursorUsage) {
            if (pd.fromFeedFile && (pd.p1 >= 0 || pd.p2 >= 0)) {
                const Text l1 = pd.l1.empty() ? L"Auto" : pd.l1;
                const Text l2 = pd.l2.empty() ? L"API" : pd.l2;
                infoLine = l1 + L": " + (pd.p1 >= 0 ? (num(pd.p1) + L"% kalan") : L"—") +
                           L"  ·  " + l2 + L": " + (pd.p2 >= 0 ? (num(pd.p2) + L"% kalan") : L"—");
                detailLine = L"%LOCALAPPDATA%\\Vetra\\ai_usage.json izleniyor (5 sn).";
                metaLine = L"Kaynak: ai_usage.json" + (updated.empty() ? L"" : L" · " + updated);
            } else {
                infoLine = pd.hasOverride
                    ? (L"Elle ayarlandı · Auto: " + (pd.p1 >= 0 ? num(pd.p1) + L"%" : L"—") +
                       L" · API: " + (pd.p2 >= 0 ? num(pd.p2) + L"%" : L"—"))
                    : L"Yerel JSON veri dosyası bekleniyor";
                detailLine = L"Canlı besleme: %LOCALAPPDATA%\\Vetra\\ai_usage.json";
                detailLine2 = L"Dosya her 5 sn’de bir okunur veya elle güncelleyebilirsin.";
            }
        } else {
            infoLine = pd.fromFeedFile ? L"ai_usage.json üzerinden canlı okunuyor"
                     : pd.hasOverride ? L"Elle ayarlanan kalan limit gösteriliyor"
                     : L"Yerel JSON veri dosyası bekleniyor";
            detailLine = L"Canlı besleme: %LOCALAPPDATA%\\Vetra\\ai_usage.json (5 sn).";
            metaLine = (live.sourceLabel.empty() ? L"" : L"Kaynak: " + live.sourceLabel) +
                       (updated.empty() ? L"" : L" · " + updated);
        }
    }
    d.text(infoLine, box(45, 190, 348, 24), Strong, tint(p.ink));
    d.text(detailLine, box(45, 222, 348, 20), Caption, tint(p.muted));
    if (!detailLine2.empty()) d.text(detailLine2, box(45, 244, 348, 20), Caption, tint(p.muted));
    if (!metaLine.empty()) d.text(metaLine, box(45, 278, 348, 20), Micro, tint(p.muted));

    if (id == StripeChart || id == DataFastChart || id == DeepSeekBalance) {
        button(sheet, p, box(29, 338, 128, 34), L"API anahtarı", [this, id] { editProKey(id); }, true);
        button(sheet, p, box(166, 338, 128, 34), L"Değer düzenle", [this, id] { editProWidget(id); });
        button(sheet, p, box(303, 338, 106, 34), L"Yenile", [this] { extra.proWebAt = 0; sampleProWidgets(true); paintSheet(); });
    } else if (id == GitHubActivity) {
        button(sheet, p, box(29, 338, 150, 34), L"Kullanıcı adı", [this, id] { editProWidget(id); }, true);
        button(sheet, p, box(188, 338, 106, 34), L"Yenile", [this] { extra.proWebAt = 0; sampleProWidgets(true); paintSheet(); });
        button(sheet, p, box(303, 338, 106, 34), L"GitHub’ı aç", [this] {
            launch(model.githubUser.empty() ? L"https://github.com" : L"https://github.com/" + model.githubUser);
        });
    } else if (id == XProfile) {
        button(sheet, p, box(29, 338, 184, 34), L"Profili düzenle", [this, id] { editProWidget(id); }, true);
        button(sheet, p, box(225, 338, 184, 34), L"X profilini aç", [this] {
            Text h = model.xHandle;
            while (!h.empty() && h[0] == L'@') h.erase(h.begin());
            launch(h.empty() ? L"https://x.com" : L"https://x.com/" + h);
        });
    } else if (id == ChatGptUsage || id == OpenCodeUsage || id == OpenCodePlanUsage) {
        button(sheet, p, box(29, 338, 210, 34), L"Değerleri düzenle",
            [this, id] { editProWidget(id); }, true);
        button(sheet, p, box(249, 338, 160, 34), L"Şimdi yenile", [this] {
            extra.proLocalAt = 0; extra.codexLiveAt = 0; extra.proWebAt = 0; sampleProWidgets(true); paintSheet();
        });
    } else {
        button(sheet, p, box(29, 338, 142, 34), L"Değer düzenle", [this, id] { editProWidget(id); }, true);
        button(sheet, p, box(179, 338, 126, 34), L"JSON dosyası", [this] { openAiFeedInExplorer(panel ? panel : hwnd); });
        button(sheet, p, box(313, 338, 96, 34), L"Yenile", [this] {
            extra.proLocalAt = 0; extra.codexLiveAt = 0; extra.proWebAt = 0; sampleProWidgets(true); paintSheet();
        });
    }
}
