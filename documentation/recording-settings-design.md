# 录制设置：先选编码器，由编码器决定容器和扩展名

分析对象：`C:/Users/zhang/vba-rerecording`
前置文档：`documentation/avi-recording-encode.md`（现状链路 + MFT/D3D12 可行性）
日期：2026-10-25

---

## 0. 结论

> **实现状态（2026-10-25）**：已落地两轮，**不含并发**（§10 留作下一步）。
> 已跑通的验证：x64 全量 178 个编译单元零诊断、链接出 exe；
> `IDD_RECORD_SETTINGS` 模板渲染正常；用一个直接链接 `MFVideoWriter.obj` 的探针
> 录了 120 帧合成画面 + 音频，ffprobe 判读四条路径全部正确——
> H.264 硬件 / H.264 软件 + AAC → `.mp4`（240×160、59727/1000 fps、aac 44100 立体声、2.02 s）、
> 纯 AAC → `.m4a`、纯 MP3 → `.mp3`。细节见 §15。

1. **菜单分两套**：新版 `Start/Stop Recording...`（MF，选编码器 → 保存），
   旧版收进 `Legacy AVI Recording` 子菜单（`AVI Recording` + `WAV Recording`）。
   两套互斥，同一时间只能录一个；WAV 独立，可以和任一套同时录。
2. **容器不由用户选**，由 `(视频 codec, 音频 codec)` 查表得到，保存对话框的扩展名跟着锁死。
3. **新版能只录音频**：视频列表里有一条 `None (audio only)`，此时容器由音频 codec 决定
   （AAC → `.m4a`、MP3 → `.mp3`、FLAC → `.flac`）。
4. **但它替代不了旧版 WAV，也不该替代。** WAV 是 **PCM 直通、零编码、零失败面**，
   MF 出不了 WAV 容器（就算能，包一层也是纯亏）。两者是互补的：
   WAV 给后期处理和音画对齐，`.m4a` 给分享。
   详见 §6。
5. **"零拷贝"严格意义做不到**，因为帧源 `pix` 在 CPU 上。但——
   **"只允许 D3D11 渲染器"这个前提是多余的**：编码链内部的 GPU 零拷贝
   （Video Processor MFT → 硬件编码器全程不上 CPU）只需要给 SinkWriter 一个
   **自己创建的** D3D11 设备，跟渲染器是 GDI 还是 OpenGL 无关。
   而它省掉的那一步（`pix` 上传，154 KB/帧）只值 0.02 ms，所以第一版不做。详见 §9。
6. **并发只做一件事**：三槽环形缓冲 + 一个编码线程，模拟线程 `acquire → 画 → submit`，
   不阻塞。它不能让编码本身变快，但把模拟线程每帧的录制开销从 1~26 ms 压到 0.1~6 ms，
   并把编码尖峰吸收进队列。§10 有账。
   顺带纠正：**本程序没有全局 COM 初始化**（"用了 D3D 所以 COM 开着"不成立），
   但 `beginCom()` 的写法对两种情况都成立。

---

## 1. 菜单结构

```
Tools
├── Start/Stop &Recording...                      ID_TOOLS_RECORDING            ← 新版
├── &Pause/Resume Recording                       ID_TOOLS_PAUSERECORDING       ← 新版
├── Record at &Window Size and Filter             ID_TOOLS_CAPTURESCALED        （不动）
├── POPUP "&Legacy AVI Recording"                                               ← 新子菜单
│   ├── Start/Stop &AVI Recording...              ID_TOOLS_LEGACY_AVIRECORDING
│   ├── Pause/Resume AVI &Recording               ID_TOOLS_LEGACY_PAUSEAVIRECORDING
│   └── Start/Stop &WAV Recording...              ID_TOOLS_SOUNDRECORDING       （移进来）
├── SEPARATOR
├── Screenshot &Capture...                        ID_FILE_SCREENCAPTURE
└── POPUP "Screenshot &Format"
```

`Record at Window Size and Filter` 留在外面：它同时影响截图和两套录制（`theApp.captureScaled`），
不属于任何一套。

### 1.1 ID 处理

| 旧符号 | 新符号 | 值 | 说明 |
|---|---|---|---|
| `ID_TOOLS_AVIRECORDING` | `ID_TOOLS_RECORDING` | 40177 | **只改名，值不变**。`.rc` 里用的是符号，值不变所以菜单资源、`VBA.APS` 都不用管 |
| `ID_TOOLS_PAUSEAVIRECORDING` | `ID_TOOLS_PAUSERECORDING` | 42406 | 同上 |
| — | `ID_TOOLS_LEGACY_AVIRECORDING` | 42463 | 新，`_APS_NEXT_COMMAND_VALUE` 当前就是 42463 |
| — | `ID_TOOLS_LEGACY_PAUSEAVIRECORDING` | 42464 | 新 |
| `ID_TOOLS_SOUNDRECORDING` | 不变 | 40124 | 只是挪进子菜单 |

改名的影响面（已 grep 全仓）：`resource.h` 2 行、`vba.rc` 2 行、`MainWnd.cpp` 4 行
（message map）、`MainWnd.h` 4 个 `afx_msg` 声明、`MainWndTools.cpp` 4 个函数名。
**加速键表里没有引用这几个 ID**，所以不用改 `.rc` 的 `ACCELERATORS` 块。

函数名一起改：`OnToolsAVIRecording` → `OnToolsRecording`、`OnUpdateToolsAVIRecording` →
`OnUpdateToolsRecording`，旧的用 `OnToolsLegacyAVIRecording` 等。
不改名的话，`OnToolsAVIRecording` 挂着 MF 录制、旁边还有个 `OnToolsLegacyAVIRecording`，
半年后没人看得懂。

### 1.2 状态与互斥

`theApp` 加一个：

```cpp
enum RecordingBackend { RECORDING_NONE, RECORDING_MF, RECORDING_VFW };
RecordingBackend recordingBackend;
```

`aviRecording`（bool）保留，表示"有视频在录"。`soundRecording` 不动。

| 菜单项 | Enable / 文案 |
|---|---|
| 新版 Start/Stop | `RECORDING_NONE` → "Start"，`RECORDING_MF` → "Stop"，`RECORDING_VFW` → 灰 |
| 新版 Pause/Resume | 仅 `RECORDING_MF` 可用 |
| 旧版 Start/Stop AVI | `RECORDING_NONE` → "Start"，`RECORDING_VFW` → "Stop"，`RECORDING_MF` → 灰 |
| 旧版 Pause/Resume AVI | 仅 `RECORDING_VFW` 可用 |
| WAV | 不受 `recordingBackend` 影响（可以边录 MP4 边录 WAV，跟现在边录 AVI 边录 WAV 一样） |

`aviRecorder` 的类型从 `AVIWrite *` 变成 `IVideoWriter *`，两套共用这一个指针。

---

## 2. 交互流程

**新版**：

```
菜单 [Start/Stop Recording...]
  → Recording Settings
      视频编码器列表   Codec | Encoder | Type      （含 None (audio only)）
      音频编码器    [AAC (Microsoft)        v]     （含 None）
      质量          [High                   v]
      ─────────────────────────────────────
      摘要：MP4 (.mp4) · 240×160 · 59.73 fps · 8 Mbps
  → 保存对话框（filter 只有 "MPEG-4 Video (*.mp4)"，默认名 game.mp4）
  → 开始
```

**旧版 AVI**（保持现状，`MainWndTools.cpp:552`）：

```
菜单 [Legacy AVI Recording] → [Start/Stop AVI Recording...]
  → 保存对话框（默认 game.avi）
  → AVISaveOptions（VFW 编码器选择）
  → 开始
```

**旧版 WAV**（保持现状，`MainWndTools.cpp:478`）：

```
菜单 [Legacy AVI Recording] → [Start/Stop WAV Recording...]
  → 保存对话框（默认 game.wav）
  → 开始（格式取声卡缓冲的格式，PCM 直通）
```

---

## 3. 对话框

新资源 `IDD_RECORD_SETTINGS = 566`（`IDD_` 当前最大 565），控件 ID 从 `1501` 起
（`_APS_NEXT_CONTROL_VALUE` 就是 1501）。

```
+--------------------------------------------------------------+
|  Recording Settings                                          |
|                                                              |
|  Video encoder:                                              |
|  +--------------------------------------------------------+  |
|  | H.264 | NVIDIA H.264 Encoder MFT        | Hardware     |  |
|  | H.264 | Microsoft H.264 Video Encoder   | Software     |  |
|  | HEVC  | NVIDIA HEVC Encoder MFT         | Hardware     |  |
|  | -     | None (audio only)               |              |  |
|  +--------------------------------------------------------+  |
|                                                              |
|  Audio encoder: [ AAC (Microsoft)                        v ] |
|  Quality:       [ High                                   v ] |
|                                                              |
|  MP4 (.mp4) - 240x160 - 59.73 fps - 8 Mbps                   |
|  Encoded on the GPU. Low CPU load, the frame rate stays.     |
|                                                              |
|                              [ Start Recording ] [ Cancel ]  |
+--------------------------------------------------------------+
```

| ID | 类型 | 说明 |
|---|---|---|
| `IDC_ENC_VIDEO` | `SysListView32` `LVS_REPORT \| LVS_SINGLESEL \| WS_BORDER \| WS_TABSTOP` | 三列：Codec 56 / Encoder 150 / Type 56 |
| `IDC_ENC_AUDIO` | `COMBOBOX` `CBS_DROPDOWNLIST \| WS_VSCROLL \| WS_TABSTOP` | 由视频 codec 过滤，末尾一条 `None` |
| `IDC_ENC_QUALITY` | `COMBOBOX` `CBS_DROPDOWNLIST \| WS_VSCROLL \| WS_TABSTOP` | High / Medium / Low |
| `IDC_ENC_SUMMARY` | `LTEXT` | 容器 · 分辨率 · 帧率 · 码率 |
| `IDC_ENC_HINT` | `LTEXT` | 随选中项变：硬件/软件/纯音频说明 |
| `ID_OK` / `ID_CANCEL` | 按钮 | 项目惯例（不是 `IDOK`/`IDCANCEL`） |

资源模板照抄 `IDD_THROTTLE`（`vba.rc:968`）的写法：
`DIALOGEX` + `FONT 8, "MS Sans Serif", 0, 0, 0x0`。**字体必须照抄 8pt MS Sans Serif**——
`uiFont` 的钩子只替换这个模板字体，换成别的会跳过（`documentation/unicode-and-l10n.md`）。
暗色模式不用管：`darkMode` 只管标题栏和菜单栏，对话框保持浅色（`DarkMode.h:7-9`）。

行为：

- 列表选中变化 → 重新算音频候选、重算摘要、更新 hint。
- 硬件项 `Type` 列显示 `Hardware`，软件项 `Software`，`None (audio only)` 留空。
- 排序：硬件在前，同组内按 codec 顺序（H.264 → HEVC → AV1），`None` 永远在最后。
- 默认选中：上次用的编码器（注册表里有且还在列表里），否则第一个硬件项，否则第一个软件项。
- 视频选 `None` → 音频下拉去掉 `None` 项（否则两个都空），摘要改成
  `M4A (.m4a) - AAC - 44.1 kHz stereo`，hint 说明"只录音频，不录画面"。
- 视频和音频**不能同时为 None**，否则 `Start Recording` 禁用。
- 没有任何 MF 编码器 → 列表里只有 `None`，hint 说明原因；用户还可以只用旧版。

---

## 4. 编码器枚举

### 4.1 数据结构

```cpp
// src/win32/VideoEncoders.h
struct VideoEncoderChoice {
    GUID         subtype;    // MFVideoFormat_H264 / HEVC / AV1；None 条目全 0
    bool         hardware;
    std::wstring name;       // 代表性 MFT 的 MFT_FRIENDLY_NAME_Attribute
    std::wstring vendor;     // "NVIDIA" / "AMD" / "Intel" / ""（软件为空）
    bool         available;  // false = None (audio only)
};

struct AudioEncoderChoice {
    GUID         subtype;    // MFAudioFormat_AAC / MP3；None 条目全 0
    std::wstring name;
    bool         available;
};
```

**没有 CLSID 字段**：`IMFSinkWriter` 不接受指定 MFT，能传给它的只有
`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS` 这一个开关。所以一行 =
"这个 codec + 硬件/软件"，不是"某个厂商"。同一 (codec, 硬件) 组合只出一行，
取第一个 MFT 的名字和厂商——本机实测 H.264 硬件那条会同时看到 NVIDIA 和
一个 `VEN_1414` 的 MFT，合并厂商会显示成 "NVIDIA, Microsoft"，反而误导，所以只取第一个。

### 4.2 枚举算法

```cpp
// 表里加一行就是一个新 codec；VP9/MPEG-4 之类同理，别硬编码"支持哪些"。
static const GUID kVideoCodecs[] = {
    MFVideoFormat_H264, MFVideoFormat_HEVC, MFVideoFormat_AV1,
};

for (GUID codec : kVideoCodecs) {
    MFT_REGISTER_TYPE_INFO out = { MFMediaType_Video, codec };
    IMFActivate **acts = NULL; UINT32 count = 0;
    MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL,
              NULL /* 任意输入 */, &out, &acts, &count);
    for (UINT32 i = 0; i < count; ++i) {
        // name:     MFT_FRIENDLY_NAME_Attribute (VT_LPWSTR)
        // flags:    MF_TRANSFORM_FLAGS_Attribute  (VT_UINT32)
        // hardware: (flags & MFT_ENUM_FLAG_HARDWARE) != 0
        // vendor:   MFT_ENUM_HARDWARE_VENDOR_ID_Attribute (VT_LPWSTR, "VEN_10DE")
        //           → 映射表：10DE=NVIDIA, 1002=AMD, 8086=Intel, 1414=Microsoft,
        //             5143=Qualcomm
        acts[i]->Release();
    }
    CoTaskMemFree(acts);   // 每个 activate 都要 Release，数组本身再 CoTaskMemFree
}
```

四个必须注意的点：

1. **按 codec 逐个查**，用 `pOutputType` 过滤。这样"这个编码器能出什么 codec"是查出来的，
   不需要 `ActivateObject` 之后再去 `GetOutputAvailableType`（那要为每个 MFT 建实例，
   硬件 MFT 会加载驱动，很慢）。
2. **硬件判定**用 `MF_TRANSFORM_FLAGS_Attribute & MFT_ENUM_FLAG_HARDWARE`，不要猜名字。
   兜底：`MFT_ENUM_HARDWARE_VENDOR_ID_Attribute` 存在也算硬件（个别驱动不设 flags，见 §14）。
3. **位数**：硬件 MFT 是 in-proc，32 位进程只看得到 32 位 MFT。Win32 构建可能看不到
   某些硬件编码器——这是实测项（§14.1），不是 bug。
4. **首次枚举会加载驱动**（可能 200~500 ms）。先做**同步枚举 + 进程内缓存**；
   实测超过 ~300 ms 再改成后台线程 + "Detecting encoders..." 占位。

音频同理，`MFT_CATEGORY_AUDIO_ENCODER` + `MFAudioFormat_AAC / MP3`。
**FLAC 没做**：`MFAudioFormat_FLAC` 在 `_WIN32_WINNT 0x0602` 下没声明（Win10 才有），
而 WAV 已经覆盖了无损需求，多一条 Win10-only 的容器不值。
**音频不标硬件**：MF 生态里没有 GPU 音频编码 MFT 的先例，硬标会骗人。

### 4.3 MF 的动态加载

按 README 的"Nothing is imported statically"（`README.md:565-572`）和
`Direct3D11.cpp:73` 的写法，写一个 `MFLoader`：

| DLL | 函数 |
|---|---|
| `mfplat.dll` | `MFStartup` `MFShutdown` `MFTEnumEx` `MFCreateAttributes` `MFCreateMediaType` `MFCreateMemoryBuffer` `MFCreateSample` |
| `mfreadwrite.dll` | `MFCreateSinkWriterFromURL` |

`mfuuid.lib` 可以静态链（只有 GUID 常量，不产生 DLL 导入），但为了和
`ImageWrite.cpp:21` 的做法一致，也可以自己 `DEFINE_GUID`。**不要**链 `mfplat.lib` /
`mfreadwrite.lib`，那会把这两个 DLL 写进导入表，XP-flavoured 源码构建直接起不来。

COM：主线程没有 apartment，照 `ImageWrite.cpp:224` 的 `beginCom()` 写一个作用域内的
`CoInitializeEx` + `MFStartup` / `MFShutdown`，别动 `VBA::InitInstance()`。

---

## 5. 容器决策表

容器 = f(视频 codec, 音频 codec)。不兼容的组合在对话框里直接禁用，不让用户选到。

| 视频 codec | 音频 codec | 容器 | 扩展名 |
|---|---|---|---|
| 任意 | 任意可用的音频 codec | MP4 | `.mp4` |
| 任意 | 无音频 | MP4 | `.mp4` |
| **无视频** | AAC / ALAC / Opus / AC-3 | MP4（纯音频） | `.m4a` |
| **无视频** | MP3 | MP3 | `.mp3` |
| **无视频** | FLAC | FLAC | `.flac` |
| **无视频** | 无音频 | 不合法 | — |

```cpp
const ContainerDesc *containerFor(const GUID &video, const GUID &audio);
// 返回 NULL = 不合法，对话框据此禁用该组合
```

`.m4a` 和 `.mp4` 是同一个容器（MPEG-4）只是扩展名不同，`MFCreateMPEG4MediaSink` 直接出。
MP3 走 `.mp3`（MP3 sink），AAC 走 `.m4a`（还是 MPEG-4 容器，只是扩展名不同）。
容器完全由 `MFCreateSinkWriterFromURL` 按扩展名推断，所以 `RecordContainer`
里只存扩展名和保存对话框的 filter id，不存 `MFTranscodeContainerType_*`。

保存对话框的参数由容器推出来（`FileDlg` 已经支持）：

```cpp
LPCTSTR exts[] = { desc->extension, NULL };          // ".mp4"
CString filter = winResLoadFilter(desc->filterId);   // "MPEG-4 Video_*.mp4_All Files_*.*__"
FileDlg dlg(this, baseName + desc->extension, filter, 1, desc->extension, exts,
            aviDir, winResLoadString(IDS_SELECT_RECORD_NAME), true);
```

`IDS_AVI_DIR` 这个注册表键**不改名**（改了就丢用户的目录设置），只改显示文案。

---

## 6. 只录音频 vs 旧版 WAV：为什么不能替代

### 6.1 能做，但不是同一件事

| | 旧版 WAV（`WavWriter`） | 新版纯音频（MF） |
|---|---|---|
| 编码 | **无，PCM 直通** | AAC / MP3 有损，FLAC 无损 |
| 代码路径 | 声卡回调里直接 `fwrite`（`WavWriter.cpp:84`） | SinkWriter + 编码线程 + 队列 |
| 失败面 | 只有磁盘满 | 编码器缺失、容器不支持、MF 初始化失败、队列溢出 |
| 体积 | 10.6 MB/分钟（44.1k / 16bit / 立体声） | ~1 MB/分钟（AAC 128 kbps） |
| 用途 | 后期处理、TAS 音画对齐、无损存档 | 分享、试听 |
| 格式来源 | 直接取 `dsbSecondary->GetFormat()`，跟声卡完全一致 | 声明 PCM，MF 自己重采样 |

**WAV 的核心价值是"没有编码器"**。它不需要 MF、不需要协商、不会因为驱动更新而坏。
把它换成 MF 的音频链，是拿"零失败面"换"文件小一点"，对一个 TAS 模拟器来说不划算。

### 6.2 也不该在 UI 里统一

有人会想：新版音频下拉里加一条 `PCM (WAV)`，选中就复用 `WavWriter`，入口就统一了。
不建议，理由跟上一轮否掉"编码器列表里塞 AVI 条目"是同一个：

- `PCM` 不是编码器，是一个容器。混进编码器列表，跟 `None` 并列会让人困惑。
- 代码还是两套（`WavWriter` 不可能被 MF 替掉），只是多了一层分派，收益只有"少一个菜单项"。
- 而菜单已经按你的要求分成两套了，WAV 待在旧版子菜单里语义正确——它本来就是旧版的。

### 6.3 结论

**两者并存**：WAV 留旧版子菜单，新版纯音频作为"压缩录音"独立存在。
新版对话框里 `None (audio only)` + AAC 出 `.m4a`，这就是它的用途。

---

## 7. 数据模型与设置持久化

```cpp
// src/win32/VideoWriter.h
struct RecordParams {
    GUID videoSubtype;    // 全 0 = 不录视频
    bool videoHardware;   // 让 sink writer 优先用硬件 MFT
    GUID audioSubtype;    // 全 0 = 不录音频
    int  videoBitrate;    // kbps，100..100000，预设 2000/4000/8000
    int  audioBitrate;    // kbps，32..512，预设 64/128/192
};
```

容器和扩展名不进 `RecordParams`：它们是 `containerFor()` 的输出，不是输入。

`theApp` 加一个 `RecordParams recordParams`，在 `VBA::loadSettings()`（`VBA.cpp:2443`）/
`saveSettings()`（`:2845`）里读写：

| 键 | 值 |
|---|---|
| `recordVideoCodec` | subtype GUID 字符串；**键不存在** = 首次运行（用 H.264）；`"none"` = 只录音频 |
| `recordVideoHardware` | DWORD，1 = 硬件优先 |
| `recordAudioCodec` | subtype GUID 字符串；**键不存在** = 首次运行（用 AAC）；`"none"` = 不录音频 |
| `recordVideoBitrate` | DWORD，kbps，默认 8000 |
| `recordAudioBitrate` | DWORD，kbps，默认 192 |

"键不存在"和 `"none"` 必须分开：`regSetStringValue` 写空串等于删除值，读回来会和
"从没选过"撞在一起。

码率（质量档 → `MF_MT_AVG_BITRATE`）：Low 2 Mbps / Medium 4 Mbps / High 8 Mbps。
纯音频时质量档映射到音频码率：Low 64k / Medium 128k / High 192k。
硬件编码器在高码率下更快，所以默认 High。

---

## 8. 与现有代码的接缝

| 文件 | 改动 |
|---|---|
| `src/win32/VideoWriter.h`（新） | `IVideoWriter`：`open / acquireFrameBuffer / submitFrame / addSound / pause / isPaused / frames / videoWidth / videoHeight`。`AVIWrite` 现在就是这一组签名，只多两个方法（旧版实现成同步，行为不变） |
| `src/win32/AVIWrite.{h,cpp}` | 继承 `IVideoWriter`；`Open(name, const AVICOMPRESSOPTIONS *opts)`，`opts == NULL` 时弹 `AVISaveOptions`（`NextSegment()` 传 `&m_options`） |
| `src/win32/MFVideoWriter.{h,cpp}`（新） | `IMFSinkWriter` 实现；视频流可选（纯音频时不 `AddStream` 视频） |
| `src/win32/VideoEncoders.{h,cpp}`（新） | `MFLoader` + 枚举 + 容器表 + 默认值 |
| `src/win32/Dialogs/RecordSettings.{h,cpp}`（新） | 对话框 |
| `src/win32/VBA.h/.cpp` | `AVIWrite *aviRecorder` → `IVideoWriter *`；加 `recordParams`、`recordingBackend`、四个注册表键 |
| `src/win32/MainWnd.cpp:636-643` | message map：两个 ID 改名 + 四个 legacy 新 ID |
| `src/win32/MainWnd.h` | 4 个 `afx_msg` 改名 + 4 个新增 |
| `src/win32/MainWndTools.cpp` | 改名 + 新增 legacy 两个 handler；`OnToolsStartRecording` 改成：设置对话框 → 保存对话框 → 建 writer |
| `src/win32/WinSystem.cpp` | 只有类型变化；`systemRecordScaledFrame()` 加一个"输出 RGB32 自顶向下"的分支 |
| `src/common/Util.cpp` | 加 `utilWriteRGB32(out, w, h, depth, pix, topDown)`，和 `utilWriteBMP`（`:66`）同构 |
| `src/win32/DirectSound.cpp:405` / `XAudio2Sound.cpp:386` | 只有类型变化 |
| `src/win32/resource.h` / `vba.rc` | 两个 ID 改名 + 两个新 ID + 菜单结构 + 新对话框 + 新字符串 |
| `src/win32/lang/chs/*.tsv` | 新条目（见 §11） |

`WavWriter.{h,cpp}` **不动**。

---

## 9. 性能：硬件编码与"零拷贝"到底能省什么

### 9.1 每帧的成本账（估算，量级用）

| 阶段 | 240×160（默认） | 1440×960（captureScaled 6x） |
|---|---|---|
| CPU 滤镜 + 最近邻放大（`systemRenderScaledFrame`，`WinSystem.cpp:290`） | 不走 | **3~6 ms** |
| `pix` → RGB32 转换 | 0.05~0.1 ms | 同左（源尺寸不变） |
| 上传到 D3D11 纹理（154 KB / 5.5 MB） | 0.02 ms | 0.7 ms |
| Video Processor MFT RGB→NV12（GPU） | 0.05 ms | 0.2 ms |
| H.264 编码（硬件） | 1~2 ms | 2~4 ms |
| H.264 编码（微软软编） | 2~5 ms | 8~20 ms |

结论：
- 默认分辨率下，**CPU 转换 + 上传 ≈ 0.1 ms，占整帧不到 5%**。零拷贝省的正是这 0.1 ms。
- 真正的大头是 captureScaled 的 CPU 滤镜 + 放大（3~6 ms），它跟编码器无关，也跟零拷贝无关。
- 硬件编码省的是 CPU 占用（软编 2~5 ms → 硬编 1~2 ms，且不占 CPU 核），
  240×160 这种小画面软编其实也跑得动。所以"硬件"的价值主要在 captureScaled 和高码率下。

### 9.2 "只允许 D3D11 渲染器才能零拷贝"——这个前提不成立

零拷贝有两种，别混：

| 说法 | 能不能 | 说明 |
|---|---|---|
| **源纹理零拷贝**（`pix` 不上传） | **不能** | `pix` 是模拟器核心写的 CPU 缓冲，7 个渲染器都是"上传它再画"。核心不可能渲染到 GPU 纹理 |
| **编码链零拷贝**（VP MFT 输出 NV12 直接进硬件编码器，不上 CPU） | **能，且跟渲染器无关** | `MF_SINK_WRITER_D3D_MANAGER` 允许给 SinkWriter 一个**你自己创建的** `ID3D11Device`（`D3D11CreateDevice` + `MFCreateDXGIDeviceManager`），不需要借用渲染器的设备 |

所以"只允许 D3D11 渲染器"没有必要——用户用 GDI 也能享受编码链内部的 GPU 通路。
需要自己建 D3D11 设备的代价是：多一个设备（`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`）、
多一条 device-removed 的失败路径，而且 `MFCreateDXGIDeviceManager` 是 **Win8+**——
Win7 上这条路不存在，只能走 CPU（Win7 本来也在支持范围内，`README.md:573`）。

### 9.3 如果真去录渲染器的 back buffer

那是另一件事，收益和代价不成比例：

- **语义变了**：录的是"屏幕上那一帧"——含 GDI/OSD 文字、黑边、窗口尺寸、GPU 放大滤镜
  （NIS/FSR/xBRZ shader）。跟现在 AVI 的"录模拟器帧"不是一回事，两条路要都留着。
- **绑死在 D3D11**：GDI/DDraw/D3D9/OpenGL/WebGPU 用户不能录。
- **Present 之后 back buffer 失效** → 必须 `CopyResource` 到一张共享纹理（+0.1 ms）。
- **分辨率随窗口变**：MP4 的分辨率不能中途变，要么锁尺寸、要么重开文件、要么裁黑边。
- **device removed**（Alt-Tab、全屏切换、驱动更新）→ 录制中断。
- 省下来的只有 154 KB/帧的上传 = 0.02 ms。

**不值得。**

### 9.4 值得做的 GPU 优化（第二阶段，独立开关）

captureScaled 时把 CPU 的滤镜 + 放大换到 GPU：

```
pix (CPU, 154 KB) --上传--> D3D11 纹理
                             ↓ 复用 upscalers/*.hlsl（xBRZ / NIS / FSR / 2xSaI…）
                          离屏 RT（1440×960，GPU）
                             ↓ 直接作为 Video Processor MFT 的输入
                          NV12 → 硬件编码器
```

要点：
- 用**编码自己的** D3D11 设备，跟渲染器解耦（用户用 GDI 也能走这条路）。
- shader 源码就是 `src/win32/upscalers/*.hlsl`，D3D11 渲染器已经用
  `D3DCompile`（`d3dcompiler_47.dll`，`Direct3D11.cpp:78` 动态加载）在跑，可以直接复用。
- 离屏 RT 的尺寸由我们定（不随窗口变），所以分辨率问题不存在，也不用裁黑边。
- 省的是 3~6 ms/帧，这是唯一值得上 GPU 的地方。

第一版不做，留成 `recordGpuScale` 开关。

### 9.5 让硬件编码真的快起来

1. `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = TRUE`。
2. 硬件项排在列表前面并默认选中。
3. **编码线程 + 3~4 帧队列**。`WriteSample` 返 `MF_E_NOTACCEPTING` 就丢帧计数，
   绝不阻塞模拟线程（现在是 `AVIStreamWrite` 同步阻塞，`AVIWrite.cpp:237`，
   这是录制掉速的根因）。
4. 质量档默认 High（8 Mbps）——硬件编码器在高码率下更省力。

**纯音频也一样要队列**，但理由跟视频不同：音频块是在**模拟线程**上产生的
（`systemSoundNext()` → `systemSoundWriteToBuffer()`，`common/System.cpp:648`），
不是声卡回调线程——本程序是纯单线程。所以不存在 COM 的问题，
只是"写 SinkWriter 这件事必须在编码线程做"，主线程只往队列里塞。

---

## 10. 并发模型：把编码从模拟线程挪走

### 10.1 先纠正两个前提

**"COM 是默认开的"——不对。** 全仓只有三处 `CoInitializeEx`，都是"用之前自己调"：
`ImageWrite.cpp:228`（截图）、`ModernFileDialog.cpp:411/560`（文件对话框），
外加 `lua-engine.cpp:5152` 一个 `CoInitialize(0)`。没有 `AfxOleInit`，没有全局初始化。
`ImageWrite.cpp:221-223` 的注释比我说的清楚：

> The emulator does not otherwise initialise COM on these threads; tolerate a
> caller (or MFC) that already did.

而且 D3D9/D3D11 的 `CreateDevice`、DDraw、DirectSound **都不需要** COM 初始化，
所以"用了 D3D ⇒ COM 开着"这个推断不成立。

这不影响实现：`beginCom()` 对"已初始化（STA 或 MTA）"和"未初始化"两种情况都覆盖
（`RPC_E_CHANGED_MODE` 那个分支）。真正要记住的是 §10.4 那条——SinkWriter 必须在编码线程建。

**"纯单线程"——对。** `systemSoundWriteToBuffer()` 是从 `systemSoundNext()`
（`common/System.cpp:648`）调的，它在模拟核心的音采样路径上，**跟视频一样在模拟线程**。
（我上一版文档说"音频来自声卡回调线程、没有 COM apartment"，是错的，这里更正。）
单线程也意味着：整条录制路径上唯一需要并发的就是"编码"这一件事，不用线程池。

### 10.2 现状为什么堵

```
systemRenderFrame()                        模拟线程
├─ systemRecordAviFrame()
│    ├─ systemRenderScaledFrame()          CPU 滤镜 + 最近邻放大   3~6 ms @1440x960
│    └─ AVIStreamWrite()                   同步编码                1~20 ms
├─ ifb
└─ display->render()                       又一个 CPU 滤镜 + 上传 + GPU 缩放
```

`AVIStreamWrite`（`AVIWrite.cpp:237`）是同步的，卡在准备阶段的末尾，把整条流水线堵死。
你要的"编上一帧的同时准备下一帧"，本质就是把这个同步调用换成入队。

### 10.3 方案：三槽环形缓冲 + 一个编码线程

```
模拟线程                                   编码线程
──────────────────────────────────         ──────────────────────────
acquireFrameBuffer() → 空槽
  CPU 滤镜 + 放大，直接写进槽（零拷贝）
submitFrame(frameIndex)
  ↑ 不阻塞；三个槽都满 → 丢帧
                                           takeFrame() → 满槽
                                             WriteSample(帧, 时间戳)
                                             归还槽
```

接口只加两个方法：

```cpp
virtual u8 *acquireFrameBuffer() = 0;          // NULL = 队列满，调用方丢帧
virtual void submitFrame(long long time100ns) = 0;
```

`AVIWrite`（旧版）实现成"acquire 返回内部唯一缓冲、submit 立即同步写"——行为完全不变，
旧版一行逻辑都不用改。`MFVideoWriter` 才是三槽 + 线程。

三个必须写对的细节：

1. **缓冲所有权**。现在 `systemRenderScaledFrame()` 返回一个 static 缓存
   （`WinSystem.cpp:329-338`），`AddFrame` 同步消费完。异步后如果还用它，
   主线程下一次调用就会覆盖正在编码的数据。所以**必须由 writer 提供缓冲**，
   主线程直接往槽里画，不做额外 memcpy。
2. **时间戳由帧号算，不能用墙钟**。暂停、丢帧、掉速都不能让时间戳跳。
   主线程维护 `submittedFrames`，**丢帧也 +1**，时间戳照常前进。
   MP4 有显式时间戳，所以丢帧就是"少一帧但时长正确"，比现在"复制同一帧"
   （`WinSystem.cpp:445-509` 的 `linearSoundFrameCount` 补偿）更干净。
3. **音频不能丢**。音频队列深一点（0.5 s ≈ 30 块，每块 2940 字节），满了就阻塞模拟线程——
   它本来就在 `sound->write()` 里等声卡位置，多等一会儿无所谓。

### 10.4 线程与 COM

- 编码线程 `CoInitializeEx(NULL, COINIT_MULTITHREADED)` + `MFStartup`，
  **SinkWriter 也在编码线程创建**。
  理由：如果哪天主线程被某个组件初始化成 STA，主线程创建的 SinkWriter 跨线程调用
  需要 marshalling，MF 不保证。在编码线程建最安全。
- 所以 `open()` 是异步的：起线程 → 线程建 SinkWriter/编码器 → `SetEvent` 报结果 →
  主线程 `WaitForSingleObject`（超时 2~3 s）。失败就删半成品文件、不开始录制。
- 线程优先级 `THREAD_PRIORITY_BELOW_NORMAL`：TAS 用户对时序敏感，别跟模拟抢 CPU。
- `close()`：置停止标志 → signal → 编码线程 drain + `Finalize()` + `MFShutdown` +
  `CoUninitialize` → join。`Finalize` 写 moov 可能几十 ms，主线程等待时挂个 `CWaitCursor`。

### 10.5 能快多少（诚实版）

| 场景 | 串行（现状） | 异步后编码耗时 | **模拟线程每帧的录制开销** |
|---|---|---|---|
| 240×160，硬件 | 1.1~2.1 ms | 2 ms | **0.1 ms** |
| 240×160，软编 | 2.1~5.1 ms | 5 ms | **0.1 ms** |
| 1440×960，硬件 | 5~10 ms | max(6, 4) = 6 ms | **3~6 ms**（滤镜还在主线程） |
| 1440×960，软编 | 11~26 ms | max(6, 20) = 20 ms | **3~6 ms** |

三个结论：

1. **平均耗时不一定变短**——异步不能突破"编码本身要 N ms"的下限。
   编码比帧间隔长，队列照样满、照样丢帧。
2. **真正省的是模拟线程的时间**：从 1~26 ms 降到 0.1~6 ms。这才是"录制不影响模拟"。
3. **抖动被队列吸收**：编码偶发尖峰（关键帧、场景切换、驱动内部调度）不再变成模拟卡顿。

### 10.6 顺带发现：captureScaled 时 CPU 滤镜跑了两遍

`systemRenderScaledFrame()`（`WinSystem.cpp:323`）和 `display->render()`
（`Direct3D11.cpp:2076`）各跑一次 `theApp.filterFunction`。录制开着 + captureScaled 时，
主线程的滤镜开销翻倍。

共享一份滤镜结果要给 `IDisplay` 加接口、改 7 个渲染器的调用点，属于"代价大就砍"。
先记着：**真要优化 captureScaled，优先级是 §9.4 的 GPU 放大（省 3~6 ms），
不是共享滤镜（省 1.5~3 ms）**。

---

## 11. 失败与回退

| 场景 | 处理 |
|---|---|
| 选中的编码器 `ActivateObject` 失败 | 回退到**同 codec** 的软件 MFT；再失败 → `systemMessage` 报错，**不弹保存对话框** |
| 枚举到 0 个 MF 编码器 | 列表只剩 `None (audio only)`，hint 说明原因 |
| 硬件 MFT 中途出错（驱动重置） | 停止录制 + 状态栏消息，不自动换编码器（文件已经开了，换编码器要换容器） |
| `MF_E_NOTACCEPTING` | 丢帧 + 计数，继续 |
| 磁盘写失败 / `Finalize` 失败 | 停止录制 + 报错；已经写出的部分留在盘上 |

回退必须发生在**保存对话框之前**，否则用户会看到一个建好却录不上的文件。

---

## 12. 本地化清单

改 `vba.rc` 之后必须同步四份 tsv，`python make_rc.py --check` 要过：

| 文件 | 新增/修改 |
|---|---|
| `lang/chs/menu.tsv` | `Start/Stop &Recording...` `&Pause/Resume Recording` `&Legacy AVI Recording` `Start/Stop &AVI Recording...` `Pause/Resume AVI &Recording` `Record at &Window Size and Filter` |
| `lang/chs/strings.tsv` | `Start &Recording...` `Stop &Recording` `Pause &Recording` `Resume &Recording` `Select recording file name` `MPEG-4 Video_*.mp4_All Files_*.*__` `MPEG-4 Audio_*.m4a_All Files_*.*__` `MP3 Audio_*.mp3_...` `FLAC Audio_*.flac_...` `None (audio only)` `Hardware` `Software` `High` `Medium` `Low` `None` `Detecting encoders...` 以及报错文案 |
| `lang/chs/dialog.tsv` | `Recording Settings` `&Video encoder:` `&Audio encoder:` `&Quality:` `Codec` `Encoder` `Type` `Start Recording` |

摘要行别用复杂模板。`%s` 只能整段替换 + 纯数字，参照 `cheat-explain.md` 的教训：
模板里出现 `%X` 吃指针的老坑。拆成静态文本 + 运行时拼接：

```cpp
CString summary;
summary.Format(_T("%s (%s) - %dx%d - %.2f fps - %d Mbps"),
               containerName, ext, w, h, fps, mbps);
// 纯音频时：
summary.Format(_T("%s (%s) - %s - %d kHz %s"),
               containerName, ext, codecName, khz, stereo ? _T("stereo") : _T("mono"));
```

容器名、扩展名、`Mbps`、`kHz`、`stereo` 都不翻译；`None`/`Hardware`/`Software` 走资源。

---

## 13. 分阶段

| 阶段 | 内容 | 人日 |
|---|---|---|
| 1 | `IVideoWriter` 抽象 + `AVIWrite` 适配 + `Open()` 收 `options` 参数 | 0.5 |
| 2 | `MFLoader` + `MFTEnumEx` 枚举 + 容器表 + 默认值 | 1~1.5 |
| 3 | `IDD_RECORD_SETTINGS` + `RecordSettings` 实现 + 四份 tsv | 1.5~2 |
| 4 | `MFVideoWriter` 视频路径（SinkWriter、格式协商、时间戳、`Finalize`） | 1.5~2 |
| 5 | `MFVideoWriter` 音频路径 + 纯音频模式（容器表扩展、时间戳从样本数算） | 1~1.5 |
| 6 | 并发骨架：三槽环形缓冲 + 编码线程（线程内建 SinkWriter、drain/`Finalize`/join）+ 丢帧计数 | 1.5~2 |
| 7 | 菜单重构：ID 改名、legacy 子菜单、`recordingBackend` 互斥、message map | 1 |
| 8 | 注册表接线 + 默认值回退 | 0.5 |
| 9 | 回归（7 后端 × 2 captureScaled × 3 soundQuality × 硬件/软件 × 纯音频 × 音画同步） | 1.5~2 |
| | **合计** | **10~13** |

第 1 阶段单独提交，零行为变化，方便回退。第 7 阶段（菜单）也可以提前单独提交，
因为它不影响录制本身。第 6 阶段建议在视频/音频两条路径都跑通之后再做——
先同步跑对，再换成入队，否则"编错了"和"并发错了"会混在一起，没法定位。

---

## 14. 待验证（没跑过，别当已验证）

| # | 事项 | 怎么验 |
|---|---|---|
| 1 | 32 位构建里能枚举到几个硬件 MFT | 最小程序 `MFTEnumEx` + 打印；NVIDIA/AMD/Intel 的 32 位 MFT 是否都装齐 |
| 2 | 首次 `MFTEnumEx` 的耗时 | `QueryPerformanceCounter` 包一层；>300 ms 就改后台线程 |
| 3 | 所有硬件 MFT 都设了 `MF_TRANSFORM_FLAGS_Attribute` 吗 | 打印每个 MFT 的 flags + vendor id，看是否一致 |
| 4 | MP4 sink 对 AV1 的支持 | 选 AV1 录 10 秒（本机枚举到了 NVIDIA AV1） |
| 6 | 纯音频 MP4 sink 出 `.m4a` 是否正常（只有音频轨） | 录 10 秒，看时长/波形 |
| 7 | 硬件编码器对非 16 对齐尺寸的处理（captureScaled 任意窗口尺寸） | 1441×961 录一段，看是否报错/带黑边。240×160、160×144 都满足 16 对齐，安全 |
| 8 | `soundQuality == 4`（11025 Hz）经 AAC | 录 10 秒看时长 |
| 9 | ARM64 上 `mfh264enc.dll` 是否存在 | `ls C:\Windows\System32\mfh264enc.dll` + 在 ARM64 上跑 |
| 10 | VP MFT 在本机能否 RGB32→NV12 | 喂 RGB32 出 MP4，肉眼验颜色（BT.601 vs BT.709） |
| 11 | 对话框版面 | `~/.workbuddy/scratch/dlgshot.c`（`LoadLibraryEx(..., LOAD_LIBRARY_AS_DATAFILE)` + `CreateDialogIndirectParamW` + `PrintWindow`） |
| 12 | 子菜单里的加速键文本 | `CAcceleratorManager::UpdateMenu` 会递归进子菜单（`AcceleratorManager.cpp:430-433`），所以理论上覆盖到了；开中文界面实际看一眼 Tools → Legacy AVI Recording 的文字即可 |
| 13 | 丢帧后音画是否还同步 | 用 6x captureScaled + 软编强制丢帧录 30 秒，播放器里看音画偏移；MP4 时间戳应连续跳格而不是复制帧 |
| 14 | 编码线程真的没抢 CPU | 录制时开 `showSpeed`，对比录制前后 `systemSpeed`；再用 Process Explorer 看两个线程的 CPU 占用 |
| 15 | `Finalize()` 的耗时 | 停止录制时计时；>200 ms 就要考虑异步关闭 + "正在保存"提示 |

---

## 15. 第一版实现状态（2026-10-25）

### 15.1 新增与改动的文件

| 文件 | 作用 |
|---|---|
| `src/win32/VideoWriter.h`（新） | `IVideoWriter` + `RecordParams`。只有 `<vfw.h>` 和 `Port.h`，任何 TU 都能包含 |
| `src/win32/MFLite.h/.cpp`（新） | `LoadLibrary` 加载 `mfplat.dll`/`mfreadwrite.dll`，8 个函数指针，引用计数的 `startup()/shutdown()`，作用域 `ComApartment`。**只有这个文件和下面两个 .cpp 把 `_WIN32_WINNT` 抬到 0x0602** |
| `src/win32/VideoEncoders.h/.cpp`（新） | `MFTEnumEx` 枚举 + 硬件判定 + 容器表 + 默认值 + GUID 字符串互转 |
| `src/win32/MFVideoWriter.h/.cpp`（新） | `IMFSinkWriter` 实现，pimpl 把 MF 头挡在头文件外 |
| `src/win32/Dialogs/RecordSettings.h/.cpp`（新） | `IDD_RECORD_SETTINGS` 对话框 |
| `src/win32/AVIWrite.h/.cpp` | 改为继承 `IVideoWriter`，逻辑一行没动 |
| `src/win32/VBA.h/.cpp` | `aviRecorder` 改类型；新增 `recordingBackend`、`recordParams`、四个注册表键 |
| `src/win32/MainWnd.h/.cpp` | 命令 ID 改名 + 四个 legacy handler |
| `src/win32/MainWndTools.cpp` | 新流程 + legacy 子菜单流程 |
| `src/win32/WinSystem.cpp` | 纯音频时跳过画面转换 |
| `src/win32/resource.h`、`vba.rc` | 菜单、对话框、25 条新字符串 |
| `src/win32/lang/chs/*.tsv` + `vba_chs.rc` | 31 条译文，`make_rc.py --check` 通过 |
| `win32/VBA_Latest.vcxproj` | 9 个新文件 |

### 15.2 和设计稿不一样的地方

| 设计稿 | 实际 | 原因 |
|---|---|---|
| 编码器条目带 CLSID | 只有 `subtype + hardware` | `IMFSinkWriter` 不接受指定 MFT |
| 音频含 FLAC | 第二轮补上（见 §16） | 起初 `_WIN32_WINNT` 是 0x0602，`MFAudioFormat_FLAC` 没声明 |
| `RecordParams` 带容器/扩展名 | 只带 codec 对 + 质量 | 容器是 `containerFor()` 的输出 |
| 并发（§10） | **没做** | 你要求先不做；接口暂时还是同步的 `AddFrame()` |

### 15.3 已跑过的验证

| 项 | 结果 |
|---|---|
| x64 全量编译 | 178 个单元，0 诊断 |
| 链接 | 178 个 obj → exe，`mfuuid.lib` 解析正常（只是 GUID 常量，不进导入表） |
| `vba.rc` / `vba_chs.rc` | `rc.exe` 无错 |
| 对话框版面 | `dlgshot` 渲染 `IDD_RECORD_SETTINGS` 正常（列表 + 两个下拉 + 两行说明 + 按钮） |
| 编码器枚举（本机 x64） | H.264 硬件 `NVIDIA H.264 Encoder MFT` / 软件 `H264 Encoder MFT`；HEVC 硬件 `NVIDIA HEVC Encoder MFT` / 软件 `HEVCVideoExtensionEncoder`；AV1 硬件 `NVIDIA AV1 Encoder MFT`；音频 `Microsoft AAC Audio Encoder MFT`、`MP3 Encoder ACM Wrapper MFT` |
| 录制：H.264 硬件 + AAC | `.mp4`，ffprobe：h264 240×160、59727/1000 fps、aac 44100 立体声、2.02 s |
| 录制：H.264 软件 + AAC | `.mp4`，同上（`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS = FALSE` 生效） |
| 录制：纯 AAC | `.m4a`，只有音频轨 |
| 录制：纯 MP3 | `.mp3`，`format_name=mp3`、44100 立体声 |
| 画面方向 | 抽第 60 帧看，自下而上输入被正确翻成自顶向下（合成图案的绿色梯度在上方） |

### 15.4 还没验的

| 项 | 说明 |
|---|---|
| 在真模拟器里录一段 | 需要 ROM；目前只验到"writer 本身正确" |
| 音画同步 | 探针的音视频时间戳来自同一条循环，真机里音频来自 `systemSoundNext()` 的节奏，要单独看 |
| 32 位构建的硬件 MFT | 本机验的是 x64；Win32 构建能枚举到几个硬件编码器未测 |
| ARM64 | 未测 |
| 驱动不设 `MF_TRANSFORM_FLAGS_Attribute` 的 MFT | 走 vendor id 兜底，未遇到实例 |
| `Finalize()` 耗时 | 120 帧几毫秒；长时间录制未测 |

---

## 16. 第二轮调整（2026-10-25）

### 16.1 编译器目标改成 Windows 10

三个 MF 编译单元的 `_WIN32_WINNT`/`WINVER` 从 0x0602 提到 0x0A00。
**这只影响 SDK 头文件里哪些原型可见，不影响 exe 的导入表**——所有入口点仍然是
`GetProcAddress` 拿的，而且没有一个晚于 Win7。收益是 `MFAudioFormat_FLAC` /
`_ALAC` / `_Opus` 这些被 SDK 用 WINVER 挡住的常量直接可用（MinGW 的 `mfapi.h`
里它们是无条件定义的，可以对照）。

### 16.2 音频编码器：靠枚举，不靠开关

候选表从 AAC/MP3 扩到 **AAC / MP3 / FLAC / ALAC / Opus / AC-3**。
每个 codec 只有在**本机真的装了对应编码器 MFT** 时才出现在下拉里，所以
"操作系统是否支持"不需要写任何版本判断。

本机（x64）实测的 10 个音频编码器里能用的：

| codec | 编码器 | 容器 | 结果 |
|---|---|---|---|
| AAC | Microsoft AAC Audio Encoder MFT | `.m4a` | ✓ |
| MP3 | MP3 Encoder ACM Wrapper MFT | `.mp3` | ✓ |
| FLAC | Microsoft FLAC Audio Encoder MFT | `.flac` | ✓ |
| ALAC | Microsoft ALAC Audio Encoder MFT | `.m4a` | ✓ |
| AC-3 | Microsoft Dolby Digital Encoder MFT | `.m4a` | ✓ |
| Opus | **没有** | `.m4a` | 不出现（Windows 只带 Opus 解码器） |

FLAC / ALAC 第一次试是失败的：无损编码器要在输出类型上看到
`MF_MT_AUDIO_BITS_PER_SAMPLE`，MP3/Opus/AC-3 则相反——带上它会被拒。
现在按 codec 分开设置。

### 16.3 码率代替质量档

`RecordParams.quality` 换成 `videoBitrate` / `audioBitrate`（kbps）。
对话框里是两个 `EDITTEXT + msctls_updown32`（`UDS_AUTOBUDDY | UDS_SETBUDDYINT |
UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS`），范围 100..100000 / 32..512。

每行右边一个预设下拉（`低 / 中 / 高 / 自定义`）：低=2000/64、中=4000/128、高=8000/192。
选中就写进码率框；手改数字时下拉自动回到"自定义"（或匹配到的档位）。
无损编码器（FLAC/ALAC）的音频码率框和预设一起禁用——码率目标对它没有意义。

实测码率确实生效：同一段 120 帧，1000 kbps 出 305 KB，8000 kbps 出 545 KB。

### 16.4 列表

- `LVS_EX_FULLROWSELECT`：整行选中，点任意一列都行。
- 列宽按客户区算：编码固定 46 DLU、类型固定 88 DLU、编码器吃掉剩下的，
  最后再按 `uiFont::layoutDpi()` 缩放。算宽度要在 `OnInitDialog` 之后
  （`PostMessage`）做，因为字体钩子在那之后才把控件尺寸定下来。

### 16.5 菜单文案

`Start/Stop Recording...` 和 Movie 的"录制"撞名，改成
`Start/Stop Audio/Video File Recording...`（中文"开始/停止音视频文件录制"），
暂停项同步改成 `Pause/Resume File Recording`。

### 16.6 视频侧的配置项

用户问过"还能配什么、能不能选 RGB/YUV"。结论：

- **RGB/YUV 不该给用户选。** sink writer 的输入可以是 RGB32 / NV12 / YUY2，
  但编码器只吃 NV12；给 RGB32 时 MF 自己插 Video Processor 转，给 NV12 就得我们自己转。
  中间格式只决定"谁来转"，最终码流格式由编码器决定，暴露给用户是纯噪音。

**加上了两个**（§17）：H.264 profile、录制帧率。

**关键帧间隔砍掉**：`MF_MT_MAX_KEYFRAME_SPACING` 写在输出类型上，微软软编和 NVENC
都当没看见（120 帧、间隔 1 秒，仍然只有 1 个关键帧）。编码器真正的开关是
`ICodecAPI` 的 `CODECAPI_AVEncMPVGOPSize`，而 `IMFSinkWriter` 不给：
`GetServiceForStream(MF_SINK_WRITER_ALL_STREAMS, GUID_NULL, IID_ICodecAPI)` 返回
`0xC00D36B3`（`MF_E_UNSUPPORTED_SERVICE`），`IMFSinkWriter` 也没有
`SetOutputMediaType`（输出类型在 `AddStream` 时就定死）。要控制 GOP 只能绕开
sink writer 自己搭 MFT 链，那是另一个量级的改动。

**CBR/VBR 码率控制模式同样做不到**，理由一样：`ICodecAPI` 拿不到。

---

## 17. 第三轮：profile 与帧率（2026-10-25）

### 17.1 Profile

下拉 `Auto / Baseline / Main / High`，写 `MF_MT_MPEG2_PROFILE`（值 0/66/77/100，
和 `eAVEncH264VProfile_*` 一致）。

**只有 H.264 真的能选。** HEVC 和 AV1 的 profile 值域是另一套（Main = 1），
但两边都只有一个 8-bit 4:2:0 的 profile，选了和 Auto 没区别；更糟的是
HEVC Main 10（值 2）在这台机器上直接让 sink writer 失败（0 字节文件），
因为送进编码器的是 8-bit NV12。所以这两个 codec 的下拉只有 `Auto` 并且是灰的。

实测：`Auto`/`Baseline` → `Constrained Baseline`，`High` → `High`；
HEVC `Auto`/`Main` → `Main`；AV1 `Auto`/`Main` → `Main`；HEVC Main 10 → `open failed`。

### 17.2 录制帧率

下拉 `Auto / 60 / 30 / 20 / 15`。低于模拟器原生帧率时**丢帧，不复制帧**：
`videoEncoders::videoFrameStep()` 算出每 N 帧写一帧，然后把
`fpsDenominator *= N`——时间基跟着变，所以时间戳和时长都还是对的。

实测（120 个模拟帧，原生 59.7275 fps）：

| 目标 | 步长 | 写出的帧 | ffprobe r_frame_rate | 时长 |
|---|---|---|---|---|
| Auto | 1 | 120 | 59727/1000 | 2.020113 s |
| 30 | 2 | 60 | 179/6（29.83） | 2.020113 s |

时长一帧不差，说明丢帧没有把时间轴压短。音频时间戳本来就按采样数算，不受影响。

### 17.3 对话框最终形态

```
Video                                     ← 粗体标题，LTEXT 高 11 DLU
  Encoder:      [列表 226 DLU]
  Bitrate(kbps):[8000] [高▾]               ← 编辑框和预设下拉等宽，各 70
  Profile:      [Auto ▾]                   ← 70
  Frame rate:   [Auto ▾]                   ← 70
  摘要：MP4 (.mp4) - 1440x960 - 59.73 fps - 8000 kbps
  提示：在 GPU 上编码：CPU 占用低，帧率不受影响。
  ──────────────────────────────           ← SS_ETCHEDHORZ
Audio                                     ← 粗体标题，LTEXT 高 11 DLU
  Encoder:      [下拉 145 DLU]
  Bitrate(kbps):[192]  [高▾]
  Sample rate:  44100 Hz                   ← 只读，来自 soundQuality
                              [开始录制] [取消]
```
`IDD_RECORD_SETTINGS` **240×310 DLU**。标签列 78，右边界 233，所有右侧控件对齐。

两个坑：

1. **标题的 LTEXT 高度必须给 11 DLU，不能照抄别的标签的 8。** 8 DLU 在 96 dpi 下只有
   13 px，而 `uiFont` 换上的 9 pt 中文字体（雅黑粗体）有 16~17 px，静态控件垂直居中
   绘制，上下各切掉一截——中文界面下"视频"的顶就没了。11 DLU ≈ 18 px 才装得下。
2. 粗体标题用 `uiFont::message()` 派生一个 `FW_BOLD` 的 LOGFONT——**必须在字体钩子
   之后**（`PostMessage` 的 relayout 里）设，否则会被 `applyToDialog` 覆盖掉。

摘要和提示两行放在**视频区块的最下面**（帧率之下、分隔线之上），因为它们是整次录制的
摘要；音频区块只留自己的三项。

### 17.4 采样率（只读）

音频区块多一行 `Sample rate: 44100 Hz`，值来自模拟器：`44100 / soundQuality`，
和 `recordingSoundFormat()`（`MainWndTools.cpp`）以及声音后端
（`WinSystem.cpp:819`）用的是同一个算式。音频选 `None` 时这一行跟着灰掉。

它不可编辑是设计：采样率由 `Options -> Sound -> Sound quality` 决定，
在录制设置里改它只会让两处不一致。

---

## 18. 异步编码（2026-10-25 实现）

### 18.1 为什么 GPU 编码还需要一个线程

**"在 GPU 上执行"省的是计算，不省等待。**

`IMFSinkWriter::WriteSample` 是同步调用，它会阻塞到编码器接受这个 sample 为止；
硬件编码器的输入 surface 池是固定的（8~16 个），池满就得等编码器编完一个。
所以：

- 编码 2 ms/帧时，调用线程每帧等 2 ms；
- 场景切换、IDR 帧要 10 ms 时，调用线程等 10 ms；
- 这 10 ms 不是"GPU 在忙"，是**调用线程挂在驱动的一次同步上**。

提交本身也是 CPU 活：BGR24→RGB32 转换、`MFCreateMemoryBuffer`、`memcpy`、
驱动提交。GPU 只负责算，不负责递。

对 TAS 来说要命的不是平均帧率而是**抖动**：模拟线程每帧必须稳定在 16.7 ms，
偶发 8 ms 等待就会让 `systemSpeed` 掉、声音缓冲漂移。

### 18.2 实现

`RecordParams.async`（默认开，对话框里的 "Encode in a background thread"）。

- **SinkWriter 在编码线程创建，不在调用线程。** 这个线程用
  `CoInitializeEx(COINIT_MULTITHREADED)`（MTA，不需要消息泵），之后所有
  MF 调用都发生在它身上。`Open()` 因此是"异步启动但同步等结果"：
  起线程 → 线程建 writer → `SetEvent(ready)` → 调用线程 `WaitForSingleObject`（15 s）。
- **三槽环形缓冲**：调用线程 `AddFrame` 找空槽 → 转换进去 → 记时间戳 → 入队 →
  `SetEvent(wake)`。三个槽都占着就**丢这一帧**，但时间戳照常前进，所以时长不变。
- **音频不丢**：队列 512 块（≈8 秒），满了调用线程 `Sleep(1)` 等，而不是丢块——
  丢一块会让音轨变短、音画错位。
- 时间戳由调用线程算（`submittedFrames` / `audioSamples`），编码线程只负责写。
- 关闭：`stopping = true` → `SetEvent` → 线程排空队列 → `Finalize()` → join。
  `Finalize` 在线程里做，所以停止录制时主线程最多等一个 `Finalize`。
- **跨线程的两个裸标志用 `Interlocked`，不进临界区**：`initOk`（writer 起来了）
  和 `failed`（MF 调用失败）。用 `EnterCriticalSection` 包一个 bool 是两次系统调用
  换一个字节，没必要；`ready` 事件本身已经提供了两个线程之间的顺序保证。
  留在临界区里的只有 `stopping`，因为它必须和两个队列的状态**一起**判断。

### 18.3 实测（120 帧，60 fps 节奏，同一份合成画面）

| 场景 | 同步 | 异步 | 省下 |
|---|---|---|---|
| 240×160，硬件 | 0.10 ms/帧 | 0.08 ms/帧 | 0.02 ms |
| 1440×960，硬件 | 3.60 ms/帧 | 2.50 ms/帧 | **1.1 ms** |
| 1440×960，软件 | 5.17 ms/帧 | 4.02 ms/帧 | **1.15 ms** |

两个结论：

1. **默认分辨率下几乎没区别**（0.02 ms）。240×160 的编码本来就只要 1~2 ms，
   sink writer 的内部队列完全吸收得下，`WriteSample` 根本不阻塞。
2. **captureScaled 到 1440×960 才见真章**，省 1.1 ms/帧；剩下的 2.5 ms 是
   CPU 上的 BGR24→RGB32 转换，那部分在调用线程上，异步挪不走。

三个槽在 60 fps 下绰绰有余（编码 2 ms vs 帧间隔 16.7 ms）。不加节流的话探针
能跑出 7000 fps，那时队列必然溢出——这是设计使然，不是 bug。

---

## 19. 色彩：全范围 + 明写色彩信息（2026-10-25）

### 19.1 症状

用 PotPlayer 的 DX11 渲染器播放新版录制出来的 `.mp4`：AV1 文件画面过饱和，
HEVC 文件干脆不出画面；换播放器就正常，旧版 AVI 录制的文件在 PotPlayer 里也正常。

### 19.2 文件本身的问题

`ffprobe` 对修复前的两个文件都报：

```
color_range=tv   color_space=unknown   color_transfer=unknown   color_primaries=unknown
```

AV1 的 `sequence_header` 里 `color_description_present_flag = 0`（连
`color_range` 都是默认值 0），HEVC 的 VUI 只有 timing info，MP4 里也没有 `colr` box。
也就是说**码流里完全没有色彩信息**，每个播放器只能自己猜；猜法不同，画面就不同。

### 19.3 实际写进去的是什么

用 8 条纯色竖条（白/黑/红/绿/蓝/青/品红/黄）走一遍真实的 `MFVideoWriter`，
再把解出来的 YUV 拿四种「矩阵 × 范围」组合做正向拟合：

| 组合 | 平均误差（码值） |
|---|---|
| BT.601 + limited | 8.57 |
| **BT.601 + full** | **0.30** |
| BT.709 + limited | 10.46 |
| BT.709 + full | 9.41 |

修复前（不设任何属性）：601 + limited，误差 0.16。
也就是说 MF 的转换器对 RGB32 输入**固定用 BT.601**，
`MF_MT_YUV_MATRIX` 写在输入类型或输出类型上都被忽略（两种写法都试过）。

模拟器给的帧本来就是全范围 RGB：`WinSystem.cpp` 里每个通道是
`(v & 0x1f) << 3`（GBA 的 15 位色），取值 0…248、步长 8，没有任何 16…235 的压缩。
先压到 limited 再让播放器展开，纯属白丢码值。

### 19.4 改法

`MFVideoWriter::Impl::createVideoStream()` 给输入类型和输出类型都盖上四个属性：

| 属性 | 值 |
|---|---|
| `MF_MT_VIDEO_NOMINAL_RANGE` | `MFNominalRange_0_255`（全范围） |
| `MF_MT_YUV_MATRIX` | `MFVideoTransferMatrix_BT601`（转换器实际用的那个） |
| `MF_MT_VIDEO_PRIMARIES` | `MFVideoPrimaries_BT709` |
| `MF_MT_TRANSFER_FUNCTION` | `MFVideoTransFunc_709` |

矩阵写 601 不是偏好，是**照实说**：转换器就是 601，写 709 会让相信标签的播放器
用错矩阵，比不写还糟。primaries/transfer 用 709 是对的——源是 sRGB 式的 RGB 缓冲，
只有 YCbCr 矩阵是 601 那一套，这是这类内容的常规组合。

`AddStream` / `SetInputMediaType` 若被编码器拒绝，就 `clearColorAttributes()` 去掉
四个属性重试一次（退回到修复前的行为），**不因为色彩标签而录不上**。

### 19.5 验证（x64，本机）

`colorprobe`（`~/.workbuddy/scratch/colorprobe.cpp`，直接链 `MFVideoWriter.obj`）
录 60 帧 8 色竖条，四条路径 —— H.264 硬件 / H.264 软件 / HEVC / AV1 —— 现在都是：

```
color_range=pc  color_space=bt470bg  color_primaries=bt709  color_transfer=bt709
```

（`MFVideoTransferMatrix_BT601` 落到 H.264 的 `matrix_coefficients = 5`，
即 `bt470bg`；和 `smpte170m` 的系数完全相同，只是标签名不同。）

- MP4 里出现了 `colr` box：`nclx 0001 0001 0005 80`（primaries=1、transfer=1、
  matrix=5、full_range_flag=1）。
- 解出来的 Y 平面：白 = 255、黑 = 0（修复前是 235 / 16）。
- 用 ffmpeg（会读标签）解码回 RGB，八条竖条与输入逐个通道一致（误差 ≤2 码值，
  来自有损编码，不是色彩变换）。

### 19.6 没做的

- **不做 RGB 码流**：H.264/HEVC/AV1 的 MF 编码器只吃 NV12/YUV420，
  没有能出 RGB 4:4:4 的 MFT，给用户一个"RGB"选项只会是假的。
- **不给 limited range 选项**：源是全范围 RGB，limited 只会丢码值；
  要兼容老播放器而加开关，收益为负。
- **不自己做 RGB→YUV**：那样才能用 BT.709 系数，但要把 2400×1600 的转换
  从 GPU 搬到调用线程（3~5 ms/帧），代价远大于收益。
- **chroma siting 没动**：AV1 仍报 `unspecified`（AV1 的默认值），H.264/HEVC 是
  `left`。要改得给输出类型加 `MF_MT_VIDEO_CHROMA_SITING`，属于另一个话题。
