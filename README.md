# Floral

一个 Windows 桌面音乐播放器：**C++ / Win32 + Direct2D**，无边框窗口、全部自绘控件，不使用 Electron。
界面支持中文 / English。

数据来自酷狗、网易云、QQ 音乐三个平台，通过本机跑的 Node 接口服务获取；这些服务与 Node 运行时会内嵌进便携版 exe，开箱即用。

---

## 功能

- **歌单选择页**：正六边形点阵排布，可拖动；点击某个歌单以 Container Transform 展开成卡片，再点进入
- **选歌页**：不规则拼块布局，悬停放大并挤开邻居，滚动带运动模糊
- **播放页**：转动的唱片、上下滑动的专辑盒、可拖进度条、音量/音质面板
- **一起听**：两人同步播放（1:1，MQTT 中继），房主权威、跟随者自动对齐
- **主题**：7 套配色（莫兰迪默认、勿忘我、夏日、米饭、棉花糖、工业暗色、废墟）
- **语言**：中文 / English，设置里切换

---

## 目录结构

```
floral/
├─ src/                  C++ 源码
│   ├─ main.cpp          窗口、绘制、输入、动画（约 5600 行）
│   ├─ theme.*           配色与主题
│   ├─ audio.*           miniaudio 封装
│   ├─ kugou.*           三个平台的接口客户端
│   ├─ data.cpp          tsv 读取
│   ├─ together.*        一起听 HTTP 客户端
│   ├─ lang.*            中英文案
│   └─ app.rc            内嵌接口服务的资源脚本
├─ sidecars/             本机 Node 服务（第三方项目，随仓库分发）
│   ├─ kugou-api/        酷狗接口（端口 3000）
│   ├─ netease-api/      网易云接口（端口 4000）
│   ├─ qq-api/           QQ 音乐接口（端口 3200）
│   └─ together-mqtt/    一起听中继（端口 4100）
├─ scripts/
│   ├─ build.ps1         编译
│   ├─ build-sidecars.ps1 组装 sidecars.zip
│   └─ pack.ps1          打包便携版
└─ docs/                 设计与动画文档
```

---

## 编译

### 方式一：CMake（推荐，MinGW / MSVC 都行）

```powershell
# MinGW（Ninja 或 Makefiles 都可以）
cmake -S . -B build-cmake -G Ninja -DFLORAL_PORTABLE=ON
cmake --build build-cmake

# 装好 Visual Studio Build Tools 之后，用 MSVC 编
cmake -S . -B build-cmake -G "Visual Studio 17 2022" -DFLORAL_PORTABLE=ON
cmake --build build-cmake --config Release
```

两个版本一起编：

```powershell
.\scripts\build-cmake.ps1
```

CMake 选项：`-DFLORAL_PORTABLE=ON`（静态运行库）、`-DFLORAL_NO_TOGETHER=ON`（去掉一起听）、
`-DFLORAL_EMBED_SERVICES=OFF`（不内嵌接口服务）。

### 方式二：PowerShell 脚本（MinGW）

需要 **MinGW-w64 g++**（含 ``windres``）。在仓库根目录：

```powershell
# 开发版
.\scripts\build.ps1

# 便携版（静态链接，单文件，不含一起听）
.\scripts\build.ps1 -Portable -NoTogether
```

产物在 `build/`。

第一次编译前如果想内嵌接口服务，先准备：

```powershell
.\scripts\build-sidecars.ps1     # 生成 build\sidecars.zip（需要 node）
```

`sidecars/kugou-api/bin/app_win.exe` 是打包好的酷狗接口服务；没有它也能编译，只是不内嵌、运行时需要自己在 3000 端口起服务。

---

## 运行

双击 `build/floral.exe`（或便携版 `floral_portable.exe`）。

首次启动会提示选择平台并用对应 App 扫码登录。登录状态、歌单、封面缓存都写在 exe 同目录，
不会上传任何地方；`kugou-session.json` 之类含有凭证，**不要提交到仓库**（已在 `.gitignore` 里）。

---

## 代码约定

- 即时模式 UI：每帧重建布局并登记命中区，点击时反查最后注册的那个（后画的在上层）
- 动画统一用 `dt` 驱动，缓动集中在 `Ease()` / `smoothstep`
- 全局 `g_alpha`（整层透明度）和 `g_alphaMul`（局部透明度）都会被 `Col()` 乘进去；
  凡是改过这两个值的地方必须成对复位，否则会污染后面所有绘制
- 共享画刷（`g_brush`）的 `SetOpacity` 用完也要复位，同理

---

## 协作开发

想和别人一起改，看 [CONTRIBUTING.md](CONTRIBUTING.md)：分支模型、哪些文件不能提交、
两边分工怎么切、分支保护怎么设。CI 会在每个 PR 上用 MSVC 编一遍。

## 说明

非官方客户端，仅用于学习和个人使用；音乐版权归各平台与版权方所有。
