#pragma once
#include <string>

/*
 * 简易日志：每次启动清空重写，落到 %LOCALAPPDATA%\KugouPlayer\floral-log.txt
 *
 * 格式：[时分秒.毫秒] [模块] 内容
 * 模块：APP / UI / SRC / NET / AUD / TOG / ERR
 */
void LogInit();                                   // 清空文件并写启动头
void Logf(const char* tag, const char* fmt, ...); // printf 风格
void LogW(const char* tag, const wchar_t* msg);   // 宽字符一行
std::wstring LogPath();
std::string  Narrow(const std::wstring& w);                           // 日志文件完整路径
