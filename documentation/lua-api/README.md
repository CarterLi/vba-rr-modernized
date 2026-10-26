# Lua API 的类型定义（Lua Language Server）

`vba.lua` 是给 [Lua Language Server](https://luals.github.io/) 用的 `---@meta`
定义文件：装上之后，写 VBA-RR 的脚本时 `vba.`/`memory.`/`gui.` 会有补全、参数名和
返回值类型，写错参数类型或用了不存在的函数会当场标出来。

它不参与模拟器构建，也不会被模拟器加载。它的唯一作用是告诉编辑器
`src/common/lua-engine.cpp` 里那个 Lua 状态到底有什么。

```lua
---@meta
```

这个分支的脚本引擎是 **LuaJIT 2.1**：`lua51.dll` 在 `win32/lib/{x86,x64,arm64}/`，
构建时原样拷到 exe 旁边，语言层面仍是 Lua 5.1，另外多了 LuaJIT 的
`bit`、`jit`、`ffi`。手工把原版 Lua 5.1 的 `lua51.dll` 换上去也能跑（5.1 的 ABI 就是
LuaJIT 实现的那套），只是那三样没有了。

## 装法

编辑器认两种位置：**库目录**（`workspace.library`，对所有打开的文件生效）和**工作区
里任意一个 .lua 文件**（只对该工作区生效）。推荐库目录，因为 `vba.lua` 本身不需要被
当成脚本。

**A. 跟着脚本走（推荐）** —— 把 `vba.lua` 放进脚本目录下的一个子目录，脚本目录里加
`.luarc.json`：

```
C:\path\to\lua\
    macro.lua
    .luarc.json
    vba-api\
        vba.lua
```

```json
{
  "runtime.version": "LuaJIT",
  "workspace.library": ["./vba-api"]
}
```

**B. 全局装一次** —— 在 VS Code 的 `settings.json` 里给绝对路径，之后任何目录的脚本
都有提示：

```json
{
  "Lua.runtime.version": "LuaJIT",
  "Lua.workspace.library": ["C:/path/to/vba-rerecording/documentation/lua-api"]
}
```

**C. 直接拷到脚本旁边** —— 把 `vba.lua` 和脚本放同一个目录，再在这个目录里放
`{"runtime.version": "LuaJIT"}`。能用，但那个目录里的所有 .lua 都会被当成同一个
工作区，脚本之间互相可见。

> `runtime.version` 选 **LuaJIT**，和随附的 `lua51.dll` 对齐——这样 `bit.`、`jit.` 才有
> 提示。换成了原版 Lua 5.1 的 dll 就改成 **Lua 5.1**（此时 `bit` 不存在，编辑器也会这么
> 认为）。别选 5.2+：那会放行 `goto`、`//`、`&`、`|` 这些模拟器跑不了的语法。

## 覆盖范围

`vba.lua` 描述的是**这个分支实际注册的东西**，和 `lua-engine.cpp` 末尾的 `luaL_Reg`
表一一对应：

| 表 | 项数 | 说明 |
| --- | --- | --- |
| `emu` / `vba` | 20 | 同一张表，`vba` 是旧名字 |
| `memory` | 45 | 含 `gbrom*` 一组只读 GB ROM 的函数和全部别名 |
| `gui` | 26 | 画布固定 256x224 |
| `movie` | 15 | |
| `joypad` | 8 | |
| `savestate` | 3 | |
| `avi` | 3 | |
| `input` | 2 | |
| `sound` | 1 | |
| 全局函数 | 7 | `AND` `OR` `XOR` `SHIFT` `BIT` `addressof` `copytable` |

`bit` 不在这个文件里：它由引擎提供（LuaJIT 自带，原版 Lua 5.1 没有），`vba.lua` 不去
声明别人的库。`print` 和 `tostring` 同理，Lua 5.1 的标准库定义已经声明过（签名相同，
行为不同，见下面差异表），只把它们的行为写在文件末尾的注释里。

## 和 TASVideos 文档不一致的地方

上游文档（<https://tasvideos.org/EmulatorResources/VBA/LuaScriptingFunctions>）描述的是
一般语义，下面这些是这个分支的实际行为，`vba.lua` 按实际行为写：

| 项 | 文档 | 这个分支 |
| --- | --- | --- |
| `memory.readbyterange` | 返回 string | 返回**数组表**，`t[1]` 是 `address` 处的字节（`lua_createtable` + `lua_rawseti`）；`length` 为负时从 `address+length` 开始往后读 |
| `vba.speedmode` | 没有 | 有（`normal`/`nothrottle`/`turbo`/`maximum`），已接到帧跳过与节流，见下面一段 |
| `gui.fillbox` `gui.fillcircle` `gui.drawcircle` | 没有 | 已注册，见下面一段 |
| `joypad.get/set` 的 port | 可省略，默认 0 | **必填**（`luaL_checkinteger(L, 1)`），0 表示默认手柄，1-4 是四个口 |
| `memory.register*` 的回调 | `function()` | 收到 `(address, size)` 两个参数，一个钩子覆盖一段地址时能分辨是哪一字节 |
| `movie.author()` / `movie.name()` | 无电影时 nil | 无电影时返回 `""`（`movie.mode()` 才是 nil） |
| 颜色的数字形式 | — | `0xRRGGBBAA`，**alpha 在低字节**，和 `"#RRGGBBAA"` 字符串同序 |
| `gui.parsecolor` 的 alpha | — | 会被当前的 `gui.opacity`/`gui.transparency` 缩放后再返回 |
| `gui` 画布 | 239 高 | 256x224（`LUA_SCREEN_WIDTH/HEIGHT`），超出部分直接丢弃 |
| `gui.getpixel` | — | 读的是**画面**，不是 gui 画布，越界返回 `0,0,0` |
| `vba.message` | "显示在屏幕上" | 状态栏可见时**只**进状态栏，画面一个字都不画；画面上用的内置点阵字体只有 ASCII，含非 ASCII 的整行跳过 |
| `input.get()` | — | 只在 Windows 前端有实现，别的平台返回空表 |
| `math.random` / `math.randomseed` | C `rand()` | SFMT，**每个脚本一个生成器**。原版 Lua 5.1 的 `rand()` 在 MSVC 上只有 15 位（`RAND_MAX` 32767），`math.random(1, 100000)` 最多只能返回 32767 个值；签名不变，所以这里没有重新声明 |
| `bit` 库 | “bitop 模块的全部函数” | 就是 LuaJIT 的 `bit`；引擎不再自带一份 LuaBitOp，原版 Lua 5.1 的 dll 下没有 `bit` |

文档没写但确实存在的：`emu` 这个别名、`memory.registerread`、`memory.gbrom*`、
`memory.gbromreadbyterange`、`vba.registerrun/registerclose/registerloading/
registerloaded/registersaving/registersaved`、`vba.screenshot`、
`vba.remapinputdisplay`、`movie.framecount`、`sound.get`、`avi.pause/resume`。

三个多出来的 `gui` 绘图函数：

```lua
gui.drawcircle(x, y, radius, color)        -- 空心圆，中点算法
gui.fillbox(x1, y1, x2, y2, color)         -- 纯填充矩形，没有边框
gui.fillcircle(x, y, radius, color)        -- 实心圆
```

它们的 C 实现一直在（`gui_drawcircle`/`gui_fillbox`/`gui_fillcircle`），但 2026-10-26
之前没有登记进 `guilib`，等于调不到；现在登记了。名字沿用 Snes9x 的 `gui` 库（VBA 的
这一套就是从那儿来的），TASVideos 的 VBA 页面从来没有列过它们，所以不算“和文档不一致”。

`vba.speedmode(mode)` 现在**已注册**（`normal` / `nothrottle` / `turbo` / `maximum`）。
它原来是个空操作：`vba_speedmode()` 写得对，但没有任何东西读 `state->speedmode`，
`VBALuaSpeed()` 全工程无人调用、里面还留着 `assert(false)`。现在：

- `VBALuaSpeed()` 返回「要跳几帧」（-1 = 没有脚本要求；0 = 不跳；9 = 快进键的比例；
  `0x7FFFFFFF` = 一帧都不画），多个脚本取最快的那个。
- `systemFramesToSkip()` 消费它：录音中 / 必须绘制的帧仍然优先，其余情况下和用户设置取
  `max`，所以按住快进键照样更快。
- `systemFrame()` 与两个音频后端（DirectSound / XAudio2）在脚本拿走速度时不再按实时时钟
  节流——**这条是必须的**：同步开着的时候，声音缓冲区就是节流器，只改跳帧数根本不提速。
- 多个脚本同时要求速度时**取最快的那个**（`normal` < `nothrottle` < `turbo` < `maximum`），
  和手柄按位或、画布按载入顺序叠是同一条规矩；某个脚本停止、自然结束、或被重新 Run 时，
  它自己那份要求就撤销（重新 Run 会把 `speedmode` 复位成 `normal`），不会卡在高速上下不来。
- 已知限制：vsync 开着时 `nothrottle` 仍被刷新率卡住（`Present(1)`），`turbo`/`maximum`
  不受影响（本来就不按刷新率出帧）。

## 装好之后是什么样

```lua
local function hud()
  -- gui.text 的第三个参数是 any（内部走 tostring），颜色可以是名字/数字/表
  gui.text(4, 4, "frame " .. vba.framecount(), "yellow", { r = 0, g = 0, b = 0 })

  local pad = joypad.get(0)        -- VbaButtonTable，pad.A / pad.B / pad.start ...
  if pad.A then
    memory.writeword(0x02037368, 0x1234)   -- 参数类型是 integer，写字符串会标红
  end
end

gui.register(hud)

vba.registerbefore(function()
  local bytes = memory.readbyterange(0x02000000, 4)  -- integer[]，不是 string
  print("first byte", bytes[1])
end)

while true do vba.frameadvance() end
```

## 维护

改 `lua-engine.cpp` 里的 `luaL_Reg` 表（加/删/改名一个函数）之后，必须同步改
`vba.lua`，然后跑一次：

```
python documentation/lua-api/check_api.py
```

它从 `lua-engine.cpp` 里抽出各 `luaL_Reg` 表的函数名（跳过被注释掉的项）和
`lua_register` 的全局名，与 `vba.lua` 里声明的名字逐表对比，任何一边多了或少了都
返回非零退出码并打印差集。和 `src/win32/lang/make_rc.py --check` 是同一个套路。

`vba.lua` 本身是合法 Lua（`luajit -e "assert(loadfile('vba.lua'))"` 能过），但注解
写得对不对只能靠 `lua-language-server --check` 看：把 `vba.lua` 放进一个工作区，写
一个故意用错参数的脚本，确认它会报 `param-type-mismatch` / `undefined-field`；
`---@meta` 文件自己不参与诊断，所以“文件里没告警”不能当成注解正确的证据。
