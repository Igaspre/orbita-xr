#include "util.h"
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <mutex>

namespace ox {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring wfmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = 0;
    return buf;
}

std::string hex(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lX", (unsigned long)hr);
    return b;
}

std::wstring errorText(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s.empty() ? wfmt(L"error 0x%08X", (unsigned)code) : s + wfmt(L" (0x%08X)", (unsigned)code);
}

bool icontains(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return true;
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](wchar_t a, wchar_t b) {
               return std::towlower(a) == std::towlower(b);
           }) != hay.end();
}

std::wstring dataDir() {
    wchar_t path[MAX_PATH] = {};
    std::wstring dir = SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path)) ? path : L".";
    dir += L"\\OrbitaXR";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring exePath() {
    wchar_t p[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH * 2);
    return p;
}

static std::atomic<bool> g_english{false};  // read from the render and worker threads too
bool english() { return g_english; }
void setEnglish(bool on) { g_english = on; }
bool windowsIsSpanish() { return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH; }

static std::mutex g_logLock;
static FILE* g_log = nullptr;

void logOpen() {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_log) return;
    std::wstring p = dataDir() + L"\\log.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 2 * 1024 * 1024)
        MoveFileExW(p.c_str(), (dataDir() + L"\\log-old.txt").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfopen(p.c_str(), L"ab");
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_log, "\r\n--- %04d-%02d-%02d %02d:%02d:%02d  orbita-xr %s ---\r\n", t.wYear, t.wMonth, t.wDay,
                     t.wHour, t.wMinute, t.wSecond, OX_VERSION);
        std::fflush(g_log);
    }
}

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_logLock);
    if (g_log) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        std::fprintf(g_log, "%02d:%02d:%02d.%03d %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
        std::fflush(g_log);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

} // namespace ox
