# browsershell — Agent 入口

MiniShell 是 miniblink 132 的桌面浏览器外壳（`src/` 下的 `agave_webview_*` 适配层 +
引擎无关的自绘 UI）。本文件只放**测试纪律**和**最容易踩的坑**，功能说明见 `README.md`。

## 提交纪律

**每轮改动编译通过、headless 检查通过后，直接提交**（用户明确要求"下次改了记得提交"）。
仓库根在工作区顶层（`git -C <repo> status`），不要在 `%TEMP%` 里做临时仓库。
提交信息用英文、写清"改了什么 + 为什么"，一个主题一条，不要攒一堆无关改动。

## 测试纪律（最重要）

**所有测试都必须是 headless probe，绝不碰桌面。** 这是硬性要求，不是偏好：

1. **不启动 GUI 壳做测试**。`minishell.exe --probe <url> --eval "probeExit(...)"`，退出码是权威判据
   （0 OK / 1 usage / 2..4 超时或无 exit / 5 加载错误 / 6 空文档）。
2. **不移动真实鼠标、不 `SetForegroundWindow`、不抓桌面截图**。`SetCursorPos` / `mouse_event` /
   `SendInput` / `CopyFromScreen` 一律禁止：用户随时在用桌面，这些操作会抢他的指针和焦点。
3. **需要像素**用 `tools/screenshot.ps1`（= `--probe --shot`，抓控件自己的 mirror buffer，
   窗口从不 `ShowWindow`）。能用数字判断的就不要截图。
4. **需要看 UI 渲染**用 `--ui-shot`（chrome 离屏渲染），不要开窗口。
5. **UI 相关测试偶发失败必须二次确认**：用户可能正在操作桌面影响结果；连续两次才算失败。
6. **测试脚本放 `tools/`，不要放 `%TEMP%`**；高频使用的沉淀成可复用脚本。
7. **测试要快**：新增用例尽量 < 15s，本地文件页不要用大 `--wait/--settle`。
8. **能力缺口要如实记录**：某些行为（真实右键、焦点转移、宿主覆盖窗口）在 probe 里测不到，
   要么加探针能力，要么明确写"只能人工验证"，不要假装测过。

## 现有工具

| 脚本 | 用途 |
|---|---|
| `tools/probe.ps1` | headless 探针：`-Url -Eval -EvalFile -Inject -Wait -Settle -Timeout [-PassThru]` |
| `tools/screenshot.ps1` | headless 截图：`--probe --shot [-Rect x,y,w,h]` |
| `tools/inspect-check.ps1` | 元素标记（inspect）页面半边的回归：注入 `assets/inspect.js`，断言上报的 rect |
| `tools/inspect_hang.ps1` | 卡死时挂 cdb 抓全部线程栈（只读，摘下后恢复） |
| `tools/layered-dib-check.ps1` | 验证 `UpdateLayeredWindow` 的硬性条件：源 DC 里必须还选中着那块 DIB（未选中 → 失败 err=31） |
| `tools/settings-check.ps1` | `minishell://settings` 的双向协议回归：页面请求 → 宿主回值 → 断言主题/搜索引擎/缩放/版本/下载目录都到位 |
| `tools/zoom-check.ps1` | 页面缩放真的放大页面：每个 zoom 的着色高度 = 40px × zoom × 显示缩放，右边缘不被裁；dpr/innerWidth 仍按显示缩放 |
| `tools/dlserver.ps1` | 下载/新窗口回归用的本地 HTTP 服务 |
| `tools/hangserver.ps1` | 只 accept 不响应的 TCP 服务，用来触发"加载永不提交"和导航守卫 |
| `tools/navguard-check.ps1` | 导航守卫销毁在途控件（请求随后失败）不再把引擎带崩：隐藏壳 + 6 秒后关闭服务端，12 秒仍存活 |

崩溃链路（报告 → 崩溃页）可以 headless 验证，不要开窗口：
`AGB_TEST_CRASH=1 AGB_CRASH_POPUP=0` 只写 `agb_crash.txt`；
`AGB_SHELL_HIDDEN=1 AGB_CRASH_PAGE_SAVE=1 minishell.exe --crash-page <report>` 渲染崩溃页、
把 HTML 存到 `%TEMP%\MiniShell-crash-page.html` 后自动退出。报告必须保持无隐私
（模块名/偏移，不要路径和 URL）；崩溃页是独立进程，因为它可能正是坏掉的那部分。

## 引擎侧已知坑（会静默失败）

- `mbSetContextMenuEnabled(FALSE)` 时引擎**吞掉**右键，`WM_CONTEXTMENU` 不会送到宿主；
  右键菜单从控件自己的 `WM_RBUTTONUP` 触发（`mb_view.c`）。
- `mbInit(NULL)` 不读 Windows 代理；`mb_view.c` 用 `WinHttpGetIEProxyConfigForCurrentUser` 显式喂给引擎，
  否则强制代理网络下所有导航静默无响应。
- 首次导航提交前不能 `mbReload`（会丢掉在途加载且不重发）。
- 引擎与宿主的 `on_message` 回调在 blink 线程，必须 `postEvent` 到自己窗口再在 UI 线程处理。
- 页面里的覆盖层（inspector.js 那类）会撑出滚动条；元素标记改由宿主窗口绘制（`ui_inspect.c`），
  页面只上报 `getBoundingClientRect()`。标记窗口只画那个框，元素信息显示在左下角状态气泡
  （`updateStatus`）：贴着元素画标签会被元素自身尺寸裁掉，1x1 的元素等于什么都看不到。
- **`mbSetZoomFactor(f)` 会把缩放画两次**：实测 40 CSS px 的条在 125% 屏上，f=1.25 时
  dpr 报 1.25 但着色 62px（1.5625），页面比窗口大、右/下被裁；f=1 还会把显示缩放丢掉。
  页面缩放因此改成注入 CSS `zoom`（`mb_view.c` 的 `viewApplyEngineScale`），dpr/innerWidth
  保持显示缩放，页面只放大一次。`tools/zoom-check.ps1` 锁的是这个行为。
- **请求在途时 `mbDestroyWebView` 会崩**：销毁一个还在导航的控件后，网络完成回到已释放的
  webview，引擎在 `mb132+0x5e666f` 读 `[0xF0]`（read 0）直接 AV；导航守卫"销毁并立即重建"
  正好踩中（`tools/hangserver.ps1 -CloseAfterMs` 可 6 秒稳定复现）。适配层的
  `agave_webview_destroy` 因此把在途控件隐藏、等 loading-finish 再真正销毁
  （`destroyPending` + `MBV_EV_FINALIZE`），宿主拿到销毁后不能再碰 view。
- **webview 上方不要放任何窗口**：哪怕是一个 1/255 alpha 的分层子窗口，只要盖在页面控件上，
  resize 后页面就不再重绘（停在旧帧）。
- **`UpdateLayeredWindow` 的源 DC 里必须还选中着那块 DIB**：先把 bitmap `SelectObject` 还原再调用，
  会失败并返回 err=31（`ERROR_GEN_FAILURE`），窗口就"消失"了（状态泡泡踩过）。
  `tools/layered-dib-check.ps1` 可以把这条规则单独复现：选中=成功，未选中=31。
- **layered child 不可用**：本机对 `WS_CHILD + WS_EX_LAYERED` 调 `UpdateLayeredWindow` 返回 87，
  只能做成 owned popup（创建时带 `WS_EX_LAYERED` 的 popup 是正常的）。
- **`WH_MOUSE_LL` 钩子返回 1 会冻住鼠标**：低层鼠标钩子的返回值是"丢弃该事件"，对 `WM_MOUSEMOVE`
  返回非 0 等于把光标的移动也丢掉，鼠标就动不了了（本机实测锁死）。输入拦截必须走控件自己的消息路径
  —— `agave_webview_set_input_capture`（`mb_view.c` 的 `mbvViewProc`），光标照常移动、页面只是收不到消息。

## 构建

```powershell
# 先杀掉残留进程：正在运行的 minishell.exe 会锁住 exe，链接报 LNK1104
Get-Process minishell -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
cmake --build build-msvc --config Release
```

**只允许 `build-msvc`，禁止另建构建目录。** 用户测试的就是
`build-msvc\Release\minishell.exe`；用 `build-check`、`build-x` 之类的目录编出来的 exe
和用户手上的不是同一个文件（时间戳、复制过去的 DLL/脚本都可能不同），测试结论就对不上。
exe 被占用时先按上面的命令杀进程，不要换目录绕开。

编译完必须确认输出行是 `build-msvc\Release\minishell.exe`，再跑 `tools/` 里的脚本（它们固定用这个路径）。

改 DLL 后重生成导入库：`pwsh -File tools\gen_import_lib.ps1`。
