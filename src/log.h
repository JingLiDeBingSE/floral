#pragma once
#include <string>

/*
 * Minimal logger: truncated and rewritten on start, at %LOCALAPPDATA%\KugouPlayer\floral-log.txt
 *
 * Format: [hh:mm:ss.mmm] [module] message
 * Modules: APP / UI / SRC / NET / AUD / TOG / ERR
 */
void LogInit();                                   // clear file and write startup header
void Logf(const char* tag, const char* fmt, ...); // printf-style
void LogW(const char* tag, const wchar_t* msg);   // single wide-char line
std::wstring LogPath();
std::string  Narrow(const std::wstring& w);                           // full path of the log file
