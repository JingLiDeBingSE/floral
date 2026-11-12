#pragma once
#include <string>

/* 用 Windows MCI 播放音频：本地文件或 http 地址都行（是否支持取决于系统解码器）。
   全链路都是宽字符，返回值都带错误码，方便在 UI 上提示。 */

bool AudioOpen(const std::wstring& path);   // 打开并准备播放
void AudioPlay();
void AudioPause();
void AudioStop();
void AudioClose();

void AudioSeekMS(int ms);
void AudioSetVolume(int v);                 // 0..1000
int  AudioPosMS();                          // -1 表示不可用
int  AudioLenMS();
bool AudioIsOpen();
bool AudioIsPlaying();
bool AudioIsSeeking();                      // 后台跳转中（UI 用这个显示加载）

std::wstring AudioLastError();
