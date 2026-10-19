/* LUFS/true-peak adapter. The bundled MIT/BSD implementation is identified in
 * vendor/libebur128/README. Keeping it here preserves the single-command build. */
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <string.h>
#include <stdint.h>
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("fp-contract=off")
#elif defined(_MSC_VER)
#pragma float_control(precise, on)
#pragma fp_contract(off)
#endif
#include "../vendor/libebur128/ebur128.c"
#include "loudness.h"
#include "peak_refine.h"


struct wg_meter {
	ebur128_state *state;
	unsigned int channels;
	double *packed;
	size_t capacity;
	int finished, has_loudness, has_peak;
	wg_peak_refiner *refiner;
	double refined_peak;
};

/* Keep the library reference kernels intact. Precise uses 385 taps/16x;
 * fast uses 49 taps/4x at EVERY rate, including 192 kHz. Both are finite FIR
 * estimates and fit the kernels' maximum of 25 input history slots. */
static int wg_meter_init_peak(ebur128_state *st, WG_PEAK_MODE peak_mode)
{
	unsigned int factor = peak_mode == WG_PEAK_FAST ? 4 : 16;
	interpolator *ip = interp_create(peak_mode == WG_PEAK_FAST ? 49 : 385, factor, st->channels);
	size_t input_frames = st->d->samples_in_100ms * 4;
	size_t output_frames;
	float *input, *output;
	if (!ip) return 0;
	if (input_frames > SIZE_MAX / factor / st->channels / sizeof(float)) {
		interp_destroy(ip); return 0;
	}
	output_frames = input_frames * factor;
	input = malloc(input_frames * st->channels * sizeof(float));
	output = malloc(output_frames * st->channels * sizeof(float));
	if (!input || !output) {
		free(input); free(output); interp_destroy(ip); return 0;
	}
	ebur128_destroy_resampler(st);
	st->d->interp = ip;
	st->d->resampler_buffer_input = input;
	st->d->resampler_buffer_input_frames = input_frames;
	st->d->resampler_buffer_output = output;
	st->d->resampler_buffer_output_frames = output_frames;
	return 1;
}

WG_METER *wg_meter_create(unsigned long rate, unsigned int channels, int loudness, int peak)
{
	return wg_meter_create_mode(rate, channels, loudness, peak, WG_PEAK_PRECISE);
}

WG_METER *wg_meter_create_mode(unsigned long rate, unsigned int channels, int loudness, int peak,
	WG_PEAK_MODE peak_mode)
{
	WG_METER *m = calloc(1, sizeof(*m));
	int mode = EBUR128_MODE_M;
	if (!m || channels < 1 || channels > 2 ||
	    (peak_mode != WG_PEAK_PRECISE && peak_mode != WG_PEAK_FAST &&
	     peak_mode != WG_PEAK_EXTENDED)) { free(m); return NULL; }
	if (loudness) mode |= EBUR128_MODE_I;
	if (peak) mode |= EBUR128_MODE_TRUE_PEAK;
	m->channels = channels;
	m->has_loudness = loudness;
	m->has_peak = peak;
	m->state = ebur128_init(channels, rate, mode);
	if (!m->state) { free(m); return NULL; }
	if (peak && !wg_meter_init_peak(m->state, peak_mode)) {
		ebur128_destroy(&m->state); free(m); return NULL;
	}
	if (peak && peak_mode == WG_PEAK_EXTENDED && !(m->refiner = wg_peak_refiner_create())) {
		ebur128_destroy(&m->state); free(m); return NULL;
	}
	return m;
}

int wg_meter_add(WG_METER *m, double **pcm, size_t frames)
{
	size_t i;
	unsigned int ch;
	if (!m || m->finished || frames > (size_t)-1 / m->channels / sizeof(double)) return 0;
	if (frames > m->capacity) {
		double *p = realloc(m->packed, frames * m->channels * sizeof(double));
		if (!p) return 0;
		m->packed = p;
		m->capacity = frames;
	}
	if (m->channels == 2) {
		const double *left = pcm[0], *right = pcm[1];
		for (i = 0; i < frames; i++) {
			if (!isfinite(left[i]) || !isfinite(right[i])) return 0;
			m->packed[2 * i] = left[i];
			m->packed[2 * i + 1] = right[i];
		}
	} else {
		for (i = 0; i < frames; i++) {
			if (!isfinite(pcm[0][i])) return 0;
			m->packed[i] = pcm[0][i];
		}
	}
	if (m->refiner) wg_peak_refiner_add(m->refiner, pcm, frames, m->channels, m->refiner->peak);
#if !defined(WG_EBUR_REFERENCE) && !defined(WG_EBUR_EXTRA_REFERENCE)
	/* Peak-only measurement needs neither K-weighting nor a loudness ring.
	 * Use the same peak kernels, leaving integrated measurement unchanged. */
	if (m->has_peak && !m->has_loudness) {
		TURN_ON_FTZ
		wg_fast_true_peak(m->state, m->packed, frames);
		for (ch = 0; ch < m->channels; ch++) {
			double peak = m->state->d->sample_peak[ch];
			for (i = 0; i < frames; i++) peak = fmax(peak, fabs(m->packed[i * m->channels + ch]));
			m->state->d->sample_peak[ch] = peak;
			m->state->d->true_peak[ch] = fmax(m->state->d->true_peak[ch], m->state->d->prev_true_peak[ch]);
		}
		TURN_OFF_FTZ
		return 1;
	}
#endif
	return ebur128_add_frames_double(m->state, m->packed, frames) == EBUR128_SUCCESS;
}

int wg_meter_finish(WG_METER *m, double *lufs, double *peak)
{
	unsigned int ch;
	int release_buffers;
	if (!m) return 0;
	release_buffers = !m->finished;
	if (!m->finished && m->refiner) {
		double zero[64] = {0.}, *pcm[2] = {zero, zero};
		unsigned int i;
		for (i = 0; i < WG_REFINE_LENGTH; i += 64)
			wg_peak_refiner_add(m->refiner, pcm,
				WG_REFINE_LENGTH - i < 64 ? WG_REFINE_LENGTH - i : 64, m->channels, m->refiner->peak);
		m->refined_peak = m->refiner->peak;
	}
	/* Drain only interpolation, without adding silence to the loudness blocks. */
	if (!m->finished && m->has_peak && m->state->d->interp) {
#ifdef WG_EBUR_REFERENCE
		memset(m->state->d->resampler_buffer_input, 0, 64 * m->channels * sizeof(float));
		ebur128_check_true_peak(m->state, 64);
#else
		double zeros[128] = {0};
		wg_fast_true_peak(m->state, zeros, 64);
#endif
		for (ch = 0; ch < m->channels; ch++)
			m->state->d->true_peak[ch] = fmax(m->state->d->true_peak[ch], m->state->d->prev_true_peak[ch]);
	}
	m->finished = 1;
	if (lufs) {
		*lufs = -HUGE_VAL;
		if (m->has_loudness && ebur128_loudness_global(m->state, lufs) != EBUR128_SUCCESS) return 0;
	}
	if (peak) {
		*peak = m->refined_peak;
		for (ch = 0; m->has_peak && ch < m->channels; ch++) {
			double value;
			if (ebur128_true_peak(m->state, ch, &value) != EBUR128_SUCCESS || !isfinite(value)) return 0;
			*peak = fmax(*peak, value);
		}
	}
	/* Album integration only needs gating history. Do not retain large audio
	 * and interpolation buffers for every already-measured file in an album. */
	if (release_buffers) {
		free(m->refiner); m->refiner = NULL;
		free(m->packed); m->packed = NULL; m->capacity = 0;
		free(m->state->d->audio_data); m->state->d->audio_data = NULL;
		free(m->state->d->v); m->state->d->v = NULL;
		ebur128_destroy_resampler(m->state);
	}
	return 1;
}

int wg_meter_album(WG_METER **meters, size_t count, double *lufs)
{
	ebur128_state **states;
	size_t i;
	int ok;
	if (!count || count > (size_t)-1 / sizeof(*states)) return 0;
	states = malloc(count * sizeof(*states));
	if (!states) return 0;
	for (i = 0; i < count; i++) {
		if (!meters[i] || !meters[i]->has_loudness) { free(states); return 0; }
		states[i] = meters[i]->state;
	}
	ok = ebur128_loudness_global_multiple(states, count, lufs) == EBUR128_SUCCESS;
	free(states);
	return ok;
}

void wg_meter_free(WG_METER *m)
{
	if (!m) return;
	ebur128_destroy(&m->state);
	free(m->packed);
	free(m->refiner);
	free(m);
}
