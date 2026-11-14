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
bool AudioIsSeeking();                      // Seek queued or running in the background (UI shows loading)
bool AudioIsEnded();                        // The decoder has played the whole track
bool AudioIsFailed();                       // Playback stopped early because the download broke off

std::wstring AudioLastError();
