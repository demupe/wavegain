/* Private x86-64 FIR kernels. Each lane accumulates all 127 taps in order;
 * only independent output samples are evaluated in parallel. */
#ifndef WG_RESAMPLE_X86_H
#define WG_RESAMPLE_X86_H

#ifndef WAVEGAIN_X86_ASM
#define WAVEGAIN_X86_ASM 1
#endif
/* The existing scalar FIR may contract on globally FMA-enabled targets.
 * Retain that path there rather than silently change its rounding policy. */
#if WAVEGAIN_X86_ASM && defined(__x86_64__) && defined(__SSE2__) && \
	(defined(__GNUC__) || defined(__clang__)) && \
	!defined(__FAST_MATH__) && !defined(__FMA__)
#define WG_FIR_X86_ASM 1
/* GNU extended-asm templates exceed ISO C's minimum string-literal limit.
 * These compilers support the full templates; keep the exception local. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
typedef double wg_fir_pair __attribute__((vector_size(16)));

#define WG_FIR_EIGHT(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)
/* For the first 14 taps, later outputs draw from the unconsumed lookahead.
 * GAS .if evaluates constants at assembly time; there is no runtime branch. */
#define WG_FIR_HEAD_LANE(m, j) \
	".if " #j " < 2*" #m "\n\t" \
	"movsd (8*(2*" #m "-" #j "-1))(%[fl]), %%xmm9\n\t" \
	"movhpd (8*(2*" #m "-" #j "-1))(%[fr]), %%xmm9\n\t" \
	".else\n\t" \
	"movsd (8*(2*" #m "-" #j "))(%[l]), %%xmm9\n\t" \
	"movhpd (8*(2*" #m "-" #j "))(%[r]), %%xmm9\n\t" \
	".endif\n\t" \
	"mulpd %%xmm8, %%xmm9\n\t" \
	"addpd %%xmm9, %[a" #m "]\n\t"
#define WG_FIR_HEAD(j) \
	"movsd (8*" #j ")(%[t]), %%xmm8\n\t" \
	"unpcklpd %%xmm8, %%xmm8\n\t" \
	WG_FIR_HEAD_LANE(0, j) WG_FIR_HEAD_LANE(1, j) \
	WG_FIR_HEAD_LANE(2, j) WG_FIR_HEAD_LANE(3, j) \
	WG_FIR_HEAD_LANE(4, j) WG_FIR_HEAD_LANE(5, j) \
	WG_FIR_HEAD_LANE(6, j) WG_FIR_HEAD_LANE(7, j)
#define WG_FIR_BODY_LANE(m, j) \
	"movsd (8*(2*" #m "-" #j "))(%[l]), %%xmm9\n\t" \
	"movhpd (8*(2*" #m "-" #j "))(%[r]), %%xmm9\n\t" \
	"mulpd %%xmm8, %%xmm9\n\t" \
	"addpd %%xmm9, %[a" #m "]\n\t"
#define WG_FIR_BODY(j) \
	"movsd (8*" #j ")(%[t],%%rax), %%xmm8\n\t" \
	"unpcklpd %%xmm8, %%xmm8\n\t" \
	WG_FIR_BODY_LANE(0, j) WG_FIR_BODY_LANE(1, j) \
	WG_FIR_BODY_LANE(2, j) WG_FIR_BODY_LANE(3, j) \
	WG_FIR_BODY_LANE(4, j) WG_FIR_BODY_LANE(5, j) \
	WG_FIR_BODY_LANE(6, j) WG_FIR_BODY_LANE(7, j)
#define WG_FIR_ZERO(m) "xorpd %[a" #m "], %[a" #m "]\n\t"

static inline void
wg_fir_stereo8_sse2(const double *left, const double *right,
	const double *taps, const double *future_left, const double *future_right,
	wg_fir_pair result[8])
{
	__asm__ volatile (
		WG_FIR_EIGHT(WG_FIR_ZERO)
		WG_FIR_EIGHT(WG_FIR_HEAD)
		WG_FIR_HEAD(8) WG_FIR_HEAD(9) WG_FIR_HEAD(10)
		WG_FIR_HEAD(11) WG_FIR_HEAD(12) WG_FIR_HEAD(13)
		"movq $112, %%rax\n\t"
		"subq $112, %[l]\n\t"
		"subq $112, %[r]\n\t"
		"1:\n\t"
		WG_FIR_EIGHT(WG_FIR_BODY)
		"addq $64, %%rax\n\t"
		"subq $64, %[l]\n\t"
		"subq $64, %[r]\n\t"
		"cmpq $1008, %%rax\n\t"
		"jne 1b\n\t"
		WG_FIR_BODY(0)
		: [a0] "=&x"(result[0]), [a1] "=&x"(result[1]),
		  [a2] "=&x"(result[2]), [a3] "=&x"(result[3]),
		  [a4] "=&x"(result[4]), [a5] "=&x"(result[5]),
		  [a6] "=&x"(result[6]), [a7] "=&x"(result[7]),
		  [l] "+&r"(left), [r] "+&r"(right)
		: [t] "r"(taps), [fl] "r"(future_left), [fr] "r"(future_right)
		: "rax", "xmm8", "xmm9", "cc", "memory");
}
#undef WG_FIR_HEAD_LANE
#undef WG_FIR_HEAD
#undef WG_FIR_BODY_LANE
#undef WG_FIR_BODY
#undef WG_FIR_ZERO

#ifndef WAVEGAIN_X86_AVX2
#define WAVEGAIN_X86_AVX2 1
#endif
#if WAVEGAIN_X86_AVX2 && \
	(defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5))
#define WG_FIR_AVX2 1
/* Load eight consecutive history values and select the even four.
 * The caller provides 14 future frames and bounds the extra unused odd lane.
 * ymm0/2: left outputs 0..3/4..7; ymm1/3: right outputs 0..3/4..7.
 * Keep lanes in [0,2,1,3] order for the entire convolution; permute only at
 * output, saving four cross-lane shuffles per tap without reordering sums.
 * Only volatile vector registers are used, including on Windows x64. */
#define WG_FIR_AVX_LANE(acc, h, base, j) \
	"vmovupd (" #base "-8*" #j ")(%[" #h "]), %%ymm4\n\t" \
	"vshufpd $0, (" #base "+32-8*" #j ")(%[" #h "]), %%ymm4, %%ymm4\n\t" \
	"vmulpd %%ymm5, %%ymm4, %%ymm4\n\t" \
	"vaddpd %%ymm4, %%ymm" #acc ", %%ymm" #acc "\n\t"
#define WG_FIR_AVX_TAP(j) \
	"vbroadcastsd (8*" #j ")(%[t],%%rax), %%ymm5\n\t" \
	WG_FIR_AVX_LANE(0, l, 0, j) WG_FIR_AVX_LANE(1, r, 0, j) \
	WG_FIR_AVX_LANE(2, l, 64, j) WG_FIR_AVX_LANE(3, r, 64, j)
#define WG_FIR_AVX_MONO_TAP(j) \
	"vbroadcastsd (8*" #j ")(%[t],%%rax), %%ymm5\n\t" \
	WG_FIR_AVX_LANE(0, l, 0, j) WG_FIR_AVX_LANE(2, l, 64, j)
#define WG_FIR_AVX_LOOP(TAP, right_advance) \
	"xorq %%rax, %%rax\n\t" \
	"1:\n\t" \
	WG_FIR_EIGHT(TAP) \
	"addq $64, %%rax\n\t" \
	"subq $64, %[l]\n\t" \
	right_advance \
	"cmpq $960, %%rax\n\t" \
	"jne 1b\n\t" \
	TAP(0) TAP(1) TAP(2) TAP(3) TAP(4) TAP(5) TAP(6)

__attribute__((target("avx2"), noinline))
static void
wg_fir_stereo8_avx2(const double *left, const double *right,
	const double *taps, double output[2][8])
{
	__asm__ volatile (
		"vxorpd %%ymm0, %%ymm0, %%ymm0\n\t"
		"vxorpd %%ymm1, %%ymm1, %%ymm1\n\t"
		"vxorpd %%ymm2, %%ymm2, %%ymm2\n\t"
		"vxorpd %%ymm3, %%ymm3, %%ymm3\n\t"
		WG_FIR_AVX_LOOP(WG_FIR_AVX_TAP, "subq $64, %[r]\n\t")
		"vpermpd $216, %%ymm0, %%ymm0\n\t"
		"vpermpd $216, %%ymm1, %%ymm1\n\t"
		"vpermpd $216, %%ymm2, %%ymm2\n\t"
		"vpermpd $216, %%ymm3, %%ymm3\n\t"
		"vmovupd %%ymm0, 0(%[o])\n\t"
		"vmovupd %%ymm2, 32(%[o])\n\t"
		"vmovupd %%ymm1, 64(%[o])\n\t"
		"vmovupd %%ymm3, 96(%[o])\n\t"
		/* This is a separate AVX function: upper lanes are call-clobbered.
		 * Clear them before returning to the baseline SSE2 caller. */
		"vzeroupper\n\t"
		: [l] "+&r"(left), [r] "+&r"(right)
		: [t] "r"(taps), [o] "r"(output)
		: "rax", "ymm0", "ymm1", "ymm2", "ymm3", "ymm4", "ymm5",
		  "cc", "memory");
}

/* Mono uses the same time lanes, without loading a duplicate right channel. */
__attribute__((target("avx2"), noinline))
static void
wg_fir_mono8_avx2(const double *left, const double *taps, double output[8])
{
	__asm__ volatile (
		"vxorpd %%ymm0, %%ymm0, %%ymm0\n\t"
		"vxorpd %%ymm2, %%ymm2, %%ymm2\n\t"
		WG_FIR_AVX_LOOP(WG_FIR_AVX_MONO_TAP, "")
		"vpermpd $216, %%ymm0, %%ymm0\n\t"
		"vpermpd $216, %%ymm2, %%ymm2\n\t"
		"vmovupd %%ymm0, 0(%[o])\n\t"
		"vmovupd %%ymm2, 32(%[o])\n\t"
		"vzeroupper\n\t"
		: [l] "+&r"(left)
		: [t] "r"(taps), [o] "r"(output)
		: "rax", "ymm0", "ymm2", "ymm4", "ymm5", "cc", "memory");
}
#undef WG_FIR_AVX_LANE
#undef WG_FIR_AVX_TAP
#undef WG_FIR_AVX_MONO_TAP
#undef WG_FIR_AVX_LOOP
#endif
#undef WG_FIR_EIGHT
#pragma GCC diagnostic pop
#endif
#endif
