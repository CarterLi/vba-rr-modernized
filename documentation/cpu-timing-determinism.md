# CPU 周期记账的确定性（Debug / Release 行为不一致的根因）

> 2026-11，`aos.vbm` 在第 4000 帧左右 Debug 版与 Release 版行为不同。本文记录根因、
> 修法、以及以后怎么再查这类问题。

## 症状

同一份源码编出的两个 exe：

| 构建 | 现象 |
|---|---|
| `V82XD`（Debug，`/Od /RTC1`） | 期望行为 |
| `V82XR`（Release，`/O2 /GL /LTCG`） | 数千帧后画面/逻辑与 Debug 不同 |

`-hashtrace` 逐帧哈希显示：第 0–7 帧完全相同，**第 8 帧**开始 `ram` 列不同。
两边各自可复现（同一 exe 跑两次字节一致），所以不是竞态或未初始化内存。

## 根因：一个表达式里两次「有副作用」的调用，求值顺序未指定

`GBA-thumb.cpp` / `GBA-arm.cpp` 里分支类处理器这样写：

```cpp
clockTicks = codeTicksAccessSeq16(armNextPC) + codeTicksAccessSeq16(armNextPC) +
             codeTicksAccess16(armNextPC) + 3;
busPrefetchCount = 0;
```

`codeTicksAccess*()` / `dataTicksAccess*()`（`GBACpu.h`）**不是纯函数**：它们会读写
`busPrefetchCount`（预取缓冲计数）。而 C++ 不规定 `+` 左右操作数的求值顺序，于是
「先算哪个」决定了每个调用看到多少预取余量，直接改变这一条指令收的周期数：

| 求值顺序 | bpc 起值 3 时 | 总分 |
|---|---|---|
| 左→右（S, S, N）：/Od | 0 + 0 + `memoryWait[8]` | **6** |
| 右→左（N, S, S）：/O2+/GL | 0 + `memoryWaitSeq[8]` + `memoryWaitSeq[8]` | **5** |

MSVC 实际就是两种都选：**/Od 左到右，/O2 + /GL 先算非顺序访问**。每条「跳转类」指令
差 1 个周期。一个紧凑循环里每帧有 300+ 条这样的指令（实测第 8 帧 353 次），于是两边
CPU 预算落在不同的指令上 → 帧边界错位 → 几千帧后 VBlank/IRQ 的相对时机变了，游戏开始
表现不同。

第 8 帧实测：两边 `lcdTicks` / `VCOUNT` / `DISPSTAT` / `cpuNextEvent` /
`busPrefetchCount` 全部相同，但 CPU 停在不同的 PC；逐指令轨迹的前缀完全一致，只是
Debug 提前 94 条指令用完了预算。**这不是控制流分叉，是时间轴分叉。**

## 修法：把求值顺序钉死（不改算术）

`GBACpu.h` 新增一组包装函数，把原来写在同一个表达式里的调用改成**一条语句一次调用**，
顺序与源码书写顺序一致（也就是 /Od 的顺序、所有既有 movie 录制时的顺序）：

| 包装 | 展开 |
|---|---|
| `codeTicksBranch16/32(a)` | S(a); S(a); N(a) |
| `codeTicksBranchSeq2x16(a)` | `S(a) * 2`（只调一次，再乘 2）；N(a) |
| `codeTicksBranch32/16NonSeqFirst(a)` | N(a); S(a); S(a) |
| `data16/32ThenCode16/32(a, pc)` | D(a); C(pc) |

改动点：

- `src/gba/V8/GBA-thumb.cpp`：`thumbD0`–`thumbDD`、`thumbE0`、`thumbDF`、`thumb47`、
  `thumb44_2/44_3/46_2/46_3`、`thumbF8`（BL 是 S,N,S，单独展开），以及 17 处
  「数据访问 + 取指」的 load/store。
- `src/gba/V8/GBA-arm.cpp`：`arm121`（BX 两种目标）、ALU 写 PC 的宏（`ALU_INSN` 的
  `MODECHANGE` 分支，N,S,S）、`armA00`/`armB00`/`armF00`（S + N + S）、`STR`/`LDR`
  宏（数据 + 取指）。
- `src/gba/jit/JitInterp.cpp`：IR 解释器**另有一份**同样的公式，同样的问题（见下）。

### 为什么 JIT 也必须一起改

IR 解释器不是复用 ARM/Thumb 处理器，而是自己抄了一遍周期公式。只修解释器会造成
`-jit` 与 `-nojit` 互不一致（实测：只修解释器后，Release 的 `-jit` 与 `-nojit` 第 8
帧就分叉）。现在两边都用同一组包装，`-jit` / `-nojit` 逐帧哈希一致。

### 不要做的事

- **不要**把 `busPrefetchCount` 在算周期前清零来「更符合硬件」。那会把每条跳转从
  5/6 周期变成 8 周期，等于重新调一遍时序，所有既有 movie 全部作废。
- **不要**把 `codeTicksAccessSeq16(x) * 2` 改成调两次。函数会消耗两次预取余量，
  最多多算 8 个周期（`JitInterp.cpp` 里已有注释记着这个坑）。

## 验证

```bash
# 逐帧状态哈希（只比 ram 列，full 含 pix/OSD 会随渲染时机变）
vba.exe -rom <rom> -play <movie> 0 -synchronize 0 -frameskip 0 -vsync 0 \
        -quitAfter 4202 -hashtrace out.csv
```

- `aos.vbm` 全片 4203 帧：Release(Debug) `ram` 列 **0 处不同**
  （修前：第 8 帧起共 2592 帧不同）。
- 修后的 Debug 与修前的 Debug 逐帧完全一致 → 修法没有改变「期望行为」，只是把
  Release 拉回同一条时间轴。
- `-jit` 与 `-nojit` 全片一致；`-jitverify 20000` 全绿。

## 复现工具

`common/HashTrace.cpp` 里另有两个环境变量开关（默认关）：

- `VBA_DUMP_STATE_FRAME=n` + `VBA_DUMP_STATE_PATH=file`：在第 n 帧边界把
  `emuWriteMemState` 的 400000 字节原始状态存盘（附 `.txt` 写 crc），用于
  「两边此刻到底差在哪」。
- `VBA_PC_LOG` / `VBA_PC_START` / `VBA_PC_END`：逐指令 24 字节记录
  `(pc, clockTicks, busPrefetchCount, cpuTotalTicks, cpuNextEvent, busPrefetch)`。
  **热路径钩子默认编译掉了**，要用得先在各 CPU 文件顶部把
  `VBA_PC_TRACE_HOOK` 置 1。这个工具就是本次定位问题的关键：两边 pc 序列完全相同时，
  只有 `busPrefetchCount` 之外的东西能让同一地址的周期数不同。

## 遗留

`dataTicksAccess*` 的返回值本身与 `busPrefetchCount` 无关，所以「两个相同调用相加」
（如 `dataTicksAccess32(a) + dataTicksAccess32(a)`）不受顺序影响，这些没动。
以后新增任何「一个表达式里两次 TicksAccess」的代码，请直接用上面那组包装函数。
