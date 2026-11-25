#pragma once
#include "model.h"
#include <atomic>
#include <ctime>
#include <future>
#include <memory>
#include <optional>

namespace Gdiplus { class Bitmap; }

struct Disk { Text path; ULONGLONG free, total; };
struct TrayItem {
    Text name;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    HWND targetHwnd{};
    UINT uid = 0;
    UINT callbackMsg = 0;
};
std::unique_ptr<Gdiplus::Bitmap> normalizeAppIcon(Gdiplus::Bitmap& src);
std::unique_ptr<Gdiplus::Bitmap> highResIconForPath(const wchar_t* path);
std::shared_ptr<Gdiplus::Bitmap> resolveTrayIconBitmap(const Text& name, HICON vetraIcon = nullptr);

struct TrayMenuItem {
    Text label;
    Text rawName;
    Text launchTarget;
    UINT cmdId = 0;
    int rawMenuIndex = -1;
    std::vector<int> parentPath;
    std::vector<Text> parentLabels;
    bool separator = false;
    bool disabled = false;
    bool checked = false;
    bool isDefault = false;
    bool hasSubmenu = false;
    std::vector<TrayMenuItem> children;
};
struct JumpItem {
    Text label;
    Text targetPath;
    Text args;
    Text workingDir;
    Text aumid;
    std::vector<BYTE> pidlBytes;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    bool isFolder = false;
};
struct JumpCategory {
    Text title;
    bool isTasks = false;
    std::vector<JumpItem> items;
};
struct JumpListData {
    std::vector<JumpCategory> categories;
    bool empty() const {
        for (const auto& c : categories) if (!c.items.empty()) return false;
        return true;
    }
};
bool isChromeApp(const Shortcut& app);

struct OpenWindow {
    HWND handle{};
    Text title;
    std::shared_ptr<Gdiplus::Bitmap> icon;
    bool active = false;
    bool iconChecked = false;
    Shortcut app;
    uint64_t iconRequestId = 0;
};
using WindowIconResult = std::pair<uint64_t, std::shared_ptr<Gdiplus::Bitmap>>;
enum class QuickSubPanel { Main, Wifi, Bluetooth, Project, Accessibility, NearbyShare, AudioOutput };
struct WifiNetwork {
    GUID adapter;
    Text name, profile;
    int signal;
    bool connected, secure;
    std::string ssid;
    unsigned authentication = 0, cipher = 0, bss = 0;
};
struct WifiScanResult {
    std::vector<WifiNetwork> available, saved;
    DWORD error = 0;
    bool connected = false;
    Text connectedName;
    GUID connectedAdapter{};
};
struct BluetoothDeviceInfo {
    Text id;
    Text name;
    ULONGLONG address = 0;
    GUID containerId{};
    bool hasContainer = false;
    bool connected = false;
    bool isBle = false;
    bool isAudio = false;
    int battery = -1;
};
struct BtActionResult {
    Text message;
    ULONGLONG address = 0;
    bool connected = false;
    bool audioChanged = false;
};
struct AudioEndpointInfo {
    Text id;
    Text name;
    bool active = false;
};
struct AccessibilityState {
    bool magnifier = false;
    bool colorFilter = false;
    bool narrator = false;
    bool liveCaptions = false;
    bool stickyKeys = false;
    bool highContrast = false;
    bool monoAudio = false;
};
struct RadioStatus {
    int wifi = -1, bluetooth = -1, hotspot = -1, hotspotClients = 0;
    bool airplane = false;
    Text network;
    GUID adapter{};
    bool denied = false, ethernet = false, hasInternet = false;
};
struct KeyboardLayoutInfo {
    HKL hkl{};
    Text shortCode;
    Text subCode;
    Text languageName;
    Text layoutName;
    bool active = false;
};
struct WinNotification {
    int64_t id = 0;
    int64_t arrivalTime = 0;
    Text aumid;
    Text appName;
    Text title;
    Text body;
    Text timeLabel;
    Text launchUri;
    std::shared_ptr<Gdiplus::Bitmap> icon;
};
struct WeatherDay { int day, high, low, code; Text sunrise, sunset; };
struct WeatherHour { Text time; int temp, code; bool daylight; };
struct Forecast {
    int city = -1, current = 0, feels = 0, code = 0, wind = 0;
    double precipitation = 0;
    bool daylight = true;
    Text time;
    std::vector<WeatherDay> days;
    std::vector<WeatherHour> hours;
};
struct Quote {
    double price = 0, change = 0;
    Text currency;
    std::vector<double> trend;
};
struct MarketResult {
    std::array<std::optional<Quote>, 5> quotes;
    unsigned requested = 0;
};
struct MediaState {
    Text source, title, artist;
    bool playing = false;
    bool canPrev = false, canNext = false, canSeek = false;
    std::shared_ptr<Gdiplus::Bitmap> cover;
    int64_t elapsedTicks = 0, durationTicks = 0;
    ULONGLONG timelineStamp = 0;
    float progress(ULONGLONG now) const {
        if (durationTicks <= 0) return -1.f;
        const double elapsed = static_cast<double>(elapsedTicks) +
            (playing && timelineStamp && now > timelineStamp ? (now - timelineStamp) * 10000.0 : 0.0);
        return static_cast<float>(std::clamp(elapsed / durationTicks, 0.0, 1.0));
    }
};
struct AiPlanLive {
    bool ready = false;
    bool hasLimits = false;
    bool hasTokens = false;
    bool fromFeedFile = false;
    int p1 = -1, p2 = -1, p3 = -1;
    time_t r1At = 0, r2At = 0, r3At = 0;
    int r1WinMins = 0, r2WinMins = 0;
    Text l1, l2, l3;
    Text r1, r2, r3, modelName, modelTokens;
    Text sourceLabel, planType;
    long long sessionTokens = -1, weeklyTokens = -1, secondaryTokens = -1;
    double balance = -1, todaySpend = 0, weekSpend = 0;
    time_t updatedAt = 0;
};
struct GitHubLive {
    bool ready = false, error = false;
    int today = 0, week = 0, streak = 0;
    std::array<uint8_t, 42> grid{};
};
struct AnalyticsLive {
    bool ready = false, error = false;
    double value = 0, changePct = 0;
    std::vector<double> trend;
};
struct ProLocalSnapshot {
    int cpuTempC = -1;
    std::array<AiPlanLive, AiPlanCount> ai;
};
struct ProWebSnapshot {
    unsigned mask = 0;
    GitHubLive github;
    AnalyticsLive stripe, dataFast;
    AiPlanLive deepSeek;
    long long xFollowers = -1;
};
struct SessionTimer {
    enum class Status { Ready, Running, Paused, Finished };
    Status status = Status::Ready;
    ULONGLONG durationMs = 0, leftMs = 0, deadline = 0;
    int seconds = 0;
    bool running() const { return status == Status::Running; }
    bool active() const { return running() || status == Status::Paused; }
    float remaining(ULONGLONG stamp) const {
        const auto timeLeft = running() ? (deadline > stamp ? deadline - stamp : 0) : leftMs;
        return durationMs ? std::clamp(static_cast<float>(timeLeft) / durationMs, 0.f, 1.f) : 0.f;
    }
    bool update(ULONGLONG stamp);
    void reset(int minutes);
};
struct ExtraWidgets {
    struct ExchangeRate { int from = -1, to = -1; double rate = 0; Text date; };
    std::future<ExchangeRate> exchangeRequest;
    ExchangeRate exchange;
    ULONGLONG exchangeAt = 0;
    ULONGLONG watchStart = 0, watchAccum = 0;
    int volume = -1, usageTotal = 0;
    int microphoneMute = -1;
    Text micDeviceName;
    std::vector<Text> micActiveApps;
    std::vector<AudioEndpointInfo> micDevices;
    std::vector<KeyboardLayoutInfo> keyboardLayouts;
    HKL activeKeyboardHkl{};
    Text keyboardShort, keyboardSub;
    bool netEthernetFast = false, netWifiFast = false;
    ULONGLONG lastRadioPollAt = 0, lastStatusFastAt = 0, lastNotifPollAt = 0;
    std::vector<WinNotification> notifications;
    std::future<std::vector<WinNotification>> notifRequest;
    std::vector<int64_t> dismissedNotifIds;
    int64_t clearedNotifBefore = 0;
    bool dndEnabled = false;
    bool notifCalExpanded = true;
    Text outputName;
    ULONGLONG mediaProgressPaintAt = 0;
    bool muted = false, plugged = false, usageDirty = false;
    int batteryMinutes = -1;
    ULONGLONG lastUsage = 0, usageSaved = 0;
    std::vector<ULONGLONG> laps;
    std::vector<Disk> disks;
    std::vector<OpenWindow> windows;
    std::future<std::vector<WindowIconResult>> windowIconsRequest;
    uint64_t nextWindowIconId = 1;
    std::vector<WifiNetwork> wifi, savedWifi;
    std::future<WifiScanResult> wifiRequest;
    DWORD wifiError = 0;
    bool wifiConnected = false, wifiScanning = false, wifiShowSaved = false;
    QuickSubPanel quickSub = QuickSubPanel::Main;
    RadioStatus radios;
    std::future<RadioStatus> radioRequest;
    bool airplanePrevWifi = true, airplanePrevBt = true;
    std::vector<BluetoothDeviceInfo> btDevices;
    std::future<std::vector<BluetoothDeviceInfo>> btRequest;
    std::future<BtActionResult> btActionRequest;
    bool btLoading = false;
    bool btBusyConnect = false;
    ULONGLONG btBusyAddress = 0;
    std::vector<AudioEndpointInfo> audioDevices;
    UINT32 displayTopology = 1;
    bool energySaver = false;
    GUID prevPowerOverlay{};
    bool hasPrevPowerOverlay = false;
    DWORD prevDcSaverThreshold = 20;
    bool hasPrevDcThreshold = false;
    bool nightLight = false;
    int nearbyShare = 0;
    AccessibilityState accessibility{};
    MediaState media;
    std::future<MediaState> mediaRequest;
    std::future<bool> mediaActionRequest;
    ULONGLONG mediaRefreshAt = 0;
    ULONGLONG wifiUpdateAt = 0, wifiUpdateUntil = 0;
    std::array<long long, 7> download{}, upload{};
    ULONGLONG netAt = 0, in = 0, out = 0, lastDisks = 0;
    bool networkReady = false;
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    int zoneCity = -1;
    bool zoneFound = false;
    std::optional<Forecast> forecast;
    std::future<std::optional<Forecast>> weatherRequest;
    ULONGLONG weatherAt = 0;
    int requestedCity = -1;
    bool weatherError = false;
    std::array<std::optional<Quote>, 5> quotes;
    std::array<ULONGLONG, 3> marketAt{};
    std::future<MarketResult> marketRequest;
    unsigned marketErrors = 0;
    int cpuTempC = -1;
    std::array<AiPlanLive, AiPlanCount> aiLive;
    GitHubLive github;
    AnalyticsLive stripe, dataFast;
    long long xFollowersLive = -1;
    std::future<ProLocalSnapshot> proLocalRequest;
    std::future<ProWebSnapshot> proWebRequest;
    std::future<AiPlanLive> codexLiveRequest;
    std::future<AiPlanLive> antigravityRequest;
    AiPlanLive antigravityLive;
    ULONGLONG antigravityAt = 0;
    ULONGLONG proLocalAt = 0, proWebAt = 0, codexLiveAt = 0;
    std::shared_ptr<std::atomic<bool>> proStop = std::make_shared<std::atomic<bool>>(false);
    ULONGLONG elapsed(ULONGLONG stamp) const { return watchAccum + (watchStart ? stamp - watchStart : 0); }
};
Text durationText(ULONGLONG seconds);
Text rateText(long long bytes);
Text spaceText(ULONGLONG bytes);
Text usageText(int seconds);
Text weatherText(int code);
Text decimalText(double value, int decimals = 2);
std::optional<Forecast> readWeather(int city);
Text appKey(const Shortcut& app);
Shortcut windowApp(HWND window, bool resolveLabel = true);
std::shared_ptr<Gdiplus::Bitmap> appImage(const Text& target);
std::shared_ptr<Gdiplus::Bitmap> iconFromLocation(const Text& iconPath, int iconIndex);
JumpListData readAppJumpList(const Shortcut& app, const std::vector<HWND>& appWindows);
void prewarmJumpListIndex();
int readCpuUtilityPct();
bool setDefaultAudioEndpointId(const Text& deviceId);
