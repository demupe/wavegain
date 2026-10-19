/* Private GNU-assembly backend; included after filter_pair is defined.
 * SSE2 is part of the x86-64 baseline. All products and sums retain the
 * scalar reference order: do not replace them with FMA or partial sums. */
#ifndef WG_GAIN_ANALYSIS_X86_H
#define WG_GAIN_ANALYSIS_X86_H

#ifndef WAVEGAIN_X86_ASM
#define WAVEGAIN_X86_ASM 1
#endif
#if WAVEGAIN_X86_ASM && defined(__x86_64__) && defined(__SSE2__) && \
	(defined(__GNUC__) || defined(__clang__)) && !defined(__FAST_MATH__)
#define WG_RG_X86_ASM 1

/* xmm3..12: ten stereo Yule histories; xmm13/14: Butterworth histories.
 * xmm0: Yule result; xmm2: Butterworth result; xmm1: one rounded product.
 * Coefficients stay in aligned memory, leaving the histories in registers.
 * The compiler saves the Windows nonvolatile XMM registers from clobbers. */
#define WG_RG_TAP(y, a, offset, b) \
	"movapd %%xmm" #y ", %%xmm1\n\t" \
	"mulpd " #a "(%[k]), %%xmm1\n\t" \
	"subpd %%xmm1, %%xmm0\n\t" \
	"movsd " #offset "(%[l]), %%xmm1\n\t" \
	"movhpd " #offset "(%[r]), %%xmm1\n\t" \
	"mulpd " #b "(%[k]), %%xmm1\n\t" \
	"addpd %%xmm1, %%xmm0\n\t"
#define WG_RG_HISTORY(reg, offset, l, r) \
	"movsd " #offset "(%[" #l "]), %%xmm" #reg "\n\t" \
	"movhpd " #offset "(%[" #r "]), %%xmm" #reg "\n\t"

static void
filterStereoX86(const Float_t *left, const Float_t *right,
	Float_t *step_left, Float_t *step_right,
	Float_t *out_left, Float_t *out_right, size_t count,
	const Float_t *yule, const Float_t *butter)
{
	filter_pair k[27];
	int j;
	if (!count) return;
	for (j = 0; j < 21; j++) k[j] = (filter_pair){yule[j], yule[j]};
	for (j = 0; j < 5; j++) k[j + 21] = (filter_pair){butter[j], butter[j]};
	k[26] = (filter_pair){1e-10, 1e-10};
	__asm__ volatile (
		WG_RG_HISTORY(3, -8, sl, sr)
		WG_RG_HISTORY(4, -16, sl, sr)
		WG_RG_HISTORY(5, -24, sl, sr)
		WG_RG_HISTORY(6, -32, sl, sr)
		WG_RG_HISTORY(7, -40, sl, sr)
		WG_RG_HISTORY(8, -48, sl, sr)
		WG_RG_HISTORY(9, -56, sl, sr)
		WG_RG_HISTORY(10, -64, sl, sr)
		WG_RG_HISTORY(11, -72, sl, sr)
		WG_RG_HISTORY(12, -80, sl, sr)
		WG_RG_HISTORY(13, -8, ol, or)
		WG_RG_HISTORY(14, -16, ol, or)
		"1:\n\t"
		WG_RG_HISTORY(0, 0, l, r)
		"mulpd 0(%[k]), %%xmm0\n\t"
		"addpd 416(%[k]), %%xmm0\n\t"
		WG_RG_TAP(3, 16, -8, 32)
		WG_RG_TAP(4, 48, -16, 64)
		WG_RG_TAP(5, 80, -24, 96)
		WG_RG_TAP(6, 112, -32, 128)
		WG_RG_TAP(7, 144, -40, 160)
		WG_RG_TAP(8, 176, -48, 192)
		WG_RG_TAP(9, 208, -56, 224)
		WG_RG_TAP(10, 240, -64, 256)
		WG_RG_TAP(11, 272, -72, 288)
		WG_RG_TAP(12, 304, -80, 320)
		"movapd %%xmm0, %%xmm2\n\t"
		"mulpd 336(%[k]), %%xmm2\n\t"
		"movapd %%xmm13, %%xmm1\n\t"
		"mulpd 352(%[k]), %%xmm1\n\t"
		"subpd %%xmm1, %%xmm2\n\t"
		"movapd %%xmm3, %%xmm1\n\t"
		"mulpd 368(%[k]), %%xmm1\n\t"
		"addpd %%xmm1, %%xmm2\n\t"
		"movapd %%xmm14, %%xmm1\n\t"
		"mulpd 384(%[k]), %%xmm1\n\t"
		"subpd %%xmm1, %%xmm2\n\t"
		"movapd %%xmm4, %%xmm1\n\t"
		"mulpd 400(%[k]), %%xmm1\n\t"
		"addpd %%xmm1, %%xmm2\n\t"
		/* Keep the complete planar histories visible at every call boundary. */
		"movlpd %%xmm0, (%[sl])\n\t"
		"movhpd %%xmm0, (%[sr])\n\t"
		"movlpd %%xmm2, (%[ol])\n\t"
		"movhpd %%xmm2, (%[or])\n\t"
		"movapd %%xmm11, %%xmm12\n\t"
		"movapd %%xmm10, %%xmm11\n\t"
		"movapd %%xmm9, %%xmm10\n\t"
		"movapd %%xmm8, %%xmm9\n\t"
		"movapd %%xmm7, %%xmm8\n\t"
		"movapd %%xmm6, %%xmm7\n\t"
		"movapd %%xmm5, %%xmm6\n\t"
		"movapd %%xmm4, %%xmm5\n\t"
		"movapd %%xmm3, %%xmm4\n\t"
		"movapd %%xmm0, %%xmm3\n\t"
		"movapd %%xmm13, %%xmm14\n\t"
		"movapd %%xmm2, %%xmm13\n\t"
		"addq $8, %[l]\n\t"
		"addq $8, %[r]\n\t"
		"addq $8, %[sl]\n\t"
		"addq $8, %[sr]\n\t"
		"addq $8, %[ol]\n\t"
		"addq $8, %[or]\n\t"
		"decq %[n]\n\t"
		"jnz 1b\n\t"
		: [l] "+&r"(left), [r] "+&r"(right),
		  [sl] "+&r"(step_left), [sr] "+&r"(step_right),
		  [ol] "+&r"(out_left), [or] "+&r"(out_right), [n] "+&r"(count)
		: [k] "r"(k)
		: "memory", "cc", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4",
		  "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
		  "xmm12", "xmm13", "xmm14");
}
#undef WG_RG_TAP
#undef WG_RG_HISTORY
#endif
#endif
