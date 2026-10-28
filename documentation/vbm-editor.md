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

`src/win32/Dialogs/VbmEditor.{h,cpp}`，`IDD_VBM_EDITOR`（`vba.rc`），非模态对话框 + 单例。

### 虚拟列表

`LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER`
+ `LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER`。
**没有 `LVS_SINGLESEL`**：Ctrl/Shift 可以选多行，但那只是行命令（删除、剪切复制、标记）的目标集合，
点单元格仍然只改一帧的一个键。

行内容全部由 `LVN_GETDISPINFO` 现算：列 0 是 `>`（等于播放位置时），列 1 是 `%06u` 帧号，
列 2 是 `HH:mm:ss.SS` 时间戳，其余列是"该位为 1 就返回那个字符，否则空串"。
每格 O(1)，直接按行号读 `inputBuffer`；时间戳是一次浮点除法 + `modf`，同样与帧数无关。

行底色用 item 级 `NM_CUSTOMDRAW`：当前行淡蓝、标记行淡黄（`CDRF_NEWFONT` 才会用 `clrTextBk`）。
**不做逐格自绘**：文本既然已经由 `LVN_GETDISPINFO` 给出，就不需要 `CDDS_SUBITEM`。

### 选择：焦点行是缓存，不是每 tick 去问

帧号框和键盘动作都对着**焦点行**（`LVIS_FOCUSED`）。这个值存在 `m_focusRow` 里，
**不**在 50 ms 定时器里调 `GetNextItem(LVNI_FOCUSED)`——那会走一遍列表，几万帧就是每 50 ms 一次全表扫描。

写入点只有三处：`LVN_ITEMCHANGED`（只认"拿到焦点"那一条，丢掉焦点那条不管，否则焦点从 A 挪到 B
时会先把框清空一下）、`selectOnlyRow()`（跳转/标记/帧号框，顺带清掉多选）、右键菜单移动焦点。
`refresh()` 只做一次 O(1) 的越界钳制：行数缩到焦点以下就把焦点丢掉。

- 点单元格：`Ctrl`/`Shift` 按下时不动（那是选选手势），**选中的行数不等于 1 时也不动**；
- 右键：点在选区内 → 保留多选、只挪焦点；点在选区外 → 折叠成那一行；
- `Delete` 删掉全部选中行；`Insert` 只在恰好选中一行时插到该行之前（否则状态栏说"请只选中一帧"）；
- `M`/`F2` 用焦点行，不用"第一个选中行"。

### 刷新状态机

`WM_TIMER` 50 ms（不是每帧），读一份快照 `(active, readOnly, state, length, frame, generation)`，
与上次比较后只做必要的事：

| 变化 | 动作 |
|---|---|
| 换了录像（文件名 + uid） | 清标记、重建列、`SetItemCountEx`、滚动回顶部、改标题 |
| 长度 +1 | `SetItemCountEx(LVSICF_NOINVALIDATEALL)` + 只重绘新增那一行（录制时走这条路） |
| 长度其它变化 | `SetItemCountEx` + 整表 invalidate（插/删/读档，都是用户动作） |
| `generation` 变 | 重算 `usedMask`；**只有列集合真的变了才重建列**（否则会把用户拖过的列宽重置） |
| 播放位置变 | 重绘旧行和新行；`m_follow` 打开时按**整页**滚动（见下） |

**长度必须排在重建列之前**：虚拟列表要先有行才长出竖向滚动条，而 `fitColumns()` 量的是客户区宽度。
反过来的话（原来就是这个顺序）列宽是按"没有滚动条"算的，最后一列会被裁掉十几像素
（2026-10-28 在 200% DPI 的真机上量到：客户区 748、列宽合计 739、可用 731）。

`WM_DPICHANGED` **到不了这个消息映射**：`UiFont.cpp` 的子类过程把它整个吃掉，只把控件按比例缩放、
把窗口改成它建议的矩形，然后就 `return 0`——所以早先那个 `OnDpiChanged` 处理函数其实一次都没跑过，
2026-10-28 已经删掉。现在跨 DPI 有两道兜底，都不依赖那条消息：

- `OnSize()` 里比 `uiFont::dpiOf()` 与基线记录的 DPI，不一样就把当前几何当成新基线
  （子控件此刻已经缩放好了，再按位移算一遍就会缩放两次）；
- `refresh()` 每 tick 比一次列表当前的字体句柄和 `m_fittedFont`，变了就重跑 `fitColumns()`。
  DPI 切换和 `Options > Preferences > Use Modern UI Font` 都会换字体，换完必须重量列宽，
  而 50 ms 定时器是唯一确定在换完之后还会跑的地方。开销是一次 `GetFont()`。

所以每 tick 的工作量与帧数无关，几万帧和 20 帧一样。

### 跟随播放位置：整页滚动

`EnsureVisible` 一次只挪一行，播放时列表会一行一行地爬。`followPlayhead()` 改成：

- 播放位置还在可见范围内 → 什么都不做；
- 出界 → 按 `LVM_SCROLL` 滚动 `(target - top) / 每页行数` 页（至少一页），
  再 `EnsureVisible` 兜一次底（最后一页不满时）。因此光标永远落在页首附近。

`revealRow(row, center)` 是跳转用的：**先 `selectOnlyRow()` 把多选收成一行**，再滚到列表**中间**
（先 `EnsureVisible`，再按行高差 `LVM_SCROLL`），所以从第 100 帧跳到第 90000 帧不会只把它贴在底边。
收成一行是有意的：跳过去之后按 `Delete` 不该删掉用户已经看不见的那些行。

### 列宽：按字体量，不写死

旧版把列宽写成固定像素（标记 18 / 帧号 58 / 按键 22），在 200% DPI 上对话框放大了
而列宽没放大，列表里只有三分之一宽度有内容。现在 `fitColumns()`：

1. 取列表自己的字体（`UiFont.cpp` 在 `WM_INITDIALOG` 里把它换成了系统 UI 字体），
   量出数字宽度 `digit`、两个表头宽度、`00:00:00.00` 的宽度、平均字符宽 `pad`；
2. 箭头列 = `2 * digit`；帧号列 = `7 * digit + 2 * pad`（不足表头宽时取表头宽）——
   帧号是扫一眼就要读的东西，比六个数字再多留一位和两侧留白；时间戳列 = 样本宽度 + `pad`；
3. 其余按键列平分剩下的客户区宽度（`客户区 - SM_CXVSCROLL - pad`，给滚动条和右侧留白），
   最后一列吃掉取整余数，右边缘贴合。所以帧号列或时间戳列变宽时按键列自动变窄，不需要另调。
   ⇒ 列宽合计**永远比客户区窄** `SM_CXVSCROLL + pad`（200% DPI 上是 43 px），这是设计不是 bug，
   探针比的是"列宽合计的增量 == 客户区的增量"。

不写死像素也不依赖 dialog unit：对话框创建后字体被换过，`MapDialogRect` 与控件
实际位置已经不再一致。

窗口可以改变大小（模板里加了 `WS_THICKFRAME`），所以列宽必须跟着宽度走，见下面"窗口大小"。

### 窗口大小：纵向拉伸，列宽重算

只有高度是需求（"能多看几行"），但既然加了粗边框，宽度也就跟着能拖；宽度变化时按键列
自动平分多出来的地方，不会留白。

- 尺寸基线来自控件本身：`captureLayout()` 在打开时（以及跨 DPI 之后）把客户区和
  七个相关控件的矩形读一遍存进 `m_layout`，`layoutChildren(cx, cy)` 每次都按
  "基线 + 总位移"重新落位。**不按增量累加**：拖一个来回不会有 1 px 漂移，缩回模板尺寸时
  `dx = dy = 0` 也照样重置（早期版本在 `dx == dy == 0` 时直接 return，结果把窗口缩小回去
  时控件停在放大后的位置——探针第一次跑就撞上了）。
- 谁跟着谁动：帧列表吃掉全部增减；`HINT` 和 `STATUS` 贴底边（左右跟着拉宽）；
  顶栏右半边（"跳转到帧"标题、帧号框、上一个/下一个标记）贴右边。左边的复选框和传输按钮
  不动。"跳转到帧"的标题原本是 `IDC_STATIC`，为了跟着右边走才单独给了
  `IDC_VBM_GOTOLABEL`。
- **宽度和高度都有下限**，两个都由 `WM_GETMINMAXINFO` 给出：
  - 宽度 = 模板宽度：再窄帧号框、标记按钮和列都摆不下，而且没有横向滚动可退。
  - 高度 = **一整个表头 + 窗口里其余全部固定元素**。模板里除了帧列表以外的东西（标题栏、
    传输按钮、两个复选框、提示行、状态行）高度都不变，所以 `captureLayout()` 只量一次差
    `m_fixedHeight = 窗口高 - 列表高`，下限就是 `m_fixedHeight + 表头高`（表头高在
    `updateHeightFloor()` 里从 `LVM_GETHEADER` 的窗口矩形取）。结果是：窗口缩到最矮时
    帧列表正好只剩表头，**其余控件一个不少**——提示行紧贴表头下方，状态行在客户区内。
    字体或 DPI 变化会换掉表头高度，`refresh()` 与 `captureLayout()` 各重算一次。
  - 宽度跨 DPI 时按新旧 DPI 比例缩放（表头高度则由重算给出，更准）。
  - 再往下只能靠程序自己 `SetWindowPos` 强行压（不经过 `WM_GETMINMAXINFO`）：那时列表缩到
    1 px，提示行/状态行停在列表下面不再上移（`layoutChildren()` 里的 `bottomDy` 夹一下），
    再矮就由窗口边框裁掉。这个夹子现在只是防御，正常拖动碰不到。
- 列宽只在**宽度**变化时重算（`m_fittedWidth` 记住上次的列表客户区宽度）：纯纵向拖动
  一次 `SetColumnWidth` 都不做，否则每拖一个像素就重画一遍。
- 关掉再打开会回到上次的尺寸，只在**本次运行内**记住（文件作用域的 `g_lastSize`），
  不写进设置文件；恢复时会抬到当前下限以上，免得把更早记住的小尺寸原样搬回来。

### 图标：为什么只有这个窗口有

程序里别的弹窗都是模态的，模板写 `DS_MODALFRAME`，对话框管理器据此给窗口加上
`WS_EX_DLGMODALFRAME`——**这种边框 Windows 根本不画标题栏图标**，所以那些窗口看起来"没有
图标"并不是谁特意处理过（2026-10-28 用 `vbicon.cpp` 量过：About 框 `DLGMODALFRAME = 1`，
窗口图标和类图标都是 NULL）。

本窗口不能用这条路：它是非模态 + 可缩放边框，`WS_EX_DLGMODALFRAME` 会把边框画成对话框那种。

于是它成了唯一有图标位的窗口，而它自己又没有图标 → 标题栏去借**属主窗口**的图标，主窗口的
窗口类是 `AfxRegisterWndClass(..., LoadIcon(IDI_ICON))` 注册的，于是显示成 GBA 图标。
`WM_SETICON(NULL)` **不管用**：那只是回到"没有就借"的默认回退（实测：窗口图标、类图标都是
NULL，标题栏照样画出图标）。所以 `OnInitDialog` 里装的是一个 1×1 全透明的单色图标
（`blankIcon()`，进程内建一次），大小两种都换掉，标题栏和 Alt+Tab 都不再有图标。

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
- 帧号框（`ES_NUMBER`）跟着**焦点行**走：焦点变了就覆盖它；编辑时回车（`PreTranslateMessage`
  拦 `VK_RETURN`，`OnOK` 也改写成跳转，否则模态对话框的基类会销毁窗口）→ 选中并居中
  （顺带把多选收成一行）。超出范围按最后一帧处理。

### 右键菜单

```text
标记帧 / 取消标记帧       （整个选区；全都标了才变成"取消"）
---
在该帧之前插入一行       （恰好选中一行 + 可插删）
在该帧之后插入一行       （同上；选到最后一行时等于追加）
删除选中的行             （有选中 + 可插删）
---
剪切 / 复制 / 粘贴
---
上一个标记 / 下一个标记
清除所有标记
```

"标记"和"删除"一样针对**整个选区**（右键点在选区内时多选保留，点在选区外时先折叠成那一行）。
粘贴的启用条件最复杂（恰好一行 + 可插删 + 剪贴板能解析 + 长度不溢出），
所以构建菜单时会读一次剪贴板——见下面"剪贴板"。

### 剪贴板

三个命令都走 `vbmcsv` 的 CSV（协议见前面"CSV 协议"）：

| 命令 | 做什么 |
|---|---|
| 复制 | 选中行升序 → `VBAMovieGetInputAt` → `serialize` → 剪贴板。**只读录像也能用** |
| 剪切 | 先复制，成功后才 `deleteRows()`；删除被拒就保留剪贴板并报拒绝原因，不谎报成功 |
| 粘贴 | 恰好一行选中 → 解析剪贴板 → `VBAMovieInsertFramesAt(anchor + 1, …)` → 标记后移 |

- 快捷键 `Ctrl+X/C/V` 在 `PreTranslateMessage` 里拦，**焦点在帧号框时放行**（那里归 EDIT 自己）；
  另有 `ON_COMMAND(ID_EDIT_CUT/COPY/PASTE)`，所以外部 `WM_COMMAND` 也能驱动（探针靠这个）。
- 解析结果按 `GetClipboardSequenceNumber()` 缓存：菜单要知道"粘贴能不能点"，命令不该再解析一遍。
- 被拒时状态栏说原因，能定位的给行号（`Line %d of the clipboard is not frame data this editor can read.`）。

### 只读复选框

它是**这个窗口的编辑锁**，默认勾选，**不碰 `Movie.readOnly`**（那个标志会改变读档语义：
只读时读档只做一致性校验，可编辑时读档会用存档里的序列覆盖整条录像——那是模拟器行为）。

- 录像本身只读（`readOnly` 1 或 2）时：复选框强制勾选 + 禁用，状态栏说明怎么办（1 用菜单解除，2 是文件不可写）；
- 录像可编辑时：复选框可用，勾上=只看不改。

### 标记

对话框内的 `std::set<uint32>`，只存帧号，关闭即消失。菜单项叫"标记帧"，作用于**整个选区**：

- 选中的行**全都已经有标记** → 这一项变成"取消标记帧"（`IDS_VBMEDITOR_UNMARK`），点了全部取消；
- 其余情况（一个都没有，或只有一部分有）→ 保持"标记帧"，点了把缺的都补上。
  混合选区因此收敛到"全标"，而不是逐行翻转（`toggleMarkSelection()`）；
- 快捷键 `M` 与菜单同一套语义：有选区就对整个选区，没有选区才回落到焦点行；
  状态栏按数量说话（`Marked frame 5.` / `Marked 3 frames.`）；
- `F2` / `Shift+F2` 跳标记；顶栏两个按钮做同样的跳转；
- 插一行：`>= row` 的标记 +1；删若干行：被删的标记消失、其余按"下面删了几行"前移
  （`marksInserted()` / `marksDeleted()`，都是一次线性走）；
- "定位"只是列表选中 + 滚动，**不改变模拟器状态**（VBA 没有随机 seek 能力：
  没有逐帧存档，rewind 每槽只有 400 KB，长录像的 `emuWriteMemState` 直接失败）。

🔴 **`M` 必须在 `PreTranslateMessage` 里拦走，不能放在 `LVN_KEYDOWN` 里**：list view 把
字符输入当成"增量搜索"，owner-data 列表会因此给父窗口发 `LVN_ODFINDITEM` 问哪一行匹配；
没人处理时控件取默认值 0，于是**选中第 0 行**——通知处理完时多选已经没了。实测表现是
"`M` 标了 3 帧，但标完只剩第 0 行被选中，再按一次 M 变成 `Marked frame 0.`"。

## 已知限制（有意为之）

- 只显示 controller 0；别的控制器字节原样保留不动（本前端只录 1 个控制器）。
- 本窗口拿到焦点时模拟器会暂停（`pauseWhenInactive` 的既有行为，是有意保留的）；
  点"播放"继续，但那只是把 `theApp.active` 顶住，再切一次窗口仍按原规则重新计算。
- 帧号框跳转只是选中 + 滚动，不把模拟器带到那一帧（VBA 没有 seek）。
- 录制中不能插入/删除（下一帧的 `length_frames = currentFrame` 会把它抹掉），但可以改已经落盘的行。
- END 状态下不能插删（与现有菜单一致），单元格仍可改。
- 至少要留一帧：选中全部行删除会被拒绝（零帧录像重开就是 END，再也插不回去，且没有撤销）。
- **剪贴板只搬 10 个手柄键**：reset / motion / controller 1 不复制、不粘贴。含 reset 的帧被复制粘贴之后
  reset 位就没了——reset 会让游戏重开，编辑器不替用户发明它，但这也意味着"复制粘贴"不是保真的。
- 不增加 `rerecord_count`：手工编辑不是重录。
- 没有撤销、没有框选批量改键（多选只用于行命令）、没有列宽持久化（窗口大小也只在本次运行内记住）、
  不做重播式 seek。
- 与 `editMode` 无关：编辑器直接改 buffer，不走 DISCARD/OVERWRITE/INSERT/XOR 那套录制逻辑。
  编辑完再 Resume Recording 时，OVERWRITE/XOR 模式仍会按它自己的规则写下一帧。

## 验证

探针脚本都放在仓库外的临时目录里，不入库，下面只写文件名。

- `vbm_probe.cpp` + 真实的 `movie.obj`：造一个 20 帧的 VBM，逐项检查
  读取、单元格改写（含 reset 位不被写坏）、插入、删除（含文件截断与头里的长度）、
  追加、录制中改已落盘行（文件追加位置不动）、各种拒绝、Stop 后重新打开内容仍在。
  改造阶段 1 之后又加了批量插删：开头/中间/结尾插入、不连续行删除、reset/motion 位原样保留、
  `currentFrame`/`inputBufferPtr` 不被推动、文件长度与 truncate、删空被拒、空指针/乱序/重复/越界/溢出拒绝、
  只读与 END 拒绝、重新打开不复活被删的帧；阶段 6 又加了性能项（10 万帧录像上一次删/插 1 万行）。
  **当前 155 项全过**（原有 42 + 批量 102 + 性能 11）。
  用 `~/.workbuddy/scratch/vbm_probe_build.py` 构建：它每次把 `vbm_probe.cpp` 和**当前源码树里的**
  `movie.cpp` 一起重编（不带 `NDEBUG`，所以 `movie.cpp` 里的 `assert` 是活的），不链 zlib
  （探针自己桩掉了 `utilGz*`）。
- **`vmbshot.cpp`**：真的把 `VBA_check.exe` 跑起来，
  `WM_COMMAND(ID_TOOLS_VBMEDITOR)` 打开窗口，`PrintWindow` + 屏幕 `BitBlt` 各存一张 BMP，
  然后 `WM_CLOSE` → 再打开一次，验证"关掉后还能再开"。最后 `TerminateProcess`，
  不触发退出时的设置写回（`reg.exe export` 被沙箱拉黑，没法备份注册表）。
  注意它自己那两个坑：主窗口句柄要在找对话框之前单独存下来（第二次枚举会把 `main` 清掉），
  以及 `IsWindowVisible` 过滤会漏掉"藏起来的窗口"——那个 bug 的第一现场正是这样找到的。
- **`vbmsize.cpp`**（2026-10-28，窗口大小与标记）：同样用真 ROM + 真录像
  （`aos.gba` + `Fk-castlevaniaariaofsorrow-newwarpless.vbm`，72647 帧）打开窗口，然后：
  量模板尺寸下的客户区/列表/提示行/状态行 → `SetWindowPos` 加宽 150、加高 260 →
  断言列表宽高各涨了同样的数、左上角没动、提示行与状态行到客户区底边的距离不变、列宽合计
  涨了 150（`fitColumns()` 会**故意**留出滚动条宽度 + 一个 `pad`，所以比的是增量不是绝对值）→
  读 `WM_GETMINMAXINFO`（小于 `WM_USER` 的消息由系统自动跨进程编组，不用 `VirtualAllocEx`；
  高度应等于"模板高 − 列表高 + 表头高"）→ 按这个下限精确 `SetWindowPos`，看列表是否正好只剩
  一个表头、提示行是否紧贴表头下方、状态行是否仍在客户区里 → 再缩到比模板矮 900 px（绕过下限，
  模拟"被程序强行压扁"），看列表是否缩到 1 px、提示行/状态行是否仍然停在列表下面 →
  再拖回模板尺寸，看列表是否**像素级**回到原处（这一条正是"`dx == dy == 0` 时
  提前 return"那个 bug 的守门员）→
  选中 1/2/3 行按 `M`、再按一次、单行按、混合选区按，逐条比对状态栏文字 →
  关窗后再开，尺寸是否被记住。
  **第一次跑就抓到两个真 bug**：`layoutChildren()` 在 `dx == dy == 0` 时提前返回，导致窗口缩回
  模板尺寸时控件停在放大后的位置；以及 `M` 走 `LVN_KEYDOWN` 会被 list view 的增量搜索
  改成选中第 0 行（见"标记"）。**修完 41 项全过**。
  它自己的坑：`LVM_SETITEMSTATE` 的 `wParam = -1` 表示"所有行"，但 `lParam` 传 NULL 是**静默无效**，
  必须真的把 `LVITEM` 写进对方进程，否则选项会一直累加，测出来的"标了 4 帧"全是假的。
- `dlgshot.exe` 把 `IDD_VBM_EDITOR` 从资源里取出来渲染成 BMP 核对版面。
- **`vbmcsv_probe.cpp`**（阶段 3）：只链 `VbmFrameCsv.cpp`，连桩都不需要
  （`module_probe_build.py vbmcsv_probe.cpp VbmFrameCsv.cpp vbmcsv_probe`）。
  **46 项全过**：序列化的字节级输出、64 行往返、列序/大小写/空白、BOM、三种行尾、空行、空字段、
  9 种拒绝、`badLine` 定位、两个上限、空表头往返。
- **`vbmui.cpp`**（阶段 2 新增）：用真 ROM + 真录像驱动这个窗口。
  `vbmui.exe <exe> <rom> <movie.vbm> <out.bmp>` 把录像**复制到 %TEMP%** 再
  `-rom ... -play <副本> 0` 启动（绝不碰用户的原录像），然后：
  `WM_COMMAND(ID_TOOLS_VBMEDITOR)` 开窗口 → `BM_CLICK` 清掉默认勾上的 Read-only →
  用 `LVM_SETITEMSTATE` 选中三行（`LVITEM` 要先 `VirtualAllocEx` 写进对方进程，这个消息带指针）→
  查 `LVM_GETSELECTEDCOUNT` 和帧号框 → `WM_KEYDOWN(VK_DELETE)` 给列表 →
  回读 `%TEMP%` 里的 .vbm 逐帧比对 → 再 `VK_INSERT` 一遍 → 最后 `WM_COMMAND(ID_EDIT_COPY/CUT/PASTE)`
  把剪贴板也走一遍（`ID_EDIT_*` 就是为这个加的）。**当前全过**。
  它自己的坑：
  - 跨进程读 EDIT 必须 `WM_GETTEXT`，`GetWindowText` 只返回系统缓存的窗口标题，EDIT 永远是空串
    （Static 反而读得到，所以只看状态栏会以为一切正常）；
  - 探针和被测 exe **必须都 DPI-aware**，否则一个 200% 的显示器上 `GetWindowRect` 拿到的是折半的
    虚拟坐标、`PrintWindow` 只截到左上角四分之一；
  - 真实点击（Ctrl+点击不改键）没法用 PostMessage 模拟——控件读的是 `GetKeyState`，只有真输入才更新。
    所以那两条守卫只有代码级检查，没有真机证据。同理 `Ctrl+X/C/V` 也只验证了 `ID_EDIT_*` 那条路。
  - 它会把用户的剪贴板内容覆盖掉（测试粘贴要自己写剪贴板）。
- 全量 `build_check.py`（179 个 TU）+ `link_check.py` 编译链接通过；
  单文件改动用 `one_obj.py` 重编一个 `.obj` 再链接，省掉两分钟全量。
  `link_check.py` 现在会把 `win32/VBA_Latest.manifest` 复制到 exe 旁边（沙箱里没有 `mt.exe`，
  没法 `/MANIFEST:EMBED`），否则打出来的 exe 是 DPI-unaware 的，截图和坐标全是错的。
- 资源：`rc.exe` 编 `vba.rc` 与 `vba_chs.rc` 均无错；`make_rc.py --check` 通过。

## 改造计划：多选、插入与 CSV 剪贴板

> **状态：阶段 0–6 全部完成（2026-10-28）。** 多选、插入前/后、批量删除、CSV 剪切/复制/粘贴都能用；
> 验收结果与没做到的部分见本节末尾"阶段 6"。

### 目标与非目标

目标：

- 列表支持 Ctrl/Shift 多选；
- 右键菜单增加"在该帧前插入一帧 / 在该帧后插入一帧"；
- 增加剪切、复制、粘贴；
- 剪贴板使用可读 CSV，支持"复制 → 记事本编辑 → 复制回来 → 粘贴"；
- 只读、录制中、END 状态下禁止破坏性操作，复制仍可用；
- 大批量剪切/粘贴不出现明显卡顿。

非目标：

- 不做撤销；
- 不复制 reset / motion / controller 1；
- 不支持字段内逗号、引号等完整 CSV 方言；
- 不改变模拟器状态、`currentFrame`、seek 行为；
- 不做列宽持久化。

### CSV 协议

#### 写出格式

固定表头，列顺序与编辑器网格一致：

```csv
frame,Left,Up,Right,Down,A,B,L,R,Start,Select
120,0,0,0,0,0,0,0,0,0,0
121,0,0,1,0,1,0,0,0,0,0
```

- `frame`：原帧号，**仅供阅读，粘贴时忽略**；
- 其余 10 列：`0` 或 `1`；
- `L` / `R` 是肩键，`Left` / `Right` 是方向键；
- 使用 CRLF 和末尾换行，方便记事本；
- 始终写完整 11 列；
- 只写 `VBAMovieGetInputAt(row, 0) & BUTTON_REGULAR_MASK`，即 10 个普通手柄位。

#### 读取与校验

- 优先读 `CF_UNICODETEXT`，没有则回退 `CF_TEXT`；
- 必须有表头；列名忽略大小写和首尾空白；
- 按列名映射，允许用户调整列顺序，但 11 个列名必须各出现一次；
- 每行字段数必须与表头一致；
- `frame` 必须是非负十进制整数，但粘贴时不使用；
- 按键字段只接受 `0`、`1`，空字段视为 `0`；
- 支持 CRLF / LF / CR、结尾空行、中间空行、可选 BOM；
- 不接受字段内逗号或引号；
- 至少一行数据；
- 上限先定为 500,000 行 / 16M 字符，超过则拒绝；
- 粘贴还要满足 `行数 <= MOVIE_MAX_FRAMES - 当前长度`；
- 被拒时**带行号**（`parse()` 的 `badLine`，从 1 数、空行也计数），状态栏才能指到那一行。

### 行为矩阵

```cpp
canRestructure = editable()
              && !VBAMovieIsRecording()
              && m_state != MOVIE_STATE_END;
```

| 菜单项 | 启用条件 |
|---|---|
| 复制 | 有选中行；只读也可用 |
| 剪切 | 有选中行且 `canRestructure` |
| 粘贴 | 恰好选中一行、`canRestructure`、CSV 解析成功、长度不溢出 |
| 在该帧前插入一帧 | 恰好选中一行且 `canRestructure` |
| 在该帧后插入一帧 | 恰好选中一行且 `canRestructure` |
| 删除 | 有选中行且 `canRestructure`，删除全部选中行；**选中全部行时拒绝**（core 返回 `MOVIE_WRONG_FORMAT`，界面提示"至少要留一帧"） |
| 标记帧 / 取消标记帧 | 总是可用；作用于整个选区，选区全都已标记时才变成"取消标记帧" |

右键菜单顺序：

```text
标记/取消标记
---
在该帧前插入一帧
在该帧后插入一帧
删除选中帧
---
剪切
复制
粘贴
---
上一个标记
下一个标记
清除所有标记
```

### 实施阶段

#### 阶段 0：基线与保护 ✅

工作：

- 跑一次现有全量构建、链接、资源编译和 VBM 探针，记录当前 **42** 项结果（早先记成 43 是数错了）；
- 确认 `build_check.py` / `link_check.py` / `one_obj.py` 的可用性；
- 不改代码。

退出条件：基线测试全过，后续可以明确判断回归。✅

#### 阶段 1：批量 core API ✅

文件：`src/common/movie.h`、`src/common/movie.cpp`。

新增 API：

```cpp
int VBAMovieInsertFramesAt(uint32 row, const uint16 *values,
                           uint32 count, int controller = 0);
int VBAMovieDeleteFramesAt(const uint32 *rows, uint32 count);
```

实现要求：

- 插入：
  - 一次 `reserve_movie_buffer_space`；
  - 一次 `memmove`；
  - 新区域清零后写入 controller 0 的按键值；
  - 只写 `BUTTON_REGULAR_MASK`；
  - 一次 header + frames flush。
- 删除：
  - 要求 `rows` 升序、无重复；
  - 一次线性压缩；
  - 一次 frames flush + `truncate_movie`。
- 两者共同：
  - 校验 `row` / `count` / `MOVIE_MAX_FRAMES`；
  - `VBAMovieConvertCurrent(false)`；
  - 重算 `inputBufferPtr`；
  - `preserve_movie_next_input()`；
  - `inputGeneration++`；
  - 不碰 `currentFrame`、`state`、`editMode`、`readOnly`、`rerecord_count`。

实现细节（照现在的代码写，别再重新推导）：

- **`count == 0` 是成功的空操作**，在任何校验之前返回，连 `inputGeneration` 都不动 ——
  调用方（空选区、空剪贴板）不用自己加分支。
- **`values` 不许指进 `Movie.inputBuffer`**：`reserve_movie_buffer_space()` 可能把它搬走，
  而且 `memmove` 会把 `row` 及以后的字节整体上移。编辑器那边是解析进 `std::vector<uint16>` 的，天然满足。
- **插入的边界检查必须在 `VBAMovieConvertCurrent()` 之前做完**：转换只改 `minorVersion`、
  第 0 帧和旧 reset 位，**不改 `length_frames`**，所以前面算出的上界一直有效（早先那版在转换后又抄了一遍，
  是死代码，已删）。
- **`length_frames >= MOVIE_MAX_FRAMES` 必须排在 `count > MOVIE_MAX_FRAMES - length_frames` 前面**，
  否则减法会回绕。
- 🔴 **不允许删空**（`count >= length_frames` 直接 `MOVIE_WRONG_FORMAT`）。零帧录像重新打开时是
  `MOVIE_STATE_END`，而 END 拒绝插入 ⇒ 编辑器再也塞不回一帧，而且没有撤销。删到只剩一行仍然允许。
  单行 `VBAMovieDeleteFrameAt()` 不受这条限制（它只可能是用户对某一行的一次明确动作）。
- 删除用「一段一段 `memmove` 到前部」压缩，每段目的地址都 ≤ 源地址，重叠安全；最后 `assert` 一下
  压缩后行数等于 `newLength`，再 `memset` 旧尾巴、`flush_movie_frames()` + `truncate_movie(newLength)`。

测试：在 `vbm_probe.cpp` 中增加批量插入开头/中间/结尾、批量删除不连续行、
文件长度/header/truncate/重新打开、reset/motion 原行保持不变、
`inputBufferPtr`/generation/`currentFrame`、录制中/END/只读/越界/溢出/空指针拒绝。

结果：探针从 42 项扩到 **144 项，全过**。为了能真正验证"播放位置没动"，探针直接把 `SMovie`
的 `currentFrame`/`inputBufferPtr` 设成 7（core 没有 setter，真前端是靠跑帧推进的），
再断言批量插删之后这一对仍然自洽。
（阶段 6 又补了性能项，现在是 155 项，见"验证"。）

退出条件：批量 API 探针全过，现有 42 项不回归。✅

#### 阶段 2：多选基础 + 插入后 ✅

文件：`src/win32/vba.rc`、`src/win32/Dialogs/VbmEditor.h`、`src/win32/Dialogs/VbmEditor.cpp`。

工作：

1. 去掉 `IDD_VBM_EDITOR` 的 `LVS_SINGLESEL`。
2. 新增选择辅助：

   ```cpp
   int focusedRow() const;
   std::vector<uint32> selectedRows() const;
   void selectOnlyRow(uint32 row);
   ```

3. `m_selectionRow` 改名为 `m_focusRow`，帧号框显示焦点行。
4. `revealRow()` 先清空全部选中，再选中目标行，保证跳转/标记/帧号框回到单选。
5. `OnListItemChanged()` 只更新焦点行，不在 50 ms 定时器里枚举全部选中行。
6. `OnListClick()`：
   - 按住 Ctrl/Shift 时不触发单元格切换；
   - 选中行数不为 1 时不触发。
7. `OnListRightClick()`：
   - 右键点在已选中行上：保留多选，只移动焦点；
   - 右键点在未选中行上：清空其他选中，只选该行。
8. 菜单与快捷键：
   - `Insert` 仅在恰好选中一行时插入到该帧之前；
   - 新增"在该帧后插入一帧"，调用 `insertRow(row + 1)`；
   - `Delete` 改为删除全部选中行。
9. 标记调整：

   ```cpp
   void marksInserted(uint32 at, uint32 count);
   void marksDeleted(const std::vector<uint32>& rows);
   ```

10. 插入后状态提示、播放位置位移警告。

实现细节（照现在的代码写）：

- `focusedRow()` **返回缓存 `m_focusRow`**，不去问控件。`GetNextItem(LVNI_FOCUSED)` 会走一遍列表，
  而它在 50 ms 定时器里——那正是要避免的 O(n)。写入点只有 `LVN_ITEMCHANGED`（只认"拿到焦点"那条）、
  `selectOnlyRow()` 和右键菜单；`refresh()` 只做 O(1) 的越界钳制。
- `LVN_ITEMCHANGED` **忽略"丢掉焦点"那条**：焦点从 A 挪到 B 时控件先报 A 丢焦点，
  照那条走会把帧号框清空一瞬间再填上。
- `selectOnlyRow()` 用 `SetItemState(-1, 0, LVIS_SELECTED)` 清全场再选中目标，**它是唯一的"变回单选"入口**
  （跳转、标记、帧号框、右键选区外）。
- `canRestructure()` 把"可编辑 + 不在录制 + 不是 END"这三条收成一处，菜单灰化和命令里的拒绝共用；
  `denyRestructure()` 按原因写状态栏，别在三个命令里各抄一遍。
- 删除走 `VBAMovieDeleteFramesAt()`（一次压缩、一次 truncate），不是循环单行删除。
  删完焦点落在**第一个被删行的位置**（也就是原来跟在后面的那行），末尾删光就落在最后一行。
- `marksDeleted()` 用一次双指针走完两个有序集合：某个标记下面删掉了几行，它就往前挪几行；
  自己就是被删行则丢弃。
- 插入后焦点落在**新插的那一行**并 `EnsureVisible`，方便接着填键。
- 顺带修了一个既有的列宽 bug：`refresh()` 里"设行数"必须排在"重建列"之前，
  否则 `fitColumns()` 量的是没有滚动条的客户区，最后一列被裁（详见上面"刷新状态机"）。
- 资源：`IDS_VBMEDITOR_INSERT_ROW` 的英文原本就是"before this frame"，只把 `IDS_VBMEDITOR_DELETE_ROW`
  改成"&Delete the selected rows"；新增 `IDS_VBMEDITOR_INSERT_ROW_AFTER`(334)、
  `IDS_VBMEDITOR_DELETED_MANY`(335)、`IDS_VBMEDITOR_NEED_ONE_ROW`(336)，中文同步进 `strings.tsv`。

结果：`vbmui.cpp`（见"验证"）在真 ROM + 72647 帧真录像上驱动这个窗口，多选三行 → 帧号框显示焦点行 →
`Delete` → 网格少三行、文件头长度 −3、文件被截断、未删的帧按序前移、焦点落在第一个被删的位置；
再选中一行 `Insert` → 多一行、原行下移、新行为空、文件同步。全过。

退出条件：多选、右键、跳转、单元格编辑互不干扰；插入前/后仅单行可用；
最后一行后插入等于追加；删除多行后列表、标记、帧号框正确。✅（右键菜单的灰化状态只有代码级检查，
见"验证"里写的限制）

#### 阶段 3：CSV 模块 ✅

新文件：`src/win32/Dialogs/VbmFrameCsv.h`、`src/win32/Dialogs/VbmFrameCsv.cpp`。

接口：

```cpp
namespace vbmcsv {

struct Row {
    uint32 frame;
    uint16 buttons;   // 0..1023
};

enum class Result {
    Ok,
    Empty,
    TooLarge,
    BadHeader,
    BadFieldCount,
    BadFrame,
    BadValue,
    NoRows,
    TooManyRows
};

// badLine 是可选出参：被拒的那一行（从 1 数，空行也算，跟记事本一致）
Result parse(const std::wstring &text, std::vector<uint16> &buttons, int *badLine = 0);
Result serialize(const std::vector<Row> &rows, std::wstring &text);

}
```

要求：

- 不依赖 MFC，方便单独做探针；
- 序列化只写 `buttons & BUTTON_REGULAR_MASK`；
- 解析返回错误码，由 UI 映射到本地化字符串；
- 解析器不使用 `sscanf`/`%u` 直接吞整行，避免接受尾部垃圾；
- 行拆分、字段拆分、空白处理、BOM、换行符全部覆盖测试。

实现细节：

- **`badLine` 是我加的，计划里没有**：只有错误码的话，用户手工编辑 500 行 CSV 时被告知"有个字段不对"，
  没法定位。行号按记事本的口径数（空行也计数），整体失败（Empty/TooLarge）时为 0。
- 列名匹配是**纯 ASCII 折叠**，不碰 locale；`frame` 列手工累加并查 32 位溢出，
  所以 `12abc`、`-1`、`1.5`、`0x1` 全部 `BadFrame`，不会被 `%u` 那样悄悄吃掉前缀。
- BOM 在 `LineReader` 的构造函数里跳过（只跳一个 `0xFEFF`），不参与字段裁剪。
- 空字段 = 0，只有 `0`/`1`/空三种取值；引号、字段内逗号都不支持（引号 → `BadValue`，逗号 → 字段数不符）。
- `TooManyRows` 在第 500001 行**推入之前**返回，所以 `buttons` 里恰好留 500000 条。
- 表头必须 11 列、11 个名字各一次；列顺序随意。

测试：`vbmcsv_probe.cpp`（`module_probe_build.py` 构建，只链 `VbmFrameCsv.cpp`，连桩都不需要）。
**46 项全过**：序列化的字节级输出（含 reset/motion 位不外泄）、64 行往返、列序打乱 + 大小写 + 空格、
BOM、CRLF/LF/CR、空行、空字段、9 种拒绝、行号定位、两个上限、空表头往返。

工程改动：`win32/VBA_Latest.vcxproj` 加了 `ClCompile`/`ClInclude` 各一条；`build_check.py` 从工程文件
读单位列表，自动变成 180 个 TU。

退出条件：CSV 探针覆盖合法/非法/边界/往返测试，全过。✅

#### 阶段 4：剪切、复制、粘贴 ✅

文件：`src/win32/Dialogs/VbmEditor.h`、`src/win32/Dialogs/VbmEditor.cpp`、
`src/win32/resource.h`、`src/win32/vba.rc`。

工作：

1. 剪贴板读写：
   - `writeClipboardText(const std::wstring&)`：`CF_UNICODETEXT`；
   - `readClipboardText(std::wstring&)`：优先 `CF_UNICODETEXT`，回退 `CF_TEXT`；
   - 读取前用 `GlobalSize` 做大小上限检查；
   - `OpenClipboard` 失败时禁用粘贴或显示状态。
2. 复制：
   - 取 `selectedRows()` 升序；
   - 取 `VBAMovieGetInputAt(row, 0) & BUTTON_REGULAR_MASK`；
   - 调用 `vbmcsv::serialize`；
   - 写剪贴板；
   - 状态：`Copied %u frames.`
3. 剪切：
   - 先检查 `canRestructure`；
   - 复制成功后才调用批量删除；
   - 复制失败不能删；
   - 状态：`Cut %u frames.`
4. 粘贴：
   - 恰好一行选中 + `canRestructure` + CSV 合法；
   - 解析为 `std::vector<uint16>`；
   - 调用 `VBAMovieInsertFramesAt(anchor + 1, values, count)`；
   - 更新标记：`>= anchor + 1` 的标记后移 `count`；
   - 状态：`Pasted %u frames after frame %u.`
5. 右键菜单：
   - 构建菜单时读取并解析一次剪贴板；
   - 按行为矩阵设置 `MF_GRAYED`；
   - 粘贴命令复用已解析数据，必要时用剪贴板序号复核。
6. 快捷键：
   - `Ctrl+C` 复制；
   - `Ctrl+X` 剪切；
   - `Ctrl+V` 粘贴；
   - `Delete` 删除全部选中；
   - `Insert` 仅单行插入前。
7. 状态与错误：
   - 剪贴板无效；
   - 剪贴板过大；
   - 粘贴超过最大帧数；
   - 需要恰好选中一行。

退出条件：复制到记事本可见、可编辑、可贴回；剪切先复制后删除；
粘贴按 CSV 顺序插入，`frame` 列被忽略；只读/录制/END 下剪切、粘贴、插入、删除禁用；
大批量剪切/粘贴不卡顿。

实现细节：

- 剪贴板读写是 `VbmEditor::writeClipboard` / `readClipboard`：写只写 `CF_UNICODETEXT`；
  读先 `CF_UNICODETEXT`，没有才回退 `CF_TEXT`（走 `winWideFromAnsi()`，也就是进程 ACP），
  读之前用 `GlobalSize` 判上限，**超限直接拒绝**，不做截断。
- 🔴 **解析结果按 `GetClipboardSequenceNumber()` 缓存**（`m_clipSequence` / `m_clipButtons` /
  `m_clipResult` / `m_clipBadLine`）。右键菜单要知道"粘贴能不能点"，命令又不该把 10 MB 再解析一遍；
  序号在**任何程序**写剪贴板时都会变，正好当缓存键。粘贴命令直接用缓存里的 `m_clipButtons`。
- **剪切是"先复制、再删除"，删除失败就不算剪切**：`deleteRows()` 返回 bool，剪贴板已经写入但删除被拒时
  保留剪贴板、状态栏说的是拒绝原因，不谎报成功。
- 粘贴位置是 `anchor + 1`（焦点行之后），标记按 `marksInserted(anchor + 1, count)` 后移；
  焦点落到第一行粘贴内容。
- **新增 `ON_COMMAND(ID_EDIT_CUT/COPY/PASTE)`**：这三个是 MFC 的标准编辑命令 id，
  这样剪贴板既能被快捷键触发，也能被外部 `WM_COMMAND` 触发（探针就是靠这个驱动它的，
  否则只能靠 `GetKeyState`，而合成输入改不了它）。
- `Ctrl+X/C/V` 在 `PreTranslateMessage` 里拦，**焦点在帧号框时放行**（那里是 EDIT 自己的复制粘贴）。

结果：`vbmui.cpp` 真机驱动——选中两行 `ID_EDIT_COPY` → 剪贴板里的 CSV 与按文件内容独立算出的期望值
逐字节相同；手写两行 CSV 到剪贴板 → `ID_EDIT_PASTE` → 文件多两帧、锚点行不动、旧下一行下移两格、
文件长度同步；无表头 / 第 3 行有 `9` 两种粘贴 → 文件一帧没变、状态栏分别说"读不出来"和"第 3 行"；
选中两行 `ID_EDIT_CUT` → 文件少两帧且被截断、剪贴板留着这两帧的 CSV。全过。

退出条件：✅（"复制到记事本可见、可编辑、可贴回"这条是用剪贴板文本 + 独立解析验证的，
没有真的打开记事本；"大批量不卡顿"见阶段 6 的性能项）

#### 阶段 5：资源、本地化、文档

文件：`src/win32/resource.h`、`src/win32/vba.rc`、
`src/win32/lang/chs/strings.tsv`、`src/win32/lang/chs/vba_chs.rc`（生成）、
`src/win32/vba_l10n_version.h`（生成）、`documentation/vbm-editor.md`、`README.md`。

工作：

1. 新增资源 ID，接在现有 VBM Editor 字符串之后（约 334 起）：
   - 插入后、剪切、复制、粘贴；
   - 复制/剪切/粘贴成功；
   - 剪贴板无效/过大；
   - 需要单行；
   - 粘贴溢出；
   - 批量删除。
2. 把现有 `IDS_VBMEDITOR_INSERT_ROW` 文案改为"在该帧前插入一帧"。
3. 中文翻译写入 `strings.tsv`：
   - 英文原文必须与 `vba.rc` 完全一致；
   - 运行 `python make_rc.py` 重新生成 `vba_chs.rc` 和 `vba_l10n_version.h`；
   - 运行 `python make_rc.py --check`。
4. 文档补充：
   - 多选操作；
   - CSV 表头、字段、取值、示例；
   - `frame` 列仅阅读；
   - 只读/录制/END 行为；
   - 不复制 reset/motion/controller 1；
   - 批量插删和性能；
   - 无撤销。

退出条件：中英文资源编译无错；`make_rc.py --check` 通过；
文档与实际菜单/行为一致。

#### 阶段 6：回归与验收 ✅

构建/资源：

- 全量 `build_check.py`（TU 数会因新增 CSV 文件增加）；
- `link_check.py`；
- `rc.exe` 编译 `vba.rc` 和 `vba_chs.rc`；
- `make_rc.py --check`。

自动/探针：

- 现有 `vbm_probe.cpp` 43 项；
- 新增批量插删探针；
- 新增 CSV 解析/序列化探针；
- `vmbshot` 打开/关闭、再次打开；
- 可选：中英文界面截图检查新菜单文字不溢出。

手工 UI：

- Ctrl/Shift 多选；
- 右键在选区内/外；
- 多选下 Ctrl/Shift 点击不改按键；
- 帧号框显示焦点行，输入回车回到单选；
- 插入前/后仅单行可用，最后一行后插入等于追加；
- 复制到记事本、编辑、贴回；
- 剪切后行删除、剪贴板内容仍在；
- 只读、录制、END 状态下各命令的启用/禁用；
- 标记随插删移动；
- 播放位置在操作点之后出现位移警告。

性能：

- 10 万帧录像，选中 1 万行剪切/粘贴；
- 目标是批量操作只做一次线性搬移和一次文件写入，不出现秒级卡顿。

实际结果（2026-10-28）：

| 项 | 结果 |
|---|---|
| `build_check.py` | 180 个 TU，0 个有诊断 |
| `link_check.py` | 180 个 obj 链出 `VBA_check.exe` |
| `rc.exe` | `vba.rc` / `vba_chs.rc` 均无错 |
| `make_rc.py --check` | 通过（74 块 / 1115 条） |
| `vbm_probe` | 155 项全过 |
| `vbmcsv_probe` | 46 项全过 |
| `vbmui`（真 ROM + 72647 帧真录像） | 全过：多选删除、插入、复制、粘贴、两种拒绝、剪切 |
| 性能 | 10 万帧上一次删/插 1 万行都 **< 1 ms**（`clock()` 分辨率以下） |

**没做到的部分（如实记下来）**：

- 真实 `Ctrl/Shift + 点击` 没有真机验证——`OnListClick` 读 `GetKeyState`，合成输入改不了它，
  所以"多选时点单元格不改键"只有代码级检查；
- `Ctrl+X/C/V` 走的是同一个函数，但只验证了 `ID_EDIT_*` 那条命令通路；
- 右键菜单的灰化状态没有逐项截图核对（`TrackPopupMenu` 是模态的，探针没法安全地把它点掉）；
- 中文界面没有重新截图：新文案都在右键菜单和状态栏里，菜单自己按内容定宽，没有溢出风险。

### 文件级改动清单

| 文件 | 改动 |
|---|---|
| `src/common/movie.h` | 批量插删 API 声明 |
| `src/common/movie.cpp` | 批量插删实现 |
| `src/win32/Dialogs/VbmFrameCsv.h/.cpp` | 新增 CSV 解析/序列化 |
| `src/win32/Dialogs/VbmEditor.h` | 选择辅助、剪贴板、插删、标记函数、`m_focusRow`、布局与 DPI 状态 |
| `src/win32/Dialogs/VbmEditor.cpp` | 多选交互、菜单、快捷键、CSV 剪贴板、批量操作、缩放布局、透明图标 |
| `src/win32/vba.rc` | 去掉 `LVS_SINGLESEL`，加 `WS_THICKFRAME`，新增/修改字符串 |
| `src/win32/resource.h` | 新增字符串 ID、`IDC_VBM_GOTOLABEL` |
| `src/win32/UiFont.h` | 导出 `dpiOf()`（`OnSize` 用它发现跨 DPI 的那一秒） |
| `src/win32/lang/chs/strings.tsv` | 中文翻译 |
| `src/win32/lang/chs/vba_chs.rc` | 由 `make_rc.py` 生成 |
| `src/win32/vba_l10n_version.h` | 由 `make_rc.py` 生成 |
| `win32/VBA_Latest.vcxproj` | 加入 CSV 新文件 |
| `documentation/vbm-editor.md` | 功能、协议、限制 |
| `README.md` | VBM Editor 功能简介 |

### 风险与应对

| 风险 | 应对 |
|---|---|
| 多选破坏原有"点击单元格即改键" | Ctrl/Shift 按下不触发；选中行数不为 1 不触发；手工回归 |
| 右键丢失多选 | 选区内保留、选区外折叠；单独测试 |
| 批量 core 破坏 `inputBufferPtr` / 文件尾 | 复用现有单行实现的顺序；探针覆盖重新打开、truncate、generation |
| CSV 方言不统一 | 固定表头 + 严格字段数；文档写清楚；错误状态提示 |
| 记事本/Excel 编码差异 | 优先 Unicode 剪贴板；接受 BOM/CRLF；不支持的格式明确拒绝 |
| 大剪贴板卡死 | 行数/字符数上限；解析前检查 `GlobalSize` |
| 本地化资源漂移 | 改 `vba.rc` 后跑 `make_rc.py`，再 `--check` |
| 剪切是破坏性操作且无撤销 | 先复制成功再删除；状态栏明确提示；如需可对大剪切加确认（默认不加） |

### 预计工作量

| 阶段 | 预计 |
|---|---|
| 基线 + 批量 core API | 0.5–1 天 |
| 多选 + 插入后 | 1–1.5 天 |
| CSV 模块 + 探针 | 0.5–1 天 |
| 剪切/复制/粘贴 | 1 天 |
| 资源/本地化/文档 | 0.5 天 |
| 回归/性能/验收 | 1 天 |

合计约 4–5 个工作日，主要时间在多选交互回归和大批量操作验证。

实施顺序上，先做阶段 1（批量 core API）：它独立于 UI，可以先用探针验证，
风险最低；之后再做多选和剪贴板。
