# AVI 导出链路分析，以及接原生 MFT / D3D12 视频编码的可行性

分析对象：`C:/Users/zhang/vba-rerecording`
涉及文件：`src/win32/AVIWrite.{h,cpp}`、`src/win32/WinSystem.cpp`、`src/win32/MainWndTools.cpp`、
`src/win32/DirectSound.cpp`、`src/win32/XAudio2Sound.cpp`、`src/common/Util.cpp`
日期：2026-10-25

> 本文只做现状分析与可行性判断。**具体设计方案（选编码器 → 决定容器）见
> `documentation/recording-settings-design.md`。**
>
> **注意（2026-10-25 之后）**：文中的"现状"指的是改造之前。现在
> `Tools > Start/Stop Audio/Video File Recording...` 走 Media Foundation，
> §1 描述的那条 VFW 链路只剩下 `Tools > Legacy AVI Recording` 子菜单里的
> 旧版 AVI 和 WAV。

---

## 0. 结论

| 问题 | 答案 |
|---|---|
| 现在的导出方式是什么？ | 视频流走 **VFW（Video for Windows）**：`AVIFileOpen` → `AVIMakeCompressedStream` → 逐帧 `AVIStreamWrite`，编码器由 `AVISaveOptions` 对话框从**本机已安装的 VFW 编解码器**里选。音频是第二条 PCM 流，按字节数写入。 |
| 能不能换成原生 MFT？ | **能，而且这是唯一值得做的方向。** 用 `IMFSinkWriter` 直接出 MP4/H.264 + AAC，约 600~900 行新代码，Win7+ 即可，系统自带软编 MFT 兜底，硬件 MFT（NVENC/QSV/AMF）同一套代码自动吃到。 |
| 能不能接 D3D12 视频编码？ | **技术上能，在这个程序里不值得。** 帧在 CPU 上（`pix` 是模拟器自己的 RGB 缓冲），没有 GPU 帧可零拷贝；D3D12 编码反而要你自己上传纹理 + 自己做 RGB→NV12 + 自己管设备/命令队列/fence，而它支持的 GPU 范围比 MF 硬件 MFT 更窄，且**没有软件兜底**，MF 这条路还是得留着。等于多写 1200~2000 行换 0 收益。 |
| 那"看到什么录什么"呢？ | 想录屏幕上的画面（含 GPU 放大滤镜、OSD、黑边）是**另一件事**，需要给 7 个渲染后端各加一个取纹理的接口。只有真做这件事，D3D12 的零拷贝才有意义。见 §4.3。 |
| 建议 | 分两步：① 抽象出 `IVideoWriter`，把现在的 `AVIWrite` 降级成其中一个实现（不动行为）；② 加 `MFVideoWriter`，按**保存对话框的扩展名**分派（`.mp4` → MF，`.avi` → VFW）。VFW 那条路保留，XP API level 的源码构建不受影响。 |

---

## 1. 现状：一帧 AVI 是怎么出来的

### 1.1 调用链

```
systemRenderFrame()                                   WinSystem.cpp:512
├─ textMethod==0 → Lua 文字 / OSD 文字直接画进 pix      :517-524   ← 会进 AVI
├─ systemRecordAviFrame(copyX, copyY)                  :536       ← 录在这里
├─ theApp.ifbFunction(pix)   帧间混合                  :539       ← 录不到
└─ systemRedrawScreen() → theApp.display->render()     :549       ← 录不到（放大滤镜/OSD/黑边都在这）

systemRecordAviFrame(width, height)                    WinSystem.cpp:428
├─ 首帧：new AVIWrite
│        SetVideoFormat(24bpp, width×height, biHeight>0 → 自下而上)
│        Open(文件名)                                 MainWndTools.cpp:587-616 / WinSystem.cpp:452-479
├─ captureScaled && depth∈{16,32}
│     systemRecordScaledFrame(w,h,bottomUp=true)       WinSystem.cpp:290
│       └─ 走 CPU 滤镜 filterFunction（xBRZ/2xSaI 等）→ 最近邻放大 → 24bpp BGR
│   否则
│     utilWriteBMP(bmpBuffer, w, h, systemColorDepth, pix)   Util.cpp:66 → 24bpp BGR
└─ aviRecorder->AddFrame(buf)                          AVIWrite.cpp:229
      └─ AVIStreamWrite(压缩流, 1 帧, biSizeImage, AVIIF_KEYFRAME)
```

### 1.2 帧的规格

| 项 | 值 | 出处 |
|---|---|---|
| 像素格式 | 一律 24bpp BGR，`biSize = 0x28`，`biHeight > 0`（DIB 自下而上） | `AVIWrite.cpp:60`、`WinSystem.cpp:464-471` |
| 分辨率 | 默认 = LCD 原始分辨率（GBA 240×160 / GB 160×144）；开 `Capture scaled` 后 = `theApp.surfaceSizeX/Y`（窗口实际绘制尺寸） | `MainWndTools.cpp:591-600` |
| 分辨率上限 | 未开 `captureScaled` 时受 `bmpBuffer` 限制 **256×224×4 字节**，且只有一个静态断言 | `WinSystem.cpp:47-50, 491` |
| 帧率 | `dwRate/dwScale = frameRateDividend/frameRateDivisor`（GBA 16777216/280896 ≈ 59.7275） | `AVIWrite.cpp:115-116` |
| 丢帧补偿 | 帧跳过导致没渲染的帧，用**同一张 `pix` 重复写 N 次**，靠 `linearSoundFrameCount` 追音频 | `WinSystem.cpp:445-509` |
| 关键帧 | **每帧都标 `AVIIF_KEYFRAME`** | `AVIWrite.cpp:242` |
| 分卷 | 每 60 帧检查一次，累计 > 2000 MB 就开新文件 `xxx_part2.avi` | `AVIWrite.cpp:254`、`211` |

### 1.3 音频

两条互斥的路：

- **默认（`altAviRecordMethod == false`）**：在声音后端写入声卡缓冲的同一个回调里顺手写 AVI。
  `DirectSound.cpp:405-421` / `XAudio2Sound.cpp:386-394`，格式直接取 `dsbSecondary->GetFormat()`。
- **`altAviRecordMethod == true`**：在 `systemFrame()` 里写 `soundFrameSound`，格式硬编码
  PCM 16bit 立体声 `44100 / soundQuality`（1→44100、2→22050、4→11025），`WinSystem.cpp:805-824`。

音频流在 `AVIWrite::SetSoundFormat()`（`AVIWrite.cpp:63`）里创建，`dwScale = nBlockAlign`、
`dwRate = nAvgBytesPerSec`，所以是采样精确的。**前提是声音后端活着**——声音关掉时
`DirectSound::write()` 根本不会被调用，AVI 里就只有一条空的 PCM 流。

### 1.4 现状的缺陷（按严重程度）

| # | 问题 | 影响 | 出处 |
|---|---|---|---|
| 1 | 编码器由 `AVISaveOptions` 从本机 VFW 编解码器里选 | VFW 是 XP 时代的东西；干净 Win10/11 上一般只剩未压缩（x64 上更少，待实测 §7.1）。**大多数用户没有可用的压缩器**，录出来的 AVI 动辄几百 MB/分钟 | `AVIWrite.cpp:128-139` |
| 2 | 只支持 AVI 容器 | 2 GB 上限（`AVIFile` 写的是经典 AVI，不是 OpenDML），必须分卷；没有 MP4/MKV | `AVIWrite.cpp:254` |
| 3 | 每帧标 `AVIIF_KEYFRAME` | 索引在说谎。含 P/B 帧的流里全标关键帧，某些播放器跳到 P 帧解码出花屏 | `AVIWrite.cpp:242` |
| 4 | 没有时间戳，同步靠帧计数 | 恒定帧率下没问题，但丢帧补偿靠"复制同一帧"，节奏不对时音画会漂 | `WinSystem.cpp:445-509` |
| 5 | `AVIStreamWrite` 在**模拟线程上同步阻塞** | 编码慢 → 掉帧 → 触发帧跳过 → 录进来的帧还是重复的。这是录制掉速的根因 | `AVIWrite.cpp:237` |
| 6 | 录制点在 ifb 和 renderer 之前 | 录的是**模拟器原始帧**，不是屏幕：GPU 放大滤镜（xBRZ shader）、GDI/OSD 文字、黑边都不在片子里 | `WinSystem.cpp:536` vs `539/549` |
| 7 | 崩溃即废片 | AVI 的索引在文件尾，进程挂掉 = 无法播放 | — |
| 8 | 未开 `captureScaled` 时分辨率被 `bmpBuffer` 锁在 256×224 | GBA/GB 实际分辨率够用，但这是硬编码上限，Release 下断言不生效 | `WinSystem.cpp:47-50` |

---

## 2. 决定选型的四条硬约束

1. **`_WIN32_WINNT` 还是 `0x0501`，README 明确写了"Nothing is imported statically"**
   （`README.md:565-579`）。可选组件一律 `LoadLibrary` + `GetProcAddress`：
   D3D9/D3D11/DDraw/DSound/dinput8/XAudio2/wgpu 全这么接的，WIC 连 IID 都自己 `DEFINE_GUID`
   以免把 `windowscodecs.dll` 写进导入表（`ImageWrite.cpp:15-24`）。
   ⇒ **MF 也必须动态加载**（`mfplat.dll` / `mfreadwrite.dll`），否则 XP-flavoured 源码构建直接起不来。
2. **主线程没有 COM apartment**。全项目只有 `ImageWrite.cpp:224`（每次截图自己
   `CoInitializeEx`）和 `ModernFileDialog.cpp:411` 各自初始化。MF 必须照这个模式写一个
   作用域内的 `CoInitializeEx` + `MFStartup` / `MFShutdown`，别去动 `VBA::InitInstance()`。
3. **帧在 CPU 上**。`pix` 是模拟器的 RGB 缓冲，7 个渲染后端都是从它上传的；
   `IDisplay`（`Display.h:22`）**没有任何"把纹理交出来"的接口**。
4. **录的是模拟器帧，不是屏幕上那一帧**。改这个语义是产品决策，不是技术优化。

---

## 3. 路线 A：Media Foundation（推荐）

### 3.1 形状

```
IVideoWriter  (新，src/win32/VideoWriter.h)
├─ AVIWrite       现有实现，行为不动（VFW）
└─ MFVideoWriter  IMFSinkWriter → MP4 / H.264 + AAC
```

`theApp.aviRecorder` 的类型从 `AVIWrite *` 改成 `IVideoWriter *`，其余调用点
（`MainWndTools.cpp`、`WinSystem.cpp`、`DirectSound.cpp`、`XAudio2Sound.cpp`）只改类型不改逻辑。

现有的 `AddFrame(const u8 *bmp)` 假设"自下而上 24bpp DIB"，MF 想要的是自顶向下 RGB32 或 NV12。
两个办法：

- **推荐**：接口改成 `addFrame(const u8 *pixels, int pitch)`，由 writer 通过
  `videoFormat()` 声明自己要什么（宽/高/每像素字节/行序），`WinSystem.cpp` 按它出图。
  这样 `utilWriteBMP` 旁边加一个 `utilWriteRGB32(out, w, h, depth, pix, topDown)`
  （`Util.cpp`，约 60 行，和 `utilWriteBMP` 同构）就够了，转换仍然只有一处。
- 或者让 writer 自己收 `pix` 做转换——会把颜色位运算散到两个类里，不推荐。

### 3.2 数据流

```
pix (16/32bpp)  ─┬─ utilWriteRGB32 ─→ RGB32 自顶向下 ─┐
（可选 CPU 滤镜）  │                                    ├─ SinkWriter
                 └─ (失败时) 自算 BT.601 NV12 ─────────┘   ├─ 视频输入 RGB32 或 NV12
                                                          │  MF 自动插 Video Processor MFT
音频 2940 字节 PCM ────────────────────────────────────────┤─ 音频输入 PCM
                                                          └─ 输出 MP4：H.264 + AAC
```

关键属性：

| 方向 | 属性 |
|---|---|
| 视频输出 | `MFVideoFormat_H264`、`MF_MT_AVG_BITRATE`、`MF_MT_FRAME_SIZE`、`MF_MT_FRAME_RATE`（用 `frameRateDividend/Divisor`）、`MF_MT_INTERLACE_MODE = Progressive`、`MF_MT_PIXEL_ASPECT_RATIO = 1:1`、`MF_MT_MPEG2_PROFILE`（High） |
| 视频输入 | `MFVideoFormat_RGB32` + `MF_MT_DEFAULT_STRIDE = width*4`（正数 = 自顶向下）；或直接 `MFVideoFormat_NV12` |
| 音频输出 | `MFAudioFormat_AAC`、44100、2ch、16bit、`MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION = 0x29` |
| 音频输入 | `MFAudioFormat_PCM`，采样率 = `44100/soundQuality`（MF 会自己插重采样器） |
| SinkWriter 属性 | `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`（吃 NVENC/QSV/AMF 的硬件 MFT）、`MF_SINK_WRITER_DISABLE_THROTTLING` 视情况 |

时间戳：自己按 `frameIndex * 10^7 * dwScale / dwRate`（100ns 单位）算，**不要**指望 sink
按非整数帧率自己推——MP4 的 timescale 是整数，让它猜会慢慢漂。

### 3.3 必须处理的六个坑

1. **背压**。`IMFSinkWriter::WriteSample` 在硬件 MFT 队列满时返回 `MF_E_NOTACCEPTING`。
   现在的 `AVIStreamWrite` 是同步阻塞的，直接换过来会在模拟线程上卡住。
   ⇒ 一个编码线程 + 3~4 帧的环形队列；队列满时丢帧并计数（跟现在的帧跳过语义一致），别阻塞模拟。
2. **偶数尺寸**。NV12 要求宽高都是偶数。240×160 没问题，`captureScaled` 出来的窗口尺寸可能是奇数
   ⇒ 向下取偶（裁 1 像素），或者干脆给 Video Processor 一个偶数尺寸的中间纹理。
3. **RGB32 → NV12 的转换是谁做的**。让 SinkWriter 自动插 Video Processor MFT 代码最少，
   但 VP MFT 在某些系统上建不出来（返回 `MF_E_INVALIDMEDIATYPE`）。
   ⇒ 先试 RGB32；不行就退回自己算 NV12（约 40 行 BT.601 limited，反正每像素我们本来就要摸一遍）。
   **颜色对不对必须肉眼验一次**（BT.601 vs BT.709 会差一档）。
4. **`soundQuality == 4` 是 11025 Hz**。AAC 标准支持，但微软的 AAC 编码器 MFT 不一定收。
   ⇒ 交给 SinkWriter 的重采样器，或者录制时强制 44100。
5. **ARM64**。项目有 ARM64 配置（`VBA_Latest.vcxproj`）。`mfh264enc.dll` 在 ARM64 Windows 上
   应该有，但**没验过**，见 §7。
6. **关文件**。`Finalize()` 之前 MP4 是不完整的；`delete aviRecorder` 现在的语义是析构即关文件，
   MF writer 要在析构里保证 `Finalize()` + `Release()`，否则每次录制都留一个坏文件。

### 3.4 AVI 要不要留？

三个选择：

| 方案 | 成本 | 说明 |
|---|---|---|
| **按扩展名分派**（推荐） | 最低 | 保存对话框同时给 `.mp4` 和 `.avi`，`.mp4` → MF，`.avi` → 现有 VFW。用户想用 x264vfw 的继续用，XP 源码构建也不受影响 |
| 只出 MP4，删掉 VFW | 中 | 少 300 行，但砍掉了"用某个特定 VFW 编解码器"的能力，也会让老用户的工作流断掉 |
| MF 编码 + 自己写 AVI muxer | 高 | 要自己写 RIFF/idx1/interleaving，约 400~600 行，收益只有"AVI 容器 + 现代编码器"，不值 |

### 3.5 工作量（估算，不是实测）

| 项 | 人日 |
|---|---|
| `IVideoWriter` 抽象 + `AVIWrite` 适配 + 调用点改类型 | 0.5 |
| `MFVideoWriter`（SinkWriter、格式协商、时间戳、`Finalize`） | 1.5~2 |
| 编码线程 + 队列 + 丢帧策略 | 1 |
| `utilWriteRGB32` + `WinSystem.cpp` 出图改造 | 0.5 |
| 设置项（码率/质量预设）+ 扩展名分派 | 0.5~1 |
| 回归（7 个渲染后端 × 2 种 captureScaled × 3 种 soundQuality × 音画同步） | 1~2 |
| **合计** | **5~7 人日** |

### 3.6 风险

- 硬件 MFT 的可用性因机器而异（老显卡、虚拟机、远程桌面）。**必须有软编兜底**：
  `MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER)`
  失败就直接用系统 H.264 软编 MFT，这条永远在。
- 码率/质量预设怎么暴露给用户：MF 没有 VFW 那种万能对话框，得自己做一个小对话框。
  最省事的做法是 3 档预设（高/中/低 = 8/4/2 Mbps），别做 CRF——MF 的 H.264 MFT 不直接给 CRF。
- 现在 `AVISaveOptions` 那一步在 `Open()` 里弹窗，等于"点开始录制 → 弹编码器选择"。
  MF 路线没有这一步，交互会变（点开始就录）。这是改进，但要记得 UI 文案别再说"选压缩器"。

---

## 4. 路线 B：D3D12 视频编码

### 4.1 它到底是什么

`ID3D12VideoEncoder` / `ID3D12VideoEncoderHeap` / `ID3D12VideoEncodeCommandList2`，
2021-12 由 DirectX 博客公布，**Windows 10 2004（10.0.19041）起**，需要 SDK 与驱动同时支持。
它做的是：给一张 NV12/P010 的 D3D12 纹理，吐 Annex-B 码流。

> 顺带说明：FFmpeg 里那个叫 `d3d12va` 的 hwaccel 是**解码**用的，本程序没有视频输入可解，
> 不适用。这里按"用 D3D12 做硬件编码"理解。

### 4.2 为什么在这个程序里收益接近 0

- **没有 GPU 帧**。帧源是 `pix`（CPU 上的 RGB 缓冲），渲染器都是"上传 `pix` 再画"。
  所以不管走 MF 还是 D3D12，**第一步都是 CPU→GPU 上传**，零拷贝的好处从一开始就不存在。
- **编码器不吃 RGB**。D3D12 编码输入是 NV12/P010，RGB→NV12 还是得做——MF 让 Video Processor
  MFT（GPU）或我们自己做；D3D12 路线得自己写一个 compute/pixel shader 做，工作更多。
- **没有软件兜底**。D3D12 视频编码依赖驱动能力，`CheckFeatureSupport` 不通过就完全没得录。
  老显卡、ARM64（Qualcomm）、虚拟机、远程桌面都可能是黑的。⇒ MF 那条路无论如何都得留着，
  D3D12 只是**第二条**编码路径。
- **支持的 GPU 面比 MF 硬件 MFT 更窄**。MF 硬件 MFT 就是厂商同一套硬件编码器包了个 MF 壳，
  覆盖面只会更广不会更窄。

### 4.3 什么情况下 D3D12 才有意义

只有一个：**改成"录屏幕上那一帧"**（像 OBS 的游戏捕获），并且要求 4K 不读回 CPU。

那时需要：7 个后端各实现一个"交出可共享纹理"的接口（`Display.h` 加虚函数）

| 后端 | 能否交出可共享纹理 | 桥到 D3D12 的路 |
|---|---|---|
| D3D11 | 能 | 纹理加 `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` → `ID3D12Device::OpenSharedHandle` |
| D3D9 / DDraw / GDI | 不能 | 只能 CPU 读回 |
| OpenGL / OpenGL3 | 能（`WGL_NV_DX_interop2`） | GL → D3D11 → D3D12，两跳 |
| WebGPU | 不能（wgpu_native 没暴露共享句柄） | CPU 读回 |

也就是说，**7 个后端里只有 2 个能零拷贝**，剩下 5 个还是 CPU 路径。为了这 2 个再写一套
D3D12 编码，性价比是负的。

### 4.4 容器怎么办

好消息：不需要自己写 MP4 muxer。`MFCreateMPEG4MediaSink` 可以直接吃**已编码**的
`MFVideoFormat_H264` 样本（不需要 SinkWriter 去编码），D3D12 出来的 Annex-B 码流可以
喂进去。但要自己管时间戳、sample 时长、音视频 interleaving。
（这条**没实测过**，属 §7 待验证项。）

### 4.5 工作量与风险

| 项 | 人日 |
|---|---|
| D3D12 设备/队列/fence/描述符堆（或 D3D11on12） | 2~3 |
| 编码器堆 + 编码器 + 命令列表 + 元数据 resolve | 3~5 |
| RGB→NV12（GPU shader 或 CPU） | 1~2 |
| 码流读回 + MPEG4 sink 喂样本 + 时间戳 | 2~3 |
| 能力探测与降级到 MF | 0.5 |
| 回归 | 2~3 |
| **合计** | **10~17 人日**，且只在部分 GPU 上有意义 |

风险：SDK/驱动版本差异大、没有参考实现可抄（OBS 走的是 AMF/NVENC/QSV SDK 和 FFmpeg，
不是 D3D12 视频编码）、调不出来时没有任何东西可退。

---

## 5. 路线 C（对照）：不改架构，只修 VFW 的痛点

如果只想解决"用户选不到编码器"：

1. 用 `ICInfo`/`ICOpen` 自己枚举 VFW 编解码器，做一个自己的对话框，替掉 `AVISaveOptions`
   （约 200 行）。能改善体验，但**改变不了"没装编解码器就没得录"这个事实**。
2. 加一个"未压缩 AVI"兜底选项（现在其实就有，叫 Full Frames）。
3. 修 `AVIIF_KEYFRAME`（只在 I 帧标，或者干脆不标）。
4. 把 `AVIStreamWrite` 挪到写线程。

这四条加起来约 1~2 人日，能让现有链路不那么痛，但**出不了 MP4、也吃不掉硬件编码器**。
可以作为路线 A 之前的一步，也可以不做。

**自带编码器（x264 / FFmpeg）** 也在选项里：项目是 GPLv2，链接 x264 没有许可证问题；
但会给一个几 MB 的小 exe 加上几 MB 依赖、外加交叉编译的麻烦。MF 已经把"系统自带的 H.264
软编 + 硬件编"都给了，**没有必要**。

---

## 6. 建议

1. **先做 `IVideoWriter` 抽象**（0.5 人日，零行为变化）。这一步单独提交，方便回退。
2. **再做 `MFVideoWriter` + 按扩展名分派**（4~6 人日）。VFW 路径原样保留。
3. **不要碰 D3D12**，除非将来真要做"录屏幕上那一帧"，那时再连 §4.3 的纹理共享一起评估。
4. 顺手修的：`AVIIF_KEYFRAME`（`AVIWrite.cpp:242`）、编码线程、`captureScaled` 下的奇数尺寸。

---

## 7. 待验证（本机没跑过，不写"已验证"）

| # | 事项 | 怎么验 |
|---|---|---|
| 1 | 干净 Win10/11 x64 上 VFW 还剩哪些编码器 | 起一个空工程枚举 `ICInfo`，或看注册表 `HKLM\SOFTWARE\Classes\CLSID\{...}\Drivers` |
| 2 | SinkWriter 收 `MFVideoFormat_RGB32` 能否自动插 VP MFT | 最小 SinkWriter 程序，喂 RGB32 出 MP4 |
| 3 | `soundQuality == 4`（11025 Hz）经 AAC 是否正常 | 录 10 秒，看时长与波形 |
| 4 | ARM64 上 `mfh264enc.dll` 是否存在 | `ls C:\Windows\System32\mfh264enc.dll` + 在 ARM64 上跑 |
| 5 | `MFCreateMPEG4MediaSink` 是否接受预编码 H.264 样本 | 只有走 D3D12 路线才需要 |
| 6 | 硬件 MFT 在本机（显卡型号）上是否被 `MFTEnumEx` 枚举到 | 最小枚举程序 |
| 7 | 7 个后端 × 录制组合是否都不崩 | 手动回归 |
