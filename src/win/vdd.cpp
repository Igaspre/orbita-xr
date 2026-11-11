#include "vdd.h"
#include "res/resource.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <newdev.h>
#include <shellapi.h>
#include <cstdio>
#include <regex>

namespace ox::vdd {

static const wchar_t* kHwid = L"Root\\MttVDD";
static const wchar_t* kPipe = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";
static const wchar_t* kRegKey = L"SOFTWARE\\MikeTheTech\\VirtualDisplayDriver";
static const char* kMarker = "<!-- orbita-xr -->";
static const GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

static std::wstring configDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRegKey, L"VDDPATH", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS && buf[0])
        return buf;
    return L"C:\\VirtualDisplayDriver";
}

static std::string readFile(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static bool writeFile(const std::wstring& path, const void* data, size_t size) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

static bool hasHwid(HDEVINFO set, SP_DEVINFO_DATA& dd) {
    wchar_t buf[2048] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &dd, SPDRP_HARDWAREID, nullptr, (BYTE*)buf, sizeof(buf) - 4, nullptr))
        return false;
    for (const wchar_t* p = buf; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kHwid) == 0) return true;
    return false;
}

State state() {
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return State::Missing;
    State st = State::Missing;
    SP_DEVINFO_DATA dd = {sizeof(dd)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        if (!hasHwid(set, dd)) continue;
        ULONG status = 0, problem = 0;
        bool running = CM_Get_DevNode_Status(&status, &problem, dd.DevInst, 0) == CR_SUCCESS &&
                       (status & DN_STARTED) && !(status & DN_HAS_PROBLEM);
        st = running ? State::Ready : State::Broken;
        break;
    }
    SetupDiDestroyDeviceInfoList(set);
    return st;
}

bool ours() { return readFile(configDir() + L"\\vdd_settings.xml").find(kMarker) != std::string::npos; }

int configuredCount() {
    std::string s = readFile(configDir() + L"\\vdd_settings.xml");
    size_t a = s.find("<count>");
    return a == std::string::npos ? -1 : atoi(s.c_str() + a + 7);
}

static bool pipeSend(const std::wstring& cmd) {
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 20 && h == INVALID_HANDLE_VALUE; i++) {
        h = CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipe, 500);
            else Sleep(150);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        logf("vdd: pipe not available (%lu)", GetLastError());
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
    DWORD written = 0;
    BOOL ok = WriteFile(h, cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(h);
    logf("vdd: %s -> %d", narrow(cmd).c_str(), ok);
    return ok != 0;
}

bool setCount(int n) { return pipeSend(wfmt(L"SETDISPLAYCOUNT %d", n)); }

std::wstring configuredGpu() {
    std::string s = readFile(configDir() + L"\\vdd_settings.xml");
    size_t a = s.find("<friendlyname>");
    size_t b = a == std::string::npos ? a : s.find("</friendlyname>", a);
    if (b == std::string::npos) return L"default";
    std::string v = s.substr(a + 14, b - a - 14);
    while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && isspace((unsigned char)v.front())) v.erase(v.begin());
    return v.empty() ? L"default" : widen(v);
}

bool setGpu(const std::wstring& name) {
    // The driver rewrites its own config and restarts its adapter on the new GPU.
    bool sent = pipeSend(L"SETGPU \"" + name + L"\"");
    for (int i = 0; i < 10 && _wcsicmp(configuredGpu().c_str(), name.c_str()) != 0; i++) Sleep(200);
    if (_wcsicmp(configuredGpu().c_str(), name.c_str()) == 0) return sent;
    // Didn't take: edit the file ourselves and ask for a reload.
    std::wstring path = configDir() + L"\\vdd_settings.xml";
    std::string s = readFile(path);
    size_t a = s.find("<friendlyname>");
    size_t b = a == std::string::npos ? a : s.find("</friendlyname>", a);
    if (b == std::string::npos) return false;
    s.replace(a + 14, b - a - 14, narrow(name));
    if (!writeFile(path, s.data(), s.size())) {
        logf("vdd: can't write %s", narrow(path).c_str());
        return false;
    }
    return pipeSend(L"RELOAD_DRIVER");
}

// ---- install / uninstall (elevated) ---------------------------------------

static bool resourceBytes(int id, const void** data, DWORD* size) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return false;
    HGLOBAL g = LoadResource(nullptr, r);
    *data = g ? LockResource(g) : nullptr;
    *size = SizeofResource(nullptr, r);
    return *data && *size;
}

// The bundled config with our resolution list, 6 screens and our marker.
static std::string makeConfig() {
    const void* p;
    DWORD n;
    if (!resourceBytes(IDR_VDD_XML, &p, &n)) return {};
    std::string xml((const char*)p, n);
    xml = std::regex_replace(xml, std::regex("<count>\\s*\\d+\\s*</count>"), "<count>" + std::to_string(kScreens) + "</count>");
    static const int res[][2] = {{1280, 720},  {1600, 900},  {1920, 1080}, {1920, 1200}, {2560, 1080},
                                 {2560, 1440}, {2560, 1600}, {3440, 1440}, {3840, 2160}};
    std::string list = "<resolutions>\n";
    for (auto& r : res)
        list += "        <resolution><width>" + std::to_string(r[0]) + "</width><height>" + std::to_string(r[1]) +
                "</height><refresh_rate>60</refresh_rate></resolution>\n";
    list += "    </resolutions>";
    size_t a = xml.find("<resolutions>"), b = xml.find("</resolutions>");
    if (a != std::string::npos && b != std::string::npos && b > a) xml.replace(a, b + 14 - a, list);
    size_t root = xml.find("<vdd_settings>");
    if (root != std::string::npos) xml.insert(root + 14, std::string("\n    ") + kMarker);
    return xml;
}

typedef BOOL(WINAPI* UpdateDriverFn)(HWND, LPCWSTR, LPCWSTR, DWORD, PBOOL);

DWORD install() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + wfmt(L"orbita-vdd-%lu", GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    const struct { int id; const wchar_t* name; } files[] = {
        {IDR_VDD_DLL, L"MttVDD.dll"}, {IDR_VDD_INF, L"MttVDD.inf"}, {IDR_VDD_CAT, L"mttvdd.cat"}};
    for (auto& f : files) {
        const void* p;
        DWORD n;
        if (!resourceBytes(f.id, &p, &n) || !writeFile(dir + L"\\" + f.name, p, n)) return ERROR_FILE_NOT_FOUND;
    }
    std::wstring inf = dir + L"\\MttVDD.inf";
    auto cleanup = [&] {
        for (auto& f : files) DeleteFileW((dir + L"\\" + f.name).c_str());
        RemoveDirectoryW(dir.c_str());
    };

    // the driver reads its config from here
    std::wstring cfg = L"C:\\VirtualDisplayDriver";
    CreateDirectoryW(cfg.c_str(), nullptr);
    std::string xml = makeConfig();
    if (xml.empty() || !writeFile(cfg + L"\\vdd_settings.xml", xml.data(), xml.size())) {
        cleanup();
        return ERROR_WRITE_FAULT;
    }
    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"VDDPATH", 0, REG_SZ, (const BYTE*)cfg.c_str(), (DWORD)((cfg.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
    }

    // root enumerated device node, like `devcon install`
    bool created = false;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA dd = {sizeof(dd)};
    bool exists = false;
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &dd); i++)
        if (hasHwid(set, dd)) { exists = true; break; }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);

    HDEVINFO newSet = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA newDev = {sizeof(newDev)};
    if (!exists) {
        GUID cls;
        wchar_t clsName[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(inf.c_str(), &cls, clsName, MAX_CLASS_NAME_LEN, nullptr)) {
            DWORD e = GetLastError();
            cleanup();
            return e;
        }
        newSet = SetupDiCreateDeviceInfoList(&cls, nullptr);
        static const wchar_t hwid[] = L"Root\\MttVDD\0";
        if (newSet == INVALID_HANDLE_VALUE ||
            !SetupDiCreateDeviceInfoW(newSet, clsName, &cls, nullptr, nullptr, DICD_GENERATE_ID, &newDev) ||
            !SetupDiSetDeviceRegistryPropertyW(newSet, &newDev, SPDRP_HARDWAREID, (const BYTE*)hwid, sizeof(hwid)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, newSet, &newDev)) {
            DWORD e = GetLastError();
            if (newSet != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(newSet);
            cleanup();
            return e ? e : ERROR_GEN_FAILURE;
        }
        created = true;
    }

    // Windows shows its own "install this device software?" prompt here
    HMODULE nd = LoadLibraryW(L"newdev.dll");
    auto update = nd ? (UpdateDriverFn)GetProcAddress(nd, "UpdateDriverForPlugAndPlayDevicesW") : nullptr;
    BOOL reboot = FALSE;
    DWORD err = ERROR_SUCCESS;
    if (!update || !update(nullptr, kHwid, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) err = update ? GetLastError() : ERROR_PROC_NOT_FOUND;
    logf("vdd install: created=%d err=%lu reboot=%d", created, err, reboot);

    if (err != ERROR_SUCCESS && created) SetupDiCallClassInstaller(DIF_REMOVE, newSet, &newDev);
    if (newSet != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(newSet);
    if (nd) FreeLibrary(nd);
    cleanup();
    if (err == ERROR_SUCCESS && reboot) return ERROR_SUCCESS_REBOOT_REQUIRED;
    return err;
}

DWORD uninstall() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    DWORD err = ERROR_SUCCESS;
    std::vector<std::wstring> infs;
    SP_DEVINFO_DATA dd = {sizeof(dd)};
    for (DWORD i = 0; set != INVALID_HANDLE_VALUE && SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        if (!hasHwid(set, dd)) continue;
        HKEY k = SetupDiOpenDevRegKey(set, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (k != INVALID_HANDLE_VALUE) {
            wchar_t inf[MAX_PATH] = {};
            DWORD sz = sizeof(inf);
            if (RegQueryValueExW(k, L"InfPath", nullptr, nullptr, (BYTE*)inf, &sz) == ERROR_SUCCESS) infs.push_back(inf);
            RegCloseKey(k);
        }
        // same as `devcon remove`; the device has no children so DIF_REMOVE is enough
        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &dd)) err = GetLastError();
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    for (auto& inf : infs)
        if (_wcsnicmp(inf.c_str(), L"oem", 3) == 0) SetupUninstallOEMInfW(inf.c_str(), SUOI_FORCEDELETE, nullptr);

    std::wstring cfg = configDir();
    DeleteFileW((cfg + L"\\vdd_settings.xml").c_str());
    RemoveDirectoryW(cfg.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kRegKey);
    logf("vdd uninstall: %lu (%d packages)", err, (int)infs.size());
    return err;
}

DWORD runElevated(const wchar_t* arg) {
    std::wstring exe = exePath();
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = arg;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return GetLastError();
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

} // namespace ox::vdd
