# Lua：一个窗口一个脚本

2026-10-26 起，Lua 控制台可以同时开多个窗口，每个窗口跑自己的脚本。
本文记录这么做的结构、几条合并规则，以及改这块代码时不能踩的坑。

涉及文件：`src/common/lua-engine.cpp`、`src/common/vbalua.h`、
`src/common/LuaHost.h`、`src/win32/LuaHostMFC.cpp`、
`src/win32/Dialogs/LuaOpenDialog.{h,cpp}`、`src/win32/MainWndFile.cpp`。

## 结构

- `class VbaLuaState`（lua-engine.cpp 顶部）持有原来散在文件里的所有静态量：
  `lua_State *L`、脚本名、控制台句柄、`running`、`frameBoundary`、
  `frameAdvanceWaiting`、`numTries`、`speedmode`、`skipRerecords`、
  `transparency`、`joypad[4]`/`joypadUsed`、`inputRemap[4]`、
  `guiUsed`/`guiData`、`memHooks`/`memRegions[]`、`rng`（SFMT）。
- `g_luaStates` 是活着的脚本列表，**顺序 = 载入顺序**，也是每帧回调的执行顺序。
  想让同一组脚本每次跑出一致的结果，就不要在别处改变这个顺序。
- 一个窗口一个实例：窗口创建 → 用户按 Run → `VBALuaOpenScript(路径, 控制台句柄)`；
  关窗口 → `VBALuaCloseScript()`（停 + 释放）。
- `VbaLuaState::fromLua(lua_State *)` 从 registry 里取 `this`
  （键是 `&s_luaStateKey` 这个地址，不哈希字符串）。协程线程也能取到，
  因为线程共享创建它的那个 state 的 registry。**每个 C API 函数靠它找自己的脚本**，
  所以任何新加的 `vba.*` 函数要先 `fromLua(L)`，别再碰全局量。

## 三处必须合并的东西

| 资源 | 规则 | 位置 |
| --- | --- | --- |
| 手柄 | `joypad.set` 只写自己那格；读的时候按位或 | `mergedLuaJoypad()`、`VBALuaUsingJoypad`/`VBALuaReadJoypad` |
| 画面 | 每个脚本一张 224×256 画布，按载入顺序依次叠到帧缓冲 | `VbaLuaState::guiFrame()`、`VBALuaGui()` |
| 内存钩子 | 全局 `hookedRegions` 是所有脚本的并集（先做一次快速否决），命中后再逐脚本查自己的 `memRegions` | `CalculateMemHookRegions()`、`CallRegisteredLuaMemHook()` |

手柄为什么必须合并：`joypad.set` 是**整块覆盖**（先 `*buttons = 0` 再逐位设），
分两次调后一次会把前一次清掉。所以谁都不能直接写共享的 `lua_joypads`。

## 出错与停止

- 回调里出错 → `HandleCallbackError()` → `KillScriptAfterError()`：清掉这个脚本的
  9 个回调槽、gui 槽和它的协程引用，`memHooks = 0`，再 `info_onstop` 把窗口的
  按钮变回 "Run"。**不在这里 `lua_close`**——调用者还站在这个 `lua_State` 上，
  关了就是 use-after-free。真正的 `lua_close` 在 `stop()`（窗口关上 / 重新 Run 时）。
- `stop()` 会先跑 `beforeexit` 回调，再清钩子、`lua_close`、`resetAfterStop()`。
- 自然死亡（脚本顶层返回、没有 `while true` 循环）沿用老行为：`running=false`，
  但已注册的回调**继续生效**，这正是 `macro.lua` 那种"只挂 registerbefore"的脚本
  能工作的原因。`frameBoundaryStep()` 只在 `running` 为真时才 resume 协程。

## 前端

- `LuaConsoleHWnd` 这个单例没了。`LuaOpenDialog.cpp` 里是
  `std::vector<LuaConsoleWindow *> s_consoles`，每个元素记 `hDlg`、`script`、
  字体、窗口尺寸和控件布局状态（原来那份 `windowInfo` 是文件级全局，两个窗口
  一起改大小会互相踩，现在每窗口一份）。
- 窗口用 `GWLP_USERDATA` 找自己的 `LuaConsoleWindow`；`CreateDialogParam` 的
  lParam 传序号，只用于错开 24px 摆放。
- 菜单 `New Lua Script Window...` **每次真的新建**一个；`Close All Script Windows`
  给所有窗口发 `WM_CLOSE`；主窗口 `PreTranslateMessage` 依次
  `IsDialogMessage` 每一个控制台（`LuaWindowStepMessage`）。
- 把 `.lua` 拖到主窗口 = 新建窗口 + 填路径 + 按 Run（`LuaWindowRunScript`）。
- `lua_host::openConsole()` 已删除：控制台由窗口自己创建，句柄随
  `VBALuaOpenScript()` 传进来，`LuaHost.h` 只留三个 sink。

## 已知限制（不是 bug，改之前先想清楚）

- `chdir` 仍然在载入时按**文件名**切一次进程当前目录，多脚本时最后载入的那个赢；
  `dofile("lib.lua")` 这种相对路径会跟着串。要彻底解决得给每个脚本一份
  "按脚本目录解析"的 `dofile`，目前没做。
- `movie.rerecordcounting` 取"任一脚本要求跳过"；`speedmode` 取最快的那个；
  输入显示重映射取第一个真正设置过的脚本。三个都是"谁更激进听谁的"。
- `print`/`printerror` 的输出按**脚本自己的编码**解码：`.lua` 文件开头有 UTF-8 BOM
  就按 UTF-8，否则按进程 ANSI 代码页。**不做编码探测**——`.lua` 没有别的标记，
  字节本身也分不出来（GBK 的「中文」`D6 D0 CE C4` 同样是合法 UTF-8），
  猜错就是把本来正常的脚本变成乱码。想要中文输出就存成 **UTF-8 with BOM**。
  编码随 sink 传（`lua_host::TextEncoding`），因为两个窗口可能一个 UTF-8 一个 ANSI。

## 验证手段

探针脚本都放在仓库外的临时目录里，不入库，下面只写文件名。

- `build_check.py`：179 个 TU 全量 `/c`，要求 0 诊断。
- `link_check.py`：按工程库表链接，出 `VBA_check.exe`。
- `luashot.cpp`：真启动 exe，连按三次
  "New Lua Script Window"，核对窗口数 = 3、关掉中间那个剩 2、Close All 后剩 0、
  进程始终活着，并把每个窗口 PrintWindow 存成 BMP。
- `enc_probe.py`：把 `lua-engine.cpp` 里的 `hasUtf8Bom()`/
  `readWholeFile()` **原文抠出来**编译，跑 13 个真文件（BOM / 无 BOM UTF-8 / GBK /
  两字节 BOM 前缀 / BOM 不在开头）+ 三条 `MultiByteToWideChar` 平台断言。
- `bom_probe.py`：同样抠 `loadScriptFile()`，链真 `lua51.dll`，
  验 BOM 被丢掉（含 `#` 首行、空文件、CRLF、出错时 chunk 名仍是文件名、栈上只留一项）。
