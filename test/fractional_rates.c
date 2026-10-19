/* Arbitrary-rate history, reference peak buffers and reset boundaries. */
#include <assert.h>
#include <stdio.h>
#include "../src/loudness.c"

static void check_block(ebur128_state *st, int type, size_t chunk)
{
	size_t frames = ebur128_block_frames(st), samples = frames * st->channels;
	unsigned long previous_blocks = st->d->block_list_size;
	short *s = calloc(samples, sizeof(*s));
	int *n = calloc(samples, sizeof(*n));
	float *f = calloc(samples, sizeof(*f));
	double *d = calloc(samples, sizeof(*d));
	ebur128_state *oracle = ebur128_init(st->channels, st->samplerate, EBUR128_MODE_M);
	double expected = 0., actual;
	assert(s && n && f && d && oracle);
	memcpy(oracle->d->v, st->d->v, st->channels * sizeof(*st->d->v));
	assert(st->d->audio_data_frames >= frames);
	assert(st->d->resampler_buffer_input_frames >= frames);
	assert(st->d->resampler_buffer_output_frames >= frames * st->d->interp->factor);
	for (size_t i = 0; i < frames; i++) for (unsigned int ch = 0; ch < st->channels; ch++) {
		size_t j = i * st->channels + ch;
		double value = i == frames - 1 ? .5 : .01;
		s[j] = (short)(value * 32768); n[j] = (int)(value * 2147483648.);
		f[j] = (float)value; d[j] = value;
		if (type == 0) value = s[j] / 32768.;
		if (type == 1) value = n[j] / 2147483648.;
		if (type == 2) value = f[j];
		value = ebur128_k_sample(oracle, ch, value);
		expected += value * value;
	}
	for (size_t i = 0; i < frames;) {
		size_t count = frames - i < chunk ? frames - i : chunk;
		size_t j = i * st->channels;
		int result;
		switch (type) {
		case 0: result = ebur128_add_frames_short(st, s + j, count); break;
		case 1: result = ebur128_add_frames_int(st, n + j, count); break;
		case 2: result = ebur128_add_frames_float(st, f + j, count); break;
		default: result = ebur128_add_frames_double(st, d + j, count); break;
		}
		assert(result == EBUR128_SUCCESS); i += count;
	}
	assert(st->d->block_list_size == previous_blocks + 1);
	assert(ebur128_calc_gating_block(st, frames, &actual) == EBUR128_SUCCESS);
	assert(fabs(actual - expected / frames) < expected / frames * 1e-11);
	{
		struct ebur128_dq_entry *last = STAILQ_FIRST(&st->d->block_list);
		while (STAILQ_NEXT(last, entries)) last = STAILQ_NEXT(last, entries);
		assert(fabs(last->z - actual) < actual * 1e-11);
	}
	free(s); free(n); free(f); free(d); ebur128_destroy(&oracle);
}

int main(void)
{
	const unsigned long rates[] = {11025, 11026, 22050, 22052, 22053, 22054, 44101, 44104};
	const size_t chunks[] = {1, 257, 20000};
	for (size_t r = 0; r < sizeof(rates) / sizeof(*rates); r++)
	for (unsigned int channels = 1; channels <= 2; channels++)
	for (int long_window = 0; long_window < 2; long_window++)
	for (int type = 0; type < 4; type++)
	for (size_t c = 0; c < sizeof(chunks) / sizeof(*chunks); c++) {
		int mode = EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK | (long_window ? EBUR128_MODE_S : 0);
		ebur128_state *st = ebur128_init(channels, rates[r], mode);
		assert(st); check_block(st, type, chunks[c]); ebur128_destroy(&st);
	}
	/* Parameter and window changes must use the same units and capacities. */
	for (int type = 0; type < 4; type++) {
		ebur128_state *st = ebur128_init(1, 22050, EBUR128_MODE_I | EBUR128_MODE_TRUE_PEAK);
		assert(st && ebur128_change_parameters(st, 2, 22052) == EBUR128_SUCCESS);
		check_block(st, type, 20000);
		assert(ebur128_set_max_window(st, 1000) == EBUR128_SUCCESS);
		assert(st->d->audio_data_frames >= 22052 && st->d->audio_data_frames < 25000);
		assert(ebur128_set_max_window(st, 400) == EBUR128_SUCCESS);
		check_block(st, type, 257);
		assert(ebur128_change_parameters(st, 1, 22053) == EBUR128_SUCCESS);
		check_block(st, type, 257); ebur128_destroy(&st);
	}
	/* The adapter replaces the vendor interpolator and its staging buffers. */
	for (int mode = WG_PEAK_PRECISE; mode <= WG_PEAK_FAST; mode++) {
		WG_METER *m = wg_meter_create_mode(22054, 1, 1, 1, (WG_PEAK_MODE)mode);
		assert(m); check_block(m->state, 3, 20000); wg_meter_free(m);
	}
	puts("Fractional rates: all input types, history energy, peak buffers and parameter/window resets passed");
	return 0;
}
