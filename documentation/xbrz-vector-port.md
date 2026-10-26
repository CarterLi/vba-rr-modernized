# xBRZ 换成向量实现

## 结论

`src/filters/xbrz.cpp` 原来是上游 xBRZ 1.9 的**原样**拷贝，所有颜色距离都查一张
**64 MB** 的 `float` 表（`buildDiffToDist()`，256³ 项）。这张表在纯色画面上很快，
一旦相邻像素大多不同就彻底崩掉：每帧 45 万到 70 万次随机读 64 MB，实测
**17-26 ms/帧**，帧率被钉在 40-55 fps，同时把同线程的音频写指针拖到欠载
（爆音）。同一份探针里 2xSaI32 只要 0.4-0.5 ms，所以这是滤镜本身的问题，
不是渲染后端或编译选项。

现在换成了 VBA-M 的向量版本（`src/components/filters/internal/xBRZ/xbrz.cpp`），
距离改成一次算 4 对像素，**表彻底消失**。同机同参数下 2x 的最坏情况从
**23.5 ms 降到 2.1 ms（约 11 倍）**。

## 拿了哪些文件，改了什么

| 文件 | 处理 |
|---|---|
| `src/filters/xbrz.cpp` | 用 VBA-M 的版本整文件替换 |
| `src/filters/xbrz.h` | 同上，另外把 `SCALE_FACTOR_MAX` 从 9 改成 5 |
| `src/filters/xbrz_simd.h` | 新增（SSE2 / NEON / scalar 三后端，约 360 行） |
| `src/filters/xbrz_tools.h` | **没动**，与 VBA-M 逐字节相同 |
| `src/filters/xbrz_config.h` | **没动**，与 VBA-M 逐字节相同 |
| `src/filters/xbrzfilter.cpp` | **没动**，ABI 与 `ColorFormat::rgb` 都没变 |
| `win32/VBA_Latest.vcxproj` | `ClInclude` 加一条 `xbrz_simd.h` |

本地只改了 3 处（下次同步 VBA-M 时要重新施加）：

1. `xbrz.h`：`SCALE_FACTOR_MAX = 9` → `5`。
2. `xbrz.cpp`：`static_assert(SCALE_FACTOR_MAX == 9)` → `5`。
3. `xbrz.cpp`：删掉 `scale()` 里 `case 6` 和 `case 9` 两行。

不暴露 6x/9x 的原因不在滤镜本身，而在下游：`MainWnd` 的 `rect` 只算到 5x，
GDI 的 `filterData` 是固定 4 MB（5x@32bpp 正好 3.84 MB），D3D9 的贴图上限是
1024（1200 宽的 5x 已经会被 `Direct3D.cpp` 禁用滤镜）。留着 `Scaler6x`/
`Scaler9x` 只会多两套 `scaleImage()` 实例化。

## 新旧实现的关键差别

| | 旧（本站） | 新（VBA-M 向量版） |
|---|---|---|
| 颜色距离 | 查 64 MB 表，每通道差压到 7 bit | `distYCbCr4()` 一次算 4 对，单精度，含 `fsqrt` |
| 平坦区早退 | 逐像素 `pix1 == pix2` | 4 lane 的 mask，整批写 0 |
| 源行越界 | 每像素 `clamp(x±2)` | 行首拷贝一次，左右各 pad 4/8 像素，之后全是 `loadu` |
| 重复距离 | 每个角重算 10 个 | 每源行对算一次 `DistRow`（H/V/D1/D2 四张 float 行），后面全是 load |
| 角落判定 | 标量，逐像素 | 4 个角一批（每 lane 一个角），2 bit/角打包成字节 |
| 混合 | 每个旋转重新算距离 | 24 个距离里 8 个"马步"对一次算完，其余按编译期常量索引距离行 |
| 无混合像素 | 一次填一块 | 连续 4 个像素一次 `fillBlock4()`，向量 store |

## 实测

探针：直接调 `xbrz::scale()`，`srcPitch = 240*4+4`、`src = pix + srcPitch`，
和前端调用完全一致；x86 /O2 /arch:SSE2（与工程 Release 一致）。

| 输入 | 旧（表） | 新（向量） | 倍数 |
|---|---|---|---|
| 平坦 2x | 1.51 ms | 1.07 ms | 1.4x |
| 全杂色 2x | 23.49 ms | 2.11 ms | 11.1x |
| 平坦 5x | 2.27 ms | 1.35 ms | 1.7x |
| 全杂色 5x | 25.38 ms | 2.18 ms | 11.6x |

叠加按行切片（`yFirst/yLast`，上游声明线程安全，每片至少 8-16 行）：

| | 1 线程 | 2 | 4 | 8 |
|---|---|---|---|---|
| 2x 平坦 | 1.34 ms | 0.80 | 0.57 | 0.54 |
| 5x 平坦 | 1.56 ms | 0.89 | 0.64 | 0.59 |

顺带消失的：64 MB 常驻内存、首帧建表的 **47-57 ms** 停顿、每帧一堆表访问的
TLB 压力。每帧现在只剩约 20 KB 的临时分配（`rowBuf` + `distBuf` + `cornerBuf`）。

## 像素不是逐像素相同

表把每个通道差除以 2 后塞进一个字节，等于 7 bit 精度；新实现是精确单精度。
落在 `equalColorTolerance` / `dominantDirectionThreshold` 边界上的判定会翻。

实测（240x160 的 10% 杂色帧，2x，153600 个输出像素）：

- 与旧表实现的差异：**137 个像素（0.089%）**，最大单通道差 130，其中 126 个在
  画面内部、11 个在边框附近；
- 与 VBA-M 原文件编译出来的结果：**0 个像素不同**（逐字节相同）；
- `XBRZ_FORCE_SCALAR` 的标量后端与 SSE2 后端：**0 个像素不同**。

也就是说，这不是移植失误，而是"表的精度"和"精确浮点"本身的差别。要"零像素
变化"就不能换这套实现。

## 关于 `/fp:fast`

结论：**别开，但也不是"开了立刻就错"**。

实测（MSVC 14.52、32 位、同一帧 2x 输出逐字节比）：`/arch:SSE2` 下
`/fp:fast` 与 `/fp:precise` **一个像素都不差**，耗时也不差（杂色 2x：
1.87 ms vs 1.86 ms）；`/arch:AVX2` 配 `/fp:fast`、强制标量后端也都还是 0 差异。
原因很实在：热路径是 SSE2 intrinsics，MSVC 不会把 `_mm_mul_ps` + `_mm_add_ps`
融合成 FMA，而 `/arch:SSE2` 本来就没有 FMA 指令可用。

不开的真正理由是**保证**，不是当前结果。`xbrz_simd.h` 声明 SSE2 / NEON / scalar
三个后端逐像素一致，前提是每条浮点表达式都是单条 IEEE 运算。`/fp:fast` 允许

- 融合乘加：一行探针 `float f(float a,float b,float c){return a*b+c;}`，
  `/arch:AVX2 /fp:fast` 出来是 `vfmadd213ss`，`/fp:precise` 是两条指令；
- 重排求和（`sqrt(y²+cb²+cr²)` 的加法顺序可变）、忽略舍入。

向量路径是 intrinsics，MSVC 不碰；**标量后端是普通 C 浮点代码**，正是会中招的
地方。差 1 ULP 本身不要紧，但距离紧接着要过 `dist < 30.0f`、
`domThr * hf < ei` 这些阈值，1 ULP 就足以翻转一个混合判定 —— 不崩不花屏，
只是静默地和另一条后端不一致，而且依赖编译器版本与目标指令集。

今天测不出差异是因为 1 ULP（相对 1e-7）撞上阈值的概率极低；旧表的 7 bit 量化
（相对 1e-2）才翻出 137 个像素，两者差五个数量级。

真要上 AVX2 / ARM64，正确做法是只给这一个 TU 加 `/fp:contract-`；GCC/Clang
照 VBA-M 那样 `-fno-fast-math -ffp-contract=off`（他们踩过这个坑）。
本站 vcxproj 没有设 `FloatingPointModel`，MSVC 默认就是 `Precise`，保持现状。

## 指令集：`/arch:SSE4.2` 值不值得开

有人提过"x86 保持 SSE2、x64 抬到 SSE4.2（x86-64-v2）"，所以拿同一套探针量了
一遍。结论：**没有必要**，x64 上 SSE4.2 只让 xBRZ 快约 1.5%，其余为 0。

方法：同样两帧（GBA 风格平坦帧 / 每像素都不同的最坏帧），240x160。六个构建
（x86/x64 × SSE2/SSE4.2 × `/fp:precise`//`/fp:fast`）交替轮转跑 15 轮取中位数，
避免频率漂移把 1-2% 的差异放大成假信号。

| 构建 | xBRZ 2x | xBRZ 5x | hq2x | hq4x |
|---|---|---|---|---|
| x86 SSE2，precise（现状） | 1.86 ms | 2.20 ms | 2.85 ms | 3.16 ms |
| x64 SSE2，precise | 1.27 ms | 1.62 ms | 1.70 ms | 3.33 ms |
| x64 SSE4.2，precise | 1.25 ms | 1.59 ms | 1.70 ms | 3.33 ms |
| x64 SSE2，fast | 1.27 ms | 1.60 ms | 1.70 ms | 3.32 ms |

- **SSE4.2 的全部收益**：xBRZ 2x 少 0.02 ms、5x 少 0.02 ms（约 1.5%），hq 系
  与 2xSaI 系在噪声内不变。按 16.7 ms 的帧预算算，是 0.1%。
- **`/fp:fast` 的收益**：没有。x86 上 xBRZ 从 1.860 变 1.851，x64 上不变，
  hq2x 反而从 2.85 变 2.98（这属于噪声，但不是"变快"）。
- **真正有差别的只有 x86→x64**：xBRZ 快 27-32%、hq2x 快 40%，但 hq4x 慢 5%。
  那是 64 位寄存器数量和调用约定带来的，跟 SSE4.2 没关系。
- 目标码层面：`GBA.cpp`、`GB.cpp`、`GBASound.cpp`、`gbSound.cpp` 在
  `/arch:SSE2` 与 `/arch:SSE4.2` 下**目标文件逐字节相同**（0 处 SSE4 指令）；
  只有 `xbrz.cpp` 多出 123 条 `pinsrd`（SSE4.1 的向量插入），体积反而小 1 KB。
- 六个构建的 xBRZ 输出**逐像素相同**（153600 像素 0 差异），说明改 arch 不改画质。

为什么没收益：热路径要么是手写 SSE2 intrinsics（编译器不会把它改写成 SSE4），
要么是查表的整数代码（MSVC 不会向量化）；浮点只出现在 xBRZ 的距离计算里，
而 `/arch:SSE2` 本来就没有可用的 FMA，`/fp:fast` 也就无事可做。

真要开的话，两个前提得知道：

1. `/arch:SSE4.2` 是 **VS 2022 17.11.5 / 工具集 14.41** 才加的，更老的 VS 直接
   命令行报错。本机这套 VS 18 的 `cl.xml` 里 `EnableEnhancedInstructionSet`
   **还没有 SSE4.2 这个枚举值**，只能在 `AdditionalOptions` 里写
   `/arch:SSE4.2`；命令行上出现两个 `/arch` 不报错，**后一个生效**（实测）。
2. x64 的最低 CPU 从 SSE2 抬到 Nehalem（2008）/ Bulldozer（2011）。没有运行期
   分派，Core 2、早期 Athlon 64 这类能跑 x64 的机器会直接非法指令。

换来的 0.02 ms 对不起这两条，所以维持现状：x86 与 x64 都用 `/arch:SSE2`。

## 另一个坑

`xbrz.cpp` 的 SSE2 后端依赖 `_M_IX86_FP >= 2`，由工程的
`EnableEnhancedInstructionSet=StreamingSIMDExtensions2` 提供。不要为了兼容老
CPU 把它关掉，否则会退到标量后端（正确，但慢 3-4 倍）。

## 其他滤镜要不要也换

VBA-M 那侧只有 hq3x/hq4x 有 x86 汇编版（`hq/asm/hq3x_{16,32}.asm`、
`hq4x_{16,32}.asm`，`ENABLE_ASM_SCALERS`）和 2xSaI 的 MMX 版
（`2xSaImmx.asm`，`ENABLE_MMX`）；本站对应的 C++ 实现实测都不慢，没必要引入
MASM 依赖：

| 滤镜（32bpp） | 平坦 | 杂色 |
|---|---|---|
| Simple2x / Bilinear / Scanlines / Pixelate2x | 0.01 ms | 0.01 ms |
| AdMame2x | 0.02 ms | 0.03 ms |
| 2xSaI / Super2xSaI / SuperEagle | 0.43-0.50 ms | 0.47-0.49 ms |
| hq2x | 0.89 ms | 3.38 ms |
| hq3x | 1.13 ms | 1.95 ms |
| hq4x | 1.90 ms | 3.52 ms |
| **xBRZ（移植后）** | **1.07 ms** | **2.11 ms** |

另外本站的 16bpp 2xSaI/Super2xSaI/SuperEagle 已经是 SSE2 内联版
（`2xSaI.cpp` 的 `SaIBlock16`），比 VBA-M 那条 MMX 汇编更新，不要回退。
32bpp 的 `_2xSaI32` 两边都是标量 C++，实测 0.5 ms，不值得动。

结论：只有 xBRZ 是病态项，已经解决；其余滤镜维持现状。
