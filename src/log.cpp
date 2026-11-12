#include "log.h"

#include <windows.h>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <mutex>
#include <vector>

namespace {

FILE*        g_file = nullptr;
std::mutex   g_mx;
std::wstring g_path;

std::wstring LocalDir() {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n ? std::wstring(buf, n) : L".") + L"\\KugouPlayer";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteRaw(const char* tag, const char* msg) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] [%-4s] %s\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag, msg);
    fflush(g_file);
}

}  // namespace

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring LogPath() {
    if (g_path.empty()) g_path = LocalDir() + L"\\floral-log.txt";
    return g_path;
}

void LogInit() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    g_file = _wfsopen(LogPath().c_str(), L"wb", _SH_DENYWR);   // allow other programs to read
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "Floral 日志  %04d-%02d-%02d %02d:%02d:%02d\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(g_file, "=====================================================\n");
    fflush(g_file);
}

void Logf(const char* tag, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    WriteRaw(tag, buf);
}

void LogW(const char* tag, const wchar_t* msg) {
    std::string s = Narrow(msg ? msg : L"");
    WriteRaw(tag, s.c_str());
}
