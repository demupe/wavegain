/* 127-tap Blackman-windowed low-pass FIR for high-rate ReplayGain analysis. */
#include <math.h>
#include <string.h>
#include "resample.h"
#include "resample_x86.h"

void wg_decimator_init(WG_DECIMATOR *s, int channels)
{
	const double pi = 3.14159265358979323846, cutoff = .225;
	double sum = 0.;
	int i;
	memset(s, 0, sizeof(*s));
	s->channels = channels;
	for (i = 0; i < 127; i++) {
		int n = i - 63;
		double window = .42 - .5 * cos(2. * pi * i / 126.) + .08 * cos(4. * pi * i / 126.);
		s->taps[i] = window * (n ? sin(2. * pi * cutoff * n) / (pi * n) : 2. * cutoff);
		sum += s->taps[i];
	}
	for (i = 0; i < 127; i++) s->taps[i] /= sum;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize")))
#endif
int wg_decimator_process(WG_DECIMATOR *s, double **pcm, int frames)
{
	int i, ch, out = 0;
	/* Keep streaming metadata in registers across assembly memory clobbers.
	 * Publish the exact scalar state once, at the API call boundary. */
	const int channels = s->channels;
	int cursor = s->cursor;
	unsigned long long position = s->position;
#ifdef WG_FIR_AVX2
	const int avx2 = __builtin_cpu_supports("avx2");
#endif
	for (i = 0; i < frames; i++, position++) {
		for (ch = 0; ch < channels; ch++)
			s->history[ch][cursor] = s->history[ch][cursor + 127] = pcm[ch][i];
		if (position >= 63 && !((position - 63) & 1)) {
#ifdef WG_FIR_X86_ASM
			if (frames - i >= 15 && (channels == 2
#ifdef WG_FIR_AVX2
				|| (channels == 1 && avx2 && cursor <= 111)
#endif
				)) {
				double output[2][8];
				int k, m;
#ifdef WG_FIR_AVX2
				/* A 256-bit load also reads the unused odd frame at +15.
				 * cursor <= 111 keeps that load inside the mirrored ring.
				 * Stage only the upper mirror: the lower copy is still part
				 * of the first output's 127-frame history. */
				if (avx2 && cursor <= 111) {
					for (k = 1; k <= 14; k++)
						for (ch = 0; ch < channels; ch++)
							s->history[ch][cursor + 127 + k] = pcm[ch][i + k];
					if (channels == 1)
						wg_fir_mono8_avx2(s->history[0] + cursor + 127,
							s->taps, output[0]);
					else
						wg_fir_stereo8_avx2(s->history[0] + cursor + 127,
							s->history[1] + cursor + 127, s->taps, output);
				} else
#endif
				{
					wg_fir_pair pairs[8];
					wg_fir_stereo8_sse2(s->history[0] + cursor + 127,
						s->history[1] + cursor + 127, s->taps,
						pcm[0] + i + 1, pcm[1] + i + 1, pairs);
					for (m = 0; m < 8; m++) {
						output[0][m] = pairs[m][0];
						output[1][m] = pairs[m][1];
					}
				}
				/* Consume lookahead before overwriting in-place PCM. At the
				 * next iteration both mirror copies and all counters equal
				 * the scalar streaming state; no samples cross a call boundary. */
				for (k = 1; k <= 14; k++) {
					if (++cursor == 127) cursor = 0;
					for (ch = 0; ch < channels; ch++)
						s->history[ch][cursor] = s->history[ch][cursor + 127] = pcm[ch][i + k];
				}
				for (ch = 0; ch < channels; ch++)
					memcpy(pcm[ch] + out, output[ch], sizeof(output[ch]));
				out += 7;
				i += 14;
				position += 14;
			} else
#endif
			{
				for (ch = 0; ch < channels; ch++) {
					double value = 0.;
					const double *history = s->history[ch] + cursor + 127;
					int j;
					/* Keep the original scalar multiply-add contraction. Vectorizing
					 * products alone can change rounding on FMA-capable targets. */
#if defined(__clang__)
#pragma clang loop vectorize(disable)
#endif
					for (j = 0; j < 127; j++)
						value += s->taps[j] * history[-j];
					pcm[ch][out] = value;
				}
			}
			out++;
		}
		if (++cursor == 127) cursor = 0;
	}
	s->cursor = cursor;
	s->position = position;
	return out;
}
