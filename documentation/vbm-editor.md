# VBM Editor（帧时间轴编辑器）

## 它是什么

`Tools > Movie > VBM Editor...` 打开一个窗口，把当前 VBM 录像的按键序列画成一张表：

- 每一行是一帧，行号就是帧号（从 0 起）；
- 左边三列固定：播放箭头（`>`，跟着播放位置移动）、六位帧号、`HH:mm:ss.SS` 时间戳；
- 其余每一列是一个手柄键，字符与 OSD 的按键显示完全一致（`< ^ > v A B L R S s`）；
- 时间轴上出现过 reset / motion 位时才多出对应的列，那几列只读。

时间戳用的是「播放录像」弹窗里「录像长度」那套算法（`MovieOpen.cpp`）：
`seconds = frames / systemGetFrameRate()`，而那个帧率是 GBA 的时钟比
`262144 / 4389 = 59.7275...`，**既不是 60 也不是 59.94**；小数部分截断到百分位，
格式串也照抄那边（`%02u:%02u:%02u.%02u`）。所以它和对话框里显示的录像长度永远一致。

顶栏是播放控制：播放 / 暂停 / 单帧前进 / 跳到正在播放的帧（都有 tooltip），
右侧「跳转到帧」的数字框显示列表**选中**的帧号，输入数字回车即跳转并选中。

编辑只做一件事：**改写 VBM 里记录的按键序列**（相当于"用户在某些帧按下了某些键"）。
它不改模拟器怎么跑游戏——不重置、不读档、不 seek、不切状态机、不动只读标志。
播放控制也不在这里实现，而是把 `File > Pause` 和 `File > Frame Advance` 这两条既有命令
转发给主窗口（见下面"传输按钮"）。

## 数据模型

`SMovie`（`src/common/movie.h`）里已经有一份权威的内存副本：

| 字段 | 含义 |
|---|---|
| `inputBuffer` | 整条序列，`bytesPerFrame * length_frames` 字节 |
| `inputBufferPtr` | 指向 `currentFrame` 那一帧，必须恒等于 `inputBuffer + bytesPerFrame * min(currentFrame, length_frames)` |
| `bytesPerFrame` | `2 * 控制器数`；本前端只会录 1 个控制器，所以是 2 |
| `currentFrame` | 时间轴位置（已执行/正在执行的帧号） |
| `length_frames` | 行数 |
| `readOnly` | 0 可编辑 / 1 只读模式 / 2 文件本身不可写（`rb+` 打不开） |

每帧 16 位小端：

```
bit0 A  bit1 B  bit2 Select  bit3 Start
bit4 Right  bit5 Left  bit6 Up  bit7 Down
bit8 R  bit9 L
bit10 旧 reset   bit11 新 reset      (BUTTON_MASK_OLD_RESET / NEW_RESET)
bit12..15 motion                     (本前端从不下发，见 win32/WinSystem.cpp 的 systemGetJoypad)
```

`BUTTON_REGULAR_MASK`（0x03FF）就是 10 个手柄键；编辑器只碰这些位。

**头文件里的长度是 `length_frames - 1`**（`movie.cpp` 的 `read_movie_header/write_movie_header`，
为兼容老工具），读写两侧各做一次 ±1，别直接拿头里的值当行数。

## core API

`src/common/movie.h` 新增（实现紧跟在 `VBAMovieDeleteFrames` 之后）：

```cpp
uint32 VBAMovieGetInputGeneration();                 // 序列版本号，UI 用它判断要不要整体刷新
int    VBAMovieGetReadOnlyMode();                    // 0/1/2，见上表
uint16 VBAMovieGetInputAt(uint32 row, int ctrl = 0);
uint16 VBAMovieGetUsedInputMask(int ctrl = 0);       // 整条序列按位或，决定要不要多出 reset/motion 列
int    VBAMovieSetInputAt(uint32 row, int ctrl, uint16 value);
int    VBAMovieInsertFrameAt(uint32 row);
int    VBAMovieDeleteFrameAt(uint32 row);
#define MOVIE_MAX_FRAMES (0x0FFFFFFFu)
```

### 只允许改什么

| 允许 | 细节 |
|---|---|
| `inputBuffer` 的按键字节 | 只写 `BUTTON_REGULAR_MASK` 覆盖的位，reset/motion 原样保留（`SetInputAt` 里做掩码，调用方传错也写不进去） |
| `header.length_frames` | 插/删时 ±1 |
| 文件里的对应字节 | 单元格只写 2 字节（`fseek` + `fwrite` + 还原位置）；插/删整块回写 |
| `header.minorVersion` | 只能通过 `VBAMovieConvertCurrent(false)` 从 1.0 规范到 1.1，与现有 Insert/Delete 菜单一致 |
| `inputGeneration` | 每次改动 +1 |

### 绝对不许碰

`currentFrame`（时间轴位置，动了存档校验/帧计数/truncate 全错）、`state`、`editMode`、
`readOnly`、`pauseFrame`、`rerecord_count`，以及模拟器本身。
也不调用 `fix_movie_old_reset()`：它写的是"当前帧的 reset 状态"，需要 `resetSignaledLast`，
对任意行没有这个信息。

### 两个必须记住的坑

1. **插入/删除后要重算 `inputBufferPtr`**。`reserve_movie_buffer_space()` 靠
   `inputBufferPtr - inputBuffer` 保存偏移，指针过期会让后续 `VBAMovieRead` 读到乱数据。
2. **删除必须 ftruncate**。`flush_movie_frames()` 只写 `length_frames` 那么多，文件尾会留下旧帧，
   而 `VBAMovieOpen()` 是**按文件大小**重算长度的——不截断的话被删的帧会复活。
   所以删除走 `flush_movie_frames()` + `truncate_movie(newLength)`。

### 插入/删除对播放位置的影响

`currentFrame` 不变（它表示"已经跑了多少帧"，不是"指向哪一帧的数据"）。所以在播放位置**处或之前**
插/删，光标那一行的内容会跟着位移，下一帧读到的输入立刻变了——这正是需求里说的"可能不同步"，
界面只提示不阻止。单元格编辑则只改某一帧：已经跑过的帧产生不同步但没有即时影响，未跑的帧没有副作用。

## 界面

`src/win32/Dialogs/VbmEditor.{h,cpp}`，`IDD_VBM_EDITOR`（`vba.rc`），模型对话框 + 单例。

### 虚拟列表

`LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER`
+ `LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER`。

行内容全部由 `LVN_GETDISPINFO` 现算：列 0 是 `>`（等于播放位置时），列 1 是 `%06u` 帧号，
列 2 是 `HH:mm:ss.SS` 时间戳，其余列是"该位为 1 就返回那个字符，否则空串"。
每格 O(1)，直接按行号读 `inputBuffer`；时间戳是一次浮点除法 + `modf`，同样与帧数无关。

行底色用 item 级 `NM_CUSTOMDRAW`：当前行淡蓝、标记行淡黄（`CDRF_NEWFONT` 才会用 `clrTextBk`）。
**不做逐格自绘**：文本既然已经由 `LVN_GETDISPINFO` 给出，就不需要 `CDDS_SUBITEM`。

### 刷新状态机

`WM_TIMER` 50 ms（不是每帧），读一份快照 `(active, readOnly, state, length, frame, generation)`，
与上次比较后只做必要的事：

| 变化 | 动作 |
|---|---|
| 换了录像（文件名 + uid） | 清标记、重建列、`SetItemCountEx`、滚动回顶部、改标题 |
| `generation` 变 | 重算 `usedMask`；**只有列集合真的变了才重建列**（否则会把用户拖过的列宽重置） |
| 长度 +1 | `SetItemCountEx(LVSICF_NOINVALIDATEALL)` + 只重绘新增那一行（录制时走这条路） |
| 长度其它变化 | `SetItemCountEx` + 整表 invalidate（插/删/读档，都是用户动作） |
| 播放位置变 | 重绘旧行和新行；`m_follow` 打开时按**整页**滚动（见下） |

所以每 tick 的工作量与帧数无关，几万帧和 20 帧一样。

### 跟随播放位置：整页滚动

`EnsureVisible` 一次只挪一行，播放时列表会一行一行地爬。`followPlayhead()` 改成：

- 播放位置还在可见范围内 → 什么都不做；
- 出界 → 按 `LVM_SCROLL` 滚动 `(target - top) / 每页行数` 页（至少一页），
  再 `EnsureVisible` 兜一次底（最后一页不满时）。因此光标永远落在页首附近。

`revealRow(row, center)` 是跳转用的：选中 + 滚到列表**中间**（先 `EnsureVisible`，
再按行高差 `LVM_SCROLL`），所以从第 100 帧跳到第 90000 帧不会只把它贴在底边。

### 列宽：按字体量，不写死

旧版把列宽写成固定像素（标记 18 / 帧号 58 / 按键 22），在 200% DPI 上对话框放大了
而列宽没放大，列表里只有三分之一宽度有内容。现在 `fitColumns()`：

1. 取列表自己的字体（`UiFont.cpp` 在 `WM_INITDIALOG` 里把它换成了系统 UI 字体），
   量出数字宽度 `digit`、两个表头宽度、`00:00:00.00` 的宽度、平均字符宽 `pad`；
2. 箭头列 = `2 * digit`；帧号列 = `7 * digit + 2 * pad`（不足表头宽时取表头宽）——
   帧号是扫一眼就要读的东西，比六个数字再多留一位和两侧留白；时间戳列 = 样本宽度 + `pad`；
3. 其余按键列平分剩下的客户区宽度，最后一列吃掉取整余数，右边缘贴合。
   所以帧号列或时间戳列变宽时按键列自动变窄，不需要另调。

不写死像素也不依赖 dialog unit：对话框创建后字体被换过，`MapDialogRect` 与控件
实际位置已经不再一致。窗口不可改变大小，但 `WM_DPICHANGED`（跨不同 DPI 的显示器）
会重新量一次——`UiFont.cpp` 的子类过程先把控件按比例缩放，我们的处理函数随后运行。

### 关闭：必须自己 DestroyWindow

`CDialog::OnCancel()` 走 `EndDialog()`，对用 `Create()` 建出来的**非模态**对话框
它只会把窗口藏起来：`WM_DESTROY` 不来，`PostNcDestroy()` 不跑，单例指针 `g_editor`
仍然非空，于是下次 `open()` 只是把一个看不见的窗口提到前面 —— 表现出来就是
"第一次能打开，关掉后再也打不开"（2026-10-25 的实际 bug）。所以 `OnCancel()`
被改写成 `DestroyWindow()`；`open()` 另外检查 `GetSafeHwnd()`，指针过期就重建。

### 为什么不会卡 GUI

- 只有可见的 ~40 行会向对话框要文本；
- 不建任何 O(n) 索引、不复制序列、不用 `InsertItem` 循环（非虚拟列表插 5 万行要几百毫秒，
  而且每次插入都要重建）；
- 不用 `LVSCW_AUTOSIZE`（虚拟列表上它会遍历**全部**行算宽度，这是最典型的"几万帧卡死"写法）；
- 排序关掉（时间轴不能排序）；重建列时 `SetRedraw(FALSE)`，一次成型。

### 传输按钮与帧号框

四个 `BS_OWNERDRAW` 按钮（播放 / 暂停 / 单帧前进 / 跳到播放位置），图标是
`OnDrawItem()` 用 GDI 现画的三角形和方块——不占位图资源，随按钮尺寸缩放，
`OpenThemeData("BUTTON")` 拿到的主题句柄让它们看起来和旁边的普通按钮一致
（没有主题时退回 `DrawFrameControl`）。按钮没有文字，所以每个都挂了 tooltip。

- 播放 / 暂停 = `ID_FILE_PAUSE`（它是 toggle，所以只在语义对得上时才发）；
- 单帧前进 = `ID_DEBUG_NEXTFRAME`（`systemSetPause(false)` + `winPauseNextFrame`）；
- 两者都先置 `theApp.active = true`：本对话框有焦点时主窗口已把自己标成 inactive
  （`pauseWhenInactive`），否则 idle 循环一帧都不会跑。`inputActive` 不动，
  键盘仍然不会漏进游戏。
- 帧号框（`ES_NUMBER`）跟着**选中行**走：选中变了就覆盖它；编辑时回车（`PreTranslateMessage`
  拦 `VK_RETURN`，`OnOK` 也改写成跳转，否则模态对话框的基类会销毁窗口）→ 选中并居中。
  超出范围按最后一帧处理。

### 只读复选框

它是**这个窗口的编辑锁**，默认勾选，**不碰 `Movie.readOnly`**（那个标志会改变读档语义：
只读时读档只做一致性校验，可编辑时读档会用存档里的序列覆盖整条录像——那是模拟器行为）。

- 录像本身只读（`readOnly` 1 或 2）时：复选框强制勾选 + 禁用，状态栏说明怎么办（1 用菜单解除，2 是文件不可写）；
- 录像可编辑时：复选框可用，勾上=只看不改。

### 标记

对话框内的 `std::set<uint32>`，只存帧号，关闭即消失。

- 右键行首菜单：标记/取消标记、插入一行、删除一行、上一个/下一个标记、清除全部标记；
- 顶栏两个按钮做同样的跳转；`M` 标记、`Insert`/`Delete` 插删、`F2` / `Shift+F2` 跳标记；
- 插一行：`>= row` 的标记 +1；删一行：等于该行的标记消失、`> row` 的 −1；
- "定位"只是列表选中 + 滚动，**不改变模拟器状态**（VBA 没有随机 seek 能力：
  没有逐帧存档，rewind 每槽只有 400 KB，长录像的 `emuWriteMemState` 直接失败）。

## 已知限制（有意为之）

- 只显示 controller 0；别的控制器字节原样保留不动（本前端只录 1 个控制器）。
- 本窗口拿到焦点时模拟器会暂停（`pauseWhenInactive` 的既有行为，是有意保留的）；
  点"播放"继续，但那只是把 `theApp.active` 顶住，再切一次窗口仍按原规则重新计算。
- 帧号框跳转只是选中 + 滚动，不把模拟器带到那一帧（VBA 没有 seek）。
- 录制中不能插入/删除（下一帧的 `length_frames = currentFrame` 会把它抹掉），但可以改已经落盘的行。
- END 状态下不能插删（与现有菜单一致），单元格仍可改。
- 不增加 `rerecord_count`：手工编辑不是重录。
- 没有撤销、没有框选批量改、没有列宽持久化、不做重播式 seek。
- 与 `editMode` 无关：编辑器直接改 buffer，不走 DISCARD/OVERWRITE/INSERT/XOR 那套录制逻辑。
  编辑完再 Resume Recording 时，OVERWRITE/XOR 模式仍会按它自己的规则写下一帧。

## 验证

探针脚本都放在仓库外的临时目录里，不入库，下面只写文件名。

- `vbm_probe.cpp` + 真实的 `movie.obj`：造一个 20 帧的 VBM，逐项检查
  读取、单元格改写（含 reset 位不被写坏）、插入、删除（含文件截断与头里的长度）、
  追加、录制中改已落盘行（文件追加位置不动）、各种拒绝、Stop 后重新打开内容仍在。
  当前 43 项全过。
- **`vmbshot.cpp`**：真的把 `VBA_check.exe` 跑起来，
  `WM_COMMAND(ID_TOOLS_VBMEDITOR)` 打开窗口，`PrintWindow` + 屏幕 `BitBlt` 各存一张 BMP，
  然后 `WM_CLOSE` → 再打开一次，验证"关掉后还能再开"。最后 `TerminateProcess`，
  不触发退出时的设置写回（`reg.exe export` 被沙箱拉黑，没法备份注册表）。
  注意它自己那两个坑：主窗口句柄要在找对话框之前单独存下来（第二次枚举会把 `main` 清掉），
  以及 `IsWindowVisible` 过滤会漏掉"藏起来的窗口"——那个 bug 的第一现场正是这样找到的。
- `dlgshot.exe` 把 `IDD_VBM_EDITOR` 从资源里取出来渲染成 BMP 核对版面。
- 全量 `build_check.py`（179 个 TU）+ `link_check.py` 编译链接通过；
  单文件改动用 `one_obj.py` 重编一个 `.obj` 再链接，省掉两分钟全量。
- 资源：`rc.exe` 编 `vba.rc` 与 `vba_chs.rc` 均无错；`make_rc.py --check` 通过。
