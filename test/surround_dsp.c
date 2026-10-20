/* Independent channel-weight checks against identical mono measurements. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../src/loudness.h"

static void measure(unsigned int channels, unsigned int mask, unsigned int active,
	int loudness, WG_PEAK_MODE mode, double *level, double *peak)
{
	WG_METER *m = wg_meter_create_layout(48000, channels, mask, loudness, 1, mode);
	double samples[WG_MAX_CHANNELS][257], *pcm[WG_MAX_CHANNELS];
	size_t frame = 0, total = 24001;
	unsigned int ch;
	assert(m);
	for (ch = 0; ch < channels; ch++) pcm[ch] = samples[ch];
	while (frame < total) {
		size_t i, n = frame % 2 ? 257 : 17;
		if (n > total - frame) n = total - frame;
		for (ch = 0; ch < channels; ch++) for (i = 0; i < n; i++)
			samples[ch][i] = ch != active ? 0. : loudness ?
				.1 * sin(6.2831853071795864769 * 1000 * (frame + i) / 48000.) :
				(frame + i == total - 1 ? .95 : ((frame + i) % 4 < 2 ? .8 : -.8));
		assert(wg_meter_add(m, pcm, n));
		frame += n;
	}
	assert(wg_meter_finish(m, level, peak));
	wg_meter_free(m);
}

int main(void)
{
	const unsigned int masks[] = {WG_MASK_51_BACK, WG_MASK_51_SIDE, WG_MASK_71};
	double mono, mono_peak, level, peak;
	unsigned int layout, ch;
	assert(!wg_meter_create_layout(48000, 8, 0, 1, 1, WG_PEAK_FAST));
	assert(!wg_meter_create_layout(48000, 6, WG_MASK_71, 1, 1, WG_PEAK_FAST));
	measure(1, 0, 0, 1, WG_PEAK_FAST, &mono, &mono_peak);
	for (layout = 0; layout < 3; layout++) {
		unsigned int channels = layout == 2 ? 8 : 6;
		for (ch = 0; ch < channels; ch++) {
			double weight = ch < 3 || (channels == 8 && ch < 6) ? 1. : 1.41;
			measure(channels, masks[layout], ch, 1, WG_PEAK_FAST, &level, &peak);
			if (ch == 3) assert(isinf(level) && level < 0.);
			else assert(fabs(level - mono - 10. * log10(weight)) < 1e-10);
			/* LFE is excluded only from loudness, never from peak protection. */
			assert(fabs(peak - mono_peak) < 1e-12);
		}
	}
	for (int mode = WG_PEAK_PRECISE; mode <= WG_PEAK_EXTENDED; mode++) {
		measure(1, 0, 0, 0, (WG_PEAK_MODE)mode, NULL, &mono_peak);
		assert(mono_peak > 1.);
		for (ch = 0; ch < 8; ch++) {
			measure(8, WG_MASK_71, ch, 0, (WG_PEAK_MODE)mode, NULL, &peak);
			assert(fabs(peak - mono_peak) < 1e-12);
		}
	}
	puts("Surround channel weights, LFE exclusion and all-channel peak modes passed");
	return 0;
}
