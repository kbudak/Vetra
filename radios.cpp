#include "dock.h"
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.Rfcomm.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <cwctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Networking::Connectivity;
using namespace winrt::Windows::Networking::NetworkOperators;
using AccessOperation = winrt::Windows::Foundation::IAsyncOperation<RadioAccessStatus>;

namespace {
bool readSystemAirplaneMode() {
    DWORD val = 0, size = sizeof(val);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
        L"SYSTEM\\CurrentControlSet\\Control\\RadioManagement\\SystemRadioState",
        nullptr, RRF_RT_REG_DWORD, nullptr, &val, &size) == ERROR_SUCCESS) {
        return val != 0;
    }
    return false;
}

Text macHexLower(ULONGLONG address) {
    wchar_t buf[16]{};
    swprintf_s(buf, L"%012llx", static_cast<unsigned long long>(address & 0xFFFFFFFFFFFFULL));
    return buf;
}

Text toLowerText(const Text& in) {
    Text out = in;
    for (auto& ch : out) ch = static_cast<wchar_t>(towlower(ch));
    return out;
}

bool isHexChar(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

ULONGLONG parseMacString(const Text& raw) {
    Text hex;
    hex.reserve(12);
    for (wchar_t c : raw) {
        if (isHexChar(c)) hex.push_back(static_cast<wchar_t>(towlower(c)));
    }
    if (hex.size() < 12) return 0;
    if (hex.size() > 12) hex = hex.substr(hex.size() - 12);
    ULONGLONG addr = 0;
    for (wchar_t c : hex) {
        addr <<= 4;
        addr |= (c >= L'0' && c <= L'9') ? static_cast<ULONGLONG>(c - L'0')
                                         : static_cast<ULONGLONG>(10 + (c - L'a'));
    }
    return addr;
}

Text extractMacFromFilterId(const Text& lower) {
    // Matches "...&14147dbd3862_c00000000..." in bthenum A2DP filter IDs
    for (size_t pos = 0; (pos = lower.find(L'&', pos)) != Text::npos; ++pos) {
        if (pos + 13 < lower.size() && lower[pos + 13] == L'_') {
            bool allHex = true;
            for (size_t k = 1; k <= 12; ++k) {
                if (!isHexChar(lower[pos + k])) { allHex = false; break; }
            }
            if (allHex) return lower.substr(pos + 1, 12);
        }
    }
    return {};
}

bool isZeroGuid(const GUID& g) {
    static constexpr GUID zero{};
    return IsEqualGUID(g, zero) != FALSE;
}

// Single-pass Core Audio Bluetooth endpoint table (both Render & Capture, linked by ContainerId + MAC).
struct BtAudioEndpoint {
    Text endpointId;
    Text filterId;
    Text macLower;
    GUID containerId{};
    bool hasContainer = false;
    bool active = false;
    bool isRender = false;
    bool isStereo = false;
};

std::vector<BtAudioEndpoint> collectAllBtAudioEndpoints() {
    std::vector<BtAudioEndpoint> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator))) || !enumerator)
        return result;
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED, &collection)) || !collection)
        return result;
    constexpr PROPERTYKEY kContainerId{{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;
        LPWSTR rawEpId{};
        if (FAILED(device->GetId(&rawEpId)) || !rawEpId) continue;
        Text epId = rawEpId;
        CoTaskMemFree(rawEpId);

        DWORD state = 0;
        device->GetState(&state);
        EDataFlow flow = eRender;
        ComPtr<IMMEndpoint> epMeta;
        if (SUCCEEDED(device.As(&epMeta)) && epMeta) epMeta->GetDataFlow(&flow);

        ComPtr<IDeviceTopology> topo;
        if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(topo.GetAddressOf()))) || !topo) continue;
        ComPtr<IConnector> conn;
        if (FAILED(topo->GetConnector(0, &conn)) || !conn) continue;
        ComPtr<IConnector> peer;
        if (FAILED(conn->GetConnectedTo(&peer)) || !peer) continue;
        ComPtr<IPart> part;
        if (FAILED(peer.As(&part)) || !part) continue;
        ComPtr<IDeviceTopology> peerTopo;
        if (FAILED(part->GetTopologyObject(&peerTopo)) || !peerTopo) continue;
        LPWSTR rawFilterId{};
        if (FAILED(peerTopo->GetDeviceId(&rawFilterId)) || !rawFilterId) continue;
        Text filterId = rawFilterId;
        CoTaskMemFree(rawFilterId);

        const Text lower = toLowerText(filterId);
        if (lower.find(L"bth") == Text::npos && lower.find(L"bluetooth") == Text::npos) continue;

        GUID cid{};
        bool hasCid = false;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            PROPVARIANT pv{};
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(kContainerId, &pv))) {
                if (pv.vt == VT_CLSID && pv.puuid && !isZeroGuid(*pv.puuid)) {
                    cid = *pv.puuid;
                    hasCid = true;
                }
            }
            PropVariantClear(&pv);
        }

        const bool isStereo = lower.find(L"0000110b") != Text::npos || lower.find(L"a2dp") != Text::npos;
        Text mac = extractMacFromFilterId(lower);
        result.push_back({std::move(epId), std::move(filterId), std::move(mac), cid, hasCid,
                          state == DEVICE_STATE_ACTIVE, flow == eRender, isStereo});
    }

    // Propagate MAC across A2DP <-> HFP (Render & Capture) endpoints sharing the same ContainerId
    for (auto& ep : result) {
        if (!ep.macLower.empty() || !ep.hasContainer) continue;
        for (const auto& other : result) {
            if (!other.macLower.empty() && other.hasContainer && IsEqualGUID(ep.containerId, other.containerId)) {
                ep.macLower = other.macLower;
                break;
            }
        }
    }
    return result;
}

bool endpointMatchesDevice(const BtAudioEndpoint& ep, const Text& macLower, const GUID& containerId, bool hasContainer) {
    if (hasContainer && ep.hasContainer && IsEqualGUID(ep.containerId, containerId)) return true;
    if (!macLower.empty() && !ep.macLower.empty() && ep.macLower == macLower) return true;
    if (!macLower.empty() && toLowerText(ep.filterId).find(macLower) != Text::npos) return true;
    return false;
}

bool fireBtKsOneShot(IMMDeviceEnumerator* enumerator, const std::vector<Text>& filterIds, bool connect) {
    bool anyOk = false;
    std::unordered_set<Text> seen;
    for (const auto& fid : filterIds) {
        if (fid.empty() || !seen.insert(fid).second) continue;
        ComPtr<IMMDevice> filterDev;
        if (FAILED(enumerator->GetDevice(fid.c_str(), &filterDev)) || !filterDev) continue;
        ComPtr<IKsControl> ks;
        if (FAILED(filterDev->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(ks.GetAddressOf()))) || !ks) continue;
        KSPROPERTY prop{};
        prop.Set = KSPROPSETID_BtAudio;
        prop.Id = connect ? KSPROPERTY_ONESHOT_RECONNECT : KSPROPERTY_ONESHOT_DISCONNECT;
        prop.Flags = KSPROPERTY_TYPE_GET;
        ULONG returned = 0;
        if (SUCCEEDED(ks->KsProperty(&prop, sizeof(prop), nullptr, 0, &returned))) anyOk = true;
    }
    return anyOk;
}

Text getDefaultRenderEndpointId(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)) && def) {
        LPWSTR id{};
        if (SUCCEEDED(def->GetId(&id)) && id) { Text out(id); CoTaskMemFree(id); return out; }
    }
    return {};
}
}

void Dock::queryRadios(int toggle) {
    if (extra.radioRequest.valid()) return;
    const bool prevAirplane = extra.radios.airplane;
    const bool prevWifi = extra.airplanePrevWifi;
    const bool prevBt = extra.airplanePrevBt;
    if (toggle == 3 && !prevAirplane) {
        extra.airplanePrevWifi = extra.radios.wifi != 0;
        extra.airplanePrevBt = extra.radios.bluetooth != 0;
    }
    try {
        AccessOperation permission{nullptr};
        if (toggle >= 1 && toggle <= 3) permission = Radio::RequestAccessAsync();
        const HWND notify = hwnd;
        extra.radioRequest = std::async(std::launch::async, [toggle, permission, prevAirplane, prevWifi, prevBt, notify]() {
            winrt::init_apartment();
            RadioStatus status;
            try {
                const auto radios = Radio::GetRadiosAsync().get();
                const bool canChange = (toggle >= 1 && toggle <= 3) && permission && permission.get() == RadioAccessStatus::Allowed;
                status.denied = (toggle >= 1 && toggle <= 3) && !canChange;
                bool changed = false;
                bool anyRadio = false;
                bool allOff = true;
                for (const auto& radio : radios) {
                    const auto kind = radio.Kind();
                    anyRadio = true;
                    if (canChange && toggle == 3) {
                        const bool enableAirplane = !prevAirplane;
                        RadioState desired = RadioState::Off;
                        if (!enableAirplane) {
                            if (kind == RadioKind::WiFi) desired = prevWifi ? RadioState::On : RadioState::Off;
                            else if (kind == RadioKind::Bluetooth) desired = prevBt ? RadioState::On : RadioState::Off;
                            else desired = RadioState::On;
                        }
                        if (radio.State() != desired) {
                            if (radio.SetStateAsync(desired).get() != RadioAccessStatus::Allowed) status.denied = true;
                        }
                    } else if (canChange && !changed && ((toggle == 1 && kind == RadioKind::WiFi) ||
                               (toggle == 2 && kind == RadioKind::Bluetooth))) {
                        const auto next = radio.State() == RadioState::On ? RadioState::Off : RadioState::On;
                        status.denied = radio.SetStateAsync(next).get() != RadioAccessStatus::Allowed;
                        changed = true;
                    }
                    const auto current = radio.State();
                    if (current == RadioState::On) allOff = false;
                    if (kind == RadioKind::WiFi)
                        status.wifi = current == RadioState::On ? 1 : current == RadioState::Off ? 0 : -1;
                    else if (kind == RadioKind::Bluetooth)
                        status.bluetooth = current == RadioState::On ? 1 : current == RadioState::Off ? 0 : -1;
                }
                const bool sysAirplane = readSystemAirplaneMode();
                status.airplane = sysAirplane || (anyRadio && allOff && (toggle == 3 ? !prevAirplane : prevAirplane));
            } catch (const winrt::hresult_error&) {
                status.denied = (toggle >= 1 && toggle <= 3);
                status.airplane = readSystemAirplaneMode();
            }
            try {
                if (const auto internet = NetworkInformation::GetInternetConnectionProfile()) {
                    const auto level = internet.GetNetworkConnectivityLevel();
                    if (level == NetworkConnectivityLevel::InternetAccess) status.hasInternet = true;
                    const auto adapter = internet.NetworkAdapter();
                    if (internet.IsWlanConnectionProfile() && status.network.empty()) {
                        status.network = internet.ProfileName().c_str();
                        if (adapter) status.adapter = adapter.NetworkAdapterId();
                    } else if (adapter && adapter.IanaInterfaceType() == 6 && level != NetworkConnectivityLevel::None) {
                        status.ethernet = true;
                    }
                }
                for (const auto& profile : NetworkInformation::GetConnectionProfiles()) {
                    const auto level = profile.GetNetworkConnectivityLevel();
                    if (level == NetworkConnectivityLevel::None) continue;
                    if (level == NetworkConnectivityLevel::InternetAccess) status.hasInternet = true;
                    const auto adapter = profile.NetworkAdapter();
                    if (profile.IsWlanConnectionProfile() && status.network.empty()) {
                        status.network = profile.ProfileName().c_str();
                        if (adapter) status.adapter = adapter.NetworkAdapterId();
                    } else if (adapter && adapter.IanaInterfaceType() == 6) status.ethernet = true;
                }
            } catch (const winrt::hresult_error&) {}
            try {
                if (const auto internet = NetworkInformation::GetInternetConnectionProfile()) {
                    if (const auto tethering = NetworkOperatorTetheringManager::CreateFromConnectionProfile(internet)) {
                        if (toggle == 4) {
                            const auto cur = tethering.TetheringOperationalState();
                            if (cur == TetheringOperationalState::On) {
                                const auto res = tethering.StopTetheringAsync().get();
                                if (res.Status() != TetheringOperationStatus::Success) status.denied = true;
                            } else if (cur == TetheringOperationalState::Off) {
                                const auto res = tethering.StartTetheringAsync().get();
                                if (res.Status() != TetheringOperationStatus::Success) status.denied = true;
                            }
                        }
                        const auto state = tethering.TetheringOperationalState();
                        status.hotspot = state == TetheringOperationalState::On ? 1 :
                            state == TetheringOperationalState::Off ? 0 : 2;
                        status.hotspotClients = static_cast<int>(tethering.ClientCount());
                    }
                }
            } catch (const winrt::hresult_error&) {
                if (toggle == 4) status.denied = true;
            }
            winrt::uninit_apartment();
            if (notify) PostMessageW(notify, AsyncWake, 0, 0);
            return status;
        });
    } catch (const std::exception&) {
        if (toggle) report(L"Windows bu kablosuz denetime izin vermedi.");
    }
    paintSheet();
}

void Dock::queryBluetoothDevices() {
    if (extra.btRequest.valid()) return;
    extra.btLoading = true;
    const HWND notify = hwnd;
    extra.btRequest = std::async(std::launch::async, [notify]() {
        winrt::init_apartment();
        std::vector<BluetoothDeviceInfo> devices;
        std::unordered_map<ULONGLONG, size_t> byAddr;

        // Kick off Classic & BLE discovery in parallel while scanning Core Audio in 1 pass!
        winrt::Windows::Foundation::IAsyncOperation<DeviceInformationCollection> classicOp{nullptr};
        winrt::Windows::Foundation::IAsyncOperation<DeviceInformationCollection> bleOp{nullptr};
        try { classicOp = DeviceInformation::FindAllAsync(BluetoothDevice::GetDeviceSelectorFromPairingState(true)); }
        catch (const winrt::hresult_error&) {}
        try { bleOp = DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)); }
        catch (const winrt::hresult_error&) {}

        // Single-pass Core Audio Bluetooth endpoint scan (~2ms total, overlaps with WinRT!)
        const auto allAudio = collectAllBtAudioEndpoints();

        const auto parseIdMac = [](const Text& id) -> ULONGLONG {
            // Classic/BLE ID format: "Bluetooth#Bluetooth<adapter>-<deviceMac>"
            const size_t dash = id.rfind(L'-');
            return parseMacString(dash != Text::npos ? id.substr(dash + 1) : id);
        };

        const auto addOrMerge = [&](BluetoothDeviceInfo info) {
            if (info.name.empty() || info.address == 0) return;
            auto it = byAddr.find(info.address);
            if (it == byAddr.end()) {
                byAddr[info.address] = devices.size();
                devices.push_back(std::move(info));
            } else {
                auto& existing = devices[it->second];
                if (existing.isAudio || info.isAudio) {
                    existing.isAudio = true;
                    if (!info.isBle) existing.connected = info.connected;
                } else {
                    existing.connected |= info.connected;
                }
                if (!existing.hasContainer && info.hasContainer) {
                    existing.containerId = info.containerId;
                    existing.hasContainer = true;
                }
                if (existing.isBle && !info.isBle) {
                    existing.isBle = false;
                    existing.id = info.id;
                }
                if (info.battery >= 0) existing.battery = info.battery;
            }
        };

        // Process Classic Bluetooth devices (audio devices resolved immediately from Core Audio;
        // non-audio devices query FromIdAsync in parallel so there is zero serial stall!)
        struct PendingClassic { BluetoothDeviceInfo info; winrt::Windows::Foundation::IAsyncOperation<BluetoothDevice> op{nullptr}; };
        std::vector<PendingClassic> pendingClassic;
        if (classicOp) {
            try {
                const auto found = classicOp.get();
                for (const auto& item : found) {
                    BluetoothDeviceInfo info;
                    info.id = item.Id().c_str();
                    info.name = item.Name().c_str();
                    info.address = parseIdMac(info.id);
                    info.isBle = false;

                    const Text mac = info.address ? macHexLower(info.address) : Text{};
                    bool hasAudioEp = false, anyRenderActive = false;
                    for (const auto& ep : allAudio) {
                        if (endpointMatchesDevice(ep, mac, info.containerId, info.hasContainer)) {
                            hasAudioEp = true;
                            if (!info.hasContainer && ep.hasContainer) {
                                info.containerId = ep.containerId;
                                info.hasContainer = true;
                            }
                            if (ep.isRender && ep.active) anyRenderActive = true;
                        }
                    }
                    if (hasAudioEp && info.address != 0) {
                        info.isAudio = true;
                        // Honest audio state: "Bağlı" only when playback audio is actually ACTIVE!
                        info.connected = anyRenderActive;
                        addOrMerge(std::move(info));
                    } else {
                        try {
                            pendingClassic.push_back({std::move(info), BluetoothDevice::FromIdAsync(item.Id())});
                        } catch (const winrt::hresult_error&) {}
                    }
                }
            } catch (const winrt::hresult_error&) {}
        }

        // Process BLE devices in parallel too (skip audio devices already matched on Classic!)
        struct PendingBle { BluetoothDeviceInfo info; winrt::Windows::Foundation::IAsyncOperation<BluetoothLEDevice> op{nullptr}; };
        std::vector<PendingBle> pendingBle;
        if (bleOp) {
            try {
                const auto found = bleOp.get();
                for (const auto& item : found) {
                    BluetoothDeviceInfo info;
                    info.id = item.Id().c_str();
                    info.name = item.Name().c_str();
                    info.address = parseIdMac(info.id);
                    info.isBle = true;
                    if (info.address != 0 && byAddr.count(info.address)) continue; // already have Classic audio entry
                    try {
                        pendingBle.push_back({std::move(info), BluetoothLEDevice::FromIdAsync(item.Id())});
                    } catch (const winrt::hresult_error&) {}
                }
            } catch (const winrt::hresult_error&) {}
        }

        for (auto& p : pendingClassic) {
            try {
                if (const auto bt = p.op.get()) {
                    if (p.info.address == 0) p.info.address = bt.BluetoothAddress();
                    p.info.connected = bt.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                    if (bt.ClassOfDevice().MajorClass() == BluetoothMajorClass::AudioVideo) p.info.isAudio = true;
                    addOrMerge(std::move(p.info));
                }
            } catch (const winrt::hresult_error&) {}
        }
        for (auto& p : pendingBle) {
            try {
                if (const auto ble = p.op.get()) {
                    if (p.info.address == 0) p.info.address = ble.BluetoothAddress();
                    p.info.connected = ble.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                    addOrMerge(std::move(p.info));
                }
            } catch (const winrt::hresult_error&) {}
        }

        std::stable_sort(devices.begin(), devices.end(), [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
            if (a.connected != b.connected) return a.connected;
            if (a.isAudio != b.isAudio) return a.isAudio;
            return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
        });

        winrt::uninit_apartment();
        if (notify) PostMessageW(notify, AsyncWake, 0, 0);
        return devices;
    });
    paintSheet();
}

void Dock::toggleBluetoothDevice(size_t index) {
    if (index >= extra.btDevices.size() || extra.btActionRequest.valid()) return;
    const auto dev = extra.btDevices[index];
    const bool wantConnect = !dev.connected;
    extra.btBusyAddress = dev.address;
    extra.btBusyConnect = wantConnect;
    const HWND notify = hwnd;
    extra.btActionRequest = std::async(std::launch::async, [dev, wantConnect, notify]() -> BtActionResult {
        winrt::init_apartment();
        BtActionResult res;
        res.address = dev.address;
        res.connected = dev.connected;

        const Text mac = macHexLower(dev.address);
        auto allAudio = collectAllBtAudioEndpoints();
        std::vector<BtAudioEndpoint> matched;
        for (const auto& ep : allAudio) {
            if (endpointMatchesDevice(ep, mac, dev.containerId, dev.hasContainer)) matched.push_back(ep);
        }

        if (dev.isAudio || !matched.empty()) {
            // Sort: A2DP Stereo render first, then HFP render, then HFP capture
            std::stable_sort(matched.begin(), matched.end(), [](const auto& a, const auto& b) {
                if (a.isRender != b.isRender) return a.isRender > b.isRender;
                return a.isStereo > b.isStereo;
            });

            ComPtr<IMMDeviceEnumerator> enumerator;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&enumerator))) && enumerator && !matched.empty()) {
                std::vector<Text> stereoFilters, allFilters;
                std::vector<std::pair<Text, ComPtr<IMMDevice>>> stereoRenderDevs, otherRenderDevs;
                for (const auto& ep : matched) {
                    if (ep.isStereo) stereoFilters.push_back(ep.filterId);
                    allFilters.push_back(ep.filterId);
                    if (ep.isRender) {
                        ComPtr<IMMDevice> mmDev;
                        if (SUCCEEDED(enumerator->GetDevice(ep.endpointId.c_str(), &mmDev)) && mmDev) {
                            if (ep.isStereo) stereoRenderDevs.push_back({ep.endpointId, mmDev});
                            else otherRenderDevs.push_back({ep.endpointId, mmDev});
                        }
                    }
                }

                const auto findActiveEndpoint = [](const std::vector<std::pair<Text, ComPtr<IMMDevice>>>& list) -> Text {
                    for (const auto& [id, d] : list) {
                        DWORD st = 0;
                        if (SUCCEEDED(d->GetState(&st)) && st == DEVICE_STATE_ACTIVE) return id;
                    }
                    return {};
                };

                if (wantConnect) {
                    Text activeId = findActiveEndpoint(stereoRenderDevs);
                    if (activeId.empty()) {
                        // Fire A2DP Stereo first, then HFP so full headset handshake completes quickly
                        bool sent = fireBtKsOneShot(enumerator.Get(), allFilters, true);
                        if (sent) {
                            const ULONGLONG start = GetTickCount64();
                            const ULONGLONG deadline = start + 6500;
                            bool retried = false;
                            while (GetTickCount64() < deadline) {
                                activeId = findActiveEndpoint(stereoRenderDevs);
                                if (!activeId.empty()) break;
                                // If device has no A2DP stereo endpoint at all, accept HFP render
                                if (stereoRenderDevs.empty()) {
                                    activeId = findActiveEndpoint(otherRenderDevs);
                                    if (!activeId.empty()) break;
                                }
                                if (!retried && GetTickCount64() - start >= 1800) {
                                    retried = true;
                                    fireBtKsOneShot(enumerator.Get(), stereoFilters.empty() ? allFilters : stereoFilters, true);
                                }
                                Sleep(60);
                            }
                            // Fallback: if A2DP didn't activate in time, check HFP render
                            if (activeId.empty()) activeId = findActiveEndpoint(otherRenderDevs);
                        }
                    }

                    if (!activeId.empty()) {
                        // Immediately route Windows default audio output to A2DP Stereo!
                        setDefaultAudioEndpointId(activeId);
                        // Brief guard: if Windows' own PnP audio router bounced default back to speakers/HFP, re-assert Stereo
                        Sleep(120);
                        if (getDefaultRenderEndpointId(enumerator.Get()) != activeId) {
                            setDefaultAudioEndpointId(activeId);
                        }
                        res.connected = true;
                        res.audioChanged = true;
                        res.message = dev.name + L" bağlandı.";
                    } else {
                        res.connected = false;
                        res.message = dev.name + L" bağlanamadı; kulaklığın açık ve yakın olduğundan emin ol.";
                    }
                } else {
                    // Disconnect ALL matching profiles (A2DP Stereo + HFP Render + HFP Capture)
                    const bool sent = fireBtKsOneShot(enumerator.Get(), allFilters, false);
                    if (sent) {
                        const ULONGLONG deadline = GetTickCount64() + 2500;
                        while (GetTickCount64() < deadline) {
                            if (findActiveEndpoint(stereoRenderDevs).empty() && findActiveEndpoint(otherRenderDevs).empty())
                                break;
                            Sleep(60);
                        }
                    }
                    const bool stillActive = !findActiveEndpoint(stereoRenderDevs).empty() ||
                                             !findActiveEndpoint(otherRenderDevs).empty();
                    res.connected = stillActive;
                    res.audioChanged = !stillActive;
                    res.message = !stillActive ? (dev.name + L" bağlantısı kesildi.")
                                               : (dev.name + L" bağlantısı kesilemedi.");
                }
            } else {
                res.message = dev.name + L" için Bluetooth ses uç noktası bulunamadı.";
            }
        } else if (wantConnect) {
            bool ok = false;
            try {
                if (!dev.isBle) {
                    if (const auto bt = BluetoothDevice::FromBluetoothAddressAsync(dev.address).get()) {
                        const auto services = bt.GetRfcommServicesAsync(BluetoothCacheMode::Uncached).get();
                        ok = services.Error() == BluetoothError::Success ||
                            bt.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                    }
                } else {
                    if (const auto ble = BluetoothLEDevice::FromBluetoothAddressAsync(dev.address).get()) {
                        const auto services = ble.GetGattServicesAsync(BluetoothCacheMode::Uncached).get();
                        ok = services.Status() == winrt::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus::Success ||
                            ble.ConnectionStatus() == BluetoothConnectionStatus::Connected;
                    }
                }
            } catch (const winrt::hresult_error&) {}
            res.connected = ok;
            res.message = ok ? (dev.name + L" bağlandı.")
                             : (dev.name + L" uykuda olabilir; aygıttan uyandırın veya ayarlardan bağlayın.");
        } else {
            res.message = L"Bu Bluetooth aygıtının bağlantısını aygıt üzerinden veya ayarlardan kesin.";
        }

        winrt::uninit_apartment();
        if (notify) PostMessageW(notify, AsyncWake, 0, 0);
        return res;
    });
    paintSheet();
}
