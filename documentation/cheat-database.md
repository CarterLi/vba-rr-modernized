# 金手指数据库导入（EZ-Flash 包 / libretro .cht）

## 它做什么

用户在 `选项 → 金手指` 里指定一个数据库文件，模拟器把该游戏的金手指列出来让用户勾选，
勾中的写进现有的金手指列表。入口有两个：菜单 `Cheat → Load Cheats from Database...`，
以及金手指列表窗口里的 **Load Database...** 按钮。支持两种来源：

| 来源 | 是什么 | 地址 |
|---|---|---|
| `EZ-FLASH OMEGA.zip`（或解压后的目录） | 官方发布的 CHEATS 包，一个包覆盖整个库 | 编码值，**内存区在包里就丢了**（见下） |
| `*.cht`（libretro-database） | 一个文件一个游戏，文件名就是游戏名 | 绝对值（CodeBreaker / GameShark 码） |
| `*.cht`（从包里解出来的单个文件） | 就是包里的一个文件，单独拿出来用 | 同上，同样会丢内存区 |

三种来源最后都进同一个对话框。**扩展名不能用来分派**：EZ-Flash 包里的 928 个文件也
都是 `.cht`，和 libretro 的扩展名完全一样。所以只有 `.zip` / 目录按"包"处理，单个
`.cht` 一律先读字节、判断格式（见"怎么判断是哪种格式"），再交给 `ezflashdb.cpp` 或
`libretrocht.cpp`。两者都产出 `common/cheatdb.h` 里的 `CheatDbFile`。

导入后走的是**原有链路**：包里的写入被格式化成 VBA 自己的 raw 码（`XXXXXXXX:YY`）交给
`cheatsAddCheatCode()`；libretro 的代码文本交给 `cheatsAddCBACode()` /
`cheatsAddGSACode()`——也就是"Add GameShark/CodeBreaker"用的同一套解析。所以列表、
分组、Enable/Dis.、Explain、`.clt` 存取、存档里的金手指块全都不需要改，也不需要新的
指令类型。

## 怎么判断是哪种格式

两个格式的标记**完全不重叠**。实测（包 928 个文件 / libretro 94 个文件）：

| 标记 | 包 | libretro | 作用 |
|---|---|---|---|
| `[GameInfo]` 固定的末段 | 928 | 0 | 决定性（包） |
| `[Some_Name]` 每条一个段 | 928 | 0 | 弱（包） |
| `Switch=addr,bytes` 数据行 | 875 | 0 | 弱（包） |
| `cheatN_desc` / `cheatN_code` / `cheatN_enable` | 0 | 94 | 决定性（libretro） |
| `cheats = N` 文件头 | 0 | 94 | 弱（libretro） |

`CheatDbSniffFormat()`（`common/cheatdb.cpp`）把文本按行扫一遍：

- 出现 `[GameInfo]` → 包；出现 `cheatN_` 键 → libretro。这两条都是决定性的。
- 两条都出现（人工拼过的文件）→ `UNKNOWN`，**不投票**：靠弱标记猜不如让读取器试。
- 都没有 → 数弱标记（段头 + `Switch=` 行，各封顶 20；对 `cheats =` 行，封顶 2 个、
  每个算 3 分），还平手就是 `UNKNOWN`。

`UNKNOWN` 不是错误：UI 会**依次试两个读取器**。它们都不会把对方的文本凭空认成条目
（包读取器把 `cheat0_desc` 当坏地址丢掉，libretro 读取器找不到 `cheatN_` 就什么也不
产出），所以试第二次没有副作用，还能救回标记被编辑掉的文件。

于是"直接选一个从包里解出来的 `.cht`"能正常工作：格式由内容认出来，地址走截断地址那
一套，Memory area 下拉框照样可用——单文件路径会把文本留在内存里，换区时用
`EzParseChtText(text, len, file, base)` 重解析（包里是 `EzCheatDb::ReloadWithBase`，
两者行为一致）。

包打成 zip 但**没有** `GameID2cht.bin` 时（比如有人只把单个 `.cht` 打包），
`EzReadLoneCht()` 会数包里的 `.cht`：恰好一个就读出来当单文件处理；多个则报错说明
"有 N 个 cht、没有索引，无法确定属于哪个游戏"，不猜。

## EZ-Flash 包结构（实测 342 KB / 929 条目）

```
GameID2cht.bin          3187 条 × 8 字符的定长索引：4 字符游戏码 + 4 字符编号
Eng/<bucket>/<id>.cht   928 个金手指文件
```

- 游戏码取 `rom[0xAC]` 的 4 个字符，就是 vba-over.ini 用的那个字段。
- 目录号 = `id - id % 200`（十进制补零到 4 位）。第二代编号以 `x` 开头，目录名是
  `x` + 3 位（`x454` 在 `x400/` 下）。
- 一个游戏码可能命中多个文件（区域版 + 第二代），实现按"第一个存在的文件"取值；
  包里 787 个游戏码能命中文件，另外 1982 个码只有索引没有数据——所以"库里没有这个
  游戏"是正常结果，不是 bug。
- 包也可能被解压成目录再选，路径同时支持 .zip 和目录。

## EZ-Flash 包的 .cht 文本格式

```
DeadSkullzJr's GBA Cheat Database     ← 第一行是库标题，界面上显示在底部
                                      ← 空行随意
[Infinite_Health]                     ← 段落 = 一条金手指
Switch=4563C,E7,03                    ← 键=值；键名习惯写 Switch，解析器不看键名
                                      ← 一个段落多个键时（全库只有 1 例）当作选项列表
[GameInfo]                            ← 每个文件末尾，Name/System/Text 是元数据
```

值的语义和内核一致：**逗号分隔的每个值是两个十六进制字符，各占一个字节，地址逐个
+1**；`;` 分隔不同的地址段。所以 `Switch=4563C,E7,03` 等价于在 0x0300563C 写 0xE7、
0x0300563D 写 0x03。

## 地址换算（这套格式最大的坑）

编码不是绝对地址。依据是内核 `source/GBApatch.c` 填 cheat 表的那个循环：

```c
if (pCHEAT[ii].address >= 0x40000)
    addr = (pCHEAT[ii].address & 0x7FFF)  + 0x3000000;   // IWRAM
else
    addr = (pCHEAT[ii].address & 0x3FFFF) + 0x2000000;   // EWRAM
```

真正麻烦的是：**第二种写法在这个包里几乎不存在**。9602 个短地址**全部**是 `4xxxx`，
等于"0x40000 + 原始地址的低 16 位"——原始地址的内存区和页在生成这个库的时候就被丢了。
这个库是拿网上的 Code Breaker 码转出来的，转换步骤只保留了低 16 位。

拿 libretro 的 `Castlevania - Aria of Sorrow (USA, Europe) (Code Breaker).cht` 对一下就很
清楚：原始码 `8201328C+E0FF`（Max_Experience）在这个包里写成 `4328C,FF,E0`。按内核规则
它落到 IWRAM `0x0300328C`，而真正的地址是 **`0x0201328C`**，所以这条在实机上同样是坏的。

把 30 个文件、288 个地址按低 16 位和 libretro 原始码对上之后，原始地址的分布是：

| 原始所在区 | 占比 |
|---|---|
| EWRAM `0x0202xxxx` | 44% |
| IWRAM `0x0300xxxx` | 26% |
| EWRAM `0x0200xxxx` | 15% |
| EWRAM `0x0201xxxx` | 12% |
| EWRAM `0x0203xxxx` | 3% |

也就是说"照内核读成 IWRAM"对**大约四分之三**的地址是错的。而丢掉的页**无法从文件里推
回来**：AoS 那个文件的 20 个地址里 15 个在 `0x0201` 页、5 个在 `0x0200` 页（16 条金手指
里有 3 条整条都在 `0x0200` 页），同一个文件里就不止一个页。所以这里**不做任何猜测**，
改为把选择权交出去：

解析器接受一个 `base`（`EzParseChtText(..., base)` 或 `EzCheatDb::ReloadWithBase`），
只作用于 `0x40000` 这种写法：

| base | 结果 |
|---|---|
| 0（默认） | 与内核一致：`0x03000000 \| (raw & 0x7FFF)` |
| `0x03000000` | 同上（显式选 IWRAM） |
| `0x02000000` / `0x02010000` / `0x02020000` / `0x02030000` | `base \| (raw & 0xFFFF)` |

界面上是 `IDD_CHEAT_DB` 的 **Memory area** 下拉框：默认 "As stored (IWRAM)"，选中后整张
表按新基址重解析（勾选状态保留），**Address** 列立刻显示新地址，勾哪条一目了然。文件里
没有截断地址时下拉框置灰；状态栏用 "N addresses lost their memory area" 说明有几处被截断。

只影响 `0x40000` 形式，其它地址一律不动：

- 已经写全的 `0x02xxxxxx`（3 个文件、81 处，含 Golden Sun 0170）：按绝对值处理。按内核
  规则它 ≥0x40000 会被当成 IWRAM，**在实机上其实是坏码**，这里按作者意图走，是刻意偏离。
- 小于 `0x40000` 的 EWRAM 偏移本身已经带了页，换算完就是对的，不该被 base 移动。
- 超过 `0x4FFFF` 的编码、ROM/IO 地址一律不进列表（内核会把它们静默移位到别的地址，
  这种"猜"没有意义），记为格式问题。

## 容错与报错

文件是人工写的，所以逐行容错，坏行记一条问题，能救的条目照样保留：

| 情况 | 处理 |
|---|---|
| 段名 `[` 没闭合 | 跳过该行，条目仍归到上一个有效段 |
| 行既不是段名、也没有 `=` | 第一行当库标题；其余记为问题 |
| 地址不是十六进制 / 超出可表达范围 | 跳过该地址段，记问题 |
| 值不是十六进制 | 跳过该值，记问题 |
| 值长度不是偶数（全库 1 例 `40E4E,1EF,08`） | 左侧补 0 当 1 个字（0x01EF） |
| 地址后面没有值 | 该条目丢弃，记问题 |
| 重复 / 结尾多余的 `;` | 直接容忍，不记问题 |
| 注释 `#`、`//`、`/* */` | 按内核一样跳过 |
| UTF-8 BOM | 剥掉，不粘到第一个段名上 |
| 非 ASCII 字节 | 换成 `?`（包是纯 ASCII；转换路径会把它们变成乱码，不如显式替换） |
| 条目 >256 个字、行 >1024 字符、文件 >4 MB | 截断并记问题（上限远大于真实数据） |

致命错误（弹窗，不进列表）：打不开文件、不是可读的 zip、包里没有
`GameID2cht.bin`、索引为空、某个条目解压失败。索引里 8 字符不成形的记录只计数提示。

界面底部状态栏显示"库标题 | 条数 | 被截断的地址数 | 跳过 N 行"，跳过时 **Details...**
按钮亮起，逐行列出问题。这些都是资源字符串（`IDS_CHEAT_DB_*`），解析器本身不输出任何
文案，只报 `EzProblemKind` + 行号 + 原文，文案在 `win32/Dialogs/CheatDbDlg.cpp` 里拼。

## 相邻字节合并

一个金手指常写成十几个字节，而 `cheatsNumber` 上限是 `MAX_CHEATS`（100）。
解析完会把**连续且对齐**的字节折回 16 位 / 32 位写，一条金手指通常只要 1~2 个槽位
（实测 7065 条 → 8426 次写，平均 1.19）。

**不要为了这个功能提高 `MAX_CHEATS`**：`cheatsSaveGame()` 写的是整个
`sizeof(cheatsList)`，改上限会让旧存档的金手指块和旧 `.clt` 全部读错。加不下时按现有
做法提示 "The cheat list is full."。

## libretro-database 的 .cht

`libretro-database/cht/Nintendo - Game Boy Advance/*.cht`，一个文件一个游戏：

```
cheats = 2

cheat0_desc = "Max Experience"
cheat0_code = "8201328C+E0FF+8201328E+05F5"
cheat0_enable = false
```

- 文件名就是游戏名（`Castlevania - Aria of Sorrow (USA, Europe) (Code Breaker).cht`），
  对话框标题和状态栏用的是它。文件里既没有标题行也没有游戏码，**没法按 ROM 自动匹配**，
  用户在文件对话框里自己挑（过滤器里已经加了 `*.cht`）。
- `cheatN_enable` 是作者的默认开关，导入时忽略：勾不勾由用户决定。
- 代码串是 CodeBreaker / GameShark 码，**地址是绝对值**，所以内存区下拉框对它们无意义
  （没有截断地址时它本来就会置灰）。

### 代码串怎么切

`common/cheatcodetext.cpp` 的 `CheatSplitCodes()` 负责切分，**Add 对话框和导入共用同一
份规则**，所以两边不会漂移：

| 写法 | 结果 | 交给 |
|---|---|---|
| `8201328C+E0FF` / `8201328C E0FF` | 8 位地址 + 4 位值 | `cheatsAddCBACode` |
| `33003DDF00FF`（12 位连写） | 同上，第 8 位后补空格 | `cheatsAddCBACode` |
| `C40F4772 00008401` / `000093DD000A0007` | 8 + 8 | `cheatsAddGSACode(..., v3)` |
| 落单的 8 位 token | 当成 CodeBreaker 候选 | 校验不过 → 记为问题 |

分隔符是空格、制表、换行或 `+`——数据库里三种都有人用。**含义由类型半字节决定**
（0x03 八位写、0x08 十六位写、0x07/0x0D 条件、0x01 主码、0x05 超级码……），只有引擎
知道哪些能执行，所以解析器不翻译，切完交给 `cheatsValidate*Code` / `cheatsAdd*Code`。
条件码（按键触发、if-true 之类）和 GameShark v3 的主码因此和"Add
GameShark/CodeBreaker"里粘贴的行为完全一致，不会退化成普通写入。

### 容错（实测 94 个文件 / 1100 条 / 1788 个代码候选）

**98.8%（1766）通过引擎校验**，其余 22 个是数据库自己的笔误：`00??`、`XXXX` 占位符、
`302001453` 这种 9 位地址。处理方式：

- 一个条目里的坏码单独丢弃，好码照常加入；整条一个都不剩时不列出，记一条问题
  （"第 N 条：没有本模拟器能用的代码"），**Details...** 里能看到原文。
- **引号值不跨行**：真实文件里 Final Fantasy VI 那 10 条长码少了收尾引号，读到行尾就停；
  继续往后读会把下面的 `cheatN_desc` 一起吞掉，导致 17~26 条名字全空。
- `cheats = 27cheat0_desc = ...` 的粘连、`cheat0_enable = falsecheat1_desc = ...` 的粘连
  都能处理：扫描按 `cheatN_` 键做，不依赖换行。
- `cheatN_desc` 有、`cheatN_code` 没有 → 记一条问题（实测 1 例）；反过来名字为空则按
  空名列出（实测 0 例）。


## 文件分工

| 文件 | 职责 |
|---|---|
| `src/common/cheatdb.h` | 两个读取器共用的模型：`CheatDbFile/Entry/Write/Problem`、错误码、`CheatDbFormat` |
| `src/common/cheatdb.cpp` | 按内容判断格式（`CheatDbSniffFormat`）、读小文本文件（`CheatDbReadTextFile`） |
| `src/common/ezflashdb.{h,cpp}` | 打开 EZ-Flash 包、查索引、解压、解析、换算、合并。不依赖 MFC，也不碰模拟器状态 |
| `src/common/libretrocht.{h,cpp}` | 读 libretro 的 .cht：按 `cheatN_` 键扫描、保留代码文本 |
| `src/common/cheatcodetext.{h,cpp}` | 把代码串切成引擎的两种形式，Add 对话框和导入共用 |
| `src/common/cheatdb_test.cpp` | 离线测试程序（**不进工程**），见下 |
| `src/win32/Dialogs/CheatDbDlg.{h,cpp}` | 选文件、判断格式、把结果变成对话框或消息、把勾中的写进列表 |
| 资源 | `IDD_CHEAT_DB`、`IDC_CHEAT_DB_AREA`、`IDC_LOAD_DATABASE`、`ID_CHEATS_LOADFROMDB`、`IDS_CHEAT_DB_*` |
| 设置 | `theApp.cheatDbPath`，存在 `cheatDbPath`（注册表 / vba.ini 同一套） |

## 离线测试

`cheatdb_test.cpp` 有 `main()`，所以不进工程。改了解析规则就手工编一次：

```
cl /EHsc /MT /DWIN32 /DHAVE_STDINT_H /I src\common /I win32\lib\zlib ^
   src\common\ezflashdb.cpp src\common\ziparchive.cpp src\common\libretrocht.cpp ^
   src\common\cheatcodetext.cpp src\common\cheatdb.cpp src\common\cheatdb_test.cpp ^
   win32\lib\zlib\{adler32,crc32,inflate,inftrees,inffast,zutil}.c
cheatdb_test.exe "路径\EZ-FLASH OMEGA.zip" --libretro "路径\libretro-database\cht\Nintendo - Game Boy Advance"
```

`--libretro` 可选（不给就跳过那部分），`--dump` 输出每个记录的条数。

它先跑构造用例（包：地址两种写法、内存区基址、坏值、未闭合段名、BOM、注释、重复地址、
超限；libretro：粘连的键、缺引号的值、8+8 与 12 位连写、空文件、不存在的路径；切分器：
上下大小写、落单 token；格式判断：两边各自的样本、BOM、粘连键、缺 `[GameInfo]`、
括号写在值里、大小写与三种换行、两种标记都有的混合文件），再遍历真实包的 3187 条记录，
把总数和一份独立的 Python 实现对数：**875 个游戏有金手指、7065 条条目、8426 次写
（8 位 4636 / 16 位 2880 / 32 位 910）、9602 个地址被截断、0 条格式问题**；再用
`ReloadWithBase` 重读一个文件，断言只有低 15 位和值不变、所有地址都落进选定的页。

格式判断另有两个全量断言：包内 **928 个 .cht 全部判成包格式、0 个像 libretro**，其中
875 个"当单文件读"能出条目（正是用户直接选一个包里文件时走的路径）；给定 `--libretro`
目录时，**94 个真实文件全部判成 libretro**。

改包或改规则后这些数字会变，改完要一起改断言（现在共 158 条）。

libretro 的切分规则另有一份一次性核对：用 94 个真实 .cht 跑 `lrsplit_probe`（临时程序，
不入库）和 Python 参考实现对比**每条的 desc / 代码条数 / 问题数**，94/94 完全一致；候选里
1766/1788 能过引擎校验。

## 已知取舍

- **地址的内存区在包里就丢了**（见上）。默认照内核读成 IWRAM，多数游戏需要在上拉框里
  换一个区；换哪个由用户决定，因为一个文件里可能同时存在多个页。这是这个格式的缺陷，
  不是解析器能修好的东西。
- libretro 的每条代码在列表里占一个槽位（和 Add 对话框一致），所以"全角色满属性"那种
  16~19 个代码的条目一次就吃掉 16~19 个 `MAX_CHEATS`（上限 100）。勾之前先看 Address
  列的 `+N`。
- libretro 的 GameShark v3 代码需要同一个文件里带 "(M) Must Be On" 主码，勾的时候要一起
  勾；导入按列表顺序写入，主码在前，`cheatsAddGSACode` 会自己记住它。
- 一个 `.cht` 对应一个游戏，不能选整个目录：libretro-database 是几百个文件的目录树，
  **没有**按 ROM 名自动找到对应文件的功能（文件名和 ROM 名、包内标题都不完全一致，猜错
  比让用户挑一份更糟）。把一堆 `.cht` 打成一个 zip 也一样——没有 `GameID2cht.bin` 又有
  多个 `.cht` 时直接报错说清楚，不替用户挑。
- 与内核一致地"按位与"会让 `< 0x40000` 的地址落到 EWRAM 镜像上；这类地址在本包里没有。
- 键名不参与识别，`Switch` 只是习惯写法。段落里多个键时界面显示"段名: 键名"。
- 条目名截到 31 字节（`CheatsData::desc` 只有 32），全库有 336 条名字超过 31 字符，
  列表里靠 tooltip 看全名。
- 只支持 GBA（包里全是 `System=GBA`）；GB 模式菜单置灰。
- 只认 stored / deflate 两种压缩（`ZipArchive` 的范围），加密包会明确报错。
- 导入的金手指默认是启用的，和 Add Code 的行为一致。
