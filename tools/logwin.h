/*
 * Console and log-file output for the relay tools.
 *
 * Source strings are UTF-8. Writing them straight to the console garbles them on a GBK system, so the
 * text is converted to wide characters for the console, and the log file is opened as UTF-8 with a BOM
 * so Notepad and PowerShell decode it correctly too.
 */
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

namespace logwin {

inline std::wstring ToWide(const char* utf8, int len) {
    if (len <= 0) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, &w[0], n);
    return w;
}

/* Prints one line; uses the wide console API when there is a console attached */
inline void Console(const std::string& utf8Line) {
    const std::wstring w = ToWide(utf8Line.c_str(), (int)utf8Line.size());
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        DWORD wrote = 0;
        std::wstring line = w + L"\n";
        WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &wrote, nullptr);
        return;
    }
    // Redirected to a file or a pipe: keep the bytes as UTF-8
    fwrite(utf8Line.data(), 1, utf8Line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/* Appends one line; adds a BOM when the file is new so readers pick UTF-8 */
inline void File(const std::string& path, const std::string& utf8Line) {
    if (path.empty()) return;
    bool needBom = false;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) needBom = true;
    else if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0) needBom = true;

    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "ab") == 0 && f) {
        if (needBom) fwrite("\xEF\xBB\xBF", 1, 3, f);
        fwrite(utf8Line.data(), 1, utf8Line.size(), f);
        fputc('\n', f);
        fclose(f);
    }
}

}  // namespace logwin
