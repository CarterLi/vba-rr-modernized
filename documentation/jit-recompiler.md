# 重编译器（JIT）与 AOT 导出：可行性、依赖与实施方案

## 结论

**技术上可行，但前提要先纠正三件事，否则会白干三个月。**

1. **"提升运行速度"在这个项目里不是一句话能兑现的。** 60 fps 正常游玩时，CPU 只占整帧
   时间的一小段，其余是软件渲染（`renderLine` × 160 行）、合成/缩放、声音混音和 GDI 提交。
   JIT 只能压缩 CPU 那一份。按阿姆达尔定律，CPU 占 25% 时，CPU 快 3 倍整帧只快 **1.14 倍**。
   JIT 真正的地盘是：**turbo/快进、低端机、Lua 重脚本、以及将来的 AOT**。
2. **有一个几乎免费的先手棋没做。** `armExecute`/`thumbExecute` 每条指令都无条件调
   `CallRegisteredLuaMemHook(..., CPUReadMemoryQuick(armNextPC), ...)`——即使一个脚本都没加载，
   这个**实参**也已经先算完了：一次 `memoryMap` 查表 + 一次 4 字节读。纯浪费。先修这个，
   再谈 JIT。
3. **"AOT 导出原生游戏"能做到，但产物不是"移植版游戏"。** 它是**静态重编译的模拟器**：
   ROM 里跑过的代码变成原生函数，没覆盖到的（RAM 里生成的代码、BIOS、动态分支）仍走解释器，
   画面/声音/DMA/时序仍由运行时负责。任何声称能"彻底脱离模拟器"的方案在 GBA 上都不成立。

**工期（按一个人写、业余节奏估）**：便宜优化 2-4 天；JIT 到能用 6-9 周；AOT 导出再 4-6 周。
全职压缩到 2.5-3 个月。

**依赖**：不需要、也不存在一个叫 `armjit` 的通用库。推荐 **asmjit**（Zlib，x86-64 + AArch64 一套
API）；不想引第三方就手写编码器，x64 那部分约 1500 行。

---

## 一、先量，再动手

### 1.1 解释器每指令的成本结构

读 `src/gba/V8/GBA-arm.cpp:2309`（`armExecute`）和 `GBA-thumb.cpp:1685`（`thumbExecute`），
每条 guest 指令固定要付这些钱：

| # | 动作 | 位置 | 说明 |
|---|---|---|---|
| 1 | `CPUMasterCodeCheck()` | arm:2313 / thumb:1689 | 金手指 mastercode 比较，函数调用 |
| 2 | `cpuPrefetch[0] = cpuPrefetch[1]` | 2319 | 全局数组，两条内存操作 |
| 3 | `busPrefetchCount` 掩码 | 2322 | 全局读改写 |
| 4 | **`CallRegisteredLuaMemHook(..., CPUReadMemoryQuick(pc), ...)`** | arm:2335 / thumb:1711 | **实参无条件求值**：查表 + 读内存，然后才在函数里发现没有钩子 |
| 5 | `armNextPC = reg[15].I; reg[15].I += 4;` | 2340 | 全局结构体读写 |
| 6 | `ARM_PREFETCH_NEXT` → `CPUReadMemoryQuick(armNextPC+4)` | 2342 | 又一次查表 + 读 |
| 7 | 条件码 switch | 2345 | 绝大多数是 AL，已用 `UNLIKELY` 短路 |
| 8 | **间接调用 `armInsnTable[...](opcode)`** | 2401 | 4096 项函数指针表；分支预测器基本无解，一次误预测 15-20 周期 |
| 9 | 处理函数内部：读写 `reg[]`（BSS 全局）、`N_FLAG..V_FLAG`（4 个 `bool8` 全局，各一次字节读写） | 全文 | 寄存器不在宿主寄存器里，标志位是一次次字节存储 |
| 10 | `clockTicks` 记账 + `codeTicksAccessSeq32` | 2406-2409 | 见 1.2 |

粗估 15-25 个宿主周期/guest 指令。其中第 4、8、9 项是 JIT 能直接吃掉的，合计大概占一半。

### 1.2 时序模型（这是全案最重要的约束）

```
CPULoop:
  cpuTotalTicks = 0;  cpuNextEvent = CPUUpdateTicks();
  loop:
    armExecute();                       // 内部死循环，直到预算用完
    ...
```

`armExecute` 的退出条件在 `GBA-arm.cpp:2411`：

```c
while (cpuTotalTicks < cpuNextEvent && armState && !holdState && !SWITicks);
```

而每条指令的周期是

```
clockTicks = 指令内部周期 + 1 + codeTicksAccessSeq32(该指令地址)
cpuTotalTicks += clockTicks;
```

`codeTicksAccessSeq32`（`GBACpu.h:238`）**不是常数**：它读 `busPrefetchCount`，还会改写它。
`dataTicksAccess*` 同理，而且依赖运行时的 `address >> 24`。

**结论：任何"整块算周期、块尾对账"的做法都会改变 IRQ 落在哪条指令上，从而改变
逐帧行为。** 对一个 TAS 项目，这等于废掉 `.vbm` 的可复现性。所以 JIT 必须
**逐指令记账、逐指令检查预算**——这听起来很亏，但其实只多 3 条宿主指令
（`sub/cmp/jle`），而省掉的是间接调用和全局内存往返，净赚仍然很大。

这一点直接决定了方案形态：**不做"超级块 + 周期摊还"**，只做"直译 + 逐指令预算检查"。
后面所有设计都建立在这条上。

### 1.3 测量方案（Phase 0 的交付物）

在 `CPULoop` 里加一个可选的分段计时（`#ifdef VBA_PROFILE_FRAME`），用 `__rdtsc()` 夹住：

```
[CPU] [renderLine] [CPUDrawPixLine] [soundTick] [CPUFrameBoundaryWork]
```

输出每 60 帧一行到 `vba-frame-prof.log`。要回答的只有一个问题：

> **CPU 占整帧多少？**

- `< 20%`：JIT 的整帧收益上限 20%，先别做 JIT，去做渲染。
- `20-40%`：JIT 值得做，整帧 1.1-1.25x，turbo 2-3x。
- `> 40%`：JIT 是正确方向。

这个数据必须自己测，不能拍脑袋——`documentation/xbrz-vector-port.md` 那次就是先测才发现
瓶颈在滤镜的 64 MB 表上，而不是渲染后端。

---

## 二、现状盘点（决定架构的既成事实）

| 事实 | 位置 | 对 JIT 的影响 |
|---|---|---|
| 解释器已经是"跑到下一个事件"的模型 | `CPULoop` | 天然适合块化：块边界 = 分支 + 预算耗尽 |
| 标志位是 4 个独立 `bool8` 全局，另有 `reg[16]` 里的 CPSR 副本 | `GBAGlobals.h:66-69` | 块内可以用一个宿主寄存器缓存，块尾回写；但 `CPUUpdateCPSR` 被到处调用，必须精确知道何时"物化" |
| `reg[15]` 在 ARM 执行期等于 **当前 PC+4**，指令读到的是 PC+8 | `GBA-arm.cpp:1458` | IR 里 PC 必须单独建模，不能当普通寄存器 |
| 内存走 `memoryMap[256]` 快表，另有 `CPUReadMemory` 等带 Lua 钩子的包装 | `GBAinline.h:38-66` | 第一版 JIT 直接调包装函数，第二版再内联 RAM/ROM 快路径 |
| 三种宿主平台：`Win32` / `x64` / `ARM64` | `VBA_Latest.vcxproj:4-50` | 后端至少要做 x64 + ARM64；Win32 可以永远留在解释器 |
| 工程只有 MSVC + MFC，无 CMake | `win32/VBA_Latest.vcxproj` | 新文件要手工加进 vcxproj；JIT 代码必须**不依赖 MFC**，否则将来 wx 前端用不了 |
| 已有 `HashTrace`：每帧把 `emuWriteMemState()` 的 40 万字节整体哈希 | `common/HashTrace.h` | **现成的差分测试神兵**，JIT 正确性的主要验收手段 |
| 存档格式 `SAVE_GAME_VERSION_14`，包含全部 CPU/内存状态 | `GBA.h:24-28` | JIT 状态是派生的，**绝不进存档**；loadstate 时清空块缓存 |
| `BKPT_SUPPORT` 在 Release 也开着 | vcxproj:275 | 调试器/断点激活时必须回退解释器 |
| 金手指 mastercode 靠"每条指令比较 PC"触发 | `GBA.cpp:1596` | 编译期判断 `mastercode != 0`，变化时清空缓存 |
| Lua 有 EXEC/READ/WRITE 三类内存钩子 | `vbalua.h:24-35` | EXEC 钩子覆盖的页不编译；READ/WRITE 走原函数即可 |

---

## 三、架构

### 3.1 分层

```
CPULoop  (不动)
   └─ jitExecute()                     ← 替换 armExecute/thumbExecute 的调用点
        ├─ 快查 (pc, thumb) → JitBlock*
        ├─ 未命中 → 解码器 → IR → 后端 → 写入可执行内存 → 入表
        ├─ 失效检查（页版本号）
        └─ 执行：块内直译指令 + 逐指令预算检查 → 返回
             └─ 辅助函数（JitRuntime）：内存访问、PC 写、标志物化、异常
```

`CPULoop` 里那 8 行（`GBA.cpp:3918-3931`）改成：

```c
if (!holdState && !SWITicks)
{
    if (!jitExecute())      // 内部自己看 armState，关掉 JIT 时直接转解释器
        return;
    clockTicks = 0;
}
```

> **2026-11-02 现状**：上图里的"后端"暂时就是 `JitInterp.cpp`（逐条解释 IR），机器码后端还没写。
> 所以现在开 JIT 是**变慢**的；先接进来买的是块缓存、页失效、块边界这套机制被真实跑通，
> 将来换后端不需要动 `JitCache.cpp`。实测（超级马里奥A，6 秒）：172567 块解码、5729772 次入块、
> 命中率 97.0%、1588 次页失效、0 条坏指令。
>
> **2026-11-03**：开关已接到 `Options > Emulation > CPU Recompiler (JIT)` 并写入
> `vba.ini` 的 `jit` 键；命令行 `-jit` / `-nojit` 覆盖本次运行。默认关闭。详见 3.8。

### 3.2 为什么必须有 IR（而不是解码后直接吐机器码）

三个理由，缺一不可：

1. **AOT 要复用**。AOT 导出需要同一套语义再生成一遍 C 或另一份机器码。没有 IR，就得把
   解码逻辑写两遍，两遍之间必然漂移。
2. **差分测试要一个"可读的第三实现"**。C 后端生成的代码能直接编译、断点、打印，
   是排查"JIT 和解释器差 1 个 flag"时唯一高效的手段。
3. **优化空间**。标志位惰性计算、常量折叠、死代码消除都只能做在 IR 上。

IR 保持**极简**：一个基本块 = 一个 `JitInsn` 数组，寄存器号就用 GBA 的 0..15。
下表的字段是实际落地的形态（细节以 `src/gba/jit/JitIr.h` 为准，**不要再在文档里重抄一遍枚举**，
两处必然漂移）：

```c
struct JitInsn {
    u32 imm;        // 立即数 / 移位量 / 分支偏移 / 寄存器列表 / SWI 号
    u16 flags;      // JF_*：WRITEBACK / PRE / UP / SIGNED / LINK / IMM / PC_ALIGN4 / CLEAR_PREFETCH
    u8  op;         // JitOp：一条 guest 指令 = 一个 JitInsn
    u8  sub;        // ALU 子操作 / MUL 子操作 / 访存宽度 / LDM-STM 模式
    u8  cond;       // ARM 条件码；Thumb 一律 0xE（条件分支除外）
    u8  d, n, m;    // Rd / Rn / Rm（访存里是 Rt / base / offset 寄存器）
    u8  shift;      // JitShift：移位种类（访存时按 OFFSET_* 语义解释）
    u8  flagMask;   // 本条写哪几个标志（N/Z/C/V 位掩码），0 = 不写
};
```

两个容易踩的设计点：

- **`flagMask` 不是布尔量。** ARM 的逻辑运算写 N/Z/C、不动 V；Thumb 的大多数 ALU 只写 N/Z；
  `mov r0, r1`（Thumb 高寄存器形态）一个都不写。用「置标志」一个 bit 表达不了，这正是第一版
  最需要改的地方。
- **`JF_IMM` 决定第二操作数从哪来。** 没有这个标记时，立即数形态的 `m` 留着 0，解释器会当成
  `r0`——`ldr r0, [r1, #4]` 的偏移会被算成 `r0`，这个 bug 在差分测试跑起来之前一直藏着。

**块边界规则**（写死在解码器里，简单优先）：

- 遇到分支 / 写 r15 / `BX` / `SWI` / 未定义指令 → 本块结束（这些指令本身留在块里）。
- 跨 1 KB 页边界 → 结束（为了失效检查能只看一个页版本号）。
- 达到 `JIT_MAX_INSNS`（建议 64）→ 结束。
- 不因为内存访问结束块（IO 访问走 helper，继续往下）。

### 3.2.1 V7 与 V8 的 IR / native 后端复用

**结论：IR 的数据结构和块机制可以共用，但 IR 的"语义解释"不能默认共用；native code
generator 也能共用一套，但必须是 core-aware 的，不能假设 V7/V8 生成完全相同的指令序列。**

V7 和 V8 都是 ARMv4T，`JitInsn` 里的东西——16 个 guest 寄存器、ALU 子操作、移位种类、
访存宽度、LDM/STM 模式、条件码、`flagMask`——**作为"指令形状"是通用的**。这部分共用没
问题，也是"语义只写一次"的价值所在。但当前 `JitIr.h` 的语义是**按 V8 解释器定义的**，
V7 至少有以下差异不能忽略：

| 差异 | V8 | V7 |
|---|---|---|
| 周期 | `1 + codeTicksAccess*`，还有 `busPrefetch` / `busPrefetchCount` | `memoryWaitFetch*` + `thumbCycles[opcode >> 8]` + handler 自己的 `clockTicks +=`，没有 busPrefetch |
| 预取 | `cpuPrefetch[0..1]`，块出口必须重填 | 没有预取缓冲，块出口不用管 |
| ROR by register | 计数为 0 时不改 C（`SHIFT_ROR_REG`） | 原始计数为 0 **也**写 C（比 `SHIFT_ROR_REG_T` 更激进；需要新移位模式，见 3.2.1.6） |
| PC 写入 + 比较类 | `TST/CMP...` 的 `C_CHECK_PC` 为空，不切模式 | `dest == 15 && S` 会 `CPUSwitchMode()`，比较类也走 |
| LDM/STM | Thumb 的 `C0/C8/BC` 直接赋值 `clockTicks`，丢掉逐寄存器周期 | 逐寄存器累加，周期公式不同 |
| 未定义 / 空洞 | V8 表里有若干 halfword 空洞 | V7 实现了更多组合 |
| Thumb 基周期 | 由 `codeTicksAccessSeq16` 决定 | 由 `thumbCycles[opcode >> 8]` 查表，当前 IR 里**没有这个信息** |

因此：

- **IR schema 可以共用**，但 `JitInsn` 需要补一个字段，例如 `u8 fetchCycles`（Thumb 的
  `thumbCycles[opcode >> 8]`），或者直接存 `u16 rawOpcode`。否则 V7 的 Thumb 周期无法从
  现有 IR 还原。**已确认（2026-11-03）：选 `u8 fetchCycles`**，见 3.2.1.7。
- **IR 的语义必须按核实现**。保留一个"IR 解释器 / 规范层"，V7 和 V8 各一份（或同一份用
  `USE_GBA_CORE_V7` 分支），native 后端只消费"已经确定语义"的 IR。

native code generator 的分层：

```
JitIr.h                  共用（新增 fetchCycles / JF_QUICK / SHIFT_ROR_REG_V7）
JitDecode*.cpp           共用源码，按 USE_GBA_CORE_V7 处理表差异
JitInterp*.cpp           V7 / V8 各一份语义 + 周期规范
JitRuntime.h/.cpp        按核提供：内存访问、周期、PC 写、标志物化
JitX64.cpp / JitA64.cpp  共用 host 后端
JitCache.cpp             共用块缓存 / 失效，少量按核 glue
```

x64 / ARM64 的**编码器、标签、重定位、可执行内存、寄存器缓存、块 prologue/epilogue**
与 guest 核心无关，这部分应该共用，也只应该写一套。真正和 V7/V8 相关的是 **lowering 和
runtime**。native 后端里不要直接写死 `1 + codeTicksAccessSeq32(...)` 这种 V8 公式，而是
调用一个 **core descriptor / runtime 接口**：

```c
struct JitCoreOps {
    int  (*aluCycles)(...);
    int  (*memCycles)(...);
    void (*writePc)(...);
    void (*materializeFlags)(...);
    ...
};
```

更简单的做法：V7/V8 本来就是**不同的编译配置**（`USE_GBA_CORE_V7` 是编译期宏），直接让
同一份 `JitX64.cpp` 在 V7 构建里发射 V7 的周期序列、在 V8 构建里发射 V8 的序列即可。这样
是"**一份 native 后端源码 + 两套周期/语义 lowering**"，不需要运行时判断 core，也不会让
V7/V8 同时存在于一个 exe 里。将来若真要在同一进程里双核，再给每个 `JitBlock` 加 core
tag，并在 helper 入口分派；不要现在就为这个场景付运行时开销。

**已确认（2026-11-03）：采用编译期 `USE_GBA_CORE_V7` 分支，不引入运行时
`JitCoreOps` 虚表/函数指针。** 依据是仓库现有的构建结构，不是偏好：

- `src/gba/GBA.cpp` 在文件第 4 行就用 `#ifdef USE_GBA_CORE_V7` 决定 include
  `V7/GBA.cpp` 还是 `V8/GBA.cpp`（`src/gba/GBA.cpp:4-8`）。两个核心的
  `GBA.cpp` 定义**同名全局**（`reg`、`armState`、`memoryWait*`、`thumbCycles` 等），
  根本不可能同时链接进一个 exe。
- `src/gba/jit/JitCache.cpp` 已经用 `#ifndef USE_GBA_CORE_V7` 把整个 V8 块缓存
  包起来（`:19-240`），`jitAvailable()` 在 V7 下直接返回 `false`（`:246-253`），
  `jitInterpInsn()` 在 V7 下是返回 `-1` 的桩（`JitInterp.cpp:35-43`）。
- `win32/VBA_Latest.vcxproj` 里 V72XD/V72XR 配置定义 `USE_GB_CORE_V7;
  USE_GBA_CORE_V7`，其余配置用 `UndefinePreprocessorDefinitions` 显式去掉
  （例如 `:286`、`:798`）。这是“一配置一核心”的构建，不是运行时选择。

因此：

- **正确性**：编译期分支和运行时分支在单核构建里语义完全相同，但编译期没有
  “core tag 忘设置 / helper 指针为空 / 间接调用无法内联”的风险。
- **性能**：V7 的周期记账是热路径，函数指针会阻止内联，也会让编译器无法把
  `memoryWaitFetch` 的查表优化进块。编译期分支零成本。
- **可维护性**：不需要维护两个实现都满足的 ABI；V7 和 V8 的实现可以各自用
  最自然的签名。代价是共享代码里会出现 `#ifdef`，但这和仓库现有的
  `GBA.cpp` / `GBAMemory.cpp` / `JitCache.cpp` 风格一致。

**接口组织建议**：虽然不引入运行时 `JitCoreOps`，仍建议把按核的 helper 收进一个
编译期命名空间（例如 `namespace jitcore { int memCycles32(u32); ... }`），在
`JitRuntimeV7.cpp` / `JitRuntimeV8.cpp`（或同一文件的 `#ifdef` 分支）里各写一份。
调用点看起来和 `JitCoreOps` 一样，但由编译器静态解析。这样将来若真需要双核，
只需把这些函数改成函数指针表，调用点不用重写。

**另一个必须注意的点**：`JitInsn` 的**数据布局**必须两核一致，因为 `JitDump.cpp`、
`selftest.cpp`、`JitVerify.cpp` 是共享的。新增 `fetchCycles` / V7 flag 时，
V8 构建也要保留这些字段（填 0），不要让 `sizeof(JitInsn)` 随核心变化。

明确不做的三件事：

- **不要**让 native 后端直接复用 V8 的周期公式去跑 V7。周期差一条指令，`.vbm` 就会漂，
  这是第三节的红线。
- **不要**为了共用而把 V7 的 `thumbCycles` 硬编码成 V8 的 `codeTicksAccessSeq16`。两者不是
  同一个模型。
- **不要**写两套 x64/ARM64 发射器。差异应该收敛到编译期 `USE_GBA_CORE_V7` 分支
  （见上），而不是复制一千多行编码器。

一句话：**IR 的结构通用；IR 的语义和周期必须按核区分。native 二进制 code generator 可以
共用一套，但要把"host ISA 发射"和"guest core 语义/周期"拆开：前者共用，后者按 V7/V8
参数化。** AOT-C 后端同理，也能同时服务两个核心，只是生成出来的 C 里周期记账不同。

> **当前状态（2026-11-03 夜）**：V7 的 JIT **已落地并通过差分验证**——`jitAvailable()` 在
> V7 构建里返回 `true`，`jitInterpInsn()` 的 V7 分支、`jitExecute()` 的 V7 版本、
> `JitVerify.cpp` 的 V7 双趟差分都在（提交 `484443c` / `fc879eb`）。真实 ROM 上
> `-jitverify 20000` 的逐指令与块级比对 0 差异，`-hashtrace` 12000 帧逐帧一致；
> `selftest` 双模式 V8 1101 项 / V7 1207 项，0 失败。性能基线见 3.2.1.16
> （**JIT 目前比解释器慢 1.1–1.5 倍**，因为后端还是 IR 解释器）。
>
> 下面 3.2.1.1–3.2.1.12 是这一轮对 V7 源码逐段核对后得到的**设计记录**，写于实现之前；
> 其中被实测推翻的说法在 3.2.1.14 里逐条更正，以 3.2.1.14 为准。实施顺序见 3.2.1.11
> 第 9 步之后，验收手段见 3.2.1.10。3.2.1.12 的 13 个待确认项已在 2026-11-03 全部确认。
> **进度快照、已验证矩阵和下一步的拆解见 3.2.1.13。**

#### 3.2.1.1 V7 的取指基周期：与 V8 完全不同的模型

V7 的每条指令在进入 handler 之前，先由解释器循环给 `clockTicks` 赋一个**基周期**，
handler 再在这个值上做加法（少数 Thumb 分支是直接赋值）。这个基周期就是 IR 必须还原的
第一项。

| 指令集 | V7 基周期 | 出处 |
|---|---|---|
| ARM | `clockTicks = memoryWaitFetch32[(armNextPC >> 24) & 15]` | `src/gba/V7/arm-new.h:1413` |
| Thumb | `clockTicks = thumbCycles[opcode >> 8] + memoryWaitFetch[(armNextPC >> 24) & 15]` | `src/gba/V7/thumb.h:184` |

要点：

- 这两行都在 `armNextPC = reg[15].I` **之前**执行，所以取的是**当前指令地址**所在的区域，
  不是下一条指令。
- `memoryWaitFetch` / `memoryWaitFetch32` 是**运行期数组**，会被 `CPUReset()`、读档和
  `WAITCNT` 写入改写（`src/gba/V7/GBA.cpp:147-149`、`:3021-3040`、`:3153-3154`）。
  因此基周期**不能**在解码时固化成常数，必须在执行时按 `pc >> 24` 查表。
- `thumbCycles[]` 是 256 项常量表（`src/gba/V7/GBA.cpp:113-131`），值域目前是 1..4。
  Thumb 指令的基周期**只由 opcode 高 8 位决定**，与地址无关；地址只影响后面的
  `memoryWaitFetch`。
- V8 的基周期是 `1 + codeTicksAccessSeq16/32(...)`，还会改 `busPrefetchCount`。V7
  **没有** `busPrefetch`、`busPrefetchCount`、`cpuPrefetch`、`cpuTotalTicks`，也没有
  `SWITicks` / `IRQTicks` / `mastercode`。这些 V8 概念一个都不能带进 V7 的周期公式。

**结论**：IR 必须为 Thumb 保存 `thumbCycles[opcode >> 8]`（建议字段名 `fetchCycles`，
`u8` 足够），或者保存原始 `u16 rawOpcode` 让 V7 语义层自己查表。ARM 不需要这个字段，
基周期只依赖运行期 `memoryWaitFetch32`。两种做法的取舍见 3.2.1.7。

#### 3.2.1.2 V7 的访存周期 helper

V7 的数据访问周期由四个 inline 函数给出（`src/gba/V7/GBACpu.h:43-61`）：

| helper | 实际查表 | 用途 |
|---|---|---|
| `CPUUpdateTicksAccess32(addr)` | `memoryWait32[(addr >> 24) & 15]` | 首次 32 位数据访问 |
| `CPUUpdateTicksAccess16(addr)` | `memoryWait[(addr >> 24) & 15]` | 首次 16/8 位数据访问 |
| `CPUUpdateTicksAccessSeq32(addr)` | `memoryWaitSeq32[(addr >> 24) & 15]` | 连续 32 位访问 |
| `CPUUpdateTicksAccessSeq16(addr)` | `memoryWaitSeq[(addr >> 24) & 15]` | 连续 16/8 位访问 |

它们**只查表，没有副作用**，不会像 V8 的 `codeTicksAccess*` 那样改 `busPrefetch`。
所以 V7 的 IR 解释器可以直接调用这四个函数，不需要复刻 V8 的 `busPrefetch` 前导。

#### 3.2.1.3 V7 ARM 逐类指令的周期与语义

下面所有“增量”都是**在 3.2.1.1 的基周期之上**再加的值。出处均为
`src/gba/V7/arm-new.h`，行号是 2026-11-03 的源码位置。

| 指令类 | 周期增量 | 语义要点 |
|---|---|---|
| 普通 ALU（Rd != 15） | `+ (regShift ? 1 : 0)` | 寄存器移位形式在宏开头 `clockTicks++`；立即数/立即移位不加 |
| ALU 且 Rd == 15 | `+ (regShift ? 1 : 0) + (S ? 1 : 0)` | 写 PC 后按新状态把 `reg[15]` 对齐到 4/2 字节并设 `armNextPC`；**没有**额外的取指 access 周期 |
| ALU 且 Rd == 15 且 S == 1 | 同上，另加模式切换 | `CPUSwitchMode(reg[17].I & 0x1f, false)` 对 **TST/TEQ/CMP/CMN 也执行**，这是 V7 与 V8 的硬差异 |
| MUL / MULS | `+2..+5` | 按 `rs = reg[(opcode >> 8) & 0xF]` 的绝对值大小分档；负数先取反 |
| MLA / MLAS | `+3..+6` | 同上，累加形式多 1 |
| UMULL / UMLAL / SMULL / SMLAL | `+2..+5`（长乘）/ `+3..+6`（长乘累加） | 仍按 Rs 分档 |
| STR / STRB | `+2 + Access32/16` | 字用 `Access32`，字节用 `Access16` |
| LDR / LDRB | `+3 + Access32/16` | 字用 `Access32`，字节用 `Access16` |
| LDR 且 Rd == 15 | 上面的 `+3 + Access` 再 `+2` | 然后把 `reg[15]` 对齐到 4 字节、设 `armNextPC`、`reg[15] += 4`；V7 没有预取缓冲，不需要重填 |
| STRH | `+4 + Access16` | 半字写 |
| LDRH / LDRSB / LDRSH | `+3 + Access16` | 半字/有符号读；**Rd == 15 也不重取 PC**（与字 LDR 不同） |
| 半字/有符号的 P/U/I/W 组合 | 同上（周期只看 L 和宽度） | **V7 实现了 LDRH/STRH/LDRSB/LDRSH 的全部 16 种 P/U/I/W 组合**，包括 V8 表里缺失的 `P=0,W=1` 六种（0x02B/0x03B/0x06B/0x07B/0x0AB/0x0BB）。只有 `S=1 && L=0`（有符号存储 / STRD）是未定义。详见 3.2.1.7 |
| LDM / STM | `+2`，然后每个寄存器 `+1 + Access32`（第一个）/ `+1 + Seq32`（后续） | LDM 的 PC 槽 `+2 + Access`；STM 的 PC 槽 `+1 + Access` |
| B / BL / BX / SWI | `+3` | BX 根据目标 bit0 切 ARM/Thumb；SWI 进 BIOS |
| SWP / SWPB | `+0`（只有基周期） | 内部仍调用 `CPUReadMemory` / `CPUWriteMemory`，Lua 钩子照常 |
| MRS / MSR | `+0`（只有基周期） | MSR 会 `CPUSwitchMode` / `CPUUpdateFlags`，见 3.2.1.5 |
| 单次传输 bit4 == 1 | — | 只在**寄存器偏移**形式（bit25=1，分派键 0x600–0x7FF）里是未定义：V7 的 switch 表把这段的 128 个偶数键全部实现，奇数键（bit4=1）落到 `default: CPUUndefinedException()`。**立即数**形式（bit25=0，键 0x400–0x5FF）由 `CASE_16` 覆盖全部 256 个键，bit4 是 `imm12` 的一位，不是“寄存器移位”标志。V8 解码器的 `if (!immF && (opcode & 0x10)) return false;` 与 V7 完全一致。此前“V7 需要支持寄存器移位传输”的猜测**已证伪**；此前“bit25=0 只列 bit4=0”的说法**也已修正**。 |

补充：

- ARM 的 `reg[15]` 在执行期等于**当前指令地址 + 8**，`armNextPC` 等于当前地址 + 4。
  这是 V7 的约定；IR 不能把 `reg[15]` 当普通寄存器随便读写。
- ARM 的 PC 写入（ALU/LDR/LDM）没有 V8 的 `ARM_PREFETCH` 重填；块出口只需要设
  `armNextPC` 和 `reg[15]`。
- 比较类指令（TST/TEQ/CMP/CMN）在 `Rd == 15` 且 `S == 1` 时，V7 会先更新标志，再
  `CPUSwitchMode`，后者又从 SPSR 重新装标志。净效果是“模式切换后的标志”，但周期上
  多 1，且 banked 寄存器会切换。JIT 必须按 V7 走，不能套 V8 的“比较类不切模式”。

#### 3.2.1.4 V7 Thumb 逐类指令的周期与语义

Thumb 的基周期已经在 3.2.1.1 给出；下表是 handler 额外的增量。**大多数 Thumb
handler 不加任何周期**，这是 V7 与 V8 的另一个大差异。

| 指令类 | 周期增量 | 语义要点 |
|---|---|---|
| 格式 4 的寄存器移位（LSL/LSR/ASR/ROR Rd, Rs） | `+1` | `src/gba/V7/thumb.h:534/562/599/640` |
| MUL | `+1..+4` | 按 `reg[(opcode >> 3) & 7]` 的大小分档（注意：V7 Thumb 用**目标寄存器旧值**，ARM 用 Rs） |
| 条件分支 | 成立时 `clockTicks = 3`（**赋值**，不是加）；不成立保持基周期 | 这是 V7 的硬语义，不能用“基周期 + 3” |
| 无条件 B / BL / BX / SWI | `+0` | 只有基周期 |
| PUSH | 每个寄存器 `+1 + Access32`（第一个）/ `+1 + Seq32`（后续） | SP 在最后更新 |
| POP | 每个寄存器 `+2 + Access32`（第一个）/ `+2 + Seq32`（后续） | POP 到 PC 时 PC 槽只加 `+Access`（**没有 +2**），然后 `armNextPC = PC`、`reg[15] = PC + 2` |
| LDM | 每个寄存器 `+2 + Access32`（第一个）/ `+2 + Seq32`（后续） | 若 base 在寄存器列表里，最后不回写 base |
| STM | 每个寄存器 `+1 + Access32`（第一个）/ `+1 + Seq32`（后续） | 第一个寄存器时回写 base |
| PC 相对 LDR | `+Access32` | 用 `CPUReadMemoryQuick((reg[15] & ~3) + imm)`，不经过 Lua READ 钩子；不清预取（V7 没有预取） |
| 寄存器偏移 LDR/STR | 字 `+Access32`；半字/字节/有符号 `+Access16` | 偏移是**原始寄存器值**，不缩放 |
| SP 相对 / 立即偏移 LDR/STR | 字 `+Access32`；半字/字节 `+Access16` | 与 ARM 的单次传输公式不同，注意别混用 |
| 高寄存器 MOV/ADD/CMP | `+0` | 只有基周期 |
| 其他 ALU | `+0` | 只有基周期 |

补充：

- Thumb 的 `reg[15]` 在执行期等于**当前指令地址 + 4**，`armNextPC` 等于当前地址 + 2。
- Thumb 的 `POP {..., PC}` **不切换 ARM 状态**：V7 把读出的值 `& 0xFFFFFFFE` 后直接
  当 Thumb 地址用。JIT 必须照抄，不能按 ARMv4T 架构手册“bit0 决定状态”去实现。
- Thumb 的 `LDM/STM` 与 V8 的“直接赋值 `clockTicks`”不同：V7 是逐寄存器累加，且
  base 回写规则也不同。V8 的 `JitInterp.cpp` 里那段 Thumb 块传输语义不能直接复用。

#### 3.2.1.5 PC 写入、模式切换与 MSR 的边角

V7 的 PC/模式行为有三处必须单独建模：

1. **ALU 写 PC 且 S=1**：V7 对**所有** ALU 子操作（包括 TST/TEQ/CMP/CMN）执行
   `CPUSwitchMode(reg[17].I & 0x1f, false)`，并 `clockTicks++`。V8 的
   `C_CHECK_PC` 对比较类为空，不切模式。V7 的 IR 解释器必须按 V7 走。
2. **ALU 写 PC 且 S=0**：不切模式，但仍会把 `reg[15]` 对齐并设 `armNextPC`；也就是
   “比较类指令只要 Rd 字段是 15，也会跳 PC”。V7 的宏在 `dest == 15` 分支里先执行
   `OPCODE2`（TST/CMP 等），然后无条件对齐 PC。
3. **MSR 改 CPSR 的 T 位**：V7 的 `CPUSwitchMode()` 只改 `armState` / banked 寄存器，
   **不调整 `reg[15]`**。如果一条 ARM 指令用 MSR 切到 Thumb，下一条 Thumb 指令取指时
   `reg[15]` 仍是 ARM 约定的“当前 + 8”，比 Thumb 约定多 2，导致 `armNextPC` 跳过
   一条 Thumb 指令。这是 V7 解释器的真实行为，不是笔误。

**已确认（2026-11-03）**：JIT 解码器必须把**所有写 CPSR 的 MSR**（寄存器形式和
立即数形式，包括只写标志域的 `MSR CPSR_f, ...`）整块拒绝，回退解释器。理由不是
“控制域才会切模式”，而是 V7 的实现路径：

- V7 的 MSR CPSR 无论写哪些域，最后都会调用
  `CPUSwitchMode(newValue & 0x1f, false)`（`arm-new.h:2423`、`:2539`）。
- 这个两参数重载会转到 `CPUSwitchMode(mode, saveState, true)`
  （`GBA.cpp:1849-1852`），也就是 `breakLoop = true`。
- `CPUSwitchMode()` 内部调用 `CPUUpdateFlags(breakLoop)`（`GBA.cpp:1845`）；
  `CPUUpdateFlags(true)` 在 `armIrqEnable && (IF & IE) && (IME & 1)` 时会执行
  `CPU_BREAK_LOOP_2`（`GBA.cpp:1701-1707`），而该宏会改写 CPULoop 的局部变量
  `extCpuNextEvent` / `extTicks` / `cpuSavedTicks`（`GBA.cpp:469-472`）。
- 即使 MSR 只写标志域、模式位没变，`CPUSwitchMode()` 仍会按当前模式做一次
  save/restore banked 寄存器，并对不支持的 `armMode` 调 `systemMessage()`；
  这不是一个“只写 4 个 bool 标志”的纯操作。
- 随后还有一次 `CPUUpdateFlags()`（无参版本，`breakLoop = true`），同样可能触发
  `CPU_BREAK_LOOP_2`。

因此“只写标志域的 MSR 可以安全 JIT”这个假设**不成立**。第一版直接把 MSR CPSR 当
块边界，交给 CPULoop 的内联解释器执行，能同时避开 banked 寄存器、T 位怪癖和
`ext*` 副作用。

**可以保留在 JIT 里的 MSR 变体**（已核对，无 `CPUSwitchMode` / `CPUUpdateFlags`）：

- `MSR SPSR_fields, Rm`（`arm-new.h:2465-2480`）：只在
  `armMode > 0x10 && armMode < 0x1f` 时按字段写 `reg[17].I`，否则完全不写。
- `MSR SPSR_fields, #imm`（`arm-new.h:2561-2580`）：同样的条件写 `reg[17].I`。
- `MRS`：CPSR 形式先 `CPUUpdateCPSR()` 再读 `reg[16].I`；SPSR 形式读 `reg[17].I`。
  两者都只影响目标寄存器，没有 `ext*` 副作用。

是否值得为 MSR SPSR / MRS 单独做 IR op，而不是第一版也一并回退，**待性能测量后决定**；
正确性上它们可以 JIT。

#### 3.2.1.6 ROR by register：V7 ARM 需要一个新的移位模式

V7 ARM 的 `ROR Rd, Rm, Rs` 在 `src/gba/V7/arm-new.h:616-636`：

- `raw = reg[Rs].B.B0`；
- 若 `raw == 0`：值不变，但 `C_OUT = (value & 0x80000000) != 0`；
- 若 `raw != 0` 且 `raw & 0x1F == 0`：值不变，`C_OUT` 同样取结果符号位；
- 否则正常循环右移，`C_OUT` 取结果符号位。

V8 的 `SHIFT_ROR_REG_T`（`JitInterp.cpp:228-244`）在 `raw == 0` 时**不写 C**；V7
在 `raw == 0` 时**也写 C**。所以 V7 不能直接用 `SHIFT_ROR_REG_T`。

**已确认（2026-11-03）：新增独立枚举 `SHIFT_ROR_REG_V7`，不用 flag。** 理由：

- 语义与 `SHIFT_ROR_REG_T` 只差 `raw == 0` 一个分支；写成独立枚举后
  `evalOperand2()` 里就是一个完整的 `case`，不需要在热路径里再查 flag。
- **追加在 `JitShift` 末尾**（`SHIFT_IMM_ROT` 之后），不改动现有枚举值，
  避免任何按数值序列化/缓存的假设失效。
- `isRegShift()`（`JitInterp.cpp:76-81`）要把它加进去，ARM 的 `+1` 周期
  才和解释器一致（V7 的 ROR 分支自己 `clockTicks++`，`arm-new.h:619`）。
- V8 构建里这个枚举值永远不会被解码器产生，`case` 留着不影响 V8。
- IR 解释器实现就是 `SHIFT_ROR_REG_T` 的 `case` 去掉 `raw == 0` 的提前返回：
  `n = raw & 0x1F`，`n == 0` 时 `value` 不变、`C = 结果符号位`，否则正常旋转、
  `C = 结果符号位`。

Thumb 的 `ROR Rd, Rs` 走的是另一段宏（`thumb.h` 的 `ROR_RD_RS`）。**已确认
（2026-11-03）：V7 Thumb 的 `ROR_RD_RS` 与 V8 的 `SHIFT_ROR_REG_T` 完全一致**，
可以原样复用，不需要新枚举。

逐条对照（`thumb.h:155-160` 的宏 + `thumb.h:621-643` 的 handler）：

| 情况 | V7 Thumb 行为 | `SHIFT_ROR_REG_T` 行为 |
|---|---|---|
| `raw = reg[Rs].B.B0 == 0` | 不进 `if (value)`，值不变、**C 不变**；仍 `clockTicks++`、更新 N/Z | `raw == 0` → 直接返回 `v`，不写 `carryOut`；N/Z 由 `doAlu` 统一更新 |
| `raw != 0` 且 `raw & 0x1F == 0` | `value` 不变，`C = reg[dest].I` 的符号位 | `n == 0` → `*carryOut = (v >> 31)`，返回 `v` |
| `raw & 0x1F != 0` | `ROR_RD_RS`：`C = (dest >> (n-1)) & 1`，结果循环右移 n 位 | 循环右移 n 位，`*carryOut = 结果符号位` |

两个细节也一致：

- 计数来源都是 `reg[Rs].B.B0`（**只取低 8 位**），不是整个 `reg[Rs].I`。
- V7 的 `C = (dest >> (n-1)) & 1` 与“结果 bit31”等价：右循环 n 位后 bit31 就是
  原值的 bit `(n-1) mod 32`，所以 `SHIFT_ROR_REG_T` 用结果符号位写 C 是对的。

**结论**：`SHIFT_ROR_REG_T` 保持现状即可；只有 **ARM** 的 ROR by register 需要
`SHIFT_ROR_REG_V7`（raw=0 也写 C）。V7 Thumb 解码器继续用 `SHIFT_ROR_REG_T`，
V7 的 Thumb 周期由 3.2.1.1 的基周期 + 格式 4 的 `+1` 给出，与这个移位模式无关。

#### 3.2.1.7 解码器差异与 IR 字段

V7 与 V8 都是 ARMv4T，`JitInsn` 的“指令形状”字段（`op/sub/cond/d/n/m/shift/imm/
flagMask`）基本通用。需要按核处理的是下面这些：

| 项目 | V8 现状 | V7 要求 | 处理方式 |
|---|---|---|---|
| Thumb 基周期 | 由 `codeTicksAccessSeq16` 决定 | `thumbCycles[opcode >> 8]` | `JitInsn` 增加 `u8 fetchCycles`，解码时填入；执行时再加 `memoryWaitFetch` |
| ROR by register | `SHIFT_ROR_REG` / `SHIFT_ROR_REG_T` | raw=0 也写 C | 新增 `SHIFT_ROR_REG_V7`（独立枚举，追加在末尾；不用 flag） |
| 半字传输空洞 | P=0,W=1 且非 U=1,I=1 时拒绝（仅 LDRH/STRH） | V7 实现了这六种组合（0x02B/0x03B/0x06B/0x07B/0x0AB/0x0BB） | 解码器在 `USE_GBA_CORE_V7` 下**删除** `half && !sign && !pre && wb && !(up && immF)` 这一条；`sign && !load` 仍保留 |
| `^` 块传输 | 整块拒绝 | V7 用 `CPUSwitchMode` 实现 banked 寄存器 | V7 也整块拒绝，回退解释器 |
| 单次传输 bit4=1 | 拒绝（当未定义） | V7 在**寄存器偏移**形式下也是未定义；立即数形式的 bit4 属于 `imm12` | 保持不变：`!immF && bit4` 拒绝。已用分派键全扫确认（见 3.2.1.3 与待确认第 10 条） |
| 控制域 MSR | 拒绝 | V7 会切模式/改 T 位 | 拒绝，回退解释器 |
| Thumb 解码表 | 已有 | 形状基本兼容，差异在周期/语义 | 共用解码器，V7 分支只改周期字段和少数语义 flag |
| `JitSource` | `memoryMap[256]` | V7 同样是 `MemoryMap[256]`，字段 `address/mask` 相同 | 直接共用 |
| 块边界 | 1 KB 页、最多 64 条、分支/写 PC/BX/SWI/未定义结束 | 同上；另需在指令集切换、事件、`holdState` 时停 | 共用 `JitCache` 规则 |
| 块出口预取 | 必须重填 `cpuPrefetch[0..1]` | 没有预取缓冲 | V7 块出口只设 `armNextPC` 和 `reg[15]`，**不要**调用 `ARM_PREFETCH`/`THUMB_PREFETCH` |
| Quick 读取 | 只有 `JF_CLEAR_PREFETCH`，实际仍走带钩子的 `CPUReadMemory` | Thumb `LDR [PC,#]` 与 `LDR [SP,#]` 必须用 `CPUReadMemoryQuick` | 新增 `JF_QUICK`（`1 << 10`），只给这两类加载置位；其余走钩子包装。见 3.2.1.8 |

`JitInsn` 当前布局（`src/gba/jit/JitIr.h:139-151`）是：

```c
u32 imm; u16 flags; u8 op, sub, cond, d, n, m, shift, flagMask;
```

在末尾加一个 `u8 fetchCycles` 后，字段总字节数从 14 变 15，按 4 字节对齐后
`sizeof(JitInsn)` 仍是 16。**已确认（2026-11-03）**：`JitIr.h` 和 `selftest.cpp` 各加了
一条 `static_assert(sizeof(JitInsn) == 16, ...)`；clang（MSYS2 clang64）双模式和
MSVC x64（`cl` 14.52.37025，`/std:c++17`）双模式都编译通过，断言未被触发。
如果以后还要加多个 V7 flag，建议改成 `u16 v7Flags` 或把 `flags` 扩成 `u32`，避免布局
再次变化。`selftest.cpp` 里对 `JitInsn` 的逐字段期望值需要同步更新。

**`fetchCycles` 还是 `rawOpcode`？（已确认，2026-11-03：选 `fetchCycles`）**

核对结果：**V7 Thumb 的周期不需要 `opcode >> 8` 以外的任何 opcode 位**，因此
`u8 fetchCycles` 足够，`rawOpcode` 没有正确性收益。

依据：

- V7 Thumb 的基周期只查 `thumbCycles[opcode >> 8]`（`thumb.h:184`）；`thumbCycles`
  是 256 项常量表，从不被改写（`GBA.cpp:113` 定义，`GBACpu.h:24` 声明，
  全仓库没有其它写入点）。
- 所有 handler 里对 `clockTicks` 的**赋值**只有条件分支的 `clockTicks = 3`
  （`thumb.h:1736-1866`，共 14 个 cond case）。分支是否成立由运行期标志决定，
  IR 用 `JOP_BRANCH` 的 `cond` 字段就能判断，不需要原始 opcode。
- 其余 handler 都是 `clockTicks += 常数` 或 `+= CPUUpdateTicksAccess*`：
  常数部分由指令形状（格式 4 移位、MUL、PUSH/POP/LDM/STM、单次传输）决定，
  IR 的 `op/sub/flags` 已经能表达；访问周期由运行期地址查表决定。
- MUL 的 `+1..+4` 分档依据是 `reg[(opcode >> 3) & 7]` 的**旧值**，不是 opcode
  位本身；IR 的 `d/n/m` 已能定位寄存器。
- ARM 的基周期只查 `memoryWaitFetch32[(pc >> 24) & 15]`，与 opcode 无关。

实现方式：

- `JitInsn` 末尾加 `u8 fetchCycles`；ARM 指令填 0，Thumb 指令填
  `thumbCycles[opcode >> 8]`。
- `JitDecodeThumb.cpp` 在 `#ifdef USE_GBA_CORE_V7` 下加一行
  `extern const int32 thumbCycles[256];`（不要 include `V7/GBACpu.h`，那个头会
  带入 `UPDATE_REG` 等宏，和 JIT 的独立编译目标不符）。
- 运行期 Thumb 周期 = `fetchCycles + memoryWaitFetch[(pc >> 24) & 15]`；
  `memoryWaitFetch` 是运行期数组，**不要**在解码时固化。
- `fetchCycles` 的值域是 1..4，`u8` 足够；即使以后表变大也远小于 255。

保留 `rawOpcode` 的唯一理由是调试/反汇编可读性，而 `JitDump.cpp` 只需要 IR 字段
就能打印，不需要原始字。因此第一版不加 `u16 rawOpcode`。如果将来要做“按原始
opcode 重建反汇编”，那是调试工具的事，不应进热路径的 `JitInsn`。

#### 3.2.1.8 V7 的 IR 解释器（`jitInterpInsn`）

V7 的 IR 解释器必须是**独立语义实现**，不能复用 V8 的周期公式。建议两种组织方式：

1. 在 `JitInterp.cpp` 里用 `#ifdef USE_GBA_CORE_V7` 分成两个 namespace；
2. 新建 `JitInterpV7.cpp`，在 V7 构建里替代 `JitInterp.cpp`。

**已定（2026-11-03）：走第 1 种**，在 `JitInterp.cpp` 原地替换。现有文件已经是
`#ifdef USE_GBA_CORE_V7 → 桩 / #else → V8 实现` 的形状，直接把桩换掉即可；
新建 TU 则要往 `win32/VBA_Latest.vcxproj` 加一个"只在 V72XD/V72XR 配置参与编译、
其余配置 `ExcludedFromBuild`"的条目，等于给工程文件增加按配置的复杂度，而收益为零。

无论哪种，必须遵守：

- **PC 模型**：`jitInterpInsn(insn, thumb, pc, &nextPc)` 的 `pc` 是当前指令地址；
  ARM 执行期 `reg[15] = pc + 8`、`armNextPC = pc + 4`；Thumb 执行期
  `reg[15] = pc + 4`、`armNextPC = pc + 2`。PC 相对寻址、`BL` 的 `lr`、Thumb
  高寄存器操作都要按这个约定算。
- **周期**：返回值 = 3.2.1.1 的基周期 + 3.2.1.3/3.2.1.4 的增量。V7 没有
  `cpuTotalTicks`，所以 `jitInterpInsn` **只返回本条指令的周期**，不要往全局累加。
- **访存（已确认，2026-11-03）**：默认走 `CPUReadMemory` / `CPUWriteMemory` /
  `CPUReadHalfWord` / `CPUReadHalfWordSigned` / `CPUWriteHalfWord` / `CPUReadByte` /
  `CPUWriteByte` 包装，保证 Lua READ/WRITE 钩子和 IO 副作用照常。**只有下面三类
  读取用 Quick 版本**（已逐条核对 V7 的 handler）：
  1. **取指**：ARM `CPUReadMemoryQuick(armNextPC)`（`arm-new.h:1411`）、
     Thumb `CPUReadHalfWordQuick(armNextPC)`（`thumb.h:183`）。JIT 的取指由解码器
     直接读 `JitSource`，等价于 Quick；但 Lua **EXEC** 钩子仍必须在钩子激活时整体
     回退解释器。
  2. **Thumb format 6：`LDR Rd, [PC, #imm8*4]`**（opcode `0x48`–`0x4F`，
     `thumb.h:894-950`）用 `CPUReadMemoryQuick`。
  3. **Thumb format 9：`LDR Rd, [SP, #imm8*4]`**（opcode `0x98`–`0x9F`，
     `thumb.h:1191-1247`）用 `CPUReadMemoryQuick`。

  其余全部走带钩子的包装，包括：ARM `LDR/LDRB/LDRH/LDRSB/LDRSH`（即使 Rd==15）、
  ARM `LDM`、Thumb 寄存器偏移/立即偏移的 LDR、Thumb `LDM/POP`、Thumb `LDRSH/LDRSB`。
  写入侧**没有** Quick：所有 `STR/STRH/STRB/STM/PUSH/POP` 都走 `CPUWrite*` 包装。
  因此 V7 的 IR 需要一个新 flag（`JF_QUICK = 1 << 10`，当前 `JitFlags` 用到
  `1 << 9`，未占用）表示“用 `CPUReadMemoryQuick`”，只给上面第 2、3 类置位。

  **V8 的现有实现有偏差，V7 不要照抄**：V8 的 `JitInterp.cpp:578` 对所有
  `MEM_WORD` 读取都调用 `CPUReadMemory`（带 Lua READ 钩子），包括
  `thumb48`（PC 相对 LDR）和 `thumb98`（SP 相对 LDR）；但 V8 解释器
  `GBA-thumb.cpp:875/1038` 用的是 `CPUReadMemoryQuick`。也就是说 V8 JIT 在
  Lua READ 钩子激活时，这两类加载会多触发一次钩子回调，而解释器不会。
  现有差分测试没有覆盖“Lua READ 钩子 + PC/SP 相对 LDR”的组合，所以没暴露。
  V7 实现时用 `JF_QUICK` 明确区分，别让这个偏差延续。
- **标志**：按 V7 的 `OP_*` 宏逐条实现。不要套 V8 的“写 PC 时不更新标志”规则；
  V7 在 `dest == 15` 分支里仍会执行 `OPCODE2`，标志更新后再由 `CPUSwitchMode`
  从 SPSR 覆盖（如果切模式）。
- **模式切换**：`CPUSwitchMode()` 会调用 `CPUUpdateCPSR()` / `CPUUpdateFlags()`，
  可能改变 `armState`、banked 寄存器，甚至触发 IRQ 检查。IR 解释器要像 V7 handler
  一样在正确的位置调用，不能只改 `armState`。
- **块出口**：IR 解释器只负责算 `nextPc` 和周期；`armNextPC`/`reg[15]` 的最终落点
  由块运行器统一设置（见 3.2.1.9）。分支/写 PC 的指令可以在解释器内部先设一次，
  块运行器最后再按 `nextPc` 覆盖，保证一致。
- **`CPUCheckDMA` 的同步问题（已确认，2026-11-03）**：V7 的 IO 写（地址高字节
  `0x04`）会在 `CPUWriteMemoryWrapped()` 里同步调用 `CPUCheckDMA()`；后者会读写
  CPULoop 的局部变量指针 `extCpuNextEvent` / `extClockTicks`
  （`src/gba/V7/GBA.cpp:2204-2208`），并按“当前指令的 clockTicks”重定事件倒计时。
  如果 JIT 把多条指令批量执行，`*extClockTicks` / `*extCpuNextEvent` 在 IO 写那一刻
  可能不是解释器里的值，DMA 时序就会漂。

  已核对的 `CPUCheckDMA()` 调用点只有六处：

  | 位置 | 触发条件 |
  |---|---|
  | `GBA.cpp:2773` | 写 `DM0CNT_H`（`0x040000BA`）且启动位置 1 |
  | `GBA.cpp:2809` | 写 `DM1CNT_H` |
  | `GBA.cpp:2847` | 写 `DM2CNT_H` |
  | `GBA.cpp:2884` | 写 `DM3CNT_H` |
  | `GBA.cpp:3879` | LCD 事件处理（V-Blank/H-Blank DMA） |
  | `GBA.cpp:3899` | LCD 事件处理（H-Blank DMA） |

  前四处都在 `CPUWriteMemoryWrapped()` 的 IO 写分支里，后两处只在 CPULoop 的事件
  处理块中执行。也就是说：**块内唯一可能触发 `CPUCheckDMA()` 的，是写
  `0x04xxxxxx` 的指令**；块内不会因为读 IO、写 RAM/ROM/VRAM 而触发它。

  **第一版确认采用保守方案**：IR 解释器在执行任何可能写 `0x04xxxxxx` 的指令前，
  先算地址；如果落在 IO 区，就**不执行**，返回特殊码 `JIT_NEED_INTERP`。块运行器
  停止，设置 `jitForceInterp`，让 CPULoop 用内联解释器执行这一条。这样
  `CPUCheckDMA` 看到的就是原汁原味的 `clockTicks` / `cpuNextEvent`。需要覆盖的写
  指令包括：`STR/STRB/STRH`、`STM`、Thumb `PUSH/STM`、`SWP/SWPB`；对 `LDM/STM/
  PUSH/POP` 要检查整个地址范围是否与 `0x04000000-0x04FFFFFF` 相交，不能只看首地址。
  代价是 IO 写那一条不进 JIT；这是正确性优先的取舍，性能影响待测量。

  **后续优化（不是第一版）**：在块运行器里逐条维护
  `*extCpuNextEvent = 剩余事件 - 已执行周期`，并让 IR 解释器按 V7 handler 的顺序
  更新 `*extClockTicks`，从而允许 IO 写也走 JIT。这条路要额外处理
  `CPUCheckDMA` 对 `cpuSavedTicks` 的改写，必须在 HashTrace 上验证后再启用。
- **`armStopAddr`（已确认，2026-11-03：JIT 不需要处理）**：V7 在
  `#ifndef FINAL_VERSION` 下有“执行到 `armStopAddr` 就自增 PC”的代码
  （`arm-new.h:1415-1420`、`thumb.h:185-190`），但它**实际上没有任何效果**：
  - ARM：`armNextPC++` 之后紧接着 `armNextPC = reg[15].I;`（`arm-new.h:1422`），
    自增结果被立即覆盖。
  - Thumb：写的是 `armNextPC = armNextPC++;`（`thumb.h:188`），后自增返回旧值再
    赋回去，等于不变；下一行 `armNextPC = reg[15].I;` 又覆盖一次。
  - `armStopAddr` 全仓库只有一个赋值点：`GBAGlobals.cpp:27` 的初始化
    `0x08000568`，没有任何代码修改它；且它本身包在 `#ifndef FINAL_VERSION` 里，
    正式构建根本不存在。
  - V8 的对应代码（`V8/GBA-arm.cpp:2350-2355`、`V8/GBA-thumb.cpp:1720`）也是
    同样的“自增后被 `armNextPC = reg[15].I` 覆盖”，所以 V8 JIT 也从未处理它。

  结论：JIT 不必在 `armStopAddr` 上设块边界或回退解释器，也不必复刻这个自增。
  如果将来有人把它改成真正的调试停点，再按“非零就回退解释器”处理；当前不是
  正确性风险。
- **Lua EXEC 钩子**：V7 的 CPULoop 在每条指令前检查
  `luaMemHookActive[LUAMEMHOOK_EXEC]`。JIT 必须在钩子激活时整体回退，且不能把
  “钩子是否激活”缓存进块。

#### 3.2.1.9 V7 的块缓存与 `jitExecute()` 接入

V7 的 `CPULoop` 与 V8 的结构不同：V7 没有 `armExecute()` / `thumbExecute()` 函数，
解释器是 `#include "arm-new.h"` / `"thumb.h"` 直接展开在 `CPULoop` 的循环体里
（`src/gba/V7/GBA.cpp:3768/3776`）。因此 V7 的 `jitExecute()` 不能像 V8 那样
“回退时调用 `armExecute()`”，只能把控制权交回 CPULoop 的内联解释器。

**契约（已确认，2026-11-03）**：

```c
// V7 专用语义（用 #ifdef USE_GBA_CORE_V7 区分）
// 返回值 >= 0：本次实际执行的 guest 周期总数；CPULoop 用它给 clockTicks 赋值。
// 返回值 <  0：一条都没执行；CPULoop 立即走内联解释器。
// 若执行了若干条后必须回退（IO 写、无法解码的下一条），返回已执行周期，
// 并置 jitForceInterp = true；CPULoop 下一次循环先跑一条内联解释器。
int jitExecute();
```

CPULoop 的接入示意（伪代码，实际改动要贴合现有缩进和 `#ifndef FINAL_VERSION`）：

```c
if (!holdState)
{
    int jitTicks = -1;
    if (jitEnabled && !jitForceInterp)
        jitTicks = jitExecute();

    if (jitTicks >= 0)
    {
        clockTicks = jitTicks;
        jitForceInterp = false;   // jitExecute 可能在内部置位
    }
    else
    {
        jitForceInterp = false;
        if (UNLIKELY(luaMemHookActive[LUAMEMHOOK_EXEC]))
            CallRegisteredLuaMemHook(...);
        if (armState)
            #include "arm-new.h"
        else
            #include "thumb.h"
    }
}
else
{
    // 原 holdState 分支不动
}
```

**事件记账**：

- 块运行器在每条指令后检查 `已执行周期 >= cpuNextEvent`，一旦越过就停在该条之后。
  这与解释器“执行完当前指令再处理事件”的顺序一致。
- 块结束时 CPULoop 只做一次 `cpuNextEvent -= clockTicks`（`GBA.cpp:3813`），其中
  `clockTicks` 是整块的周期总数。因为块在越过事件的第一条之后就停，越过量不会超过
  一条指令，`cpuDmaTicksToUpdate = -cpuNextEvent` 的语义与逐条执行相同。
- **不要**在块结束时预先把 `cpuNextEvent` 减掉 total；CPULoop 自己会减。否则会
  双倍扣除。
- `cpuSavedTicks` 在块内不需要逐条更新：V7 只在事件处理时用它，而块在事件边界就停。
- V7 没有 `SWITicks`；`holdState` 由 CPULoop 的 `else` 分支处理，JIT 不进入。
- 指令集切换（BX、写 PC、POP PC 等）后，块运行器要停，让下一条从新状态重新取块。

**块出口**：

```c
armNextPC = nextPc;
if (armState)
    reg[15].I = armNextPC + 4;
else
    reg[15].I = armNextPC + 2;
// 不要调用 ARM_PREFETCH / THUMB_PREFETCH：V7 没有预取缓冲。
```

**缓存与失效**：

- 块缓存、1 KB 页版本号、`JIT_PAGE_SIZE`、`JIT_MAX_INSNS` 都可以直接复用
  `JitCache.cpp` 的机制；V7 的 `memoryMap[256]` 与 V8 同构，`JitSource` 不用改。
- 失效只关心可写代码区 `0x02xxxxxx`（EWRAM）和 `0x03xxxxxx`（IWRAM）。在 V7 的
  `CPUWriteMemory` / `CPUWriteHalfWord` / `CPUWriteByte` 外层包装
  （`src/gba/V7/GBAMemory.cpp:703-722`）里，先判断地址高字节，再调用
  `jitCodeWritten(address, size)`。不要改 `*Wrapped` 内部，否则会漏掉 DMA/内部写。
- 整表作废的路径：
  - `CPUReset()`（`src/gba/V7/GBA.cpp:3162`）：在 `systemReset()` 之后、
    清 RAM 之前或之后都可以，建议放在函数开头；
  - `CPUReadStateFromStream()`（`src/gba/V7/GBA.cpp:775`）：成功 `return true`
    之前（`:1017`）以及失败路径 `failedLoad` 都要清；读档会改 `memoryWait*`；
  - `BIOS_RegisterRamReset()`（`src/gba/V7/bios.cpp:947`）：`memset` 清
    work/internal RAM 绕过写钩子，必须在 `flags != 0` 的分支里清；
  - `BIOS_SoftReset()`（`src/gba/V7/bios.cpp:1178`）：会清 IWRAM 尾部并跳
    `0x02000000` / `0x08000000`，在函数末尾清；
  - `jitSetEnabled(true)`：现有 V8 逻辑，V7 也要生效（去掉 `#ifndef
    USE_GBA_CORE_V7` 对 `jitResetCache()` 的包裹）。
- `WAITCNT` / `memoryWait*` 改变**不需要**清缓存：基周期是执行期查表，IR 里只存
  `fetchCycles`，不存 `memoryWaitFetch*` 的旧值。前提是实施时真的按这个设计做。
- `memoryMap` 改变（重置、读档、BIOS 初始化）通过上面的整表作废覆盖。

**统计与调试**：

- `JitStats` 的字段可以共用；`jitReportStats()` 不依赖 `cpuTotalTicks`。
- `jitTracePc()` 目前写 `cpuTotalTicks`（`JitCache.cpp:346`）。V7 没有这个全局，
  需要改成写 `*extClockTicks` 或块运行器的局部累计值；`JIT_PC_TRACE` 的 V7 版本
  **待实现**。
- `jitGetStats()` 在 V7 构建里不要再 `memset` 成 0，否则统计没意义。

**构建配置**：

- `V72XD` / `V72XR` 定义 `USE_GB_CORE_V7;USE_GBA_CORE_V7`；V8 配置不定义。
  JIT 文件已经在 `win32/VBA_Latest.vcxproj` 里，V7 核心文件是 `ClInclude`
  （被 `GBA.cpp` include），不需要再往 `ClCompile` 加。
- JIT 代码不能依赖 MFC；V7 的 `GBA.cpp` 有 MFC 依赖，但 `src/gba/jit/*` 必须保持
  独立，否则将来 wx 前端用不了。
- **构建环境（已确认，2026-11-03）**：本机有 VS 18 Insiders 的 MSVC
  （`14.52.37025`）和 MSBuild，宿主是 ARM64；`V7Release|ARM64` 与
  `Release|ARM64` 的完整构建都能通过。**MFC 只有 arm64 库**，所以 x64/Win32
  的完整链接做不了（`MSB8041`），但 x64 `cl.exe` 可以做 JIT 文件的编译检查。
  详见 3.2.1.12 第 13 条。

#### 3.2.1.10 V7 的验证策略

现有 `-jitverify`（`src/gba/jit/JitVerify.cpp`）是**按 V8 写的**：它调用
`armExecute()` / `thumbExecute()`，比较 `cpuTotalTicks` / `busPrefetchCount` /
`cpuPrefetch[]`。V7 没有这些函数和全局，不能直接复用。

**已确认（2026-11-03）**：V7 的单条参考函数**可以**干净地抽出，推荐方案如下。

`arm-new.h` / `thumb.h` 对 CPULoop 局部变量的依赖比想象中少：

- 它们**不直接使用** `cpuNextEvent`、`ticks`、`holdState`；只使用一个局部
  `clockTicks`（ARM 在 `arm-new.h:1413` 赋值，Thumb 在 `thumb.h:184` 赋值）。
- `extCpuNextEvent` / `extClockTicks` / `extTicks` 只通过
  `CPUUpdateFlags()` / `CPUCheckDMA()` 间接出现，不在头文件正文里。
- 头文件里的 `return;` 只有两处，都在 `#ifdef BKPT_SUPPORT` 的调试器路径：
  `arm-new.h:2497`、`thumb.h:1936`。如果参考函数返回 `int`，这两处要改成
  `return clockTicks;`；或者把参考函数写成 `void`，用出参/全局回传周期。
- 头文件没有 include guard，但 CPULoop 现在已经在同一个 TU 里先后 include 过
  两者，所以再包进两个函数不会引入新的宏冲突。

**推荐做法**：在 `src/gba/V7/GBA.cpp` 里、现有宏和全局声明之后，新增：

```c
#ifdef USE_GBA_CORE_V7
int armExecuteOne()
{
    int clockTicks = 0;
    #include "arm-new.h"
    return clockTicks;
}

int thumbExecuteOne()
{
    int clockTicks = 0;
    #include "thumb.h"
    return clockTicks;
}
#endif
```

然后把两个 BKPT 的 `return;` 改成 `return clockTicks;`。用一个小头文件
（例如 `V7/CpuExecute.h`）声明这两个函数，供 `JitVerify.cpp` 使用。

不建议把这两个 include 搬到新的 `.cpp`：`arm-new.h` / `thumb.h` 依赖 GBA.cpp
里大量的全局、inline helper 和宏（`CPUReadMemoryQuick`、`CPUUpdateTicksAccess*`、
`cpuBitsSet`、`memoryWait*`、`thumbCycles`、`CPU_BREAK_LOOP_2` 等），单独建 TU
需要先抽出一大段 prelude，风险高、收益低。

注意：参考函数只执行一条指令，**不处理事件循环**。验证器应避免让它执行会触发
`CPUCheckDMA` 的 IO 写，或者在调用前把 `extCpuNextEvent` / `extClockTicks` 设成
与解释器一致的值；这也是逐指令差分要跳过 IO 写的原因之一。

验证层次（与 V8 第十节对应）：

| 层 | V7 做法 | 比较内容 |
|---|---|---|
| 0 解码空间全扫 | 复用 `sweepDecodeSpace` 的思路，但用 V7 的未定义判定 | 解码器是否接受/拒绝同一批编码 |
| 1 解码自测 | 扩展 `selftest.cpp`，加 V7 的 `fetchCycles`、半字空洞、ROR 用例 | `JitInsn` 字段 |
| 2 逐指令差分 | 方案 1 的参考函数 vs `jitInterpInsn()` | `r0-r15`、CPSR、N/Z/C/V、本条周期、`armNextPC` |
| 2.5 块级差分 | 快照 workRAM+IWRAM，A 跑解释器 N 条，B 跑 `jitRunBlockForTest()` | 寄存器、内存、`armNextPC`、周期；V7 没有 `cpuPrefetch`/`busPrefetchCount` |
| 3 端到端 | `-play *.vbm` + `-hashtrace`，`-jit` / `-nojit` 各跑一遍 | 每帧 `ram` / `full` 哈希逐字节相同 |

预期跳过/回退的情况：

- `SWI`：进 BIOS，副作用收不回来；
- `UNDEF`：V7 会进未定义异常，可能弹模态框；
- 控制域 `MSR`：切模式/改 T 位；
- `^` 块传输：banked 寄存器；
- IO 写（第一版保守方案）：`CPUCheckDMA` 时序；
- 调试器 / BKPT / `armStopAddr`；
- Lua EXEC 钩子激活时的整段。

**确定性红线**：同一份 `.vbm`、同一份设置（`-jit` 或 `-nojit`）必须得到同一份逐帧
哈希。JIT 状态不进存档；读档后清缓存。V7 和 V8 的哈希**不需要**互相相同——它们是
两个不同的核心，周期模型本来就不同；要比较的是“同一个核心，JIT 开/关”。

#### 3.2.1.11 实施顺序（V7）

1. **文档**：把 3.2.1 的结论写进设计文档（本次已完成）。
2. **IR（已完成，2026-11-03）**：`JitIr.h` 已加 `u8 fetchCycles`、`JF_QUICK`
   （`1 << 10`）、`SHIFT_ROR_REG_V7`（独立枚举，追加在 `JitShift` 末尾），
   并加 `static_assert(sizeof(JitInsn) == 16)`；`JitInterp.cpp` 的
   `evalOperand2()` / `isRegShift()` 已支持 V7 ROR。
3. **解码器（已完成，2026-11-03）**：`JitDecodeArm.cpp` / `JitDecodeThumb.cpp`
   已加 `USE_GBA_CORE_V7` 分支：
   - Thumb 每个块条目填 `fetchCycles = thumbCycles[opcode >> 8]`；
     `JitDecodeThumb.cpp` 里用 `extern const int32 thumbCycles[256]` 自行声明，
     不 include `V7/GBACpu.h`（那个头会带进 `UPDATE_REG` 等宏，破坏 JIT 文件的独立编译）；
   - V7 下删除 V8 的 `P=0,W=1` 半字空洞规则（`sign && !load` 仍保留）；
   - ARM ROR by register 在 V7 下产生 `SHIFT_ROR_REG_V7`；
   - Thumb `0x48` / `0x98` 的 **LDR** 置 `JF_QUICK`，`0x90`（STR）不置；
   - `selftest.cpp` 加了 V7 专用断言：`fetchCycles` 与 `thumbCycles[opcode >> 8]`
     逐条比对、ARM 填 0、半字空洞六键可解码、`0x48/0x90/0x98` 的 `JF_QUICK` 组合。
     宿主测试不链接 `V7/GBA.cpp`，所以 V7 模式下用一个**合成的 256 项 `thumbCycles`**
     （`TC64` 宏展开成 1..4 循环）来证明"解码器确实按 `opcode >> 8` 索引"，
     而不是证明表的数值。
   - 双模式结果：V8 1101 项、V7 1207 项，0 失败（clang 与 MSVC x64 都编过）。
   - 控制域 MSR、`^` 块传输仍拒绝（下一步接 IR 解释器时一起做）。
4. **IR 解释器（下一步，唯一在写的代码）**：把 `JitInterp.cpp` 里那段
   `#ifdef USE_GBA_CORE_V7` 的 `return -1` 桩换成真正的 V7 实现，按 3.2.1.3/3.2.1.4
   实现所有 op 的语义和周期。落地方式见 3.2.1.13 的"下一步"一节——**不再新建
   `JitInterpV7.cpp`**：现有文件已经是 `#ifdef V7 → 桩 / #else → V8 实现` 的结构，
   在原地替换分支比往 vcxproj 里加一个"只在 V7 配置编译"的新 TU 更省事，也避免
   `ExcludedFromBuild` 这类按配置的工程改动。
   `SWI` / `UNDEF` 先返回 `JIT_NEED_INTERP`，遇到 IO 写也返回 `JIT_NEED_INTERP`。
5. **块缓存/调度**：`JitCache.cpp` 加 V7 的 `runBlock()` / `jitExecute()`：
   - 局部累计 `total`，不碰 `cpuTotalTicks`；
   - 每条后检查事件/`holdState`/指令集切换；
   - IO 写回退置 `jitForceInterp`；
   - 块出口只设 `armNextPC` / `reg[15]`；
   - `jitAvailable()` 在 V7 构建里返回 `true`（等验证通过后再打开）。
6. **CPULoop 接入**：`src/gba/V7/GBA.cpp` 的 `if (!holdState)` 分支按 3.2.1.9
   改；保留原内联解释器作为回退。
7. **失效钩子**：`src/gba/V7/GBAMemory.cpp` 三个写包装加 `jitCodeWritten()`；
   `CPUReset()`、`CPUReadStateFromStream()`、`BIOS_RegisterRamReset()`、
   `BIOS_SoftReset()` 加 `jitResetCache()`。
8. **验证**：先做 V7 单条参考函数，再跑逐指令/块级差分；最后用 V72XR + `.vbm`
   + `-hashtrace` 做端到端。
9. **native 后端**：等 IR 解释器全绿后，再把编译期 `USE_GBA_CORE_V7` 分支接到
   x64/ARM64 发射器；V7 的周期公式只出现在 lowering/runtime，不写进编码器。
   Win32 不做机器码后端，只保留 IR 解释器；V7 不先于 V8 单独写发射器
   （见 3.2.1.12 第 8 条）。
10. **AOT**：最后做，复用同一套 V7 IR 语义。

#### 3.2.1.12 待确认清单（13 项已全部确认，2026-11-03）

下面 13 条在 2026-11-03 已逐条核对完毕，**保留原始编号和结论作为决策记录**，
实施时按这些结论执行。仍未闭环的只剩“本机没有 ROM，端到端 HashTrace 尚未跑”
（第 13 条），它属于验收环境问题，不是设计未知项。

1. **MSR CPSR（已确认，2026-11-03）**：**所有**写 CPSR 的 MSR 都拒绝，包括只写
   标志域的 `MSR CPSR_f`。原因见 3.2.1.5：V7 的 MSR 无条件调用
   `CPUSwitchMode(..., breakLoop=true)` 和 `CPUUpdateFlags(true)`，可能触发
   `CPU_BREAK_LOOP_2` 并改写 `ext*`。MSR SPSR / MRS 没有这些副作用，可以 JIT；
   是否第一版就做，待性能测量。
2. **MSR 切 T 位的 +2 怪癖（已确认，2026-11-03）**：由于第 1 条已经拒绝所有
   MSR CPSR，JIT 不需要复刻这个怪癖；解释器会按原样执行。只有将来放开
   “标志域 MSR”时才需要重新评估。
3. **`CPUCheckDMA` / IO 写（已确认，2026-11-03）**：第一版用“任何写
   `0x04xxxxxx` 的指令都回退解释器”是足够的；`CPUCheckDMA()` 的六个调用点里，
   块内只可能由 IO 写触发，事件处理的两个调用点在块外。`LDM/STM/PUSH/POP` 要按
   地址范围判断。性能影响待测量；`ext*` 逐条同步是后续优化，不是第一版必需。
4. **V7 单条参考函数（已确认，2026-11-03）**：可以在 `src/gba/V7/GBA.cpp` 里用局部
   `clockTicks` 包住 `#include "arm-new.h"` / `"thumb.h"` 抽出
   `armExecuteOne()` / `thumbExecuteOne()`；头文件不直接用
   `cpuNextEvent` / `ticks` / `holdState`，只需把两处 BKPT 的 `return;` 改成
   `return clockTicks;`。细节见 3.2.1.10。
5. **Thumb ROR（已确认，2026-11-03）**：V7 的 `ROR_RD_RS` 与 V8 的
   `SHIFT_ROR_REG_T` 逐条一致（raw=0 不写 C、raw&31=0 写符号位、否则正常旋转），
   可以原样复用。只有 ARM 的 ROR by register 需要新移位模式，见 3.2.1.6。
6. **`fetchCycles` vs `rawOpcode`（已确认，2026-11-03）**：选 `u8 fetchCycles`。
   V7 Thumb 的周期只依赖 `thumbCycles[opcode >> 8]`；唯一的赋值型周期是条件分支
   的 `clockTicks = 3`，由 IR 的 `cond` 判断；其余周期由指令形状或运行期地址查表
   决定。`rawOpcode` 没有正确性收益，第一版不加。见 3.2.1.7。
7. **`JitCoreOps` 运行时接口 vs 编译期 `USE_GBA_CORE_V7`（已确认，2026-11-03）**：
   采用**编译期分支**。V7/V8 是两个互斥的构建配置，两个 `GBA.cpp` 定义同名全局，
   不可能同进程共存；运行时函数指针会阻止内联并给热路径加间接调用。建议把按核
   helper 收进编译期命名空间，调用点形状与 `JitCoreOps` 相同但不做运行时分派。
   `JitInsn` 布局仍须两核一致。见 3.2.1 开头。
8. **V7 native 后端范围（已确认，2026-11-03）**：分两层，范围与 V8 一致。
   - **第一层：IR 解释器**（当前仓库里的“JIT”就是这一层）。平台无关，Win32 / x64 /
     ARM64 都能跑。V7 的第一交付物就是它，目的是正确性 + 差分验证，并给 native
     后端提供规范实现。验证通过后 `jitAvailable()` 在 V7 三个平台都返回 `true`，
     但仍默认关闭（和 V8 一样）。
   - **第二层：机器码后端**。范围与 3.2.1 及第七节一致：**只做 x64 + ARM64，
     不做 x86-32**；Win32 的 V7 构建永远留在 IR 解释器/内联解释器。V7 不单独
     写机器码后端，而是等 V8 的 x64/ARM64 发射器落地后，用编译期
     `USE_GBA_CORE_V7` 切换 lowering（见第 7 条）。
   - 顺序：V7 IR 解释器 → V7 差分/端到端验证 → V8 x64 后端 → V7 x64 lowering →
     V8/V7 ARM64。不要在 V8 机器码后端还不存在时先写 V7 专用后端，那会变成
     两套要同时维护的发射器。
   - 构建配置：V72XD/V72XR 有 Win32 / x64 / ARM64 三种 Platform
     （`win32/VBA_Latest.vcxproj:4-43`），所以“V7 不支持机器码”只影响 Win32
     配置的 native 部分，不影响 V7 IR 解释器在 Win32 上可用。
9. **`armStopAddr` / `FINAL_VERSION`（已确认，2026-11-03）**：JIT 不需要处理。
   V7 的“命中 `armStopAddr` 就自增 PC”在自增后立刻被 `armNextPC = reg[15].I`
   覆盖，Thumb 那行 `armNextPC = armNextPC++` 本身也不改变值；`armStopAddr`
   只有一个初始化赋值且包在 `#ifndef FINAL_VERSION` 里。V8 同样是死代码。
   不设块边界、不回退、不复刻。见 3.2.1.8。
10. **单次传输 bit4=1 的全表确认（已确认，2026-11-03）**：已用脚本展开 V7 的
    `case` 与 `CASE_16`，穷举分派键
    `((opcode >> 16) & 0xFF0) | ((opcode >> 4) & 0x0F)`：
    - **立即数形式**（bit25=0，键 0x400–0x5FF）：`CASE_16` 覆盖全部 256 个键，
      bit4 是 `imm12` 的 bit4，不是寄存器移位标志，没有空洞。
    - **寄存器偏移形式**（bit25=1，键 0x600–0x7FF）：128 个偶数键（bit4=0）
      **全部实现**，无缺失；奇数键（bit4=1）**全部**落到
      `default: CPUUndefinedException()`（`arm-new.h:6691-6698`）。
    - 唯一例外是 `#ifdef BKPT_SUPPORT` 的 `case 0x7ff`（GDB 陷阱），它本身也是
      “回退解释器”的调试路径，不影响 JIT 解码器的拒绝规则。
    结论：V8 解码器的 `if (!immF && (opcode & 0x10)) return false;`
    （`JitDecodeArm.cpp:219`）与 V7 完全一致，V7 解码器**不需要**放宽这一条。
11. **V7 的 `LDR`/`STR` 半字空洞（已确认，2026-11-03）**：已展开 V7 的
    `case` 列表穷举分派键，结论是 **V7 比 V8 多实现了六种组合**：
    - 半字/有符号组（键 0x000–0x1FF，低半字节 0xB/0xD/0xF）：
      `STRH`（S=0,H=1,L=0）、`LDRH`（S=0,H=1,L=1）、`LDRSB`（S=1,H=0,L=1）、
      `LDRSH`（S=1,H=1,L=1）各自实现**全部 16 种 P/U/I/W 组合**。
    - `S=1 && L=0`（有符号存储 / STRD）在 V7 里**没有**任何 case，落到
      `default: CPUUndefinedException()`，与 V8 的 `sign && !load` 一致。
    - V8 表的空洞是 `P=0,W=1` 且非 `U=1,I=1` 的六个键：`0x02B`、`0x03B`、
      `0x06B`、`0x07B`、`0x0AB`、`0x0BB`（`GBA-arm.cpp:2111/2113/2119/2121/2127/2129`）。
      V7 明确列出了 `0x02b/0x03b/0x06b/0x07b/0x0ab/0x0bb`，执行语义与对应的
      `W=0` 形式相同（P=0 时 W 位被忽略，总是回写 base）。
    - 因此 V7 解码器要**删除** V8 的
      `half && !sign && !pre && wb && !(up && immF)` 拒绝条件
      （`JitDecodeArm.cpp:173`），但保留 `sign && !load`（`:171`）。
    - 周期不受影响：半字/有符号读仍是 `+3 + Access16`，写仍是 `+4 + Access16`；
      P/U/I/W 只决定地址和回写，不改变周期。
12. **`CPUReadMemoryQuick` 的使用边界（已确认，2026-11-03）**：V7 handler 里只有
    三类 Quick 读取：
    - 取指（ARM `arm-new.h:1411`、Thumb `thumb.h:183`）；
    - Thumb format 6 `LDR Rd,[PC,#imm]`（`0x48`–`0x4F`，`thumb.h:894-950`）；
    - Thumb format 9 `LDR Rd,[SP,#imm]`（`0x98`–`0x9F`，`thumb.h:1191-1247`）。
    其余 ARM/Thumb 数据读取（含 ARM `LDR` 到 PC、`LDM`、Thumb `LDM/POP`、
    `LDRSH/LDRSB`）全部走带 Lua 钩子的 `CPUReadMemory` / `CPUReadHalfWord` /
    `CPUReadHalfWordSigned` / `CPUReadByte`。写入侧没有 Quick，全部走 `CPUWrite*`。
    V7 的 IR 需要新增 `JF_QUICK`（`1 << 10`）标记第 2、3 类。
    另：V8 的 `JitInterp.cpp` 对所有字读取都用 `CPUReadMemory`，与解释器的
    `CPUReadMemoryQuick` 不一致（Lua READ 钩子激活时多一次回调）；V7 不要照抄。
    见 3.2.1.8。
13. **构建/测试环境（已确认，2026-11-03）**：本机**有** MSVC，且宿主是 **ARM64**，
    ARM64 的完整 V7/V8 构建都能在本机跑通。已核实的细节：
    - 工具链：`C:\Program Files\Microsoft Visual Studio\18\Insiders`，
      MSBuild `18.12.2.51402`，MSVC 工具集 `14.52.37025`，Windows SDK
      `10.0.28000.0`。都**不在 PATH**，要用绝对路径。
    - **MFC 只装了 arm64**：`atlmfc\include` 有头文件，
      `atlmfc\lib` 下只有 `arm64`（`afxnmcd.lib`、`atls.lib` 等），没有
      `x64` / `x86`。
    - 解决方案配置名是 `V7Debug` / `V7Release` / `Debug` / `Release`；
      `V72XR` 是**项目**配置名。对 `.sln` 要用
      `/p:Configuration=V7Release /p:Platform=ARM64`，
      写 `V72XR|x64` 会得到 `MSB4126`。
    - **已验证通过**（本机 ARM64）：
      `msbuild win32/VBA_Latest.sln /p:Configuration=V7Release /p:Platform=ARM64`
      → `win32\arm64\V72XR\VBA_VS_Latest_V72XR.exe`；
      `Release|ARM64` → `win32\arm64\V82XR\VBA_VS_Latest_V82XR.exe`。
      两者都成功，说明 V7 改动不会破坏 V8 构建。
    - **x64 / Win32 的完整构建本机做不了**：`V7Release|x64` 失败于
      `MSB8041`（需要 MFC 库；语言 DLL 仍能生成）。x86 同理（没有 x86 MFC）。
    - 但 **x64 的编译检查仍可做**：`vcvars64.bat` 后
      `cl /c /EHsc /std:c++17 /DUSE_GBA_CORE_V7 /DUSE_GB_CORE_V7 /I src
      /I src/gba /I src/gba/V7 /I win32/lib/zlib src/gba/jit/JitInterp.cpp`
      能通过（`zlib.h` 在 `win32/lib/zlib`，不在默认 include 里）。
    - `V72XR` 三个平台都定义了 `USE_GB_CORE_V7;USE_GBA_CORE_V7`，JIT 六个
      `.cpp` 都参与编译；已确认 `win32\.VS_Latest\V72XR\ARM64\Jit*.obj` 生成。
    - **仍然缺的**：~~仓库里没有 ROM~~ **已过时（2026-11-03 复核）**：
      `win32/x64/V72XR/roms/` 里有 `mario.gba` / `sonic.gba` / `aos.gba`，
      第十节的 `-jitverify` / HashTrace 门槛现在可以在本机跑，实际结果见 3.2.1.13。
      宿主侧仍然是 `selftest.cpp`（clang++，V8 1101 项 / V7 1207 项断言）。

#### 3.2.1.13 进度快照与下一步（2026-11-03 夜，V7 已能跑）

本节是 3.2.1.11 的**实时对照**：3.2.1.11 是"计划顺序"，本节是"现在的实际位置"。

**一句话结论**：V7 的 JIT **跑起来了，而且与解释器等价**。`jitInterpInsn()` 的 V7 分支是
完整实现，`jitExecute()` 有 V7 版本，`jitAvailable()` 在 V7 下返回 `true`，`CPULoop` 已接上；
`-jitverify` 在三个真实 ROM 上逐指令 + 逐块比对 **0 差异**，端到端 `-hashtrace` 在
`aos.gba` 上跑 12000 帧逐帧哈希与 `-nojit` **逐字节相同**（数字见 3.2.1.15）。
剩下的问题都是"要不要更快"，不是"能不能对"。（实现过程中挖出的 V7 语义细节见 3.2.1.14，
其中若干条与 3.2.1.3/3.2.1.4 原先的表述不一致，以 3.2.1.14 为准。）

**本轮改动清单**（V7 那一轮，提交 `484443c`；之后 `75f0057` 修掉 V8 的块级差异、
`b499a1b` 把验证器的区域快照与定位两核共用，见 3.2.1.14 第 12 条）：

| 文件 | 改了什么 | 状态 |
|---|---|---|
| `src/gba/jit/JitIr.h` | （上一轮）`SHIFT_ROR_REG_V7`、`JF_QUICK`、`fetchCycles`、`static_assert`；**本轮**新增 `JIT_NEED_INTERP (-2)` 与其契约注释 | 完成 |
| `src/gba/jit/JitInterp.cpp` | V7 分支从 `return -1` 桩换成完整实现：ALU / MUL / LDR-STR / LDM-STM-PUSH-POP / SWP / B-BL / BX / MRS / MSR(SPSR)，周期按 `memoryWaitFetch*` + handler 增量；`SHIFT_ROR_REG_V7`；ARM 寄存器移位 raw≥32 回退；IO 写回退 | 完成 |
| `src/gba/jit/JitCache.cpp` | 缓存机制从 `#ifndef V7` 里提出来两核共用；V7 的 `runBlock()` / `jitExecute()`（局部累计周期、事件边界、`jitForceInterp`、reg[15] 不变式检查）；`jitAvailable()` 恒真；`jitGetStats()` V7 也返回真数据；`jitReportStats()` 同时写 `jit.log`（GUI 无 stderr） | 完成 |
| `src/gba/jit/JitCache.h` | `jitRunBlockForTest(block, bool *needInterp)` 返回周期数；`extern bool jitForceInterp` | 完成 |
| `src/gba/jit/JitDecodeArm.cpp` | V7 下**整块拒绝**所有写 CPSR 的 MSR（寄存器/立即数两种） | 完成 |
| `src/gba/jit/JitDecodeThumb.cpp` | `JF_DATA32` 只在 V8 置位（V7 的 `0x5a` 用 `Access16`） | 完成 |
| `src/gba/jit/JitDump.cpp` | `kShiftName` 扩到 12 项（V7 的 ROR-by-register 原来会读越界）；`fetchCycles` 非零时打印 `[fc=n]` | 完成 |
| `src/gba/jit/JitVerify.cpp` | V7 差分：单条 + 块级两趟，参考侧用 `armExecuteOne()`/`thumbExecuteOne()`；跳 SWI/UNDEF/MSR/内存写；块内发现地址跑出块就放弃该块；块级快照覆盖 workRAM+IWRAM+palette+VRAM+OAM+ioMem（3.2.1.14 第 10 条）；失败时本地化到"第一条分叉的指令"并打印寄存器/内存差异 | 完成 |
| `src/gba/jit/selftest.cpp` | V7 的 MSR CPSR 拒绝 / MSR SPSR 接受的用例；`JF_DATA32` 分核期望值 | 完成 |
| `src/gba/V7/GBA.cpp` | `CPULoop` 接入 `jitExecute()`（`-1` 走内联解释器，`>=0` 赋给 `clockTicks`）；`jitResetCache()` 在 `CPUReset()` 开头与读档成功/失败处；`armExecuteOne()`/`thumbExecuteOne()` 参考函数 | 完成 |
| `src/gba/V7/GBAMemory.cpp` | 三个写包装加 `jitCodeWritten()`（先判 `0x0E000000` 区，再 bump 页版本） | 完成 |
| `src/gba/V7/bios.cpp` | `BIOS_RegisterRamReset()` 与 `BIOS_SoftReset()` 里 `jitResetCache()`（`memset` 绕过写钩子） | 完成 |
| `src/win32/MainWnd.cpp` | `-jitverify` 的报告在无 stdout 句柄时写 `jitverify.log`（原来 `AllocConsole()` 在本机总是失败 → 验证器什么都不打印就退出，见下面的注意） | 完成 |

**V7 的块运行器契约**（`JitCache.cpp`）：

```c
// 返回本次执行的周期数；-1 表示一条都没执行，CPULoop 立即走内联解释器。
int jitExecute();
```

- 周期只在块运行器里累计（局部 `total`），**不碰** `cpuTotalTicks`（V7 没有这个全局）。
- 每条指令后停的条件与解释器一致：`total >= 预算`、`holdState`、指令集切换。
  预算来自 `*extCpuNextEvent`（CPULoop 的局部变量），块结束后 CPULoop 自己
  `cpuNextEvent -= clockTicks`，绝不重复扣。
- 块出口：`armNextPC = 下一条`，`reg[15] = 下一条 + 4/2`；**没有**预取缓冲要重填。
- `JIT_NEED_INTERP`：IR 说"这条我处理不了"，块就地停下、`armNextPC` 指着这条，
  置 `jitForceInterp`，由 `CPULoop` 用内联解释器执行**这一条**。原因只有三类：
  会调用 `CPUSwitchMode(..., breakLoop=true)`/`CPUCheckDMA()`（它们经 `ext*` 改写
  CPULoop 的局部量）；语义依赖宿主的 UB（3.2.1.14 第 3 条）；V7 自己没把 PC 效果实现成
  跳转的加载指令（第 4 条，以及后索引字加载那条，第 9 条）。
- 每次进块前检查 `reg[15] == armNextPC + 4/2`；不成立就先跑一条解释器。这条不变式
  是"V7 用 `reg[15]` 当下一条地址"的直接推论，也是 3.2.1.14 第 5 条那个坑的解药。

**验证状态**（本机实跑）：

| 验证项 | 命令 | 结果 |
|---|---|---|
| 解码自测 V8 | `clang++ -std=c++17 -I src/gba/jit selftest.cpp JitDecodeArm.cpp JitDecodeThumb.cpp JitDump.cpp` | 1101 checks, 0 failure |
| 解码自测 V7 | 同上加 `-DUSE_GBA_CORE_V7 -DUSE_GB_CORE_V7` | 1207 checks, 0 failure |
| MSVC 单 TU 编译 | 6 个 JIT `.cpp`，V7 / V8 两套宏 | 0 error |
| V72XR x64 完整构建 | `cl @clv7.rsp` + `link @lnkv7.rsp`（工程同参数，见下） | 出 `win32\x64\V72XR\VBA_VS_Latest_V72XR.exe` |
| V7 差分（真实 ROM） | `VBA_VS_Latest_V72XR.exe -rom roms\<rom>.gba -jitverify 20000` | mario / sonic / aos 三趟各 20000 条：单条与块级**全部 0 差异**（明细见 3.2.1.15） |
| V7 端到端 | `... -play movies\<vbm> 1 -hashtrace t.csv -jit\|-nojit -quitAfter 12000` | 12000 帧逐帧哈希**完全一致** |
| V8 差分（回归） | `VBA_VS_Latest_V82XR.exe -rom ... -jitverify 20000` | mario / sonic / aos 三趟各 20000 条：单条与块级**全部 0 差异**（块级 pass 首次在真实 ROM 上跑通，见 3.2.1.14 第 8/12 条） |

**本机怎么跑验证器**（踩过的坑）：

- `-jitverify N` 的报告走 `printf`。GUI 进程没有 console 时，原来那套
  `AllocConsole()` + `getchar()` 在本机**必然失败**（进程已经有一个 console，
  `AllocConsole` 返回 `ERROR_ACCESS_DENIED`），于是验证器一声不响地 `exit(0)`。
  现在没有有效 stdout 句柄时直接 `freopen("jitverify.log")`，报告一定能拿到。
- 别用 `cmd /c` 或 `Start-Process -RedirectStandardOutput` 去抓它：前者被沙箱拦，
  后者给 GUI 进程的句柄在这个沙箱里不生效。直接 `Start-Process`，读 `jitverify.log`。
- 跑之前先确认没有别的 VBA 进程：验证器在没装 BIOS 时会为 `SWI` 弹模态框
  （"Unsupported BIOS function ..."），被挡住的那次会把窗口留在屏幕上。
- `win32/x64/V72XR/roms/` 里有 `mario.gba` / `sonic.gba` / `aos.gba`，
  `movies/` 里有对应的 `.vbm`，端到端 HashTrace 不再受 ROM 限制。

**下一步（按优先级）**：

1. ~~端到端 HashTrace~~ **已完成**（2026-11-03）：3000 / 12000 帧逐帧哈希完全一致，
   `jit.log` 的 `bad` 为 0。这一层还顺带抓出了 3.2.1.14 第 9-11 条。
2. ~~块统计~~ **已量过**（3.2.1.15 / 3.2.1.16）。原以为命中率偏低是 1024 项缓存的容量
   问题，**实测否掉了这个猜测**：4096 项把解码砍掉 35%，时间只省 1.2%（3.2.1.16）。
   性能工作不该从这里开始。
3. **machine-code 后端**（3.2.1.11 第 9 步）：先 V8 的 x64，再按编译期 `USE_GBA_CORE_V7`
   切 lowering；V7 不单独写发射器。3.2.1.16 的基线是 **JIT 比解释器慢 1.1–1.5 倍**，
   这是唯一一步能把它变成加速的工作。
4. ~~V8 的块级差异~~ **已完成**（`75f0057`）：`stm r15!` / `str rX, [r15, ...]!` 这类
   "写回把 reg[15] 改了"的指令，V8 的块运行器现在停下并把 `armNextPC` 交回解释器，
   `jitDetour` 保证两个解释器回合内不再从它解码。顺带修了 thumb48/98 的 `JF_QUICK`
   与 Thumb 高寄存器写 PC 的 `codeTicksAccessSeq16 * 2`（3.2.1.14 第 12 条）。
5. **把 `-jitverify` 的 N 提高**（3.2.1.14 第 9 条的教训）：N 要按 ROM 覆盖到最远的代码，
   现在三个 ROM 用 20000 已能覆盖其全部代码段，换 ROM 时要重新确认。

**注意（三件容易踩的）**：

- `win32/VBA_Latest.vcxproj` 的改动**和 JIT 无关**：给 `V82XR|x64`（就是跑 JIT 的那个
  Release 配置）加了 `<EnableASAN>true</EnableASAN>`，还有 `V82XD|x64` 的
  `RunCodeAnalysis`、`V82XR|x64` 的空 `<SDLCheck>` / `<MultiProcessorCompilation>`。
  这是 VS 属性页的残留，会改变 Release 构建（ASAN 插桩、慢一个数量级、依赖 ASAN 运行时）。
  提交前应该 `git checkout -- win32/VBA_Latest.vcxproj`。
- ~~`JitInterp.cpp` 里 V8 的"所有字读取都走 `CPUReadMemory`"是已知偏差~~ **已修**
  （`75f0057`）：thumb48/thumb98 在 V8 解释器里也走 `CPUReadMemoryQuick`，解码器现在
  两个核都置 `JF_QUICK`。原先只有 V7 置，脚本（Lua READ 钩子）一开，V8 JIT 每条
  PC/SP 相对字加载都会多一次回调。
- `selftest` 在 V7 模式下校验的是"索引对了"，不是"表的数值对了"——合成表是 1..4 循环。
  真正的 `thumbCycles` 数值只能在 V72XR 里通过差分验证，宿主测试给不了这个保证。
- **本机 V82XR 的目标文件目录是混着两套编译参数的**（`LNK2038: _ITERATOR_DEBUG_LEVEL`），
  所以"改一个文件再链一次"会失败。要重建就得整份重编：`.cpp` 用 `cl.rsp`（带 `/TP`），
  `.c`（zlib / SFMT / memgzio）必须用 `/TC`，否则符号会被 C++ 修饰而挂不上。
  脚本：`~/.workbuddy/scratch/rebuild-v82xr.ps1` + `rebuild-v82xr-c.ps1`。



#### 3.2.1.14 实现与差分过程中确认的细节（含 3.2.1.4 的一处错误）

下面每一条都是本轮"写代码 + 跑差分"才暴露出来的，逐条附上依据。前三条会直接影响
周期或寄存器值，后面几条是块化带来的结构性问题。

1. **STRH 的常数是 4，不是 2**（`arm-new.h` 所有 STRH case 都是 `clockTicks += 4 +
   CPUUpdateTicksAccess16(address)`，而 STR/STRB 是 `2 + Access`）。3.2.1.3 的表里
   写的是对的，实现时按"统一 +2"抄错，块级差分立刻报 `cycles interp=25 block=23`。
   现在 `doLoadStore()` 按 `MEM_HALF ? 4 : 2` 分开。

2. **🔴 3.2.1.4 的 Thumb MUL 那句话写反了。** 原文说"V7 Thumb 用目标寄存器旧值，
   ARM 用 Rs"，实际相反：
   - V7 Thumb（`thumb.h` 的 `case 0x43: case 0x01`）：`u32 rm = reg[(opcode >> 3) & 7].I;`
     ——**Rs**，乘的是 `reg[dest].I * rm`（被乘数是 Rd 旧值，但**分档看 Rs**）。
   - V8 Thumb（`GBA-thumb.cpp` 的 `thumb43_1`）：`u32 rm = reg[dest].I;` ——**Rd 旧值**。
   也就是说"分档源"这条 V7/V8 恰好互换。差分用例里 `mul` 的周期差就是从这里来的。
   本轮实现按 V7 走（`doMul()` 的 `magSrc = rs`）。

3. **ARM 寄存器移位的 raw 计数 ≥ 32 是未定义行为，必须回退。**
   `LOGICAL_LSL_REG` / `LOGICAL_LSR_REG` / `LOGICAL_ASR_REG` 直接
   `v << shift`、`v >> (shift - 1)`，`shift = reg[Rs].B.B0`（0..255，没有掩码）。
   这是 UB：本机这版解释器给出的结果既不是"移位 32 得 0"，也不是"掩码到 5 位"，
   而是编译器/宿主做什么就是什么，**不可移植也不该猜**。所以这几类指令一律
   `JIT_NEED_INTERP`（`shiftNeedsInterp()`）。真实代码不会用 ≥32 的计数，代价为零。
   Thumb 不受影响：`thumb40_2/40_3/41_0` 把 32 及以上的分支写清楚了，可以精确建模。
   （这也是差分第一轮 10 处 `alu` 全部失败的原因。）

4. **`ldrb/ldrh/ldrsb/ldrsh pc` 不是一次跳转。** 只有**字** `LDR` 到 PC 会 `+2` 周期、
   把地址对齐到 4 字节并跳过去。字节/半字加载写 `reg[15]` 之后，**下一条指令仍然取自
   `pc+4`**，只有再下一条才跟着新的 `reg[15]` 走。块表达不了这种"绕一条"，`d == 15`
   且非字宽度 → `JIT_NEED_INTERP`。

5. **🔴 任何写 `reg[15]` 的指令都是 PC 写，哪怕它不是分支。** V7 的循环是
   `armNextPC = reg[15].I; reg[15].I += 4/2;`——下一条地址来自 `reg[15]`。所以
   `str r0, [r15, #k]!`（基址是 PC 且回写）、`stm r15!, {...}`、`mrs r15, cpsr`
   这些都会改控制流：它们执行完，**下一条仍从 `pc+4` 取**（`armNextPC` 已经定好），
   但再下一条就跳到被写坏的值上。
   处理办法是通用的一条不变式，而不是逐 op 枚举：
   - 不变式：块内任意时刻必须满足 `reg[15] == 下一条要执行的地址 + 4`（ARM）/`+ 2`（Thumb）。
   - 块运行器每条之后检查它；不成立就**停在原地、保留现场**（不写 `armNextPC`/`reg[15]`）。
   - `jitExecute()` 进块之前再查一次；不成立就先让内联解释器跑一条。
   两个检查接起来，正好复刻解释器的"绕一条"：解释器那条指令把 `armNextPC` 设成 `pc+4`、
   `reg[15]` 被写坏，下一次迭代取 `pc+4`、再把 `armNextPC` 设成被写坏的值；JIT 在这两
   步之间各让出一条指令，状态逐位相同。
   这是差分块级 pass 里 `r15 interp=07E8A2A0 block=080002A0` 那一类失败的根因。

6. **Thumb 的 LDM/STM 没有 `+2` 基数。** `clockTicks += 2` 是 ARM 的 `LDM`/`STM` 共有的
   前导；`thumbC0`/`thumbC8` 只有逐寄存器的 `+1`/`+2`。

7. **`armExecuteOne()` 用 void 包一层，不改头文件。** 3.2.1.10 建议把两个 BKPT 的
   `return;` 改成 `return clockTicks;`，但同一份 `arm-new.h`/`thumb.h` 还要在 `CPULoop`
   里以 void 上下文展开，那样改会编不过。实际做法是：
   ```c
   static int refTicks;
   static void armExecuteOneBody() { int clockTicks = 0; #include "arm-new.h"
                                     refTicks = clockTicks; }
   int armExecuteOne() { refTicks = 0; armExecuteOneBody(); return refTicks; }
   ```
   头文件一个字不用动，BKPT 那条 `return;` 仍然合法（早退时 `refTicks` 保持 0，
   验证器本来也跳过 BKPT）。

8. **V8 有同类的块级 bug（既有，不是本轮引入）。** V8 的 `-jitverify` 块级 pass 在
   `mario.gba` 上稳定报 7 处，签名全是 `r15 interp=... block=...`，块里都有
   `stm r15!, {...}` 或 `str rX, [r15, -rY lsl #k]!`。原因和第 5 条一样：块运行器
   继续顺序执行，解释器却跟着被写回的 `reg[15]` 绕一条。V8 的块级 pass 是这次才真正
   在 `mario.gba` 上跑起来的（之前只有单条 pass + 别的 ROM），所以这条一直没被发现。
   修法可以照第 5 条照搬，但那是独立一笔，别混进 V7。
   **已修**（`75f0057`）：块停下、`reg[15]` 原样留着，`jitExecute()` 用 `jitDetour`
   数两个解释器回合才允许再从 `armNextPC` 解码。

9. **后索引的 `ldr pc, [...]` 也是"绕一条"，而且更隐蔽。** `arm-new.h` 里
   `CASE_16(0x430)` / `0x4b0`（`ldr pc, [rn], #imm` 且 W=1 的形态，注释写作 LDRT）的
   函数体**没有** `dest == 15` 那一段，于是 `reg[15]` 就停在加载值上——既不对齐、也不 +4；
   只有 W=0 的 `0x410` / `0x490` 才会跳。解码器不保留原始 W 位（后索引一律回写），
   所以现在**所有后索引的字加载到 PC** 都回退解释器。
   差分签名：`BLOCK MISMATCH 0800698C count=2 reg` / `r15 interp=02000350 block=02000354`,
   指令是 `ldeq pc, [r8], #-3061`（`aos.gba`）。单条 pass 一直没抓到它，因为它在 ARM 段
   第 6756 条，而当时 `N=3000`——**`-jitverify` 的 N 要按 ROM 覆盖到最远的代码**。

10. **🔴 验证器要快照的不只是 RAM。** 块级 pass 的顺序是"先跑解释器、再跑块"，只还原
    workRAM/IWRAM 时，块里对 palette/VRAM/OAM 的写会把结果留给第二趟。`mario.gba` 的
    `08000E3A strh r1, [r0]`（`r0 = 0x0579C063`，落在 palette）和同一块的
    `08000E24 ldrh r3, [r2]` 命中同一个半字，于是第二趟读到第一趟写的值，报出
    `r1/r2 interp=FF0000FE block=FDFFFFFF` 这种"一条 store 改了两个寄存器"的假象
    （store 不可能改寄存器，这就是识破它的线索；真正的原因是 palette 奇数地址读还会
    走 `GBAMemory.cpp` 的 `value = (value >> 8) | (value << 24)` 旋转）。
    现在 `vtSnapshot` 覆盖 workRAM + internalRAM + palette + VRAM + OAM + ioMem，
    `memMatches()` 一并比较。IO 也放进去是因为少数 IO **读**会写（DMA hack、lag counter）。
    两条排查经验一起记下来：`localizeBlockMismatch()` 原来用 `seedCpu(..., 0)` 而不是
    失败那次的 `off`，于是定位到的往往是**另一条**指令；现在它直接复用
    `compareBlockOnce()` 的前缀，seed、快照、"解释器跑出块"的判断全部一致。

11. **`ticks <= 0` 判错伤了合法周期——这是 HashTrace 第 11 帧就分叉的根因。**
    开着 WAITCNT 的 prefetch 位时 `GBA.cpp` 会把 `memoryWaitFetch[8]--`（可以到 0），
    而 `thumbCycles[opcode >> 8]` 本身就有 0，所以一条 Thumb 指令的合法周期可以是 **0**。
    `runBlock()` 原来把 `<= 0` 当成"IR 没实现"，每次补 `1 + wait`；12000 帧里累计
    **279 036** 次，时钟整体漂移，IRQ/定时器随之错位。改成 `< 0` 之后 `jit.log` 的
    `bad` 归零，3000/12000 帧的逐帧哈希立刻与解释器逐帧一致。
    教训：块运行器里"IR 没实现"的判据必须是**负**返回值，`0` 是有意义的周期数。

12. **V8 的块级 pass 一跑起来就抓到三处偏差（`75f0057`）。** 都不是 V7 那种"绕一条"，
    是纯记账，单条 pass 一条都抓不到：
    - `thumb48`/`thumb98` 的解释器用 `CPUReadMemoryQuick`（绕过 Lua READ 钩子），
      解码器却只给 V7 置了 `JF_QUICK`，于是脚本一开，V8 JIT 每条 PC/SP 相对字加载
      多回调一次；
    - Thumb 高寄存器写 PC 的周期式子是 `codeTicksAccessSeq16(armNextPC) * 2` ——
      **调用一次再乘二**。IR 里写成调用两次，`busPrefetchCount` 的副作用跟着算两遍，
      块内第二条指令最多多收 8 个周期（`aos.gba` 的 `08002B6E` 块，`interp=226
      block=234`）；
    - `runBlock()` 在 `holdState` / 事件边界 / 指令集切换处停下时，`jitRunBlockForTest()`
      的调用方看不出来，验证器会拿"跑了一半的块"和"跑完的解释器"对比。
    同时 V8 的块级 pass 也换成和 V7 同一套区域快照（第 10 条那个问题），并加了
    "两次尝试必须互相一致"的可复现性过滤 —— 块里有 IO 写时，验证器自己的副作用
    （触发 DMA、改中断标志）会让第二趟跑出不同的周期数。

#### 3.2.1.15 V7 实测记录（本机，2026-11-03）

**环境**：`win32\x64\V72XR\VBA_VS_Latest_V72XR.exe`，工程同参数（`clv7.rsp` / `lnkv7.rsp`，
x64 Release，`/GL`）；ROM 是 `win32\x64\V72XR\roms\{mario,sonic,aos}.gba`。

**差分验证**（`-jitverify N`，每趟最多 N 条/块；四趟全跑）。下表是 2026-11-03 夜最终一轮
（第 9-11 条修完之后）的原始输出；每次运行都会覆盖写 `win32/x64/V72XR/jitverify.log`，
表里的数字就是从那里抄的：

| ROM | N | ARM 单条 | Thumb 单条 | ARM 块级 | Thumb 块级 | 结论 |
|---|---|---|---|---|---|---|
| mario.gba | 20000 | 20000 / 0 failed（30196 跳过，6376 解码拒绝） | 20000 / 0（2092 跳过） | 20000 blocks / 43583 insn / 0 failed（92407 跳过，13697 拒绝） | 20000 blocks / 393700 insn / 0 failed（3322 跳过） | 0 差异 |
| sonic.gba | 20000 | 20000 / 0（29105 跳过，6989 拒绝） | 20000 / 0（2508 跳过，36 拒绝） | 20000 / 48055 insn / 0（50422 跳过，9332 拒绝） | 20000 / 209537 insn / 0（1340 跳过，36 拒绝） | 0 差异 |
| aos.gba | 20000 | 20000 / 0（25522 跳过，8271 拒绝） | 20000 / 0（2435 跳过） | 20000 / 53331 insn / 0（43198 跳过，11915 拒绝） | 20000 / 151584 insn / 0（944 跳过） | 0 差异 |

（"跳过"= 单条 pass 里不比较的 store/SWI/MSR，或块级 pass 里整块含 SWI/UNDEF/MSR、
以及解释器侧跑出块的"绕一条"；"拒绝"= 解码器整块不接。两者都是安全的回退，不是失败。）

**端到端 HashTrace**（3.2.1.10 第 3 层，`aos.gba` + `movies/Fk-castlevaniaariaofsorrow-newwarpless.vbm`）：

| 帧数 | 命令 | 结果 |
|---|---|---|
| 3000 | `-play <vbm> 1 -hashtrace trace_off.csv -nojit -quitAfter 3000` / `-jit` | 两列 `ram` 哈希**逐帧完全一致** |
| 12000 | 同上，`-quitAfter 12000` | **逐帧完全一致**（12000 行，0 处不同） |

`-quitAfter` 走 `systemCounters.frameCount == quitAfterTime`，是按帧号的硬边界，所以两次运行
的帧数严格相同，不需要"比较公共前缀"。这 12000 帧覆盖了 DMA、定时器、中断、VRAM/palette
写入和一次页面失效风暴，是"块路径与解释器等价"目前最强的证据。

**块统计**（`jit.log`，上面那次 12000 帧的 `-jit` 运行）：

| 指标 | 数值 |
|---|---|
| 解码块数 | 15 642 361 |
| 入块次数 | 105 553 212 |
| 缓存命中 | 89 910 851（**85.2%**） |
| 页失效 | 1 729 006 |
| 块内指令数 | 644 420 832 |
| `bad`（IR 返回负周期） | **0** |

命中率 85% 当时被认为说明 1024 项直接映射缓存太小（每帧解码 1300 次、页失效 144 次）。
3.2.1.16 的实测否掉了这个推论：扩到 4096 只省 1.2% 的时间。它不影响正确性。

**没测的**：性能——上面这些运行都受 60 fps 节流（各 202 s），只能证明等价。基线单独测，
见 3.2.1.16。

#### 3.2.1.16 性能基线（2026-11-03 夜，两个核都测）

**机器**：i7-13700KF（16C/24T），Windows 11；两个 exe 都是 x64 Release（`/GL`），
和 3.2.1.15 同一份构建。

**方法**：同一个录像、同一帧数，`-jit` 与 `-nojit` 各跑一遍，比墙钟时间。关键是
**`-synchronize 0`**：默认的 60 fps 节流会把两次运行都钉在同一个时长上（之前 12000 帧
各 202 s），什么都量不出来。关掉同步后 `systemDrawScreen()` 既不 `Sleep()`
（`WinSystem.cpp:891` 那个分支要求 `synchronize`），DirectSound 也不按播放位置阻塞
（`DirectSound.cpp:452` 同样的条件），剩下的就是 emulation + 渲染 + 声音，两边完全相同。

```
VBA_VS_Latest_V72XR.exe -rom roms\mario.gba -play <vbm> 1 -synchronize 0 (-jit|-nojit) -quitAfter 6000
```

每个组合 3 次、交替顺序（避免总是同一模式先跑），取**最快**一次——CPU 基准里最快的一趟
受系统干扰最小，中位数反而被偶尔的调度抖动拖高。脚本：`~/.workbuddy/scratch/bench-jit.ps1`
（结果 CSV 同目录 `bench-*.csv`）。

| 核心 | ROM | 帧数 | 解释器 | JIT | 时间比 |
|---|---|---|---|---|---|
| V7 | mario | 6000 | 833 fps（7.20 s） | 721 fps（8.33 s） | **1.16x 慢** |
| V7 | sonic | 4000 | 907 fps（4.41 s） | 787 fps（5.08 s） | 1.15x 慢 |
| V7 | aos | 4000 | 855 fps（4.68 s） | 768 fps（5.21 s） | 1.11x 慢 |
| V8 | mario | 6000 | 917 fps（6.54 s） | 619 fps（9.69 s） | **1.48x 慢** |
| V8 | sonic | 4000 | 1194 fps（3.35 s） | 901 fps（4.44 s） | 1.33x 慢 |
| V8 | aos | 4000 | 1000 fps（4.00 s） | 791 fps（5.06 s） | 1.27x 慢 |

换算成每帧开销（墙钟差 / 帧数）：V8 mario 每帧多 0.53 ms（+48%），V7 mario 只多 0.19 ms
（+16%）。V8 吃亏更多，因为它的块运行器每条指令还要维护 `cpuTotalTicks`、
`busPrefetchCount` 和预取队列，而 V7 那边这三样都不存在。

**缓存容量不是瓶颈**（这条推翻了 3.2.1.13 第 2 条的猜测）：把 `JIT_CACHE_BLOCKS` 从
1024 提到 4096 再测 V8 + mario，解码次数 6 817 059 → 4 438 719（命中率 92.5% → 95.1%），
时间只从 9.81 s 掉到 9.69 s（**1.2%**）。按差值算一次解码约 50 ns，而一个块（平均 6.2
条指令）执行约 100 ns —— 解码被稀释掉了。继续扩到 8192 只会多花 4 MB BSS 换不到半个
百分点，**已还原成 1024**。

**块统计**（每次 `-jit` 运行结束时写 `jit.log`；命中率 = hits / (hits + decoded)）：

| 核心/ROM | 解码块 | 入块次数 | 命中率 | 页失效 | 块内指令 | 平均块长 |
|---|---|---|---|---|---|---|
| V7 mario | 8 597 318 | 86 317 093 | 90.0% | 59 420 | 454 882 201 | 5.3 |
| V7 sonic | 5 547 381 | 38 354 480 | 85.5% | 5 176 825 | 200 385 700 | 5.2 |
| V7 aos | 4 177 640 | 30 329 452 | 86.2% | 529 614 | 189 043 063 | 6.2 |
| V8 mario | 6 817 059 | 90 403 273 | 92.5% | 47 544 | 557 058 998 | 6.2 |
| V8 sonic | 2 384 139 | 25 487 411 | 90.6% | 2 893 189 | 151 319 779 | 5.9 |
| V8 aos | 2 676 277 | 22 655 120 | 88.2% | 401 654 | 163 912 917 | 7.2 |

`bad` 全为 0。注意 sonic 的页失效量级（5.18 M / 4000 帧 ≈ 1294 次/帧）：它的 IWRAM 里
既有代码又有频繁写的栈和变量，一次写就把那一页的版本推高，页上的块每帧要重解码好几遍，
这解释了 sonic 的命中率为什么最低。按上面的单价，这部分仍然只有几毫秒；真贵的是一次
`jitResetCache()`（读档、切 ROM）之后的冷缓存，几万个块每个都要重解码一遍。

**结论**：当前状态是"功能完整、逐帧等价、但慢 1.1–1.5 倍"，与 3.2.1.13 的预期一致——
后端还是 IR 解释器，一层函数调用不可能比内联的 switch 解释器快。真正的加速要等
machine-code 后端（3.2.1.11 第 9 步）；在那之前可以做的只有"把差距压小"（把
`jitInterpInsn()` 挪进头文件让 `runBlock()` 内联、按 op 拆成直接函数），但那充其量
把 1.5x 拉回 1.0x 附近，**不会变成加速**，所以优先级低于后端。

### 3.3 周期记账（确定性红线）

块内每条 guest 指令翻译成：

```
; 1) 指令本体（内联，操作宿主寄存器里的 GBA 寄存器）
; 2) 周期
mov  eax, [busPrefetchCount]        ; 或编译期已知时直接常数
... 与 codeTicksAccessSeq32 等价的 6-10 条指令，区域号是编译期常数 ...
add  [cpuTotalTicks], ecx
; 3) 预算检查
cmp  [cpuTotalTicks], [cpuNextEvent]
jge  .exit_block
```

第 2 步是唯一"丑"的地方。**第一版允许直接 `call codeTicksAccessSeq32`**——它本来就是
`inline`，编出来只有十几条指令，调用它比解释器的"间接调用 + 全套全局往返"仍便宜。
第二版再针对常见区域（ROM 0x08-0x0D、EWRAM、IWRAM）做编译期常量化。

> **禁止**：为了让周期变常数而"调整"时序。宁可慢 10%，也不能让 IRQ 早/晚一条指令。

#### 块边界的 `busPrefetch` / `cpuPrefetch` 契约

这三样都是**跨指令累积**的全局量，块不能"批处理"：

| 量 | 谁在改 | 块后端必须做什么 |
|---|---|---|
| `busPrefetch` / `busPrefetchCount` | `codeTicksAccess*` / `dataTicksAccess*` 的副作用 | 每条 guest 指令仍然调用同一批函数；`jitInterpInsn()` 逐条照抄循环前导那三行（`0x08020000` 补丁、`busPrefetch = false`、`busPrefetchCount & 0xFFFFFE00` 则抬成 `0x100 \| (x & 0xFF)`）。**不许合并、不许缓存、不许省** |
| `cpuPrefetch[0..1]` | 分支/写 PC 之后的 `ARM_PREFETCH` / `THUMB_PREFETCH` | **块出口重填**；块入口不用管——块是从内存重新解码的，不读解释器手里的预取字 |
| `armNextPC` / `reg[15]` | 同上 | 块出口设成"下一条要执行的指令地址"和它的 +4/+2，与一条分支 handler 留下的状态完全一致 |

块出口那五行（`JitCache.cpp` 的 `runBlock()` 末尾）：

```c
armNextPC = pc;
if (armState) { reg[15].I = armNextPC + 4; ARM_PREFETCH; }   // 读 armNextPC 与 +4
else          { reg[15].I = armNextPC + 2; THUMB_PREFETCH; }
```

出口必须重填，不能"信任上一条指令留下的值"：块可能因为
`cpuTotalTicks >= cpuNextEvent`、`SWITicks`、`holdState`、指令集切换这四种原因提前结束，
下一条要交给解释器，而解释器的循环入口直接把 `cpuPrefetch[0]` 当作要执行的那条指令。
不重填的话，页失效之后的第一次回退会执行**上一条指令**。两次 `CPUReadMemoryQuick`
换掉这类 bug，值得。

### 3.4 标志位

解释器把 N/Z/C/V 存在 4 个 `bool8` 全局里，块内改成宿主寄存器里的一个 32 位字
（ARM CPSR 的 bit 31/30/29/28 位置，方便 `MSR`/`MRS` 直出）：

- 块入口：`mov  rflags, [packedFlags]`
- 块内：ALU 用 `sets`/`setc` 直接落位，或按解释器公式算完再 `or/and` 进 `rflags`
- 块出口：`mov [packedFlags], rflags`
- 任何调用 `CPUUpdateCPSR` / `CPUUpdateFlags` / `CPUSwitchMode` 的指令（`MSR`、`SWI`、
  异常、`LDM` 到 r15）→ **先物化再调用**，然后块结束（这些路径一律回解释器最省事）

第一版可以更保守：**每条指令结束都把标志写回全局**。这样慢，但正确性风险接近零，
先跑通再优化。

### 3.5 内存访问

三档，按阶段上：

| 档 | 做法 | 适用 |
|---|---|---|
| A | 直接 `call CPUReadMemory/CPUWriteMemory`（带 Lua 钩子的包装） | Phase 2 第一版，绝对正确 |
| B | 编译期判断"本次会话没有 Lua 内存钩子"→ 改调 `CPUReadMemoryWrapped` | Phase 3 |
| C | 内联快路径：`if ((addr>>24)==2) { v = *(u32*)(workRAM + (addr & 0x3FFFC)); ticks += const; }` 否则落回 helper | Phase 3 |

C 档是性能大头，但要**逐条对齐** `dataTicksAccess32` 里 `busPrefetch` 的副作用，
包括那个 `busPrefetchCount = ((busPrefetchCount+1) << waitState) - 1`。

### 3.6 失效（自修改代码）

GBA 上代码可以住在 ROM（只读）、BIOS（只读）、EWRAM、IWRAM。前两者不用管，后两者必须管。

已落地（`src/gba/jit/JitCache.cpp`）：

- **块缓存**：直接映射 1024 项，索引 `((pc >> 2) ^ (pc >> 12)) & 1023`，tag 是 `pc | thumb`。
  一项一个 `JitBlock`（1032 B）⇒ 约 1 MB BSS。未命中就覆盖旧块，没有分配器、没有淘汰策略。
- **页版本**：每 1 KB 一页，EWRAM 256 页 + IWRAM 32 页 = `jitPageVersion[288]`，另有一张
  `jitPageHasCode[288]`。块记住自己起始页的版本，**入块时比对一次**。
- **只有这一页曾经编译过块，写它才 bump 版本**：`jitPageHasCode[page]` 为 0 时写数据只花
  一次字节 load + 不跳。这是"游戏反复写 EWRAM 数据"这种情况下必须付的代价。
- **钩子位置**：`CPUWriteMemory` / `CPUWriteHalfWord` / `CPUWriteByte`（外层包装，与 Lua
  WRITE 钩子并列），先判 `(address & 0x0E000000) == 0x02000000` 一次覆盖 EWRAM+IWRAM，
  其余区域直接跳过。DMA（`doDMA`）和 BIOS 的 `CpuSet`/`LZ77`/`Huffman` 等功能都走这三个
  函数，所以都被覆盖。
- **跨页写**：4 字节写可能横跨两页，`jitCodeWritten()` 会检查 `address + size - 1` 所在页。
- **整表作废**：`CPUReset()`、读档成功（`CPUReadStateFromStream()`）、`jitSetEnabled(true)`、
  `BIOS_RegisterRamReset()`（清 work/internal RAM 时）、`BIOS_SoftReset()`（重启到
  `0x02000000`/`0x08000000`）。后两个用 `memset` 直接清 RAM、绕过写钩子，必须手工清缓存。

不做页写保护的 `VirtualProtect` 方案：Windows 上每次异常处理几十微秒，GBA 游戏写 EWRAM
极其频繁，会直接崩掉性能。

### 3.7 和现有特性的交互（必须写进代码注释）

| 特性 | 实际行为（`JitCache.cpp`） |
|---|---|
| Lua EXEC 钩子 | `luaMemHookActive[LUAMEMHOOK_EXEC]` 非 0 ⇒ `jitExecute()` **每次调用都退回解释器**（钩子可以在运行中装/卸，不能按块缓存这个判断） |
| Lua READ/WRITE 钩子 | 不受影响：块内访存走的是 `CPUReadMemory` / `CPUWriteMemory` 包装，钩子照常触发 |
| 调试器 / 断点 | `systemDebug != 0` ⇒ 退回解释器 |
| 金手指 | `mastercode` 非 0 时，块内**每条指令前**都比一次 `mastercode == pc`，命中就调 `CPUMasterCodeCheck()`（与解释器循环同一位置）；`mastercode` 变化不需要清缓存，因为判断是逐条的 |
| 存档 | JIT 状态不入存档；读档成功后 `jitResetCache()` |
| BIOS `RegisterRamReset` / `SoftReset` | 用 `memset` 清 RAM 的那两条路径显式 `jitResetCache()`（见 3.6） |
| 电影 / TAS | 语义上无影响，但**必须**通过 HashTrace 逐帧比对（第十节第 3 层；当前还有一处未结差异，见那里） |
| 加速 / 跳帧 | 无影响 |
| V7 核心 | 已接上（2026-11-03）：`jitAvailable()` 在 V7 构建里返回 `true`，菜单项可用，`-jit` / `-nojit` 生效。V7 的块运行器在 `CPULoop` 里替换的是内联解释器那一步，返回本次周期数；遇到它不肯建模的指令（IO 写、SWI、UNDEF、写 CPSR 的 MSR、写 PC 的 S=1 运算）就把那一条交回内联解释器，见 3.2.1.9 / 3.2.1.14 |
| Win32 (x86) | IR 解释器可用（V7/V8 都是）；机器码后端仍然只做 x64 + ARM64 |
| 开关 | 菜单项 + `vba.ini` 的 `jit` 键 + 命令行 `-jit` / `-nojit`（见 3.8），默认关闭 |

### 3.8 开关（2026-11-03 已接入）

| 入口 | 行为 |
|---|---|
| `Options > Emulation > CPU Recompiler (JIT)` | 复选框，勾上即 `jitSetEnabled(true)`；`vba.ini` 的 `jit=1` 记录状态 |
| `-jit` | 本次运行强制打开，覆盖 ini 里的值 |
| `-nojit` | 本次运行强制关闭，用于在 `jit=1` 的配置下跑 HashTrace 的解释器基线 |
| 默认 | 关闭。机器码后端落地之前打开只会更慢，所以不做默认开启 |

实现上有几个约定：

- 命令行的 `-jit` / `-nojit` 先记下来，**整条命令行读完再应用**。否则 `vba.exe -jit game.gba`
  这种"裸 ROM 参数"会让 `loadSettings()` 再跑一次，把 `-jit` 覆盖回 ini 里的值。
- 切换开关调用 `jitSetEnabled()`，它会清空块缓存（`jitResetCache()`）。因此运行中切换没有
  半编译状态：下一条指令要么整块重新解码，要么直接走解释器。
- 菜单项按 `jitAvailable()` 灰显；V7 构建自 2026-11-03 起也可用（IR 解释器，见 3.2.1.13）。
  电影录制/回放期间和其它改变模拟行为的选项一样，需要按住 Ctrl 才能切换，
  避免回放中途改变执行路径。
- 存档、HashTrace、电影格式**都不记录**这个开关。它只影响 CPU 走哪条执行路径；同一份输入
  在同一设置下必须得到同一份逐帧哈希，这正是第十节第 3 层要验收的东西。
- 设置保存在 `vba.ini`（注册表模式则是对应键）的 `jit` 下；`-jit` / `-nojit` 也会在正常
  退出时经 `saveSettings()` 写回，这一点和 `-frameskip` 等命令行参数一致。要临时对比两种
  路径，两个方向都显式传参最稳。

---

## 四、AOT 导出

### 4.1 定义清楚

"导出原生游戏" = 生成一个 **不模拟 CPU** 的 Windows 可执行文件：

```
game.exe
 ├─ vba 运行时（PPU 六种模式、APU、DMA、定时器、IRQ、存档、输入、窗口）
 ├─ ROM 数据（嵌入）
 ├─ 预编译的游戏代码（原生函数，按 guest PC 索引）
 └─ 解释器（兜底，只处理没预编译到的 PC）
```

覆盖率是硬指标。多数商业 GBA 游戏完整玩一遍能覆盖 ROM 代码的 95-99%；
剩下的是没走到的分支和 RAM 里生成的代码。

### 4.2 三种产物形态

| 形态 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **AOT-C** | 生成 `.c`，用 `cl.exe` 编成 obj 再链接 | 可读、可调试、零新依赖、复用 C 后端 | 导出时要装 MSVC；编译慢 |
| AOT-blob | 直接生成机器码字节数组塞进 `.rdata` | 导出即用，不需要编译器 | 要处理重定位；不可读 |
| 完全静态 | 静态解析所有间接跳转，去掉解释器 | 产物最小 | **GBA 上不成立**，放弃 |

**先做 AOT-C。** 它和 JIT 共用同一个 IR 和 C 后端，Phase 1 的差分测试已经把这条路
验证过了，边际成本最低。

### 4.3 流程（profile-guided）

1. `vba.exe --aot-trace game.gba` 正常玩一遍（或用 `.vbm` 回放），记录每个执行过的块：
   `{pc, thumb, 字节哈希, 执行次数}` → `game.trace`。
2. `vba-aot.exe game.gba game.trace`：
   - 按 PC 从 ROM 重新解码，校验字节哈希（ROM 被改过就丢弃该块）
   - 走 IR → C 后端 → `game_blocks.c`（每块一个 `static void blk_XXXXXXXX(Cpu*)`）
   - 生成按 PC 排序的派发表
3. `cl` 链接 `game_blocks.c` + `vbaruntime.lib` + 嵌入的 ROM → `game.exe`。
4. 运行时未命中 → 解释器，并在退出时打印覆盖率报告。

### 4.4 AOT 的真实难点

- **RAM 代码**：不少游戏把热点循环拷到 IWRAM 再跑（为了零等待）。这类代码在 AOT 里
  永远走解释器，除非做"运行时二次 JIT"——那 AOT 就退化成"JIT + 预热的块缓存"了。
- **BIOS**：SWI 应该做成 HLE 运行时函数（`Cpu*` 上直接实现），不要真去执行 BIOS 镜像。
- **时序**：AOT 必须复用同一套周期模型，否则同一个输入在 exe 和模拟器里结果不同。
- **确定性**：AOT 产物是否要和 `.vbm` 逐帧一致？建议**要求一致**，否则调试成本翻倍。

### 4.5 便宜的中间态：块缓存落盘

在 Phase 3 之后顺手做：把编译好的块按 `ROM 哈希 + 块 PC` 序列化到 `game.vbajit`。
下次启动直接映射，省掉首帧的编译抖动。这一步几乎就是 AOT 的雏形，
也是验证"块序列化"这条链路的最便宜方式。

---

## 五、依赖

### 5.1 关于 `armjit`

**不存在一个叫 `armjit` 的通用 ARM JIT 库。** 能拿来参考/移植的只有：

| 来源 | 许可 | 目标架构 | 评价 |
|---|---|---|---|
| mGBA `src/arm/` | MPL-2.0 | **仅 ARM32 / AArch64 宿主** | 成熟，但绑死 mGBA 的内存模型和 CPU 结构；对 Windows x64 主力目标**没用** |
| VBA-Next `dynarec/` | GPL-2.0 | x86-32 | 已停更十年，32 位，不能直接用在 x64 |
| Dolphin / PPSSPP 的 JIT | GPL-2.0 / GPL-2.0+ | x64 / ARM64 | 是 PowerPC / MIPS 语义，只能借结构，不能借代码 |

结论：**移植不划算，自己写。**

### 5.2 机器码编码器（真正需要的依赖）

| 库 | 许可 | 架构 | 集成成本 | 评价 |
|---|---|---|---|---|
| **asmjit** | Zlib | x86/x64 + AArch64 | 中（~60 个 .cpp 进 vcxproj） | 一套 API 覆盖两个目标平台，编码/重定位/标签都由它管。**推荐** |
| Xbyak | BSD-3 | x86/x64 | 低（单头文件） | 最省事，但 ARM64 要另写一套 |
| SLJIT | BSD-2 | x86/x64/ARM/ARM64/… | 中 | 栈式 API，不适合手排指令，用在正则引擎那种场景更合适 |
| DynASM | MIT | 多架构 | 中 | 需要预处理步骤，和 MSVC 工程集成别扭 |
| 手写编码器 | — | 自己定 | x64 ~1500 行 / ARM64 ~1200 行 | 零依赖，完全可控；调试期长一点 |

**推荐 asmjit**，理由：许可证（Zlib）与 GPL-2.0 兼容；一个 API 同时解决 x64 和 ARM64；
`JitRuntime` 自带可执行内存分配和 W^X 处理，省掉 `VirtualAlloc`/`VirtualProtect` 那套平台代码。
如果坚持零第三方依赖，就手写——IR 层已经把选择隔离掉了，换编码器不影响其它模块。

> 已克隆在 `C:\Users\zhang\asmjit`：版本 1.24.0，头文件根目录是 `C:\Users\zhang\asmjit`
> （`#include <asmjit/a64.h>`、`<asmjit/x86.h>`），`asmjit/core/` 下有 38 个 `.cpp` 要加进工程。
> `a64.h` 在，AArch64 后端可用。

> ⚠️ 许可证核对项：本项目 `COPYING` 是 **GPL v2**（不是 "or later"）。Zlib / BSD / MIT
> 都兼容。若将来想移植 mGBA 的 MPL-2.0 代码，需要单独确认 "GPLv2-only + MPL-2.0"
> 的组合，这一步别拍脑袋。

### 5.3 不需要的

- 不需要 LLVM / libgccjit（体积、构建时间、MSVC 集成都不划算）。
- 不需要任何 ARM 汇编器（keystone/capstone 是反汇编器，方向反了）。
- 不需要新的构建系统。

---

## 六、分阶段方案

每个阶段都有**独立的回退开关**和**验收标准**。任何一阶段不达标就停在那里，不留半成品。

### Phase 0：测量 + 便宜优化（2-4 天）

**改动**
1. `GBA-arm.cpp:2335` / `GBA-thumb.cpp:1711`：把 `CPUReadMemoryQuick` 从实参里拿出来，
   先判 `luaMemHookActive(LUAMEMHOOK_EXEC)` 再读。在 `vbalua.h` 加一个 inline 查询。
2. `CPULoop` 加 `#ifdef VBA_PROFILE_FRAME` 分段计时，输出 `vba-frame-prof.log`。
3. 顺手把 `CPUMasterCodeCheck()` 在 `mastercode == 0` 时短路（现在每次调用都要进函数）。

**验收**：`HashTrace` 前后 CSV 逐字节相同；profile 里 CPU 占比有数。

**预期**：3-8%。这一步的产出是"到底值不值得做 JIT"的判决书。

> **2026-11-02 已实施第 1、3 条**（第 2 条未做）：
> `common/vbalua.h` 新增 `luaMemHookActive[]` 闸门，`common/lua-engine.cpp` 在
> `CalculateMemHookRegions()` 里同步；`GBA-arm.cpp` / `GBA-thumb.cpp` / `V7/GBA.cpp` 的
> 每指令 EXEC 钩子改为先测闸门；`V7`+`V8` 的 `GBAMemory.cpp` 里 6 个读写包装同样处理；
> `armExecute`/`thumbExecute` 的 `CPUMasterCodeCheck()` 改成调用点短路。
> 用 clang -O2 看 `armExecute` 的循环前导：原来每条指令 11 条地址计算 + 1 次 load +
> 1 次 call 的"钩子实参"，现在只剩 `cmp byte [luaMemHookActive+2], 0` + 不跳。
> **预期收益要按实测重估**，可能高于 3-8%（这是每指令固定开销）。

### Phase 1：IR + 解码器 + C 后端（2-3 周）

**文件（`*` = 已落地，其余是计划）**
```
src/gba/jit/JitIr.h           * IR 定义（~150 行）
src/gba/jit/JitDecodeArm.cpp  * ARM 解码 → IR
src/gba/jit/JitDecodeThumb.cpp* Thumb 解码 → IR
src/gba/jit/JitInterp.cpp     * IR 解释器（当前后端，兼作周期规范）
src/gba/jit/JitCache.cpp      * 块缓存 / 页失效 / CPULoop 入口
src/gba/jit/JitVerify.cpp     * 差分测试（-jitverify：解码空间 + 逐指令 + 块级）
src/gba/jit/JitDump.cpp       * IR → 文本
src/gba/jit/selftest.cpp      * 解码自测（宿主测试，不进 vcxproj）
src/gba/jit/JitC.cpp            IR → C（也是 AOT 的后端）
src/gba/jit/JitApi.h            对外接口
src/gba/jit/jit_test.cpp        差分测试驱动（可选，不进 Release）
```

**做法**：先不生成机器码。最初的计划是 `jitExecute` 走"C 后端"路径时把块编成一个可调用的
**函数指针**——但 C 不能运行时编译，所以这一步的正确形态是：**离线把测试 ROM 的块
导出成 C，编进一个测试程序，和解释器逐条比对**。

实际顺序反过来了：`jitExecute()` 先直接跑 IR 解释器（`JitCache.cpp`），把块缓存、失效、
块边界这套机制跑通并验证；`JitC.cpp` 仍未动笔。这样做的好处是换后端时 `CPULoop` 那一侧
一行都不用改。

**验收（硬门槛）**
- 用 jsmolka 的 `gba-tests`（ARM/Thumb 指令测试）、ARMWrestler、AGS aging cartridge，
  逐条指令比对 `reg[0..17]`、`N/Z/C/V`、`cpuTotalTicks`、内存写入序列，**零差异**。
- `HashTrace` 跑 3 个商业 ROM 各 600 帧，CSV 与解释器完全一致。

这一阶段做完，AOT 的语义层就成立了，即使一个机器码都没生成。

> **2026-11-02 第一批落地**（ARM 部分，尚未接入 `CPULoop`）：
>
> | 文件 | 内容 |
> |---|---|
> | `jit/JitIr.h` | IR 定义：13 个 op、ALU/移位/访存/块传送子类型、`JitBlock`（64 条上限）、`JitSource`（`base[addr & mask]`，对齐 `memoryMap[]`） |
> | `jit/JitDecodeArm.cpp` | ARM 全量解码，按 `armInsnTable[]` 的 bits 27-20 / 7-4 分派 |
> | `jit/JitDump.cpp` | IR → 文本，出问题时最快的定位手段 |
> | `jit/JitInterp.cpp` | IR 解释器，兼作周期记账的可执行规范 |
> | `jit/selftest.cpp` | 宿主侧解码测试，**不进 vcxproj**，clang 直接编 |
>
> 自测 256 项全过。过程中抓到两个真 bug，都记在这里免得再犯：
> 1. `read32` 用 `READ32LE` 宏把 `const u8*` 转成 `u32*` 解引用，是 const-cast 别名违规，
>    clang 在 `-O0` 下就把第二条指令读成了 0。改成 `memcpy` 后正常，编译器仍优化成一次 load。
> 2. 寄存器移位的判定位写成了 `opcode & 0x0F`（那是 Rm），应该是 `(opcode >> 4) & 0x0F`。
> 3. `armNextPC` 在 handler 里是 `pc+4` 而不是 `pc`，`reg[15]` 是 `pc+8`。第一版把 `armNextPC` 设成了 `pc`、`reg[15]` 设成了 `pc+4`，`BL` 的返回地址会整体偏 4，所有 `codeTicksAccess*` 也会差一个地址。
>
> 还没做的：Thumb 解码、`JOP_LDM`/`JOP_STM`/`JOP_SWP` 的解释器（解码已经有了，
> 解释器返回 -1 表示未实现）、C 后端、接进 `CPULoop`。
>
> **2026-11-02 第二批落地**（Thumb 全量 + ARM 补齐，仍未接入 `CPULoop`）：
>
> | 文件 | 内容 |
> |---|---|
> | `jit/JitIr.h` | 重新定义 `JitInsn`：`flagMask`（N/Z/C/V 位掩码，取代 `JF_SETFLAGS`）、`JF_IMM`、`JF_PC_ALIGN4`、`JF_CLEAR_PREFETCH`、`JF_DATA32`；新增 `JOP_PUSH`/`JOP_POP`、`SHIFT_ROR_REG_T`；`jitInterpInsn` 增加 `thumb` 参数 |
> | `jit/JitDecodeArm.cpp` | 立即数打 `JF_IMM`；寄存器偏移按 `OFFSET_*` 语义拆 shift；`^` 形式的块传送拒绝解码（整块回解释器）；块在 1 KB 页边界断开 |
> | `jit/JitDecodeThumb.cpp` | 新增。19 种格式全覆盖（移位、加减、imm8、ALU、高寄存器/BX、七种访存、SP/PC 相对、PUSH/POP、LDMIA/STMIA、条件分支、SWI、B、BL 两半） |
> | `jit/JitInterp.cpp` | ARM 补 `LDM/STM/SWP`；Thumb 全语义；标志按 `flagMask` 写；Thumb 的周期记账逐条照 `GBA-thumb.cpp` |
> | `jit/JitVerify.cpp` | 差分测试改成两趟（arm / thumb），并把 IR 返回的周期计入 `cpuTotalTicks` 再比对 |
> | `jit/selftest.cpp` | 993 项断言，ARM + Thumb + 块边界/页边界，`clang++` 直接编 |
> | `jit/JitDump.cpp` | 新 op 与 `JF_IMM` 的打印 |
>
> **这一轮抓到的 4 个 bug，前三个是上一轮留下的、差分测试一跑就会炸的**：
>
> 1. 立即数访存的 `m` 字段留着 0，解释器按 `(m < 16) ? reg[m] : imm` 取值 ⇒ 所有
>    `ldr/str [#imm]` 的偏移都变成了 `r0`。用 `JF_IMM` 解决（`evalOffset` 不再看 `m`）。
> 2. 单数据传送的寄存器偏移判定位写反了。VBA 的表里 **bit4=0 才是立即移位**
>    （`arm600/602/604/606` 对应 LSL/LSR/ASR/ROR 立即数），bit4=1 是 `OFFSET_REG`（无移位）。
>    第一版把立即移位一律当成 ROR，`ldr r0,[r1,r2,lsl #2]` 会变成 RRX。
> 3. 差分测试从来没有把 `jitInterpInsn()` 返回的周期加进 `cpuTotalTicks`，而解释器那条路
>    会累加 ⇒ 只要跑起来，每条非跳过指令都会报 `cycles` 不符。现在两边对齐后再比。
> 4. Thumb 的 `MUL`、`LDMIA`、`STMIA`、`POP` 用的是**赋值**而不是累加 `clockTicks`
>    （`thumb43_1` / `thumbC0` / `thumbC8` / `thumbBC`），数据访问的周期被丢掉；`thumb5A`
>    的半字加载还比立即数形态多算一次 32 位等待。照抄，不"修正"——差分测试比对的是模拟器。
>
> 验证：`selftest` 993 项全过；`JitInterp/JitVerify/JitDecode*/JitDump` 在 MSVC `/W3` 下零警告。
> 端到端仍然要等 `jitExecute` 接进 `CPULoop`（见第十节第 3 层）。
>
> **2026-11-02 第三批**（把差分测试从"绿"推到"穷举绿"，仍未接入 `CPULoop`）：
>
> 新增**解码空间全扫**（`JitVerify.cpp` 的 `sweepDecodeSpace()`）：两张分派表都是少数几位的
> 函数，所以"哪些字是未定义"可以穷举对账，4096 + 1024 个字一次跑完，比随机 ROM 抽样强得多。
> 为此在 `GBA-arm.cpp` / `GBA-thumb.cpp` 的表旁边各加了一个 `armInsnUndefined()` /
> `thumbInsnUndefined()`（只给校验器用，不是解码器的依赖）。
>
> 这一轮修掉的 **9 个 bug**，前 6 个都是"只在特定 ROM 的数据里才撞到"的：
>
> | # | 症状 | 根因 |
> |---|---|---|
> | 1 | `rsbs r2, r0, #0x26` 变成未定义 | 乘法/半字分支没排除 `I=1`（bit25），立即数的 bits 7-4 被当成分派键 |
> | 2 | `ldr/str` 带 bit4=1 被执行 | 表里这些位置是 `arm_UI`，模拟器根本不实现 ARM 手册的"寄存器移位"形式 |
> | 3 | `tst/cmp/cmn` 把 Rd=15 当成写 PC 并切模式 | 模拟器的 ALU 宏对**所有** ALU 指令判 `dest != 15`，但比较类不跑 `CPUSwitchMode`；两者要分开建模 |
> | 4 | `mrs r0, spsr` 之后 CPSR 不一致 | `arm140`（SPSR）不调 `CPUUpdateCPSR()`，只有 `arm100`（CPSR）调 |
> | 5 | 0x08020000 / 0x0801FFFC 处周期差 2 | prefetch 补丁测的是**当前**指令地址，我用了 pc+4 |
> | 6 | `bx lr` 变成未定义 | Thumb format 5 只按 bit7 判 BX/BLX，bits 6-3 是 Rm；我多测了 bit6 |
> | 7 | `swp` 少识别一批 | ARM 手册要求 bits 11-8 = 0，但表里那些位置也指向 SWP，模拟器不看这几位 |
> | 8 | `swi` 吃掉协处理器行 0xDx/0xEx | 判 SWI 用了 bit24，应该判 bits 27-24 == 1111 |
> | 9 | 半字的若干非法组合被执行 | `S=1 && L=0`（"有符号存储"）全表未定义；`H=1,S=0 && P=0` 只在 U=I=1 的两行实现——照抄表的空洞 |
>
> 另外三条是"模拟器表里就写着 `armBreakpoint`"的字（真正的 BKPT、以及 index 0x7FF 那个
> 空洞）：`armBreakpoint()` 会把 PC 退回去、给调试器发信号、`clockTicks = -1`，也就是**停机**。
> 块后端没法建模，解码器改成**整块拒绝**，交回解释器。
>
> 第 9 条的空洞规则是**按 V8 的表**抄的；V7 的 `arm-new.h` 把 LDRH/STRH 的 16 种
> P/U/I/W 全实现了，所以 V7 构建里这条拒绝条件被去掉（见 3.2.1.7 与 3.2.1.12 第 11 条）。
>
> 验证：解码空间 4096+1024 **零分歧**；SMA(J) / Metroid Fusion(U) / 黄金太阳(J) 各
> 20000 条，arm 与 thumb 全绿；`selftest` 1055 项全过。
> 端到端仍然要等 `jitExecute` 接进 `CPULoop`。
>
> **2026-11-02 第四批**（接进 `CPULoop`、块级差分、端到端第一次跑起来）：
>
> | 文件 | 内容 |
> |---|---|
> | `jit/JitCache.{h,cpp}` | 1024 项直接映射块缓存、1 KB 页版本失效、`jitExecute()`（契约与 `armExecute()` 相同）、`jitCodeWritten()`（写钩子）、`jitGetStats()`（退出时向 stderr 打一行）、`jitRunBlockForTest()`（只给块级差分用） |
> | `V8/GBA.cpp` | `CPULoop` 的派发点改调 `jitExecute()`；`CPUReset()` 与读档成功后 `jitResetCache()`；新增 `CPUGetRomSize()`（校验器要按 ROM 实际大小取指） |
> | `V8/GBAMemory.cpp` | 三个写函数挂失效钩子，`jitEnabled && (addr & 0x0E000000) == 0x02000000` 才真的调 |
> | `V8/bios.cpp` | `BIOS_RegisterRamReset()` / `BIOS_SoftReset()` 显式清缓存：它们用 `memset` 清 RAM，绕过写钩子 |
> | `win32/VBA.cpp` | `-jit` / `-nojit` 命令行开关、`vba.ini` 的 `jit` 键；菜单项见 3.8 |
> | `jit/JitVerify.cpp` | 新增**块级差分**（第十节第 2.5 层）；解码空间与 `armInsnUndefined` / `thumbInsnUndefined` 对账 |
>
> **最大的一个 bug，逐指令差分永远测不出来**：`doAlu()` 写 PC 时用**切换前**的 `thumb`
> 决定对齐。`SUBS pc, lr, #4`（`0xE25EF004`）是 ARM 指令、但 SPSR 的 T 位让它返回 Thumb
> 代码；`CPUSwitchMode()` 已把 `armState` 置 false，我们却仍按 ARM 掩到 4 字节 ⇒
> `0x08001D0A` 变成 `0x08001D08`。内置 BIOS（`GBA.cpp` 的 myROM）与真 BIOS 的 SWI
> 返回序列都以这条收尾，于是每条 SWI 都回错 2 字节，几帧后画面停住 + 噪声。修法：写 PC
> 分支改用切换后的 `armState` 决定**对齐**。注意周期要用相反的判据，见下面 2026-11-03
> 那条。逐指令差分测不出来，因为 `seedCpu` 把
> `reg[17]` 设成 `0x1F`（SPSR 没有 T 位），永远不会发生"跨指令集返回"。
>
> 定位手法保留为开关：`JIT_PC_TRACE`（`JitCache.h`，默认 0）+ 环境变量
> `VBA_JIT_PCTRACE=<文件>`，开 JIT 与不开各跑一遍，逐条比 `(pc, lr, sp, cpuTotalTicks)`，
> 第一个分叉点就是罪魁。解释器侧的调用点在 `GBA-arm.cpp` / `GBA-thumb.cpp` 的 `#if 0` 里。
>
> 批量回归：12 个商业 ROM × 20000 条 × 两趟（SMA、Metroid Fusion、黄金太阳、火纹、
> 火红、ZERO、晓月、马车、合金弹头、缩小帽、KOF EX2、Boktai）**全部 0 失败**，
> 解码空间 0 分歧。这一轮还修了一个 `msr` 的真 bug：IR 只实现了 CPSR 形式，
> SPSR 形式（`arm160` / `arm360`）被当成写 CPSR 并错误地切了模式；模拟器对 SPSR 形式
> 既不 `CPUUpdateCPSR` 也不 `CPUUpdateFlags`、更不切模式。
>
> 端到端：`-hashtrace` 已经能跑。JIT 关/开逐帧比对当时还有**一处**未结差异——某次 IRQ
> 比解释器晚投递一条指令（约第 50 万条指令处），见第十节第 3 层；已在第五批修掉。
> 除此之外的不一致都已随上面的 2 字节修复消失，画面与声音恢复正常。
>
> **2026-11-03 第五批**（修掉"IRQ 晚一条"与 `UMULL`/`SMULL` 反了）：
>
> 端到端哈希卡在 `aos.vbm` 第 8 帧，逐条轨迹（`JIT_PC_TRACE`）显示第一次分叉时
> `pc`/`sp`/`cpuTotalTicks` 都相同，只有 `lr` 不同——说明不是事件时刻，而是某条指令
> 写错了值。两处根因：
>
> 1. **`doAlu()` 写 PC 时用错了周期公式**（`JitInterp.cpp`）。写 PC 之后按切换后的
>    `armState` 选对齐是对的，但周期不能跟着新状态走：解释器的 ARM `ALU_INSN` 宏
>    无论切到哪个状态都按 32 位取指记账
>    （`3 + ISREGSHIFT + codeTicksAccess32 + 2 * codeTicksAccessSeq32`）。JIT 只要发现
>    落地是 Thumb 就套了 Thumb 的 16 位公式，于是每条"ARM 指令返回 Thumb"都少 6 个周期；
>    `SUBS pc, lr, #4` 正是内部 BIOS 的 SWI/IRQ 返回序列收尾，累积几帧后 IRQ 就晚投递
>    一条指令。修法：Thumb 的 `MOV/ADD Hd,pc` 仍用 16 位公式，ARM 指令落进 Thumb 用
>    32 位公式——判据是**当前指令的指令集**（`thumb`），不是切换后的 `armState`。
> 2. **`decodeMultiply()` 把 U 位取反了**（`JitDecodeArm.cpp`）。长乘法编码里
>    bit 22 为 0 是 `UMULL`（`0x08xxxxxx`）、为 1 是 `SMULL`（`0x0Cxxxxxx`），和解释器
>    `arm089`/`arm0C9` 的分组一致；JIT 却写成了 `isUnsigned = (opcode & 0x400000) != 0`。
>    于是无符号乘法被当成有符号算，高 32 位全错。`selftest` 里那条
>    `0xE0C10392 -> umull` 的用例也跟着记反了，一并改成 `0xE0810392 -> umull`，
>    并补上 `smull`/`umlal`/`smlal` 三条。
>
> 第一处是**周期记账错**（事件时刻），第二处是**纯数据错**；块级差分能看见，但逐指令
> 差分扫 ROM 头部时不一定踩到（第一处要 `CPUSwitchMode` 跨指令集，第二处要乘数高位为负）。
>
> 验证：`selftest` 1085 项全过；`-jitverify 200000` 逐指令两趟 0 失败、解码空间 0 分歧，
> 块级"失败" 824 → 803（少掉的 21 条正是 ARM 写 PC 的周期）；端到端 `aos.vbm`
> 跑 **3600 帧**（60 秒游戏时间）`ram` 与 `full` 两列**逐字节相同**。



### 为什么 IR 是自己写的，不是 LLVM / GIMPLE

**结论：自定义。** 三层理由，按重要性排：

1. **粒度对不上。** GBA 的块只有 10-50 条 guest 指令。LLVM 建 Module + 验证 + 跑 pass
   是几十微秒级，JIT 的编译开销会盖过执行开销。而且 `LLVM-C.dll` 60-100 MB，
   对一个单 exe 分发的 GPLv2 项目不现实。GIMPLE/RTL 是 GCC 内部结构，没有对外 ABI，
   根本拿不到。
2. **语义装不进去。** `reg[15]` 执行期等于 PC+4、写 PC 时抑制标志更新、`busPrefetchCount`
   被指令本身改写、7 组 bank 寄存器——这些在标准 IR 里只能退化成"调 helper"，
   那样 IR 就只剩包装壳，付了体积代价换回一串函数调用。
3. **AOT 要出 C。** 自定义 IR 直接吐 `.c`，可读、可断点、复用同一个后端。
   LLVM 只能吐 `.obj`，调试和差分测试都更贵。

后路留着：将来 AOT 若发现生成代码慢，可以**离线**把自定义 IR 翻成 LLVM IR 编译，
不参与 JIT 热路径。参考对象：mGBA 的 JIT 连 IR 都没有（直接发射机器码），
Dolphin 是自定义 IR + 多后端——"一层 IR + 多后端"是这类项目的标准做法。

### Phase 2：x64 后端（3-4 周）

**新增**
```
src/gba/jit/JitX64.cpp       IR → x64（asmjit 或手写）
src/gba/jit/JitBlock.cpp     块缓存、查表、失效
src/gba/jit/JitRuntime.cpp   从生成代码回调的辅助函数
src/gba/jit/JitMem.cpp       可执行内存（W^X）
```

**范围**：只做 ARM 状态；内存访问全走 helper（3.5 的 A 档）；标志位每条指令回写（3.4 的保守版）。

**接入**：开关已经就绪（3.8）：`Options > Emulation > CPU Recompiler (JIT)`，默认关闭，
设置存 `vba.ini` 的 `jit` 键，命令行 `-jit` / `-nojit` 覆盖。Phase 2 只换后端，
不需要再动菜单或设置格式。

**验收**
- 与 Phase 1 同一套测试全部通过。
- `HashTrace` 与解释器逐帧一致（3 个 ROM × 600 帧）。
- 纯 CPU 基准（关渲染、关声音，`systemFramesToSkip` 拉满）≥ **1.8x**。

### Phase 3：Thumb + 全覆盖 + 内存内联（2-3 周）

- Thumb 解码器接上（`thumbInsnTable` 是 1024 项，覆盖面比 ARM 碎）。
- 内存访问升到 B/C 档，ROM/EWRAM/IWRAM 内联快路径。
- 标志位改成块内寄存器缓存，只在出口物化。
- 块缓存落盘（4.5，可选）。

**验收**：同上 + 纯 CPU 基准 ≥ **2.5x**；整帧（60 fps、GDI、无滤镜）≥ **1.1x**。

### Phase 4：ARM64 后端（2-3 周）

asmjit 的 a64 后端把 Phase 2/3 的发射逻辑重写一遍。语义、IR、块缓存、失效全部复用。

**验收**：ARM64 机器上同一套差分测试通过；性能不低于 x64 的 70%。

### Phase 5：AOT 导出（4-6 周）

**新增**
```
src/gba/jit/aot/AotTrace.cpp    执行轨迹记录
src/gba/jit/aot/AotEmit.cpp     IR → C 文件 + 派发表
tools/vba-aot/                  导出工具（或 vba.exe 的一个命令行模式）
src/gba/jit/aot/RuntimeMain.cpp 导出 exe 的 main / 窗口 / 输入
```

**验收**
- 3 个 ROM 导出的 exe 能通关（或跑到与模拟器相同的进度）。
- 覆盖率报告 ≥ 95%。
- 与模拟器同一 `.vbm` 回放，逐帧 HashTrace 一致。

---

## 七、风险表

| 风险 | 概率 | 影响 | 处置 |
|---|---|---|---|
| **确定性漂移**（JIT 与解释器差一条指令的时序） | 高 | 致命：TAS/电影失效 | 逐指令记账（3.3）；HashTrace 作为合入门槛；任何阶段不达标不合入 |
| 自修改代码 / RAM 代码 | 高 | 花屏、死循环 | 页版本号失效（3.6）；测试集必须包含用 IWRAM 代码的游戏 |
| 标志位物化时机错 | 中 | 随机跑飞，极难查 | 第一版每条指令回写；C 后端可读，便于二分定位 |
| 块缓存与 loadstate/rewind 冲突 | 中 | 读档后行为异常 | loadstate 清空缓存；写进 `CPUReadMemState` |
| 三平台维护成本 | 中 | x64 好了 ARM64 落后 | 语义层共享，后端隔离；Win32 明确不支持 |
| 性能不达预期（CPU 占比本来就低） | 中 | 白干 | Phase 0 先测；不到 20% 就改做渲染 |
| 代码量（约 8-12k 行） | 高 | 工期超支 | 分阶段，每阶段可独立交付、独立回退 |
| 许可证（GPLv2-only + 第三方） | 低 | 合规问题 | 只用 Zlib/BSD/MIT；MPL 代码单独评估 |

---

## 八、明确不做的

1. **不做 x86-32 后端。** Win32 构建留在解释器，收益/成本比太差。
2. **不做寄存器分配器和指令调度。** 直译 + 宿主寄存器缓存 16 个 GBA 寄存器就够了。
   上分配器是另一个数量级的工程量，收益在 GBA 这种指令密度上不成立。
3. **不改时序模型、不改存档格式。** 这两条一破，整个项目的可复现性就没了。
4. **不做"完全静态"的 AOT。** GBA 上不成立。
5. **不在没有 HashTrace 基线的情况下合入任何 JIT 代码。**
6. **不为了 JIT 引入新窗口/新线程/新渲染路径**（项目铁律：先想复用）。

---

## 九、第一周就能做的六件事

1. 加 `VBA_PROFILE_FRAME` 分段计时，跑三个 ROM，拿到 CPU 占比。
2. 修掉 `GBA-arm.cpp:2335` / `GBA-thumb.cpp:1711` 的无条件内存读，HashTrace 验一遍。
3. 跑一遍 `HashTrace` 建立基线 CSV（三个 ROM × 600 帧），存进 `documentation/` 之外的地方
   （不要进仓库，体积大）。
4. 把 `src/gba/jit/` 目录和 `JitApi.h` 空壳建起来，加进 `VBA_Latest.vcxproj` 的
   `ClInclude`/`ClCompile`，确认三个平台都能编过（哪怕里面全是 `return false`）。
5. 写 `JitDecodeArm.cpp` 的第一个版本：只支持 `MOV/ADD/SUB/CMP/B`，配一个把 IR 打成
   文本的 `JitDump()`。
6. 拿 jsmolka 的 `gba-tests/arm` 里最小那个测试 ROM，让 `JitDump()` 的输出和
   `armdis.cpp` 的反汇编逐条对照。

这六件事做完，你会有：一个真实的性能数字、一个被验证过的 IR 骨架、和一条可复现的
验收链路。**在那之后，再决定要不要继续投入 Phase 2。**

## 十、怎么验证（四层，别跳）

重编译器的错误有两种：**解码错了**（字段拆错）和**语义错了**（行为与解释器不同）。
前者看反汇编能抓，后者只有差分测试能抓。跑游戏看花不花屏是最没用的验证方式——
错一条指令可能几万帧后才炸，也可能永远炸不了。

### 第 0 层：解码空间全扫（`-jitverify` 自带，1 秒）

比逐条差分更早、更狠的一层：**两张分派表都是少数几位的函数**（ARM 按 bits 27-20 与 7-4，
256×16；Thumb 按 bits 15-6，1024 项），所以"模拟器认为哪些字是未定义"可以穷举，
不用执行就能和解码器对账。`sweepDecodeSpace()` 就是这么做的：

```
arm decode space
  4096 words checked, 0 disagreements
thumb decode space
  1024 words checked, 0 disagreements
```

两类不一致都算 bug：解码器执行了模拟器判未定义的字（生成的代码会做解释器永远不会做的事），
以及解码器报异常而模拟器在执行。解码器**拒绝解码**（返回 false）不算——调用方会退回解释器。

这一层立刻抓出过 `bx lr`（0x4770 被当成 BLX）、`swi` 的判定位写成 bit24（吞掉协处理器行
0xDx/0xEx）之类"只靠随机 ROM 字节要碰运气"的错误。

### 第 1 层：解码自测（已就绪，秒级）

`src/gba/jit/selftest.cpp` 是宿主测试，不需要 MSVC，clang 直接编：

```
clang++ -std=c++17 -I src/gba/jit \
        src/gba/jit/selftest.cpp src/gba/jit/JitDecodeArm.cpp \
        src/gba/jit/JitDecodeThumb.cpp src/gba/jit/JitDump.cpp \
        -o selftest && ./selftest
```

同一份源码要**两种模式各编一次**，V7 分支否则根本没被编过：

```
clang++ -std=c++17 -DUSE_GBA_CORE_V7 -DUSE_GB_CORE_V7 -I src/gba/jit \
        src/gba/jit/selftest.cpp src/gba/jit/JitDecodeArm.cpp \
        src/gba/jit/JitDecodeThumb.cpp src/gba/jit/JitDump.cpp \
        -o selftest_v7 && ./selftest_v7
```

当前规模：**V8 1101 项、V7 1207 项断言**（V7 多出来的部分来自 `fetchCycles` 逐条校验和
半字空洞/`JF_QUICK` 的 V7 专属用例）。覆盖两套指令集的
ALU/移位/访存/乘法/块传送/分支/SWI/MRS/MSR/SWP/PUSH/POP
字段拆解，以及块边界（分支结束、写 PC 结束、`POP {pc}` 结束、1 KB 页边界截断、
`JIT_MAX_INSNS` 截断）和"整块拒绝"（BKPT）。它只证明"编码读对了"，不证明"行为对"。

V7 模式下 `thumbCycles` 是测试自己定义的一张合成表（1..4 循环），所以这一层验证的是
"解码器按 `opcode >> 8` 索引"，不是表的真实数值。

### 第 2 层：逐指令差分（`-jitverify`）

对 ROM 里的每一条指令，从**完全相同的机器状态**出发跑两遍（ARM 和 Thumb 各一趟）：

| | 路径 |
|---|---|
| A | 真解释器 `armExecute()`，把 `cpuNextEvent` 设成 1 让它跑一条就返回 |
| B | `jitDecodeOne()` + `jitInterpInsn()` |

比对 `r0-r15`、`CPSR`、`N/Z/C/V`、`cpuTotalTicks`、`busPrefetchCount`。

```
VBA_VS_Latest_V82XR.exe -rom win32/x64/V82XR/roms/aos.gba -jitverify 20000
```

> **当前必须用 V82XR，不是 V72XR。** 现有的 `-jitverify` 参考路径是 V8 的
> `armExecute()` / `thumbExecute()`；V7 还没有对应的单条参考函数，V72XR 会打印
> "not available"。V7 的验证方案见 3.2.1.10，等 `armExecuteOne()` /
> `thumbExecuteOne()` 落地后再切到 V72XR。

输出长这样（先两段解码空间，再两趟逐条）：

```
arm decode space
  4096 words checked, 0 disagreements
thumb decode space
  1024 words checked, 0 disagreements
arm pass
MISMATCH 08001234  raw=...  op=alu  C
  19987 tested, 1 failed, 1200 skipped (memory writes, SWI, undefined), 13 undecodable
  failures by op: alu=1
thumb pass
  9211 tested, 0 failed, 0 skipped (memory writes, SWI, undefined), 0 undecodable
arm block pass
  2000 blocks (4821 instructions) tested, 21 failed, 7988 skipped (SWI, undefined, mode switch), 1269 refused
thumb block pass
  2000 blocks (35796 instructions) tested, 5 failed, 41 skipped (SWI, undefined, mode switch), 0 refused
jit verify: 1 mismatches (1 arm, 0 thumb), 26 block mismatches (21 arm, 5 thumb), 0 decode-space disagreement(s)
```

> 上面那 26 条块级"失败"是数据当代码，不是真 bug（见第 2.5 层）。看这一行时
> 先看 `0 decode-space disagreement(s)`，再看两趟逐指令的 `0 failed`，最后才看块级。

`-jitverify N` 的 N 是每趟从 ROM 头部往后扫的指令条数上限，建议先 5000 再 50000。

两趟都要看：Thumb 解码器是另一份代码，ARM 全绿不代表 Thumb 全绿。ROM 里 ARM 和 Thumb
是交错存放的（Thumb 代码会被当成 ARM 指令扫一遍），所以极少数"错位"位置的报错通常是
数据被误解码，不是真 bug——先把 N 调小、确认报错地址落在已知的代码段里再看。

**2026-11-02 实测**（三个商业 ROM 各 20000 条 + 一次全扫）：

| ROM | arm | thumb | 解码空间 |
|---|---|---|---|
| Super Mario Advance (J) | 0 failed | 0 failed | 0 |
| Metroid Fusion (U) | 0 failed | 0 failed | 0 |
| 黄金太阳 (J) | 0 failed | 0 failed | 0 |

### 第 2.5 层：块级差分（`-jitverify` 自带）

逐指令差分只能证明"单条指令对"，证明不了"块跑起来对"：块会自己决定在哪停、会在中途切
指令集、会把 `cpuPrefetch` / `armNextPC` 留给下一条。这些都不在单条比较的视野里，而且
错了的表现是**卡死**而不是某个寄存器错。所以 `-jitverify` 在逐指令两趟之后再跑一趟：

| | 路径 |
|---|---|
| A | 解释器连跑 `block.count` 条（每次 `cpuNextEvent = cpuTotalTicks + 1`，与 `CPULoop` 的驱动方式一致） |
| B | `jitRunBlockForTest(&block)` —— 真解码器解出的块，真走 `runBlock()` |

比对 `r0-r15`、`CPSR`、`cpuTotalTicks`、`busPrefetchCount`、`armNextPC`、`cpuPrefetch[0..1]`。
**内存也比**：跑之前快照 workRAM + IWRAM，A 跑完存副本、还原，再跑 B，逐字节比 ——
所以 `STR` / `STM` / `PUSH` / `POP` / `SWP` 都进了这一层，不再跳过。

```
arm block pass
  2000 blocks (4821 instructions) tested, 21 failed, 7988 skipped (SWI, undefined, mode switch), 1269 refused
thumb block pass
  2000 blocks (35796 instructions) tested, 5 failed, 41 skipped (SWI, undefined, mode switch), 0 refused
```

（ARM 那趟只有 4821 条指令、7988 块被跳过：ROM 里大部分是 Thumb 代码，按 ARM 解出来
多半落进 SWI/未定义。这是正常的。）

**当前那 26 条不一致全部是"数据当代码"**：地址落在 ROM 的数据表里
（`0x080004D4`、`0x08008394`…），编码是 `stm r15!` 这类不可预测形态。判据是
"真代码块一条都没红"；把它们当 bug 去改解码器，反而会害死正常代码。
`SWI` / `UNDEF` / `MSR` 写控制域仍然跳过（异常与模式切换改的状态没法用内存快照还原）。

### 第 3 层：端到端（`-hashtrace`）

这一层才是最终验收：同一个 `.vbm` 回放，JIT 开/关各跑一遍，逐帧状态哈希必须逐字节相同。

```
VBA_VS_Latest_V82XR.exe -rom aos.gba -play aos.vbm 1 -nojit -hashtrace D://interp.csv
VBA_VS_Latest_V82XR.exe -rom aos.gba -play aos.vbm 1 -jit   -hashtrace D://jit.csv
```

两次都显式传 `-nojit` / `-jit`，不要依赖 `vba.ini` 里的当前值：`-jit` 会在正常退出时
写回设置，第一次对比之后 ini 就可能已经是开着的了。跑之前先删掉同目录的 `.sav`，
两次运行必须从同一状态出发。

**现状**：`aos.vbm` 跑 3600 帧，`ram` 与 `full` 两列逐字节相同。2026-11-03 之前卡在
第 8 帧，当时有**两处**独立差异：一是某次 IRQ 比解释器晚投递一条指令（`doAlu()` 写 PC
的周期记账，少 6 个周期），二是 `UMULL`/`SMULL` 解码反了（高 32 位全错）。两处都已修掉，
详见第六节 Phase 1 的第五批。

排查入口就是第 2.5 层用的那套轨迹：`JIT_PC_TRACE=1` 重编，`VBA_JIT_PCTRACE=<文件>`
各跑一遍，逐条比 `(pc, lr, sp, cpuTotalTicks)`。第一次分叉如果只有 `lr` 不同而
`pc`/`sp`/`cpuTotalTicks` 都相同，那就不是事件时刻的问题，而是上一条指令算错了值：
把 16 个寄存器和标志位一起打出来，对着出错地址反汇编，就能看出是哪条指令、哪一半算错。

### 当前覆盖范围（别指望它全绿）

| 情况 | 第 2 层（逐指令） | 第 2.5 层（块） |
|---|---|---|
| `STR` / `STM` / `PUSH` / `POP` / `SWP` | **跳过**：两条路径都会写内存，第二遍的起点就和第一遍不同了 | **覆盖**：跑前快照 workRAM+IWRAM，A 跑完存副本、还原后再跑 B |
| 模拟器判未定义的字 / `JOP_UNDEF` | **跳过**：会进 BIOS 的未定义异常处理，没有 BIOS 文件时还弹模态框；而且那是 ROM 数据，不是代码 | 跳过（整块不比） |
| `SWI` | **跳过**：进 BIOS 处理程序，副作用收不回来 | 跳过（整块不比） |
| `MSR` 写控制域 | **跳过**：会把 CPU 切进 ROM 字节恰好拼出来的任意模式，`CPUSwitchMode` 会弹框 | 跳过（banked 寄存器不在比较范围） |
| BKPT、表里指向 `armBreakpoint` 的字 | 解码器**整块拒绝**，这一条根本不会跑到 | 同左 |
| ROM 前 0xC0 字节 | 跳过（那是头，不是代码） | 跳过 |
| 访存地址 | 全部落在 work RAM（`0x02000000 + ...`），避免触发 IO 副作用 | 同左 |

所以"全绿"的含义是：**两套指令集里 ALU / 移位 / 乘法 / 分支 / LDR / LDM / 逐条访存写回 /
未定义异常 的语义和周期与解释器逐条一致（第 2 层），并且整块的寄存器、内存、`armNextPC`、
`cpuPrefetch`、周期也和解释器一致（第 2.5 层）**。还剩三类只能在第 3 层验：`SWI` 的 BIOS
副作用、异常入口、以及模式切换后的 banked 寄存器。

### 失败怎么读

1. 先看 `op=`：`alu` 多半是标志位或移位边界（shift=0 / shift=32 / RRX）。
2. 再看差异字段：`reg` 是数据错，`C`/`V` 是进位溢出公式错，`cycles` 是记账错。
3. 下面那行 `jitDumpBlock` 的输出就是这条指令的 IR，对着 ARM ARM 手算一遍。
4. 只有 `cycles` 差 1：检查 `codeTicksAccess*` 的参数。解释器传的是 `armNextPC`，而 handler 里的 `armNextPC` 等于 **pc+4**，不是 pc。

### 一个必须先记住的约定：PC 的三个值

解释器执行一条指令时同时存在三个"PC"，混用任何两个都会错：

| 名字 | 值 | 谁用 |
|---|---|---|
| 指令地址 | `pc` | 日志、`JitBlock.guestPc` |
| `armNextPC` | ARM：`pc + 4`；Thumb：`pc + 2` | 所有 `codeTicksAccess*`、`armNextPC - 4` 的日志 |
| `reg[15].I` | ARM：`pc + 8`；Thumb：`pc + 4` | 指令读 PC，所以 ARM 的 `BL` 存 `reg[15] - 4` 正好是 `pc+4` |

进入 handler 时 `armNextPC` 已经指向**下一条**指令，`reg[15]` 是 PC+8/PC+4。

`oldArmNextPC` 就是**当前指令的地址**（= `pc`），不是"上一条"：循环入口处 `armNextPC`
已经指向要执行的那条指令，`reg[15]` 才是它 +4；`u32 oldArmNextPC = armNextPC` 存下来之后
才执行 `armNextPC = reg[15].I`。所以"handler 没设 clockTicks 就用默认值"那个分支
（`clockTicks = 1 + codeTicksAccessSeq32(oldArmNextPC)`）等价于 `...Seq32(pc)`，
`0x08020000` 那个 prefetch 补丁同理（测的也是当前指令）。

> 这一条 2026-11-02 曾被我写成"上一条指令的地址"，**是错的**。块级差分跑真代码块时
> 不放过任何一个默认周期，加上复核循环才定案。`jitInterpInsn()` 用 `pc` 是对的，
> 块入口**不需要**为它做任何补偿。

`jitInterpInsn()` 进函数时先把 `armNextPC` 设成 `pc+4`/`pc+2`、`reg[15]` 设成 `pc+8`/`pc+4`，
再照抄 `armExecute()` / `thumbExecute()` 那三行 prefetch 记账，最后才分派。两个循环的掩码
不一样（ARM `0xFFFFFE00`、Thumb `0xFFFFFF00`），别合并。
