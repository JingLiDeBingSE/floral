# Floral

Windows 桌面音乐播放器，**C++ / Win32 + Direct2D**，无边框窗口、控件全部自绘，不使用 Electron。

歌曲数据来自酷狗、网易云、QQ 音乐，通过本机运行的 Node 接口服务获取；便携版已把服务与 Node 运行时内嵌进 exe，开箱即用。

## 功能

- **歌单选择页**：六边形点阵排布，可拖动；点击某个歌单展开成卡片，再点进入
- **选歌页**：不规则拼块布局，悬停放大并挤开邻居，滚动带运动模糊
- **播放页**：旋转唱片、上下滑动的专辑盒、可拖进度条、音量 / 音质面板
- **一起听**：两人同步播放，房主权威、跟随者自动对齐
- **主题**：7 套配色可切换；界面支持中文 / English

## 编译

需要 MinGW-w64 g++（含 `windres`），或者 Visual Studio Build Tools + CMake。

```powershell
# MinGW
.\scripts\build.ps1 -Portable -NoTogether

# CMake（MinGW 或 MSVC 都行）
cmake -S . -B build-cmake -G Ninja -DFLORAL_PORTABLE=ON
cmake --build build-cmake
```

产物在 `build/`。`-NoTogether` / `-DFLORAL_NO_TOGETHER=ON` 是去掉「一起听」的版本。

## 运行

双击 `build/floral_portable.exe`。首次启动会让你选平台并扫码登录。

登录状态、歌单、封面缓存都写在 exe 同目录，不会上传到任何地方；这些文件（`*-session.json`、`*.tsv`、`covers/`）
已经在 `.gitignore` 里，不要提交。

## 目录

```
src/        C++ 源码（窗口、绘制、动画、音频、接口）
sidecars/   本机 Node 服务（酷狗 / 网易云 / QQ / 一起听中继）
scripts/    编译与打包脚本
docs/       设计与动画文档
```

## 说明

非官方客户端，仅用于学习和个人使用；音乐版权归各平台与版权方所有。MIT License。
