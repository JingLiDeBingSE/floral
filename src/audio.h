#pragma once
#include <string>

/* Plays a local file or an http(s) stream (FLAC / MP3 / WAV) at its own sample rate.
   On failure AudioLastError() returns a message for the UI. */

bool AudioOpen(const std::wstring& path);   // Load and prepare; playback starts with AudioPlay
void AudioPlay();
void AudioPause();
void AudioClose();

void AudioSeekMS(int ms);
void AudioSetVolume(int v);                 // 0..1000
int  AudioPosMS();                          // -1 if unavailable
bool AudioIsPlaying();
bool AudioIsSeeking();                      // Seek in progress in background (UI uses this to show loading)
bool AudioIsEnded();                        // The decoder has played the whole track

std::wstring AudioLastError();
