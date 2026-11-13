# MiniShell

一个现代桌面浏览器外壳，**UI 来自 agave `examples/browser_shell`，Web 内核换成 miniblink 132**。

外壳负责标签条、地址栏、查找栏、状态气泡、自绘菜单、主题 / DPI、书签、历史、会话恢复和下载提示；
页面渲染、输入、IME、焦点、网络全部由 `mb132_x64.dll` 承担。

```
+--------------------------------------------------------------------+
| [1 标题] [2 标题] ... [+]                    [ — ][ □ ][ X ]       |
+--------------------------------------------------------------------+
| <  >  R/X  [ 锁 地址栏....................................... > ] ☰ |
+--------------------------------------------------------------------+
|                                                                    |
|               页面：一个 MB_WINDOW_TYPE_CONTROL 子窗口              |
|                                                                    |
+--------------------------------------------------------------------+
```

## 目录

| 路径 | 说明 |
|---|---|
| `app.rc` / `app.ico` | 可执行文件与窗口图标（ID 101，`main.c` 里注册到窗口类）；`assets/inspect.js` 也由它编进 exe（RCDATA 201） |
| `assets/inspect.js` | F12 元素标记的页面半边；已嵌入 exe，exe 旁放同名文件可覆盖（方便改脚本） |
| `src/main.c` | 外壳主体（来自 agave，替换了引擎调用与品牌/内部页） |
| `src/mb_view.c` | 适配层：用 miniblink 实现 `agave_webview_*` 控制 API |
| `src/agave_webview.h` | 外壳与控件之间的 C 接口（含 `MC_DOWNLOAD_INFO`） |
| `src/ui_*.c` / `shell_ops.*` | agave 的引擎无关 UI：主题、控件、chrome、菜单、标签搜索 |
| `src/gdiplus_c.h` | GDI+ flat API 的 C 声明（MSVC 的 `<gdiplus.h>` 只能 C++ 用） |
| `src/agb_log.*` | agave `AgbLog` 的最小替身，写 `shell.log` |
| `src/shot_png.*` | 用 WIC 写 PNG，替代 agave 的 libpng |
| `src/mb.h` | miniblink 132 头文件（来自 `demo_src`） |
| `lib/mb132_x64.lib` | 由 DLL 导出表生成的导入库 |
| `tools/dlserver.ps1` | 下载/新窗口回归用的本地 HTTP 服务 |

## 构建（MSVC 2022 x64）

```powershell
cd browsershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

产物：`build-msvc\Release\minishell.exe`，构建后会自动把 `..\mb132_x64.dll` 复制到 exe 旁边。

换了 DLL 之后重新生成导入库：

```powershell
pwsh -File tools\gen_import_lib.ps1
```

## 运行

```powershell
build-msvc\Release\minishell.exe https://example.com/
```

数据目录：`%APPDATA%\MiniShell`（不可写时退回 exe 旁的 `MiniShellData`）。
Cookie / localStorage 都在这个目录下，每个标签共享同一 profile。

## 引擎能力映射

| 外壳需要 | miniblink 实现 |
|---|---|
| 一个标签一个控件 | `mbCreateWebWindow(MB_WINDOW_TYPE_CONTROL, shellHwnd, ...)` |
| 标题 / URL / 前进后退状态 | `mbOnTitleChanged` / `mbOnURLChanged`（直接带 canGoBack/canGoForward） |
| 加载完成 | `mbOnLoadingFinish` + `mbOnDocumentReady` |
| 进度百分比 | 无原生回调：10%（URL 变化）→ 60%（DOM ready）→ 100%（加载完成） |
| 悬停链接状态栏 | `mbOnMouseOverUrlChanged` |
| 后退/前进/刷新/停止 | `mbGoBack` / `mbGoForward` / `mbReload` / `mbStopLoading`。**首次导航提交前（`mbOnURLChanged` 还没到）不能 `mbReload`**：它会把在途加载丢掉且不重发，页面永远空白、进度停在 5%，适配层此时改用 `mbLoadURL` 重新导航 |
| 执行 JS | `mbRunJs`（异步、按命令顺序执行） |
| 页面缩放 | 注入 CSS `zoom`（`mbSetZoomFactor` 在本版本会把因子画两次，页面被裁掉，见 AGENTS） |
| 查找 | 注入 JS（`<mark data-mb-find>` 高亮 + scrollIntoView），非破坏式 API 不存在 |
| 复制/粘贴/全选 | `mbEditorCopy` / `mbEditorPaste` / `mbEditorSelectAll` |
| 右键菜单 | `mbSetContextMenuEnabled(FALSE)` + 子类化 `WM_CONTEXTMENU`，用页内 JS 取 link/img/selection，再弹外壳自绘菜单 |
| 新窗口 / window.open | `mbOnCreateView` → 新建 control 窗口 → `on_new_window` 交给外壳开标签 |
| alert / confirm / prompt | `mbOnAlertBox` / `mbOnConfirmBox` / `mbOnPromptBox` |
| 下载 | `mbOnDownloadInBlinkThread` + 未文档化的 `mbDownloadByUtf8Path`（`path` 是**目录**，引擎自己处理重名） |
| 修改 / 替换返回内容 | `mbOnLoadUrlBegin` 里 `mbNetHookRequest(job)`；`mbOnLoadUrlEnd` 里 `mbNetSetData` / `mbNetSetMIMEType` / `mbNetSetResponseStatusCode`（当前外壳未启用，示例见 `tools/net_hook_demo.c`） |
| 直接拦截并伪造响应 | `mbOnLoadUrlBegin` 里 `mbNetSetData` + `mbNetSetMIMEType` 后返回 `TRUE`（mb 不再发请求） |
| 页面截图（探针） | `mbGetLockedViewDC` + `StretchBlt`（隐藏窗口也能取到 backing store） |
| `minishell://newtab` | 新标签页（搜索框）；提交走 `__agb_newtab_search__`，由外壳的默认搜索引擎和归属参数决定 |
| `minishell://downloads` | 适配层自己生成下载列表 HTML |
| `minishell://settings` | 适配层生成的设置页；控件通过 `agavePostMessage('__agb_settings_*__')` 与外壳双向通信 |

引擎回调部分在 loader 线程，**全部 PostMessage 到控件窗口**，再由控件子类在 UI 线程调用外壳回调；
探针会检查这一点（`callback thread ok`）。

## 已实现的交互

- 标签：新建 / 关闭 / 中键关闭 / 按下即切换 / 会话恢复；新标签页是 `minishell://newtab`
  （居中搜索框，地址栏显示为空），不是空白 about:blank
- 地址栏：Enter 导航、Ctrl+L 全选；无点号按搜索词走默认搜索引擎
- 快捷键：Ctrl+T/W/R/L/F/J/D、Ctrl+Tab、Ctrl+Shift+A、Ctrl+Shift+T、Ctrl+= / Ctrl+- / Ctrl+0、F5、F12、Alt+←/→、Esc
- 缩放：Ctrl+= / Ctrl+- / Ctrl+0、Ctrl+滚轮、应用菜单和设置页；按标签记录，导航后重新套用（CSS `zoom`）
- 查找栏：浮层、Enter/Shift+Enter、Esc 关闭
- 自绘右键菜单和应用菜单、书签/历史子菜单
- 主题 `AGB_SHELL_THEME=light|dark|auto`、Per-Monitor V2 DPI
- 设置页（应用菜单 → Settings）：主题、搜索引擎、默认缩放、启动恢复会话、清空历史、下载目录；
  改动即时生效并写入 `<profile>/prefs.txt`（主题仍在 `theme.txt`）；Data 区显示 profile
  目录（可打开），About 区把 **Shell**（git 版本 + 构建时间）和 **Engine**（DLL 版本 +
  SHA-256）分开显示，并有项目地址、启动参数和环境摘要
- 下载：写入系统 Downloads（或 `--download-dir` 之外的默认目录），状态气泡显示进度/结果
- 单实例 / `--new-tab` / `--new-window` 交接到已运行窗口
- 崩溃：隐私无关的 `agb_crash.txt` + 独立的崩溃页（完整诊断、一键复制报告）

## 新标签页与搜索归属

新标签页 `minishell://newtab` 的搜索框提交后由外壳按默认搜索引擎解析（和地址栏同一条
`normalizeUrl` 路径）：带点的文本按地址导航，其余按搜索词处理。百度和 Google 的结果 URL
会带本构建的归属参数，方便从搜索结果反查版本：

- Baidu：`...&tn=minibrowsershell-<webcore6>-<shellver>`
- Google：`...&client=minibrowsershell-<webcore6>-<shellver>`（不是 `tn`）

`<webcore6>` 是 `mb132_x64.dll` 的 SHA-256 前 6 位；`<shellver>` 由
`tools/gen-shell-version.ps1` 在每次构建前写进 `build-msvc/generated/shell_version.h`：
HEAD 有 tag 用 tag，否则用 commit 前 6 位，工作树有改动再追加 `-dirty`。设置页 About 和
崩溃报告显示同一个版本。

## 引擎启动竞态（导航守卫）

miniblink 偶尔会**静默丢掉一个新控件的第一次导航**：没有 `mbOnURLChanged`、没有
`mbOnLoadingFinish`、没有错误回调；同进程里新建一个控件却能正常加载，说明引擎没死，
是那个控件废了。此时标签停在 5%、页面空白，`mbLoadURL` 也救不回来——只有换控件。

外壳有两条防线：

- 首次提交前（还没收到 `mbOnURLChanged`）点刷新不走 `mbReload`：它会丢掉在途加载且
  不重发，改为重新 `mbLoadURL`；
- 导航守卫：`http/https/file` 导航超过 `AGB_NAV_GUARD_MS`（默认 4000）仍没有任何进度
  （还是 5%）就销毁并重建该标签的控件，最多 2 次；仍无响应则停掉加载状态并提示。

日志里表现为 `nav guard: engine never committed - rebuilding view ...`。

## 崩溃报告

崩溃时外壳在进程 CWD 写 `agb_crash.txt`（完整报告），并另起一个进程显示崩溃页：
异常代码/地址、访问类型、完整调用栈（`模块+偏移`）、全部已加载模块及版本、系统版本、
架构、DPI、进程存活时间，以及**排查构建用的标识**：项目地址、`mb132_x64.dll` 的 SHA-256、
启动参数和环境变量摘要（URL/profile 路径等一律显示为 `(set)` / `(url omitted)`）。
设置页的 **About** 也显示同一组信息（Project / Engine / Engine SHA-256 / Launched with /
Environment），方便用户报问题时对照。

**报告不含隐私**：没有 URL、页面标题、文件路径、账号或机器名，只有模块名和脱敏后的启动开关。
页面上有一个 **Copy report** 按钮，复制出来的就是报告全文，可以直接贴到 issue；
报告文件本身也可以作为附件。报告里的帧是 `模块+偏移`，维护者用对应版本的 PDB /
`build-msvc/minishell.map` 还原符号。

- `minishell.exe --crash-page <report>`：单独打开一个报告（崩溃处理器就是这么启动的），
  不读 profile、不恢复会话。
- `AGB_CRASH_POPUP=0`：只写报告，不显示页面。
- `AGB_TEST_CRASH=1`：下一次启动立刻制造一个访问违例，用来验证整条链路；
  配合 `AGB_SHELL_HIDDEN=1`（不显示窗口）和 `AGB_CRASH_PAGE_SAVE=1`（把崩溃页 HTML
  存到 `%TEMP%\MiniShell-crash-page.html`）可以 headless 验证。

## 已知差异

- 没有 agave 的渲染进程崩溃页 / “页面无响应”真实检测（`on_crashed` 永不触发）。
- 没有 `set_visibility` / 暖帧 / `paint_to` 之外的原生离屏接口；`has_frame` 恒为 1，切标签直接显示。
- 查找是 DOM 注入实现；页面若禁用脚本或 DOM 极大，效果不如原生 find-in-page。
- 下载进度依赖未文档化回调；若换 DLL 后行为变化，可用 `tools/dlserver.ps1` 回归。
- **不接 Chromium DevTools**（`devtools.asar` / `minidevtools_x64.exe` 不参与运行）。
  调试用 F12 的宿主元素标记：页面半边 `assets/inspect.js` 通过 `app.rc`（RCDATA 201）
  **编进 exe**，F12 / 右键菜单 / 应用菜单都不再依赖 exe 旁的文件；把同名 `inspect.js`
  放在 exe 旁仍会覆盖内嵌副本（方便改脚本后免重编译）。`--inject <file>` 仍可注入
  任意外部脚本。

## 调试

```powershell
# 无头探针：加载、执行 JS、检查 DOM
build-msvc\Release\minishell.exe --probe https://example.com/ --eval "probeExit(document.title)"

# 查找回归：--find <text> 在 --eval 之前执行
build-msvc\Release\minishell.exe --probe https://example.com/ --find domain --eval "probeExit(String(document.querySelectorAll('mark[data-mb-find]').length))"

# 页面截图（隐藏窗口也能取到）
build-msvc\Release\minishell.exe --probe https://www.baidu.com/ --shot baidu.png --eval "document.title" --settle 2000

# chrome 离屏截图 + UI 断言
build-msvc\Release\minishell.exe --ui-shot ui-dark.png --ui-theme dark
```

运行日志：exe 同目录的 `shell.log`（每行带毫秒时间戳和 channel）。

引擎时序对照：`MBV_TRACE=1` 让适配层把每次 miniblink 调用与回调各写一行 `[nav]`
（`mbInit` / 创建 / `mbLoadURL` / `mbReload` / `mbOnURLChanged` / `mbOnLoadingFinish` /
`mbOnDocumentReady` / `mbOnNavigation`）。一次"卡在 5%"和一次正常加载逐行对比，
就能直接看出哪一步的回调没再回来：

```powershell
$env:MBV_TRACE=1
build-msvc\Release\minishell.exe http://www.163.com/
```

导航守卫的等待时间也能压短，用来复现/回归"控件没被引擎接手"的路径（日志里同时会写
`nav guard: engine never committed`）：

```powershell
$env:AGB_NAV_GUARD_MS=1500
```

## 许可

- 外壳 UI 源码来自 agave（BSD 风格许可），原文见 `LICENSE.agave.txt`。
- miniblink 本体按其自身许可使用，本仓库只链接 `mb132_x64.dll`，不修改也不分发其源码。
