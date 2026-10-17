/* Differential checks for the branch-free common integer gain path. */
#include <assert.h>
#include <float.h>
#include "../wavegain.c"

int write_to_log;
char log_file_name[512];

int main(void)
{
	const int formats[] = {WAV_FMT_8BIT, WAV_FMT_16BIT, WAV_FMT_24BIT, WAV_FMT_32BIT, WAV_FMT_AIFF};
	const int widths[] = {8, 16, 24, 32, 16};
	const Int64_t minimums[] = {-128, -32768, -8388608, -INT64_C(2147483647), -32768};
	const double scales[] = {0., .01, 1., 4., 1e300};
	double input[2][1031], actual[2][1031], expected[2][1031];
	double *pcm[2] = {actual[0], actual[1]}, offsets[2] = {.001, -.002};
	uint32_t random = UINT32_C(0x91357);
	size_t f, s;
	int ch, i, no_offset, channels;
	for (ch = 0; ch < 2; ch++) for (i = 0; i < 1031; i++) {
		random = random * UINT32_C(1664525) + UINT32_C(1013904223);
		input[ch][i] = ((double)random - 2147483648.) * 0x1p-31;
	}
	for (f = 0; f < sizeof(formats) / sizeof(formats[0]); f++) {
		const int bits = widths[f];
		/* Half-LSB boundaries, signed zeros, tiny values and extreme PCM. */
		double edges[] = {0., -0., 0x1p-1074, -0x1p-1074, 1., -1., DBL_MAX, -DBL_MAX,
			ldexp(.5, 1 - bits), -ldexp(.5, 1 - bits),
			nextafter(ldexp(.5, 1 - bits), 0.), nextafter(ldexp(.5, 1 - bits), 1.)};
		memcpy(input[0], edges, sizeof(edges));
		for (channels = 1; channels <= 2; channels++) for (no_offset = 0; no_offset <= 1; no_offset++)
		for (s = 0; s < sizeof(scales) / sizeof(scales[0]); s++) {
			Init_Dither(bits, 0);
			memcpy(actual, input, sizeof(input));
			memcpy(expected, input, sizeof(input));
			for (ch = 0; ch < channels; ch++) for (i = 0; i < 1031; i++) {
				Int64_t value, minimum = minimums[f];
				Int64_t maximum = (INT64_C(1) << (bits - 1)) - 1;
				if (!no_offset) expected[ch][i] -= offsets[ch];
				expected[ch][i] *= scales[s];
				value = dither_output(0, 0, i % 32, expected[ch][i] * 2147483647.f, ch, formats[f]);
				if (value > maximum) value = maximum;
				else if (value < minimum) value = minimum;
				expected[ch][i] = (double)value;
			}
			assert(apply_plain_gain(pcm, channels, 1031, no_offset ? NULL : offsets, scales[s], bits));
			assert(!memcmp(actual, expected, sizeof(actual)));
		}
	}
	actual[1][1030] = NAN;
	assert(!apply_plain_gain(pcm, 2, 1031, NULL, 1., 16));
	actual[1][1030] = HUGE_VAL;
	assert(!apply_plain_gain(pcm, 2, 1031, NULL, 1., 16));
	puts("Plain gain: all integer widths, DC, saturation, half-LSB boundaries, extreme/subnormal input and finite guards match legacy quantization");
	return 0;
}
