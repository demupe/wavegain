/* Known continuous-tone amplitudes, independent of the library reference. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/loudness.h"

static void check_abrupt_tail(void)
{
	const long rates[] = {176400, 192000};
	double data[2][512], *pcm[] = {data[0], data[1]};
	long double expected = 0., pi = acosl(-1.L);
	/* Independent ideal reconstruction of a finite rectangular pulse near its
 * last sample, explicitly zero beyond EOF. The opposite edge is symmetric. */
	for (int phase = 0; phase <= 512; phase++) {
		long double t = 507.L + phase / 64.L, sum = 0.;
		for (int i = 0; i < 512; i++) {
			long double d = t - i;
			sum += .5L * (d == 0. ? 1.L : sinl(pi * d) / (pi * d));
		}
		if (fabsl(sum) > expected) expected = fabsl(sum);
	}
	assert(expected > .55);
	for (int i = 0; i < 512; i++) { data[0][i] = .5; data[1][i] = -.5; }
	for (size_t r = 0; r < sizeof(rates) / sizeof(*rates); r++)
	for (unsigned int channels = 1; channels <= 2; channels++)
	for (int loudness = 0; loudness <= 1; loudness++) {
		double peak;
		WG_METER *m = wg_meter_create_mode(rates[r], channels, loudness, 1, WG_PEAK_PRECISE);
		assert(m); assert(wg_meter_add(m, pcm, 512)); assert(wg_meter_finish(m, NULL, &peak));
		/* This pulse's finite-FIR estimate must include endpoint overshoot. */
		assert(fabs(20 * log10(peak / (double)expected)) < .15);
		wg_meter_free(m);
	}
}

int main(void)
{
	check_abrupt_tail();
	const long rates[] = {44100, 48000, 96000, 192000};
	const double frequencies[] = {1./8, 1./6, 1./4, 3./8, .4, .45, .475, .49};
	const int chunks[] = {1, 2, 3, 63, 64, 65, 257};
	double data[2][257], *pcm[2] = {data[0], data[1]};
	double worst_under_read[2] = {0., 0.};
	unsigned int r, f;
	int channels, phase, mode, quality;
	for (r = 0; r < sizeof(rates) / sizeof(*rates); r++)
	for (f = 0; f < sizeof(frequencies) / sizeof(*frequencies); f++)
	for (channels = 1; channels <= 2; channels++)
	for (phase = 0; phase < 8; phase++)
	for (quality = WG_PEAK_PRECISE; quality <= WG_PEAK_FAST; quality++)
	for (mode = 0; mode <= 1; mode++) {
		WG_METER *m = wg_meter_create_mode(rates[r], channels, mode, 1, (WG_PEAK_MODE)quality);
		double peak, again;
		int start = 0, block = 0;
		assert(m);
		while (start < 4096) {
			int count = chunks[block++ % (sizeof(chunks) / sizeof(*chunks))];
			if (count > 4096 - start) count = 4096 - start;
			for (int ch = 0; ch < channels; ch++) for (int i = 0; i < count; i++) {
				int n = start + i;
				double edge = fmin(1., fmin(n / 512., (4095 - n) / 512.));
				data[ch][i] = .5 * edge * sin(6.2831853071795864769 *
					(frequencies[f] * n + phase / 32. + ch / 16.));
			}
			assert(wg_meter_add(m, pcm, (size_t)count));
			start += count;
		}
		assert(wg_meter_finish(m, NULL, &peak));
		assert(wg_meter_finish(m, NULL, &again) && peak == again);
		/* Precise stays inside the 0.1 dB reserve for this corpus. Fast
		 * allows larger under-read. Neither is a universal error bound. */
		assert(peak >= .5 * pow(10., (quality == WG_PEAK_FAST ? -.5 : -.08) / 20.));
		assert(peak <= .5 * pow(10., .2 / 20.));
		worst_under_read[quality] = fmax(worst_under_read[quality], 20. * log10(.5 / peak));
		wg_meter_free(m);
	}
	assert(!wg_meter_create_mode(48000, 1, 0, 1, (WG_PEAK_MODE)3));
	fprintf(stderr, "Peak accuracy corpus max under-read: precise %.6f dB, fast %.6f dB\n",
		worst_under_read[WG_PEAK_PRECISE], worst_under_read[WG_PEAK_FAST]);
	puts("True peak: precise/fast known amplitudes, phases, 44.1..192 kHz, mono/stereo, combined/peak-only modes and streaming boundaries passed");
	return 0;
}
