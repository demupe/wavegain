/* Portions based on R128Net, Copyright (c) 2026 routersys, MIT licensed.
 * R128Net-inspired C kernels: fused interpolation/peak reduction, contiguous
 * history and independent frames/channels in SIMD lanes. See vendor licenses.
 * Included only after the reference libebur128 internal types are defined. */
#ifndef WG_LOUDNESS_FAST_H
#define WG_LOUDNESS_FAST_H
#include <string.h>
#include <stdint.h>
#if (defined(__clang__) || defined(__GNUC__)) && (defined(__aarch64__) || defined(__x86_64__))
#define WG_METER_PAIR 1
typedef double wg_vec __attribute__((vector_size(16)));
#ifdef __aarch64__
#include <arm_neon.h>
static wg_vec wg_max(wg_vec a, wg_vec b) { return vmaxq_f64(a, b); }
static wg_vec wg_abs(wg_vec a) { return vabsq_f64(a); }
#else
#include <emmintrin.h>
static wg_vec wg_max(wg_vec a, wg_vec b) { return _mm_max_pd(a, b); }
static wg_vec wg_abs(wg_vec a) { return _mm_andnot_pd(_mm_set1_pd(-0.), a); }
#endif
static wg_vec wg_load(const double *p) { wg_vec v; memcpy(&v, p, sizeof(v)); return v; }
#ifndef WG_EBUR_EXTRA_REFERENCE
/* Keep each channel's reference addition order, including ring wrap.
 * A rolling sum would change rounding and could move a block across a gate. */
static void wg_energy_stereo(ebur128_state *st, size_t frames, double sums[2])
{
	size_t end = st->d->audio_data_index / 2, start, i;
	wg_vec sum = {0., 0.};
	if (end < frames) {
		for (i = 0; i < end; i++) {
			wg_vec v = wg_load(st->d->audio_data + 2 * i);
			sum += v * v;
		}
		start = st->d->audio_data_frames - (frames - end);
		end = st->d->audio_data_frames;
	} else start = end - frames;
	for (i = start; i < end; i++) {
		wg_vec v = wg_load(st->d->audio_data + 2 * i);
		sum += v * v;
	}
	memcpy(sums, &sum, sizeof(sum));
}
#endif
#endif

static void wg_fast_true_peak(ebur128_state *st, const double *src, size_t frames)
{
	interpolator *ip = st->d->interp;
	size_t start;
	unsigned int ch;
	if (!ip) return;
	for (start = 0; start < frames; start += 64) {
		unsigned int n = frames - start > 64 ? 64 : (unsigned int)(frames - start);
		for (ch = 0; ch < st->channels; ch++) {
			double history[25 + 64], top = st->d->prev_true_peak[ch];
			unsigned int j, i, phase;
			/* The 49-tap 2x/4x interpolators have at most 25 history slots. */
			for (j = 0; j < ip->delay; j++) history[j] = ip->z[ch][(ip->zi + j) % ip->delay];
			for (j = 0; j < n; j++) history[ip->delay + j] = (float)src[(start + j) * st->channels + ch];
			i = 0;
#ifdef WG_METER_PAIR
			{
				wg_vec peaks = {top, top};
				for (; i + 2 <= n; i += 2) {
					const double *p = history + ip->delay + i;
					for (phase = 0; phase < ip->factor; phase++) {
						interp_filter *f = &ip->filter[phase];
						wg_vec value = {0., 0.};
						for (j = 0; j < f->count; j++) value += wg_load(p - f->index[j]) * f->coeff[j];
						peaks = wg_max(peaks, wg_abs(value));
					}
				}
				top = fmax(peaks[0], peaks[1]);
			}
#endif
			for (; i < n; i++) for (phase = 0; phase < ip->factor; phase++) {
				interp_filter *f = &ip->filter[phase];
				double value = 0.;
				for (j = 0; j < f->count; j++) value += history[ip->delay + i - f->index[j]] * f->coeff[j];
				top = fmax(top, fabs(value));
			}
			/* Narrow once: rounding to float is monotonic, so the maximum agrees
			 * with narrowing each individual output before scanning it. */
			st->d->prev_true_peak[ch] = (float)top;
			for (j = 0; j < n; j++) ip->z[ch][(ip->zi + j) % ip->delay] = (float)history[ip->delay + j];
		}
		ip->zi = (ip->zi + n) % ip->delay;
	}
}

static void wg_ebur_filter_double(ebur128_state *st, const double *src, size_t frames)
{
	size_t i, c;
	double *out = st->d->audio_data + st->d->audio_data_index;
	TURN_ON_FTZ
	if ((st->mode & EBUR128_MODE_SAMPLE_PEAK) == EBUR128_MODE_SAMPLE_PEAK) {
		for (c = 0; c < st->channels; c++) {
			double peak = st->d->prev_sample_peak[c];
			for (i = 0; i < frames; i++) peak = fmax(peak, fabs(src[i * st->channels + c]));
			st->d->prev_sample_peak[c] = peak;
		}
	}
	if ((st->mode & EBUR128_MODE_TRUE_PEAK) == EBUR128_MODE_TRUE_PEAK) wg_fast_true_peak(st, src, frames);
#ifdef WG_METER_PAIR
	if (st->channels == 2 && st->d->channel_map[0] != EBUR128_UNUSED && st->d->channel_map[1] != EBUR128_UNUSED) {
		wg_vec s1 = {st->d->v[0][1], st->d->v[1][1]}, s2 = {st->d->v[0][2], st->d->v[1][2]};
		wg_vec s3 = {st->d->v[0][3], st->d->v[1][3]}, s4 = {st->d->v[0][4], st->d->v[1][4]}, v = {0., 0.};
		for (i = 0; i < frames; i++) {
			wg_vec result;
			v = wg_load(src + 2 * i) - st->d->a[1] * s1 - st->d->a[2] * s2 - st->d->a[3] * s3 - st->d->a[4] * s4;
			result = st->d->b[0] * v + st->d->b[1] * s1 + st->d->b[2] * s2 + st->d->b[3] * s3 + st->d->b[4] * s4;
			memcpy(out + 2 * i, &result, sizeof(result));
			s4 = s3; s3 = s2; s2 = s1; s1 = v;
		}
		for (c = 0; c < 2; c++) {
			st->d->v[c][0] = v[c]; st->d->v[c][1] = s1[c]; st->d->v[c][2] = s2[c];
			st->d->v[c][3] = s3[c]; st->d->v[c][4] = s4[c];
			FLUSH_MANUALLY
		}
	} else
#endif
	for (c = 0; c < st->channels; c++) {
		double *v = st->d->v[c];
		if (st->d->channel_map[c] == EBUR128_UNUSED) continue;
		for (i = 0; i < frames; i++) {
			v[0] = src[i * st->channels + c] - st->d->a[1] * v[1] - st->d->a[2] * v[2] - st->d->a[3] * v[3] - st->d->a[4] * v[4];
			out[i * st->channels + c] = st->d->b[0] * v[0] + st->d->b[1] * v[1] + st->d->b[2] * v[2] + st->d->b[3] * v[3] + st->d->b[4] * v[4];
			v[4] = v[3]; v[3] = v[2]; v[2] = v[1]; v[1] = v[0];
		}
		FLUSH_MANUALLY
	}
	TURN_OFF_FTZ
}
#define ebur128_filter_double wg_ebur_filter_double
#endif
