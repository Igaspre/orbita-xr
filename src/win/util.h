#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>

#define OX_VERSION "1.5.0"
#define OX_VERSION_W L"1.5.0"

namespace ox {

// minimal COM smart pointer (avoids pulling in WRL)
template <class T>
class Com {
public:
    Com() = default;
    explicit Com(T* p) : p_(p) {}
    Com(const Com& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { reset(); }
    Com& operator=(Com o) noexcept { std::swap(p_, o.p_); return *this; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T** put() { reset(); return &p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    template <class U>
    HRESULT as(Com<U>& out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), (void**)out.put()) : E_POINTER;
    }
    static Com addRef(T* p) { if (p) p->AddRef(); return Com(p); }

private:
    T* p_ = nullptr;
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::wstring wfmt(const wchar_t* fmt, ...);
std::string hex(HRESULT hr);
std::wstring errorText(DWORD code);
bool icontains(const std::wstring& hay, const std::wstring& needle);

std::wstring dataDir();  // %APPDATA%\OrbitaXR
std::wstring exePath();

// Interface language, Spanish or English. It starts as the Windows language and
// can be changed in Shortcuts > Language.
bool english();
void setEnglish(bool on);
bool windowsIsSpanish();
inline const wchar_t* tr(const wchar_t* es, const wchar_t* en) { return english() ? en : es; }

void logOpen();
void logf(const char* fmt, ...);

} // namespace ox
