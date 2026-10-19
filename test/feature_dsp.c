/* Signal-level checks for analysis decimation and opt-in rate-aware dither. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../src/resample.h"
#include "../src/dither.h"
dither_t Dither;
#include "../src/dither.c"

static double tone_energy(double frequency, int irregular)
{
	WG_DECIMATOR s;
	double data[257], *pcm[2] = {data, NULL}, sum = 0.;
	int i, position = 0, output = 0, frames = 8193;
	wg_decimator_init(&s, 1);
	while (position < frames) {
		int count = irregular ? position % 251 + 1 : 257;
		int n;
		if (count > frames - position) count = frames - position;
		for (i = 0; i < count; i++) data[i] = sin(6.2831853071795864769 * frequency * (position + i));
		n = wg_decimator_process(&s, pcm, count);
		for (i = 0; i < n; i++, output++) if (output > 127) sum += data[i] * data[i];
		position += count;
	}
	memset(data, 0, sizeof(data));
	output += wg_decimator_process(&s, pcm, 63);
	assert(output == (frames + 1) / 2);
	return sum;
}

static double dither_high_frequency(long rate, int shape)
{
	double energy = 0., difference = 0., mean = 0., previous = 0.;
	int i;
	Init_Dither(16, shape);
	Init_Rate_Dither(rate, shape);
	for (i = 0; i < 100000; i++) {
		double value = (double)Rate_Dither(0., 0, 16);
		energy += value * value;
		difference += (value - previous) * (value - previous);
		mean += value;
		previous = value;
	}
	assert(fabs(mean / 100000.) < .02);
	assert(energy > 1000. && energy < 200000.);
	return difference / energy;
}

/* Keep the original wrapping convolution as a bitwise oracle. Mirroring must
 * preserve the accumulation order, startup delay, tail and chunk boundaries. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("fp-contract=off")))
#endif
static void check_decimator_bits(void)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(_MSC_VER)
#pragma float_control(precise, on)
#pragma fp_contract(off)
#endif
	WG_DECIMATOR state;
	double history[2][127] = {{0}}, data[2][257], expected[2][257];
	double *pcm[2] = {data[0], data[1]};
	uint32_t random = UINT32_C(0x12345);
	const int chunks[] = {1,2,3,13,14,15,16,17,29,30,31,63,64,65,
		111,112,113,125,126,127,128,129,255,256,257};
	int channels, mode, cursor, position, start, chunk, index, i, ch, j, out, got;
	for (channels = 1; channels <= 2; channels++) {
	for (mode = 0; mode < 3; mode++) {
		wg_decimator_init(&state, channels);
		memset(history, 0, sizeof(history));
		cursor = position = index = 0;
		for (start = 0; start < 10063; start += chunk) {
			chunk = mode == 0 ? start % 257 + 1 : mode == 1 ?
				chunks[index++ % (sizeof(chunks) / sizeof(*chunks))] : 257;
			if (chunk > 10063 - start) chunk = 10063 - start;
			out = 0;
			for (i = 0; i < chunk; i++, position++) {
				for (ch = 0; ch < channels; ch++) {
					random = random * UINT32_C(1664525) + UINT32_C(1013904223);
					data[ch][i] = position >= 10000 ? 0. : ((double)random - 2147483648.) * 0x1p-31;
					if (position % 59 == 0) data[ch][i] = ch ? -1e-300 : 1e-300;
					history[ch][cursor] = data[ch][i];
				}
				if (position >= 63 && !((position - 63) & 1)) {
					for (ch = 0; ch < channels; ch++) {
						double value = 0.;
						int index = cursor;
						for (j = 0; j < 127; j++) {
							value += state.taps[j] * history[ch][index];
							if (--index < 0) index = 126;
						}
						expected[ch][out] = value;
					}
					out++;
				}
				if (++cursor == 127) cursor = 0;
			}
			got = wg_decimator_process(&state, pcm, chunk);
			assert(got == out);
			assert(state.cursor == cursor && state.position == (unsigned long long)position);
			for (ch = 0; ch < channels; ch++) {
				assert(!memcmp(data[ch], expected[ch], out * sizeof(double)));
				/* Lookahead must leave both mirrored histories identical to
				 * the one-frame-at-a-time reference at every call boundary. */
				assert(!memcmp(state.history[ch], history[ch], sizeof(history[ch])));
				assert(!memcmp(state.history[ch] + 127, history[ch], sizeof(history[ch])));
			}
			{
				WG_DECIMATOR saved = state;
				assert(wg_decimator_process(&state, NULL, 0) == 0);
				assert(!memcmp(&state, &saved, sizeof(state)));
			}
		}
	}
	}
}

int main(void)
{
	check_decimator_bits();
	double pass = tone_energy(.05, 0), stop = tone_energy(.4, 0);
	double low, high, flat;
	assert(pass > 1900. && pass < 2100.);
	assert(stop < pass * 1e-8);
	assert(fabs(tone_energy(.05, 1) / pass - 1.) < .001);
	low = dither_high_frequency(8000, 3);
	high = dither_high_frequency(192000, 3);
	flat = dither_high_frequency(48000, 0);
	assert(high > low + .2);
	assert(fabs(flat - 2.) < .05);
	Init_Rate_Dither(192000, 3);
	assert(Rate_Dither(1e300, 0, 8) == 127);
	assert(Rate_Dither(-1e300, 0, 8) == -128);
	printf("DSP: decimation passband/stopband, edge length, chunking and rate-shaped dither spectra passed (ratios %.3f/%.3f/%.3f)\n", low, high, flat);
	return 0;
}
