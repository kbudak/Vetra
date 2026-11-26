<div align="center">

# Vetra

**Windows 10 & 11 için yerel, ultra hafif ve kişiselleştirilebilir masaüstü dock’u.**

[![Platform](https://img.shields.io/badge/Windows-10%20%2F%2011%20(x64)-0078D6?style=flat-square&logo=windows)](https://github.com/kbudak/Vetra)
[![Language](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus)](https://github.com/kbudak/Vetra)
[![Graphics](https://img.shields.io/badge/UI-Win32%20%2B%20Direct2D%20%2B%20DirectWrite-111827?style=flat-square)](https://github.com/kbudak/Vetra)
[![Widgets](https://img.shields.io/badge/Widget%20Katalo%C4%9Fu-84%20Widget-2FD36B?style=flat-square)](https://github.com/kbudak/Vetra)
[![Portable](https://img.shields.io/badge/Kurulum-Tek%20EXE%20%C2%B7%20Ta%C5%9F%C4%B1nabilir-8B5CF6?style=flat-square)](https://github.com/kbudak/Vetra)

</div>

---

## Vetra Nedir?

**Vetra**, Windows’un varsayılan görev çubuğunun yerini alabilen, tamamen yerel **C++20 / Win32 / Direct2D / DirectWrite** ile yazılmış modern bir masaüstü dock’udur. Electron, WebView veya harici bir çalışma zamanı gerektirmez; tek bir taşınabilir `Vetra.exe` (~2,3 MB) olarak çalışır, yönetici izni veya kurulum istemez.

Sadece uygulama başlatmakla kalmaz; **84 modüler widget**, **yerel Denetim Merkezi**, **HD Gizli Simgeler ve panel içi sağ tık menüsü**, **evrensel Windows Atlama Listeleri (Jump Lists)**, **canlı CPU/RAM/sıcaklık telemetrisi** ve **yapay zekâ kota takibi** ile masaüstünü tek bir akıcı şeritte toplar.

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
- **Güvenli görev çubuğu yönetimi:** Vetra çalışırken Windows görev çubuğunu gizler (isteğe bağlı olarak **Görünüm → Windows görev çubuğunu gizle** üzerinden kapatılabilir). Normal çıkışta veya beklenmedik kapanmada izleyici süreç (`--restore-taskbar`) görev çubuğunu Vetra açılmadan önceki son Windows ayarına (`ABS_AUTOHIDE` durumu dahil) otomatik olarak geri yükler.

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
- **Tüm uygulamalar için canlı pencere önizlemesi:** İmleci dock üzerindeki herhangi bir açık uygulama simgesinin (Chrome, Netflix, Outlook, Dosya Gezgini, Claude, ChatGPT, Antigravity veya sabitlenmemiş pencereler) üzerinde bekletince küçültülmüş olanlar dahil tüm pencereleri canlı önizleme kartları ve kart üzeri kapatma (`×`) düğmesiyle listelenir; uygulamalar arasında gezerken önizleme gecikmesiz geçiş yapar. Küçültülen pencereler son yakalanan önizlemeyi önbellekten korur; tek pencereli uygulamalara tıklamak anında odak/küçültme yapar, çok pencereli uygulamalarda seçim panelini açar. Chrome pencereleri açılma sırasına göre soldan sağa dizilir.

### Yerel Denetim Merkezi, Windows Durum Alanı ve Sıfır Titremeli Gizli Simgeler
- **Windows Durum Alanı:** Mikrofon, klavye dili, ağ, ses, pil, saat/tarih ve bildirim zili ayrı dock öğeleridir. Göstergeler, Gizli simgeler, Denetim Merkezi ve Masaüstü şeridi dock’un ortak yüzeyinde çizilir. **Dock’u düzenle** modunda bunları widget’ların ve uygulamaların arasına sürükleyebilir, kırmızı **−** ile kaldırabilir, **+ → Widget kataloğu** ile yeniden ekleyebilirsin. **Ayarlar → Durum alanı** sekmesindeki anahtarlar etkin profildeki aynı öğeleri yönetir.
  - **Mikrofon göstergesi:** Bir uygulama mikrofonu aktif kullandığında canlı turkuaz renge ve nokta işaretine döner, susturulduğunda kırmızı çarpılı simge gösterir. Sol tık **Mikrofon** panelini (aktif kayıt yapan uygulama adı, hızlı susturma, giriş aygıtı seçici ve gizlilik/ses ayarları), sağ tık anlık susturmayı çalıştırır. İsteğe bağlı olarak **yalnızca kullanımda göster** modu açılabilir.
  - **Klavye dili (`TR`, `EN` vb.):** Etkin pencerenin klavye düzenini canlı takip eder (aynı dilde birden fazla düzen varsa `Q`/`F`/`US` alt etiketiyle). Sol tık yüklü düzenleri listeleyen **Klavye dili** seçici panelini açar; sağ tık veya fare tekerleği bir sonraki düzene anında geçer.
  - **İnternet bağlantısı, Ses ve Pil:** Keskinleştirilmiş Ethernet (kablolu monitör/fiş), Wi-Fi, uçak modu veya çevrimdışı durum simgesi tıklanınca yalnızca **İnternet** panelini (Ethernet durumu, Wi-Fi anahtarı ve yakındaki/kayıtlı ağlar); ses simgesi tıklanınca yalnızca **Ses** panelini (canlı sürüklenebilir/tekerlekli ses kaydırıcısı ve çıkış aygıtı seçici; sağ tık susturma, dock üzerinde fare tekerleğiyle ±%4 ses ayarı); pil simgesi tıklanınca **Pil ve güç** panelini açar.
  - **Saat, tarih ve yerel Bildirim Merkezi:** İki satırlı Windows saat/tarih bloğu ve canlı rozetli bildirim zil düğmesi tıklanınca Windows’un kendi paneli yerine **Vetra’nın yerel Bildirim Merkezi** açılır: canlı Windows bildirimleri (`wpndatabase.db` + uygulama simgeleri, tek tek kapatma ve **Tümünü temizle**), **Rahatsız Etmeyin** modu anahtarı (`WNF`), daraltılabilir **Mini Takvim** ve doğrudan Vetra Pomodoro motoruna bağlı **Odaklanma** oturumu (`− / + / ▶ Odaklanma`). Sağ tık ilgili Windows ayarlarını açar.
- **Denetim Merkezi:** Dock’a bitişik iki sayfalı panelde **Wi-Fi** (canlı ağ taraması ve kayıtlı profiller), **Bluetooth** (A2DP Stereo + HFP `ContainerId` eşleştirmeli doğrudan ses bağlantısı ve varsayılan çıkış geçişi), **Uçak modu**, **Enerji tasarrufu**, **Gece ışığı**, **Mobil etkin nokta**, **Yansıt** (çoklu ekran düzenleri), **Erişilebilirlik** (7 yerel anahtar), **Yakın paylaşım**, **Ses çıkışı seçici** ve **canlı Medya kartı** (Windows SMTC oynat/duraklat, ileri/geri veya ±10 sn sarma) tek panelde çalışır.
- **Gizli simgeler (`^`):** Yüksek çözünürlüklü, simge sayısına göre dinamik genişleyen ızgara (`NotifyIconSettings` PNG, MSIX ve `.exe` kaynakları); hem taşma penceresindeki (kaydırılabilir satırlar dahil) hem de Windows görev çubuğuna sabitlenmiş tüm bildirim simgelerini birlikte toplar. Sağ tık, taşma penceresini ekran dışından tetikleyip açılan yerel menüyü (`#32768`, çok seviyeli alt menüler, WinUI 3, Electron, WebView2, WPF, Qt) görünmez biçimde yakalayıp kapandıktan sonra serbest bırakır ve komutu Vetra temasında çalıştırır.
- **Masaüstünü göster (`|`):** Katalogdaki **Masaüstü şeridi** tüm açık pencereleri aşağı alır veya geri getirir. **Gizli simgeler** düğmesiyle birlikte dock’ta taşınabilir, kaldırılabilir ve yeniden eklenebilir.

---

## 84 Widget’lık Katalog ve Profiller

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

Derleme `build\Vetra.exe` dosyasını üretir ve varsayılan olarak uygulamayı otomatik yeniden başlatır (`./build.ps1 -NoLaunch` yalnızca derleme yapar). Ek kurulum veya DLL kopyalaması gerekmez.

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
| **Fare tekerleği** | Ses/Cam tonu kaydırıcılarını, durum alanı ses simgesini ve klavye dilini ayarlar; Katalog, Jump List, Chrome önizlemeleri ve listeleri kaydırır |
| **Tab / Enter / Esc / F5** | Açık panelde gezinir, seçer, kapatır / üst menüye döner; Gizli simgeler ve Chrome önizlemelerinde yeniler |
| **`^`, Durum alanı ve `\|`** | Gizli bildirim simgelerini açar / mikrofon, klavye dili, ağ, ses, pil, saat ve bildirimleri yönetir / Masaüstünü gösterir |

---

## Yapay Zekâ Kota Beslemesi ve İsteğe Bağlı API Anahtarları

- **Otomatik yerel kaynaklar:**
  - **Claude:** Claude Desktop kullanım geçmişi (`plan-usage-history.json`, MSIX ve masaüstü kurulumları, 5 sn `mtime` kontrollü), Claude Code OAuth kullanım API’si (`~/.claude/.credentials.json` geçerliyse `api.anthropic.com/api/oauth/usage`, 5 dk + 429 geri çekilmesi) ve yerel `~/.claude/projects` token yedeği.
  - **ChatGPT / Codex:** Resmi `codex app-server` JSON-RPC arayüzü (60 sn) + `~/.codex/sessions` yerel yedeği.
  - **Antigravity / Gemini:** `%LOCALAPPDATA%\agy\bin` veya `PATH` üzerindeki `agy --print /usage --output-format json` (60 sn) + `~/.gemini/tmp`.
  - **OpenCode:** Etkin olduğunda `~/.local/share/opencode/opencode.db` (`mtime` kontrollü).
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
  - **Yükseltilmiş ön plan pencerelerinde klavye dili değiştirme** sırasında Windows UIPI sınırı `WM_INPUTLANGCHANGEREQUEST` iletisini engellerse Vetra otomatik olarak `Win + Boşluk` geçişine düşer.
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

### 2026-11-26
- **Tüm uygulamalar için hover pencere önizlemesi:** Canlı pencere önizleme sistemi yalnızca Chrome yerine dock üzerindeki tüm açık uygulamalara (Netflix, Outlook, Dosya Gezgini, Claude, ChatGPT, Antigravity ve sabitlenmemiş tüm pencereler) genelleştirildi. Önizleme açıkken başka bir açık uygulamanın üzerine gelindiğinde beklemeden anında o uygulamaya geçer; yakalanan önizlemeler önbellekte tutularak küçültülmüş pencerelerde de gösterilir, koyu arayüzlü ve UWP/Win32 pencereler için çift aşamalı `PrintWindow` + şık simge yedeği kullanılır ve her önizleme kartına pencere kapatma (`×`) düğmesi eklendi.
- **Hızlı pencere küçültme ve pencere boyutu koruma:** Dock üzerindeki etkin bir uygulamaya tekrar tıklandığında gecikmeli `ShowWindowAsync(SW_MINIMIZE)` yerine doğrudan `WM_SYSCOMMAND (SC_MINIMIZE)` ile anında küçültülür ve dock etkin göstergesi beklemeden güncellenir (UWP/sahip olunan pencereler ve tek pencereli Chrome doğrudan tıklama geçişi dahil). Küçültülmüş pencereleri geri getirirken `ShowWindow(SW_RESTORE)` ve `SwitchToThisWindow` kaynaklı Snap/boyut bozulmaları giderildi: artık `WM_SYSCOMMAND (SC_RESTORE)` kullanılır, küçültme öncesi konum/boyut doğrulama korumasıyla Chrome ve diğer uygulamalar açıldıkları boyutta kalır, odak yedeği ise pencere boyutuna dokunmadan (`SWP_NOMOVE | SWP_NOSIZE`) çalışır.
- **AI Limitleri tek kartta üçlü görünüm ve Claude canlı takibi:** **AI Limitleri** kartına ve paneline **Claude + Codex + Antigravity** (üçü birlikte) modu eklendi ve varsayılan yapıldı (`dock.dat` içinde `aiPair` satırında saklanır); kullanıcı panelden dilediği zaman ikili görünümlere veya **Diğer planlar** seçimine dönebilir. Claude kartları (AI Limitleri ikili/üçlü, Claude Kullanımı ve Claude Plan) artık yerel token sayacı (`0 t`) yerine Codex ile aynı biçimde kalan yüzde ve renkli ilerleme çubuğu gösterir; Claude Desktop’ın `plan-usage-history.json` (MSIX ve standart kurulumlar, 5 sn `mtime` kontrollü) dosyası ile geçerli oturum varsa Claude Code OAuth kullanım uç noktası (`api.anthropic.com/api/oauth/usage`, 429 geri çekilmeli) otomatik okunur, ikisi de yoksa `~/.claude/projects` token yedeğine düşer.
- **Gizli simgeler eksiksiz listeleme ve genişleyen ızgara:** Sistem tepsisi yakalaması artık XAML taşma penceresinin yerleşimini bekler, kaydırılabilir taşma satırlarını tarar, boş araç ipuçlu simgeleri kaybetmez ve Windows görev çubuğuna sabitlenmiş bildirim simgelerini de listeye ekler. Sabit 5 sütun / 4 satır sınırı kaldırılarak simge sayısına göre ekran sınırları içinde yatay ve dikey olarak dinamik büyüyen ızgara düzenine geçildi.
- **Sağ tık yerel menü titremesi giderildi:** Taşma penceresini ekran dışına sabit tutup önce klavye bağlam menüsü (`VK_APPS`) ile tetikleme, açılan yerel menü ve `SysShadow` pencerelerini `EVENT_OBJECT_CREATE`/`SHOW` anında ve kare-altı taramayla `alpha=0` + ekran dışına alma (`WS_EX_NOREDIRECTIONBITMAP` pencereleri dahil), kapatma tamamlanmadan gizlemeyi asla kaldırmama (ertelenmiş geri yükleme) ve geçici görev çubuğu erişimini saydam yapma mekanizmaları eklendi.

### 2026-11-25
- **Dock akıcılığı:** Mikrofon durumu ve kayıt oturumları arka planda okunur. Tekrarlanan metin yerleşimleri önbellekte tutulur ve ölçer animasyonları sırasında yalnızca ilgili ölçerin göründüğü dock’lar yeniden çizilir. Sürükle-bırak hedefi tek geçişte hesaplanır.
- **Dock göstergelerinin görünümü:** Durum simgeleri, **Denetim Merkezi**, **Gizli simgeler** ve **Masaüstü şeridi** doğrudan ortak dock yüzeyinde çizilir. Üzerine gelme ve açık panel durumunda simge rengi değişir.
- **Chrome önizleme sırası:** Vetra çalışırken açılan Chrome pencereleri açılış sırasına göre sağa eklenir. Öne alınan, küçültülen veya geçici olarak gizlenip dönen pencereler yerlerini korur. Vetra’dan önce açık pencerelerin tam açılış sırası Windows tarafından sağlanmadığı için ilk sıra başlangıç anındaki dizilimden belirlenir.
- **Taşınabilir durum öğeleri:** Yedi durum göstergesi, **Gizli simgeler** ve **Masaüstü şeridi**, profilde saklanan dokuz ayrı katalog öğesine dönüştürüldü. Mevcut durum alanı anahtarları ve mikrofonun kullanımda görünmesi ayarı eski profillerden aktarılır. Sığmayan etkin öğeler **…** menüsünden açılır.
- **Windows Durum Alanı dock’a eklendi:** Dock’un sağ ucuna (Gizli simgeler `^` ile Masaüstünü göster `|` arasına) **Mikrofon** (aktif kayıt yapan uygulamayı Core Audio `IAudioSessionManager2` ile canlı algılama ve turkuaz gösterge), **Klavye dili** (`TR`/`EN` canlı takip ve yerel seçici panel), **İnternet bağlantısı** (Ethernet / Wi-Fi / Uçak modu / Çevrimdışı), **Ses seviyesi** (sağ tık susturma + fare tekerleği), **Pil ve şarj durumu** (canlı yeşil şarj simgesi ve pil yoksa otomatik gizleme), **Saat ve tarih** (iki satırlı Windows biçimi) ve **Bildirimler** (`Win + N`) eklendi.
- **Geliştirilmiş Mikrofon ve yeni Klavye dili panelleri:** Mikrofon paneline aktif kayıt yapan uygulama bilgisi, giriş aygıtı (`eCapture`) seçici ve Windows mikrofon gizlilik ayarları kısayolu; Klavye dili paneline yüklü Windows klavye düzenleri listesi ve tek tık geçiş eklendi.
- **Ayarlar → Durum alanı sekmesi:** Durum alanının genel anahtarı, tüm öğelerin tek tek açılıp kapatılması ve **Mikrofonu yalnızca kullanımda göster** seçeneği eklenip `dock.dat` içine kalıcı olarak bağlandı.
- **Güvenli görev çubuğu geri yükleme ve derleme akışı:** Vetra çalışırken Windows görev çubuğunu gizler (isteğe bağlı olarak **Ayarlar → Görünüm → Windows görev çubuğunu gizle** ile kapatılabilir); kapandığında veya zorla sonlandırıldığında görev çubuğunu Vetra açılmadan önceki son ayarına (`Vetra.OrigTaskbarState.v1` + iptal olaylı `--restore-taskbar` izleyicisi) yarış durumu olmadan geri döndürür. `build.ps1` derleme öncesi yalnızca ana süreci kapatıp izleyicinin geri yüklemeyi bitirmesini bekler ve derleme sonunda `build\Vetra.exe` dosyasını otomatik yeniden başlatır.
- **Vetra yerel Bildirim Merkezi, ayrı İnternet ve Ses panelleri:** Durum alanındaki bildirim ziline, saat/tarih bloğuna veya Denetim Merkezi’ndeki **Bildirimler** kutucuğuna tıklandığında Windows’un yerel paneli yerine Vetra temalı **Bildirim Merkezi** (`wpndatabase.db`, Rahatsız Etmeyin, Mini Takvim, Odaklanma çubuğu) açılır. Durum alanındaki **İnternet** simgesi artık yalnızca özel **İnternet** panelini (Ethernet/Wi-Fi), **Ses** simgesi ise yalnızca sürüklenebilir kaydırıcılı ve çıkış aygıtı seçicili özel **Ses** panelini açar; internet ve hoparlör vektör simgeleri daha net çizilecek şekilde yenilendi.
