#include "widgets.h"
#include <appmodel.h>
#include <objidl.h>
#include <propkey.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {
constexpr auto AppsPrefix = L"shell:AppsFolder\\";
constexpr uint32_t CustomFooterMagic = 0xBABFFBABu;

// AppUserModel property set {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}
constexpr GUID FmtAppUserModel = {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}};
constexpr PROPERTYKEY KeyAumidId = {FmtAppUserModel, 5};
constexpr PROPERTYKEY KeyIsSeparator = {FmtAppUserModel, 6};
constexpr PROPERTYKEY KeyUwpArgs = {FmtAppUserModel, 20};
constexpr PROPERTYKEY KeyUwpDisplayName = {FmtAppUserModel, 27};
constexpr PROPERTYKEY KeyUwpDescription = {FmtAppUserModel, 28};
constexpr PROPERTYKEY KeyUwpLogoUri = {FmtAppUserModel, 29};

// PKEY_Link_Arguments {436F2667-14E2-4FEB-B30A-146C53B5B674}:100
constexpr PROPERTYKEY KeyLinkArguments = {{0x436F2667, 0x14E2, 0x4FEB, {0xB3, 0x0A, 0x14, 0x6C, 0x53, 0xB5, 0xB6, 0x74}}, 100};

Text lowerStr(const Text& s) {
    Text out;
    out.reserve(s.size());
    for (wchar_t ch : s) out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return out;
}

bool sameI(const Text& a, const Text& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

Text expandEnv(const Text& input) {
    if (input.find(L'%') == Text::npos) return input;
    wchar_t buf[32768]{};
    const DWORD n = ExpandEnvironmentStringsW(input.c_str(), buf, ARRAYSIZE(buf));
    return (n > 0 && n <= ARRAYSIZE(buf)) ? Text(buf) : input;
}

Text resolveIndirect(const Text& raw) {
    if (raw.empty()) return {};
    if (raw[0] != L'@' && !raw.starts_with(L"ms-resource:")) return raw;
    wchar_t out[1024]{};
    if (SUCCEEDED(SHLoadIndirectString(raw.c_str(), out, ARRAYSIZE(out), nullptr)) && *out)
        return out;
    return raw;
}

Text readPropString(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return {};
    PROPVARIANT pv{};
    Text out;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_LPWSTR && pv.pwszVal) out = pv.pwszVal;
        else if (pv.vt == VT_BSTR && pv.bstrVal) out = pv.bstrVal;
    }
    PropVariantClear(&pv);
    return out;
}

bool readPropBool(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return false;
    PROPVARIANT pv{};
    bool out = false;
    if (SUCCEEDED(store->GetValue(key, &pv))) {
        if (pv.vt == VT_BOOL) out = (pv.boolVal != VARIANT_FALSE);
        else if (pv.vt == VT_UI4) out = (pv.ulVal != 0);
    }
    PropVariantClear(&pv);
    return out;
}

Text shellDisplayName(IShellItem* item, SIGDN type) {
    if (!item) return {};
    PWSTR p{};
    if (FAILED(item->GetDisplayName(type, &p)) || !p) return {};
    Text out(p);
    CoTaskMemFree(p);
    return out;
}

Text formatHexMin(uint64_t value) {
    wchar_t buf[20]{};
    swprintf_s(buf, L"%llx", static_cast<unsigned long long>(value));
    return buf;
}

// Windows Jump List AppID CRC-64 (reflected polynomial 0x92C64265D32139A4, init all-1s, no final XOR)
// Note: Windows formats the hex AppID filename without leading zeros (%llx).
Text computeAppIdHash(const Text& appId) {
    if (appId.empty()) return {};
    Text upper(appId.size(), L'\0');
    const int mapped = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        appId.c_str(), static_cast<int>(appId.size()),
        upper.data(), static_cast<int>(upper.size()), nullptr, nullptr, 0);
    if (mapped <= 0) {
        upper = appId;
        for (wchar_t& ch : upper) ch = static_cast<wchar_t>(std::towupper(ch));
    }

    static const auto table = [] {
        std::array<uint64_t, 256> t{};
        for (uint64_t i = 0; i < 256; ++i) {
            uint64_t c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c & 1ULL) ? (0x92C64265D32139A4ULL ^ (c >> 1)) : (c >> 1);
            t[static_cast<size_t>(i)] = c;
        }
        return t;
    }();

    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    const auto* bytes = reinterpret_cast<const uint8_t*>(upper.data());
    const size_t byteLen = upper.size() * sizeof(wchar_t);
    for (size_t i = 0; i < byteLen; ++i)
        crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return formatHexMin(crc);
}

struct KnownFolderRule {
    Text prefixLower;
    Text guidToken;
};

const std::vector<KnownFolderRule>& knownFolderRules() {
    static const std::vector<KnownFolderRule> rules = [] {
        struct Entry { const KNOWNFOLDERID* id; const wchar_t* guid; };
        const Entry entries[] = {
            {&FOLDERID_ProgramFilesX86, L"{7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E}"},
            {&FOLDERID_ProgramFilesX64, L"{6D809377-6AF0-444B-8957-A3773F02200E}"},
            {&FOLDERID_ProgramFiles, L"{905E63B6-C1BF-494E-B29C-65B732D3D21A}"},
            {&FOLDERID_SystemX86, L"{D65231B0-B2F1-4857-A4CE-A8E7C6EA7D27}"},
            {&FOLDERID_System, L"{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}"},
            {&FOLDERID_Windows, L"{F38BF404-1D43-42F2-9305-67DE0B28FC23}"},
            {&FOLDERID_UserProgramFiles, L"{5CD7AEE2-2219-4A67-B85D-6C9CE15660CB}"},
            {&FOLDERID_LocalAppData, L"{F1B32785-6FBA-4FCF-9D55-7B8E7F157091}"},
            {&FOLDERID_RoamingAppData, L"{3EB685DB-65F9-4CF6-A03A-E3EF65729F3D}"},
            {&FOLDERID_Profile, L"{5E6C858F-0E22-4760-9AFE-EA3317B67173}"},
        };
        std::vector<KnownFolderRule> list;
        for (const auto& e : entries) {
            PWSTR p{};
            if (SUCCEEDED(SHGetKnownFolderPath(*e.id, KF_FLAG_DEFAULT, nullptr, &p)) && p) {
                Text pref = lowerStr(p);
                CoTaskMemFree(p);
                while (!pref.empty() && (pref.back() == L'\\' || pref.back() == L'/')) pref.pop_back();
                if (!pref.empty()) list.push_back({std::move(pref), e.guid});
            }
        }
        std::sort(list.begin(), list.end(), [](const KnownFolderRule& a, const KnownFolderRule& b) {
            return a.prefixLower.size() > b.prefixLower.size();
        });
        return list;
    }();
    return rules;
}

void addKnownFolderVariants(const Text& rawPath, std::vector<Text>& out) {
    if (rawPath.empty()) return;
    Text path = expandEnv(rawPath);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    const Text low = lowerStr(path);
    for (const auto& rule : knownFolderRules()) {
        if (low.starts_with(rule.prefixLower) &&
            (low.size() == rule.prefixLower.size() || low[rule.prefixLower.size()] == L'\\')) {
            out.push_back(rule.guidToken + path.substr(rule.prefixLower.size()));
        }
    }
    out.push_back(path);
}

struct AppsFolderIndex {
    std::unordered_map<Text, std::vector<Text>> byExePath;
    std::unordered_map<Text, std::vector<Text>> byExeStem;
    std::unordered_map<Text, std::vector<Text>> byDisplayName;
    ULONGLONG builtAt = 0;
};

std::mutex gIndexMutex;
AppsFolderIndex gAppsIndex;

void appendUnique(std::vector<Text>& vec, const Text& val) {
    if (val.empty()) return;
    for (const auto& existing : vec)
        if (sameI(existing, val)) return;
    vec.push_back(val);
}

void indexStartMenuLinks(const KNOWNFOLDERID& folderId, AppsFolderIndex& idx) {
    PWSTR p{};
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &p)) || !p) return;
    const fs::path root(p);
    CoTaskMemFree(p);

    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (!sameI(it->path().extension().wstring(), L".lnk")) continue;
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> pf;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&pf)) || FAILED(pf->Load(it->path().c_str(), STGM_READ)))
            continue;
        ComPtr<IPropertyStore> store;
        Text aumid;
        if (SUCCEEDED(link.As(&store))) aumid = readPropString(store.Get(), PKEY_AppUserModel_ID);
        if (aumid.empty()) continue;

        wchar_t targetBuf[32768]{};
        if (SUCCEEDED(link->GetPath(targetBuf, ARRAYSIZE(targetBuf), nullptr, 0)) && *targetBuf) {
            Text exe = expandEnv(targetBuf);
            std::replace(exe.begin(), exe.end(), L'/', L'\\');
            appendUnique(idx.byExePath[lowerStr(exe)], aumid);
            const Text stem = lowerStr(fs::path(exe).stem().wstring());
            if (!stem.empty()) appendUnique(idx.byExeStem[stem], aumid);
        }
        const Text lnkStem = lowerStr(it->path().stem().wstring());
        if (!lnkStem.empty()) appendUnique(idx.byDisplayName[lnkStem], aumid);
    }
}

AppsFolderIndex buildAppsFolderIndex(bool includeShellAppsFolder) {
    AppsFolderIndex idx;
    indexStartMenuLinks(FOLDERID_Programs, idx);
    indexStartMenuLinks(FOLDERID_CommonPrograms, idx);
    if (!includeShellAppsFolder) {
        idx.builtAt = GetTickCount64();
        return idx;
    }

    ComPtr<IShellItem> folder;
    ComPtr<IEnumShellItems> entries;
    if (FAILED(SHCreateItemFromParsingName(L"shell:AppsFolder", nullptr, IID_PPV_ARGS(&folder))) ||
        FAILED(folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&entries)))) {
        idx.builtAt = GetTickCount64();
        return idx;
    }
    ComPtr<IShellItem> item;
    while (entries->Next(1, item.ReleaseAndGetAddressOf(), nullptr) == S_OK) {
        ComPtr<IShellItem2> details;
        if (FAILED(item.As(&details))) continue;
        PWSTR pAumid{};
        Text aumid;
        if (SUCCEEDED(details->GetString(PKEY_AppUserModel_ID, &pAumid)) && pAumid) {
            aumid = pAumid;
            CoTaskMemFree(pAumid);
        } else {
            aumid = shellDisplayName(item.Get(), SIGDN_PARENTRELATIVEPARSING);
        }
        if (aumid.empty()) continue;

        const Text name = shellDisplayName(item.Get(), SIGDN_NORMALDISPLAY);
        if (!name.empty()) appendUnique(idx.byDisplayName[lowerStr(name)], aumid);

        PWSTR pLink{};
        if (SUCCEEDED(details->GetString(PKEY_Link_TargetParsingPath, &pLink)) && pLink) {
            Text exe = expandEnv(pLink);
            CoTaskMemFree(pLink);
            if (!exe.empty()) {
                std::replace(exe.begin(), exe.end(), L'/', L'\\');
                appendUnique(idx.byExePath[lowerStr(exe)], aumid);
                const Text stem = lowerStr(fs::path(exe).stem().wstring());
                if (!stem.empty()) appendUnique(idx.byExeStem[stem], aumid);
            }
        }
    }
    idx.builtAt = GetTickCount64();
    return idx;
}

bool gIndexHasAppsFolder = false;

AppsFolderIndex getAppsFolderIndex(bool includeShellAppsFolder = false, bool forceRefresh = false) {
    std::lock_guard lock(gIndexMutex);
    const ULONGLONG now = GetTickCount64();
    const bool needUpgrade = includeShellAppsFolder && !gIndexHasAppsFolder;
    const bool stale = !gAppsIndex.builtAt || (now - gAppsIndex.builtAt > 300000ULL) ||
        needUpgrade || (forceRefresh && now - gAppsIndex.builtAt > 12000ULL);
    if (stale) {
        const bool withShell = includeShellAppsFolder || gIndexHasAppsFolder;
        gAppsIndex = buildAppsFolderIndex(withShell);
        gIndexHasAppsFolder = withShell;
    }
    return gAppsIndex;
}

Text getProcessExePath(DWORD pid) {
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t buf[32768]{};
    DWORD len = ARRAYSIZE(buf);
    Text out;
    if (QueryFullProcessImageNameW(proc, 0, buf, &len) && len > 0) out.assign(buf, len);
    CloseHandle(proc);
    return out;
}

Text getProcessAumid(DWORD pid) {
    if (!pid) return {};
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};
    wchar_t id[256]{};
    UINT32 len = ARRAYSIZE(id);
    Text out;
    if (GetApplicationUserModelId(proc, &len, id) == ERROR_SUCCESS && len > 1) out = id;
    CloseHandle(proc);
    return out;
}

Text resolveUwpLogoPath(const Text& aumid, const Text& logoUri) {
    if (logoUri.empty() || aumid.empty()) return {};
    const size_t bang = aumid.find(L'!');
    if (bang == Text::npos) return {};
    const Text pfn = aumid.substr(0, bang);
    const Text low = lowerStr(logoUri);
    if (low.starts_with(L"ms-appdata:///local/")) {
        PWSTR localApp{};
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localApp)) && localApp) {
            Text rel = logoUri.substr(wcslen(L"ms-appdata:///local/"));
            std::replace(rel.begin(), rel.end(), L'/', L'\\');
            Text full = Text(localApp) + L"\\Packages\\" + pfn + L"\\LocalState\\" + rel;
            CoTaskMemFree(localApp);
            if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) return full;
        }
    }
    return {};
}

bool readFileShared(const fs::path& filePath, std::vector<BYTE>& outBytes) {
    outBytes.clear();
    HANDLE h = CreateFileW(filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = false;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 8 * 1024 * 1024) {
        outBytes.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, outBytes.data(), static_cast<DWORD>(outBytes.size()), &got, nullptr) && got == outBytes.size();
    }
    CloseHandle(h);
    if (!ok) outBytes.clear();
    return ok;
}

bool extractJumpItemFromLink(IShellLinkW* link, JumpItem& item, bool validateFilesystem) {
    if (!link) return false;

    ComPtr<IPropertyStore> store;
    link->QueryInterface(IID_PPV_ARGS(&store));
    if (readPropBool(store.Get(), KeyIsSeparator)) return false;

    item.aumid = readPropString(store.Get(), KeyAumidId);

    Text title = resolveIndirect(readPropString(store.Get(), PKEY_Title));
    if (title.empty()) title = resolveIndirect(readPropString(store.Get(), KeyUwpDisplayName));

    wchar_t argsBuf[4096]{};
    if (SUCCEEDED(link->GetArguments(argsBuf, ARRAYSIZE(argsBuf))) && *argsBuf)
        item.args = argsBuf;
    if (item.args.empty()) item.args = readPropString(store.Get(), KeyUwpArgs);
    if (item.args.empty()) item.args = readPropString(store.Get(), KeyLinkArguments);

    wchar_t dirBuf[2048]{};
    if (SUCCEEDED(link->GetWorkingDirectory(dirBuf, ARRAYSIZE(dirBuf))) && *dirBuf)
        item.workingDir = expandEnv(dirBuf);

    wchar_t pathBuf[32768]{};
    if (item.aumid.empty()) {
        if (FAILED(link->GetPath(pathBuf, ARRAYSIZE(pathBuf), nullptr, SLGP_RAWPATH)) || !*pathBuf)
            link->GetPath(pathBuf, ARRAYSIZE(pathBuf), nullptr, 0);
        if (*pathBuf) item.targetPath = expandEnv(pathBuf);
    }

    PIDLIST_ABSOLUTE pidl{};
    if (SUCCEEDED(link->GetIDList(&pidl)) && pidl) {
        const UINT pidlSize = ILGetSize(pidl);
        if (item.aumid.empty() && pidlSize > 0 && pidlSize <= 65536) {
            item.pidlBytes.resize(pidlSize);
            memcpy(item.pidlBytes.data(), pidl, pidlSize);
        }
        ComPtr<IShellItem> shellItem;
        if (SUCCEEDED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&shellItem))) && shellItem) {
            if (title.empty()) title = shellDisplayName(shellItem.Get(), SIGDN_NORMALDISPLAY);
            if (item.aumid.empty() && item.targetPath.empty()) {
                const Text fsPath = shellDisplayName(shellItem.Get(), SIGDN_FILESYSPATH);
                if (!fsPath.empty()) item.targetPath = fsPath;
            }
            SFGAOF attrs = 0;
            if (SUCCEEDED(shellItem->GetAttributes(SFGAO_FOLDER | SFGAO_STREAM, &attrs))) {
                if ((attrs & SFGAO_FOLDER) && !(attrs & SFGAO_STREAM)) item.isFolder = true;
            }
        }
        CoTaskMemFree(pidl);
    }

    if (title.empty()) {
        wchar_t descBuf[1024]{};
        if (SUCCEEDED(link->GetDescription(descBuf, ARRAYSIZE(descBuf))) && *descBuf)
            title = resolveIndirect(descBuf);
    }
    if (title.empty() && !item.targetPath.empty()) {
        title = fs::path(item.targetPath).filename().wstring();
        if (title.empty()) title = item.targetPath;
    }
    // Trim whitespace / control chars from title
    for (wchar_t& ch : title) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    while (!title.empty() && title.front() == L' ') title.erase(title.begin());
    while (!title.empty() && title.back() == L' ') title.pop_back();
    if (title.empty()) return false;
    item.label = std::move(title);

    // Filter out non-existent local files/folders in AutomaticDestinations
    if (validateFilesystem && item.aumid.empty() && !item.targetPath.empty()) {
        const bool isUnc = item.targetPath.starts_with(L"\\\\");
        const bool isUri = item.targetPath.find(L"://") != Text::npos || item.targetPath.starts_with(L"shell:");
        if (!isUnc && !isUri) {
            const DWORD attr = GetFileAttributesW(item.targetPath.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) {
                if (item.pidlBytes.empty()) return false;
                // Even with PIDL, skip obviously deleted local drive paths (e.g. C:\Windows\Temp\...)
                if (item.targetPath.size() >= 3 && item.targetPath[1] == L':' && item.targetPath[2] == L'\\')
                    return false;
            } else {
                if (attr & FILE_ATTRIBUTE_HIDDEN) return false;
                if (lowerStr(item.targetPath).find(L"\\windows\\installer\\") != Text::npos) return false;
                if (attr & FILE_ATTRIBUTE_DIRECTORY) item.isFolder = true;
            }
        }
    } else if (!item.targetPath.empty() && !item.targetPath.starts_with(L"\\\\")) {
        const DWORD attr = GetFileAttributesW(item.targetPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            item.isFolder = true;
    }

    // Must have at least one launch mechanism
    if (item.aumid.empty() && item.targetPath.empty() && item.pidlBytes.empty()) return false;

    // Resolve item icon (favicons, incognito icon, folder/file icons, UWP local logos)
    wchar_t iconBuf[2048]{};
    int iconIdx = 0;
    if (SUCCEEDED(link->GetIconLocation(iconBuf, ARRAYSIZE(iconBuf), &iconIdx)) && *iconBuf) {
        const Text expandedIcon = expandEnv(iconBuf);
        item.icon = iconFromLocation(expandedIcon, iconIdx);
    }
    if (!item.icon && !item.aumid.empty()) {
        const Text logoUri = readPropString(store.Get(), KeyUwpLogoUri);
        const Text localLogo = resolveUwpLogoPath(item.aumid, logoUri);
        if (!localLogo.empty()) item.icon = iconFromLocation(localLogo, 0);
    }
    if (!item.icon && item.aumid.empty() && !item.targetPath.empty() && !item.targetPath.starts_with(L"\\\\")) {
        item.icon = iconFromLocation(item.targetPath, 0);
    }
    return true;
}

template <class T>
bool readPod(IStream* stream, T& value) {
    ULONG got = 0;
    return SUCCEEDED(stream->Read(&value, sizeof(T), &got)) && got == sizeof(T);
}

bool skipToFooter(IStream* stream) {
    uint32_t window = 0;
    uint8_t byte = 0;
    ULONG got = 0;
    size_t scanned = 0;
    while (scanned < 512 * 1024 && SUCCEEDED(stream->Read(&byte, 1, &got)) && got == 1) {
        window = (window >> 8) | (static_cast<uint32_t>(byte) << 24);
        ++scanned;
        if (scanned >= 4 && window == CustomFooterMagic) return true;
    }
    return false;
}

struct ParsedCustom {
    std::vector<JumpCategory> taskCategories;
    std::vector<JumpCategory> customCategories;
    bool hasKnownCategory = false;
    uint32_t knownCategoryId = 0; // 1 = Frequent, 2 = Recent
};

ParsedCustom parseCustomDestinations(const fs::path& filePath) {
    ParsedCustom result;
    std::vector<BYTE> bytes;
    if (!readFileShared(filePath, bytes) || bytes.size() < 12) return result;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return result;
    if (void* ptr = GlobalLock(mem)) {
        memcpy(ptr, bytes.data(), bytes.size());
        GlobalUnlock(mem);
    }
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream)) || !stream) {
        GlobalFree(mem);
        return result;
    }

    uint32_t version = 0, categoryCount = 0, reserved = 0;
    if (!readPod(stream.Get(), version) || !readPod(stream.Get(), categoryCount) || !readPod(stream.Get(), reserved))
        return result;
    if (version == 0 || version > 8 || categoryCount == 0 || categoryCount > 64)
        return result;

    for (uint32_t c = 0; c < categoryCount; ++c) {
        uint32_t ctype = 0;
        if (!readPod(stream.Get(), ctype)) break;

        if (ctype == 1) {
            uint32_t knownId = 0, footer = 0;
            if (!readPod(stream.Get(), knownId) || !readPod(stream.Get(), footer)) break;
            if (knownId == 1 || knownId == 2) {
                result.hasKnownCategory = true;
                result.knownCategoryId = knownId;
            }
            if (footer != CustomFooterMagic && !skipToFooter(stream.Get())) break;
            continue;
        }

        if (ctype != 0 && ctype != 2) break;

        JumpCategory cat;
        cat.isTasks = (ctype == 2);
        if (ctype == 0) {
            uint16_t nameLen = 0;
            if (!readPod(stream.Get(), nameLen) || nameLen > 512) break;
            if (nameLen > 0) {
                Text rawName(nameLen, L'\0');
                ULONG got = 0;
                const ULONG byteLen = static_cast<ULONG>(nameLen * sizeof(wchar_t));
                if (FAILED(stream->Read(rawName.data(), byteLen, &got)) || got != byteLen) break;
                while (!rawName.empty() && rawName.back() == L'\0') rawName.pop_back();
                cat.title = resolveIndirect(rawName);
            }
            if (cat.title.empty()) cat.title = L"Diğer";
        } else {
            cat.title = L"Görevler";
        }

        uint32_t itemCount = 0;
        if (!readPod(stream.Get(), itemCount) || itemCount > 128) break;

        bool syncedFooter = false;
        std::unordered_set<Text> seen;
        const size_t maxItems = cat.isTasks ? 12u : 6u;
        for (uint32_t i = 0; i < itemCount; ++i) {
            CLSID clsid{};
            if (!readPod(stream.Get(), clsid)) { syncedFooter = true; break; }
            if (IsEqualCLSID(clsid, CLSID_ShellLink)) {
                ComPtr<IShellLinkW> link;
                ComPtr<IPersistStream> persist;
                if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
                    FAILED(link.As(&persist)) || FAILED(persist->Load(stream.Get()))) {
                    syncedFooter = skipToFooter(stream.Get());
                    break;
                }
                if (cat.items.size() < maxItems) {
                    JumpItem item;
                    if (extractJumpItemFromLink(link.Get(), item, false)) {
                        const Text dedup = lowerStr(item.label) + L"|" + lowerStr(item.args) + L"|" + lowerStr(item.targetPath);
                        if (seen.insert(dedup).second) cat.items.push_back(std::move(item));
                    }
                }
            } else {
                syncedFooter = skipToFooter(stream.Get());
                break;
            }
        }

        if (!syncedFooter) {
            uint32_t footer = 0;
            if (!readPod(stream.Get(), footer)) break;
            if (footer != CustomFooterMagic && !skipToFooter(stream.Get())) break;
        }

        if (!cat.items.empty()) {
            if (cat.isTasks) result.taskCategories.push_back(std::move(cat));
            else result.customCategories.push_back(std::move(cat));
        }
    }
    return result;
}

struct ParsedAuto {
    std::vector<JumpItem> pinned;
    std::vector<JumpItem> recent;
    bool empty() const { return pinned.empty() && recent.empty(); }
};

ParsedAuto parseAutomaticDestinations(const fs::path& filePath) {
    ParsedAuto result;
    std::vector<BYTE> bytes;
    if (!readFileShared(filePath, bytes) || bytes.size() < 512) return result;

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return result;
    if (void* ptr = GlobalLock(mem)) {
        memcpy(ptr, bytes.data(), bytes.size());
        GlobalUnlock(mem);
    }
    ComPtr<ILockBytes> lockBytes;
    if (FAILED(CreateILockBytesOnHGlobal(mem, TRUE, &lockBytes)) || !lockBytes) {
        GlobalFree(mem);
        return result;
    }
    ComPtr<IStorage> stg;
    if (FAILED(StgOpenStorageOnILockBytes(lockBytes.Get(), nullptr,
            STGM_READ | STGM_SHARE_EXCLUSIVE, nullptr, 0, &stg)) || !stg)
        return result;

    ComPtr<IStream> destStream;
    if (FAILED(stg->OpenStream(L"DestList", nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &destStream)) || !destStream)
        return result;

    STATSTG stat{};
    if (FAILED(destStream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart < 32 || stat.cbSize.QuadPart > 4 * 1024 * 1024)
        return result;

    std::vector<BYTE> dl(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG got = 0;
    if (FAILED(destStream->Read(dl.data(), static_cast<ULONG>(dl.size()), &got)) || got < 32)
        return result;
    dl.resize(got);

    uint32_t ver = 0, totalEntries = 0;
    memcpy(&ver, dl.data() + 0, 4);
    memcpy(&totalEntries, dl.data() + 4, 4);
    if (totalEntries == 0 || totalEntries > 512) return result;

    const size_t entryHeaderSize = (ver >= 3) ? 130u : 114u;
    const size_t strLenOff = (ver >= 3) ? 128u : 112u;
    const size_t entryTail = (ver >= 3) ? 4u : 0u;

    struct RawEntry {
        uint64_t id = 0;
        int32_t pinStatus = -1;
        size_t order = 0;
    };
    std::vector<RawEntry> entries;
    size_t offset = 32;
    for (uint32_t i = 0; i < totalEntries; ++i) {
        if (offset + entryHeaderSize > dl.size()) break;
        uint64_t entryId = 0;
        int32_t pinStatus = -1;
        uint16_t strLen = 0;
        memcpy(&entryId, dl.data() + offset + 88, 8);
        memcpy(&pinStatus, dl.data() + offset + 108, 4);
        memcpy(&strLen, dl.data() + offset + strLenOff, 2);
        const size_t nextOff = offset + entryHeaderSize + static_cast<size_t>(strLen) * 2u + entryTail;
        if (nextOff > dl.size()) break;
        entries.push_back({entryId, pinStatus, i});
        offset = nextOff;
    }

    std::vector<std::pair<int32_t, JumpItem>> pinnedRaw;
    std::unordered_set<Text> seenPinned, seenRecent;

    for (const auto& entry : entries) {
        if (entry.pinStatus < 0 && result.recent.size() >= 6) continue;
        if (entry.pinStatus >= 0 && pinnedRaw.size() >= 8) continue;

        const Text streamName = formatHexMin(entry.id);
        ComPtr<IStream> linkStream;
        if (FAILED(stg->OpenStream(streamName.c_str(), nullptr, STGM_READ | STGM_SHARE_EXCLUSIVE, 0, &linkStream)) || !linkStream)
            continue;

        ComPtr<IShellLinkW> link;
        ComPtr<IPersistStream> persist;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&persist)) || FAILED(persist->Load(linkStream.Get())))
            continue;

        JumpItem item;
        if (!extractJumpItemFromLink(link.Get(), item, true)) continue;
        const Text dedup = lowerStr(item.label) + L"|" + lowerStr(item.targetPath);
        if (entry.pinStatus >= 0) {
            if (seenPinned.insert(dedup).second) pinnedRaw.emplace_back(entry.pinStatus, std::move(item));
        } else {
            if (seenRecent.insert(dedup).second) result.recent.push_back(std::move(item));
        }
    }

    std::sort(pinnedRaw.begin(), pinnedRaw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    result.pinned.reserve(pinnedRaw.size());
    for (auto& pair : pinnedRaw) result.pinned.push_back(std::move(pair.second));
    return result;
}

std::vector<Text> collectCandidateAppIds(const Shortcut& app, const std::vector<HWND>& appWindows, const AppsFolderIndex& idx) {
    std::vector<Text> candidates;
    std::vector<Text> exePaths;

    auto addCand = [&](const Text& id) {
        if (id.empty()) return;
        Text clean = id;
        if (clean.starts_with(AppsPrefix)) clean = clean.substr(wcslen(AppsPrefix));
        if (!clean.empty()) appendUnique(candidates, clean);
    };
    auto addExe = [&](const Text& raw) {
        if (raw.empty()) return;
        Text p = expandEnv(raw);
        std::replace(p.begin(), p.end(), L'/', L'\\');
        if (p.find(L'\\') != Text::npos || p.ends_with(L".exe") || p.ends_with(L".EXE"))
            appendUnique(exePaths, p);
    };

    // 1. Live open window AUMIDs (highest accuracy for running apps, including Store/Electron/custom)
    for (HWND w : appWindows) {
        if (!w || !IsWindow(w)) continue;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(SHGetPropertyStoreForWindow(w, IID_PPV_ARGS(&store))) && store) {
            const Text winAumid = readPropString(store.Get(), PKEY_AppUserModel_ID);
            if (!winAumid.empty()) addCand(winAumid);
        }
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        const Text procAumid = getProcessAumid(pid);
        if (!procAumid.empty()) addCand(procAumid);
        const Text procExe = getProcessExePath(pid);
        if (sameI(leaf(procExe), L"ApplicationFrameHost.exe")) {
            Text childAumid;
            EnumChildWindows(w, [](HWND child, LPARAM lp) -> BOOL {
                DWORD cpid = 0;
                GetWindowThreadProcessId(child, &cpid);
                const Text id = getProcessAumid(cpid);
                if (!id.empty()) {
                    *reinterpret_cast<Text*>(lp) = id;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&childAumid));
            if (!childAumid.empty()) addCand(childAumid);
        } else if (!procExe.empty()) {
            addExe(procExe);
        }
    }

    // 2. Direct target / identity from Shortcut
    if (app.target.starts_with(AppsPrefix)) {
        addCand(app.target);
    } else if (sameI(fs::path(app.target).extension().wstring(), L".lnk")) {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> pf;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
            SUCCEEDED(link.As(&pf)) && SUCCEEDED(pf->Load(app.target.c_str(), STGM_READ))) {
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(link.As(&store))) {
                const Text lnkId = readPropString(store.Get(), PKEY_AppUserModel_ID);
                if (!lnkId.empty()) addCand(lnkId);
            }
            wchar_t targetBuf[32768]{};
            if (SUCCEEDED(link->GetPath(targetBuf, ARRAYSIZE(targetBuf), nullptr, 0)) && *targetBuf)
                addExe(targetBuf);
        }
    } else if (!app.target.empty()) {
        addExe(app.target);
    }

    if (!app.identity.empty()) {
        if (app.identity.find(L'\\') == Text::npos && app.identity.find(L'/') == Text::npos)
            addCand(app.identity);
        else
            addExe(app.identity);
    }

    // 3. Resolve exe paths via shell:AppsFolder index (works universally for Chrome, VS Code, Antigravity, future installs)
    std::vector<Text> stems;
    for (const auto& exe : exePaths) {
        const Text lowExe = lowerStr(exe);
        if (const auto it = idx.byExePath.find(lowExe); it != idx.byExePath.end()) {
            for (const auto& id : it->second) addCand(id);
        }
        const Text fileLeaf = lowerStr(leaf(exe));
        if (fileLeaf == L"explorer.exe") addCand(L"Microsoft.Windows.Explorer");
        const Text stem = lowerStr(fs::path(exe).stem().wstring());
        if (!stem.empty()) {
            appendUnique(stems, stem);
            if (const auto it = idx.byExeStem.find(stem); it != idx.byExeStem.end()) {
                for (const auto& id : it->second) addCand(id);
            }
        }
    }

    // 4. Display-name lookup in shell:AppsFolder (handles Store/MSIX or renamed executables)
    if (!app.label.empty()) {
        if (const auto it = idx.byDisplayName.find(lowerStr(app.label)); it != idx.byDisplayName.end()) {
            for (const auto& id : it->second) addCand(id);
        }
    }

    // 5. KnownFolder-normalized and raw exe path candidates (standard Win32 fallback AppIDs)
    for (const auto& exe : exePaths) {
        std::vector<Text> variants;
        addKnownFolderVariants(exe, variants);
        for (const auto& v : variants) addCand(v);
    }

    // 6. Electron AutomaticDestinations convention (electron.app.<Stem>)
    for (const auto& stem : stems) {
        addCand(L"electron.app." + stem);
    }

    return candidates;
}

JumpListData resolveFromCandidates(const std::vector<Text>& candidates, const fs::path& customDir, const fs::path& autoDir,
    bool isExplorer) {
    ParsedCustom bestCustom;
    Text customWinningHash;
    bool hasCustom = false;

    for (const auto& cand : candidates) {
        const Text hash = computeAppIdHash(cand);
        if (hash.empty()) continue;
        const fs::path customFile = customDir / (hash + L".customDestinations-ms");
        if (GetFileAttributesW(customFile.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ParsedCustom parsed = parseCustomDestinations(customFile);
        if (!parsed.taskCategories.empty() || !parsed.customCategories.empty() || parsed.hasKnownCategory) {
            bestCustom = std::move(parsed);
            customWinningHash = hash;
            hasCustom = true;
            break;
        }
    }

    ParsedAuto bestAuto;
    auto tryAutoHash = [&](const Text& hash) -> bool {
        if (hash.empty()) return false;
        const fs::path autoFile = autoDir / (hash + L".automaticDestinations-ms");
        if (GetFileAttributesW(autoFile.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        ParsedAuto parsed = parseAutomaticDestinations(autoFile);
        if (!parsed.empty()) {
            bestAuto = std::move(parsed);
            return true;
        }
        return false;
    };

    bool hasAuto = false;
    if (!customWinningHash.empty()) hasAuto = tryAutoHash(customWinningHash);
    if (!hasAuto) {
        for (const auto& cand : candidates) {
            const Text hash = computeAppIdHash(cand);
            if (hash == customWinningHash) continue;
            if (tryAutoHash(hash)) { hasAuto = true; break; }
        }
    }

    JumpListData out;
    if (hasCustom) {
        for (auto& cat : bestCustom.taskCategories) out.categories.push_back(std::move(cat));
    }
    if (hasAuto && !bestAuto.pinned.empty()) {
        JumpCategory pinnedCat;
        pinnedCat.title = L"Sabitlenenler";
        pinnedCat.items = std::move(bestAuto.pinned);
        out.categories.push_back(std::move(pinnedCat));
    }
    if (hasCustom) {
        for (auto& cat : bestCustom.customCategories) out.categories.push_back(std::move(cat));
    }
    const bool includeAutoRecent = hasAuto && !bestAuto.recent.empty() &&
        (!hasCustom || bestCustom.hasKnownCategory);
    if (includeAutoRecent) {
        JumpCategory recCat;
        const bool frequent = isExplorer || (hasCustom && bestCustom.knownCategoryId == 1);
        recCat.title = frequent ? L"Sık kullanılanlar" : L"Son kullanılanlar";
        recCat.items = std::move(bestAuto.recent);
        out.categories.push_back(std::move(recCat));
    }
    return out;
}
} // namespace

void prewarmJumpListIndex() {
    // Fast Start Menu .lnk index only (~20-40ms); shell:AppsFolder is queried lazily on miss.
    getAppsFolderIndex(false, false);
}

JumpListData readAppJumpList(const Shortcut& app, const std::vector<HWND>& appWindows) {
    PWSTR recentPath{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DEFAULT, nullptr, &recentPath)) || !recentPath)
        return {};
    const fs::path recentDir(recentPath);
    CoTaskMemFree(recentPath);

    const fs::path customDir = recentDir / L"CustomDestinations";
    const fs::path autoDir = recentDir / L"AutomaticDestinations";

    const bool isExplorer = sameI(leaf(app.target), L"explorer.exe") ||
        sameI(app.target, L"shell:AppsFolder\\Microsoft.Windows.Explorer") ||
        sameI(app.identity, L"Microsoft.Windows.Explorer");

    AppsFolderIndex idx = getAppsFolderIndex(false, false);
    std::vector<Text> candidates = collectCandidateAppIds(app, appWindows, idx);
    JumpListData result = resolveFromCandidates(candidates, customDir, autoDir, isExplorer);

    // If nothing matched (e.g. Store/MSIX app pinned by exe/label or newly installed), fall back to full shell:AppsFolder index once.
    if (result.empty()) {
        AppsFolderIndex refreshed = getAppsFolderIndex(true, true);
        if (refreshed.builtAt != idx.builtAt) {
            std::vector<Text> nextCandidates = collectCandidateAppIds(app, appWindows, refreshed);
            result = resolveFromCandidates(nextCandidates, customDir, autoDir, isExplorer);
        }
    }
    return result;
}
