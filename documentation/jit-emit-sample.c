/* =============================================================================
 *  V7 的 x64 后端到底生成了什么代码——一个能编译、能跑的样本
 * =============================================================================
 *
 *  这个文件不是工程的一部分，是给人看的。它把 mario.gba 里一个真实的 5 条
 *  Thumb 指令的块（0x080B66E2）从"guest 指令"一路翻译到"C 语言写的宿主
 *  代码"，中间那 597 字节机器码是 -jit 真跑出来的（VBA_JIT_ASM=jit-asm.txt），
 *  不是手写的。
 *
 *  编译：
 *      cl /nologo /W4 /O2 jit-emit-sample.c && jit-emit-sample.exe
 *
 *  读法：
 *      · 每个以 "@asm" 开头的注释里，十六进制就是那段机器码的真实字节，
 *        后面是它反汇编出来的助记符；再下面是"同一件事用 C 写一遍"。
 *      · 机器码里所有模拟器状态都藏在内存里，靠 rbx = &reg[0] 加固定偏移
 *        寻址，所以 C 里也照抄成 rbx[7]、N_FLAG 这些。
 *
 *  guest 块（Thumb，0x080B66E2）：
 *
 *      080B66E2  1C38   adds  r0, r7, #0
 *      080B66E4  3008   adds  r0, r0, #8
 *      080B66E6  7801   ldrb  r1, [r0]
 *      080B66E8  2905   cmp   r1, #5
 *      080B66EA  D900   bls   080B66EE      (LS = C==0 || Z==1)
 *
 *  五条指令，597 字节机器码，平均一条 guest 指令 119 字节。
 * ========================================================================== */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------------
 * 1. 模拟器状态
 *
 *    真正的工程里这些是全局变量（GBAGlobals.h），机器码把它们当作
 *    [rbx + disp32] 直接寻址：rbx 固定是 &reg[0]，disp32 在编译期由真实
 *    链接地址算出来（checkedDisplacements() 开机检查过都在 ±2 GB 内）。
 *
 *    注意下面标志位的顺序：源码里声明的是 N,C,Z,V，但 /LTCG 会重排全局量，
 *    实测链接出来是 N,V,C,Z，所以机器码里的偏移看着和声明顺序对不上——
 *    这就是为什么生成代码里必须写 [rbx-0x6E354] 这种鬼东西。
 * -------------------------------------------------------------------------- */

static uint32_t reg[16];                        /* r0..r15 */
static uint8_t  N_FLAG, V_FLAG, C_FLAG, Z_FLAG; /* 一个标志一个字节，各自独立 */
static uint8_t  armState;
static uint8_t  extMem[0x100];                  /* 假装是 EWRAM */

static const int32_t memoryWaitFetch[16] = {1, 1, 3, 1, 1, 1, 1, 1, 3, 2, 4, 2, 1, 1, 1, 1};
static const int32_t memoryWait32[16]    = {1, 1, 3, 1, 1, 1, 1, 1, 3, 2, 4, 2, 1, 1, 1, 1};

/* CPUReadByte / CPUReadMemory 是 V7 的访存 helper。机器码里它们是间接调用：
 *   48 B8 <addr64>   movabs rax, CPUReadByte
 *   FF D0            call rax
 * 之所以不内联，是因为一次读可能落在 IO / 存档 / DMA 上，必须走同一份代码。 */
static uint32_t CPUReadByte(uint32_t address)
{
	return extMem[address & 0xFF];
}

/* 出口协议，抄自 src/gba/jit/JitEmit.h */
enum
{
	JITX_NEED_INTERP = 1,
	JITX_KEEP_PC     = 2,
	JITX_BUDGET      = 4,
	JITX_DONE        = 8
};

struct JitExecResult
{
	uint32_t nextPc;
	uint32_t status;
};

/* -----------------------------------------------------------------------------
 * 2. 真机器码：-jit 跑 mario.gba 时 VBA_JIT_ASM 抓下来的 597 字节
 *
 *    头 33 字节是 prologue，尾 68 字节是 4 个"预算用完"的出口桩，中间 478
 *    字节分给 5 条 guest 指令。
 * -------------------------------------------------------------------------- */

static const unsigned char code[597] = {
	0x53, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x55, 0x57, 0x56, 0x48, 0x83,
	0xEC, 0x20, 0x48, 0xBB, 0xF0, 0x75, 0xED, 0x8F, 0xF7, 0x7F, 0x00, 0x00,
	0x41, 0x89, 0xCD, 0x49, 0x89, 0xD6, 0x45, 0x31, 0xE4, 0x41, 0xBA, 0x00,
	0x00, 0x00, 0x00, 0x44, 0x8B, 0x4B, 0x1C, 0x45, 0x89, 0xC8, 0x45, 0x01,
	0xD0, 0x44, 0x89, 0x03, 0x45, 0x85, 0xC0, 0x0F, 0x98, 0xC0, 0x88, 0x83,
	0xAC, 0x1C, 0xF9, 0xFF, 0x0F, 0x94, 0xC0, 0x88, 0x83, 0xAF, 0x1C, 0xF9,
	0xFF, 0x44, 0x89, 0xC9, 0x44, 0x31, 0xC1, 0x44, 0x89, 0xD2, 0x44, 0x31,
	0xC2, 0x21, 0xD1, 0xC1, 0xE9, 0x1F, 0x88, 0x8B, 0xAD, 0x1C, 0xF9, 0xFF,
	0x44, 0x89, 0xC9, 0x44, 0x89, 0xD2, 0x48, 0x01, 0xD1, 0x48, 0xC1, 0xE9,
	0x20, 0x88, 0x8B, 0xAE, 0x1C, 0xF9, 0xFF, 0x41, 0x83, 0xC4, 0x01, 0x44,
	0x03, 0xA3, 0xD0, 0xE0, 0xF2, 0xFF, 0x45, 0x39, 0xEC, 0x0F, 0x8D, 0x8A,
	0x01, 0x00, 0x00, 0x41, 0xBA, 0x08, 0x00, 0x00, 0x00, 0x44, 0x8B, 0x0B,
	0x45, 0x89, 0xC8, 0x45, 0x01, 0xD0, 0x44, 0x89, 0x03, 0x45, 0x85, 0xC0,
	0x0F, 0x98, 0xC0, 0x88, 0x83, 0xAC, 0x1C, 0xF9, 0xFF, 0x0F, 0x94, 0xC0,
	0x88, 0x83, 0xAF, 0x1C, 0xF9, 0xFF, 0x44, 0x89, 0xC9, 0x44, 0x31, 0xC1,
	0x44, 0x89, 0xD2, 0x44, 0x31, 0xC2, 0x21, 0xD1, 0xC1, 0xE9, 0x1F, 0x88,
	0x8B, 0xAD, 0x1C, 0xF9, 0xFF, 0x44, 0x89, 0xC9, 0x44, 0x89, 0xD2, 0x48,
	0x01, 0xD1, 0x48, 0xC1, 0xE9, 0x20, 0x88, 0x8B, 0xAE, 0x1C, 0xF9, 0xFF,
	0x41, 0x83, 0xC4, 0x01, 0x44, 0x03, 0xA3, 0xD0, 0xE0, 0xF2, 0xFF, 0x45,
	0x39, 0xEC, 0x0F, 0x8D, 0x36, 0x01, 0x00, 0x00, 0xC7, 0x43, 0x3C, 0xEA,
	0x66, 0x0B, 0x08, 0x8B, 0x3B, 0x89, 0xFE, 0x89, 0xF7, 0x89, 0xF9, 0x48,
	0xB8, 0x30, 0xE7, 0x9B, 0x8F, 0xF7, 0x7F, 0x00, 0x00, 0xFF, 0xD0, 0x0F,
	0xB6, 0xC0, 0x89, 0x43, 0x04, 0x41, 0x83, 0xC4, 0x03, 0x44, 0x03, 0xA3,
	0xD0, 0xE0, 0xF2, 0xFF, 0x41, 0x89, 0xF8, 0x41, 0xC1, 0xE8, 0x18, 0x41,
	0x83, 0xE0, 0x0F, 0x46, 0x03, 0xA4, 0x83, 0x30, 0xE1, 0xF2, 0xFF, 0x45,
	0x39, 0xEC, 0x0F, 0x8D, 0xFF, 0x00, 0x00, 0x00, 0x41, 0xBA, 0x05, 0x00,
	0x00, 0x00, 0x44, 0x8B, 0x4B, 0x04, 0x45, 0x89, 0xC8, 0x45, 0x29, 0xD0,
	0x45, 0x85, 0xC0, 0x0F, 0x98, 0xC0, 0x88, 0x83, 0xAC, 0x1C, 0xF9, 0xFF,
	0x0F, 0x94, 0xC0, 0x88, 0x83, 0xAF, 0x1C, 0xF9, 0xFF, 0x44, 0x89, 0xC9,
	0x44, 0x31, 0xD1, 0x44, 0x89, 0xCA, 0x44, 0x31, 0xC2, 0x21, 0xD1, 0xC1,
	0xE9, 0x1F, 0x88, 0x8B, 0xAD, 0x1C, 0xF9, 0xFF, 0x44, 0x89, 0xC9, 0x44,
	0x89, 0xD2, 0x48, 0x29, 0xD1, 0x48, 0xC1, 0xE9, 0x3F, 0x83, 0xF1, 0x01,
	0x88, 0x8B, 0xAE, 0x1C, 0xF9, 0xFF, 0x41, 0x83, 0xC4, 0x01, 0x44, 0x03,
	0xA3, 0xD0, 0xE0, 0xF2, 0xFF, 0x45, 0x39, 0xEC, 0x0F, 0x8D, 0xAA, 0x00,
	0x00, 0x00, 0x80, 0xBB, 0xAE, 0x1C, 0xF9, 0xFF, 0x00, 0x0F, 0x84, 0x0D,
	0x00, 0x00, 0x00, 0x80, 0xBB, 0xAF, 0x1C, 0xF9, 0xFF, 0x00, 0x0F, 0x84,
	0x18, 0x00, 0x00, 0x00, 0x41, 0x83, 0xC4, 0x03, 0x41, 0xC7, 0x06, 0xEE,
	0x66, 0x0B, 0x08, 0x41, 0xC7, 0x46, 0x04, 0x08, 0x00, 0x00, 0x00, 0xE9,
	0x33, 0x00, 0x00, 0x00, 0x41, 0x83, 0xC4, 0x01, 0x44, 0x03, 0xA3, 0xD0,
	0xE0, 0xF2, 0xFF, 0x41, 0xC7, 0x06, 0xEC, 0x66, 0x0B, 0x08, 0x41, 0xC7,
	0x46, 0x04, 0x08, 0x00, 0x00, 0x00, 0xE9, 0x14, 0x00, 0x00, 0x00, 0x41,
	0xC7, 0x06, 0xEC, 0x66, 0x0B, 0x08, 0x41, 0xC7, 0x46, 0x04, 0x08, 0x00,
	0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x44, 0x89, 0xE0, 0x48, 0x83,
	0xC4, 0x20, 0x5E, 0x5F, 0x5D, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5B,
	0xC3, 0x41, 0xC7, 0x06, 0xE4, 0x66, 0x0B, 0x08, 0x41, 0xC7, 0x46, 0x04,
	0x04, 0x00, 0x00, 0x00, 0xEB, 0xDD, 0x41, 0xC7, 0x06, 0xE6, 0x66, 0x0B,
	0x08, 0x41, 0xC7, 0x46, 0x04, 0x04, 0x00, 0x00, 0x00, 0xEB, 0xCC, 0x41,
	0xC7, 0x06, 0xE8, 0x66, 0x0B, 0x08, 0x41, 0xC7, 0x46, 0x04, 0x04, 0x00,
	0x00, 0x00, 0xEB, 0xBB, 0x41, 0xC7, 0x06, 0xEA, 0x66, 0x0B, 0x08, 0x41,
	0xC7, 0x46, 0x04, 0x04, 0x00, 0x00, 0x00, 0xEB, 0xAA,
};

/* 上面这 597 字节按功能切一下（偏移是数出来的，不是猜的） */
struct Region
{
	const char *what;
	int         from;
	int         bytes;
};

static const struct Region regions[] = {
	{"prologue（保存 7 个寄存器 + 装 rbx/r13/r14/r12）", 0, 33},
	{"adds r0, r7, #0", 33, 102},
	{"adds r0, r0, #8", 135, 101},
	{"ldrb r1, [r0]", 236, 72},
	{"cmp  r1, #5", 308, 102},
	{"bls  080B66EE", 410, 101},
	{"epilogue（返回周期数 + 恢复寄存器）", 511, 18},
	{"4 个\"预算用完\"出口桩（共用，17 字节一个）", 529, 68},
};

/* -----------------------------------------------------------------------------
 * 3. 生成代码的 C 版本
 *
 *    一行 C 对应一条 x64。入口协议（参数在 ecx 和 rdx 里）：
 *
 *        int fn(int32_t budget, struct JitExecResult *out)
 *
 *    返回这次执行花了多少周期。
 *
 *    先看 prologue。7 个 push 不是给 callee 用的——模拟器全局量在内存里，
 *    这 7 个是宿主 ABI 要求保存的 callee-saved 寄存器，因为生成代码把它们
 *    全占了：rbx=状态基址、r12=周期累计、r13=预算、r14=出口结构体指针。
 *
 *        53                push rbx
 *        41 54             push r12
 *        41 55             push r13
 *        41 56             push r14
 *        55                push rbp
 *        57                push rdi
 *        56                push rsi
 *        48 83 EC 20       sub  rsp, 0x20     ; 影子空间 + 对齐
 * -------------------------------------------------------------------------- */

static int32_t jit_block_080B66E2(int32_t budget /* ecx */, struct JitExecResult *out /* rdx */)
{
	uint32_t *rbx = reg;	/* 48 BB F0 75 ED 8F F7 7F 00 00  movabs rbx, &reg[0] */
	int32_t   r12d = 0;		/* 45 31 E4                       xor    r12d, r12d */
	int32_t   r13d = budget;/* 41 89 CD                       mov    r13d, ecx   */
	struct JitExecResult *r14 = out; /* 49 89 D6              mov    r14, rdx   */

	uint32_t r8d, r9d, r10d;	/* 临时寄存器，每条指令重新赋值 */

	/* ===== 080B66E2  adds r0, r7, #0 ====================================
	 *
	 * 一条"把 r7 加 0"，在 arm-new.h 里是
	 *      reg[0].d = reg[7].d + 0;
	 *      if (S) setAddFlags(...);
	 * 生成代码把它展开成 22 条 x64 —— 其中 14 条都在算四个标志位。
	 */

	/* 41 BA 00 00 00 00        mov  r10d, 0                 ; 立即数 */
	r10d = 0;
	/* 44 8B 4B 1C              mov  r9d, [rbx+0x1C]         ; reg[7] */
	r9d = rbx[7];
	/* 45 89 C8                 mov  r8d, r9d */
	r8d = r9d;
	/* 45 01 D0                 add  r8d, r10d */
	r8d = r8d + r10d;
	/* 44 89 03                 mov  [rbx], r8d              ; reg[0] = 结果 */
	rbx[0] = r8d;

	/* 45 85 C0                 test r8d, r8d                ; N：符号位 */
	/* 0F 98 C0                 sets al */
	/* 88 83 AC 1C F9 FF        mov  [rbx-0x6E354], al       ; N_FLAG */
	N_FLAG = (r8d >> 31) & 1;
	/* 0F 94 C0                 sete al                      ; Z：结果为零 */
	/* 88 83 AF 1C F9 FF        mov  [rbx-0x6E351], al       ; Z_FLAG */
	Z_FLAG = (r8d == 0);
	/* 44 89 C9                 mov  ecx, r9d                ; V：符号位变化 */
	/* 44 31 C1                 xor  ecx, r8d */
	/* 44 89 D2                 mov  edx, r10d */
	/* 44 31 C2                 xor  edx, r8d */
	/* 21 D1                    and  ecx, edx */
	/* C1 E9 1F                 shr  ecx, 31 */
	/* 88 8B AD 1C F9 FF        mov  [rbx-0x6E353], cl       ; V_FLAG */
	V_FLAG = (((r9d ^ r8d) & (r10d ^ r8d)) >> 31) & 1;
	/* 44 89 C9                 mov  ecx, r9d                ; C：第 33 位 */
	/* 44 89 D2                 mov  edx, r10d */
	/* 48 01 D1                 add  rcx, rdx                ; 64 位相加！ */
	/* 48 C1 E9 20              shr  rcx, 32 */
	/* 88 8B AE 1C F9 FF        mov  [rbx-0x6E352], cl       ; C_FLAG */
	C_FLAG = (uint8_t)(((uint64_t)r9d + (uint64_t)r10d) >> 32);

	/* 41 83 C4 01              add  r12d, 1                 ; clockTicks = 1 */
	/* 44 03 A3 D0 E0 F2 FF     add  r12d, [rbx-0xD1F30]     ; + memoryWaitFetch[页] */
	r12d += 1 + memoryWaitFetch[0x08];
	/* 45 39 EC                 cmp  r12d, r13d              ; 预算检查，每条指令一次 */
	/* 0F 8D 8A 01 00 00        jge  桩 0x080B66E4 */
	if (r12d >= r13d)
	{
		r14->nextPc = 0x080B66E4u;	/* 桩：mov [r14], 0x080B66E4 */
		r14->status = JITX_BUDGET;	/*     mov dword [r14+4], 4  */
		goto epilogue;			/*     jmp epilogue          */
	}

	/* ===== 080B66E4  adds r0, r0, #8 ====================================
	 *
	 * 和上一条一模一样，只是操作数换成 [rbx] 和 8。101 字节。
	 */

	/* 41 BA 08 00 00 00        mov  r10d, 8 */
	r10d = 8;
	/* 44 8B 0B                 mov  r9d, [rbx]              ; reg[0] */
	r9d = rbx[0];
	/* 45 89 C8                 mov  r8d, r9d */
	r8d = r9d;
	/* 45 01 D0                 add  r8d, r10d */
	r8d = r8d + r10d;
	/* 44 89 03                 mov  [rbx], r8d */
	rbx[0] = r8d;
	/* ...标志位同上（18 字节的 sets/sete/and/shr 序列）... */
	N_FLAG = (r8d >> 31) & 1;
	Z_FLAG = (r8d == 0);
	V_FLAG = (((r9d ^ r8d) & (r10d ^ r8d)) >> 31) & 1;
	C_FLAG = (uint8_t)(((uint64_t)r9d + (uint64_t)r10d) >> 32);
	/* 41 83 C4 01              add  r12d, 1 */
	/* 44 03 A3 D0 E0 F2 FF     add  r12d, [rbx-0xD1F30] */
	r12d += 1 + memoryWaitFetch[0x08];
	/* 45 39 EC / 0F 8D 36 01 00 00   jge 桩 0x080B66E6 */
	if (r12d >= r13d)
	{
		r14->nextPc = 0x080B66E6u;
		r14->status = JITX_BUDGET;
		goto epilogue;
	}

	/* ===== 080B66E6  ldrb r1, [r0] ======================================
	 *
	 * 访存必须调 helper，所以这里先写 reg[15]——CPUReadByte 会读它算 open
	 * bus。这就是 3.2.1.14 说的"每条指令都要付 reg[15] 的存储"。
	 */

	/* C7 43 3C EA 66 0B 08     mov  dword [rbx+0x3C], 0x080B66EA   ; reg[15] = pc+4 */
	rbx[15] = 0x080B66EAu;
	/* 8B 3B                    mov  edi, [rbx]              ; 地址 = r0 */
	/* 89 FE                    mov  esi, edi                 ; （下面两条是空的搬运，
	 * 89 F7                    mov  edi, esi                 发射器留下的，没被消掉） */
	/* 89 F9                    mov  ecx, edi                 ; 第 1 个参数 */
	/* 48 B8 30 E7 9B 8F F7 7F 00 00   movabs rax, CPUReadByte
	 * FF D0                    call rax */
	/* 0F B6 C0                 movzx eax, al                 ; 只留低 8 位 */
	/* 89 43 04                 mov  [rbx+4], eax             ; reg[1] */
	rbx[1] = CPUReadByte(rbx[0]) & 0xFFu;
	/* 41 83 C4 03              add  r12d, 3                  ; 这次访存的固定开销 */
	/* 44 03 A3 D0 E0 F2 FF     add  r12d, [rbx-0xD1F30] */
	r12d += 3 + memoryWaitFetch[0x08];
	/* 41 89 F8 / 41 C1 E8 18 / 41 83 E0 0F
	 *                          页号 = (地址 >> 24) & 0x0F */
	/* 46 03 A4 83 30 E1 F2 FF add  r12d, [rbx+r8*4-0xD1DC8]  ; + memoryWait32[页] */
	r12d += memoryWait32[(rbx[0] >> 24) & 0x0F];
	/* 45 39 EC / 0F 8D FF 00 00 00   jge 桩 0x080B66E8 */
	if (r12d >= r13d)
	{
		r14->nextPc = 0x080B66E8u;
		r14->status = JITX_BUDGET;
		goto epilogue;
	}

	/* ===== 080B66E8  cmp r1, #5 =========================================
	 *
	 * 只算标志不写寄存器（sub 但不回写）。C 是"借位的反面"，所以最后多一条
	 * xor ecx,1。102 字节，和 add 一样贵。
	 */

	/* 41 BA 05 00 00 00        mov  r10d, 5 */
	r10d = 5;
	/* 44 8B 4B 04              mov  r9d, [rbx+4]            ; reg[1] */
	r9d = rbx[1];
	/* 45 89 C8                 mov  r8d, r9d */
	/* 45 29 D0                 sub  r8d, r10d               ; 结果只给标志用 */
	r8d = r9d - r10d;
	/* 45 85 C0 / 0F 98 C0 / 88 83 AC 1C F9 FF */
	N_FLAG = (r8d >> 31) & 1;
	/* 0F 94 C0 / 88 83 AF 1C F9 FF */
	Z_FLAG = (r8d == 0);
	/* 44 89 C9 / 44 31 D1 / 44 89 CA / 44 31 C2 / 21 D1 / C1 E9 1F */
	V_FLAG = (((r9d ^ r10d) & (r9d ^ r8d)) >> 31) & 1;
	/* 44 89 C9 / 44 89 D2 / 48 29 D1 / 48 C1 E9 3F / 83 F1 01
	 *                          64 位减，看第 63 位有没有借位，再取反 */
	C_FLAG = (uint8_t)!(uint8_t)(((uint64_t)r9d - (uint64_t)r10d) >> 63);
	/* 41 83 C4 01 / 44 03 A3 ... */
	r12d += 1 + memoryWaitFetch[0x08];
	/* 45 39 EC / 0F 8D AA 00 00 00   jge 桩 0x080B66EA */
	if (r12d >= r13d)
	{
		r14->nextPc = 0x080B66EAu;
		r14->status = JITX_BUDGET;
		goto epilogue;
	}

	/* ===== 080B66EA  bls 080B66EE =======================================
	 *
	 * LS = C==0 || Z==1。生成代码直接把它翻译成两条测试 + 两条 je：
	 *
	 * 80 BB AE 1C F9 FF 00     cmp  byte [rbx-0x6E352], 0   ; C_FLAG
	 * 0F 84 0D 00 00 00        je   走"跳"
	 * 80 BB AF 1C F9 FF 00     cmp  byte [rbx-0x6E351], 0   ; Z_FLAG
	 * 0F 84 18 00 00 00        je   走"不跳"
	 */
	if (C_FLAG == 0)
		goto branch_taken;
	if (Z_FLAG == 0)
		goto branch_not_taken;

branch_taken:
	/* 41 83 C4 03              add  r12d, 3                  ; 跳转的固定开销 */
	r12d += 3;
	/* 41 C7 06 EE 66 0B 08     mov  dword [r14], 0x080B66EE  ; out->nextPc */
	r14->nextPc = 0x080B66EEu;
	/* 41 C7 46 04 08 00 00 00  mov  dword [r14+4], 8        ; JITX_DONE */
	r14->status = JITX_DONE;
	/* E9 33 00 00 00           jmp  epilogue */
	goto epilogue;

branch_not_taken:
	/* 41 83 C4 01              add  r12d, 1 */
	/* 44 03 A3 D0 E0 F2 FF     add  r12d, [rbx-0xD1F30] */
	r12d += 1 + memoryWaitFetch[0x08];
	/* 41 C7 06 EC 66 0B 08     mov  dword [r14], 0x080B66EC */
	r14->nextPc = 0x080B66ECu;
	/* 41 C7 46 04 08 00 00 00  mov  dword [r14+4], 8 */
	r14->status = JITX_DONE;
	/* E9 14 00 00 00           jmp  epilogue */
	goto epilogue;

	/* 注意 511 字节处的第三条出口（mov [r14],0x080B66EC / E9 00 00 00 00）
	 * 已经没用了：两条分支都显式 jmp 到 epilogue，它是"块跑完自然落下"那条
	 * 出口留下的死代码，25 字节。每帧几千个块，值得清掉。 */

epilogue:
	/* 44 89 E0                 mov  eax, r12d   ; 返回值 = 周期数 */
	/* 48 83 C4 20              add  rsp, 0x20 */
	/* 5E 5F 5D 41 5E 41 5D 41 5C 5B   pop rsi,rdi,rbp,r14,r13,r12,rbx */
	/* C3                       ret */
	return r12d;
}

/* 4. 同样的事，人写的话只要这么几行 ---------------------------------------
 *
 *    这就是内联解释器（arm-new.h 的 handler）的写法：直接、无冗余、
 *    但每条指令都要重新取指、解码、分派。
 */

static void setAddFlags(uint32_t lhs, uint32_t rhs, uint32_t res)
{
	N_FLAG = (res >> 31) & 1;
	Z_FLAG = (res == 0);
	V_FLAG = (((lhs ^ res) & (rhs ^ res)) >> 31) & 1;
	C_FLAG = (uint8_t)(((uint64_t)lhs + (uint64_t)rhs) >> 32);
}

static void setSubFlags(uint32_t lhs, uint32_t rhs, uint32_t res)
{
	N_FLAG = (res >> 31) & 1;
	Z_FLAG = (res == 0);
	V_FLAG = (((lhs ^ rhs) & (lhs ^ res)) >> 31) & 1;
	C_FLAG = (uint8_t)!(uint8_t)(((uint64_t)lhs - (uint64_t)rhs) >> 63);
}

static int32_t hand_written_block(int32_t budget, struct JitExecResult *out)
{
	int32_t cycles = 0;
	uint32_t r;

	reg[0] = reg[7] + 0;      setAddFlags(reg[7], 0, reg[0]); cycles += 1 + memoryWaitFetch[8];
	reg[0] = reg[0] + 8;      setAddFlags(reg[0] - 8, 8, reg[0]); cycles += 1 + memoryWaitFetch[8];
	reg[15] = 0x080B66EA;
	r = CPUReadByte(reg[0]) & 0xFF;
	reg[1] = r;                cycles += 3 + memoryWaitFetch[8] + memoryWait32[(reg[0] >> 24) & 15];
	(void)budget;
	setSubFlags(reg[1], 5, reg[1] - 5);
	cycles += 1 + memoryWaitFetch[8];

	if (!C_FLAG || Z_FLAG)	/* LS = C==0 || Z==1，和生成代码的两条 je 等价 */
	{
		cycles += 3;		/* 跳成功：固定 3 个周期 */
		out->nextPc = 0x080B66EE;
		out->status = JITX_DONE;
	}
	else
	{
		cycles += 1 + memoryWaitFetch[8];
		out->nextPc = 0x080B66EC;
		out->status = JITX_DONE;
	}
	return cycles;
}

/* 5. 跑两遍，比结果 ------------------------------------------------------- */

static void reset_state(void)
{
	memset(reg, 0, sizeof(reg));
	N_FLAG = V_FLAG = C_FLAG = Z_FLAG = 0;
	armState = 0;
	memset(extMem, 0, sizeof(extMem));
	for (int i = 0; i < 0x100; i++)
		extMem[i] = (uint8_t)(i * 7 + 3);
}

static void print_state(const char *who, int32_t cycles, struct JitExecResult *r)
{
	printf("%-14s r0=%08X r1=%08X r7=%08X r15=%08X  N%d Z%d C%d V%d  "
		   "cycles=%2d  nextPc=%08X status=%u\n",
		   who, reg[0], reg[1], reg[7], reg[15], N_FLAG, Z_FLAG, C_FLAG, V_FLAG, cycles,
		   r->nextPc, r->status);
}

int main(void)
{
	struct JitExecResult a, b;
	int32_t             ca, cb;

	printf("guest 块 080B66E2（Thumb，5 条指令）：\n");
	printf("    080B66E2  1C38  adds  r0, r7, #0\n");
	printf("    080B66E4  3008  adds  r0, r0, #8\n");
	printf("    080B66E6  7801  ldrb  r1, [r0]\n");
	printf("    080B66E8  2905  cmp   r1, #5\n");
	printf("    080B66EA  D900  bls   080B66EE\n\n");

	reset_state();
	reg[7]  = 0x08000123;	/* r0 最后 = 0x0800012B，读 extMem[0x2B] = 0x78 */
	reg[15] = 0x080B66E2;

	ca = jit_block_080B66E2(0x7FFFFFFF, &a);
	print_state("生成代码：", ca, &a);

	reset_state();
	reg[7]  = 0x08000123;
	reg[15] = 0x080B66E2;

	cb = hand_written_block(0x7FFFFFFF, &b);
	print_state("手写：", cb, &b);

	printf("\n两条路径%s\n\n",
		   (ca == cb && a.nextPc == b.nextPc && a.status == b.status)
			   ? "完全一致"
			   : "不一致——说明我把翻译写错了");

	/* ---- 字节账 ---- */
	printf("597 字节机器码花在哪：\n");
	for (int i = 0; i < (int)(sizeof(regions) / sizeof(regions[0])); i++)
	{
		int bar = regions[i].bytes / 2;
		printf("  %-46s %3d 字节  ", regions[i].what, regions[i].bytes);
		for (int k = 0; k < bar; k++)
			putchar('#');
		putchar('\n');
	}

	printf("\n结论：\n");
	printf("  · 5 条 guest 指令 -> %d 字节机器码，平均 %d 字节/条\n",
		   (int)sizeof(code), (int)(sizeof(code) / 5));
	printf("  · 其中 51 字节是 prologue + epilogue（进出块的固定开销）\n");
	printf("  · 68 字节是出口桩，也被 4 条指令共用\n");
	printf("  · 真正干活的 478 字节里，一半以上在算 N/Z/C/V 四个标志位\n");
	printf("  · 块平均只有 5 条指令，所以这 51 字节的进出开销每条指令都要摊一遍\n");
	printf("  · 这就是 -jit 现在跑不赢内联解释器的原因（见 jit-recompiler.md 3.2.1.17）\n");
	return 0;
}
