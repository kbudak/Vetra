# Prism Dock proje yönergeleri

Kullanıcıya görünen bir özellik, ayar, kısayol veya kullanım akışı değiştiğinde aynı çalışma içinde `README.md` dosyasını güncelle.

- İlgili kullanım veya özellik bölümünü yeni davranışa göre düzenle.
- **Değişiklik geçmişi** bölümüne en yeni tarihli kaydı üste ekle. Aynı tarihte yapılan ilgili değişiklikleri mevcut başlık altında birleştir.
- Derleme komutu, gereksinimler veya proje yapısı değişirse ilgili bölümü de güncelle.
- Test edilmemiş bir akışı doğrulanmış olarak yazma. Kullanımı etkileyen bilinen sınırları belirt.

## Geliştirme ve derleme kuralları

- Vetra çalışırken Windows görev çubuğu gizlenir (`model.hideTaskbar` varsayılan olarak açıktır; kullanıcı isterse **Ayarlar → Görünüm** üzerinden kapatabilir). Vetra kapandığında Windows görev çubuğu Vetra başlamadan önceki son ayarına (`Vetra.OrigTaskbarState.v1` / `originalState`) eksiksiz geri dönmelidir — asla gizli veya otomatik gizle (`ABS_AUTOHIDE`) durumunda bırakılmamalıdır.
- Mevcut `Vetra.exe` örneğini derleme öncesi kapatırken `--restore-taskbar` izleyici sürecini asla zorla öldürme; izleyicinin görev çubuğunu geri yükleyip kendiliğinden kapanmasını bekle.
- Uygulamayı güncelledikten ve derledikten sonra `build\Vetra.exe` dosyasını kapalı bırakma; derleme sonrası kesinlikle yeniden başlat (`./build.ps1` varsayılan olarak derleme bitince `build\Vetra.exe` dosyasını başlatır). Eğer ortam yalıtımı nedeniyle başlatılan pencere kullanıcının etkileşimli masaüstüne yansımıyorsa bunu açıkça belirt.
- Git commit/push işlemlerinde kullanıcının kişisel verilerini, yerel ayarlarını (`dock.dat`, `ai_usage.json`, API anahtarları, ekran görüntüleri, `build/` çıktıları veya kişisel yol/kimlik bilgileri) asla dahil etme; her push öncesi değişen dosyaları kontrol et.
