<div align="center">

# Vetra

**Windows 10 & 11 için yerel, ultra hafif ve kişiselleştirilebilir masaüstü dock’u.**

[![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011%20(x64)-0078D6?style=flat-square&logo=windows)](https://github.com/kbudak/Vetra)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus)](https://github.com/kbudak/Vetra)
[![Graphics](https://img.shields.io/badge/UI-Win32%20%2B%20Direct2D%20%2B%20DirectWrite-111827?style=flat-square)](https://github.com/kbudak/Vetra)
[![Widgets](https://img.shields.io/badge/Widget%20Katalo%C4%9Fu-75%20Widget-2FD36B?style=flat-square)](https://github.com/kbudak/Vetra)
[![Portable](https://img.shields.io/badge/Kurulum-Tek%20EXE%20%C2%B7%20Ta%C5%9F%C4%B1nabilir-8B5CF6?style=flat-square)](https://github.com/kbudak/Vetra)

</div>

---

## Vetra Nedir?

**Vetra**, Windows’un varsayılan görev çubuğunun yerini alabilen, tamamen yerel **C++20 / Win32 / Direct2D / DirectWrite** ile yazılmış modern bir masaüstü dock’udur. Electron, WebView veya harici bir çalışma zamanı gerektirmez; tek bir taşınabilir `Vetra.exe` (~2,3 MB) olarak çalışır, yönetici izni veya kurulum istemez.

Sadece uygulama başlatmakla kalmaz; **75 modüler widget**, **yerel Denetim Merkezi**, **HD Gizli Simgeler ve panel içi sağ tık menüsü**, **evrensel Windows Atlama Listeleri (Jump Lists)**, **canlı CPU/RAM/sıcaklık telemetrisi** ve **yapay zekâ kota takibi** ile masaüstünü tek bir akıcı şeritte toplar.

---

## Öne Çıkan Özellikler

### Cam Estetiği, Temalar ve Çoklu Monitör
- **5 tema:** **Liquid Glass**, **Buzlu cam**, **Beyaz**, **Siyah** ve dock zeminini gizleyip bağımsız siyah kartlar bırakan **Çerçevesiz** tema.
- **Canlı özelleştirme:** Cam temalarında doğrudan sürüklenebilir veya tekerlekle ayarlanabilir **Cam tonu (0–100)** ve özel **Vurgu rengi**.
- **Akıcı yay animasyonları:** Ekran yenileme hızına uyarlanan (≤90 Hz tam bölenli) kritik sönümlü yaylar, simge dalga ölçeklemesi ve başlatma sıçraması (Windows animasyon tercihi kapalıysa anlık geçiş).
- **Tüm monitörlerde eşzamanlı dock:** Bağlı tüm ekranlarda ortak düzeni paylaşır; paneller tıklanan ekranda açılır ve tam ekran uygulamalar yalnızca bulundukları monitörün dock’unu gizler.

### Masaüstü Dostu Görünürlük Modları
**Görünüm → Dock görünürlüğü** üzerinden üç mod arasında anında geçiş yapabilirsin:
- **Sabit göster:** Dock’u seçili kenarda sürekli görünür tutar; büyütülen pencereler dock’un arkasına uzanabilir.
- **Kenar ile aç:** İmleç ekran kenarına geldiğinde dock’u yumuşakça açar, ayrılınca gizler.
- **Yer ayır:** Büyütülen pencerelerin (Chrome, VS Code, Dosya Gezgini vb.) dock’un arkasına girmesini engelleyecek çalışma alanını her monitörde ayırır — bunu masaüstüne `SPIF_SENDCHANGE` yayını göndermeden sessiz `SPI_SETWORKAREA` ile yaptığı için **masaüstü simge düzenini bozmaz**.
- **Güvenli görev çubuğu yönetimi:** Vetra çalışırken Windows görev çubuğunu gizler; normal çıkışta veya beklenmedik kapanmada izleyici süreç (`--restore-taskbar`) görev çubuğunu otomatik olarak geri yükler.

### Akıllı Uygulama Şeridi ve Evrensel Jump List Desteği
- **Özgür sürükle-bırak düzeni:** Widget’ları, sabit uygulamaları ve açık pencereleri dock üzerinde birbirlerinin arasına serbestçe sürükleyebilir; masaüstünden, Gezgin’den veya Başlat’tan uygulama / `.lnk` kısayolu bırakarak sabitleyebilirsin (**Windows sabitlerini al** dahil).
- **Evrensel Windows Atlama Listeleri (Jump Lists):** Dock’taki herhangi bir uygulamaya (hem mevcut hem de sonradan kuracağın tüm Win32 ve MSIX/UWP uygulamalarına) sağ tıkladığında Windows görev çubuğundaki gerçek görevler, sabitlenenler ve son kullanılanlar kendi simgeleriyle görünür:
  - *Chrome:* Yeni pencere, Yeni Gizli pencere, En çok ziyaret edilenler (site favicon’larıyla), Son kapatılanlar
  - *Outlook:* Yeni İleti, Yeni Etkinlik, Takvim Aç
  - *ChatGPT / Codex:* Yeni sohbet, Yakın zamanlı sohbetler
  - *VS Code / Antigravity IDE:* New Window, Recent Folders, son dosyalar
  - *Dosya Gezgini:* Sabitlenenler ve Sık kullanılan klasörler
  - *Windows Terminal:* Kurulu tüm kabuk profilleri
  - Alt bölümde **Dock’a sabitle / kaldır**, **Pencereye geç / Uygulamayı aç** ve açık pencereler için **Pencereyi kapat / Tüm pencereleri kapat (N)** yer alır.
- **Chrome çoklu pencere önizlemesi:** İmleci açık Chrome simgesi üzerinde bekletince küçültülmüş olanlar dahil tüm Chrome pencereleri canlı kartlar hâlinde listelenir.

### Yerel Denetim Merkezi ve Sıfır Titremeli Gizli Simgeler
- **Denetim Merkezi:** Dock’a bitişik iki sayfalı panelde **Wi-Fi** (canlı ağ taraması ve kayıtlı profiller), **Bluetooth** (A2DP Stereo + HFP `ContainerId` eşleştirmeli doğrudan ses bağlantısı ve varsayılan çıkış geçişi), **Uçak modu**, **Enerji tasarrufu**, **Gece ışığı**, **Mobil etkin nokta**, **Yansıt** (çoklu ekran düzenleri), **Erişilebilirlik** (7 yerel anahtar), **Yakın paylaşım**, **Ses çıkışı seçici** ve **canlı Medya kartı** (Windows SMTC oynat/duraklat, ileri/geri veya ±10 sn sarma) tek panelde çalışır.
- **Gizli simgeler (`^`):** Yüksek çözünürlüklü 5 sütunlu ızgara (`NotifyIconSettings` PNG, MSIX ve `.exe` kaynakları). Sağ tık, taşma penceresini ve açılan yerel menüyü ekranda titreme oluşturmadan gizlice yakalayıp (`#32768`, çok seviyeli alt menüler, WinUI 3, Electron, WebView2, WPF, Qt) doğrudan Vetra temasında gösterir ve komutu gerçek uygulamada çalıştırır.
- **Masaüstünü göster (`|`):** Dock’un en sağ ucundaki ince şerit tüm açık pencereleri aşağı alır veya geri getirir.

---

## 75 Widget’lık Katalog ve Profiller

Her widget her profilde kullanılabilir; **Geliştirici**, **Satıcı**, **Kurucu** ve **Öğrenci** profilleri farklı düzen ve görev/not setlerini saklar.

| Kategori | Öne Çıkan Widget’lar |
| --- | --- |
| **Sistem & Donanım** | **Sıcaklık** (canlı PDH termal sensör `°C`, Görev Yöneticisi uyumlu `% Processor Utility` CPU yükü ve RAM), **Sistem**, **İşlemci**, **Bellek**, **Ağ trafiği**, **Depolama**, **Pil**, **Hızlı Kontroller**, **Denetim Merkezi** |
| **Yapay Zekâ** | **AI Limitleri** (ikili & üçlü: Claude, Codex, Antigravity), **ChatGPT Kullanımı** (canlı `codex app-server` + yerel oturum yedeği), **Gemini Kullanımı** (`agy` CLI canlı kota + yerel CLI), **Claude Kullanımı / Plan**, **OpenCode Kullanımı / Plan** (`opencode.db`), **Cursor**, **Grok**, **GLM**, **DeepSeek Bakiye** |
| **Üretkenlik & Zaman** | **Saat**, **Analog saat**, **Segment saat**, **Yaprak saat (Flip)**, **Dünya saati**, **Takvim**, **Bugün**, **7 Gün**, **Gün çizelgesi**, **Ekran süresi** (7 günlük geçmiş), **Pomodoro** (süre çerçeveli), **Sayaç**, **Kronometre**, **Görevler**, **Notlar**, **Gün / Ay / Yıl ilerlemesi**, **Döviz** (Frankfurter), **WhatsApp** |
| **Medya & Ses** | **Ses** (dock üzerinde 1:1 sürüklenebilir/tekerlekli mavi kaydırıcı), **Mikrofon**, **Medya**, **Medya Kompakt**, **Medya Mini**, **Medya Çubuğu** |
| **Hava Durumu** | Anlık durum, **Saat ve hava**, **Saatlik tahmin**, **3 Günlük tahmin**, **Hava ayrıntıları**, **Güneş saatleri** ([Open-Meteo](https://open-meteo.com/en/docs), API anahtarı gerekmez) |
| **Piyasalar & Analiz** | **Kripto** (Binance, anahtarsız), **Hisse grafiği & Takip listesi** (Twelve Data), **GitHub Aktivitesi** (anahtarsız katkı ızgarası), **Stripe Grafiği**, **DataFast Grafiği**, **X Profili**, **Git dalı**, **Satış / Büyüme** |
| **Uygulama & Araçlar** | **Başlat** (sürüklenebilir widget), **Uygulamalar**, **Açık pencereler**, **Son kullanılanlar**, **Ara ve aç**, **Komutlar**, **Ekran yakalama**, **Geri Dönüşüm Kutusu**, **Masaüstünü göster** |

---

## Hızlı Başlangıç ve Kaynaktan Derleme

### Gereksinimler
- **İşletim Sistemi:** Windows 10 (1607+) veya Windows 11 (x64)
- **Derleme Araçları:** Visual Studio veya Visual Studio Build Tools (**Desktop development with C++** iş yükü + C++/WinRT başlıklarını içeren Windows SDK)

### Derleme ve Çalıştırma

```powershell
git clone https://github.com/kbudak/Vetra.git
cd Vetra
./build.ps1
```

Derleme `build\Vetra.exe` dosyasını üretir. Ek kurulum veya DLL kopyalaması gerekmez; doğrudan `build\Vetra.exe` dosyasını çalıştırman yeterlidir.

> İstersen **Ayarlar → Genel** üzerinden **Windows ile başlat** seçeneğini açabilirsin.

---

## Temel Etkileşimler ve Kısayollar

| Etkileşim | Davranış |
| --- | --- |
| **Sol tık (V simgesi)** | Vetra ayar panelini (Studio) açar |
| **Sağ tık (dock boş alan)** | Dock menüsünü açar (**Dock’u düzenle**, **Widget’lar…**, **Ayarlar**, **Görev Yöneticisi**, **Vetra’yı kapat**) |
| **Sağ tık (uygulama simgesi)** | Uygulamanın Windows Jump List görevlerini, sabitleme ve pencere kapatma menüsünü açar |
| **Basılı tutup yatay sürükleme** | Widget’ları, sabit uygulamaları ve açık pencereleri yeniden sıralar (**Esc** iptal eder) |
| **V simgesinden veya dikey dışarı sürükleme** | Dock’u üst/alt kenara ve sol/orta/sağ hizalamaya taşır |
| **Fare tekerleği** | Ses/Cam tonu kaydırıcılarını ayarlar; Katalog, Jump List, Chrome önizlemeleri ve listeleri kaydırır |
| **Tab / Enter / Esc / F5** | Açık panelde gezinir, seçer, kapatır / üst menüye döner; Gizli simgeler ve Chrome önizlemelerinde yeniler |
| **`^` ve `\|` (sağ uç)** | Gizli bildirim simgelerini açar / Masaüstünü gösterir |

---

## Yapay Zekâ Kota Beslemesi ve İsteğe Bağlı API Anahtarları

- **Otomatik yerel kaynaklar:**
  - **ChatGPT / Codex:** Resmi `codex app-server` JSON-RPC arayüzü (60 sn) + `~/.codex/sessions` yerel yedeği.
  - **Antigravity / Gemini:** `%LOCALAPPDATA%\agy\bin` veya `PATH` üzerindeki `agy --print /usage --output-format json` (60 sn) + `~/.gemini/tmp`.
  - **Claude & OpenCode:** Etkin olduklarında `~/.claude/projects` ve `~/.local/share/opencode/opencode.db` (`mtime` kontrollü).
- **Harici JSON beslemesi (`%LOCALAPPDATA%\Vetra\ai_usage.json`):** Herhangi bir betik veya araç bu dosyayı güncellediğinde ilgili kartlar 5 saniye içinde yenilenir (`p1..3` veya `0..1` aralığında `remainingFraction1..3`, `label1..3`, Unix veya ISO-8601 `r1At..3`):
  ```json
  {
    "claude": { "p1": 74, "p2": 62, "r1At": 1795530000 },
    "cursor": { "p1": 85, "p2": 40, "label1": "Auto", "label2": "API" }
  }
  ```
- **API anahtarları:** Twelve Data (`VETRA_TWELVE_DATA_KEY` veya panel), DeepSeek, Stripe ve DataFast anahtarları Windows Kimlik Bilgisi Yöneticisi’nde güvenle saklanır; canlı verisi olmayan kartlar asla sahte örnek veri göstermez (`—`).

---

## Veri Saklama, Performans ve Bilinen Sınırlar

- **Ayarlar ve yerel veriler:** `%LOCALAPPDATA%\Vetra\dock.dat` dosyasında saklanır (eski `%LOCALAPPDATA%\PrismDock\dock.dat` varsa ilk açılışta okunur).
- **Performans mimarisi:** Direct2D G2 yuvarlatılmış dikdörtgen, yay, sparkline, grafik ve cam kenarı fırça önbellekleri; `SetWinEventHook` olay tabanlı pencere takibi; kalıcı PDH sıcaklık ve `% Processor Utility` sayaçları; arka planda geciktirilmiş başlangıç (`Dock::initialize()` ~86 ms, kararlı 3 sn boşta CPU ~16 ms).
- **Bilinen Windows sınırları:**
  - **Yükseltilmiş (UIPI korumalı) sistem tepsisi simgeleri** (ör. NVIDIA, Monster) için güvenlik sınırı nedeniyle yerel hızlı menü veya uygulama kısayolu sunulur.
  - **Windows 11 24H2+ canlı Wi-Fi taraması** için Windows Konum izni açık olmalıdır; kapalıysa panel bunu bildirir ve kayıtlı profilleri ayrı gösterir.
  - **WhatsApp bildirim sayısı** paket kimliği ve Windows bildirim izni gerektirir.
  - **Takvim, Satış ve Büyüme** kayıtları yerelde tutulur; bulut senkronizasyonu yoktur.

---

## Proje Yapısı

| Dosya | Sorumluluk |
| --- | --- |
| `main.cpp` | Win32 pencereleri, çoklu monitör yerleşimi, animasyon döngüsü, görünürlük ve giriş olayları |
| `canvas.cpp`, `canvas.h` | Direct2D / DirectWrite çizim motoru ve geometri/fırça/metin/ikon önbellekleri |
| `views.cpp`, `extra_views.cpp` | Dock şeridi, ayar paneli (Studio), widget kataloğu ve Denetim Merkezi görünümleri |
| `tiles.cpp`, `design.h` | Ortak tasarım ölçüleri ve dock/katalog widget kartları |
| `pro_widgets.cpp`, `reference_widgets.cpp` | Yapay zekâ limit/kullanım, PDH CPU Utility & sıcaklık, analiz ve referans widget’ları |
| `app_tools.cpp`, `jump_list.cpp` | Kurulu uygulama listesi, sürükle-bırak sabitleme ve evrensel Windows Jump List okuyucu/çalıştırıcı |
| `system_tray.cpp` | Gizli bildirim simgeleri HD yakalama, sıfır titremeli sağ tık menü okuma ve komut yürütme |
| `shell_taskbar.cpp`, `shell_taskbar.h` | Windows görev çubuğu gizleme/geri yükleme izleyicisi ve masaüstü dostu `Yer ayır` çalışma alanı |
| `widgets.cpp`, `widgets.h`, `radios.cpp` | Açık pencere/ikon yönetimi, Core Audio ses, ağ, Wi-Fi, Bluetooth ve radyo kontrolleri |
| `window_previews.cpp` | Chrome pencerelerinin hover önizleme paneli |
| `data_feeds.cpp`, `productivity.cpp`, `timers.cpp` | Hava durumu, piyasa beslemeleri, gün çizelgesi, döviz, WhatsApp ve Pomodoro/sayaç motoru |
| `model.cpp`, `model.h`, `editor.cpp` | Profil, ayar ve yerel kayıtların kalıcı saklanması ile düzenleme formları |
| `build.ps1`, `Vetra.rc`, `Vetra.manifest`, `Vetra.ico` | MSVC x64 C++20 release derleme betiği ve uygulama kaynakları |

---

## Değişiklik geçmişi

### 24 Kasım 2026

- **Gerçek CPU sıcaklığı ve Görev Yöneticisi uyumlu CPU % ölçümü** (`pro_widgets.cpp`, `main.cpp`, `widgets.h`):
  - `PersistentPdhTemp::readC()` içindeki ilk çağrı tampon boyutu hatası giderilerek canlı PDH termal sensör okuması düzeltildi; sensör yokken sahte `58°` yeşil yay çizilmesi kaldırıldı ve `proLocalRequest` tamamlandığında `AsyncWake` ile anında yenileme eklendi.
  - Düzensiz `GetSystemTimes()` yerine Windows 10/11 Görev Yöneticisi’nin kullandığı kalıcı PDH `\Processor Information(_Total)\% Processor Utility` sayacına geçildi (ilk PDH hazırlığı arka planda yapılır, `>= 500 ms` koruma ile 2 sn aralıkla güncellenir).
  - `./build.ps1` sıfır uyarılı derleme ve `build\opt-reserved-qa\run.py` (**0 hata**: `Dock::initialize()` `85,89 ms`, `cpuTempC = 65 °C`, `cpu = %68`, `ram = %71`, 3 sn boşta CPU `16 ms`) ile doğrulandı; Görev Yöneticisi ile canlı karşılaştırma kullanıcı oturumunda doğrulamaya bırakıldı.
- **Yer ayır masaüstü simge düzeni koruması ve kapsamlı performans optimizasyonu** (`shell_taskbar.h`, `shell_taskbar.cpp`, `canvas.cpp`, `pro_widgets.cpp`, `widgets.cpp`, `main.cpp`, `tiles.cpp`, `jump_list.cpp`, `dock.h`, `build.ps1`):
  - `ScreenReservation`, Explorer’ın masaüstü `IconLayouts` düzenini karıştıran `SHAppBarMessage(ABM_SETPOS)` ve `SPIF_SENDCHANGE` yayını yerine sessiz `SystemParametersInfoW(SPI_SETWORKAREA, 0, &newWork, 0)` + yalnızca büyütülmüş uygulama pencerelerine hedefli hizalama kullanacak şekilde yenilendi (`verify()` korumasıyla).
  - `requestWindowIcons()` içindeki `std::move(batch)` sonrası `win.iconChecked` işaretlenememe hatası, `requestAppImages()` ikonsuz kısayol sonsuz döngüsü ve `WarmupTick` üçlü `shell:AppsFolder` taraması giderildi.
  - `Draw::arc`, `Draw::sparkline`, `Draw::graph`, `PlaySolid`/`SkipSolid` ve Gemini yıldız geometrileri önbelleğe alındı; `getRimBrush` 64 kademeye çıkarıldı; kalıcı PDH sıcaklık sorgusu, `mtime+size` önbellekli `ai_usage.json`/`opencode.db`, seçici 30 sn AI log taraması, etkin pencere hızlı yollu tam ekran algılama, `≤2%` CPU/RAM yay atlama, `≤90 Hz` tam-bölenli yazılım D2D kare temposu, sıfır `Shortcut` kopyalı `drawAppSlot` ve `/O2 /Oi /Ot /GL /Gw /Gy /favor:AMD64` + `/LTCG /OPT:REF /OPT:ICF` derleme bayrakları uygulandı.
  - Gerçek Chrome penceresiyle sessiz `SPI_SETWORKAREA` testi (`1440 → 1368`, `IsZoomed = 1`) ve `build\opt-reserved-qa\run.py` (0 hata: `0` masaüstü yayını, `paintBar()` `4,83 ms/kare`, 3 sn boşta CPU `1500 ms → 16 ms`) ile doğrulandı.
- **Evrensel Windows Atlama Listesi (Jump List) ve pencere kapatma desteği** (`jump_list.cpp`, `app_tools.cpp`, `widgets.cpp`, `widgets.h`, `dock.h`, `main.cpp`, `build.ps1`):
  - Uygulama sağ tık menüsü (`AppMenu`), `.customDestinations-ms` ve `.automaticDestinations-ms` dosyalarını uygulama bazlı sabit kodlama olmadan evrensel olarak okur (CRC-64 `%llx` AppID, açık pencere/Başlat Menüsü/`.lnk`/`shell:AppsFolder`/KnownFolder/Electron çözümlemesi, Win32 & MSIX/UWP özellikleri, `.tmp`/`.ico`/kaynak simgeleri, fare tekerleği kaydırma ve **Pencereyi kapat / Tüm pencereleri kapat**).
  - `build\jumplist-qa\run.py` (**0 hata**: Chrome, Outlook, ChatGPT/Codex, Antigravity IDE, VS Code, Dosya Gezgini, Windows Terminal) ve `build\tray-menu-qa\run.py` (11/11 PASS) ile doğrulandı.
- **Gizli simgeler akıcı açılış, hızlı başlangıç ve sabit olmayan uygulama düzeltmeleri** (`system_tray.cpp`, `main.cpp`, `widgets.cpp`, `app_tools.cpp`, `shell_taskbar.cpp`):
  - Gizli simge yakalama ve ikon çözümlemesi arka plana taşındı; başlangıçtaki engelleyici işlemler ertelendi; sabit olmayan pencereler `SetWinEventHook` (~35 ms) ve `pendingPosition` ile anlık izlenecek şekilde düzeltildi (`build\perf-unpinned-qa\run.py` ve `build\tray-menu-qa\run.py` ile doğrulandı).

<details>
<summary>Önceki sürüm geçmişi (17 – 23 Kasım 2026)</summary>

### 23 Kasım 2026
- **Yer ayır** görünürlük modu, tüm bağlı monitörlerde eşzamanlı dock, sıfır titremeli HD **Gizli simgeler** sağ tık menüsü (Win32 `#32768`, WinUI 3, Electron, WebView2, WPF, Qt), **Medya Kompakt/Mini/Çubuk**, **Mikrofon**, **Hızlı Kontroller**, **AI Limitleri · Üçlü**, Antigravity CLI kota entegrasyonu, **Gün çizelgesi**, **WhatsApp**, **Döviz**, sürükle-bırak uygulama sabitleme ve ikon boşluk normalizasyonu eklendi.

### 20 Kasım 2026
- **Başlat** widget’ı, A2DP+HFP `ContainerId` eşleştirmeli hızlı Bluetooth kulaklık ses yönlendirmesi, sağ uç **Masaüstünü göster** şeridi (`|`), 16 kartlı **Yapay zekâ** ve analiz kategorisi (65+ widget), **Denetim Merkezi** canlı SMTC medya kartı ve panel içi Wi-Fi/Bluetooth/Yansıt/Erişilebilirlik/Yakın paylaşım/Ses çıkışı görünümleri, saf siyah **Çerçevesiz** tema ve QPC tabanlı animasyon motoru eklendi.

### 19 Kasım 2026
- Birleştirilmiş widget + uygulama + açık pencere sürükle-bırak sıralaması, yenilenen Liquid Glass cam materyali, dock üzerinde doğrudan sürüklenebilir **Ses** çubuğu, minimalist renk vurguları, Chrome çoklu pencere hover önizlemesi ve 7 günlük ekran süresi geçmişi eklendi.

### 18 Kasım 2026
- Uygulama adı **Vetra** olarak güncellendi (`Vetra.exe`, Titanium Pearl ikon); tema uyumlu Gizli simgeler paneli, odak/sayaç süre çerçeveleri, Direct2D/DirectWrite çizim altyapısı, 32-bit HD uygulama ikonları, görev çubuğu koruma izleyicisi ve 29–42 widget’a genişleyen katalog eklendi.

### 17 Kasım 2026
- İlk çalışma alanı rezervasyonu, çoklu tema temeli, DWM cam görünümü, Direct2D/DirectWrite metin çizimi, **Dock’u düzenle** modu ve temel widget/katalog mimarisi oluşturuldu.

</details>
