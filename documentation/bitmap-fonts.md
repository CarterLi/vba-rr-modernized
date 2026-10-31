# 位图字体：一个容器，一条渲染路径

> 结论先行：`font8x8` 与 DeSmuME 的 `gse*`/`mcs*`/`verdana*` 现在都是**同一个容器**
> （`src/common/TextFont.h`）里的数据，都来自 EXE 的 RCDATA 资源，都由 `common/Text.cpp`
> 一个渲染器画。`src/win32/BitmapFont.cpp` 只负责「资源字节 → `TextFont`」这一步。

## 1. 容器格式

沿用 AGG 2.5 的嵌入点阵字体格式（`agg_embedded_raster_fonts.cpp` 里那些字节的原样）：

```
[0] 高度（行数）
[1] 基线（本项目的字体全是 0，不使用）
[2] 首字符码
[3] 字形数，**0 表示 256**
[4 .. 4 + 字形数*2)  每字形一个 uint16 偏移（小端），相对 [4 + 字形数*2]
之后每字形：宽度字节 + 高度行 × ceil(宽度/8) 字节
```

行内**最左像素在最高位（MSB first）**，画笔推进量就是宽度字节 —— 与 AGG 的
`glyph_raster_bin` 一致（`r->dx = m_glyph_width`）。

两条与 AGG 不同的地方，都是为了把内置 8x8 装进来：

* **字形数是 0 表示 256。** AGG 的该字段只有一个字节，装不下内置字体的 256 个码位；
  而一个字形数为 0 的字体本来就没有意义，所以这个映射不会有歧义。gse/mcs/verdana 都是 96，
  永远走不到这条。
* 内置字体是**转换**来的，不是原样拷贝，见下一节。

解码只在 `TextFont.cpp`，并且 `textFontInit()` 会把每个字形都对着 blob 长度校验一遍：
越界、宽度为 0、高度离谱的字体直接判为不可用，不让它画出垃圾。

## 2. 内置 8x8 原来的样子与转换

原来 `fontdata2[2048]` 是「裸表」：256 字形 × 8 字节，**一个字节一行，bit0 = 最左像素**，
无头部无偏移表。转换只做两件事：

1. 每行字节做位反转（LSB first → MSB first）；
2. 补上头部与偏移表（每字形宽度恒为 8，所以推进量仍是 8）。

位图本身一个像素都没动。转换脚本是顺手的一次性 PowerShell（`BitConverter` + 位反转），
回读校验过「解出来的位图 == 原表」。故 **OSD 在 100% 下的字形与历史完全一样**。

## 3. 资源与加载

`vba.rc` 里 35 行 `RCDATA`，都指向 `resources/font/*.bin`：

* `FONT_8X8` —— 内置 8x8，`VBA::InitInstance()` 里通过 `winBitmapFontInit()` 直接交给
  `common/Text.cpp`（`textSetBuiltinFont()`）；
* `FONT_GSE4X6` … `FONT_VERDANA18_BOLD` —— DeSmuME `fonts_list` 的 34 个字体，顺序与
  命名都照抄，名字（`gse7x11`）同时是设置文件里存的值。

**一定要用 `FindResource(NULL, …)`**：字体在 EXE 里，而 `AfxGetResourceHandle()` 在加载了
语言模块之后指向 DLL，翻译版会在那里找不到字体。与 upscaler 着色器、7z.dll 的做法一致。

资源缺失只写 trace（`winlog`），那个字体从下拉框里消失，程序照常跑。

## 4. 渲染

`common/Text.cpp` 只有一条路径：查字形 → 按百分比缩放成 `glyph + 描边` 的单元 → 按位深落色。
原来的 16/24/32bpp 三段重复循环和「100% 专用路径」都删了。

需要记住的规则：

* **颜色**：16bpp 的透明混合里，字形用 `loCol`、描边用 `outCol`；24/32bpp 字形用 `hiCol`。
  这是历史行为，被三个后端共用，别顺手「统一」掉。
* **描边**：`t = (percent + 50) / 100`，最少 1px；100% 时是 1px，200% 时是 2px。
* **行高**：`drawTextLineHeight()` = 字体高度 × 百分比 + 2t，内置字体在 100% 下仍是 10 行。
* **缩放**：最近邻。像素字体不该出现插值。

### ⚠️ 100% 时整体左移/上移了 1 像素

旧的 100% 专用路径把 10×10 单元画在 `(x, y)` 而不是 `(x-1, y-1)`，于是字形实际落在
`(x+1, y+1)` —— 和其它所有百分比（走的是缩放路径，字形落在 `(x, y)`）差 1 像素。
统一之后只能选一个：现在**所有百分比都在 `(x, y)`**，与缩放路径一致。
对拍探针证明：新版与旧版的**缩放路径**在 16/24/32bpp × 25/50/100/150/200% × 4 色调色板 ×
透明/不透明的全部组合下**逐字节相同**；与旧版 100% 专用路径的差异只有这 1 像素平移。

## 5. In Game / On Game 的字体列表

选手框（`IDC_TEXT_FONT`）的顺序是：`Built-in` → 内嵌**家族** `gse` / `mcs` / `verdana`
→ 系统装了的 raster face。字号下拉框（`IDC_TEXT_SIZE`）跟着家族走：

| 来源 | 字号项 | 说明 |
|---|---|---|
| 内嵌家族 | `4x6`、`4x8`、`5x7` … `8x16_bold` | 字体名去掉家族前缀；`gse` 12 档、`mcs` 10 档、`verdana` 12 档 |
| 系统 raster face | `9x20` 之类 | `tmAveCharWidth` × `tmHeight`，同一 face 有几档 strike |
| Built-in | `8x8` | 只有原生一档 |

`GameTextStrike` 带两个新字段：`label`（上面那一列，内嵌字体直接取表里的字符串）与
`font`（这一档对应的完整字体名，如 `gse7x11_bold`；系统 face 为空）。所以：

* 字号标签不再用 `_ttoi` 反解（`4x6` 会解析成 4），选择走 `m_sizeList` 的下标；
* 打开对话框时用 `familyForFont()` 把 `gse7x11` 还原成家族 `gse`，再用名字精确
  选回那一档（`gse7x11` 与 `gse7x11_bold` 高度相同，只能靠名字区分，不能靠「最接近的高度」）。

字号标签是**名字里的数字**，不是量出来的：`verdana18_bold` 显示 `18_bold`，而它最宽的字形
是 19 px。名字是 DeSmuME 的命名，照抄比现算更好认。

### 设置文件里的写法

`gameTextFont` 与 `gameTextSize` **都是字符串**，写的就是对话框里那两个框的内容：

```
gameTextFont=gse
gameTextSize=7x11
```

| 选中的是 | `gameTextFont` | `gameTextSize` |
|---|---|---|
| 内嵌家族某一档 | `gse` | `7x11` / `8x16_bold` … |
| 内置字体 | `Built-in` | `8x8` |
| 系统 raster face | `Terminal` 之类 | **字符高度**（`CreateFont` 的 `lfHeight` = `tmHeight` − `tmInternalLeading`），如 `20` |

理由：内嵌字体的大小**本身就写在名字里**（`gse7x11`），所以家族 + 尺寸能无损还原，
`7x11` 与 `7x11_bold` 也不会混。系统 face 没有这种名字，它唯一的「字号」就是喂给
`CreateFont` 的那个高度，所以直接存数字（手写成 `9x20` 也能读，取 `x` 后面的数）。

运行时 `theApp.gameTextFont` 仍放**渲染器认的完整名**（`gse7x11`），`theApp.gameTextSize`
仍是 int；转换只在 `VBA::loadSettings()` / `saveSettings()` 里做一次
（`winBitmapFontBySize()` / `winBitmapFontSplit()`）。**不考虑兼容旧配置**：旧版存的
`gameTextFont=gse7x11` + dword 尺寸读不出来，会退回 `Built-in`。

内嵌字体走 `GameText.cpp` 的 `GlyphCache::configureBitmap()`：字形从 `TextFont` 直接取，
加上 1px 边距与描边，之后的 `blitGlyph()` 与系统字体那条路完全共用。描边宽度按**字体自身
高度**算（`scaledOutline(font->height)`），不跟着设置里可能过期的 `gameTextSize` 走 ——
边距也是从缓存问（`cache.margin()`），避免两边算出不同值。

## 6. 加一个字体

1. 把字节放进 `src/win32/resources/font/`；
2. `vba.rc` 加一行 `RCDATA`；
3. `src/win32/BitmapFont.cpp` 的表里加一行：**完整名、家族、字号标签、资源名**。家族写
   已有的（`gse`）就并进那个家族、只在字号下拉框里多一项；写新的就多一个家族。

高度与字形宽度都从字体自己读，不用另外登记。

同一家族的多个字号**不能压进一个 RCDATA**：AGG 容器的头里只有**一个** `height` 字节，
所有字形共用，两个字高放不进一个 blob（见 `common/TextFont.h`）。想少几个资源只能在外面再
套一层目录（`.FON` 装多个 `.FNT` 那种），现在的取舍是「一个字号一个资源，文件与上游逐字节
相同、能直接比对」，多包一层就丢了这一点。

## 7. 来源与许可

`gse*`、`mcs*`、`verdana*` 取自 AGG 2.5（Anti-Grain Geometry，Copyright (C) 2002-2006
Maxim Shemanarev）的 `agg_embedded_raster_fonts.cpp`，字节原样。AGG 是 GPL 2+，与本项目
一致，可以一起分发。字体清单与命名来自 DeSmuME 的 HUD（`aggdraw.cpp` 的 `fonts_list`）。
细节在 `src/win32/resources/font/README.txt`。
