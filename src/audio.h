#pragma once
#include <string>

/* Play audio via Windows MCI: local files or http URLs (support depends on system codecs).
   Everything is wide-char, and all return values carry error codes for UI prompts. */

bool AudioOpen(const std::wstring& path);   // Open and prepare for playback
void AudioPlay();
void AudioPause();
void AudioStop();
void AudioClose();

void AudioSeekMS(int ms);
void AudioSetVolume(int v);                 // 0..1000
int  AudioPosMS();                          // -1 if unavailable
int  AudioLenMS();
bool AudioIsOpen();
bool AudioIsPlaying();
bool AudioIsSeeking();                      // Seek in progress in background (UI uses this to show loading)

std::wstring AudioLastError();
