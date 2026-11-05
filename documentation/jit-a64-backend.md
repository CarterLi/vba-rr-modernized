# V7 核心的 ARM64/AArch64 native JIT：已确认事实与设计约定

> 状态（2026-11-05 夜）：**`src/gba/jit/JitEmitA64.cpp` 已按本文写出，
> `VBA_Latest.vcxproj` 的三处条件已改**。源码通过 ARM64 目标的语法检查和
> asmjit 编码器的全形态发射检查，但**尚未在 ARM64 上编译、链接或运行**。
> 三层验证（`-jitverify` / shadow / `-hashtrace`）全部留待目标机器执行。
>
> 本文的调研事实来自**另一台 Windows on ARM64 机器**；当前这份工作副本在
> 一台 x64 机器上，所以 1.2 节的"本机"指工作副本所在机器，不是目标机器。
>
> 目标与红线沿用 `documentation/jit-recompiler.md` 2.1（时序红线）与第三节
> （后端移植契约）：逐 guest 指令记账周期、IRQ 落点不变、JIT 状态不入存档、
> `-hashtrace` 的 `ram` 逐帧一致。任何 A64 表达不了的指令都必须截断前缀并交回
> IR 解释器，失败模式只能是"更慢"，不能是"不一样"。
>
> **实现顺序**：先按 `jit-recompiler.md` 3.1–3.8 把契约逐条对照（那是判据），
> 再用本文第 5 节的逐指令映射填代码（那是做法）。两者冲突时以契约和
> `JitEmit.cpp` 的实际行为为准，本文的伪代码只是参考。

---

## 0. 范围

- 只为 **V7 核心** 增加 ARM64 后端（`USE_GBA_CORE_V7`）。
- V8 构建（`Release|ARM64` / `V82XR|ARM64`）里该文件只提供三个空实现，
  行为与现在 ARM64 上 `JitEmit.cpp` 的 stub 完全相同。
- x64 后端（`JitEmit.cpp`）和 Win32 stub 不改语义；只调整工程文件的条件。
- 公共契约不变：
  - `bool jitNativeAvailable()`
  - `bool jitCompileBlock(const JitBlock *, JitCompiledBlock *)`
  - `void jitReleaseBlock(JitCompiledBlock *)`
  - `typedef int (*JitNativeFn)(s32 budget, JitExecResult *out)`
  - 退出状态：`JITX_NEED_INTERP` / `JITX_KEEP_PC` / `JITX_BUDGET` / `JITX_DONE`。

---

## 1. 当前构建事实（为什么 ARM64 现在没有 native 代码）

已核实：

1. `win32/VBA_Latest.vcxproj` 第 1001 行：
   ```xml
   <ClCompile Include="..\src\gba\jit\JitEmit.cpp" Condition="'$(Platform)'!='x64'" />
   ```
   因此 **Win32 和 ARM64** 都编译 `JitEmit.cpp`；该文件里的
   `#if (x64) && defined(USE_GBA_CORE_V7)` 在 ARM64 上为假，于是只有 stub。
2. 第 1364–1368 行：x64 专用的 `JitEmit.cpp` 条目带 `stdcpp20`、
   `ASMJIT_STATIC` 和 `..\third_party\asmjit` include。
3. 第 1369 行起：仓库内 asmjit 的全部 `.cpp`（arm/a64、axl、core、ujit、x86）
   条目条件都是 `Condition="'$(Platform)'=='x64'"`，所以 ARM64 构建根本不
   链接 asmjit。
4. 结果：ARM64 的 V7 exe 能正常跑解释器/JIT 缓存，但 `jit.log` 里
   `native blocks / compiles / native instructions` 都是 0。

选定改法（尚未实施）：

- 把第 1001 行条件改成仅 Win32：
  `Condition="'$(Platform)'=='Win32'"`（ARM64 不再需要 x64 文件的 stub）。
  **这一步是硬性要求**：`JitEmit.cpp` 在非 x64 下会提供
  `jitNativeAvailable/jitCompileBlock/jitReleaseBlock` 三个空实现
  （第 2385–2398 行），如果 ARM64 仍编译它，就会和 `JitEmitA64.cpp` 里的
  真实实现重复定义，链接失败。x64 那条独立条目（第 1364 行）不受影响。
- 新增一个 `ClCompile` 条目：
  ```xml
  <ClCompile Include="..\src\gba\jit\JitEmitA64.cpp"
             Condition="'$(Platform)'=='ARM64'">
    <LanguageStandard>stdcpp20</LanguageStandard>
    <PreprocessorDefinitions>ASMJIT_STATIC;%(PreprocessorDefinitions)</PreprocessorDefinitions>
    <AdditionalIncludeDirectories>..\third_party\asmjit;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
  </ClCompile>
  ```
- 把现有 asmjit 条目的条件从
  `'$(Platform)'=='x64'`
  扩为
  `'$(Platform)'=='x64' Or '$(Platform)'=='ARM64'`。
  不改文件清单：asmjit 的 core/arm/axl/ujit/x86 源码本身与宿主架构无关，
  全部编译最省事，也能保证 `a64_assembler.cpp` 依赖的 core 符号齐全。
- x64 的 `JitEmit.cpp` 条目保持原样。

工具链事实：**本文的调研与构建环境是一台 Windows on ARM64 机器**（VS 18
Insiders、MSVC `14.52.37025`、arm64 MFC 库齐全）。当前工作副本所在的 x64
机器是另一台，2026-11-05 在这里实测到的差异如下（只影响"能在哪里验证"，
不影响本文的 ARM64 事实）：

- 工作副本所在机器是 **AMD64 / x64**（`PROCESSOR_ARCHITECTURE=AMD64`，
  i7-13700KF）。VS 18 Insiders、MSVC `14.52.37025` 在，MSBuild/MSVC 不在 PATH。
- `bin/Hostx64/` 只有 `x64` 和 `x86` 两套，**没有 ARM64 交叉 `cl.exe`**；
  `bin/HostArm64/arm64/` 里是原生 ARM64 工具，x64 宿主上无法执行。
- `atlmfc/lib` 只有 `x64` / `x86`，**没有 arm64 MFC 库**。即使补上交叉编译器，
  `V7Release|ARM64` 的完整链接也仍会像 `MSB8041` 那样失败。
- arm64 的 C 运行时库**是齐的**：MSVC `lib/arm64`、SDK
  `Lib/10.0.28000.0/ucrt/arm64` 与 `um/arm64` 都在，SDK 头文件也全。
  所以"编译单个不依赖 MFC 的 TU"理论上可行，"链接整个 MFC 程序"不行。
- Windows x64 **不会反向模拟 ARM64**：就算产出了 ARM64 exe，本机也跑不了。
- 工程里的 ARM64 配置本身存在：`VBA_Latest.vcxproj` 的 `ProjectConfiguration`
  有 `V72XD/V72XR/V82XD/V82XR|ARM64` 四条，`.sln` 有
  `Debug|ARM64` / `Release|ARM64` / `V7Debug|ARM64` / `V7Release|ARM64` 四个
  解决方案平台。缺的是工具链和库，不是配置。
- **这份 x64 工作副本里没有 ARM64 构建痕迹**：`win32/.VS_Latest/V72XR/`
  只有 `x64`，`win32/arm64/` 不存在。目标机器上已有的 ARM64 产物不在这个
  工作区，所以不要拿这里的目录结构去否定目标机器的构建结果。
- 测试 ROM / 录像在 `win32/x64/V72XR/roms` 与 `win32/x64/V72XR/movies`，
  目标机器上要把这两个目录一起带过去。

构建要求不变：asmjit 需要 C++20（`std::endian`），`JitEmitA64.cpp` 和每个
asmjit `.cpp` 都要按文件设 `stdcpp20` 并定义 `ASMJIT_STATIC`；目标构建命令是
`msbuild win32/VBA_Latest.sln /p:Configuration=V7Release /p:Platform=ARM64`
和 `/p:Configuration=Release /p:Platform=ARM64`（V8，验证 stub）。

### 1.1 asmjit 在 Windows ARM64 上能不能编译（已核对源码，尚未真正构建）

逐项查过 asmjit 源码里的架构/平台分支，结论是**没有阻塞项**：

1. 架构识别（`core/build_defs.h:246-249`）：
   `_M_ARM64` → `ASMJIT_TARGET_ARCH_ARM 64`；
   随后（同文件 316-321）自动定义 `ASMJIT_HAS_HOST_BACKEND` 和
   `ASMJIT_UJIT_AARCH64`。
   `ASMJIT_NO_FOREIGN` 没有被定义，所以 `ASMJIT_NO_X86` 也不会被自动打开，
   x86 后端源码照常编译（它只是目标后端，与宿主无关）。
2. 双映射（`core/virt_mem.cpp:94-116`）：
   - Apple 走 `MACH_VM_REMAP`；NetBSD 走 `REMAPDUP`；
   - **其他平台（含 Windows）只要没定义 `ASMJIT_NO_DUAL_MAPPING`，就定义
     `ASMJIT_ANONYMOUS_MEMORY_USE_FD`**，走文件映射实现。
   - 第 1309 行的 `#error ... doesn't have implementation` 只在上述三条
     分支全不成立时才可能命中；Windows ARM64 命中 FD 分支，不会触发。
3. `core/cpu_info.cpp` 里的 `__cpuidex`/内联汇编全部包在
   `#if ASMJIT_TARGET_ARCH_X86` 内，ARM64 编译时整段不参与，不存在
   "MSVC ARM64 不支持 x86 内联汇编"的问题。
4. `.vcxproj` 里的 asmjit `.cpp` 清单与磁盘**完全一致**：磁盘 57 个 `.cpp`
   （arm 10、axl 4、core 29、ujit 3、x86 11），工程里也是 57 条，
   只差 `..\` 路径前缀。条目范围是第 1369–1652 行，每条 4 行
   （`ClCompile` + `LanguageStandard` + `ASMJIT_STATIC` + include），
   所以扩条件时是对这 57 条统一处理。
5. 宿主环境识别（`core/arch_traits.h:70-95`、`environment.h:259`）：
   `Arch::kHost` 在编译期就解析成
   `ASMJIT_TARGET_ARCH_ARM == 64 && 小端 → Arch::kAArch64`，
   `Platform::kHost` 解析成 `Platform::kWindows`。
   `Environment::host()` 返回的就是这组常量，所以 `JitRuntime::_environment`
   一定是 AArch64 + Windows，不会出现"`a64::Assembler` 与 `CodeHolder`
   架构不符"的 init 失败。
6. 剩余的未知只剩"MSVC 14.52 的 ARM64 模式实际编译 a64 后端"这一步，
   必须靠第一次构建确认（见第 9 节风险 1）。

### 1.2 工作副本所在机器（x64）的可行性矩阵（2026-11-05）

| 动作 | 工作副本所在机器 | 需要什么 |
| --- | --- | --- |
| 写 `JitEmitA64.cpp` 源码 | **可以** | 本文第 2–6 节 + `JitEmit.cpp` |
| 语法/API 检查 | **可以**，实测通过 | `clang++ -fsyntax-only --target=aarch64-windows-msvc -std=c++20 -DASMJIT_STATIC` + MSVC/SDK 头文件（见 4.4） |
| 实跑 asmjit A64 编码器 | **可以**，实测通过 | 编码器与宿主架构无关；在 x64 上把 `Environment` 初始化成 `Arch::kAArch64` 即可（见 4.4） |
| MSVC 编译 ARM64 目标文件 | **不行** | 缺 `Hostx64/arm64/cl.exe`；要装 ARM64 交叉工具链 |
| 链接 `V7Release\|ARM64` | **不行** | 缺 arm64 MFC 库；要装 ARM64 版 MFC |
| 运行 ARM64 exe | **不行** | Windows x64 不模拟 ARM64；要真 ARM64 机器 |
| `-jitverify` / shadow / `-hashtrace` | **不行** | 同上，见 8.1–8.3 |

因此**x64 工作副本可以产出"语法与编码都检查过的源码"，不能产出"验证过的
实现"**。项目铁律是"不在没有 HashTrace 基线的情况下合入任何 JIT 代码"
（`jit-recompiler.md` 9.1 的合入门槛），所以这份代码要等目标 ARM64 机器跑完
第 8 节的全部三层才能合入。

**2026-11-05 夜已完成的部分**（详见 11.1）：`JitEmitA64.cpp` 全部发射逻辑、
`ErrorHandler`、`VBA_JIT_ASM` dump、三个公共接口；`VBA_Latest.vcxproj` 的
三处条件改动。已验证：clang `--target=aarch64-windows-msvc -fsyntax-only`
0 错误；asmjit A64 编码器实跑 74 个指令形态共 0 发射错误、0 未绑定标签。
**未验证**：MSVC ARM64 编译、链接、任何一条指令的运行期语义。

**目标机器上的最小准备清单**：

1. Windows on ARM64 + VS 18 Insiders（含 ARM64 MFC 库和 ARM64 交叉/原生工具链）。
2. 把 `win32/x64/V72XR/roms` 与 `movies` 拷到 ARM64 产物目录。
3. 确认 `msbuild /p:Configuration=V7Release /p:Platform=ARM64` 能出 exe
   （这是第 9 节风险 1 的真正验证点）。
4. 再按 8.1 → 8.2 → 8.3 的顺序验收；`jit.log` 里 `native blocks` 必须非零。

---

## 2. 生成代码的 ABI 与寄存器计划

### 2.1 Windows ARM64 ABI 事实

- 整数参数：`x0, x1, ...`；返回值：`x0`（32 位结果用 `w0`）。
- `x19–x28` 是 callee-saved；`x29` 是 FP，`x30` 是 LR，也都是 callee-saved。
- `x0–x18` 是 caller-saved/scratch；`x18` 是平台寄存器，**不要用**。
- 函数入口 `sp` 按 16 字节对齐；因此栈帧必须是 16 的倍数。
- 调用 C++ helper 时：参数放 `w0/w1`，结果在 `w0`；`x19–x28` 跨调用保活。

### 2.2 寄存器分配（A64 后端固定）

| 寄存器 | 用途 |
|---|---|
| `x19` | `&reg[0]`；`reg[r]` 就是 `[x19, #r*4]` |
| `w20` | 已执行的周期数（cycles） |
| `w21` | 本块的周期预算（budget） |
| `x22` | `JitExecResult *out` |
| `x23` | 当前访存地址（跨 helper 调用保活） |
| `x24` | 第二个保活值：写回值 / 新基址 / SWP 读出的值 |
| `x25` | `&N_FLAG`（N/C/Z/V、armState 用固定小偏移） |
| `x26` | `&memoryWait[0]`（六张 wait 表用固定偏移） |
| `x27` | `&memoryMap[0].address`（mask 在 `+8`） |
| `x28` | `&gJitShiftCarry` |
| `x29/x30` | 只保存/恢复，不主动使用 |
| `w0/w1` | helper 参数 |
| `w0` | helper 返回值 |
| `w3–w12` | 临时寄存器（不跨 helper 调用） |
| `x16` | 装 helper 绝对地址后 `blr x16` |

注意：

- 32 位 guest 地址一律放在 `w` 寄存器里（`ldr w23, [...]` 会自动清零高 32 位），
  但作为 A64 访存基址时必须写成 `x23`；两者是同一个物理寄存器。
- 不要碰 `w20/w21/w22/w23/w24/w25/w26/w27/w28` 作为 scratch；它们承载状态。
- `w25/w26/w27/w28` 的低 32 位也不要用，避免破坏后续地址运算。

### 2.3 栈帧、序言、尾声

固定 96 字节帧（不区分是否访存；这是 A64 与 x64 的关键差异）：

```
[sp +  0] x29   [sp +  8] x30
[sp + 16] x19   [sp + 24] x20
[sp + 32] x21   [sp + 40] x22
[sp + 48] x23   [sp + 56] x24
[sp + 64] x25   [sp + 72] x26
[sp + 80] x27   [sp + 88] x28
```

序言（伪代码）：

```
stp x29, x30, [sp, #-96]!
stp x19, x20, [sp, #16]
stp x21, x22, [sp, #32]
stp x23, x24, [sp, #48]
stp x25, x26, [sp, #64]
stp x27, x28, [sp, #80]
mov w21, w0                 // budget
mov x22, x1                 // out
mov w20, wzr                // cycles = 0
emitMovImm64(x19, &reg[0])
emitMovImm64(x25, &N_FLAG)
emitMovImm64(x26, &memoryWait[0])
emitMovImm64(x27, &memoryMap[0].address)
emitMovImm64(x28, &gJitShiftCarry)
```

尾声：

```
mov w0, w20                 // 先取返回值，再恢复 x20
ldp x27, x28, [sp, #80]
ldp x25, x26, [sp, #64]
ldp x23, x24, [sp, #48]
ldp x21, x22, [sp, #32]
ldp x19, x20, [sp, #16]
ldp x29, x30, [sp], #96
ret
```

为什么 A64 可以固定帧：x64 只在有内存操作时才 push `rdi/rsi`，所以
`frameKind` 必须区分；A64 每块都保存同一组寄存器，**所有块的帧形状一致**，
`frameKind` 可以恒为 0。这样链式跳转（前驱 `br` 到后继的 `chainEntry`，
不重新执行序言）永远安全，`tryLink()` 的同 `frameKind` 检查也永远通过。

#### 2.3.1 链式跳转为什么能跳过后继的序言（已对照 x64 实现核实）

这不是"因为帧一样所以可以跳过"这么含糊：真正成立的原因是**前驱的帧被后继
的尾声直接复用**。

1. 前驱执行到 `emitChainExit` 时，它自己的 `sp` 仍指向它序言建立的帧底，
   `x19–x28/x29/x30` 的原始值都还保存在 `[sp+16 .. sp+88]`。
2. 前驱 `br x9` 直接跳到后继的 `chainEntry`（序言之后、预算守卫之前）。
   此时**没有任何栈操作**，`sp` 仍是前驱的帧底。
3. 后继的尾声（第 2.3 节）执行
   `ldp x27,x28,[sp,#80]` … `ldp x29,x30,[sp],#96`。
   它弹出的正是**前驱序言保存的那一份**原始寄存器值；因为所有块帧形状
   相同，偏移和顺序都完全匹配。
4. 所以最终 `ret` 用的是最初的调用者保存在 `[sp+8]` 的 `x30`，
   `x19–x28` 也恢复成调用者的值，和"后继自己被调用"完全等价。

推论（实现时必须守住）：

- `chainEntry` **必须**正好在"序言最后一条指令之后、第一条预算判断之前"。
  提前或推后都会让被复用的帧错位。
- 所有块（包括没有访存的块）必须保存完全相同的寄存器集合；
  一旦改成"按需保存"，`frameKind` 就必须重新承担区分职责。
- 块内不能改写 `sp`，也不能在链式出口前的路径上 `add sp`；
  栈只在序言/尾声各动一次。
- 链式入口看到的状态是：`x19–x28` 指向前驱装好的同一组全局地址，
  `w20` = 前驱已计周期，`w21` = 同一预算，`x22` = 同一 `JitExecResult`。
  后继直接用这些值，不做任何重新初始化（`mov w20,wzr` 只能出现在序言里）。

### 2.4 helper 调用

```
emitMovImm64(x16, (uint64_t)(uintptr_t)fn);
blr x16;
```

- 每次调用重新装地址（helper 可以破坏 `x16`）。
- 不用 `bl` 相对调用：JIT 代码页由 `VirtualAlloc` 分配，离静态代码可能超过
  ±128 MB。
- 参数最多两个，不需要栈参数；返回地址在 `x30`，序言已保存。
- helper 调用会破坏 NZCV，所以**所有 guest 标志位在 helper 返回后必须从内存
  重新读**；不能假设 A64 标志跨调用有效。

### 2.5 绝对地址生成

A64 没有"把 64 位立即数装进寄存器"的单条指令，也不用 `adrp`+重定位。
统一用 `movz/movk`：

```
emitMovImm64(dst, v):
  movz dst, #(v & 0xFFFF), lsl #0
  if (v >> 16) movk dst, #((v >> 16) & 0xFFFF), lsl #16
  if (v >> 32) movk dst, #((v >> 32) & 0xFFFF), lsl #32
  if (v >> 48) movk dst, #((v >> 48) & 0xFFFF), lsl #48
```

`emitMovImm32(dstW, v)` 只发前两条。

这样不受 ASLR、代码页距离、链接布局影响，代价是序言约 20 条指令、每次 helper
调用 5 条指令。先以正确性为先；如果性能不够，再考虑 adrp/重定位或常量池。

### 2.6 运行时结构的偏移

`src/gba/jit/JitEmit.h`（已核实）：

```
struct JitExecResult {       // 8 字节
    u32 nextPc;              // +0
    u32 status;              // +4
};
```

- `x22 = out`；出口写 `str wNext, [x22]`、`str wStatus, [x22, #4]`。
- 状态值是位标志：`JITX_NEED_INTERP=1`、`JITX_KEEP_PC=2`、
  `JITX_BUDGET=4`、`JITX_DONE=8`；每次都写完整的 32 位字。
- `JITX_KEEP_PC` 目前没有发射点：编译器在写 PC 的非分支指令前就截断前缀
  （见 5.1），所以 A64 也可以永远不产生它。

```
struct JitChainSlot {        // 16 字节（u64 对齐）
    u64 target;              // +0，0 表示未链接
    u32 tag;                 // +8，(nextPc & ~1) | thumb
    u16 linkIndex;           // +12，0xFFFF 表示未链接
    u16 reserved;            // +14
};
```

- `target` 是后继 `chainEntry` 的绝对地址，用 `ldr x9,[x9]` 读、`br x9` 跳。
- `JitCompiledBlock` 里 `chain[4]`（`JIT_MAX_CHAIN=4`），
  所以每个块最多 4 个可链接出口，超出就退回"直接写 out 并返回"。
- 槽位位于 `JitCodeEntry` 的普通内存里（不是可执行页），链接/解链只是
  8 字节存储，不需要改代码页权限。
- `JitCompiledBlock::chainEntry` 在头文件里是 `u8 *`，必须是
  `(u8*)fn + chainEntryOffset`；`frameKind` 是 `u8`，A64 恒 0。

---

## 3. 全局量与内存布局（已核实）

### 3.1 寄存器与标志

来源：`src/gba/GBAGlobals.h`、`src/gba/GBAGlobals.cpp`。

- `typedef unsigned char bool8;`（`src/Port.h`）。
- `reg_pair` 是 union：`u8 B[4]` / `u16 W[2]` / `u32 I`，小端下 `I` 在偏移 0，
  整个类型 4 字节；`reg[45]`。
- 因此 `reg[r]` 的 32 位值就是 `[x19, #r*4]`，偏移最大 `44*4 = 176`，
  远小于 `ldr/str` 无符号偏移上限 16380。
- 声明顺序：
  ```
  bool8 N_FLAG, C_FLAG, Z_FLAG, V_FLAG, armState;
  u32   armNextPC;
  int32 armMode;
  ```
  实现里使用 **运行时指针差** 计算偏移（`(char*)&C_FLAG - (char*)&N_FLAG` 等），
  不把 0/1/2/3 写死。当前布局预期是：
  - `N_FLAG +0`，`C_FLAG +1`，`Z_FLAG +2`，`V_FLAG +3`
  - `armState +4`
  - `armNextPC +8`（4 字节对齐产生 3 字节 padding）
  - `armMode +12`
- 这些偏移都必须能用 `ldrb/strb/ldr/str` 的 12 位立即数表示（事实上的距离是
  几十字节）；若不满足，`jitCompileBlock()` 应拒绝编译并回退 IR。
- `reg[16]` 是 CPSR，`reg[17]` 是 SPSR（x64 后端同样直接按 `r*4` 访问）。

### 3.2 wait 表

来源：`src/gba/V7/GBA.cpp` 第 140–151 行。

```
int32 memoryWait[16];
int32 memoryWait32[16];
int32 memoryWaitSeq[16];
int32 memoryWaitSeq32[16];
int32 memoryWaitFetch[16];
int32 memoryWaitFetch32[16];
```

- 每张表 16 项 × 4 字节；按声明顺序的偏移是 `0/64/128/192/256/320`。
- 和 flags 一样，**不要写死**，用 `(char*)&memoryWait32[0] - (char*)&memoryWait[0]`
  之类的指针差生成偏移；这些偏移也必须落在 `ldr w` 的 12 位缩放立即数范围内
  （实际 < 400 字节）。
- 查表方式（A64 寄存器偏移不能叠加立即数）：
  ```
  lsr  w3, addr, #24
  and  w3, w3, #15
  add  w3, w3, #(tableOffset / 4)
  ldr  w4, [x26, w3, uxtw #2]
  add  w20, w20, w4
  ```
  `and #15` 必须保留：x64 也是先 `shr 24` 再 `and 15`，地址高字节可能 > 15。

### 3.3 memoryMap

来源：`src/gba/GBAinline.h`、`src/gba/V7/GBAMemory.cpp`。

```
struct MemoryMap {
    u8 *address;   // offset 0
    u32 mask;      // offset 8
};                 // sizeof == 16
MemoryMap memoryMap[256];
```

- `x27 = &memoryMap[0].address`。
- 内联读快路径：
  ```
  ldr  x9,  [x27, w8, uxtw]       // address 指针
  ldr  w10, [x27, w8, uxtw] ...   // 需先 add w8, w8, #8 再读 mask
  ```
  A64 的 `[base, index]` 只能二选一：要么立即数，要么寄存器；所以先用
  `lsl w8, w8, #4` 算出 region*16，再分别读 `+0` 和 `+8`。
- `MemoryMap` 的 16 字节 stride、`address` 在 0、`mask` 在 8 是实际布局；
  实现时应在 `JitEmitA64.cpp` 里加 `static_assert(sizeof(MemoryMap) == 16)`，
  但 mask 偏移建议仍用指针差或 `offsetof` 计算。

### 3.4 `gJitShiftCarry`

- x64 后端在匿名 namespace 里定义 `u8 gJitShiftCarry = 0xFF;`，并把它作为
  `jitShiftReg()` 的隐式第二返回值。
- A64 文件需要自己的同名定义（ARM64 构建不编译 x64 文件，不会冲突）：
  - `0` / `1`：本次移位定义了 C；
  - `0xFF`：本次移位不定义 C（保持原 C）。
- `x28 = &gJitShiftCarry`，读写都是 `ldrb/strb [x28]`。

---

## 4. asmjit A64 API 事实（已核对头文件）

### 4.1 头文件与类型

- 包含：`<asmjit/core.h>`、`<asmjit/core/jit_runtime.h>`、`<asmjit/a64.h>`。
- 命名空间：`asmjit::a64`；汇编器 `asmjit::a64::Assembler`（每条指令固定
  4 字节，`_emitI()` 直接追加到 CodeHolder 的 buffer）。
- 寄存器常量在 `asmjit::a64::regs` 里，全部是 `static constexpr Gp`
  （`a64_operand.h:729-764`）：`x0..x30`、`w0..w30`、`sp`、`wsp`、`xzr`、
  `wzr`。`a64_operand.h:968` 有 `using namespace regs;`，所以 `a64::x19`
  与 `a64::regs::x19` 等价。`sp/wsp/xzr/wzr` 的寄存器号都是 31，含义由
  指令的编码位置决定（`ldr [sp]` 是栈指针，`add w0,w1,wzr` 是零寄存器）。
- `Gp` 有 `.w()` / `.x()` / `.r32()` / `.r64()` 克隆方法。
- `CondCode` 不是 A64 的 4 位条件域，而是 `asmjit::arm::CondCode`
  （`core/arch_commons.h:25-43`），编码成 `(cc - 2) & 0xF`：
  `kAL = 0x00`、`kNA = 0x01`、`kEQ = 0x02`、`kNE = 0x03`、
  `kHS/kCS = 0x04`、`kLO/kCC = 0x05`、`kMI = 0x06`、`kPL = 0x07`、
  `kVS = 0x08`、`kVC = 0x09`、`kHI = 0x0A`、`kLS = 0x0B`、
  `kGE = 0x0C`、`kLT = 0x0D`、`kGT = 0x0E`、`kLE = 0x0F`。
  `a64` 通过 `arch_commons.h:264-265` 的 `using namespace arm;` 把它导出，
  所以 `a64::CondCode::kEQ` 合法。**GBA 的条件码正好是上面减 2 的顺序**
  （`in.cond == 0xE` → `kAL`，其余 → `(CondCode)(in.cond + 2)`），
  不能把 `0..0xE` 直接强转成 `CondCode`。
- `Shift` 是独立的 `asmjit::arm::Shift`（`arch_commons.h:170-205`），
  `a64::lsl(n)`/`lsr(n)`/`asr(n)`/`ror(n)` 与扩展 `uxtb/uxtw/uxtx/
  sxtb/sxth/sxtw/sxtx(n)` 都返回它。`asmjit::Imm(const arm::Shift&)`
  （`core/operand.h:2450`）把 op 放进 predicate、value 放进 payload，
  因此四操作数形式 `add(Gp,Gp,Gp,Imm)` 可以直接接收 `a64::uxtw(2)`。
- 内存操作数同样由 Shift 构造：`a64::ptr(base, imm)`、
  `a64::ptr_pre(base, imm)`（`[base,#imm]!`）、
  `a64::ptr_post(base, imm)`（`[base],#imm`）、
  `a64::ptr(base, index, Shift)`。前/后索引已由 `a64_assembler.cpp` 的
  `m.is_pre_or_post()` 分支实现，`ldp/stp` 也支持（`a64_emitter.h:417/549`）。
- 立即数包装：`asmjit::Imm(v)`；enum class（`CondCode`/`ShiftOp`/
  `MemOp` 等）可直接传给 `Imm` 参数（`Imm` 有模板构造）。
- `Assembler::offset()` 返回**当前 buffer 的字节偏移**（`size_t`，
  `core/assembler.h:70`），不是指令条数。A64 定长，所以序言结束处的
  `offset()` 同时就是 `chainEntry - fn`。
- `CodeHolder::code_size()` 返回所有 section 的总字节数
  （`core/code_holder.h:1326`），`copy_flattened_data(dst, size,
  CopySectionFlags)`（1346）把代码摊平拷出；`VBA_JIT_ASM` 的 dump 就是
  这两个函数（`JitEmit.cpp:2262-2265`）。**没有 `CodeHolder::code_offset()`**。
- `rt->add(&fn, &code)` 的返回类型是 `asmjit::Error`，成功值是
  `Error::kOk`（`JitEmit.cpp:2348/2361`）；`CodeHolder::init()` 同样返回
  `Error`。`JitRuntime` 自己不记录"上一次错误"，失败只会以返回值出现。

`JitEmitA64.cpp` 的头部应当照 `JitEmit.cpp:44-88` 的写法**自己声明**模拟器
符号，而不是包含 `GBA.h` / `V7/GBACpu.h`（那两个头会拖进 `UPDATE_REG` 和
handler 宏，JIT 源要求能单独编译）：

```cpp
#include "../GBAGlobals.h"   // reg[], N_FLAG..V_FLAG, armState, armNextPC
#include "../GBAinline.h"    // memoryMap, CPUReadMemoryQuick

extern int32 memoryWait[16];
extern int32 memoryWait32[16];
extern int32 memoryWaitSeq[16];
extern int32 memoryWaitSeq32[16];
extern int32 memoryWaitFetch[16];
extern int32 memoryWaitFetch32[16];

extern u32  CPUReadMemory(u32);
extern u32  CPUReadHalfWord(u32);
extern u16  CPUReadHalfWordSigned(u32);
extern u8   CPUReadByte(u32);
extern void CPUWriteMemory(u32, u32);
extern void CPUWriteHalfWord(u32, u16);
extern void CPUWriteByte(u32, u8);
extern void CPUUpdateCPSR();
extern void jitTracePc(u32);          // JIT_PC_TRACE only
```

已核实的签名细节：

- `CPUReadHalfWord` 返回 **u32**，不是 u16；地址为奇数时
  `CPUReadHalfWordWrapped` 返回的是 32 位旋转结果（低字节会出现在
  bit24-31），所以慢路径不能无条件 `uxth`。
- `CPUReadHalfWordSigned` 返回 u16（值本身已按 `(s8)` 规则变成 16 位符号
  形态），慢路径末尾要 `sxth` 把它扩到 32 位。
- `CPUReadByte`/`CPUWriteByte` 是 u8 参数/返回，A64 调用前用 `uxtb` 或
  直接 `ldrb` 装箱即可。
- `armState` 是 `bool8`（0/1），不是 `bool`，所以 `BX` 里用 `strb` 写 0/1。

### 4.2 本后端会用到的指令与签名（均在 `a64_emitter.h` 中）

```
add/sub/adds/subs/adc/adcs/sbc/sbcs   (Gp, Gp, Gp)
add/sub/adds/subs                     (Gp, Gp, Imm)
add/sub/adds/subs                     (Gp, Gp, Gp, Imm)   // 带 shift
and_/orr/eor/bic/orn/ands             (Gp, Gp, Gp)
and_/orr/eor/bic                      (Gp, Gp, Imm)
mvn/neg/negs                          (Gp, Gp)
mov                                   (Gp, Gp) / (Gp, Imm)
movz/movk                             (Gp, Imm, Imm) /*LSL 0/16/32/48*/
movz/movk                             (Gp, Imm)      /*LSL #0*/
cmp/cmn/tst                           (Gp, Gp) / (Gp, Imm)
cset                                  (Gp, CondCode)
csel                                  (Gp, Gp, Gp, CondCode)
lsl/lsr/asr/ror                       (Gp, Gp, Imm) / (Gp, Gp, Gp)
lslv/lsrv/asrv/rorv                   (Gp, Gp, Gp)
clz                                   (Gp, Gp)
mul                                   (Gp, Gp, Gp)
madd/msub                             (Gp, Gp, Gp, Gp)
umull/smull                           (Gp, Gp, Gp)
umaddl/smaddl                         (Gp, Gp, Gp, Gp)
sxtb/sxth/uxtb/uxth                   (Gp, Gp)
ldr/str/ldrb/strb/ldrh/strh           (Gp, Mem)
ldp/stp                               (Gp, Gp, Mem)
cbz/cbnz                              (Gp, Label)
tbz/tbnz                              (Gp, Imm, Label)
b                                     (Label) / (CondCode, Label)
bl                                    (Label)
br/blr/ret                            (Gp)
adr/adrp                              (Gp, Label/Imm)       // 本后端暂不用
nop
```

注意：

- 🔴 **`mvn` 与 MSVC 的 ARM64 头冲突**：`arm64_neon.h`（经 `<intrin.h>` 在
  AArch64 目标下被包含）定义了函数式宏 `#define mvn(src) neon_not(src)`。
  写 `a.mvn(w7, w7)` 会被预处理器展开成 "too many arguments provided to
  function-like macro invocation"。实测只有 `mvn` 一个宏与 asmjit 的方法名
  相撞（扫过 `arm64_neon.h` 的全部函数式宏）。修法二选一：
  - 在所有系统头之后加 `#ifdef mvn` / `#undef mvn` / `#endif`（推荐，已实测通过）；
  - 或干脆不用 `mvn`，改写 `orn wD, wzr, wS`（实测 `errors=0`，语义等价）。
  x64 构建不会遇到这个坑，所以 **x64 后端里看不到它**。
- 逻辑 `and` 在 asmjit 里叫 **`and_`**（避免 C++ 关键字）。
- `b` 由 `ASMJIT_INST_1cc`（`a64_emitter.h:45-67`）生成，除了
  `b(Label)` / `b(CondCode, Label)`，每个条件都有简写：
  `b_eq/b_ne/b_hs/b_lo/b_mi/b_pl/b_vs/b_vc/b_hi/b_ls/b_ge/b_lt/b_gt/b_le/b_al`。
  `emitCondSkip()` 是 switch，用 `b(cc, label)` 查表更直接；手写分支可以用简写。
- `cset/csel` 的最后一个参数直接传 `CondCode` 枚举。
- **`ret` 只有 `ret(Gp)` 一个重载**（`a64_emitter.h:248`），没有无参
  `ret()`；返回必须写 `a->ret(a64::x30)`。`br/blr` 同理都要求寄存器。
- `mov(Gp, Imm)` **会自动展开成多条指令**（2026-11-05 实跑 asmjit 编码器
  核实：`mov w3,#0x12345678` → 8 字节 2 条，`mov x19,#0x123456789ABC` →
  12 字节 3 条，都是 `errors=0`）。早先"其它常量会让发射失败"的说法是错的。
  但这**只对 `mov` 生效**：`add/cmp/tst/and/bic` 的立即数操作数不会展开，
  超出编码范围会走 `ErrorHandler`（见 4.3 与 4.4）。
  `mov(Gp, Gp)` 是 `ORR rd, wzr/xzr, rm` 的别名，不碰标志。
- `ldr/str` 的寄存器偏移只能编码**一个** shift/extend：32 位 index 必须用
  `uxtw`/`sxtw`，缩放直接写进扩展量里——`uxtw(2)` 表示 `index * 4`，
  `uxtw(0)` 表示不缩放。**不能**"先 `lsl` 再 `uxtw(0)`"：`Shift` 只有一个
  op 和一个 value，没有两步组合。缩放值只允许 0 或 `log2(访问宽度)`
  （字用 2、半字用 1、字节用 0），其它值汇编器拒绝。
- A64 的 `ldr/str` 立即数偏移有符号/无符号限制，实际用到的都是很小的正偏移。
- `a->offset()` 返回当前代码缓冲区字节偏移；A64 指令定长 4 字节，标签修复
  不会改变长度，所以序言结束处的 `offset()` 就是 `chainEntry` 的准确偏移。
  `rt->add()` 不会重排代码（x64 后端已在 `-jitverify` 里验证过这一假设），
  但 A64 仍要在第一次链式命中时用日志确认
  `fn + chainEntryOffset == chainEntry`。

### 4.3 发射失败必须显式捕获

asmjit 的 `BaseEmitter` **没有** `lastError()` / `hasErrors()`，`CodeHolder`
也没有错误计数：一条 `_emitI()` 失败只返回 `Error`。而 x64 的
`BlockEmitter::emitXxx()` 全是 `void`，直接照抄会把失败悄悄丢掉，最后得到
"少了几条指令"的代码——逐指令周期和 PC 都会错，且只有 `-jitverify` 之外的
场景很难发现。必须在每个 `Assembler` 上挂 `ErrorHandler`：

```cpp
#include <asmjit/core/error_handler.h>   // core.h 已包含

class JitA64ErrorHandler : public asmjit::ErrorHandler
{
public:
	asmjit::Error first = asmjit::Error::kOk;
	unsigned	  count = 0;

	// 基类有默认实现，但我们必须覆盖它才能看到错误。
	void handle_error(asmjit::Error err, const char *message,
					  asmjit::BaseEmitter *origin) override
	{
		(void)origin;
		if (count++ == 0)
		{
			first = err;
			if (getenv("VBA_JIT_ASM_ERRORS"))
				fprintf(stderr, "[jit-a64] emit error %u: %s\n",
						(unsigned)err, message ? message : "");
		}
	}
};
```

编译入口：

```cpp
asmjit::CodeHolder code;
if (code.init(rt->environment()) != asmjit::Error::kOk)
	return false;

asmjit::a64::Assembler a(&code);
JitA64ErrorHandler eh;
a.set_error_handler(&eh);

BlockEmitter emitter(&a, block, out);
u16 insns = 0;

// compile() 内部所有 a->xxx() 的返回值都可以忽略，统一在这里判。
if (!emitter.compile(&insns) || insns == 0 || eh.count != 0 ||
	a.finalize() != asmjit::Error::kOk)
	return false;

JitNativeFn fn = nullptr;
if (rt->add(&fn, &code) != asmjit::Error::kOk || !fn)
	return false;
```

要点：

- `set_error_handler()` 在 `BaseEmitter` 上（`core/emitter.h:501`），
  `reset_error_handler()` 是它的空指针包装。
- `ErrorHandler::handle_error()` 是虚函数、基类有默认实现
  （`core/error_handler.h:218`），不是纯虚；覆盖后 asmjit 在调用前已把
  emitter 恢复到一致状态，所以这里只记录、不 `longjmp`。
- 返回 false 后 `JitCache` 会回退到 IR 解释器，失败模式仍然只是"更慢"。
- `a.finalize()` 还会做 label 修复等尾部工作，必须检查返回值；A64 的标签
  都在同一个 section，失败通常意味着前面的发射已经出过错。
- 错误捕获必须**无条件**开启；只有日志输出可以用
  `VBA_JIT_ASM_ERRORS` 之类的开关控制。

### 4.4 已实跑的 API 与立即数核对（2026-11-05）

4.1–4.3 的 API 清单不能只靠读头文件。本机写了两个探针，把本文用到的**每一种**
调用形态和常量都过了一遍；结论如下（脚本在 `~/.workbuddy/scratch/`）：

**探针 A：语法/API 检查**（`a64-api-probe.cpp`）。在 x64 宿主上让 clang 以
AArch64 为目标解析 MSVC 头：

```
clang++ -fsyntax-only --target=aarch64-windows-msvc -std=c++20 -DASMJIT_STATIC \
  -I third_party/asmjit \
  -I "C:/Program Files/Microsoft Visual Studio/18/Insiders/VC/Tools/MSVC/14.52.37025/include" \
  -I "C:/Program Files (x86)/Windows Kits/10/Include/10.0.28000.0/ucrt" \
  -I ".../shared" -I ".../um" a64-api-probe.cpp
```

覆盖 `stp ptr_pre / ptr_post`、`movz/movk` 带 lsl、`ldrb/strb`、`ldr/str` 寄存器
偏移（`uxtw(0/1/2)`）、`adds/subs/adcs/sbcs`、`cset(CondCode)`、`bic/orr/and_`、
`clz/ror/mvn/undef 后的 orn`、`mul/madd/umull/smull/umaddl/smaddl`、
`cmp/cbz/tbz/b(cc,label)/blr`、`ldp/ret(x30)`、`new_label()/bind()/finalize()`。

- **加 `#undef mvn` 之前编译失败**，加了之后 0 错误（见 4.2 的宏冲突）。
- `new_label()` 是 snake_case；`newLabel()` 不存在（实测报 C2039）。
- MSVC `arm64_neon.h` 在 clang 下会刷 5 条 `-Wignored-attributes` 警告
  （`__declspec(intrin_type)`），与代码无关，加 `-Wno-ignored-attributes` 即可。

**探针 B：编码器实跑**（`a64-encode-probe.cpp`）。asmjit 的编码器与宿主架构
无关，所以在 x64 上把 `Environment` 显式初始化成 AArch64，就能真的发射并让
`ErrorHandler` 报错：

```cpp
Environment env;
env.init(Arch::kAArch64);          // 不是 Environment::host()
CodeHolder code; code.init(env);
a64::Assembler a(&code);
a.set_error_handler(&eh);          // 见 4.3
...
printf("%u errors, %zu bytes", eh.count, code.code_size());
```

用 MSVC x64 `cl.exe /std:c++20 /DASMJIT_STATIC` 编、链 `asmjit.lib` 后**实跑**。
下表是原始输出（`errors` 来自 `ErrorHandler`，`size` 是 `code_size()`）：

| 发射形态 | errors | size | 结论 |
| --- | --- | --- | --- |
| `and w3,w3,#15` / `bic w6,w6,#3` / `#1` / `orr w7,w7,#1` | 0 | 4 | 常用掩码可直接编码 |
| `and w3,w3,#0xFFFFFF00` / `#0x00FFFFFF` | 0 | 4 | 单段掩码 |
| `and w3,w3,#0xFFFF00FF` / `#0xFF00FFFF` | 0 | 4 | **两段掩码也能编码**（旧笔记写反了） |
| `and w3,w3,#0xFF<<(0/8/16/24)` 四个 | 0 | 4 | MSR 字段掩码 |
| `and w3,w3,#0x0F000000` | 0 | 4 | IO 区域掩码 |
| `mov w3,#0x04000000` | 0 | 4 | 单条 `movz #0x400,lsl#16` |
| `mov w3,#0x040000BA` | 0 | 8 | `mov` 自动展开 2 条 |
| `mov w3,#0x12345678` | 0 | 8 | `mov` 自动展开 2 条 |
| `mov x19,#0x123456789ABC` | 0 | 12 | `mov` 自动展开 3 条 |
| `mov w3,#0xFFFFFFFE / #0xFFFFFFFC / #0x80000000` | 0 | 4 | 常用常量 |
| `add w3,w3,#4095` / `#4096` | 0 | 4 | 12 位 + 可选 lsl#12 |
| **`add w3,w3,#0x04000000`** | **1** | 0 | **ADD 立即数不展开** |
| **`cmp w3,#0x04000000`** | **1** | 0 | **CMP 立即数不展开** |
| `tst w3,#0x04000000` | 0 | 4 | 逻辑立即数可编码（但语义是位测试，不是比较） |
| `orn w7,wzr,w7` | 0 | 4 | `mvn` 的等价替代 |
| `ldr x9,[x27,w8,uxtw 0/3]` | 0 | 4 | 64 位访问可用缩放 3 |
| `ldr w4,[x26,w3,uxtw 2]` | 0 | 4 | wait 表用缩放 2 |
| **`ldr w4,[x26,w3,uxtw 3]`** | **1** | 0 | **32 位访问不能用缩放 3** |
| `ldrh w11,[x9,w10,uxtw 1]` | 0 | 4 | 半字缩放 1 |
| `strb w3,[x25,#1]` / `ldr w5,[x19,#176]` | 0 | 4 | 小立即数偏移 |
| `movz x19,#imm,lsl#48` / `movk x19,#imm,lsl#32` | 0 | 4 | 绝对地址装入 |
| `b.al label` / `cbz` / `tbz` / `blr` / `ret(x30)` | 0 | 4 | 分支与调用 |
| `and w4,w23,#0x0F000000; cmp w4,#0x04000000` | **1** | 4 | **IO 检查不能直接这么写** |
| `and ...; mov w5,#0x04000000; cmp w4,w5` | 0 | 12 | 正确写法，3 条 |

**落地规则**（从这张表直接得出的）：

1. 任意常量用 `a.mov(wDst, Imm(v))` 装，别自己数 `movz/movk`；但**地址装入**
   仍建议显式 `movz/movk`（2.5），因为要把 64 位地址写进 `x` 寄存器，而且
   要能预测序言长度。
2. `add/sub/cmp/cmn` 的立即数**只接受 12 位（可带 lsl#12）**；区域常量、
   大掩码一律先 `mov` 到 scratch 再用寄存器形式。
3. `and_/orr/bic/tst` 的立即数是 A64 位掩码，能表达的范围比直觉大；
   但**不要依赖"看起来像两段"就判定不能编码**，用 4.3 的 ErrorHandler 兜底
   （发射失败 → 整个块回退 IR，安全）。
4. `ldr/str` 的 `uxtw(n)` 里 `n` 必须等于 `log2(访问宽度)`：64 位 → 3，
   32 位 → 2，半字 → 1，字节 → 0。写错会在发射期报错，不是运行期。

### 4.5 `BlockEmitter` 骨架（照 `JitEmit.cpp:256-367` 改）

后端只需要一个发射器类；它与宿主 ISA 无关的部分原样保留，只有 `a` 的类型和
寄存器访问 helper 换掉。

```cpp
struct Carry { enum Kind { kKeep, kConst, kGlobal } kind; u32 value; };

class BlockEmitter {
public:
    BlockEmitter(a64::Assembler *a, const JitBlock *b, JitCompiledBlock *out);
    bool compile(u16 *outCount);
    u32  chainEntryOffsetOf() const { return chainEntryOffset; }
    int  chainSlotsUsed()    const { return nChain; }
    bool usesMemoryFrame()   const { return usesMem; }   // A64 恒 false

private:
    a64::Assembler   *a;
    const JitBlock   *b;
    JitCompiledBlock *outBlock;
    bool  thumb;  u32 pc;  bool usesMem;
    Label epilogue, chainBail;
    u16   count;  u32 chainEntryOffset;  int nChain;

    // 桩不能内联发射：正常路径不能"落进" NEED_INTERP / BUDGET 里。
    // 数组按 JIT_MAX_INSNS*2 开，因为一条指令最多同时需要两个标签。
    Label needLabel[JIT_MAX_INSNS * 2];  u32 needPc[JIT_MAX_INSNS * 2];  int needCount;
    Label budgetLabel[JIT_MAX_INSNS * 2];u32 budgetPc[JIT_MAX_INSNS * 2];int budgetCount;
    bool  traceDone;

    Mem gr(int r, int size = 4) { return a64::ptr(a64::x19, r * 4, size); }
    int  step() const { return thumb ? 2 : 4; }

    void emitPrologue(); void emitEpilogue();
    void emitExitImm(u32 nextPc, u32 status);
    void emitExitReg(const Gp &nextPc, u32 status);
    void emitChainExit(u32 nextPc);
    void queueNeedInterp(const Label &, bool used);
    void emitNeedInterpStubs(); void emitBudgetStubs(); void emitChainBail();
    void checkBudget(u32 nextPc);           // 不要实现寄存器版本

    void setPc(); void addFetch(u8); void addConst(int);
    void addWait(const Gp &addr, const int32_t *table);
    void emitCondSkip(u8 cond, const Label &skip);
    void emitIoRangeCheck(u32 size, const Label &ni);
    void emitOperand2(const JitInsn &, const Gp &, Carry *);
    void emitOffset(const JitInsn &, const Gp &);  void writeCarry(const Carry &);
    void emitNZ(const Gp &, u8 mask);  void emitFlagsNative(bool, u8);
    void emitFlagsLogical(const Gp &, const Carry &, u8);
    void emitFlagsAdd(...); void emitFlagsSub(...);   // A64 用 cset，形式可简化

    void emitAlu / emitMul / emitLoadStore / emitBlockTransfer / emitSwp /
         emitBranch / emitBx / emitMrs / emitMsr;
    void emitOne(const JitInsn &); void emitBody(const JitInsn &); void emitTrace();
};
```

`compile()` 的**顺序不能改**（`JitEmit.cpp:2170-2228`）：

1. 扫最长可编译前缀 `n`（逐条 `nativeSupported`），同时把 `touchesMemory()`
   记进 `usesMem`；`n == 0` 直接 false。
2. 重置 `epilogue` / `chainBail` 两个 Label，`needCount/budgetCount/nChain` 归零。
3. `emitPrologue()`。
4. **`chainEntryOffset = (u32)a->offset()`** —— 必须正好在序言最后一条之后。
5. `cmp w20,w21; b.ge chainBail`（链式入口的预算守卫）。
6. 循环：`pc = guestPc + i*step()` → `needsPcBefore(in)` 时 `setPc()` → `emitOne(in)`。
7. `emitChainExit(guestPc + n*step())`（前缀跑完的落点）。
8. `emitEpilogue()`。
9. 依次绑定 `need` 桩 / `budget` 桩 / `chainBail` —— **都在尾声之后**。
10. `*outCount = n`。

三个容易写错的地方：

- `emitOne()` 的条件分支（`JitEmit.cpp:2118-2142`）：`cond != 0xE && op != JOP_BRANCH`
  时建 `skip` / `after` 两个标签 → `emitCondSkip(cond, skip)` → `emitBody` →
  `b after` → `bind skip` → **`addFetch(in.fetchCycles)` + `checkBudget(pc+step())`**
  （条件为假也要付 fetch 周期）→ `bind after`。少掉这个 `addFetch` 会让条件为假的
  指令少算周期，几帧后 IRQ 落点就漂。
- `emitBody()` 里对 `JOP_STORE/STM/PUSH/SWP` **不**在开头调 `emitTrace()`，因为
  它们可能先因 IO 范围检查回退；trace 必须放在守卫**之后**。其余指令在开头调用，
  `traceDone` 每个 `emitOne` 重置，保证一条指令只 trace 一次。
- 所有可能运行时 bail 的标签都必须 `queueNeedInterp(ni, true)` 登记；
  漏一个会在 `finalize()` 或标签绑定时留下未绑定跳转，直接跳进垃圾。

---

## 5. 逐指令语义移植（以 x64 后端为准）

### 5.1 编译前缀与拒绝清单

`nativeSupported(in, thumb)` 必须与 x64 **逐条一致**（`JitEmit.cpp:379-458`）：

| 指令 | 拒绝条件 | 原因 |
|---|---|---|
| `JOP_ALU` | `d == 15 && flagMask != 0` | 写 PC 且 S=1 会走 `CPUSwitchMode/CPUUpdateFlags`，改写 CPULoop 局部量 |
| `JOP_MUL` | `d == 15` 或 `(imm & 0xF) == 15` | 长乘法的 RdLo/RdHi 写 PC，不是分支 |
| `JOP_LOAD` | `d == 15 && (thumb || sub != MEM_WORD || !(flags & JF_PRE))` | 只有 ARM 前索引字加载到 PC 是一条分支；其余是"绕一条" |
| `JOP_LOAD` | `n == 15 && (flags & JF_WRITEBACK)` | 把 PC 写回基址，不是分支 |
| `JOP_STORE` | `n == 15 && (flags & JF_WRITEBACK)` | 同上 |
| `JOP_LDM/STM` | `n == 15 && (flags & JF_WRITEBACK)` | 同上 |
| `JOP_SWP` | `d == 15` | 字 SWP 到 PC 不分支 |
| `JOP_MRS` | `d == 15` | 写 PC 不分支 |
| `JOP_MSR` | `sub == 0`（CPSR 形式） | 只有 SPSR 形式可安全编译 |
| `JOP_SWI/UNDEF/END` | 总是拒绝 | 必须由解释器处理 |

另有调试开关 `VBA_JIT_OPS=<mask>`：按 `1 << JitOp` 的位掩码限制可编译的 op
（`nativeSupported` 开头，x64 `JitEmit.cpp:382-390`）。A64 照抄，便于二分
定位错误指令类别。

`needsPcBefore(in)` 也必须一致（`JitEmit.cpp:465-490`）：

- `JOP_ALU`：读 `n == 15`、读 `m == 15`、或寄存器移位且计数寄存器为 15 → 需要；
  立即数形式且 n/m 都没读 PC → 不需要。
- `JOP_MUL`：`n == 15 || m == 15`。
- `JOP_BRANCH`：不需要（目标由 pc 在发射期算好）。
- 其他（含 BX、访存、LDM/STM/PUSH/POP、SWP、MRS/MSR）：都需要。

### 5.2 PC 值

`setPc()`：`reg[15] = pc + (thumb ? 4 : 8)`。

- ARM/Thumb 在指令执行期间都让 `reg[15]` 指向"当前指令 + 4/8"，内存 helper
  会读它来生成 open-bus 值。
- ALU 的目标是 PC（S=0）时：结果按 Thumb `& ~1`、ARM `& ~3` 对齐，然后直接
  以 `JITX_DONE` 退出，`nextPc` 就是这个对齐后的值；**不要把结果写进 reg[15]**。

### 5.3 周期记账

- `addFetch(fetchCycles)`：
  - Thumb：先 `add w20, w20, #fetchCycles`，再查 `memoryWaitFetch[pc>>24]`；
  - ARM：只查 `memoryWaitFetch32[pc>>24]`；
  - `pc>>24` 是编译期常量（pc 已知），索引是常量，表项仍是运行时读。
- `addWait(addr, table)`：运行时 `addr>>24 & 15` 查表，见 3.2。
- `addAccess32 = addWait(memoryWait32)`，`addAccess16 = addWait(memoryWait)`，
  `addSeq32 = addWait(memoryWaitSeq32)`。
- 每个 IR 指令的周期增量（与 x64 完全一致）：

| op | 增量 |
|---|---|
| ALU | `addFetch + (isRegShift ? 1 : 0)` |
| ALU 写 PC | `addFetch + (isRegShift?1:0) + (thumb?1:0)` |
| MUL | `addFetch + bucket + (thumb?1:(mul/umull/smull?2:3))` |
| LOAD | `addFetch + (thumb?0:3) + access32/16`；ARM 字加载到 PC 再 `+2` |
| STORE | `addFetch + (thumb?0:(half?4:2)) + access32/16` |
| PUSH | `addFetch + 每个寄存器 1 + 首次 access32/其余 seq32` |
| POP | `每个寄存器 2 + access32/seq32`；PC 槽只 `access32/seq32` 不加 2 |
| LDM(Thumb) | `addFetch + 每寄存器 2 + access32/seq32` |
| LDM(ARM) | `addFetch + 2 + 每寄存器 1（PC 槽 +2）+ access32/seq32` |
| STM | `addFetch + (thumb?0:2) + 每寄存器 1 + access32/seq32` |
| SWP | 只有 `addFetch`（V7 不为 SWP 计数据周期） |
| BRANCH(ARM) | 命中 `addFetch+3`，未命中 `addFetch` |
| BRANCH(Thumb) | 条件命中固定 `3`，无条件命中 `addFetch`，未命中 `addFetch` |
| BX | `addFetch + (thumb?0:3)` |
| MRS/MSR | `addFetch` |
| 条件为假的任何指令 | `addFetch`，然后按下一 PC 检查预算 |

顺序也很重要：x64 是"先算结果/写回寄存器，再读标志，再 addFetch/checkBudget"。
A64 必须保持同样顺序，因为 `addFetch` 会加载内存并写 `w20`，会改 A64 标志。

### 5.4 条件执行

`emitOne()`（`JitEmit.cpp:2127-2151`）：

- `cond != 0xE && op != JOP_BRANCH` 时，生成
  `条件为假 -> skip`；真路径执行 `emitBody` 后跳到 `after`；`skip` 路径只
  `addFetch + checkBudget(pc+step)`。
- 条件为假仍然要计 fetch 周期（解释器在分派前就返回 base 周期）。
- 条件判断全部基于 guest 标志字节，不使用 A64 宿主标志跨指令保存。

`emitCondSkip(cond, skip)` 的"条件为假时跳到 skip"映射：

| ARM cond | 语义 | 生成的判定 |
|---|---|---|
| 0 EQ | Z==1 | `ldrb w3,Z; cmp w3,#0; b.eq skip`（字节为 0 = 假） |
| 1 NE | Z==0 | `... b.ne skip`（字节非 0 = 假） |
| 2 CS/HS | C==1 | `ldrb C; cmp 0; b.eq skip` |
| 3 CC/LO | C==0 | `... b.ne skip` |
| 4 MI | N==1 | `ldrb N; cmp 0; b.eq skip`（N==0 为假） |
| 5 PL | N==0 | `... b.ne skip` |
| 6 VS | V==1 | `ldrb V; cmp 0; b.eq skip` |
| 7 VC | V==0 | `... b.ne skip` |
| 8 HI | C&&!Z | `C==0 -> skip; Z!=0 -> skip` |
| 9 LS | !C||Z | `C==0 -> ok; Z==0 -> skip; ok:` |
| A GE | N==V | `ldrb N; ldrb V; cmp wN,wV; b.ne skip` |
| B LT | N!=V | `... b.eq skip` |
| C GT | !Z && N==V | `Z!=0 -> skip; N!=V -> skip` |
| D LE | Z || N!=V | `Z!=0 -> ok; N==V -> skip; ok:` |
| E AL | 总是真 | 不发指令 |
| F NV | 总是假 | `b skip` |

注意：`ldrb`/`cmp`/`b` 不写内存，也不要求 A64 标志跨指令有效。

### 5.5 Operand2 与进位

`emitOperand2(in, dstW, Carry*)`（`JitEmit.cpp:840-977`）：

- `JF_IMM`：
  - `shift != SHIFT_IMM_ROT`：`emitMovImm32(dst, in.imm)`；carry = keep。
  - `SHIFT_IMM_ROT`：编译期算 `rot = (imm>>8)&0xF`、`imm8 = imm&0xFF`；
    `rot == 0` 时值 = `imm8`、carry = keep；否则值 = `ror(imm8, rot*2)`，
    carry = `(imm8 >> (rot*2-1)) & 1`（常量）。
- 寄存器：
  - `SHIFT_NONE`：`ldr dst, [x19, #rm*4]`。
  - `LSL_IMM`：`ldr`；`imm==0` 直接返回；否则
    `lsr w3,dst,#(32-imm); and w3,#1; strb w3,[x28]`，`lsl dst,dst,#imm`。
  - `LSR_IMM`：`imm==0` → carry=bit31，结果 0；否则
    `lsr w3,dst,#(imm-1); and #1; strb`，`lsr dst,dst,#imm`。
  - `ASR_IMM`：`imm==0` → carry=bit31，`asr dst,#31`；否则
    `asr w3,dst,#(imm-1); and #1; strb`，`asr dst,dst,#imm`。
  - `ROR_IMM`：`imm==0` 是 RRX：
    `and w3,dst,#1; strb`；`lsr dst,dst,#1; ldrb w4,[x25,#C]; lsl w4,#31;
    orr dst,dst,w4`。否则 `ror dst,dst,#imm; lsr w3,dst,#31; strb`。
  - 寄存器移位（`SHIFT_*_REG*`）：调用 `jitShiftReg(v, raw, carryIn, kind)`。
    - A64 调用：`ldr w0,[x19,#rm*4]; ldrb w1,[x19,#(imm&0xF)*4];
      ldrb w2,[x25,#C]; mov w3,#kind; blr helper; mov dst,w0`。
    - kind 对照：
      - `SHIFT_LSL_REG` → Thumb `SHK_LSL_T`，ARM `SHK_LSL`
      - `SHIFT_LSR_REG` → Thumb `SHK_LSR_T`，ARM `SHK_LSR`
      - `SHIFT_ASR_REG` → Thumb `SHK_ASR_T`，ARM `SHK_ASR`
      - `SHIFT_ROR_REG_V7` → `SHK_ROR_V7`
      - 其余 ROR → `SHK_ROR_T`
    - 返回后 carry = global（`gJitShiftCarry` 可能是 0/1/0xFF）。
  - `default`：`ldr dst,[x19,#rm*4]`。

`jitShiftReg(u32 v, u32 raw, u32 carryIn, u32 kind)` 的语义必须逐行照抄
`JitEmit.cpp:115-193`；关键点：

- 先把 `gJitShiftCarry = 0xFF`。
- `SHK_LSL/LSR/ASR`（ARM 寄存器移位）：`raw == 0` 原样返回且不改 C；
  其余按 `raw & 31` 移位并写 C。ARM 的 `raw >= 32` 在调用前已被截断到
  `JITX_NEED_INTERP`，所以这里不会遇到。
- `SHK_*_T`（Thumb 格式 4）：`raw == 0` 原样返回且不改 C；`0 < raw < 32`
  正常移位并写 C；`raw == 32` 时 LSL/LSR 结果为 0（C 分别是 `v & 1` 和
  `bit31`），**ASR 结果是全符号填充（`0xFFFFFFFF`/0）、C = `bit31`**；
  `raw > 32` 时 LSL/LSR 结果为 0、C=0，**ASR 仍然是全符号填充、C=`bit31`**。
- `SHK_ROR_T`：`raw == 0` 原样返回；否则走 ROR。
- `SHK_ROR_V7`：**任何** `raw & 31 == 0`（包括 raw=0/32/64…）都要写 C =
  原 bit31；这是 V7 ARM `ror Rd,Rm,Rs` 与 Thumb 的差异。
- `SHK_ROR_T/SHK_ROR_V7` 的 `n = raw & 31`；`n==0` 时返回原值、C=bit31；
  否则结果 = `(v>>n)|(v<<(32-n))`，C = 结果 bit31。

`emitOffset(in,dst)`（`JitEmit.cpp:981-1022`）用于访存偏移，语义与 Operand2
不同：

- `LSL_IMM`：`imm` 为 0 时不移位；
- `LSR_IMM`：`imm == 0` 结果 0（不是 32 位）；
- `ASR_IMM`：`imm == 0` 全符号扩展（`asr #31`）；
- `ROR_IMM`：`imm == 0` 是 RRX（`lsr #1` + 把 C 放到 bit31）；
- 其他：直接取寄存器值（ARM 寄存器偏移只会被解码成上面的立即数移位形式）。

`writeCarry(carry)`（`JitEmit.cpp:817-838`）：

- `kKeep`：不发指令。
- `kConst`：`movz w3,#0/1; strb w3,[x25,#C]`。
- `kGlobal`：`ldrb w3,[x28]; cmp w3,#0xFF; b.eq done; strb w3,[x25,#C]; done:`。

### 5.6 标志位

这是 A64 与 x64 差异最大的地方，也是必须逐条验证的地方。

#### ADD / SUB / ADC / SBC / RSC / CMN / CMP

A64 原生标志与 ARM 的语义一致：

- `adds/adcs` 的 C = 无符号进位；`subs/sbcs` 的 C = **无借位**，
  正好等于 ARM 的 C（x64 的 CF 是借位，所以才要 `setae`）。
- N/Z/V 同样直接对应。
- 因此统一用 A64 标志 + `cset`：

```
adds/subs/adcs/sbcs  w6, lhs, rhs     // 或 wzr 用于 CMP/CMN
cset w3, mi ; strb w3, [x25, #N]
cset w3, eq ; strb w3, [x25, #Z]
cset w3, hs ; strb w3, [x25, #C]      // add 和 sub 都用 hs
cset w3, vs ; strb w3, [x25, #V]
```

只在 `mask` 要求时才发对应的 `cset/strb`。`cset` 不改标志，连续四条安全。

ADC/SBC/RSC 的 C 注入：

```
ldrb w3, [x25, #C]
cmp  w3, #1                  // C_FLAG 是 0/1，cmp #1 后 A64 C = 原 ARM C
adcs w6, lhs, rhs            // ADC
sbcs w6, lhs, rhs            // SBC = lhs - rhs - !C
sbcs w6, rhs, lhs            // RSC = rhs - lhs - !C
```

`C_FLAG` 是 `bool8`，只可能是 0/1；`cmp w3,#1` 后：
- C=1 当且仅当 `w3 >= 1`（无符号），即原 C=1；
- 所以 adcs/sbcs 的进位输入正确。

与 `arm-new.h` 的公式对照（已核对）：

```
ADDCARRY(a,b,c)    = (~a31 & ~b31) | (~a31 & c31) | (~b31 & c31)
ADDOVERFLOW(a,b,c) = (~a31 & ~b31 & c31) | (a31 & b31 & ~c31)
SUBCARRY(a,b,c)    = (~a31 & b31) | (~a31 & c31) | (b31 & c31)
SUBOVERFLOW(a,b,c) = (~a31 & b31 & c31) | (a31 & ~b31 & ~c31)
```

其中 a/b 是两个源操作数、c 是最终结果（对 ADC/SBC 也包含进位/借位）。
A64 的 NZCV 正是这四条公式的硬件版本，所以 `adcs/sbcs` 不需要像 x64 那样
用 64 位重算。

#### 逻辑类：AND/EOR/ORR/BIC/MVN/TST/TEQ，以及 MOV

- 结果先算到 `w6`，**不用** `ands`。
- `mask & (N|Z)`：`cmp w6,#0; cset mi -> N; cset eq -> Z`。
- `mask & C`：调用 `writeCarry(carry)`（来自 Operand2/移位器）。
- V 不动。
- 这与 `emitFlagsLogical()` 完全一致（`JitEmit.cpp:1071-1076`）。

### 5.7 ALU

`emitAlu()`（`JitEmit.cpp:1151-1276`）的 A64 版本：

1. 寄存器移位且是 ARM 的 `LSL/LSR/ASR` 时，先检查计数寄存器：
   ```
   ldrb w3, [x19, #(imm & 0xF)*4]
   cmp  w3, #32
   b.hs needInterp             // raw >= 32，交给解释器
   ```
   （Thumb 不检查，helper 自己处理；ROR 不检查。）
   注：解释器读的是 `reg[rm].B.B0`（低字节），x64 的守卫却比较整个 32 位
   dword。A64 用 `ldrb` 比较低字节即可，既和解释器一致，也和
   `jitShiftReg` 收到的 `raw` 一致；x64 的 dword 版本只是更保守（会多回退
   一些本来能编译的指令），不影响正确性。
2. `emitOperand2(in, w4, &carry)` → rhs 在 `w4`。
3. 非 MOV/MVN 时 `ldr w5,[x19,#n*4]`；`JF_PC_ALIGN4` 时 `bic w5,w5,#3`。
4. 算结果到 `w6`：
   - AND/EOR/ORR：`and_/eor/orr w6,w5,w4`
   - BIC：`bic w6,w5,w4`（x64 是 `~rhs & lhs`，A64 BIC 正是 `lhs & ~rhs`）
   - MVN：`mvn w6,w4`
   - MOV：`mov w6,w4`
   - SUB/CMP：`subs w6,w5,w4`（mask==0 时可 `sub`）
   - RSB：`subs w6,w4,w5`
   - ADD/CMN：`adds w6,w5,w4`（mask==0 时可 `add`）
   - ADC：注入 C 后 `adcs w6,w5,w4`
   - SBC：注入 C 后 `sbcs w6,w5,w4`
   - RSC：注入 C 后 `sbcs w6,w4,w5`
5. `toPc`（`d==15`，只可能是 S=0）：
   ```
   bic w6, w6, #(thumb ? 1 : 3)
   addFetch
   addConst(extra)
   addConst(thumb ? 1 : 0)
   emitExitReg(w6, JITX_DONE)      // 不写 reg[15]
   ```
6. 否则：`!compareOnly` 时 `str w6,[x19,#d*4]`。
7. 按 5.6 写标志（算术类用 A64 标志；逻辑类用 cmp+writeCarry）。
8. `addFetch`、`addConst(extra)`、`checkBudget(pc+step())`、登记 needInterp 桩。

`compareOnly = (sub >= ALU_TST && sub <= ALU_CMN)`；这类不写 Rd。

### 5.8 乘法

`emitMul()`（`JitEmit.cpp:1282-1412`）：

- `w3 = reg[m]`（Rs，既是乘数也是计时分档的来源）。
- 结果（`w6`/`x6`）：
  - `MUL_MUL`：Thumb 的旧值是 `reg[d]`，ARM 是 `reg[n]`；`mul w6, old, w3`。
  - `MUL_MLA`：`madd w6, w_n, w3, w_lo`（`lo = imm & 0xF`）。
  - `MUL_UMULL`：`umull x6, w_n, w3`；低位 `w6`，高位 `x6>>32`。
  - `MUL_SMULL`：`smull x6, w_n, w3`。
  - `MUL_UMLAL`：先把旧 64 位拼进 `x8`
    （`ldr w8,[x19,#d*4]; lsl x8,x8,#32; ldr w7,[x19,#lo*4]; orr x8,x8,x7`），
    再 `umaddl x6, w_n, w3, x8`。
  - `MUL_SMLAL`：同上，用 `smaddl`。
- 写回：长乘法先 `str w6,[x19,#lo*4]`，再 `lsr x6,x6,#32` 后
  `str w6,[x19,#d*4]`；短乘法只写 `d`。
- 标志：
  - 短：N=bit31(d)，Z=(d==0)。
  - 长：N=bit31(Hi)，Z=((Hi|Lo)==0)。
- 计时分档：
  ```
  mov w7, w3
  if (sub in {MUL_MUL, MUL_MLA, MUL_SMULL, MUL_SMLAL}):
      cmp w7, #0
      b.ge pos
      mvn w7, w7             // 注意：x64 用的是按位 NOT，不是取负
  pos:
  orr  w7, w7, #1
  clz  w8, w7
  mov  w9, #31
  sub  w8, w9, w8            // bsr = 31 - clz
  lsr  w8, w8, #3
  add  w20, w20, w8
  ```
- 再 `addFetch`，然后 Thumb `+1`，ARM `+(2 + plus)`，
  `plus = (MUL_MUL||UMULL||SMULL) ? 0 : 1`。
- `checkBudget(pc+step())`。

### 5.9 单次加载/存储

`emitLoadStore()`（`JitEmit.cpp:1418-1681`）：

地址计算（`w23` = address，`w24` = 写回值）：

```
ldr  w23, [x19, #n*4]
if (JF_PC_ALIGN4) bic w23, w23, #3
if (JF_IMM):
    mov w24, w23
    if (imm) up ? add w24,w24,#imm : sub w24,w24,#imm
else:
    emitOffset(in, w3)
    mov w24, w23
    up ? add w24,w24,w3 : sub w24,w24,w3
if (pre) mov w23, w24
```

STORE：

1. 建 `needInterp` 标签，`emitIoRangeCheck(size, ni)`，再 `emitTrace()`。
   - `emitIoRangeCheck`：`(addr & 0x0F000000) == 0x04000000` 即 IO；
     `size > 1` 时再检查 `addr + size - 1`。实测（4.4）：
     `and w4,w23,#0x0F000000` 能编码，但 `cmp w4,#0x04000000` **不能**
     （`errors=1`）；`mov w5,#0x04000000` 能编码（单条 `movz`，4 字节）。
     所以写法是 `and w4,w23,#0x0F000000; mov w5,#0x04000000; cmp w4,w5;
     b.eq ni`（3 条，12 字节）。不要用 `add`/`cmp` 的立即数形式。
   - 指令末尾必须 `queueNeedInterp(ni, true)`，否则桩不会被发射，
     `b.eq ni` 会跳到缓冲区里的随机位置。桩的内容是
     `nextPc = 本条指令的 pc`、`status = JITX_NEED_INTERP`。
2. 写回顺序（V7 语义，必须照抄）：
   - `pre && wb && size != HALF`：在 store **之前** `str w23,[x19,#n*4]`；
   - 半字：在 store **之后**写回；
   - `!pre && wb`：store 之后 `str w24,[x19,#n*4]`。
3. 调 helper：
   - BYTE：`w0=addr; ldrb w1,[x19,#d*4]; CPUWriteByte`
   - HALF：`w0=addr; ldrh w1,[x19,#d*4]; CPUWriteHalfWord`
   - WORD：`w0=addr; ldr w1,[x19,#d*4]; CPUWriteMemory`
4. 周期：`addFetch`，非 Thumb 时 `addConst(half?4:2)`，然后 access32/16。

LOAD：

- 是否走内联快路径：
  ```
  fast = (sub == MEM_BYTE) || (sub == MEM_HALF) ||
         (sub == MEM_WORD && !(flags & JF_QUICK));
  ```
  Thumb 的 `ldr [pc]` / `ldr [sp]` 带 `JF_QUICK`，必须走 helper
  （`CPUReadMemoryQuick` 不触发 Lua 读钩子）。
- 区域判定（`w8 = address >> 24`）：
  - `2/3` → 快；
  - `5..7` → 快；
  - 半字：`8..12` **不快**（ROM 半字读要交给 helper，因为
    `0x080000C4/C6/C8` 是 RTC）；
  - 字/字节：`8..12` 也快；
  - 其他（0 BIOS/open bus、4 IO、13 EEPROM、14 SRAM/Flash、15）→ slow。
  - 不能用 `(addr & 0x0E000000)` 判区域：`0x12xxxxxx/0x13xxxxxx` 在 helper
    里是 open bus，不是 EWRAM/IWRAM。
- 快路径：
  ```
  lsl w8, w8, #4
  ldr x9,  [x27, w8, uxtw]        // address
  add w8, w8, #8
  ldr w10, [x27, w8, uxtw]        // mask
  and w10, w10, w23
  字:   bic w10, w10, #3
  半:   bic w10, w10, #1
  字节: 不处理
  ```
  然后：
  - 字：`ldr w11,[x9,w10,uxtw]`；若 `address & 3 != 0`：
    `and w12,w23,#3; lsl w12,w12,#3; ror w11,w11,w12`；
  - 半：`ldrh w11,[x9,w10,uxtw]`；若 `address & 1 != 0`：
    - unsigned：`ror w11,w11,#8`（32 位旋转，**不是** `ror w11,w11,#8`
      的 16 位版本；低字节会到 bit24-31）；
    - signed：`lsr w11,w11,#8; sxtb w11,w11`；
    最后统一 `sxth w11,w11`（signed 半字读）；
  - 字节：`ldrb w11,[x9,w10,uxtw]`。
  然后跳到 `done`。
- slow 路径：
  - BYTE：`CPUReadByte`；unsigned `uxtb w11,w0`，signed 直接 `mov w11,w0`
    （最后 `sxtb`）。
  - HALF：signed `CPUReadHalfWordSigned`，否则 `CPUReadHalfWord`；
    `mov w11,w0`，最后 signed `sxth w11,w11`。
  - WORD：`JF_QUICK ? jitReadQuick : CPUReadMemory`；`mov w11,w0`。
- 写回：
  ```
  str w11, [x19, #d*4]
  if (d != n && wb)
      pre ? str w23,[x19,#n*4] : str w24,[x19,#n*4]
  ```
- 周期：`addFetch`，非 Thumb `addConst(3)`，access32/16。
- `d == 15`（只有 ARM 前索引字加载会到这里）：
  `addConst(2); ldr w11,[x19,#60]; bic w11,w11,#3; emitExit(w11, JITX_DONE)`，
  不再 `checkBudget`。
- 否则 `checkBudget(pc+step())`。

`jitReadQuick(u32)` 是 A64 文件里的本地包装：
`u32 jitReadQuick(u32 address) { return CPUReadMemoryQuick(address); }`。

### 5.10 LDM / STM / PUSH / POP

全部按寄存器列表展开（不循环）。x64 版本在 `JitEmit.cpp:1687-1938`。

PUSH（Thumb，`regs = popcount(list & 0xFF)`，`lr = bit14`）：

```
ldr w23,[x19,#52]                    // sp
sub w23,w23,#(regs*4 + (lr?4:0))
mov w24,w23                          // new sp
bic w23,w23,#3                       // 对齐后的写地址
emitIoRangeCheck(total*4, ni); emitTrace()
for r = 0..7 (升序且 list 置位):
    w0 = w23; w1 = reg[r]; call CPUWriteMemory
    addConst(1)
    first ? addAccess32(w23) : addSeq32(w23)
    first = false
    add w23,w23,#4
if (lr):
    w0 = w23; w1 = reg[14]; call CPUWriteMemory
    addConst(1); first ? access32 : seq32
str w24,[x19,#52]
addFetch; checkBudget(pc+2)
```

POP（Thumb）：

```
withPc = list & 0x8000
ldr w23,[x19,#52]; bic w23,w23,#3
ldr w24,[x19,#52]; add w24,w24,#(regs*4 + (withPc?4:0))
for r = 0..7 (升序且 list 置位):
    w0 = w23; call CPUReadMemory
    addConst(2)
    first ? access32 : seq32
    str w0,[x19,#r*4]
    add w23,w23,#4
if (!withPc):
    str w24,[x19,#52]; addFetch; checkBudget(pc+2); return
// PC 槽：不 +2，只 access
w0 = w23; call CPUReadMemory
first ? access32 : seq32
bic w11,w0,#1
str w24,[x19,#52]        // 先写回 sp
addFetch
emitExit(w11, JITX_DONE) // 最后才写 out 并跳尾声
```

（顺序照 x64：`and eax,0xFFFFFFFE` → 写 sp → `addFetch` → 写 out → 跳尾声。
`emitExit` 是"写 out + 跳尾声"，后面不能再放任何有效指令；
`nextPc` 必须 `& ~1`。）

LDM/STM（ARM）：

- 基址先读入 `w23`，新基址算到 `w24`：
  - IA：`w24 = base + 4*popcount`；`w23 = base & ~3`
  - IB：`w24 = base + 4*popcount`；`w23 = (base+4) & ~3`
  - DA：`w24 = base - 4*popcount`；`w23 = (w24+4) & ~3`
  - DB：`w24 = base - 4*popcount`；`w23 = w24 & ~3`
- STM：
  - `emitIoRangeCheck(4*(popcount + (list&0x8000?1:0)), ni); emitTrace()`
  - 对 r=0..14 每个置位：`w0=w23; w1=reg[r]; CPUWriteMemory`；
    若 `W`：`str w24,[x19,#n*4]`；`addConst(1)`；first access32 否则 seq32；
    `add w23,#4`。
  - 若 bit15：`w0=w23; w1=reg[15]+4; CPUWriteMemory`；若 `W` 再写回；
    `addConst(1)`；access32/seq32。
  - `addFetch`；非 Thumb `addConst(2)`；`checkBudget(pc+step())`。
- LDM：
  - 对 r=0..14：`w0=w23; CPUReadMemory`；`addConst(thumb?2:1)`；
    first access32 否则 seq32；`str w0,[x19,#r*4]`；`add w23,#4`。
  - Thumb：若 base 不在 list，`str w24,[x19,#n*4]`；`addFetch`；
    `checkBudget(pc+2)`；返回。
  - ARM：`addFetch; addConst(2)`。
    - 若 bit15：再 `CPUReadMemory`；`addConst(2)`；access32/seq32；
      若 `W && base 不在 list` 写回；**不做对齐 mask**，直接
      `emitExit(w0, JITX_DONE)`。
    - 否则若 `W && base 不在 list` 写回；`checkBudget(pc+4)`。

### 5.11 SWP

`emitSwp()`（`JitEmit.cpp:1944-1976`）：

```
ni
ldr w23,[x19,#n*4]
emitIoRangeCheck(byte?1:4, ni)
emitTrace()
if (byte):
    w0=w23; CPUReadByte; mov w24,w0
    w0=w23; ldrb w1,[x19,#m*4]; CPUWriteByte
else:
    w0=w23; CPUReadMemory; mov w24,w0
    w0=w23; ldr w1,[x19,#m*4]; CPUWriteMemory
str w24,[x19,#d*4]
addFetch; checkBudget(pc+4)
queueNeedInterp(ni, true)
```

- 读出的值必须在写 helper 之后仍然可用 → 放 `w24`（callee-saved）。
- SPSR/PC 等边界见拒绝清单。
- `ni` 同样必须在末尾登记，否则 IO 地址会跳进未绑定的标签。

### 5.12 分支与 BX

`emitBranch()`（`JitEmit.cpp:1982-2031`）：

ARM：

```
skip:
emitCondSkip(cond, skip)
if (JF_LINK) reg[14] = pc + 4
addFetch; addConst(3)
emitChainExit(pc + 8 + imm)
bind(skip)
addFetch
emitChainExit(pc + 4)
```

Thumb BL 低半（`JF_LINK`）：

```
w3 = reg[14] + imm
bic w3,w3,#1
reg[14] = (pc + 2) | 1
addFetch
emitExit(w3, JITX_DONE)
```

Thumb 条件/无条件：

```
skip:
emitCondSkip(cond, skip)
命中：cond==0xE ? addFetch : addConst(3)
emitChainExit(pc + 4 + imm)
bind(skip)
addFetch
emitChainExit(pc + 2)
```

`emitChainExit(nextPc)`：

- 若没有槽位（`nChain >= JIT_MAX_CHAIN`）：直接
  `out->nextPc = nextPc; out->status = JITX_DONE; b epilogue`。
- 否则分配 `JitChainSlot`：
  ```
  slot->target = 0; slot->tag = (nextPc & ~1) | thumb;
  slot->linkIndex = 0xFFFF; slot->reserved = 0;
  ```
  生成：
  ```
  emitMovImm64(x9, &slot->target)
  ldr x9,[x9]
  cbz x9, unlinked
  br x9
  unlinked:
      out->nextPc = nextPc
      out->status = JITX_DONE
      b epilogue
  ```
- 只有 `JITX_DONE` 出口才建链；`JITX_BUDGET/NEED_INTERP` 不建。
- `JitCache::tryLink()` 只把 ROM→ROM、整块编译、`frameKind` 相同的后继写进
  slot（`JitCache.cpp:461-503`）；A64 固定帧，所以 `frameKind` 恒 0。

BX（`JitEmit.cpp:2033-2057`）：

```
w3 = reg[m]
w4 = (w3 & 1) ^ 1
strb w4,[x25,#armState]
cmp w4,#0
b.ne arm
bic w3,w3,#1          // Thumb 目标
b done
arm:
bic w3,w3,#3          // ARM 目标
done:
addFetch
addConst(thumb ? 0 : 3)
emitExit(w3, JITX_DONE)
```

### 5.13 MRS / MSR

MRS（`JitEmit.cpp:2063-2071`）：

- `sub == 0`（CPSR）：先 `CPUUpdateCPSR()`（`arm100` 会重算 reg[16]）。
- `w3 = reg[sub ? 17 : 16]`；`str w3,[x19,#d*4]`；`addFetch`；`checkBudget`。

MSR（`JitEmit.cpp:2073-2112`）：只编译 SPSR 形式。

```
if (JF_IMM):
    rot = (n & 0xF) * 2
    v = imm & 0xFF ; if (rot) v = ror(v, rot)
    w4 = v
else:
    w4 = reg[m]
skip:
  w3 = armMode
  if (w3 <= 0x10) goto skip
  if (w3 >= 0x1F) goto skip
  for byte in 0..3:
      if (!(d & (1 << byte))) continue
      keep = ~(0xFF << (byte*8))
      w3 = reg[17]
      w3 &= keep          // keep 不一定是 A64 逻辑立即数，用 movz/movk 装 w5 再 and
      w6 = w4 & (0xFF << (byte*8))
      w3 |= w6
      reg[17] = w3
skip:
addFetch; checkBudget(pc+step())
```

要点：

- `armMode` 的地址偏移从 `x25` 用运行时指针差算，不硬编码。
- 四个 `keep`（`0xFFFFFF00` / `0xFFFF00FF` / `0xFF00FFFF` / `0x00FFFFFF`）
  **全部是合法的 A64 逻辑立即数**，2026-11-05 用 asmjit 编码器逐个实跑，
  `and w3,w3,#keep` 都是 `errors=0 size=4`。早先"byte 1/2 的两段掩码不是
  逻辑立即数、必须用 movz/movk"的说法**是错的**。
  `0xFF << (byte*8)` 四个字段掩码同样都能直接编码。
  实现上仍然可以统一用 `emitMovImm32(w5, keep); and w3,w3,w5` 以求稳，
  但那会多一条指令，不是必须的。

### 5.14 跟踪

`emitTrace()`（`JitEmit.cpp:2116-2125`）：

- 只有 `JIT_PC_TRACE` 为 1 且 `VBA_JIT_PCTRACE` 环境变量存在时才发。
- 当前 `JitCache.h` 里 `#define JIT_PC_TRACE 0`，所以默认死代码。
- 每条**实际执行**的指令调用一次 `jitTracePc(pc)`。
- STORE/STM/PUSH/SWP 的 trace 必须在 IO 范围检查通过之后；
  其余指令在 `emitBody` 开头。
- `traceDone` 每个 `emitOne` 重置。

---

## 6. 缓存、链接与编译

这些机制全部复用 `JitCache.cpp`，A64 文件只实现三个接口：

- `jitNativeAvailable()`：
  - A64 不需要 x64 的 ±2GB `disp32` 检查（所有全局地址都用 movz/movk 绝对装入）。
  - 建议实现为：V7 + ARM64 时返回 true；可顺手检查所有要用的小偏移
    （flags 相对 `x25`、wait 表相对 `x26`）是否能编码，否则返回 false。
  - V8 / 非 ARM64：返回 false。
- `jitCompileBlock()`：
  - 结构与 x64 相同（`JitEmit.cpp:2322-2370`）：
    1. 校验参数；
    2. 清零 `JitCompiledBlock`（`fn/chainEntry/insnCount/chainCount/selfSlot/
       frameKind/reserved2/chain[]`）；
    3. `CodeHolder code; code.init(rt->environment())`；
    4. `a64::Assembler assembler(&code)`；
    5. `BlockEmitter::compile(&insns)`；
    6. `dumpCompiledBlock()`（`VBA_JIT_ASM`）；
    7. `rt->add(&fn, &code)`；
    8. 填 `fn / insnCount / chainEntry = (u8*)fn + chainEntryOffset /
       chainCount / frameKind = 0`。
  - `JitRuntime` 用函数内 static（与 x64 一样），进程内只创建一次。
  - 每个块新建一个 `CodeHolder`；asmjit 不允许对已 `add()` 的 holder 复用。
- `jitReleaseBlock()`：`rt->release(fn)`，然后清 `fn/chainEntry/insnCount/
  chainCount/chain[].target`。
- `BlockEmitter::compile(u16 *outCount)`（`JitEmit.cpp:2179-2237`）：
  1. 先扫描最长可编译前缀 `n`（`nativeSupported`），同时记录是否有内存指令
     （A64 固定帧，`usesMem` 只用于统计，不影响帧）；
  2. `n == 0` 返回 false；
  3. 初始化标签/计数；
  4. `emitPrologue()`；
  5. `chainEntryOffset = a->offset()`；
  6. `cmp w20,w21; b.ge chainBail`（链式入口的预算守卫）；
  7. 对每条指令：`pc = guestPc + i*step`；需要时 `setPc()`；`emitOne()`；
  8. `emitChainExit(guestPc + n*step)`（前缀跑完）；
  9. `emitEpilogue()`；
  10. 绑定 NEED_INTERP 桩、BUDGET 桩、chainBail；
  11. `*outCount = n`。
- `chainBail`：`nextPc = block->guestPc; status = JITX_BUDGET; b epilogue`。
  （前驱已经花完预算时，后继在**自己**的第一条指令前停下。）
- `checkBudget(nextPc)`：`cmp w20,w21; b.ge stub`，stub 统一在尾声后生成
  `nextPc/status=JITX_BUDGET`。
- `checkBudgetReg(nextPcGp)`：x64 里曾有这个声明和实现，但**从来没有调用点**，
  2026-11-05 已删除。**A64 不要实现它**，所有预算检查都走
  `checkBudget(constant nextPc)`。
- NEED_INTERP 桩：只在指令可能运行时 bail 时建（IO store、ARM 大移位计数）；
  统一在尾声后生成 `nextPc=当前指令; status=JITX_NEED_INTERP`，
  保证正常路径不会落入桩里。
- `JitCache::runBlock()` 的后续契约（`JitCache.cpp:696-793`）：
  - `JITX_NEED_INTERP`：`*needInterp=true`；`armNextPC = nextPc`，
    `reg[15] = nextPc + (armState?4:2)`（除非 `JITX_KEEP_PC`）；
  - `JITX_BUDGET`：同上，停止块；
  - `JITX_DONE`：从 `nextPc` 继续执行块内剩余 IR（`start = insnCount`），
    整块跑完才写 `armNextPC/reg[15]`；
  - shadow 模式下会先用同一参数跑 IR，再比较 CPU/内存/周期。

### 6.1 编译与链接阈值（已核实）

来自 `JitCache.cpp` / `JitCache.h`：

| 常量 | 值 | 作用 |
|---|---|---|
| `JIT_COMPILE_AFTER` | 8 | 同一 PC slot+tag+hash 被看到 8 次才编译；计数上限 255 |
| `JIT_CACHE_BLOCKS` | 4096 | 解码块缓存 |
| `JIT_CODE_ENTRIES` | 16384 | 本机代码槽 |
| `JIT_PAGES` | 288 | 失效页版本表 |
| `JIT_MAX_LINKS` | 8192 | 链接池 |
| `JIT_LINK_NONE` | 0xFFFF | 空链接索引 |
| `JIT_MAX_CHAIN` | 4 | 每块出口槽数 |

- 编译前会 `unlinkCodeSlot()` + `jitReleaseBlock()` 回收旧代码，
  然后才调用 `jitCompileBlock()`；A64 的 `jitReleaseBlock()` 必须在
  `fn == null` 时安全返回（x64 就是这样）。
- 只有 `runBlock()` 看到 `r.status & JITX_DONE` 且 `jitShadowMode != 1`
  时才调用 `tryLink()`；`VBA_JIT_NOCHAIN` 可以关掉链接。
- `tryLink()` 的条件（`JitCache.cpp:461-505`）：
  1. 源块 `selfSlot != 0xFFFF`、`chainCount > 0`、`insnCount == b->count`
     （整块都被后端编译，没有 IR 尾巴）；
  2. `nextPc` 的高字节在 8..13（ROM only）；
  3. 目标缓存项的 tag/version/codeStamp 全部匹配，且目标
     `insnCount == block.count`（同样是整块编译）；
  4. 目标 `fn && chainEntry` 存在，且 `frameKind` 与源块相同。
- 因此 A64 固定 `frameKind = 0` 不只是"可以"，而是让上面第 4 条永远成立；
  如果哪天后端改成按需保存寄存器，必须恢复 `frameKind` 的区分。
- `jitStats.chainLinks` 只增不减，链式执行也不计入 `blocksRun`，
  所以日志里的 `runs` 会低估实际块数；这是统计口径，不是执行错误。

---

## 7. V7 特有的语义事实（A64 必须照抄）

1. **没有** `busPrefetch` / `busPrefetchCount` / `cpuPrefetch` / `cpuTotalTicks`。
   因此：
   - `JF_CLEAR_PREFETCH` 在 V7 构建里没有实际作用（`JF_DATA32` 也只在 V8
     构建的 `JitDecodeThumb.cpp` 里置位）；
   - x64 后端忽略这两个标志对 V7 是正确的，A64 也忽略。
2. V7 的周期全部来自 6 张 `memoryWait*` 表和 `thumbCycles[opcode>>8]`，
   这些查表**没有副作用**（不像 V8 的 `codeTicksAccess*` 会改
   `busPrefetchCount`）。
3. Thumb MUL 的分档源是 **Rs**，不是 Rd 的旧值；有符号形态取 `~Rs`
   （按位 NOT），不是 `-Rs`。
4. V7 ARM `ror Rd,Rm,Rs` 在 `raw & 31 == 0` 时也要写 C（`SHK_ROR_V7`）；
   Thumb 的 `ror` 只有 raw 为 0/32 时写 C（`SHK_ROR_T`）。
5. Thumb 的 `ldr [pc]` / `ldr [sp]` 使用 `CPUReadMemoryQuick`，不触发 Lua 读钩子
   （`JF_QUICK`）；ARM 的字加载走 `CPUReadMemory`。
6. `CPUReadHalfWordSignedWrapped()` 的奇地址语义：
   - 先调 `CPUReadHalfWordWrapped()`（奇地址做 `(v>>8)|(v<<24)` 的 32 位旋转）；
   - 再 `value = (s8)value`（只保留低字节，按其符号扩展成 16 位）。
   - 因此 A64 快路径：`ldrh` → 若奇地址 `lsr #8; sxtb` → 最终 `sxth`。
7. 写 IO 区的 store 必须回解释器（`JITX_NEED_INTERP`）：`CPUWriteMemoryWrapped()`
   可能调 `CPUCheckDMA()`，后者通过 `ext*` 改写 CPULoop 的 `cpuNextEvent`。
8. 写 PC 但不是分支的指令（`ldrb/ldrh/ldrsb/ldrsh pc`、后索引字加载到 PC、
   `str rX,[r15...]!`、`stm r15!`、`mrs r15`）在 V7 是"绕一条"语义，
   块无法表达，必须拒绝编译（见 5.1）。

---

## 8. 验证计划（尚未执行）

三层缺一不可：

### 8.1 单条 + 块级差分

```
VBA_VS_Latest_V72XR.exe -rom roms\<x>.gba -jitverify 20000
```

- 覆盖 mario / sonic / aos；
- 输出在无 stdout 时写 `jitverify.log`；
- 期望：单条 ARM/Thumb、块级 ARM/Thumb 全部 0 差异；
- x64 基线是每项约 4 秒、全绿。

### 8.2 影子对比

```
set VBA_JIT_SHADOW=1
VBA_VS_Latest_V72XR.exe -rom roms\<x>.gba ... -jit
```

- `JitCache::runBlock()` 在 shadow 模式下对同一块先跑 native、再跑 IR，
  比较寄存器/内存/周期；
- **覆盖范围有限**：只有 `code->insnCount == b->count`（整块都编译了）且
  `!blockWritesMemory(b)` 的块才进影子比较（`JitCache.cpp:709`）。
  也就是说带写内存指令的块、以及被后端截断前缀的块都不覆盖，必须靠
  `-jitverify` 和端到端 `-hashtrace` 补上；
- 只要出现一处差异就会打印并自动关闭 shadow。

### 8.3 端到端逐帧

```
VBA_VS_Latest_V72XR.exe -rom roms\<x>.gba -play movies\<x>.vbm \
    -frameskip 10 -synchronize 0 -vsync 0 -hashtrace <out>.csv -jit
```

- 跑 6000 帧，`-jit` 与 `-nojit` 对比；
- **只比 `ram` 列**。`full` 列包含 pix/sound/rtc/OSD，在无限跳帧下本来就
  可能不同（`jit-recompiler.md` 9.6 已钉死原因）；
- x64 的三份 SHA-256 基线记录在 `jit-recompiler.md` 11.2。

### 8.4 编译确实发生了

`jit.log` 必须出现非零的：

```
<n> native blocks, <n> compiles, ..., <n> native instructions, ...
```

如果 ARM64 仍然全是 0，说明 `jitNativeAvailable()` 恒 false 或
`JitEmitA64.cpp` 没被编进目标。链式跳转还应看到 `chain links > 0`
（证明 `chainEntry` 偏移和固定帧可用）。

### 8.5 构建矩阵

| 配置 | 期望 |
|---|---|
| `V7Release|ARM64` | A64 后端生效，上述三层全绿 |
| `Release|ARM64`（V8） | `JitEmitA64.cpp` 走 stub，行为与现在一致 |
| `V7Release|x64` | x64 后端回归，三层全绿 |
| `V7Release|Win32` | stub，编译通过 |
| selftest（V7） | 1207 项，0 失败 |

### 8.6 性能（可选）

x64 在 `-frameskip 10 -synchronize 0` 下对解释器是 1.10–1.23x
（`jit-recompiler.md` 6.5）。A64 先正确，再比较；验收目标至少不低于 x64 的 70%
（`jit-recompiler.md` Phase 4 的口径）。

### 8.7 ARM64 机器上的完整验证脚本（模板）

在目标机器上按这个顺序跑；每行都对应本文的一层验收。

```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe"
$sln     = "C:\Users\zhang\vba-rerecording\win32\VBA_Latest.sln"
& $msbuild $sln /p:Configuration=V7Release /p:Platform=ARM64 /m
& $msbuild $sln /p:Configuration=Release   /p:Platform=ARM64 /m   # V8：验证 stub

$dir = "...\win32\arm64\V72XR"
Copy-Item "...\win32\x64\V72XR\roms\*"   (Join-Path $dir "roms")   -Force
Copy-Item "...\win32\x64\V72XR\movies\*" (Join-Path $dir "movies") -Force
Set-Location $dir
Copy-Item vba.ini vba.ini.baseline        # 见下面的红字

# 第 1 层：单条 + 块级差分，报告写 jitverify.log（GUI 没有 stdout）
& .\VBA_VS_Latest_V72XR.exe -rom roms\aos.gba -jitverify 20000
Get-Content jitverify.log

# 第 2 层：影子对比
$env:VBA_JIT_SHADOW = "1"
& .\VBA_VS_Latest_V72XR.exe -rom roms\aos.gba -jit
Get-Content jit-shadow.txt           # 必须没有 "shadow mismatch"
Remove-Item Env:VBA_JIT_SHADOW

# 第 3 层：端到端，两边显式传开关
foreach ($m in @("nojit","jit")) {
  & .\VBA_VS_Latest_V72XR.exe -rom roms\aos.gba -play movies\aos.vbm 0 `
      -frameskip 10 -synchronize 0 -vsync 0 -quitAfter 6000 `
      "-$m" -hashtrace "$m.csv"
}
```

🔴 **命令行覆盖会写回 `vba.ini`**：`VBA::saveSettings()` 在退出时把内存设置
写回（123 个字面键），所以 `-synchronize 0` / `-vsync 0` / `-frameskip 10` /
`-jit` 全都会固化。跑完必须 `Copy-Item vba.ini.baseline vba.ini` 还原，并核对
`synchronize` / `frameSkip` / `gbFrameSkip` / `jit`，否则下次启动可能永久不限速，
而「设置速度」菜单仍显示 100%。这是既有行为，不要改成 run-only。

**验收判据**：`jitverify.log` 里 `0 mismatches / 0 block mismatches`；
`jit-shadow.txt` 无 mismatch；两份 CSV 的 `ram` 列逐帧一致（`full` 列不算，
见 `jit-recompiler.md` 9.6）；`jit.log` 里 `native blocks / compiles /
native instructions` 非零且 `bad = 0`；有块链接时 `chain links > 0`。

---

## 9. 已知风险 / 待验证点

1. **asmjit 在 MSVC ARM64 上的可编译性未验证**：源码层面的架构/平台分支
   已经查清（1.1 节），没有 `#error` 或 x86-only 代码挡路；但 a64 源码 +
   C++20 + `ASMJIT_STATIC` + MSVC 14.52 ARM64 的组合仍需要第一次真正构建
   才能确认。
2. **`JitRuntime` 的宿主环境识别（已降级为低风险）**：`Arch::kHost` /
   `Platform::kHost` 都是编译期常量，ARM64 上分别解析为 `kAArch64` 和
   `kWindows`（`arch_traits.h:70-95`、`environment.h:80-120`），
   所以 `rt->environment()` 与 `a64::Assembler` 必然匹配。仍需在运行时确认
   `jitNativeAvailable()` 真的返回 true。
3. **`a->offset()` 与 `chainEntry`**：A64 指令定长，理论上准确；仍要用
   `chain links > 0` 和 shadow 验证。
4. **ADC/SBC/RSC 的 V/C**：已用 `arm-new.h` 的公式与 A64 硬件语义对照，
   但必须由 `-jitverify` 的随机操作数覆盖边界（0x7FFFFFFF/0x80000000/
   0xFFFFFFFF、进位 0/1）。
5. **半字/字节非对齐读**：`-jitverify` 会播种非对齐地址；注意有符号半字
   的 `lsr+sxtb+sxth` 链，不要写成 16 位 `ror`。
6. **立即数编码限制**：`0x04000000`、`keep` 等不是 A64 add/逻辑立即数；
   统一用 `movz/movk` 装入 scratch。发射失败本身不会自动中断，必须靠
   4.3 的 `ErrorHandler` 捕获，否则会变成"少几条指令"的静默错码。
7. **地址偏移的 12 位限制**：所有从 `x25/x26/x27` 出发的立即数偏移都要在
   发射时检查；超限则应拒绝编译整个块（安全回退），不能截断。
8. **性能**：固定 96 字节帧 + 每次 helper 5 条指令 + 序言 20 条地址装入，
   可能比 x64 慢；先保证正确，再考虑：
   - 把 flags/wait/memoryMap 的基址合并到更少的寄存器；
   - 用 `adrp`+`add` 或常量池减少地址装入；
   - 只在真正需要时保存 `x23/x24`（但会破坏固定帧/链式假设，不建议）。
9. **不要用 `x18`**：Windows ARM64 把它留给平台/TLS。
10. **不要假设全局量布局**：flags/wait 表的偏移一律在发射时用实际符号地址
    计算；`reg[]` 的 4 字节 stride 和 `MemoryMap` 的 16 字节 stride 要有
    `static_assert`。
11. **大小端**：A64 Windows 是小端；`reg_pair` 的 `I` 在偏移 0 依赖小端。
    如果将来支持大端宿主，这个后端需要重新审视。
12. **`frameKind`**：A64 固定为 0；如果以后改成按需保存寄存器，必须恢复
    "同帧形状才能链接"的约束。
13. 🔴 **当前工作副本在 x64 机器上，没有 ARM64 交叉编译器和 MFC 库**
    （1.2 节；目标机器是另一台 ARM64）。因此第 8 节的验证计划只能在
    目标机器执行。别把"源码写完了 / 语法检查过了"当成"后端能用"。
14. 🔴 **`mvn` 宏冲突**（4.2 节）：ARM64 目标下 MSVC 的 `arm64_neon.h` 会
    破坏 `a.mvn(...)`。第一次编译就会撞上，必须预先 `#undef mvn` 或改用
    `orn`。这是本机 x64 编译检查**发现不了**的类别（x64 目标不包含那个头）。
15. **立即数编码的实测结论与早期笔记冲突**（4.4 节）：`mov` 会自动展开，
    四个 `keep` 掩码都能直接编码，而 `cmp #0x04000000` 不能。实现时以
    4.4 的实测表为准，不要用旧笔记里的"必须 movz/movk"一刀切。
16. **`uxtw` 缩放值必须等于 log2(访问宽度)**：实测 `ldr w4,[x26,w3,uxtw 3]`
    直接 `errors=1`，而同形的 `ldr x9,...,uxtw 3` 可以。wait 表用 2、
    memoryMap 的 `address`（8 字节）用 3 或先 `lsl #4` 再用 0，半字用 1、
    字节用 0。

---

## 10. x64 参考实现索引（移植蓝本）

`src/gba/jit/JitEmit.cpp` 约 2400 行；按功能对照（行号是 2026-11-04 快照，
之后有过小幅删改，按函数名搜索比按行号可靠）：

| 函数/区域 | 行号 |
|---|---|
| 文件头注释、寄存器约定 | 1–32 |
| `jitShiftReg` / `ShiftKind` / `gJitShiftCarry` | 101–193 |
| `jitReadQuick` | 195 |
| `dispOf` / `gmem` / `bitsSet` / `isRegShift` | 212–236 |
| `nativeSupported` | 379–458 |
| `needsPcBefore` | 465–490 |
| `touchesMemory` | 492–506 |
| `emitPrologue` / `emitEpilogue` | 512–553 |
| `emitExitImm` / `emitExitReg` | 555–567 |
| `emitChainExit` | 569–603 |
| `queueNeedInterp` / `emitNeedInterpStubs` | 605–624 |
| `checkBudget` / `emitChainBail` / `emitBudgetStubs` / `checkBudgetReg` | 626–666 |
| `setPc` / `addFetch` / `addWait` | 672–697 |
| `emitCondSkip` | 703–792 |
| `emitIoRangeCheck` | 797–811 |
| `writeCarry` / `emitOperand2` / `emitOffset` | 817–1022 |
| `emitNZ` / `emitFlagsNative` / `emitFlagsLogical` / `emitFlagsAdd` / `emitFlagsSub` | 1024–1145 |
| `emitAlu` | 1151–1276 |
| `emitMul` | 1282–1412 |
| `emitLoadStore` | 1418–1681 |
| `emitBlockTransfer` | 1687–1938 |
| `emitSwp` | 1944–1976 |
| `emitBranch` | 1982–2031 |
| `emitBx` | 2033–2057 |
| `emitMrs` / `emitMsr` | 2063–2112 |
| `emitTrace` / `emitOne` / `emitBody` | 2116–2177 |
| `compile` | 2179–2237 |
| `dumpCompiledBlock` | 2242–2273 |
| `checkedDisplacements` / `jitNativeAvailable` | 2289–2320 |
| `jitCompileBlock` / `jitReleaseBlock` | 2322–2383 |
| 非 x64 / 非 V7 stub | 2385–2398 |

### 10.1 本轮核对中容易漏掉的 x64 细节

- `checkBudgetReg()`（原本 `JitEmit.cpp:321` 声明、`659-666` 实现）**没有任何
  调用点**；所有预算检查都是 `checkBudget(constant nextPc)`。已在 2026-11-05
  从 x64 删除。A64 不必实现寄存器版本，照抄只会多一个死函数。
- `gJitShiftCarry` / `jitShiftReg()` / `ShiftKind` 都定义在 `JitEmit.cpp`
  的匿名 namespace（101-193），**没有头文件声明**。ARM64 构建不编译这个
  TU，A64 文件必须自带一份等价实现（`jitShiftReg` 的语义见 5.5，含
  `SHK_ROR_V7`）；`checkedDisplacements()` 里也引用了
  `&gJitShiftCarry`，如果 A64 保留这个检查，同样要用自己的符号。
- A64 用 `movz/movk` 绝对寻址，不存在 x64 那种 ±2GB 位移限制，所以 A64
  的 `jitNativeAvailable()` 可以只返回 true；不要机械照搬
  `checkedDisplacements()`（它检查的是 rip 相对位移）。
- `emitMul()` 的 `~Rs` 计时只对 `MUL_MUL / MUL_MLA / MUL_SMULL /
  MUL_SMLAL` 生效，`UMULL/UMLAL` 不取反（`JitEmit.cpp:1300` 附近的
  `if`）。A64 的 `mvn` 复刻必须保持同一分组，见 5.8。
- 单次 load 的写回还有一道额外条件：`if (in.d != in.n && wb)`
  （`JitEmit.cpp:1648`）——`Rd == Rn` 时**不写回**。5.9 的伪代码已包含，
  但实现时容易只写 `if (wb)`。
- PC 写回/对齐掩码有三种，不能统一：
  - ARM `LDR pc`：`& ~3`，再 `+2` 周期，`JITX_DONE`；
  - ARM `LDM {pc}`：**不掩码**，装入值原样进 `reg[15]`；
  - Thumb `POP {pc}`：`& ~1`，且不切 ARM（`armState` 保持不变）。
- 块传输的基址写回时机：
  - STM：**每写一个寄存器就把新基址写回**（`JitEmit.cpp:1852/1868`），
    中途被 IO helper 打断时 `reg[n]` 已经是部分更新的值；
  - ARM LDM：只在 `W && base 不在 list` 时于末尾写回；
  - Thumb LDM：只要 base 不在 list 就写回，**不看 W**（`1907`）。
- 单次 store 的写回也有两种时机：`pre && wb` 时，STR/STRB 在 store
  **之前**写回，STRH 在 store **之后**写回（`JitEmit.cpp:1466/1480`）；
  `!pre && wb` 一律在 store 之后。5.9 已写明，这里单列是为了提醒
  "半字是例外"。
- `nativeSupported()` 里的 `VBA_JIT_OPS` 读取是函数内 `static`，进程内
  只读一次环境变量。A64 照抄这个实现，避免同一次运行里两套后端行为不同。
  注意 `VBA_JIT_OPS=0` 是合法值（"全部拒绝"），所以"是否已解析"必须用独立的
  `static bool`，不能拿 0 当哨兵——x64 在 2026-11-05 之前就是这么错的。

相关文件：

- `src/gba/jit/JitEmit.h`：公共 ABI、`JitExecResult`、`JitChainSlot`、
  `JitCompiledBlock`。
- `src/gba/jit/JitIr.h`：`JitInsn`、`JitOp/JitAluOp/JitMulOp/JitShift/
  JitMemSize/JitBlockMode/JitFlags/JitFlagMask`。
- `src/gba/jit/JitCache.cpp`：`runBlock()`、`tryLink()`、影子对比、编译缓存、
  热度阈值、`jitExecute()`。
- `src/gba/jit/JitInterp.cpp`：V7 逐指令语义与周期（第 35 行起是 V7 分支）。
- `src/gba/V7/arm-new.h` / `src/gba/V7/thumb.h`：V7 的权威语义与周期。
- `src/gba/V7/GBAMemory.cpp`：`memoryMap`、读写包装、奇地址旋转。
- `src/gba/V7/GBA.cpp`：六张 wait 表与顺序。
- `src/gba/GBAGlobals.h/.cpp`：`reg[]`、N/C/Z/V、`armState`、`armNextPC`、
  `armMode`。
- `src/gba/GBAinline.h`：`MemoryMap` 布局、`CPUReadMemoryQuick`。
- `third_party/asmjit/asmjit/arm/a64_emitter.h`：A64 指令 API 与 `b_*` 简写。
- `third_party/asmjit/asmjit/arm/a64_operand.h`：`Gp`/`Mem`/`Shift`/regs。
- `third_party/asmjit/asmjit/core/arch_commons.h`：`CondCode`、`Shift`、
  a64 对 `arm` 的 `using namespace`。
- `third_party/asmjit/asmjit/core/operand.h`：`Imm(const arm::Shift&)`。
- `third_party/asmjit/asmjit/core/assembler.h`：`Assembler::offset()`。
- `third_party/asmjit/asmjit/core/code_holder.h`：`code_size()`、
  `copy_flattened_data()`。
- `third_party/asmjit/asmjit/core/emitter.h`：`set_error_handler()`。
- `third_party/asmjit/asmjit/core/error_handler.h`：`ErrorHandler`。

---

## 11. 实施路线（分批，每批都有可判定的验收）

不要一次写完 2000 行再调试。下面每一批都能独立判断"对/不对"，且失败时只会
回退到 IR 解释器，不会破坏现有行为。

### 11.1 当前落地状态（2026-11-05 夜）

| 项 | 状态 |
| --- | --- |
| `src/gba/jit/JitEmitA64.cpp` | **已写出**（约 2200 行）：全部 `emit*`、固定 96 字节帧、四种退出状态、NEED_INTERP/BUDGET/chainBail 桩、条件码、六张 wait 表、内联读快路径、`jitShiftReg`、`ErrorHandler`、`VBA_JIT_ASM` dump、三个公共接口、V8/非 ARM64 stub |
| `win32/VBA_Latest.vcxproj` | **已改**：`JitEmit.cpp` 的 stub 条目改为仅 Win32；新增 `JitEmitA64.cpp`（仅 ARM64，`stdcpp20` + `ASMJIT_STATIC` + asmjit include）；57 条 asmjit `.cpp` 条件扩为 `x64 Or ARM64` |
| clang `--target=aarch64-windows-msvc -fsyntax-only` | **通过**，0 错误（含 `#undef mvn`、`WIN32`、`USE_GBA_CORE_V7`） |
| asmjit A64 编码器全形态发射 | **通过**：`~/.workbuddy/scratch/a64-emit-harness.cpp`，74 个块的 `BlockEmitter::compile()` 全部 `errors=0 / finalize=0 / code_size>0`，覆盖 16 个 ALU、12 种移位、6 种乘法、3 种宽度的读写、4 种 LDM/STM 模式、PUSH/POP/POP{pc}、SWP、各类分支、BX、MRS/MSR 和多指令混合块 |
| MSVC ARM64 编译 / 链接 / 运行 | **未做**，需目标机器 |
| `-jitverify` / shadow / `-hashtrace` | **未做**，需目标机器 |

复现静态检查（在 x64 工作副本上）：

```
# 语法/API
clang++ -fsyntax-only --target=aarch64-windows-msvc -std=c++20 -DASMJIT_STATIC \
  -DUSE_GBA_CORE_V7 -DUSE_GB_CORE_V7 -DWIN32 -D_CRT_SECURE_NO_WARNINGS \
  -I third_party/asmjit -I src -I src/gba -I src/gba/V7 \
  -I <MSVC>/include -I <SDK>/Include/10.0.28000.0/{ucrt,shared,um} \
  src/gba/jit/JitEmitA64.cpp

# 发射检查（把 A64 编码器在 x64 宿主上跑起来）
cl /std:c++20 /EHsc /DASMJIT_STATIC /DUSE_GBA_CORE_V7 /DWIN32 \
   /I src /I src/gba /I src/gba/jit /I third_party/asmjit \
   ~/.workbuddy/scratch/a64-emit-harness.cpp asmjit.lib
a64-emit-harness.exe        # 期望 "ALL OK (0 failures)"
```

harness 通过 `VBA_JIT_A64_FORCE` / `VBA_JIT_A64_EMITTER_ONLY` 让后端在 x64
宿主上也进入发射路径，并用一组桩定义替换模拟器全局；它**只验证编码**，
不执行生成的代码，所以不能替代第 8 节的任何一层。

> ⚠️ 实现时对本文伪代码做的两处偏离，都是 A64 原生能力带来的简化：
> ① `ADC/SBC/RSC` 用单条 `adcs/sbcs` 产生完整 NZCV，因此**不需要**
> `emitFlagsAdd/Sub()` 那套从操作数重算 V/C 的路径，所有算术类统一走
> `emitFlagsNative()`；② `mov(Gp,Imm)` 由 asmjit 自动展开，只有
> `add/sub/cmp` 的大立即数才需要 `addImm()` 先装寄存器。这两条已由
> 发射检查覆盖，但仍需目标机器上的 `-jitverify` 确认语义。

| 批 | 内容 | 验收（本机可做的用 ✓ 标出） |
| --- | --- | --- |
| **0 骨架** | 改 `vcxproj` 的三处条件（§1）；新建 `JitEmitA64.cpp`，只放三个接口 + 空实现（`jitNativeAvailable()` 返回 false） | ✓ clang `-fsyntax-only --target=aarch64-windows-msvc` 0 错误；ARM64 机器：`V7Release\|ARM64` 与 `Release\|ARM64` 都能链接，`jit.log` 里 native 计数仍为 0 |
| **1 只发出口** | `emitPrologue/Epilogue`、`emitExitImm`、`chainBail`、`dumpCompiledBlock`；`compile()` 编译长度为 1 的块，但**只发一个 `JITX_DONE` 出口**，不执行任何 guest 指令 | 验证 ABI、96 字节帧、`JitExecResult` 写入、`runBlock()` 的 `armNextPC/reg[15]` 修正；`-jitverify` 会报周期差异（预期），但 `-hashtrace` 应仍与解释器一致，因为周期由 IR 尾巴补跑。这一步专抓帧/出口错 |
| **2 最小语义** | `JOP_ALU` 的 `MOV/ADD/SUB/CMP` + `setPc/addFetch/checkBudget` + `emitCondSkip` + `emitFlags*`；其余 op 一律截断前缀 | `-jitverify 20000` 的 arm/thumb 单条 pass 对 ALU 0 差异；`VBA_JIT_OPS` 按位二分定位 |
| **3 控制流** | `JOP_BRANCH` / `JOP_BX` / `emitChainExit` / 链接 | 块级差分 0 差异；`jit.log` 的 `chain links > 0`；用 `VBA_JIT_NOCHAIN=1` 做 A/B 确认哈希不变 |
| **4 访存** | `JOP_LOAD/STORE` + `emitIoRangeCheck` + NEED_INTERP 桩 + 内联快路径（6.4 的规则）+ slow helper | 块级差分（它覆盖 STR/STM/PUSH/POP/SWP）；特别验证非对齐字读、半字奇地址有符号读、`memoryMap[11]` 空洞 |
| **5 剩余 op** | `JOP_MUL` / `LDM/STM/PUSH/POP` / `SWP` / `MRS/MSR` / `emitOperand2` 全移位形态 + `jitShiftReg` | 全量 `-jitverify 20000` 四趟 0 差异；`selftest` 双模式 1101/1207 项 |
| **6 工具与端到端** | `VBA_JIT_ASM` / `SHADOW` / `OPS` / `ASM_LIMIT` / `NOCHAIN` / `PCTRACE`；`VBA_JIT_ASM_ERRORS` | `VBA_JIT_SHADOW=1` 无 mismatch；三 ROM × 6000 帧 `ram` 逐帧一致；dump 出来的字节用 `llvm-objdump -d --triple=aarch64 --binary=<arch>` 反汇编核对 |

**每批开始前先做三个"静态检查"**（本机就能跑）：

1. `clang++ -fsyntax-only --target=aarch64-windows-msvc ... JitEmitA64.cpp`
   —— 抓类型、API、宏冲突。
2. 用 4.4 的编码探针把这一批新用到的常量/寻址形态加进表，确认 `errors=0`。
3. `git diff --stat` 看有没有把 CRLF 文件整篇改成 LF（本项目踩过）。

**不要做的事**：不要在 x64 机器上把 `VBA_JIT_X64` 的定义放宽来"借用" x64
后端做 A64 验证——两个 ABI 不同，得到的信息没有价值；不要在没有 ARM64
`-hashtrace` 基线之前合入（2.1 的红线）。

---

## 12. 参考表

### 12.1 IR 枚举（`src/gba/jit/JitIr.h`，实际值，别重抄成猜测）

```
JitOp:        END=0 ALU=1 MUL=2 LOAD=3 STORE=4 LDM=5 STM=6 PUSH=7 POP=8
              SWP=9 BRANCH=10 BX=11 MRS=12 MSR=13 SWI=14 UNDEF=15
JitAluOp:     AND=0 EOR=1 SUB=2 RSB=3 ADD=4 ADC=5 SBC=6 RSC=7
              TST=8 TEQ=9 CMP=10 CMN=11 ORR=12 MOV=13 BIC=14 MVN=15
              compareOnly = (sub >= TST && sub <= CMN)
JitMulOp:     MUL=0 MLA=1 UMULL=2 UMLAL=3 SMULL=4 SMLAL=5
JitShift:     NONE=0 LSL_IMM=1 LSR_IMM=2 ASR_IMM=3 ROR_IMM=4
              LSL_REG=5 LSR_REG=6 ASR_REG=7 ROR_REG=8 ROR_REG_T=9
              IMM_ROT=10 ROR_REG_V7=11
JitMemSize:   WORD=0 BYTE=1 HALF=2
JitBlockMode: IA=0 IB=1 DA=2 DB=3
JitFlags:     WRITEBACK=1<<0 PRE=1<<1 UP=1<<2 SIGNED=1<<3 LINK=1<<4
              IMM=1<<5 PC_ALIGN4=1<<6 CLEAR_PREFETCH=1<<7 USER=1<<8
              DATA32=1<<9 QUICK=1<<10
JitFlagMask:  N=1 Z=2 C=4 V=8  NZ=3 NZC=7 ALL=15
```

- `JitInsn` 有 15 字节字段、按 4 对齐成 **16 字节**（`static_assert` 在头文件里）。
  `hashBlock()` 必须按字段算哈希，**不能 `memcmp`**：有一个未初始化的 padding 字节。
- `JIT_MAX_INSNS = 64`，`JIT_PAGE_SIZE = 1024`。
- `JIT_NEED_INTERP = -2`（IR 解释器的负返回值契约；`0` 是合法周期数，
  见 `jit-recompiler.md` 10.1 第 4 条）。

### 12.2 helper ABI

后端自己声明这些符号（不要 include `GBA.h` / `V7/GBACpu.h`，它们会带入
`UPDATE_REG` 和 handler 宏）：

| 符号 | 签名 | 参数/返回 |
| --- | --- | --- |
| `CPUReadMemory` | `u32 (u32)` | `w0` 地址 → `w0` |
| `CPUReadHalfWord` | `u32 (u32)` | **返回 u32，不是 u16**；奇地址是 32 位旋转结果，不能无条件 `uxth` |
| `CPUReadHalfWordSigned` | `u16 (u32)` | 值已是 `(s8)` 扩到 16 位，慢路径末尾要 `sxth` |
| `CPUReadByte` | `u8 (u32)` | `w0` 低字节有效 |
| `CPUWriteMemory` | `void (u32,u32)` | `w0` 地址、`w1` 值 |
| `CPUWriteHalfWord` | `void (u32,u16)` | `w1` 用 `ldrh` 装箱 |
| `CPUWriteByte` | `void (u32,u8)` | `w1` 用 `ldrb` 装箱 |
| `CPUUpdateCPSR` | `void ()` | 仅 MRS CPSR 前调用 |
| `CPUReadMemoryQuick` | 经 `GBAinline.h` inline | 包一层本地 `jitReadQuick(u32)` 才好取地址 |
| `jitTracePc` | `void (u32)` | `JIT_PC_TRACE` 专用 |
| `jitShiftReg` | `u32 (u32 v, u32 raw, u32 carryIn, u32 kind)` | 隐式第二返回值放 `gJitShiftCarry`，A64 文件必须自带一份 |

调用形态统一是 `movz/movk x16, fn; blr x16`（2.4）。`x16/x17` 是 IP 寄存器，
helper 可以破坏；`x19–x28` 跨调用保活，所以地址/写回值放 `x23/x24`。

### 12.3 调试开关（全部由环境变量读，进程内只读一次）

| 变量 | 位置 | 作用 |
| --- | --- | --- |
| `VBA_JIT_ASM=<file>` | 后端 | 追加导出每块的 IR + 机器码字节 |
| `VBA_JIT_ASM_LIMIT=<n>` | 后端 | 上面那个的块数上限，默认 200，`0` 不限；到限写一行提示 |
| `VBA_JIT_OPS=<mask>` | `nativeSupported()` | 按 `1 << JitOp` 关掉某类指令的编译，二分用；`0` 是合法值 |
| `VBA_JIT_SHADOW=1` | `JitCache.cpp` | 逐块 native vs IR 对比，输出到 `jit-shadow.txt` |
| `VBA_JIT_NOCHAIN=1` | `JitCache.cpp` | 关掉块链接做 A/B |
| `VBA_JIT_PCTRACE=<file>` | `JitCache.cpp` + 后端 | 逐条 `(pc,lr,sp,ticks)` 轨迹，需 `JIT_PC_TRACE=1` 重编 |
| `VBA_JIT_PCTRACE_SKIP=<n>` | `JitCache.cpp` | 轨迹窗口起点，跳过前 n 条 |
| `VBA_JIT_ASM_ERRORS=1` | **A64 新增** | 4.3 的 `ErrorHandler` 打日志到 stderr；错误捕获本身无条件开启 |

`VBA_JIT_ASM` 的字节是**裸机器码**不是目标文件：在 ARM64 机器上用
`llvm-objdump -d --triple=aarch64 -b binary -m aarch64 <file>`（或先把字节
转成 `.bin` 再喂给它）反汇编；MSVC 的 `dumpbin /disasm` 在 x64 宿主上看不了
AArch64。

### 12.4 运行时结构（`JitEmit.h`，已核实）

```
JitExecResult { u32 nextPc; u32 status; }                 // 8 字节
JitChainSlot  { u64 target; u32 tag; u16 linkIndex; u16 reserved; } // 16 字节
JitCompiledBlock { JitNativeFn fn; u8 *chainEntry; JitChainSlot chain[4];
                   u16 insnCount, chainCount, selfSlot; u8 frameKind, reserved2; }
```

- 状态位是 `JITX_NEED_INTERP=1 / KEEP_PC=2 / BUDGET=4 / DONE=8`，每次写完整
  32 位字。`KEEP_PC` 目前没有发射点（写 PC 的非分支指令在编译期就被截断）。
- `JIT_MAX_CHAIN=4`，超出就退回"直接写 out 并返回"，不是错误。
- `frameKind`：x64 表示"要不要保存 rdi/rsi"，**A64 恒 0**（固定帧，2.3）。

### 12.5 文件地图

| 文件 | 用途 |
| --- | --- |
| `src/gba/jit/JitEmitA64.cpp` | **待写**：A64 后端全部实现 |
| `src/gba/jit/JitEmit.cpp` | x64 蓝本（约 2400 行），§10 有函数索引 |
| `src/gba/jit/JitEmit.h` | 公共 ABI 与运行时结构 |
| `src/gba/jit/JitIr.h` | IR 定义、枚举、`JitBlock` |
| `src/gba/jit/JitCache.cpp` | 块缓存、编译缓存、链接、影子、`jitExecute()` |
| `src/gba/jit/JitDecode*.cpp` | 两核共用的解码器 |
| `src/gba/jit/JitInterp.cpp` | V7 逐指令语义与周期规范 |
| `src/gba/jit/JitVerify.cpp` | 差分验证器 |
| `src/gba/jit/selftest.cpp` | 宿主侧解码自测（clang 直接编） |
| `src/gba/V7/arm-new.h` / `thumb.h` | V7 权威语义与周期 |
| `src/gba/V7/GBAMemory.cpp` | `memoryMap`、读写包装、奇地址旋转 |
| `src/gba/V7/GBA.cpp` | 六张 wait 表与顺序 |
| `src/gba/GBAGlobals.h/.cpp` | `reg[]`、N/C/Z/V、`armState`、`armNextPC`、`armMode` |
| `src/gba/GBAinline.h` | `MemoryMap` 布局、`CPUReadMemoryQuick` |
| `third_party/asmjit/asmjit/arm/a64_emitter.h` | A64 指令 API 与 `b_*` 简写 |
| `third_party/asmjit/asmjit/arm/a64_operand.h` | `Gp`/`Mem`/`Shift`/regs |
| `third_party/asmjit/asmjit/core/arch_commons.h` | `CondCode`、`Shift` |
| `third_party/asmjit/asmjit/core/{emitter,error_handler,code_holder,assembler}.h` | 错误处理、代码摊平、`offset()` |
| `documentation/jit-recompiler.md` | 设计思路与后端移植契约（本文的上位文档） |
| `documentation/jit-emit-sample.c` | x64 生成代码的教学样本，可照着做 A64 版 |
