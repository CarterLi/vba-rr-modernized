# 帧插值：PPU 逐行状态重放（方案二 + 方案三）

> 2026-11-06。目标：不改核心时序、不影响录制与验证语义，把 59.7275 fps 的 GBA 画面
> 补成 2 倍帧率输出。
>
> **状态：S1（相机档）与 S2（精灵档）已实现并验证，S3（录制插帧）未做。**
> 实现状态、验证记录与踩到的坑见 §14。

## 0. 结论

- 做：**逐行状态环 + PPU 重放**。两档：相机档（只插 BG 滚屏与仿射参考点）、
  相机 + 精灵档（再插 OAM 的 X/Y）。方案三就是相机档，不单独实现。
- 不做：shader 块匹配（1px 级瑕疵，且只有 3 个后端有位）、RunAhead（不解决插帧延迟，
  且会把 `systemFrame()` 的音频/AVI 推两遍）、跳帧时插帧（用户 2026-11-06 定：跳帧开着就
  强制关插帧）、外推档（接口留位）。
- present 只接 D3D11 与 OpenGL 3.3 两个后端，其余后端菜单项灰掉。
- 插帧是**纯显示层旁路**：不写 `pix`、不动帧计数、不进存档、不碰 JIT。判据见 §10。

## 1. 为什么不是"平均寄存器再画一次"

GBA 的帧没有可重入的 `Render()`：它在**模拟过程中逐行画**。

```c
// src/gba/V7/GBA.cpp:3966-3975（V8 同构，搜 systemFrameDrawingRequired）
if (systemFrameDrawingRequired())
{
    (*renderLine)();      // 画第 VCOUNT 行，读的是此刻的 BGxHOFS / OAM / ioMem
    CPUDrawPixLine();     // lineMix -> pix 的第 VCOUNT+1 行
}
```

所以"拿中间数据再画一次"的前提是**先把这一帧每一行用的状态存下来**。好消息是重放本身
干净：`mode0RenderLine()` 之类（`Mode0.cpp:4-37`）是对一组全局变量的纯函数，
仿射层还专门留了口子——`gfxDrawRotScreen(..., int &gfxBG2X, int &gfxBG2Y, int changed, ...)`
（`GBAGfx.h:270-277`）的 `changed` 参数就是"这一行用寄存器绝对值还是用累进值"。

⇒ 重放不需要改 PPU，只需要**按行喂值**。这就是整个方案的地基。

## 2. 生效条件

插帧是否生效，每一帧现算，不缓存：

| 条件 | 依据 / 说明 |
|---|---|
| 开关 != Off | 菜单 `Options → Video → Frame Interpolation`，或 ini `frameInterpolation`，或 `-interp 0\|1\|2` |
| `theApp.display->supportsInterpolation()` | 只有 D3D11 / OGL3.3 返回 true，见 §8 |
| `systemFramesToSkip() == 0` | **跳帧就关**。注意录制时这个函数本来就返回 0（`WinSystem.cpp:963`），所以录制不受用户的跳帧设置影响，正好是想要的 |
| `!speedup && !theApp.frameSearchSkipping` | Tab 加速、TAS 帧搜索 |
| `!systemPausesNextFrame()` | **逐帧前进强制关**（`WinSystem.cpp:1142`；`OnDebugNextframe` 置 `winPauseNextFrame`，`MainWndTools.cpp:196-200`） |
| `!theApp.paused` | |
| 上一帧也画过（环是连续的） | 否则本帧只 present 真帧 |

`interpReset()` 要清环并跳过一帧，触发点：

- 开关 off → on
- 暂停 → 播放、逐帧前进之后恢复
- 跳帧设置变化（`systemFramesToSkip()` 的结果变了）
- 切 ROM / `systemReset()` / `emuReadMemState()` / 停止录制
- 后端切换

清环后**第一帧只 present 真帧**，不要用陈旧环算中间帧。

## 3. 状态环：抓什么

```c
// src/gba/GBAGfx.h 附近新增；每帧 160 条 × 56 B ≈ 9 KB，双缓冲 18 KB，静态分配
struct GfxLineState
{
    u16 dispcnt;                 // 0x00
    u16 bldmod, colev, coly;     // 0x50 / 0x54 / 0x52
    u16 mosaic;                  // 0x4C
    u16 win0h, win1h, win0v, win1v, winin, winout;   // 0x40..0x4A
    u16 bgcnt[4];                // 0x08..0x0E
    u16 bghofs[4], bgvofs[4];    // 0x10..0x1E
    u16 bgpa[2][4];              // BG2PA..BG3PD
    s32 bgx[2], bgy[2];          // gfxBG2X/Y、gfxBG3X/Y（PPU 自己累进的运行值）
    u16 vcount;                  // 重放时喂回去，见 §4
    u8  changed;                 // bit0 = gfxBG2Changed, bit1 = gfxBG3Changed
};
```

捕获点：`V7/GBA.cpp` 与 `V8/GBA.cpp` 里 `if (systemFrameDrawingRequired())` 的
`(*renderLine)();` **之前**，抓 56 字节 memcpy（每帧约 9 KB，可忽略）。

不抓的东西：VRAM / palette / OAM 的内容。重放用**当前**内容（即帧 B 的贴图与调色板），
这也是为什么换贴图不需要任何降级——位置插值、贴图取 B，画面是锐的。

## 4. 重放

```c
// 新增，放在 GBAGfx.cpp；dst 与 pix 同布局：4*241*162，行放在 241*(y+1)
void gfxReplayFrame(u8 *dst, const GfxLineState *a, const GfxLineState *b, int t /*0..256*/);
```

步骤：

1. 备份所有会被改写的全局（清单见下）。
2. `gfxLastVCOUNT = 0;` `gfxBG2Changed = gfxBG3Changed = 0;`（见坑 2）。
3. 精灵档时把 `oam` 指向临时副本并改 X/Y（见 §5），否则不动。
4. 逐行 `y = 0..159`：
   - 把 §3 的字段按 `t` 插值后写进全局（`layerEnable`、`windowOn`、`fxOn` 要按
     `GBA.cpp:2521-2522 / 2706` 的同一算式重算）；
   - `VCOUNT = y`；
   - `(*renderLine)();`
   - 把 `lineMix` 转成 `dst` 的第 `y` 行。
5. 还原全部全局与 `oam` 指针。

**必须备份/还原**：`DISPCNT`、`BLDMOD`、`COLEV`、`COLY`、`MOSAIC`、`WIN0H`/`WIN1H`/
`WIN0V`/`WIN1V`/`WININ`/`WINOUT`、`BG0CNT`..`BG3CNT`、`BG0HOFS`..`BG3VOFS`、
`BG2PA`..`BG2PD`、`BG3PA`..`BG3PD`、`gfxBG2X`/`gfxBG2Y`/`gfxBG3X`/`gfxBG3Y`、
`gfxBG2Changed`/`gfxBG3Changed`、`gfxLastVCOUNT`、`layerEnable`、`windowOn`、`fxOn`、
`VCOUNT`、`oam`。

> `BG0HOFS` 这些是**真变量**不是宏：`UPDATE_REG` 才写 `ioMem`，所以只改变量、事后还原，
> `ioMem` 不会被污染，游戏读到的东西不变。这是重放能做到"只读"的关键。

行转换复用 `CPUDrawPixLine()`（`V7/GBA.cpp:3533`）：把它拆成
`CPUDrawPixLineTo(u32 *destRow)`，真帧与重放共用，`systemColorMap16/32` 一致 ⇒
颜色与现在逐像素相同。

### 三个坑

1. **`VCOUNT` 必须按行设。** `gfxDrawSprites()` 用 `VCOUNT` 判精灵是否落在这一行
   （`GBAGfx.h:847`）。不设会把整屏精灵都画在第 159 行的位置上。
2. **仿射要预置 `gfxBG2X/Y` 并让 `changed == 0`。** 若传 `changed != 0`，
   `gfxDrawRotScreen` 会用 `BG2X_L/H` 里的绝对值覆盖你喂的值。另外 `Mode1.cpp:31`
   有 `if (gfxLastVCOUNT > VCOUNT) changed = 3;` 的换帧回绕判定，重放前把
   `gfxLastVCOUNT` 置 0 才能让每行都走 `changed = 0`。
3. **行尾 over-read 像素**：`pix` 是 241 宽（240 可见 + 1 个给滤镜读的越界像素），
   顶部还有一行 border（`WinSystem.cpp:307/322`）。`pix2` 必须按
   `4*241*162` 分配、行偏移 `241*(y+1)`，否则开滤镜时边缘会串行。

## 5. 插值规则与守卫

`t = 128` 是中间帧；`t` 参数留着，以后做外推档时传"只有 A"的形态。

### 相机档

| 字段 | 处理 | 守卫（超出就取 B，不插） |
|---|---|---|
| `BG0HOFS`..`BG3VOFS` | 16 位环形差分后线性插值 | `\|Δ\| > 64` px |
| `gfxBG2X/Y`、`gfxBG3X/Y` | 线性插值（内部单位 16 = 1 px） | `\|Δ\| > 128` px |
| `BG2PA..PD`、`BG3PA..PD` | **不插**，取 B（旋转/缩放插值留到以后单独一档） | — |
| `MOSAIC`、窗口、混色、`BGxCNT` | 取 B | — |

### 精灵档（在相机档之上）

逐条比较 OAM 的 128 项（`oam` 是 `u8*`，1 KB）：

| 情况 | 处理 |
|---|---|
| `(a0 & 0x0300) == 0x0200`（禁用）或 A/B 都禁用 | 取 B，不动 |
| `attr0/attr1/attr2` 的**非坐标位**在 A/B 间不同（换贴图块、翻转、尺寸、模式） | 该精灵取 B 的整条记录 |
| X 差（9 位环形）或 Y 差（8 位）绝对值 > 64 | 该精灵取 B（防 OAM 轮转、复活传送、隐藏到 0/240） |
| 其余 | `x' = lerp(a.x, b.x, t)`，`y'` 同理，写进临时 OAM 副本 |

### 整帧降级

出现下列任一情况，**整帧不插**，直接 present 真帧（并可在日志计数）：

- `(A.dispcnt ^ B.dispcnt) & 0x0007`（显示模式变了）
- `A.dispcnt & 0x80`（强制白屏）
- 环里 A 的那一帧不是相邻帧（中途暂停、跳帧、切 ROM）

## 6. 呈现：双缓冲与 2× present

- 新增 `u8 *pixToPresent`（默认 `pix`）。D3D11 / OGL3.3 的 `render()` 里把
  `u8 *data = pix;`（`Direct3D11.cpp:2061`、`OpenGL3.cpp:2320`）改成
  `u8 *data = pixToPresent ? pixToPresent : pix;`。其余后端一行都不改。

`systemRenderFrame()`（`WinSystem.cpp:519`）拆成两段：

```
帧事务（每帧一次）：OSD 文字 -> AVI 真帧 -> 状态栏
插帧分支：
    buildInterp()                 // 重放到 pix2
    recordFrame(pix2)             // 先插帧帧
    paceTo(half)
    present(pix2)                 // pixToPresent = pix2
    recordFrame(pix)              // 再真帧
    paceTo(full)
    present(pix)                  // pixToPresent = pix
```

节拍：现有 `systemFrame()` 把"帧首到帧首"钉在 `100000 / (frameRate * throttle)` µs
（`WinSystem.cpp:905`，GBA 约 16742 µs）。插帧时把它拆成两个 deadline（半帧 / 整帧），
复用同一个 `sleepAmt` 平滑器。

🔴 **vsync 与音频**：`Present(1)` 每次阻塞一个 vblank，一帧阻塞两次会把音频缓冲抽干
——`Direct3D11.cpp:2325-2330` 的注释已经记着这个坑（"starves the audio buffer and is
heard as crackling"）。所以**插帧时 present interval 固定为 0**，节拍交给软件 sleep；
代价是可能撕裂。窗口模式下 DWM 会合成，撕裂基本不可见，但 DWM 本身再加约 1 帧延迟
（见 §12）。

🔴 **59.7275 vs 120 的漂移**：`262144/4389` 对 120 Hz 是 2.0091，固定 2:1 每秒少
0.4545 帧，vsync 下约每 1.8 秒顿一次。用一个累加器偶尔多等半个 vblank，不要写死 2:1。

## 7. 导出视频（已实现，只接 MF）

录制与游玩共用同一套 `pix2`，只是每模拟帧多写一次 `AddFrame`。

| 项 | 落地方式 |
|---|---|
| 触发 | **跟随插帧设置**，不另设开关。录制对话框打开时问一次 `systemInterpolationSupported()` |
| 导出帧率 | 原生 × 2 = `524288/4389` ≈ **119.4547 fps**（整数有理数，不漂移） |
| MF 头 | `createWriter()` 里 `fpsNumerator = systemGetFrameRateDividend() * 2`（`MFVideoWriter.cpp:220`） |
| 参数 | `RecordParams::interpolated`（`VideoWriter.h:69`）+ `IVideoWriter::wantsInterpolatedFrames()` |
| 帧序 | 每模拟帧先写插帧帧、再写真帧 |
| 音频 | **完全不动**。`linearFrameCount` 仍按"模拟帧"计数，`AddSound` 的时间戳只看采样数 |
| 老 AVI 菜单 | **没做**。`AVIWrite` 用接口默认的 `false`，仍是 59.73 fps |

**帧数必须严格 2×，一次都不能少。** 时间戳是 `framesToTime(submittedFrames, ...)`，
少写一张视频就短 8.4 ms，而且每失败一次就多累积一次，最后画面跑到声音前面。所以
`systemBuildInterpolatedFrame()` 返回假（换场景、模式切换、环没填满、暂停）时，
`systemRecordAviFrame()` 收到的是 `pix` 本身——**真帧写两遍**，时间轴照样对。

`systemRecordAviFrame()` 的 catch-up 循环（丢帧补偿）每轮写同一对 (插帧, 真帧)：
语义是"这一模拟帧的两张图"，重复写等于复制帧，时间轴不变。

录制对话框：插帧时帧率框只剩 `Auto` 并且灰掉，摘要行直接写 `119.45 fps`。
**"插帧 + 指定 60 fps" 是自相矛盾的组合**——按 `videoFrameStep()` 的逻辑会隔一张丢一张，
而且丢的是哪一半取决于相位。

两个副作用要一起处理：

- **OSD 文字**：`textMethod 0` 是把文字烧进 `pix` 的（`WinSystem.cpp:690-697`）。
  插帧帧也必须画一次，否则导出的视频里字幕会在 120 Hz 下闪。用 `DrawTextMessages()`
  再画一次；`systemRenderLua()` 有副作用，**不要在插帧帧调用**。
  `textMethod 1/2` 在 present 阶段画，不需要处理。
- **CPU 滤镜跑两遍**：`captureScaled` + xBRZ/HQ 时每帧要出两张图，滤镜成本翻倍
  （1280×960 的 xBRZ 可能撑不住）。见 `recording-settings-design.md` §10.6；
  插帧录制建议用 GPU 后端、关 CPU 滤镜。

**Interframe blending 必须互斥**：`ifbFunction`（`VBA.h:185`）本身就是时间域混合，
和插帧叠加只会糊。开插帧时强制关掉，菜单项灰掉。

## 8. 选项、持久化、本地化

菜单：`Options → Video` 里新增 `Frame &Interpolation` 子菜单（`vba.rc:2142` 那个 POPUP
内），三个单选：

```
Frame &Interpolation
    &Off
    &Camera Only              (方案三：只插 BG 滚屏与仿射参考点)
    Camera + &Objects         (再加 OAM 的 X/Y)
```

- 单选组用 `MainWnd::setRadioCheck()`（`MainWndTools.cpp:1028`），**不要**用
  `CCmdUI::SetRadio()`。
- `IDisplay` 加 `virtual bool supportsInterpolation() { return false; }`
  （`Display.h`，照 `upscalerAvailable()` 的写法）；D3D11 / OGL3.3 返回 true。
  `OnUpdate...` 里对不支持的后端 `Enable(false)`，并在 `winlog` 里写一句为什么。
- ini 键 `frameInterpolation` = 0/1/2，进 `loadSettings`/`saveSettings`。
- 命令行 `-interp 0|1|2`。⚠️ 按既有行为，命令行覆盖会在退出时写回 ini，
  跑完测速要核对，别把用户的设置改掉。
- `theApp` 里加 `int frameInterpolation;`（放在 `VBA.h:296` 的 `upscaler` 一带）。

本地化：新菜单字面量先登记 `src/win32/lang/chs/menu.tsv`，改完 `vba.rc` 跑
`python src/win32/lang/chs/make_rc.py`，提交前 `--check`。漏登记会在 `--check` 里报
未翻译并退回英文。

## 9. 改动清单

| 文件 | 改动 |
|---|---|
| `src/gba/GBAGfx.h` | `GfxLineState`、`gfxReplayFrame()` 声明、`line*` 缓冲不变 |
| `src/gba/GBAGfx.cpp` | 状态环、`gfxReplayFrame()`、行转换的共用函数 |
| `src/gba/V7/GBA.cpp`、`src/gba/V8/GBA.cpp` | `if (systemFrameDrawingRequired())` 内加一行捕获（两个核都要，代码同构） |
| `src/common/System.cpp` / `System.h` | `systemInterpolationActive()`、`interpReset()`、录制帧倍率的查询入口 |
| `src/win32/WinSystem.cpp` | `pix2`/`pixToPresent`、`systemRenderFrame()` 拆分、`systemRenderScaledFrame()` 接受源缓冲、插帧帧的 OSD、半帧节拍 |
| `src/win32/VBA.h` / `VBA.cpp` | `frameInterpolation` 成员、ini、`-interp`、`interpReset()` 的触发点 |
| `src/win32/Direct3D11.cpp`、`OpenGL3.cpp` | 读 `pixToPresent`、`supportsInterpolation()` 返回 true、插帧时 interval 0 |
| `src/win32/Display.h` | `supportsInterpolation()` 默认 false |
| `src/win32/MainWnd.cpp`、`MainWndOptions.cpp` | 三个命令与 update UI、互斥逻辑（IFB 灰掉） |
| `src/win32/vba.rc`、`resource.h` | 菜单项与 ID |
| `src/win32/AVIWrite.cpp`、`MFVideoWriter.cpp`、`VideoWriter.h` | 帧率倍率 |
| `src/win32/Dialogs/RecordSettings.cpp` | 帧率框锁定与摘要行 |
| `src/win32/lang/chs/menu.tsv` + 生成物 | 本地化 |

## 10. 不变量（改的时候不能碰）

- **不写 `pix`**。`pix` 是 AVI（`WinSystem.cpp:500`）、截图、`filterFunction`、
  upscaler 的唯一来源，也是 `-hashtrace` 的 `full` 列的内容。插帧只写 `pix2`。
- **不动 `systemCounters.frameCount` / `soundFrameSound` / `linearFrameCount` 的语义**。
  录制只多一次 `AddFrame`，时间基靠帧率头表达。
- **不进存档**。`GfxLineState` 环不是模拟状态，`emuWriteMemState` 不包含它；
  读档后必须 `interpReset()`。
- **不碰 JIT**。重放不执行指令，只是一次 PPU 绘制，JIT 的块缓存与周期记账完全无关。
- **关掉插帧时行为必须逐字节不变**：默认 Off，且 Off 时连状态环都不捕获。

## 11. 验证

1. **核心未被污染**：`-hashtrace` 跑同一部电影，插帧开/关的 `ram` 列必须 **0 处不同**
   （`full` 列会因 present 顺序变化，按项目惯例只比 `ram`）。
2. **逐帧前进**：暂停后连按 20 次单帧前进，每次只能前进一帧、不得出现中间帧。
3. **跳帧**：`-frameskip 2` 下开插帧，应自动不插（日志一行说明即可）。
4. **录制**：`ffprobe` 查 `r_frame_rate` = 119.4547（或 524288/4389），时长与非插帧录制
   相差 ≤ 1 帧；音频轨长度不变。
5. **相机档**：马里奥/银河战士/恶魔城 横移——HUD 不动、精灵不粘背景、无 1px 抖动。
6. **精灵档**：找子弹/粒子多的场景（OAM 轮转）确认无"传送"；30 Hz 角色不得出现
   "两帧之间滑一段"（第一版就是会，见 §12，属已知局限）。
7. **音频**：vsync 开 + 插帧 10 分钟，不得爆音。
8. **后端**：GDI / DDraw / D3D9 / OGL1 / WGPU 下菜单项灰掉且不影响任何行为。
9. **跳过 A/B**：拖窗口、全屏切换、切滤镜、改缩放后不得崩溃或串帧。

## 12. 已知局限（诚实版）

- **延迟**：插帧要等帧 B 才能算 1.5，天生 +1 帧（16.7 ms）。这套模拟器没有独占全屏
  （全屏也是 borderless 窗口），DWM 合成再加约 1 帧 ⇒ 体感约 +2 帧（30~33 ms）。
  动作游戏能感觉到。要抹掉只能做外推档（§13 的 S3），那是另一件事。
- **30 Hz 的角色动画救不了**。很多 GBA 游戏只在隔帧写 OAM，插帧会把"跳变"变成
  "两个位置之间滑一段"，比不插更怪。第一版只保证"位置没变就保持"。
- **HBlank 逐行效果**（波浪水面、逐行滚屏）按行插值，两层之间可能出现轻微形变错位；
  可接受，但应当在日志里计数，方便判断某游戏是否该关掉。
- **场景切换 / OAM 轮转 / 精灵闪烁**会落到整帧或单精灵 fallback，表现为"这一帧没插"。
- **导出 120 fps 的兼容性**：部分硬件编码器对非整数高帧率支持不好。若出现编码失败，
  退回软件编码器，或只导出 AVI。

## 13. 阶段

| 阶段 | 内容 | 状态 |
|---|---|---|
| S1 | 状态环 + 重放骨架 + **相机档** + 选项/ini/命令行 + D3D11/OGL3 present 双缓冲 | **已实现**（2026-11-06） |
| S2 | **精灵档**（OAM X/Y + 四条守卫） | **已实现**（2026-11-06） |
| S3 | 录制插帧（MF 2× 帧率、对话框、OSD、帧数守恒） | **已实现（MF）**（2026-11-06）；老 AVI 菜单没做 |
| S4（暂不做） | 外推档：只用 A + 上一帧速度，零延迟；接口上就是 `gfxReplayFrame(..., t)` 只喂一帧 | — |

## 14. 实现记录（2026-11-06）

### 14.1 改动清单（实际落地的）

| 文件 | 改了什么 |
|---|---|
| `src/gba/GBAGfx.h` | `GfxLineState`、`gfxInterpolationMode`、`gfxCaptureLineState()`、`gfxResetLineStates()`、`gfxReplayInterpolatedFrame()`；另补 `fxOn` / `windowOn` / `renderLine` 的 extern（它们原先只在 `GBA.cpp` 里裸定义，没有头文件声明） |
| `src/gba/GBAGfx.cpp` | 状态环、捕获、重放、OAM 补丁、行转换 |
| `src/gba/V7/GBA.cpp`、`src/gba/V8/GBA.cpp` | `(*renderLine)()` 之前一行 `gfxCaptureLineState();` |
| `src/common/SystemGlobals.h` | `pixToPresent` |
| `src/common/System.h` | `systemInterpolationActive()` |
| `src/win32/WinSystem.cpp` | `pix2`、判定与同步、重放调用、OSD 补画、present 拆分、半帧节拍、两行诊断日志 |
| `src/win32/Display.h` | `supportsInterpolation()`，默认 `false` |
| `src/win32/Direct3D11.cpp`、`OpenGL3.cpp` | 读 `pixToPresent`、能力返回 `true`、插帧时 present interval 固定 0 |
| `src/win32/VBA.h` / `VBA.cpp` | `frameInterpolation`（放在类末尾，见 14.3）、ini、`-interp`、帮助文本 |
| `src/win32/MainWnd.h` / `MainWnd.cpp` / `MainWndOptions.cpp` | 菜单三条命令与 `setRadioCheck` 单选、不支持的后端灰掉 |
| `src/win32/resource.h`（43460–43462）、`vba.rc`、`lang/chs/menu.tsv` + 生成物 | 菜单与本地化 |

### 14.2 实测结果

命令（`sma.gba` + 那份桃子电影）：

```
VBA_VS_Latest_V72XR.exe -rom sma.gba -play <movie> 0 -frameskip 0 -synchronize 0 \
        -vsync 0 -quitAfter 400 -hashtrace out.csv -interp 1
```

| 测试 | 结果 |
|---|---|
| hashtrace，`-interp 0` vs `1`（240 帧，无电影） | 247 行**逐字节相同** |
| hashtrace，`-interp 0` vs `2`（400 帧，带电影） | 407 行**逐字节相同** |
| 第 600 个插帧帧与真帧的像素差（相机档） | 6769 / 38400 = **17.6%** |
| 同上（精灵档） | 6921 / 38400 = **18.0%** |
| 第 120 / 300 个插帧帧 | 0 —— 开机画面是静止的，本来就该一样 |

第 600 帧两个数不同（精灵档多 152 像素）正是精灵也参与插值的证据。

### 14.3 🔴 增量构建的坑：改了 `IDisplay` 或 `VBA` 的布局，必须全量重编 win32

第一次只重编了 8 个文件，**启动即崩**。两个独立原因：

1. `VBA` 加成员加在了类中间 ⇒ 后面所有成员的偏移全变，没重编的 TU 仍按旧偏移读写
   `theApp`。现在这个成员故意放在**类末尾**（并补了 `public:`），老 TU 的偏移仍然有效。
2. `IDisplay` 加虚函数 ⇒ 每个派生类的 vtable 由**它自己的 TU** 生成。没重编的
   `GDIDisplay.cpp` / `DirectDraw.cpp` / `Direct3D.cpp` / `OpenGL.cpp` / `Wgpu.cpp`
   还是旧 vtable，虚调用直接跳到野地址——就是这么崩的。

⇒ 改动 `VBA` 的成员布局或 `IDisplay` 的虚表，`src/win32/*.cpp` 得全量重编。本机没有
MSBuild，用 `~/.workbuddy/scratch/build-interp-all.ps1`（`cl @clv7.rsp` + `link @lnkv7.rsp`，
**100 个 TU** + `rc`，约 2 分 15 秒）。**只重编"改过的文件"在这一步是不够的。**

同一个坑第二次踩到：S3 给 `RecordParams` 加了一个 `bool`，而 `theApp.recordParams` 是
`CVBA` 里靠中间的成员 ⇒ 它后面所有成员的偏移全变 ⇒ 又是全量重编。加字段前先看这个结构体
是不是被嵌在 `CVBA` 里。

顺带：`vba.rc` 改了必须重跑 `make_rc.py`，`vba_chs.dll` 也要重建，否则 exe 的 l10n 哈希
对不上，界面会退回英文。

### 14.4 与 §8 设计稿的差异

- 菜单文案 `Off / Scroll Only / Scroll + Objects`；ini 键 `frameInterpolation`；
  命令行 `-interp 0|1|2`（照既有约定，命令行覆盖退出时会写回 ini）。
- `systemInterpolationSync()` 挂在 `systemFrame()` 开头，模式变化时往
  `vba-trace-v7.log` 写一行 `frame interpolation: mode A -> B` 并清环。
  排查"为什么没效果"先看这一行：`-1 -> 0` 说明条件没满足（多半是跳帧或后端不支持）。
- 一次性诊断：第 120 / 300 / 600 / 900 个插帧帧会记一行与真帧的差异像素数。
- `ifbFunction` 在插帧生效时**跳过**，而不是把菜单项灰掉——改设置不用重启。

### 14.5 还没做 / 没验

- **老 AVI（VFW）菜单没有 2× 录制**：`AVIWrite` 用 `IVideoWriter` 默认的
  `wantsInterpolatedFrames() == false`，所以那条路仍是 59.73 fps。要做就在 `AVIWrite`
  里把 `dwRate` 乘 2 并让同一个虚函数返回真——`WinSystem.cpp` 一行都不用改。
- **画面没肉眼看过**：只证明了"确实画出一帧不同的画面"和"核心状态一字节没变"，
  1px 抖动、边缘撕裂、图层错位这类只能人看。
- 逐帧前进 / 跳帧自动关闭只有逻辑保证（`winPauseNextFrame`、`systemFramesToSkip()`），
  没做界面级验证。
- vsync 开 + 插帧的音频表现没测（sandbox 无声卡）。按设计 present interval 被强制 0。

### 14.6 待观察

- 捕获成本：`GfxLineState` 56 B × 160 × 2 ≈ 17 KB，每行一次 56 B memcpy，实测无可测开销。
- 半帧节拍用 `target/2`（整数毫秒）。原实现本来就把 16.74 ms 截成 16，这里沿用同样的截断，
  **没有**引入新的漂移；但 120 Hz 屏上的长跑漂移（2.0091）仍按 §6 的累加器方案待做。
- 精灵档的 30 Hz 动作（隔帧写 OAM）现在表现为"一帧不动、一帧跳两格"，§12 的限制照旧。

### 14.7 S3 实测（2026-11-06）

录制是用窗口消息驱动菜单录出来的（`~/.workbuddy/scratch/recprobe.ps1`：EnumWindows +
`WM_COMMAND`）。**这个工程的 `ID_OK` 是 32771，不是 `IDOK`(1)**：发 1 会走
`CDialog::OnOK()` 直接关掉对话框，跳过 `OnOk()` 里收集参数那一段——第一次测试就是这么
把 `interpolated` 丢掉的，录出来仍是 59.727 fps。

同一份 ROM 与电影、同样约 11.9 秒：

| 录制 | 帧率 | 帧数 | 视频时长 | 音轨 |
|---|---|---|---|---|
| `-interp 0` | `59727/1000` = 59.727 | 711 | 11.904 s | AAC 11.912 s |
| `-interp 1` | `23891/200` = 119.455 | **1422** | 11.904 s | AAC 11.912 s |

帧数正好 2×、时长完全一致 ⇒ 帧数守恒成立，音频没被拖走。再抽 10 帧算相邻帧差异
（灰度、阈值 6）：`1147 / 1723 / 1149 / 1361 / 1147 / 1714 / 1149 / 1359`——每一对都不一样，
说明文件里是插值出来的画面而不是复制帧（复制的话会交替出现 0 和约两倍的值）。

顺带验了帧率显示：`showSpeed=2` 时标题栏从 `100% (60 fps | 0 skipped)` 变成
`100% (120 fps | 0 skipped)`——`renderedFrames` 现在数的是**present 出去的画面**。

## 15. 行为问答

**限速（throttle）下生效吗？** 生效。`systemInterpolationActive()` 不看 `throttle`；
节拍是"把一帧的周期折成两半"：`systemFrame()` 里的 sleep 减半，另一半由
`systemInterpolationPace()` 在两次 present 之间等出来，`target = 100000/(frameRate*throttle)`。
所以 50% 时限速照样准，只是两张图的间隔变成 33.4 ms 的一半。实测 `-interp 2` +
`-synchronize 1`：标题栏 `100% (120 fps)`——倍速没变、present 翻倍。

（`-synchronize 0` 时不睡也不等，两张图背靠背发出去，只有后一张会被显示器扫到；
这是基准测试模式，插帧在那里的意义是让录制拿到插帧帧，不是给人看。）

**30 fps 的游戏会怎样？** 不会出现鬼影，也不会凭空变顺。游戏隔帧写 OAM / 滚屏时，相邻两帧
的状态相同 ⇒ 插值出来的就是**同一张图**，输出变成"4 张一样的 + 1 张中点"。关键帧仍是
30 Hz，只在跳变处多一个真正的一半位置。比原来"一步一顿"好，但**不是 120 fps 的流畅度**：
插帧只能把一步劈成两步，补不出游戏没画的那一帧。

**为什么不做外推？** 外推（只用当前帧 + 上帧速度）能消掉 1 帧延迟，但游戏随时会变向或停下，
外推出来的位置是猜的，猜错就是整屏撕裂/抖动。§13 留了接口位置（`t` 只喂一帧），要做得
先有逐精灵的速度可信度判断。
