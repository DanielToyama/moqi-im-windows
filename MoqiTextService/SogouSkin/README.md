# Moqi 搜狗皮肤兼容层

这是给 `moqi-im-windows` 增加“搜狗皮肤外观”的独立模块。核心目标是让候选窗可以读取并绘制搜狗 `.ssf` / `skin.ini` 皮肤，同时提供小型外观/输入体验增强（状态栏菜单、符号输入、长按连发）。

## 设计原则：尽量不碰上游

新增的皮肤逻辑全部在：

```
MoqiTextService/SogouSkin/
SogouSkinTools/
```

另外新增两个独立的小模块：

```
MoqiTextService/KeyRepeatAccelerator.*
MoqiTextService/SogouSymbols.*
```

对上游文件的改动集中在少量挂载点：

| 文件 | 改动 |
|---|---|
| `MoqiTextService/CMakeLists.txt` | `add_subdirectory(SogouSkin)` + 链接 `MoqiSogouSkin` + 新增源文件 |
| `MoqiTextService/MoqiCandidateWindow.h/.cpp` | 增加皮肤 Renderer、候选皮肤分支、`reloadSogouSkin()` |
| `MoqiTextService/MoqiImeModule.cpp` | 没有 `configTool` 时，语言栏“设置”打开 `SogouSkinSettings.exe` |
| `MoqiTextService/MoqiTextService.h/.cpp` | 状态栏/右键菜单/符号插入/长按加速的挂载点 |
| `MoqiTextService/MoqiClient.h/.cpp` | `findButton()`，把皮肤状态栏映射到后端语言栏按钮；u 模式/拼字模式的候选窗 preedit 显示（`CompositionDisplay.h`） |
| `MoqiTextService/CompositionDisplay.h` | u 模式原始按键跟踪与 `u'... [拼字]` 显示格式（无 TSF 依赖，便于 `SogouLogicTest` 覆盖） |
| `MoqiTextService/MoqiLangBarButton.h` | 暴露 `iconFile()` 供状态图标判断 |
| 根 `CMakeLists.txt` | 增加可选的 `SogouSkinTools` 子目录 |

没有改：

- 后端协议 / protobuf
- TSF 生命周期
- 按键/候选/上屏的核心流程
- 现有外观配置的字段语义（没有皮肤路径时行为与上游一致）

因此后续 `git merge upstream/master` 时，冲突只会集中在这些挂载点；皮肤/辅助逻辑都在独立文件里。

## 如何启用

优先级从高到低：

1. 环境变量：
   ```powershell
   $env:MOQI_SOGOU_SKIN = "C:\path\to\win7风格.ssf"
   ```
   或指向已解包目录：`C:\path\to\win7`（目录内有 `skin.ini` 和图片）。
2. `%APPDATA%\Moqi\sogou_skin_path.txt`：第一行写皮肤路径。
3. `%APPDATA%\Moqi\sogou_skin.ssf`
4. `%APPDATA%\Moqi\sogou_skin\skin.ini`（目录形式）
5. `%APPDATA%\Moqi\skins\current\skin.ini`

没有找到皮肤时，`CandidateWindow` 使用原来的绘制路径，外观与上游一致。

也可以直接用安装脚本复制皮肤：

```powershell
powershell -ExecutionPolicy Bypass -File .\SogouSkinTools\install-skin.ps1 `
    -SkinPath "C:\Program Files (x86)\SogouImeSkinEditor\win7风格.ssf"
```

脚本会把 `.ssf` 复制到 `%APPDATA%\Moqi\sogou_skin.ssf`；如果传入的是已解包目录，则复制到 `%APPDATA%\Moqi\sogou_skin\`。重启 Moqi/文本服务后生效。

## 覆盖文件（无需重新打包 .ssf）

`%APPDATA%\Moqi\sogou_skin_overrides.ini` 会在 `skin.ini` 解析后按同名
section/key 合并，可覆盖任意已支持的皮肤字段：

```ini
[Display]
font_size=30
scale=120

[Scheme_H1]
layout_horizontal=1,93,150
pinyin_marge=55,2,22,76
separator=0xd8d8d8,20,180

[StatusBar]
cn_en_display=1
cn_en_pos=70,81

[Render]
key_gap=1
item_spacing=11
pinyin_bold=1
text_offset_y=0
pinyin_separator=auto
candidate_row_height=0

[MoqiOverrides]
version=3
layout_mode=global
```

`[Display] font_size` 是新语义下的“实际字体大小”（皮肤设计像素），只影响字形测量、
候选行高和容纳文字所需的布局增长；`[Display] scale` 是整窗缩放百分比（50–300，
100 为皮肤原始尺寸），9-slice 背景、边距、分隔线和候选 rect 一起按它缩放。

新版设置 UI 会自动写 `[MoqiOverrides] version=3` 标记：有该标记时，
`[Scheme_*]` 的 layout/marge/separator/anchor 一律视为用户手工高级项；
没有该标记的旧文件在加载时会自动忽略“基准值 × 旧字号/基准字号”生成的布局键。
旧版覆盖文件（无标记或 `version=2`）里的 `font_size` 是旧语义的“字体+整窗缩放”，
加载时会自动换算为 `scale = old_font_size / base_font_size × 100`，并把字体大小
恢复为皮肤自身的 `font_size`，因此升级后窗口尺寸与之前一致；设置 UI 保存时会写成
新语义的 `font_size` + `scale` + `version=3`。

`[Render]` 默认值就是旧版硬编码行为：`key_gap=1`、`item_spacing=11`、
`pinyin_bold=1`、`text_offset_y=0`、`pinyin_separator=auto`
（auto：带声调时保留空格，普通拼音用撇号）。`candidate_row_height=0`
表示自动（字号 + 4 px）。这些文件由 `E:\SogouSkin\studio` 的本地网页
控制台生成，也可以手工编辑。

## 显示选项

`%APPDATA%\Moqi\sogou_skin_options.ini`：

```ini
[Options]
statusbar=1
statusbar_fullscreen_hide=1
statusbar_fullscreen_scope=monitor
horizontal=1
statusbar_hide=
```

- `statusbar=1/0`：显示/隐藏右下角常驻状态栏；修改后切换到另一个应用再回来即可生效。
- `statusbar_fullscreen_hide=1/0`：前台窗口真正全屏（独占全屏、无边框全屏、浏览器 F11、视频全屏）时自动隐藏状态栏，默认 `1`；设为 `0` 完全恢复旧行为。桌面、普通窗口和最大化浏览器不会隐藏。
- `statusbar_fullscreen_scope=monitor/all`：默认 `monitor`，只隐藏被全屏窗口覆盖的显示器上的状态栏；`all` 表示任何显示器全屏都隐藏全部状态栏。状态栏不会跟随鼠标移动。
- `horizontal=1`：候选窗强制横排（H1）；`0` 使用 Moqi 的 `candidate_per_row` 设置（1 = 竖排 V1，>1 = 横排 H1）。
- `statusbar_hide=biaodian,quan_ban,...`：按名字隐藏状态栏按钮（逗号分隔），用于皮肤本身没有 `_display=1`、但你想强制隐藏/显示的微调。
- `statusbar_x` / `statusbar_y`：状态栏左上角坐标；留空或 `-1` 时使用屏幕右下角默认位置。
- 也可以用环境变量 `MOQI_SOGOU_STATUSBAR=0/1`、`MOQI_SOGOU_STATUSBAR_FULLSCREEN=0/1`、`MOQI_SOGOU_HORIZONTAL=0/1` 覆盖。

快捷脚本：

```powershell
.\SogouSkinTools\set-skin-options.ps1 -StatusBar 1 -Horizontal 1
# 隐藏不需要的状态栏按钮
.\SogouSkinTools\set-skin-options.ps1 -StatusBar 1 -Horizontal 1 -StatusBarHide "biaodian,quan_ban"
```

皮肤模式下拼音串会画在候选窗皮肤上（类似官方行为），并自动关闭 Moqi 的 inline preedit，避免重复显示。

字体大小与整窗缩放已经拆分：`Theme::baseFontSize()` 保留皮肤在合并 overrides 前的 `[Display] font_size`，`Display::fontSize` 是实际文字大小（设计像素），`Display::scalePercent` 是整窗缩放（50–300，默认 100）。渲染时 `scale = scalePercent / 100 × dpi / 96`，只对 9-slice 背景、边距、文字、分隔线和候选 rect 做统一变换；改变字体大小时只按文本测量增长候选行/窗口，不会把边距和背景按字体比例整体放大。100% 缩放 + 皮肤原生字体大小时输出与旧版完全一致。Web 调参台与原生设置 UI 都只写 `[Display] font_size` / `scale`，不再把 layout/marge/separator/anchor 按字号改写。

## 按皮肤保存文字位置微调

`%APPDATA%\Moqi\sogou_skin_layouts.ini` 保存每个皮肤自己的输入字/候选词偏移：

```ini
[MoqiLayouts]
version=1

[skin_4b85a907a8f7a3a0]
preedit_x=20
preedit_y=0
candidate_x=0
candidate_y=-15
```

- section 使用皮肤内容的稳定指纹：`.ssf` 对整个文件字节做 FNV-1a 64，解包目录对
  `skin.ini` + 资源相对路径/大小清单做哈希。这样设置界面预览用的源文件和 IME
  安装后的 `%APPDATA%\Moqi\sogou_skin.ssf` 副本会落到同一 section；不同皮肤
  （包括只有图片不同的皮肤）不会串用偏移。
- 偏移范围 -100..100 设计像素，默认 0；正值向右/向下。它只平移 `preeditRect`
  和候选点击 rect，背景 9-slice 与窗口设计尺寸保持不动。偏移在全局缩放/DPI 变换
  之前应用，因此 150%/200% 下会按同一比例正确换算。
- 设置界面修改后实时预览、应用和切换皮肤都会反映；应用时写入当前皮肤的 section，
  状态栏不受影响。

## 支持范围

已实现：

- `.ssf` ZIP 读取（miniz，支持 store/deflate）
- 已解包目录读取
- `skin.ini` 解析：UTF-16LE/UTF-8/ANSI
- 四套方案解析：`Scheme_H1/H2/V1/V2`
- 三种拉伸模式：中间拉伸、中间平铺、纵向固定
- 合窗口的 `pinyin_marge` / `zhongwen_marge` / `separator`
- PNG/BMP 背景、遮罩、字体/颜色
- 候选窗尺寸计算、候选点击命中（用皮肤布局的 rect）
- 独立字体大小 / 整窗缩放（50–300%）与按皮肤保存的输入字/候选词 X/Y 微调
- 拼音串绘制在候选窗皮肤上（`pinyin_marge` 位置），并关闭 inline preedit 避免重复
- H1（横排）和 V1（竖排）自动选择：
  - `sogou_skin_options.ini` 的 `horizontal=1` 强制 H1
  - 否则 `candPerRow > 1` → H1，`candPerRow == 1` → V1
- 竖排模式已按官方逻辑去掉首行额外偏移
- 如果皮肤只有 H2/V2，则回退到分窗口绘制
- **常驻状态栏窗口**：读取 `[StatusBar]` 的 `bar.png` / `bar_mask.png`，在屏幕右下角显示；没有输入/没有候选时也显示，可拖动，支持按钮悬停态和 `statusbar=0/1` 开关
- **单一前台状态栏**：每个 TSF 宿主进程最多只显示一个状态栏；状态栏每 150ms 校验当前前台窗口是否属于本进程（含父/owner 链、宿主子窗口），切换应用后旧窗口立即隐藏，切回该应用时由 `onSetFocus`/`onActivate` 重新显示。新应用没有皮肤状态栏（UWP、部分 Electron/特殊 TSF 宿主）时宁可不显示，也不会残留上一个应用的窗口。前台窗口真正全屏时（`SogouSkinFullscreen.*`：DWM 可见边界 + `SHQueryUserNotificationState` + 窗口样式）状态栏以 400ms 低频 timer 保持隐藏，退出全屏后自动恢复；默认只隐藏全屏窗口覆盖的显示器，Windows 搜索、任务管理器最大化、自动隐藏任务栏下的最大化浏览器和桌面都不会误隐藏
- **u 模式/拼字模式 preedit**：Rime `radical_lookup` 的 `prefix: u` 会在识别后吞掉组合串里的 `u`；前端跟踪空闲状态下以 `u` 开始的原始按键，在候选窗 preedit 显示 `u'...` 并追加 `[拼字]`（例如 `u'shui'shang[拼字]`）。只影响候选窗显示，不改变后端 composition/提交逻辑；按 Enter 仍沿用 Rime 的原始输入提交行为
- **状态栏按钮状态**：`cn_en`/`quan_ban` 直接映射到后端 `switch-lang`/`switch-shape` 语言栏按钮，按 `chi.ico`/`eng.ico`、`full.ico`/`half.ico` 判断当前状态；快捷键 Shift、Shift+Space 切换后，状态栏图标会在 150ms 内跟随更新
- **状态栏右键菜单**：右键状态栏可打开快捷菜单，包含中英文、全半角、中英标点、简繁、软键盘、**符号输入**（常用标点/数学符号/货币单位/特殊符号，通过 TSF 编辑会话插入），以及候选横排/竖排、状态栏显隐、皮肤设置、重新加载皮肤
- **搜索框输入优化**：状态栏图标只在状态变化时重绘；按键抬起不再每次重建分层窗口。Windows 搜索/开始菜单仍然加载搜狗皮肤，不关闭皮肤渲染。
- **长按加速**：普通字母/退格长按约 420ms 后以 25ms 间隔连发，最多 60 个；`sogou_skin_options.ini` 的 `key_repeat=0` 可关闭；Ctrl/Alt/Win、方向键、候选选择键不受影响
- **默认无衬线字体**：皮肤未指定 `font_ch` 时默认使用 `微软雅黑`；原生候选窗默认 `Microsoft YaHei UI`，仍可由后端 `candFontName` 覆盖
- **真透明**：候选窗和状态栏都使用 `WS_EX_LAYERED` + 32 位 alpha 合成，保留 PNG 的透明/阴影边缘；状态栏缺省只显示显式 `_display=1` 的按钮
- **字体解析与排版**：按 `font_ch` / `font_en` 在 `%LOCALAPPDATA%\Microsoft\Windows\Fonts`、`%WINDIR%\Fonts`、`SogouImeSkinEditor\Fonts` 查找字体文件，并用 `Gdiplus::PrivateFontCollection::AddFontFile` 显式加载；测量与绘制统一使用 GDI+ `GenericTypographic`，逐字符 advance 按 Python/Pillow 的整数舍入对齐；拼音粗体用 1px 描边实现，候选保持常规字重

暂未实现：

- `quan_shuang`/`sogousearch` 还没有稳定的后端命令，点击时不做动作，也不会伪造可能误触应用快捷键的按键；`softkeyboard` 会启动 Windows 触控键盘；`menu` 打开皮肤设置
- `biaodian`/`fan_jian` 可通过后端命令切换，但后端没有为它们发布语言栏状态按钮，图标只能跟随本地点击状态
- `Skin` version 3 加密皮肤
- Flash/相框/动画皮肤
- 精确的逐像素 window region（alpha 已支持；仅未按 mask 做 GDI Region 裁剪）
- 皮肤自带候选高亮图（Sogou 没有标准 highlight 图，本实现用第一候选颜色区分）

## 原生 C++ 设置界面

`SogouSkinTools/SogouSkinSettings` 是一个独立的 Win32/GDI+ 设置程序，部署为
`C:\Program Files (x86)\MoqiIM\SogouSkinSettings.exe`。入口：

- 语言栏右键 →“设置”（`ITfFnConfigure`；`ime.json` 没有 `configTool` 时自动打开）
- 皮肤状态栏右键 →“皮肤设置...”

功能：

- 换肤：列出 `%APPDATA%\Moqi\skins`、`%APPDATA%\Moqi` 和搜狗皮肤编辑器目录下的 `.ssf`，也可浏览任意 `.ssf`
- 缩放：50%–300% 滑块，控制整套皮肤/候选窗的全局缩放；默认 100%，不改变皮肤原生字体大小
- 字体大小：8–72 滑块，只改变文字大小；文字变大时候选行/窗口按测量结果增长，但 9-slice/边距不会乘字体比例
- 文字位置微调：输入字 X/Y、候选词 X/Y 四个带 Up-Down 步进的数值框（-100..100 设计像素，步长 1），按当前皮肤独立保存到 `sogou_skin_layouts.ini`，实时预览、应用和切换皮肤都会反映
- 字体颜色：拼音、首选候选、候选文字、注释四组颜色（Windows 颜色选择器）
- 状态栏：显示/隐藏状态栏、逐个勾选要显示的皮肤按钮
- 应用后写 `sogou_skin_overrides.ini` / `sogou_skin_options.ini` / `sogou_skin_layouts.ini`，并向所有皮肤状态栏广播
  `WM_MOQI_SOGOU_SETTINGS_CHANGED`，正在运行的输入法会立即重载皮肤，不需要重启应用
- 旧版设置 UI 曾把 `[Scheme_*]` 的 layout/marge/separator/anchor 按字号缩放后写入 overrides；旧版 Web 调参台还会缩放状态栏 `_pos`。新版保存时会按“皮肤基准值 × 旧字号/基准字号”识别并删除这些自动生成的布局键与状态栏坐标，保留手工高级项，并写入 `[MoqiOverrides] version=3 / layout_mode=global` 标记；渲染器在加载无标记或 version=2 的旧文件时也会自动把旧 `font_size` 迁移为新的 `scale`，避免与全局缩放叠加或改变老用户窗口尺寸

## 图形化调参台

本地 Web 控制台：`E:\SogouSkin\studio\start.cmd`，打开 <http://127.0.0.1:8765/>。可加载皮肤、实时预览 H1/V1、逐项调整 `[Display]`/`[Scheme_*]`/`[StatusBar]`/`[Render]`，并一键写入覆盖配置。

## 安装、部署与使用

本模块的**部署教程已统一维护在根目录 [README.md](../../README.md) 的“部署与使用教程（搜狗皮肤版）”**，包括：

- Windows 10/11、x64/x86、VS2022/CMake 环境要求；
- 构建 `build-vs32` / `build-vs64`（或 `*-local`）产物；
- `deploy-installed.ps1` 的 UAC 用法；
- 四份 `MoqiTextService.dll` 目标路径与 `sogou-skin-backup-<时间戳>` 备份/回滚；
- `.ssf` 皮肤导入、设置界面、字体/字号/颜色、状态栏按钮；
- 白霜拼音模糊音 `rime_frost.custom.yaml` 与独立 `sogou_user.dict.yaml` 词库导入；
- 旧 DLL 重启应用、状态栏单实例、SearchHost/WebView、长按连发等常见问题；
- 卸载与恢复原版。

本文只保留模块设计、支持范围和开发/测试说明，避免同一套安装步骤在两份 README 中重复并出现过时路径。

## 构建与测试

独立渲染测试工具不依赖 protobuf/TSF，可以单独构建：

```powershell
cmake -S SogouSkinTools -B build-sogou -G "Visual Studio 17 2022" -A x64
cmake --build build-sogou --config Release

# 从 .ssf 直接渲染（无窗口）
.\build-sogou\Release\SogouSkinRenderTest.exe "C:\Program Files (x86)\SogouImeSkinEditor\win7风格.ssf" H1 artifacts\win7_h1.png
.\build-sogou\Release\SogouSkinRenderTest.exe "C:\Program Files (x86)\SogouImeSkinEditor\win7风格.ssf" V1 artifacts\win7_v1.png
```

输出示例：

- `artifacts/win7_h1.png`：504×67
- `artifacts/win7_v1.png`：240×237

与搜狗编辑器导出的预览对照见 `artifacts/win7_h1.png` 和 `E:\SogouSkin\samples\previews\win7_editor_preview.png`。

完整构建仍按上游脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1 -Configuration Release
```

本地测试：

```powershell
# 无窗口逻辑测试（按键连发、符号、u 模式 preedit、状态栏单实例）
.\build-vs64-local\SogouSkinTools\Release\SogouLogicTest.exe
.\build-vs32-local\SogouSkinTools\Release\SogouLogicTest.exe

# 可选：真实窗口级状态栏前台切换冒烟测试
.\build-vs64-local\SogouSkinTools\Release\SogouLogicTest.exe --statusbar-smoke

# 可选：全屏隐藏/恢复/关闭开关冒烟测试（会短暂创建全屏 popup）
.\build-vs64-local\SogouSkinTools\Release\SogouLogicTest.exe --statusbar-fullscreen-smoke

# 可选：如果 MoqiWebView2TestHost 正在运行，检查 WebView2 宿主/渲染子进程归属
.\build-vs64-local\SogouSkinTools\Release\SogouLogicTest.exe --webview-check
```

> 注意：本机没有把 CMake 加进 PATH，使用 VS2022 自带的 CMake：
> `C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

## 上游跟进建议

1. 只在新目录里迭代皮肤解析/绘制；上游文件只在“挂载点”处改。
2. 若上游重写了 `CandidateWindow::onPaint` 或 `recalculateSize`，只需重新插入 `hasSogouSkin()` 分支，不要合并皮肤实现。
3. 若上游新增了候选窗外观配置，优先让皮肤路径从环境变量/`sogou_skin_path.txt` 读取，不要把皮肤字段塞进上游 `appearance_config.json` 的语义里。
4. 关闭皮肤：不设置任何皮肤路径即可；或把 `MOQI_BUILD_SOGOU_SKIN_PREVIEW=OFF` 只关闭预览工具。
