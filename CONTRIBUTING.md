# 协作开发

两个人（或更多人）同时改这个仓库时的约定。目标：少冲突、能各自跑起来、不把私人数据传上去。

---

## 一、第一次拿到代码

```powershell
git clone https://github.com/<owner>/floral.git
cd floral

# 编译（MinGW 或 MSVC 都行，见 README）
.\scripts\build.ps1 -Run
```

**不需要**向任何人要歌单文件或登录状态：那些是每台机器自己的，程序首次启动会让你扫码登录。

编译依赖：

| 依赖 | 说明 |
|---|---|
| MinGW-w64 g++（含 windres） | 最快上手 |
| 或 Visual Studio 2022 Build Tools + CMake | CI 用的就是这套 |
| CMake ≥ 3.20（可选） | `.\scripts\build-cmake.ps1` |

---

## 二、日常同步

主分支保护，所有改动走分支 + PR：

```powershell
git switch main
git pull --rebase            # 拉最新
git switch -c feat/playlist-drag
# ... 改代码 ...
git add -A
git commit -m "feat: 歌单页支持惯性拖动"
git push -u origin feat/playlist-drag
```

然后在 GitHub 上开 PR，合并方式统一用 **Squash and merge**（历史干净）。

- 拉之前有本地改动：先 `git stash` 或提交到分支
- 长期分支容易冲突：分支活不过两三天，合完就删
- 不要 `git push --force` 到 `main`

提交信息用 `类型: 说明` 的格式：

```
feat:     新功能
fix:      修 bug
refactor: 不改变行为的重构
style:    只动格式/配色
docs:     文档
build:    构建脚本、依赖
chore:    杂项
```

---

## 三、绝对不要提交的东西

`.gitignore` 已经挡住，但别用 `git add -f` 绕过：

| 文件 | 为什么 |
|---|---|
| `kugou-session.json` / `netease-session.json` / `qq-session.json` | 含登录 token，泄露等于账号被盗 |
| `*.tsv`（`kugou-likes.tsv` / `playlists.tsv` / `pl-*.tsv`） | 个人歌单数据 |
| `covers/` | 封面缓存，几百 MB |
| `build/` `dist/` `*.zip` `*.o` | 构建产物，每个人自己编 |
| `sidecars/**/node_modules/` | 依赖，用 `npm install` 自己装 |

提交前自查：

```powershell
git status --short          # 看一眼有没有意外文件
git diff --cached --stat    # 确认改动范围
```

---

## 四、两边都要改同一处时的分工

按**文件**分工最省事，这个项目的天然边界：

| 区域 | 文件 |
|---|---|
| 界面绘制 / 输入 | `src/main.cpp`（大头，尽量一个人主改） |
| 动画与转场 | `src/main.cpp` 里的 `Animate` / `DrawExiting` 等 |
| 配色主题 | `src/theme.cpp` `src/theme.h` |
| 接口与歌单 | `src/kugou.cpp` `src/data.cpp` |
| 音频 | `src/audio.cpp` |
| 一起听 | `src/together.cpp` `sidecars/together-mqtt/` |
| 文案 | `src/lang.*` |

`main.cpp` 有 5000 多行，两个人同时改冲突会很难受——**约定同一时间只有一个人动它**，
另一人先做 `theme.cpp` / `kugou.cpp` / 文案这类独立文件。

---

## 五、分支保护（仓库管理员设置一次）

GitHub → Settings → Branches → Add branch protection rule：

- Branch name pattern: `main`（或 `master`，看你仓库默认分支名）
- ✅ Require a pull request before merging
- ✅ Require status checks to pass（选 `build`，即 CI）
- ✅ Require branches to be up to date before merging

这样谁都不能直接推 `main`，也就不会互相覆盖。

---

## 六、加协作者

GitHub → Settings → Collaborators → Add people → 输入对方 GitHub 用户名 → 选 `Write` 权限。

对方接受邮件邀请后就能 `git clone` 和推分支了。
如果以后要更多人，建议转成 Organization 用 Teams 管理。

---

## 七、CI

`.github/workflows/build.yml` 在每次 push / PR 时用 **MSVC** 编一遍（不带内嵌服务），
确保两边代码在任何机器上都能过编译。PR 页面会显示绿色的 ✓ 或红色的 ✗。

本地想复现 CI 的编译：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DFLORAL_PORTABLE=ON -DFLORAL_EMBED_SERVICES=OFF
cmake --build build --config Release
```
