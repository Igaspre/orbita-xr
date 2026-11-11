#include "monitors.h"
#include <algorithm>
#include <map>
#include <set>

namespace ox {

std::wstring Monitor::label() const {
    std::wstring g = gdi;
    size_t p = g.find(L"DISPLAY");
    if (p != std::wstring::npos) g = g.substr(p);
    std::wstring tags;
    if (primary) tags += tr(L", principal", L", main");
    if (mirrorsPrimary) tags += tr(L", duplicada", L", duplicate");
    if (isVirtual) tags += L", virtual";
    return wfmt(L"%ls (%dx%d, %d Hz%ls) %ls", name.empty() ? tr(L"Pantalla", L"Screen") : name.c_str(), width, height,
                refresh, tags.c_str(), g.c_str());
}

static std::wstring pnpId(UINT16 raw) {
    UINT16 v = (UINT16)((raw >> 8) | (raw << 8));  // stored big endian
    wchar_t s[4] = {(wchar_t)(((v >> 10) & 31) + 64), (wchar_t)(((v >> 5) & 31) + 64), (wchar_t)((v & 31) + 64), 0};
    return s;
}

static bool queryConfig(UINT32 flags, std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                        std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    for (int tries = 0; tries < 5; tries++) {
        UINT32 np = 0, nm = 0;
        if (GetDisplayConfigBufferSizes(flags, &np, &nm) != ERROR_SUCCESS) return false;
        paths.resize(np);
        modes.resize(nm);
        LONG rc = QueryDisplayConfig(flags, &np, paths.data(), &nm, modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            paths.resize(np);
            modes.resize(nm);
            return true;
        }
        if (rc != ERROR_INSUFFICIENT_BUFFER) return false;
    }
    return false;
}

static DISPLAYCONFIG_TARGET_DEVICE_NAME targetName(const LUID& adapter, UINT32 id) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
    tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    tn.header.size = sizeof(tn);
    tn.header.adapterId = adapter;
    tn.header.id = id;
    DisplayConfigGetDeviceInfo(&tn.header);
    return tn;
}

static bool isDriverScreen(const DISPLAYCONFIG_TARGET_DEVICE_NAME& tn) {
    std::wstring path = tn.monitorDevicePath;
    return icontains(path, L"MTT1337") || icontains(tn.monitorFriendlyDeviceName, L"VDD by MTT") ||
           (tn.flags.edidIdsValid && pnpId(tn.edidManufactureId) == L"MTT");
}

std::vector<Monitor> listMonitors() {
    std::vector<Monitor> out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!queryConfig(QDC_ONLY_ACTIVE_PATHS, paths, modes)) return out;

    // Ask GDI which one is primary rather than trusting the (0,0) position.
    std::wstring primaryGdi;
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof(dd))
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) primaryGdi = dd.DeviceName;

    std::map<std::wstring, size_t> seen;
    for (auto& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sn = {};
        sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sn.header.size = sizeof(sn);
        sn.header.adapterId = p.sourceInfo.adapterId;
        sn.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS) continue;
        auto tn = targetName(p.targetInfo.adapterId, p.targetInfo.id);

        Monitor m;
        m.gdi = sn.viewGdiDeviceName;
        m.name = tn.monitorFriendlyDeviceName;
        m.path = tn.monitorDevicePath;
        if (tn.flags.edidIdsValid) m.edidMfg = pnpId(tn.edidManufactureId);
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsExW(m.gdi.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) {
            m.rect = {dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + (LONG)dm.dmPelsWidth,
                      dm.dmPosition.y + (LONG)dm.dmPelsHeight};
            m.width = (int)dm.dmPelsWidth;
            m.height = (int)dm.dmPelsHeight;
            m.refresh = (int)dm.dmDisplayFrequency;
            m.orientation = (int)dm.dmDisplayOrientation;
        }
        m.primary = primaryGdi.empty() ? (m.rect.left == 0 && m.rect.top == 0) : _wcsicmp(m.gdi.c_str(), primaryGdi.c_str()) == 0;
        m.isVirtual = isDriverScreen(tn);
        m.otherVirtual = !m.isVirtual && (icontains(m.name, L"virtual") || icontains(m.path, L"Virtual") ||
                                          icontains(m.path, L"PSCCDD"));
        m.looksLikeGlasses = icontains(m.name, L"XREAL") || icontains(m.name, L"NREAL") ||
                             icontains(m.path, L"XREAL") || icontains(m.path, L"NREAL");

        // two targets on one source = mirroring (typically the glasses cloning the laptop)
        auto it = seen.find(m.gdi);
        if (it != seen.end()) {
            m.mirrorsPrimary = out[it->second].primary;
            m.primary = false;
        } else {
            seen[m.gdi] = out.size();
        }
        out.push_back(m);
    }
    return out;
}

float guessFov(const std::wstring& n) {
    if (icontains(n, L"One Pro") || icontains(n, L"1 Pro")) return 57;
    if (icontains(n, L"1S") || icontains(n, L"One S")) return 52;
    if (icontains(n, L"Ultra")) return 52;
    if (icontains(n, L"One")) return 50;
    if (icontains(n, L"Air")) return 46;
    return 52;
}

const Monitor* findGlasses(const std::vector<Monitor>& mons, const std::wstring& manual, bool guess) {
    if (!manual.empty())
        for (auto& m : mons)
            if (!m.primary && (_wcsicmp(m.gdi.c_str(), manual.c_str()) == 0 || _wcsicmp(m.name.c_str(), manual.c_str()) == 0))
                return &m;
    for (auto& m : mons)
        if (m.looksLikeGlasses) return &m;
    if (!guess) return nullptr;
    const Monitor* found = nullptr;
    int count = 0;
    for (auto& m : mons) {
        if (m.primary || m.isVirtual || m.otherVirtual) continue;
        bool glassesMode = (m.width == 1920 && (m.height == 1080 || m.height == 1200)) ||
                           (m.width == 3840 && (m.height == 1080 || m.height == 1200));
        if (glassesMode) {
            found = &m;
            count++;
        }
    }
    return count == 1 ? found : nullptr;
}

bool mirrorsPrimary(const std::vector<Monitor>& mons) {
    for (auto& m : mons)
        if (m.mirrorsPrimary) return true;
    return false;
}

bool extendDesktop() {
    LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
    logf("extend desktop: %ld", rc);
    return rc == ERROR_SUCCESS;
}

bool mirrorDesktop() {
    LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_CLONE);
    logf("mirror desktop: %ld", rc);
    return rc == ERROR_SUCCESS;
}

int applyModes(const std::vector<ModeChange>& changes, std::wstring* warnings) {
    int done = 0;
    for (auto& c : changes) {
        DEVMODEW cur = {};
        cur.dmSize = sizeof(cur);
        if (!EnumDisplaySettingsExW(c.gdi.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0)) {
            logf("mode %s: can't read its current mode", narrow(c.gdi).c_str());
            continue;
        }
        DEVMODEW dm = cur;
        dm.dmFields = 0;
        int ori = c.orientation >= 0 ? c.orientation : (int)cur.dmDisplayOrientation;
        bool portrait = ori == DMDO_90 || ori == DMDO_270;
        if (c.width > 0 && c.height > 0) {
            dm.dmPelsWidth = (DWORD)(portrait ? c.height : c.width);
            dm.dmPelsHeight = (DWORD)(portrait ? c.width : c.height);
            dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
        }
        if (c.orientation >= 0 && c.orientation != (int)cur.dmDisplayOrientation) {
            bool wasPortrait = cur.dmDisplayOrientation == DMDO_90 || cur.dmDisplayOrientation == DMDO_270;
            if (!(dm.dmFields & DM_PELSWIDTH) && wasPortrait != portrait) {
                std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
                dm.dmFields |= DM_PELSWIDTH | DM_PELSHEIGHT;
            }
            dm.dmDisplayOrientation = (DWORD)c.orientation;
            dm.dmFields |= DM_DISPLAYORIENTATION;
        }
        if (c.refresh > 0) {
            dm.dmDisplayFrequency = (DWORD)c.refresh;
            dm.dmFields |= DM_DISPLAYFREQUENCY;
        }
        if (c.move) {
            dm.dmPosition = {c.x, c.y};
            dm.dmFields |= DM_POSITION;
        }
        if (!dm.dmFields) { done++; continue; }

        LONG t = ChangeDisplaySettingsExW(c.gdi.c_str(), &dm, nullptr, CDS_TEST, nullptr);
        if (t != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_DISPLAYFREQUENCY)) {
            dm.dmFields &= ~DM_DISPLAYFREQUENCY;
            t = ChangeDisplaySettingsExW(c.gdi.c_str(), &dm, nullptr, CDS_TEST, nullptr);
        }
        if (t != DISP_CHANGE_SUCCESSFUL && (dm.dmFields & DM_PELSWIDTH)) {
            if (warnings)
                *warnings += wfmt(tr(L"%ls no admite %dx%d, se queda en %ux%u.\n", L"%ls doesn't support %dx%d, staying at %ux%u.\n"),
                                  c.gdi.c_str(), c.width, c.height, (unsigned)cur.dmPelsWidth, (unsigned)cur.dmPelsHeight);
            dm.dmFields &= ~(DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYORIENTATION);
            dm.dmPelsWidth = cur.dmPelsWidth;
            dm.dmPelsHeight = cur.dmPelsHeight;
        }
        if (!dm.dmFields) { done++; continue; }
        LONG r = ChangeDisplaySettingsExW(c.gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        logf("mode %s %lux%lu@%lu pos %ld,%ld fields %lx -> %ld", narrow(c.gdi).c_str(), dm.dmPelsWidth,
             dm.dmPelsHeight, dm.dmDisplayFrequency, dm.dmPosition.x, dm.dmPosition.y, dm.dmFields, r);
        if (r == DISP_CHANGE_SUCCESSFUL) done++;
    }
    ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    return done;
}

// ---- driver screens -------------------------------------------------------
//
// The driver always exposes a fixed number of screens (we install it with 6).
// Instead of reloading it to change the count, we connect/disconnect its targets
// with the CCD API, which needs no admin rights.

namespace {

struct TargetKey {
    LUID adapter;
    UINT32 id;
    bool operator<(const TargetKey& o) const {
        if (adapter.HighPart != o.adapter.HighPart) return adapter.HighPart < o.adapter.HighPart;
        if (adapter.LowPart != o.adapter.LowPart) return adapter.LowPart < o.adapter.LowPart;
        return id < o.id;
    }
    bool operator==(const TargetKey& o) const {
        return adapter.HighPart == o.adapter.HighPart && adapter.LowPart == o.adapter.LowPart && id == o.id;
    }
};

TargetKey targetOf(const DISPLAYCONFIG_PATH_INFO& p) { return {p.targetInfo.adapterId, p.targetInfo.id}; }

std::vector<TargetKey> driverTargets(const std::vector<DISPLAYCONFIG_PATH_INFO>& all) {
    std::set<TargetKey> keys;
    std::map<TargetKey, bool> cache;
    for (auto& p : all) {
        if (!p.targetInfo.targetAvailable) continue;
        TargetKey k = targetOf(p);
        auto it = cache.find(k);
        if (it == cache.end()) it = cache.emplace(k, isDriverScreen(targetName(k.adapter, k.id))).first;
        if (it->second) keys.insert(k);
    }
    return {keys.begin(), keys.end()};
}

} // namespace

int virtualScreensAvailable() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!queryConfig(QDC_ALL_PATHS, paths, modes)) return 0;
    return (int)driverTargets(paths).size();
}

int virtualScreensActive() {
    int n = 0;
    for (auto& m : listMonitors())
        if (m.isVirtual && !m.mirrorsPrimary) n++;
    return n;
}

bool setVirtualScreens(int count) {
    std::vector<DISPLAYCONFIG_PATH_INFO> all, active;
    std::vector<DISPLAYCONFIG_MODE_INFO> allModes, modes;
    if (!queryConfig(QDC_ALL_PATHS, all, allModes) || !queryConfig(QDC_ONLY_ACTIVE_PATHS, active, modes)) return false;
    std::vector<TargetKey> targets = driverTargets(all);
    if ((int)targets.size() < count) count = (int)targets.size();
    std::vector<TargetKey> wanted(targets.begin(), targets.begin() + count);
    auto isWanted = [&](const TargetKey& k) { return std::find(wanted.begin(), wanted.end(), k) != wanted.end(); };
    auto isDriver = [&](const TargetKey& k) { return std::find(targets.begin(), targets.end(), k) != targets.end(); };

    // keep every active path except the driver screens we no longer want
    std::vector<DISPLAYCONFIG_PATH_INFO> next;
    std::set<TargetKey> present;
    std::map<std::pair<LONG, DWORD>, std::set<UINT32>> usedSources;
    for (auto& p : active) {
        TargetKey k = targetOf(p);
        if (isDriver(k) && !isWanted(k)) continue;
        next.push_back(p);
        present.insert(k);
        usedSources[{p.sourceInfo.adapterId.HighPart, p.sourceInfo.adapterId.LowPart}].insert(p.sourceInfo.id);
    }
    // add the missing ones on a free source of their adapter
    for (auto& k : wanted) {
        if (present.count(k)) continue;
        for (auto& p : all) {
            if (!(targetOf(p) == k)) continue;
            auto& used = usedSources[{p.sourceInfo.adapterId.HighPart, p.sourceInfo.adapterId.LowPart}];
            if (used.count(p.sourceInfo.id)) continue;
            DISPLAYCONFIG_PATH_INFO np = p;
            np.flags = DISPLAYCONFIG_PATH_ACTIVE;
            np.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            np.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            next.push_back(np);
            used.insert(p.sourceInfo.id);
            present.insert(k);
            break;
        }
    }
    if (next.size() == active.size() && (int)std::count_if(active.begin(), active.end(), [&](auto& p) {
            return isDriver(targetOf(p));
        }) == count)
        return true;  // nothing to do

    // only pass the modes the remaining paths still point at
    std::vector<DISPLAYCONFIG_MODE_INFO> outModes;
    auto remap = [&](UINT32& idx) {
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size()) {
            idx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            return;
        }
        outModes.push_back(modes[idx]);
        idx = (UINT32)(outModes.size() - 1);
    };
    for (auto& p : next) {
        if (p.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            p.targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
            continue;
        remap(p.sourceInfo.modeInfoIdx);
        remap(p.targetInfo.modeInfoIdx);
    }
    LONG rc = SetDisplayConfig((UINT32)next.size(), next.data(), (UINT32)outModes.size(), outModes.data(),
                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);
    logf("virtual screens -> %d of %d: SetDisplayConfig %ld", count, (int)targets.size(), rc);
    if (rc == ERROR_SUCCESS) return true;

    // Old school fallback, only for "disconnect all": a zero sized mode detaches a screen.
    if (count == 0) {
        for (auto& m : listMonitors()) {
            if (!m.isVirtual) continue;
            DEVMODEW dm = {};
            dm.dmSize = sizeof(dm);
            dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
            ChangeDisplaySettingsExW(m.gdi.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        }
        ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
        return virtualScreensActive() == 0;
    }
    return false;
}

} // namespace ox
