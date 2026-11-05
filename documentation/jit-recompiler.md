# 重编译器（JIT）与 AOT 导出：设计思路与后端契约

> **这份文档解决两个问题**：① 为什么这么设计（改代码前先看第二节）；② 写一个新后端要满足什么
> （第三节是可以逐条打勾的契约清单，ARM64 后端按它做）。
>
> **A64 的专属事实不在这里**：Windows ARM64 ABI、寄存器分配、asmjit A64 API、逐指令映射、
> 风险清单都在 [`jit-a64-backend.md`](jit-a64-backend.md)。本文只提供它需要的**后端无关契约**和
> **x64 的对照索引**。

---

## 一、现状与阅读路径

### 1.1 一页纸现状（2026-11-05）

**一句话**：V7 的 x64 机器码后端已经跑起来，**逐帧等价**，纯 CPU 场景比内联解释器快
**10–23%**；整帧仍基本持平（渲染/合成/声音占大头）。剩下的事都是"更快"，不是"更对"。

| 项 | 状态 |
| --- | --- |
| IR + 解码器（ARM / Thumb 全量） | 已落地：`JitIr.h`、`JitDecode*.cpp` |
| IR 解释器（两核各一份语义，兼作周期规范） | 已落地：`JitInterp.cpp` |
| 块缓存 / 页失效 / `jitExecute()` | 已落地：`JitCache.cpp`，V7/V8 共用机制 |
| x64 机器码后端 | 已落地：`JitEmit.cpp` + asmjit，**只做 V7 + x64** |
| 开关 | 菜单 + `vba.ini` 的 `jit` + `-jit`/`-nojit`，默认关闭 |
| 差分验证 | `-jitverify 20000` × 三 ROM：单条 + 块级 0 差异 |
| 端到端 | `-hashtrace` × 三 ROM × 6000 帧：`ram` 列逐帧一致 |
| 纯 CPU 性能 | mario 1.11x / sonic 1.22x / aos 1.23x |
| 整帧性能（60 fps 正常玩） | 约 1.0x —— 见 2.1 |
| ARM64 后端 | 未开始，见 `jit-a64-backend.md` |
| AOT 导出 | 未开始（语义层已就绪，`JitC.cpp` 未动笔） |

### 1.2 三个入口

| 你要做的事 | 先读 |
| --- | --- |
| **写 ARM64 后端** | 第三节全部（契约）+ 第五节（V7 语义）+ `jit-a64-backend.md` |
| 改 x64 生成代码 | 第二节（红线）+ 第六节（实现笔记与已知问题） |
| 改 IR / 解码器 | 4.3（数据模型）+ 5.5（两核差异）+ 第九节（验收） |

### 1.3 三条验收命令

```
clang++ -std=c++17 -DUSE_GBA_CORE_V7 -DUSE_GB_CORE_V7 -I src/gba/jit \
        src/gba/jit/selftest.cpp src/gba/jit/JitDecodeArm.cpp \
        src/gba/jit/JitDecodeThumb.cpp src/gba/jit/JitDump.cpp -o selftest_v7 && ./selftest_v7

VBA_VS_Latest_V72XR.exe -rom roms\aos.gba -jitverify 20000        # 报告写 jitverify.log

VBA_VS_Latest_V72XR.exe -rom <rom> -play movies\<x>.vbm 0 -synchronize 0 -frameskip 10 \
        -vsync 0 -quitAfter 6000 -jit -hashtrace jit.csv         # 再跑一遍 -nojit 比 ram 列
```

---

## 二、设计思路：六条约束

这一节是全文的"为什么"。每一条都写清**问题 → 结论 → 代价**，因为后面所有代码形态都是它们的
推论；不理解这些就去改结构，改出来的东西一定会在某处破等价。

### 2.1 约束一：时序是红线，所以逐指令记账

**问题**：解释器的周期模型是"跑到下一个事件"：

```c
// CPULoop
while (cpuTotalTicks < cpuNextEvent && armState && !holdState && !SWITicks);

// 每条指令
clockTicks = 指令内部周期 + 1 + codeTicksAccessSeq32(该指令地址);
cpuTotalTicks += clockTicks;
```

而 `codeTicksAccessSeq32`（`GBACpu.h:238`）**不是常数**：它读 `busPrefetchCount`，还会改写它；
`dataTicksAccess*` 同理，且依赖运行时的 `address >> 24`。

**结论**：任何"整块算周期、块尾对账"的做法都会改变 IRQ 落在哪条指令上，从而改变逐帧行为。
对一个 TAS 项目，这等于废掉 `.vbm` 的可复现性。所以**一条 guest 指令 = 一次周期记账 + 一次
预算检查**，不做超级块周期摊还。

**代价**：每条指令多 3 条宿主指令（`sub/cmp/jle`）。但省掉的是间接调用和全局内存往返，净赚
仍然很大。**这条不可协商**：宁可慢 10%，也不能让 IRQ 早/晚一条指令。

### 2.2 约束二：块必须能"原地交还控制权"，所以有回退而不是修正

**问题**：有些指令块后端表达不了——它们要么改写 `CPULoop` 的局部变量（`CPUCheckDMA`、
`CPUSwitchMode(..., breakLoop=true)`），要么语义依赖宿主的未定义行为，要么"写 PC 但不是跳转"。

**结论**：**失败模式只能是"更慢"，绝不能是"不同"**。具体做法是三层降级：

| 层 | 粒度 | 触发 |
| --- | --- | --- |
| 解码器拒绝 | 整块 | 遇到 BKPT / 控制域 MSR / `^` 块传输 |
| 前缀截断 | 从某条起 | `nativeSupported()` 说不 |
| `JIT_NEED_INTERP` | 单条（运行期才知道） | IO 写、寄存器移位计数 ≥ 32 |

被截断/回退的那部分交给 IR 解释器，最终交给 CPULoop 的内联解释器。**这条直接决定了后端形态**：
每个"不肯做"的地方都要有一个明确的、代价可测的出口，而不是尽力而为地猜。

### 2.3 约束三：V7 与 V8 是两个互斥的构建，所以用编译期分支

**问题**：两个核心的 `GBA.cpp` 定义**同名全局**（`reg`、`armState`、`memoryWait*`、
`thumbCycles`），根本不可能同时链接进一个 exe；`src/gba/GBA.cpp` 第 4 行就用
`#ifdef USE_GBA_CORE_V7` 决定 include 哪个。

**结论**：编译期 `USE_GBA_CORE_V7` 分支，**不引入运行时 `JitCoreOps` 函数指针表**。

- 正确性：没有"core tag 忘设置 / helper 指针为空"的风险；
- 性能：V7 的周期记账是热路径，函数指针会阻止内联；
- 可维护性：不需要维护两个实现都满足的 ABI。代价是共享代码里出现 `#ifdef`，和仓库现有的
  `GBA.cpp` / `GBAMemory.cpp` / `JitCache.cpp` 风格一致。

接口组织上仍建议把按核的 helper 收进编译期命名空间（`namespace jitcore { int memCycles32(u32); }`），
调用点看起来和函数指针表一样，但由编译器静态解析；将来真要双核，把这些函数改成表即可。

**推论**：`JitInsn` 的数据布局必须两核一致（`JitDump.cpp`、`selftest.cpp`、`JitVerify.cpp`
共享），`sizeof(JitInsn)` 恒为 16（`JitIr.h` 与 `selftest.cpp` 各有一条 `static_assert`）。
新增字段时 V8 构建也要保留（填 0）。

### 2.4 约束四：编译比解码贵 200 倍，所以有三层缓存

**问题**：解码一个块约 50 ns，**编译一个块约 10 µs**。块缓存是直接映射且页失效会整页丢弃，
`aos` 1200 帧就有 37 万次解码——每次都编译就是 60 s 起步（实测 3000 帧 60–124 s，用户看到的
就是"25 帧"）。

**结论**：三层，各管一件事：

1. `jitCache[4096]`：解码块缓存。纯解码成本可忽略（4096 vs 1024 只省 1.2% 时间，见 2.6）。
2. `jitCodeCache[16384]`：**机器码缓存，键 = 地址 + 解码指令的哈希**。重新解码出同样的指令就
   复用已有机器码，页失效/缓存碰撞都不再触发编译。`hashBlock()` 按字段算，**不能用裸
   `memcmp`**——`JitInsn` 有一个未初始化的 padding 字节。
3. `jitHot[512]`：**命中 8 次才编译**（`JIT_COMPILE_AFTER = 8`）。计数按 `(tag, hash)` 存在
   侧表里，不放在解码块条目中——页失效会重解码，热块还没到阈值就被清零。冷块走 IR 解释器
   （约 100 ns），比给它编译（10 µs）便宜 100 倍。

**代价**：多两级表（约 1 MB + 若干），以及"为什么这个块没编译"要多查一个计数器。收益是把
`mario` 600 帧的编译次数从 95 822 降到 15 955。

### 2.5 约束五：块边界由"状态可见性"决定，不由指令语义决定

**问题**：看起来"分支结束块"就够了，实际上块还必须在不满足某些不变式时停下。

**结论**：块边界有四种，前三种是编译期的，第四种是运行期的：

| 边界 | 时机 | 理由 |
| --- | --- | --- |
| 分支 / 写 r15 / `BX` / `SWI` / 未定义 | 编译期 | 控制流离开块 |
| 跨 1 KB 页 | 编译期 | 失效检查只需看一个页版本号 |
| `JIT_MAX_INSNS`（64） | 编译期 | 块太大则编译成本、icache 压力上升 |
| **`reg[15]` 不变式被破坏** | 运行期 | 见 3.2 第 3 条 |

**不因为内存访问结束块**：IO 访问走 helper 或回退，RAM/ROM 走内联快路径，继续往下。

### 2.6 被实测否掉的三个假设

记录下来是为了别再翻案。

1. **"扩块缓存能提速"** —— 1024 → 4096 把解码砍掉 35%，时间只省 **1.2%**。按差值算一次解码
   约 50 ns，而一个块（平均 6 条指令）执行约 100 ns，解码被稀释掉了。继续扩到 8192 只会多花
   4 MB BSS 换不到半个百分点。**已还原成 1024**（解码缓存），另建了 16384 项机器码缓存。
2. **"把 guest 寄存器放进宿主寄存器就能快"** —— 用独立 C 探针（MSVC `/O2` 后看反汇编）量：
   状态全在全局量 2.88 ns / 5 条指令，状态进局部变量 2.74 ns，只快 **1.05x**。编译器确实把
   `reg[]` 塞进了 `r12-r15`、`Z` 塞进了 `bpl`，但该付的一条没少。**瓶颈是"标志位算得太贵"**，
   所以先做了 `SETcc`（4.5），而不是寄存器分配。
3. **"条件判断打包读标志能省指令"** —— 见 6.6.2 第 1 条：8 个条件本来就一次 byte load，
   N/V 比较用 dword 反而更贵，而且会踩 store-forwarding。**否决。**

---

## 三、后端移植契约

**这一节是给新后端（ARM64）的 checklist。** 每条都标了"为什么"，因为照抄代码容易，照抄理由
才不会在移植时把某个约束悄悄丢掉。x64 的具体实现位置见第六节。

### 3.1 块执行契约

| # | 契约 | 为什么 | x64 位置 |
| --- | --- | --- | --- |
| 1 | **一条 guest 指令 = 一次周期累加 + 一次预算检查** | 2.1 | `checkBudget()` 每条一调 |
| 2 | 块入口前提：`reg[15] == armNextPC + 4`（ARM）/`+ 2`（Thumb） | V7 用 `reg[15]` 当下一条地址 | `jitExecute()` 进块前查 |
| 3 | 块出口写 `armNextPC` 和 `reg[15]`；V8 还要重填 `cpuPrefetch[0..1]`，V7 不要 | 解释器循环入口直接把 `cpuPrefetch[0]` 当作要执行的指令 | `runBlock()` 末尾 |
| 4 | 返回值 ≥ 0 = 本次执行周期数；< 0 = 一条都没执行 | `CPULoop` 据此给 `clockTicks` 赋值或走内联解释器 | `JitEmit.h` 的 `fn` |
| 5 | `NEED_INTERP` 时 `nextPc` 必须指着**这一条**，并置 `jitForceInterp` | 解释器只补跑这一条，不多不少 | `emitExitImm(pc, JITX_NEED_INTERP)` |
| 6 | **`NEED_INTERP` 桩必须发在 epilogue 之后** | 桩紧跟预算检查内联发出会被顺序执行穿透：IO store 跑两遍、`push` 把 sp 减两次 ⇒ 白屏 | `emitNeedInterpStubs()` |
| 7 | 预算守卫在**链头**也要有一条 | 只含分支的块没有逐指令预算检查，两块的死循环会在生成代码里永远转下去 | `emitChainBail` |
| 8 | 退出状态只有四种：`NEED_INTERP` / `KEEP_PC` / `BUDGET` / `DONE` | 少一种就要在调用方猜 | `JitExecResult` |

### 3.2 状态可见性契约

**这一组是"逐帧等价"的真正内容**，任何一条错了都会在几千帧后炸。

1. **三个 PC 值不能混**：`pc`（指令地址）/ `armNextPC`（`pc+4` 或 `pc+2`）/ `reg[15]`
   （`pc+8` 或 `pc+4`）。三者各有用处，详见 9.9。

2. **调 helper 之前必须把 `reg[15]` 写对**：内存 helper 读它做 open bus 值。这就是
   `setPc()` 存在的唯一理由，也是"哪些指令需要先 `setPc()`"的判断依据（`needsPcBefore()`）。
   `bx r15` / `bx pc` 是 Thumb→ARM 惯用法，**必须先写 `reg[15] = pc+4`**，否则用的是上一条的
   旧值 ⇒ 死循环黑屏。

3. **`reg[15]` 不变式**：块内任意时刻必须满足 `reg[15] == 下一条要执行的地址 + 4/2`。任何写
   `reg[15]` 的指令（`str r0, [r15,#k]!`、`stm r15!`、`mrs r15, cpsr`）都会破坏它——它们执行
   完下一条仍从 `pc+4` 取，但再下一条就跳到被写坏的值上。块运行器每条之后检查，不成立就
   **停在原地、保留现场**；进块前再查一次。两个检查接起来正好复刻解释器的"绕一条"。

4. **标志始终是 `N_FLAG/C_FLAG/Z_FLAG/V_FLAG` 四个 `bool8` 全局**。条件判断读它们；写的时候
   用宿主的原生标志位（见 4.5），但 `SETcc` 一类必须在周期记账/预算检查之前——那两者会改宿主
   标志。

5. **内存访问默认走带钩子的包装**，只有三类用 Quick 版本：取指、Thumb `0x48`（PC 相对 LDR）、
   Thumb `0x98`（SP 相对 LDR）。写入侧没有 Quick。理由：Lua READ/WRITE 钩子必须看到每一次
   访问。

6. **写 IO 区（`0x04xxxxxx`）的指令不执行，回退**。`CPUWriteMemoryWrapped()` 会同步调
   `CPUCheckDMA()`，后者读写 CPULoop 的局部变量 `extCpuNextEvent` / `extClockTicks`。块内唯一
   可能触发它的就是 IO 写；对块传送要检查**整个地址范围**是否与 `0x04000000-0x04FFFFFF` 相交，
   不能只看首地址。

7. **banked 寄存器与模式切换不建模**：`^` 块传输、所有写 CPSR 的 MSR、SWI 一律回退。

8. **`POP {pc}`（Thumb）不切换 ARM 状态**：V7 把值 `& 0xFFFFFFFE` 后直接当 Thumb 地址用。
   不要按 ARMv4T 架构手册"bit0 决定状态"去实现。

9. **PC 写回掩码有三种，不能统一**：ARM `LDR pc` 用 `~3`；ARM `LDM {pc}` **不掩码**；
   Thumb `POP {pc}` 用 `~1`。写回时机也有四种（详见 6.6.4）。

### 3.3 编译契约

| # | 契约 | 为什么 |
| --- | --- | --- |
| 1 | 只编译**前缀**：遇到拒绝项就截断，前面的照编 | 一条不肯做的指令不该让整块退回解释器 |
| 2 | 拒绝清单按"块表达不了"划，不按"难写"划 | 见 6.2 |
| 3 | 编译结果缓存键 = 地址 + **指令字段哈希**，不是 `memcmp` | `JitInsn` 有未初始化 padding |
| 4 | 热度阈值 `JIT_COMPILE_AFTER = 8`，计数放侧表 | 页失效会重解码，per-entry 计数会被清零 |
| 5 | **发射错误必须显式捕获**，当作"没编译"处理 | asmjit 的 `set_error_handler()`；否则会产出半截函数 |
| 6 | 编译失败不能让块缓存进入"已编译"状态 | 否则会跳进垃圾 |

### 3.4 链接契约

块链接（6.3）是**可选优化**，但一旦做，五条约束必须同时满足：

1. **只链接 ROM 后继**（`0x08..0x0D`）。EWRAM/IWRAM 页会被写操作顶版本号，链接进去就得做逐页
   校验；ROM 只读，省掉了。
2. 只链接**整块都被编译**的块（`insnCount == block.count`），保证 `runBlock()` 的 IR 尾巴逻辑
   不会被跳过。
3. **链头要有预算守卫**（见 3.1 第 7 条）。
4. **栈帧形状必须一致**（`frameKind` 相同），否则后继的 epilogue 会按错的形状收尾。
5. 代码槽被复用前**只拆与该槽相关的链接**；线性扫描拆链会比链接省下的还贵（实测 600 帧
   2946 万次扫描，反而慢 2–9%）。

### 3.5 缓存与失效契约

- 页版本：每 1 KB 一页，EWRAM 256 + IWRAM 32。块记住起始页版本，**入块时比对一次**。
- **只有这一页曾经编译过块，写它才 bump 版本**（`jitPageHasCode[page]`）。游戏反复写 EWRAM
  数据时这是必须付的代价。
- 写钩子挂在 `CPUWriteMemory/HalfWord/Byte` 的**外层包装**，与 Lua WRITE 钩子并列；不要改
  `*Wrapped` 内部，否则会漏掉 DMA/内部写。
- 4 字节写可能横跨两页，要检查 `address + size - 1` 所在页。
- **整表作废**：`CPUReset()`、读档成功/失败、`jitSetEnabled(true)`、
  `BIOS_RegisterRamReset()`、`BIOS_SoftReset()`。后两个用 `memset` 清 RAM、绕过写钩子，必须
  手工清。
- **`WAITCNT` / `memoryWait*` 改变不需要清缓存**：基周期是执行期查表，IR 里只存 `fetchCycles`。

### 3.6 交互契约

| 特性 | 要求 |
| --- | --- |
| Lua EXEC 钩子 | 每次调用 `jitExecute()` 都退回解释器（钩子可以运行中装/卸，不能按块缓存判断） |
| Lua READ/WRITE 钩子 | 装上就整体停用 JIT（内联快路径绕过 `CPURead*` 包装，钩子看不到访问） |
| 调试器 / 断点 | `systemDebug != 0` ⇒ 退回解释器 |
| 金手指 | `mastercode` 非 0 时块内**每条指令前**比一次 `mastercode == pc`，命中调 `CPUMasterCodeCheck()`；变化不需要清缓存 |
| 存档 | JIT 状态不入存档；读档后 `jitResetCache()` |
| 电影 / TAS | 语义上无影响，但必须通过 `-hashtrace` 逐帧比对 |
| Win32 (x86) | IR 解释器可用；机器码后端只做 x64 + ARM64 |

### 3.7 调试开关契约

| 开关 | 作用 | 新后端要不要实现 |
| --- | --- | --- |
| `VBA_JIT_SHADOW=1` | 每个块先跑机器码、恢复现场再用 IR 解释器跑，逐字段比对 | **必须**，这是抓"生成代码错了但差分没覆盖"的主力 |
| `VBA_JIT_OPS=<mask>` | 按 `JitOp` 位掩码关掉某类指令的编译，二分用 | 必须。`=0` 是合法值，别拿 0 当"未初始化"哨兵 |
| `VBA_JIT_ASM=<file>` | 导出每个块的 IR + 机器码字节 | 必须 |
| `VBA_JIT_ASM_LIMIT=<n>` | 上面那个的块数上限（默认 200，0 = 不限） | 必须 |
| `VBA_JIT_NOCHAIN=1` | 关掉块链接做 A/B | 做了链接就要有 |
| `VBA_JIT_PCTRACE=<file>` + `JIT_PC_TRACE` | 逐条打印 `(pc, lr, sp, ticks)` | 强烈建议，端到端分叉只能靠它定位 |

### 3.8 移植顺序（为什么是这个顺序）

```
IR 解释器跑通 → 单条差分 → 块级差分 → 影子对比 → 端到端 hashtrace → 性能
```

- **先 IR 解释器**：它同时是"周期规范"和"差分参考"，机器码后端的一切争议都以它为准。
- **单条差分先于块级**：字段拆错的成本比"块边界错"低一个数量级。
- **块级差分不能省**：它能抓到单条 pass 结构上抓不到的东西——块在哪停、中途切指令集、
  `armNextPC`/预取留给下一条。这类错的表现是**卡死**而不是某个寄存器错。
- **影子对比先于端到端**：影子能给出"哪个块、哪个字段"；端到端只给"第 N 帧不一样"。
- **端到端最后**：它是唯一能覆盖 SWI 的 BIOS 副作用、异常入口、banked 寄存器的层。

---

## 四、架构与数据模型

### 4.1 分层

```
CPULoop  (不动)
   └─ jitExecute()                     ← 替换 armExecute/thumbExecute 的调用点
        ├─ 快查 (pc, thumb) → JitBlock*       JitCache.cpp
        ├─ 未命中 → 解码器 → IR                JitDecodeArm/Thumb.cpp
        ├─ 热度够 → 机器码后端 → 可执行内存      JitEmit.cpp (x64 only)
        └─ 执行：块内直译指令 + 逐指令预算检查 → 返回
             └─ 辅助函数：内存访问、PC 写、标志物化、异常
```

后端有两层，**同一套 `JitCache` / `CPULoop` 接线**：

1. **IR 解释器**（`JitInterp.cpp`）：平台无关，Win32/x64/ARM64 都能跑，兼作周期规范。
2. **机器码后端**（`JitEmit.cpp` / 将来的 `JitA64.cpp`）：只做 64 位宿主。

这样换后端时 `CPULoop` 那一侧一行都不用改——这是当初先做 IR 解释器而不是先做 C 后端的
直接原因，也是 A64 能"只写一个发射器"的前提。

### 4.2 为什么必须有 IR（以及为什么不用 LLVM）

1. **AOT 要复用**。AOT 需要同一套语义再生成一遍 C 或另一份机器码；没有 IR 就得把解码逻辑
   写两遍，两遍之间必然漂移。
2. **差分测试要一个"可读的第三实现"**。C 后端生成的代码能直接编译、断点、打印。
3. **优化空间**。标志位惰性计算、常量折叠、死代码消除只能做在 IR 上。

**为什么不用 LLVM / GIMPLE**（按重要性排）：

1. **粒度对不上。** GBA 的块只有 10–50 条 guest 指令。LLVM 建 Module + 验证 + 跑 pass 是
   几十微秒级，编译开销会盖过执行开销；`LLVM-C.dll` 60–100 MB 对单 exe 分发不现实。
   GIMPLE/RTL 是 GCC 内部结构，没有对外 ABI。
2. **语义装不进去。** `reg[15]` 执行期等于 PC+4、写 PC 时抑制标志更新、`busPrefetchCount`
   被指令本身改写、7 组 bank 寄存器——在标准 IR 里只能退化成"调 helper"，那样 IR 就只剩
   包装壳。
3. **AOT 要出 C。** 自定义 IR 直接吐 `.c`，可读、可断点。LLVM 只能吐 `.obj`。

后路留着：将来 AOT 若发现生成代码慢，可以**离线**把自定义 IR 翻成 LLVM IR 编译，不参与
JIT 热路径。参考：mGBA 的 JIT 连 IR 都没有（直接发射机器码），Dolphin 是自定义 IR + 多后端
——"一层 IR + 多后端"是这类项目的标准做法。

### 4.3 数据模型

**`JitInsn`**（以 `src/gba/jit/JitIr.h` 为准，**不要在这里重抄枚举**，两处必然漂移）：

```c
struct JitInsn {
    u32 imm;        // 立即数 / 移位量 / 分支偏移 / 寄存器列表 / SWI 号
    u16 flags;      // JF_*：WRITEBACK / PRE / UP / SIGNED / LINK / IMM / PC_ALIGN4 / CLEAR_PREFETCH / QUICK / DATA32
    u8  op;         // JitOp：一条 guest 指令 = 一个 JitInsn
    u8  sub;        // ALU 子操作 / MUL 子操作 / 访存宽度 / LDM-STM 模式
    u8  cond;       // ARM 条件码；Thumb 一律 0xE（条件分支除外）
    u8  d, n, m;    // Rd / Rn / Rm（访存里是 Rt / base / offset 寄存器）
    u8  shift;      // JitShift：移位种类（访存时按 OFFSET_* 语义解释）
    u8  flagMask;   // 本条写哪几个标志（N/Z/C/V 位掩码），0 = 不写
    u8  fetchCycles; // V7 Thumb 的 thumbCycles[opcode >> 8]；ARM 填 0（见 5.1）
};
```

两个容易踩的设计点：

- **`flagMask` 不是布尔量。** ARM 的逻辑运算写 N/Z/C、不动 V；Thumb 的大多数 ALU 只写 N/Z；
  `mov r0, r1`（Thumb 高寄存器形态）一个都不写。用"置标志"一个 bit 表达不了。
- **`JF_IMM` 决定第二操作数从哪来。** 没有这个标记时立即数形态的 `m` 留着 0，解释器会当成
  `r0`——`ldr r0, [r1, #4]` 的偏移会被算成 `r0`。

`JitSource` 直接对齐 `memoryMap[]`（`base[addr & mask]`），两核同构，不用按核改。

**块边界**见 2.5。

### 4.4 周期记账与预取契约

块内每条 guest 指令翻译成：

```
; 1) 指令本体（操作宿主寄存器里的 GBA 寄存器）
; 2) 周期
mov  eax, [busPrefetchCount]        ; 或编译期已知时直接常数
... 与 codeTicksAccessSeq32 等价的 6-10 条指令，区域号是编译期常数 ...
add  [cpuTotalTicks], ecx
; 3) 预算检查
cmp  [cpuTotalTicks], [cpuNextEvent]
jge  .exit_block
```

第 2 步是唯一"丑"的地方。**第一版允许直接 `call codeTicksAccessSeq32`**——它本来就是
`inline`，编出来只有十几条指令，仍比解释器的"间接调用 + 全套全局往返"便宜。

**块边界的预取契约**（V8 专属，V7 没有这两个概念）：这三样都是**跨指令累积**的全局量，块不能
"批处理"。

| 量 | 谁在改 | 块后端必须做什么 |
| --- | --- | --- |
| `busPrefetch` / `busPrefetchCount` | `codeTicksAccess*` / `dataTicksAccess*` 的副作用 | 每条 guest 指令仍调同一批函数；`jitInterpInsn()` 逐条照抄循环前导那三行（`0x08020000` 补丁、`busPrefetch = false`、`busPrefetchCount & 0xFFFFFE00` 则抬成 `0x100 \| (x & 0xFF)`）。**不许合并、不许缓存、不许省** |
| `cpuPrefetch[0..1]` | 分支/写 PC 之后的 `ARM_PREFETCH` / `THUMB_PREFETCH` | **块出口重填**；块入口不用管——块是从内存重新解码的 |
| `armNextPC` / `reg[15]` | 同上 | 块出口设成"下一条要执行的指令地址"和它的 +4/+2 |

出口必须重填，不能"信任上一条指令留下的值"：块可能因为预算耗尽、`SWITicks`、`holdState`、
指令集切换提前结束，下一条要交给解释器，而解释器的循环入口直接把 `cpuPrefetch[0]` 当作要
执行的那条指令。不重填的话，页失效之后的第一次回退会执行**上一条指令**。

### 4.5 标志位

**原计划**（块内打包进一个 32 位字、出口物化）没做，而且**否决了打包读**——理由见 6.6.2。

**实际落地**：标志始终留在 `N_FLAG/C_FLAG/Z_FLAG/V_FLAG` 四个 `bool8` 全局里，但**写**的
方式换了。`arm-new.h` 的 `setAddFlags()` / `setSubFlags()` 和宿主 x86 的 SF/ZF/CF/OF 是同一套
公式：

```
N = SF        Z = ZF        V = OF        C = CF (ADD) / !CF (SUB，ARM 的 C 是"无借位")
```

所以 `add/sub` 刚做完，宿主标志寄存器里就躺着答案，直接 `SETcc m8` 四条即可，不必从操作数
重算（V 要两次 xor + and + shr，C 要 64 位加法 + shr）。一条 ADD 从 102 字节降到约 70 字节。

两个例外保留原路径：`ADC/SBC/RSC` 的结果由两条 x86 加减拼成，第二条的 OF/CF 不是三元
guest 运算的标志；逻辑类的 C 来自移位器而不是 ALU。`SETcc` 不修改标志，四条之间可任意排；
但 `addFetch()` / `checkBudget()` 会破坏宿主标志，所以 `SETcc` 必须在它们之前。

> **给 A64 的提示**：AArch64 的 `adds/subs` 也直接产出 N/Z/C/V（且 C 的语义同样是"无借位"），
> `cset` 等价于 `SETcc`，所以这套思路是**可移植的**；A64 的寄存器级写法见
> `jit-a64-backend.md` §5.6。

### 4.6 内存访问

**原计划的三档**（按阶段上）：

| 档 | 做法 | 状态 |
| --- | --- | --- |
| A | 直接 `call CPUReadMemory/CPUWriteMemory`（带 Lua 钩子的包装） | 已落地，作为兜底 |
| B | 编译期判断"本次会话没有 Lua 内存钩子"→ 改调 `*Wrapped` | 被 C 档吸收 |
| C | 内联快路径 `if (region == 2) v = *(u32*)(workRAM + (addr & 0x3FFFC))` 否则落回 helper | **已落地**，规则见 6.4 |

### 4.7 失效（自修改代码）

**契约见 3.5**，这里只补实现形态和两条设计取舍。

GBA 上代码可以住在 ROM（只读）、BIOS（只读）、EWRAM、IWRAM。前两者不用管，后两者必须管。

- **块缓存是"无淘汰"的直接映射**：1024 项，索引 `((pc >> 2) ^ (pc >> 12)) & 1023`，tag 是
  `pc | thumb`；一项一个 `JitBlock`（1032 B）⇒ 约 1 MB BSS。未命中就覆盖旧块，没有分配器、
  没有 LRU。这个粗暴设计能成立，靠的是 2.4 的机器码缓存——覆盖掉解码块不会丢掉已编译的
  机器码。
- **不做页写保护的 `VirtualProtect` 方案**：Windows 上每次异常处理几十微秒，GBA 游戏写 EWRAM
  极其频繁，会直接崩掉性能。改成"写时比对页版本"，代价是一次字节 load。
- 页版本数组是 `jitPageVersion[288]` + `jitPageHasCode[288]`（EWRAM 256 + IWRAM 32）。

### 4.8 编译结果缓存

见 2.4 的三层设计。实现位置：`JitCache.cpp` 的 `jitCodeCache` / `jitHot` / `hashBlock()`。

### 4.9 开关

| 入口 | 行为 |
| --- | --- |
| `Options > Emulation > CPU Recompiler (JIT)` | 复选框，勾上即 `jitSetEnabled(true)`；`vba.ini` 的 `jit=1` 记录状态 |
| `-jit` | 本次运行强制打开，覆盖 ini 里的值 |
| `-nojit` | 本次运行强制关闭，用于在 `jit=1` 的配置下跑 HashTrace 的解释器基线 |
| 默认 | 关闭。机器码后端只覆盖 V7 + x64，其余配置开了只会更慢 |

实现约定：

- 命令行的 `-jit` / `-nojit` 先记下来，**整条命令行读完再应用**。否则 `vba.exe -jit game.gba`
  这种"裸 ROM 参数"会让 `loadSettings()` 再跑一次，把 `-jit` 覆盖回 ini 里的值。
- 切换开关调用 `jitSetEnabled()`，它会清空块缓存（`jitResetCache()`），因此运行中切换没有
  半编译状态。
- 菜单项按 `jitAvailable()` 灰显。电影录制/回放期间和其它改变模拟行为的选项一样，需要按住
  Ctrl 才能切换。
- 存档、HashTrace、电影格式**都不记录**这个开关：它只影响 CPU 走哪条执行路径。
- 🔴 **命令行覆盖会写回 ini**：`VBA::saveSettings()` 退出时把内存设置写回（123 个字面键），
  `-synchronize 0` / `-vsync` / `-frameskip` / `-jit` 都会固化。跑完基准必须核对
  `synchronize` / `frameSkip` / `gbFrameSkip` / `jit`，否则下次启动可能永久不限速，而
  「设置速度」菜单仍显示 100%。这是既有行为，**不要**改成 run-only。

---

## 五、V7 的语义事实（后端无关，A64 必须照抄）

这一节是"V7 到底怎么算"的权威记录。**任何后端都不该自己重新推导**——这里的每条都是拿
`arm-new.h` / `thumb.h` 逐段核对、再被差分测试钉过的。

### 5.1 周期模型

#### 5.1.1 取指基周期

V7 的每条指令在进入 handler 之前，先由解释器循环给 `clockTicks` 赋一个**基周期**，handler
再在这个值上做加法（少数 Thumb 分支是直接赋值）。

| 指令集 | V7 基周期 | 出处 |
| --- | --- | --- |
| ARM | `clockTicks = memoryWaitFetch32[(armNextPC >> 24) & 15]` | `src/gba/V7/arm-new.h:1413` |
| Thumb | `clockTicks = thumbCycles[opcode >> 8] + memoryWaitFetch[(armNextPC >> 24) & 15]` | `src/gba/V7/thumb.h:184` |

要点：

- 这两行都在 `armNextPC = reg[15].I` **之前**执行，所以取的是**当前指令地址**所在的区域。
- `memoryWaitFetch` / `memoryWaitFetch32` 是**运行期数组**，会被 `CPUReset()`、读档和
  `WAITCNT` 写入改写。因此基周期**不能**在解码时固化成常数，必须在执行时按 `pc >> 24` 查表。
- `thumbCycles[]` 是 256 项常量表（`V7/GBA.cpp:113-131`），值域 1..4。Thumb 的基周期**只由
  opcode 高 8 位决定**，与地址无关。
- V7 **没有** `busPrefetch`、`busPrefetchCount`、`cpuPrefetch`、`cpuTotalTicks`、`SWITicks`、
  `IRQTicks`、`mastercode`。这些 V8 概念一个都不能带进 V7 的周期公式。

因此 IR 必须为 Thumb 保存 `thumbCycles[opcode >> 8]`（`u8 fetchCycles`，1..4 足够）。
`JitDecodeThumb.cpp` 用 `extern const int32 thumbCycles[256];` 自行声明，**不要** include
`V7/GBACpu.h`（那个头会带进 `UPDATE_REG` 等宏，破坏 JIT 文件的独立编译）。

**为什么不是 `rawOpcode`**：V7 Thumb 的周期不需要 `opcode >> 8` 以外的任何 opcode 位。所有
handler 里对 `clockTicks` 的**赋值**只有条件分支的 `clockTicks = 3`，分支是否成立由运行期标志
决定，IR 用 `cond` 就能判断；其余都是 `+= 常数` 或 `+= CPUUpdateTicksAccess*`，常数部分由指令
形状决定。MUL 的 `+1..+4` 分档依据是 `reg[(opcode >> 3) & 7]` 的**旧值**，不是 opcode 位。

#### 5.1.2 访存周期 helper

V7 的数据访问周期由四个 inline 函数给出（`src/gba/V7/GBACpu.h:43-61`）：

| helper | 实际查表 | 用途 |
| --- | --- | --- |
| `CPUUpdateTicksAccess32(addr)` | `memoryWait32[(addr >> 24) & 15]` | 首次 32 位数据访问 |
| `CPUUpdateTicksAccess16(addr)` | `memoryWait[(addr >> 24) & 15]` | 首次 16/8 位数据访问 |
| `CPUUpdateTicksAccessSeq32(addr)` | `memoryWaitSeq32[(addr >> 24) & 15]` | 连续 32 位访问 |
| `CPUUpdateTicksAccessSeq16(addr)` | `memoryWaitSeq[(addr >> 24) & 15]` | 连续 16/8 位访问 |

它们**只查表，没有副作用**，不会像 V8 的 `codeTicksAccess*` 那样改 `busPrefetch`。所以 V7 的
后端可以直接调用这四个函数，不需要复刻 V8 的 `busPrefetch` 前导。

#### 5.1.3 ARM 逐类指令的周期与语义

下面所有"增量"都是**在 5.1.1 的基周期之上**再加的值。出处均为 `src/gba/V7/arm-new.h`。

| 指令类 | 周期增量 | 语义要点 |
| --- | --- | --- |
| 普通 ALU（Rd != 15） | `+ (regShift ? 1 : 0)` | 寄存器移位形式在宏开头 `clockTicks++` |
| ALU 且 Rd == 15 | `+ (regShift ? 1 : 0) + (S ? 1 : 0)` | 写 PC 后按新状态把 `reg[15]` 对齐到 4/2 字节并设 `armNextPC`；**没有**额外的取指 access 周期 |
| ALU 且 Rd == 15 且 S == 1 | 同上，另加模式切换 | `CPUSwitchMode(reg[17].I & 0x1f, false)` 对 **TST/TEQ/CMP/CMN 也执行**，这是 V7 与 V8 的硬差异 |
| MUL / MULS | `+2..+5` | 按 `rs = reg[(opcode >> 8) & 0xF]` 的绝对值大小分档；负数先取反 |
| MLA / MLAS | `+3..+6` | 同上，累加形式多 1 |
| UMULL / UMLAL / SMULL / SMLAL | `+2..+5`（长乘）/ `+3..+6`（长乘累加） | 仍按 Rs 分档 |
| STR / STRB | `+2 + Access32/16` | 字用 `Access32`，字节用 `Access16` |
| LDR / LDRB | `+3 + Access32/16` | 字用 `Access32`，字节用 `Access16` |
| LDR 且 Rd == 15 | 上面的 `+3 + Access` 再 `+2` | 然后把 `reg[15]` 对齐到 4 字节、设 `armNextPC`、`reg[15] += 4` |
| STRH | `+4 + Access16` | 半字写。**是 4，不是 2** |
| LDRH / LDRSB / LDRSH | `+3 + Access16` | **Rd == 15 也不重取 PC**（与字 LDR 不同） |
| 半字/有符号的 P/U/I/W 组合 | 同上（周期只看 L 和宽度） | V7 实现了全部 16 种组合，包括 V8 表里缺失的 `P=0,W=1` 六种。只有 `S=1 && L=0`（有符号存储 / STRD）未定义 |
| LDM / STM | `+2`，然后每个寄存器 `+1 + Access32`（第一个）/ `+1 + Seq32`（后续） | LDM 的 PC 槽 `+2 + Access`；STM 的 PC 槽 `+1 + Access` |
| B / BL / BX / SWI | `+3` | BX 根据目标 bit0 切 ARM/Thumb |
| SWP / SWPB | `+0`（只有基周期） | 内部仍调用 `CPUReadMemory` / `CPUWriteMemory`，Lua 钩子照常 |
| MRS / MSR | `+0`（只有基周期） | MSR 会 `CPUSwitchMode` / `CPUUpdateFlags`，见 5.3 |
| 单次传输 bit4 == 1 | — | 只在**寄存器偏移**形式（bit25=1，键 0x600–0x7FF）里未定义：128 个偶数键全实现，奇数键落 `default: CPUUndefinedException()`。**立即数**形式（bit25=0，键 0x400–0x5FF）由 `CASE_16` 覆盖全部 256 个键，bit4 是 `imm12` 的一位 |

补充：

- ARM 的 `reg[15]` 在执行期等于**当前指令地址 + 8**，`armNextPC` 等于当前地址 + 4。
- ARM 的 PC 写入（ALU/LDR/LDM）没有 V8 的 `ARM_PREFETCH` 重填；块出口只需要设 `armNextPC`
  和 `reg[15]`。
- 比较类指令（TST/TEQ/CMP/CMN）在 `Rd == 15` 且 `S == 1` 时，V7 会先更新标志，再
  `CPUSwitchMode`，后者又从 SPSR 重新装标志。净效果是"模式切换后的标志"，但周期上多 1，
  且 banked 寄存器会切换。

#### 5.1.4 Thumb 逐类指令的周期与语义

基周期见 5.1.1；下表是 handler 额外的增量。**大多数 Thumb handler 不加任何周期**。

| 指令类 | 周期增量 | 语义要点 |
| --- | --- | --- |
| 格式 4 的寄存器移位（LSL/LSR/ASR/ROR Rd, Rs） | `+1` | `thumb.h:534/562/599/640` |
| MUL | `+1..+4` | 按 `reg[(opcode >> 3) & 7]` 的大小分档（V7 Thumb 用 **Rs**，见 10.1 第 2 条） |
| 条件分支 | 成立时 `clockTicks = 3`（**赋值**，不是加）；不成立保持基周期 | 不能用"基周期 + 3" |
| 无条件 B / BL / BX / SWI | `+0` | 只有基周期 |
| PUSH | 每寄存器 `+1 + Access32`（第一个）/ `+1 + Seq32`（后续） | SP 在最后更新 |
| POP | 每寄存器 `+2 + Access32`（第一个）/ `+2 + Seq32`（后续） | POP 到 PC 时 PC 槽只加 `+Access`（**没有 +2**），然后 `armNextPC = PC`、`reg[15] = PC + 2` |
| LDM | 每寄存器 `+2 + Access32` / `+2 + Seq32` | 若 base 在寄存器列表里，最后不回写 base |
| STM | 每寄存器 `+1 + Access32` / `+1 + Seq32` | 第一个寄存器时回写 base |
| PC 相对 LDR | `+Access32` | 用 `CPUReadMemoryQuick((reg[15] & ~3) + imm)`，不经过 Lua READ 钩子 |
| 寄存器偏移 LDR/STR | 字 `+Access32`；半字/字节/有符号 `+Access16` | 偏移是**原始寄存器值**，不缩放 |
| SP 相对 / 立即偏移 LDR/STR | 字 `+Access32`；半字/字节 `+Access16` | 与 ARM 的单次传输公式不同，别混用 |
| 高寄存器 MOV/ADD/CMP | `+0` | 只有基周期 |
| 其他 ALU | `+0` | 只有基周期 |

补充：

- Thumb 的 `reg[15]` 在执行期等于**当前指令地址 + 4**，`armNextPC` 等于当前地址 + 2。
- Thumb 的 `POP {..., PC}` **不切换 ARM 状态**：V7 把读出的值 `& 0xFFFFFFFE` 后直接当 Thumb
  地址用。必须照抄，不能按 ARMv4T 架构手册"bit0 决定状态"去实现。
- Thumb 的 `LDM/STM` 与 V8 的"直接赋值 `clockTicks`"不同：V7 是逐寄存器累加，且 base 回写
  规则也不同。

### 5.2 PC 写入与模式切换

1. **ALU 写 PC 且 S=1**：V7 对**所有** ALU 子操作（包括 TST/TEQ/CMP/CMN）执行
   `CPUSwitchMode(reg[17].I & 0x1f, false)`，并 `clockTicks++`。V8 的 `C_CHECK_PC` 对比较类
   为空，不切模式。
2. **ALU 写 PC 且 S=0**：不切模式，但仍会把 `reg[15]` 对齐并设 `armNextPC`；也就是"比较类
   指令只要 Rd 字段是 15，也会跳 PC"。宏在 `dest == 15` 分支里先执行 `OPCODE2`，然后无条件
   对齐 PC。
3. **任何写 `reg[15]` 的指令都是 PC 写，哪怕它不是分支**（见 3.2 第 3 条的不变式）。
4. **对齐用切换后的状态，周期用切换前的指令集**——这两个判据相反，别合并：
   - `SUBS pc, lr, #4`（`0xE25EF004`）是 ARM 指令，但 SPSR 的 T 位让它返回 Thumb 代码。
     `CPUSwitchMode()` 已把 `armState` 置 false，所以**对齐**要按切换后的 `armState`（掩到 2 字节）。
     按切换前掩 4 字节会把 `0x08001D0A` 变成 `0x08001D08`，而内置 BIOS 与真 BIOS 的 SWI 返回
     序列都以这条收尾 ⇒ 每条 SWI 都回错 2 字节，几帧后画面停住 + 噪声。
   - 但**周期**不能跟着新状态走：解释器的 ARM `ALU_INSN` 宏无论切到哪个状态都按 32 位取指
     记账（`3 + ISREGSHIFT + codeTicksAccess32 + 2 * codeTicksAccessSeq32`）。判据是**当前
     指令的指令集**（`thumb`）。按新状态套 Thumb 的 16 位公式，每条"ARM 指令返回 Thumb"都少
     6 个周期，累积几帧后 IRQ 就晚投递一条指令。
   - Thumb 的 `MOV/ADD Hd, pc` 仍用 16 位公式。

### 5.3 MSR：所有写 CPSR 的整块拒绝

**已定（2026-11-03）**：把**所有写 CPSR 的 MSR**（寄存器形式和立即数形式，包括只写标志域的
`MSR CPSR_f`）整块拒绝，回退解释器。理由不是"控制域才会切模式"，而是 V7 的实现路径：

- V7 的 MSR CPSR 无论写哪些域，最后都会调用 `CPUSwitchMode(newValue & 0x1f, false)`
  （`arm-new.h:2423`、`:2539`）。
- 这个两参数重载会转到 `CPUSwitchMode(mode, saveState, true)`，即 `breakLoop = true`。
- 内部调用 `CPUUpdateFlags(breakLoop)`；`CPUUpdateFlags(true)` 在
  `armIrqEnable && (IF & IE) && (IME & 1)` 时执行 `CPU_BREAK_LOOP_2`，该宏会改写 CPULoop 的
  局部变量 `extCpuNextEvent` / `extTicks` / `cpuSavedTicks`。
- 即使只写标志域、模式位没变，`CPUSwitchMode()` 仍会做一次 banked 寄存器 save/restore，并对
  不支持的 `armMode` 调 `systemMessage()`——不是"只写 4 个 bool 标志"的纯操作。

因此"只写标志域的 MSR 可以安全 JIT"这个假设**不成立**。

**可以保留在 JIT 里的变体**（已核对，无 `CPUSwitchMode` / `CPUUpdateFlags`）：

- `MSR SPSR_fields, Rm` / `MSR SPSR_fields, #imm`：只在 `armMode > 0x10 && armMode < 0x1f` 时
  按字段写 `reg[17].I`，否则完全不写。
- `MRS`：CPSR 形式先 `CPUUpdateCPSR()` 再读 `reg[16].I`；SPSR 形式读 `reg[17].I`。两者都只
  影响目标寄存器。

**MSR 切 T 位的 +2 怪癖**：`CPUSwitchMode()` 只改 `armState` / banked 寄存器，**不调整
`reg[15]`**。如果一条 ARM 指令用 MSR 切到 Thumb，下一条 Thumb 取指时 `reg[15]` 仍是 ARM 约定
的"当前 + 8"，比 Thumb 约定多 2，导致 `armNextPC` 跳过一条 Thumb 指令。这是 V7 解释器的真实
行为，不是笔误；因为上面已经拒绝所有 MSR CPSR，后端不需要复刻它。

### 5.4 ROR by register：ARM 需要新的移位模式

V7 ARM 的 `ROR Rd, Rm, Rs`（`arm-new.h:616-636`）：

- `raw = reg[Rs].B.B0`；
- 若 `raw == 0`：值不变，但 `C_OUT = (value & 0x80000000) != 0`；
- 若 `raw != 0` 且 `raw & 0x1F == 0`：值不变，`C_OUT` 同样取结果符号位；
- 否则正常循环右移，`C_OUT` 取结果符号位。

V8 的 `SHIFT_ROR_REG_T` 在 `raw == 0` 时**不写 C**，所以 V7 ARM 不能直接用。

**已定（2026-11-03）：新增独立枚举 `SHIFT_ROR_REG_V7`，不用 flag。** 追加在 `JitShift` 末尾，
不改动现有枚举值（避免按数值序列化/缓存的假设失效）；`isRegShift()` 要把它加进去，ARM 的
`+1` 周期才和解释器一致。V8 构建里这个值永远不会被解码器产生，`case` 留着不影响 V8。

**Thumb 的 `ROR_RD_RS` 与 `SHIFT_ROR_REG_T` 完全一致**（raw=0 不写 C、`raw&31=0` 写符号位、
否则正常旋转），原样复用，不需要新枚举。计数来源都是 `reg[Rs].B.B0`（只取低 8 位）。

### 5.5 解码器差异

| 项目 | V8 现状 | V7 要求 | 处理方式 |
| --- | --- | --- | --- |
| Thumb 基周期 | 由 `codeTicksAccessSeq16` 决定 | `thumbCycles[opcode >> 8]` | `JitInsn.fetchCycles`，解码时填入 |
| ROR by register | `SHIFT_ROR_REG` / `SHIFT_ROR_REG_T` | raw=0 也写 C | 新增 `SHIFT_ROR_REG_V7` |
| 半字传输空洞 | `P=0,W=1` 且非 `U=1,I=1` 时拒绝（仅 LDRH/STRH） | V7 实现了这六种组合（0x02B/0x03B/0x06B/0x07B/0x0AB/0x0BB） | `USE_GBA_CORE_V7` 下**删除** `half && !sign && !pre && wb && !(up && immF)`；`sign && !load` 仍保留 |
| `^` 块传输 | 整块拒绝 | V7 用 `CPUSwitchMode` 实现 banked 寄存器 | V7 也整块拒绝 |
| 单次传输 bit4=1 | 拒绝（当未定义） | V7 在寄存器偏移形式下也是未定义 | 保持不变：`!immF && bit4` 拒绝 |
| 控制域 MSR | 拒绝 | V7 会切模式/改 T 位 | 拒绝，回退解释器 |
| 块出口预取 | 必须重填 `cpuPrefetch[0..1]` | 没有预取缓冲 | V7 只设 `armNextPC` 和 `reg[15]`，**不要**调用 `ARM_PREFETCH`/`THUMB_PREFETCH` |
| Quick 读取 | 只有 `JF_CLEAR_PREFETCH` | Thumb `LDR [PC,#]` 与 `LDR [SP,#]` 必须用 `CPUReadMemoryQuick` | `JF_QUICK`（`1 << 10`），只给这两类加载置位 |
| Thumb `0x5a` 的半字访问 | `JF_DATA32` 置位 | 用 `Access16` | `JF_DATA32` 只在 V8 置位 |
| BKPT / `armBreakpoint` | 整块拒绝 | 同 | `armBreakpoint()` 会退 PC、发调试信号、`clockTicks = -1`（停机），块后端无法建模 |

`JitInsn` 加 `u8 fetchCycles` 后字段总字节数 14 → 15，按 4 字节对齐后 `sizeof` 仍是 16。

### 5.6 V7 的 IR 解释器（`jitInterpInsn`）契约

**组织方式**：不新建 `JitInterpV7.cpp`，在 `JitInterp.cpp` 里用 `#ifdef USE_GBA_CORE_V7` 分两份。
新建 TU 要往 vcxproj 加"只在 V72XD/V72XR 参与编译"的条目，等于给工程文件增加按配置的复杂度，
收益为零。

必须遵守：

- **PC 模型**：`jitInterpInsn(insn, thumb, pc, &nextPc)` 的 `pc` 是当前指令地址；ARM 执行期
  `reg[15] = pc + 8`、`armNextPC = pc + 4`；Thumb 执行期 `reg[15] = pc + 4`、`armNextPC = pc + 2`。
- **周期**：返回值 = 基周期 + 增量。V7 没有 `cpuTotalTicks`，所以**只返回本条指令的周期**，
  不要往全局累加。
- **访存**：默认走 `CPUReadMemory` / `CPUWriteMemory` / `CPUReadHalfWord` /
  `CPUReadHalfWordSigned` / `CPUWriteHalfWord` / `CPUReadByte` / `CPUWriteByte` 包装，保证 Lua
  READ/WRITE 钩子和 IO 副作用照常。**只有三类读取用 Quick 版本**：
  1. 取指（ARM `CPUReadMemoryQuick(armNextPC)`、Thumb `CPUReadHalfWordQuick(armNextPC)`）。
     JIT 的取指由解码器直接读 `JitSource`，等价于 Quick；但 Lua **EXEC** 钩子激活时必须整体
     回退解释器。
  2. Thumb format 6：`LDR Rd, [PC, #imm8*4]`（`0x48`–`0x4F`）。
  3. Thumb format 9：`LDR Rd, [SP, #imm8*4]`（`0x98`–`0x9F`）。
  其余全部走带钩子的包装。写入侧**没有** Quick。
- **标志**：按 V7 的 `OP_*` 宏逐条实现。不要套 V8 的"写 PC 时不更新标志"规则。
- **模式切换**：`CPUSwitchMode()` 会调 `CPUUpdateCPSR()` / `CPUUpdateFlags()`，可能改变
  `armState`、banked 寄存器，甚至触发 IRQ 检查。要在和 V7 handler 相同的位置调用。
- **块出口**：IR 解释器只负责算 `nextPc` 和周期；`armNextPC`/`reg[15]` 的最终落点由块运行器
  统一设置。分支/写 PC 的指令可以在解释器内部先设一次，块运行器最后按 `nextPc` 覆盖。
- **`CPUCheckDMA` 的同步**：见 3.2 第 6 条。已核对的调用点只有六处：写 `DM0CNT_H`–`DM3CNT_H`
  （`GBA.cpp:2773/2809/2847/2884`，都在 IO 写分支里）和 LCD 事件处理（`:3879`/`:3899`，只在
  CPULoop 的事件块里）。所以**块内唯一可能触发它的是写 `0x04xxxxxx` 的指令**。
- **`armStopAddr` 不需要处理**：V7 在 `#ifndef FINAL_VERSION` 下有"执行到 `armStopAddr` 就自增
  PC"的代码，但它**实际上没有任何效果**——ARM 是 `armNextPC++` 之后紧接着
  `armNextPC = reg[15].I`（自增被覆盖）；Thumb 写的是 `armNextPC = armNextPC++`（后自增返回
  旧值再赋回去，等于不变）。`armStopAddr` 全仓库只有一个初始化赋值，且包在
  `#ifndef FINAL_VERSION` 里。V8 同样是死代码。不设块边界、不回退、不复刻。

### 5.7 V7 的块运行器与 `jitExecute()`

V7 的 `CPULoop` 与 V8 结构不同：V7 **没有** `armExecute()` / `thumbExecute()` 函数，解释器是
`#include "arm-new.h"` / `"thumb.h"` 直接展开在 `CPULoop` 的循环体里
（`src/gba/V7/GBA.cpp:3768/3776`）。所以 V7 的 `jitExecute()` 不能像 V8 那样"回退时调用
`armExecute()`"，只能把控制权交回 CPULoop 的内联解释器。

```c
// 返回值 >= 0：本次实际执行的 guest 周期总数；CPULoop 用它给 clockTicks 赋值。
// 返回值 <  0：一条都没执行；CPULoop 立即走内联解释器。
// 若执行了若干条后必须回退（IO 写、无法解码的下一条），返回已执行周期，
// 并置 jitForceInterp = true；CPULoop 下一次循环先跑一条内联解释器。
int jitExecute();
```

- 周期只在块运行器里累计（局部 `total`），**不碰** `cpuTotalTicks`。
- 每条指令后停的条件与解释器一致：`total >= 预算`、`holdState`、指令集切换。预算来自
  `*extCpuNextEvent`；块结束后 CPULoop 自己 `cpuNextEvent -= clockTicks`，绝不重复扣。
- 块出口：`armNextPC = 下一条`，`reg[15] = 下一条 + 4/2`；**没有**预取缓冲要重填。
- `JIT_NEED_INTERP` 的三类原因：会调用 `CPUSwitchMode(..., breakLoop=true)` /
  `CPUCheckDMA()`；语义依赖宿主的 UB（10.1 第 3 条）；V7 自己没把 PC 效果实现成跳转的加载
  指令（10.2 第 1、2 条）。
- `jitTracePc()` 原本写 `cpuTotalTicks`（V8 全局）。V7 的 `JIT_PC_TRACE` 版本待实现。
- `jitGetStats()` 在 V7 构建里不要 `memset` 成 0；`jitReportStats()` 同时写 `jit.log`
  （GUI 没有 stderr）。

### 5.8 V7 的单条参考函数（差分验证的前提）

`-jitverify` 需要一个"只执行一条"的参考实现。`arm-new.h` / `thumb.h` 对 CPULoop 局部变量的
依赖比想象中少：不直接用 `cpuNextEvent` / `ticks` / `holdState`，只用一个局部 `clockTicks`；
`ext*` 只通过 `CPUUpdateFlags()` / `CPUCheckDMA()` 间接出现；头文件里的 `return;` 只有两处，
都在 `#ifdef BKPT_SUPPORT` 的调试器路径。

**实际做法**（不改头文件一个字）：同一份头文件还要在 `CPULoop` 里以 void 上下文展开，把
`return;` 改成 `return clockTicks;` 会编不过。所以用 void 包一层、经静态变量回传：

```c
static int refTicks;
static void armExecuteOneBody() { int clockTicks = 0; #include "arm-new.h"
                                  refTicks = clockTicks; }
int armExecuteOne() { refTicks = 0; armExecuteOneBody(); return refTicks; }
```

早退时 `refTicks` 保持 0，验证器本来也跳过 BKPT。**不建议**把这两个 include 搬到新的 `.cpp`：
它们依赖 `GBA.cpp` 里大量全局、inline helper 和宏，单独建 TU 要先抽出一大段 prelude。

参考函数只执行一条指令，**不处理事件循环**。验证器应避免让它执行会触发 `CPUCheckDMA` 的 IO 写，
或在调用前把 `extCpuNextEvent` / `extClockTicks` 设成与解释器一致的值——这也是逐指令差分要
跳过 IO 写的原因之一。

---

## 六、x64 实现笔记

**读这一节的姿势**：这里的**结构和规则**可以移植，**编码和字节数**不可以。每小节开头都标了
可移植性。A64 的对应实现在 `jit-a64-backend.md`，本节只提供"为什么这么摆"。

### 6.1 文件与 ABI（x64 专属）

| 文件 | 作用 |
| --- | --- |
| `third_party/asmjit/` | 编码器库（Zlib），`f566238` 快照 |
| `src/gba/jit/JitEmit.h` | 后端接口 + `JitExecResult` + `JitChainSlot` + `JitCompiledBlock` |
| `src/gba/jit/JitEmit.cpp` | 唯一的实现：`JitBlock` 的一段前缀 → 一个原生函数（约 2400 行） |
| `src/gba/jit/JitCache.cpp` | 块缓存 + 编译结果缓存 + 热度阈值 + 影子对比 |
| `documentation/jit-emit-sample.c` | 教学样本：`0x080B66E2` 那个块（5 条 Thumb → 597 字节）逐行翻译成 C，附真实字节 |

asmjit 需要 **C++20**（`std::endian`），所以在 vcxproj 里是**按文件**加 `stdcpp20` +
`ASMJIT_STATIC` + `..\third_party\asmjit` 的，工程其余部分仍是 C++17；只有 x64 配置编它，
Win32/ARM64 走 `JitEmit.cpp` 的空实现。

```c
int fn(s32 budget, JitExecResult *out);      // 返回本次执行的周期数
// out->nextPc + out->status ∈ { NEED_INTERP, KEEP_PC, BUDGET, DONE }
```

| 寄存器 | 用途 |
| --- | --- |
| `rbx` | `&reg[0]`。所有模拟器全局量都是 `[rbx + disp32]`，disp32 在编译期由真实链接地址算出来；`checkedDisplacements()` 开机检查一次全部在 ±2 GB 内 |
| `r12d` | 周期累计 |
| `r13d` | 周期预算 |
| `r14` | `JitExecResult*` 输出指针 |
| `rdi` | 当前访存地址（跨 helper 调用保持） |
| `rsi` | 当前访存写回值（跨 helper 调用保持） |
| `rax rcx rdx r8 r9 r10 r11` | 临时，helper 调用会破坏 |

> **给 A64 的两个提醒**：① A64 用 `movz/movk` 绝对寻址，不存在 ±2 GB 位移限制，所以
> `jitNativeAvailable()` 可以只返回 true，**不要**机械照搬 `checkedDisplacements()`；
> ② `gJitShiftCarry` / `jitShiftReg()` / `ShiftKind` 定义在 `JitEmit.cpp` 的匿名 namespace
> 里，**没有头文件声明**，A64 文件必须自带一份等价实现。

### 6.2 退出协议（可移植）

规则见 3.1 第 5–8 条。这里只补一条**踩过的坑**：

`NEED_INTERP` 桩必须发在 `emitEpilogue()` **之后**。原来桩紧跟 `checkBudget()` 内联发出，
于是每条 IO store 都是"原生跑完 → 落到桩上 → 报 NEED_INTERP → 解释器再跑一遍"，栈指针被减
两次（`push {r7,lr}` 的 sp 差 8），AoS 直接白屏。现在桩只能被跳进来。

**拒绝编译的情形**（`nativeSupported()`，前缀在它前面截断）：

- `d==15 && S` 的 ALU；
- 写 PC 的 MUL（`RdHi`/`RdLo` 任一是 15）；
- 写 PC 的 SWP；
- `ldrb/ldrh/ldrsb/ldrsh pc` 与非前索引的 `ldr pc`；
- **基址是 r15 且回写的 load/store**、基址 r15 且回写的 LDM/STM；
- 写 CPSR 的 MSR；
- SWI / UNDEF。

这些就是 3.2 第 3 条那类"写 PC 但不是跳转"的绕一条，块表达不了。**A64 的清单应该一模一样**
（判据是指令语义，不是编码），差别只在怎么写 `NEED_INTERP` 出口。

### 6.3 块链接（可移植）

`JITX_DONE` 出口里嵌一个 `JitChainSlot`（在 `JitCodeEntry` 里，不在代码页上），初值 0 表示
"回 epilogue"；`jitExecute()` 第一次从这个出口回来时，如果后继块已经有机器码，就把后继的
`chainEntry`（prologue 之后那条指令的地址）写进槽里。之后这条出口直接 `jmp rax` 跳过去，
`rbx/r12/r13/r14` 和栈帧全程保持活跃，一次原生调用跑完一整条链。

安全约束见 3.4。**踩过的坑**：最初"任何编译都清空所有链接"会把刚建好的热循环拆掉；改成"只拆
相关链接"却用线性扫描，600 帧累计扫了 **2946 万**次，比链接省下的还多，实测反而慢 2–9%。
现在每个代码槽挂一条双向链表、每个槽位记住自己的节点。

**净收益只有 0.3–1.4%**（噪声量级）。它确实把 dispatcher 的入块次数降了约 29%（200 帧：
306 万 → 219 万），说明链是连上了，但每块真正的大头在块体（标志位、周期、预算、helper）。
代码保留（等价性全绿、三个 ROM 都不是负收益），但要清楚它买到的不是加速，是"链足够长以后
（比如将来做 superblock/trace）才有用的基础设施"。

### 6.4 内联快路径（规则可移植，编码不可）

`CPUReadMemoryWrapped()` 在 `case 2/3` 里就是 `READ32LE(&workRAM[address & 0x3FFFC])`，前面却
要走一整个 helper（Lua 钩子检查 + 大 switch）。现在生成代码先判 region，命中就查
`memoryMap[region]` 直接取数，否则回退 helper。

**能内联的区域按读宽度分**：

| 宽度 | 可内联 region |
| --- | --- |
| 字 / 字节 | `2..3`（EWRAM/IWRAM）、`5..7`（palette/VRAM/OAM）、`8..12`（ROM 镜像） |
| 半字 | `2..3`、`5..7`（ROM 半字读仍走 helper，见下） |

**四条规则必须照抄（它们是语义，不是优化）**：

1. **区域判断不能用 `JIT_CODE_WRITE` 的 `(addr & 0x0E000000)`**：那个掩码会把 `0x12xxxxxx` /
   `0x13xxxxxx` 当成 EWRAM/IWRAM，但 `CPUReadMemoryWrapped()` 按完整的 `addr >> 24` 分派，
   这些镜像其实落到 `unreadable`（返回 open bus）。必须严格判 region 值。
2. **非对齐字读要循环右移**：`CPUReadMemoryWrapped()` 结尾有 `_rotr(value, (address & 3) * 8)`。
   对齐地址可以跳过。
3. **半字奇地址不是 16 位字节交换**：`CPUReadHalfWordWrapped()` 结尾是
   `value = (value >> 8) | (value << 24)`，`value` 是 `u32`，所以低字节被送到 **bit 24-31**。
   对零扩展半字是 `ror 32 位, 8`，**不是** `ror 16 位, 8`。有符号版本
   `CPUReadHalfWordSignedWrapped()` 再取 `(s8)value`（高字节符号扩展），`LDRSH` 最后再 `(s16)`
   一次，所以快路径是"右移 8 位，再取低字节符号扩展"。
4. **`memoryMap[]` 有空洞**：初始化只写了 8/9/10/12，**11 号（`0x0B000000`）一直是
   `&dummyAddress` + mask 0**。解释器的 switch 直接读 `rom`，所以行为一直是对的；但快路径按
   `memoryMap[11]` 取数会读到 0。补上 `memoryMap[11]`（V7/V8 各一行）后 `-jitverify` 从
   95 个块差异归零。

**ROM 半字读是唯一不能内联的 ROM 路径**：`CPUReadHalfWordWrapped()` 在 `case 8..12` 里先判
`address == 0x80000C4/C6/C8`，命中就调 `rtcRead()`；而 `rtcEnabled` 是 `RTC.cpp` 的
file-static，JIT 代码看不到。让生成代码再复刻一遍这个判断，等于把 RTC 状态机的一部分搬进
机器码；不值得。三个测试 ROM 里没有半字访问那三个 RTC 地址（内联版本也全绿），但这是语义
正确性问题，不是性能问题。

**IO（region 4）永远不内联**：它要看 `ioReadable[]`、还可能清 `systemCounters.lagged`，不是
纯读。EEPROM（13）、SRAM/Flash（14）、BIOS/open bus 同理走 helper。

### 6.5 已落地的优化与实测（思路可移植）

| # | 优化 | 说明 |
| --- | --- | --- |
| 1 | 原生标志位直接 `SETcc` | 见 4.5。一条 ADD 从 102 字节降到约 70 字节 |
| 2 | EWRAM/IWRAM/ROM 读内联 | 见 6.4 |
| 3 | 块链接 | 见 6.3，净收益 0.3–1.4% |
| 4 | 编译阈值 `JIT_COMPILE_AFTER = 8` | 见 2.4 |
| 5 | 区域判断复用寄存器、对齐读跳过轮换、helper 参数移进 slow 分支 | `emitLoadStore()` 的收尾清理 |
| 6 | 链出口的两条 `outPc/outStatus` 存储移到"未链接"分支 | `emitChainExit()` |

**性能（2000 帧，`-frameskip 10 -synchronize 0 -vsync 0 soundOff=1`，多轮交替取最快，
比 = 解释器时间 / JIT 时间）**：

| ROM | 解释器 | JIT | 比 |
| --- | --- | --- | --- |
| mario | 3.62 s | 3.27 s | 1.11x |
| sonic | 3.11 s | 2.54 s | 1.22x |
| aos | 2.96 s | 2.40 s | 1.23x |

**演进对照**（同一口径，600 帧，含每 10 帧一次的渲染）：机器码刚落地时 0.89–0.99x →
`SETcc` + 内联读之后 0.97–1.04x → 再加块链接 0.97–1.04x → 无限跳帧 1.10–1.23x。
**别把收益全算给某一项**：最后那一跳是"抽掉最后一帧渲染"+"编译阈值提到 8"+"内联读快路径"
叠出来的。

**为什么整帧只有持平**：块平均只有约 5 条指令（`blocksRun / 帧 ≈ 15 400`，
`指令 / 帧 ≈ 75 000`），而每次进出块要付 7 个 push + 7 个 pop + 装载 `&reg[0]` + 预算/结果
指针约 20 条宿主指令，加上 `jitExecute` 里 `getEntry()` 的 10 来条——**平均每条 guest 指令要
摊 6 条左右的开销**，和指令本体的 15 条是一个量级。这也解释了为什么"把预算桩挪到函数末尾"
"不必读 PC 时不写 `reg[15]`"这两项优化测不出差别（只减少 1–2 条/指令）。

> **A64 的起点会比 x64 好**：A64 的 `adds/subs` 直接产出 guest 需要的 N/Z/C/V，`cset` 一条
> 就能落标志，不需要 x64 那种"先 64 位加、再 shr 取进位"的补丁；固定 32 位指令长度也让
> prologue/epilogue 更短。但"块平均 5 条指令"这个结构性问题是同一个，所以**A64 的第一版
> 大概也是持平**，别指望一上来就加速。

### 6.6 已知问题与优化项

这一节记"已经核实"的点，每条都标明是**真问题**、**调试工具限制**，还是**看着不一致但必须
保留的语义**，避免后来者把最后一种当 bug 改掉。

#### 6.6.1 调试工具

`dumpCompiledBlock()`（`VBA_JIT_ASM=<file>` 时导出每个块的 IR + 机器码字节）：

- 缓冲区按 `code->code_size()` 分配（`malloc`/`free`，只在开关打开时走）。原来是
  `static u8 bytes[16384]`，超过 16 KB 的块（展开的 LDM/STM 很容易到）会**静默**消失，
  看着就像"这个块没编译"。
- 块数上限用 `VBA_JIT_ASM_LIMIT=<n>`（默认 200，`0` = 不限）。到达上限时往 dump 文件里写一行
  `=== dump limit N reached; ...`，不再无声截断。

#### 6.6.2 性能优化项

1. **条件判断打包读标志 —— 评估后否决。** 原本的想法是：`N_FLAG/C_FLAG/Z_FLAG/V_FLAG` 是连续
   4 个 `bool8`，一条 dword load 就能同时拿到四个字节。逐条对过代码后，这个前提基本不成立：
   - EQ/NE/CS/CC/MI/PL/VS/VC（8 个）本来就是**一次** byte load（`cmp byte [Z_FLAG], 0`），
     没有"2-4 次"可省；
   - 只有 HI/LS/GE/LT/GT/LE 是两次 load，其中 GE/LT/GT/LE 要比 N 与 V **是否相等**，dword
     load 拿到的是两个分离字节，还得 `shr`/`movzx` 归一化才能 `cmp`，指令数反而多一条；
   - 更要紧的是：`emitFlagsNative()` 用 4 条 `setcc byte [N_FLAG]` 分别写四个字节，紧跟一条
     跨这四个字节的 dword load 属于部分重叠的 store-to-load forwarding，Intel 上不能转发
     （约 13 周期）。而"条件指令紧跟标志写入"正是热路径常态，这一下比省掉的那次 load 贵得多。

   结论：净收益 ≈ 0，还要把 codegen 绑死到 BSS 布局上。**不做。** 真要做，前提是先把四个标志
   改成一次 32 位 store 打包写。
2. **内存区域判定用位图**：`emitLoadStore()` 的快路径区域检查现在是最多 6 条 `cmp/jcc`。
   区域号只有 0..15（就是 `addr >> 24`），可以做成一个 16 位常量掩码
   `bt ecx, r8d; jnc slow`（字/字节与半字用两张掩码）。可读性会下降，收益只在访存密集的块上，
   留到性能阶段再做。
3. **`emitCondSkip()` 的分支顺序**：EQ/NE 最常见，目前已经是第一组；HI/LS/GE/LT/GT/LE 的
   "提前跳到 skip"写法可以按实际分布重排，减少热路径的 taken branch。属于微优化，要先有
   profile 数据（`-hashtrace` 全绿只是前提，不是依据）。

#### 6.6.3 验证盲区

`JitCache::runBlock()` 的影子比较（`VBA_JIT_SHADOW=1`）只覆盖
`code->insnCount == b->count && !blockWritesMemory(b)` 的块。所以**所有写内存的块**
（STORE/STM/PUSH/POP/SWP）和**被后端截断前缀的块**都不在影子比较里，只能靠 `-jitverify` 与
`-hashtrace`。这不是 x64 的错，但排查"只有某个游戏出错"时要知道影子模式给不了信号。

#### 6.6.4 看着不一致、但**不要**"优化"的语义

下面几条都逐字对照过 `arm-new.h`/`thumb.h`/`JitInterp.cpp`，改成统一就会破坏逐指令等价：

- **PC 写回掩码三种**：ARM `LDR pc` 用 `~3`；ARM `LDM {pc}` **不掩码**；Thumb `POP {pc}` 用
  `~1`。
- **写回时机四种**：STM **每写一个寄存器**就写回基址（中途被 IO helper 打断时 `reg[n]` 已是
  部分更新的值）；ARM LDM 只在 `W && base 不在 list` 时末尾写回；Thumb LDM 只要 base 不在 list
  就写回，**不看 W**；单次 store 里 `pre && wb` 时 STR/STRB 在 store **之前**写回、STRH 在
  store **之后**写回，`!pre && wb` 一律在 store 之后。
- **单次 load 的 `d != n` 条件**：`Rd == Rn` 时**不写回**。实现时很容易只写 `if (wb)`。
- **乘法计时里的 `~Rs` 只对 `MUL/MLA/SMULL/SMLAL` 生效**，`UMULL/UMLAL` 不取反。
- **周期记账顺序**：先写结果/写回寄存器，再读标志，最后 `addFetch/checkBudget`；`addFetch` 会
  改宿主标志，顺序反了就会读错。

如果以后要动这些，先补一条 `-jitverify` 用例，再改。

### 6.7 找热区的工具（MSVC）

本机 `/Zi` + `/DEBUG` 生成 45 MB PDB，`/GL` + `/LTCG` 也在，所以 VS 的 **Performance
Profiler → CPU Usage**（ETW 采样）可以直接 "Attach to process"，不需要 MSBuild；
`Instrumentation` 插桩会淹没纳秒级路径，别用。本机 Insiders 没装命令行采样器，系统级采样用
Windows Performance Recorder（`-start CPU` / `-stop` 出一份 ETL，再用 WPA 看）。PGO 也可用
（`/FASTGENPROFILE`，`pgomgr /summary` 能看粗粒度函数计数）。

**关键限制**：asmjit 生成的代码在 `VirtualAlloc` 出来的 RWX 匿名页上，没有模块名也没有 PDB，
profiler 里只会显示成 `Unknown`/裸地址；而 1 ms 的 ETW 采样对"每块几十纳秒"根本没有分辨率。
看 JIT 内部还是得靠 `VBA_JIT_ASM` 导字节、`__rdtsc` 埋点（`jit.log` 的 `compileTicks`）、
VTune/AMD uProf 的 PEBS/IBS 硬件采样，以及最省事的端到端 A/B。

---

## 七、AOT 导出

### 7.1 定义清楚

"导出原生游戏" = 生成一个 **不模拟 CPU** 的 Windows 可执行文件：

```
game.exe
 ├─ vba 运行时（PPU 六种模式、APU、DMA、定时器、IRQ、存档、输入、窗口）
 ├─ ROM 数据（嵌入）
 ├─ 预编译的游戏代码（原生函数，按 guest PC 索引）
 └─ 解释器（兜底，只处理没预编译到的 PC）
```

覆盖率是硬指标。多数商业 GBA 游戏完整玩一遍能覆盖 ROM 代码的 95–99%；剩下的是没走到的分支
和 RAM 里生成的代码。

### 7.2 三种产物形态

| 形态 | 做法 | 优点 | 缺点 |
| --- | --- | --- | --- |
| **AOT-C** | 生成 `.c`，用 `cl.exe` 编成 obj 再链接 | 可读、可调试、零新依赖、复用 C 后端 | 导出时要装 MSVC；编译慢 |
| AOT-blob | 直接生成机器码字节数组塞进 `.rdata` | 导出即用 | 要处理重定位；不可读 |
| 完全静态 | 静态解析所有间接跳转，去掉解释器 | 产物最小 | **GBA 上不成立**，放弃 |

**先做 AOT-C。** 它和 JIT 共用同一个 IR 和 C 后端，边际成本最低——这也是 4.2 里"IR 必须有"
的第一条理由。

### 7.3 流程（profile-guided）

1. `vba.exe --aot-trace game.gba` 正常玩一遍（或用 `.vbm` 回放），记录每个执行过的块：
   `{pc, thumb, 字节哈希, 执行次数}` → `game.trace`。
2. `vba-aot.exe game.gba game.trace`：
   - 按 PC 从 ROM 重新解码，校验字节哈希（ROM 被改过就丢弃该块）
   - 走 IR → C 后端 → `game_blocks.c`（每块一个 `static void blk_XXXXXXXX(Cpu*)`）
   - 生成按 PC 排序的派发表
3. `cl` 链接 `game_blocks.c` + `vbaruntime.lib` + 嵌入的 ROM → `game.exe`。
4. 运行时未命中 → 解释器，并在退出时打印覆盖率报告。

### 7.4 真实难点

- **RAM 代码**：不少游戏把热点循环拷到 IWRAM 再跑（为了零等待）。这类代码在 AOT 里永远走
  解释器，除非做"运行时二次 JIT"——那 AOT 就退化成"JIT + 预热的块缓存"了。
- **BIOS**：SWI 应该做成 HLE 运行时函数（`Cpu*` 上直接实现），不要真去执行 BIOS 镜像。
- **时序**：AOT 必须复用同一套周期模型，否则同一个输入在 exe 和模拟器里结果不同。
- **确定性**：AOT 产物建议**要求**与 `.vbm` 逐帧一致，否则调试成本翻倍。

### 7.5 便宜的中间态：块缓存落盘

把编译好的块按 `ROM 哈希 + 块 PC` 序列化到 `game.vbajit`，下次启动直接映射，省掉首帧的编译
抖动。这一步几乎就是 AOT 的雏形，也是验证"块序列化"这条链路最便宜的方式。

### 7.6 文件规划

```
src/gba/jit/JitC.cpp            IR → C（也是 AOT 的后端）
src/gba/jit/aot/AotTrace.cpp    执行轨迹记录
src/gba/jit/aot/AotEmit.cpp     IR → C 文件 + 派发表
tools/vba-aot/                  导出工具（或 vba.exe 的一个命令行模式）
src/gba/jit/aot/RuntimeMain.cpp 导出 exe 的 main / 窗口 / 输入
```

---

## 八、阶段状态与路线

原计划分六阶段，现在只剩后半段没做。**每个阶段都有独立的回退开关和验收标准**；任何一阶段
不达标就停在那里，不留半成品。

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| **Phase 0** 测量 + 便宜优化 | Lua EXEC 钩子闸门、`CPUMasterCodeCheck` 短路 | **已完成**（profile 计时没做，结论已由纯 CPU 基准给出，见 2.1） |
| **Phase 1** IR + 解码器 + IR 解释器 | `JitIr.h` / `JitDecode*` / `JitInterp` / `JitCache` / `JitVerify` / `selftest` | **已完成**（两核；`JitC.cpp` 未动笔） |
| **Phase 2** x64 机器码后端 | `JitEmit.cpp` + asmjit | **已完成（V7 + x64）**；验收指标"纯 CPU ≥ 1.8x"**未达到**，实测 1.11–1.23x |
| **Phase 3** 内存内联 + 标志优化 | 内联快路径、`SETcc`、块链接 | **部分完成**（6.4/6.5）；标志惰性求值、块缓存落盘未做 |
| **Phase 4** ARM64 后端 | asmjit a64 重写发射逻辑 | **未开始**，见 `jit-a64-backend.md` |
| **Phase 5** AOT 导出 | 见第七节 | **未开始** |

**为什么 Phase 2 的 1.8x 没达到**：那个数字是按"纯 CPU、寄存器全在宿主寄存器"的理想估算的。
实测下来块平均只有 5 条指令，进出块的开销摊到每条 guest 指令上是 6 条宿主指令，抵消了大部分
收益。所以 Phase 3 里真正值钱的不是"更多微优化"，而是**把块做大**（见 6.5 下一步）。

**Phase 4 的验收**：ARM64 机器上同一套差分测试通过；性能不低于 x64 的 70%。调研阶段的事实、
A64 寄存器/帧约定、逐指令映射和验证矩阵已单独记录在 `jit-a64-backend.md`。

> ⚠️ **A64 调研文档写在另一台 Windows on ARM64 机器上；当前这份工作副本在
> x64 机器上**，且这里没有 ARM64 交叉编译器和 arm64 MFC 库。`JitEmitA64.cpp`
> 已写出并通过语法/编码检查，但 Phase 4 的编译、链接和三层验证只能在目标
> ARM64 机器上做。按 9.1 的合入门槛，验证之前不合入。细节见
> `jit-a64-backend.md` 1.2 / 11.1。

**Phase 5 的验收**：3 个 ROM 导出的 exe 能通关（或跑到与模拟器相同的进度）；覆盖率 ≥ 95%；
与模拟器同一 `.vbm` 回放，逐帧 HashTrace 一致。

---

## 九、怎么验证（四层，别跳）

重编译器的错误有两种：**解码错了**（字段拆错）和**语义错了**（行为与解释器不同）。前者看
反汇编能抓，后者只有差分测试能抓。跑游戏看花不花屏是最没用的验证方式——错一条指令可能几万帧
后才炸，也可能永远炸不了。

### 9.1 四层总览

| 层 | 手段 | 覆盖 | 成本 |
| --- | --- | --- | --- |
| 0 | 解码空间全扫 | 解码器接受/拒绝的集合 | 1 秒 |
| 1 | `selftest.cpp` | `JitInsn` 字段拆解 | 秒级 |
| 2 | `-jitverify` 单条 pass | 单条指令的寄存器/标志/周期 | 分钟级 |
| 2.5 | `-jitverify` 块级 pass | 整块的寄存器/内存/周期/块边界 | 分钟级 |
| 3 | `-hashtrace` | 端到端逐帧等价 | 十分钟级 |
| — | `VBA_JIT_SHADOW=1` | 生成代码 vs IR 解释器逐块 | 分钟级，慢一倍 |

**这四层缺一不可**，而且每一层都真的抓到过别层抓不到的东西（见第十节）。移植到 A64 时按
3.8 的顺序逐层打开。

> 🔴 **合入门槛**：不在没有 `-hashtrace` 逐帧基线的情况下合入任何 JIT 代码。
> "源码写完了""编译过了""单条差分绿了"都不是合入理由；只有第 3 层给得出
> `ram` 列逐帧一致的证据才算。这条也意味着**在跑不了目标架构的机器上写出来的
> 后端必须停在未合入状态**（见 `jit-a64-backend.md` 1.2）。

### 9.2 第 0 层：解码空间全扫（`-jitverify` 自带，1 秒）

**两张分派表都是少数几位的函数**（ARM 按 bits 27-20 与 7-4，256×16；Thumb 按 bits 15-6，
1024 项），所以"模拟器认为哪些字是未定义"可以穷举，不用执行就能和解码器对账：

```
arm decode space
  4096 words checked, 0 disagreements
thumb decode space
  1024 words checked, 0 disagreements
```

两类不一致都算 bug：解码器执行了模拟器判未定义的字（生成的代码会做解释器永远不会做的事），
以及解码器报异常而模拟器在执行。解码器**拒绝解码**（返回 false）不算——调用方会退回解释器。

这一层立刻抓出过 `bx lr`（`0x4770` 被当成 BLX）、`swi` 的判定位写成 bit24（吞掉协处理器行
`0xDx`/`0xEx`）之类"只靠随机 ROM 字节要碰运气"的错误。

### 9.3 第 1 层：解码自测（秒级）

`src/gba/jit/selftest.cpp` 是宿主测试，不需要 MSVC，clang 直接编（命令见 1.3）。同一份源码要
**两种模式各编一次**，V7 分支否则根本没被编过。

当前规模：**V8 1101 项、V7 1207 项断言**。覆盖两套指令集的 ALU/移位/访存/乘法/块传送/分支/
SWI/MRS/MSR/SWP/PUSH/POP 字段拆解，以及块边界（分支结束、写 PC 结束、`POP {pc}` 结束、
1 KB 页边界截断、`JIT_MAX_INSNS` 截断）和"整块拒绝"（BKPT）。**它只证明"编码读对了"，不证明
"行为对"。**

V7 模式下 `thumbCycles` 是测试自己定义的一张合成表（1..4 循环），所以这一层验证的是"解码器按
`opcode >> 8` 索引"，不是表的真实数值——真实数值只能靠第 2 层以后的差分。

### 9.4 第 2 层：逐指令差分（`-jitverify`）

对 ROM 里的每一条指令，从**完全相同的机器状态**出发跑两遍（ARM 和 Thumb 各一趟）：

| | 路径 |
| --- | --- |
| A | 真解释器（V8 用 `armExecute()` 把 `cpuNextEvent` 设成 1；V7 用 `armExecuteOne()`） |
| B | `jitDecodeOne()` + `jitInterpInsn()` |

比对 `r0-r15`、`CPSR`、`N/Z/C/V`、周期（V8 是 `cpuTotalTicks` + `busPrefetchCount`，V7 是
返回的本条周期）。

输出长这样：

```
arm pass
MISMATCH 08001234  raw=...  op=alu  C
  19987 tested, 1 failed, 1200 skipped (memory writes, SWI, undefined), 13 undecodable
  failures by op: alu=1
```

`-jitverify N` 的 N 是每趟从 ROM 头部往后扫的指令条数上限。**N 必须覆盖到 ROM 里最远的代码**：
`aos` 那条后索引 `ldr pc` 在 ARM 段第 6756 条，当时 `N=3000` 一直没抓到。

两趟都要看：Thumb 解码器是另一份代码，ARM 全绿不代表 Thumb 全绿。ROM 里 ARM 和 Thumb 是交错
存放的（Thumb 代码会被当成 ARM 指令扫一遍），所以极少数"错位"位置的报错通常是数据被误解码，
不是真 bug。

### 9.5 第 2.5 层：块级差分（`-jitverify` 自带）

逐指令差分只能证明"单条指令对"，证明不了"块跑起来对"：块会自己决定在哪停、会在中途切指令集、
会把预取和 `armNextPC` 留给下一条。这些都不在单条比较的视野里，而且错了的表现是**卡死**而不是
某个寄存器错。所以 `-jitverify` 在逐指令两趟之后再跑一趟：

| | 路径 |
| --- | --- |
| A | 解释器连跑 `block.count` 条（V8 每次 `cpuNextEvent = cpuTotalTicks + 1`，与 `CPULoop` 的驱动方式一致） |
| B | `jitRunBlockForTest(&block)` —— 真解码器解出的块，真走 `runBlock()` |

比对 `r0-r15`、`CPSR`、周期、`armNextPC`、`busPrefetchCount`、`cpuPrefetch[0..1]`。
**内存也比**：跑之前快照 workRAM + IWRAM + palette + VRAM + OAM + ioMem，A 跑完存副本、还原，
再跑 B，逐字节比 —— 所以 `STR`/`STM`/`PUSH`/`POP`/`SWP` 都进了这一层。

> **IO 也进快照**：少数 IO **读**会写（DMA hack、lag counter）。只快照 RAM 会报出
> "一条 store 改了两个寄存器"这种假象——store 不可能改寄存器，看到这种签名就该怀疑快照不全。

`SWI` / `UNDEF` / 控制域 `MSR` 仍然跳过（异常与模式切换改的状态没法用内存快照还原）。
`runBlock()` 在 `holdState` / 事件边界 / 指令集切换处停下时，验证器要能看出"这是跑了一半的
块"而不是"块跑完了"——否则会拿半个块和跑完的解释器比。

### 9.6 第 3 层：端到端（`-hashtrace`）

这一层才是最终验收：同一个 `.vbm` 回放，JIT 开/关各跑一遍，逐帧状态哈希必须逐字节相同。

```
VBA_VS_Latest_V72XR.exe -rom aos.gba -play aos.vbm 0 -synchronize 0 -frameskip 10 \
        -vsync 0 -quitAfter 6000 -nojit -hashtrace D:\interp.csv
VBA_VS_Latest_V72XR.exe -rom aos.gba -play aos.vbm 0 -synchronize 0 -frameskip 10 \
        -vsync 0 -quitAfter 6000 -jit   -hashtrace D:\jit.csv
```

**只比 `ram` 列。** `full` 是 `emuWriteMemState` blob（含 pix/sound/rtc/OSD）的 crc32，文件头
自己写着 "may vary with OSD timing"：不渲染时 pix/OSD 留在什么状态两边不同，实测
`-frameskip 9` vs `-frameskip 10`（都 `-nojit`）`ram` 0 差异、`full` 差 5932 行。三个对照钉死
了原因：无限跳帧 `-jit` 重跑两次 `full` 0 差异（不是随机抖动）、`-frameskip 9` 的 `-jit` vs
`-nojit` 0 差异。**无限跳帧不改变任何架构状态。**

两次都显式传 `-nojit` / `-jit`，不要依赖 `vba.ini` 里的当前值：`-jit` 会在正常退出时写回设置，
第一次对比之后 ini 就可能已经是开着的了。跑之前先删掉同目录的 `.sav`。

`-quitAfter` 走 `systemCounters.frameCount == quitAfterTime`，是按帧号的硬边界，所以两次运行
的帧数严格相同，不需要"比较公共前缀"。

**排查入口**是第 2.5 层那套轨迹：`JIT_PC_TRACE=1` 重编，`VBA_JIT_PCTRACE=<文件>` 各跑一遍，
逐条比 `(pc, lr, sp, cpuTotalTicks)`。第一次分叉如果只有 `lr` 不同而 `pc`/`sp`/`ticks` 都相同，
那就不是事件时刻的问题，而是上一条指令算错了值：把 16 个寄存器和标志位一起打出来，对着出错
地址反汇编，就能看出是哪条指令、哪一半算错。

### 9.7 覆盖盲区（别指望它全绿）

| 情况 | 第 2 层（逐指令） | 第 2.5 层（块） |
| --- | --- | --- |
| `STR` / `STM` / `PUSH` / `POP` / `SWP` | **跳过**：两条路径都会写内存，第二遍的起点就和第一遍不同了 | **覆盖**：跑前快照、A 跑完存副本、还原后再跑 B |
| 模拟器判未定义的字 / `JOP_UNDEF` | **跳过**：会进 BIOS 的未定义异常处理，没有 BIOS 文件时还弹模态框；而且那是 ROM 数据，不是代码 | 跳过（整块不比） |
| `SWI` | **跳过**：进 BIOS 处理程序，副作用收不回来 | 跳过（整块不比） |
| `MSR` 写控制域 | **跳过**：会把 CPU 切进 ROM 字节恰好拼出来的任意模式，`CPUSwitchMode` 会弹框 | 跳过（banked 寄存器不在比较范围） |
| BKPT、表里指向 `armBreakpoint` 的字 | 解码器**整块拒绝**，这一条根本不会跑到 | 同左 |
| ROM 前 0xC0 字节 | 跳过（那是头，不是代码） | 跳过 |
| 访存地址 | 全部落在 work RAM（`0x02000000 + ...`），避免触发 IO 副作用 | 同左 |

所以"全绿"的含义是：**两套指令集里 ALU / 移位 / 乘法 / 分支 / LDR / LDM / 逐条访存写回 /
未定义异常 的语义和周期与解释器逐条一致（第 2 层），并且整块的寄存器、内存、`armNextPC`、
预取、周期也和解释器一致（第 2.5 层）**。还剩三类只能在第 3 层验：`SWI` 的 BIOS 副作用、
异常入口、以及模式切换后的 banked 寄存器。

### 9.8 失败怎么读

1. 先看 `op=`：`alu` 多半是标志位或移位边界（shift=0 / shift=32 / RRX）。
2. 再看差异字段：`reg` 是数据错，`C`/`V` 是进位溢出公式错，`cycles` 是记账错。
3. 下面那行 `jitDumpBlock` 的输出就是这条指令的 IR，对着 ARM ARM 手算一遍。
4. 只有 `cycles` 差 1：检查 `codeTicksAccess*` 的参数。解释器传的是 `armNextPC`，而 handler
   里的 `armNextPC` 等于 **pc+4**，不是 pc。

### 9.9 PC 的三个值

解释器执行一条指令时同时存在三个"PC"，混用任何两个都会错：

| 名字 | 值 | 谁用 |
| --- | --- | --- |
| 指令地址 | `pc` | 日志、`JitBlock.guestPc` |
| `armNextPC` | ARM：`pc + 4`；Thumb：`pc + 2` | 所有 `codeTicksAccess*`、`armNextPC - 4` 的日志 |
| `reg[15].I` | ARM：`pc + 8`；Thumb：`pc + 4` | 指令读 PC，所以 ARM 的 `BL` 存 `reg[15] - 4` 正好是 `pc+4` |

进入 handler 时 `armNextPC` 已经指向**下一条**指令，`reg[15]` 是 PC+8/PC+4。

`oldArmNextPC` 就是**当前指令的地址**（= `pc`），不是"上一条"：循环入口处 `armNextPC` 已经
指向要执行的那条指令，`reg[15]` 才是它 +4；`u32 oldArmNextPC = armNextPC` 存下来之后才执行
`armNextPC = reg[15].I`。所以"handler 没设 clockTicks 就用默认值"那个分支
（`clockTicks = 1 + codeTicksAccessSeq32(oldArmNextPC)`）等价于 `...Seq32(pc)`，`0x08020000`
那个 prefetch 补丁同理（测的也是当前指令）。

> 这一条 2026-11-02 曾被写成"上一条指令的地址"，**是错的**。块级差分跑真代码块时不放过任何
> 一个默认周期，加上复核循环才定案。`jitInterpInsn()` 用 `pc` 是对的，块入口**不需要**为它
> 做任何补偿。

`jitInterpInsn()` 进函数时先把 `armNextPC` 设成 `pc+4`/`pc+2`、`reg[15]` 设成 `pc+8`/`pc+4`，
再照抄 `armExecute()` / `thumbExecute()` 那三行 prefetch 记账，最后才分派。两个循环的掩码
不一样（ARM `0xFFFFFE00`、Thumb `0xFFFFFF00`），别合并。

---

## 十、踩过的坑（按主题，不按时间）

每一条都是"跑起来才发现"的，不是读代码推出来的。**移植到新后端时这些坑会以同样的形状出现**，
所以按主题归类，而不是按修复时间。

### 10.1 周期与时序

1. **STRH 的常数是 4，不是 2。** `arm-new.h` 所有 STRH case 都是
   `clockTicks += 4 + CPUUpdateTicksAccess16(address)`，而 STR/STRB 是 `2 + Access`。实现时按
   "统一 +2"抄错，块级差分立刻报 `cycles interp=25 block=23`。现在按 `MEM_HALF ? 4 : 2` 分开。
2. **Thumb MUL 的分档源，V7 和 V8 恰好互换。** V7 Thumb（`thumb.h` 的 `case 0x43/0x01`）用
   `u32 rm = reg[(opcode >> 3) & 7].I`——**Rs**；V8 Thumb（`thumb32_1` 一类）用
   `reg[dest].I`——**Rd 旧值**。被乘数都是 Rd 旧值，但**分档看谁**不同。差分用例里 `mul` 的
   周期差就是从这里来的。（原先文档写成"V7 用 Rd 旧值、ARM 用 Rs"，是反的。）
3. **ARM 寄存器移位的 raw 计数 ≥ 32 是未定义行为，必须回退。** `LOGICAL_LSL_REG` /
   `LOGICAL_LSR_REG` / `LOGICAL_ASR_REG` 直接 `v << shift`、`v >> (shift - 1)`，
   `shift = reg[Rs].B.B0`（0..255，没有掩码）。本机这版解释器给出的结果既不是"移位 32 得 0"，
   也不是"掩码到 5 位"，而是编译器/宿主做什么就是什么。所以这几类一律 `JIT_NEED_INTERP`
   （`shiftNeedsInterp()`）。真实代码不会用 ≥32 的计数，代价为零。Thumb 不受影响：`thumb40_2`
   等把 32 及以上的分支写清楚了。
4. **`ticks <= 0` 判错会吃掉合法周期。** 开着 WAITCNT 的 prefetch 位时 `GBA.cpp` 会把
   `memoryWaitFetch[8]--`（可以到 0），而 `thumbCycles[opcode >> 8]` 本身就有 0，所以一条 Thumb
   指令的合法周期可以是 **0**。块运行器原来把 `<= 0` 当成"IR 没实现"，每次补 `1 + wait`；
   12000 帧里累计 **279 036** 次，时钟整体漂移，IRQ/定时器随之错位——这是 HashTrace 第 11 帧
   就分叉的根因。**"IR 没实现"的判据必须是负返回值，`0` 是有意义的周期数。**
5. **Thumb 的 LDM/STM 没有 `+2` 基数。** `clockTicks += 2` 是 ARM 的 `LDM`/`STM` 共有的前导；
   `thumbC0`/`thumbC8` 只有逐寄存器的 `+1`/`+2`。
6. **V8 的 Thumb 高寄存器写 PC 是"调用一次再乘二"**：周期式子是
   `codeTicksAccessSeq16(armNextPC) * 2`。IR 里写成调用两次，`busPrefetchCount` 的副作用跟着
   算两遍，块内第二条指令最多多收 8 个周期（`aos.gba` 的 `08002B6E` 块，`interp=226 block=234`）。
7. **对齐用切换后的状态、周期用切换前的指令集**——见 5.2 第 4 条。这是端到端"IRQ 晚一条"的
   根因，逐指令差分测不出来。

### 10.2 PC 与 `reg[15]`

1. **`ldrb/ldrh/ldrsb/ldrsh pc` 不是一次跳转。** 只有**字** `LDR` 到 PC 会 `+2` 周期、把地址
   对齐到 4 字节并跳过去。字节/半字加载写 `reg[15]` 之后，**下一条指令仍然取自 `pc+4`**，只有
   再下一条才跟着新的 `reg[15]` 走。块表达不了这种"绕一条"。
2. **后索引的 `ldr pc, [...]` 也是"绕一条"，而且更隐蔽。** `arm-new.h` 里 `CASE_16(0x430)` /
   `0x4b0`（`ldr pc, [rn], #imm` 且 W=1 的形态，注释写作 LDRT）的函数体**没有** `dest == 15`
   那一段，于是 `reg[15]` 就停在加载值上——既不对齐、也不 +4；只有 W=0 的 `0x410`/`0x490` 才
   会跳。解码器不保留原始 W 位（后索引一律回写），所以现在**所有后索引的字加载到 PC** 都回退
   解释器。差分签名：`BLOCK MISMATCH 0800698C count=2 reg` / `r15 interp=02000350 block=02000354`。
3. **"写 PC 但不是跳转"是一个通用问题，不是几个特例。** 最初按 op 逐个枚举，结果 `stm r15!`、
   `mrs r15, cpsr` 这些漏了。正确做法是 3.2 第 3 条那条不变式：块内任意时刻
   `reg[15] == 下一条 + 4/2`，不成立就停在原地。差分块级 pass 里
   `r15 interp=07E8A2A0 block=080002A0` 那一类失败全是它的变体。
4. **`bx r15` 忘了设 `reg[15]`。** `compile()` 原来对 `JOP_BX` 跳过 `setPc()`，于是 `bx r15`
   用的是上一条指令留下的 `reg[15]`，跳回原处死循环——表现是**玩家输完名字后永久黑屏、全是
   lag 帧、音频还在响**。`bx r15`/`bx pc` 读的是 `pc+4`，必须先写对。
5. **V8 有同类的块级 bug（既有，不是 V7 引入）。** `-jitverify` 块级 pass 在 `mario.gba` 上
   稳定报 7 处，签名全是 `r15 interp=... block=...`，块里都有 `stm r15!, {...}` 或
   `str rX, [r15, -rY lsl #k]!`。修法：块停下、`reg[15]` 原样留着，`jitExecute()` 用
   `jitDetour` 数两个解释器回合才允许再从 `armNextPC` 解码。

### 10.3 解码与分派表

VBA 的两张分派表**不等于 ARM 架构手册**：表里有些位置是 `arm_UI`（未实现），有些位置模拟器
根本不看架构要求的那几位。**解码器必须照抄表，不能照抄手册。**

1. **立即数访存的 `m` 字段留着 0** ⇒ 所有 `ldr/str [#imm]` 的偏移都变成了 `r0`。用 `JF_IMM`
   解决（`evalOffset` 不再看 `m`）。
2. **单数据传送的 bit4 判反。** VBA 的表里 **bit4=0 才是立即移位**（`arm600/602/604/606` 对应
   LSL/LSR/ASR/ROR 立即数），bit4=1 是 `OFFSET_REG`。第一版把立即移位一律当成 ROR，
   `ldr r0,[r1,r2,lsl #2]` 会变成 RRX。
3. **寄存器移位的判定位写成 `opcode & 0x0F`**（那是 Rm），应该是 `(opcode >> 4) & 0x0F`。
4. **`rsbs r2, r0, #0x26` 变成未定义**：乘法/半字分支没排除 `I=1`（bit25），立即数的 bits 7-4
   被当成分派键。
5. **`ldr/str` 带 bit4=1 被执行**：表里这些位置是 `arm_UI`，模拟器根本不实现 ARM 手册的
   "寄存器移位"形式。
6. **`tst/cmp/cmn` 把 Rd=15 当成写 PC 并切模式**：模拟器的 ALU 宏对**所有** ALU 指令判
   `dest != 15`，但比较类不跑 `CPUSwitchMode`；两者要分开建模。
7. **`mrs r0, spsr` 之后 CPSR 不一致**：`arm140`（SPSR）不调 `CPUUpdateCPSR()`，只有
   `arm100`（CPSR）调。
8. **`bx lr` 变成未定义**：Thumb format 5 只按 bit7 判 BX/BLX，bits 6-3 是 Rm；多测了 bit6。
9. **`swp` 少识别一批**：ARM 手册要求 bits 11-8 = 0，但表里那些位置也指向 SWP，模拟器不看
   这几位。
10. **`swi` 吃掉协处理器行 `0xDx`/`0xEx`**：判 SWI 用了 bit24，应该判 bits 27-24 == 1111。
11. **半字的若干非法组合被执行**：`S=1 && L=0`（"有符号存储"）全表未定义；`H=1,S=0 && P=0`
    只在 U=I=1 的两行实现——照抄表的空洞。
12. **`msr` 的 SPSR 形式被当成写 CPSR 并错误地切了模式**：模拟器对 SPSR 形式既不
    `CPUUpdateCPSR` 也不 `CPUUpdateFlags`、更不切模式。
13. **`decodeMultiply()` 把 U 位取反了**：长乘法编码里 bit22 为 0 是 `UMULL`（`0x08xxxxxx`）、
    为 1 是 `SMULL`（`0x0Cxxxxxx`），和解释器 `arm089`/`arm0C9` 的分组一致；JIT 写成了
    `isUnsigned = (opcode & 0x400000) != 0`，于是无符号乘法被当成有符号算，高 32 位全错。
    `selftest` 里那条 `0xE0C10392 -> umull` 的用例也跟着记反了。
14. **`read32` 用 `READ32LE` 把 `const u8*` 转成 `u32*` 解引用**是 const-cast 别名违规，
    clang 在 `-O0` 下就把第二条指令读成了 0。改成 `memcpy` 后正常，编译器仍优化成一次 load。

**BKPT 那一类要整块拒绝**：模拟器表里写着 `armBreakpoint` 的字（真正的 BKPT、以及 index 0x7FF
那个空洞）会让 `armBreakpoint()` 把 PC 退回去、给调试器发信号、`clockTicks = -1`（停机）。
块后端没法建模。

### 10.4 构建与工程

1. **`vcxproj` 里 116 行写成 `%$(PreprocessorDefinitions)`，多了一个 `%`。** MSBuild 展开成
   `%<值>`，`JitEmit.cpp` 的 `USE_GBA_CORE_V7` 就变成 `%USE_GBA_CORE_V7`，`#ifdef` 判假，
   x64 后端编成空实现。现象是 `jit: 0 native instructions`——**哈希仍然逐帧一致，因为全部
   退回解释器**，Win32/ARM64 和 V8 配置都看不出来。纯机械替换，但没有它 x64 JIT 根本不会跑。
2. **V82XR 的目标文件目录混着两套编译参数**（`LNK2038: _ITERATOR_DEBUG_LEVEL`），所以
   "改一个文件再链一次"会失败。要重建就整份重编：`.cpp` 用带 `/TP` 的 rsp，`.c`
   （zlib / SFMT / memgzio）必须用 `/TC`，否则符号会被 C++ 修饰而挂不上。
3. **本机 MFC 只装了 arm64**：`atlmfc\lib` 下只有 `arm64`。所以 x64/Win32 的完整链接做不了
   （`MSB8041`），但 x64 的**单 TU 编译检查**可以做（`cl /c` 带 `USE_GBA_CORE_V7`）。解决方案
   配置名是 `V7Release`，`V72XR` 是**项目**配置名，写 `V72XR|x64` 会得到 `MSB4126`。
4. **`-jitverify` 的报告走 `printf`，GUI 进程没有 console。** 原来那套 `AllocConsole()` +
   `getchar()` 在本机必然失败（进程已有一个 console，`AllocConsole` 返回
   `ERROR_ACCESS_DENIED`），于是验证器一声不响地 `exit(0)`。现在没有有效 stdout 句柄时直接
   `freopen("jitverify.log")`。也别用 `cmd /c` 或 `Start-Process -RedirectStandardOutput` 去抓。
5. **属性页残留会污染 Release 构建**：`V82XR|x64` 上被加过 `<EnableASAN>true</EnableASAN>`，
   会插桩、慢一个数量级、依赖 ASAN 运行时。看到"Release 比 Debug 还慢"先查这个。
6. **跑之前确认没有别的 VBA 进程**：验证器在没装 BIOS 时会为 `SWI` 弹模态框
   （"Unsupported BIOS function ..."），被挡住的那次会把窗口留在屏幕上。

### 10.5 验证器自身

1. **块级 pass 只快照 RAM 不够。** 顺序是"先跑解释器、再跑块"，只还原 workRAM/IWRAM 时，块里
   对 palette/VRAM/OAM 的写会把结果留给第二趟，报出"一条 store 改了两个寄存器"这种假象
   （store 不可能改寄存器，这就是识破它的线索）。现在覆盖 workRAM + internalRAM + palette +
   VRAM + OAM + ioMem。
2. **`localizeBlockMismatch()` 定位到另一条指令**：原来用 `seedCpu(..., 0)` 而不是失败那次的
   `off`。现在直接复用 `compareBlockOnce()` 的前缀，seed、快照、"解释器跑出块"的判断全部一致。
3. **块级 pass 会把"跑了一半的块"和"跑完的解释器"比。** `runBlock()` 在 `holdState` / 事件
   边界 / 指令集切换处停下时，验证器必须看得出来。另外块里有 IO 写时，验证器自己的副作用
   （触发 DMA、改中断标志）会让第二趟跑出不同的周期数——所以加了"两次尝试必须互相一致"的
   可复现性过滤。
4. **`-jitverify` 的 N 要覆盖到 ROM 最远的代码。** `aos` 那条后索引 `ldr pc` 在第 6756 条，
   `N=3000` 一直没抓到。
5. **单条差分永远测不出"跨指令集返回"。** `seedCpu` 把 `reg[17]` 设成 `0x1F`（SPSR 没有 T
   位），所以 `SUBS pc, lr, #4` 那种"ARM 指令返回 Thumb"永远不发生。这类只能靠块级和端到端。
6. **影子对比只覆盖"整块编译且不写内存"的块**（见 6.6.3）。写内存的块无法回滚，所以
   STORE/STM/PUSH/POP/SWP 的 bug 只能靠块级差分和端到端。

### 10.6 后端结构

1. **条件码整段漏了。** `emitOne()` 只给分支发了条件判断，ALU/访存全部无条件执行。单条差分
   抓不到（它测的是 IR 解释器），块级差分抓得到——修好后 `mario` 的块级差分从 9 处降到 0。
2. **编译一个块 ~10 µs 是解码的 200 倍**，必须"机器码缓存（键=地址+指令哈希）+ 首次不编译
   （`JIT_COMPILE_AFTER`）"，否则 `aos` 1200 帧要 60 s。见 2.4。
3. **`NEED_INTERP` 桩的位置**见 6.2；**拆链的复杂度**见 6.3。

---

## 十一、实测存档

### 11.1 差分矩阵（2026-11-04 晚，x64 V72XR，`-jitverify 20000`）

| ROM | 单条 ARM | 单条 Thumb | 块级 ARM | 块级 Thumb |
| --- | --- | --- | --- | --- |
| aos | 20000 / 0 failed | 20000 / 0 failed | 20000（53 331 insn）/ 0 failed | 20000（151 584 insn）/ 0 failed |
| mario | 20000 / 0 failed | 20000 / 0 failed | 20000（43 583 insn）/ 0 failed | 20000（393 700 insn）/ 0 failed |
| sonic | 20000 / 0 failed | 20000 / 0 failed | 20000（48 055 insn）/ 0 failed | 20000（209 537 insn）/ 0 failed |

解码空间：ARM 4096 字 + Thumb 1024 字，**0 分歧**。`selftest`：V8 1101 项 / V7 1207 项，0 失败。
`VBA_JIT_SHADOW=1` × 三 ROM：无 mismatch。

"跳过"= 单条 pass 里不比较的 store/SWI/MSR，或块级 pass 里整块含 SWI/UNDEF/MSR、以及解释器侧
跑出块的"绕一条"；"拒绝"= 解码器整块不接。两者都是安全的回退，不是失败。

### 11.2 端到端（`-hashtrace`，6000 帧，`-jit` vs `-nojit`）

| ROM | 结果 | `ram` 列 SHA-256 |
| --- | --- | --- |
| aos | IDENTICAL | `C2C5394CC828E69A66B1CD35E65D6A573446E21C14D6FB92F9295EB395313A89` |
| mario | IDENTICAL | `7D7198DA3564458DDF2851CE7D03E2EBF9ABC2A5479E3ACF6A1F780AFDA33F65` |
| sonic | IDENTICAL | `8DC581838BC91E16905D1B878C6A85E1117B36DAA60BC6D26D83BB3C69A18A22` |

### 11.3 块统计

`jit.log`（sonic 6000 帧，一行原样，这里按宽度折开）：

```
jit: 6093242 blocks decoded, 29827265 runs, 23734023 cache hits, 8274455 page
invalidations, 8977394 IR instructions, 0 bad, 304727 native blocks, 304727
compiles, 5182101 code hits, 198719870 native instructions, 173407 chain links,
3365 ms compiling
```

**已知口径问题（未改）**：`JitStats.chainLinks` 只增不减，链式执行不计 `blocksRun`，所以 `runs`
会低估实际执行的块数。这是统计口径，不是执行错误。

### 11.4 一个历史基线（V8，IR 解释器时代，仅供对照）

机器码后端落地之前，"JIT" 就是 IR 解释器，比内联解释器**慢 1.1–1.5 倍**（V8 mario 1.48x、
V7 mario 1.16x）。V8 吃亏更多，因为它的块运行器每条指令还要维护 `cpuTotalTicks`、
`busPrefetchCount` 和预取队列，而 V7 这三样都不存在。

记这个数字的理由只有一个：**"IR 解释器比解释器慢"是正常的，不是 bug**。一层函数调用不可能
比内联的 switch 快；把它优化到 1.0x 附近也不叫加速。真正的加速只能来自机器码后端。
