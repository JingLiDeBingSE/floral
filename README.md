# Floral

A desktop music player for Windows, written in **C++ with Win32 + Direct2D**.
Frameless window, every control drawn by hand — no Electron.

Music data comes from Kugou, NetEase Cloud Music and QQ Music through small Node
services running on localhost. The portable build embeds those services and a Node
runtime into the executable, so it works out of the box.

English | [简体中文](README.zh-CN.md)

## Features

- **Playlist picker** — hexagonal grid, draggable; click a playlist to expand it into
  a card, click again to enter
- **Song grid** — irregular tile layout; hovering magnifies a tile and pushes its
  neighbours aside; scrolling applies motion blur
- **Player page** — spinning record, sliding album cases, draggable progress bar,
  volume and quality panels
- **Listen together** — two-person synced playback, host is authoritative and the
  guest follows automatically
- **Themes** — 7 colour schemes; UI available in Chinese and English

## Build

Requires MinGW-w64 g++ (with `windres`), or Visual Studio Build Tools + CMake.

```powershell
# MinGW
.\scripts\build.ps1 -Portable -NoTogether

# CMake (works with MinGW or MSVC)
cmake -S . -B build-cmake -G Ninja -DFLORAL_PORTABLE=ON
cmake --build build-cmake
```

Output goes to `build/`. `-NoTogether` / `-DFLORAL_NO_TOGETHER=ON` builds the variant
without the listen-together feature.

## Run

Double-click `build/floral_portable.exe`. On first launch you pick a platform and scan
a QR code with the corresponding mobile app.

Sign-in state, playlists and cover cache are written next to the executable and never
uploaded anywhere. Those files (`*-session.json`, `*.tsv`, `covers/`) are covered by
`.gitignore` — please don't commit them.

## Layout

```
src/        C++ sources (window, drawing, animation, audio, platform APIs)
sidecars/   local Node services (Kugou / NetEase / QQ / listen-together relay)
scripts/    build and packaging scripts
docs/       design and animation notes
```

## Disclaimer

Unofficial client, intended for learning and personal use only. All music rights
belong to the respective platforms and rights holders. MIT License.
