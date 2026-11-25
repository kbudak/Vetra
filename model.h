#pragma once
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <vector>

using Text = std::wstring;
inline Text num(long long value) { return std::to_wstring(value); }
inline Text two(int value) { return (value < 10 ? L"0" : L"") + num(value); }
inline constexpr const wchar_t* Months[] = {L"Ocak", L"Şubat", L"Mart", L"Nisan", L"Mayıs", L"Haziran", L"Temmuz", L"Ağustos", L"Eylül", L"Ekim", L"Kasım", L"Aralık"};
inline constexpr const wchar_t* ShortMonths[] = {L"Oca", L"Şub", L"Mar", L"Nis", L"May", L"Haz", L"Tem", L"Ağu", L"Eyl", L"Eki", L"Kas", L"Ara"};
inline constexpr const wchar_t* ShortMonthsUpper[] = {L"OCA", L"ŞUB", L"MAR", L"NİS", L"MAY", L"HAZ", L"TEM", L"AĞU", L"EYL", L"EKİ", L"KAS", L"ARA"};
inline constexpr const wchar_t* Days[] = {L"PAZ", L"PZT", L"SAL", L"ÇAR", L"PER", L"CUM", L"CMT"};
inline constexpr const wchar_t* ShortDays[] = {L"Paz", L"Pzt", L"Sal", L"Çar", L"Per", L"Cum", L"Cmt"};
inline constexpr const wchar_t* LongDays[] = {L"Pazar", L"Pazartesi", L"Salı", L"Çarşamba", L"Perşembe", L"Cuma", L"Cumartesi"};
inline constexpr const wchar_t* LongDaysUpper[] = {L"PAZAR", L"PAZARTESİ", L"SALI", L"ÇARŞAMBA", L"PERŞEMBE", L"CUMA", L"CUMARTESİ"};
struct Date {
    int year, month, day;
    int key() const { return year * 10000 + month * 100 + day; }
    static Date fromKey(int key) { return {key / 10000, key / 100 % 100, key % 100}; }
    static Date from(const SYSTEMTIME& time) { return {time.wYear, time.wMonth, time.wDay}; }
    int weekday() const;
    int length() const;
    bool valid() const { return year >= 1601 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= length(); }
    Date add(int delta) const;
    Text label() const { return num(day) + L" " + Months[month - 1]; }
};
inline SYSTEMTIME now() { SYSTEMTIME value{}; GetLocalTime(&value); return value; }

enum Widget {
    Apps, Calendar, Focus, Tasks, Notes, System, Git, Sales, Growth, Media, Clock,
    Countdown, Stopwatch, World, Storage, NetworkWidget, Power, Volume, OpenWindows, Quick, Weather, Usage,
    SevenDays, TodayWidget, DateCard, DayCard, SegmentClock, ClockWeather, FlipClock,
    WeatherDetails, WeatherForecast, WeatherStatus, WeatherHourly, SunTimes,
    StockChart, StockWatchlist, Crypto, Recent, RecycleBin, Search, Commands, Capture,
    AnalogClock, CpuMeter, RamMeter, DayProgress, MonthProgress, YearProgress, DesktopToggle,
    AiLimits, StripeChart, ChatGptUsage, GitHubActivity, DataFastChart, Temperature, XProfile,
    ClaudeUsage, ClaudePlanUsage, GeminiUsage, GrokUsage, DeepSeekBalance, GlmUsage,
    OpenCodeUsage, OpenCodePlanUsage, CursorUsage, StartButton, AiLimitsTriple,
    DayTimeline, WhatsApp, Currency, MediaCompact, MediaMini, MediaBar, MicControl, QuickToggles, WidgetCount,
    Studio = WidgetCount, ContextMenu, SystemTray, AppMenu, Overflow, ChromeWindows, KeyboardLang
};
enum WidgetGroup { Workspace, Productivity, Device, Analytics, WeatherGroup, MarketGroup, AppGroup, AiGroup };
enum Profile { Developer, Seller, Founder, Student, ProfileCount };
inline constexpr const wchar_t* ProfileNames[] = {L"Geliştirici", L"Satıcı", L"Kurucu", L"Öğrenci"};
struct Info {
    const wchar_t* title;
    const wchar_t* description;
    unsigned color;
    int width;
    WidgetGroup group;
};
inline constexpr Info Widgets[] = {
    {L"Uygulamalar", L"Sabitlenen uygulamalar ve açık pencereler", 0x579AFF, 178, AppGroup},
    {L"Takvim", L"Aylık görünüm ve ajanda", 0x4E8CFF, 224, Productivity},
    {L"Pomodoro", L"Odak ve mola döngüsü, dört tur", 0xFF4038, 188, Productivity},
    {L"Görevler", L"Sonraki görevi dock’tan tamamla", 0x0F88F9, 210, Productivity},
    {L"Notlar", L"Profile özel hızlı not", 0xFFCB38, 210, Productivity},
    {L"Sistem", L"Anlık CPU ve RAM", 0x65ADFF, 167, Device},
    {L"Git", L"Yerel depo ve aktif dal", 0xA195FF, 134, Analytics},
    {L"Satışlar", L"Gelir, sipariş ve yedi günlük trend", 0x49CC86, 170, Analytics},
    {L"Büyüme", L"Ziyaret, kayıt ve dönüşüm", 0xA195FF, 170, Analytics},
    {L"Medya", L"Kapak görseli, başlık ve oynatma kontrolleri", 0xEB83B7, 246, Workspace},
    {L"Saat", L"Saniyeli büyük saat ve güncel tarih", 0x73ABFF, 140, Workspace},
    {L"Sayaç", L"Sesli geri sayım", 0x0F88F9, 174, Productivity},
    {L"Kronometre", L"Süre ve tur kayıtları", 0xFFA10A, 174, Productivity},
    {L"Dünya saati", L"Seçilen şehrin saati ve saat farkı", 0x60BCFB, 178, Workspace},
    {L"Depolama", L"Boş alan ve disk doluluğu", 0x77A5FF, 172, AppGroup},
    {L"Ağ trafiği", L"İndirme ve yükleme hızı", 0x63CFF5, 179, Device},
    {L"Pil", L"Şarj ve güç durumu", 0x52D58E, 99, Device},
    {L"Ses", L"Windows ses seviyesi", 0xA399FF, 170, Device},
    {L"Pencereler", L"Açık uygulamalar arasında geçiş", 0x77A5FF, 137, Workspace},
    {L"Hızlı ayarlar", L"Windows ayarlarına erişim", 0xA4B7D5, 53, Device},
    {L"Hava durumu", L"Şu anki sıcaklık, en yüksek ve en düşük", 0x59B3FF, 157, WeatherGroup},
    {L"Ekran süresi", L"Bugünkü saatlik bilgisayar kullanımı ve uygulamalar", 0x0F88F9, 208, Productivity},
    {L"7 Gün", L"Haftalık gün şeridi ve anlık sıcaklık", 0x0A84FF, 216, Workspace},
    {L"Bugün", L"Gün, tarih, saat ve hava durumu tek kartta", 0xFF5A4F, 184, Workspace},
    {L"Tarih", L"Tam gün ve tarih, büyük görünüm", 0x73ABFF, 144, Workspace},
    {L"Gün sayısı", L"Takvim yaprağı olarak bugünün günü", 0xFF5545, 58, Workspace},
    {L"Segment saat", L"Yedi segmentli dijital saat ve tarih", 0xFF5533, 168, Workspace},
    {L"Saat ve hava", L"Saat, gün ve sıcaklık tek kartta", 0xFF3B14, 68, Workspace},
    {L"Yaprak saat", L"Her dakika yaprak çeviren mekanik saat", 0x8E9EFF, 154, Workspace},
    {L"Hava ayrıntıları", L"Rüzgâr, yağış ve günün sıcaklık aralığı", 0x59B3FF, 246, WeatherGroup},
    {L"Bugün ve tahmin", L"Bugün ve sonraki üç gün", 0x59B3FF, 260, WeatherGroup},
    {L"Hava özeti", L"Hava durumu, şehir ve sıcaklık", 0x59B3FF, 236, WeatherGroup},
    {L"Saatlik hava", L"Şu an ve sonraki beş saat", 0x59B3FF, 258, WeatherGroup},
    {L"Gün doğumu ve batımı", L"Güneşin gün içindeki konumu", 0xFFB34D, 239, WeatherGroup},
    {L"Hisse grafiği", L"Bir hissenin fiyatı ve gün içi grafiği", 0x2BCB71, 242, MarketGroup},
    {L"Hisse takip listesi", L"Üç hissenin fiyatı ve günlük değişimi", 0x2BCB71, 242, MarketGroup},
    {L"Kripto", L"Binance fiyatı, 24 saatlik değişim ve grafik", 0xF6A93A, 242, MarketGroup},
    {L"Son açılanlar", L"Yakın zamanda açılan uygulamalar ve dosyalar", 0xA6A6B5, 144, AppGroup},
    {L"Geri Dönüşüm Kutusu", L"Kutudaki öğeleri görüntüle veya boşalt", 0xA6A6B5, 65, AppGroup},
    {L"Ara ve aç", L"Uygulama, web, dosya veya klasör ara", 0xA6A6B5, 212, AppGroup},
    {L"Komutlar", L"Bir kısayol veya Windows eylemi çalıştır", 0x2BCB71, 210, AppGroup},
    {L"Ekran yakalama", L"Ekranı, bölgeyi veya bir pencereyi yakala", 0xA792FF, 173, AppGroup},
    {L"Analog saat", L"Kadranlı yerel saat", 0x86A8F8, 108, Workspace},
    {L"İşlemci", L"Anlık CPU kullanımı", 0x67BEFF, 112, Device},
    {L"Bellek", L"Anlık RAM kullanımı", 0xB19CFA, 112, Device},
    {L"Gün ilerlemesi", L"Geçen günün yüzdesi", 0x6FCAB6, 135, Productivity},
    {L"Ay ilerlemesi", L"Ayın geçen kısmı", 0xAF9AEB, 135, Productivity},
    {L"Yıl ilerlemesi", L"Yılın geçen kısmı", 0xEDAD78, 135, Productivity},
    {L"Masaüstünü göster", L"Pencereler ve masaüstü arasında geçiş", 0x9DA6EB, 57, AppGroup},
    {L"AI Limitleri", L"Claude, Codex ve Antigravity arasından iki plan seç", 0x2BCB71, 248, AiGroup},
    {L"Stripe Grafiği", L"Stripe net gelir çizgi grafiği ve değişim yüzdesi", 0x635BFF, 244, Analytics},
    {L"ChatGPT Kullanımı", L"ChatGPT Codex 5 saatlik oturum ve haftalık limitleri", 0x2BCB71, 246, AiGroup},
    {L"GitHub Aktivitesi", L"GitHub katkı ızgarası, günlük/haftalık sayı ve seri", 0x39D353, 236, Analytics},
    {L"DataFast Grafiği", L"14 günlük site ziyaretçileri ve haftalık değişim", 0xFF7A45, 244, Analytics},
    {L"Sıcaklık", L"CPU sıcaklık göstergesi, işlemci ve bellek yükü", 0x2BCB71, 224, Device},
    {L"X Profili", L"X hesabındaki takipçi sayısı", 0xE2E2E2, 152, Analytics},
    {L"Claude Kullanımı", L"Claude oturum limiti ve sıfırlanma süresi", 0xD97757, 216, AiGroup},
    {L"Claude Plan", L"Oturum, haftalık Claude limitleri ve en çok kullanılan model", 0xD97757, 258, AiGroup},
    {L"Gemini Kullanımı", L"5 saatlik ve haftalık Gemini kota kullanımı", 0x4285F4, 246, AiGroup},
    {L"Grok Kullanımı", L"SuperGrok kredi kullanımı ve yenilenme tarihi", 0xE2E2E2, 238, AiGroup},
    {L"DeepSeek Bakiye", L"DeepSeek bakiyesi, bugünkü ve 7 günlük harcama", 0x4D6BFE, 232, AiGroup},
    {L"GLM Kullanımı", L"Z.ai / Zhipu GLM Coding Plan oturum, haftalık ve MCP limitleri", 0x2BCB71, 250, AiGroup},
    {L"OpenCode Kullanımı", L"Yerel geçmişten son 5 saatlik OpenCode token kullanımı", 0xE2E2E2, 212, AiGroup},
    {L"OpenCode Plan", L"OpenCode oturum, haftalık token ve en çok kullanılan model", 0xE2E2E2, 248, AiGroup},
    {L"Cursor Kullanımı", L"Cursor Auto ve API harcama/kota limitleri", 0x2BCB71, 246, AiGroup},
    {L"Başlat", L"Windows Başlat menüsünü aç", 0x599CFF, 54, AppGroup},
    {L"AI Limitleri · Üçlü", L"Claude, Codex ve Antigravity 5 saatlik ve haftalık kotaları", 0x2BCB71, 248, AiGroup},
    {L"Gün çizelgesi", L"Saatlik ekran süresi, odak, etkinlikler ve güneş saatleri", 0x0F88F9, 244, Productivity},
    {L"WhatsApp", L"Windows bildirimleri ve son gönderen", 0x25D366, 170, Productivity},
    {L"Döviz", L"Günlük referans kurlarıyla para birimi dönüştürme", 0xA7A9AC, 184, Productivity},
    {L"Medya Kompakt", L"Parça adı, sanatçı ve oynat/duraklat", 0xAD7EE8, 190, Workspace},
    {L"Medya Mini", L"Oynat/duraklat düğmesi ve parça ilerleme halkası", 0xAD7EE8, 56, Workspace},
    {L"Medya Çubuğu", L"Kapak, parça ilerlemesi ve sonraki parça", 0xAD7EE8, 235, Workspace},
    {L"Mikrofon", L"Varsayılan mikrofonu ve ses çıkışını ayrı ayrı sustur", 0x519EFF, 99, Device},
    {L"Hızlı Kontroller", L"Wi-Fi, Bluetooth, mobil erişim noktası ve uçak modu", 0x0F88F9, 232, Device},
};
static_assert(ARRAYSIZE(Widgets) == WidgetCount);
inline bool mediaWidget(int id) { return id == Media || (id >= MediaCompact && id <= MediaBar); }
inline bool weatherWidget(int id) { return id == Weather || (id >= WeatherDetails && id <= SunTimes); }
inline bool marketWidget(int id) { return id >= StockChart && id <= Crypto; }
inline bool proWidget(int id) { return id == AiLimitsTriple || (id >= AiLimits && id <= CursorUsage); }
inline bool aiWidget(int id) { return id == AiLimitsTriple || id == AiLimits || id == ChatGptUsage || (id >= ClaudeUsage && id <= CursorUsage); }
inline constexpr size_t MaxShortcuts = 64;
struct City { const wchar_t* name; const wchar_t* zone; const wchar_t* coordinates; };
inline constexpr City Cities[] = {
    {L"İstanbul", L"Turkey Standard Time", L"latitude=41.0082&longitude=28.9784"},
    {L"Ankara", L"Turkey Standard Time", L"latitude=39.9334&longitude=32.8597"},
    {L"İzmir", L"Turkey Standard Time", L"latitude=38.4237&longitude=27.1428"},
    {L"Londra", L"GMT Standard Time", L"latitude=51.5072&longitude=-0.1276"},
    {L"Berlin", L"W. Europe Standard Time", L"latitude=52.5200&longitude=13.4050"},
    {L"New York", L"Eastern Standard Time", L"latitude=40.7128&longitude=-74.0060"},
    {L"Tokyo", L"Tokyo Standard Time", L"latitude=35.6762&longitude=139.6503"},
};
struct Tile { int id; bool enabled; };
struct DockSlot {
    bool isApp = false;
    int widget = -1;
    Text appKey;
};
struct Shortcut { Text label, target, identity; };
struct Event { int day, kind; Text time, title; };
struct Task { int profile; bool complete; Text title; };
struct Metric { int day, widget; long long first, second; };
enum AppearanceTheme { LiquidGlass = 0, WhiteTheme = 1, BlackTheme = 3, FrostGlass = 4, BareTheme = 6 };
inline bool glassTheme(AppearanceTheme theme) { return theme == LiquidGlass || theme == FrostGlass; }
struct AccentOption { const wchar_t* label; unsigned light, dark; };
inline constexpr AccentOption Accents[] = {
    {L"Varsayılan", 0, 0}, {L"Mavi", 0x155BBB, 0x8BB9FF}, {L"Turkuaz", 0x08675B, 0x77DCCA},
    {L"Mor", 0x663EB1, 0xC4A6FF}, {L"Mercan", 0x9A391E, 0xFFAE91}
};
inline constexpr int UsageHistoryDays = 7, MaxUsageAppsPerDay = 64;
inline constexpr const wchar_t* CurrencyCodes[] = {L"CNY", L"USD", L"EUR", L"TRY", L"GBP", L"JPY", L"CHF", L"CAD", L"AUD"};
inline constexpr unsigned TimelineColors[] = {0x0F88F9, 0x37BD80, 0xF6AD2E, 0xB883EF};
struct UsageDaily { int day, seconds; };
struct UsageApp { int day, seconds; Text key, label; };
inline constexpr int FocusMaxMinutes = 180, CountdownMaxMinutes = 1440;
inline constexpr int AiPlanCount = 8;
inline constexpr const wchar_t* AiPlanNames[AiPlanCount] = {
    L"Claude", L"ChatGPT", L"Gemini", L"Grok", L"Cursor", L"GLM", L"DeepSeek", L"OpenCode"
};
struct AiOverride {
    int p1 = -1, p2 = -1, p3 = -1;
    Text r1, r2, r3, extraLabel, extraVal;
};
enum class DockVisibility { Overlay, Edge, Reserved };

struct StatusBarConfig {
    bool enabled = true;
    bool microphone = true;
    bool micOnlyInUse = false;
    bool keyboard = true;
    bool network = true;
    bool volume = true;
    bool battery = true;
    bool clock = true;
    bool notifications = true;
};

struct Model {
    int profile = Student;
    AppearanceTheme theme = LiquidGlass;
    int opacity = 50, size = 1, edge = 0, alignment = 1, accent = 0;
    StatusBarConfig statusBar;
    int focusMinutes = 25, countdownMinutes = 5, worldCity = 3, weatherCity = -1;
    int usageDay = 0;
    std::array<int, 24> usageSeconds{};
    std::array<std::array<int, 24>, ARRAYSIZE(TimelineColors)> timelineUsage{};
    std::array<int, 24> timelineFocus{};
    double currencyAmount = 500.0;
    int currencyFrom = 0, currencyTo = 1;
    std::vector<UsageDaily> usageHistory;
    std::vector<UsageApp> usageApps;
    bool topmost = true;
    bool hideTaskbar = true;
    DockVisibility visibility = DockVisibility::Overlay;
    bool autoHides() const { return visibility == DockVisibility::Edge; }
    Text repo;
    std::array<Text, 4> stockSymbols{L"TSLA", L"CRM", L"ORCL", L"AAPL"};
    Text cryptoSymbol = L"BTC/USDT";
    int aiLimitFirst = 0, aiLimitSecond = 1;
    std::array<AiOverride, AiPlanCount> aiOverrides;
    Text githubUser, xHandle;
    long long xFollowers = -1;
    double stripeRevenue = -1, stripeChange = 0;
    long long dataFastVisitors = -1;
    double dataFastChange = 0;
    double deepSeekBalance = -1, deepSeekToday = 0, deepSeekWeek = 0;
    std::array<Text, ProfileCount> notes;
    std::array<std::vector<Tile>, ProfileCount> layouts;
    std::array<std::vector<DockSlot>, ProfileCount> dockOrder;
    std::vector<Shortcut> shortcuts, recentApps;
    std::vector<Event> events;
    std::vector<Task> tasks;
    std::vector<Metric> metrics;
    Text folder;
    Model();
    void load();
    bool save() const;
    bool startup() const;
    bool setStartup(bool enable) const;
    void rollUsageDay(Date day);
    int usageFor(int day) const;
    std::array<long long, 2> total(int widget, Date day) const;
};
inline constexpr const wchar_t* EventKinds[] = {L"Etkinlik", L"Teslim", L"Kişisel"};
inline constexpr unsigned EventColors[] = {0x8C9AFF, 0xF4B472, 0x68D9B1};
Text utf16(const std::string& value);
Text leaf(const Text& value);
Text money(long long cents);
Text grouped(long long value);
Text currentBranch(const Text& repo);

