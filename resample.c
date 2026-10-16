/* 127-tap Blackman-windowed low-pass FIR for high-rate ReplayGain analysis. */
#include <math.h>
#include <string.h>
#include "resample.h"

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

int wg_decimator_process(WG_DECIMATOR *s, double **pcm, int frames)
{
	int i, ch, out = 0;
	for (i = 0; i < frames; i++, s->position++) {
		for (ch = 0; ch < s->channels; ch++) s->history[ch][s->cursor] = pcm[ch][i];
		if (s->position >= 63 && !((s->position - 63) & 1)) {
			for (ch = 0; ch < s->channels; ch++) {
				double value = 0.;
				int j, index = s->cursor;
				for (j = 0; j < 127; j++) {
					value += s->taps[j] * s->history[ch][index];
					if (--index < 0) index = 126;
				}
				pcm[ch][out] = value;
			}
			out++;
		}
		if (++s->cursor == 127) s->cursor = 0;
	}
	return out;
}
