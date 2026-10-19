/* Portions based on R128Net, Copyright (c) 2026 routersys, MIT licensed.
 * R128Net-inspired C kernels: fused interpolation/peak reduction, contiguous
 * history and independent frames/channels in SIMD lanes. See vendor licenses.
 * Included only after the reference libebur128 internal types are defined. */
#ifndef WG_LOUDNESS_FAST_H
#define WG_LOUDNESS_FAST_H
#include <string.h>
#include <stdint.h>
#include <float.h>
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

#if defined(WG_METER_PAIR) && defined(__x86_64__) && \
    (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5))
#ifndef WG_TP_AVX2
#define WG_TP_AVX2 1
#endif
#if WG_TP_AVX2 && !defined(WG_TP_PAIR_ONLY)
#define WG_TP_HAVE_AVX2 1
#include <immintrin.h>
/* Only dense 4x interpolation enters this function. Keep the caller and all
 * other layouts at the baseline ISA, and retain each frame's tap order. */
__attribute__((target("avx2")))
static double wg_tp_dense_avx2(const double *src, unsigned int frames,
	const interpolator *ip, double top)
{
	typedef double wg_vec4 __attribute__((vector_size(32)));
	wg_vec4 peaks = {top, top, top, top};
	unsigned int i, j;
	for (i = 0; i < frames; i += 4) {
		const double *p = src + i;
		wg_vec4 a = {0., 0., 0., 0.}, b = a, c = a, zero = a, input;
		memcpy(&input, p - ip->filter[0].index[0], sizeof(input));
		zero += input * ip->filter[0].coeff[0];
		for (j = 0; j < 12; j++) {
			memcpy(&input, p - j, sizeof(input));
			a += input * ip->filter[1].coeff[j];
			b += input * ip->filter[2].coeff[j];
			c += input * ip->filter[3].coeff[j];
		}
		peaks = _mm256_max_pd(peaks, _mm256_andnot_pd(_mm256_set1_pd(-0.), zero));
		peaks = _mm256_max_pd(peaks, _mm256_andnot_pd(_mm256_set1_pd(-0.), a));
		peaks = _mm256_max_pd(peaks, _mm256_andnot_pd(_mm256_set1_pd(-0.), b));
		peaks = _mm256_max_pd(peaks, _mm256_andnot_pd(_mm256_set1_pd(-0.), c));
	}
	{
		wg_vec pair = _mm_max_pd(_mm256_castpd256_pd128(peaks), _mm256_extractf128_pd(peaks, 1));
		return fmax(pair[0], pair[1]);
	}
}

/* The CLI's longer 16x filter has a different phase layout. Parallelize
 * independent frames, retaining every phase's original accumulation order. */
__attribute__((target("avx2")))
static double wg_tp_generic_avx2(const double *src, unsigned int frames,
	const interpolator *ip, double top)
{
	typedef double wg_vec4 __attribute__((vector_size(32)));
	wg_vec4 peaks = {top, top, top, top};
	unsigned int i, phase, j;
	for (i = 0; i < frames; i += 4) {
		for (phase = 0; phase < ip->factor; phase++) {
			const interp_filter *f = &ip->filter[phase];
			wg_vec4 value = {0., 0., 0., 0.};
			for (j = 0; j < f->count; j++) {
				wg_vec4 input;
				memcpy(&input, src + i - f->index[j], sizeof(input));
				value += input * f->coeff[j];
			}
			peaks = _mm256_max_pd(peaks, _mm256_andnot_pd(_mm256_set1_pd(-0.), value));
		}
	}
	{
		wg_vec pair = _mm_max_pd(_mm256_castpd256_pd128(peaks), _mm256_extractf128_pd(peaks, 1));
		return fmax(pair[0], pair[1]);
	}
}
#endif
#endif

static void wg_fast_true_peak(ebur128_state *st, const double *src, size_t frames)
{
	interpolator *ip = st->d->interp;
	size_t start;
	unsigned int ch;
#ifdef WG_METER_PAIR
	int dense = 0;
	double norm = 0.;
#endif
	if (!ip) return;
#ifdef WG_METER_PAIR
	/* The 4x Hann interpolator has three dense 12-tap phases and one
	 * single-tap phase. Keep the generic path for every other layout. */
	if (ip->factor == 4 && ip->filter[0].count == 1 &&
	    ip->filter[1].count == 12 && ip->filter[2].count == 12 &&
	    ip->filter[3].count == 12) {
		unsigned int phase, j;
		dense = 1;
		for (phase = 1; phase < 4; phase++)
			for (j = 0; j < 12; j++)
				if (ip->filter[phase].index[j] != j) dense = 0;
		for (phase = 0; phase < 4; phase++) {
			double sum = 0.;
			for (j = 0; j < ip->filter[phase].count; j++) sum += fabs(ip->filter[phase].coeff[j]);
			norm = fmax(norm, sum);
		}
	}
#endif
#ifdef WG_TP_HAVE_AVX2
	int use_avx2 = __builtin_cpu_supports("avx2");
#endif
	for (start = 0; start < frames; start += 64) {
		unsigned int n = frames - start > 64 ? 64 : (unsigned int)(frames - start);
		for (ch = 0; ch < st->channels; ch++) {
			double history[25 + 64], top = st->d->prev_true_peak[ch];
			unsigned int j, i, phase;
			/* Library 2x/4x and CLI 16x interpolators use at most 25 slots. */
			{
				unsigned int at = ip->zi;
				for (j = 0; j < ip->delay; j++) {
					history[j] = ip->z[ch][at];
					if (++at == ip->delay) at = 0;
				}
			}
			for (j = 0; j < n; j++) history[ip->delay + j] = (float)src[(start + j) * st->channels + ch];
			i = 0;
#ifdef WG_METER_PAIR
#ifndef WG_TP_FULL_INTERPOLATION
			if (dense) {
				double magnitude = 0.;
				for (j = 0; j < ip->delay + n; j++) {
					if (!isfinite(history[j])) { magnitude = HUGE_VAL; break; }
					magnitude = fmax(magnitude, fabs(history[j]));
				}
				/* Triangle inequality bounds all phases. The margin covers the
				 * <=12 positive norm additions, multiply and <=12-tap dot product;
				 * DBL_MIN also dominates absolute subnormal rounding errors.
				 * Only skip when even that upper bound cannot raise this call's
				 * peak. History still advances, and the exact fallback is unchanged. */
				if (isfinite(magnitude) &&
				    magnitude * norm * (1. + 128. * DBL_EPSILON) + DBL_MIN <= top) i = n;
			}
#endif
			{
				wg_vec peaks = {top, top};
#ifdef WG_TP_HAVE_AVX2
				if (use_avx2 && i + 4 <= n) {
					unsigned int count = (n - i) & ~3u;
					double peak = dense ? wg_tp_dense_avx2(history + ip->delay + i, count, ip, top) :
						wg_tp_generic_avx2(history + ip->delay + i, count, ip, top);
					peaks = (wg_vec){peak, peak};
					i += count;
				}
#endif
#if !defined(WG_TP_PAIR_ONLY)
				/* Four independent frames retain each phase's tap order. */
				if (dense) for (; i + 4 <= n; i += 4) {
					const double *p = history + ip->delay + i;
					wg_vec a = {0., 0.}, b = {0., 0.}, c = {0., 0.};
					wg_vec d = {0., 0.}, e = {0., 0.}, f = {0., 0.};
					wg_vec zero = {0., 0.}, next_zero = {0., 0.};
					zero += wg_load(p - ip->filter[0].index[0]) * ip->filter[0].coeff[0];
					next_zero += wg_load(p + 2 - ip->filter[0].index[0]) * ip->filter[0].coeff[0];
					for (j = 0; j < 12; j++) {
						wg_vec input = wg_load(p - j), next = wg_load(p + 2 - j);
						a += input * ip->filter[1].coeff[j];
						b += input * ip->filter[2].coeff[j];
						c += input * ip->filter[3].coeff[j];
						d += next * ip->filter[1].coeff[j];
						e += next * ip->filter[2].coeff[j];
						f += next * ip->filter[3].coeff[j];
					}
					peaks = wg_max(peaks, wg_abs(zero));
					peaks = wg_max(peaks, wg_abs(next_zero));
					peaks = wg_max(peaks, wg_abs(a));
					peaks = wg_max(peaks, wg_abs(b));
					peaks = wg_max(peaks, wg_abs(c));
					peaks = wg_max(peaks, wg_abs(d));
					peaks = wg_max(peaks, wg_abs(e));
					peaks = wg_max(peaks, wg_abs(f));
				}
#endif
				for (; i + 2 <= n; i += 2) {
					const double *p = history + ip->delay + i;
					if (dense) {
						wg_vec a = {0., 0.}, b = {0., 0.}, c = {0., 0.};
						wg_vec zero = {0., 0.};
						zero += wg_load(p - ip->filter[0].index[0]) * ip->filter[0].coeff[0];
						for (j = 0; j < 12; j++) {
							wg_vec input = wg_load(p - j);
							a += input * ip->filter[1].coeff[j];
							b += input * ip->filter[2].coeff[j];
							c += input * ip->filter[3].coeff[j];
						}
						peaks = wg_max(peaks, wg_abs(zero));
						peaks = wg_max(peaks, wg_abs(a));
						peaks = wg_max(peaks, wg_abs(b));
						peaks = wg_max(peaks, wg_abs(c));
						continue;
					}
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
			{
				/* Earlier writes in a long block would be overwritten in this
				 * ring. Retain only its final contents, without a divide per sample. */
				unsigned int first = n > ip->delay ? n - ip->delay : 0;
				unsigned int at = (ip->zi + first) % ip->delay;
				for (j = first; j < n; j++) {
					ip->z[ch][at] = (float)history[ip->delay + j];
					if (++at == ip->delay) at = 0;
				}
			}
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
