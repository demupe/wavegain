/* Longer true-peak reconstruction with conservative partial-sum screening.
 * This remains a finite, zero-extended Hann FIR estimate, not a universal
 * bound on every possible DAC reconstruction or arbitrary near-Nyquist PCM. */
#ifndef WG_PEAK_REFINE_H
#define WG_PEAK_REFINE_H
#include <float.h>
#include "channels.h"

#define WG_REFINE_RADIUS 128
#define WG_REFINE_LENGTH (2 * WG_REFINE_RADIUS + 1)
typedef struct {
	double coefficient[4][WG_REFINE_LENGTH][4];
	double tail[4][WG_REFINE_LENGTH + 1], norm;
	unsigned int index[WG_REFINE_LENGTH];
	double history[WG_MAX_CHANNELS][2 * WG_REFINE_LENGTH], peak;
	unsigned int cursor;
} wg_peak_refiner;

static wg_peak_refiner *wg_peak_refiner_create(void)
{
	wg_peak_refiner *r = calloc(1, sizeof(*r));
	unsigned int phase, j, group, lane;
	if (!r) return NULL;
	for (j = 0; j < WG_REFINE_LENGTH; j++)
		r->index[j] = j & 1 ? WG_REFINE_RADIUS - (j + 1) / 2 : WG_REFINE_RADIUS + j / 2;
	for (phase = 0; phase < 15; phase++) {
		double sum = 0.;
		group = phase / 4; lane = phase % 4;
		for (j = 0; j < WG_REFINE_LENGTH; j++) {
			double d = (double)r->index[j] - WG_REFINE_RADIUS + (phase + 1) / 16.;
			double c = fabs(d) < WG_REFINE_RADIUS ?
				sin(M_PI * d) / (M_PI * d) * (.5 + .5 * cos(M_PI * d / WG_REFINE_RADIUS)) : 0.;
			r->coefficient[group][j][lane] = c; sum += c;
		}
		for (j = 0; j < WG_REFINE_LENGTH; j++) r->coefficient[group][j][lane] /= sum;
	}
	for (group = 0; group < 4; group++) {
		for (j = WG_REFINE_LENGTH; j-- > 0;) {
			double maximum = 0.;
			for (lane = 0; lane < 4; lane++) maximum = fmax(maximum, fabs(r->coefficient[group][j][lane]));
			r->tail[group][j] = nextafter(r->tail[group][j + 1] + maximum, HUGE_VAL);
		}
		r->norm = fmax(r->norm, r->tail[group][0]);
	}
	return r;
}

static void wg_peak_refiner_frame(wg_peak_refiner *r, const double *p, double maximum, double room)
{
	unsigned int group, j, lane;
	for (group = 0; group < 4; group++) {
		double v[4] = {0., 0., 0., 0.};
		if (r->tail[group][0] * maximum + room <= r->peak) continue;
		for (j = 0; j < WG_REFINE_LENGTH; j++) {
			double x = p[-(int)r->index[j]];
			for (lane = 0; lane < 4; lane++) v[lane] += x * r->coefficient[group][j][lane];
#ifndef WG_PEAK_REFINE_FULL
			if ((j & 7) == 7) {
				double top = 0.;
				for (lane = 0; lane < 4; lane++) top = fmax(top, fabs(v[lane]));
				if (top + r->tail[group][j + 1] * maximum + room <= r->peak) break;
			}
#endif
		}
		if (j == WG_REFINE_LENGTH) for (lane = 0; lane < 4; lane++) r->peak = fmax(r->peak, fabs(v[lane]));
	}
}

#ifndef WG_REFINE_AVX2
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#define WG_REFINE_AVX2 1
#else
#define WG_REFINE_AVX2 0
#endif
#endif
#if WG_REFINE_AVX2
#include <immintrin.h>
__attribute__((target("avx2")))
static void wg_peak_refiner_frame_avx2(wg_peak_refiner *r, const double *p, double maximum, double room)
{
	unsigned int group, j;
	for (group = 0; group < 4; group++) {
		__m256d v = _mm256_setzero_pd();
		if (r->tail[group][0] * maximum + room <= r->peak) continue;
		for (j = 0; j < WG_REFINE_LENGTH; j++) {
			__m256d x = _mm256_set1_pd(p[-(int)r->index[j]]);
			v = _mm256_add_pd(v, _mm256_mul_pd(x, _mm256_loadu_pd(r->coefficient[group][j])));
#ifndef WG_PEAK_REFINE_FULL
			if ((j & 7) == 7) {
				__m256d a = _mm256_andnot_pd(_mm256_set1_pd(-0.), v);
				__m128d m = _mm_max_pd(_mm256_castpd256_pd128(a), _mm256_extractf128_pd(a, 1));
				double pair[2]; _mm_storeu_pd(pair, m);
				if (fmax(pair[0], pair[1]) + r->tail[group][j + 1] * maximum + room <= r->peak) break;
			}
#endif
		}
		if (j == WG_REFINE_LENGTH) {
			double values[4]; _mm256_storeu_pd(values, v);
			for (unsigned int lane = 0; lane < 4; lane++) r->peak = fmax(r->peak, fabs(values[lane]));
		}
	}
}
#endif

static void wg_peak_refiner_add(wg_peak_refiner *r, double **pcm, size_t frames,
	unsigned int channels, double known_peak)
{
	size_t start;
	unsigned int ch, j;
#if WG_REFINE_AVX2
	int avx2 = __builtin_cpu_supports("avx2");
#endif
	r->peak = fmax(r->peak, known_peak);
	for (start = 0; start < frames; start += 64) {
		unsigned int count = frames - start > 64 ? 64 : (unsigned int)(frames - start);
		for (ch = 0; ch < channels; ch++) {
			unsigned int cursor = r->cursor, i;
			double maximum = 0., room;
			for (j = 0; j < WG_REFINE_LENGTH; j++) maximum = fmax(maximum, fabs(r->history[ch][j]));
			for (i = 0; i < count; i++) maximum = fmax(maximum, fabs(pcm[ch][start + i]));
			r->peak = fmax(r->peak, maximum);
			room = maximum * r->norm * (8. * WG_REFINE_LENGTH * DBL_EPSILON) + DBL_MIN;
			for (i = 0; i < count; i++) {
				double *p = r->history[ch] + cursor + WG_REFINE_LENGTH;
				p[0] = p[-WG_REFINE_LENGTH] = pcm[ch][start + i];
				if (maximum > 0. && maximum * r->norm + room > r->peak) {
#if WG_REFINE_AVX2
					if (avx2) wg_peak_refiner_frame_avx2(r, p, maximum, room);
					else
#endif
					wg_peak_refiner_frame(r, p, maximum, room);
				}
				if (++cursor == WG_REFINE_LENGTH) cursor = 0;
			}
		}
		r->cursor = (r->cursor + count) % WG_REFINE_LENGTH;
	}
}
#endif
