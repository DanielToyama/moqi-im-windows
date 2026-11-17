# 墨奇输入法 Windows 版（搜狗皮肤增强版）

这是 [`DanielToyama/moqi-im-windows`](https://github.com/DanielToyama/moqi-im-windows) 的搜狗皮肤增强分支，fork 自 [`gaboolic/moqi-im-windows`](https://github.com/gaboolic/moqi-im-windows)。

本分支只修改 Windows 前端外观和输入体验：搜狗 `.ssf` / `skin.ini` 皮肤、候选窗、常驻状态栏、原生皮肤设置界面、字体/字号/颜色、状态栏按钮、长按连发、u 模式 preedit 等。不修改 `moqi-ime` 后端、协议和候选/上屏核心流程，后续可以继续合并上游更新。

## 快速开始（部署与使用教程）

普通用户不需要编译，直接下载 Release 里的 DLL 覆盖即可。

### 1. 下载 Release 文件

打开 [Releases](https://github.com/DanielToyama/moqi-im-windows/releases)，把下面这些文件下载到**同一个文件夹**：

| 文件 | 用途 |
|---|---|
| `MoqiTextService-ProgramFiles-x86.dll` | 安装目录的 32 位 DLL |
| `MoqiTextService-ProgramFiles-x64.dll` | 安装目录的 64 位 DLL |
| `MoqiTextService-System32.dll` | `C:\Windows\System32` 的 64 位 DLL |
| `MoqiTextService-SysWOW64.dll` | `C:\Windows\SysWOW64` 的 32 位 DLL |
| `SogouSkinSettings.exe` | 皮肤设置程序 |
| `install.bat` | 双击运行的安装脚本入口 |
| `install.ps1` | 安装脚本本体 |

也可以直接下载 `moqi-sogou-skin-dlls-<版本>.zip`，解压到任意文件夹，里面已经包含上述文件。

> 四个 DLL 缺一不可，而且必须和 `install.bat`、`install.ps1` 放在同一个文件夹里。

### 2. 放到同一个文件夹

例如新建：

```text
C:\Users\你的用户名\Downloads\moqi-skin-install\
```

把上面 7 个文件全部拖进去。不要只把 DLL 拖到桌面后单独双击，脚本会找不到同目录的安装文件。

### 3. 双击 `install.bat`

1. 双击 `install.bat`。
2. Windows 弹出 UAC 时点“是”。
3. 脚本会自动：
   - 停止 `MoqiLauncher.exe`；
   - 把原文件备份到 `C:\Program Files (x86)\MoqiIM\sogou-skin-backup-<时间戳>\`；
   - 把四个 DLL 分别复制到四个 TSF 实际加载位置；
   - 安装 `SogouSkinSettings.exe`；
   - 重新启动 `MoqiLauncher.exe`。
4. 看到“安装完成”后按回车退出。

对应关系如下：

| Release 文件 | 复制到 |
|---|---|
| `MoqiTextService-ProgramFiles-x86.dll` | `C:\Program Files (x86)\MoqiIM\MoqiTextService.dll` |
| `MoqiTextService-ProgramFiles-x64.dll` | `C:\Program Files (x86)\MoqiIM\x64\MoqiTextService.dll` |
| `MoqiTextService-System32.dll` | `C:\Windows\System32\MoqiTextService.dll` |
| `MoqiTextService-SysWOW64.dll` | `C:\Windows\SysWOW64\MoqiTextService.dll` |

安装完成后，关闭并重新打开正在使用输入法的应用（浏览器、微信、VS Code、记事本等）。**不需要重启系统。**
如果某个应用还是旧外观，通常是因为它还在用安装前加载的旧 DLL，重开该应用即可。

### 4. 选择搜狗皮肤

打开：

```text
C:\Program Files (x86)\MoqiIM\SogouSkinSettings.exe
```

在“皮肤”下拉框选择 `.ssf`（会列出最近使用、下载目录、桌面和搜狗皮肤编辑器目录，也可以点“浏览...”），点“应用”。正在运行的输入法会立即重载，不需要重启应用。

设置界面还可以调整：

- 字号 8–72，整套候选窗按比例缩放；
- 中文/英文字体；
- 拼音、首选、候选、注释四组颜色；
- 状态栏按钮显示/隐藏；
- 候选横排/竖排。

### 5. 回滚

如果安装后不满意，关闭 `MoqiLauncher` 和相关应用，把
`C:\Program Files (x86)\MoqiIM\sogou-skin-backup-<时间戳>\` 里的文件按原路径复制回去即可。
也可以重新运行官方墨奇安装包选择“修复/卸载”。

## 特色功能

### 搜狗皮肤与输入体验

- **搜狗皮肤引擎**：支持 `.ssf` / `skin.ini`，H1/H2 横排、V1/V2 竖排四种方案，9-slice、透明 PNG、锚点、分隔线和皮肤字体。
- **原生 C++ 设置界面**：皮肤下拉、最近使用、字体、字号、四组颜色、状态栏按钮开关、实时预览。
- **高分屏/多显示器**：支持系统缩放 100%–200% 和不同 DPI 显示器之间切换，候选窗、状态栏、设置界面会按当前显示器 DPI 同步缩放。
- **常驻皮肤状态栏**：同一时刻只显示当前前台应用的一个状态栏；按钮可切换中英文、全半角、中英标点、简繁；右键菜单包含皮肤设置、符号输入、横竖排、显示/隐藏状态栏、重新加载皮肤。
- **长按连发**：按住普通字母或退格约 420ms 后连续输入，最多 60 个；组合键、方向键、候选数字键不参与。
- **常见符号工具**：状态栏右键可直接插入标点、数学、货币、特殊符号。
- **u 模式/拼字模式**：候选窗显示 `u[拼字]`、`u'shui'shang[拼字]`；长输入、空候选、逐字退格时窗口保持可见；回车提交临时英文时去掉前导 `u`。

### 上游原有功能

- 托盘切换中英文、全半角、简繁；
- 白霜拼音，支持切换方案集、切换输入方案、更新配置；
- 多整句候选、置顶短语、超级简拼；
- 自动插入成对符号、分号键次选；
- WebDAV 词库同步、云剪贴板、AI 整句/翻译/问答；
- 外观主题、字体、主题色和快捷键设置。

上游功能的详细说明见 [上游 README](https://github.com/gaboolic/moqi-im-windows)。

## 常见问题

- **双击 `install.bat` 提示缺少文件**：四个 DLL、`SogouSkinSettings.exe`、`install.bat`、`install.ps1` 必须放在同一个文件夹。
- **安装时提示文件被占用**：先关闭 `MoqiLauncher.exe`、浏览器、微信、编辑器等正在使用输入法的应用，再运行 `install.bat`。
- **安装后还是旧皮肤/旧界面**：重开对应应用；Explorer 可重启一次 `explorer.exe`。不需要重启系统。
- **2K/150% 或更换缩放后候选窗/设置界面错位**：v1.0.7 起已按当前显示器 DPI 缩放候选窗、状态栏和设置界面；确认四个 DLL 和 `SogouSkinSettings.exe` 都已更新。
- **状态栏不显示或残留旧窗口**：确认皮肤 `[StatusBar]` 有 `pic=...`，并在设置里勾选“显示皮肤状态栏”；本分支同一时刻只保留当前前台应用的一个状态栏。

- **长按连发想关闭**：在 `%APPDATA%\Moqi\sogou_skin_options.ini` 写 `key_repeat=0`。
- **状态栏按钮不见了**：默认只显示皮肤 `[StatusBar]` 中显式 `_display=1` 的按钮，可在设置界面逐个勾选。

## 高级设置

### 皮肤覆盖文件

`%APPDATA%\Moqi\sogou_skin_overrides.ini` 可以覆盖皮肤字体、字号、颜色和布局字段；
`%APPDATA%\Moqi\sogou_skin_options.ini` 控制状态栏显示、横竖排、长按连发和状态栏坐标。

### 白霜拼音模糊音

只改用户目录下的 `%APPDATA%\Moqi\Rime\rime_frost.custom.yaml`，用 `speller/algebra/+` 追加规则，例如：

```yaml
patch:
  "speller/algebra/+":
    - derive/^([zcs])h/$1/
    - derive/^([zcs])([^h])/$1h$2/
    - derive/ang$/an/
    - derive/an$/ang/
    - derive/eng$/en/
    - derive/en$/eng/
    - derive/ing$/in/
    - derive/in$/ing/
```

保存后在托盘菜单执行“刷新配置(&R)”。

### 导入搜狗词库

不要把几万条词库塞进 `custom_phrase`。新建独立的
`%APPDATA%\Moqi\Rime\sogou_user.dict.yaml`，再在 `rime_frost.dict.yaml` 的 `import_tables:` 中加入 `sogou_user`，最后刷新配置。这样可以避免每次按键扫描大表导致卡顿。

## 开发者：从源码构建

前置：Visual Studio 2022（桌面 C++ 工作负载）、CMake 3.21+、Windows SDK。部署需要管理员权限，构建不需要。

```powershell
git clone https://github.com/DanielToyama/moqi-im-windows.git
cd moqi-im-windows

# 如果搜狗皮肤改动在独立分支：
git checkout feature/sogou-skin

git submodule update --init --recursive

# 应用 libIME2 的外部 preedit/蓝底修复补丁（已包含时会自动跳过）
powershell -ExecutionPolicy Bypass -File .\scripts\apply-libime2-patch.ps1
```

推荐构建方式：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

产物：

- `build-vs32\MoqiTextService\Release\MoqiTextService.dll`
- `build-vs64\MoqiTextService\Release\MoqiTextService.dll`
- `build-vs32\SogouSkinTools\Release\SogouSkinSettings.exe`
- `build-vs32\SogouSkinTools\Release\SogouLogicTest.exe`
- `build-vs64\SogouSkinTools\Release\SogouLogicTest.exe`

如果直接用 VS 自带 CMake：

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

& $cmake -S . -B build-vs32 -G "Visual Studio 17 2022" -A Win32
& $cmake --build build-vs32 --config Release --target MoqiTextService SogouSkinSettings SogouLogicTest --parallel 8

& $cmake -S . -B build-vs64 -G "Visual Studio 17 2022" -A x64
& $cmake --build build-vs64 --config Release --target MoqiTextService SogouLogicTest --parallel 8
```

源码构建后的部署可以继续使用脚本（会备份原文件）：

```powershell
powershell -ExecutionPolicy Bypass -File .\SogouSkinTools\deploy-installed.ps1
```

或者运行 `.\SogouSkinTools\run-deploy.ps1` 自动请求 UAC。

### 自动发布 DLL

仓库自带 `.github/workflows/release-dll.yml`。推送 `v*` 或 `dll-*` tag，或在 Actions 页面手动运行
`Release patched DLLs`，会自动：

1. 应用 `libIME2` 补丁；
2. 构建 x64/x86 DLL 和 `SogouSkinSettings.exe`；
3. 跑 x64/x86 的 `SogouLogicTest`；
4. 把四个目标 DLL、`install.bat`、`install.ps1`、设置程序和 zip 上传到 GitHub Release。

```powershell
git tag v1.0.0
git push origin v1.0.0
```

## 上游同步

```powershell
git remote add upstream https://github.com/gaboolic/moqi-im-windows.git
git fetch upstream
git merge upstream/main
```

如果 `upstream` 已存在，跳过 `git remote add`。合并后重新构建并运行 `SogouLogicTest` /
`SogouSkinRenderTest`。

搜狗皮肤逻辑集中在 `MoqiTextService/SogouSkin/` 和 `SogouSkinTools/`，上游文件只保留少量挂载点；
详细模块说明见 [MoqiTextService/SogouSkin/README.md](MoqiTextService/SogouSkin/README.md)。

## 参考

- [Microsoft TSF 文档](https://learn.microsoft.com/windows/win32/tsf/text-services-framework)
- [PIME](https://github.com/EasyIME/PIME)
- [小狼毫](https://github.com/rime/weasel)
- [白霜拼音](https://github.com/gaboolic/rime-frost)

[![Star History Chart](https://api.star-history.com/svg?repos=DanielToyama/moqi-im-windows&type=Date)](https://star-history.com/#DanielToyama/moqi-im-windows&Date)
