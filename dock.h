#pragma once
#include "canvas.h"
#include "design.h"
#include "shell_taskbar.h"
#include "widgets.h"
#include <cmath>
#include <functional>
#include <future>
#include <memory>
#include <map>

inline constexpr auto DockClass = L"Vetra.Window";
inline constexpr auto MonitorDockClass = L"Vetra.MonitorDock";
inline constexpr auto PanelClass = L"Vetra.Panel";
inline constexpr UINT TrayMessage = WM_APP + 1, ShowStudio = WM_APP + 2, AsyncWake = WM_APP + 3,
    StartupDeferred = WM_APP + 4, LaunchFailedMsg = WM_APP + 5;
inline constexpr UINT_PTR SessionTimerTick = 12;
inline constexpr UINT_PTR ChromePreviewTick = 13;
inline constexpr UINT_PTR ChromeHoverTick = 14;
inline constexpr UINT_PTR InstalledAppsTick = 15;
inline constexpr UINT_PTR WindowSyncTick = 16;
inline constexpr UINT_PTR WarmupTick = 17;

double animNowMs();
void releaseAudioEndpoint();

inline float easeOutQuint(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float u = 1.f - t;
    return 1.f - u * u * u * u * u;
}
inline float easeOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    const float u = 1.f - t;
    return 1.f - u * u * u;
}
inline float easeInCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * t;
}
inline float easeInOutCubic(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t < 0.5f ? 4.f * t * t * t : 1.f - std::pow(-2.f * t + 2.f, 3.f) / 2.f;
}
inline float easeOutBack(float t) {
    t = std::clamp(t, 0.f, 1.f);
    constexpr float c1 = 1.35f, c3 = c1 + 1.f;
    const float u = t - 1.f;
    return 1.f + c3 * u * u * u + c1 * u * u;
}

// Analytical critically damped spring (Apple Fluid Interfaces / WWDC 2023 formulation).
struct Spring {
    float value = 0.f, target = 0.f, velocity = 0.f;
    float response = 0.22f;
    bool initialized = false;

    void snap(float v) { value = target = v; velocity = 0.f; initialized = true; }
    void setTarget(float t, bool smooth = true, float eps = 0.0005f) {
        if (!initialized || !smooth) { snap(t); return; }
        if (std::abs(target - t) > eps * 0.1f) target = t;
    }
    bool step(float dt, float epsPos = 0.0008f, float epsVel = 0.008f) {
        if (!initialized) { snap(target); return false; }
        const float dx = value - target;
        if (std::abs(dx) < epsPos && std::abs(velocity) < epsVel) {
            value = target; velocity = 0.f; return false;
        }
        const float omega = 6.f / std::max(0.03f, response);
        const float e = std::exp(-omega * dt);
        const float c2 = velocity + omega * dx;
        value = target + (dx + c2 * dt) * e;
        velocity = (velocity - omega * c2 * dt) * e;
        if (std::abs(value - target) < epsPos && std::abs(velocity) < epsVel) {
            value = target; velocity = 0.f; return false;
        }
        return true;
    }
    bool active(float epsPos = 0.0008f, float epsVel = 0.008f) const {
        return initialized && (std::abs(value - target) >= epsPos || std::abs(velocity) >= epsVel);
    }
};

struct WindowPreview {
    HWND window{};
    Text title;
    bool active = false, minimized = false, requested = false;
    std::shared_ptr<Gdiplus::Bitmap> icon, preview;
};

enum class OrderKind { None, Widget, Shortcut, Window };
struct OrderItem {
    OrderKind kind = OrderKind::None;
    int index = -1;
    HWND window{};
    explicit operator bool() const { return kind != OrderKind::None; }
    bool operator==(const OrderItem&) const = default;
};
struct LiveDockItem {
    OrderKind kind = OrderKind::None;
    int widget = -1;
    Text appKey;
    HWND window{};
    bool chromeGroup = false;
};
struct PlacedDockItem {
    OrderItem item;
    float x = 0.f, span = 0.f;
};
struct DockAnimPos {
    LiveDockItem key;
    float x = 0.f, vel = 0.f;
};
struct Hit { Box bounds; std::function<void()> action, secondary; bool chromePreview = false; OrderItem order{}; };
struct View {
    HWND hwnd{};
    Draw d;
    float scale = 1, w = 0, h = 0;
    int hover = -1, key = -1, pressed = -1;
    bool tracking = false;
    std::vector<float> hoverVals, pressVals;
    float hoverAmount() const;
    float pressAmount() const;
    void resetMotion();
    void swap(View& other) noexcept;
    std::vector<Hit> hits;
    uint64_t placementRevision = 0;
    std::vector<PlacedDockItem> placement;
    int at(int x, int y, bool secondary = false) const;
};
struct Palette {
    AppearanceTheme theme;
    bool light, glass;
    unsigned accent, action, activeInk;
    unsigned ink, muted, base, high, line, rail;
    int opacity;
    explicit Palette(const Model& model) : Palette(model.theme, model.opacity, model.accent) {}
    Palette(AppearanceTheme theme, int opacity, int accentChoice);
    void surface(Draw& draw, Box bounds, float radius, bool panel = true) const;
    void card(Draw& draw, Box bounds, float radius = 16) const;
};
struct Field {
    Text label, value;
    bool multiline = false;
    std::vector<Text> choices;
    bool password = false;
};

class Dock {
public:
    HINSTANCE instance;
    HWND hwnd{}, panel{};
    Model model;
    ExtraWidgets extra;
    View bar, sheet;
    ShellTaskbar shellTaskbar;
    std::map<HMONITOR, ScreenReservation> reservations;
    enum class NativeFlyout { None, Quick };
    NativeFlyout nativeFlyout = NativeFlyout::None;
    bool nativeBarRevealed = false, nativeQuickOpen = false, volumeDrag = false;
    Box barVolumeCard{}, barVolumeTrack{}, quickVolumeTrack{}, opacitySliderTrack{};
    Box statusVolumeHit{}, statusKeyboardHit{};
    HWND lastExternalFg{};
    bool barVolumeDrag = false, barVolumeAnimating = false, opacitySliderDrag = false;
    float barVolumeLevel = -1.f, barVolumeTarget = -1.f, barVolumeSent = -1.f;
    double barVolumeTickMs = 0.0, barVolumeCommitMs = 0.0, barVolumePaintMs = 0.0, opacityPaintMs = 0.0;
    int nativeFlyoutTries = 0;
    enum class TrayAction { None, Capture, LeftClick, RightClickMenu, ExecuteMenu };
    TrayAction trayAction = TrayAction::None;
    struct TrayWindowSnap {
        HWND hwnd{};
        bool visible = false;
        RECT rect{};
    };
    std::vector<TrayItem> trayIcons;
    std::vector<std::pair<Box, int>> trayRows;
    std::vector<TrayMenuItem> trayMenuItems;
    std::vector<size_t> trayMenuPath;
    int trayMenuIconIndex = -1;
    int trayPendingMenuIndex = -1;
    ULONGLONG trayRightClickStartedAt = 0;
    bool trayMenuLoading = false, trayMenuUnavailable = false;
    int trayActionIndex = -1, trayCaptureTries = 0, trayPopupSeenTries = 0;
    Text trayActionName;
    POINT trayActionPoint{}, trayLocalOffset{}, trayNativeClick{};
    RECT trayLastOvRect{};
    HWND trayOverflow{};
    HWND trayCapturedMenuHwnd{};
    HWND trayLastPopupHwnd{};
    HWND trayPreForeground{};
    DWORD trayMenuOwnerPid = 0;
    std::vector<TrayWindowSnap> trayPreWindows;
    std::vector<HWND> trayIgnoredPopups;
    TrayMenuItem trayPendingExec;
    bool trayLoading = false, trayNeedsReveal = false, trayOverflowSettled = false, trayRightClickPending = false;
    int trayRightClickStep = 0;
    HWINEVENTHOOK trayHook{};
    struct TrayCaptureResult {
        uint64_t generation = 0;
        bool ok = false;
        bool revealedTaskbar = false;
        std::vector<TrayItem> items;
    };
    std::future<TrayCaptureResult> trayCaptureRequest;
    uint64_t trayCaptureGen = 0;
    std::shared_ptr<std::atomic<bool>> trayCaptureCancel;
    HWINEVENTHOOK windowHook{};
    bool windowSyncScheduled = false;
    bool pendingPosition = false;
    bool startupCompleted = false;
    bool trayWarmupDone = false;
    int quickPage = 0, positionedQuickPage = 0;
    SYSTEMTIME catalogTime = now();
    RECT sheetDestination{};
    POINT sheetFrom{};
    double sheetMotionMs = 0.0;
    float sheetFromAlpha = 0.f;
    float sheetAnimDurationMs = Design::SheetOpenMs;
    bool sheetClosing = false;
    double sheetContentMs = 0.0;
    float sheetContentDx = 0.f, sheetContentDy = 0.f;
    int positionedPage = -1, positionedTab = -1;
    float sheetTail = 0;
    int switchHit = -1;
    bool switchWasOn = false;
    double switchAtMs = 0.0;
    int page = -1, tab = 0, listOffset = 0, catalogGroup = 0, catalogPage = 0, orderPage = 0;
    std::vector<WindowPreview> chromeWindows;
    std::future<std::vector<WindowPreview>> chromePreviewRead;
    ULONGLONG chromeHoverSince = 0, chromeLeftAt = 0;
    bool chromeHoverPanel = false, chromeHoverTracking = false, chromeIconHovered = false;
    int dragSource = -1, dragTarget = -1;
    OrderItem dockOrderSource;
    int dockDropIndex = -1;
    POINT dockOrderOrigin{};
    bool dockOrderDragging = false;
    std::vector<DockAnimPos> dockVisualX;
    LiveDockItem dockSettlingKey;
    float dockDragGrabX = 0.f, dockDragX = 0.f, dockDragTargetX = 0.f, dockDragVel = 0.f, dockDragLift = 0.f;
    Spring dockDragLiftSpring{0.f, 0.f, 0.f, 0.14f, true};
    double dockAnimTickMs = 0.0;
    bool dockAnimating = false;
    std::array<std::vector<LiveDockItem>, ProfileCount> liveOrder;
    std::vector<OrderItem> cachedDockItems;
    bool dockItemsDirty = true;
    uint64_t dockRevision = 1, pinnedWindowRevision = 0;
    struct PinnedDockWindow { HWND target{}; bool active = false; };
    std::vector<PinnedDockWindow> pinnedWindows;
    Text catalogSearch;
    bool searchingCatalog = false;
    std::vector<std::pair<Box, int>> orderRows;
    Date selected = Date::from(now()), month = selected;
    std::vector<HMONITOR> monitors;
    struct MonitorDock {
        HMONITOR monitor{};
        View view;
        RECT work{}, bounds{};
        POINT origin{};
        std::vector<OrderItem> folded;
        bool hidden = false;
        ULONGLONG pointerLeftAt = 0;
        bool fullscreen = false;
    };
    HMONITOR activeMonitor{};
    std::vector<std::unique_ptr<MonitorDock>> otherDocks;
    std::vector<OrderItem> folded;
    RECT work{}, monitorRect{};
    float panelDockX = -1;
    bool hidden = false, fullscreenActive = false, modal = false, boot = false, cityPicker = false, dockEditMode = false, openingSheet = false;
    HICON appIcon{};
    struct IndexedBitmap {
        Text key;
        std::shared_ptr<Gdiplus::Bitmap> bitmap;
    };
    std::vector<std::shared_ptr<Gdiplus::Bitmap>> appImages;
    std::vector<bool> appImageChecked;
    std::future<std::vector<IndexedBitmap>> appImagesRequest;
    std::vector<Shortcut> installedApps, recentFiles;
    std::vector<std::shared_ptr<Gdiplus::Bitmap>> installedImages;
    std::future<std::vector<IndexedBitmap>> installedImagesRequest;
    std::vector<Text> installedImageRequested;
    std::future<std::vector<Shortcut>> installedRequest;
    ULONGLONG installedAt = 0;
    Shortcut contextApp;
    HWND contextWindow{};
    std::vector<HWND> contextWindows;
    std::shared_ptr<Gdiplus::Bitmap> contextIcon;
    JumpListData contextJumpList;
    struct JumpListResult {
        uint64_t generation = 0;
        Text cacheKey;
        JumpListData data;
    };
    uint64_t jumpListGen = 0;
    bool contextJumpLoading = false;
    Text toolInput;
    int shortcutOffset = 0;
    bool typingTool = false;
    LONGLONG recycleItems = -1;
    double widgetMotionMs = 0.0;
    HWND captureWindow{};
    int captureMode = -1;
    UINT trayRestore{};
    Text notice, branch;
    ULONGLONG noticeUntil = 0, slowTick = 0;
    double noticeAtMs = 0.0;
    SessionTimer focusTimer, countdownTimer;
    std::optional<Text> timerInput;
    bool selectTimerInput = false, invalidTimerInput = false;
    int sessions = 0, cpu = -1, ram = 0, battery = -1;
    bool focusBreak = false;
    int focusRound = 1;
    struct TimerRimTransition { float from = 0.f; double start = 0.0; };
    std::array<TimerRimTransition, 3> timerRims{};
    struct WhatsAppFeed;
    std::shared_ptr<WhatsAppFeed> whatsAppFeed;
    int lastMinute = -1, lastSecond = -1, lastDay = -1;
    ULONGLONG lastIdle = 0, lastTotal = 0;
    double flipClockAtMs = 0.0, segmentClockAtMs = 0.0, analogSecAtMs = 0.0, usageAnimMs = 0.0;
    int analogPrevSec = -1;
    std::array<int, 4> prevFlipDigits{-1, -1, -1, -1}, curFlipDigits{-1, -1, -1, -1};
    std::array<int, 4> prevSegDigits{-1, -1, -1, -1}, curSegDigits{-1, -1, -1, -1};

    bool cachedMotionEnabled = true, animTimerActive = false;
    HANDLE animationTimer{};
    HMONITOR animationMonitor{};
    UINT animIntervalMs = 16;
    double animationPeriodMs = 1000.0 / 60.0;
    double lastAnimTickMs = 0.0, nextAnimTickMs = 0.0;
    ULONGLONG lastFullscreenScan = 0;
    HWND lastFullscreenFg{};
    UINT visibilityInterval = 0;
    std::vector<RECT> fullscreenFrames;
    Spring dockVisSpring{1.f, 1.f, 0.f, 0.20f, true};
    Spring barWaveStrength{0.f, 0.f, 0.f, 0.16f, true};
    Spring barCursorSmoothX{0.f, 0.f, 0.f, 0.075f, false};
    Spring dockEditSpring{0.f, 0.f, 0.f, 0.18f, true};
    Spring cpuAnim{0.f, 0.f, 0.f, 0.20f, false};
    Spring ramAnim{0.f, 0.f, 0.f, 0.20f, false};
    Spring batteryAnim{0.f, 0.f, 0.f, 0.20f, false};
    Spring taskAnim{0.f, 0.f, 0.f, 0.26f, false};
    Spring opacityAnim{50.f, 50.f, 0.f, 0.16f, false};
    Spring studioNavY{0.f, 0.f, 0.f, 0.16f, false};
    Spring catalogNavY{0.f, 0.f, 0.f, 0.16f, false};
    Spring catalogScrollY{0.f, 0.f, 0.f, 0.18f, true};
    Spring menuHoverY{0.f, 0.f, 0.f, 0.13f, false};
    bool menuHoverVisible = false;
    Text appBounceKey;
    double appBounceAtMs = 0.0;
    struct AppIndicator { LiveDockItem key; float value, target; };
    std::vector<AppIndicator> appActiveWidth;
    HCURSOR curArrow{}, curHand{}, curSizeNS{}, curSizeWE{}, curSizeAll{};

    explicit Dock(HINSTANCE inst) : instance(inst) {}
    ~Dock() {
        if (extra.proStop) extra.proStop->store(true);
        if (trayCaptureCancel) trayCaptureCancel->store(true);
        stopWindowHook();
        stopTraySync();
    }
    bool initialize();
    LRESULT message(HWND window, UINT msg, WPARAM wp, LPARAM lp);
    void tray(bool add);
    void menu();
    void refreshMonitors();
    void syncMonitorDocks();
    void destroyMonitorDocks();
    bool selectDockWindow(HWND window, bool force = false);
    bool anyDockVisible() const;
    bool layoutMonitorDock(HMONITOR monitor, View& view, RECT& area, RECT& bounds, POINT& origin,
        std::vector<OrderItem>& overflow);
    void updateOtherDockVisibility(ULONGLONG stamp, const POINT& cursor,
        const RECT& foregroundFrame, const POINT* normalForegroundCenter);
    void position();
    void updateVisibility();
    void startMenu();
    void beginDockDragFromCursor();
    void endDockDrag(bool commit);
    void ensureDockOrder(int p);
    void invalidateDockItems() { dockItemsDirty = true; ++dockRevision; }
    const std::vector<OrderItem>& dockItems();
    bool isFoldedWidget(int id) const;
    float dockItemSpan(OrderItem item) const;
    float dockGapBetween(OrderItem prev, OrderItem curr) const;
    LiveDockItem dockItemKey(OrderItem item) const;
    std::vector<PlacedDockItem> layoutDockItems(const std::vector<OrderItem>& items) const;
    int dockDropAt(int x, int y) const;
    void startDockDragAnim(int cursorX, int cursorY);
    void advanceDockDrag(double nowMs);
    void finishDockDragAnim();
    bool findVisualX(const LiveDockItem& key, float& outX) const;
    bool reorderDockSlot(OrderItem source, int destIndex);
    bool reorderDockItems(OrderItem source, OrderItem target);
    void persistLiveOrder();
    ULONGLONG pointerLeftAt = 0;
    POINT dockOrigin{};
    POINT dockDragOffset{};
    POINT dockIconDragOrigin{};
    bool dockDragging = false, dockIconDragPending = false;
    bool usageWeek = false;
    float dockExposure = 1.f, dockOffset = 0.f;
    bool motionEnabled() const { return cachedMotionEnabled; }
    void refreshDisplayTiming();
    void startAnimTimer();
    void stopAnimTimer();
    void armAnimTimer();
    BOOL nextMessage(MSG& msg);
    void animateFrame();
    void updateVisibilityTimer();
    bool advanceViewMotion(View& view, float dt);
    std::optional<POINT> advanceVisibility(float dt);
    void positionSheet(bool reopening = false);
    std::optional<POINT> advanceSheet(double nowMs);
    void triggerSheetContentAnim(float dx, float dy);
    float sheetContentProgress() const;
    void triggerAppBounce(const Text& key);
    float appBounceOffset(const Text& key) const;
    float appWaveBoost(float centerX) const;
    float appActiveRatio(const LiveDockItem& key, bool active);
    void syncMeterTargets();
    void scrollCatalog(int delta);
    void switchQuickPage(int next);
    void closeSheet(bool redrawBar = true);
    void changeHover(View& view, int hovered);
    void setPressed(View& view, int pressed);
    void beginSwitch(int index, bool enabled);
    void setOpacityFromX(float localX, bool save);
    void open(int id, float x = -1, bool focus = true);
    void close();
    void sample();
    void tick(bool pollWindows = true);
    void sampleExtras(bool immediate = false);
    bool tickExtras(ULONGLONG stamp, const SYSTEMTIME& local);
    void changeVolume(int by, bool mute = false);
    void setVolumeLevel(float level, bool instant = false, bool dragging = false);
    void changeVolumeAt(float x, bool dragging = false, bool commit = true);
    void changeBarVolumeAt(float localX, bool dragging, bool commit = true);
    void advanceBarVolume(double nowMs);
    void refreshWifi(bool scan = false, bool list = true);
    void connectWifi(size_t index, bool fromSaved = false);
    void disconnectWifi(const GUID& adapter);
    void queryRadios(int toggle = 0);
    void queryBluetoothDevices();
    void toggleBluetoothDevice(size_t index);
    void queryQuickStates();
    void toggleEnergySaver();
    void setDisplayTopology(UINT32 topoFlag);
    void toggleNightLight();
    void setNearbyShare(int mode);
    void toggleAccessibility(int item);
    void refreshAudioEndpoints();
    void setAudioEndpoint(const Text& deviceId);
    void openQuickSub(QuickSubPanel sub);
    void closeQuickSub();
    void showSystemQuick();
    void showSystemTray(float x = -1);
    void refreshSystemTray(bool forceReload = false);
    void pollTrayCapture();
    void prewarmTrayCaches();
    void startWindowHook();
    void stopWindowHook();
    void scheduleWindowSync();
    void completeDeferredStartup();
    void activateTrayIcon(int index, bool rightClick, POINT screenPt = {});
    void startTrayIconAction(int index, TrayAction action, POINT screenPt = {});
    void executeTrayMenuItem(const TrayMenuItem& item);
    void finishTrayMenu(bool executed = false);
    const std::vector<TrayMenuItem>& currentTrayMenu() const;
    Text trayMenuTitle() const;
    void updateTraySync();
    void stopTraySync();
    bool trayBusy() const { return trayAction != TrayAction::None; }
    bool invokeSystemTray();
    void requestNativeFlyout(NativeFlyout flyout);
    void updateNativeFlyout();
    void systemShortcut(WORD key, bool control = false);
    void requestWeather(bool refresh = false);
    bool weatherInUse() const;
    void requestMarket(bool refresh = false);
    bool tickMarket();
    void editMarket();
    void editMarketKey();
    void animateWidgets();
    float widgetProgress() const;
    void selectCity(int city);
    Text worldTime();
    Text worldOffsetText(bool* isDaylight = nullptr);
    Text tempText(bool preview = false) const;
    void refreshAppImages();
    void requestAppImages();
    void pollAppImages();
    void requestInstalledAppImages(const std::vector<Text>& targets);
    void pollInstalledAppImages();
    int refreshOpenWindows();
    void requestWindowIcons();
    void activateWindow(HWND target, bool toggleMinimize = false);
    size_t pinnedOnBar() const;
    std::vector<size_t> runningOnBar() const;
    int pinnedIndex(const Shortcut& app) const;
    void togglePin(const Shortcut& app);
    bool setPinned(const Shortcut& app, bool pin, HWND sourceWindow = nullptr);
    void registerAppDrop(HWND window);
    bool appDropHover = false;
    void importTaskbarPins();
    void toggleDesktop();
    void queryMedia();
    bool mediaInUse() const;
    void sampleMicrophone(bool toggle = false);
    void microphonePanel(Palette p);
    void refreshMicEndpoints();
    void setMicEndpoint(const Text& deviceId);
    void sampleKeyboardLayout();
    void switchKeyboardLayout(HKL targetHkl);
    void cycleKeyboardLayout(int delta);
    void keyboardPanel(Palette p);
    void notificationPanel(Palette p);
    void networkPanel(Palette p);
    void refreshNotifications(bool force = false);
    void pollNotifications();
    void dismissNotification(int64_t id);
    void clearAllNotifications();
    void activateNotification(const WinNotification& item);
    void queryDoNotDisturb();
    void toggleDoNotDisturb();
    void sampleNetworkFast();
    float statusBarWidth() const;
    float trailingWidth() const;
    void drawStatusBar(View& view, float x, Palette p);
    void drawQuickToggles(View& view, Box b, Palette p, bool preview, bool labels = false);
    void showAppMenu(Shortcut app, float x = -1, HWND window = nullptr, std::shared_ptr<Gdiplus::Bitmap> icon = {});
    void sizeAppMenu();
    void pollJumpList();
    void launchJumpItem(const JumpItem& item);
    void closeAppWindows(const std::vector<HWND>& windows);
    void showChromeWindows(float x, bool hover = false);
    void trackChromeHover();
    void updateChromeHover();
    bool syncChromeWindows(bool renew = false);
    void refreshChromeWindows();
    void updateChromePreviews();
    void sizeChromeWindows(float availableWidth);
    void scrollChromeWindows(int delta);
    void chromeWindowsPanel(Palette p);
    void backTrayMenu();
    void showWindowMenu(HWND window, float x = -1);
    void loadApps();
    void updateInstalledApps();
    void loadRecentFiles();
    void refreshRecycleBin();
    void startCapture(int mode, HWND target = nullptr);
    void finishCapture();
    void launchShortcut(const Shortcut& app);
    void runTool(const Text& input);
    void toggleTile(int id);
    bool shown(int id) const;
    void changed(bool layout = false);
    void report(Text message);
    void launch(const Text& target, const Text& dir = {});
    int edit(const Text& title, std::vector<Field>& fields, bool remove = false,
             const std::function<Text(const std::vector<Field>&)>& validate = {});
    void editEvent(int index);
    void editTask(int index);
    void editShortcut(int index);
    void editMetric(int widget, int index);
    void editRepo();
    void editNote();
    SessionTimer& timerFor(int id) { return id == Focus ? focusTimer : countdownTimer; }
    int& minutesFor(int id) { return id == Focus ? model.focusMinutes : model.countdownMinutes; }
    bool updateTimers(ULONGLONG stamp);
    void skipFocus(bool keepRunning = true);
    int sessionMinutes(int id) const;
    void toggleStopwatch();
    void lapStopwatch();
    void resetStopwatch();
    float timerRim(int id, ULONGLONG stamp) const;
    void animateTimerRim(int id, float previous);
    bool timerRimMoving(double nowMs);
    void requestCurrency(bool force = false);
    bool tickProductivity(ULONGLONG stamp);
    void requestWhatsAppAccess();
    void launchWhatsApp();
    void productivityPanel(Palette p);
    void drawProductivityWidget(View& view, int id, Box b, Palette p, bool preview);
    void toggleTimer(int id);
    void resetTimer(int id);
    void setTimerMinutes(int id, int minutes);
    bool applyTimerInput();
    void openOverflow();
    void media(WORD key);

    void paintBar(const POINT* origin = nullptr, bool repaintOtherDocks = true);
    void paintBarView(View& view, const std::vector<OrderItem>& overflow, const POINT* origin = nullptr);
    void paintSheet(const POINT* origin = nullptr);
    void studio(Palette p);
    void contextMenu(Palette p);
    void overflowPanel(Palette p);
    void systemTray(Palette p);
    void gallery(Palette p);
    void calendar(Palette p);
    void timerPanel(Palette p);
    void tasks(Palette p);
    void notes(Palette p);
    void system(Palette p);
    void git(Palette p);
    void metrics(Palette p);
    void playback(Palette p);
    void extraPanel(Palette p);
    void appPanel(Palette p);
    void marketPanel(Palette p);
    void proPanel(Palette p);
    void drawNewWidget(View& view, int id, Box b, Palette p, bool preview, bool editing);
    void drawProWidget(View& view, int id, Box b, Palette p, bool preview, bool editing);
    void sampleProWidgets(bool forceWeb = false);
    bool tickProWidgets(ULONGLONG stamp);
    void editProWidget(int id);
    void editProKey(int id);
    float widgetWidth(int id, bool preview = false) const;
    void widgetCard(View& view, int id, Box b, Palette p, bool preview = false);
    void drawAppSlot(View& view, OrderItem item, Box slot, Palette p, float dragLift = 0.f);
    void knob(Palette p, Box b, bool enabled, std::function<void()> action);
    bool hit(View& view, Box bounds, std::function<void()> action, std::function<void()> secondary = {});
    void button(View& view, Palette p, Box bounds, const Text& label, std::function<void()> action, bool active = false, Font font = Caption);
    void iconButton(View& view, Palette p, Box bounds, Glyph icon, std::function<void()> action, bool active = false);
    void badge(Draw& draw, Box bounds, Glyph icon, unsigned color);
    void heading(Palette p, const Text& title, const Text& detail);
    void spark(Draw& draw, Box b, const std::array<long long, 7>& values, unsigned color, bool area = false);
    std::array<long long, 7> history(int widget) const;
};
