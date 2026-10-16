/* Signal-level checks for analysis decimation and opt-in rate-aware dither. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../resample.h"
#include "../dither.h"
dither_t Dither;
#include "../dither.c"

static double tone_energy(double frequency, int irregular)
{
	WG_DECIMATOR s;
	double data[257], *pcm[2] = {data, NULL}, sum = 0.;
	int i, position = 0, output = 0, frames = 8193;
	wg_decimator_init(&s, 1);
	while (position < frames) {
		int count = irregular ? position % 251 + 1 : 257;
		int n;
		if (count > frames - position) count = frames - position;
		for (i = 0; i < count; i++) data[i] = sin(6.2831853071795864769 * frequency * (position + i));
		n = wg_decimator_process(&s, pcm, count);
		for (i = 0; i < n; i++, output++) if (output > 127) sum += data[i] * data[i];
		position += count;
	}
	memset(data, 0, sizeof(data));
	output += wg_decimator_process(&s, pcm, 63);
	assert(output == (frames + 1) / 2);
	return sum;
}

static double dither_high_frequency(long rate, int shape)
{
	double energy = 0., difference = 0., mean = 0., previous = 0.;
	int i;
	Init_Dither(16, shape);
	Init_Rate_Dither(rate, shape);
	for (i = 0; i < 100000; i++) {
		double value = (double)Rate_Dither(0., 0, 16);
		energy += value * value;
		difference += (value - previous) * (value - previous);
		mean += value;
		previous = value;
	}
	assert(fabs(mean / 100000.) < .02);
	assert(energy > 1000. && energy < 200000.);
	return difference / energy;
}

int main(void)
{
	double pass = tone_energy(.05, 0), stop = tone_energy(.4, 0);
	double low, high, flat;
	assert(pass > 1900. && pass < 2100.);
	assert(stop < pass * 1e-8);
	assert(fabs(tone_energy(.05, 1) / pass - 1.) < .001);
	low = dither_high_frequency(8000, 3);
	high = dither_high_frequency(192000, 3);
	flat = dither_high_frequency(48000, 0);
	assert(high > low + .2);
	assert(fabs(flat - 2.) < .05);
	Init_Rate_Dither(192000, 3);
	assert(Rate_Dither(1e300, 0, 8) == 127);
	assert(Rate_Dither(-1e300, 0, 8) == -128);
	printf("DSP: decimation passband/stopband, edge length, chunking and rate-shaped dither spectra passed (ratios %.3f/%.3f/%.3f)\n", low, high, flat);
	return 0;
}
