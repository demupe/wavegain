/* Compare optimized kernels to the original implementation on this platform. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "../src/loudness.c"

static void emit_state(ebur128_state *s)
{
	double value;
	unsigned int ch;
	assert(ebur128_loudness_global(s, &value) == EBUR128_SUCCESS);
	assert(fwrite(&value, sizeof(value), 1, stdout) == 1);
	for (ch = 0; ch < s->channels; ch++) {
		assert(ebur128_true_peak(s, ch, &value) == EBUR128_SUCCESS);
		assert(fwrite(&value, sizeof(value), 1, stdout) == 1);
		assert(ebur128_sample_peak(s, ch, &value) == EBUR128_SUCCESS);
		assert(fwrite(&value, sizeof(value), 1, stdout) == 1);
		assert(fwrite(s->d->v[ch], sizeof(double), 5, stdout) == 5);
	}
	assert(fwrite(s->d->audio_data, sizeof(double), s->d->audio_data_frames * s->channels, stdout)
	       == s->d->audio_data_frames * s->channels);
	if (s->d->use_histogram)
		assert(fwrite(s->d->block_energy_histogram, sizeof(unsigned long), 1000, stdout) == 1000);
	else {
		struct ebur128_dq_entry *entry;
		STAILQ_FOREACH(entry, &s->d->block_list, entries)
			assert(fwrite(&entry->z, sizeof(double), 1, stdout) == 1);
	}
}

static void adapter_regression(unsigned long rate, unsigned int channels)
{
	size_t lengths[] = {0, 1, 37, rate / 2 + 13}, k;
	int quality;
	for (quality = WG_PEAK_PRECISE; quality <= WG_PEAK_FAST; quality++)
	for (k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
		size_t n = lengths[k], i, pos = 0;
		unsigned int ch;
		double *pcm[2], peak, again;
		WG_METER *m = wg_meter_create_mode(rate, channels, 0, 1, (WG_PEAK_MODE)quality);
		assert(m);
		for (ch = 0; ch < channels; ch++) {
			pcm[ch] = calloc(n + 1, sizeof(double));
			assert(pcm[ch]);
			for (i = 0; i < n; i++) pcm[ch][i] = .4 * sin(i * (.11 + ch * .07));
			if (n) pcm[ch][n - 1] = -.99;
		}
		assert(wg_meter_add(m, pcm, 0));
		while (pos < n) {
			size_t count = pos % 521 + 1;
			double *block[2] = {pcm[0] + pos, channels == 2 ? pcm[1] + pos : NULL};
			if (count > n - pos) count = n - pos;
			assert(wg_meter_add(m, block, count));
			pos += count;
		}
		assert(wg_meter_finish(m, NULL, &peak));
		assert(wg_meter_finish(m, NULL, &again) && peak == again);
		assert(!wg_meter_add(m, pcm, 0));
		assert(fwrite(&peak, sizeof(peak), 1, stdout) == 1);
		wg_meter_free(m);
		for (ch = 0; ch < channels; ch++) free(pcm[ch]);
	}
}

/* Stress interpolation maxima at different magnitudes and caller boundaries.
 * The runner compares these bytes with the original libebur128 kernels. */
static void peak_patterns(void)
{
	static const unsigned long rates[] = {44100, 48000, 96000, 192000};
	static const size_t chunks[] = {1, 2, 3, 4, 63, 64, 65, 511};
	unsigned int r, channels, pattern;
	for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
	for (channels = 1; channels <= 2; channels++)
	for (pattern = 0; pattern < 4; pattern++) {
		double audio[4098], value;
		size_t i, pos = 0, block = 0;
		uint32_t rng = 17;
		ebur128_state *s = ebur128_init(channels, rates[r], EBUR128_MODE_TRUE_PEAK);
		assert(s);
		for (i = 0; i < 2049 * channels; i++) {
			rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
			value = (double)rng / 2147483648. - 1.;
			if (pattern == 0) value = i & 1 ? -0. : 0.;
			if (pattern == 1) value = ldexp(value, (int)(i % 201) - 100);
			if (pattern == 2) value = i % 127 ? 1e-38 * value : (i & 1 ? -1. : 1.);
			if (pattern == 3) value = nextafterf((float)value, i & 1 ? INFINITY : -INFINITY);
			audio[i] = value;
		}
		while (pos < 2049) {
			size_t n = chunks[block++ % (sizeof(chunks) / sizeof(chunks[0]))];
			unsigned int ch;
			if (n > 2049 - pos) n = 2049 - pos;
			assert(ebur128_add_frames_double(s, audio + pos * channels, n) == EBUR128_SUCCESS);
			for (ch = 0; ch < channels; ch++) {
				assert(ebur128_prev_true_peak(s, ch, &value) == EBUR128_SUCCESS);
				assert(fwrite(&value, sizeof(value), 1, stdout) == 1);
				assert(ebur128_true_peak(s, ch, &value) == EBUR128_SUCCESS);
				assert(fwrite(&value, sizeof(value), 1, stdout) == 1);
			}
			pos += n;
		}
		ebur128_destroy(&s);
	}
}

static void regression(void)
{
	peak_patterns();
	static const unsigned long rates[] = {8000,11025,12000,16000,22050,24000,32000,44100,48000,64000,88200,96000,176400,192000};
	unsigned int r, channels, histogram, chunked;
	for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
	for (channels = 1; channels <= 2; channels++)
	for (histogram = 0; histogram < 2; histogram++)
	for (chunked = 0; chunked < 2; chunked++) {
		size_t n = rates[r] * 2 + 37, i, pos = 0;
		uint32_t rng = 1;
		double *audio = calloc(n * channels, sizeof(double));
		ebur128_state *s = ebur128_init(channels, rates[r], EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK |
		                             (histogram ? EBUR128_MODE_HISTOGRAM : 0));
		assert(audio && s);
		for (i = 0; i < n * channels; i++) {
			rng = rng * UINT32_C(1664525) + UINT32_C(1013904223);
			audio[i] = .33 * sin(i * .17) + ((double)rng / 4294967296. - .5) * .1;
			if (i > n * channels / 3 && i < n * channels * 2 / 3) audio[i] = i & 1 ? 1e-310 : 0.;
		}
		/* An edge impulse exercises the final interpolation history. */
		audio[(n - 1) * channels] = -.99;
		while (pos < n) {
			size_t count = chunked ? (pos % 521 + 1) : 16384;
			if (count > n - pos) count = n - pos;
			assert(ebur128_add_frames_double(s, audio + pos * channels, count) == EBUR128_SUCCESS);
			pos += count;
		}
		emit_state(s);
		ebur128_destroy(&s);
		free(audio);
		if (!histogram && !chunked) adapter_regression(rates[r], channels);
	}
}

static void benchmark(void)
{
	const int frames = 4800, blocks = 600;
	double audio[9600], value;
	int i, mode;
	for (i = 0; i < 9600; i++) audio[i] = .4 * sin(i * .17) + .1 * cos(i * .113);
	for (mode = 0; mode < 2; mode++) {
		ebur128_state *s = ebur128_init(2, 48000, EBUR128_MODE_I | (mode ? EBUR128_MODE_TRUE_PEAK : 0));
		clock_t begin;
		assert(s);
		begin = clock();
		for (i = 0; i < blocks; i++) assert(ebur128_add_frames_double(s, audio, frames) == EBUR128_SUCCESS);
		assert(ebur128_loudness_global(s, &value) == EBUR128_SUCCESS);
		printf("%s %.6f %.17g\n", mode ? "lufs_tp" : "lufs", (double)(clock() - begin) / CLOCKS_PER_SEC, value);
		ebur128_destroy(&s);
	}
	for (mode = 0; mode < 2; mode++) {
		double left[4800], right[4800], *pcm[2] = {left, right};
		WG_METER *m = wg_meter_create(mode ? 192000 : 48000, 2, 0, 1);
		clock_t begin;
		assert(m);
		for (i = 0; i < frames; i++) { left[i] = audio[2 * i]; right[i] = audio[2 * i + 1]; }
		begin = clock();
		for (i = 0; i < blocks * (mode ? 4 : 1); i++) assert(wg_meter_add(m, pcm, frames));
		assert(wg_meter_finish(m, NULL, &value));
		printf("%s %.6f %.17g\n", mode ? "peak_only_192k" : "peak_only", (double)(clock() - begin) / CLOCKS_PER_SEC, value);
		wg_meter_free(m);
	}
}

int main(int argc, char **argv)
{
	(void)argv;
#ifdef WG_TP_HAVE_AVX2
	fprintf(stderr, "Dense true peak: runtime AVX2 %s.\n",
	        __builtin_cpu_supports("avx2") ? "enabled" : "unavailable; using baseline SIMD");
#endif
	if (argc > 1) benchmark(); else regression();
	return 0;
}
