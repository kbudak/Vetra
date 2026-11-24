#include "dock.h"
#include <appmodel.h>
#include <dwmapi.h>
#include <propkey.h>
#include <shldisp.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr auto AppsPrefix = L"shell:AppsFolder\\";
constexpr size_t OnBarPins = 8, MaxRunningOnBar = 16;

std::mutex gLabelCacheMutex;
std::unordered_map<Text, Text> gLabelCache;

Text lowerText(const Text& s) {
    Text out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return out;
}

bool same(const Text& a, const Text& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

Text property(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value{};
    Text text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) text = value.pwszVal;
    PropVariantClear(&value);
    return text;
}

Text shellName(IShellItem* item, SIGDN type) {
    PWSTR value{};
    if (FAILED(item->GetDisplayName(type, &value))) return {};
    Text text(value);
    CoTaskMemFree(value);
    return text;
}

Text processPath(HANDLE process) {
    wchar_t path[32768];
    DWORD length = ARRAYSIZE(path);
    return QueryFullProcessImageNameW(process, 0, path, &length) ? Text(path, length) : Text{};
}

Text processAumid(DWORD pid) {
    Text result;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t id[256]{};
        UINT32 length = ARRAYSIZE(id);
        if (GetApplicationUserModelId(process, &length, id) == ERROR_SUCCESS) result = id;
        CloseHandle(process);
    }
    return result;
}

Text installedLabel(const Text& target) {
    if (target.empty()) return {};
    const Text cacheKey = lowerText(target);
    {
        std::lock_guard lock(gLabelCacheMutex);
        if (const auto it = gLabelCache.find(cacheKey); it != gLabelCache.end()) return it->second;
    }
    Text result;
    ComPtr<IShellItem2> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item)))) {
        if (!target.starts_with(AppsPrefix)) {
            PWSTR description{};
            if (SUCCEEDED(item->GetString(PKEY_FileDescription, &description))) {
                const Text label(description);
                CoTaskMemFree(description);
                if (!label.empty()) result = label;
            }
        }
        if (result.empty()) result = shellName(item.Get(), SIGDN_NORMALDISPLAY);
    }
    if (result.empty()) result = leaf(target);
    {
        std::lock_guard lock(gLabelCacheMutex);
        if (gLabelCache.size() > 512) gLabelCache.clear();
        gLabelCache[cacheKey] = result;
    }
    return result;
}

bool contains(const Text& text, const Text& query) {
    if (query.empty()) return true;
    return FindNLSStringEx(L"tr-TR", FIND_FROMSTART | NORM_IGNORECASE | NORM_IGNORENONSPACE,
        text.c_str(), -1, query.c_str(), -1, nullptr, nullptr, nullptr, 0) >= 0 ||
        FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE, text.c_str(), -1,
            query.c_str(), -1, nullptr, nullptr, nullptr, 0) >= 0;
}

Text encodeQuery(const Text& text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    Text result;
    constexpr auto hex = L"0123456789ABCDEF";
    for (unsigned char ch : bytes) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') result += ch;
        else { result += L'%'; result += hex[ch >> 4]; result += hex[ch & 15]; }
    }
    return result;
}

struct Command { const wchar_t* title; const wchar_t* shortcut; const wchar_t* target; };
constexpr Command CommandsList[] = {
    {L"Terminal", L"terminal", L"wt.exe"}, {L"Not Defteri", L"not", L"notepad.exe"},
    {L"Dosya Gezgini", L"dosyalar", L"explorer.exe"}, {L"Hesap Makinesi", L"hesap", L"calc.exe"},
    {L"Windows ayarları", L"ayarlar", L"ms-settings:"}, {L"Görev Yöneticisi", L"görev", L"taskmgr.exe"},
};
}

Text appKey(const Shortcut& app) { return app.identity.empty() ? app.target : app.identity; }

Shortcut windowApp(HWND window, bool resolveLabel) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    Text path;
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        path = processPath(process);
        CloseHandle(process);
    }
    ComPtr<IPropertyStore> store;
    Text id;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store))))
        id = property(store.Get(), PKEY_AppUserModel_ID);
    if (id.empty()) id = processAumid(pid);
    if (id.empty() && same(leaf(path), L"ApplicationFrameHost.exe")) {
        EnumChildWindows(window, [](HWND child, LPARAM output) -> BOOL {
            DWORD childPid{};
            GetWindowThreadProcessId(child, &childPid);
            auto& aumid = *reinterpret_cast<Text*>(output);
            aumid = processAumid(childPid);
            return aumid.empty();
        }, reinterpret_cast<LPARAM>(&id));
    }
    if (!id.empty()) {
        const Text target = AppsPrefix + id;
        if (id.find(L'!') != Text::npos) {
            return {resolveLabel ? installedLabel(target) : Text{}, target, id};
        }
        if (resolveLabel || path.empty()) {
            const auto label = installedLabel(target);
            if (label != leaf(target)) return {label, target, id};
        } else {
            // Fast path during window discovery: avoid Shell COM disk reads while keeping identity matchable.
            const Text cacheKey = lowerText(target);
            std::lock_guard lock(gLabelCacheMutex);
            if (const auto it = gLabelCache.find(cacheKey); it != gLabelCache.end() && it->second != leaf(target))
                return {it->second, target, id};
        }
    }
    if (path.empty() || same(leaf(path), L"ApplicationFrameHost.exe")) return {};
    return {resolveLabel ? installedLabel(path) : leaf(path), path, path};
}

static std::vector<Shortcut> readInstalledApps(const std::shared_ptr<std::atomic<bool>>& stop = {}) {
    std::vector<Shortcut> installedApps;
    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> entries;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder))) ||
        FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&entries)))) return installedApps;
    ComPtr<IShellItem> item;
    while (entries->Next(1, item.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        if (stop && stop->load()) return {};
        ComPtr<IShellItem2> details;
        if (FAILED(item.As(&details))) continue;
        PWSTR appId{};
        Text id;
        if (SUCCEEDED(details->GetString(PKEY_AppUserModel_ID, &appId))) {
            id = appId;
            CoTaskMemFree(appId);
        } else id = shellName(item.Get(), SIGDN_PARENTRELATIVEPARSING);
        const auto name = shellName(item.Get(), SIGDN_NORMALDISPLAY);
        if (id.empty() || name.empty()) continue;
        PWSTR linkPath{};
        Text identity = id;
        if (id.find(L'!') == Text::npos && SUCCEEDED(details->GetString(PKEY_Link_TargetParsingPath, &linkPath))) {
            const Text target(linkPath);
            CoTaskMemFree(linkPath);
            if (target.ends_with(L".exe") || target.ends_with(L".EXE")) identity = target;
        }
        if (std::none_of(installedApps.begin(), installedApps.end(), [&](const Shortcut& listed) { return same(listed.target, AppsPrefix + id); }))
            installedApps.push_back({name, AppsPrefix + id, identity});
    }
    std::sort(installedApps.begin(), installedApps.end(), [](const Shortcut& a, const Shortcut& b) {
        return CompareStringEx(L"tr-TR", NORM_IGNORECASE, a.label.c_str(), -1, b.label.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    return installedApps;
}

void Dock::loadApps() {
    const auto stamp = GetTickCount64();
    if (installedRequest.valid() || (installedAt && stamp - installedAt < 120000)) return;
    installedAt = stamp;
    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    installedRequest = std::async(std::launch::async, [notify, stop] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return std::vector<Shortcut>{};
        auto result = readInstalledApps(stop);
        CoUninitialize();
        if (notify && (!stop || !stop->load())) PostMessageW(notify, AsyncWake, 0, 0);
        return result;
    });
    SetTimer(hwnd, InstalledAppsTick, 80, nullptr);
}

void Dock::updateInstalledApps() {
    if (!installedRequest.valid() || installedRequest.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    auto apps = installedRequest.get();
    KillTimer(hwnd, InstalledAppsTick);
    std::vector<std::shared_ptr<Gdiplus::Bitmap>> images(apps.size());
    for (size_t i = 0; i < apps.size(); ++i) {
        const auto found = std::find_if(installedApps.begin(), installedApps.end(), [&](const Shortcut& old) {
            return apps[i].target == old.target && apps[i].identity == old.identity;
        });
        if (found != installedApps.end()) images[i] = std::move(installedImages[found - installedApps.begin()]);
    }
    if (apps.empty() && !installedApps.empty()) return;
    installedApps = std::move(apps);
    installedImages = std::move(images);
    installedImageRequested.clear();
    if (panel && (page == Apps || page == Search || page == Commands)) {
        sheet.key = sheet.hover = -1;
        paintSheet();
    }
}

bool isChromeApp(const Shortcut& app) {
    const auto& key = app.identity.empty() ? app.target : app.identity;
    return same(leaf(key), L"chrome.exe") || same(app.target, L"shell:AppsFolder\\Chrome");
}

void Dock::toggleDesktop() {
    if (panel) close();
    ComPtr<IShellDispatch4> shell;
    if (FAILED(CoCreateInstance(__uuidof(Shell), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))) ||
        FAILED(shell->ToggleDesktop())) {
        report(L"Windows masaüstü açılamadı.");
        return;
    }
    if (bar.hwnd) {
        SetWindowPos(bar.hwnd, model.topmost ? HWND_TOPMOST : HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void Dock::importTaskbarPins() {
    PWSTR folder{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &folder))) {
        report(L"Windows kısayolları okunamadı.");
        return;
    }
    const auto directory = fs::path(folder) / LR"(Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar)";
    CoTaskMemFree(folder);

    std::vector<fs::path> links;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; it != end && !error; it.increment(error)) {
        if (it->is_regular_file(error) && same(it->path().extension().wstring(), L".lnk")) links.push_back(it->path());
    }
    if (links.empty()) {
        report(L"İçe aktarılabilecek Windows kısayolu bulunamadı.");
        return;
    }
    std::sort(links.begin(), links.end());
    const auto originalSize = model.shortcuts.size();
    for (const auto& path : links) {
        if (model.shortcuts.size() >= MaxShortcuts) break;
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> file;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) continue;
        wchar_t target[32768]{};
        link->GetPath(target, ARRAYSIZE(target), nullptr, 0);
        Text identity = target;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(link.As(&store))) {
            const auto id = property(store.Get(), PKEY_AppUserModel_ID);
            if (!id.empty() && (identity.empty() || id.find(L'!') != Text::npos)) identity = id;
        }
        Shortcut app{path.stem().wstring(), path.wstring(), std::move(identity)};
        if (pinnedIndex(app) < 0) model.shortcuts.push_back(std::move(app));
    }
    const auto imported = model.shortcuts.size() - originalSize;
    if (!imported) {
        report(model.shortcuts.size() >= MaxShortcuts ? L"Kısayol sınırına ulaşıldı." : L"Eklenecek yeni Windows kısayolu yok.");
        return;
    }
    auto& tiles = model.layouts[model.profile];
    auto apps = std::find_if(tiles.begin(), tiles.end(), [](Tile item) { return item.id == Apps; });
    const bool wasEnabled = apps->enabled;
    apps->enabled = true;
    if (!model.save()) {
        model.shortcuts.resize(originalSize);
        apps->enabled = wasEnabled;
        report(L"Kısayollar kaydedilemedi.");
        return;
    }
    refreshAppImages();
    for (int p = 0; p < ProfileCount; ++p) ensureDockOrder(p);
    model.save();
    position();
    report(num(imported) + L" Windows kısayolu eklendi.");
}

int Dock::pinnedIndex(const Shortcut& app) const {
    if (app.target.empty()) return -1;
    const auto& key = app.identity.empty() ? app.target : app.identity;
    for (size_t i = 0; i < model.shortcuts.size(); ++i) {
        const auto& pinned = model.shortcuts[i];
        const auto& pinnedKey = pinned.identity.empty() ? pinned.target : pinned.identity;
        if (same(app.target, pinned.target) || same(key, pinnedKey) ||
            (isChromeApp(app) && isChromeApp(pinned))) return static_cast<int>(i);
        if ((same(key, L"Microsoft.WindowsNotepad_8wekyb3d8bbwe!App") && same(pinned.target, L"notepad.exe")) ||
            (same(key, L"Microsoft.WindowsTerminal_8wekyb3d8bbwe!App") && same(pinned.target, L"wt.exe")) ||
            (same(key, L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App") && same(pinned.target, L"calc.exe")))
            return static_cast<int>(i);
    }
    return -1;
}

size_t Dock::pinnedOnBar() const { return std::min(OnBarPins, model.shortcuts.size()); }

std::vector<size_t> Dock::runningOnBar() const {
    std::vector<size_t> result;
    bool chromeOnBar = false;
    for (size_t i = 0; i < extra.windows.size(); ++i) {
        if (result.size() == MaxRunningOnBar) break;
        const int pin = pinnedIndex(extra.windows[i].app);
        if (pin >= 0 && static_cast<size_t>(pin) < pinnedOnBar()) continue;
        if (isChromeApp(extra.windows[i].app)) {
            if (chromeOnBar) continue;
            chromeOnBar = true;
        }
        result.push_back(i);
    }
    return result;
}

void Dock::togglePin(const Shortcut& app) {
    setPinned(app, pinnedIndex(app) < 0, contextWindow);
}

bool Dock::setPinned(const Shortcut& app, bool pin, HWND sourceWindow) {
    if (app.target.empty()) { report(L"Windows bu uygulamanın başlatma hedefini paylaşmadı."); return false; }
    const int index = pinnedIndex(app);
    if (pin == (index >= 0)) return false;
    if (index < 0 && model.shortcuts.size() >= MaxShortcuts) {
        report(L"Dock’a en fazla " + num(MaxShortcuts) + L" kısayol sabitlenebilir.");
        return false;
    }
    const auto oldShortcuts = model.shortcuts;
    const auto oldDockOrder = model.dockOrder;
    const auto oldLiveOrder = liveOrder;
    const auto keyMatches = [&](const DockSlot& slot, const Shortcut& s) {
        return slot.isApp && (same(slot.appKey, appKey(s)) || same(slot.appKey, s.target));
    };

    for (int p = 0; p < ProfileCount; ++p) ensureDockOrder(p);
    auto& layout = model.layouts[model.profile];
    const auto tile = std::find_if(layout.begin(), layout.end(), [](Tile entry) { return entry.id == Apps; });
    const bool enabled = tile->enabled;

    if (index >= 0) {
        const Shortcut removed = model.shortcuts[index];
        model.shortcuts.erase(model.shortcuts.begin() + index);
        for (int p = 0; p < ProfileCount; ++p) {
            std::erase_if(model.dockOrder[p], [&](const DockSlot& s) { return keyMatches(s, removed); });
        }
        // Keep an open window in place if it was just unpinned.
        HWND openWin{};
        bool openChrome = false;
        for (const auto& w : extra.windows) {
            if (same(appKey(w.app), appKey(removed)) || same(w.app.target, removed.target) ||
                (isChromeApp(w.app) && isChromeApp(removed))) {
                openWin = w.handle;
                openChrome = isChromeApp(w.app);
                break;
            }
        }
        auto& live = liveOrder[model.profile];
        for (auto it = live.begin(); it != live.end(); ) {
            if (it->kind == OrderKind::Shortcut && (same(it->appKey, appKey(removed)) || same(it->appKey, removed.target))) {
                if (openWin) {
                    *it = {OrderKind::Window, -1, {}, openChrome ? nullptr : openWin, openChrome};
                    ++it;
                } else it = live.erase(it);
            } else ++it;
        }
    } else {
        Shortcut selected = app;
        const Text newKey = appKey(selected);
        model.shortcuts.insert(model.shortcuts.begin(), selected);
        tile->enabled = true;

        bool pinnedInPlace = false;
        auto& live = liveOrder[model.profile];
        for (auto& entry : live) {
            if (entry.kind != OrderKind::Window) continue;
            bool match = (entry.chromeGroup && isChromeApp(selected));
            if (!match && entry.window) {
                if (sourceWindow && entry.window == sourceWindow) match = true;
                else {
                    const auto wf = std::find_if(extra.windows.begin(), extra.windows.end(),
                        [&](const OpenWindow& w) { return w.handle == entry.window; });
                    if (wf != extra.windows.end() && (same(appKey(wf->app), newKey) || same(wf->app.target, selected.target)))
                        match = true;
                }
            }
            if (match) {
                entry = {OrderKind::Shortcut, -1, newKey, nullptr, false};
                pinnedInPlace = true;
                break;
            }
        }
        for (int p = 0; p < ProfileCount; ++p) {
            auto& slots = model.dockOrder[p];
            if (p == model.profile && pinnedInPlace) continue;
            auto firstApp = std::find_if(slots.begin(), slots.end(), [](const DockSlot& s) { return s.isApp; });
            slots.insert(firstApp != slots.end() ? firstApp : slots.begin(), {true, -1, newKey});
        }
        if (pinnedInPlace) {
            auto& slots = model.dockOrder[model.profile];
            auto firstApp = std::find_if(slots.begin(), slots.end(), [](const DockSlot& s) { return s.isApp; });
            slots.insert(firstApp != slots.end() ? firstApp : slots.begin(), {true, -1, newKey});
            persistLiveOrder();
        }
    }

    if (!model.save()) {
        tile->enabled = enabled;
        model.shortcuts = oldShortcuts;
        model.dockOrder = oldDockOrder;
        liveOrder = oldLiveOrder;
        report(L"Sabitlenen uygulamalar kaydedilemedi.");
        return false;
    }
    refreshAppImages();
    if (page == AppMenu) close();
    position();
    report(index >= 0 ? L"Dock’tan kaldırıldı." : L"Dock’a sabitlendi.");
    return true;
}

namespace {
struct DropMedium {
    STGMEDIUM value{};
    ~DropMedium() { if (value.tymed != TYMED_NULL) ReleaseStgMedium(&value); }
};

struct DroppedApp {
    Shortcut app;
    ComPtr<IPersistFile> virtualLink;
};

FORMATETC dropFormat(CLIPFORMAT format, DWORD medium = TYMED_HGLOBAL, LONG index = -1) {
    return {format, nullptr, DVASPECT_CONTENT, index, medium};
}

CLIPFORMAT shellDropFormat(const wchar_t* name) {
    return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name));
}

Text linkIdentity(IShellLinkW* link) {
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&store)))) {
        auto id = property(store.Get(), PKEY_AppUserModel_ID);
        if (id.find(L'!') != Text::npos) return id;
    }
    wchar_t target[32768]{};
    if (SUCCEEDED(link->GetPath(target, ARRAYSIZE(target), nullptr, 0)) && *target) return target;
    PIDLIST_ABSOLUTE pidl{};
    if (FAILED(link->GetIDList(&pidl)) || !pidl) return {};
    ComPtr<IShellItem> item;
    SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    CoTaskMemFree(pidl);
    return item ? shellName(item.Get(), SIGDN_DESKTOPABSOLUTEPARSING) : Text{};
}

DroppedApp droppedFile(const Text& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    const fs::path file(path);
    if (same(file.extension().wstring(), L".exe")) return {{installedLabel(path), path, path}, {}};
    if (!same(file.extension().wstring(), L".lnk")) return {};

    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> disk;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&disk)) || FAILED(disk->Load(path.c_str(), STGM_READ))) return {};
    auto identity = linkIdentity(link.Get());
    if (identity.empty()) identity = path;
    return {{file.stem().wstring(), path, std::move(identity)}, {}};
}

DroppedApp virtualShortcut(IDataObject* data) {
    auto descriptorFormat = dropFormat(shellDropFormat(CFSTR_FILEDESCRIPTORW));
    DropMedium descriptor;
    if (FAILED(data->GetData(&descriptorFormat, &descriptor.value)) || descriptor.value.tymed != TYMED_HGLOBAL ||
        GlobalSize(descriptor.value.hGlobal) < sizeof(FILEGROUPDESCRIPTORW)) return {};
    const auto* group = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(descriptor.value.hGlobal));
    if (!group) return {};
    const auto& file = group->fgd[0];
    const auto* end = wmemchr(file.cFileName, L'\0', ARRAYSIZE(file.cFileName));
    const Text name = group->cItems == 1 && end &&
        !((file.dwFlags & FD_ATTRIBUTES) && (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        ? Text(file.cFileName, end) : Text{};
    GlobalUnlock(descriptor.value.hGlobal);
    if (name.empty() || name.find_first_of(L"/\\") != Text::npos ||
        !same(fs::path(name).extension().wstring(), L".lnk")) return {};

    constexpr ULONG limit = 1024 * 1024;
    auto contentFormat = dropFormat(shellDropFormat(CFSTR_FILECONTENTS), TYMED_ISTREAM | TYMED_HGLOBAL, 0);
    DropMedium contents;
    if (FAILED(data->GetData(&contentFormat, &contents.value))) return {};
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return {};
    if (contents.value.tymed == TYMED_ISTREAM) {
        ULARGE_INTEGER max{}, read{};
        max.QuadPart = limit + 1;
        contents.value.pstm->Seek({}, STREAM_SEEK_SET, nullptr);
        if (FAILED(contents.value.pstm->CopyTo(stream.Get(), max, &read, nullptr)) ||
            read.QuadPart == 0 || read.QuadPart > limit) return {};
    } else if (contents.value.tymed == TYMED_HGLOBAL) {
        const auto size = GlobalSize(contents.value.hGlobal);
        if (!size || size > limit) return {};
        const void* bytes = GlobalLock(contents.value.hGlobal);
        if (!bytes) return {};
        const HRESULT written = stream->Write(bytes, static_cast<ULONG>(size), nullptr);
        GlobalUnlock(contents.value.hGlobal);
        if (FAILED(written)) return {};
    } else return {};

    ComPtr<IShellLinkW> link;
    ComPtr<IPersistStream> serialized;
    ComPtr<IPersistFile> disk;
    if (FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr)) ||
        FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&serialized)) || FAILED(serialized->Load(stream.Get())) || FAILED(link.As(&disk))) return {};
    auto identity = linkIdentity(link.Get());
    if (identity.empty()) return {};
    return {{fs::path(name).stem().wstring(), name, std::move(identity)}, std::move(disk)};
}

DroppedApp droppedApp(IDataObject* data) {
    ComPtr<IShellItemArray> items;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(data, IID_PPV_ARGS(&items)))) {
        DWORD count{};
        ComPtr<IShellItem> item;
        if (SUCCEEDED(items->GetCount(&count)) && count == 1 && SUCCEEDED(items->GetItemAt(0, &item))) {
            if (auto path = shellName(item.Get(), SIGDN_FILESYSPATH); !path.empty()) {
                auto result = droppedFile(path);
                if (!result.app.target.empty()) return result;
            } else {
                PIDLIST_ABSOLUTE folder{}, pidl{};
                const bool inAppsFolder = SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &folder)) &&
                    SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl)) && ILIsParent(folder, pidl, TRUE);
                CoTaskMemFree(folder);
                CoTaskMemFree(pidl);
                if (inAppsFolder) {
                    ComPtr<IShellItem2> details;
                    Text id;
                    PWSTR value{};
                    if (SUCCEEDED(item.As(&details)) && SUCCEEDED(details->GetString(PKEY_AppUserModel_ID, &value))) {
                        id = value;
                        CoTaskMemFree(value);
                    }
                    if (id.empty()) id = shellName(item.Get(), SIGDN_PARENTRELATIVEPARSING);
                    const Text label = shellName(item.Get(), SIGDN_NORMALDISPLAY);
                    if (!id.empty() && !label.empty()) return {{label, AppsPrefix + id, id}, {}};
                }
            }
        }
    }

    auto filesFormat = dropFormat(CF_HDROP);
    DropMedium files;
    if (SUCCEEDED(data->GetData(&filesFormat, &files.value)) && files.value.tymed == TYMED_HGLOBAL) {
        const auto list = reinterpret_cast<HDROP>(files.value.hGlobal);
        if (DragQueryFileW(list, 0xFFFFFFFF, nullptr, 0) == 1) {
            const UINT length = DragQueryFileW(list, 0, nullptr, 0);
            if (length > 0 && length < 32768) {
                Text path(length + 1, L'\0');
                if (DragQueryFileW(list, 0, path.data(), static_cast<UINT>(path.size())) == length) {
                    path.resize(length);
                    auto result = droppedFile(path);
                    if (!result.app.target.empty()) return result;
                }
            }
        }
    }
    return virtualShortcut(data);
}

bool pinDroppedApp(Dock& dock, DroppedApp dropped) {
    auto& app = dropped.app;
    if (app.target.empty() || app.target.find_first_of(L"\r\n") != Text::npos ||
        app.identity.find_first_of(L"\r\n") != Text::npos) {
        dock.report(L"Bir uygulama veya uygulama kısayolu bırak.");
        return false;
    }
    for (auto& ch : app.label) if (ch == L'\r' || ch == L'\n') ch = L' ';
    if (dock.pinnedIndex(app) >= 0) {
        dock.report(L"Bu uygulama zaten sabitli.");
        return true;
    }
    if (dock.model.shortcuts.size() >= MaxShortcuts) return dock.setPinned(app, true);
    if (!same(fs::path(app.target).extension().wstring(), L".lnk")) return dock.setPinned(app, true);

    // Keep the exact launch settings even when the source shortcut is later moved.
    GUID id{};
    wchar_t filename[40]{};
    std::error_code error;
    const auto folder = fs::path(dock.model.folder) / L"Dropped shortcuts";
    if (!dock.model.folder.empty()) fs::create_directories(folder, error);
    if (dock.model.folder.empty() || error || FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, filename, ARRAYSIZE(filename))) {
        dock.report(L"Uygulama kısayolu kaydedilemedi.");
        return false;
    }
    const auto path = folder / (Text(filename) + L".lnk");
    const bool copied = dropped.virtualLink ? SUCCEEDED(dropped.virtualLink->Save(path.c_str(), FALSE))
        : CopyFileW(app.target.c_str(), path.c_str(), TRUE) != FALSE;
    if (!copied) {
        dock.report(L"Uygulama kısayolu kaydedilemedi.");
        return false;
    }
    app.target = path.wstring();
    if (dock.setPinned(app, true)) return true;
    DeleteFileW(path.c_str());
    return false;
}

class DockDropTarget final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropTarget> {
public:
    explicit DockDropTarget(Dock& dock, HWND window) : dock(dock), window(window) {}

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) noexcept override {
        auto files = dropFormat(CF_HDROP);
        auto items = dropFormat(shellDropFormat(CFSTR_SHELLIDLIST));
        auto descriptor = dropFormat(shellDropFormat(CFSTR_FILEDESCRIPTORW));
        supported = data && (data->QueryGetData(&files) == S_OK || data->QueryGetData(&items) == S_OK ||
            data->QueryGetData(&descriptor) == S_OK);
        return offer(effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) noexcept override { return offer(effect); }
    HRESULT STDMETHODCALLTYPE DragLeave() noexcept override {
        supported = false;
        highlight(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) noexcept override {
        if (!effect) return E_POINTER;
        const DWORD accepted = pickEffect(*effect);
        *effect = DROPEFFECT_NONE;
        DragLeave();
        if (accepted && data) {
            try {
                if (pinDroppedApp(dock, droppedApp(data))) *effect = accepted;
            } catch (const std::exception&) {
                dock.report(L"Uygulama sabitlenemedi.");
            }
        }
        return S_OK;
    }

private:
    Dock& dock;
    HWND window;
    bool supported = false;

    DWORD pickEffect(DWORD offered) const {
        if (!supported || dock.modal || dock.dockDragging || dock.dockOrderDragging) return DROPEFFECT_NONE;
        if (offered & DROPEFFECT_COPY) return DROPEFFECT_COPY;
        return offered & DROPEFFECT_LINK;
    }
    void highlight(bool active) noexcept {
        if (dock.appDropHover == active) return;
        dock.appDropHover = active;
        try { dock.paintBar(); } catch (const std::exception&) {}
    }
    HRESULT offer(DWORD* effect) noexcept {
        if (!effect) return E_POINTER;
        *effect = pickEffect(*effect);
        try {
            if (*effect != DROPEFFECT_NONE && !dock.selectDockWindow(window))
                *effect = DROPEFFECT_NONE;
            highlight(*effect != DROPEFFECT_NONE);
        } catch (const std::exception&) {
            *effect = DROPEFFECT_NONE;
        }
        return S_OK;
    }
};
}

void Dock::registerAppDrop(HWND window) {
    auto target = Microsoft::WRL::Make<DockDropTarget>(*this, window);
    if (target) RegisterDragDrop(window, target.Get());
}

namespace {
struct CachedJumpList {
    JumpListData data;
    ULONGLONG fetchedAt = 0;
};
std::mutex gJumpCacheMutex;
std::unordered_map<Text, CachedJumpList> gJumpCache;
std::mutex gJumpPendingMutex;
std::optional<Dock::JumpListResult> gJumpPending;

struct AppMenuEntry {
    bool isHeader = false;
    bool isTask = false;
    Text title;
    const JumpItem* item = nullptr;
};

constexpr float AppMenuHeaderH = 54.f;
constexpr float AppMenuCatH = 23.f;
constexpr float AppMenuItemH = 31.f;
constexpr float AppMenuMaxJumpH = 296.f;
constexpr float AppMenuDividerH = 11.f;
constexpr float AppMenuActionH = 34.f;
constexpr float AppMenuActionGap = 5.f;
constexpr float AppMenuBottomPad = 14.f;

std::vector<AppMenuEntry> flattenJumpEntries(const JumpListData& list) {
    std::vector<AppMenuEntry> out;
    for (const auto& cat : list.categories) {
        if (cat.items.empty()) continue;
        out.push_back({true, cat.isTasks, cat.title, nullptr});
        for (const auto& item : cat.items)
            out.push_back({false, cat.isTasks, {}, &item});
    }
    return out;
}

float jumpEntriesHeight(const std::vector<AppMenuEntry>& entries) {
    float h = 0.f;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].isHeader && i > 0) h += 4.f;
        h += entries[i].isHeader ? AppMenuCatH : AppMenuItemH;
    }
    return h;
}

Text jumpCacheKey(const Shortcut& app) {
    const Text key = appKey(app);
    return lowerText(key.empty() ? app.label : key);
}
} // namespace

void Dock::sizeAppMenu() {
    const auto entries = flattenJumpEntries(contextJumpList);
    const bool hasJump = !entries.empty();
    sheet.w = hasJump ? 298.f : 260.f;

    float h = AppMenuHeaderH + 4.f;
    if (hasJump) {
        h += std::min(AppMenuMaxJumpH, jumpEntriesHeight(entries));
        h += AppMenuDividerH;
    }
    const int actionCount = contextWindows.empty() ? 2 : 3;
    h += actionCount * AppMenuActionH + (actionCount - 1) * AppMenuActionGap + AppMenuBottomPad;
    sheet.h = h;
}

void Dock::showAppMenu(Shortcut app, float x, HWND window, std::shared_ptr<Gdiplus::Bitmap> icon) {
    if (sheetClosing) closeSheet();
    contextApp = std::move(app);
    contextWindow = (window && IsWindow(window)) ? window : nullptr;
    contextWindows.clear();
    if (contextWindow) contextWindows.push_back(contextWindow);

    const Text ctxKey = appKey(contextApp);
    const int ctxPin = pinnedIndex(contextApp);
    const Text ctxLeaf = lowerText(leaf(contextApp.target));
    const bool ctxExeLeaf = ctxLeaf.ends_with(L".exe") && ctxLeaf != L"applicationframehost.exe";
    for (const auto& w : extra.windows) {
        if (!w.handle || !IsWindow(w.handle)) continue;
        if (std::find(contextWindows.begin(), contextWindows.end(), w.handle) != contextWindows.end()) continue;
        bool match = false;
        if (isChromeApp(contextApp) && isChromeApp(w.app)) {
            match = true;
        } else if (ctxPin >= 0 && pinnedIndex(w.app) == ctxPin) {
            match = true;
        } else if (!ctxKey.empty() && (same(appKey(w.app), ctxKey) || same(w.app.target, contextApp.target))) {
            match = true;
        } else if (ctxExeLeaf && !w.app.target.empty() && same(leaf(w.app.target), ctxLeaf)) {
            match = true;
        }
        if (match) contextWindows.push_back(w.handle);
    }
    if (!contextWindow && !contextWindows.empty()) contextWindow = contextWindows.front();

    contextIcon = icon ? std::move(icon) : appImage(contextApp.target);
    contextJumpList = {};
    contextJumpLoading = false;

    const Text cacheKey = jumpCacheKey(contextApp);
    const ULONGLONG nowTick = GetTickCount64();
    bool needAsyncRefresh = true;
    if (!cacheKey.empty()) {
        std::lock_guard lock(gJumpCacheMutex);
        if (const auto it = gJumpCache.find(cacheKey); it != gJumpCache.end()) {
            const ULONGLONG age = nowTick - it->second.fetchedAt;
            if (age < 30000ULL) contextJumpList = it->second.data;
            if (age < 4000ULL) needAsyncRefresh = false;
        }
    }

    const uint64_t gen = ++jumpListGen;
    open(AppMenu, x);

    if (needAsyncRefresh) {
        contextJumpLoading = true;
        const Shortcut appCopy = contextApp;
        const std::vector<HWND> windowsCopy = contextWindows;
        const HWND notify = hwnd;
        const auto stop = extra.proStop;
        try {
            std::thread([gen, cacheKey, appCopy, windowsCopy, notify, stop] {
                if (stop && stop->load()) return;
                JumpListData data;
                const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE) {
                    data = readAppJumpList(appCopy, windowsCopy);
                    if (SUCCEEDED(comHr)) CoUninitialize();
                }
                if (stop && stop->load()) return;
                if (!cacheKey.empty()) {
                    std::lock_guard lock(gJumpCacheMutex);
                    if (gJumpCache.size() > 64) gJumpCache.clear();
                    gJumpCache[cacheKey] = {data, GetTickCount64()};
                }
                {
                    std::lock_guard lock(gJumpPendingMutex);
                    if (!gJumpPending || gen >= gJumpPending->generation)
                        gJumpPending = Dock::JumpListResult{gen, cacheKey, std::move(data)};
                }
                if (notify && (!stop || !stop->load())) PostMessageW(notify, AsyncWake, 0, 0);
            }).detach();
        } catch (const std::exception&) {
            contextJumpLoading = false;
        }
    }
}

void Dock::pollJumpList() {
    std::optional<JumpListResult> ready;
    {
        std::lock_guard lock(gJumpPendingMutex);
        if (!gJumpPending) return;
        ready = std::move(gJumpPending);
        gJumpPending.reset();
    }
    if (ready->generation != jumpListGen) return;
    contextJumpLoading = false;
    if (!panel || page != AppMenu || sheetClosing) return;

    const auto sameList = [](const JumpListData& a, const JumpListData& b) {
        if (a.categories.size() != b.categories.size()) return false;
        for (size_t i = 0; i < a.categories.size(); ++i) {
            if (a.categories[i].title != b.categories[i].title ||
                a.categories[i].items.size() != b.categories[i].items.size()) return false;
            for (size_t j = 0; j < a.categories[i].items.size(); ++j) {
                if (a.categories[i].items[j].label != b.categories[i].items[j].label ||
                    a.categories[i].items[j].args != b.categories[i].items[j].args ||
                    a.categories[i].items[j].targetPath != b.categories[i].items[j].targetPath)
                    return false;
            }
        }
        return true;
    };

    const bool changedData = !sameList(contextJumpList, ready->data);
    contextJumpList = std::move(ready->data);
    if (changedData) {
        listOffset = 0;
        sheet.hover = sheet.key = -1;
        positionSheet();
    }
}

void Dock::launchJumpItem(const JumpItem& item) {
    AllowSetForegroundWindow(ASFW_ANY);
    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    const Text aumid = item.aumid;
    const Text targetPath = item.targetPath;
    const Text args = item.args;
    const Text workingDir = item.workingDir;
    const std::vector<BYTE> pidlBytes = item.pidlBytes;

    std::thread([aumid, targetPath, args, workingDir, pidlBytes, notify, stop] {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;

        if (!aumid.empty()) {
            ComPtr<IApplicationActivationManager> manager;
            if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                    IID_PPV_ARGS(&manager))) && manager) {
                DWORD pid = 0;
                if (SUCCEEDED(manager->ActivateApplication(aumid.c_str(),
                        args.empty() ? nullptr : args.c_str(), AO_NONE, &pid))) {
                    if (pid) AllowSetForegroundWindow(pid);
                    ok = true;
                }
            }
        }
        if (!ok && !targetPath.empty()) {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            info.lpFile = targetPath.c_str();
            info.lpParameters = args.empty() ? nullptr : args.c_str();
            info.lpDirectory = workingDir.empty() ? nullptr : workingDir.c_str();
            info.nShow = SW_SHOWNORMAL;
            ok = ShellExecuteExW(&info) != FALSE;
        }
        if (!ok && !pidlBytes.empty()) {
            SHELLEXECUTEINFOW info{sizeof(info)};
            info.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            info.lpIDList = const_cast<BYTE*>(pidlBytes.data());
            info.nShow = SW_SHOWNORMAL;
            ok = ShellExecuteExW(&info) != FALSE;
        }

        if (SUCCEEDED(comHr)) CoUninitialize();
        if (!ok && notify && (!stop || !stop->load())) PostMessageW(notify, LaunchFailedMsg, 0, 0);
    }).detach();
}

void Dock::closeAppWindows(const std::vector<HWND>& windows) {
    for (HWND w : windows) {
        if (w && IsWindow(w)) PostMessageW(w, WM_CLOSE, 0, 0);
    }
    scheduleWindowSync();
}

void Dock::showWindowMenu(HWND window, float x) {
    auto found = std::find_if(extra.windows.begin(), extra.windows.end(),
        [window](const OpenWindow& item) { return item.handle == window; });
    Shortcut app = IsWindow(window) ? windowApp(window) : Shortcut{};
    if (found != extra.windows.end()) {
        if (app.target.empty() && !found->app.target.empty()) app = found->app;
        if (app.label.empty()) app.label = found->title;
    }
    showAppMenu(std::move(app), x, IsWindow(window) ? window : nullptr,
        found != extra.windows.end() ? found->icon : nullptr);
}

void Dock::launchShortcut(const Shortcut& app) {
    if (app.target.empty()) return;
    triggerAppBounce(appKey(app));
    AllowSetForegroundWindow(ASFW_ANY);

    const Shortcut launched = app;
    auto& recent = model.recentApps;
    std::erase_if(recent, [&](const Shortcut& entry) { return same(appKey(entry), appKey(launched)); });
    recent.insert(recent.begin(), launched);
    if (recent.size() > 12) recent.resize(12);
    model.save();

    const HWND notify = hwnd;
    const auto stop = extra.proStop;
    const Text target = app.target;
    const Text identity = app.identity;
    std::thread([target, identity, notify, stop] {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool ok = false;

        if (target.starts_with(AppsPrefix)) {
            const Text aumid = target.substr(wcslen(AppsPrefix));
            ComPtr<IApplicationActivationManager> manager;
            if (!aumid.empty() &&
                SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER,
                    IID_PPV_ARGS(&manager))) && manager) {
                DWORD pid = 0;
                if (SUCCEEDED(manager->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid))) {
                    if (pid) AllowSetForegroundWindow(pid);
                    ok = true;
                }
            }
            if (!ok && !identity.empty() && identity.find(L'\\') != Text::npos) {
                const DWORD attr = GetFileAttributesW(identity.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    SHELLEXECUTEINFOW info{sizeof(info)};
                    info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
                    info.lpVerb = L"open";
                    info.lpFile = identity.c_str();
                    info.nShow = SW_SHOWNORMAL;
                    ok = ShellExecuteExW(&info) != FALSE;
                }
            }
        }

        if (!ok) {
            SHELLEXECUTEINFOW launchInfo{sizeof(launchInfo)};
            launchInfo.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            launchInfo.lpVerb = L"open";
            launchInfo.lpFile = target.c_str();
            launchInfo.nShow = SW_SHOWNORMAL;
            ok = ShellExecuteExW(&launchInfo) != FALSE;
        }
        if (!ok && target.starts_with(AppsPrefix)) {
            SHELLEXECUTEINFOW fallback{sizeof(fallback)};
            fallback.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            fallback.lpFile = L"explorer.exe";
            fallback.lpParameters = target.c_str();
            fallback.nShow = SW_SHOWNORMAL;
            ok = ShellExecuteExW(&fallback) != FALSE;
        }

        if (SUCCEEDED(comHr)) CoUninitialize();
        if (!ok && notify && (!stop || !stop->load())) PostMessageW(notify, LaunchFailedMsg, 0, 0);
    }).detach();
}

void Dock::loadRecentFiles() {
    recentFiles.clear();
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DEFAULT, nullptr, &path))) return;
    const Text folder(path);
    CoTaskMemFree(path);
    const Text pattern = folder + L"\\*.lnk";
    WIN32_FIND_DATAW fd{};
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return;
    std::vector<std::pair<ULONGLONG, Text>> entries;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const ULONGLONG stamp = (static_cast<ULONGLONG>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
            fd.ftLastWriteTime.dwLowDateTime;
        entries.emplace_back(stamp, fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    const size_t keep = std::min<size_t>(12, entries.size());
    std::partial_sort(entries.begin(), entries.begin() + keep, entries.end(),
        [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < keep; ++i) {
        const Text& file = entries[i].second;
        const Text label = file.size() > 4 ? file.substr(0, file.size() - 4) : file;
        recentFiles.push_back({label, folder + L"\\" + file, {}});
    }
}

void Dock::refreshRecycleBin() {
    SHQUERYRBINFO status{sizeof(status)};
    recycleItems = SUCCEEDED(SHQueryRecycleBinW(nullptr, &status)) ? status.i64NumItems : -1;
}

void Dock::runTool(const Text& input) {
    const size_t begin = input.find_first_not_of(L" \t");
    if (begin == Text::npos) return;
    Text query = input.substr(begin, input.find_last_not_of(L" \t") - begin + 1);
    if (page == Commands) {
        for (auto command : CommandsList) if (same(command.shortcut, query) || same(command.title, query)) {
            close();
            launchShortcut({command.title, command.target, {}});
            return;
        }
    }
    for (const auto* apps : {&model.shortcuts, &installedApps})
        for (const auto& app : *apps)
            if (same(app.label, query) || (page == Commands && same(leaf(app.target), query))) {
                const Shortcut chosen = app;
                close();
                launchShortcut(chosen);
                return;
            }
    if (page == Commands) {
        report(installedRequest.valid() ? L"Windows uygulamaları yükleniyor…" : L"Komut bulunamadı.");
        return;
    }
    if (query.size() >= 2 && query.front() == L'"' && query.back() == L'"') query = query.substr(1, query.size() - 2);
    wchar_t expanded[32768]{};
    const DWORD length = ExpandEnvironmentStringsW(query.c_str(), expanded, ARRAYSIZE(expanded));
    if (length && length <= ARRAYSIZE(expanded) && GetFileAttributesW(expanded) != INVALID_FILE_ATTRIBUTES) {
        close();
        launchShortcut({leaf(expanded), expanded, {}});
    } else if (query.starts_with(L"https://") || query.starts_with(L"http://")) {
        close();
        launch(query);
    } else if (query.find(L'\\') != Text::npos || (query.size() > 1 && query[1] == L':') || query.starts_with(L"%")) {
        report(L"Dosya veya klasör bulunamadı.");
    } else if (installedRequest.valid()) {
        report(L"Windows uygulamaları yükleniyor…");
    } else {
        close();
        launch(L"https://www.google.com/search?q=" + encodeQuery(query));
    }
}

void Dock::startCapture(int mode, HWND target) {
    if (captureMode >= 0) return;
    close();
    captureMode = mode;
    captureWindow = target;
    if (target) activateWindow(target);
    if (!SetTimer(hwnd, 11, 300, nullptr)) { captureMode = -1; report(L"Ekran yakalama başlatılamadı."); }
}

void Dock::finishCapture() {
    KillTimer(hwnd, 11);
    const int mode = std::exchange(captureMode, -1);
    if (mode == 1) {
        INPUT keys[6]{};
        const WORD seq[] = {VK_LWIN, VK_SHIFT, 'S'};
        for (int i = 0; i < 3; ++i) {
            keys[i].type = INPUT_KEYBOARD;
            keys[i].ki.wVk = seq[i];
            keys[5 - i] = keys[i];
            keys[5 - i].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        if (SendInput(ARRAYSIZE(keys), keys, sizeof(INPUT)) != ARRAYSIZE(keys)) report(L"Windows ekran alıntısı açılamadı.");
        return;
    }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    RECT screen{x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    RECT region = screen;
    if (mode == 2) {
        RECT frame{};
        if (!captureWindow || GetForegroundWindow() != captureWindow ||
            (FAILED(DwmGetWindowAttribute(captureWindow, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) &&
                !GetWindowRect(captureWindow, &frame)) || !IntersectRect(&region, &screen, &frame)) {
            report(L"Yakalanacak pencere öne getirilemedi.");
            return;
        }
    }
    const int w = region.right - region.left, h = region.bottom - region.top;
    HDC desktop = GetDC(nullptr);
    HDC buffer = desktop ? CreateCompatibleDC(desktop) : nullptr;
    HBITMAP capture = buffer ? CreateCompatibleBitmap(desktop, w, h) : nullptr;
    HGDIOBJ old = capture ? SelectObject(buffer, capture) : nullptr;
    bool saved = old && BitBlt(buffer, 0, 0, w, h, desktop, region.left, region.top, SRCCOPY | CAPTUREBLT);
    if (old) SelectObject(buffer, old);
    if (buffer) DeleteDC(buffer);
    if (desktop) ReleaseDC(nullptr, desktop);
    if (saved && OpenClipboard(hwnd)) {
        saved = EmptyClipboard() && SetClipboardData(CF_BITMAP, capture) != nullptr;
        CloseClipboard();
    } else saved = false;
    if (!saved && capture) DeleteObject(capture);
    report(saved ? L"Ekran görüntüsü panoya kopyalandı." : L"Ekran görüntüsü kopyalanamadı.");
}

void Dock::appPanel(Palette p) {
    auto& d = sheet.d;
    auto row = [&](int i, const Shortcut& app, const std::shared_ptr<Gdiplus::Bitmap>& image, bool contextMenu) {
        const float y = 112.f + 42.f * i;
        const auto b = box(25, y, sheet.w - 50, 38);
        hit(sheet, b, [this, app] { close(); launchShortcut(app); },
            contextMenu ? std::function<void()>([this, app] { showAppMenu(app); }) : std::function<void()>{});
        if (p.glass) {
            p.card(d, b, 11);
            const float hot = sheet.hoverAmount();
            if (hot > 0) d.fill(b, 11, tint(p.light ? 0x10141A : 0xFFFFFF, .06f * hot));
        } else {
            d.fill(b, 10, tint(p.ink, .065f + .13f * sheet.hoverAmount()));
        }
        if (image) d.image(image, box(34, y + 8, 22, 22), p.light);
        else d.icon(contextMenu ? Layers : Document, box(35, y + 10, 18, 18), tint(p.accent));
        d.text(app.label, box(65, y, width(b) - 50, 38), Body, tint(p.ink));
    };
    auto paging = [&](int count, int visible) {
        const int last = std::max(0, count - visible);
        if (!last) return;
        iconButton(sheet, p, box(sheet.w - 103, sheet.h - 47, 30, 27), ChevronUp,
            [this] { listOffset = std::max(0, listOffset - 1); paintSheet(); });
        iconButton(sheet, p, box(sheet.w - 67, sheet.h - 47, 30, 27), ChevronDown,
            [this, last] { listOffset = std::min(last, listOffset + 1); paintSheet(); });
    };
    if (page == AppMenu) {
        if (contextIcon) d.image(contextIcon, box(22, 15, 28, 28), p.light);
        else d.icon(Layers, box(25, 18, 22, 22), tint(p.accent));
        d.text(contextApp.label.empty() ? L"Uygulama" : contextApp.label,
            box(58, 13, sheet.w - 78, 32), Strong, tint(p.ink));
        d.line(18, 51, sheet.w - 18, 51, tint(p.line, .25f));

        const auto entries = flattenJumpEntries(contextJumpList);
        const bool hasJump = !entries.empty();
        float actionsTop = AppMenuHeaderH + 4.f;

        if (hasJump) {
            const float jumpTop = AppMenuHeaderH + 4.f;
            const float totalJumpH = jumpEntriesHeight(entries);
            const float jumpViewH = std::min(AppMenuMaxJumpH, totalJumpH);
            const bool scrollable = totalJumpH > AppMenuMaxJumpH + 0.5f;

            int maxOffset = 0;
            if (scrollable) {
                float tailH = 0.f;
                int start = static_cast<int>(entries.size());
                while (start > 0) {
                    const float nextH = entries[start - 1].isHeader ? AppMenuCatH : AppMenuItemH;
                    const float extraGap = (start < static_cast<int>(entries.size()) && entries[start].isHeader) ? 4.f : 0.f;
                    if (tailH + nextH + extraGap > jumpViewH + 0.5f) break;
                    tailH += nextH + extraGap;
                    --start;
                }
                maxOffset = std::max(0, start);
            }
            listOffset = std::clamp(listOffset, 0, maxOffset);

            float y = jumpTop;
            const float rightPad = scrollable ? 24.f : 17.f;
            for (int idx = listOffset; idx < static_cast<int>(entries.size()); ++idx) {
                const auto& entry = entries[idx];
                const float preGap = (entry.isHeader && idx > listOffset) ? 4.f : 0.f;
                const float rowH = entry.isHeader ? AppMenuCatH : AppMenuItemH;
                // Never draw an orphan category header unless at least one item below it also fits.
                const float minNeed = entry.isHeader ? (AppMenuCatH + AppMenuItemH) : rowH;
                if (y + preGap + minNeed > jumpTop + jumpViewH + 0.5f) break;
                y += preGap;

                if (entry.isHeader) {
                    d.text(entry.title, box(24, y + 3.f, sheet.w - 24.f - rightPad, AppMenuCatH - 4.f),
                        Small, tint(p.muted, .90f));
                } else if (entry.item) {
                    const Box b = box(17.f, y, sheet.w - 17.f - rightPad, AppMenuItemH - 2.f);
                    JumpItem itemCopy = *entry.item;
                    hit(sheet, b, [this, itemCopy] {
                        close();
                        launchJumpItem(itemCopy);
                    });
                    const float hot = sheet.hoverAmount();
                    if (hot > 0.f) {
                        if (p.glass) {
                            d.fill(b, 9.f, tint(p.light ? 0x10141A : 0xFFFFFF, .10f * hot));
                            d.glassRim(b, 9.f, .24f * hot);
                        } else {
                            d.fill(b, 9.f, tint(p.accent, .14f * hot));
                        }
                    }
                    if (entry.item->icon) {
                        d.image(entry.item->icon, box(b.left + 8.f, b.top + 6.f, 17.f, 17.f), p.light);
                    } else {
                        const Glyph g = entry.item->isFolder ? FolderIcon : (entry.isTask ? Play : Document);
                        d.icon(g, box(b.left + 9.f, b.top + 7.f, 15.f, 15.f), tint(p.accent, .88f));
                    }
                    d.text(entry.item->label, box(b.left + 33.f, b.top, width(b) - 41.f, height(b)),
                        Body, tint(p.ink));
                }
                y += rowH;
            }

            if (scrollable && maxOffset > 0) {
                const float trackX = sheet.w - 15.f;
                const float trackY = jumpTop + 4.f;
                const float trackH = std::max(24.f, jumpViewH - 8.f);
                d.fill(box(trackX, trackY, 3.5f, trackH), 1.75f, tint(p.ink, .08f));
                const float thumbH = std::clamp(trackH * (jumpViewH / std::max(1.f, totalJumpH)), 22.f, trackH);
                const float thumbY = trackY + (trackH - thumbH) * (static_cast<float>(listOffset) / maxOffset);
                d.fill(box(trackX, thumbY, 3.5f, thumbH), 1.75f, tint(p.ink, .30f));
            }

            actionsTop = jumpTop + jumpViewH + AppMenuDividerH;
            d.line(18, actionsTop - 6.f, sheet.w - 18, actionsTop - 6.f, tint(p.line, .25f));
        }

        const auto app = contextApp;
        const bool pinned = pinnedIndex(app) >= 0;
        auto menuRow = [&](float y, Glyph glyph, const Text& label, std::function<void()> action, bool destructive = false) {
            const auto b = box(17.f, y, sheet.w - 34.f, AppMenuActionH);
            hit(sheet, b, std::move(action));
            const float hot = sheet.hoverAmount();
            if (p.glass) {
                p.card(d, b, 11.f);
                if (hot > 0.f)
                    d.fill(b, 11.f, destructive ? tint(Design::Red, .18f * hot)
                        : tint(p.light ? 0x10141A : 0xFFFFFF, .07f * hot));
            } else {
                d.fill(b, 10.f, tint(destructive ? Design::Red : p.accent, .08f + .15f * hot));
            }
            d.icon(glyph, box(30.f, y + 8.f, 18.f, 18.f), tint(destructive ? Design::Red : p.accent));
            d.text(label, box(58.f, y, sheet.w - 79.f, AppMenuActionH), Body,
                tint(destructive && hot > .25f ? Design::Red : p.ink));
        };

        float rowY = actionsTop;
        if (!app.target.empty())
            menuRow(rowY, pinned ? Minus : PinIcon, pinned ? L"Dock’tan kaldır" : L"Dock’a sabitle", [this, app] { togglePin(app); });
        else
            menuRow(rowY, SearchIcon, L"Kurulu uygulamalarda bul", [this] { open(Apps, panelDockX); });
        rowY += AppMenuActionH + AppMenuActionGap;

        menuRow(rowY, Play, contextWindow ? L"Pencereye geç" : L"Uygulamayı aç", [this, app, target = contextWindow] {
            close();
            if (target && IsWindow(target)) activateWindow(target);
            else launchShortcut(app);
        });

        if (!contextWindows.empty()) {
            rowY += AppMenuActionH + AppMenuActionGap;
            const Text closeLabel = contextWindows.size() > 1
                ? (L"Tüm pencereleri kapat (" + num(contextWindows.size()) + L")")
                : L"Pencereyi kapat";
            menuRow(rowY, Close, closeLabel, [this, wins = contextWindows] {
                close();
                closeAppWindows(wins);
            }, true);
        }
        return;
    }
    if (page == RecycleBin) {
        heading(p, Widgets[page].title, L"Windows Geri Dönüşüm Kutusu");
        d.icon(Trash, box(42, 94, 52, 52), tint(p.accent));
        d.text(recycleItems < 0 ? L"Öğe sayısı alınamadı" : num(recycleItems) + L" öğe", box(112, 94, 275, 50), Number, tint(p.ink));
        button(sheet, p, box(29, 175, 175, 40), L"Kutuyu aç", [this] { close(); launch(L"shell:RecycleBinFolder"); }, true, Body);
        if (recycleItems != 0) button(sheet, p, box(215, 175, 190, 40), L"Kutuyu boşalt…", [this] {
            modal = true;
            SHEmptyRecycleBinW(panel, nullptr, 0);
            modal = false;
            refreshRecycleBin();
            paintBar();
            paintSheet();
        }, false, Body);
        return;
    }
    if (page == Capture) {
        heading(p, Widgets[page].title, L"Ekran görüntüsünü panoya kopyala");
        button(sheet, p, box(27, 92, 180, 36), L"Tüm ekranlar", [this] { startCapture(0); }, true, Body);
        button(sheet, p, box(218, 92, 180, 36), L"Bölge seç", [this] { startCapture(1); }, false, Body);
        d.text(L"Pencere seç", box(29, 137, 360, 24), Strong, tint(p.ink));
        constexpr int visible = 4;
        const int count = static_cast<int>(extra.windows.size());
        listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
        if (!count) d.text(L"Açık pencere yok.", box(29, 170, 360, 42), Body, tint(p.muted));
        for (int n = 0; n < visible && n + listOffset < count; ++n) {
            const auto& window = extra.windows[n + listOffset];
            const float y = 168.f + 36.f * n;
            button(sheet, p, box(27, y, 372, 32), window.title,
                [this, target = window.handle] { startCapture(2, target); }, false, Body);
        }
        paging(count, visible);
        return;
    }
    if (page == Recent) {
        heading(p, Widgets[page].title, L"Vetra geçmişi ve Windows’un son dosyaları");
        const auto count = static_cast<int>(model.recentApps.size() + recentFiles.size());
        constexpr int visible = 6;
        listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
        if (!count) d.text(L"Yakın zamanda açılan bir öğe yok.", box(29, 112, 360, 42), Body, tint(p.muted));
        for (int n = 0; n < visible && n + listOffset < count; ++n) {
            const auto index = static_cast<size_t>(n + listOffset);
            const bool app = index < model.recentApps.size();
            const auto& entry = app ? model.recentApps[index] : recentFiles[index - model.recentApps.size()];
            row(n, entry, nullptr, false);
        }
        paging(count, visible);
        return;
    }

    const bool command = page == Commands, picker = page == Apps;
    d.text(Widgets[page].title, box(27, 17, sheet.w - 88, 34), Title, tint(p.ink));
    iconButton(sheet, p, box(sheet.w - 58, 20, 32, 32), Close, [this] { close(); });
    const auto field = box(25, 60, sheet.w - (picker ? 50 : 133), 38);
    hit(sheet, field, [this] { typingTool = true; sheet.key = -1; paintSheet(); });
    if (p.glass) {
        p.card(d, field, 12);
        if (typingTool) d.glassRim(field, 12, .52f);
    } else {
        d.fill(field, 10, tint(p.ink, .1f));
        if (typingTool) d.outline(field, 10, tint(p.accent, .85f));
    }
    d.icon(command ? Terminal : SearchIcon, box(36, 70, 18, 18), tint(command ? Design::Green : p.muted));
    const Text placeholder = command ? L"Komut yaz" : picker ? L"Kurulu uygulamaları ara" : L"Uygulama, klasör veya web ara";
    d.text(toolInput.empty() ? placeholder : toolInput + (typingTool ? L"|" : L""),
        box(63, 60, width(field) - 46, 38), Body, tint(toolInput.empty() ? p.muted : p.ink));
    if (!picker) button(sheet, p, box(sheet.w - 99, 60, 74, 38), command ? L"Çalıştır" : L"Aç / Ara",
        [this] { runTool(toolInput); }, true);

    struct Match { const Shortcut* app; size_t index; bool pinned; };
    std::vector<Match> matches;
    if (picker || !toolInput.empty()) {
        for (size_t i = 0; i < model.shortcuts.size(); ++i)
            if (contains(model.shortcuts[i].label, toolInput)) matches.push_back({&model.shortcuts[i], i, true});
        for (size_t i = 0; i < installedApps.size(); ++i) {
            const auto& entry = installedApps[i];
            if (pinnedIndex(entry) < 0 && contains(entry.label, toolInput)) matches.push_back({&entry, i, false});
        }
    }
    std::vector<const Command*> commands;
    if (command) for (const auto& entry : CommandsList)
        if (contains(entry.title, toolInput) || contains(entry.shortcut, toolInput)) commands.push_back(&entry);
    const int count = static_cast<int>(commands.size() + matches.size());
    constexpr int visible = 6;
    listOffset = std::clamp(listOffset, 0, std::max(0, count - visible));
    if (!count) d.text(installedRequest.valid() ? L"Windows uygulamaları yükleniyor…" : !toolInput.empty() ? L"Eşleşen uygulama veya komut yok." :
        picker ? L"Windows uygulama listesi boş." : L"Aramak veya bir yol açmak için yaz.", box(25, 114, sheet.w - 50, 39), Body, tint(p.muted));
    std::vector<Text> missingIcons;
    for (int n = 0; n < visible && listOffset + n < count; ++n) {
        const size_t index = listOffset + n;
        if (index < commands.size()) {
            const auto item = *commands[index];
            button(sheet, p, box(25, 112 + 42.f * n, sheet.w - 50, 38),
                Text(item.shortcut) + L"  ·  " + item.title, [this, item] { runTool(item.shortcut); }, false, Body);
            continue;
        }
        const auto& match = matches[index - commands.size()];
        std::shared_ptr<Gdiplus::Bitmap> image;
        if (match.pinned) {
            if (match.index < appImages.size()) image = appImages[match.index];
        } else {
            if (match.index < installedImages.size()) image = installedImages[match.index];
            if (!image) missingIcons.push_back(match.app->target);
        }
        row(n, *match.app, image, true);
    }
    if (!missingIcons.empty()) requestInstalledAppImages(missingIcons);
    if (picker) d.text(notice.empty() ? L"Sabitlemek veya kaldırmak için sağ tıkla." : notice,
        box(26, sheet.h - 50, sheet.w - 135, 33), Caption, tint(notice.empty() ? p.muted : p.ink));
    paging(count, visible);
}
