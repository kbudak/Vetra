#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <powrprof.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <wlanapi.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <numeric>
#include <sstream>
#include <unordered_map>
#include "dock.h"

using Microsoft::WRL::ComPtr;

Text durationText(ULONGLONG seconds) {
    Text tail = two(static_cast<int>(seconds / 60 % 60)) + L":" + two(static_cast<int>(seconds % 60));
    return seconds >= 3600 ? num(seconds / 3600) + L":" + tail : tail;
}
Text rateText(long long bytes) {
    return bytes >= 1024 * 1024 ? num(bytes / (1024 * 1024)) + L"," + num((bytes / (1024 * 1024 / 10)) % 10) + L" MB/s" :
        num(bytes / 1024) + L" KB/s";
}
Text spaceText(ULONGLONG bytes) {
    constexpr ULONGLONG GiB = 1024ULL * 1024 * 1024;
    return bytes >= GiB * 1024 ? num(bytes / (GiB * 1024)) + L"," + num((bytes / (GiB * 1024 / 10)) % 10) + L" TB" :
        num(bytes / GiB) + L" GB";
}
Text usageText(int seconds) {
    return seconds >= 3600 ? num(seconds / 3600) + L" sa " + num(seconds / 60 % 60) + L" dk" : num(seconds / 60) + L" dk";
}
Text weatherText(int code) {
    if (code == 0) return L"Açık";
    if (code < 3) return L"Parçalı bulutlu";
    if (code == 3) return L"Bulutlu";
    if (code < 50) return L"Sisli";
    if (code < 60) return L"Çisenti";
    if (code >= 95) return L"Gök gürültülü";
    if ((code >= 71 && code <= 77) || code >= 85) return L"Kar";
    return L"Yağmurlu";
}
static std::vector<Disk> localDisks() {
    wchar_t drives[512]{};
    DWORD size = GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    std::vector<Disk> result;
    if (!size || size >= ARRAYSIZE(drives)) return result;
    for (auto root = drives; *root; root += wcslen(root) + 1) {
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        ULARGE_INTEGER free{}, total{};
        if (GetDiskFreeSpaceExW(root, &free, &total, nullptr)) result.push_back({root, free.QuadPart, total.QuadPart});
    }
    return result;
}
static BOOL CALLBACK enumerateWindows(HWND window, LPARAM value) {
    auto& windows = *reinterpret_cast<std::vector<OpenWindow>*>(value);
    if (windows.size() >= 40) return FALSE;
    if (!IsWindowVisible(window) || !GetWindowTextLengthW(window)) return TRUE;
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    const auto styles = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((styles & WS_EX_TOOLWINDOW) || (GetWindow(window, GW_OWNER) && !(styles & WS_EX_APPWINDOW))) return TRUE;
    DWORD cloaked{};
    DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    wchar_t cls[80]{};
    GetClassNameW(window, cls, ARRAYSIZE(cls));
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
        wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
        wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
        wcscmp(cls, L"NotifyIconOverflowWindow") == 0 ||
        wcscmp(cls, L"ControlCenterWindow") == 0)
        return TRUE;
    wchar_t title[256]{};
    if (GetWindowTextW(window, title, ARRAYSIZE(title)) && wcscmp(title, L"Program Manager") != 0 &&
        wcscmp(title, L"Windows Input Experience") != 0 && wcscmp(title, L"Microsoft Text Input Application") != 0)
        windows.push_back({window, title, nullptr, false, {}});
    return TRUE;
}

static std::unique_ptr<Gdiplus::Bitmap> scaleBitmapHighQuality(Gdiplus::Bitmap* src, int targetSize = 64) {
    if (!src || src->GetWidth() == 0 || src->GetHeight() == 0) return nullptr;
    if (static_cast<int>(src->GetWidth()) == targetSize && static_cast<int>(src->GetHeight()) == targetSize &&
        src->GetPixelFormat() == PixelFormat32bppPARGB) {
        return std::unique_ptr<Gdiplus::Bitmap>(src->Clone(0, 0, targetSize, targetSize, PixelFormat32bppPARGB));
    }
    auto dst = std::make_unique<Gdiplus::Bitmap>(targetSize, targetSize, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(dst.get());
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g.DrawImage(src, 0, 0, targetSize, targetSize);
    return dst;
}

std::unique_ptr<Gdiplus::Bitmap> normalizeAppIcon(Gdiplus::Bitmap& src) {
    const int w = static_cast<int>(src.GetWidth()), h = static_cast<int>(src.GetHeight());
    if (w <= 0 || h <= 0) return nullptr;
    Gdiplus::Rect crop(0, 0, w, h);
    Gdiplus::BitmapData data{};
    if (src.LockBits(&crop, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
        RECT strong{w, h, 0, 0}, soft{w, h, 0, 0};
        const auto include = [](RECT& bounds, LONG x, LONG y) {
            bounds.left = std::min(bounds.left, x);
            bounds.top = std::min(bounds.top, y);
            bounds.right = std::max(bounds.right, x + 1);
            bounds.bottom = std::max(bounds.bottom, y + 1);
        };
        for (int y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const DWORD*>(static_cast<const BYTE*>(data.Scan0) +
                static_cast<ptrdiff_t>(y) * data.Stride);
            for (int x = 0; x < w; ++x) {
                const DWORD alpha = row[x] >> 24;
                if (alpha >= 12) include(soft, x, y);
                if (alpha >= 96) include(strong, x, y);
            }
        }
        src.UnlockBits(&data);

        // Weak outlines and shadows can span the full asset even when the logo is tiny.
        const RECT art = IsRectEmpty(&strong) ? soft : strong;
        const LONG extent = std::max(art.right - art.left, art.bottom - art.top);
        if (!IsRectEmpty(&art) && extent < std::max(w, h) * .84f) {
            const LONG pad = std::max(1L, extent / 16);
            const LONG x = std::max(0L, art.left - pad), y = std::max(0L, art.top - pad);
            const LONG right = std::min(static_cast<LONG>(w), art.right + pad);
            const LONG bottom = std::min(static_cast<LONG>(h), art.bottom + pad);
            crop = {x, y, right - x, bottom - y};
        }
    }

    constexpr int size = 64;
    if (w == h && crop.Width == w && crop.Height == h) return scaleBitmapHighQuality(&src, size);
    auto dst = std::make_unique<Gdiplus::Bitmap>(size, size, PixelFormat32bppPARGB);
    Gdiplus::Graphics graphics(dst.get());
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    Gdiplus::ImageAttributes attributes;
    attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
    const float fit = static_cast<float>(size) / std::max(crop.Width, crop.Height);
    const float fittedWidth = crop.Width * fit, fittedHeight = crop.Height * fit;
    const Gdiplus::RectF fitted((size - fittedWidth) / 2.f, (size - fittedHeight) / 2.f, fittedWidth, fittedHeight);
    if (graphics.DrawImage(&src, fitted, static_cast<float>(crop.X), static_cast<float>(crop.Y),
        static_cast<float>(crop.Width), static_cast<float>(crop.Height), Gdiplus::UnitPixel, &attributes) != Gdiplus::Ok)
        return nullptr;
    return dst;
}

namespace {
winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession pickMediaSession(
    const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager& manager) {
    using namespace winrt::Windows::Media::Control;
    if (auto cur = manager.GetCurrentSession()) return cur;
    auto sessions = manager.GetSessions();
    GlobalSystemMediaTransportControlsSession fallback{nullptr};
    for (uint32_t i = 0; i < sessions.Size(); ++i) {
        auto s = sessions.GetAt(i);
        if (!fallback) fallback = s;
        if (s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
            return s;
    }
    return fallback;
}
}

void Dock::queryMedia() {
    if (extra.mediaRequest.valid()) return;
    extra.mediaRequest = std::async(std::launch::async, [previous = extra.media] {
        using namespace winrt::Windows::Media::Control;
        using namespace winrt::Windows::Storage::Streams;
        MediaState state;
        try {
            winrt::init_apartment();
        } catch (const winrt::hresult_error&) { return state; }
        try {
            const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            const auto session = pickMediaSession(manager);
            if (session) {
                const auto properties = session.TryGetMediaPropertiesAsync().get();
                const auto info = session.GetPlaybackInfo();
                const auto controls = info.Controls();
                state.source = session.SourceAppUserModelId().c_str();
                state.title = properties.Title().c_str();
                state.artist = properties.Artist().c_str();
                state.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                state.canPrev = controls.IsPreviousEnabled();
                state.canNext = controls.IsNextEnabled();
                state.canSeek = controls.IsPlaybackPositionEnabled();
                const auto timeline = session.GetTimelineProperties();
                const auto trackStart = timeline.StartTime().count(), trackEnd = timeline.EndTime().count();
                if (trackEnd > trackStart) {
                    state.durationTicks = trackEnd - trackStart;
                    const auto updated = timeline.LastUpdatedTime();
                    const auto since = (winrt::clock::now() - updated).count();
                    const auto advance = state.playing && updated.time_since_epoch().count() > 0
                        ? std::clamp<int64_t>(since, 0, state.durationTicks) : 0;
                    state.elapsedTicks = std::clamp<int64_t>(timeline.Position().count() - trackStart + advance, 0, state.durationTicks);
                    state.timelineStamp = GetTickCount64();
                }
                if (previous.cover && previous.source == state.source && previous.title == state.title && previous.artist == state.artist) {
                    state.cover = previous.cover;
                } else if (const auto thumbnail = properties.Thumbnail()) {
                    const auto data = thumbnail.OpenReadAsync().get();
                    const auto bytes = data.Size();
                    if (bytes > 0 && bytes <= 2 * 1024 * 1024) {
                        DataReader reader(data);
                        if (reader.LoadAsync(static_cast<uint32_t>(bytes)).get() == bytes) {
                            std::vector<uint8_t> image(static_cast<size_t>(bytes));
                            reader.ReadBytes(image);
                            ComPtr<IStream> stream;
                            ULONG written{};
                            LARGE_INTEGER start{};
                            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
                                SUCCEEDED(stream->Write(image.data(), static_cast<ULONG>(bytes), &written)) && written == bytes &&
                                SUCCEEDED(stream->Seek(start, STREAM_SEEK_SET, nullptr))) {
                                Gdiplus::Bitmap source(stream.Get());
                                state.cover = scaleBitmapHighQuality(&source, 96);
                            }
                        }
                    }
                }
            }
        } catch (const winrt::hresult_error&) {}
        winrt::uninit_apartment();
        return state;
    });
}

bool Dock::mediaInUse() const {
    if (page == Media || page == Quick) return true;
    if (!anyDockVisible()) return false;
    for (const int id : {Media, MediaCompact, MediaMini, MediaBar})
        if (shown(id) && !isFoldedWidget(id)) return true;
    return false;
}

void Dock::media(WORD vk) {
    if (extra.mediaActionRequest.valid() &&
        extra.mediaActionRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    if (vk == VK_MEDIA_PLAY_PAUSE && !extra.media.title.empty()) {
        const auto stamp = GetTickCount64();
        if (extra.media.durationTicks > 0)
            extra.media.elapsedTicks = static_cast<int64_t>(extra.media.progress(stamp) * extra.media.durationTicks);
        extra.media.timelineStamp = stamp;
        extra.media.playing = !extra.media.playing;
        if (anyDockVisible()) paintBar();
        if (panel && (page == Quick || page == Media)) paintSheet();
    }
    extra.mediaRefreshAt = GetTickCount64() + 260;
    extra.mediaActionRequest = std::async(std::launch::async, [vk]() -> bool {
        using namespace winrt::Windows::Media::Control;
        bool handled = false;
        bool inited = false;
        try {
            winrt::init_apartment();
            inited = true;
            const auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            if (const auto session = pickMediaSession(manager)) {
                const auto info = session.GetPlaybackInfo();
                const auto controls = info.Controls();
                const bool playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                const auto trySeekDelta = [&](int64_t deltaTicks) -> bool {
                    if (!controls.IsPlaybackPositionEnabled()) return false;
                    const auto tl = session.GetTimelineProperties();
                    int64_t pos = tl.Position().count();
                    const int64_t start = tl.StartTime().count();
                    const int64_t end = tl.EndTime().count();
                    if (end <= start) return false;
                    if (playing && tl.LastUpdatedTime().time_since_epoch().count() > 0) {
                        const auto nowTp = winrt::clock::now();
                        const int64_t elapsed = (nowTp - tl.LastUpdatedTime()).count();
                        if (elapsed > 0 && elapsed < 36000000000LL) pos += elapsed;
                    }
                    const int64_t target = std::clamp(pos + deltaTicks, start, end);
                    return session.TryChangePlaybackPositionAsync(target).get();
                };
                if (vk == VK_MEDIA_PLAY_PAUSE) {
                    handled = session.TryTogglePlayPauseAsync().get();
                    if (!handled) handled = playing ? session.TryPauseAsync().get() : session.TryPlayAsync().get();
                } else if (vk == VK_MEDIA_PREV_TRACK) {
                    if (controls.IsPreviousEnabled()) handled = session.TrySkipPreviousAsync().get();
                    if (!handled) handled = trySeekDelta(-100000000LL); // -10s for single videos (e.g. YouTube)
                    if (!handled && controls.IsRewindEnabled()) handled = session.TryRewindAsync().get();
                } else if (vk == VK_MEDIA_NEXT_TRACK) {
                    if (controls.IsNextEnabled()) handled = session.TrySkipNextAsync().get();
                    if (!handled) handled = trySeekDelta(100000000LL); // +10s for single videos (e.g. YouTube)
                    if (!handled && controls.IsFastForwardEnabled()) handled = session.TryFastForwardAsync().get();
                }
            }
        } catch (const winrt::hresult_error&) {}
        if (inited) winrt::uninit_apartment();
        if (!handled) {
            INPUT keys[2]{};
            for (auto& k : keys) { k.type = INPUT_KEYBOARD; k.ki.wVk = vk; }
            keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            handled = SendInput(2, keys, sizeof(INPUT)) == 2;
        }
        return handled;
    });
}

static std::unique_ptr<Gdiplus::Bitmap> bitmapFromHBitmap(HBITMAP hBmp) {
    if (!hBmp) return nullptr;
    BITMAP bm{};
    if (!GetObjectW(hBmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return nullptr;
    const int w = bm.bmWidth, h = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<DWORD> pixels(static_cast<size_t>(w) * h);
    HDC hdc = GetDC(nullptr);
    const int got = GetDIBits(hdc, hBmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    if (!got) return nullptr;

    bool hasAlpha = false, needsPremul = false;
    for (DWORD px : pixels) {
        const BYTE a = static_cast<BYTE>(px >> 24);
        if (a != 0) {
            hasAlpha = true;
            const BYTE r = static_cast<BYTE>((px >> 16) & 0xFF);
            const BYTE g = static_cast<BYTE>((px >> 8) & 0xFF);
            const BYTE b = static_cast<BYTE>(px & 0xFF);
            if (r > a || g > a || b > a) { needsPremul = true; break; }
        }
    }

    auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, w, h);
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) != Gdiplus::Ok) return nullptr;
    for (int y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.Scan0) + y * data.Stride);
        const DWORD* src = pixels.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            DWORD px = src[x];
            if (!hasAlpha) {
                px |= 0xFF000000u;
            } else if (needsPremul) {
                const unsigned a = (px >> 24) & 0xFFu;
                const unsigned r = ((px >> 16) & 0xFFu) * a / 255u;
                const unsigned g = ((px >> 8) & 0xFFu) * a / 255u;
                const unsigned b = (px & 0xFFu) * a / 255u;
                px = (a << 24) | (r << 16) | (g << 8) | b;
            }
            dst[x] = px;
        }
    }
    bmp->UnlockBits(&data);
    return normalizeAppIcon(*bmp);
}

static std::unique_ptr<Gdiplus::Bitmap> bitmapFromHIcon(HICON hIcon) {
    if (!hIcon) return nullptr;
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    std::unique_ptr<Gdiplus::Bitmap> result;
    BITMAP bm{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        std::vector<DWORD> colorPx(static_cast<size_t>(w) * h);
        HDC hdc = GetDC(nullptr);
        const int gotColor = GetDIBits(hdc, ii.hbmColor, 0, h, colorPx.data(), &bi, DIB_RGB_COLORS);
        std::vector<DWORD> maskPx(static_cast<size_t>(w) * h);
        const int gotMask = ii.hbmMask ? GetDIBits(hdc, ii.hbmMask, 0, h, maskPx.data(), &bi, DIB_RGB_COLORS) : 0;
        ReleaseDC(nullptr, hdc);

        if (gotColor) {
            bool hasAlpha = false;
            for (DWORD px : colorPx) {
                if ((px >> 24) != 0) { hasAlpha = true; break; }
            }
            auto bmp = std::make_unique<Gdiplus::Bitmap>(w, h, PixelFormat32bppPARGB);
            Gdiplus::BitmapData data{};
            Gdiplus::Rect rect(0, 0, w, h);
            if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Gdiplus::Ok) {
                for (int y = 0; y < h; ++y) {
                    auto* dst = reinterpret_cast<DWORD*>(static_cast<BYTE*>(data.Scan0) + y * data.Stride);
                    const DWORD* src = colorPx.data() + static_cast<size_t>(y) * w;
                    const DWORD* msk = gotMask ? (maskPx.data() + static_cast<size_t>(y) * w) : nullptr;
                    for (int x = 0; x < w; ++x) {
                        DWORD px = src[x];
                        if (hasAlpha) {
                            const unsigned a = (px >> 24) & 0xFFu;
                            const unsigned r = ((px >> 16) & 0xFFu) * a / 255u;
                            const unsigned g = ((px >> 8) & 0xFFu) * a / 255u;
                            const unsigned b = (px & 0xFFu) * a / 255u;
                            dst[x] = (a << 24) | (r << 16) | (g << 8) | b;
                        } else if (msk && (msk[x] & 0x00FFFFFFu) != 0) {
                            dst[x] = 0;
                        } else {
                            dst[x] = px | 0xFF000000u;
                        }
                    }
                }
                bmp->UnlockBits(&data);
                result = normalizeAppIcon(*bmp);
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

static std::unique_ptr<Gdiplus::Bitmap> highResBitmapForPath(const wchar_t* path) {
    if (!path || !*path) return nullptr;
    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&factory))) && factory) {
        HBITMAP hBmp = nullptr;
        if (SUCCEEDED(factory->GetImage(SIZE{128, 128}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hBmp)) && hBmp) {
            auto bmp = bitmapFromHBitmap(hBmp);
            DeleteObject(hBmp);
            if (bmp) return bmp;
        }
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path, 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON) && info.hIcon) {
        auto bmp = bitmapFromHIcon(info.hIcon);
        DestroyIcon(info.hIcon);
        return bmp;
    }
    return nullptr;
}

std::unique_ptr<Gdiplus::Bitmap> highResIconForPath(const wchar_t* path) {
    return highResBitmapForPath(path);
}

static std::mutex gAppImageCacheMutex;
static std::unordered_map<Text, std::shared_ptr<Gdiplus::Bitmap>> gAppImageCache;

static Text lowerCacheKey(const Text& s) {
    Text out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return out;
}

static std::shared_ptr<Gdiplus::Bitmap> lookupCachedAppImage(const Text& target) {
    if (target.empty()) return {};
    std::lock_guard lock(gAppImageCacheMutex);
    const auto it = gAppImageCache.find(lowerCacheKey(target));
    return it != gAppImageCache.end() ? it->second : std::shared_ptr<Gdiplus::Bitmap>{};
}

static void storeCachedAppImage(const Text& target, const std::shared_ptr<Gdiplus::Bitmap>& bmp) {
    if (target.empty() || !bmp) return;
    std::lock_guard lock(gAppImageCacheMutex);
    if (gAppImageCache.size() > 512) gAppImageCache.clear();
    gAppImageCache[lowerCacheKey(target)] = bmp;
}

std::shared_ptr<Gdiplus::Bitmap> iconFromLocation(const Text& iconPath, int iconIndex) {
    if (iconPath.empty()) return {};
    const Text cacheKey = iconPath + L"#" + num(iconIndex);
    if (auto cached = lookupCachedAppImage(cacheKey)) return cached;

    std::unique_ptr<Gdiplus::Bitmap> fresh;
    const Text lower = lowerCacheKey(iconPath);
    const bool isImageFile = lower.ends_with(L".png") || lower.ends_with(L".jpg") ||
        lower.ends_with(L".jpeg") || lower.ends_with(L".bmp");
    const bool isIconFile = lower.ends_with(L".ico") || lower.ends_with(L".tmp");
    const bool isResourceBinary = lower.ends_with(L".dll") || lower.ends_with(L".mun") || lower.ends_with(L".icl");

    if (iconIndex == 0 && isImageFile) {
        Gdiplus::Bitmap img(iconPath.c_str());
        if (img.GetLastStatus() == Gdiplus::Ok && img.GetWidth() > 0 && img.GetHeight() > 0)
            fresh = normalizeAppIcon(img);
    }
    if (!fresh && iconIndex == 0 && isIconFile) {
        HICON hIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE));
        if (!hIcon) hIcon = static_cast<HICON>(LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
        if (hIcon) {
            fresh = bitmapFromHIcon(hIcon);
            DestroyIcon(hIcon);
        }
    }
    if (!fresh && (iconIndex != 0 || isIconFile || isResourceBinary)) {
        HICON hLarge = nullptr, hSmall = nullptr;
        const UINT got = ExtractIconExW(iconPath.c_str(), iconIndex, &hLarge, &hSmall, 1);
        if (got > 0 && (hLarge || hSmall)) {
            fresh = bitmapFromHIcon(hLarge ? hLarge : hSmall);
        }
        if (hLarge) DestroyIcon(hLarge);
        if (hSmall) DestroyIcon(hSmall);
    }
    if (!fresh && iconIndex == 0 && !isIconFile && !isResourceBinary) {
        fresh = highResBitmapForPath(iconPath.c_str());
    }
    if (!fresh) return {};
    std::shared_ptr<Gdiplus::Bitmap> shared(std::move(fresh));
    storeCachedAppImage(cacheKey, shared);
    return shared;
}

static std::shared_ptr<Gdiplus::Bitmap> queryWindowIcon(HWND window, const Text& appTarget) {
    // 1. Check per-window custom icon (preserves browser profile badges, etc.).
    HICON hIcon = nullptr;
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(window, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 35, &result) && result)
        hIcon = reinterpret_cast<HICON>(result);
    if (!hIcon && SendMessageTimeoutW(window, WM_GETICON, 2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 35, &result) && result)
        hIcon = reinterpret_cast<HICON>(result);
    if (hIcon) {
        if (auto bmp = bitmapFromHIcon(hIcon)) return std::shared_ptr<Gdiplus::Bitmap>(std::move(bmp));
    }

    // 2. For UWP/MSIX windows (shell:AppsFolder\...), resolve the real Store app icon instead of ApplicationFrameHost.exe.
    if (appTarget.starts_with(L"shell:AppsFolder\\")) {
        if (auto bmp = appImage(appTarget)) return bmp;
    }

    // 3. Prefer cached high-resolution 128x128 Shell icon from the process executable.
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid) {
        if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t exePath[1024]{};
            DWORD len = ARRAYSIZE(exePath);
            std::shared_ptr<Gdiplus::Bitmap> exeBmp;
            if (QueryFullProcessImageNameW(proc, 0, exePath, &len) && len > 0) {
                const Text key(exePath, len);
                exeBmp = lookupCachedAppImage(key);
                if (!exeBmp) {
                    if (auto fresh = highResBitmapForPath(exePath)) {
                        exeBmp = std::shared_ptr<Gdiplus::Bitmap>(std::move(fresh));
                        storeCachedAppImage(key, exeBmp);
                    }
                }
            }
            CloseHandle(proc);
            if (exeBmp) return exeBmp;
        }
    }

    // 4. Fallback to window class icon.
    hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
    if (!hIcon) hIcon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
    if (hIcon) {
        if (auto bmp = bitmapFromHIcon(hIcon)) return std::shared_ptr<Gdiplus::Bitmap>(std::move(bmp));
    }
    return nullptr;
}

void Dock::requestWindowIcons() {
    if (!hwnd || extra.windowIconsRequest.valid()) return;
    struct Job { HWND window; uint64_t id; Text target; };
    std::vector<Job> batch;
    for (auto& win : extra.windows) {
        if (win.iconChecked) continue;
        batch.push_back({win.handle, win.iconRequestId, win.app.target});
        win.iconChecked = true;
        if (batch.size() == 16) break;
    }
    if (batch.empty()) return;
    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    try {
        extra.windowIconsRequest = std::async(std::launch::async, [batch = std::move(batch), stop, notify] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::vector<WindowIconResult> icons;
            icons.reserve(batch.size());
            try {
                for (const auto& job : batch) {
                    if (stop->load()) break;
                    icons.emplace_back(job.id, queryWindowIcon(job.window, job.target));
                }
            } catch (const std::exception&) {}
            if (SUCCEEDED(apartment)) CoUninitialize();
            if (notify && !stop->load()) PostMessageW(notify, AsyncWake, 0, 0);
            return icons;
        });
    } catch (const std::exception&) {
        for (auto& win : extra.windows) {
            if (!win.icon) win.iconChecked = false;
        }
    }
}

int Dock::refreshOpenWindows() {
    std::vector<OpenWindow> discovered;
    EnumWindows(enumerateWindows, reinterpret_cast<LPARAM>(&discovered));
    const HWND fg = GetForegroundWindow();
    const auto runningHwnds = [&]() {
        std::vector<HWND> handles;
        for (const size_t idx : runningOnBar()) {
            if (idx < extra.windows.size()) handles.push_back(extra.windows[idx].handle);
        }
        return handles;
    };
    const auto oldRunning = runningHwnds();

    std::vector<OpenWindow> ordered;
    ordered.reserve(discovered.size());

    // Keep existing windows in their stable dock order
    for (const auto& prev : extra.windows) {
        auto it = std::find_if(discovered.begin(), discovered.end(), [&](const OpenWindow& w) { return w.handle == prev.handle; });
        if (it != discovered.end()) {
            OpenWindow item = prev;
            item.title = it->title;
            item.active = (item.handle == fg);
            ordered.push_back(std::move(item));
        }
    }
    // Append newly opened windows at the end
    for (auto& w : discovered) {
        if (std::none_of(ordered.begin(), ordered.end(), [&](const OpenWindow& existing) { return existing.handle == w.handle; })) {
            w.active = (w.handle == fg);
            w.iconRequestId = extra.nextWindowIconId++;
            w.app = windowApp(w.handle, false);
            if (w.app.label.empty()) w.app.label = w.title;
            if (!w.app.target.empty()) {
                if (auto cached = lookupCachedAppImage(w.app.target)) w.icon = std::move(cached);
            }
            ordered.push_back(std::move(w));
        }
    }

    bool changed = ordered.size() != extra.windows.size();
    if (!changed) {
        for (size_t i = 0; i < ordered.size(); ++i) {
            if (ordered[i].handle != extra.windows[i].handle ||
                ordered[i].active != extra.windows[i].active ||
                ordered[i].title != extra.windows[i].title) {
                changed = true;
                break;
            }
        }
    }
    extra.windows = std::move(ordered);
    if (changed) pinnedWindowRevision = 0;
    requestWindowIcons();
    if (oldRunning != runningHwnds()) return 2;
    return changed ? 1 : 0;
}

void Dock::activateWindow(HWND target, bool toggleMinimize) {
    if (!IsWindow(target)) {
        if (refreshOpenWindows() == 2) position();
        else paintBar();
        if (panel) paintSheet();
        return;
    }
    const HWND fg = GetForegroundWindow();
    const HWND fgRoot = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
    if (toggleMinimize && (fg == target || fgRoot == target) && !IsIconic(target)) {
        ShowWindowAsync(target, SW_MINIMIZE);
        scheduleWindowSync();
        return;
    }

    HWND focusTarget = GetLastActivePopup(target);
    if (!focusTarget || !IsWindow(focusTarget) || !IsWindowVisible(focusTarget)) focusTarget = target;

    if (IsIconic(target)) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        const int restoreCmd = (GetWindowPlacement(target, &wp) && (wp.flags & WPF_RESTORETOMAXIMIZED))
            ? SW_SHOWMAXIMIZED : SW_RESTORE;
        if (!IsHungAppWindow(target)) ShowWindow(target, restoreCmd);
        else ShowWindowAsync(target, restoreCmd);
    }

    DWORD targetPid = 0;
    const DWORD targetTid = GetWindowThreadProcessId(focusTarget, &targetPid);
    if (targetPid) AllowSetForegroundWindow(targetPid);
    AllowSetForegroundWindow(ASFW_ANY);

    const DWORD myTid = GetCurrentThreadId();
    const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attachedFg = false, attachedTarget = false;
    if (fgTid && fgTid != myTid) attachedFg = AttachThreadInput(myTid, fgTid, TRUE) != FALSE;
    if (targetTid && targetTid != myTid && targetTid != fgTid)
        attachedTarget = AttachThreadInput(myTid, targetTid, TRUE) != FALSE;

    BringWindowToTop(focusTarget);
    BOOL focused = SetForegroundWindow(focusTarget);
    if (!focused && focusTarget != target) focused = SetForegroundWindow(target);

    if (attachedTarget) AttachThreadInput(myTid, targetTid, FALSE);
    if (attachedFg) AttachThreadInput(myTid, fgTid, FALSE);

    if (!focused) {
        SwitchToThisWindow(focusTarget, TRUE);
        SetForegroundWindow(focusTarget);
    }
    scheduleWindowSync();
}
static void networkSample(ExtraWidgets& e, ULONGLONG stamp) {
    if (e.netAt && stamp - e.netAt < 1100) return;
    MIB_IF_TABLE2* table{};
    if (GetIfTable2(&table) != NO_ERROR) { e.networkReady = false; e.netAt = 0; return; }
    ULONGLONG received = 0, sent = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& item = table->Table[i];
        if (item.OperStatus != IfOperStatusUp || item.Type == IF_TYPE_SOFTWARE_LOOPBACK || item.Type == IF_TYPE_TUNNEL ||
            !item.InterfaceAndOperStatusFlags.HardwareInterface) continue;
        received += item.InOctets; sent += item.OutOctets;
    }
    FreeMibTable(table);
    if (e.netAt && stamp > e.netAt && received >= e.in && sent >= e.out) {
        std::rotate(e.download.begin(), e.download.begin() + 1, e.download.end());
        std::rotate(e.upload.begin(), e.upload.begin() + 1, e.upload.end());
        e.download.back() = (received - e.in) * 1000 / (stamp - e.netAt);
        e.upload.back() = (sent - e.out) * 1000 / (stamp - e.netAt);
        e.networkReady = true;
    } else e.networkReady = false;
    e.in = received; e.out = sent; e.netAt = stamp;
}
namespace {
ComPtr<IMMDeviceEnumerator> gAudioEnum;
ComPtr<IAudioEndpointVolume> gAudioEndpoint;
Text gAudioDeviceId;
Text gAudioDeviceName;
ULONGLONG gAudioCheckedAt = 0;

Text endpointName(IMMDevice& device) {
    constexpr PROPERTYKEY key{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
    ComPtr<IPropertyStore> store;
    Text name;
    if (SUCCEEDED(device.OpenPropertyStore(STGM_READ, &store))) {
        PROPVARIANT value{};
        if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
        PropVariantClear(&value);
    }
    return name;
}

IAudioEndpointVolume* cachedAudioEndpoint(bool forceCheck) {
    const ULONGLONG nowTick = GetTickCount64();
    if (!gAudioEnum) {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&gAudioEnum))))
            return nullptr;
    }
    if (!gAudioEndpoint || forceCheck || nowTick - gAudioCheckedAt >= 3000) {
        gAudioCheckedAt = nowTick;
        ComPtr<IMMDevice> device;
        if (FAILED(gAudioEnum->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) || !device) {
            gAudioEndpoint.Reset();
            gAudioDeviceId.clear();
            gAudioDeviceName.clear();
            return nullptr;
        }
        LPWSTR devId{};
        Text currentId;
        if (SUCCEEDED(device->GetId(&devId)) && devId) {
            currentId = devId;
            CoTaskMemFree(devId);
        }
        if (!gAudioEndpoint || currentId != gAudioDeviceId) {
            gAudioEndpoint.Reset();
            gAudioDeviceName = endpointName(*device.Get());
            if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(gAudioEndpoint.GetAddressOf())))) {
                gAudioDeviceId = std::move(currentId);
            } else {
                gAudioDeviceId.clear();
            }
        }
    }
    return gAudioEndpoint.Get();
}
}

void releaseAudioEndpoint() {
    gAudioEndpoint.Reset();
    gAudioEnum.Reset();
    gAudioDeviceId.clear();
    gAudioDeviceName.clear();
    gAudioCheckedAt = 0;
}

static void audio(ExtraWidgets& e, int by, bool toggle, float setScalar = -1.f) {
    e.volume = -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto* endpoint = cachedAudioEndpoint(attempt > 0);
        e.outputName = gAudioDeviceName;
        if (!endpoint) break;
        float level{};
        BOOL muted{};
        if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&level))) {
            if (setScalar >= 0.f) endpoint->SetMasterVolumeLevelScalar(std::clamp(setScalar, 0.f, 1.f), nullptr);
            else if (by) endpoint->SetMasterVolumeLevelScalar(std::clamp(level + by / 100.f, 0.f, 1.f), nullptr);
            if (SUCCEEDED(endpoint->GetMasterVolumeLevelScalar(&level))) e.volume = static_cast<int>(std::lround(level * 100));
            if (SUCCEEDED(endpoint->GetMute(&muted))) {
                if (setScalar > 0.f && muted) endpoint->SetMute(FALSE, nullptr);
                else if (toggle) endpoint->SetMute(!muted, nullptr);
                endpoint->GetMute(&muted);
                e.muted = muted != FALSE;
            }
            return;
        }
        gAudioEndpoint.Reset();
    }
}
static Text friendlyProcessName(DWORD pid) {
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t path[1024]{};
    DWORD len = ARRAYSIZE(path);
    Text name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len) && len > 0) {
        name = leaf(Text(path, len));
        const size_t dot = name.rfind(L'.');
        if (dot != Text::npos) name.resize(dot);
        if (!name.empty()) name[0] = static_cast<wchar_t>(std::towupper(name[0]));
    }
    CloseHandle(proc);
    return name;
}

void Dock::sampleMicrophone(bool toggle) {
    extra.microphoneMute = -1;
    extra.micDeviceName.clear();
    extra.micActiveApps.clear();
    if (!gAudioEnum && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&gAudioEnum)))) return;
    ComPtr<IMMDevice> device;
    if (FAILED(gAudioEnum->GetDefaultAudioEndpoint(eCapture, eCommunications, &device)) &&
        FAILED(gAudioEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) return;
    if (device) {
        extra.micDeviceName = endpointName(*device.Get());
        ComPtr<IAudioEndpointVolume> endpoint;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(endpoint.GetAddressOf()))) && endpoint) {
            BOOL muted{};
            if (SUCCEEDED(endpoint->GetMute(&muted))) {
                if (toggle && SUCCEEDED(endpoint->SetMute(!muted, nullptr))) endpoint->GetMute(&muted);
                extra.microphoneMute = muted != FALSE;
            }
        }
    }
    // Enumerate active recording sessions across all active capture endpoints
    ComPtr<IMMDeviceCollection> captures;
    if (SUCCEEDED(gAudioEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &captures)) && captures) {
        UINT devCount = 0;
        captures->GetCount(&devCount);
        for (UINT dIdx = 0; dIdx < devCount; ++dIdx) {
            ComPtr<IMMDevice> capDev;
            if (FAILED(captures->Item(dIdx, &capDev)) || !capDev) continue;
            ComPtr<IAudioSessionManager2> mgr;
            if (FAILED(capDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(mgr.GetAddressOf()))) || !mgr) continue;
            ComPtr<IAudioSessionEnumerator> sessEnum;
            if (FAILED(mgr->GetSessionEnumerator(&sessEnum)) || !sessEnum) continue;
            int sCount = 0;
            sessEnum->GetCount(&sCount);
            for (int sIdx = 0; sIdx < sCount; ++sIdx) {
                ComPtr<IAudioSessionControl> ctrl;
                if (FAILED(sessEnum->GetSession(sIdx, &ctrl)) || !ctrl) continue;
                AudioSessionState state = AudioSessionStateInactive;
                if (FAILED(ctrl->GetState(&state)) || state != AudioSessionStateActive) continue;
                ComPtr<IAudioSessionControl2> ctrl2;
                if (FAILED(ctrl.As(&ctrl2)) || !ctrl2) continue;
                if (ctrl2->IsSystemSoundsSession() == S_OK) continue;
                DWORD pid = 0;
                ctrl2->GetProcessId(&pid);
                Text appName = friendlyProcessName(pid);
                if (appName.empty()) {
                    LPWSTR disp{};
                    if (SUCCEEDED(ctrl->GetDisplayName(&disp)) && disp) {
                        if (*disp && disp[0] != L'@') appName = disp;
                        CoTaskMemFree(disp);
                    }
                }
                if (appName.empty()) appName = L"Uygulama";
                if (std::none_of(extra.micActiveApps.begin(), extra.micActiveApps.end(), [&](const Text& existing) {
                    return CompareStringOrdinal(existing.c_str(), -1, appName.c_str(), -1, TRUE) == CSTR_EQUAL;
                })) {
                    extra.micActiveApps.push_back(std::move(appName));
                }
            }
        }
    }
    if (toggle) { paintBar(); paintSheet(); }
}

void Dock::sampleNetworkFast() {
    MIB_IF_TABLE2* table{};
    if (GetIfTable2(&table) != NO_ERROR || !table) return;
    bool eth = false, wifi = false;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const auto& item = table->Table[i];
        if (item.OperStatus != IfOperStatusUp ||
            item.MediaConnectState != MediaConnectStateConnected ||
            !item.InterfaceAndOperStatusFlags.HardwareInterface ||
            item.Type == IF_TYPE_SOFTWARE_LOOPBACK || item.Type == IF_TYPE_TUNNEL ||
            item.PhysicalMediumType == NdisPhysicalMediumBluetooth)
            continue;
        if (item.Type == IF_TYPE_ETHERNET_CSMACD) eth = true;
        else if (item.Type == IF_TYPE_IEEE80211) wifi = true;
    }
    FreeMibTable(table);
    extra.netEthernetFast = eth;
    extra.netWifiFast = wifi;
}

namespace {
struct LayoutRegCache {
    std::unordered_map<Text, Text> byKlid;
    std::unordered_map<WORD, Text> byLayoutId;
    bool loaded = false;

    void ensure() {
        if (loaded) return;
        loaded = true;
        HKEY root{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
            0, KEY_READ, &root) != ERROR_SUCCESS) return;
        wchar_t subName[64]{};
        for (DWORD idx = 0; ; ++idx) {
            DWORD subLen = ARRAYSIZE(subName);
            if (RegEnumKeyExW(root, idx, subName, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY sub{};
            if (RegOpenKeyExW(root, subName, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
            wchar_t dispBuf[256]{}, textBuf[256]{}, idBuf[32]{};
            DWORD bytes = sizeof(dispBuf);
            Text name;
            if (RegGetValueW(sub, nullptr, L"Layout Display Name", RRF_RT_REG_SZ, nullptr, dispBuf, &bytes) == ERROR_SUCCESS && *dispBuf) {
                wchar_t resolved[256]{};
                if (SUCCEEDED(SHLoadIndirectString(dispBuf, resolved, ARRAYSIZE(resolved), nullptr)) && *resolved)
                    name = resolved;
            }
            if (name.empty()) {
                bytes = sizeof(textBuf);
                if (RegGetValueW(sub, nullptr, L"Layout Text", RRF_RT_REG_SZ, nullptr, textBuf, &bytes) == ERROR_SUCCESS && *textBuf)
                    name = textBuf;
            }
            if (!name.empty()) {
                Text klid(subName);
                for (auto& ch : klid) ch = static_cast<wchar_t>(std::towupper(ch));
                byKlid[klid] = name;
                bytes = sizeof(idBuf);
                if (RegGetValueW(sub, nullptr, L"Layout Id", RRF_RT_REG_SZ, nullptr, idBuf, &bytes) == ERROR_SUCCESS && *idBuf) {
                    const WORD lid = static_cast<WORD>(wcstoul(idBuf, nullptr, 16));
                    if (lid) byLayoutId[lid] = name;
                }
            }
            RegCloseKey(sub);
        }
        RegCloseKey(root);
    }
};

LayoutRegCache gLayoutReg;

Text resolveKeyboardLayoutName(LANGID langId, WORD deviceId) {
    gLayoutReg.ensure();
    if ((deviceId & 0xF000) == 0xF000) {
        const WORD lid = deviceId & 0x0FFF;
        if (auto it = gLayoutReg.byLayoutId.find(lid); it != gLayoutReg.byLayoutId.end()) return it->second;
    }
    wchar_t klid[16]{};
    swprintf_s(klid, L"0000%04X", deviceId ? deviceId : langId);
    if (auto it = gLayoutReg.byKlid.find(klid); it != gLayoutReg.byKlid.end()) return it->second;
    swprintf_s(klid, L"0000%04X", langId);
    if (auto it = gLayoutReg.byKlid.find(klid); it != gLayoutReg.byKlid.end()) return it->second;
    return {};
}

Text deriveLayoutSubCode(const Text& layoutName, LANGID langId, WORD deviceId) {
    if (deviceId == 0xF014 || layoutName.find(L" F") != Text::npos || layoutName.ends_with(L" F")) return L"F";
    if (langId == 0x041F && (deviceId == 0x041F || layoutName.find(L" Q") != Text::npos || layoutName.ends_with(L" Q"))) return L"Q";
    if (deviceId == 0x0409 || layoutName.find(L"US") != Text::npos || layoutName.find(L"ABD") != Text::npos) return L"US";
    if (deviceId == 0x0809 || layoutName.find(L"UK") != Text::npos || layoutName.find(L"Birleşik Krallık") != Text::npos) return L"UK";
    if (!layoutName.empty()) {
        Text out;
        for (wchar_t ch : layoutName) {
            if (std::iswalnum(ch)) {
                out.push_back(static_cast<wchar_t>(std::towupper(ch)));
                if (out.size() == 2) break;
            }
        }
        return out;
    }
    return {};
}

KeyboardLayoutInfo describeKeyboardHkl(HKL hkl, bool active) {
    KeyboardLayoutInfo info;
    info.hkl = hkl;
    info.active = active;
    const LANGID langId = LOWORD(reinterpret_cast<UINT_PTR>(hkl));
    const WORD deviceId = HIWORD(reinterpret_cast<UINT_PTR>(hkl));
    const LCID lcid = MAKELCID(langId, SORT_DEFAULT);
    wchar_t buf[128]{};
    if (GetLocaleInfoW(lcid, LOCALE_SISO639LANGNAME, buf, ARRAYSIZE(buf)) > 0) {
        info.shortCode = buf;
        for (auto& ch : info.shortCode) ch = static_cast<wchar_t>(std::towupper(ch));
    }
    if (info.shortCode.empty() && GetLocaleInfoW(lcid, LOCALE_SABBREVLANGNAME, buf, ARRAYSIZE(buf)) > 0) {
        info.shortCode = buf;
        if (info.shortCode.size() > 3) info.shortCode.resize(3);
    }
    if (info.shortCode.empty()) info.shortCode = L"KB";
    if (GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDDISPLAYNAME, buf, ARRAYSIZE(buf)) > 0) info.languageName = buf;
    else if (GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDLANGUAGENAME, buf, ARRAYSIZE(buf)) > 0) info.languageName = buf;
    else info.languageName = info.shortCode;
    info.layoutName = resolveKeyboardLayoutName(langId, deviceId);
    if (info.layoutName.empty()) info.layoutName = info.languageName;
    info.subCode = deriveLayoutSubCode(info.layoutName, langId, deviceId);
    return info;
}

HWND resolveInputTargetWindow(HWND fg) {
    if (!fg || !IsWindow(fg)) return nullptr;
    wchar_t cls[64]{};
    if (GetClassNameW(fg, cls, ARRAYSIZE(cls)) && wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        if (HWND core = FindWindowExW(fg, nullptr, L"Windows.UI.Core.CoreWindow", nullptr))
            return core;
    }
    return fg;
}
}

void Dock::sampleKeyboardLayout() {
    const HWND fg = GetForegroundWindow();
    if (fg && IsWindow(fg)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid && pid != GetCurrentProcessId()) lastExternalFg = fg;
    }
    HWND source = (lastExternalFg && IsWindow(lastExternalFg)) ? lastExternalFg : fg;
    HWND target = resolveInputTargetWindow(source);
    DWORD tid = target ? GetWindowThreadProcessId(target, nullptr) : 0;
    HKL current = tid ? GetKeyboardLayout(tid) : nullptr;
    if (!current) current = GetKeyboardLayout(0);

    const int count = GetKeyboardLayoutList(0, nullptr);
    std::vector<HKL> rawList(count > 0 ? count : 0);
    if (count > 0) GetKeyboardLayoutList(count, rawList.data());
    if (current && std::find(rawList.begin(), rawList.end(), current) == rawList.end())
        rawList.insert(rawList.begin(), current);

    std::vector<KeyboardLayoutInfo> layouts;
    layouts.reserve(rawList.size());
    std::unordered_map<Text, int> shortCounts;
    for (HKL hkl : rawList) {
        if (!hkl) continue;
        auto info = describeKeyboardHkl(hkl, hkl == current);
        ++shortCounts[info.shortCode];
        layouts.push_back(std::move(info));
    }
    Text activeShort = L"KB", activeSub;
    for (auto& item : layouts) {
        if (shortCounts[item.shortCode] <= 1) item.subCode.clear();
        if (item.active) {
            activeShort = item.shortCode;
            activeSub = item.subCode;
        }
    }
    extra.keyboardLayouts = std::move(layouts);
    extra.activeKeyboardHkl = current;
    extra.keyboardShort = std::move(activeShort);
    extra.keyboardSub = std::move(activeSub);
}

void Dock::switchKeyboardLayout(HKL targetHkl) {
    if (!targetHkl) return;
    if (panel && page == KeyboardLang) closeSheet();
    HWND source = (lastExternalFg && IsWindow(lastExternalFg)) ? lastExternalFg : GetForegroundWindow();
    HWND target = resolveInputTargetWindow(source);
    bool posted = false;
    bool accessDenied = false;
    if (target && IsWindow(target)) {
        const DWORD tid = GetWindowThreadProcessId(target, nullptr);
        GUITHREADINFO gui{sizeof(gui)};
        HWND focusWin = (tid && GetGUIThreadInfo(tid, &gui) && gui.hwndFocus && IsWindow(gui.hwndFocus))
            ? gui.hwndFocus : nullptr;
        if (focusWin) {
            SetLastError(0);
            if (PostMessageW(focusWin, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(targetHkl)))
                posted = true;
            else if (GetLastError() == ERROR_ACCESS_DENIED) accessDenied = true;
        }
        if (target != focusWin) {
            SetLastError(0);
            if (PostMessageW(target, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(targetHkl)))
                posted = true;
            else if (GetLastError() == ERROR_ACCESS_DENIED) accessDenied = true;
        }
    }
    ActivateKeyboardLayout(targetHkl, KLF_SETFORPROCESS);
    if (!posted && accessDenied) {
        // Elevated foreground window blocks WM_INPUTLANGCHANGEREQUEST via UIPI; trigger Windows Win+Space switcher.
        INPUT keys[4]{};
        for (auto& k : keys) k.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = keys[3].ki.wVk = VK_LWIN;
        keys[1].ki.wVk = keys[2].ki.wVk = VK_SPACE;
        keys[2].ki.dwFlags = keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT));
    }
    extra.activeKeyboardHkl = targetHkl;
    for (auto& item : extra.keyboardLayouts) {
        item.active = (item.hkl == targetHkl);
        if (item.active) {
            extra.keyboardShort = item.shortCode;
            extra.keyboardSub = item.subCode;
        }
    }
    paintBar();
    if (panel && page == KeyboardLang) paintSheet();
}

void Dock::cycleKeyboardLayout(int delta) {
    sampleKeyboardLayout();
    if (extra.keyboardLayouts.size() < 2) {
        report(L"Yalnızca tek klavye düzeni yüklü.");
        return;
    }
    int cur = 0;
    for (int i = 0; i < static_cast<int>(extra.keyboardLayouts.size()); ++i) {
        if (extra.keyboardLayouts[i].active) { cur = i; break; }
    }
    const int n = static_cast<int>(extra.keyboardLayouts.size());
    const int next = ((cur + (delta >= 0 ? 1 : -1)) % n + n) % n;
    switchKeyboardLayout(extra.keyboardLayouts[next].hkl);
}
void Dock::changeVolume(int by, bool mute) {
    audio(extra, by, mute);
    if (extra.volume < 0) report(L"Etkin ses çıkışı bulunamadı.");
    else {
        barVolumeTarget = extra.muted ? 0.f : std::clamp(extra.volume / 100.f, 0.f, 1.f);
        barVolumeSent = barVolumeTarget;
        if (barVolumeLevel < 0.f || !motionEnabled()) barVolumeLevel = barVolumeTarget;
        else {
            barVolumeAnimating = true;
            barVolumeTickMs = animNowMs();
            startAnimTimer();
        }
        paintBar();
        paintSheet();
    }
}
static bool setAudioScalarFast(ExtraWidgets& e, float scalar) {
    scalar = std::clamp(scalar, 0.f, 1.f);
    auto* endpoint = cachedAudioEndpoint(false);
    if (!endpoint) endpoint = cachedAudioEndpoint(true);
    if (!endpoint) return false;
    if (FAILED(endpoint->SetMasterVolumeLevelScalar(scalar, nullptr))) {
        gAudioEndpoint.Reset();
        endpoint = cachedAudioEndpoint(true);
        if (!endpoint || FAILED(endpoint->SetMasterVolumeLevelScalar(scalar, nullptr))) return false;
    }
    if (scalar > 0.f && e.muted) {
        if (SUCCEEDED(endpoint->SetMute(FALSE, nullptr))) e.muted = false;
    }
    e.volume = static_cast<int>(std::lround(scalar * 100.f));
    return true;
}

void Dock::setVolumeLevel(float level, bool instant, bool dragging) {
    level = std::clamp(level, 0.f, 1.f);
    const double nowMs = animNowMs();
    if (dragging) {
        barVolumeTarget = level;
        barVolumeLevel = level;
        barVolumeAnimating = false;
        extra.volume = static_cast<int>(std::lround(level * 100.f));
        if (level > 0.f && extra.muted) {
            setAudioScalarFast(extra, level);
            barVolumeSent = level;
            barVolumeCommitMs = nowMs;
        } else if (barVolumeSent < 0.f || level == 0.f || level == 1.f ||
                   (nowMs - barVolumeCommitMs >= 26.0 && std::abs(level - barVolumeSent) >= 0.004f)) {
            if (setAudioScalarFast(extra, level)) {
                barVolumeSent = level;
                barVolumeCommitMs = nowMs;
            }
        }
        if (nowMs - barVolumePaintMs >= 8.0 || level == 0.f || level == 1.f) {
            barVolumePaintMs = nowMs;
            paintBar();
            if (panel && (page == Quick || page == Volume)) paintSheet();
        }
        return;
    }
    if (!setAudioScalarFast(extra, level)) {
        audio(extra, 0, false, level);
        if (extra.volume < 0) {
            report(L"Etkin ses çıkışı bulunamadı.");
            return;
        }
    }
    barVolumeTarget = extra.muted ? 0.f : level;
    barVolumeSent = level;
    barVolumeCommitMs = nowMs;
    if (instant || barVolumeLevel < 0.f || !motionEnabled()) {
        barVolumeLevel = barVolumeTarget;
        barVolumeAnimating = false;
    } else {
        if (!barVolumeAnimating) barVolumeTickMs = nowMs;
        barVolumeAnimating = true;
        startAnimTimer();
    }
    barVolumePaintMs = nowMs;
    paintBar();
    if (panel && (page == Quick || page == Volume)) paintSheet();
}
void Dock::sampleExtras(bool immediate) {
    const auto enabled = [&](int id) { return page == id || (shown(id) && !isFoldedWidget(id)); };
    const auto& sb = model.statusBar;
    const bool sbMic = sb.enabled && sb.microphone;
    const bool sbKey = sb.enabled && sb.keyboard;
    const bool sbNet = sb.enabled && sb.network;
    const bool sbVol = sb.enabled && sb.volume;
    const bool sbBat = sb.enabled && sb.battery;
    const auto stamp = GetTickCount64();
    if (enabled(NetworkWidget)) networkSample(extra, stamp);
    else extra.netAt = 0;
    if (enabled(Storage) && ((immediate && page == Storage) || !extra.lastDisks || stamp - extra.lastDisks >= 35000)) {
        extra.disks = localDisks(); extra.lastDisks = stamp;
    }
    if (enabled(Power) || sbBat) {
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power)) {
            battery = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
            extra.plugged = power.ACLineStatus == 1;
            extra.batteryMinutes = power.BatteryLifeTime == DWORD(-1) ? -1 : static_cast<int>(power.BatteryLifeTime / 60);
            syncMeterTargets();
        }
    }
    if ((enabled(Volume) || enabled(MicControl) || page == Quick || sbVol) && !barVolumeDrag && !volumeDrag) {
        audio(extra, 0, false);
        if (extra.volume >= 0) {
            const float sampled = extra.muted ? 0.f : std::clamp(extra.volume / 100.f, 0.f, 1.f);
            barVolumeTarget = sampled;
            if (barVolumeLevel < 0.f || !motionEnabled()) barVolumeLevel = sampled;
            else if (std::abs(barVolumeTarget - barVolumeLevel) >= 0.005f) {
                barVolumeAnimating = true;
                barVolumeTickMs = animNowMs();
                startAnimTimer();
            }
        }
    }
    if (enabled(MicControl) || sbMic) sampleMicrophone();
    if (sbKey || page == KeyboardLang) sampleKeyboardLayout();
    if (sbNet) sampleNetworkFast();
    if (page == Quick || enabled(QuickToggles) ||
        (sbNet && (immediate || !extra.lastRadioPollAt || stamp - extra.lastRadioPollAt >= 10000))) {
        extra.lastRadioPollAt = stamp;
        queryRadios();
    }
    if (immediate && mediaInUse()) queryMedia();
    if (page == Quick) {
        queryQuickStates();
        if (immediate) refreshWifi(false, false);
    }
    if (weatherInUse()) requestWeather();
    if (enabled(RecycleBin)) refreshRecycleBin();
    requestMarket();
    sampleProWidgets(immediate && proWidget(page));
}
bool Dock::weatherInUse() const {
    if (weatherWidget(page) || page == DayTimeline) return true;
    if (!anyDockVisible()) return false;
    return std::any_of(model.layouts[model.profile].begin(), model.layouts[model.profile].end(), [this](Tile tile) {
        return tile.enabled && (weatherWidget(tile.id) || tile.id == SevenDays || tile.id == TodayWidget ||
            tile.id == ClockWeather || tile.id == DayTimeline) &&
            !isFoldedWidget(tile.id);
    });
}
namespace {
struct WlanScanSignal {
    HANDLE event{};
    GUID iface{};
};
void WINAPI wlanScanCallback(PWLAN_NOTIFICATION_DATA data, PVOID ctx) {
    if (!data || !ctx) return;
    auto* sig = static_cast<WlanScanSignal*>(ctx);
    if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM &&
        IsEqualGUID(data->InterfaceGuid, sig->iface) &&
        (data->NotificationCode == wlan_notification_acm_scan_complete ||
         data->NotificationCode == wlan_notification_acm_scan_fail)) {
        SetEvent(sig->event);
    }
}

WifiScanResult scanWifiSync(bool doScan, bool includeLists) {
    WifiScanResult out;
    HANDLE client{};
    DWORD version{};
    DWORD error = WlanOpenHandle(2, nullptr, &version, &client);
    if (error != ERROR_SUCCESS) { out.error = error; return out; }
    WLAN_INTERFACE_INFO_LIST* interfaces{};
    error = WlanEnumInterfaces(client, nullptr, &interfaces);
    if (error == ERROR_SUCCESS && interfaces) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& adapter = interfaces->InterfaceInfo[i];
            const bool ifaceConnected = adapter.isState == wlan_interface_state_connected;
            out.connected |= ifaceConnected;

            // Query live active connection details (SSID, profile, signal quality)
            Text activeProfile;
            std::string activeSsid;
            int activeSignal = -1;
            if (ifaceConnected) {
                DWORD connSize = 0;
                WLAN_CONNECTION_ATTRIBUTES* connAttr{};
                if (WlanQueryInterface(client, &adapter.InterfaceGuid, wlan_intf_opcode_current_connection,
                    nullptr, &connSize, reinterpret_cast<PVOID*>(&connAttr), nullptr) == ERROR_SUCCESS && connAttr) {
                    activeProfile = connAttr->strProfileName;
                    const auto& dot = connAttr->wlanAssociationAttributes.dot11Ssid;
                    if (dot.uSSIDLength > 0 && dot.uSSIDLength <= DOT11_SSID_MAX_LENGTH)
                        activeSsid.assign(reinterpret_cast<const char*>(dot.ucSSID), dot.uSSIDLength);
                    activeSignal = static_cast<int>(connAttr->wlanAssociationAttributes.wlanSignalQuality);
                    if (out.connectedName.empty()) {
                        out.connectedName = activeSsid.empty() ? activeProfile : utf16(activeSsid);
                        out.connectedAdapter = adapter.InterfaceGuid;
                    }
                    WlanFreeMemory(connAttr);
                }
            }
            if (!includeLists) continue;

            if (doScan) {
                HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                WlanScanSignal sig{ev, adapter.InterfaceGuid};
                bool registered = false;
                if (ev && WlanRegisterNotification(client, WLAN_NOTIFICATION_SOURCE_ACM, TRUE,
                    wlanScanCallback, &sig, nullptr, nullptr) == ERROR_SUCCESS) {
                    registered = true;
                }
                if (WlanScan(client, &adapter.InterfaceGuid, nullptr, nullptr, nullptr) == ERROR_SUCCESS && ev) {
                    WaitForSingleObject(ev, 2400);
                }
                if (registered) WlanRegisterNotification(client, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
                if (ev) CloseHandle(ev);
            }

            WLAN_AVAILABLE_NETWORK_LIST* networks{};
            DWORD result = WlanGetAvailableNetworkList(client, &adapter.InterfaceGuid,
                WLAN_AVAILABLE_NETWORK_INCLUDE_ALL_MANUAL_HIDDEN_PROFILES, nullptr, &networks);
            if (result != ERROR_SUCCESS && result != ERROR_ACCESS_DENIED) {
                result = WlanGetAvailableNetworkList(client, &adapter.InterfaceGuid, 0, nullptr, &networks);
            }
            if (result != ERROR_SUCCESS) {
                if (out.error == 0 || result == ERROR_ACCESS_DENIED) out.error = result;
            }
            for (DWORD j = 0; networks && j < networks->dwNumberOfItems; ++j) {
                const auto& net = networks->Network[j];
                if (!net.dot11Ssid.uSSIDLength || net.dot11Ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) continue;
                std::string raw(reinterpret_cast<const char*>(net.dot11Ssid.ucSSID), net.dot11Ssid.uSSIDLength);
                Text name = utf16(raw);
                if (name.empty()) continue;
                const bool isConn = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0 ||
                    (!activeSsid.empty() && raw == activeSsid);
                const int sig = static_cast<int>(net.wlanSignalQuality);
                const Text prof = net.strProfileName;
                auto existing = std::find_if(out.available.begin(), out.available.end(), [&](const WifiNetwork& w) {
                    return IsEqualGUID(w.adapter, adapter.InterfaceGuid) && w.ssid == raw;
                });
                if (existing != out.available.end()) {
                    existing->connected |= isConn;
                    if (sig > existing->signal) existing->signal = sig;
                    if (existing->profile.empty() && !prof.empty()) existing->profile = prof;
                    if (existing->authentication == 0 && net.dot11DefaultAuthAlgorithm != 0) {
                        existing->authentication = static_cast<unsigned>(net.dot11DefaultAuthAlgorithm);
                        existing->cipher = static_cast<unsigned>(net.dot11DefaultCipherAlgorithm);
                    }
                    existing->secure |= (net.bSecurityEnabled != FALSE);
                } else {
                    out.available.push_back({adapter.InterfaceGuid, name, prof, sig, isConn,
                        net.bSecurityEnabled != FALSE, raw, static_cast<unsigned>(net.dot11DefaultAuthAlgorithm),
                        static_cast<unsigned>(net.dot11DefaultCipherAlgorithm), static_cast<unsigned>(net.dot11BssType)});
                }
            }
            if (networks) WlanFreeMemory(networks);

            // If connected to a network not yet in available (e.g. Location denied), surface the active network
            if (ifaceConnected && (!activeSsid.empty() || !activeProfile.empty())) {
                const Text activeName = activeSsid.empty() ? activeProfile : utf16(activeSsid);
                const bool listed = std::any_of(out.available.begin(), out.available.end(), [&](const WifiNetwork& w) {
                    return IsEqualGUID(w.adapter, adapter.InterfaceGuid) &&
                        ((!activeSsid.empty() && w.ssid == activeSsid) || w.name == activeName);
                });
                if (!listed && !activeName.empty()) {
                    out.available.push_back({adapter.InterfaceGuid, activeName, activeProfile,
                        activeSignal, true, true, activeSsid});
                }
            }

            // Collect historical saved profiles separately so they never pollute live nearby scan results
            WLAN_PROFILE_INFO_LIST* profiles{};
            if (WlanGetProfileList(client, &adapter.InterfaceGuid, nullptr, &profiles) == ERROR_SUCCESS && profiles) {
                for (DWORD j = 0; j < profiles->dwNumberOfItems; ++j) {
                    const Text name = profiles->ProfileInfo[j].strProfileName;
                    if (name.empty()) continue;
                    const bool inLive = std::any_of(out.available.begin(), out.available.end(), [&](const WifiNetwork& item) {
                        return IsEqualGUID(item.adapter, adapter.InterfaceGuid) && (item.profile == name || item.name == name);
                    });
                    if (!inLive) {
                        const bool isConn = ifaceConnected && !activeProfile.empty() && name == activeProfile;
                        out.saved.push_back({adapter.InterfaceGuid, name, name, isConn ? activeSignal : -1, isConn, true, {}});
                    }
                }
                WlanFreeMemory(profiles);
            }
        }
        WlanFreeMemory(interfaces);
    } else {
        out.error = error;
    }
    WlanCloseHandle(client, nullptr);

    std::stable_sort(out.available.begin(), out.available.end(), [](const auto& a, const auto& b) {
        if (a.connected != b.connected) return a.connected;
        return a.signal > b.signal;
    });
    std::stable_sort(out.saved.begin(), out.saved.end(), [](const auto& a, const auto& b) {
        return a.connected > b.connected;
    });
    return out;
}
}

void Dock::refreshWifi(bool scan, bool list) {
    if (!list) {
        const auto quick = scanWifiSync(false, false);
        extra.wifiConnected = quick.connected;
        if (quick.connected && !quick.connectedName.empty()) {
            extra.radios.network = quick.connectedName;
            extra.radios.adapter = quick.connectedAdapter;
        }
        return;
    }
    if (extra.wifiRequest.valid()) return;
    extra.wifiScanning = true;
    try {
        extra.wifiRequest = std::async(std::launch::async, [scan]() {
            return scanWifiSync(scan, true);
        });
    } catch (const std::exception&) {
        extra.wifiScanning = false;
    }
    if (panel && page == Quick) paintSheet();
}
static Text xmlText(const Text& text) {
    Text escaped;
    for (wchar_t c : text) {
        if (c == L'&') escaped += L"&amp;";
        else if (c == L'<') escaped += L"&lt;";
        else if (c == L'>') escaped += L"&gt;";
        else escaped += c;
    }
    return escaped;
}
void Dock::disconnectWifi(const GUID& adapter) {
    HANDLE client{};
    DWORD version{};
    DWORD error = WlanOpenHandle(2, nullptr, &version, &client);
    if (error == ERROR_SUCCESS) {
        error = WlanDisconnect(client, &adapter, nullptr);
        WlanCloseHandle(client, nullptr);
    }
    if (error == ERROR_SUCCESS) {
        extra.wifiConnected = false;
        extra.radios.network.clear();
        extra.wifiUpdateAt = GetTickCount64() + 900;
        extra.wifiUpdateUntil = GetTickCount64() + 5000;
        report(L"Wi-Fi bağlantısı kesildi.");
    } else {
        report(L"Bağlantı kesilemedi (" + num(error) + L").");
    }
}
void Dock::connectWifi(size_t index, bool fromSaved) {
    const auto& source = fromSaved ? extra.savedWifi : extra.wifi;
    if (index >= source.size()) return;
    auto network = source[index];
    if (network.connected) {
        disconnectWifi(network.adapter);
        return;
    }
    Text xml;
    if (network.profile.empty() && network.secure) {
        const bool wpa3 = network.authentication == DOT11_AUTH_ALGO_WPA3_SAE;
        if ((!wpa3 && network.authentication != DOT11_AUTH_ALGO_RSNA_PSK) || network.cipher != DOT11_CIPHER_ALGO_CCMP) {
            report(L"Bu ağın ilk kurulumunu Windows ağ panelinden tamamla.");
            return;
        }
        std::vector<Field> fields{{L"Wi-Fi parolası", L"", false, {}, true}};
        if (edit(network.name, fields, false, [](const auto& f) -> Text {
            const auto& pass = f[0].value;
            if (pass.size() < 8 || pass.size() > 63 || std::any_of(pass.begin(), pass.end(), [](wchar_t c) { return c < 32 || c > 126; }))
                return L"8–63 karakterlik Wi-Fi parolasını gir.";
            return {};
        }) != 1) return;
        auto& password = fields[0].value;
        Text hex;
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        for (unsigned char c : network.ssid) { hex += digits[c >> 4]; hex += digits[c & 15]; }
        network.profile = network.name;
        xml = L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\"><name>" +
            xmlText(network.profile) + L"</name><SSIDConfig><SSID><hex>" + hex +
            L"</hex></SSID></SSIDConfig><connectionType>ESS</connectionType><connectionMode>manual</connectionMode>"
            L"<MSM><security><authEncryption><authentication>" + Text(wpa3 ? L"WPA3SAE" : L"WPA2PSK") +
            L"</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption>"
            L"<sharedKey><keyType>passPhrase</keyType><protected>false</protected><keyMaterial>" +
            xmlText(password) + L"</keyMaterial></sharedKey></security></MSM></WLANProfile>";
        SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
    }
    HANDLE client{};
    DWORD version{};
    DWORD error = WlanOpenHandle(2, nullptr, &version, &client);
    if (error == ERROR_SUCCESS) {
        WLAN_CONNECTION_PARAMETERS parameters{};
        DOT11_SSID ssid{};
        if (!xml.empty()) {
            DWORD reason{};
            error = WlanSetProfile(client, &network.adapter, WLAN_PROFILE_USER, xml.c_str(), nullptr, FALSE, nullptr, &reason);
        }
        if (network.profile.empty()) {
            ssid.uSSIDLength = static_cast<ULONG>(network.ssid.size());
            memcpy(ssid.ucSSID, network.ssid.data(), ssid.uSSIDLength);
            parameters.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
            parameters.pDot11Ssid = &ssid;
            parameters.dot11BssType = static_cast<DOT11_BSS_TYPE>(network.bss);
        } else {
            parameters.wlanConnectionMode = wlan_connection_mode_profile;
            parameters.strProfile = network.profile.c_str();
            parameters.dot11BssType = dot11_BSS_type_any;
        }
        if (error == ERROR_SUCCESS) error = WlanConnect(client, &network.adapter, &parameters, nullptr);
        WlanCloseHandle(client, nullptr);
    }
    if (!xml.empty()) SecureZeroMemory(xml.data(), xml.size() * sizeof(wchar_t));
    if (error == ERROR_SUCCESS) {
        extra.wifiUpdateAt = GetTickCount64() + 1000;
        extra.wifiUpdateUntil = GetTickCount64() + 12000;
    }
    report(error == ERROR_SUCCESS ? L"Bağlantı isteği gönderildi." : L"Windows bağlantıyı başlatamadı (" + num(error) + L").");
}

namespace {
constexpr GUID GuidOverlayBetterBattery = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
constexpr GUID GuidOverlayBalanced      = {0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr GUID GuidSubEnergySaver       = {0xde830923, 0xa562, 0x41af, {0xa0, 0x86, 0xe3, 0xa2, 0xc6, 0xba, 0xd2, 0xda}};
constexpr GUID GuidEsBattThreshold      = {0xe69653ca, 0xcf7f, 0x4f05, {0xaa, 0x73, 0xcb, 0x83, 0x3f, 0xa9, 0x0a, 0xd4}};

using PowerGetActualOverlaySchemeFn = DWORD(WINAPI*)(GUID*);
using PowerSetActiveOverlaySchemeFn = DWORD(WINAPI*)(GUID);

bool getOverlayScheme(GUID& out) {
    static auto fn = reinterpret_cast<PowerGetActualOverlaySchemeFn>(
        GetProcAddress(GetModuleHandleW(L"powrprof.dll") ? GetModuleHandleW(L"powrprof.dll") : LoadLibraryW(L"powrprof.dll"),
            "PowerGetActualOverlayScheme"));
    return fn && fn(&out) == ERROR_SUCCESS;
}
bool setOverlayScheme(const GUID& g) {
    static auto fn = reinterpret_cast<PowerSetActiveOverlaySchemeFn>(
        GetProcAddress(GetModuleHandleW(L"powrprof.dll") ? GetModuleHandleW(L"powrprof.dll") : LoadLibraryW(L"powrprof.dll"),
            "PowerSetActiveOverlayScheme"));
    return fn && fn(g) == ERROR_SUCCESS;
}

constexpr const wchar_t* NightLightKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";

bool readNightLightBlob(std::vector<BYTE>& data) {
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NightLightKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    const LONG q = RegQueryValueExW(hKey, L"Data", nullptr, &type, nullptr, &size);
    if (q == ERROR_SUCCESS && type == REG_BINARY && size >= 24 && size <= 1024) {
        data.resize(size);
        if (RegQueryValueExW(hKey, L"Data", nullptr, &type, data.data(), &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return true;
        }
    }
    RegCloseKey(hKey);
    return false;
}

int findNightLightInnerHeader(const std::vector<BYTE>& data) {
    for (size_t i = 16; i + 6 < data.size(); ++i) {
        if (data[i] == 0x43 && data[i + 1] == 0x42 && data[i + 2] == 0x01 && data[i + 3] == 0x00)
            return static_cast<int>(i);
    }
    return -1;
}

bool isNightLightOn() {
    std::vector<BYTE> data;
    if (!readNightLightBlob(data)) return false;
    const int pos = findNightLightInnerHeader(data);
    if (pos < 0 || static_cast<size_t>(pos + 5) >= data.size()) return false;
    return data[pos + 4] == 0x10 && data[pos + 5] == 0x00;
}

bool processRunningByName(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) { found = true; break; }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

void sendWinCombo(WORD key, bool control) {
    INPUT keys[6]{};
    const WORD sequence[] = {VK_LWIN, VK_CONTROL, key};
    int count = 0;
    for (WORD code : sequence) {
        if (code == VK_CONTROL && !control) continue;
        keys[count].type = INPUT_KEYBOARD;
        keys[count++].ki.wVk = code;
    }
    for (int i = 0; i < count; ++i) {
        keys[count + i] = keys[count - i - 1];
        keys[count + i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(2 * count, keys, sizeof(INPUT));
}

// Undocumented-but-standard Windows PolicyConfig COM interface used by Core Audio switchers
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
}

void Dock::queryQuickStates() {
    // 1. Display projection topology
    UINT32 numPath = 0, numMode = 0;
    if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &numPath, &numMode) == ERROR_SUCCESS && numPath > 0) {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPath);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(numMode);
        DISPLAYCONFIG_TOPOLOGY_ID topo = DISPLAYCONFIG_TOPOLOGY_INTERNAL;
        if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &numPath, paths.data(), &numMode, modes.data(), &topo) == ERROR_SUCCESS) {
            extra.displayTopology = static_cast<UINT32>(topo);
        }
    }

    // 2. Energy saver / Power efficiency mode
    SYSTEM_POWER_STATUS sps{};
    bool saver = GetSystemPowerStatus(&sps) && sps.SystemStatusFlag != 0;
    GUID overlay{};
    if (getOverlayScheme(overlay) && IsEqualGUID(overlay, GuidOverlayBetterBattery)) saver = true;
    extra.energySaver = saver;

    // 3. Night light
    extra.nightLight = isNightLightOn();

    // 4. Nearby sharing
    DWORD nearShare = 0, sz = sizeof(nearShare);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\CDP",
        L"NearShareChannelUserAuthzPolicy", RRF_RT_REG_DWORD, nullptr, &nearShare, &sz) == ERROR_SUCCESS) {
        extra.nearbyShare = std::clamp(static_cast<int>(nearShare), 0, 2);
    } else {
        extra.nearbyShare = 0;
    }

    // 5. Accessibility
    auto& acc = extra.accessibility;
    acc.magnifier = FindWindowW(L"MagUIClass", nullptr) != nullptr || processRunningByName(L"Magnify.exe");
    DWORD cf = 0; sz = sizeof(cf);
    acc.colorFilter = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\ColorFiltering",
        L"Active", RRF_RT_REG_DWORD, nullptr, &cf, &sz) == ERROR_SUCCESS && cf != 0;
    acc.narrator = FindWindowW(L"NarratorHelperWindow", nullptr) != nullptr || processRunningByName(L"Narrator.exe");
    acc.liveCaptions = FindWindowW(L"LiveCaptionsDesktopWindow", nullptr) != nullptr || processRunningByName(L"LiveCaptions.exe");
    STICKYKEYS sk{sizeof(sk)};
    acc.stickyKeys = SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0) && (sk.dwFlags & SKF_STICKYKEYSON) != 0;
    HIGHCONTRASTW hc{sizeof(hc)};
    acc.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    DWORD mono = 0; sz = sizeof(mono);
    acc.monoAudio = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Multimedia\\Audio",
        L"AccessibilityMonoMixState", RRF_RT_REG_DWORD, nullptr, &mono, &sz) == ERROR_SUCCESS && mono != 0;
}

void Dock::toggleEnergySaver() {
    const bool enable = !extra.energySaver;
    GUID* activeScheme{};
    const bool gotScheme = PowerGetActiveScheme(nullptr, &activeScheme) == ERROR_SUCCESS && activeScheme != nullptr;
    if (enable) {
        GUID cur{};
        if (getOverlayScheme(cur) && !IsEqualGUID(cur, GuidOverlayBetterBattery)) {
            extra.prevPowerOverlay = cur;
            extra.hasPrevPowerOverlay = true;
        }
        setOverlayScheme(GuidOverlayBetterBattery);
        if (gotScheme) {
            DWORD dcVal = 20;
            if (PowerReadDCValueIndex(nullptr, activeScheme, &GuidSubEnergySaver, &GuidEsBattThreshold, &dcVal) == ERROR_SUCCESS && dcVal < 100) {
                extra.prevDcSaverThreshold = dcVal;
                extra.hasPrevDcThreshold = true;
            }
            PowerWriteDCValueIndex(nullptr, activeScheme, &GuidSubEnergySaver, &GuidEsBattThreshold, 100);
            PowerWriteACValueIndex(nullptr, activeScheme, &GuidSubEnergySaver, &GuidEsBattThreshold, 100);
            PowerSetActiveScheme(nullptr, activeScheme);
        }
    } else {
        setOverlayScheme(extra.hasPrevPowerOverlay ? extra.prevPowerOverlay : GuidOverlayBalanced);
        if (gotScheme) {
            const DWORD restoreDc = extra.hasPrevDcThreshold ? extra.prevDcSaverThreshold : 20;
            PowerWriteDCValueIndex(nullptr, activeScheme, &GuidSubEnergySaver, &GuidEsBattThreshold, restoreDc);
            PowerWriteACValueIndex(nullptr, activeScheme, &GuidSubEnergySaver, &GuidEsBattThreshold, 0);
            PowerSetActiveScheme(nullptr, activeScheme);
        }
    }
    if (activeScheme) LocalFree(activeScheme);
    queryQuickStates();
    paintSheet();
}

void Dock::setDisplayTopology(UINT32 topoFlag) {
    const LONG res = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | topoFlag);
    if (res == ERROR_SUCCESS) {
        queryQuickStates();
        refreshMonitors();
        position();
        const wchar_t* label = topoFlag == SDC_TOPOLOGY_INTERNAL ? L"Yalnızca bilgisayar ekranı seçildi."
            : topoFlag == SDC_TOPOLOGY_CLONE ? L"Ekranlar yinelendi."
            : topoFlag == SDC_TOPOLOGY_EXTEND ? L"Ekranlar uzatıldı."
            : L"Yalnızca ikinci ekran seçildi.";
        report(label);
    } else {
        report(L"İkinci ekran algılanmadı veya mod uygulanamadı.");
    }
}

void Dock::toggleNightLight() {
    std::vector<BYTE> data;
    if (!readNightLightBlob(data)) {
        launch(L"ms-settings:nightlight");
        return;
    }
    const int pos = findNightLightInnerHeader(data);
    if (pos < 1 || static_cast<size_t>(pos + 6) >= data.size()) {
        launch(L"ms-settings:nightlight");
        return;
    }
    const bool currentlyOn = data[pos + 4] == 0x10 && data[pos + 5] == 0x00;
    if (currentlyOn) {
        data.erase(data.begin() + pos + 4, data.begin() + pos + 6);
        if (data[pos - 1] >= 2) data[pos - 1] -= 2;
    } else {
        data.insert(data.begin() + pos + 4, {0x10, 0x00});
        data[pos - 1] += 2;
    }
    // Increment the CloudStore 7-bit varint timestamp starting at byte 10 so Windows Shell applies it immediately.
    for (size_t i = 10; i < 15 && i < data.size(); ++i) {
        const BYTE low = data[i] & 0x7F;
        if (low < 0x7F) {
            data[i] = static_cast<BYTE>((data[i] & 0x80) | (low + 1));
            break;
        }
        data[i] = static_cast<BYTE>(data[i] & 0x80);
    }
    HKEY hKey{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, NightLightKey, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        const LONG wr = RegSetValueExW(hKey, L"Data", 0, REG_BINARY, data.data(), static_cast<DWORD>(data.size()));
        RegCloseKey(hKey);
        if (wr == ERROR_SUCCESS) {
            queryQuickStates();
            paintSheet();
            return;
        }
    }
    launch(L"ms-settings:nightlight");
}

void Dock::setNearbyShare(int mode) {
    mode = std::clamp(mode, 0, 2);
    HKEY hKey{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\CDP",
        0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        DWORD val = static_cast<DWORD>(mode);
        RegSetValueExW(hKey, L"NearShareChannelUserAuthzPolicy", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&val), sizeof(val));
        if (mode > 0) {
            DWORD one = 1;
            RegSetValueExW(hKey, L"CdpSessionUserAuthzPolicy", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegSetValueExW(hKey, L"RomeSdkChannelUserAuthzPolicy", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        }
        RegCloseKey(hKey);
    }
    queryQuickStates();
    paintSheet();
}

void Dock::toggleAccessibility(int item) {
    queryQuickStates();
    switch (item) {
    case 0: // Büyüteç (Magnifier)
        if (extra.accessibility.magnifier) {
            if (HWND mag = FindWindowW(L"MagUIClass", nullptr)) PostMessageW(mag, WM_CLOSE, 0, 0);
            else sendWinCombo(VK_ESCAPE, false);
        } else {
            ShellExecuteW(nullptr, L"open", L"Magnify.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
        extra.accessibility.magnifier = !extra.accessibility.magnifier;
        break;
    case 1: { // Renk filtreleri (Color filters)
        HKEY hKey{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\ColorFiltering",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            DWORD one = 1;
            RegSetValueExW(hKey, L"HotkeyEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegCloseKey(hKey);
        }
        sendWinCombo('C', true);
        extra.accessibility.colorFilter = !extra.accessibility.colorFilter;
        break;
    }
    case 2: // Ekran Okuyucusu (Narrator)
        if (extra.accessibility.narrator) {
            sendWinCombo(VK_RETURN, true);
        } else {
            ShellExecuteW(nullptr, L"open", L"Narrator.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
        extra.accessibility.narrator = !extra.accessibility.narrator;
        break;
    case 3: // Canlı alt yazılar (Live captions)
        if (extra.accessibility.liveCaptions) {
            if (HWND lc = FindWindowW(L"LiveCaptionsDesktopWindow", nullptr)) PostMessageW(lc, WM_CLOSE, 0, 0);
            else sendWinCombo('L', true);
        } else {
            ShellExecuteW(nullptr, L"open", L"LiveCaptions.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
        extra.accessibility.liveCaptions = !extra.accessibility.liveCaptions;
        break;
    case 4: { // Yapışkan Tuşlar (Sticky Keys)
        STICKYKEYS sk{sizeof(sk)};
        if (SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0)) {
            if (sk.dwFlags & SKF_STICKYKEYSON) sk.dwFlags &= ~SKF_STICKYKEYSON;
            else sk.dwFlags |= SKF_STICKYKEYSON;
            SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(sk), &sk, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        }
        queryQuickStates();
        break;
    }
    case 5: { // Yüksek karşıtlık (High Contrast)
        HIGHCONTRASTW hc{sizeof(hc)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
            if (hc.dwFlags & HCF_HIGHCONTRASTON) hc.dwFlags &= ~HCF_HIGHCONTRASTON;
            else hc.dwFlags |= HCF_HIGHCONTRASTON;
            SystemParametersInfoW(SPI_SETHIGHCONTRAST, sizeof(hc), &hc, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        }
        queryQuickStates();
        break;
    }
    case 6: { // Mono ses (Mono audio)
        const DWORD next = extra.accessibility.monoAudio ? 0 : 1;
        HKEY hKey{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Multimedia\\Audio",
            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, L"AccessibilityMonoMixState", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&next), sizeof(next));
            RegCloseKey(hKey);
            SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Audio"));
        }
        queryQuickStates();
        break;
    }
    }
    paintSheet();
}

void Dock::refreshAudioEndpoints() {
    extra.audioDevices.clear();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) || !enumerator)
        return;
    Text defaultId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)) && def) {
            LPWSTR id{};
            if (SUCCEEDED(def->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
        }
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) || !collection) return;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        LPWSTR rawId{};
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;
        Text id = rawId;
        CoTaskMemFree(rawId);
        Text name = endpointName(*device.Get());
        if (name.empty()) name = L"Ses çıkışı";
        extra.audioDevices.push_back({id, name, id == defaultId});
    }
    std::stable_sort(extra.audioDevices.begin(), extra.audioDevices.end(), [](const auto& a, const auto& b) {
        return a.active > b.active;
    });
}

bool setDefaultAudioEndpointId(const Text& deviceId) {
    if (deviceId.empty()) return false;
    ComPtr<IPolicyConfig> policy;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy))) || !policy)
        return false;
    const HRESULT hr1 = policy->SetDefaultEndpoint(deviceId.c_str(), eConsole);
    const HRESULT hr2 = policy->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
    const HRESULT hr3 = policy->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
    return SUCCEEDED(hr1) || SUCCEEDED(hr2) || SUCCEEDED(hr3);
}

void Dock::setAudioEndpoint(const Text& deviceId) {
    if (deviceId.empty()) return;
    if (setDefaultAudioEndpointId(deviceId)) {
        releaseAudioEndpoint();
        audio(extra, 0, false);
        if (extra.volume >= 0) {
            barVolumeTarget = extra.muted ? 0.f : std::clamp(extra.volume / 100.f, 0.f, 1.f);
            barVolumeLevel = barVolumeTarget;
        }
        refreshAudioEndpoints();
        paintBar();
        paintSheet();
    } else {
        report(L"Ses çıkışı değiştirilemedi.");
    }
}

void Dock::refreshMicEndpoints() {
    extra.micDevices.clear();
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) || !enumerator)
        return;
    Text defaultId;
    ComPtr<IMMDevice> def;
    if ((SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, &def)) ||
         SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &def))) && def) {
        LPWSTR raw{};
        if (SUCCEEDED(def->GetId(&raw)) && raw) { defaultId = raw; CoTaskMemFree(raw); }
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) || !collection) return;
    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        LPWSTR rawId{};
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;
        Text id = rawId;
        CoTaskMemFree(rawId);
        Text name = endpointName(*device.Get());
        if (name.empty()) name = L"Mikrofon";
        extra.micDevices.push_back({id, name, id == defaultId});
    }
    std::stable_sort(extra.micDevices.begin(), extra.micDevices.end(), [](const auto& a, const auto& b) {
        return a.active > b.active;
    });
}

void Dock::setMicEndpoint(const Text& deviceId) {
    if (deviceId.empty()) return;
    if (setDefaultAudioEndpointId(deviceId)) {
        sampleMicrophone();
        refreshMicEndpoints();
        paintBar();
        paintSheet();
    } else {
        report(L"Mikrofon aygıtı değiştirilemedi.");
    }
}

void Dock::openQuickSub(QuickSubPanel sub) {
    extra.quickSub = sub;
    listOffset = 0;
    sheet.resetMotion();
    triggerSheetContentAnim(22.f, 0.f);
    if (sub == QuickSubPanel::Wifi) {
        extra.wifiShowSaved = false;
        refreshWifi(true, true);
        queryRadios();
    } else if (sub == QuickSubPanel::Bluetooth) {
        queryBluetoothDevices();
        queryRadios();
    } else if (sub == QuickSubPanel::Project || sub == QuickSubPanel::Accessibility || sub == QuickSubPanel::NearbyShare) {
        queryQuickStates();
    } else if (sub == QuickSubPanel::AudioOutput) {
        refreshAudioEndpoints();
    }
    positionSheet();
}

void Dock::closeQuickSub() {
    if (extra.quickSub == QuickSubPanel::Main) return;
    extra.quickSub = QuickSubPanel::Main;
    listOffset = 0;
    sheet.resetMotion();
    triggerSheetContentAnim(-22.f, 0.f);
    queryQuickStates();
    queryMedia();
    positionSheet();
}
Text Dock::worldTime() {
    if (extra.zoneCity != model.worldCity) {
        extra.zoneCity = model.worldCity;
        extra.zoneFound = false;
        for (DWORD index = 0; EnumDynamicTimeZoneInformation(index, &extra.zone) == ERROR_SUCCESS; ++index)
            if (wcscmp(extra.zone.TimeZoneKeyName, Cities[model.worldCity].zone) == 0) {
                extra.zoneFound = true;
                break;
            }
    }
    SYSTEMTIME utc{}, local{};
    GetSystemTime(&utc);
    if (!extra.zoneFound || !SystemTimeToTzSpecificLocalTimeEx(&extra.zone, &utc, &local)) return L"—";
    return two(local.wHour) + L":" + two(local.wMinute);
}
Text Dock::worldOffsetText(bool* isDaylight) {
    worldTime();
    SYSTEMTIME utc{}, targetLocal{}, hereLocal{};
    GetSystemTime(&utc);
    GetLocalTime(&hereLocal);
    if (!extra.zoneFound || !SystemTimeToTzSpecificLocalTimeEx(&extra.zone, &utc, &targetLocal)) {
        if (isDaylight) *isDaylight = true;
        return L"Bugün, 0 sa";
    }
    if (isDaylight) *isDaylight = targetLocal.wHour >= 6 && targetLocal.wHour < 19;
    FILETIME ftTarget{}, ftHere{};
    SystemTimeToFileTime(&targetLocal, &ftTarget);
    SystemTimeToFileTime(&hereLocal, &ftHere);
    const LONGLONG tVal = (static_cast<LONGLONG>(ftTarget.dwHighDateTime) << 32) | ftTarget.dwLowDateTime;
    const LONGLONG hVal = (static_cast<LONGLONG>(ftHere.dwHighDateTime) << 32) | ftHere.dwLowDateTime;
    const LONGLONG diffMinutes = (tVal - hVal) / (60LL * 10000000LL);
    const int diffHours = static_cast<int>(std::lround(diffMinutes / 60.0));
    const int targetDayKey = Date::from(targetLocal).key();
    const int hereDayKey = Date::from(hereLocal).key();
    const wchar_t* dayLabel = targetDayKey > hereDayKey ? L"Yarın" : targetDayKey < hereDayKey ? L"Dün" : L"Bugün";
    if (diffHours == 0) return Text(dayLabel) + L", 0 sa";
    return Text(dayLabel) + L", " + (diffHours > 0 ? L"+" : L"−") + num(std::abs(diffHours)) + L" sa";
}
Text Dock::tempText(bool preview) const {
    if (extra.forecast) return num(extra.forecast->current) + L"°";
    return preview ? L"22°" : L"—°";
}
std::shared_ptr<Gdiplus::Bitmap> appImage(const Text& name) {
    if (name.empty() || name.find(L"://") != Text::npos || name.starts_with(L"\\\\")) return {};
    if (auto cached = lookupCachedAppImage(name)) return cached;
    wchar_t path[1024]{};
    const DWORD length = name.find_first_of(L"\\/:") == Text::npos ?
        SearchPathW(nullptr, name.c_str(), nullptr, ARRAYSIZE(path), path, nullptr) : 0;
    std::shared_ptr<Gdiplus::Bitmap> bmp = highResBitmapForPath(length && length < ARRAYSIZE(path) ? path : name.c_str());
    if (bmp) storeCachedAppImage(name, bmp);
    return bmp;
}
void Dock::refreshAppImages() {
    appImages.assign(model.shortcuts.size(), {});
    appImageChecked.assign(model.shortcuts.size(), false);
    for (size_t i = 0; i < model.shortcuts.size(); ++i) {
        auto& shortcut = model.shortcuts[i];
        if (shortcut.identity.empty() && shortcut.target.ends_with(L".exe") &&
            shortcut.target.find_first_of(L"\\/:") == Text::npos) {
            wchar_t path[1024]{};
            const DWORD length = SearchPathW(nullptr, shortcut.target.c_str(), nullptr, ARRAYSIZE(path), path, nullptr);
            if (length && length < ARRAYSIZE(path)) shortcut.identity = path;
        }
        appImages[i] = lookupCachedAppImage(shortcut.target);
        if (appImages[i]) appImageChecked[i] = true;
    }
    requestAppImages();
}
void Dock::requestAppImages() {
    if (!hwnd || appImagesRequest.valid()) return;
    if (appImageChecked.size() != model.shortcuts.size()) appImageChecked.resize(model.shortcuts.size(), false);
    std::vector<Text> missing;
    std::vector<size_t> missingIdx;
    for (size_t i = 0; i < model.shortcuts.size(); ++i) {
        if ((i < appImages.size() && appImages[i]) || appImageChecked[i]) continue;
        const auto& target = model.shortcuts[i].target;
        if (target.empty() || target.find(L"://") != Text::npos || target.starts_with(L"\\\\")) {
            appImageChecked[i] = true;
            continue;
        }
        missing.push_back(target);
        missingIdx.push_back(i);
    }
    if (missing.empty()) return;
    for (const size_t i : missingIdx) appImageChecked[i] = true;
    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    try {
        appImagesRequest = std::async(std::launch::async, [missing = std::move(missing), stop, notify] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::vector<IndexedBitmap> loaded;
            loaded.reserve(missing.size());
            try {
                for (const auto& target : missing) {
                    if (stop && stop->load()) break;
                    if (auto bmp = appImage(target)) loaded.push_back({target, std::move(bmp)});
                }
            } catch (const std::exception&) {}
            if (SUCCEEDED(apartment)) CoUninitialize();
            if (notify && (!stop || !stop->load())) PostMessageW(notify, AsyncWake, 0, 0);
            return loaded;
        });
    } catch (const std::exception&) {}
}
void Dock::pollAppImages() {
    if (!appImagesRequest.valid() ||
        appImagesRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    bool updated = false;
    try {
        auto loaded = appImagesRequest.get();
        if (appImages.size() != model.shortcuts.size()) appImages.resize(model.shortcuts.size());
        for (auto& item : loaded) {
            for (size_t i = 0; i < model.shortcuts.size(); ++i) {
                if (CompareStringOrdinal(model.shortcuts[i].target.c_str(), -1, item.key.c_str(), -1, TRUE) == CSTR_EQUAL &&
                    !appImages[i] && item.bitmap) {
                    appImages[i] = item.bitmap;
                    updated = true;
                }
            }
        }
    } catch (const std::exception&) {}
    if (updated) {
        if (anyDockVisible()) paintBar();
        if (panel && (page == Studio || page == Apps || page == Search || page == Commands || page == Overflow))
            paintSheet();
    }
    requestAppImages();
}
void Dock::requestInstalledAppImages(const std::vector<Text>& targets) {
    if (!hwnd || targets.empty() || installedImagesRequest.valid()) return;
    std::vector<Text> batch;
    for (const auto& target : targets) {
        if (target.empty()) continue;
        if (std::find(installedImageRequested.begin(), installedImageRequested.end(), target) != installedImageRequested.end())
            continue;
        installedImageRequested.push_back(target);
        batch.push_back(target);
        if (batch.size() == 8) break;
    }
    if (batch.empty()) return;
    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    try {
        installedImagesRequest = std::async(std::launch::async, [batch = std::move(batch), stop, notify] {
            const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::vector<IndexedBitmap> loaded;
            loaded.reserve(batch.size());
            try {
                for (const auto& target : batch) {
                    if (stop && stop->load()) break;
                    if (auto bmp = appImage(target)) loaded.push_back({target, std::move(bmp)});
                }
            } catch (const std::exception&) {}
            if (SUCCEEDED(apartment)) CoUninitialize();
            if (notify && (!stop || !stop->load())) PostMessageW(notify, AsyncWake, 0, 0);
            return loaded;
        });
    } catch (const std::exception&) {}
}
void Dock::pollInstalledAppImages() {
    if (!installedImagesRequest.valid() ||
        installedImagesRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    bool updated = false;
    try {
        auto loaded = installedImagesRequest.get();
        if (installedImages.size() != installedApps.size()) installedImages.resize(installedApps.size());
        for (auto& item : loaded) {
            for (size_t i = 0; i < installedApps.size(); ++i) {
                if (installedApps[i].target == item.key && !installedImages[i] && item.bitmap) {
                    installedImages[i] = item.bitmap;
                    updated = true;
                }
            }
        }
    } catch (const std::exception&) {}
    if (updated && panel && !sheetClosing && (page == Apps || page == Search || page == Commands))
        paintSheet();
}
void Dock::toggleTile(int id) {
    auto& layout = model.layouts[model.profile];
    auto tile = std::find_if(layout.begin(), layout.end(), [id](Tile item) { return item.id == id; });
    if (tile == layout.end()) return;
    tile->enabled = !tile->enabled;
    if (tile->enabled && (id == System || id == CpuMeter || id == RamMeter || id == Git)) sample();
    if (tile->enabled) {
        sampleExtras(true);
        animateWidgets();
    }
    changed(true);
}
bool Dock::tickExtras(ULONGLONG stamp, const SYSTEMTIME& local) {
    bool dirty = tickProductivity(stamp);
    pollTrayCapture();
    pollAppImages();
    pollInstalledAppImages();
    pollJumpList();
    if (extra.windowIconsRequest.valid() &&
        extra.windowIconsRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            for (auto& [id, icon] : extra.windowIconsRequest.get()) {
                const auto win = std::find_if(extra.windows.begin(), extra.windows.end(),
                    [id](const OpenWindow& item) { return item.iconRequestId == id; });
                if (win != extra.windows.end() && icon) {
                    win->icon = std::move(icon);
                    dirty = true;
                }
            }
        } catch (const std::exception&) {}
        requestWindowIcons();
    }
    if (extra.mediaRequest.valid() && extra.mediaRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto state = extra.mediaRequest.get();
        const bool hadTitle = !extra.media.title.empty();
        const bool hasTitle = !state.title.empty();
        const bool mediaChanged = state.title != extra.media.title || state.artist != extra.media.artist ||
            state.playing != extra.media.playing || state.cover != extra.media.cover ||
            state.canPrev != extra.media.canPrev || state.canNext != extra.media.canNext ||
            state.canSeek != extra.media.canSeek || state.durationTicks != extra.media.durationTicks ||
            std::abs(state.progress(stamp) - extra.media.progress(stamp)) > .002f;
        extra.media = std::move(state);
        if (panel && page == Quick && extra.quickSub == QuickSubPanel::Main && hadTitle != hasTitle) {
            positionSheet();
        } else dirty |= mediaChanged;
    }
    if (extra.mediaActionRequest.valid() && extra.mediaActionRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { extra.mediaActionRequest.get(); } catch (const std::exception&) {}
    }
    if (extra.mediaRefreshAt && stamp >= extra.mediaRefreshAt) {
        extra.mediaRefreshAt = 0;
        queryMedia();
    }
    dirty |= tickProWidgets(stamp);
    if (extra.radioRequest.valid() && extra.radioRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const int prevBt = extra.radios.bluetooth;
        try { extra.radios = extra.radioRequest.get(); }
        catch (const std::exception&) { extra.radios.denied = true; }
        if (shown(QuickToggles) || (model.statusBar.enabled && model.statusBar.network)) dirty = true;
        if (page == Quick) {
            refreshWifi(false, extra.quickSub == QuickSubPanel::Wifi);
            if (extra.quickSub == QuickSubPanel::Bluetooth && (extra.radios.bluetooth != prevBt || extra.btDevices.empty()))
                queryBluetoothDevices();
            if (extra.radios.denied) report(L"Windows kablosuz denetime izin vermedi.");
            dirty = true;
        }
    }
    if (extra.wifiRequest.valid() && extra.wifiRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        extra.wifiScanning = false;
        try {
            auto res = extra.wifiRequest.get();
            extra.wifi = std::move(res.available);
            extra.savedWifi = std::move(res.saved);
            extra.wifiError = res.error;
            extra.wifiConnected = res.connected;
            if (res.connected && !res.connectedName.empty()) {
                extra.radios.network = res.connectedName;
                extra.radios.adapter = res.connectedAdapter;
            }
            if (extra.wifiConnected && !extra.radios.network.empty()) {
                for (auto& network : extra.wifi) {
                    if (IsEqualGUID(network.adapter, extra.radios.adapter) &&
                        (network.name == extra.radios.network || network.profile == extra.radios.network))
                        network.connected = true;
                }
            }
        } catch (const std::exception&) {}
        dirty = true;
    }
    if (extra.btRequest.valid() && extra.btRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        extra.btLoading = false;
        try { extra.btDevices = extra.btRequest.get(); }
        catch (const std::exception&) {}
        dirty = true;
    }
    if (extra.btActionRequest.valid() && extra.btActionRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        extra.btBusyAddress = 0;
        extra.btBusyConnect = false;
        BtActionResult res;
        try { res = extra.btActionRequest.get(); }
        catch (const std::exception&) {}
        for (auto& dev : extra.btDevices) {
            if (res.address != 0 && dev.address == res.address) {
                dev.connected = res.connected;
                break;
            }
        }
        if (res.audioChanged) {
            releaseAudioEndpoint();
            audio(extra, 0, false);
            if (extra.volume >= 0) {
                barVolumeTarget = extra.muted ? 0.f : std::clamp(extra.volume / 100.f, 0.f, 1.f);
                barVolumeLevel = barVolumeTarget;
            }
            refreshAudioEndpoints();
        }
        if (!res.message.empty()) report(std::move(res.message));
        if (page == Quick && extra.quickSub == QuickSubPanel::Bluetooth) queryBluetoothDevices();
        dirty = true;
    }
    if (page == Quick && extra.wifiUpdateAt && stamp >= extra.wifiUpdateAt) {
        extra.wifiUpdateAt = stamp < extra.wifiUpdateUntil ? stamp + 2000 : 0;
        refreshWifi(false, extra.quickSub == QuickSubPanel::Wifi);
        queryRadios();
        dirty = true;
    }
    if (extra.watchStart && (shown(Stopwatch) || page == Stopwatch)) dirty = true;
    const Date today = Date::from(local);
    const int key = today.key();
    if (model.usageDay != key) {
        model.rollUsageDay(today);
        extra.usageTotal = 0;
        extra.lastUsage = 0;
        extra.usageDirty = true;
        dirty = true;
    }
    if (page == Usage || page == DayTimeline || shown(Usage) || shown(DayTimeline)) {
        if (extra.lastUsage && stamp - extra.lastUsage >= 900 && stamp - extra.lastUsage < 3500) {
            LASTINPUTINFO input{sizeof(input)};
            const HWND foreground = GetForegroundWindow();
            if (foreground && foreground != panel && foreground != hwnd &&
                GetLastInputInfo(&input) && static_cast<DWORD>(stamp) - input.dwTime < 60000) {
                auto& seconds = model.usageSeconds[local.wHour];
                const int add = std::min(3600 - seconds, static_cast<int>((stamp - extra.lastUsage + 100) / 1000));
                seconds += add;
                extra.usageTotal += add;
                const auto window = std::find_if(extra.windows.begin(), extra.windows.end(),
                    [&](const auto& item) { return item.handle == foreground; });
                int category = 3;
                if (window != extra.windows.end()) {
                    Text name = window->app.target + L" " + window->app.label;
                    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return std::towlower(ch); });
                    const auto has = [&](std::initializer_list<const wchar_t*> words) {
                        return std::any_of(words.begin(), words.end(), [&](const wchar_t* word) { return name.find(word) != Text::npos; });
                    };
                    if (has({L"chrome", L"firefox", L"msedge", L"brave", L"opera", L"vivaldi"})) category = 0;
                    else if (has({L"code", L"devenv", L"word", L"excel", L"terminal", L"antigravity", L"notepad"})) category = 1;
                    else if (has({L"spotify", L"vlc", L"media", L"steam", L"foobar"})) category = 2;
                }
                model.timelineUsage[category][local.wHour] = std::min(3600, model.timelineUsage[category][local.wHour] + add);
                if (add > 0 && window != extra.windows.end() && !window->app.target.empty()) {
                    const auto app = appKey(window->app);
                    auto& apps = model.usageApps;
                    const auto found = std::find_if(apps.begin(), apps.end(), [&](const auto& item) {
                        return item.day == key && CompareStringOrdinal(item.key.c_str(), -1, app.c_str(), -1, TRUE) == CSTR_EQUAL;
                    });
                    if (found != apps.end()) found->seconds += add;
                    else if (std::count_if(apps.begin(), apps.end(), [&](const auto& item) { return item.day == key; }) < MaxUsageAppsPerDay)
                        apps.push_back({key, add, app, window->app.label});
                }
                extra.usageDirty |= add > 0;
                dirty |= add > 0 && (page == Usage || page == DayTimeline || extra.usageTotal % 60 < add);
            }
        }
        extra.lastUsage = stamp;
    } else extra.lastUsage = 0;
    if (extra.usageDirty && stamp - extra.usageSaved >= 60000) {
        if (model.save()) extra.usageDirty = false;
        extra.usageSaved = stamp;
    }
    if (extra.weatherRequest.valid() && extra.weatherRequest.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto result = extra.weatherRequest.get();
        if (extra.requestedCity == model.weatherCity) {
            extra.weatherError = !result;
            if (result) { extra.forecast = std::move(result); animateWidgets(); }
            extra.weatherAt = stamp + (extra.weatherError ? 180000 : 900000);
        } else extra.weatherAt = 0;
        if (weatherInUse()) requestWeather();
        dirty = true;
    }
    return dirty;
}

void Dock::requestWeather(bool refresh) {
    if (model.weatherCity < 0 || extra.weatherRequest.valid() || (!refresh && GetTickCount64() < extra.weatherAt)) return;
    extra.requestedCity = model.weatherCity;
    extra.weatherError = false;
    try { extra.weatherRequest = std::async(std::launch::async, readWeather, model.weatherCity); }
    catch (const std::system_error&) { extra.weatherError = true; extra.weatherAt = GetTickCount64() + 180000; }
}

