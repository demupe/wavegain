/* Assert that FMA-capable targets still compile the verified x86 FIRs. */
#include "../src/resample.c"
#if WAVEGAIN_X86_ASM && defined(__x86_64__) && defined(__SSE2__) && !defined(__FAST_MATH__) && \
	(defined(__GNUC__) || defined(__clang__))
#ifndef WG_FIR_X86_ASM
#error Expected the x86 FIR backend, including on FMA-capable targets
#endif
#endif
int main(void) { return 0; }
